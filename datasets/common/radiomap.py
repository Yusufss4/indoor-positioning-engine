"""Transmitter fitting: log-distance path loss (position, tx power, exponent) from labelled RSSI."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy.optimize import least_squares

MIN_DISTANCE_M = 0.5  # same clamp as RssiModelConfig::min_distance


def physical_ap(bssid: str) -> str:
    """One radio often advertises several SSIDs under BSSIDs that differ only in the first octet
    (locally administered addresses). Their readings are the same measurement, not independent
    evidence, so they are merged into one transmitter keyed by the last five octets."""
    return bssid[3:] if len(bssid) == 17 else bssid


def merge_virtual_aps(wifi: list, window_s: float = 0.05) -> list:
    """Keeps one reading (the strongest) per physical access point within `window_s`, tagged with
    the scan of the group's first reading. Rows are (t, ap, rssi, scan)."""
    by_ap: dict[str, list] = {}
    for t, ap, rssi, t_scan in wifi:
        by_ap.setdefault(ap, []).append((t, rssi, t_scan))
    out = []
    for ap, readings in by_ap.items():
        readings.sort()
        group_t, best, group_scan = readings[0]
        for t, rssi, t_scan in readings[1:]:
            if t - group_t < window_s:
                best = max(best, rssi)
                continue
            out.append((group_t, ap, best, group_scan))
            group_t, best, group_scan = t, rssi, t_scan
        out.append((group_t, ap, best, group_scan))
    return sorted(out)


@dataclass
class Transmitter:
    id: str
    x: float
    y: float
    tx_power: float
    path_loss_exp: float
    samples: int
    rms_db: float
    level: int = 0  # ordinal; single-floor converters leave it at 0


def path_loss(d: np.ndarray, tx_power: float, n: float) -> np.ndarray:
    return tx_power - 10.0 * n * np.log10(np.maximum(d, MIN_DISTANCE_M))


def fit_transmitter(tid, xy: np.ndarray, rssi: np.ndarray, box: tuple[float, float, float, float],
                    max_rms: float, known_xy: tuple[float, float] | None = None) -> Transmitter | None:
    """Least-squares fit (soft-L1, 4 dB scale) of position, tx power and exponent inside
    `box` = (min_x, min_y, max_x, max_y) plus a 10 m margin. With `known_xy` only tx power and the
    exponent are fitted. Rejected when the RMS exceeds `max_rms`, the position falls outside the box
    or the exponent sits on its bound."""
    x_min, y_min, x_max, y_max = box
    lo_n, hi_n = 1.2, 6.0
    if known_xy is not None:
        d = np.hypot(xy[:, 0] - known_xy[0], xy[:, 1] - known_xy[1])

        def residual_known(p):
            return path_loss(d, p[0], p[1]) - rssi

        sol = least_squares(residual_known, [rssi.max() + 6.0, 2.5], bounds=([-110.0, lo_n], [-10.0, hi_n]),
                            loss="soft_l1", f_scale=4.0)
        p0, n = sol.x
        rms = float(np.sqrt(np.mean(residual_known(sol.x) ** 2)))
        if rms > max_rms or n <= 1.21 or n >= 5.99:
            return None
        return Transmitter(tid, float(known_xy[0]), float(known_xy[1]), float(p0), float(n), len(rssi), rms)

    order = np.argsort(-rssi)[:5]
    x0 = [xy[order, 0].mean(), xy[order, 1].mean(), rssi.max() + 6.0, 2.5]

    def residual(p):
        d = np.maximum(np.hypot(xy[:, 0] - p[0], xy[:, 1] - p[1]), MIN_DISTANCE_M)
        return p[2] - 10.0 * p[3] * np.log10(d) - rssi

    margin = 10.0
    lo = [x_min - margin, y_min - margin, -110.0, lo_n]
    hi = [x_max + margin, y_max + margin, -10.0, hi_n]
    x0 = np.clip(x0, np.array(lo) + 1e-6, np.array(hi) - 1e-6)
    sol = least_squares(residual, x0, bounds=(lo, hi), loss="soft_l1", f_scale=4.0)
    bx, by, p0, n = sol.x
    rms = float(np.sqrt(np.mean(residual(sol.x) ** 2)))
    inside = x_min <= bx <= x_max and y_min <= by <= y_max
    at_bound = n <= 1.21 or n >= 5.99
    if rms > max_rms or not inside or at_bound:
        return None
    return Transmitter(tid, float(bx), float(by), float(p0), float(n), len(rssi), rms)


def fit_samples(samples: dict[str, list], box, min_samples: int, min_spread: float, max_rms: float,
                known: dict[str, tuple[float, float]] | None = None) -> list[Transmitter]:
    """Fits every transmitter in `samples` ({id: [(x, y, rssi), ...]}) with enough samples and
    spatial spread. Transmitters in `known` keep their surveyed position."""
    known = known or {}
    out = []
    for tid, rows in sorted(samples.items()):
        arr = np.array(rows)
        if len(arr) < min_samples:
            continue
        # A transmitter heard from a single spot cannot be located.
        if tid not in known and np.hypot(arr[:, 0].std(), arr[:, 1].std()) < min_spread:
            continue
        tx = fit_transmitter(tid, arr[:, :2], arr[:, 2], box, max_rms, known.get(tid))
        if tx:
            out.append(tx)
    return out


def fit_levels(samples: dict[str, list], box, elev: dict[int, float], known: dict[str, tuple[float, float, int]],
               min_samples: int, min_spread: float, max_rms: float, prefer: set[str] | None = None):
    """Fits a venue with several floors from `samples` ({id: [(x, y, ordinal, rssi), ...]}).

    Each transmitter is placed on the floor where it is heard strongest (median of its five
    strongest readings) and fitted on that floor's samples; one in `known` ({id: (x, y, ordinal)})
    keeps its surveyed place. The other floors' samples then give the loss per slab: median over
    transmitters of the median excess loss divided by the slabs between. `elev` is the elevation [m]
    per ordinal. The loss comes from the transmitters in `prefer` when any of them was fitted (fixed
    infrastructure: BLE devices can move between floors).

    Returns (transmitters, loss per slab [dB], transmitters the loss was taken from); the loss is
    15 dB when no transmitter is heard on another floor."""
    by_level: dict[int, dict[str, list]] = {}
    for tid, rows in samples.items():
        arr = np.array(rows)
        if tid in known:
            o = known[tid][2]
        else:
            best = {}
            for o in np.unique(arr[:, 2]).astype(int):
                r = np.sort(arr[arr[:, 2] == o, 3])[::-1][:5]
                if len(r) >= 3:
                    best[o] = float(np.median(r))
            if not best:
                continue
            o = max(best, key=best.get)
        o = int(o)
        by_level.setdefault(o, {})[tid] = [(x, y, r) for x, y, lv, r in rows if int(lv) == o]
    txs: list[Transmitter] = []
    for o, smp in sorted(by_level.items()):
        kn = {tid: known[tid][:2] for tid in smp if tid in known}
        for tx in fit_samples(smp, box, min_samples, min_spread, max_rms, kn):
            tx.level = o
            txs.append(tx)
    per_tx = []
    ref = [t for t in txs if t.id in (prefer or ())] or txs
    order = sorted(elev)
    for tx in ref:
        arr = np.array(samples[tx.id])
        other = arr[arr[:, 2].astype(int) != tx.level]
        if len(other) < 10:
            continue
        dz = np.array([elev[int(o)] - elev[tx.level] for o in other[:, 2]])
        d = np.sqrt((other[:, 0] - tx.x) ** 2 + (other[:, 1] - tx.y) ** 2 + dz ** 2)
        slabs = np.abs(np.array([order.index(int(o)) - order.index(tx.level) for o in other[:, 2]]))
        excess = (path_loss(d, tx.tx_power, tx.path_loss_exp) - other[:, 3]) / np.maximum(slabs, 1)
        per_tx.append(float(np.median(excess)))
    floor_loss = float(np.median(per_tx)) if per_tx else 15.0
    return txs, floor_loss, len(per_tx)
