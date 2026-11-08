"""Writing ipe traces (docs/formats.md) and trace manifests."""

from __future__ import annotations

import json
import zlib
from pathlib import Path


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
