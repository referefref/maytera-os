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
