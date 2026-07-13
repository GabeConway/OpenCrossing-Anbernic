# Known issues & leads

## Open

- **Dynamic-fps governor bistable — locks at 30 in scenes that could run
  50-60** (2026-07-13, device report: new outdoor area renders 30 fps; enter
  house → 60; exit → same outdoor area now 60 and stays there). Root cause
  (pc_vi.c): `pc_dynamic_fps_update` feeds on `work_us` = whole batch
  (all frameskip logic-only ticks + render tick). At target 30 the batch is
  2 logic ticks + 1 render, so measured work stays ≥33ms → EMA never drops
  → fps_opt stays ≤30 → self-sustaining, even when a 1-tick/60fps frame
  would cost ~18ms. Cheap interior breaks the loop; state then holds at 60.
  NOT a shader issue (disk cache rules out recompiles). Fix directions:
  normalize work by ticks-in-batch, or estimate 1-tick cost
  (logic_per_tick + render) and target from that, or periodic upward probe
  (every ~2s force target+step and keep it if speed holds 100%).

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
  **P2 — per-program uniform value shadowing: IMPLEMENTED 2026-07-13**
  (kb/perf.md #10), awaiting device test. Generation counters per uniform
  group + per-program uploaded-gen records; shader switch no longer forces
  dirty=ALL. Kill switch PC_NO_UNIFORM_SHADOW=1. Then re-measure before
  deciding on (P3) per-draw glBufferData orphan → one big VBO with offset
  accumulation per frame (fewer/larger draws after P1 may deflate this).
- **One-time 8.7s stall on home menu** (device log frame 606: work=8746ms,
  gl=13ms, tex=0): pure game-side stall — synchronous iso reads
  (pc_disc/pc_dvd fread on SD) and/or decompression in unoptimized decomp
  code during menu/save load. Leads: add timing counters to pc_dvd_read /
  pc_disc reads, consider read-ahead thread or optimizing the decomp's
  decompression TUs (same per-TU -O2 pattern as emu64).

## Resolved (keep for pattern-matching)

- Intro train black screen → decomp code must build unoptimized (kb/perf.md).
- First-boot menu music stutter → shader seed warmup during splash.
- No audio → 32-bit PipeWire env in launcher (kb/device.md).
- Game running at 57% speed under load → fps_target must be dynamic (6);
  fixed targets tie game logic to render rate.
