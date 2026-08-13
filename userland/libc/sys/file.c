// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/file.c - flock(), which refuses. The whole reasoning is in file.h.
#include "file.h"
#include "../errno.h"

int flock(int fd, int operation) {
    (void)fd;
    (void)operation;
    // Not "unimplemented yet" in the sense of a missing wrapper: there is no
    // lock manager in the kernel to wrap. Returning success here would hand
    // the caller an exclusive lock that does not exist.
    errno = ENOSYS;
    return -1;
}
