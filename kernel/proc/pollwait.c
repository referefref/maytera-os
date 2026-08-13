// pollwait.c - the ONLY C in the poll(2) implementation (#745, local 82).
//
// WHY THIS FILE IS C AND NOT RUST, stated explicitly because the 2026-07-16
// rule requires a justification rather than a habit:
//
//   wait_event_interruptible_deadline() is a statement-expression MACRO. Its
//   condition argument is pasted textually into the loop body and re-evaluated
//   between sleeps; that is the whole mechanism by which a wake that lands
//   between __wait_prepare() and the sleep cannot be lost. A macro cannot be
//   called from Rust. The alternatives were (a) re-implement the prepare /
//   sleep / finish / re-check dance in Rust against the raw waitq entry points,
//   which duplicates the single most subtle piece of concurrency code in the
//   tree and would drift from it, or (b) one four-line C shim that passes the
//   condition in as a function pointer. (b) keeps waitq.h as the ONE definition
//   of what a wait is, which is the rule that matters more here than the
//   language rule. All poll LOGIC (the scan, the classification, the queue
//   choice, the deadline arithmetic) is in rustkern/pollsys.rs.
//
// Nothing else lives here. If this file grows a second job, it is in the wrong
// place.

#include "../types.h"
#include "../sync/waitq.h"
#include "../fs/vfs.h"

// The userland struct pollfd. Mirrored by #[repr(C)] struct PollFd in
// rustkern/pollsys.rs; the sizeof lock below is what keeps the two honest,
// because the argtab descriptor for SYS_POLL computes nfds * sizeof and a
// silent size change would validate the wrong number of bytes.
struct k_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};
_Static_assert(sizeof(struct k_pollfd) == 8,
               "struct pollfd must stay 8 bytes: rustkern/pollsys.rs PollFd and the "
               "#503 argtab entry for SYS_POLL both assume it");
_Static_assert(sizeof(((struct k_pollfd *)0)->fd) == 4, "pollfd.fd is int32");
_Static_assert(sizeof(((struct k_pollfd *)0)->events) == 2, "pollfd.events is int16");

// The queue used when there is nothing better to park on: nfds == 0 (a plain
// POSIX sleep), or a descriptor set that publishes no wait queue at all. NOTHING
// EVER WAKES THIS. That is deliberate and is not a lost wake: in both cases the
// DEADLINE is the intended wake, and parking on a real queue that happens to
// belong to an unrelated subsystem would be worse (a spurious wake from someone
// else's traffic, charged to us). A signal still interrupts it, because
// wait_event_interruptible_deadline() is the interruptible form.
static wait_queue_head_t g_poll_idle_wq = { .head = NULL, .lock = SPINLOCK_INIT };

void *poll_idle_wq(void) { return &g_poll_idle_wq; }

// wait_event_interruptible_deadline() with the condition supplied as a callback
// so Rust can own it. Returns WAIT_OK / WAIT_TIMEOUT / WAIT_EINTR verbatim.
//
// `deadline_ms` is an ABSOLUTE sched_now_ms() value, or WAIT_DEADLINE_NEVER.
// The caller computes it ONCE; this shim never re-arms a relative timeout,
// which is the failure mode waitq.h warns about for retry loops.
int poll_wait_cond(void *wq, uint64_t deadline_ms,
                   int (*ready)(void *), void *ctx) {
    if (!wq || !ready) return WAIT_TIMEOUT;
    wait_queue_head_t *q = (wait_queue_head_t *)wq;
    return wait_event_interruptible_deadline(q, ready(ctx) != 0, deadline_ms);
}

// Boot self-test glue: hands the kernel's OWN fs/vfs.h POLL_* values to Rust so
// the two definitions are compared against each other rather than one against
// itself. Returns the Rust failure mask.
extern uint32_t poll_bits_match_rs(int c_in, int c_out, int c_err, int c_hup);
uint32_t poll_bits_check(void) {
    return poll_bits_match_rs(POLL_IN, POLL_OUT, POLL_ERR, POLL_HUP);
}
