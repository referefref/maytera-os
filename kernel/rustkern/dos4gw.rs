// rustkern/dos4gw.rs - #740: THE REGISTER-FRAME BRIDGE for a 32-bit DOS/4GW guest.
//
// Three pieces of #740 landed on dev already and none of them touched each
// other: rustkern/le.rs loads and relocates a real LE module, rustkern/x86_32.rs
// retires 32-bit protected-mode instructions and stops with an EXIT REASON, and
// rustkern/dpmi.rs answers INT 31h. This file is the seam that makes the three
// one running guest. It is the answer to x86_32.rs's `X32_EXIT_INT`.
//
// ===========================================================================
// THE ONE RULE THIS FILE EXISTS TO OBEY
// ---------------------------------------------------------------------------
// THERE IS NO INT 21h IMPLEMENTATION IN HERE, AND THERE MUST NEVER BE ONE.
//
// #713 spent real effort deleting a SECOND INT 21h. exec/x86_32_test.c's header
// says, of its own two-function toy handler, "if you find yourself wanting to
// add a case, you are writing the bridge, and the bridge belongs in the DPMI
// host next to a dos_svc_ctx_t, not in a test file". This IS that bridge, so
// the temptation arrives here, and the answer is the same one dos/int21svc.h
// gives: attach as a THIRD CALLER, not a third implementation.
//
// Concretely, this file contains no DOS semantics at all. It contains exactly
// one kind of knowledge: WHERE A FUNCTION'S POINTER ARGUMENT IS. That is
// extender knowledge, not DOS knowledge, and a real DOS extender has exactly
// the same table for exactly the same reason (see THE POINTER PROBLEM below).
// Nothing here decides what AH=3Dh does; it decides that AH=3Dh's DS:EDX is an
// ASCIIZ string that has to be somewhere a 16-bit service core can address.
//
// ===========================================================================
// THE POINTER PROBLEM, WHICH IS THE WHOLE DIFFICULTY
// ---------------------------------------------------------------------------
// dos_svc_int21() takes an x86_16_cpu_t and reaches guest memory through
// ctx->mem's (seg, off) accessors: two 16-bit values, so 20 bits of address, so
// the first megabyte and not one byte more. A DOS/4GW guest is FLAT: its
// pointers are 32-bit offsets from linear 0, and its own code and data sit
// ABOVE the first megabyte (le.c relocates by a default 1 MiB slide precisely
// so the low megabyte stays free for this).
//
// So a guest's `DS:EDX = 0x00142A10` cannot be handed to the service core. It
// has to be brought DOWN. That is not a workaround, it is what DOS/4GW itself
// does: the extender copies the argument into a real-mode transfer buffer below
// 1 MiB, calls real-mode DOS with a genuine seg:off, and copies the result back.
// docs/DOS4GW_DESIGN.md section 6 and dos/dpmi_rmcs.h describe the same shape
// for the 0300h path; this is the same marshalling for the DIRECT protected-mode
// INT 21h, which DOS/4GW auto-passes-up to its own handler.
//
// THE TRANSFER BUFFER IS AT A FIXED GUEST LINEAR ADDRESS inside the guest's own
// flat space (dos/dos4gw.h: 0x00080000, 64 KiB, below the VGA aperture and above
// the PSP). It is not a host-side scratch buffer. That matters: the arena the
// 32-bit core executes in and the arena the DPMI/INT 21h side addresses as "the
// first megabyte" are THE SAME BYTES, so there is one memory, one bounds check,
// and no coherency question. A host-side bounce buffer would have introduced a
// second copy of guest state, and this project's whole class of DOS bugs is two
// copies of one thing disagreeing.
//
// ===========================================================================
// MISS DISCIPLINE: EVERY MISS STUBS, AND THE STUB HAS THE RIGHT EFFECT
// ---------------------------------------------------------------------------
// blame.md is explicit and this file was written to it: a LOG-ONLY miss
// desynchronises the guest and surfaces the bug far from its cause. An
// unimplemented INT 21h function here therefore leaves with the effect DOS
// itself defines for a function it does not have:
//
//     CF = 1  and  AX = 0x0001 (invalid function number)
//
// ... in the GUEST'S OWN 32-bit register file and EFLAGS, not just in a log
// line. A guest that tests CF (all of them do; the Watcom runtime tests it on
// every call) takes its own error path, which is a behaviour we can read on
// serial, instead of proceeding on a register that still holds whatever it held
// before the call.
//
// An unimplemented INTERRUPT VECTOR (as opposed to function) is worse if got
// wrong, because a guest reads different registers per vector. The stub effect
// is therefore per-vector, in `int_stub_effect()`, and each one is justified
// where it is written.
//
// AND THE CENSUS IS THE POINT. Every MISS is counted by (vector, function) and
// dos4gw_miss_report_rs() prints the table sorted by count at guest teardown.
// That histogram is the deliverable of a first contact with a real binary: it
// is what scopes the next round, and it cannot be obtained by reading the game
// with a disassembler, because a Watcom program loads AH from a register
// (blame.md, 2026-08-07).
//
// ===========================================================================
// ONE HOST, ONE GUEST
// ---------------------------------------------------------------------------
// rustkern/dpmi.rs states that it is one host with one guest and every table a
// module static, with a pointer to #736 Stage 1b if a second concurrent guest is
// ever allowed. This file does NOT silently exceed that: dosexec.c's `g_dos_busy`
// already permits exactly one DOS-family guest at a time, and that is the same
// one. But nothing here is a module static anyway. All state lives in a
// Dos4gwState the C guest object allocates and owns, which is the #736 Stage 1b
// shape rather than the Stage 1a shape, so allowing a second guest later is a
// matter of allocating a second state and not of finding the file statics.
//
// THE STATE STRUCT IS OPAQUE TO C ON PURPOSE. C allocates
// dos4gw_state_size_rs() bytes and never reads a field. There is therefore no
// #[repr(C)] mirror to drift, no _Static_assert set to maintain, and no
// possibility of the silently-wrong-offset fault that dos/dpmi.c and
// dos/dpmi_rmcs.c each need twenty asserts to prevent. Where C needs a number it
// gets it from an accessor.

#![allow(dead_code)]

use crate::dpmi_rmcs::X86RegFrame;
use crate::x86_32::X8632Cpu;

extern "C" {
    fn kprintf(fmt: *const u8, ...);
}

// ---------------------------------------------------------------------------
// EFLAGS / FLAGS bits we touch. CF is the only one a DOS call is defined to
// return, and the only one written back.
// ---------------------------------------------------------------------------
const F_CF: u32 = 1 << 0;
const F_ZF: u32 = 1 << 6;

// x86_32 register indices, ModRM encoding order.
const R_EAX: usize = 0;
const R_ECX: usize = 1;
const R_EDX: usize = 2;
const R_EBX: usize = 3;
const R_ESP: usize = 4;
const R_EBP: usize = 5;
const R_ESI: usize = 6;
const R_EDI: usize = 7;

// x86_16 exhi[] index order. NOT alphabetical: it is the same ModRM order, and
// exec/x86_16.h:60 records in full why that is worth naming rather than
// inlining (the high half of EAX lands in ECX if you assume 0=AX,1=BX).
const XH_AX: usize = 0;
const XH_CX: usize = 1;
const XH_DX: usize = 2;
const XH_BX: usize = 3;
const XH_SP: usize = 4;
const XH_BP: usize = 5;
const XH_SI: usize = 6;
const XH_DI: usize = 7;

// x86_32 segment indices.
const S_ES: usize = 0;
const S_DS: usize = 3;

// ---------------------------------------------------------------------------
// Argument classes. This is the whole "what does the bridge know" surface.
// ---------------------------------------------------------------------------
const A_REGS: u32 = 0; // register-only; nothing to marshal
const A_ASCIIZ_DX: u32 = 1; // DS:EDX -> NUL-terminated path, IN
const A_DOLLAR_DX: u32 = 2; // DS:EDX -> '$'-terminated string, IN
const A_WBUF_DX: u32 = 3; // DS:EDX, CX bytes, IN  (AH=40h write)
const A_RBUF_DX: u32 = 4; // DS:EDX, CX bytes, OUT (AH=3Fh read)
const A_CWD_SI: u32 = 5; // DS:ESI, 64 bytes, OUT  (AH=47h)
const A_ASCIIZ_DX_DI: u32 = 6; // DS:EDX IN and ES:EDI IN (AH=56h rename)
const A_DTA_SET: u32 = 7; // DS:EDX is the guest's new DTA (AH=1Ah)
const A_DTA_GET: u32 = 8; // returns the DTA in ES:BX     (AH=2Fh)
const A_DTA_FIND: u32 = 9; // ASCIIZ in, DTA written out   (AH=4Eh)
const A_DTA_NEXT: u32 = 10; // DTA written out             (AH=4Fh)
const A_MISS: u32 = 11; // nobody has classified this one

/// Where in the transfer buffer each piece goes. Fixed offsets rather than a
/// bump pointer: a fixed layout is inspectable in a memory dump and cannot leak
/// across calls, and 64 KiB is far more room than the largest single argument.
const XO_PATH: u32 = 0x0000; // 256 bytes, the DS:DX ASCIIZ
const XO_PATH2: u32 = 0x0100; // 256 bytes, the ES:DI ASCIIZ (rename)
const XO_DTA: u32 = 0x0200; // 128 bytes, the DTA mirror
const XO_DATA: u32 = 0x0400; // the bulk read/write window
const XO_DATA_MAX: u32 = 0xF000 - XO_DATA; // ~60 KiB usable in one call

const DTA_LEN: u32 = 128;
/// (#rafault) HOW MANY BYTES A FIND ACTUALLY PRODUCES, and why this is not
/// DTA_LEN.
///
/// A DOS FindFirst/FindNext result is a 43-byte structure: 21 bytes of search
/// state, then attribute, time, date, size and a 13-byte ASCIIZ name ending at
/// offset 0x2A. dos/int21svc.c's write_find_result() writes exactly that range
/// and nothing above it, so bytes 0x2B..0x7F of the 128-byte mirror are never
/// written by a find and hold whatever the mirror last held - zero on the first
/// call, a previous find's bytes after that.
///
/// Copying DTA_LEN back into the guest therefore wrote 85 bytes of NOT-A-RESULT
/// over whatever followed the guest's find buffer. MEASURED on Red Alert: its
/// CD-detect function keeps `struct find_t` at [ebp-0x3c], so DTA+0x30..0x3b are
/// its saved EDI/ESI/EBX, DTA+0x3c is the saved EBP and DTA+0x40 IS THE RETURN
/// ADDRESS. The successful volume-label find zeroed all five, and the epilogue
/// then popped four zero registers and returned to guest linear 0 with every GPR
/// zero. A 43-byte copy-back is what a real DOS writes and it cannot reach past
/// a 43-byte buffer.
///
/// The FAILURE path was harmless for the same reason it hid the bug for so long:
/// a find that returns CF=1 copies nothing, so the two earlier searches in the
/// same run (SC*.MIX, SS*.MIX, both "none") did no damage and only the one that
/// SUCCEEDED killed the guest.
const DTA_FIND_LEN: u32 = 43;
const PATH_MAX: u32 = 255;

/// Classify an INT 21h call by AX. THE ONLY TABLE IN THIS FILE, and it says
/// where a pointer is, never what a function does.
///
/// A function that is NOT in this table is A_MISS, deliberately: passing an
/// unclassified call through would hand the service core the low 16 bits of a
/// 32-bit pointer as if they were a real-mode offset, which is not a failure,
/// it is a plausible wrong address. A_MISS is diagnosable; that is not.
fn int21_class(ax: u32) -> u32 {
    let ah = (ax >> 8) & 0xFF;
    match ah {
        // Console and register-only calls. No pointer, so nothing to do but
        // copy registers: the service core reaches the console through
        // ctx->con, which the C guest binds to the same sink the 16-bit DOS
        // task uses.
        0x01 | 0x02 | 0x05 | 0x06 | 0x07 | 0x08 | 0x0B | 0x0C | 0x0D | 0x0E => A_REGS,
        0x19 | 0x2A | 0x2C | 0x2D | 0x30 | 0x33 | 0x36 | 0x59 | 0x62 => A_REGS,
        // Handle-based: the handle is in BX, no pointer.
        0x3E | 0x42 | 0x44 | 0x45 | 0x46 | 0x5C | 0x68 | 0x6A => A_REGS,
        // Memory (48h/49h/4Ah) is register-only and is answered by the guest's
        // ctx->extend hook, exactly as dosexec.c's own DOS task answers it.
        0x48 | 0x49 | 0x4A => A_REGS,
        // Terminate: AL is the exit code, no pointer.
        0x4C | 0x00 => A_REGS,
        // Interrupt vectors. DS:DX / ES:BX carry an ADDRESS, but under a DPMI
        // host it is a protected-mode address that no 16-bit path will ever
        // call. Passed through as a 32-bit value split across the seg and off
        // fields so 35h returns byte-for-byte what 25h was given: a guest that
        // saves and restores a vector sees its own value back, which is the
        // property that actually matters. See vec_pack/vec_unpack.
        0x25 | 0x35 => A_REGS,
        // Path in DS:EDX.
        0x39 | 0x3A | 0x3B | 0x3C | 0x3D | 0x41 | 0x43 => A_ASCIIZ_DX,
        0x09 => A_DOLLAR_DX,
        0x40 => A_WBUF_DX,
        0x3F => A_RBUF_DX,
        0x47 => A_CWD_SI,
        0x56 => A_ASCIIZ_DX_DI,
        0x1A => A_DTA_SET,
        0x2F => A_DTA_GET,
        0x4E => A_DTA_FIND,
        0x4F => A_DTA_NEXT,
        _ => A_MISS,
    }
}

// ---------------------------------------------------------------------------
// The per-guest state. OPAQUE to C (see the header comment).
// ---------------------------------------------------------------------------

const MISS_SLOTS: usize = 96;
const BLK_SLOTS: usize = 64;

#[derive(Clone, Copy)]
struct MissRec {
    vector: u16,
    func: u16, // AH for INT 21h, AX for INT 31h, AH for the rest
    count: u32,
    first_eip: u32,
}

#[derive(Clone, Copy)]
struct Blk {
    base: u32, // guest linear
    len: u32,
    live: bool,
}

pub struct Dos4gwState {
    mem: *mut u8,
    mem_size: u32,

    xfer_lin: u32,
    xfer_len: u32,

    /// The guest's OWN DTA, as a flat 32-bit linear address. 0 until it sets
    /// one. The service core writes the DTA through ctx->mem at the seg:off we
    /// hand it, which is the low-memory mirror; anything that writes the mirror
    /// is copied back up to here, or the guest reads a stale find result.
    dta_lin: u32,

    /// DPMI 0501 memory. A bump allocator over the arena tail plus a free list
    /// that only ever coalesces the top block, which is enough: Watcom's
    /// runtime allocates a small number of large blocks at startup and does not
    /// churn them. An honest small allocator beats a wrong general one.
    heap_base: u32,
    heap_top: u32,
    heap_next: u32,
    blocks: [Blk; BLK_SLOTS],

    /// (RA4GW) THE FLAT DATA SEGMENT'S BREAK. This is what INT 21h AH=4Ah
    /// MEANS to a 32-bit DOS/4GW guest, and it is not what it means to
    /// real-mode DOS.
    ///
    /// A Watcom 32-bit program's `sbrk` is not a DOS memory-block call. It is
    /// "raise the limit of the selector in ES to EBX paragraphs", issued with
    /// ES holding the guest's own flat data selector. Routed to a real-mode MCB
    /// allocator it is asked of the wrong allocator entirely: THE DIG asked for
    /// 0x1CF86 paragraphs (about 1.8 MB, its image top plus one 4 KiB page) and
    /// was refused against a 640 KiB ceiling by a 16-bit `maxpara`, in a guest
    /// whose arena is 32 MiB. See the header comment on the AH=EDh/AH=4Ah
    /// interception in dos4gw_int21_pre_rs().
    ///
    /// `brk_base` is the module's own top: the break may never be lowered below
    /// it, or the guest would hand its heap the bytes its own image is in.
    /// `brk_cur` is the current limit in BYTES, and it is also the FLOOR of the
    /// DPMI pool, which grows downward to meet it. Neither number is a guess or
    /// a fixed split: the break grows up, the DPMI pool grows down, and each
    /// refuses at whatever the other has actually taken.
    brk_base: u32,
    brk_cur: u32,
    brk_high: u32,
    brk_grants: u32,
    brk_refusals: u32,

    /// What the post-call step has to copy back, decided by the pre-call step.
    pend_class: u32,
    pend_lin: u32,
    pend_len: u32,

    calls21: u32,
    calls_int: u32,
    calls31: u32,
    miss_total: u32,
    miss: [MissRec; MISS_SLOTS],
    miss_n: u32,
    miss_overflow: u32,

    oob: u32,
}

impl Dos4gwState {
    fn slice(&mut self) -> &mut [u8] {
        // SAFETY: `mem` and `mem_size` are the arena the C guest object
        // allocated and holds for the whole life of this state; dos4gw_init_rs
        // is the only writer of both and rejects a null or empty arena.
        unsafe { core::slice::from_raw_parts_mut(self.mem, self.mem_size as usize) }
    }

    /// Bounds-checked window into the arena. THE chokepoint: every
    /// guest-controlled address in this file resolves here and nowhere else, so
    /// "did we bounds-check?" has one answer in one place (the same design
    /// dpmi_rmcs.rs states for its own arena).
    fn win(&mut self, lin: u32, len: u32) -> Option<(usize, usize)> {
        let end = (lin as u64) + (len as u64);
        if end > self.mem_size as u64 {
            self.oob = self.oob.wrapping_add(1);
            return None;
        }
        Some((lin as usize, (lin as usize) + (len as usize)))
    }

    fn rd_byte(&mut self, lin: u32) -> Option<u8> {
        let (a, _) = self.win(lin, 1)?;
        Some(self.slice()[a])
    }

    fn copy_within_guest(&mut self, dst: u32, src: u32, len: u32) -> bool {
        if len == 0 {
            return true;
        }
        if self.win(dst, len).is_none() || self.win(src, len).is_none() {
            return false;
        }
        let (d, s, n) = (dst as usize, src as usize, len as usize);
        let m = self.slice();
        // copy_within is the right primitive: the two windows may overlap in
        // principle and a hand-rolled byte loop in the wrong direction is a
        // classic silent corruption.
        m.copy_within(s..s + n, d);
        true
    }

    /// Copy a NUL- or '$'-terminated string DOWN into the transfer buffer.
    /// Returns the number of bytes placed, including the terminator, or None if
    /// the guest pointer left the arena or the string is unterminated within
    /// `max`. An unterminated string is refused rather than truncated: a
    /// truncated path is a DIFFERENT VALID PATH, which is the worst outcome.
    fn marshal_down_str(&mut self, guest_lin: u32, slot: u32, max: u32, term: u8) -> Option<u32> {
        let dst = self.xfer_lin + slot;
        let mut n: u32 = 0;
        loop {
            if n >= max {
                return None;
            }
            let b = self.rd_byte(guest_lin.wrapping_add(n))?;
            let (a, _) = self.win(dst + n, 1)?;
            self.slice()[a] = b;
            n += 1;
            if b == term {
                return Some(n);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Interrupt-vector packing for INT 21h 25h/35h. See int21_class.
//
// A 32-bit protected-mode handler address is carried through the 16-bit frame's
// (seg, off) pair as (high 16, low 16). That is not a real seg:off and is never
// used as one: nothing in the service core interprets a vector-table entry, it
// stores it and gives it back. Naming the transform is the point, so nobody
// later "fixes" it into a << 4 that would silently mangle the top 12 bits.
// ---------------------------------------------------------------------------
#[inline]
fn vec_pack(flat: u32) -> (u16, u16) {
    ((flat >> 16) as u16, (flat & 0xFFFF) as u16)
}
#[inline]
fn vec_unpack(seg: u16, off: u16) -> u32 {
    ((seg as u32) << 16) | (off as u32)
}

// ---------------------------------------------------------------------------
// Register-frame marshalling, both directions.
//
// Only individual FIELDS are ever assigned through the *mut X86RegFrame:
// dpmi_rmcs.rs:173 records why (assigning the whole struct writes 72 bytes over
// the head of a larger C object and zeroes `mem`, which the service core
// dereferences). The same warning applies here and for the same reason, because
// it is the same struct.
// ---------------------------------------------------------------------------

unsafe fn regs_down(c: &X8632Cpu, f: *mut X86RegFrame) {
    (*f).ax = c.regs[R_EAX] as u16;
    (*f).cx = c.regs[R_ECX] as u16;
    (*f).dx = c.regs[R_EDX] as u16;
    (*f).bx = c.regs[R_EBX] as u16;
    (*f).sp = c.regs[R_ESP] as u16;
    (*f).bp = c.regs[R_EBP] as u16;
    (*f).si = c.regs[R_ESI] as u16;
    (*f).di = c.regs[R_EDI] as u16;
    (*f).exhi[XH_AX] = (c.regs[R_EAX] >> 16) as u16;
    (*f).exhi[XH_CX] = (c.regs[R_ECX] >> 16) as u16;
    (*f).exhi[XH_DX] = (c.regs[R_EDX] >> 16) as u16;
    (*f).exhi[XH_BX] = (c.regs[R_EBX] >> 16) as u16;
    (*f).exhi[XH_SP] = (c.regs[R_ESP] >> 16) as u16;
    (*f).exhi[XH_BP] = (c.regs[R_EBP] >> 16) as u16;
    (*f).exhi[XH_SI] = (c.regs[R_ESI] >> 16) as u16;
    (*f).exhi[XH_DI] = (c.regs[R_EDI] >> 16) as u16;
    (*f).flags = (c.eflags & 0xFFFF) as u16;
    (*f).halted = 0;
}

/// Copy results back UP into the guest's 32-bit register file.
///
/// ONLY the low 16 bits of each GPR are written, and the guest's high halves
/// are preserved. That is not a shortcut, it is correct: a real-mode DOS call
/// through a DPMI host returns 16-bit results, and a guest that had a live
/// 32-bit value in the top half of ECX before an INT 21h still has it after.
/// Zeroing the high half here would corrupt the guest in a way that is
/// invisible until something much later reads a register it never expected to
/// have changed.
unsafe fn regs_up(c: &mut X8632Cpu, f: *const X86RegFrame) {
    c.regs[R_EAX] = (c.regs[R_EAX] & 0xFFFF_0000) | (*f).ax as u32;
    c.regs[R_ECX] = (c.regs[R_ECX] & 0xFFFF_0000) | (*f).cx as u32;
    c.regs[R_EDX] = (c.regs[R_EDX] & 0xFFFF_0000) | (*f).dx as u32;
    c.regs[R_EBX] = (c.regs[R_EBX] & 0xFFFF_0000) | (*f).bx as u32;
    c.regs[R_ESI] = (c.regs[R_ESI] & 0xFFFF_0000) | (*f).si as u32;
    c.regs[R_EDI] = (c.regs[R_EDI] & 0xFFFF_0000) | (*f).di as u32;
    // ESP and EBP are NOT copied back. The guest's stack pointer belongs to the
    // 32-bit guest; a 16-bit service core has no business changing it, and a
    // frame that came back with a stale SP would relocate the guest's stack to
    // somewhere in the first 64 KiB. This is the "stack side effect" class
    // blame.md warns about, in its most destructive form.
    //
    // CF is the one flag a DOS call defines. Copy it and nothing else: ZF and
    // the rest are not specified across an INT 21h and inventing them is the
    // "answer given by omission" fault in reverse.
    if (*f).flags & 1 != 0 {
        c.eflags |= F_CF;
    } else {
        c.eflags &= !F_CF;
    }
}

// ---------------------------------------------------------------------------
// THE STUB EFFECTS. Every one of these is what the guest would see from a real
// DOS/BIOS that did not have the call, and every one is applied to the GUEST'S
// OWN registers, never only to a log line.
// ---------------------------------------------------------------------------
fn int_stub_effect(c: &mut X8632Cpu, vector: u32) {
    match vector {
        0x21 => {
            // DOS: CF=1, AX=0x0001 "invalid function number". This is the
            // documented result and every runtime tests CF.
            c.regs[R_EAX] = (c.regs[R_EAX] & 0xFFFF_0000) | 0x0001;
            c.eflags |= F_CF;
        }
        0x31 => {
            // DPMI: CF=1, AX=0x8001 "unsupported function", identical to what
            // rustkern/dpmi.rs's own MISS path returns, so a guest cannot tell
            // whether the dispatcher or the bridge refused it. One behaviour.
            c.regs[R_EAX] = (c.regs[R_EAX] & 0xFFFF_0000) | 0x8001;
            c.eflags |= F_CF;
        }
        0x10 => {
            // BIOS video has no error convention: it returns "whatever that
            // function returns". The only safe stub is to leave AX alone and
            // clear CF, which is what a BIOS that ignored the call does. AL is
            // NOT set to 0, because for several functions AL=0 is a meaningful
            // answer and this call has no answer.
            c.eflags &= !F_CF;
        }
        0x16 => {
            // BIOS keyboard: report "no key available". AH=01h/11h use ZF, so
            // ZF=1 and AX=0 is the correct "nothing waiting" for both the
            // check and the (blocking) read, and it never invents a keystroke.
            c.regs[R_EAX] &= 0xFFFF_0000;
            c.eflags |= F_ZF;
            c.eflags &= !F_CF;
        }
        0x33 => {
            // Mouse driver absent: AX=0 is exactly "no mouse installed", which
            // is what function 00h returns and what every other function is
            // safe to answer when there is no driver.
            c.regs[R_EAX] &= 0xFFFF_0000;
            c.eflags &= !F_CF;
        }
        0x1A | 0x15 | 0x2F => {
            // BIOS time / system services / multiplex: CF=1 is the generic
            // "not supported" for all three, and INT 2Fh in particular MUST
            // NOT fall through untouched: blame.md records that 2Fh/1687h
            // returning an untouched AX was right by accident and would have
            // had a probe far-call into its own poison.
            c.eflags |= F_CF;
            c.regs[R_EAX] = (c.regs[R_EAX] & 0xFFFF_0000) | 0x0001;
        }
        _ => {
            // Anything else: CF=1 and AX=0xFFFF. A distinctive value, so that
            // if a guest proceeds on it anyway the wrong number in the later
            // trace is recognisable as "this came from an unstubbed vector"
            // rather than looking like arithmetic.
            c.eflags |= F_CF;
            c.regs[R_EAX] = (c.regs[R_EAX] & 0xFFFF_0000) | 0xFFFF;
        }
    }
}

// ---------------------------------------------------------------------------
// The census
// ---------------------------------------------------------------------------
fn note_miss(st: &mut Dos4gwState, vector: u16, func: u16, eip: u32) {
    st.miss_total = st.miss_total.wrapping_add(1);
    let n = st.miss_n as usize;
    for i in 0..n {
        if st.miss[i].vector == vector && st.miss[i].func == func {
            st.miss[i].count = st.miss[i].count.wrapping_add(1);
            return;
        }
    }
    if n >= MISS_SLOTS {
        st.miss_overflow = st.miss_overflow.wrapping_add(1);
        return;
    }
    st.miss[n] = MissRec { vector, func, count: 1, first_eip: eip };
    st.miss_n += 1;
    // First sighting only. A per-call line would drown the log the moment a
    // guest polls an unimplemented function in a loop, which is exactly what a
    // startup that is failing does.
    unsafe {
        kprintf(
            b"[4GW] MISS int %02Xh func %04Xh at EIP 0x%08X (first sighting; stubbed with the documented failure effect)\n\0"
                .as_ptr(),
            vector as u32,
            func as u32,
            eip,
        );
    }
}

// ===========================================================================
// FFI
// ===========================================================================

/// The largest single buffered INT 21h transfer the bridge can carry.
///
/// EXPORTED because the C router needs it to CHUNK, and the alternative is a
/// second copy of the transfer-window arithmetic on the C side that would drift
/// the first time the window is resized.
///
/// TWO limits, and the second is the one that bites. The transfer WINDOW bounds
/// it (XO_DATA_MAX), and so does CX: the service core takes its count in a
/// 16-bit register, so no single call can move more than 0xFFFF bytes however
/// large the window gets. A 32-bit client's ECX is not bounded by either, and a
/// request of 68168 bytes silently became 2632 (68168 & 0xFFFF) and was
/// reported to the guest as a SUCCESSFUL SHORT READ. DOOM printed
/// "W_ReadLump: only read 2632 of 68168 on lump 235" and exited.
#[no_mangle]
pub extern "C" fn dos4gw_xfer_max_rs() -> u32 {
    if XO_DATA_MAX > 0xFFFF { 0xFFFF } else { XO_DATA_MAX }
}

/// sizeof(Dos4gwState) as rustc computed it. The C guest allocates this many
/// bytes and never looks inside, so this number is the ENTIRE ABI of the state
/// object: there is no field layout for C to get wrong.
#[no_mangle]
pub extern "C" fn dos4gw_state_size_rs() -> u32 {
    core::mem::size_of::<Dos4gwState>() as u32
}

/// Initialise a state object over the guest's flat arena.
///
/// # Safety
/// `st` must point to at least `dos4gw_state_size_rs()` writable, suitably
/// aligned bytes, and `mem`/`mem_size` must describe the guest arena for the
/// whole life of the state. Returns 0 on success, -1 if the arena or the
/// transfer window is not inside it (checked here rather than trusted).
#[no_mangle]
pub unsafe extern "C" fn dos4gw_init_rs(
    st: *mut Dos4gwState,
    mem: *mut u8,
    mem_size: u32,
    xfer_lin: u32,
    xfer_len: u32,
    heap_base: u32,
    heap_top: u32,
) -> i32 {
    if st.is_null() || mem.is_null() || mem_size == 0 {
        return -1;
    }
    if (xfer_lin as u64) + (xfer_len as u64) > mem_size as u64 {
        return -1;
    }
    if xfer_len < 0xF000 {
        return -1; // the fixed layout above needs this much
    }
    if (heap_top as u64) > mem_size as u64 || heap_base > heap_top {
        return -1;
    }
    let s = &mut *st;
    s.mem = mem;
    s.mem_size = mem_size;
    s.xfer_lin = xfer_lin;
    s.xfer_len = xfer_len;
    s.dta_lin = 0;
    s.heap_base = heap_base;
    s.heap_top = heap_top;
    // The DPMI pool bumps DOWN from the top of the arena tail, so that the
    // guest's break can grow UP from the bottom of the same region and the two
    // meet in the middle instead of needing a split chosen in advance.
    s.heap_next = heap_top & !0xFFFu32;
    s.blocks = [Blk { base: 0, len: 0, live: false }; BLK_SLOTS];
    s.brk_base = heap_base;
    s.brk_cur = heap_base;
    s.brk_high = heap_base;
    s.brk_grants = 0;
    s.brk_refusals = 0;
    s.pend_class = A_REGS;
    s.pend_lin = 0;
    s.pend_len = 0;
    s.calls21 = 0;
    s.calls_int = 0;
    s.calls31 = 0;
    s.miss_total = 0;
    s.miss = [MissRec { vector: 0, func: 0, count: 0, first_eip: 0 }; MISS_SLOTS];
    s.miss_n = 0;
    s.miss_overflow = 0;
    s.oob = 0;
    0
}

/// Prepare an INT 21h call: marshal the guest's 32-bit register file into a
/// 16-bit frame, bring any pointer argument down into the transfer buffer, and
/// point the frame's seg:off at it.
///
/// Returns 1 if the caller should now run dos_svc_int21() on the frame, or 0 if
/// the bridge refused the call, in which case THE GUEST'S REGISTERS AND FLAGS
/// HAVE ALREADY BEEN SET to the documented failure and there is nothing to do.
///
/// # Safety
/// All three pointers must be valid; `frame` must be a zeroed x86_16_cpu_t.
#[no_mangle]
pub unsafe extern "C" fn dos4gw_int21_pre_rs(
    st: *mut Dos4gwState,
    cpu: *mut X8632Cpu,
    frame: *mut X86RegFrame,
) -> i32 {
    if st.is_null() || cpu.is_null() || frame.is_null() {
        return 0;
    }
    let s = &mut *st;
    let c = &mut *cpu;
    let ax = c.regs[R_EAX] & 0xFFFF;
    let ah = ((ax >> 8) & 0xFF) as u16;
    s.calls21 = s.calls21.wrapping_add(1);

    // (discentry, 2026-08-26) THE RATIONAL DOS/4G EXTENDED-API PROBE. This is
    // the first INT 21h a Watcom 32-bit program makes that we can get WRONG in
    // the "answered by omission" direction, and the wrong answer is not "the
    // call failed", it is "yes, I am a Rational extender".
    //
    // Every Watcom C 32-bit startup runs the same three-step extender ladder,
    // 0x13E bytes past its own entry point: INT 21h AH=30h and test the high
    // half of EAX for 'DX'; else compare AX with 0x4243; else
    //     mov dx,0x78 / mov ax,0xFF00 / int 21h / cmp al,0
    // AL == 0 means "no Rational DOS/4G extended API, I am a plain DPMI client"
    // and takes the generic-DPMI arm. ANY NON-ZERO AL takes the DOS/4G arm and
    // the runtime then sets its memory strategy from a host that is not there.
    //
    // We are a plain DPMI host, so AL=0 is the TRUE answer. The generic INT 21h
    // stub effect answers AX=0x0001 CF=1 ("invalid function number"), which is
    // the right shape for a DOS call and exactly the wrong VALUE here: AL=1 is
    // non-zero, so both titles measured on 2026-08-26 read it as "extender
    // present". MEASURED, same kernel, same medium:
    //   THE DIG (DIG.EXE, LE at 0x2c90): MISS int 21h func 00FFh at EIP
    //     0x00147BBA, then both of its allocators return 0 for an 8-byte
    //     request, "Not enough memory to allocate file structures", INT 21h
    //     AH=4Ch exit code 1 at 7,700 instructions.
    //   RED ALERT (GAME.DAT, LE at 0x3bca4): the identical MISS at EIP
    //     0x002633C6, entry+0x13E again.
    // A guest that proceeds on a wrong answer is worse than one that stops on a
    // refusal, which is why this is a real answer and not another stub.
    //
    // Not routed through note_miss(): this is not a missing implementation any
    // more, it is an implemented "no". CF=1 keeps the DOS convention for an
    // unsupported function; the callers above read AL only.
    if ax == 0xFF00 {
        c.regs[R_EAX] &= 0xFFFF_0000;
        c.eflags |= F_CF;
        return 0;
    }

    // (RA4GW) THE PROTECTED-MODE MEMORY BRIDGE. AH=EDh AND AH=4Ah ARE NOT DOS
    // CALLS WHEN A 32-BIT GUEST MAKES THEM, AND ROUTING THEM AS IF THEY WERE
    // ASKS THE WRONG ALLOCATOR.
    //
    // int21_class() classed 0x48/0x49/0x4A as A_REGS with the note that they
    // are "answered by the guest's ctx->extend hook, exactly as dosexec.c's own
    // DOS task answers it". For 48h and 49h that is right. For 4Ah issued by a
    // 32-bit guest it is not, and the difference is not a detail:
    //
    //   dosexec.c's 4Ah is the REAL-MODE MCB resize. Its ceiling is 0xA000
    //   paragraphs because that is where the first megabyte ends, and its
    //   `maxpara` is a uint16_t because a real-mode paragraph count is.
    //
    //   A 32-bit Watcom program's 4Ah is `sbrk`. ES holds its own FLAT DATA
    //   SELECTOR and EBX holds a 32-BIT paragraph count, and it means "raise
    //   this selector's LIMIT", which under a real extender commits more of the
    //   flat linear space to the process.
    //
    // MEASURED, THE DIG (DIG.EXE, LE at 0x2c90, golden of 2026-08-26): the
    // runtime asked for 0x1CF86 paragraphs, its image top plus one 4 KiB page,
    // about 1.8 MB. The 16-bit path saw BX = 0xCF86 (the low half of a number
    // that never fit), compared it against maxpara = 0xA000, and refused. The
    // guest printed "Not enough memory to allocate file structures" and left
    // through INT 21h AH=4Ch with exit code 1 after 77,379 instructions, inside
    // a 32,572 KiB arena. THE MEMORY WAS THERE; THE QUESTION WENT TO THE WRONG
    // ALLOCATOR.
    //
    // WHY ES == DS IS THE DISCRIMINATOR AND NOT "the guest is 32-bit". A 32-bit
    // guest may still legitimately resize a REAL-MODE block it got from AH=48h,
    // and that one does belong to the MCB allocator. It would carry that
    // block's segment in ES. dos4gw_prepare() seeds the MCB allocator's floor
    // at DOS4GW_DOSMEM_FLOOR (paragraph 0x2000), so no MCB segment can ever
    // equal the flat selector, and "ES is the same selector I address data
    // through" is exactly the question whose answer decides which call this is.
    //
    // AH=EDh IS PART OF THE SAME CALL, NOT A SEPARATE FEATURE. It is the alias
    // probe the runtime issues immediately before the 4Ah, and answering it
    // with the generic MISS effect told the guest an alias selector EXISTED;
    // see int21_ed_no_alias() for the one bit that decides it.
    if ah == 0xED {
        int21_ed_no_alias(c);
        return 0;
    }
    if ah == 0x4A && c.seg[S_ES] == c.seg[S_DS] {
        let paras = c.regs[R_EBX];
        match brk_set(s, paras) {
            Ok(newbrk) => {
                c.eflags &= !F_CF;
                if LOGGED_BRK_OK == 0 {
                    LOGGED_BRK_OK = 1;
                    kprintf(
                        b"[4GW] flat-segment break: first grant at EIP 0x%08X, ES=DS=%04x, %u paragraphs -> break 0x%08X (module top 0x%08X, ceiling 0x%08X = the DPMI pool's floor)\n\0"
                            .as_ptr(),
                        c.eip,
                        c.seg[S_DS] as u32,
                        paras,
                        newbrk,
                        s.brk_base,
                        brk_ceiling(s),
                    );
                }
            }
            Err(maxparas) => {
                // The DOS convention, in the guest's own 32-bit registers:
                // CF=1, AX=8 ("insufficient memory"), BX/EBX = the largest
                // count that would have been granted.
                c.regs[R_EAX] = (c.regs[R_EAX] & 0xFFFF_0000) | 0x0008;
                c.regs[R_EBX] = maxparas;
                c.eflags |= F_CF;
                if LOGGED_BRK_NO == 0 {
                    LOGGED_BRK_NO = 1;
                    kprintf(
                        b"[4GW] flat-segment break: first REFUSAL at EIP 0x%08X, wanted %u paragraphs, ceiling is %u (0x%08X); the DPMI pool has taken the rest\n\0"
                            .as_ptr(),
                        c.eip,
                        paras,
                        maxparas,
                        brk_ceiling(s),
                    );
                }
            }
        }
        return 0;
    }

    let cls = int21_class(ax);

    if cls == A_MISS {
        note_miss(s, 0x21, ah, c.eip);
        int_stub_effect(c, 0x21);
        return 0;
    }

    regs_down(c, frame);
    s.pend_class = cls;
    s.pend_lin = 0;
    s.pend_len = 0;

    // The transfer buffer's seg:off form. It is 64 KiB-aligned by construction
    // (dos/dos4gw.h asserts it) so the segment arithmetic is exact and every
    // offset below is a plain 16-bit number.
    let xseg = (s.xfer_lin >> 4) as u16;

    let ds_base = c.seg_base[S_DS];
    let es_base = c.seg_base[S_ES];

    match cls {
        A_REGS => {
            if ah == 0x25 {
                // set vector: pack the 32-bit PM handler through (seg, off).
                let (sg, of) = vec_pack(c.regs[R_EDX]);
                (*frame).ds = sg;
                (*frame).dx = of;
            }
        }
        A_ASCIIZ_DX | A_DTA_FIND => {
            let src = ds_base.wrapping_add(c.regs[R_EDX]);
            match s.marshal_down_str(src, XO_PATH, PATH_MAX, 0) {
                Some(_) => {
                    (*frame).ds = xseg;
                    (*frame).dx = XO_PATH as u16;
                }
                None => {
                    note_miss(s, 0x21, ah, c.eip);
                    int_stub_effect(c, 0x21);
                    return 0;
                }
            }
        }
        A_DOLLAR_DX => {
            let src = ds_base.wrapping_add(c.regs[R_EDX]);
            // 4 KiB cap: AH=09h has no length, so the terminator is the only
            // bound and a missing '$' must fail rather than walk the arena.
            match s.marshal_down_str(src, XO_PATH, 4096, b'$') {
                Some(_) => {
                    (*frame).ds = xseg;
                    (*frame).dx = XO_PATH as u16;
                }
                None => {
                    note_miss(s, 0x21, ah, c.eip);
                    int_stub_effect(c, 0x21);
                    return 0;
                }
            }
        }
        A_ASCIIZ_DX_DI => {
            let a = ds_base.wrapping_add(c.regs[R_EDX]);
            let b = es_base.wrapping_add(c.regs[R_EDI]);
            let ok = s.marshal_down_str(a, XO_PATH, PATH_MAX, 0).is_some()
                && s.marshal_down_str(b, XO_PATH2, PATH_MAX, 0).is_some();
            if !ok {
                note_miss(s, 0x21, ah, c.eip);
                int_stub_effect(c, 0x21);
                return 0;
            }
            (*frame).ds = xseg;
            (*frame).dx = XO_PATH as u16;
            (*frame).es = xseg;
            (*frame).di = XO_PATH2 as u16;
        }
        A_WBUF_DX => {
            let n = c.regs[R_ECX] & 0xFFFF;
            if n > XO_DATA_MAX {
                // A write longer than the window is refused, not silently
                // short-written: a short write that reports success is how a
                // guest ends up with a truncated file it believes is complete.
                note_miss(s, 0x21, ah, c.eip);
                int_stub_effect(c, 0x21);
                return 0;
            }
            let src = ds_base.wrapping_add(c.regs[R_EDX]);
            if !s.copy_within_guest(s.xfer_lin + XO_DATA, src, n) {
                note_miss(s, 0x21, ah, c.eip);
                int_stub_effect(c, 0x21);
                return 0;
            }
            (*frame).ds = xseg;
            (*frame).dx = XO_DATA as u16;
        }
        A_RBUF_DX => {
            let n = c.regs[R_ECX] & 0xFFFF;
            if n > XO_DATA_MAX {
                note_miss(s, 0x21, ah, c.eip);
                int_stub_effect(c, 0x21);
                return 0;
            }
            // Nothing to copy down; record where it has to go back UP to.
            s.pend_lin = ds_base.wrapping_add(c.regs[R_EDX]);
            s.pend_len = n;
            if s.win(s.pend_lin, n).is_none() {
                note_miss(s, 0x21, ah, c.eip);
                int_stub_effect(c, 0x21);
                return 0;
            }
            (*frame).ds = xseg;
            (*frame).dx = XO_DATA as u16;
        }
        A_CWD_SI => {
            s.pend_lin = ds_base.wrapping_add(c.regs[R_ESI]);
            s.pend_len = 64;
            if s.win(s.pend_lin, 64).is_none() {
                note_miss(s, 0x21, ah, c.eip);
                int_stub_effect(c, 0x21);
                return 0;
            }
            (*frame).ds = xseg;
            (*frame).si = XO_DATA as u16;
        }
        A_DTA_SET => {
            // Remember the guest's own DTA and give the service core the
            // mirror. Every later call that WRITES a DTA copies the mirror back
            // up to here in the post step; without that the guest's find
            // results are written to memory it never reads.
            s.dta_lin = ds_base.wrapping_add(c.regs[R_EDX]);
            (*frame).ds = xseg;
            (*frame).dx = XO_DTA as u16;
        }
        A_DTA_GET => { /* answered entirely in the post step */ }
        A_DTA_NEXT => { /* no input pointer; the DTA mirror is already set */ }
        _ => {}
    }
    1
}

/// Finish an INT 21h call: copy any OUT payload back up into the guest's flat
/// space and merge the 16-bit results into its 32-bit register file.
///
/// # Safety
/// As dos4gw_int21_pre_rs, and `frame` must be the one that call filled.
#[no_mangle]
pub unsafe extern "C" fn dos4gw_int21_post_rs(
    st: *mut Dos4gwState,
    cpu: *mut X8632Cpu,
    frame: *const X86RegFrame,
) {
    if st.is_null() || cpu.is_null() || frame.is_null() {
        return;
    }
    let s = &mut *st;
    let c = &mut *cpu;
    let cf = (*frame).flags & 1 != 0;

    match s.pend_class {
        A_RBUF_DX => {
            // AX is the count actually read. Copying AX bytes rather than CX is
            // the difference between a correct short read and overwriting the
            // guest's buffer tail with stale transfer-buffer bytes.
            if !cf {
                let got = (*frame).ax as u32;
                let n = if got > s.pend_len { s.pend_len } else { got };
                s.copy_within_guest(s.pend_lin, s.xfer_lin + XO_DATA, n);
            }
        }
        A_CWD_SI => {
            // (#rafault) THE SAME SHAPE, one size down. AH=47h documents a
            // 64-byte buffer, so a 64-byte copy is legal where the DTA one was
            // not - but real DOS writes only the ASCIIZ path, and copying the
            // tail hands the guest 60-odd bytes of transfer-buffer residue it
            // did not ask for. Copy to the NUL, and fall back to the full 64
            // only if the service core produced no terminator at all.
            if !cf {
                let mut n: u32 = 64;
                for k in 0..64u32 {
                    if s.rd_byte(s.xfer_lin + XO_DATA + k) == Some(0) {
                        n = k + 1;
                        break;
                    }
                }
                s.copy_within_guest(s.pend_lin, s.xfer_lin + XO_DATA, n);
            }
        }
        A_DTA_FIND | A_DTA_NEXT => {
            // DTA_FIND_LEN, not DTA_LEN. See the constant: the mirror is 128
            // bytes of storage, a find result is 43 bytes of DATA, and copying
            // the difference back is a 85-byte write into the guest's stack.
            if !cf && s.dta_lin != 0 {
                s.copy_within_guest(s.dta_lin, s.xfer_lin + XO_DTA, DTA_FIND_LEN);
            }
        }
        _ => {}
    }

    regs_up(c, frame);

    // Two results the bridge owns rather than the service core, because both
    // are ADDRESSES and the service core only knows the mirror's address.
    let ax_in = (*frame).ax as u32;
    let _ = ax_in;
    match s.pend_class {
        A_DTA_GET => {
            // 2Fh returns the DTA in ES:BX. Give back the guest's own flat
            // pointer, split the same way 25h/35h split a handler, so a guest
            // that saves and restores its DTA gets its own value back.
            let (sg, of) = vec_pack(s.dta_lin);
            c.seg[S_ES] = sg;
            c.regs[R_EBX] = (c.regs[R_EBX] & 0xFFFF_0000) | of as u32;
        }
        _ => {}
    }
    if ((c.regs[R_EAX] >> 8) & 0xFF) == 0x35 {
        // 35h returns the vector in ES:BX; recompose the 32-bit value.
        let flat = vec_unpack((*frame).es, (*frame).bx);
        c.regs[R_EBX] = flat;
    }
    s.pend_class = A_REGS;
}

/// Prepare a non-INT-21h software interrupt for the existing 16-bit service
/// path (INT 10h video, 16h keyboard, 33h mouse, and the rest of what
/// dos/dosexec.c already implements).
///
/// Returns 1 to route, 0 if the bridge refused (guest state already set).
///
/// NOTHING IS MARSHALLED HERE. These vectors are register-only in every form
/// this guest is known to use, and a form that is not (INT 10h AH=13h writes a
/// string through ES:BP) is refused rather than passed with a truncated
/// pointer, for the same reason A_MISS exists.
///
/// # Safety
/// As dos4gw_int21_pre_rs.
#[no_mangle]
pub unsafe extern "C" fn dos4gw_int_pre_rs(
    st: *mut Dos4gwState,
    cpu: *mut X8632Cpu,
    frame: *mut X86RegFrame,
    vector: u32,
) -> i32 {
    if st.is_null() || cpu.is_null() || frame.is_null() {
        return 0;
    }
    let s = &mut *st;
    let c = &mut *cpu;
    let ah = ((c.regs[R_EAX] >> 8) & 0xFF) as u16;
    s.calls_int = s.calls_int.wrapping_add(1);

    let al = (c.regs[R_EAX] & 0xFF) as u16;
    let routable = match vector {
        0x10 => ah != 0x13, // 13h writes a string through ES:BP: not register-only
        0x11 | 0x12 | 0x16 | 0x1A | 0x33 => true,
        // (#740) INT 2Fh AH=15h, MSCDEX, and ONLY its register-only members.
        //
        // MEASURED on Discworld II: having been told by INT 21h AX=4409h that
        // E: answers "remote", the game immediately issues INT 2Fh AH=15h from
        // protected mode to confirm the drive really is a CD-ROM. That MISSed
        // here, and a game that cannot confirm the drive concludes there is no
        // disc, which is the whole failure. dos/dosexec.c has implemented the
        // MSCDEX discovery subset since #196; it was simply unreachable from a
        // 32-bit guest.
        //
        // 1500h (count/first), 1501h (sets BX=0, no buffer written), 150Bh
        // (drive check) and 150Ch (version) are register-only, so they marshal
        // through this path exactly as INT 10h does. 150Dh WRITES A DRIVE-
        // LETTER LIST through ES:BX and is deliberately NOT routed: a 32-bit
        // client's ES:BX is not a real-mode seg:off pair, and passing it would
        // scribble on whatever 20-bit address it happened to fold down to. A
        // guest that needs 150Dh reaches it through DPMI 0300h, where the
        // registers really are real-mode and the arena bounds them. Anything
        // else under AH=15h (raw sector, audio) stays a logged MISS.
        0x2F => ah == 0x15 && matches!(al, 0x00 | 0x01 | 0x0B | 0x0C),
        _ => false,
    };
    if !routable {
        note_miss(s, vector as u16, ah, c.eip);
        int_stub_effect(c, vector);
        return 0;
    }
    regs_down(c, frame);
    s.pend_class = A_REGS;
    1
}

/// Merge a non-INT-21h result back. Separate from the 21h form because that one
/// also has payloads to copy and address results to recompose, and folding the
/// two would put a branch on the vector in the middle of the copy-back logic.
///
/// # Safety
/// As dos4gw_int21_post_rs.
#[no_mangle]
pub unsafe extern "C" fn dos4gw_int_post_rs(
    st: *mut Dos4gwState,
    cpu: *mut X8632Cpu,
    frame: *const X86RegFrame,
) {
    if st.is_null() || cpu.is_null() || frame.is_null() {
        return;
    }
    let c = &mut *cpu;
    regs_up(c, frame);
    // BIOS calls return results in the full AX/BX/CX/DX and use ZF (INT 16h
    // AH=01h). regs_up copies CF only, so ZF is carried here, for the vectors
    // where it is defined, rather than being invented for all of them.
    if (*frame).flags & 0x0040 != 0 {
        c.eflags |= F_ZF;
    } else {
        c.eflags &= !F_ZF;
    }
    let _ = st;
}

/// Record that a vector was refused outright, and apply its stub effect.
/// The C side calls this for any vector it has no route for at all.
///
/// # Safety
/// `st` and `cpu` must be valid.
#[no_mangle]
pub unsafe extern "C" fn dos4gw_int_refuse_rs(st: *mut Dos4gwState, cpu: *mut X8632Cpu, vector: u32) {
    if st.is_null() || cpu.is_null() {
        return;
    }
    let s = &mut *st;
    let c = &mut *cpu;
    let ah = ((c.regs[R_EAX] >> 8) & 0xFF) as u16;
    note_miss(s, vector as u16, ah, c.eip);
    int_stub_effect(c, vector);
}

// ---------------------------------------------------------------------------
// INT 31h: the guest's 32-bit register file to and from dpmi_regs_t.
//
// This is the easy direction and it is worth saying why: both sides are 32-bit
// and flat, so there is no marshalling at all, only a copy. Every hard problem
// in this file comes from the 16-bit service core's 20-bit address space, and
// none of it applies to DPMI.
// ---------------------------------------------------------------------------

/// dos/dpmi.h's dpmi_regs_t. #[repr(C)], locked on the C side by the
/// _Static_assert set in dos/dpmi.c, which is the same lock rustkern/dpmi.rs
/// relies on: one layout, two readers, one set of asserts.
#[repr(C)]
pub struct DpmiRegs {
    pub eax: u32,
    pub ebx: u32,
    pub ecx: u32,
    pub edx: u32,
    pub esi: u32,
    pub edi: u32,
    pub ebp: u32,
    pub esp: u32,
    pub eflags: u32,
    pub eip: u32,
    pub cs: u16,
    pub ds: u16,
    pub es: u16,
    pub fs: u16,
    pub gs: u16,
    pub ss: u16,
}

/// # Safety
/// Both pointers must be valid.
#[no_mangle]
pub unsafe extern "C" fn dos4gw_int31_pre_rs(st: *mut Dos4gwState, cpu: *const X8632Cpu, r: *mut DpmiRegs) {
    if cpu.is_null() || r.is_null() {
        return;
    }
    if !st.is_null() {
        (*st).calls31 = (*st).calls31.wrapping_add(1);
    }
    let c = &*cpu;
    (*r).eax = c.regs[R_EAX];
    (*r).ecx = c.regs[R_ECX];
    (*r).edx = c.regs[R_EDX];
    (*r).ebx = c.regs[R_EBX];
    (*r).esp = c.regs[R_ESP];
    (*r).ebp = c.regs[R_EBP];
    (*r).esi = c.regs[R_ESI];
    (*r).edi = c.regs[R_EDI];
    (*r).eflags = c.eflags;
    (*r).eip = c.eip;
    (*r).cs = c.seg[1];
    (*r).ds = c.seg[3];
    (*r).es = c.seg[0];
    (*r).fs = c.seg[4];
    (*r).gs = c.seg[5];
    (*r).ss = c.seg[2];
}

/// # Safety
/// Both pointers must be valid.
#[no_mangle]
pub unsafe extern "C" fn dos4gw_int31_post_rs(cpu: *mut X8632Cpu, r: *const DpmiRegs) {
    if cpu.is_null() || r.is_null() {
        return;
    }
    let c = &mut *cpu;
    c.regs[R_EAX] = (*r).eax;
    c.regs[R_ECX] = (*r).ecx;
    c.regs[R_EDX] = (*r).edx;
    c.regs[R_EBX] = (*r).ebx;
    c.regs[R_EBP] = (*r).ebp;
    c.regs[R_ESI] = (*r).esi;
    c.regs[R_EDI] = (*r).edi;
    // ESP is deliberately NOT copied back, exactly as in regs_up: INT 31h does
    // not move the guest's stack, and a dispatcher that left ESP unset would
    // otherwise relocate it to 0.
    c.seg[0] = (*r).es;
    c.seg[3] = (*r).ds;
    // Only CF and ZF are defined across an INT 31h. Take those two and leave
    // the guest's own arithmetic flags alone.
    c.eflags = (c.eflags & !(F_CF | F_ZF)) | ((*r).eflags & (F_CF | F_ZF));
}

// ---------------------------------------------------------------------------
// DPMI 0500 / 0501 / 0502: the guest's memory.
//
// These are attached through dpmi_set_ext_rs()'s hook, NOT by editing
// rustkern/dpmi.rs. That file states they are "NOT IMPLEMENTED BY ANYONE YET"
// and left as a deliberate MISS so a first run would measure them; the extension
// hook is the seam it provides for exactly this, and using it means the DPMI
// host acquires no branch on who is asking.
//
// WHY THEY ARE HERE AT ALL, given the MISS policy. rustkern/dpmi.rs records the
// measurement that decides it: "Doom sizes its ENTIRE zone heap from 0500's
// meminfo[0], so a wrong or missing answer there is not a failed call, it is a
// game that allocates the wrong heap". A MISS on 0500 does not produce a
// diagnosable failure, it produces a guest that computes a heap size from a
// register we refused to write. That is the one case where implementing beats
// measuring, and it is implemented against the arena we actually have, so the
// number is true rather than generous.
//
// THE ALLOCATOR IS SMALL ON PURPOSE. A bump pointer plus a fixed block table,
// with free only coalescing the top block. Watcom's runtime takes a handful of
// large blocks at startup and does not churn them, so a general allocator would
// be untested code carrying a fragmentation bug nobody would find. If a MISS
// log later shows churn, THAT is when to make it general.
// ---------------------------------------------------------------------------

fn dpmi_mem_alloc(s: &mut Dos4gwState, len: u32) -> Option<u32> {
    if len == 0 {
        return None;
    }
    // 4 KiB granularity, which is what a DPMI host hands out and what keeps a
    // guest's page-aligned assumptions true.
    let need = (len as u64 + 0xFFF) & !0xFFFu64;
    // DOWNWARD from the pool's current bottom. The floor is whatever the
    // guest's break has actually taken (never less than the module top), so a
    // guest that uses BOTH INT 31h 0501h and the AH=4Ah break cannot be handed
    // the same bytes twice, and neither side needs a fixed share of the tail.
    let floor = if s.brk_cur > s.brk_base { s.brk_cur } else { s.brk_base } as u64;
    let next = s.heap_next as u64;
    if next < need || next - need < floor {
        return None;
    }
    let base = (next - need) & !0xFFFu64;
    if base < floor {
        return None;
    }
    let mut slot = usize::MAX;
    for i in 0..BLK_SLOTS {
        if !s.blocks[i].live {
            slot = i;
            break;
        }
    }
    if slot == usize::MAX {
        return None;
    }
    s.blocks[slot] = Blk { base: base as u32, len: need as u32, live: true };
    s.heap_next = base as u32;
    Some(base as u32)
}

fn dpmi_mem_free(s: &mut Dos4gwState, base: u32) -> bool {
    for i in 0..BLK_SLOTS {
        if s.blocks[i].live && s.blocks[i].base == base {
            let top = s.blocks[i].base + s.blocks[i].len;
            s.blocks[i].live = false;
            // Coalesce only if this was the block at the pool's bottom, i.e.
            // the most recent one. (The pool bumps DOWNWARD, so "the newest
            // block" is the lowest, not the highest; before RA4GW this same
            // rule read `top == heap_next`.) Anything else leaks until
            // teardown, and that is stated rather than hidden: the whole arena
            // is freed with the guest.
            if base == s.heap_next {
                s.heap_next = top;
            }
            return true;
        }
    }
    false
}

/// One-shot gate for the 0500h report line. A file static rather than a
/// Dos4gwState field on purpose: the struct's size is the C/Rust FFI contract
/// (dos4gw_state_size_rs), and a diagnostic must not move it.
static mut LOGGED_0500: u32 = 0;

fn dpmi_mem_free_bytes(s: &Dos4gwState) -> u32 {
    let floor = if s.brk_cur > s.brk_base { s.brk_cur } else { s.brk_base };
    if s.heap_next > floor {
        s.heap_next - floor
    } else {
        0
    }
}

/// The highest byte the guest's break may reach: the bottom of whatever the
/// DPMI pool has handed out. ONE function, so the grant test and the number
/// reported back on a refusal can never disagree.
fn brk_ceiling(s: &Dos4gwState) -> u32 {
    s.heap_next
}

/// INT 21h AH=4Ah on the guest's OWN flat data selector: set its limit to
/// `paras` paragraphs. Ok(new break in bytes), or Err(the largest paragraph
/// count that WOULD have been granted), which is what DOS puts in BX on a
/// failed 4Ah.
///
/// A shrink below the module's own top is clamped rather than refused: DOS
/// grants a shrink unconditionally, and the guest is entitled to give memory
/// back, but it is not entitled to give back the bytes its own image is in.
fn brk_set(s: &mut Dos4gwState, paras: u32) -> Result<u32, u32> {
    let want = (paras as u64) << 4;
    let ceil = brk_ceiling(s) as u64;
    if want > ceil {
        s.brk_refusals = s.brk_refusals.wrapping_add(1);
        return Err((ceil >> 4) as u32);
    }
    let new = if want < s.brk_base as u64 { s.brk_base } else { want as u32 };
    s.brk_cur = new;
    if new > s.brk_high {
        s.brk_high = new;
    }
    s.brk_grants = s.brk_grants.wrapping_add(1);
    Ok(new)
}

/// INT 21h AH=EDh: "for the selector in BX, is there an ALIAS selector that
/// maps the same memory, and if so what is it?" A real Rational extender hands
/// out CS and DS as two descriptors over one region, so a heap grow has to
/// raise BOTH limits, and this is how the runtime asks which second one to
/// raise. We hand out ONE flat space with no aliases, so the truthful answer is
/// "no", and the guest then resizes only the selector it already has.
///
/// The answer is bit 0 of AL, and NOTHING ELSE IS READ: the caller does
/// `shl eax,31` to move that one bit into the sign, ORs the low half of EDI
/// under it, and branches on the sign. So the generic MISS effect (CF=1,
/// AX=0x0001) does not read as "no such function", it reads as "YES, and the
/// alias selector is whatever happened to be in DI" - which is why this is an
/// implemented answer and not a stub.
fn int21_ed_no_alias(c: &mut X8632Cpu) {
    c.regs[R_EAX] &= 0xFFFF_0000; // AL bit 0 clear: no alias exists
    c.regs[R_EDI] &= 0xFFFF_0000; // DI = 0: and no selector to go with it
    c.eflags &= !F_CF;            // the call itself succeeded
}

/// One-shot gate for the break's first grant and first refusal. File statics
/// for the same reason LOGGED_0500 is one: the state struct's size is the
/// C/Rust FFI contract and a diagnostic must not move it.
static mut LOGGED_BRK_OK: u32 = 0;
static mut LOGGED_BRK_NO: u32 = 0;

/// The DPMI extension hook's memory half. Returns 1 if it serviced `ax`, 0 to
/// let the caller try the next handler (0300h) or fall through to the host's
/// own MISS path, which is the correct place for anything neither of us has.
///
/// # Safety
/// Both pointers must be valid.
#[no_mangle]
pub unsafe extern "C" fn dos4gw_dpmi_mem_rs(st: *mut Dos4gwState, r: *mut DpmiRegs, ax: u16) -> i32 {
    if st.is_null() || r.is_null() {
        return 0;
    }
    let s = &mut *st;
    match ax {
        0x0500 => {
            // Get free memory information: a 48-byte block at ES:EDI, all
            // fields in 4 KiB PAGES except the first, which is in BYTES.
            //
            // Fields we do not know are written 0xFFFFFFFF, which the DPMI spec
            // defines as "unknown" and every client handles. That is the honest
            // answer and it is a DEFINED one; leaving them untouched would be
            // the omission fault, and inventing plausible values would be worse
            // because a client can size an allocation from them.
            let free = dpmi_mem_free_bytes(s);
            let pages = free >> 12;
            let base = (*r).es as u32; // flat: the selector's base is 0
            let _ = base;
            let dst = (*r).edi;
            if s.win(dst, 48).is_none() {
                (*r).eflags |= F_CF;
                (*r).eax = ((*r).eax & 0xFFFF_0000) | 0x8025; // invalid linear address
                return 1;
            }
            let vals: [u32; 12] = [
                free,        // 00 largest available block, BYTES
                pages,       // 04 maximum unlocked page allocation
                pages,       // 08 maximum locked page allocation
                pages,       // 0C total linear address space, pages
                // 10 total unlocked pages. This USED to be 0xFFFFFFFF
                // ("unknown", which the spec defines and which was the honest
                // answer while nothing tracked locking). We do not page at all,
                // so every page in the pool is unlocked in the only sense the
                // number can have here, and a real count is strictly better
                // than "unknown" for a client that arithmetics on it.
                pages,       // 10 total unlocked pages
                pages,       // 14 free pages
                pages,       // 18 total physical pages
                pages,       // 1C free linear address space, pages
                0xFFFF_FFFF, // 20 size of paging file, pages
                0xFFFF_FFFF, // 24 reserved
                0xFFFF_FFFF, // 28 reserved
                0xFFFF_FFFF, // 2C reserved
            ];
            let (a, _) = match s.win(dst, 48) {
                Some(w) => w,
                None => return 1,
            };
            // (#740) ONE line, first call only. "How much did we tell the guest
            // it had" is the question a title that exits on the answer makes you
            // ask, and it was not recorded anywhere.
            if LOGGED_0500 == 0 {
                LOGGED_0500 = 1;
                kprintf(
                    b"[4GW] 0500h free-memory report: EIP=0x%08x ES:EDI=%04x:%08x -> flat 0x%08x, largest block %u KiB (%u pages)\n\0"
                        .as_ptr(),
                    (*r).eip,
                    (*r).es as u32,
                    (*r).edi,
                    dst,
                    free >> 10,
                    pages,
                );
            }
            {
                let m = s.slice();
                for (i, v) in vals.iter().enumerate() {
                    let b = v.to_le_bytes();
                    m[a + i * 4] = b[0];
                    m[a + i * 4 + 1] = b[1];
                    m[a + i * 4 + 2] = b[2];
                    m[a + i * 4 + 3] = b[3];
                }
            }
            (*r).eflags &= !F_CF;
            1
        }
        0x0501 => {
            // Allocate memory block: BX:CX = size in bytes. Returns the linear
            // address in BX:CX and the handle in SI:DI. We use the linear
            // address as the handle, which is what several real hosts do and
            // which makes 0502's "free by handle" exact.
            let size = (((*r).ebx & 0xFFFF) << 16) | ((*r).ecx & 0xFFFF);
            match dpmi_mem_alloc(s, size) {
                Some(base) => {
                    (*r).ebx = ((*r).ebx & 0xFFFF_0000) | (base >> 16);
                    (*r).ecx = ((*r).ecx & 0xFFFF_0000) | (base & 0xFFFF);
                    (*r).esi = ((*r).esi & 0xFFFF_0000) | (base >> 16);
                    (*r).edi = ((*r).edi & 0xFFFF_0000) | (base & 0xFFFF);
                    (*r).eflags &= !F_CF;
                }
                None => {
                    (*r).eflags |= F_CF;
                    (*r).eax = ((*r).eax & 0xFFFF_0000) | 0x8013; // physical memory unavailable
                }
            }
            1
        }
        0x0502 => {
            let h = (((*r).esi & 0xFFFF) << 16) | ((*r).edi & 0xFFFF);
            if dpmi_mem_free(s, h) {
                (*r).eflags &= !F_CF;
            } else {
                (*r).eflags |= F_CF;
                (*r).eax = ((*r).eax & 0xFFFF_0000) | 0x8023; // invalid handle
            }
            1
        }
        _ => 0,
    }
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

/// Print the MISS histogram, highest count first, plus the call totals.
/// THIS IS THE DELIVERABLE of a first run against a real binary.
///
/// # Safety
/// `st` must be a valid, initialised state.
#[no_mangle]
pub unsafe extern "C" fn dos4gw_report_rs(st: *mut Dos4gwState) {
    if st.is_null() {
        return;
    }
    let s = &mut *st;
    kprintf(
        b"[4GW] service calls: INT 21h=%u  INT 31h=%u  other INT=%u  arena-oob=%u\n\0".as_ptr(),
        s.calls21,
        s.calls31,
        s.calls_int,
        s.oob,
    );
    kprintf(
        b"[4GW] DPMI memory: %u KiB handed out, %u KiB free of a %u KiB pool\n\0".as_ptr(),
        ((s.heap_top & !0xFFFu32) - s.heap_next) >> 10,
        dpmi_mem_free_bytes(s) >> 10,
        (s.heap_top - s.heap_base) >> 10,
    );
    kprintf(
        b"[4GW] flat-segment break (INT 21h AH=4Ah on the guest's own selector): %u grants, %u refusals, high-water 0x%08X = %u KiB above the module top 0x%08X\n\0"
            .as_ptr(),
        s.brk_grants,
        s.brk_refusals,
        s.brk_high,
        (s.brk_high - s.brk_base) >> 10,
        s.brk_base,
    );
    if s.miss_n == 0 {
        kprintf(b"[4GW] MISS histogram: EMPTY. Every call the guest made was routed.\n\0".as_ptr());
        return;
    }
    kprintf(
        b"[4GW] MISS histogram: %u distinct, %u total%s. Ranked by count; this is what scopes the next round.\n\0"
            .as_ptr(),
        s.miss_n,
        s.miss_total,
        if s.miss_overflow != 0 {
            b" (TABLE FULL, some distinct sites merged)\0".as_ptr()
        } else {
            b"\0".as_ptr()
        },
    );
    // Selection sort over at most 96 entries, printed as we go. No allocation,
    // no recursion, and the array is left untouched so a second call prints the
    // same table.
    let n = s.miss_n as usize;
    let mut done = [false; MISS_SLOTS];
    for rank in 0..n {
        let mut best = usize::MAX;
        for i in 0..n {
            if done[i] {
                continue;
            }
            if best == usize::MAX || s.miss[i].count > s.miss[best].count {
                best = i;
            }
        }
        if best == usize::MAX {
            break;
        }
        done[best] = true;
        kprintf(
            b"[4GW]   #%u  int %02Xh func %04Xh  x%u  first at EIP 0x%08X\n\0".as_ptr(),
            (rank + 1) as u32,
            s.miss[best].vector as u32,
            s.miss[best].func as u32,
            s.miss[best].count,
            s.miss[best].first_eip,
        );
    }
}

/// Number of distinct MISS sites, for a caller that wants the number without
/// the table (the boot self-test asserts on it).
///
/// # Safety
/// `st` must be a valid, initialised state.
#[no_mangle]
pub unsafe extern "C" fn dos4gw_miss_count_rs(st: *mut Dos4gwState, out_total: *mut u32) -> u32 {
    if st.is_null() {
        return 0;
    }
    let s = &*st;
    if !out_total.is_null() {
        *out_total = s.miss_total;
    }
    s.miss_n
}

// ---------------------------------------------------------------------------
// Self-test.
//
// It runs on every boot for the reason dos/dpmi.c states about its own: in this
// tree a linked symbol proves nothing, and until a real guest runs this is the
// only thing that reaches the marshalling code. Every assertion is about a
// property that would otherwise fail silently.
// ---------------------------------------------------------------------------

/// Returns 0 if every check passed, else the 1-based number of the first
/// failure. `*out_checks` receives the number of assertions that RAN, so
/// "passed" is distinguishable from "never ran" (the failure mode #514 records).
///
/// # Safety
/// `scratch` must point to `scratch_len` writable bytes; `st` to a state-sized
/// allocation.
#[no_mangle]
pub unsafe extern "C" fn dos4gw_selftest_rs(
    st: *mut Dos4gwState,
    scratch: *mut u8,
    scratch_len: u32,
    out_checks: *mut u32,
) -> i32 {
    let mut n: u32 = 0;
    let mut first_fail: i32 = 0;
    macro_rules! chk {
        ($cond:expr) => {{
            n += 1;
            if !($cond) && first_fail == 0 {
                first_fail = n as i32;
                kprintf(b"[4GW-ST] FAIL check %u\n\0".as_ptr(), n);
            }
        }};
    }

    if st.is_null() || scratch.is_null() || scratch_len < 0x20000 {
        if !out_checks.is_null() {
            *out_checks = 0;
        }
        return -1;
    }
    core::ptr::write_bytes(scratch, 0, scratch_len as usize);

    // Arena: 128 KiB, transfer window at 0x10000 (64 KiB-aligned, as the real
    // guest's is), heap in the tail.
    let rc = dos4gw_init_rs(st, scratch, 0x20000, 0x10000, 0xF000, 0x1F000, 0x20000);
    chk!(rc == 0);
    let s = &mut *st;

    // 1. The classifier. A function with a pointer must NOT be A_REGS, and an
    //    unclassified one must be A_MISS: those two mistakes are the only ways
    //    this table can be wrong, and both are silent at run time.
    chk!(int21_class(0x0900) == A_DOLLAR_DX);
    chk!(int21_class(0x3D00) == A_ASCIIZ_DX);
    chk!(int21_class(0x4000) == A_WBUF_DX);
    chk!(int21_class(0x3F00) == A_RBUF_DX);
    chk!(int21_class(0x3000) == A_REGS);
    chk!(int21_class(0x7700) == A_MISS);

    // 1b. (#740 dw2) A DPMI SELECTOR HAS A BASE, AND AN UNKNOWN ONE DOES NOT.
    //
    // THE BUG THIS REPLACES A COMMENT ABOUT. exec/x86_32 threaded seg_base[]
    // through every effective address and nothing ever wrote it, so `mov es,
    // dx` after INT 31h AX=0100h left ES addressing flat 0. Discworld II wrote
    // its 512-byte VbeInfoBlock over the interrupt vector table and the next
    // timer tick was delivered to what that left in vector 08h, 6.5 million
    // instructions from the cause.
    //
    // BOTH HALVES ARE CHECKED. A selector the host handed out must address its
    // base; a selector it did not (an extender's own flat GDT selector, or 0)
    // must still address flat zero, which is what this core did for EVERY
    // selector before. A fix that got the first right and the second wrong
    // would break every flat DOS/4GW title, and would pass a one-sided test.
    {
        extern "C" fn st_sel(sel: u16, base: *mut u32, lim: *mut u32, ar: *mut u8) -> i32 {
            if sel != 0x0117 {
                return -1;   // "not one of ours" -> the core must use base 0
            }
            unsafe {
                if !base.is_null() { *base = 0x0001_2000; }
                if !lim.is_null() { *lim = 0x1FF; }
                if !ar.is_null() { *ar = 0xF3; }
            }
            0
        }

        // mov ax,<sel> / mov es,ax / mov esi,0 / mov eax,0x41424344 /
        // mov [es:esi],eax          -- five instructions, one store.
        let prog = |sel: u16| -> [u8; 17] {
            [
                0x66, 0xB8, (sel & 0xFF) as u8, (sel >> 8) as u8, // mov ax, sel
                0x8E, 0xC0,                                       // mov es, ax
                0xBE, 0x00, 0x00, 0x00, 0x00,                     // mov esi, 0
                0xB8, 0x44, 0x43, 0x42, 0x41,                     // mov eax, 0x41424344
                0x00,                                             // (patched below)
            ]
        };

        for &(sel, want) in &[(0x0117u16, 0x0001_2000u32), (0x0100u16, 0u32)] {
            core::ptr::write_bytes(scratch, 0, scratch_len as usize);
            let p = prog(sel);
            let m = core::slice::from_raw_parts_mut(scratch, scratch_len as usize);
            m[0x800..0x800 + 16].copy_from_slice(&p[..16]);
            // 26 89 06 = mov [es:esi], eax
            m[0x810] = 0x26;
            m[0x811] = 0x89;
            m[0x812] = 0x06;

            let mut cpu: X8632Cpu = core::mem::zeroed();
            crate::x86_32::x86_32_init(&mut cpu, scratch, 0, scratch_len as u32);
            crate::x86_32::x86_32_set_sel_base_cb(&mut cpu, Some(st_sel));
            cpu.eip = 0x800;
            cpu.regs[4] = 0x1F000;      // ESP somewhere sane; nothing pushes
            crate::x86_32::x86_32_run(&mut cpu, 5);

            let m = core::slice::from_raw_parts(scratch, scratch_len as usize);
            chk!(cpu.seg_base[0] == want);          // 0 == S_ES
            chk!(m[want as usize] == 0x44);
            chk!(m[want as usize + 3] == 0x41);
        }
    }

    // 2. Vector packing round-trips a full 32-bit protected-mode address.
    //    A << 4 "fix" would silently lose the top 12 bits, so this is the
    //    check that stops it.
    let (vs, vo) = vec_pack(0xDEAD_BEEF);
    chk!(vec_unpack(vs, vo) == 0xDEAD_BEEF);

    // 3. The bounds chokepoint refuses an address past the arena, and COUNTS
    //    it. Both halves matter: a refusal nobody counts is invisible.
    let oob0 = s.oob;
    chk!(s.win(0x1FFFF, 4).is_none());
    chk!(s.oob == oob0 + 1);
    chk!(s.win(0x1FFF0, 4).is_some());

    // 4. A string with no terminator inside the cap is REFUSED, not truncated.
    //    A truncated path is a different valid path; that is the whole reason
    //    marshal_down_str returns Option.
    {
        let m = s.slice();
        for i in 0..64 {
            m[0x400 + i] = b'A';
        }
        m[0x400 + 8] = 0;
    }
    chk!(s.marshal_down_str(0x400, XO_PATH, 255, 0) == Some(9));
    chk!(s.marshal_down_str(0x400, XO_PATH, 4, 0).is_none());
    // ... and the bytes actually arrived in the transfer buffer.
    chk!(s.rd_byte(0x10000 + XO_PATH) == Some(b'A'));
    chk!(s.rd_byte(0x10000 + XO_PATH + 8) == Some(0));

    // 4b. (#rafault) A SUCCESSFUL FIND COPIES 43 BYTES BACK, NOT 128.
    //
    // THE BUG THIS REPLACES. The post step copied DTA_LEN (the mirror's SIZE)
    // rather than the result's LENGTH, so every AH=4Eh that succeeded wrote 85
    // bytes of mirror residue over whatever followed the guest's find buffer.
    // Red Alert keeps its `struct find_t` at [ebp-0x3c], which puts its saved
    // registers at DTA+0x30, its saved EBP at DTA+0x3c and ITS RETURN ADDRESS
    // at DTA+0x40; the volume-label find zeroed all of them and the function
    // returned to guest linear 0 with every GPR zero.
    //
    // The check is written as a POISON TEST rather than a constant comparison:
    // asserting DTA_FIND_LEN == 43 would pass just as happily if the copy site
    // went back to using DTA_LEN. This drives the real pre/post pair and reads
    // the guest's memory afterwards, so only the actual write can satisfy it.
    {
        let dta: u32 = 0x800;
        let mirror = s.xfer_lin + XO_DTA;
        {
            let m = s.slice();
            for i in 0..0x100usize {
                m[dta as usize + i] = 0xEE;      // poison past the guest's buffer
            }
            for i in 0..DTA_LEN as usize {
                m[mirror as usize + i] = 0x55;   // residue above the result
            }
            for i in 0..43usize {
                m[mirror as usize + i] = i as u8; // the 43 bytes a find produces
            }
            m[0x900] = b'X';                     // the ASCIIZ path AH=4Eh needs
            m[0x901] = 0;
        }
        let mut cpu: X8632Cpu = core::mem::zeroed();
        crate::x86_32::x86_32_init(&mut cpu, scratch, 0, scratch_len as u32);
        let mut f: X86RegFrame = core::mem::zeroed();

        cpu.regs[R_EAX] = 0x1A00;                // AH=1Ah, set DTA
        cpu.regs[R_EDX] = dta;
        chk!(dos4gw_int21_pre_rs(st, &mut cpu, &mut f) == 1);
        chk!((*st).dta_lin == dta);

        cpu.regs[R_EAX] = 0x4E00;                // AH=4Eh, findfirst
        cpu.regs[R_EDX] = 0x900;
        chk!(dos4gw_int21_pre_rs(st, &mut cpu, &mut f) == 1);
        f.flags = 0;                             // CF clear: the find SUCCEEDED
        f.ax = 0;
        dos4gw_int21_post_rs(st, &mut cpu, &f);

        let m = core::slice::from_raw_parts(scratch, scratch_len as usize);
        chk!(m[dta as usize] == 0);              // the result arrived ...
        chk!(m[dta as usize + 42] == 42);        // ... all 43 bytes of it
        chk!(m[dta as usize + 43] == 0xEE);      // ... and NOTHING above it
        chk!(m[dta as usize + 0x3c] == 0xEE);    // RA's saved EBP slot
        chk!(m[dta as usize + 0x40] == 0xEE);    // RA's RETURN ADDRESS slot
        chk!(m[dta as usize + 0x7f] == 0xEE);    // the far end of the old 128
    }

    // 5. The DPMI allocator hands out 4 KiB-aligned blocks, refuses what does
    //    not fit, and reports free space that shrinks by what it gave away.
    let free0 = dpmi_mem_free_bytes(s);
    let b1 = dpmi_mem_alloc(s, 100);
    chk!(b1.is_some());
    chk!(b1.unwrap() & 0xFFF == 0);
    chk!(dpmi_mem_free_bytes(s) == free0 - 0x1000);
    chk!(dpmi_mem_alloc(s, 0x1000_0000).is_none());
    chk!(dpmi_mem_free(s, b1.unwrap()));
    chk!(dpmi_mem_free_bytes(s) == free0);
    chk!(!dpmi_mem_free(s, 0x12345));

    // 5b. (RA4GW) THE FLAT-SEGMENT BREAK, and the fact that it and the DPMI
    //     pool cannot both be handed the same bytes. The selftest arena puts
    //     the module top at 0x1F000 and the arena end at 0x20000, so there is
    //     exactly one 4 KiB page for the two of them to argue over, which is
    //     the only interesting case.
    //
    //     THE REFUSAL CHECK IS THE POINT. A grant-everything break would pass
    //     every game test on day one and hand a guest the DPMI pool's bytes on
    //     the day some title used both.
    chk!(brk_ceiling(s) == 0x20000);
    chk!(brk_set(s, 0x2000) == Ok(0x20000));      // exactly to the ceiling
    chk!(dpmi_mem_free_bytes(s) == 0);            // ... and the pool now has nothing
    chk!(dpmi_mem_alloc(s, 0x1000).is_none());    // ... and says so
    chk!(brk_set(s, 0x2001) == Err(0x2000));      // one paragraph too far
    chk!(brk_set(s, 0x1F00) == Ok(0x1F000));      // a shrink below the module top clamps
    chk!(dpmi_mem_free_bytes(s) == free0);        // ... and gives the page back
    {
        let b2 = dpmi_mem_alloc(s, 0x800);
        chk!(b2 == Some(0x1F000));                 // the pool grows DOWNWARD
        chk!(brk_set(s, 0x1F80) == Err(0x1F00));   // ... and the break stops at it
        chk!(dpmi_mem_free(s, 0x1F000));
        chk!(brk_set(s, 0x2000) == Ok(0x20000));   // freed, so the break may have it
        chk!(brk_set(s, 0x1F00) == Ok(0x1F000));
    }

    // 5c. (RA4GW) AH=EDh answers with the ONE BIT the guest branches on.
    {
        let mut cpu: X8632Cpu = core::mem::zeroed();
        cpu.regs[R_EAX] = 0x1234_ED55; // AL odd: the wrong answer, if left alone
        cpu.regs[R_EDI] = 0xDEAD_BEEF;
        cpu.eflags = 0x0003; // CF set, as the old MISS effect left it
        int21_ed_no_alias(&mut cpu);
        chk!(cpu.regs[R_EAX] & 1 == 0); // "no alias": the bit `shl eax,31` reads
        chk!(cpu.regs[R_EAX] & 0xFFFF_0000 == 0x1234_0000); // high half preserved
        chk!(cpu.regs[R_EDI] & 0xFFFF == 0);
        chk!(cpu.eflags & F_CF == 0);
    }

    // 6. THE STUB EFFECTS ARE APPLIED TO THE GUEST, not merely logged. This is
    //    the blame.md rule made testable: after a refusal the guest's own CF
    //    and AX carry the documented failure.
    {
        let mut cpu: X8632Cpu = core::mem::zeroed();
        cpu.regs[R_EAX] = 0x1234_7700; // AH=77h, unclassified
        cpu.eflags = 0x0002;
        int_stub_effect(&mut cpu, 0x21);
        chk!(cpu.eflags & F_CF != 0);
        chk!(cpu.regs[R_EAX] & 0xFFFF == 0x0001);
        chk!(cpu.regs[R_EAX] & 0xFFFF_0000 == 0x1234_0000); // high half preserved

        cpu.eflags = 0x0002;
        int_stub_effect(&mut cpu, 0x31);
        chk!(cpu.eflags & F_CF != 0);
        chk!(cpu.regs[R_EAX] & 0xFFFF == 0x8001);

        cpu.regs[R_EAX] = 0xAAAA_0100;
        cpu.eflags = 0x0002;
        int_stub_effect(&mut cpu, 0x16);
        chk!(cpu.eflags & F_ZF != 0); // "no key waiting", never an invented key
        chk!(cpu.regs[R_EAX] & 0xFFFF == 0);
    }

    // 7. THE REGISTER ROUND TRIP, which is the single most consequential thing
    //    in this file: a wrong exhi[] index puts the high half of EAX into ECX
    //    and is invisible to any 16-bit-only test (exec/x86_16.h:60).
    {
        let mut cpu: X8632Cpu = core::mem::zeroed();
        cpu.regs[R_EAX] = 0x1111_2222;
        cpu.regs[R_ECX] = 0x3333_4444;
        cpu.regs[R_EDX] = 0x5555_6666;
        cpu.regs[R_EBX] = 0x7777_8888;
        cpu.regs[R_ESP] = 0x9999_AAAA;
        cpu.regs[R_EBP] = 0xBBBB_CCCC;
        cpu.regs[R_ESI] = 0xDDDD_EEEE;
        cpu.regs[R_EDI] = 0x0F0F_1E1E;
        let mut f: X86RegFrame = core::mem::zeroed();
        regs_down(&cpu, &mut f);
        chk!(f.ax == 0x2222 && f.exhi[XH_AX] == 0x1111);
        chk!(f.cx == 0x4444 && f.exhi[XH_CX] == 0x3333);
        chk!(f.dx == 0x6666 && f.exhi[XH_DX] == 0x5555);
        chk!(f.bx == 0x8888 && f.exhi[XH_BX] == 0x7777);
        chk!(f.sp == 0xAAAA && f.exhi[XH_SP] == 0x9999);
        chk!(f.bp == 0xCCCC && f.exhi[XH_BP] == 0xBBBB);
        chk!(f.si == 0xEEEE && f.exhi[XH_SI] == 0xDDDD);
        chk!(f.di == 0x1E1E && f.exhi[XH_DI] == 0x0F0F);

        // Coming back up: the low half changes, the guest's high half survives,
        // and ESP is untouched. The ESP check is the one that matters most: a
        // frame carrying a stale SP back would move the guest's stack into the
        // first 64 KiB and the crash would be nowhere near the cause.
        f.ax = 0x0005;
        f.si = 0x00FF;
        f.sp = 0x0010;
        f.flags = 1;
        regs_up(&mut cpu, &f);
        chk!(cpu.regs[R_EAX] == 0x1111_0005);
        chk!(cpu.regs[R_ESI] == 0xDDDD_00FF);
        chk!(cpu.regs[R_ESP] == 0x9999_AAAA);
        chk!(cpu.eflags & F_CF != 0);
        f.flags = 0;
        regs_up(&mut cpu, &f);
        chk!(cpu.eflags & F_CF == 0);
    }

    // 7b. (#740) THE MSCDEX ROUTING DECISION, which is what lets a 32-bit guest
    //     find a CD at all. Register-only AH=15h members route; the one that
    //     writes a buffer through ES:BX does not, and neither does anything
    //     else on 2Fh, and a refusal must still leave the guest a correct stub.
    {
        let mut f: X86RegFrame = core::mem::zeroed();
        for (ax, want) in [
            (0x1500u32, 1i32), // installation check: BX/CX only
            (0x1501, 1),       // driver header list: answers BX=0, writes nothing
            (0x150B, 1),       // drive check: CX in, AX/BX out
            (0x150C, 1),       // version: BX out
            (0x150D, 0),       // writes drive letters through ES:BX -> refused
            (0x1508, 0),       // absolute sector read: not supported at all
            (0x1600, 0),       // not AH=15h: still refused
        ] {
            let mut cpu: X8632Cpu = core::mem::zeroed();
            cpu.regs[R_EAX] = ax;
            cpu.eflags = 0x0002;
            chk!(dos4gw_int_pre_rs(st, &mut cpu, &mut f, 0x2F) == want);
            if want == 0 {
                // A refusal is not silence: the guest gets 2Fh's own stub.
                chk!(cpu.regs[R_EAX] != ax || cpu.eflags != 0x0002);
            }
        }
    }

    // 8. INT 31h round trip, including the ESP rule.
    {
        let mut cpu: X8632Cpu = core::mem::zeroed();
        cpu.regs[R_EAX] = 0x0000_0400;
        cpu.regs[R_ESP] = 0x0020_0000;
        cpu.eflags = 0x0002;
        let mut r: DpmiRegs = core::mem::zeroed();
        dos4gw_int31_pre_rs(st, &cpu, &mut r);
        chk!(r.eax == 0x0000_0400);
        chk!(r.esp == 0x0020_0000);
        r.eax = 0x0000_005A;
        r.esp = 0; // a dispatcher that never set it
        r.eflags = F_CF;
        dos4gw_int31_post_rs(&mut cpu, &r);
        chk!(cpu.regs[R_EAX] == 0x0000_005A);
        chk!(cpu.regs[R_ESP] == 0x0020_0000);
        chk!(cpu.eflags & F_CF != 0);
    }

    // 9. The census de-duplicates by (vector, function) and keeps the first EIP.
    {
        let before = s.miss_n;
        note_miss(s, 0x21, 0x77, 0x1000);
        note_miss(s, 0x21, 0x77, 0x2000);
        note_miss(s, 0x21, 0x78, 0x3000);
        chk!(s.miss_n == before + 2);
        let mut found = 0u32;
        for i in 0..s.miss_n as usize {
            if s.miss[i].vector == 0x21 && s.miss[i].func == 0x77 {
                found = s.miss[i].count;
                chk!(s.miss[i].first_eip == 0x1000);
            }
        }
        chk!(found == 2);
    }

    if !out_checks.is_null() {
        *out_checks = n;
    }
    first_fail
}
