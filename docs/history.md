# History

Why the engine is the way it is: each experiment's question, what was measured and what was kept. The
engine was a particle filter until experiment 16; entries 2 to 13 were run on it, and most of their
findings carried over, because the grid filter has the same sensor models, map handling and metrics. The
full log with every table is in the repository's history
(`git show 627d995:docs/experiments.md`, and `real_data.md`, `ipin.md`, `benchmarks.md`, `grid.md`,
`uji_ble.md` next to it).

Numbers are means over seeds on the simulated scenarios unless a dataset is named.

## What was kept, in one table

| # | Question | Outcome | In the engine today |
|---|---|---|---|
| 2 | Why does a moved beacon pull the estimate 17 m away? | Recovery mass found a foothold at the beacon's mapped position | Beacon integrity monitor; recovery mass kept tiny |
| 3 | Do results match across compilers? | `std::normal_distribution` differs between standard libraries | `ipe::Rng` in the simulator; a filter without random numbers |
| 4, 9 | Can the start be seeded from the beacons heard? | Yes for the particle filter, in large open venues | Not needed: the grid's uniform prior is exact |
| 5 | Can the map predict which beacons matter? | A Cramér-Rao score can reject layouts, not rank single removals | Removed with the planner (`ipe-plan`) |
| 6 | Why does the dot go through walls? | The mean of a split posterior lands on the wall | `map_consistent_estimate` |
| 7 | Why does the reported floor flicker? | It followed every packet | `LevelSelector`: averaging with hysteresis |
| 8, 11 | Why does the dot shake and jump? | The estimate was drawn as computed | `TrackSmoother`: steps at once, corrections glided |
| 10 | Can the loss per wall and slab be learned online? | Not per hypothesis; a venue-wide slab loss, weakly | Removed with the particle filter |
| 12 | Should repeated packets of one beacon count for less? | Worse than a plain wider sigma | Not kept |
| 13 | Why does the dot enter rooms without a door? | The venue was "everything inside the outline" | Closed regions are not cells |
| 14 | Would a grid do better than particles? | Yes on accuracy and smoothness, at 2–20 times the compute | It became the engine in 16 |
| 15 | How far is the engine from the competition winners? | Far, until training scans were used as position fingerprints | `filter.fingerprint_weight`, `fingerprint.json` |
| 16 | Are two filters needed? | No | The grid filter alone |
| 17 | What can go? | The planner, IMDF, the UJI study, three unused filter options | A smaller repository, identical results |
| 18 | Why must a user know which config file to pass? | The three files are one patch, limited by what the venue has | `recommended_config(venue)`; an installable package |
| 19 | Can the engine run live in a phone's browser? | Yes on motion sensors; a web page gets no radio | `on_position_fix`, `web/live.html`, recordings as traces |
| 20 | Must every update touch every cell? | No: 300 cells hold the mass; the rest can be coarse blocks | `filter.active_floor`, on by default since 22 |
| 21 | Does the active floor delay convergence on real Wi-Fi? | The delay was inside the metric's noise; the background was too light under the compass | Background blocks with a heading row that move with the steps; `datasets/fidelity.py` |
| 22 | Should the active floor be the default? | Decided on the numbers of 21; the gates pass unchanged | `filter.active_floor` 1e-9 by default |

## 2. A moved beacon hijacking the estimate (`office_field_issue`)

One beacon is mounted in a different room than the map says, and two are dead. The particle filter's
CEP95 on the walk was 5.41 m, with a 17 m error for ten seconds. The moved beacon reads 25 dB hot where
the walker is; the likelihood drops, recovery injects uniform hypotheses, and those next to the beacon's
*mapped* position explain its packets perfectly.

- **Kept:** a slower recovery trigger with wider priors on the device offset (5.41 → 2.19 m), and the
  **beacon integrity monitor**, which stops using a beacon whose averaged residual at the converged
  estimate exceeds 10 dB.
- **In the grid filter** the same trade-off sets the recovery rate: with 0.1% of uniform mass mixed in per
  second, CEP95 on this walk is 3.65 m; with the default 1e-8 it is 1.7 m.

## 3. Cross-platform determinism

The same seed gave different traces under libstdc++ and libc++, because `std::normal_distribution` is
implementation-defined. All sampling goes through `ipe::Rng` (xoshiro256** with Box-Muller), pinned by a
golden-value test, so a scenario's trace is the same natively and in WebAssembly.

## 4 and 9. Starting from the beacons heard

In the 80 x 40 m airport hall the particle filter took about 70 s to converge from a uniform start. Drawing
the first hypotheses on rings around the beacons heard fixed large open venues and slowed small walled
offices, so it was switched on by particle density (0.6 per m²). The grid filter needs none of it: its
uniform prior is exact. The airport still takes 34.5 s, because with 35% packet loss and beacons 20 m apart
the posterior itself stays ambiguous for half a minute.

## 5. Can the map predict which beacons matter?

`ipe-plan` scored a beacon layout from the map and the RSSI model alone: beacons heard above -85 dBm and
the Cramér-Rao bound on the horizontal error, pooled over the last 10 m of walk. It could reject bad
layouts, but single beacon removals had to be judged by simulation with more than 10 seeds. Removed in 17.

## 6. Estimates that go through walls

No hypothesis ever crossed a wall, but the reported position was the mean. When the posterior is split
across a wall (corridor and shop, two corridors), the mean lands on the wall or in an empty shop: 1,705 of
64,776 estimates on the ten mall demo walks. The filter now reports the mean of the dominant group when
the overall mean is off the map (`map_consistent_estimate`).

## 7. A reported floor that flickers

On `office_floor_bleed` the reported floor changed 20 times per walk for 2 real changes: it was the most
likely level of every update, and one packet can swap two floors that share the mass. The reported level
is now an average over 0.5 s with hysteresis; a real floor change shows about 0.4 s later. The
`level_flips` metric was added to make this visible.

## 8 and 11. A dot that shakes, then a dot that glides

Drawn as computed, the estimate shook (jitter p95 1.07 m on `office_nominal`, where the truth has 0.10 m)
and jumped more than 3 m in a second 4–9 times a minute on real walks. `TrackSmoother` first low-passed it
(8), then separated what the walker did from what the filter learned (11): a step moves the dot at once by
the step's own motion, the dot keeps moving between steps, and every other change is glided towards, never
through a wall. The Wi-Fi configs cap the glide at 1.5 m/s. Smoothness metrics (`jitter_p95_m`,
`jumps_per_min`, `wall_crossings_per_min`, `path_ratio`) date from here, and a change to what is reported
must quote them next to accuracy.

## 10. Learning the loss per wall and per slab online

The map's 3.5 dB per wall is 7 dB in `office_nlos_heavy`, and its 15 dB per slab is 2–4 dB in the IPIN
buildings. A loss per hypothesis did not work: the device offset and the wall loss trade off too easily. A
venue-wide slab loss calibrated from residuals at the converged estimate helped the particle filter a
little on IPIN. Neither exists in the grid filter; the floor vote and the fingerprints do that job on real
Wi-Fi. Per-wall-type losses from the map are the untried alternative.

## 12. Discounting repeated packets of one beacon (not kept)

Consecutive packets of one beacon share their shadowing, and the filter counts them as independent.
Scaling a reading's log-likelihood by the time since the beacon's last packet was worse than a plain wider
sigma on every mean. The engine keeps 5 dB for BLE, the measured per-packet spread.

## 13. A dot inside a room without a door

On the mall floors with shop walls, every shop no training walk entered is a closed room, and the dot
jumped into them (7 moves through a wall a minute on one walk). Start, recovery and the estimate treated
the venue as everything inside the outline. `ReachableArea` now finds the regions the walls close off; in
the grid filter their cells do not exist.

## 14. A grid Bayes filter instead of particles

Much of the particle filter's machinery existed because a sample covers the posterior sparsely: recovery
injections, the beacon-ring start, rejection sampling around closed rooms, a re-seed when every particle
died, and seed noise in every comparison. A grid holds the probability of every cell.

- **Better:** mean CEP50 and CEP95 on the scenarios, half the jitter, 60% fewer jumps; the two-floor walks
  converge in 1–2 s instead of 8–13 s.
- **Tried and not kept:** a three-value step-scale state (it fixed `office_step_scale` and cost most other
  offices), augmented-MCL recovery (it fires exactly when a beacon misbehaves) and a sparse active set (it
  saved nothing on Wi-Fi, where most cells stay above any useful floor). Their code was removed in 17.
- **Worse:** compute, and a covariance that is too tight.

## 15. Position fingerprints, against the competitions' leaderboards

The real datasets come from competitions, and the results had never been set against their leaderboards.
With a log-distance curve per access point the engine scored 15–42 m on the IPIN scoring walks, against
winning scores of 5.3 and 7.9 m; every winner used Wi-Fi fingerprints.

- **Change:** training scans keep their position in the venue, access points that fit no model stay as
  fingerprint dimensions, and the filter adds a kernel density around a scan's nearest training scans to
  its likelihood. A scan's k-NN position alone was no better than the old filter; the gain is in the
  fusion with steps, walls and the barometer.
- **Settings** (weight 3, 10 neighbours, 40 compared readings, 4 m kernel, model weight 0.3) were chosen on
  the IPIN validation walks; within wide ranges they move the medians by less than seeds did.
- **Result:** 5.0–8.6 m on the IPIN scoring walks, and 13.0 → 8.4 m on held-out Kaggle walks before the
  offline steps ([datasets.md](datasets.md)).

## 16. One filter: the grid filter becomes the engine

With fingerprints the grid filter was better on five of six IPIN scoring trials, equal on the mall floors
(Kaggle metric 8.76 against 8.70 m) and better on the scenarios. The particle filter, its proposals and its
tests were removed. Particle filter → grid, 10 seeds:

| | Mean CEP50 [m] | Mean CEP95 [m] | Jitter p95 [m] | Jumps/min | Through walls/min |
|---|---|---|---|---|---|
| 12 scenarios | 0.78 → 0.66 | 2.94 → 1.83 | 0.50 → 0.29 | 1.06 → 0.41 | 1.59 → 0.62 |

**What got worse:** `office_step_scale` CEP50 0.73 → 0.78 m; convergence 0.5–3 s later on five single-floor
offices and `office_elevator`; coverage95 82% → 74%; compute about 25 times higher on a mall floor; and the
particle filter's barometer-driven move to the nearest stairs has no counterpart.

## 17. Simplifying the repository

Removed: the beacon placement planner (5), IMDF import and export, the UJI BLE study, and three filter
options that were off by default and set by nothing (the step-scale state, adaptive recovery and the
active-cell floor of 14). The filter was split into one file per concern and the config JSON moved into
the core library. Every step reproduced the metrics of the 12 scenarios (10 seeds), the 109 mall walks and
the 6 IPIN scoring walks to the last digit.

**The UJI BLE study**, for the record: on surveyed beacons and standing-still fingerprints in a library and
a lab (CC BY), the per-packet RSSI spread was 5 dB, a shelf edge cost about 1 dB rather than the 3 dB the
map assumes per wall, and BLE alone gave 1.4–1.9 m CEP50.

## 18. Settings chosen from the venue, and a package

A mall walk replayed without `--config data/configs/fingerprint.json` ran with the settings for surveyed
BLE beacons, without a warning: a mean error of 19.3 m instead of 3.2 m on the demo's first mall walk. The
three config files turned out to be one patch:
the floor vote does nothing on one floor or without floor fingerprints, and the fingerprint likelihood
nothing without positions. `recommended_config(venue)` therefore needs only one question, whether the
venue has training scans, and `PositioningEngine(venue)` and the tools use it when no config is given.
The files stay for the regression gates and for Wi-Fi venues without training scans, which nothing in a
venue marks as such.

The library also became installable (`find_package(ipe)`), and inside another project's build it builds
only itself and no longer passes this repository's warning flags on.

## 19. Live in a phone's browser

Until here the engine only replayed: recordings and simulations. `web/live.html` feeds it from the phone's
own sensors as the user walks, and saves what it fed as a trace.

- **What a browser gives:** acceleration, turn rate and a compass heading. No Wi-Fi or BLE signal
  strength and, on most phones, no barometer. Live mode in a browser is therefore steps, heading and the
  map, from a start the user taps. Radio needs a native app.
- **A position fix** became an engine input for that tap (`on_position_fix`, a `fix` row in traces); it
  also serves a QR code or a GNSS fix at an entrance. Before it, the estimate was only valid after a
  radio reading.
- **No magnetometer** is now expressible: a zero horizontal field gives steps without an absolute
  heading. It used to read as "heading east".
- **The recording is the test.** The binding rounds every value to the trace file's precision before the
  engine sees it, so `ipe-replay` of a saved recording gives exactly what the page showed.
  `web/check_live.py` checks that in headless Chrome, feeding a simulated walk as browser sensor events.
- **Measured without a phone:** on the simulated office walk, a start fix, steps and walls alone give
  0.47 m mean error (0.50 m with the 12 beacons). Simulated steps are far cleaner than a real phone's, so
  this shows the plumbing, not what a phone will do.
- **Not known until tried on phones:** the step detector on browser sensor data at 60 Hz, the compass
  indoors, and the engine's load on a phone's processor (the page shows it).

## 20. Updating only where the mass is

Compute was the grid filter's cost since experiment 14: a mall walk took a third of its duration on a
desktop core. Half of the work scaled with cells × heading bins, the rest with cells × readings.

- **Measured first:** on an office, the airport hall, the KIT building and a mall floor of 136,491 cells,
  the 300 heaviest cells hold all of the mass once the walk has converged; the 300th is typically at 1e-13
  to 1e-10 of the total. Everything else is recovery mass. Experiment 14's active floor saved nothing because
  recovery was 1e-4 per second then, which kept every cell near any useful floor, and because it still
  visited every cell to test it.
- **Change** (`filter.active_floor`, [design.md](design.md#the-active-set)): the lattice is cut into 4 m
  blocks. Blocks holding a cell above the floor, and their neighbours, are updated cell by cell. Every
  other block is one coarse cell that radio and barometer updates score once. Dropping the far cells
  instead would have removed the way back from a wrong lock; a unit test locks the filter in the wrong
  corner of a hall, and with and without the floor it comes back after the same 16 packets.
- **Off (the default at the time):** results identical to before on the 12 scenarios (10 seeds), the 109 mall tune
  walks and the 27 IPIN tune walks, and 1–2% faster.

At 1e-9, 10 seeds, before → after (compute on one core, 2 seeds):

| Scenario | CEP50 [m] | CEP95 [m] | Converged [s] | Jitter p95 [m] | Jumps/min | Through walls/min | Floor ok, flips | µs/update |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| airport_hall_sparse | 1.01 → 1.00 | 3.76 → 3.76 | 34.5 → 34.5 | 0.20 → 0.20 | 0.2 → 0.2 | 0.05 → 0.05 | - | 348 → 48 |
| baro_drift_long | 0.54 → 0.54 | 1.42 → 1.42 | 1.0 → 1.0 | 0.28 → 0.28 | 0.6 → 0.6 | 0.17 → 0.17 | 99.2% → 99.2%, 4.2 → 4.2 | 246 → 33 |
| kit_tour | 0.66 → 0.64 | 2.67 → 2.67 | 0.7 → 0.7 | 0.26 → 0.26 | 0.9 → 0.9 | 1.19 → 1.16 | 99.0% → 99.0%, 3.0 → 3.0 | 1931 → 81 |
| office_device_offset | 0.49 → 0.49 | 1.15 → 1.15 | 5.2 → 5.2 | 0.31 → 0.31 | 0.1 → 0.1 | 0.73 → 0.73 | - | 110 → 34 |
| office_elevator | 0.54 → 0.54 | 1.28 → 1.28 | 2.5 → 2.5 | 0.22 → 0.22 | 0.9 → 0.9 | 0.12 → 0.12 | 99.0% → 99.0%, 2.0 → 2.0 | 221 → 31 |
| office_field_issue | 0.56 → 0.56 | 1.68 → 1.67 | 5.7 → 5.7 | 0.30 → 0.30 | 0.1 → 0.0 | 0.49 → 0.39 | - | 114 → 35 |
| office_floor_bleed | 0.68 → 0.68 | 1.50 → 1.53 | 1.8 → 1.7 | 0.32 → 0.31 | 1.0 → 1.0 | 0.52 → 0.52 | 97.9% → 98.0%, 2.9 → 2.8 | 253 → 42 |
| office_mag_disturbance | 0.94 → 0.94 | 2.65 → 2.65 | 6.1 → 6.1 | 0.37 → 0.37 | 0.3 → 0.3 | 2.60 → 2.60 | - | 117 → 35 |
| office_nlos_heavy | 0.57 → 0.57 | 1.28 → 1.28 | 3.8 → 3.8 | 0.28 → 0.28 | 0.0 → 0.0 | 0.34 → 0.34 | - | 112 → 34 |
| office_nominal | 0.55 → 0.55 | 1.14 → 1.14 | 2.2 → 2.2 | 0.29 → 0.29 | 0.0 → 0.0 | 0.39 → 0.39 | - | 118 → 33 |
| office_stairs_up_down | 0.58 → 0.58 | 1.62 → 1.62 | 1.2 → 1.2 | 0.30 → 0.30 | 0.8 → 0.8 | 0.08 → 0.08 | 99.1% → 99.1%, 2.0 → 2.0 | 248 → 36 |
| office_step_scale | 0.78 → 0.78 | 1.77 → 1.77 | 3.3 → 3.3 | 0.28 → 0.28 | 0.0 → 0.0 | 0.78 → 0.78 | - | 119 → 33 |

Real walks, tune sets, all walks pooled (off → 1e-12 → 1e-9 → 1e-6):

| | Mean [m] | CEP50 [m] | CEP95 [m] | Converged [s] | Jitter p95 [m] |
|---|---:|---:|---:|---:|---:|
| 109 mall walks, `msil_wifi.json` | 11.16 → 11.08 → 11.05 → 11.07 | 10.62 → 10.58 → 10.58 → 10.59 | 16.24 → 16.25 → 16.21 → 16.06 | 44.4 → 43.8 → 44.4 → 46.0 | 0.35 → 0.35 → 0.35 → 0.34 |
| 27 IPIN walks, `fingerprint.json` | 4.55 → 4.45 → 4.35 → 4.25 | 3.56 → 3.50 → 3.57 → 3.41 | 10.61 → 10.15 → 9.59 → 9.36 | 79.9 → 74.2 → 89.0 → 90.6 | 0.67 → 0.63 → 0.64 → 0.65 |

Held out, replayed once at 1e-9 (off → 1e-9, all walks pooled):

| | Mean [m] | CEP50 [m] | CEP95 [m] | Converged [s] | Jitter p95 [m] |
|---|---:|---:|---:|---:|---:|
| 109 mall walks, `msil_wifi.json` | 10.75 → 10.71 | 10.79 → 10.73 | 14.40 → 14.34 | 24.4 → 26.0 | 0.32 → 0.31 |
| 6 IPIN scoring walks, `fingerprint.json` | 5.15 → 5.17 | 4.15 → 4.00 | 11.85 → 13.24 | 78.2 → 120.5 | 0.53 → 0.54 |

Two demo walks on one core, off → 1e-12 → 1e-9 → 1e-6: the mall floor site1_F2 (160 s, 136,491 cells)
replays in 49.6 → 3.4 → 2.4 → 2.3 s, the Nuremberg walk in 17.5 → 6.4 → 2.7 → 1.6 s.

- **What differed at 1e-9:** convergence on IPIN. The tune walks converge 9 s later in the mean and the
  scoring walks 42 s later (Nuremberg 100 → 163 s), and the scoring walks' pooled CEP95 is 11.85 → 13.24 m
  (UMinho 14.5 → 18.8 m). Nuremberg's CEP50 on the 4 tune walks is 2.85 → 2.99 m, with coverage95 at 18%
  instead of 22%. At 1e-12 the tune walks converge no later and Nuremberg's CEP50 is 2.89 m, at 2.4 times
  the compute on its demo walk; the scoring walks were not replayed at 1e-12. Two mall floors with one
  held-out walk each are 0.6–0.9 m worse in the mean (site2_F2, site2_F4) and one is 1.1 m better
  (site2_F7). This entry first called the convergence "what got worse". Experiment 21 measured the noise
  of these numbers: the 9 s on the tune walks is inside it. What was wrong with the background is there
  too.
- **Tried first:** background blocks that keep the prior device offset and a constant barometer
  likelihood. Simpler, and the mall walks were 0.5 m *better* in the mean with it than without a floor
  (10.65 against 11.16 m at 1e-9). Such a background responds less, and recovery mass costs on real Wi-Fi
  (experiment 14). That is a change to recovery, not to compute, so the blocks got offsets of their own,
  which follow the exact filter more closely.
- **Not done:** the default. It stays 0 until the floor has been tried on a phone and on the Kaggle walks.
  The first seconds of a walk still cost a full update per cell; there, `log10`, `exp` and `log` per
  (cell, reading) are what is left to cut.

## 21. Does the active floor delay convergence on real Wi-Fi?

Experiment 20 reported a later convergence on the IPIN buildings with `active_floor` 1e-9. Looked at on
the 27 tune walks only.

- **What the number is.** IPIN's ground truth is waypoints about 30 s apart, 10 to 28 per walk. Converged
  (the error stays under 3 m for 10 s) is then the first waypoint under 3 m. The 9 s came from 2 of 27
  walks: one waypoint at 2.90 → 3.05 m (+28 s), and one at 2.17 → 3.40 m before a gap of 164 s (+217 s).
- **The noise of a comparison.** A replay is deterministic, but on these walks the filter amplifies small
  differences. `datasets/fidelity.py` replays a manifest with two patches and compares the tracks (the
  estimate every 0.2 s). Against the exact filter:

| Run | Track apart, p50 / p95 [m] | Over 3 m apart | Converged [s] | Mean / CEP95 [m] |
|---|---:|---:|---:|---:|
| exact | - | - | 79.9 | 4.55 / 10.61 |
| `rssi.sigma_db` 8 → 8.01 | 0.22 / 1.61 | 1.9% | 79.9 | 4.51 / 10.33 |
| `teleport_rate_per_s` 1e-8 → 2e-8 | 0.22 / 1.63 | 2.2% | 86.6 (one walk +180 s) | 4.49 / 10.56 |
| `step_length_rel_sigma` 0.2 → 0.201 | 0.21 / 1.64 | 2.1% | 71.8 (one walk -293 s) | 4.49 / 10.65 |
| `heading_sigma_deg` 3 → 3.01 | 0.22 / 1.61 | 2.1% | 79.9 | 4.52 / 10.68 |
| `active_floor` 1e-12, experiment 20 | 0.22 / 1.86 | 2.5% | 74.2 | 4.45 / 10.15 |
| `active_floor` 1e-9, experiment 20 | 0.30 / 3.18 | 5.4% | 89.0 | 4.35 / 9.59 |

  Mean convergence moves by -8 to +7 s when nothing has changed, so the floor's +9 s was not a result.
  The 111 mall tune walks are steadier: the same change of the step noise puts the track 0.47 m apart at
  p95 and over 3 m apart 0.3% of the time.
- **What was real.** At 1e-9 the track was over 3 m from the exact filter's 5.4% of the time, against 2%
  for a change in a setting's third digit: 848 s in 398 episodes, 18 of them of 10 s or more, where the
  noise has 279 s in 254 episodes and one that long. For those episodes the filter was following another
  hypothesis than the exact one.
- **Where it came from.** With parts switched off in both filters (8 walks), the device offsets, the
  recovery mass and the estimate on the map each left the gap as it was. The width of the ring of active
  blocks did not: 3.0–3.2 m at p95 with a 4 m ring, 2.1–2.3 m with an 8 m ring, whatever the block size.
  So it was mass just under the floor, and what the background did to it:
  - It had no heading. A cell learns the heading at its first magnetometer update and agrees with the
    later ones; the background took the mean over all headings at every step. In a unit test of 20
    steps with a compass it held a sixth of the mass the same cells hold in the exact filter. Mass that
    became active again started with any heading.
  - It did not move. A hypothesis folded into a block stayed where it was folded while the walker went on.
- **Change:** a background block has a heading row and the centroid of its mass
  ([design.md](design.md#the-active-set)). The magnetometer updates the row as a cell's; a step moves the
  mass as a whole by the row's mean move, into the next block or into the cell it lands in; cells of a
  block that becomes active get the row. A radio update scores the block at the centroid.
- **Tried and removed**, each on the 27 walks at 1e-9 (p95 apart, share over 3 m; before: 3.18 m, 5.4%):
  - Moves from active cells into the background added to the block instead of dropped: 3.19 m, 5.4%.
  - A block made active before the scan that lifts it is applied, so that its cells read that scan
    themselves: 3.13 m, 5.3% at the activation threshold, 3.04 m, 5.1% at the floor itself.
  - On top of the change, the spread of a block's mass, to bring it back around its centroid and not
    evenly: 2.20–2.22 m and 3.1%, against 2.25–2.32 m and 3.3–3.5% without. No more than the options
    above moved it when switched on and off next to the change (2.27–2.36 m).
  - Background mass that walks through walls, or stays in front of them: 2.31 and 2.42 m against 2.32 m.
    The wall test stayed, because cells have it.

At 1e-9, exact → experiment 20 → now:

| | Exact | Experiment 20 | Now |
|---|---:|---:|---:|
| 27 IPIN tune walks: track apart from exact, p50 / p95 [m] | - | 0.30 / 3.18 | 0.27 / 2.32 |
| ... over 3 m apart; seconds in episodes; episodes of 10 s or more | - | 5.4%; 848; 18 | 3.4%; 523; 4 |
| ... mean / CEP50 / CEP95 [m] | 4.55 / 3.56 / 10.61 | 4.35 / 3.57 / 9.59 | 4.57 / 3.59 / 10.62 |
| ... converged [s] | 79.9 | 89.0 | 83.2 |
| 111 mall tune walks: track apart, p95 [m]; over 3 m apart | - | 1.01; 2.1% | 0.90; 1.0% |
| 109 mall tune walks, pooled: mean / CEP50 / CEP95 [m] | 11.16 / 10.62 / 16.24 | 11.05 / 10.58 / 16.21 | 11.25 / 10.69 / 16.30 |
| 12 scenarios, 10 seeds: mean CEP50 / CEP95 [m] | 0.656 / 1.827 | 0.654 / 1.828 | 0.653 / 1.827 |
| 12 scenarios, one core [µs/update] | 108–1897 | 30–80 | 30–84 |
| Mall floor site1_F2, 160 s walk, replay [s] | 48.5 | 2.29 | 2.75 |
| Nuremberg demo walk, replay [s] | 17.4 | 2.52 | 3.36 |

Held out, replayed once with the final code at 1e-9 (all walks pooled):

| | Exact | Experiment 20 | Now |
|---|---:|---:|---:|
| 109 mall walks: mean / CEP50 / CEP95 [m] | 10.75 / 10.79 / 14.40 | 10.71 / 10.73 / 14.34 | 10.77 / 10.80 / 14.44 |
| ... converged [s] | 24.4 | 26.0 | 24.4 |
| 6 IPIN scoring walks: mean / CEP50 / CEP95 [m] | 5.15 / 4.15 / 11.85 | 5.17 / 4.00 / 13.24 | 5.13 / 3.94 / 12.11 |
| ... converged [s] | 78.2 | 120.5 | 120.5 |

- **Off (the default at the time):** results identical to before on the 12 scenarios (10 seeds), the 109 mall tune
  walks and the 27 IPIN tune walks.
- **What it follows now.** The exact filter, not a better number: experiment 20's lower CEP95 on the IPIN
  tune walks (9.59 m) was a difference from the exact filter, and is gone with it.
- **What is left.** The track is still over 3 m apart 3.4% of the time where the noise is at 2%, and 1.0%
  against 0.3% on the mall walks. At 1e-12 the IPIN tune walks are at the noise (1.85 m at p95, 2.5%), for
  6.7 s on the Nuremberg walk. The scoring walks' convergence did not move (Nuremberg 100 → 163 s over 4
  walks, a sum of 252 s). On the tune walks single walks move by 180 to 293 s without a real change, but
  the scoring walks' own noise was not measured: they are replayed once. One mall floor where nothing
  localises (site2_F7, 3 walks, 34 m mean error) went the other way: 34.3 → 31.4 → 37.4 m.
- **Cost.** On the Nuremberg walk the active set is larger (3,817 → 5,138 cells in the mean): the
  background weighs more, so more blocks pass the threshold. On the mall floor it is the same size (2,638
  → 2,676) and the extra time is the work on the blocks themselves, which was not profiled further.
- **For the next comparison on real walks:** run a change in a setting's third digit through
  `datasets/fidelity.py` first. A difference that is no larger is not a result.

## 22. The active floor becomes the default

`filter.active_floor` is 1e-9 by default, on every venue. This is a decision on the numbers of experiment
21, not the outcome of a new experiment. What the default was checked against:

- **The gates.** The three baselines in `tests/regression/` pass unchanged.
- **Simulated scenarios** (10 seeds, one core): mean CEP50 0.657 → 0.654 m, CEP95 1.827 → 1.827 m, and
  107–1,935 → 31–74 µs per sensor update. Three scenarios' CEP50 moved by 0.01 to 0.02 m; right floor,
  convergence and jitter are the same.
- **Held-out walks**, replayed with the exact filter and with the default to report; nothing was chosen on
  them:

| Held-out walks | Exact | Default |
|---|---:|---:|
| 109 mall walks, `msil_wifi.json`: median CEP50 / CEP95 [m] | 6.76 / 9.41 | 6.75 / 9.41 |
| ... mean CEP50 / CEP95 [m] | 10.79 / 14.40 | 10.80 / 14.44 |
| 109 mall walks, `fingerprint.json`: median CEP50 / CEP95 [m] | 5.18 / 8.94 | 5.18 / 8.94 |
| ... mean CEP50 / CEP95 [m] | 7.35 / 10.63 | 7.29 / 10.63 |
| 6 IPIN scoring trials, `fingerprint.json`: competition metric [m] | 6.58, 5.14, 8.63, 4.97, 7.71, 6.82 | 6.79, 5.24, 9.23, 5.48, 7.57, 6.57 |
| ... median CEP50 / CEP95 [m] | 4.01 / 14.68 | 4.02 / 14.79 |
| ... right floor | 96% | 96% |

- **A claim that changed.** The best 2023 trial in Nuremberg scores 5.5 m, not 5.0 m: behind that year's
  winner (5.3 m), where the exact filter is ahead of it. [datasets.md](datasets.md) now quotes the
  default's numbers for the scoring trials. Whether 0.5 m on one trial is more than noise was not
  measured: the six trials were not replayed with a setting changed in its third digit.
- **Where the localisation is poor, the two filters part further.** With `ipin.json` (no position
  fingerprints, errors of 10 to 25 m) the six trials score 11.9, 8.1, 16.5, 19.0, 14.8 and 27.4 m exact
  and 11.2, 11.1, 17.5, 13.2, 19.5 and 25.9 m with the default: single trials move by up to 6 m in both
  directions, the median from 15.6 to 15.3 m. (Neither is the `ipin.json` column of datasets.md, which
  is from an earlier conversion and was not reproduced.)
- **Not repeated with the default:** the Kaggle held-out table and the submission (made with the exact
  filter), the demo-walk table in datasets.md, the timing in WebAssembly, and a phone.
- `active_floor` 0 is the exact filter, as before.

## Open questions

- **Compute before convergence.** With `active_floor` the cost follows the mass, but until the first
  scans have concentrated it every cell is updated, at the cost of experiment 14.
- **Uncertainty.** Coverage95 is 53–92% on the simulated walks and under a quarter on real Wi-Fi. Tempering
  a scan made the spread honest and cost accuracy at every setting tried.
- **Stairs taken away from the estimate.** Mass changes level only inside a connector.
- **A per-user step length.**
- **Whole-walk optimisation for offline use.** A grid makes an exact forward-backward smoother possible, at
  the cost of keeping every step's posterior.
- **Learned motion.** What separates the engine from the Kaggle winners most.
