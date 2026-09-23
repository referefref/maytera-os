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

// POSIX permission and special mode bits. Added for the libarchive mports port
// (#745): archive_entry_strmode.c and archive_write_set_format_pax.c need the
// setuid/setgid/sticky bits and the rwx triples. Purely additive standard
// <sys/stat.h> macros; no existing caller is affected.
// POSIX permission/mode bits, added for the libedit port (#745): history.c
// opens its history file with S_IRUSR|S_IWUSR. Standard octal values.
#define S_ISUID  04000
#define S_ISGID  02000
#define S_ISVTX  01000
#define S_IRWXU  00700
#define S_IRUSR  00400
#define S_IWUSR  00200
#define S_IXUSR  00100
#define S_IRWXG  00070
#define S_IRGRP  00040
#define S_IWGRP  00020
#define S_IXGRP  00010
#define S_IRWXO  00007
#define S_IROTH  00004
#define S_IWOTH  00002
#define S_IXOTH  00001

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

// umask(). Added for the Angband port (userland/ports/angband, #745 Tier 3
// #15): main.c calls umask(0) unconditionally on any UNIX build before
// create_needed_dirs() so its own directory/save-file creation is not
// clamped by an inherited mask.
//
// HONEST LIMIT: this is process-local BOOKKEEPING ONLY. MayteraOS's mkdir()/
// open(O_CREAT) syscalls apply the caller's explicit mode argument directly
// (kernel/proc/syscall.c) with no per-process mask consulted anywhere in the
// path, so there is nothing for a "real" umask to mask. Storing and
// returning the previous value keeps the POSIX contract (a caller that reads
// its own umask back gets what it set) without pretending to enforce a
// kernel behaviour that does not exist.
mode_t umask(mode_t mask);

#endif
