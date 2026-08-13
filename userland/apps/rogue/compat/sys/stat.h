
#ifndef COMPAT_SYS_STAT_H
#define COMPAT_SYS_STAT_H

#include "types.h"

struct stat {
    mode_t  st_mode;
    off_t   st_size;
    uid_t   st_uid;
    long    st_nlink;
    long    st_mtime;
};

/* MayteraOS port note: S_IFMT/S_IFREG/S_IFDIR and the S_IS* macros were
 * missing from this shim; mach_dep.c's is_symlink() needs S_IFMT/S_IFREG
 * (BSD Rogue upstream, unmodified). Values match the traditional Unix
 * st_mode format bits also used elsewhere in the MayteraOS libc. */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000
#define S_ISLNK(m) (((m)&S_IFMT)==S_IFLNK)
#define S_ISREG(m) (((m)&S_IFMT)==S_IFREG)
#define S_ISDIR(m) (((m)&S_IFMT)==S_IFDIR)

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
