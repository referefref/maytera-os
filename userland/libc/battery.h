#ifndef _LIBC_BATTERY_H
#define _LIBC_BATTERY_H
// battery.h (Ring 3) - control-method battery status, for the compositor's
// tray meter and Settings (#battmeter).
//
// THE DATA ITSELF IS NOT HERE. It lives in the kernel (drivers/battery.c +
// rustkern/battery.rs); this header is a thin syscall wrapper, following
// uiscale.h's precedent exactly - no caching, no state, just the syscall.
//
// CALLERS MUST THROTTLE THEMSELVES. Each call below is one syscall, and the
// kernel evaluates a small ACPI table scan (or a test-injection file read)
// on every BATT_PRESENT/BATT_PCT/BATT_STATE/BATT_MINUTES call (the kernel
// also rate-limits its own re-scan internally, but that is a safety net, not
// a licence to poll from a draw loop). The compositor draw path MUST NOT call
// these directly every frame - poll on a timer (a few seconds) and cache the
// result, exactly like every other tray indicator that reads a slow-changing
// external fact. battery_gen() is provided so a poller can skip the other
// four calls entirely when nothing changed.

#include "syscall.h"

static inline int battery_present(void) { return (int)syscall2(SYS_BATTERY, BATT_PRESENT, 0); }
static inline int battery_pct(void)     { return (int)syscall2(SYS_BATTERY, BATT_PCT, 0); }
static inline int battery_state(void)   { return (int)syscall2(SYS_BATTERY, BATT_STATE, 0); }
static inline int battery_minutes(void) { return (int)syscall2(SYS_BATTERY, BATT_MINUTES, 0); }
static inline int battery_gen(void)     { return (int)syscall2(SYS_BATTERY, BATT_GEN, 0); }

#endif // _LIBC_BATTERY_H
