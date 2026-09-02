// tlbflush.c - cross-CPU TLB shootdown (#404). See tlbflush.h for the design.
//
// LANGUAGE JUSTIFICATION (CLAUDE.md requires one for new C). This is not new
// logic in a place Rust could sit behind a clean FFI boundary. It is (a) the C
// half of a raw interrupt stub that must run with no prologue that could take a
// lock, (b) inline `invlpg`/CR3 paging asm, and (c) a function that is CALLED
// FROM INSIDE spinlock_acquire() and bkl_take_locked(), the kernel's two
// innermost locking primitives, on their contended path. Putting a cross-
// language call there would add an FFI boundary to the most re-entrancy-
// sensitive code in the tree to buy nothing measurable.

#include "../types.h"
#include "tlbflush.h"
#include "vmm.h"
#include "../cpu/smp.h"
#include "../cpu/mono.h"
#include "../sync/spinlock.h"

#include "../fs/bootlog.h"
#include "../security/selftest_registry.h"

extern void kprintf(const char *fmt, ...);

// Provided by cpu/smp.c: per_cpu_data[] and cpus_online are static there.
extern uint32_t smp_online_mask_excluding_self(void);
extern void     smp_send_tlb_ipi_mask(uint32_t mask);
extern void     lapic_eoi(void);

int g_tlb_shootdown_enable = 1;   // /NOTLBSHOOT.TXT clears it: the control arm
int g_tlb_verbose = 0;

// #404 THIRD ARM. /TLBNOIPI.TXT suppresses the IPI send while leaving the
// request published, so the ONLY way a peer can acknowledge is the cooperative
// poll in spinlock_acquire() / bkl_take_locked(). Without this arm the
// cooperative backstop is code that has never been seen to run: every arm of
// the first campaign reported coop=0, because the IPI always won. A redundant
// wake source whose second arm is unproven is one arm with extra comments.
int g_tlb_no_ipi = 0;

// ---------------------------------------------------------------------------
// Local interrupt-flag helpers. sync/spinlock.c has identical static inlines
// but does not export them, and this file must not include a .c.
// ---------------------------------------------------------------------------
static inline uint64_t tlb_irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(f) : : "memory");
    return f;
}
static inline void tlb_irq_restore(uint64_t f) {
    __asm__ volatile("pushq %0\n\tpopfq" : : "r"(f) : "memory", "cc");
}

// ---------------------------------------------------------------------------
// The request. ONE at a time, serialised by g_tlb_lock. On a machine whose
// whole kernel is already behind a Big Kernel Lock, a per-mm request queue
// would be throughput engineering with no throughput to win; one global slot is
// the version that can be reasoned about, and reasoning about it is the point.
// ---------------------------------------------------------------------------
#define TLB_RANGE_PAGES_MAX 32          // above this, flush everything instead
#define TLB_ACK_SOFT_US     2000ull     // resend the IPIs after this
#define TLB_ACK_HARD_US     100000ull   // give up, count it, shout

static volatile uint64_t g_tlb_start;
static volatile uint64_t g_tlb_end;      // 0,0 with all_flag set = flush all
static volatile uint32_t g_tlb_all;
static volatile uint32_t g_tlb_wait_mask;   // one bit per CPU still to confirm

// Statistics. Diagnostic only; torn reads are acceptable.
static volatile uint64_t g_tlb_calls;       // shootdowns requested
static volatile uint64_t g_tlb_broadcasts;  // of those, ones that sent an IPI
static volatile uint64_t g_tlb_skipped;     // suppressed by the control gate
static volatile uint64_t g_tlb_serviced;    // requests serviced on this machine
static volatile uint64_t g_tlb_via_ipi;     // ... entered through the 0xF2 stub
static volatile uint64_t g_tlb_via_coop;    // ... entered through a spin loop
static volatile uint64_t g_tlb_wait_us;     // total ack wait
static volatile uint64_t g_tlb_wait_max_us;
static volatile uint64_t g_tlb_resends;
static volatile uint64_t g_tlb_missed;      // acks that NEVER arrived: bad

// Per-CPU service counts, so "the receiver fires" can be shown per core rather
// than as one aggregate that a single busy core could account for on its own.
static volatile uint64_t g_tlb_cpu_serviced[MAYTERA_MAX_CPUS];

// ---------------------------------------------------------------------------
// Local invalidation. Always correct, never conditional on any gate: a core
// must invalidate its OWN stale entry even in the control arm, otherwise the
// control arm would be testing local invalidation rather than cross-CPU
// invalidation and the negative result would mean nothing.
// ---------------------------------------------------------------------------
static void tlb_local_invalidate(uint64_t start, uint64_t end, uint32_t all) {
    if (all || end <= start ||
        (end - start) > (uint64_t)TLB_RANGE_PAGES_MAX * 4096ull) {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
        return;
    }
    for (uint64_t a = start; a < end; a += 4096ull)
        __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
}

// ---------------------------------------------------------------------------
// RECEIVER. Reached from the 0xF2 stub and from every cooperative poll point.
//
// Interrupts are held OFF across the read-invalidate-clear. That is what makes
// it atomic against a nested delivery of the SAME core: without it, an outer
// cooperative call could read (start,end) for generation N, be interrupted by
// the 0xF2 IPI which completes generation N, have a new sender publish
// generation N+1, and then resume and clear ITS OWN BIT FOR N+1 having
// invalidated only N's range. The whole request would be silently lost, which
// is the exact failure mode this file exists to prevent.
// ---------------------------------------------------------------------------
static void tlb_service_local_inner(int via_ipi) {
    // ORDER MATTERS. Test the plain global FIRST, before smp_this_cpu(), which
    // reads gs:24. This function is called from inside spinlock_acquire(), and
    // spinlocks are taken long before GS has a per-CPU base; a gs-relative load
    // with a null GS base would read absolute address 24. The mask is zero
    // until an actual shootdown is in flight, so on every early-boot path and
    // in the overwhelmingly common steady-state case this costs exactly one
    // load of an already-hot word and a predicted branch, and never touches GS.
    if (!g_tlb_wait_mask) return;
    uint32_t me = smp_this_cpu();
    if (me >= 32) return;
    const uint32_t bit = 1u << me;
    if (!(g_tlb_wait_mask & bit)) return;

    uint64_t fl = tlb_irq_save();
    if (g_tlb_wait_mask & bit) {
        uint64_t s = g_tlb_start, e = g_tlb_end;
        uint32_t a = g_tlb_all;
        __asm__ volatile("" ::: "memory");
        tlb_local_invalidate(s, e, a);
        __sync_fetch_and_and(&g_tlb_wait_mask, ~bit);
        __sync_fetch_and_add(&g_tlb_serviced, 1);
        if (me < MAYTERA_MAX_CPUS) g_tlb_cpu_serviced[me]++;
        if (via_ipi) __sync_fetch_and_add(&g_tlb_via_ipi, 1);
        else         __sync_fetch_and_add(&g_tlb_via_coop, 1);
    }
    tlb_irq_restore(fl);
}

void tlb_service_local(void) { tlb_service_local_inner(0); }

void tlb_ipi_handler(void) {
    tlb_service_local_inner(1);
    lapic_eoi();
}

// ---------------------------------------------------------------------------
// The shootdown lock is the SHARED spinlock, and that choice does real work
// here rather than merely satisfying the reuse rule.
//
// A hand-rolled lock was written first and the concurrency lint rejected it,
// correctly: a private acquire loop that does not mask RFLAGS.IF lets the
// holder be interrupted into cpu/idt.c, which takes the BKL in every ISR, so
// the holder becomes a BKL waiter while still holding this lock, and any BKL
// owner wanting this lock then deadlocks AB-BA. spinlock_acquire_irqsave()
// masks IF for the whole hold and closes that.
//
// It is ALSO the loop that #404 taught to service incoming shootdowns
// (sync/spinlock.c), so a core queued here for the lock keeps acknowledging
// the current sender while it waits. Two cores each waiting on the other for
// an acknowledgement, one of them queued behind this lock, would otherwise
// deadlock. One primitive, both properties, no private copy.
static spinlock_t g_tlb_lock;

// ---------------------------------------------------------------------------
// SENDER.
// ---------------------------------------------------------------------------
static void tlb_shootdown(uint64_t start, uint64_t end, uint32_t all) {
    __sync_fetch_and_add(&g_tlb_calls, 1);

    // 1. THIS core, always and unconditionally, gate or no gate.
    tlb_local_invalidate(start, end, all);

    // 2. Single core: there is nobody else to tell. No IPI, no lock, no spin,
    //    no memory barrier. This is the shipping configuration and it must cost
    //    one predicted branch. cpus_online is 1 until an AP actually comes up,
    //    so this is self-adjusting and does not depend on reading a gate flag.
    if (smp_get_online_count() <= 1) return;

    // 3. The negative control: behave exactly like the pre-#404 kernel.
    if (!g_tlb_shootdown_enable) { __sync_fetch_and_add(&g_tlb_skipped, 1); return; }

    // IF is masked for the whole hold, so this context cannot migrate core
    // between computing the target mask and waiting for that exact mask.
    uint64_t fl = spinlock_acquire_irqsave(&g_tlb_lock);

    g_tlb_start = start; g_tlb_end = end; g_tlb_all = all;
    __sync_synchronize();           // publish the address BEFORE the mask

    uint32_t mask = smp_online_mask_excluding_self();
    if (!mask) { spinlock_release_irqrestore(&g_tlb_lock, fl); return; }
    g_tlb_wait_mask = mask;
    __sync_synchronize();

    __sync_fetch_and_add(&g_tlb_broadcasts, 1);
    if (!g_tlb_no_ipi) smp_send_tlb_ipi_mask(mask);

    uint64_t t0 = mono_us(), last = t0;
    while (g_tlb_wait_mask) {
        __asm__ volatile("pause");
        uint64_t now = mono_us();
        if (now - last >= TLB_ACK_SOFT_US) {
            // A lost IPI is the likeliest cause and costs one resend. The
            // cooperative poll would eventually cover this anyway; resending
            // keeps the common case fast instead of relying on the backstop.
            last = now;
            __sync_fetch_and_add(&g_tlb_resends, 1);
            if (!g_tlb_no_ipi) smp_send_tlb_ipi_mask(g_tlb_wait_mask);
        }
        if (now - t0 >= TLB_ACK_HARD_US) {
            // NOT silent. A missed acknowledgement means a core may still hold
            // a stale translation, so the caller's about-to-free frame is
            // unsafe. There is nothing correct left to do from here, so the
            // one useful thing is to make it impossible to miss in a log.
            __sync_fetch_and_add(&g_tlb_missed, 1);
            kprintf("[TLBSHOOT] MISSED ack: mask=0x%x still pending after "
                    "%lluus for [0x%llx,0x%llx) all=%u. A core may hold a "
                    "stale translation.\n", g_tlb_wait_mask,
                    (unsigned long long)(now - t0),
                    (unsigned long long)start, (unsigned long long)end, all);
            g_tlb_wait_mask = 0;
            break;
        }
    }
    uint64_t took = mono_us() - t0;
    __sync_fetch_and_add(&g_tlb_wait_us, took);
    if (took > g_tlb_wait_max_us) g_tlb_wait_max_us = took;

    spinlock_release_irqrestore(&g_tlb_lock, fl);
}

void tlb_flush_page(uint64_t virt_addr) {
    uint64_t s = virt_addr & ~0xFFFULL;
    tlb_shootdown(s, s + 4096ull, 0);
}

void tlb_flush_range(uint64_t start, uint64_t end) {
    if (end <= start) return;
    tlb_shootdown(start & ~0xFFFULL, (end + 0xFFFULL) & ~0xFFFULL, 0);
}

void tlb_flush_all(void) { tlb_shootdown(0, 0, 1); }

// ---------------------------------------------------------------------------
void tlb_report(const char *why) {
    kprintf("[TLBSHOOT] %s: calls=%llu bcast=%llu skipped=%llu serviced=%llu "
            "(ipi=%llu coop=%llu) resend=%llu MISSED=%llu wait_us=%llu "
            "max=%lluus online=%u gate=%d noipi=%d\n",
            why ? why : "",
            (unsigned long long)g_tlb_calls, (unsigned long long)g_tlb_broadcasts,
            (unsigned long long)g_tlb_skipped, (unsigned long long)g_tlb_serviced,
            (unsigned long long)g_tlb_via_ipi, (unsigned long long)g_tlb_via_coop,
            (unsigned long long)g_tlb_resends, (unsigned long long)g_tlb_missed,
            (unsigned long long)g_tlb_wait_us,
            (unsigned long long)g_tlb_wait_max_us,
            smp_get_online_count(), g_tlb_shootdown_enable, g_tlb_no_ipi);
    for (uint32_t c = 0; c < MAYTERA_MAX_CPUS; c++) {
        if (g_tlb_cpu_serviced[c])
            kprintf("[TLBSHOOT]   cpu%u serviced %llu\n", c,
                    (unsigned long long)g_tlb_cpu_serviced[c]);
    }
}

// ---------------------------------------------------------------------------
// SELF-TEST: prove the receiver fires before anything trusts it.
//
// This project has shipped a stop-IPI with no gate and no stub, a truncation
// detector structurally unable to fire, and a panic harness whose regex did not
// match what panic prints. So the mechanism gets exercised at boot, with its
// result printed whether it passes or fails, and the pass criterion is that
// OTHER CORES incremented their OWN service counters - not that a function
// returned.
// ---------------------------------------------------------------------------
int tlb_selftest(void) {
    uint32_t online = smp_get_online_count();
    if (online <= 1) {
        selftest_notrun("tlbshoot", "only one CPU is online: there is no peer "
                        "core to shoot down to, and the single-core path sends "
                        "no IPI by design");
        return 0;
    }
    uint64_t before[MAYTERA_MAX_CPUS];
    for (uint32_t c = 0; c < MAYTERA_MAX_CPUS; c++) before[c] = g_tlb_cpu_serviced[c];
    uint64_t miss0 = g_tlb_missed;
    uint64_t t0 = mono_us();
    // A harmless address: invalidating a page nobody has mapped is a no-op on
    // every core, so the ONLY thing this measures is delivery and acknowledgement.
    for (int i = 0; i < 8; i++) tlb_flush_page(0x00000000DEAD0000ull);
    uint64_t took = mono_us() - t0;

    uint32_t responders = 0, expected = 0;
    uint32_t me = smp_this_cpu();
    for (uint32_t c = 0; c < MAYTERA_MAX_CPUS; c++) {
        if (c == me) continue;
        if (c < online) expected++;
        if (g_tlb_cpu_serviced[c] > before[c]) responders++;
    }
    int ok = (responders >= expected) && (g_tlb_missed == miss0);
    kprintf("[TLBSHOOT] selftest %s: 8 shootdowns in %lluus, %u/%u peer cores "
            "acknowledged, missed=%llu\n", ok ? "PASS" : "FAIL",
            (unsigned long long)took, responders, expected,
            (unsigned long long)(g_tlb_missed - miss0));
    bootlog_write(ok ? "[TLBSHOOT] selftest PASS (peers acknowledged)"
                     : "[TLBSHOOT] selftest FAIL (peers did NOT acknowledge)");
    tlb_report("after-selftest");
    return ok ? 0 : -1;
}
