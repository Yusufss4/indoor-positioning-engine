# Real multi-floor walks: IPIN competition Track 3

The Microsoft Indoor Location data ([real_data.md](real_data.md)) has no barometer, and every walk stays
on one floor. The IPIN competition's smartphone track (Track 3, off-site) fills that gap. Its walks
cross floors on stairs, half of its phones log pressure, and its packages include georeferenced floor
plans. This page replays two of its buildings through the engine.

**Summary.** On the competition's scoring walks the particle filter, with the MSIL Wi-Fi settings
unchanged, reaches:
- Nuremberg (4 walks): median CEP50 7.6 m, floor right 76% of the time;
- UMinho (2 walks): median CEP50 13.5 m, floor right 64% of the time.

These numbers come from `evaluate.py --seeds 5`, with the per-walk metrics averaged over the seeds. The
filter loses the floor in two ways:
- It can only change floor inside a stair or lift area, so a walker who takes the stairs while the
  estimate is 10–20 m off stays on the old floor for minutes.
- The radio model barely separates floors. A nearest-neighbour Wi-Fi fingerprint finds the right floor
  for 93–95% of the scans, but the log-distance model with one floor loss for the whole venue fits
  only 2–4 dB per slab.

The data also exposed a gap in the venue model. Nuremberg's floor −1 is only a stair landing, so venue
levels now take their own `footprint` ([formats.md](formats.md#several-floors)).

An opt-in floor model, `data/configs/ipin.json`, targets both failure modes (see [Floor model](#floor-model)).
On the validation walks it raises the floor hit rate:
- Nuremberg: from 76% to 86%;
- UMinho: from 46% to 70%.

On the scoring walks the gain is smaller. Nuremberg's floor hit goes from 75% to 76% and UMinho's from 64%
to 73%; IPIN p75 is about flat and CEP95 is worse. The regression gate therefore stays on `msil_wifi.json`.

## Data

All three packages are CC BY 4.0 and are downloaded by `datasets/ipin/fetch.sh` with their md5
checked.

| Site | Packages | Floors | Walks: training / validation / scoring | Phones | Barometer |
|---|---|---|---|---|---|
| `nuremberg`: Museum Industriekultur | 2023 ([8362205](https://doi.org/10.5281/zenodo.8362205)), 2024 ([13931119](https://doi.org/10.5281/zenodo.13931119)) | 0, −1 (stair landing), −2 | 54 / 4 / 2 + 2 | Galaxy A5 2017, S7 | all |
| `uminho`: building 11, Guimarães | 2022 ([7612915](https://doi.org/10.5281/zenodo.7612915)) | 0, 1, 2 | 89 / 24 / 3 | 8 models | 4 of 8 models |

- The 2023 and 2024 packages contain byte-identical maps, training walks and validation walks. Each
  year adds two different scoring walks. The converter drops repeated files by md5 and keeps all four
  scoring walks.
- Every log is GetSensorData text: one `TYPE;AppTimestamp;...` line per reading. `ACCE`, `GYRO` and
  `MAGN` are logged at 50–100 Hz, `AHRS` gives the Android rotation vector, `PRES` the pressure in hPa,
  `WIFI` and `BLE4` the radio scans. `POSI` is a landmark the surveyor tapped.
- **Ground truth** is the landmarks only:
  - in training walks, one at every turn (technical annex), so straight lines between them follow the
    path;
  - in validation walks, sparse, with free movement between them;
  - for scoring walks, in `03 Evaluation/GT/*.csv`.
- **Maps** are raster images, one per floor, with reference markers at the points in
  `references.csv`. The 2022 package also has the plans as vector PDFs.
- **Beacons:** Nuremberg has six surveyed iBeacons. UMinho has no BLE infrastructure (technical annex),
  so its BLE packets come from unknown devices that may move.

## Conversion (`datasets/ipin/convert.py`)

| Step | What it does | Check (from `report.json`) |
|---|---|---|
| Frame | The site frame is the map image's axes in metres. The image-to-WGS84 similarity is fitted on the reference markers drawn on the maps (`maps.fit_frame`); the calibration file only seeds the search. | Marker residuals: 1 mm (Nuremberg), 8 cm (UMinho). The calibration file alone is off by up to 0.6 m and 0.3 m. |
| Walls, raster plans | Dark pixels, with the marker labels erased, opened by a 9 cm disk. Text, grid lines and furniture are thinner than walls and drop out. Thick regions (over 0.6 m: the solid areas you can't enter) contribute their outline; the rest is skeletonised and traced into segments. | Walls on the debug PNGs (`--debug`). |
| Walls, vector plans | Every straight PDF segment of 0.2 m or more, closed by 15 cm so that the two outlines of a wall become one strip, then vectorised as above. Door swings are drawn as chains of short pieces: each swing's box is cleared, which opens its doorway (115–142 per floor). | |
| Edits (`edits/<site>.json`) | Stairs and lifts (read off the plans, then checked where training walks change floor), the Nuremberg footprint, and the floor −1 landing (walls plus its own footprint). UMinho also cuts 14 doors that the plans draw shut, wherever straight lines of at least three training walks cross the same wall (`walked_doors`). | |
| Floors | Height of each floor from the pressure at its landmarks, per walk, median over walks. | Nuremberg −2→−1→0: 2.46, 3.18 m. UMinho 0→1→2: 5.33, 3.86 m (IQR 0.1–0.2 m). |
| Heading | Android rotation vector, turned into the engine's convention as for MSIL. One map offset per site, fitted on straight training segments. | Offset −26.5° / −31.5°, circular spread 14–15°. |
| Radio map | Each transmitter goes on the floor where it is heard strongest, then position, 1 m power and exponent are fitted on that floor's training samples (`common/radiomap.py`, shared with MSIL). Surveyed beacons keep their position. The loss per slab is the median excess loss on the other floors. | Nuremberg: 26 Wi-Fi APs + 4 of 6 beacons, median RMS 5.7 dB, 2.0 dB per slab. UMinho: 99 Wi-Fi APs, 6.4 dB, 4.1 dB per slab. |
| Traces | Validation walks (tune) and scoring walks (holdout). IMU, barometer, BLE and Wi-Fi scans as batches, and `gt,t,x,y,heading,level,z` rows at the landmarks. | 27 tune + 6 holdout traces. Two walks recorded on an Aquaris X5 Plus are skipped: it has no orientation sensor or magnetometer, so the trace has no absolute heading. |

**Map checks.**
- **Reachability.** Every pair of consecutive landmarks must be connected on a 0.2 m grid with 0.25 m
  clearance; a floor change must go through a connector both landmarks can reach. Nuremberg passes
  0 of 1,102 pairs unreachable. UMinho leaves 11 of 1,687 unreachable, all in scoring walks: doors to
  rooms the training walks never entered. Those were left alone rather than fixed with holdout data.
- **Straight-line crossings.** Training lines between landmarks cross no wall at either site. Lines in
  validation and scoring walks do, as expected, because those walks are not rectilinear.
- **Footprint.** All landmarks lie inside it. For UMinho the footprint is the convex hull of walls and
  landmarks, because walks there use outdoor patios and entrances.

`outdoor_ne` (UMinho) is a connector that isn't drawn. Training walks go from floor 0 at (82, 69) to
floor 1 at (105, 67) outdoors in about 20 s, so the ground rises there.

## Method

- Settings are chosen on the validation walks only (`manifest_tune.json`). The scoring walks
  (`manifest_holdout.json`) were replayed once, with the frozen settings.
- `datasets/ipin/evaluate.py` replays a manifest and reports per walk:
  - CEP50/CEP95;
  - the floor hit rate;
  - the competition metric: the 75th percentile of horizontal error plus 15 m per wrong floor, over the
    landmarks.
- With `--seeds N` it averages each walk over filter seeds 1..N. A walk is still one recording; the
  seeds only average out the filter's own randomness, which on four walks moves the medians by about 1 m.

## Results

Validation walks (tune), 5 seeds:

| Config | Nuremberg CEP50 / CEP95 [m] | floor hit | IPIN p75 [m] | UMinho CEP50 / CEP95 [m] | floor hit | IPIN p75 [m] |
|---|---:|---:|---:|---:|---:|---:|
| default `pf` | 9.5 / 41.0 | 71% | 33.4 | 19.4 / 47.4 | 29% | 49.7 |
| `msil_wifi.json` | **7.2 / 24.8** | **74%** | **27.2** | **13.1 / 37.6** | **41%** | **37.2** |
| + `rssi.sigma_db` 6 | 7.7 / 27.7 | 73% | 24.8 | 12.9 / 37.8 | 42% | 36.4 |
| + `rssi.sigma_db` 10 | 8.7 / 23.3 | 75% | 25.2 | 12.0 / 37.1 | 39% | 36.4 |
| + level change 0.3 (baro) / 0.2 (step) | 7.5 / 24.2 | 76% | 24.1 | 13.4 / 39.5 | 43% | 37.1 |
| + `recovery_max_fraction` 0.15 | 8.1 / 20.6 | 76% | 26.7 | 11.2 / 39.9 | 43% | 39.1 |
| + `rssi.wall_aware` off | 7.6 / 26.2 | 69% | 33.9 | 11.2 / 46.5 | 55% | 35.9 |

The variations on `msil_wifi.json` move the numbers by about as much as changing the seeds does, so
IPIN uses `data/configs/msil_wifi.json` unchanged.

Scoring walks (holdout), `msil_wifi.json`, 5 seeds:

| Walk | Phone | Barometer | CEP50 [m] | CEP95 [m] | Floor hit | IPIN p75 [m] |
|---|---|---|---:|---:|---:|---:|
| Nuremberg 2024 #1 | Galaxy A5 2017 | yes | 7.6 | 37.7 | 84% | 19.4 |
| Nuremberg 2024 #2 | Galaxy S7 | yes | 6.3 | 31.1 | 80% | 24.6 |
| Nuremberg 2023 #1 | Galaxy A5 2017 | yes | 7.7 | 28.4 | 70% | 33.3 |
| Nuremberg 2023 #2 | Galaxy S7 | yes | 8.4 | 41.0 | 70% | 35.7 |
| UMinho #12 | Galaxy A31 | no | 11.4 | 30.6 | 69% | 26.2 |
| UMinho #13 | Galaxy A5 2017 | yes | 15.7 | 50.1 | 59% | 38.5 |

With the default config the holdout medians are 11.6 m / 67% (Nuremberg) and 18.1 m / 46% (UMinho).
`tests/regression/ipin_baseline.json` gates the holdout walks: CTest `ipin_regression`, labels
`regression;realdata`, skipped until the data is converted.

**What changed the numbers.**
- **Level footprint (Nuremberg, tune, 5 seeds).** Without it, particles live on floor −1 across the
  whole building, and the barometer prefers that floor for anything between 0 and −2. The floor hit
  rate goes from 52% without the footprint to 74% with it. CEP50 (7.0 → 7.2 m) and IPIN p75
  (23.7 → 27.2 m) move within the noise of four walks.
- **UMinho BLE left out.** The packets come from unknown devices. Without them the tune IPIN p75 goes
  from 41.9 to 37.2 m and the floor hit from 37% to 41%, but CEP50 from 9.7 to 13.1 m. The decision
  rests on the technical annex, not on these mixed numbers.
- **Floor loss from Wi-Fi only.** With the BLE devices included, UMinho's fitted floor loss was 1.6 dB
  per slab; Wi-Fi alone gives 4.1 dB.

## Floor model

`data/configs/ipin.json` is `msil_wifi.json` plus five settings; all are off in the default config.

| Setting | What it does | Failure it targets |
|---|---|---|
| `pf.baro_jump` | Once the cloud's barometer residual has pointed at the next floor (more than half the gap) for 3 s, particles whose own residual fits that floor better move onto it inside the nearest stairs or lift within 25 m. | A walker takes the stairs while the estimate is 10–20 m from them. |
| `pf.baro_offset_walk_m` 0.01 | The particles' barometer offset drifts 0.01 m per update instead of 0.05 m. | At 0.05 m, resampling let the offset absorb a 2.5 m descent within about 20 s, before the jump could see it. Weather drift is about 0.05 m per minute. |
| `pf.baro_reseed_keep_reference` 0.5 | Half the new particles (recovery, ring proposals, re-seeds) take the cloud's barometer reference, not one fitted to their own floor. | A re-seed on the wrong floor fits the barometer by construction, so nothing rejects it. |
| `pf.calibrate_floor_loss` | Once converged, every admitted reading from another floor, predicted above -85 dBm, nudges one venue-wide slab loss by 0.02 x its residual. Added after the tables below (experiment 10). | The fitted 2–4 dB per slab is one number for a whole building. |
| `pf.floor_prior_weight` 3 | k-NN (k = 5) over the training Wi-Fi scans stored in the venue (`floor_fingerprints`) gives p(floor) per scan. It is applied as `3 · log p(level)`, and recovery injections draw their floor from it. | The radio model's 2–4 dB per slab barely separates floors; the fingerprints get the floor right for 93–96% of validation scans. |

Validation walks (tune), 5 seeds; the weight was swept from 0.25 to 4 on these walks only:

| Config | Nuremberg CEP50 / CEP95 [m] | floor hit | IPIN p75 [m] | UMinho CEP50 / CEP95 [m] | floor hit | IPIN p75 [m] |
|---|---:|---:|---:|---:|---:|---:|
| `msil_wifi.json` | 7.2 / 24.8 | 76% | 27.2 | 13.1 / 37.6 | 46% | 37.2 |
| + offset walk 0.01 | 8.0 / 21.7 | 73% | 28.3 | 12.3 / 38.9 | 46% | 35.5 |
| + keep reference 0.8 | 8.5 / 24.7 | 70% | 28.2 | 14.1 / 39.0 | 46% | 39.9 |
| + jump, walk 0.01 | 8.0 / 21.7 | 73% | 28.3 | 11.7 / 37.2 | 46% | 35.5 |
| + jump, walk 0.01, keep 0.5 | 6.6 / 22.5 | 80% | 20.9 | 11.7 / 38.9 | 46% | 35.5 |
| + floor prior 2 only | 7.3 / 21.8 | 84% | 16.1 | 12.6 / 37.8 | 56% | 38.2 |
| `ipin.json`, prior 1 | 6.8 / 22.0 | 83% | 20.3 | 12.6 / 38.5 | 60% | 36.3 |
| `ipin.json`, prior 2 | 6.6 / 20.8 | 86% | 13.8 | 11.4 / 37.0 | 63% | 34.5 |
| **`ipin.json` (prior 3)** | **6.8 / 22.0** | **86%** | **17.2** | **12.0 / 36.6** | **70%** | **32.5** |
| `ipin.json`, prior 4 | 7.2 / 21.5 | 86% | 13.8 | 11.6 / 36.8 | 72% | 33.9 |

Floor hit is the median over walks. Weights 2 to 4 differ by about the seed noise on Nuremberg's four
walks; 3 is the middle of that range.

Scoring walks (holdout), replayed once with the frozen settings, 5 seeds:

| Walk | `msil_wifi.json` CEP50 / CEP95 [m] | floor hit | IPIN p75 [m] | `ipin.json` CEP50 / CEP95 [m] | floor hit | IPIN p75 [m] |
|---|---:|---:|---:|---:|---:|---:|
| Nuremberg 2024 #1 | 7.6 / 37.7 | 84% | 19.4 | 7.3 / 51.0 | 81% | 23.5 |
| Nuremberg 2024 #2 | 6.3 / 31.1 | 80% | 24.6 | 7.0 / 31.4 | 76% | 27.1 |
| Nuremberg 2023 #1 | 7.7 / 28.4 | 70% | 33.3 | 7.9 / 39.7 | 76% | 29.0 |
| Nuremberg 2023 #2 | 8.4 / 41.0 | 70% | 35.7 | 8.6 / 40.4 | 72% | 34.9 |
| UMinho #12 (no barometer) | 11.4 / 30.6 | 69% | 26.2 | 13.4 / 37.5 | 64% | 33.7 |
| UMinho #13 | 15.7 / 50.1 | 59% | 38.5 | 14.2 / 50.9 | 83% | 30.4 |
| **Nuremberg median** | 7.6 / 34.4 | 75% | 29.0 | 7.6 / 40.1 | 76% | 28.1 |
| **UMinho median** | 13.5 / 40.4 | 64% | 32.3 | 13.8 / 44.2 | 73% | 32.1 |

The tune gains carry over only in part:
- **UMinho #13**, the scoring walk with a barometer: floor hit 59% → 83%.
- **Nuremberg's scoring walks** are 25–35 minutes long against the validation walks' 8–12. Their floor hit
  is about unchanged, and CEP95 gets worse on two of them.

Six walks are too few to call that a regression or a gain. So `ipin.json` stays opt-in, and
`ipin_regression` keeps gating `msil_wifi.json`. The web demo uses `ipin.json`. Its Nuremberg walk is a
validation walk; on the full recording, floor accuracy goes from 63% to 96%. The demo cuts the first
90 s ([In the web demo](#in-the-web-demo)).

On the synthetic scenarios (10 seeds), the jump changes nothing: their walkers always reach the stairs.
The other two settings trade:
- `baro_reseed_keep_reference` 0.5 improves kit_tour (mean 3.6 → 2.1 m, floor 94% → 99%), but raises
  office_floor_bleed's CEP95 (4.0 → 4.8 m).
- The 0.01 m offset walk costs `baro_drift_long` a little (CEP95 2.0 → 2.4 m), as expected for a scenario
  that injects weather drift.

The engine defaults are therefore unchanged.

**Slab-loss calibration** (`pf.calibrate_floor_loss`, added to `ipin.json` later, experiment 10 in
[experiments.md](experiments.md)). Validation walks, `ipin.json` with and without it. The engine then
already had the floor hysteresis, the track smoother and the automatic init proposal, so the "without"
row differs from the table above. CEP from `ipe-regress` (1 seed); floor hit and p75 from `evaluate.py`
(3 seeds):

| Config | Nuremberg CEP50 / CEP95 [m] | floor hit | IPIN p75 [m] | UMinho CEP50 / CEP95 [m] | floor hit | IPIN p75 [m] |
|---|---:|---:|---:|---:|---:|---:|
| without | 4.42 / 22.00 | 83% | 18.5 | 11.03 / 27.48 | 70% | 33.8 |
| **with** | **5.26 / 19.78** | **89%** | **14.9** | **10.78 / 27.66** | **69%** | **31.3** |

The competition metric improves in both buildings; the reported floor changes more often (flips 3.5 →
5.5 and 14 → 19 per walk, for 2 real changes). The scoring walks were not replayed for this setting.

## Failure modes

- **The floor follows the position.** Particles change floor only inside a connector. When the walker
  takes the stairs while the estimate is 10–20 m away, the cloud stays on the old floor until recovery
  re-seeds it: about 3 minutes on the first Nuremberg validation walk. The barometer only reports changes,
  and with the default offset random walk the cloud absorbs a one-floor change within about 20 s.
  `pf.baro_jump` with a smaller walk moves the cloud to the nearest stairs instead
  ([Floor model](#floor-model)).
- **The radio model loses the floor.** A 5-nearest-neighbour Wi-Fi fingerprint on training scans picks
  the right floor for 93% (UMinho) and 95% (Nuremberg) of validation scans. The engine's model puts
  each AP on one floor with one loss per slab for the whole venue, and the fit gives only 2–4 dB. That
  is a model limitation, not a data problem. The fingerprint floor prior (`pf.floor_prior_weight`) uses
  what the data holds; per-AP floor losses would be the model-side alternative.
- **The starting floor.** Without a barometer, or before the first floor change, the starting floor
  comes from radio alone. On UMinho validation walks that start on floor 0, `msil_wifi.json` often
  settles on floor 2 and never recovers. The floor prior lifts the median floor hit there from 46% to 70%.
- **Sloped ground.** UMinho's floor 0 includes outdoor areas up to 5 m above the floor's datum
  (barometer at the landmarks), which looks like a floor change.
- **Uncertainty is still too small**, as on MSIL: the 95% ellipse holds the truth for 34–42% of fixes
  (`ipe-regress` coverage95).

## In the web demo

The demo's menu has one walk per building under "Real recordings (IPIN competition, several floors)",
picked by a fixed rule: the shortest validation walk with a barometer that visits every floor. Each
floor shows its competition plan under the extracted walls, and the 3D view stacks the floors (Nuremberg's
floor −1 at the size of its landing). `datasets/ipin/export_demo.py` writes `data/real_ipin/` (venue,
GeoJSON export, plans, trace, `demos.json`); a unit test checks it against the venue export. The demo
replays with `data/configs/ipin.json`.

The demo traces are cut (`DEMO_START_S` in `export_demo.py`): Nuremberg starts 90 s in and UMinho 82 s
in, and both are shifted to start at 0.
- **Why.** The first landmark is tapped about 61 s in, and the walker only becomes visible on the 3D
  overview once it moves.
- **UMinho's cut** falls between two landmarks on the same floor, so the trace gets one truth row at 0 s,
  interpolated between them. Scoring skips the first 5 s, so that row is never scored.
- **Accuracy.** The filter starts cold at the cut. With `ipin.json` the trimmed Nuremberg walk has the
  floor right for 100% of its landmarks (88% before the slab-loss calibration), and UMinho for 30% (40%
  before; CEP50 7.5 -> 12.5 m). These are single walks. The UMinho validation set as a whole improved
  ([Floor model](#floor-model)), but this walk did not.

## Reproduce

```bash
datasets/ipin/fetch.sh
.venv/bin/pip install -r datasets/ipin/requirements.txt
.venv/bin/python datasets/ipin/convert.py --wifi --scans --fingerprints --debug --out datasets/ipin/out_scans \
    --data data/ipin
.venv/bin/python datasets/ipin/evaluate.py datasets/ipin/out_scans/manifest_tune.json \
    --config data/configs/msil_wifi.json --seeds 5   # or data/configs/ipin.json
ctest --test-dir build -R ipin_regression --output-on-failure
.venv/bin/python datasets/ipin/export_demo.py   # data/real_ipin/ for the web demo
```

Conversion takes about 5 minutes and writes debug PNGs (walls in red, landmarks in orange, transmitters
in green and blue, connectors in purple) to `datasets/ipin/out_scans/debug/`.
