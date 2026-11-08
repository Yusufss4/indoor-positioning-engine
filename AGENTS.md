# AGENTS.md

Instructions for AI coding agents (Claude Code, Codex, Cursor) working in this repository.

## What this is

A C++20 indoor positioning engine: pedestrian dead reckoning plus BLE RSSI, fused by a map-aware particle
filter (the engine's only filter). A deterministic simulator produces traces with ground
truth; a replay tool and a regression suite measure accuracy.

## Layout

- `core/` - the engine. No I/O beyond venue/trace parsing; this is what a mobile SDK or WASM build links.
  [docs/design.md](docs/design.md#where-things-live) maps each concern to its file and lists the steps
  for adding a sensor.
- `sim/` - scenario JSON to synthetic trace (IMU, BLE, barometer, ground truth).
- `eval/` - the `pf` preset, config overrides, replay runner, metrics, regression comparison.
- `geo/` - GeoJSON / IMDF import and export (`ipe_geo`); native tooling only, not linked by the engine.
- `plan/` - beacon placement analysis (`ipe_plan`): coverage, Cramer-Rao bound and floor margin of a
 beacon layout from the map and RSSI model; native tooling only. `viz/placement_study.py` checks the
 scores against simulated CEP on thinned layouts.
- `tools/` - `ipe-sim`, `ipe-replay`, `ipe-regress`, `ipe-import`, `ipe-plan`.
- `tests/` - GoogleTest unit tests and the `accuracy_regression` CTest.
- `data/venues`, `data/scenarios` - inputs. `tests/regression/baseline.json` - accepted accuracy.
- `data/geojson`, `data/imdf` - generated from `data/venues` by `ipe-import`; a unit test fails when they
  are stale. `data/mappings` - GeoJSON property mapping tables.
- `data/configs` - engine config patches for `--config` (e.g. `open_venue.json`, `msil_wifi.json`, `ipin.json`
  = `msil_wifi.json` plus the opt-in floor model, see [docs/ipin.md](docs/ipin.md#floor-model)).
- `datasets/msil` - fetch and convert scripts for the Microsoft Indoor Location sample. The raw data and
  the converted `out/` (readings one by one) and `out_scans/` (`--scans`, one batch per Wi-Fi scan) are
  git-ignored; `tests/regression/msil_baseline.json` gates the held-out walks of `out_scans/`.
  `data/real` holds ten converted walks for the web demo, with shop walls (`convert.py --wifi --scans
  --walls doors --out datasets/msil/out_doors`); regenerate it with `datasets/msil/export_demo.py`
  (a unit test checks it against the venue export).
- `datasets/osm` - OpenStreetMap indoor data to venue (`convert.py`, from the committed Overpass snapshot
  in `data/osm/`, ODbL) and an A* walk planner that writes scenarios (`plan_walk.py`). They produce
  `data/venues/kit_informatikbau.json` and `data/scenarios/kit_tour.json`; the commands are in
  [docs/formats.md](docs/formats.md#openstreetmap-datasetsosm).
- `datasets/ipin` - IPIN competition Track 3 packages (2022 UMinho, 2023/2024 Nuremberg, CC BY) to venues and
  traces ([docs/ipin.md](docs/ipin.md)). Walls are extracted from the floor plans (`maps.py`) and corrected
  in `edits/<site>.json` (stairs, lifts, footprints). The venues are committed in `data/ipin/`; raw data and
  `out_scans/` are git-ignored. `tests/regression/ipin_baseline.json` gates the scoring walks
  (`ipin_regression`, label `realdata`). Settings are chosen on `manifest_tune.json` (validation walks)
  only. `evaluate.py` adds floor hit rate and the competition metric.
- `datasets/uji_ble` - UJI BLE fingerprints (CC BY): RSSI model study, venues in `data/uji_ble/` and
  standing-still traces ([docs/uji_ble.md](docs/uji_ble.md)).
- `data/real_ipin`, `data/real_uji` - web demo recordings written by `export_demo.py` in those folders
  (`data/real` is MSIL's and is rewritten by its own script). The demo reads every `demos.json`.
- `datasets/common` - helpers shared by the real-data converters (transmitter fit, wall geometry, georef,
  trace writer). MSIL output must stay byte-identical when they change.
- `viz/` - Python plotting; `web/` - Emscripten bindings and the browser demo.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -LE regression --output-on-failure   # unit tests, ~1 s
ctest --test-dir build -L regression --output-on-failure    # accuracy gate, ~3 s
```

`-L regression` also runs `real_data_regression` (label `realdata`), which is skipped unless
`datasets/msil/out_scans/` exists (see [docs/real_data.md](docs/real_data.md)). Settings for real data are
chosen on `manifest_tune.json` only; `manifest_holdout.json` is for reporting, not for tuning.

Sanitizers: `-DIPE_SANITIZE=address,undefined` with `CMAKE_BUILD_TYPE=Debug`.

## Rules

1. **Accuracy claims need numbers.** Any change that touches `core/` must be evaluated with
   `ipe-regress --no-baseline --seeds 10` before and after, and the summary must quote both. A single seed
   is noise; do not tune on one run.
2. **Never update `tests/regression/baseline.json` to make CI pass** unless the change is an intentional
   accuracy trade-off and the PR description explains which scenarios got worse and why.
3. **Do not edit scenarios to flatter the filter.** Scenarios encode real-world failure modes; if one looks
   wrong, say so instead of changing it. Every scenario must keep `wall_violations == 0`
   (`ipe-sim` exits 3 otherwise).
4. **Determinism matters.** All randomness goes through `ipe::Rng`; never use `std::*_distribution`
   (implementation-defined across standard libraries, would break the WASM build and the baseline).
5. **Separate model limitations from data problems.** Before changing the filter, check whether the error
   comes from the sensor model (e.g. walls not modelled, step length scale) or from the data
   (dead/moved beacons, magnetic anomalies). The scenario descriptions say which effect each one injects.
6. Keep `core/` free of platform code, allocation in hot loops, and global state. Particle updates are
   O(N) per measurement; keep them that way.
7. Config fields are exposed through `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT` in
   `eval/src/eval.cpp`. A new config field must be added there or `--set` overrides will not reach it.
8. Format with `clang-format-18` (CI enforces it).

## Useful commands

```bash
# One run with per-sample output for plotting
./build/tools/ipe-replay --scenario data/scenarios/office_field_issue.json --out /tmp/est.csv

# A/B experiment across all scenarios
./build/tools/ipe-regress --scenarios data/scenarios --no-baseline --seeds 10 \
    --set '{"pf":{"beacon_integrity":false}}'

# Effective config of a preset
./build/tools/ipe-replay --print-config
```

The companion repo `positioning-agent-tools` exposes these tools as an MCP server.

## Common agent failure modes here

- Claiming an improvement from a single seed or a single scenario.
- "Fixing" a failing regression by loosening tolerances or regenerating the baseline.
- Adding config fields without registering them for JSON (silently ignored by presets, rejected by `--set`).
- Treating `compute_us_per_update` from parallel runs as a performance measurement (use `--serial`).
- Judging smoothing by CEP alone. A change to what is reported must also quote `jitter_p95_m`,
  `jumps_per_min` and `level_flips` (in `ipe-regress` tables; definitions in
  [docs/formats.md](docs/formats.md#replay-outputs)).
