"""Plane geometry on wall segments ([x1, y1, x2, y2] rows, metres) and phone orientation."""

from __future__ import annotations

import math

import numpy as np


def wrap(a):
    return (a + np.pi) % (2 * np.pi) - np.pi


def rotation_matrices(q: np.ndarray) -> np.ndarray:
    """Android rotation vector (x, y, z, w) -> phone-to-world matrices (world = east, north, up)."""
    x, y, z, w = q[:, 0], q[:, 1], q[:, 2], q[:, 3]
    r = np.empty((len(q), 3, 3))
    r[:, 0, 0] = 1 - 2 * y * y - 2 * z * z
    r[:, 0, 1] = 2 * x * y - 2 * z * w
    r[:, 0, 2] = 2 * x * z + 2 * y * w
    r[:, 1, 0] = 2 * x * y + 2 * z * w
    r[:, 1, 1] = 1 - 2 * x * x - 2 * z * z
    r[:, 1, 2] = 2 * y * z - 2 * x * w
    r[:, 2, 0] = 2 * x * z - 2 * y * w
    r[:, 2, 1] = 2 * y * z + 2 * x * w
    r[:, 2, 2] = 1 - 2 * x * x - 2 * y * y
    return r


def forward_heading_from_quat(q: np.ndarray) -> np.ndarray:
    """Heading of the phone's top edge (+y; walking direction when held flat in portrait),
    counter-clockwise from magnetic east."""
    r = rotation_matrices(q)
    return np.arctan2(r[:, 1, 1], r[:, 0, 1])


def fit_heading_offset(walks, min_len=4.0) -> tuple[float, float, int]:
    """Angle to add to the phone's magnetic heading to get the map heading: length-weighted circular
    mean over straight waypoint segments, with the circular spread as a sanity check.

    `walks` yields (t, psi, waypoints): heading samples psi at times t, and waypoint rows (t, x, y).
    A segment counts only if it is at least `min_len` metres and has 20 heading samples."""
    diffs, weights = [], []
    for t, psi, waypoints in walks:
        if len(t) == 0 or len(waypoints) < 2:
            continue
        for (t0, x0, y0), (t1, x1, y1) in zip(waypoints[:-1], waypoints[1:]):
            length = math.hypot(x1 - x0, y1 - y0)
            sel = (t >= t0) & (t <= t1)
            if length < min_len or sel.sum() < 20:
                continue
            mean_psi = math.atan2(np.sin(psi[sel]).mean(), np.cos(psi[sel]).mean())
            diffs.append(math.atan2(y1 - y0, x1 - x0) - mean_psi)
            weights.append(length)
    if not diffs:
        return 0.0, float("nan"), 0
    d, w = np.array(diffs), np.array(weights)
    s, c = np.sum(w * np.sin(d)), np.sum(w * np.cos(d))
    resultant = math.hypot(s, c) / w.sum()
    spread = math.sqrt(max(-2.0 * math.log(max(resultant, 1e-12)), 0.0))
    return math.atan2(s, c), spread, len(diffs)


def ring_segments(rings) -> np.ndarray:
    segs = []
    for ring in rings:
        for a, b in zip(ring[:-1], ring[1:]):
            if np.hypot(*(b - a)) > 1e-3:
                segs.append([a[0], a[1], b[0], b[1]])
    return np.array(segs) if segs else np.zeros((0, 4))


def dedupe_segments(segs: np.ndarray, tol: float = 0.05, max_angle_deg: float = 1.0) -> np.ndarray:
    """Merges collinear segments that overlap or touch (within `tol` metres). Neighbouring shops
    share edges, and a shared edge listed twice would count twice in the radio model's wall
    attenuation."""
    if len(segs) == 0:
        return segs
    cos_max = math.cos(math.radians(max_angle_deg))
    origins, dirs, intervals = [], [], []
    o_arr = np.zeros((len(segs), 2))
    d_arr = np.zeros((len(segs), 2))
    for a0, a1, b0, b1 in segs:
        a, b = np.array([a0, a1]), np.array([b0, b1])
        length = float(np.hypot(*(b - a)))
        if length < tol:
            continue
        d = (b - a) / length
        n = len(origins)
        match = -1
        if n:
            parallel = np.abs(d_arr[:n] @ d) >= cos_max
            rel_a, rel_b = a - o_arr[:n], b - o_arr[:n]
            off_a = np.abs(rel_a[:, 0] * d_arr[:n, 1] - rel_a[:, 1] * d_arr[:n, 0])
            off_b = np.abs(rel_b[:, 0] * d_arr[:n, 1] - rel_b[:, 1] * d_arr[:n, 0])
            hits = np.flatnonzero(parallel & (off_a < tol) & (off_b < tol))
            if len(hits):
                match = int(hits[0])
        if match < 0:
            origins.append(a)
            dirs.append(d)
            o_arr[n], d_arr[n] = a, d
            intervals.append([])
            match = n
        ta, tb = float((a - origins[match]) @ dirs[match]), float((b - origins[match]) @ dirs[match])
        intervals[match].append((min(ta, tb), max(ta, tb)))
    out = []
    for o, d, ivs in zip(origins, dirs, intervals):
        ivs.sort()
        lo, hi = ivs[0]
        for s, e in ivs[1:]:
            if s <= hi + tol:
                hi = max(hi, e)
                continue
            out.append([*(o + lo * d), *(o + hi * d)])
            lo, hi = s, e
        out.append([*(o + lo * d), *(o + hi * d)])
    return np.array(out)


def _orient(u, v, w):
    return (v[..., 0] - u[..., 0]) * (w[..., 1] - u[..., 1]) - (v[..., 1] - u[..., 1]) * (w[..., 0] - u[..., 0])


def crossing_params(p, q, walls: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Indices of the wall segments properly intersected by segment p-q, and where along each
    wall (0 at its first endpoint, 1 at its second) the intersection lies."""
    if len(walls) == 0:
        return np.zeros(0, dtype=int), np.zeros(0)
    a, b = walls[:, :2], walls[:, 2:]
    p = np.asarray(p)[None, :]
    q = np.asarray(q)[None, :]
    d1, d2 = _orient(a, b, p), _orient(a, b, q)
    d3, d4 = _orient(p, q, a), _orient(p, q, b)
    idx = np.flatnonzero((d1 * d2 < 0) & (d3 * d4 < 0))
    return idx, d3[idx] / (d3[idx] - d4[idx])


def crossings(p, q, walls: np.ndarray) -> int:
    """Number of wall segments properly intersected by segment p-q."""
    return len(crossing_params(p, q, walls)[0])


def point_in_rings(pt, rings) -> bool:
    """Even-odd rule over all rings (outer rings and holes)."""
    x, y = pt
    inside = False
    for ring in rings:
        xs, ys = ring[:, 0], ring[:, 1]
        x0, y0, x1, y1 = xs[:-1], ys[:-1], xs[1:], ys[1:]
        cond = (y0 > y) != (y1 > y)
        with np.errstate(divide="ignore", invalid="ignore"):
            xint = x0 + (y - y0) * (x1 - x0) / (y1 - y0)
        inside ^= bool(np.sum(cond & (x < xint)) % 2)
    return inside


def cut_gaps(walls: np.ndarray, cuts: dict[int, list[float]], width: float, min_piece: float = 0.1):
    """Opens a gap `width` metres wide on wall i centred at each fraction u in cuts[i] (0 at the
    wall's first endpoint). Returns the cut walls and the centre of each merged gap."""
    out, centres = [], []
    for i, w in enumerate(walls):
        if i not in cuts:
            out.append(w)
            continue
        a, b = w[:2], w[2:]
        length = float(np.hypot(*(b - a)))
        holes = sorted((u * length - width / 2, u * length + width / 2) for u in cuts[i])
        merged = [list(holes[0])]
        for s, e in holes[1:]:
            if s <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], e)
            else:
                merged.append([s, e])
        centres += [[*(a + (b - a) * ((s + e) / 2 / length))] for s, e in merged]
        pos = 0.0
        for s, e in merged + [[length, length]]:
            if s - pos >= min_piece:
                p0, p1 = a + (b - a) * (pos / length), a + (b - a) * (min(s, length) / length)
                out.append([*p0, *p1])
            pos = max(pos, e)
    return (np.array(out) if out else np.zeros((0, 4))), centres
