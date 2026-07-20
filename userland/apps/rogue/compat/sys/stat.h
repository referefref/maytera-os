
#ifndef COMPAT_SYS_STAT_H
#define COMPAT_SYS_STAT_H

#include "types.h"

struct stat {
    mode_t  st_mode;
    off_t   st_size;
    uid_t   st_uid;
};

#define S_IFLNK  0120000
#define S_ISLNK(m) (((m)&0170000)==S_IFLNK)

static inline int stat(const char *path, struct stat *buf) {
    (void)path; (void)buf; return -1;
}
static inline int lstat(const char *path, struct stat *buf) {
    (void)path; (void)buf; return -1;
}
static inline int chmod(const char *path, mode_t mode) {
    (void)path; (void)mode; return 0;
}

#endif
