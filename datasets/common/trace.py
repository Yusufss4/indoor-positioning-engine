"""Writing and reading ipe traces (docs/formats.md), and trace manifests."""

from __future__ import annotations

import json
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np


class TraceWriter:
    """Collects rows with their time and writes them sorted (stable, so the rows of one scan stay
    together in the order given)."""

    def __init__(self, name: str, source: str):
        self.header = ["# ipe-trace v1", f"# scenario: {name}", f"# source: {source}"]
        self.rows: list[tuple[float, str]] = []

    def imu(self, t, a, g, m):
        self.rows.append((t, f"imu,{t:.3f},{a[0]:.4f},{a[1]:.4f},{a[2]:.4f},{g[0]:.5f},{g[1]:.5f},{g[2]:.5f},"
                             f"{m[0]:.3f},{m[1]:.3f},{m[2]:.3f}"))

    def ble(self, t, tid, rssi, scan: int | None = None):
        self.rows.append((t, f"ble,{t:.3f},{tid},{rssi:.0f}" + (f",{scan}" if scan else "")))

    def baro(self, t, hpa):
        self.rows.append((t, f"baro,{t:.3f},{hpa:.4f}"))

    def gt(self, t, x, y, heading, level: int | None = None, z: float = 0.0):
        if level is None:
            self.rows.append((t, f"gt,{t:.3f},{x:.3f},{y:.3f},{heading:.4f}"))
        else:
            self.rows.append((t, f"gt,{t:.3f},{x:.3f},{y:.3f},{heading:.4f},{level},{z:.3f}"))

    def write(self, path: Path):
        self.rows.sort(key=lambda r: r[0])
        with open(path, "w") as f:
            f.write("\n".join(self.header) + "\n")
            for _, line in self.rows:
                f.write(line + "\n")


def split_by_hash(entries: list[dict]) -> tuple[list[dict], list[dict]]:
    """Tune / holdout halves by trace-name hash."""
    tune = [e for e in entries if zlib.crc32(e["name"].encode()) % 2 == 0]
    holdout = [e for e in entries if zlib.crc32(e["name"].encode()) % 2 == 1]
    return tune, holdout


def write_manifests(out: Path, entries: list[dict], tune: list[dict] | None = None, holdout: list[dict] | None = None):
    """manifest.json lists every test trace. Settings must be chosen on manifest_tune.json and
    reported on manifest_holdout.json, a disjoint subset (by default a trace-name hash split)."""
    out.mkdir(parents=True, exist_ok=True)
    if tune is None or holdout is None:
        tune, holdout = split_by_hash(entries)
    for name, subset in [("manifest", entries), ("manifest_tune", tune), ("manifest_holdout", holdout)]:
        (out / f"{name}.json").write_text(json.dumps({"traces": subset}, indent=1))


@dataclass
class RadioWalk:
    """What a trace says about radio and truth: the part a radio map is fitted from."""
    path: Path
    readings: list = field(default_factory=list)  # (t, id, dBm, scan); scan 0 is a lone packet
    gt: np.ndarray = field(default_factory=lambda: np.zeros((0, 4)))  # t, x, y, level index
    imu_rows: int = 0


def read_walk(path: Path) -> RadioWalk:
    """The `ble` and `gt` rows of a trace. A `gt` row without a level (single-floor traces) is on
    level index 0."""
    walk = RadioWalk(Path(path))
    gt = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, start=1):
            line = line.strip()
            if not line or line[0] == "#":
                continue
            c = line.split(",")
            try:
                if c[0] == "ble":
                    walk.readings.append((float(c[1]), c[2], float(c[3]), int(c[4]) if len(c) > 4 else 0))
                elif c[0] == "gt":
                    gt.append((float(c[1]), float(c[2]), float(c[3]), int(c[5]) if len(c) > 5 else 0))
                elif c[0] == "imu":
                    walk.imu_rows += 1
            except (ValueError, IndexError) as e:
                raise ValueError(f"{path}:{n}: {line!r}") from e
    walk.gt = np.array(sorted(gt)) if gt else np.zeros((0, 4))
    return walk


def interp_same_level(gt: np.ndarray, t: np.ndarray):
    """Position at times t by linear interpolation between consecutive landmarks on the same floor;
    mask is False elsewhere (before the first, after the last, or across a floor change).
    `gt` rows are (t, x, y, level); returns (xy, level, mask)."""
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
