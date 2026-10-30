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
- `footprint` (optional, `[[x, y], ...]`) is the outline of the walkable area on every floor, for buildings
  that fill only part of their bounds (an L or a U). The particle filter then starts, recovers and keeps
  particles inside it, and the beacon-ring proposal weights use its area. Without it the whole bounds
  rectangle is walkable; one covering 99% or more of the bounds is ignored.
- A file has either top-level `walls` (one floor, ordinal 0) or `levels`, not both.

### Rooms

`rooms` (optional) names the areas the engine reports Enter and Exit events for:

```json
"rooms": [
  {"id": "office_n1", "name": "Office N1", "level": 0, "area": [[0, 13], [10, 13], [10, 24], [0, 24]]}
]
```

- `id` is required and unique; `name` defaults to the id; `level` is an ordinal and defaults to the
  lowest floor. `area` is a polygon with at least three points and a non-zero area; a repeated closing
  point is dropped.
- Rooms are independent of walls: they add no walls, they may overlap (a shop inside a hall, a corridor
  crossing an open-plan area), and they need not cover the floor.
- Every committed venue has rooms: hand-drawn for the synthetic offices and the airport hall, mapped
  `indoor=room|corridor|area` outlines for the OSM building, and the named units (shops, toilets, ...)
  of the MSIL malls.

## Scenario (`data/scenarios/*.json`)

| Section | Key | Meaning |
|---|---|---|
| top | `venue` | Venue file, relative to the scenario file |
| top | `seed` | Default simulation seed |
| top | `demo_config` | Optional particle-filter config patch the web demo applies, relative to the scenario file. `ipe-regress` ignores it and uses the presets |
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
ble,t,beacon_id,rssi[,scan]
baro,t,pressure_hpa
gt,t,x,y,heading,level,z
```

- IMU in the phone frame (phone flat, x forward): m/s^2, rad/s, uT.
- `ble` rows with the optional `scan` column (a positive integer) are readings delivered together in one
  Wi-Fi scan. The rows of a scan share `t` and are contiguous; replay hands them to the engine as one
  batch (`PositioningEngine::on_scan`). Rows without the column are single packets, as before.
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

The `rooms` section tunes the room event detector (defaults shown; see
[design.md](design.md#room-enter-and-exit-events)):

```json
{"rooms": {"enter_prob": 0.6, "exit_prob": 0.3, "enter_dwell_s": 1.5, "exit_dwell_s": 1.5,
           "update_period_s": 0.25}}
```

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

- `--out`: `t,truth_x,truth_y,est_x,est_y,sigma_m,error_m,has_fix,truth_level,est_level,cov_xx,cov_xy,cov_yy`,
  one row per ground-truth sample.
- `--particles`: `t,x,y,w,level`, subsampled particle cloud every N ground-truth samples.
- `--events`: `t,source,room_id,room_name,type,probability`, the engine's room events (`source` is
  `estimate`) and the events of the ground-truth path (`truth`), sorted by time. `type` is `enter` or
  `exit`; `probability` is the room probability that fired the event (1 or 0 for truth). Names are
  quoted when they contain a comma or a quote.
- stdout / `--metrics`: JSON with `mean_m`, `rmse_m`, `cep50_m`, `cep95_m`, `max_m`, `availability`,
  `time_to_first_fix_s`, `convergence_s`, `steps`, `compute_us_per_update`, `floor_accuracy`,
  `floor_switch_latency_s`, `level_changes`, `coverage95`, `nees_median`, `room_events`,
  `room_event_precision`, `room_event_recall`, `room_event_latency_median_s`, `room_accuracy`.

`cep50`/`cep95` are the 50th/95th percentiles of horizontal error after a 5 s warm-up, over fixes on the
correct floor. `floor_accuracy` is the share of fixes on the correct floor (1.0 on a single-floor venue).
`floor_switch_latency_s` is the mean time from a true level change until the estimate reports the new
level (-1 if the walk never changes level).

`coverage95` and `nees_median` say whether the reported uncertainty is honest. For each scored fix,
NEES = e^T P^-1 e is the squared horizontal error measured against the reported covariance P.
`coverage95` is the share of fixes with NEES <= 5.991, i.e. inside the 95% ellipse; an honest filter
scores about 0.95, and lower means overconfident. `nees_median` is about 1.39 for an honest filter. Both
are -1 for filters that report no covariance (the weighted centroid). They are reported, not gated:
`ipe-regress` adds `coverage95` (and `median_coverage95` for recorded traces) to its JSON and tables.

The room metrics compare the engine's events with those of the ground-truth path (a detector with no
dwell and a 0.5 threshold on the true position). An estimated event matches the earliest unmatched true
event of the same room and type within 5 s. `room_events` counts true events after the warm-up;
`room_event_recall` is the share of them that were matched, `room_event_precision` the share of
estimated events after the warm-up that match one, and `room_event_latency_median_s` the median of
estimated minus true time over matches (dwell included, so about 1.5 s at best). Precision and recall
are 1 when there is nothing to score. `room_accuracy` is the share of scored samples whose set of
current rooms equals the true set. All are -1 for venues without rooms. They are reported, not gated.

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
`--wall-attenuation DB`, `--floor-attenuation DB`, `--min-wall M` (drop wall pieces shorter than this
after doors are cut; default 0.01), `--origin LAT,LON[,ROT]`. Warnings (unmapped feature
types, connectors that serve one floor, missing beacons) go to stderr.

### Exported GeoJSON

One `FeatureCollection` in WGS84, one feature per line. `properties.feature_type` is one of:

| `feature_type` | Geometry | Properties |
|---|---|---|
| `venue` | footprint, else bounds polygon | `name`, `wall_attenuation_db`, `floor_attenuation_db`, `origin_lat`, `origin_lon`, `rotation_deg` |
| `level` | bounds polygon | `level`, `ordinal`, `name`, `elevation_m`, `height_m` |
| `wall` | 0.2 m wide rectangle around the wall | `level`, `elevation_m`, `height_m` |
| `stairs` / `elevator` / `escalator` | connector footprint | `id`, `level` (lowest), `levels`, `elevation_m`, `height_m` (shaft) |
| `space` | room area | `id`, `name`, `level`, `elevation_m` |
| `beacon` | point | `id`, `level`, `elevation_m`, `tx_power`, `path_loss_exp` |

The committed exports in `data/geojson/` feed the 3D view; a unit test fails when they are stale.

### Mapping tables (`data/mappings/*.json`)

The GeoJSON importer is driven by a table that says which property classifies a feature and which values
mean what. Supporting another platform's export means writing a table, not code.

```json
{
  "type_property": "kind",
  "types": {"wall": [...], "room": [...], "opening": [...], "level": [...], "beacon": [...], "venue": [...],
            "stairs": [...], "elevator": [...], "escalator": [...], "space": [...]},
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
- `venue` features: the largest polygon ring becomes the venue `footprint` (holes are ignored).
- `space` features: the outer ring of the (first) polygon becomes a venue room, with its `id` and `name`
  properties; it adds no walls. A space without an id gets `space_N`; a reused id gets a `_2`, `_3`, ...
  suffix and a warning. Spaces with no area are dropped with a warning. A feature that should be both a
  wall outline and a room event area is listed twice, once as `room` and once as `space`.
- `level` values can be numbers or numeric strings. Without a venue georef, the local frame is anchored at
  the south-west corner of the level outlines.
- `ipe.json` describes the exported scheme (also the built-in default).

### IMDF subset

`ipe-import imdf` reads `venue.geojson`, `level.geojson` (required), `unit.geojson` and `opening.geojson`:

- Level `ordinal` and `name` (the `en` label) map directly. Features refer to levels through `level_id`.
  The venue polygon becomes the `footprint`.
- Unit categories `structure`, `concrete`, `brick`, `drywall`, `glass`, `wood` and `column` are walls;
  `stairs`, `elevator` and `escalator` are connectors; open areas (`walkway`, `lobby`, `opentobelow`,
  `unenclosedarea`, ...) are skipped; every other unit is a room.
- Openings cut doors out of the room walls.
- IMDF has no beacons and no elevations, so these come from extensions written by `export-imdf`:
  `ipe:georef`, `ipe:wall_attenuation_db` and `ipe:floor_attenuation_db` on the venue,
  `ipe:elevation_m` and `ipe:height_m` on levels, `ipe:connector_id` on connector units, and a
  `beacons.json` sidecar (`{"beacons": [{"id", "lat", "lon", "level", "tx_power", "path_loss_exp"}]}`).
- Venue rooms are exported as `room` units with the room name as their label and an `ipe:room_id`
  property. On import, a unit with `ipe:room_id` becomes a venue room and adds no walls; third-party
  units are walls only.
  Third-party IMDF works without them: levels are stacked `--level-height` apart, the local frame is
  anchored at the south-west corner, and beacons come from `--beacons` or `--beacon-grid`.
- `export-imdf` writes manifest, venue, level, unit (walls as `structure` units, connectors per floor)
  and an empty opening file. It is a subset: no addresses, buildings, footprints, anchors or amenities,
  so a strict IMDF validator will ask for more.

`data/imdf/office_two_floor/` is exported from `data/venues/office_two_floor.json`; the tests import it and
check walls, beacons, levels and connectors match within 1 cm.

### OpenStreetMap (`datasets/osm/`)

OpenStreetMap indoor data ([Simple Indoor Tagging](https://wiki.openstreetmap.org/wiki/Simple_Indoor_Tagging))
needs geometry work before `ipe-import`, so `datasets/osm/convert.py` writes GeoJSON in the `ipe` scheme:

- `indoor=room` outlines (ways and multipolygons) are rooms on every level they list, `indoor=wall` ways
  are walls, and the building outline is a wall on every level and the venue polygon, so it becomes the
  `footprint`. Corridors and areas have walls only where rooms border them, so they add none.
- Door nodes within 10 cm of a room or building outline cut a doorway of their `width` (default 1 m),
  centred on the node.
- Closed stairs (`highway=steps`, `stairs=yes`, `room=stairs`, `indoor=stairs`) and lifts
  (`highway=elevator`, `room=elevator`) listing two or more levels (`level=-1;0;1`) are connectors.
- Beacons go on a 10 m grid, keeping points inside a mapped room, corridor or area of each level.
- Every `indoor=room`, `corridor` and `area` outline is also a `space` (a venue room for Enter/Exit
  events), one per level it lists, with id `osm_<w|r><id>` (plus `_L<level>` for several levels and
  `_<k>` for several outer rings) and its `ref` and `name` as the name. Identical outlines on the same
  level are written once.

The Overpass response is committed (`data/osm/kit_informatikbau.osm.json`, (c) OpenStreetMap
contributors, [ODbL](../data/osm/LICENSE)), so the venue regenerates without network access; `--fetch`
refreshes it. Rebuild the venue and the planned walk with:

```bash
python3 datasets/osm/convert.py --out /tmp/kit.geojson
./build/tools/ipe-import geojson /tmp/kit.geojson --name kit_informatikbau --level-height 3.5 --min-wall 0.2 \
    --out data/venues/kit_informatikbau.json
./build/tools/ipe-import export data/venues/kit_informatikbau.json --out data/geojson/kit_informatikbau.geojson
.venv/bin/python datasets/osm/plan_walk.py --venue data/venues/kit_informatikbau.json --name kit_tour \
    --seed 501 --demo-config ../configs/open_venue.json --out data/scenarios/kit_tour.json \
    --goals 66,13.5,1 20,12,1 7,35,1 @osm_w1120093940 13,45,2 13,68,2 @osm_w1120094033 13,60,0 13,35,0 \
            7,25,0 38,8,0
```

`--min-wall 0.2` drops wall stubs shorter than the exported wall thickness, left where doorways meet
corners. `plan_walk.py` works on any venue: goals are `x,y,level`, snapped to the nearest point the
walker can reach, and `@ID` names the stairwell or lift between two levels. Without `--description` it
keeps the description already in the `--out` file. Each leg is A* on a 0.2 m
grid that keeps 0.25 m from walls, shortened to straight segments with the same clearance. The OSM
venue is not in the exact GeoJSON round-trip test: its walls are only nearly collinear, and a second
import merges a few more of them.
