# Extending the engine

Where a change goes, in the order people usually need it. [design.md](design.md) explains what each part
does; [AGENTS.md](../AGENTS.md) has the rules for measuring a change.

**Before and after any change to `core/`:**

```bash
./build/tools/ipe-regress --scenarios data/scenarios --no-baseline --seeds 10 --json before.json
# ... change ...
./build/tools/ipe-regress --scenarios data/scenarios --no-baseline --seeds 10 --json after.json
```

The filter is deterministic. A refactoring must give identical metrics (only `compute_us_per_update`
differs between runs); a change of behaviour must be explained by the rows that moved.

## Use it as a library

Link `ipe::core` and include `ipe/engine.hpp`. [`examples/minimal.cpp`](../examples/minimal.cpp) is the
whole pattern, built and run by CTest:

```cpp
const ipe::Venue venue = ipe::load_venue_file("venue.json");
ipe::PositioningEngine engine(venue);                       // with recommended_config(venue)

// or with settings of your own (include ipe/config.hpp):
ipe::EngineConfig config = ipe::recommended_config(venue);
ipe::apply_config_patch(config, R"({"smoothing": {"max_correction_mps": 2.0}})");
ipe::PositioningEngine tuned(venue, config);

engine.on_imu(sample);                 // 50 Hz accelerometer, gyroscope, magnetometer
engine.on_scan(t, readings);           // a Wi-Fi scan: {beacon index, dBm} per access point
engine.on_ble(t, "beacon-id", rssi);   // one BLE advertisement
engine.on_pressure(t, hpa);            // barometer
engine.on_position_fix(t, {x, y}, level, 1.0);   // a position known from outside: a tap, a QR code
const ipe::Estimate fix = engine.estimate();   // position, level, covariance, valid
```

- Events must arrive in time order. `engine.estimate()` is what to draw; `raw_estimate()` is the filter's
  own, unsmoothed.
- `recommended_config(venue)` picks the settings from what the venue contains: the defaults for surveyed
  beacons, the Wi-Fi settings with the floor vote for a venue with training scans, and the fingerprint
  likelihood on top when those scans carry positions (the three files in `data/configs/`, see
  [formats.md](formats.md#config-patches-dataconfigsjson)). A Wi-Fi venue without training scans cannot be
  told from a beacon venue: give it `data/configs/msil_wifi.json`.
- From CMake, as part of your build: `add_subdirectory(indoor-positioning-engine)` or `FetchContent`, then
  `target_link_libraries(app PRIVATE ipe::core)`. Inside another project only the library is built, with
  that project's own compiler flags.
- From CMake, installed: `cmake --install build --prefix <dir>` installs the library, its headers, the
  tools and a package; then `find_package(ipe REQUIRED)` with `<dir>` in `CMAKE_PREFIX_PATH`, and the same
  `ipe::core`. The CTests `package_install` and `package_subdirectory` build the example both ways.
- Building the lattice is the expensive part of start-up on a large venue. Build a `GridMap` once and pass
  it to every engine on that venue (`PositioningEngine`'s third constructor argument).
- The estimate is valid once a radio reading or a position fix has arrived. Without radio, a fix at the
  start is what the engine walks on from, with steps and the map.
- In the browser, `web/bindings.cpp` has two embind classes: `Demo` replays a trace or a scenario, and
  `Live` is the engine fed one event at a time (`onImu`, `onBle`, `onPressure`, `onPositionFix`), keeping
  what it was fed as a trace. `web/live.js` feeds it from a phone's motion sensors and is the place to
  see how browser sensor data becomes the engine's frame; `web/check_live.py` tests the page without a
  phone. A native app does the same against `PositioningEngine` directly.

## A config field

1. Add the field with its default and a `///<` comment to the struct (`GridFilterConfig` in
   `core/include/ipe/grid_filter.hpp`, or `PdrConfig`, `RssiModelConfig`, `BaroConfig`,
   `TrackSmootherConfig`, `RoomEventConfig`).
2. Add its name to that struct's `IPE_CONFIG_JSON(...)` line in `core/src/config.cpp`. The build fails
   until you do: a field that is not listed there could never be reached by a patch.

It is then available as `--set '{"filter": {"my_field": 1.5}}'`, in `data/configs/*.json` and in
`--print-config`. Keep a new behaviour off by default until it has earned its place on the scenarios and
the real walks.

## A measurement

A measurement is a log-likelihood per cell. The radio update (`GridFilter::update_rssi` in
`core/src/filter/radio.cpp`) shows the pattern:

```cpp
std::fill(loglik_.begin(), loglik_.end(), 0.0);
add_radio_model_loglik(readings);                     // each term adds to loglik_[cell]
if (neighbour_count_ > 0) add_fingerprint_loglik();
if (floor_prior_active_) add_floor_vote_loglik();
apply_cell_loglik(t);                                 // multiply the masses, normalise, update the estimate
```

**Another reading of the same scan** (say, a learned fingerprint model): add an `add_*_loglik()` next to
these, with its weight as a config field that is 0 by default.

**A new sensor** (a UWB range, a time-of-flight distance to an access point). The position fix
(`core/src/filter/position_fix.cpp`, added for taps, tags and GNSS at an entrance) went through exactly
these steps and is the shortest complete example:

1. `GridFilter::on_x(t, ...)` in a new file under `core/src/filter/`: fill `loglik_` for every cell with
   mass, then `apply_cell_loglik(t)`. `pressure.cpp` is the shortest example, including a per-cell nuisance
   state (the barometer offset). Per cell you have the centroid (`cx_`, `cy_`), the level
   (`map_->cell(c).level`) and the mass. Keep it O(cells): no allocation, no per-cell wall tests that are
   not cached in `GridMap`.
2. `PositioningEngine::on_x` in `core/src/engine.cpp`: call the filter, then `after_update(t)`.
3. To replay and simulate it: a stream in `Trace` with its CSV row (`core/src/trace.cpp`,
   [formats.md](formats.md#trace-csv)), a case in `TracePlayer::step`
   (`core/include/ipe/trace_player.hpp`), and the generator in `sim/src/simulator.cpp`. Every replay, the
   regression suite and the web demo then pick it up.
4. A unit test with a known answer (`tests/test_grid_filter.cpp` has point-mass and two-room helpers) and
   a scenario that stresses it.

Use a robust likelihood (`filter_detail::mixture`, Gaussian plus uniform): one bad reading must not be
able to wipe out the posterior.

## A motion model

The filter consumes `StepEvent`s: a time, a length, the turn since the previous step and optionally an
absolute heading. The built-in detector (`core/src/pdr.cpp`) produces them from IMU samples. To replace it
with a model of your own (a learned stride and heading network, a wheel odometer):

```cpp
engine.on_step({.t = t, .length = metres, .heading_delta = radians});   // instead of on_imu
engine.on_heading(t, heading, sigma);                                   // an absolute heading, any source
```

The filter spreads each step along track with `step_length_sigma` and `step_length_rel_sigma` and in
heading with `heading_sigma_deg`; set them to your model's errors. To evaluate such a model with the
tools, traces would need a `step` stream (see "A new sensor", step 3); that does not exist yet.

Whole-walk (offline) processing lives outside the engine, in `datasets/common/offline.py`: two-pass
replay, gap filling and snapping work on the replay's per-sample output and must never be quoted as live
results.

## A dataset

A converter turns recordings into the three things every tool reads:

- **Venues** (`venue.json` per building or floor, [formats.md](formats.md#venue-datavenuesjson)): bounds,
  walls, beacons or access points, optionally `floor_fingerprints` with positions.
- **Traces** (`*.csv`, [formats.md](formats.md#trace-csv)): IMU in the engine's frame, radio readings,
  pressure, and `gt` rows wherever ground truth exists.
- **Manifests** (`manifest.json`, `manifest_tune.json`, `manifest_holdout.json`): which trace runs on
  which venue, in groups. Settings are chosen on the tune manifest; the held-out one is for reporting.

`datasets/common/` has the parts converters share:

| Module | What it gives |
|---|---|
| `trace.py` | `TraceWriter` for the CSV, `write_manifests` |
| `radiomap.py` | fitting access points (`fit_samples`), merging virtual access points |
| `fingerprints.py` | the venue's `floor_fingerprints` table and fingerprint-only beacons |
| `geom.py` | wall crossings, cutting doorways, the heading offset between phone and map |
| `georef.py` | local metres to WGS84 |
| `offline.py` | the offline steps |
| `demo.py` | what the web demo's exporters share |

`datasets/ipin/convert.py` is the model for a multi-floor dataset with raster plans, and
`datasets/msil/convert.py` for a per-floor one with vector plans. Then:

```bash
.venv/bin/python datasets/evaluate.py datasets/mine/out/manifest_tune.json --config data/configs/fingerprint.json
./build/tools/ipe-regress --traces datasets/mine/out/manifest_holdout.json --no-baseline
```

- **A regression gate**: an `add_test` in `tests/CMakeLists.txt` like `ipin_regression`, with a baseline
  written by `ipe-regress --traces ... --baseline FILE --update`.
- **In the web demo**: an `export_demo.py` that writes `data/demo/<name>/` with a `demos.json` index
  (`datasets/common/demo.py`), and one line in `RECORDING_SOURCES` in `web/app.js`. Commit the data only if
  its licence allows redistribution; otherwise add the folder to `.gitignore`, as `data/demo/kaggle/` is.

## A scenario

`data/scenarios/<name>.json` ([formats.md](formats.md#scenario-datascenariosjson)): a venue, waypoints and
the faults to inject. `ipe-sim` exits with 3 if the walk crosses a wall. A scenario encodes a failure
mode, so it is added with the fault it stresses in its description, and with its row in
`tests/regression/baseline.json` (`ipe-regress --update`, explained in the commit). For a walk through a
real building, `datasets/osm/plan_walk.py` plans one through the mapped doors and stairs.
