# Known issues & leads

## Open

- **Redd never sends the visit letter** (2026-07-19 user report; user
  hadn't checked prerequisites). Mechanics (traced): special events are
  scheduled in `init_special_event` (m_event.c:865) — RNG seeded from RTC
  (`player_id + (year-month) + day + hour`, :963) with the gap between
  events derived from `day + month * sec` (:980); when the event day
  arrives the event manager drops the sale leaflet in the mailbox
  (`aEvMgr_actor_regist_broker_handbill`, ac_event_manager.c:130, mail
  type mMl_TYPE_BROKER_SALE_LEAFLET) — no player-action prerequisite
  found in the chain, it is purely schedule-driven. PORT SUSPECT: the
  seed/gap math consumes live RTC fields, and until the osGetTime u64
  wrap fix (2026-07-15, v0.4.0) the PC clock sawtoothed every 455.5s —
  degenerate RTC values plausibly starved/pushed out the whole
  special-event schedule. ACTION: have reporters confirm build version;
  retest on v0.4.0+ across several real-time days before digging further.
  Sale Day (post-Thanksgiving) force-schedules a Redd sale
  (m_event.c:994) — a quick calendar-set test. INSTRUMENTED 2026-07-19:
  `[PC] Special event scheduled: type=N start=M/D end=M/D (today M/D)`
  logs whenever the scheduler rolls — next device log shows whether/when
  Redd (type=BROKER_SALE) is on the calendar.

- **Villagers "fishing on land" during the fishing tournament**
  (2026-07-19 user report). Tourney flow: `anglingtournament_start`
  (ac_event_manager.c:2722) reserves the pool block and spawns 5 NPCs;
  placement goes through `search_free_unit` (:714) whose seed again uses
  live RTC (`month*day + sec + (hour+cur)*3 + seed*9`, :728), and
  `be_flat_unit` (:1586) converts block/unit → world pos WITHOUT
  validating the tile is water (GC never needed to — sane seeds kept
  positions in the reserved pool block). Same port suspect as the Redd
  entry: pre-v0.4.0 frozen/sawtoothing clock → degenerate seeds →
  positions GC could never roll. ACTION: retest on v0.4.0+ during a
  tournament; if it still happens, add a water-attribute check
  (mCoBG attribute lookup) before accepting a wade position.
  INSTRUMENTED 2026-07-19: `[PC] EvMgr wade place: event=N actor=0x....
  block=(x,z) unit=(x,z)` logs every wade placement — compare against
  the pool block on the town map.

- **Green diary furniture renders hot pink (missing-texture magenta)**
  (2026-07-19 user report): one diary recolor ("listed as green") draws in
  the placeholder pink used for graphical glitches; other recolors fine.
  Furniture recolors share one texture and differ by TLUT palette, so the
  lead is the TLUT/recolor path, not the texture itself: emu64
  dl_G_LOADTLUT reuse-detection (emu64.c:3841-3855 first-word heuristic —
  a palette whose first u16 matches the cached one but differs later would
  be wrongly kept), TLUT-keyed texture cache in pc_gx_texture.c
  (GXLoadTexObj tlut_key/tlut_hash_key), or an unhandled palette format
  variant. Needs the item id + repro save to pin down.
  INSTRUMENTED 2026-07-19: `[PC/TEX] C4/C8 texture WxH draws with EMPTY
  tlut slot N` (pc_gx_texture.c GXLoadTexObj, capped at 16 lines) fires
  if a palette item draws before its TLUT is loaded — grep the device
  log right after reproducing the pink item.

- **Design Editor SIGSEGV (upstream Dia2809/ACGC-PC-Port#18) — our code
  path looks NOT affected; needs a device test to confirm** (2026-07-19,
  analysis corrected same day). Upstream gdb trace shows dl_G_LOADTLUT
  with tlut_name=198906/count=29732 indexing 16-entry TLUT arrays → OOB →
  prbuf corruption → crash in pc_gx_flush_vertices. In OUR tree that
  value is unreachable: the type==2 path reads tlut_name through the
  4-bit bitfield (gbi_extensions.h:455, TARGET_PC-reversed layout) so it
  can never exceed 15 (= NUM_TLUTS-1), and the N64 else-path both masks
  `& 0xF` (emu64.c:3897) and bounds-checks (:3924); GXLoadTlut also
  rejects idx>=16 (pc_gx_texture.c:1156 — the flush before it is
  harmless, it only drains pending vertices). The Design Editor's DL
  comes from gsDPLoadTLUT_Dolphin at m_design_ovl.c:2590 — well-formed.
  Upstream's crash is most plausibly their divergent DL parsing (their
  emu64.c differs; a raw-shift read without the bitfield/mask would
  produce exactly such garbage). Residual local risk is only the
  `*(u16*)tlut_addr` reuse-detection deref (emu64.c:3850) on a garbage DL
  address — no evidence of that here. ACTION: open the Design Editor on
  device once and record the result here.

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

## Port limitations (by design — answer reports with these, don't reopen)

- **NES/famicom furniture always says "I don't have any software"**
  (2026-07-19, upstream Dia2809/ACGC-PC-Port#29 + our user report "SNES
  only spawning Donkey Kong / game not working"): the GC NES core
  (src/static/Famicom/ks_nes_core.cpp — `ksNesResetAsm`,
  `ksNesEmuFrameAsm`) is PowerPC inline assembly; it cannot build for
  ARM/x86, so pc/CMakeLists.txt:351 excludes Famicom/ entirely and
  `ac_my_room.c:2111` `#ifdef TARGET_PC` routes every NES furniture
  interaction to `aMR_MSG_STATE_NO_PACK_NO_DATA` (now logs
  `[PC] NES furniture: emulator not available on PC`). Everything else is
  present and portable: item→ROM table (`fFC_game_table[]`
  ac_famicom_common.c:78, 19 titles + hayakawa rom_no=20), ROM data in
  the user's own ISO (/FAMICOM/*.szs), management code famicom.cpp, stubs
  pc_stubs.c:82-98, GL-restore scaffolding `pc_gx_restore_after_nes()`
  (pc_gx.c:522, currently zero callers). Enabling would mean porting the
  6502+PPU core (weeks) or wiring an external emulator — tracked as a
  possible future feature, not a bug.

- **Campsite (tent/igloo) villagers can never move in** — game design,
  same as real GC hardware; full source proof in kb/game.md. Not RNG,
  not village rating.

## Resolved (keep for pattern-matching)

- **Edited/foreign GCI saves loaded unvalidated → crash + home-save
  corruption** (RESOLVED on dev 2026-07-19, device-verify pending; from
  user report "save editor crash right after the enter-game dialogue" +
  upstream Dia2809/ACGC-PC-Port#28 "random gamefaqs GCI in card B → train
  crash → white face, empty inventory"). Root cause: m_card.c (and its
  `mFRm_CheckSaveData_common` + whole-entry checksum gating,
  m_card.c:3106/3357) is excluded from the PC build and pc_m_card.c
  replaced it with almost no validation — Card B checked only the
  `mLd_CheckId` land_id bitmask, Card A nothing (roundtrip return
  ignored). Corrupt data then flowed into `mCD_toNextLand` →
  `mFM_SetBlockKindLoadCombi` → OOB `Save_Get(fg[bz][bx])`
  (m_field_make.c:169-193) → segfault at exactly "after the enter-game
  dialogue" / mid-train-ride; the next save persisted the trashed state.
  Fix (pc_m_card.c): `pc_save_be_sum_ok()` — BE u16 sum over
  `sizeof(Save)` on the RAW image before bswap (struct-aware bswap does
  not preserve u16 sums; region matches GC entrysize and the PC writer's
  flat checksum, stamped since the first commit so every legit PC/GC/
  Dolphin save passes) + `mFRm_CheckSaveData_common()` after bswap; both
  card paths now try main copy then in-file backup, then refuse — Card A
  falls back to .bak rotations, Card B aborts the trip with
  TRANS_ERR_CORRUPT before anything is copied. ARAM blocks
  (mail/original/diary) get per-block sums like GC
  (m_card.c:6081-6098), bad block → empty block; enforced only for
  gc_order saves (legacy PC block order predates block stamping).
  Triage switch: `PC_NO_SAVE_VALIDATE=1`. Note for save-editor users:
  editors must recompute the AC checksum (most GCI editors do; raw hex
  edits now get rejected at load instead of crashing — game falls back
  to backups/new-game rather than booting garbage).
- **Resetti never appears after quitting without saving** (RESOLVED on
  dev 2026-07-19, device-verify pending; user report). Two port gaps
  vs GC (m_card.c:3329-3334 + the start-of-game card write): (a) the
  armed `reset_code` set at game start existed only in memory — PC wrote
  the GCI at session end only, so a no-save quit left the file with a
  cleared code and detection could never fire; (b) `mCD_SaveHome_bg`
  cleared the code on EVERY save, but GC clears it only on the final
  save-and-quit (`_04==0`) and keeps it armed for save-and-continue
  (param 1, aNRST door save). Fix (pc_m_card.c): persist the save right
  after arming in `mCD_InitGameStart_bg` (like GC's game-start card
  write), and make `mCD_SaveHome_bg` mirror GC param semantics (clear
  iff param_1==0, else keep/arm; dropped the old always-re-arm-in-memory
  block). Repro check: quit without saving → next load logs
  `[PC] Reset detected!` and Resetti shows; save-and-quit → no Resetti.
  `disable_resetti` setting unaffected (default 0).

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
