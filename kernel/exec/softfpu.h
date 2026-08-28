// exec/softfpu.h - THE ONE software-IEEE754 arithmetic used by both guest CPU
// cores (#211).
//
// WHY THIS FILE EXISTS, AND WHY IT IS C RATHER THAN RUST.
//
// The kernel is built `-mno-sse -mno-sse2`, i.e. soft-float with SSE disabled
// (kernel/Makefile), so nothing in Ring 0 may use a C `double`. A guest x87
// instruction still has to produce an IEEE-754 result, so the arithmetic is
// done on the raw 64-bit BIT PATTERNS with integer operations only.
//
// That code already existed. It was written for the Win16 layer's software x87
// (exec/x86_16.c, used by TETRIS/Word 6 layout geometry) and lived there as
// seven `static` functions. When the 32-bit DJGPP guest needed x87 (#211,
// NetHack's `__npxsetup` runs FNINIT/FLD1/FLDZ/FDIVP/FCHS/FCOMPP before main),
// the choice was to fork a second implementation or to share this one. This
// project's standing rule says share, so the bodies were MOVED here verbatim
// and only the names changed. exec/x86_16.c `#define`s the old spellings, so
// not one of its call sites moved and the Win16 behaviour cannot have drifted.
//
// It is therefore NOT new kernel code and the Rust-first rule does not apply to
// it: it is an existing, in-use C implementation relocated so that two callers
// can share one copy. Porting it to Rust is a separate strangler step that
// needs a differential against these exact bodies (see RUST_PORT_LEDGER.md);
// doing it in the same change would have put a silent-arithmetic-drift risk
// under a Win16 app nobody in this ticket can regression-test.
//
// `sfp_cmp()` is the one genuinely NEW routine, and it is here rather than in
// the new Rust because it belongs to this primitive: the 16-bit core has never
// implemented FCOM/FCOMPP at all, so adding the comparison HERE is the
// "improve the shared one" half of the rule and the 16-bit core can adopt it.
//
// Representation, in one line: every value is the raw little-endian bit pattern
// of an IEEE-754 binary64, exactly as it would sit in memory.
#ifndef EXEC_SOFTFPU_H
#define EXEC_SOFTFPU_H

#include "../types.h"

// signed 64-bit integer -> binary64 bits
uint64_t sfp_fp_from_i64(int64_t v);
// binary64 bits -> signed 64-bit integer (round half away from zero)
int64_t  sfp_fp_to_i64(uint64_t b);
uint64_t sfp_fp_mul(uint64_t a, uint64_t b);
// add when sub == 0, subtract (a - b) when sub != 0
uint64_t sfp_fp_add(uint64_t a, uint64_t b, int sub);
uint64_t sfp_fp_div(uint64_t a, uint64_t b);
uint64_t sfp_f32_to_f64(uint32_t f);
uint32_t sfp_f64_to_f32(uint64_t b);

// Ordered comparison of two binary64 bit patterns.
//   -1  a <  b
//    0  a == b   (including +0 == -0)
//   +1  a >  b
//    2  UNORDERED: at least one operand is a NaN
// The x87 C3/C2/C0 status triple is derived from this by the caller, because
// which status bits mean what is a property of the INSTRUCTION, not of the
// arithmetic (FCOM and FUCOMI encode the same answer differently).
int sfp_cmp(uint64_t a, uint64_t b);

// Self-test: drives the routines above over fixed vectors whose answers are
// stated as literal bit patterns, so a future edit that changes an exponent
// bias or a rounding rule fails loudly instead of silently moving a guest's
// geometry. Returns the number of FAILING checks (0 = pass) and writes the
// number of checks that RAN to *out_checks, so "passed" and "never ran" are
// distinguishable (#514).
int sfp_selftest(int *out_checks);

#endif // EXEC_SOFTFPU_H
