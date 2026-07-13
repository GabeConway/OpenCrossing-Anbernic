# Building & development

Developer notes. If you just want to play, grab a
[release zip](../../releases/latest) instead — see the install guide in
[README.md](README.md).

## Device build (armhf, Docker)

Cross-build the 32-bit ARM binary from any machine:

```bash
docker run --rm --platform linux/arm/v7 \
  -v "$PWD":/work \
  debian:bookworm bash /work/pc/build-armhf-docker.sh
```

Output: `pc/build-armhf/bin/AnimalCrossing` (ELF 32-bit ARM EABI5 hard-float).
The build dir persists on the host mount, so rebuilds are incremental.

**Colima gotcha:** colima's docker only has the legacy builder, which
*silently ignores* `--platform` when an arm64 base image is already cached —
you end up with an arm64 build that looks fine until it hits the device.
Always verify the container arch:

```bash
docker run --rm --platform linux/arm/v7 debian:bookworm uname -m   # must print: armv7l
```

## Desktop dev build (macOS / Linux)

```bash
./build_pc.sh
```

Needs SDL2 and a C toolchain. Output: `pc/build/bin/AnimalCrossing`. Fastest
iteration loop for gameplay/logic work (64-bit, desktop GL).

## Smoke tests (harness/)

Headless Docker boot tests that launch the game under Xvfb and check it gets
past disc load:

```bash
./harness/smoke.sh armhf    # exact device ABI (slow, QEMU)
./harness/smoke.sh arm64    # near-native, same GLES class as device
```

Put your GAFE01 USA disc image at `harness/rom/Animal Crossing.iso` — the
`harness/rom/` directory is git-ignored and its contents must **never** be
committed. Logs land in `harness/out/<arch>-smoke.log`. See
`harness/README.md` for the full three-tier setup (including one-time
colima/binfmt install).

## Cutting a release

Releases are built by CI (`.github/workflows/release.yml`):

1. Merge `dev` into `main`.
2. Tag on `main`: `git tag vX.Y.Z && git push origin vX.Y.Z`.
3. CI builds the armhf binary under QEMU (slow — up to a couple of hours),
   packages the PortMaster zip via `portmaster/make-package.sh`, and publishes
   a GitHub release with the zip attached.

**Don't tag on `dev`** — the workflow triggers on any `v*` tag regardless of
branch, and releases should only come from `main`. Manual `workflow_dispatch`
runs upload the zip as a build artifact instead of creating a release.
