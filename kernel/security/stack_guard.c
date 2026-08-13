// stack_guard.c - kernel stack canary protection for MayteraOS.
// See stack_guard.h for the measured statement of what is live, and for the
// list of zero-caller machinery deleted in the #646 honesty pass.
#include "stack_guard.h"
#include "../serial.h"
#include "../fs/panic.h"

// ============================================================================
// Module State
// ============================================================================

// Global canary value (used by GCC's -fstack-protector-strong).
uint64_t __stack_chk_guard = STACK_CANARY_MAGIC;

static bool g_stack_guard_initialized = false;
static uint64_t g_stack_smashes_detected = 0;

// ============================================================================
// Initialization
// ============================================================================

// #624 step 2. Until build 996 this function had no callers (it ran only from
// security_init(), which itself had none), AND the kernel was compiled
// -fno-stack-protector, so even a perfect canary would have been checked by
// nothing. Both halves are now fixed: the Makefile builds with
// -fstack-protector-strong -mstack-protector-guard=global, and this runs.
//
// MUST BE no_stack_protector, AND SO MUST EVERY FRAME LIVE BELOW IT.
// GCC's epilogue RE-READS __stack_chk_guard and compares it against the copy
// the prologue saved (verified in the emitted code: `mov __stack_chk_guard(%rip)`
// on entry, `sub __stack_chk_guard(%rip)` on exit). Any protected function that
// is already on the stack when the global changes will therefore compare an old
// saved copy against the new global and trip a FALSE stack-smash panic. The
// live chain at flip time is entry.asm -> kernel_main -> security_canary_init
// -> here, so kernel_main and security_canary_init carry the same attribute.
// Changing where this is called from means re-checking that chain.
__attribute__((no_stack_protector))
void stack_guard_init(void) {
    extern uint64_t sec_canary_generate_rs(void);
    extern uint32_t sec_entropy_source_rs(void);

    extern void sec_mark_canary_live_rs(void);
    __stack_chk_guard = sec_canary_generate_rs();
    sec_mark_canary_live_rs();
    g_stack_guard_initialized = true;

    static const char *const src[3] = { "TSC-JITTER (no RDRAND/RDSEED)",
                                        "RDRAND", "RDSEED" };
    uint32_t s = sec_entropy_source_rs();
    kprintf("[CANARY] __stack_chk_guard = 0x%016lx (entropy: %s)\n",
            __stack_chk_guard, src[s < 3 ? s : 0]);
}

// ============================================================================
// Stack Smashing Handler
// ============================================================================

// GCC stack protector failure function. THIS IS THE ONLY ENTRY POINT: with
// -fstack-protector-strong every protected function's epilogue calls it on a
// canary mismatch.
//
// #480/#624: routed through kpanic(), the ONE canonical fatal primitive, rather
// than a private kprintf-and-spin loop. kpanic() logs to serial AND persists to
// /PANIC.TXT, so a stack smash on real hardware leaves evidence behind instead
// of a silent frozen screen.
//
// #646: the rival stack_guard_fail() that did the kprintf-and-spin is deleted.
// One smash, one exit.
__attribute__((no_stack_protector))
void __stack_chk_fail(void) {
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    g_stack_smashes_detected++;

    // __builtin_return_address(0) is the protected function that caught it,
    // which is the single most useful thing to know: run it through addr2line.
    kpanic("STACK SMASHING DETECTED: canary mismatch in caller %p (rsp=%p, "
           "expected guard 0x%016llx). A kernel stack buffer was overflowed.",
           __builtin_return_address(0), (void *)rsp, __stack_chk_guard);
}

#ifdef CANARY_SELFTEST
// #624 step 2 PROOF-OF-FIRE. Armed only by `make CANARYTEST=1`, never in a
// golden. Deliberately overflows a 16-byte stack buffer by 64 bytes so the
// canary can be OBSERVED catching it. The length comes through a volatile
// global specifically to defeat GCC's interprocedural constant propagation,
// which would otherwise see the overflow at compile time and reject the build
// under -Werror=array-bounds instead of letting it happen at runtime.
volatile unsigned g_canary_overflow_len = 80;

__attribute__((noinline))
static void canary_victim(const volatile char *src, unsigned n) {
    char buf[16];
    for (unsigned i = 0; i < n; i++) buf[i] = src[i];
    __asm__ volatile("" :: "r"(buf) : "memory");
}

void security_canary_selftest(void) {
    volatile char pattern[96];
    for (int i = 0; i < 96; i++) pattern[i] = (char)('A' + (i % 26));
    kprintf("[CANARY-TEST] arming a deliberate 16-byte buffer overflow (+%u bytes)\n",
            g_canary_overflow_len - 16);
    kprintf("[CANARY-TEST] if the canary works, the NEXT line is a kpanic\n");
    canary_victim(pattern, g_canary_overflow_len);
    kprintf("[CANARY-TEST] *** FAILED *** overflow was NOT caught, canary is INERT\n");
}
#endif

// ============================================================================
// Reporting
// ============================================================================

void stack_guard_print_info(void) {
    kprintf("[STACK_GUARD] Stack protection posture (measured, #646):\n");
    kprintf("  Compiler:         -fstack-protector-strong, guard=global\n");
    kprintf("  Per-boot canary:  %s\n", g_stack_guard_initialized ? "installed" : "NOT INSTALLED");
    kprintf("  Global canary:    0x%016lx\n", __stack_chk_guard);
    kprintf("  Smashes caught:   %lu\n", g_stack_smashes_detected);
    kprintf("  Per-thread canary: not implemented\n");
    kprintf("  Stack guard pages: not implemented\n");
    kprintf("  User-stack canary: not implemented (Ring-3 apps carry their own,\n");
    kprintf("                     built by the userland toolchain)\n");
}
