// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/param.h - the BSD grab-bag of limits and small macros.
//
// Header only, so nothing here can be missing at link time.
//
// PATH_MAX/MAXPATHLEN is 1024 because that is SC_PATH_MAX in
// kernel/proc/syscall_path.h, the buffer every path-taking syscall bounces a
// Ring-3 string into. It is the real limit, not a conventional number: hand a
// longer path to open()/stat() and the bounce fails. NAME_MAX is 255 because
// that is the size of the name field in the kernel dirent (sc_dirent_t) that
// SYS_READDIR fills.
#ifndef LIBC_SYS_PARAM_H
#define LIBC_SYS_PARAM_H

#include "../types.h"
#include "../endian.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef MAXNAMLEN
#define MAXNAMLEN NAME_MAX
#endif

// Bits in a byte, and the classic bitmap helpers built on it.
#ifndef NBBY
#define NBBY 8
#endif

// Standard file descriptors, for code that includes only this header.
#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

// Both arguments are evaluated twice. That is what the BSD macros have always
// done and what ported code expects; do not pass an expression with a side
// effect. Guarded because plenty of ported code defines its own first.
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef howmany
#define howmany(x, y)   (((x) + ((y) - 1)) / (y))
#endif
#ifndef roundup
#define roundup(x, y)   ((((x) + ((y) - 1)) / (y)) * (y))
#endif
#ifndef rounddown
#define rounddown(x, y) (((x) / (y)) * (y))
#endif
#ifndef powerof2
#define powerof2(x)     ((((x) - 1) & (x)) == 0)
#endif

#endif // LIBC_SYS_PARAM_H
