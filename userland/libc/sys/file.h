// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/file.h - flock() for MayteraOS userland.
//
// READ THIS BEFORE YOU USE IT: flock() ALWAYS FAILS, with errno ENOSYS, and it
// will keep failing until this OS grows a locking primitive.
//
// MayteraOS has NO file locking of any kind. Not advisory, not mandatory, not
// fcntl(F_SETLK), not a lock file: even O_EXCL is unimplemented on both local
// filesystems (see kernel/fs/fat_vfs.c), so the open(O_CREAT|O_EXCL) trick
// people fall back on SUCCEEDS ON AN EXISTING FILE and locks nothing either.
//
// So this function does the only honest thing available: it refuses. It is
// declared, rather than left out of the tree, for two reasons. A port that
// includes <sys/file.h> for the LOCK_* constants compiles, and a port that
// actually calls flock() and checks the return - which is the only way to use
// a lock correctly - finds out at once and loudly that it has no mutual
// exclusion here. What it must never do is return 0, because a caller that
// believes it holds an exclusive lock and does not is the worst of the three
// outcomes by a distance.
#ifndef LIBC_SYS_FILE_H
#define LIBC_SYS_FILE_H

#define LOCK_SH 1   // shared lock
#define LOCK_EX 2   // exclusive lock
#define LOCK_NB 4   // do not block (or with LOCK_SH/LOCK_EX)
#define LOCK_UN 8   // unlock

// ALWAYS returns -1 with errno == ENOSYS. See the note above.
int flock(int fd, int operation);

#endif // LIBC_SYS_FILE_H
