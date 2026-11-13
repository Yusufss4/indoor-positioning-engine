#!/usr/bin/env python3
"""Copies one held-out walk per floor of the converted Kaggle set into data/demo/kaggle/ for the web demo.

The pick is mechanical, not chosen for good results: on every floor, the longest held-out walk that has a
Wi-Fi scan, where it lasts at least --min-duration seconds (five held-out walks have none; one floor has no
other). For each floor it writes the venue as it was scored (fitted access
points, fingerprint-only access points and the training scans with their positions), its GeoJSON export
for the 3D view, the floor image and the trace, plus data/demo/kaggle/demos.json, which the demo reads to
fill its scenario menu, one group per building.

The competition's rules do not allow its data to be redistributed, so data/demo/kaggle/ is git-ignored:
it exists on the machine that downloaded the data, and the deployed demo does not have it.

    .venv/bin/python datasets/msil/convert.py --kaggle datasets/kaggle/raw --test-sites --wifi --scans \
        --fingerprints --jobs 8 --out datasets/kaggle/out_fp
    .venv/bin/python datasets/kaggle/export_demo.py
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent / "msil"))
from convert import georef, load_floor  # noqa: E402  (puts datasets/ on the path)

from common.demo import export_geojson, longest_per_group, view_box, waypoints, write_index  # noqa: E402

VIEW_PAD_M = 25.0


def floor_key(name: str) -> tuple[int, str]:
    """Basements first, then upwards: B2, B1, F1 / 1F, F2, ..."""
    digits = "".join(c for c in name if c.isdigit())
    n = int(digits) if digits else 0
    return (-n if name.upper().startswith("B") else n, name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", type=Path, default=HERE / "out_fp",
                    help="converter output folder (convert.py --kaggle ... --fingerprints)")
    ap.add_argument("--raw", type=Path, default=HERE / "raw")
    ap.add_argument("--dest", type=Path, default=ROOT / "data" / "demo" / "kaggle")
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    ap.add_argument("--min-duration", type=float, default=30.0,
                    help="skip floors whose longest held-out walk is shorter than this [s]")
    ap.add_argument("--sites", nargs="*", help="only these buildings (ids, or the ends of ids)")
    args = ap.parse_args()

    longest = longest_per_group(json.loads((args.out_dir / "manifest_holdout.json").read_text())["traces"],
                                need_scans=True)
    picks = [t for t in longest.values() if t["duration_s"] >= args.min_duration]
    if args.sites:
        picks = [t for t in picks if any(t["group"].split("_", 1)[0].endswith(s) for s in args.sites)]
    picks.sort(key=lambda t: (t["group"].split("_", 1)[0], floor_key(t["group"].split("_", 1)[1])))
    if not picks:
        raise SystemExit("no floor has a held-out walk that long")

    if args.dest.exists():
        shutil.rmtree(args.dest)
    args.dest.mkdir(parents=True)
    (args.dest / "README.txt").write_text(
        "Converted from Kaggle's \"Indoor Location & Navigation\" competition data by datasets/kaggle/export_demo.py.\n"
        "The competition's rules do not allow redistribution: keep this folder local (it is git-ignored).\n")

    demos = []
    for t in picks:
        site, floor = t["group"].split("_", 1)
        src_trace = args.out_dir / t["trace"]
        rel = f"{site}/{floor}"
        floor_dir = args.dest / site / floor
        floor_dir.mkdir(parents=True)
        # The place on the globe comes from the raw plan, so that output converted before the converter
        # read the full set's Web Mercator plans needs no re-fit. The engine does not use it; the 3D view does.
        venue = json.loads((args.out_dir / t["venue"]).read_text())
        venue["georef"] = georef(load_floor(args.raw / "metadata" / site / floor))
        (floor_dir / "venue.json").write_text(json.dumps(venue))
        shutil.copy(src_trace, floor_dir / src_trace.name)
        entry = {
            "id": f"kaggle_{site[-6:]}_{floor}_{src_trace.stem[:8]}",
            "group": f"Kaggle building …{site[-6:]}",
            "title": f"…{site[-6:]} {floor}, {t['duration_s']:.0f} s walk",
            "venue": f"{rel}/venue.json",
            "geojson": f"{rel}/venue.geojson",
            "trace": f"{rel}/{src_trace.name}",
            "config": "../../configs/fingerprint.json",
        }
        image = args.raw / "metadata" / site / floor / "floor_image.png"
        if image.exists():
            shutil.copy(image, floor_dir / "floor.png")
            entry["image"] = f"{rel}/floor.png"
        export_geojson(args.build, floor_dir / "venue.json", floor_dir / "venue.geojson")

        pts = waypoints(src_trace)
        beacons = venue["beacons"]
        fitted = sum(b.get("model", True) for b in beacons)
        scans = len(venue.get("floor_fingerprints", {}).get("xy", []))
        entry.update({
            "view": view_box(venue["bounds"], pts, VIEW_PAD_M),
            "duration_s": round(t["duration_s"], 1),
            "waypoints": len(pts),
            "description":
                f"Kaggle Indoor Location & Navigation, building …{site[-6:]}, floor {floor}: a {t['duration_s']:.0f} s "
                f"walk with a real Android phone (IMU + Wi-Fi), held out from fitting and tuning. Squares are the "
                f"surveyor's waypoints, the only ground truth; the black path is interpolated between them. The "
                f"engine uses data/configs/fingerprint.json: {scans} training scans of this floor as position "
                f"fingerprints, next to the log-distance model of {fitted} located access points (purple). The "
                f"other {len(beacons) - fitted} access points (pale) fit no such model and count in the "
                f"fingerprints only. This is the live, causal estimate; the competition score in "
                f"docs/datasets.md adds offline steps the page does not run. The floor is given here.",
        })
        demos.append(entry)
        print(f"{site[-6:]} {floor}: {src_trace.name} ({t['duration_s']:.0f} s, {len(pts)} waypoints, "
              f"{scans} training scans, {fitted}+{len(beacons) - fitted} access points)")

    index = {
        "menu": "Real recordings (Kaggle indoor location, local only)",
        "source": "Kaggle \"Indoor Location & Navigation\" competition data; not redistributable, local only",
        "selection": f"longest held-out walk with a Wi-Fi scan on each floor where it lasts at least {args.min_duration:g} s",
        "default": demos[0]["id"],
        "demos": demos,
    }
    write_index(args.dest, index, ensure_ascii=False)
    print(f"{len(demos)} floors in {len({d['group'] for d in demos})} buildings")


if __name__ == "__main__":
    main()
