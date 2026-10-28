# Experiment log

Each entry: the observation, the hypothesis, the change, and the measured effect across all scenarios.
Numbers are means over seeds (`ipe-regress --no-baseline --seeds N`).

## 1. EKF drifting out of the building

**Observation.** On `office_nominal` the EKF had CEP50 21.6 m while the map-free particle filter had
0.57 m on the same trace, so the measurement model itself was usable.

**Diagnosis.** The estimate walked steadily away from the far side of the floor. Packets from beacons
behind one or two walls arrive ~10 dB below the free-space prediction. At 10-40 m the path-loss curve is
flat (about 0.2-0.9 dB/m), so the innovation is small enough to pass a 3-sigma gate, yet the Kalman gain
converts it into a 1-2 m push away from the beacon. Several such beacons in the same direction add up.
The particle filter survives because its likelihood is evaluated non-linearly and is dominated by close
beacons, whose RSSI changes quickly with position.

**Change.** Ignore packets below `ekf.rssi_min_dbm = -85`.

| `rssi_min_dbm` | CEP50 [m] | CEP95 [m] |
|---:|---:|---:|
| none | 21.64 | 24.05 |
| -85 | 0.56 | 1.39 |
| -80 | 0.59 | 2.02 |
| -75 | 0.52 | 1.23 |

-85 was kept: -75 looks slightly better on this trace, but it discards most packets in sparse venues.

## 2. Moved beacon hijacking the particle filter (`office_field_issue`)

**Observation.** The map-aware filter was *worse* than the map-free one on this scenario
(CEP95 5.41 m vs 2.42 m, 5 seeds), with a 17 m error plateau between t = 30 s and 40 s.

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
(field_issue CEP95 2.32 -> 2.19 m, others unchanged within noise). It does not help the map-free preset,
whose residuals are biased by unmodelled walls, so `pf-nomap` disables it.

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
the same seed, e.g. `office_field_issue` seed 1: PF mean 0.71 m / CEP95 1.70 m, EKF 0.80 / 1.72, WCL
3.46 / 13.48. This holds because all randomness uses `ipe::Rng` instead of `std::normal_distribution`.

## Open questions

- Airport hall convergence (~70 s) is dominated by sparse beacons and symmetric geometry. An initial
  proposal drawn from the strongest beacons, instead of uniform over 3200 m^2, should cut it sharply.
  It should also recover the CEP95 lost in experiment 2: the slower recovery trigger mainly hurts there
  because the cloud takes longer to escape a wrong mode, and a beacon-driven proposal would not depend
  on the trigger. A per-venue recovery rate (fast in open halls, slow in walled offices) is the fallback.
- The map-aware filter beats the map-free one everywhere except `office_mag_disturbance` convergence time,
  where wall kills during the first heading-uncertain steps slow convergence (3.7 s vs 0.7 s).
- `office_nlos_heavy`: the wall-aware model assumes 3.5 dB per wall while the truth is 7 dB; estimating
  wall loss online (per venue) is a natural next state to add.
