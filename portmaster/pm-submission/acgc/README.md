# Animal Crossing (GameCube decompilation)

Native ARM port of Animal Crossing for PortMaster handhelds. This is **not
emulation**: the decompiled game code (from the ac-decomp project) is compiled
directly for the device, with a translation layer mapping the GameCube's GX
graphics API onto OpenGL ES.

## Requirements

- Your own legally dumped **Animal Crossing (USA, GAFE01)** disc image, named
  `Animal Crossing.iso`, placed in `ports/acgc/rom/`.
- No game assets are included in this package.

## Controls

| Button | Action |
|--------|--------|
| D-pad / Left stick | Move |
| A | Confirm / Interact |
| B | Cancel / Run |
| X / Y | Inventory shortcuts |
| Start | Menu |
| Select | Port settings overlay (FPS counter, performance, controls) |

## Notes

- First launch precompiles shaders during the LOADING screen (slower once).
- Saves are standard GCI files in `ports/acgc/save/` — Dolphin-compatible in
  both directions.
- In-game settings menu (Select) exposes FPS target (dynamic recommended),
  render scale, shadows/particles, and control remapping.
- Log file for bug reports: `ports/acgc/log.txt`.

## Thanks

- **ACreTeam** — the Animal Crossing decompilation (ac-decomp)
- **flyngmt** — the original PC port
- **Dia2809** — Linux + GLES groundwork
- Source & issues: https://github.com/GabeConway/OpenCrossing-Anbernic

## Building

See BUILDING.md in the source repository. armhf binary is produced by a
Debian bookworm Docker cross-build (`pc/build-armhf-docker.sh`).
