"""What the web demo's recording exporters share (datasets/*/export_demo.py).

A recordings folder is data/demo/<name>/ with a demos.json index: per recording a venue, its GeoJSON
export for the 3D view, a floor image, a trace and the engine config to run it with, all as paths relative
to the folder. web/app.js lists the folders it reads in RECORDING_SOURCES and skips the ones that are
missing.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path


def waypoints(trace: Path) -> list[tuple[float, float]]:
    """The ground-truth points of a trace, in order."""
    pts = []
    for line in trace.read_text().splitlines():
        if line.startswith("gt,"):
            _, _, x, y, *_ = line.split(",")
            pts.append((float(x), float(y)))
    return pts


def view_box(bounds: list[float], pts: list[tuple[float, float]], pad_m: float) -> list[float]:
    """What the 2D view opens on: the points with `pad_m` around them, inside the venue's bounds."""
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    view = [max(bounds[0], min(xs) - pad_m), max(bounds[1], min(ys) - pad_m),
            min(bounds[2], max(xs) + pad_m), min(bounds[3], max(ys) + pad_m)]
    return [round(v, 1) for v in view]


def longest_per_group(traces: list[dict], need_scans: bool = False) -> dict[str, dict]:
    """Per manifest group (a floor), the entry of its longest walk; with `need_scans`, of those with a scan."""
    longest: dict[str, dict] = {}
    for t in traces:
        if need_scans and t["scans"] == 0:
            continue  # inertial sensors only: nothing to position with
        if t["group"] not in longest or t["duration_s"] > longest[t["group"]]["duration_s"]:
            longest[t["group"]] = t
    return longest


def export_geojson(build: Path, venue_json: Path, out: Path) -> None:
    """The venue as GeoJSON (ipe-import export), which the 3D view draws."""
    subprocess.run([str(build / "tools" / "ipe-import"), "export", str(venue_json), "--out", str(out)],
                   check=True, capture_output=True)


def write_index(dest: Path, index: dict, ensure_ascii: bool = True) -> None:
    (dest / "demos.json").write_text(json.dumps(index, indent=1, ensure_ascii=ensure_ascii) + "\n")
    print(f"wrote {dest / 'demos.json'}")
