// dos/dos4gw.h - #740: the C surface of THE DOS/4GW GUEST.
//
// Three finished pieces of #740 met here and became one running guest:
//
//   exec/le.h        parses and relocates a real LE module (Milestones 1-2)
//   exec/x86_32.h    retires 32-bit protected-mode instructions and STOPS with
//                    an exit reason, deliberately leaving the decision to a host
//   dos/dpmi.h       answers INT 31h, with an extension hook for what it does
//                    not own
//
// ... plus the ONE INT 21h service core (dos/int21svc.h, #736) and the 0300h
// marshaller (dos/dpmi_rmcs.h). The bridge logic is Rust, in
// rustkern/dos4gw.rs; read its header comment first, because it states the two
// rules that shape everything here (no fourth INT 21h, and every MISS stubs
// with the documented effect rather than only logging).
//
// ===========================================================================
// THE MEMORY LAYOUT, WHICH IS THE ONE DESIGN DECISION EVERYTHING FOLLOWS FROM
// ---------------------------------------------------------------------------
// ONE BUFFER IS THE GUEST'S ENTIRE FLAT LINEAR SPACE, from linear 0 upward:
//
//   0x00000000  +--------------------------------------------------+
//               | IVT, BDA, PSP: the guest's first megabyte, which |
//               | is REAL-MODE ADDRESSABLE and is exactly the      |
//               | dpmi_arena_t the 0300h path and the INT 21h      |
//   0x00080000  | service core see.                                |
//               | ... the 64 KiB transfer buffer (below)           |
//   0x000A0000  | ... the VGA aperture. dosexec.c's mode-13h       |
//               |     present path reads t->mem + 0xA0000, so a    |
//               |     guest write here reaches the screen with no  |
//   0x00100000  +--------------------------------------------------+
//               | the LE module's objects, relocated by the 1 MiB  |
//               | default slide exec/le.c already applies          |
//               +--------------------------------------------------+
//               | DPMI 0501 heap (rustkern/dos4gw.rs)              |
//               +--------------------------------------------------+
//
// The alternative was two buffers: a module arena for the 32-bit core and a
// separate megabyte for the DOS side. That is the shape this project's DOS bugs
// come in (two copies of one thing disagreeing), and it would have made the
// VGA aperture a copy rather than a location. One buffer means the 32-bit
// core's `mem` and the DOS task's `t->mem` are THE SAME POINTER, so:
//
//   * the mode 13h present path, the EGA planar hooks, the B800 text page and
//     dos_build_psp() all keep working with no change at all, because every one
//     of them addresses the first megabyte and that is where it still is;
//   * there is ONE bounds check (the 32-bit core's window) rather than two that
//     can disagree about where the guest ends.
//
// WHY THE TRANSFER BUFFER EXISTS is in rustkern/dos4gw.rs under THE POINTER
// PROBLEM: dos_svc_int21() addresses guest memory as a 16-bit (seg, off) pair,
// which cannot express a flat 32-bit pointer, so pointer arguments are brought
// down here exactly as a real extender brings them down.
#ifndef DOS_DOS4GW_H
#define DOS_DOS4GW_H

#include "../types.h"

// The guest's real-mode-addressable region. Not a size we chose: it is what a
// 20-bit seg:off can reach, and the reason the transfer buffer has to be in it.
#define DOS4GW_LOW_SIZE   0x00100000u

// The real-mode transfer buffer. 64 KiB at a 64 KiB boundary so its segment
// arithmetic is exact and every offset inside it is a plain 16-bit number.
//
// IT SITS LOW, at 0x10000, and that placement is load-bearing rather than
// arbitrary. DPMI 0100h hands the guest REAL DOS MEMORY out of this same
// megabyte through the existing MCB allocator (dos_extend_int21), whose ceiling
// is the VGA aperture at 0xA0000. Putting the transfer buffer at 0x80000, where
// it started, left it INSIDE that allocator's range, so the first large DOS
// allocation would have been handed the bytes every INT 21h pointer argument is
// marshalled through. Below the allocator's floor there is no such overlap and
// no need for the allocator to learn about a reserved hole.
//
// The layout of the first megabyte is therefore:
//   0x00000  IVT / BDA
//   0x01000  PSP (segment 0x0100, dos_build_psp)
//   0x10000  this transfer buffer, 64 KiB
//   0x20000  the DOS memory pool the MCB allocator hands out (512 KiB)
//   0xA0000  the VGA aperture
#define DOS4GW_XFER_LIN   0x00010000u
#define DOS4GW_XFER_LEN   0x00010000u

// The first paragraph DPMI 0100h / INT 21h AH=48h may hand out, i.e. the first
// paragraph above the transfer buffer.
#define DOS4GW_DOSMEM_FLOOR ((uint16_t)((DOS4GW_XFER_LIN + DOS4GW_XFER_LEN) >> 4))

// (#740 digsel) THE DOS ENVIRONMENT BLOCK, and why a 32-bit guest needs one at
// a KNOWN address.
//
// A Watcom 32-bit runtime that finds no extender takes the plain-DPMI arm of
// its startup ladder, and that arm does not ASK where anything is: it names
// three selectors as constants (0x17 flat alias, 0x24 PSP, 0x2C environment),
// loads the environment selector at offset 0 and walks it - VAR=VALUE strings,
// a NUL to end them, a WORD count, then the program's own path, which becomes
// argv[0]. So the block has to exist somewhere this host can point selector
// 0x2C at.
//
// It goes at 0x2000, between the PSP (which ends at 0x1100) and the INT 21h
// transfer buffer at 0x10000. That gap was unused, it is below the MCB
// allocator's floor so AH=48h can never hand it out, and it is inside the
// real-mode-addressable megabyte, which keeps it reachable by every existing
// DOS-side accessor for free.
#define DOS4GW_ENV_LIN    0x00002000u
#define DOS4GW_ENV_MAX    0x00000400u

// Headroom above the module for the DPMI 0501 heap, and a hard ceiling on the
// whole arena. docs/DOS4GW_DESIGN.md 7.2 measures the constraint: the kernel
// heap ceiling is 256 MiB, so 32 MiB is 12.5% of it and is as far as one guest
// should reach. Doom wants about 8 MiB, Duke 3D about 16.
//
// (#740) RAISED FROM 12 MiB TO 30 MiB, MEASURED IN THREE STEPS, and the middle
// step is the reason the number is not 24.
//
// Discworld II's module top is only about 2 MiB, so 12 MiB of slack made a
// 14 MiB arena and DPMI 0500h reported 12224 KiB free. The game read that,
// printed "Discworld needs more extended memory. Try removing EMM386.EXE..."
// and exited 1 WITHOUT EVER CALLING 0501h: it did not fail to allocate, it
// declined to try. Raising the slack to 24 MiB (0500h then reported 24512 KiB)
// changed NOTHING: same message, same exit 1, same 343410 instructions. At
// 30 MiB (0500h reports 30592 KiB) the game accepts and runs on to 6123814
// instructions and 38 presented frames. So the threshold this title applies is
// somewhere in (24512, 30592] KiB, which is consistent with its own DOSBox
// configuration asking for memsize=32 and with nothing smaller.
//
// The intermediate measurement is recorded because it is the interesting one:
// a plausible-looking increase that moves the reported number and changes no
// behaviour at all is exactly the shape of a fix that is not a fix, and without
// the 24 MiB run there would be no evidence distinguishing "we raised it and it
// worked" from "we raised it twice and the second one mattered".
//
// 30 MiB of slack still lands the whole arena inside the 32 MiB ceiling for a
// module of this size (Discworld II gets 30768 KiB), and it covers Doom's 8 and
// Duke 3D's 16 with room, so this stays ONE number rather than a per-title
// table. The cost is one kmalloc of that size per running DOS/4GW guest.
#define DOS4GW_HEAP_SLACK (30u * 1024u * 1024u)
#define DOS4GW_ARENA_MAX  (32u * 1024u * 1024u)

// The guest's initial stack when the LE header does not carry one. It is carved
// off the TOP of the arena rather than placed in the first megabyte, because the
// first megabyte now belongs to DOS memory and a fallback stack sitting in it
// would be handed out by the next 0100h. DOS4GW_STACK_RESERVE is subtracted from
// the DPMI 0501 heap's ceiling so the two cannot meet.
#define DOS4GW_STACK_RESERVE 0x00010000u

_Static_assert((DOS4GW_XFER_LIN & 0xFFFFu) == 0,
               "the transfer buffer must be 64 KiB-aligned: rustkern/dos4gw.rs "
               "derives its real-mode segment as (xfer_lin >> 4) and treats every "
               "offset inside it as a 16-bit number");
_Static_assert(DOS4GW_XFER_LIN + DOS4GW_XFER_LEN <= 0x000A0000u,
               "the transfer buffer must not overlap the VGA aperture at 0xA0000: "
               "dosexec.c's mode-13h present path reads those bytes as pixels");
_Static_assert(DOS4GW_XFER_LIN + DOS4GW_XFER_LEN <= DOS4GW_LOW_SIZE,
               "the transfer buffer must be real-mode addressable");
_Static_assert(DOS4GW_DOSMEM_FLOOR == 0x2000u,
               "the DOS memory floor is the paragraph above the transfer buffer; "
               "if the transfer buffer moves, dos4gw_prepare()'s alloc_top_para "
               "seed must move with it or DPMI 0100h will hand out the buffer "
               "every INT 21h pointer argument is marshalled through");
_Static_assert(DOS4GW_HEAP_SLACK > DOS4GW_STACK_RESERVE,
               "the DPMI heap must survive the stack reservation");

// ---------------------------------------------------------------------------
// rustkern/dos4gw.rs. Declared here so exactly ONE C header owns the surface
// (#742: the owning header, never a private extern at the call site).
//
// The state object is OPAQUE: allocate dos4gw_state_size_rs() bytes, pass the
// pointer, never read a field. That is why there is no struct here and no
// _Static_assert set locking one, unlike dos/dpmi.c and dos/dpmi_rmcs.c which
// each need twenty. A layout that does not cross the FFI cannot drift across it.
// ---------------------------------------------------------------------------
struct x86_32_cpu_s;
struct dpmi_regs;
struct x86_16_cpu;

uint32_t dos4gw_state_size_rs(void);

// The largest single buffered INT 21h transfer (AH=3Fh / AH=40h) the bridge can
// carry: bounded by the transfer window AND by CX being 16 bits. A 32-bit
// client's ECX is bounded by neither, so the router must chunk anything larger.
uint32_t dos4gw_xfer_max_rs(void);
int  dos4gw_init_rs(void *st, uint8_t *mem, uint32_t mem_size,
                    uint32_t xfer_lin, uint32_t xfer_len,
                    uint32_t heap_base, uint32_t heap_top);

// INT 21h. `pre` returns 1 if the caller should now run dos_svc_int21() on the
// frame, or 0 if the bridge refused, in which case the GUEST'S OWN registers
// and flags already carry the documented failure and there is nothing to do.
int  dos4gw_int21_pre_rs (void *st, void *cpu32, struct x86_16_cpu *frame);
void dos4gw_int21_post_rs(void *st, void *cpu32, const struct x86_16_cpu *frame);

// Every other software interrupt that the existing 16-bit service path already
// implements (INT 10h video, 16h keyboard, 33h mouse, ...). Same 1/0 contract.
int  dos4gw_int_pre_rs   (void *st, void *cpu32, struct x86_16_cpu *frame, uint32_t vector);
void dos4gw_int_post_rs  (void *st, void *cpu32, const struct x86_16_cpu *frame);

// A vector with no route at all: counts it and applies that vector's own stub
// effect to the guest. Never log-only.
void dos4gw_int_refuse_rs(void *st, void *cpu32, uint32_t vector);

// INT 31h. Both sides are 32-bit and flat, so this is a copy, not a marshalling.
void dos4gw_int31_pre_rs (void *st, const void *cpu32, struct dpmi_regs *r);
void dos4gw_int31_post_rs(void *cpu32, const struct dpmi_regs *r);

// The DPMI extension hook's memory half (0500/0501/0502). Returns 1 if it
// serviced `ax`, 0 to fall through. See rustkern/dos4gw.rs for why these three
// are implemented here rather than left as the deliberate MISS rustkern/dpmi.rs
// makes them: Doom sizes its entire zone heap from 0500, so a MISS there does
// not produce a diagnosable failure, it produces a wrong heap.
int  dos4gw_dpmi_mem_rs(void *st, struct dpmi_regs *r, uint16_t ax);

// The MISS histogram, ranked by count. THE DELIVERABLE of a first contact with
// a real binary: a static scan cannot produce it, because a Watcom program
// loads AH from a register (blame.md, 2026-08-07).
void dos4gw_report_rs(void *st);
uint32_t dos4gw_miss_count_rs(void *st, uint32_t *out_total);

// Drives the marshalling with synthesised register files and asserts the
// results. Returns 0 if every check passed, else the number of the first
// failure; *out_checks receives the number of assertions that RAN, so "passed"
// and "never ran" are distinguishable (#514).
int  dos4gw_selftest_rs(void *st, uint8_t *scratch, uint32_t scratch_len,
                        uint32_t *out_checks);

// ---------------------------------------------------------------------------
// dos/dos4gw.c
// ---------------------------------------------------------------------------

// Runs the bridge self-test and prints one PASS/FAIL line to serial and
// /BOOTLOG.TXT, in the shape main.c already uses for dpmi_selftest_report().
// It runs on EVERY boot for the reason dos/dpmi.c states about its own: in this
// tree a linked symbol proves nothing.
void dos4gw_selftest_report(void);

#endif // DOS_DOS4GW_H
