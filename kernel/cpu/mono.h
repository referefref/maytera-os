// mono.h - #525: THE shared monotonic clock. Real elapsed time, kernel-wide.
#ifndef MONO_H
#define MONO_H

// The kernel builds -nostdinc with its own fixed-width types (uint64_t is
// `unsigned long long` here, NOT `unsigned long`). Including <stdint.h> instead
// silently redefines them and detonates in unrelated translation units.
#include "../types.h"

// USE THIS, NOT timer_ticks, FOR ANY DEADLINE.
//
// `timer_ticks` counts ticks DELIVERED, not time ELAPSED. Under KVM the PIT
// keeps tick REINJECTION enabled, so a starved vCPU gets its missed ticks
// re-delivered in a BURST: timer_ticks leaps ~1250 (a nominal 5 SECONDS at
// 250Hz) in ~15ms of real time, while its long-run average still looks like an
// innocent 250Hz. So `timer_ticks + N` deadlines expire near-instantly in
// wall-clock terms exactly when the machine is busiest. Proven with identical
// kernel bytes under lost_tick_policy=discard (#524); swept tree-wide (#499).
//
// The implementation is Rust (rustkern.rs, `mono_*_rs`), per the 2026-07-16
// all-new-kernel-code-in-Rust rule. It is TSC-backed and calibrated once at
// boot against PIT channel 0 (the one wall-clock reference already proven on
// the real iMac14,4 by #307/#375), so it is ready before usb_init() and works
// with interrupts off.
//
// SEMANTICS
//   mono_ready()   - 1 once calibrated. Everything else reports 0 until then,
//                    so branch on this to tell "not ready" from "0ms elapsed".
//                    A calibration failure leaves callers on their previous
//                    behaviour; it can never be worse than the status quo.
//   mono_ms()      - milliseconds of REAL time since mono_init().
//   mono_us()      - microseconds of REAL time since mono_init().
//   mono_tsc_khz() - the calibrated rate, so the boot banner can print it.
//
// TYPICAL USE (a bounded device wait):
//   uint64_t t0 = mono_ms();
//   while (!done) { if (mono_ms() - t0 >= budget_ms) break; ... }
//
// PRINTING: uint64_t is `unsigned long long` in this kernel, so use %llu.
//
// DO NOT hand-roll another clock. drivers/xhci.c (private PIT-latch spin),
// drivers/ata.c (private rdtsc), net/url.c / crypto/sha512.c / media/aac.c /
// gui/jpeg.c (private rdtsc bench helpers) each grew their own before this
// existed, which is exactly how the tick-deadline bug family spread. Subsystems
// still on tick deadlines that can adopt this: net/tls, sync/futex, ipc/msg,
// net/smb, net/ftp, proc/cron.

uint64_t mono_init_rs(uint32_t timer_hz);
int32_t  mono_ready_rs(void);
uint64_t mono_ms_rs(void);
uint64_t mono_us_rs(void);
uint64_t mono_tsc_khz_rs(void);

// Rust u64/u32/i32 must match this kernel's uint64_t/uint32_t/int32_t exactly
// across the FFI. Lock it in rather than trusting it (the tree's sizeof-lock
// convention for every Rust seam).
_Static_assert(sizeof(uint64_t) == 8, "mono: u64 FFI width");
_Static_assert(sizeof(uint32_t) == 4, "mono: u32 FFI width");
_Static_assert(sizeof(int32_t)  == 4, "mono: i32 FFI width");

// The `_rs` suffix is this tree's marker for a Rust-implemented symbol; these
// wrappers give callers the clean name without hiding where it comes from.
static inline uint64_t mono_init(uint32_t timer_hz) { return mono_init_rs(timer_hz); }
static inline int      mono_ready(void)             { return (int)mono_ready_rs(); }
static inline uint64_t mono_ms(void)                { return mono_ms_rs(); }
static inline uint64_t mono_us(void)                { return mono_us_rs(); }
static inline uint64_t mono_tsc_khz(void)           { return mono_tsc_khz_rs(); }

#endif // MONO_H
