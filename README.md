# Animal Crossing — Native Anbernic Handheld Port

A **native ARM build of Animal Crossing (GameCube)** for Anbernic handhelds, built
and tuned for the **RG-34XX SP** (Allwinner H700, Mali-G31) running **muOS** with
**PortMaster**. This is not emulation: the game's original decompiled C code runs
directly on the handheld's CPU, with a translation layer mapping the GameCube's GX
graphics API onto OpenGL ES 3.2.

This repository contains **no game assets or original assembly whatsoever**. You
must provide your own disc image of the game.

Supported game version: `GAFE01` — Animal Crossing (USA), Rev 0.

## Why this project exists

The excellent [ac-decomp](https://github.com/ACreTeam/ac-decomp) decompilation made
a PC port of Animal Crossing possible, and the ports this project descends from
(see [Credits](#credits)) target x86 desktops with desktop OpenGL. None of them run
on the low-power ARM handhelds this game feels made for — a pocket Animal Crossing
town you can check in on anywhere.

Getting there required work that goes well beyond a recompile, and that is what this
project is about:

- **32-bit ARM (armhf) build** targeting the H700's Cortex-A53 cores, built via a
  Debian bookworm Docker toolchain.
- **OpenGL ES-only rendering path** for the Mali-G31 (no desktop GL on device),
  including runtime-specialized branchless TEV fragment shaders — Mali GPUs are
  extremely slow at the dynamic branching an "uber-shader" approach needs.
- **Shader binary disk cache with boot-time preload**, so shader compilation
  hitches only ever happen the first time a material is seen — every run after
  that, shaders load as precompiled driver binaries before gameplay starts.
- **Handheld performance features**: dynamic FPS target, render-scale, frame
  pacing, distance culling, shadow/particle/acre-draw quality settings, all
  adjustable from an in-game settings menu designed for gamepad use.
- **muOS/PortMaster integration**: launcher script, bundled 32-bit fallback
  libraries, and workarounds for the device's 32-bit PipeWire audio stack.
- **480p-friendly UI**: aspect handling, letterbox/stretch modes, and an overlay
  menu readable on a 3.4" screen.

Upstream work continues to target desktops; this project is its own thing going
forward, focused on making the game genuinely good on handheld hardware.

## Installing on a muOS device

1. Copy the `ports/ac-gc/` folder (binary, `libs.armhf/`, `shaders/`, `conf/`)
   onto the SD card, and `Animal Crossing.sh` into `ROMS/Ports/`.
2. Place your `Animal Crossing.iso` (GAFE01 USA) in `ports/ac-gc/rom/`.
3. Launch **Animal Crossing** from the Ports menu.

The game reads all assets directly from the disc image — no extraction step. Saves,
settings, key bindings, and logs are written next to the binary in `ports/ac-gc/`.

## Building from source (armhf)

Cross-build in Docker from any machine:

```bash
docker run --rm --platform linux/arm/v7 \
  -v "$PWD":/work \
  debian:bookworm bash /work/pc/build-armhf-docker.sh
```

Output: `pc/build-armhf/bin/AnimalCrossing` (32-bit ARM hard-float ELF).

A desktop build (`build_pc.sh`) still works for development and testing; it needs
SDL2 and a C toolchain.

## Settings

Everything is adjustable from the in-game settings menu (or `settings.ini`):

- FPS target (fixed 20–60, unlimited, or **dynamic** — adapts to load)
- Render scale (25–100%) and window/screen size
- Shadow quality, particle quality, acre draw distance, distance culling
- FPS counter overlay (off by default)
- Volume controls, key/button bindings

## Texture packs

Dolphin-format texture packs (XXHash64 DDS) go in `texture_pack/`. The
[community HD texture pack](https://forums.dolphin-emu.org/Thread-animal-crossing-hd-texture-pack-version-23-feb-22nd-2026)
works, but expect a performance cost on handheld GPUs.

## Save data

Saves use the standard GCI format in `save/`, fully compatible with Dolphin.
Drop a Dolphin GCI export there to import an existing town.

## Credits

This project stands on the shoulders of:

- **[ACreTeam / ac-decomp](https://github.com/ACreTeam/ac-decomp)** — the complete
  C decompilation of Animal Crossing that makes any of this possible.
- **[flyngmt/ACGC-PC-Port](https://github.com/flyngmt/ACGC-PC-Port)** — the
  original PC port of the decompilation (GX→OpenGL translation layer).
- **[Dia2809's fork](https://github.com/Dia2809/ACGC-PC-Port)** — Linux support,
  the OpenGL ES renderer, and the ARM branches this project was bootstrapped from.

Animal Crossing is © Nintendo. This project is not affiliated with or endorsed by
Nintendo, and distributes none of its assets.

## AI notice

AI tools (Claude) were used in the porting and optimization work (platform code
only, not the decompiled game code).
