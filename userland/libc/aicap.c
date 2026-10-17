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
#include "userconf.h"  // #aiaudit: userconf_path / userconf_ensure_parent_dir
#include "aicap.h"

// ---------------------------------------------------------------------------
// #aiaudit: WHERE THE AUDIT TRAIL LIVES, AND WHY IT MOVED OUT OF /CONFIG.
//
// It was "/CONFIG/AIAUDIT.LOG". /CONFIG is root:root 0711, so a Ring-3 process
// at a non-root uid is refused -13 on sys_open(O_WRONLY|O_CREAT) there, on
// every machine, before and after the setup wizard (MEASURED on golden 2480,
// tools/permprobe). Every AI action any normal user has ever taken therefore
// went unrecorded, and until dev 45cffc93 the control said nothing about it.
//
// THE FIX IS NOT TO LOOSEN /CONFIG. That directory also holds SHADOW, MFA.DB
// and AICAPS.CFG, the standing-consent store. A Ring-3 process able to create
// names in it is a strictly worse problem than a missing log, and the current
// asymmetry (Ring 0 writes /CONFIG/CAPALLOW.CFG through fat_write_file() while
// a Ring-3 sys_open of the same path is refused) is the boundary working.
//
// So the sink moves into the SESSION USER'S OWN HOME, through userconf_path(),
// the same mechanism #683 used for the notification spool. appwrite MEASURED
// that a provisioned user's home already works: CREATE /HOME/TESTER/PPROBE.TXT
// rc=259. The home directory was never the blocker.
//
// TWO CASES THAT DO NOT CHANGE, both on purpose:
//   * A ROOT session. userhome_root() is "/", so this resolves to exactly
//     "/CONFIG/AIAUDIT.LOG", byte for byte the pre-#aiaudit path.
//   * A machine with NO ACCOUNT YET. login_run() hands out a bootstrap session
//     with no passwd entry, userhome_root() falls back to "/", and the write is
//     refused again. That is CORRECT: there is no user yet to own the record.
//     It is also why every freshly built golden, and so every throwaway test
//     VM, shows total failure here. That is the rig, not the product.
//
// NO LEGACY READ FALLBACK, unlike userconf_open_read(). For a PREFERENCE,
// reading the old absolute path is what makes an upgrade keep your settings.
// For an AUDIT TRAIL it would pull ANOTHER PRINCIPAL's records into your file
// and re-present them as yours. A log that misattributes is worse than a log
// that starts empty, so each user's trail begins empty and stays theirs.
//
// The threat model this serves, and what the record is and is not evidence of,
// is stated in aicap.h. Short version: it answers "what did the AI do on my
// behalf", it is deletable by its owner by design, and its integrity ceiling
// is set by aicap living in Ring 3, not by which directory the file is in.
// ---------------------------------------------------------------------------
#define AIAUDIT_NAME   "AIAUDIT.LOG"
#define AIAUDIT_LEGACY "/CONFIG/AIAUDIT.LOG"


// ---------------------------------------------------------------------------
// #peruserpath: THE GRANT STORE MOVES INTO THE SESSION USER'S HOME TOO, for
// the same reason and through the same mechanism as AIAUDIT.LOG above.
//
// It was "/CONFIG/AICAPS.CFG". MEASURED on golden 2515 (plan doc section 7m):
// a uid-1000 Settings session pressing a CT_GUARDED button produced, exactly
// once, inside the action window:
//
//     [PERMS-DENY] ... uid=1000 gid=1000 want=-w- path=/CONFIG/AICAPS.CFG
//     [FSDENY] REFUSED op=write ... target=/CONFIG/AICAPS.CFG
//     [PERMS-DENY] ... uid=1000 gid=1000 want=-wx path=/CONFIG
//
// save_tokens() ignored the refusal, persist_if_needed() ignored save_tokens(),
// and the gate returned ALLOW. So a non-root session COULD NOT PERSIST GRANT
// CONSUMPTION AT ALL: `uses` incremented in memory, the file was never written,
// and the next process start re-read a table that had never heard of the use.
// A once-only grant was therefore once PER PROCESS for every non-root user,
// which is the consent half of #293 being silently inoperative for exactly the
// sessions it exists to constrain. It did not fail the action, which is why it
// survived: nothing surfaced.
//
// AND THE READ SIDE WAS WORSE THAN THE WRITE SIDE, which the survey that found
// this did not state. load_tokens() read the SAME root-owned absolute path.
// /CONFIG is 0711 (search for all) and a grant file created there carries the
// default 0644, so a uid-1000 process could READ root's persisted grants and
// find_usable() would hand them out as its own. A persisted grant is an
// authorization: root consenting once to fs.write on "/" would have become
// every user's standing fs.write grant. That is the opposite of a capability
// system. So this gets NO LEGACY READ FALLBACK, for the reason stated for
// AIAUDIT.LOG two paragraphs up and more sharply: a trail that misattributes
// is bad, a GRANT that misattributes is an escalation.
//
// NOT BY LOOSENING /CONFIG. That directory holds SHADOW, MFA.DB and PERMS.DB; a
// Ring-3 process able to create names in it is strictly worse than a missing
// grant file. The FILE moves; the system directory does not change.
//
// Root's home is "/", so a root session resolves to exactly
// "/CONFIG/AICAPS.CFG", byte for byte the pre-#peruserpath path. A machine with
// no account yet resolves there too and is still refused, which is correct:
// there is no principal yet to own a grant.
// ---------------------------------------------------------------------------
#define AICAPS_NAME      "AICAPS.CFG"            // persisted grants
#define AICAPS_LEGACY    "/CONFIG/AICAPS.CFG"
#define AICONSENT_NAME   "AICONSENT.CFG"         // headless pre-seeded decisions
#define AICONSENT_LEGACY "/CONFIG/AICONSENT.CFG"
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

// #aiaudit: resolve a sink's leaf name to an absolute per-user path. ALWAYS
// fills `out` when it returns 0. Falls back to the pre-#aiaudit absolute path
// only when the join will not fit, which is the one case where the old
// behaviour is strictly better than no path at all.
static int sink_path(const char *name, const char *legacy,
                     char *out, unsigned long cap) {
    if (!out || cap == 0) return -1;
    if (userconf_path(name, out, cap) == 0) return 0;
    unsigned long n = strlen(legacy);
    if (n + 1 > cap) return -1;
    memcpy(out, legacy, n + 1);
    return 0;
}

// #peruserpath: where THIS SESSION's grant store is. Exported (aicap.h) for the
// same reason aicap_audit_path() is: section 7m caught a probe still checking
// the literal a write had moved away from, and printing ABSENT about a file it
// had just written correctly. A reader asks the resolver the writer uses, or
// the two become two answers.
int aicap_grants_path(char *out, unsigned long cap) {
    return sink_path(AICAPS_NAME, AICAPS_LEGACY, out, cap);
}

static void load_tokens(void) {
    char path[256];
    if (aicap_grants_path(path, sizeof(path)) != 0) return;
    int fd = sys_open(path, O_RDONLY);
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

// #peruserpath: RETURNS A RESULT, and the result is used. This was `void` with
// three unchecked syscalls in it, which is how a refused grant store looked
// identical to a written one for every non-root session. Three further changes
// in the same breath, each of which the old shape made impossible:
//
//   * the destination is RESOLVED, not a literal (see the AICAPS_NAME comment);
//   * <home>/CONFIG is CREATED first. It is in the home skeleton since #745 but
//     not in any home that predates it, so on an upgraded install the open
//     would be refused for a missing parent, not for a permission;
//   * the bytes are FSYNCED before the close, through the shared
//     userconf_finish_write(). A grant store with no fsync cannot honestly be
//     said to persist across a reboot, and "the grant survives a restart" is
//     the whole claim a once-only grant makes.
static int save_tokens(void) {
    char path[256];
    if (aicap_grants_path(path, sizeof(path)) != 0) {
        uc_report_write_refusal("aicap grants (path too long)", AICAPS_LEGACY, -1);
        return -1;
    }
    // Build the whole file first: userconf_finish_write() wants one buffer, and
    // one buffer is also what lets a short write be DETECTED rather than leaving
    // a half-written grant table behind. 32 tokens * 700 bytes + header.
    static char body[24576];
    int o = snprintf(body, sizeof(body),
                     "# MayteraOS AI capability grants (#293)\n"
                     "# id|cap|risk|expires_at|max_uses|uses|allowed_paths|denied_commands|persist|audit_tag\n");
    if (o < 0) return -1;
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (!g_tok[i].used || !g_tok[i].persist) continue;
        aicap_token_t *t = &g_tok[i];
        char line[700];
        int l = snprintf(line, sizeof(line), "%s|%s|%d|%ld|%d|%d|%s|%s|%d|%s\n",
                         t->id, t->cap, t->risk, t->expires_at, t->max_uses, t->uses,
                         t->allowed_paths, t->denied_commands, t->persist, t->audit_tag);
        if (l <= 0 || o + l >= (int)sizeof(body)) continue;
        memcpy(body + o, line, (unsigned long)l); o += l;
    }
    userconf_ensure_parent_dir(path);
    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        uc_report_write_refusal("aicap grants", path, fd);
        return -1;
    }
    // ALWAYS closes fd, on every path.
    if (userconf_finish_write(fd, body, (unsigned long)o) != 0) {
        uc_report_write_refusal("aicap grants (write/fsync)", path, -1);
        return -1;
    }
    return 0;
}

// Persist current "uses" of a persisted token (after consuming a use).
//
// #peruserpath: a FAILURE HERE IS REPORTED. The use has already been consumed in
// memory, so if it does not reach disk the next process start hands the same use
// out again. That is the exact defect section 7m measured, and the only reason it
// was invisible is that this function discarded the answer.
static void persist_if_needed(const aicap_token_t *t) {
    if (!t->persist) return;
    if (save_tokens() != 0)
        printf("[aicap] GRANT-USE NOT PERSISTED: token %s (cap %s) was consumed "
               "in memory but the grant store write was refused; a restart will "
               "hand this use out again (uid=%d)\n",
               t->id, t->cap, (int)getuid());
}

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

// ZERO SHIPPING CALLERS, stated here rather than left to be discovered. The
// escrow no-delete policy that used to call this moved into the kernel
// (kernel/fs/escrow_guard.c), and nothing else has needed a process-lifetime
// capability denial since. The gate side IS live and IS tested, so this is a
// working mechanism waiting for a use, not a stub; it is recorded as
// uncalled so that nobody reads the gate's existence as evidence that
// something is being denied. The lying comment that used to sit on the
// declaration in aicap.h is corrected there.
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
int aicap_audit_path(char *out, unsigned long cap) {
    return sink_path(AIAUDIT_NAME, AIAUDIT_LEGACY, out, cap);
}
int aicap_metric_path(char *out, unsigned long cap) {
    return sink_path(AIMETRIC_NAME, AIMETRIC_LEGACY, out, cap);
}

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
    // #aiaudit: <home>/CONFIG is not part of the home skeleton and does not
    // exist on an upgraded install, so make it. Shared with
    // userconf_open_write() rather than copied: two implementations of "make
    // the preference directory" is how the writer and the reader come to
    // disagree about where it is.
    userconf_ensure_parent_dir(path);

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
        // #aiaudit MOVED THE SINK (see the block comment at the top of this
        // file), so the common case, a provisioned user, now succeeds. THIS
        // REPORTER STAYS, and stays UNBOUNDED, because the happy path working
        // is exactly when a silent failure would go unnoticed again. Dropping
        // an audit record is not routine noise; a flood of these lines is
        // itself the finding.
        //
        // The line lands in /BOOTLOG.TXT, which Ring 0 writes and which is
        // seeded root:root 0600 in perms_system_seed[], so the report does not
        // depend on the permission that just failed. HONEST LIMIT: /BOOTLOG.TXT
        // is REPLACED on the next boot's first persist (fs/bootlog.c treats
        // persisted == 0 as "first write, replace the whole file"), so it is a
        // within-boot channel, not a durable archive. It is the difference
        // between a diagnosable loss and an invisible one, and nothing more.
        //
        // The remaining way to land here is a machine with no account, where
        // the resolver falls back to "/CONFIG/...". Say so, so the next reader
        // triages "nobody has set this machine up" apart from "this user's
        // home is broken" without re-deriving it.
        //
        // -------------------------------------------------------------------
        // #aiaudit: AND THE REPORTER ITSELF WAS SILENT. THE FAILURE-PATH TEST
        // IS THE ONLY REASON WE KNOW.
        //
        // MEASURED on golden 2486, VM 2497 (no account, so the append really is
        // refused): the KERNEL logged "[FSDENY] REFUSED op=create ...
        // target=/CONFIG/AIAUDIT.LOG", and this reporter produced ZERO lines.
        //
        // Cause, then confirmed by source read: sys_bootlog_write()
        // (proc/syscall.c) bounces the message with
        // sc_bounce_str(umsg, buf, sizeof buf) where buf is char[200]. That is
        // strncpy_from_user() -> validate_user_string(), which returns
        // VALIDATE_STRING_UNTERMINATED if no NUL appears within max_len. The
        // syscall then returns -1 having logged NOTHING. **It does not
        // truncate: a message of 200 characters or more vanishes whole.**
        //
        // So the longer and more informative the diagnostic, the more certain
        // it was to disappear, which is precisely backwards. This is the
        // #appwrite finding one level down: the reporter for a silent failure
        // was itself silent, for a different reason.
        //
        // TWO lines now, each clamped far below the limit, so neither a long
        // home path nor a long record can push the HEADER out of existence.
        // The header is the part that must never be lost; the record is the
        // part that is nice to have. Sizing them separately is what makes that
        // ordering true rather than hopeful.
        // -------------------------------------------------------------------
        int nohome = (path && path[0] == '/' && path[1] == 'C');
        char m[180];
        snprintf(m, sizeof(m),
                 "[FSDENY-U] AUDIT RECORD LOST: append to %.60s refused "
                 "(rc=%d, uid=%d)%s",
                 path ? path : "?", wfd, getuid(),
                 nohome ? " [no account: home resolved to /]" : "");
        (void)sys_bootlog(m);
        snprintf(m, sizeof(m),
                 "[FSDENY-U] AUDIT RECORD LOST: the record follows and exists "
                 "nowhere else: %.80s", line);
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
    char path[256];
    if (sink_path(AIAUDIT_NAME, AIAUDIT_LEGACY, path, sizeof(path)) != 0) return;
    log_append(path, line, l, buf, (int)sizeof(buf));
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
    char path[256];
    if (sink_path(AIMETRIC_NAME, AIMETRIC_LEGACY, path, sizeof(path)) != 0) return;
    log_append(path, line, l, buf, (int)sizeof(buf));
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
    // #peruserpath: prefer this session's own copy, fall back to the legacy
    // admin-placed file, and REWRITE THE ONE WE READ. Writing the consumption
    // back to a different path than the one it came from is how a consumed line
    // reappears forever while a half-consumed copy accumulates elsewhere.
    //
    // HONEST LIMIT, stated because it is a real behaviour difference: when a
    // non-root session reads the legacy /CONFIG copy, the consuming rewrite is
    // REFUSED, so the decision is honoured but NOT consumed and a second
    // identical call gets the same answer. That is the pre-existing behaviour;
    // what changes is that the refusal is now REPORTED instead of discarded. The
    // determinism a rig wants comes from seeding the per-user path, which this
    // now prefers. Unlike the grant store, the legacy READ is kept: this file is
    // a deliberately admin-placed headless hook, not an authorization the
    // session earned, and a test rig that cannot seed it has no hook at all.
    char path[256];
    int fd = -1;
    if (userconf_path(AICONSENT_NAME, path, sizeof(path)) == 0)
        fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        strlcpy(path, AICONSENT_LEGACY, sizeof(path));
        fd = sys_open(path, O_RDONLY);
    }
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
        // rewrite the file without the consumed line, into the file we READ
        userconf_ensure_parent_dir(path);
        int wfd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
        if (wfd < 0) {
            uc_report_write_refusal("aicap preseed consume", path, wfd);
        } else if (userconf_finish_write(wfd, rebuilt, (unsigned long)ro) != 0) {
            uc_report_write_refusal("aicap preseed consume", path, -1);
        }
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
    if (persist && save_tokens() != 0)
        printf("[aicap] NEW PERSISTENT GRANT NOT PERSISTED: %s (cap %s) was "
               "granted for this process only; the grant store write was "
               "refused (uid=%d)\n", nt->id, nt->cap, (int)getuid());
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
