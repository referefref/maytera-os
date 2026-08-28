// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// tmpfile.c - ISO C tmpfile(), and the reason it is ALONE in this file.
//
// MayteraOS HAS NO TEMPORARY FILE FACILITY. tmpfile() is specified to create a
// file that is REMOVED WHEN IT IS CLOSED, which needs a filesystem that can
// unlink an open file; neither the FAT nor the ext2 path in this kernel can.
// Returning NULL is a conforming outcome (C99 7.21.4.3) and it is LOUD: every
// caller has to handle it, and Lua's io.tmpfile() turns it into a script-visible
// error. Handing back a file that is not temporary would be the dishonest
// answer, and the caller would never find out.
//
// KEEP THIS FUNCTION ALONE IN THIS FILE, AND DEFINE NOTHING ELSE HERE.
// userland/libcompat/libc_gap.cpp defines its own tmpfile() for the three game
// ports that link it (assaultcube, openarena, classicube); that one returns a
// REAL, non-anonymous file, which is right for a single-process game's scratch
// data and wrong as a system-wide contract. Because libc_gap.o is listed
// explicitly on those links, it satisfies the symbol before libc.a is scanned
// and this object is never pulled in. Put a second function in here and the day
// something references it, this object gets pulled and collides. That is not
// hypothetical: it is exactly how assaultcube stopped linking when tmpfile()
// was first added to stdio_file.c (local queue 91).
//
// DEBT, stated rather than left to be discovered: two implementations of one
// name is the duplication the owner rule exists to prevent. Retiring
// libc_gap's copy is the right end state, but it CHANGES BEHAVIOUR for three
// shipped games (their scratch file would stop existing), so it needs those
// games re-verified, which is its own piece of work and not a drive-by.
// While reading that copy, note its system() claims a command processor EXISTS
// (it returns -1 for a NULL argument, and C says NULL must return 0 when there
// is none) - the inverse of the answer below.
//
// If MayteraOS ever grows a real temporary directory with a collector, THIS is
// the place to implement it, for every app at once.
#include "stdio.h"
#include "errno.h"

FILE *tmpfile(void) {
    errno = ENOSYS;
    return 0;
}
