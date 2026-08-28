// exec/softfpu.c - see softfpu.h for what this is and why it is not new code.
//
// EVERYTHING BETWEEN THE BANNER BELOW AND `sfp_cmp` WAS CUT FROM
// exec/x86_16.c, NOT REWRITTEN. The only edit is the `sfp_` prefix on the
// seven function names. If you are about to "clean this up", read the header
// comment first: the value of this block is that it is the code Word 6 and
// TETRIS have been running, byte for byte.
#include "softfpu.h"

// ===========================================================================
// MOVED VERBATIM FROM exec/x86_16.c (#211)
// ===========================================================================
// signed 64-bit integer -> IEEE-754 double bits.
uint64_t sfp_fp_from_i64(int64_t v) {
    if (v == 0) return 0;
    uint64_t sign = 0;
    uint64_t mag;
    if (v < 0) { sign = 1ULL << 63; mag = (uint64_t)(-(v + 1)) + 1ULL; }
    else       { mag = (uint64_t)v; }
    // Normalise: find highest set bit.
    int msb = 63;
    while (!((mag >> msb) & 1ULL)) msb--;
    int exp = msb + 1023;
    // mantissa: bits below the implicit leading 1, scaled to 52 bits.
    uint64_t mant;
    if (msb >= 52) mant = mag >> (msb - 52);
    else           mant = mag << (52 - msb);
    mant &= (1ULL << 52) - 1ULL;
    return sign | ((uint64_t)exp << 52) | mant;
}

// IEEE-754 double bits -> signed 64-bit integer (round toward nearest, ties to
// even is not required by these apps; we use round-half-away which matches the
// default FISTP rounding closely enough for pixel geometry).
int64_t sfp_fp_to_i64(uint64_t b) {
    uint64_t sign = b >> 63;
    int exp = (int)((b >> 52) & 0x7FF);
    uint64_t mant = b & ((1ULL << 52) - 1ULL);
    if (exp == 0) return 0;                 // zero / subnormal -> 0
    if (exp == 0x7FF) return 0;             // inf/nan -> 0 (defensive)
    int e = exp - 1023;
    if (e < 0) {
        // |x| < 1: round to nearest integer (0 or 1).
        // value = 1.mant * 2^e ; for e == -1 it is in [0.5,1) -> rounds to 1.
        int64_t r = (e == -1) ? 1 : 0;
        return sign ? -r : r;
    }
    uint64_t full = (1ULL << 52) | mant;    // 53-bit significand (1.mant)
    int64_t r;
    if (e >= 52) {
        if (e - 52 >= 11) return 0;         // overflow guard (>2^63) -> 0
        r = (int64_t)(full << (e - 52));
    } else {
        int sh = 52 - e;
        uint64_t intpart = full >> sh;
        uint64_t frac_top = (full >> (sh - 1)) & 1ULL;   // round bit
        r = (int64_t)(intpart + frac_top);               // round half up
    }
    return sign ? -r : r;
}

// Multiply two IEEE-754 doubles (bit patterns). Integer-only; uses the 128-bit
// integer type (available under -mno-sse, it is not a floating-point feature) to
// hold the 106-bit significand product, then normalises back to a 53-bit double.
uint64_t sfp_fp_mul(uint64_t a, uint64_t b) {
    uint64_t sa = a >> 63, sb = b >> 63, sign = sa ^ sb;
    int ea = (int)((a >> 52) & 0x7FF), eb = (int)((b >> 52) & 0x7FF);
    uint64_t ma = a & ((1ULL << 52) - 1ULL), mb = b & ((1ULL << 52) - 1ULL);
    if (ea == 0 || eb == 0) return sign << 63;   // zero/subnormal operand -> 0
    if (ea == 0x7FF || eb == 0x7FF) return (sign << 63) | (0x7FFULL << 52); // inf
    uint64_t fa = (1ULL << 52) | ma;             // 53-bit significand 1.mant
    uint64_t fb = (1ULL << 52) | mb;
    unsigned __int128 P = (unsigned __int128)fa * (unsigned __int128)fb;  // <=106 bits
    // True value = P / 2^104. Leading 1 sits at bit 104 (value in [1,2)) or
    // bit 105 (value in [2,4)). Find it and renormalise to 1.xxx * 2^exp.
    int msb = 105;
    while (msb >= 0 && !((P >> msb) & 1)) msb--;
    if (msb < 0) return sign << 63;
    int exp = ea + eb - 1023 + (msb - 104);
    uint64_t mant;
    int shift = msb - 52;
    if (shift >= 0) mant = (uint64_t)(P >> shift) & ((1ULL << 52) - 1ULL);
    else            mant = (uint64_t)(P << (-shift)) & ((1ULL << 52) - 1ULL);
    if (exp <= 0)     return sign << 63;                       // underflow -> 0
    if (exp >= 0x7FF) return (sign << 63) | (0x7FFULL << 52);  // overflow -> inf
    return (sign << 63) | ((uint64_t)exp << 52) | mant;
}

// Add (sub=0) or subtract (sub=1) two IEEE-754 doubles, integer-only.
uint64_t sfp_fp_add(uint64_t a, uint64_t b, int sub) {
    uint64_t sa = a >> 63, sb = (b >> 63) ^ (sub ? 1u : 0u);
    int ea = (int)((a >> 52) & 0x7FF), eb = (int)((b >> 52) & 0x7FF);
    uint64_t ma = a & ((1ULL << 52) - 1), mb = b & ((1ULL << 52) - 1);
    if (ea == 0 && ma == 0) return sub ? (b ^ (1ULL << 63)) : b;  // a==0
    if (eb == 0 && mb == 0) return a;                              // b==0
    if (ea == 0x7FF || eb == 0x7FF)
        return (ea == 0x7FF) ? a : (b ^ ((uint64_t)(sub ? 1u : 0u) << 63));
    uint64_t fa = (1ULL << 52) | ma, fb = (1ULL << 52) | mb;
    fa <<= 3; fb <<= 3;                 // 3 guard bits (keeps sums within int64)
    int e;
    if (ea >= eb) { fb >>= (ea - eb); e = ea; }
    else          { fa >>= (eb - ea); e = eb; }
    int64_t va = sa ? -(int64_t)fa : (int64_t)fa;
    int64_t vb = sb ? -(int64_t)fb : (int64_t)fb;
    int64_t r = va + vb;
    uint64_t sign = 0;
    if (r < 0) { sign = 1ULL << 63; r = -r; }
    if (r == 0) return 0;
    uint64_t m = (uint64_t)r;
    int msb = 63; while (!((m >> msb) & 1ULL)) msb--;
    int target = 55;                    // 52 + 3 guard bits
    e += (msb - target);
    if (msb >= target) m >>= (msb - target); else m <<= (target - msb);
    uint64_t mant = (m >> 3) & ((1ULL << 52) - 1);
    if (e <= 0)     return sign;
    if (e >= 0x7FF) return sign | (0x7FFULL << 52);
    return sign | ((uint64_t)e << 52) | mant;
}

// Divide a/b (IEEE-754 doubles), integer-only.
uint64_t sfp_fp_div(uint64_t a, uint64_t b) {
    uint64_t sa = a >> 63, sb = b >> 63, sign = sa ^ sb;
    int ea = (int)((a >> 52) & 0x7FF), eb = (int)((b >> 52) & 0x7FF);
    uint64_t ma = a & ((1ULL << 52) - 1), mb = b & ((1ULL << 52) - 1);
    if (ea == 0) return sign << 63;                       // 0 / x -> 0
    if (eb == 0) return (sign << 63) | (0x7FFULL << 52);  // x / 0 -> inf
    uint64_t fa = (1ULL << 52) | ma, fb = (1ULL << 52) | mb;
    // q = (fa << 53) / fb, computed by restoring binary long division using only
    // 64-bit arithmetic (freestanding has no __udivti3 for 128-bit division).
    // fa,fb are <= 53 bits, so the running remainder never overflows 64 bits.
    uint64_t q = 0, rem = 0;
    for (int bit = 105; bit >= 0; bit--) {           // dividend = fa << 53
        rem <<= 1;
        if (bit >= 53) rem |= (fa >> (bit - 53)) & 1ULL;
        if (rem >= fb) { rem -= fb; if (bit < 64) q |= (1ULL << bit); }
    }
    int msb = 63; while (msb >= 0 && !((q >> msb) & 1ULL)) msb--;
    if (msb < 0) return sign << 63;
    int exp = ea - eb + 1023 + (msb - 53);
    uint64_t mant; int sh = msb - 52;
    if (sh >= 0) mant = (q >> sh) & ((1ULL << 52) - 1);
    else         mant = (q << (-sh)) & ((1ULL << 52) - 1);
    if (exp <= 0)     return sign << 63;
    if (exp >= 0x7FF) return (sign << 63) | (0x7FFULL << 52);
    return (sign << 63) | ((uint64_t)exp << 52) | mant;
}

// IEEE-754 single (32-bit bits) <-> double (64-bit bits) conversions.
uint64_t sfp_f32_to_f64(uint32_t f) {
    uint64_t s = (uint64_t)(f >> 31) << 63;
    int e = (int)((f >> 23) & 0xFF);
    uint64_t mn = f & 0x7FFFFF;
    if (e == 0)    return s;                          // zero/subnormal -> +-0
    if (e == 0xFF) return s | (0x7FFULL << 52) | (mn << 29);
    return s | ((uint64_t)(e - 127 + 1023) << 52) | (mn << 29);
}
uint32_t sfp_f64_to_f32(uint64_t b) {
    uint64_t s = b >> 63;
    int e = (int)((b >> 52) & 0x7FF);
    uint64_t mn = (b >> 29) & 0x7FFFFF;     // top 23 fraction bits (truncate)
    if (e == 0)     return (uint32_t)(s << 31);
    if (e == 0x7FF) return (uint32_t)((s << 31) | (0xFFu << 23) | mn);
    int e32 = e - 1023 + 127;
    if (e32 <= 0)   return (uint32_t)(s << 31);       // underflow -> 0
    if (e32 >= 0xFF) return (uint32_t)((s << 31) | (0xFFu << 23)); // overflow -> inf
    return (uint32_t)((s << 31) | ((uint32_t)e32 << 23) | mn);
}

// ===========================================================================
// NEW HERE (#211): the ordered comparison.
//
// The 16-bit core has never implemented FCOM/FCOMP/FCOMPP in any form, so
// there was no comparison to share and this is the "improve the shared one"
// half of the reuse rule rather than an addition to a private copy. The
// 32-bit core needs it because DJGPP's `__detect_80387` decides whether the
// machine has an FPU by dividing 1 by 0, negating the result and COMPARING
// the two infinities: get the comparison wrong and the answer is "no FPU",
// which sends the C runtime down an emulator-loading path instead.
//
// Bit-pattern ordering, with no arithmetic at all. For binary64 the encoding
// is deliberately monotonic within a sign: a larger unsigned magnitude field
// is a larger value. So the whole comparison is integer.
//
// SUBNORMALS ARE TREATED AS ZERO, matching sfp_fp_mul/sfp_fp_add/sfp_fp_div
// above, which all collapse an exponent of 0 to zero. Disagreeing here would
// mean a value that compares as nonzero and multiplies as zero.
// ===========================================================================
#define SFP_EXP(b)  ((int)(((b) >> 52) & 0x7FF))
#define SFP_MANT(b) ((b) & 0xFFFFFFFFFFFFFULL)

int sfp_cmp(uint64_t a, uint64_t b) {
    if (SFP_EXP(a) == 0x7FF && SFP_MANT(a) != 0) return 2;   // a is NaN
    if (SFP_EXP(b) == 0x7FF && SFP_MANT(b) != 0) return 2;   // b is NaN

    int za = (SFP_EXP(a) == 0);       // zero or subnormal -> zero
    int zb = (SFP_EXP(b) == 0);
    if (za && zb) return 0;           // +0 == -0, and every subnormal with them

    uint64_t sa = a >> 63, sb = b >> 63;
    if (za) return sb ? 1 : -1;       // 0 vs a nonzero: the sign of b decides
    if (zb) return sa ? -1 : 1;
    if (sa != sb) return sa ? -1 : 1; // different signs, neither zero

    uint64_t ma = a & 0x7FFFFFFFFFFFFFFFULL;
    uint64_t mb = b & 0x7FFFFFFFFFFFFFFFULL;
    if (ma == mb) return 0;
    // Same sign: magnitude order, reversed for negatives.
    if (sa) return (ma > mb) ? -1 : 1;
    return (ma > mb) ? 1 : -1;
}

// ---------------------------------------------------------------------------
// The self-test. Literal bit patterns, not expressions computed by the same
// code being tested (which would pass for any consistent-but-wrong encoding).
// ---------------------------------------------------------------------------
#define SFP_ONE   0x3FF0000000000000ULL
#define SFP_TWO   0x4000000000000000ULL
#define SFP_THREE 0x4008000000000000ULL
#define SFP_SIX   0x4018000000000000ULL
#define SFP_TEN   0x4024000000000000ULL
#define SFP_PINF  0x7FF0000000000000ULL
#define SFP_NINF  0xFFF0000000000000ULL
#define SFP_NAN   0x7FF8000000000000ULL
#define SFP_NEG1  0xBFF0000000000000ULL
#define SFP_NEG2  0xC000000000000000ULL

int sfp_selftest(int *out_checks) {
    int bad = 0, n = 0;
#define CK(cond) do { n++; if (!(cond)) bad++; } while (0)

    // int -> binary64
    CK(sfp_fp_from_i64(0) == 0);
    CK(sfp_fp_from_i64(1) == SFP_ONE);
    CK(sfp_fp_from_i64(2) == SFP_TWO);
    CK(sfp_fp_from_i64(-1) == SFP_NEG1);
    CK(sfp_fp_from_i64(10) == SFP_TEN);

    // binary64 -> int
    CK(sfp_fp_to_i64(SFP_ONE) == 1);
    CK(sfp_fp_to_i64(SFP_TEN) == 10);
    CK(sfp_fp_to_i64(SFP_NEG1) == -1);
    CK(sfp_fp_to_i64(0) == 0);

    // arithmetic
    CK(sfp_fp_mul(SFP_TWO, SFP_THREE) == SFP_SIX);
    CK(sfp_fp_add(SFP_ONE, SFP_TWO, 0) == SFP_THREE);
    CK(sfp_fp_add(SFP_THREE, SFP_ONE, 1) == SFP_TWO);
    CK(sfp_fp_div(SFP_SIX, SFP_TWO) == SFP_THREE);

    // THE ONE NETHACK'S FPU PROBE DEPENDS ON: 1/0 must be +infinity, because
    // the probe then negates it and requires the two to compare UNEQUAL.
    CK(sfp_fp_div(SFP_ONE, 0) == SFP_PINF);

    // single <-> double
    CK(sfp_f32_to_f64(0x3F800000u) == SFP_ONE);
    CK(sfp_f64_to_f32(SFP_ONE) == 0x3F800000u);
    CK(sfp_f32_to_f64(0) == 0);

    // comparison
    CK(sfp_cmp(SFP_ONE, SFP_ONE) == 0);
    CK(sfp_cmp(SFP_ONE, SFP_TWO) == -1);
    CK(sfp_cmp(SFP_TWO, SFP_ONE) == 1);
    CK(sfp_cmp(SFP_NEG1, SFP_NEG2) == 1);      // -1 > -2
    CK(sfp_cmp(SFP_NEG2, SFP_NEG1) == -1);
    CK(sfp_cmp(SFP_NEG1, SFP_ONE) == -1);
    CK(sfp_cmp(0, 0x8000000000000000ULL) == 0); // +0 == -0
    CK(sfp_cmp(SFP_ONE, 0) == 1);
    CK(sfp_cmp(0, SFP_ONE) == -1);
    CK(sfp_cmp(0, SFP_NEG1) == 1);
    CK(sfp_cmp(SFP_NAN, SFP_ONE) == 2);
    CK(sfp_cmp(SFP_ONE, SFP_NAN) == 2);
    CK(sfp_cmp(SFP_PINF, SFP_PINF) == 0);
    // ... and the exact pair __detect_80387 forms: -inf must be BELOW +inf.
    CK(sfp_cmp(SFP_NINF, SFP_PINF) == -1);
    CK(sfp_cmp(SFP_PINF, SFP_NINF) == 1);

#undef CK
    if (out_checks) *out_checks = n;
    return bad;
}
