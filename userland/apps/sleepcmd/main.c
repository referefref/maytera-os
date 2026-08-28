// sleep - pause for a length of time
// Usage: sleep NUMBER[SUFFIX]...   (suffix s, m, h or d; NUMBER may be fractional)
//
// #745 (local 108, THIRD batch).
//
// WHAT WAS WRONG. `int seconds = atoi(argv[1]);` and then `if (seconds <= 0)`
// error. Three consequences:
//
//   * `sleep 0.5` was "invalid time interval". atoi("0.5") is 0, and 0 hit the
//     <= 0 rejection, so the single most common fractional sleep anyone types
//     was refused by a program that COULD have done it.
//   * `sleep 0` was an error. sleep(1) accepts it and returns immediately.
//   * `sleep 1.9` would have been 1 second had it not been rejected: atoi stops
//     at the '.', silently. `sleep 2m`, `sleep 1 2` and `sleep 1.5h` were all
//     either refused or silently misread.
//
// SUB-SECOND SLEEP IS REAL ON THIS KERNEL. Established rather than assumed:
// SYS_SLEEP (7) takes MILLISECONDS, and kernel/proc/process.c's proc_sleep()
// arms `me->wake_time = sched_now_ms() + ms` against the MONOTONIC millisecond
// clock (#483/#499 moved it off timer_ticks precisely so a KVM tick burst cannot
// collapse the deadline) and blocks on the scheduler. So the granularity of the
// PRIMITIVE is 1 ms. The granularity of the WAKE is the timer tick, 250 Hz = 4 ms
// (kernel/proc/syscall.c SYS_GET_TICKS: "250Hz monotonic ticks (4ms each)" -
// note the stale "100Hz/10ms" comment on the #define in syscall.h, which is the
// two-comments-disagree trap blame.md already records).
//
// SO THIS ROUNDS UP, NEVER DOWN. POSIX says sleep suspends for AT LEAST the
// requested interval, so converting 1500 microseconds to 2 ms is conformant and
// converting it to 1 ms is not. Every conversion here is a ceiling, and the
// residual error is bounded by one tick, which is a property of the hardware
// timer and not something a userland tool can improve.
//
// WHAT IS REFUSED, AND WHY EACH ONE RATHER THAN A SILENT APPROXIMATION:
//   * `infinity` - sleep(1) accepts it. Implementing it is a loop, but a
//     process parked for ever with no way for a user at the console to reach it
//     is a worse answer than saying no.
//   * A negative or unparsable interval - atoi() used to turn every one of these
//     into 0 or a prefix.
//
// MULTIPLE OPERANDS ARE VALIDATED BEFORE ANYTHING SLEEPS. sleep(1) sums them,
// and `sleep 1 x` must fail IMMEDIATELY rather than sleep a second and then
// complain. mtool_each_operand() drives the validation pass, which is the shared
// loop doing exactly the job it exists for; the sleeping is then one call on the
// total.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "getopt.h"
#include "syscall.h"
#include "mtool.h"

typedef struct { unsigned long long total_ms; } acc_t;

// Parse "NUMBER[SUFFIX]" into milliseconds, rounding UP. Returns 0 on success.
// Deliberately not strtod(): a decimal fraction of a second is an exact decimal
// quantity and this converts it exactly, so `sleep 0.001` is 1 ms and not
// whatever the nearest double happened to be.
static int parse_interval(const char *s, unsigned long long *out_ms)
{
	if (!s || !*s) return -1;

	unsigned long long whole = 0;
	unsigned long long frac = 0;      // scaled to millionths
	unsigned long long scale = 100000ULL;
	int any_digit = 0, extra_nonzero = 0;
	const char *p = s;

	if (*p == '+' || *p == '-') return -1;   // sleep(1) rejects a signed interval

	for (; *p >= '0' && *p <= '9'; p++) {
		any_digit = 1;
		// Bounded so that whole * 86400000 * 1000 (the largest suffix, in
		// microseconds) cannot wrap a uint64. Refusing is the only honest answer
		// to an interval this build cannot represent; wrapping would sleep for a
		// short random time and report success.
		if (whole > 100000000ULL) return -1;
		whole = whole * 10ULL + (unsigned long long)(*p - '0');
	}
	if (*p == '.') {
		p++;
		for (; *p >= '0' && *p <= '9'; p++) {
			any_digit = 1;
			if (scale) { frac += (unsigned long long)(*p - '0') * scale; scale /= 10ULL; }
			else if (*p != '0') extra_nonzero = 1;   // below a microsecond
		}
	}
	if (!any_digit) return -1;

	unsigned long long mult_ms = 1000ULL;    // default suffix is seconds
	if (*p) {
		switch (*p) {
		case 's': mult_ms = 1000ULL; break;
		case 'm': mult_ms = 60ULL * 1000ULL; break;
		case 'h': mult_ms = 3600ULL * 1000ULL; break;
		case 'd': mult_ms = 86400ULL * 1000ULL; break;
		default:  return -1;
		}
		p++;
		if (*p) return -1;
	}

	// value_us = (whole + frac/1e6) * mult_ms * 1000
	unsigned long long us = whole * mult_ms * 1000ULL + (frac * mult_ms) / 1000ULL;
	if ((frac * mult_ms) % 1000ULL) us++;    // ceiling
	unsigned long long ms = us / 1000ULL;
	if (us % 1000ULL || extra_nonzero) ms++; // ceiling: never sleep short
	*out_ms = ms;
	return 0;
}

static int check_one(const char *operand, void *ctx)
{
	acc_t *a = (acc_t *)ctx;
	unsigned long long ms = 0;

	if (strcmp(operand, "infinity") == 0 || strcmp(operand, "inf") == 0)
		mtool_refuse("sleep infinity",
		             "a process parked for ever cannot be reached from the "
		             "console on this OS; give a real interval");

	if (parse_interval(operand, &ms) != 0) {
		mtool_warn("invalid time interval '%s' (want NUMBER[s|m|h|d], e.g. 0.5 or 2m)",
		           operand);
		return 1;
	}
	if (a->total_ms + ms < a->total_ms) {
		mtool_warn("total interval overflows");
		return 1;
	}
	a->total_ms += ms;
	return 0;
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);

	int c;
	while ((c = getopt(argc, argv, "")) != -1) {
		(void)c;
		mtool_bad_option(argv[optind - 1]);
	}

	acc_t a;
	a.total_ms = 0;

	// PASS ONE: every operand must parse before ANY of them is slept. The
	// missing-operand message is the shared loop's, because "sleep with no
	// argument is an error" is not a fact about sleep, it is the same fact for
	// every tool that has required operands.
	int rc = mtool_each_operand(argc, argv, optind, check_one, &a,
	                            "missing operand (usage: sleep NUMBER[s|m|h|d]...)");
	if (rc != MTOOL_EX_OK) return rc;

	// PASS TWO. SYS_SLEEP takes a uint32 of milliseconds, so a very long total is
	// slept in chunks rather than truncated into the argument's width - which is
	// exactly the silent-cap shape this ticket exists to remove.
	unsigned long long left = a.total_ms;
	while (left > 0) {
		unsigned int chunk = (left > 0x7FFFFFFFULL) ? 0x7FFFFFFFu : (unsigned int)left;
		sys_sleep(chunk);
		left -= chunk;
	}
	return MTOOL_EX_OK;
}
