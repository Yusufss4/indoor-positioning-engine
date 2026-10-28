# Indoor Positioning Engine

[![ci](https://github.com/Yusufss4/indoor-positioning-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/Yusufss4/indoor-positioning-engine/actions/workflows/ci.yml)

A C++20 indoor positioning engine that turns noisy phone sensor data (IMU + BLE RSSI) into a blue dot,
plus the tooling to measure it: a venue simulator, trace replay, an accuracy regression suite that gates CI,
Python visualisation and a WebAssembly build that runs the same engine in the browser.

**[Live demo (WebAssembly)](https://yusufss4.github.io/indoor-positioning-engine/)**

![particle filter converging](docs/img/pf_convergence.gif)

## What is inside

| Component | Description |
|---|---|
| **Pedestrian dead reckoning** | Step detection on accelerometer magnitude, Weinberg step length, gyro yaw integration, magnetometer heading |
| **Particle filter** | Fuses steps, heading and RSSI; walls kill particles that cross them and attenuate radio in the likelihood; per-particle step-length scale and device RSSI offset states; robust likelihoods; systematic resampling; augmented-MCL recovery; beacon integrity monitor |
| **EKF** | `[x, y, heading]` with Jacobian-based prediction, scalar RSSI/heading updates, chi-square gating and Joseph-form covariance update |
| **Weighted centroid** | RSSI-only baseline |
| **Simulator** | Pure-pursuit walker through a floor plan; synthesises 50 Hz IMU and BLE with fading, shadowing, wall loss, path-loss mismatch, device offsets, packet loss, magnetic anomalies, dead and moved beacons |
| **Replay + metrics** | Mean, RMSE, CEP50, CEP95, availability, time to first fix, convergence time, compute per update |
| **Regression suite** | 7 scenarios x 4 filter presets x N seeds compared against a stored baseline in CI |
| **WebAssembly** | Emscripten/embind build of the unchanged core with a canvas front end, deployed to GitHub Pages |
| **Agent tooling** | [`AGENTS.md`](AGENTS.md) with evaluation rules for coding agents; MCP server in [positioning-agent-tools](https://github.com/Yusufss4/positioning-agent-tools) |

## Results

Mean over 10 seeds per scenario; errors after a 5 s warm-up; compute time is single-threaded wall clock per
sensor update (IMU and BLE averaged) on a desktop CPU. Full table: [docs/results.md](docs/results.md).

| Scenario | What it stresses | PF CEP50 / CEP95 | PF without map | EKF | Weighted centroid |
|---|---|---|---|---|---|
| office_nominal | reference walk | **0.66 / 1.59** | 0.82 / 2.23 | 0.95 / 4.13 | 2.32 / 4.88 |
| office_nlos_heavy | walls lose 7 dB, map says 3.5 | **0.65 / 1.87** | 0.86 / 2.22 | 0.98 / 2.72 | 2.31 / 5.14 |
| office_device_offset | phone reads 9 dB low | **0.75 / 2.05** | 0.96 / 2.77 | 7.00 / 9.45 | 2.48 / 5.31 |
| office_mag_disturbance | magnetic anomalies, gyro bias | **1.09 / 2.63** | 1.45 / 3.29 | 1.60 / 3.85 | 2.28 / 4.93 |
| office_field_issue | 2 dead beacons, 1 installed 16 m off | **0.72 / 2.00** | 1.01 / 3.67 | 2.85 / 5.75 | 2.99 / 12.95 |
| office_step_scale | walker's step 17% shorter than modelled | **0.73 / 1.87** | 0.81 / 2.28 | 0.97 / 2.55 | 2.23 / 4.80 |
| airport_hall_sparse | 80 x 40 m hall, beacons 20 m apart, 35% loss | **1.30 / 7.25** | 1.41 / 7.58 | 1.61 / 10.47 | 4.73 / 12.85 |

![CEP95 per scenario](docs/img/benchmark_cep95.png)

The simulator and the filters share a model family, so absolute numbers are optimistic compared to
real venues. The comparisons between filters and the before/after deltas are the point. See
[docs/design.md](docs/design.md#known-model-limitations).

### Field issue to engine fix

`office_field_issue` reproduces a deployment fault. One beacon is mounted in a different room than the
map says, and two have dead batteries. The map-aware particle filter was initially *worse* than the
map-free one there (CEP95 5.4 m vs 2.4 m), because recovery particles locked onto the beacon's mapped
position (17 m error for 10 s). Widening the per-user priors and slowing the recovery trigger fixed it
without hurting the other scenarios (now 2.0 m). The investigation, including the variants that did
not work, is in [docs/experiments.md](docs/experiments.md).

![field issue before/after](docs/img/field_issue_before_after.png)

<details>
<summary>Tracks and error CDF on office_nominal</summary>

![tracks](docs/img/office_nominal_trajectories.png)
![cdf](docs/img/office_nominal_cdf.png)
![error over time](docs/img/office_nominal_error.png)

</details>

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure          # 50 unit tests + accuracy regression

# simulate a scenario and replay it through a filter
./build/tools/ipe-sim --scenario data/scenarios/office_nominal.json --out /tmp/trace.csv
./build/tools/ipe-replay --venue data/venues/office_floor.json --trace /tmp/trace.csv --filter pf --out /tmp/est.csv

# A/B experiment: does the beacon integrity monitor help, across every scenario and 10 seeds?
./build/tools/ipe-regress --scenarios data/scenarios --no-baseline --seeds 10 --presets pf \
    --set '{"pf":{"beacon_integrity":false}}'

# regenerate all figures in docs/
python3 -m venv .venv && .venv/bin/pip install -r viz/requirements.txt
.venv/bin/python viz/make_figures.py
```

WebAssembly build (requires emsdk):

```bash
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release && cmake --build build-wasm -j
python3 -m http.server -d build-wasm/site 8000   # open http://localhost:8000
```

![web demo](docs/img/web_demo.png)

## Engineering notes

- **Deterministic everywhere.** All sampling goes through a xoshiro256** generator with Box-Muller
  normals, because `std::normal_distribution` differs between standard libraries. The WebAssembly build
  (Clang/libc++) reproduces native GCC/libstdc++ metrics exactly for the same seed.
- **Spatial index.** Wall queries (motion constraint and radio wall-count) use a uniform grid. A property
  test checks it against brute force on 5000 random segments.
- **CI** builds with GCC and Clang with `-Werror`, runs unit tests under ASan and UBSan, enforces
  clang-format, runs the accuracy regression, builds the WASM module and deploys the demo.
- **Config as data.** Every tuning parameter is reachable through JSON merge patches (`--set`, `--config`).
  Unknown keys are rejected so experiment typos fail loudly.

## Repository layout

```
core/     engine: PDR, RSSI model, particle filter, EKF, WCL, venue + trace I/O
sim/      scenario -> synthetic trace
eval/     presets, config overrides, replay, metrics, benchmark/baseline comparison
tools/    ipe-sim, ipe-replay, ipe-regress
tests/    GoogleTest unit tests, regression baseline
data/     venues and scenarios
viz/      matplotlib figures and animation
web/      embind bindings and browser front end
docs/     design notes, file formats, experiment log, results
```

## License

MIT
