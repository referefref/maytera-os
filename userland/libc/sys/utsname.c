// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/utsname.c - uname(). Field provenance is documented in utsname.h.
//
// SYS_GET_VERSION hands back exactly one string, built by the kernel from
// MAYTERA_VERSION_STRING and MAYTERA_BUILD_NUMBER:  "X.Y.Z (build N)".
// Splitting it here is the ONLY place a version is interpreted in userland,
// and nothing is hardcoded, so a build bump cannot leave this lying.
#include "utsname.h"
#include "../syscall.h"
#include "../string.h"
#include "../errno.h"

int uname(struct utsname *buf) {
    char raw[96];
    int n, i, sp;

    if (!buf) { errno = EFAULT; return -1; }
    memset(buf, 0, sizeof(*buf));

    n = get_version(raw, (int)sizeof(raw));
    if (n <= 0) {
        // No version means no kernel to ask, or a kernel that predates
        // SYS_GET_VERSION. Either way we do not know, and we say so.
        errno = EIO;
        return -1;
    }
    if (n >= (int)sizeof(raw)) n = (int)sizeof(raw) - 1;
    raw[n] = '\0';

    strlcpy(buf->sysname, "MayteraOS", sizeof(buf->sysname));
    strlcpy(buf->machine, "x86_64",    sizeof(buf->machine));

    // release: everything up to the first space, i.e. the X.Y.Z.
    for (sp = 0; raw[sp] && raw[sp] != ' '; sp++) { }
    {
        int cap = (int)sizeof(buf->release) - 1;
        int k = sp < cap ? sp : cap;
        for (i = 0; i < k; i++) buf->release[i] = raw[i];
        buf->release[k] = '\0';
    }

    // version: the text inside the parentheses ("build N"). If the kernel ever
    // stops using that shape, fall back to the whole string rather than to a
    // guess: an unsplit but TRUE version beats a confidently wrong one.
    {
        const char *open = 0, *close = 0;
        for (i = 0; raw[i]; i++) {
            if (raw[i] == '(' && !open) open = &raw[i];
            else if (raw[i] == ')')     close = &raw[i];
        }
        if (open && close && close > open + 1) {
            int len = (int)(close - open - 1);
            int cap = (int)sizeof(buf->version) - 1;
            if (len > cap) len = cap;
            for (i = 0; i < len; i++) buf->version[i] = open[1 + i];
            buf->version[len] = '\0';
        } else {
            strlcpy(buf->version, raw, sizeof(buf->version));
        }
    }

    // nodename and domainname stay "": there is no hostname on this OS. See
    // the header for why that is an empty string and not "localhost".
    return 0;
}
