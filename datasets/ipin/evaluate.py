#!/usr/bin/env python3
"""Replays an IPIN manifest with ipe-replay and reports what ipe-regress does not: the floor hit rate
and the competition metric (75th percentile of horizontal error plus 15 m per wrong floor, over the
landmarks of a walk), next to CEP50/CEP95 per walk.

    python3 datasets/ipin/evaluate.py datasets/ipin/out_scans/manifest_tune.json \
        --config data/configs/ipin_wifi.json [--set JSON] [--json out.json]
"""

from __future__ import annotations

import argparse
import csv
import json
import statistics
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def merge(a: dict, b: dict) -> dict:
    out = dict(a)
    for k, v in b.items():
        out[k] = merge(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else v
    return out


def replay(entry: dict, seed: int, base: Path, args, tmp: Path) -> dict:
    out = tmp / f"{entry['name']}_{seed}.csv"
    cmd = [str(args.build / "tools" / "ipe-replay"), "--venue", str(base / entry["venue"]), "--trace",
           str(base / entry["trace"]), "--warmup", str(args.warmup), "--out", str(out)]
    if args.config:
        cmd += ["--config", str(args.config)]
    patch = json.loads(args.set) if args.set else {}
    if args.seeds > 1:
        patch = merge(patch, {"pf": {"seed": seed}})
    if patch:
        cmd += ["--set", json.dumps(patch)]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    rows = list(csv.DictReader(open(out)))
    err = sorted(float(r["error_m"]) for r in rows if r["has_fix"] == "1")
    floor_ok = [r["truth_level"] == r["est_level"] for r in rows if r["has_fix"] == "1"]
    score = sorted(float(r["error_m"]) + 15.0 * abs(int(r["truth_level"]) - int(r["est_level"]))
                   for r in rows if r["has_fix"] == "1")

    def pct(v, q):
        if not v:
            return float("nan")
        k = max(0, min(len(v) - 1, int(round(q * (len(v) - 1)))))
        return v[k]

    return {"name": entry["name"], "group": entry["group"], "points": len(err), "cep50_m": pct(err, 0.5),
            "cep95_m": pct(err, 0.95), "floor_hit": sum(floor_ok) / len(floor_ok) if floor_ok else float("nan"),
            "ipin_p75_m": pct(score, 0.75), "floors": entry.get("floors"), "phone": entry.get("phone"),
            "barometer": entry.get("barometer")}


def summarise(rows: list[dict]) -> dict:
    def med(k):
        return statistics.median(r[k] for r in rows)

    return {"walks": len(rows), "median_cep50_m": med("cep50_m"), "median_cep95_m": med("cep95_m"),
            "median_floor_hit": med("floor_hit"), "median_ipin_p75_m": med("ipin_p75_m"),
            "pooled_floor_hit": sum(r["floor_hit"] * r["points"] for r in rows) / sum(r["points"] for r in rows)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("manifest", type=Path)
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    ap.add_argument("--config", type=Path)
    ap.add_argument("--set")
    ap.add_argument("--warmup", type=float, default=5.0)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--quiet", action="store_true", help="only the per-group summary")
    ap.add_argument("--seeds", type=int, default=1,
                    help="replay each walk with filter seeds 1..N and average its metrics (a walk is one recording; "
                         "the seeds only average out the filter's own randomness)")
    args = ap.parse_args()

    entries = json.loads(args.manifest.read_text())["traces"]
    jobs = [(e, k) for e in entries for k in range(1, args.seeds + 1)]
    with tempfile.TemporaryDirectory() as tmp, ThreadPoolExecutor() as pool:
        runs = list(pool.map(lambda j: replay(j[0], j[1], args.manifest.parent, args, Path(tmp)), jobs))
    rows = []
    for i, e in enumerate(entries):
        rs = runs[i * args.seeds:(i + 1) * args.seeds]
        row = dict(rs[0])
        for k in ("cep50_m", "cep95_m", "floor_hit", "ipin_p75_m"):
            row[k] = statistics.mean(r[k] for r in rs)
        rows.append(row)
    groups: dict[str, list] = {}
    for r in rows:
        groups.setdefault(r["group"], []).append(r)
    if not args.quiet:
        print("| Walk | Phone | Baro | Floors | Points | CEP50 [m] | CEP95 [m] | Floor hit | IPIN p75 [m] |")
        print("|---|---|---|---|---:|---:|---:|---:|---:|")
        for r in rows:
            print(f"| {r['name']} | {r['phone']} | {'yes' if r['barometer'] else 'no'} | {r['floors']} | {r['points']} | {r['cep50_m']:.2f} | {r['cep95_m']:.2f} | "
                  f"{r['floor_hit']:.0%} | {r['ipin_p75_m']:.2f} |")
        print()
    summary = {g: summarise(v) for g, v in groups.items()}
    print("| Group | Walks | Median CEP50 [m] | Median CEP95 [m] | Median floor hit | Pooled floor hit | Median IPIN p75 [m] |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for g, s in summary.items():
        print(f"| {g} | {s['walks']} | {s['median_cep50_m']:.2f} | {s['median_cep95_m']:.2f} | {s['median_floor_hit']:.0%} | "
              f"{s['pooled_floor_hit']:.0%} | {s['median_ipin_p75_m']:.2f} |")
    if args.json:
        args.json.write_text(json.dumps({"walks": rows, "groups": summary}, indent=1))


if __name__ == "__main__":
    main()
