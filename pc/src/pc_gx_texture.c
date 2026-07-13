/* pc_gx_texture.c - GC texture format decoders + 2048-entry texture cache */
#include "pc_gx_internal.h"
#include "pc_texture_pack.h"
#include <dolphin/gx/GXEnum.h>
#include <stdlib.h>

static int pc_gx_tlut_force_be(void);
static void decode_rgb5a3_entry(u16 val, u8* r, u8* g, u8* b, u8* a);
static u32 tlut_content_hash(const void* data, int tlut_fmt, int n_entries, int is_be);

/* --- TLUT stale-data detection ---
 * On GC, gsDPLoadTLUT_Dolphin always re-DMA'd palette data from memory.
 * emu64 optimizes by skipping reload when the TLUT address hasn't changed.
 * On PC, the game reuses memory buffers — same address can hold different
 * palette data (e.g., different NPC clothing). We track the first u16 of
 * each TLUT slot to detect content changes and force reloads. */
u16 s_tlut_first_word[16];

/* --- texture cache --- */
#define TEX_CACHE_SIZE 2048

typedef struct {
    u32 data_ptr;
    u16 width;
    u16 height;
    u32 format;
    u32 tlut_name;   /* TLUT slot for CI4/CI8, 0xFFFFFFFF for non-indexed */
    u32 tlut_ptr;
    u32 tlut_hash;   /* FNV-1a of TLUT data + metadata */
    u32 data_hash;   /* FNV-1a of first N bytes */
    GLuint gl_tex;
    u32 wrap_s;
    u32 wrap_t;
    u32 min_filter;
    u8 external;     /* owned by texture pack, don't delete on eviction */
} TexCacheEntry;

/* Bits-per-pixel for each GC texture format (8 for unknown as safe default) */
static int gc_format_bpp(u32 format) {
    switch (format) {
        case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR: return 4;
        case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: return 8;
        case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3: return 16;
        case GX_TF_RGBA8: return 32;
        default: return 8;
    }
}

/* FNV-1a hash of texture data to detect buffer reuse with different content.
 * Hashes first 256 + last 256 bytes (or all if <= 512). */
static u32 tex_content_hash(const void* data, int width, int height, u32 format) {
    if (!data) return 0;
    int bpp = gc_format_bpp(format);
    int data_size = (width * height * bpp) / 8;
    const u8* p = (const u8*)data;
    u32 h = 0x811c9dc5u;
    if (data_size <= 512) {
        /* small texture: hash everything */
        for (int i = 0; i < data_size; i++) {
            h ^= p[i];
            h *= 0x01000193u;
        }
    } else {
        /* large texture: sample head + tail */
        for (int i = 0; i < 256; i++) {
            h ^= p[i];
            h *= 0x01000193u;
        }
        for (int i = data_size - 256; i < data_size; i++) {
            h ^= p[i];
            h *= 0x01000193u;
        }
    }
    return h;
}

/* FNV-1a hash of TLUT data + metadata. CI textures need palette identity
 * in the cache key since the same image is often reused with different TLUTs. */
static u32 tlut_content_hash(const void* data, int tlut_fmt, int n_entries, int is_be) {
    if (!data || n_entries <= 0) return 0;
    int bytes = n_entries * 2;
    if (bytes > 512) bytes = 512;
    const u8* p = (const u8*)data;
    u32 h = 0x811c9dc5u;
    for (int i = 0; i < bytes; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    /* mix in metadata to distinguish different TLUT modes */
    h ^= (u32)(tlut_fmt & 0xFF); h *= 0x01000193u;
    h ^= (u32)(n_entries & 0xFFFF); h *= 0x01000193u;
    h ^= (u32)(is_be & 1); h *= 0x01000193u;
    return h;
}

static TexCacheEntry tex_cache[TEX_CACHE_SIZE];
static int tex_cache_count = 0;
static int tex_cache_hits = 0;
static int tex_cache_misses = 0;

/* Open-addressed hash index over tex_cache. Slots store entry_index+1
 * (0 = empty). Deletions only happen in bulk (eviction rebuilds, invalidate
 * clears), so no tombstones needed. */
#define TEX_INDEX_SIZE 4096
static u16 tex_index[TEX_INDEX_SIZE];

/* FNV-1a over the 8-field cache key */
static u32 tex_key_hash(u32 data_ptr, u32 w, u32 h, u32 fmt, u32 tlut_name,
                        u32 tlut_ptr, u32 tlut_hash, u32 data_hash) {
    u32 key[8] = { data_ptr, w, h, fmt, tlut_name, tlut_ptr, tlut_hash, data_hash };
    u32 hash = 0x811c9dc5u;
    for (int i = 0; i < 8; i++) {
        u32 v = key[i];
        for (int b = 0; b < 4; b++) {
            hash ^= (v >> (b * 8)) & 0xFF;
            hash *= 0x01000193u;
        }
    }
    return hash;
}

static void tex_index_insert(int entry_idx) {
    TexCacheEntry* e = &tex_cache[entry_idx];
    u32 slot = tex_key_hash(e->data_ptr, e->width, e->height, e->format, e->tlut_name,
                            e->tlut_ptr, e->tlut_hash, e->data_hash) & (TEX_INDEX_SIZE - 1);
    while (tex_index[slot] != 0)
        slot = (slot + 1) & (TEX_INDEX_SIZE - 1);
    tex_index[slot] = (u16)(entry_idx + 1);
}

static void tex_index_rebuild(void) {
    memset(tex_index, 0, sizeof(tex_index));
    for (int i = 0; i < tex_cache_count; i++)
        tex_index_insert(i);
}

/* O(1) hash lookup with linear probing */
static TexCacheEntry* tex_cache_find(u32 data_ptr, int w, int h, u32 fmt, u32 tlut_name,
                                     u32 tlut_ptr, u32 tlut_hash, u32 data_hash) {
    u32 slot = tex_key_hash(data_ptr, (u32)w, (u32)h, fmt, tlut_name,
                            tlut_ptr, tlut_hash, data_hash) & (TEX_INDEX_SIZE - 1);
    for (int i = 0; i < TEX_INDEX_SIZE; i++) {
        u16 idx = tex_index[slot];
        if (idx == 0)
            return NULL;
        TexCacheEntry* e = &tex_cache[idx - 1];
        if (e->data_ptr == data_ptr && e->width == w && e->height == h &&
            e->format == fmt && e->tlut_name == tlut_name && e->tlut_ptr == tlut_ptr &&
            e->tlut_hash == tlut_hash && e->data_hash == data_hash) {
            return e;
        }
        slot = (slot + 1) & (TEX_INDEX_SIZE - 1);
    }
    return NULL;
}

static TexCacheEntry* tex_cache_insert(u32 data_ptr, int w, int h, u32 fmt, u32 tlut_name,
                                       u32 tlut_ptr, u32 tlut_hash, u32 data_hash, GLuint gl_tex) {
    if (tex_cache_count >= TEX_CACHE_SIZE) {
        /* evict oldest half */
        int half = TEX_CACHE_SIZE / 2;
        for (int i = 0; i < half; i++) {
            if (tex_cache[i].gl_tex) {
                for (int s = 0; s < 8; s++) {
                    if (g_gx.gl_textures[s] == tex_cache[i].gl_tex)
                        g_gx.gl_textures[s] = 0;
                }
                if (!tex_cache[i].external)
                    glDeleteTextures(1, &tex_cache[i].gl_tex);
            }
        }
        memmove(&tex_cache[0], &tex_cache[half], (tex_cache_count - half) * sizeof(TexCacheEntry));
        tex_cache_count -= half;
        tex_index_rebuild();
    }
    TexCacheEntry* e = &tex_cache[tex_cache_count++];
    e->data_ptr = data_ptr;
    e->width = (u16)w;
    e->height = (u16)h;
    e->format = fmt;
    e->tlut_name = tlut_name;
    e->tlut_ptr = tlut_ptr;
    e->tlut_hash = tlut_hash;
    e->data_hash = data_hash;
    e->gl_tex = gl_tex;
    e->wrap_s = 0xFFFFFFFF;
    e->wrap_t = 0xFFFFFFFF;
    e->min_filter = 0xFFFFFFFF;
    e->external = 0;
    tex_index_insert(tex_cache_count - 1);
    return e;
}

void pc_gx_texture_cache_invalidate(void) {
    for (int i = 0; i < tex_cache_count; i++) {
        if (tex_cache[i].gl_tex && !tex_cache[i].external) {
            glDeleteTextures(1, &tex_cache[i].gl_tex);
        }
    }
    tex_cache_count = 0;
    memset(tex_index, 0, sizeof(tex_index));
}

void pc_gx_texture_init(void) {
    tex_cache_count = 0;
    tex_cache_hits = 0;
    tex_cache_misses = 0;
    (void)pc_gx_tlut_force_be();
}

void pc_gx_texture_shutdown(void) {
    pc_gx_texture_cache_invalidate();
    memset(g_gx.gl_textures, 0, sizeof(g_gx.gl_textures));
}

/* --- texture object API --- */

/* GXTexObj layout for PC: 22 u32s (88 bytes) */
#define TEXOBJ_IMAGE_PTR   0
#define TEXOBJ_WIDTH       1
#define TEXOBJ_HEIGHT      2
#define TEXOBJ_FORMAT      3
#define TEXOBJ_WRAP_S      4
#define TEXOBJ_WRAP_T      5
#define TEXOBJ_MIPMAP      6
#define TEXOBJ_MIN_FILTER  7
#define TEXOBJ_MAG_FILTER  8
#define TEXOBJ_MIN_LOD     9
#define TEXOBJ_MAX_LOD     10
#define TEXOBJ_LOD_BIAS    11
#define TEXOBJ_BIAS_CLAMP  12
#define TEXOBJ_EDGE_LOD    13
#define TEXOBJ_MAX_ANISO   14
#define TEXOBJ_GL_TEX      15
#define TEXOBJ_CI_FORMAT   16
#define TEXOBJ_TLUT_NAME   17
#define TEXOBJ_SIZE        22  /* total u32 count */

/* GXTlutObj layout (4 u32s) */
#define TLUTOBJ_DATA       0
#define TLUTOBJ_FORMAT     1
#define TLUTOBJ_N_ENTRIES  2

void GXInitTexObj(void* obj, void* image_ptr, u16 width, u16 height, u32 format,
                  u32 wrap_s, u32 wrap_t, u8 mipmap) {
    u32* o = (u32*)obj;
    memset(o, 0, TEXOBJ_SIZE * sizeof(u32));
    o[TEXOBJ_IMAGE_PTR] = (u32)(uintptr_t)image_ptr;
    o[TEXOBJ_WIDTH] = width;
    o[TEXOBJ_HEIGHT] = height;
    o[TEXOBJ_FORMAT] = format;
    o[TEXOBJ_WRAP_S] = wrap_s;
    o[TEXOBJ_WRAP_T] = wrap_t;
    o[TEXOBJ_MIPMAP] = mipmap;
    o[TEXOBJ_MIN_FILTER] = 1; /* GX_LINEAR */
    o[TEXOBJ_MAG_FILTER] = 1; /* GX_LINEAR */
}

void GXInitTexObjCI(void* obj, void* image_ptr, u16 width, u16 height, u32 format,
                    u32 wrap_s, u32 wrap_t, u8 mipmap, u32 tlut_name) {
    GXInitTexObj(obj, image_ptr, width, height, format, wrap_s, wrap_t, mipmap);
    u32* o = (u32*)obj;
    o[TEXOBJ_CI_FORMAT] = format;
    o[TEXOBJ_TLUT_NAME] = tlut_name;
}

void GXInitTexObjData(void* obj, void* image_ptr) {
    u32* o = (u32*)obj;
    o[TEXOBJ_IMAGE_PTR] = (u32)(uintptr_t)image_ptr;
}

void GXInitTexObjLOD(void* obj, u32 min_filt, u32 mag_filt, f32 min_lod, f32 max_lod,
                     f32 lod_bias, GXBool bias_clamp, GXBool edge_lod, u32 max_aniso) {
    u32* o = (u32*)obj;
    o[TEXOBJ_MIN_FILTER] = min_filt;
    o[TEXOBJ_MAG_FILTER] = mag_filt;
    /* store floats as bits */
    memcpy(&o[TEXOBJ_MIN_LOD], &min_lod, sizeof(f32));
    memcpy(&o[TEXOBJ_MAX_LOD], &max_lod, sizeof(f32));
    memcpy(&o[TEXOBJ_LOD_BIAS], &lod_bias, sizeof(f32));
    o[TEXOBJ_BIAS_CLAMP] = bias_clamp;
    o[TEXOBJ_EDGE_LOD] = edge_lod;
    o[TEXOBJ_MAX_ANISO] = max_aniso;
}

void GXInitTexObjWrapMode(void* obj, u32 s, u32 t) {
    u32* o = (u32*)obj;
    o[TEXOBJ_WRAP_S] = s;
    o[TEXOBJ_WRAP_T] = t;
}

/* --- GC texture format decoders (tile layout -> linear RGBA8) --- */

/* Read a big-endian u16 (GC textures store 16-bit pixels in BE order) */
static inline u16 read_be16(const u8* p) {
    return (u16)((p[0] << 8) | p[1]);
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

/* PC_NO_NEON_DECODE=1 routes all decoders to the scalar paths (triage) */
static int pc_tex_no_neon(void) {
    static int v = -1;
    if (v < 0) v = (getenv("PC_NO_NEON_DECODE") != NULL);
    return v;
}
/* Exact lookup tables matching the scalar truncating expansions.
 * Note: bit replication (v<<3)|(v>>2) does NOT match v*255/31 for all v
 * (e.g. v=4: 33 vs 32), so tables keep NEON output byte-identical. */
static const u8 s_exp5_tbl[32] = { /* v*255/31 */
    0, 8, 16, 24, 32, 41, 49, 57, 65, 74, 82, 90, 98, 106, 115, 123,
    131, 139, 148, 156, 164, 172, 180, 189, 197, 205, 213, 222, 230, 238, 246, 255
};
static const u8 s_exp3_tbl[8] = { /* v*255/7 */
    0, 36, 72, 109, 145, 182, 218, 255
};

/* g*255/63 == 4g + floor(g/21) exactly; floor(g/21) via three compares
 * (vcge yields all-ones == -1, so subtracting adds 1). */
static inline uint8x8_t neon_expand6(uint16x8_t g6) {
    uint16x8_t g8 = vshlq_n_u16(g6, 2);
    g8 = vsubq_u16(g8, vcgeq_u16(g6, vdupq_n_u16(21)));
    g8 = vsubq_u16(g8, vcgeq_u16(g6, vdupq_n_u16(42)));
    g8 = vsubq_u16(g8, vcgeq_u16(g6, vdupq_n_u16(63)));
    return vmovn_u16(g8);
}
#endif

/* I4: 8x8 blocks, 4bpp, each byte = 2 pixels */
static void decode_I4(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 7) / 8, bh = (h + 7) / 8;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x += 2) {
                    u8 val = *src++;
                    int px0 = bx * 8 + x, py = by * 8 + y;
                    int px1 = px0 + 1;
                    u8 i0 = (val >> 4) | (val & 0xF0);
                    u8 i1 = (val & 0x0F) | ((val & 0x0F) << 4);
                    if (px0 < w && py < h) {
                        int idx = (py * w + px0) * 4;
                        dst[idx] = dst[idx+1] = dst[idx+2] = i0;
                        dst[idx+3] = i0;
                    }
                    if (px1 < w && py < h) {
                        int idx = (py * w + px1) * 4;
                        dst[idx] = dst[idx+1] = dst[idx+2] = i1;
                        dst[idx+3] = i1;
                    }
                }
            }
        }
    }
}

/* I8: 8x4 blocks, 8bpp */
static void decode_I8(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 7) / 8, bh = (h + 3) / 4;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (!pc_tex_no_neon()) {
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            if ((bx + 1) * 8 <= w && (by + 1) * 4 <= h) {
                /* fully-interior block: 8 pixels per row, RGBA = IIII */
                for (int y = 0; y < 4; y++) {
                    uint8x8_t v = vld1_u8(src); src += 8;
                    uint8x8x4_t pix;
                    pix.val[0] = v; pix.val[1] = v; pix.val[2] = v; pix.val[3] = v;
                    vst4_u8(&dst[((by * 4 + y) * w + bx * 8) * 4], pix);
                }
            } else {
                /* edge block: scalar with bounds checks */
                for (int y = 0; y < 4; y++) {
                    for (int x = 0; x < 8; x++) {
                        u8 val = *src++;
                        int px = bx * 8 + x, py = by * 4 + y;
                        if (px < w && py < h) {
                            int idx = (py * w + px) * 4;
                            dst[idx] = dst[idx+1] = dst[idx+2] = val;
                            dst[idx+3] = val;
                        }
                    }
                }
            }
        }
    }
    return;
    }
#endif
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 8; x++) {
                    u8 val = *src++;
                    int px = bx * 8 + x, py = by * 4 + y;
                    if (px < w && py < h) {
                        int idx = (py * w + px) * 4;
                        dst[idx] = dst[idx+1] = dst[idx+2] = val;
                        dst[idx+3] = val;
                    }
                }
            }
        }
    }
}

/* IA4: 8x4 blocks, high nibble = alpha, low nibble = intensity */
static void decode_IA4(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 7) / 8, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 8; x++) {
                    u8 val = *src++;
                    int px = bx * 8 + x, py = by * 4 + y;
                    if (px < w && py < h) {
                        u8 a = (val >> 4) | (val & 0xF0);
                        u8 i = (val & 0x0F) | ((val & 0x0F) << 4);
                        int idx = (py * w + px) * 4;
                        dst[idx] = dst[idx+1] = dst[idx+2] = i; dst[idx+3] = a;
                    }
                }
            }
        }
    }
}

/* IA8: 4x4 blocks, 16bpp, alpha byte + intensity byte per pixel */
static void decode_IA8(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (!pc_tex_no_neon()) {
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            if ((bx + 1) * 4 <= w && (by + 1) * 4 <= h) {
                /* fully-interior block: 8 pixels (2 rows) per iteration */
                for (int y = 0; y < 4; y += 2) {
                    uint8x8x2_t ai = vld2_u8(src); src += 16; /* val[0]=A, val[1]=I */
                    uint8x8x4_t pix;
                    pix.val[0] = ai.val[1]; pix.val[1] = ai.val[1];
                    pix.val[2] = ai.val[1]; pix.val[3] = ai.val[0];
                    u8 tmp[32];
                    vst4_u8(tmp, pix);
                    vst1q_u8(&dst[((by * 4 + y) * w + bx * 4) * 4], vld1q_u8(tmp));
                    vst1q_u8(&dst[((by * 4 + y + 1) * w + bx * 4) * 4], vld1q_u8(tmp + 16));
                }
            } else {
                /* edge block: scalar with bounds checks */
                for (int y = 0; y < 4; y++) {
                    for (int x = 0; x < 4; x++) {
                        u8 a = *src++;
                        u8 i = *src++;
                        int px = bx * 4 + x, py = by * 4 + y;
                        if (px < w && py < h) {
                            int idx = (py * w + px) * 4;
                            dst[idx] = dst[idx+1] = dst[idx+2] = i; dst[idx+3] = a;
                        }
                    }
                }
            }
        }
    }
    return;
    }
#endif
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    u8 a = *src++;
                    u8 i = *src++;
                    int px = bx * 4 + x, py = by * 4 + y;
                    if (px < w && py < h) {
                        int idx = (py * w + px) * 4;
                        dst[idx] = dst[idx+1] = dst[idx+2] = i; dst[idx+3] = a;
                    }
                }
            }
        }
    }
}

/* RGB565: 4x4 blocks, 16bpp */
static void decode_RGB565(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (!pc_tex_no_neon()) {
    {
        uint8x8x4_t t5;
        t5.val[0] = vld1_u8(&s_exp5_tbl[0]);
        t5.val[1] = vld1_u8(&s_exp5_tbl[8]);
        t5.val[2] = vld1_u8(&s_exp5_tbl[16]);
        t5.val[3] = vld1_u8(&s_exp5_tbl[24]);
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                if ((bx + 1) * 4 <= w && (by + 1) * 4 <= h) {
                    /* fully-interior block: 8 pixels (2 rows) per iteration */
                    for (int y = 0; y < 4; y += 2) {
                        uint16x8_t v = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(src)));
                        src += 16;
                        uint8x8x4_t pix;
                        pix.val[0] = vtbl4_u8(t5, vmovn_u16(vshrq_n_u16(v, 11)));
                        pix.val[1] = neon_expand6(vandq_u16(vshrq_n_u16(v, 5), vdupq_n_u16(0x3F)));
                        pix.val[2] = vtbl4_u8(t5, vmovn_u16(vandq_u16(v, vdupq_n_u16(0x1F))));
                        pix.val[3] = vdup_n_u8(255);
                        u8 tmp[32];
                        vst4_u8(tmp, pix);
                        vst1q_u8(&dst[((by * 4 + y) * w + bx * 4) * 4], vld1q_u8(tmp));
                        vst1q_u8(&dst[((by * 4 + y + 1) * w + bx * 4) * 4], vld1q_u8(tmp + 16));
                    }
                } else {
                    /* edge block: scalar with bounds checks */
                    for (int y = 0; y < 4; y++) {
                        for (int x = 0; x < 4; x++) {
                            u16 val = (src[0] << 8) | src[1]; src += 2;
                            int px = bx * 4 + x, py = by * 4 + y;
                            if (px < w && py < h) {
                                u8 r = ((val >> 11) & 0x1F) * 255 / 31;
                                u8 g = ((val >> 5) & 0x3F) * 255 / 63;
                                u8 b = (val & 0x1F) * 255 / 31;
                                int idx = (py * w + px) * 4;
                                dst[idx] = r; dst[idx+1] = g; dst[idx+2] = b; dst[idx+3] = 255;
                            }
                        }
                    }
                }
            }
        }
        return;
    }
    }
#endif
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    u16 val = (src[0] << 8) | src[1]; src += 2;
                    int px = bx * 4 + x, py = by * 4 + y;
                    if (px < w && py < h) {
                        u8 r = ((val >> 11) & 0x1F) * 255 / 31;
                        u8 g = ((val >> 5) & 0x3F) * 255 / 63;
                        u8 b = (val & 0x1F) * 255 / 31;
                        int idx = (py * w + px) * 4;
                        dst[idx] = r; dst[idx+1] = g; dst[idx+2] = b; dst[idx+3] = 255;
                    }
                }
            }
        }
    }
}

/* RGB5A3: 4x4 blocks, 16bpp. Bit 15=1: RGB555 opaque, bit 15=0: ARGB3444 */
static void decode_RGB5A3(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (!pc_tex_no_neon()) {
    {
        uint8x8x4_t t5;
        t5.val[0] = vld1_u8(&s_exp5_tbl[0]);
        t5.val[1] = vld1_u8(&s_exp5_tbl[8]);
        t5.val[2] = vld1_u8(&s_exp5_tbl[16]);
        t5.val[3] = vld1_u8(&s_exp5_tbl[24]);
        uint8x8_t t3 = vld1_u8(s_exp3_tbl);
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                if ((bx + 1) * 4 <= w && (by + 1) * 4 <= h) {
                    /* fully-interior block: 8 pixels (2 rows) per iteration */
                    for (int y = 0; y < 4; y += 2) {
                        uint16x8_t v = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(src)));
                        src += 16;
                        /* RGB555 opaque decode for all pixels */
                        uint8x8_t ro = vtbl4_u8(t5, vmovn_u16(vandq_u16(vshrq_n_u16(v, 10), vdupq_n_u16(0x1F))));
                        uint8x8_t go = vtbl4_u8(t5, vmovn_u16(vandq_u16(vshrq_n_u16(v, 5), vdupq_n_u16(0x1F))));
                        uint8x8_t bo = vtbl4_u8(t5, vmovn_u16(vandq_u16(v, vdupq_n_u16(0x1F))));
                        /* ARGB3444 decode for all pixels (4-bit: n*17 = (n<<4)|n) */
                        uint16x8_t r4 = vandq_u16(vshrq_n_u16(v, 8), vdupq_n_u16(0x0F));
                        uint16x8_t g4 = vandq_u16(vshrq_n_u16(v, 4), vdupq_n_u16(0x0F));
                        uint16x8_t b4 = vandq_u16(v, vdupq_n_u16(0x0F));
                        uint8x8_t rt = vmovn_u16(vorrq_u16(vshlq_n_u16(r4, 4), r4));
                        uint8x8_t gt = vmovn_u16(vorrq_u16(vshlq_n_u16(g4, 4), g4));
                        uint8x8_t bt = vmovn_u16(vorrq_u16(vshlq_n_u16(b4, 4), b4));
                        uint8x8_t at = vtbl1_u8(t3, vmovn_u16(vandq_u16(vshrq_n_u16(v, 12), vdupq_n_u16(0x07))));
                        /* bit 15 selects mode per pixel */
                        uint8x8_t m = vmovn_u16(vtstq_u16(v, vdupq_n_u16(0x8000)));
                        uint8x8x4_t pix;
                        pix.val[0] = vbsl_u8(m, ro, rt);
                        pix.val[1] = vbsl_u8(m, go, gt);
                        pix.val[2] = vbsl_u8(m, bo, bt);
                        pix.val[3] = vbsl_u8(m, vdup_n_u8(255), at);
                        u8 tmp[32];
                        vst4_u8(tmp, pix);
                        vst1q_u8(&dst[((by * 4 + y) * w + bx * 4) * 4], vld1q_u8(tmp));
                        vst1q_u8(&dst[((by * 4 + y + 1) * w + bx * 4) * 4], vld1q_u8(tmp + 16));
                    }
                } else {
                    /* edge block: scalar with bounds checks */
                    for (int y = 0; y < 4; y++) {
                        for (int x = 0; x < 4; x++) {
                            u16 val = (src[0] << 8) | src[1]; src += 2;
                            int px = bx * 4 + x, py = by * 4 + y;
                            if (px < w && py < h) {
                                int idx = (py * w + px) * 4;
                                decode_rgb5a3_entry(val, &dst[idx], &dst[idx+1], &dst[idx+2], &dst[idx+3]);
                            }
                        }
                    }
                }
            }
        }
        return;
    }
    }
#endif
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    u16 val = (src[0] << 8) | src[1]; src += 2;
                    int px = bx * 4 + x, py = by * 4 + y;
                    if (px < w && py < h) {
                        int idx = (py * w + px) * 4;
                        decode_rgb5a3_entry(val, &dst[idx], &dst[idx+1], &dst[idx+2], &dst[idx+3]);
                    }
                }
            }
        }
    }
}

/* RGBA8: 4x4 blocks, 32bpp. Two passes per block: AR then GB (64 bytes total) */
static void decode_RGBA8(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            u8 ar[16][2]; /* AR pass */
            for (int i = 0; i < 16; i++) {
                ar[i][0] = *src++;
                ar[i][1] = *src++;
            }
            for (int i = 0; i < 16; i++) { /* GB pass */
                int x = i % 4, y = i / 4;
                int px = bx * 4 + x, py = by * 4 + y;
                u8 g = *src++;
                u8 b = *src++;
                if (px < w && py < h) {
                    int idx = (py * w + px) * 4;
                    dst[idx] = ar[i][1];
                    dst[idx+1] = g;
                    dst[idx+2] = b;
                    dst[idx+3] = ar[i][0];
                }
            }
        }
    }
}

/* CMPR (S3TC/DXT1): 8x8 super-blocks of 2x2 sub-blocks, each sub is 4x4 DXT1 */
static void decode_CMPR(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 7) / 8, bh = (h + 7) / 8;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int sub = 0; sub < 4; sub++) {
                int sx = (sub & 1) * 4, sy = (sub >> 1) * 4;
                u16 c0 = (src[0] << 8) | src[1]; /* DXT1 block (BE) */
                u16 c1 = (src[2] << 8) | src[3];
                src += 4;
                u8 palette[4][4];
                palette[0][0] = ((c0 >> 11) & 0x1F) * 255 / 31;
                palette[0][1] = ((c0 >> 5) & 0x3F) * 255 / 63;
                palette[0][2] = (c0 & 0x1F) * 255 / 31;
                palette[0][3] = 255;
                palette[1][0] = ((c1 >> 11) & 0x1F) * 255 / 31;
                palette[1][1] = ((c1 >> 5) & 0x3F) * 255 / 63;
                palette[1][2] = (c1 & 0x1F) * 255 / 31;
                palette[1][3] = 255;
                /* interpolated colors */
                if (c0 > c1) {
                    for (int c = 0; c < 3; c++) {
                        palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3;
                        palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3;
                    }
                    palette[2][3] = palette[3][3] = 255;
                } else {
                    for (int c = 0; c < 3; c++)
                        palette[2][c] = (palette[0][c] + palette[1][c]) / 2;
                    palette[2][3] = 255;
                    palette[3][0] = palette[3][1] = palette[3][2] = 0;
                    palette[3][3] = 0;
                }
                /* u32 palette copy for single-store pixel writes (RGBA memory order) */
                u32 pal32[4];
                memcpy(pal32, palette, sizeof(pal32));
                for (int y = 0; y < 4; y++) {
                    u8 row = *src++;
                    for (int x = 0; x < 4; x++) {
                        int ci = (row >> (6 - x * 2)) & 3;
                        int px = bx * 8 + sx + x, py = by * 8 + sy + y;
                        if (px < w && py < h) {
                            int idx = (py * w + px) * 4;
                            *(u32*)&dst[idx] = pal32[ci];
                        }
                    }
                }
            }
        }
    }
}

/* Decode a single RGB5A3 value to RGBA8 */
static void decode_rgb5a3_entry(u16 val, u8* r, u8* g, u8* b, u8* a) {
    if (val & 0x8000) { /* RGB555 opaque */
        *r = ((val >> 10) & 0x1F) * 255 / 31;
        *g = ((val >> 5) & 0x1F) * 255 / 31;
        *b = (val & 0x1F) * 255 / 31;
        *a = 255;
    } else { /* ARGB3444 */
        *a = ((val >> 12) & 0x07) * 255 / 7;
        *r = ((val >> 8) & 0x0F) * 255 / 15;
        *g = ((val >> 4) & 0x0F) * 255 / 15;
        *b = (val & 0x0F) * 255 / 15;
    }
}

/* Decode a single RGB565 value to RGBA8 (always opaque) */
static void decode_rgb565_entry(u16 val, u8* r, u8* g, u8* b, u8* a) {
    *r = (u8)(((val >> 11) & 0x1F) * 255 / 31);
    *g = (u8)(((val >> 5) & 0x3F) * 255 / 63);
    *b = (u8)((val & 0x1F) * 255 / 31);
    *a = 255;
}

/* Build a 256-entry RGBA8 palette from TLUT data.
 * is_be=1 for ROM/JSystem data (BE), 0 for emu64 tlutconv output (native LE). */
static void build_palette(const void* tlut_data, int tlut_fmt, int n_entries,
                          u8 palette[256][4], int is_be) {
    /* default: grayscale ramp for missing palette */
    for (int i = 0; i < 256; i++) {
        palette[i][0] = palette[i][1] = palette[i][2] = (u8)i;
        palette[i][3] = 255;
    }
    if (!tlut_data || n_entries <= 0) return;
    if (n_entries > 256) n_entries = 256;

    const u16* pal16 = (const u16*)tlut_data;
    const u8* pal_bytes = (const u8*)tlut_data;
    for (int i = 0; i < n_entries; i++) {
        u16 val;
        if (is_be) {
            val = read_be16(pal_bytes + i * 2);
        } else {
            val = pal16[i];
        }
        if (tlut_fmt == GX_TL_RGB5A3) {
            decode_rgb5a3_entry(val, &palette[i][0], &palette[i][1],
                               &palette[i][2], &palette[i][3]);
        } else if (tlut_fmt == GX_TL_RGB565) {
            decode_rgb565_entry(val, &palette[i][0], &palette[i][1],
                                &palette[i][2], &palette[i][3]);
        } else { /* GX_TL_IA8: high byte = intensity, low byte = alpha */
            if (is_be) {
                palette[i][0] = palette[i][1] = palette[i][2] = (u8)(val >> 8);
                palette[i][3] = (u8)(val & 0xFF);
            } else {
                /* LE read of BE bytes [I,A] swaps them */
                palette[i][0] = palette[i][1] = palette[i][2] = (u8)(val & 0xFF);
                palette[i][3] = (u8)(val >> 8);
            }
        }
    }
}

/* CI4: 8x8 blocks, 4bpp indexed */
static void decode_CI4(const u8* src, u8* dst, int w, int h, const u8 palette[256][4]) {
    int bw = (w + 7) / 8, bh = (h + 7) / 8;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x += 2) {
                    u8 val = *src++;
                    int px0 = bx * 8 + x, py = by * 8 + y;
                    int px1 = px0 + 1;
                    int ci0 = (val >> 4) & 0xF;
                    int ci1 = val & 0xF;
                    if (px0 < w && py < h) {
                        int idx = (py * w + px0) * 4;
                        dst[idx] = palette[ci0][0]; dst[idx+1] = palette[ci0][1];
                        dst[idx+2] = palette[ci0][2]; dst[idx+3] = palette[ci0][3];
                    }
                    if (px1 < w && py < h) {
                        int idx = (py * w + px1) * 4;
                        dst[idx] = palette[ci1][0]; dst[idx+1] = palette[ci1][1];
                        dst[idx+2] = palette[ci1][2]; dst[idx+3] = palette[ci1][3];
                    }
                }
            }
        }
    }
}

/* CI8: 8x4 blocks, 8bpp indexed */
static void decode_CI8(const u8* src, u8* dst, int w, int h, const u8 palette[256][4]) {
    int bw = (w + 7) / 8, bh = (h + 3) / 4;
    /* u32 palette copy: one store per pixel instead of four byte writes
     * (RGBA memory order is preserved by the byte copy) */
    u32 pal32[256];
    memcpy(pal32, palette, sizeof(pal32));
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            if ((bx + 1) * 8 <= w && (by + 1) * 4 <= h) {
                /* fully-interior block: unrolled row, no bounds checks */
                for (int y = 0; y < 4; y++) {
                    u32* d = (u32*)&dst[((by * 4 + y) * w + bx * 8) * 4];
                    d[0] = pal32[src[0]]; d[1] = pal32[src[1]];
                    d[2] = pal32[src[2]]; d[3] = pal32[src[3]];
                    d[4] = pal32[src[4]]; d[5] = pal32[src[5]];
                    d[6] = pal32[src[6]]; d[7] = pal32[src[7]];
                    src += 8;
                }
            } else {
                /* edge block: scalar with bounds checks */
                for (int y = 0; y < 4; y++) {
                    for (int x = 0; x < 8; x++) {
                        u8 val = *src++;
                        int px = bx * 8 + x, py = by * 4 + y;
                        if (px < w && py < h) {
                            int idx = (py * w + px) * 4;
                            *(u32*)&dst[idx] = pal32[val];
                        }
                    }
                }
            }
        }
    }
}

static void decode_gc_texture(const void* src, u8* dst_rgba, int w, int h, u32 fmt,
                              const u8 palette[256][4]) {
    /* transparent fallback for unhandled formats */
    memset(dst_rgba, 0, w * h * 4);

    switch (fmt) {
        case GX_TF_I4:     decode_I4((const u8*)src, dst_rgba, w, h); break;
        case GX_TF_I8:     decode_I8((const u8*)src, dst_rgba, w, h); break;
        case GX_TF_IA4:    decode_IA4((const u8*)src, dst_rgba, w, h); break;
        case GX_TF_IA8:    decode_IA8((const u8*)src, dst_rgba, w, h); break;
        case GX_TF_RGB565: decode_RGB565((const u8*)src, dst_rgba, w, h); break;
        case GX_TF_RGB5A3: decode_RGB5A3((const u8*)src, dst_rgba, w, h); break;
        case GX_TF_RGBA8:  decode_RGBA8((const u8*)src, dst_rgba, w, h); break;
        case GX_TF_C4:     decode_CI4((const u8*)src, dst_rgba, w, h, palette); break;
        case GX_TF_C8:     decode_CI8((const u8*)src, dst_rgba, w, h, palette); break;
        case GX_TF_CMPR:   decode_CMPR((const u8*)src, dst_rgba, w, h); break;
        default:
            break;
    }
}

/* Per-frame CPU decode budget: entering a new area misses dozens of textures
 * in one frame; cap decodes per frame and let the rest pop in next frame. */
extern u32 pc_frame_counter; /* pc_vi.c */
static u32 s_decode_frame = 0;
static int s_decode_count = 0;
#define DECODE_BUDGET_PER_FRAME 8

void GXLoadTexObj(void* obj, u32 id) {
    pc_gx_flush_if_begin_complete();

    if (id >= 8 && id != 0xFF && id < 0x100) return;
    if (id >= 8) return;

    u32* o = (u32*)obj;
    void* image_ptr = (void*)(uintptr_t)o[TEXOBJ_IMAGE_PTR];
    int width = (int)o[TEXOBJ_WIDTH];
    int height = (int)o[TEXOBJ_HEIGHT];
    u32 format = o[TEXOBJ_FORMAT];
    u32 wrap_s = o[TEXOBJ_WRAP_S], wrap_t = o[TEXOBJ_WRAP_T];
    u32 tlut_key = (format == GX_TF_C4 || format == GX_TF_C8) ? o[TEXOBJ_TLUT_NAME] : 0xFFFFFFFF;
    u32 tlut_ptr_key = 0;
    u32 tlut_hash_key = 0;
    u32 filter_mode = o[TEXOBJ_MIN_FILTER];

    if (format == GX_TF_C4 || format == GX_TF_C8) {
        int tlut_name = (int)o[TEXOBJ_TLUT_NAME];
        if (tlut_name >= 0 && tlut_name < 16 && g_gx.tlut[tlut_name].data) {
            tlut_ptr_key = (u32)(uintptr_t)g_gx.tlut[tlut_name].data;
            tlut_hash_key = tlut_content_hash(g_gx.tlut[tlut_name].data,
                                              g_gx.tlut[tlut_name].format,
                                              g_gx.tlut[tlut_name].n_entries,
                                              g_gx.tlut[tlut_name].is_be);
        }
    }

#ifdef PC_ENHANCEMENTS
    /* EFB capture bypass: use full-res FBO texture instead of re-decoding */
    {
        GLuint efb_tex = pc_gx_efb_capture_find(o[TEXOBJ_IMAGE_PTR]);
        if (efb_tex) {
            glBindTexture(GL_TEXTURE_2D, efb_tex);
            GLenum gl_filter = filter_mode ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);
            o[TEXOBJ_GL_TEX] = efb_tex;
            g_gx.gl_textures[id] = efb_tex;
            g_gx.tex_obj_w[id] = width;
            g_gx.tex_obj_h[id] = height;
            g_gx.tex_obj_fmt[id] = (int)format;
            DIRTY(PC_GX_DIRTY_TEXTURES);
            return;
        }
    }
#endif

    /* detect when emu64 reuses the same buffer with different data */
    u32 hash = tex_content_hash(image_ptr, width, height, format);

    /* cache lookup */
    TexCacheEntry* cached = tex_cache_find(o[TEXOBJ_IMAGE_PTR], width, height, format, tlut_key,
                                           tlut_ptr_key, tlut_hash_key, hash);
    if (cached) {
        tex_cache_hits++;
        GLuint tex = cached->gl_tex;
        glBindTexture(GL_TEXTURE_2D, tex);

        /* update wrap/filter if changed */
        if (cached->wrap_s != wrap_s || cached->wrap_t != wrap_t) {
            GLenum gl_ws = (wrap_s == 2) ? GL_MIRRORED_REPEAT :
                           (wrap_s == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            GLenum gl_wt = (wrap_t == 2) ? GL_MIRRORED_REPEAT :
                           (wrap_t == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_ws);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wt);
            cached->wrap_s = wrap_s;
            cached->wrap_t = wrap_t;
        }
        if (cached->min_filter != filter_mode) {
            GLenum gl_filter = filter_mode ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);
            cached->min_filter = filter_mode;
        }

        o[TEXOBJ_GL_TEX] = tex;
        g_gx.gl_textures[id] = tex;
        g_gx.tex_obj_w[id] = width;
        g_gx.tex_obj_h[id] = height;
        g_gx.tex_obj_fmt[id] = (int)format;
        DIRTY(PC_GX_DIRTY_TEXTURES);
        return;
    }

    /* cache miss */
    tex_cache_misses++;

    /* try texture pack replacement before decoding */
    if (pc_texture_pack_active()) {
        const void* tp_tlut = NULL;
        int tp_tlut_entries = 0;
        int tp_tlut_is_be = 1;
        if ((format == GX_TF_C4 || format == GX_TF_C8)) {
            int tlut_name = (int)o[TEXOBJ_TLUT_NAME];
            if (tlut_name >= 0 && tlut_name < 16 && g_gx.tlut[tlut_name].data) {
                tp_tlut = g_gx.tlut[tlut_name].data;
                tp_tlut_entries = g_gx.tlut[tlut_name].n_entries;
                tp_tlut_is_be = g_gx.tlut[tlut_name].is_be;
            }
        }
        int data_size = (width * height * gc_format_bpp(format)) / 8;
        int hd_w = 0, hd_h = 0;
        GLuint hd_tex = pc_texture_pack_lookup(image_ptr, data_size,
                                               width, height, format,
                                               tp_tlut, tp_tlut_entries, tp_tlut_is_be,
                                               &hd_w, &hd_h);
        if (hd_tex) {
            glBindTexture(GL_TEXTURE_2D, hd_tex);
            GLenum gl_ws = (wrap_s == 2) ? GL_MIRRORED_REPEAT :
                           (wrap_s == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            GLenum gl_wt = (wrap_t == 2) ? GL_MIRRORED_REPEAT :
                           (wrap_t == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_ws);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wt);

            TexCacheEntry* entry = tex_cache_insert(o[TEXOBJ_IMAGE_PTR], width, height, format,
                                                    tlut_key, tlut_ptr_key, tlut_hash_key, hash, hd_tex);
            entry->wrap_s = wrap_s;
            entry->wrap_t = wrap_t;
            entry->min_filter = filter_mode;
            entry->external = 1;

            o[TEXOBJ_GL_TEX] = hd_tex;
            g_gx.gl_textures[id] = hd_tex;
            g_gx.tex_obj_w[id] = width;
            g_gx.tex_obj_h[id] = height;
            g_gx.tex_obj_fmt[id] = (int)format;
            DIRTY(PC_GX_DIRTY_TEXTURES);
            return;
        }
    }

    /* per-frame decode budget (tiny textures exempt: cheap and often UI).
     * PC_NO_DECODE_BUDGET=1 disables (triage). */
    static int s_no_budget = -1;
    if (s_no_budget < 0) s_no_budget = (getenv("PC_NO_DECODE_BUDGET") != NULL);
    if (!s_no_budget && image_ptr && width > 0 && height > 0 &&
        width <= 1024 && height <= 1024 && width * height > 32 * 32) {
        if (pc_frame_counter != s_decode_frame) {
            s_decode_frame = pc_frame_counter;
            s_decode_count = 0;
        }
        if (s_decode_count >= DECODE_BUDGET_PER_FRAME) {
            /* over budget: bind shared 1x1 white placeholder, skip cache
             * insert so this texture misses again and decodes next frame */
            static GLuint s_placeholder_tex = 0;
            if (!s_placeholder_tex) {
                u8 white[4] = {255, 255, 255, 255};
                glGenTextures(1, &s_placeholder_tex);
                glBindTexture(GL_TEXTURE_2D, s_placeholder_tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, white);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            } else {
                glBindTexture(GL_TEXTURE_2D, s_placeholder_tex);
            }
            o[TEXOBJ_GL_TEX] = s_placeholder_tex;
            g_gx.gl_textures[id] = s_placeholder_tex;
            g_gx.tex_obj_w[id] = width;
            g_gx.tex_obj_h[id] = height;
            g_gx.tex_obj_fmt[id] = (int)format;
            DIRTY(PC_GX_DIRTY_TEXTURES);
            return;
        }
        s_decode_count++;
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    if (image_ptr && width > 0 && height > 0 && width <= 1024 && height <= 1024) {
        u8* rgba = (u8*)malloc(width * height * 4);
        if (rgba) {
            u8 palette[256][4];
            if (format == GX_TF_C4 || format == GX_TF_C8) {
                int tlut_name = (int)o[TEXOBJ_TLUT_NAME];
                if (tlut_name >= 0 && tlut_name < 16 && g_gx.tlut[tlut_name].data) {
                    build_palette(g_gx.tlut[tlut_name].data,
                                  g_gx.tlut[tlut_name].format,
                                  g_gx.tlut[tlut_name].n_entries,
                                  palette,
                                  g_gx.tlut[tlut_name].is_be);
                } else {
                    build_palette(NULL, 0, 0, palette, 0);
                }
            } else {
                build_palette(NULL, 0, 0, palette, 0);
            }

            decode_gc_texture(image_ptr, rgba, width, height, format, palette);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            PC_GL_CHECK("glTexImage2D");
            free(rgba);
        } else {
            u8 white[4] = {255, 255, 255, 255};
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        }
    } else {
        u8 white[4] = {255, 255, 255, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    }

    {
        GLenum gl_filter = filter_mode ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);
    }

    {
        GLenum gl_ws = (wrap_s == 2) ? GL_MIRRORED_REPEAT :
                       (wrap_s == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        GLenum gl_wt = (wrap_t == 2) ? GL_MIRRORED_REPEAT :
                       (wrap_t == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_ws);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wt);
    }

    /* insert into cache */
    TexCacheEntry* entry = tex_cache_insert(o[TEXOBJ_IMAGE_PTR], width, height, format, tlut_key,
                                            tlut_ptr_key, tlut_hash_key, hash, tex);
    entry->wrap_s = wrap_s;
    entry->wrap_t = wrap_t;
    entry->min_filter = filter_mode;

    o[TEXOBJ_GL_TEX] = tex;
    g_gx.gl_textures[id] = tex;
    g_gx.tex_obj_w[id] = width;
    g_gx.tex_obj_h[id] = height;
    g_gx.tex_obj_fmt[id] = (int)format;
    DIRTY(PC_GX_DIRTY_TEXTURES);
}

u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, GXBool mipmap, u8 max_lod) {
    u32 bpp = (u32)gc_format_bpp(format);
    u32 total = (width * height * bpp) / 8;
    if (mipmap) {
        u32 w = width, h = height;
        for (u8 lod = 1; lod <= max_lod && (w > 1 || h > 1); lod++) {
            if (w > 1) w >>= 1;
            if (h > 1) h >>= 1;
            total += (w * h * bpp) / 8;
        }
    }
    return total;
}

void GXInvalidateTexAll(void) {
    /* no-op: PC cache keys on pointer+format, no TMEM to flush */
}
void GXInvalidateTexRegion(void* region) { (void)region; }

/* --- TLUT --- */

void GXInitTlutObj(void* obj, void* lut, u32 fmt, u16 n_entries) {
    u32* o = (u32*)obj;
    memset(o, 0, 4 * sizeof(u32));
    o[TLUTOBJ_DATA] = (u32)(uintptr_t)lut;
    o[TLUTOBJ_FORMAT] = fmt;
    o[TLUTOBJ_N_ENTRIES] = n_entries;
}

void GXLoadTlut(void* obj, u32 idx) {
    pc_gx_flush_if_begin_complete();
    if (idx >= 16) return;
    u32* o = (u32*)obj;
    g_gx.tlut[idx].data = (const void*)(uintptr_t)o[TLUTOBJ_DATA];
    g_gx.tlut[idx].format = (int)o[TLUTOBJ_FORMAT];
    g_gx.tlut[idx].n_entries = (int)o[TLUTOBJ_N_ENTRIES];
    g_gx.tlut[idx].is_be = 1; /* default to BE (ROM/JSystem data) */
}

/* PC_GX_TLUT_MODE env var: "be" forces BE decode for diagnostics */
static int pc_gx_tlut_force_be(void) {
    static int init = 0;
    static int force_be = 0;
    if (!init) {
        const char* mode = getenv("PC_GX_TLUT_MODE");
        if (mode != NULL) {
            if (mode[0] == 'b' || mode[0] == 'B') {
                force_be = 1;
            }
        }
        init = 1;
    }
    return force_be;
}

/* Mark a TLUT slot as native-LE (from emu64 tlutconv) */
void pc_gx_tlut_set_native_le(unsigned int idx) {
    if (idx < 16) {
        if (pc_gx_tlut_force_be()) {
            g_gx.tlut[idx].is_be = 1;
        } else {
            g_gx.tlut[idx].is_be = 0;
        }
    }
}

void GXInitTexCacheRegion(void* region, GXBool is_32b, u32 tmem_even, u32 size_even,
                          u32 tmem_odd, u32 size_odd) {
    (void)region; (void)is_32b; (void)tmem_even; (void)size_even; (void)tmem_odd; (void)size_odd;
}

void* GXSetTexRegionCallback(void* callback) { return NULL; }
void GXInitTlutRegion(void* region, u32 tmem_addr, u32 tlut_size) {
    (void)region; (void)tmem_addr; (void)tlut_size;
}

/* --- accessors --- */
GXBool GXGetTexObjMipMap(const void* obj) { return ((const u32*)obj)[TEXOBJ_MIPMAP] != 0; }
u32    GXGetTexObjFmt(const void* obj)    { return ((const u32*)obj)[TEXOBJ_FORMAT]; }
u16    GXGetTexObjHeight(const void* obj) { return (u16)((const u32*)obj)[TEXOBJ_HEIGHT]; }
u16    GXGetTexObjWidth(const void* obj)  { return (u16)((const u32*)obj)[TEXOBJ_WIDTH]; }
u32    GXGetTexObjWrapS(const void* obj)  { return ((const u32*)obj)[TEXOBJ_WRAP_S]; }
u32    GXGetTexObjWrapT(const void* obj)  { return ((const u32*)obj)[TEXOBJ_WRAP_T]; }
void*  GXGetTexObjData(const void* obj)   { return (void*)(uintptr_t)((const u32*)obj)[TEXOBJ_IMAGE_PTR]; }

void GXDestroyTexObj(void* obj) {
    u32* o = (u32*)obj;
    /* don't delete GL texture here; cache eviction handles that */
    o[TEXOBJ_GL_TEX] = 0;
}

void GXDestroyTlutObj(void* obj) { (void)obj; }
