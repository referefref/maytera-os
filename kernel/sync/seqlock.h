// seqlock.h - #131 (local 151) shared primitive: a writer that NEVER waits
// or blocks, and a reader that gets a torn-free snapshot or a clean, fast
// "not this time" answer, never a wait.
//
// This is deliberately NOT a Linux-style seqlock. A Linux reader loops
// internally until it observes an even sequence with no change in between,
// which is a bounded-but-real spin from the reader's point of view. The
// compositor's draw thread must never block or spin (#426 - the exact
// anti-pattern behind #211/#212/#230/#231/#347/#381/#419/#420), so there is
// no internal retry loop here at all: seqlock_read_begin()/
// seqlock_read_retry() are a single snapshot-and-check pair. If the check
// says "torn", the CALLER decides what to do - retry a small FIXED number
// of times with no waiting in between (each attempt is real work, not a
// poll), or give up immediately and reuse whatever it already has. Putting
// that policy at the call site (not in here) is what makes "never blocks"
// something a reviewer can verify by reading a few lines of a bounded
// `for` loop, instead of trusting an assertion buried in this header.
//
// WRITER SIDE NEVER WAITS, NEVER RETRIES, ALWAYS MAKES IMMEDIATE PROGRESS:
// mark odd, do the write, mark even. It does not check for readers and
// cannot be blocked by one - a slow or absent reader has zero effect on
// the writer's forward progress. Keep the critical section between
// seqlock_write_begin()/seqlock_write_end() SHORT and non-blocking (a
// bounded memcpy-shaped update, not app-logic time) - the longer it runs,
// the more likely a reader's bounded retry budget is exhausted, not the
// less correct anything is (correctness holds regardless of length; only
// reader freshness deteriorates under a long writer critical section, and
// only for as long as that one critical section runs).
//
// First user: kernel/proc/syscall.c #131 (local 151) - a per-window
// content_buffer -> content_presented commit copy (writer: sys_win_
// invalidate()/win16_host_invalidate(), running on the OWNING APP's own
// process) versus the compositor's blit read of content_presented (reader:
// user_window_draw_handler, running on the COMPOSITOR's process). Add
// future users here rather than reimplementing this pattern locally -
// forking a private copy into subsystem code is this project's recurring
// defect (see CLAUDE.md "Reuse the shared primitives").

#ifndef SEQLOCK_H
#define SEQLOCK_H

#include "../types.h"
#include "spinlock.h"   // atomic_fetch_add32, atomic_load32, {read,write}_barrier

typedef struct {
    // Even = stable, no writer in progress. Odd = a writer is between
    // seqlock_write_begin() and seqlock_write_end() right now.
    volatile uint32_t seq;
} seqlock_t;

#define SEQLOCK_INIT { .seq = 0 }

static inline void seqlock_init(seqlock_t *sl) {
    sl->seq = 0;
}

// ---- Writer side ----------------------------------------------------------

static inline void seqlock_write_begin(seqlock_t *sl) {
    atomic_fetch_add32(&sl->seq, 1);   // even -> odd: readers must now reject
    write_barrier();                   // the odd value must be visible before any write below
}

static inline void seqlock_write_end(seqlock_t *sl) {
    write_barrier();                   // every write above must be visible before...
    atomic_fetch_add32(&sl->seq, 1);   // ...odd -> even: readers may now accept
}

// ---- Reader side ------------------------------------------------------
// Usage (single attempt):
//   uint32_t s = seqlock_read_begin(&uw->seq);
//   ... copy/inspect the protected data ...
//   if (seqlock_read_retry(&uw->seq, s)) { /* torn: discard what you read */ }
//
// A bounded-retry caller wraps this in its OWN small fixed-count `for`
// loop (see user_window_draw_handler for the canonical shape) - that loop
// belongs at the call site, not here, so it is visible and auditable
// without reading this header.

static inline uint32_t seqlock_read_begin(const seqlock_t *sl) {
    uint32_t s = atomic_load32((volatile uint32_t *)&sl->seq);
    read_barrier();                    // the snapshot must be taken before any read below
    return s;
}

// Returns non-zero ("reject this read, it may be torn") if a writer was
// mid-update at the start snapshot, or ran (fully or partially) since.
static inline int seqlock_read_retry(const seqlock_t *sl, uint32_t start_seq) {
    read_barrier();                    // every read above must complete before this check
    uint32_t now = atomic_load32((volatile uint32_t *)&sl->seq);
    return (now != start_seq) || (start_seq & 1u);
}

#endif // SEQLOCK_H
