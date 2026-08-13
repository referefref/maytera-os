// dlprof.c - #615 download-phase region profiler (MEASUREMENT BUILD).
#include "dlprof.h"
#include "mono.h"

extern int kprintf(const char *fmt, ...);

volatile uint64_t g_dp_gcm_cyc,   g_dp_gcm_bytes;
volatile uint64_t g_dp_gh_cyc,    g_dp_gh_calls;
volatile uint64_t g_dp_sha_cyc,   g_dp_sha_bytes;
volatile uint64_t g_dp_poll_cyc,  g_dp_poll_calls;
volatile uint64_t g_dp_tcprx_cyc, g_dp_tcprx_calls, g_dp_tcprx_bytes;
volatile uint64_t g_dp_wg_iter,   g_dp_wg_sleep,    g_dp_wg_bytes;
volatile uint64_t g_dp_tls_iter,  g_dp_tls_sleep,   g_dp_tls_bytes;
volatile uint64_t g_dp_hb_iter,   g_dp_hb_sleep;
volatile uint64_t g_dp_fetch_cyc, g_dp_fetch_calls;
volatile uint64_t g_dp_wr_cyc,    g_dp_wr_bytes;

void dlprof_report(void) {
    static uint64_t p[24];
    static uint64_t p_ms;
    uint64_t c[24];
    c[0]=g_dp_gcm_cyc;   c[1]=g_dp_gcm_bytes;
    c[2]=g_dp_gh_cyc;    c[3]=g_dp_gh_calls;
    c[4]=g_dp_sha_cyc;   c[5]=g_dp_sha_bytes;
    c[6]=g_dp_poll_cyc;  c[7]=g_dp_poll_calls;
    c[8]=g_dp_tcprx_cyc; c[9]=g_dp_tcprx_calls; c[10]=g_dp_tcprx_bytes;
    c[11]=g_dp_wg_iter;  c[12]=g_dp_wg_sleep;   c[13]=g_dp_wg_bytes;
    c[14]=g_dp_tls_iter; c[15]=g_dp_tls_sleep;  c[16]=g_dp_tls_bytes;
    c[17]=g_dp_hb_iter;  c[18]=g_dp_hb_sleep;
    c[19]=g_dp_fetch_cyc;c[20]=g_dp_fetch_calls;
    c[21]=g_dp_wr_cyc;   c[22]=g_dp_wr_bytes;

    uint64_t d[24];
    for (int i = 0; i < 23; i++) d[i] = c[i] - p[i];
    uint64_t ms = mono_ms();
    uint64_t dms = ms - p_ms;
    for (int i = 0; i < 23; i++) p[i] = c[i];
    p_ms = ms;
    if (dms == 0) return;
    uint64_t khz = mono_tsc_khz();
    if (khz == 0) khz = 1;
    uint64_t denom = khz * dms;      /* cycles of one core in this interval */
    if (denom == 0) denom = 1;

    /* Stay silent when nothing in the download path moved, so an idle desktop
       boot log is unchanged. */
    if (d[19] == 0 && d[1] == 0 && d[13] == 0 && d[16] == 0 && d[22] == 0) return;

    kprintf("[DLPROF] dt=%llums fetch=%llu%%/%llu wr=%llu%%/%lluKB "
            "gcm=%llu%%/%lluKB gh=%llu%%/%llu poll=%llu%%/%llu "
            "tcprx=%llu%%/%llu/%lluKB sha=%llu%%/%lluKB "
            "wg=%llu:%llu:%lluKB tls=%llu:%llu:%lluKB hb=%llu:%llu\n",
            dms,
            d[19] * 100 / denom, d[20],
            d[21] * 100 / denom, d[22] / 1024,
            d[0]  * 100 / denom, d[1] / 1024,
            d[2]  * 100 / denom, d[3],
            d[6]  * 100 / denom, d[7],
            d[8]  * 100 / denom, d[9], d[10] / 1024,
            d[4]  * 100 / denom, d[5] / 1024,
            d[11], d[12], d[13] / 1024,
            d[14], d[15], d[16] / 1024,
            d[17], d[18]);
}
