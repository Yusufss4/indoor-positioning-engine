#!/usr/bin/env python3
"""Does ipe-plan's map-only score match the UJI measurements?

For every zone and power: `ipe-plan analyze` on the venue (0.5 m grid) gives, per point, the beacons
expected above -85 dBm and the Cramer-Rao bound. At each surveyed point these are compared with the
beacons actually heard per fingerprint and with the static replay's CEP50 (A5 sessions, every seed
of evaluate.py --json). Spearman correlations over points.

    python3 datasets/uji_ble/plan_check.py --replay datasets/uji_ble/out/replay.json
"""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from scipy.stats import spearmanr

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

from load import load_zone  # noqa: E402

VENUES = [("lib", -12, "lib"), ("geo", -4, "geo_m4"), ("geo", -12, "geo_m12"), ("geo", -20, "geo_m20")]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--replay", type=Path, required=True, help="evaluate.py --json output over manifest.json")
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    ap.add_argument("--json", type=Path, default=HERE / "out" / "plan_check.json")
    args = ap.parse_args()

    walks = json.loads(args.replay.read_text())["walks"]
    manifest = {e["name"]: e for e in json.loads((HERE / "out" / "manifest.json").read_text())["traces"]}
    rows = []
    print("| Venue | Points | Heard: planned | Heard: measured | rho(heard) | Bound p50 [m] | CEP50 p50 [m] | rho(bound, CEP50) |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for zname, power, vname in VENUES:
        z = load_zone(zname)
        with tempfile.TemporaryDirectory() as tmp:
            grid_csv = Path(tmp) / "grid.csv"
            subprocess.run([str(args.build / "tools" / "ipe-plan"), "analyze", "--venue", str(ROOT / "data" / "uji_ble" / f"{vname}.json"),
                            "--grid", "0.5", "--out", str(grid_csv)], check=True, capture_output=True)
            grid = np.array([[float(r["x"]), float(r["y"]), float(r["heard"]), float(r["crlb_m"])] for r in csv.DictReader(open(grid_csv))])
        pts = sorted({(round(x, 3), round(y, 3)) for x, y in z.xy[z.power == power]})
        planned_heard, measured_heard, bound, cep = [], [], [], []
        for x, y in pts:
            k = int(np.argmin(np.hypot(grid[:, 0] - x, grid[:, 1] - y)))
            sel = (z.power == power) & (np.round(z.xy[:, 0], 3) == x) & (np.round(z.xy[:, 1], 3) == y) & (z.phone == 1)
            if not sel.any():
                continue
            ceps = [w["cep50_m"] for w in walks if w["name"] in manifest and manifest[w["name"]]["group"] == f"{vname}_A5"
                    and abs(manifest[w["name"]]["x"] - x) < 1e-3 and abs(manifest[w["name"]]["y"] - y) < 1e-3]
            if not ceps:
                continue
            planned_heard.append(grid[k, 2])
            measured_heard.append(float(np.mean(np.sum(~np.isnan(z.rss[sel]), axis=1))))
            bound.append(grid[k, 3])
            cep.append(float(np.mean(ceps)))
        rho_h = spearmanr(planned_heard, measured_heard).statistic
        rho_b = spearmanr(bound, cep).statistic
        row = {"venue": vname, "points": len(cep), "planned_heard_mean": float(np.mean(planned_heard)),
               "measured_heard_mean": float(np.mean(measured_heard)), "rho_heard": float(rho_h),
               "bound_p50_m": float(np.median(bound)), "cep50_p50_m": float(np.median(cep)), "rho_bound_cep50": float(rho_b)}
        rows.append(row)
        print(f"| {vname} | {row['points']} | {row['planned_heard_mean']:.1f} | {row['measured_heard_mean']:.1f} | {rho_h:.2f} | "
              f"{row['bound_p50_m']:.2f} | {row['cep50_p50_m']:.2f} | {rho_b:.2f} |")
    args.json.write_text(json.dumps(rows, indent=1))


if __name__ == "__main__":
    main()
