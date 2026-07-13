/* pc_gx.c - GX API → OpenGL 3.3: state management, vertex submission, draw dispatch */
#include "pc_gx_internal.h"
#include <stddef.h>
static GLushort quad_index_buf[(PC_GX_MAX_VERTS / 4) * 6];
#include <math.h>
#include <dolphin/gx/GXEnum.h>

/* Per-frame timing accumulators for GL overhead measurement */
static Uint64 s_flush_time_acc = 0;
Uint64 s_texload_time_acc = 0;     /* non-static: also written from pc_gx_texture.c */

Uint64 pc_gx_flush_time_us = 0;    /* exported for vi diagnostic */
Uint64 pc_gx_texload_time_us = 0;

/* Can't include GXTev.h — it uses enum types while we use u32 */
void GXSetTevColorIn(u32 stage, u32 a, u32 b, u32 c, u32 d);
void GXSetTevAlphaIn(u32 stage, u32 a, u32 b, u32 c, u32 d);
void GXSetTevColorOp(u32 stage, u32 op, u32 bias, u32 scale, GXBool clamp, u32 out_reg);
void GXSetTevAlphaOp(u32 stage, u32 op, u32 bias, u32 scale, GXBool clamp, u32 out_reg);

typedef struct { u8 r, g, b, a; } GXColor;

/* --- Global GX State --- */
PCGXState g_gx;

#ifdef PC_ENHANCEMENTS
/* Aspect correction: factor = gc_aspect/actual_aspect, offset = content left edge in GC coords */
static float g_aspect_factor = 1.0f;
static float g_aspect_offset = 0.0f;
static int   g_aspect_active = 0;

static void pc_gx_update_aspect(void) {
    float gc_aspect = (float)PC_GC_WIDTH / (float)PC_GC_HEIGHT;
    float win_aspect = (float)g_pc_window_w / (float)g_pc_window_h;
    if (win_aspect > gc_aspect + 0.01f) {
        g_aspect_factor = gc_aspect / win_aspect;
        g_aspect_offset = (1.0f - g_aspect_factor) / 2.0f * (float)PC_GC_WIDTH;
        g_aspect_active = 1;
    } else {
        g_aspect_factor = 1.0f;
        g_aspect_offset = 0.0f;
        g_aspect_active = 0;
    }
}

/* EFB capture: keep full-res GL textures from GXCopyTex instead of downsampling to 640x480 */
#define MAX_EFB_CAPTURES 4
static struct {
    u32 dest_ptr;
    GLuint gl_tex;
} s_efb_captures[MAX_EFB_CAPTURES];
static int s_efb_capture_count = 0;
static GLuint s_efb_copy_fbo = 0; /* persistent FBO for GPU-side EFB copy */

/* --- Render-to-texture FBO for render scale / resolution override ---
 * When g_pc_render_w < g_pc_window_w (or height), the game renders into this
 * FBO at the lower resolution, then pc_gx_blit_to_screen() upscales it to the
 * full window before the SDL buffer swap. */
static GLuint s_render_fbo = 0;
static GLuint s_render_tex = 0;
static GLuint s_render_rbo = 0;
static int    s_fbo_w = 0;
static int    s_fbo_h = 0;

static void pc_gx_ensure_render_fbo(int w, int h) {
    if (s_render_fbo && s_fbo_w == w && s_fbo_h == h) return;
    /* Delete old resources */
    if (s_render_fbo) { glDeleteFramebuffers(1, &s_render_fbo);  s_render_fbo = 0; }
    if (s_render_tex) { glDeleteTextures(1, &s_render_tex);      s_render_tex = 0; }
    if (s_render_rbo) { glDeleteRenderbuffers(1, &s_render_rbo); s_render_rbo = 0; }
    /* Create new FBO */
    glGenFramebuffers(1, &s_render_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_render_fbo);
    glGenTextures(1, &s_render_tex);
    glBindTexture(GL_TEXTURE_2D, s_render_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_render_tex, 0);
    glGenRenderbuffers(1, &s_render_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, s_render_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_render_rbo);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    s_fbo_w = w;
    s_fbo_h = h;
    fprintf(stderr, "[GX] Render FBO: %dx%d\n", w, h);
}

void pc_gx_efb_capture_store(u32 dest_ptr, GLuint gl_tex) {
    for (int i = 0; i < s_efb_capture_count; i++) {
        if (s_efb_captures[i].dest_ptr == dest_ptr) {
            if (s_efb_captures[i].gl_tex)
                glDeleteTextures(1, &s_efb_captures[i].gl_tex);
            s_efb_captures[i].gl_tex = gl_tex;
            return;
        }
    }
    if (s_efb_capture_count >= MAX_EFB_CAPTURES) {
        if (s_efb_captures[0].gl_tex)
            glDeleteTextures(1, &s_efb_captures[0].gl_tex);
        memmove(&s_efb_captures[0], &s_efb_captures[1],
                (MAX_EFB_CAPTURES - 1) * sizeof(s_efb_captures[0]));
        s_efb_capture_count = MAX_EFB_CAPTURES - 1;
    }
    s_efb_captures[s_efb_capture_count].dest_ptr = dest_ptr;
    s_efb_captures[s_efb_capture_count].gl_tex = gl_tex;
    s_efb_capture_count++;
}

GLuint pc_gx_efb_capture_find(u32 data_ptr) {
    for (int i = 0; i < s_efb_capture_count; i++) {
        if (s_efb_captures[i].dest_ptr == data_ptr)
            return s_efb_captures[i].gl_tex;
    }
    return 0;
}

void pc_gx_efb_capture_cleanup(void) {
    for (int i = 0; i < s_efb_capture_count; i++) {
        if (s_efb_captures[i].gl_tex)
            glDeleteTextures(1, &s_efb_captures[i].gl_tex);
    }
    s_efb_capture_count = 0;
}
#endif

typedef struct {
    int active;
    u8* buf;
    u32 size;
    u32 off;
    int overflow;
} PCGXDLBuildState;

static PCGXDLBuildState g_pc_gx_dl = {0};

enum {
    PCGX_DL_OP_TEXCOPY_SRC = 0x1001,
    PCGX_DL_OP_TEXCOPY_DST = 0x1002,
    PCGX_DL_OP_COPY_FILTER = 0x1003,
    PCGX_DL_OP_COPY_TEX = 0x1004,
};

static void pc_gx_dl_write(const void* data, u32 len) {
    if (!g_pc_gx_dl.active || g_pc_gx_dl.overflow) return;
    if (g_pc_gx_dl.off + len > g_pc_gx_dl.size) {
        g_pc_gx_dl.overflow = 1;
        return;
    }
    memcpy(g_pc_gx_dl.buf + g_pc_gx_dl.off, data, len);
    g_pc_gx_dl.off += len;
}

static void pc_unpack_rgba8f(u32 packed, float* out_rgba) {
    /* Shift-based: works for colors packed as (r<<24|g<<16|b<<8|a) from N64 DL data */
    out_rgba[0] = ((packed >> 24) & 0xFF) / 255.0f;
    out_rgba[1] = ((packed >> 16) & 0xFF) / 255.0f;
    out_rgba[2] = ((packed >> 8) & 0xFF) / 255.0f;
    out_rgba[3] = (packed & 0xFF) / 255.0f;
}

/* Byte-based: reads RGBA from memory order. Use for GXColor structs (not N64 DL data). */
static void pc_unpack_gxcolor_f(u32 color_as_u32, float* out_rgba) {
    const u8* bytes = (const u8*)&color_as_u32;
    out_rgba[0] = bytes[0] / 255.0f;
    out_rgba[1] = bytes[1] / 255.0f;
    out_rgba[2] = bytes[2] / 255.0f;
    out_rgba[3] = bytes[3] / 255.0f;
}

/* Map tex matrix ID to slot: raw 0..9, GX enum 30..57 (stride 3), or 60=identity */
static int pc_tex_mtx_id_to_slot(int id) {
    if (id == GX_IDENTITY) return -1;
    if (id >= 0 && id < 10) return id;
    if (id >= GX_TEXMTX0 && id < GX_IDENTITY) return (id - GX_TEXMTX0) / 3;
    return -1;
}

/* Commit pending vertex + flush batch to GL. Used by GXBegin/GXEnd/GXCopyDisp/etc. */
static void pc_gx_commit_pending_and_flush(void) {
    if (!g_gx.in_begin) return;
    if (g_gx.vertex_pending && g_gx.current_vertex_idx < PC_GX_MAX_VERTS) {
        g_gx.vertex_buffer[g_gx.current_vertex_idx] = g_gx.current_vertex;
        g_gx.current_vertex_idx++;
        g_gx.vertex_pending = 0;
    }
    g_gx.in_begin = 0;
    if (g_gx.current_vertex_idx > 0)
        pc_gx_flush_vertices();
}

/* emu64 omits GXEnd() — flush when expected vertex count is reached so the
 * batch renders with the state it was built with, not subsequent state changes. */
void pc_gx_flush_if_begin_complete(void) {
    if (!g_gx.in_begin || g_gx.expected_vertex_count <= 0) return;

    int submitted = g_gx.current_vertex_idx + (g_gx.vertex_pending ? 1 : 0);
    if (submitted < g_gx.expected_vertex_count) return;

    pc_gx_commit_pending_and_flush();
}

/* The decomp re-sets identical GX state constantly (free on real HW); each
 * set flushes the open batch, so no-op sets are the main batch breaker.
 * When enabled, setters compare against current state and return early on
 * no-ops — no flush, no dirty — letting GXBegin merging span them. */
int pc_gx_state_dedup = 1;

/* Per-program uniform value shadowing: a shader switch no longer forces
 * dirty=ALL. Every real state change bumps its group's generation counter
 * (pc_gx_mark_dirty); each program records the generation it last uploaded,
 * so a switch only re-uploads groups that changed since that program last
 * drew. PC_NO_UNIFORM_SHADOW=1 disables. */
int pc_gx_uniform_shadow = 1;

void pc_gx_mark_dirty(unsigned int flag) {
    g_gx.dirty |= flag;
    unsigned int m = flag & PC_GX_DIRTY_UNIFORM_MASK;
    while (m) {
        g_gx.group_gen[__builtin_ctz(m)]++;
        m &= m - 1;
    }
}

/* Mark everything dirty AND advance every group generation, so every program
 * re-uploads. Needed when GL state was touched outside the GX layer (init,
 * NES emulator) — a plain dirty=ALL wouldn't invalidate the per-program
 * uploaded-generation records. */
static void pc_gx_dirty_all(void) {
    g_gx.dirty = PC_GX_DIRTY_ALL;
    for (int b = 0; b < PC_GX_UNIFORM_GROUP_COUNT; b++)
        g_gx.group_gen[b]++;
}

int pc_emu64_frame_cmds = 0;
int pc_emu64_frame_crashes = 0;
int pc_emu64_frame_noop_cmds = 0;
int pc_emu64_frame_tri_cmds = 0;
int pc_emu64_frame_vtx_cmds = 0;
int pc_emu64_frame_dl_cmds = 0;
int pc_emu64_frame_cull_visible = 0;
int pc_emu64_frame_cull_rejected = 0;

void pc_gx_init(void) {
    memset(&g_gx, 0, sizeof(g_gx));
    pc_gx_state_dedup = (getenv("PC_NO_STATE_DEDUP") == NULL);
    pc_gx_uniform_shadow = (getenv("PC_NO_UNIFORM_SHADOW") == NULL);

    g_gx.projection_type = GX_PERSPECTIVE;
    g_gx.num_tev_stages = 1;
    g_gx.num_chans = 1;
    g_gx.num_tex_gens = 0;
    g_gx.cull_mode = GX_CULL_NONE;
    g_gx.z_compare_enable = 1;
    g_gx.z_compare_func = GX_LEQUAL;
    g_gx.z_update_enable = 1;
    g_gx.color_update_enable = 1;
    g_gx.alpha_update_enable = 1;
    g_gx.blend_mode = GX_BM_NONE;
    g_gx.blend_src = GX_BL_ONE;
    g_gx.blend_dst = GX_BL_ZERO;
    g_gx.clear_color[3] = 0.0f;
    g_gx.clear_depth = 1.0f;

    for (int i = 0; i < 4; i++)
        g_gx.projection_mtx[i][i] = 1.0f;

    for (int i = 0; i < 10; i++) {
        g_gx.pos_mtx[i][0][0] = 1.0f;
        g_gx.pos_mtx[i][1][1] = 1.0f;
        g_gx.pos_mtx[i][2][2] = 1.0f;
    }

    for (int i = 0; i < 10; i++) {
        g_gx.nrm_mtx[i][0][0] = 1.0f;
        g_gx.nrm_mtx[i][1][1] = 1.0f;
        g_gx.nrm_mtx[i][2][2] = 1.0f;
    }

    g_gx.tev_swap_table[0] = (PCGXTevSwapTable){0, 1, 2, 3};
    g_gx.tev_swap_table[1] = (PCGXTevSwapTable){0, 1, 2, 3};
    g_gx.tev_swap_table[2] = (PCGXTevSwapTable){0, 1, 2, 3};
    g_gx.tev_swap_table[3] = (PCGXTevSwapTable){0, 1, 2, 3};

    for (int i = 0; i < 2; i++) {
        g_gx.chan_mat_color[i][0] = 1.0f;
        g_gx.chan_mat_color[i][1] = 1.0f;
        g_gx.chan_mat_color[i][2] = 1.0f;
        g_gx.chan_mat_color[i][3] = 1.0f;
    }

    /* Quad-to-triangle index buffer */
    for (int q = 0; q < PC_GX_MAX_VERTS / 4; q++) {
        int base = q * 4;
        quad_index_buf[q * 6 + 0] = base + 0;
        quad_index_buf[q * 6 + 1] = base + 1;
        quad_index_buf[q * 6 + 2] = base + 2;
        quad_index_buf[q * 6 + 3] = base + 0;
        quad_index_buf[q * 6 + 4] = base + 2;
        quad_index_buf[q * 6 + 5] = base + 3;
    }

    glGenVertexArrays(1, &g_gx.vao);
    glGenBuffers(1, &g_gx.vbo);
    glGenBuffers(1, &g_gx.ebo);

    /* VAO setup: attrib pointers persist since PCGXVertex layout and VBO ID never change */
    glBindVertexArray(g_gx.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_gx.vbo);

    glBufferData(GL_ARRAY_BUFFER, PC_GX_MAX_VERTS * sizeof(PCGXVertex), NULL, GL_STREAM_DRAW);

    {
        size_t stride = sizeof(PCGXVertex);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(PCGXVertex, position));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(PCGXVertex, normal));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(PCGXVertex, color0));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(PCGXVertex, texcoord));
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_gx.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_index_buf), quad_index_buf, GL_STATIC_DRAW);

    /* Keep VAO/VBO bound — never unbind, avoids redundant binds per draw */

    pc_gx_tev_init();
    pc_gx_texture_init();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    pc_gx_dirty_all();
}

/* Last GL viewport/scissor actually applied. GXSetViewport/GXSetScissor skip
 * flush + GL entirely when re-set with identical values; anything that touches
 * GL viewport/scissor outside those setters must invalidate these shadows. */
static int s_gl_viewport[4];
static float s_gl_depth_range[2];
static int s_gl_viewport_valid = 0;
static int s_gl_scissor[4];
static int s_gl_scissor_valid = 0;

static void pc_gx_invalidate_vp_shadow(void) {
    s_gl_viewport_valid = 0;
    s_gl_scissor_valid = 0;
}

void pc_gx_begin_frame(void) {
    pc_gx_invalidate_vp_shadow();
    pc_emu64_frame_cmds = 0;
    pc_emu64_frame_crashes = 0;
    pc_emu64_frame_noop_cmds = 0;
    pc_emu64_frame_tri_cmds = 0;
    pc_emu64_frame_vtx_cmds = 0;
    pc_emu64_frame_dl_cmds = 0;
    pc_emu64_frame_cull_visible = 0;
    pc_emu64_frame_cull_rejected = 0;
    pc_gx_draw_call_count = 0;
    g_pc_widescreen_stretch = 0;

    /* Frameskip: skip all GL state reset on skipped frames */
    if (g_pc_frameskip_active) {
        g_gx.dirty = PC_GX_DIRTY_ALL;  /* force full re-upload on next rendered frame */
        return;
    }

    /* glClear respects write masks — must enable all before clearing */
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    /* GL masks now diverge from g_gx state; without this, a game re-set of
     * the same values would dedup away and leave the forced masks active. */
    DIRTY(PC_GX_DIRTY_DEPTH | PC_GX_DIRTY_COLOR_MASK);
#ifdef PC_ENHANCEMENTS
    pc_gx_update_aspect();
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, g_pc_render_w, g_pc_render_h);
    /* Bind render FBO when game renders at sub-window resolution */
    if (g_pc_render_w < g_pc_window_w || g_pc_render_h < g_pc_window_h) {
        pc_gx_ensure_render_fbo(g_pc_render_w, g_pc_render_h);
        glBindFramebuffer(GL_FRAMEBUFFER, s_render_fbo);
    } else {
        /* Full-res: render directly to default framebuffer, release any old FBO */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
#endif
#ifdef PC_USE_GLES
    glClearDepthf(g_gx.clear_depth);
#else
    glClearDepth(g_gx.clear_depth);
#endif
    glClearColor(g_gx.clear_color[0], g_gx.clear_color[1], g_gx.clear_color[2], g_gx.clear_color[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void pc_gx_restore_after_nes(void) {
    /* NES emulator uses its own shader/VAO/state. Rebind the game's GL objects
     * and mark all GX state dirty so uniforms/textures get re-uploaded. */
    glBindVertexArray(g_gx.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_gx.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_gx.ebo);
    pc_gx_dirty_all();
    g_gx.current_shader = 0; /* Force shader rebind on next draw */
    pc_gx_invalidate_vp_shadow();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void pc_gx_shutdown(void) {
    pc_gx_tev_shutdown();
    pc_gx_texture_shutdown();
#ifdef PC_ENHANCEMENTS
    pc_gx_efb_capture_cleanup();
    if (s_efb_copy_fbo) { glDeleteFramebuffers(1, &s_efb_copy_fbo); s_efb_copy_fbo = 0; }
    if (s_render_fbo) { glDeleteFramebuffers(1, &s_render_fbo); s_render_fbo = 0; }
    if (s_render_tex) { glDeleteTextures(1, &s_render_tex);     s_render_tex = 0; }
    if (s_render_rbo) { glDeleteRenderbuffers(1, &s_render_rbo); s_render_rbo = 0; }
#endif

    if (g_gx.ebo) glDeleteBuffers(1, &g_gx.ebo);
    if (g_gx.vbo) glDeleteBuffers(1, &g_gx.vbo);
    if (g_gx.vao) glDeleteVertexArrays(1, &g_gx.vao);
}

void pc_gx_blit_to_screen(void) {
#ifdef PC_ENHANCEMENTS
    if (!s_render_fbo) return;  /* rendering directly to default FB, nothing to do */

    /* Bind default framebuffer as draw target */
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    /* Clear to black so letterbox regions are black */
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Compute destination rectangle */
    int dst_x, dst_y, dst_w, dst_h;
    if (g_pc_scale_mode == 1) {
        /* Center: preserve pixel size, pad with black bars */
        dst_w = s_fbo_w;
        dst_h = s_fbo_h;
        dst_x = (g_pc_window_w - dst_w) / 2;
        dst_y = (g_pc_window_h - dst_h) / 2;
        if (dst_x < 0) dst_x = 0;
        if (dst_y < 0) dst_y = 0;
    } else {
        /* Stretch: fill entire window */
        dst_x = 0; dst_y = 0;
        dst_w = g_pc_window_w;
        dst_h = g_pc_window_h;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_render_fbo);
    glBlitFramebuffer(
        0, 0, s_fbo_w, s_fbo_h,
        dst_x, dst_y, dst_x + dst_w, dst_y + dst_h,
        GL_COLOR_BUFFER_BIT, GL_LINEAR);

    /* Leave default framebuffer bound for overlay draw */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
}

/* --- Vertex Submission --- */
void GXBegin(u32 primitive, u32 vtxfmt, u16 nverts) {
    /* Batch merge: every GX state setter flushes the open batch immediately,
     * so arriving here with a complete batch still open means no state changed
     * since it was built — the two draws share identical GL state and can be
     * concatenated into one draw call. Only list primitives merge safely
     * (strips/fans would bridge geometry across batches). Mali per-draw
     * driver overhead is significant; typical scenes merge many calls. */
    static int s_no_merge = -1;
    if (s_no_merge < 0) s_no_merge = (getenv("PC_NO_DRAW_MERGE") != NULL);
    if (!s_no_merge && g_gx.in_begin && g_gx.dirty == 0 &&
        (g_gx.current_primitive == GX_QUADS || g_gx.current_primitive == GX_TRIANGLES) &&
        primitive == (u32)g_gx.current_primitive &&
        vtxfmt == (u32)g_gx.current_vtxfmt &&
        g_gx.expected_vertex_count > 0) {
        int submitted = g_gx.current_vertex_idx + (g_gx.vertex_pending ? 1 : 0);
        if (submitted == g_gx.expected_vertex_count &&
            submitted + (int)nverts <= PC_GX_MAX_VERTS) {
            /* Commit the previous batch's pending vertex, extend the batch */
            if (g_gx.vertex_pending && g_gx.current_vertex_idx < PC_GX_MAX_VERTS) {
                g_gx.vertex_buffer[g_gx.current_vertex_idx] = g_gx.current_vertex;
                g_gx.current_vertex_idx++;
                g_gx.vertex_pending = 0;
            }
            g_gx.expected_vertex_count += nverts;
            /* Reset the working vertex exactly like a fresh GXBegin */
            memset(&g_gx.current_vertex, 0, sizeof(PCGXVertex));
            g_gx.current_vertex.color0[0] = 255;
            g_gx.current_vertex.color0[1] = 255;
            g_gx.current_vertex.color0[2] = 255;
            g_gx.current_vertex.color0[3] = 255;
            return;
        }
    }

    /* Auto-flush previous batch if GXEnd was omitted (normal on real HW) */
    pc_gx_commit_pending_and_flush();

    g_gx.current_primitive = primitive;
    g_gx.current_vtxfmt = vtxfmt;
    g_gx.expected_vertex_count = nverts;
    g_gx.current_vertex_idx = 0;
    g_gx.in_begin = 1;
    g_gx.vertex_pending = 0;
    memset(&g_gx.current_vertex, 0, sizeof(PCGXVertex));
    g_gx.current_vertex.color0[0] = 255;
    g_gx.current_vertex.color0[1] = 255;
    g_gx.current_vertex.color0[2] = 255;
    g_gx.current_vertex.color0[3] = 255;
}

void GXEnd(void) {
    pc_gx_commit_pending_and_flush();
}

void GXPosition3f32(f32 x, f32 y, f32 z) {
    /* Deferred commit: position call commits the previous vertex */
    if (g_gx.vertex_pending && g_gx.current_vertex_idx < PC_GX_MAX_VERTS) {
        g_gx.vertex_buffer[g_gx.current_vertex_idx] = g_gx.current_vertex;
        g_gx.current_vertex_idx++;
    }

    /* Reset vertex — only zero fields that matter (not all 96 bytes).
     * Carry last color forward. Only texcoord[0] is sent to GPU. */
    {
        u8 cr = g_gx.current_vertex.color0[0];
        u8 cg = g_gx.current_vertex.color0[1];
        u8 cb = g_gx.current_vertex.color0[2];
        u8 ca = g_gx.current_vertex.color0[3];
        g_gx.current_vertex.normal[0] = 0;
        g_gx.current_vertex.normal[1] = 0;
        g_gx.current_vertex.normal[2] = 0;
        g_gx.current_vertex.color0[0] = cr;
        g_gx.current_vertex.color0[1] = cg;
        g_gx.current_vertex.color0[2] = cb;
        g_gx.current_vertex.color0[3] = ca;
        g_gx.current_vertex.color1[0] = 0;
        g_gx.current_vertex.color1[1] = 0;
        g_gx.current_vertex.color1[2] = 0;
        g_gx.current_vertex.color1[3] = 0;
        g_gx.current_vertex.texcoord[0][0] = 0;
        g_gx.current_vertex.texcoord[0][1] = 0;
        g_gx.current_vertex.texcoord[1][0] = 0;
        g_gx.current_vertex.texcoord[1][1] = 0;
    }

    g_gx.current_vertex.position[0] = x;
    g_gx.current_vertex.position[1] = y;
    g_gx.current_vertex.position[2] = z;
    g_gx.vertex_pending = 1;
}

void GXPosition3u16(u16 x, u16 y, u16 z) { GXPosition3f32((f32)x, (f32)y, (f32)z); }
void GXPosition3s16(s16 x, s16 y, s16 z) { GXPosition3f32((f32)x, (f32)y, (f32)z); }
void GXPosition3u8(u8 x, u8 y, u8 z) { GXPosition3f32((f32)x, (f32)y, (f32)z); }
void GXPosition3s8(s8 x, s8 y, s8 z) { GXPosition3f32((f32)x, (f32)y, (f32)z); }

void GXPosition2f32(f32 x, f32 y) { GXPosition3f32(x, y, 0.0f); }
void GXPosition2u16(u16 x, u16 y) { GXPosition3f32((f32)x, (f32)y, 0.0f); }
void GXPosition2s16(s16 x, s16 y) { GXPosition3f32((f32)x, (f32)y, 0.0f); }
void GXPosition2u8(u8 x, u8 y) { GXPosition3f32((f32)x, (f32)y, 0.0f); }
void GXPosition2s8(s8 x, s8 y) { GXPosition3f32((f32)x, (f32)y, 0.0f); }

void GXPosition1x16(u16 index) {
    if (g_gx.array_base[GX_VA_POS]) {
        const u8* base = (const u8*)g_gx.array_base[GX_VA_POS];
        const f32* pos = (const f32*)(base + index * g_gx.array_stride[GX_VA_POS]);
        GXPosition3f32(pos[0], pos[1], pos[2]);
    }
}
void GXPosition1x8(u8 index) { GXPosition1x16(index); }

void GXNormal3f32(f32 x, f32 y, f32 z) {
    g_gx.current_vertex.normal[0] = x;
    g_gx.current_vertex.normal[1] = y;
    g_gx.current_vertex.normal[2] = z;
}
void GXNormal3s16(s16 x, s16 y, s16 z) {
    GXNormal3f32(x / 32767.0f, y / 32767.0f, z / 32767.0f);
}
void GXNormal3s8(s8 x, s8 y, s8 z) {
    GXNormal3f32(x / 127.0f, y / 127.0f, z / 127.0f);
}
void GXNormal1x16(u16 index) {
    if (g_gx.array_base[GX_VA_NRM]) {
        const u8* base = (const u8*)g_gx.array_base[GX_VA_NRM];
        const f32* nrm = (const f32*)(base + index * g_gx.array_stride[GX_VA_NRM]);
        GXNormal3f32(nrm[0], nrm[1], nrm[2]);
    }
}
void GXNormal1x8(u8 index) { GXNormal1x16(index); }

void GXColor4u8(u8 r, u8 g, u8 b, u8 a) {
    g_gx.current_vertex.color0[0] = r;
    g_gx.current_vertex.color0[1] = g;
    g_gx.current_vertex.color0[2] = b;
    g_gx.current_vertex.color0[3] = a;
}
void GXColor3u8(u8 r, u8 g, u8 b) { GXColor4u8(r, g, b, 255); }
void GXColor1u32(u32 clr) {
    GXColor4u8((clr >> 24) & 0xFF, (clr >> 16) & 0xFF, (clr >> 8) & 0xFF, clr & 0xFF);
}
void GXColor1u16(u16 clr) { GXColor1u32((u32)clr << 16); }
void GXColor1x16(u16 index) {
    if (g_gx.array_base[GX_VA_CLR0]) {
        const u8* base = (const u8*)g_gx.array_base[GX_VA_CLR0];
        const u8* clr = base + index * g_gx.array_stride[GX_VA_CLR0];
        GXColor4u8(clr[0], clr[1], clr[2], clr[3]);
    }
}
void GXColor1x8(u8 index) { GXColor1x16(index); }

void GXColor4f32(float r, float g, float b, float a) {
    GXColor4u8((u8)(r * 255.0f + 0.5f), (u8)(g * 255.0f + 0.5f),
               (u8)(b * 255.0f + 0.5f), (u8)(a * 255.0f + 0.5f));
}

void GXTexCoord2f32(f32 s, f32 t) {
    /* Channel 0 only — emu64 emits one texcoord; multi-tex uses matrix transforms */
    g_gx.current_vertex.texcoord[0][0] = s;
    g_gx.current_vertex.texcoord[0][1] = t;
}
void GXTexCoord2u16(u16 s, u16 t) { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord2s16(s16 s, s16 t) {
    /* No frac scaling — emu64 already provides pre-scaled texcoords */
    GXTexCoord2f32((f32)s, (f32)t);
}
void GXTexCoord2u8(u8 s, u8 t) { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord2s8(s8 s, s8 t) { GXTexCoord2f32((f32)s, (f32)t); }

void GXTexCoord1f32(f32 s, f32 t) { GXTexCoord2f32(s, t); }
void GXTexCoord1u16(u16 s, u16 t) { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord1s16(s16 s, s16 t) { GXTexCoord2s16(s, t); }
void GXTexCoord1u8(u8 s, u8 t) { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord1s8(s8 s, s8 t) { GXTexCoord2f32((f32)s, (f32)t); }

void GXTexCoord1x16(u16 index) {
    if (g_gx.array_base[GX_VA_TEX0]) {
        const u8* base = (const u8*)g_gx.array_base[GX_VA_TEX0];
        const f32* tc = (const f32*)(base + index * g_gx.array_stride[GX_VA_TEX0]);
        GXTexCoord2f32(tc[0], tc[1]);
    }
}
void GXTexCoord1x8(u8 index) { GXTexCoord1x16(index); }

/* --- Uniform Location Fill (called once per program by the TEV shader cache) --- */
void pc_gx_fill_uniform_locations(GLuint shader, PCGXUniformLocs* u) {
    char name[48];
    int i;
    #define UL(n) glGetUniformLocation(shader, n)

    u->projection = UL("u_projection");
    u->modelview  = UL("u_modelview");
    u->normal_mtx = UL("u_normal_mtx");

    u->tev_prev = UL("u_tev_prev");
    u->tev_reg0 = UL("u_tev_reg0");
    u->tev_reg1 = UL("u_tev_reg1");
    u->tev_reg2 = UL("u_tev_reg2");

    u->num_tev_stages = UL("u_num_tev_stages");
    for (i = 0; i < PC_GX_MAX_TEV_STAGES; i++) {
        snprintf(name, sizeof(name), "u_tev%d_color_in", i);
        u->tev_color_in[i] = UL(name);
        snprintf(name, sizeof(name), "u_tev%d_alpha_in", i);
        u->tev_alpha_in[i] = UL(name);
        snprintf(name, sizeof(name), "u_tev%d_color_op", i);
        u->tev_color_op[i] = UL(name);
        snprintf(name, sizeof(name), "u_tev%d_alpha_op", i);
        u->tev_alpha_op[i] = UL(name);
        snprintf(name, sizeof(name), "u_tev%d_tc_src", i);
        u->tev_tc_src[i] = UL(name);
        snprintf(name, sizeof(name), "u_tev%d_ind_cfg", i);
        u->tev_ind_cfg[i] = UL(name);
        snprintf(name, sizeof(name), "u_tev%d_ind_wrap", i);
        u->tev_ind_wrap[i] = UL(name);
    }

    u->kcolor   = UL("u_kcolor");
    u->tev_ksel = UL("u_tev_ksel");

    u->alpha_comp0 = UL("u_alpha_comp0");
    u->alpha_ref0  = UL("u_alpha_ref0");
    u->alpha_op    = UL("u_alpha_op");
    u->alpha_comp1 = UL("u_alpha_comp1");
    u->alpha_ref1  = UL("u_alpha_ref1");

    u->lighting_enabled = UL("u_lighting_enabled");
    u->mat_color  = UL("u_mat_color");
    u->amb_color  = UL("u_amb_color");
    u->chan_mat_src = UL("u_chan_mat_src");
    u->chan_amb_src = UL("u_chan_amb_src");
    u->num_chans  = UL("u_num_chans");
    u->alpha_lighting_enabled = UL("u_alpha_lighting_enabled");
    u->alpha_mat_src = UL("u_alpha_mat_src");

    u->light_mask = UL("u_light_mask");
    u->diff_fn = UL("u_diff_fn");
    u->attn_fn = UL("u_attn_fn");
    for (i = 0; i < 8; i++) {
        snprintf(name, sizeof(name), "u_light_pos[%d]", i);
        u->light_pos[i] = UL(name);
        snprintf(name, sizeof(name), "u_light_color[%d]", i);
        u->light_color[i] = UL(name);
        snprintf(name, sizeof(name), "u_light_dir[%d]", i);
        u->light_dir[i] = UL(name);
        snprintf(name, sizeof(name), "u_light_a[%d]", i);
        u->light_a[i] = UL(name);
        snprintf(name, sizeof(name), "u_light_k[%d]", i);
        u->light_k[i] = UL(name);
    }

    u->texmtx_enable[0] = UL("u_texmtx_enable");
    u->texmtx_row0[0]  = UL("u_texmtx_row0");
    u->texmtx_row1[0]  = UL("u_texmtx_row1");
    u->texgen_src[0]   = UL("u_texgen_src0");
    u->texmtx_enable[1] = UL("u_texmtx1_enable");
    u->texmtx_row0[1]  = UL("u_texmtx1_row0");
    u->texmtx_row1[1]  = UL("u_texmtx1_row1");
    u->texgen_src[1]   = UL("u_texgen_src1");

    u->use_texture0 = UL("u_use_texture0");
    u->use_texture1 = UL("u_use_texture1");
    u->use_texture2 = UL("u_use_texture2");
    u->texture0 = UL("u_texture0");
    u->texture1 = UL("u_texture1");
    u->texture2 = UL("u_texture2");

    u->num_ind_stages = UL("u_num_ind_stages");
    for (i = 0; i < 4; i++) {
        snprintf(name, sizeof(name), "u_ind_tex%d", i);
        u->ind_tex[i] = UL(name);
        snprintf(name, sizeof(name), "u_ind_scale[%d]", i);
        u->ind_scale[i] = UL(name);
    }
    for (i = 0; i < PC_GX_MAX_TEV_STAGES; i++) {
        snprintf(name, sizeof(name), "u_ind_mtx_r0[%d]", i);
        u->ind_mtx_r0[i] = UL(name);
        snprintf(name, sizeof(name), "u_ind_mtx_r1[%d]", i);
        u->ind_mtx_r1[i] = UL(name);
    }

    u->fog_type  = UL("u_fog_type");
    u->fog_start = UL("u_fog_start");
    u->fog_end   = UL("u_fog_end");
    u->fog_color = UL("u_fog_color");

    /* Per-stage bias/scale/clamp/output */
    for (i = 0; i < PC_GX_MAX_TEV_STAGES; i++) {
        snprintf(name, sizeof(name), "u_tev%d_bsc", i);
        u->tev_bsc[i] = UL(name);
        snprintf(name, sizeof(name), "u_tev%d_out", i);
        u->tev_out[i] = UL(name);
        snprintf(name, sizeof(name), "u_tev%d_swap", i);
        u->tev_swap[i] = UL(name);
    }
    u->swap_table = UL("u_swap_table");

    #undef UL
}

/* --- Vertex Flush --- */
int pc_gx_draw_call_count = 0;

void pc_gx_flush_vertices(void) {
    int count = g_gx.current_vertex_idx;
    if (count == 0) return;

    Uint64 t_flush_start = SDL_GetPerformanceCounter();
    pc_gx_draw_call_count++;

    /* Frameskip: skip all GL work. Leave dirty set — nothing was applied to
     * GL, so clearing it here would drop GL-state changes (depth/blend/cull/
     * masks) made during skipped frames. */
    if (g_pc_frameskip_active) {
        s_flush_time_acc += SDL_GetPerformanceCounter() - t_flush_start;
        return;
    }

    GLuint shader = pc_gx_tev_get_shader(&g_gx);
    unsigned int* prog_gens = pc_gx_uniform_shadow ? pc_gx_tev_last_gens : NULL;
    if (shader && shader != g_gx.current_shader) {
        glUseProgram(shader);
        PC_GL_CHECK("glUseProgram");
        g_gx.current_shader = shader;
        /* Locations were resolved once at link time by the shader cache;
         * a struct copy replaces ~150 glGetUniformLocation driver calls. */
        g_gx.uloc = *pc_gx_tev_last_locs;
        if (!prog_gens)
            g_gx.dirty = PC_GX_DIRTY_ALL;  /* shadowing off: re-upload all */
        /* Set constant sampler bindings once per shader change */
        {
            GLint sl;
            sl = g_gx.uloc.texture0; if (sl >= 0) glUniform1i(sl, 0);
            sl = g_gx.uloc.texture1; if (sl >= 0) glUniform1i(sl, 1);
            sl = g_gx.uloc.texture2; if (sl >= 0) glUniform1i(sl, 2);
        }
    }

    /* VAO/VBO stay bound from init. glBufferData orphans + uploads in one call. */
    glBufferData(GL_ARRAY_BUFFER, count * sizeof(PCGXVertex), g_gx.vertex_buffer, GL_STREAM_DRAW);

    /* Upload only dirty state groups */
    if (shader) {
        GLint loc;
        unsigned int dirty;
        if (prog_gens) {
            /* Upload a group iff this program hasn't seen its current
             * generation — covers state changes AND shader switches, without
             * a switch re-uploading values the program already holds. */
            dirty = 0;
            for (int b = 0; b < PC_GX_UNIFORM_GROUP_COUNT; b++)
                if (prog_gens[b] != g_gx.group_gen[b]) dirty |= (1u << b);
            /* Texture bindings and use_texture* upload as one block below —
             * run and record them together. */
            if (dirty & (PC_GX_DIRTY_TEXTURES | PC_GX_DIRTY_TEV_STAGES))
                dirty |= PC_GX_DIRTY_TEXTURES | PC_GX_DIRTY_TEV_STAGES;
        } else {
            dirty = g_gx.dirty;
        }
        #define UL(field) g_gx.uloc.field

        if (dirty & PC_GX_DIRTY_PROJECTION) {
            loc = UL(projection);
            if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_TRUE, (float*)g_gx.projection_mtx);
        }

        if (dirty & PC_GX_DIRTY_MODELVIEW) {
            loc = UL(modelview);
            if (loc >= 0) {
                float mv44[16];
                const float* src = (const float*)g_gx.pos_mtx[g_gx.current_mtx];
                mv44[ 0] = src[0]; mv44[ 1] = src[1]; mv44[ 2] = src[2]; mv44[ 3] = src[3];
                mv44[ 4] = src[4]; mv44[ 5] = src[5]; mv44[ 6] = src[6]; mv44[ 7] = src[7];
                mv44[ 8] = src[8]; mv44[ 9] = src[9]; mv44[10] = src[10]; mv44[11] = src[11];
                mv44[12] = 0.0f;   mv44[13] = 0.0f;   mv44[14] = 0.0f;    mv44[15] = 1.0f;
                glUniformMatrix4fv(loc, 1, GL_TRUE, mv44);
            }
            loc = UL(normal_mtx);
            if (loc >= 0) glUniformMatrix3fv(loc, 1, GL_TRUE, (const float*)g_gx.nrm_mtx[g_gx.current_mtx]);
        }

        if (dirty & PC_GX_DIRTY_TEV_COLORS) {
            loc = UL(tev_prev); if (loc >= 0) glUniform4fv(loc, 1, g_gx.tev_colors[0]);
            loc = UL(tev_reg0); if (loc >= 0) glUniform4fv(loc, 1, g_gx.tev_colors[1]);
            loc = UL(tev_reg1); if (loc >= 0) glUniform4fv(loc, 1, g_gx.tev_colors[2]);
            loc = UL(tev_reg2); if (loc >= 0) glUniform4fv(loc, 1, g_gx.tev_colors[3]);
        }

        if (dirty & PC_GX_DIRTY_TEV_STAGES) {
            loc = UL(num_tev_stages); if (loc >= 0) glUniform1i(loc, g_gx.num_tev_stages);
            for (int s = 0; s < PC_GX_MAX_TEV_STAGES && s < g_gx.num_tev_stages; s++) {
                PCGXTevStage* ts = &g_gx.tev_stages[s];
                loc = UL(tev_color_in[s]); if (loc >= 0) glUniform4i(loc, ts->color_a, ts->color_b, ts->color_c, ts->color_d);
                loc = UL(tev_alpha_in[s]); if (loc >= 0) glUniform4i(loc, ts->alpha_a, ts->alpha_b, ts->alpha_c, ts->alpha_d);
                loc = UL(tev_color_op[s]); if (loc >= 0) glUniform1i(loc, ts->color_op);
                loc = UL(tev_alpha_op[s]); if (loc >= 0) glUniform1i(loc, ts->alpha_op);
                loc = UL(tev_bsc[s]);  if (loc >= 0) glUniform4i(loc, ts->color_bias, ts->color_scale, ts->alpha_bias, ts->alpha_scale);
                loc = UL(tev_out[s]);  if (loc >= 0) glUniform4i(loc, ts->color_clamp, ts->alpha_clamp, ts->color_out, ts->alpha_out);
                loc = UL(tev_swap[s]); if (loc >= 0) glUniform2i(loc, ts->ras_swap, ts->tex_swap);
            }
            loc = UL(tev_ksel);
            if (loc >= 0) {
                int ksel[PC_GX_MAX_TEV_STAGES * 3];
                for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
                    ksel[s * 3 + 0] = g_gx.tev_stages[s].k_color_sel;
                    ksel[s * 3 + 1] = g_gx.tev_stages[s].k_alpha_sel;
                    ksel[s * 3 + 2] = s;
                }
                glUniform3iv(loc, PC_GX_MAX_TEV_STAGES, ksel);
            }
            for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
                int tc_src = 0;
                if (s < g_gx.num_tev_stages) {
                    int tc = g_gx.tev_stages[s].tex_coord;
                    if (tc >= 0 && tc < 8) tc_src = tc;
                    else tc_src = s;
                }
                loc = UL(tev_tc_src[s]); if (loc >= 0) glUniform1i(loc, tc_src);
            }
        }

        if (dirty & PC_GX_DIRTY_SWAP_TABLES) {
            loc = UL(swap_table);
            if (loc >= 0) {
                int sw[16];
                for (int t = 0; t < 4; t++) {
                    sw[t*4+0] = g_gx.tev_swap_table[t].r;
                    sw[t*4+1] = g_gx.tev_swap_table[t].g;
                    sw[t*4+2] = g_gx.tev_swap_table[t].b;
                    sw[t*4+3] = g_gx.tev_swap_table[t].a;
                }
                glUniform4iv(loc, 4, sw);
            }
        }

        if (dirty & PC_GX_DIRTY_KONST) {
            loc = UL(kcolor); if (loc >= 0) glUniform4fv(loc, 4, (const float*)g_gx.tev_k_colors);
        }

        if (dirty & PC_GX_DIRTY_ALPHA_CMP) {
            loc = UL(alpha_comp0); if (loc >= 0) glUniform1i(loc, g_gx.alpha_comp0);
            loc = UL(alpha_ref0);  if (loc >= 0) glUniform1i(loc, g_gx.alpha_ref0);
            loc = UL(alpha_op);    if (loc >= 0) glUniform1i(loc, g_gx.alpha_op);
            loc = UL(alpha_comp1); if (loc >= 0) glUniform1i(loc, g_gx.alpha_comp1);
            loc = UL(alpha_ref1);  if (loc >= 0) glUniform1i(loc, g_gx.alpha_ref1);
        }

        if (dirty & PC_GX_DIRTY_LIGHTING) {
            loc = UL(lighting_enabled); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_enable[0]);
            loc = UL(mat_color);  if (loc >= 0) glUniform4fv(loc, 1, g_gx.chan_mat_color[0]);
            loc = UL(amb_color);  if (loc >= 0) glUniform4fv(loc, 1, g_gx.chan_amb_color[0]);
            loc = UL(chan_mat_src); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_mat_src[0]);
            loc = UL(chan_amb_src); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_amb_src[0]);
            loc = UL(num_chans);  if (loc >= 0) glUniform1i(loc, g_gx.num_chans);
            loc = UL(alpha_lighting_enabled); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_enable[1]);
            loc = UL(alpha_mat_src); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_mat_src[1]);
            /* Per-vertex lighting: upload light mask, diff/attn functions, and per-light data */
            loc = UL(light_mask); if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_light_mask[0]);
            loc = UL(diff_fn);   if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_diff_fn[0]);
            loc = UL(attn_fn);   if (loc >= 0) glUniform1i(loc, g_gx.chan_ctrl_attn_fn[0]);
            {
                int li;
                u32 mask = (u32)g_gx.chan_ctrl_light_mask[0];
                for (li = 0; li < 8; li++) {
                    if (mask & (1u << li)) {
                        float a_vec[3], k_vec[3];
                        loc = g_gx.uloc.light_pos[li];   if (loc >= 0) glUniform3fv(loc, 1, g_gx.lights[li].pos);
                        loc = g_gx.uloc.light_color[li];  if (loc >= 0) glUniform4fv(loc, 1, g_gx.lights[li].color);
                        loc = g_gx.uloc.light_dir[li];   if (loc >= 0) glUniform3fv(loc, 1, g_gx.lights[li].dir);
                        a_vec[0] = g_gx.lights[li].a0; a_vec[1] = g_gx.lights[li].a1; a_vec[2] = g_gx.lights[li].a2;
                        k_vec[0] = g_gx.lights[li].k0; k_vec[1] = g_gx.lights[li].k1; k_vec[2] = g_gx.lights[li].k2;
                        loc = g_gx.uloc.light_a[li];     if (loc >= 0) glUniform3fv(loc, 1, a_vec);
                        loc = g_gx.uloc.light_k[li];     if (loc >= 0) glUniform3fv(loc, 1, k_vec);
                    }
                }
            }
        }

        if (dirty & PC_GX_DIRTY_TEXGEN) {
            for (int tg = 0; tg < 2; tg++) {
                int mtx_id = g_gx.tex_gen_mtx[tg];
                int slot = pc_tex_mtx_id_to_slot(mtx_id);
                int has_mtx = (slot >= 0 && slot < 10);
                loc = g_gx.uloc.texmtx_enable[tg]; if (loc >= 0) glUniform1i(loc, has_mtx);
                if (has_mtx) {
                    const float* tm = (const float*)g_gx.tex_mtx[slot];
                    loc = g_gx.uloc.texmtx_row0[tg]; if (loc >= 0) glUniform4f(loc, tm[0], tm[1], tm[2], tm[3]);
                    loc = g_gx.uloc.texmtx_row1[tg]; if (loc >= 0) glUniform4f(loc, tm[4], tm[5], tm[6], tm[7]);
                }
                loc = g_gx.uloc.texgen_src[tg]; if (loc >= 0) glUniform1i(loc, g_gx.tex_gen_src[tg]);
            }
        }

        if (dirty & (PC_GX_DIRTY_TEXTURES | PC_GX_DIRTY_TEV_STAGES)) {
            int use_tex_stage[PC_GX_MAX_TEV_STAGES] = { 0 };
            GLuint tex_obj_stage[PC_GX_MAX_TEV_STAGES] = { 0 };
            for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
                if (s < g_gx.num_tev_stages) {
                    int tex_map = g_gx.tev_stages[s].tex_map;
                    if (tex_map >= 0 && tex_map < 8)
                        tex_obj_stage[s] = g_gx.gl_textures[tex_map];
                }
                if (tex_obj_stage[s] != 0) {
                    use_tex_stage[s] = 1;
                    glActiveTexture(GL_TEXTURE0 + s);
                    glBindTexture(GL_TEXTURE_2D, tex_obj_stage[s]);
                }
            }
            glActiveTexture(GL_TEXTURE0);
            loc = UL(use_texture0); if (loc >= 0) glUniform1i(loc, use_tex_stage[0]);
            loc = UL(use_texture1); if (loc >= 0) glUniform1i(loc, use_tex_stage[1]);
            loc = UL(use_texture2); if (loc >= 0) glUniform1i(loc, use_tex_stage[2]);
            /* Sampler bindings (texture0=0, texture1=1, texture2=2) set once on shader init */
        }

        /* Indirect textures stripped — shader doesn't use them.
         * Uniforms declared for C-side compat but never read in fragment shader.
         * Skipping saves ~25 GL calls per draw on ARM Mali. */

        if (dirty & PC_GX_DIRTY_FOG) {
            loc = UL(fog_type);  if (loc >= 0) glUniform1i(loc, g_gx.fog_type);
            loc = UL(fog_start); if (loc >= 0) glUniform1f(loc, g_gx.fog_start);
            loc = UL(fog_end);   if (loc >= 0) glUniform1f(loc, g_gx.fog_end);
            loc = UL(fog_color);  if (loc >= 0) glUniform4fv(loc, 1, g_gx.fog_color);
        }

        /* Record what this program now holds */
        if (prog_gens) {
            for (int b = 0; b < PC_GX_UNIFORM_GROUP_COUNT; b++)
                if (dirty & (1u << b)) prog_gens[b] = g_gx.group_gen[b];
        }

        #undef UL
    }

    GLenum gl_prim;
    switch (g_gx.current_primitive) {
        case GX_QUADS:         gl_prim = GL_TRIANGLES; break;
        case GX_TRIANGLES:     gl_prim = GL_TRIANGLES; break;
        case GX_TRIANGLESTRIP: gl_prim = GL_TRIANGLE_STRIP; break;
        case GX_TRIANGLEFAN:   gl_prim = GL_TRIANGLE_FAN; break;
        case GX_LINES:         gl_prim = GL_LINES; break;
        case GX_LINESTRIP:     gl_prim = GL_LINE_STRIP; break;
        case GX_POINTS:        gl_prim = GL_POINTS; break;
        default:               gl_prim = GL_TRIANGLES; break;
    }

    if (g_gx.dirty & PC_GX_DIRTY_DEPTH) {
        if (g_gx.z_compare_enable) {
            glEnable(GL_DEPTH_TEST);
            GLenum zfunc;
            switch (g_gx.z_compare_func) {
                case GX_NEVER:   zfunc = GL_NEVER; break;
                case GX_LESS:    zfunc = GL_LESS; break;
                case GX_EQUAL:   zfunc = GL_EQUAL; break;
                case GX_LEQUAL:  zfunc = GL_LEQUAL; break;
                case GX_GREATER: zfunc = GL_GREATER; break;
                case GX_NEQUAL:  zfunc = GL_NOTEQUAL; break;
                case GX_GEQUAL:  zfunc = GL_GEQUAL; break;
                case GX_ALWAYS:  zfunc = GL_ALWAYS; break;
                default:         zfunc = GL_LEQUAL; break;
            }
            glDepthFunc(zfunc);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        glDepthMask(g_gx.z_update_enable ? GL_TRUE : GL_FALSE);
    }

    if (g_gx.dirty & PC_GX_DIRTY_COLOR_MASK) {
        glColorMask(
            g_gx.color_update_enable ? GL_TRUE : GL_FALSE,
            g_gx.color_update_enable ? GL_TRUE : GL_FALSE,
            g_gx.color_update_enable ? GL_TRUE : GL_FALSE,
            g_gx.alpha_update_enable ? GL_TRUE : GL_FALSE
        );
    }

    if (g_gx.dirty & PC_GX_DIRTY_CULL) {
        switch (g_gx.cull_mode) {
            case GX_CULL_NONE:  glDisable(GL_CULL_FACE); break;
            case GX_CULL_FRONT: glEnable(GL_CULL_FACE); glCullFace(GL_FRONT); break;
            case GX_CULL_BACK:  glEnable(GL_CULL_FACE); glCullFace(GL_BACK); break;
            case GX_CULL_ALL:   glEnable(GL_CULL_FACE); glCullFace(GL_FRONT_AND_BACK); break;
        }
    }

    if (g_gx.dirty & PC_GX_DIRTY_BLEND) {
        switch (g_gx.blend_mode) {
            case GX_BM_NONE:
                glDisable(GL_BLEND);
                break;
            case GX_BM_BLEND:
                glEnable(GL_BLEND);
                {
                    GLenum src, dst;
                    switch (g_gx.blend_src) {
                        case GX_BL_ZERO:        src = GL_ZERO; break;
                        case GX_BL_ONE:         src = GL_ONE; break;
                        case GX_BL_DSTCLR:      src = GL_DST_COLOR; break;
                        case GX_BL_INVDSTCLR:   src = GL_ONE_MINUS_DST_COLOR; break;
                        case GX_BL_SRCALPHA:    src = GL_SRC_ALPHA; break;
                        case GX_BL_INVSRCALPHA: src = GL_ONE_MINUS_SRC_ALPHA; break;
                        case GX_BL_DSTALPHA:    src = GL_DST_ALPHA; break;
                        case GX_BL_INVDSTALPHA: src = GL_ONE_MINUS_DST_ALPHA; break;
                        default:                src = GL_ONE; break;
                    }
                    switch (g_gx.blend_dst) {
                        case GX_BL_ZERO:        dst = GL_ZERO; break;
                        case GX_BL_ONE:         dst = GL_ONE; break;
                        case GX_BL_SRCCLR:      dst = GL_SRC_COLOR; break;
                        case GX_BL_INVSRCCLR:   dst = GL_ONE_MINUS_SRC_COLOR; break;
                        case GX_BL_SRCALPHA:    dst = GL_SRC_ALPHA; break;
                        case GX_BL_INVSRCALPHA: dst = GL_ONE_MINUS_SRC_ALPHA; break;
                        case GX_BL_DSTALPHA:    dst = GL_DST_ALPHA; break;
                        case GX_BL_INVDSTALPHA: dst = GL_ONE_MINUS_DST_ALPHA; break;
                        default:                dst = GL_ZERO; break;
                    }
                    /* DST_ALPHA→SRC_ALPHA: GC EFB alpha semantics differ from GL */
                    if (g_gx.blend_src == GX_BL_DSTALPHA && g_gx.blend_dst == GX_BL_INVDSTALPHA) {
                        src = GL_SRC_ALPHA;
                        dst = GL_ONE_MINUS_SRC_ALPHA;
                    }
                    glBlendFunc(src, dst);
                }
                break;
            case GX_BM_LOGIC:
                glDisable(GL_BLEND);
                break;
            case GX_BM_SUBTRACT:
                glEnable(GL_BLEND);
                glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
                glBlendFunc(GL_ONE, GL_ONE);
                break;
        }
    }

    if (g_gx.current_primitive == GX_QUADS) {
        int num_quads = count / 4;
        int num_indices = num_quads * 6;
        glDrawElements(GL_TRIANGLES, num_indices, GL_UNSIGNED_SHORT, 0);
        PC_GL_CHECK("glDrawElements");
    } else {
        glDrawArrays(gl_prim, 0, count);
        PC_GL_CHECK("glDrawArrays");
    }

    if (g_gx.blend_mode == GX_BM_SUBTRACT)
        glBlendEquation(GL_FUNC_ADD);

    g_gx.dirty = 0;

    s_flush_time_acc += SDL_GetPerformanceCounter() - t_flush_start;
}

/* Called from VIWaitForRetrace to snapshot & reset per-frame timing */
void pc_gx_frame_timing_snapshot(void) {
    Uint64 freq = SDL_GetPerformanceFrequency();
    pc_gx_flush_time_us  = s_flush_time_acc * 1000000 / freq;
    pc_gx_texload_time_us = s_texload_time_acc * 1000000 / freq;
    s_flush_time_acc = 0;
    s_texload_time_acc = 0;
}

/* --- Vertex Descriptor / Format --- */
void GXSetVtxDesc(u32 attr, u32 type) {
    if (attr < PC_GX_MAX_ATTR) g_gx.vtx_desc[attr] = type;
}
void GXSetVtxDescv(const void* list) {
    const u32* p = (const u32*)list;
    while (p[0] != GX_VA_NULL) {
        GXSetVtxDesc(p[0], p[1]);
        p += 2;
    }
}
void GXClearVtxDesc(void) { memset(g_gx.vtx_desc, 0, sizeof(g_gx.vtx_desc)); }

void GXSetVtxAttrFmt(u32 vtxfmt, u32 attr, u32 cnt, u32 type, u8 frac) {
    (void)cnt; (void)type;
    if (vtxfmt < GX_MAX_VTXFMT) {
        if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
            int tc = (int)attr - GX_VA_TEX0;
            g_gx.vtx_fmt[vtxfmt].has_texcoord[tc] = 1;
            g_gx.vtx_fmt[vtxfmt].texcoord_frac[tc] = frac;
        }
    }
}

void GXSetArray(u32 attr, const void* data, u32 size, u8 stride) {
    if (attr < GX_VA_MAX_ATTR) {
        g_gx.array_base[attr] = data;
        g_gx.array_stride[attr] = stride;
    }
}

void GXInvalidateVtxCache(void) { }

/* --- Transforms --- */
void GXSetProjection(const void* mtx, u32 type) {
    /* Shadow the raw input plus the globals baked into the final matrix —
     * the stored projection_mtx is post-aspect/zoom, so it can't be compared
     * against the incoming GX matrix directly. */
    static float s_proj_in[12];
    static u32 s_proj_type;
    static int s_proj_valid = 0;
#ifdef PC_ENHANCEMENTS
    static int s_proj_ws, s_proj_aspect;
    static float s_proj_factor, s_proj_zoom;
#endif
    if (pc_gx_state_dedup && s_proj_valid && type == s_proj_type &&
        memcmp(mtx, s_proj_in, sizeof(s_proj_in)) == 0
#ifdef PC_ENHANCEMENTS
        && s_proj_ws == g_pc_widescreen_stretch && s_proj_aspect == g_aspect_active
        && s_proj_factor == g_aspect_factor && s_proj_zoom == g_pc_zoom
#endif
        )
        return;
    memcpy(s_proj_in, mtx, sizeof(s_proj_in));
    s_proj_type = type;
#ifdef PC_ENHANCEMENTS
    s_proj_ws = g_pc_widescreen_stretch;
    s_proj_aspect = g_aspect_active;
    s_proj_factor = g_aspect_factor;
    s_proj_zoom = g_pc_zoom;
#endif
    s_proj_valid = 1;

    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_PROJECTION);
    g_gx.projection_type = type;
    memcpy(g_gx.projection_mtx, mtx, sizeof(float) * 12);
    /* GX only stores 3 rows — 4th row is implicit based on projection type */
    if (type == GX_PERSPECTIVE) {
        g_gx.projection_mtx[3][0] = 0.0f;
        g_gx.projection_mtx[3][1] = 0.0f;
        g_gx.projection_mtx[3][2] = -1.0f;
        g_gx.projection_mtx[3][3] = 0.0f;
    } else { /* GX_ORTHOGRAPHIC */
        g_gx.projection_mtx[3][0] = 0.0f;
        g_gx.projection_mtx[3][1] = 0.0f;
        g_gx.projection_mtx[3][2] = 0.0f;
        g_gx.projection_mtx[3][3] = 1.0f;
    }

#ifdef PC_ENHANCEMENTS
    /* Widescreen: 0=hor+ (both), 1=stretch (none), 2=UI (ortho only) */
    if (g_pc_widescreen_stretch == 0 ||
        (g_pc_widescreen_stretch == 2 && type == GX_ORTHOGRAPHIC)) {
        if (g_aspect_active) {
            g_gx.projection_mtx[0][0] *= g_aspect_factor;
        }
    }
    /* Camera zoom — only for perspective (3D world), not orthographic (UI) */
    if (type == GX_PERSPECTIVE && g_pc_zoom != 1.0f) {
        g_gx.projection_mtx[0][0] *= g_pc_zoom;
        g_gx.projection_mtx[1][1] *= g_pc_zoom;
    }
#endif
}

void GXLoadPosMtxImm(const void* mtx, u32 id) {
    int slot = id / 3;
    if (slot >= 10) return;
    if (pc_gx_state_dedup && memcmp(g_gx.pos_mtx[slot], mtx, sizeof(float) * 12) == 0)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_MODELVIEW);
    memcpy(g_gx.pos_mtx[slot], mtx, sizeof(float) * 12);
}

void GXLoadNrmMtxImm(const void* mtx, u32 id) {
    int slot = id / 3;
    if (slot >= 10) return;
    /* Extract upper-left 3x3 from 3x4 row-major Mtx (stride 4, not contiguous) */
    const float* src = (const float*)mtx;
    float (*d)[3] = g_gx.nrm_mtx[slot];
    if (pc_gx_state_dedup &&
        d[0][0] == src[0] && d[0][1] == src[1] && d[0][2] == src[2] &&
        d[1][0] == src[4] && d[1][1] == src[5] && d[1][2] == src[6] &&
        d[2][0] == src[8] && d[2][1] == src[9] && d[2][2] == src[10])
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_MODELVIEW);
    d[0][0] = src[0]; d[0][1] = src[1]; d[0][2] = src[2];
    d[1][0] = src[4]; d[1][1] = src[5]; d[1][2] = src[6];
    d[2][0] = src[8]; d[2][1] = src[9]; d[2][2] = src[10];
}

void GXLoadTexMtxImm(const void* mtx, u32 id, u32 type) {
    int slot = pc_tex_mtx_id_to_slot((int)id);
    if (slot < 0 || slot >= 10) return;
    if (pc_gx_state_dedup && memcmp(g_gx.tex_mtx[slot], mtx, sizeof(float) * 12) == 0)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_TEXGEN);
    memcpy(g_gx.tex_mtx[slot], mtx, sizeof(float) * 12);
}

void GXSetCurrentMtx(u32 id) {
    u32 slot = id / 3;
    if (slot >= 10) return;
    if (pc_gx_state_dedup && g_gx.current_mtx == (int)slot) return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_MODELVIEW);
    g_gx.current_mtx = slot;
}

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    g_gx.viewport[0] = left;
    g_gx.viewport[1] = top;
    g_gx.viewport[2] = wd;
    g_gx.viewport[3] = ht;
    g_gx.viewport[4] = nearz;
    g_gx.viewport[5] = farz;
    if (g_pc_frameskip_active) return;
    int gl_x, gl_y, gl_w, gl_h;
#ifdef PC_ENHANCEMENTS
    {
        float sx = (float)g_pc_render_w / (float)PC_GC_WIDTH;
        float sy = (float)g_pc_render_h / (float)PC_GC_HEIGHT;
        float adj_left = left;
        float adj_wd = wd;

        /* UI mode: remap sub-viewports to match aspect-corrected content */
        if (g_pc_widescreen_stretch == 2 && g_aspect_active) {
            int is_full = (left < 1.0f && top < 1.0f &&
                           wd > (float)(PC_GC_WIDTH - 1) &&
                           ht > (float)(PC_GC_HEIGHT - 1));
            if (!is_full) {
                adj_left = g_aspect_offset + left * g_aspect_factor;
                adj_wd = wd * g_aspect_factor;
            }
        }

        gl_x = (int)(adj_left * sx);
        gl_w = (int)(adj_wd * sx);
        gl_h = (int)(ht * sy);
        gl_y = g_pc_render_h - (int)(top * sy) - gl_h;
    }
#else
    /* GX is Y-down, GL is Y-up */
    gl_x = (int)left;
    gl_y = PC_GC_HEIGHT - (int)top - (int)ht;
    gl_w = (int)wd;
    gl_h = (int)ht;
#endif
    /* Identical viewport → GL state already right, no flush needed. A real
     * change must flush first: glViewport applies immediately and a
     * buffered-but-unflushed batch would draw with the NEW viewport. */
    if (pc_gx_state_dedup && s_gl_viewport_valid &&
        gl_x == s_gl_viewport[0] && gl_y == s_gl_viewport[1] &&
        gl_w == s_gl_viewport[2] && gl_h == s_gl_viewport[3] &&
        nearz == s_gl_depth_range[0] && farz == s_gl_depth_range[1])
        return;
    pc_gx_flush_if_begin_complete();
    glViewport(gl_x, gl_y, gl_w, gl_h);
#ifdef PC_USE_GLES
    glDepthRangef(nearz, farz);
#else
    glDepthRange((double)nearz, (double)farz);
#endif
    s_gl_viewport[0] = gl_x;
    s_gl_viewport[1] = gl_y;
    s_gl_viewport[2] = gl_w;
    s_gl_viewport[3] = gl_h;
    s_gl_depth_range[0] = nearz;
    s_gl_depth_range[1] = farz;
    s_gl_viewport_valid = 1;
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field) {
    GXSetViewport(left, top, wd, ht, nearz, farz);
}

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    g_gx.scissor[0] = left;
    g_gx.scissor[1] = top;
    g_gx.scissor[2] = wd;
    g_gx.scissor[3] = ht;
    if (g_pc_frameskip_active) return;
    int gl_x, gl_y, gl_w, gl_h;
#ifdef PC_ENHANCEMENTS
    {
        float sx = (float)g_pc_render_w / (float)PC_GC_WIDTH;
        float sy = (float)g_pc_render_h / (float)PC_GC_HEIGHT;
        gl_x = (int)(left * sx);
        gl_w = (int)(wd * sx);
        gl_h = (int)(ht * sy);
        gl_y = g_pc_render_h - (int)(top * sy) - gl_h;
    }
#else
    /* GX is Y-down, GL is Y-up */
    gl_x = (int)left;
    gl_y = PC_GC_HEIGHT - (int)top - (int)ht;
    gl_w = (int)wd;
    gl_h = (int)ht;
#endif
    /* Identical scissor (and test already enabled) → skip flush + GL. A real
     * change must flush first: glScissor applies immediately (see
     * GXSetViewport). Shadow is invalidated wherever GL scissor state changes
     * outside this setter (begin_frame disables the test each frame). */
    if (pc_gx_state_dedup && s_gl_scissor_valid &&
        gl_x == s_gl_scissor[0] && gl_y == s_gl_scissor[1] &&
        gl_w == s_gl_scissor[2] && gl_h == s_gl_scissor[3])
        return;
    pc_gx_flush_if_begin_complete();
    glEnable(GL_SCISSOR_TEST);
    glScissor(gl_x, gl_y, gl_w, gl_h);
    s_gl_scissor[0] = gl_x;
    s_gl_scissor[1] = gl_y;
    s_gl_scissor[2] = gl_w;
    s_gl_scissor[3] = gl_h;
    s_gl_scissor_valid = 1;
}

void GXSetScissorBoxOffset(s32 x, s32 y) { (void)x; (void)y; }
void GXSetClipMode(u32 mode) { (void)mode; }

void GXGetProjectionv(f32* p) {
    if (p) memcpy(p, g_gx.projection_mtx, sizeof(float) * 16);
}

void GXGetVtxAttrFmt(u32 idx, u32 attr, u32* compCnt, u32* compType, u8* shift) {
    if (compCnt) *compCnt = 0;
    if (compType) *compType = 0;
    if (shift) *shift = 0;
}

/* --- TEV Configuration --- */
void GXSetNumTevStages(u8 nStages) {
    if (pc_gx_state_dedup && g_gx.num_tev_stages == (int)nStages) return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_TEV_STAGES);
    g_gx.num_tev_stages = nStages;
}

void GXSetTevOp(u32 stage, u32 mode) {
    /* No flush here — the GXSetTev* calls below flush iff they change state */
    if (stage >= 16) return;

    /* TEV formula: out = (d + ((1-c)*a + c*b) + bias) * scale */
    switch (mode) {
    case GX_MODULATE:
        GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
        break;
    case GX_DECAL:
        GXSetTevColorIn(stage, GX_CC_RASC, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
        break;
    case GX_BLEND:
        GXSetTevColorIn(stage, GX_CC_ONE, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
        break;
    case GX_REPLACE:
        GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
        break;
    case GX_PASSCLR:
        GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
        break;
    default:
        return;
    }
    GXSetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

void GXSetTevColorIn(u32 stage, u32 a, u32 b, u32 c, u32 d) {
    if (stage >= 16) return;
    PCGXTevStage* ts = &g_gx.tev_stages[stage];
    if (pc_gx_state_dedup &&
        ts->color_a == (int)a && ts->color_b == (int)b &&
        ts->color_c == (int)c && ts->color_d == (int)d)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_TEV_STAGES);
    ts->color_a = a;
    ts->color_b = b;
    ts->color_c = c;
    ts->color_d = d;
}

void GXSetTevAlphaIn(u32 stage, u32 a, u32 b, u32 c, u32 d) {
    if (stage >= 16) return;
    PCGXTevStage* ts = &g_gx.tev_stages[stage];
    if (pc_gx_state_dedup &&
        ts->alpha_a == (int)a && ts->alpha_b == (int)b &&
        ts->alpha_c == (int)c && ts->alpha_d == (int)d)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_TEV_STAGES);
    ts->alpha_a = a;
    ts->alpha_b = b;
    ts->alpha_c = c;
    ts->alpha_d = d;
}

void GXSetTevColorOp(u32 stage, u32 op, u32 bias, u32 scale, GXBool clamp, u32 out_reg) {
    if (stage >= 16) return;
    PCGXTevStage* ts = &g_gx.tev_stages[stage];
    if (pc_gx_state_dedup &&
        ts->color_op == (int)op && ts->color_bias == (int)bias &&
        ts->color_scale == (int)scale && ts->color_clamp == (int)clamp &&
        ts->color_out == (int)out_reg)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_TEV_STAGES);
    ts->color_op = op;
    ts->color_bias = bias;
    ts->color_scale = scale;
    ts->color_clamp = clamp;
    ts->color_out = out_reg;
}

void GXSetTevAlphaOp(u32 stage, u32 op, u32 bias, u32 scale, GXBool clamp, u32 out_reg) {
    if (stage >= 16) return;
    PCGXTevStage* ts = &g_gx.tev_stages[stage];
    if (pc_gx_state_dedup &&
        ts->alpha_op == (int)op && ts->alpha_bias == (int)bias &&
        ts->alpha_scale == (int)scale && ts->alpha_clamp == (int)clamp &&
        ts->alpha_out == (int)out_reg)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_TEV_STAGES);
    ts->alpha_op = op;
    ts->alpha_bias = bias;
    ts->alpha_scale = scale;
    ts->alpha_clamp = clamp;
    ts->alpha_out = out_reg;
}

void GXSetTevOrder(u32 stage, u32 coord, u32 map, u32 color) {
    if (stage >= 16) return;
    PCGXTevStage* ts = &g_gx.tev_stages[stage];
    if (pc_gx_state_dedup &&
        ts->tex_coord == (int)coord && ts->tex_map == (int)map &&
        ts->color_chan == (int)color)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_TEV_STAGES | PC_GX_DIRTY_TEXTURES);
    ts->tex_coord = coord;
    ts->tex_map = map;
    ts->color_chan = color;
}

void GXSetTevColor(u32 id, u32 color_packed) {
    if (id >= GX_MAX_TEVREG) return;
    /* TEVREG0 uses GXColor fields (byte unpack), others come from EmuColor.raw (shift unpack) */
    float tmp[4];
    if (id == GX_TEVREG0) {
        pc_unpack_gxcolor_f(color_packed, tmp);
    } else {
        pc_unpack_rgba8f(color_packed, tmp);
    }
    if (pc_gx_state_dedup && memcmp(tmp, g_gx.tev_colors[id], sizeof(tmp)) == 0)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_TEV_COLORS);
    memcpy(g_gx.tev_colors[id], tmp, sizeof(tmp));
}

void GXSetTevColorS10(u32 id, s16 r, s16 g, s16 b, s16 a) {
    if (id >= GX_MAX_TEVREG) return;
    float tmp[4] = { r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
    if (pc_gx_state_dedup && memcmp(tmp, g_gx.tev_colors[id], sizeof(tmp)) == 0)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_TEV_COLORS);
    memcpy(g_gx.tev_colors[id], tmp, sizeof(tmp));
}

void GXSetTevKColor(u32 id, u32 color_packed) {
    if (id >= 4) return;
    float tmp[4];
    pc_unpack_rgba8f(color_packed, tmp);
    if (pc_gx_state_dedup && memcmp(tmp, g_gx.tev_k_colors[id], sizeof(tmp)) == 0)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_KONST);
    memcpy(g_gx.tev_k_colors[id], tmp, sizeof(tmp));
}

void GXSetTevKColorSel(u32 stage, u32 sel) {
    if (stage >= 16) return;
    if (pc_gx_state_dedup && g_gx.tev_stages[stage].k_color_sel == (int)sel) return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_TEV_STAGES);
    g_gx.tev_stages[stage].k_color_sel = sel;
}
void GXSetTevKAlphaSel(u32 stage, u32 sel) {
    if (stage >= 16) return;
    if (pc_gx_state_dedup && g_gx.tev_stages[stage].k_alpha_sel == (int)sel) return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_TEV_STAGES);
    g_gx.tev_stages[stage].k_alpha_sel = sel;
}

void GXSetTevSwapMode(u32 stage, u32 ras_sel, u32 tex_sel) {
    if (stage >= 16) return;
    PCGXTevStage* ts = &g_gx.tev_stages[stage];
    if (pc_gx_state_dedup &&
        ts->ras_swap == (int)ras_sel && ts->tex_swap == (int)tex_sel)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_TEV_STAGES);
    ts->ras_swap = ras_sel;
    ts->tex_swap = tex_sel;
}

void GXSetTevSwapModeTable(u32 table, u32 red, u32 green, u32 blue, u32 alpha) {
    if (table >= 4) return;
    PCGXTevSwapTable* st = &g_gx.tev_swap_table[table];
    if (pc_gx_state_dedup &&
        st->r == (int)red && st->g == (int)green &&
        st->b == (int)blue && st->a == (int)alpha)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_SWAP_TABLES);
    st->r = red;
    st->g = green;
    st->b = blue;
    st->a = alpha;
}

/* --- Alpha / Depth / Blend --- */
void GXSetAlphaCompare(u32 comp0, u8 ref0, u32 op, u32 comp1, u8 ref1) {
    if (pc_gx_state_dedup &&
        g_gx.alpha_comp0 == (int)comp0 && g_gx.alpha_ref0 == (int)ref0 &&
        g_gx.alpha_op == (int)op &&
        g_gx.alpha_comp1 == (int)comp1 && g_gx.alpha_ref1 == (int)ref1)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_ALPHA_CMP);
    g_gx.alpha_comp0 = comp0;
    g_gx.alpha_ref0 = ref0;
    g_gx.alpha_op = op;
    g_gx.alpha_comp1 = comp1;
    g_gx.alpha_ref1 = ref1;
}

void GXSetBlendMode(u32 type, u32 src, u32 dst, u32 logic_op) {
    if (pc_gx_state_dedup &&
        g_gx.blend_mode == (int)type && g_gx.blend_src == (int)src &&
        g_gx.blend_dst == (int)dst && g_gx.blend_logic_op == (int)logic_op)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_BLEND);
    g_gx.blend_mode = type;
    g_gx.blend_src = src;
    g_gx.blend_dst = dst;
    g_gx.blend_logic_op = logic_op;
}

void GXSetZMode(GXBool compare_enable, u32 func, GXBool update_enable) {
    if (pc_gx_state_dedup &&
        g_gx.z_compare_enable == (int)compare_enable &&
        g_gx.z_compare_func == (int)func &&
        g_gx.z_update_enable == (int)update_enable)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_DEPTH);
    g_gx.z_compare_enable = compare_enable;
    g_gx.z_compare_func = func;
    g_gx.z_update_enable = update_enable;
}

void GXSetColorUpdate(GXBool enable) {
    if (pc_gx_state_dedup && g_gx.color_update_enable == (int)enable) return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_COLOR_MASK);
    g_gx.color_update_enable = enable;
}
void GXSetAlphaUpdate(GXBool enable) {
    if (pc_gx_state_dedup && g_gx.alpha_update_enable == (int)enable) return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_COLOR_MASK);
    g_gx.alpha_update_enable = enable;
}
void GXSetZCompLoc(GXBool before_tex) { (void)before_tex; }
void GXSetDither(GXBool dither) { (void)dither; }
void GXSetDstAlpha(GXBool enable, u8 alpha) { (void)enable; (void)alpha; }
void GXSetFieldMask(GXBool odd, GXBool even) { (void)odd; (void)even; }
void GXSetFieldMode(GXBool field_mode, GXBool half_aspect) { (void)field_mode; (void)half_aspect; }
void GXSetPixelFmt(u32 pix_fmt, u32 z_fmt) { (void)pix_fmt; (void)z_fmt; }

void GXSetCullMode(u32 mode) {
    int final_mode = g_pc_model_viewer_no_cull ? GX_CULL_NONE : (int)mode;
    if (pc_gx_state_dedup && g_gx.cull_mode == final_mode) return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_CULL);
    g_gx.cull_mode = final_mode;
}
void GXSetCoPlanar(GXBool enable) { (void)enable; }

/* --- Fog --- */
void GXSetFog(u32 type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    float fc[4] = { color.r / 255.0f, color.g / 255.0f,
                    color.b / 255.0f, color.a / 255.0f };
    if (pc_gx_state_dedup &&
        g_gx.fog_type == (int)type &&
        g_gx.fog_start == startz && g_gx.fog_end == endz &&
        g_gx.fog_near == nearz && g_gx.fog_far == farz &&
        memcmp(fc, g_gx.fog_color, sizeof(fc)) == 0)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_FOG);
    g_gx.fog_type = type;
    g_gx.fog_start = startz;
    g_gx.fog_end = endz;
    g_gx.fog_near = nearz;
    g_gx.fog_far = farz;
    memcpy(g_gx.fog_color, fc, sizeof(fc));
}

void GXInitFogAdjTable(void* table, u16 width, f32 projmtx[4][4]) {
    (void)table; (void)width; (void)projmtx;
}
void GXSetFogRangeAdj(GXBool enable, u16 center, void* table) {
    (void)enable; (void)center; (void)table;
}

/* --- Lighting --- */
static int pc_gx_chan_index(u32 chan) {
    switch (chan) {
        case GX_COLOR0:
        case GX_ALPHA0:
        case GX_COLOR0A0:
            return 0;
        case GX_COLOR1:
        case GX_ALPHA1:
        case GX_COLOR1A1:
            return 1;
        default:
            return -1;
    }
}

void GXSetNumChans(u8 nChans) {
    if (pc_gx_state_dedup && g_gx.num_chans == (int)nChans) return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_LIGHTING);
    g_gx.num_chans = nChans;
}

static int pc_gx_chan_ctrl_same(int k, GXBool enable, u32 amb_src, u32 mat_src,
                                u32 light_mask, u32 diff_fn, u32 attn_fn) {
    return g_gx.chan_ctrl_enable[k] == (int)enable &&
           g_gx.chan_ctrl_amb_src[k] == (int)amb_src &&
           g_gx.chan_ctrl_mat_src[k] == (int)mat_src &&
           g_gx.chan_ctrl_light_mask[k] == (int)light_mask &&
           g_gx.chan_ctrl_diff_fn[k] == (int)diff_fn &&
           g_gx.chan_ctrl_attn_fn[k] == (int)attn_fn;
}

void GXSetChanCtrl(u32 chan, GXBool enable, u32 amb_src, u32 mat_src,
                   u32 light_mask, u32 diff_fn, u32 attn_fn) {
    int idx = pc_gx_chan_index(chan);
    if (idx < 0) return;
    int is_combined = (chan >= GX_COLOR0A0);
    int is_alpha = (chan == GX_ALPHA0 || chan == GX_ALPHA1);

    if (pc_gx_state_dedup &&
        ((is_alpha && !is_combined) ||
         pc_gx_chan_ctrl_same(idx * 2, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn)) &&
        ((!is_alpha && !is_combined) ||
         pc_gx_chan_ctrl_same(idx * 2 + 1, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn)))
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_LIGHTING);

    if (!is_alpha || is_combined) {
        g_gx.chan_ctrl_enable[idx * 2] = enable;
        g_gx.chan_ctrl_amb_src[idx * 2] = amb_src;
        g_gx.chan_ctrl_mat_src[idx * 2] = mat_src;
        g_gx.chan_ctrl_light_mask[idx * 2] = light_mask;
        g_gx.chan_ctrl_diff_fn[idx * 2] = diff_fn;
        g_gx.chan_ctrl_attn_fn[idx * 2] = attn_fn;
    }
    if (is_alpha || is_combined) {
        g_gx.chan_ctrl_enable[idx * 2 + 1] = enable;
        g_gx.chan_ctrl_amb_src[idx * 2 + 1] = amb_src;
        g_gx.chan_ctrl_mat_src[idx * 2 + 1] = mat_src;
        g_gx.chan_ctrl_light_mask[idx * 2 + 1] = light_mask;
        g_gx.chan_ctrl_diff_fn[idx * 2 + 1] = diff_fn;
        g_gx.chan_ctrl_attn_fn[idx * 2 + 1] = attn_fn;
    }
}

void GXSetChanAmbColor(u32 chan, u32 color_packed) {
    int idx = pc_gx_chan_index(chan);
    if (idx < 0 || idx >= 2) return;
    float tmp[4];
    pc_unpack_gxcolor_f(color_packed, tmp);
    if (pc_gx_state_dedup && memcmp(tmp, g_gx.chan_amb_color[idx], sizeof(tmp)) == 0)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_LIGHTING);
    memcpy(g_gx.chan_amb_color[idx], tmp, sizeof(tmp));
}

void GXSetChanMatColor(u32 chan, u32 color_packed) {
    int idx = pc_gx_chan_index(chan);
    if (idx < 0 || idx >= 2) return;
    float tmp[4];
    pc_unpack_gxcolor_f(color_packed, tmp);
    if (pc_gx_state_dedup && memcmp(tmp, g_gx.chan_mat_color[idx], sizeof(tmp)) == 0)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_LIGHTING);
    memcpy(g_gx.chan_mat_color[idx], tmp, sizeof(tmp));
}

/* GXLightObj internal layout (from GXPriv.h) */
typedef struct {
    u32 padding[3];
    u32 color;
    f32 a0, a1, a2;
    f32 k0, k1, k2;
    f32 px, py, pz;
    f32 nx, ny, nz;
} PCGXLightObjInternal;

void GXInitLightSpot(void* lt, f32 cutoff, u32 spot_func) {
    PCGXLightObjInternal* l = (PCGXLightObjInternal*)lt;
    f32 a0, a1, a2, r, cr, d;

    if (cutoff <= 0.0f || cutoff > 90.0f)
        spot_func = GX_SP_OFF;

    r = PC_PIf * cutoff / 180.0f;
    cr = cosf(r);
    switch (spot_func) {
    case GX_SP_FLAT:
        a0 = -1000.0f * cr;
        a1 = 1000.0f;
        a2 = 0.0f;
        break;
    case GX_SP_COS:
        a0 = -cr / (1.0f - cr);
        a1 = 1.0f / (1.0f - cr);
        a2 = 0.0f;
        break;
    case GX_SP_COS2:
        a0 = 0.0f;
        a1 = -cr / (1.0f - cr);
        a2 = 1.0f / (1.0f - cr);
        break;
    case GX_SP_SHARP:
        d = (1.0f - cr) * (1.0f - cr);
        a0 = (cr * (cr - 2.0f)) / d;
        a1 = 2.0f / d;
        a2 = -1.0f / d;
        break;
    case GX_SP_RING1:
        d = (1.0f - cr) * (1.0f - cr);
        a0 = (-4.0f * cr) / d;
        a1 = (4.0f * (1.0f + cr)) / d;
        a2 = -4.0f / d;
        break;
    case GX_SP_RING2:
        d = (1.0f - cr) * (1.0f - cr);
        a0 = 1.0f - ((2.0f * cr * cr) / d);
        a1 = (4.0f * cr) / d;
        a2 = -2.0f / d;
        break;
    case GX_SP_OFF:
    default:
        a0 = 1.0f;
        a1 = 0.0f;
        a2 = 0.0f;
        break;
    }
    l->a0 = a0; l->a1 = a1; l->a2 = a2;
}
void GXInitLightDistAttn(void* lt, f32 ref_dist, f32 ref_bright, u32 dist_func) {
    PCGXLightObjInternal* l = (PCGXLightObjInternal*)lt;
    f32 k0, k1, k2;

    if (ref_dist < 0.0f)
        dist_func = GX_DA_OFF;
    if (ref_bright <= 0.0f || ref_bright >= 1.0f)
        dist_func = GX_DA_OFF;

    switch (dist_func) {
    case GX_DA_GENTLE:
        k0 = 1.0f;
        k1 = (1.0f - ref_bright) / (ref_bright * ref_dist);
        k2 = 0.0f;
        break;
    case GX_DA_MEDIUM:
        k0 = 1.0f;
        k1 = (0.5f * (1.0f - ref_bright)) / (ref_bright * ref_dist);
        k2 = (0.5f * (1.0f - ref_bright)) / (ref_bright * ref_dist * ref_dist);
        break;
    case GX_DA_STEEP:
        k0 = 1.0f;
        k1 = 0.0f;
        k2 = (1.0f - ref_bright) / (ref_bright * ref_dist * ref_dist);
        break;
    case GX_DA_OFF:
    default:
        k0 = 1.0f;
        k1 = 0.0f;
        k2 = 0.0f;
        break;
    }
    l->k0 = k0; l->k1 = k1; l->k2 = k2;
}
void GXInitLightPos(void* lt, f32 x, f32 y, f32 z) {
    PCGXLightObjInternal* l = (PCGXLightObjInternal*)lt;
    l->px = x; l->py = y; l->pz = z;
}
void GXInitLightDir(void* lt, f32 nx, f32 ny, f32 nz) {
    PCGXLightObjInternal* l = (PCGXLightObjInternal*)lt;
    l->nx = nx; l->ny = ny; l->nz = nz;
}
void GXInitLightColor(void* lt, u32 color) {
    PCGXLightObjInternal* l = (PCGXLightObjInternal*)lt;
    l->color = color;
}
void GXInitLightAttn(void* lt, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    PCGXLightObjInternal* l = (PCGXLightObjInternal*)lt;
    l->a0 = a0; l->a1 = a1; l->a2 = a2;
    l->k0 = k0; l->k1 = k1; l->k2 = k2;
}
void GXInitLightAttnA(void* lt, f32 a0, f32 a1, f32 a2) {
    PCGXLightObjInternal* l = (PCGXLightObjInternal*)lt;
    l->a0 = a0; l->a1 = a1; l->a2 = a2;
}
void GXInitLightAttnK(void* lt, f32 k0, f32 k1, f32 k2) {
    PCGXLightObjInternal* l = (PCGXLightObjInternal*)lt;
    l->k0 = k0; l->k1 = k1; l->k2 = k2;
}
void GXLoadLightObjImm(void* lt, u32 light) {
    PCGXLightObjInternal* l = (PCGXLightObjInternal*)lt;
    int slot = -1;
    for (int i = 0; i < 8; i++) {
        if (light == (1u << i)) { slot = i; break; }
    }
    if (slot < 0) return;

    float col[4];
    pc_unpack_gxcolor_f(l->color, col);
    if (pc_gx_state_dedup &&
        g_gx.lights[slot].pos[0] == l->px && g_gx.lights[slot].pos[1] == l->py &&
        g_gx.lights[slot].pos[2] == l->pz &&
        g_gx.lights[slot].dir[0] == l->nx && g_gx.lights[slot].dir[1] == l->ny &&
        g_gx.lights[slot].dir[2] == l->nz &&
        g_gx.lights[slot].a0 == l->a0 && g_gx.lights[slot].a1 == l->a1 &&
        g_gx.lights[slot].a2 == l->a2 &&
        g_gx.lights[slot].k0 == l->k0 && g_gx.lights[slot].k1 == l->k1 &&
        g_gx.lights[slot].k2 == l->k2 &&
        memcmp(col, g_gx.lights[slot].color, sizeof(col)) == 0)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_LIGHTING);

    g_gx.lights[slot].pos[0] = l->px;
    g_gx.lights[slot].pos[1] = l->py;
    g_gx.lights[slot].pos[2] = l->pz;
    g_gx.lights[slot].dir[0] = l->nx;
    g_gx.lights[slot].dir[1] = l->ny;
    g_gx.lights[slot].dir[2] = l->nz;
    g_gx.lights[slot].a0 = l->a0;
    g_gx.lights[slot].a1 = l->a1;
    g_gx.lights[slot].a2 = l->a2;
    g_gx.lights[slot].k0 = l->k0;
    g_gx.lights[slot].k1 = l->k1;
    g_gx.lights[slot].k2 = l->k2;
    memcpy(g_gx.lights[slot].color, col, sizeof(col));
}
void GXGetLightPos(void* lt, f32* x, f32* y, f32* z) {
    PCGXLightObjInternal* l = (PCGXLightObjInternal*)lt;
    *x = l->px; *y = l->py; *z = l->pz;
}
void GXGetLightColor(void* lt, void* color) {
    PCGXLightObjInternal* l = (PCGXLightObjInternal*)lt;
    memcpy(color, &l->color, 4);
}

/* --- Texture Coordinate Generation --- */
void GXSetNumTexGens(u8 n) {
    if (pc_gx_state_dedup && g_gx.num_tex_gens == (int)n) return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_TEXGEN);
    g_gx.num_tex_gens = n;
}
void GXSetTexCoordGen2(u32 dst, u32 func, u32 src, u32 mtx, GXBool normalize, u32 postmtx) {
    if (dst >= 8) return;
    if (pc_gx_state_dedup &&
        g_gx.tex_gen_type[dst] == (int)func &&
        g_gx.tex_gen_src[dst] == (int)src &&
        g_gx.tex_gen_mtx[dst] == (int)mtx)
        return;
    pc_gx_flush_if_begin_complete();
    DIRTY(PC_GX_DIRTY_TEXGEN);
    g_gx.tex_gen_type[dst] = func;
    g_gx.tex_gen_src[dst] = src;
    g_gx.tex_gen_mtx[dst] = mtx;
}
void GXSetLineWidth(u8 width, u32 texOffsets) {
#ifndef PC_USE_GLES
    glLineWidth(width / 16.0f);
#endif
    (void)width; (void)texOffsets;
}
void GXSetPointSize(u8 size, u32 texOffsets) {
#ifndef PC_USE_GLES
    glPointSize(size / 16.0f);
#endif
    (void)size; (void)texOffsets;
}
void GXEnableTexOffsets(u32 coord, GXBool line, GXBool point) {
    (void)coord; (void)line; (void)point;
}
void GXSetTexCoordScaleManually(u32 coord, GXBool enable, u16 ss, u16 ts) {
    (void)coord; (void)enable; (void)ss; (void)ts;
}
void GXSetTexCoordBias(u32 coord, u8 s, u8 t) { (void)coord; (void)s; (void)t; }

/* --- Framebuffer / Copy --- */
void GXSetCopyClear(GXColor clear_clr, u32 clear_z) {
    g_gx.clear_color[0] = clear_clr.r / 255.0f;
    g_gx.clear_color[1] = clear_clr.g / 255.0f;
    g_gx.clear_color[2] = clear_clr.b / 255.0f;
    g_gx.clear_color[3] = clear_clr.a / 255.0f;
    g_gx.clear_depth = clear_z / (float)0x00FFFFFF;
}

void GXCopyDisp(void* dest, GXBool clear) {
    /* On PC we render to the back buffer directly; swap happens in VIWaitForRetrace.
     * Just flush pending geometry — do NOT swap or clear here. */
    pc_gx_commit_pending_and_flush();
    (void)dest;
    (void)clear;
}

void GXSetDispCopyGamma(u32 gamma) { (void)gamma; }
void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    g_gx.copy_src[0] = left; g_gx.copy_src[1] = top;
    g_gx.copy_src[2] = wd; g_gx.copy_src[3] = ht;
}
void GXSetDispCopyDst(u16 wd, u16 ht) { g_gx.copy_dst[0] = wd; g_gx.copy_dst[1] = ht; }
f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) {
    return (f32)xfbHeight / (f32)efbHeight;
}
u32 GXSetDispCopyYScale(f32 vscale) { return (u32)(vscale * 256.0f); }
u16 GXGetNumXfbLines(u16 efbHeight, f32 yScale) { return (u16)(efbHeight * yScale); }
void GXSetCopyFilter(GXBool aa, const void* pattern, GXBool vf, const void* vfilter) {
    if (g_pc_gx_dl.active) {
        u32 op = PCGX_DL_OP_COPY_FILTER;
        pc_gx_dl_write(&op, sizeof(op));
        return;
    }
    (void)aa; (void)pattern; (void)vf; (void)vfilter;
}
void GXAdjustForOverscan(void* rmin, void* rmout, u16 hor, u16 ver) {
    memcpy(rmout, rmin, sizeof(u32) * 16);
}

static void pc_gx_copy_tex_execute(void* dest, GXBool clear) {
    pc_gx_commit_pending_and_flush();

    if (!dest) return;

    int out_wd = g_gx.tex_copy_src[2];
    int out_ht = g_gx.tex_copy_src[3];
    if (out_wd <= 0 || out_ht <= 0) return;
    if (out_wd > 4096 || out_ht > 4096) return;

#ifdef PC_ENHANCEMENTS
    /* Scale readback coordinates from GC coords to render resolution */
    float sx = (float)g_pc_render_w / (float)PC_GC_WIDTH;
    float sy = (float)g_pc_render_h / (float)PC_GC_HEIGHT;
    int read_left = (int)(g_gx.tex_copy_src[0] * sx);
    int read_top  = (int)(g_gx.tex_copy_src[1] * sy);
    int read_wd   = (int)(out_wd * sx);
    int read_ht   = (int)(out_ht * sy);
#else
    int read_left = g_gx.tex_copy_src[0];
    int read_top  = g_gx.tex_copy_src[1];
    int read_wd   = out_wd;
    int read_ht   = out_ht;
#endif

    if (read_left < 0) { read_wd += read_left; read_left = 0; }
    if (read_top < 0)  { read_ht += read_top;  read_top = 0; }
    if (read_left + read_wd > g_pc_render_w) read_wd = g_pc_render_w - read_left;
    if (read_top + read_ht > g_pc_render_h)  read_ht = g_pc_render_h - read_top;
    if (read_wd <= 0 || read_ht <= 0) return;

    int gl_y = g_pc_render_h - (read_top + read_ht);
    if (gl_y < 0) return;

#ifdef PC_ENHANCEMENTS
    /* GPU-side EFB copy: blit framebuffer region into a texture via FBO.
     * Avoids the glReadPixels GPU→CPU→GPU round-trip which stalls the pipeline
     * and is extremely expensive on mobile/embedded GPUs (Mali, Adreno, etc.). */
    {
        GLuint efb_tex;
        glGenTextures(1, &efb_tex);
        glBindTexture(GL_TEXTURE_2D, efb_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, read_wd, read_ht, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        /* Create persistent FBO on first use */
        if (!s_efb_copy_fbo)
            glGenFramebuffers(1, &s_efb_copy_fbo);

        /* Attach destination texture to FBO, blit with Y-flip */
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_efb_copy_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, efb_tex, 0);

        /* Source: default framebuffer region (bottom-up GL coords).
         * Dest: texture via FBO, with Y flipped (src bottom→dst top) so the
         * texture ends up in top-down order matching GC convention. */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBlitFramebuffer(
            read_left, gl_y, read_left + read_wd, gl_y + read_ht,  /* src: GL bottom-up */
            0, read_ht, read_wd, 0,                                 /* dst: flipped Y */
            GL_COLOR_BUFFER_BIT, GL_LINEAR);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

        pc_gx_efb_capture_store((u32)(uintptr_t)dest, efb_tex);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
#else
    {
        size_t rgba_size = (size_t)read_wd * (size_t)read_ht * 4;
        u8* rgba = (u8*)malloc(rgba_size);
        if (!rgba) return;

        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(read_left, gl_y, read_wd, read_ht, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

        if (g_gx.tex_copy_fmt == 0x4) {
            u8* out = (u8*)dest;
            int bw = (out_wd + 3) / 4;
            int bh = (out_ht + 3) / 4;

            for (int by = 0; by < bh; by++) {
                for (int bx = 0; bx < bw; bx++) {
                    for (int y = 0; y < 4; y++) {
                        for (int x = 0; x < 4; x++) {
                            int px = bx * 4 + x;
                            int py = by * 4 + y;
                            u16 rgb565 = 0;

                            if (px < out_wd && py < out_ht) {
                                int src_x = px * read_wd / out_wd;
                                int src_y = py * read_ht / out_ht;
                                src_y = read_ht - 1 - src_y;
                                const u8* p = &rgba[(src_y * read_wd + src_x) * 4];
                                rgb565 = (u16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
                            }

                            out[0] = (u8)((rgb565 >> 8) & 0xFF);
                            out[1] = (u8)(rgb565 & 0xFF);
                            out += 2;
                        }
                    }
                }
            }
        }
        free(rgba);
    }
#endif
    (void)clear;
}

void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    if (g_pc_gx_dl.active) {
        u32 pkt[5] = { PCGX_DL_OP_TEXCOPY_SRC, left, top, wd, ht };
        pc_gx_dl_write(pkt, sizeof(pkt));
        return;
    }
    g_gx.tex_copy_src[0] = left;
    g_gx.tex_copy_src[1] = top;
    g_gx.tex_copy_src[2] = wd;
    g_gx.tex_copy_src[3] = ht;
}
void GXSetTexCopyDst(u16 wd, u16 ht, u32 fmt, GXBool mipmap) {
    if (g_pc_gx_dl.active) {
        u32 pkt[5] = { PCGX_DL_OP_TEXCOPY_DST, wd, ht, fmt, mipmap ? 1u : 0u };
        pc_gx_dl_write(pkt, sizeof(pkt));
        return;
    }
    g_gx.tex_copy_dst[0] = wd;
    g_gx.tex_copy_dst[1] = ht;
    g_gx.tex_copy_fmt = fmt;
    g_gx.tex_copy_mipmap = mipmap ? 1 : 0;
}
void GXCopyTex(void* dest, GXBool clear) {
    if (g_pc_gx_dl.active) {
        u32 op = PCGX_DL_OP_COPY_TEX;
        u64 dest64 = (u64)(uintptr_t)dest;
        u32 clear_u32 = clear ? 1u : 0u;
        pc_gx_dl_write(&op, sizeof(op));
        pc_gx_dl_write(&dest64, sizeof(dest64));
        pc_gx_dl_write(&clear_u32, sizeof(clear_u32));
        return;
    }

    pc_gx_copy_tex_execute(dest, clear);
}
void GXSetCopyClamp(u32 clamp) { (void)clamp; }

/* --- GX Init / Management --- */
void* GXInit(void* base, u32 size) {
    (void)base; (void)size;
    return base;
}

void GXSetMisc(u32 token, u32 val) { (void)token; (void)val; }
void GXFlush(void) { if (!g_pc_frameskip_active) glFlush(); }
void GXResetWriteGatherPipe(void) {}
void GXAbortFrame(void) {}
void GXSetDrawSync(u16 token) { (void)token; }
u16  GXReadDrawSync(void) { return 0; }
void GXSetDrawDone(void) {}
void GXWaitDrawDone(void) {}
void GXDrawDone(void) {}
void GXPixModeSync(void) {}
void GXTexModeSync(void) {}

void* GXSetDrawSyncCallback(void* cb) { return NULL; }
void* GXSetDrawDoneCallback(void* cb) { return NULL; }

/* --- FIFO --- */
typedef struct { u8 pad[128]; } GXFifoObj;
void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size) { (void)fifo; (void)base; (void)size; }
void GXInitFifoPtrs(GXFifoObj* fifo, void* rp, void* wp) { (void)fifo; (void)rp; (void)wp; }
void GXInitFifoLimits(GXFifoObj* fifo, u32 hi, u32 lo) { (void)fifo; (void)hi; (void)lo; }
void GXSetCPUFifo(GXFifoObj* fifo) { (void)fifo; }
void GXSetGPFifo(GXFifoObj* fifo) { (void)fifo; }
void GXSaveCPUFifo(GXFifoObj* fifo) { (void)fifo; }
void GXSaveGPFifo(GXFifoObj* fifo) { (void)fifo; }
void GXGetGPStatus(GXBool* a, GXBool* b, GXBool* c, GXBool* d, GXBool* e) {
    if (a) *a = 0;
    if (b) *b = 0;
    if (c) *c = 1;
    if (d) *d = 1;
    if (e) *e = 0;
}
void GXGetFifoStatus(GXFifoObj* f, GXBool* a, GXBool* b, u32* c, GXBool* d, GXBool* e, GXBool* g) {
    if (a) *a = 0;
    if (b) *b = 0;
    if (c) *c = 0;
    if (d) *d = 0;
    if (e) *e = 0;
    if (g) *g = 0;
}
void GXGetFifoPtrs(GXFifoObj* f, void** rp, void** wp) { if (rp) *rp = NULL; if (wp) *wp = NULL; }
void* GXGetFifoBase(GXFifoObj* f) { return NULL; }
u32 GXGetFifoSize(GXFifoObj* f) { return 0; }
void GXGetFifoLimits(GXFifoObj* f, u32* hi, u32* lo) { if (hi) *hi = 0; if (lo) *lo = 0; }
void* GXSetBreakPtCallback(void* cb) { return NULL; }
void GXEnableBreakPt(void* bp) { (void)bp; }
void GXDisableBreakPt(void) {}
void* GXSetCurrentGXThread(void) { return NULL; }
void* GXGetCurrentGXThread(void) { return NULL; }
GXFifoObj* GXGetCPUFifo(void) { static GXFifoObj f; return &f; }
GXFifoObj* GXGetGPFifo(void) { static GXFifoObj f; return &f; }
u32 GXGetOverflowCount(void) { return 0; }
u32 GXResetOverflowCount(void) { return 0; }
volatile void* GXRedirectWriteGatherPipe(void* ptr) { return ptr; }
void GXRestoreWriteGatherPipe(void) {}
int IsWriteGatherBufferEmpty(void) { return 1; }

/* --- Display List --- */
void GXBeginDisplayList(void* list, u32 size) {
    g_pc_gx_dl.active = 1;
    g_pc_gx_dl.buf = (u8*)list;
    g_pc_gx_dl.size = size;
    g_pc_gx_dl.off = 0;
    g_pc_gx_dl.overflow = 0;
}
u32 GXEndDisplayList(void) {
    u32 nbytes = 0;
    if (g_pc_gx_dl.active && !g_pc_gx_dl.overflow) {
        nbytes = g_pc_gx_dl.off;
    }
    g_pc_gx_dl.active = 0;
    g_pc_gx_dl.buf = NULL;
    g_pc_gx_dl.size = 0;
    g_pc_gx_dl.off = 0;
    g_pc_gx_dl.overflow = 0;
    return nbytes;
}
void GXCallDisplayList(void* list, u32 nbytes) {
    if (!list || nbytes == 0) return;

    const u8* p = (const u8*)list;
    u32 off = 0;

    while (off + sizeof(u32) <= nbytes) {
        u32 op = 0;
        memcpy(&op, p + off, sizeof(op));
        off += sizeof(op);

        switch (op) {
            case PCGX_DL_OP_TEXCOPY_SRC: {
                u32 v[4];
                if (off + sizeof(v) > nbytes) return;
                memcpy(v, p + off, sizeof(v));
                off += sizeof(v);
                GXSetTexCopySrc((u16)v[0], (u16)v[1], (u16)v[2], (u16)v[3]);
                break;
            }
            case PCGX_DL_OP_TEXCOPY_DST: {
                u32 v[4];
                if (off + sizeof(v) > nbytes) return;
                memcpy(v, p + off, sizeof(v));
                off += sizeof(v);
                GXSetTexCopyDst((u16)v[0], (u16)v[1], v[2], (GXBool)(v[3] ? 1 : 0));
                break;
            }
            case PCGX_DL_OP_COPY_FILTER:
                break;
            case PCGX_DL_OP_COPY_TEX: {
                u64 dest64 = 0;
                u32 clear = 0;
                if (off + sizeof(dest64) + sizeof(clear) > nbytes) return;
                memcpy(&dest64, p + off, sizeof(dest64));
                off += sizeof(dest64);
                memcpy(&clear, p + off, sizeof(clear));
                off += sizeof(clear);
                pc_gx_copy_tex_execute((void*)(uintptr_t)dest64, (GXBool)(clear ? 1 : 0));
                break;
            }
            default:
                return;
        }
    }
}

/* --- Indirect Texture --- */
void GXSetTevIndirect(u32 stage, u32 ind_stage, u32 fmt, u32 bias_sel,
                      u32 mtx_sel, u32 wrap_s, u32 wrap_t, GXBool add_prev,
                      GXBool ind_lod, u32 alpha_sel);

void GXSetTevDirect(u32 stage) {
    GXSetTevIndirect(stage, 0/*GX_INDTEXSTAGE0*/, 0/*GX_ITF_8*/, 0/*GX_ITB_NONE*/,
                     0/*GX_ITM_OFF*/, 0/*GX_ITW_OFF*/, 0/*GX_ITW_OFF*/, 0, 0, 0/*GX_ITBA_OFF*/);
}
void GXSetNumIndStages(u8 n) { DIRTY(PC_GX_DIRTY_INDIRECT); g_gx.num_ind_stages = n; }

void GXSetIndTexMtx(u32 mtx_sel, const void* offset, s8 scale) {
    DIRTY(PC_GX_DIRTY_INDIRECT);
    int id;
    switch (mtx_sel) {
        case 1: case 2: case 3:   id = mtx_sel - 1; break;
        case 5: case 6: case 7:   id = mtx_sel - 5; break;
        case 9: case 10: case 11: id = mtx_sel - 9; break;
        default: return;
    }
    if (id < 0 || id >= 3) return;
    const float* mtx = (const float*)offset;
    g_gx.ind_mtx[id][0][0] = mtx[0];
    g_gx.ind_mtx[id][0][1] = mtx[1];
    g_gx.ind_mtx[id][0][2] = mtx[2];
    g_gx.ind_mtx[id][1][0] = mtx[3];
    g_gx.ind_mtx[id][1][1] = mtx[4];
    g_gx.ind_mtx[id][1][2] = mtx[5];
    g_gx.ind_mtx_scale[id] = scale;
}

void GXSetIndTexOrder(u32 ind_stage, u32 tex_coord, u32 tex_map) {
    DIRTY(PC_GX_DIRTY_INDIRECT);
    if (ind_stage >= 4) return;
    g_gx.ind_order[ind_stage].tex_coord = tex_coord;
    g_gx.ind_order[ind_stage].tex_map = tex_map;
}

void GXSetTevIndirect(u32 stage, u32 ind_stage, u32 fmt, u32 bias_sel,
                      u32 mtx_sel, u32 wrap_s, u32 wrap_t, GXBool add_prev,
                      GXBool ind_lod, u32 alpha_sel) {
    DIRTY(PC_GX_DIRTY_INDIRECT);
    if (stage >= 16) return;
    PCGXTevStage* s = &g_gx.tev_stages[stage];
    s->ind_stage  = ind_stage;
    s->ind_format = fmt;
    s->ind_bias   = bias_sel;
    s->ind_mtx    = mtx_sel;
    s->ind_wrap_s = wrap_s;
    s->ind_wrap_t = wrap_t;
    s->ind_add_prev = add_prev;
    s->ind_lod    = ind_lod;
    s->ind_alpha  = alpha_sel;
}

void GXSetTevIndWarp(u32 stage, u32 ind_stage, GXBool signed_ofs, GXBool replace, u32 mtx_sel) {
    u32 wrap = replace ? 6/*GX_ITW_0*/ : 0/*GX_ITW_OFF*/;
    u32 bias = signed_ofs ? 7/*GX_ITB_STU*/ : 0/*GX_ITB_NONE*/;
    GXSetTevIndirect(stage, ind_stage, 0/*GX_ITF_8*/, bias, mtx_sel, wrap, wrap, 0, 0, 0);
}

void GXSetIndTexCoordScale(u32 ind_stage, u32 scale_s, u32 scale_t) {
    DIRTY(PC_GX_DIRTY_INDIRECT);
    if (ind_stage >= 4) return;
    g_gx.ind_order[ind_stage].scale_s = scale_s;
    g_gx.ind_order[ind_stage].scale_t = scale_t;
}

void __GXSetIndirectMask(u32 mask) { (void)mask; }

/* --- Z Texture --- */
void GXSetZTexture(u32 op, u32 fmt, u32 bias) { (void)op; (void)fmt; (void)bias; }

/* --- Draw Utility --- */
void GXDrawSphere(u8 numMajor, u8 numMinor) { (void)numMajor; (void)numMinor; }

/* --- Perf --- */
void GXReadXfRasMetric(u32* xf_wait_in, u32* xf_wait_out, u32* ras_busy, u32* clocks) {
    if (xf_wait_in) *xf_wait_in = 0;
    if (xf_wait_out) *xf_wait_out = 0;
    if (ras_busy) *ras_busy = 0;
    if (clocks) *clocks = 0;
}

/* --- Verify --- */
void GXSetVerifyLevel(u32 level) { (void)level; }
void* GXSetVerifyCallback(void* cb) { return NULL; }

