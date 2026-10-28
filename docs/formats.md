# File formats

## Venue (`data/venues/*.json`)

```json
{
  "name": "office_floor",
  "bounds": [min_x, min_y, max_x, max_y],
  "wall_attenuation_db": 3.5,
  "walls": [[x1, y1, x2, y2], ...],
  "beacons": [{"id": "C1", "x": 5, "y": 11.5, "tx_power": -59, "path_loss_exp": 2.0}]
}
```

- Units are metres in a local east-north frame (x east, y north).
- Walls are line segments; they block pedestrians (particle-filter motion constraint) and attenuate radio by
  `wall_attenuation_db` each in the map-aware RSSI model.
- `tx_power` is the expected RSSI at 1 m. `path_loss_exp` is the log-distance exponent. These are the
  *surveyed* values; scenarios can make the real ones differ.

### Several floors

Instead of top-level `walls`, a multi-floor venue has `levels`, and may add `connectors` and a `georef`
(`data/venues/office_two_floor.json`):

```json
{
  "name": "office_two_floor",
  "bounds": [0, 0, 40, 24],
  "wall_attenuation_db": 3.5,
  "floor_attenuation_db": 15.0,
  "georef": {"origin_lat": 41.0151, "origin_lon": 28.9795, "rotation_deg": 0},
  "levels": [
    {"ordinal": 0, "name": "Ground", "elevation_m": 0.0, "height_m": 4.0, "walls": [[0, 0, 40, 0], ...]},
    {"ordinal": 1, "name": "First",  "elevation_m": 4.0, "height_m": 4.0, "walls": [...]}
  ],
  "connectors": [
    {"id": "stairs_sw", "kind": "stairs",   "levels": [0, 1], "area": [[0.5, 0.5], [5.5, 0.5], [5.5, 2.5], [0.5, 2.5]]},
    {"id": "lift_se",   "kind": "elevator", "levels": [0, 1], "area": [[37.5, 0.5], [39.5, 0.5], [39.5, 2.5], [37.5, 2.5]]}
  ],
  "beacons": [{"id": "C1", "x": 5, "y": 11.5, "level": 0}, ...]
}
```

- `ordinal` is the floor number as in IMDF (0 ground, -1 basement). Every `level` reference in venue and
  scenario files is an ordinal. Inside the engine, levels are indices into the list sorted by ordinal.
- `elevation_m` defaults to `ordinal * height_m`; `height_m` defaults to 4. Only elevation differences
  matter to the engine (barometer, 3D radio distance); `height_m` is also used for rendering.
- `floor_attenuation_db` is the radio loss per slab between beacon and phone (default 15).
- A connector's `area` polygon is the only place a pedestrian (and a particle) can change level. `kind`
  is `stairs`, `elevator` or `escalator`; `levels` lists two or more ordinals.
- `georef` anchors the local frame on the globe: `origin_lat`/`origin_lon` is local (0, 0), and
  `rotation_deg` is the counter-clockwise angle from east to local +x. It is needed for GeoJSON/IMDF export
  and the 3D view, and is optional for everything else.
- A file has either top-level `walls` (one floor, ordinal 0) or `levels`, not both.

## Scenario (`data/scenarios/*.json`)

| Section | Key | Meaning |
|---|---|---|
| top | `venue` | Venue file, relative to the scenario file |
| top | `seed` | Default simulation seed |
| `walk` | `waypoints` | Polyline followed by a pure-pursuit walker: `[x, y]` or `[x, y, level_ordinal]` |
| `walk` | `step_frequency_hz`, `step_length_m`, `step_length_sigma`, `turn_rate_deg_s` | Gait |
| `walk` | `stairs_speed_factor` | Horizontal speed on stairs relative to walking (0.6) |
| `walk` | `elevator_speed_mps`, `elevator_accel_mps2`, `elevator_dwell_s` | Lift ride profile; no steps while in the cabin |
| `imu` | `rate_hz`, `accel_noise`, `gyro_noise`, `gyro_bias` | Inertial sensor model |
| `imu` | `weinberg_k` | The walker's true Weinberg constant (the engine assumes 0.48) |
| `imu` | `mag_disturbances` | `[{center, radius, field}]` local magnetic anomalies (uT, world frame) |
| `ble` | `noise_db`, `shadowing_db`, `shadowing_tau_s` | Fast fading and AR(1) slow fading |
| `ble` | `wall_attenuation_db`, `path_loss_exp_delta` | True propagation vs. the surveyed model |
| `ble` | `device_offset_db` | Receiver RSSI bias (phone model differences) |
| `ble` | `dropout`, `rx_interval_s`, `sensitivity_dbm` | Packet reception |
| `ble` | `dead_beacons`, `moved_beacons` | Deployment faults: silent beacons, beacons not where the map says |
| `ble` | `floor_attenuation_db` | True loss per slab (14; the map's value may differ) |
| `baro` | (section present) | Turns on the phone barometer; `"baro": {}` uses the defaults below |
| `baro` | `rate_hz`, `noise_hpa` | 5 Hz, 0.03 hPa white noise |
| `baro` | `reference_hpa`, `drift_hpa_per_hour`, `drift_walk_hpa` | Weather: start pressure, trend, random walk per minute |
| `baro` | `device_offset_hpa` | Constant sensor bias |
| `baro` | `spike_rate_per_min`, `spike_hpa`, `spike_tau_s` | Door and ventilation transients |
| `baro` | `phone_height_m` | Phone height above the floor |

Two consecutive waypoints on different levels must both lie inside one connector that serves both levels;
the walker climbs the stairs or rides the lift between them. The simulator rejects a level change anywhere
else.

## Trace (`*.csv`)

Line-oriented, one sample per row, merged in timestamp order. Lines starting with `#` are comments;
`# key: value` comments are metadata.

```
# ipe-trace v1
# scenario: office_nominal
imu,t,ax,ay,az,gx,gy,gz,mx,my,mz
ble,t,beacon_id,rssi
baro,t,pressure_hpa
gt,t,x,y,heading,level,z
```

- IMU in the phone frame (phone flat, x forward): m/s^2, rad/s, uT.
- `baro` rows are optional phone barometer samples in hPa (Android `TYPE_PRESSURE`, iOS `CMAltimeter`
  reports kPa). Only pressure changes matter; the absolute value is never trusted.
- `gt` rows are optional ground truth; replay scores the estimate at each one. `level` is the level index
  and `z` the walking-surface height in metres. Single-floor traces keep the short `gt,t,x,y,heading` form,
  and both forms are accepted.
- A phone logger that writes this format can be replayed directly. `datasets/msil/convert.py` is a
  worked example: it rotates Android sensor data into the expected frame (see
  [real_data.md](real_data.md#conversion-datasetsmsilconvertpy)).

## Config patches (`data/configs/*.json`)

JSON merge patches over a preset's engine config, the same shape as `--print-config` output.
`ipe-replay` and `ipe-regress` take `--config FILE`. It is applied before any `--set`, and unknown keys
are rejected.

## Trace manifest (`ipe-regress --traces`)

Replays recorded traces (one run each) instead of simulating scenarios:

```json
{"traces": [
  {"name": "site1_F1_5d8f...", "group": "site1_F1", "venue": "site1/F1/venue.json",
   "trace": "site1/F1/5d8f....csv"}
]}
```

Paths are relative to the manifest. Results are reported per `group` plus an `all` group. They include
the median and mean over traces of each trace's mean error, CEP50 and CEP95, and traces without ground
truth after the warm-up are skipped. `--baseline` and `--update` work as for scenarios.

## Replay outputs

- `--out`: `t,truth_x,truth_y,est_x,est_y,sigma_m,error_m,has_fix,truth_level,est_level`, one row per
  ground-truth sample.
- `--particles`: `t,x,y,w,level`, subsampled particle cloud every N ground-truth samples.
- stdout / `--metrics`: JSON with `mean_m`, `rmse_m`, `cep50_m`, `cep95_m`, `max_m`, `availability`,
  `time_to_first_fix_s`, `convergence_s`, `steps`, `compute_us_per_update`, `floor_accuracy`,
  `floor_switch_latency_s`, `level_changes`.

`cep50`/`cep95` are the 50th/95th percentiles of horizontal error after a 5 s warm-up, over fixes on the
correct floor. `floor_accuracy` is the share of fixes on the correct floor (1.0 on a single-floor venue).
`floor_switch_latency_s` is the mean time from a true level change until the estimate reports the new
level (-1 if the walk never changes level).

## GeoJSON and IMDF (`ipe-import`)

`ipe-import` converts between the venue JSON and the formats indoor-mapping platforms export.

```bash
ipe-import export data/venues/office_two_floor.json --out office.geojson          # venue -> GeoJSON (WGS84)
ipe-import geojson office.geojson --out venue.json                                 # GeoJSON -> venue ("ipe" scheme)
ipe-import geojson their_export.geojson --mapping my_mapping.json --out venue.json
ipe-import export-imdf data/venues/office_two_floor.json --out imdf_dir/          # venue -> IMDF subset
ipe-import imdf imdf_dir/ --out venue.json                                         # IMDF -> venue
```

Import options: `--name`, `--beacons FILE`, `--beacon-grid M`, `--level-height M`,
`--wall-attenuation DB`, `--floor-attenuation DB`, `--origin LAT,LON[,ROT]`. Warnings (unmapped feature
types, connectors that serve one floor, missing beacons) go to stderr.

### Exported GeoJSON

One `FeatureCollection` in WGS84, one feature per line. `properties.feature_type` is one of:

| `feature_type` | Geometry | Properties |
|---|---|---|
| `venue` | bounds polygon | `name`, `wall_attenuation_db`, `floor_attenuation_db`, `origin_lat`, `origin_lon`, `rotation_deg` |
| `level` | bounds polygon | `level`, `ordinal`, `name`, `elevation_m`, `height_m` |
| `wall` | 0.2 m wide rectangle around the wall | `level`, `elevation_m`, `height_m` |
| `stairs` / `elevator` / `escalator` | connector footprint | `id`, `level` (lowest), `levels`, `elevation_m`, `height_m` (shaft) |
| `beacon` | point | `id`, `level`, `elevation_m`, `tx_power`, `path_loss_exp` |

The committed exports in `data/geojson/` feed the 3D view; a unit test fails when they are stale.

### Mapping tables (`data/mappings/*.json`)

The GeoJSON importer is driven by a table that says which property classifies a feature and which values
mean what. Supporting another platform's export means writing a table, not code.

```json
{
  "type_property": "kind",
  "types": {"wall": [...], "room": [...], "opening": [...], "level": [...], "beacon": [...], "venue": [...],
            "stairs": [...], "elevator": [...], "escalator": [...]},
  "level_property": "floor", "levels_property": "floors", "ordinal_property": "floor",
  "name_property": "name", "id_property": "uid", "elevation_property": "elevation", "height_property": "height",
  "tx_power_property": "txPower", "path_loss_property": "pathLossExponent",
  "wall_max_thickness_m": 0.6
}
```

- `wall` features: lines become wall segments. Rectangles thinner than `wall_max_thickness_m` become their
  centre line; any other polygon contributes its outline.
- `room` features: every polygon edge is a wall. Shared edges of adjacent rooms are merged into one wall, so
  they are not counted twice by the radio model.
- `opening` features (doors, any geometry): cut out of the walls they lie on (within half the wall
  thickness).
- Connector features: one feature per floor is normal. Footprints of the same kind whose centroids are
  within 1 m (or that share an id) become one shaft serving all their levels. A connector on one floor
  only is dropped with a warning.
- `level` values can be numbers or numeric strings. Without a venue georef, the local frame is anchored at
  the south-west corner of the level outlines.
- `ipe.json` describes the exported scheme (also the built-in default).

### IMDF subset

`ipe-import imdf` reads `venue.geojson`, `level.geojson` (required), `unit.geojson` and `opening.geojson`:

- Level `ordinal` and `name` (the `en` label) map directly. Features refer to levels through `level_id`.
- Unit categories `structure`, `concrete`, `brick`, `drywall`, `glass`, `wood` and `column` are walls;
  `stairs`, `elevator` and `escalator` are connectors; open areas (`walkway`, `lobby`, `opentobelow`,
  `unenclosedarea`, ...) are skipped; every other unit is a room.
- Openings cut doors out of the room walls.
- IMDF has no beacons and no elevations, so these come from extensions written by `export-imdf`:
  `ipe:georef`, `ipe:wall_attenuation_db` and `ipe:floor_attenuation_db` on the venue,
  `ipe:elevation_m` and `ipe:height_m` on levels, `ipe:connector_id` on connector units, and a
  `beacons.json` sidecar (`{"beacons": [{"id", "lat", "lon", "level", "tx_power", "path_loss_exp"}]}`).
  Third-party IMDF works without them: levels are stacked `--level-height` apart, the local frame is
  anchored at the south-west corner, and beacons come from `--beacons` or `--beacon-grid`.
- `export-imdf` writes manifest, venue, level, unit (walls as `structure` units, connectors per floor)
  and an empty opening file. It is a subset: no addresses, buildings, footprints, anchors or amenities,
  so a strict IMDF validator will ask for more.

`data/imdf/office_two_floor/` is exported from `data/venues/office_two_floor.json`; the tests import it and
check walls, beacons, levels and connectors match within 1 cm.
