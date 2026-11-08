#!/usr/bin/env python3
"""Checks the engine's RSSI model against the UJI BLE database (standing-still fingerprints at
surveyed points, surveyed beacons, shelves and pillars mapped).

Per zone and transmit power, with every packet a sample:

* path loss: one log-distance curve (1 m power, exponent) for all beacons, and one per beacon;
* obstacle loss: the same fit with a loss per crossed obstacle edge, counted like the engine's
  wall-aware model counts walls (a shelf crossed through costs two edges);
* residual spread: standard deviation and a robust one (1.4826 MAD), and the share of packets more
  than 3 robust sigmas off (the engine's outlier mixture weight is 0.05);
* phone offset: mean residual per phone (library: three phones; the engine estimates an offset with
  prior sigma 8 dB);
* detection: share of fingerprints in which a beacon is heard, against its predicted RSSI
  (ipe-plan counts a beacon as heard above -85 dBm);
* time correlation: lag-1 autocorrelation of one beacon's residual over consecutive fingerprints at a
  point (the filter treats packets as independent).

Fits use the points of the fit half only (coordinate hash, the same split convert.py uses for the
venues); residual statistics are reported on both halves.

    python3 datasets/uji_ble/study.py [--out datasets/uji_ble/out] [--figures viz/output/uji_ble]
"""

from __future__ import annotations

import argparse
import json
import sys
import zlib
from pathlib import Path

import numpy as np
from scipy.optimize import least_squares

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

from common.geom import crossings  # noqa: E402
from load import PHONES, Zone, load_zone  # noqa: E402

MIN_DISTANCE_M = 0.5


def fit_half(xy: np.ndarray) -> np.ndarray:
    """True for points in the fit half (by a hash of the rounded coordinates)."""
    return np.array([zlib.crc32(f"{x:.2f},{y:.2f}".encode()) % 2 == 0 for x, y in xy])


def packets(z: Zone, sel: np.ndarray) -> dict:
    """Every heard (fingerprint, beacon) pair of the selected fingerprints, with its distance and the
    number of obstacle edges on the straight line."""
    walls = z.walls()
    rows = np.flatnonzero(sel)
    cache: dict[tuple, int] = {}
    out = {k: [] for k in ("fp", "beacon", "rssi", "d", "walls", "phone")}
    for i in rows:
        for b in np.flatnonzero(~np.isnan(z.rss[i])):
            key = (round(z.xy[i, 0], 3), round(z.xy[i, 1], 3), b)
            if key not in cache:
                cache[key] = crossings(z.xy[i], z.beacons[b], walls)
            out["fp"].append(i)
            out["beacon"].append(b)
            out["rssi"].append(z.rss[i, b])
            out["d"].append(max(float(np.hypot(*(z.xy[i] - z.beacons[b]))), MIN_DISTANCE_M))
            out["walls"].append(cache[key])
            out["phone"].append(z.phone[i])
    return {k: np.array(v) for k, v in out.items()}


def fit_curve(p: dict, with_walls: bool, phones: list[int]) -> dict:
    """RSSI = P0 + offset(phone) - 10 n log10(d) - wall_db * walls. The first phone is the reference
    (offset 0). Soft-L1 loss, 4 dB scale, as the transmitter fits of the real-data converters."""
    ph_index = {ph: k for k, ph in enumerate(phones)}
    k_ph = np.array([ph_index[v] for v in p["phone"]])
    n_off = len(phones) - 1

    def model(x):
        off = np.concatenate([[0.0], x[3:3 + n_off]])
        wall = x[2] if with_walls else 0.0
        return x[0] + off[k_ph] - 10 * x[1] * np.log10(p["d"]) - wall * p["walls"]

    x0 = np.concatenate([[-60.0, 2.0, 2.0], np.zeros(n_off)])
    lo = np.concatenate([[-110.0, 0.5, 0.0], np.full(n_off, -30.0)])
    hi = np.concatenate([[0.0, 6.0, 30.0], np.full(n_off, 30.0)])
    sol = least_squares(lambda x: model(x) - p["rssi"], x0, bounds=(lo, hi), loss="soft_l1", f_scale=4.0)
    res = {"tx_power_1m": float(sol.x[0]), "path_loss_exp": float(sol.x[1])}
    if with_walls:
        res["obstacle_edge_db"] = float(sol.x[2])
    if n_off:
        res["phone_offset_db"] = {PHONES_NAME[ph]: float(v) for ph, v in zip(phones, np.concatenate([[0.0], sol.x[3:3 + n_off]]))}
    res["_x"] = sol.x
    res["_model"] = model
    return res


PHONES_NAME: dict[int, str] = {}


def spread(r: np.ndarray) -> dict:
    mad = 1.4826 * float(np.median(np.abs(r - np.median(r))))
    return {"packets": int(len(r)), "mean_db": float(np.mean(r)), "std_db": float(np.std(r)), "robust_sigma_db": mad,
            "beyond_3_robust_sigma": float(np.mean(np.abs(r - np.median(r)) > 3 * mad))}


def detection_curve(z: Zone, sel: np.ndarray, fit: dict, phones: list[int]) -> dict:
    """Share of fingerprints hearing a beacon, binned by the beacon's predicted RSSI (fitted curve,
    obstacles included, reference phone)."""
    walls = z.walls()
    pred, heard = [], []
    for i in np.flatnonzero(sel):
        for b in range(len(z.beacons)):
            d = max(float(np.hypot(*(z.xy[i] - z.beacons[b]))), MIN_DISTANCE_M)
            w = crossings(z.xy[i], z.beacons[b], walls)
            pred.append(fit["tx_power_1m"] - 10 * fit["path_loss_exp"] * np.log10(d) - fit.get("obstacle_edge_db", 0.0) * w)
            heard.append(not np.isnan(z.rss[i, b]))
    pred, heard = np.array(pred), np.array(heard)
    edges = np.arange(-110, -40, 5)
    bins = []
    for a, b in zip(edges[:-1], edges[1:]):
        m = (pred >= a) & (pred < b)
        if m.sum() >= 20:
            bins.append({"pred_db": [int(a), int(b)], "pairs": int(m.sum()), "heard": float(heard[m].mean())})
    # Predicted RSSI where half the pairs are heard: logistic fit.
    sol = least_squares(lambda x: 1 / (1 + np.exp(-(pred - x[0]) / x[1])) - heard, [-85.0, 3.0],
                        bounds=([-130.0, 0.3], [-40.0, 30.0]))
    return {"bins": bins, "half_heard_at_db": float(sol.x[0]), "logistic_width_db": float(sol.x[1])}


def lag1(z: Zone, p: dict, resid: np.ndarray) -> float:
    """Mean lag-1 autocorrelation of residual series (one beacon, consecutive fingerprints of one
    session) with at least 5 packets."""
    key = {}
    for k, (i, b) in enumerate(zip(p["fp"], p["beacon"])):
        key.setdefault((z.point[i], z.phone[i], z.campaign[i], z.power[i], b), []).append((z.sample[i], resid[k]))
    acs = []
    for v in key.values():
        if len(v) < 5:
            continue
        r = np.array([x for _, x in sorted(v)])
        r = r - r.mean()
        den = float((r * r).sum())
        if den > 0:
            acs.append(float((r[:-1] * r[1:]).sum() / den))
    return float(np.mean(acs)) if acs else float("nan")


def study_zone(name: str, figures: Path | None) -> list[dict]:
    z = load_zone(name)
    PHONES_NAME.clear()
    PHONES_NAME.update(PHONES[name])
    half = fit_half(z.xy)
    out = []
    for power in sorted(np.unique(z.power), reverse=True):
        at = z.power == power
        phones = sorted(int(v) for v in np.unique(z.phone[at]))
        p_fit = packets(z, at & half)
        p_all = packets(z, at)
        plain = fit_curve(p_fit, False, phones)
        walled = fit_curve(p_fit, True, phones)
        r_plain = plain["_model"].__call__(plain["_x"]) - p_fit["rssi"]
        # Residuals of the walled fit on both halves.
        ph_index = {ph: k for k, ph in enumerate(phones)}
        off = np.concatenate([[0.0], walled["_x"][3:]])

        def resid(p):
            k_ph = np.array([ph_index[v] for v in p["phone"]])
            return p["rssi"] - (walled["_x"][0] + off[k_ph] - 10 * walled["_x"][1] * np.log10(p["d"]) - walled["_x"][2] * p["walls"])

        held = packets(z, at & ~half)
        r_fit, r_held = resid(p_fit), resid(held)
        r_all = resid(p_all)
        # Per-beacon curves (no obstacles), to see how much a shared curve costs.
        per_beacon = []
        for b in np.unique(p_fit["beacon"]):
            m = p_fit["beacon"] == b
            if m.sum() < 30:
                continue
            A = np.column_stack([np.ones(m.sum()), -10 * np.log10(p_fit["d"][m])])
            coef, *_ = np.linalg.lstsq(A, p_fit["rssi"][m], rcond=None)
            per_beacon.append({"beacon": z.beacon_ids[b], "tx_power_1m": float(coef[0]), "path_loss_exp": float(coef[1]),
                               "rms_db": float(np.sqrt(np.mean((A @ coef - p_fit["rssi"][m]) ** 2)))})
        row = {
            "zone": name, "power_dbm": int(power), "phones": [PHONES_NAME[p] for p in phones],
            "fingerprints": int(at.sum()), "points": int(len({tuple(v) for v in z.xy[at]})),
            "fit_points": int(len({tuple(v) for v in z.xy[at & half]})),
            "curve": {k: v for k, v in plain.items() if not k.startswith("_")},
            "curve_with_obstacles": {k: v for k, v in walled.items() if not k.startswith("_")},
            "residual_plain_fit_half": spread(-r_plain),
            "residual_fit_half": spread(r_fit),
            "residual_held_half": spread(r_held),
            "per_beacon": per_beacon,
            "per_beacon_exp_range": [float(min(b["path_loss_exp"] for b in per_beacon)), float(max(b["path_loss_exp"] for b in per_beacon))] if per_beacon else None,
            "per_beacon_rms_median_db": float(np.median([b["rms_db"] for b in per_beacon])) if per_beacon else None,
            "detection": detection_curve(z, at, walled, phones),
            "lag1_autocorrelation": lag1(z, p_all, r_all),
            "mean_heard_per_fingerprint": float(np.mean(np.sum(~np.isnan(z.rss[at]), axis=1))),
        }
        out.append(row)
        if figures:
            plot(figures / f"{name}_{abs(int(power))}dbm.png", row, p_fit, r_fit)
    return out


def plot(path: Path, row: dict, p: dict, r: np.ndarray):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(1, 3, figsize=(13, 3.6))
    c = row["curve_with_obstacles"]
    d = np.geomspace(0.5, max(p["d"].max(), 1.0), 100)
    ax[0].scatter(p["d"], p["rssi"], s=3, alpha=0.25, c=p["walls"], cmap="viridis")
    ax[0].plot(d, c["tx_power_1m"] - 10 * c["path_loss_exp"] * np.log10(d), "r-", label="fit, no obstacle")
    ax[0].set_xscale("log")
    ax[0].set_xlabel("distance [m]")
    ax[0].set_ylabel("RSSI [dBm]")
    ax[0].legend(fontsize=8)
    ax[1].hist(r, bins=60, density=True, color="0.5")
    s = row["residual_fit_half"]["robust_sigma_db"]
    x = np.linspace(-30, 30, 200)
    ax[1].plot(x, np.exp(-0.5 * (x / s) ** 2) / (s * np.sqrt(2 * np.pi)), "r-", label=f"N(0, {s:.1f} dB)")
    ax[1].set_xlabel("residual [dB]")
    ax[1].legend(fontsize=8)
    det = row["detection"]["bins"]
    ax[2].plot([np.mean(b["pred_db"]) for b in det], [b["heard"] for b in det], "o-")
    ax[2].axvline(-85, color="r", ls="--", lw=1, label="ipe-plan -85 dBm")
    ax[2].set_xlabel("predicted RSSI [dBm]")
    ax[2].set_ylabel("share heard")
    ax[2].legend(fontsize=8)
    fig.suptitle(f"UJI BLE {row['zone']}, {row['power_dbm']} dBm")
    fig.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=110)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=HERE / "out")
    ap.add_argument("--figures", type=Path, help="write one PNG per zone and power here")
    args = ap.parse_args()
    rows = study_zone("lib", args.figures) + study_zone("geo", args.figures)
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "study.json").write_text(json.dumps(rows, indent=1))
    print("| Zone | Power | Phones | P0 [dBm] | n | Obstacle edge [dB] | sigma (robust) [dB] | sigma held-out | >3 sigma | "
          "half heard at [dBm] | lag-1 r | per-beacon n |")
    print("|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---|")
    for r in rows:
        c = r["curve_with_obstacles"]
        rng = r["per_beacon_exp_range"]
        print(f"| {r['zone']} | {r['power_dbm']} | {', '.join(r['phones'])} | {c['tx_power_1m']:.1f} | {c['path_loss_exp']:.2f} | "
              f"{c['obstacle_edge_db']:.1f} | {r['residual_fit_half']['std_db']:.1f} ({r['residual_fit_half']['robust_sigma_db']:.1f}) | "
              f"{r['residual_held_half']['std_db']:.1f} | {r['residual_held_half']['beyond_3_robust_sigma']:.3f} | "
              f"{r['detection']['half_heard_at_db']:.1f} | {r['lag1_autocorrelation']:.2f} | "
              f"{rng[0]:.1f}-{rng[1]:.1f} |")
        if "phone_offset_db" in c:
            print("|  | phone offsets: " + ", ".join(f"{k} {v:+.1f} dB" for k, v in c["phone_offset_db"].items()) + " |")


if __name__ == "__main__":
    main()
