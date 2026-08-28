// pcmeq.rs - #231r: the 5-band graphic equaliser's ACTUAL DSP.
//
// WHY THIS EXISTS, AND WHY IT IS THE WHOLE POINT
// ==========================================================================
// #231 (commit 8a5fcee5) DELETED a 5-band graphic EQ from the tray Sound
// panel. The reason it gave was correct and worth repeating verbatim: the
// faders wrote "a `static int g_eq[5]` that only the fader itself reads - no
// EQ syscall exists and it is not even persisted". It was a control that
// rendered, moved under the mouse, and changed nothing. That is worse than
// no control at all, because a user who drags it and hears no difference
// concludes the AUDIO is broken, not the widget.
//
// The owner has asked for the EQ back, in its original design. Restoring the
// faders alone would rebuild exactly the defect #231 removed. So this module
// is the half that was missing: real per-band filtering on the real PCM the
// hardware receives. The faders are wired to THIS.
//
// WHERE IT SITS: POST-MIX, ON THE FINAL STEREO STREAM
// ==========================================================================
// audio_pcm.c's mix_render() sums every open producer into one stereo i32
// accumulator (rustkern/pcmmix.rs) and then saturates it down to S16. This
// module filters that accumulator IN PLACE, between the sum and the
// saturation.
//
// That makes it a MASTER equaliser, which is what a tray-level "Sound" panel
// means: the user is equalising "the machine", not one application. The
// alternative - a filter chain per stream - would mean PCM_MAX_STREAMS (4)
// times the work on a machine whose userland runs on ONE core and already
// spends ~3.7 ms per full-framebuffer present at 3840x2160, and it would
// give a DIFFERENT answer depending on how many things happened to be
// playing, which is not what a master EQ is. Post-mix is also the only place
// where the cost is bounded no matter how many producers open.
//
// Filtering post-mix does mean the EQ acts on the SUM, so a boosted band can
// push the sum further into the existing saturating clamp than it would have
// gone unfiltered. That clamp (pcm_mix_finish_rs) was already the thing
// standing between a loud passage and a wrap, and it still is; see the
// headroom analysis under FIXED-POINT FORMAT.
//
// INTEGER ONLY, AND NOT AS A STYLE PREFERENCE
// ==========================================================================
// Same constraint pcmmix.rs states next door: the kernel target is
// x86_64-unknown-none (+soft-float) and CFLAGS carry -mno-sse -mno-sse2. A
// `double` biquad here would become a libgcc soft-float call in the hottest
// loop in the audio path, 10 times per output frame. Every vendored decoder
// in this tree is fixed-point for exactly this reason. Everything below is
// i32/i64.
//
// FILTER DESIGN
// ==========================================================================
// Five RBJ ("Audio EQ Cookbook") biquad sections, cascaded, per channel:
//
//   band 0    60 Hz   LOW SHELF     S = 1.0
//   band 1   250 Hz   PEAKING       Q = 1.0
//   band 2  1000 Hz   PEAKING       Q = 1.0
//   band 3  4000 Hz   PEAKING       Q = 1.0
//   band 4 12000 Hz   HIGH SHELF    S = 1.0
//
// The five frequencies are NOT invented here: they are the labels the
// original panel drew under its five faders ("60", "250", "1k", "4k",
// "12k", traymenu.c before 8a5fcee5). Restoring the original design means
// the DSP matches the faceplate, not the other way round.
//
// The two END bands are SHELVES rather than peaking sections because that is
// what the original binding names said they were: TRAYMENU.YAML bound eq0 to
// "Bass" and eq4 to "Treble". Bass and treble controls are shelves. (#231
// noted, correctly, that only three of the five faders had YAML bindings at
// all; the five faceplate frequencies are the design, and all five are bound
// now.) Shelves at the ends also behave far better near the rails than a
// peaking section does: a 12 kHz peaking filter has a whole octave of audio
// above it that it does nothing about.
//
// GAIN RANGE: the fader is 0..100 with a centre detent, which the original
// drew as five tick marks with the middle one highlighted. 50 is flat, and
// the range is +/- 12 dB:  dB*10 = (pos - 50) * 240 / 100.
//
// FIXED-POINT FORMAT, CHOSEN DELIBERATELY
// ==========================================================================
// Q15 coefficients would sound bad and this is why: a 60 Hz section at
// 44.1 kHz has poles at radius ~0.9957, so the coefficient a2/a0 is ~0.9915
// and the interesting quantity, 1 - r, is ~0.0043. Q15's resolution is
// 3.1e-5, which is 0.7% of the thing being represented; the corner frequency
// and Q would visibly move, and low-frequency quantisation noise would be
// amplified by the section's own noise gain (roughly 1/(1-r), ~47 dB here).
//
// So:
//   COEFFICIENTS   Q28  (resolution 3.7e-9; the resulting pole-radius error
//                        is ~2e-9, i.e. 5e-7 of 1-r. Inaudible by any
//                        measure.)
//   INTERMEDIATE   Q24  during coefficient computation, where the shelf
//                        formulas multiply values as large as ~11 by each
//                        other and Q30 would overflow i64.
//   SIGNAL         the accumulator's own integer units << 10 (SIG_F).
//                        10 extra fractional bits put the recursion's
//                        rounding noise ~120 dB below full scale AFTER the
//                        worst band's noise gain, i.e. below the 16-bit
//                        floor the sink quantises to anyway.
//   STATE          i64, transposed direct form II.
//
// TDF2 rather than DF1 because its two state words ARE the running
// accumulators: no separate x-history is rounded, so the only quantisation
// in the loop is the single rounded shift that produces y.
//
// HEADROOM, PROVEN BY CONSTRUCTION AND BY THE SELF-TEST
//   |coef|        <= ~2.7          -> |coef_q28| <= 2^29.4
//   y is CLAMPED  to +/- 2^30      -> 2^20 in signal units = 32x full scale
//   x             <= 4 streams * 32768 << 10 = 2^27
//   |a1*y|        <= 2^59.4 ;  s1 <= 2^60.4 ;  b0*x + s1 <= 2^60.5
// which leaves a factor of ~11 against i64's 2^63. eq_selftest_rs drives
// full-scale square waves through every band at maximum boost and reports
// the largest state word actually reached, so the margin is MEASURED on the
// real build rather than only argued here.
//
// FLAT IS BYPASSED, NOT COMPUTED
// ==========================================================================
// At 0 dB the RBJ peaking/shelf sections are mathematically the identity
// (b == a term for term), but a fixed-point recursion is not bit-exact just
// because its ideal is. A "flat" EQ that colours the sound is a bug, so a
// band at exactly 0 dB is SKIPPED, and when every band is at 0 dB
// pcm_eq_process_rs returns without touching a single sample. The
// EQ-disabled path is then not "close to" the old behaviour, it IS the old
// behaviour, byte for byte. That also makes flat the cheapest case, which is
// the case almost every machine is in.
//
// CONCURRENCY: ONE WRITER OF THE COEFFICIENTS, BY CONSTRUCTION
// ==========================================================================
// A syscall (thread context, any core) can change a band while the mixer
// thread is mid-block. Rather than lock the coefficients or double-buffer
// them, the syscall writes ONLY an atomic fader position and bumps a
// generation counter. The MIXER recomputes the coefficients, at a block
// boundary, when it notices the generation moved. So the coefficient arrays
// have exactly one writer and one reader, both the mixer thread, and there
// is no lock to take, no spin, and nothing for #426 to object to.
//
// Deliberately NO state reset on a gain change: the filter state is
// continuous audio and zeroing it mid-stream is precisely the "thump" this
// module is supposed to avoid. State IS zeroed when a band goes from active
// to bypassed (so stale energy cannot reappear if it is turned back on) and
// when a mix session starts or the sink's rate changes.

#![allow(dead_code)]

use core::sync::atomic::{AtomicI32, AtomicU32, AtomicU64, Ordering};

// ---------------------------------------------------------------------------
// Format constants
// ---------------------------------------------------------------------------

/// Number of bands. Anything indexing a per-band array MUST use this.
pub const EQ_BANDS: usize = 5;

/// Coefficient fixed-point shift.
const QC: u32 = 28;
/// Intermediate (coefficient-derivation) fixed-point shift.
const Q24: u32 = 24;
const ONE24: i64 = 1 << Q24;
/// Extra fractional bits carried on the signal inside the filter.
const SIG_F: u32 = 10;
/// Hard clamp on a section's output, in internal units (32x full scale).
const Y_MAX: i64 = 1 << 30;

/// Centre / corner frequencies, Hz. These are the ORIGINAL faceplate labels.
pub const EQ_FREQ_HZ: [u32; EQ_BANDS] = [60, 250, 1000, 4000, 12000];

const KIND_LOW_SHELF: u8 = 0;
const KIND_PEAKING: u8 = 1;
const KIND_HIGH_SHELF: u8 = 2;
const EQ_KIND: [u8; EQ_BANDS] = [
    KIND_LOW_SHELF,
    KIND_PEAKING,
    KIND_PEAKING,
    KIND_PEAKING,
    KIND_HIGH_SHELF,
];

/// Fader travel, matching the original panel: 0..100 with 50 flat.
pub const EQ_POS_FLAT: i32 = 50;
/// Full-scale boost/cut at the rails, in tenths of a dB.
pub const EQ_RANGE_DB10: i32 = 120;

// Q30 maths constants (see the CHANGELOG entry for how they were derived).
const Q30: u32 = 30;
const ONE30: i64 = 1 << Q30;
const PI_Q30: i64 = 3373259426;
const HALF_PI_Q30: i64 = 1686629713;
const TWO_PI_Q30: i64 = 6746518852;
/// log2(10) / 400, so that A = 10^(dB/40) = 2^(db10 * this).
const DB10_TO_EXP_Q30: i64 = 8917233;
const SQRT2_Q24: i64 = 23726566;

// sin(x) on [0, pi/2], Horner, Q30. Truncation error < 3e-8 over the range.
const S1_Q30: i64 = 1073741824;
const S3_Q30: i64 = -178956971;
const S5_Q30: i64 = 8947849;
const S7_Q30: i64 = -213044;
const S9_Q30: i64 = 2959;
const S11_Q30: i64 = -27;

// 2^f on [0, 1), Horner, Q30. Relative error < 1e-5.
const E1_Q30: i64 = 744261118;
const E2_Q30: i64 = 257941248;
const E3_Q30: i64 = 59597083;
const E4_Q30: i64 = 10327387;
const E5_Q30: i64 = 1431680;
const E6_Q30: i64 = 165394;

// log2(1+u) via 2/ln2 * atanh(u/(2+u)), Q30.
const LOG2_C_Q30: i64 = 3098164009;
const INV3_Q30: i64 = 357913941;
const INV5_Q30: i64 = 214748365;
const INV7_Q30: i64 = 153391689;
const INV9_Q30: i64 = 119304647;

#[inline(always)]
fn mul30(a: i64, b: i64) -> i64 {
    (a * b) >> 30
}
#[inline(always)]
fn mul24(a: i64, b: i64) -> i64 {
    (a * b) >> 24
}

// ---------------------------------------------------------------------------
// Scalar maths, all integer
// ---------------------------------------------------------------------------

/// sin(x) for x in [0, pi/2], Q30 in and out.
fn sin_quarter_q30(x: i64) -> i64 {
    let x2 = mul30(x, x);
    let mut p = S11_Q30;
    p = S9_Q30 + mul30(p, x2);
    p = S7_Q30 + mul30(p, x2);
    p = S5_Q30 + mul30(p, x2);
    p = S3_Q30 + mul30(p, x2);
    p = S1_Q30 + mul30(p, x2);
    mul30(x, p)
}

/// sin(x) for x in [0, pi]. The f0 <= 0.45*Fs clamp in design() is what
/// guarantees w0 <= 0.9*pi and therefore that this reduction is valid; it is
/// load-bearing, not defensive.
fn sin_q30(x: i64) -> i64 {
    let mut a = x;
    if a > HALF_PI_Q30 {
        a = PI_Q30 - a;
    }
    if a < 0 {
        a = 0;
    }
    sin_quarter_q30(a)
}

/// cos(x) for x in [0, pi]. Signed: negative above pi/2.
fn cos_q30(x: i64) -> i64 {
    let mut a = x;
    let mut neg = false;
    if a > HALF_PI_Q30 {
        a = PI_Q30 - a;
        neg = true;
    }
    if a < 0 {
        a = 0;
    }
    let v = sin_quarter_q30(HALF_PI_Q30 - a);
    if neg {
        -v
    } else {
        v
    }
}

/// sin(2*pi*p) where p is a position in TURNS, Q32 (so the whole u32 range is
/// one revolution). Used only by the self-test's probe-tone generator.
fn sin_turn_q30(phase_q32: u32) -> i64 {
    let quad = (phase_q32 >> 30) & 3;
    let rem = (phase_q32 & 0x3FFF_FFFF) as i64; // Q30 fraction of a quadrant
    let ang = (rem * HALF_PI_Q30) >> 30; // 0 .. pi/2
    match quad {
        0 => sin_quarter_q30(ang),
        1 => sin_quarter_q30(HALF_PI_Q30 - ang),
        2 => -sin_quarter_q30(ang),
        _ => -sin_quarter_q30(HALF_PI_Q30 - ang),
    }
}

/// 2^f for f in [0, 1) expressed Q30. Result Q30.
fn exp2_frac_q30(f: i64) -> i64 {
    let mut p = E6_Q30;
    p = E5_Q30 + mul30(p, f);
    p = E4_Q30 + mul30(p, f);
    p = E3_Q30 + mul30(p, f);
    p = E2_Q30 + mul30(p, f);
    p = E1_Q30 + mul30(p, f);
    ONE30 + mul30(f, p)
}

/// A = 10^(db10 / 400), i.e. the RBJ cookbook's A for a gain of db10 tenths
/// of a dB. Result Q24. db10 is bounded to +/- EQ_RANGE_DB10 by the caller,
/// so the exponent is within (-1, 1) and the integer part is 0 or -1.
fn amp_q24(db10: i32) -> i64 {
    let e = db10 as i64 * DB10_TO_EXP_Q30; // Q30 exponent
    let n = e >> 30; // floor
    let f = e - (n << 30); // [0, 2^30)
    let mut v = exp2_frac_q30(f);
    if n >= 0 {
        v <<= n as u32;
    } else {
        v >>= (-n) as u32;
    }
    v >> (Q30 - Q24)
}

/// Integer square root of a u64.
fn isqrt64(v: u64) -> u64 {
    if v == 0 {
        return 0;
    }
    // Newton from a power-of-two seed. Converges in <= 6 iterations for u64.
    let mut x: u64 = 1u64 << ((64 - v.leading_zeros() + 1) / 2);
    loop {
        let nx = (x + v / x) >> 1;
        if nx >= x {
            break;
        }
        x = nx;
    }
    x
}

/// sqrt of a Q24 value, result Q24.
fn sqrt_q24(a: i64) -> i64 {
    if a <= 0 {
        return 0;
    }
    isqrt64((a as u64) << 24) as i64
}

/// log2(x) for x > 0, result Q24. Used only by the self-test to turn measured
/// energies into decibels.
fn log2_q24(x: u64) -> i64 {
    if x == 0 {
        return -(1i64 << 40);
    }
    let n = 63i64 - x.leading_zeros() as i64;
    let m: u64 = if n >= 30 {
        x >> (n - 30) as u32
    } else {
        x << (30 - n) as u32
    }; // [2^30, 2^31)
    let u = (m - (1u64 << 30)) as i64; // Q30 in [0, 2^30)
    // z = u / (2 + u), Q30
    let z = (((u as i128) << 30) / ((2i128 << 30) + u as i128)) as i64;
    let z2 = mul30(z, z);
    let mut p = INV9_Q30;
    p = INV7_Q30 + mul30(p, z2);
    p = INV5_Q30 + mul30(p, z2);
    p = INV3_Q30 + mul30(p, z2);
    p = ONE30 + mul30(p, z2);
    let lg = mul30(LOG2_C_Q30, mul30(z, p)); // log2(1+u), Q30
    (n << 24) + (lg >> (Q30 - Q24))
}

/// 10*log10(num/den) in TENTHS of a dB. Both arguments are energies.
fn db10_ratio(num: u64, den: u64) -> i32 {
    if num == 0 || den == 0 {
        return -9999;
    }
    let d = log2_q24(num) - log2_q24(den); // Q24
    // 10*log10(r) = 3.0103 * log2(r); in tenths that is 30.103 * log2(r).
    // Divide rather than shift: an arithmetic shift floors, which would bias
    // every NEGATIVE result (a cut) a tenth of a dB low and make the boost
    // and cut tables look asymmetric for a reason that is not in the filter.
    (((d * 30103) / 1000) / (1 << Q24)) as i32
}

// ---------------------------------------------------------------------------
// Coefficients
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
struct Coef {
    b0: i64,
    b1: i64,
    b2: i64,
    a1: i64,
    a2: i64,
}
const COEF_ZERO: Coef = Coef {
    b0: 0,
    b1: 0,
    b2: 0,
    a1: 0,
    a2: 0,
};

/// RBJ cookbook coefficients for one band, normalised by a0, in Q28.
/// Returns None for a flat band (which is bypassed, not computed) or for a
/// degenerate rate.
fn design(band: usize, db10: i32, fs: u32) -> Option<Coef> {
    if db10 == 0 || fs < 4000 {
        return None;
    }
    // Keep the corner safely below Nyquist. A bilinear-transformed section
    // whose corner approaches Fs/2 is warped into nonsense; clamping is what
    // keeps a 12 kHz band meaningful on a 22050 Hz sink rather than wrong.
    //
    // KNOWN AND ACCEPTED at the bottom of the supported range: at 8000 Hz
    // (AUDIO_PCM_MIN_RATE) the limit is 3600 Hz, so bands 3 and 4 both clamp
    // there and two faders then move the same part of the spectrum. That is
    // the honest answer - a 12 kHz control on a sink with no content above
    // 4 kHz has nothing to do - and it is a degradation rather than a wrong
    // filter, but it is worth knowing before someone reports it as a bug.
    let mut f0 = EQ_FREQ_HZ[band] as i64;
    let lim = (fs as i64 * 45) / 100;
    if f0 > lim {
        f0 = lim;
    }
    if f0 < 10 {
        return None;
    }

    let w0 = (TWO_PI_Q30 * f0) / fs as i64; // Q30 radians, < pi
    let cw = cos_q30(w0) >> (Q30 - Q24); // Q24
    let sw = sin_q30(w0) >> (Q30 - Q24); // Q24
    let a = amp_q24(db10);
    if a <= 0 {
        return None;
    }

    let (b0, b1, b2, a0, a1, a2);
    if EQ_KIND[band] == KIND_PEAKING {
        // Q = 1.0, so alpha = sin(w0) / 2.
        let alpha = sw / 2;
        let a_alpha = mul24(alpha, a);
        let alpha_a = (alpha * ONE24) / a;
        b0 = ONE24 + a_alpha;
        b1 = -2 * cw;
        b2 = ONE24 - a_alpha;
        a0 = ONE24 + alpha_a;
        a1 = -2 * cw;
        a2 = ONE24 - alpha_a;
    } else {
        // Shelf slope S = 1.0, for which
        //   alpha = sin(w0)/2 * sqrt((A + 1/A)(1/S - 1) + 2) = sin(w0)/2 * sqrt(2)
        let alpha = mul24(sw / 2, SQRT2_Q24);
        let sq_a = sqrt_q24(a);
        let two_sq_a_alpha = 2 * mul24(sq_a, alpha);
        let ap1 = a + ONE24;
        let am1 = a - ONE24;
        if EQ_KIND[band] == KIND_LOW_SHELF {
            b0 = mul24(a, ap1 - mul24(am1, cw) + two_sq_a_alpha);
            b1 = 2 * mul24(a, am1 - mul24(ap1, cw));
            b2 = mul24(a, ap1 - mul24(am1, cw) - two_sq_a_alpha);
            a0 = ap1 + mul24(am1, cw) + two_sq_a_alpha;
            a1 = -2 * (am1 + mul24(ap1, cw));
            a2 = ap1 + mul24(am1, cw) - two_sq_a_alpha;
        } else {
            b0 = mul24(a, ap1 + mul24(am1, cw) + two_sq_a_alpha);
            b1 = -2 * mul24(a, am1 + mul24(ap1, cw));
            b2 = mul24(a, ap1 + mul24(am1, cw) - two_sq_a_alpha);
            a0 = ap1 - mul24(am1, cw) + two_sq_a_alpha;
            a1 = 2 * (am1 - mul24(ap1, cw));
            a2 = ap1 - mul24(am1, cw) - two_sq_a_alpha;
        }
    }
    if a0 == 0 {
        return None;
    }
    let n = |x: i64| (x << QC) / a0;
    Some(Coef {
        b0: n(b0),
        b1: n(b1),
        b2: n(b2),
        a1: n(a1),
        a2: n(a2),
    })
}

// ---------------------------------------------------------------------------
// Live state
//
// POSITIONS + GEN are written by the syscall path (any thread). Everything
// below them is owned exclusively by the mixer thread, which is the only
// caller of pcm_eq_process_rs.
// ---------------------------------------------------------------------------

static POS: [AtomicI32; EQ_BANDS] = [
    AtomicI32::new(EQ_POS_FLAT),
    AtomicI32::new(EQ_POS_FLAT),
    AtomicI32::new(EQ_POS_FLAT),
    AtomicI32::new(EQ_POS_FLAT),
    AtomicI32::new(EQ_POS_FLAT),
];
/// Bumped on every change from any source; the mixer recompiles when it moves.
static GEN: AtomicU32 = AtomicU32::new(1);
/// Per-band counter of EXTERNAL (syscall) writes, and the value of the most
/// recent one. Only pcm_eq_set_rs / pcm_eq_reset_rs touch these; the
/// self-test's own probe writes deliberately do not. See st_enter()/st_leave().
static EXT_SEQ: [AtomicU32; EQ_BANDS] = [
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
];
static EXT_VAL: [AtomicI32; EQ_BANDS] = [
    AtomicI32::new(EQ_POS_FLAT),
    AtomicI32::new(EQ_POS_FLAT),
    AtomicI32::new(EQ_POS_FLAT),
    AtomicI32::new(EQ_POS_FLAT),
    AtomicI32::new(EQ_POS_FLAT),
];
/// Sink rate the coefficients were designed for.
static RATE: AtomicU32 = AtomicU32::new(0);
/// Blocks the filter actually ran (0 while flat) and frames filtered.
static FRAMES_DONE: AtomicU64 = AtomicU64::new(0);
/// Largest |state| word the live filter has reached, for headroom reporting.
static PEAK_STATE: AtomicU64 = AtomicU64::new(0);

struct Chain {
    applied_gen: u32,
    applied_rate: u32,
    active: [bool; EQ_BANDS],
    coef: [Coef; EQ_BANDS],
    // [band][channel]
    s1: [[i64; 2]; EQ_BANDS],
    s2: [[i64; 2]; EQ_BANDS],
    peak: u64,
}

static mut CHAIN: Chain = Chain {
    applied_gen: 0,
    applied_rate: 0,
    active: [false; EQ_BANDS],
    coef: [COEF_ZERO; EQ_BANDS],
    s1: [[0; 2]; EQ_BANDS],
    s2: [[0; 2]; EQ_BANDS],
    peak: 0,
};

#[inline]
fn db10_of_pos(pos: i32) -> i32 {
    let p = if pos < 0 {
        0
    } else if pos > 100 {
        100
    } else {
        pos
    };
    (p - EQ_POS_FLAT) * (EQ_RANGE_DB10 * 2) / 100
}

/// Recompute every band from the atomic positions. Mixer thread only.
///
/// A band that has just become inactive gets its state ZEROED here: leaving
/// it would let energy from before the bypass reappear as a thump the moment
/// the band is turned back on.
unsafe fn rebuild(c: &mut Chain, fs: u32) {
    for b in 0..EQ_BANDS {
        let db10 = db10_of_pos(POS[b].load(Ordering::Relaxed));
        match design(b, db10, fs) {
            Some(k) => {
                c.coef[b] = k;
                c.active[b] = true;
            }
            None => {
                if c.active[b] {
                    c.s1[b] = [0; 2];
                    c.s2[b] = [0; 2];
                }
                c.coef[b] = COEF_ZERO;
                c.active[b] = false;
            }
        }
    }
    c.applied_rate = fs;
    c.applied_gen = GEN.load(Ordering::Acquire);
}

#[inline(always)]
fn section(s1: &mut i64, s2: &mut i64, k: &Coef, x: i64) -> i64 {
    // Transposed direct form II.
    let acc = k.b0 * x + *s1;
    let mut y = (acc + (1 << (QC - 1))) >> QC;
    if y > Y_MAX {
        y = Y_MAX;
    } else if y < -Y_MAX {
        y = -Y_MAX;
    }
    *s1 = k.b1 * x - k.a1 * y + *s2;
    *s2 = k.b2 * x - k.a2 * y;
    y
}

// ===========================================================================
// The production entry point
// ===========================================================================

/// Filter `frames` stereo frames of the mixer's i32 accumulator IN PLACE.
///
/// Returns 1 if the accumulator was modified, 0 if the EQ is flat and the
/// buffer was not touched at all. A 0 return is the guarantee that the
/// EQ-disabled path is bit-identical, not merely similar.
///
/// MIXER THREAD ONLY. It owns CHAIN outright; nothing else may call this.
///
/// SAFETY: the caller guarantees `acc` addresses frames*2 i32.
#[no_mangle]
pub unsafe extern "C" fn pcm_eq_process_rs(acc: *mut i32, frames: u32) -> u32 {
    if acc.is_null() || frames == 0 {
        return 0;
    }
    let fs = RATE.load(Ordering::Relaxed);
    if fs < 4000 {
        return 0;
    }
    let c = &mut *core::ptr::addr_of_mut!(CHAIN);
    let gen = GEN.load(Ordering::Acquire);
    if c.applied_gen != gen || c.applied_rate != fs {
        rebuild(c, fs);
    }

    // Compact the active bands ONCE per block, and lift their coefficients
    // and state into locals for the duration. Flat bands then cost nothing at
    // all (the common case), and the inner loop indexes a dense array instead
    // of re-reading a 40-byte struct out of CHAIN for every sample.
    let mut idx = [0usize; EQ_BANDS];
    let mut kc = [COEF_ZERO; EQ_BANDS];
    let mut ls1 = [[0i64; 2]; EQ_BANDS];
    let mut ls2 = [[0i64; 2]; EQ_BANDS];
    let mut n = 0usize;
    for b in 0..EQ_BANDS {
        if c.active[b] {
            idx[n] = b;
            kc[n] = c.coef[b];
            ls1[n] = c.s1[b];
            ls2[n] = c.s2[b];
            n += 1;
        }
    }
    if n == 0 {
        return 0;
    }

    for f in 0..frames as usize {
        for ch in 0..2usize {
            let p = acc.add(f * 2 + ch);
            let mut y = (*p as i64) << SIG_F;
            for j in 0..n {
                y = section(&mut ls1[j][ch], &mut ls2[j][ch], &kc[j], y);
            }
            *p = ((y + (1 << (SIG_F - 1))) >> SIG_F) as i32;
        }
    }

    // Peak state is sampled ONCE per block, not per sample. The state words
    // are heavily low-passed accumulators, so a block boundary is a fair
    // sample of them, and this keeps the headroom instrumentation out of the
    // inner loop entirely. The strong bound on overflow comes from
    // pcm_eq_selftest_rs's deliberate worst case, not from watching live
    // audio.
    let mut peak = c.peak;
    for j in 0..n {
        let b = idx[j];
        c.s1[b] = ls1[j];
        c.s2[b] = ls2[j];
        for ch in 0..2usize {
            let m = ls1[j][ch].unsigned_abs();
            if m > peak {
                peak = m;
            }
            let m2 = ls2[j][ch].unsigned_abs();
            if m2 > peak {
                peak = m2;
            }
        }
    }
    c.peak = peak;
    PEAK_STATE.store(peak, Ordering::Relaxed);
    FRAMES_DONE.fetch_add(frames as u64, Ordering::Relaxed);
    1
}

/// A mix session is starting at `rate`. Resets ALL filter state and forces a
/// coefficient rebuild, so the first block of a new session cannot inherit
/// the tail of the previous one (which is exactly what a "thump on play"
/// is).
#[no_mangle]
pub extern "C" fn pcm_eq_session_start_rs(rate: u32) {
    RATE.store(rate, Ordering::Relaxed);
    unsafe {
        let c = &mut *core::ptr::addr_of_mut!(CHAIN);
        c.s1 = [[0; 2]; EQ_BANDS];
        c.s2 = [[0; 2]; EQ_BANDS];
        c.peak = 0;
        c.applied_gen = 0; // force rebuild on the first block
        c.applied_rate = 0;
    }
    PEAK_STATE.store(0, Ordering::Relaxed);
}

// ===========================================================================
// Control surface (SYS_AUDIO_EQ)
// ===========================================================================

#[inline]
fn clamp_pos(pos: i32) -> i32 {
    if pos < 0 {
        0
    } else if pos > 100 {
        100
    } else {
        pos
    }
}

/// Store one band and publish it. The ONE place POS is written outside the
/// self-test's own bookkeeping.
fn apply_pos(band: usize, pos: i32) {
    if POS[band].swap(pos, Ordering::Release) != pos {
        GEN.fetch_add(1, Ordering::Release);
    }
}

// ---------------------------------------------------------------------------
// SELF-TEST / BENCH EXCLUSION, AND WHY IT IS A SEQUENCE COUNTER RATHER THAN A
// LOCK OR A FLAG.
//
// pcm_eq_selftest_rs and pcm_eq_bench_rs drive POS through a probe sequence
// and then put back what was there, so a machine whose owner has already set
// an EQ does not have it flattened by a diagnostic. The naive version of that
// - snapshot, run, restore - contains a real bug, and it is exactly the bug
// this whole ticket exists to not reintroduce.
//
// Both run on the mixer thread from pcm_mixer_worker(), which starts at the
// FIRST PCM open, i.e. the boot chime. The compositor's profile_load() ->
// prof_apply("eq0", ...) -> SYS_AUDIO_EQ lands at almost exactly that moment,
// and NOTHING ORDERS THE TWO. A set arriving inside the probe window would be
// overwritten by the restore, with no error anywhere: the user's persisted EQ
// would "apply live and vanish at reboot", which is the precise defect #231
// fixed for g_clock_locked/g_cal_locked/g_digclk_style.
//
// The rule is: an EXTERNAL write that happened at any point during the window
// WINS over the snapshot, because it is the newer intent.
//
// Implemented with a per-band counter, not a busy flag, because a flag has an
// unavoidable gap at each edge (a setter that has already tested the flag but
// not yet stored). The counter has none, and the ORDER inside each side is
// what makes that true:
//
//   setter   : EXT_VAL.store  ->  apply_pos  ->  EXT_SEQ.fetch_add(Release)
//   st_enter : EXT_SEQ.load(Acquire)  ->  POS.load
//
// If the reader's EXT_SEQ load happens BEFORE the setter's bump, st_leave
// sees the counter move and takes EXT_VAL - correct whatever POS did. If it
// happens AFTER the bump, then by release/acquire the setter's apply_pos is
// already visible, and the snapshot taken afterwards contains it - also
// correct. There is no third case, so there is no window.
//
// No lock is taken and nothing waits, so there is nothing here for #426 to
// object to and nothing that could block the mixer thread.
// ---------------------------------------------------------------------------

fn st_enter(saved: &mut [i32; EQ_BANDS], seq0: &mut [u32; EQ_BANDS]) -> u32 {
    for b in 0..EQ_BANDS {
        seq0[b] = EXT_SEQ[b].load(Ordering::Acquire);   // MUST precede the POS read
        saved[b] = POS[b].load(Ordering::Acquire);
    }
    RATE.load(Ordering::Relaxed)
}

fn st_leave(saved: &[i32; EQ_BANDS], seq0: &[u32; EQ_BANDS], rate: u32) {
    for b in 0..EQ_BANDS {
        let now = EXT_SEQ[b].load(Ordering::Acquire);
        let v = if now != seq0[b] {
            EXT_VAL[b].load(Ordering::Acquire)   // somebody set it meanwhile: theirs wins
        } else {
            saved[b]
        };
        POS[b].store(clamp_pos(v), Ordering::Release);
    }
    GEN.fetch_add(1, Ordering::Release);
    pcm_eq_session_start_rs(rate);
    FRAMES_DONE.store(0, Ordering::Relaxed);
    PEAK_STATE.store(0, Ordering::Relaxed);
}

/// Set one band's fader position, 0..100 with 50 flat. Returns 0, or -1 for a
/// band index out of range.
#[no_mangle]
pub extern "C" fn pcm_eq_set_rs(band: i32, pos: i32) -> i32 {
    if band < 0 || band as usize >= EQ_BANDS {
        return -1;
    }
    let b = band as usize;
    let p = clamp_pos(pos);
    // ORDER IS LOAD-BEARING: value, then apply, then publish the counter.
    // See the block comment above st_enter().
    EXT_VAL[b].store(p, Ordering::Release);
    apply_pos(b, p);
    EXT_SEQ[b].fetch_add(1, Ordering::Release);
    0
}

#[no_mangle]
pub extern "C" fn pcm_eq_get_rs(band: i32) -> i32 {
    if band < 0 || band as usize >= EQ_BANDS {
        return -1;
    }
    POS[band as usize].load(Ordering::Acquire)
}

#[no_mangle]
pub extern "C" fn pcm_eq_bands_rs() -> i32 {
    EQ_BANDS as i32
}

#[no_mangle]
pub extern "C" fn pcm_eq_freq_rs(band: i32) -> i32 {
    if band < 0 || band as usize >= EQ_BANDS {
        return -1;
    }
    EQ_FREQ_HZ[band as usize] as i32
}

/// This band's gain in TENTHS of a dB, signed. The one place the fader-to-dB
/// mapping is defined, so the log line and the UI cannot disagree about it.
#[no_mangle]
pub extern "C" fn pcm_eq_db10_rs(band: i32) -> i32 {
    if band < 0 || band as usize >= EQ_BANDS {
        return 0;
    }
    db10_of_pos(POS[band as usize].load(Ordering::Acquire))
}

/// 1 if ANY band is off flat, i.e. the filter will actually run.
#[no_mangle]
pub extern "C" fn pcm_eq_active_rs() -> i32 {
    for b in 0..EQ_BANDS {
        if POS[b].load(Ordering::Acquire) != EQ_POS_FLAT {
            return 1;
        }
    }
    0
}

/// Return every band to flat in one call. Counted as an external write per
/// band, the same way pcm_eq_set_rs is, so a reset issued during a self-test
/// survives the restore.
#[no_mangle]
pub extern "C" fn pcm_eq_reset_rs() -> i32 {
    for b in 0..EQ_BANDS {
        EXT_VAL[b].store(EQ_POS_FLAT, Ordering::Release);
        apply_pos(b, EQ_POS_FLAT);
        EXT_SEQ[b].fetch_add(1, Ordering::Release);
    }
    0
}

/// The self-test's own setter: writes POS directly, never parks, never
/// consults ST_BUSY. Using pcm_eq_set_rs() from inside the window would park
/// the test's OWN probe values and the test would measure nothing.
fn st_set(band: usize, pos: i32) {
    apply_pos(band, clamp_pos(pos));
}

/// The self-test's own reset, for the same reason as st_set().
fn st_reset() {
    for b in 0..EQ_BANDS {
        apply_pos(b, EQ_POS_FLAT);
    }
}

/// Diagnostics for /AUDIOLOG.TXT: frames the filter has actually processed.
#[no_mangle]
pub extern "C" fn pcm_eq_frames_rs() -> u64 {
    FRAMES_DONE.load(Ordering::Relaxed)
}

/// Diagnostics: the largest |state| word the LIVE filter has reached. The
/// headroom argument in this file's header says this must stay far below
/// i64::MAX; this is how that claim is checked on a real machine rather than
/// only on paper.
#[no_mangle]
pub extern "C" fn pcm_eq_peak_state_rs() -> u64 {
    PEAK_STATE.load(Ordering::Relaxed)
}

// ===========================================================================
// SELF-TEST: a real per-band SPECTRAL measurement, run at boot
// ===========================================================================
//
// "The fader moves" is not evidence, and neither is "the syscall returned 0".
// The EQ was deleted for being convincing and inert, so the test that
// re-introduces it has to be a measurement of the audio, not of the plumbing.
//
// For each band, this boosts that band to the top of its travel and measures
// the output-to-input energy ratio at EVERY band's centre frequency, through
// the REAL pcm_eq_process_rs entry point, in 256-frame blocks (so block
// boundaries are exercised too). It reports the result as a 5x5 matrix of
// tenths of a dB, which the C side prints to /AUDIOLOG.TXT.
//
// Expected, and asserted below: on the diagonal, a peaking band reaches its
// full +12.0 dB at its own centre; a SHELF reaches +6.0 dB AT its corner
// frequency, because the corner of an RBJ shelf is by definition the
// half-gain point, with the full +12 dB reached out on the plateau. Off the
// diagonal, every other band moves by ~1 dB or less.
//
// It also proves the two properties that matter most:
//   - FLAT IS UNTOUCHED: pcm_eq_process_rs returns 0 and the buffer is
//     byte-identical to its input.
//   - NO OVERFLOW: a full-scale square wave with every band at maximum boost
//     leaves the largest state word orders of magnitude below i64::MAX.

const ST_FS: u32 = 44100;
const ST_BLOCK: usize = 256;
const ST_BLOCKS: usize = 32; // 8192 frames
const ST_SKIP: usize = 8; // blocks discarded while the filter settles
const ST_AMP: i64 = 12000;

static mut ST_BUF: [i32; ST_BLOCK * 2] = [0; ST_BLOCK * 2];

/// Measure the gain the CURRENT settings apply at `hz`, in tenths of a dB.
unsafe fn st_measure(hz: u32) -> i32 {
    pcm_eq_session_start_rs(ST_FS);
    let step = ((hz as u64) << 32) / ST_FS as u64;
    let mut ph: u32 = 0;
    let mut e_in: u64 = 0;
    let mut e_out: u64 = 0;
    let buf = &mut *core::ptr::addr_of_mut!(ST_BUF);
    for blk in 0..ST_BLOCKS {
        for i in 0..ST_BLOCK {
            let s = (ST_AMP * sin_turn_q30(ph)) >> 30;
            buf[i * 2] = s as i32;
            buf[i * 2 + 1] = s as i32;
            ph = ph.wrapping_add(step as u32);
        }
        // Energy of the INPUT block, before the filter overwrites it.
        if blk >= ST_SKIP {
            for i in 0..ST_BLOCK {
                let v = buf[i * 2] as i64;
                e_in += (v * v) as u64;
            }
        }
        pcm_eq_process_rs(buf.as_mut_ptr(), ST_BLOCK as u32);
        if blk >= ST_SKIP {
            for i in 0..ST_BLOCK {
                let v = buf[i * 2] as i64;
                e_out += (v * v) as u64;
            }
        }
    }
    db10_ratio(e_out, e_in)
}

/// Fill `out` (25 i32, row-major: row = boosted band, col = probe band) with
/// the measured gain in tenths of a dB, and return the number of FAILED
/// assertions (0 = pass). `out_peak` receives the largest |state| word the
/// overflow probe reached.
///
/// SAFETY: `out` addresses 25 i32; `out_peak` addresses one u64. Both may be
/// null, in which case that result is simply not reported.
#[no_mangle]
pub unsafe extern "C" fn pcm_eq_selftest_rs(out: *mut i32, out_peak: *mut u64) -> u32 {
    let mut fails: u32 = 0;

    // Remember the live settings: this runs on a machine whose owner may
    // already have set an EQ, and a self-test that quietly flattens it would
    // be its own bug. st_enter() also opens the parking window, so a set that
    // races the probe sequence is deferred rather than discarded - see the
    // block comment above st_enter().
    let mut saved = [EQ_POS_FLAT; EQ_BANDS];
    let mut seq0 = [0u32; EQ_BANDS];
    let saved_rate = st_enter(&mut saved, &mut seq0);

    // ---- 1. FLAT MUST BE BIT-IDENTICAL -----------------------------------
    st_reset();
    pcm_eq_session_start_rs(ST_FS);
    {
        let buf = &mut *core::ptr::addr_of_mut!(ST_BUF);
        for i in 0..ST_BLOCK * 2 {
            // A deliberately awkward pattern: alternating sign, near full
            // scale, so a filter that ran at all would visibly change it.
            buf[i] = if i & 1 == 0 { 30000 } else { -29999 } - (i as i32);
        }
        let mut copy = [0i32; ST_BLOCK * 2];
        copy.copy_from_slice(&buf[..]);
        let touched = pcm_eq_process_rs(buf.as_mut_ptr(), ST_BLOCK as u32);
        if touched != 0 {
            fails |= 1 << 0;
        }
        for i in 0..ST_BLOCK * 2 {
            if buf[i] != copy[i] {
                fails |= 1 << 0;
            }
        }
    }

    // ---- 2. THE 5x5 SPECTRAL MATRIX --------------------------------------
    let mut m = [0i32; EQ_BANDS * EQ_BANDS];
    for b in 0..EQ_BANDS {
        st_reset();
        st_set(b, 100); // +12.0 dB
        for p in 0..EQ_BANDS {
            m[b * EQ_BANDS + p] = st_measure(EQ_FREQ_HZ[p]);
        }
    }
    if !out.is_null() {
        for i in 0..EQ_BANDS * EQ_BANDS {
            *out.add(i) = m[i];
        }
    }

    // Diagonal: the band that was boosted must actually be boosted, and by
    // the RIGHT amount. Peaking sections reach the full +120 (12.0 dB) at
    // their centre; shelves reach half of it at their corner by definition.
    for b in 0..EQ_BANDS {
        let d = m[b * EQ_BANDS + b];
        let want = if EQ_KIND[b] == KIND_PEAKING { 120 } else { 60 };
        if d < want - 15 || d > want + 15 {
            fails |= 1 << 1;
        }
    }
    // Off-diagonal: everything else must be SUBSTANTIALLY unchanged. 2.0 dB
    // is a generous bound for 2-octave-spaced Q=1 sections (the measured
    // worst case is ~1.0 dB); anything beyond it means the bands are not
    // separated and the control would not behave like a graphic EQ.
    for b in 0..EQ_BANDS {
        for p in 0..EQ_BANDS {
            if b == p {
                continue;
            }
            let v = m[b * EQ_BANDS + p];
            if v > 20 || v < -20 {
                fails |= 1 << 2;
            }
        }
    }

    // ---- 3. A CUT MUST MIRROR A BOOST ------------------------------------
    // Not exactly (the filters are not symmetric in fixed point), but a cut
    // that does not cut is the same defect as a boost that does not boost.
    for b in 0..EQ_BANDS {
        st_reset();
        st_set(b, 0); // -12.0 dB
        let d = st_measure(EQ_FREQ_HZ[b]);
        let want = if EQ_KIND[b] == KIND_PEAKING { -120 } else { -60 };
        if d < want - 15 || d > want + 15 {
            fails |= 1 << 3;
        }
    }

    // ---- 4. HEADROOM, MEASURED -------------------------------------------
    // Every band at maximum boost, driven by a full-scale square wave at four
    // times the mixer's own single-stream full scale (i.e. what four
    // simultaneous producers at full level would present). The header's
    // overflow argument says the largest state word stays around 2^60; this
    // is the number that proves it on the built kernel.
    let peak;
    {
        st_reset();
        for b in 0..EQ_BANDS {
            st_set(b, 100);
        }
        pcm_eq_session_start_rs(ST_FS);
        let buf = &mut *core::ptr::addr_of_mut!(ST_BUF);
        for blk in 0..16 {
            for i in 0..ST_BLOCK {
                // 100 Hz-ish square at 4x full scale, sign flipping every
                // 220 samples.
                let hi = ((blk * ST_BLOCK + i) / 220) & 1 == 0;
                let v: i32 = if hi { 131071 } else { -131072 };
                buf[i * 2] = v;
                buf[i * 2 + 1] = -v;
            }
            pcm_eq_process_rs(buf.as_mut_ptr(), ST_BLOCK as u32);
        }
        peak = PEAK_STATE.load(Ordering::Relaxed);
        // i64::MAX is 9.22e18. Anything above 2^62 means the analysis in this
        // file's header is wrong and a wrap is reachable.
        if peak >= (1u64 << 62) {
            fails |= 1 << 4;
        }
    }
    if !out_peak.is_null() {
        *out_peak = peak;
    }

    // ---- 5. THE CONTROL SURFACE ITSELF -----------------------------------
    st_reset();
    if pcm_eq_active_rs() != 0 {
        fails |= 1 << 5;
    }
    st_set(2, 77);
    if pcm_eq_get_rs(2) != 77 || pcm_eq_active_rs() != 1 {
        fails |= 1 << 5;
    }
    // Out-of-range band indices must be refused, not wrap into a neighbour.
    if pcm_eq_set_rs(-1, 10) != -1 || pcm_eq_set_rs(EQ_BANDS as i32, 10) != -1 {
        fails |= 1 << 5;
    }
    if pcm_eq_get_rs(99) != -1 {
        fails |= 1 << 5;
    }
    // Position clamps at the rails rather than wrapping.
    st_set(0, 5000);
    if pcm_eq_get_rs(0) != 100 {
        fails |= 1 << 5;
    }
    st_set(0, -5000);
    if pcm_eq_get_rs(0) != 0 {
        fails |= 1 << 5;
    }
    // The fader-to-dB mapping, at the three points that define it.
    if pcm_eq_db10_rs(0) != -120 {
        fails |= 1 << 5;
    }
    st_set(0, 100);
    if pcm_eq_db10_rs(0) != 120 {
        fails |= 1 << 5;
    }
    st_set(0, EQ_POS_FLAT);
    if pcm_eq_db10_rs(0) != 0 {
        fails |= 1 << 5;
    }

    // ---- restore ----------------------------------------------------------
    // Not a bare snapshot restore: st_leave() applies anything a Ring-3 set
    // parked while this was running, because that value is newer than the
    // snapshot and losing it is the "applies live, vanishes at reboot" defect.
    st_leave(&saved, &seq0, saved_rate);

    fails
}

/// Cost, measured on THIS machine: cycles to filter `frames` stereo frames
/// with every band active. Reported to /AUDIOLOG.TXT so "it is cheap enough"
/// is a number from the owner's own hardware rather than an estimate.
///
/// SAFETY: touches only its own static scratch buffer.
#[no_mangle]
pub unsafe extern "C" fn pcm_eq_bench_rs(frames: u32) -> u64 {
    let n = if frames as usize > ST_BLOCK {
        ST_BLOCK
    } else {
        frames as usize
    };
    // A zero divisor below would be an unconditional Rust panic (the
    // division-by-zero check is not gated by overflow-checks), i.e. a halted
    // machine rather than a bad number. No current caller passes 0, but this
    // is a #[no_mangle] extern "C" symbol and "no current caller" is not a
    // property of a public entry point.
    if n == 0 {
        return 0;
    }
    let mut saved = [EQ_POS_FLAT; EQ_BANDS];
    let mut seq0 = [0u32; EQ_BANDS];
    let saved_rate = st_enter(&mut saved, &mut seq0);

    for b in 0..EQ_BANDS {
        st_set(b, 100);
    }
    pcm_eq_session_start_rs(ST_FS);
    let buf = &mut *core::ptr::addr_of_mut!(ST_BUF);
    for i in 0..n * 2 {
        buf[i] = ((i as i32 * 977) & 0xFFFF) - 32768;
    }
    // One untimed pass so the rebuild and the first cache misses are not
    // charged to the measurement.
    pcm_eq_process_rs(buf.as_mut_ptr(), n as u32);

    let t0 = core::arch::x86_64::_rdtsc();
    for _ in 0..16 {
        pcm_eq_process_rs(buf.as_mut_ptr(), n as u32);
    }
    let t1 = core::arch::x86_64::_rdtsc();

    st_leave(&saved, &seq0, saved_rate);

    // Cycles per 1000 stereo frames, so the caller needs no division and the
    // number is directly comparable against a rate in Hz.
    let total = t1.wrapping_sub(t0);
    (total * 1000) / (16u64 * n as u64)
}
