# CLAUDE.md

## What this is

**Standalone project** (as of 2026-07-13): native Animal Crossing build for
Anbernic handhelds — its own thing going forward, no longer tracking upstream.
Originally bootstrapped from the Animal Crossing GameCube decomp PC port
(flyngmt/ACGC-PC-Port via the Dia2809 fork which added Linux + GLES; both
credited in README.md). Target device: **Anbernic RG-34XX SP**: Allwinner
H700, Mali-G31 (GLES only), 720x480 screen, running **muOS** with
**PortMaster** (armhf runtime).

## Repository layout

- This checkout (`ACGC-PC-Port`): branch `arm64-local`, tracks `dia/arm64`.
  Contains the test harness and macOS build fixes.
- `../ACGC-PC-Port-armhf`: linked git worktree on branch `armhf-local`
  (tracks `dia/armhf`). This is where the device build lives. GitHub Desktop
  cannot open linked worktrees — use the CLI for that one.
- Remotes: `origin` = GabeConway/ACGC-PC-Port (private mirror; not a GitHub
  fork because forks of public repos cannot be private), `dia` = Dia2809 fork,
  `upstream` = flyngmt original.

## Building for the device (armhf)

```bash
docker run --rm --platform linux/arm/v7 \
  -v "$PWD/../ACGC-PC-Port-armhf":/work \
  debian:bookworm bash /work/pc/build-armhf-docker.sh
```

Output: `pc/build-armhf/bin/AnimalCrossing` (ELF 32-bit ARM EABI5 hard-float).
Incremental: the build dir persists on the host mount.

**Gotcha:** colima's docker has only the legacy builder (no buildx) and it
silently ignores `--platform` when an arm64 base image is cached. The
`acgc-smoke-deps:armhf` image had to be rebuilt via `docker commit` from a
container started with an explicit `--platform linux/arm/v7`. Always verify
with `docker run --rm <img> uname -m` → must print `armv7l`.

## Test harness (`harness/`)

Three tiers — see `harness/README.md`. Quick reference:

- `./harness/run-native.sh` — macOS native build, fastest iteration.
- `./harness/smoke.sh arm64|armhf` — headless Docker boot test with the rom;
  writes `harness/out/<arch>-smoke.log`; exit 0 = booted past disc load.
- Rom lives in `harness/rom/Animal Crossing.iso` (GAFE01 USA, git-ignored —
  **never commit; also never commit anything under `harness/rom/`**).

Both arm64 and armhf smoke tests pass and reach the title-screen logo.

## Device deployment (SD card)

muOS SD card (volume name `ROMS`, FAT):

- `ports/ac-gc/` — `AnimalCrossing` binary, `libs.armhf/` (SDL2 2.26.5,
  libstdc++, libgcc as fallbacks; device `/usr/lib32` takes precedence via
  LD_LIBRARY_PATH; GLES intentionally NOT bundled — device Mali driver),
  `rom/`, `shaders/`, `conf/`; game writes `log.txt`, `settings.ini`,
  `keybindings.ini`, `save/` here.
- `ROMS/Ports/Animal Crossing.sh` — PortMaster launcher. **Source of truth
  is `port_files/Animal Crossing.sh` in the armhf worktree** — keep the two
  in sync when editing either.
- `settings.ini`: `fps_target = 6` means "dynamic" (enum in
  `pc/include/pc_settings.h`), not 6 fps.

## Current status (2026-07-13)

Working on device: boots to menu, Mali-G31 GLES 3.2 rendering, controls,
settings, saves, **audio** (fixed by the launcher pointing 32-bit PipeWire at
`/usr/lib32` plugin dirs: `SPA_PLUGIN_DIR=/usr/lib32/spa-0.2`,
`PIPEWIRE_MODULE_DIR=/usr/lib32/pipewire-0.3`,
`SDL_AUDIODRIVER=pipewire,alsa,dsp`). Audio stays good even at low fps.

## Performance systems (added 2026-07-13, tuning first-visit fps dips)

All in the armhf worktree, `pc/src/`:

- **Per-program uniform-location cache** (`pc_gx_tev.c` cache entries carry
  `PCGXUniformLocs`, filled once at link; `pc_gx.c` copies the struct on
  shader switch instead of ~150 `glGetUniformLocation` driver calls).
  `pc_gx_tev_last_locs` points at the locs of the last-returned program.
- **Shader binary disk cache** (`shader_cache.bin` next to settings.ini;
  GLES builds only, `#ifdef SHADER_DISK_CACHE`). Header: magic/version/
  driver-hash (renderer+version strings — driver update invalidates). New
  compiles append; boot preloads all via `glProgramBinary` → first-visit
  Mali compile hitches only happen on the very first run per config.
  `PC_NO_SHADER_CACHE` env var disables.
- **Texture cache hash index** (`pc_gx_texture.c`): 4096-slot open-addressed
  index over the 2048-entry cache; replaced the per-`GXLoadTexObj` linear
  scan. Bulk-only deletion (eviction rebuilds index, invalidate clears).
- **Boot splash** (`pc_overlay_boot_splash("LOADING...")` from
  `pc_platform_init`; overlay now inits before pc_gx so text shows during
  shader preload + disc load instead of a black screen).

FPS counter overlay already existed: settings menu item, `show_fps` in
settings.ini, off by default.

Remaining known fps dip cause (unfixed): burst CPU texture decode + upload
when entering a new area (dozens of cache misses in one frame). Ideas:
decode-ahead on acre transition, or spread uploads across frames.
