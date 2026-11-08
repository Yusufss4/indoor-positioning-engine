#!/usr/bin/env python3
"""Does the ipe-plan score predict simulated accuracy?

For each scenario, builds thinned beacon layouts of its venue (every beacon
left out in turn, and every k-th beacon kept), scores each layout along the
walk with `ipe-plan analyze` and simulates it with `ipe-regress`. Beacons are
removed from the venue itself, so they neither transmit nor appear on the map.

Every layout is simulated twice with disjoint seeds. The target is the mean
CEP of both; how well the two replicates rank the layouts alike is the noise
ceiling no planning metric can beat. Correlations are Spearman's, within one
scenario (same walk, same injected faults), over all layouts and over the
drop-one layouts only (the choice a beacon-removal optimiser makes).

usage: python viz/placement_study.py [--build build] [--seeds 10] [--windows 0,5,10,20] [--out DIR]
                                     [--reuse DIR/placement_study.json]
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SCENARIOS = ["office_nominal", "airport_hall_sparse", "office_floor_bleed", "kit_tour"]
LEAVE_ONE_OUT_MAX = 30
REPLICATE_SEED_OFFSET = 1000


def run(cmd: list[str]) -> str:
    print("+", " ".join(cmd), file=sys.stderr)
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def layouts(beacons: list[dict]) -> dict[str, list[dict]]:
    out = {"full": beacons}
    if len(beacons) <= LEAVE_ONE_OUT_MAX:
        for i, b in enumerate(beacons):
            out[f"drop_{b['id']}"] = beacons[:i] + beacons[i + 1:]
    for k in (2, 3, 4):
        for phase in range(k):
            kept = beacons[phase::k]
            if len(kept) >= 3:
                out[f"keep{k}_{phase}"] = kept
    return out


def rank(values: list[float]) -> list[float]:
    """Ranks with ties averaged; infinities rank last."""
    order = sorted(range(len(values)), key=lambda k: values[k])
    ranks = [0.0] * len(values)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and values[order[j + 1]] == values[order[i]]:
            j += 1
        for k in order[i:j + 1]:
            ranks[k] = (i + j) / 2.0
        i = j + 1
    return ranks


def spearman(x: list[float], y: list[float]) -> float:
    rx, ry = rank(x), rank(y)
    if len(rx) < 3:
        return float("nan")
    mx, my = statistics.fmean(rx), statistics.fmean(ry)
    sxy = sum((a - mx) * (b - my) for a, b in zip(rx, ry))
    sxx = sum((a - mx) ** 2 for a in rx)
    syy = sum((b - my) ** 2 for b in ry)
    return sxy / math.sqrt(sxx * syy) if sxx > 0 and syy > 0 else float("nan")


def as_float(v) -> float:
    return math.inf if v is None else float(v)


def simulate(tools: Path, scen_dir: Path, seeds: int, preset: str, out: Path) -> dict:
    run([str(tools / "ipe-regress"), "--scenarios", str(scen_dir), "--no-baseline", "--seeds", str(seeds),
         "--presets", preset, "--json", str(out)])
    return json.loads(out.read_text())["results"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=str(ROOT / "build"))
    ap.add_argument("--seeds", type=int, default=10, help="seeds per layout and replicate")
    ap.add_argument("--preset", default="pf")
    ap.add_argument("--scenarios", default=",".join(DEFAULT_SCENARIOS))
    ap.add_argument("--windows", default="0,5,10,20", help="ipe-plan --window values to compare")
    ap.add_argument("--out", help="directory for placement_study.json")
    ap.add_argument("--reuse", help="take the CEPs from an earlier placement_study.json instead of simulating")
    args = ap.parse_args()
    tools = Path(args.build) / "tools"
    names = args.scenarios.split(",")
    windows = [float(w) for w in args.windows.split(",")]
    # Planning metrics; sign +1 when a larger value should mean a larger error.
    metrics = {"heard_mean": -1, "share_heard_below_3": +1}
    for w in windows:
        metrics[f"bound_p50@{w:g}m"] = +1
        metrics[f"bound_p95@{w:g}m"] = +1

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for sub in ("venues", "a", "b"):
            (work / sub).mkdir()
        rows = []
        for name in names:
            scen_path = ROOT / "data" / "scenarios" / f"{name}.json"
            scen = json.loads(scen_path.read_text())
            venue_path = (scen_path.parent / scen["venue"]).resolve()
            venue = json.loads(venue_path.read_text())
            for tag, kept in layouts(venue["beacons"]).items():
                key = f"{name}__{tag}"
                vpath = work / "venues" / f"{key}.json"
                vpath.write_text(json.dumps({**venue, "beacons": kept}))
                variant = {**scen, "name": key, "venue": str(vpath)}
                (work / "a" / f"{key}.json").write_text(json.dumps(variant))
                seed_b = variant.get("seed", 1) + REPLICATE_SEED_OFFSET
                (work / "b" / f"{key}.json").write_text(json.dumps({**variant, "seed": seed_b}))
                row = {"scenario": name, "layout": tag, "beacons": len(kept)}
                for w in windows:
                    plan = json.loads(run([str(tools / "ipe-plan"), "analyze", "--scenario",
                                           str(work / "a" / f"{key}.json"), "--window", f"{w:g}"]))["path"]
                    row["heard_mean"] = plan["heard_mean"]
                    row["share_heard_below_3"] = plan["share_heard_below_3"]
                    row[f"bound_p50@{w:g}m"] = as_float(plan["crlb_p50_m"])
                    row[f"bound_p95@{w:g}m"] = as_float(plan["crlb_p95_m"])
                rows.append(row)

        if args.reuse:
            old = json.loads(Path(args.reuse).read_text())
            if old["seeds"] != args.seeds:
                raise SystemExit(f"{args.reuse} was simulated with {old['seeds']} seeds")
            sims = {(r["scenario"], r["layout"]): r for r in old["rows"]}
            for r in rows:
                s = sims[(r["scenario"], r["layout"])]
                for cep in ("cep50_m", "cep95_m"):
                    r[f"{cep}_a"], r[f"{cep}_b"], r[cep] = s[f"{cep}_a"], s[f"{cep}_b"], s[cep]
        else:
            res_a = simulate(tools, work / "a", args.seeds, args.preset, work / "a.json")
            res_b = simulate(tools, work / "b", args.seeds, args.preset, work / "b.json")
            for r in rows:
                key = f"{r['scenario']}__{r['layout']}"
                for cep in ("cep50_m", "cep95_m"):
                    a, b = res_a[key][args.preset][cep], res_b[key][args.preset][cep]
                    r[f"{cep}_a"], r[f"{cep}_b"], r[cep] = a, b, (a + b) / 2.0

    summary: dict = {}
    for name in names:
        groups = {"all": [r for r in rows if r["scenario"] == name]}
        groups["drop_one"] = [r for r in groups["all"] if r["layout"].startswith("drop_")]
        summary[name] = {g: {"layouts": len(sub)} for g, sub in groups.items()}
        for g, sub in groups.items():
            for cep in ("cep50_m", "cep95_m"):
                s = summary[name][g].setdefault(cep, {})
                s["replicates"] = spearman([r[f"{cep}_a"] for r in sub], [r[f"{cep}_b"] for r in sub])
                for m, sign in metrics.items():
                    s[m] = spearman([sign * r[m] for r in sub], [r[cep] for r in sub])

    print(f"Spearman rank correlation with {args.preset} CEP (mean of two {args.seeds}-seed replicates), "
          f"over all layouts / drop-one layouts. 'replicates' is the agreement of the two replicates.")
    cols = ["replicates", *metrics]
    for cep in ("cep50_m", "cep95_m"):
        print(f"\n{cep}\n")
        print("| Scenario | Layouts | " + " | ".join(cols) + " |")
        print("|" + "---|" * (2 + len(cols)))
        for name in names:
            s = summary[name]
            cells = []
            for c in cols:
                drop = s["drop_one"].get(cep, {}).get(c, float("nan"))
                cells.append(f"{s['all'][cep][c]:+.2f} / {drop:+.2f}")
            print(f"| {name} | {s['all']['layouts']} / {s['drop_one']['layouts']} | " + " | ".join(cells) + " |")

    if args.out:
        out = Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        text = json.dumps({"seeds": args.seeds, "rows": rows, "spearman": summary}, indent=2)
        (out / "placement_study.json").write_text(text.replace("Infinity", "null").replace("NaN", "null"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
