#!/usr/bin/env python3
"""Builds a submission for the Kaggle "Indoor Location & Navigation" competition from the engine.

For each of the test walks:

1. the site is read from `sample_submission.csv`; the floor is not given, so it is a k-NN vote of the
   walk's Wi-Fi scans over the site's training scans (the fingerprints `convert.py --fingerprints`
   stored in each floor's venue);
2. the walk is written as an ipe trace with a truth row (at 0, 0) at every timestamp the submission
   asks for, and replayed through that floor's venue;
3. the estimates at those rows become `site_path_timestamp,floor,x,y`. Rows before the first fix take
   the first fix: the competition is scored offline, so looking ahead is within its rules. `--two-pass`
   and `--snap` are further offline steps (datasets/common/offline.py).

    python3 datasets/kaggle/submit.py --converted datasets/kaggle/out_fp --config data/configs/msil_wifi.json \\
        --out datasets/kaggle/out_fp/submission.csv

`--check-floors` instead predicts the floor of the converted held-out training walks, whose floor is
known, and prints the accuracy. Nothing here uploads anything: see `kaggle competitions submit`.
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import tempfile
from collections import Counter
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent / "msil"))

import convert as msil  # noqa: E402
from common import offline  # noqa: E402

FILL_DBM = -100.0


def floor_number(name: str) -> int:
    """The competition's floor integer: F1 / 1F = 0, F2 = 1, B1 = -1."""
    n = name.upper()
    digits = "".join(c for c in n if c.isdigit())
    if not digits:
        raise ValueError(f"floor name without a number: {name}")
    return -int(digits) if "B" in n else int(digits) - 1


class SiteFloors:
    """k-NN floor vote over one site's training scans, compared over a scan's strongest readings (as
    the engine's FingerprintMap does)."""

    def __init__(self, site_dir: Path, k: int = 5, strongest: int = 20):
        self.k, self.strongest = k, strongest
        self.floors = sorted(p.name for p in site_dir.iterdir() if (p / "venue.json").exists())
        ids: dict[str, int] = {}
        rows, labels = [], []
        for fi, name in enumerate(self.floors):
            table = json.loads((site_dir / name / "venue.json").read_text()).get("floor_fingerprints")
            if not table:
                continue
            local = [ids.setdefault(i, len(ids)) for i in table["ids"]]
            for scan in table["scans"]:
                rows.append([(local[scan[j]], scan[j + 1]) for j in range(1, len(scan) - 1, 2)])
                labels.append(fi)
        self.ids = ids
        self.labels = np.array(labels)
        self.matrix = np.full((len(rows), len(ids)), FILL_DBM, dtype=np.float32)
        for i, row in enumerate(rows):
            for j, r in row:
                self.matrix[i, j] = r

    def votes(self, scans: list[dict[str, float]]) -> Counter:
        votes: Counter = Counter()
        for scan in scans:
            known = sorted(((r, self.ids[a]) for a, r in scan.items() if a in self.ids), reverse=True)[: self.strongest]
            if len(known) < 3 or not len(self.labels):
                continue
            cols = [j for _, j in known]
            q = np.array([r for r, _ in known], dtype=np.float32)
            d = ((self.matrix[:, cols] - q) ** 2).sum(axis=1)
            for i in np.argsort(d)[: self.k]:
                votes[self.floors[self.labels[i]]] += 1
        return votes

    def predict(self, scans: list[dict[str, float]]) -> str | None:
        votes = self.votes(scans)
        return votes.most_common(1)[0][0] if votes else None


def site_offsets(converted: Path) -> dict[str, float]:
    """Heading offset per site [rad], as fitted by the converter."""
    return {r["site"]: math.radians(r["heading_offset_deg"]) for r in json.loads((converted / "report.json").read_text())}


def prepare_walk(job) -> dict:
    """Parses one test walk, predicts its floor and writes its trace. Runs in a worker process."""
    path_file, site, stamps_ms, converted, floors_from, out_dir, offset = job
    pd = msil.parse_path(Path(path_file))
    floors = _site_floors(Path(floors_from) / site)
    floor = floors.predict([rssi for _, rssi in msil.wifi_scans(pd)])
    voted = floor is not None
    if floor is None:
        # No usable scan: the site's floor with the most training scans.
        floor = floors.floors[int(np.bincount(floors.labels).argmax())] if len(floors.labels) else floors.floors[0]
    row = {"path": pd.path_id, "site": site, "floor": floor, "voted": voted, "trace": None,
           "scans": len(msil.wifi_scans(pd))}
    if len(pd.imu_t) < 100:
        return row
    # Truth rows carry no truth here, only the times the submission asks for.
    pd.waypoints = np.array([(t / 1000.0, 0.0, 0.0) for t in stamps_ms])
    venue = json.loads((Path(converted) / site / floor / "venue.json").read_text())
    known = {b["id"] for b in venue["beacons"]}
    trace = Path(out_dir) / f"{pd.path_id}.csv"
    msil.write_trace(trace, pd, f"test_{pd.path_id}", offset, known, wifi=True, scans=True)
    row["trace"] = str(trace)
    row["t0"] = float(pd.imu_t[0])
    return row


_FLOORS: dict[str, SiteFloors] = {}


def _site_floors(site_dir: Path) -> SiteFloors:
    key = str(site_dir)
    if key not in _FLOORS:
        _FLOORS[key] = SiteFloors(site_dir)
    return _FLOORS[key]


def run_engine(venue: Path, trace: Path, out: Path, args) -> list[dict]:
    cmd = [str(args.build / "tools" / "ipe-replay"), "--venue", str(venue), "--trace", str(trace), "--warmup", "0",
           "--out", str(out)]
    if args.config:
        cmd += ["--config", str(args.config)]
    if args.set:
        cmd += ["--set", args.set]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    return offline.read_estimates(out)


def replay(walk: dict, converted: Path, args, tmp: Path) -> list[dict]:
    """The estimate at each of the walk's truth rows, in time order, after the offline steps asked for."""
    floor_dir = converted / walk["site"] / walk["floor"]
    trace = Path(walk["trace"])
    rows = run_engine(floor_dir / "venue.json", trace, tmp / f"{walk['path']}.csv", args)
    if args.two_pass:
        reversed_trace = tmp / f"{walk['path']}_reversed.csv"
        end = offline.reverse_trace(trace, reversed_trace)
        back = run_engine(floor_dir / "venue.json", reversed_trace, tmp / f"{walk['path']}_back.csv", args)
        rows = offline.fuse(rows, back, end, args.fuse_floor)
    offline.fill_gaps(rows)
    if args.snap > 0.0 and (floor_dir / "waypoints.json").exists():
        offline.snap(rows, json.loads((floor_dir / "waypoints.json").read_text()), args.snap)
    return rows


def check_floors(converted: Path, jobs: int):
    """Floor accuracy of the vote on the converted held-out walks (their folder is their floor)."""
    entries = json.loads((converted / "manifest.json").read_text())["traces"]
    right = total = 0
    wrong: Counter = Counter()
    for e in entries:
        site, floor = Path(e["venue"]).parts[:2]
        scans: dict[str, dict[str, float]] = {}
        for line in (converted / e["trace"]).read_text().splitlines():
            f = line.split(",")
            if f[0] == "ble" and len(f) >= 5:
                scans.setdefault(f[4], {})[f[2]] = float(f[3])
        got = _site_floors(converted / site).predict(list(scans.values()))
        total += 1
        right += got == floor
        if got != floor:
            wrong[(site[-6:], floor, got)] += 1
    print(f"floor right for {right} of {total} held-out walks ({right / max(total, 1):.1%})")
    for (site, floor, got), n in wrong.most_common(10):
        print(f"  site ..{site}: {floor} read as {got} x{n}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw", type=Path, default=HERE / "raw", help="the unpacked competition data")
    ap.add_argument("--converted", type=Path, default=HERE / "out_fp",
                    help="output of datasets/msil/convert.py --kaggle ... --fingerprints")
    ap.add_argument("--floors-from", type=Path,
                    help="converted folder whose venues carry the fingerprints for the floor vote (default: --converted); "
                         "lets venues converted without fingerprints be replayed on the same floors")
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    ap.add_argument("--config", type=Path)
    ap.add_argument("--set")
    ap.add_argument("--out", type=Path, help="submission CSV to write")
    ap.add_argument("--two-pass", action="store_true",
                    help="replay each walk forwards and backwards and combine the two (datasets/common/offline.py)")
    ap.add_argument("--fuse-floor", type=float, default=3.0)
    ap.add_argument("--snap", type=float, default=0.0,
                    help="move each answer onto the nearest surveyed training waypoint within this many metres")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--check-floors", action="store_true")
    args = ap.parse_args()

    floors_from = args.floors_from or args.converted
    if args.check_floors:
        check_floors(floors_from, args.jobs)
        return
    if not args.out:
        ap.error("--out is needed")

    # The rows to predict, per walk, in the sample's order.
    order, stamps = [], {}
    for line in (args.raw / "sample_submission.csv").read_text().splitlines()[1:]:
        key = line.split(",", 1)[0]
        site, path, t = key.split("_")
        order.append((site, path, t))
        stamps.setdefault((site, path), []).append(int(t))

    offsets = site_offsets(args.converted)
    test_dir = args.converted / "test"
    test_dir.mkdir(parents=True, exist_ok=True)
    jobs = [(str(args.raw / "test" / f"{path}.txt"), site, ts, str(args.converted), str(floors_from), str(test_dir),
             offsets[site])
            for (site, path), ts in stamps.items()]
    with ProcessPoolExecutor(args.jobs) as pool:
        walks = list(pool.map(prepare_walk, jobs, chunksize=4))
    (test_dir / "walks.json").write_text(json.dumps(walks, indent=1))
    print(f"{len(walks)} test walks; floors predicted: {dict(sorted(Counter(w['floor'] for w in walks).items()))}; "
          f"voted: {sum(w['voted'] for w in walks)}; without IMU: {sum(w['trace'] is None for w in walks)}")
    if not any(w["voted"] for w in walks):
        raise SystemExit(f"no floor could be voted: are there fingerprints under {floors_from}?")

    answers: dict[tuple[str, str, int], tuple[int, float, float]] = {}
    no_fix_walks = 0
    with tempfile.TemporaryDirectory() as tmp, ThreadPoolExecutor(args.jobs) as pool:
        replays = list(pool.map(lambda w: replay(w, args.converted, args, Path(tmp)) if w["trace"] else [], walks))
    for w, rows in zip(walks, replays):
        ts = stamps[(w["site"], w["path"])]
        venue = json.loads((args.converted / w["site"] / w["floor"] / "venue.json").read_text())
        centre = (venue["bounds"][2] / 2.0, venue["bounds"][3] / 2.0)
        # Truth rows come back in time order, which is the order of the walk's sorted timestamps.
        if len(rows) != len(ts) or not any(r["has_fix"] for r in rows):
            no_fix_walks += 1
            rows = [{"est_x": centre[0], "est_y": centre[1]} for _ in ts]
        for t, r in zip(sorted(ts), rows):
            answers[(w["site"], w["path"], t)] = (floor_number(w["floor"]), r["est_x"], r["est_y"])

    with open(args.out, "w") as f:
        f.write("site_path_timestamp,floor,x,y\n")
        for site, path, t in order:
            floor, x, y = answers[(site, path, int(t))]
            f.write(f"{site}_{path}_{t},{floor},{x:.3f},{y:.3f}\n")
    print(f"wrote {len(order)} rows to {args.out}; walks without any fix (answered with the floor's centre): {no_fix_walks}")


if __name__ == "__main__":
    main()
