#!/bin/bash
# Assemble the PortMaster submission zip: copies the built armhf binary and
# shaders into the skeleton, then zips it as acgc.zip.
# Usage: ./assemble.sh  (after building pc/build-armhf)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BIN="$REPO/pc/build-armhf/bin/AnimalCrossing"
[ -f "$BIN" ] || { echo "Build the armhf binary first"; exit 1; }
STAGE="$HERE/acgc/acgc"
cp "$BIN" "$STAGE/AnimalCrossing"
mkdir -p "$STAGE/shaders"
cp "$REPO/pc/shaders/default.vert" "$REPO/pc/shaders/default.frag" \
   "$REPO/pc/shaders/shader_seed.bin" "$STAGE/shaders/"
[ -f "$HERE/acgc/screenshot.png" ] || echo "WARNING: screenshot.png missing (required: 4:3 gameplay, >=640x480)"
cd "$HERE/acgc" && zip -r ../acgc.zip . -x '.*' && echo "Wrote $HERE/acgc.zip"
