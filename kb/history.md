# Project history & decisions

- **2026-07-12** — Project started as a port attempt of flyngmt/ACGC-PC-Port
  (Animal Crossing GC decomp PC port) to the RG-34XX SP. Chose the
  **Dia2809 fork** as base: it already had Linux, GLES 3.2 renderer
  (`-DPC_USE_GLES=ON`), and arm64/armhf branches. Not in the PortMaster
  catalog (checked all 1326 ports). Private mirror created (not a GitHub
  fork — forks of public repos can't be private).
- **2026-07-12** — Test harness built (Docker smoke tests arm64+armhf via
  colima/qemu, macOS native run). macOS build fixes (Apple ld guards, cKF
  stubs). armhf emu64 fix: `mw_data` → `moveword->data` (emu64.c:5537).
- **2026-07-13 (morning)** — First device boot: rendering, controls, saves
  worked; audio dead. Root cause: 32-bit PipeWire couldn't find SPA plugins;
  ALSA fallback routed through the same broken plugin. Fixed via launcher env
  (SPA_PLUGIN_DIR/PIPEWIRE_MODULE_DIR → /usr/lib32, SDL_AUDIODRIVER list).
- **2026-07-13** — **Pivot decision (maintainer)**: this is its own project now —
  "native Animal Crossing build for Anbernic" — not a tracking fork.
  Upstream/Dia credited in README but development is independent.
- **2026-07-13** — Repo consolidation: renamed to
  `GabeConway/animal-crossing-anbernic`; branches arm64-local/armhf-local/
  master merged into single `main` (harness/portmaster/macOS fixes harvested
  from arm64-local; armhf-local was the base since the device build is the
  product); worktree removed; local dir renamed. `dia`/`upstream` remotes
  kept read-only. Later same day: `dev` branch created — main = releases
  (CI on v* tags), dev = daily work, to conserve Actions minutes.
- **2026-07-13** — Perf pass 1 (uniform-loc cache, texture hash index,
  shader binary disk cache + boot preload, boot splash). Device feedback:
  audio good, minor first-boot menu stutter that self-resolved (cache
  warming). FPS counter option existed but hid in DEBUG tab → moved to VIDEO.
- **2026-07-13** — Perf pass 2 (the -O0 discovery + -O2/NEON flags, NEON
  decoders, decode budget, draw-call merging), CI release workflow,
  noob-friendly README, BUILDING.md, kb/ knowledge split.
- **2026-07-13 (later)** — Perf pass 3 shipped as **v0.2.0** (GX state
  dedup, per-program uniform shadowing, dynamic-fps upward probe, seed
  regrown to 101 configs). Post-release: streaming VBO (P3) v1
  black-screened the Mali blob (SubData + per-batch attrib respec —
  silent, fine on Mesa); v2 rework (glMapBufferRange UNSYNC + BaseVertex
  draws + glGetError probe with auto-fallback) device-verified. Lesson:
  Mali blob tolerates zero per-draw attrib-pointer churn; always ship a
  runtime kill switch + self-healing fallback with renderer experiments.
  Measurement across passes: per-draw cost pinned at ~30µs regardless of
  buffer strategy → future perf work targets draw COUNT. README gained
  the H700 "likely working devices" table.

## Standing decisions

- Device build (armhf) is the product; desktop builds exist for development.
- Never commit ROM material (`harness/rom/`, card contents).
- README carries "not tested end-to-end" status until a full playthrough
  happens on device.
- AI notice stays in README (platform code only, not decomp game code).
