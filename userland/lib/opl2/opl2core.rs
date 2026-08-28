// opl2core.rs - #182: THE YM3812 (OPL2) FM SYNTHESIS CORE. ONE COPY, IN USERLAND.
//
// ===========================================================================
// WHAT THIS IS, AND WHY THERE IS EXACTLY ONE OF IT
// ---------------------------------------------------------------------------
// This is the only FM synthesiser in MayteraOS. It has two consumers and it
// will never have two implementations:
//
//   1. THE DOS OPL2 EMULATION.  kernel/dos/dosexec.c accepts a guest's writes
//      to ports 0x388/0x389, timestamps them, and queues them for Ring 3
//      (SYS_DOS_FM_EVENTS). userland/apps/fmsynth drains that queue into
//      Opl2::write_reg and pushes the rendered PCM at the sink.
//   2. THE MIDI PLAYER (#183).  Links this same file and calls the same
//      write_reg/render pair. It does not get its own synthesiser.
//
// Two consumers with two implementations is this project's signature failure:
// two Task Managers, two g_wallpapers[] arrays, five version.h files, msh and
// the terminal each with a private env table (#112), and FIVE hand-written
// CPU-ranking copies (#178) where one defect got fixed twice and missed once.
// userland/lib/opl2/core-gate.sh FAILS THE BUILD if a second log-sine table or
// a second OPL register decoder appears anywhere in the tree, so "there is only
// one" is a mechanism and not a paragraph.
//
// ===========================================================================
// WHY INTEGER, WHICH IS A DECISION WITH THREE MEASURED REASONS AND NOT A HABIT
// ---------------------------------------------------------------------------
// The brief for this ticket said, correctly for C, that userland is hardware
// SSE2 float and warned against reaching for fixed-point out of kernel habit.
// That warning is right and this is not that. Three reasons, in order of
// weight:
//
//   1. FIDELITY. The YM3812 is a DIGITAL device that computes in the log
//      domain with integers: a quarter-wave log-sine ROM, an exponent ROM, a
//      9-bit attenuation accumulator in 0.1875 dB steps, and a phase
//      accumulator. There is no analogue stage and no real number anywhere in
//      it. Implementing it in float would not be more accurate, it would be a
//      float approximation OF an integer pipeline: strictly worse and slower.
//      This is why every serious OPL implementation is integer.
//
//   2. MEASURED, 2026-08-20, on the pinned rustc 1.97.0 in the build container/the build container:
//      userland RUST is NOT hardware float on this project's pinned target.
//        rustc --print target-spec-json --target x86_64-unknown-none
//          "features":   "-mmx,-sse,-sse2,...,+soft-float"
//          "rustc-abi":  "softfloat"
//      and adding -C target-feature=+sse,+sse2 DOES NOT CHANGE IT: a trivial
//      `a * b + 1.5` compiled both ways emits byte-identical code calling
//      __mulsf3 and __addsf3 through the GOT. The float ABI is part of the
//      target, not a feature flag. So userland C gets -msse -msse2 and real
//      mulsd (that part of the brief is true and stays true), while userland
//      Rust on the pinned target would route every multiply through libgcc
//      soft-float, which this libc does not even export. A float core here
//      would either fail to link or be far slower than this integer one.
//      Changing that needs a custom target JSON, which is a toolchain decision
//      well outside one ticket, and it would buy nothing given reason 1.
//
//   3. PORTABILITY OF THE PROOF. Integer arithmetic is bit-identical between
//      the host test harness (userland/lib/opl2/hosttest, ordinary Linux
//      rustc) and the shipped Ring-3 ELF. The A440 frequency check and the
//      envelope check therefore prove the SHIPPED code, not a lookalike. With
//      float they would prove two different binaries and the fast falsifiable
//      loop would be worth much less.
//
// ===========================================================================
// SOURCES. Every constant below is from public documentation, not from any
// emulator's source. Nothing in this file is adapted from Nuked-OPL3 (LGPL2.1)
// or DOSBox's opl.cpp (GPL2); neither was copied, and consequently neither
// appears in ATTRIBUTION.md, which would be a licence defect if it were wrong.
// See docs/OPL2_FM_CORE.md for the derivation of each table and rate.
// ===========================================================================
//
// #426 (NO BUSY-WAIT): this module contains no waiting of any kind. It is pure
// computation over its own state. All pacing lives in the consumer, which
// blocks in exactly one place, sys_audio_pcm_write's wait queue.


// ---------------------------------------------------------------------------
// CHIP CONSTANTS
// ---------------------------------------------------------------------------

/// The YM3812's own sample rate: master clock 3.579545 MHz divided by 72.
/// 3_579_545 / 72 = 49715.9028 Hz. Kept as the exact integer pair so the
/// frequency arithmetic never picks up a rounding error at the source.
pub const CHIP_CLOCK_HZ: u64 = 3_579_545;
pub const CHIP_CLOCK_DIV: u64 = 72;

pub const NUM_CHANNELS: usize = 9;
pub const NUM_OPERATORS: usize = 18;

/// Envelope-generator state.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Eg {
    Off = 0,
    Attack = 1,
    Decay = 2,
    Sustain = 3,
    Release = 4,
}

/// Maximum envelope attenuation. 9 bits, 0.1875 dB per step, so 511 steps is
/// 95.8 dB, which is silence at 16-bit resolution.
const EG_MAX: u16 = 511;

/// MULT: the operator's frequency multiplier, as HALVES so that the 0 -> x0.5
/// case is exact in integer arithmetic. The duplicated entries at 11/13 and
/// 14/15 are the chip's, not a typo: the field has 16 codes and 12 distinct
/// multipliers.
///   code: 0    1  2  3  4  5  6  7  8  9  10 11 12 13 14 15
///   mult: 0.5  1  2  3  4  5  6  7  8  9  10 10 12 12 15 15
const MULT_X2: [u32; 16] = [1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30];

/// KSL: attenuation added as the note rises, so high notes are quieter, as on
/// a real instrument. Indexed by the TOP FOUR BITS of the F-Number, in units
/// of 0.75 dB, giving the attenuation at the top block.
const KSL_ROM: [u16; 16] = [0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64];

/// Right-shift applied to the KSL base for each of the four KSL settings.
///
/// THE ORDERING IS NOT MONOTONE AND THIS FILE GOT IT WRONG ONCE ALREADY.
/// The field's numeric order is NOT its strength order:
///
///     KSL bits  00 -> off          KSL_SHIFT 8  (nothing survives >>8)
///     KSL bits  01 -> 3.0 dB/oct   KSL_SHIFT 1
///     KSL bits  10 -> 1.5 dB/oct   KSL_SHIFT 2
///     KSL bits  11 -> 6.0 dB/oct   KSL_SHIFT 0
///
/// This table read [8, 2, 1, 0] until 2026-08-20, which swapped settings 1 and
/// 2, and the COMMENT above it confidently described the swapped version as if
/// it were the trap it was falling into. Every patch using KSL 1 or 2 would
/// have been scaled the wrong way with the keyboard, quietly and only at the
/// extremes of the range. Corrected against three independent sources (Lee's
/// AdLib guide, the ArduinoOPL2 notes, and the die-derived kslshift = {8,1,2,0});
/// see docs/OPL2_FM_CORE.md. A comment that describes a trap is not the same
/// as code that avoids it.
const KSL_SHIFT: [u8; 4] = [8, 1, 2, 0];

/// Sustain level, 4 bits, in envelope units (0.1875 dB). Each step is 3 dB,
/// i.e. 16 envelope units, EXCEPT code 15.
///
/// CODE 15 IS NOT 45 dB. The hardware stores the 4-bit field as 0x1F rather
/// than 0x0F and compares it against the top five bits of the envelope, so the
/// decay target becomes 496 units = 93 dB, i.e. effectively silence. A naive
/// reading of the bit weights gives 45 dB and produces instruments that fade to
/// an audible floor and sit there, which is the difference between a note that
/// ends and one that does not.
const SL_TABLE: [u16; 16] = [
    0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 496,
];

/// Operator register offsets. Channel c's modulator is at OP_OFFSET[c] and its
/// carrier at OP_OFFSET[c] + 3, added to the 0x20/0x40/0x60/0x80/0xE0 bases.
/// The gaps at 0x06/0x07 and 0x0E/0x0F are real: the address space is three
/// banks of six, not eighteen consecutive slots. Indexing this wrong is the
/// classic way to produce a synth that plays the right notes on the wrong
/// operators, which sounds like an instrument problem and is not one.
const OP_OFFSET: [usize; NUM_CHANNELS] = [0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12];

/// Register offset of channel `ch`'s MODULATOR. Its carrier is at +3. Added
/// by #183 (see userland/lib/midi), which has to program per-operator
/// registers and must not carry a second copy of this map: core-gate.sh would
/// fail the build for it, correctly. Exposing the map the one core already has
/// is what "improve the shared primitive" means here.
///
/// The index is taken modulo NUM_CHANNELS rather than asserted, because a
/// panic in a Ring-3 app is an abort with no diagnosis.
pub fn op_offset(ch: u8) -> u8 {
    OP_OFFSET[(ch as usize) % NUM_CHANNELS] as u8
}

/// Reverse map: register offset (0x00..0x15) -> operator index, or 0xFF.
/// Built as a const so a write to a gap address is REJECTED rather than
/// aliasing onto a real operator.
const fn build_off2op() -> [u8; 0x16] {
    let mut t = [0xFFu8; 0x16];
    let mut c = 0usize;
    while c < NUM_CHANNELS {
        t[OP_OFFSET[c]] = (c * 2) as u8;
        t[OP_OFFSET[c] + 3] = (c * 2 + 1) as u8;
        c += 1;
    }
    t
}
const OFF2OP: [u8; 0x16] = build_off2op();

// ===========================================================================
// THE ENVELOPE RATE MODEL, AND THE FOLKLORE CONSTANT THAT IS WRONG
// ---------------------------------------------------------------------------
// THIS SECTION WAS REWRITTEN ON 2026-08-20 AFTER MEASUREMENT DISAGREED WITH IT.
//
// The first version used the widely-repeated formulation "divider exponent =
// 13 - (rate >> 2), applied once per output sample, with an eight-entry
// density pattern". It is coherent, it produces a clean geometric ladder, and
// it is WRONG BY A UNIFORM FACTOR OF 2.25. Measured with the rate probe in
// .scratch (release time from key-off to silence, output rate 44100):
//
//                          this file, v1     published    ratio
//     effective rate 30       998 ms           439 ms       2.27
//     effective rate 48        46 ms            20.6 ms     2.25
//
// and 2.25 is not a mystery once you see it: it is exactly 2 * 49716 / 44100,
// i.e. one factor of two from the wrong divider exponent and one factor of
// (chip rate / output rate) from running the envelope on the OUTPUT clock
// instead of the chip's. A synthesiser wrong by 2.25x on every envelope does
// not sound broken, it sounds SLUGGISH, which is the kind of wrong that gets
// argued about for a week instead of measured in an afternoon.
//
// THE ACTUAL STRUCTURE (from the decapsulated-die analysis and confirmed
// against Cimalando's hardware measurements; see docs/OPL2_FM_CORE.md for the
// citations):
//
//   * There is a global envelope counter that advances at the CHIP's sample
//     rate, 49715.9 Hz, NOT at whatever rate the sink happens to want. This
//     core therefore keeps a fixed-point accumulator of CHIP TICKS and steps
//     the envelope 1 or 2 times per output sample as required. That is why the
//     envelope timing is identical at 44100 and at 48000, which it was not
//     before.
//   * `eg_state` is the low bit of that counter: the slow rates only advance
//     on alternate chip ticks, which is the missing factor of two.
//   * `eg_timer` is the counter >> 1.
//   * For rate_hi = rate >> 2 BELOW 12, the advance is driven by the position
//     of the lowest set bit of eg_timer: an advance happens when
//     rate_hi + (ctz(eg_timer) + 1) lands on 12, 13 or 14, which are the base
//     rate and two progressively rarer extra steps selected by rate_lo. The
//     average multiplier works out at exactly 1 + rate_lo / 4.
//   * For rate_hi AT OR ABOVE 12 the divider has run out and the speed comes
//     from a BIGGER STEP instead, every chip tick, via the 4x4 table below.
//
// The result is a single ladder in which every +4 of effective rate doubles the
// speed, from 42 seconds at rate 4 to 2.57 ms at rate 60, and rates 60..63 are
// all identical because the step saturates. EG_RATE_ANCHOR in the self-test
// pins the rate-48 rung to its published 20.56 ms, so a future "simplification"
// of this arithmetic cannot quietly reintroduce a factor of two.

/// The 4x4 step table for rate_hi >= 12, indexed [rate_lo][eg_timer & 3].
const EG_INCSTEP: [[u8; 4]; 4] = [
    [0, 0, 0, 0],
    [1, 0, 0, 0],
    [1, 0, 1, 0],
    [1, 1, 1, 0],
];

/// The envelope generator's own clock: the chip's sample rate.
/// 3579545 / 72 = 49715.9028 Hz.
const EG_CLOCK_NUM: u64 = CHIP_CLOCK_HZ;
const EG_CLOCK_DEN: u64 = CHIP_CLOCK_DIV;

// ---------------------------------------------------------------------------
// OPERATOR
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
struct Op {
    // ---- register-backed fields ----
    am: u8,   // 0x20 bit 7: amplitude modulation (tremolo) enable
    vib: u8,  // 0x20 bit 6: vibrato enable
    egt: u8,  // 0x20 bit 5: 0 = percussive (decays through sustain), 1 = sustaining
    ksr: u8,  // 0x20 bit 4: key scale rate
    mult: u8, // 0x20 bits 3..0
    ksl: u8,  // 0x40 bits 7..6
    tl: u8,   // 0x40 bits 5..0, 0.75 dB per step
    ar: u8,   // 0x60 bits 7..4
    dr: u8,   // 0x60 bits 3..0
    sl: u8,   // 0x80 bits 7..4
    rr: u8,   // 0x80 bits 3..0
    wave: u8, // 0xE0 bits 1..0

    // ---- derived / running state ----
    phase: u32,     // 32-bit accumulator; the top 10 bits index the wave
    phase_inc: u32,
    eg_state: Eg,
    eg_level: u16,  // 0 = loudest, 511 = silent
    ksl_att: u16,   // derived from block/fnum/ksl, in envelope units
    out_prev1: i32, // last two outputs, for the feedback average
    out_prev2: i32,
}

impl Op {
    const fn new() -> Op {
        Op {
            am: 0, vib: 0, egt: 0, ksr: 0, mult: 0,
            ksl: 0, tl: 0, ar: 0, dr: 0, sl: 0, rr: 0, wave: 0,
            phase: 0, phase_inc: 0,
            eg_state: Eg::Off, eg_level: EG_MAX, ksl_att: 0,
            out_prev1: 0, out_prev2: 0,
        }
    }
}

// ---------------------------------------------------------------------------
// CHANNEL
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
struct Ch {
    fnum: u16,  // 10 bits, from 0xA0 (low 8) and 0xB0 bits 1..0 (high 2)
    block: u8,  // 3 bits, 0xB0 bits 4..2
    kon: u8,    // 0xB0 bit 5
    fb: u8,     // 0xC0 bits 3..1: feedback depth
    cnt: u8,    // 0xC0 bit 0: 0 = FM (mod -> car), 1 = additive (mod + car)
}

impl Ch {
    const fn new() -> Ch {
        Ch { fnum: 0, block: 0, kon: 0, fb: 0, cnt: 0 }
    }
}

// ---------------------------------------------------------------------------
// THE CHIP
// ---------------------------------------------------------------------------

pub struct Opl2 {
    ops: [Op; NUM_OPERATORS],
    chs: [Ch; NUM_CHANNELS],
    /// Every register the guest has written, so a consumer can dump the exact
    /// programmed state. Also what makes the corpus evidence in #182
    /// reproducible: a title's instrument bank IS this array.
    regs: [u8; 256],
    /// Reg 0x01 bit 5. When clear the waveform-select registers are ignored and
    /// every operator is a plain sine, which is the OPL2's power-on state and
    /// is what an AdLib-era title that never sets it expects.
    wave_select_enable: bool,
    /// Reg 0x08 bit 6 (NOTE-SEL / NTS): chooses which F-Number bit feeds the
    /// key-scale-rate.
    note_sel: bool,
    /// Reg 0xBD bits 7 and 6: tremolo and vibrato DEPTH.
    am_depth: u8,
    vib_depth: u8,

    sample_rate: u32,
    /// The envelope runs on the CHIP's clock (49715.9 Hz), not on the sink's.
    /// `chip_acc` is a 16.16 fixed-point accumulator of chip ticks owed, and
    /// `chip_per_out` is how many chip ticks one output sample is worth. At
    /// 44100 that is 1.1274, so the envelope steps once or twice per sample.
    /// This is what makes envelope timing independent of the output rate.
    chip_acc: u32,
    chip_per_out: u32,
    /// Free-running count of CHIP ticks. Bit 0 is the hardware's `eg_state`
    /// and the whole value >> 1 is its `eg_timer`.
    chip_ticks: u32,
    /// LFO phase accumulators, 32-bit, one cycle per full wrap.
    am_phase: u32,
    vib_phase: u32,
    am_inc: u32,
    vib_inc: u32,
}

impl Opl2 {
    pub const fn new() -> Opl2 {
        Opl2 {
            ops: [Op::new(); NUM_OPERATORS],
            chs: [Ch::new(); NUM_CHANNELS],
            regs: [0u8; 256],
            wave_select_enable: false,
            note_sel: false,
            am_depth: 0,
            vib_depth: 0,
            sample_rate: 44100,
            chip_acc: 0,
            chip_per_out: 0,
            chip_ticks: 0,
            am_phase: 0,
            vib_phase: 0,
            am_inc: 0,
            vib_inc: 0,
        }
    }

    /// Set the OUTPUT sample rate and reset to power-on state.
    ///
    /// TWO CLOCKS, ON PURPOSE. The oscillators render directly at the sink's
    /// rate: every phase increment is derived from the operator's frequency in
    /// Hz, so the SIGNAL is the same signal, merely sampled on a different
    /// grid. The ENVELOPES do not: they run on the chip's own 49715.9 Hz clock
    /// via the chip_acc accumulator, because their rate ladder is defined in
    /// chip ticks and deriving it from the output rate makes every attack and
    /// decay wrong by the ratio between the two. That is not hypothetical; it
    /// is the bug this file had until 2026-08-20 and it cost a factor of 2.25.
    ///
    /// What this is NOT is cycle-exact: a recording made here will not be
    /// sample-identical to one made on hardware. It is pitch-exact and
    /// envelope-rate-exact, which is what this ticket measures and claims.
    pub fn init(&mut self, sample_rate: u32) {
        *self = Opl2::new();
        self.sample_rate = if sample_rate == 0 { 44100 } else { sample_rate };
        // Chip ticks per output sample, 16.16 fixed point.
        self.chip_per_out =
            ((EG_CLOCK_NUM << 16) / (EG_CLOCK_DEN * self.sample_rate as u64)) as u32;
        // The two LFOs. The OPL2's tremolo runs at 3.7 Hz and its vibrato at
        // 6.1 Hz, both derived from the same chip clock.
        self.am_inc = hz_to_inc(37, 10, self.sample_rate);
        self.vib_inc = hz_to_inc(61, 10, self.sample_rate);
    }

    pub fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    /// The register file, exactly as written. Read-only.
    pub fn regs(&self) -> &[u8; 256] {
        &self.regs
    }

    // -----------------------------------------------------------------------
    // REGISTER WRITE. This is one of the two halves of the whole interface.
    // -----------------------------------------------------------------------
    pub fn write_reg(&mut self, reg: u8, val: u8) {
        self.regs[reg as usize] = val;
        let hi = reg & 0xF0;
        let lo = (reg & 0x1F) as usize;

        match reg {
            0x01 => {
                self.wave_select_enable = (val & 0x20) != 0;
                return;
            }
            0x08 => {
                self.note_sel = (val & 0x40) != 0;
                // NOTE-SEL changes the key-scale-rate input for every operator,
                // so every operator's rates are now stale.
                for c in 0..NUM_CHANNELS {
                    self.refresh_channel(c);
                }
                return;
            }
            0xBD => {
                self.am_depth = (val >> 7) & 1;
                self.vib_depth = (val >> 6) & 1;
                // Rhythm mode (bit 5 and the five trigger bits) is NOT
                // implemented; see the "WHAT IS DELIBERATELY NOT HERE" note at
                // the end of this file. The depth bits above ARE, because they
                // affect the six melodic channels whether or not rhythm is on.
                return;
            }
            _ => {}
        }

        match hi {
            0x20 | 0x30 => {
                if lo >= 0x16 { return; }
                let oi = OFF2OP[lo];
                if oi == 0xFF { return; }
                let o = &mut self.ops[oi as usize];
                o.am = (val >> 7) & 1;
                o.vib = (val >> 6) & 1;
                o.egt = (val >> 5) & 1;
                o.ksr = (val >> 4) & 1;
                o.mult = val & 0x0F;
                self.refresh_op(oi as usize);
            }
            0x40 | 0x50 => {
                if lo >= 0x16 { return; }
                let oi = OFF2OP[lo];
                if oi == 0xFF { return; }
                let o = &mut self.ops[oi as usize];
                o.ksl = (val >> 6) & 3;
                o.tl = val & 0x3F;
                self.refresh_op(oi as usize);
            }
            0x60 | 0x70 => {
                if lo >= 0x16 { return; }
                let oi = OFF2OP[lo];
                if oi == 0xFF { return; }
                let o = &mut self.ops[oi as usize];
                o.ar = (val >> 4) & 0x0F;
                o.dr = val & 0x0F;
            }
            0x80 | 0x90 => {
                if lo >= 0x16 { return; }
                let oi = OFF2OP[lo];
                if oi == 0xFF { return; }
                let o = &mut self.ops[oi as usize];
                o.sl = (val >> 4) & 0x0F;
                o.rr = val & 0x0F;
            }
            0xE0 | 0xF0 => {
                if lo >= 0x16 { return; }
                let oi = OFF2OP[lo];
                if oi == 0xFF { return; }
                self.ops[oi as usize].wave = val & 0x03;
            }
            0xA0 => {
                let c = (reg & 0x0F) as usize;
                if c >= NUM_CHANNELS { return; }
                self.chs[c].fnum = (self.chs[c].fnum & 0x300) | val as u16;
                self.refresh_channel(c);
            }
            0xB0 => {
                let c = (reg & 0x0F) as usize;
                if c >= NUM_CHANNELS { return; }
                let was_on = self.chs[c].kon;
                self.chs[c].fnum = (self.chs[c].fnum & 0x0FF) | (((val & 0x03) as u16) << 8);
                self.chs[c].block = (val >> 2) & 0x07;
                self.chs[c].kon = (val >> 5) & 1;
                self.refresh_channel(c);
                if self.chs[c].kon != 0 && was_on == 0 {
                    self.key_on(c);
                } else if self.chs[c].kon == 0 && was_on != 0 {
                    self.key_off(c);
                }
            }
            0xC0 => {
                let c = (reg & 0x0F) as usize;
                if c >= NUM_CHANNELS { return; }
                self.chs[c].fb = (val >> 1) & 0x07;
                self.chs[c].cnt = val & 1;
            }
            _ => {}
        }
    }

    // -----------------------------------------------------------------------
    // RENDER. The other half of the interface.
    // -----------------------------------------------------------------------

    /// Render `out.len()` MONO samples, signed 16-bit.
    pub fn render(&mut self, out: &mut [i16]) {
        for s in out.iter_mut() {
            *s = self.next_sample();
        }
    }

    /// Render `out.len() / 2` interleaved STEREO frames, the same signal in
    /// both channels. The OPL2 is a mono part; a stereo pan would be an
    /// invention, so both channels carry the identical sample rather than a
    /// fabricated stereo image.
    pub fn render_stereo(&mut self, out: &mut [i16]) {
        let n = out.len() / 2;
        for i in 0..n {
            let v = self.next_sample();
            out[i * 2] = v;
            out[i * 2 + 1] = v;
        }
    }

    /// True when every operator has reached the OFF state, i.e. nothing at all
    /// is sounding. A consumer can use this to stop pushing silence.
    pub fn is_silent(&self) -> bool {
        for o in self.ops.iter() {
            if o.eg_state != Eg::Off {
                return false;
            }
        }
        true
    }

    /// How many channels currently have a sounding carrier. Diagnostics.
    pub fn active_voices(&self) -> u32 {
        let mut n = 0;
        for c in 0..NUM_CHANNELS {
            if self.ops[c * 2 + 1].eg_state != Eg::Off {
                n += 1;
            }
        }
        n
    }

    fn next_sample(&mut self) -> i16 {
        // Run the envelope generator on the CHIP's clock, not the sink's. At
        // 44100 out this is one or two steps per sample; at 48000 it is
        // sometimes zero. Either way an attack takes the same number of
        // MILLISECONDS at every output rate, which is the whole point of
        // separating the two clocks.
        self.chip_acc = self.chip_acc.wrapping_add(self.chip_per_out);
        let mut ticks = self.chip_acc >> 16;
        self.chip_acc &= 0xFFFF;
        // Bound the catch-up. This is not a poll and cannot spin: chip_per_out
        // is a constant below 8.0 for every rate this sink accepts (>= 8000 Hz
        // gives 6.2), so `ticks` is at most 7. The bound exists so that a
        // corrupted chip_per_out degrades into wrong timing rather than into a
        // long loop inside an audio callback.
        if ticks > 8 {
            ticks = 8;
        }
        for _ in 0..ticks {
            self.chip_ticks = self.chip_ticks.wrapping_add(1);
            for oi in 0..NUM_OPERATORS {
                self.eg_step(oi);
            }
        }

        self.am_phase = self.am_phase.wrapping_add(self.am_inc);
        self.vib_phase = self.vib_phase.wrapping_add(self.vib_inc);

        // Tremolo: a triangle 0..26 envelope units (4.875 dB) at full depth,
        // 0..1 unit at low depth. Applied as EXTRA attenuation, so it can only
        // ever make a note quieter, which is what the hardware does.
        let am_tri = tri_unit(self.am_phase); // 0..255
        let am_att = if self.am_depth != 0 {
            (am_tri as u16 * 26) >> 8
        } else {
            (am_tri as u16 * 1) >> 8
        };

        let mut acc: i32 = 0;
        for c in 0..NUM_CHANNELS {
            acc += self.channel_sample(c, am_att);
        }

        // Nine channels of up to +-4084 each is +-36756, well past i16. The
        // chip's own output is 16 bits after its DAC, and titles do not
        // normally key nine full-level carriers at once. Clamp rather than
        // wrap: a wrap turns a loud chord into white noise, which is the worst
        // possible failure mode and an unmistakable one, whereas a clamp is
        // ordinary clipping.
        //
        // The >> 1 gives ~4.5 simultaneous full-level channels of headroom
        // before clipping starts, which covers every corpus title measured.
        let v = acc >> 1;
        if v > 32767 {
            32767
        } else if v < -32768 {
            -32768
        } else {
            v as i16
        }
    }

    fn channel_sample(&mut self, c: usize, am_att: u16) -> i32 {
        let mi = c * 2;
        let ci = c * 2 + 1;
        let fb = self.chs[c].fb;
        let cnt = self.chs[c].cnt;

        // FEEDBACK. The modulator's own two most recent outputs are averaged
        // and fed back into its phase. Averaging TWO samples rather than using
        // the last one is the hardware's own one-pole smoothing; without it
        // high feedback settings turn into noise instead of the intended
        // brightening.
        let fb_in = if fb != 0 {
            let avg = (self.ops[mi].out_prev1 + self.ops[mi].out_prev2) >> 1;
            // fb 1..7 scales the average by 2^(fb-1) / 256 of a cycle.
            (avg << fb) >> 8
        } else {
            0
        };

        let m = self.op_sample(mi, fb_in, am_att);
        self.ops[mi].out_prev2 = self.ops[mi].out_prev1;
        self.ops[mi].out_prev1 = m;

        if cnt == 0 {
            // FM: the modulator's output is added DIRECTLY to the carrier's
            // 10-bit phase index, with no scaling. A full-scale modulator
            // (+-4084) therefore swings the carrier by almost four whole
            // cycles, which is the OPL's characteristically aggressive
            // modulation index and the reason its patches are so bright.
            //
            // This was `m >> 1` until 2026-08-20, i.e. half the correct
            // modulation depth. Nothing in the frequency tests could see it:
            // every one of them silences the modulator on purpose so that the
            // carrier is a pure sine. It is a TIMBRE error, and it is called
            // out here because a timbre error is exactly the kind that gets
            // discovered by ear months later and blamed on the instrument
            // patches rather than on the synthesiser.
            self.op_sample(ci, m, am_att)
        } else {
            // Additive: both operators sound, in parallel.
            m + self.op_sample(ci, 0, am_att)
        }
    }

    fn op_sample(&mut self, oi: usize, phase_mod: i32, am_att: u16) -> i32 {
        // The envelope was already advanced for this sample in next_sample(),
        // on the chip clock. It is NOT advanced here: doing it per operator
        // per sample is what tied the envelope rate to the output rate.
        let o = &mut self.ops[oi];
        if o.eg_state == Eg::Off {
            o.phase = o.phase.wrapping_add(o.phase_inc);
            return 0;
        }

        // Vibrato: +-7 cents at low depth, +-14 at high, as a phase-increment
        // trim. Cheap and correct in shape: the hardware's vibrato is also a
        // small periodic addition to the phase increment.
        let inc = if o.vib != 0 {
            let d = tri_signed(self.vib_phase); // -128..127
            let depth = if self.vib_depth != 0 { 14i64 } else { 7i64 };
            let trim = (o.phase_inc as i64 * d as i64 * depth) >> 18;
            (o.phase_inc as i64 + trim) as u32
        } else {
            o.phase_inc
        };
        o.phase = o.phase.wrapping_add(inc);

        let idx = (((o.phase >> 22) as i32).wrapping_add(phase_mod)) as u32 & 0x3FF;

        // Total attenuation for this operator, in envelope units.
        let mut env = o.eg_level + o.ksl_att + (o.tl as u16) * 4;
        if o.am != 0 {
            env += am_att;
        }
        if env > EG_MAX {
            env = EG_MAX;
        }

        let wave = if self.wave_select_enable { o.wave } else { 0 };
        wave_sample(wave, idx, env)
    }

    // -----------------------------------------------------------------------
    // ENVELOPE
    // -----------------------------------------------------------------------

    fn key_on(&mut self, c: usize) {
        for k in 0..2 {
            let oi = c * 2 + k;
            let o = &mut self.ops[oi];
            o.eg_state = Eg::Attack;
            // The chip resets the phase on key-on. This is audible: without it
            // repeated notes on one channel start at an arbitrary point in the
            // cycle and the attack transient is inconsistent.
            o.phase = 0;
            o.out_prev1 = 0;
            o.out_prev2 = 0;
            // An attack rate of 0 means "no attack": the envelope never moves,
            // so the note would be silent forever. The chip's behaviour is that
            // rate 0 holds; going straight to Decay at full volume would be an
            // invention. Held at EG_MAX, i.e. silent, which is what rate 0
            // actually gives you.
            if o.eg_level > EG_MAX {
                o.eg_level = EG_MAX;
            }
        }
    }

    fn key_off(&mut self, c: usize) {
        for k in 0..2 {
            let o = &mut self.ops[c * 2 + k];
            if o.eg_state != Eg::Off {
                o.eg_state = Eg::Release;
            }
        }
    }

    /// The effective rate for a 4-bit register field, with key-scale applied.
    fn eff_rate(&self, oi: usize, field: u8) -> u8 {
        if field == 0 {
            return 0; // rate 0 means the envelope does not advance at all
        }
        let c = oi / 2;
        let ch = &self.chs[c];
        // The key-scale-rate input: block in the high bits, plus ONE bit of
        // F-Number selected by NOTE-SEL, which decides where in the keyboard
        // the rate steps up.
        //
        // The selection is `fnum >> (9 - NTS)`, so NTS CLEAR picks bit 9 and
        // NTS SET picks bit 8. This file had it the other way round until
        // 2026-08-20. The error is nearly invisible: it only changes envelope
        // speed, only for operators with KSR set, and only for notes in the
        // half of each block on the far side of the split point. It would have
        // been a "some instruments decay slightly wrong in some octaves" report
        // with no reproducer.
        let fbit = if self.note_sel {
            ((ch.fnum >> 8) & 1) as u8
        } else {
            ((ch.fnum >> 9) & 1) as u8
        };
        let ksr_in = (ch.block << 1) | fbit;
        let rof = if self.ops[oi].ksr != 0 { ksr_in } else { ksr_in >> 2 };
        let r = (field as u16) * 4 + rof as u16;
        if r > 63 { 63 } else { r as u8 }
    }

    /// The envelope SHIFT for this chip tick at effective rate `r`, 0..3.
    ///
    /// Zero means "do not advance on this tick". A non-zero shift means
    /// different things to the two envelope shapes, which is why this returns
    /// the shift rather than an increment:
    ///
    ///   decay / sustain-decay / release   advance by  1 << (shift - 1) units
    ///   attack                            remove      (level + 1) >> (4 - shift)
    ///
    /// `raw` is the RAW 4-bit register field, before key-scaling. It is passed
    /// separately from `r` because R == 0 means "this phase never advances AT
    /// ALL", regardless of how much key-scale rate would otherwise add. An
    /// implementation that only checks the scaled rate makes AR=0 attack and
    /// RR=0 release on high notes, which is a stuck-note bug that appears only
    /// at the top of the keyboard.
    ///
    /// A PREVIOUS VERSION OF THIS FUNCTION HAD A HOLE IN THE MIDDLE OF ITS
    /// LADDER, and the shape will recur so it is recorded: it computed a
    /// divider exponent as `13 - (r >> 2)` in u32, which UNDERFLOWED for
    /// r = 56..59 to 4294967295. Rust masks an over-wide shift in release mode
    /// rather than trapping, so the mask became 0x7FFFFFFF and the envelope
    /// advanced on one tick in two billion, i.e. never. Release rate 14 did not
    /// decay while 13 and 15 both did. It was found by PRINTING THE WHOLE
    /// LADDER, not by any single-rate test, which is why EG_RATE_MONOTONE now
    /// asserts the ladder's shape and EG_RATE_ANCHOR pins one rung to a
    /// published figure.
    fn eg_shift(&self, r: u8, raw: u8) -> u32 {
        if raw == 0 {
            return 0;
        }
        let rate_hi = if (r >> 2) > 15 { 15u32 } else { (r >> 2) as u32 };
        let rate_lo = (r & 3) as u32;

        if rate_hi < 12 {
            // SLOW HALF. The divider still has room, so the speed comes from
            // how rarely the counter's low bits line up. Only alternate chip
            // ticks count here (`eg_state`), which is the factor of two the
            // first version of this file was missing.
            if (self.chip_ticks & 1) == 0 {
                return 0;
            }
            let timer = self.chip_ticks >> 1;
            if timer == 0 {
                return 0;
            }
            // ctz + 1 is how far up the counter the lowest set bit sits. When
            // rate_hi lifts it to exactly 12 the base advance fires; 13 and 14
            // are the two progressively rarer extra advances that rate_lo
            // switches on, giving an average multiplier of 1 + rate_lo / 4.
            let ctz = timer.trailing_zeros();
            let eg_add = if ctz > 12 { 0 } else { ctz + 1 };
            match rate_hi + eg_add {
                12 => 1,
                13 => (rate_lo >> 1) & 1,
                14 => rate_lo & 1,
                _ => 0,
            }
        } else {
            // FAST HALF. The divider has run out at one advance per chip tick,
            // so the speed comes from a BIGGER STEP instead.
            let timer_lo = ((self.chip_ticks >> 1) & 3) as usize;
            let mut s = (rate_hi & 3) + EG_INCSTEP[rate_lo as usize][timer_lo] as u32;
            if s & 4 != 0 {
                // Saturation, and it is why effective rates 60, 61, 62 and 63
                // are all the SAME speed rather than a continuing ladder.
                s = 3;
            }
            if s == 0 {
                // rate_hi == 12, rate_lo == 0: half a step per chip tick, i.e.
                // advance on alternate ticks. This is the `eg_state` bit again.
                s = self.chip_ticks & 1;
            }
            s
        }
    }

    fn eg_step(&mut self, oi: usize) {
        let state = self.ops[oi].eg_state;
        if state == Eg::Off {
            return;
        }
        let field = match state {
            Eg::Attack => self.ops[oi].ar,
            Eg::Decay => self.ops[oi].dr,
            Eg::Sustain => {
                // A PERCUSSIVE operator (EGT = 0) keeps decaying through the
                // sustain level at the RELEASE rate, which is why a piano-like
                // patch dies away while a held key is still down. A SUSTAINING
                // operator (EGT = 1) holds. Getting this backwards produces
                // organ patches that fade and piano patches that never stop,
                // and both are instantly recognisable as wrong.
                if self.ops[oi].egt != 0 {
                    return; // hold
                }
                self.ops[oi].rr
            }
            Eg::Release => self.ops[oi].rr,
            Eg::Off => return,
        };
        let r = self.eff_rate(oi, field);
        let shift = self.eg_shift(r, field);
        if shift == 0 {
            return;
        }

        let o = &mut self.ops[oi];
        match state {
            Eg::Attack => {
                // ATTACK removes a fixed FRACTION of the attenuation that is
                // still left: 1/8 at shift 1, 1/4 at shift 2, 1/2 at shift 3.
                // That is a geometric decay of the value in dB, which is the
                // saturating exponential rise in LINEAR amplitude that a real
                // instrument has. A linear fall here would sound like a slow
                // fade-in and would be wrong in an immediately audible way.
                //
                // The `+ 1` is not rounding. It guarantees forward progress at
                // every level: without it the step reaches zero once the level
                // drops below 8 and the attack STALLS a few dB short of full
                // volume, forever. That failure is completely invisible to a
                // frequency check, which is why ATTACK_PEAK exists as a
                // separate assertion with its own RED twin.
                let step = ((o.eg_level as u32) + 1) >> (4 - shift);
                let step = if step == 0 { 1u16 } else { step as u16 };
                if o.eg_level <= step {
                    o.eg_level = 0;
                    o.eg_state = Eg::Decay;
                } else {
                    o.eg_level -= step;
                }
            }
            _ => {
                // DECAY, SUSTAIN-DECAY and RELEASE are all LINEAR IN dB, i.e.
                // exponential in linear amplitude, at 1 << (shift - 1) units
                // per tick.
                let inc = 1u16 << (shift - 1);
                o.eg_level = o.eg_level.saturating_add(inc);
                if state == Eg::Decay {
                    let target = SL_TABLE[o.sl as usize];
                    if o.eg_level >= target {
                        o.eg_level = target;
                        o.eg_state = Eg::Sustain;
                    }
                }
                // The hardware's "envelope off" snap: once the top five bits
                // are all set the operator is inaudible, and it is forced to
                // full attenuation rather than being allowed to keep counting
                // and wrap the 9-bit value back round to LOUD.
                if o.eg_level >= 504 {
                    o.eg_level = EG_MAX;
                    o.eg_state = Eg::Off;
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // DERIVED-STATE REFRESH
    // -----------------------------------------------------------------------

    fn refresh_channel(&mut self, c: usize) {
        self.refresh_op(c * 2);
        self.refresh_op(c * 2 + 1);
    }

    /// Recompute an operator's phase increment and KSL attenuation from the
    /// channel's F-Number and Block. Called on every write that can change
    /// either, so the running state can never lag the register file.
    fn refresh_op(&mut self, oi: usize) {
        let c = oi / 2;
        let fnum = self.chs[c].fnum as u64;
        let block = self.chs[c].block as u32;
        let mult2 = MULT_X2[self.ops[oi].mult as usize] as u64;

        // THE FREQUENCY. From the F-Number/Block definition:
        //
        //   f_channel = FNUM * CHIP_RATE / 2^(20 - BLOCK)
        //
        // and the operator multiplies that by MULT. Expressed as a 32-bit
        // phase increment at the OUTPUT rate R, where a full 2^32 wrap is one
        // cycle:
        //
        //   inc = f_op * 2^32 / R
        //       = MULT * FNUM * CHIP_RATE * 2^BLOCK * 2^32 / (2^20 * R)
        //       = (MULT_X2/2) * FNUM * CHIP_RATE * 2^BLOCK * 2^12 / R
        //
        // and CHIP_RATE = CHIP_CLOCK_HZ / CHIP_CLOCK_DIV, so the whole thing is
        // ONE integer expression with a single division at the end. Doing the
        // division last is not a style preference: dividing early throws away
        // the low bits of the increment, which is a pitch error that grows with
        // the note and is the classic way to ship a synth that is slightly flat
        // at the top of the keyboard.
        //
        // Worst case numerator: 30 * 1023 * 3579545 * 128 * 2048 / 72
        //   = 4.0e14, which is 2^48.5. u64 has room and u32 does not, which is
        // why the intermediate is u64 and only the result is u32.
        let num = mult2
            * fnum
            * CHIP_CLOCK_HZ
            * (1u64 << block)
            * 2048u64;
        let den = CHIP_CLOCK_DIV * self.sample_rate as u64;
        let inc = if den == 0 { 0 } else { num / den };
        self.ops[oi].phase_inc = inc as u32;

        // KSL. The attenuation depends on the note, so it is recomputed here
        // rather than at the 0x40 write, which is why a pitch bend correctly
        // changes the level as well as the pitch.
        let ksl = self.ops[oi].ksl as usize;
        let sh = KSL_SHIFT[ksl];
        self.ops[oi].ksl_att = if sh >= 8 {
            0
        } else {
            // base = (KSL_ROM[fnum >> 6] << 2) - ((8 - block) << 5), floored
            // at zero, all in ENVELOPE units (0.1875 dB). The ROM is in 0.75 dB
            // units, hence the << 2; the block term is 32 envelope units, i.e.
            // exactly 6 dB, per octave.
            //
            // The block term is (8 - block), NOT (7 - block). This file used
            // (7 - block) until 2026-08-20, which left every key-scaled
            // operator 6 dB too loud at every block. Do the subtraction in the
            // SCALED domain and floor there: flooring the ROM before scaling
            // gives a different answer at the point where the term goes
            // negative.
            let rom = (KSL_ROM[((fnum >> 6) & 0x0F) as usize]) << 2;
            let drop = (8u16.saturating_sub(block as u16)) << 5;
            (rom.saturating_sub(drop)) >> sh
        };
    }
}

// ---------------------------------------------------------------------------
// FREE FUNCTIONS
// ---------------------------------------------------------------------------

/// Convert a frequency given as the rational num/den Hz into a 32-bit phase
/// increment at `rate`. Rational rather than float because the LFO rates
/// (3.7 Hz, 6.1 Hz) are not integers and this core has no float.
fn hz_to_inc(hz_num: u64, hz_den: u64, rate: u32) -> u32 {
    if rate == 0 || hz_den == 0 {
        return 0;
    }
    ((hz_num << 32) / (hz_den * rate as u64)) as u32
}

/// A 0..255 triangle from the top bits of a 32-bit phase.
fn tri_unit(phase: u32) -> u8 {
    let x = (phase >> 23) & 0x1FF; // 0..511
    if x < 256 { x as u8 } else { (511 - x) as u8 }
}

/// A -128..127 triangle from the top bits of a 32-bit phase.
fn tri_signed(phase: u32) -> i32 {
    tri_unit(phase) as i32 - 128
}

/// The exponent ROM lookup: turn a log-domain attenuation into a linear
/// amplitude.
///
/// EXPTAB holds 2^(i/256) - 1 scaled by 1024, so the implicit leading 1 is
/// re-added as | 0x400. The index is COMPLEMENTED because the table runs the
/// other way from attenuation: more attenuation is a smaller output.
///
/// The shift is clamped at 12 rather than allowed to reach 31. That is not
/// defensive decoration: `u32 >> 32` is undefined behaviour in Rust and
/// panics in a debug build, and the attenuation here reaches 9777 (logsin's
/// 2137 plus 511 envelope units times 8), which is a shift of 38. The clamp is
/// exact rather than approximate, because the maximum value 4084 shifted right
/// by 12 is already 0.
fn exp_lookup(att: u32) -> i32 {
    let sh = att >> 8;
    if sh > 12 {
        return 0;
    }
    let idx = (!att & 0xFF) as usize;
    (((EXPTAB[idx] as u32 | 0x400) << 1) >> sh) as i32
}

/// One operator sample: quarter-wave lookup, waveform shaping, sign, exponent.
///
/// `idx` is the 10-bit phase index and `env` the total attenuation in envelope
/// units (0.1875 dB). The `env << 3` converts envelope units into the exponent
/// table's units of 1/256 of a log2, since 0.1875 dB / (6.0206 dB / 256) = 8
/// exactly.
fn wave_sample(wave: u8, idx: u32, env: u16) -> i32 {
    let quarter = (idx & 0xFF) as usize;
    let half = (idx & 0x100) != 0; // second quarter of this half
    let neg = (idx & 0x200) != 0; // second half of the cycle

    // The four OPL2 waveforms, all built from the SAME quarter-wave ROM:
    //   0  full sine
    //   1  half sine     : the negative half is silenced
    //   2  absolute sine : the negative half is mirrored positive
    //   3  pulse sine    : only the first quarter of each half survives
    let (logv, negate) = match wave {
        1 => {
            if neg {
                return 0;
            }
            (if half { LOGSIN[255 - quarter] } else { LOGSIN[quarter] }, false)
        }
        2 => (if half { LOGSIN[255 - quarter] } else { LOGSIN[quarter] }, false),
        3 => {
            if half {
                return 0;
            }
            (LOGSIN[quarter], false)
        }
        _ => (if half { LOGSIN[255 - quarter] } else { LOGSIN[quarter] }, neg),
    };

    let att = logv as u32 + ((env as u32) << 3);
    let v = exp_lookup(att);
    if negate {
        -v
    } else {
        v
    }
}

// ===========================================================================
// WHAT IS DELIBERATELY NOT HERE, stated so nobody reports it as a bug or,
// worse, quietly adds a second core to get it.
// ---------------------------------------------------------------------------
//   * RHYTHM MODE (reg 0xBD bits 5..0: bass drum, snare, tom, cymbal, hi-hat).
//     The two noise-based percussion operators need a separate noise generator
//     and a different operator routing. NOT ONE TITLE in the measured corpus
//     enables it; see the register census in the CHANGELOG. The depth bits of
//     0xBD, which DO affect the melodic channels, are implemented.
//   * CSM MODE (reg 0x08 bit 7). Keys every channel from timer 1 for speech
//     synthesis. Unused by the corpus, and it would need the OPL timers, which
//     live in kernel/rustkern/opl2.rs on the other side of the syscall.
//   * THE TIMERS AND THE STATUS REGISTER. They stay in rustkern/opl2.rs where
//     #175 put them, because DETECTION has to work with or without this core,
//     and because a guest's timer poll must be answered at guest speed, not at
//     audio-block granularity.
//   * CYCLE-EXACT OUTPUT. See init() above.
// ===========================================================================
