#!/usr/bin/env python3
"""OpenStreetMap indoor data (Simple Indoor Tagging) to GeoJSON in the scheme `ipe-import geojson` reads.

OSM needs geometry work that a mapping table cannot express, so this script does it before the importer:

- `indoor=room` outlines (closed ways and multipolygons) become `room` features on every level they list;
  `indoor=wall` ways become `wall` features. Corridors and areas only have walls where rooms border them,
  so they are not walls themselves. The building outline is a wall on every level.
- Doors are nodes on room outlines. Each becomes `opening` segments reaching half its `width` (default
  1 m) along both outline edges at the node, so the importer cuts a doorway into those walls.
- Closed stairs (`highway=steps`, `stairs=yes`, `room=stairs`, `indoor=stairs`) and lift
  (`highway=elevator`, `room=elevator`) footprints that list two or more levels become connectors. Ones on
  a single level (a few steps inside a floor) are skipped.
- Beacons go on a regular grid, keeping the points that fall inside a mapped room, corridor or area of
  that level, as if they were mounted in the building.

The Overpass response is kept as a snapshot (data/osm/<name>.osm.json) so the conversion is reproducible;
--fetch refreshes it. OSM data is (c) OpenStreetMap contributors, ODbL 1.0 (see data/osm/LICENSE).

    python3 datasets/osm/convert.py --fetch --out /tmp/kit.geojson   # refresh the snapshot from Overpass
    python3 datasets/osm/convert.py --out /tmp/kit.geojson
    ./build/tools/ipe-import geojson /tmp/kit.geojson --name kit_informatikbau --level-height 3.5 \\
        --min-wall 0.2 --out data/venues/kit_informatikbau.json

The full pipeline, including the planned walk, is in docs/formats.md.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
import urllib.parse
import urllib.request
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OVERPASS = "https://overpass-api.de/api/interpreter"

# KIT Informatikbau (building 50.34), Karlsruhe: one of the most completely mapped indoor buildings on the
# KIT campus (rooms, doors, stairwells and lifts on levels -2 to 4).
DEFAULT_WAY = 155674961
DEFAULT_NAME = "kit_informatikbau"
DOOR_SNAP_M = 0.1  # a door node this close to an outline edge is a doorway in it

QUERY = """[out:json][timeout:160];
way({way})->.b; .b out body geom; .b map_to_area->.a;
(nwr(area.a)["indoor"]; node(area.a)["door"]; nwr(area.a)["highway"~"^(elevator|steps)$"];
 nwr(area.a)["stairs"]; nwr(area.a)["room"];);
out body geom;"""


def fetch(way: int) -> dict:
    data = urllib.parse.urlencode({"data": QUERY.format(way=way)}).encode()
    req = urllib.request.Request(OVERPASS, data=data,
                                 headers={"User-Agent": "indoor-positioning-engine (datasets/osm/convert.py)"})
    with urllib.request.urlopen(req, timeout=200) as res:
        return json.loads(res.read())


def parse_levels(value: str | None) -> list[int]:
    """'0', '-1;0;1' or '-2-1' (a range) -> ordinals; non-integer levels (mezzanines) are dropped."""
    if not value:
        return []
    out: set[int] = set()
    for part in value.split(";"):
        part = part.strip()
        m = re.fullmatch(r"(-?\d+)-(-?\d+)", part)
        if m:
            lo, hi = sorted((int(m.group(1)), int(m.group(2))))
            out.update(range(lo, hi + 1))
        elif re.fullmatch(r"-?\d+", part):
            out.add(int(part))
    return sorted(out)


def parse_width(value: str | None, default: float) -> float:
    m = re.match(r"\s*([0-9.]+)\s*(cm|m)?", value or "")
    if not m:
        return default
    w = float(m.group(1)) / (100.0 if m.group(2) == "cm" else 1.0)
    return w if 0.5 <= w <= 4.0 else default


def lonlat(geometry: list[dict]) -> list[list[float]]:
    return [[p["lon"], p["lat"]] for p in geometry]


def closed(ring: list[list[float]]) -> bool:
    return len(ring) >= 4 and ring[0] == ring[-1]


def assemble_rings(members: list[dict], role: str) -> list[list[list[float]]]:
    """Joins the member ways of a multipolygon relation into closed rings."""
    pieces = [lonlat(m["geometry"]) for m in members if m.get("role") == role and m.get("geometry")]
    rings = []
    while pieces:
        ring = pieces.pop(0)
        while not closed(ring) and pieces:
            for i, p in enumerate(pieces):
                if p[0] == ring[-1]:
                    ring += p[1:]
                elif p[-1] == ring[-1]:
                    ring += p[::-1][1:]
                else:
                    continue
                pieces.pop(i)
                break
            else:
                break
        if closed(ring):
            rings.append(ring)
    return rings


def polygons(e: dict) -> list[list[list[float]]]:
    """Closed outer rings of a way or multipolygon relation (holes are not walls we need to model)."""
    if e["type"] == "way" and e.get("geometry"):
        ring = lonlat(e["geometry"])
        return [ring] if closed(ring) else []
    if e["type"] == "relation":
        return assemble_rings(e.get("members", []), "outer") + assemble_rings(e.get("members", []), "inner")
    return []


def inside(pt: tuple[float, float], ring: list[list[float]]) -> bool:
    x, y = pt
    hit = False
    for (x1, y1), (x2, y2) in zip(ring, ring[1:]):
        if (y1 > y) != (y2 > y) and x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
            hit = not hit
    return hit


def feature(geom_type: str, coords, **props) -> dict:
    return {"type": "Feature", "properties": props, "geometry": {"type": geom_type, "coordinates": coords}}


def is_stairs(t: dict) -> bool:
    return (t.get("highway") == "steps" or t.get("stairs") == "yes" or t.get("room") == "stairs"
            or t.get("indoor") == "stairs")


def is_elevator(t: dict) -> bool:
    return t.get("highway") == "elevator" or t.get("room") == "elevator"


def convert(osm: dict, name: str, way: int, door_width: float, beacon_grid: float) -> tuple[dict, Counter]:
    elements = osm["elements"]
    building = next(e for e in elements if e["type"] == "way" and e["id"] == way)
    stats: Counter = Counter()
    features = [feature("Polygon", [lonlat(building["geometry"])], feature_type="venue",
                        name=building["tags"].get("name", name))]

    rooms_by_level: dict[int, list] = defaultdict(list)  # ring -> door lookup and beacon placement
    spaces_by_level: dict[int, list] = defaultdict(list)
    used_levels: set[int] = set()
    for e in elements:
        t = e.get("tags", {})
        if e is building or "level" not in t:
            continue
        levels = parse_levels(t.get("level"))
        if not levels:
            stats["skipped: no integer level"] += 1
            continue
        rings = polygons(e)
        connector = "stairs" if is_stairs(t) else "elevator" if is_elevator(t) else None
        if connector and e["type"] == "way" and rings:
            if len(levels) >= 2:
                features.append(feature("Polygon", [rings[0]], feature_type=connector, id=f"osm_w{e['id']}",
                                        level=levels[0], levels=levels))
                stats[connector] += 1
                used_levels.update(levels)
            else:
                stats[f"skipped: {connector} on one level"] += 1
        indoor = t.get("indoor")
        if indoor == "room" and rings:
            for lv in levels:
                for ring in rings:
                    features.append(feature("Polygon", [ring], feature_type="room", level=lv))
                    rooms_by_level[lv].append((e, ring))
                    spaces_by_level[lv].append(ring)
            stats["room"] += 1
            used_levels.update(levels)
        elif indoor in ("corridor", "area") and rings:
            for lv in levels:
                spaces_by_level[lv].extend(rings)
            stats[indoor] += 1
        elif indoor == "wall" and e["type"] == "way" and e.get("geometry"):
            for lv in levels:
                features.append(feature("LineString", lonlat(e["geometry"]), feature_type="wall", level=lv))
            stats["wall"] += 1
            used_levels.update(levels)

    # Doors: a segment of the door's width, centred on the node, along every outline edge it lies on.
    # Matched by geometry rather than node ids, because Overpass gives relation members without ids.
    lat0 = math.radians(building["geometry"][0]["lat"])
    m_per_deg = (111_320.0 * math.cos(lat0), 110_540.0)
    edges_by_level: dict[int, list] = defaultdict(list)
    for lv in used_levels:
        rings = [ring for _, ring in rooms_by_level[lv]] + [lonlat(building["geometry"])]
        for ring in rings:
            for a, b in zip(ring, ring[1:]):
                edges_by_level[lv].append(((a[0] * m_per_deg[0], a[1] * m_per_deg[1]),
                                           (b[0] * m_per_deg[0], b[1] * m_per_deg[1])))
    for e in elements:
        t = e.get("tags", {})
        if e["type"] != "node" or "door" not in t or t["door"] == "no":
            continue
        half = 0.5 * parse_width(t.get("width"), door_width)
        p = (e["lon"] * m_per_deg[0], e["lat"] * m_per_deg[1])
        cut = 0
        for lv in parse_levels(t.get("level")):
            dirs = []
            for a, b in edges_by_level.get(lv, []):
                dx, dy = b[0] - a[0], b[1] - a[1]
                d2 = dx * dx + dy * dy
                if d2 < 1e-6:
                    continue
                s = max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / d2))
                if math.hypot(a[0] + s * dx - p[0], a[1] + s * dy - p[1]) > DOOR_SNAP_M:
                    continue
                u = (dx / math.sqrt(d2), dy / math.sqrt(d2))
                if any(abs(u[0] * v[1] - u[1] * v[0]) < 0.05 for v in dirs):
                    continue  # the same wall direction, already cut
                dirs.append(u)
                ends = [[(p[0] + k * half * u[0]) / m_per_deg[0], (p[1] + k * half * u[1]) / m_per_deg[1]]
                        for k in (-1, 1)]
                features.append(feature("LineString", ends, feature_type="opening", level=lv))
                cut += 1
        stats["door" if cut else "skipped: door not on a room or building outline"] += 1

    # The building outline is a wall on every level with indoor data.
    for lv in sorted(used_levels):
        features.append(feature("Polygon", [lonlat(building["geometry"])], feature_type="room", level=lv))

    # Beacons on a grid, inside mapped spaces only.
    if beacon_grid > 0:
        lons = [p["lon"] for p in building["geometry"]]
        lats = [p["lat"] for p in building["geometry"]]
        step_lon, step_lat = beacon_grid / m_per_deg[0], beacon_grid / m_per_deg[1]
        for lv in sorted(used_levels):
            k = 0
            lat = min(lats) + 0.5 * step_lat
            while lat < max(lats):
                lon = min(lons) + 0.5 * step_lon
                while lon < max(lons):
                    if any(inside((lon, lat), r) for r in spaces_by_level[lv]):
                        k += 1
                        features.append(feature("Point", [round(lon, 8), round(lat, 8)], feature_type="beacon",
                                                id=f"B{lv}-{k}", level=lv))
                    lon += step_lon
                lat += step_lat
            stats[f"beacons on level {lv}"] = k
    return {"type": "FeatureCollection", "features": features}, stats


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--way", type=int, default=DEFAULT_WAY, help="OSM way id of the building outline")
    ap.add_argument("--name", default=DEFAULT_NAME)
    ap.add_argument("--snapshot", type=Path, help="Overpass response (default data/osm/<name>.osm.json)")
    ap.add_argument("--fetch", action="store_true", help="download a fresh snapshot from Overpass first")
    ap.add_argument("--door-width", type=float, default=1.0, help="doorway width when a door has no width tag [m]")
    ap.add_argument("--beacon-grid", type=float, default=10.0, help="beacon spacing [m]; 0 for none")
    ap.add_argument("--out", type=Path, required=False, help="GeoJSON output (default stdout)")
    args = ap.parse_args()

    snapshot = args.snapshot or ROOT / "data" / "osm" / f"{args.name}.osm.json"
    if args.fetch:
        osm = fetch(args.way)
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        snapshot.write_text(json.dumps(osm, indent=0, sort_keys=True) + "\n")
        print(f"fetched {len(osm['elements'])} elements into {snapshot}", file=sys.stderr)
    osm = json.loads(snapshot.read_text())
    fc, stats = convert(osm, args.name, args.way, args.door_width, args.beacon_grid)
    for k, v in sorted(stats.items()):
        print(f"  {k}: {v}", file=sys.stderr)
    text = json.dumps(fc, separators=(",", ":")) + "\n"
    if args.out:
        args.out.write_text(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
