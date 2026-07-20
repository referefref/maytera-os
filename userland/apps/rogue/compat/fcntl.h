
#ifndef COMPAT_FCNTL_H
#define COMPAT_FCNTL_H

#include "../../../libc/syscall.h"

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

static inline int open(const char *path, int flags, ...) {
    return (int)syscall2(SYS_OPEN, (long)path, (long)flags);
}

#endif
