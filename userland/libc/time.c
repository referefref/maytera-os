// time.c - calendar time for MayteraOS userland (#422 / CPython #359).
// Pure epoch<->civil conversion (Howard Hinnant's algorithm). No timezone
// database: localtime == gmtime (UTC). time() returns whatever the kernel
// clock provides via SYS_TIME (currently seconds; wall-clock epoch depends on
// the kernel RTC wiring, a separate concern).
#include "time.h"
#include "stdio.h"
#include "string.h"
#include "syscall.h"

// time() itself lives in unistd.c (long time(long*)); time.h just declares it.

double difftime(time_t end, time_t start) {
    return (double)(end - start);
}

// days from civil date (Hinnant). y/m/d are proleptic Gregorian.
static long days_from_civil(long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

static void civil_from_days(long z, int *year, int *month, int *day) {
    z += 719468L;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : -9);
    *year = (int)(y + (m <= 2));
    *month = (int)m;
    *day = (int)d;
}

struct tm *gmtime_r(const time_t *timep, struct tm *r) {
    time_t t = *timep;
    long days = t / 86400;
    long rem = t % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    r->tm_hour = (int)(rem / 3600);
    r->tm_min = (int)((rem % 3600) / 60);
    r->tm_sec = (int)(rem % 60);
    // weekday: 1970-01-01 was Thursday (4)
    int wd = (int)((days % 7 + 4) % 7);
    if (wd < 0) wd += 7;
    r->tm_wday = wd;
    int year, month, day;
    civil_from_days(days, &year, &month, &day);
    r->tm_year = year - 1900;
    r->tm_mon = month - 1;
    r->tm_mday = day;
    // day of year
    long jan1 = days_from_civil(year, 1, 1);
    r->tm_yday = (int)(days - jan1);
    r->tm_isdst = 0;
    // #359 Phase 3a: MayteraOS has no timezone database, everything is UTC.
    r->tm_gmtoff = 0;
    r->tm_zone = "UTC";
    return r;
}

static struct tm g_tm;
struct tm *gmtime(const time_t *timep) { return gmtime_r(timep, &g_tm); }
struct tm *localtime_r(const time_t *timep, struct tm *r) { return gmtime_r(timep, r); }
struct tm *localtime(const time_t *timep) { return gmtime_r(timep, &g_tm); }

time_t timegm(struct tm *tm) {
    long days = days_from_civil(tm->tm_year + 1900, (unsigned)(tm->tm_mon + 1),
                                (unsigned)tm->tm_mday);
    time_t t = (time_t)days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
    // normalize the passed-in struct
    gmtime_r(&t, tm);
    return t;
}
time_t mktime(struct tm *tm) { return timegm(tm); }   // UTC only

static const char *wday_short[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *wday_long[]  = {"Sunday","Monday","Tuesday","Wednesday",
                                   "Thursday","Friday","Saturday"};
static const char *mon_short[]  = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
static const char *mon_long[]   = {"January","February","March","April","May","June",
                                   "July","August","September","October","November","December"};

char *asctime_r(const struct tm *tm, char *buf) {
    int w = tm->tm_wday & 7; if (w > 6) w = 0;
    int m = tm->tm_mon; if (m < 0 || m > 11) m = 0;
    snprintf(buf, 26, "%s %s %2d %02d:%02d:%02d %d\n",
             wday_short[w], mon_short[m], tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    return buf;
}
static char g_asc[32];
char *asctime(const struct tm *tm) { return asctime_r(tm, g_asc); }
char *ctime(const time_t *timep) { return asctime(localtime(timep)); }

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    size_t o = 0;
    char tmp[64];
    #define PUT(str) do { const char *_p = (str); while (*_p && o + 1 < max) s[o++] = *_p++; } while (0)
    int w = tm->tm_wday & 7; if (w > 6) w = 0;
    int m = tm->tm_mon; if (m < 0 || m > 11) m = 0;
    while (*fmt) {
        if (*fmt != '%') { if (o + 1 < max) s[o++] = *fmt; fmt++; continue; }
        fmt++;
        switch (*fmt) {
            case 'a': PUT(wday_short[w]); break;
            case 'A': PUT(wday_long[w]); break;
            case 'b': case 'h': PUT(mon_short[m]); break;
            case 'B': PUT(mon_long[m]); break;
            case 'd': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mday); PUT(tmp); break;
            case 'e': snprintf(tmp, sizeof tmp, "%2d", tm->tm_mday); PUT(tmp); break;
            case 'H': snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour); PUT(tmp); break;
            case 'I': { int h = tm->tm_hour % 12; if (!h) h = 12;
                        snprintf(tmp, sizeof tmp, "%02d", h); PUT(tmp); break; }
            case 'j': snprintf(tmp, sizeof tmp, "%03d", tm->tm_yday + 1); PUT(tmp); break;
            case 'm': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mon + 1); PUT(tmp); break;
            case 'M': snprintf(tmp, sizeof tmp, "%02d", tm->tm_min); PUT(tmp); break;
            case 'p': PUT(tm->tm_hour < 12 ? "AM" : "PM"); break;
            case 'S': snprintf(tmp, sizeof tmp, "%02d", tm->tm_sec); PUT(tmp); break;
            case 'y': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_year + 1900) % 100); PUT(tmp); break;
            case 'Y': snprintf(tmp, sizeof tmp, "%d", tm->tm_year + 1900); PUT(tmp); break;
            case 'w': snprintf(tmp, sizeof tmp, "%d", w); PUT(tmp); break;
            case 'C': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_year + 1900) / 100); PUT(tmp); break;
            case 'n': PUT("\n"); break;
            case 't': PUT("\t"); break;
            case 'F': snprintf(tmp, sizeof tmp, "%04d-%02d-%02d",
                               tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); PUT(tmp); break;
            case 'T': case 'X': snprintf(tmp, sizeof tmp, "%02d:%02d:%02d",
                               tm->tm_hour, tm->tm_min, tm->tm_sec); PUT(tmp); break;
            case 'R': snprintf(tmp, sizeof tmp, "%02d:%02d", tm->tm_hour, tm->tm_min); PUT(tmp); break;
            case 'D': case 'x': snprintf(tmp, sizeof tmp, "%02d/%02d/%02d",
                               tm->tm_mon + 1, tm->tm_mday, (tm->tm_year + 1900) % 100); PUT(tmp); break;
            case 'c': snprintf(tmp, sizeof tmp, "%s %s %2d %02d:%02d:%02d %d",
                               wday_short[w], mon_short[m], tm->tm_mday,
                               tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
                      PUT(tmp); break;
            case '%': if (o + 1 < max) s[o++] = '%'; break;
            case '\0': goto done;
            default:
                if (o + 1 < max) s[o++] = '%';
                if (o + 1 < max) s[o++] = *fmt;
                break;
        }
        fmt++;
    }
done:
    #undef PUT
    if (max > 0) s[o < max ? o : max - 1] = '\0';
    return o;
}

// #359 Phase 3a: timezone/daylight/tzname/tzset() are declared in time.h (for
// CPython's timemodule.c) but DEFINED in the CPython port's miscsupp
// supplement (compatsupp/../miscsupp/misc.c already has UTC-only stubs from
// an earlier pass) - not duplicated here to avoid a multiple-definition link
// error.
