// proccpu.c - the ONE ranking of "what is eating the CPU" (#178).
// Read proccpu.h first: it records why five copies of this existed, which
// defect each copy had, and why the kernel heartbeat is a stated exception.

#include "proccpu.h"

// The snapshot is a kernel ABI struct. If its size moves, every array here and
// every caller's snapshot buffer is wrong, so lock it at the one place that
// touches all of them. (#145 put the idle mark in pre-existing tail padding
// precisely so this stayed 64 and the ABI stayed byte-identical.)
_Static_assert(sizeof(proc_info_t) == 64, "proc_info_t ABI changed; see kernel/proc/procinfo.h");

// INVARIANT 3, and its only implementation: baselines are found BY PID.
// Linear over at most PROCCPU_MAX=64 entries. Not a hash, not an index: a pid
// is an identifier, and indexing by it is the defect this file exists to make
// unrepeatable.
static unsigned long long proccpu_base(const proccpu_t *st, unsigned int pid)
{
    for (int i = 0; i < st->n; i++)
        if (st->pid[i] == pid)
            return st->ticks[i];
    return 0;   /* new this interval: no baseline, so no delta is attributed */
}

// This row's CPU ticks since the previous snapshot. Saturating: cpu_ticks is
// monotonic per process so cur < base should be impossible, and a subtraction
// that CAN wrap to ~2^64 is not worth leaving to "should".
static unsigned long long proccpu_delta(const proccpu_t *st, const proc_info_t *p)
{
    unsigned long long b = proccpu_base(st, p->pid);
    return (p->cpu_ticks >= b) ? (p->cpu_ticks - b) : 0ULL;
}

int proccpu_rank(proccpu_t *st, proc_info_t *procs, int nproc, unsigned int *pct)
{
    if (!st || !procs || !pct || nproc <= 0)
        return 0;

    // Cannot happen while MAX_PROCESSES is 64, but a row we cannot store a
    // baseline for is a row whose next delta would be its whole lifetime, which
    // is defect #2. Drop it rather than show it with a broken number.
    if (nproc > PROCCPU_MAX)
        nproc = PROCCPU_MAX;

    // INVARIANT 1: the denominator sums EVERY row, IDLE INCLUDED, so a share is
    // a share of total CPU capacity and agrees with sys_get_cpu_usage().
    // Computed BEFORE any row is dropped, and the deltas are recomputed rather
    // than buffered: 64 rows twice is nothing, and it keeps this function free
    // of a scratch array whose size would be one more thing to get wrong.
    unsigned long long tot = 0;
    for (int i = 0; i < nproc; i++)
        tot += proccpu_delta(st, &procs[i]);

    for (int i = 0; i < nproc; i++)
        pct[i] = (st->valid && tot)
               ? (unsigned int)((proccpu_delta(st, &procs[i]) * 100ULL) / tot)
               : 0u;

    // The baseline is saved from the FULL list, idle included, BEFORE any row
    // is dropped. Save it after compaction and the next interval's denominator
    // silently loses the idle deltas, which is invariant 1 broken one refresh
    // later instead of immediately.
    st->n = nproc;
    for (int i = 0; i < nproc; i++) {
        st->pid[i]   = procs[i].pid;
        st->ticks[i] = procs[i].cpu_ticks;
    }
    st->valid = 1;

    // INVARIANT 2: idle leaves the LIST, having already done its work in the
    // denominator. procs[] and pct[] move together or the numbers detach from
    // the names.
    int w = 0;
    for (int i = 0; i < nproc; i++) {
        if (procs[i].flags & PROC_INFO_F_IDLE)
            continue;
        if (w != i) {
            procs[w] = procs[i];
            pct[w]   = pct[i];
        }
        w++;
    }
    return w;
}

void proccpu_sort(proc_info_t *procs, unsigned int *pct, int nproc)
{
    if (!procs || !pct || nproc <= 1)
        return;
    // Insertion sort, stable: n <= 64, and a list that reorders equal rows on
    // every refresh is unusable. Strictly-greater is what makes it stable.
    for (int i = 1; i < nproc; i++) {
        for (int j = i; j > 0; j--) {
            int hi = (pct[j] > pct[j - 1]) ||
                     (pct[j] == pct[j - 1] && procs[j].mem_kb > procs[j - 1].mem_kb);
            if (!hi)
                break;
            proc_info_t tp = procs[j]; procs[j] = procs[j - 1]; procs[j - 1] = tp;
            unsigned int tc = pct[j];  pct[j]  = pct[j - 1];    pct[j - 1]  = tc;
        }
    }
}
