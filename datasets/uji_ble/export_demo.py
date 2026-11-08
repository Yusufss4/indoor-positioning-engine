#!/usr/bin/env python3
"""Copies one standing-still session per UJI BLE venue into data/real_uji/ for the web demo.

Per venue (library; lab at -4, -12, -20 dBm): the held-out session of the reference phone (A5) with the
most fingerprints, ties broken by point id, so the pick is mechanical. The zones have local coordinates
only (no georeference), so there is no 3D view and no plan image: the map is the beacons and the mapped
shelves and pillars.

    .venv/bin/python datasets/uji_ble/convert.py
    .venv/bin/python datasets/uji_ble/export_demo.py
"""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

TITLES = {"lib": "UJI library, bookshelves, -12 dBm", "geo_m4": "UJI lab, -4 dBm", "geo_m12": "UJI lab, -12 dBm",
          "geo_m20": "UJI lab, -20 dBm"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-dir", type=Path, default=HERE / "out", help="convert.py output")
    ap.add_argument("--dest", type=Path, default=ROOT / "data" / "real_uji")
    args = ap.parse_args()

    holdout = json.loads((args.out_dir / "manifest_holdout.json").read_text())["traces"]
    if args.dest.exists():
        shutil.rmtree(args.dest)
    args.dest.mkdir(parents=True)
    shutil.copy(ROOT / "data" / "uji_ble" / "LICENSE", args.dest / "LICENSE")
    demos = []
    for vname, title in TITLES.items():
        cands = [e for e in holdout if e["group"] == f"{vname}_A5"]
        e = max(cands, key=lambda c: (c["fingerprints"], c["name"]))
        d = args.dest / vname
        d.mkdir()
        shutil.copy(ROOT / "data" / "uji_ble" / f"{vname}.json", d / "venue.json")
        src = args.out_dir / e["trace"]
        shutil.copy(src, d / src.name)
        demos.append({
            "id": f"uji_{vname}",
            "title": f"{title}, {e['fingerprints']} scans at one point",
            "venue": f"{vname}/venue.json",
            "trace": f"{vname}/{src.name}",
            "waypoints": e["fingerprints"],
            "description": (
                f"{title}: a phone (Galaxy A5) standing still at a surveyed point (black square) for {e['fingerprints']} "
                "BLE scans, about one per second. Grey outlines are the mapped shelves and pillars, dots the surveyed "
                "beacons. The point was held out of the path-loss fit; the filter has no steps to go on, only BLE. "
                "Default particle filter config. Results over all points are in docs/uji_ble.md."),
        })
        print(f"{vname}: {src.name} ({e['fingerprints']} fingerprints)")
    index = {
        "menu": "Real recordings (UJI BLE, standing still)",
        "source": "UJI BLE RSS measurements database, CC BY 4.0 (see LICENSE)",
        "selection": "per venue, the held-out A5 session with the most fingerprints",
        "default": demos[0]["id"],
        "demos": demos,
    }
    (args.dest / "demos.json").write_text(json.dumps(index, indent=1) + "\n")
    print(f"wrote {args.dest / 'demos.json'}")


if __name__ == "__main__":
    main()
