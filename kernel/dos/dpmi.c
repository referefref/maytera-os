// dos/dpmi.c - #740: the C side of the DPMI host core.
//
// Deliberately almost empty. The host is Rust (rustkern/dpmi.rs); this file
// exists for the two things that can only be done from C:
//
//   1. THE ABI LOCK. dpmi_regs_t is shared field-for-field with a #[repr(C)]
//      struct in Rust. Nothing in either compiler checks that, so a
//      _Static_assert set is the check. docs/DPMI_BRIDGE_DESIGN.md 3.2 makes
//      the same point about the real-mode call structure: "a silently wrong
//      offset here corrupts a register in every DOS call the guest makes".
//
//   2. The boot self-test line, in the shape main.c already uses for
//      ktime_selftest_rs and clip_selftest_rs: PASS/FAIL plus the number of
//      assertions that actually RAN, to serial and to /BOOTLOG.TXT.
//
// WHY THE SELF-TEST RUNS AT BOOT, EVERY BOOT.
// The 32-bit execution path for a DOS/4GW guest does not exist yet, so no real
// guest can reach dpmi_int31_rs(). This tree has a documented history of code
// that linked and never executed (validate_user_ptr, sse_save, graphfs's 72
// declarations with 0 callers), and its standing rule is that "it compiles" and
// "nm shows the symbol" prove nothing. So the dispatcher is driven at boot with
// synthesised register files and the results are asserted and printed. When the
// execution core lands, this stops being the only caller and stays as the
// regression check.
#include "dpmi.h"
#include "../serial.h"
#include "../fs/bootlog.h"

// ---------------------------------------------------------------------------
// THE ABI LOCK. Field order and offsets must match rustkern/dpmi.rs's
// #[repr(C)] struct DpmiRegs exactly.
// ---------------------------------------------------------------------------
_Static_assert(sizeof(dpmi_regs_t) == 52,
               "dpmi_regs_t must be 52 bytes: 10 u32 then 6 u16, matching "
               "#[repr(C)] DpmiRegs in rustkern/dpmi.rs");
_Static_assert(__builtin_offsetof(dpmi_regs_t, eax) == 0, "dpmi_regs_t.eax offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, ebx) == 4, "dpmi_regs_t.ebx offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, ecx) == 8, "dpmi_regs_t.ecx offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, edx) == 12, "dpmi_regs_t.edx offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, esi) == 16, "dpmi_regs_t.esi offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, edi) == 20, "dpmi_regs_t.edi offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, ebp) == 24, "dpmi_regs_t.ebp offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, esp) == 28, "dpmi_regs_t.esp offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, eflags) == 32, "dpmi_regs_t.eflags offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, eip) == 36, "dpmi_regs_t.eip offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, cs) == 40, "dpmi_regs_t.cs offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, ds) == 42, "dpmi_regs_t.ds offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, es) == 44, "dpmi_regs_t.es offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, fs) == 46, "dpmi_regs_t.fs offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, gs) == 48, "dpmi_regs_t.gs offset");
_Static_assert(__builtin_offsetof(dpmi_regs_t, ss) == 50, "dpmi_regs_t.ss offset");

// ---------------------------------------------------------------------------
// THE SECOND ABI LOCK, and this one is across TWO agents' work.
//
// 0500h writes its block into the guest through dpmi_arena_t, which is declared
// in dos/dpmi_rmcs.h (0300h's marshaller) and mirrored by #[repr(C)] DpmiArena
// in rustkern/dpmi.rs. Nothing in either compiler checks that a struct declared
// in one subsystem still matches a copy in another, and the failure mode is not
// a build error: it is a guest memory block written through the wrong offsets,
// or a bounds check performed against the wrong field. Both would look like a
// game misbehaving for unrelated reasons.
//
// Reusing their struct rather than declaring a second one is deliberate: two
// bounds-check stories for one guest address space is how "did we bounds-check
// this?" stops having one answer (docs/DPMI_BRIDGE_DESIGN.md 3.10). The price
// of that reuse is exactly these four asserts.
// ---------------------------------------------------------------------------
// 8-byte pointer, then three u32 (offsets 8, 12, 16), then 4 bytes of tail
// padding to the pointer's alignment: 24 bytes, not 20.
_Static_assert(sizeof(dpmi_arena_t) == 24,
               "dpmi_arena_t must be 24 bytes: a pointer then three u32 plus "
               "tail padding, matching #[repr(C)] DpmiArena in rustkern/dpmi.rs");
_Static_assert(__builtin_offsetof(dpmi_arena_t, base) == 0, "dpmi_arena_t.base offset");
_Static_assert(__builtin_offsetof(dpmi_arena_t, size) == 8, "dpmi_arena_t.size offset");
_Static_assert(__builtin_offsetof(dpmi_arena_t, oob_rd) == 12, "dpmi_arena_t.oob_rd offset");
_Static_assert(__builtin_offsetof(dpmi_arena_t, oob_wr) == 16, "dpmi_arena_t.oob_wr offset");

void dpmi_selftest_report(void) {
    uint32_t checks = 0;
    int rc = dpmi_selftest_rs(&checks);
    // The check COUNT is printed for the same reason ktime's is: a self-test
    // that ran zero assertions and a self-test that passed look identical
    // otherwise, and this project has been bitten by exactly that.
    kprintf("[dpmi] INT31 host selftest: %s (%u checks%s)\n",
            rc == 0 ? "PASS" : "FAIL", checks,
            rc == 0 ? "" : ", see [dpmi-st] FAIL lines above");
    bootlog_write("[dpmi] INT31 host selftest %s checks=%u firstfail=%d",
                  rc == 0 ? "PASS" : "FAIL", checks, rc);
}
