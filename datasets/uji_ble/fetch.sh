#!/usr/bin/env bash
# Downloads the UJI BLE RSS measurements database (Mendoza-Silva, Matey-Sanz, Torres-Sospedra, Huerta,
# doi:10.5281/zenodo.1618692; data CC BY 4.0, scripts MIT, 451 kB) into datasets/uji_ble/raw/ and
# unpacks it. A zip already there with the right md5 is not downloaded again.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
raw="$here/raw"
zip="$raw/UJI_BLE_DB.zip"
md5="e3ed8712316db24b99d4fddb54efc9d4"
mkdir -p "$raw"
if [ ! -f "$zip" ] || [ "$(md5sum "$zip" | cut -d' ' -f1)" != "$md5" ]; then
  curl -fL --retry 3 -o "$zip.part" "https://zenodo.org/records/1618692/files/UJI_BLE_DB.zip?download=1"
  mv "$zip.part" "$zip"
fi
if [ "$(md5sum "$zip" | cut -d' ' -f1)" != "$md5" ]; then
  echo "md5 mismatch for $zip" >&2
  exit 1
fi
[ -d "$raw/db" ] || unzip -q "$zip" -d "$raw/db"
echo "UJI BLE: $(ls "$raw/db/data/rss" | wc -l) files in $raw/db/data/rss"
