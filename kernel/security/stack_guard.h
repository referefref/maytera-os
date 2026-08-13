// stack_guard.h - kernel stack canary protection for MayteraOS.
//
// #646 HONESTY PASS (2026-08-05). Measured state, by grepping the whole kernel
// tree for call sites:
//
//   LIVE - the GCC stack protector. The kernel is built with
//          -fstack-protector-strong -mstack-protector-guard=global (kernel/
//          Makefile), so every protected function's epilogue compares against
//          __stack_chk_guard and calls __stack_chk_fail() on a mismatch.
//   LIVE - stack_guard_init(), called from kernel_main via
//          security_canary_init() (security.c), installs a per-boot random
//          guard before almost anything else runs.
//   LIVE - security_canary_selftest(), armed by `make CANARYTEST=1`, is a real
//          proof-of-fire: it overflows a 16-byte buffer and the canary must
//          kpanic. Never armed in a golden.
//
// DELETED in this pass, all with ZERO callers, all of them implying protection
// that did not exist:
//   stack_guard_t, stack_guard_init_thread(), stack_guard_check(),
//   stack_guard_generate_canary()  - a whole per-thread canary scheme. Nothing
//       ever created a stack_guard_t, so stack_guard_check() was never called
//       and the "Checks performed" statistic was structurally always 0.
//       stack_guard_generate_canary() was also a SECOND, divergent canary
//       generator: stack_guard_init() does not use it, it uses
//       sec_canary_generate_rs() (rustkern/seccore.rs). Two generators means
//       two behaviours to keep in sync, for a value only one of them produces.
//   stack_guard_fail(), stack_guard_set_handler(), stack_overflow_handler_t
//       - a second, WORSE fatal path: a private kprintf-and-spin that neither
//       kpanic()s nor persists to /PANIC.TXT. It was declared noreturn in this
//       header, so any future caller would have silently got the bad handler
//       instead of __stack_chk_fail(). There is now exactly one stack-smash
//       exit and it is the one GCC calls.
//   stack_guard_setup_page(), stack_guard_remove_page(),
//   stack_guard_is_guard_page() - guard pages. Zero callers; remove_page() was
//       an explicit no-op ("Would need to remap") and is_guard_page() was a
//       guess ("For now, check if address is unmapped"). No stack in this
//       kernel has a guard page. SECURITY_FEATURE_GUARD_PAGES is correspondingly
//       never set.
//   stack_guard_get_usage(), stack_guard_get_usage_percent(),
//   stack_guard_near_overflow() - stack usage monitoring over the deleted
//       stack_guard_t.
//   stack_guard_get_canary(), stack_guard_set_flags(), stack_guard_get_flags()
//       and the STACK_GUARD_* / STACK_CANARY_{TERMINATOR,NEWLINE,CR} flags -
//       configuration for the deleted features.
#ifndef SECURITY_STACK_GUARD_H
#define SECURITY_STACK_GUARD_H

#include "../types.h"

// Compile-time placeholder. Replaced with a 64-bit random value by
// stack_guard_init() before any protected frame that outlives the call exists.
#define STACK_CANARY_MAGIC      0xDEADBEEFCAFEBABEULL

// ============================================================================
// GCC Stack Protector Support
// ============================================================================

// Referenced by GCC's -fstack-protector-strong -mstack-protector-guard=global.
// The name is fixed by the compiler.
extern uint64_t __stack_chk_guard;

// Called by GCC when a protected function's epilogue sees a canary mismatch.
extern void __stack_chk_fail(void) __attribute__((noreturn));

/**
 * Install the real per-boot canary into __stack_chk_guard.
 *
 * MUST be called from a chain of no_stack_protector frames only: see the long
 * comment on the definition in stack_guard.c. Today that chain is
 * entry.asm -> kernel_main -> security_canary_init -> here.
 */
void stack_guard_init(void);

/**
 * Print the measured stack-protection posture.
 */
void stack_guard_print_info(void);

#endif // SECURITY_STACK_GUARD_H
