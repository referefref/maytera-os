// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// ftw.h - file tree walk for MayteraOS userland.
//
// Built on the shipping opendir()/readdir()/closedir() in dirent.c and on
// stat(), which is what every other directory consumer in this tree uses.
// There is no new syscall path here and no private copy of one.
//
// WHAT IS HONOURED, EXACTLY:
//   FTW_DEPTH  yes. Post-order: contents first, then the directory as FTW_DP.
//   FTW_PHYS   accepted, and it is a no-op HERE ONLY because MayteraOS has no
//              symbolic links on either local filesystem, so lstat() and
//              stat() cannot differ and FTW_SL can never be reported.
//   FTW_MOUNT  #120: ACCEPTED and implemented (it used to be refused with
//              EINVAL). A directory whose st_dev differs from the root's is
//              reported to the callback and not descended into.
//              BUT READ THIS BEFORE RELYING ON IT: the branch is UNEXERCISED on
//              the shipping layout and is correct by inspection only. /EFI is
//              on the FAT ESP and readdir("/") enumerates the ext2 root, which
//              has no EFI entry, so no walk from "/" can reach a boundary to
//              stop at. Measured on build 1905: with and without the flag,
//              nftw("/") visits the same 4212 entries. st_dev itself IS real
//              and differs (/EFI is 1, /APPS is 2, by path) - it is the
//              enumeration, not the field, that cannot see across.
//
// WHAT IS REFUSED, with -1 and errno EINVAL, rather than quietly ignored:
//   FTW_CHDIR  would have to chdir() into each directory before calling fn,
//              because the whole point of the flag is that fn may then use
//              fpath + base as a RELATIVE name. Ignoring it would leave that
//              relative name resolving against the wrong directory, which
//              opens the wrong file and reports success. Refusing is loud.
// (FTW_MOUNT was in this refused list until #120. It is now HONOURED; see
// above. The reason it was refused - "this kernel's stat() zero-fills st_dev
// for every file on every filesystem" - stopped being true at #115.)
//
// OTHER LIMITS, stated rather than discovered:
//   * FTW_SL and FTW_SLN are defined so callers' switch statements compile.
//     They are NEVER returned: there are no symlinks to return them for.
//   * A path longer than PATH_MAX (1024, the kernel's own SC_PATH_MAX) stops
//     the walk with -1 / ENAMETOOLONG. Longer paths cannot be opened anyway.
//   * The walk is recursive and depth is capped at FTW_MAX_DEPTH. Past that it
//     stops with -1 / ELOOP. It does not silently skip the deep part.
//   * #120: THIS ENTRY USED TO SAY the struct stat handed to fn has
//     st_atime/st_mtime/st_ctime == 0 for every file, "because this kernel's
//     stat() does not report timestamps", and told callers not to sort by
//     mtime. #115 made all three REAL on all four backends, so that advice was
//     not merely stale, it was steering callers away from a working field.
//     The rule that IS still true is narrower and is the kernel's own
//     convention: 0 means UNKNOWN, not 1970-01-01. A volume written before #115
//     has zeros in its inodes and they stay zero. Sort by mtime if you like,
//     but treat 0 as unknown rather than as the oldest file.
//   * nopenfd must be at least 1. It is otherwise irrelevant: each directory
//     is read fully and CLOSED before descending, so exactly one directory
//     descriptor is open at any moment however deep the tree is.
#ifndef LIBC_FTW_H
#define LIBC_FTW_H

#include "sys/stat.h"

// typeflag values passed to the callback.
#define FTW_F   0   // regular file (or anything that is not a directory)
#define FTW_D   1   // directory, reported BEFORE its contents
#define FTW_DNR 2   // directory that could not be opened; not descended into
#define FTW_NS  3   // stat() failed; the struct stat is zeroed, not valid
#define FTW_SL  4   // symbolic link. NEVER PRODUCED on MayteraOS.
#define FTW_DP  5   // directory, reported AFTER its contents (FTW_DEPTH only)
#define FTW_SLN 6   // dangling symbolic link. NEVER PRODUCED on MayteraOS.

// nftw() flags.
#define FTW_PHYS  1   // do not follow symlinks (no-op here: none exist)
#define FTW_MOUNT 2   // #120: honoured - do not cross a filesystem boundary
#define FTW_CHDIR 4   // REFUSED with EINVAL, see the header note
#define FTW_DEPTH 8   // post-order walk

// Recursion cap. Hitting it is an error (-1 / ELOOP), never a silent skip.
#define FTW_MAX_DEPTH 256

struct FTW {
    int base;    // offset of the basename within fpath
    int level;   // depth below the starting directory; 0 for it
};

// Both return 0 when the whole tree was walked, the callback's value if the
// callback returned non-zero (the walk stops at once), or -1 with errno set.
int ftw(const char *dirpath,
        int (*fn)(const char *fpath, const struct stat *sb, int typeflag),
        int nopenfd);

int nftw(const char *dirpath,
         int (*fn)(const char *fpath, const struct stat *sb, int typeflag,
                   struct FTW *ftwbuf),
         int nopenfd, int flags);

#endif // LIBC_FTW_H
