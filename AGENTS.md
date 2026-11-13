# AGENTS.md

Instructions for AI coding agents (Claude Code, Codex, Cursor) working in this repository.

## What this is

A C++20 indoor positioning engine: pedestrian dead reckoning plus BLE / Wi-Fi RSSI and a barometer, fused
by a map-aware grid filter (the posterior on a lattice of the venue's walkable cells, no random numbers).
A deterministic simulator produces traces with ground truth; a replay tool and a regression suite measure
accuracy. [docs/design.md](docs/design.md) explains the engine and maps each concern to its file;
[docs/extending.md](docs/extending.md) says where a new sensor, setting, motion model or dataset goes.

## Layout

- `core/` - the engine (`ipe::core`): what an app or the WASM build links. No I/O beyond venue, trace and
  config parsing. The grid filter is `core/src/filter/`, one file per kind of update.
- `sim/` - scenario JSON to a synthetic trace (IMU, BLE, barometer, ground truth).
- `eval/` - replay runner, metrics, regression comparison.
- `geo/` - GeoJSON import and export; native tooling only, not linked by the engine.
- `tools/` - `ipe-sim`, `ipe-replay`, `ipe-regress`, `ipe-import`. `examples/` - the engine as a library.
- `tests/` - GoogleTest unit tests and the regression CTests; `tests/regression/*.json` are the accepted
  accuracy baselines.
- `web/` - Emscripten bindings and the browser demo. `viz/` - figures and the README GIF.
- `data/venues`, `data/scenarios` - inputs. `data/geojson` is generated from `data/venues` by `ipe-import`;
  a unit test fails when it is stale.
- `data/configs` - config patches for `--config`: `msil_wifi.json` (Wi-Fi), `ipin.json` (plus the floor
  vote) and `fingerprint.json` (plus the fingerprint likelihood, for venues with fingerprint positions).
- `data/demo/{msil,ipin}` - recordings for the web demo, written by `datasets/*/export_demo.py`; a unit
  test checks them. `data/demo/kaggle` is the same for the Kaggle set, git-ignored (see below).
- `datasets/` - download and conversion scripts for the real datasets ([docs/datasets.md](docs/datasets.md)):
  - `msil` - the Microsoft Indoor Location sample (two malls); also converts the full Kaggle set
    (`--kaggle`). `tests/regression/msil_baseline.json` gates the held-out walks of `out_scans/`.
  - `ipin` - IPIN competition Track 3 (two multi-floor buildings); venues in `data/ipin/`.
    `tests/regression/ipin_baseline.json` gates the scoring walks of `out_fp/` with `fingerprint.json`.
  - `kaggle` - fetch, submission and demo export for the full Kaggle set.
  - `osm` - OpenStreetMap indoor data to a venue, and a walk planner that writes scenarios.
  - `common` - what the converters share; `offline.py` holds the offline steps (two-pass replay, gap
    filling, waypoint snapping). `evaluate.py` scores a manifest with the competitions' metrics.
  - Raw data and converter output (`raw/`, `out*/`) are git-ignored.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -LE regression --output-on-failure   # unit tests, a few seconds
ctest --test-dir build -L regression --output-on-failure    # accuracy gate, ~40 s
```

`-L regression` also runs `real_data_regression` and `ipin_regression` (label `realdata`, about 9 minutes
together), which are skipped unless `datasets/msil/out_scans/` and `datasets/ipin/out_fp/` exist
([docs/datasets.md](docs/datasets.md#reproduce)).

Sanitizers: `-DIPE_SANITIZE=address,undefined` with `CMAKE_BUILD_TYPE=Debug`.

## Rules

1. **Accuracy claims need numbers.** Any change that touches `core/` must be evaluated with
   `ipe-regress --no-baseline --seeds 10` before and after, and the summary must quote both. The filter is
   deterministic, so the seeds are the simulator's: ten different walks of each scenario. One walk is
   noise; do not tune on one run. A refactoring must give identical metrics (compare the `--json` outputs;
   only `compute_us_per_update` may differ).
2. **Never update a baseline in `tests/regression/` to make CI pass** unless the change is an intentional
   accuracy trade-off and the PR description explains which scenarios got worse and why.
3. **Do not edit scenarios to flatter the filter.** Scenarios encode real-world failure modes; if one looks
   wrong, say so instead of changing it. Every scenario must keep `wall_violations == 0` (`ipe-sim` exits 3
   otherwise).
4. **Determinism matters.** The filter draws no random numbers, and must not start to. The simulator's
   randomness goes through `ipe::Rng`; never use `std::*_distribution` (implementation-defined across
   standard libraries, would break the WASM build and the baseline).
5. **Separate model limitations from data problems.** Before changing the filter, check whether the error
   comes from the sensor model (walls not modelled, step length scale) or from the data (dead or moved
   beacons, magnetic anomalies). The scenario descriptions say which effect each one injects.
6. **Keep `core/` free of platform code, allocation in hot loops, and global state.** A radio update is
   O(cells) and a step O(cells x heading bins); keep them that way, and quote `--serial` timings for any
   change to those loops (compute is this filter's cost, see [docs/design.md](docs/design.md#cost)).
7. **Real data: tune and held-out stay apart.** Settings are chosen on `manifest_tune.json` only;
   `manifest_holdout.json` is replayed once to report.
8. **Offline steps are not the engine.** Numbers from `--two-pass`, `--fill` or `--snap` must never be
   quoted as what the engine does live.
9. **Nothing converted from the Kaggle data is committed.** Its competition rules forbid redistribution:
   only code and aggregate numbers. `data/demo/kaggle/` must stay git-ignored.
10. **New behaviour is off by default** behind a config field, until it has earned its place on the
    scenarios and the real walks. A config field must be listed in `core/src/config.cpp` (the build fails
    otherwise).
11. Format with `clang-format-18` (CI enforces it).

## Useful commands

```bash
./build/tools/ipe-replay --scenario data/scenarios/office_field_issue.json --out /tmp/est.csv   # one run
./build/tools/ipe-regress --scenarios data/scenarios --no-baseline --seeds 10 \
    --set '{"filter":{"beacon_integrity":false}}'                                               # A/B test
./build/tools/ipe-replay --print-config                                                         # every setting
.venv/bin/python datasets/evaluate.py datasets/ipin/out_fp/manifest_tune.json \
    --config data/configs/fingerprint.json                                                      # real walks
```

The companion repo `positioning-agent-tools` wraps these tools as an MCP server. It still calls the removed
`pf` presets (`--filter pf`, `--presets pf,wcl`) and needs updating before it works against this version.

## Common agent failure modes here

- Claiming an improvement from a single seed or a single scenario.
- "Fixing" a failing regression by loosening tolerances or regenerating the baseline.
- Treating `compute_us_per_update` from parallel runs as a performance measurement (use `--serial`).
- Comparing with a competition leaderboard on other walks or another metric than the competition's.
  [docs/datasets.md](docs/datasets.md#against-the-leaderboards) says which numbers are like for like.
- Judging smoothing by CEP alone. A change to what is reported must also quote `jitter_p95_m`,
  `jumps_per_min`, `level_flips` and, on walled maps, `wall_crossings_per_min`
  ([docs/formats.md](docs/formats.md#replay-outputs)).
- Running an exporter that rewrites `data/demo/` without checking the diff: those files are what the
  public demo serves.
