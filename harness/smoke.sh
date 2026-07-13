#!/bin/bash
# smoke.sh - Headless boot check inside Docker.
# Usage: ./harness/smoke.sh [arm64|armhf] [frames]
# Exit 0 = game process started, created a GL(ES) context, and survived past
# disc load (requires a disc image in harness/rom/).
set -e

ARCH="${1:-arm64}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO/harness/out"
mkdir -p "$OUT"

case "$ARCH" in
  arm64) PLATFORM=linux/arm64;   BUILDDIR=build-linux-arm64-gles; SRC="$REPO" ;;
  armhf) PLATFORM=linux/arm/v7;  BUILDDIR=build-armhf;            SRC="${REPO}-armhf" ;;
  *) echo "usage: $0 [arm64|armhf]"; exit 2 ;;
esac

LOG="$OUT/${ARCH}-smoke.log"
docker run --rm --platform "$PLATFORM" \
  -v "$SRC:/work" \
  -v "$REPO/harness/rom:/work/pc/$BUILDDIR/bin/rom:ro" \
  -w "/work/pc/$BUILDDIR/bin" \
  acgc-smoke-deps:$ARCH \
  bash -c 'xvfb-run -a timeout 120 ./AnimalCrossing --verbose --framelimit 0; echo "EXIT:$?"' \
  2>&1 | tee "$LOG"

# timeout(124) after surviving boot counts as pass; instant crash does not
if grep -qE "EXIT:(0|124)" "$LOG" && ! grep -qiE "segfault|Assertion|failed to create" "$LOG"; then
    echo "SMOKE PASS ($ARCH)"; exit 0
else
    echo "SMOKE FAIL ($ARCH) — see $LOG"; exit 1
fi
