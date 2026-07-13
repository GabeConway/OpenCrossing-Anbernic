# Known issues & leads

## Open

- **FPS dips to ~30 walking fast across acre grids** (2026-07-13 device
  test of v0.2.0): overall range is 60↔30 depending on scene; steady areas
  hold 55-60, acre-streaming walks dip to ~30 worst case. Long-term goal:
  60 stable / most of the time (kb/perf.md "Current state"). Next levers:
  P3 VBO orphan decision after re-measure, per-TU -O2 on loader/
  decompression TUs, iso read-ahead (the zero-draw stall class).

- **Inventory-open aspect flicker** (2026-07-13): opening the inventory makes
  the game EFB-capture the frame and redraw it as a background; during the
  handoff emu64's NOOP markers switch `g_pc_widescreen_stretch`
  (0=hor+ → 1=stretch for bg blits → 2=UI pillarbox; reset to 0 each frame
  in pc_gx_begin_frame). The captured letterboxed image and the blit mode
  disagree for 1-2 frames → visible width jump. Where to look:
  emu64.c `dl_G_NOOP` (marker handling), m_play.c ~688/738 (marker emission),
  pc_gx.c GXSetProjection/GXSetViewport mode-2 remap, EFB capture path in
  pc_gx_copy_tex_execute / GXLoadTexObj bypass. Fix needs device A/B.
- **Main-area perf — NEXT TARGET: per-draw GL overhead** (2026-07-13,
  measured on device): steady frames 42ms with gl=25ms at 491-600 draws
  (~40-50µs/draw), tex=0.0 (texture pipeline fully solved), speed 94-100%
  (dynamic fps working). emu64 -O2 experiment SHIPPED SAFE (train passes,
  crashes=0) and improved home area 36→35-40 fps. PERF-tab toggles don't
  matter because draw dispatch, not scene volume, is the cost.
  **P1 GX state-set dedup: SHIPPED + DEVICE-VERIFIED 2026-07-13** (kb/perf.md
  #9) — playtest: much better loading, acres load right, smoother, better 1%
  lows, ~30 fps avg walking while acres load. log.txt PERF numbers (draws,
  gl ms vs 491-600/15-26ms baseline) still worth grabbing next SD mount.
  Triage switch PC_NO_STATE_DEDUP=1.
  **P2 — per-program uniform value shadowing: SHIPPED + DEVICE-VERIFIED
  2026-07-13** (kb/perf.md #10, v0.2.0). Kill switch PC_NO_UNIFORM_SHADOW=1.
  Re-measure (fresh log.txt PERF numbers on the P2 build) before deciding
  on (P3) per-draw glBufferData orphan → one big VBO with offset
  accumulation per frame (fewer/larger draws after P1 may deflate this).
- **One-time 8.7s stall on home menu** (device log frame 606: work=8746ms,
  gl=13ms, tex=0): pure game-side stall — synchronous iso reads
  (pc_disc/pc_dvd fread on SD) and/or decompression in unoptimized decomp
  code during menu/save load. Leads: add timing counters to pc_dvd_read /
  pc_disc reads, consider read-ahead thread or optimizing the decomp's
  decompression TUs (same per-TU -O2 pattern as emu64).
  2026-07-13 P1-build log confirms pattern persists: 3 zero-draw stalls,
  worst 3.2s (work=3222ms, gl≈0, draws=0). Now the top remaining stutter
  class alongside work-dominated moderate stutters (268 of 345 STUTTER
  lines were work-dominated; gl-dominated was 74, of which the worst were
  mid-session shader compiles — seed regrown to 101 configs to kill those,
  kb/perf.md #12).

## Resolved (keep for pattern-matching)

- Intro train black screen → decomp code must build unoptimized (kb/perf.md).
- First-boot menu music stutter → shader seed warmup during splash.
- No audio → 32-bit PipeWire env in launcher (kb/device.md).
- Game running at 57% speed under load → fps_target must be dynamic (6);
  fixed targets tie game logic to render rate.
- Outdoor area locked at 30 fps until a house visit reset it → dynamic-fps
  governor was bistable (batch measurement); fixed with upward probe,
  kb/perf.md #11. If it recurs, check PC_NO_FPS_PROBE handling first.
