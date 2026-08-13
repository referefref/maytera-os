// socket.h - #524 real BSD sockets API over the existing TCP/UDP stack.
//
// This is the fd-integrated, blocking-capable BSD layer. It is DISTINCT from
// the legacy raw socket syscalls (SYS_SOCKET/SYS_CONNECT/... 60..66, 303/304)
// which return a raw TCB slot index, are non-blocking only, and are used by
// irc/nc/curl/vnc. Those are left untouched. The new SYS_SOCK_* syscalls below
// return real per-process file descriptors (struct file in the VFS fd table),
// support blocking + non-blocking + select/poll, and cover TCP and UDP.
//
// WHY C, NOT RUST (per the 2026-07-16 Rust-first rule): this layer is glue that
// is genuinely entangled with C-only primitives that have no Rust FFI surface:
//   - the blocking waits go through wait_event_interruptible_timeout(), a C
//     preprocessor macro that stack-allocates a wait_queue_entry and drives the
//     scheduler; it cannot be invoked from no_std Rust.
//   - the fd model is the C VFS struct file / file_ops vtable (fs/vfs.h).
//   - every NIC-touching call runs inside a net_cr3_enter()/exit() inline-asm
//     CR3 window with interrupts disabled (NIC MMIO lives in the kernel
//     lower-half identity map, absent from the process CR3).
//   - copy_from_user/copy_to_user (security/validate.h) are C.
// The MANDATED Rust part (the syscall pointer-argument descriptors) IS in Rust:
// see rustkern/argtab.rs entries for SYS_SOCK_*.
#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include "../types.h"

// ---- Address family / socket type / protocol (BSD constants) ----
#define AF_INET       2
#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17

// ---- setsockopt levels / options (minimal, what we actually honor) ----
#define SOL_SOCKET    1
#define SO_RCVTIMEO   20   // struct timeval: receive timeout
#define SO_SNDTIMEO   21   // struct timeval: send timeout
#define SO_REUSEADDR  2    // accepted, best-effort no-op (stack allows rebind)
#define SO_ERROR      4    // get pending socket error (int)
#define SO_TYPE       3    // get socket type (int)

// ---- shutdown() how ----
#define SHUT_RD       0
#define SHUT_WR       1
#define SHUT_RDWR     2

// send()/recv() flags we recognize (others ignored).
#define MSG_DONTWAIT  0x40

// sockaddr_in as exchanged with userland. 16 bytes. sin_port / sin_addr are
// NETWORK byte order, matching BSD. Locked by _Static_assert in socket.c.
typedef struct k_sockaddr_in {
    uint16_t sin_family;    // AF_INET
    uint16_t sin_port;      // network byte order
    uint32_t sin_addr;      // network byte order
    uint8_t  sin_zero[8];
} k_sockaddr_in_t;

// struct timeval as exchanged for SO_RCVTIMEO/SO_SNDTIMEO. 16 bytes on x86-64.
typedef struct k_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
} k_timeval_t;

// fd_set: MayteraOS caps a process at MAX_FDS (64) fds, so a single 64-bit
// mask covers the whole table. select() reads/writes these as 8-byte masks.
typedef struct k_fd_set {
    uint64_t bits;
} k_fd_set_t;

// ---- init ----
void socket_init(void);

// ---- syscall entry points (called from proc/syscall.c dispatcher) ----
// All operate on / return real fds and take user pointers where noted; the
// user-pointer args are validated by the argtab descriptors before the handler
// runs, and copied via copy_*_user inside.
int64_t sys_sock_open(int domain, int type, int protocol);
int64_t sys_sock_bind(int fd, const void *uaddr, int addrlen);
int64_t sys_sock_connect(int fd, const void *uaddr, int addrlen);
int64_t sys_sock_listen(int fd, int backlog);
int64_t sys_sock_accept(int fd, void *uaddr, void *uaddrlen);
int64_t sys_sock_send(int fd, const void *ubuf, uint64_t len, int flags);
int64_t sys_sock_recv(int fd, void *ubuf, uint64_t len, int flags);
int64_t sys_sock_sendto(int fd, const void *ubuf, uint64_t len, int flags,
                        const void *uaddr, int addrlen);
int64_t sys_sock_recvfrom(int fd, void *ubuf, uint64_t len, int flags,
                          void *uaddr, void *uaddrlen);
int64_t sys_sock_setsockopt(int fd, int level, int optname,
                            const void *uoptval, int optlen);
int64_t sys_sock_getsockopt(int fd, int level, int optname,
                            void *uoptval, void *uoptlen);
int64_t sys_sock_select(int nfds, void *ureadfds, void *uwritefds,
                        void *uexceptfds, void *utimeout);
int64_t sys_sock_shutdown(int fd, int how);

// ---- RX plumbing hooks (called from the net stack under net_lock) ----
// Wake any process blocked in a socket recv/accept/connect. Safe from IRQ /
// net_lock context (only touches a wait_queue_head's own spinlock).
void socket_net_wake(void);

// Deliver an inbound UDP datagram to a bound BSD DGRAM socket, if any. Called
// from udp_handle() (which runs under net_lock). dest_port is host order.
// Returns 1 if a BSD socket consumed it, 0 otherwise.
int socket_udp_input(uint32_t src_ip, uint16_t src_port, uint16_t dest_port,
                     const void *data, uint16_t len);

#endif // NET_SOCKET_H
