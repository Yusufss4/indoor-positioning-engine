"""Radio fingerprints for a venue: training scans with the floor and the position they were taken at.

The venue stores them as `floor_fingerprints` (docs/formats.md):

    {"ids": [beacon id, ...],
     "scans": [[ordinal, id index, dBm, id index, dBm, ...], ...],
     "xy": [[x, y], ...]}          # optional, one per scan

The engine's floor classifier uses the ordinals; with `xy` its fingerprint likelihood also uses the
positions. A transmitter that the log-distance fit rejected can still be a fingerprint dimension: it is
written to the venue's beacons with `"model": false`, at the centroid of where it was heard strongest.
"""

from __future__ import annotations

import numpy as np


def fingerprint_table(scans: list[tuple[int, float, float, dict[str, float]]], positions: bool = True) -> dict:
    """`scans` are (ordinal, x, y, {id: dBm}); readings are written as integer dBm, ids sorted."""
    ids = sorted({k for *_, rssi in scans for k in rssi})
    index = {k: i for i, k in enumerate(ids)}
    table = {"ids": ids,
             "scans": [[int(o)] + [v for k, r in sorted(rssi.items()) for v in (index[k], int(round(r)))]
                       for o, _, _, rssi in scans]}
    if positions:
        table["xy"] = [[round(float(x), 2), round(float(y), 2)] for _, x, y, _ in scans]
    return table


def unmodelled_beacons(scans: list[tuple[int, float, float, dict[str, float]]], modelled: set[str],
                       min_scans: int) -> list[dict]:
    """Venue beacon entries for the ids heard in at least `min_scans` scans that have no fitted
    transmitter: on the floor where they are heard most, at the centroid of their five strongest
    readings there. They carry `"model": false`, so the engine uses them in fingerprints only."""
    heard: dict[str, list[tuple[float, int, float, float]]] = {}
    for o, x, y, rssi in scans:
        for k, r in rssi.items():
            if k not in modelled:
                heard.setdefault(k, []).append((r, int(o), x, y))
    out = []
    for k in sorted(heard):
        rows = heard[k]
        if len(rows) < min_scans:
            continue
        floors = [o for _, o, _, _ in rows]
        home = max(sorted(set(floors)), key=floors.count)
        best = sorted((row for row in rows if row[1] == home), reverse=True)[:5]
        out.append({"id": k, "x": round(float(np.mean([b[2] for b in best])), 3),
                    "y": round(float(np.mean([b[3] for b in best])), 3), "level": home, "model": False})
    return out


def drop_rare(scans: list[tuple[int, float, float, dict[str, float]]], keep: set[str]) -> list:
    """Scans restricted to the ids in `keep`; scans left empty are dropped."""
    out = []
    for o, x, y, rssi in scans:
        kept = {k: r for k, r in rssi.items() if k in keep}
        if kept:
            out.append((o, x, y, kept))
    return out
