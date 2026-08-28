// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// utime.c - utime()/utimes(). The reasoning is in utime.h.
//
// #115 (local 120): THESE USED TO RETURN ENOSYS ALWAYS, because no syscall in
// this kernel could set a timestamp and none could read one back either. Both
// halves now exist: SYS_UTIME sets, and sys_stat_path() reports what was set.
//
// These two live ALONE in this translation unit on purpose. A port that
// supplies its own utime() (the CPython port does, in
// userland/apps/python/port/src-cpython/compat.c) must not drag a second
// definition in out of libc.a and fail to link; keeping the pair isolated
// means the archive member is only ever pulled when nothing else defines them.
#include "utime.h"
#include "errno.h"
#include "syscall.h"

// Map a kernel return code to errno and the POSIX -1. The kernel returns
// negative errno values; -38 (ENOSYS) is what SMB and NFS paths give, because
// neither client implements a set-times request and reporting success for a
// write that never left the machine would be the exact lie #115 removed.
static int utime_rc(long r) {
    if (r == 0) return 0;
    errno = (r < 0) ? (int)-r : EIO;
    return -1;
}

int utime(const char *path, const struct utimbuf *times) {
    if (!path) { errno = EINVAL; return -1; }
    // POSIX: a NULL `times` means "now". It is resolved BY THE KERNEL, via the
    // UTIME_NOW sentinel, and not by calling time() here - time() returns
    // SECONDS SINCE BOOT on this OS (#113), so sending it would stamp every
    // file with 1970 plus the uptime. The kernel has the RTC; userland does
    // not have a calendar clock to send.
    long a = times ? (long)times->actime  : UTIME_NOW;
    long m = times ? (long)times->modtime : UTIME_NOW;
    return utime_rc(syscall3(SYS_UTIME, (long)path, a, m));
}

int utimes(const char *path, const struct timeval times[2]) {
    if (!path) { errno = EINVAL; return -1; }
    // Sub-second precision is DISCARDED, not honoured: ext2 rev-1 inodes and
    // FAT directory entries both store whole seconds (FAT stores two-second
    // granularity for the modify time and a DATE ONLY for access). Truncating
    // and saying so is correct; silently accepting a microsecond field the
    // filesystem cannot hold would make utimes() look more precise than it is.
    long a = times ? (long)times[0].tv_sec : UTIME_NOW;
    long m = times ? (long)times[1].tv_sec : UTIME_NOW;
    return utime_rc(syscall3(SYS_UTIME, (long)path, a, m));
}
