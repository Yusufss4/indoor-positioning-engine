# The grid filter

The engine finds the position with a Bayes filter on a grid of map cells that holds the whole posterior
(`core/src/grid_filter.cpp`, `core/src/grid_map.cpp`). It has no random particles and no seed.
[design.md](design.md#grid-filter) has the overview; this page has the detail.

Until experiment 16 the engine was a particle filter, and this filter a prototype next to it. The
comparison that made it the engine is in [experiments.md](experiments.md) (14 and 16). In short, against
the particle filter:
- **Simulated scenarios** (10 seeds): better mean CEP50 and CEP95, half the jitter and fewer jumps; the
  two-floor walks converge in 1–2 s.
- **Real walks:** better on the IPIN buildings, equal on the mall floors, about equal on UJI.
- **Costs:** a covariance that is too tight, and compute: 2–20× the particle filter's on the simulated
  venues and up to about half of real time on a mall floor ([Cost](#cost)).

```bash
./build/tools/ipe-replay --scenario data/scenarios/office_nominal.json     # one trace, metrics as JSON
./build/tools/ipe-replay --print-config                                    # every setting, "filter" among them
./build/tools/ipe-regress --scenarios data/scenarios --no-baseline --seeds 10 \
    --set '{"filter":{"heading_bins":72}}'                                 # an A/B run over all scenarios
```

The filter's settings are `GridFilterConfig` (`core/include/ipe/grid_filter.hpp`), the `filter` section of
the engine config; `--config` and `--set` patch them like every other section.

## Why a grid

Particles sample the posterior. Most of the machinery of the particle filter this engine used to have
existed because a sample covers it sparsely:
- augmented-MCL recovery, and the trade-off it brought when a beacon misbehaved (experiment 2);
- the beacon-ring start, and the density rule that switched it on (experiments 4 and 9);
- rejection sampling around closed rooms (experiment 13);
- the re-seed when every particle died;
- seed noise in every comparison.

A grid holds the probability of every cell, so the uniform prior is represented exactly from the first
update. A room without a door is just a cell that does not exist. Room probability is the mass of the
room's cells. The filter is deterministic: no `ipe::Rng`, no seed. Its sensor models are the ones the
particle filter had, which is what made the two comparable.

## The map (`GridMap`)

Built once per venue and shared read-only by every engine on it (`ipe-regress` keeps one per venue;
`PositioningEngine` builds its own unless it is handed one).
- **Cells.** One lattice over the venue bounds, 0.5 m cells (`cell_m`). Cells are coarser when the
  walkable area would need more than `max_cells` (200,000) of them. A cell exists on a level when its
  centre is inside the venue and the level's footprint, and outside closed regions (`ReachableArea`,
  `filter.closed_region_*`).
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
- the barometer offset, as a Gaussian. Carrying these as Gaussians is exact Rao-Blackwellisation.

Kept per (cell, heading bin), as `float`:
- the distribution of the heading within the cell, over 36 bins (`heading_bins`);
- optionally a persistent step scale with `step_scale_bins` values.

The heading bins are offsets from the gyro-integrated yaw (heading = yaw + 10° × k). A step's gyro turn
therefore shifts every bin exactly, with no interpolation. Only the 3° per step process noise moves mass
between bins, as a 3-tap kernel with that variance.

## Updates

- **Step.**
  1. Apply the gyro turn and the heading kernel.
  2. Every (cell, heading, scale) moves along track to three Gauss-Hermite lengths `L ± √3 σ`, weighted
     1/6, 2/3, 1/6, with σ = √(0.08² + (0.2 L)²). These match the mean and variance of the length noise.
     The relative part (`step_length_rel_sigma`) also stands in for a per-user step-length scale; a
     scale state (`step_scale_bins` 3) was worse overall.
  3. Mass whose move is not allowed by the mask dies at the wall.
  4. The rest is scattered into the target cells together with its centroid and the moments of both
     offset Gaussians (moment matching).
  5. In connector cells 8% of the mass moves to the levels next to it.
  `step_motion()` is the mean move of the mass that survived; the track smoother draws it at once.
- **Magnetometer.** A wrapped Gaussian with 15% uniform, evaluated once per heading bin.
- **RSSI.** For every cell:
  - the prediction is the path loss at the centroid (3D distance and slab loss across levels), minus the
    wall loss times the cell's wall count, plus the cell's mean device offset;
  - its variance is σ² plus the offset's variance, in the same Gaussian/uniform mixture;
  - the offset Gaussian is updated by an assumed-density Kalman step, weighted by the reading's inlier
    responsibility.

  A scan applies its readings one after another and renormalises once. The readings of one scan share
  the device offset, so they are not counted as fully independent of each other. Readings first pass the
  `BeaconIntegrityMonitor`, judged at the estimate on the most likely level.
- **Floor prior** (`filter.floor_prior_weight`, off by default; 3 in `ipin.json` and
  `fingerprint.json`). On venues with Wi-Fi floor fingerprints (the IPIN buildings), each scan also adds
  `floor_prior_weight · log p(level)` to every cell, with p from the k-NN `FloorClassifier`
  ([ipin.md](ipin.md#floor-model)). Without it, nothing rejects mass that walks the same path one floor
  up: the radio model's 2–4 dB per slab barely separates floors, and every level's barometer offset was
  fitted to it at the first reading. On the Nuremberg demo walk the filter spent 84% of 10–60 s on Floor 0
  while the walker stayed on Floor -2 (0% with the prior). IPIN validation walks, `msil_wifi.json`, prior
  off → on (median CEP50 / CEP95, floor hit, competition p75):
  - Nuremberg, 4 walks: 5.90 / 19.80 m, 91%, 10.20 m → 5.73 / 17.50 m, 95%, 8.07 m;
  - UMinho, 23 walks: 8.75 / 27.22 m, 40%, 32.36 m → 8.03 / 23.64 m, 82%, 18.68 m.

  What the prior leaves is the stairs. Mass changes level only inside a connector, and dead reckoning can
  carry it out of a small stairwell before the barometer has seen half the climb: the mass moved up too
  early is rejected, the rest is stranded on the old floor. The filter then reaches the new floor through
  recovery mass, whose barometer offset is fitted to its own level and so matches the reading on any
  floor. On the Nuremberg demo walk that takes until 248 s for a climb at 240–256 s. Giving recovery mass
  the distribution's barometer reference instead closes that route: on the validation walks Nuremberg's
  floor hit fell from 95% to 77%. A barometer-driven move to the nearest stairs, which the particle filter
  had, would be the fix; the position fingerprints below make it matter less, because their neighbours
  carry a level too.
- **Fingerprints** (`filter.fingerprint_weight`, off by default; 3 in `fingerprint.json`). On venues
  whose training scans carry positions, each scan's nearest training scans (`FingerprintMap`) form a
  kernel density over position and level, and every cell gets `fingerprint_weight · log density`; the
  log-distance term of that scan is scaled by `fingerprint_model_weight`
  ([design.md](design.md#grid-filter), [benchmarks.md](benchmarks.md)).
- **Barometer.** Every 0.5 s, a Gaussian/uniform mixture likelihood and an assumed-density update of
  the offset, with a 0.05 m random walk. The first sample sets each cell's offset from its own level, and
  lift rides move 10% of each connector cell's mass to the next level.
- **Recovery.** The filter mixes in uniform mass, at most every 0.5 s
  (`teleport_period_s`):
  - at a constant rate, `teleport_rate_per_s`. The default, 1e-8 per second, is tiny on purpose: mass
    that is everywhere at once gives a moved beacon or one Wi-Fi scan a foothold wherever the map allows
    one;
  - optionally also augmented MCL's trigger (`recovery_max_fraction` > 0): 1 − w_fast / w_slow of the mass
    when the radio's predictive likelihood drops. It fires exactly when a beacon misbehaves and is off.

  The new mass has the prior device offset and a barometer offset fitted to its own level. If every
  cell's mass dies, the filter starts again from the uniform prior.

## The estimate

- **Level.** The mass per level goes through `LevelSelector` (an average over 0.5 s with hysteresis).
- **Position.** Mean and covariance of the centroids on the reported level, plus the spread within a cell
  (h²/12). When that mean is off the map, `MapConsistentEstimate` picks the dominant group. It
  only sees cells holding at least 0.1% of the heaviest cell's mass; otherwise the recovery mass in every
  cell would make any mean look supported.
- **Validity.** The estimate is invalid until radio has been applied.
- **What is drawn.** `PositioningEngine` passes the estimate through `TrackSmoother`, and computes room
  probabilities for `RoomTracker` from the mass of each room's cells.

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

The web demo runs the grid filter alone in a Web Worker. Timed in Node through the demo's worker, in
WebAssembly, the demo recordings take this share of their duration:
- site1 floors: 0.46–0.63 (site1_F2 was 1.33 before the speed-ups, when the demo also ran the particle
  filter);
- site2 floors: 0.62–0.79, and 1.18 on site2_F8 (site2_F6 was 4.5);
- IPIN: 0.08–0.19.

On a mall floor about half of that is the step loop. Most of the rest is the radio update: `log10`, `exp`
and `log` for every (cell, reading). It is a third of the time in WebAssembly, where those functions are
slower than natively.
