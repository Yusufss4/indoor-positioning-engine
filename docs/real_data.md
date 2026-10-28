# Real phone traces: Microsoft Indoor Location dataset

The simulator and the filters share one family of propagation models (see
[design.md](design.md#known-model-limitations)), so the simulated results are optimistic. This page
replays real phone recordings through the unchanged engine.

**Summary.** On 109 held-out walks in two Chinese shopping malls (14 floors, with plan extents of
42,000-119,000 m^2), the
particle filter reaches a median CEP50 of 7.5 m and a median CEP95 of 11.1 m. That is about ten times the
simulated office (0.63 / 1.36 m). It is level with the EKF (7.8 / 10.4 m) and ahead of the weighted
centroid (8.4 / 14.9 m) on CEP95, but it needs a Wi-Fi configuration chosen on separate walks.
With default settings, the particle filter is the worst of the four (11.6 / 20.5 m). Most of the gap
came from data handling, not filter tuning: virtual Wi-Fi access points counted up to six times, and
a uniform start over very large floors.

## Data

[Indoor Location Competition 2.0](https://github.com/location-competition/indoor-location-competition-20)
sample data (MIT licence), pinned at commit `8ab177c`: 1,095 walks on 14 floors of two malls, recorded on
Android phones held flat in front of the body. Each walk has:

- 50 Hz accelerometer, gyroscope, magnetometer and rotation vector;
- Wi-Fi scans (about one every 2-3 s, each reporting about 100 access points) and iBeacon packets;
- waypoints that the surveyor tapped on the floor plan (typically 3-15 per walk): the only ground truth;
- per floor, the floor size in metres, a raster image and a GeoJSON plan.

There is no barometer data, and every walk stays on one floor. The full Kaggle release has 200+
buildings in the same format; its competition rules restrict redistribution, so nothing from either
release is committed. `datasets/msil/fetch.sh` downloads the sample, and only derived metrics are in
this repository.

## Conversion (`datasets/msil/convert.py`)

| Problem the simulator hides | What the converter does | Check |
|---|---|---|
| The phone is not "flat, x forward" | Rotates the accelerometer and gyro into the earth frame using `TYPE_ROTATION_VECTOR`. The step detector uses the magnitude, and gyro z becomes the yaw rate. The absolute heading is the rotation-vector azimuth of the phone's top edge, written as an engine-convention magnetometer sample. | Step length works out to 0.55-0.73 m per detected step over the surveyed distance |
| Map frame vs magnetic north | Fits one offset per site: length-weighted circular mean of (waypoint segment direction - phone azimuth) on training walks | +1.4 deg (site 1) and +0.7 deg (site 2), with 15-18 deg spread per segment: the plan is north-up |
| Plan in WGS84 | Scales the floor polygon's longitude/latitude box to the floor's width x height in metres | 100% of waypoints fall inside the floor outline |
| Unknown transmitter positions | Robust least squares (soft-L1, 4 dB scale) of position, tx power and path-loss exponent for each transmitter, on interpolated positions from training walks. Keeps transmitters with at least 30 samples, at least 3 m of spatial spread, RMS of 8 dB or less, inside the floor | 252-887 transmitters per floor; median fit RMS 4.5-5.9 dB |
| Virtual access points | One radio advertises up to 6 SSIDs under BSSIDs that differ only in the first octet. These are merged into one transmitter, keeping one reading per access point per 50 ms | In 40 walks on site1 F1, half of the Wi-Fi readings were duplicates and 162 of 715 physical access points had 6 BSSIDs |
| Walls | Floor outline as walls (`boundary`). Shop outlines (`units`) have no doors, so the surveyor's path crosses them | `units`: 8-132 truth crossings on every floor, so never used. `boundary`: 0 crossings, except site1 F4 (2), which therefore gets no walls |

Two more data facts shaped the setup:

- iBeacons barely exist. The most frequent ID (`..._0_0`, 30,000 packets on one floor) is shared by
  unconfigured beacons and cannot be located; only 0-9 beacons per floor pass the fit. The results are
  therefore **Wi-Fi positioning**, with access points fed through the engine's existing BLE path.
- Walks are short (median 36 s, 7 waypoints), and floor plans span 42,000-119,000 m^2. A uniform start
  puts 1,500 particles at 0.01-0.04 per m^2.

Ground truth rows are the surveyor's waypoints only. Replay scores at those instants, never at
interpolated positions, after the usual 5 s warm-up.

## Method

- Walks are split by a hash of their id: 80% fit the radio map, 20% (222 walks) are test walks.
- The test walks are split again by hash into a **tune** half and a **held-out** half (111 each). Every
  setting below was chosen on the tune half. The tables report the held-out half, which was replayed
  once with the frozen settings. 109 of 111 walks are longer than the warm-up.
- A recording is one run, so medians over walks are the headline numbers. Means are shown too, because a
  few lost walks dominate them.

## Results (held-out walks)

Median CEP50 / median CEP95 [m] over walks. "Wi-Fi config" is
[`data/configs/msil_wifi.json`](../data/configs/msil_wifi.json): RSSI sigma 8 dB, 5,000 particles, and
the beacon-driven init and recovery proposals from experiment 4 in [experiments.md](experiments.md).

| Filter | Median CEP50 | Median CEP95 | Mean CEP50 | Mean CEP95 |
|---|---:|---:|---:|---:|
| pf, default | 11.6 | 20.5 | 21.8 | 31.2 |
| pf-nomap, default | 11.7 | 25.0 | 20.5 | 33.5 |
| pf, Wi-Fi config without the proposals | 9.9 | 14.1 | 14.7 | 20.4 |
| pf-nomap, Wi-Fi config without the proposals | 8.9 | 12.7 | 14.5 | 19.0 |
| **pf, Wi-Fi config** | **7.5** | 11.1 | 12.3 | 17.0 |
| pf-nomap, Wi-Fi config | 8.0 | 11.8 | 12.2 | 16.9 |
| ekf, default | 7.8 | **10.4** | 12.7 | **15.4** |
| wcl, default | 8.4 | 14.9 | **10.5** | 16.7 |

Per floor (median CEP50 / CEP95; few walks per floor, so single floors are noisy):

| Floor | Walks | pf (default) | pf (Wi-Fi config, no proposal) | pf (Wi-Fi config) | ekf | wcl |
|---|---:|---:|---:|---:|---:|---:|
| site1_B1 | 18 | 20.0 / 24.6 | 17.5 / 18.3 | 10.4 / 12.7 | 10.2 / 11.6 | 9.8 / 14.8 |
| site1_F1 | 7 | 10.6 / 11.6 | 9.0 / 10.4 | 5.6 / 7.4 | 7.4 / 8.8 | 6.4 / 15.0 |
| site1_F2 | 13 | 19.2 / 30.4 | 8.3 / 16.3 | 6.6 / 12.0 | 5.9 / 8.4 | 8.0 / 14.7 |
| site1_F3 | 17 | 9.3 / 11.0 | 7.4 / 10.8 | 4.9 / 8.0 | 7.3 / 8.6 | 6.7 / 8.1 |
| site1_F4 | 13 | 9.1 / 17.2 | 6.0 / 9.3 | 7.1 / 10.5 | 7.3 / 8.7 | 7.3 / 14.2 |
| site2_B1 | 5 | 8.6 / 18.2 | 7.7 / 13.1 | 8.0 / 8.5 | 5.2 / 6.7 | 9.9 / 14.1 |
| site2_F1 | 9 | 7.7 / 26.5 | 8.3 / 14.7 | 7.5 / 13.2 | 7.8 / 13.0 | 8.7 / 21.7 |
| site2_F2 | 2 | 10.1 / 21.8 | 18.2 / 20.4 | 9.0 / 9.8 | 18.0 / 19.9 | 9.1 / 11.8 |
| site2_F3 | 9 | 20.4 / 25.7 | 12.2 / 21.2 | 13.2 / 14.2 | 11.4 / 14.3 | 16.5 / 19.6 |
| site2_F4 | 1 | 55.0 / 92.8 | 48.3 / 64.3 | 13.4 / 20.1 | 6.3 / 10.3 | 20.6 / 29.7 |
| site2_F5 | 2 | 43.0 / 52.1 | 44.6 / 60.6 | 63.5 / 70.5 | 21.0 / 27.3 | 24.8 / 32.5 |
| site2_F6 | 6 | 9.9 / 14.7 | 8.4 / 9.1 | 6.0 / 12.1 | 5.3 / 9.1 | 8.4 / 13.8 |
| site2_F7 | 4 | 24.3 / 30.6 | 15.4 / 19.7 | 8.3 / 21.0 | 10.0 / 12.2 | 15.5 / 19.9 |
| site2_F8 | 3 | 23.1 / 25.4 | 16.7 / 21.7 | 11.8 / 12.7 | 18.2 / 18.7 | 8.7 / 15.0 |

![held-out walk on site2 F1](img/msil_trajectory.png)

The walk above is the held-out walk longer than 70 s whose particle filter CEP50 is closest to the
overall median (217 s, 38 waypoints). Lines join each waypoint to each filter's estimate at that time.

### What changed the numbers (tune half, `pf`, median CEP50 / CEP95)

| Step | pf | Note |
|---|---:|---|
| defaults, before merging virtual access points | 16.0 / 31.5 | the same reading counted up to 6 times: tight, wrong clouds |
| defaults, virtual access points merged | 15.5 / 22.8 | converter change only |
| + init proposal | 11.4 / 16.8 | floors this large are where the beacon-driven start pays |
| RSSI sigma 8 dB, no proposal | 14.2 / 23.6 | |
| sigma 8 + init proposal | 11.4 / 15.4 | |
| sigma 12 + init proposal | 13.0 / 16.7 | wider is not better |
| sigma 8 + init + 5,000 particles | 10.0 / 13.9 | |
| sigma 8 + init + recovery proposal | 9.3 / 15.2 | |
| **sigma 8 + init + recovery + 5,000 particles** | **8.8 / 12.2** | frozen as `msil_wifi.json` |
| ekf / wcl, defaults | 9.3 / 12.8, 9.5 / 14.8 | reference |

Each line is one replay of the same 109 tune walks; differences under about 1 m are within the
walk-to-walk noise.

## Simulation vs. real data

| | pf CEP50 / CEP95 [m] | EKF CEP50 / CEP95 [m] |
|---|---:|---:|
| office_nominal (simulated, CI baseline) | 0.63 / 1.36 | 0.84 / 4.07 |
| airport_hall_sparse (simulated, CI baseline) | 1.48 / 8.88 | 1.72 / 10.13 |
| Microsoft malls (real, held-out median over walks) | 7.5 / 11.1 | 7.8 / 10.4 |

The simulated rows pool all scored epochs over 3 seeds, as in `tests/regression/baseline.json`.
The real row is the median of per-walk CEPs, so the rows are comparable in scale but not to the decimal.

In the simulated office the particle filter's CEP95 is a third of the EKF's; on real data the two are level. On
raw Wi-Fi the particle filter's per-packet independence assumption is its weak point. The EKF
ignores packets below -85 dBm and the weighted centroid uses only the strongest few, so both are
naturally robust to the weak, correlated readings that mislead the particle filter. The map helps much
less than in simulation (`pf` vs `pf-nomap` is within noise). Only the floor outline is used, and
the radio model's walls are not the mall's: a mall's radio environment is dominated by open atria and
glass shop fronts.

## Failure modes and open questions

- **Correlated Wi-Fi errors.** Each scan reports about 100 access points whose fitted-model errors are
  spatially correlated. The likelihood multiplies them as if independent, which makes the cloud
  overconfident: under 1 m spread while 20 m off. Merging virtual access points removed the worst
  duplicates. Weighting a scan as one measurement (for example, a tempered likelihood per scan) is the
  natural next engine change.
- **Short walks on large floors.** Most walks end before any filter meets the "error under 3 m for
  10 s" convergence rule (91-101 of 109 never do), so convergence time is not reported here. Multi-walk
  sessions from the full dataset would show steady-state accuracy.
- **Radio map quality.** Positions come from interpolating between waypoints that are 5-10 s apart, and
  surveyors do not walk straight lines. The radio map inherits that error, and so does the scoring.
- **Scan latency.** Wi-Fi readings are timestamped with the access point's `last_seen`, up to a few
  seconds before the scan completes. The walker has moved by then; the engine has no notion of
  measurement age.
- **Doors.** The shop outlines would be useful walls if the doors were known. IMDF `opening` features
  carry exactly that; this GeoJSON does not.
- **site2 F5** (2 walks) is lost by every filter. Its transmitter density is average (9 per 1,000 m^2),
  so the cause is not established; two walks are too few to tell.

## Reproduce

```bash
./datasets/msil/fetch.sh                                         # 2.5 GB, pinned commit
python3 -m venv .venv && .venv/bin/pip install -r datasets/msil/requirements.txt -r viz/requirements.txt
.venv/bin/python datasets/msil/convert.py --wifi                 # writes datasets/msil/out/
./build/tools/ipe-regress --traces datasets/msil/out/manifest_holdout.json --no-baseline
./build/tools/ipe-regress --traces datasets/msil/out/manifest_holdout.json --no-baseline \
    --presets pf,pf-nomap --config data/configs/msil_wifi.json
ctest --test-dir build -L realdata                               # gate against tests/regression/msil_baseline.json
.venv/bin/python datasets/msil/plot.py --trace site2/F1/5dd35c9844333f00067aa0e0.csv --out docs/img/msil_trajectory.png
```

`convert.py --help` lists the options: `--walls`, the fit thresholds, `--test-every`, and `--sites` /
`--floors` to convert part of the data. The `real_data_regression` CTest is skipped when
`datasets/msil/out/` is missing, so CI stays green without the dataset.
