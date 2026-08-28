// rustkern/mono.rs - #525 the shared TSC monotonic clock (C API in cpu/mono.h)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// =============================================================================
// #525: THE SHARED MONOTONIC CLOCK. One clock, one place, for the whole kernel.
// =============================================================================
//
// WHY THIS EXISTS (read before adding any deadline anywhere in this kernel).
//
// `timer_ticks` counts ticks DELIVERED, not time ELAPSED. Under KVM the PIT is
// left with tick REINJECTION enabled (the `kvm-pit.lost_tick_policy="delay"`
// default), so when the vCPU is starved the missed ticks are re-delivered in a
// BURST: `timer_ticks` leaps ~1250 - a nominal FIVE SECONDS at 250Hz - in ~15ms
// of real time, while its long-run average stays an innocent 250Hz. Therefore
// EVERY `timer_ticks + N` deadline in this kernel can expire almost instantly in
// wall-clock terms exactly when the host is busiest. This was proven with
// identical kernel bytes: booting with `lost_tick_policy=discard` snapped DHCP's
// retry spacing from 19ms to a correct 5.07s (#524). The #499 sweep then found
// the same broken idiom across the tree; this module is the shared answer.
//
// A tick is NOT a unit of time and must never be used as one. This module
// provides the real one: the CPU's Time Stamp Counter, which is driven by the
// CPU's own clock and cannot be reinjected, replayed, starved or lied about by
// a hypervisor's tick accounting.
//
// REUSE, DO NOT REINVENT (CLAUDE.md hard rule). This is deliberately ONE clock
// in ONE shared place. Before it, real-time measurement was hand-rolled per
// subsystem: drivers/xhci.c carried a private PIT-latch spin, drivers/ata.c its
// own rdtsc(), and net/url.c + crypto/sha512.c + media/aac.c + gui/jpeg.c each
// their own serialized rdtsc bench helper. That per-subsystem duplication is
// precisely the pattern that let the tick-deadline bug family spread. Anything
// that needs REAL elapsed time must call mono_*() rather than grow another
// private clock. Still carrying tick deadlines and able to adopt this directly:
// net/tls, sync/futex, ipc/msg, net/smb, proc/cron. (net/ftp adopted it in #499/#604.)
//
// CALIBRATION REFERENCE. Calibration counts PIT channel 0's countdown register,
// read via the latch command, and times it with the TSC. That reference is a
// deliberate reuse of the one wall-clock source already PROVEN on the real
// iMac14,4 (the #307/#375 xhci_delay work): PIT channel 0 keeps counting in
// hardware whether or not interrupts are enabled, so mono_init() can run before
// sti() and be ready long before usb_init() enumerates the xHCI. pit_init()
// (cpu/pic.c) programs channel 0 and nothing else reprograms it (the PC speaker
// uses channel 2).
//
// ACCURACY. A device timeout needs order-of-magnitude accuracy, not ppm: the
// fault being removed is a 300x error, so a few percent of TSC drift is
// irrelevant. Crucially this means we do NOT need to gate on the invariant-TSC
// CPUID bit (which the kvm64 CPU model may not even advertise): RDTSC is
// architecturally present on every x86-64, and even a non-invariant TSC drifts
// by a small factor, never by 300x. So this fix applies on BOTH the VM and the
// real iMac rather than silently disabling itself on one of them.

use core::sync::atomic::{AtomicU64, Ordering};

const MONO_PIT_CMD: u16 = 0x43;
const MONO_PIT_CH0: u16 = 0x40;
const MONO_PIT_INPUT_HZ: u64 = 1_193_182;

// cpu/pic.c pit_init() programs channel 0 with command byte 0x36, whose mode
// bits (3-1) are 011 = MODE 3, square-wave generator. In mode 3 the counter is
// decremented by TWO per PIT input clock (that is how it halves the period to
// produce a square wave), so N observed counter units == N/2 input clocks.
// Getting this factor wrong scales the entire clock by 2x, so it is NOT taken
// on trust: mono_init() returns the derived kHz and main.c prints it as a
// [MONO] boot line, which must land near the CPU's nominal TSC rate. Verified
// against a known host, not asserted from a datasheet.
const MONO_PIT_DEC_PER_CLOCK: u64 = 2;

// Channel 0 reload assumed by the PRE-CALIBRATION fallback delay below, used
// only to detect counter wraparound. cpu/pic.c main() programs 250 Hz, and
// pit_set_frequency() can raise it for games; a stale value here would at worst
// mis-attribute a wrap in the fallback path, which the TSC path (in use by the
// time anything can change the frequency) does not depend on at all.
const MONO_PIT_FALLBACK_HZ: u64 = 250;

static MONO_TSC_KHZ: AtomicU64 = AtomicU64::new(0);
static MONO_TSC_BASE: AtomicU64 = AtomicU64::new(0);

#[inline(always)]
fn mono_rdtsc() -> u64 {
    let lo: u32;
    let hi: u32;
    // SAFETY: rdtsc has no memory operands and no side effects beyond writing
    // EAX/EDX, both of which are declared as outputs. Present on all x86-64.
    unsafe {
        core::arch::asm!("rdtsc", out("eax") lo, out("edx") hi,
                         options(nomem, nostack, preserves_flags));
    }
    ((hi as u64) << 32) | (lo as u64)
}

/// # Safety: performs port I/O; caller must be Ring 0 (always true in-kernel).
#[inline(always)]
unsafe fn mono_inb(port: u16) -> u8 {
    let v: u8;
    core::arch::asm!("in al, dx", out("al") v, in("dx") port,
                     options(nomem, nostack, preserves_flags));
    v
}

/// # Safety: performs port I/O; caller must be Ring 0 (always true in-kernel).
#[inline(always)]
unsafe fn mono_outb(port: u16, val: u8) {
    core::arch::asm!("out dx, al", in("dx") port, in("al") val,
                     options(nomem, nostack, preserves_flags));
}

/// Latch-and-read PIT channel 0's live countdown value. The latch command does
/// not disturb counting, so this is safe to call at any time after pit_init().
/// # Safety: port I/O; Ring 0 only.
#[inline(always)]
unsafe fn mono_pit_latch() -> u16 {
    mono_outb(MONO_PIT_CMD, 0x00); // latch, channel 0
    let lo = mono_inb(MONO_PIT_CH0) as u16;
    let hi = mono_inb(MONO_PIT_CH0) as u16;
    (hi << 8) | lo
}

// ---------------------------------------------------------------------------
// #ASUSDIAG: CALIBRATION MUST NOT BE ABLE TO LOOK LIKE A HANG.
//
// The loop below used to be bounded ONLY by a 200,000,000-iteration safety
// counter. That is a bound, so it was not a hang, but on a machine whose 8254
// is absent or powered down it is very nearly the same thing. An unclaimed x86
// I/O port floats to 0xFF, so every latch returns 0xFFFF, `cur <= prev` always
// holds, the delta is always 0, `counted` never advances, and the loop runs the
// full safety count at THREE legacy port accesses per iteration. A legacy I/O
// access on a modern laptop goes out over eSPI at roughly a microsecond, which
// puts the worst case in the range of MINUTES, spent at main.c:932, which is
// BEFORE console_init(). Nothing on screen, no serial port on a laptop, no
// filesystem mounted: a machine that is indistinguishable from dead for several
// minutes and then carries on. That is the worst diagnostic outcome available.
//
// Three changes, in order of how much they matter:
//
//  1. PROBE FIRST, CALIBRATE SECOND. mono_pit_probe() decides in microseconds
//     whether channel 0 is counting at all. A frozen or floating counter is
//     detected and reported instead of being discovered slowly.
//  2. A REAL FALLBACK SOURCE. CPUID leaf 0x15 gives the TSC/core-crystal ratio
//     and leaf 0x16 the base frequency, either of which yields the TSC rate
//     outright with no timing loop. Before this there was no second source at
//     all: a dead PIT meant no clock, and every mono_busy_delay_us() then took
//     its own PIT fallback with its own 200,000,000 cap, so USB enumeration
//     would effectively never finish.
//  3. A PLAUSIBILITY BAND. The old code rejected only khz == 0. Ports returning
//     VARYING garbage rather than a steady 0xFF produce a non-zero but wildly
//     wrong kHz that every deadline in the kernel then trusts in silence. A
//     rate outside 200 MHz to 10 GHz is not a measurement, it is noise.
//
// The PIT stays the PREFERRED source when it is healthy: it is the reference
// proven on the real iMac14,4 (#307/#375), and CPUID 0x15/0x16 are Intel-only
// and absent on the kvm64 CPU model every VM here uses. This adds a fallback
// and a floor, it does not move the primary.

/// Outcome of mono_pit_probe(). Reported by mono_pit_state_rs() so the boot log
/// can say WHICH failure this machine has rather than only that timing failed.
const PIT_COUNTING: u32 = 0;
const PIT_FROZEN: u32 = 1; // decodes, but the count never changes
const PIT_FLOATING: u32 = 2; // reads 0xFFFF: nothing is driving the bus
const PIT_UNPROBED: u32 = 3;

/// Samples taken by the probe. Each is separated by a short pause spin because
/// channel 0 decrements every ~838ns and three back-to-back port accesses can
/// be quicker than that, which would make a HEALTHY counter look frozen.
const MONO_PIT_PROBE_SAMPLES: u32 = 64;

/// Iteration cap on the calibration loop itself, kept as a backstop BELOW the
/// probe. A healthy calibration exits after roughly 17,000 iterations (50ms of
/// PIT time at ~3us per latch), so 2,000,000 is about 100x headroom and caps the
/// worst case at a few seconds rather than a few minutes.
const MONO_CALIB_MAX_ITERS: u64 = 2_000_000;

/// A derived rate outside this band is noise, not a measurement.
const MONO_KHZ_MIN: u64 = 200_000; // 200 MHz
const MONO_KHZ_MAX: u64 = 10_000_000; // 10 GHz

/// How the live rate was derived. Reported by mono_source_rs().
const SRC_NONE: u32 = 0;
const SRC_PIT: u32 = 1;
const SRC_CPUID15: u32 = 2;
const SRC_CPUID16: u32 = 3;

static MONO_PIT_STATE: AtomicU64 = AtomicU64::new(PIT_UNPROBED as u64);
static MONO_SOURCE: AtomicU64 = AtomicU64::new(SRC_NONE as u64);
static MONO_CPUID_KHZ: AtomicU64 = AtomicU64::new(0);
static MONO_PIT_KHZ: AtomicU64 = AtomicU64::new(0);

/// Is PIT channel 0 actually counting? Returns one of PIT_*.
///
/// # Safety: Ring 0 port I/O.
unsafe fn mono_pit_probe() -> u32 {
    let first = mono_pit_latch();
    let mut moved = false;
    let mut any_not_ff = first != 0xFFFF;
    let mut i = 0u32;
    while i < MONO_PIT_PROBE_SAMPLES {
        let mut d = 0u32;
        while d < 64 {
            core::arch::asm!("pause", options(nomem, nostack, preserves_flags));
            d += 1;
        }
        let cur = mono_pit_latch();
        if cur != first {
            moved = true;
        }
        if cur != 0xFFFF {
            any_not_ff = true;
        }
        i += 1;
    }
    if moved {
        PIT_COUNTING
    } else if !any_not_ff {
        PIT_FLOATING
    } else {
        PIT_FROZEN
    }
}

/// TSC rate in kHz from CPUID, or 0 if the CPU does not report it.
///
/// Leaf 0x15 is exact where the firmware fills in the crystal frequency:
/// tsc_hz = ECX (core crystal Hz) * EBX (numerator) / EAX (denominator).
/// Leaf 0x16 EAX is the processor BASE frequency in MHz, and on every Intel part
/// with an invariant TSC the TSC ticks at exactly that base frequency, so this
/// is the correct number rather than an approximation of the current turbo
/// clock. Both leaves are Intel-only; AMD and the kvm64 model return zeros,
/// which the non-zero guards below reject, so this can only ever ADD a source.
fn mono_tsc_khz_from_cpuid() -> (u64, u32) {
    // CPUID has no memory operands and is unprivileged, so core exposes
    // __cpuid_count as a SAFE function on this target. The only correctness
    // requirement is checking the leaf number against the maximum basic leaf
    // before use, which every read below does.
    let max_leaf = core::arch::x86_64::__cpuid_count(0, 0).eax;

    if max_leaf >= 0x15 {
        let l = core::arch::x86_64::__cpuid_count(0x15, 0);
        if l.eax != 0 && l.ebx != 0 && l.ecx != 0 {
            let hz = (l.ecx as u64)
                .saturating_mul(l.ebx as u64)
                / (l.eax as u64);
            let khz = hz / 1000;
            if (MONO_KHZ_MIN..=MONO_KHZ_MAX).contains(&khz) {
                return (khz, SRC_CPUID15);
            }
        }
    }
    if max_leaf >= 0x16 {
        let l = core::arch::x86_64::__cpuid_count(0x16, 0);
        let mhz = (l.eax & 0xFFFF) as u64;
        let khz = mhz.saturating_mul(1000);
        if (MONO_KHZ_MIN..=MONO_KHZ_MAX).contains(&khz) {
            return (khz, SRC_CPUID16);
        }
    }
    (0, SRC_NONE)
}

/// PIT channel 0 probe verdict: 0 counting, 1 frozen, 2 floating, 3 not probed.
#[no_mangle]
pub extern "C" fn mono_pit_state_rs() -> u32 {
    MONO_PIT_STATE.load(Ordering::Relaxed) as u32
}

/// How the live rate was derived: 0 none, 1 PIT, 2 CPUID leaf 0x15, 3 leaf 0x16.
#[no_mangle]
pub extern "C" fn mono_source_rs() -> u32 {
    MONO_SOURCE.load(Ordering::Relaxed) as u32
}

/// The CPUID-derived rate in kHz, 0 if the CPU does not report one. Exposed so
/// the boot log can print BOTH sources side by side: two independent numbers
/// that agree are evidence, and two that disagree are a finding. A single number
/// with nothing to check it against is the situation this module was in when a
/// varying-garbage PIT could hand it a silently wrong clock.
#[no_mangle]
pub extern "C" fn mono_cpuid_khz_rs() -> u64 {
    MONO_CPUID_KHZ.load(Ordering::Relaxed)
}

/// The PIT-derived rate in kHz, 0 if PIT calibration was not run or failed.
#[no_mangle]
pub extern "C" fn mono_pit_khz_rs() -> u64 {
    MONO_PIT_KHZ.load(Ordering::Relaxed)
}

/// Calibrate the TSC against PIT channel 0 and start the clock.
/// Returns the derived TSC rate in kHz, or 0 if calibration failed (in which
/// case every mono_*() reader reports not-ready and callers keep their old
/// behaviour, so a calibration failure can never be worse than the status quo).
///
/// Call once, early, after pit_init() and before anything that needs real time.
/// Interrupts may be off: that is the point of using the PIT counter directly.
#[no_mangle]
pub extern "C" fn mono_init_rs(timer_hz: u32) -> u64 {
    let hz = if timer_hz == 0 { 250u64 } else { timer_hz as u64 };
    let mut reload = MONO_PIT_INPUT_HZ / hz;
    if reload == 0 || reload > 65535 {
        reload = 65535;
    }

    // A second, INDEPENDENT source, obtained with no timing loop at all. Read it
    // first, so that even a machine whose PIT wastes the whole calibration
    // budget still ends up with a usable clock rather than none.
    let (cpuid_khz, cpuid_src) = mono_tsc_khz_from_cpuid();
    MONO_CPUID_KHZ.store(cpuid_khz, Ordering::SeqCst);

    // SAFETY: Ring 0 port I/O against the PIT, which pit_init() has programmed.
    let pit_state = unsafe { mono_pit_probe() };
    MONO_PIT_STATE.store(pit_state as u64, Ordering::SeqCst);

    let mut pit_khz = 0u64;
    if pit_state == PIT_COUNTING {
        // Calibrate over ~50ms of PIT time: long enough that the per-latch
        // sampling cost is noise, short enough not to stall boot.
        let target_counts = (MONO_PIT_INPUT_HZ * 50 / 1000) * MONO_PIT_DEC_PER_CLOCK;
        let mut counted: u64 = 0;
        let mut safety: u64 = MONO_CALIB_MAX_ITERS;

        // SAFETY: Ring 0 port I/O against the PIT, as above.
        let k = unsafe {
            let t0 = mono_rdtsc();
            let mut prev = mono_pit_latch();
            while counted < target_counts && safety > 0 {
                safety -= 1;
                core::arch::asm!("pause", options(nomem, nostack, preserves_flags));
                let cur = mono_pit_latch();
                // Channel 0 counts down and wraps at `reload`. saturating_sub
                // keeps a nonsense reading (cur > reload) from underflowing.
                let delta = if cur <= prev {
                    (prev - cur) as u64
                } else {
                    (prev as u64) + reload.saturating_sub(cur as u64)
                };
                counted += delta;
                prev = cur;
            }
            let t1 = mono_rdtsc();

            if safety == 0 || counted == 0 {
                0u64
            } else {
                let tsc_delta = t1.wrapping_sub(t0);
                // elapsed_ms = counted / DEC / INPUT_HZ * 1000
                // khz        = tsc_delta / elapsed_ms
                //            = tsc_delta * INPUT_HZ * DEC / (counted * 1000)
                // Worst case is comfortably inside u64, and saturating_mul makes
                // that structural rather than a comment.
                let num = tsc_delta
                    .saturating_mul(MONO_PIT_INPUT_HZ)
                    .saturating_mul(MONO_PIT_DEC_PER_CLOCK);
                let den = counted.saturating_mul(1000);
                if den == 0 {
                    0u64
                } else {
                    num / den
                }
            }
        };
        // A rate outside the plausibility band is noise, not a measurement. The
        // old code rejected only zero, so ports returning VARYING garbage rather
        // than a steady 0xFF produced a wildly wrong but non-zero clock that
        // every deadline in the kernel then trusted in silence.
        if (MONO_KHZ_MIN..=MONO_KHZ_MAX).contains(&k) {
            pit_khz = k;
        }
    }
    MONO_PIT_KHZ.store(pit_khz, Ordering::SeqCst);

    // The PIT stays PREFERRED when healthy: it is the reference proven on the
    // real iMac14,4 (#307/#375), and CPUID 0x15/0x16 are Intel-only and absent
    // on the kvm64 CPU model every VM here uses. CPUID is a floor, not a
    // replacement.
    let (khz, src) = if pit_khz != 0 {
        (pit_khz, SRC_PIT)
    } else if cpuid_khz != 0 {
        (cpuid_khz, cpuid_src)
    } else {
        (0u64, SRC_NONE)
    };
    MONO_SOURCE.store(src as u64, Ordering::SeqCst);

    if khz == 0 {
        return 0;
    }
    MONO_TSC_KHZ.store(khz, Ordering::SeqCst);
    MONO_TSC_BASE.store(mono_rdtsc(), Ordering::SeqCst);
    khz
}

/// 1 once the clock is calibrated and usable, 0 otherwise. Callers that have a
/// legacy tick fallback should branch on this rather than guessing.
#[no_mangle]
pub extern "C" fn mono_ready_rs() -> i32 {
    if MONO_TSC_KHZ.load(Ordering::Relaxed) != 0 { 1 } else { 0 }
}

#[inline(always)]
fn mono_cycles_since_base() -> u64 {
    let base = MONO_TSC_BASE.load(Ordering::Relaxed);
    let d = mono_rdtsc().wrapping_sub(base);
    // TSC skew between cores, or a thread migrating mid-wait, can make `now`
    // appear to PRECEDE `base`. Left alone the wraparound would present as a
    // colossal elapsed time and fire every deadline at once: the exact failure
    // mode this module exists to remove, reintroduced by the fix. Clamp to 0.
    if (d as i64) < 0 { 0 } else { d }
}

/// Milliseconds of REAL time since mono_init(). 0 if the clock is not ready:
/// check mono_ready_rs() to tell "not ready" from "0ms elapsed".
#[no_mangle]
pub extern "C" fn mono_ms_rs() -> u64 {
    let khz = MONO_TSC_KHZ.load(Ordering::Relaxed);
    if khz == 0 {
        return 0;
    }
    // khz == cycles per millisecond, so this is a plain u64 divide: no u128, no
    // __udivti3, and no float (the kernel target is soft-float, SSE disabled).
    mono_cycles_since_base() / khz
}

/// Microseconds of REAL time since mono_init(). 0 if the clock is not ready.
#[no_mangle]
pub extern "C" fn mono_us_rs() -> u64 {
    let khz = MONO_TSC_KHZ.load(Ordering::Relaxed);
    if khz == 0 {
        return 0;
    }
    let d = mono_cycles_since_base();
    // us = cycles * 1000 / khz. The multiply overflows past ~1.8e16 cycles
    // (~83 days at 2.5GHz), so fall back to a coarser order of operations
    // beyond that rather than wrapping.
    if d < u64::MAX / 1000 {
        d * 1000 / khz
    } else {
        (d / khz) * 1000
    }
}

/// The calibrated TSC rate in kHz (0 if not ready). Exposed so the boot banner
/// can print it: a clock nobody can check is a clock nobody should trust.
#[no_mangle]
pub extern "C" fn mono_tsc_khz_rs() -> u64 {
    MONO_TSC_KHZ.load(Ordering::Relaxed)
}

// =============================================================================
// #507: THE shared short busy-delay. Real microseconds, interrupts in any
// state, no scheduler required.
// =============================================================================
//
// WHY THIS EXISTS. drivers/xhci.c carried a PRIVATE copy of the PIT-latch loop
// below, and that copy was missing the MODE 3 factor of two: it treated one
// observed counter unit as one PIT input clock, so every xhci_delay(ms) slept
// for ms/2 of real time. USB enumeration is built out of spec-mandated MINIMUM
// waits (TDRSTR port-reset recovery, the 2ms Set-Address recovery of USB 2.0
// 9.2.6.3, hub power-good), and silently halving all of them is precisely the
// shape of the intermittent real-iMac enumeration failures (#433/#373/#366).
//
// The factor of two was never the real defect though: the DUPLICATION was. The
// constant that was wrong here (MONO_PIT_DEC_PER_CLOCK) was already RIGHT forty
// lines up, in this file, because mono_init_rs() needs it too. So this is not a
// "patch a 2 into the call site" fix. The private clock is deleted and both
// users now derive their timing from the one place that owns the PIT: this
// module. Get the constant wrong in future and BOTH the monotonic clock and
// every device delay move together, which the boot [MONO] kHz line catches.
//
// PRECEDENCE. Once the TSC is calibrated (which happens at main.c immediately
// after pit_init(), long before usb_init()) this spins on the TSC: no port I/O
// per iteration, so it is far cheaper than latching the PIT thousands of times.
// The PIT path remains only as the pre-calibration / calibration-failed
// fallback, and it is now correct rather than 2x fast.
//
// BOUNDED (#426). Both paths carry an iteration cap. A delay is not a device
// poll, so it terminates by construction, but a wedged TSC or an absent PIT
// must degrade to "returns early" and never to an unbounded hang.
#[no_mangle]
pub extern "C" fn mono_busy_delay_us_rs(us: u64) {
    if us == 0 {
        return;
    }

    let khz = MONO_TSC_KHZ.load(Ordering::Relaxed);
    if khz != 0 {
        // cycles = us * khz / 1000. saturating_mul keeps an absurd caller
        // argument from wrapping the target to a tiny value (which would turn
        // a long delay into no delay at all: the very bug class above).
        let target = us.saturating_mul(khz) / 1000;
        let t0 = mono_rdtsc();
        let mut safety: u64 = 1_000_000_000;
        while safety > 0 {
            safety -= 1;
            // SAFETY: PAUSE has no operands and no memory effects.
            unsafe {
                core::arch::asm!("pause", options(nomem, nostack, preserves_flags));
            }
            if mono_rdtsc().wrapping_sub(t0) >= target {
                return;
            }
        }
        return;
    }

    // ---- Fallback: measure PIT channel 0's countdown directly. -------------
    // Used only before mono_init_rs() has run (or if it failed). Channel 0
    // keeps counting in hardware whether or not interrupts are enabled, so this
    // works arbitrarily early.
    // #ASUSDIAG: if mono_init_rs() already established that channel 0 is not
    // counting, this loop cannot ever reach its target and would run its full
    // safety count at three legacy port accesses per iteration, which is minutes
    // on a machine whose legacy I/O goes over eSPI. Every caller of this is a
    // hardware settle delay, so on such a machine the honest thing is a short
    // bounded pause spin: it does not pretend to be microseconds, but it also
    // does not turn one xHCI reset step into a multi-minute stall. Callers
    // already treat this as best-effort; mono_ready_rs() is how a caller asks
    // whether real time is available.
    let pit_st = MONO_PIT_STATE.load(Ordering::Relaxed) as u32;
    if pit_st == PIT_FROZEN || pit_st == PIT_FLOATING {
        let spins = us.saturating_mul(64).min(4_000_000);
        let mut i = 0u64;
        while i < spins {
            // SAFETY: PAUSE has no operands and no memory effects.
            unsafe {
                core::arch::asm!("pause", options(nomem, nostack, preserves_flags));
            }
            i += 1;
        }
        return;
    }

    let hz = MONO_PIT_FALLBACK_HZ;
    let mut reload = MONO_PIT_INPUT_HZ / hz;
    if reload == 0 || reload > 65535 {
        reload = 65535;
    }

    // MODE 3: the counter is decremented by MONO_PIT_DEC_PER_CLOCK per input
    // clock, so a microsecond is that many more counter units than a naive
    // reading of the datasheet's input frequency suggests. This is the exact
    // factor drivers/xhci.c used to omit.
    let target_counts = us
        .saturating_mul(MONO_PIT_INPUT_HZ)
        .saturating_mul(MONO_PIT_DEC_PER_CLOCK)
        / 1_000_000;
    if target_counts == 0 {
        return;
    }

    let mut counted: u64 = 0;
    let mut safety: u64 = 200_000_000;
    // SAFETY: Ring 0 port I/O against the PIT, programmed by pit_init().
    unsafe {
        let mut prev = mono_pit_latch();
        while counted < target_counts && safety > 0 {
            safety -= 1;
            core::arch::asm!("pause", options(nomem, nostack, preserves_flags));
            let cur = mono_pit_latch();
            let delta = if cur <= prev {
                (prev - cur) as u64
            } else {
                (prev as u64) + reload.saturating_sub(cur as u64)
            };
            counted += delta;
            prev = cur;
        }
    }
}
