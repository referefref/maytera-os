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
// net/tls, sync/futex, ipc/msg, net/smb, net/ftp, proc/cron.
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

    // Calibrate over ~50ms of PIT time: long enough that the per-latch sampling
    // cost is noise, short enough not to stall boot.
    let target_counts = (MONO_PIT_INPUT_HZ * 50 / 1000) * MONO_PIT_DEC_PER_CLOCK;

    let mut counted: u64 = 0;
    // Bounded: a wedged or absent PIT must degrade to "calibration failed",
    // never to an unbounded hang. Every poll in this kernel is bounded (#426).
    let mut safety: u64 = 200_000_000;

    // SAFETY: Ring 0 port I/O against the PIT, which pit_init() has programmed.
    let khz = unsafe {
        let t0 = mono_rdtsc();
        let mut prev = mono_pit_latch();
        while counted < target_counts && safety > 0 {
            safety -= 1;
            core::arch::asm!("pause", options(nomem, nostack, preserves_flags));
            let cur = mono_pit_latch();
            // Channel 0 counts down and wraps at `reload`. saturating_sub keeps
            // a nonsense reading (cur > reload) from underflowing the delta.
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
            return 0;
        }

        let tsc_delta = t1.wrapping_sub(t0);
        // elapsed_ms = counted / DEC / INPUT_HZ * 1000
        // khz        = tsc_delta / elapsed_ms
        //            = tsc_delta * INPUT_HZ * DEC / (counted * 1000)
        // Worst case ~1.3e8 * 1193182 * 2 ~= 3e14: comfortably inside u64, and
        // saturating_mul makes that structural rather than a comment.
        let num = tsc_delta
            .saturating_mul(MONO_PIT_INPUT_HZ)
            .saturating_mul(MONO_PIT_DEC_PER_CLOCK);
        let den = counted.saturating_mul(1000);
        if den == 0 {
            return 0;
        }
        num / den
    };

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
