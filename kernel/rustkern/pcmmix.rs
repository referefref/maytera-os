// pcmmix.rs - #205/#242: the fixed-point mix kernel behind the shared PCM sink.
//
// WHY THIS EXISTS
// ---------------
// drivers/audio_pcm.c used to serve exactly ONE stream (PCM_MAX_STREAMS = 1).
// On a machine with one DAC that reads like an honest statement about the
// hardware, and it is not: a DAC has one stream, but an operating system has
// many things that want to make a sound, and the job of turning N producers
// into one hardware stream is called MIXING. Refusing the second opener does
// not avoid the work, it moves the failure to the user: on the owner's ASUS
// i7-4720HQ, /APPS/FMSYNTH held the single stream from the first DOS game of
// the session and /APPS/MIDIPLAY was refused with EBUSY, so the MIDI player was
// silent with no diagnosis anywhere. See CHANGELOG #205.
//
// So: N producers, one sink, summed here.
//
// WHAT THIS MODULE IS AND IS NOT
// ------------------------------
// It is the arithmetic ONLY: resample one source into a stereo accumulator, and
// clamp an accumulator down to S16. It owns no rings, no locks, no threads and
// no wait queues; audio_pcm.c owns all of those and calls in with plain
// pointers. That split is deliberate: the numeric part is where a silent
// wrong-answer bug lives (a wrap, a sign, a saturation that folds instead of
// clamping), and it is the part that can be exhaustively self-tested with no
// hardware, which pcm_mix_selftest_rs() does on every boot.
//
// INTEGER ONLY, and not as a style preference. The kernel target is
// x86_64-unknown-none (+soft-float) and CFLAGS carry -mno-sse -mno-sse2, so a
// float multiply in here would become a libgcc soft-float call in the hottest
// loop in the audio path. Everything below is i32/i64/u64.
//
// FIXED-POINT CONVENTIONS
// -----------------------
//   step_q32   source frames advanced per output frame, Q32
//              (src_rate << 32) / dev_rate. Equal rates give exactly 1 << 32.
//   phase_q32  fractional position in [0, 1) between `prev` and the next
//              unconsumed source frame. Carried across calls so a block
//              boundary is not a resampling discontinuity.
//   prev       the last source frame consumed, held so that interpolation
//              never needs to look one frame AHEAD. Looking ahead is what would
//              force the mixer to stall at the end of a ring, and a mixer that
//              stalls because one producer ran dry is the single-holder bug
//              again wearing a different hat.
//
// EQUAL RATES ARE A SEPARATE PATH, ON PURPOSE. When step_q32 is exactly 1 << 32
// and the phase is 0 the interpolator is mathematically the identity, but only
// after a multiply and a shift per sample and with a one-frame group delay.
// The overwhelmingly common case (every producer at the sink's own rate) takes
// the straight accumulate instead: bit-exact, no delay, and no arithmetic to
// get wrong. selftest bit 0 asserts that identity rather than assuming it.

/// Q32 unity.
const ONE_Q32: u64 = 1u64 << 32;

/// Pack two u32 results into the u64 an `extern "C"` function can return
/// without inventing a struct ABI: low word = source frames consumed, high
/// word = output frames actually produced (less than requested means the
/// source ran dry, i.e. that producer underran this block).
#[inline]
fn pack(src_used: u32, out_done: u32) -> u64 {
    (src_used as u64) | ((out_done as u64) << 32)
}

#[inline]
fn clamp_i16(v: i32) -> i16 {
    if v > 32767 {
        32767
    } else if v < -32768 {
        -32768
    } else {
        v as i16
    }
}

/// Mix one source into a stereo i32 accumulator.
///
/// `acc`        out_frames * 2 interleaved i32 accumulator, ADDED to (never
///              overwritten) so N calls sum N producers.
/// `src`        src_frames * src_ch interleaved S16, LINEAR (the caller has
///              already de-wrapped its ring).
/// `src_ch`     1 or 2. A mono source is duplicated to both output channels,
///              which is what uac_file_fill() has always done.
/// `step_q32`   see the header.
/// `phase_q32`  in/out, carried across blocks.
/// `prev`       in/out, 2 x i32, the last consumed source frame.
///
/// Returns pack(source frames consumed, output frames produced).
///
/// SAFETY: the caller guarantees `acc` holds out_frames*2 i32, `src` holds
/// src_frames*src_ch i16, `phase_q32` is one u64 and `prev` is two i32.
#[no_mangle]
pub unsafe extern "C" fn pcm_mix_add_rs(
    acc: *mut i32,
    out_frames: u32,
    src: *const i16,
    src_frames: u32,
    src_ch: u32,
    step_q32: u64,
    phase_q32: *mut u64,
    prev: *mut i32,
) -> u64 {
    if acc.is_null() || phase_q32.is_null() || prev.is_null() {
        return pack(0, 0);
    }
    if out_frames == 0 {
        return pack(0, 0);
    }
    if src.is_null() || src_frames == 0 {
        return pack(0, 0);
    }
    let ch = if src_ch >= 2 { 2usize } else { 1usize };

    // ---- fast path: the producer is already at the sink's rate --------------
    if step_q32 == ONE_Q32 && *phase_q32 == 0 {
        let n = if src_frames < out_frames { src_frames } else { out_frames };
        for i in 0..(n as usize) {
            let l = *src.add(i * ch) as i32;
            let r = if ch == 2 { *src.add(i * ch + 1) as i32 } else { l };
            *acc.add(i * 2) += l;
            *acc.add(i * 2 + 1) += r;
            *prev = l;
            *prev.add(1) = r;
        }
        return pack(n, n);
    }

    // ---- resampling path ----------------------------------------------------
    let mut phase = *phase_q32;
    let mut p0 = *prev;
    let mut p1 = *prev.add(1);
    let mut used: u32 = 0;
    let mut done: u32 = 0;

    while done < out_frames {
        if used >= src_frames {
            break; // dry: the caller reports the shortfall as this stream's underrun
        }
        let i = used as usize;
        let c0 = *src.add(i * ch) as i32;
        let c1 = if ch == 2 { *src.add(i * ch + 1) as i32 } else { c0 };

        // Linear interpolation between the last CONSUMED frame and the next
        // one. frac is Q32, so the product fits comfortably in i64 for any
        // S16 delta (delta <= 65535, frac < 2^32).
        let frac = (phase & 0xFFFF_FFFF) as i64;
        let o0 = p0 + (((c0 - p0) as i64 * frac) >> 32) as i32;
        let o1 = p1 + (((c1 - p1) as i64 * frac) >> 32) as i32;

        let d = done as usize;
        *acc.add(d * 2) += o0;
        *acc.add(d * 2 + 1) += o1;
        done += 1;

        phase += step_q32;
        while phase >= ONE_Q32 {
            if used >= src_frames {
                // Ran out mid-advance. Leave `phase` where it is: the next call
                // resumes at exactly this fractional position rather than
                // silently snapping to a frame boundary, which is what would
                // make a block boundary audible.
                break;
            }
            let j = used as usize;
            p0 = *src.add(j * ch) as i32;
            p1 = if ch == 2 { *src.add(j * ch + 1) as i32 } else { p0 };
            used += 1;
            phase -= ONE_Q32;
        }
    }

    *phase_q32 = phase;
    *prev = p0;
    *prev.add(1) = p1;
    pack(used, done)
}

/// Clamp a stereo i32 accumulator down to interleaved S16.
///
/// SATURATING, NEVER WRAPPING. Two producers at full scale sum to +/-65534,
/// and a wrap would turn a loud passage into white noise, which is the classic
/// "the mixer works until two things play at once" defect.
///
/// SAFETY: `acc` holds `samples` i32 and `out` holds `samples` i16.
#[no_mangle]
pub unsafe extern "C" fn pcm_mix_finish_rs(acc: *const i32, out: *mut i16, samples: u32) {
    if acc.is_null() || out.is_null() {
        return;
    }
    for i in 0..(samples as usize) {
        *out.add(i) = clamp_i16(*acc.add(i));
    }
}

/// (src_rate << 32) / dev_rate, computed here so no caller has to get the
/// 64-bit shift right, and so a zero dev_rate can never divide by zero.
#[no_mangle]
pub extern "C" fn pcm_mix_step_rs(src_rate: u32, dev_rate: u32) -> u64 {
    if dev_rate == 0 || src_rate == 0 {
        return ONE_Q32;
    }
    ((src_rate as u64) << 32) / (dev_rate as u64)
}

// ===========================================================================
// SELF-TEST. Returns a bitmask; 0 means PASS. Called once at audio_pcm init and
// reported to /AUDIOLOG.TXT, because a mixer that is subtly wrong sounds like a
// bad recording rather than like a bug, and nobody files it.
// ===========================================================================
#[no_mangle]
pub extern "C" fn pcm_mix_selftest_rs() -> u32 {
    let mut mask: u32 = 0;

    // bit 0: equal rates are BIT-EXACT and consume 1:1.
    {
        let src: [i16; 8] = [100, -100, 200, -200, 300, -300, 400, -400];
        let mut acc = [0i32; 8];
        let mut phase = 0u64;
        let mut prev = [0i32; 2];
        let rc = unsafe {
            pcm_mix_add_rs(
                acc.as_mut_ptr(), 4,
                src.as_ptr(), 4, 2,
                ONE_Q32, &mut phase, prev.as_mut_ptr(),
            )
        };
        let used = (rc & 0xFFFF_FFFF) as u32;
        let done = (rc >> 32) as u32;
        if used != 4 || done != 4 {
            mask |= 1 << 0;
        }
        for i in 0..8 {
            if acc[i] != src[i] as i32 {
                mask |= 1 << 0;
            }
        }
    }

    // bit 1: two producers SUM. Not "the second one wins", which is what an
    // overwrite would look like and would still play a sound.
    {
        let a: [i16; 4] = [1000, 1000, 1000, 1000];
        let b: [i16; 4] = [-250, 500, -250, 500];
        let mut acc = [0i32; 4];
        let mut ph1 = 0u64;
        let mut pv1 = [0i32; 2];
        let mut ph2 = 0u64;
        let mut pv2 = [0i32; 2];
        unsafe {
            pcm_mix_add_rs(acc.as_mut_ptr(), 2, a.as_ptr(), 2, 2, ONE_Q32, &mut ph1, pv1.as_mut_ptr());
            pcm_mix_add_rs(acc.as_mut_ptr(), 2, b.as_ptr(), 2, 2, ONE_Q32, &mut ph2, pv2.as_mut_ptr());
        }
        if acc[0] != 750 || acc[1] != 1500 || acc[2] != 750 || acc[3] != 1500 {
            mask |= 1 << 1;
        }
    }

    // bit 2: saturation CLAMPS, it does not wrap.
    {
        let acc: [i32; 4] = [40000, -40000, 100, -100];
        let mut out = [0i16; 4];
        unsafe { pcm_mix_finish_rs(acc.as_ptr(), out.as_mut_ptr(), 4) };
        if out[0] != 32767 || out[1] != -32768 || out[2] != 100 || out[3] != -100 {
            mask |= 1 << 2;
        }
    }

    // bit 3: a mono source reaches BOTH output channels.
    {
        let src: [i16; 3] = [700, 800, 900];
        let mut acc = [0i32; 6];
        let mut phase = 0u64;
        let mut prev = [0i32; 2];
        unsafe {
            pcm_mix_add_rs(acc.as_mut_ptr(), 3, src.as_ptr(), 3, 1, ONE_Q32, &mut phase, prev.as_mut_ptr())
        };
        if acc[0] != 700 || acc[1] != 700 || acc[4] != 900 || acc[5] != 900 {
            mask |= 1 << 3;
        }
    }

    // bit 4: downsampling 2:1 consumes twice the source it produces, and
    // upsampling 1:2 consumes half. This is the arithmetic that decides whether
    // a 44100 Hz producer on a 48000 Hz sink drifts, and drift is inaudible for
    // ten seconds and then unmistakable.
    {
        let src = [0i16; 64];
        let mut acc = [0i32; 32];
        let mut phase = 0u64;
        let mut prev = [0i32; 2];
        let rc = unsafe {
            pcm_mix_add_rs(acc.as_mut_ptr(), 16, src.as_ptr(), 32, 2,
                           2 * ONE_Q32, &mut phase, prev.as_mut_ptr())
        };
        let used = (rc & 0xFFFF_FFFF) as u32;
        let done = (rc >> 32) as u32;
        // 16 output frames at 2 source frames each: 32 consumed (the last
        // advance stops at the end of the buffer, so accept 31 or 32).
        if done != 16 || used < 31 || used > 32 {
            mask |= 1 << 4;
        }
    }
    {
        let src = [0i16; 64];
        let mut acc = [0i32; 64];
        let mut phase = 0u64;
        let mut prev = [0i32; 2];
        let rc = unsafe {
            pcm_mix_add_rs(acc.as_mut_ptr(), 32, src.as_ptr(), 32, 2,
                           ONE_Q32 / 2, &mut phase, prev.as_mut_ptr())
        };
        let used = (rc & 0xFFFF_FFFF) as u32;
        let done = (rc >> 32) as u32;
        if done != 32 || used < 15 || used > 17 {
            mask |= 1 << 4;
        }
    }

    // bit 5: a DRY source produces a SHORT block rather than looping or
    // stalling. The mixer relies on this to keep going when one producer is
    // late; if this ever returned `out_frames` the late producer would silently
    // repeat its tail, which is exactly the #189 ring-repeat defect one layer up.
    {
        let src: [i16; 4] = [1, 2, 3, 4];
        let mut acc = [0i32; 16];
        let mut phase = 0u64;
        let mut prev = [0i32; 2];
        let rc = unsafe {
            pcm_mix_add_rs(acc.as_mut_ptr(), 8, src.as_ptr(), 2, 2,
                           ONE_Q32, &mut phase, prev.as_mut_ptr())
        };
        let done = (rc >> 32) as u32;
        if done != 2 {
            mask |= 1 << 5;
        }
        if acc[4] != 0 || acc[15] != 0 {
            mask |= 1 << 5;
        }
    }

    // bit 6: step computation. 44100 into 48000 must be < 1, 48000 into 44100
    // must be > 1, equal rates EXACTLY unity (the fast path depends on it).
    {
        if pcm_mix_step_rs(48000, 48000) != ONE_Q32 {
            mask |= 1 << 6;
        }
        if pcm_mix_step_rs(44100, 48000) >= ONE_Q32 {
            mask |= 1 << 6;
        }
        if pcm_mix_step_rs(48000, 44100) <= ONE_Q32 {
            mask |= 1 << 6;
        }
        if pcm_mix_step_rs(44100, 0) != ONE_Q32 {
            mask |= 1 << 6;
        }
    }

    // bit 7: phase is CARRIED across calls. Two 8-frame calls at a fractional
    // rate must consume the same total as one 16-frame call, or every block
    // boundary loses or repeats a fraction of a frame.
    {
        let src = [0i16; 128];
        let step = pcm_mix_step_rs(44100, 48000);
        let mut acc = [0i32; 32];

        let mut ph_a = 0u64;
        let mut pv_a = [0i32; 2];
        let r1 = unsafe {
            pcm_mix_add_rs(acc.as_mut_ptr(), 8, src.as_ptr(), 64, 2, step, &mut ph_a, pv_a.as_mut_ptr())
        };
        let r2 = unsafe {
            pcm_mix_add_rs(acc.as_mut_ptr(), 8, src.as_ptr(), 64, 2, step, &mut ph_a, pv_a.as_mut_ptr())
        };
        let split = ((r1 & 0xFFFF_FFFF) + (r2 & 0xFFFF_FFFF)) as u32;

        let mut ph_b = 0u64;
        let mut pv_b = [0i32; 2];
        let r3 = unsafe {
            pcm_mix_add_rs(acc.as_mut_ptr(), 16, src.as_ptr(), 64, 2, step, &mut ph_b, pv_b.as_mut_ptr())
        };
        let whole = (r3 & 0xFFFF_FFFF) as u32;
        if split != whole {
            mask |= 1 << 7;
        }
    }

    mask
}
