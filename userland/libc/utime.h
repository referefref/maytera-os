// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// utime.h - utime()/utimes() for MayteraOS userland.
//
// #115 (local 120): THESE WORK NOW. This header used to open with "READ THIS
// BEFORE YOU USE THEM: BOTH ALWAYS FAIL, with errno ENOSYS", because no syscall
// in this kernel could set a file timestamp and none could read one back
// either: sys_stat_path() zero-filled st_atime/st_mtime/st_ctime on every
// branch. Both halves now exist (SYS_UTIME sets; sys_stat_path reports).
//
// WHAT YOU STILL NEED TO KNOW, because none of it is hidden:
//
//  * PRECISION IS WHOLE SECONDS. utimes() takes a struct timeval and DISCARDS
//    the microseconds: ext2 rev-1 inodes store whole seconds, and a FAT
//    directory entry stores the modify time in TWO-second units.
//
//  * ON FAT, ACCESS TIME IS A DATE. The FAT directory entry has an access DATE
//    field and no access time-of-day, so an atime set on the ESP lands at
//    midnight UTC of that day. That is the on-disk format, not a shortcut.
//
//  * SMB AND NFS REFUSE with ENOSYS. Both protocols can set times (SMB2
//    SET_INFO, NFSv3 SETATTR) and neither client here implements it. Returning
//    0 for a write that never left the machine is the defect class #115 exists
//    to remove, so they say so instead.
//
//  * A NULL `times` means "now", and NOW IS DECIDED BY THE KERNEL. Do not
//    build one from time(): on this OS time() and gettimeofday() return
//    SECONDS SINCE BOOT (#113), so a caller-supplied "now" would stamp files
//    with 1970 plus the uptime. The kernel reads the RTC.
//
//  * READING BACK: st_mtime == 0 means the filesystem does not know, NOT
//    1970-01-01. Nothing here can produce a legitimate epoch 0.
#ifndef LIBC_UTIME_H
#define LIBC_UTIME_H

#include "time.h"        // time_t
#include "sys/time.h"    // struct timeval

// The two sentinels the kernel understands (kernel/proc/syscall.h). They are
// declared HERE, next to the functions that take them, so there is one
// definition rather than one per caller.
//   UTIME_KEEP  leave this timestamp exactly as it is on disk.
//   UTIME_NOW   set it from the KERNEL's wall clock. Use this rather than
//               time(), which returns seconds since BOOT on this OS (#113).
#define UTIME_KEEP  (-1L)
#define UTIME_NOW   (-2L)

struct utimbuf {
    time_t actime;    // access time
    time_t modtime;   // modification time
};

// 0 on success, -1 with errno on failure. `times` == NULL means "now" (resolved
// by the kernel). ENOSYS on SMB/NFS paths; EACCES if the caller cannot write
// the file or traverse to it.
int utime(const char *path, const struct utimbuf *times);

// As utime(). times[0] is atime, times[1] mtime; microseconds are discarded.
int utimes(const char *path, const struct timeval times[2]);

#endif // LIBC_UTIME_H
