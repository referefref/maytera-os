// hdastarve.rs - #189: what the HD Audio output ring must contain when the
// producer stops feeding it.
//
// THE BUG THIS EXISTS FOR
// -----------------------
// An HDA output stream is a CYCLIC DMA engine. Once SDnCTL.RUN is set the
// controller walks the Buffer Descriptor List forever, and when it reaches the
// Last Valid Index it goes straight back to entry 0. It has no concept of "the
// data ran out". So when a producer stops writing but leaves the stream
// running, the engine keeps handing the codec whatever bytes were last left in
// the ring: the tail of the last sound, on a loop, indefinitely.
//
// Measured on 2026-08-20 (#183's host-side WAV capture): 328 non-silent
// segments, the final 0.743 s of a tune repeating for 60+ seconds after
// playback ended. 0.743 s is exactly one lap of the 131072-byte ring at
// 44100 Hz S16 stereo, which is what identifies the ring rather than the
// synthesiser as the source of the repeat.
//
// WHAT THIS MODULE DECIDES
// ------------------------
// The policy is SILENCE-ON-STARVE: a slot the DMA has finished playing is
// zeroed, so it plays as silence the next time round rather than repeating.
// The engine is left RUNNING, which costs nothing audible and avoids a
// start/stop click and a restart latency on every new sound.
//
// That policy needs an UNAMBIGUOUS answer to "which slots has the DMA
// finished?", and that is the whole reason this file exists in Rust rather
// than as a few lines of modular arithmetic in the driver. LPIB is a position
// WITHIN the ring, so slot indices wrap, and "the head is 5 slots ahead of the
// producer" and "the head has lapped the producer and is 27 slots behind" are
// THE SAME MODULAR VALUE. Getting that wrong does not produce a wrong number,
// it produces the original bug back again: the silencer concludes there is
// plenty of queued data and stops silencing, and the tail loops forever.
//
// So every counter here is FREE-RUNNING in units of BDL slots. Modular values
// (what LPIB gives us) are converted in exactly one place,
// `hda_starve_advance_rs`, which is the only function allowed to guess at a
// wrap, and it can only do so correctly because the caller samples faster than
// one lap (the driver's service path runs at >= 100 Hz from the 10 ms poll
// worker, against a lap of ~0.7 s).
//
// THREE WINDOWS OVER ONE RING, AND WHY THEY MUST NOT ALIAS
// --------------------------------------------------------
//   sil_slot   the next slot the silencer will zero.  Owns [sil_slot, dma_slot)
//   dma_slot   the slot the DMA engine is reading right now.
//   wr_slot    the next slot the producer will fill.   Owns [dma_slot, wr_slot)
//
// Both windows are expressed in free-running slots but land in only `nbdl`
// physical slots, so if they ever TOGETHER span a whole lap they alias, and
// the silencer would zero bytes the producer had just written. That is a data
// race that no lock can fix, because the two are legitimately disjoint in
// free-running space and only collide after the modulo. `hda_starve_avail_slots_rs`
// is what prevents it: the producer is only ever handed slots that keep the
// combined span strictly below one lap.
//
// Integer-only, as the whole kernel must be (x86_64-unknown-none, soft-float,
// CFLAGS -mno-sse -mno-sse2). Nothing here needs a fraction anyway.

/// Ceiling on how many slots the silencer zeroes in a single service pass.
/// The service path is reached from an interrupt handler as well as from the
/// poll worker, so the work per call is bounded rather than "catch up
/// completely": 4 slots is 16 KB of memset, and at the driver's 100 Hz service
/// rate it is ~400 slots/s against a consumption of ~43 slots/s at 44.1 kHz
/// stereo, i.e. an order of magnitude of headroom.
pub const HDA_STARVE_ZERO_PER_PASS: u32 = 4;

/// The largest silencer backlog (`dma_slot - sil_slot`) the rest of this module
/// is allowed to see, in slots below one lap.
///
/// This is not a tuning knob, it is the contract that makes the anti-aliasing
/// bound solvable: `hda_starve_avail_slots_rs` can only offer a starved
/// producer a slot while `lag <= nbdl - 3`, and `hda_starve_sil_floor_rs` is
/// what enforces it. Call sil_floor before avail_slots on every path, or
/// avail_slots will correctly but uselessly refuse to hand out any slots.
///
/// `nbdl - 3` rather than `nbdl - 2` because the producer's window already
/// gives up one slot of margin ahead of the DMA head, for the controller's own
/// stream FIFO prefetch: writing the slot the engine is about to read is a race
/// with hardware that no software bound can win. That margin is also exactly
/// the depth the driver allowed before this policy existed, so a healthy stream
/// loses no buffer depth to #189.
#[inline]
fn max_lag(nbdl: u32) -> u64 {
    (nbdl as u64) - 3
}

/// Convert a modular LPIB slot index into the free-running slot counter.
///
/// `last_slot` is the modular index this counter was last advanced to, and is
/// how a lap is detected: the position going BACKWARDS means the ring wrapped.
/// This is the only wrap guess in the module, and it is sound only because the
/// caller samples much faster than one lap. If a caller were ever to sample
/// slower than a lap it would silently lose whole laps, so do not move this
/// call out of the >= 100 Hz service path.
#[no_mangle]
pub extern "C" fn hda_starve_advance_rs(dma_slot: u64, last_slot: u32, cur_slot: u32, nbdl: u32) -> u64 {
    if nbdl == 0 {
        return dma_slot;
    }
    let n = nbdl as u64;
    let cur = (cur_slot % nbdl) as u64;
    let last = (last_slot % nbdl) as u64;
    let lap_base = dma_slot - (dma_slot % n);
    let mut next = lap_base + cur;
    if cur < last {
        next += n;
    }
    // Monotonic by construction. A caller that reset the modular position
    // without resetting the counter would otherwise be able to walk it
    // backwards, and a backwards counter turns every window below into a
    // nonsense span.
    if next < dma_slot { dma_slot } else { next }
}

/// How many slots the silencer may zero right now: everything the DMA has
/// finished, capped at `max_per_pass`.
///
/// Never includes `dma_slot` itself. The engine may already have prefetched
/// part of the slot it is reading, so zeroing it would be a race against the
/// controller's own FIFO for no benefit: those bytes are being played now and
/// will be zeroed on the next pass, one slot (~23 ms at 44.1 kHz) later.
#[no_mangle]
pub extern "C" fn hda_starve_zero_count_rs(sil_slot: u64, dma_slot: u64, max_per_pass: u32) -> u32 {
    if dma_slot <= sil_slot {
        return 0;
    }
    let behind = dma_slot - sil_slot;
    if behind > max_per_pass as u64 {
        max_per_pass
    } else {
        behind as u32
    }
}

/// Drop an unrecoverable backlog.
///
/// If the silencer fell more than `max_lag` behind (the service path starved
/// for close to a whole lap), those slots have ALREADY been replayed. Zeroing
/// them now cannot un-play them, and grinding through the backlog would keep
/// the silencer permanently behind the head. Skip forward and start silencing
/// what has not yet been repeated.
///
/// This also ENFORCES the `lag <= nbdl - 2` contract that
/// `hda_starve_avail_slots_rs` depends on, which is why it must be called
/// before it on every path and not only from the silencer.
///
/// A skip is a real degradation and the driver counts it separately, because
/// "we silenced everything" and "we gave up on 30 slots" must not report the
/// same way.
#[no_mangle]
pub extern "C" fn hda_starve_sil_floor_rs(sil_slot: u64, dma_slot: u64, nbdl: u32) -> u64 {
    if nbdl < 2 {
        return sil_slot;
    }
    let m = max_lag(nbdl);
    if dma_slot > sil_slot + m {
        dma_slot - m
    } else {
        sil_slot
    }
}

/// Move the producer's frontier back in front of the DMA head.
///
/// `wr_slot <= dma_slot` means the producer's queue is exhausted: the engine
/// has caught up with, or run past, the last thing written. Writing at the old
/// frontier from there would put fresh audio BEHIND the play head, where it
/// would not be heard until a full lap later. Resync to the slot after the
/// head, which is also the lowest-latency place the new sound can start.
#[no_mangle]
pub extern "C" fn hda_starve_resync_rs(wr_slot: u64, dma_slot: u64) -> u64 {
    if wr_slot <= dma_slot {
        dma_slot + 1
    } else {
        wr_slot
    }
}

/// Slots the producer may fill right now, given where the silencer has got to.
///
/// Call `hda_starve_resync_rs` first: this assumes `wr_slot > dma_slot`.
///
/// The bound is the anti-aliasing rule from the header comment. The silencer's
/// window is `lag = dma_slot - sil_slot` slots long and the producer's is
/// `inflight = wr_slot - dma_slot`; together they must stay strictly under one
/// lap of `nbdl` physical slots, so `inflight <= nbdl - 1 - lag`.
///
/// In the healthy case the silencer is fully caught up (`lag == 0`) and this is
/// `nbdl - 1 - inflight`, i.e. the entire ring bar one slot, which is what the
/// driver allowed before this policy existed. The producer therefore loses no
/// buffer depth on any normal path; it only yields slots while the silencer is
/// genuinely behind.
#[no_mangle]
pub extern "C" fn hda_starve_avail_slots_rs(wr_slot: u64, dma_slot: u64, sil_slot: u64, nbdl: u32) -> u32 {
    if nbdl < 4 {
        return 0;
    }
    let lap = nbdl as u64;
    let lag = dma_slot.saturating_sub(sil_slot);
    if lag > max_lag(nbdl) {
        // Unreachable while callers honour the contract (call sil_floor first).
        // Defended anyway rather than clamped: at this lag the silencer's window
        // covers so much of the ring that there is no slot the producer could
        // be given without the two aliasing, and quietly clamping would hand
        // out a slot that IS about to be zeroed - i.e. it would silently
        // reintroduce exactly the corruption this bound exists to prevent.
        return 0;
    }
    let max_inflight = lap - 1 - lag; // >= 1
    let inflight = wr_slot.saturating_sub(dma_slot);
    if inflight >= max_inflight {
        0
    } else {
        (max_inflight - inflight) as u32
    }
}

/// Self-test of the arithmetic above. Returns a bitmask: 0 is a pass, every set
/// bit is a named property that failed. Called once at HDA init and reported in
/// the persistent boot log, because the machine this matters on (the owner's
/// iMac14,4) has no serial console and a silent instrument there is no
/// instrument at all.
///
/// These are the properties that, if broken, reproduce #189 rather than
/// producing an obviously wrong number.
#[no_mangle]
pub extern "C" fn hda_starve_selftest_rs() -> u32 {
    let n: u32 = 32;
    let mut bad: u32 = 0;

    // bit0: a plain forward step within one lap.
    if hda_starve_advance_rs(5, 5, 9, n) != 9 {
        bad |= 1 << 0;
    }
    // bit1: THE WRAP. Head goes 31 -> 0; the free-running counter must go 31 ->
    // 32, not back to 0. This is the exact confusion that lets a starved ring
    // look like a full one.
    if hda_starve_advance_rs(31, 31, 0, n) != 32 {
        bad |= 1 << 1;
    }
    // bit2: many laps in, the wrap still lands on the next lap and not lap 1.
    if hda_starve_advance_rs(31 + 320, 31, 0, n) != 32 + 320 {
        bad |= 1 << 2;
    }
    // bit3: never runs backwards.
    if hda_starve_advance_rs(40, 8, 8, n) != 40 {
        bad |= 1 << 3;
    }

    // bit4: nothing to zero when the silencer is level with the head, and in
    // particular the head's OWN slot is never zeroed.
    if hda_starve_zero_count_rs(10, 10, HDA_STARVE_ZERO_PER_PASS) != 0 {
        bad |= 1 << 4;
    }
    // bit5: bounded per pass.
    if hda_starve_zero_count_rs(0, 1000, HDA_STARVE_ZERO_PER_PASS) != HDA_STARVE_ZERO_PER_PASS {
        bad |= 1 << 5;
    }
    // bit6: a backlog at the limit is kept, not discarded.
    if hda_starve_sil_floor_rs(100, 100 + 29, n) != 100 {
        bad |= 1 << 6;
    }
    // bit7: a backlog past the limit is dropped back to exactly the limit.
    if hda_starve_sil_floor_rs(100, 100 + 40, n) != 100 + 40 - 29 {
        bad |= 1 << 7;
    }

    // bit8: an exhausted producer is resynced to just past the head.
    if hda_starve_resync_rs(50, 50) != 51 || hda_starve_resync_rs(20, 50) != 51 {
        bad |= 1 << 8;
    }
    // bit9: a producer that is ahead is left alone.
    if hda_starve_resync_rs(60, 50) != 60 {
        bad |= 1 << 9;
    }

    // bit10: healthy case gives back the whole ring bar the head slot, so this
    // policy costs no buffer depth during normal playback.
    if hda_starve_avail_slots_rs(51, 50, 50, n) != 30 {
        bad |= 1 << 10;
    }
    // bit11: a full producer queue offers nothing.
    if hda_starve_avail_slots_rs(50 + 31, 50, 50, n) != 0 {
        bad |= 1 << 11;
    }
    // bit12: THE ANTI-ALIASING PROPERTY, exhaustively over the whole contract
    // domain. For every legal silencer lag and every producer position, the
    // silencer's window [sil, dma) and the producer's window [dma, wr+avail)
    // must TOGETHER stay strictly inside one lap, or after the modulo they
    // address the same physical slot and the silencer zeroes fresh audio.
    // This is the property that makes the whole policy safe without a lock
    // between the producer and the interrupt handler.
    let dma = 1_000_000u64;
    for lag in 0..=(n as u64 - 3) {
        for inflight in 1..=(n as u64 + 2) {
            let sil = dma - lag;
            let wr = dma + inflight;
            let avail = hda_starve_avail_slots_rs(wr, dma, sil, n) as u64;
            if avail > 0 && (wr + avail) - sil > n as u64 {
                bad |= 1 << 12;
            }
        }
    }
    // bit13: past the contract, it REFUSES rather than clamping. A silently
    // clamped answer here would be a slot that is about to be zeroed.
    if hda_starve_avail_slots_rs(dma + 1, dma, dma - (n as u64 - 2), n) != 0 {
        bad |= 1 << 13;
    }
    // bit14: at the maximum LEGAL lag a producer with an empty queue can still
    // write, or playback could never resume after a long service stall.
    if hda_starve_avail_slots_rs(dma + 1, dma, dma - (n as u64 - 3), n) == 0 {
        bad |= 1 << 14;
    }
    // bit15: sil_floor and avail_slots agree. Feeding avail_slots an arbitrary
    // backlog THROUGH sil_floor must always yield a usable answer, which is the
    // exact call order the driver uses.
    for lag in 0..=(2 * n as u64) {
        let sil = hda_starve_sil_floor_rs(dma - lag, dma, n);
        if hda_starve_avail_slots_rs(dma + 1, dma, sil, n) == 0 {
            bad |= 1 << 15;
        }
    }

    bad
}

// ---------------------------------------------------------------------------
// AUDLEAD: MAKE STARVATION VISIBLE.
//
// `hda_starve_resync_rs` above is the ONE place in this driver that knows the
// producer ran dry: `wr_slot <= dma_slot` means the engine has caught up with,
// or run past, the last slot anyone wrote. It repairs the frontier and returns,
// and until now that was ALL it did. The repair is correct and must stay, but a
// repair that leaves no trace is why this fault has only ever been reportable
// as "it sounds stuttery": the driver knew, every single time, and said nothing.
//
// The counter that DOES exist, `hda_state.underruns`, counts SDnSTS.FIFOE, and
// that is a different event. FIFOE fires when the controller cannot fetch from
// memory fast enough. A late PRODUCER never trips it, because the ring always
// holds valid bytes, just not the right ones. So the one statistic named
// "underruns" is structurally incapable of seeing the underrun that actually
// happens here.
//
// These two are pure functions over the same pair of counters, so a caller can
// record the event WITHOUT touching the repair path. Deliberately split from
// the repair rather than folded into it: a function that both moves the
// frontier and reports a statistic has two reasons to change, and the repair
// is already pinned by the self-test below.
// ---------------------------------------------------------------------------

/// Slots of audio the producer FAILED to supply before the engine reached them.
///
/// Zero on every healthy call. Non-zero means the DMA has already played
/// `deficit` slots for which nothing was ever written, so the listener heard
/// `deficit` slots of whatever the silencer had left there. This is the
/// underrun magnitude, in slots; the driver converts it to milliseconds using
/// the slot size it programmed.
///
/// `+ 1` because `wr_slot == dma_slot` is ALREADY a starve: the engine is
/// reading the slot the producer was about to fill, so the next thing played is
/// not the next thing written. Treating that case as healthy would miss the
/// first slot of every underrun, which is also the only slot of the SHORTEST
/// underruns, i.e. exactly the ones a tuning change is trying to move.
#[no_mangle]
pub extern "C" fn hda_starve_deficit_rs(wr_slot: u64, dma_slot: u64) -> u64 {
    if wr_slot <= dma_slot {
        (dma_slot + 1) - wr_slot
    } else {
        0
    }
}

/// The producer's lead: slots written but not yet played.
///
/// This is the buffer depth the listener is actually protected by, measured
/// against the HARDWARE position rather than against any software clock. A
/// pacing loop that trusts a tick counter can believe it holds a lead it does
/// not have, because the tick counter stops being a wall clock precisely when
/// the machine is too busy, which is precisely when the lead matters. This
/// cannot, because `dma_slot` is derived from LPIB, which is advanced by the
/// same engine that is consuming the audio.
///
/// Saturating: a starved producer reports 0 rather than wrapping to ~2^64.
#[no_mangle]
pub extern "C" fn hda_starve_lead_rs(wr_slot: u64, dma_slot: u64) -> u64 {
    wr_slot.saturating_sub(dma_slot)
}

/// Self-test for the two accessors above. Returns a bitmask of FAILURES, so
/// zero is a pass, matching `hda_starve_selftest_rs`.
#[no_mangle]
pub extern "C" fn hda_starve_stat_selftest_rs() -> u32 {
    let mut bad: u32 = 0;
    let dma: u64 = 1_000_000;

    // A producer sitting one slot ahead is NOT starved, and its lead is
    // exactly one slot.
    if hda_starve_deficit_rs(dma + 1, dma) != 0 { bad |= 1 << 0; }
    if hda_starve_lead_rs(dma + 1, dma) != 1 { bad |= 1 << 1; }

    // Exactly caught up: one slot of deficit, zero lead. This is the
    // off-by-one the doc comment argues for; pin it.
    if hda_starve_deficit_rs(dma, dma) != 1 { bad |= 1 << 2; }
    if hda_starve_lead_rs(dma, dma) != 0 { bad |= 1 << 3; }

    // Run past: the deficit counts every unwritten slot the engine reached.
    if hda_starve_deficit_rs(dma - 9, dma) != 10 { bad |= 1 << 4; }
    if hda_starve_lead_rs(dma - 9, dma) != 0 { bad |= 1 << 5; }

    // THE INVARIANT THAT MATTERS: the deficit is exactly what
    // `hda_starve_resync_rs` is about to repair, at every position on both
    // sides of the head. If these two ever disagree then the counter is
    // measuring something other than the event it is named after, which is the
    // failure mode that would make a tuning change look effective when it was
    // not.
    for d in 0..64u64 {
        let wr = dma + 8 - d;
        let repaired = hda_starve_resync_rs(wr, dma);
        let deficit = hda_starve_deficit_rs(wr, dma);
        if repaired != wr && repaired - wr != deficit { bad |= 1 << 6; }
        if repaired == wr && deficit != 0 { bad |= 1 << 7; }
    }

    // A wr_slot far below the head must saturate to 0, never wrap.
    if hda_starve_lead_rs(0, dma) != 0 { bad |= 1 << 8; }
    if hda_starve_deficit_rs(0, dma) != dma + 1 { bad |= 1 << 9; }

    bad
}
