# Target device & deployment

## Community-confirmed devices (beyond the dev device)

- **RG28XX on Knulli** (2026-07-14 user report): "played for an hour,
  worked great" — confirms both a second H700 device (640×480 4:3, 2.83"
  screen) AND a second CFW (Knulli, not just muOS). Same binary, no
  changes. Listed in README supported-devices table.
- **RG35XX SP** (2026-07-14 user report): "works great", praised install
  instructions. No analog stick on this device — user needed the in-game
  Control menu toggle **"Use Dpad as Joystick"** to walk. Tip added to
  README. Idea: auto-enable dpad-as-joystick when no analog stick is
  detected at startup (SDL axis count) — removes the one manual step for
  the whole stickless clamshell family.

## Hardware: Anbernic RG-34XX SP

- SoC: Allwinner H700, 4x Cortex-A53 (~1.5 GHz), armv7 userland (armhf)
- GPU: Mali-G31 MP2 — **GLES 3.2 only, no desktop GL**. Slow at dynamic
  branching in shaders (why TEV shaders are runtime-specialized, branchless).
  Slow shader compiler (why the program-binary disk cache exists).
- Screen: 3.4" 720x480. UI must be readable at 2x 8px font scale.
- OS: muOS with PortMaster runtime (armhf). PipeWire audio.

## SD card layout (volume name `ROMS`, FAT — mounts at /Volumes/ROMS on mac)

- `ports/ac-gc/` — `AnimalCrossing` binary, `libs.armhf/` (SDL2 2.26.5,
  libstdc++, libgcc fallbacks; device `/usr/lib32` takes precedence via
  LD_LIBRARY_PATH; GLES intentionally NOT bundled — device Mali driver),
  `rom/` (user's GAFE01 iso), `shaders/` (default.vert/frag), `conf/`.
  Game writes here: `log.txt`, `settings.ini`, `keybindings.ini`, `save/`,
  `shader_cache.bin`.
- `ROMS/Ports/Animal Crossing.sh` — PortMaster launcher. **Source of truth is
  `port_files/Animal Crossing.sh` in the repo**; `portmaster/Animal Crossing.sh`
  (used for release zips) must be kept in sync with it.

## Deploying a new build

```bash
cp pc/build-armhf/bin/AnimalCrossing "/Volumes/ROMS/ports/ac-gc/AnimalCrossing"
sync
```

Only the binary usually needs copying; shaders/launcher only when changed.
Do NOT copy a locally-generated `shader_cache.bin` to the device — binaries
are driver-specific (header driver-hash rejects mismatches, but it's noise).

## Audio (fixed 2026-07-13 — do not regress)

muOS PipeWire from a 32-bit process failed in `pw_loop_new` ("can't make
support.system handle") because the 32-bit SPA plugins weren't found. Fix
lives in the launcher: when the dirs exist it sets
`SPA_PLUGIN_DIR=/usr/lib32/spa-0.2`,
`PIPEWIRE_MODULE_DIR=/usr/lib32/pipewire-0.3`,
`SDL_AUDIODRIVER=pipewire,alsa,dsp`. ALSA alone does NOT work (muOS routes
the ALSA default through the same broken 32-bit pw plugin). The launcher also
dumps an "--- audio diag ---" block into log.txt.

## Reading device logs

`ports/ac-gc/log.txt` after a session. Useful lines:
- `[PC/TEV] Preloaded N shader(s) from disk cache` — shader cache working
- `[PC/TEV] Compiled specialized shader #N` — new TEV config (first-visit)
- `[STUTTER] frame N: total=..ms work=.. swap=.. gl=.. tex=.. draws=..` —
  only with verbose on; tex=ms high → texture decode burst, gl=ms high →
  driver/GL overhead, work high with others low → game logic
- `[PERF] ...` — periodic; needs verbose=1 (settings.ini or --verbose)
