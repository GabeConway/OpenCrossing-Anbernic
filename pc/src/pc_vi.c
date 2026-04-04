/* pc_vi.c - video interface → SDL window swap + frame pacing */
#include "pc_platform.h"
#include "pc_settings.h"
#include "pc_overlay.h"

/* GL timing and frame reset from pc_gx.c */
extern void pc_gx_frame_timing_snapshot(void);
extern void pc_gx_begin_frame(void);
extern void pc_gx_blit_to_screen(void);
extern Uint64 pc_gx_flush_time_us;
extern Uint64 pc_gx_texload_time_us;

#define VI_TVMODE_NTSC_INT    0
#define VI_TVMODE_NTSC_DS     1
#define VI_TVMODE_PAL_INT     4
#define VI_TVMODE_MPAL_INT    8
#define VI_TVMODE_EURGB60_INT 20

static u32 retrace_count = 0;
u32 pc_frame_counter = 0;
static Uint64 frame_start_time = 0;
static Uint64 perf_freq = 0;
static void (*vi_pre_callback)(u32) = NULL;
static void (*vi_post_callback)(u32) = NULL;

/* Total logic ticks since last FPS update (includes skipped render ticks) */
static int s_logic_tick_count = 0;

/* --- Dynamic FPS controller state --- */
static double s_dyn_ema_us = 0.0;
static int    s_dyn_inited = 0;

void VIInit(void) { }

void VIConfigure(void* rm) { (void)rm; }

void VISetNextFrameBuffer(void* fb) { (void)fb; }

void VIFlush(void) {}

void pc_dynamic_fps_reset(void) {
    s_dyn_ema_us = 0.0;
    s_dyn_inited = 0;
}

static void pc_dynamic_fps_update(Uint64 work_us) {
    if (g_pc_settings.fps_target != 6) return;

    /* EMA alpha=0.25: absorbs single-frame spikes, reacts to sustained load in ~4 frames */
    if (!s_dyn_inited) {
        s_dyn_ema_us = (double)work_us;
        s_dyn_inited = 1;
    } else {
        s_dyn_ema_us = 0.25 * (double)work_us + 0.75 * s_dyn_ema_us;
    }

    /* fps = 1e6 / work_us converges to optimal fps for 100% speed.
     * Clamp: max 60fps, min 10fps. */
    double fps_opt = 1000000.0 / s_dyn_ema_us;
    if (fps_opt > 60.0) fps_opt = 60.0;
    if (fps_opt < 10.0) fps_opt = 10.0;
    g_pc_fps_target = (int)(fps_opt + 0.5);
}

void VIWaitForRetrace(void) {
    if (!perf_freq) perf_freq = SDL_GetPerformanceFrequency();

    /* Always pump events — even on logic-only ticks — to avoid OS hangs */
    if (!pc_platform_poll_events()) {
        g_pc_running = 0;
        return;
    }

    /* Count every tick (logic-only and render) for game speed calculation */
    s_logic_tick_count++;

    /* --- Logic-only tick fast path ---
     * No swap, no pacing, no overlay. frame_start_time is intentionally NOT
     * reset here so that the render tick at the end of the batch measures the
     * full batch duration for pacing. */
    if (g_pc_frameskip_active) {
        pc_gx_begin_frame();
        retrace_count++;
        pc_frame_counter++;
        return;
    }

    /* --- Render tick: measure frame time --- */
    Uint64 vi_enter = SDL_GetPerformanceCounter();
    double frame_ms = 0.0;
    if (frame_start_time) {
        frame_ms = (double)(vi_enter - frame_start_time) * 1000.0 / (double)perf_freq;
    }

    /* Overlay + buffer swap */
    Uint64 t_before_swap = SDL_GetPerformanceCounter();
    pc_gx_blit_to_screen();
    pc_overlay_draw();
    pc_platform_swap_buffers();
    Uint64 t_after_swap = SDL_GetPerformanceCounter();

    /* Dynamic FPS: compute optimal fps from actual work cost before the pacing wait */
    if (g_pc_settings.fps_target == 6 && frame_start_time) {
        Uint64 work_us = (t_after_swap - frame_start_time) * 1000000 / perf_freq;
        pc_dynamic_fps_update(work_us);
    }

    /* Frame pacing: wait until the target visual-frame period has elapsed since
     * frame_start_time (which was set at the start of this batch, not this tick). */
    Uint64 t_before_pace = SDL_GetPerformanceCounter();
    if (!g_pc_no_framelimit && g_pc_fps_target > 0) {
        Uint64 target_us = 1000000 / (Uint64)g_pc_fps_target;
        if (frame_start_time) {
            Uint64 now = SDL_GetPerformanceCounter();
            Uint64 elapsed_us = (now - frame_start_time) * 1000000 / perf_freq;
            while (elapsed_us < target_us) {
                Uint64 remain_us = target_us - elapsed_us;
                if (remain_us > 2000) {
                    SDL_Delay(1);
                }
                now = SDL_GetPerformanceCounter();
                elapsed_us = (now - frame_start_time) * 1000000 / perf_freq;
            }
        }
    }
    Uint64 t_after_pace = SDL_GetPerformanceCounter();

    /* Snapshot GL timing before reporting */
    pc_gx_frame_timing_snapshot();

    /* Adaptive stutter detection: only log frames significantly above average */
    {
        static double avg_frame_ms = 16.67;
        if (frame_ms > 0.0)
            avg_frame_ms = avg_frame_ms * 0.95 + frame_ms * 0.05;
        double stutter_thresh = avg_frame_ms * 1.5;
        if (stutter_thresh < 20.0) stutter_thresh = 20.0;
        if (frame_ms > stutter_thresh && g_pc_verbose) {
            double swap_ms = (double)(t_after_swap - t_before_swap) * 1000.0 / (double)perf_freq;
            double pace_ms = (double)(t_after_pace - t_before_pace) * 1000.0 / (double)perf_freq;
            double work_ms = (double)(vi_enter - frame_start_time) * 1000.0 / (double)perf_freq;
            double flush_ms = (double)pc_gx_flush_time_us / 1000.0;
            double texld_ms = (double)pc_gx_texload_time_us / 1000.0;
            printf("[STUTTER] frame %u: total=%.1fms (avg=%.1fms) work=%.1fms swap=%.1fms pace=%.1fms gl=%.1fms tex=%.1fms draws=%d\n",
                   pc_frame_counter, frame_ms, avg_frame_ms, work_ms, swap_ms, pace_ms,
                   flush_ms, texld_ms, pc_gx_draw_call_count);
        }
    }

    /* FPS counter: report render FPS and true game speed every 60 render frames */
    {
        static Uint64 fps_start = 0;
        static int fps_count = 0;
        static int logic_count_snap = 0;
        if (fps_start == 0) { fps_start = SDL_GetPerformanceCounter(); logic_count_snap = s_logic_tick_count; }
        fps_count++;
        if (fps_count >= 60) {
            Uint64 now = SDL_GetPerformanceCounter();
            double secs = (double)(now - fps_start) / (double)perf_freq;
            double render_fps = (double)fps_count / secs;
            int logic_ticks = s_logic_tick_count - logic_count_snap;
            double logic_tps = (double)logic_ticks / secs;
            double speed_pct = logic_tps / 60.0 * 100.0;

            char title[128];
            snprintf(title, sizeof(title), "Animal Crossing - %.1f FPS (%.0f%% Speed, %d draws)",
                     render_fps, speed_pct, pc_gx_draw_call_count);
            SDL_SetWindowTitle(g_pc_window, title);
            pc_overlay_update(render_fps, speed_pct);

            if (g_pc_verbose) {
                extern int pc_emu64_frame_cmds, pc_emu64_frame_crashes;
                double flush_ms = (double)pc_gx_flush_time_us / 1000.0;
                double texld_ms = (double)pc_gx_texload_time_us / 1000.0;
                printf("[PERF] %.1f FPS | %.0f%% speed | draws=%d cmds=%d crashes=%d gl=%.1fms tex=%.1fms\n",
                       render_fps, speed_pct, pc_gx_draw_call_count, pc_emu64_frame_cmds,
                       pc_emu64_frame_crashes, flush_ms, texld_ms);
            }

            fps_start = now;
            fps_count = 0;
            logic_count_snap = s_logic_tick_count;
        }
    }

    /* Reset batch start time for next visual frame */
    frame_start_time = SDL_GetPerformanceCounter();

    pc_gx_begin_frame();
    retrace_count++;
    pc_frame_counter++;
}

u32 VIGetRetraceCount(void) { return retrace_count; }

void VISetBlack(BOOL black) { (void)black; }

u32 VIGetTvFormat(void) { return 0; /* VI_NTSC */ }
u32 VIGetDTVStatus(void) { return 0; }

void* VISetPreRetraceCallback(void* cb) {
    void* old = (void*)vi_pre_callback;
    vi_pre_callback = (void (*)(u32))cb;
    return old;
}

void* VISetPostRetraceCallback(void* cb) {
    void* old = (void*)vi_post_callback;
    vi_post_callback = (void (*)(u32))cb;
    return old;
}

u32 VIGetCurrentLine(void) { return 0; }

void VISetNextXFB(void* xfb) { (void)xfb; }
