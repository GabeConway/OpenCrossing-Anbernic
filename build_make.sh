#!/bin/bash
# build_pc.sh - Build the Animal Crossing PC port
#
# Windows (MSYS2 MINGW32):
#   Prerequisites: pacman -S mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-SDL2
#   Run from MSYS2 MINGW32 shell
#
# Linux x86 (32-bit):
#   Prerequisites: sudo apt install gcc-multilib g++-multilib cmake libsdl2-dev:i386
#                  sudo dnf install gcc.i686 glibc-devel.i686 SDL2-devel.i686
#
# Linux ARMhf (32-bit ARM):
#   Prerequisites: sudo dpkg --add-architecture armhf && sudo apt update
#                  sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf \
#                                   cmake libsdl2-dev:armhf
#   Pass --armhf flag: ./build_pc.sh --armhf
#
# Usage:
#   1. ./build_pc.sh [--armhf]
#   2. Place your disc image (.ciso/.iso/.gcm) in pc/build32/bin/rom/
#   3. Run: pc/build32/bin/AnimalCrossing[.exe]

set -e

# --- Parse arguments ---
ARCH="x86"
USE_GLES=""
for arg in "$@"; do
    case "$arg" in
        --armhf) ARCH="armhf" ;;
        --amd64) ARCH="amd64" ;;
        --gles)  USE_GLES="-DPC_USE_GLES=ON" ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

# --- Detect platform ---
if [ "$MSYSTEM" = "MINGW32" ]; then
    PLATFORM="mingw32"
elif [ "$(uname -s)" = "Linux" ]; then
    PLATFORM="linux"
else
    echo "Error: Unsupported platform."
    echo "  Windows: Run from MSYS2 MINGW32 shell"
    echo "  Linux:   Run normally (bash build_pc.sh)"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ "$ARCH" = "amd64" ]; then
    BUILD_DIR="$SCRIPT_DIR/pc/buildamd64"
else
    BUILD_DIR="$SCRIPT_DIR/pc/build32"
fi
BIN_DIR="$BUILD_DIR/bin"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ "$PLATFORM" = "mingw32" ]; then
    # --- Windows / MSYS2 MINGW32 ---
    if [ ! -f Makefile ]; then
        echo "=== Configuring CMake (Windows MinGW32) ==="
        cmake .. -G "MinGW Makefiles"
    fi
    echo "=== Building ==="
    mingw32-make -j$(nproc)

elif [ "$ARCH" = "amd64" ]; then
    # --- Linux x86_64 native 64-bit ---
    export PKG_CONFIG_PATH="/usr/lib/x86_64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}"
    if [ ! -f Makefile ]; then
        echo "=== Configuring CMake (Linux x86_64 64-bit) ==="
        cmake .. -G "Unix Makefiles" \
            -DCMAKE_C_COMPILER=gcc \
            -DCMAKE_CXX_COMPILER=g++ \
            -DSDL2_DIR=/usr/lib/x86_64-linux-gnu/cmake/SDL2 \
            $USE_GLES
    fi
    echo "=== Building ==="
    make -j$(nproc)

elif [ "$ARCH" = "armhf" ]; then
    # --- Linux ARMhf (cross-compile from x86_64) ---

    # Help pkg-config find armhf libraries on multiarch hosts.
    for dir in \
        /usr/lib/arm-linux-gnueabihf/pkgconfig \
        /usr/lib/pkgconfig; do
        if [ -d "$dir" ]; then
            export PKG_CONFIG_PATH="$dir:${PKG_CONFIG_PATH:-}"
        fi
    done

    if command -v arm-linux-gnueabihf-pkg-config &>/dev/null; then
        export PKG_CONFIG=arm-linux-gnueabihf-pkg-config
    fi

    if [ ! -f Makefile ]; then
        echo "=== Configuring CMake (Linux ARMhf 32-bit) ==="
        cmake .. -G "Unix Makefiles" \
            -DCMAKE_TOOLCHAIN_FILE="../cmake/Toolchain-arm-linux-gnueabihf.cmake" \
            $USE_GLES
    fi
    echo "=== Building ==="
    make -j$(nproc)

else
    # --- Linux native 32-bit ---
    # Detect arch from the compiler target (uname -m reports host kernel in chroots)
    GCC_TARGET="$(gcc -dumpmachine 2>/dev/null || echo unknown)"
    HOST_ARCH="$(uname -m)"

    if echo "$GCC_TARGET" | grep -q "arm"; then
        # Native ARM 32-bit (chroot, RPi, etc.) — no toolchain needed
        if [ ! -f Makefile ]; then
            echo "=== Configuring CMake (Linux ARM 32-bit native) ==="
            cmake .. -G "Unix Makefiles" $USE_GLES
        fi

    elif [ "$HOST_ARCH" = "x86_64" ] || [ "$HOST_ARCH" = "i686" ]; then
        # x86: need 32-bit toolchain on x86_64 hosts
        for dir in \
            /usr/lib/i386-linux-gnu/pkgconfig \
            /usr/lib32/pkgconfig \
            /usr/lib/pkgconfig; do
            if [ -d "$dir" ]; then
                export PKG_CONFIG_PATH="$dir:${PKG_CONFIG_PATH:-}"
            fi
        done

        if command -v i686-linux-gnu-pkg-config &>/dev/null; then
            export PKG_CONFIG=i686-linux-gnu-pkg-config
        fi

        if [ ! -f Makefile ]; then
            echo "=== Configuring CMake (Linux x86 32-bit) ==="
            cmake .. -G "Unix Makefiles" \
                -DCMAKE_TOOLCHAIN_FILE="../cmake/Toolchain-linux32.cmake" \
                $USE_GLES
        fi

    else
        # Other 32-bit arch — try native
        if [ ! -f Makefile ]; then
            echo "=== Configuring CMake (Linux $(uname -m) native) ==="
            cmake .. -G "Unix Makefiles" $USE_GLES
        fi
    fi

    echo "=== Building ==="
    make -j$(nproc)
fi

# --- Create runtime directories ---
mkdir -p "$BIN_DIR/rom"
mkdir -p "$BIN_DIR/texture_pack"
mkdir -p "$BIN_DIR/save"

echo ""
echo "=== Build complete! ==="
echo ""
echo "Place your Animal Crossing disc image (.ciso/.iso/.gcm) in:"
echo "  $BIN_DIR/rom/"
echo ""
if [ "$PLATFORM" = "mingw32" ]; then
    echo "Run: $BIN_DIR/AnimalCrossing.exe"
else
    echo "Run: $BIN_DIR/AnimalCrossing"
fi
