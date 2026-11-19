#!/usr/bin/env python3
"""Fits a venue's radio map from walks recorded in it: the step between a map and a position.

    fit_radio_map.py --venue walls.json --traces walk1.csv walk2.csv ... --out venue.json

The venue (docs/formats.md) holds the map: walls, and for several floors the levels and connectors. The
walks are traces (docs/formats.md) with radio rows (`ble`) and the true position at some moments (`gt`
rows, for example where the walker tapped the map). Nothing here depends on a data set.

* A reading is placed on the straight line between the two `gt` rows around it, when both are on the
  same floor. A walk therefore needs a tap at every turn. Readings before the first tap, after the
  last and across a floor change are not used.
* Every transmitter heard often enough, from places far enough apart, gets a position, a power at 1 m
  and a path-loss exponent (datasets/common/radiomap.py). In a venue with several floors it is placed
  on the floor where it is heard strongest, and the loss per slab comes from the other floors.
* Readings that arrived in scans (rows with the scan column) become the venue's fingerprints: every
  scan with its floor and position. Transmitters the fit rejected stay as fingerprint dimensions.

The result is the input venue with `beacons`, `floor_fingerprints` and `"radio_map": "fitted"`, which is
what the engine chooses its settings from. Beacons the input venue already has are surveyed: they are
kept as they are and not fitted.

--cross-validate fits without some of the walks and replays those through the engine, so every walk is
scored on a radio map it had no part in: the accuracy to expect in this building, from these walks.
"""

from __future__ import annotations

import argparse
import copy
import json
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from common.fingerprints import drop_rare, fingerprint_table, unmodelled_beacons  # noqa: E402
from common.radiomap import fit_levels  # noqa: E402
from common.trace import RadioWalk, interp_same_level, read_walk  # noqa: E402


def venue_levels(venue: dict) -> tuple[list[int], dict[int, float]]:
    """The venue's ordinals in level-index order, and the elevation [m] of each (docs/formats.md)."""
    if "levels" not in venue:
        return [0], {0: 0.0}
    levels = sorted(venue["levels"], key=lambda lv: lv["ordinal"])
    return ([lv["ordinal"] for lv in levels],
            {lv["ordinal"]: float(lv.get("elevation_m", lv["ordinal"] * lv.get("height_m", 4.0))) for lv in levels})


def placed_readings(walk: RadioWalk, ordinals: list[int]):
    """(reading, x, y, ordinal) for the walk's readings that lie between two taps on one floor."""
    if not walk.readings:
        return []
    if len(walk.gt) and not (0 <= walk.gt[:, 3].min() and walk.gt[:, 3].max() < len(ordinals)):
        raise ValueError(f"{walk.path}: a gt row is on level index {int(walk.gt[:, 3].max())}, "
                         f"and the venue has {len(ordinals)} level(s)")
    xy, lv, ok = interp_same_level(walk.gt, np.array([r[0] for r in walk.readings]))
    return [(r, float(p[0]), float(p[1]), ordinals[int(k)]) for r, p, k, good in zip(walk.readings, xy, lv, ok) if good]


def radio_samples(walks: list[RadioWalk], ordinals: list[int]) -> tuple[dict[str, list], int]:
    """{id: [(x, y, ordinal, dBm), ...]} over all walks, and how many readings had no position."""
    samples: dict[str, list] = {}
    dropped = 0
    for walk in walks:
        placed = placed_readings(walk, ordinals)
        dropped += len(walk.readings) - len(placed)
        for (_, tid, rssi, _), x, y, o in placed:
            samples.setdefault(tid, []).append((x, y, o, rssi))
    return samples, dropped


def training_scans(walks: list[RadioWalk], ordinals: list[int]) -> list[tuple[int, float, float, dict[str, float]]]:
    """(ordinal, x, y, {id: dBm}) per scan; a transmitter heard twice in a scan keeps its strongest reading."""
    out = []
    for walk in walks:
        by_scan: dict[int, tuple[int, float, float, dict[str, float]]] = {}
        for (_, tid, rssi, scan), x, y, o in placed_readings(walk, ordinals):
            if scan == 0:
                continue
            row = by_scan.setdefault(scan, (o, x, y, {}))
            row[3][tid] = max(row[3].get(tid, -200.0), rssi)
        out += [by_scan[k] for k in sorted(by_scan)]
    return out


def fit_venue(venue: dict, walks: list[RadioWalk], args) -> tuple[dict, dict]:
    """The venue with a radio map fitted from the walks, and a report of what went into it."""
    if venue.get("radio_map", "surveyed") == "fitted":
        raise ValueError("the venue already has a fitted radio map; start from the venue without it")
    ordinals, elev = venue_levels(venue)
    surveyed = {b["id"] for b in venue.get("beacons", [])}
    samples, dropped = radio_samples(walks, ordinals)
    heard = len(samples)
    samples = {tid: rows for tid, rows in samples.items() if tid not in surveyed}
    scans = [] if args.no_fingerprints else training_scans(walks, ordinals)
    in_scans = {tid for *_, rssi in scans for tid in rssi}

    txs, floor_loss, n_floor = fit_levels(samples, tuple(venue["bounds"]), elev, {}, args.min_samples, args.min_spread,
                                          args.max_rms, prefer=in_scans)
    several = "levels" in venue
    out = copy.deepcopy(venue)
    out["radio_map"] = "fitted"
    if several and n_floor:
        out["floor_attenuation_db"] = round(floor_loss, 2)
    beacons = [{"id": t.id, "x": round(t.x, 3), "y": round(t.y, 3), "level": t.level, "tx_power": round(t.tx_power, 2),
                "path_loss_exp": round(t.path_loss_exp, 3)} for t in txs]
    known = surveyed | {t.id for t in txs}
    extra = []
    if scans:
        extra = unmodelled_beacons(scans, known, args.min_fingerprint_scans)
        known |= {b["id"] for b in extra}
        scans = drop_rare(scans, known)
        out["floor_fingerprints"] = fingerprint_table(scans)
    if not several:
        for b in beacons + extra:
            del b["level"]
    out["beacons"] = list(venue.get("beacons", [])) + beacons + extra

    report = {
        "walks": len(walks),
        "walks_without_two_taps": sum(len(w.gt) < 2 for w in walks),
        "readings": sum(len(w.readings) for w in walks),
        "readings_without_position": dropped,
        "transmitters_heard": heard,
        "surveyed_kept": len(surveyed),
        "fitted": len(txs),
        "fingerprint_only": len(extra),
        "fit_rms_db_median": float(np.median([t.rms_db for t in txs])) if txs else None,
        "scans": len(scans),
        "floor_attenuation_db": round(floor_loss, 2) if several and n_floor else None,
    }
    return out, report


def print_report(report: dict, file=sys.stdout) -> None:
    r = report
    used = r["readings"] - r["readings_without_position"]
    print(f"{r['walks']} walks, {r['readings']} readings, {used} of them between two taps on one floor", file=file)
    if r["walks_without_two_taps"]:
        print(f"  {r['walks_without_two_taps']} walk(s) have fewer than two gt rows and add nothing", file=file)
    print(f"{r['transmitters_heard']} transmitters heard: {r['fitted']} fitted"
          + (f" (median fit error {r['fit_rms_db_median']:.1f} dB)" if r["fitted"] else "")
          + f", {r['fingerprint_only']} kept for fingerprints only"
          + (f", {r['surveyed_kept']} surveyed and kept" if r["surveyed_kept"] else ""), file=file)
    print(f"{r['scans']} scans in the fingerprint table", file=file)
    if r["floor_attenuation_db"] is not None:
        print(f"loss per floor slab: {r['floor_attenuation_db']:.1f} dB", file=file)


def cross_validate(venue: dict, walks: list[RadioWalk], args) -> list[dict]:
    """Every walk replayed on a radio map fitted without it (and without the other walks of its fold)."""
    replay = args.build / "tools" / "ipe-replay"
    if not replay.exists():
        raise FileNotFoundError(f"{replay} not found: build the tools, or pass --build")
    folds = min(args.folds, len(walks))
    if folds < 2:
        raise ValueError("cross-validation needs at least two walks")
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for k in range(folds):
            train = [w for i, w in enumerate(walks) if i % folds != k]
            fitted, _ = fit_venue(venue, train, args)
            venue_path = Path(tmp) / f"fold{k}.json"
            venue_path.write_text(json.dumps(fitted))
            for walk in (w for i, w in enumerate(walks) if i % folds == k):
                if walk.imu_rows == 0 or len(walk.gt) == 0:
                    print(f"  {walk.path.name}: no IMU rows or no gt rows, not replayed")
                    continue
                run = subprocess.run([str(replay), "--venue", str(venue_path), "--trace", str(walk.path), "--warmup",
                                      str(args.warmup)], capture_output=True, text=True)
                if run.returncode != 0:
                    print(f"  {walk.path.name}: replay failed: {run.stderr.strip()}")
                    continue
                m = json.loads(run.stdout)
                rows.append({"walk": walk.path.name, "cep50_m": m["cep50_m"], "cep95_m": m["cep95_m"],
                             "floor_accuracy": m["floor_accuracy"], "availability": m["availability"]})
                print(f"  {walk.path.name}: CEP50 {m['cep50_m']:.2f} m, CEP95 {m['cep95_m']:.2f} m"
                      + (f", right floor {100 * m['floor_accuracy']:.0f}%" if "levels" in venue else ""))
    if rows:
        print(f"cross-validation over {len(rows)} walks in {folds} folds: median CEP50 "
              f"{statistics.median(r['cep50_m'] for r in rows):.2f} m, median CEP95 "
              f"{statistics.median(r['cep95_m'] for r in rows):.2f} m")
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--venue", type=Path, required=True, help="venue JSON with the map")
    ap.add_argument("--traces", type=Path, nargs="+", required=True, help="recorded walks with gt rows")
    ap.add_argument("--out", type=Path, required=True, help="the venue with the radio map")
    ap.add_argument("--no-fingerprints", action="store_true", help="fit transmitters only")
    ap.add_argument("--min-samples", type=int, default=30, help="readings a transmitter needs to be fitted")
    ap.add_argument("--min-spread", type=float, default=3.0, help="metres; transmitters heard from one spot are not fitted")
    ap.add_argument("--max-rms", type=float, default=8.0, help="dB; transmitters fitting worse are not fitted")
    ap.add_argument("--min-fingerprint-scans", type=int, default=10,
                    help="scans a transmitter without a fit must appear in to stay a fingerprint dimension")
    ap.add_argument("--report", type=Path, help="also write the report as JSON")
    ap.add_argument("--cross-validate", action="store_true",
                    help="also replay every walk on a radio map fitted without it and print its error")
    ap.add_argument("--folds", type=int, default=5, help="with --cross-validate: groups the walks are left out in")
    ap.add_argument("--warmup", type=float, default=5.0, help="with --cross-validate: seconds not scored at the start")
    ap.add_argument("--build", type=Path, default=ROOT / "build", help="with --cross-validate: the build with ipe-replay")
    args = ap.parse_args()

    try:
        venue = json.loads(args.venue.read_text())
        walks = [read_walk(p) for p in args.traces]
        fitted, report = fit_venue(venue, walks, args)
        print_report(report)
        if report["fitted"] + report["fingerprint_only"] == 0:
            print("error: no transmitter could be fitted; the venue was not written", file=sys.stderr)
            return 1
        args.out.write_text(json.dumps(fitted) + "\n")
        print(f"wrote {args.out}")
        if args.cross_validate:
            report["cross_validation"] = cross_validate(venue, walks, args)
        if args.report:
            args.report.write_text(json.dumps(report, indent=1) + "\n")
    except (ValueError, OSError, json.JSONDecodeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
