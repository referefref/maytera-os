// rustkern/hdadma.rs - HD Audio output-DMA liveness verdict, as pure functions.
//
// WHY THIS EXISTS
// ---------------
// The one question #71 has never been able to answer on the owner's iMac14,4
// is "did the output stream's DMA engine actually run while a sound was
// supposed to be coming out?". Every capture so far answered a DIFFERENT
// question by accident. Build 1932's /AUDIOLOG.TXT reported
//
//     OUT stream 4: ... RUN=0 ... LPIB=0
//     LPIB poll worker: NOT running
//     Interrupt: ... MSI=NOT armed
//
// and all three were true BY CONSTRUCTION at the instant the log was written:
// drivers/audio.c emits that dump between audio_init() and the calls that start
// the poll worker, arm the MSI, and play the tone. A snapshot taken before the
// thing it is describing has been set up is not evidence, it is a tautology,
// and three separate wrong theories were built on that one.
//
// So the measurement is defined here, once, as: take TWO link-position reads
// separated by a known amount of REAL time and say what the delta means. A
// single read can never distinguish "stopped" from "we looked at the wrong
// moment"; a delta can.
//
// THE CLOCK MUST NOT BE THE THING UNDER TEST
// ------------------------------------------
// The elapsed time is passed IN, in microseconds, and the caller takes it from
// cpu/mono.h (`mono_us()`, TSC-backed). blame.md's standing rule is that
// `timer_ticks` is not a wall clock (KVM replays lost ticks in bursts), so a
// rate computed from ticks would report a healthy engine on a starved vCPU and
// a stalled one on a healthy machine. Same split, same reason, as
// rustkern/tickwatch.rs.
//
// WRAP IS NOT OPTIONAL TO HANDLE
// ------------------------------
// LPIB is a cyclic byte position within the Cyclic Buffer Length, so a healthy
// engine's second read is routinely SMALLER than its first. Subtracting them
// naively yields a huge unsigned number, which would read as "running" for the
// wrong reason, or as a negative rate. Worse, if the sampling window is at
// least as long as one lap of the buffer, a delta is genuinely AMBIGUOUS (n
// laps plus the delta look identical to the delta), and the only honest answer
// is "not measured" -- which is why UNKNOWN is a distinct value from RUNNING
// and not folded into it. tickwatch.rs records the same trap in its own words:
// "not measured" and "measured healthy" must never collapse to one value.
//
// Integer-only: the kernel target is soft-float with SSE disabled
// (x86_64-unknown-none, CFLAGS -mno-sse -mno-sse2), so the rate is carried as
// per-mille of expected and never as a float.

// ---------------------------------------------------------------------------
// Verdicts. Kept in sync with the HDA_DMA_* defines in drivers/hda.h.
// ---------------------------------------------------------------------------

/// Nothing can be said: the window was zero-length, or it was long enough that
/// a buffer wrap makes the delta ambiguous. NOT a synonym for a failure and
/// NOT a synonym for health.
pub const HDA_DMA_UNKNOWN: i32 = 0;
/// The engine walked the buffer at approximately the rate the stream format
/// says it should. This is the pass.
pub const HDA_DMA_RUNNING: i32 = 1;
/// The position advanced, but at well under the format's byte rate. The engine
/// is alive and something is throttling it; a different fault from STALLED and
/// audibly different (glitching, not silence).
pub const HDA_DMA_SLOW: i32 = 2;
/// RUN was set and the position did not move at all. The stream engine is the
/// fault and nothing downstream of it matters.
pub const HDA_DMA_STALLED: i32 = 3;
/// RUN was clear when we looked. The engine was never asked to run, so its not
/// running says nothing about whether it CAN. This is exactly the state build
/// 1932's log captured and reported as if it were a finding.
pub const HDA_DMA_NOT_STARTED: i32 = 4;

/// Fraction of the expected byte rate, in per-mille, below which the engine is
/// called SLOW rather than RUNNING. Generous on purpose: the two reads bracket
/// a scheduler sleep, so the measured window includes wake-up jitter, and the
/// question being asked is "does the engine move at all, roughly right", not
/// "is the clock accurate to a percent".
pub const HDA_DMA_RUNNING_PERMILLE: u64 = 700;

/// Bytes the link position advanced between two reads, correcting for the
/// cyclic wrap at `cbl`.
///
/// `cbl` of 0 means the caller could not read a buffer length; the wrapping
/// subtraction is then done on the raw 32-bit position, which is right for the
/// no-wrap case and is all that can be done for any other.
#[no_mangle]
pub extern "C" fn hda_dma_bytes_advanced_rs(lpib0: u32, lpib1: u32, cbl: u32) -> u32 {
    if cbl == 0 {
        return lpib1.wrapping_sub(lpib0);
    }
    let a = lpib0 % cbl;
    let b = lpib1 % cbl;
    if b >= a { b - a } else { (cbl - a) + b }
}

/// Bytes a stream of this format should deliver in `elapsed_us` microseconds.
///
/// Returns 0 if any term is 0, which the verdict treats as "no expectation
/// available" rather than as "expected nothing".
#[no_mangle]
pub extern "C" fn hda_dma_expected_bytes_rs(
    sample_rate: u32,
    channels: u32,
    bits: u32,
    elapsed_us: u64,
) -> u64 {
    let bps = hda_dma_bytes_per_sec_rs(sample_rate, channels, bits);
    if bps == 0 || elapsed_us == 0 {
        return 0;
    }
    // u64 throughout: 192000 B/s * 1e6 us overflows u32 immediately.
    bps.saturating_mul(elapsed_us) / 1_000_000
}

/// Byte rate of a stream format. Bits below 8 (or not a whole number of bytes)
/// yield 0 rather than a rounded-down lie.
#[no_mangle]
pub extern "C" fn hda_dma_bytes_per_sec_rs(sample_rate: u32, channels: u32, bits: u32) -> u64 {
    if sample_rate == 0 || channels == 0 || bits < 8 {
        return 0;
    }
    (sample_rate as u64) * (channels as u64) * ((bits / 8) as u64)
}

/// Measured advance as per-mille of expected. 0 when there is no expectation to
/// compare against, so a caller must not read 0 here as "stopped" -- read the
/// verdict for that.
#[no_mangle]
pub extern "C" fn hda_dma_rate_permille_rs(delta: u32, expected: u64) -> u32 {
    if expected == 0 {
        return 0;
    }
    let p = (delta as u64).saturating_mul(1000) / expected;
    if p > 0xFFFF_FFFF { 0xFFFF_FFFF } else { p as u32 }
}

/// The verdict.
///
/// `run` is the SDnCTL RUN bit as sampled with the FIRST position read, so a
/// caller cannot accidentally report a window during which the engine was
/// started or stopped underneath it.
#[no_mangle]
pub extern "C" fn hda_dma_verdict_rs(
    run: u32,
    lpib0: u32,
    lpib1: u32,
    cbl: u32,
    elapsed_us: u64,
    bytes_per_sec: u64,
) -> i32 {
    // Asked to run or not is the first question: a stopped engine that did not
    // move is not a fault, and calling it one is what sent this ticket sideways.
    if run == 0 {
        return HDA_DMA_NOT_STARTED;
    }
    if elapsed_us == 0 {
        return HDA_DMA_UNKNOWN;
    }
    // One lap of the ring is the point past which a delta stops being
    // interpretable. Refuse to guess.
    if cbl != 0 && bytes_per_sec != 0 {
        let lap_us = (cbl as u64).saturating_mul(1_000_000) / bytes_per_sec;
        if lap_us != 0 && elapsed_us >= lap_us {
            return HDA_DMA_UNKNOWN;
        }
    }
    let delta = hda_dma_bytes_advanced_rs(lpib0, lpib1, cbl);
    if delta == 0 {
        return HDA_DMA_STALLED;
    }
    let expected = if bytes_per_sec == 0 {
        0
    } else {
        bytes_per_sec.saturating_mul(elapsed_us) / 1_000_000
    };
    if expected == 0 {
        // It moved, and there is no rate to judge it against. Moving under RUN
        // is the pass criterion; the rate is a refinement, not the test.
        return HDA_DMA_RUNNING;
    }
    let permille = (delta as u64).saturating_mul(1000) / expected;
    if permille >= HDA_DMA_RUNNING_PERMILLE {
        HDA_DMA_RUNNING
    } else {
        HDA_DMA_SLOW
    }
}

/// Verdict as a NUL-terminated C string, so the C log line and any future
/// caller cannot drift out of sync with the numbers above.
#[no_mangle]
pub extern "C" fn hda_dma_verdict_name_rs(verdict: i32) -> *const u8 {
    match verdict {
        HDA_DMA_RUNNING => b"RUNNING\0".as_ptr(),
        HDA_DMA_SLOW => b"SLOW\0".as_ptr(),
        HDA_DMA_STALLED => b"STALLED\0".as_ptr(),
        HDA_DMA_NOT_STARTED => b"NOT-STARTED\0".as_ptr(),
        _ => b"NOT-MEASURED\0".as_ptr(),
    }
}

// ===========================================================================
// #173: DID A START ATTEMPT ACTUALLY START THE STREAM?
// ===========================================================================
//
// The DMA verdict above answers "is the engine moving". This answers the
// question one step earlier and, on the owner's iMac14,4, the one that was
// actually failing: "was RUN set at all, and if not, why not".
//
// The captured boot holds a direct contradiction. The init-time liveness check
// reported `output DMA check: LPIB 0 -> 8188 : RUNNING (DMA advances)` on
// stream 4, and the self-tone, later in the same boot, reported `DMA
// NOT-STARTED` on the SAME stream 4 with the SAME format 0x0011 and STS=0x00.
// A stream engine that ran once and then would not start again is not a broken
// engine. It is a start path that is not idempotent across a stop.
//
// It was not. hda_start() gated on a shadow bool, hda_state.playing, and three
// paths clear SDnCTL.RUN in hardware without clearing that shadow. Once they
// disagreed, hda_start() returned success without writing RUN, and the machine
// went silent with nothing logged. The distinction between "the shadow lied"
// and "the controller refused the write" cannot be recovered after the fact,
// which is why it is decided here, from the two register reads that bracket
// the write, and named in the log.
//
// Pure integer logic over three u32s: no MMIO, no state, no allocation. The
// register access that produces the inputs stays in C because it is inseparable
// from the existing hda_sd_read32/hda_sd_write32 window over hda_state.mmio and
// from the ordering contract between the write and the read-back; the decision
// it feeds does not need to be, and belongs with the other HDA verdicts.

/// SDnCTL.RUN, bit 1. Kept in sync with HDA_SD_CTL_RUN in drivers/hda.h.
const SD_CTL_RUN: u32 = 1 << 1;

/// RUN was already set before we touched it: somebody else is playing, and a
/// second start is a no-op. Not an error and not a fault.
pub const HDA_START_ALREADY_RUNNING: i32 = 0;
/// RUN was clear, we set it, the controller latched it. The normal pass.
pub const HDA_START_OK: i32 = 1;
/// RUN was clear while the shadow said "playing". THIS IS THE #173 SIGNATURE:
/// the old code would have returned early here and left the engine stopped.
/// The start still succeeded, so this is a caught fault, not a live one, but it
/// must be logged: it means something stopped the stream behind the driver's
/// back and the next such site may not be covered.
pub const HDA_START_SHADOW_STALE: i32 = 2;
/// RUN was clear, we wrote it, and it read back clear. The controller refused.
/// This is the only one of the four that indicts the hardware or the stream
/// descriptor rather than our bookkeeping, and it is the one the pre-#173 code
/// could never distinguish from the others.
pub const HDA_START_REFUSED: i32 = 3;
/// #205: RUN read back CLEAR, and yet LPIB advanced across the settle window.
/// The stream is running; the SDnCTL readback is what is wrong.
///
/// MEASURED on the owner's iMac14,4 (Lynx Point-LP 8086:9c20, Cirrus CS4208),
/// build 2007 /BOOTLOG.TXT. Eight times in one boot `hda_start()` wrote RUN,
/// read SDnCTL back one instruction later, saw RUN clear and reported
/// REFUSED-BY-CONTROLLER - while the very next log line, from a caller that
/// wrote the SAME bit at the SAME address and simply did not read it back,
/// measured "LPIB 0 -> 19468 ... 100.0% of rate : DMA RUNNING". Both cannot be
/// true of the same engine, and LPIB is the witness that cannot be faked: a
/// stopped DMA engine does not advance its own link position.
///
/// This verdict exists so that answer is a MEASUREMENT and not an exception.
/// The pre-#205 code had no way to say "the bit is lying" - REFUSED was its
/// only word for "RUN is clear" - so it did the one thing guaranteed to make it
/// worse: it drove a full SRST stream reset on an engine that had just started
/// correctly, then re-read too early again, then recorded playing=false. The
/// machine was silent and every log said the hardware refused.
pub const HDA_START_RUNNING_UNREPORTED: i32 = 4;

/// Classify one start attempt.
///
/// `shadow_playing` is the driver's belief BEFORE the attempt, `ctl_before` and
/// `ctl_after` are SDnCTL read immediately before and after the RUN write. When
/// no write was issued (RUN already set) the caller passes the same value twice.
#[no_mangle]
pub extern "C" fn hda_start_verdict_rs(shadow_playing: u32, ctl_before: u32, ctl_after: u32) -> i32 {
    if ctl_before & SD_CTL_RUN != 0 {
        // Already running. The shadow agreeing or not is irrelevant: the
        // hardware is in the state the caller asked for.
        return HDA_START_ALREADY_RUNNING;
    }
    if ctl_after & SD_CTL_RUN == 0 {
        // Asked for RUN, did not get RUN. Nothing about the shadow can excuse
        // this, so it is reported ahead of the stale-shadow case.
        return HDA_START_REFUSED;
    }
    if shadow_playing != 0 {
        HDA_START_SHADOW_STALE
    } else {
        HDA_START_OK
    }
}

/// #205: classify one start attempt using SDnCTL **and** LPIB.
///
/// Same inputs as `hda_start_verdict_rs`, plus the link position sampled
/// immediately before the RUN write and again after the settle window, and CBL
/// so a ring wrap is handled by the existing `hda_dma_bytes_advanced_rs`.
///
/// The order of the tests is the whole point. "RUN reads set" is believed
/// first because it is unambiguous. "LPIB advanced" is believed SECOND, and
/// only when the register has already failed to say so, because a moving link
/// position proves the engine is running no matter what the control register
/// claims. REFUSED is now what is left when NEITHER witness says yes, which is
/// what it always should have meant.
#[no_mangle]
pub extern "C" fn hda_start_verdict_lpib_rs(shadow_playing: u32,
                                            ctl_before: u32,
                                            ctl_after: u32,
                                            lpib0: u32,
                                            lpib1: u32,
                                            cbl: u32) -> i32 {
    if ctl_before & SD_CTL_RUN != 0 {
        return HDA_START_ALREADY_RUNNING;
    }
    if ctl_after & SD_CTL_RUN != 0 {
        return if shadow_playing != 0 { HDA_START_SHADOW_STALE } else { HDA_START_OK };
    }
    if hda_dma_bytes_advanced_rs(lpib0, lpib1, cbl) > 0 {
        return HDA_START_RUNNING_UNREPORTED;
    }
    HDA_START_REFUSED
}

/// Verdict as a NUL-terminated C string.
#[no_mangle]
pub extern "C" fn hda_start_verdict_name_rs(verdict: i32) -> *const u8 {
    match verdict {
        HDA_START_ALREADY_RUNNING => b"ALREADY-RUNNING\0".as_ptr(),
        HDA_START_OK => b"STARTED\0".as_ptr(),
        HDA_START_SHADOW_STALE => b"STARTED-AFTER-STALE-SHADOW\0".as_ptr(),
        HDA_START_REFUSED => b"REFUSED-BY-CONTROLLER\0".as_ptr(),
        HDA_START_RUNNING_UNREPORTED =>
            b"RUNNING-BUT-RUN-BIT-READS-CLEAR\0".as_ptr(),
        _ => b"UNKNOWN\0".as_ptr(),
    }
}
