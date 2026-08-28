// rustkern/dpmi_rmcs.rs - #740: DPMI FUNCTION 0300h, SIMULATE REAL-MODE INTERRUPT.
//
// This is THE BRIDGE by which a 32-bit protected-mode DOS/4GW guest reaches the
// real-mode DOS/BIOS services that already exist in this kernel. Read
// docs/DPMI_BRIDGE_DESIGN.md sections 3.1, 3.2 and 3.10 first, and
// dos/int21svc.h's "HOW A DPMI HOST FOR DOS/4GW ATTACHES" recipe second.
//
// ===========================================================================
// WHY THIS FILE CONTAINS NO DOS FUNCTION NUMBERS, AND WHY THAT IS THE POINT
// ---------------------------------------------------------------------------
// The tree has carried FOUR INT 21h implementations at various times (#713,
// #736). The mechanism that produced them was not carelessness: the interpreter
// had ONE GLOBAL interrupt-handler slot and its handlers discarded the CPU they
// were called for, so a second guest lineage physically could not register one
// and had to write its own. #736 Stage 1 removed that pressure by splitting the
// SERVICES from the per-guest STATE (dos/int21svc.c + dos_svc_ctx_t), and
// Stage 1b moved the hooks onto x86_16_cpu_t.
//
// A DPMI host is the THIRD guest lineage, so it is exactly the caller that
// would have written the fourth copy. It does not, because DPMI 0300h needs
// ZERO service knowledge:
//
//   the guest has ALREADY marshalled its own buffers into DOS-addressable
//   memory (that is what it called DPMI 0100h for), so the bridge is a STRUCT
//   FIELD COPY in each direction and a dispatch.
//
// That is counter-intuitive and worth stating plainly: the DPMI function that
// looks like the hard one is the trivial one, precisely because DPMI was
// designed so the CLIENT does the marshalling.
//
// Concretely, and this is checkable by reading rather than by trusting: this
// file contains no `0x21` service semantics, no AH values, no filenames, no
// handles. `dispatch` is a function pointer the host supplies. If a future
// change wants to add "what AH=3Dh does" here, there is nowhere to put it
// without adding a match arm over DOS function numbers to a file that has
// none, which is a visible and reviewable act rather than a drift.
//
// ===========================================================================
// LANGUAGE: RUST, per the 2026-07-16 rule, and it earns it here
// ---------------------------------------------------------------------------
// New kernel logic with no C twin to strangle. Two properties make Rust worth
// more than habit on this particular code:
//
//  1. EVERY guest-controlled address in the bridge goes through the four
//     dpmi_arena_* accessors and the one RMCS bounds check below, and each is a
//     checked_add against the arena length that FAILS the call rather than
//     clamping. The C tradition here is the opposite: exec/x86_16.c:328 folds an
//     out-of-range guest access into a spare page so the interpreter keeps
//     running, which was right for chasing a Word 6 rendering bug and is wrong
//     for a service bridge, because it converts a guest pointer error into
//     silent corruption of the guest's own data.
//  2. The 50-byte RMCS layout is a wire format. A silently wrong offset here
//     corrupts one register in EVERY DOS call the guest makes, which presents
//     as "the game is subtly broken" and never as a crash. The layout is locked
//     by offset_of! here AND by __builtin_offsetof _Static_asserts on the C
//     side (dos/dpmi_rmcs.c), so the two languages cannot disagree silently.
//
// ===========================================================================
// THE REAL-MODE CALL STRUCTURE (RMCS)
// ---------------------------------------------------------------------------
// 50 bytes, packed, no alignment holes. Identical for 0300h, 0301h, 0302h and
// 0303h. Five independent sources agree with zero discrepancies (DPMI 0.9 spec
// section 11; Tenberry's int31.gml shipped inside the Open Watcom tree;
// Watcom's dpmi_regs_struct; DJGPP's __dpmi_regs; dosemu2's
// RealModeCallStructure, which is declared packed too).
//
//   00h EDI   04h ESI   08h EBP   0Ch reserved (never read or written)
//   10h EBX   14h EDX   18h ECX   1Ch EAX
//   20h Flags (16-bit, and note it sits AFTER the eight dwords, BEFORE ES)
//   22h ES    24h DS    26h FS    28h GS
//   2Ah IP    2Ch CS    2Eh SP    30h SS
//
// Semantics that vary by function while the layout does not:
//   - CS:IP is IGNORED for 0300h (the real-mode IVT entry for the interrupt
//     number supplies it) but IS the target for 0301h/0302h.
//   - SS:SP == 0:0 means "host supplies a real-mode stack". Doom does NOT use
//     this: it points SS:SP at a 1 KB DOS-memory stack it allocated itself with
//     0100h (i_ibm.c:1203). Both are supported.
//   - On return from 0300h, write back everything EXCEPT SS, SP, CS and IP.
//     (A 0303h callback writes back those too. We do not implement 0303h: it is
//     measured absent from all four target games, not deferred.)
//   - Take the interrupt number from BL and IGNORE BH. Doom passes it in BX,
//     Descent in BL; masking is what makes both work.
//
// ===========================================================================
// THE RMCS IS ALSO THE KERNEL-BOUNDARY MESSAGE IF THE EMULATOR MOVES OUT
// ---------------------------------------------------------------------------
// docs/DPMI_BRIDGE_DESIGN.md section 10: a userland DOS/4GW core cannot call
// int21() directly, and moving int21() to userland would create the second copy
// this whole design exists to prevent. The incremental answer is an RPC into
// the kernel's one implementation, and that needs a SERIALIZABLE call frame.
// The RMCS is already a flat POD with no pointers, so it serves unchanged as
// the guest-visible structure for 0300h, the in-kernel argument here, and the
// syscall payload later. Nothing below reads a host pointer out of the RMCS,
// deliberately, so that property is preserved rather than merely claimed.

use core::ffi::c_void;

// ---------------------------------------------------------------------------
// C-side reporting. kprintf is variadic and is deliberately kept out of the
// Rust FFI (the same call rustkern/guestfs.rs and rustkern/permpath.rs made):
// the DECISION is Rust, the REPORTING is C.
// ---------------------------------------------------------------------------
extern "C" {
    // An interrupt number with no service behind it. See MISS DISCIPLINE below.
    fn dpmi_rmcs_log_miss(intno: u8, ax: u16);
    // A guest-controlled address that left the arena. kind: 1 = the RMCS block
    // itself, 2 = a guest read, 3 = a guest write.
    fn dpmi_rmcs_log_fault(kind: u32, flat: u32, len: u32);
    // SS:SP arrived as 0:0, so the host stack was substituted.
    fn dpmi_rmcs_log_hoststack(seg: u16, sp: u16);
}

// ---------------------------------------------------------------------------
// RMCS field offsets. Used by the byte-level marshaller below, which reads and
// writes the structure through explicit offsets rather than through a struct
// pointer. That is not stylistic: the RMCS lives in GUEST memory at a
// guest-supplied ES:EDI, so it can be at any alignment, and a &DpmiRmcs formed
// over an odd address would be UB before a single field was touched.
// ---------------------------------------------------------------------------
pub const RMCS_EDI: usize = 0x00;
pub const RMCS_ESI: usize = 0x04;
pub const RMCS_EBP: usize = 0x08;
pub const RMCS_RSV: usize = 0x0C;
pub const RMCS_EBX: usize = 0x10;
pub const RMCS_EDX: usize = 0x14;
pub const RMCS_ECX: usize = 0x18;
pub const RMCS_EAX: usize = 0x1C;
pub const RMCS_FLAGS: usize = 0x20;
pub const RMCS_ES: usize = 0x22;
pub const RMCS_DS: usize = 0x24;
pub const RMCS_FS: usize = 0x26;
pub const RMCS_GS: usize = 0x28;
pub const RMCS_IP: usize = 0x2A;
pub const RMCS_CS: usize = 0x2C;
pub const RMCS_SP: usize = 0x2E;
pub const RMCS_SS: usize = 0x30;
pub const RMCS_SIZE: usize = 0x32; // 50

/// The RMCS as a type. NOT used to access guest memory (see above); it exists
/// so the layout is stated once in a form offset_of! can check, and so the C
/// side has a struct to hang __builtin_offsetof asserts on.
#[repr(C, packed)]
pub struct DpmiRmcs {
    pub edi: u32,
    pub esi: u32,
    pub ebp: u32,
    pub reserved: u32,
    pub ebx: u32,
    pub edx: u32,
    pub ecx: u32,
    pub eax: u32,
    pub flags: u16,
    pub es: u16,
    pub ds: u16,
    pub fs: u16,
    pub gs: u16,
    pub ip: u16,
    pub cs: u16,
    pub sp: u16,
    pub ss: u16,
}

// ---------------------------------------------------------------------------
// The register frame handed to the service core.
//
// This mirrors the LEADING FIELDS of exec/x86_16.h's x86_16_cpu_t, which the
// service core (dos_svc_int21) takes. It is a PREFIX mirror on purpose: the C
// struct continues past `exhi` with hook pointers and an x87 stack that this
// bridge has no business knowing about, and #736 Stage 1b appended them
// specifically so every offset above stayed byte-identical.
//
// Only individual FIELDS are ever assigned through a *mut X86RegFrame. Never
// assign the whole struct: that would write 72 bytes over the head of a larger
// C object and zero `mem`, which the service core dereferences.
//
// Every offset below is locked on the C side against the real x86_16_cpu_t
// (dos/dpmi_rmcs.c), and against this struct by dpmi_rmcs_layout_selftest_rs.
// ---------------------------------------------------------------------------
#[repr(C)]
pub struct X86RegFrame {
    pub mem: *mut u8, // 0x00
    pub ax: u16,      // 0x08
    pub bx: u16,
    pub cx: u16,
    pub dx: u16,
    pub si: u16,
    pub di: u16,
    pub bp: u16,
    pub sp: u16,
    pub cs: u16, // 0x18
    pub ds: u16,
    pub es: u16,
    pub ss: u16,
    pub ip: u16,
    pub flags: u16,
    pub fs: u16, // 0x24
    pub gs: u16,
    pub halted: i32,     // 0x28
    pub exit_code: i32,  // 0x2C
    pub insn_count: u64, // 0x30
    pub exhi: [u16; 8],  // 0x38
}

// exhi[] index order. This is NOT 0=AX,1=BX,...: it is the x86 ModRM register
// encoding order, because exhi is indexed by the same `r` the interpreter's
// reg16_ptr() uses (exec/x86_16.c:692-703, get_reg32 at :710). Getting this
// wrong would put the high half of EAX into ECX and would be invisible to any
// test that only used 16-bit values, so it is named rather than inlined.
const XH_AX: usize = 0;
const XH_CX: usize = 1;
const XH_DX: usize = 2;
const XH_BX: usize = 3;
const XH_SP: usize = 4;
const XH_BP: usize = 5;
const XH_SI: usize = 6;
const XH_DI: usize = 7;

// ---------------------------------------------------------------------------
// THE GUEST MEMORY CHOKEPOINT (docs/DPMI_BRIDGE_DESIGN.md section 3.10).
//
// One flat byte region standing in for the guest's first megabyte. Every
// guest-controlled address in the bridge resolves here and nowhere else, so
// "did we bounds-check?" has one answer in one place.
//
// A violation is COUNTED and REPORTED, never clamped and never folded into a
// spare page. A read answers 0xFF, which is what dos_in() already answers for
// an undecoded port, so a guest that reads off the end sees a consistent
// "nothing there" rather than another guest's bytes. A write is dropped.
// ---------------------------------------------------------------------------
#[repr(C)]
pub struct DpmiArena {
    pub base: *mut u8,
    pub size: u32,
    pub oob_rd: u32,
    pub oob_wr: u32,
}

#[inline]
unsafe fn arena_ok(a: *mut DpmiArena) -> bool {
    !a.is_null() && !(*a).base.is_null() && (*a).size > 0
}

/// Real-mode seg:off to a flat offset. The offset wraps WITHIN the segment
/// (16-bit), which is what a real 8086 does and what the interpreter's own
/// accessors do; the segment part is not wrapped here because the arena bound
/// below rejects anything past the end anyway.
#[inline]
fn seg_off_flat(seg: u16, off: u16) -> u32 {
    ((seg as u32) << 4).wrapping_add(off as u32)
}

/// # Safety
/// `u` must be a valid `*mut DpmiArena` whose `base`/`size` describe a live
/// allocation. Called through dos_svc_mem_ops_t, whose `void *u` is the arena
/// the caller bound at attach time.
#[no_mangle]
pub unsafe extern "C" fn dpmi_arena_rd8_rs(u: *mut c_void, seg: u16, off: u16) -> u8 {
    let a = u as *mut DpmiArena;
    if !arena_ok(a) {
        return 0xFF;
    }
    let flat = seg_off_flat(seg, off);
    if flat >= (*a).size {
        (*a).oob_rd = (*a).oob_rd.wrapping_add(1);
        dpmi_rmcs_log_fault(2, flat, 1);
        return 0xFF;
    }
    // SAFETY: flat < size, and base..base+size is a live allocation supplied by
    // the caller, so this byte is inside it.
    *(*a).base.add(flat as usize)
}

/// # Safety
/// As dpmi_arena_rd8_rs.
#[no_mangle]
pub unsafe extern "C" fn dpmi_arena_wr8_rs(u: *mut c_void, seg: u16, off: u16, v: u8) {
    let a = u as *mut DpmiArena;
    if !arena_ok(a) {
        return;
    }
    let flat = seg_off_flat(seg, off);
    if flat >= (*a).size {
        (*a).oob_wr = (*a).oob_wr.wrapping_add(1);
        dpmi_rmcs_log_fault(3, flat, 1);
        return;
    }
    // SAFETY: flat < size, inside the caller's live allocation.
    *(*a).base.add(flat as usize) = v;
}

/// # Safety
/// As dpmi_arena_rd8_rs. The two bytes are fetched independently so the offset
/// wraps within the segment exactly as an 8086 would, rather than reading one
/// byte past the segment end.
#[no_mangle]
pub unsafe extern "C" fn dpmi_arena_rd16_rs(u: *mut c_void, seg: u16, off: u16) -> u16 {
    let lo = dpmi_arena_rd8_rs(u, seg, off) as u16;
    let hi = dpmi_arena_rd8_rs(u, seg, off.wrapping_add(1)) as u16;
    lo | (hi << 8)
}

/// # Safety
/// As dpmi_arena_wr8_rs.
#[no_mangle]
pub unsafe extern "C" fn dpmi_arena_wr16_rs(u: *mut c_void, seg: u16, off: u16, v: u16) {
    dpmi_arena_wr8_rs(u, seg, off, (v & 0xFF) as u8);
    dpmi_arena_wr8_rs(u, seg, off.wrapping_add(1), (v >> 8) as u8);
}

// ---------------------------------------------------------------------------
// Byte-level RMCS access over a local 50-byte snapshot.
// ---------------------------------------------------------------------------
#[inline]
fn ld16(r: &[u8; RMCS_SIZE], o: usize) -> u16 {
    (r[o] as u16) | ((r[o + 1] as u16) << 8)
}
#[inline]
fn ld32(r: &[u8; RMCS_SIZE], o: usize) -> u32 {
    (r[o] as u32) | ((r[o + 1] as u32) << 8) | ((r[o + 2] as u32) << 16) | ((r[o + 3] as u32) << 24)
}
#[inline]
fn st16(r: &mut [u8; RMCS_SIZE], o: usize, v: u16) {
    r[o] = (v & 0xFF) as u8;
    r[o + 1] = (v >> 8) as u8;
}
#[inline]
fn st32(r: &mut [u8; RMCS_SIZE], o: usize, v: u32) {
    r[o] = (v & 0xFF) as u8;
    r[o + 1] = ((v >> 8) & 0xFF) as u8;
    r[o + 2] = ((v >> 16) & 0xFF) as u8;
    r[o + 3] = ((v >> 24) & 0xFF) as u8;
}

// Return codes. Negative is a failure of the SIMULATION itself, which the host
// reports to the guest as INT 31h returning CF=1; it is NOT the same thing as
// the simulated interrupt failing, which is reported inside the RMCS flags.
pub const DPMI_RMCS_OK: i32 = 0;
pub const DPMI_RMCS_EARENA: i32 = -1; // no arena / no frame: a host bug
pub const DPMI_RMCS_EBOUNDS: i32 = -2; // the RMCS block is not in the arena

// Run counters, so a bridge that is never reached is distinguishable from one
// that is. Same argument as dos_svc_ctx_t's n_calls/n_miss.
static mut N_CALLS: u32 = 0;
static mut N_MISS: u32 = 0;
static mut N_HOSTSTACK: u32 = 0;

/// Copy the 50-byte snapshot INTO the register frame.
///
/// # Safety
/// `f` must point at a live object at least `size_of::<X86RegFrame>()` bytes
/// long whose layout matches (an x86_16_cpu_t; locked by _Static_assert in
/// dos/dpmi_rmcs.c). Only individual fields are written.
unsafe fn rmcs_to_frame(r: &[u8; RMCS_SIZE], f: *mut X86RegFrame, stack_seg: u16, stack_sp: u16) {
    let eax = ld32(r, RMCS_EAX);
    let ebx = ld32(r, RMCS_EBX);
    let ecx = ld32(r, RMCS_ECX);
    let edx = ld32(r, RMCS_EDX);
    let esi = ld32(r, RMCS_ESI);
    let edi = ld32(r, RMCS_EDI);
    let ebp = ld32(r, RMCS_EBP);

    (*f).ax = eax as u16;
    (*f).bx = ebx as u16;
    (*f).cx = ecx as u16;
    (*f).dx = edx as u16;
    (*f).si = esi as u16;
    (*f).di = edi as u16;
    (*f).bp = ebp as u16;

    (*f).exhi[XH_AX] = (eax >> 16) as u16;
    (*f).exhi[XH_BX] = (ebx >> 16) as u16;
    (*f).exhi[XH_CX] = (ecx >> 16) as u16;
    (*f).exhi[XH_DX] = (edx >> 16) as u16;
    (*f).exhi[XH_SI] = (esi >> 16) as u16;
    (*f).exhi[XH_DI] = (edi >> 16) as u16;
    (*f).exhi[XH_BP] = (ebp >> 16) as u16;

    (*f).flags = ld16(r, RMCS_FLAGS);
    (*f).es = ld16(r, RMCS_ES);
    (*f).ds = ld16(r, RMCS_DS);
    (*f).fs = ld16(r, RMCS_FS);
    (*f).gs = ld16(r, RMCS_GS);

    // CS:IP is copied IN so a service that logs it sees what the guest passed,
    // and is never copied back out: for 0300h the real-mode IVT entry supplies
    // the target, so the field is an input the host does not own.
    (*f).cs = ld16(r, RMCS_CS);
    (*f).ip = ld16(r, RMCS_IP);

    // SS:SP == 0:0 means "host supplies the stack".
    //
    // No real-mode CODE executes in this design (we service the interrupt
    // directly rather than running the IVT handler), so nothing pushes onto
    // this stack today. It is still set to a valid host-owned area rather than
    // left at 0:0, because the register frame handed to a service must be
    // self-consistent: a service that ever reads SS:SP (or a future 0301h,
    // which DOES transfer control) must not see a null stack that looks
    // deliberate. The substitution is logged, so if a guest ever depends on
    // what is on that stack the log says where to look.
    let ss = ld16(r, RMCS_SS);
    let sp = ld16(r, RMCS_SP);
    if ss == 0 && sp == 0 {
        (*f).ss = stack_seg;
        (*f).sp = stack_sp;
        N_HOSTSTACK = N_HOSTSTACK.wrapping_add(1);
        dpmi_rmcs_log_hoststack(stack_seg, stack_sp);
    } else {
        (*f).ss = ss;
        (*f).sp = sp;
    }
    (*f).exhi[XH_SP] = 0; // the RMCS carries a 16-bit SP only
}

/// Copy the register frame BACK into the 50-byte snapshot.
///
/// Everything except SS, SP, CS and IP, per the DPMI 0.9 specification for
/// 0300h/0301h/0302h. The reserved dword at 0Ch is never touched.
///
/// # Safety
/// As rmcs_to_frame.
unsafe fn rmcs_from_frame(r: &mut [u8; RMCS_SIZE], f: *const X86RegFrame) {
    // Read the high halves out first. Deliberately not a closure over `f`: a
    // closure body is not covered by the enclosing unsafe fn's implicit unsafe
    // block in every edition, and this file must not depend on which.
    let xh = (*f).exhi;
    let hi = |i: usize| (xh[i] as u32) << 16;
    st32(r, RMCS_EAX, (*f).ax as u32 | hi(XH_AX));
    st32(r, RMCS_EBX, (*f).bx as u32 | hi(XH_BX));
    st32(r, RMCS_ECX, (*f).cx as u32 | hi(XH_CX));
    st32(r, RMCS_EDX, (*f).dx as u32 | hi(XH_DX));
    st32(r, RMCS_ESI, (*f).si as u32 | hi(XH_SI));
    st32(r, RMCS_EDI, (*f).di as u32 | hi(XH_DI));
    st32(r, RMCS_EBP, (*f).bp as u32 | hi(XH_BP));
    st16(r, RMCS_FLAGS, (*f).flags);
    st16(r, RMCS_ES, (*f).es);
    st16(r, RMCS_DS, (*f).ds);
    st16(r, RMCS_FS, (*f).fs);
    st16(r, RMCS_GS, (*f).gs);
    // Deliberately NOT written: RMCS_SS, RMCS_SP, RMCS_CS, RMCS_IP, RMCS_RSV.
}

// ---------------------------------------------------------------------------
// MISS DISCIPLINE
//
// An interrupt number with no service behind it LOGS and STUBS WITH THE CORRECT
// REGISTER AND FLAG EFFECT. It never returns quietly.
//
// This is the durable Win16 lesson (blame.md: an UNIMPLEMENTED import that only
// logged desynchronised the whole interpreter stack) applied one layer down,
// and it is a live fault in the DOS layer today: dos/dosexec.c's INT 21h
// default arm logs and returns with CF UNTOUCHED, so ~228 unimplemented
// functions currently look like SUCCESS to a DOS caller.
//
// The stub effect here is the DOS convention for "invalid function": CF set in
// the SIMULATED interrupt's flags, AX = 0001h. The INT 31h call itself still
// succeeds, because the simulation did happen; what failed is the interrupt,
// and that distinction is exactly what a guest can act on.
// ---------------------------------------------------------------------------

/// DPMI 0300h: simulate a real-mode interrupt.
///
/// `rmcs_flat` is the guest's ES:EDI resolved to a flat offset in the arena.
///
/// `rmcs_limit` is the size of the CLIENT'S OWN flat address space, which is
/// NOT the same number as `arena.size` and this is the whole point of the
/// parameter (#740, measured on Discworld II).
///
///   - `arena.size` bounds REAL-MODE seg:off addresses, i.e. everything the
///     simulated interrupt itself reaches through DS:DX, ES:BX and friends.
///     For a DOS/4GW guest that is the first megabyte and nothing above it,
///     because a real-mode service genuinely cannot address higher.
///   - `rmcs_limit` bounds THE RMCS BLOCK, which is a PROTECTED-MODE address in
///     the client's flat space and is routinely far above 1 MiB. Discworld II
///     builds its RMCS on its own 32-bit stack at flat 0x0020F944, so checking
///     it against the 1 MiB real-mode window refused every 0300h the game ever
///     made: two calls, both refused, no VESA detection and no CD detection.
///     That was not a hostile pointer, it was the normal DPMI calling
///     convention being measured against the wrong limit.
///
/// Zero means "same as `arena.size`", which is right for a host whose arena IS
/// the client's whole address space (the selftest harness).
///
/// The caller guarantees `rmcs_limit` bytes are live behind `arena.base`, the
/// same contract `arena.size` already carries.
///
/// `bx` is the guest's BX; the interrupt number is BL and BH is ignored.
/// `dispatch` is the host's service router: it returns non-zero if it serviced
/// `intno`, zero if nothing implements it. A NULL dispatch is treated as "no
/// service", i.e. every interrupt MISSes, which is the correct behaviour for a
/// host that has not wired its services up yet and is loudly diagnosable.
///
/// # Safety
/// `arena`, `frame` and `dispatch`/`user` must be valid for the duration of the
/// call. The frame must be an x86_16_cpu_t (see X86RegFrame).
#[no_mangle]
pub unsafe extern "C" fn dpmi_rmcs_call_rs(
    arena: *mut DpmiArena,
    rmcs_flat: u32,
    rmcs_limit: u32,
    bx: u16,
    frame: *mut X86RegFrame,
    dispatch: Option<unsafe extern "C" fn(*mut c_void, u8, *mut X86RegFrame) -> i32>,
    user: *mut c_void,
    stack_seg: u16,
    stack_sp: u16,
) -> i32 {
    if !arena_ok(arena) || frame.is_null() {
        return DPMI_RMCS_EARENA;
    }
    // The RMCS block itself is a guest-controlled address, so it is bounds
    // checked like any other. checked_add, not `flat + 50 <= size`: the latter
    // wraps for a flat near 2^32 and the check then PASSES.
    //
    // Checked against rmcs_limit (the client's flat space), NOT arena.size (the
    // real-mode window). See the doc comment: conflating the two refused every
    // real 0300h a 32-bit guest makes.
    let limit = if rmcs_limit == 0 { (*arena).size } else { rmcs_limit };
    let end = match (rmcs_flat as usize).checked_add(RMCS_SIZE) {
        Some(e) => e,
        None => {
            dpmi_rmcs_log_fault(1, rmcs_flat, RMCS_SIZE as u32);
            return DPMI_RMCS_EBOUNDS;
        }
    };
    if end > limit as usize {
        (*arena).oob_rd = (*arena).oob_rd.wrapping_add(1);
        dpmi_rmcs_log_fault(1, rmcs_flat, RMCS_SIZE as u32);
        return DPMI_RMCS_EBOUNDS;
    }

    N_CALLS = N_CALLS.wrapping_add(1);

    // Snapshot in, snapshot out. Working on a local copy rather than in place
    // means a service that scribbles over guest memory (a legitimate thing for
    // AH=3Fh to do) cannot half-modify the structure being marshalled.
    let mut r = [0u8; RMCS_SIZE];
    let src = (*arena).base.add(rmcs_flat as usize);
    let mut i = 0usize;
    while i < RMCS_SIZE {
        // SAFETY: [rmcs_flat, rmcs_flat+50) was proven inside the arena above.
        r[i] = *src.add(i);
        i += 1;
    }

    let intno = (bx & 0x00FF) as u8;

    rmcs_to_frame(&r, frame, stack_seg, stack_sp);

    let handled = match dispatch {
        Some(d) => d(user, intno, frame) != 0,
        None => false,
    };

    if !handled {
        N_MISS = N_MISS.wrapping_add(1);
        dpmi_rmcs_log_miss(intno, (*frame).ax);
        (*frame).ax = 0x0001; // DOS "invalid function"
        (*frame).exhi[XH_AX] = 0;
        (*frame).flags |= 0x0001; // CF
    }

    rmcs_from_frame(&mut r, frame);

    let dst = (*arena).base.add(rmcs_flat as usize);
    let mut j = 0usize;
    while j < RMCS_SIZE {
        // SAFETY: same range, proven in-arena above; nothing between then and
        // now changed arena.base or arena.size (neither is written by the
        // accessors, only the oob counters are).
        *dst.add(j) = r[j];
        j += 1;
    }
    DPMI_RMCS_OK
}

/// Counters for the run: calls, MISSes, host-stack substitutions.
///
/// # Safety
/// The three out-pointers may each be NULL; any non-NULL one must be writable.
#[no_mangle]
pub unsafe extern "C" fn dpmi_rmcs_stats_rs(calls: *mut u32, miss: *mut u32, hoststack: *mut u32) {
    if !calls.is_null() {
        *calls = N_CALLS;
    }
    if !miss.is_null() {
        *miss = N_MISS;
    }
    if !hoststack.is_null() {
        *hoststack = N_HOSTSTACK;
    }
}

// ---------------------------------------------------------------------------
// LAYOUT SELF-TEST.
//
// A wire format that nobody has checked at runtime is a comment. This proves
// the Rust side's own view of both structures; the C side's __builtin_offsetof
// asserts in dos/dpmi_rmcs.c prove the C view matches the same numbers at
// COMPILE time, so a divergence cannot reach a running kernel.
// ---------------------------------------------------------------------------

/// Returns 0 on success, or the 1-based index of the first failing check.
/// `*out_checks` receives how many checks ran, so "0 checks, PASS" is not a
/// thing that can be reported.
///
/// # Safety
/// `out_checks` may be NULL; otherwise it must be writable.
#[no_mangle]
pub unsafe extern "C" fn dpmi_rmcs_layout_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut n: u32 = 0;
    let mut fail: i32 = 0;
    let mut chk = |cond: bool| {
        n += 1;
        if !cond && fail == 0 {
            fail = n as i32;
        }
    };

    chk(core::mem::size_of::<DpmiRmcs>() == RMCS_SIZE);
    chk(core::mem::offset_of!(DpmiRmcs, edi) == RMCS_EDI);
    chk(core::mem::offset_of!(DpmiRmcs, esi) == RMCS_ESI);
    chk(core::mem::offset_of!(DpmiRmcs, ebp) == RMCS_EBP);
    chk(core::mem::offset_of!(DpmiRmcs, reserved) == RMCS_RSV);
    chk(core::mem::offset_of!(DpmiRmcs, ebx) == RMCS_EBX);
    chk(core::mem::offset_of!(DpmiRmcs, edx) == RMCS_EDX);
    chk(core::mem::offset_of!(DpmiRmcs, ecx) == RMCS_ECX);
    chk(core::mem::offset_of!(DpmiRmcs, eax) == RMCS_EAX);
    chk(core::mem::offset_of!(DpmiRmcs, flags) == RMCS_FLAGS);
    chk(core::mem::offset_of!(DpmiRmcs, es) == RMCS_ES);
    chk(core::mem::offset_of!(DpmiRmcs, ds) == RMCS_DS);
    chk(core::mem::offset_of!(DpmiRmcs, fs) == RMCS_FS);
    chk(core::mem::offset_of!(DpmiRmcs, gs) == RMCS_GS);
    chk(core::mem::offset_of!(DpmiRmcs, ip) == RMCS_IP);
    chk(core::mem::offset_of!(DpmiRmcs, cs) == RMCS_CS);
    chk(core::mem::offset_of!(DpmiRmcs, sp) == RMCS_SP);
    chk(core::mem::offset_of!(DpmiRmcs, ss) == RMCS_SS);

    // The x86_16_cpu_t prefix. These constants are repeated verbatim in
    // dos/dpmi_rmcs.c's _Static_asserts against the REAL struct.
    chk(core::mem::size_of::<X86RegFrame>() == 72);
    chk(core::mem::offset_of!(X86RegFrame, mem) == 0);
    chk(core::mem::offset_of!(X86RegFrame, ax) == 8);
    chk(core::mem::offset_of!(X86RegFrame, bx) == 10);
    chk(core::mem::offset_of!(X86RegFrame, cx) == 12);
    chk(core::mem::offset_of!(X86RegFrame, dx) == 14);
    chk(core::mem::offset_of!(X86RegFrame, si) == 16);
    chk(core::mem::offset_of!(X86RegFrame, di) == 18);
    chk(core::mem::offset_of!(X86RegFrame, bp) == 20);
    chk(core::mem::offset_of!(X86RegFrame, sp) == 22);
    chk(core::mem::offset_of!(X86RegFrame, cs) == 24);
    chk(core::mem::offset_of!(X86RegFrame, ds) == 26);
    chk(core::mem::offset_of!(X86RegFrame, es) == 28);
    chk(core::mem::offset_of!(X86RegFrame, ss) == 30);
    chk(core::mem::offset_of!(X86RegFrame, ip) == 32);
    chk(core::mem::offset_of!(X86RegFrame, flags) == 34);
    chk(core::mem::offset_of!(X86RegFrame, fs) == 36);
    chk(core::mem::offset_of!(X86RegFrame, gs) == 38);
    chk(core::mem::offset_of!(X86RegFrame, halted) == 40);
    chk(core::mem::offset_of!(X86RegFrame, exit_code) == 44);
    chk(core::mem::offset_of!(X86RegFrame, insn_count) == 48);
    chk(core::mem::offset_of!(X86RegFrame, exhi) == 56);

    chk(core::mem::size_of::<DpmiArena>() == 24);
    chk(core::mem::offset_of!(DpmiArena, base) == 0);
    chk(core::mem::offset_of!(DpmiArena, size) == 8);
    chk(core::mem::offset_of!(DpmiArena, oob_rd) == 12);
    chk(core::mem::offset_of!(DpmiArena, oob_wr) == 16);

    if !out_checks.is_null() {
        *out_checks = n;
    }
    fail
}
