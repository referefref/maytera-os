// rustkern/x87.rs - the x87 execution unit for the 32-bit guest core (#211).
//
// WHY THIS EXISTS
// ---------------
// `rustkern/x86_32.rs` decoded D8..DF for LENGTH and then MISSed. That is the
// right default (several of those forms carry a push or a pop, and skipping one
// desynchronises the guest's FP stack so the eventual crash is nowhere near its
// cause), but it stops any DJGPP program dead: djgpp's `__npxsetup` runs
// FNINIT / FNSTSW / FNSTCW / FLDCW / FLD1 / FLDZ / FDIVP / FLD / FCHS / FCOMPP
// unconditionally before `main`, whatever the program does afterwards. NetHack
// is integer-only and still cannot reach its first line of C without them.
//
// WHAT IS NEW HERE AND WHAT IS NOT
// --------------------------------
// The ARITHMETIC is not new and is not duplicated: every numeric result comes
// from `exec/softfpu.c`, which is the same code the Win16 layer's software x87
// has been running (see softfpu.h for the move). This module owns the things
// that are genuinely the 32-bit core's own: the register stack, the control and
// status words, the encode/decode of the memory operand formats, and the
// decision of which encodings are implemented.
//
// THE STACK MODEL IS THE 16-BIT CORE'S, DELIBERATELY. ST(0) is `fp[fp_top]`,
// the stack grows DOWNWARD (a push decrements), and `fp_top == 8` means empty.
// Anyone who has read exec/x86_16.c already knows this model; a second, subtly
// different one would be the exact "two copies of one idea" failure the shared
// primitive rule exists to prevent. It also happens to make the architectural
// TOP field free: real x87 TOP counts down from 0 through 7, so `fp_top & 7` IS
// the TOP field, with no translation table to get wrong.
//
// ROUNDING CONTROL IS STORED, NOT HONOURED, and that is stated rather than
// implied: FLDCW records the control word so FNSTCW/FSTCW read back what the
// guest wrote (djgpp's `__control87` does exactly that round trip and would
// otherwise loop or mis-report), but softfpu's helpers round one fixed way.
// A guest that sets round-to-zero and depends on it will get round-to-nearest
// results. No target measured here does; if one ever does, this comment is
// where the discrepancy is already written down.
//
// PRECISION CONTROL is likewise stored and not honoured: every value is a
// binary64, so an 80-bit `long double` loses its extra 11 mantissa bits on the
// way in. FLD m80/FSTP m80 are implemented anyway, because refusing them turns
// a small precision loss into a dead guest.
#![allow(dead_code)]

use crate::x86_32::X8632Cpu;

extern "C" {
    // exec/softfpu.c - THE shared IEEE-754 arithmetic (see softfpu.h).
    fn sfp_fp_from_i64(v: i64) -> u64;
    fn sfp_fp_to_i64(b: u64) -> i64;
    fn sfp_fp_mul(a: u64, b: u64) -> u64;
    fn sfp_fp_add(a: u64, b: u64, sub: i32) -> u64;
    fn sfp_fp_div(a: u64, b: u64) -> u64;
    fn sfp_f32_to_f64(f: u32) -> u64;
    fn sfp_f64_to_f32(b: u64) -> u32;
    fn sfp_cmp(a: u64, b: u64) -> i32;
}

/// Depth of the register stack. Architectural, not a tuning knob.
pub const X87_STACK: u32 = 8;

/// Control word after FNINIT: all exceptions masked, round to nearest,
/// extended precision. 0x037F is the architectural reset value and is what
/// djgpp's `__detect_80387` checks for ((cw & 0x103F) == 0x3F).
pub const X87_CW_RESET: u32 = 0x037F;

// Status-word bits.
const SW_IE: u32 = 0x0001; // invalid operation
const SW_ZE: u32 = 0x0004; // zero divide
const SW_SF: u32 = 0x0040; // stack fault
const SW_ES: u32 = 0x0080; // error summary
const SW_C0: u32 = 0x0100;
const SW_C1: u32 = 0x0200;
const SW_C2: u32 = 0x0400;
const SW_C3: u32 = 0x4000;
const SW_TOP: u32 = 0x3800;

// Constants FLDxx pushes, as literal binary64 bit patterns. Written out rather
// than computed, because computing them would use the very arithmetic they are
// here to be independent of.
const F_ONE: u64 = 0x3FF0_0000_0000_0000; // 1.0
const F_ZERO: u64 = 0x0000_0000_0000_0000; // +0.0
const F_L2T: u64 = 0x400A_934F_0979_A371; // log2(10)
const F_L2E: u64 = 0x3FF7_1547_652B_82FE; // log2(e)
const F_PI: u64 = 0x4009_21FB_5444_2D18; // pi
const F_LG2: u64 = 0x3FD3_4413_509F_79FF; // log10(2)
const F_LN2: u64 = 0x3FE6_2E42_FEFA_39EF; // ln(2)

/// Up to ten bytes of a memory operand, in the layout the encodings use:
/// 2/4 bytes in `lo`; 8 bytes as `lo` (low) + `hi` (high); 10 bytes as a 64-bit
/// significand in `lo`/`hi` plus the sign-and-exponent word in `ex`.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct X87Mem {
    pub lo: u32,
    pub hi: u32,
    pub ex: u32,
}

impl X87Mem {
    pub const ZERO: X87Mem = X87Mem { lo: 0, hi: 0, ex: 0 };
    #[inline]
    fn q(&self) -> u64 {
        ((self.hi as u64) << 32) | (self.lo as u64)
    }
    #[inline]
    fn set_q(&mut self, v: u64) {
        self.lo = (v & 0xFFFF_FFFF) as u32;
        self.hi = (v >> 32) as u32;
    }
}

/// What the caller must do with the memory operand of an x87 instruction.
/// `None` means the encoding has no memory operand or is not implemented.
#[derive(Clone, Copy, PartialEq)]
pub enum X87MemDir {
    /// The caller must READ `n` bytes into X87Mem before calling `mem_form`.
    In(u8),
    /// `mem_form` fills X87Mem and the caller must WRITE `n` bytes after.
    Out(u8),
    /// Not implemented here: the caller should MISS.
    Unhandled,
}

// ---------------------------------------------------------------------------
// Stack helpers
// ---------------------------------------------------------------------------
#[inline]
fn sw_set_flag(c: &mut X8632Cpu, bits: u32) {
    c.fp_sw |= bits;
}

fn stack_underflow(c: &mut X8632Cpu) {
    c.fp_sw |= SW_IE | SW_SF | SW_ES;
    c.fp_sw &= !SW_C1; // C1 = 0 distinguishes underflow from overflow
}

fn stack_overflow(c: &mut X8632Cpu) {
    c.fp_sw |= SW_IE | SW_SF | SW_ES | SW_C1;
}

/// ST(i), or None when that slot is not occupied.
#[inline]
fn st(c: &X8632Cpu, i: usize) -> Option<u64> {
    let idx = c.fp_top as usize + i;
    if idx < X87_STACK as usize {
        Some(c.fp[idx])
    } else {
        None
    }
}

#[inline]
fn st_set(c: &mut X8632Cpu, i: usize, v: u64) -> bool {
    let idx = c.fp_top as usize + i;
    if idx < X87_STACK as usize {
        c.fp[idx] = v;
        true
    } else {
        false
    }
}

fn push(c: &mut X8632Cpu, v: u64) {
    if c.fp_top == 0 {
        stack_overflow(c);
        return;
    }
    c.fp_top -= 1;
    let t = c.fp_top as usize;
    c.fp[t] = v;
}

fn pop(c: &mut X8632Cpu) {
    if c.fp_top >= X87_STACK {
        stack_underflow(c);
        return;
    }
    c.fp_top += 1;
}

/// The status word as the guest sees it: our stored flags plus the
/// architectural TOP field, which `fp_top & 7` already is.
#[inline]
pub fn status_word(c: &X8632Cpu) -> u32 {
    (c.fp_sw & !SW_TOP) | (((c.fp_top & 7) as u32) << 11)
}

/// FNINIT / FINIT / power-on.
pub fn reset(c: &mut X8632Cpu) {
    c.fp_top = X87_STACK;
    c.fp_cw = X87_CW_RESET;
    c.fp_sw = 0;
    let mut i = 0usize;
    while i < X87_STACK as usize {
        c.fp[i] = 0;
        i += 1;
    }
}

// ---------------------------------------------------------------------------
// Arithmetic dispatch: the /digit of the arithmetic escapes.
//   0 ADD  1 MUL  2 COM  3 COMP  4 SUB  5 SUBR  6 DIV  7 DIVR
// ---------------------------------------------------------------------------
fn arith(a: u64, b: u64, rf: usize) -> u64 {
    unsafe {
        match rf {
            0 => sfp_fp_add(a, b, 0),
            1 => sfp_fp_mul(a, b),
            4 => sfp_fp_add(a, b, 1),
            5 => sfp_fp_add(b, a, 1),
            6 => sfp_fp_div(a, b),
            7 => sfp_fp_div(b, a),
            _ => a,
        }
    }
}

/// Set C3/C2/C0 from a comparison of `a` against `b`.
/// The encoding is the architectural one: 000 = a>b, 001 = a<b, 100 = a==b,
/// 111 = unordered.
fn set_compare_flags(c: &mut X8632Cpu, a: u64, b: u64) {
    let r = unsafe { sfp_cmp(a, b) };
    c.fp_sw &= !(SW_C0 | SW_C2 | SW_C3);
    match r {
        -1 => c.fp_sw |= SW_C0,
        0 => c.fp_sw |= SW_C3,
        1 => {}
        _ => {
            c.fp_sw |= SW_C0 | SW_C2 | SW_C3;
            c.fp_sw |= SW_IE | SW_ES;
        }
    }
}

/// Same comparison, but written into EFLAGS the way FCOMI/FUCOMI do
/// (ZF=C3, PF=C2, CF=C0), which is what a compiler-emitted compare uses.
fn set_compare_eflags(c: &mut X8632Cpu, a: u64, b: u64) {
    let r = unsafe { sfp_cmp(a, b) };
    // EFLAGS bit positions: CF 0, PF 2, ZF 6.
    c.eflags &= !(0x0001 | 0x0004 | 0x0040);
    match r {
        -1 => c.eflags |= 0x0001,
        0 => c.eflags |= 0x0040,
        1 => {}
        _ => {
            c.eflags |= 0x0001 | 0x0004 | 0x0040;
            c.fp_sw |= SW_IE | SW_ES;
        }
    }
}

// ---------------------------------------------------------------------------
// 80-bit extended <-> binary64.
//
// The 80-bit format is sign(1) exponent(15) EXPLICIT-integer-bit(1)
// fraction(63). binary64 has an IMPLICIT leading 1, which is the only
// structural difference and the one that a naive shift gets wrong.
// ---------------------------------------------------------------------------
fn f80_to_f64(sig: u64, se: u32) -> u64 {
    let sign = ((se >> 15) & 1) as u64;
    let exp80 = (se & 0x7FFF) as i32;
    if exp80 == 0 {
        return sign << 63; // zero or 80-bit subnormal -> signed zero
    }
    if exp80 == 0x7FFF {
        // Inf or NaN. Keep the quiet bit so a NaN stays a NaN.
        let frac = (sig >> 11) & 0xF_FFFF_FFFF_FFFF;
        return (sign << 63) | (0x7FFu64 << 52) | frac;
    }
    let e = exp80 - 16383 + 1023;
    if e <= 0 {
        return sign << 63; // underflows binary64
    }
    if e >= 0x7FF {
        return (sign << 63) | (0x7FFu64 << 52); // overflows to infinity
    }
    // Drop the explicit integer bit (bit 63) and truncate to 52 fraction bits.
    let frac = (sig << 1) >> 12;
    (sign << 63) | ((e as u64) << 52) | (frac & 0xF_FFFF_FFFF_FFFF)
}

fn f64_to_f80(b: u64) -> (u64, u32) {
    let sign = (b >> 63) as u32;
    let exp = ((b >> 52) & 0x7FF) as i32;
    let frac = b & 0xF_FFFF_FFFF_FFFF;
    if exp == 0 {
        return (0, sign << 15); // signed zero (subnormals are zero here)
    }
    if exp == 0x7FF {
        return ((1u64 << 63) | (frac << 11), (sign << 15) | 0x7FFF);
    }
    let e80 = (exp - 1023 + 16383) as u32;
    // Restore the implicit 1 as the explicit integer bit.
    let sig = (1u64 << 63) | (frac << 11);
    (sig, (sign << 15) | (e80 & 0x7FFF))
}

// ---------------------------------------------------------------------------
// Register forms (mod == 3). Returns false if the encoding is not implemented,
// in which case the caller MISSes and the guest stops on the instruction.
// ---------------------------------------------------------------------------
pub fn reg_form(c: &mut X8632Cpu, op: u8, modrm: u8) -> bool {
    let reg = ((modrm >> 3) & 7) as usize;
    let i = (modrm & 7) as usize;

    match op {
        0xD8 => {
            // ST(0) <- ST(0) op ST(i); /2 and /3 are compares.
            let (a, b) = match (st(c, 0), st(c, i)) {
                (Some(a), Some(b)) => (a, b),
                _ => {
                    stack_underflow(c);
                    return true;
                }
            };
            match reg {
                2 | 3 => {
                    set_compare_flags(c, a, b);
                    if reg == 3 {
                        pop(c);
                    }
                }
                _ => {
                    let v = arith(a, b, reg);
                    st_set(c, 0, v);
                }
            }
            true
        }

        0xD9 => match modrm {
            0xC0..=0xC7 => {
                // FLD ST(i). Read BEFORE the push, because the push moves TOP
                // and would renumber ST(i) underneath us.
                match st(c, i) {
                    Some(v) => push(c, v),
                    None => stack_underflow(c),
                }
                true
            }
            0xC8..=0xCF => {
                // FXCH ST(i)
                match (st(c, 0), st(c, i)) {
                    (Some(a), Some(b)) => {
                        st_set(c, 0, b);
                        st_set(c, i, a);
                    }
                    _ => stack_underflow(c),
                }
                true
            }
            0xD0 => true, // FNOP
            0xE0 => {
                // FCHS
                match st(c, 0) {
                    Some(v) => {
                        st_set(c, 0, v ^ (1u64 << 63));
                    }
                    None => stack_underflow(c),
                }
                true
            }
            0xE1 => {
                // FABS
                match st(c, 0) {
                    Some(v) => {
                        st_set(c, 0, v & 0x7FFF_FFFF_FFFF_FFFF);
                    }
                    None => stack_underflow(c),
                }
                true
            }
            0xE4 => {
                // FTST: compare ST(0) with +0.0
                match st(c, 0) {
                    Some(v) => set_compare_flags(c, v, F_ZERO),
                    None => stack_underflow(c),
                }
                true
            }
            0xE5 => {
                // FXAM. Only the cases a real program acts on are classified:
                // empty, zero, infinity, NaN, and "normal finite".
                c.fp_sw &= !(SW_C0 | SW_C1 | SW_C2 | SW_C3);
                match st(c, 0) {
                    None => {
                        c.fp_sw |= SW_C3 | SW_C0; // empty
                    }
                    Some(v) => {
                        if v >> 63 != 0 {
                            c.fp_sw |= SW_C1; // sign
                        }
                        let e = ((v >> 52) & 0x7FF) as u32;
                        let m = v & 0xF_FFFF_FFFF_FFFF;
                        if e == 0x7FF {
                            if m == 0 {
                                c.fp_sw |= SW_C2 | SW_C0; // infinity
                            } else {
                                c.fp_sw |= SW_C0; // NaN
                            }
                        } else if e == 0 {
                            c.fp_sw |= SW_C3; // zero
                        } else {
                            c.fp_sw |= SW_C2; // normal finite
                        }
                    }
                }
                true
            }
            0xE8 => {
                push(c, F_ONE);
                true
            }
            0xE9 => {
                push(c, F_L2T);
                true
            }
            0xEA => {
                push(c, F_L2E);
                true
            }
            0xEB => {
                push(c, F_PI);
                true
            }
            0xEC => {
                push(c, F_LG2);
                true
            }
            0xED => {
                push(c, F_LN2);
                true
            }
            0xEE => {
                push(c, F_ZERO);
                true
            }
            // F0..FF are the transcendentals (F2XM1, FYL2X, FPTAN, FSQRT,
            // FSIN, FCOS, FSCALE, FPREM, FRNDINT...). softfpu has no series
            // evaluation and inventing one here would be a second, worse copy
            // of an arithmetic library. They MISS, which stops the guest ON
            // the instruction and names it.
            _ => false,
        },

        0xDA => {
            if modrm == 0xE9 {
                // FUCOMPP
                match (st(c, 0), st(c, 1)) {
                    (Some(a), Some(b)) => set_compare_flags(c, a, b),
                    _ => stack_underflow(c),
                }
                pop(c);
                pop(c);
                return true;
            }
            false // FCMOVcc
        }

        0xDB => match modrm {
            0xE0 | 0xE1 | 0xE4 => true, // FNENI / FNDISI / FNSETPM: 8087-era no-ops
            0xE2 => {
                // FNCLEX: clear the exception flags, keep TOP and the C bits.
                c.fp_sw &= !(SW_IE | SW_ZE | SW_SF | SW_ES);
                true
            }
            0xE3 => {
                reset(c);
                true
            }
            0xE8..=0xEF | 0xF0..=0xF7 => {
                // FCMOVcc share this space on P6, but with mod==3 and reg 5/6
                // these are FUCOMI/FCOMI ST(0),ST(i): compare into EFLAGS.
                match (st(c, 0), st(c, i)) {
                    (Some(a), Some(b)) => set_compare_eflags(c, a, b),
                    _ => {
                        stack_underflow(c);
                        set_compare_eflags(c, 0, 0);
                    }
                }
                true
            }
            _ => false,
        },

        0xDC => {
            // ST(i) <- ST(i) op ST(0). The SUB/SUBR and DIV/DIVR senses are
            // SWAPPED relative to D8 in this encoding; getting that backwards
            // is silent and produces a plausible wrong number.
            let (a, b) = match (st(c, i), st(c, 0)) {
                (Some(a), Some(b)) => (a, b),
                _ => {
                    stack_underflow(c);
                    return true;
                }
            };
            match reg {
                2 | 3 => {
                    set_compare_flags(c, b, a);
                    if reg == 3 {
                        pop(c);
                    }
                }
                _ => {
                    let rf = swap_sense(reg);
                    let v = arith(a, b, rf);
                    st_set(c, i, v);
                }
            }
            true
        }

        0xDD => match reg {
            0 => {
                // FFREE ST(i): mark it empty. With a downward stack the only
                // honest cheap model is "no observable effect unless it is
                // ST(0)", and no measured guest depends on more.
                true
            }
            2 | 3 => {
                // FST / FSTP ST(i)
                match st(c, 0) {
                    Some(v) => {
                        st_set(c, i, v);
                    }
                    None => stack_underflow(c),
                }
                if reg == 3 {
                    pop(c);
                }
                true
            }
            4 | 5 => {
                // FUCOM / FUCOMP ST(i)
                match (st(c, 0), st(c, i)) {
                    (Some(a), Some(b)) => set_compare_flags(c, a, b),
                    _ => stack_underflow(c),
                }
                if reg == 5 {
                    pop(c);
                }
                true
            }
            _ => false,
        },

        0xDE => {
            if modrm == 0xD9 {
                // FCOMPP: compare ST(0) with ST(1), pop both. THE instruction
                // djgpp's FPU probe ends on.
                match (st(c, 0), st(c, 1)) {
                    (Some(a), Some(b)) => set_compare_flags(c, a, b),
                    _ => stack_underflow(c),
                }
                pop(c);
                pop(c);
                return true;
            }
            // FADDP / FMULP / FSUBP / FSUBRP / FDIVP / FDIVRP ST(i), ST(0)
            let (a, b) = match (st(c, i), st(c, 0)) {
                (Some(a), Some(b)) => (a, b),
                _ => {
                    stack_underflow(c);
                    return true;
                }
            };
            if reg == 2 || reg == 3 {
                return false; // FCOMP with mod==3 and reg 2/3 is not encodable
            }
            let rf = swap_sense(reg);
            let v = arith(a, b, rf);
            st_set(c, i, v);
            pop(c);
            true
        }

        0xDF => match modrm {
            0xE0 => {
                // FNSTSW AX. Writes AX only; the high half of EAX is a guest
                // value that must survive.
                let sw = status_word(c);
                c.regs[0] = (c.regs[0] & 0xFFFF_0000) | (sw & 0xFFFF);
                true
            }
            0xE8..=0xEF | 0xF0..=0xF7 => {
                // FUCOMIP / FCOMIP ST(0), ST(i)
                match (st(c, 0), st(c, i)) {
                    (Some(a), Some(b)) => set_compare_eflags(c, a, b),
                    _ => {
                        stack_underflow(c);
                        set_compare_eflags(c, 0, 0);
                    }
                }
                pop(c);
                true
            }
            _ => false,
        },

        _ => false,
    }
}

#[inline]
fn swap_sense(reg: usize) -> usize {
    match reg {
        4 => 5,
        5 => 4,
        6 => 7,
        7 => 6,
        r => r,
    }
}

// ---------------------------------------------------------------------------
// Memory forms. Two calls: the caller asks what to do with the operand, then
// performs the read (or the write) around `mem_form`.
// ---------------------------------------------------------------------------
pub fn mem_dir(op: u8, reg: usize) -> X87MemDir {
    match op {
        0xD8 => X87MemDir::In(4),  // m32 real, all eight /digits
        0xDA => X87MemDir::In(4),  // m32 int
        0xDC => X87MemDir::In(8),  // m64 real
        0xDE => X87MemDir::In(2),  // m16 int
        0xD9 => match reg {
            0 => X87MemDir::In(4),   // FLD m32
            2 | 3 => X87MemDir::Out(4), // FST/FSTP m32
            5 => X87MemDir::In(2),   // FLDCW
            7 => X87MemDir::Out(2),  // FNSTCW
            _ => X87MemDir::Unhandled, // FLDENV / FNSTENV
        },
        0xDB => match reg {
            0 => X87MemDir::In(4),      // FILD m32
            1 => X87MemDir::Out(4),     // FISTTP m32
            2 | 3 => X87MemDir::Out(4), // FIST/FISTP m32
            5 => X87MemDir::In(10),     // FLD m80
            7 => X87MemDir::Out(10),    // FSTP m80
            _ => X87MemDir::Unhandled,
        },
        0xDD => match reg {
            0 => X87MemDir::In(8),      // FLD m64
            2 | 3 => X87MemDir::Out(8), // FST/FSTP m64
            7 => X87MemDir::Out(2),     // FNSTSW m16
            _ => X87MemDir::Unhandled,  // FRSTOR / FNSAVE
        },
        0xDF => match reg {
            0 => X87MemDir::In(2),      // FILD m16
            2 | 3 => X87MemDir::Out(2), // FIST/FISTP m16
            5 => X87MemDir::In(8),      // FILD m64
            7 => X87MemDir::Out(8),     // FISTP m64
            _ => X87MemDir::Unhandled,  // FBLD / FBSTP (packed BCD)
        },
        _ => X87MemDir::Unhandled,
    }
}

/// Execute a memory-operand x87 instruction. For `In` forms `m` already holds
/// the operand; for `Out` forms this fills it and the caller stores it.
/// Returns false only for encodings `mem_dir` reported as Unhandled.
pub fn mem_form(c: &mut X8632Cpu, op: u8, reg: usize, m: &mut X87Mem) -> bool {
    match op {
        0xD8 => {
            let b = unsafe { sfp_f32_to_f64(m.lo) };
            arith_mem(c, b, reg);
            true
        }
        0xDC => {
            let b = m.q();
            arith_mem(c, b, reg);
            true
        }
        0xDA => {
            let b = unsafe { sfp_fp_from_i64((m.lo as i32) as i64) };
            arith_mem(c, b, reg);
            true
        }
        0xDE => {
            let b = unsafe { sfp_fp_from_i64(((m.lo & 0xFFFF) as u16 as i16) as i64) };
            arith_mem(c, b, reg);
            true
        }

        0xD9 => match reg {
            0 => {
                let v = unsafe { sfp_f32_to_f64(m.lo) };
                push(c, v);
                true
            }
            2 | 3 => {
                let v = st(c, 0).unwrap_or_else(|| {
                    stack_underflow(c);
                    0
                });
                m.lo = unsafe { sfp_f64_to_f32(v) };
                if reg == 3 {
                    pop(c);
                }
                true
            }
            5 => {
                // FLDCW. See the header: stored, not honoured.
                c.fp_cw = m.lo & 0xFFFF;
                true
            }
            7 => {
                m.lo = c.fp_cw & 0xFFFF;
                true
            }
            _ => false,
        },

        0xDB => match reg {
            0 => {
                let v = unsafe { sfp_fp_from_i64((m.lo as i32) as i64) };
                push(c, v);
                true
            }
            1 | 2 | 3 => {
                let v = st(c, 0).unwrap_or_else(|| {
                    stack_underflow(c);
                    0
                });
                let i = unsafe { sfp_fp_to_i64(v) };
                m.lo = i as u32;
                if reg != 2 {
                    pop(c);
                }
                true
            }
            5 => {
                let v = f80_to_f64(m.q(), m.ex & 0xFFFF);
                push(c, v);
                true
            }
            7 => {
                let v = st(c, 0).unwrap_or_else(|| {
                    stack_underflow(c);
                    0
                });
                let (sig, se) = f64_to_f80(v);
                m.set_q(sig);
                m.ex = se;
                pop(c);
                true
            }
            _ => false,
        },

        0xDD => match reg {
            0 => {
                let v = m.q();
                push(c, v);
                true
            }
            2 | 3 => {
                let v = st(c, 0).unwrap_or_else(|| {
                    stack_underflow(c);
                    0
                });
                m.set_q(v);
                if reg == 3 {
                    pop(c);
                }
                true
            }
            7 => {
                m.lo = status_word(c) & 0xFFFF;
                true
            }
            _ => false,
        },

        0xDF => match reg {
            0 => {
                let v = unsafe { sfp_fp_from_i64(((m.lo & 0xFFFF) as u16 as i16) as i64) };
                push(c, v);
                true
            }
            2 | 3 => {
                let v = st(c, 0).unwrap_or_else(|| {
                    stack_underflow(c);
                    0
                });
                let i = unsafe { sfp_fp_to_i64(v) };
                m.lo = (i as u32) & 0xFFFF;
                if reg == 3 {
                    pop(c);
                }
                true
            }
            5 => {
                let v = unsafe { sfp_fp_from_i64(m.q() as i64) };
                push(c, v);
                true
            }
            7 => {
                let v = st(c, 0).unwrap_or_else(|| {
                    stack_underflow(c);
                    0
                });
                let i = unsafe { sfp_fp_to_i64(v) };
                m.set_q(i as u64);
                pop(c);
                true
            }
            _ => false,
        },

        _ => false,
    }
}

fn arith_mem(c: &mut X8632Cpu, b: u64, reg: usize) {
    let a = match st(c, 0) {
        Some(a) => a,
        None => {
            stack_underflow(c);
            return;
        }
    };
    match reg {
        2 | 3 => {
            set_compare_flags(c, a, b);
            if reg == 3 {
                pop(c);
            }
        }
        _ => {
            let v = arith(a, b, reg);
            st_set(c, 0, v);
        }
    }
}

// ---------------------------------------------------------------------------
// Self-test. It runs the EXACT instruction sequence djgpp's `__detect_80387`
// executes, because that sequence is the reason this module exists and a
// module that passes a set of invented vectors while failing the one real
// caller has proved nothing (#514).
//
// Returns the number of FAILING checks; *out_checks receives how many RAN.
// ---------------------------------------------------------------------------
#[no_mangle]
pub unsafe extern "C" fn x87_selftest_rs(cpu: *mut X8632Cpu, out_checks: *mut u32) -> u32 {
    if cpu.is_null() {
        if !out_checks.is_null() {
            unsafe { *out_checks = 0 };
        }
        return 1;
    }
    let c = unsafe { &mut *cpu };
    let mut bad = 0u32;
    let mut n = 0u32;
    macro_rules! ck {
        ($cond:expr) => {{
            n += 1;
            if !($cond) {
                bad += 1;
            }
        }};
    }

    // --- the reset state djgpp checks for -------------------------------
    reset(c);
    ck!(c.fp_top == X87_STACK);
    ck!(status_word(c) == 0); // low byte 0: `cmp byte ptr [esi],0` must pass
    ck!(c.fp_cw == 0x037F);
    ck!((c.fp_cw & 0x103F) == 0x3F); // the exact mask __detect_80387 applies

    // --- FLD1; FLDZ; FDIVP st(1),st  ->  ST(0) = +inf --------------------
    reg_form(c, 0xD9, 0xE8); // FLD1
    ck!(c.fp_top == 7);
    ck!(st(c, 0) == Some(F_ONE));
    reg_form(c, 0xD9, 0xEE); // FLDZ
    ck!(c.fp_top == 6);
    ck!(st(c, 0) == Some(F_ZERO));
    reg_form(c, 0xDE, 0xF9); // FDIVP st(1),st
    ck!(c.fp_top == 7);
    ck!(st(c, 0) == Some(0x7FF0_0000_0000_0000)); // +infinity

    // --- FLD st(0); FCHS  ->  ST(0) = -inf, ST(1) = +inf -----------------
    reg_form(c, 0xD9, 0xC0); // FLD ST(0)
    ck!(c.fp_top == 6);
    reg_form(c, 0xD9, 0xE0); // FCHS
    ck!(st(c, 0) == Some(0xFFF0_0000_0000_0000));
    ck!(st(c, 1) == Some(0x7FF0_0000_0000_0000));

    // --- FCOMPP: the two infinities must compare UNEQUAL ------------------
    // C3 lands in ZF through the guest's `fstsw; sahf; je`. C3 SET here would
    // make djgpp conclude "no FPU" and go looking for an emulator DXE.
    reg_form(c, 0xDE, 0xD9);
    ck!((c.fp_sw & SW_C3) == 0);
    ck!((c.fp_sw & SW_C0) != 0); // -inf < +inf
    ck!(c.fp_top == X87_STACK); // both popped: the stack is empty again
    let sw = status_word(c);
    ck!(((sw >> 8) & 0x40) == 0); // the SAHF ZF bit the probe branches on
    ck!(((sw >> 8) & 0x01) != 0); // the SAHF CF bit

    // --- __control87(0x33f, 0xffff): FSTCW, mask, FLDCW, FSTCW -----------
    reset(c);
    let mut m = X87Mem::ZERO;
    mem_form(c, 0xD9, 7, &mut m); // FNSTCW
    ck!(m.lo == 0x037F);
    m.lo = 0x033F;
    mem_form(c, 0xD9, 5, &mut m); // FLDCW
    m.lo = 0;
    mem_form(c, 0xD9, 7, &mut m); // FNSTCW again
    ck!(m.lo == 0x033F); // it must read back what was written

    // --- FNSTSW m16 after FNINIT is 0, which is check one of the probe ----
    reset(c);
    m.lo = 0x5A5A;
    mem_form(c, 0xDD, 7, &mut m);
    ck!(m.lo == 0);
    ck!((m.lo & 0xFF) == 0);

    // --- ordinary loads and stores ---------------------------------------
    reset(c);
    m = X87Mem::ZERO;
    m.lo = 0x3F80_0000; // 1.0f
    mem_form(c, 0xD9, 0, &mut m); // FLD m32
    ck!(st(c, 0) == Some(F_ONE));
    m = X87Mem::ZERO;
    mem_form(c, 0xDD, 2, &mut m); // FST m64
    ck!(m.q() == F_ONE);
    m.lo = 0;
    m.hi = 0;
    m.lo = 7;
    mem_form(c, 0xDB, 0, &mut m); // FILD m32 (7)
    ck!(c.fp_top == 6);
    m = X87Mem::ZERO;
    mem_form(c, 0xDB, 3, &mut m); // FISTP m32
    ck!(m.lo == 7);
    ck!(c.fp_top == 7);

    // --- FADD/FMUL/FSUB/FDIV through the shared arithmetic ---------------
    reset(c);
    m = X87Mem::ZERO;
    m.set_q(0x4008_0000_0000_0000); // 3.0
    mem_form(c, 0xDD, 0, &mut m); // FLD m64
    m.set_q(0x4000_0000_0000_0000); // 2.0
    mem_form(c, 0xDC, 1, &mut m); // FMUL m64 -> 6.0
    ck!(st(c, 0) == Some(0x4018_0000_0000_0000));
    m.set_q(0x4000_0000_0000_0000);
    mem_form(c, 0xDC, 6, &mut m); // FDIV m64 -> 3.0
    ck!(st(c, 0) == Some(0x4008_0000_0000_0000));
    m.set_q(0x4008_0000_0000_0000);
    mem_form(c, 0xDC, 4, &mut m); // FSUB m64 -> 0.0
    ck!(st(c, 0) == Some(0));

    // --- FSUBR/FDIVR sense, and the DC/DE swap that is easy to invert -----
    reset(c);
    m.set_q(0x4000_0000_0000_0000); // 2.0
    mem_form(c, 0xDD, 0, &mut m); // ST0 = 2.0
    m.set_q(0x4018_0000_0000_0000); // 6.0
    mem_form(c, 0xDC, 7, &mut m); // FDIVR m64: ST0 = 6.0/2.0 = 3.0
    ck!(st(c, 0) == Some(0x4008_0000_0000_0000));

    // --- 80-bit round trip -------------------------------------------------
    reset(c);
    m = X87Mem::ZERO;
    m.set_q(F_ONE);
    mem_form(c, 0xDD, 0, &mut m); // FLD 1.0
    m = X87Mem::ZERO;
    mem_form(c, 0xDB, 7, &mut m); // FSTP m80
    ck!(m.ex == 0x3FFF); // exponent of 1.0 in the 80-bit bias
    ck!(m.q() == 0x8000_0000_0000_0000); // explicit integer bit, zero fraction
    mem_form(c, 0xDB, 5, &mut m); // FLD m80
    ck!(st(c, 0) == Some(F_ONE));

    // --- the stack really is eight deep and overflow is not silent --------
    reset(c);
    let mut k = 0;
    while k < 8 {
        reg_form(c, 0xD9, 0xE8); // FLD1 x8
        k += 1;
    }
    ck!(c.fp_top == 0);
    ck!((c.fp_sw & SW_SF) == 0);
    reg_form(c, 0xD9, 0xE8); // the ninth
    ck!((c.fp_sw & SW_SF) != 0);
    ck!(c.fp_top == 0); // and it did NOT wrap round and corrupt ST(7)

    // --- an unimplemented transcendental must report NOT HANDLED ----------
    reset(c);
    ck!(reg_form(c, 0xD9, 0xFA) == false); // FSQRT
    ck!(reg_form(c, 0xD9, 0xFE) == false); // FSIN
    ck!(matches!(mem_dir(0xD9, 6), X87MemDir::Unhandled)); // FNSTENV
    ck!(matches!(mem_dir(0xD9, 0), X87MemDir::In(4)));
    ck!(matches!(mem_dir(0xDD, 3), X87MemDir::Out(8)));

    reset(c);
    if !out_checks.is_null() {
        unsafe { *out_checks = n };
    }
    bad
}
