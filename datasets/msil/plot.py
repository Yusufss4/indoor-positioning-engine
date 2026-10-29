#!/usr/bin/env python3
"""Draws one converted Microsoft Indoor Location trace over its floor plan.

Recorded traces are scored only at the surveyor's waypoints, so the figure shows the waypoints,
each filter's estimate at those times and the error between them, rather than a dense track.

    .venv/bin/python datasets/msil/plot.py --trace site1/F3/<path>.csv --out docs/img/msil_trajectory.png

With --walls-only, it instead draws a floor's walls over the floor image, with the doorways the
converter cut and the test walks' waypoints, to check the walls against the plan:

    .venv/bin/python datasets/msil/plot.py --walls-only site2/F1 --out docs/img/msil_walls.png
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


def waypoints(trace: Path) -> np.ndarray:
    pts = [line.split(",")[2:4] for line in trace.read_text().splitlines() if line.startswith("gt,")]
    return np.array(pts, dtype=float).reshape(-1, 2)


def plot_walls(args):
    site, floor = args.walls_only.split("/")
    floor_dir = args.out_dir / site / floor
    venue = ipeviz.Venue.load(floor_dir / "venue.json")
    report = json.loads((args.out_dir / "report.json").read_text())
    doors = next((np.array(r.get("door_centres", [])).reshape(-1, 2) for r in report
                  if r["site"] == site and r["floor"] == floor), np.zeros((0, 2)))

    fig, ax = plt.subplots(figsize=(10, 7.5))
    x0, y0, x1, y1 = venue.bounds
    ax.imshow(plt.imread(args.raw / site / floor / "floor_image.png"), extent=(x0, x1, y0, y1), origin="upper",
              zorder=0)
    for w in venue.walls:
        ax.plot([w[0], w[2]], [w[1], w[3]], color="#222222", lw=1.0, zorder=1)
    for i, trace in enumerate(sorted(floor_dir.glob("*.csv"))):
        wp = waypoints(trace)
        ax.plot(wp[:, 0], wp[:, 1], "-", color="#1f77b4", lw=0.8, alpha=0.7, zorder=2,
                label="test walks (waypoints)" if i == 0 else None)
    ax.plot(doors[:, 0], doors[:, 1], "o", mfc="none", mec="#d62728", ms=7, mew=1.5, zorder=3,
            label=f"doorways from training walks ({len(doors)})")
    ax.set_xlim(x0, x1)
    ax.set_ylim(y0, y1)
    ax.set_aspect("equal")
    ax.legend(loc="lower right", fontsize=8, framealpha=0.9)
    ax.set_title(f"Microsoft Indoor Location {site} {floor}: {len(venue.walls)} wall segments")
    fig.tight_layout()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=120)
    print(f"wrote {args.out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", type=Path, default=HERE / "out", help="converter output folder")
    ap.add_argument("--raw", type=Path, default=HERE / "raw" / "data")
    ap.add_argument("--trace", help="trace path relative to --out-dir, e.g. site1/F3/<id>.csv")
    ap.add_argument("--walls-only", metavar="SITE/FLOOR", help="draw a floor's walls and doorways, e.g. site2/F1")
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    if args.walls_only:
        plot_walls(args)
        return
    if not args.trace:
        ap.error("--trace or --walls-only is required")

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
