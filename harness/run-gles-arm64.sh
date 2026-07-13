#!/bin/bash
# run-gles-arm64.sh - Tier 2: interactive GLES run in Docker, displayed via XQuartz.
# Prereqs: XQuartz running, "Allow connections from network clients" enabled,
#          xhost +localhost run once in an XQuartz terminal.
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN_DIR="$REPO/pc/build-linux-arm64-gles/bin"
[ -x "$BIN_DIR/AnimalCrossing" ] || { echo "Build missing — run harness/smoke.sh arm64 build first"; exit 1; }
mkdir -p "$REPO/harness/rom"

docker run --rm -it --platform linux/arm64 \
  -e DISPLAY=host.docker.internal:0 \
  -e SDL_VIDEODRIVER=x11 \
  -v "$REPO:/work" \
  -v "$REPO/harness/rom:/work/pc/build-linux-arm64-gles/bin/rom" \
  -w /work/pc/build-linux-arm64-gles/bin \
  acgc-smoke-deps:arm64 \
  ./AnimalCrossing "$@"
