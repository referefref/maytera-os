// proc/capgate.c - C glue for the Stage 0 capability gate.
//
// The DECISIONS all live in rustkern/capgate.rs (2026-07-16 rule: new kernel
// code is Rust). This file is the thin part that cannot be: it reaches into
// process_t for the caller's pid/tgid and it calls kprintf/bootlog_write.
// Nothing here decides anything.

#include "capgate.h"
#include "process.h"
#include "../serial.h"
#include "../string.h"   // snprintf (see the truncation note in capgate_report)
#include "../fs/bootlog.h"

extern uint32_t fbown_owner_rs(void);

void capgate_selftest(void)
{
    uint32_t r = capgate_selftest_rs();
    if (r == 0) {
        kprintf("[CAPGATE] selftest OK (compositor principal, handle ownership,"
                " unclaimed-latch refusal, pthread widening)\n");
    } else {
        kprintf("[CAPGATE] SELFTEST FAILED case %u\n", (unsigned)r);
        // Durable, because neither of the owner's machines has a serial port
        // and a security gate that failed its own self-test must not be
        // discoverable only by someone who happened to be watching COM1.
        (void)bootlog_write("[CAPGATE] SELFTEST FAILED case %u", (unsigned)r);
    }
}

int capgate_caller_is_compositor(void)
{
    process_t *p = proc_current();
    if (!p) return 0;
    return capgate_is_compositor_rs(p->pid);
}

int capgate_caller_owns(uint32_t owner_pid, uint32_t owner_tgid)
{
    process_t *p = proc_current();
    if (!p) return 0;
    return capgate_owner_ok_rs(owner_pid, owner_tgid, p->pid, p->tgid);
}

void capgate_report(void)
{
    capgate_stats_t st;
    if (capgate_stats_rs(&st) != 0) return;

    // ONE LINE, and it carries BOTH numbers for every gate on purpose.
    //
    // "refused=0" on its own reads exactly like "this gate is not wired", and
    // this codebase has shipped that confusion repeatedly. The pair
    // "obs=0/41203" (refused/allowed) says something a single number cannot:
    // the gate is LIVE, it has admitted the compositor 41,203 times, and it has
    // turned nobody away. That is also the POSITIVE CONTROL for Stage 0's
    // riskiest change: if gating SYS_GET_KEYBOARD had broken the compositor,
    // the allowed count would be pinned at 0 and the refused count would be
    // climbing instead.
    //
    // Rides the [SCHEDSTAT] heartbeat rather than only serial, because neither
    // of the owner's target machines has a serial port and an audit trail that
    // reaches only COM1 is worthless on them (#307 is the same lesson).
    char b[256];
    int k = snprintf(b, sizeof(b),
        "[CAPGATE] fbowner=%u obs=%u/%u inj=%u/%u tcp=%u/%u shm=%u/%u dev=%u/%u"
        " (refused/allowed)",
        (unsigned)fbown_owner_rs(),
        (unsigned)st.refused[CAPGATE_K_INPUT_OBSERVE], (unsigned)st.allowed[CAPGATE_K_INPUT_OBSERVE],
        (unsigned)st.refused[CAPGATE_K_INPUT_INJECT],  (unsigned)st.allowed[CAPGATE_K_INPUT_INJECT],
        (unsigned)st.refused[CAPGATE_K_TCP_OWNER],     (unsigned)st.allowed[CAPGATE_K_TCP_OWNER],
        (unsigned)st.refused[CAPGATE_K_SHM_OWNER],     (unsigned)st.allowed[CAPGATE_K_SHM_OWNER],
        (unsigned)st.refused[CAPGATE_K_DEV_PERM],      (unsigned)st.allowed[CAPGATE_K_DEV_PERM]);
    if (k <= 0) return;
    // NO `if (k >= sizeof(b))` TRUNCATION GUARD HERE, AND ITS ABSENCE IS
    // DELIBERATE. The first draft of this function had one, copied from the
    // [SCHEDSTAT] producer in proc/process.c. It is DEAD CODE in both places:
    // string.h:49-53 states that this kernel's vsnprintf "returns the bytes
    // actually WRITTEN (capped at size-1), not the C99 'would have written', so
    // the standard `if (n >= size)` test can never be true and every copy of it
    // in this tree is dead code". Shipping a guard that cannot fire is worse
    // than shipping none, because it reads as coverage.
    //
    // The real argument is that this line CANNOT truncate: the fixed text is
    // about 70 bytes and the eleven %u fields are at most 10 digits each, so the
    // worst case is roughly 180 of the 256 available. If a counter is ever added
    // here, either widen the buffer or switch to vsnprintf_dropped(), which is
    // the only call in this kernel that can actually detect the loss.
    kprintf("%s\n", b);
    bootlog_heartbeat_note(b);
}
