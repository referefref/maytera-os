// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/uio.h - scatter/gather I/O for MayteraOS userland.
//
// HONEST LIMIT, READ THIS BEFORE USING IT ON A PIPE. There is no vectored
// syscall in this kernel, so readv()/writev() are a LOOP over read()/write().
// The consequence, and the only one: the transfer is NOT ATOMIC. POSIX
// requires writev() of at most PIPE_BUF bytes to a pipe to be indivisible with
// respect to other writers, and this implementation cannot promise that. For a
// regular file, or a pipe with a single writer, the behaviour is identical to a
// real writev. Multiple processes interleaving records into one pipe is the
// case that will bite, and it will bite as interleaved output, not as an error.
//
// Everything else is exact: the short-transfer rule, the return value, the
// EINVAL on an iov_len sum that overflows ssize_t, and errno on failure.
#ifndef LIBC_SYS_UIO_H
#define LIBC_SYS_UIO_H

#include "../types.h"

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

// Maximum iovcnt accepted. Anything larger is EINVAL, as POSIX allows.
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif
#ifndef UIO_MAXIOV
#define UIO_MAXIOV IOV_MAX
#endif

// Returns the number of bytes transferred, which may be less than the total
// iov_len on a short transfer or at end of file, or -1 with errno set if
// NOTHING was transferred. If some bytes were transferred before an error, the
// count is returned and errno is left as the failing call set it, which is the
// same bargain read()/write() make.
ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#endif // LIBC_SYS_UIO_H
