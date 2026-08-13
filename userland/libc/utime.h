// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// utime.h - utime()/utimes() for MayteraOS userland.
//
// READ THIS BEFORE YOU USE THEM: BOTH ALWAYS FAIL, with errno ENOSYS.
//
// There is no syscall in this kernel that sets a file's timestamps. There is
// not even one that READS them: sys_stat_path() zero-fills the struct and
// fills in only mode, nlink, size, blksize and blocks, so st_atime, st_mtime
// and st_ctime come back as 0 for every file on every filesystem. A utime()
// that returned 0 would therefore be claiming to have set a field that does
// not exist and that nothing can read back, which is unfalsifiable from the
// caller's side and exactly the kind of quiet lie this libc will not ship.
//
// They are declared, rather than left out, so a port that includes <utime.h>
// compiles and so a port that CHECKS the return finds out immediately. A
// build/install tool that preserves mtimes will report a real failure here
// instead of appearing to work.
#ifndef LIBC_UTIME_H
#define LIBC_UTIME_H

#include "time.h"        // time_t
#include "sys/time.h"    // struct timeval

struct utimbuf {
    time_t actime;    // access time
    time_t modtime;   // modification time
};

// ALWAYS returns -1 with errno == ENOSYS. See the note above.
int utime(const char *path, const struct utimbuf *times);

// ALWAYS returns -1 with errno == ENOSYS. times[0] is atime, times[1] mtime.
int utimes(const char *path, const struct timeval times[2]);

#endif // LIBC_UTIME_H
