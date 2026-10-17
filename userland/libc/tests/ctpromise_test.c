// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// ctpromise_test.c - MEASURED host test that the ESCROW LAYER ACTUALLY CONSUMES
// a per-item contract promise (#<config-ref>, task 2 of
// docs/CONTRACT_COVERAGE_PLAN.md).
//
// WHAT IT IS FOR. The task-2 brief is explicit that "a field nothing reads is
// decoration", and that the promise term must be wired to the EXISTING escrow
// machinery rather than beside it. The way to know which of those happened is to
// run the real contract engine over a real promised action and look at what the
// escrow oracle reports. So the units under test are the SHIPPED files, compiled
// freestanding exactly as userland/libc/Makefile does and symbol-prefixed with
// "ad_" so they link next to glibc:
//
//     ../contract.c    the engine (ct_do_call: authorize, open, act, settle)
//     ../ctpromise.c   the translation layer (templates -> escrow posts)
//     ../escrow.c      the promise machinery itself (#712), unchanged
//     ../aicap.c       the capability gate and the audit sink
//
// Nothing here reimplements any of them. The harness backs their file, dir and
// clock syscalls with the host, rooted at $AIFS_ROOT.
//
// FIVE SCENARIOS, each a separate process (argv[1]) so aicap's in-memory token
// cache and the on-disk /CONFIG files start clean:
//
//   fulfilled  an action that really does create the file it promised ->
//              "ok ... promise=fulfilled paths=2/2 moves=0/0 deletes=0", CT_OK.
//   unmet      an action that RETURNS 0 AND CREATES NOTHING -> the call FAILS.
//              This is the scenario that proves the term is load-bearing: the
//              return code says success and the verdict says the postcondition
//              does not hold, and the engine believes the verdict.
//   nobackend  the same promised action with NO backend registered -> refused
//              (CT_ERR_DENIED), NOT run unverified. Fail closed.
//   moved      a CT_POST_MOVED promise: the source must be gone and the
//              destination must hold its bytes. Exercises the escrow's
//              bytes-intact identity check through a contract row.
//   timing     an action declaring CT_TIME_UNBOUNDED is REFUSED on the spawn
//              path and ALLOWED when serving live, which is the structural fix
//              for the unbounded-sys_waitpid hang (#469m). No escrow involved:
//              it belongs to the timing term, and it is here because this is the
//              rig that can run contract_run_verb() at all.
//
// Exit code: 0 = behaved exactly as asserted, 1 = it did not, 2 = harness error.
//
// WHAT THIS CANNOT PROVE, stated plainly because escrow.h and contract.h both
// state the same boundary about themselves: there is NO KERNEL here, so
// SYS_ESCROW_ENTER/_EXIT/_ABORT are stubbed successful and this rig says nothing
// about kernel ENFORCEMENT (scope, no-delete, device binding). Those are proven
// in a VM by /APPS/ESCU6 (docs/CONTRACT_ENFORCEMENT_PLAN.md 3g). What this rig
// proves is the thing task 2 asked about: that a promise declared ON A CONTRACT
// ROW reaches the escrow's postcondition oracle and that its verdict decides the
// call's outcome.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "../contract.h"   // ct_item_t / ct_terms_t / ct_promise_t (self-contained)
#include "../escrow.h"

// ---- units under test (ad_-prefixed by run_ctpromise.sh's objcopy) ----------
int  ad_contract_run_verb(const ct_contract_t *c, const char *verb, int nargs, char **args);
void ad_contract_capture_begin(char *buf, int cap);
int  ad_contract_capture_end(void);
void ad_contract_set_promise_backend(void *o, void *s);
void ad_contract_set_serving_live(int on);
void ad_contract_use_escrow_promises(void);
int  ad_contract_terms_of(const ct_item_t *it, ct_terms_t *out);
int  ad_contract_budget_ms(const ct_item_t *it);

static char g_root[512];
static int  g_fail = 0;

static void mapped(const char *vpath, char *out, int ocap) {
    if (vpath && vpath[0] == '/') snprintf(out, ocap, "%s%s", g_root, vpath);
    else                          snprintf(out, ocap, "%s/%s", g_root, vpath ? vpath : "");
}
static void BAD(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("FAIL: "); vprintf(fmt, ap); printf("\n");
    va_end(ap); g_fail = 1;
}
static void OK(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("ok:   "); vprintf(fmt, ap); printf("\n");
    va_end(ap);
}

// ===========================================================================
// THE APP UNDER TEST: a synthetic contract with promised actions, in exactly the
// shape a real app declares (Snapshot's file.save is the shipped instance).
// ===========================================================================
#define OUTDIR "/HOME/SHOTS"

static char g_last[256];                       // the "app's" own last-saved path
static int  g_last_get(char *o, int n) { snprintf(o, (size_t)n, "%s", g_last[0] ? g_last : "(none)"); return 0; }
static int  g_flag = 0;

// A real write through the SAME syscall path the shipped code uses, so the
// escrow oracle is looking at a file that genuinely appeared.
static int write_file(const char *path, const char *body) {
    char hp[700]; mapped(path, hp, sizeof hp);
    char tmp[700]; snprintf(tmp, sizeof tmp, "%s", hp);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    int fd = open(hp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t w = write(fd, body, strlen(body));
    close(fd);
    return (w == (ssize_t)strlen(body)) ? 0 : -1;
}

// HONEST action: writes the file and reports it.
static int act_save_ok(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    snprintf(g_last, sizeof g_last, "%s/SHOT-0001.BMP", OUTDIR);
    if (write_file(g_last, "BM-synthetic-capture") != 0) {
        snprintf(out, (size_t)ocap, "write-failed"); return 1;
    }
    snprintf(out, (size_t)ocap, "saved=%s", g_last);
    return 0;
}

// LYING action: returns 0, names a path, writes nothing. The whole reason the
// success/promise terms exist: "returned 0" is the action's opinion of itself.
static int act_save_lies(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    snprintf(g_last, sizeof g_last, "%s/SHOT-9999.BMP", OUTDIR);
    snprintf(out, (size_t)ocap, "saved=%s", g_last);
    return 0;                                  // and it did not
}

// SILENT action: returns 0 and never chooses a path, so the postcondition
// template can never resolve. THIS IS THE SHAPE THAT WAS MEASURED ON A BOOTED VM
// (golden 2508): Snapshot's file.save on a spawned copy found "Nothing to save",
// returned 0, and left file.last_saved reading "(unsaved)". No postcondition was
// ever registered, escrow_verify() was asked whether ZERO postconditions hold,
// said yes, and the reply claimed promise=fulfilled paths=0/0 for an action that
// promised a file and produced none. A promise with nothing left in it verifies
// VACUOUSLY unless somebody counts what was DECLARED.
static int act_save_silent(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    g_last[0] = 0;                       // nothing chosen; the item reads "(none)"
    snprintf(out, (size_t)ocap, "nothing-to-save");
    return 0;                            // and it reports success
}

// MOVE action: relocates argv[0] to argv[1]. Both paths come from the CALL, which
// is what a CT_POST_MOVED promise requires: the escrow has to hash the source
// BEFORE the move to prove the destination holds those exact bytes afterwards.
static int act_move(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it;
    if (argc < 2) { snprintf(out, (size_t)ocap, "need-a-source-and-a-destination"); return 1; }
    char src[700], dst[700];
    mapped(argv[0], src, sizeof src);
    snprintf(g_last, sizeof g_last, "%s", argv[1]);
    mapped(g_last, dst, sizeof dst);
    char tmp[700]; snprintf(tmp, sizeof tmp, "%s", dst);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    if (rename(src, dst) != 0) { snprintf(out, (size_t)ocap, "rename-failed"); return 1; }
    snprintf(out, (size_t)ocap, "moved=%s", g_last);
    return 0;
}

static int act_flag(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv; g_flag = 1;
    snprintf(out, (size_t)ocap, "flag=1"); return 0;
}

static const ct_post_t POSTS_SAVE[] = {
    { CT_POST_PATH, "item:file.last_saved", 0, 0 },
    { CT_POST_END,  0, 0, 0 }
};
static const ct_promise_t PROMISE_SAVE = {
    "synthetic save: the reported file exists afterwards and nothing is deleted",
    OUTDIR, 60, POSTS_SAVE
};
static const ct_terms_t T_SAVE = {
    CT_TIME_SLOW, 0, CT_TIME_MS_SLOW, CT_SUCC_PATH, CT_DEPTH_FS,
    CT_FAIL_IO | CT_FAIL_NOSPACE, "item:file.last_saved", &PROMISE_SAVE
};

static const ct_post_t POSTS_MOVE[] = {
    { CT_POST_MOVED, "argv:0", "argv:1", 0 },
    { CT_POST_END,   0, 0, 0 }
};
static const ct_promise_t PROMISE_MOVE = {
    "synthetic move: the source is gone and the destination holds its bytes",
    OUTDIR, 60, POSTS_MOVE
};
static const ct_terms_t T_MOVE = {
    CT_TIME_SLOW, 0, CT_TIME_MS_SLOW, CT_SUCC_PATH, CT_DEPTH_FS,
    CT_FAIL_IO, "item:file.last_saved", &PROMISE_MOVE
};

// An action whose timing is UNBOUNDED: refused on the spawn path, allowed live.
static const ct_terms_t T_UNBOUNDED = {
    CT_TIME_UNBOUNDED, 0, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW, CT_FAIL_TIMEOUT,
    "flag", 0
};

static ct_item_t ITEMS[5];
static ct_contract_t CONTRACT;

// mode: 0 = honest, 1 = lying (names a path, writes nothing),
//       2 = SILENT (returns 0, names nothing at all)
static void build_contract(int lying) {
    memset(ITEMS, 0, sizeof ITEMS);
    ITEMS[0].name = "file.last_saved"; ITEMS[0].type = CT_STR;
    ITEMS[0].access = CT_READ; ITEMS[0].risk = CT_SAFE;
    ITEMS[0].getstrfn = g_last_get; ITEMS[0].desc = "the path the app last wrote";

    ITEMS[1].name = "file.save"; ITEMS[1].type = CT_ACTION;
    ITEMS[1].access = CT_WRITE; ITEMS[1].risk = CT_SAFE;
    ITEMS[1].actfn = (lying == 2) ? act_save_silent
                   : (lying ? act_save_lies : act_save_ok);
    ITEMS[1].desc = "write the capture and promise that it exists";
    ITEMS[1].terms = &T_SAVE;

    ITEMS[2].name = "file.relocate"; ITEMS[2].type = CT_ACTION;
    ITEMS[2].access = CT_WRITE; ITEMS[2].risk = CT_SAFE;
    ITEMS[2].actfn = act_move;
    ITEMS[2].desc = "move a file in and promise it landed bytes-intact";
    ITEMS[2].terms = &T_MOVE;

    ITEMS[3].name = "flag"; ITEMS[3].type = CT_BOOL; ITEMS[3].access = CT_READ;
    ITEMS[3].risk = CT_SAFE; ITEMS[3].var = &g_flag; ITEMS[3].desc = "a readable flag";

    ITEMS[4].name = "wait.forever"; ITEMS[4].type = CT_ACTION;
    ITEMS[4].access = CT_WRITE; ITEMS[4].risk = CT_SAFE;
    ITEMS[4].actfn = act_flag;
    ITEMS[4].desc = "an action that declares it may never return";
    ITEMS[4].terms = &T_UNBOUNDED;

    memset(&CONTRACT, 0, sizeof CONTRACT);
    CONTRACT.app = "synth"; CONTRACT.title = "Synthetic";
    CONTRACT.note = "the host rig for the contract promise terms";
    CONTRACT.items = ITEMS; CONTRACT.n = 5;
}

// Run one verb through the REAL engine, capturing the reply lines it emits.
static int call(const char *verb, int n, char **a, char *reply, int rcap) {
    ad_contract_capture_begin(reply, rcap);
    int rc = ad_contract_run_verb(&CONTRACT, verb, n, a);
    ad_contract_capture_end();
    return rc;
}

static int exists(const char *vpath) {
    char hp[700]; mapped(vpath, hp, sizeof hp);
    struct stat st; return stat(hp, &st) == 0;
}

// ===========================================================================
int main(int argc, char **argv) {
    const char *r = getenv("AIFS_ROOT");
    if (!r || !*r) { printf("harness: AIFS_ROOT unset\n"); return 2; }
    snprintf(g_root, sizeof g_root, "%s", r);
    const char *sc = (argc > 1) ? argv[1] : "fulfilled";
    // 8192, not 1024: a describe is the WHOLE table, and a short capture buffer
    // truncates it in exactly the way the describe scenario is asserting must not
    // happen, so a small buffer here would report a fault in contract.c that is
    // really a fault in the rig. Caught on the first run of that scenario.
    static char reply[8192]; char *av[4];

    if (!strcmp(sc, "fulfilled")) {
        build_contract(0);
        ad_contract_use_escrow_promises();
        av[0] = (char *)"file.save";
        int rc = call("call", 1, av, reply, sizeof reply);
        printf("      reply: %s", reply);
        if (rc != CT_OK) BAD("an honest promised action returned %d, expected CT_OK", rc);
        else OK("the honest promised action succeeded");
        if (!strstr(reply, "promise=fulfilled"))
            BAD("the reply line carries no fulfilled verdict, so the escrow "
                "oracle was not consulted: %s", reply);
        else OK("the reply carries the ESCROW's own verdict, not just the rc");
        if (!strstr(reply, "deletes=0"))
            BAD("the verdict does not report the escrow delete counter");
        else OK("the verdict reports the escrow no-delete counter");
        if (!exists(OUTDIR "/SHOT-0001.BMP")) BAD("the file was not actually written");
        else OK("the promised file really exists on disk");
        return g_fail;
    }

    // THE LOAD-BEARING SCENARIO. The action returns 0 and creates nothing.
    if (!strcmp(sc, "unmet")) {
        build_contract(1);
        ad_contract_use_escrow_promises();
        av[0] = (char *)"file.save";
        int rc = call("call", 1, av, reply, sizeof reply);
        printf("      reply: %s", reply);
        if (rc == CT_OK)
            BAD("an action that returned 0 and wrote NOTHING was reported as "
                "success. The promise term is then decoration: the engine "
                "believed the action's own opinion of itself.");
        else OK("a promised action whose postcondition does NOT hold FAILS the "
                "call even though it returned 0 (rc=%d)", rc);
        if (!strstr(reply, "promise-unmet") && !strstr(reply, "unmet"))
            BAD("the reply does not say the promise was unmet: %s", reply);
        else OK("the reply names the unmet promise, so a caller can see why");
        if (exists(OUTDIR "/SHOT-9999.BMP")) BAD("the rig wrote the file itself");
        else OK("the file genuinely does not exist, as the oracle found");
        return g_fail;
    }

    // FAIL CLOSED: a promise with no backend must not run unverified.
    if (!strcmp(sc, "nobackend")) {
        build_contract(0);
        ad_contract_set_promise_backend(0, 0);   // explicitly none
        av[0] = (char *)"file.save";
        int rc = call("call", 1, av, reply, sizeof reply);
        printf("      reply: %s", reply);
        if (rc == CT_OK)
            BAD("a promised action RAN with no backend to check its promise. "
                "That is the worst case: the declaration survives and nothing "
                "verifies it.");
        else OK("a promised action with no backend is REFUSED, not run "
                "unverified (rc=%d)", rc);
        if (exists(OUTDIR "/SHOT-0001.BMP"))
            BAD("the action ran despite the refusal");
        else OK("the action did not run at all");
        return g_fail;
    }

    // CT_POST_MOVED through a contract row.
    if (!strcmp(sc, "moved")) {
        build_contract(0);
        ad_contract_use_escrow_promises();
        if (write_file(OUTDIR "/SOURCE.BIN", "payload-bytes-1234") != 0) {
            printf("harness: could not seed the source\n"); return 2;
        }
        av[0] = (char *)"file.relocate";
        av[1] = (char *)(OUTDIR "/SOURCE.BIN");
        av[2] = (char *)(OUTDIR "/MOVED.BIN");
        int rc = call("call", 3, av, reply, sizeof reply);
        printf("      reply: %s", reply);
        if (rc != CT_OK) BAD("a real move failed its promise (rc=%d): %s", rc, reply);
        else OK("a CT_POST_MOVED promise verified through the escrow's "
                "bytes-intact identity check");
        if (!strstr(reply, "moves=1/1"))
            BAD("the verdict does not report 1/1 moves: %s", reply);
        else OK("the verdict reports the move count from the escrow oracle");
        if (exists(OUTDIR "/SOURCE.BIN")) BAD("the source is still there");
        else OK("the source is gone, as the promise required");
        if (!exists(OUTDIR "/MOVED.BIN")) BAD("the destination does not exist");
        else OK("the destination exists");
        return g_fail;
    }

    // THE TIMING TERM. No escrow: this is the unbounded-wait refusal.
    if (!strcmp(sc, "timing")) {
        build_contract(0);
        ad_contract_use_escrow_promises();

        ct_terms_t t;
        if (!ad_contract_terms_of(&ITEMS[3], &t)) BAD("a value row derived no terms");
        else if (t.timing != CT_TIME_INSTANT || t.success != CT_SUCC_ITEM)
            BAD("a value row derived the wrong terms (timing=%d success=%d)",
                t.timing, t.success);
        else OK("a value row's terms are DERIVED, not declared (instant, "
                "check-the-item), so a value row cannot carry a wrong one");
        if (ad_contract_budget_ms(&ITEMS[4]) != 0)
            BAD("an UNBOUNDED action reported a nonzero budget, so a caller "
                "would wait on it");
        else OK("an UNBOUNDED action reports budget 0: do not wait");
        if (ad_contract_budget_ms(&ITEMS[1]) != CT_TIME_MS_SLOW)
            BAD("a SLOW action did not report the slow budget");
        else OK("a declared class yields its ceiling as the caller's budget");

        av[0] = (char *)"wait.forever";
        ad_contract_set_serving_live(0);
        int rc = call("call", 1, av, reply, sizeof reply);
        printf("      spawn-path reply: %s", reply);
        if (rc == CT_OK || g_flag)
            BAD("an action declaring CT_TIME_UNBOUNDED RAN on the spawn path. "
                "contract_invoke() waits on it with sys_waitpid and no timeout, "
                "so this is the measured #469m hang re-armed.");
        else OK("an UNBOUNDED action is REFUSED on the spawn path with a reason "
                "the caller can read, instead of hanging it (rc=%d)", rc);

        ad_contract_set_serving_live(1);
        rc = call("call", 1, av, reply, sizeof reply);
        ad_contract_set_serving_live(0);
        printf("      live-path reply: %s", reply);
        if (rc != CT_OK || !g_flag)
            BAD("the SAME action was refused on the LIVE path too (rc=%d), which "
                "would make it unreachable rather than merely unsafe to spawn", rc);
        else OK("the same action IS allowed when serving a live instance, which "
                "has a bounded wait (CTLIVE_WAIT_MS -> CT_LIVE_TIMEOUT)");
        return g_fail;
    }

    // THE DESCRIBE LINE MUST NOT TRUNCATE. A truncated describe is the worst
    // failure mode this API has: the line still parses, it just stops mid-field,
    // so a consumer reads a row missing its desc or half its promise and cannot
    // tell. Adding the terms pushed the promised row past ct_line()'s old
    // 512-byte buffer, and only this assertion noticed.
    if (!strcmp(sc, "describe")) {
        build_contract(0);
        ad_contract_use_escrow_promises();
        int rc = call("describe", 0, av, reply, sizeof reply);
        printf("%s", reply);
        if (rc != CT_OK) BAD("describe returned %d", rc);
        // Find the file.save row and check it survived whole: its LAST field is
        // desc, and its promise sits before that, so both present means nothing
        // in between was cut.
        const char *row = strstr(reply, "name: file.save,");
        if (!row) { BAD("describe emitted no file.save row at all"); return g_fail; }
        const char *end = strchr(row, '\n');
        int len = end ? (int)(end - row) : (int)strlen(row);
        char line[1600];
        if (len > (int)sizeof(line) - 1) len = (int)sizeof(line) - 1;
        memcpy(line, row, (size_t)len); line[len] = 0;
        OK("the file.save describe row is %d bytes", len);
        if (!strstr(line, "promise: {"))
            BAD("the row carries no promise field: %s", line);
        else OK("the row carries its promise");
        if (!strstr(line, "posts: [path:item:file.last_saved]"))
            BAD("the promise posts were cut or malformed: %s", line);
        else OK("the promise posts survived whole");
        if (!strstr(line, "desc: "))
            BAD("the row was TRUNCATED before its desc, which is the last field: %s", line);
        else OK("the row reaches its desc, so nothing in between was cut");
        if (!strstr(line, "timing: slow") || !strstr(line, "depth: fs"))
            BAD("the row is missing a term: %s", line);
        else OK("timing and depth are both present");
        // And a DERIVED row must describe its terms too, not leave them blank.
        const char *vrow = strstr(reply, "name: flag,");
        if (!vrow) BAD("describe emitted no flag row");
        else if (!strstr(vrow, "timing: instant"))
            BAD("a value row's DERIVED terms are not in describe");
        else OK("a value row's terms are described as derived (instant), so a "
                "consumer need not know which rows declare");
        return g_fail;
    }

    // THE VACUOUS-FULFILLED SCENARIO. Measured on a booted VM before it was a
    // test: an action that returns 0 without ever naming the path it promised
    // leaves the oracle with nothing to check, and "nothing failed" is not
    // "the promise held".
    if (!strcmp(sc, "vacuous")) {
        build_contract(2);
        ad_contract_use_escrow_promises();
        av[0] = (char *)"file.save";
        int rc = call("call", 1, av, reply, sizeof reply);
        printf("      reply: %s", reply);
        if (rc == CT_OK)
            BAD("an action that returned 0 and named NO path was reported as a "
                "fulfilled promise. A promise with zero registered postconditions "
                "verifies vacuously, and that is indistinguishable from the "
                "\"returned 0 therefore it worked\" claim these terms replace.");
        else OK("a promise whose postconditions could never be pinned to a path "
                "FAILS, rather than verifying vacuously (rc=%d)", rc);
        if (!strstr(reply, "unresolved=1"))
            BAD("the verdict does not report the unresolved count: %s", reply);
        else OK("the verdict reports declared_fs and unresolved, so a caller can "
                "see that the oracle was asked nothing");
        if (strstr(reply, "promise=fulfilled"))
            BAD("the verdict still says fulfilled: %s", reply);
        else OK("the verdict says unmet");
        return g_fail;
    }

    printf("harness: unknown scenario '%s'\n", sc);
    return 2;
}

// ===========================================================================
// THE SHIM LAYER. Same shape as escrow_test.c's: the UUT's syscalls are backed
// by the host FS rooted at $AIFS_ROOT, its libc primitives by host libc.
// ===========================================================================
#define M_O_CREAT 0x0040
int ad_getuid(void) { return 0; }
int ad_strcasecmp(const char *a, const char *b) { return strcasecmp(a, b); }
char *ad_strrchr(const char *s, int c) { return strrchr((char *)s, c); }
char *ad_strchr(const char *s, int c) { return strchr((char *)s, c); }

int ad_userconf_path(const char *name, char *out, unsigned long cap) {
    int n = snprintf(out, (size_t)cap, "/CONFIG/%s", name ? name : "");
    return (n > 0 && (unsigned long)n < cap) ? 0 : -1;
}
// userhome_dir() is what ctpromise.c's "home:<rel>" template resolves through.
// The rig models a root session, so home is "/", matching ad_userconf_path above.
int ad_userhome_dir(const char *sub, char *out, unsigned long cap) {
    int n = snprintf(out, (size_t)cap, "/%s", sub ? sub : "");
    return (n > 0 && (unsigned long)n < cap) ? 0 : -1;
}
int ad_userhome_path(const char *sub, const char *name, char *out, unsigned long cap) {
    int n = sub && sub[0] ? snprintf(out, (size_t)cap, "/%s/%s", sub, name ? name : "")
                          : snprintf(out, (size_t)cap, "/%s", name ? name : "");
    return (n > 0 && (unsigned long)n < cap) ? 0 : -1;
}
int ad_userhome_root(char *out, unsigned long cap) {
    snprintf(out, (size_t)cap, "/"); return 0;
}
int  ad_userconf_open_read(const char *a, const char *b) { (void)a; (void)b; return -1; }
int  ad_userconf_open_write(const char *a) {
    char hp[700]; char v[300];
    snprintf(v, sizeof v, "/CONFIG/%s", a ? a : "x");
    mapped(v, hp, sizeof hp);
    char tmp[700]; snprintf(tmp, sizeof tmp, "%s", hp);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    return open(hp, O_CREAT | O_WRONLY | O_APPEND, 0644);
}
void ad_uc_report_write_refusal(const char *w, const char *p, int rc) { (void)w;(void)p;(void)rc; }
void ad_userconf_ensure_parent_dir(const char *path) {
    char hp[700]; mapped(path, hp, (int)sizeof(hp));
    char *last = 0;
    for (char *s = hp; *s; s++) if (*s == '/') last = s;
    if (last && last != hp) { *last = 0; mkdir(hp, 0755); }
}
void ad_userconf_protect(const char *p) { (void)p; }
static void host_mkparents(const char *hostpath);
int  ad_userconf_finish_write(int fd, const char *n) { (void)n; if (fd >= 0) close(fd); return 0; }
// contract.c emits its reply lines through write(2) on fd 1; the harness lets
// them through so a failure prints the actual line a caller would have read.
long ad_write(int fd, const void *b, unsigned long n) { return write(fd, b, (size_t)n); }
long ad_read(int fd, void *b, unsigned long n) { return read(fd, b, (size_t)n); }
int  ad_close(int fd) { return close(fd); }
int  ad_open(const char *p, int fl, ...) {
    char hp[700]; mapped(p, hp, sizeof hp);
    if (fl & M_O_CREAT) host_mkparents(hp);
    return open(hp, fl, 0644);
}
int  ad_aidev_authorize_tool(const char *t, const char *a, char *o, int c) {
    (void)t; (void)a; if (o && c) o[0] = 0; return 1;
}

static void host_mkparents(const char *hostpath) {
    char tmp[700]; snprintf(tmp, sizeof(tmp), "%s", hostpath);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
}

#define SYS_OPEN 10
#define SYS_CLOSE 11
#define SYS_READ 12
#define SYS_WRITE 13
#define SYS_SEEK 14
#define SYS_STAT 15
#define SYS_MKDIR 16
#define SYS_UNLINK 18
#define SYS_READDIR 19
#define SYS_TIME 50
#define SYS_RENAME 70

typedef struct {
    unsigned long m_dev, m_ino;
    unsigned int  m_mode, m_nlink, m_uid, m_gid;
    unsigned long m_rdev;
    long          m_size, m_blksize, m_blocks;
    unsigned long m_atime, m_mtime, m_ctime;
} mstat_t;
typedef struct { char name[256]; unsigned int type; unsigned int size; } mdirent_t;

#define DIRFD_BASE 100000
static DIR *g_dirs[64];

long ad_syscall0(long n) { if (n == SYS_TIME) return (long)time(NULL); return 0; }
long ad_syscall1(long n, long a) {
    if (n == SYS_CLOSE) {
        if (a >= DIRFD_BASE) { int s = (int)(a - DIRFD_BASE);
            if (s >= 0 && s < 64 && g_dirs[s]) { closedir(g_dirs[s]); g_dirs[s] = 0; } return 0; }
        return close((int)a);
    }
    if (n == SYS_UNLINK) { char hp[700]; mapped((const char *)a, hp, sizeof hp); return unlink(hp); }
    return 0;
}
long ad_syscall2(long n, long a, long b) {
    if (n == SYS_OPEN) {
        char hp[700]; mapped((const char *)a, hp, sizeof hp);
        struct stat st;
        if (stat(hp, &st) == 0 && S_ISDIR(st.st_mode)) {
            for (int s = 0; s < 64; s++) if (!g_dirs[s]) {
                DIR *d = opendir(hp); if (!d) return -1;
                g_dirs[s] = d; return DIRFD_BASE + s;
            }
            return -1;
        }
        if (((int)b) & M_O_CREAT) host_mkparents(hp);
        return open(hp, (int)b, 0644);
    }
    if (n == SYS_STAT) {
        char hp[700]; mapped((const char *)a, hp, sizeof hp);
        struct stat hs;
        if (stat(hp, &hs) != 0) return -1;
        mstat_t *m = (mstat_t *)b; memset(m, 0, sizeof(*m));
        m->m_mode = hs.st_mode; m->m_size = hs.st_size;
        m->m_mtime = (unsigned long)hs.st_mtime; m->m_nlink = 1;
        return 0;
    }
    if (n == SYS_MKDIR) {
        char hp[700]; mapped((const char *)a, hp, sizeof hp);
        host_mkparents(hp); return mkdir(hp, (int)b);
    }
    if (n == SYS_RENAME) {
        char op[700], np[700];
        mapped((const char *)a, op, sizeof op);
        mapped((const char *)b, np, sizeof np);
        host_mkparents(np);
        return rename(op, np);
    }
    if (n == SYS_READDIR) {
        if (a < DIRFD_BASE) return 1;
        int s = (int)(a - DIRFD_BASE);
        if (s < 0 || s >= 64 || !g_dirs[s]) return 1;
        struct dirent *de = readdir(g_dirs[s]);
        mdirent_t *m = (mdirent_t *)b;
        if (!de) { m->name[0] = 0; return 1; }
        snprintf(m->name, sizeof(m->name), "%s", de->d_name);
        int isdir = 0; unsigned sz = 0;
        struct stat es;
        if (fstatat(dirfd(g_dirs[s]), de->d_name, &es, 0) == 0) {
            isdir = S_ISDIR(es.st_mode); sz = (unsigned)es.st_size;
        } else isdir = (de->d_type == DT_DIR);
        m->type = isdir ? 1 : 0; m->size = sz;
        return 0;
    }
    return 0;
}
long ad_syscall3(long n, long a, long b, long c) {
    if (n == SYS_READ)  return read((int)a, (void *)b, (size_t)c);
    if (n == SYS_WRITE) return write((int)a, (const void *)b, (size_t)c);
    if (n == SYS_SEEK)  return lseek((int)a, b, (int)c);
    return 0;
}
long ad_syscall4(long n, long a, long b, long c, long d) { (void)n;(void)a;(void)b;(void)c;(void)d; return 0; }
long ad_syscall5(long n, long a, long b, long c, long d, long e) { (void)n;(void)a;(void)b;(void)c;(void)d;(void)e; return 0; }
long ad_syscall6(long n, long a, long b, long c, long d, long e, long f) { (void)n;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return 0; }

int   ad_snprintf(char *s, size_t n, const char *f, ...) { va_list ap; va_start(ap,f); int r=vsnprintf(s,n,f,ap); va_end(ap); return r; }
int   ad_vsnprintf(char *s, size_t n, const char *f, va_list ap) { return vsnprintf(s,n,f,ap); }
int   ad_printf(const char *f, ...) { va_list ap; va_start(ap,f); int r=vprintf(f,ap); va_end(ap); return r; }
void *ad_malloc(size_t n) { return malloc(n); }
void  ad_free(void *p) { free(p); }
void *ad_memcpy(void *d, const void *s, size_t n) { return memcpy(d,s,n); }
void *ad_memmove(void *d, const void *s, size_t n) { return memmove(d,s,n); }
void *ad_memset(void *d, int c, size_t n) { return memset(d,c,n); }
int   ad_strcmp(const char *a, const char *b) { return strcmp(a,b); }
int   ad_strncmp(const char *a, const char *b, size_t n) { return strncmp(a,b,n); }
size_t ad_strlen(const char *s) { return strlen(s); }
char *ad_strstr(const char *h, const char *n) { return strstr((char*)h,n); }
size_t ad_strlcpy(char *d, const char *s, size_t n) {
    size_t sl = strlen(s);
    if (n) { size_t c = sl < n-1 ? sl : n-1; memcpy(d,s,c); d[c]=0; }
    return sl;
}
