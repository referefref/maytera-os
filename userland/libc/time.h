// time.h - calendar time for MayteraOS userland (#422 / CPython #359).
#ifndef LIBC_TIME_H
#define LIBC_TIME_H

#include <stddef.h>
#include <stdint.h>

typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 250L   // kernel timer tick rate (see SYS_CLOCK)

struct tm {
    int tm_sec;    // 0-60
    int tm_min;    // 0-59
    int tm_hour;   // 0-23
    int tm_mday;   // 1-31
    int tm_mon;    // 0-11
    int tm_year;   // years since 1900
    int tm_wday;   // 0-6 (Sunday=0)
    int tm_yday;   // 0-365
    int tm_isdst;  // daylight saving flag
    // #359 Phase 3a: BSD/glibc extension fields. gcc on this build host always
    // predefines __linux__ (no real freestanding target triple) with __GLIBC__
    // undefined (no glibc headers, -nostdinc), so CPython's Modules/timemodule.c
    // "#if defined(__linux__) && !defined(__GLIBC__)" branch always compiles and
    // needs these; MayteraOS has no timezone database so gmtime_r() always fills
    // them in as UTC / offset 0.
    long  tm_gmtoff;
    const char *tm_zone;
};

// #359 Phase 3a: POSIX tzset() globals. timemodule.c's HAVE_DECL_TZNAME branch
// (defined-but-0 in pyconfig.h, so #ifdef still takes it) references these
// unconditionally. MayteraOS has no timezone database, so these are fixed at
// UTC / no DST and tzset() is a no-op.
extern long timezone;
extern int daylight;
extern char *tzname[2];
void tzset(void);

time_t time(time_t *t);
double difftime(time_t end, time_t start);
struct tm *gmtime(const time_t *timep);
struct tm *gmtime_r(const time_t *timep, struct tm *result);
struct tm *localtime(const time_t *timep);
struct tm *localtime_r(const time_t *timep, struct tm *result);
time_t mktime(struct tm *tm);
time_t timegm(struct tm *tm);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
char *asctime(const struct tm *tm);
char *asctime_r(const struct tm *tm, char *buf);
char *ctime(const time_t *timep);

/* #359 Phase 2: monotonic/real clocks. struct timespec shares the guard used
   by pthread.h so including both is safe. */
#ifndef _TIMESPEC_DEFINED
#define _TIMESPEC_DEFINED
struct timespec {
    long tv_sec;
    long tv_nsec;
};
#endif

typedef int clockid_t;
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4

int clock_gettime(clockid_t clk, struct timespec *ts);
int clock_getres(clockid_t clk, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif // LIBC_TIME_H
