# Known issues & leads

## Open

- **Inventory-open aspect flicker** (2026-07-13): opening the inventory makes
  the game EFB-capture the frame and redraw it as a background; during the
  handoff emu64's NOOP markers switch `g_pc_widescreen_stretch`
  (0=hor+ → 1=stretch for bg blits → 2=UI pillarbox; reset to 0 each frame
  in pc_gx_begin_frame). The captured letterboxed image and the blit mode
  disagree for 1-2 frames → visible width jump. Where to look:
  emu64.c `dl_G_NOOP` (marker handling), m_play.c ~688/738 (marker emission),
  pc_gx.c GXSetProjection/GXSetViewport mode-2 remap, EFB capture path in
  pc_gx_copy_tex_execute / GXLoadTexObj bypass. Fix needs device A/B.
- **Main-area perf** (2026-07-13): ~36 fps render in home area; PERF-tab
  toggles (shadows/acres/particles) barely move it → not render-volume
  bound. Current experiment: emu64.c + emu64_utility.c at -O2 (rest of
  decomp stays unoptimized — see kb/perf.md "-O2 regression").
  Awaiting device verbose log for work/gl/tex attribution.

## Resolved (keep for pattern-matching)

- Intro train black screen → decomp code must build unoptimized (kb/perf.md).
- First-boot menu music stutter → shader seed warmup during splash.
- No audio → 32-bit PipeWire env in launcher (kb/device.md).
- Game running at 57% speed under load → fps_target must be dynamic (6);
  fixed targets tie game logic to render rate.
