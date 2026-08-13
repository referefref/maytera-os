// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/socket.c - #524 BSD sockets libc wrappers over the SYS_SOCK_* syscalls.
#include "socket.h"
#include "../syscall.h"
#include "../errno.h"

// Kernel syscall numbers (mirror proc/syscall.h).
#define SYS_SOCK_OPEN      343
#define SYS_SOCK_BIND      344
#define SYS_SOCK_CONNECT   345
#define SYS_SOCK_LISTEN    346
#define SYS_SOCK_ACCEPT    347
#define SYS_SOCK_SEND      348
#define SYS_SOCK_RECV      349
#define SYS_SOCK_SENDTO    350
#define SYS_SOCK_RECVFROM  351
#define SYS_SOCK_SETOPT    352
#define SYS_SOCK_GETOPT    353
#define SYS_SOCK_SELECT    354
#define SYS_SOCK_SHUTDOWN  355

// Map a negative kernel return to errno + -1; pass non-negative through.
static long sk_ret(long r) {
    if (r < 0) { errno = (int)(-r); return -1; }
    return r;
}

int socket(int domain, int type, int protocol) {
    return (int)sk_ret(syscall3(SYS_SOCK_OPEN, domain, type, protocol));
}
int bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    return (int)sk_ret(syscall3(SYS_SOCK_BIND, fd, (long)addr, (long)addrlen));
}
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    return (int)sk_ret(syscall3(SYS_SOCK_CONNECT, fd, (long)addr, (long)addrlen));
}
int listen(int fd, int backlog) {
    return (int)sk_ret(syscall2(SYS_SOCK_LISTEN, fd, backlog));
}
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    return (int)sk_ret(syscall3(SYS_SOCK_ACCEPT, fd, (long)addr, (long)addrlen));
}
long send(int fd, const void *buf, unsigned long len, int flags) {
    return sk_ret(syscall4(SYS_SOCK_SEND, fd, (long)buf, (long)len, flags));
}
long recv(int fd, void *buf, unsigned long len, int flags) {
    return sk_ret(syscall4(SYS_SOCK_RECV, fd, (long)buf, (long)len, flags));
}
long sendto(int fd, const void *buf, unsigned long len, int flags,
            const struct sockaddr *addr, socklen_t addrlen) {
    return sk_ret(syscall6(SYS_SOCK_SENDTO, fd, (long)buf, (long)len, flags,
                           (long)addr, (long)addrlen));
}
long recvfrom(int fd, void *buf, unsigned long len, int flags,
              struct sockaddr *addr, socklen_t *addrlen) {
    return sk_ret(syscall6(SYS_SOCK_RECVFROM, fd, (long)buf, (long)len, flags,
                           (long)addr, (long)addrlen));
}
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen) {
    return (int)sk_ret(syscall5(SYS_SOCK_SETOPT, fd, level, optname, (long)optval, (long)optlen));
}
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen) {
    return (int)sk_ret(syscall5(SYS_SOCK_GETOPT, fd, level, optname, (long)optval, (long)optlen));
}
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout) {
    long r = syscall5(SYS_SOCK_SELECT, nfds, (long)readfds, (long)writefds,
                       (long)exceptfds, (long)timeout);
    // #745 (local 82): THE COMMENT THAT USED TO BE HERE WAS FACTUALLY WRONG,
    // AND THE CODE IT JUSTIFIED WAS STILL LYING TO CALLERS.
    //
    // It said SYS_SOCK_SELECT (354) "is NOT YET IMPLEMENTED in the kernel
    // dispatcher (falls into its default case)". It is implemented, and has
    // been: proc/syscall.c dispatches case SYS_SOCK_SELECT to
    // sys_sock_select() in net/socket.c, which scans every fd through
    // file_poll() and blocks on the net RX wait queue with a real deadline.
    //
    // The original problem was real - an unimplemented 354 returned -1 on
    // every call, so ioquake3's NET_Sleep pacing loop never paced and a single
    // OpenArena process pinned a vCPU. The workaround (sleep the caller's
    // timeout, report "no fds ready") fixed the symptom. But it was written as
    // "if (r < 0)", i.e. it fired on ANY negative return, and once the syscall
    // existed that stopped being a workaround and became a mask: EINVAL,
    // EFAULT and EINTR all came back to the caller as 0, which means "your
    // timeout expired and nothing is ready". A function that reports a
    // plausible non-answer for a real error is worse than one that fails,
    // because there is nothing left to debug from.
    //
    // A genuine error is now propagated as -1 with errno set, which is what
    // POSIX select() does. The sleep-and-report-timeout fallback is kept for
    // EXACTLY ONE value, -1, and only when the caller supplied a timeout:
    //   * -1 is what the dispatcher's default case returns for an
    //     unimplemented syscall, so an app built against this libc still
    //     behaves correctly on an OLDER kernel that lacks 354;
    //   * every other negative value can only come from the real handler.
    // This is the same "-1 is ambiguous, everything else is definite" split
    // that sys/stat.c's stat() already uses, for the same reason.
    if (r == -1) {
        long ms = 0;
        if (timeout) {
            ms = (long)timeout->tv_sec * 1000 + (long)timeout->tv_usec / 1000;
        }
        if (ms > 0) syscall1(SYS_SLEEP, ms);
        return 0;
    }
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)sk_ret(r);
}
int shutdown(int fd, int how) {
    return (int)sk_ret(syscall2(SYS_SOCK_SHUTDOWN, fd, how));
}
