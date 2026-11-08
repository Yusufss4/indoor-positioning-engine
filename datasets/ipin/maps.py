"""Floor plans to walls: georeferencing the competition's map images and extracting wall segments.

Frame. The local frame of a site is the map image's own axes in metres: x to the right, y up, origin
at the image's bottom-left corner, so the building's walls come out axis-aligned. The image-to-WGS84
similarity (scale, rotation, offset) is fitted on the reference markers drawn on the maps (coloured
dots at the surveyed points in `references.csv` / `calibration_points.csv`); the calibration file's
centre/rotation/scale only seeds the marker search. On the 2023/2024 maps the fit leaves 1 cm
residuals where the calibration file is off by up to 0.6 m.

Walls. Each floor becomes a boolean wall mask on a 5 cm grid in the local frame:
- raster plans (Nuremberg): dark pixels, with marker labels erased, opened by a disk of about 9 cm so
  that text, dimension and grid lines and furniture (thin strokes) drop out and walls (thick black)
  stay;
- vector plans (UMinho PDF): every straight path segment drawn one cell wide, then closed by 15 cm so
  that the two outlines of a wall become one strip. Curves (door leaves, round columns) are skipped.
Edits (`edits/<site>.json`) erase polygons from the mask (stair treads, false walls, labels) before
it is vectorised; connector areas are erased too. Thick regions (over 0.6 m, the solid black areas
you cannot enter on the Nuremberg plans) contribute their outline; the rest is skeletonised and the
skeleton traced into polylines, simplified to 5 cm and split into segments.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

from common.georef import GeoRef
from common.geom import dedupe_segments

Image.MAX_IMAGE_PIXELS = None
CELL = 0.05  # metres per wall-mask cell


# --------------------------------------------------------------------------------------------
# Georeferencing


@dataclass
class Calib:
    """One floor image of the calibration file: centre, rotation (deg) and scale (m/px)."""
    image: str
    floor: int
    lat: float
    lon: float
    rotation_deg: float
    scale: float


def read_calib(path: Path) -> list[Calib]:
    rows = [ln.split() for ln in path.read_text(errors="replace").splitlines() if ln.strip() and not ln.startswith("%")]
    names, floors, _, lats, lons, rots, scales = rows[:7]
    return [Calib(n, int(f), float(la), float(lo), float(r), float(s))
            for n, f, la, lo, r, s in zip(names, floors, lats, lons, rots, scales)]


def read_references(path: Path) -> dict[str, tuple[float, float]]:
    """Reference points as {id: (lat, lon)}; both the `pointID,Lat,Lon` and the
    `X_Longitude,Y_Latitude,PointID` layouts."""
    lines = [ln.strip() for ln in path.read_text().splitlines() if ln.strip()]
    head = [h.strip().strip('"').lower() for h in lines[0].split(",")]
    out = {}
    for ln in lines[1:]:
        f = dict(zip(head, [v.strip().strip('"') for v in ln.split(",")]))
        pid = f.get("pointid")
        lat = f.get("lat", f.get("y_latitude"))
        lon = f.get("lon", f.get("x_longitude"))
        out[pid] = (float(lat), float(lon))
    return out


@dataclass
class Frame:
    """Image pixels (u right, v down) <-> local metres (x right, y up) <-> WGS84."""
    scale: float  # metres per pixel
    width_px: float
    height_px: float
    georef: GeoRef

    def to_local(self, u, v):
        return np.asarray(u, dtype=float) * self.scale, (self.height_px - np.asarray(v, dtype=float)) * self.scale

    @property
    def size_m(self) -> tuple[float, float]:
        return self.width_px * self.scale, self.height_px * self.scale


def similarity_fit(uv: np.ndarray, en: np.ndarray) -> tuple[float, float, np.ndarray]:
    """[e, n] = s R(th) [u, -v] + t, least squares. Returns s, th, t."""
    U = np.column_stack([uv[:, 0], -uv[:, 1]])
    mu, me = U.mean(0), en.mean(0)
    X, Y = U - mu, en - me
    a = float((X * Y).sum())
    b = float((X[:, 0] * Y[:, 1] - X[:, 1] * Y[:, 0]).sum())
    th = math.atan2(b, a)
    s = (math.cos(th) * a + math.sin(th) * b) / float((X ** 2).sum())
    r = np.array([[math.cos(th), -math.sin(th)], [math.sin(th), math.cos(th)]])
    return s, th, me - s * r @ mu


def calib_predict(c: Calib, w: int, h: int, en_of, latlon) -> tuple[float, float]:
    """Pixel where the calibration file puts (lat, lon); en_of maps lat/lon to east/north metres."""
    e0, n0 = en_of(c.lat, c.lon)
    e, n = en_of(*latlon)
    th = math.radians(-c.rotation_deg)
    de, dn = e - e0, n - n0
    x = (de * math.cos(th) + dn * math.sin(th)) / c.scale
    y = (-de * math.sin(th) + dn * math.cos(th)) / c.scale
    return w / 2 + x, h / 2 - y


def find_markers(rgb: np.ndarray, min_px: int, max_px: int) -> np.ndarray:
    """Centroids (u, v) of saturated colour blobs: the reference and beacon dots on the plans."""
    sat = (rgb.max(axis=2).astype(int) - rgb.min(axis=2).astype(int)) > 40
    lab, n = ndimage.label(sat)
    if n == 0:
        return np.zeros((0, 2))
    sizes = ndimage.sum(sat, lab, range(1, n + 1))
    keep = [i + 1 for i, s in enumerate(sizes) if min_px <= s <= max_px]
    if not keep:
        return np.zeros((0, 2))
    com = ndimage.center_of_mass(sat, lab, keep)
    return np.array([[c[1] + 0.5, c[0] + 0.5] for c in com])


def fit_frame(images: list[Path], calib: list[Calib], refs: dict[str, tuple[float, float]],
              marker_px: tuple[int, int], max_match_m: float = 3.0) -> tuple[Frame, dict]:
    """Fits the site frame on the reference markers of every floor image (all images of a site share
    one frame)."""
    lat0, lon0 = next(iter(refs.values()))
    en_ref = GeoRef(lat0, lon0, 0.0)

    def en_of(lat, lon):
        x, y = en_ref.to_local(lat, lon)
        return float(x), float(y)

    uv_all, en_all, ids = [], [], []
    w = h = 0
    for img_path in images:
        c = next(c for c in calib if c.image == img_path.name or Path(c.image).stem in img_path.stem)
        rgb = np.asarray(Image.open(img_path).convert("RGB"))
        h, w = rgb.shape[:2]
        blobs = find_markers(rgb, *marker_px)
        for pid, ll in refs.items():
            pu, pv = calib_predict(c, w, h, en_of, ll)
            if len(blobs) == 0:
                continue
            d = np.hypot(blobs[:, 0] - pu, blobs[:, 1] - pv) * c.scale
            k = int(np.argmin(d))
            if d[k] < max_match_m:
                uv_all.append(blobs[k])
                en_all.append(en_of(*ll))
                ids.append(pid)
    if len(set(ids)) < 3:
        raise SystemExit(f"only {len(set(ids))} reference markers found; cannot georeference {images[0].parent}")
    uv, en = np.array(uv_all), np.array(en_all)
    s, th, t = similarity_fit(uv, en)
    r = np.array([[math.cos(th), -math.sin(th)], [math.sin(th), math.cos(th)]])
    res = np.hypot(*((s * (r @ np.column_stack([uv[:, 0], -uv[:, 1]]).T)).T + t - en).T)
    # Local origin = bottom-left image corner = pixel (0, h).
    e0, n0 = s * r @ np.array([0.0, -float(h)]) + t
    lat, lon = en_ref.to_wgs84(e0, n0)
    frame = Frame(s, float(w), float(h), GeoRef(float(lat), float(lon), math.degrees(th)))
    report = {"markers": len(ids), "unique_points": len(set(ids)), "scale_m_per_px": s,
              "rotation_deg": math.degrees(th), "residual_max_m": float(res.max()),
              "calib_scale_m_per_px": calib[0].scale, "calib_rotation_deg": -calib[0].rotation_deg}
    return frame, report


# --------------------------------------------------------------------------------------------
# Wall masks


class Grid:
    """CELL-sized grid over the local frame; row 0 at y = 0 (bottom)."""

    def __init__(self, width_m: float, height_m: float):
        self.nx = int(math.ceil(width_m / CELL))
        self.ny = int(math.ceil(height_m / CELL))

    def cell(self, x, y):
        return np.asarray(y) / CELL, np.asarray(x) / CELL  # fractional (row, col)

    def xy(self, row, col):
        return (np.asarray(col) + 0.5) * CELL, (np.asarray(row) + 0.5) * CELL


def disk(radius_cells: float) -> np.ndarray:
    r = int(math.ceil(radius_cells))
    yy, xx = np.mgrid[-r:r + 1, -r:r + 1]
    return xx * xx + yy * yy <= radius_cells * radius_cells


def raster_mask(img_path: Path, frame: Frame, grid: Grid, dark_level: int = 110, min_wall_m: float = 0.09,
                erase_px: list[tuple[float, float, float, float]] = ()) -> np.ndarray:
    """Thick dark strokes of a raster plan, resampled to the grid."""
    gray = np.asarray(Image.open(img_path).convert("L"))
    dark = gray < dark_level
    for u0, v0, u1, v1 in erase_px:
        dark[max(int(v0), 0):max(int(v1), 0), max(int(u0), 0):max(int(u1), 0)] = False
    r_px = 0.5 * min_wall_m / frame.scale
    dark = ndimage.binary_opening(dark, structure=disk(r_px))
    # Resample: the grid is axis-aligned with the image (both in image axes), only the scale differs.
    # Row 0 of the grid is the image's bottom row.
    img = Image.fromarray((dark[::-1] * 255).astype(np.uint8))
    box = (0, 0, min(grid.nx * CELL / frame.scale, img.size[0]), min(grid.ny * CELL / frame.scale, img.size[1]))
    small = np.asarray(img.resize((grid.nx, grid.ny), Image.BOX, box=box))
    return small > 64


def pdf_door_arcs(segs, scale: float, max_piece_m: float = 0.2, min_run: int = 4,
                  min_size_m: float = 0.5, max_size_m: float = 2.0) -> list[tuple[float, float, float, float]]:
    """Bounding boxes (page units) of door swings: runs of at least `min_run` consecutive short
    pieces (an arc drawn as a polyline) whose box is door-sized. A swing's box spans the doorway along
    the wall it closes, so clearing it opens the door."""
    boxes, run = [], []

    def flush():
        if len(run) >= min_run:
            xs = [v for s in run for v in (s[0], s[2])]
            ys = [v for s in run for v in (s[1], s[3])]
            w, h = (max(xs) - min(xs)) * scale, (max(ys) - min(ys)) * scale
            if min_size_m <= max(w, h) <= max_size_m:
                boxes.append((min(xs), min(ys), max(xs), max(ys)))
        run.clear()

    prev_end = None
    for sg in segs:
        short = math.hypot(sg[2] - sg[0], sg[3] - sg[1]) * scale < max_piece_m
        joined = prev_end is not None and math.hypot(sg[0] - prev_end[0], sg[1] - prev_end[1]) < 1e-6
        if not (short and (joined or not run)):
            flush()
        if short:
            run.append(sg)
        prev_end = (sg[2], sg[3])
    flush()
    return boxes


def pdf_segments(pdf_path: Path) -> tuple[list[tuple[float, float, float, float]], tuple[float, float]]:
    """Straight segments of every stroked path on the page, in page units with y down (the PNG's
    pixels for these plans), in drawing order."""
    import pdfplumber

    segs = []
    with pdfplumber.open(pdf_path) as pdf:
        page = pdf.pages[0]
        size = (float(page.width), float(page.height))
        for o in page.lines + page.curves + page.rects:
            if not o.get("stroke", True):
                continue
            path = o.get("path") or ([("m", o["pts"][0])] + [("l", q) for q in o["pts"][1:]])
            start = prev = None
            for cmd in path:
                op = cmd[0]
                if op == "m":
                    start = prev = cmd[1]
                elif op == "l":
                    segs.append((prev[0], prev[1], cmd[1][0], cmd[1][1]))
                    prev = cmd[1]
                elif op == "c":
                    prev = cmd[-1]
                elif op == "h" and prev is not None and start is not None and prev != start:
                    segs.append((prev[0], prev[1], start[0], start[1]))
                    prev = start
    return segs, size


def vector_mask(segs_px, frame: Frame, grid: Grid, close_m: float = 0.15, min_len_m: float = 0.0) -> np.ndarray:
    img = Image.new("1", (grid.nx, grid.ny), 0)
    d = ImageDraw.Draw(img)
    for u0, v0, u1, v1 in segs_px:
        x0, y0 = frame.to_local(u0, v0)
        x1, y1 = frame.to_local(u1, v1)
        if math.hypot(x1 - x0, y1 - y0) < min_len_m:
            continue
        d.line([float(x0) / CELL, float(y0) / CELL, float(x1) / CELL, float(y1) / CELL], fill=1, width=1)
    mask = np.asarray(img, dtype=bool)
    return ndimage.binary_closing(mask, structure=disk(close_m / CELL))


def erase_polygons(mask: np.ndarray, polys: list[list[list[float]]]):
    if not polys:
        return mask
    img = Image.new("1", (mask.shape[1], mask.shape[0]), 0)
    d = ImageDraw.Draw(img)
    for poly in polys:
        d.polygon([(x / CELL, y / CELL) for x, y in poly], fill=1, outline=1)
    return mask & ~np.asarray(img, dtype=bool)


# --------------------------------------------------------------------------------------------
# Vectorising


def _trace_skeleton(skel: np.ndarray) -> list[np.ndarray]:
    """Splits a one-pixel skeleton into polylines (row, col) between end points and junctions."""
    rows, cols = np.nonzero(skel)
    pix = set(zip(rows.tolist(), cols.tolist()))
    nbrs = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]

    def neighbours(p):
        return [(p[0] + a, p[1] + b) for a, b in nbrs if (p[0] + a, p[1] + b) in pix]

    degree = {p: len(neighbours(p)) for p in pix}
    nodes = {p for p, k in degree.items() if k != 2}
    used = set()  # undirected edges (p, q) with p < q
    lines = []

    def walk(a, b):
        path = [a, b]
        used.add((min(a, b), max(a, b)))
        prev, cur = a, b
        while cur not in nodes:
            nxt = [q for q in neighbours(cur) if q != prev and (min(cur, q), max(cur, q)) not in used]
            if not nxt:
                break
            prev, cur = cur, nxt[0]
            used.add((min(prev, cur), max(prev, cur)))
            path.append(cur)
        return path

    for n in nodes:
        for q in neighbours(n):
            if (min(n, q), max(n, q)) not in used:
                lines.append(np.array(walk(n, q), dtype=float))
    # Closed loops without nodes.
    for p in pix:
        for q in neighbours(p):
            if (min(p, q), max(p, q)) not in used:
                lines.append(np.array(walk(p, q), dtype=float))
    return lines


def _simplify(line: np.ndarray, tol: float) -> np.ndarray:
    from skimage.measure import approximate_polygon

    return approximate_polygon(line, tolerance=tol)


def vectorise(mask: np.ndarray, solid_m: float = 0.6, tol_m: float = 0.06, min_piece_m: float = 0.25) -> np.ndarray:
    """Wall segments [x1, y1, x2, y2] (local metres) from a wall mask."""
    from skimage.measure import find_contours
    from skimage.morphology import skeletonize

    solid = ndimage.binary_opening(mask, structure=disk(0.5 * solid_m / CELL))
    solid = ndimage.binary_propagation(solid, mask=mask) & ndimage.binary_dilation(solid, structure=disk(2))
    thin = mask & ~ndimage.binary_dilation(solid, structure=disk(1))
    lines = []
    if solid.any():
        padded = np.pad(solid, 1)
        for c in find_contours(padded.astype(float), 0.5):
            lines.append(c - 1.0)
    skel = skeletonize(thin)
    lines += _trace_skeleton(skel)
    segs = []
    tol = tol_m / CELL
    for ln in lines:
        if len(ln) < 2:
            continue
        length = float(np.sum(np.hypot(*np.diff(ln, axis=0).T))) * CELL
        if length < min_piece_m:
            continue
        pts = _simplify(ln, tol)
        for (r0, c0), (r1, c1) in zip(pts[:-1], pts[1:]):
            segs.append([(c0 + 0.5) * CELL, (r0 + 0.5) * CELL, (c1 + 0.5) * CELL, (r1 + 0.5) * CELL])
    segs = np.array(segs) if segs else np.zeros((0, 4))
    return dedupe_segments(segs, tol=0.03)


# --------------------------------------------------------------------------------------------
# Edits and debug output


def load_edits(path: Path) -> dict:
    return json.loads(path.read_text()) if path.exists() else {}


def debug_png(path: Path, mask: np.ndarray, walls: np.ndarray, points: list[tuple[float, float, str]],
              polys: list[tuple[list, str]], scale: int = 2):
    """Mask in grey, walls in red, points and polygons on top; 10 cm per pixel at scale 2."""
    f = 1.0 / (CELL * scale)
    ny, nx = mask.shape
    small = Image.fromarray((~mask[::-1] * 255).astype(np.uint8)).resize((nx // scale, ny // scale), Image.BOX)
    img = Image.merge("RGB", [small.point(lambda v: 160 + v * 95 // 255)] * 3)
    d = ImageDraw.Draw(img)
    h = img.size[1]
    for poly, colour in polys:
        d.polygon([(x * f, h - y * f) for x, y in poly], outline=colour)
    for x1, y1, x2, y2 in walls:
        d.line([x1 * f, h - y1 * f, x2 * f, h - y2 * f], fill=(220, 0, 0), width=1)
    for x, y, colour in points:
        d.ellipse([x * f - 3, h - y * f - 3, x * f + 3, h - y * f + 3], outline=colour)
    img.save(path)
