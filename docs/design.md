# Design

How the engine works today. Why it is this way, and what was tried on the way, is in
[history.md](history.md); how to add to it is in [extending.md](extending.md).

## Pipeline

```mermaid
flowchart LR
  IMU[IMU 50 Hz] --> PDR[PDR: step detector, step length, gyro yaw]
  PDR -->|"step (length, turn)"| F[Grid filter]
  PDR -->|magnetometer heading per step| F
  Radio[BLE packets, Wi-Fi scans] -->|"beacon, RSSI"| F
  Baro[Barometer] -->|pressure| F
  F --> S[Track smoother] --> Est[Estimate: x, y, level, covariance]
  F --> Rooms[Room enter / exit events]
```

`PositioningEngine` (`core/include/ipe/engine.hpp`) is the only entry point a platform layer needs: push
IMU samples, radio readings and barometer readings in time order, read the estimate. `TracePlayer` feeds a
recorded or simulated trace into it in timestamp order; the replay tool, the regression suite and the
WebAssembly demo all use it, so they see identical inputs.

### Where things live

| Concern | Code |
|---|---|
| Entry point, settings | `core/src/engine.cpp`, `core/src/config.cpp` |
| Steps from IMU samples | `core/src/pdr.cpp` |
| The venue as a lattice of cells: allowed moves, wall counts per beacon, rooms per cell | `core/src/grid_map.cpp` |
| Grid filter: state, start, applying a log-likelihood | `core/src/filter/grid_filter.cpp` |
| Steps and magnetometer heading | `core/src/filter/motion.cpp` |
| A radio update: integrity gate, log-distance model | `core/src/filter/radio.cpp`, `core/src/rssi_model.cpp`, `core/src/beacon_integrity.cpp` |
| Fingerprints and the floor vote | `core/src/filter/fingerprint.cpp`, `core/src/fingerprint_map.cpp`, `core/src/floor_classifier.cpp` |
| Barometer, floor changes | `core/src/filter/pressure.cpp`, `core/src/baro.cpp` |
| A position given from outside (a tap, a tag) | `core/src/filter/position_fix.cpp` |
| Recovery | `core/src/filter/recovery.cpp` |
| Which cells an update visits (`active_floor`) | `core/src/filter/active_set.cpp` |
| Reported estimate: level, position on the map | `core/src/filter/estimate.cpp`, `core/src/level_selector.cpp`, `core/src/map_estimate.cpp` |
| Drawn position between updates | `core/src/track_smoother.cpp` |
| Room events | `core/src/rooms.cpp` |
| Rooms nobody can walk into | `core/src/reachable_area.cpp` |
| Venue and trace files, trace playback | `core/src/venue.cpp`, `core/src/trace.cpp`, `core/src/trace_player.cpp` |
| Replay, metrics, regression comparison | `eval/src/eval.cpp`, `eval/src/regression.cpp` |

## Pedestrian dead reckoning

- **Step detection**: low-pass filtered `|a| - g`, a peak above 1 m/s^2 confirmed by a fall below 0, with
  a 0.25 s refractory period.
- **Step length**: Weinberg, `L = K (a_max - a_min)^(1/4)` with `K = 0.48`. `K` is person specific, so the
  filter does not trust it: its along-track noise grows with the step length.
- **Heading**: gyro z integrated between steps gives the turn; the magnetometer heading averaged over the
  step is a separate, outlier-prone absolute observation. Keeping them apart lets the filter decide how
  much to trust the magnetometer. A sample whose horizontal field is exactly zero has no magnetometer
  reading; steps then carry no absolute heading.

A motion model of your own can replace this stage: `PositioningEngine::on_step` takes its steps
([extending.md](extending.md#a-motion-model)).

## The grid filter

The engine holds the posterior as probability mass on a lattice of the venue's walkable cells (0.5 m by
default), with a distribution over heading in every cell. There are no particles and no random numbers:
the same inputs give the same estimate, on every platform. Its parameters are `GridFilterConfig`
(`core/include/ipe/grid_filter.hpp`); the few a deployment changes are the `filter` section of the
[settings](formats.md#settings), and the names in backticks below are the parameters' names.

![the posterior converging on a simulated office walk](img/convergence.gif)

**Why a grid.** The floor plan is a hard, non-Gaussian constraint. A grid expresses it exactly: a cell that
cannot be stood on does not exist, and a move that crosses a wall is not in the cell's move mask. The
uniform prior is exact from the first update, so nothing has to seed, re-seed or inject hypotheses, and
room probability is simply the mass of a room's cells.

### The map (`GridMap`)

Built once per venue and shared read-only by every engine on it.

- **Cells.** One lattice over the venue bounds, 0.5 m cells (`cell_m`), coarser when the walkable area
  would need more than `max_cells` (200,000). A cell exists on a level when its centre is inside the venue's
  bounds, its `footprint` polygon if it has one, and the level's own footprint, and outside closed regions.
- **Closed regions** (`closed_region_fraction` 0.1, `closed_region_min_m2` 10). The walls split each level
  into connected regions. A region of at least 10 m^2, smaller than a tenth of its level's largest and
  without a stairwell or lift is closed: a room without a door, an island drawn as an outline. Its cells do
  not exist, so no Wi-Fi scan can pull the estimate in.
- **Moves.** Each cell has one bit per lattice offset within `max_reach_m` (2 m): set when the segment
  between the two centres crosses no wall and the target cell exists. A step's wall test is one bit.
- **Radio.** The walls between each cell's centre and each beacon are counted the first time the beacon is
  heard, by filling each wall's shadow as seen from the beacon lattice row by lattice row (1–10 ms per
  beacon). The result is exactly `Venue::count_wall_crossings`.
- **Floors and rooms.** Cells inside a connector (stairwell, lift, escalator) link to the cell at the same
  place on the levels next to it. Each cell lists the rooms its centre is in.
- **Blocks.** The lattice is also cut into squares of `background_block_m` (4 m, 8 by 8 cells), per level,
  each with its neighbours: the blocks a step or a level change from its cells can end in. They are what
  the active set below is made of.

### The state

Per cell, so that a radio update costs O(cells):
- the cell's probability (mass);
- the centroid of that mass. Moves start from the centroid and land at exact points, so mass neither
  drifts towards cell centres nor smears between cells;
- the device RSSI offset and the barometer offset, each as a Gaussian.

Per (cell, heading bin), as `float`: the distribution of the heading within the cell, over 36 bins
(`heading_bins`). The bins are offsets from the gyro-integrated yaw, so a step's gyro turn shifts every
bin exactly; only the 3° per step process noise moves mass between bins.

### Updates

- **Step** (`motion.cpp`).
  1. The gyro turn and the heading noise.
  2. Every (cell, heading) moves along track to three lengths `L ± √3 σ`, weighted 1/6, 2/3, 1/6, with
     σ = √(0.08² + (0.2 L)²). They match the mean and variance of the step-length noise; the relative part
     also stands in for a per-user step-length scale.
  3. Mass whose move the mask does not allow dies at the wall.
  4. The rest lands in the target cells with its centroid and the moments of both offset Gaussians.
  5. In connector cells 8% of the mass moves to the levels next to it.
- **Magnetometer.** A wrapped Gaussian with a 15% uniform part, once per heading bin: indoor magnetic
  anomalies are common rather than exceptional.
- **Radio** (`radio.cpp`, `fingerprint.cpp`). A reading or a whole Wi-Fi scan is one update: a sum of
  log-likelihood terms per cell, applied together.
  - *Log-distance model.* The prediction is the path loss at the cell's centroid (3D distance and
    `floor_attenuation_db` per slab across levels), minus `wall_attenuation_db` per wall on the straight
    path, plus the cell's device offset. The likelihood is a Gaussian/uniform mixture, so one bad packet
    cannot wipe out the posterior, and the offset Gaussian is updated with the reading.
  - *Beacon integrity.* Once converged, the monitor averages each beacon's residual at the estimate and
    stops using beacons whose bias exceeds 10 dB (re-admitted below 5 dB, never more than a third of the
    beacons). It catches beacons that were moved or died.
  - *Fingerprints* (`fingerprint_weight`, 0 by default; 3 on a venue whose training scans carry
    positions). `FingerprintMap` returns a scan's 10 nearest training scans, compared
    over its 40 strongest readings (missing ones at -100 dBm), with weights 1 / (RMS difference + 1 dB).
    They form a kernel density over position and level: 4 m Gaussians around the neighbours plus 5% of the
    uniform prior. Every cell gets `fingerprint_weight` times its logarithm, and the log-distance term of
    that scan is scaled by `fingerprint_model_weight` (0.3), since both read the same scan. Beacons with
    `"model": false` are fingerprint dimensions only.
  - *Floor vote* (`floor_prior_weight`, 0 by default; 3 on a venue with training scans). A k-NN vote
    of the scan over the training scans gives p(level), applied as `weight · log p(level)` on every
    cell. The radio model's 2–4 dB per slab barely separates floors on real Wi-Fi.
- **Barometer** (`pressure.cpp`). Every 0.5 s: the altitude against the cell's level and offset, scored
  with a Gaussian plus a 5% uniform part over 20 m, then an update of the cell's offset. The first sample
  sets each cell's offset from its own level, so the barometer argues about changes, not absolute height.
  Lift rides produce no steps, so each barometer update also moves 10% of the mass in connector cells.
- **Position fix** (`position_fix.cpp`). A position known from outside the engine: a tap on the map, a QR
  code or NFC tag at a known place, a GNSS fix at an entrance. 1% of the uniform prior is mixed in first,
  so that it can move an estimate that is confidently somewhere else; then a Gaussian of the fix's sigma
  around it, on its level. Mass that is already near the fix outweighs the mixed-in prior and keeps its
  heading and offsets. Like a radio update it makes the estimate valid, so a venue without any radio can
  be walked from a fix on steps and walls alone.
- **Recovery** (`recovery.cpp`). A tiny share of uniform mass is mixed in (`teleport_rate_per_s`, 1e-8 per
  second): a way back from a wrong lock that a misbehaving beacon cannot exploit. If all mass dies, the
  filter starts again from the uniform prior.

### The active set

`active_floor`, 1e-9 by default. After the first seconds nearly all of the mass sits in a few hundred cells,
and the rest of the venue holds recovery mass: about 1e-8 of the probability, spread over every other
cell. With an active floor the filter does its full work only where the mass is.

- **Active blocks.** A block is active when one of its cells holds at least `active_floor` of the heaviest
  cell's mass, or when it is next to such a block. Its cells are updated exactly as above. The ring of
  neighbours means that mass above the floor never steps, or changes level, into a block that is not
  active.
- **Background blocks.** Every other block is one coarse cell: one mass with its centroid, one heading
  row, one device offset and one barometer offset. Each update treats it as it treats a cell:
  - a radio update scores it once, at the centroid, through the walls of the cell nearest to the block's
    centre, and a barometer update scores it against its level;
  - the magnetometer reweights its heading row and its mass;
  - a step moves its mass as a whole, by the mean move of its heading row. Within the block the centroid
    moves. Across the edge the mass joins the next block, or the cell it lands in when that block is
    active. A move through a wall or off the map ends it, as it ends a cell's;
  - recovery mass is added to it as to cells.

  It shares the normalisation with the cells, so a region the radio starts to favour grows as it would
  cell by cell.
- **Changing sides.** A background block whose mass per cell reaches 16 times the floor becomes active:
  its cells get that mass evenly, at their centres, with the block's heading row and offsets. An active
  block with nothing above the floor in it or next to it is folded: its mass is summed, and its centroid,
  heading row and offsets are those of its cells' mass. The gap between the two thresholds keeps a block
  from changing sides at every update.
- **Why a block has a heading.** A cell learns the heading at its first magnetometer update and agrees
  with every later one. Mass without a heading is cut again at every step: in a unit test of 20 steps with
  a compass, such a background held a sixth of the mass the same cells hold in the exact filter
  (experiment 21).
- **What is approximate.** Mass under the floor is one point per block: it loses its shape within the
  block, and what differs between its cells in heading and offsets. Moves from active cells into a
  background block are dropped, as if a wall stood there, and background mass does not change level. A
  position fix is scored on every cell (every block is made active first), because a fix can be narrower
  than a block.
- Cells in a background block read as mass 0 (`GridFilter::mass()`); `active_cells()` lists the others and
  `background_mass()` is what the blocks hold. The filter stays deterministic: the active cells are kept
  in index order, the order every sum is taken in with the floor off.

With `active_floor` 0 every block is active at every update, and the results are those of a filter
without blocks, bit for bit.

### The estimate

- **Level.** The mass per level is averaged over 0.5 s (`level_smoothing_s`), and the reported level moves
  to another one only when that one leads by 0.1 (`level_hysteresis`), or at once when no mass is left on
  the reported level. A posterior split between floors then does not flicker with every packet.
- **Position.** Mean and covariance of the centroids on the reported level, plus the spread within a cell.
- **On the map** (`map_consistent_estimate`, on). The mean of a posterior split across a wall can fall on
  the wall or in an empty room. If the mean is outside the walkable area or a wall separates it from its
  nearest cell, the reported position is the mean of the dominant group instead: the heaviest 3x3 block of
  a 2 m weight histogram, keeping the block's cells on the same side of the walls.
- **Validity.** The estimate is invalid until radio has been applied.

### What is drawn

`PositioningEngine::estimate()` passes the filter's estimate through `TrackSmoother` (the `smoothing`
section); `raw_estimate()` is the filter's own. The smoother separates what the walker did from what the
filter learned:

- **Steps.** A step moves the dot at once by the step's own motion: the mean move of the mass that
  survived it. Whatever else the step did to the estimate counts as a correction.
- **Between steps.** The dot keeps moving at the last step's pace, centred on the filter's estimate. If no
  step comes for 1.2 s, the walker has stopped.
- **Corrections.** Every other change of the estimate is glided towards: after `dt`, the gap left is
  `exp(-dt / tau_s)` of what it was (`tau_s` 0.5 s). `max_correction_mps` caps the glide's speed; it is off
  by default and 1.5 m/s on a fitted radio map, where one scan can move the estimate several metres.
- **Jumps.** The dot jumps when the estimate is more than 10 m away (`snap_m`) and when the level changes.
  An estimate behind a wall is glided towards only as far as the wall; the dot never slides through one.

Room events use the posterior, not the drawn point. `smoothing.enabled = false` gives the filter's estimate
bit for bit.

### Cost

Per step and per magnetometer update: O(cells × heading bins). Per radio reading: O(cells). Memory is about
8 bytes per (cell, heading bin) plus about 120 bytes per cell. A 960 m² office has 3,840 cells; the KIT
building 60,351 on 7 floors; a mall floor up to 200,000.

That is the exact filter (`active_floor` 0). Replayed on one desktop core (Ryzen 7 5800H) it takes 0.1–2 ms
per sensor update on the simulated scenarios and a third to a half of a mall walk's duration, more on the
largest floors with many access points; in WebAssembly 0.5–1.2 of a mall recording's duration. About half
of that is the step loop; most of the rest is `log10`, `exp` and `log` for every (cell, reading).

With the [active set](#the-active-set), the default, the cost follows the mass, not the venue: O(active
cells) per update plus O(blocks x heading bins) for the background. On the same core the simulated
scenarios take 0.03–0.08 ms per sensor update ([results.md](results.md); 3 to 23 times less, `kit_tour`
1.90 → 0.08 ms) and a 160 s mall walk on 136,491 cells replays in 2.8 s instead of 48 s. The first
seconds, until the posterior has concentrated, cost what the exact filter costs. In WebAssembly it has not
been timed. The accuracy is in [history.md](history.md) (experiments 20 to 22): unchanged on the simulated
walks, and on real Wi-Fi the mean, CEP50 and CEP95 of the exact filter to within 0.3 m, with the track
itself more than 3 m away from the exact filter's 1.0% (malls) to 3.4% (IPIN) of the time, where a setting
changed in its third digit gives 0.3% and 2%.

## Floors

A venue is a stack of levels, each with its own walls, plus connectors whose footprint polygons are the
only places where anyone changes level.

- **Barometer.** Air pressure drops about 0.12 hPa per metre, so a 4 m storey is 15–50 times a phone
  barometer's noise. The absolute reading is useless (weather moves it by several hPa a day, and phones
  disagree by a few hPa), so `BaroAltimeter` low-passes the altitude and reports it relative to the first
  sample.
- **RSSI.** A beacon on another floor is heard through the slab; strong beacons right below or above can
  still look closer than same-floor beacons farther away.
- **Steps.** The engine does not detect stairs from the IMU; it only lets mass change level where the map
  has stairs.

`floor_accuracy` is the share of fixes on the correct floor. CEP50/CEP95 only count fixes on the correct
floor. `floor_switch_latency_s` is the time from a true level change until the estimate reports the new
floor, and `level_flips` counts how often the reported floor changes
([formats.md](formats.md#replay-outputs)).

## Room enter and exit events

A venue can name rooms (polygons per floor, [formats.md](formats.md#rooms)); `PositioningEngine` then
reports `Enter`/`Exit` events through `take_room_events()`.

- **Room probability** is the mass in the room's cells, so a posterior split across a doorway gives both
  rooms a share. Without a valid fix every room is at 0.
- **Debounce.** Each room has its own two-state machine. Outside, the probability must stay at or above
  `enter_prob` (0.6) for `enter_dwell_s` (1.5 s) before `Enter`; inside, at or below `exit_prob` (0.3) for
  `exit_dwell_s` before `Exit`. The gap keeps a posterior hovering in a doorway from toggling.
- **Ground truth.** Replay runs the same tracker on the true position with a 0.5 threshold and no dwell,
  and scores the engine's events against it. The dwell is a deliberate delay, so latency is at least 1.5 s.

One seed each, default config:

| Scenario | True events | Precision | Recall | Median latency | Room accuracy |
|---|---|---|---|---|---|
| office_nominal | 16 | 1.00 | 1.00 | 1.7 s | 0.88 |
| office_stairs_up_down | 24 | 1.00 | 1.00 | 1.8 s | 0.85 |

## Determinism

The filter draws no random numbers. The simulator does, and all of its sampling uses `ipe::Rng`
(xoshiro256** + Box-Muller): `std::normal_distribution` is implementation-defined, so the same seed would
produce different traces under libstdc++, libc++ (used by Emscripten) and MSVC. A golden-value test pins
the generator.

## Known limitations

- **Simulation is optimistic.** The simulator and the filter share a family of propagation models. The
  scenarios inject mismatches (wall loss, exponent, device offset, moved beacons), but real errors are
  about ten times the simulated office's ([datasets.md](datasets.md)).
- **Phone handling.** The phone is assumed flat and pointing in the walking direction. Pocket and swing
  carry modes need attitude estimation.
- **The uncertainty is too small.** The model counts time-correlated packets and the readings of one scan
  as independent. The 95% ellipse holds the truth for 74% of the fixes on the simulated scenarios and for
  13–21% on real Wi-Fi.
- **Floor changes follow the position.** Mass changes floor only inside a connector. Dead reckoning can
  carry it out of a small stairwell before the barometer has seen half the climb; the floor then follows
  through recovery mass or the floor vote, seconds late. Without a barometer, only RSSI reveals a lift
  ride, slowly.
- **Floors are flat slabs.** Atriums and ramps between half-levels are not modelled.
- **Step length** has no per-user scale state; a wider along-track noise stands in for it.
- **The active set is an approximation.** By default the filter does its full work only where the mass is
  (`active_floor` 1e-9). On real Wi-Fi the track differs from the exact filter's by more than a trivially
  changed setting makes it differ (experiment 21), and it has not been tried on a phone. `active_floor` 0
  is the exact filter, at 3 to 23 times the compute.
- **Room events are only as good as the position.** In the malls (5–7 m typical error) rooms smaller than
  the error are not detectable; the malls' room events are a demo of the plumbing, not a result.
- **Offline use.** Competitions scored offline reward whole-walk optimisation. The two-pass replay in
  `datasets/common/offline.py` is two causal filters combined; an exact forward-backward smoother on the
  grid is untried.
