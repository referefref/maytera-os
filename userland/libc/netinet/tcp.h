// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// netinet/tcp.h - TCP-level socket option names (#745 darkhttpd port).
//
// Used with setsockopt(fd, IPPROTO_TCP, ...). The MayteraOS kernel accepts
// these as no-ops (net/socket.c sys_sock_setsockopt returns 0 for options it
// does not act on), so a program that sets TCP_NODELAY behaves correctly: the
// call succeeds and the (single-threaded) stack simply does not implement
// Nagle toggling. The numeric values match Linux so a value parsed from the
// wire or a config file is interpreted the same way.
#ifndef _NETINET_TCP_H
#define _NETINET_TCP_H

#define TCP_NODELAY   1
#define TCP_MAXSEG    2
#define TCP_CORK      3

#endif // _NETINET_TCP_H
