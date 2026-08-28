// dos/dpmi.h - #740: the C surface of THE DPMI HOST CORE (INT 31h).
//
// The host itself is Rust (rustkern/dpmi.rs) per the 2026-07-16 rule. This
// header is the C-visible boundary: the register frame, the two bindings, and
// the lookup the 32-bit execution core needs. Read rustkern/dpmi.rs's header
// comment first; it states what is implemented, what is refused BY DESIGN, and
// what is a MISS on purpose.
//
// ===========================================================================
// WHO ATTACHES WHAT
// ---------------------------------------------------------------------------
// Three separate pieces of work meet here, and the seams are deliberate so
// none of them has to edit another's file.
//
//  1. THE 32-BIT EXECUTION CORE (does not exist yet). On a guest INT 31h it
//     fills a dpmi_regs_t from its own register file, calls dpmi_int31_rs(),
//     and copies back. For a selector:offset it calls dpmi_sel_lookup_rs().
//     It must NOT read the descriptor table any other way; that lookup is the
//     only reason the table is load-bearing rather than decorative.
//
//  2. THE 0300 / 0301 / 0302 REAL-MODE CALL PATH. Attaches with
//     dpmi_set_ext_rs(). The dispatcher calls the hook for every AX it does
//     not service itself; the hook returns 1 if it handled the call (having
//     set CF and the registers itself) and 0 to fall through to the MISS path.
//     This is the same shape as dos_svc_ctx_t::extend in dos/int21svc.h, and
//     for the same reason: a caller's own functions plug in without the core
//     acquiring a branch on who is asking.
//
//     THE MARSHALLER FOR 0300h ALREADY EXISTS: dpmi_rmcs_call_rs() in
//     dos/dpmi_rmcs.h. What is NOT written is the ~20 lines of glue between
//     the two, and that is deliberate rather than forgotten: the glue needs a
//     dpmi_arena_t and a dos_svc_ctx_t, both of which belong to the guest
//     object that the 32-bit execution core will own, and neither exists yet.
//     Whoever builds that guest writes the hook, roughly:
//
//         static int guest_ext(void *user, dpmi_regs_t *r, uint16_t ax) {
//             dos4gw_guest_t *g = user;
//             if (ax != 0x0300) return 0;          // decline -> MISS, correctly
//             x86_16_cpu_t frame; memset(&frame, 0, sizeof frame);
//             int rc = dpmi_rmcs_call_rs(&g->arena, (r->es << 4) + r->edi,
//                                        (uint16_t)r->ebx, &frame,
//                                        dpmi_rmcs_dos_dispatch, &g->ctx,
//                                        g->stack_seg, g->stack_sp);
//             if (rc < 0) r->eflags |= 1; else r->eflags &= ~1u;
//             return 1;
//         }
//
//     Note dos/dpmi_rmcs.h's warning that THE TWO CARRY FLAGS ARE NOT THE SAME
//     CARRY FLAG: `rc` is whether the DPMI call worked, and the simulated
//     interrupt's own CF lives in the RMCS flags word where the guest reads it.
//     The line above is the whole of that distinction, and getting it wrong
//     reports a failed DOS call as a failed DPMI call.
//
//  3. THE DOS MEMORY MODEL. Attaches with dpmi_bind_dosmem_rs(). DPMI 0100h is
//     a THIN WRAPPER over the existing INT 21h AH=48h MCB allocator
//     (docs/DPMI_BRIDGE_DESIGN.md 3.9). There is no allocator in the DPMI host
//     and there must never be one: two allocators over one megabyte is the
//     memory-shaped version of the three-INT-21h fault that dos/int21svc.h
//     exists to undo.
//
//  4. THE GUEST'S FLAT MEMORY, with dpmi_bind_arena_rs(), and THE FREE-MEMORY
//     MODEL behind 0500h, with dpmi_bind_meminfo_rs().
//
//     Bind the SAME dpmi_arena_t you gave dpmi_rmcs_bind_arena(); a guest with
//     two arenas has two address spaces. The arena is where 0500h writes its
//     block and the bounds check that decides whether it may, so there is ONE
//     "did we bounds-check?" answer for the whole DPMI host.
//
//     The memory model reports three plain numbers, NOT a filled-in block:
//     largest free block in BYTES, total pages, free pages. The 48-byte block's
//     field order and its 0xFFFFFFFF "unknown" fields are decided inside
//     rustkern/dpmi.rs on purpose, so a guest owner cannot transpose them.
//     Read that file's 0500h section before changing what any field reports:
//     `meminfo[0]` is what Doom sizes its ENTIRE zone heap from, and unlike
//     0600h this function REFUSES rather than guessing, because here a
//     plausible number is silently catastrophic and a refusal is diagnosable.
//
// Until (2), (3) and (4) are bound, 0300 is a logged MISS, 0100 fails cleanly
// with the DOS "insufficient memory" code, and 0500 fails with CF=1 having
// written nothing. All three behaviours are asserted by the boot self-test, so
// "not yet wired" is a measured state rather than a hope.
#ifndef DOS_DPMI_H
#define DOS_DPMI_H

#include "../types.h"
// For dpmi_arena_t, the guest's flat memory window. Included rather than
// forward-declared because it is an anonymous-struct typedef, and duplicated
// here rather than shared through a third header because there is exactly one
// arena per guest and both halves of the DPMI host must name the same one.
#include "dpmi_rmcs.h"

// ---------------------------------------------------------------------------
// The guest's protected-mode register frame.
//
// LAYOUT IS LOAD-BEARING: it is shared with rustkern/dpmi.rs's #[repr(C)]
// DpmiRegs, and a silently wrong offset corrupts one register on every DPMI
// call the guest makes. The _Static_asserts in dos/dpmi.c lock it, in the same
// style the tree already uses for its Rust FFI structs.
//
// This is NOT the DPMI real-mode call structure (RMCS). That is a different,
// 50-byte, guest-visible layout belonging to 0300/0301/0302 and is defined by
// whoever owns those.
// ---------------------------------------------------------------------------
typedef struct dpmi_regs {
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, ebp, esp;
    uint32_t eflags;
    uint32_t eip;
    uint16_t cs, ds, es, fs, gs, ss;
} dpmi_regs_t;

// The DOS memory binding. `alloc` returns 0 on success with *out_seg set to the
// real-mode segment; nonzero on failure with *out_largest set to the largest
// available block in paragraphs (Descent probes with BX=0xFFFF expecting
// exactly that, BIOS/DPMI.C:107). `free` returns 0 on success.
typedef int (*dpmi_dosmem_alloc_fn)(void *user, uint16_t paras,
                                    uint16_t *out_seg, uint16_t *out_largest);
typedef int (*dpmi_dosmem_free_fn)(void *user, uint16_t seg);

// The extension hook. Return 1 if handled, 0 to fall through to the MISS path.
typedef int (*dpmi_ext_fn)(void *user, dpmi_regs_t *r, uint16_t ax);

// The free-memory model behind 0500h. Reports three plain numbers; this host
// owns where they go in the block. Return 0 on success, nonzero if the model
// cannot answer, in which case 0500h returns CF=1 and writes NOTHING rather
// than reporting a guess into a client that will size a heap from it.
typedef int (*dpmi_meminfo_fn)(void *user, uint32_t *largest_free_bytes,
                               uint32_t *total_pages, uint32_t *free_pages);

// ---------------------------------------------------------------------------
// rustkern/dpmi.rs
// ---------------------------------------------------------------------------

// Service one INT 31h call against `r`, in place. Always leaves a DEFINED
// result: an AX nobody implements returns CF=1 and AX=0x8001 rather than an
// untouched register file (blame.md, "An answer given by OMISSION is not an
// answer").
void dpmi_int31_rs(dpmi_regs_t *r);

// Drop every descriptor, lock, census entry and binding. Call at guest
// teardown. Does NOT free DOS memory blocks: that megabyte has one owner and it
// is not this host.
void dpmi_host_reset_rs(void);

// Bind / unbind (NULL, NULL, NULL) the DOS memory model behind 0100h/0101h.
void dpmi_bind_dosmem_rs(dpmi_dosmem_alloc_fn alloc, dpmi_dosmem_free_fn free_fn,
                         void *user);

// Bind / unbind (NULL, NULL) the 0300-family handler.
void dpmi_set_ext_rs(dpmi_ext_fn ext, void *user);

// Bind / unbind (NULL) the guest's flat memory window. Pass the SAME
// dpmi_arena_t given to dpmi_rmcs_bind_arena(). dos/dpmi.c _Static_asserts our
// Rust view of that struct against the declaration in dos/dpmi_rmcs.h, so a
// change to it fails the build instead of silently writing a guest block
// through the wrong offsets.
void dpmi_bind_arena_rs(dpmi_arena_t *arena);

// Bind / unbind (NULL, NULL) the free-memory model behind 0500h.
void dpmi_bind_meminfo_rs(dpmi_meminfo_fn f, void *user);

// Resolve a client selector to base / byte limit / access-rights byte. Returns
// 0 on success, -1 if the selector is not one of ours (a GDT/flat selector, or
// one that has been freed). Out pointers may be NULL.
// (#740 digsel) Install a descriptor AT A SPECIFIC SELECTOR VALUE, for a
// loader that must satisfy a guest's HARDCODED selector constants (the Watcom
// no-extender arm names 0x17, 0x24 and 0x2C and never asks for them). Returns
// 0, or -1 not an LDT-form selector we may own, -2 already live, -3 a system
// descriptor. Refusing changes nothing. See rustkern/dpmi.rs.
int dpmi_seed_desc_rs(uint16_t sel, uint32_t base, uint32_t byte_limit,
                      uint8_t ar, uint8_t ext);

int dpmi_sel_lookup_rs(uint16_t sel, uint32_t *out_base, uint32_t *out_limit,
                       uint8_t *out_ar);
// (#740 dw2) This is also the 32-bit core's segment-base resolver: its
// signature IS x86_32_sel_base_fn, and dos4gw_prepare() binds it with
// x86_32_set_sel_base_cb(). If you change these parameters, change that
// typedef in the same commit; the compiler will insist, which is the point.

// Dump the per-AX call census and the lock counters to serial. THE MEASURING
// INSTRUMENT (docs/DPMI_BRIDGE_DESIGN.md 2.7): the first run of a real DOS/4GW
// binary tells us what it actually calls, which a static scan cannot, because
// Watcom programs load AX from a register (blame.md, 2026-08-07).
void dpmi_report_rs(void);

// Drive the dispatcher with synthesised register files and assert the results.
// Returns 0 if every check passed, else the number of the first failing case;
// *out_checks receives the number of assertions that actually RAN, so "passed"
// is distinguishable from "never ran".
int dpmi_selftest_rs(uint32_t *out_checks);

// ---------------------------------------------------------------------------
// dos/dpmi.c
// ---------------------------------------------------------------------------

// Runs the self-test and prints one PASS/FAIL line to serial and /BOOTLOG.TXT.
// Called from main.c at boot. Until a 32-bit execution path exists this is the
// ONLY thing that reaches the dispatcher, and it is on purpose: in this tree a
// linked symbol proves nothing (validate_user_ptr, sse_save, graphfs's 72
// declarations with 0 callers), so the host is made to RUN on every boot.
void dpmi_selftest_report(void);

#endif // DOS_DPMI_H
