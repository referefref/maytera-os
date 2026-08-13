// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/utsname.h - uname() for MayteraOS userland.
//
// WHERE THE FIELDS COME FROM, so nobody has to guess and nothing drifts:
//
//   sysname   "MayteraOS", a constant.
//   release   the X.Y.Z part of the kernel's OWN version string, fetched at
//             run time with SYS_GET_VERSION (syscall 246). It is not baked
//             into this libc, so it cannot go stale against the kernel that
//             is actually running.
//   version   the "build N" part of the same string.
//   machine   "x86_64". A compile-time truth: userland is built -m64 for
//             x86-64 and there is no other userland target.
//   nodename  EMPTY, and deliberately so. MayteraOS has no hostname: the
//             installer asks for one and then discards it, there is no
//             gethostname(), no kernel field and no config key holding it.
//             Rather than invent "localhost" and have programs print a name
//             that is not this machine's, the field is left as the empty
//             string. When a real hostname facility lands, uname.c is the
//             one place to wire it in.
//   domainname likewise empty, and it is a GNU extension besides.
//
// uname() FAILS (-1, errno EIO) if the kernel will not report its version,
// rather than filling in a plausible-looking release. A wrong version number
// is worse than no answer.
#ifndef LIBC_SYS_UTSNAME_H
#define LIBC_SYS_UTSNAME_H

#define _UTSNAME_LENGTH 65

struct utsname {
    char sysname[_UTSNAME_LENGTH];
    char nodename[_UTSNAME_LENGTH];
    char release[_UTSNAME_LENGTH];
    char version[_UTSNAME_LENGTH];
    char machine[_UTSNAME_LENGTH];
    char domainname[_UTSNAME_LENGTH];   // GNU extension
};

int uname(struct utsname *buf);

#endif // LIBC_SYS_UTSNAME_H
