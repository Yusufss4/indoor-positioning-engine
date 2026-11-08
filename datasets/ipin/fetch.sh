#!/usr/bin/env bash
# Downloads the IPIN competition Track 3 (smartphone, off-site) packages from Zenodo (CC BY 4.0)
# into datasets/ipin/raw/<year>/ and unpacks them. Raw data stays out of git; see .gitignore.
#
#   2022  UMinho building 11, Guimaraes   doi:10.5281/zenodo.7612915  (444 MB)
#   2023  Museum Industriekultur, Nuremberg doi:10.5281/zenodo.8362205 (100 MB)
#   2024  same venue and training walks as 2023, two new scoring walks
#                                         doi:10.5281/zenodo.13931119 (95 MB)
#
# A zip already in raw/ with the right md5 is not downloaded again.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
raw="$here/raw"
mkdir -p "$raw"

fetch() {
  local year="$1" record="$2" md5="$3"
  local zip="$raw/$year.zip" url="https://zenodo.org/records/$record/files/${year}_IPIN_Competition_Track03.zip?download=1"
  if [ ! -f "$zip" ] || [ "$(md5sum "$zip" | cut -d' ' -f1)" != "$md5" ]; then
    echo "downloading $year from Zenodo record $record"
    curl -fL --retry 3 -o "$zip.part" "$url"
    mv "$zip.part" "$zip"
  fi
  if [ "$(md5sum "$zip" | cut -d' ' -f1)" != "$md5" ]; then
    echo "md5 mismatch for $zip" >&2
    exit 1
  fi
  if [ ! -d "$raw/$year" ]; then
    mkdir -p "$raw/$year.tmp"
    unzip -q "$zip" -d "$raw/$year.tmp"
    mv "$raw/$year.tmp/${year}_IPIN_Competition_Track03" "$raw/$year"
    rmdir "$raw/$year.tmp"
  fi
  echo "$year: $(find "$raw/$year/01 Logfiles" -name '*.txt' | wc -l) logfiles in $raw/$year"
}

fetch 2022 7612915 5b2876681bb7855e0f562ec2ab4510c1
fetch 2023 8362205 6b29fe32c3c7bd1e7361b605b33d32ae
fetch 2024 13931119 cd80f7edca6bb2bf1c1bfe1c1a982246
