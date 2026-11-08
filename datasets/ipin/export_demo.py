#!/usr/bin/env python3
"""Copies one IPIN walk per building into data/real_ipin/ for the web demo.

The pick is mechanical, not chosen for good results: per site, the shortest validation walk that
has a barometer and visits every floor. (Scoring walks run 25-35 minutes, 6-24 MB each.) For each
site it writes the venue (data/ipin/<site>.json), its GeoJSON export for the 3D view, one plan image per
floor (the competition's map, which covers exactly the venue's bounds), the trace, and
data/real_ipin/demos.json, which the demo reads to fill its scenario menu.

    .venv/bin/python datasets/ipin/convert.py --wifi --scans --out datasets/ipin/out_scans --data data/ipin
    .venv/bin/python datasets/ipin/export_demo.py
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
VIEW_PAD_M = 8.0
MAX_IMAGE_PX = 2400

Image.MAX_IMAGE_PIXELS = None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-dir", type=Path, default=HERE / "out_scans", help="convert.py output")
    ap.add_argument("--raw", type=Path, default=HERE / "raw")
    ap.add_argument("--dest", type=Path, default=ROOT / "data" / "real_ipin")
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
        subprocess.run([str(args.build / "tools" / "ipe-import"), "export", str(site_dir / "venue.json"),
                        "--out", str(site_dir / "venue.geojson")], check=True, capture_output=True)
        images = {}
        for o in ordinals:
            img = Image.open(args.raw / cfg["packages"][0] / cfg["maps"][str(o)]).convert("L")
            if img.width > MAX_IMAGE_PX:
                img = img.resize((MAX_IMAGE_PX, round(img.height * MAX_IMAGE_PX / img.width)), Image.LANCZOS)
            name = f"floor_{o}.png"
            img.save(site_dir / name, optimize=True)
            images[str(o)] = f"{site}/{name}"
        src = args.out_dir / t["trace"]
        shutil.copy(src, site_dir / src.name)

        venue = json.loads((site_dir / "venue.json").read_text())
        bounds = venue["bounds"]
        pts = [tuple(map(float, ln.split(",")[2:4])) for ln in src.read_text().splitlines() if ln.startswith("gt,")]
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        view = [max(bounds[0], min(xs) - VIEW_PAD_M), max(bounds[1], min(ys) - VIEW_PAD_M),
                min(bounds[2], max(xs) + VIEW_PAD_M), min(bounds[3], max(ys) + VIEW_PAD_M)]
        title = cfg["description"].split(" (")[0]
        demos.append({
            "id": f"ipin_{site}",
            "title": f"{title}, {t['duration_s'] / 60:.0f} min walk, {len(ordinals)} floors",
            "venue": f"{site}/venue.json",
            "geojson": f"{site}/venue.geojson",
            "images": images,
            "trace": f"{site}/{src.name}",
            "config": "../configs/msil_wifi.json",
            "view": [round(v, 1) for v in view],
            "duration_s": round(t["duration_s"], 1),
            "waypoints": len(pts),
            "description": (
                f"{title}: an IPIN competition validation walk ({t['phone']}, IMU + Wi-Fi + barometer) over floors "
                f"{', '.join(map(str, ordinals))}, taking the stairs. Squares are the landmarks the surveyor tapped, "
                "the only ground truth; the floor menu follows the walker. Walls were extracted from the "
                "competition's floor plans and the Wi-Fi access points (purple) located from the training walks. "
                "This walk is from the set the settings were chosen on; the held-out scoring walks are in "
                "docs/ipin.md. The particle filter uses data/configs/msil_wifi.json."),
        })
        print(f"{site}: {src.name} ({t['duration_s']:.0f} s, {len(pts)} landmarks, {t['phone']})")

    index = {
        "menu": "Real recordings (IPIN competition, several floors)",
        "source": "IPIN competition Track 3 datasets, CC BY 4.0 (see LICENSE)",
        "selection": "per site, the shortest validation walk with a barometer that visits every floor",
        "default": demos[0]["id"],
        "demos": demos,
    }
    (args.dest / "demos.json").write_text(json.dumps(index, indent=1) + "\n")
    print(f"wrote {args.dest / 'demos.json'}")


if __name__ == "__main__":
    main()
