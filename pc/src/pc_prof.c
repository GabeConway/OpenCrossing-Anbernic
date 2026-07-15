/* pc_prof.c - slow-phase profiler: logs any wrapped call over a threshold.
 * Threshold set via PC_PROF_MS env var (default 50 ms). */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "pc_prof.h"

static long long g_thresh_us = -1;

static void prof_init(void) {
    const char* env = getenv("PC_PROF_MS");
    long ms = 50;
    if (env && *env) {
        ms = strtol(env, NULL, 10);
        if (ms <= 0) ms = 50;
    }
    g_thresh_us = (long long)ms * 1000;
}

unsigned long long pc_prof_now_us(void) {
#ifdef _WIN32
    /* coarse fallback; instrumentation targets the Linux device build */
    return (unsigned long long)clock() * 1000000ull / CLOCKS_PER_SEC;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)(ts.tv_nsec / 1000);
#endif
}

void pc_prof_report(const char* tag, int id, unsigned long long t0_us) {
    unsigned long long dt;
    if (g_thresh_us < 0) prof_init();
    dt = pc_prof_now_us() - t0_us;
    if ((long long)dt >= g_thresh_us) {
        printf("[PROF] %s(0x%X): %llu.%03llums\n", tag, (unsigned)id,
               dt / 1000, dt % 1000);
        fflush(stdout);
    }
}
