// proccpu.h - the ONE ranking of "what is eating the CPU" (#178)
//
// WHY THIS FILE EXISTS
// -------------------
// Ranking processes by CPU is four lines of arithmetic and every consumer wrote
// its own. There were FIVE hand-written copies (#178 found a fifth #145 had not
// counted), and they drifted exactly as duplicated logic does:
//
//   userland/apps/taskmanager/main.rs  shipping Task Manager, Processes tab
//   userland/apps/taskmanager/main.c   its rollback-only C twin (DELETED at
//                                      #179 by owner request; see below)
//   userland/apps/top/main.c           /APPS/top, the CLI listing
//   userland/apps/sysmon/main.c        /APPS/SYSMON, the GUI monitor
//   kernel/main.c hb_top_consumers()   "top=" in /HEARTBEAT.TXT (see below)
//
// #145 found ONE defect (idle ranked as a consumer) in all five and a SECOND
// (baselines indexed by pid behind a fixed bound) in two of them, with DIFFERENT
// bounds, 64 and 256. It then fixed the second defect in main.rs and sysmon and
// MISSED IT IN main.c, which was still `if (pid < 64) prev_ticks[pid]` at the
// moment this file was written: a defect fixed twice and missed once, inside
// the very fix whose comment explains the hazard. That is not carelessness, it
// is what five copies do. So the arithmetic now exists once, and the callers
// keep no CPU state of their own to get wrong.
//
// #179 (2026-08-20, owner request) then deleted the taskmanager C twin
// outright, so there are now FOUR consumers, not five. Do not read that as
// this file having become less necessary: the twin was deleted BECAUSE the
// duplication this file exists to end had already cost two defects in one
// week. If you are about to add a fifth ranking, add a caller of this header
// instead.
//
// The three settled invariants, each with exactly one definition, in proccpu.c:
//
//  1. THE DENOMINATOR IS EVERY ROW, IDLE INCLUDED. sched_tick() credits one
//     tick per tick to whatever is on the core, idle included, so that sum is
//     elapsed CPU CAPACITY and a row's share means "share of the whole
//     machine". That is the same quantity sys_get_cpu_usage() puts in the
//     footer, so list and footer can never disagree (#182). Renormalise against
//     BUSY time instead and a compositor using 1% of a quiet box prints 60%.
//
//  2. IDLE LEAVES THE LIST. It is capacity nobody asked for, not a consumer,
//     and because these lists sort by CPU% it took first place on every machine
//     that was not saturated. The headroom is not lost: it is already on screen
//     as 100 minus the footer. The mark is the kernel's PROC_INFO_F_IDLE bit,
//     NEVER a string compare against the name "idle".
//
//  3. BASELINES MATCH BY PID, never by array index. A pid is an identifier, not
//     an index. Indexed-by-pid gave every process past the bound a baseline of
//     zero, so its "delta this interval" was its ENTIRE LIFETIME CPU: enough to
//     win the ranking outright and hold it forever.
//
// THE KERNEL HEARTBEAT IS A STATED EXCEPTION, NOT AN OVERSIGHT
// -----------------------------------------------------------
// kernel/main.c hb_top_consumers() does NOT call this, for two reasons that are
// checkable rather than stylistic:
//
//   (a) It physically cannot. build/build-golden.sh:483 ships the kernel build
//       container `git archive $GIT_COMMIT kernel` and nothing else, so no file
//       outside kernel/ exists when kernel.elf is compiled. A shared thing
//       placed where the kernel build cannot see it is #514 repeating: the
//       concurrency lint sat at <repo>/tools/ and could never run.
//   (b) It is a DIFFERENT quantity. It divides by interval_ticks, a fixed
//       wall-clock window, so its number is "share of one core over the
//       heartbeat interval". Invariant 1 above does not apply to it and must
//       not be imposed on it. It shares only invariants 2 and 3, and those are
//       four lines it holds under a comment pointing back here.
//
// If you are adding a SIXTH consumer: it is in userland, so it calls this.

#ifndef _PROCCPU_H
#define _PROCCPU_H

#include "syscall.h"   /* proc_info_t, PROC_INFO_F_IDLE */

// The kernel's process table is MAX_PROCESSES = 64 slots (kernel/proc/process.h:13),
// so proc_snapshot() can never return more than 64 rows and one bound serves
// every caller. The old copies each guessed a different one (64, 96, 192, 256);
// guessing is what let a row exist with no baseline slot. If the kernel table
// ever grows, this is the one line to follow it.
#define PROCCPU_MAX 64

// Opaque-ish baseline: the previous snapshot's (pid, cpu_ticks), kept BY PID.
// Callers own the storage and must zero it once (static/global storage does
// this for free); they must not read or write the fields.
typedef struct {
    unsigned int       pid[PROCCPU_MAX];
    unsigned long long ticks[PROCCPU_MAX];
    int                n;        /* rows in the previous snapshot */
    int                valid;    /* 0 until a baseline exists */
} proccpu_t;

// Rank ONE snapshot against the previous one.
//
//   st     baseline state, zeroed once by the caller, updated here.
//   procs  the snapshot from sys_proc_list(). COMPACTED IN PLACE: idle rows are
//          removed and the rows below move up.
//   nproc  rows in procs[], as returned by sys_proc_list().
//   pct    caller array of >= nproc entries, filled with each surviving row's
//          percentage, kept in step with procs[].
//
// Returns the number of NON-IDLE rows now at the front of procs[]/pct[]. That
// is the count to display; it is <= nproc. Returns 0 on a bad argument.
//
// On the FIRST call there is no baseline, so every percentage is 0 and the
// return value is still the row count: show the list, not the numbers, or take
// a priming call one interval earlier (top does exactly this).
int proccpu_rank(proccpu_t *st, proc_info_t *procs, int nproc, unsigned int *pct);

// Sort a ranked list by CPU% descending, ties broken by memory descending.
// Stable, so equal rows keep their order between refreshes instead of jittering.
// Separate from proccpu_rank() because top(1) deliberately does not sort.
void proccpu_sort(proc_info_t *procs, unsigned int *pct, int nproc);

#endif /* _PROCCPU_H */
