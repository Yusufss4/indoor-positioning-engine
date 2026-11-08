"""Python mirror of core/src/georef.cpp: the venue's local tangent-plane frame on WGS84.

`rotation_deg` is the counter-clockwise angle from true east to local +x, as in the venue `georef`.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

WGS84_A = 6378137.0
WGS84_E2 = 6.69437999014e-3


def radii(lat_deg: float) -> tuple[float, float]:
    """Metres per radian of latitude and of longitude at `lat_deg`."""
    s = math.sin(math.radians(lat_deg))
    w = 1.0 - WGS84_E2 * s * s
    return WGS84_A * (1.0 - WGS84_E2) / (w * math.sqrt(w)), WGS84_A / math.sqrt(w) * math.cos(math.radians(lat_deg))


@dataclass
class GeoRef:
    origin_lat: float
    origin_lon: float
    rotation_deg: float = 0.0

    def to_local(self, lat, lon):
        m_lat, m_lon = radii(self.origin_lat)
        north = np.radians(np.asarray(lat, dtype=float) - self.origin_lat) * m_lat
        east = np.radians(np.asarray(lon, dtype=float) - self.origin_lon) * m_lon
        th = math.radians(self.rotation_deg)
        return east * math.cos(th) + north * math.sin(th), -east * math.sin(th) + north * math.cos(th)

    def to_wgs84(self, x, y):
        th = math.radians(self.rotation_deg)
        x, y = np.asarray(x, dtype=float), np.asarray(y, dtype=float)
        east = x * math.cos(th) - y * math.sin(th)
        north = x * math.sin(th) + y * math.cos(th)
        m_lat, m_lon = radii(self.origin_lat)
        return self.origin_lat + np.degrees(north / m_lat), self.origin_lon + np.degrees(east / m_lon)

    def as_json(self) -> dict:
        return {"origin_lat": self.origin_lat, "origin_lon": self.origin_lon, "rotation_deg": self.rotation_deg}
