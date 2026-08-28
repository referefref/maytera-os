// opl2check.rs - #182: the OBJECTIVE test of the FM core. Shared by the host
// harness and by the in-OS /APPS/FMTEST, so both arms run the SAME analysis.
//
// THE POINT OF THIS FILE
// ----------------------
// "FM synthesis works" is not a testable claim and neither is "I heard music".
// These are:
//
//   * Program A440. Capture the samples at the sink. The DOMINANT FREQUENCY
//     must be 440 Hz within a stated tolerance.
//   * Key the note off. The ENVELOPE must actually decay to silence, within a
//     stated time. A note that starts and never stops passes a frequency
//     check and is still wrong, which is why this is a separate assertion and
//     not an afterthought.
//
// Both are falsifiable without a speaker, and neither can be satisfied by
// "the stream opened" or "DMA started". #174 measured a scheduler change that
// left a task on no queue at all and it still reached DESKTOP_READY with no
// panic and no error counter, so reaching the end of a code path proves
// nothing at all.
//
// EVERY CHECK HERE IS INTEGER. That is what lets the host harness and the
// Ring-3 app run bit-identical analysis on bit-identical synthesis, so a green
// host run is evidence about the shipped ELF and not about a lookalike.


/// Estimate the fundamental frequency of a periodic signal, in MILLIHERTZ.
///
/// METHOD: positive-going zero crossings. For a signal with one dominant
/// component and no DC offset this is exact to within the sample grid, it is
/// pure integer, and it is auditable by inspection, which a DFT is not. It
/// measures from the FIRST crossing to the LAST rather than over the whole
/// buffer, so a partial cycle at either end cannot bias the result.
///
/// Returns 0 if fewer than three crossings were found, i.e. "no periodic
/// signal", which is a distinct answer from "the wrong frequency" and must not
/// be confused with one. A silent buffer returns 0 and every frequency
/// assertion against it fails, which is the correct outcome.
///
/// LIMIT, stated because it decides where this may be used: it is a
/// FUNDAMENTAL estimator, not a spectrum. A heavily FM-modulated waveform can
/// cross zero more than twice per cycle and this will read high. Every
/// frequency assertion in this ticket therefore uses a patch whose modulator is
/// attenuated to silence, making the carrier a pure sine. That is a deliberate
/// restriction of the test, not an unnoticed hole in it.
pub fn dominant_mhz(samples: &[i16], rate: u32) -> u64 {
    if samples.len() < 4 || rate == 0 {
        return 0;
    }
    let mut first: usize = 0;
    let mut last: usize = 0;
    let mut count: u64 = 0;
    let mut prev = samples[0];
    for i in 1..samples.len() {
        let cur = samples[i];
        if prev < 0 && cur >= 0 {
            if count == 0 {
                first = i;
            }
            last = i;
            count += 1;
        }
        prev = cur;
    }
    if count < 3 || last <= first {
        return 0;
    }
    // count crossings span (count - 1) whole periods over (last - first)
    // samples.
    let periods = count - 1;
    let span = (last - first) as u64;
    (periods * rate as u64 * 1000) / span
}

/// Peak absolute amplitude in each window of `win` samples. Writes at most
/// `out.len()` windows and returns how many it wrote.
///
/// PEAK rather than RMS: peak needs no multiply, no accumulator wider than the
/// sample, and no square root, so it is trivially correct. For an envelope
/// shape, which is what is being checked, peak and RMS differ by a constant
/// factor and the assertions below are all about SHAPE.
pub fn envelope(samples: &[i16], win: usize, out: &mut [u16]) -> usize {
    if win == 0 {
        return 0;
    }
    let mut n = 0;
    let mut i = 0;
    while i + win <= samples.len() && n < out.len() {
        let mut peak: u16 = 0;
        for s in &samples[i..i + win] {
            let a = if *s == i16::MIN {
                32768u16
            } else if *s < 0 {
                (-*s) as u16
            } else {
                *s as u16
            };
            if a > peak {
                peak = a;
            }
        }
        out[n] = peak;
        n += 1;
        i += win;
    }
    n
}

/// The index of the loudest window, and its value.
pub fn envelope_peak(env: &[u16]) -> (usize, u16) {
    let mut bi = 0;
    let mut bv = 0u16;
    for (i, v) in env.iter().enumerate() {
        if *v > bv {
            bv = *v;
            bi = i;
        }
    }
    (bi, bv)
}

/// The first window at or after `from` whose peak is at or below `floor`, i.e.
/// where the note has effectively stopped. Returns None if it never does, which
/// is the "starts and never decays" defect this exists to catch.
pub fn envelope_silence_at(env: &[u16], from: usize, floor: u16) -> Option<usize> {
    let mut i = from;
    while i < env.len() {
        if env[i] <= floor {
            return Some(i);
        }
        i += 1;
    }
    None
}

/// Is the envelope decaying over [from, to)? Tolerates `slack` windows that go
/// back up, because a decaying FM tone's peak-per-window is not perfectly
/// monotone at window boundaries. A tone that is NOT decaying fails this by a
/// wide margin, so the tolerance does not weaken the assertion.
pub fn envelope_is_decaying(env: &[u16], from: usize, to: usize, slack: usize) -> bool {
    if from + 2 > to || to > env.len() {
        return false;
    }
    let mut rises = 0usize;
    for i in (from + 1)..to {
        if env[i] > env[i - 1] {
            rises += 1;
        }
    }
    if rises > slack {
        return false;
    }
    // And it must actually have gone DOWN, not merely failed to go up: a flat
    // hold has zero rises and is exactly the defect being hunted.
    env[to - 1] < env[from] / 2
}

/// Absolute difference in millihertz, for a tolerance check that reads the same
/// way in both arms.
pub fn mhz_err(actual: u64, want: u64) -> u64 {
    if actual > want { actual - want } else { want - actual }
}

/// Error in CENTS x 100 (so 1 cent = 100), computed in integer.
///
/// cents = 1200 * log2(actual/want). Near unity, log2(1+x) ~ x/ln2, so
/// cents ~ 1200 * (actual - want) / (want * ln2) = 1731.234 * d / want.
/// Used with the x100 scaling: 173123 * d / want.
///
/// The approximation is stated rather than hidden: it is within 0.5% of the
/// true value for ratios inside +-6%, which covers every tolerance in this
/// ticket and is nowhere near the pass/fail boundaries.
pub fn cents_x100(actual: u64, want: u64) -> i64 {
    if want == 0 {
        return 0;
    }
    let d = actual as i64 - want as i64;
    (173123i64 * d) / want as i64
}
