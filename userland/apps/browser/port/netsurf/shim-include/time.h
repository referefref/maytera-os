/* time.h - freestanding shim for MayteraOS NetSurf port.
 * libdom uses only time(NULL) for event timestamps. We declare the standard
 * time types/struct tm and time() as an extern. A real time() must be added
 * to the MayteraOS libc (trivially: wrap the existing SYS_TIME syscall, see
 * libc/syscall.h sys_time()). No localtime/strftime (not needed). C only. */
#ifndef MAYTERA_SHIM_TIME_H
#define MAYTERA_SHIM_TIME_H
#include <types.h>

typedef long time_t;
typedef long clock_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

extern time_t time(time_t *t);

#endif
