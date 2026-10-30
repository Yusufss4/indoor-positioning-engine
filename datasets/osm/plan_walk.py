#!/usr/bin/env python3
"""Plans a walk through a venue and writes it as a scenario, for venues too detailed to route by hand.

Goals are `x,y,level` in the venue's local frame; `@CONNECTOR_ID` between two goals on different levels
takes that stairwell or lift. Each leg is an A* path on a 0.2 m grid that keeps --clearance away from
walls and prefers the middle of corridors; the path is then shortened to the waypoints the walker needs,
keeping every straight segment at least --clearance from walls. Stairs are climbed along their long axis;
lifts are entered and left at the cabin centre. `ipe-sim` checks the result (exit code 3 on a wall
crossing).

    python3 datasets/osm/plan_walk.py --venue data/venues/kit_informatikbau.json \\
        --goals 60,12,1 10,30,1 @osm_w1120093940 10,32,2 ... --name kit_tour --out data/scenarios/kit_tour.json
"""

from __future__ import annotations

import argparse
import heapq
import json
import math
import os
from pathlib import Path

import numpy as np
from scipy import ndimage

RES = 0.2


class Level:
    def __init__(self, walls: list[list[float]], bounds: list[float]):
        self.x0, self.y0 = bounds[0] - 1.0, bounds[1] - 1.0
        self.nx = int(math.ceil((bounds[2] - bounds[0] + 2.0) / RES))
        self.ny = int(math.ceil((bounds[3] - bounds[1] + 2.0) / RES))
        wall = np.zeros((self.ny, self.nx), dtype=bool)
        for x1, y1, x2, y2 in walls:
            n = max(2, int(math.hypot(x2 - x1, y2 - y1) / (0.25 * RES)) + 1)
            for t in np.linspace(0.0, 1.0, n):
                i, j = self.cell(x1 + t * (x2 - x1), y1 + t * (y2 - y1))
                if 0 <= i < self.ny and 0 <= j < self.nx:
                    wall[i, j] = True
        # Distance from each cell centre to the nearest wall, less half a cell for the rasterisation.
        self.dist = np.maximum(ndimage.distance_transform_edt(~wall) * RES - 0.5 * RES, 0.0)

    def cell(self, x: float, y: float) -> tuple[int, int]:
        return int((y - self.y0) / RES), int((x - self.x0) / RES)

    def pos(self, i: int, j: int) -> tuple[float, float]:
        return self.x0 + (j + 0.5) * RES, self.y0 + (i + 0.5) * RES

    def clearance(self, x: float, y: float) -> float:
        i, j = self.cell(x, y)
        if not (0 <= i < self.ny and 0 <= j < self.nx):
            return 0.0
        return float(self.dist[i, j])

    def segment_clearance(self, a, b) -> float:
        n = max(2, int(math.hypot(b[0] - a[0], b[1] - a[1]) / (0.5 * RES)) + 1)
        return min(self.clearance(a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])) for t in np.linspace(0, 1, n))

    def labels(self, clearance: float) -> np.ndarray:
        """Connected free space with enough clearance, one label per region."""
        if getattr(self, "_labels_for", None) != clearance:
            self._labels, _ = ndimage.label(self.dist >= clearance)
            self._labels_for = clearance
        return self._labels

    def region(self, x: float, y: float, clearance: float) -> int:
        return int(self.labels(clearance)[self.snap(x, y, clearance)])

    def snap(self, x: float, y: float, clearance: float, region: int | None = None) -> tuple[int, int]:
        """Nearest cell with enough clearance, optionally within one connected region."""
        i0, j0 = self.cell(x, y)
        free = np.argwhere(self.dist >= clearance if region is None else self.labels(clearance) == region)
        k = np.argmin((free[:, 0] - i0) ** 2 + (free[:, 1] - j0) ** 2)
        return int(free[k, 0]), int(free[k, 1])

    def astar(self, a, b, clearance: float, centre_m: float) -> list[tuple[float, float]]:
        start, goal = self.snap(*a, clearance), self.snap(*b, clearance)
        penalty = 3.0 * np.clip((centre_m - self.dist) / centre_m, 0.0, 1.0)
        best = {start: 0.0}
        prev = {}
        heap = [(0.0, start)]
        steps = [(di, dj, math.hypot(di, dj)) for di in (-1, 0, 1) for dj in (-1, 0, 1) if di or dj]
        while heap:
            _, c = heapq.heappop(heap)
            if c == goal:
                break
            g = best[c]
            for di, dj, d in steps:
                n = (c[0] + di, c[1] + dj)
                if not (0 <= n[0] < self.ny and 0 <= n[1] < self.nx) or self.dist[n] < clearance:
                    continue
                ng = g + d * (1.0 + penalty[n])
                if ng < best.get(n, math.inf):
                    best[n] = ng
                    prev[n] = c
                    heapq.heappush(heap, (ng + math.hypot(goal[0] - n[0], goal[1] - n[1]), n))
        if goal not in best:
            raise SystemExit(f"no path from {a} to {b} with {clearance} m clearance")
        cells = [goal]
        while cells[-1] != start:
            cells.append(prev[cells[-1]])
        return [self.pos(*c) for c in reversed(cells)]


def shorten(path, level: Level, clearance: float):
    out = [path[0]]
    i = 0
    while i < len(path) - 1:
        j = len(path) - 1
        while j > i + 1 and level.segment_clearance(path[i], path[j]) < clearance:
            j -= 1
        out.append(path[j])
        i = j
    return out


def inside(p, poly) -> bool:
    x, y = p
    hit = False
    for (x1, y1), (x2, y2) in zip(poly, poly[1:] + poly[:1]):
        if (y1 > y) != (y2 > y) and x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
            hit = not hit
    return hit


def connector_ends(c: dict, levels: dict, lo: int, hi: int, clearance: float):
    """Two points inside the connector: the ends of a climb along its long axis, or the lift centre twice."""
    pts = np.array(c["area"], dtype=float)
    centre = pts.mean(axis=0)
    if c["kind"] == "elevator":
        return tuple(centre), tuple(centre)
    _, _, vt = np.linalg.svd(pts - centre)
    axis = vt[0]
    proj = (pts - centre) @ axis
    centre = centre + axis * 0.5 * (proj.min() + proj.max())
    half = 0.5 * np.ptp(proj)
    for inset in np.arange(0.6, half, 0.2):
        a, b = tuple(centre - axis * (half - inset)), tuple(centre + axis * (half - inset))
        ok = inside(a, c["area"]) and inside(b, c["area"])
        if ok and all(levels[o].segment_clearance(a, b) >= clearance for o in (lo, hi)):
            return a, b
    raise SystemExit(f"connector {c['id']}: no straight climb with {clearance} m clearance")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--venue", type=Path, required=True)
    ap.add_argument("--goals", nargs="+", required=True, help="x,y,level goals and @connector_id transitions")
    ap.add_argument("--clearance", type=float, default=0.25, help="minimum distance from walls [m]")
    ap.add_argument("--centre", type=float, default=1.0, help="prefer cells at least this far from walls [m]")
    ap.add_argument("--name", required=True)
    ap.add_argument("--description", help="default: the description already in --out, if any")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--demo-config", help="particle-filter config patch the web demo applies, relative to --out")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    venue = json.loads(args.venue.read_text())
    levels = {l["ordinal"]: Level(l["walls"], venue["bounds"]) for l in venue["levels"]}
    connectors = {c["id"]: c for c in venue.get("connectors", [])}

    waypoints: list[list[float]] = []
    pos = None
    via = None
    for tok in args.goals:
        if tok.startswith("@"):
            via = connectors[tok[1:]]
            continue
        x, y, lv = tok.split(",")
        goal, lv = (float(x), float(y)), int(lv)
        if pos is None:
            pos = (goal, lv)
            waypoints.append([*levels[lv].pos(*levels[lv].snap(*goal, args.clearance)), lv])
            continue
        (cur, cur_lv) = pos
        if lv != cur_lv:
            if via is None or not {cur_lv, lv} <= set(via["levels"]):
                raise SystemExit(f"goal {tok}: name a connector serving levels {cur_lv} and {lv} with @ID")
            ends = connector_ends(via, levels, cur_lv, lv, args.clearance)
            # Enter at an end reachable from here; shorter walk first.
            options = sorted([ends, ends[::-1]], key=lambda e: math.dist(cur, e[0]) + math.dist(e[1], goal))
            here = levels[cur_lv].region(*cur, args.clearance)
            options = [(a, b) for a, b in options if levels[cur_lv].region(*a, args.clearance) == here]
            if not options:
                raise SystemExit(f"{via['id']}: no end reachable from {cur} on level {cur_lv}")
            a, b = options[0]
            leg = levels[cur_lv].astar(cur, a, args.clearance, args.centre)
            waypoints += [[*p, cur_lv] for p in shorten(leg, levels[cur_lv], args.clearance)[1:]]
            waypoints.append([*a, cur_lv])
            waypoints.append([*b, lv])
            cur, via = b, None
        # A goal in a closed room (or off by a little) becomes the nearest point the walker can reach.
        goal = levels[lv].pos(*levels[lv].snap(*goal, args.clearance, levels[lv].region(*cur, args.clearance)))
        leg = levels[lv].astar(cur, goal, args.clearance, args.centre)
        waypoints += [[*p, lv] for p in shorten(leg, levels[lv], args.clearance)[1:]]
        pos = (tuple(waypoints[-1][:2]), lv)

    # Drop repeats (a snapped goal equal to the previous waypoint).
    walk = [waypoints[0]]
    for w in waypoints[1:]:
        if w[2] != walk[-1][2] or math.dist(w[:2], walk[-1][:2]) > 0.05:
            walk.append(w)
    length = sum(math.dist(a[:2], b[:2]) for a, b in zip(walk, walk[1:]))
    description = args.description
    if description is None:
        description = json.loads(args.out.read_text()).get("description", "") if args.out.exists() else ""
    scenario = {
        "name": args.name,
        "description": description,
        "venue": os.path.relpath(args.venue.resolve(), args.out.resolve().parent),
        "seed": args.seed,
        **({"demo_config": args.demo_config} if args.demo_config else {}),
        "walk": {"waypoints": "WAYPOINTS"},
        "baro": {},
    }
    rows = ",\n      ".join(json.dumps([round(w[0], 2), round(w[1], 2), w[2]]) for w in walk)
    text = json.dumps(scenario, indent=2).replace('"WAYPOINTS"', "[\n      " + rows + "\n    ]")
    args.out.write_text(text + "\n")
    print(f"{args.out}: {len(walk)} waypoints, {length:.0f} m horizontal")


if __name__ == "__main__":
    main()
