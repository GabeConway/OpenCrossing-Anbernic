/* pc_platform.h - SDL2/OpenGL platform layer, global state, crash protection */
#ifndef PC_PLATFORM_H
#define PC_PLATFORM_H

/* 32-bit required: decomp code (JSystem, emu64) casts pointers to u32 */
#include <stdint.h>
#if UINTPTR_MAX != 0xFFFFFFFFu
#error "This project must be compiled as 32-bit (pointer size != 4 bytes)"
#endif

#define SDL_MAIN_HANDLED
#include <SDL.h>
#ifdef PC_USE_GLES
#include <glad/gles2.h>
#else
#include <glad/gl.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>

#include "pc_types.h"

/* --- Configuration --- */
#define PC_GC_WIDTH       640
#define PC_GC_HEIGHT      480
#define PC_SCREEN_WIDTH   PC_GC_WIDTH
#define PC_SCREEN_HEIGHT  PC_GC_HEIGHT
#define PC_WINDOW_TITLE   "Animal Crossing"

#define PC_MAIN_MEMORY_SIZE   (24 * 1024 * 1024)
#define PC_ARAM_SIZE          (16 * 1024 * 1024)
#define PC_FIFO_SIZE          (256 * 1024)

#define PC_PI  3.14159265358979323846
#define PC_PIf 3.14159265358979323846f
#define PC_DEG_TO_RAD (PC_PI / 180.0)
#define PC_DEG_TO_RADf (PC_PIf / 180.0f)

/* GC hardware clocks */
#define GC_BUS_CLOCK          162000000u
#define GC_CORE_CLOCK         486000000u
#define GC_TIMER_CLOCK        (GC_BUS_CLOCK / 4)

/* --- Platform headers --- */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef near
#undef far
#else
/* Rename POSIX link() before including signal.h, which pulls in unistd.h.
 * unistd.h declares "extern int link(const char*, const char*)" which conflicts
 * with jaudio_NES audiostruct.h's "typedef struct link_ link".
 * We don't call link() anywhere, so silently redirecting it is safe. */
#define link _pc_posix_link_fn
#include <signal.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <elf.h>
#undef link
#endif
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Global state --- */
extern SDL_Window*   g_pc_window;
extern SDL_GLContext  g_pc_gl_context;
extern int           g_pc_running;
extern int           g_pc_verbose;
extern int           g_pc_no_framelimit;
extern int           g_pc_time_override;
extern int           g_pc_min_override;
extern int           g_pc_sec_override;

extern int g_pc_window_w;
extern int g_pc_window_h;
void pc_platform_update_window_size(void);

/* --- Widescreen mode (3-state) ---
 * 0 = hor+ (default): full-window viewport, FOV-corrected projection. Resets each frame.
 * 1 = stretch: full-window, no correction. For transitions/inventory BG.
 * 2 = pillarbox: centered 4:3 with black bars. For inventory UI alignment.
 *
 * m_play.c inserts NOOPTag markers in POLY_OPA to switch between states:
 *   ON (0xAC5701) -> 1, OFF (0xAC5700) -> 2. emu64 reads these during DL processing. */
#define PC_NOOP_WIDESCREEN_STRETCH     0xAC5701u
#define PC_NOOP_WIDESCREEN_STRETCH_OFF 0xAC5700u
extern int g_pc_widescreen_stretch;

/* --- Frameskip / tick batching --- */
extern int g_pc_frameskip_active;  /* 1 = this is a logic-only tick (no GL draws, no pacing) */

/* --- FPS target & render resolution --- */
extern int g_pc_fps_target;   /* target FPS: 60, 50, 40, 30, or 0=unlimited; driven by fps_target setting */
extern int g_pc_render_w;     /* render width  = window_w * render_scale/100 */
extern int g_pc_render_h;     /* render height = window_h * render_scale/100 */
extern int g_pc_scale_mode;   /* 0=stretch, 1=center */

/* --- Camera zoom --- */
extern float g_pc_zoom;  /* 1.0 = default, >1.0 = zoomed out, <1.0 = zoomed in */
#define PC_ZOOM_MIN  0.5f
#define PC_ZOOM_MAX  2.0f
#define PC_ZOOM_STEP 0.02f

/* --- Settings menu --- */
extern int g_pc_menu_open;  /* 1 = settings menu is open, suppress game input */
SDL_GameController* pc_pad_get_controller(void);

/* --- Functions --- */
void pc_gx_blit_to_screen(void);
void pc_platform_init(void);
void pc_platform_shutdown(void);
void pc_platform_swap_buffers(void);
int  pc_platform_poll_events(void);

/* --- Crash protection (VEH + setjmp/longjmp) --- */
void pc_crash_protection_init(void);
void pc_crash_set_jmpbuf(jmp_buf* buf);  /* NULL to disable */
jmp_buf* pc_crash_get_jmpbuf(void);
unsigned int pc_crash_get_addr(void);
unsigned int pc_crash_get_data_addr(void);

/* EXE image range for seg2k0 pointer disambiguation (vs N64 segment addresses) */
extern unsigned int pc_image_base;
extern unsigned int pc_image_end;

/* --- Model viewer --- */
extern int g_pc_model_viewer;
extern int g_pc_model_viewer_start;
extern int g_pc_model_viewer_no_cull;

/* --- Per-frame diagnostics --- */
extern int pc_emu64_frame_cmds;
extern int pc_emu64_frame_crashes;
extern int pc_emu64_frame_noop_cmds;
extern int pc_emu64_frame_tri_cmds;
extern int pc_emu64_frame_vtx_cmds;
extern int pc_emu64_frame_dl_cmds;
extern int pc_emu64_frame_cull_visible;
extern int pc_emu64_frame_cull_rejected;
extern int pc_gx_draw_call_count;
extern int pc_gx_prim_draws[5];    /* draws by primitive: quads, tris, strips, fans, other */
extern int pc_gx_merged_batches;   /* GXBegin calls absorbed into an open batch */
extern int pc_gx_culled_draws;     /* batches skipped by whole-batch CPU frustum cull */
extern int pc_gx_flush_reason[18]; /* 0-15 = PC_GX_DIRTY_* bit that broke the batch,
                                      16 = GXBegin (prim/fmt change), 17 = viewport/scissor */

/* --- Audio --- */
extern int pc_save_loaded;
int  pc_audio_get_buffer_fill(void);
int  pc_audio_is_active(void);
void pc_audio_shutdown(void);
void pc_audio_start_producer_thread(void);
void pc_audio_mq_init(void);
void pc_audio_mq_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_PLATFORM_H */
