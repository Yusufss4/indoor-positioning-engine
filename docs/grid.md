# Grid filter (prototype)

The engine finds the position with a particle filter. `grid/` is a different way to do it: a Bayes filter
on a grid of map cells that holds the whole posterior. It has no random particles at all. It is a
prototype next to the particle filter, not part of the engine:
- `PositioningEngine` and the baselines do not use it; the web demo runs it next to the particle filter
  (the "grid" checkbox), in a Web Worker;
- `ipe-grid` runs it and compares it with the particle filter on the same traces.

The results are experiment 14 in [experiments.md](experiments.md). In short:
- **Simulated scenarios** (10 seeds): mean CEP50 / CEP95 goes from 0.78 / 2.94 m to 0.66 / 1.83 m, with
  half the jitter and fewer jumps. The two-floor walks converge in 1–2 s.
- **Real walks:** better median CEP50 on MSIL, IPIN and UJI, and a much lower IPIN score. UJI's tune
  CEP95 is 2% worse.
- **Costs:** a covariance that is too tight, and compute. At the time the grid took 3–25× the particle
  filter's compute on the simulated venues and about real time on a mall floor. After the exact speed-ups
  under [Cost](#cost) it takes 2–20× the compute and about half of real time.

```bash
# one trace, metrics as JSON (the flags of ipe-replay)
./build/tools/ipe-grid replay --scenario data/scenarios/office_nominal.json

# the same traces through pf and the grid filter, as an ipe-regress table
./build/tools/ipe-grid compare --scenarios data/scenarios --seeds 10
./build/tools/ipe-grid compare --traces datasets/msil/out_scans/manifest_tune.json --config data/configs/msil_wifi.json

# settings: --grid-set for all columns, --variants for one column each
./build/tools/ipe-grid compare --scenarios data/scenarios --seeds 10 --no-pf \
    --variants '{"36":{}, "72":{"heading_bins":72}}'
```

`--config` and `--set` patch the engine config as usual. The grid filter takes PDR, the RSSI model, the
barometer, the smoother and the room settings from it. Its own settings are `GridFilterConfig`
(`grid/include/ipe/grid_filter.hpp`), patched with `--grid-set`; `ipe-grid replay --print-config` lists
them.

## Why a grid

Particles sample the posterior. Most of the particle filter's machinery exists because a sample covers
it sparsely:
- augmented-MCL recovery, and the trade-off it brings when a beacon misbehaves (experiment 2);
- the beacon-ring start, and the density rule that switches it on (experiments 4 and 9);
- rejection sampling around closed rooms (experiment 13);
- the re-seed when every particle dies;
- seed noise in every comparison.

A grid holds the probability of every cell, so the uniform prior is represented exactly from the first
update. A room without a door is just a cell that does not exist. Room probability is the mass of the
room's cells. The filter is deterministic: no `ipe::Rng`, no seed. It still uses the particle filter's
models, so the two can be compared.

## The map (`GridMap`)

Built once per venue and shared read-only by every run on it (`ipe-grid compare` keeps one per venue).
- **Cells.** One lattice over the venue bounds, 0.5 m cells (`cell_m`). Cells are coarser when the
  walkable area would need more than `max_cells` (200,000) of them. A cell exists on a level when its
  centre is inside the venue and the level's footprint, and outside closed regions (`ReachableArea`, the
  same rule as `pf.closed_region_*`).
- **Moves.** Each cell has one bit per lattice offset within `max_reach_m` (2 m, 4 cells): set when the
  segment between the two centres crosses no wall and the target cell exists. A step's wall test is one
  bit.
- **Radio.** The walls between each cell's centre and each beacon are counted the first time the beacon is
  heard (thread-safe), with exactly the result of `Venue::count_wall_crossings`. Testing every wall against
  every cell took about a second per beacon on the shop-wall mall floors. Instead, each wall's shadow seen
  from the beacon (the wedge behind it) is filled in lattice row by lattice row. Only the cells within
  10 µm of a shadow's edge get the segment test. This takes 1–5 ms per beacon on IPIN, KIT and site1_F2,
  and 10 ms on site2_F6 (1,217 walls): 25–100× faster.
- **Floors and rooms.** Cells inside a connector link to the cell at the same place on the levels next to
  it. Each cell lists the rooms its centre is in.

## The state

Kept per cell, so that a radio update costs O(cells):
- the cell's probability (mass);
- the centroid of that mass. Moves start from the centroid and land at exact points, so the mass moves
  exactly: no drift towards cell centres and no smearing between cells;
- the device RSSI offset, as a Gaussian;
- the barometer offset, as a Gaussian. Carrying these as Gaussians is exact Rao-Blackwellisation; the
  particle filter samples them instead.

Kept per (cell, heading bin), as `float`:
- the distribution of the heading within the cell, over 36 bins (`heading_bins`);
- optionally a persistent step scale with `step_scale_bins` values.

The heading bins are offsets from the gyro-integrated yaw (heading = yaw + 10° × k). A step's gyro turn
therefore shifts every bin exactly, with no interpolation. Only the 3° per step process noise moves mass
between bins, as a 3-tap kernel with that variance.

## Updates

They use the particle filter's models and, where a setting has a counterpart, its default.
- **Step.**
  1. Apply the gyro turn and the heading kernel.
  2. Every (cell, heading, scale) moves along track to three Gauss-Hermite lengths `L ± √3 σ`, weighted
     1/6, 2/3, 1/6, with σ = √(0.08² + (0.2 L)²). These match the mean and variance of the length noise.
     The relative part (`step_length_rel_sigma`) also stands in for the step-length scale the particle
     filter carries per particle; a scale state (`step_scale_bins` 3) was worse overall.
  3. Mass whose move is not allowed by the mask dies at the wall.
  4. The rest is scattered into the target cells together with its centroid and the moments of both
     offset Gaussians (moment matching).
  5. In connector cells 8% of the mass moves to the levels next to it.
  `step_motion()` is the mean move of the mass that survived, as `ParticleFilter::step_motion()`.
- **Magnetometer.** The particle filter's wrapped Gaussian with 15% uniform, evaluated once per heading
  bin.
- **RSSI.** For every cell:
  - the prediction is the path loss at the centroid (3D distance and slab loss across levels), minus the
    wall loss times the cell's wall count, plus the cell's mean device offset;
  - its variance is σ² plus the offset's variance, in the same Gaussian/uniform mixture;
  - the offset Gaussian is updated by an assumed-density Kalman step, weighted by the reading's inlier
    responsibility.

  A scan applies its readings one after another and renormalises once. The readings of one scan share
  the device offset, so they are not counted as fully independent of each other. Readings first pass the
  same `BeaconIntegrityMonitor` as in the particle filter, judged at the cloud estimate.
- **Floor prior.** On venues with Wi-Fi floor fingerprints (the IPIN buildings), each scan also adds
  `floor_prior_weight · log p(level)` to every cell, with p from the particle filter's k-NN
  `FloorClassifier` ([ipin.md](ipin.md#floor-model)). By default the grid takes `pf.floor_prior_*` from
  the engine config, so `--config ipin.json` turns it on for both filters; `--grid-set
  '{"floor_prior_weight":0}'` turns it off. Without it, nothing rejects mass that walks the same path one
  floor up: the radio model's 2–4 dB per slab barely separates floors, and every level's barometer offset
  was fitted to it at the first reading. On the Nuremberg demo walk the grid spent 84% of 10–60 s on
  Floor 0 while the walker stayed on Floor -2 (0% with the prior). Tune walks, `ipin.json`, prior off →
  on (median CEP50 / CEP95, floor hit, p75):
  - Nuremberg, 4 walks: 5.90 / 19.80 m, 91%, 10.20 m → 5.73 / 17.50 m, 95%, 8.07 m;
  - UMinho, 23 walks: 8.75 / 27.22 m, 40%, 32.36 m → 8.03 / 23.64 m, 82%, 18.68 m.

  Floor hit rises or holds on all 27 walks; p75 is better on 22 and worse on 5 (by 0.9–4.1 m). The
  scoring walks, replayed once with these settings, carry the gain over in part, as they did for the
  particle filter:
  - UMinho, 2 walks: floor hit 56% → 84%, p75 32.19 → 25.02 m, CEP95 39.65 → 36.01 m (both walks better);
  - Nuremberg, 4 walks (25–35 min against the tune walks' 8–12): median floor hit 81% → 83%, p75 18.56 →
    17.99 m, but CEP95 30.39 → 55.06 m. Two walks improve (p75 34.9 → 13.7 and 16.9 → 12.2 m); two get
    worse (p75 18.1 → 35.4 and 19.1 → 22.3 m, CEP95 32 → 79 and 29 → 100 m).

  What is left is the stairs. Mass changes level only inside a connector, and dead reckoning can carry it
  out of a small stairwell before the barometer has seen half the climb: the mass moved up too early is
  rejected, the rest is stranded on the old floor. The grid then reaches the new floor through recovery
  mass, whose barometer offset is fitted to its own level and so matches the reading on any floor. On the
  Nuremberg demo walk that takes until 248 s for a climb at 240–256 s, and the floor prior pulls the grid
  back to the old floor for 252–264 s, where the classifier is unsure near the stairs. Giving recovery mass
  the distribution's barometer reference instead (as `pf.baro_reseed_keep_reference`) closes that route:
  on the tune walks Nuremberg's floor hit fell from 95% to 77% and its p75 rose from 8.07 to 25.34 m. The
  fix would be a barometer jump like `pf.baro_jump`.
- **Barometer.** Every 0.5 s, the particle filter's mixture likelihood and an assumed-density update of
  the offset, with a 0.05 m random walk. The first sample sets each cell's offset from its own level, and
  lift rides move 10% of each connector cell's mass to the next level, as in the particle filter.
- **Recovery.** Instead of injecting particles, the filter mixes in uniform mass, at most every 0.5 s
  (`teleport_period_s`):
  - at a constant rate, `teleport_rate_per_s`. The default, 1e-8 per second, is tiny on purpose: mass
    that is everywhere at once gives a moved beacon or one Wi-Fi scan a foothold wherever the map allows
    one;
  - optionally also augmented MCL's trigger (`recovery_max_fraction` > 0): 1 − w_fast / w_slow of the mass
    when the radio's predictive likelihood drops. It fires exactly when a beacon misbehaves and is off.

  The new mass has the prior device offset and a barometer offset fitted to its own level. If every
  cell's mass dies, the filter starts again from the uniform prior.

## The estimate

- **Level.** The mass per level goes through the same `LevelSelector` as in the particle filter.
- **Position.** Mean and covariance of the centroids on the reported level, plus the spread within a cell
  (h²/12). When that mean is off the map, the same `MapConsistentEstimate` picks the dominant group. It
  only sees cells holding at least 0.1% of the heaviest cell's mass; otherwise the recovery mass in every
  cell would make any mean look supported.
- **Validity.** The estimate is invalid until radio has been applied.
- **What is drawn.** `GridEngine` passes the estimate through the engine's `TrackSmoother`, and computes
  room probabilities for `RoomTracker` from the mass of each room's cells.

## Cost

Per step and per magnetometer update: O(cells × heading bins × scales). Per radio reading: O(cells).
Memory is about 8 bytes per (cell, heading bin, scale) plus about 120 bytes per cell. A 960 m² office has
3,840 cells; the KIT building, 60,351 cells on 7 floors; a mall floor, up to 200,000. Experiment 14 has the
timings of the first version.

Two later speed-ups leave every result unchanged to the byte. That was checked on the per-sample
estimates, tracks, room events and cell snapshots of the 12 scenarios, three mall walks and an IPIN walk,
and on runs with an active floor and step-scale states.
- **Wall counts**, by filling each wall's shadow (see [the map](#the-map-gridmap)), per beacon:
  - site1_F2: 467 → 4.8 ms;
  - site2_F6: 957 → 10 ms;
  - KIT: 46 → 4 ms;
  - IPIN Nuremberg: 39 → 1.4 ms.
- **The step loop** works on local copies of the members' pointers and of the cell's lattice position and
  mask, and keeps the running sums of the current target in registers (byte stores into the arrays forced
  the compiler to reload them on every move). A target cell's seven accumulators sit side by side, and the
  lattice offsets of all moves come from a separate loop that the compiler vectorises. A step on site1_F2
  (136,491 cells) went from 178 to 116 ms. Compute per update fell 1.25–1.6× on the 12 scenarios.

Replay on one desktop core (Ryzen 7 5800H) with `msil_wifi.json`:

| Walk | Cells | APs heard | Before | After |
|---|---:|---:|---:|---:|
| site1_F2, 160 s | 136,491 | 148 | 145 s | 65 s |
| site2_F1, 217 s | 159,278 | 526 | 366 s | 121 s |
| site2_F6, 69 s | 155,276 | 211 | 213 s | 35 s |

The web demo runs both filters in one Web Worker. Timed in Node, in WebAssembly, the demo recordings take
this share of their duration:
- site1 floors: 0.47–0.63 (site1_F2 was 1.33);
- site2 floors: 0.86–1.27 (site2_F6 was 4.5);
- IPIN: 0.09–0.2.

The particle filter is about a quarter of that (0.26 on site2_F6). On a mall floor, what remains is about
half the step loop. Most of the rest is the radio update: `log10`, `exp` and `log` for every (cell,
reading). It is a third of the time in WebAssembly, where those functions are slower than natively.
