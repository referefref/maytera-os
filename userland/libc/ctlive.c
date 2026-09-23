// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// ctlive.c - the LIVE-INSTANCE contract call path (tier 2 wire).
//   docs/AI_ACTION_CAPABILITY_BINDING.md section 4.2/4.3.
//
// contract_invoke() (ctinvoke.c) SPAWNS a fresh copy of an app and runs its
// --contract verb before it draws a window; that copy exits, so it can never
// touch a live document (CONTRACT_API.md section 3). This file is the OTHER
// half: it delivers the SAME contract verb to an app instance ALREADY running
// with a window, which runs the action's actfn against its LIVE g_doc/state and
// answers with the SAME machine-readable line contract_cli() would. That "same
// line" is not a coincidence: both paths funnel through contract_run_verb() in
// contract.c, so the live path cannot answer a verb differently from the spawn
// path, and cannot skip the gate or the audit.
//
// TRANSPORT, AND ITS HONEST CEILING
// The bytes cross between the caller and the app through a small request/reply
// MAILBOX pair in the caller's home directory, keyed by the app id. This is
// deliberately the least new machinery that proves the wire end to end:
//   - The window EVENT queue carries ints only (no string payload), so the
//     existing capability-gated window route (SYS_CAP_INJECT_*) cannot carry a
//     contract call (measured, kernel/gui/window.h gui_event_t).
//   - kernel/ipc/msg.c is ungated, not window-scoped, and busy-yields in
//     msg_recv (CONTRACT_API.md section 3) - explicitly not to be built on.
//   - The drag payload session is tied to a compositor-arbitrated mouse drop,
//     so a background AI process cannot deposit a payload through it.
// A file mailbox has EXACTLY the threat surface the spawn path already has: any
// process that can already run `/APPS/PAINT --contract call ...` can already
// drive the app; this lets it drive the LIVE instance instead of a throwaway
// one, and the receiving app runs the same contract_authorize()/aicap gate
// either way. So it neither raises nor lowers the #679 uid-0 ceiling the whole
// contract API already states (CONTRACT_API.md section 4). The KERNEL teeth are
// unchanged: an action whose `needs` reach a gated syscall (screen_check ->
// screen.capture) still hits syscall_cap_check in the kernel regardless of how
// the call was delivered.
//
// THE SEAM (docs/AI_ACTION_CAPABILITY_BINDING.md section 4.4, Explore option b):
// to make the DELIVERY itself kernel-gated and per-window-scoped, replace the
// two transport primitives below (mbox_write_req/mbox_read_resp on the client
// and the mailbox read/write in contract_live_poll) with a gated per-window
// "action mailbox" syscall pair that reuses the CAP_SCOPE_WINDOW_TARGET scope
// model already proven by SYS_CAP_INJECT_*. Nothing above the transport - the
// contract engine, the gate, the audit, the app's actfns, the AI tool - has to
// change. That is why the transport is isolated in these few functions.

#include "contract.h"
#include "aicap.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "stdlib.h"
#include "fcntl.h"
#include "syscall.h"
#include "userconf.h"

#define CTLIVE_REQ_MAX   1024   // one request line-set (verb + args)
#define CTLIVE_RESP_MAX   768   // the app's reply, matched to contract.c's out[]
#define CTLIVE_MAX_ARGS     8
#define CTLIVE_ARG_MAX    192
#define CTLIVE_WAIT_MS   4000   // an app polls at ~100ms; 4s is generous
#define CTLIVE_SENTINEL  "END\n"

// ---------------------------------------------------------------------------
// Mailbox files. Keyed by the app id so the app and its callers agree without a
// window handle: v1 assumes ONE live instance per app (a second instance would
// share the mailbox). The upgrade to per-window keying rides on the kernel
// mailbox seam above.
//
// The mailbox lives in the PER-USER CONFIG area (userconf_open_*), NOT in the
// home root: the whole desktop runs as one non-root user (measured: the
// compositor and every app are uid 1000), whose home root is not writable but
// whose <home>/CONFIG is, and userconf_open_write() creates it. This is the
// exact mechanism Settings uses to persist AISVC.CFG, so the mailbox inherits a
// location that is guaranteed writable by the same user every AI-drivable app
// already runs as. Names are uppercased for a FAT home. e.g. CTLVPAINT.Q / .R.
// ---------------------------------------------------------------------------
static void mbox_name(const char *app, char which, char *out, int ocap) {
    char up[24]; int j = 0;
    for (int i = 0; app[i] && j < (int)sizeof(up) - 1; i++) {
        char c = app[i];
        up[j++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    up[j] = 0;
    snprintf(out, ocap, "CTLV%s.%c", up, which);
}

// Read a whole small mailbox file into buf; returns length (0 if absent/empty).
static int mbox_slurp(const char *name, char *buf, int cap) {
    int fd = userconf_open_read(name, 0);
    if (fd < 0) { buf[0] = 0; return 0; }
    int used = 0;
    for (;;) {
        if (used >= cap - 1) break;
        long n = read(fd, buf + used, (size_t)(cap - 1 - used));
        if (n <= 0) break;
        used += (int)n;
    }
    close(fd);
    buf[used] = 0;
    return used;
}

// userconf_open_write() O_CREAT|O_TRUNC|O_WRONLY and mkdir's <home>/CONFIG.
static int mbox_spew(const char *name, const char *buf, int len) {
    int fd = userconf_open_write(name);
    if (fd < 0) return -1;
    int off = 0;
    while (off < len) {
        long n = write(fd, buf + off, (size_t)(len - off));
        if (n <= 0) break;
        off += (int)n;
    }
    close(fd);
    return off == len ? 0 : -1;
}

static void mbox_unlink(const char *name) {
    char p[200];
    if (userconf_path(name, p, sizeof(p)) == 0) unlink(p);
}

// A monotonic-ms request id so a client matches its OWN reply and never reads a
// stale one. uptime_ms() differs across the client processes that each `ctl`
// or AI action runs as, which a per-process counter would not.
static unsigned long ctlive_reqid(void) {
    unsigned long t = uptime_ms();
    return t ? t : 1;
}

// ---------------------------------------------------------------------------
// SERVE side: called by the app once per event-loop tick.
// ---------------------------------------------------------------------------
int contract_live_poll(const ct_contract_t *c) {
    if (!c || !c->app) return 0;

    char qname[40];
    mbox_name(c->app, 'Q', qname, sizeof(qname));

    char req[CTLIVE_REQ_MAX];
    int n = mbox_slurp(qname, req, sizeof(req));
    if (n <= 0) return 0;

    // Only act on a COMPLETE request: the client ends every request with the
    // sentinel line, so a request read mid-write (no sentinel yet) is left for
    // the next tick instead of being parsed half-formed.
    if (!strstr(req, CTLIVE_SENTINEL)) return 0;

    // Parse "REQ <reqid>\n<nargs>\n<arg0>\n...".
    const char *p = req;
    if (strncmp(p, "REQ ", 4) != 0) { mbox_unlink(qname); return 0; }
    p += 4;
    unsigned long reqid = 0;
    while (*p >= '0' && *p <= '9') { reqid = reqid * 10 + (unsigned long)(*p - '0'); p++; }
    while (*p == ' ' || *p == '\r') p++;
    if (*p == '\n') p++;

    int nargs = 0;
    while (*p >= '0' && *p <= '9') { nargs = nargs * 10 + (*p - '0'); p++; }
    while (*p == ' ' || *p == '\r') p++;
    if (*p == '\n') p++;
    if (nargs < 1) { mbox_unlink(qname); return 0; }
    if (nargs > CTLIVE_MAX_ARGS) nargs = CTLIVE_MAX_ARGS;

    static char argbuf[CTLIVE_MAX_ARGS][CTLIVE_ARG_MAX];
    static char *argv[CTLIVE_MAX_ARGS];
    for (int i = 0; i < nargs; i++) {
        int k = 0;
        while (*p && *p != '\n' && *p != '\r' && k < CTLIVE_ARG_MAX - 1)
            argbuf[i][k++] = *p++;
        argbuf[i][k] = 0;
        while (*p == '\r') p++;
        if (*p == '\n') p++;
        argv[i] = argbuf[i];
    }

    // Authorization must reflect a grant made AFTER this app started, exactly as
    // a freshly spawned contract process would see it. Without this the live
    // path would be stuck on the AICAPS.CFG that was current at window-create.
    aicap_reload();

    // Run the SAME engine the CLI runs, capturing its reply lines. argv[0] is
    // the verb; argv[1] is the item name for get/set/call.
    char resp[CTLIVE_RESP_MAX];
    contract_capture_begin(resp, sizeof(resp));
    contract_run_verb(c, argv[0], nargs - 1, argv + 1);
    contract_capture_end();

    // Reply, then remove the request so it is served exactly once.
    char rname[40];
    mbox_name(c->app, 'R', rname, sizeof(rname));
    {
        char out[CTLIVE_RESP_MAX + 64];
        int hl = snprintf(out, sizeof(out), "RSP %lu\n", reqid);
        int rl = (int)strlen(resp);
        if (hl + rl > (int)sizeof(out) - 1) rl = (int)sizeof(out) - 1 - hl;
        memcpy(out + hl, resp, (size_t)rl);
        out[hl + rl] = 0;
        mbox_spew(rname, out, hl + rl);
    }
    mbox_unlink(qname);
    return 1;
}

// ---------------------------------------------------------------------------
// CLIENT side.
// ---------------------------------------------------------------------------

// Is an instance of `app` currently on screen? Match the wm app_id (the binary
// basename, uppercase) to the app id, among visible non-minimized windows.
static int app_is_live(const char *app) {
    static wm_window_info_t wins[32];
    int n = wm_get_windows(wins, (int)(sizeof(wins) / sizeof(wins[0])));
    if (n <= 0) return 0;
    char up[24]; int j = 0;
    for (int i = 0; app[i] && j < (int)sizeof(up) - 1; i++) {
        char ch = app[i];
        up[j++] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
    }
    up[j] = 0;
    for (int i = 0; i < n; i++) {
        if (!wins[i].visible || wins[i].minimized) continue;
        if (wins[i].app_id[0] && !strcmp(wins[i].app_id, up)) return 1;
    }
    return 0;
}

int contract_invoke_live(const char *app, int argc, char **argv,
                         char *out, int ocap) {
    if (ocap > 0) out[0] = 0;
    if (!app || argc < 1) return CT_ERR_USAGE;

    // app_is_live() only TUNES how long we wait, it does not gate delivery: an
    // instance whose wm app_id we failed to match would otherwise be missed
    // even though it is polling. If a window that looks like the app is up we
    // wait the full window; otherwise we still post and wait a short probe, so
    // a truly-absent app fails fast to the spawn fallback and a running one is
    // always reached.
    int wait_ms = app_is_live(app) ? CTLIVE_WAIT_MS : 900;

    char qname[40], rname[40];
    mbox_name(app, 'Q', qname, sizeof(qname));
    mbox_name(app, 'R', rname, sizeof(rname));

    // Clear any stale reply so we only ever read our own.
    mbox_unlink(rname);

    unsigned long reqid = ctlive_reqid();

    // Build "REQ <reqid>\n<argc>\n<arg0>\n...\nEND\n". One arg per line handles
    // spaces inside an argument; the sentinel makes a partial read detectable.
    char req[CTLIVE_REQ_MAX];
    int at = snprintf(req, sizeof(req), "REQ %lu\n%d\n", reqid, argc);
    for (int i = 0; i < argc && at < (int)sizeof(req) - 8; i++) {
        at += snprintf(req + at, sizeof(req) - at, "%s\n", argv[i] ? argv[i] : "");
    }
    at += snprintf(req + at, sizeof(req) - at, CTLIVE_SENTINEL);
    if (mbox_spew(qname, req, at) != 0) return CT_LIVE_NONE;

    // Wait for the reply that carries our reqid. Bounded; the app answers on
    // its next event-loop tick (~100ms). This is a poll on another PROCESS's
    // output, the acceptable timeout case (blame.md waiting rules), not a
    // core-burning spin: sys_sleep parks in the kernel.
    char expect[32];
    snprintf(expect, sizeof(expect), "RSP %lu\n", reqid);
    char resp[CTLIVE_RESP_MAX + 64];
    for (int waited = 0; waited <= wait_ms; waited += 60) {
        int n = mbox_slurp(rname, resp, sizeof(resp));
        if (n > 0 && !strncmp(resp, expect, strlen(expect))) {
            const char *body = resp + strlen(expect);
            if (ocap > 0) { strlcpy(out, body, (size_t)ocap); }
            mbox_unlink(rname);
            mbox_unlink(qname);
            // The reply body starts with the app's own "ok "/"err " line; the
            // reply CODE is derived from it the same way contract_invoke()
            // leaves the caller to read the "err " prefix. Report OK transport;
            // the caller inspects out[] exactly as for the spawn path.
            return CT_OK;
        }
        sys_sleep(60);
    }
    // No answer: leave nothing behind and let the caller fall back to a spawn.
    mbox_unlink(qname);
    if (ocap > 0) snprintf(out, ocap, "err code=failed detail=live-timeout app=%s\n", app);
    return CT_LIVE_TIMEOUT;
}
