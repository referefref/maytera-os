// rustkern/fmq.rs - #182: the ONE-WAY BRIDGE that carries a DOS guest's OPL2
// register writes out of Ring 0 to the Ring-3 synthesiser.
//
// New kernel logic, so Rust per the 2026-07-16 rule. There is no float here and
// no performance reason for C: the hot path is one bounds check and a 16-byte
// store per guest `out dx,al` to port 0x389, on a path that already costs an
// emulated ISA bus cycle (#176, 1000 ns).
//
// ===========================================================================
// WHY A QUEUE AND NOT A SYNTHESISER
// ---------------------------------------------------------------------------
// The synthesiser is in USERLAND and there will never be a second one. The
// reasons are in userland/lib/opl2/opl2core.rs; the one that decides THIS
// file's existence is that the kernel is built -mno-sse soft-float, so the
// per-sample work of nine FM channels has no business in Ring 0 even if it
// could be made to fit. What the kernel owes the synthesiser is the guest's
// register writes, IN ORDER, WITH TIMESTAMPS, and nothing else.
//
// TIMESTAMPS ARE NOT OPTIONAL AND THEY ARE NOT timer_ticks.
// Each event carries mono_us(), the TSC-backed monotonic microsecond clock
// (cpu/mono.h, SYS_MONO_US). It is NOT timer_ticks, because blame.md's
// "timer-ticks-is-not-a-wall-clock" entry records that KVM replays a starved
// vCPU's lost tick IRQs in BURSTS: a tick-derived timestamp would bunch a
// second's worth of note-ons into one instant and the music would stutter in a
// way that looked like a synthesis bug and is not one.
//
// Without timestamps the consumer could only apply every drained write at the
// start of its next audio block, quantising note timing to the block size. With
// them it can place each write at the right SAMPLE inside the block. That is
// the difference between a melody and a melody played on a grid.
//
// ===========================================================================
// #426 (NO BUSY-WAIT). This module never waits and neither does its syscall:
// the drain is NON-BLOCKING and returns 0 on an empty queue.
//
// That is not a poll loop by omission, and here is why it is not. The consumer
// (/APPS/FMSYNTH) is paced by the AUDIO SINK: its loop blocks in exactly one
// place, sys_audio_pcm_write's wait queue (drivers/audio_pcm.c), which sleeps
// until the pump has consumed frames. The FM queue is drained once per audio
// block on the way round, so an empty queue costs one syscall per block and
// never spins. Making the drain block instead would DEADLOCK the design: a
// sustaining note must keep producing samples with no register writes arriving
// at all, so a consumer that slept on the event queue would stop feeding the
// sink and the note would cut off.
//
// The producer never waits either. dos_out is called from the guest's
// interpreter with the DOS window lock discipline around it, and an enqueue
// onto a full ring DROPS and COUNTS rather than waiting for space. Dropping is
// correct here: the alternative is stalling a DOS guest's I/O instruction on a
// Ring-3 process, which is a priority inversion with a game on the wrong side
// of it. The drop counter is reported so a drop is never silent.
// ===========================================================================

/// One OPL2 register write, as it happened.
///
/// Mirrors `dos_fm_event_t` in dos/dosexec.c, sizeof- and offsetof-locked
/// there. 16 bytes, naturally aligned, no padding surprises.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FmEvent {
    /// mono_us() at the instant the guest issued the write. See the header.
    pub t_us: u64,
    /// The register index that was latched at port 0x388.
    pub reg: u8,
    /// The byte written to port 0x389.
    pub val: u8,
    /// FMEV_* below.
    pub flags: u8,
    pub _pad: u8,
    /// Monotonic across the life of the queue. The consumer uses the DELTA
    /// between consecutive events, not the value: a gap larger than 1 means
    /// events were dropped between them, which it reports rather than
    /// silently rendering the wrong music.
    pub seq: u32,
}

/// A new guest has taken the chip: reset the synthesiser before applying this
/// event. Carried as a flag on the FIRST event of a session rather than as a
/// side channel, so it can never arrive out of order with respect to the
/// writes it precedes.
pub const FMEV_RESET: u8 = 0x01;

/// Ring capacity. 1024 events is 16 KB.
///
/// SIZED FROM MEASUREMENT, not from a round number. The corpus's heaviest
/// measured burst is Keen 5 loading its instrument bank: 264 register writes
/// back to back (#176's DOSBUS re-measurement). The consumer drains once per
/// audio block, and at 512 frames / 44100 Hz that is every 11.6 ms. A title
/// would have to issue 1024 writes inside one 11.6 ms block to overflow this,
/// which is four times the largest burst ever measured and would itself be the
/// interesting fact. See `dropped`.
pub const FMQ_CAP: usize = 1024;

/// Mirrors `dos_fm_queue_t` in dos/dosexec.c. Locked there.
///
/// The C side owns the lock. This module is pure logic over a &mut, exactly as
/// rustkern/opl2.rs is, so that the locking discipline has ONE definition in
/// the file that also holds the lock rather than two opinions in two languages.
#[repr(C)]
pub struct FmQueue {
    /// Producer index, free-running. Modulo FMQ_CAP for the slot.
    pub head: u32,
    /// Consumer index, free-running.
    pub tail: u32,
    /// Events the producer had to throw away because the ring was full. Any
    /// non-zero value is reported by the consumer AND by the guest-exit
    /// diagnostic: a dropped register write is a wrong note or a stuck one,
    /// and a synthesiser that sounded wrong for a reason nobody logged is the
    /// unexplained-silence failure this whole ticket exists to avoid.
    pub dropped: u32,
    /// Next sequence number to hand out.
    pub next_seq: u32,
    /// 1 while a DOS guest holds the chip. Cleared at guest exit, which is how
    /// the consumer learns to render its tail and exit rather than feeding the
    /// sink silence forever.
    pub active: u8,
    /// Set on the next event to be produced. See FMEV_RESET.
    pub pending_reset: u8,
    pub _pad: [u8; 2],
    /// Total events ever enqueued, including dropped ones. Diagnostics.
    pub n_pushed: u32,
    /// (#187) HIGH-WATER queue depth: the largest `used()` ever reached.
    ///
    /// WHY A HIGH-WATER MARK AND NOT A SAMPLE. `dropped` only becomes non-zero
    /// once the ring has ALREADY overflowed, so it answers "did we lose music"
    /// but not "how close did we come". Sampling `used()` on a periodic tick
    /// cannot answer it either: the consumer drains every audio block (~23 ms)
    /// and the producer arrives in bursts, so a 5 s sample reads ~0 essentially
    /// always and would report a comfortable queue right up until the run that
    /// drops. Only a peak recorded IN the push path can distinguish "never got
    /// past 3 of 1024" from "repeatedly reached 900".
    ///
    /// COST: one compare against a field already in a dirty cache line, on a
    /// path that already costs an emulated 1000 ns ISA bus cycle (#176). That
    /// is why this is unconditional rather than gated: per #122 an instrument
    /// must cost less than what it measures, and this one is ~6 orders of
    /// magnitude under its own event.
    pub hi_used: u32,
    /// Keeps `ev` 8-byte aligned (FmEvent leads with a u64). Without this the
    /// compiler inserts the padding anyway and the offsetof locks in dosexec.c
    /// would encode an implicit number instead of a declared one.
    pub _pad2: u32,
    pub ev: [FmEvent; FMQ_CAP],
}

impl FmQueue {
    fn used(&self) -> u32 {
        self.head.wrapping_sub(self.tail)
    }
}

/// Reset the queue and mark the chip claimed by a guest.
///
/// `active = 1` here rather than at the first register write: a guest that
/// detects the chip and then plays no music must still cause the consumer to
/// start and stop cleanly, or the FIRST note of the next guest would be the one
/// that starts it, and that note would be late.
#[no_mangle]
pub extern "C" fn dos_fmq_open_rs(q: *mut FmQueue) {
    if q.is_null() {
        return;
    }
    // SAFETY: the caller passes &g_dos_fmq, a file-scope static in dosexec.c
    // that outlives every DOS task, under g_dos_fmq_lock.
    let s = unsafe { &mut *q };
    s.head = 0;
    s.tail = 0;
    s.dropped = 0;
    s.next_seq = 0;
    s.n_pushed = 0;
    s.hi_used = 0;
    s.active = 1;
    s.pending_reset = 1;
}

/// The guest is gone. The consumer will drain what is left, render the tail of
/// any note still sounding, and exit.
#[no_mangle]
pub extern "C" fn dos_fmq_close_rs(q: *mut FmQueue) {
    if q.is_null() {
        return;
    }
    // SAFETY: as above.
    let s = unsafe { &mut *q };
    s.active = 0;
}

/// Enqueue one register write. Never waits. Returns 1 if it was queued, 0 if
/// it was dropped.
#[no_mangle]
pub extern "C" fn dos_fmq_push_rs(q: *mut FmQueue, reg: u8, val: u8, t_us: u64) -> i32 {
    if q.is_null() {
        return 0;
    }
    // SAFETY: as above; caller holds g_dos_fmq_lock.
    let s = unsafe { &mut *q };
    if s.active == 0 {
        return 0;
    }
    s.n_pushed = s.n_pushed.wrapping_add(1);
    if s.used() >= FMQ_CAP as u32 {
        // FULL. Drop the NEW event, not the oldest.
        //
        // That choice is deliberate and it is the opposite of what a ring
        // buffer usually wants. FM register writes are not samples, they are
        // STATE TRANSITIONS: dropping the oldest would discard a note-on and
        // keep the note-off that ends it, leaving a channel that never sounds.
        // Dropping the newest loses the tail of a burst, which is recoverable
        // because the next write to the same register supersedes it.
        s.dropped = s.dropped.wrapping_add(1);
        return 0;
    }
    let slot = (s.head as usize) % FMQ_CAP;
    let flags = s.pending_reset;
    s.pending_reset = 0;
    let seq = s.next_seq;
    s.next_seq = seq.wrapping_add(1);
    s.ev[slot] = FmEvent {
        t_us,
        reg,
        val,
        flags,
        _pad: 0,
        seq,
    };
    s.head = s.head.wrapping_add(1);
    // (#187) after the push, so the peak includes the event just queued.
    let used = s.used();
    if used > s.hi_used {
        s.hi_used = used;
    }
    1
}

/// Copy up to `max` events into `out`. Returns how many were copied.
///
/// The caller (the syscall) is responsible for `out` pointing at `max` valid
/// FmEvent slots in a KERNEL buffer; the copy to Ring 3 happens above this, so
/// this function never touches a user pointer and cannot be the site of a
/// copy_to_user fault.
#[no_mangle]
pub extern "C" fn dos_fmq_drain_rs(q: *mut FmQueue, out: *mut FmEvent, max: u32) -> u32 {
    if q.is_null() || out.is_null() || max == 0 {
        return 0;
    }
    // SAFETY: as above, plus the caller's guarantee about `out`.
    let s = unsafe { &mut *q };
    let mut n = 0u32;
    while n < max && s.tail != s.head {
        let slot = (s.tail as usize) % FMQ_CAP;
        // SAFETY: n < max and the caller guarantees max valid slots.
        unsafe {
            *out.add(n as usize) = s.ev[slot];
        }
        s.tail = s.tail.wrapping_add(1);
        n += 1;
    }
    n
}

/// Packed status for the consumer, so it learns "guest gone" and "events were
/// dropped" without a second syscall.
///   bit      0  active (a guest holds the chip)
///   bits 32..63 dropped count, saturated at u32
#[no_mangle]
pub extern "C" fn dos_fmq_status_rs(q: *const FmQueue) -> u64 {
    if q.is_null() {
        return 0;
    }
    // SAFETY: as above.
    let s = unsafe { &*q };
    (s.active as u64 & 1) | ((s.dropped as u64) << 32)
}

/// Self-test. Returns the number of FAILING checks, so 0 is the pass, matching
/// dos_opl2_selftest_rs.
///
/// EVERY CHECK HAS A NEGATIVE ARM, for the reason stated at the top of
/// rustkern/opl2.rs's own self-test: a queue tested only in the case where it
/// works has not been tested. In particular the OVERFLOW case is tested
/// explicitly, because the drop policy above (drop the NEWEST) is a decision
/// that would otherwise be a comment nobody ever exercised.
#[no_mangle]
pub extern "C" fn dos_fmq_selftest_rs(q: *mut FmQueue, scratch: *mut FmEvent, scratch_n: u32) -> i32 {
    if q.is_null() || scratch.is_null() || scratch_n < 8 {
        return 99;
    }
    let mut fails = 0i32;

    // ---- 1. a closed queue accepts nothing --------------------------------
    dos_fmq_close_rs(q);
    if dos_fmq_push_rs(q, 0x20, 0x01, 1000) != 0 {
        fails += 1;
    }

    // ---- 2. open, push, drain, in order and with the RESET flag first -----
    dos_fmq_open_rs(q);
    for i in 0..8u32 {
        if dos_fmq_push_rs(q, 0xA0, i as u8, 1000 + i as u64) != 1 {
            fails += 1;
        }
    }
    let got = dos_fmq_drain_rs(q, scratch, 8);
    if got != 8 {
        fails += 1;
    }
    // SAFETY: got <= 8 <= scratch_n.
    let ev = unsafe { core::slice::from_raw_parts(scratch, got as usize) };
    if ev.is_empty() || ev[0].flags & FMEV_RESET == 0 {
        fails += 1; // the first event of a session must carry RESET
    }
    for i in 1..ev.len() {
        if ev[i].flags & FMEV_RESET != 0 {
            fails += 1; // and ONLY the first
        }
        if ev[i].seq != ev[i - 1].seq + 1 {
            fails += 1; // sequence must be dense when nothing was dropped
        }
        if ev[i].t_us <= ev[i - 1].t_us {
            fails += 1; // timestamps must advance
        }
        if ev[i].val != ev[i - 1].val + 1 {
            fails += 1; // and ORDER must be preserved
        }
    }

    // ---- 3. an empty queue drains ZERO and does not wait ------------------
    if dos_fmq_drain_rs(q, scratch, 8) != 0 {
        fails += 1;
    }

    // (#187) the peak after the 8-event section above must be exactly 8, not 0
    // and not left over from a previous session. This is the arm that catches a
    // counter that is never updated: every other check here would still pass.
    unsafe {
        if (*q).hi_used != 8 {
            fails += 1;
        }
    }

    // ---- 4. OVERFLOW drops the NEWEST and counts it -----------------------
    // Fill the ring exactly, then push one more. The extra must be refused,
    // the counter must move, and the OLDEST event must still be there, because
    // dropping the oldest would discard note-ons and keep note-offs.
    dos_fmq_open_rs(q);
    let mut i = 0u32;
    while i < FMQ_CAP as u32 {
        dos_fmq_push_rs(q, 0xB0, (i & 0xFF) as u8, 2000 + i as u64);
        i += 1;
    }
    if dos_fmq_push_rs(q, 0xB0, 0xEE, 9_000_000) != 0 {
        fails += 1; // a full ring must refuse
    }
    let st = dos_fmq_status_rs(q);
    if (st >> 32) != 1 {
        fails += 1; // exactly one drop counted
    }
    // (#187) the high-water mark must have reached EXACTLY capacity: the ring
    // was filled and then over-pushed. Both arms matter. If it read less than
    // capacity the counter is not tracking the peak, and if it read more it is
    // counting the dropped event, which never occupied a slot.
    unsafe {
        if (*q).hi_used != FMQ_CAP as u32 {
            fails += 1;
        }
    }
    if (st & 1) != 1 {
        fails += 1; // still active
    }
    let got2 = dos_fmq_drain_rs(q, scratch, 4);
    if got2 != 4 {
        fails += 1;
    }
    // SAFETY: got2 <= 4 <= scratch_n.
    let ev2 = unsafe { core::slice::from_raw_parts(scratch, got2 as usize) };
    if ev2.is_empty() || ev2[0].t_us != 2000 {
        fails += 1; // the OLDEST survived, which is the whole point
    }

    // ---- 5. close makes the queue report inactive, drain still works ------
    // The consumer MUST be able to drain after the guest is gone, or the last
    // note-off of the session is lost and the final note hangs forever. That
    // is a real failure mode, not a hypothetical: it is the exact shape of the
    // stuck-note bug this ordering is chosen to prevent.
    dos_fmq_close_rs(q);
    if (dos_fmq_status_rs(q) & 1) != 0 {
        fails += 1;
    }
    if dos_fmq_drain_rs(q, scratch, 8) == 0 {
        fails += 1; // there were 1020 left; a closed queue must still drain
    }

    // Leave it closed and empty rather than in whatever state the last check
    // happened to reach. A self-test that leaves the device armed is how you
    // get a boot that behaves differently depending on whether tests ran.
    dos_fmq_open_rs(q);
    dos_fmq_close_rs(q);
    let _ = scratch_n;
    fails
}
