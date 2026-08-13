/* time.h - Duktape-port shim (shadows the NetSurf shim for this build only). */
#ifndef MAYTERA_DUK_TIME_H
#define MAYTERA_DUK_TIME_H
#include <types.h>
typedef long time_t;
typedef long clock_t;
struct tm {
    int tm_sec; int tm_min; int tm_hour;
    int tm_mday; int tm_mon; int tm_year;
    int tm_wday; int tm_yday; int tm_isdst;
};
extern time_t time(time_t *t);
extern double difftime(time_t a, time_t b);
extern struct tm *gmtime(const time_t *tp);
extern struct tm *localtime(const time_t *tp);
extern time_t mktime(struct tm *tm);
#endif
