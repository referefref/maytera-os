// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
#include "notify.h"
#include "stdio.h"     // #appwrite: snprintf for the dropped-toast line
#include "syscall.h"    // #appwrite: sys_bootlog
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
    // [foundbugs] #168: userconf_open_read_scratch(), not userconf_open_read():
    // a no-account session's per-user path is "/CONFIG/NOTIFY.TXT", and
    // /CONFIG ends up root-owned 0711 (searchable, not writable) before any
    // userland process runs, so this process could never CREATE that file in
    // the first place; see userconf.h for the full rule. This must match
    // notif.c's reader and notif_init()'s unlink, and seclog.c's Ring-0
    // poster, or the spool splits across two files that nobody is
    // cross-checking.
    int rfd = userconf_open_read_scratch(SPOOL_NAME);
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
    int wfd = userconf_open_write_scratch(SPOOL_NAME);   /* #683/#foundbugs: per-user spool, with a no-account fallback */
    if (wfd < 0) {
        // #appwrite: THE NOTIFICATION CHANNEL IS ITSELF A FILE WRITE, so on a
        // machine where writes are refused the OS loses its only way to say
        // that anything was refused. Worse, several callers raise a toast
        // precisely BECAUSE their own save failed (compositor/desktop.c:973 is
        // one), so the report and the thing it reports die together and the
        // feature presents as "the button does nothing".
        //
        // This does not make the toast appear. It makes the LOSS OF THE TOAST
        // visible, in the one channel MEASURED to work from a uid-1000 app on
        // this image. Deliberately not a retry to some other path: inventing a
        // second spool location would be a second thing to keep in step, and
        // the right fix for the underlying refusal is the path the write
        // targets, not another fallback.
        // #aiaudit: CLAMPED. An over-long sys_bootlog message is DROPPED
        // WHOLE, not truncated (see BOOTLOG_MSG_MAX in syscall.h), and `t` is
        // a caller-supplied title with no bound, so a long title silently
        // deleted the report of its own lost toast. The same shape MEASURED as
        // a real loss on the AI audit sink (golden 2486, VM 2497).
        char m[180];
        snprintf(m, sizeof(m),
                 "[FSDENY-U] notify_post DROPPED (spool unwritable, rc=%d) sev=%d title=%.80s",
                 wfd, severity, t);
        (void)sys_bootlog(m);
        return -1;
    }
    sys_write(wfd, buf, blen);
    sys_close(wfd);
    return 0;
}
