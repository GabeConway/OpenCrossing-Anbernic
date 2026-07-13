# PortMaster package — Animal Crossing (ac-gc)

Zip layout (what `make-package.sh` assembles, and what CI releases contain):

```
Animal Crossing.sh        <- launcher: copy to ROMS/Ports/ on the SD card
ports/
  ac-gc/
    AnimalCrossing        <- armhf binary (pc/build-armhf/bin/AnimalCrossing)
    shaders/              <- default.vert / default.frag (from pc/shaders/)
    rom/<your dump>.iso   <- user-supplied GAFE01 USA disc image
    libs.armhf/           <- fallback lib dir (empty; device /usr/lib32 wins)
    conf/                 <- created at first run
```

Assemble a zip with `./make-package.sh` after the armhf build exists
(optionally: `./make-package.sh <binary> <output.zip>`).

The launcher script here must stay in sync with
`port_files/Animal Crossing.sh` (the source of truth) — copy it over when
either changes.

Device notes (RG-34XX SP):
- 720x480 screen; launcher seeds settings.ini with fullscreen 720x480, msaa=0,
  vsync=1, dynamic FPS target. All tweakable in the in-game settings menu.
- If shader-compile hitches appear on the Mali-G31 driver, try `--uber-shader`
  (add to the launcher exec line).
- Diagnostics land in ports/ac-gc/log.txt.
