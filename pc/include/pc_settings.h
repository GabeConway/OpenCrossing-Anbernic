#ifndef PC_SETTINGS_H
#define PC_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int window_width;
    int window_height;
    int fullscreen;       /* 0=windowed, 1=fullscreen, 2=borderless */
    int vsync;            /* 0=off, 1=on */
    int msaa;             /* 0=off, 2/4/8=samples */
    int preload_textures; /* 0=off (load on demand), 1=on (load all at startup), 2=on + cache file */
    int frameskip;        /* legacy: mapped to fps_target on load */
    int verbose;          /* 0=off, 1=on */
    int show_fps;         /* 0=off, 1=on */
    int master_volume;    /* 0-100 */
    int bgm_volume;       /* 0-100 */
    int sfx_volume;       /* 0-100 */
    int voice_volume;     /* 0-100 */
    int zoom_enabled;     /* 0=off, 1=on */
    int fps_target;       /* 0=60fps, 1=50fps, 2=40fps, 3=30fps, 4=20fps, 5=unlimited, 6=dynamic */
    int render_scale;     /* render resolution %: 100, 75, 50, 25 */
    int window_size;      /* 0=320x240, 1=480x360, 2=640x480, 3=960x720, 4=1280x960, 5=custom */
    int scale_mode;       /* 0=stretch, 1=center (letterbox) */
    int dpad_as_stick;    /* 0=off, 1=on — dpad directions also drive main stick */
    int left_deadzone;    /* left stick deadzone 0-50 (percent of axis range) */
    int right_deadzone;   /* right stick deadzone 0-50 (percent of axis range) */
    int swap_ab_xy;       /* 0=off, 1=on — swap A↔B and X↔Y on the hardcoded gamepad path */
    /* Performance (ARM64 low-spec) — all hot-toggled at runtime, no restart needed */
    int frustum_cull;              /* 0=off, 1=on — PC uses horizontal distance from player, not GPU frustum */
    int frustum_cull_z_margin;     /* extra world units added to each actor's draw range; higher = less culling */
    int frustum_cull_max_distance; /* 0=no cap, else max XZ draw distance (world units); lower = more culling */
    int shadow_quality;         /* 0=all actors+world decals, 1=player only, 2=off (no shadows), 3=player+NPC */
    int reduce_acre_draw;       /* 0=full (up to 4 adjacent), 1=cross (orthogonal only), 2=current acre only */
    int particle_quality;       /* 0=off, 1=25%, 2=50%, 3=75%, 4=full */
} PCSettings;

extern PCSettings g_pc_settings;

/* Effective XZ cull radius when frustum_cull is on: min(cull_distance+cull_radius+margin, max) or uncapped max */
float pc_settings_cull_limit_xz(float cull_distance, float cull_radius);

void pc_settings_load(void);
void pc_settings_save(void);
void pc_settings_apply(void);
void pc_settings_reset_controllers(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_SETTINGS_H */
