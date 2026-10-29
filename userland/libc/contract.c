// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// contract.c - engine for the per-app tool-contract API (#233).
// See contract.h for the two invariants (non-drift, completeness) and why
// projection is the preferred form.
//
// This file deliberately contains NO policy about any particular app. It knows
// how to walk a ct_contract_t, validate a value against the row's declared
// range, route the write to the row's backing store, ask libc/aicap.c whether
// the call is allowed, and emit the result. Everything app-specific lives in
// the app, in the same table that draws its controls.

#include "contract.h"
#include "aicap.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "stdlib.h"    // open() is declared here, not in fcntl.h (see ctinvoke.c)
#include "fcntl.h"
#include "userconf.h"  // #239: the ONE place that knows where a preference lives

// ---------------------------------------------------------------------------
// Output. Everything goes to fd 1 in one write() per line: the kernel's fd-1
// fallback (kernel/proc/fdlayer.c:1468) puts stdout on the serial console and
// mirrors it into syslog, so a harness with no GUI reads the result off the
// serial socket. One write per line keeps lines from interleaving with an
// app's own logging.
// ---------------------------------------------------------------------------
// When a capture buffer is active (contract_capture_begin), output is appended
// there instead of going to fd 1. This is what lets contract_live_poll() run
// the SAME get/set/call/describe path the CLI runs and hand the app's reply
// lines back to the live caller, with zero second copy of the emit logic.
static char *g_cap_buf = 0;
static int   g_cap_max = 0;
static int   g_cap_len = 0;

void contract_capture_begin(char *buf, int cap) {
    g_cap_buf = buf; g_cap_max = cap; g_cap_len = 0;
    if (buf && cap > 0) buf[0] = 0;
}
int contract_capture_end(void) {
    int n = g_cap_len;
    g_cap_buf = 0; g_cap_max = 0; g_cap_len = 0;
    return n;
}

static void ct_emit(const char *s) {
    size_t n = 0; while (s[n]) n++;
    if (g_cap_buf) {
        for (size_t i = 0; i < n && g_cap_len < g_cap_max - 1; i++)
            g_cap_buf[g_cap_len++] = s[i];
        if (g_cap_max > 0) g_cap_buf[g_cap_len] = 0;
        return;
    }
    if (n) write(1, s, n);
}

static void ct_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ct_line(const char *fmt, ...) {
    // 1536, NOT 512. MEASURED: adding the contract terms (#<config-ref>) pushed a
    // describe row past 512 and ct_line silently TRUNCATED it, which is the worst
    // possible failure for a describe: the line still parses, it just stops
    // mid-field, so a consumer reads a row that is missing its desc or half its
    // promise and has no way to tell. The worst case a row can produce is
    // name 64 + bounds/options 256 + cfgkey 24 + cap 80 + needs 192 + terms 512 +
    // desc 128 plus the literal field names, about 1376 bytes. Caught by the
    // host test asserting a described row still carries BOTH its promise and its
    // desc, which is the assertion to keep if this line ever grows again.
    char b[1536];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    __builtin_va_end(ap);
    ct_emit(b);
}

// ---------------------------------------------------------------------------
// Small helpers (freestanding: no stdlib)
// ---------------------------------------------------------------------------
static int ct_atoi(const char *s, int *ok) {
    int v = 0, neg = 0, any = 0;
    if (!s) { if (ok) *ok = 0; return 0; }
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    if (ok) *ok = any && (*s == '\0');
    return neg ? -v : v;
}

static const char *ct_typename(int t) {
    switch (t) {
        case CT_BOOL:   return "bool";
        case CT_INT:    return "int";
        case CT_ENUM:   return "enum";
        case CT_ACTION: return "action";
        case CT_STR:    return "string";
    }
    return "?";
}

static const char *ct_riskname(int r) {
    switch (r) {
        case CT_SAFE:    return "safe";
        case CT_GUARDED: return "guarded";
        case CT_DENIED:  return "denied";
    }
    return "?";
}

static const char *ct_codename(int rc) {
    switch (rc) {
        case CT_OK:          return "ok";
        case CT_ERR_UNKNOWN: return "unknown-item";
        case CT_ERR_RANGE:   return "out-of-range";
        case CT_ERR_ACCESS:  return "access-denied";
        case CT_ERR_DENIED:  return "capability-denied";
        case CT_ERR_USAGE:   return "usage";
        case CT_ERR_FAILED:  return "failed";
    }
    return "?";
}

static const char *ct_access_str(int a) {
    if ((a & CT_RW) == CT_RW) return "rw";
    if (a & CT_WRITE)         return "w";
    if (a & CT_READ)          return "r";
    return "-";
}

// ---------------------------------------------------------------------------
// Table walking
// ---------------------------------------------------------------------------
int contract_count(const ct_contract_t *c) {
    int n = c->items ? c->n : 0;
    if (c->project) {
        ct_item_t tmp;
        int i = 0;
        // Bounded: a projection that never terminates is a bug in the app, and
        // an unbounded loop here would hang every enumeration including the
        // AI's. 4096 is far above any real control surface (Settings' whole
        // audited surface is 128).
        while (i < 4096 && c->project(i, &tmp)) i++;
        n += i;
    }
    return n;
}

int contract_at(const ct_contract_t *c, int idx, ct_item_t *out) {
    if (idx < 0) return 0;
    int ns = c->items ? c->n : 0;
    if (idx < ns) { *out = c->items[idx]; return 1; }
    if (!c->project) return 0;
    // Zero the row before the app fills it, so a projector written before the
    // tier-2 `needs` field existed leaves it NULL ("needs nothing") rather than
    // stack garbage. A designated-initializer projector already does this; this
    // makes it true for the field-by-field kind too.
    for (unsigned i = 0; i < sizeof(*out); i++) ((char *)out)[i] = 0;
    return c->project(idx - ns, out);
}

int contract_find(const ct_contract_t *c, const char *name, ct_item_t *out) {
    ct_item_t it;
    for (int i = 0; contract_at(c, i, &it); i++) {
        if (it.name && !strcmp(it.name, name)) { *out = it; return 1; }
    }
    return 0;
}

int contract_by_key(const ct_contract_t *c, char cfgkey, ct_item_t *out) {
    if (!cfgkey) return 0;
    ct_item_t it;
    for (int i = 0; contract_at(c, i, &it); i++) {
        if (it.cfgkey == cfgkey) { *out = it; return 1; }
    }
    return 0;
}

int contract_get(const ct_item_t *it) {
    if (it->var)   return *it->var;
    if (it->getfn) return it->getfn();
    return 0;
}

int contract_put(const ct_item_t *it, int v) {
    if (it->var)   { *it->var = v; return 0; }
    if (it->setfn) return it->setfn(v);
    return -1;
}

// FNV-1a over every persisted row's (key, value). DERIVED from the table, so a
// new persisted row is watched the moment it exists. #231 removed exactly this
// fault from the widget serializer, where a hand-maintained hash had silently
// stopped watching three settings.
int contract_hash(const ct_contract_t *c) {
    unsigned int h = 2166136261u;
    ct_item_t it;
    for (int i = 0; contract_at(c, i, &it); i++) {
        if (!it.cfgkey) continue;
        int v = contract_get(&it);
        unsigned char b[5];
        b[0] = (unsigned char)it.cfgkey;
        b[1] = (unsigned char)(v & 0xFF);
        b[2] = (unsigned char)((v >> 8) & 0xFF);
        b[3] = (unsigned char)((v >> 16) & 0xFF);
        b[4] = (unsigned char)((v >> 24) & 0xFF);
        for (int k = 0; k < 5; k++) { h ^= b[k]; h *= 16777619u; }
    }
    return (int)(h & 0x7FFFFFFF);
}

// ---------------------------------------------------------------------------
// Derived per-app persistence (#239). See contract.h for why this is here and
// not copied into each app.
//
// The file is trivially small (one "<key>=<int>" line per persisted row), so
// both directions fit in one buffer and neither needs a parser. The format is
// deliberately the same single-char-key shape /CONFIG/SETTINGS.CFG already
// uses, rather than a new dialect.
// ---------------------------------------------------------------------------
#define CT_CFG_MAX 512

// A row is persistable only if it has a key AND an integer backing store we can
// both read and write. Stated once, so save and load can never disagree about
// which rows are in the file.
static int ct_persistable(const ct_item_t *it) {
    if (!it->cfgkey) return 0;
    if (it->type == CT_STR || it->type == CT_ACTION) return 0;
    if (!(it->access & CT_READ) || !(it->access & CT_WRITE)) return 0;
    return it->var || (it->getfn && it->setfn);
}

int contract_save_cfg(const ct_contract_t *c, const char *name) {
    char buf[CT_CFG_MAX];
    int used = 0;
    ct_item_t it;
    for (int i = 0; contract_at(c, i, &it); i++) {
        if (!ct_persistable(&it)) continue;
        char line[32];
        snprintf(line, sizeof(line), "%c=%d\n", it.cfgkey, contract_get(&it));
        int n = 0; while (line[n]) n++;
        // Refuse rather than truncate: half a preference file is a file that
        // silently loses whichever rows sorted last.
        if (used + n >= CT_CFG_MAX) return -1;
        for (int k = 0; k < n; k++) buf[used++] = line[k];
    }
    int fd = userconf_open_write(name);
    if (fd < 0) return -1;
    // userconf_finish_write() ALWAYS closes fd, on every path.
    return userconf_finish_write(fd, buf, (unsigned long)used);
}

int contract_load_cfg(const ct_contract_t *c, const char *name,
                      const char *legacy) {
    int fd = userconf_open_read(name, legacy);
    if (fd < 0) return -1;
    char buf[CT_CFG_MAX];
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;

    for (long i = 0; i < n; ) {
        long e = i;
        while (e < n && buf[e] != '\n') e++;
        buf[e] = 0;
        // "<key>=<int>". Anything else in the file is ignored rather than
        // rejected: an older build's key that no longer has a row must not
        // stop the rows that do have one from loading.
        if (e - i >= 3 && buf[i + 1] == '=') {
            char key = buf[i];
            int ok = 0;
            int v = ct_atoi(buf + i + 2, &ok);
            ct_item_t it;
            if (ok && contract_by_key(c, key, &it) && ct_persistable(&it)) {
                int lo = it.lo, hi = it.hi;
                if (it.type == CT_BOOL) { lo = 0; hi = 1; }
                // The file is INPUT. Range-check it exactly as ct_do_set()
                // does: an unchecked index out of a corrupt file would run off
                // the end of the app's own table.
                if (v >= lo && v <= hi) contract_put(&it, v);
            }
        }
        i = e + 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Authorization. ONE site, so #305's move of this into the immutable core is
// mechanical, exactly as aicap.h promises for the AI tool loop.
// ---------------------------------------------------------------------------

// ===========================================================================
// THE CONTRACT TERMS (#<config-ref>): derive, bound, and REFUSE rather than hang.
// See contract.h for why these three terms and why most rows declare none.
// ===========================================================================

static ct_promise_open_fn   g_prom_open   = 0;
static ct_promise_settle_fn g_prom_settle = 0;
static int                  g_serving_live = 0;

void contract_set_promise_backend(ct_promise_open_fn open_fn,
                                  ct_promise_settle_fn settle_fn) {
    g_prom_open = open_fn; g_prom_settle = settle_fn;
}
int  contract_serving_live(void) { return g_serving_live; }
void contract_set_serving_live(int on) { g_serving_live = on ? 1 : 0; }

int contract_terms_of(const ct_item_t *it, ct_terms_t *out) {
    if (!it || !out) return 0;
    if (it->terms) { *out = *it->terms; return 1; }

    // DERIVED. Not a default: for a value row each of these follows from what
    // the row already declares, so a hand-written copy could only ever be a
    // second version of the same fact. An ACTION row is not derivable and
    // returns 0, which the engine treats as unbounded.
    memset(out, 0, sizeof(*out));
    switch (it->type) {
    case CT_BOOL:
    case CT_INT:
    case CT_ENUM:
        out->timing      = CT_TIME_INSTANT;
        out->budget_ms   = CT_TIME_MS_INSTANT;
        out->success     = CT_SUCC_ITEM;
        out->success_ref = it->name;          // read it back: it equals what you wrote
        out->depth       = it->cfgkey ? CT_DEPTH_PREF : CT_DEPTH_VIEW;
        out->failures    = CT_FAIL_RANGE | (it->risk == CT_SAFE ? 0u : CT_FAIL_DENIED);
        return 1;
    case CT_STR:
        out->timing    = CT_TIME_INSTANT;
        out->budget_ms = CT_TIME_MS_INSTANT;
        out->success   = CT_SUCC_RC;          // a read either answered or did not
        out->depth     = CT_DEPTH_VIEW;
        out->failures  = (it->risk == CT_SAFE ? 0u : CT_FAIL_DENIED);
        return 1;
    default:
        return 0;                             // CT_ACTION: must be declared
    }
}

int contract_budget_ms(const ct_item_t *it) {
    ct_terms_t t;
    if (!contract_terms_of(it, &t)) return 0;
    if (t.budget_ms) return (int)t.budget_ms;
    switch (t.timing) {
    case CT_TIME_INSTANT: return CT_TIME_MS_INSTANT;
    case CT_TIME_FAST:    return CT_TIME_MS_FAST;
    case CT_TIME_SLOW:    return CT_TIME_MS_SLOW;
    case CT_TIME_LONG:    return CT_TIME_MS_LONG;
    default:              return 0;   // UNSPEC / UNBOUNDED: do not wait
    }
}

static const char *ct_timename(int t) {
    switch (t) {
    case CT_TIME_INSTANT:   return "instant";
    case CT_TIME_FAST:      return "fast";
    case CT_TIME_SLOW:      return "slow";
    case CT_TIME_LONG:      return "long";
    case CT_TIME_UNBOUNDED: return "unbounded";
    default:                return "unspecified";
    }
}
static const char *ct_succname(int s) {
    switch (s) {
    case CT_SUCC_RC:    return "return-code-only";
    case CT_SUCC_ITEM:  return "item";
    case CT_SUCC_PATH:  return "path";
    case CT_SUCC_ARMED: return "armed";
    default:            return "unspecified";
    }
}
static const char *ct_depthname(int d) {
    switch (d) {
    case CT_DEPTH_VIEW:   return "view";
    case CT_DEPTH_PREF:   return "pref";
    case CT_DEPTH_DOC:    return "doc";
    case CT_DEPTH_FS:     return "fs";
    case CT_DEPTH_SYSTEM: return "system";
    default:              return "unspecified";
    }
}

const char *contract_failures_str(unsigned int mask, char *out, int ocap) {
    static const struct { unsigned int bit; const char *nm; } tbl[] = {
        { CT_FAIL_DENIED, "denied" }, { CT_FAIL_RANGE, "range" },
        { CT_FAIL_NOTFOUND, "notfound" }, { CT_FAIL_BUSY, "busy" },
        { CT_FAIL_NOSPACE, "nospace" }, { CT_FAIL_IO, "io" },
        { CT_FAIL_NEEDSLIVE, "needslive" }, { CT_FAIL_TIMEOUT, "timeout" },
        { CT_FAIL_UNSUPPORTED, "unsupported" }, { CT_FAIL_PARTIAL, "partial" },
    };
    int o = 0;
    if (ocap > 0) out[0] = 0;
    for (unsigned i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (!(mask & tbl[i].bit)) continue;
        o += snprintf(out + o, (size_t)(ocap - o), "%s%s", o ? "|" : "", tbl[i].nm);
        if (o >= ocap - 1) break;
    }
    return out;
}

// Emit the terms of one row as flow-map fields, appended to the describe line.
// Written for EVERY row including a derived one, because a consumer must not
// have to know which rows derive: "timing: instant" is true either way, and an
// absent field would read as "no bound stated" when a bound is known.
// APPEND WITH THE REMAINING ROOM, NEVER WITH (ocap - o).
//
// snprintf() returns the length it WOULD have written, so `o` can exceed ocap on
// a truncating call. `(size_t)(ocap - o)` is then a negative int widened to a
// huge size_t, and the next snprintf believes it has 18 exabytes of buffer. The
// strings here are bounded well under 512 today, so this was not reachable, but
// "not reachable today" is how the next field-widening turns a describe into a
// stack overwrite. ct_apnd caps the length and clamps `o`, so the arithmetic
// cannot go negative no matter what a row declares.
static int ct_apnd(char *out, int o, int ocap, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static int ct_apnd(char *out, int o, int ocap, const char *fmt, ...) {
    if (o < 0) o = 0;
    if (o >= ocap - 1) return ocap - 1;        // full: keep the NUL, write nothing
    // __builtin_va_* to match ct_line() above: this TU is freestanding and does
    // not include stdarg.h, and adding an include for one function would be a
    // second way of doing the same thing in the same file.
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(out + o, (size_t)(ocap - o), fmt, ap);
    __builtin_va_end(ap);
    if (n < 0) return o;
    o += n;
    if (o > ocap - 1) o = ocap - 1;            // it truncated; stop believing n
    return o;
}

static void ct_terms_fields(const ct_item_t *it, char *out, int ocap) {
    ct_terms_t t;
    if (ocap > 0) out[0] = 0;
    if (ocap < 8) return;
    if (!contract_terms_of(it, &t)) {
        ct_apnd(out, 0, ocap, ", timing: unspecified, budget_ms: 0, "
                "success: unspecified, depth: unspecified, failures: []");
        return;
    }
    char fails[128]; contract_failures_str(t.failures, fails, (int)sizeof(fails));
    char flags[48]; int fo = 0; flags[0] = 0;
    if (t.tflags & CT_TF_LIVE_ONLY) fo = ct_apnd(flags, fo, (int)sizeof(flags), "%slive-only", fo ? "|" : "");
    if (t.tflags & CT_TF_ASYNC)     fo = ct_apnd(flags, fo, (int)sizeof(flags), "%sasync", fo ? "|" : "");
    if (t.tflags & CT_TF_MODAL)     fo = ct_apnd(flags, fo, (int)sizeof(flags), "%smodal", fo ? "|" : "");
    int o = ct_apnd(out, 0, ocap,
                    ", timing: %s, budget_ms: %d, tflags: [%s], success: %s",
                    ct_timename(t.timing), contract_budget_ms(it), flags,
                    ct_succname(t.success));
    if (t.success_ref)
        o = ct_apnd(out, o, ocap, ", success_ref: %s", t.success_ref);
    o = ct_apnd(out, o, ocap, ", depth: %s, failures: [%s]",
                ct_depthname(t.depth), fails);
    if (t.promise) {
        o = ct_apnd(out, o, ocap, ", promise: {scope: %s, ttl: %ld, posts: [",
                    t.promise->scope_tmpl ? t.promise->scope_tmpl : "",
                    t.promise->ttl_secs);
        if (t.promise->posts) {
            int first = 1;
            for (const ct_post_t *ps = t.promise->posts; ps->kind != CT_POST_END; ps++) {
                const char *kn = ps->kind == CT_POST_PATH  ? "path"
                               : ps->kind == CT_POST_MOVED ? "moved" : "item";
                o = ct_apnd(out, o, ocap, "%s%s:%s", first ? "" : "|",
                            kn, ps->a ? ps->a : "");
                first = 0;
                if (o >= ocap - 4) break;      // leave room for the closer
            }
        }
        o = ct_apnd(out, o, ocap, "]}");
    }
    (void)o;
}

// Is this action safe to run in THIS invocation? A caller on the spawn path
// waits on an UNBOUNDED sys_waitpid() (ctinvoke.c; the kernel ignores WNOHANG),
// so an action that has told us it may never return must be refused HERE, where
// there is still somebody to tell, rather than allowed to wedge the caller. On
// the live path the caller already has a bounded wait (contract_invoke_live's
// CTLIVE_WAIT_MS -> CT_LIVE_TIMEOUT), so the same action is allowed.
static int ct_timing_refusal(const ct_item_t *it, char *why, int wcap) {
    ct_terms_t t;
    if (!contract_terms_of(it, &t)) {
        snprintf(why, (size_t)wcap,
                 "action declares no timing term, so no caller can bound it; "
                 "declare ct_terms_t.timing (contract.h)");
        return 1;
    }
    if (contract_serving_live()) return 0;   // bounded by the live transport
    if (t.tflags & CT_TF_MODAL) {
        snprintf(why, (size_t)wcap,
                 "modal: it opens a dialog and waits for a person, and a spawned "
                 "contract process has no window and nobody to answer it. Deliver "
                 "it to a RUNNING instance (contract_invoke_live)");
        return 1;
    }
    if (t.timing == CT_TIME_UNBOUNDED) {
        snprintf(why, (size_t)wcap,
                 "timing: unbounded, and the spawn path waits with no timeout "
                 "(sys_waitpid ignores WNOHANG). Deliver it to a RUNNING instance "
                 "(contract_invoke_live), which does have a bounded wait");
        return 1;
    }
    return 0;
}

int contract_authorize(const ct_contract_t *c, const ct_item_t *it,
                       const char *args, char *reason, int rcap) {
    if (it->risk == CT_DENIED) {
        if (reason && rcap > 0)
            strlcpy(reason, "declared but never invocable through the contract API", (size_t)rcap);
        return CT_ERR_DENIED;
    }
    if (it->risk == CT_SAFE) return CT_OK;

    // GUARDED: the SAME gate the AI tool loop uses. No consent callback in a
    // CLI process, so aicap fails CLOSED unless a token was already granted
    // and persisted to /CONFIG/AICAPS.CFG. That is the intended behaviour: a
    // headless caller must not be able to mint its own authority.
    aicap_init();
    char amode[64]; amode[0] = 0;
    char why[192];  why[0] = 0;
    // The tool id is the contract-qualified name, so the audit trail names the
    // app and the exact item, not just "settings.set".
    char tool[CT_NAME_MAX + 40];
    snprintf(tool, sizeof(tool), "%s.%s", c->app, it->name);
    int rc = aicap_authorize(it->cap ? it->cap : tool, args,
                             why, (int)sizeof(why), amode, (int)sizeof(amode));
    if (rc != AICAP_ALLOW) {
        if (reason && rcap > 0) {
            if (why[0]) strlcpy(reason, why, (size_t)rcap);
            else        strlcpy(reason, aicap_code(rc), (size_t)rcap);
        }
        return CT_ERR_DENIED;
    }
    return CT_OK;
}

// Audit every invocation, allowed or refused. Reuses the AI audit sink rather
// than opening a second log: one place to read, one format to parse.
static void ct_audit(const ct_contract_t *c, const ct_item_t *it,
                     const char *args, int rc, const char *how) {
    char tool[CT_NAME_MAX + 40];
    snprintf(tool, sizeof(tool), "%s.%s", c->app, it ? it->name : "?");
    aicap_audit(tool, it && it->cap ? it->cap : "contract.invoke",
                args ? args : "", ct_codename(rc), how);
}

// ---------------------------------------------------------------------------
// describe: the YAML subset already used by /APPS/TRAYMENU.YML - a section
// header at column 0 and one flow map per line. Deliberately the SAME grammar
// traymenu.c already parses, rather than a fifth config dialect.
// ---------------------------------------------------------------------------
// BRIEF vs FULL, and why BOTH come out of THIS function rather than out of a
// post-processor in the caller (#<config-ref>).
//
// The AI tool layer needs an app's action list BEFORE it calls the app, and a
// full describe is too big to put in front of a model: MEASURED on golden 2588,
// the fifteen contracted apps' full describes total 235,096 bytes over 786
// rows: Maytera Studio alone is 62,994 (197 rows), Files 29,668, Terminal
// 30,028, Settings 18,441. aiclient.c's observation buffer (OBS_MAX) is 8,192
// bytes, so not one of the four largest fits in a SINGLE observation, let alone
// fifteen contracts in a prompt. Method: each app's own `--contract describe`
// run inside the OS and redirected to a file, then read off the image; not
// estimated from row counts.
//
// What costs those bytes is NOT what a caller needs in order to CHOOSE an
// action. It is `needs: [...]` (the kernel capability binding), `cap:` (the
// aicap id), `key:` (the config-file letter) and the whole `ct_terms_fields()`
// block (timing / success / depth / promise), all of which exist for the GATE
// and the ESCROW, not for the chooser. Strip those four and a row is name,
// type, access, risk, its valid values and its description: everything needed
// to pick a row and nothing else.
//
// THE REASON THE FILTER LIVES HERE. aiclient.c could have run a full describe
// and deleted fields from the text. That is a SECOND description of the
// contract, maintained apart from the first, which is precisely the defect
// contract.h's header records four previous instances of. One emitter, two
// widths: a field that is added to a row appears in `describe` and is absent
// from `brief` by one decision taken in one place, and the two cannot disagree
// about what a row IS.
//
// `only` is NULL for the whole surface, or one item name for a single row.
// A single FULL row is how a caller goes from "this action looks right" to
// "here is its timing, its postcondition and the capability it needs" without
// paying for the other sixty rows.
// width: CT_W_FULL / CT_W_BRIEF / CT_W_TERSE (see contract.h).
static int ct_describe(const ct_contract_t *c, int width, const char *only) {
    int brief = (width >= CT_W_BRIEF);
    int matched = 0;
    ct_line("contract: %s\n", c->app);
    ct_line("  title: %s\n", c->title ? c->title : c->app);
    ct_line("  source: %s\n", c->project ? "projected" : "static");
    if (c->note && !brief) ct_line("  note: %s\n", c->note);
    ct_line("items:\n");
    ct_item_t it;
    for (int i = 0; contract_at(c, i, &it); i++) {
        // PREFIX, not equality. An exact name still selects exactly its own row
        // (a name is a prefix of itself), and a namespace selects its family:
        // `brief fx.colors` on Maytera Studio is 27 rows of 197, 4,247 bytes
        // because the whole point is to fit an answer in a caller's buffer, and
        // the apps that do not fit are precisely the ones whose items are
        // namespaced. One comparison, two uses, no second selector syntax.
        if (only && strncmp(it.name, only, strlen(only)) != 0) continue;
        matched++;
        char extra[CT_OPTS_MAX + 64]; extra[0] = 0;
        if (it.type == CT_ENUM && it.options)
            snprintf(extra, sizeof(extra), ", options: %s", it.options);
        else if (it.type == CT_INT)
            snprintf(extra, sizeof(extra), ", min: %d, max: %d", it.lo, it.hi);
        char keybuf[24]; keybuf[0] = 0;
        if (it.cfgkey && !brief) snprintf(keybuf, sizeof(keybuf), ", key: %c", it.cfgkey);
        char capbuf[80]; capbuf[0] = 0;
        if (it.cap && !brief) snprintf(capbuf, sizeof(capbuf), ", cap: %s", it.cap);
        // tier 2: the KERNEL capabilities the action reaches, as a flow list.
        // Emitted even when empty ("needs: []") so a consumer can tell a
        // cooperative action (no system capability) from one whose `needs`
        // were simply never declared.
        char needsbuf[192]; int nb = 0;
        needsbuf[0] = 0;
        if (!brief) nb = (int)snprintf(needsbuf, sizeof(needsbuf), ", needs: [");
        if (it.needs && !brief) {
            for (const ct_cap_need_t *nd = it.needs; nd->cap; nd++) {
                nb += snprintf(needsbuf + nb, sizeof(needsbuf) - nb, "%s%d:%s",
                               nd == it.needs ? "" : "|",
                               (int)nd->cap, nd->scope_tmpl ? nd->scope_tmpl : "");
                if (nb > (int)sizeof(needsbuf) - 24) break;
            }
        }
        if (!brief) snprintf(needsbuf + nb, sizeof(needsbuf) - nb, "]");
        // #<config-ref>: the TERMS, on every row. A consumer must be able to read
        // timing/success/depth without knowing whether the row declared them or
        // the engine derived them, so they are always emitted.
        char termsbuf[512];
        termsbuf[0] = 0;
        if (!brief) ct_terms_fields(&it, termsbuf, (int)sizeof(termsbuf));
        // TERSE drops the description, which is just over HALF of a BRIEF row
        // and the only field left that is prose rather than shape. (MEASURED,
        // because "60 to 70 per cent" stood here first and was a guess: across
        // the five apps dumped at both widths on golden 2588 it is 77,525 bytes
        // of brief against 36,775 of terse, so the descriptions are 52.6 per
        // cent of a brief row.) It exists
        // because of one measured number: Settings' brief is 9,511 bytes against
        // an 8,192-byte observation, so the app the assistant is asked about most
        // missed a single-call answer by 1.3 KB. Terse puts it at about 5 KB.
        // This is the LAST field that can go; below terse there is only `list`.
        if (width >= CT_W_TERSE)
            ct_line("  - {name: %s, type: %s, access: %s, risk: %s%s}\n",
                    it.name, ct_typename(it.type), ct_access_str(it.access),
                    ct_riskname(it.risk), extra);
        else
            ct_line("  - {name: %s, type: %s, access: %s, risk: %s%s%s%s%s%s, desc: %s}\n",
                    it.name, ct_typename(it.type), ct_access_str(it.access),
                    ct_riskname(it.risk), extra, keybuf, capbuf, needsbuf, termsbuf,
                    it.desc ? it.desc : "");
    }
    if (only && !matched) {
        // An unknown name must FAIL, not return an empty but well-formed
        // "items:" block. An empty list reads as "this app has no such thing
        // and that is fine"; the caller asked about ONE row and there isn't
        // one, which is the same answer get/set/call already give.
        ct_line("err code=%s name=%s detail=no-item-with-this-name-or-prefix\n",
                ct_codename(CT_ERR_UNKNOWN), only);
        return CT_ERR_UNKNOWN;
    }
    return CT_OK;
}

// ---------------------------------------------------------------------------
// get / set / call
// ---------------------------------------------------------------------------
static int ct_do_get(const ct_contract_t *c, const char *name) {
    ct_item_t it;
    if (!contract_find(c, name, &it)) {
        ct_line("err code=%s name=%s\n", ct_codename(CT_ERR_UNKNOWN), name);
        return CT_ERR_UNKNOWN;
    }
    if (!(it.access & CT_READ)) {
        ct_line("err code=%s name=%s detail=not-readable\n",
                ct_codename(CT_ERR_ACCESS), name);
        return CT_ERR_ACCESS;
    }
    char reason[192]; reason[0] = 0;
    // A read of a GUARDED row is still gated: reading the network config or a
    // user list is a disclosure, not a free action.
    int arc = contract_authorize(c, &it, "", reason, (int)sizeof(reason));
    if (arc != CT_OK) {
        ct_audit(c, &it, "", arc, "get");
        ct_line("err code=%s name=%s detail=%s\n", ct_codename(arc), name, reason);
        return arc;
    }
    if (it.type == CT_STR) {
        char sv[256]; sv[0] = 0;
        if (it.getstrfn) it.getstrfn(sv, (int)sizeof(sv));
        ct_audit(c, &it, "", CT_OK, "get");
        ct_line("ok name=%s type=string value=%s\n", name, sv);
        return CT_OK;
    }
    if (it.type == CT_ACTION) {
        ct_line("err code=%s name=%s detail=action-not-readable\n",
                ct_codename(CT_ERR_ACCESS), name);
        return CT_ERR_ACCESS;
    }
    int v = contract_get(&it);
    ct_audit(c, &it, "", CT_OK, "get");
    ct_line("ok name=%s type=%s value=%d\n", name, ct_typename(it.type), v);
    return CT_OK;
}

static int ct_do_set(const ct_contract_t *c, const char *name, const char *valstr) {
    ct_item_t it;
    if (!contract_find(c, name, &it)) {
        ct_line("err code=%s name=%s\n", ct_codename(CT_ERR_UNKNOWN), name);
        return CT_ERR_UNKNOWN;
    }
    if (!(it.access & CT_WRITE) || it.type == CT_ACTION || it.type == CT_STR) {
        ct_line("err code=%s name=%s detail=not-writable\n",
                ct_codename(CT_ERR_ACCESS), name);
        return CT_ERR_ACCESS;
    }

    // Accept an enum LABEL as well as its index, so a caller (and the AI) can
    // say "Dark" rather than 1. The labels come from the same options string
    // the UI draws, so there is no second name table.
    int ok = 0;
    int v = ct_atoi(valstr, &ok);
    if (!ok && it.type == CT_ENUM && it.options) {
        int idx = it.lo, i = 0, start = 0, matched = -1;
        for (;; i++) {
            char ch = it.options[i];
            if (ch == '|' || ch == '\0') {
                int len = i - start;
                int vl = 0; while (valstr[vl]) vl++;
                if (len == vl && !strncmp(it.options + start, valstr, (size_t)len))
                    matched = idx;
                idx++; start = i + 1;
                if (ch == '\0') break;
            }
        }
        if (matched >= 0) { v = matched; ok = 1; }
    }
    if (!ok && it.type == CT_BOOL) {
        if (!strcmp(valstr, "true")  || !strcmp(valstr, "on"))  { v = 1; ok = 1; }
        if (!strcmp(valstr, "false") || !strcmp(valstr, "off")) { v = 0; ok = 1; }
    }
    if (!ok) {
        ct_line("err code=%s name=%s detail=unparseable-value\n",
                ct_codename(CT_ERR_RANGE), name);
        return CT_ERR_RANGE;
    }

    // Range check BEFORE authorization, so a refused call cannot be
    // distinguished from an out-of-range one by timing, and so the audit
    // records the honest reason.
    int lo = it.lo, hi = it.hi;
    if (it.type == CT_BOOL) { lo = 0; hi = 1; }
    if (v < lo || v > hi) {
        ct_line("err code=%s name=%s value=%d min=%d max=%d\n",
                ct_codename(CT_ERR_RANGE), name, v, lo, hi);
        return CT_ERR_RANGE;
    }

    char args[CT_NAME_MAX + 32];
    snprintf(args, sizeof(args), "%s=%d", name, v);
    char reason[192]; reason[0] = 0;
    int arc = contract_authorize(c, &it, args, reason, (int)sizeof(reason));
    if (arc != CT_OK) {
        ct_audit(c, &it, args, arc, "set");
        ct_line("err code=%s name=%s detail=%s\n", ct_codename(arc), name, reason);
        return arc;
    }

    int prev = contract_get(&it);
    if (contract_put(&it, v) != 0) {
        ct_audit(c, &it, args, CT_ERR_FAILED, "set");
        ct_line("err code=%s name=%s detail=setter-refused\n",
                ct_codename(CT_ERR_FAILED), name);
        return CT_ERR_FAILED;
    }
    // The ROW's own writer first, then the app-wide one. A row that names a
    // commitfn is saying "commit() does not persist me"; running it before
    // commit() means an app whose commit() recomputes a change-signature from
    // its own file (Settings does exactly that) sees the new value already on
    // disk rather than one launch behind it.
    if (it.commitfn) it.commitfn();
    if (c->commit) c->commit();

    // Read the value BACK through the same accessor rather than echoing what
    // was asked for. A setter that clamps, rejects or ignores is then visible
    // in the response instead of being hidden behind our own return code -
    // which is the class of defect this whole ticket exists to make
    // detectable.
    int now = contract_get(&it);
    ct_audit(c, &it, args, CT_OK, "set");
    ct_line("ok name=%s prev=%d value=%d%s\n", name, prev, now,
            now == v ? "" : " note=clamped-or-ignored-by-app");
    return CT_OK;
}

static int ct_do_call(const ct_contract_t *c, const char *name,
                      int argc, char **argv) {
    ct_item_t it;
    if (!contract_find(c, name, &it)) {
        ct_line("err code=%s name=%s\n", ct_codename(CT_ERR_UNKNOWN), name);
        return CT_ERR_UNKNOWN;
    }
    if (it.type != CT_ACTION || !it.actfn) {
        ct_line("err code=%s name=%s detail=not-an-action\n",
                ct_codename(CT_ERR_ACCESS), name);
        return CT_ERR_ACCESS;
    }
    char args[256]; args[0] = 0;
    for (int i = 0; i < argc; i++) {
        size_t l = 0; while (args[l]) l++;
        snprintf(args + l, sizeof(args) - l, i ? " %s" : "%s", argv[i]);
    }
    // #<config-ref> TIMING, BEFORE authorization and before any work: refuse an
    // action that has declared it may not return, instead of letting the caller
    // block on it. This is the half of the #469m hang that contract_declared()
    // could not close: that guard rejects an app the index does not name, and
    // this one rejects an action the app itself says is unbounded or modal.
    char why[192]; why[0] = 0;
    if (ct_timing_refusal(&it, why, (int)sizeof(why))) {
        ct_audit(c, &it, args, CT_ERR_DENIED, "call-timing-refused");
        ct_line("err code=%s name=%s detail=%s\n", ct_codename(CT_ERR_DENIED), name, why);
        return CT_ERR_DENIED;
    }

    char reason[192]; reason[0] = 0;
    int arc = contract_authorize(c, &it, args, reason, (int)sizeof(reason));
    if (arc != CT_OK) {
        ct_audit(c, &it, args, arc, "call");
        ct_line("err code=%s name=%s detail=%s\n", ct_codename(arc), name, reason);
        return arc;
    }

    // #<config-ref> THE PROMISE. A row that declares filesystem postconditions
    // runs INSIDE an escrow contract: the kernel scopes the writes and refuses
    // every delete, and the escrow's own postcondition oracle then says whether
    // what was promised actually holds. FAIL CLOSED when no backend is
    // registered: running the action anyway would leave the declaration
    // unverified, and an unverified promise is a claim a caller will believe.
    ct_terms_t tm; int have_tm = contract_terms_of(&it, &tm);
    int promised = (have_tm && tm.promise != 0);
    if (promised) {
        if (!g_prom_open || !g_prom_settle) {
            ct_audit(c, &it, args, CT_ERR_DENIED, "call-promise-no-backend");
            ct_line("err code=%s name=%s detail=%s\n", ct_codename(CT_ERR_DENIED), name,
                    "this action declares a promise but the app registered no promise "
                    "backend; call contract_use_escrow_promises() (libc/ctpromise.h) "
                    "before contract_cli(), or the postconditions go unverified");
            return CT_ERR_DENIED;
        }
        char pwhy[192]; pwhy[0] = 0;
        if (g_prom_open(c, &it, argc, argv, pwhy, (int)sizeof(pwhy)) != 0) {
            ct_audit(c, &it, args, CT_ERR_DENIED, "call-promise-open-failed");
            ct_line("err code=%s name=%s detail=promise-not-opened: %s\n",
                    ct_codename(CT_ERR_DENIED), name, pwhy[0] ? pwhy : "escrow refused");
            return CT_ERR_DENIED;
        }
    }

    char out[384]; out[0] = 0;
    int rc = it.actfn(&it, argc, argv, out, (int)sizeof(out));

    // Settle the promise BEFORE commit(): commit() persists preferences under
    // /CONFIG or the user's home, which is outside the escrow scope, and the
    // kernel would (correctly) refuse that write while the contract is still
    // marked. Order matters and this is the one place it is decided.
    char how[192]; how[0] = 0; int verdict = 0;
    if (promised) verdict = g_prom_settle(c, &it, how, (int)sizeof(how));

    if (c->commit) c->commit();
    ct_audit(c, &it, args, rc == 0 ? CT_OK : CT_ERR_FAILED, promised ? how : "call");
    if (rc != 0) {
        ct_line("err code=%s name=%s detail=%s%s%s\n",
                ct_codename(CT_ERR_FAILED), name, out[0] ? out : "action-failed",
                promised ? " promise=" : "", promised ? how : "");
        return CT_ERR_FAILED;
    }
    // A PROMISED action that returned 0 but whose postconditions do not hold is
    // NOT a success. This is the whole point of the success/promise terms: the
    // return code is the action's opinion of itself, and the promise is a fact
    // somebody else checked.
    if (promised && verdict != 0) {
        ct_line("err code=%s name=%s detail=promise-unmet result=%s promise=%s\n",
                ct_codename(CT_ERR_FAILED), name, out[0] ? out : "(none)", how);
        return CT_ERR_FAILED;
    }
    ct_line("ok name=%s%s%s%s%s\n", name, out[0] ? " result=" : "", out[0] ? out : "",
            promised ? " promise=" : "", promised ? how : "");
    return CT_OK;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int contract_is_invocation(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (argv[i] && !strcmp(argv[i], "--contract")) return 1;
    return 0;
}

// Run one already-parsed verb. args[0] is the item NAME for get/set/call; for
// probe/list/describe nargs may be 0. This is the ONE place that maps a verb to
// the engine, so contract_cli() (spawn path) and contract_live_poll() (live
// path) cannot answer the same verb differently. Emits through ct_line, which
// goes to fd 1 or to the active capture buffer.
int contract_run_verb(const ct_contract_t *c, const char *verb,
                      int nargs, char **args) {
    if (!verb) verb = "probe";

    // Every verb reads live state, so pull persisted state in first. Without
    // this a headless `set` would save a table full of defaults over the
    // user's other six preferences. (A live instance's load() is a no-op or
    // re-reads its own store; either way the live document is the truth.)
    if (c->load) c->load();

    if (!strcmp(verb, "probe")) {
        ct_line("contract: %s items=%d source=%s\n",
                c->app, contract_count(c), c->project ? "projected" : "static");
        return CT_OK;
    }
    if (!strcmp(verb, "list")) {
        ct_item_t it;
        for (int i = 0; contract_at(c, i, &it); i++) ct_line("%s\n", it.name);
        return CT_OK;
    }
    // describe          the WHOLE surface, every field (unchanged, and flowrun's
    //                    flow_app_risk() parses exactly this).
    // describe <prefix> the matching rows, every field. A full name matches one.
    // brief [<prefix>]  the same selection without the gate/escrow fields.
    // terse [<prefix>]  brief without the descriptions: shape only.
    // All three are ONE emitter at three widths, so a field added to a row
    // appears or is suppressed by one decision in one place.
    if (!strcmp(verb, "describe"))
        return ct_describe(c, CT_W_FULL,  nargs >= 1 ? args[0] : 0);
    if (!strcmp(verb, "brief"))
        return ct_describe(c, CT_W_BRIEF, nargs >= 1 ? args[0] : 0);
    if (!strcmp(verb, "terse"))
        return ct_describe(c, CT_W_TERSE, nargs >= 1 ? args[0] : 0);
    if (!strcmp(verb, "get")) {
        if (nargs < 1) { ct_line("err code=usage detail=get-needs-a-name\n"); return CT_ERR_USAGE; }
        return ct_do_get(c, args[0]);
    }
    if (!strcmp(verb, "set")) {
        if (nargs < 2) { ct_line("err code=usage detail=set-needs-a-name-and-value\n"); return CT_ERR_USAGE; }
        return ct_do_set(c, args[0], args[1]);
    }
    if (!strcmp(verb, "call")) {
        if (nargs < 1) { ct_line("err code=usage detail=call-needs-a-name\n"); return CT_ERR_USAGE; }
        return ct_do_call(c, args[0], nargs - 1, args + 1);
    }
    ct_line("err code=usage detail=unknown-verb verb=%s\n", verb);
    return CT_ERR_USAGE;
}

int contract_cli(int argc, char **argv, const ct_contract_t *c) {
    int at = -1;
    for (int i = 1; i < argc; i++)
        if (argv[i] && !strcmp(argv[i], "--contract")) { at = i; break; }
    if (at < 0) return CT_ERR_USAGE;

    const char *verb = (at + 1 < argc) ? argv[at + 1] : "probe";
    int nargs = argc - (at + 2);
    if (nargs < 0) nargs = 0;
    return contract_run_verb(c, verb, nargs, argv + (at + 2));
}
