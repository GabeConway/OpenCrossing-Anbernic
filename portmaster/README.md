# PortMaster package — Animal Crossing (ac-gc)

Layout to copy onto the device SD card (muOS/Knulli/Rocknix `ports` roms dir):

```
ports/
  Animal Crossing.sh      <- launcher (this folder)
  ac-gc/
    AnimalCrossing        <- armhf binary (from harness/smoke.sh armhf build,
                             pc/build-armhf/bin/AnimalCrossing in the armhf worktree)
    shaders/              <- default.vert / default.frag
    rom/<your dump>.iso   <- user-supplied GAFE01 USA disc image
    conf/                 <- created at first run
```

Assemble a zip with: `./make-package.sh` (run after the armhf build exists).

Device notes (RG-34XX SP):
- 720x480 screen; launcher seeds settings.ini with fullscreen 720x480, msaa=0,
  vsync=1, dynamic FPS target. All tweakable in the in-game settings menu.
- If shader-compile hitches appear on the Mali-G31 driver, try `--uber-shader`
  (add to the launcher exec line).
- Diagnostics land in ports/ac-gc/log.txt.
