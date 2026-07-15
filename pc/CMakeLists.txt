cmake_minimum_required(VERSION 3.16)
project(ac_pc C CXX)

# C standard is set per-source via COMPILE_OPTIONS (not CMAKE_C_STANDARD)
# because CMake appends the standard flag AFTER compile options, preventing
# decomp sources from overriding to gnu89. See "Per-source warning settings".
set(CMAKE_CXX_STANDARD 17)

# Decomp code relies on undefined behavior that GCC may exploit at -O1+.
# We use -O2 for performance but add flags to prevent UB-based miscompilation:
#   -fno-strict-aliasing  (already global) — type-punning through pointer casts
#   -fwrapv               (already global) — signed integer overflow wraps
#   -fno-delete-null-pointer-checks        — keep null checks after UB deref
#   -fno-lifetime-dse                      — don't optimize dead stores to dying objects
#   -fno-aggressive-loop-optimizations     — don't assume no UB in loop bounds
#   -fno-strict-overflow                   — pointer arithmetic overflow is defined
# If crashes occur, first try -O1, then -Og, then remove these lines for -O0.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()
# Decomp game code optimization level. History on the armhf device build:
#   -O2 → wild-pointer CRASH loop from boot (recovered per frame, black
#         screen with music);
#   -O1 → hard SIGBUS on the intro train scene (unaligned LDRD/VFP access
#         the decomp's pointer-punning provokes once GCC merges loads);
#   no -O (default) → stable. Decomp code stays UNOPTIMIZED until specific
# translation units are proven safe; pc/ platform sources are unaffected
# (they carry per-source -O2/-O3 below). Do not raise this without device-
# testing a full new-game intro (KK Slider → train → town arrival).
set(CMAKE_C_FLAGS_RELEASE "-DNDEBUG -fno-delete-null-pointer-checks -fno-lifetime-dse -fno-aggressive-loop-optimizations -fno-strict-overflow")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG -fno-delete-null-pointer-checks -fno-lifetime-dse -fno-aggressive-loop-optimizations -fno-strict-overflow")

# Options
option(PC_CONSOLE "Show console window (disables -mwindows, useful for debugging)" OFF)
option(PC_USE_GLES "Use OpenGL ES 3.2 instead of OpenGL 3.3" OFF)

# =============================================================================
# 32-bit enforcement (required: JSystem casts pointers to u32)
# =============================================================================
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "This project MUST be built as 32-bit (pointer-to-u32 casts in JSystem).\n"
        "  Windows/MSYS2: Use the MINGW32 shell (not MINGW64)\n"
        "  Linux: Use a 32-bit toolchain, e.g.:\n"
        "    cmake .. -DCMAKE_C_COMPILER=i686-linux-gnu-gcc -DCMAKE_CXX_COMPILER=i686-linux-gnu-g++\n"
        "    OR: cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/Toolchain-linux32.cmake")
endif()

# Allow mixed C/C++ linkage
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_EXTENSIONS ON)

# =============================================================================
# Build info (user who ran cmake/make)
# =============================================================================
if(DEFINED ENV{USER})
    set(BUILD_USER "$ENV{USER}")
elseif(DEFINED ENV{USERNAME})
    set(BUILD_USER "$ENV{USERNAME}")
else()
    execute_process(COMMAND whoami OUTPUT_VARIABLE BUILD_USER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT BUILD_USER)
        set(BUILD_USER "unknown")
    endif()
endif()
message(STATUS "Build user: ${BUILD_USER}")

# =============================================================================
# Compiler definitions
# =============================================================================
add_compile_definitions(
    BUILD_USER="${BUILD_USER}"
    TARGET_PC
    VERSION=0
    F3DEX_GBI_2
    NDEBUG
    _LANGUAGE_C    # Required for N64 GBI/ultratypes headers
    PC_ENHANCEMENTS   # Optional visual improvements (MSAA, etc). Remove to get pure PC port.
)

# _GNU_SOURCE: required only for pc_main.c (dladdr/Dl_info for image range detection).
# Do NOT define globally — it causes glibc to expose fsqrt(double) which conflicts
# with the game's f32 fsqrt(f32) declaration in libc64/math64.h.
if(UNIX AND NOT APPLE)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/pc_main.c"
        PROPERTIES COMPILE_DEFINITIONS "_GNU_SOURCE"
    )
endif()

# Common compiler flags (applied to all sources)
if(MSVC)
    add_compile_options(/W0)
else()
    add_compile_options(
        -fno-strict-aliasing
        -fwrapv
    )
    # On 32-bit x86 Linux, GCC defaults to x87 FPU which uses 80-bit extended
    # precision for ALL float ops. This causes angle interpolation to overshoot
    # (characters spin endlessly) because intermediate results don't match the
    # 32-bit precision the GameCube code expects. Force SSE2 math (proper 32/64-bit).
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-6]86" OR
       (CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86|X86"))
        add_compile_options(-msse2 -mfpmath=sse)
    endif()
endif()

# =============================================================================
# Include paths
# =============================================================================
set(DECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

include_directories(
    ${DECOMP_ROOT}/include
    # NOTE: Do NOT add ${DECOMP_ROOT}/include/libc here!
    # Having both include/ and include/libc/ in the path breaks #include_next
    # chains in the decomp's shadow headers (e.g. libc/string.h). Without it,
    # #include "string.h" etc. resolve directly to system headers on PC.
    ${DECOMP_ROOT}/src
    # Project root needed for includes like "src/actor/npc/event/file.c_inc"
    ${DECOMP_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/glad/include
    # .inc assets are loaded at runtime from disc image (no build/GAFE01_00 needed)
)

# =============================================================================
# Find SDL2
# =============================================================================
# Try system SDL2 first, then fall back to platform-specific locations
if(WIN32)
    set(SDL2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/SDL2/cmake" CACHE PATH "SDL2 cmake dir")
endif()
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    # Try pkg-config (works on both Linux and MSYS2)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SDL2 QUIET sdl2)
    endif()
endif()
if(NOT SDL2_FOUND)
    if(WIN32)
        # Windows-specific fallback paths
        # MSYS2 32-bit location
        set(SDL2_MSYS2_DIR "C:/msys64/mingw32")
        if(EXISTS "${SDL2_MSYS2_DIR}/include/SDL2/SDL.h")
            set(SDL2_FOUND TRUE)
            set(SDL2_INCLUDE_DIRS "${SDL2_MSYS2_DIR}/include/SDL2")
            set(SDL2_LIBRARIES "${SDL2_MSYS2_DIR}/lib/libSDL2.a")
        else()
            # Downloaded SDL2 MinGW dev package (32-bit)
            set(SDL2_DOWNLOAD_DIR "${DECOMP_ROOT}/../SDL2-2.30.10/i686-w64-mingw32")
            if(EXISTS "${SDL2_DOWNLOAD_DIR}/include/SDL2/SDL.h")
                set(SDL2_FOUND TRUE)
                set(SDL2_INCLUDE_DIRS "${SDL2_DOWNLOAD_DIR}/include/SDL2")
                set(SDL2_LIBRARIES "${SDL2_DOWNLOAD_DIR}/lib/libSDL2.a")
            else()
                # Last resort: vendored SDL2 in lib/
                set(SDL2_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/SDL2")
                if(EXISTS "${SDL2_LOCAL_DIR}/include/SDL.h")
                    set(SDL2_FOUND TRUE)
                    set(SDL2_INCLUDE_DIRS "${SDL2_LOCAL_DIR}/include")
                    set(SDL2_LIBRARIES "${SDL2_LOCAL_DIR}/lib/libSDL2.a")
                endif()
            endif()
        endif()
    else()
        # Linux manual fallback: search cross-compile multiarch paths directly.
        # Needed when pkg-config returns the host (64-bit) SDL2 instead of the
        # target one, or when pkg-config is not installed.
        # Prefer shared libraries (.so) over static (.a) so the binary works
        # across devices with different SDL2 video/audio backends.
        # Put the target arch directory first so multiarch hosts pick the right one.
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
            set(_sdl2_arch_dir /usr/lib/x86_64-linux-gnu)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(_sdl2_arch_dir /usr/lib/aarch64-linux-gnu)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
            set(_sdl2_arch_dir /usr/lib/arm-linux-gnueabihf)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-6]86")
            set(_sdl2_arch_dir /usr/lib/i386-linux-gnu)
        endif()
        set(_sdl2_search_paths
            ${_sdl2_arch_dir}              # target arch first
            /usr/lib/aarch64-linux-gnu     # Debian/Ubuntu multiarch arm64
            /usr/lib/arm-linux-gnueabihf   # Debian/Ubuntu multiarch armhf
            /usr/lib/i386-linux-gnu        # Debian/Ubuntu multiarch i386
            /usr/lib32                      # Fedora/Arch lib32
        )
        # First try to find the shared library explicitly
        find_library(SDL2_SHARED_LIB
            NAMES libSDL2.so
            PATHS ${_sdl2_search_paths}
            NO_DEFAULT_PATH
        )
        if(NOT SDL2_SHARED_LIB)
            # Fall back to any SDL2 library (may be static)
            find_library(SDL2_SHARED_LIB
                NAMES SDL2
                PATHS ${_sdl2_search_paths}
                NO_DEFAULT_PATH
            )
        endif()
        if(SDL2_SHARED_LIB)
            set(SDL2_FOUND TRUE)
            set(SDL2_LIBRARIES "${SDL2_SHARED_LIB}")
            # Headers are architecture-independent; SDL2 always installs to /usr/include/SDL2
            set(SDL2_INCLUDE_DIRS "/usr/include/SDL2")
        endif()
        unset(SDL2_SHARED_LIB CACHE)
    endif()

    if(NOT SDL2_FOUND)
        message(FATAL_ERROR
            "SDL2 not found!\n"
            "  Linux (x86):   sudo apt install libsdl2-dev:i386  (Debian/Ubuntu)\n"
            "                 sudo dnf install SDL2-devel.i686   (Fedora)\n"
            "  Linux (armhf): sudo apt install libsdl2-dev:armhf (Debian/Ubuntu)\n"
            "  Linux (arm64): sudo apt install libsdl2-dev:arm64 (Debian/Ubuntu)\n"
            "  Windows/MSYS2: pacman -S mingw-w64-i686-SDL2\n"
            "  Or download SDL2 development libraries to ${CMAKE_CURRENT_SOURCE_DIR}/lib/SDL2")
    endif()
endif()
# On Linux, force shared SDL2 linking for cross-device portability.
# Static SDL2 bakes in specific video/audio backends at compile time;
# dynamic linking uses the target device's SDL2 which has the right backends.
if(UNIX AND NOT APPLE AND SDL2_FOUND)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
        set(_sdl2_so_arch_dir /usr/lib/x86_64-linux-gnu)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_sdl2_so_arch_dir /usr/lib/aarch64-linux-gnu)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
        set(_sdl2_so_arch_dir /usr/lib/arm-linux-gnueabihf)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-6]86")
        set(_sdl2_so_arch_dir /usr/lib/i386-linux-gnu)
    endif()
    set(_sdl2_so_search_paths
        ${_sdl2_so_arch_dir}
        /usr/lib/aarch64-linux-gnu
        /usr/lib/arm-linux-gnueabihf
        /usr/lib/i386-linux-gnu
        /usr/lib32
        /usr/lib
    )
    find_library(_sdl2_shared_lib
        NAMES libSDL2.so
        PATHS ${_sdl2_so_search_paths}
        NO_DEFAULT_PATH
    )
    if(NOT _sdl2_shared_lib)
        # Also try default search paths
        find_library(_sdl2_shared_lib NAMES libSDL2.so)
    endif()
    if(_sdl2_shared_lib)
        set(SDL2_LIBRARIES "${_sdl2_shared_lib}")
        set(SDL2_FORCE_SHARED TRUE)
        message(STATUS "SDL2 forced shared: ${_sdl2_shared_lib}")
    endif()
    unset(_sdl2_shared_lib CACHE)
endif()


include_directories(${SDL2_INCLUDE_DIRS})
message(STATUS "SDL2 include: ${SDL2_INCLUDE_DIRS}")
message(STATUS "SDL2 libraries: ${SDL2_LIBRARIES}")

# =============================================================================
# GLAD (OpenGL loader - generated by glad2)
# =============================================================================
if(PC_USE_GLES)
    add_library(glad STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/glad/src/gles2.c
    )
    target_compile_definitions(glad PUBLIC PC_USE_GLES)
else()
    add_library(glad STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/glad/src/gl.c
    )
endif()
target_include_directories(glad PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/glad/include
)

# =============================================================================
# Collect game source files
# =============================================================================
file(GLOB_RECURSE GAME_C_SOURCES CONFIGURE_DEPENDS
    "${DECOMP_ROOT}/src/*.c"
)
file(GLOB_RECURSE GAME_CXX_SOURCES CONFIGURE_DEPENDS
    "${DECOMP_ROOT}/src/*.cpp"
)

# Exclude files that conflict with PC port or are platform-specific
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/AUS/.*")  # AUS-specific versions

# Exclude all Dolphin SDK hardware abstraction (we replace with pc_*.c stubs)
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/gx/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/os/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/vi/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/pad/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/card/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/dvd/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/ai/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/ar/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/dsp/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/exi/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/si/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/db/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/mtx/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/base/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/gba/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/amcstubs/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/OdemuExi2/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/dolphin/odenotstub/.*")

# Exclude PPC-specific runtime/compiler support (Metrowerks CodeWarrior)
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/TRK_MINNOW_DOLPHIN/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/Runtime\\.PPCEABI\\.H/.*")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/MSL_C\\.PPCEABI\\.bare\\.H/.*")

# Bootdata: Include window/logo model display lists (gam_win1.c, gam_win2.c, gam_win3.c, logo_nin.c)
# These provide Gfx[] arrays used by initial_menu.c for UI windows and Nintendo logo.

# Include ALL data files now that .inc assets are extracted from ROM
# (build/GAFE01_00/include/ is in the include path)
file(GLOB_RECURSE DATA_C_SOURCES CONFIGURE_DEPENDS "${DECOMP_ROOT}/src/data/*.c")
list(APPEND GAME_C_SOURCES ${DATA_C_SOURCES})

# f_furniture.c #includes all 696 src/furniture/*.c files as a single TU (like GC build).
# Exclude the individual files to avoid duplicate symbol definitions.
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/src/furniture/.*")

# Exclude duplicate data files (subdirectory verbose versions conflict with top-level)
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/src/data/model/obj_e_boat/.*")

# Exclude template/include .c files that are #included from other files
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/ac_npc_shop_common\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/ac_animal_logo_misc\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/m_item_debug\\.c$")

# Exclude NPC anime data fragments (no headers, #included from other NPC files)
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/actor/npc/.*_anime\\.c$")

# Exclude Dolphin OS sources that conflict with PC stubs
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/osreport\\.c$")
# audio.c now included (jaudio_NES provides Na_* functions)

# Exclude Famicom/NES emulator (contains PPC inline assembly)
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/Famicom/.*")

# Exclude MSL_C library sources (using system libc on PC)
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/MSL_C/.*")

# jaudio_NES: Include audio engine, exclude GBA-specific files
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/jaudio_NES/internal/dsp_cardunlock\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/jaudio_NES/internal/dsp_GBAKey\\.c$")

# libc64: Include most files (qrand, math64, sleep, etc.)
# Exclude malloc.c (redefines system malloc/free which crashes CRT on PC)
# Keep __osMalloc.c (game code uses __osMalloc* directly via m_malloc.c)
# Exclude sprintf.c/aprintf.c (they need _Printf from libultra xprintf)
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/libc64/malloc\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/libc64/sprintf\\.c$")
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/libc64/aprintf\\.c$")

# libultra: Exclude most (we have pc_os.c replacements), but keep xprintf for libc64
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/libultra/.*")

# Exclude PPC-specific files
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/ReconfigBATs\\.c$")

# Exclude GC-specific entry point (PC has its own main)
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/executor\\.c$")

# ROM asset .inc files now available in build/GAFE01_00/include/assets/
# All previously-excluded files are now compiled with real data.

# Exclude ef_effect_lib.c (has no #includes, pure function definitions without type context)
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/ef_effect_lib\\.c$")

# Exclude m_card.c (needs Dolphin CARD system headers with non-standard include paths)
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/m_card\\.c$")

# Also filter C++ sources
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/AUS/.*")
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/dolphin/.*")
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/TRK_MINNOW_DOLPHIN/.*")
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/Runtime\\.PPCEABI\\.H/.*")
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/MSL_C\\.PPCEABI\\.bare\\.H/.*")
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/Famicom/.*")

# Exclude emu64_print.cpp (it's #included directly from emu64.c, not a standalone file)
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/emu64_print\\.cpp$")

# libjsys: jsyswrapper_ext.cpp and jsyswrapper_main.cpp are #included from jsyswrapper.cpp
# so exclude them from standalone compilation (just like emu64_utility.c)
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/jsyswrapper_ext\\.cpp$")
list(FILTER GAME_CXX_SOURCES EXCLUDE REGEX ".*/jsyswrapper_main\\.cpp$")

# =============================================================================
# PC port source files
# =============================================================================
set(PC_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_gx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_gx_tev.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_gx_texture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_os.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_dvd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_pad.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_typing.c
    ${DECOMP_ROOT}/src/static/dolphin/pad/Padclamp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_card.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_vi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_audio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_aram.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_misc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_mtx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_m_card.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_save_bswap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_model_viewer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_texture_pack.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_settings.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_keybindings.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_overlay.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_stubs_cpp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_assets.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_disc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pc_prof.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/glibc_compat.c
)

# Rename game's main() to ac_entry() to avoid conflict with PC main() and game_main(GAME*)
set_source_files_properties(
    "${DECOMP_ROOT}/src/main.c"
    PROPERTIES COMPILE_DEFINITIONS "main=ac_entry"
)

# Rename boot.c's main() to avoid conflict with PC main()
set_source_files_properties(
    "${DECOMP_ROOT}/src/static/boot.c"
    PROPERTIES COMPILE_DEFINITIONS "main=boot_main"
)

# Exclude emu64_utility.c (it's #included from emu64.c, not a standalone file)
list(FILTER GAME_C_SOURCES EXCLUDE REGEX ".*/emu64_utility\\.c$")

# jaudio_NES C files use nullptr: define it as NULL for C compilation on PC.
# Generator expression ensures this only applies to C files (C++ has nullptr natively).
add_compile_options($<$<COMPILE_LANGUAGE:C>:-Dnullptr=NULL>)

# =============================================================================
# Per-source warning settings & GCC compatibility
# =============================================================================
# Decomp code: suppress all warnings (fires heavily on decompiled code)
# Clang treats some decomp patterns as errors even with -w:
# -Wno-return-type: missing return values in non-void functions
# -Wno-initializer-overrides: GBI macros use designated initializers with overlapping fields
set(DECOMP_WARN_FLAGS "-w;-fpermissive;-Wno-return-type;-Wno-error=return-type;-fno-stack-protector")
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    list(APPEND DECOMP_WARN_FLAGS "-Wno-c99-designator" "-Wno-initializer-overrides")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR
            "64-bit builds require GCC (Clang rejects pointer-to-u32 truncation in GBI static initializers).\n"
            "  macOS:  brew install gcc && cmake .. -DCMAKE_C_COMPILER=gcc-15 -DCMAKE_CXX_COMPILER=g++-15\n"
            "          (adjust version number to match installed GCC, e.g. gcc-14/g++-14)\n"
            "  Linux:  GCC is typically the default compiler.")
    endif()
endif()


# Decomp code uses K&R-style empty parameter lists: void f() {} with prototype
# void f(int, int). In C99/C11 () means "no args" → type conflict. In gnu89
# () means "unspecified args" → compatible. Use gnu89 for all decomp C sources.
# -fpermissive: on GCC 12+ also downgrades unnamed-param errors to warnings;
# on GCC <12 it's silently ignored for C (unnamed params fixed in source).
set(DECOMP_C_WARN_FLAGS "-w;-std=gnu89;-fpermissive")
set(DECOMP_CXX_WARN_FLAGS "-w;-fpermissive")
set_source_files_properties(${GAME_C_SOURCES}
    PROPERTIES COMPILE_OPTIONS "${DECOMP_C_WARN_FLAGS}"
)

# These .c files contain C++ code (classes, namespaces, ::) — compile as C++.
# Must override COMPILE_OPTIONS to use CXX flags (no -std=gnu89).
set_source_files_properties(
    "${DECOMP_ROOT}/src/static/libforest/emu64/emu64.c"
    "${DECOMP_ROOT}/src/static/jaudio_NES/internal/ja_calc.c"
    "${DECOMP_ROOT}/src/static/jaudio_NES/internal/jammain_2.c"
    "${DECOMP_ROOT}/src/static/jaudio_NES/game/game64.c"
    PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "${DECOMP_CXX_WARN_FLAGS}"
)

# jaudio_NES and libforest decomp files contain PC-port additions using C99/C11
# features (for-loop declarations, declarations after labels). Override all of them
# to gnu11. Files already set to LANGUAGE CXX above are unaffected (CXX wins).
set(DECOMP_C11_WARN_FLAGS "-w;-std=gnu11;-fpermissive")
file(GLOB _JAUDIO_C_FILES
    "${DECOMP_ROOT}/src/static/jaudio_NES/game/*.c"
    "${DECOMP_ROOT}/src/static/jaudio_NES/internal/*.c"
    "${DECOMP_ROOT}/src/static/libforest/emu64/*.c"
)
set_source_files_properties(${_JAUDIO_C_FILES}
    PROPERTIES COMPILE_OPTIONS "${DECOMP_C11_WARN_FLAGS}"
)

set_source_files_properties(${GAME_CXX_SOURCES}
    PROPERTIES COMPILE_OPTIONS "${DECOMP_CXX_WARN_FLAGS}"
)

# PC port code: enable warnings to catch bugs in code we control
set(PC_C_SOURCES ${PC_SOURCES})
list(FILTER PC_C_SOURCES EXCLUDE REGEX "\\.cpp$")
set(PC_CXX_SOURCES ${PC_SOURCES})
list(FILTER PC_CXX_SOURCES INCLUDE REGEX "\\.cpp$")
# Note: -Wno-comment/-Wno-unused-function/-Wno-builtin-declaration-mismatch suppress
# warnings from decomp headers (types.h, gfxprint.h, OSRtc.h, libultra.h) that we can't modify.
set(PC_WARN_SUPPRESS "-Wno-unused-parameter;-Wno-sign-compare;-Wno-comment;-Wno-unused-function;-Wno-builtin-declaration-mismatch")
set_source_files_properties(${PC_C_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-O2;-std=gnu11;-Wall;-Wextra;${PC_WARN_SUPPRESS}"
)
set_source_files_properties(${PC_CXX_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-O2;-Wall;-Wextra;${PC_WARN_SUPPRESS};-fpermissive"
)

# Texture decoders are the hottest CPU path on new-area loads: -O3 for
# aggressive unrolling/vectorization on top of the global tuning flags.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/pc_gx_texture.c
    PROPERTIES COMPILE_OPTIONS "-O3;-std=gnu11;-Wall;-Wextra;${PC_WARN_SUPPRESS}"
)

# emu64 (the game's N64->GX graphics-command engine) dominates per-frame game
# CPU time. Optimize just these TUs while the rest of the decomp stays at the
# crash-safe default (see the CMAKE_*_FLAGS_RELEASE note near the top).
# -fno-strict-aliasing matches the global decomp expectations.
# DEVICE-TEST GATE: any change here needs a full new-game intro on hardware.
set_source_files_properties(
    "${DECOMP_ROOT}/src/static/libforest/emu64/emu64.c"
    "${DECOMP_ROOT}/src/static/libforest/emu64/emu64_utility.c"
    PROPERTIES COMPILE_OPTIONS "${DECOMP_C_WARN_FLAGS};-O2;-fno-strict-aliasing;-fwrapv;-fno-delete-null-pointer-checks;-fno-lifetime-dse;-fno-aggressive-loop-optimizations;-fno-strict-overflow"
)

# =============================================================================
# Windows resource file (sets process name in Task Manager)
# =============================================================================
if(WIN32)
    set(RC_FILE "${CMAKE_CURRENT_SOURCE_DIR}/res/app.rc")
endif()

# =============================================================================
# Build executable
# =============================================================================
add_executable(ac_pc
    ${PC_SOURCES}
    ${GAME_C_SOURCES}
    ${GAME_CXX_SOURCES}
    ${RC_FILE}
)

# Link SDL2 without SDL2main (we define SDL_MAIN_HANDLED)
if(TARGET SDL2::SDL2)
    target_link_libraries(ac_pc glad SDL2::SDL2)
else()
    list(FILTER SDL2_LIBRARIES EXCLUDE REGEX "SDL2main")
    target_link_libraries(ac_pc glad ${SDL2_LIBRARIES})
endif()

if(WIN32)
    target_link_libraries(ac_pc opengl32 winmm imm32 version setupapi)
elseif(PC_USE_GLES)
    # -Wl,-no-as-needed forces NEEDED entries even when symbols are loaded
    # via function pointers at runtime (glad, dladdr). Without this, --as-needed
    # silently drops libGLESv2/libdl/libpthread from the dynamic section.
    target_link_libraries(ac_pc -Wl,-no-as-needed dl pthread m -Wl,-as-needed)
    find_library(GLESV2_LIB GLESv2 HINTS /usr/lib/aarch64-linux-gnu /usr/lib/arm-linux-gnueabihf)
    if(GLESV2_LIB)
        target_link_libraries(ac_pc -Wl,-no-as-needed ${GLESV2_LIB} -Wl,-as-needed)
    else()
        message(WARNING "libGLESv2 not found — GLES rendering will fail at runtime")
    endif()
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(ac_pc OpenGL::GL dl pthread m)
endif()

if(NOT WIN32 AND NOT APPLE)
    # Hide game's operator new/delete from shared libraries. On Linux, ELF symbol
    # interposition causes SDL/Mesa/ALSA threads to use JKRHeap's allocator which
    # isn't thread-safe and crashes. This version script keeps them binary-local.
    target_link_options(ac_pc PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/version_script.lds"
    )
endif()

# Decomp headers define functions as bare `inline` (not `static inline`). In gnu89
# mode each TU may emit an out-of-line copy → multiple definition errors on older ld.
# GCC 13/x86 ld is lenient; GCC 10/ARM ld is strict. Allow duplicates like x86 does.
if(NOT APPLE)
    target_link_options(ac_pc PRIVATE "LINKER:--allow-multiple-definition")
endif()

set_target_properties(ac_pc PROPERTIES
    OUTPUT_NAME "AnimalCrossing"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Copy shader files to output directory (loaded at runtime, with embedded fallback)
add_custom_command(TARGET ac_pc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/default.vert"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/default.frag"
        "${CMAKE_BINARY_DIR}/bin/shaders/"
    COMMENT "Copying shader files to bin/shaders/"
)

# Copy SDL2.dll next to executable so users don't need it on PATH
if(WIN32)
    find_file(SDL2_DLL_PATH SDL2.dll PATHS
        "${SDL2_MSYS2_DIR}/bin"
        "${SDL2_DOWNLOAD_DIR}/bin"
        "${SDL2_LOCAL_DIR}/bin"
        "$ENV{MINGW_PREFIX}/bin"
    )
    if(SDL2_DLL_PATH)
        add_custom_command(TARGET ac_pc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SDL2_DLL_PATH}" "${CMAKE_BINARY_DIR}/bin/"
            COMMENT "Copying SDL2.dll to bin/"
        )
    endif()
endif()

# Static link MinGW runtime (no libgcc_s_dw2-1.dll, libstdc++-6.dll, libwinpthread-1.dll)
# Strip version.o from winpthread to prevent its version resource from overriding app.rc
if(MINGW)
    find_file(WINPTHREAD_LIB libwinpthread.a PATHS ${CMAKE_C_IMPLICIT_LINK_DIRECTORIES} /mingw32/lib NO_DEFAULT_PATH)
    if(NOT WINPTHREAD_LIB)
        find_file(WINPTHREAD_LIB libwinpthread.a)
    endif()
    set(WINPTHREAD_STRIPPED "${CMAKE_CURRENT_BINARY_DIR}/libwinpthread_stripped.a")
    if(WINPTHREAD_LIB AND NOT EXISTS "${WINPTHREAD_STRIPPED}")
        file(COPY_FILE ${WINPTHREAD_LIB} ${WINPTHREAD_STRIPPED})
        execute_process(COMMAND ${CMAKE_AR} d "${WINPTHREAD_STRIPPED}" version.o)
        message(STATUS "Stripped version.o from winpthread: ${WINPTHREAD_STRIPPED}")
    endif()
    target_link_options(ac_pc PRIVATE -static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive "${WINPTHREAD_STRIPPED}" -Wl,-Bdynamic,--no-whole-archive)
endif()

# Hide console window on Windows (GUI subsystem)
# Use -DPC_CONSOLE=ON to keep the console visible for debugging
if(WIN32 AND NOT PC_CONSOLE)
    target_link_options(ac_pc PRIVATE -mwindows)
endif()

# =============================================================================
# Install rules (cmake --install . --prefix <dest>)
# =============================================================================
install(TARGETS ac_pc RUNTIME DESTINATION bin)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/shaders/"
    DESTINATION bin/shaders
    FILES_MATCHING PATTERN "*.vert" PATTERN "*.frag"
)
