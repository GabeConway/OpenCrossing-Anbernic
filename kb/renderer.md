# Renderer architecture (GX → GLES translation layer)

All in `pc/src/` + `pc/include/pc_gx_internal.h`. The game's decompiled code
calls the GameCube GX API; these files implement it on GL/GLES.

## Pipeline overview

- `pc_gx.c` — GX state machine (`g_gx` global `PCGXState`), vertex
  submission (GXBegin/GXPosition*/GXEnd), draw dispatch
  (`pc_gx_flush_vertices`), dirty-flag uniform upload, framebuffer copies.
- `pc_gx_tev.c` — TEV combiner → fragment shader. Generates a small
  **branchless GLSL shader per unique TEV config** (~30-50 configs in AC)
  instead of an uber-shader; Mali hates dynamic branching. 256-slot in-memory
  cache keyed by 72-byte `ShaderKey`.
- `pc_gx_texture.c` — GC texture format decoders (I4/I8/IA4/IA8/RGB565/
  RGB5A3/RGBA8/CI4/CI8/CMPR, tiled → linear RGBA8) + 2048-entry texture cache.
- `pc_gx_mtx.c`, `pc_vi.c` (frame pacing/swap/dynamic fps), `pc_overlay.c`
  (bitmap-font FPS counter + settings menu + boot splash).

## Batching & draw dispatch

- Vertices accumulate in `g_gx.vertex_buffer` (64k cap, 48-byte PCGXVertex).
- emu64 usually omits GXEnd; `pc_gx_flush_if_begin_complete()` flushes when
  the declared vertex count arrives. **Every GX state setter calls it first**,
  so state changes always flush the open batch before mutating state.
- **Draw-call merging (2026-07-13)**: `GXBegin` appends to the still-open
  previous batch when it completed with `dirty == 0` (nothing ran in between),
  same primitive (QUADS/TRIANGLES only) and same vtxfmt. Strips/fans never
  merge (would bridge geometry). See GXBegin in pc_gx.c.
- Quads draw via a static quad→triangle index buffer (GL_ELEMENT_ARRAY);
  everything else via glDrawArrays. One VAO/VBO bound forever; per-flush
  glBufferData orphan+upload.

## Shader system details

- Uniforms uploaded per dirty group (PC_GX_DIRTY_* bits). Shader switch sets
  `dirty = ALL`.
- **Per-program uniform locations**: `PCGXUniformLocs` filled once at link
  (`pc_gx_fill_uniform_locations` in pc_gx.c), stored in the shader cache
  entry; `pc_gx_tev_last_locs` exposes the last-returned program's locs and
  pc_gx.c struct-copies it on switch. Never call glGetUniformLocation per
  switch (~150 calls — was a real cost).
- **Shader binary disk cache** (`shader_cache.bin`, cwd; GLES builds only,
  `SHADER_DISK_CACHE` define): header magic `ACSC` + version + driver hash
  (FNV of GL_RENDERER+GL_VERSION — driver update invalidates). Entries:
  ShaderKey + binfmt + len + blob, appended on each new compile
  (`sdc_append`), all preloaded at boot (`sdc_load`) via glProgramBinary.
  `GL_PROGRAM_BINARY_RETRIEVABLE_HINT` set before link. Env
  `PC_NO_SHADER_CACHE=1` disables. Fallback uber-shader kept for
  compile-failure path (its locs in `s_fallback_locs`).
- Changing generated shader source or ShaderKey layout? **Bump SDC_VERSION**
  so stale caches self-invalidate.

## Texture system details

- Cache key: data_ptr + w/h + format + TLUT identity + content hashes
  (FNV of head+tail bytes — game reuses buffers with different contents).
- **4096-slot open-addressed hash index** over the entry array (2026-07-13);
  bulk-only deletion: eviction (oldest half, memmove) rebuilds the index,
  invalidate clears it. Never delete single entries without handling index.
- **Per-frame decode budget**: max 8 non-tiny (>32x32) CPU decodes per frame
  on the miss path; over budget → shared 1x1 white placeholder bound, entry
  NOT cached, retried next frame. Texture-pack and EFB-capture paths exempt.
- **NEON decode paths** (`__ARM_NEON` guarded) for the hot formats + u32
  palette writes; scalar fallbacks kept for other archs and edge blocks.
  Output must stay byte-identical to scalar.
- EFB captures (GXCopyTex) stay GPU-side: blit into a texture via FBO
  (`pc_gx_efb_capture_*`), no glReadPixels round trip (PC_ENHANCEMENTS).

## Frame flow

`VIWaitForRetrace` (pc_vi.c): poll events → (frameskip fast path) → blit
render-FBO to screen → overlay → swap → dynamic-fps EMA update →
frame pacing wait → begin next frame. Render-scale renders into
`s_render_fbo` at reduced res, upscaled at blit. `fps_target=6` in
settings.ini means **dynamic**, not 6 fps.
