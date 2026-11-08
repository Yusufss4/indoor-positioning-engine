#!/usr/bin/env python3
"""Converts IPIN competition Track 3 packages (smartphone, off-site) into ipe venues and traces.

Per site (sites.json; one site can draw on several yearly packages):

* the floor maps are georeferenced on their reference markers and turned into walls per floor
  (maps.py), corrected by edits/<site>.json, which also holds the stairs and lifts;
* training walks fit the map heading offset, the floor elevations (barometer at the landmarks of each
  floor) and a radio map: position, tx power and path-loss exponent per transmitter, on the floor it
  is heard best; surveyed beacons keep their position;
* validation walks (landmarks in the log) and scoring walks (landmarks in 03 Evaluation/GT) become
  ipe traces: earth-frame IMU in the engine's convention, barometer, BLE and Wi-Fi rows and one
  ground-truth row per landmark. Validation walks form the tune manifest, scoring walks the holdout.

See docs/ipin.md.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.append(str(HERE.parent / "osm"))

import maps  # noqa: E402
from common.geom import crossing_params, crossings, cut_gaps, fit_heading_offset, forward_heading_from_quat, point_in_rings, rotation_matrices, wrap  # noqa: E402
from common.radiomap import Transmitter, fit_samples, path_loss  # noqa: E402
from common.trace import TraceWriter, write_manifests  # noqa: E402
from logfile import Walk, parse, read_gt_csv  # noqa: E402
from plan_walk import Level as GridLevel  # noqa: E402

MAG_FIELD_UT = 45.0
GRAVITY = 9.80665


# --------------------------------------------------------------------------------------------
# Walks


def logfiles(pkg: Path, folder_prefix: str) -> list[Path]:
    dirs = sorted((pkg / "01 Logfiles").glob(folder_prefix + "*"))
    return sorted(p for d in dirs for p in d.glob("*.txt"))


def load_walks(cfg: dict, raw: Path) -> dict[str, list[Walk]]:
    """Training and validation walks of every package, repeated files dropped; scoring walks with
    their ground truth from 03 Evaluation/GT."""
    out = {"train": [], "tune": [], "holdout": []}
    seen = set()
    for pkg_name in cfg["packages"]:
        pkg = raw / pkg_name
        for split, prefix in (("train", "01"), ("tune", "02"), ("holdout", "03")):
            for p in logfiles(pkg, prefix):
                w = parse(p)
                if w.digest in seen:
                    continue
                seen.add(w.digest)
                if split == "holdout":
                    gt = pkg / "03 Evaluation" / "GT" / f"GT_{p.stem}.csv"
                    if not gt.exists():
                        print(f"  {p.name}: no ground truth, skipped")
                        continue
                    w.posi = read_gt_csv(gt)
                out[split].append(w)
    return out


def gt_local(w: Walk, frame: maps.Frame) -> np.ndarray:
    """Landmarks as (t, x, y, ordinal) in the site frame."""
    if len(w.posi) == 0:
        return np.zeros((0, 4))
    x, y = frame.georef.to_local(w.posi[:, 1], w.posi[:, 2])
    return np.column_stack([w.posi[:, 0], x, y, np.round(w.posi[:, 3])])


def same_level_runs(gt: np.ndarray) -> list[np.ndarray]:
    """Maximal runs of consecutive landmarks on one floor, as (t, x, y) rows."""
    runs, start = [], 0
    for i in range(1, len(gt) + 1):
        if i == len(gt) or gt[i, 3] != gt[start, 3]:
            if i - start >= 1:
                runs.append(gt[start:i, :3])
            start = i
    return runs


def interp_same_level(gt: np.ndarray, t: np.ndarray):
    """Position at times t by linear interpolation between consecutive landmarks on the same floor;
    mask is False elsewhere (before the first, after the last, or across a floor change)."""
    xy = np.full((len(t), 2), np.nan)
    lv = np.zeros(len(t), dtype=int)
    if len(gt) < 2:
        return xy, lv, np.zeros(len(t), dtype=bool)
    k = np.searchsorted(gt[:, 0], t, side="right") - 1
    ok = (k >= 0) & (k < len(gt) - 1)
    k = np.clip(k, 0, len(gt) - 2)
    ok &= gt[k, 3] == gt[k + 1, 3]
    t0, t1 = gt[k, 0], gt[k + 1, 0]
    u = np.clip((t - t0) / np.maximum(t1 - t0, 1e-9), 0, 1)
    xy[:, 0] = gt[k, 1] + u * (gt[k + 1, 1] - gt[k, 1])
    xy[:, 1] = gt[k, 2] + u * (gt[k + 1, 2] - gt[k, 2])
    lv[:] = gt[k, 3]
    return xy, lv, ok


# --------------------------------------------------------------------------------------------
# Site maps


def marker_label_boxes(img_path: Path, frame: maps.Frame, marker_px, extra_xy=()) -> list[tuple[float, float, float, float]]:
    """Pixel boxes around the marker dots and their labels (drawn up and to the right of the dot).
    `extra_xy` adds dots at known local positions (surveyed beacons), which the colour search misses
    on some floors."""
    from PIL import Image

    rgb = np.asarray(Image.open(img_path).convert("RGB"))
    s = frame.scale
    boxes = []
    dots = [tuple(p) for p in maps.find_markers(rgb, *marker_px)]
    dots += [(x / s, frame.height_px - y / s) for x, y in extra_xy]
    for u, v in dots:
        boxes.append((u - 0.35 / s, v - 1.3 / s, u + 3.4 / s, v + 0.35 / s))
    return boxes


def footprint_from(masks: list[np.ndarray], points: np.ndarray, margin_m: float) -> list[list[float]]:
    """Convex hull of the walls of every floor and every landmark (the 2022 walks use outdoor patios
    and entrances), grown by `margin_m`."""
    from scipy.spatial import ConvexHull

    m = np.zeros_like(masks[0])
    for mk in masks:
        m |= mk
    rows, cols = np.nonzero(m)
    pts = np.vstack([np.column_stack([(cols + 0.5) * maps.CELL, (rows + 0.5) * maps.CELL]), points])
    hull = pts[ConvexHull(pts).vertices]
    c = hull.mean(axis=0)
    grown = c + (hull - c) * (1.0 + margin_m / np.maximum(np.linalg.norm(hull - c, axis=1, keepdims=True), 1e-9))
    return [[round(float(x), 3), round(float(y), 3)] for x, y in grown]


def cut_walked_doors(levels: dict, train: list[np.ndarray], width: float, min_walks: int):
    """Doorways the plans close with a line (entrances, double doors drawn shut): a gap `width` wide
    wherever straight lines between consecutive same-floor training landmarks cross a wall, if
    `min_walks` different walks cross it there. Training walks have a landmark at every turn
    (technical annex), so their straight lines are close to the path."""
    out, report = {}, {}
    for o, walls in levels.items():
        hits: dict[int, dict[int, list[float]]] = {}
        for k, gt in enumerate(train):
            for a, b in zip(gt[:-1], gt[1:]):
                if int(a[3]) != o or int(b[3]) != o:
                    continue
                idx, u = crossing_params(a[1:3], b[1:3], walls)
                for i, ui in zip(idx, u):
                    hits.setdefault(int(i), {}).setdefault(k, []).append(float(ui))
        cuts = {}
        for i, by_walk in hits.items():
            if len(by_walk) < min_walks:
                continue
            us = sorted(u for v in by_walk.values() for u in v)
            cuts[i] = [float(np.median(us))] if np.ptp(us) * np.hypot(*(walls[i, 2:] - walls[i, :2])) < width else us
        cut, centres = cut_gaps(walls, cuts, width)
        out[o] = cut
        report[o] = len(centres)
    return out, report


def build_levels(site: str, cfg: dict, raw: Path, frame: maps.Frame, edits: dict, gt_points: np.ndarray, out: Path,
                 beacons: dict):
    pkg = raw / cfg["packages"][0]
    grid = maps.Grid(*frame.size_m)
    ordinals = sorted(int(k) for k in cfg["maps"])
    connectors = edits.get("connectors", [])
    levels, masks = {}, {}
    door_counts: dict[int, int] = {}
    for o in ordinals:
        img = pkg / cfg["maps"][str(o)]
        e = edits.get("levels", {}).get(str(o), {})
        labels = marker_label_boxes(img, frame, cfg["marker_px"], [(x, y) for x, y, lv in beacons.values() if lv == o])
        if cfg["kind"] == "raster":
            mask = maps.raster_mask(img, frame, grid, erase_px=labels)
        else:
            segs, _ = maps.pdf_segments(img.with_suffix(".pdf"))
            label_polys = [[list(frame.to_local(u0, v0)), list(frame.to_local(u1, v0)), list(frame.to_local(u1, v1)),
                            list(frame.to_local(u0, v1))] for u0, v0, u1, v1 in labels]
            # Door swings are drawn as chains of short straight pieces; walls are longer.
            mask = maps.vector_mask(segs, frame, grid, min_len_m=cfg.get("min_segment_m", 0.2))
            mask = maps.erase_polygons(mask, label_polys)
            # Door swings: clear each swing's box (grown by 12 cm to take the wall line on its edge).
            g = 0.12 / frame.scale
            arcs = maps.pdf_door_arcs(segs, frame.scale)
            mask = maps.erase_polygons(mask, [[list(frame.to_local(u0 - g, v0 - g)), list(frame.to_local(u1 + g, v0 - g)),
                                               list(frame.to_local(u1 + g, v1 + g)), list(frame.to_local(u0 - g, v1 + g))]
                                              for u0, v0, u1, v1 in arcs])
            door_counts[o] = len(arcs)
        erase = list(e.get("erase", [])) + [c["area"] for c in connectors if o in c["levels"]]
        mask = maps.erase_polygons(mask, erase)
        walls = maps.vectorise(mask)
        add = np.array(e.get("add", []), dtype=float).reshape(-1, 4)
        walls = np.vstack([walls, add]) if len(add) else walls
        levels[o], masks[o] = walls, mask
    footprint = edits.get("footprint") or footprint_from(list(masks.values()), gt_points[:, 1:3], 1.5)
    if door_counts:
        print(f"  door swings opened per floor: {door_counts}")
    return ordinals, levels, masks, footprint


# --------------------------------------------------------------------------------------------
# Calibration from training walks


def hpa_to_height(p: np.ndarray, p_ref: float) -> np.ndarray:
    """Height above the p_ref level (international barometric formula)."""
    return 44330.0 * (1.0 - (np.asarray(p) / p_ref) ** 0.1903)


def fit_elevations(walks: list[tuple[Walk, np.ndarray]], ordinals: list[int]) -> tuple[dict, dict]:
    """Height of each floor above the lowest, from the pressure at landmarks: within one walk, the
    median height at each floor's landmarks relative to its first floor; medians over walks of the
    differences between adjacent floors."""
    diffs: dict[tuple[int, int], list[float]] = {}
    for w, gt in walks:
        if len(w.baro) < 10 or len(gt) < 2:
            continue
        p = np.interp(gt[:, 0], w.baro[:, 0], w.baro[:, 1])
        h = hpa_to_height(p, p[0])
        per = {}
        for o in np.unique(gt[:, 3]).astype(int):
            per[o] = float(np.median(h[gt[:, 3] == o]))
        for a in per:
            for b in per:
                if a < b and ordinals.index(b) == ordinals.index(a) + 1:
                    diffs.setdefault((a, b), []).append(per[b] - per[a])
    steps = {f"{a}->{b}": {"median_m": float(np.median(v)), "walks": len(v), "iqr_m": float(np.subtract(*np.percentile(v, [75, 25])))}
             for (a, b), v in diffs.items()}
    elev = {ordinals[0]: 0.0}
    for a, b in zip(ordinals[:-1], ordinals[1:]):
        v = diffs.get((a, b))
        elev[b] = elev[a] + (float(np.median(v)) if v else 4.0)
    return elev, steps


def heading(w: Walk) -> np.ndarray:
    return forward_heading_from_quat(w.quat)


def radio_samples(train: list[tuple[Walk, np.ndarray]], wifi: bool, ble: bool = True) -> dict[str, list]:
    """{id: [(x, y, ordinal, rssi)]} at positions interpolated between same-floor landmarks."""
    samples: dict[str, list] = {}
    for w, gt in train:
        streams = ([("b:", w.ble)] if ble else []) + ([("w:", w.wifi)] if wifi else [])
        for prefix, stream in streams:
            if not stream:
                continue
            t = np.array([s[0] for s in stream])
            xy, lv, ok = interp_same_level(gt, t)
            for (_, tid, rssi, *_), p, o, good in zip(stream, xy, lv, ok):
                if good:
                    samples.setdefault(prefix + tid, []).append((p[0], p[1], int(o), rssi))
    return samples


def floor_fingerprints(train: list[tuple[Walk, np.ndarray]], known: set[str]) -> dict:
    """One fingerprint per training Wi-Fi scan: the scan's floor is the landmarks' when it falls between
    two landmarks on the same floor; other scans are dropped. Only the venue's own access points are
    kept, as integer dBm. Written compactly (docs/formats.md): {"ids": [id, ...], "scans": [[ordinal,
    id index, dBm, ...], ...]}."""
    out = []
    for w, gt in train:
        by_scan: dict[float, list] = {}
        for t, mac, rssi, scan in w.wifi:
            if "w:" + mac in known and t >= 0:
                by_scan.setdefault(scan, []).append((t, mac, rssi))
        if not by_scan:
            continue
        scans = sorted(by_scan.items())
        t = np.array([max(rd[0] for rd in readings) for _, readings in scans])
        _, lv, ok = interp_same_level(gt, t)
        for (_, readings), o, good in zip(scans, lv, ok):
            if good:
                rssi = {}
                for _, mac, r in readings:
                    rssi["w:" + mac] = max(rssi.get("w:" + mac, -200), int(round(r)))
                out.append((int(o), sorted(rssi.items())))
    ids = sorted({k for _, rssi in out for k, _ in rssi})
    index = {k: i for i, k in enumerate(ids)}
    return {"ids": ids, "scans": [[o] + [v for k, r in rssi for v in (index[k], r)] for o, rssi in out]}


def fit_radio_map(samples: dict[str, list], box, elev: dict, known: dict[str, tuple[float, float, int]], args):
    """Each transmitter is placed on the floor where it is heard strongest (median of its five
    strongest readings) and fitted on that floor's samples. The other floors' samples then give the
    loss per slab: median over transmitters of the median excess loss divided by the slabs between."""
    by_level: dict[int, dict[str, list]] = {}
    home = {}
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
        home[tid] = o
        by_level.setdefault(o, {})[tid] = [(x, y, r) for x, y, lv, r in rows if int(lv) == o]
    txs: list[Transmitter] = []
    for o, smp in sorted(by_level.items()):
        kn = {tid: known[tid][:2] for tid in smp if tid in known}
        for tx in fit_samples(smp, box, args.min_samples, args.min_spread, args.max_rms, kn):
            tx.level = o
            txs.append(tx)
    # Floor loss, from Wi-Fi when there is any (BLE devices can move between floors).
    per_tx = []
    ref = [t for t in txs if t.id.startswith("w:")] or txs
    for tx in ref:
        arr = np.array(samples[tx.id])
        other = arr[arr[:, 2].astype(int) != tx.level]
        if len(other) < 10:
            continue
        dz = np.array([elev[int(o)] - elev[tx.level] for o in other[:, 2]])
        d = np.sqrt((other[:, 0] - tx.x) ** 2 + (other[:, 1] - tx.y) ** 2 + dz ** 2)
        slabs = np.abs(np.array([sorted(elev).index(int(o)) - sorted(elev).index(tx.level) for o in other[:, 2]]))
        excess = (path_loss(d, tx.tx_power, tx.path_loss_exp) - other[:, 3]) / np.maximum(slabs, 1)
        per_tx.append(float(np.median(excess)))
    floor_loss = float(np.median(per_tx)) if per_tx else 15.0
    return txs, floor_loss, len(per_tx)


def known_beacons(cfg: dict, raw: Path, frame: maps.Frame) -> dict[str, tuple[float, float, int]]:
    """Surveyed beacons (BLE_Locations.csv: BLEk -> the MAC ending in :0k)."""
    if "beacons" not in cfg:
        return {}
    path = raw / cfg["packages"][0] / cfg["beacons"]
    out = {}
    for line in path.read_text().splitlines()[1:]:
        f = [v.strip().strip('"') for v in line.split(",")]
        if len(f) < 4:
            continue
        k = int("".join(ch for ch in f[0] if ch.isdigit()))
        x, y = frame.georef.to_local(float(f[1]), float(f[2]))
        out[f"b:be:ac:20:23:00:{k:02d}"] = (float(x), float(y), int(float(f[3])))
    return out


# --------------------------------------------------------------------------------------------
# Checks


def reachability(levels: dict, bounds, connectors: list, gts: list[tuple[str, np.ndarray]], clearance=0.25) -> dict:
    """Every pair of consecutive landmarks on one floor must be connected in free space with
    `clearance`; a floor change must be possible through a connector serving both floors whose area
    both landmarks' regions reach."""
    grids = {o: GridLevel(w.tolist(), bounds) for o, w in levels.items()}
    bad, total = [], 0

    def region(o, x, y):
        return grids[o].region(x, y, clearance)

    def touches(o, reg, area):
        lab = grids[o].labels(clearance)
        xs = np.array([p[0] for p in area])
        ys = np.array([p[1] for p in area])
        i0, j0 = grids[o].cell(xs.min(), ys.min())
        i1, j1 = grids[o].cell(xs.max(), ys.max())
        sub = lab[max(i0, 0):i1 + 1, max(j0, 0):j1 + 1]
        return bool((sub == reg).any())

    for name, gt in gts:
        for a, b in zip(gt[:-1], gt[1:]):
            total += 1
            oa, ob = int(a[3]), int(b[3])
            ra, rb = region(oa, a[1], a[2]), region(ob, b[1], b[2])
            if oa == ob:
                ok = ra == rb
            else:
                ok = any({oa, ob} <= set(c["levels"]) and touches(oa, ra, c["area"]) and touches(ob, rb, c["area"])
                         for c in connectors)
            if not ok:
                bad.append({"walk": name, "t": [round(float(a[0]), 2), round(float(b[0]), 2)],
                            "from": [round(float(a[1]), 2), round(float(a[2]), 2), oa],
                            "to": [round(float(b[1]), 2), round(float(b[2]), 2), ob]})
    return {"pairs": total, "unreachable": len(bad), "examples": bad[:30]}


# --------------------------------------------------------------------------------------------
# Output


def write_trace(path: Path, w: Walk, gt: np.ndarray, name: str, offset: float, known: set[str], wifi: bool,
                scans: bool, level_index: dict, elev: dict) -> dict:
    tw = TraceWriter(name, f"ipin {w.name} ({w.model})")
    r = rotation_matrices(w.quat)
    acc_w = np.einsum("nij,nj->ni", r, w.acc)
    gyro_w = np.einsum("nij,nj->ni", r, w.gyro)
    psi = wrap(heading(w) + offset)
    for i in range(len(w.imu_t)):
        t = w.imu_t[i]
        if t < 0:
            continue
        tw.imu(t, acc_w[i], (0.0, 0.0, gyro_w[i, 2]),
               (MAG_FIELD_UT * math.sin(psi[i]), MAG_FIELD_UT * math.cos(psi[i]), 0.0))
    for t, p in w.baro:
        if t >= 0:
            tw.baro(t, p)
    packets = 0
    for t, mac, rssi in w.ble:
        if "b:" + mac in known and t >= 0:
            tw.ble(t, "b:" + mac, rssi)
            packets += 1
    n_scans = 0
    if wifi:
        by_scan: dict[float, list] = {}
        for t, mac, rssi, scan in w.wifi:
            if "w:" + mac in known and t >= 0:
                by_scan.setdefault(scan, []).append((t, mac, rssi))
        for k, (scan, readings) in enumerate(sorted(by_scan.items()), start=1):
            t_batch = max(rd[0] for rd in readings)
            for _, mac, rssi in sorted(readings, key=lambda rd: rd[1]):
                tw.ble(t_batch, "w:" + mac, rssi, k if scans else None)
                packets += 1
            n_scans = k
    for i, (t, x, y, o) in enumerate(gt):
        j = min(i + 1, len(gt) - 1)
        k = j - 1 if j == i else i
        hd = math.atan2(gt[j, 2] - gt[k, 2], gt[j, 1] - gt[k, 1]) if j != k else 0.0
        tw.gt(t, x, y, hd, level_index[int(o)], elev[int(o)])
    tw.write(path)
    floors = sorted({int(o) for o in gt[:, 3]})
    return {"duration_s": float(w.imu_t[-1] - w.imu_t[0]) if len(w.imu_t) else 0.0, "packets": packets,
            "scans": n_scans, "waypoints": len(gt), "floors": floors, "phone": w.model,
            "barometer": bool(len(w.baro))}


def convert_site(site: str, cfg: dict, args) -> tuple[list, list, list, dict]:
    raw = args.raw
    pkg = raw / cfg["packages"][0]
    calib = maps.read_calib(pkg / cfg["calib"])
    refs = maps.read_references(pkg / cfg["references"])
    images = [pkg / p for p in cfg["maps"].values()]
    frame, georef_report = maps.fit_frame(images, calib, refs, tuple(cfg["marker_px"]))
    report = {"site": site, "georef": georef_report}
    print(f"{site}: frame {frame.size_m[0]:.1f} x {frame.size_m[1]:.1f} m, marker fit max residual "
          f"{georef_report['residual_max_m']:.3f} m")

    walks = load_walks(cfg, raw)
    report["walks"] = {k: len(v) for k, v in walks.items()}
    print(f"  walks: {report['walks']}")
    gts = {k: [(w, gt_local(w, frame)) for w in v] for k, v in walks.items()}
    all_gt = np.vstack([gt for v in gts.values() for _, gt in v if len(gt)])

    edits = maps.load_edits(HERE / "edits" / f"{site}.json")
    known = known_beacons(cfg, raw, frame)
    ordinals, levels, masks, footprint = build_levels(site, cfg, raw, frame, edits, all_gt, args.out, known)
    connectors = edits.get("connectors", [])
    if cfg.get("walked_doors"):
        levels, doors = cut_walked_doors(levels, [gt for _, gt in gts["train"]], cfg["walked_doors"]["width_m"],
                                         cfg["walked_doors"]["min_walks"])
        report["walked_doors"] = doors
    bounds = [0.0, 0.0, round(frame.size_m[0], 3), round(frame.size_m[1], 3)]
    level_index = {o: i for i, o in enumerate(ordinals)}

    # Floors and heading.
    elev, steps = fit_elevations(gts["train"], ordinals)
    report["elevation_m"] = elev
    report["floor_steps"] = steps
    runs = [(w.imu_t, heading(w), run) for w, gt in gts["train"] for run in same_level_runs(gt) if len(w.imu_t)]
    offset, spread, nseg = fit_heading_offset(runs)
    report["heading_offset_deg"] = math.degrees(offset)
    report["heading_spread_deg"] = math.degrees(spread)
    report["heading_segments"] = nseg
    print(f"  heading offset {math.degrees(offset):+.1f} deg (spread {math.degrees(spread):.1f} deg over {nseg} segments); "
          f"floor steps " + ", ".join(f"{k}: {v['median_m']:.2f} m" for k, v in steps.items()))

    # Radio map.
    # The 2022 building has no BLE infrastructure (technical annex): its BLE packets come from
    # unknown, possibly moving devices, so sites.json can leave BLE out.
    samples = radio_samples(gts["train"], args.wifi, cfg.get("ble", True))
    txs, floor_loss, n_floor = fit_radio_map(samples, tuple(bounds), elev, known, args)
    report["transmitters"] = {"ble": sum(t.id.startswith("b:") for t in txs), "wifi": sum(t.id.startswith("w:") for t in txs),
                              "surveyed_ble": sum(t.id in known for t in txs)}
    report["fit_rms_db_median"] = float(np.median([t.rms_db for t in txs])) if txs else float("nan")
    report["floor_attenuation_db"] = {"fitted": floor_loss, "transmitters": n_floor}
    print(f"  transmitters {report['transmitters']}, median fit RMS {report['fit_rms_db_median']:.1f} dB, "
          f"floor loss {floor_loss:.1f} dB/slab over {n_floor}")

    # Checks.
    fp = np.array(footprint) if footprint else None
    inside = [point_in_rings((x, y), [np.vstack([fp, fp[:1]])]) for _, x, y, _ in all_gt] if fp is not None else [True]
    report["landmarks_inside_footprint"] = float(np.mean(inside))
    report["walls_per_level"] = {o: len(w) for o, w in levels.items()}
    named = [(f"{split}:{w.name}", gt) for split, v in gts.items() for w, gt in v]
    report["reachability"] = reachability(levels, bounds, connectors, named)
    cross = {"train": 0, "tune": 0, "holdout": 0}
    for split, v in gts.items():
        for _, gt in v:
            for a, b in zip(gt[:-1], gt[1:]):
                if a[3] == b[3]:
                    cross[split] += crossings(a[1:3], b[1:3], levels[int(a[3])])
    report["straight_line_wall_crossings"] = cross
    rch = report["reachability"]
    print(f"  map: walls {report['walls_per_level']}, landmarks inside footprint {report['landmarks_inside_footprint']:.3f}, "
          f"unreachable landmark pairs {rch['unreachable']}/{rch['pairs']}, straight-line crossings {cross}")

    venue = {
        "name": f"ipin_{site}",
        "bounds": bounds,
        "wall_attenuation_db": args.wall_attenuation_db,
        "floor_attenuation_db": round(floor_loss, 2),
        "georef": frame.georef.as_json(),
        "levels": [{"ordinal": o, "name": f"Floor {o}", "elevation_m": round(elev[o], 3),
                    "walls": [[round(float(v), 3) for v in w] for w in levels[o]],
                    **({"footprint": lf} if (lf := edits.get("levels", {}).get(str(o), {}).get("footprint")) else {})}
                   for o in ordinals],
        "connectors": connectors,
        "beacons": [{"id": t.id, "x": round(t.x, 3), "y": round(t.y, 3), "level": t.level,
                     "tx_power": round(t.tx_power, 2), "path_loss_exp": round(t.path_loss_exp, 3)} for t in txs],
    }
    if footprint:
        venue["footprint"] = footprint
    if args.wifi and args.fingerprints:
        venue["floor_fingerprints"] = floor_fingerprints(gts["train"], {t.id for t in txs})
        report["floor_fingerprints"] = len(venue["floor_fingerprints"]["scans"])
        print(f"  floor fingerprints: {report['floor_fingerprints']} training scans")
    site_out = args.out / site
    site_out.mkdir(parents=True, exist_ok=True)
    venue_path = site_out / "venue.json"
    venue_path.write_text(json.dumps(venue))
    if args.data:
        args.data.mkdir(parents=True, exist_ok=True)
        (args.data / f"{site}.json").write_text(json.dumps(venue) + "\n")

    if args.debug:
        dbg = args.out / "debug"
        dbg.mkdir(parents=True, exist_ok=True)
        tx_by_level = {}
        for t in txs:
            tx_by_level.setdefault(t.level, []).append((t.x, t.y, (0, 120, 255) if t.id in known else (0, 170, 0)))
        for o in ordinals:
            pts = [(x, y, (255, 140, 0)) for _, x, y, lv in all_gt if int(lv) == o] + tx_by_level.get(o, [])
            polys = [(c["area"], (160, 0, 200)) for c in connectors if o in c["levels"]]
            if edits.get("levels", {}).get(str(o), {}).get("footprint"):
                polys.append((edits["levels"][str(o)]["footprint"], (0, 150, 150)))
            if footprint:
                polys.append((footprint, (0, 0, 255)))
            maps.debug_png(dbg / f"{site}_L{o}.png", masks[o], levels[o], pts, polys)

    known_ids = {t.id for t in txs}
    entries = {"tune": [], "holdout": []}
    report["skipped"] = []
    for split in ("tune", "holdout"):
        for w, gt in gts[split]:
            if len(w.imu_t) < 100 or len(gt) < 2:
                # The engine needs an absolute heading: phones without a rotation vector (AHRS rows)
                # cannot be written in the engine's frame.
                why = "no orientation sensor (AHRS)" if len(w.imu_t) < 100 else "fewer than 2 landmarks"
                report["skipped"].append({"walk": w.name, "phone": w.model, "reason": why})
                print(f"  skipped {w.name} ({w.model}): {why}")
                continue
            name = f"{site}_{w.name}"
            trace_path = site_out / f"{w.name}.csv"
            stats = write_trace(trace_path, w, gt, name, offset, known_ids, args.wifi, args.scans, level_index, elev)
            entries[split].append({"name": name, "group": f"{site}_{split}", "venue": str(venue_path.relative_to(args.out)),
                                   "trace": str(trace_path.relative_to(args.out)), **stats})
    report["test_traces"] = {k: len(v) for k, v in entries.items()}
    return entries["tune"] + entries["holdout"], entries["tune"], entries["holdout"], report


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw", type=Path, default=HERE / "raw", help="folder with the unpacked <year>/ packages")
    ap.add_argument("--out", type=Path, default=HERE / "out")
    ap.add_argument("--sites", nargs="*", help="limit to these sites (sites.json keys)")
    ap.add_argument("--wifi", action="store_true", help="also fit and replay Wi-Fi access points")
    ap.add_argument("--scans", action="store_true", help="with --wifi, write each Wi-Fi scan as one batch")
    ap.add_argument("--min-samples", type=int, default=30)
    ap.add_argument("--min-spread", type=float, default=3.0, help="metres; transmitters heard from one spot are dropped")
    ap.add_argument("--max-rms", type=float, default=8.0, help="dB; transmitters fitting worse are dropped")
    ap.add_argument("--wall-attenuation-db", type=float, default=3.0)
    ap.add_argument("--debug", action="store_true", help="write per-floor wall/landmark PNGs to <out>/debug")
    ap.add_argument("--fingerprints", action="store_true",
                    help="with --wifi, store the training scans as floor fingerprints in the venue")
    ap.add_argument("--data", type=Path, help="also write each site's venue here as <site>.json (data/ipin)")
    args = ap.parse_args()
    if args.scans and not args.wifi:
        ap.error("--scans needs --wifi")

    sites = json.loads((HERE / "sites.json").read_text())
    all_entries, tune, holdout, reports = [], [], [], []
    for site, cfg in sites.items():
        if args.sites and site not in args.sites:
            continue
        e, t, h, rep = convert_site(site, cfg, args)
        all_entries += e
        tune += t
        holdout += h
        reports.append(rep)
    write_manifests(args.out, all_entries, tune, holdout)
    (args.out / "report.json").write_text(json.dumps(reports, indent=1))
    print(f"wrote {len(all_entries)} traces ({len(tune)} tune, {len(holdout)} holdout) to {args.out / 'manifest.json'}")


if __name__ == "__main__":
    main()
