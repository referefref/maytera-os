// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
#include "notify.h"
#include "syscall.h"
// MayteraOS notifications producer API (#168).
//
// ext2 has no append mode (a write-open rewrites the file), so each post does a
// read-modify-write of the fixed spool /CONFIG/NOTIFY.TXT: read existing bytes,
// append one "S|title|body\n" record, write the whole thing back. Cross-process
// fixed-file read/write is the same mechanism background services already use to
// feed the compositor, so an external producer reaches the compositor. The
// compositor resets the spool at session start, so old records never replay.
// #683: the spool is per-SESSION, not system state: it holds the notifications
// being shown to the logged-in user. It lives in that user's home so the
// compositor can drain (read AND truncate) it without write access to /etc.
// The Ring-0 poster (kernel/security/seclog.c) resolves the SAME path via
// fs/userconf.c, so there is exactly one spool and no split-brain.
// No legacy fallback here: a spool is drained and rewritten, and reading an old
// one we cannot truncate would replay its records on every poll.
#include "userconf.h"
#define SPOOL_NAME "NOTIFY.TXT"
#define SPOOL_CAP 8000
static void ns_sani(char *d, const char *s, int max) {
    int i = 0;
    while (s && s[i] && i < max - 1) {
        char c = s[i];
        if (c == '\n' || c == '\r' || c == '|') c = ' ';
        d[i] = c; i++;
    }
    d[i] = 0;
}
int notify_post(const char *title, const char *body, int severity) {
    if (severity < 0 || severity > 3) severity = 0;
    char t[64], b[160];
    ns_sani(t, title ? title : "", sizeof(t));
    ns_sani(b, body  ? body  : "", sizeof(b));
    static char buf[SPOOL_CAP + 280];
    int blen = 0;
    int rfd = userconf_open_read(SPOOL_NAME, 0);
    if (rfd >= 0) {
        long n = sys_read(rfd, buf, SPOOL_CAP);
        if (n > 0) blen = (int)n;
        sys_close(rfd);
    }
    char *p = buf + blen;
    *p++ = (char)('0' + severity);
    *p++ = '|';
    for (int i = 0; t[i]; i++) *p++ = t[i];
    *p++ = '|';
    for (int i = 0; b[i]; i++) *p++ = b[i];
    *p++ = '\n';
    blen = (int)(p - buf);
    int wfd = userconf_open_write(SPOOL_NAME);   /* #683: per-user spool */
    if (wfd < 0) return -1;
    sys_write(wfd, buf, blen);
    sys_close(wfd);
    return 0;
}
