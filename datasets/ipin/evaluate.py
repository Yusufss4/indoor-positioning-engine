#!/usr/bin/env python3
"""Replays a manifest with ipe-replay and reports what ipe-regress does not, per walk and per group, next
to CEP50/CEP95:

* the floor hit rate;
* the IPIN competition metric: 75th percentile of horizontal error plus 15 m per wrong floor, over the
  landmarks of a walk. A competitor's score is the best of its scoring trials, so groups also get the
  best walk;
* the same percentile of the horizontal error alone, i.e. with the floor given: what the floor costs;
* the Kaggle indoor location metric: mean of horizontal error plus 15 m per wrong floor, over the
  waypoints. Groups get it pooled over all waypoints, as the leaderboard computes it.

The engine is causal, and so is everything above by default. --two-pass, --fill and --snap are offline
post-processing for competitions scored offline (Kaggle); they are not what a phone can show live.

    python3 datasets/ipin/evaluate.py datasets/ipin/out_scans/manifest_tune.json \
        --config data/configs/fingerprint.json [--set JSON] [--json out.json]
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "datasets"))

from common import offline  # noqa: E402

FLOOR_PENALTY_M = 15.0  # per floor of error, in both the IPIN and the Kaggle metric


def run_engine(venue: Path, trace: Path, out: Path, args) -> list[dict]:
    cmd = [str(args.build / "tools" / "ipe-replay"), "--venue", str(venue), "--trace", str(trace), "--warmup",
           str(args.warmup), "--out", str(out)]
    if args.config:
        cmd += ["--config", str(args.config)]
    if args.set:
        cmd += ["--set", args.set]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    rows = offline.read_estimates(out)
    for r in rows:
        r["error_m"] = math.hypot(r["est_x"] - r["truth_x"], r["est_y"] - r["truth_y"])
    return rows


def replay(entry: dict, base: Path, args, tmp: Path) -> dict:
    venue, trace = base / entry["venue"], base / entry["trace"]
    rows = run_engine(venue, trace, tmp / f"{entry['name']}.csv", args)
    if args.two_pass:
        reversed_trace = tmp / f"{entry['name']}_reversed.csv"
        end = offline.reverse_trace(trace, reversed_trace)
        back = run_engine(venue, reversed_trace, tmp / f"{entry['name']}_back.csv", args)
        rows = offline.fuse(rows, back, end, args.fuse_floor)
    if args.fill:
        offline.fill_gaps(rows)
    if args.snap > 0.0:
        waypoints = venue.parent / "waypoints.json"
        offline.snap(rows, json.loads(waypoints.read_text()) if waypoints.exists() else [], args.snap)
    fixes = [r for r in rows if r["has_fix"]]
    err = sorted(r["error_m"] for r in fixes)
    floor_ok = [r["truth_level"] == r["est_level"] for r in fixes]
    penalised = [r["error_m"] + FLOOR_PENALTY_M * abs(r["truth_level"] - r["est_level"]) for r in fixes]
    score = sorted(penalised)

    def pct(v, q):
        if not v:
            return float("nan")
        k = max(0, min(len(v) - 1, int(round(q * (len(v) - 1)))))
        return v[k]

    return {"name": entry["name"], "group": entry["group"], "points": len(err), "truth_points": len(rows),
            "cep50_m": pct(err, 0.5), "cep95_m": pct(err, 0.95),
            "floor_hit": sum(floor_ok) / len(floor_ok) if floor_ok else float("nan"),
            "ipin_p75_m": pct(score, 0.75), "h_p75_m": pct(err, 0.75),
            "kaggle_m": statistics.mean(penalised) if penalised else float("nan"),
            "floors": entry.get("floors"), "phone": entry.get("phone"), "barometer": entry.get("barometer")}


def summarise(rows: list[dict]) -> dict:
    def med(k):
        return statistics.median(r[k] for r in rows)

    # A walk without a single fix has nothing to score; fix_share still counts its rows.
    truth_points = sum(r["truth_points"] for r in rows)
    rows = [r for r in rows if r["points"] > 0]
    if not rows:
        nan = float("nan")
        return {"walks": 0, "median_cep50_m": nan, "median_cep95_m": nan, "median_floor_hit": nan, "median_ipin_p75_m": nan,
                "best_ipin_p75_m": nan, "median_h_p75_m": nan, "pooled_floor_hit": nan, "pooled_kaggle_m": nan,
                "fix_share": 0.0}
    points = sum(r["points"] for r in rows)
    return {"walks": len(rows), "median_cep50_m": med("cep50_m"), "median_cep95_m": med("cep95_m"),
            "median_floor_hit": med("floor_hit"), "median_ipin_p75_m": med("ipin_p75_m"),
            "best_ipin_p75_m": min(r["ipin_p75_m"] for r in rows), "median_h_p75_m": med("h_p75_m"),
            "pooled_floor_hit": sum(r["floor_hit"] * r["points"] for r in rows) / points,
            "pooled_kaggle_m": sum(r["kaggle_m"] * r["points"] for r in rows) / points,
            "fix_share": points / truth_points}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("manifest", type=Path)
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    ap.add_argument("--config", type=Path)
    ap.add_argument("--set")
    ap.add_argument("--warmup", type=float, default=5.0)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--quiet", action="store_true", help="only the per-group summary")
    ap.add_argument("--two-pass", action="store_true",
                    help="offline: replay each walk forwards and backwards and combine the two estimates "
                         "(datasets/common/offline.py)")
    ap.add_argument("--fuse-floor", type=float, default=3.0,
                    help="with --two-pass, metres added (in quadrature) to each pass's spread before weighting")
    ap.add_argument("--fill", action="store_true",
                    help="offline: truth rows without a fix take the nearest fix in time, so every row is scored")
    ap.add_argument("--snap", type=float, default=0.0,
                    help="offline: move each fix onto the nearest surveyed waypoint within this many metres "
                         "(waypoints.json next to the venue, written by the MSIL converter with --fingerprints)")
    ap.add_argument("--only", help="replay only the walks whose name or group contains this text")
    ap.add_argument("--jobs", type=int, default=12,
                    help="walks replayed at once (an engine on a mall-size floor holds over 100 MB)")
    args = ap.parse_args()

    entries = json.loads(args.manifest.read_text())["traces"]
    if args.only:
        entries = [e for e in entries if args.only in e["name"] or args.only in e["group"]]
    # The filter has no random seed: a walk is replayed once.
    with tempfile.TemporaryDirectory() as tmp, ThreadPoolExecutor(args.jobs) as pool:
        rows = list(pool.map(lambda e: replay(e, args.manifest.parent, args, Path(tmp)), entries))
    groups: dict[str, list] = {}
    for r in rows:
        groups.setdefault(r["group"], []).append(r)
    if not args.quiet:
        print("| Walk | Phone | Baro | Floors | Points | CEP50 [m] | CEP95 [m] | Floor hit | IPIN p75 [m] | p75, floor given [m] | Kaggle [m] |")
        print("|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|")
        for r in rows:
            print(f"| {r['name']} | {r['phone']} | {'yes' if r['barometer'] else 'no'} | {r['floors']} | {r['points']} | {r['cep50_m']:.2f} | {r['cep95_m']:.2f} | "
                  f"{r['floor_hit']:.0%} | {r['ipin_p75_m']:.2f} | {r['h_p75_m']:.2f} | {r['kaggle_m']:.2f} |")
        print()
    summary = {g: summarise(v) for g, v in groups.items()}
    if len(groups) > 1:
        summary["all"] = summarise(rows)
    print("| Group | Walks | Median CEP50 [m] | Median CEP95 [m] | Median floor hit | Pooled floor hit | Median IPIN p75 [m] | Best IPIN p75 [m] | "
          "Median p75, floor given [m] | Kaggle, pooled [m] | Fixes |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for g, s in summary.items():
        print(f"| {g} | {s['walks']} | {s['median_cep50_m']:.2f} | {s['median_cep95_m']:.2f} | {s['median_floor_hit']:.0%} | "
              f"{s['pooled_floor_hit']:.0%} | {s['median_ipin_p75_m']:.2f} | {s['best_ipin_p75_m']:.2f} | {s['median_h_p75_m']:.2f} | "
              f"{s['pooled_kaggle_m']:.2f} | {s['fix_share']:.0%} |")
    if args.json:
        args.json.write_text(json.dumps({"walks": rows, "groups": summary}, indent=1))


if __name__ == "__main__":
    main()
