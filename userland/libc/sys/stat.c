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

// #120: THIS FUNCTION USED TO CALL NO SYSCALL AT ALL.
//
// It was a SEEK_CUR + SEEK_END + restore to find the size, and then:
//
//     st->st_mode  = S_IFREG | 0644;
//     st->st_nlink = 1;
//
// Everything else - st_dev, st_ino, st_uid, st_gid, st_rdev, st_blksize,
// st_blocks and all three timestamps - stayed at the memset zero. So every
// caller got a plausible struct describing a regular file with fixed
// permissions, whatever the descriptor actually referred to: a DIRECTORY
// reported S_IFREG, a pipe reported S_IFREG, a socket reported S_IFREG.
//
// And it returned 0 UNCONDITIONALLY. On a closed or negative fd both seeks fail,
// the `if` is skipped, and it still reported success with size 0 and mode
// S_IFREG|0644. There was no input for which it returned -1, so no caller could
// detect any of this. That is the shape of this project's defining defect: a
// call that succeeds, fills a struct, and is believed.
//
// SYS_FSTAT (101) now answers from the kernel, which is the only side of the
// boundary that knows what an fd refers to. Ring 3 holds an integer and there is
// no F_GETPATH in this kernel's fcntl, so no libc-only fix was possible.
//
// The -1 fallback is the SAME discipline stat() uses above and for the same
// reason: -1 is what an unimplemented syscall returns from the dispatcher's
// default case, so a userland binary built against this libc still runs on an
// older kernel. Any DEFINITE error (-9 EBADF, -14 EFAULT) is propagated, never
// masked. Note what the fallback does NOT do any more: it does not invent
// st_mode. An old kernel yields size-only, with mode 0, and mode 0 makes
// S_ISREG/S_ISDIR/S_ISFIFO all FALSE - "unknown", which is true, rather than
// "regular file", which was a guess.
int fstat(int fd, struct stat *st) {
    if (!st) { errno = EFAULT; return -1; }
    memset(st, 0, sizeof(*st));
    long r = syscall2(SYS_FSTAT, fd, (long)st);
    if (r == 0) return 0;
    if (r != -1) { errno = (int)-r; return -1; }   // definite error, do not mask
    // Legacy kernel only.
    long cur = syscall3(SYS_SEEK, fd, 0, 1);  // SEEK_CUR
    long end = syscall3(SYS_SEEK, fd, 0, 2);  // SEEK_END
    if (cur < 0 || end < 0) { errno = EBADF; return -1; }
    syscall3(SYS_SEEK, fd, cur, 0);           // restore
    st->st_size = end;
    st->st_nlink = 1;
    return 0;
}

// #120: lstat() IS stat(), and that is CORRECT here rather than a shortcut.
//
// lstat differs from stat only in that it reports the SYMLINK rather than what
// the link points at. MayteraOS has no symbolic links on either local
// filesystem: FAT has no such entry type, and this kernel's ext2 driver neither
// creates (there is no symlink()) nor resolves (ext2_resolve_path walks
// directory entries only) an S_IFLNK inode. So there is nothing for the two
// calls to disagree about and no S_IFLNK this kernel can ever report.
//
// The distinction is REAL, not absent: if an ext2 volume produced elsewhere
// carries an S_IFLNK inode, sys_stat_path reports its i_mode verbatim - the
// type bits come straight off the disk - so S_ISLNK would be true, for BOTH
// stat and lstat, because nothing here follows the link either. That makes this
// kernel's stat() behave like lstat(), not the other way round, and it is the
// safe direction: it never silently follows a link a caller asked not to follow.
//
// When symlinks are implemented, THIS is the line that must change, and the
// change is stat() gaining resolution, not lstat() losing it.
int lstat(const char *path, struct stat *st) {
    return stat(path, st);
}
