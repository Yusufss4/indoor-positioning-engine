#!/usr/bin/env python3
"""Draws one converted Microsoft Indoor Location trace over its floor plan.

Recorded traces are scored only at the surveyor's waypoints, so the figure shows the waypoints,
each filter's estimate at those times and the error between them, rather than a dense track.

    .venv/bin/python datasets/msil/plot.py --trace site1/F3/<path>.csv --out docs/img/msil_trajectory.png
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "viz"))
import ipeviz  # noqa: E402

RUNS = [
    ("pf", "pf", ROOT / "data" / "configs" / "msil_wifi.json", "pf (Wi-Fi config)"),
    ("ekf", "ekf", None, "ekf"),
    ("wcl", "wcl", None, "wcl"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", type=Path, default=HERE / "out", help="converter output folder")
    ap.add_argument("--raw", type=Path, default=HERE / "raw" / "data")
    ap.add_argument("--trace", required=True, help="trace path relative to --out-dir, e.g. site1/F3/<id>.csv")
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    trace = args.out_dir / args.trace
    venue_path = trace.parent / "venue.json"
    site, floor = trace.parent.parent.name, trace.parent.name
    venue = ipeviz.Venue.load(venue_path)

    fig, ax = plt.subplots(figsize=(10, 7.5))
    ipeviz.draw_venue(ax, venue, beacon_labels=False, background=args.raw / site / floor / "floor_image.png",
                      beacon_size=8)
    first = None
    with tempfile.TemporaryDirectory() as tmp:
        for key, preset, config, label in RUNS:
            est_csv = Path(tmp) / f"{key}.csv"
            cmd = [str(args.build / "tools" / "ipe-replay"), "--venue", str(venue_path), "--trace", str(trace),
                   "--filter", preset, "--out", str(est_csv)]
            if config:
                cmd += ["--config", str(config)]
            metrics = json.loads(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout)
            est = ipeviz.Estimates.load(est_csv)
            first = first or est
            color = ipeviz.COLORS[key]
            m = est.has_fix & (est.t >= 5.0)
            for (tx, ty), (ex, ey) in zip(est.truth[m], est.est[m]):
                ax.plot([tx, ex], [ty, ey], color=color, lw=0.8, alpha=0.6, zorder=3)
            ax.plot(est.est[m, 0], est.est[m, 1], "o", color=color, ms=5, zorder=4,
                    label=f"{label}: CEP50 {metrics['cep50_m']:.1f} m, CEP95 {metrics['cep95_m']:.1f} m")
    ax.plot(first.truth[:, 0], first.truth[:, 1], "-s", color=ipeviz.COLORS["truth"], lw=1.8, ms=4,
            label="surveyor waypoints", zorder=5)
    pad = 25.0
    xs, ys = first.truth[:, 0], first.truth[:, 1]
    ax.set_xlim(max(venue.bounds[0], xs.min() - pad), min(venue.bounds[2], xs.max() + pad))
    ax.set_ylim(max(venue.bounds[1], ys.min() - pad), min(venue.bounds[3], ys.max() + pad))
    ax.legend(loc="upper right", fontsize=8, framealpha=0.9)
    ax.set_title(f"Microsoft Indoor Location {site} {floor}, held-out path {trace.stem[:8]}")
    fig.tight_layout()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=120)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
