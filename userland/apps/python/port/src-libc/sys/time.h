// sys/time.h - timeval + gettimeofday for MayteraOS libc (#359 Phase 2).
#ifndef LIBC_SYS_TIME_H
#define LIBC_SYS_TIME_H
#include "../time.h"    // time_t

struct timeval {
    time_t      tv_sec;
    long        tv_usec;
};
struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};
struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};

int gettimeofday(struct timeval *tv, void *tz);

#endif // LIBC_SYS_TIME_H
