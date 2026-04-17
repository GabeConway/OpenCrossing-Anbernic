/* pc_settings.c - runtime settings loaded from settings.ini */
#include "pc_settings.h"
#include "pc_platform.h"

float pc_settings_cull_limit_xz(float cull_distance, float cull_radius) {
    float t = cull_distance + cull_radius + (float)g_pc_settings.frustum_cull_z_margin;
    int cap = g_pc_settings.frustum_cull_max_distance;

    if (cap > 0 && t > (float)cap) {
        t = (float)cap;
    }
    return t;
}

PCSettings g_pc_settings = {
    .window_width  = PC_SCREEN_WIDTH,
    .window_height = PC_SCREEN_HEIGHT,
    .fullscreen    = 0,
    .vsync         = 0,
    .msaa          = 4,
    .preload_textures = 0,
    .frameskip = 0,
    .verbose = 0,
    .show_fps = 0,
    .master_volume = 100,
    .bgm_volume = 100,
    .sfx_volume = 100,
    .voice_volume = 100,
    .zoom_enabled = 1,
    .fps_target    = 0,    /* 60fps default */
    .render_scale  = 100,
    .window_size   = 2,    /* 640x480 default */
    .scale_mode    = 0,
    .dpad_as_stick  = 0,
    .left_deadzone  = 0,
    .right_deadzone = 0,
    .swap_ab_xy     = 0,
    .frustum_cull              = 0,
    .frustum_cull_z_margin     = 50,
    .frustum_cull_max_distance = 0,
    .shadow_quality         = 0,
    .reduce_acre_draw       = 0,
    .particle_quality       = 4,
    .disable_resetti        = 0,
    .nes_aspect             = 1,
};

static const char* SETTINGS_FILE = "settings.ini";

static const char* DEFAULT_SETTINGS =
    "[Graphics]\n"
    "# Window size (ignored in fullscreen)\n"
    "window_width = 640\n"
    "window_height = 480\n"
    "\n"
    "# 0 = windowed, 1 = fullscreen, 2 = borderless fullscreen\n"
    "fullscreen = 0\n"
    "\n"
    "# Vertical sync: 0 = off, 1 = on\n"
    "vsync = 0\n"
    "\n"
    "# Anti-aliasing samples: 0 = off, 2, 4, or 8\n"
    "msaa = 4\n"
    "\n"
    "[Enhancements]\n"
    "# Preload HD textures at startup: 0 = off (load on demand), 1 = preload, 2 = preload + cache file (fastest)\n"
    "preload_textures = 0\n"
    "\n"
    "[Performance]\n"
    "# FPS target: 0=60fps, 1=50fps, 2=40fps, 3=30fps, 4=20fps, 5=unlimited, 6=dynamic\n"
    "fps_target = 0\n"
    "\n"
    "# Render scale %%: 100=native, 75, 50, 25 (lower = faster on limited hardware)\n"
    "render_scale = 100\n"
    "\n"
    "# Window size preset: 0=320x240, 1=480x360, 2=640x480, 3=960x720, 4=1280x960, 5=custom\n"
    "window_size = 2\n"
    "\n"
    "# Scale mode: 0=stretch to fill screen, 1=center (letterbox)\n"
    "scale_mode = 0\n"
    "\n"
    "[Audio]\n"
    "# Volume levels: 0-100\n"
    "master_volume = 100\n"
    "bgm_volume = 100\n"
    "sfx_volume = 100\n"
    "voice_volume = 100\n"
    "\n"
    "[Gameplay]\n"
    "# Show FPS counter: 0 = off, 1 = on\n"
    "show_fps = 0\n"
    "\n"
    "# Camera zoom (L + D-pad): 0 = off, 1 = on\n"
    "zoom_enabled = 1\n"
    "\n"
    "[Debug]\n"
    "# Verbose logging: 0 = off, 1 = on (log to console)\n"
    "verbose = 1\n"
    "\n"
    "[Controls]\n"
    "# D-pad also drives main analog stick: 0 = off, 1 = on\n"
    "dpad_as_stick = 0\n"
    "\n"
    "# Left/right stick deadzone in percent (0-50, increments of 5)\n"
    "left_deadzone = 0\n"
    "right_deadzone = 0\n"
    "\n"
    "# Swap A↔B and X↔Y: 0 = off, 1 = on\n"
    "swap_ab_xy = 0\n"
    "\n"
    "[LowSpec]\n"
    "# Distance cull (props/actors vs player XZ): 0=off, 1=on (not GPU frustum)\n"
    "frustum_cull = 0\n"
    "\n"
    "# Extra draw range in world units (0-200). Higher = draw farther = less culling.\n"
    "frustum_cull_z_margin = 50\n"
    "\n"
    "# Hard max XZ draw distance (0 = no cap, use per-object range + margin only).\n"
    "# Set e.g. 500-800 to see distant trees/props disappear (lower = more aggressive).\n"
    "frustum_cull_max_distance = 0\n"
    "\n"
    "# Shadow quality: 0=all, 1=player only, 2=off (actors + trees/sign decals), 3=player+NPC\n"
    "shadow_quality = 0\n"
    "\n"
    "# Acre background draw: 0=full (adjacent), 1=cross (orthogonal only), 2=current acre only\n"
    "reduce_acre_draw = 0\n"
    "\n"
    "# Weather effect quality: 0=off, 1=25%, 2=50%, 3=75%, 4=full\n"
    "particle_quality = 4\n";

static const char* skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void trim_end(char* s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                       s[len-1] == '\r' || s[len-1] == '\n')) {
        s[--len] = '\0';
    }
}

static void apply_setting(const char* key, const char* value) {
    int val = atoi(value);

    if (strcmp(key, "window_width") == 0) {
        if (val >= 320) g_pc_settings.window_width = val;
    } else if (strcmp(key, "window_height") == 0) {
        if (val >= 240) g_pc_settings.window_height = val;
    } else if (strcmp(key, "fullscreen") == 0) {
        if (val >= 0 && val <= 2) g_pc_settings.fullscreen = val;
    } else if (strcmp(key, "vsync") == 0) {
        if (val == 0 || val == 1) g_pc_settings.vsync = val;
    } else if (strcmp(key, "msaa") == 0) {
        if (val == 0 || val == 2 || val == 4 || val == 8)
            g_pc_settings.msaa = val;
    } else if (strcmp(key, "preload_textures") == 0) {
        if (val >= 0 && val <= 2) g_pc_settings.preload_textures = val;
    } else if (strcmp(key, "frameskip") == 0) {
        /* Legacy: map old frameskip values to fps_target */
        if (val >= 1 && g_pc_settings.fps_target == 0)
            g_pc_settings.fps_target = 3; /* 30fps */
        g_pc_settings.frameskip = val;
    } else if (strcmp(key, "fps_target") == 0) {
        if (val >= 0 && val <= 6) g_pc_settings.fps_target = val;
    } else if (strcmp(key, "render_scale") == 0) {
        if (val == 25 || val == 50 || val == 75 || val == 100)
            g_pc_settings.render_scale = val;
    } else if (strcmp(key, "window_size") == 0) {
        if (val >= 0 && val <= 5) g_pc_settings.window_size = val;
    } else if (strcmp(key, "scale_mode") == 0) {
        if (val == 0 || val == 1) g_pc_settings.scale_mode = val;
    } else if (strcmp(key, "verbose") == 0) {
        if (val == 0 || val == 1) g_pc_settings.verbose = val;
    } else if (strcmp(key, "show_fps") == 0) {
        if (val == 0 || val == 1) g_pc_settings.show_fps = val;
    } else if (strcmp(key, "master_volume") == 0) {
        if (val >= 0 && val <= 100) g_pc_settings.master_volume = val;
    } else if (strcmp(key, "bgm_volume") == 0) {
        if (val >= 0 && val <= 100) g_pc_settings.bgm_volume = val;
    } else if (strcmp(key, "sfx_volume") == 0) {
        if (val >= 0 && val <= 100) g_pc_settings.sfx_volume = val;
    } else if (strcmp(key, "voice_volume") == 0) {
        if (val >= 0 && val <= 100) g_pc_settings.voice_volume = val;
    } else if (strcmp(key, "zoom_enabled") == 0) {
        if (val == 0 || val == 1) g_pc_settings.zoom_enabled = val;
    } else if (strcmp(key, "dpad_as_stick") == 0) {
        if (val == 0 || val == 1) g_pc_settings.dpad_as_stick = val;
    } else if (strcmp(key, "left_deadzone") == 0) {
        if (val >= 0 && val <= 50) g_pc_settings.left_deadzone = val;
    } else if (strcmp(key, "right_deadzone") == 0) {
        if (val >= 0 && val <= 50) g_pc_settings.right_deadzone = val;
    } else if (strcmp(key, "swap_ab_xy") == 0) {
        if (val == 0 || val == 1) g_pc_settings.swap_ab_xy = val;
    } else if (strcmp(key, "frustum_cull") == 0) {
        if (val == 0 || val == 1) g_pc_settings.frustum_cull = val;
    } else if (strcmp(key, "frustum_cull_z_margin") == 0) {
        if (val >= 0 && val <= 200) g_pc_settings.frustum_cull_z_margin = val;
    } else if (strcmp(key, "frustum_cull_max_distance") == 0) {
        if (val >= 0 && val <= 2500) g_pc_settings.frustum_cull_max_distance = val;
    } else if (strcmp(key, "frustum_cull_x_margin") == 0) {
        /* Legacy ini key (was unused in-game). Ignored. */
        (void)val;
    } else if (strcmp(key, "shadow_quality") == 0) {
        if (val >= 0 && val <= 3) g_pc_settings.shadow_quality = val;
    } else if (strcmp(key, "reduce_acre_draw") == 0) {
        if (val >= 0 && val <= 2) g_pc_settings.reduce_acre_draw = val;
    } else if (strcmp(key, "particle_quality") == 0) {
        if (val >= 0 && val <= 4) g_pc_settings.particle_quality = val;
    } else if (strcmp(key, "disable_resetti") == 0) {
        if (val == 0 || val == 1) g_pc_settings.disable_resetti = val;
    } else if (strcmp(key, "nes_aspect") == 0) {
        if (val == 0 || val == 1) g_pc_settings.nes_aspect = val;
    }
}

static void write_defaults(const char* path) {
    FILE* f = fopen(path, "w");
    if (f) {
        fputs(DEFAULT_SETTINGS, f);
        fclose(f);
    }
}

void pc_settings_save(void) {
    FILE* f = fopen(SETTINGS_FILE, "w");
    if (!f) {
        printf("[Settings] Failed to write %s\n", SETTINGS_FILE);
        return;
    }
    fprintf(f, "[Graphics]\n");
    fprintf(f, "# Window size (ignored in fullscreen)\n");
    fprintf(f, "window_width = %d\n", g_pc_settings.window_width);
    fprintf(f, "window_height = %d\n", g_pc_settings.window_height);
    fprintf(f, "\n");
    fprintf(f, "# 0 = windowed, 1 = fullscreen, 2 = borderless fullscreen\n");
    fprintf(f, "fullscreen = %d\n", g_pc_settings.fullscreen);
    fprintf(f, "\n");
    fprintf(f, "# Vertical sync: 0 = off, 1 = on\n");
    fprintf(f, "vsync = %d\n", g_pc_settings.vsync);
    fprintf(f, "\n");
    fprintf(f, "# Anti-aliasing samples: 0 = off, 2, 4, or 8\n");
    fprintf(f, "msaa = %d\n", g_pc_settings.msaa);
    fprintf(f, "\n");
    fprintf(f, "[Enhancements]\n");
    fprintf(f, "# Preload HD textures at startup: 0 = off (load on demand), 1 = preload, 2 = preload + cache file (fastest)\n");
    fprintf(f, "preload_textures = %d\n", g_pc_settings.preload_textures);
    fprintf(f, "\n");
    fprintf(f, "[Performance]\n");
    fprintf(f, "# FPS target: 0=60fps, 1=50fps, 2=40fps, 3=30fps, 4=20fps, 5=unlimited, 6=dynamic\n");
    fprintf(f, "fps_target = %d\n", g_pc_settings.fps_target);
    fprintf(f, "\n");
    fprintf(f, "# Render scale %%: 100=native, 75, 50, 25\n");
    fprintf(f, "render_scale = %d\n", g_pc_settings.render_scale);
    fprintf(f, "\n");
    fprintf(f, "# Window size preset: 0=320x240, 1=480x360, 2=640x480, 3=960x720, 4=1280x960, 5=custom\n");
    fprintf(f, "window_size = %d\n", g_pc_settings.window_size);
    fprintf(f, "\n");
    fprintf(f, "# Scale mode: 0=stretch to fill screen, 1=center (letterbox)\n");
    fprintf(f, "scale_mode = %d\n", g_pc_settings.scale_mode);
    fprintf(f, "\n");
    fprintf(f, "[Audio]\n");
    fprintf(f, "master_volume = %d\n", g_pc_settings.master_volume);
    fprintf(f, "bgm_volume = %d\n", g_pc_settings.bgm_volume);
    fprintf(f, "sfx_volume = %d\n", g_pc_settings.sfx_volume);
    fprintf(f, "voice_volume = %d\n", g_pc_settings.voice_volume);
    fprintf(f, "\n");
    fprintf(f, "[Gameplay]\n");
    fprintf(f, "show_fps = %d\n", g_pc_settings.show_fps);
    fprintf(f, "zoom_enabled = %d\n", g_pc_settings.zoom_enabled);
    fprintf(f, "\n");
    fprintf(f, "[Debug]\n");
    fprintf(f, "verbose = %d\n", g_pc_settings.verbose);
    fprintf(f, "\n");
    fprintf(f, "[Controls]\n");
    fprintf(f, "# D-pad also drives main analog stick: 0 = off, 1 = on\n");
    fprintf(f, "dpad_as_stick = %d\n", g_pc_settings.dpad_as_stick);
    fprintf(f, "\n");
    fprintf(f, "# Left/right stick deadzone in percent (0-50)\n");
    fprintf(f, "left_deadzone = %d\n", g_pc_settings.left_deadzone);
    fprintf(f, "right_deadzone = %d\n", g_pc_settings.right_deadzone);
    fprintf(f, "\n");
    fprintf(f, "# Swap A↔B and X↔Y: 0 = off, 1 = on\n");
    fprintf(f, "swap_ab_xy = %d\n", g_pc_settings.swap_ab_xy);
    fprintf(f, "[LowSpec]\n");
    fprintf(f, "# Distance cull vs player XZ: 0=off, 1=on\n");
    fprintf(f, "frustum_cull = %d\n", g_pc_settings.frustum_cull);
    fprintf(f, "\n");
    fprintf(f, "# Extra draw range (world units, 0-200). Higher = less culling.\n");
    fprintf(f, "frustum_cull_z_margin = %d\n", g_pc_settings.frustum_cull_z_margin);
    fprintf(f, "\n");
    fprintf(f, "# Max XZ draw distance (0=no cap). Lower = more culling (try 500-900 to verify).\n");
    fprintf(f, "frustum_cull_max_distance = %d\n", g_pc_settings.frustum_cull_max_distance);
    fprintf(f, "\n");
    fprintf(f, "# Shadow quality: 0=all, 1=player only, 2=off, 3=player+NPC\n");
    fprintf(f, "shadow_quality = %d\n", g_pc_settings.shadow_quality);
    fprintf(f, "\n");
    fprintf(f, "# Acre background draw: 0=full (adjacent), 1=cross (orthogonal only), 2=current acre only\n");
    fprintf(f, "reduce_acre_draw = %d\n", g_pc_settings.reduce_acre_draw);
    fprintf(f, "\n");
    fprintf(f, "# Weather effect quality: 0=off, 1=25%%, 2=50%%, 3=75%%, 4=full\n");
    fprintf(f, "particle_quality = %d\n", g_pc_settings.particle_quality);
    fprintf(f, "\n");
    fprintf(f, "[Gameplay]\n");
    fprintf(f, "# Disable Mr. Resetti: 0 = normal, 1 = disable\n");
    fprintf(f, "disable_resetti = %d\n", g_pc_settings.disable_resetti);
    fprintf(f, "\n");
    fprintf(f, "# NES emulator aspect ratio: 0 = stretch to fullscreen, 1 = 4:3 pillarbox\n");
    fprintf(f, "nes_aspect = %d\n", g_pc_settings.nes_aspect);
    fclose(f);
    printf("[Settings] Saved %s\n", SETTINGS_FILE);
}

int pc_settings_get_nes_aspect(void) {
    return g_pc_settings.nes_aspect;
}

void pc_settings_reset_controllers(void) {
    g_pc_settings.dpad_as_stick = 0;
    g_pc_settings.left_deadzone  = 0;
    g_pc_settings.right_deadzone = 0;
    g_pc_settings.swap_ab_xy    = 0;
    pc_settings_save();
}

/* Window size preset table: {width, height} */
static const int s_window_presets[5][2] = {
    {320, 240},
    {480, 360},
    {640, 480},
    {960, 720},
    {1280, 960},
};

/* FPS target enum -> actual Hz */
static const int s_fps_target_hz[7] = {60, 50, 40, 30, 20, 0, 60}; /* 6=dynamic starts at 60 */

void pc_settings_apply(void) {
    if (!g_pc_window) return;

    /* Resolve window size from preset (unless custom) */
    if (g_pc_settings.window_size >= 0 && g_pc_settings.window_size < 5) {
        g_pc_settings.window_width  = s_window_presets[g_pc_settings.window_size][0];
        g_pc_settings.window_height = s_window_presets[g_pc_settings.window_size][1];
    }

    if (g_pc_settings.fullscreen == 1 || g_pc_settings.fullscreen == 2) {
        SDL_SetWindowFullscreen(g_pc_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(g_pc_window, 0);
        SDL_SetWindowSize(g_pc_window, g_pc_settings.window_width, g_pc_settings.window_height);
        SDL_SetWindowPosition(g_pc_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }

    SDL_GL_SetSwapInterval(g_pc_settings.vsync);

    /* Apply fps_target to the global used by the frame pacing system */
    int ti = g_pc_settings.fps_target;
    if (ti < 0 || ti > 6) ti = 0;
    g_pc_fps_target = s_fps_target_hz[ti];

    pc_platform_update_window_size(); /* also updates g_pc_render_w/h */

    printf("[Settings] Applied: %dx%d (render %dx%d) fullscreen=%d vsync=%d msaa=%d fps_target=%d\n",
           g_pc_settings.window_width, g_pc_settings.window_height,
           g_pc_render_w, g_pc_render_h,
           g_pc_settings.fullscreen, g_pc_settings.vsync, g_pc_settings.msaa,
           g_pc_fps_target);
}

void pc_settings_load(void) {
    FILE* f = fopen(SETTINGS_FILE, "r");
    if (!f) {
        write_defaults(SETTINGS_FILE);
        printf("[Settings] Created default %s\n", SETTINGS_FILE);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        const char* p = skip_ws(line);

        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n' || *p == '[')
            continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = (char*)skip_ws(line);
        trim_end(key);
        char* value = (char*)skip_ws(eq + 1);
        trim_end(value);

        if (*key && *value) {
            apply_setting(key, value);
        }
    }
    fclose(f);
    /* Apply fps_target to global immediately (window not open yet at load time,
     * but g_pc_fps_target must be set before the game loop starts) */
    {
        int ti = g_pc_settings.fps_target;
        if (ti < 0 || ti > 6) ti = 0;
        extern int g_pc_fps_target;
        const int fps_hz[7] = {60, 50, 40, 30, 20, 0, 60};
        g_pc_fps_target = fps_hz[ti];
    }
    printf("[Settings] Loaded %s: %dx%d fullscreen=%d vsync=%d msaa=%d preload=%d fps_target=%d render_scale=%d\n",
           SETTINGS_FILE, g_pc_settings.window_width, g_pc_settings.window_height,
           g_pc_settings.fullscreen, g_pc_settings.vsync, g_pc_settings.msaa,
           g_pc_settings.preload_textures, g_pc_settings.fps_target,
           g_pc_settings.render_scale);
}
