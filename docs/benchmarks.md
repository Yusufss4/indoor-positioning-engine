# Against the competitions' leaderboards

Two of the real datasets in this repository come from competitions: the IPIN competition's smartphone
track ([ipin.md](ipin.md)) and Kaggle's "Indoor Location & Navigation" ([real_data.md](real_data.md) uses
its public sample). This page scores the engine with each competition's own metric, on the competition's
own walks where their truth is public, and sets the result next to the leaderboard.

**Summary.**

| Competition | Winner | Podium | Engine before | Engine now |
|---|---:|---|---:|---:|
| IPIN 2024 Track 3, Nuremberg | 7.9 m | 10.1, 29.7 m | 15.2 m | **6.3 m** (grid filter 5.1 m) |
| IPIN 2023 Track 3, Nuremberg | 5.3 m | 6.3, 7.7 m | 27.1 m | **6.7 m** (grid filter 5.0 m) |
| IPIN 2022 Track 3, UMinho | 30.1 m | 39.8 m, then over 45 m | 26.8 m | **10.0 m** (grid filter 6.8 m) |
| Kaggle 2021, private leaderboard | 1.50 m | 2.20, 2.49 m | 8.92 m | **5.31 m**, offline |

- The IPIN rows are like for like: the competition's scoring walks, its metric, a causal estimate, and
  the best of a year's trials, as the competition counts. "Before" is `msil_wifi.json`, the config the
  regression gate uses; "now" is `data/configs/fingerprint.json`, chosen on the validation walks and
  replayed once on the scoring walks.
- The Kaggle row is like for like too: late submissions of the 626 test walks, scored by Kaggle. The
  engine stays far behind the winner there, at about the level of the 500th of 1,170 teams (5.7 m). That
  competition is scored offline, and "now" includes offline steps a phone cannot do live
  ([below](#kaggle-indoor-location--navigation-2021)).
- What changed is one thing: the venue's training scans are used as position fingerprints
  ([below](#what-changed-in-the-engine)). It is opt-in; the engine's defaults and the synthetic scenarios
  are untouched.

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

Competition metric per scoring trial [m], with the share of landmarks on the right floor. The particle
filter's numbers are means over 5 filter seeds; the grid filter has no seed.

| Trial | Phone | `msil_wifi.json` | `ipin.json` | `fingerprint.json`, particle filter | `fingerprint.json`, grid filter |
|---|---|---:|---:|---:|---:|
| Nuremberg 2024 #1 | Galaxy A5 2017 | 15.2 / 83% | 23.4 / 82% | **6.3 / 97%** | 6.6 / 97% |
| Nuremberg 2024 #2 | Galaxy S7 | 21.2 / 82% | 21.8 / 84% | **6.6 / 90%** | 5.1 / 96% |
| Nuremberg 2023 #1 | Galaxy A5 2017 | 32.2 / 69% | 32.4 / 74% | **8.9 / 95%** | 8.6 / 94% |
| Nuremberg 2023 #2 | Galaxy S7 | 27.1 / 75% | 37.8 / 72% | **6.7 / 92%** | 5.0 / 96% |
| UMinho 2022 #12 | Galaxy A31, no barometer | 26.8 / 65% | 31.8 / 74% | **10.0 / 90%** | 7.7 / 95% |
| UMinho 2022 #13 | Galaxy A5 2017 | 41.9 / 54% | 36.5 / 79% | **11.0 / 92%** | 6.8 / 97% |

Against the leaderboards, taking the best trial of a year as the competition does:

- **2024.** 6.3 m (particle filter) and 5.1 m (grid filter) are both below the winner's 7.9 m.
- **2023.** 6.7 m would be third, between 6.3 and 7.7 m; the grid filter's 5.0 m is below the winner's
  5.3 m.
- **2022.** 10.0 m and 6.8 m are far below the winner's 30.1 m. That year's field was weak, and the
  engine's old 26.8 m was already ahead of it.

Other statistics of the same replays (particle filter, median over the trials of a building):

| Building | CEP50 [m] | CEP95 [m] | Floor right | p75 with the floor given [m] |
|---|---|---|---|---|
| Nuremberg, 4 trials | 7.66 → 3.83 | 31.6 → 24.1 | 78% → 93% | 14.2 → 6.3 |
| UMinho, 2 trials | 14.2 → 4.8 | 43.0 → 35.1 | 59% → 91% | 23.2 → 9.8 |

CEP95 improves least: in five of the six trials, 5% of the landmarks are still more than 20 m off.

**What is and is not comparable.**

- The walks, the metric, the output rate and causality are the competition's. The estimate scored here
  is the one the engine reports at each landmark's time.
- The settings were chosen on the validation walks (`manifest_tune.json`, the competition's "testing
  trials") and the scoring walks were replayed once with them. Competitors had the same split.
- Competitors ran each scoring trial once, with no second chance. The particle filter's number is a mean
  over 5 seeds, so a single run can be better or worse than it; the grid filter's is a single run.
- 2022 has three scoring trials and this page two: the third phone (Aquaris X5 Plus) logs no
  orientation, which the converter needs for the heading ([ipin.md](ipin.md#conversion-datasetsipinconvertpy)).
- The competition ended years ago and its truth is public. Nothing was tuned on the scoring walks, but
  the engine was developed after their results were known, which a competitor's was not.
- Six walks are few. The validation walks say the same thing with more data: 27 walks, competition
  metric 15.1 → 4.7 m (Nuremberg) and 36.9 → 11.0 m (UMinho), floor right 100% and 96%
  ([experiments.md](experiments.md#15-position-fingerprints-measured-against-the-competitions-leaderboards)).

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
snapping radius was chosen here, on the tune half.

| Pipeline | Tune half (1,060 walks) | Held-out half (1,126 walks) |
|---|---:|---:|
| `msil_wifi.json`, causal, gaps filled (the engine before) | 12.17 | 13.00 |
| `fingerprint.json`, causal, gaps filled | 8.13 | 8.39 |
| + two passes | 6.07 | 6.20 |
| + snapping within 6 m | 5.69 | 5.89 |

- The first row leaves out 8 walks in each half in which the old engine never got a fix.
- Snapping radii from 4 to 12 m score within 0.04 m of each other on the tune half; 1 m gains nothing.
- The floor added to each pass's spread before weighting (1, 3 or 8 m) moves the result by 0.06 m.
- Without the two passes, snapping within 4 m gains 0.3 m (8.13 → 7.85).

**On the leaderboard.** Two late submissions, made on 2026-11-13 (late submissions are scored but not
ranked):

| Submission | Public (15% of the test set) | Private (85%) |
|---|---:|---:|
| The engine before: `msil_wifi.json`, causal, gaps filled | 9.55 | 8.92 |
| `fingerprint.json`, two passes, snapping within 6 m | 4.65 | **5.31** |
| Winner / 2nd / 3rd | 0.89 / 1.39 / 2.04 | 1.50 / 2.20 / 2.49 |
| 18th | 2.75 | 3.74 |

Both files were built once and submitted once; nothing was tuned against the leaderboard. The test walks
score better than the held-out ones in both rows (8.9 against 13.0 m, 5.3 against 5.9 m). The cause was
not looked into; the test walks are a different selection ("based on real-world user trace length",
mostly 40–150 m long), while the held-out walks are a random fifth of the survey.

**Why 5.3 m and not 1.5 m.** The winners' pipeline and this one share the first step, a k-NN over Wi-Fi
fingerprints, and little else ([below](#what-the-winners-do-that-the-engine-does-not)). The paper's own
breakdown says where the rest is: the tracking model, the joint optimisation of a walk and the snapping
are each worth 0.3–1.4 m to a top team, on top of positioning models tuned per building. Here the walks
are short (a median of 36 s on the sample), so a filter has few scans to settle with, and the first
waypoints of a walk weigh as much as the last.

## What changed in the engine

Before, radio meant one log-distance curve per access point: a fitted position, power at 1 m and
exponent. Access points whose readings fit no such curve were dropped, which in these buildings is most
of them (113 of 139 in Nuremberg). Every winner above used Wi-Fi fingerprints instead.

- **Training scans carry their position.** The converters already interpolate a position for every
  training reading between the surveyed landmarks; the venue's `floor_fingerprints` now keep it
  ([formats.md](formats.md#several-floors)).
- **Access points without a model stay**, marked `"model": false`, as dimensions of the fingerprints.
- **`FingerprintMap`** (`core/src/fingerprint_map.cpp`) finds a scan's 10 nearest training scans, compared
  over its 40 strongest readings.
- **The filters use them as a likelihood**, not as a position: a kernel density over position and level
  around the neighbours, next to the down-weighted log-distance term. The steps, the walls, the
  barometer and the floor vote act on it as before. The particle filter also seeds its cloud and draws
  its recovery particles from that density ([design.md](design.md#particle-filter)).

A scan's weighted k-NN position alone is no better than the old filter (7.1 m median error on the MSIL
sample against a CEP50 of 7.0 m); the gain is in the fusion.

Everything is off unless `pf.fingerprint_weight` is set and the venue has fingerprint positions:
`ipe-regress --no-baseline --seeds 10` on the 12 synthetic scenarios is identical before and after, and
the regression gates still run `msil_wifi.json` on venues without positions. The sweep that chose the
settings, the smoothness of the drawn track and what did not matter are in
[experiment 15](experiments.md#15-position-fingerprints-measured-against-the-competitions-leaderboards).

## What the winners do that the engine does not

- **Learned relative motion.** The Kaggle winners trained networks on the inertial data that cut the
  tracking error by about half against step counting. The engine's PDR is a step detector with a
  Weinberg step length and the phone's own heading.
- **Whole-walk optimisation.** The top Kaggle teams solve each walk as one least-squares or discrete
  optimisation problem over all its scans. The two-pass replay here is two causal filters, combined.
- **Per-building tuning.** The top three Kaggle teams tuned their k-NN per site and per floor. The engine
  uses one setting everywhere, chosen on two other buildings.
- **Side channels.** Some Kaggle teams identified the surveyor's device from sensor timing and linked
  walks through timestamps the organisers had not anonymised. The engine uses neither.
- **Uncertainty.** None of the metrics above reward it, and the engine's is still too narrow: the 95%
  ellipse holds the truth for 25–51% of the fixes on the IPIN validation walks.

## Reproduce

```bash
# IPIN (640 MB from Zenodo)
datasets/ipin/fetch.sh
.venv/bin/python datasets/ipin/convert.py --wifi --scans --fingerprints --positions --unmodelled \
    --out datasets/ipin/out_fp
.venv/bin/python datasets/ipin/evaluate.py datasets/ipin/out_fp/manifest_tune.json \
    --config data/configs/fingerprint.json --seeds 5                 # validation walks: choose settings here
.venv/bin/python datasets/ipin/evaluate.py datasets/ipin/out_fp/manifest_holdout.json \
    --config data/configs/fingerprint.json --seeds 5                 # scoring walks
.venv/bin/python datasets/ipin/evaluate.py datasets/ipin/out_fp/manifest_holdout.json \
    --config data/configs/fingerprint.json --tool grid               # the grid filter

# Kaggle sample (2.5 GB, no account needed)
datasets/msil/fetch.sh
.venv/bin/python datasets/msil/convert.py --wifi --scans --fingerprints --out datasets/msil/out_fp
.venv/bin/python datasets/ipin/evaluate.py datasets/msil/out_fp/manifest_tune.json --warmup 0 \
    --config data/configs/fingerprint.json --two-pass --fill --snap 4

# Full Kaggle set (69 GB unpacked; needs a Kaggle token and the competition rules accepted)
.venv/bin/pip install -r datasets/kaggle/requirements.txt
datasets/kaggle/fetch.sh
.venv/bin/python datasets/msil/convert.py --kaggle datasets/kaggle/raw --test-sites --wifi --scans \
    --fingerprints --jobs 8 --out datasets/kaggle/out_fp
.venv/bin/python datasets/kaggle/submit.py --converted datasets/kaggle/out_fp --check-floors
.venv/bin/python datasets/kaggle/submit.py --converted datasets/kaggle/out_fp \
    --config data/configs/fingerprint.json --two-pass --snap 4 --out datasets/kaggle/out_fp/submission.csv
```

`evaluate.py` prints, per walk and per group, CEP50/CEP95, the floor hit rate, the IPIN metric (also the
best walk of a group, and with the floor given) and the Kaggle metric. `--two-pass`, `--fill` and `--snap`
are the offline steps of `datasets/common/offline.py`; without them every number is causal.
