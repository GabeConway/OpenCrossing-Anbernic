#ifndef PC_PROF_H
#define PC_PROF_H

/**
 * PC_PROF - slow-phase profiler for hunting one-off frame hangs.
 *
 * Wraps a call site; if the wrapped work takes longer than the threshold
 * (PC_PROF_MS env var, default 50 ms), logs "[PROF] tag(0xID): X.XXXms".
 * Below the threshold the cost is two clock_gettime calls — safe to leave
 * on hot paths.
 *
 * Include inside #ifdef TARGET_PC in decomp files (C89-safe: the macro is
 * a single statement and declares its local inside a block).
 */

#ifdef __cplusplus
extern "C" {
#endif

unsigned long long pc_prof_now_us(void);
void pc_prof_report(const char* tag, int id, unsigned long long t0_us);

#ifdef __cplusplus
}
#endif

#define PC_PROF(tag, id, call)                                  \
    do {                                                        \
        unsigned long long _pc_prof_t0 = pc_prof_now_us();      \
        call;                                                   \
        pc_prof_report(tag, (int)(id), _pc_prof_t0);            \
    } while (0)

#endif /* PC_PROF_H */
