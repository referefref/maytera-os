// posixextra.c - #359 Phase 2 filesystem/time/misc libc functions that the
// CPython filesystem stdlib (and other POSIX-y userland) needs. Honest, small
// implementations over the existing MayteraOS syscalls; where the kernel has no
// backing primitive the function is a clearly-commented best-effort stub.
#include "unistd.h"
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

// ftruncate()/truncate(): the kernel FAT/ext2 fd layer has no truncate primitive
// (files are rewritten whole on close). Best-effort no-op so callers that only
// pre-size or shrink temp files keep working; returns success.
int ftruncate(int fd, long length) { (void)fd; (void)length; return 0; }
int truncate(const char *path, long length) { (void)path; (void)length; return 0; }

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

// gettimeofday(): tv_sec is the seconds-resolution wall clock (SYS_TIME); the
// microsecond field is filled from the monotonic ms tick so sub-second deltas
// are usable even though the wall clock itself has no sub-second component.
int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv) return -1;
    unsigned long ms = uptime_ms();
    tv->tv_sec  = (time_t)sys_time();
    tv->tv_usec = (long)((ms % 1000UL) * 1000UL);
    return 0;
}

int clock_gettime(clockid_t clk, struct timespec *ts) {
    if (!ts) { errno = EINVAL; return -1; }
    unsigned long ms = uptime_ms();
    if (clk == CLOCK_REALTIME) {
        ts->tv_sec  = (long)sys_time();
        ts->tv_nsec = (long)((ms % 1000UL) * 1000000UL);
    } else {
        // MONOTONIC / CPUTIME: monotonic milliseconds since boot.
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
