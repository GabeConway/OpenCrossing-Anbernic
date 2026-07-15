# Local test harness (macOS host)

Three tiers, fastest feedback first:

| Tier | What | Speed | Fidelity |
|------|------|-------|----------|
| 1 | Native macOS build (`pc/build/bin/AnimalCrossing`) | full speed | 64-bit, desktop GL — gameplay/logic checks |
| 2 | Linux arm64 + GLES 3.2 in Docker (native arch via colima) | near-native | same GL stack class as device (GLES3), 64-bit |
| 3 | Linux armhf (32-bit ARM) in Docker via QEMU | slow | exact device ABI (muOS PortMaster is armhf) |

A GameCube disc image (GAFE01 USA, `.iso`/`.gcm`/`.ciso`) must be placed in `harness/rom/`
(git-ignored). It is mounted read-only into containers; never committed.

## Scripts

- `run-native.sh` — build (if needed) and play the macOS binary windowed.
- `run-gles-arm64.sh` — interactive tier-2 run under XQuartz (needs `xhost +localhost`, XQuartz "Allow connections from network clients").
- `smoke.sh [arm64|armhf]` — headless boot check in Docker: launches the game under Xvfb,
  waits for frames, dumps `harness/out/<arch>-smoke.log`. Exit 0 = booted past disc load.
- `check-launcher-sync.sh` — verifies the three hand-maintained launcher `.sh`
  copies haven't drifted (ignores the intentional GAMEDIR/PORT_32BIT
  differences). Run before packaging a release.
- `inspect-gci.py <file.gci>` — dumps equipment + pockets from a save for
  forensics (see kb/issues.md shovel-dupe entry for the layout).

## One-time setup

```bash
brew install colima docker
colima start --cpu 4 --memory 8
docker run --rm --privileged tonistiigi/binfmt --install arm   # for armhf tier
```
