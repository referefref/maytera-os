// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// posixextra.c - #359 Phase 2 filesystem/time/misc libc functions that the
// CPython filesystem stdlib (and other POSIX-y userland) needs. Honest, small
// implementations over the existing MayteraOS syscalls; where the kernel has no
// backing primitive the function is a clearly-commented best-effort stub.
#include "unistd.h"
#include "fcntl.h"
#include "stdlib.h"
#include "time.h"
#include "sys/time.h"
#include "sys/stat.h"
#include "stdio.h"
#include "errno.h"
#include "syscall.h"

// access(): resolve existence/permission via stat(). We do not have per-mode
// kernel checks here, so a successful stat means the path is reachable.
int access(const char *path, int mode) {
    (void)mode;
    struct stat st;
    if (stat(path, &st) != 0) { errno = ENOENT; return -1; }
    return 0;
}

// ftruncate()/truncate(). These were no-ops that RETURNED SUCCESS, with a
// comment claiming the kernel had no truncate primitive (#745 local 109). The
// claim was false for FAT and unnecessary for ext2, and the success return was
// the damage: busybox vi opens without O_TRUNC on purpose and calls ftruncate()
// after writing, so on the FAT ESP every save of a SHORTENED file kept its old
// tail and reported that it had not.
//
// SYS_FTRUNCATE shrinks; it refuses a grow rather than pretending, so a caller
// that needs a longer file gets EINVAL instead of a wrong answer.
int ftruncate(int fd, long length) {
    if (fd < 0 || length < 0) { errno = EINVAL; return -1; }
    if (syscall2(SYS_FTRUNCATE, (long)fd, length) != 0) { errno = EINVAL; return -1; }
    return 0;
}

// truncate() is ftruncate() plus an open, and is written that way so there is
// no second definition of what truncation means. O_WRONLY without O_TRUNC is
// deliberate: opening with O_TRUNC would empty the file before the requested
// length could be applied.
int truncate(const char *path, long length) {
    if (!path || length < 0) { errno = EINVAL; return -1; }
    int fd = open(path, O_WRONLY, 0);
    if (fd < 0) { errno = ENOENT; return -1; }
    int rc = ftruncate(fd, length);
    close(fd);
    return rc;
}

int getpagesize(void) { return 4096; }

long sysconf(int name) {
    switch (name) {
        case _SC_PAGESIZE:          return 4096;
        case _SC_CLK_TCK:           return 250;   // kernel timer tick rate
        case _SC_OPEN_MAX:          return 256;
        case _SC_ARG_MAX:           return 131072;
        case _SC_CHILD_MAX:         return 64;
        case _SC_NGROUPS_MAX:       return 32;
        case _SC_NPROCESSORS_CONF:  return 1;
        case _SC_NPROCESSORS_ONLN:  return 1;
        case _SC_PHYS_PAGES:        return (256 * 1024 * 1024) / 4096;
        case _SC_AVPHYS_PAGES:      return (64 * 1024 * 1024) / 4096;
        default:                    return -1;
    }
}

// #113: gettimeofday()/clock_gettime(CLOCK_REALTIME) are the UNIX EPOCH in UTC.
//
// WHAT THEY USED TO BE. tv_sec came from SYS_TIME, which returned SECONDS SINCE
// BOOT, and tv_usec came from `uptime_ms % 1000`. So the whole OS believed it
// was a few seconds past 1970-01-01, and, worse, the two fields came from TWO
// DIFFERENT COUNTERS with no common origin: the microsecond field was not the
// sub-second part of the second field, it was the sub-second part of an
// unrelated clock. Their second boundaries did not coincide, so a caller
// reassembling tv_sec*1e6 + tv_usec could see time go BACKWARDS across a
// tick. #594 had to repair exactly this desync once already, when tv_sec and
// tv_usec were derived from two different divisors of timer_ticks.
//
// Now BOTH fields are the same 64-bit epoch-microsecond reading, split. They
// cannot desync, because there is nothing to desync: it is one number.
//
// TIMEZONE IS DELIBERATELY NOT APPLIED HERE. POSIX gettimeofday() is UTC. The
// offset belongs to the presentation layer (libc/tz.c, THE one place that
// applies it in this tree). Applying it here would double-count for every
// caller that then formats through tz.c, and would silently corrupt every
// absolute-instant consumer: TOTP in /apps/mfa, audit timestamps in
// libc/aicap.c, and file mtimes.
//
// IF THE KERNEL HAS NO CALENDAR, sys_realtime_us() returns 0, and we report
// the epoch rather than substituting an uptime. That is deliberate: an uptime
// in a time_t is a wrong value that LOOKS right, which is the entire defect
// #113 exists to remove. Zero at least fails loudly the moment anyone formats it.
int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv) return -1;
    long long us = sys_realtime_us();
    if (us < 0) us = 0;
    tv->tv_sec  = (time_t)(us / 1000000LL);
    tv->tv_usec = (long)(us % 1000000LL);
    return 0;
}

int clock_gettime(clockid_t clk, struct timespec *ts) {
    if (!ts) { errno = EINVAL; return -1; }
    if (clk == CLOCK_REALTIME) {
        // Calendar time. Steps when SNTP corrects the clock, by definition.
        long long us = sys_realtime_us();
        if (us < 0) us = 0;
        ts->tv_sec  = (long)(us / 1000000LL);
        ts->tv_nsec = (long)((us % 1000000LL) * 1000LL);
    } else {
        // MONOTONIC / CPUTIME: milliseconds since boot, which is what a
        // deadline wants and what must NOT step when the wall clock does.
        unsigned long ms = uptime_ms();
        ts->tv_sec  = (long)(ms / 1000UL);
        ts->tv_nsec = (long)((ms % 1000UL) * 1000000UL);
    }
    return 0;
}

int clock_getres(clockid_t clk, struct timespec *ts) {
    (void)clk;
    if (ts) { ts->tv_sec = 0; ts->tv_nsec = 1000000L; } // 1 ms resolution
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    if (!req) { errno = EINVAL; return -1; }
    unsigned long ms = (unsigned long)req->tv_sec * 1000UL + (unsigned long)(req->tv_nsec / 1000000L);
    if (ms == 0 && (req->tv_sec || req->tv_nsec)) ms = 1;
    sys_sleep((uint32_t)ms);
    return 0;
}

// stdio helpers missing from the base libc.
int getc(FILE *f) { return fgetc(f); }
void clearerr(FILE *f) { (void)f; }  // no error-flag setter in this libc; no-op
