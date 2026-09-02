// tlbtest.c - #404 stale-TLB reproducer AND its negative control.
//
// THE POINT OF THIS FILE. A shootdown that is never seen to be NEEDED is an
// unfalsifiable change: it cannot be distinguished from a no-op that happens to
// leave the machine working. This project has shipped a stop-IPI with no gate,
// a truncation detector structurally unable to fire, and a panic-detecting
// regex that scored a boot with 65 panic lines as a pass. So the claim being
// made here is not "the code compiles and the machine still boots". It is:
//
//   ON THE SAME kernel.elf, with /NOTLBSHOOT.TXT present the peer cores READ
//   THE OLD PAGE THROUGH A STALE TRANSLATION, and with it absent they do not.
//
// HOW THE STALENESS IS MADE TO HAPPEN ON DEMAND
//
//   W  is a physical frame taken from the PMM. Because the kernel is identity
//      mapped, its address doubles as a kernel VIRTUAL address that nothing
//      else uses, so its PTE can be rewritten freely.
//   F1, F2 are two more frames, filled with two different 64-bit patterns.
//
//   The mutator (BSP) alternates the mapping of VA W between F1 and F2. The
//   readers (one per AP) read VA W and compare it against the pattern the
//   mutator says should be there. A reader that has cached W -> F1 and never
//   receives a shootdown keeps reading P1 after the mapping has become F2.
//   That is the corruption, made visible: same address, same instant, two cores
//   disagreeing about what memory contains.
//
// WHY THE READERS RUN AS smp_work JOBS AND NOT AS KERNEL THREADS. The AP work
// loop (cpu/smp.c) pops submitted jobs BEFORE it falls into sched_ap_enter(),
// so a job runs on a known AP, in kernel context, with no scheduler and no CR3
// switching underneath it. A scheduler thread would be preempted onto a user
// process, whose context switch reloads CR3 and flushes the whole TLB, which
// would destroy the very staleness being measured. The jobs are bounded and
// return, after which the APs continue into normal scheduling.
//
// The ordering is a generation counter with a full acknowledgement barrier, so
// a reader can never be comparing against a pattern from a different iteration:
// the mutator publishes the expected value, THEN bumps the generation, and then
// refuses to start the next iteration until every reader has acknowledged this
// one. A false positive would need the mutator to run ahead of a reader, and it
// structurally cannot.

#include "../types.h"
#include "vmm.h"
#include "pmm.h"
#include "tlbflush.h"
#include "../cpu/smp.h"
#include "../cpu/mono.h"
#include "../fs/bootlog.h"
#include "../sync/spinlock.h"

extern void kprintf(const char *fmt, ...);
// persist-extern-gate: include the owning header, never re-declare it here.

#define TLBT_P1   0x1111111111111111ULL
#define TLBT_P2   0x2222222222222222ULL
#define TLBT_ITERS 200

static volatile uint64_t g_tlbt_window;      // the VA under test
static volatile uint64_t g_tlbt_gen;         // bumped after each remap
static volatile uint64_t g_tlbt_expect;      // value that must be at the window
static volatile uint32_t g_tlbt_readers;     // bit per participating AP
static volatile uint32_t g_tlbt_ack;         // bit per AP that has checked gen
static volatile int      g_tlbt_stop;
static volatile uint64_t g_tlbt_reads[MAYTERA_MAX_CPUS];
static volatile uint64_t g_tlbt_stale[MAYTERA_MAX_CPUS];   // THE MEASUREMENT
static volatile uint64_t g_tlbt_first_bad[MAYTERA_MAX_CPUS];

// EVERY reader hammers this ONE lock, on purpose. Two reasons, both load-bearing:
//
//  1. REALISM. The state a TLB shootdown has to survive is not "a peer core
//     idling". It is "a peer core spinning inside an IRQSAVE spinlock with
//     RFLAGS.IF CLEAR", because that is where mm_lock() puts a core while
//     another core unmaps and frees pages under it. A core in that state CANNOT
//     take the 0xF2 IPI at all, however many are sent.
//  2. IT IS THE ONLY WAY TO EXERCISE THE COOPERATIVE ARM. The first campaign
//     measured ipi=630 coop=0 in every run: with idle peers the IPI always won,
//     so the cooperative poll in spinlock_acquire() was code that had never
//     been observed to execute. Three cores contending here put them in the
//     poll loop continuously, so the /TLBNOIPI.TXT arm can prove that the
//     backstop delivers on its own.
static spinlock_t g_tlbt_contend;


// ---------------------------------------------------------------------------
// MEASURED 2026-08-30, build 2281, and it stopped the first run of this test
// dead: the FIRMWARE PAGE TABLES ARE READ-ONLY IN THEIR OWN IDENTITY MAPPING,
// and CR0.WP is set, so a Ring-0 write to them page-faults.
//
//   [KERNEL PANIC] Page Fault at RIP=0x45b85e  CR2=0x7bc03fb8 err=0x3
//   -> mm/vmm.c:1240, "pd[pd_idx] = pt_phys | ..." inside vmm_map_page_in()
//
// err=0x3 is present + write + supervisor, i.e. a supervisor write to a
// read-only page, and 0x7bc03fb8 is a page-directory entry, not data. So
// vmm_map_page_in() cannot edit the KERNEL address space at all once it has to
// split a huge leaf or create a table: it can only ever edit the PMM-allocated
// tables of a USER address space. That is consistent with every one of its real
// callers, and it is why this file needs the two instructions below rather than
// being able to remap a kernel VA the ordinary way.
//
// CR0.WP=0 lifts read-only enforcement for supervisor writes only, on THIS core
// only, for the duration of one vmm_map_page_in() call. It is confined to this
// test file, which only runs when /TLBSTRESS.TXT is present.
static uint64_t tlbt_wp_off(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    if (cr0 & (1ULL << 16))
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 & ~(1ULL << 16)) : "memory");
    return cr0;
}
static void tlbt_wp_restore(uint64_t cr0) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}
static int tlbt_map(uint64_t cr3, uint64_t va, uint64_t phys, uint64_t flags) {
    uint64_t cr0 = tlbt_wp_off();
    int rc = vmm_map_page_in(cr3, va, phys, flags);
    tlbt_wp_restore(cr0);
    return rc;
}

// One reader, pinned by construction to whichever AP pops it.
static void tlbt_reader(void *arg) {
    (void)arg;
    uint32_t me = smp_this_cpu();
    if (me >= 32) return;
    const uint32_t bit = 1u << me;
    if (g_tlbt_readers & bit) return;        // this AP already has a reader
    __sync_fetch_and_or(&g_tlbt_readers, bit);

    uint64_t last = 0;
    // Touch the window once up front so this core definitely has a translation
    // cached before the first remap. Without this the first iteration would be
    // a cold miss on every core and would prove nothing either way.
    (void)*(volatile uint64_t *)g_tlbt_window;

    while (!g_tlbt_stop) {
        uint64_t g = g_tlbt_gen;
        if (g == last) { __asm__ volatile("pause"); continue; }
        // Read the window from INSIDE a contended irqsave spinlock: IF is
        // clear here, which is exactly the state the IPI cannot reach.
        uint64_t fl = spinlock_acquire_irqsave(&g_tlbt_contend);
        uint64_t want = g_tlbt_expect;
        __asm__ volatile("" ::: "memory");
        uint64_t got = *(volatile uint64_t *)g_tlbt_window;
        spinlock_release_irqrestore(&g_tlbt_contend, fl);
        g_tlbt_reads[me]++;
        if (got != want) {
            if (!g_tlbt_stale[me]) g_tlbt_first_bad[me] = g;
            g_tlbt_stale[me]++;
        }
        last = g;
        __sync_fetch_and_or(&g_tlbt_ack, bit);
    }
}

void tlb_stress_start(void) {
    uint32_t online = smp_get_online_count();
    if (online <= 1) {
        kprintf("[TLBTEST] SKIPPED: online=%u. This test needs a peer core; "
                "with one core there is no stale peer TLB to create.\n", online);
        return;
    }

    uint64_t W  = pmm_alloc_page();
    uint64_t F1 = pmm_alloc_page();
    uint64_t F2 = pmm_alloc_page();
    if (!W || !F1 || !F2) {
        kprintf("[TLBTEST] SKIPPED: could not allocate 3 frames (W=0x%llx "
                "F1=0x%llx F2=0x%llx)\n", (unsigned long long)W,
                (unsigned long long)F1, (unsigned long long)F2);
        return;
    }
    for (int i = 0; i < 8; i++) {
        ((volatile uint64_t *)F1)[i] = TLBT_P1;
        ((volatile uint64_t *)F2)[i] = TLBT_P2;
    }
    g_tlbt_window = W;
    g_tlbt_expect = 0;
    g_tlbt_gen = 0;
    g_tlbt_ack = 0;
    g_tlbt_readers = 0;
    g_tlbt_stop = 0;
    spinlock_init(&g_tlbt_contend);

    uint64_t cr3 = vmm_get_pml4();
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;

    // Start at F1 and publish it, so the readers' warm-up touch caches W -> F1.
    if (tlbt_map(cr3, W, F1, flags) != 0) {
        kprintf("[TLBTEST] ABORTED: could not map the window VA\n");
        pmm_free_page(F1); pmm_free_page(F2); pmm_free_page(W);
        return;
    }
    g_tlbt_expect = TLBT_P1;
    __sync_synchronize();

    // Hand one reader job to each AP. If one AP happens to pop two, the second
    // returns immediately (the bit test above) and that AP simply does not take
    // part; the report prints how many cores actually did, rather than assuming.
    for (uint32_t i = 1; i < online; i++)
        (void)smp_work_submit(tlbt_reader, 0, 0);

    uint64_t t0 = mono_us();
    while (g_tlbt_readers == 0 && (mono_us() - t0) < 2000000ull)
        __asm__ volatile("pause");
    if (!g_tlbt_readers) {
        kprintf("[TLBTEST] ABORTED: no AP picked up a reader job in 2s. The "
                "test proves nothing; do not read a pass into this.\n");
        g_tlbt_stop = 1;
        return;
    }
    // Let any remaining APs register too, then freeze the participant set.
    t0 = mono_us();
    while ((mono_us() - t0) < 200000ull) __asm__ volatile("pause");
    uint32_t participants = g_tlbt_readers;
    int npart = 0;
    for (int c = 0; c < 32; c++) if (participants & (1u << c)) npart++;

    kprintf("[TLBTEST] start: window VA=0x%llx F1=0x%llx F2=0x%llx, %d peer "
            "core(s) reading (mask=0x%x), shootdown gate=%d, %d iterations\n",
            (unsigned long long)W, (unsigned long long)F1,
            (unsigned long long)F2, npart, participants,
            g_tlb_shootdown_enable, TLBT_ITERS);

    int timeouts = 0;
    for (int it = 0; it < TLBT_ITERS; it++) {
        uint64_t phys = (it & 1) ? F2 : F1;
        uint64_t pat  = (it & 1) ? TLBT_P2 : TLBT_P1;

        g_tlbt_ack = 0;
        __sync_synchronize();

        // THE REMAP. The shootdown, if it happens at all, happens inside here:
        // vmm_map_page_in() -> vmm_tlb_page() -> tlb_flush_page(). With
        // /NOTLBSHOOT.TXT present that call still invalidates THIS core and
        // then returns without telling anybody, which is precisely the
        // pre-#404 kernel.
        tlbt_map(cr3, W, phys, flags);

        g_tlbt_expect = pat;
        __sync_synchronize();
        g_tlbt_gen = (uint64_t)it + 1;
        __sync_synchronize();

        uint64_t s = mono_us();
        while ((g_tlbt_ack & participants) != participants) {
            __asm__ volatile("pause");
            if (mono_us() - s > 1000000ull) { timeouts++; break; }
        }
    }
    g_tlbt_stop = 1;
    __sync_synchronize();
    // Give the readers a moment to leave their loop before the mapping is
    // restored underneath them.
    t0 = mono_us();
    while ((mono_us() - t0) < 300000ull) __asm__ volatile("pause");

    // Put the identity mapping back and give the frames away again.
    tlbt_map(cr3, W, W, flags);
    pmm_free_page(F1); pmm_free_page(F2); pmm_free_page(W);

    uint64_t total_reads = 0, total_stale = 0;
    for (int c = 0; c < MAYTERA_MAX_CPUS; c++) {
        total_reads += g_tlbt_reads[c];
        total_stale += g_tlbt_stale[c];
        if (g_tlbt_reads[c])
            kprintf("[TLBTEST]   cpu%d: %llu reads, %llu STALE (first bad at "
                    "gen %llu)\n", c, (unsigned long long)g_tlbt_reads[c],
                    (unsigned long long)g_tlbt_stale[c],
                    (unsigned long long)g_tlbt_first_bad[c]);
    }
    kprintf("[TLBTEST] RESULT gate=%d cores=%d reads=%llu STALE=%llu "
            "timeouts=%d\n", g_tlb_shootdown_enable, npart,
            (unsigned long long)total_reads,
            (unsigned long long)total_stale, timeouts);
    // The verdict is deliberately phrased so that neither arm can be mistaken
    // for the other in a log grep.
    if (total_stale)
        kprintf("[TLBTEST] VERDICT=CORRUPTION-OBSERVED (%llu stale reads on "
                "%d peer core(s))\n", (unsigned long long)total_stale, npart);
    else
        kprintf("[TLBTEST] VERDICT=CLEAN (no peer core ever read through a "
                "stale translation in %llu reads)\n",
                (unsigned long long)total_reads);
    bootlog_write("[TLBTEST] gate=%d cores=%d reads=%llu stale=%llu",
                  g_tlb_shootdown_enable, npart,
                  (unsigned long long)total_reads,
                  (unsigned long long)total_stale);
    tlb_report("after-stress");
}
