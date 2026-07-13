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
   unchanged state concatenate into one draw call).

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
