/* pc_gx_tev.c - TEV: runtime-specialized fragment shaders (zero GPU branching)
 *
 * Instead of one uber-shader with 16-way switches per TEV input, we generate
 * a small branchless GLSL fragment shader for each unique TEV configuration.
 * Animal Crossing uses ~30-50 unique configs; each gets a ~30-line shader.
 * Massive win on Mali-G52 which hates dynamic branching.
 */
#include "pc_gx_internal.h"
#include <stdarg.h>

/* --- file I/O --- */

static char* load_text_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (rd != (size_t)len) { free(buf); return NULL; }
    buf[rd] = '\0';
    return buf;
}

static char* load_shader(const char* filename) {
    char path[512];
    snprintf(path, sizeof(path), "shaders/%s", filename);
    char* src = load_text_file(path);
    if (src) printf("[PC/TEV] Loaded shader: %s\n", path);
    else fprintf(stderr, "FATAL: Could not load shader: %s\n", path);
    return src;
}

/* --- Shader compilation --- */

static GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
#ifdef PC_USE_GLES
    const char* version_line = "#version 310 es\n";
    const char* precision_line = (type == GL_FRAGMENT_SHADER)
        ? "precision mediump float;\nprecision highp int;\n"
        : "precision highp float;\nprecision highp int;\n";
    const char* past_version = source;
    if (strncmp(source, "#version", 8) == 0) {
        past_version = strchr(source, '\n');
        if (past_version) past_version++;
        else past_version = source;
    }
    const char* sources[3] = { version_line, precision_line, past_version };
    glShaderSource(shader, 3, sources, NULL);
#else
    glShaderSource(shader, 1, &source, NULL);
#endif
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "Shader compile error: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* Disk cache for linked program binaries (GLES only: Mali/Adreno drivers
 * compile GLSL slowly and mid-frame, causing visible hitches on first visit
 * to each area. glProgramBinary reload at boot is ~1ms per program.) */
#ifdef PC_USE_GLES
#define SHADER_DISK_CACHE 1
#endif

/* --- Vertex shader (compiled once, kept alive) --- */
static GLuint s_vs = 0;

static GLuint link_with_vs(GLuint frag) {
    if (!s_vs || !frag) { if (frag) glDeleteShader(frag); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, s_vs);
    glAttachShader(prog, frag);
    glBindAttribLocation(prog, 0, "a_position");
    glBindAttribLocation(prog, 1, "a_normal");
    glBindAttribLocation(prog, 2, "a_color0");
    glBindAttribLocation(prog, 3, "a_texcoord0");
#ifdef SHADER_DISK_CACHE
    if (glProgramParameteri)
        glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
#endif
    glLinkProgram(prog);
    GLint success;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "Program link error: %s\n", log);
        glDeleteProgram(prog);
        prog = 0;
    }
    glDeleteShader(frag); /* frag stays alive inside prog */
    return prog;
}

/* ===================================================================
 * Shader key: compact representation of TEV state that affects
 * fragment shader code. Two draws with the same key → same shader.
 * =================================================================== */
typedef struct {
    u8 num_stages;
    u8 num_chans;
    u8 light_enable;      /* chan_ctrl_enable[0] */
    u8 light_mat_src;     /* chan_ctrl_mat_src[0] */
    u8 light_amb_src;     /* chan_ctrl_amb_src[0] */
    u8 alpha_light_en;    /* chan_ctrl_enable[1] */
    u8 alpha_mat_src;     /* chan_ctrl_mat_src[1] */
    u8 fog_enabled;
    u8 alpha_comp0, alpha_aop, alpha_comp1;
    u8 _pad;
    struct {
        u8 cin[4];  /* color inputs a,b,c,d (0-15) */
        u8 ain[4];  /* alpha inputs a,b,c,d (0-7) */
        u8 color_op, alpha_op;
        u8 color_bias, color_scale, alpha_bias, alpha_scale;
        u8 color_clamp, alpha_clamp;
        u8 color_out, alpha_out;
        u8 k_color_sel, k_alpha_sel;
    } s[3]; /* 20 bytes × 3 = 60 bytes */
} ShaderKey; /* 72 bytes */

static void build_key(PCGXState* st, ShaderKey* k) {
    memset(k, 0, sizeof(*k));
    int ns = st->num_tev_stages;
    if (ns > PC_GX_MAX_TEV_STAGES) ns = PC_GX_MAX_TEV_STAGES;
    k->num_stages = (u8)ns;
    k->num_chans = (u8)st->num_chans;
    k->light_enable = (u8)st->chan_ctrl_enable[0];
    k->light_mat_src = (u8)st->chan_ctrl_mat_src[0];
    k->light_amb_src = (u8)st->chan_ctrl_amb_src[0];
    k->alpha_light_en = (u8)st->chan_ctrl_enable[1];
    k->alpha_mat_src = (u8)st->chan_ctrl_mat_src[1];
    k->fog_enabled = (st->fog_type != 0) ? 1 : 0;
    k->alpha_comp0 = (u8)st->alpha_comp0;
    k->alpha_aop = (u8)st->alpha_op;
    k->alpha_comp1 = (u8)st->alpha_comp1;
    for (int i = 0; i < ns; i++) {
        PCGXTevStage* ts = &st->tev_stages[i];
        k->s[i].cin[0] = (u8)ts->color_a; k->s[i].cin[1] = (u8)ts->color_b;
        k->s[i].cin[2] = (u8)ts->color_c; k->s[i].cin[3] = (u8)ts->color_d;
        k->s[i].ain[0] = (u8)ts->alpha_a; k->s[i].ain[1] = (u8)ts->alpha_b;
        k->s[i].ain[2] = (u8)ts->alpha_c; k->s[i].ain[3] = (u8)ts->alpha_d;
        k->s[i].color_op = (u8)ts->color_op;
        k->s[i].alpha_op = (u8)ts->alpha_op;
        k->s[i].color_bias = (u8)ts->color_bias;
        k->s[i].color_scale = (u8)ts->color_scale;
        k->s[i].alpha_bias = (u8)ts->alpha_bias;
        k->s[i].alpha_scale = (u8)ts->alpha_scale;
        k->s[i].color_clamp = (u8)ts->color_clamp;
        k->s[i].alpha_clamp = (u8)ts->alpha_clamp;
        k->s[i].color_out = (u8)ts->color_out;
        k->s[i].alpha_out = (u8)ts->alpha_out;
        k->s[i].k_color_sel = (u8)ts->k_color_sel;
        k->s[i].k_alpha_sel = (u8)ts->k_alpha_sel;
    }
}

/* ===================================================================
 * Shader cache: hash table with open addressing.
 * =================================================================== */
#define SCACHE_SIZE 256
#define SCACHE_MASK (SCACHE_SIZE - 1)

typedef struct {
    ShaderKey key;
    GLuint program;
    PCGXUniformLocs locs;  /* resolved once at link time */
    int valid;
} SCacheEntry;

static SCacheEntry s_cache[SCACHE_SIZE];

/* Locations of the program returned by the last pc_gx_tev_get_shader call */
const PCGXUniformLocs* pc_gx_tev_last_locs = NULL;

static u32 hash_key(const ShaderKey* k) {
    const u8* p = (const u8*)k;
    u32 h = 0x811c9dc5u;
    for (int i = 0; i < (int)sizeof(ShaderKey); i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

static SCacheEntry* cache_lookup(const ShaderKey* k) {
    u32 slot = hash_key(k) & SCACHE_MASK;
    for (int i = 0; i < SCACHE_SIZE; i++) {
        u32 s = (slot + i) & SCACHE_MASK;
        if (!s_cache[s].valid) return NULL;
        if (memcmp(&s_cache[s].key, k, sizeof(ShaderKey)) == 0)
            return &s_cache[s];
    }
    return NULL;
}

static SCacheEntry* cache_insert(const ShaderKey* k, GLuint prog) {
    SCacheEntry* e = NULL;
    u32 slot = hash_key(k) & SCACHE_MASK;
    for (int i = 0; i < SCACHE_SIZE; i++) {
        u32 s = (slot + i) & SCACHE_MASK;
        if (!s_cache[s].valid) {
            e = &s_cache[s];
            break;
        }
    }
    if (!e) {
        /* cache full — evict this slot */
        e = &s_cache[slot];
        if (e->program) glDeleteProgram(e->program);
    }
    e->key = *k;
    e->program = prog;
    e->valid = 1;
    pc_gx_fill_uniform_locations(prog, &e->locs);
    return e;
}

/* ===================================================================
 * Fragment shader code generation
 * =================================================================== */
#define FRAG_BUF 16384

/* string builder */
typedef struct { char* buf; int pos; int max; } SB;
static void sb_printf(SB* sb, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->pos, sb->max - sb->pos, fmt, ap);
    va_end(ap);
    if (n > 0) sb->pos += n;
}
#define P(...) sb_printf(&sb, __VA_ARGS__)

/* TEV color input expression (hardcoded per input ID) */
static const char* tevC(int id) {
    switch (id) {
        case 0:  return "prev.rgb";
        case 1:  return "vec3(prev.a)";
        case 2:  return "r0.rgb";
        case 3:  return "vec3(r0.a)";
        case 4:  return "r1.rgb";
        case 5:  return "vec3(r1.a)";
        case 6:  return "r2.rgb";
        case 7:  return "vec3(r2.a)";
        case 8:  return "sTex.rgb";
        case 9:  return "vec3(sTex.a)";
        case 10: return "sRas.rgb";
        case 11: return "vec3(sRas.a)";
        case 12: return "vec3(1.0)";
        case 13: return "vec3(0.5)";
        case 14: return "kc";
        default: return "vec3(0.0)";
    }
}

/* TEV alpha input expression */
static const char* tevA(int id) {
    switch (id) {
        case 0: return "prev.a";
        case 1: return "r0.a";
        case 2: return "r1.a";
        case 3: return "r2.a";
        case 4: return "sTex.a";
        case 5: return "sRas.a";
        case 6: return "ka";
        default: return "0.0";
    }
}

/* NOTE: Helper functions below take SB* (pointer), so they must call
 * sb_printf(sb, ...) directly — NOT the P() macro, which does &sb
 * and would create SB** when sb is already SB*. */

/* Emit getKonstC for a specific selector value */
static void emit_kc(SB* sb, int sel) {
    if (sel <= 7) {
        sb_printf(sb, "    vec3 kc = vec3(%.6f);\n", (8.0f - sel) / 8.0f);
    } else if (sel <= 11) {
        sb_printf(sb, "    vec3 kc = vec3(0.0);\n");
    } else if (sel <= 15) {
        sb_printf(sb, "    vec3 kc = u_kcolor[%d].rgb;\n", sel - 12);
    } else {
        int ki = (sel - 16) & 3;
        int ch = (sel - 16) >> 2;
        const char* c[] = {"r","g","b","a"};
        sb_printf(sb, "    vec3 kc = vec3(u_kcolor[%d].%s);\n", ki, c[ch & 3]);
    }
}

/* Emit getKonstA for a specific selector value */
static void emit_ka(SB* sb, int sel) {
    if (sel <= 7) {
        sb_printf(sb, "    float ka = %.6f;\n", (8.0f - sel) / 8.0f);
    } else if (sel <= 15) {
        sb_printf(sb, "    float ka = 0.0;\n");
    } else {
        int ki = (sel - 16) & 3;
        int ch = (sel - 16) >> 2;
        const char* c[] = {"r","g","b","a"};
        sb_printf(sb, "    float ka = u_kcolor[%d].%s;\n", ki, c[ch & 3]);
    }
}

/* Emit one alpha comparison */
static void emit_acomp(SB* sb, const char* var, int comp, const char* ref) {
    switch (comp) {
        case 0: sb_printf(sb, "    bool %s = false;\n", var); break;
        case 1: sb_printf(sb, "    bool %s = prev.a < %s;\n", var, ref); break;
        case 2: sb_printf(sb, "    bool %s = abs(prev.a - %s) < (0.5/255.0);\n", var, ref); break;
        case 3: sb_printf(sb, "    bool %s = prev.a <= %s;\n", var, ref); break;
        case 4: sb_printf(sb, "    bool %s = prev.a > %s;\n", var, ref); break;
        case 5: sb_printf(sb, "    bool %s = abs(prev.a - %s) >= (0.5/255.0);\n", var, ref); break;
        case 6: sb_printf(sb, "    bool %s = prev.a >= %s;\n", var, ref); break;
        default: sb_printf(sb, "    bool %s = true;\n", var); break;
    }
}

/* Write register: emit direct assignment to the known output register */
static void emit_write_rgb(SB* sb, int out_reg, int do_clamp) {
    const char* reg[] = {"prev","r0","r1","r2"};
    if (do_clamp)
        sb_printf(sb, "    %s.rgb = clamp(cResult, 0.0, 1.0);\n", reg[out_reg & 3]);
    else
        sb_printf(sb, "    %s.rgb = cResult;\n", reg[out_reg & 3]);
}

static void emit_write_a(SB* sb, int out_reg, int do_clamp) {
    const char* reg[] = {"prev","r0","r1","r2"};
    if (do_clamp)
        sb_printf(sb, "    %s.a = clamp(aResult, 0.0, 1.0);\n", reg[out_reg & 3]);
    else
        sb_printf(sb, "    %s.a = aResult;\n", reg[out_reg & 3]);
}

static char* generate_frag(PCGXState* st) {
    char* buf = (char*)malloc(FRAG_BUF);
    if (!buf) return NULL;
    SB sb = { buf, 0, FRAG_BUF };
    int ns = st->num_tev_stages;
    if (ns > PC_GX_MAX_TEV_STAGES) ns = PC_GX_MAX_TEV_STAGES;

    /* --- Header & inputs --- */
    P("#version 330 core\n");
    P("in vec4 v_color;\n");
    P("in vec2 v_texcoord0;\n");
    P("in vec2 v_texcoord1;\n");
    P("in vec3 v_normal;\n");
    P("in float v_fog_z;\n");
    P("in vec3 v_light_accum;\n\n");

    /* --- Uniforms: only declare what this specific shader reads.
     * pc_gx.c's uniform upload uses if(loc >= 0), so undeclared
     * uniforms get loc=-1 and uploads are skipped automatically. --- */

    /* Fog uniforms only if fog is active */
    if (st->fog_type != 0) {
        P("uniform float u_fog_start;\n");
        P("uniform float u_fog_end;\n");
        P("uniform vec4 u_fog_color;\n");
    }

    /* TEV register init */
    P("uniform vec4 u_tev_prev;\n");
    P("uniform vec4 u_tev_reg0;\n");
    P("uniform vec4 u_tev_reg1;\n");
    P("uniform vec4 u_tev_reg2;\n");

    /* Konst colors — check if any stage uses konst */
    {
        int needs_kcolor = 0;
        for (int i = 0; i < ns; i++) {
            PCGXTevStage* ts = &st->tev_stages[i];
            if (ts->k_color_sel >= 12 || ts->k_alpha_sel >= 16) { needs_kcolor = 1; break; }
        }
        if (needs_kcolor) P("uniform vec4 u_kcolor[4];\n");
    }

    /* Textures — only for stages that exist */
    for (int s = 0; s < ns; s++) {
        P("uniform sampler2D u_texture%d;\n", s);
        P("uniform int u_use_texture%d;\n", s);
        P("uniform int u_tev%d_tc_src;\n", s);
    }

    /* Material/ambient colors — only if lighting path reads them */
    {
        int need_mat = 0, need_amb = 0;
        if (st->num_chans > 0) {
            if (st->chan_ctrl_mat_src[0] == 0) need_mat = 1;  /* RGB mat from register */
            if (st->chan_ctrl_mat_src[1] == 0) need_mat = 1;  /* Alpha mat from register */
            if (st->chan_ctrl_enable[0] && st->chan_ctrl_amb_src[0] == 0) need_amb = 1;
            if (st->chan_ctrl_enable[1]) need_amb = 1;  /* alpha lighting uses u_amb_color.a */
        }
        if (need_mat) P("uniform vec4 u_mat_color;\n");
        if (need_amb) P("uniform vec4 u_amb_color;\n");
    }

    /* Alpha compare refs — only if alpha test is non-trivial */
    if (st->alpha_comp0 != 7 || st->alpha_comp1 != 7) {
        P("uniform int u_alpha_ref0;\n");
        P("uniform int u_alpha_ref1;\n");
    }

    /* Swap tables */
    P("uniform ivec4 u_swap_table[4];\n");
    for (int s = 0; s < ns; s++)
        P("uniform ivec2 u_tev%d_swap;\n", s);

    P("\nout vec4 fragColor;\n\n");

    /* --- Swap helper (still needed for dynamic swap tables) --- */
    P("vec4 applySwap(vec4 v, ivec4 sw) {\n");
    P("    return vec4(v[sw.x], v[sw.y], v[sw.z], v[sw.w]);\n");
    P("}\n\n");

    /* --- main() --- */
    P("void main() {\n");
    P("    vec2 tc0 = v_texcoord0;\n");
    P("    vec2 tc1 = v_texcoord1;\n\n");

    /* Texcoord selection per stage */
    for (int s = 0; s < ns; s++) {
        P("    vec2 stc%d = (u_tev%d_tc_src == 0) ? tc0 : tc1;\n", s, s);
    }
    P("\n");

    /* Texture sampling — only for stages that exist */
    const char* tex_names[] = { "u_texture0", "u_texture1", "u_texture2" };
    const char* use_names[] = { "u_use_texture0", "u_use_texture1", "u_use_texture2" };
    for (int s = 0; s < ns; s++) {
        P("    vec4 texColor%d = vec4(1.0);\n", s);
        P("    if (%s != 0) texColor%d = texture(%s, stc%d);\n",
          use_names[s], s, tex_names[s], s);
    }
    P("\n");

    /* --- Rasterized color (hardcoded lighting path) --- */
    if (st->num_chans == 0) {
        P("    vec4 rasColor = vec4(1.0);\n");
    } else {
        P("    vec4 rasColor;\n");
        /* RGB */
        if (st->chan_ctrl_mat_src[0] != 0)
            P("    vec3 matC = v_color.rgb;\n");
        else
            P("    vec3 matC = u_mat_color.rgb;\n");

        if (st->chan_ctrl_enable[0] != 0) {
            if (st->chan_ctrl_amb_src[0] != 0)
                P("    rasColor.rgb = matC * clamp(v_color.rgb + v_light_accum, 0.0, 1.0);\n");
            else
                P("    rasColor.rgb = matC * clamp(u_amb_color.rgb + v_light_accum, 0.0, 1.0);\n");
        } else {
            P("    rasColor.rgb = matC;\n");
        }
        /* Alpha */
        if (st->chan_ctrl_mat_src[1] != 0)
            P("    float matA = v_color.a;\n");
        else
            P("    float matA = u_mat_color.a;\n");

        if (st->chan_ctrl_enable[1] != 0)
            P("    rasColor.a = matA * u_amb_color.a;\n");
        else
            P("    rasColor.a = matA;\n");
    }
    P("\n");

    /* --- Register initialization --- */
    P("    vec4 prev = u_tev_prev;\n");
    P("    vec4 r0 = u_tev_reg0;\n");
    P("    vec4 r1 = u_tev_reg1;\n");
    P("    vec4 r2 = u_tev_reg2;\n\n");

    /* --- TEV stages (fully inlined, zero branches) --- */
    for (int s = 0; s < ns; s++) {
        PCGXTevStage* ts = &st->tev_stages[s];
        const char* swap_names[] = { "u_tev0_swap", "u_tev1_swap", "u_tev2_swap" };

        P("    /* TEV stage %d */\n", s);
        P("    {\n");
        P("        vec4 sTex = applySwap(texColor%d, u_swap_table[%s.y]);\n", s, swap_names[s]);
        P("        vec4 sRas = applySwap(rasColor,  u_swap_table[%s.x]);\n", swap_names[s]);

        /* Konst color/alpha for this stage */
        emit_kc(&sb, ts->k_color_sel);
        emit_ka(&sb, ts->k_alpha_sel);

        /* Color combiner: d ± mix(a, b, c) */
        P("        vec3 ca = %s;\n", tevC(ts->color_a));
        P("        vec3 cb = %s;\n", tevC(ts->color_b));
        P("        vec3 cc = %s;\n", tevC(ts->color_c));
        P("        vec3 cd = %s;\n", tevC(ts->color_d));
        if (ts->color_op == 1)
            P("        vec3 cResult = cd - mix(ca, cb, cc);\n");
        else
            P("        vec3 cResult = cd + mix(ca, cb, cc);\n");

        /* Color BSC (bias/scale) */
        if (ts->color_bias == 1) P("        cResult += 0.5;\n");
        else if (ts->color_bias == 2) P("        cResult -= 0.5;\n");
        if (ts->color_scale == 1) P("        cResult *= 2.0;\n");
        else if (ts->color_scale == 2) P("        cResult *= 4.0;\n");
        else if (ts->color_scale == 3) P("        cResult *= 0.5;\n");

        /* Color write to register */
        emit_write_rgb(&sb, ts->color_out, ts->color_clamp);

        /* Alpha combiner: d ± mix(a, b, c) */
        P("        float aa = %s;\n", tevA(ts->alpha_a));
        P("        float ab = %s;\n", tevA(ts->alpha_b));
        P("        float ac = %s;\n", tevA(ts->alpha_c));
        P("        float ad = %s;\n", tevA(ts->alpha_d));
        if (ts->alpha_op == 1)
            P("        float aResult = ad - mix(aa, ab, ac);\n");
        else
            P("        float aResult = ad + mix(aa, ab, ac);\n");

        /* Alpha BSC */
        if (ts->alpha_bias == 1) P("        aResult += 0.5;\n");
        else if (ts->alpha_bias == 2) P("        aResult -= 0.5;\n");
        if (ts->alpha_scale == 1) P("        aResult *= 2.0;\n");
        else if (ts->alpha_scale == 2) P("        aResult *= 4.0;\n");
        else if (ts->alpha_scale == 3) P("        aResult *= 0.5;\n");

        /* Alpha write to register */
        emit_write_a(&sb, ts->alpha_out, ts->alpha_clamp);

        P("    }\n\n");
    }

    /* --- Alpha compare (hardcoded test) --- */
    if (st->alpha_comp0 != 7 || st->alpha_comp1 != 7) {
        P("    /* Alpha test */\n");
        P("    float ref0 = float(u_alpha_ref0) / 255.0;\n");
        P("    float ref1 = float(u_alpha_ref1) / 255.0;\n");
        emit_acomp(&sb, "pass0", st->alpha_comp0, "ref0");
        emit_acomp(&sb, "pass1", st->alpha_comp1, "ref1");
        switch (st->alpha_op) {
            case 0: P("    if (!(pass0 && pass1)) discard;\n"); break;
            case 1: P("    if (!(pass0 || pass1)) discard;\n"); break;
            case 2: P("    if (!(pass0 != pass1)) discard;\n"); break;
            default: P("    if (!(pass0 == pass1)) discard;\n"); break;
        }
        P("\n");
    }

    /* --- Output --- */
    P("    fragColor = prev;\n\n");

    /* --- Fog (hardcoded on/off) --- */
    if (st->fog_type != 0) {
        P("    /* Fog */\n");
        P("    float fog_f = clamp((v_fog_z - u_fog_start) / max(u_fog_end - u_fog_start, 1e-6), 0.0, 1.0);\n");
        P("    fragColor.rgb = mix(fragColor.rgb, u_fog_color.rgb, fog_f);\n");
    }

    P("}\n");
    #undef P
    return buf;
}

/* Fallback uber-shader (loaded from files, used if specialization fails) */
static GLuint s_fallback = 0;
static PCGXUniformLocs s_fallback_locs;

/* ===================================================================
 * Program binary disk cache (shader_cache.bin in the working dir,
 * next to settings.ini). Kills first-visit shader compile hitches on
 * every run after the config was first seen.
 *
 * Layout: [magic u32][version u32][driver_hash u32] then per entry:
 * [ShaderKey][binary_format u32][length u32][data]
 * =================================================================== */
#ifdef SHADER_DISK_CACHE

#define SDC_FILE    "shader_cache.bin"
#define SDC_MAGIC   0x41435343u /* "ACSC" */
#define SDC_VERSION 1u
#define SDC_MAX_BIN (1u << 20)  /* sanity cap per program binary */

static int s_sdc_ok = 0; /* file exists with valid header; safe to append */

/* Binaries are driver-specific: mix renderer+version strings into the header
 * so a GPU driver update invalidates the cache instead of feeding stale blobs. */
static u32 sdc_driver_hash(void) {
    u32 h = 0x811c9dc5u;
    const char* strs[2];
    strs[0] = (const char*)glGetString(GL_RENDERER);
    strs[1] = (const char*)glGetString(GL_VERSION);
    for (int i = 0; i < 2; i++) {
        for (const char* p = strs[i]; p && *p; p++) {
            h ^= (u8)*p;
            h *= 0x01000193u;
        }
    }
    return h;
}

static int sdc_supported(void) {
    return glProgramBinary != NULL && glGetProgramBinary != NULL &&
           getenv("PC_NO_SHADER_CACHE") == NULL;
}

static void sdc_write_header(void) {
    FILE* f = fopen(SDC_FILE, "wb");
    if (!f) return;
    u32 hdr[3] = { SDC_MAGIC, SDC_VERSION, sdc_driver_hash() };
    if (fwrite(hdr, sizeof(hdr), 1, f) == 1) s_sdc_ok = 1;
    fclose(f);
}

static void sdc_load(void) {
    if (!sdc_supported()) return;

    FILE* f = fopen(SDC_FILE, "rb");
    if (!f) { sdc_write_header(); return; }

    u32 hdr[3];
    if (fread(hdr, sizeof(hdr), 1, f) != 1 ||
        hdr[0] != SDC_MAGIC || hdr[1] != SDC_VERSION || hdr[2] != sdc_driver_hash()) {
        fclose(f);
        printf("[PC/TEV] Shader disk cache stale/invalid, rebuilding\n");
        sdc_write_header();
        return;
    }

    int loaded = 0;
    for (;;) {
        ShaderKey key;
        u32 meta[2]; /* binary_format, length */
        if (fread(&key, sizeof(key), 1, f) != 1) break;
        if (fread(meta, sizeof(meta), 1, f) != 1) break;
        if (meta[1] == 0 || meta[1] > SDC_MAX_BIN) break;
        void* bin = malloc(meta[1]);
        if (!bin) break;
        if (fread(bin, 1, meta[1], f) != meta[1]) { free(bin); break; }

        if (!cache_lookup(&key)) {
            GLuint prog = glCreateProgram();
            glProgramBinary(prog, (GLenum)meta[0], bin, (GLsizei)meta[1]);
            GLint ok = 0;
            glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            if (ok) {
                cache_insert(&key, prog);
                loaded++;
            } else {
                glDeleteProgram(prog); /* driver rejected blob; will recompile */
            }
        }
        free(bin);
    }
    fclose(f);
    s_sdc_ok = 1;
    if (loaded)
        printf("[PC/TEV] Preloaded %d shader(s) from disk cache\n", loaded);
}

static void sdc_append(const ShaderKey* key, GLuint prog) {
    if (!s_sdc_ok || !sdc_supported()) return;

    GLint len = 0;
    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0 || (u32)len > SDC_MAX_BIN) return;
    void* bin = malloc((size_t)len);
    if (!bin) return;
    GLsizei written = 0;
    GLenum fmt = 0;
    glGetProgramBinary(prog, len, &written, &fmt, bin);
    if (written > 0) {
        FILE* f = fopen(SDC_FILE, "ab");
        if (f) {
            u32 meta[2] = { (u32)fmt, (u32)written };
            fwrite(key, sizeof(*key), 1, f);
            fwrite(meta, sizeof(meta), 1, f);
            fwrite(bin, 1, (size_t)written, f);
            fclose(f);
        }
    }
    free(bin);
}

/* --- Seed warmup ---------------------------------------------------
 * shaders/shader_seed.bin ships with the port: same container format as
 * shader_cache.bin but the binary blobs are stripped (len=0) — only the
 * driver-independent ShaderKeys matter. On first launch (empty local cache)
 * every seeded TEV config is generated and compiled during the LOADING
 * splash, then appended to the local cache as a driver binary, so gameplay
 * never compiles a known shader — even on the very first run. */

/* Rebuild the minimal PCGXState that generate_frag() reads from a key.
 * Every field generate_frag consults is captured in ShaderKey. */
static void state_from_key(const ShaderKey* k, PCGXState* st) {
    st->num_tev_stages = k->num_stages;
    st->num_chans = k->num_chans;
    st->chan_ctrl_enable[0] = k->light_enable;
    st->chan_ctrl_mat_src[0] = k->light_mat_src;
    st->chan_ctrl_amb_src[0] = k->light_amb_src;
    st->chan_ctrl_enable[1] = k->alpha_light_en;
    st->chan_ctrl_mat_src[1] = k->alpha_mat_src;
    st->fog_type = k->fog_enabled;
    st->alpha_comp0 = k->alpha_comp0;
    st->alpha_op = k->alpha_aop;
    st->alpha_comp1 = k->alpha_comp1;
    for (int i = 0; i < k->num_stages && i < PC_GX_MAX_TEV_STAGES; i++) {
        PCGXTevStage* ts = &st->tev_stages[i];
        ts->color_a = k->s[i].cin[0]; ts->color_b = k->s[i].cin[1];
        ts->color_c = k->s[i].cin[2]; ts->color_d = k->s[i].cin[3];
        ts->alpha_a = k->s[i].ain[0]; ts->alpha_b = k->s[i].ain[1];
        ts->alpha_c = k->s[i].ain[2]; ts->alpha_d = k->s[i].ain[3];
        ts->color_op = k->s[i].color_op;
        ts->alpha_op = k->s[i].alpha_op;
        ts->color_bias = k->s[i].color_bias;
        ts->color_scale = k->s[i].color_scale;
        ts->alpha_bias = k->s[i].alpha_bias;
        ts->alpha_scale = k->s[i].alpha_scale;
        ts->color_clamp = k->s[i].color_clamp;
        ts->alpha_clamp = k->s[i].alpha_clamp;
        ts->color_out = k->s[i].color_out;
        ts->alpha_out = k->s[i].alpha_out;
        ts->k_color_sel = k->s[i].k_color_sel;
        ts->k_alpha_sel = k->s[i].k_alpha_sel;
    }
}

static void sdc_warm_from_seed(void) {
    FILE* f = fopen("shaders/shader_seed.bin", "rb");
    if (!f) return;

    u32 hdr[3];
    if (fread(hdr, sizeof(hdr), 1, f) != 1 ||
        hdr[0] != SDC_MAGIC || hdr[1] != SDC_VERSION) {
        fclose(f);
        return; /* seed from a different format version — ignore */
    }
    /* driver hash intentionally NOT checked: only the keys are used */

    /* PCGXState is ~3MB (vertex buffer) — heap, not stack */
    PCGXState* st = (PCGXState*)calloc(1, sizeof(PCGXState));
    if (!st) { fclose(f); return; }

    int compiled = 0;
    for (;;) {
        ShaderKey key;
        u32 meta[2];
        if (fread(&key, sizeof(key), 1, f) != 1) break;
        if (fread(meta, sizeof(meta), 1, f) != 1) break;
        if (meta[1] > SDC_MAX_BIN) break;
        if (meta[1] > 0 && fseek(f, (long)meta[1], SEEK_CUR) != 0) break;

        if (cache_lookup(&key)) continue; /* already loaded from local cache */

        memset(st, 0, sizeof(PCGXState));
        state_from_key(&key, st);
        char* frag_src = generate_frag(st);
        if (!frag_src) continue;
        GLuint prog = 0;
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
        if (fs) prog = link_with_vs(fs);
        free(frag_src);
        if (prog) {
            cache_insert(&key, prog);
            sdc_append(&key, prog); /* persist as driver binary for next boot */
            compiled++;
        }
    }
    free(st);
    fclose(f);
    if (compiled)
        printf("[PC/TEV] Warmed %d shader(s) from seed during boot\n", compiled);
}

#endif /* SHADER_DISK_CACHE */

/* ===================================================================
 * Public API
 * =================================================================== */
void pc_gx_tev_init(void) {
    /* Compile vertex shader once (kept alive for all programs) */
    char* vs_src = load_shader("default.vert");
    if (!vs_src) {
        fprintf(stderr, "FATAL: Missing shaders/default.vert\n");
        exit(1);
    }
    s_vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    free(vs_src);
    if (!s_vs) {
        fprintf(stderr, "FATAL: Vertex shader compilation failed\n");
        exit(1);
    }

    /* Load uber-shader as fallback */
    char* fs_src = load_shader("default.frag");
    if (fs_src) {
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
        if (fs) s_fallback = link_with_vs(fs);
        free(fs_src);
    }
    if (s_fallback) {
        pc_gx_fill_uniform_locations(s_fallback, &s_fallback_locs);
        printf("[PC/TEV] Fallback uber-shader ready. Specialized shaders will be generated on demand.\n");
    } else {
        printf("[PC/TEV] Warning: no fallback shader. All shaders will be generated.\n");
    }

    memset(s_cache, 0, sizeof(s_cache));

#ifdef SHADER_DISK_CACHE
    /* Preload program binaries from previous runs — happens during boot,
     * before gameplay, so first visits to areas no longer compile mid-frame. */
    sdc_load();
    /* First launch (or new configs in the seed): compile every known TEV
     * config now, while the LOADING splash is up, instead of mid-gameplay. */
    sdc_warm_from_seed();
#endif
}

void pc_gx_tev_shutdown(void) {
    for (int i = 0; i < SCACHE_SIZE; i++) {
        if (s_cache[i].valid && s_cache[i].program) {
            glDeleteProgram(s_cache[i].program);
        }
    }
    memset(s_cache, 0, sizeof(s_cache));
    if (s_fallback) { glDeleteProgram(s_fallback); s_fallback = 0; }
    if (s_vs) { glDeleteShader(s_vs); s_vs = 0; }
    pc_gx_tev_last_locs = NULL;
}

GLuint pc_gx_tev_get_shader(PCGXState* state) {
    ShaderKey key;
    build_key(state, &key);

    /* Fast path: cached lookup */
    SCacheEntry* e = cache_lookup(&key);
    if (e) {
        pc_gx_tev_last_locs = &e->locs;
        return e->program;
    }

    /* Cache miss: generate specialized fragment shader */
    GLuint prog = 0;
    char* frag_src = generate_frag(state);
    if (frag_src) {
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
        if (fs) {
            prog = link_with_vs(fs);
        }
        if (!prog) {
            /* Compilation failed — dump source for debugging */
            fprintf(stderr, "[PC/TEV] Specialized shader failed to compile. Source:\n%s\n", frag_src);
        }
        free(frag_src);
    }

    if (prog) {
        e = cache_insert(&key, prog);
        pc_gx_tev_last_locs = &e->locs;
#ifdef SHADER_DISK_CACHE
        sdc_append(&key, prog);
#endif
        static int s_shader_count = 0;
        s_shader_count++;
        printf("[PC/TEV] Compiled specialized shader #%d\n", s_shader_count);
        return prog;
    }

    /* Fall back to uber-shader */
    pc_gx_tev_last_locs = &s_fallback_locs;
    return s_fallback;
}
