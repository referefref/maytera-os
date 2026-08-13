// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/stat.c - stub implementation using open/seek
#include "stat.h"
#include "../syscall.h"
#include "../errno.h"
#include "../string.h"

int stat(const char *path, struct stat *st) {
    // SYS_STAT fills *st directly (size + type) by reading the directory
    // entry, with no cluster-chain walk.
    //
    // #745: THE FALLBACK USED TO SWALLOW EVERY FAILURE, INCLUDING A DENIAL.
    //
    // The comment here used to say the fallback ran "only if the kernel reports
    // the call unsupported". It did not: it ran on ANY non-zero return. So when
    // SYS_STAT gained a permission check, a refused stat fell through to
    // open()+SEEK_END and, if open happened to succeed, returned 0 with a
    // FABRICATED st_mode of S_IFREG | 0644. A denial became a silent success
    // reporting the wrong type, which is worse than either outcome on its own:
    // the caller cannot see the refusal, and a DIRECTORY comes back as a
    // regular file of size 0, which is exactly the _path_isdir breakage that
    // made filesystem imports fail before the ext2 branch was added to
    // sys_stat_path.
    //
    // A control that Ring 3 cannot observe is not a control, and this project
    // has the scar tissue to prove it (blame.md: verify the artifact, not the
    // description). So DEFINITE errors are now propagated and only the legacy
    // ambiguous -1 still falls back. -1 is what both "no such file" and an
    // unimplemented syscall return, so preserving the fallback for it keeps
    // every existing caller behaving exactly as before, while -13 (EACCES) and
    // -14 (EFAULT) reach the caller as errno, which is the whole point.
    long r = syscall2(SYS_STAT, (long)path, (long)st);
    if (r == 0) return 0;
    if (r != -1) { errno = (int)-r; return -1; }   // #745: definite error, do not mask
    int fd = (int)syscall2(SYS_OPEN, (long)path, 0);
    if (fd < 0) { errno = -fd; return -1; }
    long sz = syscall3(SYS_SEEK, fd, 0, 2);  // SEEK_END
    syscall1(SYS_CLOSE, fd);
    memset(st, 0, sizeof(*st));
    st->st_size  = (sz >= 0) ? sz : 0;
    st->st_mode  = S_IFREG | 0644;
    st->st_nlink = 1;
    return 0;
}

int fstat(int fd, struct stat *st) {
    memset(st, 0, sizeof(*st));
    long cur = syscall3(SYS_SEEK, fd, 0, 1);  // SEEK_CUR
    long end = syscall3(SYS_SEEK, fd, 0, 2);  // SEEK_END
    if (cur >= 0 && end >= 0) {
        syscall3(SYS_SEEK, fd, cur, 0);       // restore
        st->st_size = end;
    }
    st->st_mode  = S_IFREG | 0644;
    st->st_nlink = 1;
    return 0;
}

int lstat(const char *path, struct stat *st) {
    return stat(path, st);
}
