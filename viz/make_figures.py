#!/usr/bin/env python3
"""Regenerates the figures and the results table under docs/ from the C++ tools: docs/img/convergence.gif,
docs/img/benchmark_cep95.png, docs/results.md and docs/results.json. (docs/img/demo.gif is recorded from the
web demo by viz/record_demo.py.)

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


def replay(build: Path, scenario: str, preset: str, work: Path, cells_every: int = 0,
           patch: str | None = None, tag: str | None = None) -> tuple[Path, Path | None]:
    tag = tag or preset
    est = work / f"{scenario}_{tag}.csv"
    cmd = [str(build / "tools" / "ipe-replay"), "--scenario", str(ROOT / "data" / "scenarios" / f"{scenario}.json"),
           "--filter", preset, "--out", str(est)]
    if patch:
        cmd += ["--set", patch]
    parts = None
    if cells_every:
        parts = work / f"{scenario}_{tag}_cells.csv"
        cmd += ["--cells", str(parts), "--cells-every", str(cells_every)]
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

        est, _ = replay(build, "office_nominal", "grid", work, cells_every=3)
        cells = ipeviz.load_cells(work / "office_nominal_grid_cells.csv")
        ipeviz.animate_cells(office, ipeviz.Estimates.load(est), cells, out / "convergence.gif", t_max=45.0, fps=8,
                             title="Grid filter: the posterior's heaviest cells, from start to tracking")

    if not args.skip_benchmark:
        js = out.parent / "results.json"
        md = out.parent / "results.md"
        # Serial so the us/update column reflects one core, not a contended machine.
        run([str(build / "tools" / "ipe-regress"), "--scenarios", str(ROOT / "data" / "scenarios"), "--no-baseline",
             "--serial", "--seeds", str(args.seeds), "--json", str(js), "--markdown", str(md)])
        results = json.loads(js.read_text())
        ipeviz.plot_benchmark(results, out / "benchmark_cep95.png", "cep95_m")

    print(f"figures written to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
