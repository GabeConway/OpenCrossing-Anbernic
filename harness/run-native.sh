#!/bin/bash
# run-native.sh - Tier 1: build (if needed) and play the native macOS binary.
# Usage: ./harness/run-native.sh [-- extra game args]
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO/pc/build"
BIN="$BUILD/bin/AnimalCrossing"
HARNESS_ROM="$REPO/harness/rom"

GCC="$(ls /opt/homebrew/bin/gcc-* 2>/dev/null | grep -E 'gcc-[0-9]+$' | sort -V | tail -1)"
GXX="${GCC/gcc/g++}"
[ -z "$GCC" ] && { echo "Homebrew GCC required: brew install gcc"; exit 1; }

if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && cmake .. \
        -DCMAKE_C_COMPILER="$(basename "$GCC")" \
        -DCMAKE_CXX_COMPILER="$(basename "$GXX")" \
        -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk)
fi
(cd "$BUILD" && make -j"$(sysctl -n hw.ncpu)")

# Link the shared harness rom/ dir next to the binary
mkdir -p "$HARNESS_ROM" "$BUILD/bin"
[ -e "$BUILD/bin/rom" ] || ln -s "$HARNESS_ROM" "$BUILD/bin/rom"

if ! ls "$HARNESS_ROM"/*.iso "$HARNESS_ROM"/*.gcm "$HARNESS_ROM"/*.ciso >/dev/null 2>&1; then
    echo "NOTE: no disc image found in harness/rom/ — game will not boot past the loader."
fi

cd "$BUILD/bin"
exec "$BIN" "$@"
