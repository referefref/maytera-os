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

// Census. Each counter is a reason a stack is NOT protected, so they are all
// printed even when zero: "0 unguarded" is the claim worth being able to make.
static uint64_t g_ks_guarded, g_ks_freed;
static uint64_t g_ks_fb_toobig, g_ks_fb_nomem, g_ks_fb_full, g_ks_fb_align;
static uint64_t g_ks_multigran;   // stacks that needed more than one granule
static uint64_t g_ks_free_foreign;

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

    uint32_t slot;
    for (slot = 0; slot < KSTACK_MAX; slot++) if (g_ks[slot].usable == 0) break;
    if (slot == KSTACK_MAX) { g_ks_fb_full++; return kmalloc(size); }

    // pmm_alloc_pages() guarantees 4 KiB alignment, not KSTACK_GRAN.
    // Over-allocate by one granule minus one page so a KSTACK_GRAN-aligned run
    // of the required length is guaranteed to exist inside, then hand the head
    // and the tail straight back. Net cost is exactly the rounded-up size; the
    // over-allocation is transient.
    uint64_t need = grans * KSTACK_PAGES;
    uint64_t want = need + (KSTACK_PAGES - 1);
    uint64_t raw = pmm_alloc_pages(want);
    if (raw == 0) { g_ks_fb_nomem++; return kmalloc(size); }

    uint64_t rawend = raw + want * KSTACK_PAGE;
    uint64_t block = (raw + KSTACK_GRAN - 1) & ~((uint64_t)KSTACK_GRAN - 1);
    if (block < raw || block + need * KSTACK_PAGE > rawend) {
        // Cannot happen with the over-allocation above; counted rather than
        // asserted so a future change to KSTACK_GRAN cannot fail silently.
        pmm_free_pages(raw, want);
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

    g_ks[slot].usable = block + KSTACK_BAND;
    g_ks[slot].block  = block;
    g_ks[slot].pages  = need;
    g_ks_live++; g_ks_guarded++;
    return (void *)(block + KSTACK_BAND);
}

void kstack_free(void *base, size_t size) {
    (void)size;
    if (!base) return;
    uint64_t u = (uint64_t)base;
    for (uint32_t i = 0; i < KSTACK_MAX; i++) {
        if (g_ks[i].usable != u) continue;
        // Clear the magic BEFORE the frames go back to the PMM. Skip this and a
        // reallocated frame still says "I am a kernel stack" to the check in
        // isr_handler(), which would then police an unrelated allocation and
        // could panic a perfectly healthy machine.
        *(volatile uint64_t *)g_ks[i].block = 0;
        pmm_free_pages(g_ks[i].block, g_ks[i].pages);
        g_ks[i].usable = 0; g_ks[i].block = 0; g_ks[i].pages = 0;
        if (g_ks_live) g_ks_live--;
        g_ks_freed++;
        return;
    }
    // Not ours: a fallback allocation from kmalloc. kfree() validates its own
    // magic, so a genuinely foreign pointer is caught there rather than here.
    g_ks_free_foreign++;
    kfree(base);
}

int kstack_is_guarded(const void *base) {
    uint64_t u = (uint64_t)base;
    for (uint32_t i = 0; i < KSTACK_MAX; i++) if (g_ks[i].usable == u) return 1;
    return 0;
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

    int ok = aligned && magic_ok && poison_ok && fires_in_band && quiet_above;
    kprintf("[KSTACK] selftest %s: block=0x%lx gran=%u band=%u aligned=%d "
            "magic=%d poison=%d fires_in_band=%d quiet_above_band=%d\n",
            ok ? "PASS" : "FAILED", (unsigned long)block,
            (unsigned)KSTACK_GRAN, (unsigned)KSTACK_BAND,
            aligned, magic_ok, poison_ok, fires_in_band, quiet_above);
    if (!ok)
        kprintf("[KSTACK] selftest FAILED means kernel stacks are NOT protected. "
                "Treat the guard claim as false until this reads PASS.\n");
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
            "| free_foreign=%lu\n",
            (unsigned long)g_ks_guarded, (unsigned)g_ks_live,
            (unsigned long)g_ks_freed, (unsigned)KSTACK_GRAN, (unsigned)KSTACK_BAND,
            (unsigned long)g_ks_multigran, (unsigned long)g_ks_fb_toobig, (unsigned long)g_ks_fb_nomem,
            (unsigned long)g_ks_fb_full, (unsigned long)g_ks_fb_align,
            (unsigned long)g_ks_free_foreign);
}
