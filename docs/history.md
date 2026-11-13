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

## Open questions

- **Compute on large floors.** The filter touches every cell at every update. Coarser cells away from the
  mass, or a hierarchy, are untried.
- **Uncertainty.** Coverage95 is 53–92% on the simulated walks and under a quarter on real Wi-Fi. Tempering
  a scan made the spread honest and cost accuracy at every setting tried.
- **Stairs taken away from the estimate.** Mass changes level only inside a connector.
- **A per-user step length.**
- **Whole-walk optimisation for offline use.** A grid makes an exact forward-backward smoother possible, at
  the cost of keeping every step's posterior.
- **Learned motion.** What separates the engine from the Kaggle winners most.
