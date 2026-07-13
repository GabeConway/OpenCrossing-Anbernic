#!/bin/bash
# armhf docker build script (run inside linux/arm/v7 debian:bookworm)
set -x
export DEBIAN_FRONTEND=noninteractive
apt-get update 2>&1 | tail -2
apt-get install -y --no-install-recommends \
  gcc g++ cmake make libsdl2-dev libgles-dev pkg-config \
  xvfb libgl1-mesa-dri libegl1 ca-certificates 2>&1 | tail -3
mkdir -p /work/pc/build-armhf
cd /work/pc/build-armhf || exit 1
# Target tuning: Allwinner H700 = Cortex-A53 (armv7 userland) with NEON.
# Debian armhf gcc defaults to vfpv3-d16; NEON must be enabled explicitly for
# the vectorized texture decoders and general auto-vectorization.
TUNE="-fsigned-char -mcpu=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=hard"
cmake .. -DPC_USE_GLES=ON \
  -DCMAKE_C_FLAGS="$TUNE" \
  -DCMAKE_CXX_FLAGS="$TUNE" 2>&1 | tail -20 || exit 1
make -j4 2>&1 | tee /work/pc/build-armhf/make.log | grep -E "^\[|Error|error:|warning: .*\[-W(error|fatal)|Built target" | tail -40
MAKE_RC=${PIPESTATUS[0]}
echo "=== MAKE EXIT: $MAKE_RC ==="
if [ "$MAKE_RC" -ne 0 ]; then
  echo "=== ERROR CONTEXT ==="
  grep -B3 -A5 -E "error:|Error " /work/pc/build-armhf/make.log | tail -80
  exit $MAKE_RC
fi
echo "=== FILE OUTPUT ==="
file /work/pc/build-armhf/bin/AnimalCrossing || find /work/pc/build-armhf -name AnimalCrossing -exec file {} \;
echo "=== READELF NEEDED ==="
BIN=$(find /work/pc/build-armhf -name AnimalCrossing -type f | head -1)
readelf -d "$BIN" | grep NEEDED
echo "=== SMOKE TEST ==="
cd "$(dirname "$BIN")" || exit 1
xvfb-run -a timeout 60 ./AnimalCrossing --verbose 2>&1 | tail -40
echo "=== SMOKE EXIT: $? ==="
