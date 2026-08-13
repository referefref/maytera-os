// noblock.c - #426 Phase 2 runtime assertion. See noblock.h for the design,
// the exact definition of a no-block context, and the honest list of what this
// does NOT cover.

#include "noblock.h"
#include "../proc/process.h"
#include "../fs/panic.h"

extern int kprintf(const char *fmt, ...);

volatile uint64_t g_wq_noblock_violations = 0;

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

uint32_t wq_noblock_reason(void) {
    uint32_t why = 0;

    // 1. Scheduler live? Nothing to switch to before this is true.
    if (!sched_preemption_enabled()) why |= WQ_NB_NO_SCHED;

    // 2. Are we a process? A wait_queue_entry_t needs a process_t to park.
    if (!proc_current()) why |= WQ_NB_NO_PROC;

    // 3. Interrupts on? IF clear means an interrupt gate (ISR) or a
    //    cli+spinlock section. Parking there cannot be woken.
    uint64_t rfl;
    __asm__ volatile("pushfq; popq %0" : "=r"(rfl));
    if (!(rfl & 0x200ULL)) why |= WQ_NB_IRQ_OFF;

    return why;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
//
// De-duplicated by CALLER ADDRESS, not by count: one violating call site
// usually fires thousands of times (it is in a loop by definition), and a
// serial port that is 100% busy printing the same line is itself a hang. Each
// distinct site is reported ONCE with its return address, which addr2line
// resolves against that build's kernel.elf; after that it is silently counted.

#define WQ_NB_SITES_MAX  16
static void *g_nb_sites[WQ_NB_SITES_MAX];
static int   g_nb_site_count = 0;
static int   g_nb_overflow_logged = 0;

// Returns 1 if this caller has not been reported before.
static int nb_site_is_new(void *caller) {
    for (int i = 0; i < g_nb_site_count; i++) {
        if (g_nb_sites[i] == caller) return 0;
    }
    if (g_nb_site_count >= WQ_NB_SITES_MAX) {
        if (!g_nb_overflow_logged) {
            g_nb_overflow_logged = 1;
            kprintf("[WQBLOCK] site table full (%d distinct sites); further "
                    "NEW sites are counted but not printed\n", WQ_NB_SITES_MAX);
        }
        return 0;
    }
    g_nb_sites[g_nb_site_count++] = caller;
    return 1;
}

static void nb_reason_str(uint32_t why, char *out, unsigned outsz) {
    unsigned p = 0;
    const char *parts[3];
    int n = 0;
    if (why & WQ_NB_NO_SCHED) parts[n++] = "SCHEDULER-NOT-LIVE";
    if (why & WQ_NB_NO_PROC)  parts[n++] = "NO-CURRENT-PROCESS";
    if (why & WQ_NB_IRQ_OFF)  parts[n++] = "INTERRUPTS-OFF(ISR-or-cli+spinlock)";
    for (int i = 0; i < n && p + 1 < outsz; i++) {
        if (i && p + 1 < outsz) out[p++] = '+';
        const char *s = parts[i];
        while (*s && p + 1 < outsz) out[p++] = *s++;
    }
    if (p < outsz) out[p] = '\0';
    else if (outsz) out[outsz - 1] = '\0';
}

uint32_t wq_assert_may_block(const char *what, void *caller) {
    uint32_t why = wq_noblock_reason();
    if (why == WQ_NB_OK) return WQ_NB_OK;

    g_wq_noblock_violations++;

    process_t *me = proc_current();
    char reasons[80];
    nb_reason_str(why, reasons, sizeof(reasons));

#ifdef WQ_NOBLOCK_PANIC
    // Hunting mode: stop at the first one, with the full state intact.
    kpanic("#426 wq_assert_may_block: %s called from a NO-BLOCK context "
           "[%s] caller=%p pid=%u '%s'",
           what ? what : "?", reasons, caller,
           me ? me->pid : 0u, me ? me->name : "(none)");
#else
    // Shipping mode: report each distinct site once, loudly, and carry on.
    // Deliberately NOT fatal: an assertion that bricks a user's boot is an
    // assertion that gets compiled out, and then it protects nobody.
    if (nb_site_is_new(caller)) {
        kprintf("[WQBLOCK] #426 VIOLATION: %s from a no-block context [%s] "
                "caller=%p pid=%u '%s' (violation #%lu). "
                "addr2line -e kernel.elf %p\n",
                what ? what : "?", reasons, caller,
                me ? me->pid : 0u, me ? me->name : "(none)",
                (unsigned long)g_wq_noblock_violations, caller);
    }
#endif
    return why;
}
