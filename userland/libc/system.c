// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// system.c - ISO C system(), and the reason it is ALONE in this file.
//
// MayteraOS has NO command processor that a program can hand a shell string to,
// and this reports that honestly rather than pretending:
//   system(NULL) returns 0. That is the ISO C encoding of "no command processor
//                is available", and it is the question a caller is REQUIRED to
//                ask before relying on system() at all.
//   system(cmd)  returns -1 with errno = ENOSYS. It does not run anything and
//                it does not claim to.
// Returning 0 for an unexecuted command would tell every caller the command
// succeeded.
//
// KEEP THIS FUNCTION ALONE IN THIS FILE, AND DEFINE NOTHING ELSE HERE, for the
// same reason as tmpfile.c: userland/libcompat/libc_gap.cpp defines its own
// system() for the three game ports that link it, and an explicitly-listed
// object satisfies the symbol before libc.a is scanned only as long as this
// object has no other reason to be pulled in.
//
// Worth knowing while that duplicate exists: libc_gap's version returns -1 for
// EVERY argument INCLUDING NULL, and its comment says that matches "command
// processor unavailable". It is the inverse - C99 7.22.4.8 says a NULL argument
// returns ZERO when no command processor is available - so a caller probing
// with system(NULL) is told a shell EXISTS. Nothing in those ports probes, so
// nothing is broken by it today.
#include "stdlib.h"
#include "errno.h"

int system(const char *command) {
    if (command == 0) return 0;
    errno = ENOSYS;
    return -1;
}
