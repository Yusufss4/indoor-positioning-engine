#!/usr/bin/env python3
"""Checks datasets/fit_radio_map.py on simulated walks: the venue's beacons are taken away, fitted back
from walks with a tap every few seconds, and a walk the fit never saw is replayed on the result.

    fit_radio_map_check.py --build BUILD_DIR

Run by CTest (`fit_radio_map`). Exits 77 (skipped) without numpy and scipy.
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

try:
    import numpy  # noqa: F401
    import scipy  # noqa: F401
except ImportError as e:
    print(f"skipped: {e}")
    sys.exit(77)

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "datasets" / "fit_radio_map.py"
TAP_EVERY = 25  # truth rows; the simulator writes one every 0.2 s, so a tap every 5 s
SCAN_S = 2.0


def run(cmd: list, ok: bool = True) -> subprocess.CompletedProcess:
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if ok and p.returncode != 0:
        raise SystemExit(f"FAILED ({p.returncode}): {' '.join(str(c) for c in cmd)}\n{p.stdout}{p.stderr}")
    return p


def check(condition: bool, what: str) -> None:
    print(("ok    " if condition else "FAIL  ") + what)
    if not condition:
        check.failed = True


check.failed = False


def simulate(build: Path, scenario: str, seed: int, out: Path) -> Path:
    run([build / "tools" / "ipe-sim", "--scenario", ROOT / "data" / "scenarios" / f"{scenario}.json", "--seed", seed,
         "--out", out])
    return out


def rewrite(src: Path, dst: Path, taps: bool, scans: bool) -> Path:
    """The trace as a walker with a phone would record it: with `taps`, a truth row only now and then;
    with `scans`, the radio packets of every two seconds as one scan (the strongest reading per id)."""
    lines = src.read_text().splitlines()
    gt = [i for i, line in enumerate(lines) if line.startswith("gt,")]
    keep = set(gt) if not taps else set(gt[::TAP_EVERY]) | {gt[-1]}
    out, batch, n_scans = [], {}, 0

    def flush(t_end: float) -> None:
        nonlocal batch, n_scans
        if batch:
            n_scans += 1
            out.extend(f"ble,{t_end:.3f},{tid},{rssi:.0f},{n_scans}" for tid, rssi in sorted(batch.items()))
            batch = {}

    window_end = SCAN_S
    for i, line in enumerate(lines):
        if line.startswith("gt,") and i not in keep:
            continue
        if scans and line[:4] in ("imu,", "ble,", "gt,", "baro"):
            t = float(line.split(",")[1])
            while t >= window_end:
                flush(window_end)
                window_end += SCAN_S
        if scans and line.startswith("ble,"):
            _, _, tid, rssi = line.split(",")[:4]
            batch[tid] = max(batch.get(tid, -200.0), float(rssi))
            continue
        out.append(line)
    dst.write_text("\n".join(out) + "\n")
    return dst


def replay(build: Path, venue: Path, trace: Path) -> dict:
    return json.loads(run([build / "tools" / "ipe-replay", "--venue", venue, "--trace", trace]).stdout)


def position_errors(fitted: dict, true: list[dict]) -> tuple[list[float], int]:
    """Distance of every fitted beacon from the true one, and how many are on the wrong floor."""
    by_id = {b["id"]: b for b in true}
    errs, wrong = [], 0
    for b in fitted["beacons"]:
        if b.get("model", True) is False:
            continue
        t = by_id[b["id"]]
        errs.append(math.hypot(b["x"] - t["x"], b["y"] - t["y"]))
        wrong += b.get("level", 0) != t.get("level", 0)
    return errs, wrong


def case(build: Path, tmp: Path, name: str, venue_file: str, scenarios: list[str], seeds: list[int], held: tuple[str, int],
         scans: bool, bounds: dict) -> None:
    print(f"--- {name}")
    venue = json.loads((ROOT / "data" / "venues" / venue_file).read_text())
    true = venue.pop("beacons")
    venue.pop("floor_attenuation_db", None)
    walls = tmp / f"{name}_walls.json"
    walls.write_text(json.dumps(venue))
    walks = [rewrite(simulate(build, sc, seed, tmp / f"{name}_{sc}_{seed}_raw.csv"), tmp / f"{name}_{sc}_{seed}.csv", True, scans)
             for sc in scenarios for seed in seeds]
    out = tmp / f"{name}_fitted.json"
    p = run([sys.executable, TOOL, "--venue", walls, "--traces", *walks, "--out", out, "--cross-validate", "--build", build,
             "--folds", 2])
    print(p.stdout.rstrip())
    fitted = json.loads(out.read_text())
    errs, wrong = position_errors(fitted, true)
    check(fitted.get("radio_map") == "fitted", "the venue is marked as fitted")
    check(len(errs) >= bounds["fitted"], f"{len(errs)} of {len(true)} beacons fitted (at least {bounds['fitted']})")
    check(wrong == 0, f"{wrong} fitted beacons on the wrong floor")
    med = statistics.median(errs)
    check(med <= bounds["median_m"], f"median position error {med:.2f} m (at most {bounds['median_m']})")
    check("cross-validation over" in p.stdout, "cross-validation printed its summary")
    if scans:
        table = fitted.get("floor_fingerprints", {})
        check(len(table.get("scans", [])) > 50 and len(table.get("xy", [])) == len(table.get("scans", [])),
              f"{len(table.get('scans', []))} scans with positions in the fingerprint table")
        cfg = json.loads(run([build / "tools" / "ipe-replay", "--venue", out, "--print-config"]).stdout)
        check(cfg["filter"]["fingerprint_weight"] > 0, "the engine turns fingerprints on for this venue")
    else:
        check("floor_fingerprints" not in fitted, "no fingerprint table from lone packets")

    held_trace = rewrite(simulate(build, held[0], held[1], tmp / f"{name}_held_raw.csv"), tmp / f"{name}_held.csv", False, scans)
    m = replay(build, out, held_trace)
    check(m["cep50_m"] <= bounds["cep50_m"] and m["cep95_m"] <= bounds["cep95_m"],
          f"unseen walk on the fitted venue: CEP50 {m['cep50_m']:.2f} m, CEP95 {m['cep95_m']:.2f} m "
          f"(at most {bounds['cep50_m']} and {bounds['cep95_m']})")
    if "floor" in bounds:
        check(m["floor_accuracy"] >= bounds["floor"], f"right floor {100 * m['floor_accuracy']:.1f}% (at least {100 * bounds['floor']:.0f}%)")

    # What the tool refuses.
    again = run([sys.executable, TOOL, "--venue", out, "--traces", *walks, "--out", tmp / "again.json"], ok=False)
    check(again.returncode == 1 and "already has a fitted radio map" in again.stderr, "a fitted venue is not fitted twice")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        # Bounds are about twice what these runs measure (docs/history.md, experiment 24).
        case(args.build, tmp, "one_floor", "office_floor.json", ["office_nominal"], [1, 2, 3, 4, 5], ("office_nominal", 6), False,
             {"fitted": 9, "median_m": 2.0, "cep50_m": 1.1, "cep95_m": 2.7})
        case(args.build, tmp, "one_floor_scans", "office_floor.json", ["office_nominal"], [1, 2, 3, 4, 5], ("office_nominal", 6),
             True, {"fitted": 9, "median_m": 2.0, "cep50_m": 1.1, "cep95_m": 2.7})
        case(args.build, tmp, "two_floors", "office_two_floor.json", ["office_stairs_up_down", "office_elevator"], [1, 2, 3],
             ("office_stairs_up_down", 7), False, {"fitted": 18, "median_m": 2.5, "cep50_m": 1.5, "cep95_m": 3.6, "floor": 0.9})

        # A walk without taps gives the fit nothing.
        bare = tmp / "bare.csv"
        bare.write_text("\n".join(line for line in (tmp / "one_floor_office_nominal_1.csv").read_text().splitlines()
                                  if not line.startswith("gt,")) + "\n")
        p = run([sys.executable, TOOL, "--venue", tmp / "one_floor_walls.json", "--traces", bare, "--out", tmp / "bare.json"], ok=False)
        check(p.returncode == 1 and "no transmitter could be fitted" in p.stderr and not (tmp / "bare.json").exists(),
              "walks without taps write no venue")
    print("FAILED" if check.failed else "all checks passed")
    return 1 if check.failed else 0


if __name__ == "__main__":
    sys.exit(main())
