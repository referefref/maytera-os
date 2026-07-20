
#ifndef COMPAT_TIME_H
#define COMPAT_TIME_H

#include "../../../libc/syscall.h"

typedef long time_t;
typedef long clock_t;
#define CLOCKS_PER_SEC 1000

static inline time_t time(time_t *tp) {
    time_t t = (time_t)syscall0(SYS_TIME);
    if (tp) *tp = t;
    return t;
}

static inline clock_t clock(void) {
    return (clock_t)syscall0(SYS_CLOCK);
}

#endif
