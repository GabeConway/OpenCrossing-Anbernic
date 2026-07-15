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

- **Item dupe: save while holding a tool → reload → copy in hands AND
  pockets** (RESOLVED 2026-07-15; opened 2026-07-14, GCI forensics detail
  in git history of this file). Root cause: NOT a pipeline stow —
  `src/game/m_card.c` is excluded from the PC build (pc/CMakeLists.txt
  ~384), so the whole GC `mCD_InitGameStart_bg` pipeline, including the
  decoy-gated equipment clear (~5086), is dead code; the port's
  replacement in pc_m_card.c does no inventory writes. Exhaustive
  enumeration of every `equipment`/`pockets[]` writer in linked code
  proved no load-time stow exists; the save-while-holding state
  (equipment=0x2202, its pocket slot empty) simply survives the PC load —
  GC re-reads the card each start and never surfaces that state — and the
  held copy then gets re-materialized into the first free pocket slot
  in-session. Only non-atomic transaction on the whole surface: the
  submenu's deferred PLAYER-slot grab (`mTG_catch_item_from_table`
  m_tag_ovl.c:2917 arms shared `wait_timer=16`; completion+clear deferred
  to m_tag_ovl.c:8018-8028, cursor-state dependent, timer shared with the
  money-sack flows). Fix: pc_m_card.c `mCD_InitGameStart_bg` normalizes
  at game start — stows held `equipment` via `mPr_SetFreePossessionItem`
  and clears `equipment` in the same step (clear iff stow succeeded;
  pockets full → stays in hands, never destroyed). Held+pocket-copy state
  now unconstructible from a reload. DEVICE-VERIFIED 2026-07-15: user
  repro confirms hands empty + no new dupe after reload; GCI bytes
  confirm equipment=0x0000, shovel count stable (the pre-fix pair from
  the original dupe remains in pockets, as expected).
- Wrong resolution on 640×480 panels until users hand-edited the .sh
  (2026-07-15, RG35XX H report): launcher first-run settings.ini hardcoded
  720×480 (dev-device value). Now the launcher writes no
  window_width/height; the game queries SDL_GetCurrentDisplayMode(0) after
  SDL_Init when settings.ini sets no resolution key and fullscreen=1
  (640×480→preset 2, 720×480→preset 5, CubeXX 720×720→custom; existing
  letterbox/stretch handles aspect). Explicit ini values and in-game menu
  changes still win; saves keep resolution commented while in auto mode so
  cards stay portable across panels. Log:
  `[Settings] Auto-detected display WxH (window_size=N)`. Note: the three
  launcher .sh copies (port_files/, portmaster/, pm-submission/) are
  hand-maintained duplicates — assemble.sh does not copy them.
  DEVICE-VERIFIED 2026-07-15 (RG-34XX SP): fresh ini → 720×480 fullscreen
  window, saved ini carries `# window_width = 720` / `# window_size = 5`
  commented (auto mode persisted). The log line itself was missing from
  log.txt — the log's tail (all gameplay stdout) was truncated by an
  unclean unmount/power-off, so autodetect prints moved to stderr
  (matches pc_main boot-log convention). Lesson: quit via in-game exit
  and eject cleanly before reading device logs, or the buffered tail
  (incl. any [PROF] lines) is lost.
- In-game clock frozen on device: correct at boot/character select, then
  ~1 in-game min per 24 real min (2026-07-15, RG28XX user repro 6:03→6:04
  over 24 min) → u64 overflow in pc_os.c osGetTime():
  `elapsed_ns * GC_TIMER_CLOCK` wraps every 455.5s when
  SDL_GetPerformanceFrequency()=1e9 (Linux CLOCK_MONOTONIC), so the clock
  sawtooths: climbs 7.59 min then snaps back to boot time (24 min real →
  wrapped 73.6s elapsed = exactly the observed 6:04). All gameplay time
  (lbRTC_GetGameTime = OSGetTime + time_delta) sat downstream. Fixed by
  splitting whole seconds from remainder before tick scaling
  ((diff/freq)*CLK + (diff%freq)*CLK/freq) — overflow-free. If clock bugs
  recur, first suspect other cumulative-counter * constant multiplies.
  DEVICE-VERIFIED 2026-07-15: clock tracks real time through a full play
  session ("i think the clock is fixed too" — user, >10 min session).
- Intro train black screen → decomp code must build unoptimized (kb/perf.md).
- First-boot menu music stutter → shader seed warmup during splash.
- No audio → 32-bit PipeWire env in launcher (kb/device.md).
- Game running at 57% speed under load → fps_target must be dynamic (6);
  fixed targets tie game logic to render rate.
- Outdoor area locked at 30 fps until a house visit reset it → dynamic-fps
  governor was bistable (batch measurement); fixed with upward probe,
  kb/perf.md #11. If it recurs, check PC_NO_FPS_PROBE handling first.
