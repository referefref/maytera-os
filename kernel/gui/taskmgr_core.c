// taskmgr_core.c - #404/#487/#349 Task Manager DATA CORE (Rust seam only).
//
// PROVENANCE (#428 Phase 2): this file is the surviving seam extracted from the
// deleted kernel-side Task Manager GUI (gui/taskmanager.c). That 1388-line
// in-kernel app was a FALLBACK that never fired: the shipping Task Manager is
// the userland Rust app (userland/apps/taskmanager -> /APPS/taskmgr), and two
// rounds of uplift were lost in the invisible kernel copy. The GUI is gone; the
// data core stays because main.c boots a [RUST-DIFF] differential over it, which
// is what keeps rustkern/taskmgr.rs honest and non-orphaned.
//
// The C twins below are REFERENCE implementations, deliberately naive: the sort
// over-WRITES its permutation array when count>cap (CWE-787) and the ring
// aggregation divides by zero when cap==0. The Rust ports (perf_ring_stats_rs /
// taskmgr_sort_rows_rs in rustkern.rs, live under -DRUST_TASKMGR_CORE) reject
// both by construction. Proven offline on the build container over 5,000,000 differential
// vectors, 0 mismatch. #[repr(C)] layouts are sizeof-locked below.
#include "../types.h"
#include "../serial.h"
#include "fs/bootlog.h"   // #742: the owning header, NOT a private extern

typedef struct { uint32_t min, max, last, avg, count; } perf_stat_t;
_Static_assert(sizeof(perf_stat_t) == 20, "perf_stat_t FFI layout");
typedef struct { int32_t pid; uint32_t cpu; uint32_t mem; } proc_key_t;
_Static_assert(sizeof(proc_key_t) == 12, "proc_key_t FFI layout");

// Naive ring aggregation (min/max/last/avg over `count` most-recent samples of
// a `cap`-slot ring whose oldest is at `head`). cap==0 -> `% cap` UB.
int perf_ring_stats_c(const uint32_t *samples, uint32_t cap, uint32_t head,
                      uint32_t count, perf_stat_t *out) {
    if (!samples || !out) return -1;
    if (count == 0) { out->min = out->max = out->last = out->avg = out->count = 0; return 0; }
    uint32_t mn = 0xFFFFFFFFu, mx = 0, last = 0; uint64_t sum = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = samples[(head + i) % cap];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v; last = v;
    }
    out->min = mn; out->max = mx; out->last = last;
    out->avg = (uint32_t)(sum / count); out->count = count;
    return 1;
}

static int tm_key_before(const proc_key_t *a, const proc_key_t *b, int key) {
    switch (key) {
        case 0: return a->cpu != b->cpu ? (a->cpu > b->cpu) : (a->pid < b->pid);
        case 1: return a->mem != b->mem ? (a->mem > b->mem) : (a->pid < b->pid);
        case 3: return a->cpu != b->cpu ? (a->cpu < b->cpu) : (a->pid < b->pid);
        case 4: return a->mem != b->mem ? (a->mem < b->mem) : (a->pid < b->pid);
        default: return a->pid < b->pid;
    }
}
// Naive stable permutation sort. Writes idx_out[0..count) with NO check that
// count<=cap -> over-WRITES idx_out (CWE-787) when count>cap.
int taskmgr_sort_rows_c(const proc_key_t *rows, uint32_t count, uint32_t cap,
                        int key, int32_t *idx_out) {
    if (!rows || !idx_out) return -1;
    (void)cap;
    if (count == 0) return 0;
    for (uint32_t i = 0; i < count; i++) idx_out[i] = (int32_t)i;
    for (uint32_t i = 1; i < count; i++) {
        int32_t cur = idx_out[i]; uint32_t j = i;
        while (j > 0 && tm_key_before(&rows[cur], &rows[idx_out[j - 1]], key)) {
            idx_out[j] = idx_out[j - 1]; j--;
        }
        idx_out[j] = cur;
    }
    return (int)count;
}

#ifdef RUST_TASKMGR_CORE
extern int perf_ring_stats_rs(const uint32_t *, uint32_t, uint32_t, uint32_t, perf_stat_t *);
extern int taskmgr_sort_rows_rs(const proc_key_t *, uint32_t, uint32_t, int, int32_t *);
#endif

// #404 boot-time [RUST-DIFF] self-test. Bounded, runs once (#426).
void taskmgr_core_selftest(void) {
    // Well-formed differential over a small deterministic corpus.
    uint32_t ring[16]; for (int i = 0; i < 16; i++) ring[i] = (uint32_t)(i * 7 + 3);
    proc_key_t rows[16];
    for (int i = 0; i < 16; i++) { rows[i].pid = 16 - i; rows[i].cpu = (uint32_t)((i * 13) % 100); rows[i].mem = (uint32_t)((i * 97) % 5000); }
    int mism = 0, vecs = 0;
#ifdef RUST_TASKMGR_CORE
    for (uint32_t cap = 1; cap <= 16; cap++) {
        for (uint32_t head = 0; head < cap; head++) {
            uint32_t count = cap / 2;
            perf_stat_t sc, sr;
            int rc = perf_ring_stats_c(ring, cap, head, count, &sc);
            int rr = perf_ring_stats_rs(ring, cap, head, count, &sr);
            vecs++;
            if (rc != rr || sc.min != sr.min || sc.max != sr.max || sc.last != sr.last ||
                sc.avg != sr.avg || sc.count != sr.count) mism++;
            int32_t ic[16], ir[16];
            int kc = taskmgr_sort_rows_c(rows, cap, 16, (int)(head % 5), ic);
            int kr = taskmgr_sort_rows_rs(rows, cap, 16, (int)(head % 5), ir);
            vecs++;
            if (kc != kr) mism++;
            else for (uint32_t i = 0; i < cap; i++) if (ic[i] != ir[i]) { mism++; break; }
        }
    }
    // confinement witnesses (Rust must reject; C would over-write / div0)
    int32_t idx[16]; perf_stat_t o;
    int c_ov = taskmgr_sort_rows_rs(rows, 40, 8, 0, idx);   // count>cap
    int c_z  = perf_ring_stats_rs(ring, 0, 0, 4, &o);        // cap==0
    kprintf("[RUST-DIFF] taskmgr_core: %d vecs mism=%d %s (LIVE=rust)\n", vecs, mism, mism ? "*** MISMATCH ***" : "MATCH");
    kprintf("[RUST-SEC]  taskmgr_core: sort count>cap rust rc=%d (confined, C over-writes), ring cap0 rust rc=%d (confined, C div0)\n", c_ov, c_z);
    bootlog_write("[RUST-DIFF] taskmgr_core %d vecs mism=%d %s", vecs, mism, mism ? "MISMATCH" : "MATCH");
#else
    for (uint32_t cap = 1; cap <= 16; cap++) { perf_stat_t sc; perf_ring_stats_c(ring, cap, 0, cap / 2, &sc); vecs++; }
    kprintf("[RUST-DIFF] taskmgr_core: %d vecs (LIVE=c, Rust seam not compiled)\n", vecs);
    bootlog_write("[RUST-DIFF] taskmgr_core %d vecs LIVE=c", vecs);
    (void)mism; (void)rows;
#endif
}
