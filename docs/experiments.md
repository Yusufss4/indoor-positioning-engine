# Experiment log

Each entry: the observation, the hypothesis, the change, and the measured effect across all scenarios.
Numbers are means over seeds (`ipe-regress --no-baseline --seeds N`).

## 2. Moved beacon hijacking the particle filter (`office_field_issue`)

**Observation.** The particle filter's CEP95 on this scenario was 5.41 m (5 seeds), with a 17 m error
plateau between t = 30 s and 40 s.

**Diagnosis.** S3 is mapped in the south meeting room but physically sits in room N3. When the walker
enters N3, S3 reads ~25 dB stronger than the map predicts. The short-term likelihood drops, augmented-MCL
recovery injects uniform particles, and the ones that land next to S3's *mapped* position explain its
packets perfectly. The doors line up (x = 25 m on both corridor walls), so the other beacons barely
contradict the alias. Once the cloud has jumped, walls keep it there until those particles die out.

**Things tried** (5-8 seeds, all seven scenarios):

| Variant | field_issue CEP95 | device_offset CEP95 | airport CEP95 | Verdict |
|---|---:|---:|---:|---|
| original (`alpha_fast` 0.3, offset prior 5 dB) | 5.41 | 2.41 | 6.58 | hijack |
| recovery off | 2.30 | 8.18 | 6.34 | device offset never learned |
| recovery off + wider priors | 2.11 | 1.66 | 13.14 | airport loses track in 1/5 runs |
| `alpha_fast` 0.1 + wider priors | 2.62 | 2.99 | 8.35 | |
| **`alpha_fast` 0.05 + wider priors** | **2.19** | **2.04** | **7.43** | kept |

"Wider priors" means device-offset prior 5 -> 8 dB and step-scale prior 0.10 -> 0.15. Most recovery
injections were really compensating for priors too narrow to contain the true offset or step scale. With
wider priors, recovery only has to handle real loss of track, so it can react more slowly and is no
longer triggered by a few seconds of one inconsistent beacon.

**Beacon integrity monitor.** Added at the same time: per-beacon residual averaging at the converged
estimate, with exclusion above 10 dB. Its effect on top of the recovery change is small but consistent
(field_issue CEP95 2.32 -> 2.19 m, others unchanged within noise).

![before/after](img/field_issue_before_after.png)

**20-seed check of the kept change.** The 5-8 seed numbers above were too noisy to judge side effects,
so the original and kept configurations were rerun with 20 seeds (CEP95, m):

| Scenario | original | kept | |
|---|---:|---:|---|
| office_field_issue | 3.69 | 2.01 | better |
| office_device_offset | 3.77 | 2.87 | better |
| office_nominal | 1.51 | 1.54 | same |
| office_nlos_heavy | 1.80 | 1.75 | same |
| office_mag_disturbance | 2.48 | 2.52 | same |
| office_step_scale | 1.51 | 1.76 | worse |
| airport_hall_sparse | 5.60 | 7.27 | worse |

Reverting one piece at a time (20 seeds) shows both halves of the change cost the airport hall:
offset prior back to 5 dB gives airport 6.32 m but device offset 4.68 m; `alpha_fast` back to 0.3 gives
airport 6.33 m but field issue 2.53 m. The integrity monitor makes no difference to the airport hall
(7.27 m either way). No single setting won everywhere, so the kept configuration favours the
deployment faults, which are the common field complaint, and the airport cost is tracked below.

## 3. Cross-platform determinism

The WebAssembly demo (Clang + libc++) and the native CLI (GCC + libstdc++) report identical metrics for
the same seed, e.g. `office_field_issue` seed 1: mean 0.71 m / CEP95 1.70 m. This holds because all randomness uses `ipe::Rng` instead of `std::normal_distribution`.

## 4. Beacon-driven proposal (airport hall convergence)

**Observation.** Airport hall convergence took ~70 s, and the slower recovery trigger from experiment 2
had cost it CEP95 (5.6 -> 7.3 m). The particle filter starts uniform over 3200 m^2 and injects recovery
particles uniformly too, so a sparse hall with 35% packet loss needs a long time to find the right mode.

**Change.** Two opt-in proposals (`pf.init_proposal`, `pf.recovery_proposal`):

- *Init.* Packets are collected for `init_window_s` (1 s). The one with the shortest apparent range then
  re-seeds the cloud: 80% of the particles come from a range ring around that beacon, 20% stay uniform.
  The ring inverts the path-loss model for each particle's own device offset, so offset and range are
  consistent. Every particle is importance-weighted by prior/proposal, and then every collected packet
  is applied as usual. The result is the same posterior as a uniform start, sampled more densely where
  the radio says the user is. A unit test checks that the weighted cloud reproduces the uniform prior
  over position, device offset and level; another checks the ring density against a brute-force grid.
- *Recovery.* Augmented-MCL injections are drawn from the ring of a random packet heard in the last 2 s
  instead of uniformly.

Two earlier versions were discarded. The first drew the range and the device offset independently, and
most particles had a range that did not fit their own offset. The second seeded from the first packet
received, often a far beacon behind walls. Both were worse in every office scenario.

**Result** (CEP95 [m] / convergence, mean over 20 seeds, `pf` preset):

| Scenario | uniform | init | recovery | both |
|---|---:|---:|---:|---:|
| airport_hall_sparse | 7.26 / 71 s | **5.40 / 41 s** | 7.66 / 73 s | 5.70 / 42 s |
| baro_drift_long | 2.02 / 12 s | 1.87 / 6 s | 1.84 / 9 s | 1.81 / 4 s |
| office_stairs_up_down | 2.89 / 10 s | 2.04 / 4 s | 2.44 / 8 s | 1.86 / 4 s |
| office_step_scale | 1.76 / 2 s | 1.58 / 5 s | 1.71 / 2 s | 1.68 / 6 s |
| office_floor_bleed | 4.22 / 7 s | 4.97 / 21 s | 3.72 / 10 s | 3.81 / 16 s |
| office_device_offset | 2.87 / 6 s | 3.82 / 19 s | 2.12 / 5 s | 2.99 / 13 s |
| office_field_issue | 2.01 / 4 s | 2.60 / 7 s | 2.83 / 4 s | 2.36 / 8 s |
| office_elevator | 1.95 / 2 s | 2.67 / 10 s | 2.00 / 3 s | 2.53 / 7 s |
| office_mag_disturbance | 2.52 / 3 s | 2.66 / 5 s | 2.44 / 3 s | 2.64 / 4 s |
| office_nlos_heavy | 1.75 / 2 s | 2.65 / 9 s | 1.74 / 2 s | 2.71 / 9 s |
| office_nominal | 1.54 / 2 s | 1.78 / 5 s | 1.52 / 2 s | 1.86 / 4 s |

The init proposal answers the airport question. Convergence drops from 71 s to 41 s, and CEP95 drops to
5.4 m, below the 5.6 m the hall had before experiment 2. The two-floor walks gain too. It costs every
small walled office: convergence goes from 2-6 s to 5-21 s and CEP95 rises by 0.2-1 m.

The recovery proposal alone is close to neutral on the walls-and-offset scenarios and helps
`office_device_offset` and `office_floor_bleed`. It also brings back the moved-beacon hijack of
experiment 2 (`office_field_issue` 2.01 -> 2.83 m): when the packet that
triggers recovery comes from the moved beacon, the new particles land at its mapped position.

Ruled out as the cause of the office cost:

- The importance weights, which pass the prior-recovery test.
- The integrity monitor: disabling it does not change the failing seeds.
- The ring width: 2x and 3x the RSSI sigma were no better at 10 seeds.

The likely reason, not verified: 1500 particles over a 40 x 24 m office are already 1.6 per m^2, so a
uniform start samples the prior well. Putting 80% of them on one free-space ring, which ignores walls,
trades away diversity in heading, step scale and offset for density the office does not need.

**Kept.** Both proposals stay off by default, so `tests/regression/baseline.json` is unchanged and
the default filter is bit-identical (unit test `ProposalSettingsAreInertWhenDisabled`, and identical
10-seed regression output). [`data/configs/open_venue.json`](../data/configs/open_venue.json) turns on
the init proposal for large open venues:
`ipe-replay --config data/configs/open_venue.json ...`.

## 5. Can the map predict which beacons matter? (`ipe-plan`)

**Question.** A beacon-removal planner needs a cheap score that ranks layouts the way the filter's error
does. `ipe-plan analyze` scores a layout from the map and RSSI model alone: beacons heard above -85 dBm,
and the Cramer-Rao bound on horizontal error with the device offset as an unknown, either per point or
pooled over the last `--window` metres of walk.

**Method.** `viz/placement_study.py` thins the venue of four scenarios: every beacon dropped in turn
(drop-one) and every k-th beacon kept (k = 2, 3, 4). Each layout is simulated with `pf`, twice with
disjoint seeds (10 each). The target is the mean CEP of both runs; how well the two runs agree with
each other bounds what any score can reach. Spearman correlation with CEP50, all layouts / drop-one
only (KIT has 155 beacons, so no drop-one):

| Scenario | Layouts | Two runs agree | Beacons heard | Bound p50, per point | Bound p50, 10 m window | Bound p50, 20 m window |
|---|---|---:|---:|---:|---:|---:|
| `office_nominal` | 22 / 12 | 0.48 / -0.20 | 0.81 / 0.40 | 0.67 / -0.18 | 0.84 / 0.50 | 0.84 / 0.54 |
| `airport_hall_sparse` | 18 / 10 | 0.82 / 0.52 | 0.61 / -0.19 | 0.45 / -0.31 | 0.60 / -0.33 | 0.69 / 0.09 |
| `office_floor_bleed` | 34 / 24 | 0.79 / 0.52 | 0.74 / 0.30 | 0.74 / 0.28 | 0.75 / 0.34 | 0.77 / 0.39 |
| `kit_tour` | 10 / - | 0.78 / - | 0.72 / - | 0.70 / - | 0.85 / - | 0.83 / - |

**Findings.**

- Pooling along the walk is what makes the bound useful. Per point, an office with 4 of its 12 beacons
  has no bounded fix on 86% of the walk, yet `pf` reaches CEP50 0.65 m (full layout 0.66 m), because
  dead reckoning and walls carry it between beacons. With a 10 m window the bound ranks thinned layouts
  at 0.60-0.85 and its scale is close to the filter's (0.61 m vs CEP50 0.66 m on `office_nominal`).
- Drop-one decisions are not predictable from the map. On `office_nominal` two 10-seed runs do not
  even agree which beacon matters (-0.20): the differences are seed noise. On the airport and
  `office_floor_bleed` they agree at 0.52, and no score gets there.
- Some beacons hurt. Dropping H1-H4 (along the first leg) lowers airport CEP50 from 1.29 m to
  1.10-1.23 m. The simulated path-loss exponent and wall loss differ from the map, so a far beacon adds
  bias. An information bound cannot see this: under the model a beacon never hurts.
- The median along the walk barely moves when one beacon goes (0.58 m for most drop-one layouts of
  `office_floor_bleed`); a root-mean-square aggregate was tried and ranked worse.
- KIT's 10 m grid has slack, but which half stays matters: keeping 77 of 155 gives CEP50/CEP95
  2.78/10.15 m against 2.27/11.10 m, the other 78 give 3.39/16.37 m.

**Kept.** `ipe-plan` with a 10 m window as default. A removal planner can use it to reject layouts, but
single removals have to be judged by simulation with more than 10 seeds.

## 6. Estimates that go through walls

**Symptom.** In the web demo the dot slides into and through walls, mostly on the mall walks.

**Cause.** No particle crosses a wall: 1.3 M particle steps on two mall walks were checked with
`crosses_wall(before, after)`, and none did. The reported position is the weighted mean of the cloud,
though. When the cloud is split across a wall (corridor and shop, two corridors), the mean lands between
the groups: on the wall, in an empty shop, or behind a wall from every particle. On the ten `data/real`
demo walks (shop walls), 1,705 of 64,776 estimates were outside the walkable area or separated by a wall
from their nearest particle.

**Change.** The filter reports the mean of its dominant group when the cloud mean is not on the map
(`pf.map_consistent_estimate`, on by default). It only changes what is reported; the integrity monitor
and recovery keep using the cloud mean, so the filter evolves as before. The web demo leaves a gap in the
trail where two consecutive estimates are separated by a wall.

**Results.** CEP50 / CEP95 [m]. Simulated: 10 seeds per scenario. Real: `ipe-regress --traces`
with `msil_wifi.json`, median over walks. "Before" is the code before the change, and is bit-identical
to `map_consistent_estimate=false`.

| Data | Before | After |
|---|---:|---:|
| office_nominal | 0.66 / 1.59 | 0.66 / 1.59 |
| office_field_issue | 0.72 / 2.00 | 0.72 / 1.98 |
| office_mag_disturbance | 1.09 / 2.63 | 1.09 / 2.60 |
| airport_hall_sparse | 1.30 / 7.25 | 1.30 / 7.25 |
| kit_tour | 2.27 / 11.10 | 2.28 / 11.02 |
| MSIL holdout, floor outline (109 walks) | 11.79 / 15.25 | 11.79 / 15.43 |
| MSIL holdout, shop walls (demo maps) | 12.23 / 17.28 | 12.20 / 17.45 |
| MSIL tune, shop walls | 11.45 / 15.83 | 11.36 / 16.38 |

The other seven simulated scenarios are unchanged to within 0.01 m. On the ten demo walks, estimate moves
that cross a wall go from 4.8% to 3.6%, and estimates off the map from 1,705 to 0. The remaining moves
through a wall are jumps from one group of particles to another, which the demo draws as gaps.

**Findings.**

- The change is neutral on accuracy. Simulated scenarios move by 0.01 m or less. On real data CEP50 is
  unchanged and CEP95 rises by 1-3%. One `real_data_regression` group moves past the 10% tolerance:
  site2 F5, CEP95 75.6 to 84.3 m. Its 2 walks are lost in every configuration (CEP50 54 m), and when the
  cloud is lost its mean happens to be closer to the truth than either group.
  `tests/regression/msil_baseline.json` was regenerated to accept it.
- `compute_us_per_update` (`--serial`, 2 seeds) moves by 1% or less.

**Kept.** `map_consistent_estimate` on.

## 7. A reported floor that flickers

**Symptom.** On `office_floor_bleed` the reported floor changed 20 times per walk for 2 real changes
(10 seeds), and on the IPIN UMinho validation walks 20 times for 2. Each flip also moves the dot, because
the position comes from the particles on the reported floor.

**Cause.** The reported level was the most likely level of every update. When the cloud is split between
floors, one packet moves enough weight to swap the two. The new smoothness metrics
([formats.md](formats.md#replay-outputs)) made this visible: `level_flips` against `level_changes`.

**Change.** `LevelSelector` (`core/src/level_selector.cpp`) averages the level weights with a 0.5 s time
constant (`pf.level_smoothing_s`). It moves the reported level only when another level leads by 0.1 in
the average (`pf.level_hysteresis`), or at once when the reported level has no particle left. The filter's
own decisions keep using the most likely level of the update. Both settings at 0 reproduce the old
output bit for bit (10 seeds, every metric).

**Results.** 10 seeds; floor flips / true changes, floor accuracy, switch latency, jumps per minute:

| Scenario | Flips before | Flips after | Floor ok | Switch [s] | Jumps/min |
|---|---:|---:|---:|---:|---:|
| office_floor_bleed | 20.2 / 2 | 3.8 / 2 | 96.3% -> 96.6% | 1.0 -> 2.5 | 5.3 -> 4.8 |
| kit_tour | 9.5 / 3 | 4.4 / 3 | 94.2% -> 93.9% | 0.5 -> 0.9 | 4.3 -> 4.1 |
| baro_drift_long | 5.8 / 4 | 4.4 / 4 | 99.5% -> 99.1% | 0.4 -> 0.8 | 1.2 -> 1.3 |
| office_elevator | 3.0 / 2 | 2.0 / 2 | 99.5% -> 99.0% | 0.4 -> 0.8 | 1.8 -> 1.9 |
| office_stairs_up_down | 3.2 / 2 | 2.0 / 2 | 99.5% -> 98.9% | 0.4 -> 0.8 | 1.9 -> 2.1 |

CEP50/CEP95 move by 0.05 m or less on every scenario. The single-floor scenarios and the MSIL malls are
bit-identical. IPIN validation walks (`ipin.json`): floor flips 7.5 -> 3.5 (Nuremberg) and 20 -> 14
(UMinho) for 2 real changes each. Floor hit rate (3 seeds) 83% -> 83% and 69% -> 70%, IPIN p75
unchanged.

Variants tried and dropped (10 seeds):
- **Hold for 1.5 s.** A 0.2 weight lead or 1.5 s as the most likely level, without averaging, still let
  single packets through: 18.8 flips on `office_floor_bleed`.
- **Averaging over 1 s.** 3.0 flips instead of 3.8 on `office_floor_bleed`, but switch latency
  1.2-3.1 s and floor accuracy 1 point lower on the stairs and lift walks.
- **Leaving a floor below 25% weight at once.** Meant to cut the lag on real floor changes, it brought the
  flicker back: 11.2 flips on `office_floor_bleed`.
- **Horizontal position from the most likely floor while the reported floor lags.** Worse jitter on
  `office_floor_bleed` (jitter p95 5.6 vs 5.2 m).
- **Sticky dominant group** in the map-consistent estimate (keep the group next to the previous
  estimate unless another block outweighs it 1.5x or 2.5x). No measurable change on any scenario or on
  MSIL, so it was not kept.

**Kept.** The averaged, hysteretic reported level, on by default. The jumps that remain are horizontal:
the dot moves one step at a time and follows every radio update (jitter p95 about 1 m on
`office_nominal`, 4 jumps per minute on MSIL and 8 on IPIN). That is the next change.

## 8. A dot that shakes: smoothing what is drawn

**Symptom.** The filter recomputes its estimate after every packet, step and heading update. Drawn as-is
it shakes even when the walk is clean. On `office_nominal` the second difference of the 5 Hz track has a
95th percentile of 1.07 m (the truth: 0.10 m), and the drawn path is 1.39 times the walked one. On real
walks the dot jumps more than 3 m in a second 3.8 times a minute (MSIL) and 8-9 times a minute (IPIN).

**Change.** `TrackSmoother` (`core/src/track_smoother.cpp`) sits between the filter and
`PositioningEngine::estimate()`:
- a step moves the dot by the filter's own displacement for that step, at once;
- other updates pull it towards the filter's estimate with time constant `smoothing.tau_s`;
- it jumps instead when the filter is more than `smoothing.snap_m` away, when the level changes, or when
  gliding would cross a wall.

The covariance is taken about the drawn point. `raw_estimate()` is the filter's own;
`smoothing.enabled = false` is bit-identical to it (unit test `DisabledEngineReportsTheFilterEstimate`).

**Settings** (means over the 12 scenarios, 10 seeds each; MSIL = 109 tune walks, median over walks):

| Setting | CEP50 | CEP95 | Jitter p95 | Jumps/min | MSIL CEP50 / CEP95 | MSIL jumps/min |
|---|---:|---:|---:|---:|---:|---:|
| off | 0.924 | 3.433 | 1.66 | 1.56 | 6.37 / 10.19 | 3.8 |
| tau 1 s, cap 1.5 m/s, snap 6 m, extrapolate 1 step | 0.926 | 3.606 | 1.06 | 1.39 | | |
| tau 0.3 s, no cap, snap 10 m, extrapolate 1 step | 0.922 | 3.507 | 0.99 | 1.47 | | |
| tau 0.3 s, no cap, snap 10 m, extrapolate 1/2 step | 0.912 | 3.481 | 0.98 | 1.48 | 6.68 / 10.28 | 3.2 |
| **tau 0.5 s, no cap, snap 10 m, no extrapolation** | 0.930 | 3.442 | 1.02 | 1.30 | 6.67 / 10.17 | 2.5 |
| tau 1 s, no cap, snap 20 m, extrapolate 1/2 step | | | | | 6.74 / 10.23 | 2.3 |

- **Correction lag costs nothing.** With extrapolation off, tau from 0.3 to 0.5 s leaves CEP95 at
  3.43-3.44 m.
- **The speed cap costs the tail.** Capping corrections at 1.5 m/s holds the dot back while the filter
  moves to new radio evidence; without the cap CEP95 is 0.08 m lower.
- **Extrapolation overshoots.** Moving the dot at the last step's pace between steps removes the
  one-step-at-a-time motion. The raw estimate's along-track bias differs by scenario, though: -0.13 m on
  `office_nominal`, +0.15 m on `office_step_scale`, -0.36 m on `office_device_offset` (3 seeds each). So
  extrapolation helps some walks and hurts others, overshoots where the walker stops or turns (CEP95
  +0.04-0.07 m), and on MSIL adds jumps (3.2 vs 2.5 per minute).
- **A small snap distance brings the jumps back.** 3-4 m on simulation; real Wi-Fi corrections are often
  larger.

**Result** with the kept setting (10 seeds; before is the code before the change):

| Scenario | CEP50 | CEP95 | Jitter p95 [m] | Jumps/min | Path ratio |
|---|---:|---:|---:|---:|---:|
| office_nominal | 0.66 -> 0.66 | 1.59 -> 1.58 | 1.07 -> 0.81 | 0.2 -> 0.3 | 1.39 -> 1.19 |
| airport_hall_sparse | 1.30 -> 1.30 | 7.25 -> 7.26 | 1.25 -> 0.85 | 1.0 -> 0.7 | 1.46 -> 1.25 |
| kit_tour | 2.28 -> 2.31 | 11.05 -> 11.11 | 2.73 -> 1.81 | 4.1 -> 3.7 | 2.54 -> 2.18 |
| office_floor_bleed | 0.89 -> 0.89 | 4.08 -> 3.90 | 5.19 -> 2.33 | 4.8 -> 3.8 | 3.27 -> 2.37 |
| office_mag_disturbance | 1.09 -> 1.12 | 2.60 -> 2.65 | 1.51 -> 0.87 | 0.9 -> 0.9 | 1.72 -> 1.39 |
| office_stairs_up_down | 0.78 -> 0.79 | 3.17 -> 3.26 | 1.35 -> 0.80 | 2.1 -> 1.7 | 1.88 -> 1.57 |

The other six scenarios move by 0.03 m or less in CEP, with jitter down 20-35%. Coverage95 rises by about
1 point everywhere, because the covariance includes the dot's offset from the filter.

Real tune walks:
- MSIL: CEP50 6.37 -> 6.67 m, within the 1 m walk-to-walk noise; CEP95 10.19 -> 10.17 m; jitter p95
  1.78 -> 1.33 m; jumps 3.8 -> 2.5 per minute.
- IPIN Nuremberg: CEP50/CEP95 4.45/22.75 -> 4.42/22.00 m; jumps 8.2 -> 6.7 per minute.
- IPIN UMinho: 11.38/27.52 -> 11.03/27.48 m; jumps 9.2 -> 8.6 per minute.

**Kept.** The smoother, on by default with tau 0.5 s, no cap, snap 10 m, no extrapolation.

**Left.** Jitter p50 is still about the step length (0.67 m on `office_nominal`), because the dot moves
once per step. Extrapolation would fix that if the step timing were known better; for now it is a setting
(`smoothing.extrapolate_steps`).

## 9. Choosing the beacon-driven start automatically

**Question.** Experiment 4 left the init proposal opt-in. It fixes large venues and slows small walled
offices. Can the filter tell the two apart by itself?

**Change.** `pf.init_proposal_auto_density`: with `init_proposal` off, the proposal is used when the
cloud has fewer than this many particles per m^2 of walkable area, all levels together. The reasoning in
experiment 4 was that 1500 particles over a 40 x 24 m office (1.56 per m^2) already sample a uniform
start well. The shipped venues:
- 1.56 per m^2: the single-floor offices;
- 0.78: the two-floor offices;
- 0.47: the airport hall;
- 0.09: KIT, 7 levels;
- well below 0.5: the malls and the IPIN buildings at 1500 particles.

**Results** (10 seeds; the other scenarios are identical at both thresholds because they stay above them):

| Scenario | off | 0.6 per m^2 | 1.0 per m^2 |
|---|---:|---:|---:|
| airport_hall_sparse, CEP50 / CEP95, converged | 1.30 / 7.26, 70.5 s | **1.08 / 5.71, 47.9 s** | same as 0.6 |
| kit_tour | 2.31 / 11.11, 9/10 runs, floor 93.9% | **0.72 / 3.52, 10/10, floor 98.9%** | same as 0.6 |
| baro_drift_long | 0.63 / 1.99 | (off) | 0.61 / 1.70 |
| office_stairs_up_down | 0.79 / 3.26 | (off) | 0.71 / 2.41 |
| office_elevator | 0.60 / 1.72 | (off) | 0.74 / 2.76 |
| office_floor_bleed | 0.89 / 3.90 | (off) | 0.99 / 5.25 |

At 1.0 the two-floor offices split, as they did in experiment 4. At 0.6 only the two large venues change,
and both gain a lot. KIT gets the same numbers as `open_venue.json`.

Real tune walks with the default config (no `--config`, 1500 particles; median CEP50 / CEP95 [m]):

| Data | off | 0.6 per m^2 |
|---|---:|---:|
| MSIL, 109 walks | 20.38 / 28.45 | 9.61 / 12.50 |
| IPIN Nuremberg, 4 walks | 6.91 / 23.09 | 5.74 / 24.13 (mean CEP95 21.3 -> 30.7) |
| IPIN UMinho, 23 walks | 17.89 / 36.66 | 13.82 / 22.44 |

`msil_wifi.json` and `ipin.json` already set `init_proposal`, so the real-data gates do not change.

**Kept.** 0.6 particles per m^2 by default. With two venue types above the threshold and two below it,
this is still a coarse rule: a venue between 0.6 and 1.0 (a large two-floor office, for example) gets
the uniform start, which experiment 4 found mixed there.

## 10. Learning the loss per wall and per slab online

**Question.** The radio model takes one loss per wall and one per slab from the map.
- `office_nlos_heavy`: walls lose 7 dB, the map says 3.5.
- `office_floor_bleed`: the slab loses 4 dB, the map says 15.
- The IPIN buildings: the fitted slab loss is 2–4 dB for the whole building.

Can the filter learn the real values as it goes?

**Per-particle state (dropped).** A `wall_db`/`floor_db` per particle, with a prior around the map's value
and a random walk, like the device offset. 10 seeds, means over the 12 scenarios:

| Variant | CEP50 | CEP95 |
|---|---:|---:|
| off | 0.780 | 2.680 |
| wall loss, prior 2 dB | 0.775 | 3.425 |
| wall loss, prior 1 dB, slower walk | 0.776 | 2.868 |
| slab loss, prior 2 dB, walk 0.01 dB | 0.752 | 2.681 |

The wall loss raised CEP95 even on `office_nlos_heavy` (1.88 → 4.04 m). The slab loss fixed
`office_floor_bleed` (20 seeds: 0.94/4.08 → 0.64/1.91 m) but ruined `office_elevator` (0.62/1.91 →
0.79/4.33 m, convergence 2 → 17 s on every seed). The cause is identifiability: with a loss per particle, a
group at the wrong position with a matching loss explains the other floor's beacons as well as the right
one, so half the venue's beacons stop locating the walker.

A side lesson: the first version of the slab state drew random numbers in single-floor venues too, where
the slab is never used. That alone moved `airport_hall_sparse` CEP95 by 0.5 m and `office_device_offset` by
0.6 m. Differences of that size between 10-seed runs are seed noise, not effects.

**Venue-wide calibration.** One shared loss, calibrated from residuals at the converged estimate, the way
the integrity monitor judges beacons. Once the cloud's sigma is under 3 m, each admitted reading nudges the
loss by 0.02 x its residual per slab or wall, with residuals clipped to 15 dB.
- **Slab loss, ungated:** it settled at 11–12 dB on walks whose true slab loses 14 dB. Receivers drop
  packets below their sensitivity, so the weak other-floor readings that arrive are biased strong.
- **Gated on the prediction:** only readings predicted above -85 dBm are used, so the selection does not
  depend on the noise.
- **Gated on the prediction or the measurement:** this would let a loss that starts far too high recover.
  It did worse on the real walks (below).
- **Wall loss calibration:** unstable at every setting (mean CEP95 3.0–5.9 m). Dropped.

Slab calibration, gated on the prediction, against off (10 seeds; single-floor scenarios are
bit-identical):

| Scenario | off | calibrated |
|---|---:|---:|
| office_floor_bleed | 0.89 / 3.90, floor 96.6% | **0.68 / 2.08, floor 97.8%** |
| office_stairs_up_down | 0.79 / 3.26 | 0.77 / 3.41 |
| kit_tour | 0.72 / 3.52 | 0.68 / 3.35 |
| baro_drift_long | 0.63 / 1.99 | 0.65 / 2.86 |
| office_elevator | 0.60 / 1.72 | 0.63 / 2.29 |

The calibration helps where the map's slab loss is badly wrong and costs a little where it is about
right, so it stays off by default.

On the IPIN validation walks (`ipin.json`):

| Config | Nuremberg IPIN p75 [m] | floor hit | UMinho IPIN p75 [m] | floor hit |
|---|---:|---:|---:|---:|
| without | 18.5 | 83% | 33.8 | 70% |
| gated on the prediction | **14.9** | **89%** | **31.3** | 69% |
| gated on prediction or measurement | 16.1 | 85% | 36.2 | 70% |

**Kept.** `pf.calibrate_floor_loss`, gated on the prediction. Off by default, on in `ipin.json`.

**Limit.** A map loss far above the truth predicts every other-floor reading below -85 dBm, so nothing
corrects it. The unit test opens the gate to check the estimator itself.

## 11. A dot that glides: steps, corrections and walls apart

**Symptom.** After experiment 8, jitter p50 still equals the step length (0.64 m, sim mean) because the
dot moves once per step. Real walks jump 2.5 (MSIL) to 6–7 (IPIN) times a minute, and UMinho's jitter p95
is 3.1 m.

**Diagnosis.** A scratch probe attributed each 5 Hz second difference, each window over 3 m/s, and each
change of the raw estimate to the event that caused it.

- **Steps.** In simulation, step events cause 70–96% of the top-5% jitter ticks.
- **The glide was not a glide on Wi-Fi.** `correct()` applied `1 - exp(-dt/tau)` of the *new* error at
  once, where `dt` was the time since the previous update. With BLE packets 40 ms apart that is a
  low-pass. A Wi-Fi scan arrives about 0.5 s after the last step, so 63% of its correction was drawn
  instantly and most of the rest at the next step. Scans move the raw estimate by a lot: p95 7.8 m (MSIL)
  and 9.4 m (IPIN), p99 25 m.
- **Step events carried corrections.** The dot moved by the estimate's change across a step. That change
  includes other things than the step:
  - particles dying at walls and resampling: more than 0.5 m on 18% of IPIN steps;
  - the magnetometer update that follows: more than 0.5 m on 11% (MSIL) and 18% (IPIN) of steps.

  With smoothing off, 7.3% of MSIL step events move the estimate more than 1.5 m, against a step of about
  0.65 m. This also polluted the pace that `extrapolate_steps` used, which is why experiment 8 found it
  added jumps on MSIL. `map_consistent_estimate` is not the cause (6.3% with it off).

**Change** (`core/src/track_smoother.cpp`, `ParticleFilter::step_motion()`):
- **Steps.** A step moves the dot by its own motion: the weighted mean move of the particles that survived
  it. It is accumulated in the existing loop with no draws, so the filter is bit-identical (10 seeds,
  smoothing off, every metric). The rest of the step's change is a correction.
- **Corrections.** They glide in continuous time: after `dt`, `exp(-dt/tau)` of the gap is left, and
  `max_correction_mps` caps the speed in closed form. The track no longer depends on the update rate (unit
  test `TheGlideDoesNotDependOnTheUpdateRate`).
- **Walls.** An estimate behind a wall is glided towards only as far as the wall. If it is still behind
  the wall when the dot gets there, the dot jumps. The first version jumped at once, which more than
  doubled the jumps on `office_floor_bleed` (jitter p95 2.33 → 3.46 m): the estimate of a cloud split by a
  wall flips sides and comes back.
- **Between steps.** The dot moves at the last step's pace, centred on the estimate (half a step behind it
  at a step, half a step ahead at the next), and goes back once no step has come for 1.2 s.

**Why centred.** Along-track bias of the drawn point, mean over 3 seeds, in metres:

| Scenario | No extrapolation | Ahead of the estimate (experiment 8) | Centred |
|---|---:|---:|---:|
| office_nominal | +0.01 | +0.34 | +0.00 |
| office_step_scale | +0.08 | +0.39 | +0.07 |
| office_field_issue | +0.05 | +0.37 | +0.04 |
| office_device_offset | −0.24 | +0.06 | −0.25 |

Radio holds the estimate on the walker's mean position over a step interval, so running ahead of it by the
time since the step draws the dot half a step too far. That is the overshoot experiment 8 saw. Centred, the
mean error is lower than with no extrapolation in all four scenarios (0.670 → 0.650 m on
`office_nominal`).

**Steps of the change.** Sim: means over 12 scenarios, 10 seeds. Real: tune walks with `msil_wifi.json`,
median CEP over walks.

| Variant | Sim CEP50 / CEP95 | Sim jitter p50 / p95 | Sim jumps/min | MSIL CEP50 / CEP95 | MSIL jitter p95 | MSIL jumps | Nuremberg jumps | UMinho jitter p95 | UMinho jumps |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| before | 0.780 / 2.680 | 0.64 / 0.94 | 1.09 | 6.67 / 10.17 | 1.33 | 2.54 | 5.81 | 3.09 | 6.62 |
| step motion, continuous glide, jump at walls | 0.787 / 2.670 | 0.65 / 1.01 | 1.12 | 6.51 / 10.06 | 0.92 | 2.43 | 5.43 | 2.01 | 6.38 |
| + glide up to the wall | 0.786 / 2.673 | 0.65 / 0.87 | 1.06 | 6.51 / 10.06 | 0.92 | 1.86 | 5.42 | 1.41 | 6.21 |
| + extrapolation ahead of the estimate | 0.782 / 2.713 | 0.05 / 0.74 | 1.06 | 6.68 / 10.19 | 0.62 | 1.82 | 5.54 | 1.35 | 6.32 |
| **+ centred (engine default)** | **0.769 / 2.659** | **0.04 / 0.50** | **1.04** | 6.58 / 10.06 | 0.47 | 1.65 | 5.48 | 1.23 | 6.10 |
| **+ cap 1.5 m/s (Wi-Fi configs)** | 0.770 / 2.728 | 0.04 / 0.41 | 0.80 | **6.63 / 10.16** | **0.33** | **0.00** | **4.02** | **0.90** | **4.92** |

**Settings.** All with centred extrapolation over one step; same sets as above.

| tau [s] | cap [m/s] | Sim CEP50 / CEP95 | Sim jitter p95 | Sim jumps | MSIL CEP95 | MSIL jumps | Nuremberg CEP95 / jumps | UMinho CEP95 / jumps |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.5 | - | 0.769 / 2.659 | 0.50 | 1.04 | 10.06 | 1.65 | 16.32 / 5.48 | 25.73 / 6.10 |
| 0.5 | 1.5 | 0.770 / 2.728 | 0.41 | 0.80 | 10.16 | 0.00 | 16.63 / 4.02 | 24.47 / 4.92 |
| 0.5 | 2 | 0.769 / 2.691 | 0.42 | 0.85 | 10.17 | 0.88 | 16.53 / 3.85 | 24.73 / 5.38 |
| 0.5 | 3 | 0.768 / 2.666 | 0.44 | 0.91 | 10.18 | 1.65 | 16.39 / 4.68 | 25.83 / 6.12 |
| 1 | - | 0.775 / 2.667 | 0.45 | 0.92 | 10.11 | 1.54 | 16.54 / 5.01 | 25.46 / 5.50 |
| 1 | 1.5 | 0.777 / 2.758 | 0.41 | 0.80 | 10.11 | 0.00 | 16.65 / 4.07 | 24.39 / 4.92 |
| 1 | 2 | 0.776 / 2.726 | 0.42 | 0.81 | 10.11 | 0.81 | 16.60 / 4.01 | 24.50 / 5.08 |

- **On BLE the cap costs the tail.** At 1.5 m/s, `office_elevator` CEP95 goes 1.72 → 2.01 m and
  `office_floor_bleed` 3.90 → 4.30 m, while simulated jumps are already near one a minute. So it is off in
  the engine.
- **On Wi-Fi the cap is what removes jumps.** A walker plus a 1.5 m/s correction stays under the 3 m/s
  jump threshold. What is left on IPIN (4–5 a minute) are fast walkers with a correction along the path,
  and floor flips; only 53 of 1,483 jump windows contained a jump of the dot itself.
- **tau** 1 s instead of 0.5 s changes little once extrapolation and the cap are in.

**Result, engine default** (10 seeds; before → after):

| Scenario | CEP50 | CEP95 | Jitter p50 [m] | Jitter p95 [m] | Jumps/min | Floor flips | Path ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| airport_hall_sparse | 1.08 → 1.06 | 5.71 → 5.67 | 0.64 → 0.03 | 0.83 → 0.29 | 0.5 → 0.6 | - | 1.23 → 1.15 |
| baro_drift_long | 0.63 → 0.62 | 1.99 → 1.95 | 0.64 → 0.04 | 0.77 → 0.39 | 1.3 → 1.3 | 4.4 → 4.4 | 1.49 → 1.38 |
| kit_tour | 0.72 → 0.71 | 3.52 → 3.53 | 0.63 → 0.04 | 0.77 → 0.34 | 1.4 → 1.3 | 3.0 → 3.0 | 1.38 → 1.27 |
| office_device_offset | 0.76 → 0.74 | 2.08 → 2.03 | 0.61 → 0.04 | 0.79 → 0.36 | 0.4 → 0.4 | - | 1.17 → 1.09 |
| office_elevator | 0.60 → 0.58 | 1.72 → 1.70 | 0.65 → 0.04 | 0.79 → 0.40 | 1.5 → 1.7 | 2.0 → 2.0 | 1.71 → 1.61 |
| office_field_issue | 0.71 → 0.70 | 1.98 → 1.95 | 0.69 → 0.04 | 0.87 → 0.44 | 0.6 → 0.4 | - | 1.31 → 1.18 |
| office_floor_bleed | 0.89 → 0.88 | 3.90 → 3.87 | 0.64 → 0.07 | 2.33 → 1.79 | 3.8 → 3.6 | 3.8 → 3.8 | 2.37 → 2.17 |
| office_mag_disturbance | 1.12 → 1.14 | 2.65 → 2.71 | 0.65 → 0.05 | 0.87 → 0.49 | 0.9 → 0.8 | - | 1.39 → 1.26 |
| office_nlos_heavy | 0.65 → 0.64 | 1.88 → 1.83 | 0.67 → 0.04 | 0.82 → 0.35 | 0.5 → 0.4 | - | 1.25 → 1.15 |
| office_nominal | 0.66 → 0.64 | 1.58 → 1.55 | 0.67 → 0.04 | 0.81 → 0.35 | 0.3 → 0.1 | - | 1.19 → 1.08 |
| office_stairs_up_down | 0.79 → 0.78 | 3.26 → 3.24 | 0.62 → 0.04 | 0.80 → 0.46 | 1.7 → 1.9 | 2.0 → 2.0 | 1.57 → 1.42 |
| office_step_scale | 0.74 → 0.73 | 1.88 → 1.88 | 0.62 → 0.04 | 0.79 → 0.34 | 0.1 → 0.0 | - | 1.28 → 1.14 |
| **mean** | 0.780 → 0.769 | 2.680 → 2.659 | 0.64 → 0.04 | 0.94 → 0.50 | 1.09 → 1.04 | | 1.44 → 1.33 |

Floor flips are unchanged: the level is the filter's.

**Result, real tune walks.** Median over walks; one replay each, or 5 seeds for `evaluate.py`.

| Data | CEP50 / CEP95 [m] | Jitter p50 / p95 [m] | Jumps/min |
|---|---:|---:|---:|
| MSIL, 109 walks, `msil_wifi.json` | 6.67 / 10.17 → 6.63 / 10.16 | 0.66 / 1.33 → 0.06 / 0.33 | 2.54 → 0.00 |
| IPIN Nuremberg, `msil_wifi.json` | 4.53 / 16.23 → 4.61 / 16.63 | 0.45 / 1.54 → 0.07 / 0.56 | 5.81 → 4.02 |
| IPIN UMinho, `msil_wifi.json` | 9.90 / 25.92 → 10.08 / 24.47 | 0.48 / 3.09 → 0.08 / 0.90 | 6.62 → 4.92 |
| IPIN Nuremberg, `ipin.json` | 5.26 / 19.78 → 5.18 / 19.45 | → 0.54 (from 1.76) | 6.3 → 3.6 |
| IPIN UMinho, `ipin.json` | 10.78 / 27.66 → 10.90 / 27.80 | → 1.85 (from 3.99) | 8.1 → 5.8 |
| UJI BLE, 178 sessions, default | 1.74 / 1.86 → 1.72 / 1.91 | p95 0.28 → 0.06 | - |

With `ipin.json`, `datasets/ipin/evaluate.py --seeds 5`:
- Nuremberg: CEP50 / CEP95 6.71 / 29.03 → 6.57 / 29.18 m, floor hit 90% → 90%, IPIN p75 15.03 → 14.50 m.
- UMinho: 11.55 / 38.01 → 11.35 / 38.16 m, floor hit 66% → 66%, IPIN p75 32.49 → 32.49 m.

**Costs.**
- `office_mag_disturbance`: CEP95 2.65 → 2.71 m.
- UJI (standing still, BLE scans 1 s apart):
  - CEP95 rises 2.7%;
  - fewer of the 6–13 s sessions stay under 3 m for 10 s (67 → 40 of 178).

  The truth rows sit exactly at the scans. The old smoother drew 63% of each scan's correction at that
  instant; the glide draws none of it there. This is the price of never drawing a correction at once.
- Nuremberg CEP95 with `msil_wifi.json`: 16.23 → 16.63 m (+2.5%), against 31% fewer jumps.
- `tests/regression/baseline.json` passes unchanged (3 seeds), so it was not regenerated.

**Kept.**
- Engine defaults: centred extrapolation over one step, tau 0.5 s, no cap.
- `msil_wifi.json` and `ipin.json` add `smoothing.max_correction_mps` 1.5.

## 12. Discounting repeated packets of one beacon (not kept)

**Observation.** A wider BLE likelihood improves every simulated mean at once (10 seeds, 12 scenarios, on
the experiment 11 smoother):

| `rssi.sigma_db` | CEP50 | CEP95 | Jitter p95 | Jumps/min |
|---:|---:|---:|---:|---:|
| 5 (default) | 0.769 | 2.659 | 0.50 | 1.04 |
| 6 | 0.754 | 2.671 | 0.37 | 0.83 |
| 7 | 0.723 | 2.605 | 0.35 | 0.72 |

Real BLE does not prefer it. UJI standing still gives median CEP50 1.74, 1.73 and 1.74 m at 5, 7 and 8 dB
(before experiment 11), and its measured spread per packet is 4.5–6.2 dB.

**Hypothesis.** The filter counts time-correlated packets as independent. The simulator draws 4 dB AR(1)
shadowing per beacon with a 4 s time constant and a packet every 0.5 s, so consecutive packets are far
from independent. A plain wider sigma then helps by accident, and a discount on repeats should do better.

**Change tried.** `pf.rssi_repeat_s` scaled each reading's log-likelihood by the time since the same
beacon's previous reading over `rssi_repeat_s`, kept between `rssi_repeat_min_weight` and 1. The first
reading after a silence counted in full. It cost O(1) per reading, needed no draws, and was bit-identical
at 0.

| `rssi_repeat_s` / min weight | CEP50 | CEP95 | Jitter p95 | Jumps/min |
|---|---:|---:|---:|---:|
| off | 0.769 | 2.659 | 0.50 | 1.04 |
| 0.5 s / 0.25 or 0.5 | 0.755 | 2.807 | 0.52 | 1.13 |
| 1 s / 0.25 | 0.788 | 2.808 | 0.41 | 0.91 |
| 1 s / 0.5 | 0.759 | 2.895 | 0.40 | 0.95 |
| 2 s / 0.25 | 0.787 | 3.164 | 0.37 | 0.82 |
| 2 s / 0.5 | 0.756 | 2.996 | 0.38 | 0.88 |

At 0.5 s the two minimum weights give the same numbers: packets come 0.35–0.65 s apart, so the weight
never falls below 0.7.

**Result.** It is worse than a plain wider sigma on every mean, and CEP95 is worse than doing nothing.
Neither passes the bar set for a default:
- no scenario's CEP95 worse by more than 10%;
- two-floor convergence at most 2 s slower.

Both trade the same scenarios (CEP95 m, convergence s):

| Scenario | off | sigma 7 | repeat 1 s / 0.5 |
|---|---:|---:|---:|
| office_stairs_up_down | 3.24, 11 s | 4.26, 24 s | 4.06, 23 s |
| baro_drift_long | 1.95, 13 s | 2.94, 24 s | 3.15, 23 s |
| office_mag_disturbance | 2.71 | 3.27 | 2.86 |
| office_floor_bleed | 3.87 | **1.69** | 4.33 |
| office_nominal | 1.55 | **1.21** | 1.31 |

A weaker radio helps most offices, but the two-floor walks take twice as long to lock on, and the
disturbed-magnetometer walk loses its anchor. The repeat discount keeps the costs without most of the gains.

**Not kept.** The code was removed. Wi-Fi scans (each access point 2–3 s apart) and the UJI scans (1 s)
would not have been touched at `rssi_repeat_s` ≤ 1 s anyway. The engine keeps 5 dB for BLE, the measured
per-packet spread. Why the two-floor walks need the sharper radio to lock on is open.

## 13. A dot inside a room without a door

**Symptom.** In the web demo, on "site1 F1, 98 s walk", the dot jumps into shops that have no door and
back out. The demo maps come from `convert.py --walls doors`. Every shop outline is a wall, with a doorway
only where a training walk crossed it, so every shop that no training walk entered is closed. At seed 1, the
seed the demo uses, the dot enters three such shops six times: 11 moves through a wall, 7.1 a minute.
Jitter p95 is 11.1 m and jumps are 9.8 a minute.

**Cause.** No particle walks into a closed room: a step that crosses a wall kills the particle, and
roughening and the barometer jump stay on the map. Three places put particles there anyway, because to them
the venue is everything inside the outline (`Venue::inside`):

| Path | On site1 F1, seed 1 |
|---|---|
| The uniform start and the beacon-ring seed | t = 4.9 s |
| Recovery injections: rings around a packet from the last 2 s, up to 5% per resample | t = 7.7, 31.1, 42.1, 45.5 and 49.1 s |
| The re-seed when every particle has died: 3 m around the estimate, through walls | t = 87.9 s, with recovery off |

Access points often sit inside shops, so their rings do too. Once a few particles are inside, one Wi-Fi
scan moves 85–95% of the weight onto them, because the scan's readings count as independent (see
[real_data.md](real_data.md#failure-modes-and-open-questions)). The particles die at the shop's walls a few
seconds later, and the dot jumps back out. The raw estimate goes into the shop too: the smoother only glides
to the wall and jumps there (experiment 11).

Labelling each level's connected regions shows how common such rooms are. A 0.25 m grid is enough, with
neighbouring cells joined unless a wall lies between their centres.

| Map | Enclosed regions under 10% of the level's largest |
|---|---|
| Simulated offices | none: one region per level |
| `airport_hall` | 180 m^2: three solid 10 x 6 m check-in islands |
| KIT (OpenStreetMap) | 1–24% of each level: rooms whose door is not mapped |
| IPIN Nuremberg, UMinho | 1–6% |
| MSIL demo floors | 20–26%: the shops without an inferred door |
| MSIL outline-only floors | 135–173 m^2 corners of the bounds outside the building |
| UJI library | shelf slots, all under 10 m^2 |

**Change.** `ReachableArea` labels the regions once per venue. A region is closed (`pf.closed_region_*`) if:
- it is at least 10 m^2 (`closed_region_min_m2`);
- it is smaller than a tenth of its level's largest region (`closed_region_fraction`);
- it holds no stairwell or lift.

No particle is placed in a closed region:
- **Uniform draws.** The uniform start and uniform recovery reject closed positions, as does the ring's
  fallback. Levels are drawn by their area that is not closed.
- **Ring draws.** A ring draw in a closed region is drawn again, up to 16 times, without using up one of the
  4 tries. Draws that leave the venue therefore fall back to the uniform prior exactly as often as before.
  The seed's importance weights divide the ring density by the share of all draws accepted rather than by
  the share of tries; the two are the same without closed regions. A recovery particle that a level jump
  puts in a closed region is drawn uniformly instead.
- **The re-seed.** It is drawn again around the estimate until it lies outside closed regions. It still
  crosses walls, because a door missing from the map is the usual reason every particle died.

Smaller pockets are left alone. Closing UJI's shelf slots moved its median CEP50 by 5% for no physical
gain. A venue without a closed region builds no map and draws exactly as before. That covers the 10
single-floor and two-floor office scenarios and UJI: bit-identical, checked to the last digit.
`wall_crossings_per_min` is new ([formats.md](formats.md#replay-outputs)): it counts moves of the drawn
dot through a wall.

**Results.** Shop-wall maps first: the problem and the holdout walks the demo plays. Means over filter seeds
(`pf.seed`). CEP50 / CEP95 [m], with jitter p95 [m], jumps and moves through a wall per minute. For
recorded walks the figures are medians over walks.

| Data | Before | After |
|---|---|---|
| `out_doors` tune, 111 walks, 3 seeds | 8.21 / 12.67, 0.58, 2.10, 3.28 | 8.31 / 13.08, 0.57, 2.18, 1.83 |
| Demo walks (held out), 10 walks x 6 seeds | 14.17 / 21.83, 0.64, 2.36, 3.80 | 9.09 / 16.48, 0.55, 2.31, 2.63 |
| site1 F1, 6 seeds | 7.16 / 20.62, 6.00, 8.48, 5.71 | 8.67 / 30.13, 5.36, 7.17, 3.23 |
| site1 F1, seed 1 (the demo) | 7.6 / 16.7, 11.09, 9.8, 7.76 | 9.7 / 32.1, 4.03, 7.2, 2.59 |

What changes on site1 F1 over the 6 seeds:
- The dot is in a doorless shop at some point in every seed before, and in none after.
- Moves between regions fall from 46 to 15.
- The share of time in a region the walker is not in falls from 17% to 6%.

What is left is the cloud moving across the corridor wall into the 15,510 m^2 region next to the walker's.
Those stays last longer than a shop visit did, since nothing kills the particles there. With only 11 scored
waypoints, that is what moves this walk's CEP95. Over all 10 demo walks the shop walls stop trapping the
cloud, and the walks [real_data.md](real_data.md#in-the-web-demo) lists as hurt by shop walls recover:

| Walk (shop walls, 6 seeds) | Before | After |
|---|---|---|
| site1 F4 | 16.8 / 37.8 | 6.2 / 15.0 |
| site2 F6 | 17.1 / 36.2 | 8.1 / 12.5 |
| site2 F8 | 21.8 / 34.2 | 11.6 / 16.1 |
| site1 F2 | 11.5 / 17.6 | 3.9 / 8.6 |

Everything else: 10 seeds for the scenarios, 80 runs (8 filter seeds x 10) where one scenario changes.

| Data | Before | After |
|---|---|---|
| 10 office scenarios | | bit-identical |
| `airport_hall_sparse`, 80 runs | 1.30 / 5.92, 0.33, 0.80, 0.34; converged in 53.6 s | 1.23 / 5.82, 0.31, 0.70, 0.22; 51.4 s |
| `kit_tour`, 80 runs | 0.69 / 3.44, 0.32, 1.16, 0.91; 8.2 s; floor 98.9% | 0.68 / 3.70, 0.31, 1.15, 0.78; 7.2 s; 98.8% |
| MSIL outline-only tune, 109 walks | 6.63 / 10.16, 0.33, 0.0, 0.00 | 6.74 / 9.66, 0.33, 0.0, 0.00 |
| UJI tune | 1.72 / 1.91 | bit-identical |
| IPIN tune, `evaluate.py --seeds 16`: CEP, floor hit, IPIN p75 | Nuremberg 7.23 / 28.15, 91%, 13.24; UMinho 11.97 / 40.20, 67%, 33.05 | 7.31 / 26.21, 91%, 14.60; 12.88 / 38.56, 69%, 35.09 |

In the standard 10-seed run the scenario means move from 0.769 / 2.659 to 0.784 / 2.937. Almost all of
that is one seed set of `kit_tour` with a CEP95 of 6.58; over 80 runs its CEP95 spread is 2.5–4.5 m with
or without the change. The closed corners of the MSIL outline-only floors lie outside the building, so
those walks only see a different random path.

On IPIN, CEP95 improves by 4–7% and the official p75 worsens by 6–10%. Comparing 8 against 16 seeds of the
code before the change moves p75 by 3–8%, so the cost is about the size of the noise, but it is in the same
direction at both seed counts. The tune walks never stand in a closed region. The straight lines between
their reference points cross closed regions on 1.3% of their length, which is corners cut by the
interpolation. Two UMinho holdout walks do have a reference point inside a closed region: a room the map
has no door to, which the filter now cannot follow anyone into.

Held-out halves, replayed for reporting only:
- **IPIN holdout** (`evaluate.py --seeds 16`): Nuremberg goes from 8.28 / 38.13 m, p75 27.16 m to
  8.65 / 42.78 m, p75 28.02 m. UMinho goes from 15.29 / 42.97 m, p75 33.68 m to 15.72 / 43.92 m,
  p75 34.08 m. Floor hit is unchanged.
- **MSIL outline-only holdout** (3 seeds): 11.31 / 15.73 m to 11.53 / 15.84 m. site1 F4 and every site2
  floor have no closed region and are bit-identical. The four site1 floors, each with a closed corner
  outside the building, move within their seed spread.

`compute_us_per_update` (`--serial`, 2 seeds) goes from 70.5 to 70.6 µs. Labelling the regions costs
0.07–0.18 s once per venue on the largest floors (MSIL B1, 320 x 232 m).

**Tried, not kept.** All measured on the same sets.
- **Skipping a recovery injection whose ring lies entirely in closed regions**, instead of falling back
  to a uniform draw. `out_doors` 8.36 / 13.00, 0.55, 2.05, 1.46; IPIN p75 14.55 and 34.87. Close to the
  kept version everywhere, but site1 F1 gets worse: 11.35 / 28.87 over 6 seeds, and jitter p95 11.65 in the
  demo seed. The uniform fallback is what lets the cloud recover there.
- **Moving such a draw to the nearest open ground**, the shop front, since an access point in a closed
  shop was heard from outside it. IPIN p75 15.07 and 35.77, and 2.32 jumps a minute on `out_doors`.
- **Injecting only into regions holding at least 1% of the weight**, unless the filter looks lost. On the
  prototype, 20–30% fewer moves through walls than the kept version, but CEP95 3% worse on `out_doors`
  and site1 F1's CEP50 8.6 to 11.6 m. It blocks the escape from a wrong corridor that recovery exists for:
  with recovery off, site1 F2 spends 96% of its time in a wrong region instead of 0.5%.
- **Counting every closed draw as a try with 16 tries**, instead of redrawing it for free. IPIN got worse:
  it also changes how often rings that leave the venue fall back.
- **Accepting an injection only if the cloud's last 8–25 m of step motion, walked back from it, crosses no
  wall.** It misses shops early in a walk, when there is little motion to check, and lets the cloud into
  large wrong regions.

**Kept.** `closed_region_fraction` 0.1 and `closed_region_min_m2` 10, on by default. The baselines
were updated only where this change moves the results:
- **`baseline.json`:** `airport_hall_sparse` and `kit_tour`. The airport's 3-seed CEP crossed the 10%
  tolerance: 1.09 / 4.69 m to 1.40 / 5.85 m. Over 80 runs it is better.
- **`msil_baseline.json`:** the four site1 floors and `all`. site1 F1's CEP95 (7.94 to 9.56 m) and site1
  F3's CEP50 (5.54 to 6.93 m) crossed it, a different random path. site1 F2 improved past it.
- **`ipin_baseline.json`:** all groups. Nuremberg's CEP50 crossed it (5.23 to 6.54 m) while its CEP95 fell
  from 33.04 to 20.72 m. UMinho's two walks pass through a room the map closes: 10.52 / 39.53 m to
  13.65 / 44.18 m.

## 14. A grid Bayes filter instead of particles (prototype)

**Question.** Much of the particle filter's machinery is there because a sample covers the posterior
sparsely:
- recovery injections, and the hijack trade-off they bring (experiment 2);
- the beacon-ring start, and the density rule that switches it on (experiments 4 and 9);
- rejection sampling around closed rooms (experiment 13);
- seed noise in every comparison.

What do the same sensor models do as an exact posterior on a grid of map cells?

**Change.** `grid/` and `ipe-grid`, a standalone prototype ([grid.md](grid.md)).
- The state lives on a 0.5 m lattice of walkable cells. Each cell carries its mass, the mass centroid,
  the device RSSI offset and the barometer offset as Gaussians, and a distribution over 36 heading bins.
- Heading bins are offsets from the gyro yaw, so turns need no interpolation.
- Walls are one bit per lattice offset.
- The filter uses no random numbers.
- `GridEngine` runs it through the engine's PDR, `TrackSmoother`, `LevelSelector`,
  `MapConsistentEstimate`, `BeaconIntegrityMonitor` and `RoomTracker`.

The engine, the web demo and the baselines are untouched. Three hooks let `ipe-grid compare` replay the
same traces through both filters and score them with the same code:
- `TracePlayer::step` is a template over the engine;
- `eval::replay_with` holds the replay loop;
- `BenchmarkOptions::estimators` adds extra estimators to a benchmark.

The particle filter is unchanged by them: `ipe-regress --seeds 10` gives identical results, and the
per-sample estimates, track, room events and particle snapshots of `kit_tour`, `office_floor_bleed` and
`office_field_issue` are byte-identical.

**Settings.** All chosen on 10-seed runs of the 12 scenarios (means below); recovery was then checked on
the tune walks. "rel" is the along-track step noise relative to the step length (`step_length_rel_sigma`).
"Recovery" is the uniform mass mixed in per second (`teleport_rate_per_s`); rows without a rate use 1e-3.

| Setting | CEP50 | CEP95 | Coverage95 | Jitter p95 | Jumps/min | field_issue CEP95 | step_scale CEP50 |
|---|---:|---:|---:|---:|---:|---:|---:|
| PF (engine default) | 0.784 | 2.937 | 82% | 0.50 | 1.06 | 1.95 | 0.73 |
| rel 0.1, recovery 1e-3 (first version) | 0.741 | 2.167 | 61% | 0.32 | 0.67 | 3.64 | 0.92 |
| + 72 heading bins | 0.753 | 2.207 | 60% | 0.32 | 0.67 | 3.67 | 0.92 |
| + step scale state (0.85, 1, 1.15) | 0.683 | 2.218 | 75% | 0.33 | 0.63 | 3.97 | 0.49 |
| rel 0.05 | 0.775 | 2.282 | 56% | 0.33 | 0.72 | 4.34 | 0.93 |
| rel 0.15 | 0.688 | 2.090 | 70% | 0.32 | 0.64 | 3.59 | 0.87 |
| rel 0.2 | 0.658 | 2.026 | 78% | 0.31 | 0.61 | 3.65 | 0.78 |
| rel 0.15, augmented-MCL trigger, no constant rate | 0.699 | 2.271 | 74% | 0.39 | 0.96 | 4.13 | 0.83 |
| rel 0.2, recovery 1e-4 | 0.655 | 1.898 | 76% | 0.30 | 0.52 | 2.42 | 0.78 |
| rel 0.2, recovery 1e-4, active floor 1e-6 | 0.658 | 1.885 | 75% | 0.30 | 0.51 | 2.04 | 0.79 |
| rel 0.2, recovery 1e-4, step scale state | 0.673 | 1.967 | 79% | 0.31 | 0.50 | 2.27 | 0.49 |
| rel 0.25, recovery 1e-4 | 0.647 | 1.893 | 79% | 0.30 | 0.50 | 2.45 | 0.71 |
| rel 0.2, recovery 1e-5 | 0.653 | 1.853 | 75% | 0.29 | 0.48 | 1.95 | 0.79 |
| rel 0.2, recovery 1e-6 | 0.655 | 1.832 | 74% | 0.29 | 0.44 | 1.71 | 0.78 |
| **rel 0.2, recovery 1e-8** | 0.656 | 1.827 | 74% | 0.28 | 0.41 | 1.68 | 0.78 |
| rel 0.2, recovery 1e-12 | 0.655 | 1.836 | 74% | 0.28 | 0.40 | 1.67 | 0.78 |
| rel 0.2, no recovery | 0.656 | 1.831 | 74% | 0.28 | 0.39 | 1.67 | 0.78 |

- **Recovery is the setting that matters.** Recovery mass is in every cell at once. When the moved beacon
  S3 reads 25 dB hot in N3 (experiment 2), the mass already sitting at S3's mapped position wins within
  a few packets, before the integrity monitor can exclude S3: 15 m for 10 s on one seed checked. A lower
  constant rate only delays it; at 1e-6 and below the walk is better than the PF's.
- **Augmented MCL's trigger is the wrong signal.** It injects exactly when S3 misbehaves, and is worse
  than any constant rate (4.13 m).
- **On real Wi-Fi recovery mass costs more still.** On the IPIN tune walks, mean IPIN p75 is 26.6 m with
  no recovery, 27.0 at 1e-8, 27.2 at 1e-6 and 28.5 at 1e-4 (the PF: 38.1). Medians swing more from walk to
  walk: Nuremberg p75 is 10.9, 10.2, 15.0 and 14.1 m. One scan's readings count as independent, so a scan
  can pull the posterior into any region that holds mass. 1e-8 is kept: no measurable cost, and a way
  back from a wrong lock.
- **Wider along-track noise beats a step-scale state.** Three scale values fix `office_step_scale`
  (CEP50 0.49 m) but cost most other offices 0.1–0.35 m of CEP95, and triple the work. rel 0.2 stands in
  for the scale the particle filter carries.
- **Neutral.**
  - 72 heading bins.
  - The active floor, which leaves cells under 1e-6 of the heaviest in place at a step. On the traces
    checked, 1e-9 equals the exact filter to 4 digits but saves nothing on Wi-Fi, where most cells stay
    above it. It stays off.

**Result, simulated** (10 seeds, PF → grid with the defaults above; the PF numbers are the engine
default):

| Scenario | CEP50 | CEP95 | Converged [s] | Coverage95 | Jitter p95 [m] | Jumps/min | Through walls/min | Floor ok / flips |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| airport_hall_sparse | 1.21 → 1.01 | 5.95 → 3.76 | 35.9 → 34.5 | 72% → 68% | 0.29 → 0.20 | 0.7 → 0.2 | 0.31 → 0.05 | - |
| baro_drift_long | 0.62 → 0.54 | 1.95 → 1.42 | 13.3 → 1.0 | 87% → 78% | 0.39 → 0.28 | 1.3 → 0.6 | 1.15 → 0.17 | 99.1% / 4.4 → 99.2% / 4.2 |
| kit_tour | 0.74 → 0.66 | 6.58 → 2.67 | 8.0 → 0.7 | 76% → 65% | 0.35 → 0.26 | 1.4 → 0.9 | 1.13 → 1.19 | 98.9% / 3.2 → 99.0% / 3.0 |
| office_device_offset | 0.74 → 0.49 | 2.03 → 1.15 | 5.7 → 5.2 | 83% → 92% | 0.36 → 0.31 | 0.4 → 0.1 | 1.22 → 0.73 | - |
| office_elevator | 0.58 → 0.54 | 1.70 → 1.28 | 1.3 → 2.5 | 91% → 76% | 0.40 → 0.22 | 1.7 → 0.9 | 1.24 → 0.12 | 99.0% / 2.0 → 99.0% / 2.0 |
| office_field_issue | 0.70 → 0.56 | 1.95 → 1.68 | 2.5 → 5.7 | 86% → 83% | 0.44 → 0.30 | 0.4 → 0.1 | 1.13 → 0.49 | - |
| office_floor_bleed | 0.88 → 0.68 | 3.87 → 1.50 | 8.0 → 1.8 | 80% → 66% | 1.79 → 0.32 | 3.6 → 1.0 | 5.51 → 0.52 | 96.6% / 3.8 → 97.9% / 2.9 |
| office_mag_disturbance | 1.14 → 0.94 | 2.71 → 2.65 | 3.8 → 6.1 | 64% → 53% | 0.49 → 0.37 | 0.8 → 0.3 | 2.80 → 2.60 | - |
| office_nlos_heavy | 0.64 → 0.57 | 1.83 → 1.28 | 2.7 → 3.8 | 91% → 84% | 0.35 → 0.28 | 0.4 → 0.0 | 0.94 → 0.34 | - |
| office_nominal | 0.64 → 0.55 | 1.55 → 1.14 | 1.7 → 2.2 | 88% → 85% | 0.35 → 0.29 | 0.1 → 0.0 | 0.64 → 0.39 | - |
| office_stairs_up_down | 0.78 → 0.58 | 3.24 → 1.62 | 11.1 → 1.2 | 79% → 74% | 0.46 → 0.30 | 1.9 → 0.8 | 2.01 → 0.08 | 98.9% / 2.0 → 99.1% / 2.0 |
| office_step_scale | 0.73 → 0.78 | 1.88 → 1.77 | 2.1 → 3.3 | 82% → 62% | 0.34 → 0.28 | 0.0 → 0.0 | 1.00 → 0.78 | - |
| **mean** | 0.784 → 0.656 | 2.937 → 1.827 | 8.0 → 5.7 | 82% → 74% | 0.50 → 0.28 | 1.06 → 0.41 | 1.59 → 0.62 | |

The PF's `kit_tour` CEP95 here is dominated by one seed set (experiment 13: 2.5–4.5 m over 80 runs), so
that row overstates the gap. Every run of both filters converges. Path ratio falls from 1.08–2.17 to
1.01–1.13.

**Result, real tune walks** (settings were chosen on these and on the scenarios; median over walks).
CEP counts only fixes on the right floor, so IPIN also gets the floor hit and the competition's p75
(`datasets/ipin/evaluate.py`; the PF with 5 filter seeds, the grid once, since it has none):

| Data | PF | Grid |
|---|---|---|
| MSIL, 109 walks, `msil_wifi.json`: CEP50 / CEP95 (mean CEP95) | 6.74 / 9.66 (15.48) | 6.46 / 9.25 (16.24) |
| IPIN Nuremberg, 4 walks, `msil_wifi.json`: CEP50 / CEP95, floor hit, p75 | 7.84 / 19.92, 73%, 27.32 | 5.90 / 19.80, 91%, 10.20 |
| IPIN UMinho, 23 walks, same | 12.49 / 39.43, 41%, 38.43 | 8.75 / 27.22, 40%, 32.36 |
| UJI BLE, 270 sessions, default config, no warm-up: CEP50 / CEP95 | 2.04 / 3.33 | 1.88 / 3.40 |

- **The PF with its opt-in floor model** (`ipin.json`: floor prior, barometer jump, slab-loss
  calibration) reaches 91% / 64% floor hit and p75 15.12 / 36.91 m. The grid has none of these and still
  has the lower p75, but UMinho's floor stays where the radio puts it (about 40%).
- **Smoothness on real walks** (`ipe-grid compare`, median over walks):
  - MSIL: jitter p95 0.33 → 0.35 m, no jumps for either (the Wi-Fi config caps corrections).
  - IPIN Nuremberg: 0.58 → 0.59 m, jumps 4.0 → 2.3 a minute.
  - IPIN UMinho: 0.91 → 1.25 m, jumps 4.8 → 5.5 a minute. The grid's track is less smooth there.

**Held out** (replayed once with the settings above, for reporting only):

| Data | PF | Grid |
|---|---|---|
| MSIL, 109 walks: CEP50 / CEP95 (mean CEP95) | 7.34 / 9.46 (15.67) | 6.76 / 9.41 (14.40) |
| IPIN Nuremberg, 4 walks: CEP50 / CEP95, floor hit, p75 | 7.66 / 31.57, 78%, 24.13 | 6.56 / 30.39, 81%, 18.56 |
| IPIN UMinho, 2 walks, same | 14.22 / 42.98, 59%, 34.36 | 9.10 / 39.65, 56%, 32.19 |
| UJI BLE, 282 sessions: CEP50 / CEP95 | 1.78 / 3.27 | 1.72 / 2.89 |

**Cost.** `--serial`, microseconds per update (IMU samples included); the map is built once per venue.

| Venue | Cells (size) | PF | Grid | Map build |
|---|---:|---:|---:|---:|
| Offices (one floor) | 3,840 (0.5 m) | 41–67 | 171–179 | 7 ms |
| Offices (two floors) | 7,680 (0.5 m) | 72–92 | 310–357 | 13 ms |
| `airport_hall` | 12,080 (0.5 m) | 37 | 556 | 18 ms |
| KIT, 7 levels | 60,351 (0.5 m) | 112 | 2,860 | 0.3 s |
| MSIL site2 B1, outline map, 42 s walk | 200,152 (0.77 m) | 214 | 14,676 | 0.3 s |
| MSIL site2 F1, outline map, 172 s walk | 199,984 (0.51 m) | 1,069 | 19,685 | 0.3 s |

- **Wall counts** for the radio model are computed the first time a beacon is heard, and count as update
  time. They take 23–150 ms per beacon on KIT and IPIN, and 0.8–1.6 s on the mall floors with shop walls
  (`out_doors`). Later rewritten without changing any result: 1–10 ms per beacon, and the step loop
  1.3–1.6× faster; see [grid.md](grid.md#cost).
- **Against the clock.** Replaying a walk on one desktop core takes 1–2% of its duration for an office.
  For a mall walk it takes 60–100% (the PF: 2–5%).
- **Memory** is about 8 bytes per (cell, heading) plus about 120 bytes per cell: 130–210 MB for a mall
  floor.

**Findings.**
- **No sampling, no lock-on problem.** The uniform prior is exact from the first update. Without any
  beacon-ring start, three of the four two-floor walks and KIT converge in 0.7–1.8 s instead of 8–13 s.
  `office_elevator`, which the PF already settles in 1.3 s, takes 2.5 s.
- **The airport is not faster** (35.9 → 34.5 s). With 35% loss and beacons 20 m apart, the posterior
  itself stays ambiguous for half a minute; the PF's ring start was not what limited it.
- **Most single-floor offices converge 0.5–3 s later** (`office_field_issue` 2.5 → 5.7 s,
  `office_mag_disturbance` 3.8 → 6.1 s; `office_device_offset` 0.5 s sooner). Likely reason, not
  verified: the heading stays spread until the walls and the magnetometer settle it, and the error must
  stay under 3 m for 10 s to count.
- **The raw track is calm.**
  - Jitter p95 nearly halves.
  - Jumps fall by 60%.
  - Moves through walls fall by 60%: `office_floor_bleed` 5.51 → 0.52 a minute, the stairs walk 2.01 →
    0.08.
  There is no sampling noise for the smoother to hide.
- **The posterior is sharper than the truth warrants.** Coverage95 is 82% → 74%, and 62% on
  `office_step_scale`; on the real walks it falls too (IPIN 33% → 15%). The grid's spread is what the
  model says, and the model counts time-correlated packets as independent. Likely, not verified: the PF's
  roughening and sampling noise inflate its spread.
- **One scenario's CEP50 is worse**: `office_step_scale` 0.73 → 0.78 m (+7%), the cost of not carrying a
  step scale.
- **MSIL is mixed.** The median CEP improves and the held-out mean CEP95 too, but the tune mean CEP95
  rises 5%.
- **Compute is the price.**
  - 3–5× the PF on the offices;
  - 15× on the airport hall;
  - 25× on KIT;
  - 18–70× on a mall floor.

**Verdict.** The grid filter clears the accuracy bar set for a later promotion:
- mean CEP95 over the 12 scenarios no worse (2.94 → 1.83 m);
- no scenario more than 10% worse;
- real tune medians no worse;
- on IPIN, p75 lower in every group: by 17 m (Nuremberg) and 6 m (UMinho) on the tune walks, 6 m and 2 m
  held out.

It does not clear the cost bar for the mall floors: about real time on a desktop core, not a phone.
Its covariance is less honest, and its track is rougher on UMinho. It stays a prototype next to the
engine. Making it the engine would need:
- a sparse active set that works on Wi-Fi, or coarser cells away from the cloud;
- a wall count that does not scale with cells × beacons on shop-wall maps;
- the floor prior that `ipin.json` gives the PF.

## Open questions

- A Wi-Fi scan can still move the whole cloud across a wall into a large region next to the walker's (a
  parallel corridor), where it lasts longer than in a closed room (experiment 13). Forbidding injections
  across walls blocks the escape from a wrong corridor that recovery is for. A likelihood that does not
  treat a scan's readings as independent would attack the cause.
- The beacon-driven init proposal is now chosen by particle density (experiment 9), but venues between
  0.6 and 1 particle per m^2 are where it is mixed. A wall-aware ring (rejection sampling against the
  full likelihood) might remove the office cost and let it be on everywhere.
- A wider BLE likelihood (sigma 7 dB) improves 9 of 12 simulated scenarios, but the two-floor walks then
  take twice as long to lock on (experiment 12). If the floor decision could be kept sharp while the
  horizontal update is softened, the gain might be had without the cost.
- `office_nlos_heavy`: the wall-aware model assumes 3.5 dB per wall while the truth is 7 dB. Learning the
  wall loss online failed both as a particle state and as a venue-wide calibration (experiment 10); the
  device offset and the wall loss trade off too easily. Per-wall-type losses from the map are the
  alternative.
