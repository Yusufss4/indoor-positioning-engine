# Design notes

## Measurement pipeline

```mermaid
flowchart LR
  IMU[IMU 50 Hz] --> PDR[PDR: step detector, Weinberg length, gyro yaw]
  PDR -->|"step (L, dpsi)"| F[Particle filter]
  PDR -->|mag heading per step| F
  BLE[BLE packets] -->|"beacon, RSSI"| F
  Baro[Barometer 5 Hz] -->|"pressure (hPa)"| F
  F --> Est[Estimate: x, y, level, covariance]
```

`PositioningEngine` is the only entry point a platform layer needs: push IMU samples, BLE packets and
barometer readings in time order, read the estimate. `TracePlayer` feeds a recorded or simulated
trace into it in timestamp order; the replay tool, the regression suite and the WebAssembly demo all
use it, so they see identical inputs.

### Where things live

| Concern | Code |
|---|---|
| Engine entry point, room events | `core/src/engine.cpp`, `core/src/rooms.cpp` |
| Trace streams in time order | `core/src/trace_player.cpp` |
| Particle filter: sampling, motion, measurement updates, resampling, recovery, estimate | `core/src/particle_filter.cpp` |
| Beacon-driven init/recovery proposals | `core/src/particle_filter_proposal.cpp` |
| Beacon integrity monitor | `core/src/beacon_integrity.cpp` |
| Estimate kept on the map (dominant group of a split cloud) | `core/src/map_estimate.cpp` |
| Reported level (averaged level weights with hysteresis) | `core/src/level_selector.cpp` |
| Drawn position: filter estimate smoothed between updates | `core/src/track_smoother.cpp` |
| Config JSON, presets, replay, metrics | `eval/src/eval.cpp` |

**Adding a sensor.** Add a stream to `Trace` (and its CSV reader/writer in `core/src/trace.cpp`), a
case to `TracePlayer::step`, an `on_x` to `PositioningEngine`, and an `on_x` to `ParticleFilter` that
fills `loglik_` with one log-likelihood per particle and calls `commit_log_likelihoods(t)`. The
simulator (`sim/`) generates the stream for scenarios; new config fields go into the JSON registration
in `eval/src/eval.cpp`.

## Pedestrian dead reckoning

- **Step detection**: low-pass filtered `|a| - g`, a peak above 1 m/s^2 confirmed by a fall below 0,
  with a 0.25 s refractory period.
- **Step length**: Weinberg, `L = K (a_max - a_min)^(1/4)` with `K = 0.48`. `K` is person specific, so the
  particle filter carries a per-user scale state rather than trusting it (see `office_step_scale`).
- **Heading**: gyro z integrated between steps gives `dpsi`; the magnetometer heading averaged over the
  step is a separate, outlier-prone absolute observation. Keeping them separate lets the filter decide
  how much to trust the magnetometer.

## Particle filter

State per particle: position, heading, step-length scale, device RSSI offset.

- **Motion**: `psi += dpsi + noise`, move `L * scale + noise` along `psi`. A particle whose move crosses a
  wall or leaves the venue gets weight zero. This is the main reason to use a particle filter here: the
  floor plan is a hard, non-Gaussian constraint that a Gaussian filter cannot express.
- **Venue area**: "the venue" is its bounds rectangle, intersected with the `footprint` polygon when the
  venue has one. Uniform draws (start, recovery, ring fallback) are rejection-sampled inside the footprint.
  The footprint matters for buildings that fill a small part of their bounds: in an L-shaped building,
  particles started in the empty corner sit outside the outline walls, and the walls keep them there.
- **RSSI likelihood**: log-distance path loss, minus `wall_attenuation_db` per wall on the straight radio
  path, plus the particle's device offset. The likelihood is a Gaussian/uniform mixture so one bad packet
  cannot wipe out the cloud.
- **Heading likelihood**: wrapped Gaussian with a 15% uniform component, since indoor magnetic anomalies
  are common rather than exceptional.
- **Resampling**: systematic, when ESS < N/2, followed by small position roughening that respects walls.
- **Recovery**: augmented MCL (`w_slow`/`w_fast`) injects up to 5% uniform particles when the short-term
  likelihood falls below the long-term one. If every particle dies, the cloud is re-seeded around the last
  estimate.
- **Beacon-driven proposal** (`init_proposal` / `recovery_proposal`): after a 1 s collection
  window, the packet with the shortest apparent range re-seeds the cloud. 80% of the particles come from a
  ring around that beacon, with log-range normal around the inverted path-loss model for each particle's
  device offset; 20% stay uniform. Weights are prior/proposal, computed analytically. Draws that leave
  the venue are rejected jointly with their offset (up to 4 tries), so one acceptance rate normalises
  the truncated ring. Recovery can draw from the ring of a recent packet instead of uniformly. The init
  proposal slows convergence in small walled venues (experiment 4), so it turns itself on only when the
  cloud is sparse: under `init_proposal_auto_density` (0.6) particles per m^2 of walkable area, all
  levels together (experiment 9). `init_proposal: true` turns it on everywhere; the recovery proposal is
  opt-in.
- **Beacon integrity monitor**: once converged, keeps an exponential average of each beacon's RSSI
  residual at the estimate and stops using beacons whose bias exceeds 10 dB (re-admitted below 5 dB, never
  more than a third of the beacons).
- **Scans** (`on_rssi_scan`): a Wi-Fi scan reports dozens of access points at once. The filter applies
  the whole scan as one update: one device-offset random-walk step, one recovery-statistics update, one
  resample decision and one estimate. The integrity monitor still checks each reading. Readings count
  as independent; tempering the scan's likelihood made the spread honest but cost accuracy at every
  setting tried, and was removed (see [real_data.md](real_data.md#scans-as-one-update)). A one-reading
  scan is bit-identical to `on_rssi`.
- **Wall queries** use a uniform grid over wall segments, with a linear scan for long radio paths where
  the grid would visit most cells anyway. A property test checks the grid against brute force.

### What is drawn

`PositioningEngine::estimate()` passes the filter's estimate through `TrackSmoother`
(`EngineConfig::smoothing`); `raw_estimate()` is the filter's own. The filter recomputes its estimate after
every packet, step and heading update, so drawn as-is it shakes (jitter p95 about 1 m on `office_nominal`)
and on real Wi-Fi jumps several times a minute. The smoother:

- moves the dot by the filter's own displacement for each step, immediately, so it does not lag the walker;
- pulls it towards the filter's estimate after every other update with a 0.5 s time constant
  (`smoothing.tau_s`), so radio noise is not drawn as motion;
- jumps to the filter's estimate when that is more than 10 m away (`smoothing.snap_m`), when the reported
  level changes, and whenever gliding would cross a wall or leave the walkable area, so it never slides
  through a wall;
- reports the filter's covariance taken about the drawn point, so `coverage95` stays honest.

Room events use the particles, not the drawn point, so they are unaffected. `smoothing.enabled = false`
gives the filter's estimate bit for bit. `smoothing.extrapolate_steps` (off) keeps the dot moving at the
last step's pace between steps; it overshoots where the walker stops or turns (experiment 8).

## Floors

A venue is a stack of levels, each with its own walls, plus connectors (stairwells, lifts, escalators)
whose footprint polygons are the only places where anyone changes level.

### What tells the engine which floor it is on

- **Barometer.** Air pressure drops about 0.12 hPa per metre near sea level, so one 4 m storey is about
  0.48 hPa. A phone barometer has roughly 0.01-0.03 hPa of noise, so a floor change is 15-50 times the
  noise. The absolute reading is useless: weather moves it by several hPa a day, and phones disagree by a
  few hPa. Only changes over minutes are trusted. `BaroAltimeter` converts pressure to altitude with the
  barometric formula `h = 44330 (1 - (p / p0)^(1/5.255))`, low-passes it (`tau = 1 s`, which kills door
  slams and ventilation gusts) and reports it relative to the first sample.
- **RSSI.** A beacon on another floor is heard through the slab: the RSSI model uses the 3D distance
  `hypot(horizontal, dz)` and subtracts `floor_attenuation_db` per slab. Strong beacons right below or
  above the user can still look closer than same-floor beacons farther away (`office_floor_bleed`).
- **Steps.** Climbing stairs produces steps with little horizontal progress. The engine does not detect
  stairs from the IMU; it only lets particles change level where the map has stairs.

### Particle filter

Each particle carries a `level` and a `baro_offset`: the unknown altitude, in barometer metres, of the
floor of level 0 at the start of the walk. It absorbs weather, device bias and the unknown starting floor.

- **Motion.** After a step, a particle inside a connector that serves another level switches to an
  adjacent level with probability 0.08. Lifts produce no steps, so every barometer update (every 0.5 s)
  also offers the switch, with probability 0.1. Outside a connector the level never changes, so a particle
  cannot "teleport" through a slab. The offset follows a random walk (0.05 m per update) that tracks
  weather drift.
- **Barometer likelihood.** `z = (altitude - baro_offset - elevation(level)) / 0.8 m`, scored with a
  Gaussian plus a 5% uniform component over 20 m, so a pressure spike cannot wipe out the cloud. The first
  barometer sample sets each particle's offset from its own level (plus 0.3 m noise), so the barometer
  first learns which floor each hypothesis is on and only afterwards argues about changes.
- **RSSI likelihood** uses the particle's level for 3D distance, slab loss and the walls it counts.
- **Initialisation and recovery** sample levels as well as positions; recovery re-seeds around the last
  estimate on its level.
- **Estimate.** The level weights are averaged over 0.5 s (`pf.level_smoothing_s`), and the reported
  level moves to another one only when that one leads by 0.1 in the average (`pf.level_hysteresis`), or at
  once when no particle is left on the reported level. A cloud split between floors then does not flicker
  with every packet (`office_floor_bleed`: 20 reported floor changes per walk for 2 real ones, 3.8 with
  it), and a real floor change shows about 0.4 s later. Position and covariance are computed from the
  particles on the reported level only, so a minority hypothesis upstairs does not drag the mean into
  the wrong room. The filter's own decisions (integrity monitor, recovery, barometer jump and reference)
  use the most likely level of the current update, so the hysteresis changes what is reported, not how
  the filter evolves (experiment 7).
- **Estimate on the map** (`map_consistent_estimate`, on by default). Particles never cross walls, but the
  weighted mean of a cloud split across a wall (corridor and shop, two corridors) can fall on the wall,
  in an empty shop or behind a wall from every particle. If the mean is outside the walkable area or a
  wall separates it from its nearest particle, the reported position is the mean of the dominant group
  instead: the heaviest 3x3 block of a 2 m weight histogram, keeping only the block's particles on the
  same side of the walls as the one nearest the block's centre. The covariance is taken about the
  reported point, so it still covers the whole cloud. The integrity monitor and recovery keep using the
  cloud mean, so the option changes what is reported, not how the filter evolves (experiment 6).

Everything stays O(N) per measurement. On a single-floor venue none of this runs: no extra random draws,
no extra arithmetic, and the results are bit-identical to the single-floor engine.

### Measuring it

`floor_accuracy` is the share of fixes on the correct floor. CEP50/CEP95 only count fixes on the correct
floor, because a horizontal error measured against a point on another floor is meaningless.
`floor_switch_latency_s` is the time from a true level change until the estimate reports the new floor.
`level_flips` counts how often the reported floor changes, so a flickering floor shows up even when
`floor_accuracy` is high ([formats.md](formats.md#replay-outputs)).

## Room enter and exit events

A venue can name rooms (polygons per floor, see [formats.md](formats.md#rooms)); `PositioningEngine`
then owns a `RoomTracker` and reports `Enter`/`Exit` events through `take_room_events()`.

- **Room probability.** It is the normalised particle weight inside the room on the room's floor, so a
  cloud split across a doorway gives both rooms a share. Without a valid fix every room is at 0.
  Candidate rooms come from a 5 m grid per floor, so a particle costs one cell lookup and a
  point-in-polygon test for the few rooms in that cell.
- **Debounce.** Each room has its own two-state machine, so overlapping rooms (a shop inside a hall) are
  independent. Outside, the probability must stay at or above `enter_prob` (0.6) for `enter_dwell_s`
  (1.5 s) before `Enter`; inside, at or below `exit_prob` (0.3) for `exit_dwell_s` (1.5 s) before `Exit`.
  The gap between the thresholds keeps a cloud hovering at 50% in a doorway from toggling, and the dwell
  removes short excursions. Exits at an update come before enters, so walking through a door reads
  "exit A, enter B". A lost fix exits every room after the exit dwell.
- **Cost.** Probabilities are recomputed at most every `update_period_s` (0.25 s), after any sensor
  update, so the cost does not scale with the IMU rate. The per-particle loop does not allocate; only
  emitting an event does.
- **Ground truth.** Replay runs the same tracker on the true position with a 0.5 threshold and no
  dwell, and scores the engine's events against it (metrics in
  [formats.md](formats.md#replay-outputs)). The dwell is a deliberate delay, so latency is at least
  1.5 s and `room_accuracy` loses a few samples at every transition even for a perfect estimate.

One seed each, default config:

| Scenario | True events | Precision | Recall | Median latency | Room accuracy |
|---|---|---|---|---|---|
| office_nominal | 16 | 1.00 | 1.00 | 1.5 s | 0.89 |
| office_stairs_up_down | 24 | 1.00 | 1.00 | 2.2 s | 0.83 |
| kit_tour (open-venue config) | 34 | 0.96 | 0.79 | 1.9 s | 0.77 |

The walls keep the cloud on the right side of a door, which is what room events need. In the KIT building
most misses are short corridor segments and small rooms passed in under the dwell time.

## Determinism

All sampling uses `ipe::Rng` (xoshiro256** + Box-Muller). `std::normal_distribution` is
implementation-defined, so the same seed would produce different traces under libstdc++, libc++ (used by
Emscripten) and MSVC. A golden-value test pins the generator.

## Known model limitations

- The simulator and the filter share the same family of propagation models (log-distance plus walls), so
  results are optimistic compared to real venues. The scenarios inject mismatches (wall loss, exponent,
  device offset, moved beacons) to reduce this, but the next step is replaying traces recorded on phones.
- The phone is assumed flat and pointing in the walking direction. Real PDR needs attitude estimation and
  handling of pocket/swing carry modes.
- Floors without a barometer: lift rides produce no steps, so only RSSI can reveal a lift ride on phones
  without a pressure sensor, and it does so slowly. Stair climbs are not detected from the IMU either.
- Floors are flat slabs. A level can be smaller than the building (its own `footprint`, e.g. a stair
  landing), but atriums and ramps between half-levels are not modelled; radio through an atrium is wrongly
  given slab loss, and sloped outdoor ground reads as a floor change on the barometer
  ([ipin.md](ipin.md#failure-modes)).
- One loss per slab for the whole venue (`pf.calibrate_floor_loss` can learn it online, opt-in, see
  experiment 10) and one floor per transmitter separate floors poorly on real Wi-Fi:
  on the IPIN buildings a nearest-neighbour fingerprint finds the floor for 93-95% of scans, while the
  fitted slab loss is 2-4 dB ([ipin.md](ipin.md#failure-modes)). A venue can carry those fingerprints
  (`floor_fingerprints`). `pf.floor_prior_weight` then applies a k-NN floor vote per scan
  (`core/src/floor_classifier.cpp`); it is off by default.
- Particles change floor only inside a connector, so the floor can only follow once the position is right.
  `pf.baro_jump` (off by default) relaxes this: after a sustained one-floor barometer change, particles
  within 25 m of a connector move onto the new floor inside it.
  `pf.baro_reseed_keep_reference` makes new particles keep the cloud's barometer reference, so a re-seed
  on the wrong floor no longer fits the barometer by construction.
- Connectors are polygons on the plan, so a long staircase that is itself inside a corridor polygon must be
  drawn tightly, or particles can change level on the corridor floor next to it.
- Shadowing is correlated in time per beacon, not in space.
- Room events are only as good as the position. In the MSIL malls (7-11 m typical error) the recorded
  walks stay in the corridors, so every event the engine reports there is a false enter into a shop
  next to the corridor; rooms smaller than the error, or visits shorter than the dwell time, are not
  detectable. The malls' room events are a demo of the plumbing, not a result.
