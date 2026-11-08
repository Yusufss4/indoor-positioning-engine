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

## Failure modes

- **The floor follows the position.** Particles change floor only inside a connector. When the walker
  takes the stairs while the estimate is 10–20 m away, the cloud stays on the old floor until recovery
  re-seeds it: about 3 minutes on the first Nuremberg validation walk. The barometer cannot fix this alone,
  because it only reports changes.
- **The radio model loses the floor.** A 5-nearest-neighbour Wi-Fi fingerprint on training scans picks
  the right floor for 93% (UMinho) and 95% (Nuremberg) of validation scans. The engine's model puts
  each AP on one floor with one loss per slab for the whole venue, and the fit gives only 2–4 dB. That
  is a model limitation, not a data problem: per-AP floor losses or a fingerprint floor classifier
  would use what the data holds.
- **The starting floor.** Without a barometer, or before the first floor change, the starting floor
  comes from radio alone. On UMinho validation walks that start on floor 0, the filter often settles on
  floor 2 and never recovers.
- **Sloped ground.** UMinho's floor 0 includes outdoor areas up to 5 m above the floor's datum
  (barometer at the landmarks), which looks like a floor change.
- **Uncertainty is still too small**, as on MSIL: the 95% ellipse holds the truth for 34–42% of fixes
  (`ipe-regress` coverage95).

## In the web demo

The demo's menu has one walk per building under "Real recordings (IPIN competition, several floors)",
picked by a fixed rule: the shortest validation walk with a barometer that visits every floor. Each
floor shows its competition plan under the extracted walls, and the 3D view stacks the floors (Nuremberg's
floor −1 at the size of its landing). `datasets/ipin/export_demo.py` writes `data/real_ipin/` (venue,
GeoJSON export, plans, trace, `demos.json`); a unit test checks it against the venue export.

## Reproduce

```bash
datasets/ipin/fetch.sh
.venv/bin/pip install -r datasets/ipin/requirements.txt
.venv/bin/python datasets/ipin/convert.py --wifi --scans --debug --out datasets/ipin/out_scans --data data/ipin
.venv/bin/python datasets/ipin/evaluate.py datasets/ipin/out_scans/manifest_tune.json \
    --config data/configs/msil_wifi.json --seeds 5
ctest --test-dir build -R ipin_regression --output-on-failure
.venv/bin/python datasets/ipin/export_demo.py   # data/real_ipin/ for the web demo
```

Conversion takes about 3 minutes and writes debug PNGs (walls in red, landmarks in orange, transmitters
in green and blue, connectors in purple) to `datasets/ipin/out_scans/debug/`.
