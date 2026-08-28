// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/stat.h
#ifndef LIBC_SYS_STAT_H
#define LIBC_SYS_STAT_H

#include "../types.h"
#include "types.h"   // mode_t (task #568: sys/stat.h used mode_t without pulling in sys/types.h)

#define S_IFMT   0xF000
#define S_IFREG  0x8000
#define S_IFDIR  0x4000
#define S_IFCHR  0x2000
#define S_IFIFO  0x1000
// #120: SYS_FSTAT can now report these two, so callers need to be able to test
// for them. S_IFLNK is declared for completeness and because an ext2 volume
// written elsewhere can carry one (the kernel reports i_mode verbatim); nothing
// in MayteraOS creates one. See the lstat() note in sys/stat.c.
#define S_IFSOCK 0xC000
#define S_IFLNK  0xA000
#define S_IFBLK  0x6000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)

struct stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int  st_mode;
    unsigned int  st_nlink;
    unsigned int  st_uid;
    unsigned int  st_gid;
    unsigned long st_rdev;
    long          st_size;
    long          st_blksize;
    long          st_blocks;
    unsigned long st_atime;
    unsigned long st_mtime;
    unsigned long st_ctime;
};

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int lstat(const char *path, struct stat *st);

#endif
