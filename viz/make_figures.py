#!/usr/bin/env python3
"""Regenerates every figure and results table under docs/ from the C++ tools.

usage: python viz/make_figures.py [--build build] [--out docs/img] [--seeds 10]
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ipeviz  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def run(cmd: list[str]) -> str:
    print("+", " ".join(cmd), file=sys.stderr)
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def replay(build: Path, scenario: str, preset: str, work: Path, particles_every: int = 0,
           patch: str | None = None, tag: str | None = None) -> tuple[Path, Path | None]:
    tag = tag or preset
    est = work / f"{scenario}_{tag}.csv"
    cmd = [str(build / "tools" / "ipe-replay"), "--scenario", str(ROOT / "data" / "scenarios" / f"{scenario}.json"),
           "--filter", preset, "--out", str(est)]
    if patch:
        cmd += ["--set", patch]
    parts = None
    if particles_every:
        parts = work / f"{scenario}_{tag}_particles.csv"
        cmd += ["--particles", str(parts), "--particles-every", str(particles_every)]
    run(cmd)
    return est, parts


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=str(ROOT / "build"))
    ap.add_argument("--out", default=str(ROOT / "docs" / "img"))
    ap.add_argument("--seeds", type=int, default=10)
    ap.add_argument("--skip-benchmark", action="store_true")
    args = ap.parse_args()

    build = Path(args.build)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    office = ipeviz.Venue.load(ROOT / "data" / "venues" / "office_floor.json")

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)

        runs = {}
        for preset in ["pf", "pf-nomap", "ekf", "wcl"]:
            est, _ = replay(build, "office_nominal", preset, work, particles_every=3 if preset == "pf" else 0)
            runs[preset] = ipeviz.Estimates.load(est)
        ipeviz.plot_trajectories(office, {k: runs[k] for k in ["pf", "ekf", "wcl"]},
                                 out / "office_nominal_trajectories.png", "office_nominal: estimated tracks")
        ipeviz.plot_error_over_time(runs, out / "office_nominal_error.png", "office_nominal: error over time")
        ipeviz.plot_error_cdf(runs, out / "office_nominal_cdf.png", "office_nominal: error CDF")

        particles = ipeviz.load_particles(work / "office_nominal_pf_particles.csv")
        ipeviz.animate_particles(office, runs["pf"], particles, out / "pf_convergence.gif", t_max=45.0, fps=8,
                                 title="Particle filter: global convergence then tracking")

        field = {}
        for preset in ["pf", "pf-nomap", "ekf"]:
            est, _ = replay(build, "office_field_issue", preset, work)
            field[preset] = ipeviz.Estimates.load(est)
        old, _ = replay(build, "office_field_issue", "pf", work, tag="pf-before",
                        patch='{"pf":{"beacon_integrity":false,"recovery_alpha_fast":0.3,'
                              '"rssi_offset_sigma":5,"step_scale_sigma":0.1}}')
        field_cmp = {"before": ipeviz.Estimates.load(old), "after": field["pf"]}
        highlight = {"C2": "#bbbbbb", "N3": "#bbbbbb", "S3": "#e377c2"}
        ipeviz.plot_trajectories(office, field, out / "field_issue_trajectories.png",
                                 "office_field_issue: C2/N3 dead (grey), S3 installed 16 m from its map position",
                                 highlight=highlight)
        ipeviz.plot_error_over_time(field_cmp, out / "field_issue_before_after.png",
                                    "office_field_issue: particle filter before vs after the recovery/integrity fix",
                                    colors={"before": "#ff7f0e", "after": "#d62728"})

    if not args.skip_benchmark:
        js = out.parent / "results.json"
        md = out.parent / "results.md"
        # Serial so the us/update column reflects one core, not a contended machine.
        run([str(build / "tools" / "ipe-regress"), "--scenarios", str(ROOT / "data" / "scenarios"), "--no-baseline",
             "--serial", "--seeds", str(args.seeds), "--json", str(js), "--markdown", str(md)])
        results = json.loads(js.read_text())
        ipeviz.plot_benchmark(results, out / "benchmark_cep95.png", "cep95_m")
        ipeviz.plot_benchmark(results, out / "benchmark_cep50.png", "cep50_m")

    print(f"figures written to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
