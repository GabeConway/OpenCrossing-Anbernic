# OpenCrossing-Anbernic

Native ARM port of the Animal Crossing (GameCube) decompilation for Anbernic
H700 handhelds (RG-34XX SP, RG35XX/RG40XX family, RG28XX, CubeXX) running
muOS, Knulli, or other PortMaster-capable OSes. **This is not emulation**: the
game's original decompiled C code runs directly on the handheld's CPU, with a
translation layer mapping the GameCube's GX graphics API onto OpenGL ES 3.2.
Built on the [ac-decomp](https://github.com/ACreTeam/ac-decomp) project and
the PC ports it enabled — see [Credits](#credits).

Supported game version: `GAFE01` — Animal Crossing (USA), Rev 0.

> ## Disclaimer
>
> This project is not affiliated with, endorsed by, or sponsored by Nintendo.
> "Animal Crossing" and all related names and marks are trademarks of
> Nintendo; they are used here only to factually describe compatibility.
> No game assets, ROM contents, or original code ship with this repository —
> you must supply your own legally obtained disc image of a game you own.
> All other trademarks are the property of their respective owners.

> **Project status:** actively developed, not yet tested end-to-end — it
> boots, renders, and saves on real hardware, but expect rough edges and
> **save often**. ~56 fps average at full game speed on the RG-34XX SP
> (v0.3.0), dipping to the low 40s during heavy acre streaming. Bug reports
> welcome — attach `ports/ac-gc/log.txt`.

## Install (muOS / PortMaster)

No build tools needed. You need: a PortMaster-capable device, a computer with
an SD card reader, and **your own legally-dumped copy** of Animal Crossing
(USA) as an `.iso` file.

1. **Download the release zip.** Go to the
   [latest release](../../releases/latest) and download
   `opencrossing-anbernic-vX.Y.Z-armhf.zip` (under "Assets").
2. **Unzip it** on your computer. Inside you'll find a folder called `ports`
   and a file called `Animal Crossing.sh`.
3. **Plug the device's SD card into your computer.**
4. **Copy the files onto the card:**
   - Copy the whole `ports` folder to the **root** of the SD card (next to
     folders like `ROMS`). If asked whether to merge or replace an existing
     `ports` folder, choose **merge**.
   - Copy `Animal Crossing.sh` into the `ROMS/Ports/` folder on the card.
5. **Add your game dump.** Copy your own `Animal Crossing.iso` (USA, game ID
   `GAFE01`) into `ports/ac-gc/rom/`. Without your disc image, nothing will
   start.
6. **Eject safely**, put the card back, power on, and launch
   **Animal Crossing** from the **Ports** menu.

The **first launch takes noticeably longer** — the game is building its
shader cache. Later launches are much faster. Assets are read straight from
the disc image (no extraction); saves, settings, and logs live in
`ports/ac-gc/`.

### Troubleshooting

- **Black screen / instant exit:** check that your iso is in
  `ports/ac-gc/rom/` and is the USA `GAFE01` version. `.iso`, `.gcm`, and
  `.ciso` all work.
- **No audio:** quit and relaunch once — the audio stack sometimes needs a
  second start after first install.
- **Can't walk / no analog stick:** current builds detect stickless
  controllers and enable D-pad walking automatically. On older builds (or
  to override): press **Select** → **Control** → set
  **"Use Dpad as Joystick"**.
- **Wrong resolution:** the port auto-detects your panel on launch; to force
  one, uncomment `window_width` / `window_height` in
  `ports/ac-gc/settings.ini`.
- **Modded stock OS:** Select and Menu keys are reversed (all PortMaster
  games there) — **Menu/Function** opens settings, **Select+Start** exits.
- **Lid sleep/wake weirdness (muOS clamshells):** known muOS firmware quirk,
  not the port — press the power button to wake.
- **Reporting a bug:** attach `ports/ac-gc/log.txt` — it's rewritten every
  launch and has the diagnostics we need.

## Supported devices

Developed on the **RG-34XX SP**; every H700 Anbernic shares the same chip
(quad Cortex-A53 + Mali-G31), so the same binary should run on all of them:

| Device | Screen | Notes |
|---|---|---|
| RG-34XX SP | 720×480 (3:2) | ✅ Tested — the development device |
| RG34XX | 720×480 (3:2) | Same screen and chip as the SP; safest bet |
| RG35XX SP | 640×480 (4:3) | ✅ Community-tested — works great |
| RG35XX H | 640×480 (4:3) | ✅ Community-tested on **modded stock OS** |
| RG35XX Plus / 2024 / Pro | 640×480 (4:3) | ✅ Community-tested on **Knulli** — "perfectly stable" |
| RG40XX V | 640×480 (4:3) | ✅ Community-tested on **Knulli** (two reports) |
| RG40XX H | 640×480 (4:3) | ✅ Community-tested — "works seamless" |
| RG28XX | 640×480 (4:3) | ✅ Community-tested on **Knulli** |
| RG CubeXX | 720×720 (1:1) | Square panel — expect letterboxing |

Trying an untested one? Report the result (working or not) in an issue with
`ports/ac-gc/log.txt` attached. Non-H700 devices (older RG35XX models,
RK3566, etc.) are **not** expected to work.

## FAQ

- **Does the Deluxe mod work?** Not yet — technically possible but a large
  amount of work; don't expect it soon.
- **Can other GameCube games be ported this way?** Only games with a complete
  decompilation — see [decomp.dev](https://decomp.dev). A finished decomp
  makes a port possible, not guaranteed.
- **How much RAM?** Runs comfortably on both 2GB (RG34XX family) and 1GB
  (RG35XX / RG28XX family) devices.
- **Desktop Linux / PC?** That's the upstream project's target — see
  [Credits](#credits). This fork focuses on H700 handhelds.

## Settings, saves, texture packs

- **Settings:** everything is adjustable in the in-game settings menu (or
  `settings.ini`) — FPS target (fixed / unlimited / dynamic), render scale,
  shadow/particle/acre quality, distance culling, volume, bindings.
- **Saves:** standard GCI format, fully Dolphin-compatible both directions.
  Slot A is `ports/ac-gc/save/card_a/`, Slot B `save/card_b/`. With the game
  closed, manage `.gci` files directly: copy out = backup, delete = fresh
  town, drop a Dolphin export in = import. Back up before updating.
- **Texture packs:** Dolphin-format (XXHash64 DDS) in `texture_pack/`. The
  [community HD pack](https://forums.dolphin-emu.org/Thread-animal-crossing-hd-texture-pack-version-23-feb-22nd-2026)
  works, with a performance cost on handheld GPUs.

## Building & branches

`main` is the release branch (tagged `vX.Y.Z`, CI-built zips on the
[Releases page](../../releases)); `dev` is daily development and may be
broken. Building is only needed for development — cross-build in Docker from
any machine:

```bash
docker run --rm --platform linux/arm/v7 \
  -v "$PWD":/work \
  debian:bookworm bash /work/pc/build-armhf-docker.sh
```

Output: `pc/build-armhf/bin/AnimalCrossing`. A desktop build (`build_pc.sh`)
also works for development; see [BUILDING.md](BUILDING.md).

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

AI tools (Claude) were used in the porting and optimization work (platform
code only, not the decompiled game code).
