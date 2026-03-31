/* pc_overlay.c - FPS counter + in-game settings menu with embedded bitmap font */
#include "pc_platform.h"
#include "pc_overlay.h"
#include "pc_settings.h"
#include "pc_keybindings.h"

/* ---- Vertex type ---- */
typedef struct { float x, y, u, v, r, g, b, a; } OvVtx;
#define OV_MAX_VERTS 3072
static OvVtx ov_verts[OV_MAX_VERTS]; /* static to avoid 96KB on stack */

/* ---- GL objects ---- */
static GLuint ov_prog = 0;
static GLuint ov_vao = 0;
static GLuint ov_vbo = 0;
static GLuint ov_tex = 0;
static GLint  ov_loc_font = -1;

/* ---- FPS state ---- */
static double ov_fps   = 60.0;
static double ov_speed = 100.0;

/* ---- Menu state ---- */
int g_pc_menu_open = 0;
static int menu_cursor = 0;

/* Input repeat: initial delay then fast repeat */
static int rep_up = 0, rep_down = 0, rep_left = 0, rep_right = 0;
#define REP_DELAY  16  /* frames before repeat starts */
#define REP_RATE    3  /* frames between repeats */

/* ---- Controls tab: scroll + keybind capture ---- */
#define CTRL_VISIBLE_ROWS 8
static int   ctrl_scroll       = 0;   /* scroll offset within Controls tab */
static int   ctrl_capture_idx  = -1;  /* index into KB_COUNT being captured, -1 = none */
static Uint32 ctrl_capture_start = 0; /* SDL_GetTicks() when capture started */
#define CTRL_CAPTURE_MS 3000
static Uint8 s_prev_keys[SDL_NUM_SCANCODES];              /* previous-frame keyboard state for edge detection */
static Uint8 s_prev_gamepad[SDL_CONTROLLER_BUTTON_MAX];   /* previous-frame gamepad button state */
static int   s_prev_ctrl_a = 0;                           /* gamepad A button last frame (for enter-capture edge) */
static Uint32 ctrl_clear_start  = 0;  /* SDL_GetTicks() when X/Del hold started on a KB item, 0 = not held */
static int    ctrl_clear_kb_idx = -1; /* which kb_idx is being held for clear */
#define CTRL_CLEAR_MS 1500            /* hold this long to clear a binding */

/* ---- Menu items ---- */
enum {
    MI_FPS_COUNTER,
    MI_MASTER_VOLUME,
    MI_BGM_VOLUME,
    MI_SFX_VOLUME,
    MI_VOICE_VOLUME,
    MI_FULLSCREEN,
    MI_VSYNC,
    MI_MSAA,
    MI_ZOOM_ENABLED,
    MI_FPS_TARGET,
    MI_RENDER_SCALE,
    MI_WINDOW_SIZE,
    MI_SCALE_MODE,
    MI_VERBOSE,
    /* Controls tab */
    MI_CTRL_SWAP_AB,
    MI_CTRL_DPAD_STICK,
    MI_LEFT_DEADZONE,
    MI_RIGHT_DEADZONE,
    MI_CTRL_RESET,
    MI_KB_BASE,
    MI_COUNT = MI_KB_BASE + KB_COUNT,
};

/* Labels for non-KB items; KB items use pc_keybinding_label() at draw time */
static const char* menu_labels[MI_COUNT] = {
    [MI_FPS_COUNTER]     = "FPS Counter",
    [MI_MASTER_VOLUME]   = "Master Volume",
    [MI_BGM_VOLUME]      = "Music",
    [MI_SFX_VOLUME]      = "Sound Effects",
    [MI_VOICE_VOLUME]    = "Voices",
    [MI_FULLSCREEN]      = "Fullscreen",
    [MI_VSYNC]           = "V-Sync",
    [MI_MSAA]            = "MSAA",
    [MI_ZOOM_ENABLED]    = "Camera Zoom",
    [MI_FPS_TARGET]      = "FPS Target",
    [MI_RENDER_SCALE]    = "Render Scale",
    [MI_WINDOW_SIZE]     = "Render Res",
    [MI_SCALE_MODE]      = "Scale Mode",
    [MI_VERBOSE]         = "Verbose Log",
    [MI_CTRL_SWAP_AB]    = "Swap A/B, X/Y",
    [MI_CTRL_DPAD_STICK] = "Dpad as Joystick",
    [MI_LEFT_DEADZONE]   = "L-Stick Deadzone",
    [MI_RIGHT_DEADZONE]  = "R-Stick Deadzone",
    [MI_CTRL_RESET]      = "Reset Defaults",
    /* MI_KB_BASE..MI_KB_BASE+KB_COUNT-1: NULL, handled via pc_keybinding_label() */
};

static const char* get_item_label(int i) {
    if (i >= MI_KB_BASE && i < MI_KB_BASE + KB_COUNT)
        return pc_keybinding_label(i - MI_KB_BASE);
    return menu_labels[i];
}

/* ---- Menu tabs ---- */
enum { TAB_VIDEO, TAB_AUDIO, TAB_CONTROLS, TAB_DEBUG, TAB_COUNT };
static const char* tab_labels[TAB_COUNT] = { "VIDEO", "AUDIO", "CTRL", "DEBUG" };
static int s_active_tab = 0;

/* Which tab each menu item belongs to (indexed by MI_*) */
static const int menu_item_tab[MI_COUNT] = {
    [MI_FPS_COUNTER]     = TAB_DEBUG,
    [MI_MASTER_VOLUME]   = TAB_AUDIO,
    [MI_BGM_VOLUME]      = TAB_AUDIO,
    [MI_SFX_VOLUME]      = TAB_AUDIO,
    [MI_VOICE_VOLUME]    = TAB_AUDIO,
    [MI_FULLSCREEN]      = TAB_VIDEO,
    [MI_VSYNC]           = TAB_VIDEO,
    [MI_MSAA]            = TAB_VIDEO,
    [MI_ZOOM_ENABLED]    = TAB_VIDEO,
    [MI_FPS_TARGET]      = TAB_VIDEO,
    [MI_RENDER_SCALE]    = TAB_VIDEO,
    [MI_WINDOW_SIZE]     = TAB_VIDEO,
    [MI_SCALE_MODE]      = TAB_VIDEO,
    [MI_VERBOSE]         = TAB_DEBUG,
    [MI_CTRL_SWAP_AB]    = TAB_CONTROLS,
    [MI_CTRL_DPAD_STICK] = TAB_CONTROLS,
    [MI_LEFT_DEADZONE]   = TAB_CONTROLS,
    [MI_RIGHT_DEADZONE]  = TAB_CONTROLS,
    [MI_CTRL_RESET]      = TAB_CONTROLS,
    /* MI_KB_BASE..MI_KB_BASE+KB_COUNT-1: handled by item_tab() helper below */
};

/* Tab for item i — KB items always belong to TAB_CONTROLS */
static int item_tab(int i) {
    if (i >= MI_KB_BASE && i < MI_KB_BASE + KB_COUNT) return TAB_CONTROLS;
    return menu_item_tab[i];
}

/* Returns index of nth visible item in the active tab, or -1 if out of range. */
static int tab_item_at(int n) {
    int found = 0;
    for (int i = 0; i < MI_COUNT; i++) {
        if (item_tab(i) == s_active_tab) {
            if (found == n) return i;
            found++;
        }
    }
    return -1;
}
static int tab_item_count(void) {
    int count = 0;
    for (int i = 0; i < MI_COUNT; i++)
        if (item_tab(i) == s_active_tab) count++;
    return count;
}

static void menu_get_value(int item, char* buf, int sz) {
    switch (item) {
    case MI_FPS_COUNTER:   snprintf(buf, sz, "%s", g_pc_settings.show_fps ? "ON" : "OFF"); break;
    case MI_MASTER_VOLUME: snprintf(buf, sz, "%d%%", g_pc_settings.master_volume); break;
    case MI_BGM_VOLUME:    snprintf(buf, sz, "%d%%", g_pc_settings.bgm_volume); break;
    case MI_SFX_VOLUME:    snprintf(buf, sz, "%d%%", g_pc_settings.sfx_volume); break;
    case MI_VOICE_VOLUME:  snprintf(buf, sz, "%d%%", g_pc_settings.voice_volume); break;
    case MI_FULLSCREEN: {
        const char* m[] = {"Windowed", "Fullscreen", "Borderless"};
        snprintf(buf, sz, "%s", m[g_pc_settings.fullscreen]);
        break;
    }
    case MI_VSYNC:         snprintf(buf, sz, "%s", g_pc_settings.vsync ? "ON" : "OFF"); break;
    case MI_MSAA:
        if (g_pc_settings.msaa == 0) snprintf(buf, sz, "Off");
        else snprintf(buf, sz, "%dx", g_pc_settings.msaa);
        break;
    case MI_ZOOM_ENABLED:  snprintf(buf, sz, "%s", g_pc_settings.zoom_enabled ? "ON" : "OFF"); break;
    case MI_FPS_TARGET: {
        static const char* names[] = {"60 FPS", "50 FPS", "40 FPS", "30 FPS", "20 FPS", "Unlimited", "Auto", "Dynamic"};
        int t = g_pc_settings.fps_target;
        if (t < 0 || t > 7) t = 0;
        snprintf(buf, sz, "%s", names[t]);
        break;
    }
    case MI_RENDER_SCALE:
        snprintf(buf, sz, "%d%%", g_pc_settings.render_scale);
        break;
    case MI_WINDOW_SIZE: {
        static const char* wsnames[] = {"320x240", "480x360", "640x480", "960x720", "1280x960", "Custom"};
        int w = g_pc_settings.window_size;
        if (w < 0 || w > 5) w = 2;
        snprintf(buf, sz, "%s", wsnames[w]);
        break;
    }
    case MI_SCALE_MODE:
        snprintf(buf, sz, "%s", g_pc_settings.scale_mode == 1 ? "Center" : "Stretch");
        break;
    case MI_VERBOSE:         snprintf(buf, sz, "%s", g_pc_settings.verbose ? "ON" : "OFF"); break;
    case MI_CTRL_SWAP_AB:    snprintf(buf, sz, "Press >"); break;
    case MI_CTRL_DPAD_STICK: snprintf(buf, sz, "%s", g_pc_settings.dpad_as_stick ? "ON" : "OFF"); break;
    case MI_LEFT_DEADZONE:   snprintf(buf, sz, "%d%%", g_pc_settings.left_deadzone); break;
    case MI_RIGHT_DEADZONE:  snprintf(buf, sz, "%d%%", g_pc_settings.right_deadzone); break;
    case MI_CTRL_RESET:      snprintf(buf, sz, "Press >"); break;
    default:
        if (item >= MI_KB_BASE && item < MI_KB_BASE + KB_COUNT) {
            int kb_idx = item - MI_KB_BASE;
            if (kb_idx == ctrl_capture_idx) {
                /* Rebind capture: show countdown */
                Uint32 elapsed = SDL_GetTicks() - ctrl_capture_start;
                int secs_left = (int)((CTRL_CAPTURE_MS - (int)elapsed + 999) / 1000);
                if (secs_left < 1) secs_left = 1;
                snprintf(buf, sz, "[%d...]", secs_left);
            } else if (kb_idx == ctrl_clear_kb_idx && ctrl_clear_start) {
                /* Hold-to-clear: show shrinking countdown */
                int ms_left = CTRL_CLEAR_MS - (int)(SDL_GetTicks() - ctrl_clear_start);
                if (ms_left < 0) ms_left = 0;
                snprintf(buf, sz, "[CLR %d]", (ms_left + 999) / 1000);
            } else {
                PCInputCode* p = pc_keybinding_ptr(kb_idx);
                PCInputCode code = p ? *p : -1;
                if (code < 0) snprintf(buf, sz, "[NONE]");
                else if ((unsigned)code & PC_INPUT_GAMEPAD_BIT) {
                    const char* s = SDL_GameControllerGetStringForButton(
                        (SDL_GameControllerButton)(code & 0xFF));
                    snprintf(buf, sz, "GP %s", s ? s : "?");
                } else {
                    const char* name = SDL_GetScancodeName((SDL_Scancode)code);
                    snprintf(buf, sz, "%.*s", sz - 1, name);
                }
            }
        }
        break;
    }
}

static void menu_adjust_vol(int* vol, int dir) {
    int v = *vol + dir * 5;
    if (v < 0) v = 0; if (v > 100) v = 100;
    *vol = v;
}

static void menu_adjust(int item, int dir) {
    switch (item) {
    case MI_FPS_COUNTER:   g_pc_settings.show_fps ^= 1; break;
    case MI_MASTER_VOLUME: menu_adjust_vol(&g_pc_settings.master_volume, dir); break;
    case MI_BGM_VOLUME:    menu_adjust_vol(&g_pc_settings.bgm_volume, dir); break;
    case MI_SFX_VOLUME:    menu_adjust_vol(&g_pc_settings.sfx_volume, dir); break;
    case MI_VOICE_VOLUME:  menu_adjust_vol(&g_pc_settings.voice_volume, dir); break;
    case MI_FULLSCREEN: {
        int v = g_pc_settings.fullscreen + dir;
        if (v < 0) v = 2; if (v > 2) v = 0;
        g_pc_settings.fullscreen = v;
        pc_settings_apply();
        break;
    }
    case MI_VSYNC:
        g_pc_settings.vsync ^= 1;
        pc_settings_apply();
        break;
    case MI_MSAA: {
        int vals[] = {0, 2, 4, 8};
        int cur = 0;
        for (int i = 0; i < 4; i++) if (vals[i] == g_pc_settings.msaa) cur = i;
        cur += dir;
        if (cur < 0) cur = 3; if (cur > 3) cur = 0;
        g_pc_settings.msaa = vals[cur];
        break; /* MSAA needs restart */
    }
    case MI_ZOOM_ENABLED:  g_pc_settings.zoom_enabled ^= 1; break;
    case MI_FPS_TARGET: {
        /* Right = faster (lower index), Left = slower (higher index) */
        int v = g_pc_settings.fps_target - dir;
        if (v < 0) v = 7; if (v > 7) v = 0;
        g_pc_settings.fps_target = v;
        static const int fps_hz[8] = {60, 50, 40, 30, 20, 0, 60, 60};
        g_pc_fps_target = fps_hz[v];
        if (v == 5) g_pc_no_framelimit = 1;
        else        g_pc_no_framelimit = 0;
        extern void pc_dynamic_fps_reset(void);
        pc_dynamic_fps_reset();
        break;
    }
    case MI_RENDER_SCALE: {
        static const int scales[] = {25, 50, 75, 100};
        int cur = 3; /* default to 100% */
        for (int i = 0; i < 4; i++) if (scales[i] == g_pc_settings.render_scale) { cur = i; break; }
        cur += dir;
        if (cur < 0) cur = 3; if (cur > 3) cur = 0;
        g_pc_settings.render_scale = scales[cur];
        pc_settings_apply();
        break;
    }
    case MI_WINDOW_SIZE: {
        int v = g_pc_settings.window_size + dir;
        if (v < 0) v = 4; if (v > 4) v = 0; /* skip Custom (5) from menu cycling */
        g_pc_settings.window_size = v;
        pc_settings_apply();
        break;
    }
    case MI_SCALE_MODE:
        g_pc_settings.scale_mode ^= 1;
        g_pc_scale_mode = g_pc_settings.scale_mode;
        break;
    case MI_VERBOSE:         g_pc_settings.verbose ^= 1; break;
    case MI_CTRL_SWAP_AB: {
        /* Swap A↔B and X↔Y directly in keybindings so the mapping list reflects it */
        PCInputCode tmp;
        tmp = g_pc_keybindings.a; g_pc_keybindings.a = g_pc_keybindings.b; g_pc_keybindings.b = tmp;
        tmp = g_pc_keybindings.x; g_pc_keybindings.x = g_pc_keybindings.y; g_pc_keybindings.y = tmp;
        pc_keybindings_save();
        break;
    }
    case MI_CTRL_DPAD_STICK: g_pc_settings.dpad_as_stick ^= 1; break;
    case MI_LEFT_DEADZONE: {
        int v = g_pc_settings.left_deadzone + dir * 5;
        if (v < 0) v = 0; if (v > 50) v = 50;
        g_pc_settings.left_deadzone = v;
        break;
    }
    case MI_RIGHT_DEADZONE: {
        int v = g_pc_settings.right_deadzone + dir * 5;
        if (v < 0) v = 0; if (v > 50) v = 50;
        g_pc_settings.right_deadzone = v;
        break;
    }
    case MI_CTRL_RESET:      pc_keybindings_reset(); break;
    default:
        if (item >= MI_KB_BASE && item < MI_KB_BASE + KB_COUNT && dir == 1) {
            ctrl_capture_idx = item - MI_KB_BASE;
            ctrl_capture_start = SDL_GetTicks();
        }
        break;
    }
}

/* ---- Embedded shaders ---- */
#ifdef PC_USE_GLES
static const char* ov_vert_src =
    "#version 310 es\n"
    "precision mediump float;\n"
    "precision highp int;\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "out vec2 v_uv; out vec4 v_col;\n"
    "void main(){gl_Position=vec4(a_pos,0.0,1.0);v_uv=a_uv;v_col=a_col;}\n";
static const char* ov_frag_src =
    "#version 310 es\n"
    "precision mediump float;\n"
    "precision highp int;\n"
    "in vec2 v_uv; in vec4 v_col;\n"
    "uniform sampler2D u_font;\n"
    "out vec4 fragColor;\n"
    "void main(){float a=texture(u_font,v_uv).r;fragColor=vec4(v_col.rgb,v_col.a*a);}\n";
#else
static const char* ov_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "out vec2 v_uv; out vec4 v_col;\n"
    "void main(){gl_Position=vec4(a_pos,0.0,1.0);v_uv=a_uv;v_col=a_col;}\n";
static const char* ov_frag_src =
    "#version 330 core\n"
    "in vec2 v_uv; in vec4 v_col;\n"
    "uniform sampler2D u_font;\n"
    "out vec4 fragColor;\n"
    "void main(){float a=texture(u_font,v_uv).r;fragColor=vec4(v_col.rgb,v_col.a*a);}\n";
#endif

/* ---- 8x8 bitmap font (ASCII 32-127 = 96 glyphs, IBM PC BIOS style) ---- */
static const unsigned char font_8x8[96][8] = {
    /* 32 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 '!' */ {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    /* 34 '"' */ {0x6C,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00},
    /* 35 '#' */ {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00},
    /* 36 '$' */ {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00},
    /* 37 '%' */ {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00},
    /* 38 '&' */ {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00},
    /* 39 '\''*/ {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    /* 40 '(' */ {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    /* 41 ')' */ {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    /* 42 '*' */ {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    /* 43 '+' */ {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    /* 44 ',' */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    /* 45 '-' */ {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    /* 46 '.' */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    /* 47 '/' */ {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},
    /* 48 '0' */ {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00},
    /* 49 '1' */ {0x18,0x38,0x78,0x18,0x18,0x18,0x7E,0x00},
    /* 50 '2' */ {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00},
    /* 51 '3' */ {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00},
    /* 52 '4' */ {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00},
    /* 53 '5' */ {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00},
    /* 54 '6' */ {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00},
    /* 55 '7' */ {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00},
    /* 56 '8' */ {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00},
    /* 57 '9' */ {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00},
    /* 58 ':' */ {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},
    /* 59 ';' */ {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
    /* 60 '<' */ {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},
    /* 61 '=' */ {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00},
    /* 62 '>' */ {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},
    /* 63 '?' */ {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00},
    /* 64 '@' */ {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00},
    /* 65 'A' */ {0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00},
    /* 66 'B' */ {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00},
    /* 67 'C' */ {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00},
    /* 68 'D' */ {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00},
    /* 69 'E' */ {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00},
    /* 70 'F' */ {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00},
    /* 71 'G' */ {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00},
    /* 72 'H' */ {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00},
    /* 73 'I' */ {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    /* 74 'J' */ {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00},
    /* 75 'K' */ {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00},
    /* 76 'L' */ {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00},
    /* 77 'M' */ {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00},
    /* 78 'N' */ {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00},
    /* 79 'O' */ {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    /* 80 'P' */ {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    /* 81 'Q' */ {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06},
    /* 82 'R' */ {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00},
    /* 83 'S' */ {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00},
    /* 84 'T' */ {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x00},
    /* 85 'U' */ {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    /* 86 'V' */ {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00},
    /* 87 'W' */ {0xC6,0xC6,0xD6,0xFE,0xFE,0xEE,0xC6,0x00},
    /* 88 'X' */ {0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00},
    /* 89 'Y' */ {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00},
    /* 90 'Z' */ {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00},
    /* 91 '[' */ {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    /* 92 '\\'*/ {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00},
    /* 93 ']' */ {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    /* 94 '^' */ {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    /* 95 '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    /* 96 '`' */ {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},
    /* 97 'a' */ {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00},
    /* 98 'b' */ {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00},
    /* 99 'c' */ {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00},
    /*100 'd' */ {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00},
    /*101 'e' */ {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00},
    /*102 'f' */ {0x1C,0x36,0x30,0x78,0x30,0x30,0x78,0x00},
    /*103 'g' */ {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x78},
    /*104 'h' */ {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00},
    /*105 'i' */ {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    /*106 'j' */ {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C},
    /*107 'k' */ {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00},
    /*108 'l' */ {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    /*109 'm' */ {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0x00},
    /*110 'n' */ {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00},
    /*111 'o' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00},
    /*112 'p' */ {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0},
    /*113 'q' */ {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E},
    /*114 'r' */ {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00},
    /*115 's' */ {0x00,0x00,0x7C,0xC0,0x7C,0x06,0xFC,0x00},
    /*116 't' */ {0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00},
    /*117 'u' */ {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00},
    /*118 'v' */ {0x00,0x00,0xC6,0xC6,0x6C,0x38,0x10,0x00},
    /*119 'w' */ {0x00,0x00,0xC6,0xD6,0xD6,0xFE,0x6C,0x00},
    /*120 'x' */ {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00},
    /*121 'y' */ {0x00,0x00,0xC6,0xC6,0xCE,0x76,0x06,0x7C},
    /*122 'z' */ {0x00,0x00,0xFC,0x98,0x30,0x64,0xFC,0x00},
    /*123 '{' */ {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
    /*124 '|' */ {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    /*125 '}' */ {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
    /*126 '~' */ {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00},
    /*127 solid*/{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
};

/* ---- Shader compile helper ---- */
static GLuint ov_compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "[OVERLAY] shader error: %s\n", log);
    }
    return s;
}

/* ---- Vertex helpers ---- */
static int ov_nv; /* current vertex count */

static void ov_push(float px, float py, float u, float vt,
                    float r, float g, float b, float a) {
    if (ov_nv >= OV_MAX_VERTS) return;
    OvVtx* p = &ov_verts[ov_nv++];
    p->x = px / (float)g_pc_window_w * 2.0f - 1.0f;
    p->y = 1.0f - py / (float)g_pc_window_h * 2.0f;
    p->u = u; p->v = vt;
    p->r = r; p->g = g; p->b = b; p->a = a;
}

static void ov_quad(float x, float y, float w, float h,
                    float u0, float v0, float u1, float v1,
                    float r, float g, float b, float a) {
    ov_push(x,     y,     u0, v0, r, g, b, a);
    ov_push(x,     y + h, u0, v1, r, g, b, a);
    ov_push(x + w, y,     u1, v0, r, g, b, a);
    ov_push(x + w, y,     u1, v0, r, g, b, a);
    ov_push(x,     y + h, u0, v1, r, g, b, a);
    ov_push(x + w, y + h, u1, v1, r, g, b, a);
}

/* Solid block UV (char 127 = all white) for backgrounds */
#define BG_U ((15.0f + 0.5f) / 16.0f)
#define BG_V ((5.0f + 0.5f) / 6.0f)

static void ov_char(int ch, float x, float y, float cw, float ch_h,
                    float r, float g, float b, float a) {
    int idx = ch - 32;
    if (idx < 0 || idx > 95) idx = 0;
    float col = (float)(idx % 16);
    float row = (float)(idx / 16);
    ov_quad(x, y, cw, ch_h,
            col / 16.0f, row / 6.0f,
            (col + 1.0f) / 16.0f, (row + 1.0f) / 6.0f,
            r, g, b, a);
}

static void ov_string(const char* s, float x, float y, float cw, float ch,
                      float r, float g, float b, float a) {
    for (int i = 0; s[i]; i++)
        ov_char((unsigned char)s[i], x + i * cw, y, cw, ch, r, g, b, a);
}

/* Right-aligned string ending at x_right */
static void ov_string_right(const char* s, float x_right, float y, float cw, float ch,
                            float r, float g, float b, float a) {
    int len = (int)strlen(s);
    ov_string(s, x_right - len * cw, y, cw, ch, r, g, b, a);
}

/* ---- Input repeat helper ---- */
static int ov_repeat(int held, int* timer) {
    if (!held) { *timer = 0; return 0; }
    if (*timer == 0) { *timer = 1; return 1; } /* first press */
    (*timer)++;
    if (*timer == REP_DELAY) { return 1; }
    if (*timer > REP_DELAY && ((*timer - REP_DELAY) % REP_RATE) == 0) { return 1; }
    return 0;
}

/* ---- Menu input (called from draw, polls SDL state) ---- */
static void menu_process_input(void) {
    if (!g_pc_menu_open) return;

    int nkeys;
    const Uint8* keys = SDL_GetKeyboardState(&nkeys);
    SDL_GameController* ctrl = pc_pad_get_controller();

    /* ---- Capture mode: intercept all input until key is pressed or timeout ---- */
    if (ctrl_capture_idx >= 0) {
        Uint32 elapsed = SDL_GetTicks() - ctrl_capture_start;
        if (elapsed >= CTRL_CAPTURE_MS) {
            /* Timed out — cancel */
            ctrl_capture_idx = -1;
        } else {
            /* Scan for any new keyboard press */
            for (int sc = SDL_SCANCODE_UNKNOWN + 1; sc < nkeys && sc < SDL_NUM_SCANCODES; sc++) {
                if (keys[sc] && !s_prev_keys[sc]) {
                    /* Set binding and exit capture */
                    PCInputCode* p = pc_keybinding_ptr(ctrl_capture_idx);
                    if (p) *p = (PCInputCode)sc;
                    pc_keybindings_save();
                    ctrl_capture_idx = -1;
                    break;
                }
            }
        }
        /* Also scan gamepad buttons (covers PortMaster and other gamepad-only devices) */
        if (ctrl_capture_idx >= 0 && ctrl) {
            int btn;
            for (btn = 0; btn < SDL_CONTROLLER_BUTTON_MAX; btn++) {
                int cur = SDL_GameControllerGetButton(ctrl, (SDL_GameControllerButton)btn);
                if (cur && !s_prev_gamepad[btn]) {
                    PCInputCode* p = pc_keybinding_ptr(ctrl_capture_idx);
                    if (p) *p = PC_INPUT_GAMEPAD_BTN(btn);
                    pc_keybindings_save();
                    ctrl_capture_idx = -1;
                    break;
                }
            }
        }

        /* Always update prev state and return — suppress normal nav in capture mode */
        if (nkeys > SDL_NUM_SCANCODES) nkeys = SDL_NUM_SCANCODES;
        SDL_memcpy(s_prev_keys, keys, nkeys);
        {
            int btn;
            for (btn = 0; btn < SDL_CONTROLLER_BUTTON_MAX; btn++)
                s_prev_gamepad[btn] = (Uint8)(ctrl ? SDL_GameControllerGetButton(ctrl, (SDL_GameControllerButton)btn) : 0);
        }
        s_prev_ctrl_a = ctrl ? SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_A) : 0;
        return;
    }

    /* ---- Normal navigation ---- */
    int up    = keys[SDL_SCANCODE_UP];
    int down  = keys[SDL_SCANCODE_DOWN];
    int left  = keys[SDL_SCANCODE_LEFT];
    int right = keys[SDL_SCANCODE_RIGHT];
    int tab_l = keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_PAGEUP];
    int tab_r = keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_PAGEDOWN];
    int ctrl_a = 0, ctrl_x = 0;

    if (ctrl) {
        up    |= SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_UP);
        down  |= SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        left  |= SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        right |= SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        tab_l |= SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        tab_r |= SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        ctrl_a = SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_A);
        ctrl_x = SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_X);
    }

    /* Tab switching */
    static int rep_tabl = 0, rep_tabr = 0;
    if (ov_repeat(tab_l, &rep_tabl)) {
        s_active_tab--;
        if (s_active_tab < 0) s_active_tab = TAB_COUNT - 1;
        menu_cursor = 0;
        ctrl_scroll = 0;
    }
    if (ov_repeat(tab_r, &rep_tabr)) {
        s_active_tab++;
        if (s_active_tab >= TAB_COUNT) s_active_tab = 0;
        menu_cursor = 0;
        ctrl_scroll = 0;
    }

    int count = tab_item_count();
    if (ov_repeat(up,   &rep_up))   { menu_cursor--; if (menu_cursor < 0) menu_cursor = count - 1; }
    if (ov_repeat(down, &rep_down)) { menu_cursor++; if (menu_cursor >= count) menu_cursor = 0; }

    /* Keep scroll window in sync with cursor (Controls tab only) */
    if (s_active_tab == TAB_CONTROLS) {
        if (menu_cursor < ctrl_scroll)
            ctrl_scroll = menu_cursor;
        if (menu_cursor >= ctrl_scroll + CTRL_VISIBLE_ROWS)
            ctrl_scroll = menu_cursor - CTRL_VISIBLE_ROWS + 1;
        if (ctrl_scroll < 0) ctrl_scroll = 0;
    }

    int mi = tab_item_at(menu_cursor);
    if (mi >= 0) {
        if (ov_repeat(left,  &rep_left))  menu_adjust(mi, -1);
        if (ov_repeat(right, &rep_right)) menu_adjust(mi, +1);

        /* Gamepad A (rising edge) on a KB item → enter capture */
        if (mi >= MI_KB_BASE && mi < MI_KB_BASE + KB_COUNT) {
            if (ctrl_a && !s_prev_ctrl_a) {
                ctrl_capture_idx = mi - MI_KB_BASE;
                ctrl_capture_start = SDL_GetTicks();
            }
            /* X button or Delete key held → clear binding after CTRL_CLEAR_MS.
             * Uses a hold timer rather than edge detection to prevent the button
             * just used for capture from immediately clearing the new binding. */
            {
                int x_or_del = ctrl_x || keys[SDL_SCANCODE_DELETE];
                if (x_or_del) {
                    if (!ctrl_clear_start) {
                        ctrl_clear_start  = SDL_GetTicks();
                        ctrl_clear_kb_idx = mi - MI_KB_BASE;
                    }
                    if (SDL_GetTicks() - ctrl_clear_start >= CTRL_CLEAR_MS) {
                        PCInputCode* p = pc_keybinding_ptr(mi - MI_KB_BASE);
                        if (p) *p = -1;
                        pc_keybindings_save();
                        ctrl_clear_start  = 0;
                        ctrl_clear_kb_idx = -1;
                    }
                } else {
                    ctrl_clear_start  = 0;
                    ctrl_clear_kb_idx = -1;
                }
            }
        }
    }

    /* Update edge-detection state */
    if (nkeys > SDL_NUM_SCANCODES) nkeys = SDL_NUM_SCANCODES;
    SDL_memcpy(s_prev_keys, keys, nkeys);
    {
        int btn;
        for (btn = 0; btn < SDL_CONTROLLER_BUTTON_MAX; btn++)
            s_prev_gamepad[btn] = (Uint8)(ctrl ? SDL_GameControllerGetButton(ctrl, (SDL_GameControllerButton)btn) : 0);
    }
    s_prev_ctrl_a = ctrl_a;
}

/* ---- Draw: FPS counter (top-right corner) ---- */
static void draw_fps(float cw, float ch, float pad) {
    char line1[32], line2[32];
    if (g_pc_fps_target > 0 && g_pc_settings.fps_target != 7)
        snprintf(line1, sizeof(line1), "%.1f/%d FPS", ov_fps, g_pc_fps_target);
    else
        snprintf(line1, sizeof(line1), "%.1f FPS", ov_fps);
    snprintf(line2, sizeof(line2), "%d%% Speed", (int)(ov_speed + 0.5));

    int len1 = (int)strlen(line1);
    int len2 = (int)strlen(line2);
    int max_len = len1 > len2 ? len1 : len2;

    float pw = max_len * cw + 2.0f * pad;
    float ph = 2.0f * ch + 3.0f * pad;
    float px = (float)g_pc_window_w - pw - pad;
    float py = pad;

    ov_quad(px, py, pw, ph, BG_U, BG_V, BG_U, BG_V, 0, 0, 0, 0.65f);
    ov_string(line1, px + pad, py + pad, cw, ch, 1, 1, 1, 1);
    ov_string(line2, px + pad, py + pad + ch + pad, cw, ch, 0.8f, 0.8f, 0.8f, 1);
}

/* ---- Draw: Settings menu (centered) ---- */
static void draw_menu(float cw, float ch, float pad) {
    int tab_count = tab_item_count();
    int cols = 28;
    int val_col = 18;

    /* Controls tab shows at most CTRL_VISIBLE_ROWS items */
    int vis_count = tab_count;
    int vis_start = 0;
    if (s_active_tab == TAB_CONTROLS) {
        vis_start = ctrl_scroll;
        vis_count = tab_count - ctrl_scroll;
        if (vis_count > CTRL_VISIBLE_ROWS) vis_count = CTRL_VISIBLE_ROWS;
    }

    /* Title row + tab bar row + blank + visible items + blank + 2 footer rows */
    int lines = 1 + 1 + 1 + vis_count + 1 + 2;

    float mw = cols * cw + 2.0f * pad;
    float mh = lines * (ch + pad) + pad;
    float mx = ((float)g_pc_window_w - mw) * 0.5f;
    float my = ((float)g_pc_window_h - mh) * 0.5f;

    /* Background */
    ov_quad(mx, my, mw, mh, BG_U, BG_V, BG_U, BG_V, 0, 0, 0, 0.80f);

    float row_h = ch + pad;
    float tx = mx + pad;
    float ty = my + pad;

    /* Title */
    float title_x = mx + (mw - 8.0f * cw) * 0.5f;
    ov_string("SETTINGS", title_x, ty, cw, ch, 1, 1, 1, 1);
    ty += row_h;

    /* Tab bar — draw all tabs, highlight active one */
    {
        int tab_col_w = cols / TAB_COUNT;
        for (int t = 0; t < TAB_COUNT; t++) {
            float tab_x = tx + t * tab_col_w * cw;
            const char* lbl = tab_labels[t];
            int lbl_len = (int)strlen(lbl);
            float lbl_x = tab_x + ((tab_col_w - lbl_len) / 2) * cw;
            if (t == s_active_tab) {
                ov_string(lbl, lbl_x, ty, cw, ch, 1, 1, 0.3f, 1);
                ov_quad(tab_x, ty + ch, tab_col_w * cw - pad, 2.0f,
                        BG_U, BG_V, BG_U, BG_V, 1, 1, 0.3f, 1);
            } else {
                ov_string(lbl, lbl_x, ty, cw, ch, 0.5f, 0.5f, 0.5f, 1);
            }
        }
        ty += row_h + pad;
    }

    /* Scroll up arrow */
    if (s_active_tab == TAB_CONTROLS && ctrl_scroll > 0)
        ov_string("^", tx + (cols - 1) * cw, ty, cw, ch, 0.6f, 0.6f, 0.6f, 1);

    /* Items for active tab (scrolled window) */
    for (int n = 0; n < vis_count; n++) {
        int item_n = vis_start + n;
        int i = tab_item_at(item_n);
        if (i < 0) break;

        float r, g, b;
        int selected = (item_n == menu_cursor);
        if (selected) { r = 1.0f; g = 1.0f; b = 0.3f; }
        else          { r = 0.75f; g = 0.75f; b = 0.75f; }

        if (selected)
            ov_string(">", tx, ty, cw, ch, r, g, b, 1);

        ov_string(get_item_label(i), tx + 2 * cw, ty, cw, ch, r, g, b, 1);

        char val[32];
        menu_get_value(i, val, sizeof(val));
        float vr = 1, vg = 1, vb = 1;
        if (selected) { vr = 1; vg = 1; vb = 0.5f; }
        ov_string_right(val, tx + cols * cw, ty, cw, ch, vr, vg, vb, 1);

        /* Volume bar */
        {
            int vol_val = -1;
            if (i == MI_MASTER_VOLUME) vol_val = g_pc_settings.master_volume;
            else if (i == MI_BGM_VOLUME)   vol_val = g_pc_settings.bgm_volume;
            else if (i == MI_SFX_VOLUME)   vol_val = g_pc_settings.sfx_volume;
            else if (i == MI_VOICE_VOLUME) vol_val = g_pc_settings.voice_volume;
            if (vol_val >= 0) {
                float bar_x = tx + (val_col - 1) * cw;
                float bar_w = (cols - val_col - 5) * cw;
                float bar_h = ch * 0.3f;
                float bar_y = ty + ch * 0.35f;
                float fill = (float)vol_val / 100.0f;
                ov_quad(bar_x, bar_y, bar_w, bar_h, BG_U, BG_V, BG_U, BG_V, 0.3f, 0.3f, 0.3f, 1);
                if (fill > 0)
                    ov_quad(bar_x, bar_y, bar_w * fill, bar_h, BG_U, BG_V, BG_U, BG_V,
                            selected ? 1.0f : 0.6f,
                            selected ? 1.0f : 0.6f,
                            selected ? 0.3f : 0.6f, 1);
            }
        }

        ty += row_h;
    }

    /* Scroll down arrow — drawn in the gap row, not over items */
    if (s_active_tab == TAB_CONTROLS && ctrl_scroll + CTRL_VISIBLE_ROWS < tab_count)
        ov_string("v", tx + (cols - 1) * cw, ty, cw, ch, 0.6f, 0.6f, 0.6f, 1);

    /* Footer — one full row below items */
    ty += row_h;
    if (ctrl_capture_idx >= 0) {
        ov_string("Any key: bind  Back: cancel", tx, ty, cw, ch, 0.5f, 0.8f, 0.5f, 1);
    } else if (s_active_tab == TAB_CONTROLS) {
        ov_string(">:rebind  X/Del:clear", tx, ty, cw, ch, 0.5f, 0.5f, 0.5f, 1);
    } else {
        ov_string("L/R:Tab  U/D:Nav  L/R:Adj", tx, ty, cw, ch, 0.5f, 0.5f, 0.5f, 1);
    }
    ty += row_h;
    ov_string("Select/Tab:Close+Save", tx, ty, cw, ch, 0.5f, 0.5f, 0.5f, 1);
}

/* ---- Public API ---- */

void pc_overlay_init(void) {
    /* Save GL state that we'll disturb — pc_gx keeps its VAO/VBO bound at all times */
    GLint prev_vao, prev_vbo, prev_tex;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

    GLuint vs = ov_compile_shader(GL_VERTEX_SHADER, ov_vert_src);
    GLuint fs = ov_compile_shader(GL_FRAGMENT_SHADER, ov_frag_src);
    ov_prog = glCreateProgram();
    glAttachShader(ov_prog, vs);
    glAttachShader(ov_prog, fs);
    glLinkProgram(ov_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok;
    glGetProgramiv(ov_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[256];
        glGetProgramInfoLog(ov_prog, sizeof(log), NULL, log);
        fprintf(stderr, "[OVERLAY] link error: %s\n", log);
    }
    ov_loc_font = glGetUniformLocation(ov_prog, "u_font");

    /* Font texture: 128x48 R8, 16x6 grid of 8x8 chars */
    unsigned char pixels[128 * 48];
    memset(pixels, 0, sizeof(pixels));
    for (int i = 0; i < 96; i++) {
        int col = i % 16, row = i / 16;
        for (int y = 0; y < 8; y++) {
            unsigned char bits = font_8x8[i][y];
            for (int x = 0; x < 8; x++)
                if (bits & (0x80 >> x))
                    pixels[(row * 8 + y) * 128 + col * 8 + x] = 0xFF;
        }
    }
    glGenTextures(1, &ov_tex);
    glBindTexture(GL_TEXTURE_2D, ov_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 128, 48, 0,
                 GL_RED, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* VAO/VBO */
    glGenVertexArrays(1, &ov_vao);
    glGenBuffers(1, &ov_vbo);
    glBindVertexArray(ov_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ov_vbo);
    glBufferData(GL_ARRAY_BUFFER, OV_MAX_VERTS * sizeof(OvVtx), NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(OvVtx), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(OvVtx), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(OvVtx), (void*)(4 * sizeof(float)));

    /* Restore previous GL state */
    glBindVertexArray(prev_vao);
    glBindBuffer(GL_ARRAY_BUFFER, prev_vbo);
    glBindTexture(GL_TEXTURE_2D, prev_tex);
}

void pc_overlay_shutdown(void) {
    if (ov_tex)  { glDeleteTextures(1, &ov_tex);       ov_tex = 0; }
    if (ov_vbo)  { glDeleteBuffers(1, &ov_vbo);        ov_vbo = 0; }
    if (ov_vao)  { glDeleteVertexArrays(1, &ov_vao);   ov_vao = 0; }
    if (ov_prog) { glDeleteProgram(ov_prog);            ov_prog = 0; }
}

void pc_overlay_update(double fps, double speed) {
    ov_fps = fps;
    ov_speed = speed;
}

void pc_overlay_menu_toggle(void) {
    g_pc_menu_open ^= 1;
    if (!g_pc_menu_open) {
        /* Save settings and keybindings on close */
        pc_settings_save();
        pc_keybindings_save();
        ctrl_capture_idx  = -1;
        ctrl_clear_start  = 0;
        ctrl_clear_kb_idx = -1;
    }
    /* Reset repeat timers */
    rep_up = rep_down = rep_left = rep_right = 0;
}

void pc_overlay_draw(void) {
    if (!ov_prog) return;

    /* Nothing to draw? */
    int want_fps = g_pc_settings.show_fps && !g_pc_menu_open;
    if (!want_fps && !g_pc_menu_open) return;

    /* Process menu input */
    if (g_pc_menu_open) menu_process_input();

    /* ---- Save GL state ---- */
    GLint sv_prog, sv_vao, sv_vbo, sv_tex, sv_active_tex, sv_vp[4], sv_bsrc, sv_bdst;
    glGetIntegerv(GL_CURRENT_PROGRAM, &sv_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &sv_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &sv_vbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &sv_active_tex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &sv_tex);
    glGetIntegerv(GL_VIEWPORT, sv_vp);
    glGetIntegerv(GL_BLEND_SRC_RGB, &sv_bsrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &sv_bdst);
    GLboolean sv_blend   = glIsEnabled(GL_BLEND);
    GLboolean sv_depth   = glIsEnabled(GL_DEPTH_TEST);
    GLboolean sv_scissor = glIsEnabled(GL_SCISSOR_TEST);

    /* ---- Set overlay GL state ---- */
    glUseProgram(ov_prog);
    glBindVertexArray(ov_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ov_tex);
    glUniform1i(ov_loc_font, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, g_pc_window_w, g_pc_window_h);

    /* ---- Build vertices ---- */
    int scale = g_pc_window_h / 240;
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;
    float cw  = 8.0f * scale;
    float ch  = 8.0f * scale;
    float pad = 4.0f * scale;

    ov_nv = 0;

    if (g_pc_menu_open) {
        draw_menu(cw, ch, pad);
    } else if (want_fps) {
        draw_fps(cw, ch, pad);
    }

    /* ---- Upload & draw ---- */
    if (ov_nv > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, ov_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(ov_nv * sizeof(OvVtx)), ov_verts);
        glDrawArrays(GL_TRIANGLES, 0, ov_nv);
    }

    /* ---- Restore GL state ---- */
    glUseProgram(sv_prog);
    glBindVertexArray(sv_vao);
    glBindBuffer(GL_ARRAY_BUFFER, sv_vbo);
    glActiveTexture(sv_active_tex);
    glBindTexture(GL_TEXTURE_2D, sv_tex);
    glViewport(sv_vp[0], sv_vp[1], sv_vp[2], sv_vp[3]);
    glBlendFunc(sv_bsrc, sv_bdst);
    if (!sv_blend)   glDisable(GL_BLEND);
    if (sv_depth)    glEnable(GL_DEPTH_TEST);
    if (sv_scissor)  glEnable(GL_SCISSOR_TEST);
}
