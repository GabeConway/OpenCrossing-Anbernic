#!/bin/bash
# make-package.sh - Assemble the PortMaster zip for the RG-34XX SP (armhf).
#
# Usage: ./make-package.sh [path/to/AnimalCrossing] [output.zip]
#   binary default: <repo>/pc/build-armhf/bin/AnimalCrossing
#   output default: portmaster/AnimalCrossing-ac-gc-armhf.zip
#
# Zip layout (matches the install guide in README.md):
#   Animal Crossing.sh          -> copy to ROMS/Ports/ on the SD card
#   ports/ac-gc/AnimalCrossing
#   ports/ac-gc/shaders/        (canonical copies from pc/shaders/)
#   ports/ac-gc/rom/README.txt  (user supplies their own GAFE01 iso)
#   ports/ac-gc/libs.armhf/       (empty; device libs used, fallback .so drop-in)
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
ARMHF_BIN="${1:-$REPO/pc/build-armhf/bin/AnimalCrossing}"
OUT="${2:-$HERE/AnimalCrossing-ac-gc-armhf.zip}"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac

[ -f "$ARMHF_BIN" ] || { echo "armhf binary not found: $ARMHF_BIN"; exit 1; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

PORT="$STAGE/ports/ac-gc"
mkdir -p "$PORT"
cp -R "$HERE/ac-gc/." "$PORT/"
# pc/shaders/ is the canonical shader source; overwrite any staged copies.
mkdir -p "$PORT/shaders"
cp "$REPO/pc/shaders/"* "$PORT/shaders/"
cp "$ARMHF_BIN" "$PORT/AnimalCrossing"
chmod +x "$PORT/AnimalCrossing"
cp "$HERE/Animal Crossing.sh" "$STAGE/Animal Crossing.sh"

rm -f "$OUT"
(cd "$STAGE" && zip -qr "$OUT" .)
echo "Package: $OUT"
unzip -l "$OUT"
