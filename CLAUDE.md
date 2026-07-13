# CLAUDE.md

Native Animal Crossing (GameCube decomp) build for Anbernic handhelds —
standalone project since 2026-07-13, bootstrapped from the Dia2809 fork of
flyngmt/ACGC-PC-Port (credited in README). Target: **RG-34XX SP** (Allwinner
H700 Cortex-A53 armhf, Mali-G31 GLES-only, 720x480, muOS + PortMaster).

## Ground rules

- Branches: `main` = releases (CI builds on `v*` tags only — conserve
  Actions minutes), `dev` = daily work. Never tag dev.
- **Never commit ROM material** (`harness/rom/`, SD card contents).
- Decomp game code (`src/`) is vendored: gnu89, UB-dependent (keep the CMake
  UB-guard flags), 32-bit pointer assumptions everywhere. Platform code lives
  in `pc/`.
- After any renderer/texture/shader change: build armhf in Docker and run
  `./harness/smoke.sh armhf` (twice, to also exercise the shader disk cache).
- Launcher sync: `port_files/Animal Crossing.sh` is source of truth;
  keep `portmaster/Animal Crossing.sh` (release zip copy) in sync.

## Knowledge base — read on demand, don't guess

Load the relevant file before working in its area:

| File | Read when |
|------|-----------|
| `kb/device.md` | Deploying to SD, audio/PipeWire, device hardware limits, reading log.txt |
| `kb/renderer.md` | Touching pc_gx*, TEV shaders, shader/texture caches, batching |
| `kb/perf.md` | Any fps/stutter work — what's fixed, how to measure, open ideas, gotchas |
| `kb/build-test.md` | Building (flags! colima gotcha), harness, CI/release/packaging |
| `kb/game.md` | Touching decomp code, emu64, saves, settings enums, texture packs |
| `kb/history.md` | Why things are the way they are; decisions log |

Keep kb/ files updated when the facts change — they are the project memory.

## Quick reference

```bash
# device build (output: pc/build-armhf/bin/AnimalCrossing)
docker run --rm --platform linux/arm/v7 -v "$PWD":/work \
  debian:bookworm bash /work/pc/build-armhf-docker.sh
# boot test with rom
./harness/smoke.sh armhf
# deploy (SD card mounted as ROMS)
cp pc/build-armhf/bin/AnimalCrossing "/Volumes/ROMS/ports/ac-gc/AnimalCrossing" && sync
```

## Status (2026-07-13)

Works on device: rendering, controls, saves, audio. Two perf passes done
(see kb/perf.md). Second pass (dev branch: -O2 for game code + NEON, NEON
decoders, decode budget, draw-call merging) is **awaiting device testing** —
README carries a "not tested end-to-end" notice until a full playthrough.
