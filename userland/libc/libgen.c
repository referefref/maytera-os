// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// libgen.c - POSIX basename()/dirname(). Contract is in libgen.h.
//
// The cases that catch people out, and which the test battery pins:
// a trailing slash is not a component ("/usr/" -> "usr" / "/"), a path that is
// nothing but slashes is the root, and the empty string and NULL are ".".
#include "libgen.h"

static char s_dot[]   = ".";
static char s_slash[] = "/";

char *basename(char *path) {
    char *p, *end;

    if (path == 0 || path[0] == '\0') return s_dot;

    // Drop trailing slashes. All-slashes means the root.
    end = path;
    while (*end) end++;
    while (end > path && end[-1] == '/') end--;
    if (end == path) return s_slash;   // "/", "//", "///"
    // Only write when a trailing slash was actually stripped. A caller may
    // legitimately hand us a string that needs no edit at all, and writing a
    // byte it did not ask us to write is a fault waiting to happen.
    if (*end) *end = '\0';

    // Last component starts after the last remaining slash.
    p = end;
    while (p > path && p[-1] != '/') p--;
    return p;
}

char *dirname(char *path) {
    char *end, *p;

    if (path == 0 || path[0] == '\0') return s_dot;

    end = path;
    while (*end) end++;
    while (end > path && end[-1] == '/') end--;
    if (end == path) return s_slash;   // path was all slashes

    // Strip the last component.
    p = end;
    while (p > path && p[-1] != '/') p--;
    if (p == path) return s_dot;       // no slash at all: "usr" -> "."

    // Strip the slashes that separated it, then any trailing slashes of what
    // is left. If nothing is left the answer is the root.
    while (p > path && p[-1] == '/') p--;
    if (p == path) return s_slash;     // "/usr" -> "/", "/usr/" -> "/"
    *p = '\0';
    return path;
}
