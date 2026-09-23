// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/resource.h - minimal getrusage() surface (#745 darkhttpd port).
//
// MayteraOS has no per-process resource accounting in the kernel, so
// getrusage() is a clearly-commented best-effort stub in posixextra.c that
// zero-fills the struct and reports ENOSYS. This header exists so BSD-derived
// userland (darkhttpd prints CPU stats at shutdown) compiles and links; it
// deliberately declares only what such programs reference.
#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H

#include "time.h"   // struct timeval

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

struct rusage {
    struct timeval ru_utime;   // user CPU time used
    struct timeval ru_stime;   // system CPU time used
};

int getrusage(int who, struct rusage *usage);

#endif // _SYS_RESOURCE_H
