// rustkern/dpmi.rs - #740: THE DPMI HOST CORE (INT 31h) for DOS/4GW guests.
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
// Pinned rustc 1.97.0, x86_64-unknown-none, panic=abort, no_std, no allocator.
//
// ===========================================================================
// WHAT THIS FILE IS
// ---------------------------------------------------------------------------
// We do not run DOS4GW.EXE. We ARE the extender (docs/DOS4GW_DESIGN.md 5.1), so
// the only DPMI surface that has to exist is the set of INT 31h functions the
// GAME calls. This file is the dispatcher plus the four families that own
// state:
//
//   0400                         get DPMI version
//   0000 0001 0002 0003          LDT descriptor allocation / segment mapping
//   0006 0007 0008 0009 000A     descriptor base / limit / rights / alias
//   0100 0101                    DOS memory block allocate / free
//   0500                         get free memory information
//   0600 0601                    lock / unlock linear region
//
// EXPLICITLY NOT HERE, and the distinction matters (see MISS DISCIPLINE below):
//
//   0300 0301 0302   simulate real-mode interrupt / call real-mode proc.
//                    Owned by a separate agent and attached through
//                    dpmi_set_ext_rs(). The seam is real and is proved by the
//                    boot self-test, case group H.
//   0501 0502        allocate / free memory block. Doom's 0501 is inside
//                    `#if 0` and only C&C [BIN] shows 0502, so
//                    docs/DPMI_BRIDGE_DESIGN.md 2.5 moved them to tier 3: the
//                    guest's heap comes from the extender's own allocation,
//                    not from 0501. MISS until a census line says otherwise.
//   0200 0201        real-mode interrupt vectors (Warcraft II [BIN] only)
//   0204 0205        protected-mode interrupt vectors. Measured tier 3: Doom
//                    and Descent hook through INT 21h AH=25h and the DOS/4GW
//                    auto-passup range instead (DPMI_BRIDGE_DESIGN 2.6), which
//                    is dosexec.c work, not DPMI work.
//   0303 0304        real-mode callbacks. Measured ABSENT from all four target
//                    games. Refused BY DESIGN rather than left a MISS, so the
//                    log distinguishes "we decided" from "nobody looked".
//
// ===========================================================================
// MISS DISCIPLINE, WHICH IS THE POINT OF THE FILE
// ---------------------------------------------------------------------------
// blame.md, "An answer given by OMISSION is not an answer": INT 2Fh 1687h fell
// through to a return that touched no registers, and was RIGHT BY ACCIDENT
// because an untouched AX still held the function number. A probe that poisoned
// ES:DI got its poison back and would have far-called into it.
//
// So every AX this dispatcher does not service leaves here with a DEFINED
// effect, not an untouched register file:
//
//   CF = 1  and  AX = 0x8001 (unsupported function).
//
// CF=1 is what a DPMI client tests. AX is written, never left as the caller's
// own value. Nothing else is touched, because on a failed INT 31h call nothing
// else is defined, and inventing an output is how a caller that skips the CF
// test lands somewhere plausible instead of faulting.
//
// There is no stack effect to get wrong: INT 31h returns through the
// interpreter's own IRET path, so the Win16 argbytes class of desync
// (blame.md, "an UNIMPLEMENTED Win16 import pops the stack wrong") cannot
// occur here. That is a property of the interface, not of this code, and it is
// worth stating so nobody adds a stack adjustment "for symmetry".
//
// THREE CLASSES, all visible on serial and all counted:
//   SERVICED    implemented here; first call per AX logs one line
//   REFUSED     measured absent from every target game; deliberate, logged as
//               "BY DESIGN" so it is never mistaken for an oversight
//   MISS        nobody has looked at it. This is the measurement (2.7).
//
// dpmi_report_rs() dumps the whole per-AX table at guest teardown. That table
// IS the instrument docs/DPMI_BRIDGE_DESIGN.md 2.7 asks to build before the
// function table is complete: the first run of a real binary is a measurement.
//
// ===========================================================================
// WHAT THE DESCRIPTORS ARE, HONESTLY
// ---------------------------------------------------------------------------
// The guest runs in a flat interpreter. There is no real LDT, no LGDT, and no
// CPU that ever loads one of these selectors. A descriptor here is BOOKKEEPING:
// the guest sets a base and a limit, and the 32-bit core resolves a seg:off
// through dpmi_sel_lookup_rs() when it needs to.
//
// That is not a shortcut, it is the whole "be the extender" decision, and it is
// what makes Descent's real sequence work: 0000 (allocate), 0007 (base
// 0x000A0000), 0008 (limit 0xFFFF) is how it reaches the VGA aperture at all
// (2D/VESA.ASM:282-284, DPMI_BRIDGE_DESIGN 2.2). Without the lookup export the
// table would be decorative, which is the "72 declarations, 0 callers" trap
// this project keeps hitting. dpmi_sel_lookup_rs() is the reason it is not.
//
// Selector encoding: (index << 3) | 7, i.e. TI=1 (LDT) and RPL=3, which is what
// a DPMI client's selectors look like. Selector increment is therefore 8, and
// that is the value 0003 returns.
//
// ===========================================================================
// DOS MEMORY: THERE IS EXACTLY ONE ALLOCATOR, AND IT IS NOT THIS FILE
// ---------------------------------------------------------------------------
// DPMI_BRIDGE_DESIGN 3.9: 0100h is a thin wrapper over the EXISTING INT 21h
// AH=48h MCB allocator, because a second allocator over the same megabyte is
// how the three-INT-21h fault started (dos/int21svc.h). This file therefore
// contains NO allocator. It holds a bound vtable (dpmi_bind_dosmem_rs) and
// calls it, exactly as dos_svc_ctx_t holds ctx->mem rather than computing a
// linear address itself.
//
// Unbound, 0100 fails with CF=1 / AX=0x0008 (DOS "insufficient memory") and
// BX=0, which is a correct DPMI failure, not a crash. That matters right now,
// because the 32-bit execution path does not exist yet: an unbound host must
// refuse cleanly rather than pretend.
//
// ===========================================================================
// 0500h: THE FIELD DOOM SIZES ITS ENTIRE HEAP FROM, AND ELEVEN WE MOSTLY
// CANNOT HONESTLY ANSWER
// ---------------------------------------------------------------------------
// 0500h fills a 48-byte block of twelve dwords at ES:EDI.
// docs/DPMI_BRIDGE_DESIGN.md 2.5 measured why it is tier 1 and why it is
// unusually dangerous: `meminfo[0]` is what Doom's I_ZoneBase()/I_GetHeapSize()
// size THE WHOLE ZONE HEAP from (i_ibm.c:1491, :1515). A wrong answer is not a
// failed call. It is a game that runs with the wrong heap and dies later of
// corruption or exhaustion, at a point where nobody will suspect a DPMI call.
//
// THE VALIDATION ASYMMETRY POINTS THE OPPOSITE WAY FROM 0600h, AND THAT IS THE
// WHOLE DESIGN OF THIS FUNCTION. For 0600h, inventing success is free and
// refusing is fatal, so it never fails. Here, refusing is loud and diagnosable
// while inventing a plausible number is silently catastrophic. So 0500h REFUSES
// rather than guesses, and every field it cannot determine is 0xFFFFFFFF.
//
// That sentinel is not our invention: the DPMI specification defines exactly
// this function's unsupported fields as -1, and states that only the FIRST
// field is guaranteed valid. It also happens to be the right engineering
// choice for the reason this project keeps rediscovering: 0xFFFFFFFF is LOUD.
// A plausible page count is silent. If a target ever misbehaves after a 0500h,
// the distinctive value is what makes it findable.
//
//  off  dword  field                                  our answer
//  ---  -----  -------------------------------------  --------------------
//  00h   [0]   largest available free block, BYTES    model            KNOWN
//  04h   [1]   maximum unlocked page allocation       0xFFFFFFFF     UNKNOWN
//  08h   [2]   maximum locked page allocation         0xFFFFFFFF     UNKNOWN
//  0Ch   [3]   linear address space size, PAGES       model            KNOWN
//  10h   [4]   total number of unlocked pages         0xFFFFFFFF     UNKNOWN
//  14h   [5]   total number of free pages             model            KNOWN
//  18h   [6]   total number of physical pages         0xFFFFFFFF     UNKNOWN
//  1Ch   [7]   free linear address space, PAGES       model            KNOWN
//  20h   [8]   size of paging file/partition, PAGES   0                KNOWN
//  24h   [9]   reserved                               0xFFFFFFFF
//  28h  [10]   reserved                               0xFFFFFFFF
//  2Ch  [11]   reserved                               0xFFFFFFFF
//
// Why each UNKNOWN is unknown, because "we did not bother" and "it is not
// knowable" must not look the same:
//
//   [1] [2] maximum unlocked / locked page allocation. These are POLICY
//           statements about a FUTURE allocation, and the allocator is a
//           binding this file does not own (see DOS MEMORY above). Answering
//           with the free-page count would assert something about the bound
//           allocator that this file cannot know. It is very nearly true and
//           that is exactly what makes it the dangerous kind of wrong.
//   [4]     total unlocked pages. We do not track page lock state AT ALL:
//           0600h is bookkeeping over BYTE RANGES and nothing is ever paged
//           out, so there is no unlocked-page population to count. Any number
//           here would be fabricated.
//   [6]     total physical pages. The guest's memory is a kernel-heap arena;
//           the host's real physical page count is not the guest's, and the
//           guest has no physical pages of its own. Reporting the arena size
//           here would be a plausible lie about a different quantity.
//
// And why [8] is a KNOWN ZERO rather than an UNKNOWN: there is no paging file
// and there never will be, so 0 is the TRUE answer. Marking a fact we do know
// as "unknown" is its own small dishonesty, and it would deny a client the one
// piece of information that tells it swapping cannot happen here.
//
// [5] and [7] are deliberately the same number. For a single flat arena the
// free physical memory and the free linear address space ARE the same bytes;
// they diverge only where a host has separate page and address-space
// allocators, which we do not.
//
// The LAYOUT LIVES HERE, not in the memory model. The bound model reports three
// plain numbers and this file places them, so a guest owner cannot get the
// field order wrong. A block of plausible-looking numbers in the wrong order is
// precisely the bug a weak test passes, so the boot self-test asserts all
// twelve dwords individually against three deliberately DISTINCT fixture
// values (case group J).
//
// ===========================================================================
// LOCKING, AND WHY IT NEVER FAILS
// ---------------------------------------------------------------------------
// 0600/0601 were tier 3 ("a no-op for us") until DPMI_BRIDGE_DESIGN 2.5
// measured that Doom locks its ENTIRE program image at startup
// (_dpmi_lockregion(&__begtext, &___argc - &__begtext), i_ibm.c:1242) and
// Descent makes nine lock calls before installing its keyboard ISR alone. The
// IMPLEMENTATION is correctly a no-op; the TIER was wrong. A CF=1 from 0600
// aborts Doom before anything renders.
//
// The risk is asymmetric and that decides the design:
//   * succeeding on a region we should have rejected costs NOTHING, because
//     nothing in this kernel is ever paged out;
//   * failing on a region we should have accepted aborts the game.
// So 0600 NEVER returns CF=1. Regions are recorded (nesting counted, per the
// DPMI rule that locks nest and need matching unlocks) so the call is
// OBSERVABLE, and anything odd (table full, unmatched unlock, zero size) is
// counted and logged rather than turned into a failure the guest cannot
// survive. A no-op that cannot be distinguished from an absent implementation
// would be the same "did it ever run?" trap; the counters are what make it
// distinguishable.

// ===========================================================================
// ONE HOST, ONE GUEST. STATED SO NOBODY ASSUMES OTHERWISE.
// ---------------------------------------------------------------------------
// Every table below is a module static, so this file holds exactly ONE DPMI
// host. That is correct for today (the DOS layer runs at most one guest, and
// g_dos_busy enforces it) and it is NOT a per-guest design. If a second
// concurrent DOS/4GW guest is ever allowed, these tables must move onto the
// guest object and be reached through cpu->owner, exactly as #736 Stage 1b
// moved the interpreter's int/in/out hooks, pmode flag and software x87 stack
// off file-scope statics, after the single global g_int_handler let a Win16
// launch silently answer a RUNNING DOS game's INT 21h.
//
// Written down rather than left implicit because that fault was found by
// measurement, not by reading, and the shape here is identical. There is no
// locking either: the host runs on the guest's own kernel thread and is not
// re-entrant.
// ===========================================================================

// ---------------------------------------------------------------------------
// The protected-mode register frame the dispatcher operates on.
//
// This is the boundary between the 32-bit execution core (which does not exist
// yet, and is another agent's) and the DPMI host. Layout is locked from the C
// side by _Static_assert in dos/dpmi.h; a silently wrong offset here would
// corrupt a register on every DPMI call the guest makes.
// ---------------------------------------------------------------------------
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

// DPMI error codes (DPMI 1.0 numbering; 0.9 hosts return the same values).
const E_UNSUPPORTED: u16 = 0x8001; // unsupported function
const E_DESC_UNAVAIL: u16 = 0x8011; // descriptor unavailable
const E_INVALID_VALUE: u16 = 0x8021; // invalid value
const E_INVALID_SEL: u16 = 0x8022; // invalid selector
const E_INVALID_LINEAR: u16 = 0x8025; // invalid linear address
// DOS error code, which is what 0100 returns on failure (NOT a 0x80xx code).
const E_DOS_NOMEM: u16 = 0x0008;

const CF: u32 = 1; // EFLAGS.CF

// 256 LDT descriptors. Descent allocates one, C&C a handful, Warcraft II [BIN]
// shows 0000 sites but no count. 256 is far more than any measured target needs
// and costs 4 KiB of BSS; running out returns 0x8011, which is a legal DPMI
// answer rather than a crash.
const NDESC: usize = 256;
// Index 0 is never handed out: selector 0x0007 would still be a null selector
// in the RPL/TI sense a client may test.
const FIRST_DESC: usize = 1;

const KIND_FREE: u8 = 0;
const KIND_LDT: u8 = 1; // plain 0000 allocation
const KIND_SEGMAP: u8 = 2; // 0002 real-mode segment -> selector
const KIND_DOSBLK: u8 = 3; // the selector half of a 0100 DOS memory block
const KIND_ALIAS: u8 = 4; // 000A alias of another descriptor

// Default contents of a freshly allocated descriptor: expand-up writable data,
// present, DPL 3 (0xF3), 32-bit default (D/B set in the extended byte), base 0,
// limit 0. A client is expected to fill these in with 0007/0008/0009.
const AR_DATA_RW: u8 = 0xF3;
const EXT_32BIT: u8 = 0x40;

#[derive(Clone, Copy)]
struct Desc {
    kind: u8,
    ar: u8,   // access-rights byte (descriptor byte 5)
    ext: u8,  // extended byte (descriptor byte 6), G/D-B/AVL in the high nibble
    base: u32,
    limit: u32,     // BYTE limit, as the client set it
    dos_seg: u16,   // KIND_DOSBLK only
    dos_paras: u16, // KIND_DOSBLK only
}

const DESC_INIT: Desc = Desc {
    kind: KIND_FREE,
    ar: 0,
    ext: 0,
    base: 0,
    limit: 0,
    dos_seg: 0,
    dos_paras: 0,
};

static mut DESCS: [Desc; NDESC] = [DESC_INIT; NDESC];

// ---------------------------------------------------------------------------
// Locked linear regions. See "LOCKING" above: this table exists to make the
// no-op observable, never to make it fail.
// ---------------------------------------------------------------------------
const NLOCK: usize = 64;

#[derive(Clone, Copy)]
struct LockRgn {
    used: u8,
    base: u32,
    size: u32,
    nest: u32,
}

const LOCK_INIT: LockRgn = LockRgn {
    used: 0,
    base: 0,
    size: 0,
    nest: 0,
};

static mut LOCKS: [LockRgn; NLOCK] = [LOCK_INIT; NLOCK];

// ---------------------------------------------------------------------------
// (#211) PROTECTED-MODE INTERRUPT VECTORS (0204/0205) AND PROCESSOR EXCEPTION
// HANDLERS (0202/0203).
//
// TWO TABLES, NOT ONE, AND NEITHER IS THE REAL-MODE TABLE. A DPMI client has
// three distinct vector spaces and a host that folds any two of them together
// produces a client whose Ctrl-C handler runs on a page fault. RMVEC above is
// the real-mode space (0200/0201); PMVEC is what the CPU would use for an INT
// executed in protected mode; EXCVEC is the processor's own exception space,
// which overlaps PMVEC numerically (0..0x1F) and is deliberately SEPARATE
// because DPMI defines them as separate calls with separate defaults.
//
// WHAT THIS HOST DOES AND DOES NOT PROMISE, stated rather than implied:
// the tables are STORED AND RETURNED FAITHFULLY, and nothing in this kernel
// ever DELIVERS to them. djgpp installs 18 exception handlers and a couple of
// protected-mode vectors during startup and, on a run that does not fault, has
// never called one; that is the case #211 needed. A guest that genuinely
// divides by zero will not have its handler run - it will stop at the
// interpreter's own FAULT_DIV exit, which names the EIP. That is a worse
// outcome than a real DPMI host gives and a much better one than a silent
// refusal at startup, which is what these calls got before.
//
// An unset vector reads back as selector 0, offset 0. A client that CHAINS to
// it would jump to a null selector; djgpp only chains from inside a handler it
// has entered, so on a fault-free run nothing reads these back except its own
// teardown, which compares them and restores.
// ---------------------------------------------------------------------------
#[derive(Clone, Copy)]
struct FarPtr {
    sel: u16,
    off: u32,
}

const FARPTR_NULL: FarPtr = FarPtr { sel: 0, off: 0 };

static mut PMVEC: [FarPtr; 256] = [FARPTR_NULL; 256];
// (rakbd) One "installed" line per vector per run; see the 0205h arm.
static mut PMVEC_LOGGED: [bool; 256] = [false; 256];

/// (rakbd) Read back the protected-mode handler a client installed with
/// DPMI 0205h, for the ONE caller that now needs to DELIVER to it.
///
/// Until this existed, PMVEC was write-only from the host's point of view: the
/// file's own comment two hundred lines up says "nothing in this kernel ever
/// DELIVERS to them", and that was the whole reason a DOS/4GW guest which
/// installs its keyboard ISR this way received nothing. A faithful store is
/// indistinguishable from a working install from the guest's side, which is why
/// the gap survived so long.
///
/// Returns 0 and fills sel/off when a non-null handler is installed for `vec`,
/// -1 otherwise. An unset vector reads back as selector 0 offset 0, which is
/// not a handler, so it is reported as absent rather than delivered to.
#[no_mangle]
pub unsafe extern "C" fn dpmi_pmvec_get_rs(vec: u8, out_sel: *mut u16, out_off: *mut u32) -> i32 {
    let h = unsafe { PMVEC[vec as usize] };
    if h.sel == 0 && h.off == 0 {
        return -1;
    }
    if !out_sel.is_null() {
        unsafe { *out_sel = h.sel };
    }
    if !out_off.is_null() {
        unsafe { *out_off = h.off };
    }
    0
}
static mut EXCVEC: [FarPtr; 32] = [FARPTR_NULL; 32];

// (#211) 0E01: what the client last asked for. Recorded so the request is
// OBSERVABLE, and deliberately not acted on: rustkern/x87.rs is unconditional,
// so there is no coprocessor to enable or disable. Reporting success while
// doing nothing is honest here in a way it usually is not, because the client
// asks for "an FPU that works" and gets exactly that either way.
static mut NPX_EMU_FLAGS: u16 = 0;
static mut NPX_EMU_CALLS: u32 = 0;

// ---------------------------------------------------------------------------
// THE REAL-MODE INTERRUPT VECTORS (DPMI 0200h / 0201h), packed (seg << 16) | off.
//
// A SHADOW, deliberately, and NOT the arena's first kilobyte where a real
// host's real-mode IVT lives: for a 32-bit guest those bytes are already the
// PROTECTED-mode vector table (dos4gw_seed_pm_ivt seeds them, INT 21h AH=25h
// writes a flat 32-bit handler into them, dos4gw_deliver reads it back). One
// table cannot mean both a flat PM address and a real-mode seg:off, and the
// #740 derail this round began with is what it looks like when it tries.
//
// Initialised to the F000:FF53 IRET stub the per-launch setup writes into the
// arena, so a vector nobody has set reads back as a real address holding a
// real IRET.
const RMVEC_INIT: u32 = (0xF000u32 << 16) | 0xFF53u32;

/// (rakbd2) THE VECTORS WHOSE CORRECT DEFAULT IS NOTHING AT ALL.
///
/// 60h-66h are the DOS "user interrupt" vectors. Neither the BIOS nor DOS ever
/// writes them, so on a real machine 0200h answers 0000:0000 for each of them
/// until a TSR claims one, and a program that wants a vector of its own SCANS
/// them for exactly that value.
///
/// Seeding them with the IRET stub, as every other vector legitimately is,
/// tells such a program that all six are taken. MEASURED on Red Alert 1.04 DOS:
/// its Install_Keyboard_Interrupt scans 60h..65h with 0200h, exhausts the loop,
/// and returns failure WITHOUT ever calling 0204h/0205h/0201h to install its
/// INT 9 handler. The keyboard was never installed, so no amount of delivery
/// work on our side could be reached; see dos/dosexec.c dos_vec_seed_free() for
/// the disassembly and the census that proves it.
///
/// This is the same shape as the 33h mouse bug (raplay): a blanket default
/// defeats a guest that INSPECTS a vector's value. There the wrong answer was
/// "an IRET is there"; here it is "something is there". One predicate, stated
/// once, so the answer cannot depend on which of the two seeding paths ran.
const fn rmvec_default(v: usize) -> u32 {
    if v >= 0x60 && v <= 0x66 {
        0
    } else {
        RMVEC_INIT
    }
}

const fn rmvec_defaults() -> [u32; 256] {
    let mut t = [RMVEC_INIT; 256];
    let mut v = 0usize;
    while v < 256 {
        t[v] = rmvec_default(v);
        v += 1;
    }
    t
}

static mut RMVEC: [u32; 256] = rmvec_defaults();

/// (#dpmi301) WHICH VECTORS THE GUEST INSTALLED ITSELF, as opposed to the stub
/// this host seeded.
///
/// The two cannot be told apart by VALUE. dpmi_rmvec_seed_rs() overwrites RMVEC
/// for every vector at launch with whatever bytes dos_vec_seed_stub() chose, so
/// comparing an entry against rmvec_default() would classify every seeded
/// vector as guest-installed and hand our own IRET stub to an interpreter as
/// though it were a driver. A one-bit provenance flag is exact and needs no
/// heuristic: it is set ONLY by the 0201h arm, i.e. only where a
/// protected-mode client deliberately published a real-mode entry point.
///
/// WHY THIS EXISTS, MEASURED on Discworld II (build 2270, run3-inifmt):
/// the Miles Sound System does NOT reach its real-mode digital-audio driver
/// with DPMI 0301h, and never issues 0301h at all. It reads DIG.INI, loads
/// SBLASTER.DIG into DOS memory with 0100h, publishes the driver's real-mode
/// entry point as INT 66h with 0201h, and then calls the driver with 0300h,
/// carrying the AIL function number in AX (0300h, 0301h and 0304h were the
/// three observed, which are AIL function numbers and NOT DPMI ones: that
/// coincidence is exactly how this was misread as a missing DPMI 03xx family).
/// Without this flag a host cannot tell that request apart from a request to
/// simulate a BIOS service it happens not to implement, and answers the
/// documented MISS, which is what left DIG_DRIVER at zero.
static mut RMVEC_GUEST: [u8; 256] = [0u8; 256];

/// Report a real-mode vector the GUEST published with 0201h.
///
/// Returns 1 and fills `seg`/`off` when this vector carries guest code, 0 when
/// it does not (a host stub, or never set). A zero return means "do not
/// execute": it is the difference between running a driver and running our own
/// IRET.
///
/// # Safety
/// `seg` and `off` may each be NULL; any non-NULL one must be writable.
#[no_mangle]
pub unsafe extern "C" fn dpmi_rmvec_guest_rs(vec: u8, seg: *mut u16, off: *mut u16) -> i32 {
    let v = vec as usize;
    if RMVEC_GUEST[v] == 0 {
        return 0;
    }
    let packed = RMVEC[v];
    // A guest that published 0000:0000 published "nothing is here". Executing
    // address zero is the interrupt vector table, so this is refused rather
    // than treated as code.
    if packed == 0 {
        return 0;
    }
    if !seg.is_null() {
        *seg = (packed >> 16) as u16;
    }
    if !off.is_null() {
        *off = lo16(packed);
    }
    1
}

// (raplay) AND "THE F000:FF53 IRET STUB" IS NOT WHAT THE ARENA HOLDS FOR EVERY
// VECTOR, WHICH IS THE WHOLE OF THIS BUG.
//
// The per-launch setup does not write one stub into all 256 IVT entries. It
// asks dos_vec_seed_stub() per vector, and vector 33h gets `CD 33 CB`
// (INT 33h; RETF) at F000:FF54 instead of the IRET, for a reason dosexec.c
// spells out: the documented mouse-driver test reads the INT 33h vector and
// treats a 0xCF (IRET) at the target as "no driver installed".
//
// This shadow was initialised from a SIMPLIFICATION of that, one constant for
// all 256, so DPMI 0200h answered F000:FF53 for vector 33h and every
// protected-mode guest was told there is no mouse. MEASURED on Red Alert,
// which does exactly the documented test through DPMI rather than through DOS:
//
//     mov  $0x200,%eax ; mov $0x33,%bl ; int $0x31   ; get real-mode vector 33h
//     mov  %cx,%ax ; or %dx,%ax ; je no_driver       ; 0000:0000 -> none
//     shl  $4,%ecx ; add %edx,%ecx ; cmpb $0xcf,(%ecx)
//     je   no_driver                                  ; IRET there -> none
//
// It put up "Red Alert is unable to detect your mouse driver." and never
// issued a single INT 33h call, so nothing in the mouse driver could be
// reached.
//
// The fix is NOT a second copy of the stub table here. dos_vec_seed_stub() in
// dosexec.c stays the one chooser; the C side calls dpmi_rmvec_seed_rs() for
// each vector as it writes the arena, so the shadow and the bytes it describes
// come from the same decision and cannot drift apart again.
#[no_mangle]
pub extern "C" fn dpmi_rmvec_seed_rs(vec: u8, seg: u16, off: u16) {
    // (#dpmi301) Seeding is the HOST writing a stub, so it CLEARS the
    // guest-provenance bit. Without this a relaunch would inherit the previous
    // guest's claim on a vector that now holds our own stub bytes.
    unsafe {
        RMVEC[vec as usize] = ((seg as u32) << 16) | (off as u32);
        RMVEC_GUEST[vec as usize] = 0;
    };
}
static mut LOCK_CALLS: u32 = 0;
static mut UNLOCK_CALLS: u32 = 0;
static mut LOCK_TABLE_FULL: u32 = 0;
static mut UNLOCK_UNMATCHED: u32 = 0;
static mut LOCK_ZERO_SIZE: u32 = 0;

// ---------------------------------------------------------------------------
// The bound DOS-memory allocator (see "DOS MEMORY" above). Not an allocator:
// a binding to the ONE that already exists.
//
//   alloc(user, paras, out_seg, out_largest) -> 0 on success (out_seg set),
//                                               nonzero on failure with
//                                               out_largest = the largest
//                                               available block in paragraphs
//   free(user, seg) -> 0 on success
// ---------------------------------------------------------------------------
type DosAllocFn = extern "C" fn(*mut u8, u16, *mut u16, *mut u16) -> i32;
type DosFreeFn = extern "C" fn(*mut u8, u16) -> i32;

static mut DOSMEM_ALLOC: Option<DosAllocFn> = None;
static mut DOSMEM_FREE: Option<DosFreeFn> = None;
static mut DOSMEM_USER: *mut u8 = core::ptr::null_mut();

// ---------------------------------------------------------------------------
// The extension seam. 0300/0301/0302 attach here. Returns 1 if it handled the
// call (having set CF and the registers itself), 0 to fall through to this
// file's MISS path.
//
// This is the same shape as dos_svc_ctx_t::extend, deliberately: a caller's own
// functions plug in, and the core dispatches to it only for AX values it does
// not service itself. It means the 0300 work needs no edit to this dispatcher.
// ---------------------------------------------------------------------------
type ExtFn = extern "C" fn(*mut u8, *mut DpmiRegs, u16) -> i32;

static mut EXT_FN: Option<ExtFn> = None;
static mut EXT_USER: *mut u8 = core::ptr::null_mut();

// ---------------------------------------------------------------------------
// The guest's flat memory window, and THE bounds-check chokepoint for anything
// this host writes into guest space.
//
// This is dpmi_arena_t from dos/dpmi_rmcs.h, NOT a second copy of the idea:
// 0300h's marshaller already bounds-checks a flat guest address against exactly
// this struct, and `_Static_assert`s in dos/dpmi.c lock our view of it against
// theirs in both size and field offset. Two independent bounds-check stories
// for one guest address space is how a "did we bounds-check?" question stops
// having one answer (docs/DPMI_BRIDGE_DESIGN.md 3.10), so there is one struct
// and the counters below are theirs.
//
// `oob_wr` counts writes that left the arena. They are REFUSED, never clamped
// and never folded into a spare page: the Win16 arena's guard-page behaviour
// was right for chasing a Word 6 rendering bug and is wrong for a service
// bridge, because it turns a guest pointer error into silent corruption of the
// guest's own data.
// ---------------------------------------------------------------------------
#[repr(C)]
pub struct DpmiArena {
    pub base: *mut u8,
    pub size: u32,
    pub oob_rd: u32,
    pub oob_wr: u32,
}

static mut ARENA: *mut DpmiArena = core::ptr::null_mut();

// ---------------------------------------------------------------------------
// The free-memory model behind 0500h. Reports three plain numbers; this file
// owns where they go in the block (see the 0500h section above). Returns 0 on
// success, nonzero if the model cannot answer, in which case 0500h REFUSES
// rather than reporting a guess.
// ---------------------------------------------------------------------------
type MemInfoFn = extern "C" fn(*mut u8, *mut u32, *mut u32, *mut u32) -> i32;

static mut MEMINFO_FN: Option<MemInfoFn> = None;
static mut MEMINFO_USER: *mut u8 = core::ptr::null_mut();

// One-shot log flags, so an unprovisioned host says so once rather than once
// per call from a guest that asks every level load.
static mut WARNED_NO_ARENA: u8 = 0;
static mut WARNED_NO_MEMINFO: u8 = 0;

// ---------------------------------------------------------------------------
// Per-AX call census. THE MEASURING INSTRUMENT (DPMI_BRIDGE_DESIGN 2.7).
// ---------------------------------------------------------------------------
const NSEEN: usize = 96;

const CLS_SERVICED: u8 = 1;
const CLS_REFUSED: u8 = 2;
const CLS_MISS: u8 = 3;
const CLS_EXT: u8 = 4;

#[derive(Clone, Copy)]
struct Seen {
    ax: u16,
    cls: u8,
    calls: u32,
}

const SEEN_INIT: Seen = Seen {
    ax: 0,
    cls: 0,
    calls: 0,
};

static mut SEEN_TAB: [Seen; NSEEN] = [SEEN_INIT; NSEEN];
static mut SEEN_N: usize = 0;
static mut SEEN_OVERFLOW: u32 = 0;
static mut N_CALLS: u32 = 0;
static mut N_MISS: u32 = 0;
static mut N_FAIL: u32 = 0;

// Suppresses the per-AX first-call kprintf during the boot self-test, which
// deliberately drives ~30 calls including MISSes and would otherwise bury its
// own PASS/FAIL lines in first-call chatter.
static mut QUIET: u8 = 0;

extern "C" {
    // serial.h. Variadic; kept out of the declared FFI surface deliberately
    // (see rustkern/permpath.rs), used here the same way rustkern/instdisk.rs
    // uses it: a DECISION visible on serial rather than inferred.
    fn kprintf(fmt: *const u8, ...);
}

#[inline]
fn lo16(v: u32) -> u16 {
    (v & 0xFFFF) as u16
}

/// Write AX without disturbing the high half of EAX. A DPMI host writes AX; a
/// client that kept something in the high half of EAX across the call (Watcom
/// does) must get it back.
#[inline]
fn set_ax(r: &mut DpmiRegs, v: u16) {
    r.eax = (r.eax & 0xFFFF_0000) | (v as u32);
}

#[inline]
fn set_bx(r: &mut DpmiRegs, v: u16) {
    r.ebx = (r.ebx & 0xFFFF_0000) | (v as u32);
}

#[inline]
fn set_cx(r: &mut DpmiRegs, v: u16) {
    r.ecx = (r.ecx & 0xFFFF_0000) | (v as u32);
}

#[inline]
fn set_dx(r: &mut DpmiRegs, v: u16) {
    r.edx = (r.edx & 0xFFFF_0000) | (v as u32);
}

#[inline]
fn succeed(r: &mut DpmiRegs) {
    r.eflags &= !CF;
}

/// The ONE failure path. CF=1 and a DEFINED AX, never an untouched register
/// file (blame.md: "An answer given by OMISSION is not an answer").
#[inline]
fn fail(r: &mut DpmiRegs, code: u16) {
    r.eflags |= CF;
    set_ax(r, code);
    unsafe { N_FAIL += 1 };
}

// ---------------------------------------------------------------------------
// Selector <-> descriptor index
// ---------------------------------------------------------------------------
#[inline]
fn sel_of(idx: usize) -> u16 {
    ((idx as u16) << 3) | 7
}

/// Resolve a client selector to a LIVE descriptor index, or None.
///
/// TI (bit 2) must be set: a TI=0 selector is a GDT selector, which under a
/// real DOS/4GW is one of the extender's own flat selectors and is not ours to
/// modify. Refusing it with 0x8022 is correct and is also the honest answer,
/// because we do not know what it is.
///
/// RPL (bits 0-1) is NOT checked. A client may present its selectors with any
/// RPL, and rejecting on RPL would be a false failure of exactly the kind that
/// aborts a game.
fn idx_of_live(sel: u16) -> Option<usize> {
    if (sel & 4) == 0 {
        return None;
    }
    let idx = (sel >> 3) as usize;
    if idx < FIRST_DESC || idx >= NDESC {
        return None;
    }
    if unsafe { DESCS[idx].kind } == KIND_FREE {
        return None;
    }
    Some(idx)
}

/// Allocate `count` CONTIGUOUS descriptors, returning the first index.
/// DPMI 0000 promises contiguity: the client walks the block with the selector
/// increment from 0003, so a scattered allocation would be silently wrong.
fn alloc_descs(count: usize) -> Option<usize> {
    if count == 0 || count > NDESC {
        return None;
    }
    let mut i = FIRST_DESC;
    while i + count <= NDESC {
        let mut ok = true;
        let mut j = 0;
        while j < count {
            if unsafe { DESCS[i + j].kind } != KIND_FREE {
                ok = false;
                break;
            }
            j += 1;
        }
        if ok {
            let mut j = 0;
            while j < count {
                unsafe {
                    DESCS[i + j] = Desc {
                        kind: KIND_LDT,
                        ar: AR_DATA_RW,
                        ext: EXT_32BIT,
                        base: 0,
                        limit: 0,
                        dos_seg: 0,
                        dos_paras: 0,
                    };
                }
                j += 1;
            }
            return Some(i);
        }
        // Skip past the occupied slot that broke the run rather than retrying
        // every offset inside it.
        i += j + 1;
    }
    None
}

// ---------------------------------------------------------------------------
// Census
// ---------------------------------------------------------------------------
fn census(ax: u16, cls: u8) {
    unsafe {
        N_CALLS += 1;
        let mut i = 0;
        while i < SEEN_N {
            if SEEN_TAB[i].ax == ax {
                SEEN_TAB[i].calls += 1;
                // A class can legitimately change: an AX that was a MISS
                // becomes CLS_EXT the moment 0300's owner binds its hook.
                SEEN_TAB[i].cls = cls;
                return;
            }
            i += 1;
        }
        if SEEN_N >= NSEEN {
            SEEN_OVERFLOW += 1;
            return;
        }
        SEEN_TAB[SEEN_N] = Seen {
            ax,
            cls,
            calls: 1,
        };
        SEEN_N += 1;
        if QUIET != 0 {
            return;
        }
        match cls {
            CLS_SERVICED => kprintf(b"[dpmi] INT31 AX=%04x serviced\n\0".as_ptr(), ax as u32),
            CLS_EXT => kprintf(b"[dpmi] INT31 AX=%04x -> extension\n\0".as_ptr(), ax as u32),
            CLS_REFUSED => kprintf(
                b"[dpmi] INT31 AX=%04x NOT IMPLEMENTED BY DESIGN (measured absent from every target game)\n\0"
                    .as_ptr(),
                ax as u32,
            ),
            _ => kprintf(b"[dpmi] INT31 AX=%04x UNIMPLEMENTED\n\0".as_ptr(), ax as u32),
        }
    }
}

// ===========================================================================
// THE DISPATCHER
// ===========================================================================

/// Service one INT 31h call. `r` is the guest's protected-mode register frame;
/// the result (CF plus whatever the function returns) is written back into it.
///
/// Always "handles" the call: an unknown AX leaves with CF=1 and AX=0x8001
/// rather than with the frame untouched.
///
/// # Safety
/// `r` must point at a valid, writable `DpmiRegs`. Touches only this module's
/// tables and the bound callbacks.
#[no_mangle]
pub unsafe extern "C" fn dpmi_int31_rs(r: *mut DpmiRegs) {
    if r.is_null() {
        return;
    }
    let r = unsafe { &mut *r };
    let ax = lo16(r.eax);

    match ax {
        // ---- version ----------------------------------------------------
        0x0400 => {
            census(ax, CLS_SERVICED);
            // AH = major, AL = minor -> DPMI 0.90.
            set_ax(r, 0x005A);
            // Bit 0: 32-bit host. Bit 1: reflected interrupts do NOT return the
            // processor to real mode. Bit 2: virtual memory supported - ZERO,
            // and that is a deliberate honest answer, not an omission. Both
            // Warcraft II and Command and Conquer are bound to DOS/4GW
            // PROFESSIONAL, which has a VMM (DPMI_BRIDGE_DESIGN 2.0), and open
            // question 2 is whether either binary refuses without one. Claiming
            // VM support we do not have would convert that measurable question
            // into an unexplained hang.
            set_bx(r, 0x0001);
            set_cx(r, 0x0004); // 80486-class client processor
            // DH/DL: virtual master and slave PIC base interrupts, i.e. the
            // ordinary PC values. A client uses these to translate an IRQ
            // number into a vector; answering 0 would send its ISR to vector 0.
            set_dx(r, 0x0870);
            succeed(r);
        }

        // ---- real-mode interrupt vectors --------------------------------
        //
        // BL is the vector in both. 0200h answers CX:DX, 0201h takes CX:DX.
        // Neither can fail for a valid vector, and BL is a byte, so every
        // vector IS valid: there is no error path to get wrong. See the shadow
        // table's comment for what these do and do not promise.
        0x0200 => {
            census(ax, CLS_SERVICED);
            let v = (r.ebx & 0xFF) as usize;
            let packed = unsafe { RMVEC[v] };
            set_cx(r, (packed >> 16) as u16);
            set_dx(r, lo16(packed));
            succeed(r);
        }

        0x0201 => {
            census(ax, CLS_SERVICED);
            let v = (r.ebx & 0xFF) as usize;
            let packed = ((lo16(r.ecx) as u32) << 16) | (lo16(r.edx) as u32);
            // (#dpmi301) THE PROVENANCE BIT. Set here and nowhere else: this is
            // the one call by which a client states "there is real-mode code of
            // mine at this vector", which is exactly the fact 0300h needs in
            // order to EXECUTE it rather than MISS it.
            unsafe {
                RMVEC[v] = packed;
                RMVEC_GUEST[v] = 1;
            };
            succeed(r);
        }

        // ---- descriptors ------------------------------------------------
        0x0000 => {
            census(ax, CLS_SERVICED);
            let n = lo16(r.ecx) as usize;
            if n == 0 {
                fail(r, E_INVALID_VALUE);
                return;
            }
            match alloc_descs(n) {
                Some(i) => {
                    set_ax(r, sel_of(i));
                    succeed(r);
                }
                None => fail(r, E_DESC_UNAVAIL),
            }
        }

        0x0001 => {
            census(ax, CLS_SERVICED);
            let sel = lo16(r.ebx);
            match idx_of_live(sel) {
                Some(i) => {
                    // Freeing the descriptor of a live DOS memory block is a
                    // client error (0101 is how that block is released), but
                    // real hosts free the descriptor and leave the block. Do
                    // the same and SAY SO, rather than returning a CF=1 the
                    // client is not expecting.
                    if unsafe { DESCS[i].kind } == KIND_DOSBLK {
                        unsafe {
                            kprintf(
                                b"[dpmi] 0001: selector %04x is a live DOS block (seg %04x, %u paras); descriptor freed, block LEAKED (client should use 0101)\n\0".as_ptr(),
                                sel as u32,
                                DESCS[i].dos_seg as u32,
                                DESCS[i].dos_paras as u32,
                            );
                        }
                    }
                    unsafe { DESCS[i] = DESC_INIT };
                    succeed(r);
                }
                None => fail(r, E_INVALID_SEL),
            }
        }

        0x0002 => {
            // Segment to descriptor. Descent applies this to 0xA000 and that is
            // how it reaches the VGA aperture at all (2D/VESA.ASM:282-284).
            census(ax, CLS_SERVICED);
            let seg = lo16(r.ebx);
            // The spec says the selector may not be modified or freed by the
            // client, and that repeated calls for the same segment may return
            // the same selector. Returning the SAME one is the safer reading:
            // a client that calls this per frame must not exhaust the LDT.
            let mut i = FIRST_DESC;
            while i < NDESC {
                unsafe {
                    if DESCS[i].kind == KIND_SEGMAP && DESCS[i].dos_seg == seg {
                        set_ax(r, sel_of(i));
                        succeed(r);
                        return;
                    }
                }
                i += 1;
            }
            match alloc_descs(1) {
                Some(i) => {
                    unsafe {
                        DESCS[i] = Desc {
                            kind: KIND_SEGMAP,
                            ar: AR_DATA_RW,
                            ext: EXT_32BIT,
                            base: (seg as u32) << 4,
                            limit: 0xFFFF,
                            dos_seg: seg,
                            dos_paras: 0,
                        };
                    }
                    set_ax(r, sel_of(i));
                    succeed(r);
                }
                None => fail(r, E_DESC_UNAVAIL),
            }
        }

        0x0003 => {
            census(ax, CLS_SERVICED);
            // Selectors are 8 bytes apart. This is the value a client adds to
            // walk a block allocated by 0000, so it must agree with sel_of().
            set_ax(r, 8);
            succeed(r);
        }

        0x0006 => {
            // Get segment base address, CX:DX. DPMI_BRIDGE_DESIGN 3.9 flags
            // this: a client that asks for the base of the selector 0100
            // returned must get segment << 4, and it does, because 0100 builds
            // the descriptor with exactly that base.
            census(ax, CLS_SERVICED);
            match idx_of_live(lo16(r.ebx)) {
                Some(i) => {
                    let base = unsafe { DESCS[i].base };
                    set_cx(r, (base >> 16) as u16);
                    set_dx(r, lo16(base));
                    succeed(r);
                }
                None => fail(r, E_INVALID_SEL),
            }
        }

        0x0007 => {
            census(ax, CLS_SERVICED);
            match idx_of_live(lo16(r.ebx)) {
                Some(i) => {
                    let base = ((lo16(r.ecx) as u32) << 16) | (lo16(r.edx) as u32);
                    unsafe { DESCS[i].base = base };
                    succeed(r);
                }
                None => fail(r, E_INVALID_SEL),
            }
        }

        0x0008 => {
            census(ax, CLS_SERVICED);
            let sel = lo16(r.ebx);
            let limit = ((lo16(r.ecx) as u32) << 16) | (lo16(r.edx) as u32);
            match idx_of_live(sel) {
                Some(i) => {
                    // A limit above 1 MiB can only be expressed with page
                    // granularity, so the low 12 bits must all be set. This is
                    // the ONE validation in the descriptor family that a real
                    // 386 would also enforce, so refusing here is not a false
                    // failure: the client could not have got what it asked for
                    // on real hardware either.
                    if limit > 0x000F_FFFF && (limit & 0xFFF) != 0xFFF {
                        fail(r, E_INVALID_VALUE);
                        return;
                    }
                    unsafe {
                        DESCS[i].limit = limit;
                        // Reflect granularity in the stored extended byte so a
                        // future descriptor dump is not misleading.
                        if limit > 0x000F_FFFF {
                            DESCS[i].ext |= 0x80;
                        } else {
                            DESCS[i].ext &= !0x80;
                        }
                    }
                    succeed(r);
                }
                None => fail(r, E_INVALID_SEL),
            }
        }

        0x0009 => {
            census(ax, CLS_SERVICED);
            let sel = lo16(r.ebx);
            let cx = lo16(r.ecx);
            let arb = (cx & 0xFF) as u8;
            let extb = ((cx >> 8) & 0xFF) as u8;
            match idx_of_live(sel) {
                Some(i) => {
                    // S (bit 4) clear means a SYSTEM descriptor: a gate, a TSS
                    // or an LDT. A DPMI client may not create one, and a client
                    // that asks for one has a bug we should not silently
                    // record. Everything else is stored without judgement,
                    // because a false rejection here aborts a game and this
                    // host never loads these descriptors into a real CPU
                    // anyway.
                    if (arb & 0x10) == 0 {
                        fail(r, E_INVALID_VALUE);
                        return;
                    }
                    unsafe {
                        DESCS[i].ar = arb;
                        DESCS[i].ext = extb & 0xF0;
                    }
                    succeed(r);
                }
                None => fail(r, E_INVALID_SEL),
            }
        }

        // ---- get / set descriptor (#211) ---------------------------------
        //
        // These move a RAW 8-BYTE 386 DESCRIPTOR between the client and this
        // host's table, so the encode and the decode have to be exact
        // inverses. They are written next to each other for that reason, and
        // the self-test round-trips a descriptor through both rather than
        // checking each against a hand-written byte string, which would pass
        // for any pair of consistently wrong transforms.
        //
        // Byte layout (Intel SDM 3.4.5): limit15:0, base15:0, base23:16,
        // access rights, (flags | limit19:16), base31:24.
        0x000B => {
            census(ax, CLS_SERVICED);
            let sel = lo16(r.ebx);
            let i = match idx_of_live(sel) {
                Some(i) => i,
                None => {
                    fail(r, E_INVALID_SEL);
                    return;
                }
            };
            let flat = match resolve_es_edi(r) {
                Some(f) => f,
                None => {
                    fail(r, E_INVALID_SEL);
                    return;
                }
            };
            let (base, limit, arb, extb) =
                unsafe { (DESCS[i].base, DESCS[i].limit, DESCS[i].ar, DESCS[i].ext) };
            // The stored limit is the BYTE limit as the client set it. Put it
            // back in the field the hardware uses, which is a page count when
            // G is set.
            let lf = if (extb & 0x80) != 0 { limit >> 12 } else { limit };
            let mut d = [0u8; 8];
            d[0] = (lf & 0xFF) as u8;
            d[1] = ((lf >> 8) & 0xFF) as u8;
            d[2] = (base & 0xFF) as u8;
            d[3] = ((base >> 8) & 0xFF) as u8;
            d[4] = ((base >> 16) & 0xFF) as u8;
            d[5] = arb;
            d[6] = (extb & 0xF0) | (((lf >> 16) & 0x0F) as u8);
            d[7] = ((base >> 24) & 0xFF) as u8;
            if arena_write_bytes(flat, &d) != 0 {
                fail(r, E_INVALID_LINEAR);
                return;
            }
            succeed(r);
        }

        0x000C => {
            census(ax, CLS_SERVICED);
            let sel = lo16(r.ebx);
            let i = match idx_of_live(sel) {
                Some(i) => i,
                None => {
                    fail(r, E_INVALID_SEL);
                    return;
                }
            };
            let flat = match resolve_es_edi(r) {
                Some(f) => f,
                None => {
                    fail(r, E_INVALID_SEL);
                    return;
                }
            };
            let mut d = [0u8; 8];
            if arena_read_bytes(flat, &mut d) != 0 {
                fail(r, E_INVALID_LINEAR);
                return;
            }
            // Same refusal 0009 makes, for the same reason: a client may not
            // build a system descriptor (gate/TSS/LDT) through this host.
            if (d[5] & 0x10) == 0 {
                fail(r, E_INVALID_VALUE);
                return;
            }
            let lf = (d[0] as u32) | ((d[1] as u32) << 8) | (((d[6] & 0x0F) as u32) << 16);
            let g = (d[6] & 0x80) != 0;
            let byte_limit = if g { (lf << 12) | 0xFFF } else { lf };
            let base = (d[2] as u32)
                | ((d[3] as u32) << 8)
                | ((d[4] as u32) << 16)
                | ((d[7] as u32) << 24);
            unsafe {
                DESCS[i].base = base;
                DESCS[i].limit = byte_limit;
                DESCS[i].ar = d[5];
                DESCS[i].ext = d[6] & 0xF0;
            }
            succeed(r);
        }

        // ---- processor exception handlers (#211) -------------------------
        // BL is the exception number and it is a BYTE, so the only invalid
        // value is one above 0x1F. djgpp asks for all 18 of 0x00-0x11 at
        // startup and sets 17 of them.
        0x0202 => {
            census(ax, CLS_SERVICED);
            let v = (r.ebx & 0xFF) as usize;
            if v >= 32 {
                fail(r, E_INVALID_VALUE);
                return;
            }
            let h = unsafe { EXCVEC[v] };
            set_cx(r, h.sel);
            r.edx = h.off;
            succeed(r);
        }

        0x0203 => {
            census(ax, CLS_SERVICED);
            let v = (r.ebx & 0xFF) as usize;
            if v >= 32 {
                fail(r, E_INVALID_VALUE);
                return;
            }
            unsafe { EXCVEC[v] = FarPtr { sel: lo16(r.ecx), off: r.edx } };
            succeed(r);
        }

        // ---- protected-mode interrupt vectors (#211) ---------------------
        0x0204 => {
            census(ax, CLS_SERVICED);
            let v = (r.ebx & 0xFF) as usize;
            let h = unsafe { PMVEC[v] };
            set_cx(r, h.sel);
            r.edx = h.off;
            succeed(r);
        }

        0x0205 => {
            census(ax, CLS_SERVICED);
            let v = (r.ebx & 0xFF) as usize;
            unsafe { PMVEC[v] = FarPtr { sel: lo16(r.ecx), off: r.edx } };
            // (rakbd) SAY WHICH VECTOR, ONCE PER VECTOR.
            //
            // Before this, a protected-mode client could install an interrupt
            // handler and the whole event left NO trace anywhere: 0205h stored
            // it, 0204h could read it back, and nothing else in the kernel ever
            // looked. So "does this guest own INT 9?" was unanswerable from a
            // serial log, and the honest answer for Red Alert (it does, through
            // THIS call and not through the low table dos_vec_hooked() reads)
            // could not be reached without a custom build. One line per vector
            // per run is cheap and it is the line that settles the question.
            unsafe {
                if !PMVEC_LOGGED[v] {
                    PMVEC_LOGGED[v] = true;
                    kprintf(
                        b"[dpmi] INT %02Xh PROTECTED-MODE handler installed by the guest (0205h) -> %04x:%08x\n\0"
                            .as_ptr(),
                        v as u32,
                        lo16(r.ecx) as u32,
                        r.edx,
                    );
                }
            }
            succeed(r);
        }

        // ---- coprocessor emulation (#211) ---------------------------------
        //
        // Reported as SUCCESS, and here is the whole of what that means: this
        // host has one x87 (rustkern/x87.rs) which is always present and always
        // software. There is no hardware coprocessor to switch off and no
        // client emulator to switch on, so both of the states BX can ask for
        // are already true. The request is counted so that a guest that
        // depends on the distinction shows up as a number in the report rather
        // than as an unexplained difference from real DOS.
        0x0E01 => {
            census(ax, CLS_SERVICED);
            unsafe {
                NPX_EMU_FLAGS = lo16(r.ebx);
                NPX_EMU_CALLS += 1;
            }
            succeed(r);
        }

        0x000A => {
            census(ax, CLS_SERVICED);
            match idx_of_live(lo16(r.ebx)) {
                Some(i) => {
                    let (base, limit) = unsafe { (DESCS[i].base, DESCS[i].limit) };
                    match alloc_descs(1) {
                        Some(j) => {
                            unsafe {
                                DESCS[j] = Desc {
                                    kind: KIND_ALIAS,
                                    // An alias is always a DATA descriptor,
                                    // whatever the original was: aliasing a
                                    // code segment to write through it is the
                                    // entire purpose of the call.
                                    ar: AR_DATA_RW,
                                    ext: DESCS[i].ext,
                                    base,
                                    limit,
                                    dos_seg: 0,
                                    dos_paras: 0,
                                };
                            }
                            set_ax(r, sel_of(j));
                            succeed(r);
                        }
                        None => fail(r, E_DESC_UNAVAIL),
                    }
                }
                None => fail(r, E_INVALID_SEL),
            }
        }

        // ---- DOS memory --------------------------------------------------
        0x0100 => {
            census(ax, CLS_SERVICED);
            let paras = lo16(r.ebx);
            let f = unsafe { DOSMEM_ALLOC };
            let f = match f {
                Some(f) => f,
                None => {
                    // No allocator bound. Refuse cleanly: AX = the DOS error
                    // code (0100 returns a DOS code on failure, not a 0x80xx
                    // one) and BX = 0 paragraphs available. Descent probes with
                    // BX=0xFFFF expecting exactly this shape (BIOS/DPMI.C:107).
                    fail(r, E_DOS_NOMEM);
                    set_bx(r, 0);
                    return;
                }
            };
            let mut seg: u16 = 0;
            let mut largest: u16 = 0;
            let rc = f(
                unsafe { DOSMEM_USER },
                paras,
                &mut seg as *mut u16,
                &mut largest as *mut u16,
            );
            if rc != 0 {
                fail(r, E_DOS_NOMEM);
                set_bx(r, largest);
                return;
            }
            // Order matters: if the descriptor cannot be allocated we must give
            // the DOS memory BACK before failing, or a client that retries
            // leaks the megabyte one call at a time.
            match alloc_descs(1) {
                Some(i) => {
                    unsafe {
                        DESCS[i] = Desc {
                            kind: KIND_DOSBLK,
                            ar: AR_DATA_RW,
                            ext: EXT_32BIT,
                            base: (seg as u32) << 4,
                            // Byte limit of the block, minus one. A zero-
                            // paragraph request yields limit 0, which is what a
                            // zero-length segment looks like.
                            limit: if paras == 0 {
                                0
                            } else {
                                (paras as u32) * 16 - 1
                            },
                            dos_seg: seg,
                            dos_paras: paras,
                        };
                    }
                    set_ax(r, seg);
                    set_dx(r, sel_of(i));
                    succeed(r);
                }
                None => {
                    if let Some(fr) = unsafe { DOSMEM_FREE } {
                        fr(unsafe { DOSMEM_USER }, seg);
                    }
                    fail(r, E_DESC_UNAVAIL);
                }
            }
        }

        0x0101 => {
            census(ax, CLS_SERVICED);
            let sel = lo16(r.edx);
            match idx_of_live(sel) {
                Some(i) => {
                    if unsafe { DESCS[i].kind } != KIND_DOSBLK {
                        fail(r, E_INVALID_SEL);
                        return;
                    }
                    let seg = unsafe { DESCS[i].dos_seg };
                    let rc = match unsafe { DOSMEM_FREE } {
                        Some(fr) => fr(unsafe { DOSMEM_USER }, seg),
                        None => -1,
                    };
                    if rc != 0 {
                        fail(r, E_DOS_NOMEM);
                        return;
                    }
                    unsafe { DESCS[i] = DESC_INIT };
                    succeed(r);
                }
                None => fail(r, E_INVALID_SEL),
            }
        }

        // ---- free memory information --------------------------------------
        0x0500 => {
            // THE OWNER OF THE MEMORY ANSWERS FIRST (#740).
            //
            // 0501 (allocate) and 0502 (free) have no arm in this file, so they
            // fall to the extension seam below and are answered by the host
            // that actually owns the pool. 0500 had an arm, so it was answered
            // HERE instead, by the generic model, and the two halves of one
            // memory model were served by different owners. On a DOS/4GW guest
            // the generic model has no arena bound at all, so the answer was
            // "REFUSING (CF=1)".
            //
            // MEASURED, and it is the whole visible failure: Discworld II sizes
            // its heap from 0500h, got the refusal, and printed "Discworld
            // needs more extended memory." dos4gw_dpmi_mem_rs() had implemented
            // 0500 against the real arena since the bridge was written and
            // COULD NEVER RUN, because this arm shadowed it. A zero-callers
            // implementation is this tree's characteristic defect, and the fix
            // is to route, not to write a second one.
            //
            // The fallback is unchanged: a host with no extension, or one that
            // declines, still gets meminfo_0500().
            if try_ext(r, ax) {
                return;
            }
            census(ax, CLS_SERVICED);
            meminfo_0500(r);
        }

        // ---- locking ------------------------------------------------------
        0x0600 => {
            census(ax, CLS_SERVICED);
            let base = ((lo16(r.ebx) as u32) << 16) | (lo16(r.ecx) as u32);
            let size = ((lo16(r.esi) as u32) << 16) | (lo16(r.edi) as u32);
            lock_region(base, size);
            succeed(r);
        }

        0x0601 => {
            census(ax, CLS_SERVICED);
            let base = ((lo16(r.ebx) as u32) << 16) | (lo16(r.ecx) as u32);
            let size = ((lo16(r.esi) as u32) << 16) | (lo16(r.edi) as u32);
            unlock_region(base, size);
            succeed(r);
        }

        // ---- refused by design, not by omission ---------------------------
        0x0303 | 0x0304 => {
            // Real-mode callbacks. Measured absent from Doom, Descent, C&C and
            // Warcraft II (DPMI_BRIDGE_DESIGN 2.5). If one of these ever fires,
            // the census line is the finding: a target does something all four
            // measured targets did not, and the tier list needs revisiting.
            census(ax, CLS_REFUSED);
            fail(r, E_UNSUPPORTED);
        }

        // ---- everything else ----------------------------------------------
        _ => {
            // The extension seam: 0300/0301/0302 land here until their owner
            // binds a hook, and route to it afterwards without this file
            // changing. Anything the hook declines falls through to MISS.
            if try_ext(r, ax) {
                return;
            }
            census(ax, CLS_MISS);
            unsafe { N_MISS += 1 };
            fail(r, E_UNSUPPORTED);
        }
    }
}

/// The DPMI "field not supported" sentinel. The specification's own encoding
/// for a field the host cannot determine, and a value no plausible page count
/// can be mistaken for.
const UNKNOWN: u32 = 0xFFFF_FFFF;

/// Number of dwords in the 0500h free-memory information block. The block is
/// 0x30 bytes; a host that writes 0x20 or 0x80 corrupts the client either way,
/// so the length is asserted by the self-test rather than assumed.
const MEMINFO_DWORDS: usize = 12;

/// Resolve the client's ES:EDI to a flat guest address.
///
/// A TI=0 selector is a GDT selector, which under a real DOS/4GW is one of the
/// extender's own flat selectors with base 0, so the flat address is EDI. A
/// TI=1 selector is one of ours and MUST be live: silently treating a freed
/// LDT selector as base 0 would put the client's block at a plausible wrong
/// address instead of failing.
///
/// Returns None if the selector is an LDT selector we do not have.
/// Offer this call to the bound extension. Returns true when the extension
/// handled it (census already recorded), false when there is none or it
/// declined and the caller must go on to its own answer.
///
/// ONE definition, used by both the explicit 0500 arm and the catch-all, so
/// "who gets first refusal" cannot drift between them the way it already did.
fn try_ext(r: &mut DpmiRegs, ax: u16) -> bool {
    if let Some(ext) = unsafe { EXT_FN } {
        if ext(unsafe { EXT_USER }, r as *mut DpmiRegs, ax) != 0 {
            census(ax, CLS_EXT);
            return true;
        }
    }
    false
}

fn resolve_es_edi(r: &DpmiRegs) -> Option<u32> {
    let es = r.es;
    if (es & 4) == 0 {
        return Some(r.edi);
    }
    match idx_of_live(es) {
        Some(i) => Some(unsafe { DESCS[i].base }.wrapping_add(r.edi)),
        None => None,
    }
}

/// Write `src` into the guest's flat space at `flat`, or refuse.
///
/// THE WHOLE RANGE IS CHECKED BEFORE ANY BYTE IS WRITTEN. A partial write is
/// worse than a refusal here: the client would get half a memory-information
/// block, with the remaining fields holding whatever was there, and no
/// indication which half is real.
///
/// Returns 0 on success, -1 if no arena is bound, -2 if the range leaves it.
fn arena_write(flat: u32, src: &[u32]) -> i32 {
    let a = unsafe { ARENA };
    if a.is_null() {
        return -1;
    }
    let len = (src.len() * 4) as u64;
    let end = flat as u64 + len;
    let size = unsafe { (*a).size } as u64;
    if len == 0 || end > size {
        unsafe { (*a).oob_wr += 1 };
        return -2;
    }
    let base = unsafe { (*a).base };
    if base.is_null() {
        return -1;
    }
    // Byte-at-a-time little-endian stores: the destination is guest memory at
    // an arbitrary flat address and carries no alignment guarantee.
    let mut off = flat as usize;
    for w in src {
        let v = *w;
        unsafe {
            *base.add(off) = (v & 0xFF) as u8;
            *base.add(off + 1) = ((v >> 8) & 0xFF) as u8;
            *base.add(off + 2) = ((v >> 16) & 0xFF) as u8;
            *base.add(off + 3) = ((v >> 24) & 0xFF) as u8;
        }
        off += 4;
    }
    0
}

/// (#211) Byte-granular guest access for 000B/000C, which move an 8-byte
/// descriptor rather than a whole number of 32-bit words. Same chokepoint and
/// same counters as arena_write(): one arena, one bounds check.
///
/// Returns 0 on success, -1 with no arena bound, -2 if the range leaves it.
fn arena_write_bytes(flat: u32, src: &[u8]) -> i32 {
    let a = unsafe { ARENA };
    if a.is_null() {
        return -1;
    }
    let end = flat as u64 + src.len() as u64;
    if src.is_empty() || end > unsafe { (*a).size } as u64 {
        unsafe { (*a).oob_wr += 1 };
        return -2;
    }
    let base = unsafe { (*a).base };
    if base.is_null() {
        return -1;
    }
    let mut i = 0usize;
    while i < src.len() {
        unsafe { *base.add(flat as usize + i) = src[i] };
        i += 1;
    }
    0
}

fn arena_read_bytes(flat: u32, dst: &mut [u8]) -> i32 {
    let a = unsafe { ARENA };
    if a.is_null() {
        return -1;
    }
    let end = flat as u64 + dst.len() as u64;
    if dst.is_empty() || end > unsafe { (*a).size } as u64 {
        unsafe { (*a).oob_rd += 1 };
        return -2;
    }
    let base = unsafe { (*a).base };
    if base.is_null() {
        return -1;
    }
    let mut i = 0usize;
    while i < dst.len() {
        dst[i] = unsafe { *base.add(flat as usize + i) };
        i += 1;
    }
    0
}

/// 0500h: get free memory information.
///
/// Refuses rather than guesses. See the 0500h section in this file's header for
/// the field table and for why each UNKNOWN is genuinely unknown.
fn meminfo_0500(r: &mut DpmiRegs) {
    // Resolve the destination FIRST. A bad ES must fail before the model is
    // consulted, so a client with a broken pointer gets the pointer error and
    // not a memory error.
    let flat = match resolve_es_edi(r) {
        Some(f) => f,
        None => {
            fail(r, E_INVALID_SEL);
            return;
        }
    };

    if unsafe { ARENA }.is_null() {
        unsafe {
            if WARNED_NO_ARENA == 0 && QUIET == 0 {
                WARNED_NO_ARENA = 1;
                kprintf(
                    b"[dpmi] 0500: no guest arena bound; REFUSING (CF=1) rather than reporting a heap size into nowhere\n\0"
                        .as_ptr(),
                );
            }
        }
        fail(r, E_UNSUPPORTED);
        return;
    }

    let f = match unsafe { MEMINFO_FN } {
        Some(f) => f,
        None => {
            unsafe {
                if WARNED_NO_MEMINFO == 0 && QUIET == 0 {
                    WARNED_NO_MEMINFO = 1;
                    kprintf(
                        b"[dpmi] 0500: no memory model bound; REFUSING (CF=1). Doom sizes its whole zone heap from this call, so a guess here is worse than a failure\n\0"
                            .as_ptr(),
                    );
                }
            }
            fail(r, E_UNSUPPORTED);
            return;
        }
    };

    let mut largest_bytes: u32 = 0;
    let mut total_pages: u32 = 0;
    let mut free_pages: u32 = 0;
    let rc = f(
        unsafe { MEMINFO_USER },
        &mut largest_bytes as *mut u32,
        &mut total_pages as *mut u32,
        &mut free_pages as *mut u32,
    );
    if rc != 0 {
        fail(r, E_UNSUPPORTED);
        return;
    }

    // The layout lives HERE so a guest owner cannot get the order wrong.
    let block: [u32; MEMINFO_DWORDS] = [
        largest_bytes, // 00h largest available free block, BYTES
        UNKNOWN,       // 04h maximum unlocked page allocation
        UNKNOWN,       // 08h maximum locked page allocation
        total_pages,   // 0Ch linear address space size, PAGES
        UNKNOWN,       // 10h total number of unlocked pages
        free_pages,    // 14h total number of free pages
        UNKNOWN,       // 18h total number of physical pages
        free_pages,    // 1Ch free linear address space, PAGES
        0,             // 20h size of paging file/partition: there is none
        UNKNOWN,       // 24h reserved
        UNKNOWN,       // 28h reserved
        UNKNOWN,       // 2Ch reserved
    ];

    match arena_write(flat, &block) {
        0 => succeed(r),
        -2 => fail(r, E_INVALID_LINEAR),
        _ => fail(r, E_UNSUPPORTED),
    }
}

fn lock_region(base: u32, size: u32) {
    unsafe {
        LOCK_CALLS += 1;
        if size == 0 {
            // Degenerate but harmless. Counted rather than refused: a client
            // that locks an empty range has a bug, and killing it over that
            // bug helps nobody.
            LOCK_ZERO_SIZE += 1;
            return;
        }
        let mut i = 0;
        while i < NLOCK {
            if LOCKS[i].used != 0 && LOCKS[i].base == base && LOCKS[i].size == size {
                LOCKS[i].nest += 1;
                return;
            }
            i += 1;
        }
        let mut i = 0;
        while i < NLOCK {
            if LOCKS[i].used == 0 {
                LOCKS[i] = LockRgn {
                    used: 1,
                    base,
                    size,
                    nest: 1,
                };
                return;
            }
            i += 1;
        }
        // Table full. STILL a success to the client (see "LOCKING" above);
        // record it once so a target that locks more than 64 distinct regions
        // is visible rather than mysterious.
        if LOCK_TABLE_FULL == 0 {
            kprintf(
                b"[dpmi] 0600: lock table full (%u regions); further locks succeed unrecorded\n\0"
                    .as_ptr(),
                NLOCK as u32,
            );
        }
        LOCK_TABLE_FULL += 1;
    }
}

fn unlock_region(base: u32, size: u32) {
    unsafe {
        UNLOCK_CALLS += 1;
        if size == 0 {
            LOCK_ZERO_SIZE += 1;
            return;
        }
        let mut i = 0;
        while i < NLOCK {
            if LOCKS[i].used != 0 && LOCKS[i].base == base && LOCKS[i].size == size {
                if LOCKS[i].nest > 1 {
                    LOCKS[i].nest -= 1;
                } else {
                    LOCKS[i] = LOCK_INIT;
                }
                return;
            }
            i += 1;
        }
        UNLOCK_UNMATCHED += 1;
    }
}

// ===========================================================================
// C-facing surface
// ===========================================================================

/// Drop every descriptor, lock and census entry, and unbind the callbacks.
/// Called at guest teardown (and at both ends of the self-test) so no state
/// crosses from one guest to the next.
///
/// NOTE: this does NOT free DOS memory blocks. The DOS memory model outlives
/// this host and is torn down by whoever owns it; calling free here would be a
/// second owner of the same megabyte, which is the fault dos/int21svc.h exists
/// to prevent.
///
/// # Safety
/// Touches only this module's tables.
#[no_mangle]
pub unsafe extern "C" fn dpmi_host_reset_rs() {
    unsafe {
        let mut i = 0;
        while i < NDESC {
            DESCS[i] = DESC_INIT;
            i += 1;
        }
        let mut i = 0;
        while i < NLOCK {
            LOCKS[i] = LOCK_INIT;
            i += 1;
        }
        let mut i = 0;
        while i < 256 {
            // (rakbd2) NOT one constant for all 256: rmvec_default() is the one
            // place that decides, so a reset cannot re-introduce the "every
            // user vector is taken" answer the static initialiser no longer
            // gives.
            RMVEC[i] = rmvec_default(i);
            RMVEC_GUEST[i] = 0;   // (#dpmi301) provenance resets with the table
            i += 1;
        }
        SEEN_N = 0;
        SEEN_OVERFLOW = 0;
        N_CALLS = 0;
        N_MISS = 0;
        N_FAIL = 0;
        LOCK_CALLS = 0;
        UNLOCK_CALLS = 0;
        LOCK_TABLE_FULL = 0;
        UNLOCK_UNMATCHED = 0;
        LOCK_ZERO_SIZE = 0;
        EXT_FN = None;
        EXT_USER = core::ptr::null_mut();
        DOSMEM_ALLOC = None;
        DOSMEM_FREE = None;
        DOSMEM_USER = core::ptr::null_mut();
        ARENA = core::ptr::null_mut();
        // (#211) The vector tables belong to the guest that installed them.
        // A second launch inheriting the first guest's handlers is the same
        // class of bug the descriptor reset above exists to prevent.
        let mut v = 0;
        while v < 256 {
            PMVEC[v] = FARPTR_NULL;
            PMVEC_LOGGED[v] = false;   // (rakbd) a second launch must log again
            v += 1;
        }
        let mut e = 0;
        while e < 32 {
            EXCVEC[e] = FARPTR_NULL;
            e += 1;
        }
        NPX_EMU_FLAGS = 0;
        NPX_EMU_CALLS = 0;
        MEMINFO_FN = None;
        MEMINFO_USER = core::ptr::null_mut();
        WARNED_NO_ARENA = 0;
        WARNED_NO_MEMINFO = 0;
    }
}

/// Bind the DOS memory allocator 0100/0101 wrap. Pass NULLs to unbind.
///
/// # Safety
/// `user` is opaque and is handed back to the callbacks unchanged.
#[no_mangle]
pub unsafe extern "C" fn dpmi_bind_dosmem_rs(
    alloc: Option<DosAllocFn>,
    free: Option<DosFreeFn>,
    user: *mut u8,
) {
    unsafe {
        DOSMEM_ALLOC = alloc;
        DOSMEM_FREE = free;
        DOSMEM_USER = user;
    }
}

/// Bind the extension handler (0300/0301/0302 and anything else its owner
/// claims). Pass None to unbind.
///
/// # Safety
/// `user` is opaque and is handed back to the callback unchanged.
#[no_mangle]
pub unsafe extern "C" fn dpmi_set_ext_rs(ext: Option<ExtFn>, user: *mut u8) {
    unsafe {
        EXT_FN = ext;
        EXT_USER = user;
    }
}

/// Bind the guest's flat memory window, i.e. where this host may write a block
/// the client asked for, and the bounds check that decides whether it may.
/// Pass NULL to unbind. This is the SAME dpmi_arena_t 0300h's marshaller uses;
/// binding a second, different arena to the two halves of one guest would give
/// the guest two address spaces.
///
/// # Safety
/// `arena` must outlive the guest and point at a valid `dpmi_arena_t`.
#[no_mangle]
pub unsafe extern "C" fn dpmi_bind_arena_rs(arena: *mut DpmiArena) {
    unsafe { ARENA = arena };
}

/// Bind the free-memory model behind 0500h. Pass None to unbind, in which case
/// 0500h REFUSES: see this file's 0500h section for why a refusal is the safe
/// answer here and a guess is not.
///
/// # Safety
/// `user` is opaque and is handed back to the callback unchanged.
#[no_mangle]
pub unsafe extern "C" fn dpmi_bind_meminfo_rs(f: Option<MemInfoFn>, user: *mut u8) {
    unsafe {
        MEMINFO_FN = f;
        MEMINFO_USER = user;
    }
}

/// Install a descriptor AT A SPECIFIC SELECTOR VALUE.
///
/// WHY A HOST NEEDS THIS AT ALL, given 0000h exists. 0000h returns whatever
/// index is free, which is right for a client asking for "a descriptor". It is
/// useless for the other direction: a guest whose startup names a selector as
/// a CONSTANT. The Watcom 32-bit runtime's extender ladder ends in an arm that
/// assumes a fixed layout and never asks for it - selector 0x17 the flat
/// alias, 0x24 the PSP, 0x2C the environment - so a host that cannot say
/// "this exact selector, this base" cannot satisfy it. Measured on The Dig
/// (DIG.EXE): it reads its command tail as `mov cl,[es:edi-1]` with ES = 0x24
/// and EDI = 0x81, which is PSP:0x80, the tail length byte.
///
/// It is deliberately NOT a second way to make a descriptor. The fields it
/// writes are the same fields 0009h (access rights) and 0008h (limit) write,
/// under the same two rules those two enforce: a SYSTEM descriptor is refused,
/// and a limit above 1 MiB carries page granularity in the stored extended
/// byte. What it adds is the ability to choose the index.
///
/// Returns 0 on success. Refuses, and changes nothing, if:
///   -1  `sel` is not an LDT-form selector (TI clear = a GDT selector, which is
///       not ours to own), or its index is out of range, or index 0 (never
///       handed out, so that 0x0007 stays a selector a client may test).
///   -2  that descriptor is already live. A loader that seeds two things onto
///       one selector has a bug, and overwriting would hide it.
///   -3  the access-rights byte describes a SYSTEM descriptor (S clear).
///
/// # Safety
/// Touches this module's descriptor table. One host, one guest.
#[no_mangle]
pub unsafe extern "C" fn dpmi_seed_desc_rs(
    sel: u16,
    base: u32,
    byte_limit: u32,
    ar: u8,
    ext: u8,
) -> i32 {
    if (sel & 4) == 0 {
        return -1;
    }
    let idx = (sel >> 3) as usize;
    if idx < FIRST_DESC || idx >= NDESC {
        return -1;
    }
    if unsafe { DESCS[idx].kind } != KIND_FREE {
        return -2;
    }
    if (ar & 0x10) == 0 {
        return -3;
    }
    let mut e = ext & 0xF0;
    if byte_limit > 0x000F_FFFF {
        e |= 0x80;
    } else {
        e &= !0x80;
    }
    unsafe {
        DESCS[idx] = Desc {
            kind: KIND_LDT,
            ar,
            ext: e,
            base,
            limit: byte_limit,
            dos_seg: 0,
            dos_paras: 0,
        };
    }
    0
}

/// Resolve a client selector to its base, byte limit and access-rights byte.
///
/// THIS IS WHAT MAKES THE DESCRIPTOR TABLE LOAD-BEARING rather than decorative:
/// the 32-bit core calls it to turn a guest's selector:offset into a linear
/// address. Returns 0 on success, -1 if the selector is not one of ours (a
/// GDT/flat selector, or a freed one). Out pointers may be NULL.
///
/// # Safety
/// Out pointers, where non-NULL, must be writable.
#[no_mangle]
pub unsafe extern "C" fn dpmi_sel_lookup_rs(
    sel: u16,
    out_base: *mut u32,
    out_limit: *mut u32,
    out_ar: *mut u8,
) -> i32 {
    match idx_of_live(sel) {
        Some(i) => unsafe {
            if !out_base.is_null() {
                *out_base = DESCS[i].base;
            }
            if !out_limit.is_null() {
                *out_limit = DESCS[i].limit;
            }
            if !out_ar.is_null() {
                *out_ar = DESCS[i].ar;
            }
            0
        },
        None => -1,
    }
}

/// Dump the per-AX census plus the lock counters. THE MEASUREMENT
/// (DPMI_BRIDGE_DESIGN 2.7): the first run of a real DOS/4GW binary tells us
/// what it actually calls, instead of us guessing from a static scan that
/// cannot see an AX loaded from a register (blame.md, 2026-08-07).
///
/// # Safety
/// Reads this module's tables and calls kprintf.
#[no_mangle]
pub unsafe extern "C" fn dpmi_report_rs() {
    unsafe {
        kprintf(
            b"[dpmi] census: %u INT31 calls, %u distinct AX, %u MISS, %u failed\n\0".as_ptr(),
            N_CALLS,
            SEEN_N as u32,
            N_MISS,
            N_FAIL,
        );
        let mut i = 0;
        while i < SEEN_N {
            let tag: &[u8] = match SEEN_TAB[i].cls {
                CLS_SERVICED => b"serviced\0",
                CLS_EXT => b"extension\0",
                CLS_REFUSED => b"REFUSED-BY-DESIGN\0",
                _ => b"MISS\0",
            };
            kprintf(
                b"[dpmi]   AX=%04x %-18s x%u\n\0".as_ptr(),
                SEEN_TAB[i].ax as u32,
                tag.as_ptr(),
                SEEN_TAB[i].calls,
            );
            i += 1;
        }
        if SEEN_OVERFLOW != 0 {
            kprintf(
                b"[dpmi]   (census table full; %u calls with unrecorded AX)\n\0".as_ptr(),
                SEEN_OVERFLOW,
            );
        }
        kprintf(
            b"[dpmi] locks: %u lock, %u unlock, %u unmatched-unlock, %u zero-size, %u table-full\n\0"
                .as_ptr(),
            LOCK_CALLS,
            UNLOCK_CALLS,
            UNLOCK_UNMATCHED,
            LOCK_ZERO_SIZE,
            LOCK_TABLE_FULL,
        );
    }
}

// ===========================================================================
// THE BOOT SELF-TEST
// ===========================================================================
//
// The 32-bit execution path does not exist yet, so no real DOS/4GW guest can
// reach this dispatcher. "It compiles" and "nm shows the symbol" prove nothing
// in this tree (validate_user_ptr, sse_save, graphfs's 72 declarations with 0
// callers). So the dispatcher is driven HERE, at boot, with synthesised
// register files, and the results are asserted and printed to serial.
//
// Every case is a real INT 31h frame through the real dpmi_int31_rs(). There is
// no test-only entry point and no mock dispatcher: the thing that runs is the
// thing a guest will run.

static mut ST_CHECKS: u32 = 0;
static mut ST_FAILS: u32 = 0;
static mut ST_FIRST_FAIL: u32 = 0;
static mut ST_CASE: u32 = 0;

fn st_check(cond: bool, what: *const u8) {
    unsafe {
        ST_CHECKS += 1;
        if !cond {
            ST_FAILS += 1;
            if ST_FIRST_FAIL == 0 {
                ST_FIRST_FAIL = ST_CASE;
            }
            kprintf(b"[dpmi-st] FAIL case %u: %s\n\0".as_ptr(), ST_CASE, what);
        }
    }
}

fn blank() -> DpmiRegs {
    DpmiRegs {
        eax: 0,
        ebx: 0,
        ecx: 0,
        edx: 0,
        esi: 0,
        edi: 0,
        ebp: 0,
        esp: 0,
        // CF pre-set on every synthesised frame, so "CF is clear" is a result
        // this code produced and never the initial value it was handed.
        eflags: CF,
        eip: 0,
        cs: 0,
        ds: 0,
        es: 0,
        fs: 0,
        gs: 0,
        ss: 0,
    }
}

fn call31(r: &mut DpmiRegs) {
    unsafe { dpmi_int31_rs(r as *mut DpmiRegs) };
}

#[inline]
fn cf(r: &DpmiRegs) -> bool {
    (r.eflags & CF) != 0
}

// --- the fake DOS memory allocator the self-test binds ---------------------
// It is a bump allocator over a pretend 640 KiB, NOT a second real allocator:
// its whole job is to prove that 0100 calls its binding, builds the right
// descriptor from the answer, and gives the memory back on 0101.
static mut FAKE_NEXT: u16 = 0x1000; // paragraph 0x1000 = 64 KiB
static mut FAKE_FREED: u16 = 0;
static mut FAKE_FREES: u32 = 0;
static mut FAKE_REFUSE: u8 = 0;

extern "C" fn fake_alloc(_u: *mut u8, paras: u16, out_seg: *mut u16, out_largest: *mut u16) -> i32 {
    unsafe {
        if FAKE_REFUSE != 0 || (FAKE_NEXT as u32) + (paras as u32) > 0xA000 {
            if !out_largest.is_null() {
                *out_largest = 0xA000u16.wrapping_sub(FAKE_NEXT);
            }
            return -1;
        }
        if !out_seg.is_null() {
            *out_seg = FAKE_NEXT;
        }
        FAKE_NEXT += paras;
        0
    }
}

extern "C" fn fake_free(_u: *mut u8, seg: u16) -> i32 {
    unsafe {
        FAKE_FREED = seg;
        FAKE_FREES += 1;
        0
    }
}

// --- the fake guest arena and memory model for 0500h -----------------------
//
// THE THREE FIXTURE VALUES ARE DELIBERATELY DISTINCT, and case group J asserts
// that they are before it asserts where they landed. That is what makes the
// per-offset assertions ORDER-SENSITIVE: if the block were emitted with the
// fields permuted, a test using the same number twice would still pass. A
// memory-information block of plausible-looking numbers in the wrong order is
// exactly the bug a weak test waves through, and the consequence is a game
// sizing its heap from the wrong field.
const ST_LARGEST_BYTES: u32 = 0x00A5_0000; // 10.3 MB, distinctive
const ST_TOTAL_PAGES: u32 = 0x0000_4000; // 16384 pages = 64 MB
const ST_FREE_PAGES: u32 = 0x0000_2A00; // 10752 pages
const ST_FILL: u8 = 0x5A; // sentinel: any byte still 0x5A was NOT written

static mut ST_ARENA_BUF: [u8; 4096] = [0; 4096];
static mut ST_ARENA: DpmiArena = DpmiArena {
    base: core::ptr::null_mut(),
    size: 0,
    oob_rd: 0,
    oob_wr: 0,
};
static mut ST_MEMINFO_CALLS: u32 = 0;
static mut ST_MEMINFO_REFUSE: u8 = 0;

extern "C" fn fake_meminfo(
    _u: *mut u8,
    largest: *mut u32,
    total: *mut u32,
    free: *mut u32,
) -> i32 {
    unsafe {
        ST_MEMINFO_CALLS += 1;
        if ST_MEMINFO_REFUSE != 0 {
            return -1;
        }
        if !largest.is_null() {
            *largest = ST_LARGEST_BYTES;
        }
        if !total.is_null() {
            *total = ST_TOTAL_PAGES;
        }
        if !free.is_null() {
            *free = ST_FREE_PAGES;
        }
        0
    }
}

/// Refill the fixture arena with the sentinel, so "was this byte written?" has
/// an answer rather than an assumption.
fn st_arena_reset() {
    unsafe {
        let mut i = 0;
        while i < 4096 {
            ST_ARENA_BUF[i] = ST_FILL;
            i += 1;
        }
        ST_ARENA.base = core::ptr::addr_of_mut!(ST_ARENA_BUF) as *mut u8;
        ST_ARENA.size = 4096;
        ST_ARENA.oob_rd = 0;
        ST_ARENA.oob_wr = 0;
    }
}

fn st_dw(off: usize) -> u32 {
    unsafe {
        (ST_ARENA_BUF[off] as u32)
            | ((ST_ARENA_BUF[off + 1] as u32) << 8)
            | ((ST_ARENA_BUF[off + 2] as u32) << 16)
            | ((ST_ARENA_BUF[off + 3] as u32) << 24)
    }
}

/// True if every byte in [off, off+len) is still the sentinel, i.e. untouched.
fn st_untouched(off: usize, len: usize) -> bool {
    unsafe {
        let mut i = 0;
        while i < len {
            if ST_ARENA_BUF[off + i] != ST_FILL {
                return false;
            }
            i += 1;
        }
        true
    }
}

// --- the fake 0300 owner the self-test binds -------------------------------
// Proves the seam the 0300 agent attaches through actually dispatches, and that
// declining falls through to MISS.
static mut EXT_HITS: u32 = 0;

extern "C" fn fake_ext(_u: *mut u8, r: *mut DpmiRegs, ax: u16) -> i32 {
    unsafe {
        if ax != 0x0300 {
            return 0; // decline: must fall through to MISS
        }
        EXT_HITS += 1;
        (*r).eflags &= !CF;
        set_ax(&mut *r, 0xBEEF);
        1
    }
}

/// Drive the dispatcher with synthesised register files and assert the results.
/// Returns 0 if every check passed, otherwise the number of the first failing
/// case. `out_checks` receives the number of assertions that actually RAN, so
/// "passed" is distinguishable from "never ran".
///
/// # Safety
/// Resets the host state at both ends; must not be called while a guest is
/// live. It is called once from main.c at boot, before any guest can exist.
#[no_mangle]
pub unsafe extern "C" fn dpmi_selftest_rs(out_checks: *mut u32) -> i32 {
    unsafe {
        dpmi_host_reset_rs();
        QUIET = 1;
        ST_CHECKS = 0;
        ST_FAILS = 0;
        ST_FIRST_FAIL = 0;
        FAKE_NEXT = 0x1000;
        FAKE_FREED = 0;
        FAKE_FREES = 0;
        FAKE_REFUSE = 0;
        EXT_HITS = 0;
    }

    // ---- A: version ------------------------------------------------------
    unsafe { ST_CASE = 1 };
    {
        let mut r = blank();
        r.eax = 0x0400;
        call31(&mut r);
        st_check(!cf(&r), b"0400 set CF\0".as_ptr());
        st_check(lo16(r.eax) == 0x005A, b"0400 AX != 0.90\0".as_ptr());
        st_check((lo16(r.ebx) & 1) == 1, b"0400 BX bit0 (32-bit host)\0".as_ptr());
        st_check((lo16(r.ebx) & 4) == 0, b"0400 BX bit2 claims VM support\0".as_ptr());
        st_check(lo16(r.ecx) == 4, b"0400 CL processor type\0".as_ptr());
        st_check(lo16(r.edx) == 0x0870, b"0400 DX PIC bases\0".as_ptr());
        unsafe {
            kprintf(
                b"[dpmi-st] A 0400 version: AX=%04x BX=%04x CX=%04x DX=%04x CF=%d\n\0".as_ptr(),
                lo16(r.eax) as u32,
                lo16(r.ebx) as u32,
                lo16(r.ecx) as u32,
                lo16(r.edx) as u32,
                cf(&r) as u32,
            );
        }
    }

    // ---- A2: real-mode interrupt vectors (0200h / 0201h) -----------------
    unsafe { ST_CASE = 1 };
    {
        // A vector nobody has set reads back as the IRET stub, not as zero. A
        // client that saves a vector it never installed and restores it later
        // would otherwise install a null far pointer on its way out.
        let mut r = blank();
        r.eax = 0x0200;
        r.ebx = 0x1C;
        call31(&mut r);
        st_check(!cf(&r), b"0200 set CF\0".as_ptr());
        st_check(lo16(r.ecx) == 0xF000, b"0200 unset vector CX != F000\0".as_ptr());
        st_check(lo16(r.edx) == 0xFF53, b"0200 unset vector DX != FF53\0".as_ptr());

        // (rakbd2) ...AND THE USER INTERRUPT VECTORS READ BACK AS FREE.
        // 60h-66h must answer 0000:0000, because that is what a real DOS
        // machine answers and it is what a guest scanning for a vector of its
        // own is looking for. Asserted for the WHOLE range and for 5Fh/67h on
        // either side of it, so a fencepost cannot pass this test.
        let mut v = 0x5Fu32;
        while v <= 0x67 {
            let mut r = blank();
            r.eax = 0x0200;
            r.ebx = v;
            call31(&mut r);
            st_check(!cf(&r), b"0200 user-vector scan set CF\0".as_ptr());
            let free = (0x60..=0x66).contains(&v);
            let packed = ((lo16(r.ecx) as u32) << 16) | (lo16(r.edx) as u32);
            if free {
                st_check(packed == 0, b"0200 user vector 60h-66h is not free\0".as_ptr());
            } else {
                st_check(packed == RMVEC_INIT, b"0200 vector outside 60h-66h lost its stub\0".as_ptr());
            }
            v += 1;
        }

        let mut r = blank();
        r.eax = 0x0201;
        r.ebx = 0x1C;
        r.ecx = 0x1234;
        r.edx = 0x5678;
        call31(&mut r);
        st_check(!cf(&r), b"0201 set CF\0".as_ptr());

        let mut r = blank();
        r.eax = 0x0200;
        r.ebx = 0x1C;
        call31(&mut r);
        st_check(lo16(r.ecx) == 0x1234, b"0200 did not read back 0201's CX\0".as_ptr());
        st_check(lo16(r.edx) == 0x5678, b"0200 did not read back 0201's DX\0".as_ptr());

        // A DIFFERENT vector must be untouched: one table entry, not one
        // variable. This is the check that catches an ignored BL.
        let mut r = blank();
        r.eax = 0x0200;
        r.ebx = 0x08;
        call31(&mut r);
        st_check(lo16(r.ecx) == 0xF000, b"0201 wrote the wrong vector\0".as_ptr());
    }

    // ---- B: selector increment + allocation ------------------------------
    unsafe { ST_CASE = 2 };
    let sel_a;
    {
        let mut r = blank();
        r.eax = 0x0003;
        call31(&mut r);
        st_check(!cf(&r), b"0003 set CF\0".as_ptr());
        st_check(lo16(r.eax) == 8, b"0003 increment != 8\0".as_ptr());
        let inc = lo16(r.eax);

        let mut r = blank();
        r.eax = 0x0000;
        r.ecx = 0;
        call31(&mut r);
        st_check(cf(&r), b"0000 CX=0 succeeded\0".as_ptr());
        st_check(lo16(r.eax) == E_INVALID_VALUE, b"0000 CX=0 wrong error\0".as_ptr());

        let mut r = blank();
        r.eax = 0x0000;
        r.ecx = 3;
        call31(&mut r);
        st_check(!cf(&r), b"0000 CX=3 set CF\0".as_ptr());
        sel_a = lo16(r.eax);
        st_check(sel_a != 0, b"0000 returned null selector\0".as_ptr());
        st_check((sel_a & 4) != 0, b"0000 selector TI bit clear (not LDT)\0".as_ptr());
        st_check((sel_a & 3) == 3, b"0000 selector RPL != 3\0".as_ptr());

        // The block must be CONTIGUOUS at the increment 0003 advertised: a
        // client walks it with exactly that stride.
        let mut base: u32 = 0;
        st_check(
            unsafe { dpmi_sel_lookup_rs(sel_a + inc, &mut base, core::ptr::null_mut(), core::ptr::null_mut()) } == 0,
            b"0000 second selector of the run is not live\0".as_ptr(),
        );
        st_check(
            unsafe { dpmi_sel_lookup_rs(sel_a + 2 * inc, &mut base, core::ptr::null_mut(), core::ptr::null_mut()) } == 0,
            b"0000 third selector of the run is not live\0".as_ptr(),
        );
        st_check(
            unsafe { dpmi_sel_lookup_rs(sel_a + 3 * inc, &mut base, core::ptr::null_mut(), core::ptr::null_mut()) } != 0,
            b"0000 CX=3 allocated a fourth descriptor\0".as_ptr(),
        );
        unsafe {
            kprintf(
                b"[dpmi-st] B 0000 CX=3 -> sel %04x,%04x,%04x (increment %u)\n\0".as_ptr(),
                sel_a as u32,
                (sel_a + inc) as u32,
                (sel_a + 2 * inc) as u32,
                inc as u32,
            );
        }
    }

    // ---- C: base / limit / rights, i.e. Descent's real VGA sequence -------
    unsafe { ST_CASE = 3 };
    {
        // 0007: base = 0x000A0000
        let mut r = blank();
        r.eax = 0x0007;
        r.ebx = sel_a as u32;
        r.ecx = 0x000A;
        r.edx = 0x0000;
        call31(&mut r);
        st_check(!cf(&r), b"0007 set CF\0".as_ptr());

        // 0008: limit = 0xFFFF
        let mut r = blank();
        r.eax = 0x0008;
        r.ebx = sel_a as u32;
        r.ecx = 0x0000;
        r.edx = 0xFFFF;
        call31(&mut r);
        st_check(!cf(&r), b"0008 limit 0xFFFF set CF\0".as_ptr());

        let mut base: u32 = 0;
        let mut limit: u32 = 0;
        let mut ar: u8 = 0;
        let rc = unsafe { dpmi_sel_lookup_rs(sel_a, &mut base, &mut limit, &mut ar) };
        st_check(rc == 0, b"lookup of a live selector failed\0".as_ptr());
        st_check(base == 0x000A_0000, b"0007 base not stored\0".as_ptr());
        st_check(limit == 0xFFFF, b"0008 limit not stored\0".as_ptr());

        // 0006 must agree with the lookup, in CX:DX.
        let mut r = blank();
        r.eax = 0x0006;
        r.ebx = sel_a as u32;
        call31(&mut r);
        st_check(!cf(&r), b"0006 set CF\0".as_ptr());
        st_check(lo16(r.ecx) == 0x000A, b"0006 CX (base high)\0".as_ptr());
        st_check(lo16(r.edx) == 0x0000, b"0006 DX (base low)\0".as_ptr());

        // 0008 above 1 MiB without the low 12 bits set is the one validation a
        // real 386 would also enforce.
        let mut r = blank();
        r.eax = 0x0008;
        r.ebx = sel_a as u32;
        r.ecx = 0x0010;
        r.edx = 0x0000;
        call31(&mut r);
        st_check(cf(&r), b"0008 non-page-granular >1MB limit accepted\0".as_ptr());
        st_check(lo16(r.eax) == E_INVALID_VALUE, b"0008 wrong error\0".as_ptr());

        let mut r = blank();
        r.eax = 0x0008;
        r.ebx = sel_a as u32;
        r.ecx = 0x001F;
        r.edx = 0xFFFF;
        call31(&mut r);
        st_check(!cf(&r), b"0008 page-granular 2MB limit refused\0".as_ptr());

        // Put it back where case D expects it.
        let mut r = blank();
        r.eax = 0x0008;
        r.ebx = sel_a as u32;
        r.edx = 0xFFFF;
        call31(&mut r);
        st_check(!cf(&r), b"0008 restore 0xFFFF\0".as_ptr());

        // 0009: a data descriptor is accepted, a system descriptor is not.
        let mut r = blank();
        r.eax = 0x0009;
        r.ebx = sel_a as u32;
        r.ecx = 0x40F3;
        call31(&mut r);
        st_check(!cf(&r), b"0009 CL=F3 refused\0".as_ptr());
        let mut ar2: u8 = 0;
        unsafe {
            dpmi_sel_lookup_rs(sel_a, core::ptr::null_mut(), core::ptr::null_mut(), &mut ar2);
        }
        st_check(ar2 == 0xF3, b"0009 access byte not stored\0".as_ptr());

        let mut r = blank();
        r.eax = 0x0009;
        r.ebx = sel_a as u32;
        r.ecx = 0x0089; // S=0: a system descriptor
        call31(&mut r);
        st_check(cf(&r), b"0009 accepted a system descriptor\0".as_ptr());
        st_check(lo16(r.eax) == E_INVALID_VALUE, b"0009 wrong error\0".as_ptr());

        unsafe {
            kprintf(
                b"[dpmi-st] C sel %04x -> base %08x limit %08x ar %02x (0006 said %04x:%04x)\n\0"
                    .as_ptr(),
                sel_a as u32,
                base,
                limit,
                ar as u32,
                0x000Au32,
                0x0000u32,
            );
        }
    }

    // ---- D: alias, free, and the invalid-selector answer ------------------
    unsafe { ST_CASE = 4 };
    {
        let mut r = blank();
        r.eax = 0x000A;
        r.ebx = sel_a as u32;
        call31(&mut r);
        st_check(!cf(&r), b"000A set CF\0".as_ptr());
        let alias = lo16(r.eax);
        st_check(alias != sel_a, b"000A returned the same selector\0".as_ptr());
        let mut ab: u32 = 0;
        let mut al: u32 = 0;
        st_check(
            unsafe { dpmi_sel_lookup_rs(alias, &mut ab, &mut al, core::ptr::null_mut()) } == 0,
            b"000A alias not live\0".as_ptr(),
        );
        st_check(ab == 0x000A_0000, b"000A alias base differs\0".as_ptr());
        st_check(al == 0xFFFF, b"000A alias limit differs\0".as_ptr());

        // Free the alias, then prove the selector is genuinely dead.
        let mut r = blank();
        r.eax = 0x0001;
        r.ebx = alias as u32;
        call31(&mut r);
        st_check(!cf(&r), b"0001 set CF\0".as_ptr());
        st_check(
            unsafe { dpmi_sel_lookup_rs(alias, core::ptr::null_mut(), core::ptr::null_mut(), core::ptr::null_mut()) } != 0,
            b"0001 left the selector live\0".as_ptr(),
        );

        let mut r = blank();
        r.eax = 0x0007;
        r.ebx = alias as u32;
        call31(&mut r);
        st_check(cf(&r), b"0007 on a freed selector succeeded\0".as_ptr());
        st_check(lo16(r.eax) == E_INVALID_SEL, b"0007 freed: wrong error\0".as_ptr());

        // A GDT selector (TI=0) is one of the extender's own and is not ours.
        let mut r = blank();
        r.eax = 0x0007;
        r.ebx = 0x0010;
        call31(&mut r);
        st_check(cf(&r), b"0007 accepted a GDT selector\0".as_ptr());
        st_check(lo16(r.eax) == E_INVALID_SEL, b"0007 GDT: wrong error\0".as_ptr());

        unsafe {
            kprintf(
                b"[dpmi-st] D alias %04x base %08x limit %08x; freed -> lookup dead, 0007 CF=1 AX=%04x\n\0"
                    .as_ptr(),
                alias as u32,
                ab,
                al,
                E_INVALID_SEL as u32,
            );
        }
    }

    // ---- E: 0002 segment -> selector, and its stability -------------------
    unsafe { ST_CASE = 5 };
    {
        let mut r = blank();
        r.eax = 0x0002;
        r.ebx = 0xB800;
        call31(&mut r);
        st_check(!cf(&r), b"0002 set CF\0".as_ptr());
        let s1 = lo16(r.eax);
        let mut b: u32 = 0;
        let mut l: u32 = 0;
        st_check(
            unsafe { dpmi_sel_lookup_rs(s1, &mut b, &mut l, core::ptr::null_mut()) } == 0,
            b"0002 selector not live\0".as_ptr(),
        );
        st_check(b == 0x000B_8000, b"0002 base != seg<<4\0".as_ptr());
        st_check(l == 0xFFFF, b"0002 limit != 64K\0".as_ptr());

        let mut r = blank();
        r.eax = 0x0002;
        r.ebx = 0xB800;
        call31(&mut r);
        let s2 = lo16(r.eax);
        st_check(s2 == s1, b"0002 handed out a second selector for one segment\0".as_ptr());
        unsafe {
            kprintf(
                b"[dpmi-st] E 0002 seg B800 -> sel %04x base %08x limit %08x (repeat -> %04x)\n\0"
                    .as_ptr(),
                s1 as u32,
                b,
                l,
                s2 as u32,
            );
        }
    }

    // ---- F: DOS memory, which is Doom's real-mode ISR stack ---------------
    unsafe { ST_CASE = 6 };
    {
        // Unbound first: the state the host is in TODAY, with no 32-bit core.
        let mut r = blank();
        r.eax = 0x0100;
        r.ebx = 0x0040;
        call31(&mut r);
        st_check(cf(&r), b"0100 unbound succeeded\0".as_ptr());
        st_check(lo16(r.eax) == E_DOS_NOMEM, b"0100 unbound wrong error\0".as_ptr());
        st_check(lo16(r.ebx) == 0, b"0100 unbound BX != 0\0".as_ptr());

        unsafe {
            dpmi_bind_dosmem_rs(Some(fake_alloc), Some(fake_free), core::ptr::null_mut());
        }

        // Doom: realstackseg = I_AllocLow(1024) >> 4, i.e. 64 paragraphs.
        let mut r = blank();
        r.eax = 0x0100;
        r.ebx = 0x0040;
        call31(&mut r);
        st_check(!cf(&r), b"0100 bound set CF\0".as_ptr());
        let seg = lo16(r.eax);
        let dsel = lo16(r.edx);
        st_check(seg == 0x1000, b"0100 segment not from the binding\0".as_ptr());
        let mut b: u32 = 0;
        let mut l: u32 = 0;
        st_check(
            unsafe { dpmi_sel_lookup_rs(dsel, &mut b, &mut l, core::ptr::null_mut()) } == 0,
            b"0100 selector not live\0".as_ptr(),
        );
        st_check(b == (seg as u32) << 4, b"0100 selector base != seg<<4\0".as_ptr());
        st_check(l == 1023, b"0100 selector limit != size-1\0".as_ptr());

        // 0006 on that selector: DPMI_BRIDGE_DESIGN 3.9's flagged case.
        let mut r6 = blank();
        r6.eax = 0x0006;
        r6.ebx = dsel as u32;
        call31(&mut r6);
        st_check(!cf(&r6), b"0006 on a DOS block selector set CF\0".as_ptr());
        st_check(
            ((lo16(r6.ecx) as u32) << 16 | lo16(r6.edx) as u32) == (seg as u32) << 4,
            b"0006 on a DOS block: wrong base\0".as_ptr(),
        );

        // 0101 must reach the binding, not a private free list.
        let mut r = blank();
        r.eax = 0x0101;
        r.edx = dsel as u32;
        call31(&mut r);
        st_check(!cf(&r), b"0101 set CF\0".as_ptr());
        st_check(unsafe { FAKE_FREES } == 1, b"0101 did not call the binding\0".as_ptr());
        st_check(unsafe { FAKE_FREED } == seg, b"0101 freed the wrong segment\0".as_ptr());
        st_check(
            unsafe { dpmi_sel_lookup_rs(dsel, core::ptr::null_mut(), core::ptr::null_mut(), core::ptr::null_mut()) } != 0,
            b"0101 left the selector live\0".as_ptr(),
        );

        // 0101 on something that is not a DOS block.
        let mut r = blank();
        r.eax = 0x0101;
        r.edx = sel_a as u32;
        call31(&mut r);
        st_check(cf(&r), b"0101 freed a non-DOS-block selector\0".as_ptr());
        st_check(lo16(r.eax) == E_INVALID_SEL, b"0101 wrong error\0".as_ptr());

        // Descent's size probe: BX=0xFFFF is a request for a megabyte, which
        // must fail with the largest available block in BX (BIOS/DPMI.C:107).
        let mut r = blank();
        r.eax = 0x0100;
        r.ebx = 0xFFFF;
        call31(&mut r);
        st_check(cf(&r), b"0100 BX=FFFF succeeded\0".as_ptr());
        st_check(lo16(r.eax) == E_DOS_NOMEM, b"0100 probe wrong error\0".as_ptr());
        st_check(lo16(r.ebx) != 0, b"0100 probe returned no largest-block size\0".as_ptr());
        unsafe {
            kprintf(
                b"[dpmi-st] F 0100 64 paras -> seg %04x sel %04x base %08x limit %u; 0101 freed seg %04x; probe BX=%04x\n\0"
                    .as_ptr(),
                seg as u32,
                dsel as u32,
                b,
                l,
                FAKE_FREED as u32,
                lo16(r.ebx) as u32,
            );
        }
    }

    // ---- G: locking, which Doom does to its entire image at startup -------
    unsafe { ST_CASE = 7 };
    {
        // Doom: _dpmi_lockregion(&__begtext, &___argc - &__begtext).
        let mut r = blank();
        r.eax = 0x0600;
        r.ebx = 0x0001; // BX:CX = 0x00010000
        r.ecx = 0x0000;
        r.esi = 0x0002; // SI:DI = 0x00020000 bytes
        r.edi = 0x0000;
        call31(&mut r);
        st_check(!cf(&r), b"0600 FAILED - this aborts Doom at startup\0".as_ptr());

        // Nesting: the same region twice, then one unlock, must leave it held.
        let mut r2 = blank();
        r2.eax = 0x0600;
        r2.ebx = 0x0001;
        r2.esi = 0x0002;
        call31(&mut r2);
        st_check(!cf(&r2), b"0600 nested lock set CF\0".as_ptr());

        let mut r3 = blank();
        r3.eax = 0x0601;
        r3.ebx = 0x0001;
        r3.esi = 0x0002;
        call31(&mut r3);
        st_check(!cf(&r3), b"0601 set CF\0".as_ptr());

        // An unlock of something never locked must still succeed (see LOCKING).
        let mut r4 = blank();
        r4.eax = 0x0601;
        r4.ebx = 0x0777;
        r4.esi = 0x0001;
        call31(&mut r4);
        st_check(!cf(&r4), b"0601 unmatched set CF\0".as_ptr());

        // Zero size: degenerate, counted, never a failure.
        let mut r5 = blank();
        r5.eax = 0x0600;
        r5.ebx = 0x0001;
        r5.esi = 0;
        r5.edi = 0;
        call31(&mut r5);
        st_check(!cf(&r5), b"0600 zero size set CF\0".as_ptr());

        st_check(unsafe { LOCK_CALLS } == 3, b"0600 call counter\0".as_ptr());
        st_check(unsafe { UNLOCK_CALLS } == 2, b"0601 call counter\0".as_ptr());
        st_check(unsafe { UNLOCK_UNMATCHED } == 1, b"unmatched-unlock counter\0".as_ptr());
        st_check(unsafe { LOCK_ZERO_SIZE } == 1, b"zero-size counter\0".as_ptr());
        unsafe {
            kprintf(
                b"[dpmi-st] G 0600/0601: %u lock, %u unlock, %u unmatched, %u zero-size, all CF=0\n\0"
                    .as_ptr(),
                LOCK_CALLS,
                UNLOCK_CALLS,
                UNLOCK_UNMATCHED,
                LOCK_ZERO_SIZE,
            );
        }
    }

    // ---- H: the MISS discipline and the 0300 extension seam ---------------
    unsafe { ST_CASE = 8 };
    {
        // Unowned 0300 with no hook bound: MISS, with EVERY other register
        // preserved. This is the blame.md 1687h check: the caller's poison must
        // come back untouched EXCEPT for AX, which must be ours.
        let mut r = blank();
        r.eax = 0x0300;
        r.ebx = 0xDEAD_BEEF;
        r.edi = 0x0100_DEAD;
        r.es = 0x0100;
        call31(&mut r);
        st_check(cf(&r), b"0300 unhandled did not set CF\0".as_ptr());
        st_check(lo16(r.eax) == E_UNSUPPORTED, b"0300 unhandled wrong AX\0".as_ptr());
        st_check(r.ebx == 0xDEAD_BEEF, b"MISS clobbered EBX\0".as_ptr());
        st_check(r.edi == 0x0100_DEAD, b"MISS clobbered EDI\0".as_ptr());
        st_check(r.es == 0x0100, b"MISS clobbered ES\0".as_ptr());
        st_check(r.eax >> 16 == 0, b"MISS clobbered the high half of EAX\0".as_ptr());

        // A completely unknown AX must behave identically.
        let mut r = blank();
        r.eax = 0x0E00;
        call31(&mut r);
        st_check(cf(&r), b"0E00 did not set CF\0".as_ptr());
        st_check(lo16(r.eax) == E_UNSUPPORTED, b"0E00 wrong AX\0".as_ptr());

        // 0303 is refused BY DESIGN, which must be indistinguishable to the
        // client from a MISS (same CF, same AX) and distinguishable in the log.
        let mut r = blank();
        r.eax = 0x0303;
        call31(&mut r);
        st_check(cf(&r), b"0303 did not set CF\0".as_ptr());
        st_check(lo16(r.eax) == E_UNSUPPORTED, b"0303 wrong AX\0".as_ptr());

        // Now bind a hook, exactly as the 0300 owner will.
        unsafe { dpmi_set_ext_rs(Some(fake_ext), core::ptr::null_mut()) };
        let mut r = blank();
        r.eax = 0x0300;
        call31(&mut r);
        st_check(!cf(&r), b"bound extension did not clear CF\0".as_ptr());
        st_check(lo16(r.eax) == 0xBEEF, b"bound extension did not run\0".as_ptr());
        st_check(unsafe { EXT_HITS } == 1, b"extension hit counter\0".as_ptr());

        // A hook that DECLINES must fall through to MISS, not be treated as
        // handled. This is the half that would otherwise silently swallow
        // every unimplemented function once a hook exists.
        let mut r = blank();
        r.eax = 0x0301;
        call31(&mut r);
        st_check(cf(&r), b"declined extension did not fall through to MISS\0".as_ptr());
        st_check(lo16(r.eax) == E_UNSUPPORTED, b"declined extension wrong AX\0".as_ptr());

        unsafe { dpmi_set_ext_rs(None, core::ptr::null_mut()) };
        let mut r = blank();
        r.eax = 0x0300;
        call31(&mut r);
        st_check(cf(&r), b"unbinding the extension did not restore MISS\0".as_ptr());
        unsafe {
            kprintf(
                b"[dpmi-st] H MISS: 0300/0E00/0303 -> CF=1 AX=%04x, EBX/EDI/ES preserved; ext hook %u hit, decline -> MISS\n\0"
                    .as_ptr(),
                E_UNSUPPORTED as u32,
                EXT_HITS,
            );
        }
    }

    // ---- I: descriptor exhaustion is an error, not a crash ----------------
    unsafe { ST_CASE = 9 };
    {
        let mut r = blank();
        r.eax = 0x0000;
        r.ecx = (NDESC + 1) as u32;
        call31(&mut r);
        st_check(cf(&r), b"0000 over-large request succeeded\0".as_ptr());
        st_check(lo16(r.eax) == E_DESC_UNAVAIL, b"0000 exhaustion wrong error\0".as_ptr());

        // Drain the table for real, then confirm the next request fails
        // cleanly rather than handing out a stale or out-of-range selector.
        let mut guard = 0;
        loop {
            let mut r = blank();
            r.eax = 0x0000;
            r.ecx = 8;
            call31(&mut r);
            if cf(&r) {
                st_check(lo16(r.eax) == E_DESC_UNAVAIL, b"drain: wrong error\0".as_ptr());
                break;
            }
            guard += 1;
            if guard > NDESC {
                st_check(false, b"descriptor table never exhausted\0".as_ptr());
                break;
            }
        }
        unsafe {
            kprintf(
                b"[dpmi-st] I descriptor table drained after %u further blocks; exhaustion -> CF=1 AX=%04x\n\0"
                    .as_ptr(),
                guard as u32,
                E_DESC_UNAVAIL as u32,
            );
        }
    }

    // ---- J: 0500h free memory info, the block Doom sizes its heap from ----
    //
    // Resets the host first, so it is self-contained rather than inheriting the
    // descriptor table case I deliberately drained. A test whose result depends
    // on which group ran before it is a test that will eventually pass for the
    // wrong reason.
    unsafe { ST_CASE = 10 };
    {
        unsafe {
            dpmi_host_reset_rs();
            QUIET = 1;
            ST_MEMINFO_CALLS = 0;
            ST_MEMINFO_REFUSE = 0;
        }
        st_arena_reset();

        // The order-sensitivity precondition. If any two fixture values were
        // equal, every per-offset assertion below would survive a permutation
        // of the block, and this whole group would be decoration.
        st_check(
            ST_LARGEST_BYTES != ST_TOTAL_PAGES
                && ST_TOTAL_PAGES != ST_FREE_PAGES
                && ST_LARGEST_BYTES != ST_FREE_PAGES,
            b"0500 fixture values are not pairwise distinct: the field-order checks below would be blind\0".as_ptr(),
        );
        st_check(
            ST_LARGEST_BYTES != UNKNOWN && ST_TOTAL_PAGES != UNKNOWN && ST_FREE_PAGES != UNKNOWN,
            b"0500 fixture value collides with the UNKNOWN sentinel\0".as_ptr(),
        );

        // ---- unprovisioned: REFUSE, and write NOTHING ----
        let mut r = blank();
        r.eax = 0x0500;
        r.edi = 0x100;
        call31(&mut r);
        st_check(cf(&r), b"0500 with no arena bound succeeded\0".as_ptr());
        st_check(lo16(r.eax) == E_UNSUPPORTED, b"0500 no-arena wrong error\0".as_ptr());
        st_check(st_untouched(0, 4096), b"0500 no-arena wrote into the guest anyway\0".as_ptr());

        unsafe { dpmi_bind_arena_rs(core::ptr::addr_of_mut!(ST_ARENA)) };

        let mut r = blank();
        r.eax = 0x0500;
        r.edi = 0x100;
        call31(&mut r);
        st_check(cf(&r), b"0500 with no memory model bound succeeded\0".as_ptr());
        st_check(lo16(r.eax) == E_UNSUPPORTED, b"0500 no-model wrong error\0".as_ptr());
        st_check(st_untouched(0, 4096), b"0500 no-model wrote a block anyway\0".as_ptr());

        // Nothing above this line may have consulted the model: an
        // unprovisioned host must fail BEFORE asking anyone for numbers.
        st_check(
            unsafe { ST_MEMINFO_CALLS } == 0,
            b"0500 consulted the memory model while unprovisioned\0".as_ptr(),
        );

        // ---- a model that cannot answer must REFUSE, not report zeroes ----
        unsafe {
            dpmi_bind_meminfo_rs(Some(fake_meminfo), core::ptr::null_mut());
            ST_MEMINFO_REFUSE = 1;
        }
        let before = unsafe { ST_MEMINFO_CALLS };
        let mut r = blank();
        r.eax = 0x0500;
        r.edi = 0x100;
        call31(&mut r);
        st_check(cf(&r), b"0500 with a refusing model succeeded\0".as_ptr());
        st_check(st_untouched(0, 4096), b"0500 refusing model still wrote a block\0".as_ptr());
        // The refusal must have come FROM the model, not from a short-circuit
        // that never asked. Otherwise this arm would pass even if 0500 refused
        // for some entirely different reason.
        st_check(
            unsafe { ST_MEMINFO_CALLS } == before + 1,
            b"0500 refusal did not come from the model (it was never consulted)\0".as_ptr(),
        );
        unsafe { ST_MEMINFO_REFUSE = 0 };

        // ---- the real call ----
        // Counting the INCREMENT rather than a total: a hard-coded total is a
        // magic number that has to be re-derived every time an arm is added
        // above, and getting it wrong is a self-test failure that says nothing
        // about the code under test. (It said exactly that on the first run.)
        let before = unsafe { ST_MEMINFO_CALLS };
        let mut r = blank();
        r.eax = 0x0500;
        r.es = 0x0010; // TI=0: a flat GDT selector, base 0, so flat == EDI
        r.edi = 0x100;
        call31(&mut r);
        st_check(!cf(&r), b"0500 set CF\0".as_ptr());
        st_check(
            unsafe { ST_MEMINFO_CALLS } == before + 1,
            b"0500 consulted the model other than exactly once\0".as_ptr(),
        );

        // EVERY dword, individually. This is the field-order check.
        st_check(st_dw(0x100 + 0x00) == ST_LARGEST_BYTES, b"0500 [0] 00h largest free block\0".as_ptr());
        st_check(st_dw(0x100 + 0x04) == UNKNOWN, b"0500 [1] 04h max unlocked page alloc must be UNKNOWN\0".as_ptr());
        st_check(st_dw(0x100 + 0x08) == UNKNOWN, b"0500 [2] 08h max locked page alloc must be UNKNOWN\0".as_ptr());
        st_check(st_dw(0x100 + 0x0C) == ST_TOTAL_PAGES, b"0500 [3] 0Ch linear address space pages\0".as_ptr());
        st_check(st_dw(0x100 + 0x10) == UNKNOWN, b"0500 [4] 10h total unlocked pages must be UNKNOWN\0".as_ptr());
        st_check(st_dw(0x100 + 0x14) == ST_FREE_PAGES, b"0500 [5] 14h total free pages\0".as_ptr());
        st_check(st_dw(0x100 + 0x18) == UNKNOWN, b"0500 [6] 18h total physical pages must be UNKNOWN\0".as_ptr());
        st_check(st_dw(0x100 + 0x1C) == ST_FREE_PAGES, b"0500 [7] 1Ch free linear address space pages\0".as_ptr());
        st_check(st_dw(0x100 + 0x20) == 0, b"0500 [8] 20h paging file size must be a KNOWN zero\0".as_ptr());
        st_check(st_dw(0x100 + 0x24) == UNKNOWN, b"0500 [9] 24h reserved\0".as_ptr());
        st_check(st_dw(0x100 + 0x28) == UNKNOWN, b"0500 [10] 28h reserved\0".as_ptr());
        st_check(st_dw(0x100 + 0x2C) == UNKNOWN, b"0500 [11] 2Ch reserved\0".as_ptr());

        // Explicitly: the two fields most likely to be transposed are NOT.
        // Stated separately from the table above because a copy-paste slip in
        // the table would break both rows the same way.
        st_check(
            st_dw(0x100 + 0x0C) != ST_FREE_PAGES && st_dw(0x100 + 0x14) != ST_TOTAL_PAGES,
            b"0500 total-pages and free-pages are transposed\0".as_ptr(),
        );
        st_check(
            st_dw(0x100 + 0x00) != ST_TOTAL_PAGES && st_dw(0x100 + 0x00) != ST_FREE_PAGES,
            b"0500 [0] holds a PAGE count where Doom expects BYTES\0".as_ptr(),
        );

        // EXACTLY 0x30 bytes: not 0x20, not 0x80, and nothing before it.
        st_check(st_untouched(0x100 - 16, 16), b"0500 wrote BEFORE the block\0".as_ptr());
        st_check(st_untouched(0x100 + 0x30, 64), b"0500 wrote PAST the 0x30-byte block\0".as_ptr());

        unsafe {
            kprintf(
                b"[dpmi-st] J 0500 block @flat 0x%x: %08x %08x %08x %08x %08x %08x\n\0".as_ptr(),
                0x100u32,
                st_dw(0x100 + 0x00),
                st_dw(0x100 + 0x04),
                st_dw(0x100 + 0x08),
                st_dw(0x100 + 0x0C),
                st_dw(0x100 + 0x10),
                st_dw(0x100 + 0x14),
            );
            kprintf(
                b"[dpmi-st] J                       %08x %08x %08x %08x %08x %08x (0x30 bytes exactly, CF=0)\n\0".as_ptr(),
                st_dw(0x100 + 0x18),
                st_dw(0x100 + 0x1C),
                st_dw(0x100 + 0x20),
                st_dw(0x100 + 0x24),
                st_dw(0x100 + 0x28),
                st_dw(0x100 + 0x2C),
            );
        }

        // ---- ES:EDI is a SELECTOR pair, not a bare offset ----
        // An LDT selector with a non-zero base must shift the destination, or a
        // client whose ES is not the flat selector gets its block elsewhere.
        st_arena_reset();
        let mut r = blank();
        r.eax = 0x0000;
        r.ecx = 1;
        call31(&mut r);
        st_check(!cf(&r), b"0500 fixture: 0000 failed\0".as_ptr());
        let esel = lo16(r.eax);
        let mut r = blank();
        r.eax = 0x0007;
        r.ebx = esel as u32;
        r.ecx = 0;
        r.edx = 0x0200;
        call31(&mut r);
        st_check(!cf(&r), b"0500 fixture: 0007 failed\0".as_ptr());

        let mut r = blank();
        r.eax = 0x0500;
        r.es = esel;
        r.edi = 0x100;
        call31(&mut r);
        st_check(!cf(&r), b"0500 with a based ES set CF\0".as_ptr());
        st_check(
            st_dw(0x300) == ST_LARGEST_BYTES,
            b"0500 ignored the ES base: block is not at base+EDI\0".as_ptr(),
        );
        st_check(st_untouched(0x100, 16), b"0500 wrote at EDI as well as base+EDI\0".as_ptr());

        // ---- a freed LDT selector in ES is an ERROR, not base 0 ----
        let mut r = blank();
        r.eax = 0x0001;
        r.ebx = esel as u32;
        call31(&mut r);
        st_check(!cf(&r), b"0500 fixture: 0001 failed\0".as_ptr());
        st_arena_reset();
        let mut r = blank();
        r.eax = 0x0500;
        r.es = esel;
        r.edi = 0x100;
        call31(&mut r);
        st_check(cf(&r), b"0500 accepted a freed selector in ES\0".as_ptr());
        st_check(lo16(r.eax) == E_INVALID_SEL, b"0500 freed-ES wrong error\0".as_ptr());
        st_check(st_untouched(0, 4096), b"0500 freed-ES wrote a block anyway\0".as_ptr());

        // ---- a block that does not fit is REFUSED WHOLE, never partially ----
        st_arena_reset();
        let mut r = blank();
        r.eax = 0x0500;
        r.es = 0x0010;
        r.edi = 4096 - 16; // 0x30 bytes from here runs off the end
        call31(&mut r);
        st_check(cf(&r), b"0500 out-of-arena destination succeeded\0".as_ptr());
        st_check(lo16(r.eax) == E_INVALID_LINEAR, b"0500 out-of-arena wrong error\0".as_ptr());
        st_check(
            st_untouched(4096 - 16, 16),
            b"0500 PARTIALLY wrote a block that did not fit\0".as_ptr(),
        );
        st_check(
            unsafe { ST_ARENA.oob_wr } == 1,
            b"0500 out-of-arena did not reach the shared bounds counter\0".as_ptr(),
        );
        unsafe {
            kprintf(
                b"[dpmi-st] J unprovisioned/refusing-model/freed-ES -> CF=1 nothing written; out-of-arena -> CF=1 AX=%04x oob_wr=%u no partial write\n\0"
                    .as_ptr(),
                E_INVALID_LINEAR as u32,
                ST_ARENA.oob_wr,
            );
        }

        // Leave nothing behind for the final reset to trip over.
        unsafe {
            dpmi_host_reset_rs();
            QUIET = 1;
        }
    }

    // ---- K: seeding a descriptor at a NAMED selector ---------------------
    //
    // The three constants a Watcom no-extender guest names are the fixture,
    // because they are the ones this exists for. What is asserted is not that
    // the call returns 0 but that the RESOLVER then answers with the seeded
    // base: the resolver is what every effective address goes through, and a
    // seed that the resolver cannot see would be a table entry nothing reads.
    unsafe { ST_CASE = 11 };
    {
        unsafe { dpmi_host_reset_rs() };

        st_check(
            unsafe { dpmi_seed_desc_rs(0x0017, 0, 0xFFFF_FFFF, AR_DATA_RW, 0xC0) } == 0,
            b"seed 0017 (flat alias) refused\0".as_ptr(),
        );
        st_check(
            unsafe { dpmi_seed_desc_rs(0x0024, 0x0000_1000, 0xFF, AR_DATA_RW, 0x40) } == 0,
            b"seed 0024 (PSP) refused\0".as_ptr(),
        );
        st_check(
            unsafe { dpmi_seed_desc_rs(0x002C, 0x0000_2000, 0x1FF, AR_DATA_RW, 0x40) } == 0,
            b"seed 002C (environment) refused\0".as_ptr(),
        );

        let mut b: u32 = 0xDEAD_BEEF;
        let mut l: u32 = 0;
        let mut a: u8 = 0;
        st_check(
            unsafe { dpmi_sel_lookup_rs(0x0024, &mut b, &mut l, &mut a) } == 0,
            b"seeded 0024 does not resolve\0".as_ptr(),
        );
        st_check(b == 0x0000_1000, b"seeded 0024 resolves to the wrong base\0".as_ptr());
        st_check(l == 0xFF, b"seeded 0024 resolves to the wrong limit\0".as_ptr());

        let mut b: u32 = 0;
        st_check(
            unsafe { dpmi_sel_lookup_rs(0x002C, &mut b, core::ptr::null_mut(), core::ptr::null_mut()) } == 0,
            b"seeded 002C does not resolve\0".as_ptr(),
        );
        st_check(b == 0x0000_2000, b"seeded 002C resolves to the wrong base\0".as_ptr());

        // The 4 GB limit must come back as 4 GB, with page granularity
        // recorded. This is the shape go32_make_sel's 0009-before-0008 comment
        // is about: a 4 GB selector stored byte-granular reads 4096x too small.
        let mut b: u32 = 0;
        let mut l: u32 = 0;
        let mut a: u8 = 0;
        st_check(
            unsafe { dpmi_sel_lookup_rs(0x0017, &mut b, &mut l, &mut a) } == 0,
            b"seeded 0017 does not resolve\0".as_ptr(),
        );
        st_check(b == 0, b"seeded 0017 is not flat\0".as_ptr());
        st_check(l == 0xFFFF_FFFF, b"seeded 0017 lost its 4 GB limit\0".as_ptr());

        // REFUSALS, each for its own reason.
        st_check(
            unsafe { dpmi_seed_desc_rs(0x0024, 0x5000, 0xFF, AR_DATA_RW, 0x40) } == -2,
            b"seeding an already-live selector was allowed\0".as_ptr(),
        );
        let mut b: u32 = 0;
        unsafe { dpmi_sel_lookup_rs(0x0024, &mut b, core::ptr::null_mut(), core::ptr::null_mut()) };
        st_check(b == 0x0000_1000, b"a refused seed changed the descriptor anyway\0".as_ptr());
        st_check(
            unsafe { dpmi_seed_desc_rs(0x0020, 0, 0xFF, AR_DATA_RW, 0x40) } == -1,
            b"seeding a TI=0 (GDT) selector was allowed\0".as_ptr(),
        );
        st_check(
            unsafe { dpmi_seed_desc_rs(0x0007, 0, 0xFF, AR_DATA_RW, 0x40) } == -1,
            b"seeding descriptor index 0 was allowed\0".as_ptr(),
        );
        st_check(
            unsafe { dpmi_seed_desc_rs(0x0034, 0, 0xFF, 0x82, 0x40) } == -3,
            b"seeding a SYSTEM descriptor was allowed\0".as_ptr(),
        );

        // A seeded slot must be INVISIBLE to the client allocator, or the
        // guest's own 0000h would eventually be handed its own PSP selector.
        let mut r = blank();
        r.eax = 0x0000;
        r.ecx = 1;
        call31(&mut r);
        st_check(!cf(&r), b"0000 failed with seeded descriptors present\0".as_ptr());
        let got = lo16(r.eax);
        st_check(
            got != 0x0017 && got != 0x0024 && got != 0x002C,
            b"0000 handed out a SEEDED selector\0".as_ptr(),
        );

        unsafe {
            kprintf(
                b"[dpmi-st] K seeded 0017/0024/002C resolve to 00000000/00001000/00002000; live/GDT/index-0/system seeds refused; 0000 handed out %04x\n\0"
                    .as_ptr(),
                got as u32,
            );
            dpmi_host_reset_rs();
            QUIET = 1;
        }
    }

    let (checks, fails, first) = unsafe { (ST_CHECKS, ST_FAILS, ST_FIRST_FAIL) };
    unsafe {
        if !out_checks.is_null() {
            *out_checks = checks;
        }
        QUIET = 0;
        // Leave nothing behind: the census, the descriptors, the fake
        // allocator binding and the fake hook all go.
        dpmi_host_reset_rs();
    }
    if fails == 0 {
        0
    } else {
        first as i32
    }
}
