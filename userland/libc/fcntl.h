// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// fcntl.h - POSIX file control for MayteraOS userland
#ifndef LIBC_FCNTL_H
#define LIBC_FCNTL_H

// open flags
#define O_RDONLY      0x0000
#define O_WRONLY      0x0001
#define O_RDWR        0x0002
#define O_ACCMODE     0x0003
#define O_CREAT       0x0040
#define O_EXCL        0x0080
#define O_NOCTTY      0x0100
#define O_TRUNC       0x0200
#define O_APPEND      0x0400
#define O_NONBLOCK    0x0800
#define O_DIRECTORY   0x10000
#define O_CLOEXEC     0x80000

// fcntl cmds
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030

// Advisory record locking (POSIX fcntl locks). As of #404 Stage 6 these are
// IMPLEMENTED: fcntl.c forwards them to SYS_FCNTL, and the kernel advisory lock
// manager (kernel/proc/advlock.c over rustkern/advlock.rs) applies real
// byte-range record locks. F_GETLK tests (fills the flock with a conflicting
// holder, or l_type=F_UNLCK if the range is free), F_SETLK is non-blocking
// (returns -1/EAGAIN on conflict), F_SETLKW blocks until grantable (and returns
// -1/EDEADLK on a detected deadlock, -1/EINTR if interrupted). F_RDLCK is
// shared, F_WRLCK exclusive, F_UNLCK releases; l_whence/l_start/l_len select the
// byte range (l_len==0 means to EOF). A process's fcntl locks are released when
// it closes ANY fd on the file and when it exits, per POSIX. The Angband and
// moon-buggy ports (which build a struct flock and call fcntl) now get real
// locking instead of the old -1/ENOSYS.
#define F_RDLCK  0
#define F_WRLCK  1
#define F_UNLCK  2
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7

// NOTE: raw 64-bit int fields (not off_t/pid_t) so fcntl.h stays self-sufficient
// and does not pull types.h (and its standard `bool`/`off_t`) into consumers
// like the compositor (legacy `typedef int bool`) and app compat shims
// (off_t=long). These locks are advisory/source-compat only (see above).
struct flock {
    short  l_type;
    short  l_whence;
    long long l_start;
    long long l_len;
    int    l_pid;
};

#define FD_CLOEXEC      1

// open() declared in stdlib.h
int creat(const char *path, int mode);
int fcntl(int fd, int cmd, ...);

#endif
