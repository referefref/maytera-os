// dirent.c - userland directory traversal via SYS_READDIR
// Uses the kernel fd-based convention: open dir with SYS_OPEN, read with SYS_READDIR.
#include "dirent.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"
#include "syscall.h"

// Kernel dirent struct: must match the typedef in kernel/proc/syscall.c sys_readdir
typedef struct {
    char     name[256];
    uint32_t type;    // 0 = regular file, 1 = directory
    uint32_t size;    // file size in bytes
} k_dirent_t;

struct DIR {
    int          fd;   // open fd for this directory (from SYS_OPEN)
    struct dirent cur; // buffer for the current entry
};

DIR *opendir(const char *name) {
    // Open the directory path with O_RDONLY (flags=0)
    int fd = (int)syscall2(SYS_OPEN, (long)name, 0L);
    if (fd < 0) { errno = ENOENT; return 0; }
    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d) {
        syscall1(SYS_CLOSE, (long)fd);
        errno = ENOMEM;
        return 0;
    }
    d->fd = fd;
    return d;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp) return 0;
    k_dirent_t kd;
    // Kernel: syscall2(SYS_READDIR, fd, entry_buf) -> 0=entry, nonzero=done/error
    long r = syscall2(SYS_READDIR, (long)dirp->fd, (long)&kd);
    if (r != 0) return 0;
    // Copy name into the dirent buffer
    int i = 0;
    while (kd.name[i] && i < 255) { dirp->cur.d_name[i] = kd.name[i]; i++; }
    dirp->cur.d_name[i] = 0;
    dirp->cur.d_ino    = 1;
    dirp->cur.d_off    = 0;
    dirp->cur.d_reclen = sizeof(struct dirent);
    dirp->cur.d_type   = (kd.type == 1) ? DT_DIR : DT_REG;
    return &dirp->cur;
}

int closedir(DIR *dirp) {
    if (!dirp) { errno = EBADF; return -1; }
    syscall1(SYS_CLOSE, (long)dirp->fd);
    free(dirp);
    return 0;
}

void rewinddir(DIR *dirp) {
    // Seeking back to the start of a directory requires reopening the fd.
    // This stub exists for API compatibility; callers that need restart
    // should closedir() and opendir() again.
    (void)dirp;
}
