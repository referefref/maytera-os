// proc/affinity.h - #affinity: PERSISTENT PER-PROCESS CPU AFFINITY.
// Implementation is Rust (rustkern/affinity.rs), per the
// all-new-kernel-code-in-Rust rule. Read that file first: it opens with what
// ALREADY EXISTED, which is more than the ticket assumed.
//
// THE SHORT VERSION OF WHAT WAS ALREADY THERE, measured at dev 75d0b2c8:
//   * SOFT affinity already exists and works. process_t::last_cpu is sticky and
//     is fed to sched_place_rs() as prev_cpu, where it wins outright among idle
//     cores and breaks ties when queueing (rustkern/schedwatch.rs:187,234).
//     The gaps are placement's PREEMPT step and WORK STEALING, neither of which
//     has any affinity term.
//   * process_t::sched_pinned is the #75 SELECTION PIN, not affinity.
//   * process_t::migratable is DEAD (proc_create_user_as sets `int __mig = 0;`).
//
// WHAT IS NEW is a persistent CPU SET per process, plus the per-process
// migration counters without which no affinity claim can be checked.
//
// THE MASK IS A PREFERENCE, NOT A PRISON. A hard pin that leaves a core idle
// while its task waits is worse than the migration it prevents. Enforcement
// carries a starvation escape; this header only answers "is core N preferred".
//
// DEFAULT IS EVERY CORE, and it is a property of the LOOKUP (an absent entry
// reads as AFF_ALL), not a value written at creation. So a process that never
// asks behaves exactly as it did before, and nothing had to be added to the
// process lifecycle for that to be true.
#ifndef PROC_AFFINITY_H
#define PROC_AFFINITY_H

#include "../types.h"

// Every core. The value an unknown pid reads back.
#define AFFINITY_ALL  ((uint64_t)~0ull)

// Bit N set = core N is PREFERRED for this pid.
// Returns 0 on success, -1 for a bad pid or a ZERO mask (which would be a
// request to hang the process, and is refused rather than stored), -2 if the
// table is full.
int32_t  affinity_set_rs(uint32_t pid, uint64_t mask);

// The mask for pid, or AFFINITY_ALL if it has none.
uint64_t affinity_get_rs(uint32_t pid);

// 1 if core cpu is preferred for pid (including the default), else 0.
// A cpu index too wide for the mask answers 1, never 0: a mask that cannot
// express a core must not be read as excluding it.
int32_t  affinity_allows_rs(uint32_t pid, uint32_t cpu);

// Release the entry at process exit. Safe for a pid that has none.
void     affinity_clear_rs(uint32_t pid);

// Per-process migration accounting. Fed from sched_cpuobs_note_rs(), which the
// scheduler already calls on every switch-in, so this costs proc/process.c
// nothing. Returns 0, or -1 if the pid has never been switched in.
int32_t  affinity_stats_rs(uint32_t pid, uint64_t *migrations, uint64_t *switchins,
                           uint64_t *mask, int32_t *last_cpu);

// Walk live entries for a report; returns how many were written.
uint32_t affinity_walk_rs(uint32_t max, uint32_t *pid_out, uint64_t *mig_out,
                          uint64_t *sw_out, uint64_t *mask_out);

// Entries refused because the table was full. MUST be 0; non-zero means the
// migration totals are LOW and some processes are unaccounted.
uint64_t affinity_full_rs(void);

// Slots reclaimed from processes that stopped running long ago. The honest
// release point is process exit (proc/process.c), which this change does not
// touch; without any reclaim an exited process would hold its slot for ever.
// Small and non-zero on a long boot is expected; large means AFF_SLOTS is too
// small for the workload.
uint64_t affinity_evicted_rs(void);

// ---- The honest replacement for the dead `migratable` flag ----------------
// SYS_RUN_NEXT_ON_AP ("runap") has set a flag that proc_create_user_as() throws
// away three lines before reading it since #67 pass 2, and returned success for
// it. These give that syscall a real mechanism: a one-shot, OWNER-CHECKED
// affinity request that the next spawn by that process consumes.
//
// affinity_ap_mask_rs() returns 0 on a uniprocessor rather than a mask, because
// "run it on an AP" cannot be honoured there and the caller must be told, not
// handed a 0 that placement would read as "no constraint".
int32_t  affinity_next_spawn_set_rs(uint32_t requester, uint64_t mask);
uint64_t affinity_next_spawn_take_rs(uint32_t requester);
uint64_t affinity_ap_mask_rs(uint32_t ncpu);

// Zero the counters, keeping the masks, so a measurement can be bracketed.
void     affinity_reset_stats_rs(void);

// ---- A/B gate: /NOAFF.TXT on the FAT ESP makes the mask INERT ------------
// So a before/after compares ONE change and not two binaries. Gates only the
// two read paths; masks are still stored and still reported, so the [AFFMIG]
// line is directly comparable line for line between the two arms.
int32_t  affinity_set_disabled_rs(int32_t on);   // returns the previous value
int32_t  affinity_disabled_rs(void);

// Self-test, and the arm that proves the self-test can fail.
int32_t  affinity_selftest_rs(void);
int32_t  affinity_selftest_negative_rs(void);

_Static_assert(sizeof(uint64_t) == 8, "affinity: u64 FFI width");
_Static_assert(sizeof(uint32_t) == 4, "affinity: u32 FFI width");
_Static_assert(sizeof(int32_t)  == 4, "affinity: i32 FFI width");

#endif
