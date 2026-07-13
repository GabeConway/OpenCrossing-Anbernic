#!/bin/bash
# make-package.sh - Assemble the PortMaster zip for the RG-34XX SP (armhf).
# Requires the armhf binary at ../../ACGC-PC-Port-armhf/pc/build-armhf/bin/AnimalCrossing
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ARMHF_BIN="$HERE/../../ACGC-PC-Port-armhf/pc/build-armhf/bin/AnimalCrossing"
OUT="$HERE/AnimalCrossing-ac-gc-armhf.zip"

[ -f "$ARMHF_BIN" ] || { echo "armhf binary not found: $ARMHF_BIN"; exit 1; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/ac-gc"
cp -R "$HERE/ac-gc/." "$STAGE/ac-gc/"
cp "$ARMHF_BIN" "$STAGE/ac-gc/AnimalCrossing"
chmod +x "$STAGE/ac-gc/AnimalCrossing"
cp "$HERE/Animal Crossing.sh" "$STAGE/"

(cd "$STAGE" && zip -qr "$OUT" .)
echo "Package: $OUT"
unzip -l "$OUT"
