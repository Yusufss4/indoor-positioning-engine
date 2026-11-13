#!/usr/bin/env python3
"""Copies held-out walks into data/demo/msil/ for the web demo.

Picks the longest held-out walk on every floor whose longest held-out walk lasts at least
--min-duration seconds, so the selection is mechanical rather than picked for good results. For each
floor it writes the venue,
its GeoJSON export (for the 3D view), the floor image and the trace, plus data/demo/msil/demos.json,
which the demo reads to fill its scenario menu.

The venues carry the shop walls with inferred doorways (see docs/datasets.md), the shop outlines as
rooms, and the floor's training scans as position fingerprints, which the demo's config
(data/configs/fingerprint.json) uses. The sample data is MIT-licensed; its licence is copied next to the
files.

    .venv/bin/python datasets/msil/convert.py --wifi --scans --walls doors --fingerprints \
        --out datasets/msil/out_doors_fp
    .venv/bin/python datasets/msil/export_demo.py
"""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

from convert import load_floor, room_list  # puts datasets/ on the path

from common.demo import export_geojson, longest_per_group, view_box, waypoints, write_index

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
VIEW_PAD_M = 25.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", type=Path, default=HERE / "out_doors_fp",
                    help="converter output folder (convert.py --wifi --scans --walls doors --fingerprints)")
    ap.add_argument("--raw", type=Path, default=HERE / "raw")
    ap.add_argument("--dest", type=Path, default=ROOT / "data" / "demo" / "msil")
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    ap.add_argument("--min-duration", type=float, default=60.0,
                    help="skip floors whose longest held-out walk is shorter than this [s]")
    ap.add_argument("--count", type=int, default=None, help="at most this many floors (one walk each)")
    ap.add_argument("--default", default="site1_F2", help="floor whose walk the demo opens with")
    args = ap.parse_args()

    longest = longest_per_group(json.loads((args.out_dir / "manifest_holdout.json").read_text())["traces"])
    picks = [t for t in sorted(longest.values(), key=lambda t: -t["duration_s"])
             if t["duration_s"] >= args.min_duration][: args.count]
    picks.sort(key=lambda t: t["group"])

    # The sample's licence: from the raw checkout, or the copy already next to the demo files (fetch.sh
    # downloads the data folder only). Read before anything is deleted.
    licence = next((p for p in (args.raw / "LICENSE", args.dest / "LICENSE") if p.exists()), None)
    if licence is None:
        raise SystemExit(f"no LICENSE in {args.raw} or {args.dest}: the demo files must ship with the sample's licence")
    licence_text = licence.read_bytes()
    if args.dest.exists():
        shutil.rmtree(args.dest)
    args.dest.mkdir(parents=True)
    (args.dest / "LICENSE").write_bytes(licence_text)

    demos = []
    for t in picks:
        group = t["group"]
        site, floor = group.split("_", 1)
        src_trace = args.out_dir / t["trace"]
        floor_dir = args.dest / group
        floor_dir.mkdir()
        # Rooms come from the raw plan, so converter output from before rooms existed needs no re-fit.
        venue = json.loads((args.out_dir / t["venue"]).read_text())
        venue["rooms"] = room_list(load_floor(args.raw / "data" / site / floor))
        (floor_dir / "venue.json").write_text(json.dumps(venue))
        shutil.copy(args.raw / "data" / site / floor / "floor_image.png", floor_dir / "floor.png")
        shutil.copy(src_trace, floor_dir / src_trace.name)
        export_geojson(args.build, floor_dir / "venue.json", floor_dir / "venue.geojson")

        fitted = sum(b.get("model", True) for b in venue["beacons"])
        scans = len(venue.get("floor_fingerprints", {}).get("xy", []))
        pts = waypoints(src_trace)
        demos.append({
            "id": f"{group}_{src_trace.stem[:8]}",
            "title": f"{site} {floor}, {t['duration_s']:.0f} s walk",
            "venue": f"{group}/venue.json",
            "geojson": f"{group}/venue.geojson",
            "image": f"{group}/floor.png",
            "trace": f"{group}/{src_trace.name}",
            "config": "../../configs/fingerprint.json",
            "view": view_box(venue["bounds"], pts, VIEW_PAD_M),
            "duration_s": round(t["duration_s"], 1),
            "waypoints": len(pts),
            "description":
                f"{site} {floor}: a {t['duration_s']:.0f} s walk through a shopping mall with a real Android phone "
                f"(IMU + Wi-Fi), held out from fitting and tuning. Squares are the surveyor's waypoints, the only "
                f"ground truth; the black path is interpolated between them, and the live error is measured against "
                f"it. The engine uses data/configs/fingerprint.json: {scans} training scans of this floor as "
                f"position fingerprints, next to the log-distance model of {fitted} located access points (purple). "
                f"The other {len(venue['beacons']) - fitted} access points (pale) fit no such model and count in "
                f"the fingerprints only. The shop walls are the plan's, with doorways inferred from the training "
                f"walks.",
        })
        print(f"{group}: {src_trace.name} ({t['duration_s']:.0f} s, {len(pts)} waypoints)")

    index = {
        "source": "Microsoft Indoor Location Competition 2.0 sample data, "
                  "https://github.com/location-competition/indoor-location-competition-20 (MIT, see LICENSE)",
        "selection": f"longest held-out walk on each floor where it lasts at least {args.min_duration:g} s",
        "default": next((d["id"] for d in demos if d["id"].startswith(args.default + "_")), demos[0]["id"]),
        "demos": demos,
    }
    write_index(args.dest, index)


if __name__ == "__main__":
    main()
