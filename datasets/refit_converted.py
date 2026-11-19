#!/usr/bin/env python3
"""Fits every venue of a converted data set again with fit_radio_map.py, from the training traces alone,
and writes a copy of the data set with those venues: the check that the tool for a building of your own
does what the converters do.

    msil/convert.py --wifi --scans --fingerprints --train-traces --all-readings --out OUT
    refit_converted.py OUT OUT_refit
    ipe-regress --traces OUT/manifest_holdout.json --no-baseline         # the converter's radio maps
    ipe-regress --traces OUT_refit/manifest_holdout.json --no-baseline   # fit_radio_map.py's

The copy has the same manifests and links to the same test traces. Each venue loses its beacons,
fingerprints and slab loss and gets them back from `<venue folder>/train/*.csv` (--train-traces). The test
traces must hold every reading (--all-readings), because the two radio maps do not know the same
transmitters. See docs/datasets.md.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fit_radio_map  # noqa: E402
from common.trace import read_walk  # noqa: E402

DEFAULTS = SimpleNamespace(no_fingerprints=False, min_samples=30, min_spread=3.0, max_rms=8.0, min_fingerprint_scans=10)


def refit(job: tuple[Path, Path]) -> tuple[str, dict, dict]:
    src, dst = job
    venue = json.loads((src / "venue.json").read_text())
    before = {"fitted": sum(b.get("model", True) is not False for b in venue["beacons"]), "beacons": len(venue["beacons"]),
              "scans": len(venue.get("floor_fingerprints", {}).get("scans", []))}
    for key in ("beacons", "floor_fingerprints", "radio_map", "floor_attenuation_db"):
        venue.pop(key, None)
    walks = [read_walk(p) for p in sorted((src / "train").glob("*.csv"))]
    fitted, report = fit_radio_map.fit_venue(venue, walks, DEFAULTS)
    dst.mkdir(parents=True, exist_ok=True)
    (dst / "venue.json").write_text(json.dumps(fitted))
    for trace in src.glob("*.csv"):
        if not (dst / trace.name).exists():
            os.symlink(trace.resolve(), dst / trace.name)
    if (src / "waypoints.json").exists():
        shutil.copy(src / "waypoints.json", dst / "waypoints.json")
    return str(src), before, report


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("converted", type=Path, help="converter output with train/ folders")
    ap.add_argument("out", type=Path)
    ap.add_argument("--jobs", type=int, default=4, help="venues fitted in parallel")
    args = ap.parse_args()
    venues = sorted(args.converted.rglob("venue.json"))
    missing = [str(v.parent) for v in venues if not (v.parent / "train").is_dir()]
    if missing:
        sys.exit(f"error: no training traces in {missing[0]}: convert with --train-traces")
    args.out.mkdir(parents=True, exist_ok=True)
    for manifest in args.converted.glob("manifest*.json"):
        shutil.copy(manifest, args.out / manifest.name)
    rows = []
    with ProcessPoolExecutor(args.jobs) as pool:
        for src, before, report in pool.map(refit, [(v.parent, args.out / v.parent.relative_to(args.converted)) for v in venues]):
            rows.append({"venue": src, "converter": before, "fit_radio_map": report})
            print(f"{Path(src).relative_to(args.converted)}: converter {before['fitted']} fitted of {before['beacons']} beacons, "
                  f"{before['scans']} scans; fit_radio_map.py {report['fitted']} fitted of "
                  f"{report['fitted'] + report['fingerprint_only']}, {report['scans']} scans"
                  + (f", {report['floor_attenuation_db']} dB per slab" if report["floor_attenuation_db"] is not None else ""),
                  flush=True)
    (args.out / "refit_report.json").write_text(json.dumps(rows, indent=1) + "\n")


if __name__ == "__main__":
    main()
