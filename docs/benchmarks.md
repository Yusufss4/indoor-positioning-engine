# Against the competitions' leaderboards

Two of the real datasets in this repository come from competitions: the IPIN competition's smartphone
track ([ipin.md](ipin.md)) and Kaggle's "Indoor Location & Navigation" ([real_data.md](real_data.md) uses
its public sample). This page scores the engine with each competition's own metric, on the competition's
own walks where their truth is public, and sets the result next to the leaderboard.

**Summary.**

| Competition | Winner | Podium | Engine before | Engine now |
|---|---:|---|---:|---:|
| IPIN 2024 Track 3, Nuremberg | 7.9 m | 10.1, 29.7 m | 15.2 m | **5.1 m** |
| IPIN 2023 Track 3, Nuremberg | 5.3 m | 6.3, 7.7 m | 27.1 m | **5.0 m** |
| IPIN 2022 Track 3, UMinho | 30.1 m | 39.8 m, then over 45 m | 26.8 m | **6.8 m** |
| Kaggle 2021, private leaderboard | 1.50 m | 2.20, 2.49 m | 8.92 m | **5.30 m** |

- The IPIN rows are like for like: the competition's scoring walks, its metric, a causal estimate, and
  the best of a year's trials, as the competition counts. "Now" is `data/configs/fingerprint.json`, chosen
  on the validation walks and replayed once on the scoring walks.
- The Kaggle row is like for like too: late submissions of the 626 test walks, scored by Kaggle. The
  engine stays far behind the winner there, at about the level of the 500th of 1,170 teams (5.7 m). That
  competition is scored offline, and "now" includes offline steps a phone cannot do live
  ([below](#kaggle-indoor-location--navigation-2021)).
- "Before" is the engine before the two changes this page is about: a particle filter with a log-distance
  radio model (`msil_wifi.json` of that time).
- Two things changed. The venue's training scans are used as position fingerprints, and the particle
  filter was replaced by a grid filter ([below](#what-changed-in-the-engine)).

## The leaderboards

### IPIN Track 3 (smartphone, off-site)

The score is the third quartile of the point error at the landmarks, where a wrong floor adds 15 m per
floor. Competitors receive the sensor stream through the EvaalAPI and must answer every 0.5 s, so the
estimate is causal. A team runs up to three scoring trials and its best one counts (2023 technical annex).

| Year, building | 1st | 2nd | 3rd | Further |
|---|---|---|---|---|
| 2022, UMinho building 11 | X-lab 30.1 m | Leviathan 39.8 m | three teams over 45 m | two without a score |
| 2023, Museum Industriekultur Nuremberg | THWS 5.3 m | QAQ 6.3 m | KNU WCSL 7.7 m | 8.6, 13.5, two over 45 m |
| 2024, same building | CARELab 7.9 m | THWS & cronn 10.1 m | SZU-Escope 29.7 m | two over 45 m |

Sources: the results pages of [2022](https://evaal.isti.cnr.it/2022/results),
[2023](https://evaal.isti.cnr.it/2023/results) and [2024](https://evaal.isti.cnr.it/2024/results), and the
[2023 Track 3 technical annex](https://evaal.isti.cnr.it/fileadmin/assets/Current_Competition/Track-3_TA-2023-v1.pdf).
The 2022 results slides give 40.8 m for the second place.

### Kaggle "Indoor Location & Navigation" (2021)

The score is the mean over the test waypoints of the horizontal error plus 15 m per floor of floor error.
1,170 teams took part. The test set is 626 walks in 24 of the 204 buildings; a walk's building is known,
its floor is not.

| Place | Private score |
|---|---:|
| 1st | 1.50 m |
| 2nd | 2.20 m |
| 3rd | 2.49 m |
| 18th | 3.74 m |
| 500th | 5.7 m |

Source: Hu et al., "The Wisdom of 1,170 Teams: Lessons and Experiences from a Large Indoor Localization
Competition", MobiCom 2023 (Table 1 and section 1). Three things in that paper matter for a comparison:

- **It is not real-time.** Teams had every walk in full and could "leverage future information to
  correct their estimates of the previous test points".
- **The test points are surveyed waypoints** that the training walks also use, so top teams snapped
  their answers to them (0.3–1.4 m of gain). The paper notes this "may not be effective in real-world
  usage".
- **A final score follows the tracking error more than the positioning error** (correlation 0.78 against
  0.34): the top teams learned the relative motion from the sensors and optimised each walk as a whole.

## IPIN: scoring walks

Competition metric per scoring trial [m], with the share of landmarks on the right floor. The engine has
no random seed, so each number is one replay.

| Trial | Phone | Before: particle filter, log-distance model | Grid filter, `ipin.json` | Grid filter, `fingerprint.json` |
|---|---|---:|---:|---:|
| Nuremberg 2024 #1 | Galaxy A5 2017 | 15.2 / 83% | 35.4 / 70% | **6.6 / 97%** |
| Nuremberg 2024 #2 | Galaxy S7 | 21.2 / 82% | 12.2 / 87% | **5.1 / 96%** |
| Nuremberg 2023 #1 | Galaxy A5 2017 | 32.2 / 69% | 13.7 / 84% | **8.6 / 94%** |
| Nuremberg 2023 #2 | Galaxy S7 | 27.1 / 75% | 22.3 / 81% | **5.0 / 96%** |
| UMinho 2022 #12 | Galaxy A31, no barometer | 26.8 / 65% | 24.4 / 82% | **7.7 / 95%** |
| UMinho 2022 #13 | Galaxy A5 2017 | 41.9 / 54% | 25.7 / 85% | **6.8 / 97%** |

The first column is a mean over 5 filter seeds of the particle filter the engine had then. The middle
column is today's engine without position fingerprints: the log-distance radio model and the k-NN floor
vote. It shows how much of the gain is the fingerprints.

Against the leaderboards, taking the best trial of a year as the competition does:

- **2024.** 5.1 m is below the winner's 7.9 m; so is the other trial's 6.6 m.
- **2023.** 5.0 m is below the winner's 5.3 m. The other trial, 8.6 m, would tie for fourth.
- **2022.** 6.8 m is far below the winner's 30.1 m. That year's field was weak, and the engine's old
  26.8 m was already ahead of it.

Other statistics of the same replays (median over the trials of a building, before → now):

| Building | CEP50 [m] | CEP95 [m] | Floor right | p75 with the floor given [m] |
|---|---|---|---|---|
| Nuremberg, 4 trials | 7.66 → 3.60 | 31.6 → 12.8 | 78% → 96% | 14.2 → 5.8 |
| UMinho, 2 trials | 14.2 → 4.7 | 43.0 → 15.6 | 59% → 96% | 23.2 → 7.2 |

Smoothness of the drawn track on the same replays (`ipe-regress --traces`): jitter p95 0.48 m and 1.2
jumps a minute in Nuremberg, 0.84 m and 4.1 in UMinho; the dot crosses a wall 1.7 and 7.6 times a minute;
the reported floor changes 11.5 and 22.5 times per walk for 6 real changes. The 95% ellipse holds the
truth for 16% and 21% of the fixes.

**What is and is not comparable.**

- The walks, the metric, the output rate and causality are the competition's. The estimate scored here
  is the one the engine reports at each landmark's time.
- The settings were chosen on the validation walks (`manifest_tune.json`, the competition's "testing
  trials") and the scoring walks were replayed once with them. Competitors had the same split. The
  fingerprint settings were chosen with the particle filter and taken over unchanged by the grid filter.
- Competitors ran each scoring trial once, with no second chance. So does the engine: it is
  deterministic, and a trial has one result.
- 2022 has three scoring trials and this page two: the third phone (Aquaris X5 Plus) logs no
  orientation, which the converter needs for the heading ([ipin.md](ipin.md#conversion-datasetsipinconvertpy)).
- The competition ended years ago and its truth is public. Nothing was tuned on the scoring walks, but
  the engine was developed after their results were known, which a competitor's was not. The choice of
  filter was also made with the scoring walks' results in view (experiment 16), next to the synthetic
  scenarios and the validation walks, which point the same way.
- Six walks are few. The validation walks say the same thing with more data: 27 walks, competition
  metric 5.3 m (Nuremberg, floor right 100%) and 7.7 m (UMinho, 94%), against 27.3 and 38.4 m before
  ([experiments.md](experiments.md#15-position-fingerprints-measured-against-the-competitions-leaderboards)).
- `tests/regression/ipin_baseline.json` gates these six replays (CTest `ipin_regression`).

## Kaggle: Indoor Location & Navigation (2021)

The repository's public sample ([real_data.md](real_data.md)) has two of the 204 buildings. For this page
the full set was downloaded (`datasets/kaggle/fetch.sh`) and the 24 buildings of the test set converted:
139 floors, 10,877 walks. The competition rules do not allow the data to be redistributed, so nothing
converted from it is in the repository.

**Pipeline** (`datasets/msil/convert.py --kaggle`, `datasets/kaggle/submit.py`):

1. Per floor, a venue with the floor outline, the fitted access points and the training scans as
   fingerprints, as for the sample. 80% of a floor's walks train it; the other 20% are split in a tune
   half and a held-out half by a hash of the walk's id.
2. A test walk does not say its floor. It is a k-NN vote of the walk's Wi-Fi scans over the building's
   training scans: right for 99.1% of 2,186 held-out walks.
3. The walk is replayed through that floor's venue with `fingerprint.json`.
4. Offline steps (`datasets/common/offline.py`), since the competition is scored offline:
   - **Two passes.** The walk is replayed again backwards (same accelerations, opposite turn rates and
     heading), and the two estimates at each waypoint are averaged with weights from the filters' own
     spread. The forward pass has seen little at a walk's start and the backward pass little at its end.
   - **Gaps.** A waypoint before the first Wi-Fi scan takes the nearest fix in time.
   - **Snapping.** An answer within 6 m of a surveyed training waypoint moves onto it.

**Held-out walks of the 24 test buildings** (Kaggle metric [m]; the floor is given here, so add about
0.1 m for the 0.9% of walks whose floor the vote gets wrong). The settings are the IPIN ones; only the
snapping radius was chosen here, on the tune half, with the particle filter the engine had then. The grid
filter took the pipeline over unchanged and was replayed once, on the held-out half.

| Pipeline | Particle filter, tune half (1,060 walks) | Particle filter, held-out half (1,126 walks) | Grid filter, held-out half |
|---|---:|---:|---:|
| `msil_wifi.json`, causal, gaps filled | 12.17 | 13.00 | 11.92 |
| `fingerprint.json`, causal, gaps filled | 8.13 | 8.39 | 8.33 |
| + two passes | 6.07 | 6.20 | 6.09 |
| + snapping within 6 m | 5.69 | 5.89 | **5.70** |

- The first row leaves out 8 walks in each half in which the filter never got a fix (the same number
  for the grid filter).
- Snapping radii from 4 to 12 m score within 0.04 m of each other on the tune half; 1 m gains nothing.
  The grid filter's held-out half says the same (5.75, 5.70, 5.70, 5.70 m for 4, 6, 8 and 12 m).
- The floor added to each pass's spread before weighting (1, 3 or 8 m) moves the result by 0.06 m.
- Without the two passes, snapping within 4 m gains 0.3 m (8.13 → 7.85).
- The grid filter's last row has a median waypoint error of 4.07 m and a 75th percentile of 8.09 m; the
  median walk scores 3.54 m.

**On the leaderboard.** Three late submissions, made on 2026-11-13 (late submissions are scored but not
ranked):

| Submission | Public (15% of the test set) | Private (85%) |
|---|---:|---:|
| The engine before: particle filter, `msil_wifi.json`, causal, gaps filled | 9.55 | 8.92 |
| Particle filter, `fingerprint.json`, two passes, snapping within 6 m | 4.65 | 5.31 |
| Grid filter (the engine now), same pipeline | 5.05 | **5.30** |
| Winner / 2nd / 3rd | 0.89 / 1.39 / 2.04 | 1.50 / 2.20 / 2.49 |
| 18th | 2.75 | 3.74 |

- Each file was built once and submitted once; nothing was tuned against the leaderboard.
- The first two were made with the particle filter, which is no longer in the repository (commit
  `c00a90e` has it). The third is today's engine.
- The two filters tie on the private score. The public score, a seventh of the test set, is 0.4 m worse
  for the grid filter; on the held-out walks it is 0.2 m better. Both submissions answer the same floor
  for every walk, and after snapping at least half of the rows are the same point.
- The test walks score better than the held-out ones (8.9 against 13.0 m, 5.3 against 5.7–5.9 m). The
  cause was not looked into; the test walks are a different selection ("based on real-world user trace
  length", mostly 40–150 m long), while the held-out walks are a random fifth of the survey.

**Why 5.3 m and not 1.5 m.** The winners' pipeline and this one share the first step, a k-NN over Wi-Fi
fingerprints, and little else ([below](#what-the-winners-do-that-the-engine-does-not)). The paper's own
breakdown says where the rest is: the tracking model, the joint optimisation of a walk and the snapping
are each worth 0.3–1.4 m to a top team, on top of positioning models tuned per building. Here the walks
are short (a median of 36 s on the sample), so a filter has few scans to settle with, and the first
waypoints of a walk weigh as much as the last.

## What changed in the engine

**Fingerprints.** Before, radio meant one log-distance curve per access point: a fitted position, power at
1 m and exponent. Access points whose readings fit no such curve were dropped, which in these buildings is
most of them (113 of 139 in Nuremberg). Every winner above used Wi-Fi fingerprints instead.

- **Training scans carry their position.** The converters already interpolate a position for every
  training reading between the surveyed landmarks; the venue's `floor_fingerprints` now keep it
  ([formats.md](formats.md#several-floors)).
- **Access points without a model stay**, marked `"model": false`, as dimensions of the fingerprints.
- **`FingerprintMap`** (`core/src/fingerprint_map.cpp`) finds a scan's 10 nearest training scans, compared
  over its 40 strongest readings.
- **The filter uses them as a likelihood**, not as a position: a kernel density over position and level
  around the neighbours, next to the down-weighted log-distance term. The steps, the walls, the
  barometer and the floor vote act on it as before ([design.md](design.md#grid-filter)).

A scan's weighted k-NN position alone is no better than the old filter (7.1 m median error on the MSIL
sample against a CEP50 of 7.0 m); the gain is in the fusion. Fingerprints are off unless
`filter.fingerprint_weight` is set and the venue has fingerprint positions
([experiment 15](experiments.md#15-position-fingerprints-measured-against-the-competitions-leaderboards)).

**One filter.** The fingerprints were built on a particle filter, with a grid filter next to it as a
prototype. With fingerprints the grid filter was better on every IPIN scoring trial but one, equal on the
mall floors and better on the synthetic scenarios, with no random seed and far less machinery; its price
is compute. The particle filter was removed and the grid filter is the engine
([experiment 16](experiments.md#16-one-filter-the-grid-filter-becomes-the-engine)). With the particle
filter and fingerprints, the six trials above scored 6.3, 6.6, 8.9, 6.7, 10.0 and 11.0 m.

## What the winners do that the engine does not

- **Learned relative motion.** The Kaggle winners trained networks on the inertial data that cut the
  tracking error by about half against step counting. The engine's PDR is a step detector with a
  Weinberg step length and the phone's own heading.
- **Whole-walk optimisation.** The top Kaggle teams solve each walk as one least-squares or discrete
  optimisation problem over all its scans. The two-pass replay here is two causal filters, combined; an
  exact forward-backward smoother on the grid is untried.
- **Per-building tuning.** The top three Kaggle teams tuned their k-NN per site and per floor. The engine
  uses one setting everywhere, chosen on two other buildings.
- **Side channels.** Some Kaggle teams identified the surveyor's device from sensor timing and linked
  walks through timestamps the organisers had not anonymised. The engine uses neither.
- **Uncertainty.** None of the metrics above reward it, and the engine's is too narrow: the 95% ellipse
  holds the truth for 16–21% of the fixes on the IPIN scoring walks.

## Reproduce

```bash
# IPIN (640 MB from Zenodo)
datasets/ipin/fetch.sh
.venv/bin/python datasets/ipin/convert.py --wifi --scans --fingerprints --positions --unmodelled \
    --out datasets/ipin/out_fp
.venv/bin/python datasets/ipin/evaluate.py datasets/ipin/out_fp/manifest_tune.json \
    --config data/configs/fingerprint.json                           # validation walks: choose settings here
.venv/bin/python datasets/ipin/evaluate.py datasets/ipin/out_fp/manifest_holdout.json \
    --config data/configs/fingerprint.json                           # scoring walks
ctest --test-dir build -R ipin_regression --output-on-failure        # the gate on the scoring walks

# Kaggle sample (2.5 GB, no account needed)
datasets/msil/fetch.sh
.venv/bin/python datasets/msil/convert.py --wifi --scans --fingerprints --out datasets/msil/out_fp
.venv/bin/python datasets/ipin/evaluate.py datasets/msil/out_fp/manifest_tune.json --warmup 0 \
    --config data/configs/fingerprint.json --two-pass --fill --snap 6

# Full Kaggle set (69 GB unpacked; needs a Kaggle token and the competition rules accepted)
.venv/bin/pip install -r datasets/kaggle/requirements.txt
datasets/kaggle/fetch.sh
.venv/bin/python datasets/msil/convert.py --kaggle datasets/kaggle/raw --test-sites --wifi --scans \
    --fingerprints --jobs 8 --out datasets/kaggle/out_fp
.venv/bin/python datasets/kaggle/submit.py --converted datasets/kaggle/out_fp --check-floors
.venv/bin/python datasets/kaggle/submit.py --converted datasets/kaggle/out_fp \
    --config data/configs/fingerprint.json --two-pass --snap 6 --out datasets/kaggle/out_fp/submission.csv
.venv/bin/python datasets/kaggle/export_demo.py                      # local web demo: a walk per floor
```

`evaluate.py` prints, per walk and per group, CEP50/CEP95, the floor hit rate, the IPIN metric (also the
best walk of a group, and with the floor given) and the Kaggle metric. `--two-pass`, `--fill` and `--snap`
are the offline steps of `datasets/common/offline.py`; without them every number is causal.
