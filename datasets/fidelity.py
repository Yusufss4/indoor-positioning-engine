#!/usr/bin/env python3
"""How far apart two configurations put the estimate on the same walks, without ground truth.

Replays every walk of a manifest twice, with patch A and with patch B, and compares the two tracks (the
estimate every 0.2 s, `ipe-replay --track`): the distance between them, how often they are on different
levels, and which walks' convergence time moved.

Two uses:

* An approximation of the filter (`filter.active_floor`, on by default) against the exact filter: A is
  `{"filter": {"active_floor": 0}}`.
* The noise of a comparison. The replay is deterministic, but on real Wi-Fi walks the filter amplifies
  small differences: with B a setting changed in its third digit (`{"rssi": {"sigma_db": 8.01}}`), the
  output is what "no real change" looks like. A difference between two configurations that is no larger
  than that is not a result. Ground truth that exists only at waypoints makes this worse: convergence is
  then the first waypoint under 3 m, and one waypoint moves a walk by the gap to the next.

    python3 datasets/fidelity.py datasets/ipin/out_fp/manifest_tune.json \
        --a '{"filter": {"active_floor": 0}}' --b '{}' [--cache DIR] [--json out.json]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(entry: dict, base: Path, build: Path, patch: str, args, store: Path) -> dict:
    """One replay: the track as (t, x, y, level) rows with a valid estimate, and the walk's metrics."""
    replay = build / "tools" / "ipe-replay"
    key = hashlib.sha1("\0".join([
        str(replay.resolve()), str(replay.stat().st_mtime_ns), args.config.read_text() if args.config else "",
        patch, str(args.warmup), str((base / entry["trace"]).resolve())]).encode()).hexdigest()[:16]
    track, metrics = store / f"{key}.csv", store / f"{key}.json"
    if not (track.exists() and metrics.exists()):
        cmd = [str(replay), "--venue", str(base / entry["venue"]), "--trace", str(base / entry["trace"]),
               "--warmup", str(args.warmup), "--set", patch, "--track", str(track) + ".part",
               "--metrics", str(metrics)]
        if args.config:
            cmd += ["--config", str(args.config)]
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        Path(str(track) + ".part").rename(track)
    rows = {}
    lines = track.read_text().splitlines()
    cols = lines[0].split(",")
    for line in lines[1:]:
        r = dict(zip(cols, line.split(",")))
        if int(float(r["valid"])):
            rows[r["t"]] = (float(r["x"]), float(r["y"]), int(float(r["level"])))
    return {"track": rows, "metrics": json.loads(metrics.read_text())}


def percentile(sorted_values: list[float], q: float) -> float:
    if not sorted_values:
        return float("nan")
    return sorted_values[min(len(sorted_values) - 1, int(q * len(sorted_values)))]


def summarise(apart: list[float], levels_differ: int) -> dict:
    v = sorted(apart)
    n = max(len(v), 1)
    return {"samples": len(v), "p50_m": percentile(v, 0.5), "p90_m": percentile(v, 0.9), "p95_m": percentile(v, 0.95),
            "p99_m": percentile(v, 0.99), "max_m": v[-1] if v else float("nan"),
            "over_1m": sum(d > 1.0 for d in v) / n, "over_3m": sum(d > 3.0 for d in v) / n,
            "level_differs": levels_differ / n}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("manifest", type=Path)
    ap.add_argument("--a", default="{}", help="JSON merge patch of the reference run (default: none)")
    ap.add_argument("--b", required=True, help="JSON merge patch of the run compared with it")
    ap.add_argument("--build", type=Path, default=ROOT / "build", help="build directory of both runs")
    ap.add_argument("--build-b", type=Path, help="another build directory for run B (to compare two versions)")
    ap.add_argument("--config", type=Path, help="JSON patch file both runs apply before their own patch")
    ap.add_argument("--warmup", type=float, default=5.0)
    ap.add_argument("--cache", type=Path,
                    help="keep the tracks here; a replay with the same binary, config, patch and trace is reused")
    ap.add_argument("--only", help="replay only the walks whose name or group contains this text")
    ap.add_argument("--quiet", action="store_true", help="only the pooled summary")
    ap.add_argument("--json", type=Path)
    ap.add_argument("--jobs", type=int, default=12,
                    help="replays at once (an engine on a mall-size floor holds over 100 MB)")
    args = ap.parse_args()

    entries = json.loads(args.manifest.read_text())["traces"]
    if args.only:
        entries = [e for e in entries if args.only in e["name"] or args.only in e["group"]]
    base = args.manifest.parent
    build_b = args.build_b or args.build
    with tempfile.TemporaryDirectory() as tmp, ThreadPoolExecutor(args.jobs) as pool:
        store = args.cache or Path(tmp)
        store.mkdir(parents=True, exist_ok=True)
        jobs = [(e, args.build, args.a) for e in entries] + [(e, build_b, args.b) for e in entries]
        runs = list(pool.map(lambda j: run(j[0], base, j[1], j[2], args, store), jobs))
    walks, pooled, pooled_levels = [], [], 0
    for e, a, b in zip(entries, runs[:len(entries)], runs[len(entries):]):
        apart, levels = [], 0
        for t, (ax, ay, al) in a["track"].items():
            if t not in b["track"]:
                continue
            bx, by, bl = b["track"][t]
            apart.append(math.hypot(ax - bx, ay - by))
            levels += al != bl
        ca, cb = a["metrics"]["convergence_s"], b["metrics"]["convergence_s"]
        walks.append({"name": e["name"], "group": e["group"], **summarise(apart, levels),
                      "convergence_a_s": ca, "convergence_b_s": cb,
                      "a": {k: a["metrics"][k] for k in ("mean_m", "cep50_m", "cep95_m")},
                      "b": {k: b["metrics"][k] for k in ("mean_m", "cep50_m", "cep95_m")}})
        pooled += apart
        pooled_levels += levels
    summary = summarise(pooled, pooled_levels)

    def mean(side: str, key: str) -> float:
        return sum(w[side][key] for w in walks) / max(len(walks), 1)

    def mean_convergence(key: str) -> float:
        v = [w[key] for w in walks if w[key] >= 0.0]
        return sum(v) / len(v) if v else -1.0

    moved = [w for w in walks if abs(w["convergence_a_s"] - w["convergence_b_s"]) > 1.0]
    if not args.quiet:
        print("| Walk | p50 [m] | p95 [m] | Max [m] | Over 3 m | Other level | Converged A → B [s] |")
        print("|---|---:|---:|---:|---:|---:|---:|")
        for w in walks:
            print(f"| {w['name']} | {w['p50_m']:.2f} | {w['p95_m']:.2f} | {w['max_m']:.1f} | {100 * w['over_3m']:.1f}% | "
                  f"{100 * w['level_differs']:.1f}% | {w['convergence_a_s']:.0f} → {w['convergence_b_s']:.0f} |")
        print()
    print(f"B apart from A over {len(walks)} walks, {summary['samples']} samples: "
          f"p50 {summary['p50_m']:.2f} m, p90 {summary['p90_m']:.2f} m, p95 {summary['p95_m']:.2f} m, "
          f"p99 {summary['p99_m']:.2f} m; over 1 m {100 * summary['over_1m']:.1f}%, "
          f"over 3 m {100 * summary['over_3m']:.1f}%; on another level {100 * summary['level_differs']:.2f}%")
    print(f"mean over walks, A → B: converged {mean_convergence('convergence_a_s'):.1f} → "
          f"{mean_convergence('convergence_b_s'):.1f} s, mean {mean('a', 'mean_m'):.2f} → {mean('b', 'mean_m'):.2f} m, "
          f"CEP50 {mean('a', 'cep50_m'):.2f} → {mean('b', 'cep50_m'):.2f} m, "
          f"CEP95 {mean('a', 'cep95_m'):.2f} → {mean('b', 'cep95_m'):.2f} m")
    print(f"convergence moved on {len(moved)} of {len(walks)} walks" + (": " if moved else "") + ", ".join(
        f"{w['name']} {w['convergence_b_s'] - w['convergence_a_s']:+.0f} s" for w in moved))
    if args.json:
        args.json.write_text(json.dumps({"pooled": summary, "walks": walks}, indent=1) + "\n")


if __name__ == "__main__":
    main()
