# CMake toolchain file for Linux AArch64 (64-bit ARM)
#
# Requires:
#   sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
#                    libsdl2-dev:arm64
#
# On Debian/Ubuntu multiarch you may first need:
#   sudo dpkg --add-architecture arm64
#   sudo apt update

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH
    /usr/lib/aarch64-linux-gnu
    /usr/aarch64-linux-gnu
    /usr
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)   # build tools from host
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)    # libs from target (arm64)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)    # headers from target
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)    # cmake package configs from either
