// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/socket.h - #524 BSD sockets API for MayteraOS userland.
//
// Wraps the SYS_SOCK_* syscalls (kernel net/socket.c). These are fd-integrated
// (close() with the normal close(), select() via SYS_SOCK_SELECT), blocking by
// default, non-blocking via O_NONBLOCK (fcntl) or MSG_DONTWAIT. TCP + UDP.
//
// Distinct from the legacy raw tcp_socket()/tcp_connect()/... inline helpers in
// <syscall.h>, which return TCB slot indices and are non-blocking only.
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

typedef unsigned int   socklen_t;
typedef unsigned int   in_addr_t;
typedef unsigned short in_port_t;
typedef unsigned short sa_family_t;

// Address families / socket types / protocols
#define AF_INET       2
// task #568: parse-only IPv6 support. MayteraOS has NO real IPv6 stack;
// this exists only so dual-stack-aware callers (qcommon/net_ip.c) compile.
// Never returned by a real socket() call on this OS today.
#define AF_INET6      10
#define AF_UNSPEC     0
struct in6_addr { unsigned char s6_addr[16]; };
struct sockaddr_in6 {
    sa_family_t     sin6_family;
    in_port_t       sin6_port;
    unsigned int    sin6_flowinfo;
    struct in6_addr sin6_addr;
    unsigned int    sin6_scope_id;
};
struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    int             ipv6mr_interface;
};
#define NI_NUMERICHOST 1   // task #568: getnameinfo() flag, parse-only
// task #568: real (not fabricated) IPv6 address-class checks, matching the
// real RFC 4291 bit patterns: multicast = first byte 0xff; unspecified =
// all 16 bytes zero. Genuinely correct even though MayteraOS has no real
// IPv6 stack to feed them non-degenerate addresses yet.
#define IN6_IS_ADDR_MULTICAST(a) (((const unsigned char *)(a))[0] == 0xff)
static inline int mos_in6_is_unspecified(const struct in6_addr *a) {
    for (int _i = 0; _i < 16; _i++) if (a->s6_addr[_i] != 0) return 0;
    return 1;
}
#define IN6_IS_ADDR_UNSPECIFIED(a) mos_in6_is_unspecified(a)

#define PF_INET6      AF_INET6
#define IPPROTO_IPV6      41
#define IPV6_MULTICAST_IF 9
#define IPV6_JOIN_GROUP   20
#define IPV6_LEAVE_GROUP  21
#define FIONBIO       0x5421   // real Linux ioctl() request number, parse-only (MayteraOS sockets are non-blocking via fcntl/MSG_DONTWAIT, see file header comment; ioctlsocket() itself is a Winsock-ism guarded out on this build)
#define SO_BROADCAST  6
extern const struct in6_addr in6addr_any;

#define PF_INET       AF_INET
#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17

// setsockopt levels / names
#define SOL_SOCKET    1
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_RCVTIMEO   20
#define SO_SNDTIMEO   21

// send/recv flags
#define MSG_DONTWAIT  0x40

// shutdown how
#define SHUT_RD       0
#define SHUT_WR       1
#define SHUT_RDWR     2

// Well-known addresses (HOST byte order; pass through htonl for sin_addr).
#define INADDR_ANY        ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK   ((in_addr_t)0x7f000001)
#define INADDR_BROADCAST  ((in_addr_t)0xffffffff)
#define INADDR_NONE       ((in_addr_t)0xffffffff)

struct in_addr { in_addr_t s_addr; };   // network byte order

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_in {
    sa_family_t     sin_family;   // AF_INET
    in_port_t       sin_port;     // network byte order
    struct in_addr  sin_addr;     // network byte order
    unsigned char   sin_zero[8];
};

#include "time.h"   // task #568: struct timeval belongs to sys/time.h; was duplicated here unguarded, causing a redefinition error when both headers are included together

// task #568: protocol-agnostic address storage (POSIX sockaddr_storage).
// MayteraOS is IPv4-only today; sized/aligned like the real one anyway
// (128 bytes total) so a future IPv6 sockaddr_in6 still fits without an ABI
// break.
struct sockaddr_storage {
    sa_family_t ss_family;
    char        ss_pad[128 - sizeof(sa_family_t)];
};

// fd_set: MayteraOS caps a process at 64 fds, so one 64-bit word suffices.
#define FD_SETSIZE 64
typedef struct { unsigned long fds_bits[1]; } fd_set;
#define FD_ZERO(s)     ((s)->fds_bits[0] = 0UL)
#define FD_SET(fd, s)  ((s)->fds_bits[0] |= (1UL << (fd)))
#define FD_CLR(fd, s)  ((s)->fds_bits[0] &= ~(1UL << (fd)))
#define FD_ISSET(fd,s) (((s)->fds_bits[0] >> (fd)) & 1UL)

// Byte order (host is little-endian x86-64).
static inline unsigned short htons(unsigned short h){ return (unsigned short)((h>>8)|(h<<8)); }
static inline unsigned short ntohs(unsigned short n){ return htons(n); }
static inline unsigned int htonl(unsigned int h){
    return ((h&0xFF)<<24)|((h&0xFF00)<<8)|((h>>8)&0xFF00)|((h>>24)&0xFF);
}
static inline unsigned int ntohl(unsigned int n){ return htonl(n); }

// inet_addr: dotted-quad -> network-order in_addr_t; INADDR_NONE on parse error.
static inline in_addr_t inet_addr(const char *s){
    unsigned int parts[4]; int pi=0; unsigned int cur=0; int digits=0;
    for (const char *p=s;;p++){
        if (*p>='0'&&*p<='9'){ cur=cur*10+(unsigned)(*p-'0'); digits=1; if(cur>255) return INADDR_NONE; }
        else if (*p=='.'||*p=='\0'){
            if(!digits||pi>3) return INADDR_NONE;
            parts[pi++]=cur; cur=0; digits=0;
            if(*p=='\0') break;
        } else return INADDR_NONE;
    }
    if (pi!=4) return INADDR_NONE;
    unsigned int host=(parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3];
    return htonl(host);
}

// ---- the BSD calls (net/socket.c). Return >=0 / bytes on success, -1 + errno. ----
int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
long send(int fd, const void *buf, unsigned long len, int flags);
long recv(int fd, void *buf, unsigned long len, int flags);
long sendto(int fd, const void *buf, unsigned long len, int flags,
            const struct sockaddr *addr, socklen_t addrlen);
long recvfrom(int fd, void *buf, unsigned long len, int flags,
              struct sockaddr *addr, socklen_t *addrlen);
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);
int shutdown(int fd, int how);
// close() lives in <unistd.h> / <syscall.h> and works on socket fds too.

#endif // _SYS_SOCKET_H
