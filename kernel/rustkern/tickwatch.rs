// rustkern/tickwatch.rs - #745 (#62): periodic-tick health verdict and the
// tick-failover decision, as pure functions.
//
// WHY THIS EXISTS
// ---------------
// On the real iMac14,4 the owner reports that the desktop does not advance
// unless something is running: "if i stop moving the mouse the timers all hang!
// uptime stops counting, system monitor stops updating". Every user-visible
// timer in this system is downstream of exactly ONE interrupt: the legacy 8254
// PIT on IRQ0, vector 32, EOI'd through the 8259 pair (cpu/pic.c `pit_init`,
// cpu/isr.c `timer_handler`). If that one interrupt is not delivered then
// `sched_tick()` never runs, no sleeping thread is ever woken on time, and the
// machine only steps forward when some OTHER interrupt happens to drag the
// scheduler along with it. That is a single point of failure for the entire
// notion of time on this OS, and it is invisible in a VM because QEMU always
// delivers.
//
// THE MEASUREMENT MUST NOT USE THE CLOCK UNDER TEST
// -------------------------------------------------
// `timer_ticks` is the counter being judged, so it can never also be the
// reference. blame.md's standing rule - "timer_ticks is NOT a wall clock, KVM
// replays lost ticks in BURSTS" - is exactly why every function here takes the
// elapsed REAL time as a separate argument. The caller supplies it from
// cpu/mono.h (`mono_ms()`), which is TSC-backed and calibrated once at boot,
// and therefore cannot inherit the defect it is measuring. A verdict computed
// from ticks alone would be circular and would report a healthy machine no
// matter what the hardware did.
//
// WHY THE DECISIONS ARE HERE AND THE REGISTERS ARE NOT
// ----------------------------------------------------
// This module owns the DECISIONS, in Rust, per the all-new-kernel-code-in-Rust
// rule. The port/MMIO reads, the IDT entry and the mutable state live in
// cpu/tickwatch.c because they are `in`/`out` instructions, memory-mapped APIC
// access and interrupt-table surgery. That is the same split, for the same
// reason, as rustkern/schedwatch.rs and rustkern/sched_age.rs, both of which
// are pure decision functions over caller-built snapshots.
//
// Integer-only throughout: the kernel target is soft-float with SSE disabled
// (`x86_64-unknown-none`, CFLAGS `-mno-sse -mno-sse2`), so a rate is carried as
// a scaled integer (per-mille of nominal) and never as a float.

// ---------------------------------------------------------------------------
// Verdicts
// ---------------------------------------------------------------------------

/// The window was too short, or the caller had no usable reference clock yet,
/// to say anything. Deliberately distinct from `TICK_OK`: "not measured" and
/// "measured healthy" must never collapse to the same value, which is the
/// exact trap blame.md records from this ticket's own previous pass ("a unit
/// choice made HEALTHY and NEVER MEASURED the same value").
pub const TICK_UNKNOWN: i32 = 0;
/// Ticks arrived at approximately the programmed rate.
pub const TICK_OK: i32 = 1;
/// Ticks arrived, but at well under the programmed rate. Time advances, slowly
/// and unevenly. Different bug from TICK_DEAD, different fix, and they look
/// identical from the desktop.
pub const TICK_SLOW: i32 = 2;
/// NOT ONE tick arrived across a window long enough to have contained many.
/// This is the iMac hypothesis.
pub const TICK_DEAD: i32 = 3;
/// Far MORE ticks arrived than real time can account for: the KVM reinjection
/// burst already documented in cpu/isr.c `tickburst_sample`. Reported
/// separately so a VM artefact is never mistaken for the hardware fault.
pub const TICK_BURST: i32 = 4;

/// Shortest window this will judge, in real milliseconds. At the default
/// 250 Hz a 200 ms window nominally contains 50 ticks, so "zero" is a real
/// statement rather than a sampling accident.
pub const TICK_MIN_WINDOW_MS: u64 = 200;

/// Rate at or below this fraction of nominal (per-mille) is SLOW.
const SLOW_PERMILLE: u64 = 500;
/// Rate at or above this fraction of nominal (per-mille) is a BURST.
const BURST_PERMILLE: u64 = 2000;

/// Measured tick rate as per-mille of the programmed rate.
///
/// 1000 means "exactly the programmed rate". Returns 0 when the window is
/// unusable, which the caller must distinguish from a real measured 0 by
/// checking the verdict rather than this number.
///
/// `dticks` ticks counted across the window (any source).
/// `dms`    REAL elapsed milliseconds across the same window, from mono_ms().
/// `hz`     the rate the timer was PROGRAMMED for.
#[no_mangle]
pub extern "C" fn tick_permille_rs(dticks: u64, dms: u64, hz: u32) -> u64 {
    if hz == 0 || dms == 0 {
        return 0;
    }
    // expected = hz * dms / 1000, computed so a 2 s window at 250 Hz cannot
    // overflow and a sub-second window does not truncate to zero prematurely.
    let expected = (hz as u64).saturating_mul(dms) / 1000;
    if expected == 0 {
        return 0;
    }
    dticks.saturating_mul(1000) / expected
}

/// Judge one measurement window.
///
/// `dticks` ticks counted across the window (any source).
/// `dms`    REAL elapsed milliseconds across the same window, from mono_ms().
///          MUST NOT be derived from timer_ticks.
/// `hz`     the rate the timer was PROGRAMMED for.
#[no_mangle]
pub extern "C" fn tick_health_verdict_rs(dticks: u64, dms: u64, hz: u32) -> i32 {
    if hz == 0 || dms < TICK_MIN_WINDOW_MS {
        return TICK_UNKNOWN;
    }
    let expected = (hz as u64).saturating_mul(dms) / 1000;
    if expected == 0 {
        return TICK_UNKNOWN;
    }
    if dticks == 0 {
        // A window of at least TICK_MIN_WINDOW_MS of real time that contained
        // no tick at all. Nothing about a correctly programmed periodic timer
        // can produce this.
        return TICK_DEAD;
    }
    let pm = dticks.saturating_mul(1000) / expected;
    if pm < SLOW_PERMILLE {
        TICK_SLOW
    } else if pm >= BURST_PERMILLE {
        TICK_BURST
    } else {
        TICK_OK
    }
}

// ---------------------------------------------------------------------------
// Failover decision
// ---------------------------------------------------------------------------

/// How many nominal tick periods of silence from the NATIVE source before the
/// redundant source considers itself in charge. Three, so that one late tick
/// (a long interrupts-off section, a slow ext2 write) is absorbed rather than
/// double-counted, while a genuinely dead source is picked up within ~12 ms at
/// the default 250 Hz.
pub const TICK_NATIVE_ALIVE_PERIODS: u64 = 3;

/// THE HOT-PATH DECISION: how many ticks should the redundant source
/// synthesise right now?
///
/// This is the whole of the structural fix in one function. The Local APIC
/// timer is armed from boot and fires unconditionally, which is the project's
/// preferred shape - a REDUNDANT, ALWAYS-ARMED wake source, so no wake can
/// ever be lost (CLAUDE.md's preference order; the worked example is the PCM
/// pump, woken from both the MSI ISR and a 10 ms poll worker). It is NOT armed
/// on demand by a watchdog, because any watchdog capable of noticing a dead
/// tick would itself have to be scheduled, and scheduling is downstream of the
/// tick. A watchdog for the clock cannot be clocked by the clock.
///
/// On a healthy machine this returns 0 on every call and the redundant
/// interrupt costs one comparison and an EOI. When the native tick stops, it
/// returns the number of ticks REAL TIME says have elapsed, so `timer_ticks`
/// keeps approximately correct time instead of running at the redundant
/// source's own slower rate.
///
/// `now_us`          current monotonic microseconds (TSC-backed).
/// `last_native_us`  when the NATIVE source last fired; 0 if never/unknown.
/// `last_synth_us`   when this function last authorised a synthesised tick.
/// `period_us`       one nominal tick period.
/// `max_catchup`     hard bound on ticks returned in one call.
///
/// THE BOUND IS NOT OPTIONAL. Returning an unbounded catch-up would recreate,
/// by our own hand, the exact defect blame.md records as the reason nothing in
/// this kernel may use ticks as a clock: a burst of ticks delivered in no real
/// time, collapsing every `timer_ticks + N` deadline at once. Capping it means
/// a long stall is recovered from over several interrupts rather than in one
/// destructive jump.
#[no_mangle]
pub extern "C" fn tick_synth_decide_rs(
    now_us: u64,
    last_native_us: u64,
    last_synth_us: u64,
    period_us: u64,
    max_catchup: u64,
) -> u64 {
    if period_us == 0 || max_catchup == 0 {
        return 0;
    }
    // Is the native source alive? If it fired within the last few periods it
    // is doing its job and this source must stay out of the way, or every tick
    // would be counted twice and the whole system would run at double speed.
    if last_native_us != 0
        && now_us >= last_native_us
        && (now_us - last_native_us) < period_us.saturating_mul(TICK_NATIVE_ALIVE_PERIODS)
    {
        return 0;
    }
    // Not yet seeded, or the clock went backwards (it must not, but a wrong
    // answer here would be a tick storm, so refuse rather than trust).
    if last_synth_us == 0 || now_us <= last_synth_us {
        return 0;
    }
    let due = (now_us - last_synth_us) / period_us;
    if due == 0 {
        return 0;
    }
    if due > max_catchup {
        max_catchup
    } else {
        due
    }
}

// ---------------------------------------------------------------------------
// Self-tests. Compiled into the kernel and run at boot, in the manner of
// sched_watch_selftest_rs(). Returns the number of FAILURES, so 0 is a pass.
//
// Every case below is a shape that has to behave a specific way for the
// instrument to be trustworthy; in particular the "healthy" and "never
// measured" cases must return DIFFERENT values, because collapsing those two
// is the documented way this ticket's previous pass produced a number that was
// not a measurement.
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn tick_watch_selftest_rs() -> u32 {
    let mut fails: u32 = 0;
    let mut check = |cond: bool| {
        if !cond {
            fails += 1;
        }
    };

    // --- verdicts -----------------------------------------------------------
    // Healthy: 250 Hz, 2000 ms window, 500 ticks.
    check(tick_health_verdict_rs(500, 2000, 250) == TICK_OK);
    // Dead: same window, zero ticks. THE iMac HYPOTHESIS.
    check(tick_health_verdict_rs(0, 2000, 250) == TICK_DEAD);
    // Window too short to judge: must be UNKNOWN, never OK and never DEAD.
    check(tick_health_verdict_rs(0, 10, 250) == TICK_UNKNOWN);
    // Slow: a quarter of nominal.
    check(tick_health_verdict_rs(125, 2000, 250) == TICK_SLOW);
    // Just above the slow floor is OK, just below is SLOW: prove the boundary
    // is where it is claimed rather than approximately there.
    check(tick_health_verdict_rs(260, 2000, 250) == TICK_OK);
    check(tick_health_verdict_rs(240, 2000, 250) == TICK_SLOW);
    // Burst: KVM reinjection, ten times nominal. Must NOT read as OK and must
    // NOT read as DEAD.
    check(tick_health_verdict_rs(5000, 2000, 250) == TICK_BURST);
    // hz==0 is a caller bug, not a dead clock.
    check(tick_health_verdict_rs(500, 2000, 0) == TICK_UNKNOWN);
    // A healthy reading and a never-measured reading must differ.
    check(tick_health_verdict_rs(500, 2000, 250) != tick_health_verdict_rs(0, 10, 250));

    // --- rate ---------------------------------------------------------------
    check(tick_permille_rs(500, 2000, 250) == 1000);
    check(tick_permille_rs(250, 2000, 250) == 500);
    check(tick_permille_rs(0, 2000, 250) == 0);
    check(tick_permille_rs(500, 0, 250) == 0);

    // --- redundant-source synthesis ----------------------------------------
    // period 4000us (250 Hz). Native fired 1000us ago: alive, synthesise none.
    check(tick_synth_decide_rs(100_000, 99_000, 50_000, 4000, 64) == 0);
    // Native fired 4000us ago (one period): still within the 3-period grace.
    check(tick_synth_decide_rs(100_000, 96_000, 50_000, 4000, 64) == 0);
    // Native silent for 20000us (5 periods) and 40000us since the last synth:
    // 10 ticks are due.
    check(tick_synth_decide_rs(100_000, 80_000, 60_000, 4000, 64) == 10);
    // The catch-up bound is honoured: 10 due, cap 4 -> 4.
    check(tick_synth_decide_rs(100_000, 80_000, 60_000, 4000, 4) == 4);
    // Native never seen (0) and a synth baseline exists: take over.
    check(tick_synth_decide_rs(100_000, 0, 60_000, 4000, 64) == 10);
    // No synth baseline yet: refuse, do not jump from zero.
    check(tick_synth_decide_rs(100_000, 0, 0, 4000, 64) == 0);
    // Clock apparently went backwards: refuse rather than emit a storm.
    check(tick_synth_decide_rs(50_000, 0, 60_000, 4000, 64) == 0);
    // Degenerate arguments must be inert, not divide by zero.
    check(tick_synth_decide_rs(100_000, 0, 60_000, 0, 64) == 0);
    check(tick_synth_decide_rs(100_000, 0, 60_000, 4000, 0) == 0);
    // Less than one period since the last synth: nothing due yet.
    check(tick_synth_decide_rs(61_000, 0, 60_000, 4000, 64) == 0);

    fails
}
