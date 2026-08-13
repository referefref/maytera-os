// rustkern/conring.rs - #745 (task #70): THE ASYNCHRONOUS CONSOLE RING.
//
// The index arithmetic and the overflow POLICY for the kernel console ring, and
// nothing else. Rust per the 2026-07-16 rule: this is new kernel code, it is
// pure, and every bug this class of code has ever had lives in exactly these
// four numbers (head, tail, "is it full", "where does a dropped line resume").
//
// The BYTES stay in C, in a static array in serial.c, for two reasons: the
// buffer has to be reachable from the panic path with no allocator and no
// borrow discipline, and this module then needs no `static mut` at all. Same
// shape as procreap.rs and proc_mem.rs: C owns the storage, Rust owns the
// decision. The struct layout is locked by a `_Static_assert` on
// `sizeof(conring_t)` in serial.c.
//
// ===========================================================================
// THE DEFECT THIS EXISTS TO REMOVE
// ---------------------------------------------------------------------------
// `serial_write()` polls the UART's THRE bit once per character. At 115200
// baud that is about 87us per character, so a 120-character kprintf line is
// about 10 MILLISECONDS of busy-polling. kprintf is called from inside ISRs
// and from inside locks, and since #67 pass 7 it is called under
// `g_console_lock`. On one core nobody waits, so the cost was invisible for
// the life of the project. With a second scheduling core it serialises the
// whole kernel: a caller holding the Big Kernel Lock calls kprintf, blocks on
// the console lock, and the BKL is now held for as long as SOMEBODY ELSE'S log
// line takes to drain. Measured (#67, same workload, only the console lock
// differing):
//
//   build 253, no console lock:   7 contended BKL acquires,       961 spins, maxhold     12 us
//   build 256, console lock:     34 contended BKL acquires, 4,165,755 spins, maxhold 26,475 us
//
// The lock is not the defect and has to stay: without it two cores interleave
// the log a character at a time and the log is the only diagnostic this work
// has. The defect is that the WRITE IS SYNCHRONOUS. So kprintf now pushes into
// this ring under that same short lock and returns, and a drain thread does the
// slow polled writes holding no long-lived lock.
//
// ===========================================================================
// THE THREE RISKS, HANDLED HERE RATHER THAN DISCOVERED LATER
// ---------------------------------------------------------------------------
// 1. OVERFLOW MUST NOT BLOCK, AND A DROP MUST NOT BE SILENT. When the ring is
//    full we drop, never wait. But we do not drop a byte here and a byte there,
//    which would corrupt lines into something that reads like a memory bug:
//    the ring enters DROPPING mode and swallows everything until it sees a
//    newline AND has real space again, so the log resumes on a clean line
//    boundary. The caller then injects a visible `[CONDROP n]` marker, so a
//    gap in the log always announces itself and carries its own size.
//
// 2. A DRAIN THAT NEVER RUNS MUST NOT BLACK-HOLE THE LOG. `stall_drops` counts
//    how long the consumer has moved NOTHING AT ALL, measured with a clock the
//    caller supplies (conring_stall_check_rs, called only on a drop). If that
//    passes the caller's limit the consumer is gone - never scheduled, wedged,
//    or dead - and the ring has
//    silently become a shredder. We then return FALLBACK, and serial.c latches
//    the console back to the old synchronous path for the rest of the boot
//    after flushing whatever the ring still holds. Slow and complete beats fast
//    and empty, and an unflushed buffer at a panic is a worse bug than the one
//    this ticket is fixing.
//
// 3. WAKE-UP MUST NOT BE LOST. A push that takes the ring from EMPTY to
//    NON-EMPTY returns the WAKE bit. That is the only edge on which the
//    consumer can be asleep, and the caller does the wake AFTER dropping the
//    console lock, so the console lock is never held across the scheduler.
// ===========================================================================

/// The byte was stored.
pub const R_ACCEPTED: u32 = 0x1;
/// The ring went from empty to non-empty: the consumer may be asleep.
pub const R_WAKE: u32 = 0x2;
/// Dropping mode just ended at a line boundary; emit the `[CONDROP n]` marker.
pub const R_RESUME: u32 = 0x4;
/// This byte was thrown away.
pub const R_DROPPED: u32 = 0x8;
/// Reserved: the fallback verdict now comes from `conring_stall_check_rs`,
/// which is the only place with a clock. Kept so the bit values in serial.h do
/// not silently shift meaning if another result is ever added.
pub const R_FALLBACK: u32 = 0x10;

/// Ring state. The byte buffer itself is separate and owned by C.
///
/// Field order is chosen so the struct has no interior padding on x86-64
/// (five u64 then eight u32), because the C side locks `sizeof` with a
/// `_Static_assert` and a padding surprise would fail the build for a reason
/// that reads like a compiler bug.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ConRing {
    /// Characters thrown away since boot.
    pub dropped: u64,
    /// Number of separate times the ring entered dropping mode.
    pub drop_events: u64,
    /// Characters accepted since boot.
    pub pushed: u64,
    /// Characters handed to the consumer since boot.
    pub popped: u64,
    /// Incremented by every pop that moved at least one byte. THIS is the
    /// consumer-liveness signal; the stall rule keys on it not advancing.
    pub drain_seq: u64,
    /// `drain_seq` as observed when the current stall window opened.
    pub stall_seq: u64,
    /// Timestamp (mono_us) when the current stall window opened, 0 = none.
    pub stall_since_us: u64,
    /// Capacity of the C byte buffer. MUST be a power of two.
    pub size: u32,
    /// Next write index.
    pub head: u32,
    /// Next read index.
    pub tail: u32,
    /// 1 while swallowing bytes until a clean line boundary.
    pub dropping: u32,
    /// Greatest occupancy ever seen, so the ring can be sized from evidence.
    pub highwater: u32,
    /// Characters dropped since the consumer last moved anything. Diagnostic
    /// only: it is NOT the fallback trigger, because a producer can drop an
    /// unbounded number of characters in the 22 ms it takes the UART to shift
    /// one 256-byte chunk, which is normal back-pressure and not a dead
    /// consumer. Keying the fallback on this number latched a perfectly
    /// healthy console back to synchronous on the very first boot that used it.
    pub stall_drops: u32,
    /// Explicit tail padding so the layout is stated, not inferred: seven
    /// u64 then eight u32 is 88 bytes with no interior padding on x86-64.
    pub _pad: u32,
    pub _pad2: u32,
}

const _: () = assert!(core::mem::size_of::<ConRing>() == 88);

impl ConRing {
    #[inline]
    fn mask(&self) -> u32 {
        self.size - 1
    }
    /// Bytes currently waiting to be drained.
    #[inline]
    fn used(&self) -> u32 {
        (self.head.wrapping_sub(self.tail)) & self.mask()
    }
    /// Bytes that can still be accepted. One slot is permanently reserved so
    /// `head == tail` means EMPTY and never FULL.
    #[inline]
    fn free(&self) -> u32 {
        self.size - 1 - self.used()
    }
}

/// Initialise. Returns 0 on success, non-zero if `size` is unusable (not a
/// power of two, smaller than 64, or larger than 8 MiB). A bad size is a
/// programming error in serial.c, and returning an error rather than clamping
/// means it shows up at boot instead of as a subtly wrong ring.
///
/// # Safety
/// `st` must point at a writable `ConRing`.
#[no_mangle]
pub unsafe extern "C" fn conring_init_rs(st: *mut ConRing, size: u32) -> u32 {
    if st.is_null() {
        return 1;
    }
    if size < 64 || size > (8 << 20) || (size & (size - 1)) != 0 {
        return 2;
    }
    core::ptr::write(
        st,
        ConRing {
            dropped: 0,
            drop_events: 0,
            pushed: 0,
            popped: 0,
            drain_seq: 0,
            stall_seq: 0,
            stall_since_us: 0,
            size,
            head: 0,
            tail: 0,
            dropping: 0,
            highwater: 0,
            stall_drops: 0,
            _pad: 0,
            _pad2: 0,
        },
    );
    0
}

/// IS THE CONSUMER DEAD? Called by C on a DROP only (never per character), with
/// the console lock held and a real clock in hand, because this module has no
/// clock of its own and must not acquire one.
///
/// Returns 1 exactly when the drain has moved NOT ONE BYTE for `limit_us`
/// while we were still throwing characters away. Any pop at all reopens the
/// window. That distinguishes "the UART is slower than the kernel right now",
/// which is normal and self-correcting, from "nothing is draining this ring",
/// which means the console has silently become a shredder and must revert to
/// synchronous writes.
///
/// # Safety
/// `st` must point at an initialised `ConRing`.
#[no_mangle]
pub unsafe extern "C" fn conring_stall_check_rs(
    st: *mut ConRing,
    now_us: u64,
    limit_us: u64,
) -> u32 {
    if st.is_null() {
        return 0;
    }
    let s = &mut *st;
    if s.stall_since_us == 0 || s.stall_seq != s.drain_seq {
        s.stall_seq = s.drain_seq;
        s.stall_since_us = if now_us == 0 { 1 } else { now_us };
        return 0;
    }
    if now_us.wrapping_sub(s.stall_since_us) >= limit_us {
        1
    } else {
        0
    }
}

/// Push one byte. Returns a bitwise OR of the `R_*` constants above.
///
/// The caller MUST hold the console lock (serial.c's `g_console_lock`) across
/// this call: the ring is single-producer only in the sense that the lock makes
/// it so. It must NOT hold that lock while acting on `R_WAKE`.
///
/// # Safety
/// `st` must point at a `ConRing` initialised by [`conring_init_rs`], and `buf`
/// at a writable array of `st->size` bytes.
#[no_mangle]
pub unsafe extern "C" fn conring_push_rs(st: *mut ConRing, buf: *mut u8, c: u8) -> u32 {
    if st.is_null() || buf.is_null() {
        return 0;
    }
    let s = &mut *st;

    if s.dropping != 0 {
        // Swallow until BOTH conditions hold: we are at a line boundary, and a
        // real amount of room has come back. Resuming on the first free byte
        // would re-enter dropping mode on the very next line and turn the log
        // into confetti; a quarter of the ring is enough headroom to show the
        // consumer has genuinely caught up.
        note_drop(s);
        if c == b'\n' && s.free() >= s.size / 4 {
            s.dropping = 0;
            return R_RESUME;
        }
        return R_DROPPED;
    }

    if s.free() == 0 {
        s.dropping = 1;
        s.drop_events = s.drop_events.wrapping_add(1);
        note_drop(s);
        return R_DROPPED;
    }

    let was_empty = s.used() == 0;
    let h = s.head;
    core::ptr::write(buf.add(h as usize), c);
    s.head = (h + 1) & s.mask();
    s.pushed = s.pushed.wrapping_add(1);
    let u = s.used();
    if u > s.highwater {
        s.highwater = u;
    }
    R_ACCEPTED | if was_empty { R_WAKE } else { 0 }
}

/// Account for one thrown-away character. Split out so the two drop sites
/// cannot drift apart. The liveness VERDICT is not taken here: it needs a
/// clock, and this is the per-character path.
#[inline]
fn note_drop(s: &mut ConRing) {
    s.dropped = s.dropped.wrapping_add(1);
    s.stall_drops = s.stall_drops.saturating_add(1);
}

/// Pop up to `cap` bytes into `out`. Returns how many were copied.
///
/// The caller MUST hold the console lock across this call, and MUST NOT hold it
/// while writing the returned bytes to the UART - that is the entire point of
/// the exercise.
///
/// # Safety
/// `st` and `buf` as for [`conring_push_rs`]; `out` must be writable for `cap`
/// bytes.
#[no_mangle]
pub unsafe extern "C" fn conring_pop_rs(
    st: *mut ConRing,
    buf: *const u8,
    out: *mut u8,
    cap: u32,
) -> u32 {
    if st.is_null() || buf.is_null() || out.is_null() || cap == 0 {
        return 0;
    }
    let s = &mut *st;
    let mut n = s.used();
    if n > cap {
        n = cap;
    }
    if n == 0 {
        return 0;
    }
    let mask = s.mask();
    let mut t = s.tail;
    for i in 0..n {
        core::ptr::write(out.add(i as usize), core::ptr::read(buf.add(t as usize)));
        t = (t + 1) & mask;
    }
    s.tail = t;
    s.popped = s.popped.wrapping_add(n as u64);
    s.drain_seq = s.drain_seq.wrapping_add(1);
    // The consumer is demonstrably alive: forgive every drop taken so far and
    // close any open stall window.
    s.stall_drops = 0;
    s.stall_since_us = 0;
    n
}

/// Bytes waiting to be drained. Cheap, and safe to call with the lock held or
/// not: a torn read can only make the consumer loop once more.
///
/// # Safety
/// `st` must point at an initialised `ConRing`.
#[no_mangle]
pub unsafe extern "C" fn conring_pending_rs(st: *const ConRing) -> u32 {
    if st.is_null() {
        return 0;
    }
    (*st).used()
}

// ===========================================================================
// SELF-TEST
// ---------------------------------------------------------------------------
// Runs at boot from serial.c. Returns 0 on success, or the 1-based number of
// the first failing case, so a failure names itself on the console instead of
// being a mysterious log truncation three hours into a run.
//
// It drives the REAL exported functions over a 64-byte ring on the stack and
// never reaches inside the struct to arrange a state, so the wrap arithmetic,
// the dropping state machine and the stall detector are all exercised by the
// same code the kernel runs.
// ===========================================================================

/// Returns 0 on success, else the number of the first failing case.
#[no_mangle]
pub extern "C" fn conring_selftest_rs() -> u32 {
    const N: u32 = 64;
    let mut buf = [0u8; N as usize];
    let mut st = ConRing {
        dropped: 0,
        drop_events: 0,
        pushed: 0,
        popped: 0,
        drain_seq: 0,
        size: N,
        head: 0,
        tail: 0,
        dropping: 0,
        highwater: 0,
        stall_drops: 0,
        stall_seq: 0,
        stall_since_us: 0,
        _pad: 0,
        _pad2: 0,
    };
    let p = &mut st as *mut ConRing;
    let b = buf.as_mut_ptr();
    let mut out = [0u8; 128];
    let o = out.as_mut_ptr();

    unsafe {
        // 1: a non-power-of-two size is REJECTED, not clamped.
        let mut junk = st;
        if conring_init_rs(&mut junk as *mut ConRing, 100) == 0 {
            return 1;
        }
        if conring_init_rs(p, N) != 0 {
            return 2;
        }
        // 3: an empty ring pops nothing.
        if conring_pending_rs(p) != 0 || conring_pop_rs(p, b, o, 128) != 0 {
            return 3;
        }
        // 4/5: the first byte reports WAKE, the second does not.
        if conring_push_rs(p, b, b'A') != (R_ACCEPTED | R_WAKE) {
            return 4;
        }
        if conring_push_rs(p, b, b'B') != R_ACCEPTED {
            return 5;
        }
        // 6/7: FIFO order, and the ring empties.
        if conring_pop_rs(p, b, o, 128) != 2 || out[0] != b'A' || out[1] != b'B' {
            return 6;
        }
        if conring_pending_rs(p) != 0 {
            return 7;
        }
        // 8/9: emptying re-arms the WAKE edge.
        if conring_push_rs(p, b, b'C') & R_WAKE == 0 {
            return 8;
        }
        if conring_pop_rs(p, b, o, 128) != 1 || out[0] != b'C' {
            return 9;
        }

        // 10-12: WRAP. 40 laps of push-3/pop-3 walks head and tail right around
        // the buffer several times, checking the payload every lap. An
        // off-by-one in the mask shows up here and nowhere else.
        for lap in 0..40u32 {
            let v = (lap & 0x7f) as u8;
            for k in 0..3u8 {
                if conring_push_rs(p, b, v.wrapping_add(k)) & R_ACCEPTED == 0 {
                    return 10;
                }
            }
            if conring_pop_rs(p, b, o, 128) != 3 {
                return 11;
            }
            if out[0] != v || out[1] != v.wrapping_add(1) || out[2] != v.wrapping_add(2) {
                return 12;
            }
        }

        // 13/14: a partial pop leaves the remainder, in order.
        for k in 0..10u8 {
            if conring_push_rs(p, b, b'0' + k) & R_ACCEPTED == 0 {
                return 13;
            }
        }
        if conring_pop_rs(p, b, o, 4) != 4 || out[0] != b'0' || out[3] != b'3' {
            return 14;
        }
        if conring_pop_rs(p, b, o, 128) != 6 || out[0] != b'4' || out[5] != b'9' {
            return 15;
        }

        // 16-18: CAPACITY is exactly size-1 and the size-th byte starts dropping.
        if conring_init_rs(p, N) != 0 {
            return 16;
        }
        for _ in 0..(N - 1) {
            if conring_push_rs(p, b, b'x') & R_ACCEPTED == 0 {
                return 17;
            }
        }
        if conring_pending_rs(p) != N - 1 {
            return 18;
        }
        let r = conring_push_rs(p, b, b'y');
        if r & R_DROPPED == 0 || r & R_ACCEPTED != 0 || st.drop_events != 1 {
            return 19;
        }

        // 20: while dropping, a newline alone does NOT resume - there is still
        //     no room, and resuming into a full ring is how you get confetti.
        if conring_push_rs(p, b, b'\n') & R_RESUME != 0 {
            return 20;
        }
        // 21/22: free some space, but under a quarter of the ring: still dropping.
        if conring_pop_rs(p, b, o, 4) != 4 {
            return 21;
        }
        if conring_push_rs(p, b, b'\n') & R_RESUME != 0 {
            return 22;
        }
        // 23/24: free well over a quarter, but a NON-newline still does not
        //        resume: the resume point is a line boundary, not a threshold.
        if conring_pop_rs(p, b, o, 40) != 40 {
            return 23;
        }
        if conring_push_rs(p, b, b'z') & R_RESUME != 0 {
            return 24;
        }
        // 25-27: newline plus space -> RESUME, the byte is NOT stored (the
        //        caller emits the marker instead), and normal service resumes.
        let before = conring_pending_rs(p);
        if conring_push_rs(p, b, b'\n') & R_RESUME == 0 {
            return 25;
        }
        if conring_pending_rs(p) != before || st.dropping != 0 {
            return 26;
        }
        if conring_push_rs(p, b, b'q') & R_ACCEPTED == 0 {
            return 27;
        }

        // 28-31: THE STALL DETECTOR, which is now about TIME and not counts.
        //        A dead consumer: fill the ring, keep dropping, never pop. The
        //        clock is supplied by the caller, so the test can move it.
        if conring_init_rs(p, N) != 0 {
            return 28;
        }
        while conring_push_rs(p, b, b'#') & R_ACCEPTED != 0 {}
        // t=1s: first drop in this window, so it only OPENS the window.
        if conring_stall_check_rs(p, 1_000_000, 5_000_000) != 0 {
            return 29;
        }
        // t=5.9s: not yet 5s since the window opened.
        if conring_stall_check_rs(p, 5_900_000, 5_000_000) != 0 {
            return 30;
        }
        // t=6.1s: 5.1s with the drain having moved nothing. Dead.
        if conring_stall_check_rs(p, 6_100_000, 5_000_000) != 1 {
            return 31;
        }

        // 32-34: THE SAME PRESSURE WITH A LIVE CONSUMER never latches, no
        //        matter how long it goes on or how much it drops. One pop is
        //        enough to reopen the window.
        if conring_init_rs(p, N) != 0 {
            return 32;
        }
        for k in 0..20u64 {
            while conring_push_rs(p, b, b'#') & R_ACCEPTED != 0 {}
            // Ten seconds of wall time per round, far past the 5s limit...
            let now = 10_000_000 * (k + 1);
            if conring_stall_check_rs(p, now, 5_000_000) != 0 {
                return 33;
            }
            // ...but the drain moved bytes, so the window reopens every time.
            while conring_pop_rs(p, b, o, 128) != 0 {}
            if conring_push_rs(p, b, b'\n') & R_RESUME == 0 {
                return 34;
            }
        }
    }
    0
}
