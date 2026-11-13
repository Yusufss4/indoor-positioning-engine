#!/usr/bin/env python3
"""Converts the Microsoft Indoor Location Competition 2.0 data into ipe venues and traces.

For every site and floor:

* the floor plan (GCJ-02 GeoJSON scaled to the floor's width x height in metres) becomes a
  single-level venue, with walls from the floor outline (``boundary``), the shop outlines
  (``units``) or the shop outlines with doorways where training paths cross them (``doors``),
  and the named shop outlines as ``rooms`` for enter/exit events;
* paths are split into training and test sets by path id;
* training paths fit a radio map: position, tx power and path-loss exponent per iBeacon and,
  with ``--wifi``, per Wi-Fi access point;
* test paths become ipe traces: earth-frame IMU in the engine's convention, BLE/Wi-Fi rows and
  one ground-truth row per surveyor waypoint.

A manifest lists the venue/trace pairs for ``ipe-regress --traces``. See docs/real_data.md.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from common.geom import (crossing_params, crossings, cut_gaps, dedupe_segments,  # noqa: E402
                         forward_heading_from_quat, point_in_rings, ring_segments, rotation_matrices, wrap)
from common.geom import fit_heading_offset as _fit_heading_offset  # noqa: E402
from common.fingerprints import drop_rare, fingerprint_table, unmodelled_beacons  # noqa: E402
from common.radiomap import Transmitter, fit_samples, merge_virtual_aps, physical_ap  # noqa: E402
from common.trace import write_manifests  # noqa: E402

MAG_FIELD_UT = 45.0
# An hour. When a walk's Wi-Fi readings are typically further than this from the scan that reports them,
# `last_seen` is on another clock (a single stale reading can be that old on the same clock).
CLOCK_MISMATCH_MS = 3.6e6


# --------------------------------------------------------------------------------------------
# Path files


@dataclass
class PathData:
    path_id: str
    imu_t: np.ndarray = field(default_factory=lambda: np.zeros(0))  # seconds, absolute
    acc: np.ndarray = field(default_factory=lambda: np.zeros((0, 3)))
    gyro: np.ndarray = field(default_factory=lambda: np.zeros((0, 3)))
    rotvec: np.ndarray = field(default_factory=lambda: np.zeros((0, 4)))
    wifi: list = field(default_factory=list)  # (t_seen, bssid, rssi, t_scan)
    ble: list = field(default_factory=list)  # (t, id, rssi)
    waypoints: np.ndarray = field(default_factory=lambda: np.zeros((0, 3)))  # t, x, y


def _xyz(cols, start):
    return [float(cols[start]), float(cols[start + 1]), float(cols[start + 2])]


def parse_path(path: Path) -> PathData:
    """Reads one path file. Timestamps are milliseconds; returned in seconds."""
    acc, gyro, rot, wifi, ble, wps = {}, {}, {}, [], [], []
    seen_wifi = set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            cols = line.rstrip("\n").split("\t")
            if len(cols) < 3:
                continue
            kind = cols[1]
            try:
                t = int(cols[0])
                if kind == "TYPE_ACCELEROMETER":
                    acc[t] = _xyz(cols, 2)
                elif kind == "TYPE_GYROSCOPE":
                    gyro[t] = _xyz(cols, 2)
                elif kind == "TYPE_ROTATION_VECTOR":
                    rot[t] = _xyz(cols, 2)
                elif kind == "TYPE_WIFI" and len(cols) >= 7:
                    bssid, rssi, last_seen = cols[3], float(cols[4]), int(cols[6])
                    # A scan repeats an access point until it is heard again; keep each reading once.
                    if (bssid, last_seen) in seen_wifi:
                        continue
                    seen_wifi.add((bssid, last_seen))
                    wifi.append((last_seen, physical_ap(bssid), rssi, t))
                elif kind == "TYPE_BEACON" and len(cols) >= 7:
                    ble.append((t / 1000.0, f"{cols[2]}_{cols[3]}_{cols[4]}", float(cols[6])))
                elif kind == "TYPE_WAYPOINT":
                    wps.append((t / 1000.0, float(cols[2]), float(cols[3])))
            except (ValueError, IndexError):
                continue

    pd = PathData(path.stem)
    ts = sorted(set(acc) & set(gyro) & set(rot))
    if ts:
        pd.imu_t = np.array(ts, dtype=float) / 1000.0
        pd.acc = np.array([acc[t] for t in ts])
        pd.gyro = np.array([gyro[t] for t in ts])
        r = np.array([rot[t] for t in ts])
        w = np.sqrt(np.clip(1.0 - np.sum(r * r, axis=1), 0.0, None))
        pd.rotvec = np.column_stack([r, w])
    # The competition's test files count time from the start of the walk but leave `last_seen` on the
    # phone's clock. A reading is never newer than the scan that reports it, so the newest one marks
    # the clock difference (to within the scan's own delay).
    if wifi and abs(float(np.median([seen - t for seen, _, _, t in wifi]))) > CLOCK_MISMATCH_MS:
        shift = max(seen - t for seen, _, _, t in wifi)
        wifi = [(seen - shift, ap, rssi, t) for seen, ap, rssi, t in wifi]
    pd.wifi = merge_virtual_aps([(seen / 1000.0, ap, rssi, t / 1000.0) for seen, ap, rssi, t in wifi])
    pd.ble = sorted(ble)
    pd.waypoints = np.array(sorted(wps)) if wps else np.zeros((0, 3))
    return pd


def forward_heading(pd: PathData) -> np.ndarray:
    """Heading of the phone's top edge (+y; walking direction when held flat in portrait),
    counter-clockwise from magnetic east."""
    return forward_heading_from_quat(pd.rotvec)


def interp_waypoints(pd: PathData, t: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Linear interpolation of the surveyor's waypoints; mask is False outside the labelled span."""
    wp = pd.waypoints
    mask = (t >= wp[0, 0]) & (t <= wp[-1, 0])
    xy = np.column_stack([np.interp(t, wp[:, 0], wp[:, 1]), np.interp(t, wp[:, 0], wp[:, 2])])
    return xy, mask


# --------------------------------------------------------------------------------------------
# Floor plan


@dataclass
class Floor:
    width: float
    height: float
    lon0: float
    lat0: float
    lon1: float
    lat1: float
    outline: list  # rings of the floor polygon, metres
    units: list  # rings of the shop polygons, metres
    rooms: list = field(default_factory=list)  # (id, name, outer ring in metres) per named area

    def to_xy(self, lon, lat):
        x = (np.asarray(lon) - self.lon0) / (self.lon1 - self.lon0) * self.width
        y = (np.asarray(lat) - self.lat0) / (self.lat1 - self.lat0) * self.height
        return x, y


def _gcj02_offset(lon: float, lat: float) -> tuple[float, float]:
    """GCJ-02 minus WGS84 at a point, in degrees (the published Krasovsky-ellipsoid formula)."""
    a, ee = 6378245.0, 0.00669342162296594323
    x, y = lon - 105.0, lat - 35.0
    common = (20 * math.sin(6 * x * math.pi) + 20 * math.sin(2 * x * math.pi)) * 2 / 3
    dlat = -100 + 2 * x + 3 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * math.sqrt(abs(x)) + common
    dlat += (20 * math.sin(y * math.pi) + 40 * math.sin(y / 3 * math.pi)) * 2 / 3
    dlat += (160 * math.sin(y / 12 * math.pi) + 320 * math.sin(y * math.pi / 30)) * 2 / 3
    dlon = 300 + x + 2 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * math.sqrt(abs(x)) + common
    dlon += (20 * math.sin(x * math.pi) + 40 * math.sin(x / 3 * math.pi)) * 2 / 3
    dlon += (150 * math.sin(x / 12 * math.pi) + 300 * math.sin(x / 30 * math.pi)) * 2 / 3
    rad = math.radians(lat)
    m = 1 - ee * math.sin(rad) ** 2
    dlat = dlat * 180 / ((a * (1 - ee)) / (m * math.sqrt(m)) * math.pi)
    dlon = dlon * 180 / (a / math.sqrt(m) * math.cos(rad) * math.pi)
    return dlon, dlat


def gcj02_to_wgs84(lon: float, lat: float) -> tuple[float, float]:
    """Inverts the GCJ-02 offset by fixed-point iteration (converges to millimetres)."""
    wlon, wlat = lon, lat
    for _ in range(5):
        dlon, dlat = _gcj02_offset(wlon, wlat)
        wlon, wlat = lon - dlon, lat - dlat
    return wlon, wlat


def _rings(geom):
    if geom["type"] == "Polygon":
        return list(geom["coordinates"])
    if geom["type"] == "MultiPolygon":
        return [ring for poly in geom["coordinates"] for ring in poly]
    return []


def _outer_rings(geom):
    if geom["type"] == "Polygon":
        return [geom["coordinates"][0]]
    if geom["type"] == "MultiPolygon":
        return [poly[0] for poly in geom["coordinates"]]
    return []


# Generic unit names in the plans (pinyin) and the English room names they get. None drops the unit:
# atrium voids, staff-only areas and decorative "auxiliary surfaces" are not places a visitor can be.
UNIT_NAMES = {
    "zhongkong": None,
    "feikaifangquyu": None,
    "fuzhumian": None,
    "louti": "Stairs",
    "zidongfuti": "Escalator",
    "dianti": "Lift",
    "huoti": "Freight lift",
    "xiuxiqu": "Rest area",
    "xishoujianfuzhumian": "Washroom area",
    "nanweishengjian": "Men's toilet",
    "nvweishengjian": "Women's toilet",
    "canjirenweishengjian": "Accessible toilet",
    "churukou": "Entrance",
    "tingchechangchurukou": "Car park entrance",
    "kongpu": "Vacant unit",
}


def load_floor(floor_dir: Path) -> Floor:
    info = json.loads((floor_dir / "floor_info.json").read_text())["map_info"]
    gj = json.loads((floor_dir / "geojson_map.json").read_text())
    floor_rings, unit_rings, room_rings = [], [], []

    # site1 tags the floor polygon type/category "floor"; site2 gives it a "floor": "F1" property.
    def tagged(props):
        return props.get("type") == "floor" or props.get("category") == "floor" or "floor" in props

    floor_features = {i for i, feat in enumerate(gj["features"]) if tagged(feat.get("properties", {}))}
    if not floor_features:
        # Many buildings of the full competition set tag nothing: there the floor polygon is the
        # feature that spans the plan, i.e. the one with the largest bounding box (the first on a tie).
        def box_area(feat):
            pts = np.array([p[:2] for ring in _rings(feat["geometry"]) for p in ring])
            return float(np.ptp(pts[:, 0]) * np.ptp(pts[:, 1])) if len(pts) else 0.0

        areas = [box_area(feat) for feat in gj["features"]]
        floor_features = {int(np.argmax(areas))}
    for i, feat in enumerate(gj["features"]):
        rings = _rings(feat["geometry"])
        props = feat.get("properties", {})
        if i in floor_features:
            floor_rings += rings
        else:
            unit_rings += rings
            raw = (props.get("name") or "").strip()
            name = UNIT_NAMES.get(raw, raw)
            if name is None:
                continue
            uid = str(props.get("id") or f"unit{len(room_rings) + 1}")
            outer = _outer_rings(feat["geometry"])
            for k, ring in enumerate(outer):
                room_rings.append((uid + (f"_{k + 1}" if len(outer) > 1 else ""), name or f"Unit {uid[-6:]}", ring))
    # The floor's lon/lat bounding box spans the floor image, which is width x height metres.
    ref = floor_rings or unit_rings
    pts = np.array([p[:2] for ring in ref for p in ring])
    floor = Floor(info["width"], info["height"], pts[:, 0].min(), pts[:, 1].min(), pts[:, 0].max(), pts[:, 1].max(), [], [])

    def convert(rings):
        out = []
        for ring in rings:
            arr = np.array([p[:2] for p in ring])
            x, y = floor.to_xy(arr[:, 0], arr[:, 1])
            out.append(np.column_stack([x, y]))
        return out

    floor.outline = convert(floor_rings)
    floor.units = convert(unit_rings)
    floor.rooms = [(uid, name, ring) for (uid, name, _), ring in
                   zip(room_rings, convert([r for _, _, r in room_rings]))]
    return floor


def room_list(floor: Floor) -> list[dict]:
    """The venue's ``rooms``: one per unit outline (outer ring, closing point dropped), named for enter/exit
    events. Slivers under 0.5 m² are left out."""
    rooms, seen = [], set()
    for uid, name, ring in floor.rooms:
        pts = [[round(float(x), 3), round(float(y), 3)] for x, y in ring]
        if len(pts) > 3 and pts[0] == pts[-1]:
            pts.pop()
        x, y = np.array(pts).T
        if len(pts) < 3 or 0.5 * abs(np.dot(x, np.roll(y, 1)) - np.dot(y, np.roll(x, 1))) < 0.5 or uid in seen:
            continue
        seen.add(uid)
        rooms.append({"id": uid, "name": name, "area": pts})
    return rooms


def cut_doors(walls: np.ndarray, paths: list[PathData], width: float, min_piece: float = 0.1) -> tuple[np.ndarray, int]:
    """Opens a gap `width` metres wide, centred on the crossing, wherever a straight line between
    consecutive surveyor waypoints crosses a wall. The plans have no doors; a walk through a wall
    is the evidence for one. Returns the cut walls and the centre of each merged gap."""
    cuts: dict[int, list[float]] = {}
    for pd in paths:
        for a, b in zip(pd.waypoints[:-1], pd.waypoints[1:]):
            idx, u = crossing_params(a[1:], b[1:], walls)
            for i, ui in zip(idx, u):
                cuts.setdefault(int(i), []).append(float(ui))
    return cut_gaps(walls, cuts, width, min_piece)


# --------------------------------------------------------------------------------------------
# Map north and radio map


def fit_heading_offset(paths: list[PathData], min_len=4.0) -> tuple[float, float, int]:
    """Angle to add to the phone's magnetic heading to get the map heading (see common.geom)."""
    return _fit_heading_offset(((pd.imu_t, forward_heading(pd), pd.waypoints) for pd in paths if len(pd.imu_t)), min_len)


def fit_radio_map(train: list[PathData], floor: Floor, args) -> list[Transmitter]:
    samples: dict[str, list] = {}
    for pd in train:
        if len(pd.waypoints) < 2:
            continue
        streams = [("b:", pd.ble)] + ([("w:", pd.wifi)] if args.wifi else [])
        for prefix, stream in streams:
            if not stream:
                continue
            t = np.array([s[0] for s in stream])
            xy, mask = interp_waypoints(pd, t)
            for (_, tid, rssi, *_), p, ok in zip(stream, xy, mask):
                if ok:
                    samples.setdefault(prefix + tid, []).append((p[0], p[1], rssi))
    return fit_samples(samples, (0.0, 0.0, floor.width, floor.height), args.min_samples, args.min_spread, args.max_rms)


def wifi_scans(pd: PathData) -> list[tuple[float, dict[str, float]]]:
    """(time, {"w:" + access point: dBm}) per Wi-Fi scan, timed by its newest reading as the trace
    writer does; an access point heard twice in a scan keeps its strongest reading."""
    by_scan: dict[float, list] = {}
    for t, ap, rssi, t_scan in pd.wifi:
        by_scan.setdefault(t_scan, []).append((t, ap, rssi))
    out = []
    for _, readings in sorted(by_scan.items()):
        rssi: dict[str, float] = {}
        for _, ap, r in readings:
            rssi["w:" + ap] = max(rssi.get("w:" + ap, -200.0), r)
        out.append((max(r[0] for r in readings), rssi))
    return sorted(out, key=lambda s: s[0])


def training_scans(train: list[PathData]) -> list[tuple[int, float, float, dict[str, float]]]:
    """(0, x, y, readings) for every training scan taken between two waypoints."""
    out = []
    for pd in train:
        scans = wifi_scans(pd)
        if len(pd.waypoints) < 2 or not scans:
            continue
        xy, mask = interp_waypoints(pd, np.array([t for t, _ in scans]))
        out += [(0, p[0], p[1], rssi) for (_, rssi), p, ok in zip(scans, xy, mask) if ok]
    return out


# --------------------------------------------------------------------------------------------
# Output


def write_trace(path: Path, pd: PathData, name: str, offset: float, known: set[str], wifi: bool,
                scans: bool = False) -> dict:
    t0 = pd.imu_t[0]
    r = rotation_matrices(pd.rotvec)
    acc_w = np.einsum("nij,nj->ni", r, pd.acc)
    gyro_w = np.einsum("nij,nj->ni", r, pd.gyro)
    # Engine convention (docs/formats.md): phone flat, x forward, so heading_from_mag(m) =
    # atan2(mx, my). The absolute heading is Android's rotation-vector azimuth turned into the
    # map frame by the per-site offset.
    psi = wrap(forward_heading(pd) + offset)
    rows = []
    for i in range(len(pd.imu_t)):
        a, g = acc_w[i], gyro_w[i]
        mx, my = MAG_FIELD_UT * math.sin(psi[i]), MAG_FIELD_UT * math.cos(psi[i])
        rows.append(
            (
                pd.imu_t[i] - t0,
                f"imu,{pd.imu_t[i] - t0:.3f},{a[0]:.4f},{a[1]:.4f},{a[2]:.4f},0,0,{g[2]:.5f},{mx:.3f},{my:.3f},0",
            )
        )
    streams = [("b:", pd.ble)] + ([("w:", pd.wifi)] if wifi and not scans else [])
    packets = 0
    for prefix, stream in streams:
        for t, tid, rssi, *_ in stream:
            if prefix + tid in known and t >= t0:
                rows.append((t - t0, f"ble,{t - t0:.3f},{prefix}{tid},{rssi:.0f}"))
                packets += 1
    n_scans = 0
    if wifi and scans:
        # One batch per Wi-Fi scan, stamped with its newest reading. The rows share a time and stay
        # contiguous (the sort below is stable), which is how replay recognises the scan.
        by_scan: dict[float, list] = {}
        for t, tid, rssi, t_scan in pd.wifi:
            if "w:" + tid in known and t >= t0:
                by_scan.setdefault(t_scan, []).append((t, tid, rssi))
        batches = sorted((max(r[0] for r in readings), t_scan, readings) for t_scan, readings in by_scan.items())
        for n_scans, (t_batch, _, readings) in enumerate(batches, start=1):
            for _, tid, rssi in sorted(readings, key=lambda r: r[1]):
                rows.append((t_batch - t0, f"ble,{t_batch - t0:.3f},w:{tid},{rssi:.0f},{n_scans}"))
                packets += 1
    wp = pd.waypoints
    for i, (t, x, y) in enumerate(wp):
        j = min(i + 1, len(wp) - 1)
        k = j - 1 if j == i else i
        heading = math.atan2(wp[j, 2] - wp[k, 2], wp[j, 1] - wp[k, 1])
        rows.append((t - t0, f"gt,{t - t0:.3f},{x:.3f},{y:.3f},{heading:.4f}"))
    rows.sort(key=lambda r: r[0])
    with open(path, "w") as f:
        f.write(f"# ipe-trace v1\n# scenario: {name}\n# source: msil path {pd.path_id}\n")
        for _, line in rows:
            f.write(line + "\n")
    stats = {"duration_s": float(pd.imu_t[-1] - t0), "packets": packets, "waypoints": len(wp)}
    if scans:
        stats["scans"] = n_scans
    return stats


def is_test(path_id: str, test_every: int) -> bool:
    return zlib.crc32(path_id.encode()) % test_every == 0


def convert_floor(site: str, floor_name: str, floor_dir: Path, paths: list[PathData], offset: float, args, out: Path):
    floor = load_floor(floor_dir)
    train = [p for p in paths if not is_test(p.path_id, args.test_every)]
    test = [p for p in paths if is_test(p.path_id, args.test_every)]

    outline = ring_segments(floor.outline)
    units = dedupe_segments(np.vstack([outline, ring_segments(floor.units)]))
    # Doors come from training walks only, so test walks can still run into a wall.
    doors, door_centres = cut_doors(units, train, args.door_width)
    wall_sets = {"none": np.zeros((0, 4)), "boundary": outline, "units": units, "doors": doors}
    report = {"site": site, "floor": floor_name, "train_paths": len(train), "test_paths": len(test)}
    report["wall_segments"] = {k: len(v) for k, v in wall_sets.items()}
    report["doors_cut"] = len(door_centres)
    report["door_centres"] = [[round(float(v), 2) for v in c] for c in door_centres]

    # Ground truth vs map: a surveyor path that crosses a wall means that wall set would kill
    # the true hypothesis in the filter.
    wp_inside = wp_total = 0
    test_ids = {p.path_id for p in test}
    cross = {k: 0 for k in wall_sets}
    paths_crossing = {k: 0 for k in wall_sets}
    cross_test = {k: 0 for k in wall_sets}
    for pd in paths:
        for _, x, y in pd.waypoints:
            wp_total += 1
            wp_inside += point_in_rings((x, y), floor.outline) if floor.outline else 1
        for k, walls in wall_sets.items():
            n = sum(crossings(a[1:], b[1:], walls) for a, b in zip(pd.waypoints[:-1], pd.waypoints[1:]))
            cross[k] += n
            paths_crossing[k] += n > 0
            if pd.path_id in test_ids:
                cross_test[k] += n
    report["waypoints_inside_outline"] = wp_inside / max(wp_total, 1)
    report["truth_wall_crossings"] = cross
    report["truth_wall_crossings_test"] = cross_test
    report["paths_crossing_walls"] = paths_crossing

    mode = args.walls
    if mode == "auto":
        mode = "units" if cross["units"] == 0 else "boundary" if cross["boundary"] == 0 else "none"
    report["wall_mode"] = mode

    txs = fit_radio_map(train, floor, args)
    report["transmitters"] = {"ble": sum(t.id.startswith("b:") for t in txs), "wifi": sum(t.id.startswith("w:") for t in txs)}
    report["fit_rms_db_median"] = float(np.median([t.rms_db for t in txs])) if txs else float("nan")

    name = f"{site}_{floor_name}"
    # The plans are in GCJ-02 (China's offset datum), about 500 m from WGS84 here. The offset varies
    # by under a metre across a floor, so shifting the origin is enough.
    origin_lon, origin_lat = gcj02_to_wgs84(floor.lon0, floor.lat0)
    venue = {
        "name": name,
        "bounds": [0, 0, round(floor.width, 3), round(floor.height, 3)],
        "wall_attenuation_db": args.wall_attenuation_db,
        "georef": {"origin_lat": origin_lat, "origin_lon": origin_lon, "rotation_deg": 0},
        "walls": [[round(float(v), 3) for v in w] for w in wall_sets[mode]],
        "rooms": room_list(floor),
        "beacons": [
            {"id": t.id, "x": round(t.x, 3), "y": round(t.y, 3), "tx_power": round(t.tx_power, 2), "path_loss_exp": round(t.path_loss_exp, 3)}
            for t in txs
        ],
    }
    known = {t.id for t in txs}
    if args.wifi and args.fingerprints:
        scans = training_scans(train)
        extra = unmodelled_beacons(scans, known, args.min_fingerprint_scans)
        for b in extra:
            del b["level"]  # single-level venue
        venue["beacons"] += extra
        known |= {b["id"] for b in extra}
        scans = drop_rare(scans, known)
        venue["floor_fingerprints"] = fingerprint_table(scans)
        report["fingerprints"] = {"scans": len(scans), "unmodelled_beacons": len(extra)}
    floor_out = out / site / floor_name
    floor_out.mkdir(parents=True, exist_ok=True)
    venue_path = floor_out / "venue.json"
    venue_path.write_text(json.dumps(venue))
    if args.wifi and args.fingerprints:
        # The surveyed points of the training walks, for offline snapping (evaluate.py --snap): the
        # surveyors of this data set walked between marked points, and the test walks use the same ones.
        points = sorted({(round(float(x), 2), round(float(y), 2)) for pd in train for _, x, y in pd.waypoints})
        (floor_out / "waypoints.json").write_text(json.dumps([list(q) for q in points]))

    entries = []
    for pd in test:
        if len(pd.imu_t) < 100 or len(pd.waypoints) < 2:
            continue
        trace_path = floor_out / f"{pd.path_id}.csv"
        stats = write_trace(trace_path, pd, f"{name}_{pd.path_id}", offset, known, args.wifi, args.scans)
        entries.append({"name": f"{name}_{pd.path_id}", "group": name, "venue": str(venue_path.relative_to(out)), "trace": str(trace_path.relative_to(out)), **stats})
    report["test_traces"] = len(entries)
    return entries, report


def site_floors(args) -> list[tuple[str, list[tuple[str, Path, list[Path]]]]]:
    """[(site, [(floor name, folder with the plan, path files)])] for the sample's layout
    (<raw>/<site>/<floor>/{path_data_files/*.txt, floor_info.json, geojson_map.json}) or, with
    --kaggle, the competition's (<dir>/train/<site>/<floor>/*.txt, <dir>/metadata/<site>/<floor>/)."""
    out = []
    if args.kaggle:
        wanted = None
        if args.test_sites:
            rows = (args.kaggle / "sample_submission.csv").read_text().splitlines()[1:]
            wanted = {r.split("_", 1)[0] for r in rows}
        for site_dir in sorted(p for p in (args.kaggle / "train").iterdir() if p.is_dir()):
            if wanted is not None and site_dir.name not in wanted:
                continue
            floors = [(f.name, args.kaggle / "metadata" / site_dir.name / f.name, sorted(f.glob("*.txt")))
                      for f in sorted(p for p in site_dir.iterdir() if p.is_dir())]
            out.append((site_dir.name, floors))
    else:
        for site_dir in sorted(p for p in args.raw.iterdir() if p.is_dir()):
            floors = [(f.name, f, sorted((f / "path_data_files").glob("*.txt")))
                      for f in sorted(p for p in site_dir.iterdir() if (p / "path_data_files").is_dir())]
            out.append((site_dir.name, floors))
    if args.sites:
        out = [(site, floors) for site, floors in out if site in args.sites]
    if args.floors:
        out = [(site, [f for f in floors if f[0] in args.floors]) for site, floors in out]
    return out


def convert_site(job) -> tuple[list, list, list[str]]:
    site, floors, args = job
    parsed = {name: [parse_path(p) for p in files] for name, _, files in floors}
    # Map north is a property of the site: fit it on every floor's training paths.
    train_all = [p for ps in parsed.values() for p in ps if not is_test(p.path_id, args.test_every)]
    offset, spread, segments = fit_heading_offset(train_all)
    lines = [f"{site}: heading offset {math.degrees(offset):+.1f} deg "
             f"(circular spread {math.degrees(spread):.1f} deg over {segments} segments)"]
    manifest, reports = [], []
    for name, plan_dir, _ in floors:
        entries, report = convert_floor(site, name, plan_dir, parsed[name], offset, args, args.out)
        report["heading_offset_deg"] = math.degrees(offset)
        report["heading_spread_deg"] = math.degrees(spread)
        lines.append(json.dumps({k: v for k, v in report.items() if k != "door_centres"}))
        manifest += entries
        reports.append(report)
    return manifest, reports, lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw", type=Path, default=HERE / "raw" / "data", help="folder with <site>/<floor>/ subfolders")
    ap.add_argument("--kaggle", type=Path,
                    help="folder of the full Kaggle competition data (train/, metadata/, test/) instead of --raw")
    ap.add_argument("--test-sites", action="store_true",
                    help="with --kaggle, only the sites of the competition's test set (sample_submission.csv)")
    ap.add_argument("--jobs", type=int, default=1, help="sites converted in parallel")
    ap.add_argument("--out", type=Path, default=HERE / "out")
    ap.add_argument("--sites", nargs="*", help="limit to these sites")
    ap.add_argument("--floors", nargs="*", help="limit to these floor names (e.g. F1 B1)")
    ap.add_argument("--wifi", action="store_true", help="also fit and replay Wi-Fi access points")
    ap.add_argument("--scans", action="store_true",
                    help="with --wifi, write each Wi-Fi scan as one batch (scan column, newest reading's time)")
    ap.add_argument("--fingerprints", action="store_true",
                    help="with --wifi, store the training scans with their positions in the venue, and keep the "
                         "access points the fit rejected as fingerprint-only beacons")
    ap.add_argument("--min-fingerprint-scans", type=int, default=10,
                    help="training scans an access point without a fitted transmitter must appear in")
    ap.add_argument("--walls", choices=["auto", "none", "boundary", "units", "doors"], default="auto",
                    help="doors: shop outlines with gaps where training walks cross them")
    ap.add_argument("--door-width", type=float, default=3.0, help="metres; gap cut per doors crossing")
    ap.add_argument("--test-every", type=int, default=5, help="every N-th path (by id hash) is a test path")
    ap.add_argument("--min-samples", type=int, default=30)
    ap.add_argument("--min-spread", type=float, default=3.0, help="metres; transmitters heard from one spot are dropped")
    ap.add_argument("--max-rms", type=float, default=8.0, help="dB; transmitters fitting worse are dropped")
    ap.add_argument("--wall-attenuation-db", type=float, default=3.0)
    args = ap.parse_args()
    if args.scans and not args.wifi:
        ap.error("--scans needs --wifi")
    if args.fingerprints and not args.wifi:
        ap.error("--fingerprints needs --wifi")
    if args.test_sites and not args.kaggle:
        ap.error("--test-sites needs --kaggle")

    jobs = [(site, floors, args) for site, floors in site_floors(args)]
    manifest, reports = [], []
    if args.jobs > 1:
        from concurrent.futures import ProcessPoolExecutor
        with ProcessPoolExecutor(args.jobs) as pool:
            results = pool.map(convert_site, jobs)
            for entries, site_reports, lines in results:
                print("\n".join(lines), flush=True)
                manifest += entries
                reports += site_reports
    else:
        for job in jobs:
            entries, site_reports, lines = convert_site(job)
            print("\n".join(lines), flush=True)
            manifest += entries
            reports += site_reports

    write_manifests(args.out, manifest)
    (args.out / "report.json").write_text(json.dumps(reports, indent=1))
    print(f"wrote {len(manifest)} traces to {args.out / 'manifest.json'}")


if __name__ == "__main__":
    main()
