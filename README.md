# OpenCrossing-Anbernic

**OpenCrossing-Anbernic** is a native ARM port of the Animal Crossing (GameCube)
decompilation for Anbernic handhelds, built and tuned for the **RG-34XX SP**
(Allwinner H700, Mali-G31) running **muOS** with **PortMaster**. This is not
emulation: the game's original decompiled C code runs directly on the handheld's
CPU, with a translation layer mapping the GameCube's GX graphics API onto
OpenGL ES 3.2.

Community-confirmed working on other H700 devices too — including the
**RG28XX running Knulli** (reported after an hour of play with no issues).

This repository contains **no game assets or original assembly whatsoever**. You
must provide your own legally obtained disc image of the game.

Supported game version: `GAFE01` — Animal Crossing (USA), Rev 0.

> ## Disclaimer
>
> This project is not affiliated with, endorsed by, or sponsored by Nintendo.
> "Animal Crossing" and all related names and marks are trademarks of
> Nintendo; they are used here only to factually describe compatibility.
> No game assets, ROM contents, or original code ship with this repository —
> you must supply your own disc image of a game you own. All other trademarks
> are the property of their respective owners.

> ## ⚠️ Project status
>
> This port is **actively developed** and **has not been tested end-to-end
> yet**. It boots, renders, and saves on real hardware, but expect rough
> edges — and **save often**. Bug reports are welcome (attach
> `ports/ac-gc/log.txt`).
>
> **Performance (v0.3.0, measured on an RG-34XX SP):** ~56 fps average
> (median ~60, 78% of samples ≥55) at ~100% game speed, dipping to the
> low 40s during the heaviest acre streaming.

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

## Supported devices

Developed and tested on the **RG-34XX SP**. Every other Anbernic in muOS's
supported lineup shares the same Allwinner H700 chip (quad Cortex-A53 +
Mali-G31), so the same binary and rendering path should run on all of them.
A community report also confirms the port runs under **Knulli** (another
PortMaster-capable CFW), not just muOS:

| Device | Screen | Notes |
|---|---|---|
| RG-34XX SP | 720×480 (3:2) | ✅ Tested — the development device |
| RG34XX | 720×480 (3:2) | Same screen and chip as the SP; safest bet |
| RG35XX SP | 640×480 (4:3) | ✅ Community-tested — works great; see D-pad tip below |
| RG35XX H | 640×480 (4:3) | ✅ Community-tested on **modded stock OS** — works; see button note below |
| RG35XX Plus / 2024 / Pro | 640×480 (4:3) | Native 4:3 — the game's original aspect |
| RG40XX H / RG40XX V | 640×480 (4:3) | Same as above on a 4" panel |
| RG28XX | 640×480 (4:3) | ✅ Community-tested on **Knulli** — an hour of play, no issues reported |
| RG CubeXX | 720×720 (1:1) | Square panel — expect letterboxing |

The launcher and settings already handle 4:3 output and letterbox/stretch
modes, so no configuration should be needed beyond the normal install. If you
try one of these, please report the result (working or not) in an issue and
attach `ports/ac-gc/log.txt` — a single log is enough to move a device out of
the "untested" column.

**Tip for devices without an analog stick (or if walking doesn't work):**
press **Select** to open the in-game menu, go to **Control**, and toggle on
**"Use Dpad as Joystick"**. (Thanks to the RG35XX SP tester for this one.)

**Note for modded stock OS:** the **Select** and **Menu** keys are reversed
(true for all PortMaster games on modded stock OS) — press the
**Menu/Function** key for the in-game settings menu, and **Select+Start** to
exit the game. (Thanks to the RG35XX H tester.)

Non-H700 devices (older RG35XX models on other chips, RK3566 devices, etc.)
are **not** expected to work with the release builds.

## Installing on a muOS device

No build tools needed — everything except the game itself comes in the release
zip. You need: your Anbernic device running muOS with PortMaster installed, a
computer with an SD card reader, and **your own legally-dumped copy** of
Animal Crossing (USA) as an `.iso` file.

1. **Download the release zip.** Go to the
   [latest release](../../releases/latest) and download
   `opencrossing-anbernic-vX.Y.Z-armhf.zip` (under "Assets").
2. **Unzip it** on your computer. Inside you'll find a folder called `ports`
   and a file called `Animal Crossing.sh`.
3. **Plug the device's SD card into your computer.** Power the handheld off,
   pop out the SD card, and connect it to the computer with a card reader.
4. **Copy the files onto the card:**
   - Copy the whole `ports` folder to the **root** of the SD card (the top
     level, next to folders like `ROMS`). If the computer asks whether to
     merge or replace an existing `ports` folder, choose **merge**.
   - Copy `Animal Crossing.sh` into the `ROMS/Ports/` folder on the card.
5. **Add your game dump.** Copy your own `Animal Crossing.iso` (USA version,
   game ID `GAFE01`) into `ports/ac-gc/rom/` on the card. This port ships no
   game data — without your disc image, nothing will start.
6. **Eject the card safely**, put it back in the device, power on, and launch
   **Animal Crossing** from the **Ports** menu.

The **first launch takes noticeably longer** — the game is building its shader
cache for your device's GPU. Later launches are much faster.

The game reads all assets directly from the disc image — no extraction step.
Saves, settings, key bindings, and logs are written next to the binary in
`ports/ac-gc/`.

**Troubleshooting**

- **Black screen / instant exit:** check that your iso is in
  `ports/ac-gc/rom/` and is the USA `GAFE01` version. Any filename ending in
  `.iso`, `.gcm`, or `.ciso` works.
- **No audio:** quit and relaunch the game once — the audio stack sometimes
  needs a second start after first install.
- **Reporting a bug:** attach `ports/ac-gc/log.txt` from the SD card — it's
  rewritten on every launch and contains the diagnostics we need.

## Releases & branches

- `main` is the release branch — tagged versions (`vX.Y.Z`) are built into
  ready-to-install zips by CI and published on the
  [Releases page](../../releases).
- `dev` is daily development and may be broken at any time.

## Building from source (armhf)

Grabbing a [release zip](../../releases/latest) is the easy path — building
from source is only needed for development. Cross-build in Docker from any
machine:

```bash
docker run --rm --platform linux/arm/v7 \
  -v "$PWD":/work \
  debian:bookworm bash /work/pc/build-armhf-docker.sh
```

Output: `pc/build-armhf/bin/AnimalCrossing` (32-bit ARM hard-float ELF).

A desktop build (`build_pc.sh`) still works for development and testing; it needs
SDL2 and a C toolchain. See [BUILDING.md](BUILDING.md) for the full developer
workflow (desktop builds, smoke tests, release tagging).

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

Saves use the standard GCI format, fully compatible with Dolphin (both
directions). They live next to the game binary:

```
ports/ac-gc/save/card_a/   ← memory card Slot A (the game saves here)
ports/ac-gc/save/card_b/   ← memory card Slot B
```

### Managing saves

Close the game first, then manage the `.gci` files directly (from the device's
file manager, or with the SD card in a computer):

- **Back up a town** — copy the `.gci` file(s) out of `save/card_a/`
  somewhere safe. Do this before updating to a new release.
- **Delete a town / start fresh** — delete the `.gci` file(s) in
  `save/card_a/`. The game creates a new save on next boot, like a fresh
  memory card.
- **Import an existing town** — drop a Dolphin GCI export into
  `save/card_a/`.
- **Restore a backup** — copy the backed-up `.gci` back into `save/card_a/`,
  replacing what's there.

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
