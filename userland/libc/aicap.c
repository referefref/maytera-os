// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aicap.c - MayteraOS AI capability tokens + consent + audit (#293).
// See aicap.h for the model. Pure userland; enforced at the aiclient dispatch
// boundary. #305 will relocate this logic into a protected immutable core.
#include "unistd.h"   // #appwrite: getuid() for the lost-audit-record line
#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "fcntl.h"
#include "aicap.h"

#define AICAPS_CFG    "/CONFIG/AICAPS.CFG"      // persisted grants
#define AICONSENT_CFG "/CONFIG/AICONSENT.CFG"   // headless pre-seeded decisions
#define AIAUDIT_LOG   "/CONFIG/AIAUDIT.LOG"     // append-only audit trail
#define MAX_TOKENS    32
#define AUDIT_CAP     32768                     // keep at most this much log tail
// #469m: the metrics tail is its own budget, so a chatty measurement run can
// never evict an authorization record from AIAUDIT.LOG. ~112 bytes/record, so
// 96 KB is roughly the last 850 records: several full baseline runs.
#define AIMETRIC_CAP  98304

// ---------------------------------------------------------------------------
// Risk table: tool-id -> capability + risk. LOW-risk tools run freely (still
// audited); HIGH-risk tools require a token + (first use) user consent.
// ---------------------------------------------------------------------------
typedef struct { const char *id; const char *cap; int risk; } aicap_ent_t;

static const aicap_ent_t g_caps[] = {
    // ---- LOW: read-only or curated, no destructive effect ----
    { "files.list",          "fs.read",               AICAP_RISK_LOW  },
    { "files.read",          "fs.read",               AICAP_RISK_LOW  },
    { "files.open",          "app.files.navigate",    AICAP_RISK_LOW  },
    { "web.open",            "app.browser.open",      AICAP_RISK_LOW  },
    { "weather.current",     "system.info",           AICAP_RISK_LOW  },
    { "storage.free",        "system.info",           AICAP_RISK_LOW  },
    { "system.storage.info", "system.info",           AICAP_RISK_LOW  },
    // #293: read-only window enumerator so the AI can find a target 'win' handle
    // by title/app before an injection. Titles/ids only, no injection: LOW risk,
    // not consent-gated (still audited).
    { "input.windows",       "system.windows.read",   AICAP_RISK_LOW  },
    { "windows.list",        "system.windows.read",   AICAP_RISK_LOW  },
    { "settings.get",        "system.settings.read",  AICAP_RISK_LOW  },
    { "settings.get_theme",  "system.settings.read",  AICAP_RISK_LOW  },
    { "settings.open",       "process.launch",        AICAP_RISK_LOW  },
    { "terminal.open",       "process.launch",        AICAP_RISK_LOW  },
    { "calc.open",           "process.launch",        AICAP_RISK_LOW  },
    { "calc.eval",           "app.calc.compute",      AICAP_RISK_LOW  },
    // app.launch dispatches only from a fixed curated allowlist table (not an
    // arbitrary path), so it is treated as a low-risk launch.
    { "app.launch",          "process.launch",        AICAP_RISK_LOW  },
    // ---- HIGH: write / execute / destructive ----
    { "settings.set",        "system.settings.write", AICAP_RISK_HIGH },
    { "fs.write",            "fs.write",              AICAP_RISK_HIGH },
    { "files.write",         "fs.write",              AICAP_RISK_HIGH },
    { "fs.delete",           "fs.delete",             AICAP_RISK_HIGH },
    { "files.delete",        "fs.delete",             AICAP_RISK_HIGH },
    { "fs.mkdir",            "fs.mkdir",              AICAP_RISK_HIGH },
    { "files.mkdir",         "fs.mkdir",              AICAP_RISK_HIGH },
    { "fs.move",             "fs.move",               AICAP_RISK_HIGH },
    { "files.move",          "fs.move",               AICAP_RISK_HIGH },
    // #712: the high-level escrow-contract photo organiser. HIGH so the user
    // consents once to the whole device reorganisation; the contract then mints
    // its own scoped fs.write/move/mkdir grants (no delete) for the actual work.
    { "photos.organize",     "app.photos.organize",   AICAP_RISK_HIGH },
    { "terminal.execute",    "app.terminal.execute",  AICAP_RISK_HIGH },
    { "python.execute",      "app.python.execute",    AICAP_RISK_HIGH },
    { "taskmanager.kill",    "system.process.kill",   AICAP_RISK_HIGH },
    { "network.set",         "system.network.write",  AICAP_RISK_HIGH },
    // #238: the packet filter. HIGH, and deliberately NOT folded into
    // "system.network.write": granting an assistant permission to set an
    // IP address is not the same decision as granting it permission to
    // open or close ports, and a consent prompt that conflates the two
    // asks the user a question they cannot answer accurately.
    { "net.firewall",        "system.network.firewall", AICAP_RISK_HIGH },
    { "build.compile_app",   "app.build.compile",     AICAP_RISK_HIGH },
    { "build.deploy_app",    "app.build.deploy",      AICAP_RISK_HIGH },
    // #293 AI SYSTEM-CONTROL tools. screen.capture and the input.inject family
    // are HIGH-risk here (tool-layer consent + audit), and their executors also
    // acquire an OS capability grant whose consent prompt the TRUSTED COMPOSITOR
    // draws (Stage 1/3/4). Two gates on the same call by design, as settings.set.
    { "screen.capture",     "screen.capture",          AICAP_RISK_HIGH },
    { "input.type",         "input.inject",            AICAP_RISK_HIGH },
    { "input.key",          "input.inject",            AICAP_RISK_HIGH },
    { "input.click",        "input.inject",            AICAP_RISK_HIGH },
    { "input.mouse",        "input.inject",            AICAP_RISK_HIGH },
    // #239: Snapshot's capture.* contract rows. A screen capture is a
    // DISCLOSURE of whatever happens to be on screen - other people's windows,
    // a password field, a private document - so it sits with the writes rather
    // than with the reads. It is named here rather than left to the
    // unknown-tool default (which is also HIGH) so that a refusal says
    // "snapshot.capture / system.screen.capture" instead of failing closed for
    // a reason nobody can read; that is the same fault contract-lint check 2
    // exists to catch on the Settings side.
    { "snapshot.capture",    "system.screen.capture", AICAP_RISK_HIGH },
};
#define NCAPS ((int)(sizeof(g_caps)/sizeof(g_caps[0])))

// ---------------------------------------------------------------------------
// Token store
// ---------------------------------------------------------------------------
typedef struct {
    int  used;
    char id[24];
    char cap[40];                 // exact "fs.write" or wildcard "fs.*"
    int  risk;
    long expires_at;              // unix seconds; 0 = never
    int  max_uses;                // -1 = unlimited
    int  uses;                    // consumed so far
    char allowed_paths[160];      // comma-separated path prefixes; empty = any
    char denied_commands[160];    // comma-separated substrings; empty = none
    char audit_tag[64];
    int  persist;                 // mirror to AICAPS.CFG
} aicap_token_t;

static aicap_token_t g_tok[MAX_TOKENS];
static int  g_inited = 0;
static int  g_mint_seq = 1;
static aicap_consent_fn g_consent_cb = 0;

// #712: process-lifetime capability denylist (refcounted). A denied capability
// is refused by the gate regardless of any token or consent. The escrow layer
// uses this to make the no-delete promise an explicit, enforced policy.
static struct { char cap[40]; int refs; } g_denied[8];
static int g_denied_n = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static long aicap_now(void) {
    long t = sys_time();
    if (t > 1000000000L) return t;                 // looks like a real unix clock
    return 1700000000L + (long)(uptime_ms() / 1000); // synthetic monotonic fallback
}

static int ci_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}
static int ci_starts(const char *s, const char *pfx) {
    for (; *pfx; s++, pfx++) {
        char cs = *s, cp = *pfx;
        if (cs >= 'A' && cs <= 'Z') cs += 32;
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        if (cs != cp) return 0;
    }
    return 1;
}

// token cap matches a requested cap (exact, or wildcard "ns.*").
static int cap_match(const char *tokcap, const char *cap) {
    if (ci_eq(tokcap, cap)) return 1;
    int tl = (int)strlen(tokcap);
    if (tl >= 2 && tokcap[tl-1] == '*' && tokcap[tl-2] == '.') {
        char pfx[40];
        int n = tl - 1;                       // keep the trailing '.', drop the '*'
        if (n > (int)sizeof(pfx) - 1) n = sizeof(pfx) - 1;
        memcpy(pfx, tokcap, n); pfx[n] = 0;   // "fs.*" -> "fs."
        return ci_starts(cap, pfx);           // matches "fs.write", "fs.read", ...
    }
    return 0;
}

// #712: is this capability on the process denylist? Uses the SAME cap_match, so
// a denied "fs.delete" catches "fs.delete", and a denied "fs.*" would catch any
// fs.* capability (escrow denies only "fs.delete").
static int cap_is_denied(const char *cap) {
    for (int i = 0; i < g_denied_n; i++)
        if (cap_match(g_denied[i].cap, cap)) return 1;
    return 0;
}

int aicap_classify(const char *tool_id, char *cap_out, int capcap, int *risk_out) {
    for (int i = 0; i < NCAPS; i++) {
        if (!strcmp(g_caps[i].id, tool_id)) {
            if (cap_out) strlcpy(cap_out, g_caps[i].cap, capcap);
            if (risk_out) *risk_out = g_caps[i].risk;
            return 1;
        }
    }
    // Fail closed: unknown tools are HIGH risk and named after themselves.
    if (cap_out) strlcpy(cap_out, tool_id, capcap);
    if (risk_out) *risk_out = AICAP_RISK_HIGH;
    return 0;
}

const char *aicap_cap_of(const char *tool_id) {
    static char buf[40];
    aicap_classify(tool_id, buf, sizeof(buf), 0);
    return buf;
}

const char *aicap_code(int outcome) {
    switch (outcome) {
        case AICAP_DENIED:    return "CAPABILITY_DENIED";
        case AICAP_EXPIRED:   return "TOKEN_EXPIRED";
        case AICAP_EXHAUSTED: return "TOKEN_EXHAUSTED";
        default:              return "OK";
    }
}

// Extract the sensitive operand of a tool call into target[] (for constraint
// checks and the consent prompt). Mirrors json_get_str (flat-object string grab).
static int jget(const char *json, const char *key, char *out, int ocap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(json, pat);
    if (!k) { out[0] = 0; return 0; }
    const char *v = k + strlen(pat);
    while (*v && *v != ':') v++;
    if (*v != ':') { out[0] = 0; return 0; }
    v++;
    while (*v == ' ' || *v == '\t') v++;
    if (*v != '"') { out[0] = 0; return 0; }
    v++;
    int o = 0;
    while (*v && *v != '"' && o < ocap - 1) {
        if (*v == '\\' && v[1]) v++;
        out[o++] = *v++;
    }
    out[o] = 0;
    return 1;
}

static void target_of(const char *cap, const char *args, char *out, int ocap) {
    out[0] = 0;
    if (!strcmp(cap, "fs.move")) {
        // files.move: the CONSUMED/primary operand is the SOURCE. The
        // DESTINATION is scope-checked separately (both ends) by exec_fs_move
        // via aicap_path_in_scope(); see aiclient.c.
        jget(args, "src", out, ocap);
    } else if (ci_starts(cap, "fs.")) {
        jget(args, "path", out, ocap);
    } else if (!strcmp(cap, "system.settings.write")) {
        char c[48], k[48], val[160];
        jget(args, "category", c, sizeof(c));
        jget(args, "key", k, sizeof(k));
        jget(args, "value", val, sizeof(val));
        snprintf(out, ocap, "%s.%s=%s", c[0]?c:"?", k[0]?k:"?", val);
    } else if (!strcmp(cap, "app.terminal.execute")) {
        jget(args, "command", out, ocap);
    } else if (!strcmp(cap, "app.python.execute")) {
        if (!jget(args, "code", out, ocap)) jget(args, "path", out, ocap);
    } else if (!strcmp(cap, "system.process.kill")) {
        jget(args, "pid", out, ocap);
    } else if (!strcmp(cap, "app.photos.organize")) {
        // #712: name the device the escrow contract will reorganise.
        jget(args, "path", out, ocap);
    } else if (ci_starts(cap, "app.build.")) {
        // #294: the sensitive operand for the userland compiler is the target app.
        jget(args, "app_id", out, ocap);
    } else if (!strcmp(cap, "screen.capture")) {
        // The sensitive operand is the destination path (empty => executor default).
        jget(args, "path", out, ocap);
    } else if (!strcmp(cap, "input.inject")) {
        // Name the target window handle the assistant wants to drive.
        char w[24];
        if (jget(args, "win", w, sizeof(w)) || jget(args, "target", w, sizeof(w)))
            snprintf(out, ocap, "window %s", w);
    }
}

// Top-level scope of a path, e.g. "/HOME/X.TXT" -> "/HOME"; "/A" -> "/A".
static void path_scope(const char *path, char *out, int ocap) {
    int o = 0;
    if (path[0] != '/') { strlcpy(out, path, ocap); return; }
    out[o++] = '/';
    int i = 1;
    while (path[i] && path[i] != '/' && o < ocap - 1) out[o++] = path[i++];
    out[o] = 0;
}

// Does target satisfy a token's constraints for this capability?
static int constraints_ok(const aicap_token_t *t, const char *cap, const char *target) {
    // allowed_paths only restrict filesystem capabilities with a real path.
    if (t->allowed_paths[0] && ci_starts(cap, "fs.") && target[0] == '/') {
        const char *p = t->allowed_paths;
        int permitted = 0;
        char pre[160];
        while (*p) {
            int n = 0;
            while (*p && *p != ',' && n < (int)sizeof(pre)-1) pre[n++] = *p++;
            pre[n] = 0;
            while (*p == ',' || *p == ' ') p++;
            // trim trailing spaces
            while (n > 0 && pre[n-1] == ' ') pre[--n] = 0;
            if (n && ci_starts(target, pre)) { permitted = 1; break; }
        }
        if (!permitted) return 0;
    }
    // denied_commands: any substring present in the target rejects it.
    if (t->denied_commands[0] && target[0]) {
        const char *p = t->denied_commands;
        char sub[160];
        while (*p) {
            int n = 0;
            while (*p && *p != ',' && n < (int)sizeof(sub)-1) sub[n++] = *p++;
            sub[n] = 0;
            while (*p == ',') p++;
            while (n > 0 && sub[n-1] == ' ') sub[--n] = 0;
            if (n && strstr(target, sub)) return 0;
        }
    }
    return 1;
}

static int tok_expired(const aicap_token_t *t)   { return t->expires_at != 0 && aicap_now() >= t->expires_at; }
static int tok_exhausted(const aicap_token_t *t) { return t->max_uses >= 0 && t->uses >= t->max_uses; }

// ---------------------------------------------------------------------------
// Persistence: /CONFIG/AICAPS.CFG, one token per line, '|'-separated:
//   id|cap|risk|expires_at|max_uses|uses|allowed_paths|denied_commands|persist|audit_tag
// Lines beginning with '#' are comments. Only persist==1 tokens are written.
// ---------------------------------------------------------------------------
static void field(const char **pp, char *out, int ocap) {
    const char *p = *pp; int o = 0;
    while (*p && *p != '|' && *p != '\n' && *p != '\r' && o < ocap - 1) out[o++] = *p++;
    out[o] = 0;
    if (*p == '|') p++;
    *pp = p;
}
static long atol_s(const char *s) { long v = 0; int neg = 0; if (*s=='-'){neg=1;s++;} for (; *s>='0'&&*s<='9'; s++) v = v*10 + (*s-'0'); return neg?-v:v; }

static void load_tokens(void) {
    int fd = sys_open(AICAPS_CFG, O_RDONLY);
    if (fd < 0) return;
    static char buf[8192];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    const char *p = buf;
    while (*p) {
        // one line
        char line[640]; int ll = 0;
        while (*p && *p != '\n' && ll < (int)sizeof(line)-1) line[ll++] = *p++;
        line[ll] = 0;
        while (*p == '\n' || *p == '\r') p++;
        if (!line[0] || line[0] == '#') continue;
        // find a free slot
        int s = -1;
        for (int i = 0; i < MAX_TOKENS; i++) if (!g_tok[i].used) { s = i; break; }
        if (s < 0) break;
        aicap_token_t *t = &g_tok[s];
        memset(t, 0, sizeof(*t));
        const char *q = line; char tmp[160];
        field(&q, t->id, sizeof(t->id));
        field(&q, t->cap, sizeof(t->cap));
        field(&q, tmp, sizeof(tmp)); t->risk = (int)atol_s(tmp);
        field(&q, tmp, sizeof(tmp)); t->expires_at = atol_s(tmp);
        field(&q, tmp, sizeof(tmp)); t->max_uses = (int)atol_s(tmp);
        field(&q, tmp, sizeof(tmp)); t->uses = (int)atol_s(tmp);
        field(&q, t->allowed_paths, sizeof(t->allowed_paths));
        field(&q, t->denied_commands, sizeof(t->denied_commands));
        field(&q, tmp, sizeof(tmp)); t->persist = (int)atol_s(tmp);
        field(&q, t->audit_tag, sizeof(t->audit_tag));
        if (t->cap[0]) t->used = 1;
    }
}

static void save_tokens(void) {
    int fd = sys_open(AICAPS_CFG, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    const char *hdr = "# MayteraOS AI capability grants (#293)\n"
                      "# id|cap|risk|expires_at|max_uses|uses|allowed_paths|denied_commands|persist|audit_tag\n";
    sys_write(fd, hdr, strlen(hdr));
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (!g_tok[i].used || !g_tok[i].persist) continue;
        aicap_token_t *t = &g_tok[i];
        char line[700];
        int l = snprintf(line, sizeof(line), "%s|%s|%d|%ld|%d|%d|%s|%s|%d|%s\n",
                         t->id, t->cap, t->risk, t->expires_at, t->max_uses, t->uses,
                         t->allowed_paths, t->denied_commands, t->persist, t->audit_tag);
        if (l > 0) sys_write(fd, line, (unsigned long)l);
    }
    sys_close(fd);
}

// Persist current "uses" of a persisted token (after consuming a use).
static void persist_if_needed(const aicap_token_t *t) { if (t->persist) save_tokens(); }

// ---------------------------------------------------------------------------
// Token lookup + mint
// ---------------------------------------------------------------------------
static aicap_token_t *find_usable(const char *cap, const char *target) {
    for (int i = 0; i < MAX_TOKENS; i++) {
        aicap_token_t *t = &g_tok[i];
        if (!t->used || !cap_match(t->cap, cap)) continue;
        if (tok_expired(t) || tok_exhausted(t)) continue;
        if (!constraints_ok(t, cap, target)) continue;
        return t;
    }
    return 0;
}
// Any token for this cap (to diagnose why no usable one was found).
static aicap_token_t *find_any(const char *cap) {
    for (int i = 0; i < MAX_TOKENS; i++)
        if (g_tok[i].used && cap_match(g_tok[i].cap, cap)) return &g_tok[i];
    return 0;
}

static aicap_token_t *mint(const char *cap, int risk, int max_uses, long ttl,
                           const char *allowed_paths, int persist, const char *tag) {
    int s = -1;
    for (int i = 0; i < MAX_TOKENS; i++) if (!g_tok[i].used) { s = i; break; }
    if (s < 0) s = 0;   // overwrite the first slot if full (bounded)
    aicap_token_t *t = &g_tok[s];
    memset(t, 0, sizeof(*t));
    snprintf(t->id, sizeof(t->id), "cap_%ld%d", aicap_now() & 0xffff, g_mint_seq++);
    strlcpy(t->cap, cap, sizeof(t->cap));
    t->risk = risk;
    t->expires_at = ttl > 0 ? aicap_now() + ttl : 0;
    t->max_uses = max_uses;
    t->uses = 0;
    if (allowed_paths) strlcpy(t->allowed_paths, allowed_paths, sizeof(t->allowed_paths));
    t->persist = persist;
    if (tag) strlcpy(t->audit_tag, tag, sizeof(t->audit_tag));
    t->used = 1;
    return t;
}

// ---------------------------------------------------------------------------
// #712 escrow: public scoped mint / revoke / capability denylist.
// ---------------------------------------------------------------------------
const char *aicap_grant_scoped(const char *cap, const char *allowed_paths,
                               long ttl_secs, int max_uses, const char *tag) {
    if (!g_inited) aicap_init();
    if (!cap || !cap[0]) return 0;
    aicap_token_t *t = mint(cap, AICAP_RISK_HIGH, max_uses, ttl_secs,
                            allowed_paths, 0 /* not persisted */,
                            tag ? tag : "escrow-grant");
    return t ? t->id : 0;
}

int aicap_revoke_tag(const char *tag) {
    if (!tag || !tag[0]) return 0;
    int n = 0;
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (g_tok[i].used && ci_eq(g_tok[i].audit_tag, tag)) {
            g_tok[i].used = 0;
            n++;
        }
    }
    return n;
}

void aicap_deny_capability(const char *cap) {
    if (!cap || !cap[0]) return;
    for (int i = 0; i < g_denied_n; i++)
        if (ci_eq(g_denied[i].cap, cap)) { g_denied[i].refs++; return; }
    if (g_denied_n >= (int)(sizeof(g_denied) / sizeof(g_denied[0]))) return;
    strlcpy(g_denied[g_denied_n].cap, cap, sizeof(g_denied[0].cap));
    g_denied[g_denied_n].refs = 1;
    g_denied_n++;
}

void aicap_allow_capability(const char *cap) {
    if (!cap || !cap[0]) return;
    for (int i = 0; i < g_denied_n; i++) {
        if (ci_eq(g_denied[i].cap, cap)) {
            if (--g_denied[i].refs <= 0) {
                g_denied[i] = g_denied[g_denied_n - 1];   // compact
                g_denied_n--;
            }
            return;
        }
    }
}

int aicap_cap_denied(const char *cap) { return cap ? cap_is_denied(cap) : 0; }

// ---------------------------------------------------------------------------
// Public init / consent
// ---------------------------------------------------------------------------
void aicap_init(void) {
    if (g_inited) return;
    g_inited = 1;
    memset(g_tok, 0, sizeof(g_tok));
    load_tokens();
}

void aicap_reload(void) {
    g_inited = 1;
    memset(g_tok, 0, sizeof(g_tok));
    load_tokens();
}

void aicap_set_consent_cb(aicap_consent_fn fn) { g_consent_cb = fn; }

// ---------------------------------------------------------------------------
// Audit
// ---------------------------------------------------------------------------
static void ts_now(char *out, int ocap) {
    int h=0,m=0,s=0,d=0,mo=0,y=0;
    get_rtc_time(&h,&m,&s);
    get_rtc_date(&d,&mo,&y);
    snprintf(out, ocap, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, m, s);
}

// THE ONE APPEND. Read-modify-write with a bounded tail (portable across FAT
// and ext2, neither of which this code may assume supports O_APPEND). Factored
// out of aicap_audit() at #469m so the audit trail and the loop-instrumentation
// record share ONE implementation of the cap-and-rotate logic instead of a
// second copy that would drift. `logbuf`/`bufcap` are the caller's scratch, so
// each sink keeps its own retention without a second static buffer here.
static void log_append(const char *path, const char *line, int l,
                       char *logbuf, int bufcap) {
    if (l <= 0 || l >= bufcap) return;
    int have = 0;
    int rfd = sys_open(path, O_RDONLY);
    if (rfd >= 0) {
        long rn = sys_read(rfd, logbuf, bufcap - 1);
        sys_close(rfd);
        if (rn > 0) have = (int)rn;
    }
    // if we would overflow, keep only the tail
    if (have + l > bufcap - 1) {
        int drop = have + l - bufcap + 1;
        if (drop < have) { memmove(logbuf, logbuf + drop, have - drop); have -= drop; }
        else have = 0;
    }
    int wfd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (wfd < 0) {
        // #appwrite: AN AUDIT RECORD THAT VANISHES WITHOUT A TRACE IS NOT AN
        // AUDIT CONTROL.
        //
        // This is THE ONE APPEND for both the AI audit trail and the loop
        // instrumentation, and it discarded the write refusal. MEASURED on
        // golden 2480: AICAPS_CFG and this log are hardcoded under /CONFIG,
        // which is root:root 0711, so a uid-1000 session is refused -13 on
        // every append, on every machine, before and after the setup wizard.
        // Every AI action a normal user takes has therefore been unrecorded,
        // and nothing said so.
        //
        // This does not make the append succeed; the right fix for that is to
        // move the sink under the session user's own home (userconf_path()),
        // which is a behaviour change with a policy question attached and is
        // written up rather than taken here. It makes the LOSS visible, which
        // is the part that must never be silent. The line lands in
        // /BOOTLOG.TXT, which is written by Ring 0 and seeded root:root 0600
        // in perms_system_seed[], so as an interim sink it is strictly harder
        // for a Ring-3 actor to tamper with than the file that failed.
        //
        // Deliberately NOT bounded by a counter here, unlike the userconf
        // reporter: dropping an audit record is not routine noise, and a flood
        // of these lines is itself the finding.
        char m[224];
        snprintf(m, sizeof(m),
                 "[FSDENY-U] AUDIT RECORD LOST: append to %s refused (rc=%d, uid=%d); "
                 "the record follows and exists nowhere else: %.96s",
                 path ? path : "?", wfd, getuid(), line);
        (void)sys_bootlog(m);
        return;
    }
    if (have > 0) sys_write(wfd, logbuf, have);
    sys_write(wfd, line, l);
    sys_close(wfd);
}

void aicap_audit(const char *tool_id, const char *cap, const char *args,
                 const char *result, const char *how) {
    char ts[32]; ts_now(ts, sizeof(ts));
    // sanitize args (truncate + strip newlines/pipes)
    char a[200]; int o = 0;
    for (const char *p = args ? args : ""; *p && o < (int)sizeof(a)-1; p++) {
        char c = *p; if (c == '\n' || c == '\r' || c == '|') c = ' ';
        a[o++] = c;
    }
    a[o] = 0;
    char line[512];
    int l = snprintf(line, sizeof(line), "%s|%s|%s|%s|%s|%s\n",
                     ts, tool_id ? tool_id : "?", cap ? cap : "?",
                     a, result ? result : "?", how ? how : "?");
    static char buf[AUDIT_CAP];
    log_append(AIAUDIT_LOG, line, l, buf, (int)sizeof(buf));
}

// ---------------------------------------------------------------------------
// #469m loop instrumentation. See the contract and the redaction argument in
// aicap.h. One line per record, pipe-delimited to match the audit log's shape
// so the same splitting works on both.
// ---------------------------------------------------------------------------

// Enumerated-ish caller strings still reach a log file, so every one of them is
// clamped to a safe alphabet. This is belt and braces for `tool`, which is the
// only field whose value can originate from the MODEL rather than from our own
// string constants.
static void met_word(const char *src, char *dst, int cap) {
    int o = 0;
    for (const char *p = src ? src : ""; *p && o < cap - 1; p++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        dst[o++] = ok ? c : '_';
    }
    dst[o] = 0;
    if (!o) { dst[0] = '-'; dst[1] = 0; }
}

void aicap_metric(const aicap_metric_t *m) {
    if (!m) return;
    char ts[32]; ts_now(ts, sizeof(ts));
    char op[16], model[48], style[16], outcome[32], tool[40];
    met_word(m->op,      op,      sizeof(op));
    met_word(m->model,   model,   sizeof(model));
    met_word(m->style,   style,   sizeof(style));
    met_word(m->outcome, outcome, sizeof(outcome));
    met_word(m->tool,    tool,    sizeof(tool));
    char line[512];
    int l = snprintf(line, sizeof(line),
        // #469m aim2 adds ONE trailing column, us_state. The version tag is
        // bumped rather than left alone because a reader that splits on | and
        // indexes by position would silently mis-read a future insertion; the
        // tag is the contract that says how many columns to expect.
        "%s|aim2|%s|%d|%d|%d|%s|%s|%s|%s|"
        "%ld|%ld|%ld|%ld|%ld|%ld|"
        "%lu|%lu|%lu|%lu|%lu|%lu|%lu|%lu|%lu|%lu|%lu|%lu\n",
        ts, op, m->turn, m->step, m->attempt, model, style, outcome, tool,
        m->req_bytes, m->resp_bytes, m->img_bytes, m->b64_bytes,
        m->tok_in, m->tok_out,
        m->us_build, m->us_encode, m->us_poststart, m->us_ttfb, m->us_total,
        m->us_parse, m->us_dispatch, m->us_capture, m->us_decode, m->us_scale,
        m->us_io, m->us_state);
    static char buf[AIMETRIC_CAP];
    log_append(AIMETRIC_LOG, line, l, buf, (int)sizeof(buf));
}

// ---------------------------------------------------------------------------
// Headless pre-seeded consent: /CONFIG/AICONSENT.CFG, lines "tool_or_cap=decision"
// where decision in {deny, once, session, persist, allow(=session)}. A matching
// entry (by tool id, capability, or "*") is consumed (removed) once used so a
// scripted test resolves deterministically. Returns -1 if no match.
// ---------------------------------------------------------------------------
static int decision_word(const char *w) {
    if (ci_eq(w, "deny"))    return AICAP_CONSENT_DENY;
    if (ci_eq(w, "once"))    return AICAP_CONSENT_ONCE;
    if (ci_eq(w, "session") || ci_eq(w, "allow")) return AICAP_CONSENT_SESSION;
    if (ci_eq(w, "persist")) return AICAP_CONSENT_PERSIST;
    return -1;
}

int aicap_preseed_consent(const char *tool_id, const char *cap) {
    int fd = sys_open(AICONSENT_CFG, O_RDONLY);
    if (fd < 0) return -1;
    static char buf[2048];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    // parse lines; remember the matching line range to remove it after
    int decision = -1;
    char rebuilt[2048]; int ro = 0;
    const char *p = buf;
    int matched_once = 0;
    while (*p) {
        const char *ls = p;
        char line[256]; int ll = 0;
        while (*p && *p != '\n' && ll < (int)sizeof(line)-1) line[ll++] = *p++;
        line[ll] = 0;
        while (*p == '\n' || *p == '\r') p++;
        (void)ls;
        int keep = 1;
        if (line[0] && line[0] != '#') {
            // split key=value
            char key[160], val[64]; int ki = 0, vi = 0; int eq = 0;
            for (int i = 0; line[i]; i++) {
                if (!eq && line[i] == '=') { eq = 1; continue; }
                if (!eq) { if (ki < (int)sizeof(key)-1) key[ki++] = line[i]; }
                else     { if (vi < (int)sizeof(val)-1) val[vi++] = line[i]; }
            }
            key[ki] = 0; val[vi] = 0;
            // trim spaces
            while (ki>0 && key[ki-1]==' ') key[--ki]=0;
            int vs=0; while (val[vs]==' ') vs++;
            int d = decision_word(val + vs);
            if (!matched_once && d >= 0 &&
                (ci_eq(key, tool_id) || ci_eq(key, cap) || !strcmp(key, "*"))) {
                decision = d; matched_once = 1; keep = 0;   // consume this line
            }
        }
        if (keep) {
            int el = (int)strlen(line);
            if (ro + el + 1 < (int)sizeof(rebuilt)) {
                memcpy(rebuilt + ro, line, el); ro += el; rebuilt[ro++] = '\n';
            }
        }
    }
    if (decision >= 0) {
        // rewrite the file without the consumed line
        int wfd = sys_open(AICONSENT_CFG, O_WRONLY | O_CREAT | O_TRUNC);
        if (wfd >= 0) { if (ro > 0) sys_write(wfd, rebuilt, ro); sys_close(wfd); }
    }
    return decision;
}

// ---------------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------------
int aicap_authorize(const char *tool_id, const char *args,
                    char *reason_out, int reasoncap,
                    char *authmode_out, int authmodecap) {
    if (!g_inited) aicap_init();
    if (reason_out && reasoncap) reason_out[0] = 0;
    if (authmode_out && authmodecap) authmode_out[0] = 0;

    char cap[40]; int risk = AICAP_RISK_HIGH;
    aicap_classify(tool_id, cap, sizeof(cap), &risk);

    // #712: an explicit process-policy deny overrides any token, consent, or the
    // low-risk shortcut (e.g. a no-delete escrow forbids fs.delete outright).
    if (cap_is_denied(cap)) {
        if (reason_out) snprintf(reason_out, reasoncap,
            "capability %s is forbidden by an active policy (no-delete escrow)", cap);
        return AICAP_DENIED;
    }

    if (risk == AICAP_RISK_LOW) {
        if (authmode_out) strlcpy(authmode_out, "low-risk", authmodecap);
        return AICAP_ALLOW;
    }

    char target[256];
    target_of(cap, args, target, sizeof(target));

    // 1. A directly usable token?
    aicap_token_t *t = find_usable(cap, target);
    if (t) {
        t->uses++;
        persist_if_needed(t);
        if (authmode_out) snprintf(authmode_out, authmodecap, "token:%s", t->id);
        return AICAP_ALLOW;
    }

    // 2. A token exists for this cap but is not usable: report the precise reason.
    aicap_token_t *any = find_any(cap);
    if (any) {
        if (tok_expired(any)) {
            if (reason_out) snprintf(reason_out, reasoncap, "token %s expired", any->id);
            return AICAP_EXPIRED;
        }
        if (tok_exhausted(any)) {
            if (reason_out) snprintf(reason_out, reasoncap, "token %s exhausted (max_uses=%d)", any->id, any->max_uses);
            return AICAP_EXHAUSTED;
        }
        // present but constraints reject this target (e.g. path out of scope)
        if (reason_out) snprintf(reason_out, reasoncap,
            "'%s' is outside the granted scope (allowed_paths=%s)",
            target[0]?target:"(none)", any->allowed_paths[0]?any->allowed_paths:"(any)");
        return AICAP_DENIED;
    }

    // 3. No token at all: ask for consent. A registered UI callback owns the
    // decision (it shows the dialog and may itself honor a pre-seeded answer for
    // headless tests). With no callback (e.g. terminal/msh) fall back to the
    // file pre-seed, else fail closed.
    int dec;
    if (g_consent_cb) {
        dec = g_consent_cb(tool_id, cap, risk, args, target);
    } else {
        dec = aicap_preseed_consent(tool_id, cap);
        if (dec < 0) dec = AICAP_CONSENT_DENY;
    }
    if (dec == AICAP_CONSENT_DENY) {
        if (reason_out) snprintf(reason_out, reasoncap, "user denied consent for %s", cap);
        return AICAP_DENIED;
    }

    // Mint a token with sensible default constraints reflecting the consent.
    int   max_uses = (dec == AICAP_CONSENT_ONCE) ? 1 : -1;
    long  ttl      = (dec == AICAP_CONSENT_ONCE) ? 300 : 3600;   // 5 min / 1 h
    int   persist  = (dec == AICAP_CONSENT_PERSIST);
    // For filesystem caps, scope the grant to the top-level dir of the target so
    // the AI cannot wander outside the area the user just approved.
    char scope[160]; scope[0] = 0;
    if (ci_starts(cap, "fs.") && target[0] == '/') path_scope(target, scope, sizeof(scope));
    aicap_token_t *nt = mint(cap, risk, max_uses, ttl, scope, persist, "consent-granted");
    nt->uses++;                          // consume this first use
    if (persist) save_tokens();
    if (authmode_out) snprintf(authmode_out, authmodecap, "consent-granted:%s", nt->id);
    return AICAP_ALLOW;
}

// Non-consuming scope predicate (#711): is `path` inside the scope a usable
// token grants for the capability of `tool_id`? Reuses the SAME find_usable()
// token search and constraints_ok() allowed_paths check as the gate, so the
// two ends of a move are judged by identical rules. Consumes nothing, mints
// nothing, prompts nothing, audits nothing.
int aicap_path_in_scope(const char *tool_id, const char *path) {
    if (!g_inited) aicap_init();
    char cap[40]; int risk = AICAP_RISK_HIGH;
    aicap_classify(tool_id, cap, sizeof(cap), &risk);
    if (cap_is_denied(cap)) return 0;        // #712: forbidden by policy
    if (risk == AICAP_RISK_LOW) return 1;    // read-only tier: unrestricted
    if (!path || !path[0]) return 0;         // no path = nothing authorizable
    return find_usable(cap, path) != 0;
}
