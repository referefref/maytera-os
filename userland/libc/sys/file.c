// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/file.c - flock(): BSD advisory whole-file locking.
//
// As of #404 Stage 6 the kernel HAS an advisory lock manager (POSIX fcntl
// record locks + BSD flock), so this is a real wrapper now, no longer the
// honest refusal it used to be. It forwards to SYS_FLOCK, which applies a
// whole-file shared (LOCK_SH) or exclusive (LOCK_EX) lock keyed on the open
// file, blocks until grantable unless LOCK_NB is set, and releases on LOCK_UN,
// on close of the fd, and on process exit. See kernel/proc/advlock.c.
#include "file.h"
#include "../syscall.h"
#include "../errno.h"

int flock(int fd, int operation) {
    long r = syscall2(SYS_FLOCK, (long)fd, (long)operation);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}
