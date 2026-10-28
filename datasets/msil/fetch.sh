#!/usr/bin/env bash
# Downloads the sample sites of the Microsoft Indoor Location Competition 2.0
# (github.com/location-competition/indoor-location-competition-20, MIT licence,
# ~2.5 GB) into datasets/msil/raw/. Raw data stays out of git; see .gitignore.
#
# The full dataset (200+ buildings) is on Kaggle ("indoor-location-navigation",
# competition rules restrict redistribution) and uses the same files: arrange it
# as <dir>/<site>/<floor>/{path_data_files/*.txt,floor_info.json,geojson_map.json}
# and pass --raw <dir> to convert.py.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
dest="$here/raw"
repo="https://github.com/location-competition/indoor-location-competition-20.git"
commit="${MSIL_COMMIT:-8ab177cd7be2700442714b7d13fa452bedc685f5}"
paths="${MSIL_PATHS:-data}"

if [ -d "$dest/.git" ]; then
  echo "already fetched: $dest ($(git -C "$dest" rev-parse --short HEAD))"
  exit 0
fi

git init -q "$dest"
git -C "$dest" remote add origin "$repo"
git -C "$dest" sparse-checkout set --no-cone $paths
git -C "$dest" fetch -q --depth 1 --filter=blob:none origin "$commit"
git -C "$dest" checkout -q FETCH_HEAD
echo "fetched $(find "$dest" -name '*.txt' -path '*path_data_files*' | wc -l) path files at ${commit:0:7} into $dest"
