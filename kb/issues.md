# Known issues & leads

## Open

- **One-time 1.4s hang at the dock/beach, first visit** (2026-07-14 device log,
  v0.3.0 build Jul 13 22:35): `[STUTTER] frame 23730: total=1422.4ms
  work=1422.4ms gl=8.4ms tex=0.0ms draws=73` — single isolated frame, session
  otherwise ~60fps (one PERF dip 20.1fps = the window containing it; the
  avg=80.7ms EMA is fully explained by the one spike). Footstep SEs show WOOD
  (0x4204) right up to the hang, GRASS (0x4201) right after → fired while
  on/leaving the dock planks. Systematically ruled out by source trace:
  GPU/shader (gl=8.4ms, zero compiles logged near it), texture decode
  (tex=0.0), runtime disc I/O (acre geometry is RAM-resident from boot via
  pc_assets; forest_1st/2nd.arc mounted to malloc'd ARAM at boot; runtime
  "ARAM DMA" is pure memcpy pc_aram.c:41; audiorom.img 8.3MB fully preloaded
  in pc_neos_init_sync at boot, jaudio runtime bank loads are memcpys on the
  AudioProducer SDL thread; JKRDecomp/JKRDvdRipper callers are boot-only),
  GCI save (only written at session end). Conclusion: a silent one-shot CPU
  burst inside a single game-logic tick in -O0 decomp code (beach item/actor
  born pass, event-manager beach scans ac_event_manager.c:860-907, or similar
  first-visit path), or an external stall (stdout is SD-backed and WALK/TRG
  debug prints fire several lines per frame while dashing). mFI shell
  placement (m_field_info.c:2990-3155) examined and exonerated — loops are
  bounded small. NOT reproducible on 2nd visit (one-time), so instrument
  first: DONE 2026-07-14 — `[PROF]` slow-phase profiler shipped on dev
  (see kb/perf.md Measuring). Next SD-card log with the hang will carry
  `[PROF]` lines naming the phase (and actor profile id if it's a ct/mv).
  Then per-TU -O2 (emu64 template) on m_field_info.c / m_field_make.c /
  ac_event_manager.c + actor spawn path — attacks this AND the sustained
  acre-streaming dips regardless of which candidate wins.

- **FPS below 60 in heavy scenes** (updated 2026-07-13 post-P4, v0.3.0):
  P4 (strip conversion + whole-batch CPU cull, kb/perf.md #14)
  DEVICE-VERIFIED: avg 56.4 fps, median 59.3, 78% ≥55; worst sustained
  dips 41-44 fps during heaviest acre streaming. **GL is no longer the
  bottleneck** — gl avg 5.4ms, all 75 gameplay stutters work-dominated
  (median 24ms, max 114ms), zero gl-dominated. Remaining work to 60
  stable, in order of expected value:
  (a) **per-TU -O2 on loader/decompression + m_field/actor TUs** —
  work-dominated stutters and the 41-44 fps dips are game logic in -O0
  decomp code (emu64 -O2 template proven safe, kb/perf.md #8);
  (b) **iso read-ahead thread** — sync SD reads inside work spikes;
  (c) **CPU pre-transform at accumulation** — matrix loads are 41% of
  batch breaks and merged=0 all session; pre-transform would let GXBegin
  merging finally fire (tex breaks 30% remain, so gains capped — measure
  first). Cull-scan cost at ~500 submitted batches is part of the dip
  frames; a cheap object-space AABB cache keyed on batch identity could
  cut it if profiling says so.

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
