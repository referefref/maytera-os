// sys/stat.c - stub implementation using open/seek
#include "stat.h"
#include "../syscall.h"
#include "../errno.h"
#include "../string.h"

int stat(const char *path, struct stat *st) {
    // SYS_STAT fills *st directly (size + type) by reading the directory
    // entry, with no cluster-chain walk. Falls back to open()+SEEK_END only
    // if the kernel reports the call unsupported.
    long r = syscall2(SYS_STAT, (long)path, (long)st);
    if (r == 0) return 0;
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
