// rustkern/inputlat.rs - #affinity: INPUT-TO-PRESENT LATENCY. The first
// instrument in this kernel that measures what a user actually FEELS.
//
// WHY THIS EXISTS. Every performance number this project has is THROUGHPUT:
// frames per second, instructions per second, per-core busy percentages, lock
// occupancy, context switches per second. None of them measures RESPONSIVENESS,
// and the owner has twice reported the machine "slower" in terms no existing
// instrument could confirm or refute. A frame rate cannot: a compositor holding
// a steady 60 fps while every keystroke waits 200 ms for the compositor to be
// scheduled looks perfect on every counter we have and feels broken.
//
// WHAT IT MEASURES, precisely, in three stages. Each is a separate histogram
// because each fails differently and a single composite number would hide which
// half moved.
//
//   S_WAIT     a cooked key entering the keyboard ring  ->  a consumer
//              dequeuing it (keyboard_get_char). This is QUEUEING plus
//              SCHEDULER LATENCY: how long the key sat there because the
//              process that wanted it was not running. THIS is the stage a
//              scheduling change (priority, affinity, core count) moves.
//
//   S_INPUT    scancode arrival at keyboard_process_scancode()  ->  the same
//              dequeue. S_WAIT plus the in-ISR translation. The gap between
//              S_INPUT and S_WAIT is the cost of the funnel itself.
//
//   S_PRESENT  scancode arrival  ->  the first framebuffer present that
//              COMPLETES after that dequeue. This is the closest the kernel can
//              get to input-to-photon without the display's cooperation.
//
// HONESTY ABOUT S_PRESENT. It is a LOWER BOUND on true input-to-photon, and it
// is named for what it measures rather than for what we wish it measured:
//
//   1. The closing present is the first one after delivery. That frame may not
//      yet CONTAIN the key's visible effect - an app that needs another frame
//      to redraw will have its real photon on a later present. So the true
//      figure is >= this one, never <.
//   2. It stops at the back->front copy inside sys_fb_flip(), not at the
//      photon. Scanout, and on a VM the host's own display pipeline, are after
//      this point and are not counted.
//   3. It cannot tell a cursor-only present from a content present, so a mouse
//      moving during the measurement can close a sample early. The damaged
//      pixel AREA of the closing present is recorded alongside so that this is
//      visible in the data rather than assumed away; a run with a still mouse
//      is the clean one.
//
// S_WAIT and S_INPUT carry none of those caveats. They are exact differences of
// two mono_us() reads on the same clock, and they are the stages this ticket's
// scheduling work can actually move. Quote those two when reporting a
// scheduling change; quote S_PRESENT for the end-to-end story with its caveats
// attached.
//
// WHY mono_us AND NOT timer_ticks. timer_ticks counts ticks DELIVERED, not time
// ELAPSED: under KVM a starved vCPU gets its missed ticks re-delivered in a
// burst, so it leaps ~1250 ticks in 15 ms of real time (cpu/mono.h, #524/#525).
// An instrument built on it would report its largest error exactly when the
// machine is busiest, which is exactly when this instrument is read. mono_us()
// is TSC-backed, calibrated once against PIT channel 0, and valid with
// interrupts off - which this needs, because the first stamp is taken in an
// ISR.
//
// #426 COMPLIANCE. Everything here is integer-only (the kernel target is
// soft-float with SSE disabled), lock-free, allocation-free and wait-free. It
// is called from the PS/2 IRQ1 ISR and from inside sys_fb_flip()'s
// interrupts-off present window, so it may not block, allocate or take a lock.
// There is no loop here whose trip count is not a compile-time constant.

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

/// Depth of the arrival-timestamp FIFO. MUST be >= KEYBOARD_BUFFER_SIZE in
/// cpu/isr.c, or a burst could overflow this ring while the C ring still
/// accepts, desynchronising the FIFO pairing that makes S_WAIT exact.
/// cpu/isr.c carries a _Static_assert tying the two together; a constant that
/// agrees with another constant only because a comment says so is precisely the
/// failure mode this tree keeps hitting (see rustkern/cpuobs.rs on #143).
pub const INPUTLAT_RING: usize = 256;

/// Histogram buckets. Bucket i covers [2^i, 2^(i+1)) microseconds, so bucket 0
/// is 1..2us and bucket 23 is ~8.4s..16.8s. A sample of 0us lands in bucket 0.
/// Anything at or above 2^24 us (16.8s) saturates into the top bucket rather
/// than being dropped: an off-scale sample must stay visible, because a
/// discarded outlier is how a latency instrument comes to report that
/// everything is fine.
pub const INPUTLAT_BUCKETS: usize = 24;

/// Stage identifiers. Kept in one place so the C side and the report cannot
/// disagree about which histogram is which.
pub const S_WAIT: usize = 0;
pub const S_INPUT: usize = 1;
pub const S_PRESENT: usize = 2;
pub const INPUTLAT_STAGES: usize = 3;

// ---------------------------------------------------------------------------
// Pure accounting, testable without any kernel state.
// ---------------------------------------------------------------------------

/// Which histogram bucket a microsecond value belongs in.
///
/// Pure. This is the one place the bucket rule is expressed; the report reads
/// bucket lower bounds back out with `inputlat_bucket_lo_rs`, so the two can
/// only ever agree.
pub fn bucket_of(us: u64) -> usize {
    if us < 2 {
        return 0;
    }
    // 64 - leading_zeros(us) - 1 == floor(log2(us)) for us >= 1.
    let b = 63 - us.leading_zeros() as usize;
    if b >= INPUTLAT_BUCKETS {
        INPUTLAT_BUCKETS - 1
    } else {
        b
    }
}

/// Lower bound in microseconds of bucket `b`.
#[no_mangle]
pub extern "C" fn inputlat_bucket_lo_rs(b: u32) -> u64 {
    let b = b as usize;
    if b == 0 {
        0
    } else if b >= INPUTLAT_BUCKETS {
        1u64 << (INPUTLAT_BUCKETS - 1)
    } else {
        1u64 << b
    }
}

/// Percentile over a bucket histogram, returned as the LOWER BOUND of the
/// bucket the percentile falls in, in microseconds.
///
/// Pure, so it is directly testable. Returns 0 when there are no samples, and
/// the caller distinguishes "0 us" from "no data" by checking the count, which
/// the report line prints alongside. Reporting a bucket lower bound rather than
/// interpolating is deliberate: interpolation would invent precision the
/// histogram does not have, and a latency figure that looks more precise than
/// its instrument is how a measurement stops being evidence.
pub fn percentile(counts: &[u64; INPUTLAT_BUCKETS], pct: u32) -> u64 {
    let mut total: u64 = 0;
    let mut i = 0;
    while i < INPUTLAT_BUCKETS {
        total = total.wrapping_add(counts[i]);
        i += 1;
    }
    if total == 0 {
        return 0;
    }
    // Rank of the sample we want, 1-based, rounded up.
    let want = (total.saturating_mul(pct as u64) + 99) / 100;
    let want = if want == 0 { 1 } else { want };
    let mut acc: u64 = 0;
    let mut i = 0;
    while i < INPUTLAT_BUCKETS {
        acc = acc.wrapping_add(counts[i]);
        if acc >= want {
            return inputlat_bucket_lo_rs(i as u32);
        }
        i += 1;
    }
    inputlat_bucket_lo_rs((INPUTLAT_BUCKETS - 1) as u32)
}

// ---------------------------------------------------------------------------
// Live state.
// ---------------------------------------------------------------------------

struct Stage {
    counts: [AtomicU64; INPUTLAT_BUCKETS],
    n: AtomicU64,
    sum: AtomicU64,
    max: AtomicU64,
    min: AtomicU64,
}

#[allow(clippy::declare_interior_mutable_const)]
const ZERO_U64: AtomicU64 = AtomicU64::new(0);

impl Stage {
    const fn new() -> Self {
        Stage {
            counts: [ZERO_U64; INPUTLAT_BUCKETS],
            n: AtomicU64::new(0),
            sum: AtomicU64::new(0),
            max: AtomicU64::new(0),
            min: AtomicU64::new(u64::MAX),
        }
    }
    fn record(&self, us: u64) {
        self.counts[bucket_of(us)].fetch_add(1, Ordering::Relaxed);
        self.n.fetch_add(1, Ordering::Relaxed);
        self.sum.fetch_add(us, Ordering::Relaxed);
        // Not a compare-exchange loop on purpose: this runs in an ISR and in
        // the present's interrupts-off window, and an unbounded CAS retry there
        // is the #426 anti-pattern. A racing pair of updates can lose one
        // extremum; the counts, n and sum are exact, and those carry the
        // percentiles that the verdict is read from.
        if us > self.max.load(Ordering::Relaxed) {
            self.max.store(us, Ordering::Relaxed);
        }
        if us < self.min.load(Ordering::Relaxed) {
            self.min.store(us, Ordering::Relaxed);
        }
    }
    fn snapshot(&self) -> [u64; INPUTLAT_BUCKETS] {
        let mut out = [0u64; INPUTLAT_BUCKETS];
        let mut i = 0;
        while i < INPUTLAT_BUCKETS {
            out[i] = self.counts[i].load(Ordering::Relaxed);
            i += 1;
        }
        out
    }
}

static STAGES: [Stage; INPUTLAT_STAGES] = [Stage::new(), Stage::new(), Stage::new()];

/// Arrival time of the scancode currently being translated. Written at the top
/// of keyboard_process_scancode(), read if and only if that scancode produces a
/// cooked key. One scancode produces zero or one cooked keys (an 0xE0 prefix
/// and most release codes produce none), which is exactly why the stamp cannot
/// simply be taken at push time and why it cannot be taken only at arrival.
static CUR_T0: AtomicU64 = AtomicU64::new(0);

/// FIFO of arrival timestamps, paired one-to-one with the cooked-key ring in
/// cpu/isr.c. Pushed only when kb_push() actually ACCEPTS a key, so an overflow
/// drop on the C side drops here too and the pairing cannot skew.
#[allow(clippy::declare_interior_mutable_const)]
const ZERO_SLOT: AtomicU64 = AtomicU64::new(0);
static RING: [AtomicU64; INPUTLAT_RING] = [ZERO_SLOT; INPUTLAT_RING];
static WR: AtomicU64 = AtomicU64::new(0);
static RD: AtomicU64 = AtomicU64::new(0);
/// Pushes refused because this ring was full while the C ring was not. Must
/// stay 0; non-zero means INPUTLAT_RING is smaller than KEYBOARD_BUFFER_SIZE
/// and the pairing has skewed, so it is printed rather than inferred.
static RING_OVER: AtomicU64 = AtomicU64::new(0);

/// The delivered-but-not-yet-presented sample. Holds the OLDEST outstanding
/// one: if a second key is delivered before any present, the first key's photon
/// latency is the larger and the one a user notices, so keeping it is the
/// conservative direction.
static AWAIT_T0: AtomicU64 = AtomicU64::new(0);
static AWAIT_ACTIVE: AtomicU32 = AtomicU32::new(0);
/// Deliveries that arrived while a sample was already awaiting a present, i.e.
/// samples whose S_PRESENT was not measured. The denominator that says how much
/// of the typing this stage actually saw.
static AWAIT_COALESCED: AtomicU64 = AtomicU64::new(0);

/// Total damaged pixel area of the present that closed the last S_PRESENT
/// sample, so a suspiciously fast close can be checked against "that was a
/// cursor-only present" rather than argued about. 0 means a full present.
static LAST_CLOSE_AREA: AtomicU64 = AtomicU64::new(0);

/// Keys stamped, keys pushed, keys delivered. Three separate counters because a
/// gap between any adjacent pair localises a fault: stamped>pushed means the
/// scancode produced no cooked key (normal for prefixes and releases),
/// pushed>delivered means keys are queueing and nobody is draining them.
static N_SCANCODE: AtomicU64 = AtomicU64::new(0);
static N_PUSH: AtomicU64 = AtomicU64::new(0);
static N_DELIVER: AtomicU64 = AtomicU64::new(0);
static N_PRESENT_CLOSE: AtomicU64 = AtomicU64::new(0);

// ---------------------------------------------------------------------------
// The three hooks.
// ---------------------------------------------------------------------------

/// A raw scancode arrived. Called at the top of keyboard_process_scancode(),
/// which cpu/isr.c documents as the single function every scancode source
/// funnels through: the PS/2 IRQ1 handler, the polled i8042 drain, USB HID,
/// Bluetooth HID and the #334 serial test channel. Measuring here rather than
/// in the PS/2 ISR is what makes this instrument work on the real iMac, whose
/// keyboard is USB and never touches IRQ1.
///
/// `t0_us` is mono_us() read by the caller, so the read is visibly at the top
/// of the ISR rather than one call deeper.
#[no_mangle]
pub extern "C" fn inputlat_scancode_rs(t0_us: u64) {
    CUR_T0.store(t0_us, Ordering::Relaxed);
    N_SCANCODE.fetch_add(1, Ordering::Relaxed);
}

/// A cooked key was ACCEPTED into the keyboard ring. Called from kb_push()
/// inside the branch that actually stores, never on the drop path.
#[no_mangle]
pub extern "C" fn inputlat_push_rs() {
    let w = WR.load(Ordering::Relaxed);
    let r = RD.load(Ordering::Acquire);
    if w.wrapping_sub(r) >= INPUTLAT_RING as u64 {
        RING_OVER.fetch_add(1, Ordering::Relaxed);
        return;
    }
    RING[(w as usize) % INPUTLAT_RING].store(CUR_T0.load(Ordering::Relaxed), Ordering::Relaxed);
    WR.store(w.wrapping_add(1), Ordering::Release);
    N_PUSH.fetch_add(1, Ordering::Relaxed);
}

/// A consumer DEQUEUED a cooked key. Called from keyboard_get_char() after the
/// ring read has committed, with `t_us` = mono_us().
///
/// Closes S_WAIT and S_INPUT, and opens the S_PRESENT sample.
#[no_mangle]
pub extern "C" fn inputlat_deliver_rs(t_us: u64) {
    let r = RD.load(Ordering::Relaxed);
    if r >= WR.load(Ordering::Acquire) {
        // A key with no paired stamp: pushed before this module was reached, or
        // injected through keyboard_push_cooked_key(). Not an error, and NOT
        // recorded as a zero-latency sample, which would drag every percentile
        // down and make the instrument flatter than the truth.
        return;
    }
    let t0 = RING[(r as usize) % INPUTLAT_RING].load(Ordering::Relaxed);
    RD.store(r.wrapping_add(1), Ordering::Release);
    N_DELIVER.fetch_add(1, Ordering::Relaxed);

    // Guard against a non-monotonic pair rather than recording a wrapped
    // difference. mono_us() is monotonic, so this should never fire; if it
    // does, a silently enormous sample would be worse than a dropped one.
    if t_us < t0 {
        return;
    }
    let d = t_us - t0;
    STAGES[S_INPUT].record(d);
    STAGES[S_WAIT].record(d);

    if AWAIT_ACTIVE.load(Ordering::Relaxed) == 0 {
        AWAIT_T0.store(t0, Ordering::Relaxed);
        AWAIT_ACTIVE.store(1, Ordering::Release);
    } else {
        AWAIT_COALESCED.fetch_add(1, Ordering::Relaxed);
    }
}

/// A framebuffer present COMPLETED. Called from sys_fb_flip() after the
/// back->front copy, with `t_us` = mono_us() and `damage_area` = the total
/// damaged pixel area of this present (0 for a full-screen present).
///
/// Closes S_PRESENT if a delivered key is waiting for one.
#[no_mangle]
pub extern "C" fn inputlat_present_rs(t_us: u64, damage_area: u64) {
    if AWAIT_ACTIVE.load(Ordering::Acquire) == 0 {
        return;
    }
    let t0 = AWAIT_T0.load(Ordering::Relaxed);
    AWAIT_ACTIVE.store(0, Ordering::Release);
    if t_us < t0 {
        return;
    }
    LAST_CLOSE_AREA.store(damage_area, Ordering::Relaxed);
    N_PRESENT_CLOSE.fetch_add(1, Ordering::Relaxed);
    STAGES[S_PRESENT].record(t_us - t0);
}

// ---------------------------------------------------------------------------
// Readout.
// ---------------------------------------------------------------------------

/// Percentiles and extrema for one stage, for the serial report line.
///
/// # Safety
/// Each non-null pointer must be a valid, aligned, writable u64. Nulls are
/// skipped individually so a caller may ask for part of it.
#[no_mangle]
pub unsafe extern "C" fn inputlat_stage_rs(
    stage: u32,
    n: *mut u64,
    p50: *mut u64,
    p95: *mut u64,
    max: *mut u64,
    min: *mut u64,
    mean: *mut u64,
) -> i32 {
    let s = stage as usize;
    if s >= INPUTLAT_STAGES {
        return -1;
    }
    let st = &STAGES[s];
    let counts = st.snapshot();
    let cnt = st.n.load(Ordering::Relaxed);
    if !n.is_null() {
        *n = cnt;
    }
    if !p50.is_null() {
        *p50 = percentile(&counts, 50);
    }
    if !p95.is_null() {
        *p95 = percentile(&counts, 95);
    }
    if !max.is_null() {
        *max = st.max.load(Ordering::Relaxed);
    }
    if !min.is_null() {
        let m = st.min.load(Ordering::Relaxed);
        *min = if m == u64::MAX { 0 } else { m };
    }
    if !mean.is_null() {
        *mean = if cnt == 0 {
            0
        } else {
            st.sum.load(Ordering::Relaxed) / cnt
        };
    }
    0
}

/// The pipeline counters, so a report can say WHERE keys are being lost rather
/// than only how slow the survivors were.
///
/// # Safety
/// As `inputlat_stage_rs`.
#[no_mangle]
pub unsafe extern "C" fn inputlat_counts_rs(
    scancodes: *mut u64,
    pushed: *mut u64,
    delivered: *mut u64,
    closed: *mut u64,
    coalesced: *mut u64,
    ring_over: *mut u64,
    last_area: *mut u64,
) {
    if !scancodes.is_null() {
        *scancodes = N_SCANCODE.load(Ordering::Relaxed);
    }
    if !pushed.is_null() {
        *pushed = N_PUSH.load(Ordering::Relaxed);
    }
    if !delivered.is_null() {
        *delivered = N_DELIVER.load(Ordering::Relaxed);
    }
    if !closed.is_null() {
        *closed = N_PRESENT_CLOSE.load(Ordering::Relaxed);
    }
    if !coalesced.is_null() {
        *coalesced = AWAIT_COALESCED.load(Ordering::Relaxed);
    }
    if !ring_over.is_null() {
        *ring_over = RING_OVER.load(Ordering::Relaxed);
    }
    if !last_area.is_null() {
        *last_area = LAST_CLOSE_AREA.load(Ordering::Relaxed);
    }
}

/// Zero every histogram and counter, so a measurement can be bracketed around a
/// specific action instead of being contaminated by everything since boot.
/// The FIFO is deliberately NOT reset: a key in flight across the reset would
/// then be delivered with no stamp and, worse, could pair with a LATER key's
/// stamp and record a negative-looking interval as a huge positive one.
#[no_mangle]
pub extern "C" fn inputlat_reset_rs() {
    let mut s = 0;
    while s < INPUTLAT_STAGES {
        let st = &STAGES[s];
        let mut i = 0;
        while i < INPUTLAT_BUCKETS {
            st.counts[i].store(0, Ordering::Relaxed);
            i += 1;
        }
        st.n.store(0, Ordering::Relaxed);
        st.sum.store(0, Ordering::Relaxed);
        st.max.store(0, Ordering::Relaxed);
        st.min.store(u64::MAX, Ordering::Relaxed);
        s += 1;
    }
    N_SCANCODE.store(0, Ordering::Relaxed);
    N_PUSH.store(0, Ordering::Relaxed);
    N_DELIVER.store(0, Ordering::Relaxed);
    N_PRESENT_CLOSE.store(0, Ordering::Relaxed);
    AWAIT_COALESCED.store(0, Ordering::Relaxed);
    RING_OVER.store(0, Ordering::Relaxed);
    LAST_CLOSE_AREA.store(0, Ordering::Relaxed);
}

/// One bucket's count, for dumping the full distribution rather than only two
/// percentiles. Returns 0 for an out-of-range stage or bucket.
#[no_mangle]
pub extern "C" fn inputlat_bucket_rs(stage: u32, b: u32) -> u64 {
    let s = stage as usize;
    let b = b as usize;
    if s >= INPUTLAT_STAGES || b >= INPUTLAT_BUCKETS {
        return 0;
    }
    STAGES[s].counts[b].load(Ordering::Relaxed)
}

// ---------------------------------------------------------------------------
// Self-test.
// ---------------------------------------------------------------------------

/// Prove the ACCOUNTING, on synthetic input, before any number from the live
/// path is quoted. Returns 0 on success, or a bitmask of failed checks.
///
/// This covers the pure half: bucketing and percentiles. It cannot prove the
/// STAMPS are live - that a real keystroke reaches inputlat_scancode_rs() and a
/// real present reaches inputlat_present_rs(). Nothing a self-test can do
/// proves that, because a self-test calls the functions itself. That half is
/// proven on hardware by the deliberate negative control: cpu/isr.c can inject
/// a known delay into the delivery path, and S_WAIT must move by that amount.
/// A green self-test here is necessary and NOT sufficient, and this comment
/// exists so nobody reads it as sufficient.
#[no_mangle]
pub extern "C" fn inputlat_selftest_rs() -> i32 {
    let mut fail: i32 = 0;

    // Bucketing at the boundaries.
    if bucket_of(0) != 0 {
        fail |= 1 << 0;
    }
    if bucket_of(1) != 0 {
        fail |= 1 << 1;
    }
    if bucket_of(2) != 1 {
        fail |= 1 << 2;
    }
    if bucket_of(3) != 1 {
        fail |= 1 << 3;
    }
    if bucket_of(4) != 2 {
        fail |= 1 << 4;
    }
    if bucket_of(1023) != 9 {
        fail |= 1 << 5;
    }
    if bucket_of(1024) != 10 {
        fail |= 1 << 6;
    }
    // 20 ms, the value the negative control injects, must be its own bucket
    // and not saturate: 2^14 = 16384 <= 20000 < 32768 = 2^15.
    if bucket_of(20_000) != 14 {
        fail |= 1 << 7;
    }
    // Off-scale saturates into the top bucket rather than being dropped.
    if bucket_of(u64::MAX) != INPUTLAT_BUCKETS - 1 {
        fail |= 1 << 8;
    }

    // Bucket lower bounds are the inverse of the bucket rule.
    if inputlat_bucket_lo_rs(0) != 0 {
        fail |= 1 << 9;
    }
    if inputlat_bucket_lo_rs(10) != 1024 {
        fail |= 1 << 10;
    }
    if inputlat_bucket_lo_rs(14) != 16384 {
        fail |= 1 << 11;
    }

    // Percentiles. Empty histogram reports 0 and does not divide by zero.
    let empty = [0u64; INPUTLAT_BUCKETS];
    if percentile(&empty, 50) != 0 {
        fail |= 1 << 12;
    }

    // 100 samples all in bucket 3 (8..15us): every percentile is 8.
    let mut one = [0u64; INPUTLAT_BUCKETS];
    one[3] = 100;
    if percentile(&one, 50) != 8 {
        fail |= 1 << 13;
    }
    if percentile(&one, 95) != 8 {
        fail |= 1 << 14;
    }

    // 90 samples in bucket 3, 10 in bucket 14: p50 is the fast bucket, p95 is
    // the slow one. This is the shape the whole instrument exists to
    // distinguish - a good median hiding a bad tail - so if this check does not
    // hold, no report from this module means anything.
    let mut tail = [0u64; INPUTLAT_BUCKETS];
    tail[3] = 90;
    tail[14] = 10;
    if percentile(&tail, 50) != 8 {
        fail |= 1 << 15;
    }
    if percentile(&tail, 95) != 16384 {
        fail |= 1 << 16;
    }
    // p99 must also land in the tail, and p100 must be the very top sample.
    if percentile(&tail, 99) != 16384 {
        fail |= 1 << 17;
    }
    if percentile(&tail, 100) != 16384 {
        fail |= 1 << 18;
    }
    // p1 must be the fast bucket: a percentile function that returned the tail
    // for every query would pass every check above this one.
    if percentile(&tail, 1) != 8 {
        fail |= 1 << 19;
    }

    fail
}

/// NEGATIVE CONTROL for the self-test itself: feed the percentile function a
/// histogram whose answer is known to DIFFER from the one the checks above
/// assert, and confirm it comes out different. Returns 0 if the self-test is
/// discriminating, 1 if it is not.
///
/// This exists because this tree has shipped a truncation detector structurally
/// unable to fire and a harness that scored a boot with 65 panic lines as a
/// pass. A self-test that returns 0 proves nothing unless something can make it
/// return non-zero.
#[no_mangle]
pub extern "C" fn inputlat_selftest_negative_rs() -> i32 {
    let mut tail = [0u64; INPUTLAT_BUCKETS];
    tail[3] = 90;
    tail[14] = 10;
    // If p95 and p50 came out EQUAL on this histogram, the percentile function
    // is not discriminating and every verdict built on it is worthless.
    if percentile(&tail, 95) == percentile(&tail, 50) {
        return 1;
    }
    // And a histogram with a single sample must report that sample at every
    // percentile, including p1 - the rounding-up path.
    let mut solo = [0u64; INPUTLAT_BUCKETS];
    solo[7] = 1;
    if percentile(&solo, 1) != 128 || percentile(&solo, 100) != 128 {
        return 1;
    }
    0
}
