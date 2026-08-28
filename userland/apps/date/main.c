// date - print the date and time
// Usage: date [-u] [-R] [-I[date|seconds]] [-d @EPOCH] [+FORMAT]
//
// #745 (local 108, second batch).
//
// THE DEFECT, AND THE PREMISE IN THE TICKET THAT TURNED OUT TO BE WRONG.
// This program ignored argv entirely - `(void)argc; (void)argv;` - and printed
//
//     Uptime: 57 seconds (0:00:57)
//
// so `date +%Y-%m-%d` printed an uptime. Its own header said "Uses uptime
// since no RTC calendar is available yet", and the ticket that sent me here
// repeated it: the root cause was believed to be kernel-side, because
// sys_time() returns seconds since BOOT (blame.md, local 91).
//
// sys_time() IS still uptime, and that is still a real defect (see below). But
// it is not this program's problem, because THIS KERNEL HAS SHIPPED A WALL
// CLOCK TO USERLAND FOR A LONG TIME: SYS_GET_RTC_TIME (142) and
// SYS_GET_RTC_DATE (143) are both dispatched in kernel/proc/syscall.c, both
// read the CMOS RTC, and /APPS/CLOCK, Settings, the compositor's clock widget
// and userland/libc/tz.c were all already using them. The comment was a claim
// about the past that nobody re-checked - the third time in a fortnight this
// project has paid for that exact shape (blame.md, local 82, local 99,
// local 108).
//
// So date needs NO kernel change. It reads the RTC, applies the configured
// timezone through userland/libc/tz.c (THE one place that offset is applied -
// there is no second copy here), and formats what it read.
//
// WHAT IS STILL BROKEN AND IS NOT FIXED HERE: time() and gettimeofday() in the
// libc are built on SYS_TIME, which returns uptime, so every OTHER caller of
// time() on this OS still gets a number of seconds since boot. That is a
// kernel-side fix (read the CMOS date at boot, convert once to an epoch, and
// have sys_time() return that epoch plus uptime) and it is raised as its own
// finding rather than smuggled in here. date deliberately does not call time().
//
// -d IS DELIBERATELY ONLY "@EPOCH". A general date parser is a large thing to
// get right and a small thing to get subtly wrong; @EPOCH is exact, it is what
// scripts use, and it is what lets tools/coreutils-oracle diff this formatter
// against the host's own GNU date on a FIXED instant rather than on "now",
// which cannot be compared at all.
#include <stdarg.h>

#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "getopt.h"
#include "syscall.h"
#include "tz.h"
#include "mtool.h"

typedef struct {
	int  y, mo, d, h, mi, s, wday, yday;
	int  off_min;          // minutes east of UTC that were applied
	const char *zone;      // what %Z prints
} when_t;

static const char *WDAY[7]  = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                "Thursday", "Friday", "Saturday" };
static const char *MON[12]  = { "January", "February", "March", "April", "May",
                                "June", "July", "August", "September",
                                "October", "November", "December" };

static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int mdays(int y, int m)
{
	static const int d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
	if (m == 2 && leap(y)) return 29;
	return d[m - 1];
}

// Days since 1970-01-01 for a civil date. Howard Hinnant's days_from_civil,
// which is exact for the whole proleptic Gregorian range and needs no table.
static long days_from_civil(int y, int m, int d)
{
	y -= (m <= 2);
	long era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long)doe - 719468;
}

static void civil_from_days(long z, int *y, int *m, int *d)
{
	z += 719468;
	long era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned doe = (unsigned)(z - era * 146097);
	unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	long yy = (long)yoe + era * 400;
	unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	unsigned mp = (5 * doy + 2) / 153;
	unsigned dd = doy - (153 * mp + 2) / 5 + 1;
	unsigned mm = mp + (mp < 10 ? 3 : -9);
	*y = (int)(yy + (mm <= 2));
	*m = (int)mm;
	*d = (int)dd;
}

static long long epoch_of(const when_t *w)
{
	long days = days_from_civil(w->y, w->mo, w->d);
	long long secs = (long long)days * 86400LL
	               + w->h * 3600LL + w->mi * 60LL + w->s;
	return secs - (long long)w->off_min * 60LL;   // back to UTC
}

static void fill_derived(when_t *w)
{
	long days = days_from_civil(w->y, w->mo, w->d);
	int wd = (int)((days % 7 + 11) % 7);           // 1970-01-01 was a Thursday
	w->wday = wd;
	int yd = 0;
	for (int m = 1; m < w->mo; m++) yd += mdays(w->y, m);
	w->yday = yd + w->d;
}

static void from_epoch(long long e, int off_min, const char *zone, when_t *w)
{
	long long local = e + (long long)off_min * 60LL;
	long days = (long)(local / 86400);
	long long rem = local % 86400;
	if (rem < 0) { rem += 86400; days -= 1; }
	civil_from_days(days, &w->y, &w->mo, &w->d);
	w->h  = (int)(rem / 3600);
	w->mi = (int)((rem % 3600) / 60);
	w->s  = (int)(rem % 60);
	w->off_min = off_min;
	w->zone = zone;
	fill_derived(w);
}

static int app(char *out, int n, int cap, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int m = vsnprintf(out + n, (size_t)(cap - n), fmt, ap);
	va_end(ap);
	if (m < 0) m = 0;
	if (n + m > cap - 1) m = cap - 1 - n;
	return n + m;
}

static void format(const when_t *w, const char *fmt)
{
	char out[2048];
	int n = 0;
	int offh = w->off_min / 60, offm = w->off_min % 60;
	if (offm < 0) offm = -offm;
	for (const char *p = fmt; *p && n < (int)sizeof out - 1; p++) {
		if (*p != '%') { out[n++] = *p; continue; }
		p++;
		switch (*p) {
		case 'Y': n = app(out, n, sizeof out, "%d", w->y); break;
		case 'C': n = app(out, n, sizeof out, "%02d", w->y / 100); break;
		case 'y': n = app(out, n, sizeof out, "%02d", w->y % 100); break;
		case 'm': n = app(out, n, sizeof out, "%02d", w->mo); break;
		case 'd': n = app(out, n, sizeof out, "%02d", w->d); break;
		case 'e': n = app(out, n, sizeof out, "%2d", w->d); break;
		case 'H': n = app(out, n, sizeof out, "%02d", w->h); break;
		case 'I': { int h12 = w->h % 12; if (!h12) h12 = 12;
		            n = app(out, n, sizeof out, "%02d", h12); break; }
		case 'M': n = app(out, n, sizeof out, "%02d", w->mi); break;
		case 'S': n = app(out, n, sizeof out, "%02d", w->s); break;
		case 'j': n = app(out, n, sizeof out, "%03d", w->yday); break;
		case 'a': n = app(out, n, sizeof out, "%.3s", WDAY[w->wday]); break;
		case 'A': n = app(out, n, sizeof out, "%s", WDAY[w->wday]); break;
		case 'b': case 'h':
		          n = app(out, n, sizeof out, "%.3s", MON[w->mo - 1]); break;
		case 'B': n = app(out, n, sizeof out, "%s", MON[w->mo - 1]); break;
		case 'p': n = app(out, n, sizeof out, "%s", w->h < 12 ? "AM" : "PM"); break;
		case 'u': n = app(out, n, sizeof out, "%d", w->wday == 0 ? 7 : w->wday); break;
		case 'w': n = app(out, n, sizeof out, "%d", w->wday); break;
		case 's': n = app(out, n, sizeof out, "%lld", epoch_of(w)); break;
		case 'F': n = app(out, n, sizeof out, "%04d-%02d-%02d", w->y, w->mo, w->d); break;
		case 'D': n = app(out, n, sizeof out, "%02d/%02d/%02d", w->mo, w->d, w->y % 100); break;
		case 'T': n = app(out, n, sizeof out, "%02d:%02d:%02d", w->h, w->mi, w->s); break;
		case 'R': n = app(out, n, sizeof out, "%02d:%02d", w->h, w->mi); break;
		case 'Z': n = app(out, n, sizeof out, "%s", w->zone); break;
		case 'z': n = app(out, n, sizeof out, "%+03d%02d", offh, offm); break;
		// %:z is the ISO-8601 colon form, which is what -Iseconds needs. It is
		// the one two-character conversion here; anything else after '%:' is
		// refused with the rest.
		case ':':
			if (p[1] != 'z') {
				char spec[4] = { '%', ':', p[1] ? p[1] : '?', 0 };
				mtool_refuse(spec, "this conversion specifier is not implemented");
			}
			p++;
			n = app(out, n, sizeof out, "%+03d:%02d", offh, offm);
			break;
		case 'n': out[n++] = '\n'; break;
		case 't': out[n++] = '\t'; break;
		case '%': out[n++] = '%'; break;
		case '\0':
			mtool_die(MTOOL_EX_FAIL, "format string ends with a lone '%%'");
		default: {
			// The whole point of this ticket. An unknown conversion is not
			// copied through and it is not dropped: it is named.
			char spec[3] = { '%', *p, 0 };
			mtool_refuse(spec, "this conversion specifier is not implemented");
		}
		}
	}
	out[n++] = '\n';
	mtool_wall(1, out, (size_t)n);
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	int utc = 0, rfc = 0, iso = 0;
	long long forced = 0;
	int have_forced = 0;
	int c;
	while ((c = getopt(argc, argv, "uRI::d:")) != -1) {
		switch (c) {
		case 'u': utc = 1; break;
		case 'R': rfc = 1; break;
		case 'I':
			iso = 1;
			if (optarg && strcmp(optarg, "seconds") == 0) iso = 2;
			else if (optarg && strcmp(optarg, "date") != 0)
				mtool_refuse("date -I with that precision",
				             "only -I, -Idate and -Iseconds are implemented");
			break;
		case 'd':
			if (optarg[0] != '@')
				mtool_refuse("date -d with a date STRING",
				             "there is no date parser here; only -d @EPOCH is "
				             "implemented, which is exact");
			forced = 0;
			{
				const char *q = optarg + 1;
				int neg = 0;
				if (*q == '-') { neg = 1; q++; }
				if (!*q) mtool_die(MTOOL_EX_FAIL, "invalid epoch: '%s'", optarg);
				for (; *q; q++) {
					if (*q < '0' || *q > '9')
						mtool_die(MTOOL_EX_FAIL, "invalid epoch: '%s'", optarg);
					forced = forced * 10 + (*q - '0');
				}
				if (neg) forced = -forced;
			}
			have_forced = 1;
			break;
		default: {
			char b[4] = { '-', (char)optopt, 0, 0 };
			if (optopt == 's')
				mtool_refuse("date -s (set the clock)",
				             "setting the RTC goes through Settings, which owns "
				             "the one path that also converts local time back to "
				             "the UTC the RTC holds (userland/libc/tz.h)");
			if (optopt == 'r')
				mtool_refuse("date -r FILE (a file's timestamp)",
				             "this kernel does not store or report file "
				             "timestamps; see userland/libc/utime.h");
			mtool_bad_option(b);
		}
		}
	}

	when_t w;
	int off = utc ? 0 : tz_offset_minutes();
	const char *zone = utc ? "UTC" : tz_id(tz_index());

	if (have_forced) {
		from_epoch(forced, off, zone, &w);
	} else {
		// The RTC holds UTC (userland/libc/tz.h states this and it is why NTP
		// writes it there). Read it raw and go through the same epoch path, so
		// there is exactly one civil-date implementation in this file.
		int h, mi, s, d, mo, y;
		get_rtc_time(&h, &mi, &s);
		get_rtc_date(&d, &mo, &y);
		if (y < 1970 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
		    h > 23 || mi > 59 || s > 60)
			// A wrong-looking clock is reported, not formatted into something
			// that looks like a date. This is the one thing the old date got
			// morally right: it never claimed to know the date. It just said
			// so in a way nothing could use.
			mtool_die(MTOOL_EX_FAIL,
			          "the RTC reports %04d-%02d-%02d %02d:%02d:%02d, which is "
			          "not a plausible wall clock; refusing to format it",
			          y, mo, d, h, mi, s);
		when_t utcw;
		utcw.y = y; utcw.mo = mo; utcw.d = d;
		utcw.h = h; utcw.mi = mi; utcw.s = s;
		utcw.off_min = 0;
		utcw.zone = "UTC";
		fill_derived(&utcw);
		from_epoch(epoch_of(&utcw), off, zone, &w);
	}

	const char *fmt = NULL;
	for (int i = optind; i < argc; i++) {
		if (argv[i][0] == '+') {
			if (fmt) mtool_die(MTOOL_EX_FAIL, "more than one +FORMAT given");
			fmt = argv[i] + 1;
		} else {
			mtool_die(MTOOL_EX_FAIL, "unexpected operand '%s' (a format starts "
			                         "with '+'; setting the clock is not "
			                         "implemented here)", argv[i]);
		}
	}

	if (fmt)      format(&w, fmt);
	else if (rfc) format(&w, "%a, %d %b %Y %H:%M:%S %z");
	else if (iso == 2) format(&w, "%Y-%m-%dT%H:%M:%S%:z");
	else if (iso) format(&w, "%Y-%m-%d");
	else          format(&w, "%a %b %e %H:%M:%S %Z %Y");
	return MTOOL_EX_OK;
}
