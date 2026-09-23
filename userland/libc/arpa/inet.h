// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#include <sys/socket.h>

// inet_ntoa: network-order struct in_addr -> dotted-quad string in a static
// buffer (#745 darkhttpd port). Not reentrant, matching the POSIX contract.
char *inet_ntoa(struct in_addr in);
#endif
