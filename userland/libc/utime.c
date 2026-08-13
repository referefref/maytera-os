// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// utime.c - utime()/utimes(), which refuse. The reasoning is in utime.h.
//
// These two live ALONE in this translation unit on purpose. A port that
// supplies its own utime() (the CPython port does, in
// userland/apps/python/port/src-cpython/compat.c) must not drag a second
// definition in out of libc.a and fail to link; keeping the pair isolated
// means the archive member is only ever pulled when nothing else defines them.
#include "utime.h"
#include "errno.h"

int utime(const char *path, const struct utimbuf *times) {
    (void)path;
    (void)times;
    errno = ENOSYS;
    return -1;
}

int utimes(const char *path, const struct timeval times[2]) {
    (void)path;
    (void)times;
    errno = ENOSYS;
    return -1;
}
