# Performance knowledge & playbook

## Fixed so far (chronological, 2026-07-13)

1. ~150 glGetUniformLocation per shader switch → per-program `PCGXUniformLocs`
   cache (struct copy on switch).
2. Texture cache linear scan (up to 2048 entries per GXLoadTexObj, hundreds
   of calls/frame) → open-addressed hash index.
3. Mid-frame Mali shader compiles on first visit to each area → shader binary
   disk cache preloaded at boot. First-ever visit per TEV config still
   compiles once; all later runs are hitch-free. Verified: 2nd boot preloads
   from `shader_cache.bin`.
4. **Game code built at -O0**: CMAKE_C(XX)_FLAGS_RELEASE overrides in
   pc/CMakeLists.txt carried no -O flag; only pc/ platform sources had
   per-target -O2. Fixed with -O2 in RELEASE flags + `-mcpu=cortex-a53
   -mfpu=neon-vfpv4` in build-armhf-docker.sh (Debian armhf gcc defaults to
   vfpv3-d16 → no NEON, no vectorization) + -O3 for pc_gx_texture.c.
5. New-area texture decode burst → NEON decoders + per-frame decode budget
   (8/frame, placeholder + retry). Trades hitch for 1-2 frames of pop-in.
6. Per-draw driver overhead → GXBegin batch merging (quads/triangles with
   unchanged state concatenate into one draw call). Kill switch:
   PC_NO_DRAW_MERGE=1. GXSetViewport/GXSetScissor flush the open batch
   (they apply GL state immediately — required for merge correctness).
   Device data 2026-07-13: still 491-600 draws/frame in town, gl=15-26ms —
   per-draw overhead is the current #1 bottleneck (see kb/issues.md).
8. emu64.c + emu64_utility.c at -O2 (per-TU, with gnu89 + UB guards):
   device-verified safe (train intro passes, crashes=0), home area
   36 → 35-40 fps. Template for optimizing further decomp TUs one at a
   time; next candidates: decompression/loader TUs behind the 8.7s menu
   stall, m_field/actor update paths.
7. First-launch shader compiles (music stutter in menus, hitches) →
   **seed warmup**: pc/shaders/shader_seed.bin ships driver-independent
   ShaderKeys (43 configs harvested from a device playthrough; blobs
   stripped); pc_gx_tev_init compiles all of them during the LOADING splash
   and persists driver binaries. Regenerate seed: copy a well-traveled
   shader_cache.bin and strip blobs (see kb/build-test.md).

## -O2 regression (IMPORTANT)

-O2 on decomp game code: wild-pointer crash loop on device from frame 1
(log: "CRASH #N in game frame ... addr=0xDC08093A", black screen with
music). -O1: hard SIGBUS on the intro train scene. Game code is therefore
pinned at **no -O** in CMAKE_C(XX)_FLAGS_RELEASE; pc/ platform sources keep
per-source -O2/-O3. Do not raise game-code optimization without device-
testing a full new-game intro (KK Slider → train → town arrival).
SIGBUS is now caught by the crash-recovery handler (pc_main.c) and stdout
is line-buffered, so future device logs show the crash context.

9. **GX state-set dedup** (2026-07-13, DEVICE-VERIFIED same day: smoother,
   better 1% lows, acre loads clean, ~30 fps avg walking during acre loads;
   no visual regressions reported): decomp re-sets
   identical GX state constantly; every set flushed the open batch, which is
   why 500+ draws survived merging. All hot setters (TEV, blend/depth/cull,
   lighting, texgen, matrices, viewport/scissor, GXLoadTexObj) now compare
   against current state and early-return on no-ops — no flush, no dirty, so
   GXBegin merging spans them. Viewport/scissor shadow the *computed GL*
   values (invalidated in pc_gx_begin_frame + restore_after_nes since
   overlay/begin_frame touch GL viewport directly). GXLoadTexObj does the
   pure-CPU cache lookup before any flush and no-ops when the map already
   holds the same GL texture + sampler state; texture-cache invalidate clears
   g_gx.gl_textures so freed/reissued GL ids can't fool the check.
   Kill switch: PC_NO_STATE_DEDUP=1.

10. **Per-program uniform value shadowing** (2026-07-13, P2, awaiting device
    test): shader switch no longer sets dirty=ALL (was re-uploading every
    uniform group ~each switch, and switches happen constantly at 500+
    draws/frame). Every real state change bumps a per-group generation
    counter (`pc_gx_mark_dirty`, bits 0-11 = uniform groups); each program
    (tev cache entry + fallback) records the generation it last uploaded per
    group; flush uploads only mismatched groups and records them. GL-state
    groups (depth/colormask/cull/blend, bits 12-15) stay on plain g_gx.dirty
    — they're global GL state, not per-program. Gens bumped for ALL groups
    only when GL touched outside GX layer (`pc_gx_dirty_all`: init,
    restore_after_nes; NES clobbers texture-unit bindings, which ride the
    TEXTURES group). Eviction path in tev cache_insert resets the slot's gen
    record. Kill switch: PC_NO_UNIFORM_SHADOW=1 (restores dirty=ALL on
    switch). Two correctness fixes shipped with it: (a) pc_gx_begin_frame
    forces depth/color masks for glClear — now DIRTYs DEPTH|COLOR_MASK so a
    deduped same-value re-set can't leave the forced masks active; (b)
    frameskip flush no longer clears g_gx.dirty (it applied nothing to GL, so
    clearing dropped GL-state changes made during skipped frames).

11. **Dynamic-fps upward probe** (2026-07-13, P2.5, awaiting device test):
    governor was bistable — low target ⇒ more logic ticks per measured
    batch ⇒ measured work stays high ⇒ low target self-sustains (device:
    outdoor 30 fps until a house visit reset it to 60). When target < 60,
    every 120 render frames the EMA halves to re-test headroom; genuine
    load re-converges in ~4 frames. Kill switch PC_NO_FPS_PROBE=1.

## Runtime triage switches (launcher env vars)

- PC_NO_FPS_PROBE=1 — disable dynamic-fps upward probe
- PC_NO_UNIFORM_SHADOW=1 — disable per-program uniform value shadowing
- PC_NO_STATE_DEDUP=1 — disable no-op GX state-set dedup (batch-merge enabler)
- PC_NO_DRAW_MERGE=1 — disable GXBegin draw-call merging
- PC_NO_NEON_DECODE=1 — scalar texture decoders
- PC_NO_DECODE_BUDGET=1 — decode every texture the frame it's requested
- PC_NO_SHADER_CACHE=1 — disable shader binary disk cache + seed warmup

## Measuring

- Draw count + fps live in window title / FPS overlay ("N draws").
- verbose=1 → `[PERF]` every 60 frames and `[STUTTER]` lines for frames >1.5x
  average, with breakdown: `work` (game logic+GL calls), `swap`, `gl`
  (accumulated flush time), `tex` (accumulated texture decode+upload time),
  `draws`.
- Attribute dips: high `tex` → decode burst; high `gl` → uniform/draw
  overhead; high `work` others low → game logic (check -O flags first);
  `swap` high → GPU-bound (lower render scale).

## Remaining ideas (unimplemented)

- Decode-ahead: warm texture cache during acre-transition wipe (needs a hook
  into game-side room transition; fragile).
- Worker-thread texture decode (memcpy source at enqueue; GL upload stays on
  main thread). Superseded partially by the budget.
- LTO (`-flto`): untested; long link under qemu, try locally first.
- Uniform buffer objects for TEV state — probably not worth it at current
  uniform counts.
- Second render thread: no — game is deeply single-threaded.

## Perf-relevant settings (all runtime-toggleable in overlay menu)

`fps_target` (6=dynamic), `render_scale`, `frustum_cull` (+margin/max
distance), `shadow_quality`, `reduce_acre_draw`, `particle_quality`.

## Gotchas

- Never add per-draw glGetUniformLocation / glGetError (PC_GL_CHECK is
  compiled out unless PC_GL_DEBUG).
- Frameskip path (`g_pc_frameskip_active`) must skip ALL GL work but still
  clear buffers logically — check both paths when touching flush/begin_frame.
- Mali: prefer fewer, larger draws; avoid mid-frame shader compiles; avoid
  glReadPixels (use FBO blits).
