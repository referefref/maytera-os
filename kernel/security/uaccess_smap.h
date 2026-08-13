// uaccess_smap.h - #645 SMAP (Supervisor Mode Access Prevention) AC bracketing.
//
// WHAT SMAP DOES, AND WHY THIS FILE IS DELICATE
// ---------------------------------------------
// With CR4.SMAP set, ANY Ring-0 read or write to a page that is user-accessible
// (U/S=1 at every paging level) raises #PF, unless RFLAGS.AC is set. AC is
// therefore a per-instant, global "SMAP is off right now" switch. Every window
// in which AC is set is a window in which the single strongest user->kernel
// escalation barrier this kernel has is DISABLED. So the rules are:
//
//   1. The bracket goes around the ACCESS, never around a function. In this
//      kernel the access is `rep movsb` / the byte loop inside mm/uaccess.asm,
//      and that is where the AC set/clear lives. Nothing in C should need these
//      helpers for a normal user copy: use copy_from_user()/copy_to_user()/
//      strncpy_from_user()/strnlen_user()/clear_user(), which already bracket.
//   2. AC must NEVER survive a return to Ring 3 or leak into unrelated kernel
//      code. Three independent mechanisms enforce that; see "THE THREE EXITS".
//   3. The bracket must NEST. The deleted #646 pair (smap_disable/smap_restore)
//      got this exactly wrong: it ASSUMED AC was clear on entry rather than
//      reading it, so an inner "restore" re-armed SMAP while an outer copy was
//      still mid-flight, turning the outer copy into a fault. This pair reads
//      the live state and an inner bracket is a no-op.
//
// THE THREE EXITS (why AC cannot leak)
// ------------------------------------
//   a. SYSCALL entry: IA32_FMASK carries bit 18 (AC), so the CPU clears AC in
//      hardware on every SYSCALL. This is not optional hardening: AC is
//      writable from CPL 3, so without it a Ring-3 process could simply
//      `pushfq; or $1<<18; popfq; syscall` and run the ENTIRE syscall with SMAP
//      disabled. See proc/syscall.c MSR_SFMASK.
//   b. Interrupt/exception entry: the CPU does NOT clear AC on an IDT gate, so
//      cpu/idt.asm's isr_common issues CLAC as its first act. The saved RFLAGS
//      on the stack is untouched, so IRETQ correctly restores the interrupted
//      context's AC and a copy interrupted mid-flight resumes with AC set.
//   c. The fault-fixup path: mm/fault.c rewrites RIP to a primitive's
//      *_ex_fixup label, which SKIPS the normal CLAC after *_ex_end. Each fixup
//      therefore clears AC itself. Miss that and every -EFAULT would leave the
//      rest of the syscall running with SMAP off, which is the loudest possible
//      version of the bug this file exists to prevent.
//
// WHY THE RUNTIME GATE EXISTS
// ---------------------------
// STAC and CLAC raise #UD when CPUID.(EAX=7,ECX=0):EBX.SMAP[bit 20] is 0. They
// do NOT require CR4.SMAP to be set, but they DO require the CPU to know the
// instructions at all. This kernel must keep booting on a CPU or hypervisor
// without SMAP (and with /NOSMAP.TXT, and under -DCONFIG_NO_SMAP), so every
// STAC/CLAC is gated on g_smap_active, which is set ONLY after the CR4 write
// has been READ BACK as taken. The gate is one predictable byte load per copy
// CALL, not per byte, so it does not sit in any inner loop.
#ifndef SECURITY_UACCESS_SMAP_H
#define SECURITY_UACCESS_SMAP_H

#include "../types.h"

// RFLAGS.AC. Named once, here, so no caller re-derives the bit number.
#define SMAP_RFLAGS_AC          (1UL << 18)

// Non-zero if and only if SMAP is OBSERVABLY live in this running kernel: the
// CPU reports the feature, config did not turn it off, the CR4.SMAP write was
// issued AND the bit read back set. Owned by security_init() (security.c),
// written exactly once during single-threaded early boot and read-only after.
// Also read from assembly: mm/uaccess.asm and cpu/idt.asm reference it as
// `[rel g_smap_active]`, so it must stay a plain byte-sized object.
extern volatile uint8_t g_smap_active;

// Opaque token returned by uaccess_begin() and consumed by uaccess_end().
// It deliberately does NOT carry the old RFLAGS: it carries only "did WE set
// AC, and are we therefore the bracket that must clear it". Encoding the
// DECISION rather than the observed state makes the pair immune to
// g_smap_active changing between begin and end.
typedef unsigned long uaccess_ac_t;

#define UACCESS_AC_WE_SET       1UL

/**
 * Read RFLAGS.AC. Returns SMAP_RFLAGS_AC if set, 0 if clear.
 * This is the READ the #646 pair was missing.
 */
__attribute__((always_inline))
static inline uaccess_ac_t uaccess_ac_read(void) {
    unsigned long flags;
    // PUSHFQ/POPQ is the only architectural way to observe AC. -mno-red-zone is
    // in CFLAGS, so touching the stack below RSP here is not a concern, and the
    // push/pop pair is balanced regardless.
    __asm__ __volatile__("pushfq\n\tpopq %0" : "=r"(flags) : : "memory");
    return flags & SMAP_RFLAGS_AC;
}

/**
 * Open an AC window around a supervisor access to USER memory.
 *
 * Returns a token that MUST be passed to uaccess_end(). Nest-safe: if AC is
 * already set by an enclosing bracket, this is a no-op and the returned token
 * tells uaccess_end() not to clear it, so the OUTER bracket keeps ownership.
 *
 * NOT the normal way to touch user memory. Prefer copy_from_user() and friends,
 * which bracket at the instruction. Use this only for a site that genuinely
 * cannot be expressed as one of those copies, and keep it to the smallest
 * possible number of instructions.
 */
__attribute__((always_inline))
static inline uaccess_ac_t uaccess_begin(void) {
    if (!g_smap_active) {
        return 0;                       // SMAP not live: STAC would be #UD.
    }
    if (uaccess_ac_read()) {
        return 0;                       // Nested: an outer bracket owns AC.
    }
    __asm__ __volatile__("stac" : : : "cc", "memory");
    return UACCESS_AC_WE_SET;
}

/**
 * Close the AC window opened by uaccess_begin(). Clears AC only if THIS bracket
 * was the one that set it.
 */
__attribute__((always_inline))
static inline void uaccess_end(uaccess_ac_t tok) {
    if (tok & UACCESS_AC_WE_SET) {
        __asm__ __volatile__("clac" : : : "cc", "memory");
    }
}

/**
 * Unconditionally clear AC, ignoring nesting. This is the PANIC BUTTON for a
 * return-to-Ring-3 chokepoint, not a bracket primitive: it is the correct thing
 * to call where "AC must be clear from here on" is an invariant rather than the
 * end of a matched pair. Safe to call with AC already clear.
 */
__attribute__((always_inline))
static inline void uaccess_ac_force_clear(void) {
    if (g_smap_active) {
        __asm__ __volatile__("clac" : : : "cc", "memory");
    }
}

#endif // SECURITY_UACCESS_SMAP_H
