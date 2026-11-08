# BLE radio model against measurements: UJI BLE database

The simulator and the filter share one RSSI model ([design.md](design.md#known-model-limitations)), so
simulated accuracy cannot show whether that model is right. The UJI BLE database
([Zenodo 1618692](https://doi.org/10.5281/zenodo.1618692), data CC BY 4.0) measures it directly:
- surveyed iBeacons;
- mapped shelves and pillars;
- thousands of fingerprints taken standing still at surveyed points, with three phones and three
  transmit powers.

This page checks the engine's RSSI parameters against it, replays the fingerprints through the
unchanged engine, and compares `ipe-plan`'s map-only scores with what was measured.

**Summary.**
- The 5 dB spread the engine assumes matches the data: 4.5–6.2 dB.
- An obstacle edge costs about 1 dB, not 3–3.5.
- Real outliers are ten times rarer than the engine's 5% mixture.
- Phones differ by up to 18 dB, beyond 2 sigma of the device-offset prior. The offset estimate absorbs
  it anyway: BLE alone, standing still, gives a median CEP50 of 1.4–1.9 m on held-out points for every
  phone.
- `ipe-plan` counts nearly every beacon as heard, while a ~1 s fingerprint catches only 40–66% even of
  strong ones. Its Cramér-Rao bound has the right scale but does not rank points.

## Data

| Zone | Area | Beacons | Transmit power | Phones | Points | Fingerprints | Obstacles |
|---|---|---|---|---|---|---|---|
| `lib` | library, about 15 × 12 m among bookshelves | 24 iBeacons (Accent IBKS105) | −12 dBm | Galaxy A5 2017, BQ Aquaris X5 Plus, Galaxy S6 | 58 | 2,100 (2 campaigns) | 27 shelves, 6 pillars |
| `geo` | office lab, about 16 × 10 m | 22 iBeacons on a grid of about 2 × 2.4 m | −4, −12, −20 dBm | Galaxy A5 2017 | 34 | 884 per power | 4 + 6 |

Each fingerprint holds:
- one RSSI per beacon (100 = not heard);
- its detection time;
- the point's coordinates;
- an id that encodes phone, power, campaign, point and sample.

A standing-still session at one point gives 6–13 fingerprints, about one per second. `datasets/uji_ble/`
has the fetch script (md5 checked), the loader (`load.py`), the study (`study.py`), the venue and trace
writer (`convert.py`) and the `ipe-plan` comparison (`plan_check.py`).

**Split.** Points are split by a hash of their coordinates (`study.fit_half`).
- The path-loss curve and the obstacle loss are fitted on one half.
- The venues in `data/uji_ble/` carry those fitted values.
- The replay reports the other half as holdout (`manifest_holdout.json`). The fit half
  (`manifest_tune.json`) is in-sample for the curve.

## The RSSI model, parameter by parameter

`study.py` fits RSSI = P0 + offset(phone) − 10 n log10(d) − L × (obstacle edges crossed) on every packet
of the fit half: soft-L1 loss, 4 dB scale, A5 as the reference phone. Obstacle edges are counted like
the engine counts walls, so crossing a shelf costs two edges.

| Zone | Power | P0 [dBm] | n | L per edge [dB] | Residual SD (robust) [dB] | SD, held-out half | Beyond 3 robust SD | Lag-1 r | Per-beacon n |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| lib | −12 | −46.8 | 2.27 | 0.8 | 5.9 (5.6) | 6.1 | 0.6% | −0.06 | 2.2–3.1 |
| geo | −4 | −52.8 | 1.63 | 1.3 | 6.1 (6.2) | 6.2 | 0.3% | 0.14 | 0.9–3.8 |
| geo | −12 | −60.3 | 1.59 | 1.1 | 5.6 (5.6) | 5.4 | 0.2% | 0.12 | 0.7–2.8 |
| geo | −20 | −68.7 | 1.33 | 0.9 | 4.7 (4.5) | 4.9 | 0.3% | 0.15 | 0.9–2.1 |

Library phone offsets, relative to the A5: BQ −18.1 dB, S6 −16.8 dB.

Against the engine's defaults:

| Engine parameter | Default | Measured | Reading |
|---|---|---|---|
| `rssi.sigma_db` | 5 (MSIL Wi-Fi config: 8) | 4.5–6.2 dB, the same on the held-out half | Right for BLE. Wi-Fi needs more ([real_data.md](real_data.md)). |
| `rssi.outlier_prob` | 0.05 | 0.2–0.6% of packets beyond 3 robust SD | Conservative. Harmless for accuracy (MSIL needed robustness), but it flattens the likelihood. |
| `wall_attenuation_db` | 3–3.5 per wall | 0.8–1.3 per obstacle edge, about 2 dB through a shelf | Shelves and pillars are not walls. The shared fit also drops n from 2.8 to 2.27 (lib), which shows the two trade off. |
| Path-loss exponent | 2.0 in the synthetic venues | 1.3–2.3 shared; 0.7–3.8 per beacon | One curve per venue is a coarse fit: per-beacon exponents vary that much. |
| `pf.rssi_offset_sigma` | 8 dB | phones 17–18 dB apart | Outside 2 sigma, but the estimate still converges (replay below). |
| Independent packets | assumed | lag-1 autocorrelation −0.06 to 0.15 within a session | Close to independent at one fingerprint per second. |

**Detection.** The table gives the share of fingerprints in which a beacon is heard, by its RSSI
predicted with the fitted curve:

| Predicted RSSI [dBm] | −90 | −85 | −80 | −75 | −70 | −65 | −60 | −55 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| lib, −12 dBm | 0.00 | 0.09 | 0.26 | 0.50 | 0.61 | 0.65 | 0.66 | 0.69 |
| geo, −12 dBm | 0.20 | 0.28 | 0.36 | 0.38 | 0.41 | 0.47 | 0.38 | |
| geo, −20 dBm | 0.12 | 0.28 | 0.39 | 0.43 | 0.47 | 0.42 | | |

Detection falls off around −85 dBm, as `ipe-plan` assumes, but it levels off at 40–66%: a one-second
scan catches only part of the advertisements. Figures per zone and power (fit, residual histogram,
detection) are written to `viz/output/uji_ble/` by `study.py --figures viz/output/uji_ble`.

## Replay: BLE alone, standing still

`convert.py` writes one trace per standing-still session:
- the session's fingerprints as BLE scans, timed by their detection times;
- 50 Hz IMU of a phone lying still (no steps);
- a ground-truth row at every scan.

That gives 552 sessions, 282 of them at held-out points. Replayed with the default `pf` config,
`datasets/ipin/evaluate.py --warmup 0 --seeds 3`:

| Venue, phone | Sessions (held-out) | Median CEP50 [m] | Median CEP95 [m] |
|---|---:|---:|---:|
| lib, A5 | 62 | 1.90 | 3.09 |
| lib, BQ | 62 | 1.49 | 2.17 |
| lib, S6 | 62 | 1.44 | 2.73 |
| geo −4 dBm, A5 | 32 | 1.68 | 2.38 |
| geo −12 dBm, A5 | 32 | 1.57 | 2.46 |
| geo −20 dBm, A5 | 32 | 1.41 | 2.14 |

The fit half gives 1.6–2.0 m.
- **Phone offset.** The BQ and S6, about 18 dB below the A5 the curve was fitted for, are no worse:
  the per-particle device offset absorbs the difference within a few scans.
- **Transmit power.** −20 dBm is not worse than −4 dBm in a 16 × 10 m room with a beacon every 2 m;
  the weaker beacons are heard by fewer fingerprints but fit the curve better (4.7 against 6.1 dB).

## `ipe-plan` against the measurements

`plan_check.py` runs `ipe-plan analyze` (0.5 m grid, defaults: 5 dB, −85 dBm, offset prior 8 dB) on
each venue. At every surveyed point it compares the planned beacons heard with the mean measured per
A5 fingerprint, and the Cramér-Rao bound with the replay CEP50 (Spearman over points):

| Venue | Points | Heard: planned | Heard: measured | ρ(heard) | Bound p50 [m] | CEP50 p50 [m] | ρ(bound, CEP50) |
|---|---:|---:|---:|---:|---:|---:|---:|
| lib | 58 | 23.9 | 8.3 | 0.05 | 0.97 | 1.98 | −0.19 |
| geo −4 dBm | 34 | 22.0 | 8.4 | — (all 22 planned everywhere) | 1.75 | 1.81 | −0.39 |
| geo −12 dBm | 34 | 21.9 | 7.9 | −0.12 | 1.79 | 1.76 | −0.27 |
| geo −20 dBm | 34 | 18.0 | 6.5 | 0.14 | 2.18 | 1.51 | −0.32 |

- **Coverage.** "Heard above −85 dBm" counts beacons in range, not beacons in a one-second scan, which
  catches about a third of them. A coverage target from `ipe-plan` needs that factor before it is
  compared with a phone.
- **Bound.** Its scale is right (1–2 m against CEP50 1.5–2 m), but within a small room with dense
  beacons it does not rank points. The weak negative correlation is not explained here; one candidate
  is that points at the edge of the zone, where the bound is worst, gain from the map's bounds, which
  the bound does not model. That per-point bounds do not rank agrees with
  [experiments.md](experiments.md) §5, where pooling along a walk is what made the bound useful.

## In the web demo

"Real recordings (UJI BLE, standing still)" has one held-out session per venue (library; lab at −4,
−12 and −20 dBm): the A5 session with the most scans. The map is the mapped shelves and pillars and the
surveyed beacons. There is no 3D view and no plan image: the database gives local coordinates only.
`datasets/uji_ble/export_demo.py` writes `data/real_uji/`.

## Caveats

- Standing still only: no steps, no heading, no walls in the way of walking. This tests the radio
  model and the radio update, not the filter's motion model.
- Three phones and one building; the library's phone offsets may be specific to these models.
- The zones are small and dense: a beacon every 2–3 m. Sparse deployments are where the curve and the
  detection plateau matter most, and they are not in this data.
- Nothing in the engine was tuned on this data. The table above is evidence for a lower wall loss for
  furniture and a lower outlier weight, to be tested with `ipe-regress --seeds 10` before any default
  changes.

## Reproduce

```bash
datasets/uji_ble/fetch.sh
.venv/bin/python datasets/uji_ble/study.py --figures viz/output/uji_ble
.venv/bin/python datasets/uji_ble/convert.py          # data/uji_ble/*.json and datasets/uji_ble/out/
.venv/bin/python datasets/ipin/evaluate.py datasets/uji_ble/out/manifest_holdout.json --warmup 0 --seeds 3
.venv/bin/python datasets/ipin/evaluate.py datasets/uji_ble/out/manifest.json --warmup 0 --seeds 3 \
    --json datasets/uji_ble/out/replay.json --quiet
.venv/bin/python datasets/uji_ble/plan_check.py --replay datasets/uji_ble/out/replay.json
```
