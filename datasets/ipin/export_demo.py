#!/usr/bin/env python3
"""Copies one IPIN walk per building into data/demo/ipin/ for the web demo.

The pick is mechanical, not chosen for good results: per site, the shortest validation walk that
has a barometer and visits every floor. (Scoring walks run 25-35 minutes, 6-24 MB each.) For each
site it writes the venue (data/ipin/<site>.json), its GeoJSON export for the 3D view, one plan image per
floor (the competition's map, which covers exactly the venue's bounds), the trace, and
data/demo/ipin/demos.json, which the demo reads to fill its scenario menu.

    .venv/bin/python datasets/ipin/convert.py --wifi --scans --out datasets/ipin/out_scans --data data/ipin
    .venv/bin/python datasets/ipin/export_demo.py
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent))
from common.demo import export_geojson, view_box, write_index  # noqa: E402

VIEW_PAD_M = 8.0
MAX_IMAGE_PX = 2400
# Seconds cut from the start of each demo walk. The surveyor taps the first landmark about 61 s in, so
# before that there is no walker to show, and on the 3D overview it only becomes visible once it moves:
# from ~82 s in UMinho, and from the second landmark (90.6 s, after the stairs) in Nuremberg, whose first
# landmark is on the floor -1 landing.
DEMO_START_S = {"nuremberg": 90.0, "uminho": 82.0}

Image.MAX_IMAGE_PIXELS = None


def trim_trace(lines: list[str], start: float) -> list[str]:
    """Drops every row before `start` and shifts the rest to start at 0. If `start` falls between two
    landmarks on the same floor, a truth row interpolated between them is added at 0, as the demo draws
    the walker there anyway (and scoring skips the first 5 s)."""
    header = [ln for ln in lines if ln.startswith("#")]
    rows = [ln.split(",") for ln in lines if ln and not ln.startswith("#")]
    gt = [r for r in rows if r[0] == "gt"]
    out = []
    before = [r for r in gt if float(r[1]) < start]
    after = [r for r in gt if float(r[1]) >= start]
    if before and after and len(before[-1]) > 5 and before[-1][5] == after[0][5]:
        a, b = before[-1], after[0]
        u = (start - float(a[1])) / (float(b[1]) - float(a[1]))
        x = float(a[2]) + u * (float(b[2]) - float(a[2]))
        y = float(a[3]) + u * (float(b[3]) - float(a[3]))
        out.append(f"gt,0.000,{x:.3f},{y:.3f},{a[4]},{a[5]},{a[6]}")
    for r in rows:
        t = float(r[1])
        if t >= start:
            out.append(",".join([r[0], f"{t - start:.3f}"] + r[2:]))
    return header + out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-dir", type=Path, default=HERE / "out_scans", help="convert.py output")
    ap.add_argument("--raw", type=Path, default=HERE / "raw")
    ap.add_argument("--dest", type=Path, default=ROOT / "data" / "demo" / "ipin")
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    args = ap.parse_args()

    sites = json.loads((HERE / "sites.json").read_text())
    tune = json.loads((args.out_dir / "manifest_tune.json").read_text())["traces"]
    if args.dest.exists():
        shutil.rmtree(args.dest)
    args.dest.mkdir(parents=True)
    shutil.copy(ROOT / "data" / "ipin" / "LICENSE", args.dest / "LICENSE")

    demos = []
    for site, cfg in sites.items():
        ordinals = sorted(int(o) for o in cfg["maps"])
        candidates = [t for t in tune if t["group"] == f"{site}_tune" and t["barometer"] and t["floors"] == ordinals]
        if not candidates:
            print(f"{site}: no validation walk with a barometer visits every floor; skipped")
            continue
        t = min(candidates, key=lambda e: e["duration_s"])
        site_dir = args.dest / site
        site_dir.mkdir()
        shutil.copy(ROOT / "data" / "ipin" / f"{site}.json", site_dir / "venue.json")
        export_geojson(args.build, site_dir / "venue.json", site_dir / "venue.geojson")
        images = {}
        for o in ordinals:
            img = Image.open(args.raw / cfg["packages"][0] / cfg["maps"][str(o)]).convert("L")
            if img.width > MAX_IMAGE_PX:
                img = img.resize((MAX_IMAGE_PX, round(img.height * MAX_IMAGE_PX / img.width)), Image.LANCZOS)
            name = f"floor_{o}.png"
            img.save(site_dir / name, optimize=True)
            images[str(o)] = f"{site}/{name}"
        src = args.out_dir / t["trace"]
        start = DEMO_START_S.get(site, 0.0)
        trimmed = trim_trace(src.read_text().splitlines(), start)
        (site_dir / src.name).write_text("\n".join(trimmed) + "\n")
        duration = t["duration_s"] - start
        interpolated = sum(1 for ln in trimmed if ln.startswith("gt,0.000,")) and not any(
            ln.startswith("gt,") and abs(float(ln.split(",")[1]) - start) < 5e-4
            for ln in src.read_text().splitlines())

        venue = json.loads((site_dir / "venue.json").read_text())
        pts = [tuple(map(float, ln.split(",")[2:4])) for ln in trimmed if ln.startswith("gt,")]
        title = cfg["description"].split(" (")[0]
        demos.append({
            "id": f"ipin_{site}",
            "title": f"{title}, {duration / 60:.0f} min walk, {len(ordinals)} floors",
            "venue": f"{site}/venue.json",
            "geojson": f"{site}/venue.geojson",
            "images": images,
            "trace": f"{site}/{src.name}",
            "config": "../../configs/ipin.json",
            "view": view_box(venue["bounds"], pts, VIEW_PAD_M),
            "duration_s": round(duration, 1),
            "waypoints": len(pts) - (1 if interpolated else 0),
            "description": (
                f"{title}: an IPIN competition validation walk ({t['phone']}, IMU + Wi-Fi + barometer) over floors "
                f"{', '.join(map(str, ordinals))}, taking the stairs. The first {start:.0f} s of the recording are cut, "
                "most of it before the surveyor's first landmark"
                + (", and the square at 0 s is interpolated between the two landmarks around the cut. " if interpolated
                   else ". ")
                + "Squares are the landmarks the surveyor tapped, "
                "the only ground truth; the floor menu follows the walker. Walls were extracted from the "
                "competition's floor plans and the Wi-Fi access points (purple) located from the training walks. "
                "This walk is from the set the settings were chosen on; the held-out scoring walks are in "
                "docs/datasets.md. The engine uses data/configs/ipin.json: the Wi-Fi settings plus the Wi-Fi "
                "fingerprint floor prior."),
        })
        print(f"{site}: {src.name} ({duration:.0f} s from {start:.0f} s, {len(pts)} landmarks, {t['phone']})")

    index = {
        "menu": "Real recordings (IPIN competition, several floors)",
        "source": "IPIN competition Track 3 datasets, CC BY 4.0 (see LICENSE)",
        "selection": ("per site, the shortest validation walk with a barometer that visits every floor, without its "
                      "first seconds (DEMO_START_S)"),
        "default": demos[0]["id"],
        "demos": demos,
    }
    write_index(args.dest, index)


if __name__ == "__main__":
    main()
