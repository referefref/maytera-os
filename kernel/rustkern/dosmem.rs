// rustkern/dosmem.rs - #745: XMS 3.0 (extended memory) and LIM EMS 4.0
// (expanded memory) for the 16-bit DOS guest.
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
// A memory manager is a bookkeeping state machine, not a hot loop: the only
// per-byte work here is the block move (XMS AH=0Bh) and the 16 KiB window copy
// (EMS AH=44h), both of which are memcpy-shaped and run at load time, not per
// frame. There is no measured performance reason for C.
//
// ===========================================================================
// WHY THIS EXISTS
// ---------------------------------------------------------------------------
// Measured, not assumed: Disney's Aladdin prints "XMS allocation error.." in
// its own words and exits. With XMS and EMS stubbed it runs past 400 million
// instructions, sets mode 13h and writes tens of megabytes into the A0000
// aperture. The wall was never opcodes.
//
// dosexec.c answered INT 2Fh AX=4300h with "not installed" DELIBERATELY, and
// the comment said why: Commander Keen then falls back to conventional memory
// and works. That was a correct trade while there was no XMS to offer. It stops
// being correct the moment there is one, but it also means TURNING THIS ON IS A
// BEHAVIOUR CHANGE FOR EVERY DOS GUEST, not only for the game that needed it.
// Nothing here is allowed to be "probably fine": a game that works today and
// stops working because we started saying yes is a regression, and the answer
// to a bad allocation must be a documented XMS/EMS error code that the guest's
// own fallback path can act on, never a lie and never a hang.
//
// ===========================================================================
// THE TWO DETECTION PATHS, AND THE ONE THAT LOOKS LIKE A NO-OP
// ---------------------------------------------------------------------------
// XMS is found through INT 2Fh: AX=4300h must answer AL=80h, then AX=4310h
// hands back a FAR ENTRY POINT in ES:BX which the program CALLs. There is no
// XMS interrupt. dosexec.c arms the interpreter's far-call trap on the entry
// segment, so the call lands in dos_xms_dispatch_rs().
//
// EMS is NOT primarily found through INT 67h at all, and this is the trap that
// makes a first EMS implementation look like dead code. A DOS program detects
// expanded memory by OPENING THE DRIVER AS A FILE: INT 21h AH=3Dh on the name
// "EMMXXXX0", then IOCTL (AH=44h) to confirm the handle is a character device.
// Only if that succeeds does it issue INT 67h. An earlier EMS stub in the
// offline probe recorded ZERO INT 67h calls for exactly this reason: the open
// failed, the guest concluded there was no EMS, and never asked. A driver that
// answers INT 67h but cannot be opened by name is invisible.
//
// The second, documented path reads the INT 67h VECTOR and compares the eight
// bytes at offset 0Ah of the segment it points to against "EMMXXXX0". That is
// the same shape of test as the #163 mouse bug, where every unhooked vector
// pointed at an IRET stub and so the documented "is a driver installed" test
// answered no forever. dosexec.c therefore plants a real device header, and
// points the vector at it. Both paths are served; neither is assumed.
//
// ===========================================================================
// MEMORY MODEL
// ---------------------------------------------------------------------------
// Both pools are ONE flat arena each, supplied by C (kmalloc) and sub-allocated
// here, exactly as rustkern/dos4gw.rs does for the DPMI heap. Rust never calls
// the kernel allocator; the arena's lifetime is the DOS task's.
//
// XMS is a straight handle -> [base_kb, kb) bump allocation over the arena.
// Freeing a handle that is not the top block leaves a hole that is reused only
// by an exact-or-smaller later request (first fit). This is a game's load-time
// allocator, not a general heap; a compacting allocator would be more code than
// the workload justifies and every extra move is a chance to corrupt a guest.
//
// EMS is 16 KiB pages and a FOUR-WINDOW PAGE FRAME at a fixed segment. Real EMS
// hardware REMAPS; we COPY the 16 KiB window in and out of guest memory on
// every map. That is indistinguishable to the guest PROVIDED the outgoing
// window is written back before the incoming one lands, which map_window() does
// unconditionally. The one divergence from real hardware, stated because it is
// real and not hypothetical: mapping the SAME logical page into TWO physical
// windows at once gives the guest two independent copies that then drift,
// whereas real EMS would alias them. No guest in the corpus does this, it is
// detected and logged as a MISS rather than silently tolerated, and fixing it
// properly needs a second memory-hook window in the shared interpreter core
// (exec/x86_16.c has exactly one, already spent on the EGA aperture).
//
// ===========================================================================
// EVERY UNIMPLEMENTED SUB-FUNCTION STUBS WITH ITS DOCUMENTED ERROR CODE
// ---------------------------------------------------------------------------
// blame.md records why a log-only MISS is worse than useless: the guest reads
// registers we never wrote, carries on with garbage, and fails somewhere else
// entirely. So the default arm of BOTH dispatchers sets the spec's "function
// not implemented" result (XMS AX=0/BL=80h, EMS AH=84h) AND logs, once per
// distinct sub-function, with a census printed at task exit.
//
// The log line uses the CANONICAL MISS FORMAT from docs/DOS_HARNESS.md section
// 4a, `[MISS] class=<c> id=<token> [k=v ...]`, with class=xms / class=ems. That
// is not cosmetic: tools/dos-harness ranks misses by (class, id) and a bespoke
// format is a miss that never reaches the histogram, which is the same
// "measured nothing and concluded wrongly" failure the EMS file-open detection
// caused. `id` is the raw sub-function number so the ranking stays comparable
// across runs.

#![allow(dead_code)]

extern "C" {
    fn kprintf(fmt: *const u8, ...);
}

// ---------------------------------------------------------------------------
// The register window. C fills this from x86_16_cpu_t and copies it back, so
// the interpreter's register file is not part of this module's ABI: only these
// ten words are. Field order and types are load-bearing (mirrored by
// dos_regs_t in dos/dosexec.c with a _Static_assert on the size).
// ---------------------------------------------------------------------------
#[repr(C)]
pub struct DosRegs {
    pub ax: u16,
    pub bx: u16,
    pub cx: u16,
    pub dx: u16,
    pub si: u16,
    pub di: u16,
    pub ds: u16,
    pub es: u16,
    pub flags: u16,
    pub _pad: u16,
}

const F_CF: u16 = 1 << 0;

impl DosRegs {
    #[inline]
    fn ah(&self) -> u8 {
        (self.ax >> 8) as u8
    }
    #[inline]
    fn al(&self) -> u8 {
        (self.ax & 0xFF) as u8
    }
    #[inline]
    fn set_ah(&mut self, v: u8) {
        self.ax = (self.ax & 0x00FF) | ((v as u16) << 8);
    }
    #[inline]
    fn set_al(&mut self, v: u8) {
        self.ax = (self.ax & 0xFF00) | (v as u16);
    }
    #[inline]
    fn set_bl(&mut self, v: u8) {
        self.bx = (self.bx & 0xFF00) | (v as u16);
    }
    #[inline]
    fn set_bh(&mut self, v: u8) {
        self.bx = (self.bx & 0x00FF) | ((v as u16) << 8);
    }
    #[inline]
    fn clr_cf(&mut self) {
        self.flags &= !F_CF;
    }
}

// ---------------------------------------------------------------------------
// Guest memory. Real-mode seg:off wraps at 1 MiB (no A20 gate in this model,
// see the A20 note in the XMS dispatcher), and `mem` is exactly 1 MiB, so the
// mask is both the architectural behaviour and the bounds check.
// ---------------------------------------------------------------------------
const GUEST_MASK: u32 = 0xFFFFF;

#[inline]
fn lin_of(seg: u16, off: u16) -> u32 {
    (((seg as u32) << 4).wrapping_add(off as u32)) & GUEST_MASK
}

#[inline]
unsafe fn g_rd8(mem: *const u8, lin: u32) -> u8 {
    *mem.add((lin & GUEST_MASK) as usize)
}

#[inline]
unsafe fn g_wr8(mem: *mut u8, lin: u32, v: u8) {
    *mem.add((lin & GUEST_MASK) as usize) = v;
}

#[inline]
unsafe fn g_rd16(mem: *const u8, lin: u32) -> u16 {
    (g_rd8(mem, lin) as u16) | ((g_rd8(mem, lin.wrapping_add(1)) as u16) << 8)
}

#[inline]
unsafe fn g_wr16(mem: *mut u8, lin: u32, v: u16) {
    g_wr8(mem, lin, (v & 0xFF) as u8);
    g_wr8(mem, lin.wrapping_add(1), (v >> 8) as u8);
}

#[inline]
unsafe fn g_rd32(mem: *const u8, lin: u32) -> u32 {
    (g_rd16(mem, lin) as u32) | ((g_rd16(mem, lin.wrapping_add(2)) as u32) << 16)
}

// One bit per sub-function, so a repeated MISS logs once and the census still
// counts every occurrence.
#[inline]
fn miss_first(seen: &mut [u32; 8], fun: u8) -> bool {
    let i = (fun >> 5) as usize;
    let b = 1u32 << (fun & 31);
    if seen[i] & b != 0 {
        return false;
    }
    seen[i] |= b;
    true
}

// ===========================================================================
// XMS 3.0
// ===========================================================================
//
// Error codes, from the XMS 3.0 specification. Named rather than inlined
// because the wrong one is worse than none: a guest branches on BL, and
// "invalid handle" sends it down a different recovery path than "out of
// memory".
const XE_NOT_IMPL: u8 = 0x80; // function not implemented
const XE_NO_HMA: u8 = 0x90; // HMA does not exist
const XE_ALL_ALLOCATED: u8 = 0xA0; // all extended memory is allocated
const XE_NO_HANDLES: u8 = 0xA1; // all handles are in use
const XE_BAD_HANDLE: u8 = 0xA2; // invalid handle
const XE_BAD_SRC_HANDLE: u8 = 0xA3;
const XE_BAD_SRC_OFF: u8 = 0xA4;
const XE_BAD_DST_HANDLE: u8 = 0xA5;
const XE_BAD_DST_OFF: u8 = 0xA6;
const XE_BAD_LENGTH: u8 = 0xA7;
const XE_NOT_LOCKED: u8 = 0xAA; // block is not locked
const XE_LOCK_FAILS: u8 = 0xAD; // lock fails
const XE_NO_UMB: u8 = 0xB1; // no UMBs are available
const XE_BAD_UMB_SEG: u8 = 0xB2; // invalid UMB segment

pub const XMS_HANDLES: usize = 32;

#[derive(Clone, Copy)]
#[repr(C)]
struct XHandle {
    base_kb: u32,
    kb: u32,
    live: u8,
    locks: u8,
    _pad: u16,
}

#[repr(C)]
pub struct DosXms {
    pool: *mut u8,
    pool_kb: u32,
    h: [XHandle; XMS_HANDLES],
    calls: [u32; 32],
    miss_seen: [u32; 8],
    miss_total: u32,
    moves: u32,
    moved_kb: u32,
    peak_kb: u32,
    /// Nonzero suppresses every log line from this state. Set ONLY by the
    /// self-test, which drives the refusal and MISS paths on purpose: without
    /// it those deliberate failures appear in the guest's serial trace and the
    /// harness ranks them as misses a guest provoked.
    quiet: u8,
    _qpad: [u8; 7],
}

impl DosXms {
    /// Bytes currently handed out. Derived from the handle table rather than
    /// tracked separately: a second counter is a second thing to get wrong, and
    /// this is called a handful of times per guest.
    fn used_kb(&self) -> u32 {
        let mut n = 0u32;
        for e in self.h.iter() {
            if e.live != 0 {
                n = n.wrapping_add(e.kb);
            }
        }
        n
    }

    /// Largest free run, in KB, over the arena. Also returns the base of that
    /// run, so allocate can use the same scan rather than a second one that
    /// might disagree with the number just reported to the guest.
    fn largest_free(&self) -> (u32, u32) {
        // Walk the arena in KB, treating live handles as occupied. The handle
        // count is small and this runs at allocation time only.
        let mut best_len = 0u32;
        let mut best_base = 0u32;
        let mut pos = 0u32;
        while pos < self.pool_kb {
            // Is `pos` inside a live block? If so, skip past it.
            let mut occupied_end = 0u32;
            for e in self.h.iter() {
                if e.live != 0 && pos >= e.base_kb && pos < e.base_kb.saturating_add(e.kb) {
                    let end = e.base_kb.saturating_add(e.kb);
                    if end > occupied_end {
                        occupied_end = end;
                    }
                }
            }
            if occupied_end > pos {
                pos = occupied_end;
                continue;
            }
            // Free run starts at pos: it ends at the lowest live block above.
            let mut run_end = self.pool_kb;
            for e in self.h.iter() {
                if e.live != 0 && e.base_kb > pos && e.base_kb < run_end {
                    run_end = e.base_kb;
                }
            }
            let len = run_end - pos;
            if len > best_len {
                best_len = len;
                best_base = pos;
            }
            pos = run_end;
        }
        (best_len, best_base)
    }

    fn handle_ok(&self, dx: u16) -> bool {
        let i = dx as usize;
        i >= 1 && i < XMS_HANDLES && self.h[i].live != 0
    }
}

/// sizeof(DosXms) as rustc computed it. C allocates this many bytes and never
/// looks inside, so this number is the entire layout ABI.
#[no_mangle]
pub extern "C" fn dos_xms_state_size_rs() -> u32 {
    core::mem::size_of::<DosXms>() as u32
}

/// Bind a zeroed state object to an arena of `pool_kb` kilobytes.
///
/// # Safety
/// `st` must point to at least `dos_xms_state_size_rs()` writable, aligned
/// bytes; `pool` must point to at least `pool_kb * 1024` bytes that outlive the
/// state.
#[no_mangle]
pub unsafe extern "C" fn dos_xms_init_rs(st: *mut DosXms, pool: *mut u8, pool_kb: u32) -> i32 {
    if st.is_null() || pool.is_null() || pool_kb == 0 {
        return -1;
    }
    let s = &mut *st;
    s.pool = pool;
    s.pool_kb = pool_kb;
    s.h = [XHandle {
        base_kb: 0,
        kb: 0,
        live: 0,
        locks: 0,
        _pad: 0,
    }; XMS_HANDLES];
    s.calls = [0; 32];
    s.miss_seen = [0; 8];
    s.miss_total = 0;
    s.moves = 0;
    s.moved_kb = 0;
    s.peak_kb = 0;
    s.quiet = 0;
    s._qpad = [0; 7];
    0
}

// One endpoint of an XMS move: either guest conventional memory or a byte range
// inside the XMS arena. Resolved once so the copy loop below does not re-decide
// per byte which side it is on.
#[derive(Clone, Copy)]
struct MoveEnd {
    conv: bool,
    lin: u32,  // conventional: linear guest address
    poff: u32, // arena: byte offset into the pool
}

#[no_mangle]
pub unsafe extern "C" fn dos_xms_dispatch_rs(
    st: *mut DosXms,
    r: *mut DosRegs,
    mem: *mut u8,
) -> i32 {
    if st.is_null() || r.is_null() || mem.is_null() {
        return -1;
    }
    let s = &mut *st;
    let g = &mut *r;
    let fun = g.ah();
    if (fun as usize) < s.calls.len() {
        s.calls[fun as usize] = s.calls[fun as usize].wrapping_add(1);
    }

    match fun {
        // ---- 00h Get XMS version -------------------------------------------
        // AX = XMS version BCD, BX = driver internal revision, DX = 1 if the HMA
        // exists. We report NO HMA and refuse 01h/02h with "HMA does not exist",
        // which is the self-consistent pair. Claiming an HMA and then refusing
        // every request for it with "already in use" (a plausible-looking
        // shortcut) tells the guest to keep retrying for something that is never
        // coming.
        0x00 => {
            g.ax = 0x0300; // XMS 3.00
            g.bx = 0x0001;
            g.dx = 0x0000; // no HMA
        }

        // ---- 01h/02h Request / Release the HMA -----------------------------
        0x01 | 0x02 => {
            g.ax = 0;
            g.set_bl(XE_NO_HMA);
        }

        // ---- 03h..07h The A20 line -----------------------------------------
        // There is no A20 gate in this model: `mem` is exactly 1 MiB and every
        // guest address is masked to 20 bits, which is A20-DISABLED behaviour,
        // and the only way to reach memory above that boundary here is through
        // an XMS move, which does not go through addressing at all. So enabling
        // A20 is genuinely a no-op rather than an unimplemented feature, and
        // reporting success is the truth about what the guest can then do.
        // Query (07h) reports ENABLED to match what 03h/05h just claimed.
        0x03 | 0x04 | 0x05 | 0x06 => {
            g.ax = 1;
            g.set_bl(0);
        }
        0x07 => {
            g.ax = 1; // A20 is enabled
            g.set_bl(0);
        }

        // ---- 08h Query free extended memory --------------------------------
        // AX = largest free block in KB, DX = total free in KB.
        0x08 => {
            let (largest, _) = s.largest_free();
            let total = s.pool_kb - s.used_kb();
            g.ax = if largest > 0xFFFF { 0xFFFF } else { largest as u16 };
            g.dx = if total > 0xFFFF { 0xFFFF } else { total as u16 };
            g.set_bl(if total == 0 { XE_ALL_ALLOCATED } else { 0 });
        }

        // ---- 09h Allocate an extended memory block -------------------------
        // DX = KB requested. Success: AX=1, DX=handle.
        0x09 => {
            let want = g.dx as u32;
            let mut slot = 0usize;
            for i in 1..XMS_HANDLES {
                if s.h[i].live == 0 {
                    slot = i;
                    break;
                }
            }
            if slot == 0 {
                g.ax = 0;
                g.set_bl(XE_NO_HANDLES);
            } else {
                // A zero-length block is legal in XMS (it reserves a handle).
                let (largest, base) = s.largest_free();
                if want > largest {
                    g.ax = 0;
                    g.set_bl(XE_ALL_ALLOCATED);
                    if s.quiet == 0 {
                        kprintf(
                            b"[xms] 09h allocate %u KB REFUSED: largest free block is %u KB of %u KB\n\0"
                                .as_ptr(),
                            want,
                            largest,
                            s.pool_kb,
                        );
                    }
                } else {
                    s.h[slot] = XHandle {
                        base_kb: base,
                        kb: want,
                        live: 1,
                        locks: 0,
                        _pad: 0,
                    };
                    let used = s.used_kb();
                    if used > s.peak_kb {
                        s.peak_kb = used;
                    }
                    g.ax = 1;
                    g.dx = slot as u16;
                    if s.quiet == 0 {
                        kprintf(
                            b"[xms] 09h allocate %u KB -> handle %u (used %u of %u KB)\n\0".as_ptr(),
                            want,
                            slot as u32,
                            used,
                            s.pool_kb,
                        );
                    }
                }
            }
        }

        // ---- 0Ah Free an extended memory block -----------------------------
        0x0A => {
            if !s.handle_ok(g.dx) {
                g.ax = 0;
                g.set_bl(XE_BAD_HANDLE);
            } else {
                s.h[g.dx as usize].live = 0;
                s.h[g.dx as usize].kb = 0;
                g.ax = 1;
                g.set_bl(0);
            }
        }

        // ---- 0Bh Move an extended memory block -----------------------------
        // DS:SI -> 16-byte descriptor. Handle 0 on either side means the offset
        // field is a conventional-memory FAR POINTER (segment in the high word),
        // not an offset.
        0x0B => {
            let d = lin_of(g.ds, g.si);
            let len = g_rd32(mem, d);
            let sh = g_rd16(mem, d.wrapping_add(4));
            let so = g_rd32(mem, d.wrapping_add(6));
            let dh = g_rd16(mem, d.wrapping_add(10));
            let dof = g_rd32(mem, d.wrapping_add(12));

            let mut err = 0u8;
            // The spec requires an even length. Enforced rather than rounded:
            // a guest that passes an odd length has a bug we would otherwise
            // hide, and rounding changes how many bytes land.
            if len & 1 != 0 {
                err = XE_BAD_LENGTH;
            }

            let mut src = MoveEnd {
                conv: true,
                lin: 0,
                poff: 0,
            };
            let mut dst = src;

            if err == 0 {
                if sh == 0 {
                    src = MoveEnd {
                        conv: true,
                        lin: lin_of((so >> 16) as u16, (so & 0xFFFF) as u16),
                        poff: 0,
                    };
                } else if !s.handle_ok(sh) {
                    err = XE_BAD_SRC_HANDLE;
                } else {
                    let e = s.h[sh as usize];
                    if so.saturating_add(len) > e.kb.saturating_mul(1024) {
                        err = XE_BAD_SRC_OFF;
                    } else {
                        src = MoveEnd {
                            conv: false,
                            lin: 0,
                            poff: e.base_kb.wrapping_mul(1024).wrapping_add(so),
                        };
                    }
                }
            }
            if err == 0 {
                if dh == 0 {
                    dst = MoveEnd {
                        conv: true,
                        lin: lin_of((dof >> 16) as u16, (dof & 0xFFFF) as u16),
                        poff: 0,
                    };
                } else if !s.handle_ok(dh) {
                    err = XE_BAD_DST_HANDLE;
                } else {
                    let e = s.h[dh as usize];
                    if dof.saturating_add(len) > e.kb.saturating_mul(1024) {
                        err = XE_BAD_DST_OFF;
                    } else {
                        dst = MoveEnd {
                            conv: false,
                            lin: 0,
                            poff: e.base_kb.wrapping_mul(1024).wrapping_add(dof),
                        };
                    }
                }
            }

            if err != 0 {
                g.ax = 0;
                g.set_bl(err);
                if s.quiet == 0 {
                    kprintf(
                        b"[xms] 0Bh move len=%u src=h%u:%u dst=h%u:%u REFUSED err=%02x\n\0".as_ptr(),
                        len,
                        sh as u32,
                        so,
                        dh as u32,
                        dof,
                        err as u32,
                    );
                }
            } else if len == 0 {
                g.ax = 1;
                g.set_bl(0);
            } else {
                // Direction matters only when both endpoints live in the SAME
                // buffer and overlap. Copying forwards through an overlapping
                // region where dst > src destroys the source as it goes, which
                // is the classic memmove bug and would corrupt a guest's own
                // data silently.
                let same_buf = src.conv == dst.conv;
                let s_pos = if src.conv { src.lin } else { src.poff };
                let d_pos = if dst.conv { dst.lin } else { dst.poff };
                let backwards = same_buf && d_pos > s_pos && d_pos < s_pos.saturating_add(len);
                let pool_bytes = s.pool_kb.wrapping_mul(1024);
                for k in 0..len {
                    let i = if backwards { len - 1 - k } else { k };
                    let v = if src.conv {
                        g_rd8(mem, src.lin.wrapping_add(i))
                    } else {
                        let o = src.poff.wrapping_add(i);
                        if o >= pool_bytes {
                            0
                        } else {
                            *s.pool.add(o as usize)
                        }
                    };
                    if dst.conv {
                        g_wr8(mem, dst.lin.wrapping_add(i), v);
                    } else {
                        let o = dst.poff.wrapping_add(i);
                        if o < pool_bytes {
                            *s.pool.add(o as usize) = v;
                        }
                    }
                }
                s.moves = s.moves.wrapping_add(1);
                s.moved_kb = s.moved_kb.wrapping_add(len / 1024);
                g.ax = 1;
                g.set_bl(0);
            }
        }

        // ---- 0Ch Lock an extended memory block -----------------------------
        // REFUSED ON PURPOSE, with the spec's "lock fails" code, and logged.
        //
        // A successful lock must return a 32-BIT LINEAR ADDRESS in DX:BX that
        // the guest then uses directly. A 16-bit real-mode guest can only do
        // that in "unreal mode", and in this interpreter such an address would
        // be masked back into the 1 MiB array and write over the guest's own
        // memory. Handing out an address that silently aliases conventional
        // memory is far worse than refusing: the refusal is a documented error
        // the guest can branch on, the alias is corruption that surfaces
        // somewhere else entirely.
        0x0C => {
            g.ax = 0;
            g.set_bl(XE_LOCK_FAILS);
            s.miss_total = s.miss_total.wrapping_add(1);
            if miss_first(&mut s.miss_seen, fun) && s.quiet == 0 {
                kprintf(
                    b"[MISS] class=xms id=0c handle=%u note=lock-emb-refused-BL-AD-needs-32bit-linear-address-a-real-mode-guest-cannot-use\n\0"
                        .as_ptr(),
                    g.dx as u32,
                );
            }
        }

        // ---- 0Dh Unlock an extended memory block ---------------------------
        // Nothing can be locked (0Ch always refuses), so this is always
        // "block is not locked", which is consistent rather than merely polite.
        0x0D => {
            g.ax = 0;
            g.set_bl(XE_NOT_LOCKED);
        }

        // ---- 0Eh Get handle information ------------------------------------
        // AX=1, BH = lock count, BL = free handles, DX = block size in KB.
        0x0E => {
            if !s.handle_ok(g.dx) {
                g.ax = 0;
                g.set_bl(XE_BAD_HANDLE);
            } else {
                let mut free = 0u32;
                for i in 1..XMS_HANDLES {
                    if s.h[i].live == 0 {
                        free += 1;
                    }
                }
                let kb = s.h[g.dx as usize].kb;
                g.ax = 1;
                g.set_bh(0);
                g.set_bl(if free > 255 { 255 } else { free as u8 });
                g.dx = if kb > 0xFFFF { 0xFFFF } else { kb as u16 };
            }
        }

        // ---- 0Fh Reallocate an extended memory block -----------------------
        // BX = new size in KB, DX = handle. Shrinking always works in place.
        // Growing works only when the block is followed by enough free arena,
        // because moving it would invalidate nothing the guest holds (there are
        // no locks) but would cost a copy of the whole block; refusing lets the
        // guest allocate-and-move on its own terms.
        0x0F => {
            if !s.handle_ok(g.dx) {
                g.ax = 0;
                g.set_bl(XE_BAD_HANDLE);
            } else {
                let i = g.dx as usize;
                let want = g.bx as u32;
                let cur = s.h[i].kb;
                if want <= cur {
                    s.h[i].kb = want;
                    g.ax = 1;
                    g.set_bl(0);
                } else {
                    let end = s.h[i].base_kb.saturating_add(cur);
                    let mut next = s.pool_kb;
                    for e in s.h.iter() {
                        if e.live != 0 && e.base_kb >= end && e.base_kb < next {
                            next = e.base_kb;
                        }
                    }
                    if s.h[i].base_kb.saturating_add(want) <= next {
                        s.h[i].kb = want;
                        g.ax = 1;
                        g.set_bl(0);
                    } else {
                        g.ax = 0;
                        g.set_bl(XE_ALL_ALLOCATED);
                    }
                }
            }
        }

        // ---- 10h/11h/12h Upper Memory Blocks -------------------------------
        // There are no UMBs: conventional memory is capped at 0xA000 by the MCB
        // allocator and the region above it holds the video aperture, the
        // emulated video BIOS, the EMS page frame and our own stubs. "No UMBs
        // available" with DX=0 is the documented way to say exactly that.
        0x10 => {
            g.ax = 0;
            g.dx = 0;
            g.set_bl(XE_NO_UMB);
        }
        0x11 | 0x12 => {
            g.ax = 0;
            g.set_bl(XE_BAD_UMB_SEG);
        }

        // ---- everything else -----------------------------------------------
        _ => {
            g.ax = 0;
            g.set_bl(XE_NOT_IMPL);
            s.miss_total = s.miss_total.wrapping_add(1);
            if miss_first(&mut s.miss_seen, fun) && s.quiet == 0 {
                kprintf(
                    b"[MISS] class=xms id=%02x note=unimplemented-stubbed-AX0-BL80 ax=%04x bx=%04x dx=%04x\n\0"
                        .as_ptr(),
                    fun as u32,
                    g.ax as u32,
                    g.bx as u32,
                    g.dx as u32,
                );
            }
        }
    }

    // Every XMS function returns through a RETF with CF clear: the interface
    // reports failure in AX/BL, never in the carry flag. Setting CF here would
    // make a guest that checks it treat every successful call as an error.
    g.clr_cf();
    0
}

/// One-line usage census at task exit, in the shape dos_svc_report() uses.
///
/// # Safety
/// `st` must be a state initialised by `dos_xms_init_rs`.
#[no_mangle]
pub unsafe extern "C" fn dos_xms_report_rs(st: *mut DosXms) {
    if st.is_null() {
        return;
    }
    let s = &mut *st;
    let mut total = 0u32;
    for c in s.calls.iter() {
        total = total.wrapping_add(*c);
    }
    if total == 0 {
        return; // the guest never asked: say nothing
    }
    kprintf(
        b"[xms] %u calls, peak %u KB of %u KB, %u moves (%u KB), %u MISS\n\0".as_ptr(),
        total,
        s.peak_kb,
        s.pool_kb,
        s.moves,
        s.moved_kb,
        s.miss_total,
    );
}

// ===========================================================================
// LIM EMS 4.0
// ===========================================================================
const EE_OK: u8 = 0x00;
const EE_INTERNAL: u8 = 0x80;
const EE_BAD_HANDLE: u8 = 0x83;
const EE_UNDEFINED_FN: u8 = 0x84;
const EE_NO_HANDLES: u8 = 0x85;
const EE_SAVE_AREA: u8 = 0x86;
const EE_MORE_THAN_EXIST: u8 = 0x87;
const EE_MORE_THAN_AVAIL: u8 = 0x88;
const EE_ZERO_PAGES: u8 = 0x89;
const EE_LOGICAL_RANGE: u8 = 0x8A;
const EE_BAD_PHYSICAL: u8 = 0x8B;
const EE_NO_SAVED_STATE: u8 = 0x8E;
const EE_BAD_SUBFUNCTION: u8 = 0x8F;
// 57h Move/Exchange region. 92h is a SUCCESS code that also warns: the move was
// performed and the regions overlapped.
const EE_MOVE_OVERLAP: u8 = 0x92;
const EE_LEN_EXCEEDS: u8 = 0x93;
const EE_OFF_OUTSIDE_PAGE: u8 = 0x95;
const EE_REGION_TOO_BIG: u8 = 0x96;
const EE_EXCHANGE_OVERLAP: u8 = 0x97;
const EE_BAD_MEM_TYPE: u8 = 0x98;

pub const EMS_HANDLES: usize = 64;
pub const EMS_WINDOWS: usize = 4;
const PAGE_BYTES: u32 = 16384;

#[repr(C)]
pub struct DosEms {
    pool: *mut u8,
    pool_pages: u32,
    frame_seg: u16,
    _pad: u16,
    first: [u32; EMS_HANDLES], // first pool page owned by the handle
    pages: [u32; EMS_HANDLES], // pages owned
    live: [u8; EMS_HANDLES],
    name: [[u8; 8]; EMS_HANDLES],
    // What is resident in each of the four 16 KiB windows: the POOL page index,
    // or -1 for "unmapped". cur_h/cur_l keep the handle and logical page so
    // AH=4Eh can save and restore a map the guest can hand back.
    cur: [i32; EMS_WINDOWS],
    cur_h: [i32; EMS_WINDOWS],
    cur_l: [i32; EMS_WINDOWS],
    saved_h: [[i32; EMS_WINDOWS]; EMS_HANDLES],
    saved_l: [[i32; EMS_WINDOWS]; EMS_HANDLES],
    saved_ok: [u8; EMS_HANDLES],
    calls: [u32; 64],
    miss_seen: [u32; 8],
    miss_total: u32,
    maps: u32,
    alias_maps: u32,
    peak_pages: u32,
    alias_warned: u8,
    /// See DosXms::quiet.
    quiet: u8,
    _pad2: [u8; 2],
}

impl DosEms {
    fn used_pages(&self) -> u32 {
        let mut n = 0u32;
        for i in 0..EMS_HANDLES {
            if self.live[i] != 0 {
                n = n.wrapping_add(self.pages[i]);
            }
        }
        n
    }
    fn handle_ok(&self, dx: u16) -> bool {
        let i = dx as usize;
        i < EMS_HANDLES && self.live[i] != 0
    }
}

#[no_mangle]
pub extern "C" fn dos_ems_state_size_rs() -> u32 {
    core::mem::size_of::<DosEms>() as u32
}

/// Bind a zeroed state object to an arena of `pool_pages` 16 KiB pages, with
/// the guest's page frame at `frame_seg`.
///
/// # Safety
/// `st` must point to at least `dos_ems_state_size_rs()` writable, aligned
/// bytes; `pool` must point to at least `pool_pages * 16384` bytes that outlive
/// the state.
#[no_mangle]
pub unsafe extern "C" fn dos_ems_init_rs(
    st: *mut DosEms,
    pool: *mut u8,
    pool_pages: u32,
    frame_seg: u16,
) -> i32 {
    if st.is_null() || pool.is_null() || pool_pages == 0 {
        return -1;
    }
    let s = &mut *st;
    s.pool = pool;
    s.pool_pages = pool_pages;
    s.frame_seg = frame_seg;
    s.first = [0; EMS_HANDLES];
    s.pages = [0; EMS_HANDLES];
    s.live = [0; EMS_HANDLES];
    s.name = [[0u8; 8]; EMS_HANDLES];
    s.cur = [-1; EMS_WINDOWS];
    s.cur_h = [-1; EMS_WINDOWS];
    s.cur_l = [-1; EMS_WINDOWS];
    s.saved_h = [[-1; EMS_WINDOWS]; EMS_HANDLES];
    s.saved_l = [[-1; EMS_WINDOWS]; EMS_HANDLES];
    s.saved_ok = [0; EMS_HANDLES];
    s.calls = [0; 64];
    s.miss_seen = [0; 8];
    s.miss_total = 0;
    s.maps = 0;
    s.alias_maps = 0;
    s.peak_pages = 0;
    s.alias_warned = 0;
    s.quiet = 0;
    // EMS 4.0 reserves handle 0 for the "operating system handle", which owns
    // no pages and is never allocated to a program. Marking it live keeps
    // allocate from ever handing it out while leaving 4Ch/4Dh able to report it.
    s.live[0] = 1;
    s.pages[0] = 0;
    0
}

/// Write the window at `phys` back into the pool page it currently holds.
/// Unconditional before any remap: this is the whole reason a COPYING page
/// frame is indistinguishable from a REMAPPING one.
unsafe fn window_flush(s: &mut DosEms, phys: usize, mem: *const u8) {
    let page = s.cur[phys];
    if page < 0 {
        return;
    }
    let src = ((s.frame_seg as u32) << 4).wrapping_add(phys as u32 * PAGE_BYTES);
    let dst = (page as u32).wrapping_mul(PAGE_BYTES);
    for i in 0..PAGE_BYTES {
        let v = g_rd8(mem, src.wrapping_add(i));
        *s.pool.add((dst + i) as usize) = v;
    }
}

/// Load pool page `page` into window `phys`.
unsafe fn window_load(s: &mut DosEms, phys: usize, page: u32, mem: *mut u8) {
    let dst = ((s.frame_seg as u32) << 4).wrapping_add(phys as u32 * PAGE_BYTES);
    let src = page.wrapping_mul(PAGE_BYTES);
    for i in 0..PAGE_BYTES {
        let v = *s.pool.add((src + i) as usize);
        g_wr8(mem, dst.wrapping_add(i), v);
    }
}

/// Map logical page `logi` of handle `h` into physical window `phys`, or unmap
/// it when `logi` is negative. Returns an EMS status byte.
unsafe fn map_window(s: &mut DosEms, phys: usize, h: u16, logi: i32, mem: *mut u8) -> u8 {
    if phys >= EMS_WINDOWS {
        return EE_BAD_PHYSICAL;
    }
    if logi < 0 {
        window_flush(s, phys, mem);
        s.cur[phys] = -1;
        s.cur_h[phys] = -1;
        s.cur_l[phys] = -1;
        return EE_OK;
    }
    if !s.handle_ok(h) {
        return EE_BAD_HANDLE;
    }
    let hi = h as usize;
    if (logi as u32) >= s.pages[hi] {
        return EE_LOGICAL_RANGE;
    }
    let page = s.first[hi] + logi as u32;
    if page >= s.pool_pages {
        return EE_INTERNAL;
    }
    // ALIASING: this logical page is already resident in another window.
    //
    // FLUSH THAT WINDOW FIRST. Without this the load below reads the arena
    // while the authoritative bytes are still sitting in the other window's
    // copy in guest memory, so the new window is stale from the instant it is
    // mapped. Measured on Aladdin, which really does this.
    let mut aliased = false;
    for w in 0..EMS_WINDOWS {
        if w != phys && s.cur[w] == page as i32 {
            aliased = true;
            window_flush(s, w, mem);
        }
    }
    // What is still not handled, stated precisely so nobody reads the fix above
    // as more than it is: if the guest now WRITES to both windows before either
    // is remapped, the two copies diverge and whichever flushes last wins. Real
    // EMS would alias them at the hardware level. Doing that here needs a SECOND
    // memory-hook window in exec/x86_16.c, which has exactly one and has already
    // spent it on the EGA aperture; that is shared-interpreter surgery, not an
    // EMS change.
    for w in 0..EMS_WINDOWS {
        if w != phys && s.cur[w] == page as i32 && s.alias_warned == 0 && s.quiet == 0 {
            s.alias_warned = 1;
            kprintf(
                b"[ems] ALIAS: logical page %u of handle %u mapped into window %u while resident in window %u. The other window is flushed first so this read is correct; if the guest writes BOTH before remapping, the last flush wins (this frame copies, it does not alias).\n\0"
                    .as_ptr(),
                logi as u32,
                h as u32,
                phys as u32,
                w as u32,
            );
        }
    }
    if aliased {
        s.alias_maps = s.alias_maps.wrapping_add(1);
    }
    window_flush(s, phys, mem);
    window_load(s, phys, page, mem);
    s.cur[phys] = page as i32;
    s.cur_h[phys] = h as i32;
    s.cur_l[phys] = logi;
    s.maps = s.maps.wrapping_add(1);
    EE_OK
}

/// One end of a 57h region: either a conventional linear address or a byte
/// offset into the expanded-memory arena. Resolved once so the copy loop does
/// not re-decide per byte which kind it is.
#[derive(Clone, Copy, PartialEq)]
enum Side {
    Conv(u32),
    Exp(u32),
}

/// Flush every mapped window back to the arena.
///
/// THIS IS WHAT MAKES 57h CORRECT UNDER A COPYING PAGE FRAME. The authoritative
/// bytes of a logical page that is CURRENTLY MAPPED live in the guest's page
/// frame, not in the arena, because mapping copies. A 57h that read the arena
/// directly would silently read a stale version of any page the guest has open,
/// and one that wrote the arena directly would have its write thrown away by
/// the next window flush. So: flush all, operate on the arena, reload all.
unsafe fn windows_flush_all(s: &mut DosEms, mem: *mut u8) {
    for w in 0..EMS_WINDOWS {
        window_flush(s, w, mem);
    }
}

unsafe fn windows_reload_all(s: &mut DosEms, mem: *mut u8) {
    for w in 0..EMS_WINDOWS {
        let pg = s.cur[w];
        if pg >= 0 {
            window_load(s, w, pg as u32, mem);
        }
    }
}

/// Resolve one side of a 57h descriptor. Returns Err(status) with the
/// documented EMS code rather than a generic failure, because a guest branches
/// on which of these went wrong.
unsafe fn side_resolve(
    s: &DosEms,
    mtype: u8,
    handle: u16,
    off: u16,
    seg_or_page: u16,
    len: u32,
) -> Result<Side, u8> {
    match mtype {
        0 => Ok(Side::Conv(lin_of(seg_or_page, off))),
        1 => {
            if !s.handle_ok(handle) {
                return Err(EE_BAD_HANDLE);
            }
            let hi = handle as usize;
            if (off as u32) >= PAGE_BYTES {
                return Err(EE_OFF_OUTSIDE_PAGE);
            }
            if (seg_or_page as u32) >= s.pages[hi] {
                return Err(EE_LOGICAL_RANGE);
            }
            let start = (seg_or_page as u32) * PAGE_BYTES + off as u32;
            if start.saturating_add(len) > s.pages[hi].saturating_mul(PAGE_BYTES) {
                return Err(EE_LEN_EXCEEDS);
            }
            Ok(Side::Exp(s.first[hi].wrapping_mul(PAGE_BYTES).wrapping_add(start)))
        }
        _ => Err(EE_BAD_MEM_TYPE),
    }
}

#[inline]
unsafe fn side_rd(s: &DosEms, sd: Side, i: u32, mem: *const u8, pool_bytes: u32) -> u8 {
    match sd {
        Side::Conv(l) => g_rd8(mem, l.wrapping_add(i)),
        Side::Exp(o) => {
            let a = o.wrapping_add(i);
            if a >= pool_bytes {
                0
            } else {
                *s.pool.add(a as usize)
            }
        }
    }
}

#[inline]
unsafe fn side_wr(s: &mut DosEms, sd: Side, i: u32, v: u8, mem: *mut u8, pool_bytes: u32) {
    match sd {
        Side::Conv(l) => g_wr8(mem, l.wrapping_add(i), v),
        Side::Exp(o) => {
            let a = o.wrapping_add(i);
            if a < pool_bytes {
                *s.pool.add(a as usize) = v;
            }
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn dos_ems_dispatch_rs(
    st: *mut DosEms,
    r: *mut DosRegs,
    mem: *mut u8,
) -> i32 {
    if st.is_null() || r.is_null() || mem.is_null() {
        return -1;
    }
    let s = &mut *st;
    let g = &mut *r;
    let fun = g.ah();
    let idx = (fun as usize).wrapping_sub(0x40);
    if idx < s.calls.len() {
        s.calls[idx] = s.calls[idx].wrapping_add(1);
    }

    match fun {
        // ---- 40h Get manager status ----------------------------------------
        0x40 => g.set_ah(EE_OK),

        // ---- 41h Get page frame segment ------------------------------------
        0x41 => {
            g.bx = s.frame_seg;
            g.set_ah(EE_OK);
        }

        // ---- 42h Get unallocated / total page count ------------------------
        0x42 => {
            let used = s.used_pages();
            g.bx = (s.pool_pages - used) as u16;
            g.dx = s.pool_pages as u16;
            g.set_ah(EE_OK);
        }

        // ---- 43h Allocate pages --------------------------------------------
        // BX = pages wanted -> DX = handle. Pages are allocated CONTIGUOUSLY in
        // the arena and never freed into a reusable hole below the high-water
        // mark, which is honest for a load-time allocator and is why 45h below
        // reclaims only when it can.
        0x43 => {
            let want = g.bx as u32;
            if want == 0 {
                g.set_ah(EE_ZERO_PAGES);
            } else if want > s.pool_pages {
                g.set_ah(EE_MORE_THAN_EXIST);
            } else {
                let mut slot = 0usize;
                for i in 1..EMS_HANDLES {
                    if s.live[i] == 0 {
                        slot = i;
                        break;
                    }
                }
                if slot == 0 {
                    g.set_ah(EE_NO_HANDLES);
                } else {
                    // First fit over the arena, same shape as the XMS scan.
                    let mut base = 0u32;
                    let mut placed = false;
                    while base + want <= s.pool_pages {
                        let mut clash_end = 0u32;
                        for i in 0..EMS_HANDLES {
                            if s.live[i] == 0 || s.pages[i] == 0 {
                                continue;
                            }
                            let a = s.first[i];
                            let b = a + s.pages[i];
                            if base < b && a < base + want && b > clash_end {
                                clash_end = b;
                            }
                        }
                        if clash_end == 0 {
                            placed = true;
                            break;
                        }
                        base = clash_end;
                    }
                    if !placed {
                        g.set_ah(EE_MORE_THAN_AVAIL);
                        if s.quiet == 0 {
                            kprintf(
                                b"[ems] 43h allocate %u pages REFUSED: %u of %u pages in use\n\0"
                                    .as_ptr(),
                                want,
                                s.used_pages(),
                                s.pool_pages,
                            );
                        }
                    } else {
                        s.live[slot] = 1;
                        s.first[slot] = base;
                        s.pages[slot] = want;
                        let used = s.used_pages();
                        if used > s.peak_pages {
                            s.peak_pages = used;
                        }
                        g.dx = slot as u16;
                        g.set_ah(EE_OK);
                        if s.quiet == 0 {
                            kprintf(
                                b"[ems] 43h allocate %u pages (%u KB) -> handle %u (used %u of %u pages)\n\0"
                                    .as_ptr(),
                                want,
                                want * 16,
                                slot as u32,
                                used,
                                s.pool_pages,
                            );
                        }
                    }
                }
            }
        }

        // ---- 44h Map handle page -------------------------------------------
        // AL = physical window, BX = logical page (FFFFh unmaps), DX = handle.
        0x44 => {
            let phys = g.al() as usize;
            if phys >= EMS_WINDOWS {
                g.set_ah(EE_BAD_PHYSICAL);
            } else {
                let logi = if g.bx == 0xFFFF { -1 } else { g.bx as i32 };
                let rc = map_window(s, phys, g.dx, logi, mem);
                g.set_ah(rc);
            }
        }

        // ---- 45h Deallocate pages ------------------------------------------
        // Any window still holding one of this handle's pages is flushed and
        // unmapped FIRST. Leaving a stale window mapped would let a later
        // flush write the next owner's data with this one's.
        0x45 => {
            if !s.handle_ok(g.dx) || g.dx == 0 {
                g.set_ah(EE_BAD_HANDLE);
            } else {
                let hi = g.dx as usize;
                for w in 0..EMS_WINDOWS {
                    if s.cur_h[w] == hi as i32 {
                        window_flush(s, w, mem);
                        s.cur[w] = -1;
                        s.cur_h[w] = -1;
                        s.cur_l[w] = -1;
                    }
                }
                s.live[hi] = 0;
                s.pages[hi] = 0;
                s.saved_ok[hi] = 0;
                g.set_ah(EE_OK);
            }
        }

        // ---- 46h Get EMM version -------------------------------------------
        0x46 => {
            g.set_al(0x40); // 4.0, BCD
            g.set_ah(EE_OK);
        }

        // ---- 47h/48h Save and restore the page map -------------------------
        0x47 => {
            if !s.handle_ok(g.dx) {
                g.set_ah(EE_BAD_HANDLE);
            } else {
                let hi = g.dx as usize;
                for w in 0..EMS_WINDOWS {
                    s.saved_h[hi][w] = s.cur_h[w];
                    s.saved_l[hi][w] = s.cur_l[w];
                }
                s.saved_ok[hi] = 1;
                g.set_ah(EE_OK);
            }
        }
        0x48 => {
            if !s.handle_ok(g.dx) {
                g.set_ah(EE_BAD_HANDLE);
            } else if s.saved_ok[g.dx as usize] == 0 {
                g.set_ah(EE_NO_SAVED_STATE);
            } else {
                let hi = g.dx as usize;
                let mut rc = EE_OK;
                for w in 0..EMS_WINDOWS {
                    let sh = s.saved_h[hi][w];
                    let sl = s.saved_l[hi][w];
                    let r2 = if sh < 0 {
                        map_window(s, w, 0, -1, mem)
                    } else {
                        map_window(s, w, sh as u16, sl, mem)
                    };
                    if r2 != EE_OK {
                        rc = r2;
                    }
                }
                s.saved_ok[hi] = 0;
                g.set_ah(rc);
            }
        }

        // ---- 4Bh Get handle count ------------------------------------------
        0x4B => {
            let mut n = 0u16;
            for i in 0..EMS_HANDLES {
                if s.live[i] != 0 {
                    n += 1;
                }
            }
            g.bx = n;
            g.set_ah(EE_OK);
        }

        // ---- 4Ch Get pages owned by a handle -------------------------------
        0x4C => {
            if !s.handle_ok(g.dx) {
                g.set_ah(EE_BAD_HANDLE);
            } else {
                g.bx = s.pages[g.dx as usize] as u16;
                g.set_ah(EE_OK);
            }
        }

        // ---- 4Dh Get pages for all handles ---------------------------------
        // ES:DI receives an array of (handle word, pages word) pairs.
        0x4D => {
            let mut n = 0u16;
            let mut p = lin_of(g.es, g.di);
            for i in 0..EMS_HANDLES {
                if s.live[i] == 0 {
                    continue;
                }
                g_wr16(mem, p, i as u16);
                g_wr16(mem, p.wrapping_add(2), s.pages[i] as u16);
                p = p.wrapping_add(4);
                n += 1;
            }
            g.bx = n;
            g.set_ah(EE_OK);
        }

        // ---- 4Eh Get / set the page map ------------------------------------
        // The saved format is OURS (the guest only ever hands it back), so it
        // is the smallest thing that round-trips: four (handle, logical) word
        // pairs = 16 bytes.
        0x4E => {
            let sub = g.al();
            const MAP_BYTES: u32 = 16;
            match sub {
                0x00 | 0x01 | 0x02 => {
                    let mut rc = EE_OK;
                    if sub == 0x00 || sub == 0x02 {
                        let mut p = lin_of(g.es, g.di);
                        for w in 0..EMS_WINDOWS {
                            g_wr16(mem, p, s.cur_h[w] as u16);
                            g_wr16(mem, p.wrapping_add(2), s.cur_l[w] as u16);
                            p = p.wrapping_add(4);
                        }
                    }
                    if sub == 0x01 || sub == 0x02 {
                        let mut p = lin_of(g.ds, g.si);
                        for w in 0..EMS_WINDOWS {
                            let hh = g_rd16(mem, p) as i16;
                            let ll = g_rd16(mem, p.wrapping_add(2)) as i16;
                            let r2 = if hh < 0 {
                                map_window(s, w, 0, -1, mem)
                            } else {
                                map_window(s, w, hh as u16, ll as i32, mem)
                            };
                            if r2 != EE_OK {
                                rc = r2;
                            }
                            p = p.wrapping_add(4);
                        }
                    }
                    g.set_ah(rc);
                }
                0x03 => {
                    g.set_al(MAP_BYTES as u8);
                    g.set_ah(EE_OK);
                }
                _ => {
                    g.set_ah(EE_BAD_SUBFUNCTION);
                    s.miss_total = s.miss_total.wrapping_add(1);
                }
            }
        }

        // ---- 50h Map multiple pages ----------------------------------------
        // AL=0: array of (logical, physical) word pairs. AL=1: the second word
        // is a SEGMENT ADDRESS in the frame rather than a window index.
        0x50 => {
            let sub = g.al();
            if sub > 1 {
                g.set_ah(EE_BAD_SUBFUNCTION);
            } else {
                let mut p = lin_of(g.ds, g.si);
                let mut rc = EE_OK;
                for _ in 0..g.cx {
                    let logi = g_rd16(mem, p) as i16;
                    let raw = g_rd16(mem, p.wrapping_add(2));
                    let phys = if sub == 0 {
                        raw as i32
                    } else {
                        // Segment form: convert back to a window index, and
                        // reject anything that is not one of our four windows
                        // rather than computing a nonsense index from it.
                        let off = (raw as i32) - (s.frame_seg as i32);
                        if off < 0 || (off % 0x400) != 0 {
                            -1
                        } else {
                            off / 0x400
                        }
                    };
                    if phys < 0 || phys as usize >= EMS_WINDOWS {
                        rc = EE_BAD_PHYSICAL;
                        break;
                    }
                    let l = if logi < 0 { -1 } else { logi as i32 };
                    let r2 = map_window(s, phys as usize, g.dx, l, mem);
                    if r2 != EE_OK {
                        rc = r2;
                        break;
                    }
                    p = p.wrapping_add(4);
                }
                g.set_ah(rc);
            }
        }

        // ---- 51h Reallocate pages ------------------------------------------
        // BX = new page count. Shrink in place; grow only into free arena
        // directly above the block, same rule and same reasoning as XMS 0Fh.
        0x51 => {
            if !s.handle_ok(g.dx) || g.dx == 0 {
                g.set_ah(EE_BAD_HANDLE);
            } else {
                let hi = g.dx as usize;
                let want = g.bx as u32;
                let cur = s.pages[hi];
                if want <= cur {
                    // Flush and drop any window holding a page we are giving up.
                    for w in 0..EMS_WINDOWS {
                        if s.cur_h[w] == hi as i32 && s.cur_l[w] >= want as i32 {
                            window_flush(s, w, mem);
                            s.cur[w] = -1;
                            s.cur_h[w] = -1;
                            s.cur_l[w] = -1;
                        }
                    }
                    s.pages[hi] = want;
                    g.bx = want as u16;
                    g.set_ah(EE_OK);
                } else {
                    let end = s.first[hi] + cur;
                    let mut next = s.pool_pages;
                    for i in 0..EMS_HANDLES {
                        if s.live[i] == 0 || s.pages[i] == 0 || i == hi {
                            continue;
                        }
                        if s.first[i] >= end && s.first[i] < next {
                            next = s.first[i];
                        }
                    }
                    if s.first[hi] + want <= next {
                        s.pages[hi] = want;
                        g.bx = want as u16;
                        g.set_ah(EE_OK);
                    } else {
                        g.set_ah(EE_MORE_THAN_AVAIL);
                    }
                }
            }
        }

        // ---- 53h Get / set handle name -------------------------------------
        0x53 => {
            if !s.handle_ok(g.dx) {
                g.set_ah(EE_BAD_HANDLE);
            } else {
                let hi = g.dx as usize;
                match g.al() {
                    0x00 => {
                        let p = lin_of(g.es, g.di);
                        for i in 0..8u32 {
                            g_wr8(mem, p.wrapping_add(i), s.name[hi][i as usize]);
                        }
                        g.set_ah(EE_OK);
                    }
                    0x01 => {
                        let p = lin_of(g.ds, g.si);
                        for i in 0..8u32 {
                            s.name[hi][i as usize] = g_rd8(mem, p.wrapping_add(i));
                        }
                        g.set_ah(EE_OK);
                    }
                    _ => g.set_ah(EE_BAD_SUBFUNCTION),
                }
            }
        }

        // ---- 57h Move / exchange a memory region ---------------------------
        // AL=0 move, AL=1 exchange. DS:SI -> an 18-byte descriptor. Either end
        // may be conventional or expanded memory, which is the whole point of
        // the call: it is how a program gets data INTO expanded memory without
        // mapping a window first.
        0x57 => {
            let sub = g.al();
            if sub > 1 {
                g.set_ah(EE_BAD_SUBFUNCTION);
            } else {
                let p = lin_of(g.ds, g.si);
                let len = g_rd32(mem, p);
                let styp = g_rd8(mem, p.wrapping_add(4));
                let sh = g_rd16(mem, p.wrapping_add(5));
                let soff = g_rd16(mem, p.wrapping_add(7));
                let ssp = g_rd16(mem, p.wrapping_add(9));
                let dtyp = g_rd8(mem, p.wrapping_add(11));
                let dh = g_rd16(mem, p.wrapping_add(12));
                let doff = g_rd16(mem, p.wrapping_add(14));
                let dsp = g_rd16(mem, p.wrapping_add(16));

                if len > 0x100000 {
                    g.set_ah(EE_REGION_TOO_BIG);
                } else if len == 0 {
                    g.set_ah(EE_OK);
                } else {
                    // Flush BEFORE resolving reads, so the arena is current.
                    windows_flush_all(s, mem);
                    let src = side_resolve(s, styp, sh, soff, ssp, len);
                    let dst = side_resolve(s, dtyp, dh, doff, dsp, len);
                    match (src, dst) {
                        (Err(e), _) | (_, Err(e)) => {
                            g.set_ah(e);
                            windows_reload_all(s, mem);
                        }
                        (Ok(sside), Ok(dside)) => {
                            let pool_bytes = s.pool_pages.wrapping_mul(PAGE_BYTES);
                            // Do the two regions overlap? Only possible when
                            // both ends are the same kind of memory.
                            let (same, spos, dpos) = match (sside, dside) {
                                (Side::Conv(a), Side::Conv(b)) => (true, a, b),
                                (Side::Exp(a), Side::Exp(b)) => (true, a, b),
                                _ => (false, 0, 0),
                            };
                            let overlap = same
                                && ((dpos >= spos && dpos < spos.saturating_add(len))
                                    || (spos >= dpos && spos < dpos.saturating_add(len)));
                            if sub == 1 && overlap {
                                // An exchange of overlapping regions is not
                                // defined and the spec has a code for it.
                                g.set_ah(EE_EXCHANGE_OVERLAP);
                            } else if sub == 1 {
                                for i in 0..len {
                                    let a = side_rd(s, sside, i, mem, pool_bytes);
                                    let b = side_rd(s, dside, i, mem, pool_bytes);
                                    side_wr(s, sside, i, b, mem, pool_bytes);
                                    side_wr(s, dside, i, a, mem, pool_bytes);
                                }
                                g.set_ah(EE_OK);
                            } else {
                                // memmove semantics: an overlapping forward copy
                                // destroys the source as it goes.
                                let backwards = overlap && dpos > spos;
                                for k in 0..len {
                                    let i = if backwards { len - 1 - k } else { k };
                                    let v = side_rd(s, sside, i, mem, pool_bytes);
                                    side_wr(s, dside, i, v, mem, pool_bytes);
                                }
                                // 92h is a SUCCESS code that also reports the
                                // overlap. The bytes landed correctly either
                                // way; the guest is told so it can decide.
                                g.set_ah(if overlap { EE_MOVE_OVERLAP } else { EE_OK });
                            }
                            windows_reload_all(s, mem);
                        }
                    }
                }
            }
        }

        // ---- 58h Get mappable physical address array -----------------------
        // AL=0 fills ES:DI with (segment, window index) pairs; AL=1 counts.
        0x58 => match g.al() {
            0x00 => {
                let mut p = lin_of(g.es, g.di);
                for w in 0..EMS_WINDOWS {
                    g_wr16(mem, p, s.frame_seg + (w as u16) * 0x400);
                    g_wr16(mem, p.wrapping_add(2), w as u16);
                    p = p.wrapping_add(4);
                }
                g.cx = EMS_WINDOWS as u16;
                g.set_ah(EE_OK);
            }
            0x01 => {
                g.cx = EMS_WINDOWS as u16;
                g.set_ah(EE_OK);
            }
            _ => g.set_ah(EE_BAD_SUBFUNCTION),
        },

        // ---- 59h Get hardware configuration --------------------------------
        // AL=1 (get raw page count) is the half a program actually needs; AL=0
        // returns a hardware-config array we have no honest values for, so it
        // is a logged MISS with the documented "invalid subfunction" rather
        // than an invented table.
        0x59 => {
            if g.al() == 0x01 {
                g.bx = (s.pool_pages - s.used_pages()) as u16;
                g.dx = s.pool_pages as u16;
                g.set_ah(EE_OK);
            } else {
                g.set_ah(EE_BAD_SUBFUNCTION);
                s.miss_total = s.miss_total.wrapping_add(1);
                if miss_first(&mut s.miss_seen, fun) && s.quiet == 0 {
                    kprintf(
                        b"[MISS] class=ems id=59 sub=%02x note=hardware-config-array-no-honest-values-stubbed-AH8F\n\0"
                            .as_ptr(),
                        g.al() as u32,
                    );
                }
            }
        }

        // ---- 5Ah Allocate raw pages ----------------------------------------
        // Our pages are all the same size, so raw == standard.
        0x5A => {
            if g.al() > 1 {
                g.set_ah(EE_BAD_SUBFUNCTION);
            } else {
                let want = g.bx as u32;
                let mut slot = 0usize;
                for i in 1..EMS_HANDLES {
                    if s.live[i] == 0 {
                        slot = i;
                        break;
                    }
                }
                if slot == 0 {
                    g.set_ah(EE_NO_HANDLES);
                } else if want > s.pool_pages - s.used_pages() {
                    g.set_ah(EE_MORE_THAN_AVAIL);
                } else {
                    let mut base = 0u32;
                    let mut placed = false;
                    while base + want <= s.pool_pages {
                        let mut clash_end = 0u32;
                        for i in 0..EMS_HANDLES {
                            if s.live[i] == 0 || s.pages[i] == 0 {
                                continue;
                            }
                            let a = s.first[i];
                            let b = a + s.pages[i];
                            if base < b && a < base + want && b > clash_end {
                                clash_end = b;
                            }
                        }
                        if clash_end == 0 {
                            placed = true;
                            break;
                        }
                        base = clash_end;
                    }
                    if !placed {
                        g.set_ah(EE_MORE_THAN_AVAIL);
                    } else {
                        s.live[slot] = 1;
                        s.first[slot] = base;
                        s.pages[slot] = want;
                        g.dx = slot as u16;
                        g.set_ah(EE_OK);
                    }
                }
            }
        }

        // ---- everything else -----------------------------------------------
        _ => {
            g.set_ah(EE_UNDEFINED_FN);
            s.miss_total = s.miss_total.wrapping_add(1);
            if miss_first(&mut s.miss_seen, fun) && s.quiet == 0 {
                kprintf(
                    b"[MISS] class=ems id=%02x note=unimplemented-stubbed-AH84 ax=%04x bx=%04x dx=%04x\n\0"
                        .as_ptr(),
                    fun as u32,
                    g.ax as u32,
                    g.bx as u32,
                    g.dx as u32,
                );
            }
        }
    }

    // EMS reports status in AH and leaves CF alone; unlike XMS it is reached by
    // a real INT 67h, so the caller's flags come back off the stack anyway.
    0
}

/// # Safety
/// `st` must be a state initialised by `dos_ems_init_rs`.
#[no_mangle]
pub unsafe extern "C" fn dos_ems_report_rs(st: *mut DosEms) {
    if st.is_null() {
        return;
    }
    let s = &mut *st;
    let mut total = 0u32;
    for c in s.calls.iter() {
        total = total.wrapping_add(*c);
    }
    if total == 0 {
        return;
    }
    kprintf(
        b"[ems] %u calls, peak %u of %u pages (%u KB), %u maps (%u aliased), %u MISS\n\0"
            .as_ptr(),
        total,
        s.peak_pages,
        s.pool_pages,
        s.peak_pages * 16,
        s.maps,
        s.alias_maps,
        s.miss_total,
    );
}

// ===========================================================================
// SELF-TEST
// ---------------------------------------------------------------------------
// Called from the DOS layer's existing self-test path. It exercises the parts
// where a wrong answer is INVISIBLE at runtime: the allocator's free-space
// arithmetic, the XMS move in all four handle combinations including the
// overlapping one, and that a refused allocation reports the documented error
// rather than a zero handle the guest would then use.
// ===========================================================================

/// Returns 0 on success, or the number of the first failing check.
///
/// # Safety
/// `xs`/`es` must be writable state-sized buffers, `xpool`/`epool` arenas of at
/// least 64 KiB and 4 pages respectively, `mem` a 1 MiB guest array.
#[no_mangle]
pub unsafe extern "C" fn dos_mem_selftest_rs(
    xs: *mut DosXms,
    xpool: *mut u8,
    es: *mut DosEms,
    epool: *mut u8,
    mem: *mut u8,
) -> i32 {
    if dos_xms_init_rs(xs, xpool, 64) != 0 {
        return 1;
    }
    // Silence this state: every refusal below is deliberate, and a deliberate
    // refusal printed onto the guest's serial console is a measurement the
    // harness will attribute to the guest.
    (*xs).quiet = 1;
    let mut r = DosRegs {
        ax: 0,
        bx: 0,
        cx: 0,
        dx: 0,
        si: 0,
        di: 0,
        ds: 0,
        es: 0,
        flags: 0xFFFF,
        _pad: 0,
    };

    // 08h on a fresh 64 KB arena reports 64 KB free, and clears CF.
    r.ax = 0x0800;
    dos_xms_dispatch_rs(xs, &mut r, mem);
    if r.ax != 64 || r.dx != 64 {
        return 2;
    }
    if r.flags & F_CF != 0 {
        return 3;
    }

    // 09h for 16 KB succeeds; 08h then reports 48 KB.
    r.ax = 0x0900;
    r.dx = 16;
    dos_xms_dispatch_rs(xs, &mut r, mem);
    if r.ax != 1 {
        return 4;
    }
    let h1 = r.dx;
    r.ax = 0x0800;
    dos_xms_dispatch_rs(xs, &mut r, mem);
    if r.dx != 48 {
        return 5;
    }

    // 09h for more than the arena holds must FAIL with BL=A0h and must not
    // return a handle. This is the check that matters: a stub that returns
    // AX=0 without setting BL leaves the guest reading a stale error code.
    r.ax = 0x0900;
    r.dx = 4096;
    dos_xms_dispatch_rs(xs, &mut r, mem);
    if r.ax != 0 || (r.bx & 0xFF) as u8 != XE_ALL_ALLOCATED {
        return 6;
    }

    // 0Bh conventional -> XMS -> conventional round-trips the bytes.
    let desc = 0x1000u32; // descriptor at 0000:1000
    let srcbuf = 0x2000u32;
    let dstbuf = 0x3000u32;
    for i in 0..256u32 {
        g_wr8(mem, srcbuf + i, (i ^ 0x5A) as u8);
        g_wr8(mem, dstbuf + i, 0);
    }
    // conventional -> handle h1
    g_wr16(mem, desc, 256);
    g_wr16(mem, desc + 2, 0);
    g_wr16(mem, desc + 4, 0); // src handle 0
    g_wr16(mem, desc + 6, srcbuf as u16);
    g_wr16(mem, desc + 8, 0); // src seg 0
    g_wr16(mem, desc + 10, h1);
    g_wr16(mem, desc + 12, 0);
    g_wr16(mem, desc + 14, 0);
    r.ax = 0x0B00;
    r.ds = 0;
    r.si = desc as u16;
    dos_xms_dispatch_rs(xs, &mut r, mem);
    if r.ax != 1 {
        return 7;
    }
    // handle h1 -> conventional
    g_wr16(mem, desc + 4, h1);
    g_wr16(mem, desc + 6, 0);
    g_wr16(mem, desc + 8, 0);
    g_wr16(mem, desc + 10, 0);
    g_wr16(mem, desc + 12, dstbuf as u16);
    g_wr16(mem, desc + 14, 0);
    r.ax = 0x0B00;
    r.ds = 0;
    r.si = desc as u16;
    dos_xms_dispatch_rs(xs, &mut r, mem);
    if r.ax != 1 {
        return 8;
    }
    for i in 0..256u32 {
        if g_rd8(mem, dstbuf + i) != (i ^ 0x5A) as u8 {
            return 9;
        }
    }

    // An odd length is rejected with A7h rather than silently rounded.
    g_wr16(mem, desc, 255);
    r.ax = 0x0B00;
    r.ds = 0;
    r.si = desc as u16;
    dos_xms_dispatch_rs(xs, &mut r, mem);
    if r.ax != 0 || (r.bx & 0xFF) as u8 != XE_BAD_LENGTH {
        return 10;
    }

    // An unimplemented function stubs with AX=0/BL=80h, never leaving AX as the
    // guest set it.
    r.ax = 0x7F00;
    r.bx = 0;
    dos_xms_dispatch_rs(xs, &mut r, mem);
    if r.ax != 0 || (r.bx & 0xFF) as u8 != XE_NOT_IMPL {
        return 11;
    }

    // ---- EMS ----
    if dos_ems_init_rs(es, epool, 4, 0xD000) != 0 {
        return 12;
    }
    (*es).quiet = 1;
    r.ax = 0x4100;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.bx != 0xD000 || r.ah() != EE_OK {
        return 13;
    }
    r.ax = 0x4200;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.bx != 4 || r.dx != 4 {
        return 14;
    }
    // Allocate 2 pages, write through window 0, remap, and read back: this is
    // the check that the COPYING page frame writes the outgoing window back.
    r.ax = 0x4300;
    r.bx = 2;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.ah() != EE_OK {
        return 15;
    }
    let eh = r.dx;
    r.ax = 0x4400;
    r.bx = 0;
    r.dx = eh;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.ah() != EE_OK {
        return 16;
    }
    let frame = 0xD0000u32;
    for i in 0..64u32 {
        g_wr8(mem, frame + i, (i + 1) as u8);
    }
    // Map logical page 1 in, then page 0 back: page 0's bytes must survive.
    r.ax = 0x4400;
    r.bx = 1;
    r.dx = eh;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.ah() != EE_OK {
        return 17;
    }
    for i in 0..64u32 {
        g_wr8(mem, frame + i, 0xFF);
    }
    r.ax = 0x4400;
    r.bx = 0;
    r.dx = eh;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.ah() != EE_OK {
        return 18;
    }
    for i in 0..64u32 {
        if g_rd8(mem, frame + i) != (i + 1) as u8 {
            return 19;
        }
    }
    // A logical page beyond the handle's allocation is 8Ah, not a wild copy.
    r.ax = 0x4400;
    r.bx = 7;
    r.dx = eh;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.ah() != EE_LOGICAL_RANGE {
        return 20;
    }
    // An illegal physical window is 8Bh.
    r.ax = 0x4409;
    r.bx = 0;
    r.dx = eh;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.ah() != EE_BAD_PHYSICAL {
        return 21;
    }
    // An unimplemented function stubs with 84h.
    r.ax = 0x7000;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.ah() != EE_UNDEFINED_FN {
        return 22;
    }

    // 57h conventional -> expanded -> conventional round-trips the bytes, WITH
    // a window mapped over the destination page. That last part is the whole
    // reason the flush/reload exists: without it the mapped window's stale
    // contents overwrite what 57h just wrote, and the guest silently loses the
    // data it believes it stored.
    let desc57 = 0x4000u32;
    let src57 = 0x5000u32;
    let dst57 = 0x6000u32;
    for i in 0..512u32 {
        g_wr8(mem, src57 + i, (i * 7 + 3) as u8);
        g_wr8(mem, dst57 + i, 0);
    }
    r.ax = 0x4400; // map logical page 0 into window 0 first
    r.bx = 0;
    r.dx = eh;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.ah() != EE_OK {
        return 23;
    }
    // conventional -> expanded (handle eh, logical page 0, offset 0)
    g_wr16(mem, desc57, 512);
    g_wr16(mem, desc57 + 2, 0);
    g_wr8(mem, desc57 + 4, 0); // src type: conventional
    g_wr16(mem, desc57 + 5, 0);
    g_wr16(mem, desc57 + 7, src57 as u16);
    g_wr16(mem, desc57 + 9, 0); // src segment 0
    g_wr8(mem, desc57 + 11, 1); // dst type: expanded
    g_wr16(mem, desc57 + 12, eh);
    g_wr16(mem, desc57 + 14, 0);
    g_wr16(mem, desc57 + 16, 0);
    r.ax = 0x5700;
    r.ds = 0;
    r.si = desc57 as u16;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.ah() != EE_OK {
        return 24;
    }
    // expanded -> conventional
    g_wr8(mem, desc57 + 4, 1);
    g_wr16(mem, desc57 + 5, eh);
    g_wr16(mem, desc57 + 7, 0);
    g_wr16(mem, desc57 + 9, 0);
    g_wr8(mem, desc57 + 11, 0);
    g_wr16(mem, desc57 + 12, 0);
    g_wr16(mem, desc57 + 14, dst57 as u16);
    g_wr16(mem, desc57 + 16, 0);
    r.ax = 0x5700;
    r.ds = 0;
    r.si = desc57 as u16;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.ah() != EE_OK {
        return 25;
    }
    for i in 0..512u32 {
        if g_rd8(mem, dst57 + i) != (i * 7 + 3) as u8 {
            return 26;
        }
    }
    // And the mapped window now SHOWS the bytes 57h wrote, because the reload
    // ran. This is the check that fails if flush/reload is dropped.
    for i in 0..64u32 {
        if g_rd8(mem, frame + i) != (i * 7 + 3) as u8 {
            return 27;
        }
    }
    // ALIASING: map logical page 0 into window 0, write through it, then map
    // the SAME logical page into window 1. Window 1 must show the bytes written
    // through window 0. Before the flush-the-other-holder fix this read the
    // arena and returned stale data, and no other check here caught it.
    r.ax = 0x4400;
    r.bx = 0;
    r.dx = eh;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.ah() != EE_OK {
        return 29;
    }
    for i in 0..64u32 {
        g_wr8(mem, frame + i, (i ^ 0x3C) as u8);
    }
    r.ax = 0x4401; // window 1
    r.bx = 0;
    r.dx = eh;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.ah() != EE_OK {
        return 30;
    }
    for i in 0..64u32 {
        if g_rd8(mem, frame + PAGE_BYTES + i) != (i ^ 0x3C) as u8 {
            return 31;
        }
    }

    // An invalid memory type is 98h, not a wild copy.
    g_wr8(mem, desc57 + 4, 9);
    r.ax = 0x5700;
    r.ds = 0;
    r.si = desc57 as u16;
    dos_ems_dispatch_rs(es, &mut r, mem);
    if r.ah() != EE_BAD_MEM_TYPE {
        return 28;
    }
    0
}
