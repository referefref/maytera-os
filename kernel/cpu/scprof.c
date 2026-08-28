// scprof.c - #121: per-syscall time census and phase attribution.
//
// C, not Rust, for the same reason cpu/smp.c holds the bklsite table: this is
// the raw per-cpu storage plus the probe brackets that sit inside the syscall
// dispatch path, the spawn path and the block layer. The DECISIONS - slot
// selection, how a sample merges, which entries rank highest, and whether the
// self-test passed - all live in rustkern/scprof.rs, which is where the new
// code is. Same split as rustkern/bklsite.rs, deliberately, so there is one
// shape for "instrument" in this tree rather than two.
//
// NOT A FORK OF cpu/dlprof.h. That one is a hand-listed set of ~20 named
// globals for ONE download path (#615), on raw rdtsc rather than the calibrated
// mono clock, with no ranking and no per-call attribution. It answers "how much
// of the download was GCM"; it cannot answer "which syscall, for how long, and
// did it service any interrupts". If dlprof is ever revisited it should be
// expressed as phases here rather than the other way round.

#include "scprof.h"
#include "smp.h"
#include "mono.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"

#define SCP_CPUS 8
#define SCPROF_N 512u

// Mirrors rustkern/scprof.rs ScStat, field for field and in order. Locked so
// the FFI cannot drift.
typedef struct {
    uint64_t count, total_us, max_us, worst_irqs, worst_brk;
    uint64_t unbrk_count, unbrk_total_us, unbrk_max_us, unbrk_max_irqs;
    uint64_t unbrk_max_ph[SCP_PHASE_N];
    uint64_t unbrk_sum_ph[SCP_PHASE_N];
} scstat_t;
_Static_assert(sizeof(scstat_t) == 9 * 8 + 2 * SCP_PHASE_N * 8, "scprof: FFI struct layout");
_Static_assert(SCP_PHASE_N == 12, "scprof: phase count is locked to the FFI struct");

scstat_t g_scprof[SCPROF_N];
volatile uint64_t g_scprof_oor;      // syscall numbers >= SCPROF_N, NOT recorded

extern int      scprof_add(scstat_t *tab, uint32_t n, uint64_t num, uint64_t us,
                           uint64_t irqs, uint64_t brk, const uint64_t *ph, uint32_t nph);
extern uint32_t scprof_top(const scstat_t *tab, uint32_t n, int by,
                           uint32_t *out_idx, uint32_t out_n);
extern int      scprof_selfcheck(uint64_t want_us, uint64_t got_us, uint64_t got_ph_us);

// Per-cpu. Written only by the core that owns them, so no lock and no shared
// cacheline: a shared counter here would be most of its own reading, which is
// how #67's first two instruments went wrong.
static uint64_t g_scp_ph[SCP_CPUS][SCP_PHASE_N];   // cumulative phase us
volatile uint64_t g_scp_irq[SCP_CPUS];             // interrupt entries seen

// bkl_release_all() count per cpu, i.e. how many times a BKL hold was BROKEN by
// a context switch. Defined in cpu/smp.c next to the lock it describes.
extern volatile uint64_t g_bkl_brk[];

static const char *const g_scp_phase_name[SCP_PHASE_N] = {
    "pf", "fr", "e2f", "e2c", "fat", "blk", "usb", "spwnrd", "spwncr",
    "elf", "ustk", "pmm"
};

static inline uint32_t scp_cpu(void) {
    uint32_t c = smp_this_cpu();
    return (c < SCP_CPUS) ? c : 0;
}

void scp_irq_tick(void) {
    g_scp_irq[scp_cpu()]++;
}

scp_frame_t scp_enter(void) {
    uint32_t c = scp_cpu();
    scp_frame_t f;
    f.t0 = mono_us();
    f.i0 = g_scp_irq[c];
    f.b0 = g_bkl_brk[c];
    for (int i = 0; i < SCP_PHASE_N; i++) f.ph[i] = g_scp_ph[c][i];
    return f;
}

void scp_exit(uint64_t num, scp_frame_t f) {
    uint32_t c = scp_cpu();
    uint64_t now = mono_us();
    // mono_us() returns 0 until the TSC is calibrated (cpu/mono.h), and syscalls
    // are dispatched before that on the boot path. Dropping those samples keeps
    // "0 us" from meaning two different things: an uncounted sample is better
    // than one that reads as instantaneous.
    if (now < f.t0 || f.t0 == 0) return;
    uint64_t ph[SCP_PHASE_N];
    for (int i = 0; i < SCP_PHASE_N; i++) ph[i] = g_scp_ph[c][i] - f.ph[i];
    if (scprof_add(g_scprof, SCPROF_N, num, now - f.t0,
                   g_scp_irq[c] - f.i0, g_bkl_brk[c] - f.b0,
                   ph, SCP_PHASE_N) == 1)
        g_scprof_oor++;   // 1 == SCPROF_OOR: out of range, said so rather than folded
}

scp_span_t scp_begin(void) {
    scp_span_t s; s.t0 = mono_us(); return s;
}

void scp_end(int phase, scp_span_t s) {
    if (phase < 0 || phase >= SCP_PHASE_N || s.t0 == 0) return;
    uint64_t now = mono_us();
    if (now < s.t0) return;
    g_scp_ph[scp_cpu()][phase] += now - s.t0;
}

// PROVE THE PROBE READS A KNOWN DURATION BEFORE ANY READING IS BELIEVED.
//
// Four instruments in this family have become the thing they measured (a spin
// counter that was 93% of its own reading, an acquire counter with the same
// shared-cacheline defect, a hold timer that could not say which core it timed,
// and a [BKLMAX] line that always reported its own call site). So: run a
// synthetic "syscall" that burns a known number of microseconds inside a known
// phase, against the SAME clock the probe uses, and print whether the table
// recorded it. If this line is not OK, nothing else printed here may be used.
//
// Uses syscall number SCPROF_N-1, which proc/syscall.h does not define and the
// dispatcher rejects, so it cannot collide with a real measurement.
#define SCP_SELFTEST_NUM (SCPROF_N - 1u)
void scp_selftest(void) {
    const uint64_t want = 5000;   // 5 ms, far above any real syscall at boot
    if (!mono_ready()) {
        kprintf("[SCPROF-PROBE] SKIPPED (mono clock not calibrated)\n");
        return;
    }
    scp_frame_t f = scp_enter();
    scp_span_t  s = scp_begin();
    { uint64_t t0 = mono_us(); while (mono_us() - t0 < want) { /* bounded by the clock */ } }
    scp_end(SCP_BLKREAD, s);
    scp_exit(SCP_SELFTEST_NUM, f);
    scstat_t *e = &g_scprof[SCP_SELFTEST_NUM];
    int bad = scprof_selfcheck(want, e->unbrk_max_us, e->unbrk_max_ph[SCP_BLKREAD]);
    if (bad == 0) {
        kprintf("[SCPROF-PROBE] OK: asked %llu us, unbroken total %llu us, phase blk "
                "%llu us, irqs %llu (brk must be 0 for a phase profile to exist)\n",
                (unsigned long long)want, (unsigned long long)e->unbrk_max_us,
                (unsigned long long)e->unbrk_max_ph[SCP_BLKREAD],
                (unsigned long long)e->unbrk_max_irqs);
    } else {
        kprintf("[SCPROF-PROBE] FAILED (0x%x): asked %llu us, unbroken total %llu us, "
                "phase blk %llu us. The syscall profiler is WRONG; do not trust "
                "[SCPROF] lines from this build.\n", (unsigned)bad,
                (unsigned long long)want, (unsigned long long)e->unbrk_max_us,
                (unsigned long long)e->unbrk_max_ph[SCP_BLKREAD]);
    }
}

static void scp_phases(char *out, size_t cap, const uint64_t *ph) {
    int o = 0;
    out[0] = 0;
    for (int i = 0; i < SCP_PHASE_N; i++) {
        if (!ph[i]) continue;   // print only phases that fired
        o += snprintf(out + o, cap - (size_t)o, " %s=%llu",
                      g_scp_phase_name[i], (unsigned long long)ph[i]);
        if (o >= (int)cap - 24) break;
    }
    if (o == 0) snprintf(out, cap, " none");
}

void scp_report(void) {
    uint32_t idx[4], k;
    char ph[160];

    k = scprof_top(g_scprof, SCPROF_N, 0 /* BY_TOTAL */, idx, 4);
    for (uint32_t i = 0; i < k; i++) {
        scstat_t *e = &g_scprof[idx[i]];
        kprintf("[SCPROF-TOTAL] #%u sc=%u n=%llu total=%lluus max=%lluus(brk=%llu irq=%llu)\n",
                i, idx[i], (unsigned long long)e->count, (unsigned long long)e->total_us,
                (unsigned long long)e->max_us, (unsigned long long)e->worst_brk,
                (unsigned long long)e->worst_irqs);
    }
    // THE RANKING THIS TICKET NEEDS: longest UNBROKEN call == longest
    // syscall_entry BKL hold, with the phase profile of that exact call and the
    // aggregate profile over every unbroken call of the same number.
    k = scprof_top(g_scprof, SCPROF_N, 2 /* BY_UNBRK */, idx, 4);
    for (uint32_t i = 0; i < k; i++) {
        scstat_t *e = &g_scprof[idx[i]];
        scp_phases(ph, sizeof(ph), e->unbrk_max_ph);
        kprintf("[SCPROF-HOLD] #%u sc=%u unbrk=%llu/%llu total=%lluus max=%lluus "
                "irq=%llu worstph:%s\n", i, idx[i],
                (unsigned long long)e->unbrk_count, (unsigned long long)e->count,
                (unsigned long long)e->unbrk_total_us, (unsigned long long)e->unbrk_max_us,
                (unsigned long long)e->unbrk_max_irqs, ph);
        scp_phases(ph, sizeof(ph), e->unbrk_sum_ph);
        kprintf("[SCPROF-HOLD] #%u sc=%u sumph:%s\n", i, idx[i], ph);
    }
    // Cumulative per-phase totals, which need no unbroken qualification: each
    // phase's own inclusive time, summed over the whole run and every thread.
    {
        uint64_t tot[SCP_PHASE_N];
        for (int i = 0; i < SCP_PHASE_N; i++) {
            tot[i] = 0;
            for (uint32_t c = 0; c < SCP_CPUS; c++) tot[i] += g_scp_ph[c][i];
        }
        scp_phases(ph, sizeof(ph), tot);
        kprintf("[SCPROF-PHASE] cumulative:%s\n", ph);
    }
    if (g_scprof_oor)
        kprintf("[SCPROF] %llu sample(s) had a syscall number >= %u and were NOT "
                "recorded (totals are LOW)\n", (unsigned long long)g_scprof_oor, SCPROF_N);
    // #122: the block layer's own per-CALL census, printed in the same
    // already-released-BKL window so it cannot become the hold it reports.
    { extern void blk_census_report(void); blk_census_report(); }
}
