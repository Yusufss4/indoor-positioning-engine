#!/usr/bin/env python3
"""UJI BLE database to ipe venues and standing-still traces.

Venues (`--venues`, default data/uji_ble/): one per zone and transmit power (`lib`, `geo_m4`,
`geo_m12`, `geo_m20`). Shelves and pillars become walls, the bounds are the extent of points,
beacons and obstacles plus 1 m (the zones have no mapped outer walls). Every beacon gets the zone's
shared curve (1 m power and exponent) and the obstacle loss fitted by study.py on the fit half of the
points, for the reference phone (A5).

Traces (`--out`, default datasets/uji_ble/out/): one per standing-still session (zone, power, phone,
campaign, point id): the session's fingerprints as BLE scans timed by their detection times, 50 Hz IMU
of a phone lying still (gravity on z, no rotation, a constant field), and a ground-truth row at every
scan. Sessions at fit-half points form manifest_tune.json (in-sample for the curve), the others
manifest_holdout.json.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

from common.trace import TraceWriter, write_manifests  # noqa: E402
from load import PHONES, Zone, load_zone  # noqa: E402
from study import fit_half  # noqa: E402

GRAVITY = 9.80665
IMU_HZ = 50.0
LEAD_S = 1.0  # IMU before the first scan


def venue_name(zone: str, power: int) -> str:
    return zone if zone == "lib" else f"{zone}_m{abs(power)}"


def make_venue(z: Zone, power: int, curve: dict) -> dict:
    pts = np.vstack([z.xy, z.beacons] + [r[:, :2] for r in z.obstacles])
    lo, hi = pts.min(axis=0) - 1.0, pts.max(axis=0) + 1.0
    return {
        "name": f"uji_ble_{venue_name(z.name, power)}",
        "bounds": [round(float(lo[0]), 3), round(float(lo[1]), 3), round(float(hi[0]), 3), round(float(hi[1]), 3)],
        "wall_attenuation_db": round(curve["obstacle_edge_db"], 2),
        "walls": [[round(float(v), 3) for v in w] for w in z.walls()],
        "beacons": [{"id": bid, "x": round(float(x), 3), "y": round(float(y), 3),
                     "tx_power": round(curve["tx_power_1m"], 2), "path_loss_exp": round(curve["path_loss_exp"], 3)}
                    for bid, (x, y) in zip(z.beacon_ids, z.beacons)],
    }


def write_session(path: Path, z: Zone, rows: np.ndarray, name: str) -> dict:
    tw = TraceWriter(name, f"uji_ble {z.name} {PHONES[z.name][int(z.phone[rows[0]])]} campaign {z.campaign[rows[0]]}")
    t_fp = []
    for i in rows:
        heard = z.t_ns[i][z.t_ns[i] > 0]
        t_fp.append(heard.max() if len(heard) else -1)
    known = [t for t in t_fp if t > 0]
    t0 = min(known) if known else 0
    # Fingerprints without any packet keep their place one second after the previous one.
    times, last = [], None
    for t in t_fp:
        s = (t - t0) / 1e9 + LEAD_S if t > 0 else (last + 1.0 if last is not None else LEAD_S)
        times.append(s)
        last = s
    x, y = z.xy[rows[0]]
    scans = 0
    for k, (i, t) in enumerate(zip(rows, times), start=1):
        heard = np.flatnonzero(~np.isnan(z.rss[i]))
        if len(heard):
            scans += 1
            for b in heard:
                tw.ble(t, z.beacon_ids[b], z.rss[i, b], scans)
        tw.gt(t, x, y, 0.0)
    end = times[-1] + 0.5
    for k in range(int(end * IMU_HZ) + 1):
        tw.imu(k / IMU_HZ, (0.0, 0.0, GRAVITY), (0.0, 0.0, 0.0), (0.0, 45.0, 0.0))
    tw.write(path)
    return {"fingerprints": int(len(rows)), "scans": scans, "x": float(x), "y": float(y)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--study", type=Path, default=HERE / "out" / "study.json", help="study.py output")
    ap.add_argument("--venues", type=Path, default=ROOT / "data" / "uji_ble")
    ap.add_argument("--out", type=Path, default=HERE / "out")
    args = ap.parse_args()

    study = {(r["zone"], r["power_dbm"]): r for r in json.loads(args.study.read_text())}
    args.venues.mkdir(parents=True, exist_ok=True)
    entries, tune, holdout = [], [], []
    for zname in ("lib", "geo"):
        z = load_zone(zname)
        half = fit_half(z.xy)
        for power in sorted({int(v) for v in z.power}, reverse=True):
            curve = study[(zname, power)]["curve_with_obstacles"]
            vname = venue_name(zname, power)
            venue_path = args.venues / f"{vname}.json"
            venue_path.write_text(json.dumps(make_venue(z, power, curve)) + "\n")
            sessions: dict[tuple, list[int]] = {}
            for i in np.flatnonzero(z.power == power):
                sessions.setdefault((int(z.phone[i]), int(z.campaign[i]), int(z.point[i])), []).append(i)
            out_dir = args.out / vname
            out_dir.mkdir(parents=True, exist_ok=True)
            for (phone, campaign, point), rows in sorted(sessions.items()):
                rows = np.array(sorted(rows, key=lambda i: z.sample[i]))
                if len({tuple(v) for v in z.xy[rows]}) != 1:
                    raise SystemExit(f"{zname}: session {(phone, campaign, point)} spans several points")
                ph = PHONES[zname][phone]
                name = f"uji_{vname}_{ph}_c{campaign}_p{point:03d}"
                trace = out_dir / f"{name}.csv"
                stats = write_session(trace, z, rows, name)
                e = {"name": name, "group": f"{vname}_{ph}", "venue": str(Path("..") / ".." / ".." / venue_path.relative_to(ROOT)),
                     "trace": str(trace.relative_to(args.out)), "phone": ph, **stats}
                entries.append(e)
                (tune if half[rows[0]] else holdout).append(e)
    write_manifests(args.out, entries, tune, holdout)
    print(f"wrote {len(entries)} sessions ({len(tune)} tune, {len(holdout)} holdout) and venues in {args.venues}")


if __name__ == "__main__":
    main()
