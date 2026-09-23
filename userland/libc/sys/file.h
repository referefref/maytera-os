// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/file.h - flock() for MayteraOS userland: BSD advisory whole-file locks.
//
// As of #404 Stage 6 MayteraOS HAS an advisory lock manager in the kernel
// (POSIX fcntl record locks via fcntl(F_SETLK/F_SETLKW/F_GETLK) plus BSD
// flock()), so flock() is a real, working call now. It takes a whole-file
// shared (LOCK_SH) or exclusive (LOCK_EX) lock keyed on the open file, blocks
// until grantable unless LOCK_NB is given (then it returns -1/EAGAIN on
// conflict), releases on LOCK_UN, and is released automatically on close of the
// fd and on process exit. The lock is ADVISORY: it excludes only other callers
// that also lock, exactly like POSIX. flock and fcntl locks are independent
// lock spaces (they do not conflict with each other), matching Linux.
#ifndef LIBC_SYS_FILE_H
#define LIBC_SYS_FILE_H

#define LOCK_SH 1   // shared lock
#define LOCK_EX 2   // exclusive lock
#define LOCK_NB 4   // do not block (or with LOCK_SH/LOCK_EX)
#define LOCK_UN 8   // unlock

// ALWAYS returns -1 with errno == ENOSYS. See the note above.
int flock(int fd, int operation);

#endif // LIBC_SYS_FILE_H
