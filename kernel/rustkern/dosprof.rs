// rustkern/dosprof.rs - WHERE A DOS GUEST'S FRAME TIME ACTUALLY GOES (no-ticket).
//
// New kernel logic, so Rust per the 2026-07-16 rule. There is no performance
// exemption to claim here and none is claimed: this is called a handful of
// times per FRAME, not per instruction, so the FFI hop cannot perturb what it
// measures. That property is the whole reason the buckets are coarse.
//
// WHY IT EXISTS. Three separate owner reports of "slow" DOS guests were each
// answered with a different theory (the blit, the scaler, the emulated clock)
// because the only two instruments that existed measured the two things
// somebody had already suspected: dos_view_report() times dos_present_inner()
// and the #232 speed line counts retired instructions. Between them they can
// say "presentation is 6.6% of a core" and "the guest got 7.5 M insn/s", and
// they CANNOT say where the other 93% went. A profile that only covers the
// suspects can never exonerate them, because the residual is invisible.
//
// THE BUCKETS ARE THE RUN LOOP'S OWN PHASES, not a taxonomy invented here.
// Both interpreters (dos_run_file's 16-bit loop and dos4gw_run's 32-bit one)
// have the identical shape, and every microsecond the DOS thread spends is in
// exactly one of them:
//
//   INTERP   x86_16_run() / x86_32_run() - the emulated instructions
//   PRESENT  dos_present() - fill_bars + the scaler into the window buffer
//   PUBLISH  win16_host_invalidate() - uw_commit_content()'s FULL-WINDOW memcpy
//            into content_presented, plus marking the WM dirty
//   INPUT    dos_pump_input() - host cursor/keys into guest state
//   YIELD    proc_yield() - wall time this thread was NOT running, i.e. what
//            the rest of the machine took. Charged because on ONE core
//            (g_smp_user_sched = 0) the compositor's time is time the guest
//            did not get, and a guest-side profile that omitted it would blame
//            the interpreter for the compositor's frame.
// There is deliberately NO "other" BUCKET. The residual is computed by the C
// reporter as (wall clock of the interval) minus (the sum of the buckets) and
// PRINTED, because an unaccounted residual is the only honest way for this
// instrument to say "the model is incomplete", and a bucket that nothing ever
// adds to would read as a measured zero rather than as an omission.
//
// PUBLISH_BYTES is carried separately because the publish cost is not a
// property of the guest at all - it is content_width * content_height * 4 per
// frame regardless of what the guest drew - and a byte count is what makes
// that legible next to a microsecond count.
//
// Diagnostic only, armed by /CONFIG/DOSSPEED.CFG, off in the golden: the
// accumulate path is six relaxed atomic adds per frame, but "cheap" is not a
// reason to leave an instrument on, and the gate already exists.

use core::sync::atomic::{AtomicU64, Ordering};

pub const DOSPROF_INTERP:  usize = 0;
pub const DOSPROF_PRESENT: usize = 1;
pub const DOSPROF_PUBLISH: usize = 2;
pub const DOSPROF_INPUT:   usize = 3;
pub const DOSPROF_YIELD:   usize = 4;
pub const DOSPROF_N:       usize = 5;

// Relaxed is correct and not a shortcut: there is ONE writer (the DOS task's
// own thread) and the reader is that same thread inside its own report. The
// atomics are here so the statics need no `unsafe`, not for cross-core
// ordering, and nothing downstream depends on two counters being consistent
// with each other at an instant.
static US:    [AtomicU64; DOSPROF_N] = [const { AtomicU64::new(0) }; DOSPROF_N];
static N:     [AtomicU64; DOSPROF_N] = [const { AtomicU64::new(0) }; DOSPROF_N];
static MAXUS: [AtomicU64; DOSPROF_N] = [const { AtomicU64::new(0) }; DOSPROF_N];
static PUBLISH_BYTES: AtomicU64 = AtomicU64::new(0);

/// Charge `us` microseconds (and one occurrence) to `bucket`. An out-of-range
/// bucket is DROPPED rather than folded into a neighbour: a profile that
/// silently mis-attributes is worse than one with a hole in it.
#[no_mangle]
pub extern "C" fn dosprof_add_rs(bucket: u32, us: u64) {
    let b = bucket as usize;
    if b >= DOSPROF_N {
        return;
    }
    US[b].fetch_add(us, Ordering::Relaxed);
    N[b].fetch_add(1, Ordering::Relaxed);
    // Not a CAS loop: a lost update here can only under-report a maximum by
    // one sample on a single-writer counter, and a CAS in an instrument is a
    // cost the instrument does not need to pay.
    if us > MAXUS[b].load(Ordering::Relaxed) {
        MAXUS[b].store(us, Ordering::Relaxed);
    }
}

/// Bytes the publish step copied. Separate from the microseconds so the report
/// can state the bandwidth, which is the number that does not change when the
/// host does.
#[no_mangle]
pub extern "C" fn dosprof_add_publish_bytes_rs(bytes: u64) {
    PUBLISH_BYTES.fetch_add(bytes, Ordering::Relaxed);
}

/// Mirrored by `dosprof_report_t` in dos/dosexec.c with a _Static_assert on the
/// size, same contract as vbe_present_t and dos_view_policy_t.
#[repr(C)]
pub struct DosProfReport {
    pub us:     [u64; DOSPROF_N],
    pub n:      [u64; DOSPROF_N],
    pub max_us: [u64; DOSPROF_N],
    pub publish_bytes: u64,
}

/// Snapshot every counter into `out` and reset them, so consecutive reports
/// describe consecutive intervals rather than a growing lifetime total. A
/// lifetime total cannot show a guest getting slower, which is exactly the
/// thing these reports are read for.
#[no_mangle]
pub extern "C" fn dosprof_report_rs(out: *mut DosProfReport) -> i32 {
    if out.is_null() {
        return -1;
    }
    // SAFETY: non-null, checked; the C caller passes the address of a
    // dosprof_report_t whose layout is locked to this type by a
    // _Static_assert on its size.
    let r = unsafe { &mut *out };
    for i in 0..DOSPROF_N {
        r.us[i]     = US[i].swap(0, Ordering::Relaxed);
        r.n[i]      = N[i].swap(0, Ordering::Relaxed);
        r.max_us[i] = MAXUS[i].swap(0, Ordering::Relaxed);
    }
    r.publish_bytes = PUBLISH_BYTES.swap(0, Ordering::Relaxed);
    0
}

/// Self-test: prove the accumulator adds, tracks a maximum, drops an
/// out-of-range bucket, and RESETS on report. Returns the number of failed
/// checks, 0 = pass. Called from the C side's existing selftest wiring so this
/// is a test that has been watched go green rather than one that compiles.
#[no_mangle]
pub extern "C" fn dosprof_selftest_rs() -> i32 {
    let mut bad = 0i32;
    let mut r = DosProfReport { us: [0; DOSPROF_N], n: [0; DOSPROF_N],
                                max_us: [0; DOSPROF_N], publish_bytes: 0 };
    // Start from a known-clear state whatever ran before.
    dosprof_report_rs(&mut r);
    dosprof_add_rs(DOSPROF_INTERP as u32, 100);
    dosprof_add_rs(DOSPROF_INTERP as u32, 300);
    dosprof_add_rs(DOSPROF_PRESENT as u32, 7);
    dosprof_add_rs(DOSPROF_N as u32, 999_999);      // must be dropped
    dosprof_add_publish_bytes_rs(4096);
    dosprof_report_rs(&mut r);
    if r.us[DOSPROF_INTERP] != 400 || r.n[DOSPROF_INTERP] != 2 { bad += 1; }
    if r.max_us[DOSPROF_INTERP] != 300 { bad += 1; }
    if r.us[DOSPROF_PRESENT] != 7 || r.n[DOSPROF_PRESENT] != 1 { bad += 1; }
    if r.publish_bytes != 4096 { bad += 1; }
    // The out-of-range add must not have landed anywhere at all.
    for i in 0..DOSPROF_N {
        if r.us[i] == 999_999 { bad += 1; }
    }
    // And the report must have RESET, not accumulated.
    dosprof_report_rs(&mut r);
    for i in 0..DOSPROF_N {
        if r.us[i] != 0 || r.n[i] != 0 || r.max_us[i] != 0 { bad += 1; }
    }
    if r.publish_bytes != 0 { bad += 1; }
    if dosprof_report_rs(core::ptr::null_mut()) != -1 { bad += 1; }
    bad
}

// ---- DURABLE MIRROR TO /BOOTLOG.TXT (dosgamespeed) ------------------------
//
// WHY. The [DOSFRAME]/[IOCOST]/[DOSTICK] serial lines answer "where does a DOS
// guest's real-time second go", but ONLY on serial, and the owner's iMac has
// NO serial port. So the one machine where the two live symptoms appear (Red
// Alert steady ~50% gameplay speed; Discworld II a periodic ~0.5s hitch) is the
// one machine that can never produce the numbers. This mirrors a COMPACT
// summary of the same numbers to /BOOTLOG.TXT, which survives a power cycle and
// is read by plugging the stick into any computer. Same gate as the serial
// lines (/CONFIG/DOSSPEED.CFG); the C reporter early-returns without it.
//
// RATE-LIMITED to one line per window (default ~15s), NOT one per 2s report,
// because on the live-USB target every bootlog_write() is a whole-file rewrite:
// a per-2s line would itself perturb the I/O it is measuring. Within the window
// it keeps the TRUE per-bucket MAXIMUM (not an average) and a hitch COUNT, so a
// half-second present/publish/yield spike that lands in any of the ~7 folded 2s
// reports still shows in the line, which is what the Discworld hitch needs; the
// steady kinsn/s the Red Alert symptom needs is the window average.
//
// Single-writer, same discipline as the rest of this file: the DOS task's own
// thread both notes and drains. Relaxed atomics only so the statics need no
// `unsafe`, not for cross-core ordering.

static DUR_WALL:  AtomicU64 = AtomicU64::new(0);
static DUR_INSN:  AtomicU64 = AtomicU64::new(0);
static DUR_BUS:   AtomicU64 = AtomicU64::new(0);
static DUR_US:    [AtomicU64; DOSPROF_N] = [const { AtomicU64::new(0) }; DOSPROF_N];
static DUR_MAX:   [AtomicU64; DOSPROF_N] = [const { AtomicU64::new(0) }; DOSPROF_N];
static DUR_FRAMES: AtomicU64 = AtomicU64::new(0);
static DUR_SKIP:   AtomicU64 = AtomicU64::new(0);
static DUR_OWED:   AtomicU64 = AtomicU64::new(0);
static DUR_GOT:    AtomicU64 = AtomicU64::new(0);
static DUR_DROP:   AtomicU64 = AtomicU64::new(0);
static DUR_HITCH:  AtomicU64 = AtomicU64::new(0);

/// Display-ready snapshot of one durable window. All rates already computed so
/// the C side only formats: it owns bootlog_write() (a varargs logger with no
/// clean Rust binding), this owns the arithmetic. Mirrored by
/// `dosprof_durable_out_t` in dos/dosexec.c with a _Static_assert on the size.
#[repr(C)]
pub struct DosDurableOut {
    pub emit: u64,          // 1 = a window closed and the fields below are live
    pub wall_ms: u64,       // real time this line covers
    pub insn_s: u64,        // guest instructions retired per second (the headline)
    pub io_s: u64,          // guest port accesses per second (in-gameplay discriminator)
    pub pm_interp: u64,     // permille of wall in each phase
    pub pm_present: u64,
    pub pm_publish: u64,
    pub pm_input: u64,
    pub pm_yield: u64,
    pub pm_resid: u64,      // unaccounted: guest INT services (CD read), throttle sleep, starvation
    pub interp_max_ms: u64, // worst single call in the window, per phase (hitch attribution)
    pub present_max_ms: u64,
    pub publish_max_ms: u64,
    pub input_max_ms: u64,
    pub yield_max_ms: u64,
    pub frames: u64,        // frames shown / skipped over the window
    pub skip: u64,
    pub owed_x10: u64,      // guest timer ticks owed / got per second, in tenths
    pub got_x10: u64,
    pub drop: u64,          // ticks dropped (must be 0 if pacing is healthy)
    pub hitch: u64,         // 2s sub-intervals whose present/publish/yield max exceeded the threshold
}

/// Fold one 2s report interval into the durable window. `us`/`max_us` are the
/// DOSPROF_N-element arrays from dosprof_report_rs (this interval, already
/// reset there). When the accumulated wall reaches `window_us`, populate `out`
/// (emit=1) with the window's display-ready numbers and reset; otherwise
/// out.emit=0. `hitch_thresh_us` flags this sub-interval as a hitch if any of
/// present/publish/yield had a single call longer than it.
#[no_mangle]
pub extern "C" fn dosprof_durable_note_rs(
    interval_wall_us: u64, interval_insn: u64, interval_bus: u64,
    us: *const u64, max_us: *const u64,
    frames: u64, skip: u64, owed: u64, got: u64, drop: u64,
    window_us: u64, hitch_thresh_us: u64,
    out: *mut DosDurableOut,
) {
    if out.is_null() || us.is_null() || max_us.is_null() {
        if !out.is_null() {
            // SAFETY: non-null checked.
            unsafe { (*out).emit = 0 };
        }
        return;
    }
    DUR_WALL.fetch_add(interval_wall_us, Ordering::Relaxed);
    DUR_INSN.fetch_add(interval_insn, Ordering::Relaxed);
    DUR_BUS.fetch_add(interval_bus, Ordering::Relaxed);
    DUR_FRAMES.fetch_add(frames, Ordering::Relaxed);
    DUR_SKIP.fetch_add(skip, Ordering::Relaxed);
    DUR_OWED.fetch_add(owed, Ordering::Relaxed);
    DUR_GOT.fetch_add(got, Ordering::Relaxed);
    DUR_DROP.fetch_add(drop, Ordering::Relaxed);

    let mut hitched = false;
    for i in 0..DOSPROF_N {
        // SAFETY: caller passes DOSPROF_N-element arrays (the dosprof_report_t
        // fields), null-checked above.
        let iv_us = unsafe { *us.add(i) };
        let iv_mx = unsafe { *max_us.add(i) };
        DUR_US[i].fetch_add(iv_us, Ordering::Relaxed);
        if iv_mx > DUR_MAX[i].load(Ordering::Relaxed) {
            DUR_MAX[i].store(iv_mx, Ordering::Relaxed);
        }
        if (i == DOSPROF_PRESENT || i == DOSPROF_PUBLISH || i == DOSPROF_YIELD)
            && iv_mx >= hitch_thresh_us
        {
            hitched = true;
        }
    }
    if hitched {
        DUR_HITCH.fetch_add(1, Ordering::Relaxed);
    }

    let wall = DUR_WALL.load(Ordering::Relaxed);
    if wall < window_us {
        // SAFETY: non-null checked.
        unsafe { (*out).emit = 0 };
        return;
    }

    // Window closed: drain to display-ready values and reset.
    let insn = DUR_INSN.swap(0, Ordering::Relaxed);
    let bus  = DUR_BUS.swap(0, Ordering::Relaxed);
    let mut phase_us = [0u64; DOSPROF_N];
    let mut phase_mx = [0u64; DOSPROF_N];
    let mut sum_us = 0u64;
    for i in 0..DOSPROF_N {
        phase_us[i] = DUR_US[i].swap(0, Ordering::Relaxed);
        phase_mx[i] = DUR_MAX[i].swap(0, Ordering::Relaxed);
        sum_us = sum_us.saturating_add(phase_us[i]);
    }
    let frames_w = DUR_FRAMES.swap(0, Ordering::Relaxed);
    let skip_w   = DUR_SKIP.swap(0, Ordering::Relaxed);
    let owed_w   = DUR_OWED.swap(0, Ordering::Relaxed);
    let got_w    = DUR_GOT.swap(0, Ordering::Relaxed);
    let drop_w   = DUR_DROP.swap(0, Ordering::Relaxed);
    let hitch_w  = DUR_HITCH.swap(0, Ordering::Relaxed);
    DUR_WALL.store(0, Ordering::Relaxed);

    let ms = wall / 1000;
    let ms1 = if ms == 0 { 1 } else { ms };
    let pm = |x: u64| -> u64 { x.saturating_mul(1000) / wall };
    let resid = wall.saturating_sub(sum_us);

    // SAFETY: non-null checked.
    let o = unsafe { &mut *out };
    o.emit = 1;
    o.wall_ms = ms;
    o.insn_s = insn.saturating_mul(1_000_000) / wall;
    o.io_s = bus.saturating_mul(1_000_000) / wall;
    o.pm_interp  = pm(phase_us[DOSPROF_INTERP]);
    o.pm_present = pm(phase_us[DOSPROF_PRESENT]);
    o.pm_publish = pm(phase_us[DOSPROF_PUBLISH]);
    o.pm_input   = pm(phase_us[DOSPROF_INPUT]);
    o.pm_yield   = pm(phase_us[DOSPROF_YIELD]);
    o.pm_resid   = pm(resid);
    o.interp_max_ms  = phase_mx[DOSPROF_INTERP] / 1000;
    o.present_max_ms = phase_mx[DOSPROF_PRESENT] / 1000;
    o.publish_max_ms = phase_mx[DOSPROF_PUBLISH] / 1000;
    o.input_max_ms   = phase_mx[DOSPROF_INPUT] / 1000;
    o.yield_max_ms   = phase_mx[DOSPROF_YIELD] / 1000;
    o.frames = frames_w;
    o.skip = skip_w;
    o.owed_x10 = owed_w.saturating_mul(10_000) / ms1;
    o.got_x10 = got_w.saturating_mul(10_000) / ms1;
    o.drop = drop_w;
    o.hitch = hitch_w;
}

/// Clear the durable window at guest start so a new run's first line does not
/// fold in the previous guest's tail. Cheap; called once per launch.
#[no_mangle]
pub extern "C" fn dosprof_durable_reset_rs() {
    DUR_WALL.store(0, Ordering::Relaxed);
    DUR_INSN.store(0, Ordering::Relaxed);
    DUR_BUS.store(0, Ordering::Relaxed);
    DUR_FRAMES.store(0, Ordering::Relaxed);
    DUR_SKIP.store(0, Ordering::Relaxed);
    DUR_OWED.store(0, Ordering::Relaxed);
    DUR_GOT.store(0, Ordering::Relaxed);
    DUR_DROP.store(0, Ordering::Relaxed);
    DUR_HITCH.store(0, Ordering::Relaxed);
    for i in 0..DOSPROF_N {
        DUR_US[i].store(0, Ordering::Relaxed);
        DUR_MAX[i].store(0, Ordering::Relaxed);
    }
}

/// Self-test: a window that has not closed yet emits nothing; one that closes
/// drains the right rates, flags a hitch on a present spike, and RESETS.
/// Returns failed checks, 0 = pass.
#[no_mangle]
pub extern "C" fn dosprof_durable_selftest_rs() -> i32 {
    let mut bad = 0i32;
    dosprof_durable_reset_rs();
    let zero = [0u64; DOSPROF_N];
    let mut out = DosDurableOut {
        emit: 9, wall_ms: 0, insn_s: 0, io_s: 0,
        pm_interp: 0, pm_present: 0, pm_publish: 0, pm_input: 0, pm_yield: 0, pm_resid: 0,
        interp_max_ms: 0, present_max_ms: 0, publish_max_ms: 0, input_max_ms: 0, yield_max_ms: 0,
        frames: 0, skip: 0, owed_x10: 0, got_x10: 0, drop: 0, hitch: 0,
    };
    // One 2s interval: 2,000,000 us wall, 64,000,000 insn (=> 32 M/s), 630 bus
    // (=> 315/s), interp 1,900,000 us (95%), present 40,000 us with a 300,000 us
    // (300 ms) max spike => a hitch. Window is 15 s so this must NOT emit yet.
    let mut us = zero; let mut mx = zero;
    us[DOSPROF_INTERP] = 1_900_000; us[DOSPROF_PRESENT] = 40_000;
    mx[DOSPROF_INTERP] = 30_000;    mx[DOSPROF_PRESENT] = 300_000;
    dosprof_durable_note_rs(2_000_000, 64_000_000, 630, us.as_ptr(), mx.as_ptr(),
                            30, 1, 106, 106, 0, 15_000_000, 150_000, &mut out);
    if out.emit != 0 { bad += 1; }
    // Seven more identical intervals reach 16 s of wall => the window closes on
    // this one.
    for _ in 0..7 {
        dosprof_durable_note_rs(2_000_000, 64_000_000, 630, us.as_ptr(), mx.as_ptr(),
                                30, 1, 106, 106, 0, 15_000_000, 150_000, &mut out);
    }
    if out.emit != 1 { bad += 1; }
    if out.insn_s < 31_900_000 || out.insn_s > 32_100_000 { bad += 1; }
    if out.io_s < 300 || out.io_s > 330 { bad += 1; }
    if out.pm_interp < 940 || out.pm_interp > 960 { bad += 1; }   // ~95%
    if out.present_max_ms != 300 { bad += 1; }                    // worst spike survives folding
    if out.hitch != 8 { bad += 1; }                               // every sub-interval hitched
    if out.drop != 0 { bad += 1; }
    // Drained: the next non-closing interval must emit nothing and see fresh maxes.
    let mut out2 = DosDurableOut {
        emit: 9, wall_ms: 0, insn_s: 0, io_s: 0,
        pm_interp: 0, pm_present: 0, pm_publish: 0, pm_input: 0, pm_yield: 0, pm_resid: 0,
        interp_max_ms: 0, present_max_ms: 0, publish_max_ms: 0, input_max_ms: 0, yield_max_ms: 0,
        frames: 0, skip: 0, owed_x10: 0, got_x10: 0, drop: 0, hitch: 0,
    };
    dosprof_durable_note_rs(1_000_000, 1_000_000, 10, zero.as_ptr(), zero.as_ptr(),
                            1, 0, 53, 53, 0, 15_000_000, 150_000, &mut out2);
    if out2.emit != 0 { bad += 1; }
    // Null must not fault and must report emit=0.
    let mut out3 = out2;
    out3.emit = 7;
    dosprof_durable_note_rs(1, 1, 1, core::ptr::null(), zero.as_ptr(),
                            0, 0, 0, 0, 0, 15_000_000, 150_000, &mut out3);
    if out3.emit != 0 { bad += 1; }
    dosprof_durable_note_rs(1, 1, 1, zero.as_ptr(), zero.as_ptr(),
                            0, 0, 0, 0, 0, 15_000_000, 150_000, core::ptr::null_mut());
    dosprof_durable_reset_rs();
    bad
}

// ---- PER-EVENT RUN-LOOP STALL DETECTOR (dosgamespeed) ---------------------
//
// The Discworld II symptom is a PERIODIC ~0.5 s hitch, not a steady slowdown,
// and the prime suspect is the owner's iMac USB root-port-4 re-enumeration
// (every ~3 s). Correlating the two needs the hitch logged AT THE MOMENT it
// happens: /BOOTLOG.TXT carries no timestamps (neither the [xHCI] re-scan lines
// nor anything else), so the only way to line a hitch up with its cause is
// ADJACENCY in the append-ordered file. A per-15 s [DOSPERF] aggregate is too
// coarse for a 3 s period; a per-event line lands right next to the [xHCI]
// re-scan that caused it.
//
// A hitch is one run-loop pass whose WALL time exceeds a threshold: the guest
// made no visible progress for that long, whatever the reason (a publish
// blocked on the BKL, the thread preempted off-core by a USB worker, a CD read
// inside a guest INT). dos_prof_report() is called once per pass in BOTH DOS
// run loops, so the gap between consecutive calls is one pass. Rate-limited so a
// storm of stalls cannot itself become the I/O load it is trying to explain;
// the ones suppressed here are still counted in the [DOSPERF] hitch total.
// Single writer (the DOS thread); state kept here.

static HITCH_LAST_TOP:  AtomicU64 = AtomicU64::new(0);
static HITCH_LAST_EMIT: AtomicU64 = AtomicU64::new(0);

/// Call once per run-loop pass with the current monotonic microseconds.
/// Returns the pass's wall time in MILLISECONDS if it exceeded `thresh_us` AND
/// at least `min_gap_us` has elapsed since the last emit (so the caller should
/// log a [DOSHITCH] line); otherwise 0. `now_us == 0` (clock not ready / not
/// armed) resets the baseline and never emits, so an unarmed run leaves no
/// stale `prev` to fire a spurious hitch on the first armed pass.
#[no_mangle]
pub extern "C" fn dosprof_hitch_note_rs(now_us: u64, thresh_us: u64, min_gap_us: u64) -> u64 {
    if now_us == 0 {
        HITCH_LAST_TOP.store(0, Ordering::Relaxed);
        return 0;
    }
    let prev = HITCH_LAST_TOP.swap(now_us, Ordering::Relaxed);
    if prev == 0 || now_us <= prev {
        return 0;
    }
    let gap = now_us - prev;
    if gap < thresh_us {
        return 0;
    }
    let last_emit = HITCH_LAST_EMIT.load(Ordering::Relaxed);
    if last_emit != 0 && now_us.saturating_sub(last_emit) < min_gap_us {
        return 0;
    }
    HITCH_LAST_EMIT.store(now_us, Ordering::Relaxed);
    gap / 1000
}

/// Reset the stall baseline at guest start (paired with dosprof_durable_reset_rs).
#[no_mangle]
pub extern "C" fn dosprof_hitch_reset_rs() {
    HITCH_LAST_TOP.store(0, Ordering::Relaxed);
    HITCH_LAST_EMIT.store(0, Ordering::Relaxed);
}

/// Self-test: baseline reset, sub-threshold silence, a real stall reported once,
/// the rate-limit, and that a later well-separated stall reports again. Returns
/// failed checks, 0 = pass.
#[no_mangle]
pub extern "C" fn dosprof_hitch_selftest_rs() -> i32 {
    let mut bad = 0i32;
    dosprof_hitch_reset_rs();
    // First armed pass only seeds the baseline: no gap yet, no emit.
    if dosprof_hitch_note_rs(1_000_000, 150_000, 300_000) != 0 { bad += 1; }
    // +20 ms pass: under threshold, silent.
    if dosprof_hitch_note_rs(1_020_000, 150_000, 300_000) != 0 { bad += 1; }
    // +300 ms pass from the last call: a 300 ms stall, reported (300 ms).
    if dosprof_hitch_note_rs(1_320_000, 150_000, 300_000) != 300 { bad += 1; }
    // +200 ms later: a real 200 ms stall, but within the 300 ms rate-limit of
    // the last emit, so suppressed (0) even though it exceeds the threshold.
    if dosprof_hitch_note_rs(1_520_000, 150_000, 300_000) != 0 { bad += 1; }
    // +200 ms pass, and now far enough past the last EMIT (400 us... 400 ms
    // since 1_320_000) that the rate-limit no longer suppresses it: reported,
    // and the value is the PASS gap (200 ms), not the time since the last emit.
    if dosprof_hitch_note_rs(1_720_000, 150_000, 300_000) != 200 { bad += 1; }
    // now_us == 0 resets and never emits.
    if dosprof_hitch_note_rs(0, 150_000, 300_000) != 0 { bad += 1; }
    if dosprof_hitch_note_rs(5_000_000, 150_000, 300_000) != 0 { bad += 1; }  // baseline reseeded, no gap
    dosprof_hitch_reset_rs();
    bad
}
