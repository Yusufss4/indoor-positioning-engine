"""Parser for GetSensorData logfiles (IPIN competition Track 3, every year since 2016).

Each line is `TYPE;AppTimestamp;...`. AppTimestamp is the app's common clock for all sensors (seconds
from the start of the recording); SensorTimestamp (third field of the inertial rows) is the sensor's
own clock, accurate for sample intervals. Inertial samples are put on the app clock with one offset
per sensor (median of App - Sensor), which keeps the sensor's spacing without its clock.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np


@dataclass
class Walk:
    name: str
    model: str = ""
    digest: str = ""  # md5 of the file, to drop walks repeated across packages
    imu_t: np.ndarray = field(default_factory=lambda: np.zeros(0))
    acc: np.ndarray = field(default_factory=lambda: np.zeros((0, 3)))
    gyro: np.ndarray = field(default_factory=lambda: np.zeros((0, 3)))
    quat: np.ndarray = field(default_factory=lambda: np.zeros((0, 4)))  # rotation vector x, y, z, w
    wifi: list = field(default_factory=list)  # (t, bssid, rssi, scan) ; scan = AppTimestamp of the batch
    ble: list = field(default_factory=list)  # (t, mac, rssi)
    baro: np.ndarray = field(default_factory=lambda: np.zeros((0, 2)))  # t, hPa
    posi: np.ndarray = field(default_factory=lambda: np.zeros((0, 4)))  # t, lat, lon, floor ordinal


def _on_app_clock(rows: list) -> tuple[np.ndarray, np.ndarray]:
    """rows: (app_t, sensor_t, values...). Returns times on the app clock and the values, sorted."""
    arr = np.array(rows, dtype=float)
    arr = arr[np.argsort(arr[:, 1], kind="stable")]
    offset = float(np.median(arr[:, 0] - arr[:, 1]))
    t = arr[:, 1] + offset
    keep = np.concatenate([[True], np.diff(t) > 0])
    return t[keep], arr[keep, 2:]


def _interp(t_out: np.ndarray, t: np.ndarray, v: np.ndarray) -> np.ndarray:
    return np.column_stack([np.interp(t_out, t, v[:, k]) for k in range(v.shape[1])])


def parse(path: Path, name: str | None = None) -> Walk:
    raw = path.read_bytes()
    w = Walk(name or path.stem, digest=hashlib.md5(raw).hexdigest())
    acc, gyro, ahrs, baro, posi = [], [], [], [], []
    for line in raw.decode("utf-8", errors="replace").splitlines():
        if not line:
            continue
        if line[0] == "%":
            if line.startswith("% Model:"):
                w.model = line.split(":", 1)[1].strip()
            continue
        f = line.split(";")
        kind = f[0]
        try:
            if kind == "ACCE":
                acc.append((float(f[1]), float(f[2]), float(f[3]), float(f[4]), float(f[5])))
            elif kind == "GYRO":
                gyro.append((float(f[1]), float(f[2]), float(f[3]), float(f[4]), float(f[5])))
            elif kind == "AHRS":
                # pitch, roll, yaw (deg), then the rotation vector's x, y, z.
                ahrs.append((float(f[1]), float(f[2]), float(f[6]), float(f[7]), float(f[8])))
            elif kind == "PRES":
                baro.append((float(f[1]), float(f[3])))
            elif kind == "WIFI":
                t = float(f[1])
                w.wifi.append((t, f[4].lower(), float(f[6]), t))
            elif kind == "BLE4":
                w.ble.append((float(f[1]), f[3].lower(), float(f[4])))
            elif kind == "POSI":
                # POSI;t;counter;lat;lon;floor;building (the 2016-2019 format has no counter).
                vals = [v.strip() for v in f[1:] if v.strip()]
                if len(vals) >= 6:
                    posi.append((float(vals[0]), float(vals[2]), float(vals[3]), float(vals[4])))
                elif len(vals) == 5:
                    posi.append((float(vals[0]), float(vals[1]), float(vals[2]), float(vals[3])))
        except (ValueError, IndexError):
            continue

    if acc and gyro and ahrs:
        ta, va = _on_app_clock(acc)
        tg, vg = _on_app_clock(gyro)
        tq, vq = _on_app_clock(ahrs)
        lo, hi = max(ta[0], tg[0], tq[0]), min(ta[-1], tg[-1], tq[-1])
        sel = (ta >= lo) & (ta <= hi)
        w.imu_t = ta[sel]
        w.acc = va[sel]
        w.gyro = _interp(w.imu_t, tg, vg)
        # Interpolating quaternion components is fine at 50-100 Hz; renormalise and recover w.
        q = _interp(w.imu_t, tq, vq)
        n = np.linalg.norm(q, axis=1)
        q[n > 1.0] /= n[n > 1.0, None]
        qw = np.sqrt(np.clip(1.0 - np.sum(q * q, axis=1), 0.0, None))
        w.quat = np.column_stack([q, qw])
    w.baro = np.array(sorted(baro)) if baro else np.zeros((0, 2))
    w.posi = np.array(sorted(posi)) if posi else np.zeros((0, 4))
    w.wifi.sort()
    w.ble.sort()
    return w


def read_gt_csv(path: Path) -> np.ndarray:
    """Scoring-trial ground truth: rows `t,lon,lat,floor,index` -> (t, lat, lon, floor)."""
    rows = []
    for line in path.read_text().splitlines():
        f = [v.strip() for v in line.split(",")]
        if len(f) < 4:
            continue
        try:
            rows.append((float(f[0]), float(f[2]), float(f[1]), float(f[3])))
        except ValueError:
            continue
    return np.array(sorted(rows))
