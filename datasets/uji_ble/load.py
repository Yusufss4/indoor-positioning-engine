"""Reader for the UJI BLE RSS database (doi:10.5281/zenodo.1618692).

Two zones of Universitat Jaume I: `lib` (library, among bookshelves; 24 beacons at -12 dBm, three
phones, two campaigns) and `geo` (Geotec lab; 22 beacons on a grid, one phone, three transmit powers).
Fingerprints are taken standing still at surveyed points. Row i of `<zone>_rss.csv`, `_tms.csv`,
`_crd.csv` and `_ids.csv` is one fingerprint: per-beacon RSSI (100 = not heard) and detection time
(Android nanoseconds, 0 = not heard), the point's coordinates and an 8-digit id
`phone power campaign point(3) sample(2)` (load/getFilterFor*.m). Beacon columns follow the order of
`dep/<zone>.csv`; `obs/<zone>.csv` lists the corners of shelves and pillars, one convex polygon per id.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
DEFAULT_DB = HERE / "raw" / "db" / "data"

POWER_DBM = {"geo": {1: -4, 2: -12, 3: -20}, "lib": {2: -12}}
PHONES = {"geo": {1: "A5"}, "lib": {1: "A5", 2: "BQ", 3: "S6"}}


@dataclass
class Zone:
    name: str
    beacon_ids: list[str]
    beacons: np.ndarray  # B x 2, metres
    obstacles: list[np.ndarray]  # closed rings (first point repeated)
    obstacle_types: list[int]
    rss: np.ndarray  # N x B, NaN when not heard
    t_ns: np.ndarray  # N x B, 0 when not heard
    xy: np.ndarray  # N x 2
    phone: np.ndarray
    power: np.ndarray  # dBm
    campaign: np.ndarray
    point: np.ndarray
    sample: np.ndarray

    def walls(self) -> np.ndarray:
        """Obstacle outlines as wall segments [x1, y1, x2, y2]."""
        segs = [[*a, *b] for ring in self.obstacles for a, b in zip(ring[:-1], ring[1:])]
        return np.array(segs) if segs else np.zeros((0, 4))


def _convex_ring(pts: np.ndarray) -> np.ndarray:
    from scipy.spatial import ConvexHull

    hull = pts[ConvexHull(pts).vertices]
    return np.vstack([hull, hull[:1]])


def load_zone(name: str, db: Path = DEFAULT_DB) -> Zone:
    dep = np.genfromtxt(db / "dep" / f"{name}.csv", delimiter=",", names=True, dtype=None, encoding="utf-8")
    ids = [str(v) if name == "geo" else f"L{int(v)}" for v in dep["id"]]
    beacons = np.column_stack([dep["x"].astype(float), dep["y"].astype(float)])
    obs = np.loadtxt(db / "obs" / f"{name}.csv", delimiter=",", skiprows=1)
    rings, types = [], []
    for oid in np.unique(obs[:, 0]):
        sel = obs[:, 0] == oid
        rings.append(_convex_ring(obs[sel, 1:3]))
        types.append(int(obs[sel, 3][0]))
    rss = np.loadtxt(db / "rss" / f"{name}_rss.csv", delimiter=",")
    rss[rss == 100] = np.nan
    t_ns = np.loadtxt(db / "rss" / f"{name}_tms.csv", delimiter=",", dtype=np.int64)
    xy = np.loadtxt(db / "rss" / f"{name}_crd.csv", delimiter=",")
    code = np.loadtxt(db / "rss" / f"{name}_ids.csv", dtype=np.int64)
    if rss.shape[1] != len(ids):
        raise SystemExit(f"{name}: {rss.shape[1]} RSS columns but {len(ids)} beacons")
    power_code = (code // 10 ** 6) % 10
    return Zone(name, ids, beacons, rings, types, rss, t_ns, xy,
                phone=code // 10 ** 7, power=np.array([POWER_DBM[name][int(c)] for c in power_code]),
                campaign=(code // 10 ** 5) % 10, point=(code // 10 ** 2) % 1000, sample=code % 100)
