// irqwin.c - #69: storage and reporting for the per-site interrupts-off window
// accounting declared in irqwin.h. See that header for why this exists.

#include "irqwin.h"
#include "../string.h"
#include "../cpu/mono.h"

volatile uint64_t g_irqwin_max[IRQWIN_NSITES];
volatile uint64_t g_irqwin_tot[IRQWIN_NSITES];
volatile uint64_t g_irqwin_cnt[IRQWIN_NSITES];
volatile uint64_t g_irqwin_drain_max_frames;
volatile uint64_t g_irqwin_noblock;

// Short names, because this goes on one serial line next to [NETSTARVE] and on
// a machine whose only telemetry is /HEARTBEAT.TXT. The `sk*` prefix marks the
// five sites that hold NO net_lock, which is the point of the whole exercise.
const char *const g_irqwin_name[IRQWIN_NSITES] = {
    "skpump",   // IRQWIN_SK_PUMP
    "skrx",     // IRQWIN_SK_TCP_RX
    "sktx",     // IRQWIN_SK_TCP_TX
    "skconn",   // IRQWIN_SK_TCP_CONN
    "skudp",    // IRQWIN_SK_UDP_TX
    "dnsst",    // IRQWIN_DNS_START
    "dnspo",    // IRQWIN_DNS_POLL
    "ping",     // IRQWIN_PING
    "rxdrain",  // IRQWIN_RX_DRAIN
    "dhcpre",   // IRQWIN_DHCP_RESTART
    "tconn",    // IRQWIN_TCP_CONNECT
    "tsend",    // IRQWIN_TCP_SEND
    "trecv",    // IRQWIN_TCP_RECV
    "tclose",   // IRQWIN_TCP_CLOSE
    "tstate",   // IRQWIN_TCP_STATE
    "taccept",  // IRQWIN_TCP_ACCEPT
    "_drain",   // IRQWIN_SUB_DRAIN     (sub-measurement, inside another window)
    "_tmr",     // IRQWIN_SUB_TCPTIMER  (sub-measurement)
    "_trcv",    // IRQWIN_SUB_TCPRECV   (sub-measurement)
    "_dry",     // IRQWIN_SUB_DRAIN0    (a chunk that found the ring empty)
};

// A name table that has drifted from the enum reports the wrong holder, which
// is the exact failure #118 called out (a duration with no name is not
// actionable; a duration with the WRONG name is worse). Lock the two together
// at compile time rather than by convention.
_Static_assert(sizeof(g_irqwin_name) / sizeof(g_irqwin_name[0]) == IRQWIN_NSITES,
               "irqwin: g_irqwin_name[] and the IRQWIN_* enum have drifted");

// Report worst-max first and STOP at the buffer, so this can never be the
// source of the console stall it exists to diagnose (#745's lesson: a report
// that can storm is a second source of the freeze it is reporting).
//
// Reads and RESETS max, so each line describes its own interval, matching the
// [NETSTARVE] convention. cnt/tot are cumulative on purpose: a rate needs a
// running total, and #118's "cumulative counter looks reassuring while the
// current rate is near zero" cuts the other way for a MAX.
int irqwin_report(char *buf, unsigned long len) {
    if (!buf || len < 32) return 0;
    uint64_t khz = mono_tsc_khz();
    if (!khz) khz = 1;

    // Snapshot and reset first, so the sort below cannot be perturbed by a
    // window closing underneath it.
    uint64_t mx[IRQWIN_NSITES], cn[IRQWIN_NSITES], tt[IRQWIN_NSITES];
    for (int i = 0; i < IRQWIN_NSITES; i++) {
        mx[i] = g_irqwin_max[i]; g_irqwin_max[i] = 0;
        cn[i] = g_irqwin_cnt[i];
        tt[i] = g_irqwin_tot[i];
    }

    int n = 0;
    // Selection sort over 16 entries: no allocation, no recursion, and it runs
    // outside any lock. Bubble/selection is the right call at this size.
    int used[IRQWIN_NSITES];
    for (int i = 0; i < IRQWIN_NSITES; i++) used[i] = 0;
    for (int k = 0; k < IRQWIN_NSITES; k++) {
        int best = -1;
        for (int i = 0; i < IRQWIN_NSITES; i++) {
            if (used[i] || cn[i] == 0) continue;
            if (best < 0 || mx[i] > mx[best]) best = i;
        }
        if (best < 0) break;
        used[best] = 1;
        unsigned long us  = (unsigned long)(mx[best] * 1000ULL / khz);
        unsigned long avg = cn[best] ? (unsigned long)((tt[best] / cn[best]) * 1000ULL / khz) : 0;
        // name=MAXus/AVGus/COUNT
        int w = snprintf(buf + n, len - (unsigned long)n, "%s%s=%lu/%lu/%lu",
                         n ? " " : "", g_irqwin_name[best], us, avg,
                         (unsigned long)cn[best]);
        if (w <= 0 || (unsigned long)(n + w) >= len - 1) break;
        n += w;
    }
    // The frame high-water mark decides whether the 64 bound is even reached.
    if (n > 0 && (unsigned long)n < len - 24) {
        n += snprintf(buf + n, len - (unsigned long)n, " dfr=%lu nb=%lu",
                      (unsigned long)g_irqwin_drain_max_frames,
                      (unsigned long)g_irqwin_noblock);
    }
    if (n == 0) n = snprintf(buf, len, "none-entered");
    return n;
}
