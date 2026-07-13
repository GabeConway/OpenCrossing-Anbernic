# Known issues & leads

## Open

- **FPS dips to ~30 walking fast across acre grids** (2026-07-13 device
  test of v0.2.0): overall range is 60↔30 depending on scene; steady areas
  hold 55-60, acre-streaming walks dip to ~30 worst case. Long-term goal:
  60 stable / most of the time (kb/perf.md "Current state").
  v0.2.0-log verdict (2026-07-13): per-draw GL cost **29.5µs/draw**
  (unchanged from P1's 29.3 — P2 uniform shadowing didn't move it, so
  the cost is glBufferData orphan + draw dispatch). EVERY sub-35fps run
  is a 487-578-draw scene at gl 18-19ms — real load, no governor locks
  left. ~550 draws × 29.5µs ≈ 16ms of gl per frame = the whole 60fps
  budget. **P3 (one big VBO with per-frame offset accumulation, fewer/
  larger driver calls) is THE lever for the 30fps dips** — halving
  per-draw cost would lift the heavy scenes to ~45. Other levers:
  per-TU -O2 on loader/decompression TUs (264 work stutters, avg 42ms),
  iso read-ahead.

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
  2026-07-13 P1-build log deep-dive: the 3.2s "stall" is frame 3 of BOOT —
  vanilla's intentional 2500ms sleep ("ニンテンドー発生タイムラグまで寝てます")
  plus sound_initial2/init, NOT a gameplay stall. Real gameplay stutter
  classes: (a) 264 work-dominated moderates (avg 42ms total, 50 above
  50ms, max 148ms) — game logic in -O0 decomp; lever = per-TU -O2
  expansion (loader/decompression/m_field TUs); (b) gl-dominated spikes,
  worst were the 24 mid-session shader compiles — fixed by 101-config
  seed (kb/perf.md #12).

## Resolved (keep for pattern-matching)

- Intro train black screen → decomp code must build unoptimized (kb/perf.md).
- First-boot menu music stutter → shader seed warmup during splash.
- No audio → 32-bit PipeWire env in launcher (kb/device.md).
- Game running at 57% speed under load → fps_target must be dynamic (6);
  fixed targets tie game logic to render rate.
- Outdoor area locked at 30 fps until a house visit reset it → dynamic-fps
  governor was bistable (batch measurement); fixed with upward probe,
  kb/perf.md #11. If it recurs, check PC_NO_FPS_PROBE handling first.
