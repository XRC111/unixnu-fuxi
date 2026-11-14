#!/usr/bin/env bash
# Read this phone's Adreno 740 ZAP shader and print the pin for scripts/data/fuxi-gpu-inputs.json.
#
# The ZAP image is signed by the phone maker and authenticated by TrustZone (PAS), so only the
# phone's own image runs: the reference board's copy cannot be borrowed. Everything else in the GPU
# chain (GMU, SQE) comes from the shared SM8550 support.
#
# Read only on the phone: `adb shell ls` and `adb pull` of /vendor/firmware or
# /vendor/firmware_mnt/image; nothing is written there and no root is needed, because the files are
# world readable on stock builds. Android has to be running with USB debugging on.
#
# The image lands in .cache/firmware/fuxi-gpu/qcom/sm8550/fuxi/a740_zap.mbn of this repository,
# which is ignored and never committed.
#
# usage: scripts/pull-fuxi-gpu-firmware.sh [adb executable]
set -euo pipefail
ADB=${1:-${ADB:-adb}}
DEV=$(cd "$(dirname "$0")/.." && pwd)
OUT="$DEV/.cache/firmware/fuxi-gpu/qcom/sm8550/fuxi"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"
"$ADB" get-state >/dev/null
for dir in /vendor/firmware /vendor/firmware_mnt/image; do
  if "$ADB" shell "ls $dir/a740_zap.mbn" >/dev/null 2>&1; then
    "$ADB" pull "$dir/a740_zap.mbn" "$TMP/a740_zap.mbn" >/dev/null
    cp "$TMP/a740_zap.mbn" "$OUT/a740_zap.mbn"
    echo "source $dir/a740_zap.mbn"
    break
  fi
  if "$ADB" shell "ls $dir/a740_zap.mdt" >/dev/null 2>&1; then
    for name in $("$ADB" shell "cd $dir && ls a740_zap.mdt a740_zap.b[0-9][0-9]" | tr -d '\r'); do
      "$ADB" pull "$dir/$name" "$TMP/$name" >/dev/null
    done
    python3 "$DEV/scripts/squash-mdt.py" "$TMP/a740_zap.mdt" "$OUT/a740_zap.mbn"
    echo "source $dir/a740_zap.mdt + $(cd "$TMP" && ls a740_zap.b* | tr '\n' ' ')"
    break
  fi
done
[ -f "$OUT/a740_zap.mbn" ] || { echo "no a740_zap.mbn or a740_zap.mdt under /vendor/firmware or /vendor/firmware_mnt/image" >&2; exit 1; }
bytes=$(wc -c < "$OUT/a740_zap.mbn")
sha=$(sha256sum "$OUT/a740_zap.mbn" | cut -d' ' -f1)
echo "file   $OUT/a740_zap.mbn"
echo "pin    \"bytes\": $bytes, \"sha256\": \"$sha\""
