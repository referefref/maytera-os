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

// ---------------------------------------------------------------------------
// Output. Everything goes to fd 1 in one write() per line: the kernel's fd-1
// fallback (kernel/proc/fdlayer.c:1468) puts stdout on the serial console and
// mirrors it into syslog, so a harness with no GUI reads the result off the
// serial socket. One write per line keeps lines from interleaving with an
// app's own logging.
// ---------------------------------------------------------------------------
static void ct_emit(const char *s) {
    size_t n = 0; while (s[n]) n++;
    if (n) write(1, s, n);
}

static void ct_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ct_line(const char *fmt, ...) {
    char b[512];
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
// Authorization. ONE site, so #305's move of this into the immutable core is
// mechanical, exactly as aicap.h promises for the AI tool loop.
// ---------------------------------------------------------------------------
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
static void ct_describe(const ct_contract_t *c) {
    ct_line("contract: %s\n", c->app);
    ct_line("  title: %s\n", c->title ? c->title : c->app);
    ct_line("  source: %s\n", c->project ? "projected" : "static");
    if (c->note) ct_line("  note: %s\n", c->note);
    ct_line("items:\n");
    ct_item_t it;
    for (int i = 0; contract_at(c, i, &it); i++) {
        char extra[CT_OPTS_MAX + 64]; extra[0] = 0;
        if (it.type == CT_ENUM && it.options)
            snprintf(extra, sizeof(extra), ", options: %s", it.options);
        else if (it.type == CT_INT)
            snprintf(extra, sizeof(extra), ", min: %d, max: %d", it.lo, it.hi);
        char keybuf[24]; keybuf[0] = 0;
        if (it.cfgkey) snprintf(keybuf, sizeof(keybuf), ", key: %c", it.cfgkey);
        char capbuf[80]; capbuf[0] = 0;
        if (it.cap) snprintf(capbuf, sizeof(capbuf), ", cap: %s", it.cap);
        ct_line("  - {name: %s, type: %s, access: %s, risk: %s%s%s%s, desc: %s}\n",
                it.name, ct_typename(it.type), ct_access_str(it.access),
                ct_riskname(it.risk), extra, keybuf, capbuf,
                it.desc ? it.desc : "");
    }
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
    char reason[192]; reason[0] = 0;
    int arc = contract_authorize(c, &it, args, reason, (int)sizeof(reason));
    if (arc != CT_OK) {
        ct_audit(c, &it, args, arc, "call");
        ct_line("err code=%s name=%s detail=%s\n", ct_codename(arc), name, reason);
        return arc;
    }
    char out[384]; out[0] = 0;
    int rc = it.actfn(&it, argc, argv, out, (int)sizeof(out));
    if (c->commit) c->commit();
    ct_audit(c, &it, args, rc == 0 ? CT_OK : CT_ERR_FAILED, "call");
    if (rc != 0) {
        ct_line("err code=%s name=%s detail=%s\n",
                ct_codename(CT_ERR_FAILED), name, out[0] ? out : "action-failed");
        return CT_ERR_FAILED;
    }
    ct_line("ok name=%s%s%s\n", name, out[0] ? " result=" : "", out[0] ? out : "");
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

int contract_cli(int argc, char **argv, const ct_contract_t *c) {
    int at = -1;
    for (int i = 1; i < argc; i++)
        if (argv[i] && !strcmp(argv[i], "--contract")) { at = i; break; }
    if (at < 0) return CT_ERR_USAGE;

    const char *verb = (at + 1 < argc) ? argv[at + 1] : "probe";

    // Every verb reads live state, so pull persisted state in first. Without
    // this a headless `set` would save a table full of defaults over the
    // user's other six preferences.
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
    if (!strcmp(verb, "describe")) { ct_describe(c); return CT_OK; }
    if (!strcmp(verb, "get")) {
        if (at + 2 >= argc) { ct_line("err code=usage detail=get-needs-a-name\n"); return CT_ERR_USAGE; }
        return ct_do_get(c, argv[at + 2]);
    }
    if (!strcmp(verb, "set")) {
        if (at + 3 >= argc) { ct_line("err code=usage detail=set-needs-a-name-and-value\n"); return CT_ERR_USAGE; }
        return ct_do_set(c, argv[at + 2], argv[at + 3]);
    }
    if (!strcmp(verb, "call")) {
        if (at + 2 >= argc) { ct_line("err code=usage detail=call-needs-a-name\n"); return CT_ERR_USAGE; }
        return ct_do_call(c, argv[at + 2], argc - (at + 3), argv + (at + 3));
    }
    ct_line("err code=usage detail=unknown-verb verb=%s\n", verb);
    return CT_ERR_USAGE;
}
