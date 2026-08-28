// rustkern/x86_32.rs - the 32-bit protected-mode guest execution core (#740).
//
// WHY THIS EXISTS, AND WHY IT IS NOT A FLAG ON x86_16.c
//
// docs/DOS4GW_DESIGN.md section 1.1 measured the existing 16-bit interpreter and
// found the 32-bit DATA path already there (0x66/0x67 prefixes, exhi[8], SIB) and
// the 32-bit CONTROL path entirely absent: `ip` is a uint16_t, there is no EIP,
// no flat 4 GiB linear space and no 32-bit stack. A DOS/4GW guest runs 32-bit
// operations BY DEFAULT and jumps anywhere in a flat space, so it is not a mode
// of that core, it is a different core. This is that core.
//
// LANGUAGE: Rust, per the standing directive, and the security argument here is
// the strong form of it rather than the ceremonial one. Every byte this file
// decodes is untrusted guest input, and EVERY guest memory access is an
// attacker-controlled 32-bit index into a host buffer. In C that is an unchecked
// `arena[L]`; here it is a slice index that cannot be forgotten. This tree has
// already shipped heap overflows in exactly comparable C decoders (#476 ext2
// lookup, #490, #597). No performance justification for C was sought because
// none was measurable: the Rust ledger's own honest finding is parity on this
// kernel's soft-float build, and section 2 of the design measured the C core at
// 26 M insn/s with 7-15% of that spent in a debug preamble this core does not
// inherit.
//
// THE MEMORY MODEL: a WINDOW, not a 4 GiB array.
//
// Guest linear address L is valid iff mem_base <= L < mem_base + mem_size, and
// maps to arena byte L - mem_base. mem_base exists for two reasons: a DOS/4GW
// module is flat with base 0 but its objects sit at a reloc_base well above it,
// and the host-native oracle (tools/x86-32-oracle) runs the SAME instruction
// bytes in a real 32-bit Linux process whose addresses start at 0x08048000. One
// window covers both. An access outside it FAULTS the guest and stops the run,
// reporting the faulting EIP: it is never folded into a guard page the way the
// Win16 arena does it (x86_16.c:328), because that converts a guest memory error
// into silent corruption of the guest's own state, which is exactly the failure
// mode blame.md's x87 entry warns about.
//
// THE MISS POLICY, which is a deliberate departure from x86_16.c.
//
// blame.md: "an emulated opcode with a stack side effect must be implemented or
// must MISS loudly; it must never be a silent no-op", and, pulling the other
// way, "an interpreter that dies on an unimplemented opcode kills the diagnostic
// too". Both are right, so this core does neither of the two obvious things. An
// unimplemented opcode:
//   * is decoded far enough to compute its true LENGTH,
//   * records opcode bytes, EIP and that length in the CPU struct,
//   * increments miss_count,
//   * and RETURNS to the caller with exit_reason = MISS and EIP still pointing
//     AT the instruction.
// The core never guesses an effect it does not know. The caller decides whether
// to emulate it, skip it (miss_len is there so it can skip CORRECTLY), or stop.
// That keeps the diagnostic alive without inventing state.
//
// NOT IMPLEMENTED HERE, ON PURPOSE: x87 (D8..DF) is decoded for length and
// MISSes; guest FP goes to the existing software x87 per design section 8, and
// wiring it is its own change. Privileged and system instructions (LGDT, MOV CRn,
// task switches) MISS: a DOS/4GW guest under a DPMI host we implement never
// executes them, and a guest that does is a guest we want to hear about.

#![allow(dead_code)]

use core::ffi::c_void;

// ---------------------------------------------------------------------------
// EFLAGS bits
// ---------------------------------------------------------------------------
const F_CF: u32 = 1 << 0;
const F_PF: u32 = 1 << 2;
const F_AF: u32 = 1 << 4;
const F_ZF: u32 = 1 << 6;
const F_SF: u32 = 1 << 7;
const F_TF: u32 = 1 << 8;
const F_IF: u32 = 1 << 9;
const F_DF: u32 = 1 << 10;
const F_OF: u32 = 1 << 11;
/// Nested Task. Set only by a task-gate CALL, which this core does not model.
/// IRETD reads it because with NT set an IRET is a TASK SWITCH, not a return,
/// and executing the return form anyway would be a silently wrong answer.
const F_NT: u32 = 1 << 14;
/// Virtual-8086. Same reason: an IRET with VM set in the image is a mode
/// change. Nothing here can produce either bit; the checks exist so that a
/// guest which somehow does gets a MISS instead of a plausible wrong result.
const F_VM: u32 = 1 << 17;

/// The bits a host-native oracle can meaningfully compare. IOPL, NT, RF, VM, AC
/// and the reserved bits are host-process state, not guest arithmetic.
pub const EFLAGS_ARITH_MASK: u32 = F_CF | F_PF | F_AF | F_ZF | F_SF | F_DF | F_OF;

// ---------------------------------------------------------------------------
// Exit reasons. Mirrored in kernel/exec/x86_32.h with _Static_assert locks.
// ---------------------------------------------------------------------------
pub const X32_EXIT_BUDGET: u32 = 0; // max_insns retired, nothing wrong
pub const X32_EXIT_STOP_EIP: u32 = 1; // reached cpu.stop_eip (test harness)
pub const X32_EXIT_INT: u32 = 2; // software INT n; exit_arg = vector
pub const X32_EXIT_HLT: u32 = 3;
pub const X32_EXIT_IO_IN: u32 = 4; // exit_arg = port, io_size = 1/2/4
pub const X32_EXIT_IO_OUT: u32 = 5; // + io_val holds the value written
pub const X32_EXIT_MISS: u32 = 6; // unimplemented; EIP unmoved, miss_len valid
pub const X32_EXIT_FAULT_UD: u32 = 7; // genuinely undefined opcode
pub const X32_EXIT_FAULT_MEM: u32 = 8; // outside the arena window
pub const X32_EXIT_FAULT_DIV: u32 = 9; // #DE: divide by zero or quotient overflow
pub const X32_EXIT_FAULT_LIMIT: u32 = 10; // EIP left the window

extern "C" {
    fn kprintf(fmt: *const u8, ...);
}

/// How many unresolved-selector loads are printed per guest before the counter
/// silently carries the rest. Bounded so a guest that loads a bad selector in
/// a loop cannot flood the serial line it is being diagnosed on.
const SEL_MISS_LOG_MAX: u32 = 8;

/// Segment names for the unresolved-selector line, in S_* order.
const SEG_NAME: [&[u8]; 6] = [b"ES\0", b"CS\0", b"SS\0", b"DS\0", b"FS\0", b"GS\0"];

// Segment register indices.
const S_ES: usize = 0;
const S_CS: usize = 1;
const S_SS: usize = 2;
const S_DS: usize = 3;
const S_FS: usize = 4;
const S_GS: usize = 5;

/// The 32-bit guest CPU. `#[repr(C)]`; kernel/exec/x86_32.h carries the twin
/// declaration and `_Static_assert` locks on sizeof and on every offset the C
/// side reads, so a field added on one side and not the other fails the build.
#[repr(C)]
pub struct X8632Cpu {
    /// EAX ECX EDX EBX ESP EBP ESI EDI, in ModRM encoding order.
    pub regs: [u32; 8],
    pub eip: u32,
    pub eflags: u32,
    /// ES CS SS DS FS GS selectors. Flat model: values are carried, not decoded.
    pub seg: [u16; 6],
    pub pad0: u16,
    pub pad1: u16,
    /// Segment bases. All zero for a DOS/4GW flat guest; present so a DPMI host
    /// that hands out a non-flat descriptor is expressible without a core change.
    pub seg_base: [u32; 6],

    /// Guest arena: host pointer to the byte at guest linear address mem_base.
    pub mem: *mut u8,
    pub mem_base: u32,
    pub mem_size: u32,

    pub insn_count: u64,
    pub exit_reason: u32,
    pub exit_arg: u32,

    pub fault_eip: u32,
    pub fault_addr: u32,

    /// Harness breakpoint: when stop_eip_en != 0 and EIP == stop_eip at an
    /// instruction boundary, the run returns X32_EXIT_STOP_EIP.
    pub stop_eip: u32,
    pub stop_eip_en: u32,

    /// IN result in / OUT value out, with its width in bytes.
    pub io_val: u32,
    pub io_size: u32,

    pub miss_count: u32,
    pub miss_op: u32, // primary opcode byte
    pub miss_op2: u32, // second byte for the 0F map, else 0x100
    pub miss_modrm: u32, // ModRM byte if one was consumed, else 0x100
    pub miss_len: u32, // true length of the missing instruction, in bytes

    /// Opaque per-guest object for the C host (the dosexec.c `owner` pattern).
    pub owner: *mut c_void,

    /// (#740 doom-present) Memory-hook window, mirroring x86_16.c's
    /// mh_lo/mh_hi/mh_w/mh_r. This core had NO interception mechanism for any
    /// address range before this: every guest access on a valid page (in
    /// [mem_base, mem_base+mem_size)) went straight through `off()` into the
    /// flat `mem` slice. That is correct for ordinary guest memory, but wrong
    /// for the VGA aperture in an unchained (Mode X) mode, where four
    /// separate 64 KB PLANES share one address range and which plane a byte
    /// lands in is decided by the Sequencer Map Mask, not by the address. A
    /// flat write there silently overwrites whichever plane's data was there
    /// before, discarding the other three; dosexec.c's dos_present_modex()
    /// reads a SEPARATE per-plane array that a flat write into `mem` never
    /// touches at all, which is why every DOS/4GW Mode X title (DOOM among
    /// them) presented a permanently zero-filled, palette-fade-tinted flat
    /// colour: the plane array was zero everywhere, forever. See
    /// x86_32_set_mem_hook() and dos/dosexec.c's ega_mem_w32/ega_mem_r32.
    pub mh_lo: u32,
    pub mh_hi: u32,
    pub mh_w: Option<extern "C" fn(*mut X8632Cpu, u32, u32, i32) -> u32>,
    pub mh_r: Option<extern "C" fn(*mut X8632Cpu, u32, i32) -> u32>,

    /// (#740 dw2) LOW-MEMORY WRITE WATCH. Purely diagnostic: when `lw_hi` is
    /// non-zero, any guest store whose linear address is below it calls
    /// `lw_cb(cpu, linear, value, width, eip_of_the_storing_instruction)`
    /// BEFORE the store, and the store then happens exactly as it would have.
    /// It exists because "the guest's interrupt vector table is wrong and
    /// nothing in the tree can say who wrote it" is not a question code
    /// reading can answer: the writer may be the guest, the DOS service core,
    /// the DPMI real-mode-call bridge or the loader, and they share one arena.
    /// Off (`lw_hi == 0`) unless a host explicitly arms it.
    pub lw_hi: u32,
    pub lw_pad: u32,
    pub lw_cb: Option<extern "C" fn(*mut X8632Cpu, u32, u32, i32, u32)>,

    /// (#740 dw2) SELECTOR -> SEGMENT BASE. Every write to a segment register
    /// calls this to find out what the selector's descriptor says its base is;
    /// the answer goes in `seg_base[]`, which every effective address in this
    /// core already adds. The signature is exactly `dpmi_sel_lookup_rs`'s so
    /// the DPMI host's existing resolver plugs straight in with no shim:
    /// `(selector, out_base, out_limit, out_ar) -> 0 ok / -1 not ours`.
    ///
    /// None, or a selector the host does not know, means base 0. That is what
    /// this core did for EVERY selector before this field existed, so a flat
    /// DOS/4GW client is unaffected by construction.
    pub sb_cb: Option<
        extern "C" fn(u16, *mut u32, *mut u32, *mut u8) -> i32,
    >,

    /// (#211) SOFTWARE x87 STATE, ON THE CPU AND NOT IN A FILE STATIC.
    ///
    /// exec/x86_16.c moved its FP stack onto its cpu object for a reason worth
    /// repeating here: a SHARED x87 stack is the worst kind of shared state,
    /// because its errors are stack DEPTH errors. The moment two guests
    /// interleave a push the top pointer is wrong for BOTH and every later
    /// result drifts arbitrarily instead of failing cleanly.
    ///
    /// `fp_top` is a stack INDEX, not the architectural TOP: ST(0) is
    /// `fp[fp_top]`, a push decrements it, and 8 means empty. `fp_top & 7` is
    /// the architectural TOP field, which is why FNSTSW needs no translation.
    /// The semantics live in rustkern/x87.rs.
    pub fp: [u64; 8],
    pub fp_top: u32,
    pub fp_cw: u32,
    pub fp_sw: u32,
    pub fp_pad: u32,

    /// (#740 digsel) UNRESOLVED-SELECTOR ACCOUNTING, AND WHY IT IS NOT
    /// COSMETIC. A selector the bound resolver does not know used to resolve,
    /// silently, to base 0. That is not a neutral default: a Watcom guest that
    /// reads its command tail through a PSP selector this host never created
    /// then reads guest linear 0x80, which is the interrupt vector table, and
    /// parses the vector table as its argv. Nothing anywhere reported a
    /// problem, because base 0 is a plausible number and the bytes it yields
    /// are plausible bytes.
    ///
    /// `sel_miss_n` counts every load of a NON-NULL selector the resolver
    /// refused; the first `SEL_MISS_LOG_MAX` are printed with the EIP that
    /// loaded them, and `sel_miss_first_*` keep the first for a teardown line.
    /// A NULL selector is not counted: loading 0 is legal and means what it
    /// says.
    pub sel_miss_n: u32,
    pub sel_miss_logged: u32,
    pub sel_miss_first_sel: u32,
    pub sel_miss_first_eip: u32,

    /// Non-null selectors with TI CLEAR that the resolver did not know. These
    /// are counted but NOT reported as a defect, and the distinction is the
    /// whole value of the counter above. A TI=0 value is a GDT selector or a
    /// real-mode paragraph, not an LDT descriptor this host could ever have
    /// handed out: a 32-bit guest legitimately carries a DOS segment from
    /// INT 21h AH=48h in ES (see the AH=4Ah discriminator in
    /// rustkern/dos4gw.rs), and the bridge self-test does exactly that. Base 0
    /// is the documented pre-existing behaviour for them. Lumping the two
    /// together made the honest signal fire on every boot, which is how a
    /// diagnostic stops being read.
    pub sel_gdt_n: u32,
    pub sel_pad: u32,
}

// ---------------------------------------------------------------------------
// Internal execution context. Holds the arena as a real slice so that every
// access is bounds-checked by the language rather than by remembering.
// ---------------------------------------------------------------------------
struct Ctx<'a> {
    c: &'a mut X8632Cpu,
    mem: &'a mut [u8],
    /// Set by any memory helper that went outside the window. The dispatch loop
    /// checks it after every instruction rather than threading Result through
    /// several hundred call sites.
    faulted: bool,
    // Prefix state, reset per instruction.
    opsize32: bool,
    addr32: bool,
    seg_ovr: i32, // -1 = none
    rep: u8,      // 0 none, 0xF3 REP/REPE, 0xF2 REPNE
    lock: bool,
    /// EIP of the instruction currently being decoded.
    ins_eip: u32,
}

#[inline(always)]
fn mask_of(sz: u8) -> u32 {
    match sz {
        1 => 0xFF,
        2 => 0xFFFF,
        _ => 0xFFFF_FFFF,
    }
}

#[inline(always)]
fn msb_of(sz: u8) -> u32 {
    match sz {
        1 => 0x80,
        2 => 0x8000,
        _ => 0x8000_0000,
    }
}

#[inline(always)]
fn sext(v: u32, sz: u8) -> i64 {
    match sz {
        1 => (v as u8) as i8 as i64,
        2 => (v as u16) as i16 as i64,
        _ => (v as u32) as i32 as i64,
    }
}

impl<'a> Ctx<'a> {
    // -- memory ------------------------------------------------------------
    #[inline(always)]
    fn off(&mut self, la: u32, width: u32) -> Option<usize> {
        let base = self.c.mem_base;
        if la < base {
            self.mem_fault(la);
            return None;
        }
        let o = (la - base) as u64;
        if o + (width as u64) > self.mem.len() as u64 {
            self.mem_fault(la);
            return None;
        }
        Some(o as usize)
    }

    #[cold]
    fn mem_fault(&mut self, la: u32) {
        if !self.faulted {
            self.faulted = true;
            self.c.exit_reason = X32_EXIT_FAULT_MEM;
            self.c.fault_addr = la;
            self.c.fault_eip = self.ins_eip;
        }
    }

    // (#740 doom-present) Is `la` inside the registered VGA-aperture window?
    // A single check shared by every rd*/wr* below, so the hook window and
    // the ordinary flat path can never independently drift on where the
    // boundary is.
    /// THE ONLY PLACE THIS CORE WRITES A SEGMENT REGISTER. Every MOV Sreg,
    /// POP Sreg, far-pointer load AND FAR TRANSFER goes through here, so the
    /// selector value and the base derived from it cannot drift apart: there is
    /// one assignment, not eight.
    ///
    /// (#211) THAT SENTENCE WAS FALSE WHEN IT WAS WRITTEN, and this is what it
    /// cost. Six sites wrote seg[S_CS] directly - JMP FAR (both forms), CALL
    /// FAR (both forms), RETF and IRETD - and left seg_base[S_CS] on the
    /// PREVIOUS segment. It was undetectable for a DOS/4GW guest because that
    /// client is flat and every base is zero, so a stale base and a fresh one
    /// are the same number. The first non-flat client, a DJGPP program whose
    /// ___exit far-jumps to a selector based elsewhere, landed at the new
    /// offset inside the OLD segment: four thousand zero bytes below its own
    /// .text, which the interpreter walked as `add [eax],al` all the way back
    /// into crt0. It then re-ran its own startup eight times. Add a seventh
    /// site and route it here; the claim above is checkable by grepping for
    /// `seg[S_CS] =`, which should now find nothing outside this function.
    #[inline(always)]
    fn load_seg(&mut self, s: usize, sel: u16) {
        self.c.seg[s] = sel;
        let mut base: u32 = 0;
        if let Some(f) = self.c.sb_cb {
            let mut lim: u32 = 0;
            let mut ar: u8 = 0;
            if f(sel, &mut base as *mut u32, &mut lim as *mut u32, &mut ar as *mut u8) != 0
            {
                // (#740 digsel) SAY SO. The base stays 0, because stopping the
                // guest from inside a diagnostic would change behaviour for
                // every title at once. What changes is that it is no longer
                // SILENT: see the sel_miss_* fields for why silence, not the
                // zero, was the defect.
                base = 0;
                if sel != 0 && (sel & 4) == 0 {
                    // TI clear: a GDT selector or a real-mode paragraph.
                    // Counted, not accused. See sel_gdt_n.
                    self.c.sel_gdt_n = self.c.sel_gdt_n.wrapping_add(1);
                } else if sel != 0 {
                    self.c.sel_miss_n = self.c.sel_miss_n.wrapping_add(1);
                    if self.c.sel_miss_first_sel == 0 {
                        self.c.sel_miss_first_sel = sel as u32;
                        self.c.sel_miss_first_eip = self.ins_eip;
                    }
                    if self.c.sel_miss_logged < SEL_MISS_LOG_MAX {
                        self.c.sel_miss_logged += 1;
                        unsafe {
                            kprintf(
                                b"[x8632] UNRESOLVED SELECTOR %04x loaded into %s at EIP 0x%08X. No descriptor describes it, so it addresses BASE 0 and every access through it lands in the guest's first page, where the interrupt vector table is.\n\0"
                                    .as_ptr(),
                                sel as u32,
                                SEG_NAME[s].as_ptr(),
                                self.ins_eip,
                            );
                        }
                    }
                }
            }
        }
        self.c.seg_base[s] = base;
    }

    #[inline(always)]
    fn hook_hit(&self, la: u32) -> bool {
        self.c.mh_hi != 0 && la >= self.c.mh_lo && la < self.c.mh_hi
    }

    #[inline(always)]
    fn rd8(&mut self, la: u32) -> u8 {
        if self.hook_hit(la) {
            if let Some(f) = self.c.mh_r {
                let cptr: *mut X8632Cpu = self.c;
                return unsafe { f(cptr, la, 1) as u8 };
            }
        }
        match self.off(la, 1) {
            Some(o) => self.mem[o],
            None => 0,
        }
    }
    #[inline(always)]
    fn rd16(&mut self, la: u32) -> u16 {
        if self.hook_hit(la) {
            if let Some(f) = self.c.mh_r {
                let cptr: *mut X8632Cpu = self.c;
                return unsafe { f(cptr, la, 2) as u16 };
            }
        }
        match self.off(la, 2) {
            Some(o) => u16::from_le_bytes([self.mem[o], self.mem[o + 1]]),
            None => 0,
        }
    }
    #[inline(always)]
    fn rd32(&mut self, la: u32) -> u32 {
        if self.hook_hit(la) {
            if let Some(f) = self.c.mh_r {
                let cptr: *mut X8632Cpu = self.c;
                return unsafe { f(cptr, la, 4) };
            }
        }
        match self.off(la, 4) {
            Some(o) => u32::from_le_bytes([
                self.mem[o],
                self.mem[o + 1],
                self.mem[o + 2],
                self.mem[o + 3],
            ]),
            None => 0,
        }
    }
    #[inline(always)]
    fn rd(&mut self, la: u32, sz: u8) -> u32 {
        match sz {
            1 => self.rd8(la) as u32,
            2 => self.rd16(la) as u32,
            _ => self.rd32(la),
        }
    }
    #[inline(always)]
    fn wr8(&mut self, la: u32, v: u8) {
        if self.c.lw_hi != 0 && la < self.c.lw_hi {
            if let Some(f) = self.c.lw_cb {
                let cptr: *mut X8632Cpu = self.c;
                let e = self.ins_eip;
                unsafe { f(cptr, la, v as u32, 1, e) };
            }
        }
        if self.hook_hit(la) {
            if let Some(f) = self.c.mh_w {
                let cptr: *mut X8632Cpu = self.c;
                unsafe { f(cptr, la, v as u32, 1) };
                return;
            }
        }
        if let Some(o) = self.off(la, 1) {
            self.mem[o] = v;
        }
    }
    #[inline(always)]
    fn wr16(&mut self, la: u32, v: u16) {
        if self.c.lw_hi != 0 && la < self.c.lw_hi {
            if let Some(f) = self.c.lw_cb {
                let cptr: *mut X8632Cpu = self.c;
                let e = self.ins_eip;
                unsafe { f(cptr, la, v as u32, 2, e) };
            }
        }
        if self.hook_hit(la) {
            if let Some(f) = self.c.mh_w {
                let cptr: *mut X8632Cpu = self.c;
                unsafe { f(cptr, la, v as u32, 2) };
                return;
            }
        }
        if let Some(o) = self.off(la, 2) {
            let b = v.to_le_bytes();
            self.mem[o] = b[0];
            self.mem[o + 1] = b[1];
        }
    }
    #[inline(always)]
    fn wr32(&mut self, la: u32, v: u32) {
        if self.c.lw_hi != 0 && la < self.c.lw_hi {
            if let Some(f) = self.c.lw_cb {
                let cptr: *mut X8632Cpu = self.c;
                let e = self.ins_eip;
                unsafe { f(cptr, la, v as u32, 4, e) };
            }
        }
        if self.hook_hit(la) {
            if let Some(f) = self.c.mh_w {
                let cptr: *mut X8632Cpu = self.c;
                unsafe { f(cptr, la, v, 4) };
                return;
            }
        }
        if let Some(o) = self.off(la, 4) {
            let b = v.to_le_bytes();
            self.mem[o] = b[0];
            self.mem[o + 1] = b[1];
            self.mem[o + 2] = b[2];
            self.mem[o + 3] = b[3];
        }
    }
    #[inline(always)]
    fn wr(&mut self, la: u32, v: u32, sz: u8) {
        match sz {
            1 => self.wr8(la, v as u8),
            2 => self.wr16(la, v as u16),
            _ => self.wr32(la, v),
        }
    }

    // -- instruction fetch --------------------------------------------------
    #[inline(always)]
    fn fetch8(&mut self) -> u8 {
        let la = self.c.seg_base[S_CS].wrapping_add(self.c.eip);
        let v = self.rd8(la);
        self.c.eip = self.c.eip.wrapping_add(1);
        v
    }
    #[inline(always)]
    fn fetch16(&mut self) -> u16 {
        let a = self.fetch8() as u16;
        let b = self.fetch8() as u16;
        a | (b << 8)
    }
    #[inline(always)]
    fn fetch32(&mut self) -> u32 {
        let a = self.fetch16() as u32;
        let b = self.fetch16() as u32;
        a | (b << 16)
    }
    #[inline(always)]
    fn fetch_imm(&mut self, sz: u8) -> u32 {
        match sz {
            1 => self.fetch8() as u32,
            2 => self.fetch16() as u32,
            _ => self.fetch32(),
        }
    }

    // -- registers ----------------------------------------------------------
    #[inline(always)]
    fn r8(&self, i: usize) -> u8 {
        if i < 4 {
            self.c.regs[i] as u8
        } else {
            (self.c.regs[i - 4] >> 8) as u8
        }
    }
    #[inline(always)]
    fn set_r8(&mut self, i: usize, v: u8) {
        if i < 4 {
            self.c.regs[i] = (self.c.regs[i] & 0xFFFF_FF00) | v as u32;
        } else {
            self.c.regs[i - 4] = (self.c.regs[i - 4] & 0xFFFF_00FF) | ((v as u32) << 8);
        }
    }
    #[inline(always)]
    fn rget(&self, i: usize, sz: u8) -> u32 {
        match sz {
            1 => self.r8(i) as u32,
            2 => self.c.regs[i] & 0xFFFF,
            _ => self.c.regs[i],
        }
    }
    #[inline(always)]
    fn rset(&mut self, i: usize, v: u32, sz: u8) {
        match sz {
            1 => self.set_r8(i, v as u8),
            2 => self.c.regs[i] = (self.c.regs[i] & 0xFFFF_0000) | (v & 0xFFFF),
            _ => self.c.regs[i] = v,
        }
    }

    // -- stack --------------------------------------------------------------
    // The stack width follows the operand size, which for this core is the
    // guest's D bit (32) unless a 0x66 prefix says otherwise. SS.B is assumed 1
    // (a flat 32-bit guest); a 16-bit stack segment is a MISS-worthy guest, not
    // a silent wrong answer, and nothing DOS/4GW produces one.
    #[inline(always)]
    fn push(&mut self, v: u32, sz: u8) {
        let nsp = self.c.regs[4].wrapping_sub(sz as u32);
        self.c.regs[4] = nsp;
        let la = self.c.seg_base[S_SS].wrapping_add(nsp);
        self.wr(la, v, sz);
    }
    /// PUSH Sreg, which is NOT `push(seg as u32)`.
    ///
    /// ESP moves by the operand size, but only SIXTEEN BITS ARE WRITTEN: the
    /// upper half of a 32-bit stack slot is left holding whatever was there
    /// before. The SDM says the choice is implementation-defined and then says
    /// which way every implementation went: "all recent Core and Atom
    /// processors perform a 16-bit move, leaving the upper portion of the stack
    /// location unmodified". AMD does the same.
    ///
    /// Zero-extending is the obvious implementation and this core had it. It is
    /// invisible to any guest that pushes a selector and pops it back, and
    /// visible to one that pushes a selector and then reads the dword, which is
    /// why it survived 30 vectors: the host-native oracle caught it on the first
    /// vector that pushed CS at all (two bytes, in the stack window, #740).
    #[inline(always)]
    fn push_seg(&mut self, v: u16, sz: u8) {
        let nsp = self.c.regs[4].wrapping_sub(sz as u32);
        self.c.regs[4] = nsp;
        let la = self.c.seg_base[S_SS].wrapping_add(nsp);
        self.wr16(la, v);
    }

    #[inline(always)]
    fn pop(&mut self, sz: u8) -> u32 {
        let sp = self.c.regs[4];
        let la = self.c.seg_base[S_SS].wrapping_add(sp);
        let v = self.rd(la, sz);
        self.c.regs[4] = sp.wrapping_add(sz as u32);
        v
    }

    // -- flags --------------------------------------------------------------

    /// THE ONE DEFINITION of "restore EFLAGS from a value the guest supplied".
    /// POPFD (0x9D) and IRETD (0xCF) both go through here, so the two forms can
    /// never drift into disagreeing about which bits a guest may write: that is
    /// the shared-primitive rule applied to a four-line function, and the reason
    /// it is worth applying is that a guest's critical section is
    /// `pushfd/cli/.../popfd` while its ISR's exit is `iretd`, so a difference
    /// between them shows up as an interrupt that stays masked forever.
    ///
    /// Restorable: CF PF AF ZF SF IF DF OF. NOT TF, deliberately, because a
    /// guest that set it would single-step into a debug facility this core does
    /// not model. Not IOPL, NT, VM or any of the bits above 15, which are host
    /// state a flat DPMI client has no business changing. Bit 1 reads as 1 on a
    /// real part and is forced.
    #[inline(always)]
    fn set_flags_from_guest(&mut self, v: u32) {
        const RESTORABLE: u32 = F_CF | F_PF | F_AF | F_ZF | F_SF | F_IF | F_DF | F_OF;
        let keep = self.c.eflags & !RESTORABLE;
        self.c.eflags = (v & RESTORABLE) | keep | 2;
    }

    #[inline(always)]
    fn set_szp(&mut self, r: u32, sz: u8) {
        let m = mask_of(sz);
        let v = r & m;
        let mut f = self.c.eflags & !(F_SF | F_ZF | F_PF);
        if v == 0 {
            f |= F_ZF;
        }
        if v & msb_of(sz) != 0 {
            f |= F_SF;
        }
        if ((v as u8).count_ones() & 1) == 0 {
            f |= F_PF;
        }
        self.c.eflags = f;
    }

    fn flags_add(&mut self, a: u32, b: u32, cin: u32, sz: u8) -> u32 {
        let m = mask_of(sz);
        let a = a & m;
        let b = b & m;
        let full = (a as u64) + (b as u64) + (cin as u64);
        let r = (full as u32) & m;
        self.set_szp(r, sz);
        let mut f = self.c.eflags & !(F_CF | F_OF | F_AF);
        if full > (m as u64) {
            f |= F_CF;
        }
        if ((a ^ b ^ r) & 0x10) != 0 {
            f |= F_AF;
        }
        let msb = msb_of(sz);
        if ((a ^ r) & (b ^ r) & msb) != 0 {
            f |= F_OF;
        }
        self.c.eflags = f;
        r
    }

    fn flags_sub(&mut self, a: u32, b: u32, bin: u32, sz: u8) -> u32 {
        let m = mask_of(sz);
        let a = a & m;
        let b = b & m;
        let full = (a as u64).wrapping_sub(b as u64).wrapping_sub(bin as u64);
        let r = (full as u32) & m;
        self.set_szp(r, sz);
        let mut f = self.c.eflags & !(F_CF | F_OF | F_AF);
        if (b as u64) + (bin as u64) > (a as u64) {
            f |= F_CF;
        }
        if ((a ^ b ^ r) & 0x10) != 0 {
            f |= F_AF;
        }
        let msb = msb_of(sz);
        if ((a ^ b) & (a ^ r) & msb) != 0 {
            f |= F_OF;
        }
        self.c.eflags = f;
        r
    }

    fn flags_logic(&mut self, r: u32, sz: u8) -> u32 {
        let r = r & mask_of(sz);
        self.set_szp(r, sz);
        self.c.eflags &= !(F_CF | F_OF | F_AF);
        r
    }

    #[inline(always)]
    fn cf(&self) -> u32 {
        (self.c.eflags >> 0) & 1
    }

    fn cond(&self, cc: u8) -> bool {
        let f = self.c.eflags;
        let cfv = f & F_CF != 0;
        let zf = f & F_ZF != 0;
        let sf = f & F_SF != 0;
        let of = f & F_OF != 0;
        let pf = f & F_PF != 0;
        match cc & 0x0F {
            0x0 => of,
            0x1 => !of,
            0x2 => cfv,
            0x3 => !cfv,
            0x4 => zf,
            0x5 => !zf,
            0x6 => cfv || zf,
            0x7 => !(cfv || zf),
            0x8 => sf,
            0x9 => !sf,
            0xA => pf,
            0xB => !pf,
            0xC => sf != of,
            0xD => sf == of,
            0xE => zf || (sf != of),
            _ => !zf && (sf == of),
        }
    }
}

// ---------------------------------------------------------------------------
// ModRM
// ---------------------------------------------------------------------------
struct ModRm {
    /// reg field (also the /digit opcode extension)
    reg: usize,
    /// true when mod == 3, i.e. the r/m operand is a register
    is_reg: bool,
    /// register index when is_reg, else meaningless
    rm_reg: usize,
    /// linear address of the memory operand when !is_reg
    addr: u32,
    /// (#211) The EFFECTIVE ADDRESS, i.e. `addr` with the segment base NOT
    /// folded in. Exactly one instruction wants this and it is LEA, which
    /// computes an address WITHOUT accessing memory and must therefore produce
    /// what the guest would put in a pointer variable: an offset in its own
    /// segment, not a linear address in ours.
    ///
    /// Carried rather than subtracted at the call site, because subtracting
    /// requires knowing WHICH segment was used after a prefix override, and
    /// that knowledge already exists here and nowhere else.
    ea: u32,
}

impl<'a> Ctx<'a> {
    fn seg_for(&self, dflt: usize) -> usize {
        if self.seg_ovr >= 0 {
            self.seg_ovr as usize
        } else {
            dflt
        }
    }

    fn modrm(&mut self) -> ModRm {
        let m = self.fetch8();
        let md = m >> 6;
        let reg = ((m >> 3) & 7) as usize;
        let rm = (m & 7) as usize;
        if md == 3 {
            return ModRm { reg, is_reg: true, rm_reg: rm, addr: 0, ea: 0 };
        }
        if self.addr32 {
            let mut base_seg = S_DS;
            let mut ea: u32 = 0;
            if rm == 4 {
                // SIB
                let sib = self.fetch8();
                let scale = sib >> 6;
                let idx = ((sib >> 3) & 7) as usize;
                let bas = (sib & 7) as usize;
                if idx != 4 {
                    ea = ea.wrapping_add(self.c.regs[idx] << scale);
                }
                if bas == 5 && md == 0 {
                    ea = ea.wrapping_add(self.fetch32());
                } else {
                    ea = ea.wrapping_add(self.c.regs[bas]);
                    if bas == 4 || bas == 5 {
                        base_seg = S_SS;
                    }
                }
            } else if rm == 5 && md == 0 {
                ea = self.fetch32();
            } else {
                ea = self.c.regs[rm];
                if rm == 5 {
                    base_seg = S_SS; // EBP
                }
            }
            match md {
                1 => ea = ea.wrapping_add((self.fetch8() as i8) as i32 as u32),
                2 => ea = ea.wrapping_add(self.fetch32()),
                _ => {}
            }
            let s = self.seg_for(base_seg);
            let addr = self.c.seg_base[s].wrapping_add(ea);
            ModRm { reg, is_reg: false, rm_reg: 0, addr, ea }
        } else {
            // 16-bit addressing (0x67 in a 32-bit guest). Offsets wrap at 16 bits.
            let (mut ea, base_seg): (u32, usize) = match rm {
                0 => (self.c.regs[3].wrapping_add(self.c.regs[6]), S_DS), // BX+SI
                1 => (self.c.regs[3].wrapping_add(self.c.regs[7]), S_DS), // BX+DI
                2 => (self.c.regs[5].wrapping_add(self.c.regs[6]), S_SS), // BP+SI
                3 => (self.c.regs[5].wrapping_add(self.c.regs[7]), S_SS), // BP+DI
                4 => (self.c.regs[6], S_DS),
                5 => (self.c.regs[7], S_DS),
                6 => {
                    if md == 0 {
                        (self.fetch16() as u32, S_DS)
                    } else {
                        (self.c.regs[5], S_SS)
                    }
                }
                _ => (self.c.regs[3], S_DS),
            };
            match md {
                1 => ea = ea.wrapping_add((self.fetch8() as i8) as i32 as u32),
                2 => ea = ea.wrapping_add(self.fetch16() as u32),
                _ => {}
            }
            let s = self.seg_for(base_seg);
            let ea = ea & 0xFFFF;   // (#211) a 16-bit address size truncates
            let addr = self.c.seg_base[s].wrapping_add(ea);
            ModRm { reg, is_reg: false, rm_reg: 0, addr, ea }
        }
    }

    #[inline(always)]
    fn rm_get(&mut self, m: &ModRm, sz: u8) -> u32 {
        if m.is_reg {
            self.rget(m.rm_reg, sz)
        } else {
            self.rd(m.addr, sz)
        }
    }
    #[inline(always)]
    fn rm_set(&mut self, m: &ModRm, v: u32, sz: u8) {
        if m.is_reg {
            self.rset(m.rm_reg, v, sz)
        } else {
            self.wr(m.addr, v, sz)
        }
    }

    // -- MISS ---------------------------------------------------------------
    #[cold]
    fn miss(&mut self, op: u32, op2: u32, modrm: u32) {
        self.c.miss_count = self.c.miss_count.wrapping_add(1);
        self.c.miss_op = op;
        self.c.miss_op2 = op2;
        self.c.miss_modrm = modrm;
        // EIP is currently just past whatever we consumed while decoding; that
        // difference IS the instruction length, which is what a caller needs to
        // skip the instruction correctly. Then rewind so the caller sees EIP at
        // the instruction, never inside it.
        self.c.miss_len = self.c.eip.wrapping_sub(self.ins_eip);
        self.c.eip = self.ins_eip;
        self.c.exit_reason = X32_EXIT_MISS;
        self.faulted = true;
    }

    #[cold]
    fn ud(&mut self, op: u32, op2: u32) {
        self.c.miss_op = op;
        self.c.miss_op2 = op2;
        self.c.miss_len = self.c.eip.wrapping_sub(self.ins_eip);
        self.c.eip = self.ins_eip;
        self.c.fault_eip = self.ins_eip;
        self.c.exit_reason = X32_EXIT_FAULT_UD;
        self.faulted = true;
    }

    #[cold]
    fn div_fault(&mut self) {
        self.c.eip = self.ins_eip;
        self.c.fault_eip = self.ins_eip;
        self.c.exit_reason = X32_EXIT_FAULT_DIV;
        self.faulted = true;
    }
}

// ---------------------------------------------------------------------------
// Shifts and rotates (group 2)
// ---------------------------------------------------------------------------
impl<'a> Ctx<'a> {
    fn shift_op(&mut self, which: usize, val: u32, cnt_in: u32, sz: u8) -> u32 {
        let m = mask_of(sz);
        let bits = (sz as u32) * 8;
        let v = val & m;
        // 386+: the count is masked to 5 bits for every width.
        let cnt = cnt_in & 0x1F;
        if cnt == 0 {
            return v;
        }
        let msb = msb_of(sz);
        let mut f = self.c.eflags;
        let r: u32;
        match which {
            0 => {
                // ROL
                let c = cnt % bits;
                let rr = if c == 0 { v } else { ((v << c) | (v >> (bits - c))) & m };
                r = rr;
                f = (f & !F_CF) | (r & 1);
                if cnt == 1 {
                    f &= !F_OF;
                    if ((r & msb) != 0) != ((r & 1) != 0) {
                        f |= F_OF;
                    }
                }
                self.c.eflags = f;
                return r;
            }
            1 => {
                // ROR
                let c = cnt % bits;
                let rr = if c == 0 { v } else { ((v >> c) | (v << (bits - c))) & m };
                r = rr;
                f = (f & !F_CF) | (if r & msb != 0 { 1 } else { 0 });
                if cnt == 1 {
                    f &= !F_OF;
                    let top = (r & msb) != 0;
                    let nxt = (r & (msb >> 1)) != 0;
                    if top != nxt {
                        f |= F_OF;
                    }
                }
                self.c.eflags = f;
                return r;
            }
            2 => {
                // RCL
                let c = cnt % (bits + 1);
                let mut acc = v;
                let mut carry = self.cf();
                for _ in 0..c {
                    let nc = (acc & msb) != 0;
                    acc = ((acc << 1) & m) | carry;
                    carry = if nc { 1 } else { 0 };
                }
                r = acc;
                f = (f & !F_CF) | carry;
                if cnt == 1 {
                    f &= !F_OF;
                    if ((r & msb) != 0) != (carry != 0) {
                        f |= F_OF;
                    }
                }
                self.c.eflags = f;
                return r;
            }
            3 => {
                // RCR
                let c = cnt % (bits + 1);
                let mut acc = v;
                let mut carry = self.cf();
                for _ in 0..c {
                    let nc = acc & 1;
                    acc = (acc >> 1) | (carry << (bits - 1));
                    carry = nc;
                }
                r = acc & m;
                let old_msb_pair = ((r & msb) != 0) != ((r & (msb >> 1)) != 0);
                f = (f & !F_CF) | carry;
                if cnt == 1 {
                    f &= !F_OF;
                    if old_msb_pair {
                        f |= F_OF;
                    }
                }
                self.c.eflags = f;
                return r;
            }
            4 | 6 => {
                // SHL / SAL
                let cf_out = if cnt <= bits { (v >> (bits - cnt)) & 1 } else { 0 };
                r = if cnt >= bits { 0 } else { (v << cnt) & m };
                self.set_szp(r, sz);
                f = self.c.eflags;
                f = (f & !(F_CF | F_OF)) | cf_out;
                if cnt == 1 && (((r & msb) != 0) != (cf_out != 0)) {
                    f |= F_OF;
                }
                self.c.eflags = f;
                return r;
            }
            5 => {
                // SHR
                let cf_out = if cnt <= bits { (v >> (cnt - 1)) & 1 } else { 0 };
                r = if cnt >= bits { 0 } else { v >> cnt };
                self.set_szp(r, sz);
                f = self.c.eflags;
                f = (f & !(F_CF | F_OF)) | cf_out;
                if cnt == 1 && (v & msb) != 0 {
                    f |= F_OF;
                }
                self.c.eflags = f;
                return r;
            }
            _ => {
                // SAR. `sv` is the operand sign-extended to 64 bits, so an
                // arithmetic shift by any count >= the operand width already
                // yields all sign bits: no per-width clamp is needed, and the
                // CF bit (the last bit shifted out) is uniformly bit cnt-1 of
                // that sign-extended value. cnt is 1..31 here (0 returned early).
                let sv = sext(v, sz);
                let cf_out = ((sv >> ((cnt - 1) as i64)) & 1) as u32;
                r = ((sv >> (cnt as i64)) as u32) & m;
                self.set_szp(r, sz);
                f = self.c.eflags;
                f = (f & !(F_CF | F_OF)) | (cf_out & 1);
                self.c.eflags = f;
                return r;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// String operations. One iteration per dispatch: a REP-prefixed instruction
// leaves EIP AT the prefix while ECX != 0, so insn_count advances once per
// ITERATION. That is deliberate. blame.md #740 records that x86_16.c counts a
// whole `rep movsw` as ONE instruction, which makes every insn/sec figure taken
// from a REP-heavy program wrong by orders of magnitude, and it also makes the
// guest uninterruptible for the length of the repeat.
// ---------------------------------------------------------------------------
impl<'a> Ctx<'a> {
    #[inline(always)]
    fn str_delta(&self, sz: u8) -> u32 {
        if self.c.eflags & F_DF != 0 {
            (sz as u32).wrapping_neg()
        } else {
            sz as u32
        }
    }
    #[inline(always)]
    fn adv_si(&mut self, d: u32) {
        if self.addr32 {
            self.c.regs[6] = self.c.regs[6].wrapping_add(d);
        } else {
            let v = (self.c.regs[6] as u16).wrapping_add(d as u16);
            self.c.regs[6] = (self.c.regs[6] & 0xFFFF_0000) | v as u32;
        }
    }
    #[inline(always)]
    fn adv_di(&mut self, d: u32) {
        if self.addr32 {
            self.c.regs[7] = self.c.regs[7].wrapping_add(d);
        } else {
            let v = (self.c.regs[7] as u16).wrapping_add(d as u16);
            self.c.regs[7] = (self.c.regs[7] & 0xFFFF_0000) | v as u32;
        }
    }
    #[inline(always)]
    fn si_addr(&mut self) -> u32 {
        let s = self.seg_for(S_DS);
        let off = if self.addr32 { self.c.regs[6] } else { self.c.regs[6] & 0xFFFF };
        self.c.seg_base[s].wrapping_add(off)
    }
    #[inline(always)]
    fn di_addr(&mut self) -> u32 {
        // ES is NOT overridable for the destination of a string op.
        let off = if self.addr32 { self.c.regs[7] } else { self.c.regs[7] & 0xFFFF };
        self.c.seg_base[S_ES].wrapping_add(off)
    }
    #[inline(always)]
    fn cx_get(&self) -> u32 {
        if self.addr32 { self.c.regs[1] } else { self.c.regs[1] & 0xFFFF }
    }
    #[inline(always)]
    fn cx_dec(&mut self) {
        if self.addr32 {
            self.c.regs[1] = self.c.regs[1].wrapping_sub(1);
        } else {
            let v = (self.c.regs[1] as u16).wrapping_sub(1);
            self.c.regs[1] = (self.c.regs[1] & 0xFFFF_0000) | v as u32;
        }
    }
}

// ---------------------------------------------------------------------------
// The dispatch loop
// ---------------------------------------------------------------------------

/// Run up to `max_insns` guest instructions. Returns an X32_EXIT_* code, also
/// left in `cpu.exit_reason`.
///
/// # Safety
/// `cpu` must be a valid, initialised `X8632Cpu`, and `cpu.mem` must point to at
/// least `cpu.mem_size` writable bytes. Everything the GUEST controls is checked
/// here; those two facts are the host's contract.
#[no_mangle]
pub unsafe extern "C" fn x86_32_run(cpu: *mut X8632Cpu, max_insns: u64) -> u32 {
    if cpu.is_null() {
        return X32_EXIT_FAULT_MEM;
    }
    let c = &mut *cpu;
    if c.mem.is_null() || c.mem_size == 0 {
        c.exit_reason = X32_EXIT_FAULT_MEM;
        return X32_EXIT_FAULT_MEM;
    }
    let mem = core::slice::from_raw_parts_mut(c.mem, c.mem_size as usize);
    let mut ctx = Ctx {
        c,
        mem,
        faulted: false,
        opsize32: true,
        addr32: true,
        seg_ovr: -1,
        rep: 0,
        lock: false,
        ins_eip: 0,
    };
    ctx.c.exit_reason = X32_EXIT_BUDGET;
    let mut n: u64 = 0;
    while n < max_insns {
        if ctx.c.stop_eip_en != 0 && ctx.c.eip == ctx.c.stop_eip {
            ctx.c.exit_reason = X32_EXIT_STOP_EIP;
            return X32_EXIT_STOP_EIP;
        }
        ctx.ins_eip = ctx.c.eip;
        ctx.opsize32 = true;
        ctx.addr32 = true;
        ctx.seg_ovr = -1;
        ctx.rep = 0;
        ctx.lock = false;
        step(&mut ctx);
        // (#rafault) THE BRANCH TRACE, at the ONE site that sees every control
        // transfer: an instruction that retired without moving EIP into its own
        // tail took a branch, whatever arm moved it. See the ring at the foot of
        // this file. Off in the golden; one static load when it is.
        if BT_ON != 0 && !ctx.faulted {
            let ne = ctx.c.eip;
            if ne < ctx.ins_eip || ne > ctx.ins_eip.wrapping_add(15) {
                bt_record(&ctx, ctx.ins_eip, ne);
            }
        }
        n += 1;
        // An instruction that MISSed or faulted did NOT retire: EIP was rewound
        // to point at it, and counting it would make insn_count disagree with
        // the guest's own view of how far it got. INT/HLT/IN/OUT DID retire.
        let r = ctx.c.exit_reason;
        let retired = !(ctx.faulted
            && (r == X32_EXIT_MISS
                || r == X32_EXIT_FAULT_UD
                || r == X32_EXIT_FAULT_MEM
                || r == X32_EXIT_FAULT_DIV
                || r == X32_EXIT_FAULT_LIMIT));
        if retired {
            ctx.c.insn_count = ctx.c.insn_count.wrapping_add(1);
        }
        if ctx.faulted {
            return ctx.c.exit_reason;
        }
    }
    ctx.c.exit_reason = X32_EXIT_BUDGET;
    X32_EXIT_BUDGET
}

fn step(x: &mut Ctx) {
    // ---- prefixes ----
    let mut op;
    loop {
        op = x.fetch8();
        match op {
            0x66 => x.opsize32 = false,
            0x67 => x.addr32 = false,
            0x2E => x.seg_ovr = S_CS as i32,
            0x36 => x.seg_ovr = S_SS as i32,
            0x3E => x.seg_ovr = S_DS as i32,
            0x26 => x.seg_ovr = S_ES as i32,
            0x64 => x.seg_ovr = S_FS as i32,
            0x65 => x.seg_ovr = S_GS as i32,
            0xF0 => x.lock = true,
            0xF2 => x.rep = 0xF2,
            0xF3 => x.rep = 0xF3,
            _ => break,
        }
        if x.faulted {
            return;
        }
    }
    // A fetch that walked off the arena leaves op == 0, which would otherwise
    // execute as ADD and corrupt guest state on the way out.
    if x.faulted {
        return;
    }
    let osz: u8 = if x.opsize32 { 4 } else { 2 };

    match op {
        // ---- 00..3F: the eight ALU groups, four forms each ----
        0x00..=0x05 | 0x08..=0x0D | 0x10..=0x15 | 0x18..=0x1D | 0x20..=0x25 | 0x28..=0x2D
        | 0x30..=0x35 | 0x38..=0x3D => {
            let grp = (op >> 3) & 7;
            let form = op & 7;
            match form {
                0 | 1 | 2 | 3 => {
                    let sz = if form & 1 == 0 { 1 } else { osz };
                    let m = x.modrm();
                    if x.faulted {
                        return;
                    }
                    let to_rm = form < 2;
                    let a = if to_rm { x.rm_get(&m, sz) } else { x.rget(m.reg, sz) };
                    let b = if to_rm { x.rget(m.reg, sz) } else { x.rm_get(&m, sz) };
                    let r = alu(x, grp, a, b, sz);
                    if grp != 7 {
                        // CMP writes nothing
                        if to_rm {
                            x.rm_set(&m, r, sz);
                        } else {
                            x.rset(m.reg, r, sz);
                        }
                    }
                }
                _ => {
                    // 04/05: AL/eAX, imm
                    let sz = if form == 4 { 1 } else { osz };
                    let imm = x.fetch_imm(sz);
                    let a = x.rget(0, sz);
                    let r = alu(x, grp, a, imm, sz);
                    if grp != 7 {
                        x.rset(0, r, sz);
                    }
                }
            }
        }

        // ---- segment push/pop. Flat guest: the selector value is carried. ----
        0x06 => { let v = x.c.seg[S_ES]; x.push_seg(v, osz); }
        0x07 => { let v = x.pop(osz); x.load_seg(S_ES, v as u16); }
        0x0E => { let v = x.c.seg[S_CS]; x.push_seg(v, osz); }
        0x16 => { let v = x.c.seg[S_SS]; x.push_seg(v, osz); }
        0x17 => { let v = x.pop(osz); x.load_seg(S_SS, v as u16); }
        0x1E => { let v = x.c.seg[S_DS]; x.push_seg(v, osz); }
        0x1F => { let v = x.pop(osz); x.load_seg(S_DS, v as u16); }

        // ---- 27/2F/37/3F: DAA/DAS/AAA/AAS. Valid in 32-bit mode, rare, but a
        //      silent skip would corrupt AL, so they are implemented. ----
        0x27 => daa(x),
        0x2F => das(x),
        0x37 => aaa(x),
        0x3F => aas(x),

        // ---- INC/DEC r32. CF is preserved; that is the whole point of them. --
        0x40..=0x47 => {
            let i = (op & 7) as usize;
            let a = x.rget(i, osz);
            let cf = x.c.eflags & F_CF;
            let r = x.flags_add(a, 1, 0, osz);
            x.c.eflags = (x.c.eflags & !F_CF) | cf;
            x.rset(i, r, osz);
        }
        0x48..=0x4F => {
            let i = (op & 7) as usize;
            let a = x.rget(i, osz);
            let cf = x.c.eflags & F_CF;
            let r = x.flags_sub(a, 1, 0, osz);
            x.c.eflags = (x.c.eflags & !F_CF) | cf;
            x.rset(i, r, osz);
        }

        0x50..=0x57 => { let i = (op & 7) as usize; let v = x.rget(i, osz); x.push(v, osz); }
        0x58..=0x5F => { let i = (op & 7) as usize; let v = x.pop(osz); x.rset(i, v, osz); }

        0x60 => {
            // PUSHA/PUSHAD
            let osp = x.c.regs[4];
            for i in 0..8 {
                let v = if i == 4 { osp } else { x.c.regs[i] };
                x.push(v, osz);
            }
        }
        0x61 => {
            // POPA/POPAD: ESP's slot is discarded.
            for i in (0..8).rev() {
                let v = x.pop(osz);
                if i != 4 {
                    x.rset(i, v, osz);
                }
            }
        }
        0x62 => { let _m = x.modrm(); if x.faulted { return; } x.miss(op as u32, 0x100, 0x100); } // BOUND
        0x63 => { let _m = x.modrm(); if x.faulted { return; } x.miss(op as u32, 0x100, 0x100); } // ARPL

        0x68 => { let v = x.fetch_imm(osz); x.push(v, osz); }
        0x6A => { let v = (x.fetch8() as i8) as i32 as u32; x.push(v, osz); }
        0x69 | 0x6B => {
            let m = x.modrm();
            if x.faulted { return; }
            let a = x.rm_get(&m, osz);
            let imm = if op == 0x69 {
                x.fetch_imm(osz)
            } else {
                (x.fetch8() as i8) as i32 as u32
            };
            let r = imul_flags(x, a, imm, osz);
            x.rset(m.reg, r, osz);
        }

        0x6E | 0x6F => {
            // (#740 digsel) OUTSB / OUTSW / OUTSD: port DX <- DS:[eSI].
            //
            // The comment this replaces said "No DOS/4GW target uses them".
            // MEASURED false, The Dig (DIG.EXE) at EIP 0x0014E49E, reached only
            // once its command line stopped being the interrupt vector table
            // and it got as far as setting a video mode:
            //     ba c8 03 00 00   mov edx,0x3c8
            //     ee               out dx,al        (DAC write index)
            //     ba c9 03 00 00   mov edx,0x3c9
            //     f3 6e            rep outsb        with ECX = 0x300
            // 768 bytes to port 0x3C9 is one whole 256-colour VGA palette, and
            // it is how a great many DOS titles load one. The guest stopped
            // dead on it, which is exactly where a MISS is supposed to leave
            // you: located, and one step from running.
            string_io_out(x, op, osz);
        }
        0x6C | 0x6D => {
            // INS: still a MISS, and this is the reason rather than an
            // oversight. The value an IN produces arrives from the host AFTER
            // the exit, in EAX; the string form has to store it to ES:[eDI],
            // so the core would have to remember across the exit that a store
            // is outstanding and where it goes. That is a small piece of
            // protocol between core and host, and it deserves its own change
            // with its own test rather than a rider on this one. Until then a
            // guest that executes INS stops with `opcode 6c`, which is a
            // located failure rather than a wrong answer it cannot detect.
            x.miss(op as u32, 0x100, 0x100);
        }

        0x70..=0x7F => {
            let d = (x.fetch8() as i8) as i32;
            if x.cond(op & 0x0F) {
                x.c.eip = (x.c.eip as i32).wrapping_add(d) as u32;
            }
        }

        0x80 | 0x81 | 0x83 => {
            let sz = if op == 0x80 { 1 } else { osz };
            let m = x.modrm();
            if x.faulted { return; }
            let imm = if op == 0x83 {
                (x.fetch8() as i8) as i32 as u32
            } else {
                x.fetch_imm(sz)
            };
            let a = x.rm_get(&m, sz);
            let grp = (m.reg & 7) as u8;
            let r = alu(x, grp, a, imm, sz);
            if grp != 7 {
                x.rm_set(&m, r, sz);
            }
        }

        0x84 | 0x85 => {
            let sz = if op == 0x84 { 1 } else { osz };
            let m = x.modrm();
            if x.faulted { return; }
            let a = x.rm_get(&m, sz);
            let b = x.rget(m.reg, sz);
            x.flags_logic(a & b, sz);
        }
        0x86 | 0x87 => {
            let sz = if op == 0x86 { 1 } else { osz };
            let m = x.modrm();
            if x.faulted { return; }
            let a = x.rm_get(&m, sz);
            let b = x.rget(m.reg, sz);
            x.rm_set(&m, b, sz);
            x.rset(m.reg, a, sz);
        }
        0x88 | 0x89 => {
            let sz = if op == 0x88 { 1 } else { osz };
            let m = x.modrm();
            if x.faulted { return; }
            let v = x.rget(m.reg, sz);
            x.rm_set(&m, v, sz);
        }
        0x8A | 0x8B => {
            let sz = if op == 0x8A { 1 } else { osz };
            let m = x.modrm();
            if x.faulted { return; }
            let v = x.rm_get(&m, sz);
            x.rset(m.reg, v, sz);
        }
        0x8C => {
            let m = x.modrm();
            if x.faulted { return; }
            let v = x.c.seg[m.reg & 7] as u32;
            // Register destination writes the full operand size, zero-extended.
            x.rm_set(&m, v, if m.is_reg { osz } else { 2 });
        }
        0x8D => {
            // LEA. No memory access, and a register form is UD.
            let m = x.modrm();
            if x.faulted { return; }
            if m.is_reg {
                x.ud(op as u32, 0x100);
                return;
            }
            // (#211) THE OFFSET, NOT THE LINEAR ADDRESS. The paragraph that
            // used to be here predicted this failure and shipped it anyway:
            // "for a flat guest every base is 0 so this IS the effective
            // address; when a DPMI host hands out a non-flat descriptor this
            // becomes wrong". A DJGPP client is that descriptor. Its
            // __get_dos_version does `lea edi,[ebp-0x58]` to point at a
            // register block it then passes to DPMI 0300h; with the base
            // folded in, the pointer was a megabyte past the data and the host
            // simulated INT 21h from an all-zero register file.
            x.rset(m.reg, m.ea, osz);
        }
        0x8E => {
            let m = x.modrm();
            if x.faulted { return; }
            let v = x.rm_get(&m, 2);
            x.load_seg(m.reg & 7, v as u16);
        }
        0x8F => {
            let m = x.modrm();
            if x.faulted { return; }
            let v = x.pop(osz);
            x.rm_set(&m, v, osz);
        }

        0x90 => { /* NOP (XCHG eAX,eAX) */ }
        0x91..=0x97 => {
            let i = (op & 7) as usize;
            let a = x.rget(0, osz);
            let b = x.rget(i, osz);
            x.rset(0, b, osz);
            x.rset(i, a, osz);
        }
        0x98 => {
            if osz == 4 {
                let v = (x.c.regs[0] as u16) as i16 as i32 as u32;
                x.c.regs[0] = v;
            } else {
                let v = (x.c.regs[0] as u8) as i8 as i16 as u16 as u32;
                x.c.regs[0] = (x.c.regs[0] & 0xFFFF_0000) | v;
            }
        }
        0x99 => {
            if osz == 4 {
                x.c.regs[2] = if x.c.regs[0] & 0x8000_0000 != 0 { 0xFFFF_FFFF } else { 0 };
            } else {
                let hi = if x.c.regs[0] & 0x8000 != 0 { 0xFFFFu32 } else { 0 };
                x.c.regs[2] = (x.c.regs[2] & 0xFFFF_0000) | hi;
            }
        }
        0x9A => {
            // CALL FAR ptr16:32 (immediate far pointer).
            let off = x.fetch_imm(osz);
            let sel = x.fetch16();
            if x.faulted { return; }
            let cs = x.c.seg[S_CS] as u32;
            let ret = x.c.eip;
            x.push(cs, osz);
            x.push(ret, osz);
            if x.faulted { return; }
            x.load_seg(S_CS, sel);   // (#211) the base moves with the selector
            x.c.eip = off;
        }
        0x9B => { /* WAIT/FWAIT: no x87 exceptions are modelled */ }
        0x9C => { let f = x.c.eflags & 0x00FC_FFFF; x.push(f, osz); }
        0x9D => {
            // POPFD. Restores CF PF AF ZF SF DF OF and IF; TF is deliberately
            // NOT restorable, because a guest that sets it would single-step
            // into a debug facility this core does not model. IF matters: the
            // guest's own critical sections are pushfd/cli/.../popfd, and it is
            // the bit DPMI 0900h/0901h virtualise. Note the host-native oracle
            // cannot see IF change (ring 3, IOPL 0), which is why IF is not in
            // EFLAGS_ARITH_MASK.
            let v = x.pop(osz);
            x.set_flags_from_guest(v);
        }
        0x9E => {
            let ah = (x.c.regs[0] >> 8) & 0xFF;
            x.c.eflags = (x.c.eflags & !0xFF) | (ah & 0xD5) | 2;
        }
        0x9F => {
            let f = (x.c.eflags & 0xD5) | 2;
            x.c.regs[0] = (x.c.regs[0] & 0xFFFF_00FF) | (f << 8);
        }

        0xA0 | 0xA1 | 0xA2 | 0xA3 => {
            let sz = if op & 1 == 0 { 1 } else { osz };
            let off = if x.addr32 { x.fetch32() } else { x.fetch16() as u32 };
            let s = x.seg_for(S_DS);
            let la = x.c.seg_base[s].wrapping_add(off);
            if op < 0xA2 {
                let v = x.rd(la, sz);
                x.rset(0, v, sz);
            } else {
                let v = x.rget(0, sz);
                x.wr(la, v, sz);
            }
        }

        0xA4 | 0xA5 | 0xA6 | 0xA7 | 0xAA | 0xAB | 0xAC | 0xAD | 0xAE | 0xAF => {
            string_op(x, op, osz);
        }

        0xA8 | 0xA9 => {
            let sz = if op == 0xA8 { 1 } else { osz };
            let imm = x.fetch_imm(sz);
            let a = x.rget(0, sz);
            x.flags_logic(a & imm, sz);
        }

        0xB0..=0xB7 => { let i = (op & 7) as usize; let v = x.fetch8(); x.set_r8(i, v); }
        0xB8..=0xBF => { let i = (op & 7) as usize; let v = x.fetch_imm(osz); x.rset(i, v, osz); }

        0xC0 | 0xC1 => {
            let sz = if op == 0xC0 { 1 } else { osz };
            let m = x.modrm();
            if x.faulted { return; }
            let cnt = x.fetch8() as u32;
            let a = x.rm_get(&m, sz);
            let r = x.shift_op(m.reg & 7, a, cnt, sz);
            if (cnt & 0x1F) != 0 {
                x.rm_set(&m, r, sz);
            }
        }
        0xC2 => { let n = x.fetch16() as u32; let r = x.pop(osz); x.c.eip = r; x.c.regs[4] = x.c.regs[4].wrapping_add(n); }
        0xC3 => { let r = x.pop(osz); x.c.eip = r; }
        0xC4 => { far_ptr_load(x, S_ES, osz); } // LES
        0xC5 => { far_ptr_load(x, S_DS, osz); } // LDS
        0xC6 | 0xC7 => {
            let sz = if op == 0xC6 { 1 } else { osz };
            let m = x.modrm();
            if x.faulted { return; }
            let imm = x.fetch_imm(sz);
            x.rm_set(&m, imm, sz);
        }
        0xC8 => {
            // ENTER imm16, imm8
            let alloc = x.fetch16() as u32;
            let level = (x.fetch8() & 0x1F) as u32;
            let ebp = x.c.regs[5];
            x.push(ebp, osz);
            let frame = x.c.regs[4];
            if level > 0 {
                let mut bp = ebp;
                for _ in 1..level {
                    bp = bp.wrapping_sub(osz as u32);
                    let la = x.c.seg_base[S_SS].wrapping_add(bp);
                    let v = x.rd(la, osz);
                    x.push(v, osz);
                }
                x.push(frame, osz);
            }
            x.rset(5, frame, osz);
            x.c.regs[4] = x.c.regs[4].wrapping_sub(alloc);
        }
        0xC9 => {
            // LEAVE
            x.c.regs[4] = x.c.regs[5];
            let v = x.pop(osz);
            x.rset(5, v, osz);
        }
        0xCA | 0xCB => {
            // RETF / RETF imm16, the return half of CALL FAR. Same
            // read-then-commit discipline as IRETD: nothing moves until both
            // slots have been read successfully.
            let n = if op == 0xCA { x.fetch16() as u32 } else { 0 };
            let w = osz as u32;
            let sp = x.c.regs[4];
            let sb = x.c.seg_base[S_SS];
            let neip = x.rd(sb.wrapping_add(sp), osz);
            let ncs = x.rd(sb.wrapping_add(sp.wrapping_add(w)), osz);
            if x.faulted { return; }
            x.c.regs[4] = sp.wrapping_add(w.wrapping_mul(2)).wrapping_add(n);
            x.load_seg(S_CS, ncs as u16);   // (#211) the base moves with the selector
            x.c.eip = neip;
        }
        0xCC => { x.c.exit_reason = X32_EXIT_INT; x.c.exit_arg = 3; x.faulted = true; }
        0xCD => {
            let v = x.fetch8() as u32;
            x.c.exit_reason = X32_EXIT_INT;
            x.c.exit_arg = v;
            x.faulted = true;
        }
        0xCE => {
            // INTO
            if x.c.eflags & F_OF != 0 {
                x.c.exit_reason = X32_EXIT_INT;
                x.c.exit_arg = 4;
                x.faulted = true;
            }
        }
        0xCF => {
            // IRETD (IRET with a 0x66 prefix), the SAME-PRIVILEGE, non-nested,
            // non-V86 form and only that form: EIP, CS, EFLAGS popped in that
            // order. It is here because it is the OTHER HALF of an interrupt.
            // The core already exits on an INT the guest EXECUTES; delivering
            // one the guest RECEIVES (x86_32_inject_int below) is useless
            // without a return path, and a delivered interrupt whose handler
            // ends in a MISS is a worse hang than no interrupt at all.
            //
            // NOTHING IS MUTATED UNTIL EVERY CHECK HAS PASSED. The frame is
            // READ at ESP+0/+4/+8 rather than popped, so a refusal leaves ESP
            // and EIP exactly as they were and the MISS contract ("EIP is ON
            // the instruction, no effect invented") holds for the refused
            // forms too. Popping first and then refusing would hand the caller
            // a stack that had already moved, which is the stack-desync class
            // blame.md records from the Win16 interpreter.
            if x.c.eflags & (F_NT | F_VM) != 0 {
                // NT set: this IRET is a task switch back through a TSS link.
                // VM set: a mode change. Neither is modelled, and neither is
                // reachable from anything this core does; guessing is not on
                // the table, so both MISS.
                x.miss(op as u32, 0x100, 0x100);
                return;
            }
            let w = osz as u32;
            let sp = x.c.regs[4];
            let sb = x.c.seg_base[S_SS];
            let neip = x.rd(sb.wrapping_add(sp), osz);
            let ncs = x.rd(sb.wrapping_add(sp.wrapping_add(w)), osz);
            let nfl = x.rd(sb.wrapping_add(sp.wrapping_add(w.wrapping_mul(2))), osz);
            if x.faulted {
                return;
            }
            if (nfl & F_VM) != 0 || ((ncs as u16) & 3) != (x.c.seg[S_CS] & 3) {
                // A return to a DIFFERENT privilege level also pops SS:ESP, and
                // a return into V86 mode pops four more selectors. Both are
                // real IRET forms; neither can occur under the flat, single-ring
                // DPMI model this core implements, so they MISS rather than
                // being emulated as the same-privilege form, which would leave
                // ESP 8 bytes low and derail the guest much later.
                x.miss(op as u32, 0x100, 0x100);
                return;
            }
            x.c.regs[4] = sp.wrapping_add(w.wrapping_mul(3));
            x.load_seg(S_CS, ncs as u16);   // (#211) the base moves with the selector
            // 16-bit operand size clears the top half of EIP, exactly as RET
            // does above; x.rd() already returns a value narrowed to `osz`.
            x.c.eip = neip;
            x.set_flags_from_guest(nfl);
        }

        0xD0..=0xD3 => {
            let sz = if op & 1 == 0 { 1 } else { osz };
            let m = x.modrm();
            if x.faulted { return; }
            let cnt = if op < 0xD2 { 1u32 } else { x.c.regs[1] & 0xFF };
            let a = x.rm_get(&m, sz);
            let r = x.shift_op(m.reg & 7, a, cnt, sz);
            if (cnt & 0x1F) != 0 {
                x.rm_set(&m, r, sz);
            }
        }
        0xD4 => {
            // AAM imm8. CF, OF and AF are architecturally UNDEFINED; they are
            // CLEARED here because that is what the silicon the oracle runs on
            // does, measured, and matching it keeps those bits under comparison
            // instead of masked out. Stated as a match to one part, not as a
            // universal truth.
            let b = x.fetch8() as u32;
            if b == 0 { x.div_fault(); return; }
            let al = x.c.regs[0] & 0xFF;
            let ah = al / b;
            let nal = al % b;
            x.c.regs[0] = (x.c.regs[0] & 0xFFFF_0000) | (ah << 8) | nal;
            x.set_szp(nal, 1);
            x.c.eflags &= !(F_CF | F_OF | F_AF);
        }
        0xD5 => {
            // AAD imm8. Same undefined-flag note as AAM above.
            let b = x.fetch8() as u32;
            let al = x.c.regs[0] & 0xFF;
            let ah = (x.c.regs[0] >> 8) & 0xFF;
            let nal = (al.wrapping_add(ah.wrapping_mul(b))) & 0xFF;
            x.c.regs[0] = (x.c.regs[0] & 0xFFFF_0000) | nal;
            x.set_szp(nal, 1);
            x.c.eflags &= !(F_CF | F_OF | F_AF);
        }
        0xD6 => { x.ud(op as u32, 0x100); } // SALC, undocumented
        0xD7 => {
            // XLAT
            let s = x.seg_for(S_DS);
            let al = x.c.regs[0] & 0xFF;
            let base = if x.addr32 { x.c.regs[3] } else { x.c.regs[3] & 0xFFFF };
            let la = x.c.seg_base[s].wrapping_add(base.wrapping_add(al));
            let v = x.rd8(la) as u32;
            x.c.regs[0] = (x.c.regs[0] & 0xFFFF_FF00) | v;
        }

        0xD8..=0xDF => {
            // (#211) x87. THE SEMANTICS ARE IN rustkern/x87.rs AND ONLY THERE;
            // this arm decodes the operand and moves the bytes. An encoding
            // that unit does not implement still MISSes ON the instruction and
            // never becomes a silent no-op, because several of these forms
            // carry a push or a pop and skipping one desynchronises the guest's
            // FP stack depth so the eventual crash is nowhere near its cause
            // (blame.md, 2026-08-07).
            let m = x.modrm();
            if x.faulted { return; }
            let modrm_byte = if m.is_reg { 0xC0 | ((m.reg as u32) << 3) | m.rm_reg as u32 } else { (m.reg as u32) << 3 };
            let handled = if m.is_reg {
                crate::x87::reg_form(x.c, op, modrm_byte as u8)
            } else {
                let mut fm = crate::x87::X87Mem::ZERO;
                match crate::x87::mem_dir(op, m.reg) {
                    crate::x87::X87MemDir::In(n) => {
                        x87_mem_read(x, m.addr, n, &mut fm);
                        if x.faulted { return; }
                        crate::x87::mem_form(x.c, op, m.reg, &mut fm)
                    }
                    crate::x87::X87MemDir::Out(n) => {
                        let ok = crate::x87::mem_form(x.c, op, m.reg, &mut fm);
                        if ok { x87_mem_write(x, m.addr, n, &fm); }
                        ok
                    }
                    crate::x87::X87MemDir::Unhandled => false,
                }
            };
            if !handled { x.miss(op as u32, 0x100, modrm_byte); }
        }

        0xE0 | 0xE1 | 0xE2 => {
            let d = (x.fetch8() as i8) as i32;
            x.cx_dec();
            let cx = x.cx_get();
            let zf = x.c.eflags & F_ZF != 0;
            let take = match op {
                0xE0 => cx != 0 && !zf,
                0xE1 => cx != 0 && zf,
                _ => cx != 0,
            };
            if take {
                x.c.eip = (x.c.eip as i32).wrapping_add(d) as u32;
            }
        }
        0xE3 => {
            let d = (x.fetch8() as i8) as i32;
            if x.cx_get() == 0 {
                x.c.eip = (x.c.eip as i32).wrapping_add(d) as u32;
            }
        }

        0xE4 | 0xE5 | 0xEC | 0xED => {
            let sz = if op & 1 == 0 { 1 } else { osz };
            let port = if op < 0xE8 { x.fetch8() as u32 } else { x.c.regs[2] & 0xFFFF };
            x.c.exit_reason = X32_EXIT_IO_IN;
            x.c.exit_arg = port;
            x.c.io_size = sz as u32;
            x.faulted = true;
        }
        0xE6 | 0xE7 | 0xEE | 0xEF => {
            let sz = if op & 1 == 0 { 1 } else { osz };
            let port = if op < 0xE8 { x.fetch8() as u32 } else { x.c.regs[2] & 0xFFFF };
            x.c.exit_reason = X32_EXIT_IO_OUT;
            x.c.exit_arg = port;
            x.c.io_size = sz as u32;
            x.c.io_val = x.rget(0, sz);
            x.faulted = true;
        }

        0xE8 => {
            let d = if osz == 4 { x.fetch32() as i32 } else { (x.fetch16() as i16) as i32 };
            let ret = x.c.eip;
            x.push(ret, osz);
            x.c.eip = (x.c.eip as i32).wrapping_add(d) as u32;
        }
        0xE9 => {
            let d = if osz == 4 { x.fetch32() as i32 } else { (x.fetch16() as i16) as i32 };
            x.c.eip = (x.c.eip as i32).wrapping_add(d) as u32;
        }
        0xEA => {
            // JMP FAR ptr16:32 (immediate far pointer).
            let off = x.fetch_imm(osz);
            let sel = x.fetch16();
            if x.faulted { return; }
            x.load_seg(S_CS, sel);   // (#211) the base moves with the selector
            x.c.eip = off;
        }
        0xEB => {
            let d = (x.fetch8() as i8) as i32;
            x.c.eip = (x.c.eip as i32).wrapping_add(d) as u32;
        }

        0xF4 => { x.c.exit_reason = X32_EXIT_HLT; x.faulted = true; }
        0xF5 => { x.c.eflags ^= F_CF; }
        0xF6 | 0xF7 => { grp3(x, op, osz); }
        0xF8 => { x.c.eflags &= !F_CF; }
        0xF9 => { x.c.eflags |= F_CF; }
        0xFA => { x.c.eflags &= !F_IF; }
        0xFB => { x.c.eflags |= F_IF; }
        0xFC => { x.c.eflags &= !F_DF; }
        0xFD => { x.c.eflags |= F_DF; }
        0xFE => {
            let m = x.modrm();
            if x.faulted { return; }
            let a = x.rm_get(&m, 1);
            let cf = x.c.eflags & F_CF;
            let r = if m.reg & 7 == 0 { x.flags_add(a, 1, 0, 1) } else { x.flags_sub(a, 1, 0, 1) };
            x.c.eflags = (x.c.eflags & !F_CF) | cf;
            x.rm_set(&m, r, 1);
        }
        0xFF => { grp5(x, osz); }

        0x0F => { two_byte(x, osz); }

        _ => { x.ud(op as u32, 0x100); }
    }
}

fn alu(x: &mut Ctx, grp: u8, a: u32, b: u32, sz: u8) -> u32 {
    match grp {
        0 => x.flags_add(a, b, 0, sz),
        1 => x.flags_logic(a | b, sz),
        2 => { let c = x.cf(); x.flags_add(a, b, c, sz) }
        3 => { let c = x.cf(); x.flags_sub(a, b, c, sz) }
        4 => x.flags_logic(a & b, sz),
        5 => x.flags_sub(a, b, 0, sz),
        6 => x.flags_logic(a ^ b, sz),
        _ => x.flags_sub(a, b, 0, sz), // CMP: caller discards
    }
}

fn imul_flags(x: &mut Ctx, a: u32, b: u32, sz: u8) -> u32 {
    let m = mask_of(sz);
    let full = sext(a, sz).wrapping_mul(sext(b, sz));
    let r = (full as u64 as u32) & m;
    let ovf = sext(r, sz) != full;
    // SF/ZF/PF/AF are architecturally UNDEFINED for IMUL. They are set from the
    // low result here because that is what real parts do, but every IMUL oracle
    // vector narrows its flag mask to CF|OF so a future divergence in an
    // undefined bit cannot be mistaken for a real defect in either direction.
    x.set_szp(r, sz);
    let mut f = x.c.eflags & !(F_CF | F_OF);
    if ovf {
        f |= F_CF | F_OF;
    }
    x.c.eflags = f;
    r
}

fn grp3(x: &mut Ctx, op: u8, osz: u8) {
    let sz = if op == 0xF6 { 1 } else { osz };
    let m = x.modrm();
    if x.faulted { return; }
    let sub = m.reg & 7;
    match sub {
        0 | 1 => {
            let imm = x.fetch_imm(sz);
            let a = x.rm_get(&m, sz);
            x.flags_logic(a & imm, sz);
        }
        2 => { let a = x.rm_get(&m, sz); x.rm_set(&m, !a, sz); }
        3 => {
            let a = x.rm_get(&m, sz);
            let r = x.flags_sub(0, a, 0, sz);
            x.rm_set(&m, r, sz);
        }
        4 => {
            // MUL
            let a = x.rm_get(&m, sz);
            match sz {
                1 => {
                    let r = (x.c.regs[0] & 0xFF) * (a & 0xFF);
                    x.c.regs[0] = (x.c.regs[0] & 0xFFFF_0000) | (r & 0xFFFF);
                    set_muldiv_flags(x, (r >> 8) != 0);
                }
                2 => {
                    let r = (x.c.regs[0] & 0xFFFF) * (a & 0xFFFF);
                    x.c.regs[0] = (x.c.regs[0] & 0xFFFF_0000) | (r & 0xFFFF);
                    x.c.regs[2] = (x.c.regs[2] & 0xFFFF_0000) | ((r >> 16) & 0xFFFF);
                    set_muldiv_flags(x, (r >> 16) != 0);
                }
                _ => {
                    let r = (x.c.regs[0] as u64) * (a as u64);
                    x.c.regs[0] = r as u32;
                    x.c.regs[2] = (r >> 32) as u32;
                    set_muldiv_flags(x, (r >> 32) != 0);
                }
            }
        }
        5 => {
            // IMUL (one-operand)
            let a = x.rm_get(&m, sz);
            match sz {
                1 => {
                    let r = ((x.c.regs[0] & 0xFF) as u8 as i8 as i32) * ((a & 0xFF) as u8 as i8 as i32);
                    x.c.regs[0] = (x.c.regs[0] & 0xFFFF_0000) | ((r as u32) & 0xFFFF);
                    set_muldiv_flags(x, (r as i8 as i32) != r);
                }
                2 => {
                    let r = ((x.c.regs[0] & 0xFFFF) as u16 as i16 as i32)
                        * ((a & 0xFFFF) as u16 as i16 as i32);
                    x.c.regs[0] = (x.c.regs[0] & 0xFFFF_0000) | ((r as u32) & 0xFFFF);
                    x.c.regs[2] = (x.c.regs[2] & 0xFFFF_0000) | (((r as u32) >> 16) & 0xFFFF);
                    set_muldiv_flags(x, (r as i16 as i32) != r);
                }
                _ => {
                    let r = (x.c.regs[0] as i32 as i64) * (a as i32 as i64);
                    x.c.regs[0] = r as u32;
                    x.c.regs[2] = ((r as u64) >> 32) as u32;
                    set_muldiv_flags(x, (r as i32 as i64) != r);
                }
            }
        }
        6 => {
            // DIV
            let a = x.rm_get(&m, sz);
            match sz {
                1 => {
                    if a & 0xFF == 0 { x.div_fault(); return; }
                    let num = x.c.regs[0] & 0xFFFF;
                    let q = num / (a & 0xFF);
                    let r = num % (a & 0xFF);
                    if q > 0xFF { x.div_fault(); return; }
                    x.c.regs[0] = (x.c.regs[0] & 0xFFFF_0000) | (r << 8) | q;
                }
                2 => {
                    if a & 0xFFFF == 0 { x.div_fault(); return; }
                    let num = ((x.c.regs[2] & 0xFFFF) << 16) | (x.c.regs[0] & 0xFFFF);
                    let q = num / (a & 0xFFFF);
                    let r = num % (a & 0xFFFF);
                    if q > 0xFFFF { x.div_fault(); return; }
                    x.c.regs[0] = (x.c.regs[0] & 0xFFFF_0000) | q;
                    x.c.regs[2] = (x.c.regs[2] & 0xFFFF_0000) | r;
                }
                _ => {
                    if a == 0 { x.div_fault(); return; }
                    let num = ((x.c.regs[2] as u64) << 32) | (x.c.regs[0] as u64);
                    let q = num / (a as u64);
                    let r = num % (a as u64);
                    if q > 0xFFFF_FFFF { x.div_fault(); return; }
                    x.c.regs[0] = q as u32;
                    x.c.regs[2] = r as u32;
                }
            }
        }
        _ => {
            // IDIV
            let a = x.rm_get(&m, sz);
            match sz {
                1 => {
                    let d = (a & 0xFF) as u8 as i8 as i32;
                    if d == 0 { x.div_fault(); return; }
                    let num = (x.c.regs[0] & 0xFFFF) as u16 as i16 as i32;
                    let q = num / d;
                    let r = num % d;
                    if q < -128 || q > 127 { x.div_fault(); return; }
                    x.c.regs[0] = (x.c.regs[0] & 0xFFFF_0000)
                        | (((r as u32) & 0xFF) << 8)
                        | ((q as u32) & 0xFF);
                }
                2 => {
                    let d = (a & 0xFFFF) as u16 as i16 as i32;
                    if d == 0 { x.div_fault(); return; }
                    let num = ((((x.c.regs[2] & 0xFFFF) << 16) | (x.c.regs[0] & 0xFFFF)) as u32) as i32;
                    let q = num / d;
                    let r = num % d;
                    if q < -32768 || q > 32767 { x.div_fault(); return; }
                    x.c.regs[0] = (x.c.regs[0] & 0xFFFF_0000) | ((q as u32) & 0xFFFF);
                    x.c.regs[2] = (x.c.regs[2] & 0xFFFF_0000) | ((r as u32) & 0xFFFF);
                }
                _ => {
                    let d = a as i32 as i64;
                    if d == 0 { x.div_fault(); return; }
                    let num = (((x.c.regs[2] as u64) << 32) | (x.c.regs[0] as u64)) as i64;
                    let q = num / d;
                    let r = num % d;
                    if q < -2147483648i64 || q > 2147483647i64 { x.div_fault(); return; }
                    x.c.regs[0] = q as u32;
                    x.c.regs[2] = r as u32;
                }
            }
        }
    }
}

fn set_muldiv_flags(x: &mut Ctx, overflow: bool) {
    let mut f = x.c.eflags & !(F_CF | F_OF);
    if overflow {
        f |= F_CF | F_OF;
    }
    x.c.eflags = f;
}

fn grp5(x: &mut Ctx, osz: u8) {
    let m = x.modrm();
    if x.faulted { return; }
    match m.reg & 7 {
        0 => {
            let a = x.rm_get(&m, osz);
            let cf = x.c.eflags & F_CF;
            let r = x.flags_add(a, 1, 0, osz);
            x.c.eflags = (x.c.eflags & !F_CF) | cf;
            x.rm_set(&m, r, osz);
        }
        1 => {
            let a = x.rm_get(&m, osz);
            let cf = x.c.eflags & F_CF;
            let r = x.flags_sub(a, 1, 0, osz);
            x.c.eflags = (x.c.eflags & !F_CF) | cf;
            x.rm_set(&m, r, osz);
        }
        2 => {
            let t = x.rm_get(&m, osz);
            let ret = x.c.eip;
            x.push(ret, osz);
            x.c.eip = t;
        }
        3 => {
            // CALL FAR m16:32. This is how a DOS/4GW client CHAINS to the
            // previous handler from inside its own ISR, and it MISSing is what
            // stopped DOOM's timer ISR on the tick where it chains.
            if m.is_reg { x.miss(0xFF, 0x100, 0x18); return; }
            far_call(x, &m, osz);
        }
        4 => { let t = x.rm_get(&m, osz); x.c.eip = t; }
        5 => {
            // JMP FAR m16:32, the same load without the pushes.
            if m.is_reg { x.miss(0xFF, 0x100, 0x28); return; }
            let off = x.rd(m.addr, osz);
            let sel = x.rd16(m.addr.wrapping_add(osz as u32));
            if x.faulted { return; }
            x.load_seg(S_CS, sel);   // (#211) the base moves with the selector
            x.c.eip = off;
        }
        6 => { let v = x.rm_get(&m, osz); x.push(v, osz); }
        _ => { x.ud(0xFF, 0x100); }
    }
}

fn string_op(x: &mut Ctx, op: u8, osz: u8) {
    let sz: u8 = if op & 1 == 0 { 1 } else { osz };
    // A REP with ECX == 0 retires as a single no-op instruction.
    if x.rep != 0 && x.cx_get() == 0 {
        return;
    }
    let d = x.str_delta(sz);
    match op {
        0xA4 | 0xA5 => {
            let s = x.si_addr();
            let v = x.rd(s, sz);
            let t = x.di_addr();
            x.wr(t, v, sz);
            x.adv_si(d);
            x.adv_di(d);
        }
        0xAA | 0xAB => {
            let t = x.di_addr();
            let v = x.rget(0, sz);
            x.wr(t, v, sz);
            x.adv_di(d);
        }
        0xAC | 0xAD => {
            let s = x.si_addr();
            let v = x.rd(s, sz);
            x.rset(0, v, sz);
            x.adv_si(d);
        }
        0xA6 | 0xA7 => {
            let s = x.si_addr();
            let a = x.rd(s, sz);
            let t = x.di_addr();
            let b = x.rd(t, sz);
            x.flags_sub(a, b, 0, sz);
            x.adv_si(d);
            x.adv_di(d);
        }
        _ => {
            // AE/AF SCAS
            let t = x.di_addr();
            let b = x.rd(t, sz);
            let a = x.rget(0, sz);
            x.flags_sub(a, b, 0, sz);
            x.adv_di(d);
        }
    }
    if x.faulted {
        return;
    }
    if x.rep != 0 {
        x.cx_dec();
        let cx = x.cx_get();
        let is_cmp = matches!(op, 0xA6 | 0xA7 | 0xAE | 0xAF);
        let zf = x.c.eflags & F_ZF != 0;
        let stop = cx == 0
            || (is_cmp && ((x.rep == 0xF3 && !zf) || (x.rep == 0xF2 && zf)));
        if !stop {
            // Re-execute: leave EIP at the prefix so the next dispatch resumes
            // here, and so the guest is interruptible between iterations.
            x.c.eip = x.ins_eip;
        }
    }
}

/// OUTS: one element per dispatch, exactly like `string_op`, and for the same
/// reason (an interruptible guest and an honest instruction count).
///
/// WHY IT LOOKS LIKE THIS. This core does not perform I/O; it EXITS to the host
/// with `X32_EXIT_IO_OUT` and lets dos/dosexec.c's `dos_out()` decide what the
/// port means. There is therefore one host round trip per element, and the REP
/// bookkeeping has to happen BEFORE the exit: ECX is decremented and eSI
/// advanced here, and EIP is rewound to the prefix so the next entry resumes
/// the same instruction on the next element. From the host's side this is
/// indistinguishable from N separate `out dx,al` instructions, which is the
/// point: `dos_out()` needed no change at all, so a `rep outsb` to the VGA DAC
/// goes through the same port emulation an ordinary `out` does, and cannot
/// drift from it.
fn string_io_out(x: &mut Ctx, op: u8, osz: u8) {
    let sz: u8 = if op & 1 == 0 { 1 } else { osz };
    // A REP with ECX == 0 retires as a single no-op instruction, and must NOT
    // exit to the host: an OUT of nothing is still an OUT the host would see.
    if x.rep != 0 && x.cx_get() == 0 {
        return;
    }
    let d = x.str_delta(sz);
    let src = x.si_addr();
    let v = x.rd(src, sz);
    if x.faulted {
        // rd() has already set FAULT_MEM and rewound EIP. Do not overwrite that
        // with an I/O exit: the guest read outside its arena, and THAT is the
        // event worth reporting.
        return;
    }
    x.adv_si(d);
    if x.rep != 0 {
        x.cx_dec();
        if x.cx_get() != 0 {
            x.c.eip = x.ins_eip;
        }
    }
    x.c.exit_reason = X32_EXIT_IO_OUT;
    x.c.exit_arg = x.c.regs[2] & 0xFFFF;
    x.c.io_size = sz as u32;
    x.c.io_val = v;
    x.faulted = true;
}

// ---------------------------------------------------------------------------
// BCD helpers. Implemented rather than MISSed because they are single-byte
// opcodes that silently corrupt AL if skipped.
// ---------------------------------------------------------------------------
fn daa(x: &mut Ctx) {
    let mut al = x.c.regs[0] & 0xFF;
    let old_al = al;
    let old_cf = x.cf();
    let mut cf = 0u32;
    let mut af = 0u32;
    if (al & 0x0F) > 9 || (x.c.eflags & F_AF != 0) {
        al = al.wrapping_add(6);
        af = 1;
        cf = old_cf | if al > 0xFF { 1 } else { 0 };
        al &= 0xFF;
    }
    if old_al > 0x99 || old_cf != 0 {
        al = (al.wrapping_add(0x60)) & 0xFF;
        cf = 1;
    }
    x.c.regs[0] = (x.c.regs[0] & 0xFFFF_FF00) | al;
    x.set_szp(al, 1);
    let mut f = x.c.eflags & !(F_CF | F_AF);
    if cf != 0 { f |= F_CF; }
    if af != 0 { f |= F_AF; }
    x.c.eflags = f;
}

fn das(x: &mut Ctx) {
    let mut al = x.c.regs[0] & 0xFF;
    let old_al = al;
    let old_cf = x.cf();
    let mut cf = 0u32;
    let mut af = 0u32;
    if (al & 0x0F) > 9 || (x.c.eflags & F_AF != 0) {
        let borrow = al < 6;
        al = al.wrapping_sub(6) & 0xFF;
        af = 1;
        cf = old_cf | if borrow { 1 } else { 0 };
    }
    if old_al > 0x99 || old_cf != 0 {
        al = al.wrapping_sub(0x60) & 0xFF;
        cf = 1;
    }
    x.c.regs[0] = (x.c.regs[0] & 0xFFFF_FF00) | al;
    x.set_szp(al, 1);
    let mut f = x.c.eflags & !(F_CF | F_AF);
    if cf != 0 { f |= F_CF; }
    if af != 0 { f |= F_AF; }
    x.c.eflags = f;
}

fn aaa(x: &mut Ctx) {
    let al = x.c.regs[0] & 0xFF;
    let ah = (x.c.regs[0] >> 8) & 0xFF;
    let mut f = x.c.eflags & !(F_CF | F_AF);
    if (al & 0x0F) > 9 || (x.c.eflags & F_AF != 0) {
        let nal = (al.wrapping_add(6)) & 0x0F;
        let nah = (ah.wrapping_add(1)) & 0xFF;
        x.c.regs[0] = (x.c.regs[0] & 0xFFFF_0000) | (nah << 8) | nal;
        f |= F_CF | F_AF;
    } else {
        x.c.regs[0] = (x.c.regs[0] & 0xFFFF_FF00) | (al & 0x0F);
    }
    x.c.eflags = f;
}

fn aas(x: &mut Ctx) {
    let al = x.c.regs[0] & 0xFF;
    let ah = (x.c.regs[0] >> 8) & 0xFF;
    let mut f = x.c.eflags & !(F_CF | F_AF);
    if (al & 0x0F) > 9 || (x.c.eflags & F_AF != 0) {
        let nal = (al.wrapping_sub(6)) & 0x0F;
        let nah = (ah.wrapping_sub(1)) & 0xFF;
        x.c.regs[0] = (x.c.regs[0] & 0xFFFF_0000) | (nah << 8) | nal;
        f |= F_CF | F_AF;
    } else {
        x.c.regs[0] = (x.c.regs[0] & 0xFFFF_FF00) | (al & 0x0F);
    }
    x.c.eflags = f;
}

/// LES / LDS / LSS / LFS / LGS: load a far pointer from memory into a general
/// register and a segment register. ONE implementation for all five, because
/// they differ only in which segment register they target and a per-opcode copy
/// is how two of them end up with different bugs.
///
/// LAYOUT: the offset comes first and the SELECTOR follows it, at +4 for a
/// 32-bit operand size and at +2 for 16-bit. The selector is always 16 bits
/// whatever the operand size, which is the part that is easy to get wrong.
///
/// WHY THIS IS NOW IMPLEMENTED RATHER THAN A MISS. It used to MISS on the
/// grounds that "the segment half is a selector the DPMI host handed out, so
/// this is expressible once descriptors exist". That reasoning is right about
/// descriptors and wrong about the cost of waiting: MEASURED, DOOM's INT 8
/// dispatch stub executes `lss` to switch to its interrupt stack, so the very
/// first timer interrupt ever delivered to a 32-bit guest stopped four
/// instructions into the handler.
///
/// WHAT IS MODELLED (updated #740 dw2; this paragraph used to say the base was
/// NOT recomputed, and named that as "the one case to revisit when descriptors
/// land"). Descriptors have landed. The selector value is loaded AND resolved
/// to a base through `load_seg`, so a selector with a non-zero base, which is
/// what DPMI 0100h hands back for a DOS memory block, now addresses the block
/// instead of addressing flat zero. Discworld II's VbeInfoBlock landed on the
/// interrupt vector table for exactly as long as that was untrue.
///
/// A register operand is an invalid encoding for these (the source must be
/// memory) and MISSes rather than inventing a read.
fn far_ptr_load(x: &mut Ctx, seg: usize, osz: u8) {
    let m = x.modrm();
    if x.faulted {
        return;
    }
    if m.is_reg {
        // mod == 11 is UNDEFINED for this group: there is no far pointer in a
        // register. Report it rather than reading one out of thin air.
        x.miss(if seg == S_ES { 0xC4 } else { 0xC5 }, 0x100, 0xC0);
        return;
    }
    let off = x.rd(m.addr, osz);
    let sel = x.rd16(m.addr.wrapping_add(osz as u32));
    if x.faulted {
        return;
    }
    x.load_seg(seg, sel);
    x.rset(m.reg, off, osz);
}

/// CALL FAR m16:32. Pushes CS then the return EIP, then loads CS:EIP from the
/// far pointer in memory.
///
/// THE CS PUSH IS ZERO-EXTENDED HERE, and that is not the same rule as
/// `PUSH Sreg`. push_seg() models the documented silicon quirk for the PUSH
/// instruction (16-bit move, upper half of the slot untouched); the SDM
/// describes CALL FAR's push separately and the oracle's own far-call vector
/// is what settled which one this is, rather than an assumption carried across
/// from the neighbouring case.
///
/// WHY IT MATTERS BEYOND CALL: a DOS/4GW client's ISR chains to the previous
/// handler with `pushfd` followed by this instruction, so the three dwords on
/// the stack are exactly what an IRETD pops. The seeded stub the unhooked
/// vector points at is a single 0xCF byte, i.e. IRETD, so that chain closes
/// correctly with no special case anywhere.
fn far_call(x: &mut Ctx, m: &ModRm, osz: u8) {
    let off = x.rd(m.addr, osz);
    let sel = x.rd16(m.addr.wrapping_add(osz as u32));
    if x.faulted {
        return;
    }
    let cs = x.c.seg[S_CS] as u32;
    let ret = x.c.eip;
    x.push(cs, osz);
    x.push(ret, osz);
    if x.faulted {
        return;
    }
    x.load_seg(S_CS, sel);   // (#211) the base moves with the selector
    x.c.eip = off;
}

// ---------------------------------------------------------------------------
// (#211) x87 memory operands. Little-endian, through the SAME bounds-checked
// rd/wr the rest of the core uses, so an FP address cannot escape the window
// any more than an integer one can. Widths are 2, 4, 8 or 10 bytes; the 10-byte
// case is an 80-bit extended real, whose top word is the sign and exponent.
// ---------------------------------------------------------------------------
fn x87_mem_read(x: &mut Ctx, addr: u32, n: u8, m: &mut crate::x87::X87Mem) {
    match n {
        2 => { m.lo = x.rd(addr, 2); }
        4 => { m.lo = x.rd(addr, 4); }
        8 => { m.lo = x.rd(addr, 4); m.hi = x.rd(addr.wrapping_add(4), 4); }
        _ => {
            m.lo = x.rd(addr, 4);
            m.hi = x.rd(addr.wrapping_add(4), 4);
            m.ex = x.rd(addr.wrapping_add(8), 2);
        }
    }
}

fn x87_mem_write(x: &mut Ctx, addr: u32, n: u8, m: &crate::x87::X87Mem) {
    match n {
        2 => x.wr(addr, m.lo, 2),
        4 => x.wr(addr, m.lo, 4),
        8 => { x.wr(addr, m.lo, 4); x.wr(addr.wrapping_add(4), m.hi, 4); }
        _ => {
            x.wr(addr, m.lo, 4);
            x.wr(addr.wrapping_add(4), m.hi, 4);
            x.wr(addr.wrapping_add(8), m.ex, 2);
        }
    }
}

// ---------------------------------------------------------------------------
// The 0F two-byte map
// ---------------------------------------------------------------------------
fn two_byte(x: &mut Ctx, osz: u8) {
    let op2 = x.fetch8();
    match op2 {
        0x00 | 0x01 => {
            // Group 6 / group 7: LLDT, LTR, LGDT, LIDT, SMSW, LMSW, INVLPG.
            // A DOS/4GW guest under a DPMI host we implement never runs these.
            let m = x.modrm();
            if x.faulted { return; }
            let modrm_byte = if m.is_reg { 0xC0 | ((m.reg as u32) << 3) | m.rm_reg as u32 } else { (m.reg as u32) << 3 };
            x.miss(0x0F, op2 as u32, modrm_byte);
        }
        0x02 | 0x03 => {
            // (#211) LAR / LSL. djgpp's crt0 runs `lsl %bx,%ebx` UNCONDITIONALLY
            // on the alias descriptor it has just created, to find out whether
            // this host can give it a 4 GB selector. A MISS here stops every
            // DJGPP program 0x67 bytes into its entry, before one byte of the
            // program's own code runs, which is exactly the shape of failure
            // that looks like "the loader is broken".
            //
            // The answer comes from the DPMI host's own descriptor table
            // through the SAME resolver every segment-register load uses, so a
            // limit LSL reports and a limit an effective address is checked
            // against cannot come from two different tables.
            let m = x.modrm();
            if x.faulted { return; }
            let sel = (x.rm_get(&m, 2) & 0xFFFF) as u16;
            let mut base = 0u32;
            let mut limit = 0u32;
            let mut ar = 0u8;
            let ok = match x.c.sb_cb {
                Some(f) => f(sel, &mut base, &mut limit, &mut ar) == 0,
                None => false,
            };
            if ok {
                if op2 == 0x03 {
                    x.rset(m.reg, limit, osz);
                } else {
                    x.rset(m.reg, ((ar as u32) << 8) & 0x00FF_FF00, osz);
                }
                x.c.eflags |= F_ZF;
            } else {
                // Not a descriptor this host handed out. ZF=0 and the
                // DESTINATION IS LEFT ALONE, which is the architectural answer
                // and also the honest one: we do not know what that selector
                // describes, and writing a plausible number would be a guess a
                // guest cannot distinguish from a fact.
                x.c.eflags &= !F_ZF;
            }
        }
        0x0B => { x.ud(0x0F, op2 as u32); } // UD2, and it means it
        0x1F => { let _m = x.modrm(); /* multi-byte NOP */ }
        0x20 | 0x21 | 0x22 | 0x23 => {
            let _m = x.modrm();
            x.miss(0x0F, op2 as u32, 0x100); // MOV to/from CRn/DRn
        }
        0x31 => {
            // RDTSC. The guest gets the retired-instruction count, which is
            // monotonic, cheap, and independent of host scheduling. Wall time
            // here would be a lie: blame.md records that timer_ticks is not a
            // wall clock and that a DOS guest calibrating against real
            // microseconds measures the scheduler's holes, not the CPU.
            let t = x.c.insn_count;
            x.c.regs[0] = t as u32;
            x.c.regs[2] = (t >> 32) as u32;
        }
        0xA2 => {
            // CPUID. blame.md: "an interpreter that dies on an unimplemented
            // opcode kills the diagnostic too" -- a real 32-bit extended DOS
            // program stopped 1,660 instructions in, having printed nothing,
            // because the 16-bit core had no case for this. Report a plain
            // 486-class part with no features, which is what a DOS/4GW-era
            // binary expects and can act on.
            let leaf = x.c.regs[0];
            match leaf {
                0 => {
                    x.c.regs[0] = 1; // max leaf
                    x.c.regs[3] = 0x756E_6547; // "Genu"
                    x.c.regs[1] = 0x6C65_746E; // "ntel"
                    x.c.regs[2] = 0x4965_6E69; // "ineI"
                }
                _ => {
                    x.c.regs[0] = 0x0000_0480; // family 4, model 8
                    x.c.regs[3] = 0;
                    x.c.regs[1] = 0;
                    // (#211) EDX. The FPU bit (bit 0) is DELIBERATELY CLEAR
                    // even though x87 is now implemented (rustkern/x87.rs).
                    // Setting it would change the code path of every OTHER
                    // guest that asks - DOOM, Discworld II - from the integer
                    // one they are measured working on to an FP one whose
                    // transcendentals still MISS. The one target that needed
                    // x87 (#211's DJGPP crt) does not consult CPUID at all: it
                    // probes with FNINIT/FNSTCW. Turn this on with a
                    // measurement, not on the strength of the unit existing.
                    x.c.regs[2] = 0;
                }
            }
        }
        0x40..=0x4F => {
            // CMOVcc
            let m = x.modrm();
            if x.faulted { return; }
            let v = x.rm_get(&m, osz);
            if x.cond(op2 & 0x0F) {
                x.rset(m.reg, v, osz);
            }
        }
        0x80..=0x8F => {
            let d = if osz == 4 { x.fetch32() as i32 } else { (x.fetch16() as i16) as i32 };
            if x.cond(op2 & 0x0F) {
                x.c.eip = (x.c.eip as i32).wrapping_add(d) as u32;
            }
        }
        0x90..=0x9F => {
            let m = x.modrm();
            if x.faulted { return; }
            let v = if x.cond(op2 & 0x0F) { 1u32 } else { 0u32 };
            x.rm_set(&m, v, 1);
        }
        0xA0 => { let v = x.c.seg[S_FS]; x.push_seg(v, osz); }
        0xA1 => { let v = x.pop(osz); x.load_seg(S_FS, v as u16); }
        0xA8 => { let v = x.c.seg[S_GS]; x.push_seg(v, osz); }
        0xA9 => { let v = x.pop(osz); x.load_seg(S_GS, v as u16); }
        0xA3 | 0xAB | 0xB3 | 0xBB => {
            // BT / BTS / BTR / BTC, register bit index
            let m = x.modrm();
            if x.faulted { return; }
            let idx = x.rget(m.reg, osz);
            bit_op(x, &m, idx, op2, osz);
        }
        0xBA => {
            // Group 8: BT/BTS/BTR/BTC with an imm8 bit index
            let m = x.modrm();
            if x.faulted { return; }
            let sub = m.reg & 7;
            let imm = x.fetch8() as u32;
            if sub < 4 {
                x.miss(0x0F, 0xBA, (sub as u32) << 3);
                return;
            }
            let map = [0u8, 0, 0, 0, 0xA3, 0xAB, 0xB3, 0xBB];
            bit_op(x, &m, imm, map[sub], osz);
        }
        0xA4 | 0xA5 | 0xAC | 0xAD => {
            // SHLD / SHRD
            let m = x.modrm();
            if x.faulted { return; }
            let cnt = if op2 & 1 == 0 { x.fetch8() as u32 } else { x.c.regs[1] & 0xFF } & 0x1F;
            let dst = x.rm_get(&m, osz);
            let src = x.rget(m.reg, osz);
            if cnt == 0 {
                return;
            }
            let bits = (osz as u32) * 8;
            if cnt >= bits {
                // Architecturally undefined. Leave the destination alone rather
                // than write a value the oracle would have to be told to ignore.
                return;
            }
            let left = op2 < 0xAC;
            let (r, cf) = if left {
                let cf = (dst >> (bits - cnt)) & 1;
                (((dst << cnt) | (src >> (bits - cnt))) & mask_of(osz), cf)
            } else {
                let cf = (dst >> (cnt - 1)) & 1;
                (((dst >> cnt) | (src << (bits - cnt))) & mask_of(osz), cf)
            };
            x.set_szp(r, osz);
            x.c.eflags = (x.c.eflags & !F_CF) | cf;
            x.rm_set(&m, r, osz);
        }
        0xAF => {
            let m = x.modrm();
            if x.faulted { return; }
            let a = x.rget(m.reg, osz);
            let b = x.rm_get(&m, osz);
            let r = imul_flags(x, a, b, osz);
            x.rset(m.reg, r, osz);
        }
        0xB0 | 0xB1 => {
            // CMPXCHG
            let sz = if op2 == 0xB0 { 1 } else { osz };
            let m = x.modrm();
            if x.faulted { return; }
            let dst = x.rm_get(&m, sz);
            let acc = x.rget(0, sz);
            x.flags_sub(acc, dst, 0, sz);
            if (acc & mask_of(sz)) == (dst & mask_of(sz)) {
                let src = x.rget(m.reg, sz);
                x.rm_set(&m, src, sz);
            } else {
                x.rset(0, dst, sz);
            }
        }
        0xB2 | 0xB4 | 0xB5 => {
            // LSS / LFS / LGS. See far_ptr_load(); LSS in particular is what a
            // DOS/4GW interrupt dispatcher uses to switch to its locked stack,
            // and it MISSing is what stopped DOOM's first timer ISR dead.
            let s = match op2 { 0xB2 => S_SS, 0xB4 => S_FS, _ => S_GS };
            far_ptr_load(x, s, osz);
        }
        0xB6 | 0xB7 => {
            let m = x.modrm();
            if x.faulted { return; }
            let ssz: u8 = if op2 == 0xB6 { 1 } else { 2 };
            let v = x.rm_get(&m, ssz) & mask_of(ssz);
            x.rset(m.reg, v, osz);
        }
        0xBE | 0xBF => {
            let m = x.modrm();
            if x.faulted { return; }
            let ssz: u8 = if op2 == 0xBE { 1 } else { 2 };
            let v = x.rm_get(&m, ssz);
            x.rset(m.reg, sext(v, ssz) as u32, osz);
        }
        0xBC | 0xBD => {
            let m = x.modrm();
            if x.faulted { return; }
            let v = x.rm_get(&m, osz) & mask_of(osz);
            if v == 0 {
                x.c.eflags |= F_ZF;
                // Destination is architecturally undefined; leave it.
            } else {
                x.c.eflags &= !F_ZF;
                let n = if op2 == 0xBC {
                    v.trailing_zeros()
                } else {
                    31 - v.leading_zeros()
                };
                x.rset(m.reg, n, osz);
            }
        }
        0xC0 | 0xC1 => {
            // XADD
            let sz = if op2 == 0xC0 { 1 } else { osz };
            let m = x.modrm();
            if x.faulted { return; }
            let a = x.rm_get(&m, sz);
            let b = x.rget(m.reg, sz);
            let r = x.flags_add(a, b, 0, sz);
            x.rset(m.reg, a, sz);
            x.rm_set(&m, r, sz);
        }
        0xC8..=0xCF => {
            let i = (op2 & 7) as usize;
            let v = x.c.regs[i].swap_bytes();
            x.c.regs[i] = v;
        }
        _ => { x.miss(0x0F, op2 as u32, 0x100); }
    }
}

fn bit_op(x: &mut Ctx, m: &ModRm, idx: u32, which: u8, osz: u8) {
    let bits = (osz as u32) * 8;
    if m.is_reg {
        let b = idx & (bits - 1);
        let v = x.rget(m.rm_reg, osz);
        let cf = (v >> b) & 1;
        x.c.eflags = (x.c.eflags & !F_CF) | cf;
        let nv = match which {
            0xAB => v | (1 << b),
            0xB3 => v & !(1 << b),
            0xBB => v ^ (1 << b),
            _ => v,
        };
        if which != 0xA3 {
            x.rset(m.rm_reg, nv, osz);
        }
    } else {
        // Memory form: the bit index is a SIGNED offset into a bit string that
        // extends both ways from the effective address. Getting this wrong is a
        // silent out-of-window access, so the byte address is computed with
        // signed arithmetic and then goes through the same checked accessor.
        let si = idx as i32;
        let byte_off = si >> 3;
        let b = (si & 7) as u32;
        let la = (m.addr as i32).wrapping_add(byte_off) as u32;
        let v = x.rd8(la) as u32;
        if x.faulted { return; }
        let cf = (v >> b) & 1;
        x.c.eflags = (x.c.eflags & !F_CF) | cf;
        let nv = match which {
            0xAB => v | (1 << b),
            0xB3 => v & !(1 << b),
            0xBB => v ^ (1 << b),
            _ => v,
        };
        if which != 0xA3 {
            x.wr8(la, nv as u8);
        }
    }
}

// ---------------------------------------------------------------------------
// Small C-callable helpers so the host never reaches into the struct by hand.
// ---------------------------------------------------------------------------

/// Byte size of the struct, so the C `_Static_assert` has something to check
/// that is produced by the Rust compiler rather than restated by hand.
#[no_mangle]
pub extern "C" fn x86_32_cpu_size() -> u32 {
    core::mem::size_of::<X8632Cpu>() as u32
}

/// Reset a CPU to a defined 32-bit flat state over the given arena.
///
/// # Safety
/// `cpu` must point to writable storage of at least `x86_32_cpu_size()` bytes,
/// and `mem`/`mem_size` must describe a writable region.
#[no_mangle]
pub unsafe extern "C" fn x86_32_init(
    cpu: *mut X8632Cpu,
    mem: *mut u8,
    mem_base: u32,
    mem_size: u32,
) {
    if cpu.is_null() {
        return;
    }
    let c = &mut *cpu;
    c.regs = [0; 8];
    c.eip = 0;
    // Bit 1 is always set on a real part. IF starts SET, because a real DOS
    // program starts with interrupts enabled and because it is the state the
    // host-native oracle process is in (a ring-3 process at IOPL 0 cannot clear
    // IF, so nothing else would agree with it).
    //
    // KNOWN UNTESTABLE, stated rather than discovered later: whether POPFD may
    // change IF is NOT covered by the oracle, and cannot be. On the host the
    // vector runs at CPL 3 with IOPL 0, where POPFD silently leaves IF alone;
    // this core lets POPFD write IF, which is the behaviour a DPMI client sees
    // from a host that gives it IOPL 3, and is what makes a guest's
    // pushfd/cli/.../popfd critical section work. Both arms of the oracle end
    // with IF set, so the difference is invisible to it BY CONSTRUCTION. That is
    // #740 blame lesson 5 applied to ourselves: when a sample cannot exercise a
    // field, say so instead of letting "verified" cover it.
    c.eflags = 0x202;
    c.seg = [0; 6];
    c.pad0 = 0;
    c.pad1 = 0;
    c.seg_base = [0; 6];
    // (#211) A fresh guest starts with a reset FPU, exactly as a real one does
    // after RESET: control word 0x037F, empty stack, clear status.
    c.fp = [0; 8];
    c.fp_top = 8;
    c.fp_cw = 0x037F;
    c.fp_sw = 0;
    c.fp_pad = 0;
    c.mem = mem;
    c.mem_base = mem_base;
    c.mem_size = mem_size;
    c.insn_count = 0;
    c.exit_reason = X32_EXIT_BUDGET;
    c.exit_arg = 0;
    c.fault_eip = 0;
    c.fault_addr = 0;
    c.stop_eip = 0;
    c.stop_eip_en = 0;
    c.io_val = 0;
    c.io_size = 0;
    c.miss_count = 0;
    c.miss_op = 0;
    c.miss_op2 = 0x100;
    c.miss_modrm = 0x100;
    c.miss_len = 0;
    c.owner = core::ptr::null_mut();
    c.mh_lo = 0;
    c.mh_hi = 0;
    c.mh_w = None;
    c.mh_r = None;
    c.lw_hi = 0;
    c.lw_pad = 0;
    c.lw_cb = None;
    c.sb_cb = None;
    c.sel_miss_n = 0;
    c.sel_miss_logged = 0;
    c.sel_miss_first_sel = 0;
    c.sel_miss_first_eip = 0;
    c.sel_gdt_n = 0;
    c.sel_pad = 0;
}

/// Register (or clear, with wfn=None/rfn=None) the memory hook, mirroring
/// x86_16_set_mem_hook(). Call it AFTER x86_32_init(), which zeroes the hook
/// fields; calling it before would be overwritten.
///
/// # Safety
/// `cpu` must point to writable storage of at least `x86_32_cpu_size()` bytes.
#[no_mangle]
pub unsafe extern "C" fn x86_32_set_mem_hook(
    cpu: *mut X8632Cpu,
    lo: u32,
    hi: u32,
    wfn: Option<extern "C" fn(*mut X8632Cpu, u32, u32, i32) -> u32>,
    rfn: Option<extern "C" fn(*mut X8632Cpu, u32, i32) -> u32>,
) {
    if cpu.is_null() {
        return;
    }
    let c = &mut *cpu;
    c.mh_lo = lo;
    c.mh_hi = hi;
    c.mh_w = wfn;
    c.mh_r = rfn;
}

/// Arm (or disarm, with `hi = 0`) the low-memory write watch. See the field
/// comment on `lw_hi`. Call it AFTER `x86_32_init`, which clears it.
///
/// # Safety
/// `cpu` must point to writable storage of at least `x86_32_cpu_size()` bytes.
#[no_mangle]
pub unsafe extern "C" fn x86_32_set_low_watch(
    cpu: *mut X8632Cpu,
    hi: u32,
    cb: Option<extern "C" fn(*mut X8632Cpu, u32, u32, i32, u32)>,
) {
    if cpu.is_null() {
        return;
    }
    let c = &mut *cpu;
    c.lw_hi = hi;
    c.lw_cb = cb;
}

/// Bind the selector-to-base resolver. Pass `dpmi_sel_lookup_rs` (its signature
/// is this one). None restores the flat-only behaviour, in which every selector
/// has base 0.
///
/// # Safety
/// `cpu` must point to writable storage of at least `x86_32_cpu_size()` bytes.
#[no_mangle]
pub unsafe extern "C" fn x86_32_set_sel_base_cb(
    cpu: *mut X8632Cpu,
    cb: Option<extern "C" fn(u16, *mut u32, *mut u32, *mut u8) -> i32>,
) {
    if cpu.is_null() {
        return;
    }
    (*cpu).sb_cb = cb;
}

// ---------------------------------------------------------------------------
// ASYNCHRONOUS INTERRUPT DELIVERY: the direction that was missing.
//
// A guest interface has calls OUT (the guest executes INT n, which this core
// reports as X32_EXIT_INT) and events IN (the host delivers an IRQ the guest is
// WAITING to receive). Only the first was built, and the measured consequence
// was a DOS/4GW guest that ran its entire startup, set mode 13h and then made
// no service call for 800 seconds: it was spinning on a tic counter that its
// own timer ISR increments, and nothing could ever run that ISR.
//
// This is the whole of the second direction. It is deliberately NOT a "run the
// handler now" call: it builds the frame a real interrupt gate builds, points
// EIP at the handler, and returns. The handler then runs in the caller's normal
// x86_32_run() slices and comes back through IRETD like any other code, so
// there is no nested interpreter, no second budget to get wrong, and no way for
// a handler that never returns to run unbounded inside a host function.
// ---------------------------------------------------------------------------

/// Delivered: the frame is on the guest stack and EIP is at the handler.
pub const X32_INJ_DELIVERED: i32 = 0;
/// Refused because the guest has interrupts masked (EFLAGS.IF clear). NOT an
/// error: it is what the hardware does, and it is what stops a second interrupt
/// re-entering a handler this same mechanism started.
pub const X32_INJ_MASKED: i32 = 1;
/// The frame did not fit: the guest's stack pointer is outside the arena. The
/// CPU is left with the ESP and EIP it had, so the caller may keep running it.
pub const X32_INJ_FAULT: i32 = -1;

/// Deliver an asynchronous interrupt to the guest at an instruction boundary.
///
/// Builds the 32-bit interrupt-gate frame (EFLAGS, CS, EIP pushed in that
/// order, so IRETD pops them back in the reverse), clears IF and TF exactly as
/// an interrupt gate does, and sets EIP to `handler`. Returns one of the
/// X32_INJ_* codes above.
///
/// THE CALLER MUST ONLY CALL THIS AT AN INSTRUCTION BOUNDARY, which for this
/// core means "immediately after x86_32_run() returned". That is not a
/// documented wish: x86_32_run() has no other exit, so the requirement is
/// satisfied by construction for every caller that cannot see inside it.
///
/// # Safety
/// `cpu` must be a valid, initialised `X8632Cpu` whose `mem`/`mem_size` still
/// describe the arena, i.e. the same contract as `x86_32_run`.
#[no_mangle]
pub unsafe extern "C" fn x86_32_inject_int(cpu: *mut X8632Cpu, handler: u32) -> i32 {
    if cpu.is_null() {
        return X32_INJ_FAULT;
    }
    let c = &mut *cpu;
    if c.mem.is_null() || c.mem_size == 0 {
        return X32_INJ_FAULT;
    }
    if c.eflags & F_IF == 0 {
        return X32_INJ_MASKED;
    }
    let saved_eip = c.eip;
    let saved_esp = c.regs[4];
    let saved_exit = c.exit_reason;
    let mem = core::slice::from_raw_parts_mut(c.mem, c.mem_size as usize);
    let mut x = Ctx {
        c,
        mem,
        faulted: false,
        opsize32: true,
        addr32: true,
        seg_ovr: -1,
        rep: 0,
        lock: false,
        ins_eip: saved_eip,
    };
    // PUSHFD's own masking, reached through the same constant, so the image an
    // ISR sees on its stack is byte-identical to the one it would see from a
    // pushfd it executed itself.
    let fl = x.c.eflags & 0x00FC_FFFF;
    // ZERO-EXTENDED, deliberately, and NOT through push_seg. push_seg models
    // what `PUSH Sreg` does on silicon (a 16-bit move that leaves the upper
    // half of the slot alone), which is right for an instruction the guest
    // executed. This frame is one WE build, the SDM leaves the upper half of a
    // gate-pushed CS undefined, and handing a guest ISR two stale bytes it may
    // read is strictly worse than handing it a defined zero.
    let cs = x.c.seg[S_CS] as u32;
    x.push(fl, 4);
    x.push(cs, 4);
    x.push(saved_eip, 4);
    if x.faulted {
        // The stack pointer is put back and the exit reason restored, so a
        // guest whose ESP is briefly outside the window is not KILLED by our
        // attempt to interrupt it. fault_addr/fault_eip are left set: they are
        // the only record of where this happened and nothing else reads them
        // unless exit_reason says a fault occurred.
        x.c.regs[4] = saved_esp;
        x.c.eip = saved_eip;
        x.c.exit_reason = saved_exit;
        return X32_INJ_FAULT;
    }
    x.c.eflags &= !(F_IF | F_TF);
    x.c.eip = handler;
    X32_INJ_DELIVERED
}

// ---------------------------------------------------------------------------
// (#740 digplay) A PROTECTED-MODE FAR CALL INTO THE GUEST.
//
// x86_32_inject_int() above delivers something the guest is WAITING TO RECEIVE
// and lets it return through IRETD. A mouse driver's INT 33h 0Ch/14h event
// handler is not that shape: it is a FAR PROCEDURE the driver CALLS, and it
// returns with RETF, not IRET. MEASURED on The Dig (DIG.EXE, LE at file offset
// 0x2c90, handler at guest linear 0x0013F360, disassembled from the LE object
// this host itself loads):
//
//     1e            push %ds            <- saves the caller's DS
//     e8 ..         call <load flat DS>
//     66 83 3d ..   cmpw $0x0,<reentrancy flag>
//     ...
//     1f            pop  %ds
//     cb            lret                <- FAR return; all four exits are RETF
//
// A frame with EFLAGS on it would leave the handler returning to the wrong
// address with four bytes of stack skew, so this is a separate entry point
// rather than a flag on the interrupt one: the two build DIFFERENT frames and
// there is no parameter that turns one into the other.
//
// WHAT IT PUSHES, and why CS then EIP. A 32-bit far CALL pushes the segment
// first and the offset second, so the matching RETF pops the offset first. CS
// is zero-extended into a four-byte slot for the same reason inject_int
// zero-extends it: the SDM leaves the upper half undefined, and a defined zero
// beats two stale bytes a guest may read.
//
// `ret_eip` is where the handler will RETF to. The caller is expected to have
// set cpu.stop_eip to it, so "the handler returned" is DETECTED at an
// instruction boundary rather than executed, and nothing has to be present at
// that address. That is the same reasoning the 16-bit dos_mouse_events()
// already uses for its F000: stub.
//
// IF IS NOT CLEARED and no flags are pushed, because this is a CALL and not a
// gate. IF is still CHECKED: a guest inside a cli region has said it is not
// ready to be re-entered, and the caller can hold the event and offer it again
// on the next slice.
/// Deliver a far CALL to `handler`, returning to `ret_eip`.
///
/// Returns X32_INJ_DELIVERED / X32_INJ_MASKED / X32_INJ_FAULT.
///
/// # Safety
/// `cpu` must point at a valid, initialised X8632Cpu. Call only at an
/// instruction boundary (i.e. immediately after x86_32_run() returned).
#[no_mangle]
pub unsafe extern "C" fn x86_32_inject_farcall(
    cpu: *mut X8632Cpu,
    handler: u32,
    ret_eip: u32,
) -> i32 {
    if cpu.is_null() {
        return X32_INJ_FAULT;
    }
    let c = &mut *cpu;
    if c.mem.is_null() || c.mem_size == 0 {
        return X32_INJ_FAULT;
    }
    if c.eflags & F_IF == 0 {
        return X32_INJ_MASKED;
    }
    let saved_eip = c.eip;
    let saved_esp = c.regs[4];
    let saved_exit = c.exit_reason;
    let mem = core::slice::from_raw_parts_mut(c.mem, c.mem_size as usize);
    let mut x = Ctx {
        c,
        mem,
        faulted: false,
        opsize32: true,
        addr32: true,
        seg_ovr: -1,
        rep: 0,
        lock: false,
        ins_eip: saved_eip,
    };
    let cs = x.c.seg[S_CS] as u32;
    x.push(cs, 4);
    x.push(ret_eip, 4);
    if x.faulted {
        // Nothing is left half-done: ESP, EIP and the exit reason go back, so a
        // guest whose stack pointer is briefly outside the window is not killed
        // by our attempt to call into it.
        x.c.regs[4] = saved_esp;
        x.c.eip = saved_eip;
        x.c.exit_reason = saved_exit;
        return X32_INJ_FAULT;
    }
    x.c.eip = handler;
    X32_INJ_DELIVERED
}

/// Bounds-checked guest read for the host, so no C caller ever indexes the
/// arena directly. Returns 0 on success, -1 if the range is outside the window.
///
/// # Safety
/// `cpu` must be valid; `dst` must accept `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn x86_32_read_guest(
    cpu: *const X8632Cpu,
    la: u32,
    dst: *mut u8,
    len: u32,
) -> i32 {
    if cpu.is_null() || dst.is_null() {
        return -1;
    }
    let c = &*cpu;
    if c.mem.is_null() || la < c.mem_base {
        return -1;
    }
    let off = (la - c.mem_base) as u64;
    if off + (len as u64) > c.mem_size as u64 {
        return -1;
    }
    core::ptr::copy_nonoverlapping(c.mem.add(off as usize), dst, len as usize);
    0
}

/// Bounds-checked guest write, same contract.
///
/// # Safety
/// `cpu` must be valid; `src` must provide `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn x86_32_write_guest(
    cpu: *mut X8632Cpu,
    la: u32,
    src: *const u8,
    len: u32,
) -> i32 {
    if cpu.is_null() || src.is_null() {
        return -1;
    }
    let c = &mut *cpu;
    if c.mem.is_null() || la < c.mem_base {
        return -1;
    }
    let off = (la - c.mem_base) as u64;
    if off + (len as u64) > c.mem_size as u64 {
        return -1;
    }
    core::ptr::copy_nonoverlapping(src, c.mem.add(off as usize), len as usize);
    0
}

// ---------------------------------------------------------------------------
// (#rafault) THE BRANCH TRACE: what transferred control, not merely where the
// guest died.
//
// WHY THIS EXISTS. A guest that faults at EIP 0x00000006 with every GPR zero
// has already told you the WHERE and nothing else. The interesting fact is the
// EDGE: which instruction, at which address, sent control to a near-null
// address, and what the stack pointer was when it did. Red Alert derailed
// exactly that way and the STOP block could only report the wreckage.
//
// A per-instruction check is the only place this can live. The core has no
// single "take a branch" funnel: JMP, Jcc, CALL, RET, IRETD and the string
// instructions each move EIP from their own arm, and hooking each one is six
// chances to add a seventh arm later and forget it. Comparing the retired EIP
// against the instruction's own start is ONE site, is exhaustive by
// construction, and cannot go stale when an arm is added.
//
// COST WHEN OFF: one load of BT_ON and a not-taken branch per instruction. The
// golden ships with it off (dosexec.c arms it only under /CONFIG/DOSDIAG.CFG),
// so no shipped run pays for the ring stores.
// ---------------------------------------------------------------------------

const BT_N: usize = 256;

#[derive(Clone, Copy)]
struct BtEnt {
    from: u32,
    to: u32,
    esp: u32,
    n: u32, // insn_count at the transfer, truncated: a run marker, not a total
    op0: u8,
    op1: u8,
}

const BT_ZERO: BtEnt = BtEnt { from: 0, to: 0, esp: 0, n: 0, op0: 0, op1: 0 };

static mut BT: [BtEnt; BT_N] = [BT_ZERO; BT_N];
static mut BT_W: usize = 0;
static mut BT_TOTAL: u64 = 0;
static mut BT_ON: u32 = 0;
/// Targets strictly below this linear address are a DERAIL, latched once.
static mut BT_LOW: u32 = 0;
static mut BT_LATCHED: u32 = 0;
static mut BT_LAT: BtEnt = BT_ZERO;
static mut BT_LAT_CODE: [u8; 16] = [0u8; 16];
static mut BT_LAT_STK: [u32; 8] = [0u32; 8];

/// Arm or disarm the branch trace. `low` = 0 keeps the ring but disables the
/// derail latch.
///
/// # Safety
/// Host-side diagnostic entry point; touches only this module's statics.
#[no_mangle]
pub unsafe extern "C" fn x86_32_btrace(on: u32, low: u32) {
    BT_ON = on;
    BT_LOW = low;
    if on != 0 {
        BT_W = 0;
        BT_TOTAL = 0;
        BT_LATCHED = 0;
        BT = [BT_ZERO; BT_N];
    }
}

/// Is the trace armed? Lets the host skip work it would only throw away.
#[no_mangle]
pub unsafe extern "C" fn x86_32_btrace_on() -> u32 {
    BT_ON
}

#[cold]
unsafe fn bt_latch(x: &Ctx, e: BtEnt) {
    BT_LATCHED = 1;
    BT_LAT = e;
    // The instruction that jumped, and eight stack slots at the landing point.
    // Both are read through the SAME window arithmetic the interpreter uses, so
    // no value here can come from outside the arena.
    let base = x.c.mem_base;
    for k in 0..16u32 {
        let la = e.from.wrapping_add(k);
        let mut b = 0u8;
        if la >= base {
            let o = (la - base) as usize;
            if o < x.mem.len() {
                b = x.mem[o];
            }
        }
        BT_LAT_CODE[k as usize] = b;
    }
    for k in 0..8u32 {
        let la = e.esp.wrapping_add(k * 4);
        let mut v: u32 = 0;
        if la >= base {
            let o = (la - base) as usize;
            if o + 4 <= x.mem.len() {
                v = u32::from_le_bytes([x.mem[o], x.mem[o + 1], x.mem[o + 2], x.mem[o + 3]]);
            }
        }
        BT_LAT_STK[k as usize] = v;
    }
}

#[inline(always)]
unsafe fn bt_record(x: &Ctx, from: u32, to: u32) {
    let base = x.c.mem_base;
    let mut op0 = 0u8;
    let mut op1 = 0u8;
    if from >= base {
        let o = (from - base) as usize;
        if o + 1 < x.mem.len() {
            op0 = x.mem[o];
            op1 = x.mem[o + 1];
        }
    }
    let e = BtEnt { from, to, esp: x.c.regs[4], n: x.c.insn_count as u32, op0, op1 };
    BT[BT_W] = e;
    BT_W = (BT_W + 1) % BT_N;
    BT_TOTAL = BT_TOTAL.wrapping_add(1);
    if BT_LATCHED == 0 && BT_LOW != 0 && to < BT_LOW {
        bt_latch(x, e);
    }
}

/// Print the derail latch and the tail of the ring. `n` = 0 means all of it.
///
/// # Safety
/// Host-side diagnostic entry point.
#[no_mangle]
pub unsafe extern "C" fn x86_32_btrace_dump(n: u32) {
    if BT_ON == 0 {
        return;
    }
    if BT_LATCHED != 0 {
        let e = BT_LAT;
        kprintf(
            b"[BT] DERAIL: 0x%08x -> 0x%08x (below 0x%08x), ESP=%08x at insn %u\n\0".as_ptr(),
            e.from,
            e.to,
            BT_LOW,
            e.esp,
            e.n,
        );
        kprintf(
            b"[BT]   bytes at the branching instruction 0x%08x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n\0"
                .as_ptr(),
            e.from,
            BT_LAT_CODE[0] as u32, BT_LAT_CODE[1] as u32, BT_LAT_CODE[2] as u32,
            BT_LAT_CODE[3] as u32, BT_LAT_CODE[4] as u32, BT_LAT_CODE[5] as u32,
            BT_LAT_CODE[6] as u32, BT_LAT_CODE[7] as u32, BT_LAT_CODE[8] as u32,
            BT_LAT_CODE[9] as u32, BT_LAT_CODE[10] as u32, BT_LAT_CODE[11] as u32,
            BT_LAT_CODE[12] as u32, BT_LAT_CODE[13] as u32, BT_LAT_CODE[14] as u32,
            BT_LAT_CODE[15] as u32,
        );
        kprintf(
            b"[BT]   stack at 0x%08x: %08x %08x %08x %08x %08x %08x %08x %08x\n\0".as_ptr(),
            e.esp,
            BT_LAT_STK[0], BT_LAT_STK[1], BT_LAT_STK[2], BT_LAT_STK[3],
            BT_LAT_STK[4], BT_LAT_STK[5], BT_LAT_STK[6], BT_LAT_STK[7],
        );
    } else {
        kprintf(b"[BT] no derail latched (BT_LOW=0x%08x)\n\0".as_ptr(), BT_LOW);
    }
    let total = BT_TOTAL;
    let have = if total < BT_N as u64 { total as usize } else { BT_N };
    let want = if n == 0 || (n as usize) > have { have } else { n as usize };
    kprintf(
        b"[BT] last %u of %u control transfers (from -> to, ESP at the transfer):\n\0".as_ptr(),
        want as u32,
        total as u32,
    );
    for k in 0..want {
        let idx = (BT_W + BT_N - want + k) % BT_N;
        let e = BT[idx];
        kprintf(
            b"[BT] %5u  %08x -> %08x  esp=%08x  op=%02x %02x  insn=%u\n\0".as_ptr(),
            (total as u32).wrapping_sub((want - k) as u32),
            e.from,
            e.to,
            e.esp,
            e.op0 as u32,
            e.op1 as u32,
            e.n,
        );
    }
}
