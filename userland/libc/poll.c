// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// poll.c - the poll(2) wrapper.
//
// A separate file rather than another entry in posixextra.c so that the one
// thing that must never drift, the constants, sits next to the header that
// declares them. The kernel side (rustkern/pollsys.rs) asserts at boot that its
// POLL* values still equal fs/vfs.h's POLL_*; this end is the third copy, and
// the _Static_assert block below is what keeps it honest at compile time
// instead of at debug time.
#include "poll.h"
#include "syscall.h"
#include "errno.h"

// The kernel copies `nfds` entries of exactly this shape in and out. A silent
// size change here would make the caller and the kernel disagree about where
// revents lives, and the failure would look like "poll never reports anything"
// rather than like a build error.
_Static_assert(sizeof(struct pollfd) == 8, "struct pollfd must stay 8 bytes");
_Static_assert(POLLIN == 0x001, "POLLIN must match kernel fs/vfs.h POLL_IN");
_Static_assert(POLLOUT == 0x004, "POLLOUT must match kernel fs/vfs.h POLL_OUT");
_Static_assert(POLLERR == 0x008, "POLLERR must match kernel fs/vfs.h POLL_ERR");
_Static_assert(POLLHUP == 0x010, "POLLHUP must match kernel fs/vfs.h POLL_HUP");

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    long r = syscall3(SYS_POLL, (long)fds, (long)nfds, (long)timeout);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}
