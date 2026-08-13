// dlprof.h - #615 download-phase region profiler (MEASUREMENT BUILD).
//
// Cheap rdtsc accumulators around every candidate CPU sink in the App Store
// download path, plus iteration/sleep counters for the receive loops. The
// [DLPROF] line printed next to each [HB] heartbeat reports the per-interval
// delta as a percentage of one core's wall clock, which is what distinguishes a
// SPIN (huge iteration count, few cycles per byte) from REAL WORK (cycles
// concentrated in one region).
#ifndef DLPROF_H
#define DLPROF_H

#include "../types.h"

extern volatile uint64_t g_dp_gcm_cyc,   g_dp_gcm_bytes;
extern volatile uint64_t g_dp_gh_cyc,    g_dp_gh_calls;
extern volatile uint64_t g_dp_sha_cyc,   g_dp_sha_bytes;
extern volatile uint64_t g_dp_poll_cyc,  g_dp_poll_calls;
extern volatile uint64_t g_dp_tcprx_cyc, g_dp_tcprx_calls, g_dp_tcprx_bytes;
extern volatile uint64_t g_dp_wg_iter,   g_dp_wg_sleep,    g_dp_wg_bytes;
extern volatile uint64_t g_dp_tls_iter,  g_dp_tls_sleep,   g_dp_tls_bytes;
extern volatile uint64_t g_dp_hb_iter,   g_dp_hb_sleep;
extern volatile uint64_t g_dp_fetch_cyc, g_dp_fetch_calls;
extern volatile uint64_t g_dp_wr_cyc,    g_dp_wr_bytes;

static inline uint64_t dp_tsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void dlprof_report(void);

#endif // DLPROF_H
