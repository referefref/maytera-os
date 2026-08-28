// fpu_ymm_test.c - #745 (local 107): does a context switch preserve YMM?
//
// WHY THIS EXISTS
// ---------------
// proc/context_switch.asm saved FPU state with fxsave64/fxrstor64 (#588/#446).
// fxsave64 saves x87 and SSE, i.e. XMM0-15. It DOES NOT save the upper 128
// bits of YMM0-15. On a CPU where AVX is enabled, every context switch
// therefore dropped half of every 256-bit register: silent, non-deterministic
// corruption, no fault, nothing to trace.
//
// A fix that is only ever seen to PASS is indistinguishable from no fix, so
// this is built to go RED on demand:
//
//   make YMMTEST=1     xsave64 path (the fix)      -> expect RESULT: PASS
//   make YMMTEST=red   AVX enabled, fxsave64 path  -> expect RESULT: FAIL
//
// YMMTEST=red is not a synthetic mutation of the test; it is the real bug.
// cpu/sse.c leaves CR4.OSXSAVE set and XCR0 = x87|SSE|AVX (so AVX genuinely
// executes) and then forces g_fpu_use_xsave = 0, which is precisely the state
// a machine is in when its firmware handed us CR4.OSXSAVE already set - the
// one way this defect could have been LIVE rather than latent on hardware we
// cannot inspect.
//
// WHAT IS MEASURED
// ----------------
// Four kernel threads each own a 32-byte pattern whose LOW 16 bytes (which
// fxsave64 does save, as XMM) and HIGH 16 bytes (which it does not, as
// YMM_Hi128) are different and carry the thread id and the round number. Each
// round a thread fills all sixteen YMM registers with its pattern, sleeps one
// tick on a wait queue nothing ever wakes - the canonical primitive, not a
// yield spin, because a test that breaks the rule it is testing is worthless -
// and then reads all sixteen back and compares.
//
// The two halves are counted SEPARATELY on purpose. The expected RED signature
// is not "it broke": it is low_mismatches == 0 with high_mismatches > 0, i.e.
// the exact 128 bits fxsave64 omits and no others. Anything else means the
// test found a different bug and the result must not be read as this one.
#ifdef FPU_YMM_SELFTEST

#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../cpu/sse.h"
#include "process.h"
#include "../sync/waitq.h"

#define YMM_THREADS 4
#define YMM_ROUNDS  60
#define YMM_REGS    16

extern void ymm_fill(const void *pat32);
extern void ymm_dump(void *out);

static wait_queue_head_t ymm_done_wq  = { .head = NULL, .lock = SPINLOCK_INIT };
// A queue with no waker at all. wait_event_timeout() on a permanently false
// condition is how a thread gives up the CPU here: it is a real deschedule
// through the scheduler, which is the whole point, and it is bounded.
static wait_queue_head_t ymm_park_wq  = { .head = NULL, .lock = SPINLOCK_INIT };

static volatile int      ymm_done      = 0;
static volatile uint64_t ymm_low_bad   = 0;
static volatile uint64_t ymm_high_bad  = 0;
static volatile uint64_t ymm_rounds    = 0;
static volatile uint64_t ymm_alloc_err = 0;

static void ymm_worker(void *arg) {
    int id = (int)(long)arg;
    uint8_t pat[32];
    uint8_t *out = (uint8_t *)kmalloc(YMM_REGS * 32);
    if (!out) { __sync_fetch_and_add(&ymm_alloc_err, 1); goto done; }

    for (int r = 0; r < YMM_ROUNDS; r++) {
        for (int i = 0; i < 16; i++) pat[i]      = (uint8_t)(0x10 + id);
        for (int i = 0; i < 16; i++) pat[16 + i] = (uint8_t)(0xA0 + id * 16 + r);

        ymm_fill(pat);
        wait_event_timeout(&ymm_park_wq, 0, 1);   // forces a real switch
        ymm_dump(out);

        for (int reg = 0; reg < YMM_REGS; reg++) {
            const uint8_t *g = out + reg * 32;
            for (int b = 0;  b < 16; b++)
                if (g[b] != pat[b]) __sync_fetch_and_add(&ymm_low_bad, 1);
            for (int b = 16; b < 32; b++)
                if (g[b] != pat[b]) __sync_fetch_and_add(&ymm_high_bad, 1);
        }
        __sync_fetch_and_add(&ymm_rounds, 1);
    }
    kfree(out);

done:
    __sync_fetch_and_add(&ymm_done, 1);
    wake_up_all(&ymm_done_wq);
    proc_exit(0);
}

static void ymm_launcher(void *arg) {
    (void)arg;

    // Do not execute a VEX instruction on a machine where it would #UD. A
    // kvm64 guest masks AVX entirely, so this test can only run on a VM with
    // cpu=host (or kvm64,+avx) and on real hardware.
    if (!g_fpu_xcr0 || !(g_fpu_xcr0 & XCR0_AVX)) {
        kprintf("[YMMTEST] SKIPPED: AVX is not enabled on this CPU "
                "(xcr0=0x%lx cr4=0x%lx). No YMM state can exist, which is the "
                "other safe state - not a pass.\n",
                g_fpu_xcr0, read_cr4());
        proc_exit(0);
    }

    kprintf("[YMMTEST] %d threads x %d rounds x %d regs; xcr0=0x%lx save=%s\n",
            YMM_THREADS, YMM_ROUNDS, YMM_REGS, g_fpu_xcr0,
            g_fpu_use_xsave ? "xsave64" : "fxsave64");
#ifdef FPU_YMM_SELFTEST_RED
    kprintf("[YMMTEST] *** RED BUILD: AVX enabled, fxsave64 save path. "
            "This build MUST FAIL, with low=0 and high>0. ***\n");
#endif

    for (int i = 0; i < YMM_THREADS; i++) {
        char nm[12];
        strcpy(nm, "ymmw0");
        nm[4] = (char)('0' + i);
        if (proc_create(nm, ymm_worker, (void *)(long)i, PRIO_NORMAL) < 0) {
            kprintf("[YMMTEST] proc_create(%s) FAILED\n", nm);
            __sync_fetch_and_add(&ymm_done, 1);
        }
    }

    wait_event(&ymm_done_wq, ymm_done >= YMM_THREADS);

    uint64_t lo = ymm_low_bad, hi = ymm_high_bad, rd = ymm_rounds;
    int pass = (lo == 0) && (hi == 0) && (ymm_alloc_err == 0) &&
               (rd == (uint64_t)YMM_THREADS * YMM_ROUNDS);

    kprintf("[YMMTEST] rounds=%lu/%d low128_mismatches=%lu "
            "high128_mismatches=%lu alloc_errors=%lu\n",
            (unsigned long)rd, YMM_THREADS * YMM_ROUNDS,
            (unsigned long)lo, (unsigned long)hi,
            (unsigned long)ymm_alloc_err);
    kprintf("[YMMTEST] RESULT: %s\n", pass ? "PASS" : "FAIL");
    proc_exit(0);
}

// Called from main.c once the scheduler is live.
void fpu_ymm_selftest_start(void) {
    proc_create("ymmtest", ymm_launcher, NULL, PRIO_NORMAL);
}

#endif // FPU_YMM_SELFTEST
