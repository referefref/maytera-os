// rustkern/ktime.rs - #115 (local 120): the kernel's ONE calendar-time converter.
//
// WHY THIS EXISTS. Before #115 the kernel had FOUR separate, mutually
// inconsistent notions of calendar time and no shared converter at all:
//
//   1. fs/netfs.c fat_entry_to_vfs_stat() converted a FAT date with
//        mtime = (create_date - 0x21) * 86400 + hours + minutes
//      which treats the PACKED 16-bit date word as a count of days. That is not
//      a conversion, it is arithmetic on a bitfield. It has been dead code
//      (__attribute__((unused))) for its whole life, which is the only reason
//      it never shipped a wrong date to a user.
//   2. fs/exfat.c exfat_timestamp_to_unix() has a real bitfield decode but its
//      day count is (year-1970)*365 + (year-1969)/4, which adds the CURRENT
//      year's leap day on 1 January rather than on 1 March: every date between
//      Jan 1 and Feb 28 of a leap year is one day late.
//   3. rustkern/sntp.rs has a correct epoch -> civil loop, but only that
//      direction and only inside the SNTP validator's own result struct.
//   4. proc/syscall.c sys_time() returns SECONDS SINCE BOOT (#113), which is
//      not calendar time at all.
//
// So this module is the shared primitive, per the CLAUDE.md reuse rule: one
// exact converter, both directions, with a boot self-test. New callers must use
// it rather than growing a fifth private copy.
//
// RUST, per the standing directive. This is new kernel code, it is pure integer
// arithmetic with no paging, no asm and no FPU (the kernel target is soft-float
// with SSE disabled, so a float-based date routine was never an option in any
// language), and the one class of bug it must not have - an out-of-range month
// indexing a 12-entry table - is exactly what Rust's bounds checks remove by
// construction. sntp.rs calls that branch out explicitly as "the branch that
// would have been a C buffer overrun".
//
// EXACTNESS. The civil <-> days conversion is Howard Hinnant's algorithm, which
// is closed-form and correct for the whole proleptic Gregorian calendar: no
// year loop, no leap-year special case to get wrong, no iteration bound to
// reason about. userland/libc/time.c already uses the same algorithm, so the
// kernel and userland now agree by construction rather than by coincidence.

use core::sync::atomic::{AtomicI64, Ordering};

extern "C" {
    // gui/clock.c. Both were UNBOUNDED "wait for RTC update" spins before #115
    // (a #426 violation); they are now bounded there. Declared here because the
    // wall clock belongs with the calendar arithmetic, not with the clock
    // widget that happens to host the port I/O.
    fn rtc_read_time(hour: *mut i32, minute: *mut i32, second: *mut i32);
    fn rtc_read_date(day: *mut i32, month: *mut i32, year: *mut i32, weekday: *mut i32);
}

// Plausibility window for anything we will WRITE to a filesystem or hand to a
// user as a date. Deliberately narrow: the whole point of #115 is to stop
// shipping values that merely look populated, so a clock that reads 1970 or
// 2149 must be reported as "no timestamp" rather than laundered into a stat
// field. 1980 is also the FAT/DOS epoch floor, below which a date is not even
// representable in a directory entry.
const KTIME_MIN_YEAR: i32 = 1980;
const KTIME_MAX_YEAR: i32 = 2107; // FAT year field is 7 bits: 1980 + 127.

/// Days since 1970-01-01 for a proleptic Gregorian civil date.
/// Howard Hinnant's `days_from_civil`. Exact; no loop.
fn days_from_civil(y_in: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y_in - 1 } else { y_in };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400; // [0, 399]
    let mp = (m + 9) % 12; // [0, 11], March = 0
    let doy = (153 * mp + 2) / 5 + d - 1; // [0, 365]
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
    era * 146097 + doe - 719468
}

/// Inverse of `days_from_civil`. Returns (year, month 1-12, day 1-31).
fn civil_from_days(z_in: i64) -> (i64, i64, i64) {
    let z = z_in + 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = z - era * 146097; // [0, 146096]
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    let mp = (5 * doy + 2) / 153; // [0, 11]
    let d = doy - (153 * mp + 2) / 5 + 1; // [1, 31]
    let m = if mp < 10 { mp + 3 } else { mp - 9 }; // [1, 12]
    (if m <= 2 { y + 1 } else { y }, m, d)
}

fn days_in_month(y: i32, m: i32) -> i32 {
    match m {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 => {
            if (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0) {
                29
            } else {
                28
            }
        }
        _ => 0,
    }
}

/// Civil (UTC) -> seconds since the UNIX epoch.
///
/// Returns -1 for ANY date outside the plausibility window or with an
/// impossible field (month 13, 31 February, hour 24, ...). -1 is a REFUSAL, not
/// a timestamp: callers must map it to "unknown" and must not store it. That is
/// the whole discipline #115 exists to enforce - a wrong-but-plausible zero is
/// worse than an admitted absence.
#[no_mangle]
pub extern "C" fn ktime_civil_to_unix_rs(
    year: i32,
    month: i32,
    day: i32,
    hour: i32,
    minute: i32,
    second: i32,
) -> i64 {
    if year < KTIME_MIN_YEAR || year > KTIME_MAX_YEAR {
        return -1;
    }
    if month < 1 || month > 12 {
        return -1;
    }
    if day < 1 || day > days_in_month(year, month) {
        return -1;
    }
    // 60 is accepted for a leap second and folded to :59 rather than refused,
    // because an RTC can legitimately present it and refusing would turn one
    // second per few years into a spurious "no timestamp".
    if hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60 {
        return -1;
    }
    let sec = if second == 60 { 59 } else { second };
    let days = days_from_civil(year as i64, month as i64, day as i64);
    days * 86_400 + (hour as i64) * 3600 + (minute as i64) * 60 + sec as i64
}

/// FAT/DOS packed directory-entry date+time -> seconds since the UNIX epoch.
///
///   date: bits 0-4 day (1-31), 5-8 month (1-12), 9-15 year - 1980
///   time: bits 0-4 seconds/2, 5-10 minutes, 11-15 hours
///
/// Returns 0 for an UNSTAMPED entry (date == 0), which is what every file this
/// kernel created before #115 has on disk, and 0 for a structurally impossible
/// entry. 0 here means "this filesystem does not know", and the stat path
/// reports it as such rather than as 1970-01-01.
#[no_mangle]
pub extern "C" fn ktime_dos_to_unix_rs(dos_date: u16, dos_time: u16) -> i64 {
    if dos_date == 0 {
        return 0;
    }
    let day = (dos_date & 0x1F) as i32;
    let month = ((dos_date >> 5) & 0x0F) as i32;
    let year = ((dos_date >> 9) & 0x7F) as i32 + 1980;
    let second = ((dos_time & 0x1F) as i32) * 2;
    let minute = ((dos_time >> 5) & 0x3F) as i32;
    let hour = ((dos_time >> 11) & 0x1F) as i32;
    let r = ktime_civil_to_unix_rs(year, month, day, hour, minute, second);
    if r < 0 {
        0
    } else {
        r
    }
}

/// Seconds since the UNIX epoch -> FAT/DOS packed date+time.
/// Returns 0 on success (and writes both words), -1 if `unix_s` is outside the
/// range a FAT directory entry can represent, in which case NOTHING is written:
/// the caller keeps whatever was there rather than stamping a lie.
#[no_mangle]
pub extern "C" fn ktime_unix_to_dos_rs(
    unix_s: i64,
    out_date: *mut u16,
    out_time: *mut u16,
) -> i32 {
    if out_date.is_null() || out_time.is_null() {
        return -1;
    }
    if unix_s <= 0 {
        return -1;
    }
    let days = unix_s.div_euclid(86_400);
    let sod = unix_s.rem_euclid(86_400);
    let (y, m, d) = civil_from_days(days);
    if y < KTIME_MIN_YEAR as i64 || y > KTIME_MAX_YEAR as i64 {
        return -1;
    }
    let hour = sod / 3600;
    let minute = (sod / 60) % 60;
    let second = sod % 60;
    let date = (((y - 1980) as u16) << 9) | ((m as u16) << 5) | (d as u16);
    let time = ((hour as u16) << 11) | ((minute as u16) << 5) | ((second as u16) / 2);
    // SAFETY: both pointers were null-checked above and the caller (C, fs/fat.c)
    // passes the addresses of two u16 locals.
    unsafe {
        *out_date = date;
        *out_time = time;
    }
    0
}

// ===========================================================================
// THE WALL CLOCK.
//
// This is the ONLY calendar clock in the kernel that a filesystem may stamp a
// file with. It is the CMOS RTC, read through gui/clock.c's existing accessors,
// converted here.
//
// IT IS DELIBERATELY NOT DERIVED FROM `timer_ticks`. Under KVM the PIT has tick
// reinjection on, so timer_ticks leaps ~1250 (a nominal five seconds) in ~15ms
// of real time when the vCPU is starved (#524/#525, rustkern/mono.rs). A
// timestamp derived from it would be wrong by seconds, unpredictably, exactly
// when the machine is busy. It is also NOT derived from sys_time(), which
// returns seconds since BOOT (#113) and is not calendar time at all.
//
// CACHE. The RTC is read at most once per second of monotonic time (mono_ms_rs,
// the TSC clock). A stat() never reads it at all - stat reports what is stored
// ON DISK - so this is only on the create/write path, but a bulk unpack writing
// thousands of files should not do 8 port I/Os per file either.
//
// TIMEZONE. The RTC is read AS UTC. If the firmware keeps local time the stored
// mtimes are offset by the local zone, uniformly; ordering is unaffected. This
// is stated rather than corrected because the kernel has no tz database (that
// lives in userland/libc/tz.c) and inventing an offset here would be a guess.
// ===========================================================================

static WC_LAST_UNIX: AtomicI64 = AtomicI64::new(0);
static WC_LAST_MONO_MS: AtomicI64 = AtomicI64::new(0);

extern "C" {
    fn mono_ms_rs() -> u64;
    // #113: microseconds, for the realtime clock below. Same TSC source as
    // mono_ms_rs, just not truncated to milliseconds.
    fn mono_us_rs() -> u64;
    // 1 once the TSC has been calibrated. The realtime clock must NOT anchor
    // before this: mono_us_rs() reads 0 until calibration, so an anchor taken
    // then would be `rtc - 0`, and the moment the TSC started counting the
    // extrapolation would run ahead by the whole elapsed time and force a
    // spurious STEP. Better to serve the plain RTC until there is a real
    // monotonic clock to anchor against.
    fn mono_ready_rs() -> i32;
}

/// Current UTC time as seconds since the UNIX epoch, or 0 if the RTC does not
/// present a plausible date. 0 means "the kernel does not know what time it
/// is", and every caller must treat it as a refusal to stamp.
#[no_mangle]
pub extern "C" fn wallclock_now_unix_rs() -> i64 {
    let now_ms = unsafe { mono_ms_rs() } as i64;
    let last_ms = WC_LAST_MONO_MS.load(Ordering::Relaxed);
    let cached = WC_LAST_UNIX.load(Ordering::Relaxed);
    // now_ms == 0 means the monotonic clock is not calibrated yet (very early
    // boot): fall through and read the RTC rather than trust an empty cache.
    if cached != 0 && now_ms != 0 && now_ms >= last_ms && (now_ms - last_ms) < 1000 {
        return cached;
    }

    let (mut h, mut mi, mut s): (i32, i32, i32) = (0, 0, 0);
    let (mut d, mut mo, mut y, mut wd): (i32, i32, i32, i32) = (0, 0, 0, 0);
    // SAFETY: both C functions write exactly the scalars pointed at and take no
    // ownership. They are bounded (see gui/clock.c) and take no lock.
    unsafe {
        rtc_read_time(&mut h, &mut mi, &mut s);
        rtc_read_date(&mut d, &mut mo, &mut y, &mut wd);
    }
    let t = ktime_civil_to_unix_rs(y, mo, d, h, mi, s);
    if t < 0 {
        return 0;
    }
    WC_LAST_UNIX.store(t, Ordering::Relaxed);
    WC_LAST_MONO_MS.store(now_ms, Ordering::Relaxed);
    t
}

// ===========================================================================
// BOOT SELF-TEST. Prints one [KTIME] line. Vectors are hand-computed from an
// independent source (not from this code), because a self-test that checks a
// function against itself proves nothing - that is the #433/#478 differential
// blindness lesson in this project's blame.md.
// ===========================================================================

/// Returns 0 if every vector passes, -1 otherwise. `out_checks` receives the
/// number of assertions actually executed, so a caller can tell "passed" from
/// "never ran".
#[no_mangle]
pub extern "C" fn ktime_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut n: u32 = 0;
    let mut ok = true;
    let mut chk = |cond: bool| {
        n += 1;
        if !cond {
            ok = false;
        }
    };

    // Absolute anchors, verified against the POSIX epoch definition.
    chk(ktime_civil_to_unix_rs(1980, 1, 1, 0, 0, 0) == 315_532_800);
    chk(ktime_civil_to_unix_rs(2000, 1, 1, 0, 0, 0) == 946_684_800);
    chk(ktime_civil_to_unix_rs(2026, 1, 1, 0, 0, 0) == 1_767_225_600);
    // 2026-08-13 12:34:56 UTC. 224 whole days into a non-leap 2026.
    chk(ktime_civil_to_unix_rs(2026, 8, 13, 12, 34, 56) == 1_786_624_496);
    // Leap-day handling, the exact case fs/exfat.c gets wrong.
    chk(ktime_civil_to_unix_rs(2024, 2, 29, 0, 0, 0) == 1_709_164_800);
    chk(ktime_civil_to_unix_rs(2024, 3, 1, 0, 0, 0) == 1_709_251_200);
    // 2100 is NOT a leap year (divisible by 100, not by 400).
    chk(ktime_civil_to_unix_rs(2100, 2, 29, 0, 0, 0) == -1);
    chk(ktime_civil_to_unix_rs(2096, 2, 29, 0, 0, 0) > 0);

    // Refusals, not plausible zeros.
    chk(ktime_civil_to_unix_rs(2026, 13, 1, 0, 0, 0) == -1);
    chk(ktime_civil_to_unix_rs(2026, 2, 30, 0, 0, 0) == -1);
    chk(ktime_civil_to_unix_rs(2026, 8, 13, 24, 0, 0) == -1);
    chk(ktime_civil_to_unix_rs(1970, 1, 1, 0, 0, 0) == -1); // below the window
    chk(ktime_civil_to_unix_rs(2026, 0, 1, 0, 0, 0) == -1);

    // DOS packing, computed by hand:
    //   date = ((2026-1980) << 9) | (8 << 5) | 13 = 23552 | 256 | 13 = 23821
    //   time = (12 << 11) | (34 << 5) | (56/2)    = 24576 | 1088 | 28 = 25692
    chk(ktime_dos_to_unix_rs(23821, 25692) == 1_786_624_496);
    chk(ktime_dos_to_unix_rs(0, 0) == 0); // unstamped entry -> "unknown"
    chk(ktime_dos_to_unix_rs(0xFFFF, 0xFFFF) == 0); // garbage -> "unknown"

    let mut dt: u16 = 0;
    let mut tm: u16 = 0;
    chk(ktime_unix_to_dos_rs(1_786_624_496, &mut dt, &mut tm) == 0);
    chk(dt == 23821);
    chk(tm == 25692);
    // Round trip over a spread of instants, including a leap day and both sides
    // of a month boundary. FAT stores seconds/2, so the round trip is exact
    // only on even seconds; every vector below is even.
    let vectors: [i64; 6] = [
        315_532_800,   // 1980-01-01 00:00:00, the FAT floor
        946_684_800,   // 2000-01-01
        1_709_164_800, // 2024-02-29
        1_709_251_200, // 2024-03-01
        1_786_624_496, // 2026-08-13 12:34:56
        4_102_444_800, // 2100-01-01
    ];
    let mut i = 0;
    while i < vectors.len() {
        let mut d2: u16 = 0;
        let mut t2: u16 = 0;
        chk(ktime_unix_to_dos_rs(vectors[i], &mut d2, &mut t2) == 0);
        chk(ktime_dos_to_unix_rs(d2, t2) == vectors[i]);
        i += 1;
    }
    // Out of FAT range: refuse and write nothing.
    let mut d3: u16 = 0xAAAA;
    let mut t3: u16 = 0x5555;
    chk(ktime_unix_to_dos_rs(0, &mut d3, &mut t3) == -1);
    chk(d3 == 0xAAAA && t3 == 0x5555);

    if !out_checks.is_null() {
        // SAFETY: null-checked; the caller passes the address of a u32 local.
        unsafe {
            *out_checks = n;
        }
    }
    if ok {
        0
    } else {
        -1
    }
}

// ===========================================================================
// #113: THE REALTIME CLOCK - epoch MICROSECONDS, sub-second, step-tolerant.
//
// WHAT WAS WRONG. sys_time()/SYS_TIME returned `timer_ticks / g_timer_hz`,
// which is SECONDS SINCE BOOT. Every userland time() and gettimeofday() was
// built on it, so the whole OS believed it was a few seconds past 1970-01-01.
//
// WHY NOT JUST CALL wallclock_now_unix_rs() PER CALL. Two reasons:
//   1. It has SECOND resolution. gettimeofday() must advance sub-second or
//      every derived millisecond clock quantises to 1000ms steps.
//   2. It is 8 port I/Os. A per-call RTC read on a hot syscall is absurd.
//
// THE SHAPE. One anchor, held as a SINGLE i64:
//
//      RT_OFFSET_US = epoch_us - mono_us       (so epoch_us = OFFSET + mono_us)
//
// Storing the anchor as one atomic rather than an (epoch, mono) PAIR is not a
// micro-optimisation, it is a correctness requirement: a pair can TEAR between
// two racing CPUs (read the epoch from one update and the mono from another)
// and produce a timestamp that belongs to no instant at all. One atomic cannot
// tear, and re-anchoring is a single store.
//
// mono_us_rs() is the TSC clock, NOT timer_ticks. timer_ticks is not a wall
// clock: under KVM the PIT replays a starved vCPU's lost IRQs in BURSTS, so a
// tick-derived clock leaps seconds in milliseconds (#524/#525, blame.md).
//
// MONOTONICITY. Between steps the value is anchor + a monotonic counter, so it
// is monotonic BY CONSTRUCTION, not by a max() patch over a wobbly source.
//
// SNTP. net/sntp.c corrects the clock by WRITING THE RTC (sys_set_rtc_time /
// sys_set_rtc_date). We do not need a second notification path for that, and
// deliberately do not add one: the throttled RTC poll below SEES the step
// within RT_POLL_US and re-anchors. The monotonic clock is never touched, so
// mono_us() does not jump when the wall clock does. That is the whole reason
// the anchor is an OFFSET rather than a captured-once constant.
//
// THE ANCHOR IS INITIALLY UP TO ONE SECOND EARLY, THEN SELF-CORRECTS. The RTC
// reports whole seconds, so anchoring at an arbitrary moment inside second N
// places us anywhere in [N, N+1). Rather than busy-wait for a rollover at boot
// (a bounded poll would still cost up to a second of boot time, and #426 is
// emphatic about polls), the first observed second ROLLOVER re-anchors exactly:
// after that the error is bounded by RT_POLL_US, not by one second. This is
// stated because "accurate to a second" and "accurate to 200ms" are different
// claims and the code should not let a reader assume the better one.
// ===========================================================================

const RT_POLL_US: i64 = 200_000;      // how often we may touch the RTC
const RT_STEP_US: i64 = 2_000_000;    // disagreement that counts as a STEP

static RT_OFFSET_US:   AtomicI64 = AtomicI64::new(0);
static RT_HAVE:        AtomicI64 = AtomicI64::new(0);
static RT_LAST_POLL:   AtomicI64 = AtomicI64::new(0);
static RT_LAST_RTC_S:  AtomicI64 = AtomicI64::new(0);
static RT_REFINED:     AtomicI64 = AtomicI64::new(0);
static RT_STEPS:       AtomicI64 = AtomicI64::new(0);

// What to do with the anchor given a fresh RTC reading. PURE, and separated
// from the I/O precisely so the self-test can drive it through every branch
// including the ones a booted machine will not reach for days. A decision
// buried inside the polling function would be untestable, and an untestable
// guard is indistinguishable from an absent one.
//
//   0 KEEP    the anchor still agrees with the RTC; do not disturb it.
//   1 STEP    the RTC disagrees by >= RT_STEP_US (SNTP landed, or a human set
//             the clock). Re-anchor and go back to unrefined.
//   2 REFINE  we just watched the RTC seconds field roll over, and we have not
//             yet refined. Re-anchor exactly on that boundary.
pub fn rt_decide(have: bool, refined: bool, extrapolated_us: i64,
                 rtc_sec: i64, last_rtc_sec: i64) -> i32 {
    if !have || rtc_sec <= 0 {
        return if rtc_sec > 0 { 1 } else { 0 };
    }
    let rtc_us = rtc_sec * 1_000_000;
    let diff = if rtc_us > extrapolated_us { rtc_us - extrapolated_us }
               else { extrapolated_us - rtc_us };
    if diff >= RT_STEP_US {
        return 1;
    }
    if !refined && rtc_sec != last_rtc_sec {
        return 2;
    }
    0
}

/// Current UTC time as MICROSECONDS since the UNIX epoch, or 0 if the RTC has
/// never presented a plausible date. 0 is a REFUSAL, not 1970: a caller that
/// needs to distinguish "no clock" from "the epoch" can, and must.
#[no_mangle]
pub extern "C" fn realtime_us_rs() -> i64 {
    // SAFETY: both are pure leaf readers of the TSC clock state.
    let (ready, mono) = unsafe { (mono_ready_rs() != 0, mono_us_rs() as i64) };

    // Before the TSC is calibrated there is nothing to anchor AGAINST, so serve
    // the RTC directly at whole-second resolution and do not poison the anchor.
    // This is a real boot window: mono_init() runs after the timer is up, and
    // anything asking the time before that would otherwise install an anchor
    // that is wrong by the entire pre-calibration interval.
    if !ready {
        let s = wallclock_now_unix_rs();
        return if s > 0 { s * 1_000_000 } else { 0 };
    }

    let have = RT_HAVE.load(Ordering::Relaxed) != 0;
    let mut est = if have { RT_OFFSET_US.load(Ordering::Relaxed) + mono } else { 0 };

    // Throttle the RTC. `mono < last_poll` cannot happen with a monotonic
    // source, but it is cheaper to tolerate than to assume.
    let last_poll = RT_LAST_POLL.load(Ordering::Relaxed);
    let due = !have || mono < last_poll || (mono - last_poll) >= RT_POLL_US;
    if !due {
        return est;
    }
    RT_LAST_POLL.store(mono, Ordering::Relaxed);

    let rtc_sec = wallclock_now_unix_rs();          // 0 if the RTC is not sane
    let refined = RT_REFINED.load(Ordering::Relaxed) != 0;
    let last_s  = RT_LAST_RTC_S.load(Ordering::Relaxed);
    let action  = rt_decide(have, refined, est, rtc_sec, last_s);

    if rtc_sec > 0 {
        RT_LAST_RTC_S.store(rtc_sec, Ordering::Relaxed);
    }
    match action {
        1 => {
            // STEP (or first anchor). Re-anchor on the RTC's second value.
            RT_OFFSET_US.store(rtc_sec * 1_000_000 - mono, Ordering::Relaxed);
            RT_HAVE.store(1, Ordering::Relaxed);
            RT_REFINED.store(0, Ordering::Relaxed);
            if have { RT_STEPS.fetch_add(1, Ordering::Relaxed); }
            est = rtc_sec * 1_000_000;
        }
        2 => {
            // REFINE. We are within RT_POLL_US of a real second boundary, so
            // anchoring here removes the up-to-one-second startup error.
            RT_OFFSET_US.store(rtc_sec * 1_000_000 - mono, Ordering::Relaxed);
            RT_REFINED.store(1, Ordering::Relaxed);
            est = rtc_sec * 1_000_000;
        }
        _ => {}
    }
    est
}

/// Seconds since the UNIX epoch, from the same anchor as realtime_us_rs(), so
/// SYS_TIME and gettimeofday() can never disagree about which second it is.
/// That disagreement is exactly what #594 had to fix once already, when the two
/// fields of gettimeofday() came from two different counters.
#[no_mangle]
pub extern "C" fn realtime_sec_rs() -> i64 {
    let us = realtime_us_rs();
    if us <= 0 { 0 } else { us / 1_000_000 }
}

/// How many STEPS the clock has taken since boot (SNTP corrections and manual
/// sets). Exposed so the boot/diagnostic line can show that step detection is
/// a thing that actually happens, rather than a branch nobody has ever seen run.
#[no_mangle]
pub extern "C" fn realtime_steps_rs() -> i64 { RT_STEPS.load(Ordering::Relaxed) }

/// Epoch seconds -> civil UTC fields, plus the day of week (0 = Sunday).
/// The inverse of ktime_civil_to_unix_rs, exported because #86's login clock
/// must render a LOCAL date: adding a timezone offset to an epoch can cross a
/// midnight, so the date under the clock has to be recomputed from the shifted
/// instant, not just the hour patched. Returns 0 on success, -1 (writing
/// nothing) if the instant is outside the plausibility window.
#[no_mangle]
pub extern "C" fn ktime_unix_to_civil_rs(unix_s: i64, out: *mut i32) -> i32 {
    if out.is_null() { return -1; }
    let days = unix_s.div_euclid(86_400);
    let sod  = unix_s.rem_euclid(86_400);
    let (y, m, d) = civil_from_days(days);
    if y < KTIME_MIN_YEAR as i64 || y > KTIME_MAX_YEAR as i64 { return -1; }
    // 1970-01-01 was a Thursday (4). Euclidean rem keeps this correct for
    // negative day counts too, which a plain % would not.
    let wd = (days + 4).rem_euclid(7);
    // SAFETY: null-checked above; the caller passes a >= 7 element i32 array.
    unsafe {
        *out.offset(0) = y as i32;
        *out.offset(1) = m as i32;
        *out.offset(2) = d as i32;
        *out.offset(3) = (sod / 3600) as i32;
        *out.offset(4) = ((sod / 60) % 60) as i32;
        *out.offset(5) = (sod % 60) as i32;
        *out.offset(6) = wd as i32;
    }
    0
}

/// Self-test for the #113 realtime clock's DECISION LOGIC and the new
/// epoch -> civil converter. Separate from ktime_selftest_rs because this one
/// must be provable RED: `make RTCLKTESTFAIL=1` feeds rt_decide a deliberately
/// wrong expectation so the boot line is seen to say FAIL on a machine that is
/// otherwise fine. A check nobody has watched fail is not evidence (#514/#665).
#[no_mangle]
pub extern "C" fn rtclock_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut n: u32 = 0;
    let mut ok = true;
    let mut chk = |cond: bool| { n += 1; if !cond { ok = false; } };

    const T: i64 = 1_786_624_496;          // 2026-08-13 12:34:56 UTC
    const TUS: i64 = T * 1_000_000;

    // --- rt_decide, every branch, both directions ---------------------------
    // No anchor yet + a sane RTC -> take one (STEP=1).
    chk(rt_decide(false, false, 0, T, 0) == 1);
    // No anchor and no RTC -> stay ignorant rather than anchor on garbage.
    chk(rt_decide(false, false, 0, 0, 0) == 0);
    chk(rt_decide(true,  true,  TUS, 0, T) == 0);
    // Anchored, refined, RTC agrees -> KEEP. This is the common case, and it
    // is the one that must NOT re-anchor: re-anchoring every poll would quantise
    // the clock back to whole seconds and undo the whole point.
    chk(rt_decide(true, true, TUS + 400_000, T, T) == 0);
    // Agrees to within just under the step threshold -> still KEEP.
    chk(rt_decide(true, true, TUS, T + 1, T + 1) == 0);
    // SNTP steps the RTC forward / backward by more than RT_STEP_US -> STEP.
    chk(rt_decide(true, true, TUS, T + 3600, T) == 1);
    chk(rt_decide(true, true, TUS, T - 3600, T) == 1);
    // Exactly at the threshold counts as a step (>=, not >).
    chk(rt_decide(true, true, TUS, T + 2, T) == 1);
    // Not yet refined and the seconds field just rolled over -> REFINE.
    chk(rt_decide(true, false, TUS + 500_000, T + 1, T) == 2);
    // Not yet refined but the RTC has NOT rolled over -> nothing to refine on.
    chk(rt_decide(true, false, TUS + 500_000, T, T) == 0);
    // Already refined: a rollover is normal and must NOT re-anchor, or the
    // clock would step every single second.
    chk(rt_decide(true, true, TUS + 500_000, T + 1, T) == 0);

    // --- epoch -> civil, against hand-computed vectors -----------------------
    let mut c = [0i32; 7];
    chk(ktime_unix_to_civil_rs(T, c.as_mut_ptr()) == 0);
    chk(c[0] == 2026 && c[1] == 8 && c[2] == 13);
    chk(c[3] == 12 && c[4] == 34 && c[5] == 56);
    chk(c[6] == 4);                        // 2026-08-13 was a Thursday
    // Round trip against the FORWARD converter, which was itself proven against
    // independent anchors in ktime_selftest_rs.
    chk(ktime_civil_to_unix_rs(c[0], c[1], c[2], c[3], c[4], c[5]) == T);
    // A leap day, and the epoch-crossing behaviour of the day-of-week math.
    chk(ktime_unix_to_civil_rs(1_709_164_800, c.as_mut_ptr()) == 0);
    chk(c[0] == 2024 && c[1] == 2 && c[2] == 29 && c[6] == 4);   // Thursday
    chk(ktime_unix_to_civil_rs(946_684_800, c.as_mut_ptr()) == 0);
    chk(c[0] == 2000 && c[1] == 1 && c[2] == 1 && c[6] == 6);    // Saturday
    // Out of window -> refuse and write NOTHING.
    let mut g = [0x7Fi32; 7];
    chk(ktime_unix_to_civil_rs(0, g.as_mut_ptr()) == -1);
    chk(g[0] == 0x7F && g[6] == 0x7F);
    chk(ktime_unix_to_civil_rs(-1, g.as_mut_ptr()) == -1);

    // --- #234a the DOS/BIOS clock view -------------------------------------
    // The regression these guard is not "the arithmetic is off by a minute",
    // it is "every field is a constant". A DOS guest that seeds its generator
    // from CX+DX of INT 21h AH=2Ch (Epyx Rogue does) gets a degenerate seed
    // from a frozen clock and hangs, so CONSTANCY is the property to disprove.
    let mut k = KDosClock { hour: 0, minute: 0, second: 0, hundredth: 0,
                            weekday: 0, month: 0, day: 0, known: 0,
                            year: 0, _pad: 0, ticks: 0 };
    // Known clock: 2026-08-13 12:34:56.25 UTC (the same instant the converter
    // vectors above are anchored on), so a fault here is in the DOS view, not
    // in the calendar underneath it.
    chk(ktime_dos_clock_rs(T * 1_000_000 + 250_000, 0, &mut k) == 0);
    chk(k.hour == 12 && k.minute == 34 && k.second == 56 && k.hundredth == 25);
    chk(k.year == 2026 && k.month == 8 && k.day == 13 && k.weekday == 4);
    chk(k.known == 1);
    chk(k.ticks == 824_685);            // 45296.25 s * 1573040 / 86400, floored
    // Midnight, noon and one second, hand-computed from KDOS_TICKS_PER_DAY.
    chk(ktime_dos_clock_rs((T - 45_296) * 1_000_000, 0, &mut k) == 0);
    chk(k.hour == 0 && k.minute == 0 && k.second == 0 && k.ticks == 0);
    chk(ktime_dos_clock_rs((T - 45_296 + 43_200) * 1_000_000, 0, &mut k) == 0);
    chk(k.hour == 12 && k.ticks == 786_520);
    chk(ktime_dos_clock_rs((T - 45_296 + 1) * 1_000_000, 0, &mut k) == 0);
    chk(k.ticks == 18);
    // The last hundredth of the day must stay INSIDE the rollover constant, or
    // the counter jumps backwards past midnight instead of wrapping at it.
    chk(ktime_dos_clock_rs((T - 45_296 + 86_400) * 1_000_000 - 10_000, 0, &mut k) == 0);
    chk(k.ticks == KDOS_TICKS_PER_DAY - 1 && k.hour == 23 && k.minute == 59);
    // NO RTC: the fallback must still MOVE. This is the exact regression - the
    // old code answered zero here for ever.
    chk(ktime_dos_clock_rs(0, 0, &mut k) == 0);
    chk(k.known == 0 && k.year == 1980 && k.month == 1 && k.day == 1);
    chk(k.hour == 0 && k.ticks == 0);
    chk(ktime_dos_clock_rs(0, 3_600_000_000, &mut k) == 0);
    chk(k.hour == 1 && k.ticks == 65_543 && k.known == 0);
    chk(ktime_dos_clock_rs(0, 1_000_000, &mut k) == 0);
    let t1 = k.ticks;
    chk(ktime_dos_clock_rs(0, 2_000_000, &mut k) == 0);
    chk(k.ticks != t1);                 // NOT A CONSTANT. The whole point.
    // Two days of uptime rolls the fallback date, so a guest that reads the
    // date does not see 1980-01-01 for ever either.
    chk(ktime_dos_clock_rs(0, 2 * 86_400_000_000 + 5_000_000, &mut k) == 0);
    chk(k.year == 1980 && k.month == 1 && k.day == 3 && k.second == 5);
    // A negative / absent realtime must take the fallback, never index a
    // calendar with it.
    chk(ktime_dos_clock_rs(-1, 0, &mut k) == 0 && k.known == 0);

    // The DELIBERATE failure, so this line has been watched to go RED on a
    // machine that is otherwise healthy. Without it, PASS is an untested claim.
    #[cfg(rtclk_test_fail)]
    chk(rt_decide(true, true, TUS + 400_000, T, T) == 1);   // wrong on purpose

    if !out_checks.is_null() {
        // SAFETY: null-checked; caller passes the address of a u32 local.
        unsafe { *out_checks = n; }
    }
    if ok { 0 } else { -1 }
}

// ===========================================================================
// #234a: THE PC/DOS VIEW OF THE CLOCK, in one place.
//
// WHY THIS IS HERE AND NOT IN THE DOS LAYER.
//
// Every clock service a DOS guest could ask for answered a COMPILE-TIME
// CONSTANT:
//
//   dos/int21svc.c  AH=2Ch  get system time -> CX=0 DX=0      (midnight, always)
//   dos/int21svc.c  AH=2Ah  get system date -> 19 Nov 1992    (frozen)
//   dos/dosexec.c   INT 1Ah AH=00 tick count -> CX=0 DX=0     (zero, always)
//
// That is not a cosmetic gap. MEASURED on Epyx Rogue (/DOS/ROGUE/ROGUE.EXE,
// golden 2053): it seeds its generator with CX+DX from INT 21h AH=2Ch, so the
// seed was 0, and its generator is seed = (seed * 125) mod 2796203, for which
// ZERO IS A FIXED POINT. Every rnd() returned 0 forever, dungeon generation is
// a rejection loop over rnd(), and the game wedged in the C runtime's 32-bit
// divide helper with a blank screen after its title. The user-visible symptom
// was "pressing a key does nothing" and the keyboard was never involved.
//
// The three services above live in two files and are reached by three guest
// kinds (real-mode DOS, DOS/4GW + go32, Win16), so the constant had to stop
// being expressible ONCE rather than be patched three times. This is that one
// place: the callers do no arithmetic, they read fields.
//
// RUST, per the standing directive: pure integer arithmetic, no paging, no asm,
// no FPU, and it sits with the calendar converter it is built on rather than
// growing a private copy of it.
// ===========================================================================

/// The BIOS midnight rollover value. The 8254 runs at 1_193_182 Hz and the BIOS
/// counts one tick per 65536 of them, so a day is
/// 86400 * 1193182 / 65536 = 1_573_040 = 0x1800B0 ticks. That is the value real
/// BIOS code compares against, not an approximation of "18.2 Hz".
pub const KDOS_TICKS_PER_DAY: u32 = 0x0018_00B0;

#[repr(C)]
pub struct KDosClock {
    pub hour: u8,       // 0..23     INT 21h AH=2Ch CH
    pub minute: u8,     // 0..59     INT 21h AH=2Ch CL
    pub second: u8,     // 0..59     INT 21h AH=2Ch DH
    pub hundredth: u8,  // 0..99     INT 21h AH=2Ch DL
    pub weekday: u8,    // 0=Sunday  INT 21h AH=2Ah AL
    pub month: u8,      // 1..12     INT 21h AH=2Ah DH
    pub day: u8,        // 1..31     INT 21h AH=2Ah DL
    pub known: u8,      // 1 = from the RTC, 0 = derived from uptime alone
    pub year: u16,      //           INT 21h AH=2Ah CX
    pub _pad: u16,
    pub ticks: u32,     // 0..KDOS_TICKS_PER_DAY-1   INT 1Ah AH=00 CX:DX
}

/// Fill `out` with the DOS/BIOS view of "now".
///
///   realtime_us  epoch microseconds, or <= 0 when the RTC does not know
///                (cpu/wallclock.h realtime_us_rs(), which refuses rather than
///                inventing 1970).
///   uptime_us    microseconds since boot (cpu/mono.h mono_us()). Consulted
///                ONLY when the RTC is unknown.
///
/// Returns 0 on success, -1 if `out` is null.
///
/// THE FALLBACK IS NOT A PLACEHOLDER, IT IS WHAT THE HARDWARE DOES. A PC with
/// no CMOS clock starts at 1980-01-01 00:00:00 and counts forward from the BIOS
/// tick; DOS shows exactly that until someone types a date. The property every
/// caller here depends on is that the value MOVES, and this one does. The
/// previous behaviour was a constant, which is the one thing a clock must not
/// be.
#[no_mangle]
pub extern "C" fn ktime_dos_clock_rs(realtime_us: i64, uptime_us: u64,
                                     out: *mut KDosClock) -> i32 {
    if out.is_null() { return -1; }
    let mut c = [0i32; 7];
    let known = realtime_us > 0
        && ktime_unix_to_civil_rs(realtime_us / 1_000_000, c.as_mut_ptr()) == 0;
    let us_of_day: i64 = if known {
        (c[3] as i64) * 3_600_000_000
            + (c[4] as i64) * 60_000_000
            + (c[5] as i64) * 1_000_000
            + realtime_us.rem_euclid(1_000_000)
    } else {
        let days = (uptime_us / 86_400_000_000) as i64;
        let z = days_from_civil(1980, 1, 1) + days;
        let (y, m, d) = civil_from_days(z);
        c[0] = y as i32;
        c[1] = m as i32;
        c[2] = d as i32;
        c[6] = (z + 4).rem_euclid(7) as i32;   // 1970-01-01 was a Thursday
        (uptime_us % 86_400_000_000) as i64
    };
    // The tick count is DEFINED as an exact fraction of KDOS_TICKS_PER_DAY
    // rather than as us * 1193182 / 65536, on purpose. Those two disagree by two
    // ticks a day (the real BIOS's own rounding), and using the second one while
    // rolling over on the first would put a 0.1 s discontinuity at midnight in a
    // counter whose whole job is to be smooth. Deriving both from ONE constant
    // makes that mismatch inexpressible; the resulting rate is 18.2064815 Hz
    // against a true 18.2064819, an error of 2e-8.
    // 86_400_000_000 * 1_573_040 is 1.36e17, well inside i64.
    let ticks = (us_of_day * KDOS_TICKS_PER_DAY as i64) / 86_400_000_000;
    // SAFETY: null-checked above. Every field is range-reduced here, so no
    // caller can be handed an hour of 24 or a month of 0.
    unsafe {
        (*out).hour      = (us_of_day / 3_600_000_000 % 24) as u8;
        (*out).minute    = (us_of_day / 60_000_000 % 60) as u8;
        (*out).second    = (us_of_day / 1_000_000 % 60) as u8;
        (*out).hundredth = (us_of_day / 10_000 % 100) as u8;
        (*out).weekday   = (c[6].rem_euclid(7)) as u8;
        (*out).month     = c[1] as u8;
        (*out).day       = c[2] as u8;
        (*out).known     = if known { 1 } else { 0 };
        (*out).year      = c[0] as u16;
        (*out)._pad      = 0;
        (*out).ticks     = (ticks as u32) % KDOS_TICKS_PER_DAY;
    }
    0
}
