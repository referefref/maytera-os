// kstack.c - Ring-0 kernel stacks with a poisoned guard band. See kstack.h for
// WHY, and in particular for the MEASURED reason this is a software guard
// rather than an MMU guard page.
//
// LANGUAGE NOTE (project rule: new kernel code is Rust unless there is a stated
// reason). This file is C, and the stated reason is the one the rule itself
// names: genuine entanglement with paging and with an FPU-free hot path. The
// allocator talks to the PMM in physical frames on an identity map, and its
// product is consumed by the one-AND check in kstack.h that runs inside
// isr_handler() on every single interrupt. A Rust FFI boundary in the middle of
// that would buy nothing and cost an indirection on the hottest path in the
// kernel.
#include "kstack.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "../serial.h"
#include "../sync/spinlock.h"   // the SHARED irqsave spinlock, never a private copy
#include "../fs/panic.h"
#include "../fs/bootlog.h"
#include "../security/selftest_registry.h"   // #PERMSKIP: skips are REGISTERED
#ifdef KSTACK_OVERFLOW_SELFTEST
#include "../cpu/mono.h"   // mono_busy_delay_us(): the SHARED delay, not a new spin
#endif

#define KSTACK_PAGE     4096u
#define KSTACK_PAGES    (KSTACK_GRAN / KSTACK_PAGE)          // 16
#define KSTACK_BAND_PG  (KSTACK_BAND / KSTACK_PAGE)          // 1

// Poison word for the band. Distinct from KSTACK_MAGIC so a report can say
// WHICH of the two it found, and chosen to be an obviously-not-data value.
#define KSTACK_POISON 0xBAADB4DDBAADB4DDULL

// Live blocks. A registry rather than a pointer-alignment heuristic:
// kstack_free() must know EXACTLY whether a pointer came from here, because
// guessing wrong means either kfree() on PMM frames or frames leaked forever.
#define KSTACK_MAX 256
typedef struct { uint64_t usable; uint64_t block; uint64_t pages; } kstack_ent_t;
static kstack_ent_t g_ks[KSTACK_MAX];
static uint32_t g_ks_live;

// THIS TABLE HAD NO LOCK, AND THE WINDOW WAS NOT SMALL.
//
// kstack_alloc() scanned for the first row with usable == 0, then allocated
// frames from the PMM and poisoned 4 KiB, and only then wrote the row. Two
// allocators inside that window both saw the same row as free and both wrote it:
// the second won and the first registration was GONE while its granule was live.
// The bill arrived at that stack's kstack_free(), which missed the registry and
// took the "not one of ours" path - the same path that used to launder a kernel
// stack into kfree() and report it as "[HEAP] kfree() called on invalid pointer
// ... magic=0xbaadb4dd". #heapinvptr made that path REFUSE and name itself, so a
// lost registration now leaks a granule loudly instead of corrupting the heap
// quietly; it did not stop the registration being lost.
//
// The BKL is what has been hiding this: it serialises nearly every caller, so
// the window exists but almost nothing runs inside it. BKL narrowing is a
// standing requirement, and every stage of it widens this window. The fix is
// RESERVE-THEN-COMMIT (rustkern/kstackreg.rs): the scan and the reservation are
// one critical section, so a second scan cannot see a reserved row as free. The
// slow work still runs with no lock held.
//
// IRQSAVE IS NOT OPTIONAL. kstack_free() is reached from process teardown that
// runs under cli(); a plain spinlock_acquire() here would be exactly the
// non-irqsave hole sync/noblock.h documents as the gap its assert cannot see.
// The lock is a LEAF: nothing inside it waits, blocks, allocates, prints or
// takes a second lock (the PMM work and every kprintf are outside it), so it
// cannot take part in a deadlock cycle, which is the property that makes it safe
// to hold with interrupts masked. No wait_event is involved, so
// wq_assert_may_block() is not implicated either way.
_Static_assert(sizeof(kstack_ent_t) == 24,
               "rustkern/kstackreg.rs reinterprets these rows as a #[repr(C)] "
               "KstackEnt { u64 usable, u64 block, u64 pages }; adding a field "
               "here would silently re-point every index it computes");
static spinlock_t g_ks_lock = SPINLOCK_INIT_NAMED("kstackreg");

// rustkern/kstackreg.rs. C owns the storage, Rust owns the decision, exactly as
// conring.rs / procreap.rs / proc_mem.rs already do. Every one of these must be
// called with g_ks_lock held.
extern int32_t  kstackreg_claim_rs(kstack_ent_t *tab, uint32_t max);
extern uint32_t kstackreg_commit_rs(kstack_ent_t *tab, uint32_t max, int32_t slot,
                                    uint64_t usable, uint64_t block, uint64_t pages);
extern void     kstackreg_abort_rs(kstack_ent_t *tab, uint32_t max, int32_t slot);
extern int32_t  kstackreg_take_rs(kstack_ent_t *tab, uint32_t max, uint64_t usable,
                                  uint64_t *block_out, uint64_t *pages_out);
extern int32_t  kstackreg_find_rs(const kstack_ent_t *tab, uint32_t max,
                                  uint64_t usable);
extern uint32_t kstackreg_selftest_rs(void);

// Census. Each counter is a reason a stack is NOT protected, so they are all
// printed even when zero: "0 unguarded" is the claim worth being able to make.
static uint64_t g_ks_guarded, g_ks_freed;
static uint64_t g_ks_fb_toobig, g_ks_fb_nomem, g_ks_fb_full, g_ks_fb_align;
static uint64_t g_ks_multigran;   // stacks that needed more than one granule
static uint64_t g_ks_free_foreign;
static uint64_t g_ks_free_orphan;   // a GUARDED stack freed with no registry row
// A commit that found the same usable pointer ALREADY in another row, or a
// commit on a row nothing had reserved. Either means a registration went
// astray, which is the thing the lock exists to make impossible; it is
// counted and reported rather than assumed away, because a fix that cannot
// say "this never happened" is only a belief.
static uint64_t g_ks_reg_anomaly;

// #heapinvptr: WHAT THE LAST FEW FREES WERE, so a second free of the same stack
// is identifiable even after the frames have gone back to the PMM and been
// re-issued with the poison overwritten.
//
// The alternative, testing the shape of the block alone (granule-aligned base
// plus a poisoned band), is in rustkern/kstackid.rs and is the other half of the
// same answer. Neither is sufficient alone: the shape test goes blind once the
// frames are reused, and this ring goes blind once an entry rotates out. 32 is
// chosen to comfortably outlive one process-creation cycle, which is the window
// the measured defect lived in; it is a diagnostic aid and not a correctness
// mechanism, so there is nothing to size exactly.
#define KSTACK_FREE_HIST 32
static uint64_t g_ks_hist[KSTACK_FREE_HIST];
static uint32_t g_ks_hist_next;

// #heapinvptr CONTROL ARM. Default 1: an orphaned kernel stack is REPORTED and
// REFUSED. /NOKSTKORPH.TXT on the ESP clears it, restoring the pre-fix behaviour
// exactly (fall straight through to kfree(), which then reads the poisoned guard
// band as a block header and prints "[HEAP] ERROR: kfree() called on invalid
// pointer 0x... magic=0xbaadb4dd").
//
// IT EXISTS SO THE ORIGINAL SYMPTOM CAN STILL BE PRODUCED ON DEMAND from the same
// kernel bytes, which is how the reproducer in proc/slotdbl.c was tied to the
// reported message rather than merely to a counter of my own invention. Same
// one-binary-two-arms shape as /NOSTACKGUARD.TXT and /NOWAKEFIX.TXT.
int g_kstack_orphan_refuse = 1;

static void kstack_hist_record(uint64_t usable) {
    g_ks_hist[g_ks_hist_next % KSTACK_FREE_HIST] = usable;
    g_ks_hist_next++;
}

static int kstack_hist_has(uint64_t usable) {
    for (uint32_t i = 0; i < KSTACK_FREE_HIST; i++)
        if (g_ks_hist[i] == usable) return 1;
    return 0;
}

// The verdict lives in Rust (2026-07-16 policy: new kernel code is Rust unless
// there is a stated performance or entanglement reason, and a pure predicate over
// six integers has neither). See rustkern/kstackid.rs.
extern int32_t kstack_foreign_verdict_rs(uint64_t usable, uint64_t gran,
                                         uint64_t band, uint64_t below,
                                         uint64_t poison, int32_t recent);
extern uint32_t kstackid_selftest_rs(void);
#define KSTACK_VERDICT_ORPHAN 1

void *kstack_alloc(size_t size) {
    if (size == 0) return NULL;

    // A STACK MAY SPAN SEVERAL GRANULES, AND THE ONE-AND CHECK STILL WORKS.
    //
    // Callers legitimately ask for more than a granule: the TLS/HTTPS async
    // workers take 128 KiB and the installer, cron and several self-tests take
    // 256 KiB. Refusing those would have left 9 of 34 kernel stacks unguarded
    // on a real boot, which is what the first version of this file measured.
    //
    // So round the request up to whole granules and align the whole block to
    // ONE granule. The band stays in the LOWEST granule and the magic word sits
    // at the base of that granule only. kstack_rsp_in_band() then behaves
    // correctly with no change at all: while RSP is in an interior granule the
    // masked base lands on ordinary stack data, which is not the magic, so the
    // check returns 0; the moment RSP descends into the lowest granule the
    // masked base IS the block base, the magic matches, and the band test
    // applies. The interior granules cost one 64-bit false-magic collision
    // chance per interrupt, which is 2^-64 and not a real number.
    uint64_t grans = ((uint64_t)size + KSTACK_BAND + KSTACK_GRAN - 1) / KSTACK_GRAN;
    if (grans == 0) grans = 1;
    if (grans > KSTACK_MAX_GRAN) { g_ks_fb_toobig++; return kmalloc(size); }
    if (grans > 1) g_ks_multigran++;

    // RESERVE the row here, in the same critical section as the scan, BEFORE the
    // PMM allocation and the poison loop below. That is the whole fix: the old
    // code returned an index and wrote it much later, so two callers could both
    // be told the same index was free. kstackreg_claim_rs() marks the row
    // RESERVED as it finds it, so the second caller cannot see it as free.
    int32_t slot;
    { uint64_t __fl = spinlock_acquire_irqsave(&g_ks_lock);
      slot = kstackreg_claim_rs(g_ks, KSTACK_MAX);
      spinlock_release_irqrestore(&g_ks_lock, __fl); }
    if (slot < 0) { g_ks_fb_full++; return kmalloc(size); }

    // pmm_alloc_pages() guarantees 4 KiB alignment, not KSTACK_GRAN.
    // Over-allocate by one granule minus one page so a KSTACK_GRAN-aligned run
    // of the required length is guaranteed to exist inside, then hand the head
    // and the tail straight back. Net cost is exactly the rounded-up size; the
    // over-allocation is transient.
    uint64_t need = grans * KSTACK_PAGES;
    uint64_t want = need + (KSTACK_PAGES - 1);
    uint64_t raw = pmm_alloc_pages(want);
    if (raw == 0) {
        // Release the reservation. A reservation leaked on a failure path would
        // be invisible: the census would still read the same live count and the
        // table would simply run out one row earlier after every failed
        // allocation.
        uint64_t __fl = spinlock_acquire_irqsave(&g_ks_lock);
        kstackreg_abort_rs(g_ks, KSTACK_MAX, slot);
        spinlock_release_irqrestore(&g_ks_lock, __fl);
        g_ks_fb_nomem++; return kmalloc(size);
    }

    uint64_t rawend = raw + want * KSTACK_PAGE;
    uint64_t block = (raw + KSTACK_GRAN - 1) & ~((uint64_t)KSTACK_GRAN - 1);
    if (block < raw || block + need * KSTACK_PAGE > rawend) {
        // Cannot happen with the over-allocation above; counted rather than
        // asserted so a future change to KSTACK_GRAN cannot fail silently.
        pmm_free_pages(raw, want);
        { uint64_t __fl = spinlock_acquire_irqsave(&g_ks_lock);
          kstackreg_abort_rs(g_ks, KSTACK_MAX, slot);
          spinlock_release_irqrestore(&g_ks_lock, __fl); }
        g_ks_fb_align++;
        return kmalloc(size);
    }
    if (block > raw) pmm_free_pages(raw, (block - raw) / KSTACK_PAGE);
    uint64_t tail = block + need * KSTACK_PAGE;
    if (rawend > tail) pmm_free_pages(tail, (rawend - tail) / KSTACK_PAGE);

    // Poison the band, then stamp the identity magic at the very bottom of it.
    // Order matters: the magic must be the LAST thing written, so a block is
    // never identifiable as a kernel stack before its band is actually
    // poisoned - otherwise a concurrent interrupt could read a half-built one.
    uint64_t *w = (uint64_t *)block;
    for (uint32_t k = 0; k < KSTACK_BAND / 8; k++) w[k] = KSTACK_POISON;
    __asm__ volatile("" ::: "memory");
    w[0] = KSTACK_MAGIC;

    uint64_t usable = block + KSTACK_BAND;
    uint32_t anom;
    { uint64_t __fl = spinlock_acquire_irqsave(&g_ks_lock);
      anom = kstackreg_commit_rs(g_ks, KSTACK_MAX, slot, usable, block, need);
      g_ks_live++; g_ks_guarded++;
      if (anom) g_ks_reg_anomaly++;
      spinlock_release_irqrestore(&g_ks_lock, __fl); }
    // OUTSIDE the lock, deliberately: kprintf takes the console lock, and taking
    // a second lock inside this one would destroy the leaf-lock property that is
    // the whole deadlock-freedom argument above.
    if (anom)
        kprintf("[KSTACK] REGISTRY ANOMALY committing %p (mask=0x%x): bit0=the "
                "same pointer was ALREADY in another row, bit1=the row was not "
                "reserved by this allocation. Either means a registration went "
                "astray, which this table's lock is supposed to make impossible. "
                "The block IS recorded (its frames are live), so nothing is "
                "orphaned by the report itself.\n",
                (void *)usable, (unsigned)anom);
    return (void *)usable;
}

void kstack_free(void *base, size_t size) {
    (void)size;
    if (!base) return;
    uint64_t u = (uint64_t)base;
    // FIND AND REMOVE ARE ONE OPERATION, for the same reason the scan and the
    // reservation are: two concurrent frees of the same pointer must not both be
    // told "yes, that row is yours", or the same frames go back to the PMM twice
    // and the second return silently hands live memory to a second owner. The
    // recent-free history ring is written in the same critical section, because
    // kstack_hist_has() in the orphan path below is what tells a double free
    // apart from a kmalloc fallback once the poison has been overwritten.
    uint64_t blk = 0, pgs = 0;
    int32_t  mine;
    { uint64_t __fl = spinlock_acquire_irqsave(&g_ks_lock);
      mine = kstackreg_take_rs(g_ks, KSTACK_MAX, u, &blk, &pgs);
      if (mine) {
          kstack_hist_record(u);   // #heapinvptr: with the row, not after it
          if (g_ks_live) g_ks_live--;
          g_ks_freed++;
      }
      spinlock_release_irqrestore(&g_ks_lock, __fl); }
    if (mine) {
        // Both of these are OUTSIDE the lock, and safely so: the row is gone, so
        // no other caller can be handed this block. Keeping pmm_free_pages()
        // outside is also what keeps g_ks_lock a leaf lock.
        //
        // Clear the magic BEFORE the frames go back to the PMM. Skip this and a
        // reallocated frame still says "I am a kernel stack" to the check in
        // isr_handler(), which would then police an unrelated allocation and
        // could panic a perfectly healthy machine.
        *(volatile uint64_t *)blk = 0;
        pmm_free_pages(blk, pgs);
        return;
    }
    // Not in the registry. THAT HAS TWO MEANINGS AND THEY ARE NOT THE SAME BUG.
    //
    // This used to assume only the benign one, "a fallback allocation from
    // kmalloc", and act on it by calling kfree(). The other meaning is that the
    // block WAS a guarded kernel stack whose row is already gone: a second free,
    // or a lost registration. kfree() then gets a pointer that is not in the heap
    // at all, reads the poisoned guard band at base-32 as a block header, and
    // prints
    //     [HEAP] ERROR: kfree() called on invalid pointer 0x... (magic=0xbaadb4dd)
    // which names the HEAP for a defect in the PROCESS LIFECYCLE. 0xbaadb4dd is
    // KSTACK_POISON, 32 lines up this very file, and that message nevertheless
    // sent an investigation at the allocator. MEASURED verbatim on golden 2600,
    // /root/co2600-fatapps-run.log, pointer 0x0fdb1000 = 0x0fdb0000 + KSTACK_BAND
    // with 0x0fdb0000 granule-aligned, i.e. unambiguously a kstack_alloc() result.
    //
    // So ask which it is, and REFUSE the orphan rather than laundering it through
    // the heap allocator. Refusing leaks the granule, which is the correct trade:
    // the pointer has already been freed once, so freeing it again would hand
    // live memory to a second owner. The report names the caller for addr2line.
    {
        // base-8 is the top word of what would be this block's guard band. It is
        // at least as safe to read as base-32, which is what kfree() reads on the
        // very next line of the old code, so this adds no new fault risk.
        uint64_t below = (u >= 8) ? *(volatile uint64_t *)(u - 8) : 0;
        int recent;
        { uint64_t __fl = spinlock_acquire_irqsave(&g_ks_lock);
          recent = kstack_hist_has(u) ? 1 : 0;   // the ring is shared state
          spinlock_release_irqrestore(&g_ks_lock, __fl); }
        int32_t v = kstack_foreign_verdict_rs(u, KSTACK_GRAN, KSTACK_BAND, below,
                                              KSTACK_POISON, recent);
        if (v == KSTACK_VERDICT_ORPHAN && g_kstack_orphan_refuse) {
            g_ks_free_orphan++;
            kprintf("[KSTACK] ORPHAN FREE of kernel stack %p from ra=%p: a guarded "
                    "stack with NO registry row, i.e. freed twice or its "
                    "registration was lost. REFUSED (the granule leaks; freeing it "
                    "again would give live memory a second owner). This is the "
                    "defect that used to present as '[HEAP] kfree() on an INVALID "
                    "pointer ... magic=0xbaadb4dd'. addr2line the ra against "
                    "kernel.dbg.elf.\n",
                    base, __builtin_return_address(0));
            bootlog_fault_write("[KSTACK] ORPHAN FREE base=0x%lx ra=0x%lx "
                                "(kernel stack freed twice)",
                                u, (uint64_t)__builtin_return_address(0));
            return;
        }
    }
    // Genuinely foreign: a counted kmalloc fallback. kfree() validates its own
    // magic, so a wild pointer is still caught there rather than here.
    g_ks_free_foreign++;
    kfree(base);
}

// #heapinvptr: the fallback census, readable by a self-test.
uint64_t kstack_free_foreign_count(void) { return g_ks_free_foreign; }
uint64_t kstack_free_orphan_count(void)  { return g_ks_free_orphan; }
uint64_t kstack_guarded_count(void)      { return g_ks_guarded; }
uint64_t kstack_freed_count(void)        { return g_ks_freed; }
uint64_t kstack_reg_anomaly_count(void)  { return g_ks_reg_anomaly; }

int kstack_is_guarded(const void *base) {
    uint64_t u = (uint64_t)base;
    uint64_t __fl = spinlock_acquire_irqsave(&g_ks_lock);
    int r = (kstackreg_find_rs(g_ks, KSTACK_MAX, u) >= 0) ? 1 : 0;
    spinlock_release_irqrestore(&g_ks_lock, __fl);
    return r;
}

void kstack_overflow_panic(uint64_t rsp, uint64_t rip) {
    uint64_t block = rsp & ~((uint64_t)KSTACK_GRAN - 1);
    // kprintf_nolock, not kprintf: the panic path must not take the console
    // lock (cpu/idt.c documents the machine-wide silent hang that causes), and
    // bootlog_fault_write is the only way this reaches a machine with no serial
    // port - which is both of the owner's.
    kprintf_nolock("\n[KSTACK] KERNEL STACK OVERFLOW: rsp=0x%lx has descended "
                   "into the guard band of kernel stack block [0x%lx,0x%lx) "
                   "(usable [0x%lx,0x%lx), band %u bytes). rip=0x%lx. Caught at "
                   "interrupt entry, BEFORE anything outside this stack was "
                   "written: without this band that store would have corrupted "
                   "the next allocation silently and surfaced later, elsewhere, "
                   "as a mangled return address.\n",
                   (unsigned long)rsp, (unsigned long)block,
                   (unsigned long)(block + KSTACK_GRAN),
                   (unsigned long)(block + KSTACK_BAND),
                   (unsigned long)(block + KSTACK_GRAN),
                   (unsigned)KSTACK_BAND, (unsigned long)rip);
    bootlog_fault_write("[KSTACK] KERNEL STACK OVERFLOW rsp=0x%lx block=0x%lx "
                        "usable=0x%lx rip=0x%lx",
                        rsp, block, block + KSTACK_BAND, rip);
    kpanic_halt();
}

// ===========================================================================
// PROVING THE GUARD, IN TWO PARTS.
//
// Part 1, kstack_selftest(), runs on EVERY boot INCLUDING the golden and is
// non-destructive. It asks the questions the one-AND check depends on: is the
// block really aligned to its own size, is the band really poisoned, is the
// magic really at the bottom, and does kstack_rsp_in_band() actually return 1
// for an address in the band and 0 for one just above it. That last pair is the
// negative control: a check that says "yes" to everything protects nothing, and
// a check that says "no" to everything is the failure this project has shipped
// several times.
//
// Part 2 is `make KSTACKTEST=1`, and it is the one that has actually been SEEN
// to fire. A guard nobody has watched trigger is a guard nobody knows works.
// Same argument and same shape as `make NOBLOCKTEST=1`. NEVER for a golden.
// ===========================================================================
void kstack_selftest(void) {
    void *b = kstack_alloc(KSTACK_USABLE);
    if (!b) {
        selftest_notrun("KSTACK", "kstack_alloc failed; the guard is untested this boot");
        return;
    }
    if (!kstack_is_guarded(b)) {
        selftest_notrun("KSTACK", "allocation fell back to kmalloc (unguarded); "
                                  "see the fallback counters on the [KSTACK] line");
        kstack_free(b, KSTACK_USABLE);
        return;
    }
    uint64_t usable = (uint64_t)b;
    uint64_t block  = usable - KSTACK_BAND;

    int aligned  = ((block & ((uint64_t)KSTACK_GRAN - 1)) == 0);
    int magic_ok = (*(volatile uint64_t *)block == KSTACK_MAGIC);
    int poison_ok = 1;
    for (uint32_t i = 1; i < KSTACK_BAND / 8; i++)
        if (((volatile uint64_t *)block)[i] != KSTACK_POISON) { poison_ok = 0; break; }

    // The two that matter: the check must fire INSIDE the band and must not
    // fire one byte above it.
    int fires_in_band   = kstack_rsp_in_band(block + KSTACK_BAND - 8) ? 1 : 0;
    int quiet_above     = kstack_rsp_in_band(usable + 64) ? 0 : 1;

    // #heapinvptr: and prove the orphan-identification predicate on THIS build,
    // including its negative controls. A non-zero mask names which arm broke.
    uint32_t idmask = kstackid_selftest_rs();

    // And prove the REGISTRY ARBITRATION on this build, including its negative
    // controls. Case 0 is the defect itself: two claims with no commit between
    // them must get different rows, which the pre-fix scan does not do.
    // `make KSTACKREGTEST=1` compiles that pre-fix scan back in and this mask
    // must then be non-zero.
    uint32_t regmask = kstackreg_selftest_rs();

    // Two independent claims, reported separately. The guard-band claim is
    // "kernel stacks are protected"; the predicate claim is "an orphaned kernel
    // stack is identified rather than laundered into kfree()". Collapsing them
    // into one word would make `make KSTACKIDTEST=1` print that the guard band is
    // broken, which it is not, and a self-test that misattributes its own failure
    // sends the next reader to the wrong file.
    int guard_ok = aligned && magic_ok && poison_ok && fires_in_band && quiet_above;
    int ok = guard_ok && (idmask == 0) && (regmask == 0);
    kprintf("[KSTACK] selftest %s: block=0x%lx gran=%u band=%u aligned=%d "
            "magic=%d poison=%d fires_in_band=%d quiet_above_band=%d "
            "orphan_id_mask=0x%x registry_mask=0x%x\n",
            ok ? "PASS" : "FAILED", (unsigned long)block,
            (unsigned)KSTACK_GRAN, (unsigned)KSTACK_BAND,
            aligned, magic_ok, poison_ok, fires_in_band, quiet_above,
            (unsigned)idmask, (unsigned)regmask);
    if (!guard_ok)
        kprintf("[KSTACK] selftest FAILED on the GUARD BAND: kernel stacks are NOT "
                "protected. Treat the guard claim as false until this reads PASS.\n");
    if (regmask != 0)
        kprintf("[KSTACK] selftest FAILED on the REGISTRY ARBITRATION (mask=0x%x, "
                "rustkern/kstackreg.rs): bit0 is the race itself (two claims got "
                "the SAME row, so one registration would be lost and that "
                "stack's free would be reported as an orphan). EXPECTED only "
                "under `make KSTACKREGTEST=1`.\n", (unsigned)regmask);
    if (idmask != 0)
        kprintf("[KSTACK] selftest FAILED on the ORPHAN PREDICATE (mask=0x%x, "
                "rustkern/kstackid.rs): the guard band is fine, but a kernel stack "
                "freed twice would be handed to kfree() again and reported as a bad "
                "HEAP pointer. EXPECTED only under `make KSTACKIDTEST=1`.\n",
                (unsigned)idmask);
    kstack_free(b, KSTACK_USABLE);
}

#ifdef KSTACK_OVERFLOW_SELFTEST
static volatile uint64_t g_ks_sink;

// The bound is VOLATILE and set to something the recursion never reaches.
// -Werror=infinite-recursion (gcc 12) rejects a literally unconditional
// self-call, and it is right to everywhere except here. Reading the bound
// through a volatile means the compiler cannot prove the call never returns, so
// it compiles, while at run time the test is always true and the recursion is
// genuinely unbounded. The work AFTER the call stops a tail-call rewrite from
// turning this into a loop that never grows the stack at all - which would
// leave the self-test passing without having tested anything.
static volatile uint64_t g_ks_recurse_bound = 0xFFFFFFFFULL;

// PACING, AND WHY THE TEST IS WRONG WITHOUT IT.
//
// The guard is evaluated at INTERRUPT ENTRY. An unpaced recursion descends far
// faster than interrupts arrive, so it steps over its own 4 KiB band between
// two checks and is caught only when it eventually lands in some LATER block's
// band. That is not a theory: the first run of this test armed a stack at
// 0x3eff1000 and was caught at rsp=0x3efb0150, in the band of a DIFFERENT block
// 256 KiB lower down, having silently crossed everything in between. See
// blame.md - it is the measured limit of a software guard and the reason an MMU
// guard page is still the right long-term answer.
//
// The defect this guard exists for adds EXACTLY ONE FRAME PER INTERRUPT (a
// nested ISR frame lands, contends for the BKL, and the next tick lands another
// on top). So the test must descend at that rate to exercise the real regime.
// The delay is the SHARED mono_busy_delay_us(), not a hand-rolled spin, and it
// is longer than the 4 ms tick period so at least one interrupt is guaranteed
// per frame.
#define KSTACK_TEST_FRAME_US 6000ull

static void kstack_overflow_recurse(uint64_t depth) {
    volatile uint64_t pad[32];      // 256 bytes of frame, actually written
    pad[0] = depth;
    pad[31] = depth;
    g_ks_sink += pad[0] + pad[31];
    mono_busy_delay_us(KSTACK_TEST_FRAME_US);   // one tick or more per frame
    if (depth < g_ks_recurse_bound) kstack_overflow_recurse(depth + 1);
    g_ks_sink += pad[0];
}

void kstack_overflow_selftest(void) {
    // IT MUST RECURSE ON A GUARDED STACK, AND THAT IS THE WHOLE POINT.
    //
    // The obvious version calls the recursion directly, which runs it on
    // whatever stack the caller is already using - here the 64 KiB BSP boot
    // stack in entry.asm, which is .bss and has no band. That test would
    // "succeed" by corrupting .bss exactly the way the real defect corrupts the
    // heap, proving nothing and quite possibly reporting an unrelated fault. A
    // self-test that exercises the UNPROTECTED path is worse than none, because
    // it produces a green tick for the wrong reason.
    //
    // So: take a real stack from the allocator under test, switch RSP onto it,
    // and recurse there. This does not return.
    void *b = kstack_alloc(KSTACK_USABLE);
    if (!b || !kstack_is_guarded(b)) {
        selftest_notrun("KSTACK-OVERFLOW", "no guarded stack available; "
                                          "the deliberate overflow did NOT run");
        if (b) kstack_free(b, KSTACK_USABLE);
        return;
    }
    uint64_t lo  = (uint64_t)b;
    uint64_t top = (lo + KSTACK_USABLE) & ~0xFULL;
    kprintf("[KSTACK] OVERFLOW SELFTEST ARMED (make KSTACKTEST=1). Switching to "
            "guarded stack [0x%lx,0x%lx), band [0x%lx,0x%lx), then deliberately "
            "running it off the low end at one frame per timer tick, which is "
            "the rate the real defect grows at. EXPECTED: [KSTACK] KERNEL STACK "
            "OVERFLOW naming THIS block. If it names a different block the guard "
            "was outrun; if it keeps running, the guard is NOT working.\n",
            (unsigned long)lo, (unsigned long)top,
            (unsigned long)(lo - KSTACK_BAND), (unsigned long)lo);

    // Through a volatile pointer so nothing is inlined into a frame that would
    // then be sitting on the wrong stack.
    static void (*volatile fn)(uint64_t) = kstack_overflow_recurse;
    __asm__ volatile("mov %[top], %%rsp\n\t"
                     "xor %%ebp, %%ebp\n\t"
                     "xor %%edi, %%edi\n\t"
                     "call *%[f]\n\t"
                     :: [top] "r"(top), [f] "r"(fn) : "memory");

    kprintf("[KSTACK] OVERFLOW SELFTEST FAILED: the recursion RETURNED. The "
            "guard did not fire; kernel stacks are unprotected.\n");
}
#endif

void kstack_report(void) {
    kprintf("[KSTACK] guarded=%lu live=%u freed=%lu gran=%u band=%u | "
            "multigran=%lu | fallback: toobig=%lu nomem=%lu full=%lu align=%lu "
            "| free_foreign=%lu free_orphan=%lu reg_anomaly=%lu\n",
            (unsigned long)g_ks_guarded, (unsigned)g_ks_live,
            (unsigned long)g_ks_freed, (unsigned)KSTACK_GRAN, (unsigned)KSTACK_BAND,
            (unsigned long)g_ks_multigran, (unsigned long)g_ks_fb_toobig, (unsigned long)g_ks_fb_nomem,
            (unsigned long)g_ks_fb_full, (unsigned long)g_ks_fb_align,
            (unsigned long)g_ks_free_foreign, (unsigned long)g_ks_free_orphan,
            (unsigned long)g_ks_reg_anomaly);
}
