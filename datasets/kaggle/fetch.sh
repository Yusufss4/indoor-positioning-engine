#!/usr/bin/env bash
# Downloads the full "Indoor Location & Navigation" competition data from Kaggle (about 60 GB:
# 204 buildings, about 30,000 walks) into datasets/kaggle/raw/ and unpacks it. Raw data stays out
# of git (see .gitignore), and so does everything converted from it: the competition rules do not
# allow redistribution.
#
# Needs a Kaggle account that has accepted the rules at
#   https://www.kaggle.com/competitions/indoor-location-navigation/rules
# and its API token where the kaggle CLI looks for it (~/.kaggle/access_token, or
# ~/.kaggle/kaggle.json, or KAGGLE_API_TOKEN). The CLI comes from datasets/kaggle/requirements.txt.
#
# The two sites of the public sample (datasets/msil) are part of this set under other names.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
raw="$here/raw"
zip="$raw/indoor-location-navigation.zip"
kaggle="${KAGGLE:-$here/../../.venv/bin/kaggle}"
[ -x "$kaggle" ] || kaggle=kaggle
mkdir -p "$raw"

if [ ! -f "$raw/.unpacked" ]; then
  if [ ! -f "$zip" ]; then
    echo "downloading the competition archive into $raw"
    "$kaggle" competitions download -c indoor-location-navigation -p "$raw"
  fi
  echo "unpacking $zip"
  unzip -q -n "$zip" -d "$raw"
  touch "$raw/.unpacked"
fi

# The converters rely on this layout; fail here rather than later if Kaggle changes it.
for d in train test metadata; do
  [ -d "$raw/$d" ] || { echo "unexpected layout: $raw/$d is missing" >&2; exit 1; }
done
[ -f "$raw/sample_submission.csv" ] || { echo "unexpected layout: sample_submission.csv is missing" >&2; exit 1; }

sites=$(find "$raw/train" -mindepth 1 -maxdepth 1 -type d | wc -l)
floors=$(find "$raw/train" -mindepth 2 -maxdepth 2 -type d | wc -l)
walks=$(find "$raw/train" -mindepth 3 -maxdepth 3 -name '*.txt' | wc -l)
tests=$(find "$raw/test" -maxdepth 1 -name '*.txt' | wc -l)
rows=$(($(wc -l < "$raw/sample_submission.csv") - 1))
echo "train: $sites sites, $floors floors, $walks walks; test: $tests walks, $rows rows to predict"
