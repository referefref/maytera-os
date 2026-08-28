// cpu/wallclock.h - #115 (local 120): the kernel's ONE calendar clock + the ONE
// calendar-time converter. Implemented in rustkern/ktime.rs.
//
// READ THIS BEFORE ADDING ANY TIMESTAMP ANYWHERE IN THIS KERNEL.
//
// There are three clocks in this kernel and they answer three different
// questions. Using the wrong one is the single most repeated timing mistake in
// the tree (see blame.md):
//
//   timer_ticks / SYS_GET_TICKS   ticks DELIVERED. NOT time. Under KVM the PIT
//                                 replays lost ticks in bursts, so this can
//                                 advance five nominal seconds in 15ms of real
//                                 time (#524/#525). Never derive a duration or
//                                 a date from it.
//   mono_ms_rs() (cpu/mono.h)     real ELAPSED time since boot, from the TSC.
//                                 Correct for timeouts and deadlines. Says
//                                 nothing about what day it is.
//   wallclock_now_unix()          CALENDAR time: seconds since the UNIX epoch,
//                                 from the CMOS RTC. The only thing a
//                                 filesystem may stamp a file with.
//
// Note that sys_time() / SYS_TIME (proc/syscall.c) is NOT a fourth option: it
// returns seconds since BOOT (#113), so it is neither a duration from a fixed
// origin nor a date. It must never be used to fill an mtime.
//
// EVERY function here reports "I do not know" distinctly from a value:
// wallclock_now_unix() returns 0, ktime_civil_to_unix_rs() returns -1, and
// ktime_unix_to_dos_rs() returns -1 and writes nothing. Callers MUST propagate
// that as absence. Storing a plausible zero instead is the exact defect #115
// exists to remove.
#ifndef KERNEL_CPU_WALLCLOCK_H
#define KERNEL_CPU_WALLCLOCK_H

#include "../types.h"

// Seconds since the UNIX epoch (UTC), or 0 if the RTC does not present a
// plausible date. Cached to at most one RTC read per second of monotonic time.
int64_t wallclock_now_unix_rs(void);
#define wallclock_now_unix() wallclock_now_unix_rs()

// Civil UTC -> epoch seconds. Returns -1 for any impossible or out-of-window
// date (the window is 1980-01-01 .. 2107-12-31, the range a FAT directory
// entry can represent).
int64_t ktime_civil_to_unix_rs(int32_t year, int32_t month, int32_t day,
                               int32_t hour, int32_t minute, int32_t second);

// FAT/DOS packed directory-entry date+time -> epoch seconds.
// Returns 0 for an UNSTAMPED entry (dos_date == 0) or a structurally impossible
// one. 0 means "this filesystem does not know", never 1970-01-01.
int64_t ktime_dos_to_unix_rs(uint16_t dos_date, uint16_t dos_time);

// Epoch seconds -> FAT/DOS packed date+time. Returns 0 and writes both words on
// success; returns -1 and writes NOTHING if the instant is outside FAT range.
int32_t ktime_unix_to_dos_rs(int64_t unix_s, uint16_t *out_date, uint16_t *out_time);

// Boot self-test over hand-computed vectors. 0 = pass. *out_checks receives the
// number of assertions actually executed, so "passed" is distinguishable from
// "never ran".
int32_t ktime_selftest_rs(uint32_t *out_checks);

// ==========================================================================
// #113: THE REALTIME CLOCK. Epoch MICROSECONDS, sub-second, step-tolerant.
//
// This is what time() and gettimeofday() are built on. It is NOT a fourth
// clock: it is wallclock_now_unix() (the RTC) anchored against mono_us() (the
// TSC), so it has the RTC's ACCURACY and the TSC's RESOLUTION, and it is
// monotonic between deliberate steps by construction.
//
// It tracks an SNTP correction automatically, because net/sntp.c corrects the
// clock by writing the RTC and the anchor re-syncs to the RTC on a throttle.
// The monotonic clock is never disturbed when the wall clock steps.
//
// 0 means "the kernel does not know what time it is", exactly as
// wallclock_now_unix() does. It does NOT mean 1970-01-01.
// ==========================================================================
int64_t realtime_us_rs(void);    // epoch microseconds, 0 = unknown
int64_t realtime_sec_rs(void);   // epoch seconds from the SAME anchor
int64_t realtime_steps_rs(void); // count of wall-clock steps taken since boot

// Epoch seconds -> civil UTC. `out` receives 7 ints:
//   [0] year [1] month 1-12 [2] day [3] hour [4] minute [5] second
//   [6] weekday, 0 = Sunday
// Returns 0 on success; -1 and writes NOTHING if the instant is out of window.
int32_t ktime_unix_to_civil_rs(int64_t unix_s, int32_t *out);

// Self-test for the realtime clock's step/refine decision logic and for
// ktime_unix_to_civil_rs. 0 = pass. Provably RED via `make RTCLKTESTFAIL=1`.
int32_t rtclock_selftest_rs(uint32_t *out_checks);

// ==========================================================================
// #234a: THE PC/DOS VIEW OF THE SAME CLOCK.
//
// A DOS guest asks what time it is in three different shapes (INT 21h AH=2Ch,
// INT 21h AH=2Ah, INT 1Ah AH=00) from two different files, and every one of
// them used to answer a compile-time constant. Epyx Rogue seeds its generator
// with CX+DX from AH=2Ch; a frozen clock gave it seed 0, its LCG has 0 as a
// fixed point, and the game hung in dungeon generation with a blank screen.
// See the long note at ktime_dos_clock_rs in rustkern/ktime.rs.
//
// ONE converter, so the constant cannot come back in only one of the three.
// ==========================================================================
typedef struct {
    uint8_t  hour, minute, second, hundredth;
    uint8_t  weekday, month, day, known;   // weekday 0=Sunday; known 0 = no RTC
    uint16_t year;
    uint16_t _pad;
    uint32_t ticks;                        // 18.2065 Hz ticks since midnight
} kdos_clock_t;
_Static_assert(sizeof(kdos_clock_t) == 16,
               "kdos_clock_t must match rustkern/ktime.rs KDosClock");

// The BIOS midnight rollover constant, 24h of ticks. Shared with the Rust side
// rather than written twice: a tick counter that rolls over on one value and is
// generated against another puts a discontinuity at midnight.
#define KDOS_TICKS_PER_DAY 0x001800B0u

int32_t ktime_dos_clock_rs(int64_t realtime_us, uint64_t uptime_us,
                           kdos_clock_t *out);

// The ONE call site shape. Both arguments come from primitives that already
// exist (cpu/wallclock.h's RTC-anchored realtime clock and cpu/mono.h's TSC
// uptime), and wrapping them here means no caller has to remember that the
// second one is the fallback.
static inline void kdos_clock_now(kdos_clock_t *out) {
    extern uint64_t mono_us_rs(void);
    ktime_dos_clock_rs(realtime_us_rs(), mono_us_rs(), out);
}

#endif // KERNEL_CPU_WALLCLOCK_H
