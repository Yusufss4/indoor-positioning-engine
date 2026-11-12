"""Offline use of the engine: a whole recording is available, so a fix may use what came after it.

The engine itself is causal. Two passes get most of what a smoother would: the trace is replayed as
recorded and once more played backwards (`reverse_trace`), and the two estimates at each truth row are
combined (`fuse`). The forward pass knows a row's past and the backward pass its future, so their
errors are close to independent, and each is weakest where the other is strongest: the first seconds
of a walk, before the forward filter has seen a scan, are the last seconds of the backward one.

This is for competitions scored offline (Kaggle's indoor location) and for surveying. It is not what a
phone can show live.
"""

from __future__ import annotations

import csv
import math
from pathlib import Path


def reverse_trace(src: Path, dst: Path) -> float:
    """Writes `src` played backwards and returns the time T that maps a reversed time back: t = T - t'.

    A walker moving backwards along the same path has the same accelerations, the opposite turn rates
    and the opposite heading. Scans keep their readings; they are renumbered in their new order."""
    comments, rows = [], []
    for line in src.read_text().splitlines():
        if not line.strip():
            continue
        if line.startswith("#"):
            comments.append(line)
            continue
        f = line.split(",")
        rows.append((float(f[1]), f))
    end = max(t for t, _ in rows)
    out = []
    for index, (t, f) in enumerate(rows):
        kind = f[0]
        f = list(f)
        f[1] = f"{end - t:.3f}"
        if kind == "imu":
            for k in (5, 6, 7, 8, 9):  # gyro x, y, z and the horizontal magnetic field
                f[k] = _negated(f[k])
        elif kind == "gt":
            f[4] = f"{math.atan2(-math.sin(float(f[4])), -math.cos(float(f[4]))):.4f}"
        # Later rows first; rows of one time (a scan) keep their order.
        out.append((end - t, index if kind == "ble" else -index, f))
    out.sort(key=lambda r: (r[0], r[1]))
    scan_ids: dict[str, int] = {}
    with open(dst, "w") as fh:
        fh.write("\n".join(comments) + "\n")
        for _, _, f in out:
            if f[0] == "ble" and len(f) == 5:
                f[4] = str(scan_ids.setdefault(f[4], len(scan_ids) + 1))
            fh.write(",".join(f) + "\n")
    return end


def _negated(text: str) -> str:
    return text[1:] if text.startswith("-") else ("-" + text if float(text) != 0.0 else text)


def read_estimates(path: Path) -> list[dict]:
    """Rows of an `ipe-replay --out` file as dicts of floats (has_fix as bool, levels as int)."""
    rows = []
    for r in csv.DictReader(open(path)):
        rows.append({"t": float(r["t"]), "truth_x": float(r["truth_x"]), "truth_y": float(r["truth_y"]),
                     "est_x": float(r["est_x"]), "est_y": float(r["est_y"]), "sigma_m": float(r["sigma_m"]),
                     "has_fix": r["has_fix"] == "1", "truth_level": int(r["truth_level"]), "est_level": int(r["est_level"])})
    return rows


def fuse(forward: list[dict], backward: list[dict], end: float, sigma_floor_m: float = 3.0) -> list[dict]:
    """One row per truth row, in the forward order. `backward` is the replay of the reversed trace and
    `end` what `reverse_trace` returned.

    Where both passes have a fix on the same level, the position is their mean weighted by
    1 / (sigma^2 + sigma_floor^2): the filter's own spread, which is too small on Wi-Fi, with a floor so
    that neither pass is ignored. On different levels the pass with the smaller spread wins. A row with
    one fix takes it."""
    back = sorted(({**r, "t": end - r["t"]} for r in backward), key=lambda r: r["t"])
    if len(back) != len(forward):
        raise ValueError(f"passes disagree on the truth rows: {len(forward)} forward, {len(back)} backward")
    out = []
    for f, b in zip(forward, back):
        row = dict(f)
        if b["has_fix"] and not f["has_fix"]:
            row.update(est_x=b["est_x"], est_y=b["est_y"], est_level=b["est_level"], sigma_m=b["sigma_m"], has_fix=True)
        elif b["has_fix"] and f["has_fix"]:
            wf = 1.0 / (f["sigma_m"] ** 2 + sigma_floor_m ** 2)
            wb = 1.0 / (b["sigma_m"] ** 2 + sigma_floor_m ** 2)
            if f["est_level"] != b["est_level"]:
                best = f if wf >= wb else b
                row.update(est_x=best["est_x"], est_y=best["est_y"], est_level=best["est_level"], sigma_m=best["sigma_m"])
            else:
                row.update(est_x=(wf * f["est_x"] + wb * b["est_x"]) / (wf + wb),
                           est_y=(wf * f["est_y"] + wb * b["est_y"]) / (wf + wb), sigma_m=math.sqrt(1.0 / (wf + wb)))
        row["error_m"] = math.hypot(row["est_x"] - row["truth_x"], row["est_y"] - row["truth_y"])
        out.append(row)
    return out
