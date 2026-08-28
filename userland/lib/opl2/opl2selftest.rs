// opl2selftest.rs - #182: the FM core's test scenarios, run IDENTICALLY by the
// host harness (userland/lib/opl2/hosttest) and by the Ring-3 app /APPS/FMTEST.
//
// ONE suite, two runners, for the same reason there is one core: a test that
// exists twice drifts, and the arm you did not update is the arm that lies.
//
// EVERY POSITIVE CHECK HERE HAS A NEGATIVE TWIN.
// A test that can only pass has not been run, it has been asserted. #175's own
// detection self-test is built the same way and the comment there says so.
// Eighteen instruments were caught lying in four days on this project and the
// two most recent MANUFACTURED their ticket's central claim, so:
//
//   FM_A440         must PASS      <->  FM_A440_RED_FNUM   must FAIL the same check
//   ENV_DECAY       must PASS      <->  ENV_RED_HOLD       must FAIL the same check
//   ATTACK_PEAK     must PASS      <->  ATTACK_RED_STALL   must FAIL the same check
//
// The RED twins are not skipped, not commented out, and not "expected
// failures" in a report footnote: each one RUNS the identical assertion
// against a deliberately broken input and the suite FAILS IF IT PASSES.


// ---------------------------------------------------------------------------
// The A440 patch, register by register, so it can be read against the map.
// ---------------------------------------------------------------------------
//
// F-Number and Block for 440 Hz, derived not guessed:
//   f = FNUM * CHIP_RATE / 2^(20 - BLOCK),  CHIP_RATE = 3579545 / 72
//   FNUM = f * 2^(20 - BLOCK) / CHIP_RATE
//   BLOCK = 4:  FNUM = 440 * 65536 / 49715.9028 = 580.012  ->  580
//   back-substituting: 580 * 49715.9028 / 65536 = 439.9907 Hz, -0.037 cents.
// Block 4 is chosen because it puts F-Number near the middle of its 10-bit
// range, where the quantisation error is smallest. Block 0 would give
// FNUM = 9280, which does not fit, and block 7 would give FNUM = 72, whose
// nearest neighbours are 6 cents apart.
pub const A440_FNUM: u16 = 580;
pub const A440_BLOCK: u8 = 4;

/// The F-Number the RED arm uses: the same FNUM one block higher, i.e. exactly
/// one octave sharp. Chosen to be a plausible-looking mistake (an off-by-one in
/// the block field is the single most common F-Number bug) rather than an
/// absurd value, so the RED arm proves the check discriminates at a realistic
/// error and not merely at nonsense.
pub const A440_RED_BLOCK: u8 = 5;

pub struct Report {
    pub name: &'static str,
    pub pass: bool,
    /// The measured quantity, in whatever unit `note` describes.
    pub measured: i64,
    /// The expected quantity or bound.
    pub expected: i64,
    pub note: &'static str,
}

/// Program one channel as a PURE SINE at the given F-Number/Block.
///
/// The modulator is attenuated to TL = 63 (-47.25 dB) so it contributes no
/// meaningful phase modulation and the carrier is an unmodulated sine. That is
/// required by dominant_mhz's stated limit, and doing it here rather than in
/// each test means every frequency assertion in the suite shares one patch.
pub fn patch_pure_sine(chip: &mut Opl2, ch: u8, fnum: u16, block: u8, ar: u8, dr: u8, sl: u8, rr: u8, egt: u8) {
    let m = OP_OFFSET[ch as usize] as u8;
    let c = m + 3;
    chip.write_reg(0x01, 0x20); // waveform select enabled
    // Modulator: silenced, but still given a real envelope so it is not in a
    // degenerate state that could mask a bug in the operator path.
    chip.write_reg(0x20 + m, 0x20 | 0x01); // EGT=1 sustaining, MULT=1
    chip.write_reg(0x40 + m, 0x3F);        // KSL=0, TL=63 -> inaudible
    chip.write_reg(0x60 + m, 0xF0);        // AR=15, DR=0
    chip.write_reg(0x80 + m, 0x0F);        // SL=0, RR=15
    chip.write_reg(0xE0 + m, 0x00);        // full sine
    // Carrier: the tone under test.
    chip.write_reg(0x20 + c, if egt != 0 { 0x20 | 0x01 } else { 0x01 });
    chip.write_reg(0x40 + c, 0x00);        // KSL=0, TL=0 -> full level
    chip.write_reg(0x60 + c, (ar << 4) | (dr & 0x0F));
    chip.write_reg(0x80 + c, (sl << 4) | (rr & 0x0F));
    chip.write_reg(0xE0 + c, 0x00);        // full sine
    chip.write_reg(0xC0 + ch, 0x00);       // FB=0, CNT=0 (FM: only the carrier sounds)
    chip.write_reg(0xA0 + ch, (fnum & 0xFF) as u8);
    chip.write_reg(0xB0 + ch, ((fnum >> 8) & 0x03) as u8 | (block << 2));
}

pub fn key_on(chip: &mut Opl2, ch: u8, fnum: u16, block: u8) {
    chip.write_reg(0xB0 + ch, ((fnum >> 8) & 0x03) as u8 | (block << 2) | 0x20);
}

pub fn key_off(chip: &mut Opl2, ch: u8, fnum: u16, block: u8) {
    chip.write_reg(0xB0 + ch, ((fnum >> 8) & 0x03) as u8 | (block << 2));
}

/// The exact expected frequency in millihertz for an F-Number/Block pair,
/// computed from the chip constants in integer. Not a literal 440000: the
/// nearest F-Number to A440 is 439.99 Hz and asserting against a rounded 440
/// would either need a looser tolerance or would be asserting against a number
/// the chip cannot produce.
pub fn expected_mhz(fnum: u16, block: u8) -> u64 {
    (fnum as u64 * CHIP_CLOCK_HZ * 1000 * (1u64 << block)) / (CHIP_CLOCK_DIV * (1u64 << 20))
}

/// The whole suite. `buf` is the render scratch (the caller owns it, because
/// it must live in .bss in the Ring-3 arm: a 64 KB buffer on a 16 KB stack is
/// the exact fault that bit the earlier Rust userland port), `env` is the
/// envelope scratch, and `emit` receives one Report per check.
///
/// Returns the number of FAILING checks, so 0 is the pass, matching the
/// convention dos_opl2_selftest_rs already uses in the kernel.
pub fn run_all(rate: u32, buf: &mut [i16], env: &mut [u16], emit: &mut dyn FnMut(&Report)) -> i32 {
    let mut fails = 0i32;
    let mut rep = |r: Report, fails: &mut i32| {
        if !r.pass {
            *fails += 1;
        }
        emit(&r);
    };

    let want = expected_mhz(A440_FNUM, A440_BLOCK) as i64;

    // ---- 1. SILENCE AT RESET -------------------------------------------
    // A core that emits anything before a single note is keyed is broken in a
    // way that would also make every other measurement here meaningless.
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        let n = if buf.len() > 4096 { 4096 } else { buf.len() };
        chip.render(&mut buf[..n]);
        let mut nonzero = 0i64;
        for s in &buf[..n] {
            if *s != 0 {
                nonzero += 1;
            }
        }
        rep(Report {
            name: "RESET_SILENT",
            pass: nonzero == 0,
            measured: nonzero,
            expected: 0,
            note: "nonzero samples before any key-on",
        }, &mut fails);
    }

    // ---- 2. FM_A440: the headline claim --------------------------------
    // Tolerance: 100 millihertz, i.e. 0.023%, i.e. 0.39 cents. That is far
    // tighter than any audible error and far looser than the estimator's own
    // resolution, so it can only fail on a real arithmetic mistake.
    let a440_measured;
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, 0, 1);
        key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);
        chip.render(buf);
        a440_measured = dominant_mhz(buf, rate) as i64;
        rep(Report {
            name: "FM_A440",
            pass: (a440_measured - want).abs() <= 100,
            measured: a440_measured,
            expected: want,
            note: "millihertz at the sink; tolerance 100 mHz (0.39 cents)",
        }, &mut fails);
    }

    // ---- 3. FM_A440_RED_FNUM: PROVE THE CHECK CAN FAIL -----------------
    // The identical assertion against a block that is one too high. If this
    // "passes", the frequency check is not measuring frequency and every green
    // above it is worthless.
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_RED_BLOCK, 15, 0, 0, 0, 1);
        key_on(&mut chip, 0, A440_FNUM, A440_RED_BLOCK);
        chip.render(buf);
        let m = dominant_mhz(buf, rate) as i64;
        let would_pass = (m - want).abs() <= 100;
        rep(Report {
            name: "FM_A440_RED_FNUM",
            pass: !would_pass,
            measured: m,
            expected: want,
            note: "RED ARM: wrong block must NOT satisfy the A440 check",
        }, &mut fails);
    }

    // ---- 4. OCTAVE: block + 1 doubles the frequency ---------------------
    // Independent of the absolute A440 result: it checks the 2^BLOCK term
    // specifically, which is the half of the formula the RED arm perturbs.
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK + 1, 15, 0, 0, 0, 1);
        key_on(&mut chip, 0, A440_FNUM, A440_BLOCK + 1);
        chip.render(buf);
        let m = dominant_mhz(buf, rate) as i64;
        let err = (m - 2 * want).abs();
        rep(Report {
            name: "OCTAVE_UP",
            pass: err <= 200,
            measured: m,
            expected: 2 * want,
            note: "block+1 must be exactly one octave up",
        }, &mut fails);
    }

    // ---- 5. MULT: the operator frequency multiplier ---------------------
    // MULT = 2 on the carrier must double the pitch. This exercises MULT_X2,
    // which the A440 test cannot: it uses MULT = 1 throughout.
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, 0, 1);
        let c = OP_OFFSET[0] as u8 + 3;
        chip.write_reg(0x20 + c, 0x20 | 0x02); // EGT=1, MULT=2
        key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);
        chip.render(buf);
        let m = dominant_mhz(buf, rate) as i64;
        rep(Report {
            name: "MULT_2",
            pass: (m - 2 * want).abs() <= 200,
            measured: m,
            expected: 2 * want,
            note: "MULT=2 must double the operator frequency",
        }, &mut fails);
    }

    // ---- 6. MULT_HALF: the code-0 special case --------------------------
    // MULT code 0 is x0.5, NOT x0. Getting this wrong silences every patch
    // that uses it and is invisible to a test that only tries MULT >= 1.
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, 0, 1);
        let c = OP_OFFSET[0] as u8 + 3;
        chip.write_reg(0x20 + c, 0x20 | 0x00); // EGT=1, MULT=0 -> x0.5
        key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);
        chip.render(buf);
        let m = dominant_mhz(buf, rate) as i64;
        rep(Report {
            name: "MULT_HALF",
            pass: (m - want / 2).abs() <= 200,
            measured: m,
            expected: want / 2,
            note: "MULT code 0 is x0.5, not x0",
        }, &mut fails);
    }

    // ---- 7. TL: 8 steps of 0.75 dB is 6 dB, i.e. half the amplitude -----
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, 0, 1);
        key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);
        let n = if buf.len() > 8192 { 8192 } else { buf.len() };
        chip.render(&mut buf[..n]);
        let (_, loud) = {
            let k = envelope(&buf[..n], 1024, env);
            envelope_peak(&env[..k])
        };

        let mut chip2 = Opl2::new();
        chip2.init(rate);
        patch_pure_sine(&mut chip2, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, 0, 1);
        let c = OP_OFFSET[0] as u8 + 3;
        chip2.write_reg(0x40 + c, 0x08); // TL = 8 -> -6 dB
        key_on(&mut chip2, 0, A440_FNUM, A440_BLOCK);
        chip2.render(&mut buf[..n]);
        let (_, quiet) = {
            let k = envelope(&buf[..n], 1024, env);
            envelope_peak(&env[..k])
        };

        // ratio x100, expected 200 (twice as loud). +-8% band.
        let ratio = if quiet == 0 { 0 } else { (loud as i64 * 100) / quiet as i64 };
        rep(Report {
            name: "TL_6DB",
            pass: ratio >= 184 && ratio <= 216,
            measured: ratio,
            expected: 200,
            note: "amplitude ratio x100 for TL 0 vs TL 8 (6 dB)",
        }, &mut fails);
    }

    // ---- 8. ATTACK_PEAK: the attack must ARRIVE -------------------------
    // The failure this catches is an attack whose step reaches zero before the
    // envelope does, so the note stalls a few dB short of full volume forever.
    // It is inaudible as a defect, invisible to a frequency check, and it is
    // precisely why the +1 exists in Opl2::eg_step.
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, 0, 1);
        key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);
        let n = if buf.len() > 16384 { 16384 } else { buf.len() };
        chip.render(&mut buf[..n]);
        let k = envelope(&buf[..n], 512, env);
        let (_, peak) = envelope_peak(&env[..k]);
        // A single full-level carrier is 4084 at the operator, halved by the
        // output stage, so ~2042. Anything below 1800 means the attack never
        // arrived at full volume.
        rep(Report {
            name: "ATTACK_PEAK",
            pass: peak >= 1800,
            measured: peak as i64,
            expected: 1800,
            note: "peak amplitude of a full-level carrier at AR=15",
        }, &mut fails);
    }

    // ---- 9. ATTACK_RED_STALL: PROVE THAT CHECK CAN FAIL -----------------
    // AR = 1 is the slowest non-zero attack. Over the same short window the
    // envelope cannot possibly have arrived, so the identical assertion must
    // fail. If it passes, ATTACK_PEAK is not measuring the attack.
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 1, 0, 0, 0, 1);
        key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);
        let n = if buf.len() > 16384 { 16384 } else { buf.len() };
        chip.render(&mut buf[..n]);
        let k = envelope(&buf[..n], 512, env);
        let (_, peak) = envelope_peak(&env[..k]);
        rep(Report {
            name: "ATTACK_RED_STALL",
            pass: peak < 1800,
            measured: peak as i64,
            expected: 1800,
            note: "RED ARM: AR=1 must NOT reach full volume in this window",
        }, &mut fails);
    }

    // ---- 10. ENV_DECAY: a note must actually STOP -----------------------
    // The headline envelope claim. Key on, let it establish, key off, and
    // require the tail to fall to silence. A note that starts and never decays
    // passes every frequency check in this file and is still wrong.
    let decay_idx: i64;
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        // RR = 12: a fast but not instant release, so there is a real decay to
        // observe rather than an abrupt cut that would pass trivially.
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, 12, 1);
        key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);
        let half = buf.len() / 2;
        chip.render(&mut buf[..half]);
        key_off(&mut chip, 0, A440_FNUM, A440_BLOCK);
        chip.render(&mut buf[half..]);
        let k = envelope(buf, 512, env);
        let koff_win = half / 512;
        let sil = envelope_silence_at(&env[..k], koff_win, 16);
        decay_idx = match sil {
            Some(i) => (i - koff_win) as i64,
            None => -1,
        };
        rep(Report {
            name: "ENV_DECAY",
            pass: sil.is_some(),
            measured: decay_idx,
            expected: 0,
            note: "512-sample windows from key-off to silence; -1 = NEVER decayed",
        }, &mut fails);
    }

    // ---- 11. ENV_DECAY_MONOTONE: it must FALL, not cut ------------------
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, 7, 1);
        key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);
        let half = buf.len() / 2;
        chip.render(&mut buf[..half]);
        key_off(&mut chip, 0, A440_FNUM, A440_BLOCK);
        chip.render(&mut buf[half..]);
        let k = envelope(buf, 512, env);
        let koff_win = half / 512;
        let end = if koff_win + 40 < k { koff_win + 40 } else { k };
        let ok = envelope_is_decaying(&env[..k], koff_win + 1, end, 3);
        rep(Report {
            name: "ENV_DECAY_SHAPE",
            pass: ok,
            measured: env[if end > 0 { end - 1 } else { 0 }] as i64,
            expected: (env[koff_win] / 2) as i64,
            note: "release must FALL through the window, not hold then cut",
        }, &mut fails);
    }

    // ---- 12. ENV_RED_HOLD: PROVE THE DECAY CHECK CAN FAIL ---------------
    // A SUSTAINING operator (EGT = 1) with DR = 0 and the key still DOWN must
    // hold forever. Running the identical silence assertion against it must
    // FAIL. This is the negative twin of ENV_DECAY and it is what makes that
    // green mean something.
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, 0, 1);
        key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);
        chip.render(buf); // never keyed off
        let k = envelope(buf, 512, env);
        let koff_win = (buf.len() / 2) / 512;
        let sil = envelope_silence_at(&env[..k], koff_win, 16);
        rep(Report {
            name: "ENV_RED_HOLD",
            pass: sil.is_none(),
            measured: match sil { Some(i) => i as i64, None => -1 },
            expected: -1,
            note: "RED ARM: a held sustaining note must NOT reach silence",
        }, &mut fails);
    }

    // ---- 13. ENV_PERCUSSIVE: EGT = 0 decays with the key still DOWN -----
    // The difference between a piano and an organ, and a real corpus concern:
    // getting EGT backwards makes every percussive instrument sustain forever.
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        // EGT = 0 (percussive), SL = 0 so decay runs straight into the
        // sustain phase, which for a percussive operator keeps falling at RR.
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 15, 10, 0, 10, 0);
        key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);
        chip.render(buf); // key stays DOWN for the whole buffer
        let k = envelope(buf, 512, env);
        let sil = envelope_silence_at(&env[..k], 8, 16);
        rep(Report {
            name: "ENV_PERCUSSIVE",
            pass: sil.is_some(),
            measured: match sil { Some(i) => i as i64, None => -1 },
            expected: 0,
            note: "EGT=0 must decay to silence with the key still held down",
        }, &mut fails);
    }

    // ---- 14. WAVEFORM 1 (half sine) has no negative half ----------------
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, 0, 1);
        let c = OP_OFFSET[0] as u8 + 3;
        chip.write_reg(0xE0 + c, 0x01); // half sine
        key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);
        let n = if buf.len() > 8192 { 8192 } else { buf.len() };
        chip.render(&mut buf[..n]);
        let mut neg = 0i64;
        let mut pos = 0i64;
        for s in &buf[..n] {
            if *s < -8 { neg += 1; }
            if *s > 8 { pos += 1; }
        }
        rep(Report {
            name: "WAVE_HALF_SINE",
            pass: neg == 0 && pos > 0,
            measured: neg,
            expected: 0,
            note: "waveform 1 must have no negative samples (and some positive)",
        }, &mut fails);
    }

    // ---- 15. WAVE_SELECT_GATE: reg 0x01 bit 5 must actually gate --------
    // With wave select DISABLED, waveform 1 must be ignored and the output must
    // go back to a full sine, i.e. negative samples reappear. An OPL2 that
    // honours 0xE0 without 0x01 bit 5 mis-plays every pre-wave-select title.
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, 0, 1);
        let c = OP_OFFSET[0] as u8 + 3;
        chip.write_reg(0xE0 + c, 0x01); // ask for half sine
        chip.write_reg(0x01, 0x00);     // but DISABLE wave select
        key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);
        let n = if buf.len() > 8192 { 8192 } else { buf.len() };
        chip.render(&mut buf[..n]);
        let mut neg = 0i64;
        for s in &buf[..n] {
            if *s < -8 { neg += 1; }
        }
        rep(Report {
            name: "WAVE_SELECT_GATE",
            pass: neg > 0,
            measured: neg,
            expected: 1,
            note: "0x01 bit 5 clear must force a full sine regardless of 0xE0",
        }, &mut fails);
    }

    // ---- 16. OPERATOR MAP: writing a GAP address must not alias ---------
    // Register offsets 0x06, 0x07, 0x0E, 0x0F, 0x16..0x1F are holes in the
    // operator address space. A decoder that computes an operator index
    // arithmetically instead of via a table maps them onto real operators, so a
    // stray write silently reprograms an instrument. Assert the hole is a hole.
    {
        let mut chip = Opl2::new();
        chip.init(rate);
        patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, 0, 1);
        key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);
        let n = if buf.len() > 8192 { 8192 } else { buf.len() };
        chip.render(&mut buf[..n]);
        let k = envelope(&buf[..n], 1024, env);
        let (_, before) = envelope_peak(&env[..k]);

        let mut chip2 = Opl2::new();
        chip2.init(rate);
        patch_pure_sine(&mut chip2, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, 0, 1);
        // Slam every gap address with full attenuation. If any of them aliases
        // onto channel 0's carrier, the tone goes quiet.
        for gap in [0x06u8, 0x07, 0x0E, 0x0F] {
            chip2.write_reg(0x40 + gap, 0x3F);
        }
        key_on(&mut chip2, 0, A440_FNUM, A440_BLOCK);
        chip2.render(&mut buf[..n]);
        let k2 = envelope(&buf[..n], 1024, env);
        let (_, after) = envelope_peak(&env[..k2]);
        rep(Report {
            name: "OPMAP_GAPS_INERT",
            pass: after == before && before > 0,
            measured: after as i64,
            expected: before as i64,
            note: "writes to operator-map holes must change nothing",
        }, &mut fails);
    }

    // ---- 17. EG_RATE_MONOTONE: the whole rate ladder, not one rung --------
    //
    // THIS TEST EXISTS BECAUSE IT CAUGHT A REAL BUG, and it is the shape of
    // test the rest of this suite was missing.
    //
    // Every check above picks ONE envelope rate and asserts something about it.
    // All of them passed while release rates 14 through 14 (effective 56..59)
    // never decayed at all, because eg_inc's divider exponent underflowed there
    // and the envelope advanced on one sample in two billion. A hole in the
    // middle of a monotone ladder is invisible to any test that stands on one
    // rung.
    //
    // The assertion is a SHAPE, not a value: a larger release rate must leave
    // strictly no MORE signal after a fixed time than a smaller one. That is
    // true of the hardware by definition, it needs no timing constant to be
    // correct, and it is exactly what a hole violates.
    {
        let mut prev: i64 = i64::MAX;
        let mut worst_rr: i64 = -1;
        let mut worst_val: i64 = 0;
        let win = 4096usize;
        if buf.len() >= 2048 + win {
            for rr in 1u8..16 {
                let mut chip = Opl2::new();
                chip.init(rate);
                patch_pure_sine(&mut chip, 0, A440_FNUM, A440_BLOCK, 15, 0, 0, rr, 1);
                key_on(&mut chip, 0, A440_FNUM, A440_BLOCK);
                chip.render(&mut buf[..2048]); // let the attack settle
                key_off(&mut chip, 0, A440_FNUM, A440_BLOCK);
                chip.render(&mut buf[..win]);
                // The peak of the LAST window is what is still sounding after a
                // fixed time. Lower is faster.
                let k = envelope(&buf[..win], 512, env);
                let residual = if k > 0 { env[k - 1] as i64 } else { -1 };
                if residual > prev && worst_rr < 0 {
                    worst_rr = rr as i64;
                    worst_val = residual;
                }
                prev = residual;
            }
        }
        rep(Report {
            name: "EG_RATE_MONOTONE",
            pass: worst_rr < 0,
            measured: worst_rr,
            expected: -1,
            note: "first RR that decays SLOWER than RR-1; -1 = ladder intact",
        }, &mut fails);
        let _ = worst_val;
    }

    // ---- 18. EG_RATE_ANCHOR: pin one rung to a PUBLISHED number ----------
    //
    // EG_RATE_MONOTONE proves the ladder has no holes. It does NOT prove the
    // ladder is in the right place: a version of this core that was uniformly
    // 2.25x too slow passed a monotonicity check perfectly, because being
    // uniformly wrong preserves the ordering. That is what actually happened
    // here on 2026-08-20, and it is why a shape assertion and a magnitude
    // assertion are two different tests.
    //
    // THE ANCHOR: at effective rate 48 the envelope advances at exactly half
    // the chip's sample rate, 24857.95 units per second, so a full sweep from
    // full volume to the silence snap at 504 units takes
    //   504 / 24857.95 = 20.27 ms.
    // (The often-quoted 20.56 ms is the full 511 units; this core stops at the
    // hardware's 504-unit snap, which is where it becomes inaudible.)
    //
    // Effective rate 48 is reached with RR = 12 and a key-scale offset of ZERO,
    // which needs block <= 1 and F-Number bit 9 clear. Block 1 with F-Number
    // 300 gives exactly that: ksr_in = 2, rof = 2 >> 2 = 0, eff = 4*12 = 48.
    // The note itself is inaudibly low; only its envelope is under test.
    //
    // Tolerance is +-12%, which is wide enough to absorb the 504-vs-511
    // question and the sample quantisation, and nowhere near wide enough to
    // absorb the factor of 2.25 this exists to catch.
    {
        const ANCHOR_FNUM: u16 = 300; // bit 9 clear
        const ANCHOR_BLOCK: u8 = 1;   // ksr_in = 2, so rof = 0
        let mut chip = Opl2::new();
        chip.init(rate);
        patch_pure_sine(&mut chip, 0, ANCHOR_FNUM, ANCHOR_BLOCK, 15, 0, 0, 12, 1);
        key_on(&mut chip, 0, ANCHOR_FNUM, ANCHOR_BLOCK);
        // AR = 15 is effectively instant and DR = 0 never advances, so the
        // envelope parks at full volume. 512 samples is far more than enough.
        chip.render(&mut buf[..512]);
        key_off(&mut chip, 0, ANCHOR_FNUM, ANCHOR_BLOCK);
        let mut n: u64 = 0;
        let step = 64usize;
        let cap = rate as u64; // one second is 50x the expected answer
        while n < cap && !chip.is_silent() {
            chip.render(&mut buf[..step]);
            n += step as u64;
        }
        let us = if n >= cap { -1 } else { (n * 1_000_000 / rate as u64) as i64 };
        rep(Report {
            name: "EG_RATE_ANCHOR",
            pass: us >= 17_800 && us <= 22_700,
            measured: us,
            expected: 20_270,
            note: "microseconds for a full release at effective rate 48 (published 20270)",
        }, &mut fails);
    }

    // ---- 19. KSL_ORDER: the field's numeric order is NOT its strength -----
    //
    // KSL 01 is 3.0 dB/octave and KSL 10 is 1.5 dB/octave, so setting 1
    // attenuates MORE than setting 2. This core had the two swapped until
    // 2026-08-20 and the comment above the table described the trap while the
    // table fell into it.
    //
    // Asserting the ORDER rather than the dB values makes this test immune to
    // the exact ROM contents while still catching the only mistake anyone
    // actually makes here. A high note (block 7, F-Number 1023) is used
    // because KSL does nothing at the bottom of the keyboard, which is
    // precisely why a swap can hide.
    {
        const KFNUM: u16 = 1023;
        const KBLOCK: u8 = 7;
        let mut peaks = [0u16; 4];
        let n = if buf.len() > 8192 { 8192 } else { buf.len() };
        for k in 0..4u8 {
            let mut chip = Opl2::new();
            chip.init(rate);
            patch_pure_sine(&mut chip, 0, KFNUM, KBLOCK, 15, 0, 0, 0, 1);
            let c = OP_OFFSET[0] as u8 + 3;
            chip.write_reg(0x40 + c, k << 6); // KSL = k, TL = 0
            key_on(&mut chip, 0, KFNUM, KBLOCK);
            chip.render(&mut buf[..n]);
            let kk = envelope(&buf[..n], 1024, env);
            let (_, p) = envelope_peak(&env[..kk]);
            peaks[k as usize] = p;
        }
        // Required order, loudest first: KSL 0 (off) > KSL 2 (1.5 dB/oct)
        //                              > KSL 1 (3 dB/oct) > KSL 3 (6 dB/oct)
        let ok = peaks[0] > peaks[2] && peaks[2] > peaks[1] && peaks[1] > peaks[3];
        // Report the DISCRIMINATING quantity, not a packed blob. peaks[2] is
        // KSL 10 (1.5 dB/oct) and peaks[1] is KSL 01 (3 dB/oct), so setting 2
        // must be the LOUDER of the two and this difference must be positive.
        // A swapped table makes it negative, and the number in the report then
        // says exactly what went wrong instead of needing to be decoded.
        rep(Report {
            name: "KSL_ORDER",
            pass: ok,
            measured: peaks[2] as i64 - peaks[1] as i64,
            expected: 1,
            note: "peak(KSL=2) - peak(KSL=1); must be POSITIVE (1.5 dB/oct is quieter than 3)",
        }, &mut fails);
    }

    let _ = decay_idx;
    fails
}
