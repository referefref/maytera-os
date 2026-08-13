// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// poll.h - POSIX poll(2).
//
// Syscall 104 was declared in syscall.h and implemented nowhere in the kernel
// until #745 (local 82), so this header is new rather than a shim over an
// existing wrapper. The kernel side is rustkern/pollsys.rs.
//
// SCOPE, stated so nobody is surprised by it: a process holds at most 64 file
// descriptors (MAX_FDS in the kernel's proc/process.h), so nfds > 64 is
// rejected with EINVAL rather than silently truncated. Passing a negative fd in
// a slot is the POSIX way to disable that slot and is supported: revents is
// cleared and the slot is not counted.
#ifndef _POLL_H
#define _POLL_H

typedef unsigned long nfds_t;

struct pollfd {
    int   fd;         // descriptor to watch; negative means "skip this slot"
    short events;     // requested POLL* bits
    short revents;    // returned POLL* bits (set by the kernel)
};

// Requested and returned.
#define POLLIN      0x001   // data available to read
#define POLLPRI     0x002   // urgent data (no transport reports this yet)
#define POLLOUT     0x004   // writable without blocking

// RETURNED ONLY. Reported whether or not they were requested, which is how a
// caller learns about a peer that went away.
#define POLLERR     0x008   // error condition
#define POLLHUP     0x010   // hang up (pipe writers gone, PTY peer closed)
#define POLLNVAL    0x020   // fd is not open

// Wait for one of `nfds` descriptors to become ready.
//
// `timeout` is in MILLISECONDS: negative waits forever, 0 returns immediately
// after one scan, positive is a real-time bound.
//
// Returns the number of descriptors with a non-zero revents, 0 on timeout, or
// -1 with errno set (EINVAL for nfds > 64, EFAULT for a bad pointer, EINTR when
// a signal arrived before anything was ready).
int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#endif // _POLL_H
