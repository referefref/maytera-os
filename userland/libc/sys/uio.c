// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/uio.c - readv()/writev() as a loop over read()/write().
// The atomicity caveat is documented on the declarations in uio.h. Read it.
#include "uio.h"
#include "../unistd.h"
#include "../errno.h"

#define UIO_SSIZE_MAX 0x7fffffffffffffffL

// Common validation: POSIX requires EINVAL for a bad iovcnt and for an
// iov_len sum that will not fit in ssize_t, and it requires that check to
// happen BEFORE anything is transferred.
static int uio_check(const struct iovec *iov, int iovcnt) {
    size_t sum = 0;
    int i;
    if (iovcnt < 0 || iovcnt > IOV_MAX) { errno = EINVAL; return -1; }
    if (iovcnt > 0 && iov == 0)         { errno = EINVAL; return -1; }
    for (i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len > (size_t)UIO_SSIZE_MAX - sum) { errno = EINVAL; return -1; }
        sum += iov[i].iov_len;
    }
    return 0;
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t total = 0;
    int i;

    if (uio_check(iov, iovcnt) != 0) return -1;

    for (i = 0; i < iovcnt; i++) {
        long n;
        if (iov[i].iov_len == 0) continue;
        n = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return total > 0 ? total : -1;
        total += n;
        // Short read or end of file: stop here. Carrying on would skip a hole
        // in the caller's buffers and report a length that does not describe
        // where the bytes actually landed.
        if ((size_t)n < iov[i].iov_len) break;
    }
    return total;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t total = 0;
    int i;

    if (uio_check(iov, iovcnt) != 0) return -1;

    for (i = 0; i < iovcnt; i++) {
        long n;
        if (iov[i].iov_len == 0) continue;
        n = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return total > 0 ? total : -1;
        total += n;
        if ((size_t)n < iov[i].iov_len) break;   // short write, stop
    }
    return total;
}
