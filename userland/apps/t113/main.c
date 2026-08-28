// t113 - #113 ON-VM PROOF that time() and gettimeofday() are a REAL UNIX epoch.
//
// WHAT THIS MEASURES, AND WHY IT MEASURES IT THIS WAY.
//
// The defect is a value that LOOKS like a timestamp and is not: sys_time()
// returned SECONDS SINCE BOOT, so time() answered about 40 on a freshly booted
// machine and every date in the OS was 1970-01-01 plus the uptime.
//
// A test that only checks "is it bigger than last time" cannot see that: a boot
// counter increases perfectly. A test that compares time() against the kernel's
// OWN converter cannot see it either, because both arms would share the same
// wrong origin. That is the differential blindness this project has been bitten
// by (blame.md: the RUST-DIFF tls_parse test was green because both arms and the
// generator all shared one wrong constant).
//
// So the reference clock here is INDEPENDENT and this program computes it
// ITSELF:
//   * It reads the CMOS RTC through SYS_GET_RTC_DATE (143) / SYS_GET_RTC_TIME
//     (142), which are a different kernel path from SYS_TIME / SYS_REALTIME_US.
//   * It converts that civil date to epoch seconds with its OWN arithmetic,
//     written out below. It does NOT call ktime_civil_to_unix_rs, does NOT call
//     libc mktime, and shares no code with the thing under test.
//   * VALIDATE THE INSTRUMENT BEFORE TRUSTING IT: that arithmetic is first run
//     against four hand-computed anchors (test 0). If the reference converter
//     is wrong, this program says so and stops, rather than confidently
//     reporting that the kernel disagrees with a broken ruler.
//
// THE RTC IS UTC ON THIS OS. That is not an assumption: net/sntp.c sets it from
// an SNTP reply's UTC instant, and userland/libc/tz.h states the convention for
// the whole tree. time() must therefore equal the RTC epoch with NO timezone
// applied, and test 3 asserts exactly that: if anyone ever "helpfully" adds the
// local offset inside time(), this goes red.
//
// OUTPUT DISCIPLINE. Launched from /CONFIG/AUTORUN.CFG, and an autorun-launched
// process emits ONE SERIAL RECORD PER write(), so every line is formatted into
// a buffer and issued as exactly one write(2, ...). Same rule as t115/toolchk.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "syscall.h"
#include "sys/time.h"
#include "time.h"

static int g_pass = 0, g_fail = 0;

static void line(const char *s) { write(2, s, strlen(s)); }

static void say(const char *fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	line(b);
}

static void check(int ok, const char *what)
{
	char b[256];
	if (ok) { g_pass++; snprintf(b, sizeof(b), "[t113] PASS  %s\n", what); }
	else    { g_fail++; snprintf(b, sizeof(b), "[t113] FAIL  %s\n", what); }
	line(b);
}

// ---------------------------------------------------------------------------
// THE INDEPENDENT REFERENCE CLOCK. Deliberately a different algorithm from the
// kernel's (which is Hinnant's closed form): this is a plain accumulate-years
// loop. Two implementations that agree are evidence; one implementation checked
// against itself is not.
// ---------------------------------------------------------------------------
static int ref_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

static long long ref_civil_to_epoch(int y, int mo, int d, int h, int mi, int s)
{
	static const int md[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
	long long days = 0;
	int i;
	if (y < 1970 || mo < 1 || mo > 12 || d < 1) return -1;
	for (i = 1970; i < y; i++) days += ref_leap(i) ? 366 : 365;
	for (i = 1; i < mo; i++) {
		days += md[i - 1];
		if (i == 2 && ref_leap(y)) days += 1;
	}
	days += (d - 1);
	return days * 86400LL + (long long)h * 3600LL + (long long)mi * 60LL + s;
}

// Read the RTC through the syscalls that are NOT under test.
static long long rtc_epoch_now(int *oy, int *omo, int *od, int *oh, int *omi, int *os)
{
	long pd = syscall0(SYS_GET_RTC_DATE);
	long pt = syscall0(SYS_GET_RTC_TIME);
	int y  = (int)((pd >> 16) & 0xFFFF);
	int mo = (int)((pd >> 8)  & 0xFF);
	int d  = (int)(pd & 0xFF);
	int h  = (int)((pt >> 16) & 0xFF);
	int mi = (int)((pt >> 8)  & 0xFF);
	int s  = (int)(pt & 0xFF);
	if (oy) { *oy = y; *omo = mo; *od = d; *oh = h; *omi = mi; *os = s; }
	return ref_civil_to_epoch(y, mo, d, h, mi, s);
}

int main(void)
{
	line("[t113] ===== #113: time()/gettimeofday() epoch proof =====\n");

	// --- TEST 0: VALIDATE THE INSTRUMENT --------------------------------
	// Four anchors computed independently of this program and of the kernel.
	// Until these pass, nothing below this line means anything.
	{
		int ok = 1;
		ok &= (ref_civil_to_epoch(1970, 1, 1, 0, 0, 0)   == 0LL);
		ok &= (ref_civil_to_epoch(2000, 1, 1, 0, 0, 0)   == 946684800LL);
		ok &= (ref_civil_to_epoch(2024, 2, 29, 0, 0, 0)  == 1709164800LL);
		ok &= (ref_civil_to_epoch(2026, 8, 13, 12, 34, 56) == 1786624496LL);
		check(ok, "reference converter matches 4 hand-computed anchors "
		          "(instrument validated)");
		if (!ok) {
			line("[t113] ABORT: the reference clock is broken; any verdict "
			     "about the kernel would be worthless.\n");
			line("[t113] RESULT: FAIL\n");
			return 1;
		}
	}

	// --- What the independent clock says it is right now -----------------
	int y, mo, d, h, mi, s;
	long long ref = rtc_epoch_now(&y, &mo, &d, &h, &mi, &s);
	say("[t113] RTC (independent path, UTC): %04d-%02d-%02d %02d:%02d:%02d "
	    "-> epoch %lld\n", y, mo, d, h, mi, s, ref);
	check(ref > 1750000000LL,
	      "RTC epoch is after 2025-06 (the RTC itself is sane)");

	// --- TEST 1: time() is an epoch, not an uptime -----------------------
	long t = time(NULL);
	say("[t113] time()            = %ld\n", t);
	// A freshly booted machine has an uptime in the tens or hundreds. The OLD
	// behaviour lands far below this bound; a real epoch is far above it.
	check((long long)t > 1750000000LL,
	      "time() is a real UNIX epoch, not seconds since boot");

	// --- TEST 2: time() agrees with the INDEPENDENT RTC reading ----------
	{
		long long diff = (long long)t - ref;
		if (diff < 0) diff = -diff;
		say("[t113] |time() - RTC|      = %lld s\n", diff);
		check(diff <= 2LL,
		      "time() agrees with the independently-read RTC within 2s");
	}

	// --- TEST 3: NO timezone is applied inside time() --------------------
	// If someone adds the local offset inside time()/gettimeofday(), the delta
	// above becomes a whole number of half-hours. Assert the small offsets too,
	// so a +00:30 zone cannot slip through the 2s window.
	{
		long long diff = (long long)t - ref;
		if (diff < 0) diff = -diff;
		check(diff < 1800LL,
		      "time() is UTC: no timezone offset applied inside it (POSIX)");
	}

	// --- TEST 4: gettimeofday() agrees with time(), field by field -------
	struct timeval tv;
	int grc = gettimeofday(&tv, NULL);
	say("[t113] gettimeofday()    = %ld.%06ld (rc=%d)\n",
	    (long)tv.tv_sec, (long)tv.tv_usec, grc);
	check(grc == 0, "gettimeofday() returns 0");
	check((long long)tv.tv_sec > 1750000000LL,
	      "gettimeofday().tv_sec is a real UNIX epoch");
	check(tv.tv_usec >= 0 && tv.tv_usec < 1000000L,
	      "gettimeofday().tv_usec is in range [0,1000000)");
	{
		long long dd = (long long)tv.tv_sec - (long long)t;
		if (dd < 0) dd = -dd;
		check(dd <= 1LL, "gettimeofday().tv_sec agrees with time() (one anchor)");
	}

	// --- TEST 5: it actually ADVANCES, and SUB-SECOND --------------------
	// The old clock could only move in 1000ms steps. Take samples across a
	// short interval and require (a) never backwards, and (b) at least one
	// strictly sub-second increment, which a seconds-resolution clock cannot
	// produce. This is the check that separates "a plausible number" from "a
	// working clock".
	{
		struct timeval a;
		long long prev = -1, first = -1, last = -1;
		int backwards = 0, subsec = 0, i;
		for (i = 0; i < 400; i++) {
			gettimeofday(&a, NULL);
			long long us = (long long)a.tv_sec * 1000000LL + a.tv_usec;
			if (first < 0) first = us;
			if (prev >= 0) {
				if (us < prev) backwards++;
				long long step = us - prev;
				if (step > 0 && step < 1000000LL) subsec++;
			}
			prev = us;
			last = us;
			// Do a little real work so time genuinely passes between samples.
			{ volatile int spin = 0; int k; for (k = 0; k < 20000; k++) spin += k; }
		}
		say("[t113] 400 samples spanned %lld us; %d sub-second steps; "
		    "%d backward steps\n", last - first, subsec, backwards);
		check(backwards == 0, "gettimeofday() never goes backwards");
		check(subsec > 0,
		      "gettimeofday() advances SUB-SECOND (impossible for the old "
		      "seconds-resolution clock)");
		check(last > first, "gettimeofday() advances at all");
	}

	// --- TEST 6: clock_gettime(CLOCK_REALTIME) shares the same anchor ----
	{
		struct timespec ts;
		int rc = clock_gettime(CLOCK_REALTIME, &ts);
		say("[t113] CLOCK_REALTIME    = %ld.%09ld (rc=%d)\n",
		    (long)ts.tv_sec, (long)ts.tv_nsec, rc);
		check(rc == 0, "clock_gettime(CLOCK_REALTIME) returns 0");
		long long dd = (long long)ts.tv_sec - (long long)t;
		if (dd < 0) dd = -dd;
		check(dd <= 2LL, "CLOCK_REALTIME agrees with time()");
	}

	// --- TEST 7: CLOCK_MONOTONIC did NOT become a calendar ---------------
	// The monotonic clock must stay an uptime. If a future change points it at
	// the epoch anchor, every deadline in the OS silently changes meaning.
	{
		struct timespec ms;
		int rc = clock_gettime(CLOCK_MONOTONIC, &ms);
		say("[t113] CLOCK_MONOTONIC   = %ld.%09ld (rc=%d)\n",
		    (long)ms.tv_sec, (long)ms.tv_nsec, rc);
		check(rc == 0, "clock_gettime(CLOCK_MONOTONIC) returns 0");
		check((long long)ms.tv_sec < 1000000000LL,
		      "CLOCK_MONOTONIC is still an UPTIME, not the epoch");
	}

	say("[t113] RESULT: %s  (%d passed, %d failed)\n",
	    g_fail == 0 ? "PASS" : "FAIL", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
