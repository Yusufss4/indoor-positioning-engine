#!/usr/bin/env python3
"""Copies a few held-out walks into data/real/ for the web demo.

Picks the longest held-out walk on each of the floors with the longest held-out walks, so the
selection is mechanical rather than picked for good results. For each floor it writes the venue,
its GeoJSON export (for the 3D view), the floor image and the trace, plus data/real/demos.json,
which the demo reads to fill its scenario menu.

The venues carry the shop walls with inferred doorways (see docs/real_data.md). The sample data is
MIT-licensed; its licence is copied next to the files.

    .venv/bin/python datasets/msil/convert.py --wifi --scans --walls doors --out datasets/msil/out_doors
    .venv/bin/python datasets/msil/export_demo.py
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
VIEW_PAD_M = 25.0


def waypoints(trace: Path) -> list[tuple[float, float]]:
    pts = []
    for line in trace.read_text().splitlines():
        if line.startswith("gt,"):
            _, _, x, y, *_ = line.split(",")
            pts.append((float(x), float(y)))
    return pts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", type=Path, default=HERE / "out_doors",
                    help="converter output folder (convert.py --wifi --scans --walls doors)")
    ap.add_argument("--raw", type=Path, default=HERE / "raw")
    ap.add_argument("--dest", type=Path, default=ROOT / "data" / "real")
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    ap.add_argument("--count", type=int, default=3, help="number of floors (one walk each)")
    ap.add_argument("--default", default="site1_F2", help="floor whose walk the demo opens with")
    args = ap.parse_args()

    traces = json.loads((args.out_dir / "manifest_holdout.json").read_text())["traces"]
    longest = {}
    for t in traces:
        if t["group"] not in longest or t["duration_s"] > longest[t["group"]]["duration_s"]:
            longest[t["group"]] = t
    picks = sorted(longest.values(), key=lambda t: -t["duration_s"])[: args.count]

    if args.dest.exists():
        shutil.rmtree(args.dest)
    args.dest.mkdir(parents=True)
    shutil.copy(args.raw / "LICENSE", args.dest / "LICENSE")

    demos = []
    for t in picks:
        group = t["group"]
        site, floor = group.split("_", 1)
        src_trace = args.out_dir / t["trace"]
        floor_dir = args.dest / group
        floor_dir.mkdir()
        shutil.copy(args.out_dir / t["venue"], floor_dir / "venue.json")
        shutil.copy(args.raw / "data" / site / floor / "floor_image.png", floor_dir / "floor.png")
        shutil.copy(src_trace, floor_dir / src_trace.name)
        subprocess.run([str(args.build / "tools" / "ipe-import"), "export", str(floor_dir / "venue.json"),
                        "--out", str(floor_dir / "venue.geojson")], check=True, capture_output=True)

        bounds = json.loads((floor_dir / "venue.json").read_text())["bounds"]
        pts = waypoints(src_trace)
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        view = [max(bounds[0], min(xs) - VIEW_PAD_M), max(bounds[1], min(ys) - VIEW_PAD_M),
                min(bounds[2], max(xs) + VIEW_PAD_M), min(bounds[3], max(ys) + VIEW_PAD_M)]
        demos.append({
            "id": f"{group}_{src_trace.stem[:8]}",
            "title": f"{site} {floor}, {t['duration_s']:.0f} s walk",
            "venue": f"{group}/venue.json",
            "geojson": f"{group}/venue.geojson",
            "image": f"{group}/floor.png",
            "trace": f"{group}/{src_trace.name}",
            "config": "../configs/msil_wifi.json",
            "view": [round(v, 1) for v in view],
            "duration_s": round(t["duration_s"], 1),
            "waypoints": len(pts),
        })
        print(f"{group}: {src_trace.name} ({t['duration_s']:.0f} s, {len(pts)} waypoints)")

    index = {
        "source": "Microsoft Indoor Location Competition 2.0 sample data, "
                  "https://github.com/location-competition/indoor-location-competition-20 (MIT, see LICENSE)",
        "selection": "longest held-out walk on each of the floors with the longest held-out walks",
        "default": next((d["id"] for d in demos if d["id"].startswith(args.default + "_")), demos[0]["id"]),
        "demos": demos,
    }
    (args.dest / "demos.json").write_text(json.dumps(index, indent=1) + "\n")
    print(f"wrote {args.dest / 'demos.json'}")


if __name__ == "__main__":
    main()
