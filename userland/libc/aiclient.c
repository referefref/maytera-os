// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aiclient.c - Shared MayteraOS AI (Kimi) client + ReAct tool loop.
//
// Single source of truth for the AI integration (Kimi HTTPS POST + the
// ACTION/OBSERVATION ReAct loop + the OS tool executors + the interim
// permission gate). Compiled into libc.a and used by BOTH the aichat GUI widget
// and the terminal/msh "?" prefix so the tools never drift between them.
//
// Extracted verbatim from apps/aichat/main.c (#292) into a shared module (#224).
#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "fcntl.h"
#include "aiclient.h"
#include "aicap.h"      // #293 capability tokens + consent + audit
#include "aiguard.h"    // #745 prompt-injection screen (kernel-owned ruleset)
#include "userconf.h"   // #684: per-user protected AI settings
#include "contract.h"   // #235: THE settings surface, asked not re-described

// ===========================================================================
// #684: THE AI KEY IS PROVISIONED, NOT READ FROM /etc
// ===========================================================================
// User's decision, verbatim: "we can leave kimi.key in there, in a users
// deployment they'll insert their own key in the settings app, this kimi.key
// file should only be used to write that value to settings (in a way that's
// protected from other users and apps) not be called directly by the ai chat
// app".
//
// So /CONFIG/KIMI.KEY is now a one-time PROVISIONING SEED, read by exactly one
// root-privileged path (kernel/main.c at session start, Ring 0), which writes
// its value into the user's own protected settings at <home>/CONFIG/AISVC.CFG,
// owned by that user, mode 0600. It may be ABSENT ENTIRELY on a normal
// deployment, where the user types their key into Settings and that is the only
// way it is ever set. Nothing in userland opens /CONFIG/KIMI.KEY any more.
//
// NO LEGACY READ FALLBACK, AND THAT IS THE GENERAL RULE FOR SECRETS.
// Preferences fall back to their old system location so an upgrade keeps them
// (see libc/userconf.c). Credentials MUST NOT, because a fallback silently
// re-creates the very read we are denying: it would put /CONFIG/AISVC.CFG back
// in this app's open() path, and on a non-root session that is exactly the
// [PERMS-DENY] we set out to remove. Migration for a secret is the privileged
// provisioner's job, not the reader's. If you are adding a config file: does it
// hold a credential? Then no fallback. Otherwise fall back.
//
// WHAT THIS ACHIEVES, HONESTLY. The key is now protected from OTHER USERS: the
// per-user file is 0600 owned by its user, and since #674 perms_check() walks
// the path properly, so another uid cannot read it. It is NOT protected from
// other APPS RUNNING AS THE SAME USER. Any process with this user's euid can
// open the same file. Closing that requires the per-app capability broker
// (#465-#468, #656); this change does not do it and must not be described as
// if it did.
#define AISVC_NAME    "AISVC.CFG"   // per-user, via libc/userconf.c
#define API_URL       "https://api.moonshot.ai/v1/chat/completions"
#define API_MODEL     "kimi-k2.6"
#define MAX_MSGS      AICLIENT_MAX_MSGS
#define RESP_MAX      65536
#define BODY_MAX      65536
#define AITOOLS_INDEX "/AITOOLS/INDEX.yaml"
#define STARTURL_PATH "/STARTURL.TXT"
#define MAX_ACTIONS   4
#define TOOLLIST_MAX  4096
#define OBS_MAX       8192

// #294: AI-driven userland compiler. The OS POSTs the patched app source to the
// build service (an HTTPS service on the build CT; TLS forced by the kernel POST
// path), receives the compiled ELF, writes it to /APPS/<app_id>, and relaunches.
// The URL is overridable at /CONFIG/BUILDSVC.TXT so the host need not be baked in.
#define BUILD_SVC_DEFAULT "https://127.0.0.1:8899/compile"
#define BUILD_SVC_CFG     "/CONFIG/BUILDSVC.TXT"
#define BUILD_RESP_MAX    (160 * 1024)   // ELF up to the kernel's 128KB POST cap

// #327 chat-to-app: a compact reference of the REAL userland app API is loaded
// from /AITOOLS/APPGEN.md and injected into the Kimi system prompt so the model
// writes a COMPILABLE new app main.c against actual signatures (not hallucinated).
#define APPGEN_PATH   "/AITOOLS/APPGEN.md"
#define APPGEN_MAX    12288

typedef ai_msg_t msg_t;

static char g_toollist[TOOLLIST_MAX];
static int  g_have_tools = 0;
static char g_apikey[256];
static int  g_have_key = 0;

// #367: provider-agnostic AI config. Defaults reproduce the historical Kimi
// behavior EXACTLY, so with no /CONFIG/AISVC.CFG present the client is byte-for
// -byte unchanged (endpoint/model/style below = the old hardcoded constants).
// (was /CONFIG/AISVC.CFG; now per-user only, no fallback: see the #684 note above)
#define AI_STYLE_BEARER    0   // OpenAI-compatible: Authorization: Bearer, chat/completions
#define AI_STYLE_ANTHROPIC 1   // Anthropic Messages API: x-api-key, /v1/messages
static char g_endpoint[256] = API_URL;
static char g_model[96]     = API_MODEL;
static int  g_api_style     = AI_STYLE_BEARER;
static char *g_resp = 0;
static char *g_body = 0;
static char  g_buildsvc[256] = BUILD_SVC_DEFAULT;
static char  g_appgen[APPGEN_MAX];   // #327 app-generation RAG corpus, if present
static int   g_have_appgen = 0;

static msg_t g_msgs[MAX_MSGS];
static int   g_nmsgs = 0;

// ===========================================================================
// #745 PROMPT-INJECTION SCREENING, at add_msg().
//
// WHY HERE. add_msg() is the ONE function through which every message enters
// the conversation, whatever produced it: the user's typed prompt, an RSS
// article, a file read by files.read, a filename from files.list, a weather
// response off the network, a compiler log from the #294 build service. There
// is no second door. Screening per producer would have meant eleven call sites
// and a twelfth one appearing next week unscreened.
//
// WHAT THIS IS NOT. It is not the enforcement point and must never be
// described as one. It is Ring 3 screening its own input, so a hostile or
// simply forgetful app can skip it. The control that BINDS is in the kernel,
// at sys_http_post_start(), which refuses the POST whether or not this ran.
// This layer exists so the refusal has WORDS: which rule, which severity,
// which literal, and which SOURCE.
//
// ROLES SCREENED: 0 (the user's own turn) and 5 (a tool OBSERVATION, i.e. text
// the OS fetched rather than authored). Role 3 is our own system prompt, role
// 2 is a local error string, and roles 1/4 are the model's own output. The
// model's output is a real exfiltration vector but it is a different control
// with a different policy, and pretending this one covers it would overstate
// the coverage.
// ===========================================================================
static int  g_guard_block = 0;      // one-shot: consumed by the next send
static char g_guard_note[320];      // what the user is told, verbatim
static char g_guard_last[320];      // last note, for a host app to display

static void guard_note(int role, const aiguard_verdict_t *v, int blocked)
{
    const char *src = (role == 5) ? "A tool result" : "Your message";
    snprintf(g_guard_note, sizeof(g_guard_note),
             "%s %s the prompt-injection screen: rule %s (%s, %s), matched \"%s\".%s",
             src,
             blocked ? "was BLOCKED by" : "was flagged by",
             v->rule[0] ? v->rule : "(unnamed)",
             v->category[0] ? v->category : "-",
             aiguard_sev_name(v->severity),
             v->matched[0] ? v->matched : "-",
             blocked ? " Nothing was sent to the model."
                     : " It was sent, and the event was recorded.");
    strlcpy(g_guard_last, g_guard_note, sizeof(g_guard_last));
}

int aiclient_guard_blocked(void) { return g_guard_block; }
const char *aiclient_guard_note(void) { return g_guard_last; }
static void add_msg(int role, const char *text) {
    // #745: screen untrusted-origin text as it enters the conversation.
    if (text && (role == 0 || role == 5)) {
        aiguard_verdict_t v;
        int verdict = aiguard_check(text, &v);
        if (verdict == AIGUARD_BLOCK) {
            guard_note(role, &v, 1);
            g_guard_block = 1;
            // A tool OBSERVATION cannot simply be dropped: the ReAct loop has
            // already emitted the matching ACTION, and a turn with an ACTION
            // and no OBSERVATION is malformed. Substitute an explicit refusal
            // so the model sees that the tool ran and its output was withheld,
            // rather than seeing the payload. The turn ends either way, because
            // g_guard_block makes the next send return the note.
            if (role == 5)
                text = "OBSERVATION {\"error\":\"BLOCKED_BY_AIGUARD\","
                       "\"detail\":\"tool output withheld: prompt-injection "
                       "pattern\"}";
            else
                text = "[blocked by the prompt-injection screen]";
        } else if (verdict == AIGUARD_ANNOTATE) {
            // Allowed. Recorded, and the host app can show the note. Not
            // blocking on LOW/MEDIUM is deliberate: those rules fire on
            // ordinary words often enough that blocking would train the user
            // to distrust the guard, which is how a control gets switched off.
            guard_note(role, &v, 0);
        }
    }
    if (g_nmsgs >= MAX_MSGS) {
        // drop the oldest pair to make room (keep history bounded)
        free(g_msgs[0].text);
        memmove(&g_msgs[0], &g_msgs[1], sizeof(msg_t) * (g_nmsgs - 1));
        g_nmsgs--;
    }
    int len = (int)strlen(text);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return;
    memcpy(copy, text, len + 1);
    g_msgs[g_nmsgs].role = role;
    g_msgs[g_nmsgs].text = copy;
    g_nmsgs++;
}
// #684: load_key() DELETED. It opened /CONFIG/KIMI.KEY directly, which is the
// read the user's decision removes. The key now arrives only via the per-user
// AISVC.CFG that the kernel provisions (or that Settings writes).
// #367: load /CONFIG/AISVC.CFG (provider-agnostic endpoint/model/key/style).
// key=value lines: provider= endpoint= model= api_key= api_style=(bearer|anthropic).
// Absent or partial => keep the compiled defaults, so existing installs (which
// carry only /CONFIG/KIMI.KEY) keep working unchanged. Called AFTER load_key so
// an api_key here overrides KIMI.KEY.
static void load_aisvc(void) {
    int fd = userconf_open_read(AISVC_NAME, 0);   // #684: no legacy fallback (secret)
    if (fd < 0) return;
    static char buf[1024];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    int i = 0;
    while (buf[i]) {
        char line[512]; int ll = 0;
        while (buf[i] && buf[i] != '\n') { if (ll < (int)sizeof(line) - 1) line[ll++] = buf[i]; i++; }
        if (buf[i] == '\n') i++;
        line[ll] = 0;
        int e = ll - 1;
        while (e >= 0 && (line[e]=='\r'||line[e]==' '||line[e]=='\t')) line[e--] = 0;
        if (line[0] == '#' || line[0] == 0) continue;
        char *eq = 0;
        for (char *p = line; *p; p++) if (*p == '=') { eq = p; break; }
        if (!eq) continue;
        *eq = 0;
        const char *key = line; char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        if      (!strcmp(key, "endpoint"))  { if (val[0]) strlcpy(g_endpoint, val, sizeof(g_endpoint)); }
        else if (!strcmp(key, "model"))     { if (val[0]) strlcpy(g_model, val, sizeof(g_model)); }
        else if (!strcmp(key, "api_style")) { g_api_style = !strcmp(val, "anthropic") ? AI_STYLE_ANTHROPIC : AI_STYLE_BEARER; }
        else if (!strcmp(key, "api_key"))   { if (val[0]) { strlcpy(g_apikey, val, sizeof(g_apikey)); g_have_key = 1; } }
    }
}
// #294: load the build-service URL override, if present.
static void load_buildsvc(void) {
    int fd = sys_open(BUILD_SVC_CFG, O_RDONLY);
    if (fd < 0) return;
    char b[256];
    long n = sys_read(fd, b, sizeof(b) - 1);
    sys_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    int i = (int)n - 1;
    while (i >= 0 && (b[i] == '\n' || b[i] == '\r' || b[i] == ' ' || b[i] == '\t')) b[i--] = 0;
    if (b[0]) strlcpy(g_buildsvc, b, sizeof(g_buildsvc));
}
// #327: load the app-generation RAG corpus (real userland API + exemplars) so it
// can be injected into the Kimi system prompt for the build/new-app tools.
static void load_appgen(void) {
    g_have_appgen = 0;
    g_appgen[0] = 0;
    int fd = sys_open(APPGEN_PATH, O_RDONLY);
    if (fd < 0) fd = sys_open("/AITOOLS/APPGEN.MD", O_RDONLY);   // case-tolerant
    if (fd < 0) return;
    long n = sys_read(fd, g_appgen, APPGEN_MAX - 1);
    sys_close(fd);
    if (n <= 0) { g_appgen[0] = 0; return; }
    g_appgen[n] = 0;
    g_have_appgen = 1;
}
// Append src into dst (bounded) with JSON string escaping. Returns new length.
static int json_escape_append(char *dst, int dlen, int dcap, const char *src) {
    for (const char *p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        const char *esc = 0; char ebuf[8];
        switch (c) {
            case '\\': esc = "\\\\"; break;
            case '"':  esc = "\\\""; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:
                if (c < 0x20) { snprintf(ebuf, sizeof(ebuf), "\\u%04x", c); esc = ebuf; }
                break;
        }
        if (esc) {
            int el = (int)strlen(esc);
            if (dlen + el >= dcap) break;
            memcpy(dst + dlen, esc, el); dlen += el;
        } else {
            if (dlen + 1 >= dcap) break;
            dst[dlen++] = (char)c;
        }
    }
    dst[dlen] = 0;
    return dlen;
}

static int str_append(char *dst, int dlen, int dcap, const char *src) {
    int el = (int)strlen(src);
    if (dlen + el >= dcap) el = dcap - 1 - dlen;
    if (el < 0) el = 0;
    memcpy(dst + dlen, src, el);
    dlen += el;
    dst[dlen] = 0;
    return dlen;
}

// Build the chat-completions request body from the full conversation history.
static void build_body(void) {
    int n = 0;
    n = str_append(g_body, n, BODY_MAX, "{\"model\":\"");
    n = json_escape_append(g_body, n, BODY_MAX, g_model);
    n = str_append(g_body, n, BODY_MAX, "\",");
    // roles: 0=user 1=assistant 2=local error(skipped) 3=system
    //        4=internal assistant ACTION (sent as assistant, not rendered)
    //        5=internal tool OBSERVATION (sent as user, not rendered)
    if (g_api_style == AI_STYLE_ANTHROPIC) {
        // Anthropic Messages API: system prompt is a top-level "system" field,
        // max_tokens is required, and "messages" carries only user/assistant.
        n = str_append(g_body, n, BODY_MAX, "\"max_tokens\":4096,\"system\":\"");
        for (int i = 0; i < g_nmsgs; i++)
            if (g_msgs[i].role == 3) n = json_escape_append(g_body, n, BODY_MAX, g_msgs[i].text);
        n = str_append(g_body, n, BODY_MAX, "\",\"messages\":[");
        int first = 1;
        for (int i = 0; i < g_nmsgs; i++) {
            if (g_msgs[i].role == 2 || g_msgs[i].role == 3) continue;  // skip local + system
            if (!first) n = str_append(g_body, n, BODY_MAX, ",");
            first = 0;
            const char *open = (g_msgs[i].role == 0 || g_msgs[i].role == 5)
                ? "{\"role\":\"user\",\"content\":\""
                : "{\"role\":\"assistant\",\"content\":\"";
            n = str_append(g_body, n, BODY_MAX, open);
            n = json_escape_append(g_body, n, BODY_MAX, g_msgs[i].text);
            n = str_append(g_body, n, BODY_MAX, "\"}");
        }
        str_append(g_body, n, BODY_MAX, "]}");
        return;
    }
    // OpenAI-compatible (bearer): system is a normal message in the array.
    n = str_append(g_body, n, BODY_MAX, "\"messages\":[");
    int first = 1;
    for (int i = 0; i < g_nmsgs; i++) {
        if (g_msgs[i].role == 2) continue;   // skip local error/system notes
        if (!first) n = str_append(g_body, n, BODY_MAX, ",");
        first = 0;
        const char *open;
        if      (g_msgs[i].role == 0 || g_msgs[i].role == 5)
            open = "{\"role\":\"user\",\"content\":\"";
        else if (g_msgs[i].role == 3)
            open = "{\"role\":\"system\",\"content\":\"";
        else
            open = "{\"role\":\"assistant\",\"content\":\"";
        n = str_append(g_body, n, BODY_MAX, open);
        n = json_escape_append(g_body, n, BODY_MAX, g_msgs[i].text);
        n = str_append(g_body, n, BODY_MAX, "\"}");
    }
    str_append(g_body, n, BODY_MAX, "]}");
}

// Decode a \uXXXX escape into out. The TTF renderer only draws ASCII glyphs,
// so we fold the common typographic code points to readable ASCII and drop the
// rest (instead of emitting multi-byte UTF-8 that renders as garbage).
static int decode_unicode(unsigned int cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    const char *rep = 0;
    switch (cp) {
        case 0x2018: case 0x2019: case 0x201B: rep = "'";  break; // smart single quotes
        case 0x201C: case 0x201D: case 0x201F: rep = "\""; break; // smart double quotes
        case 0x2013: case 0x2014: case 0x2015: rep = "-";  break; // en/em dash
        case 0x2026: rep = "..."; break;                          // ellipsis
        case 0x2022: case 0x2023: case 0x25CF: case 0x25AA: rep = "*"; break; // bullets
        case 0x2192: rep = "->"; break;
        case 0x2190: rep = "<-"; break;
        case 0x00A0: rep = " ";  break;                           // nbsp
        case 0x00B7: rep = "*";  break;                           // middle dot
        default: break;
    }
    if (rep) { int l = (int)strlen(rep); memcpy(out, rep, l); return l; }
    // unknown non-ASCII: emit a placeholder to keep text readable
    out[0] = '?';
    return 1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Unescape a JSON string body (in place into out). src points at the first char
// AFTER the opening quote; copies until the closing unescaped quote.
// Returns out length. *endp set to the char after the closing quote.
static int json_unescape(const char *src, char *out, int ocap, const char **endp) {
    int o = 0;
    const char *p = src;
    while (*p && o < ocap - 4) {
        char c = *p;
        if (c == '"') { p++; break; }            // closing quote
        if (c == '\\') {
            p++;
            char e = *p;
            switch (e) {
                case 'n': out[o++] = '\n'; p++; break;
                case 't': out[o++] = '\t'; p++; break;
                case 'r': out[o++] = '\r'; p++; break;
                case 'b': out[o++] = '\b'; p++; break;
                case 'f': out[o++] = '\f'; p++; break;
                case '/': out[o++] = '/';  p++; break;
                case '"': out[o++] = '"';  p++; break;
                case '\\': out[o++] = '\\'; p++; break;
                case 'u': {
                    p++;
                    int h0=hexval(p[0]), h1=hexval(p[1]), h2=hexval(p[2]), h3=hexval(p[3]);
                    if (h0<0||h1<0||h2<0||h3<0) { out[o++] = '?'; break; }
                    unsigned int cp = (h0<<12)|(h1<<8)|(h2<<4)|h3;
                    p += 4;
                    o += decode_unicode(cp, out + o);
                    break;
                }
                default: out[o++] = e; if (e) p++; break;
            }
        } else if ((unsigned char)c >= 0x80) {
            // raw UTF-8 byte sequence: decode the code point and fold to ASCII
            unsigned char b0 = (unsigned char)c;
            unsigned int cp = 0; int extra = 0;
            if      ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; extra = 1; }
            else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; extra = 2; }
            else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; extra = 3; }
            else { p++; out[o++] = '?'; continue; }
            p++;
            for (int k = 0; k < extra && ((unsigned char)*p & 0xC0) == 0x80; k++) {
                cp = (cp << 6) | ((unsigned char)*p & 0x3F); p++;
            }
            o += decode_unicode(cp, out + o);
        } else {
            out[o++] = c; p++;
        }
    }
    out[o] = 0;
    if (endp) *endp = p;
    return o;
}

// Extract choices[0].message.content from the response JSON into out.
// Returns 1 on success.
static int extract_content(const char *json, char *out, int ocap) {
    // Find the "content" key that follows a "message" object. Be lenient: look
    // for "message" then the first "content" after it.
    const char *m = json;
    // search for "message"
    while (1) {
        const char *q = m;
        // naive substring search for "message"
        const char *found = 0;
        for (const char *s = q; *s; s++) {
            if (s[0]=='"'&&s[1]=='m'&&s[2]=='e'&&s[3]=='s'&&s[4]=='s'&&
                s[5]=='a'&&s[6]=='g'&&s[7]=='e'&&s[8]=='"') { found = s; break; }
        }
        if (!found) break;
        // from found, look for "content"
        const char *c = found;
        const char *ck = 0;
        for (const char *s = c; *s; s++) {
            if (s[0]=='"'&&s[1]=='c'&&s[2]=='o'&&s[3]=='n'&&s[4]=='t'&&
                s[5]=='e'&&s[6]=='n'&&s[7]=='t'&&s[8]=='"') { ck = s; break; }
        }
        if (!ck) break;
        // advance past "content" then to the opening quote of the value
        const char *v = ck + 9;
        while (*v && *v != ':') v++;
        if (*v != ':') break;
        v++;
        while (*v && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
        if (*v != '"') break;
        v++;
        json_unescape(v, out, ocap, 0);
        return 1;
    }
    return 0;
}

// Try to extract an error message: {"error":{"message":"..."}}
static int extract_error(const char *json, char *out, int ocap) {
    for (const char *s = json; *s; s++) {
        if (s[0]=='"'&&s[1]=='m'&&s[2]=='e'&&s[3]=='s'&&s[4]=='s'&&
            s[5]=='a'&&s[6]=='g'&&s[7]=='e'&&s[8]=='"') {
            const char *v = s + 9;
            while (*v && *v != ':') v++;
            if (*v != ':') continue;
            v++;
            while (*v && (*v==' '||*v=='\t'||*v=='\n'||*v=='\r')) v++;
            if (*v != '"') continue;
            v++;
            json_unescape(v, out, ocap, 0);
            return 1;
        }
    }
    return 0;
}

// #367: Anthropic Messages API response shape:
// {"content":[{"type":"text","text":"..."}],"role":"assistant",...}
// (no "message" object, and content is an array), so extract the first text.
static int extract_content_anthropic(const char *json, char *out, int ocap) {
    const char *c = 0;
    for (const char *s = json; *s; s++) {
        if (s[0]=='"'&&s[1]=='c'&&s[2]=='o'&&s[3]=='n'&&s[4]=='t'&&
            s[5]=='e'&&s[6]=='n'&&s[7]=='t'&&s[8]=='"') { c = s; break; }
    }
    if (!c) return 0;
    const char *tk = 0;
    for (const char *s = c; *s; s++) {
        if (s[0]=='"'&&s[1]=='t'&&s[2]=='e'&&s[3]=='x'&&s[4]=='t'&&s[5]=='"') { tk = s; break; }
    }
    if (!tk) return 0;
    const char *v = tk + 6;
    while (*v && *v != ':') v++;
    if (*v != ':') return 0;
    v++;
    while (*v && (*v==' '||*v=='\t'||*v=='\n'||*v=='\r')) v++;
    if (*v != '"') return 0;
    v++;
    json_unescape(v, out, ocap, 0);
    return 1;
}

// ===========================================================================
// AI tool-contract layer (#292)
// ===========================================================================

// Load /AITOOLS/INDEX.yaml and distill a compact "  <id>: <summary>" tool list
// for the system message. We parse only the "- id:" and "summary:" lines (a
// tiny, forgiving subset of YAML, enough for the index we generate).
static void load_tools(void) {
    g_have_tools = 0;
    g_toollist[0] = 0;
    int fd = sys_open(AITOOLS_INDEX, O_RDONLY);
    if (fd < 0) fd = sys_open("/AITOOLS/INDEX.YAML", O_RDONLY);   // case-tolerant
    if (fd < 0) fd = sys_open("/AITOOLS/INDEX.YML", O_RDONLY);    // 8.3 name a FAT ESP always exposes
    if (fd < 0) return;
    static char raw[16384];
    long n = sys_read(fd, raw, sizeof(raw) - 1);
    sys_close(fd);
    if (n <= 0) return;
    raw[n] = 0;

    int out = 0;
    char cur_id[64]; cur_id[0] = 0;
    int i = 0;
    while (raw[i]) {
        // read one line
        char line[512]; int ll = 0;
        while (raw[i] && raw[i] != '\n') { if (ll < (int)sizeof(line) - 1) line[ll++] = raw[i]; i++; }
        if (raw[i] == '\n') i++;
        line[ll] = 0;
        // skip leading spaces/dashes for keyword detection
        int p = 0; while (line[p] == ' ' || line[p] == '\t') p++;
        int dash = 0; if (line[p] == '-') { dash = 1; p++; while (line[p] == ' ') p++; }
        (void)dash;
        if (!strncmp(line + p, "id:", 3)) {
            int q = p + 3; while (line[q] == ' ') q++;
            int k = 0; while (line[q] && k < 63) cur_id[k++] = line[q++]; cur_id[k] = 0;
        } else if (!strncmp(line + p, "summary:", 8) && cur_id[0]) {
            int q = p + 8; while (line[q] == ' ') q++;
            // emit "  <id>: <summary>\n"
            if (out + (int)strlen(cur_id) + (int)strlen(line + q) + 6 < TOOLLIST_MAX) {
                g_toollist[out++] = ' '; g_toollist[out++] = ' ';
                for (const char *s = cur_id; *s; s++) g_toollist[out++] = *s;
                g_toollist[out++] = ':'; g_toollist[out++] = ' ';
                for (const char *s = line + q; *s; s++) g_toollist[out++] = *s;
                g_toollist[out++] = '\n';
            }
            cur_id[0] = 0;
        }
    }
    g_toollist[out] = 0;
    if (out > 0) g_have_tools = 1;

    // Optional diagnostic (#459 verification): if /CONFIG/AITOOLSDBG.ON exists,
    // dump the parsed tool list to /CONFIG/AITOOLSDBG.TXT so contract loading can
    // be confirmed on a VM. No-op in production (the flag file is not shipped).
    {
        int chk = sys_open("/CONFIG/AITOOLSDBG.ON", O_RDONLY);
        if (chk >= 0) {
            sys_close(chk);
            int dfd = sys_open("/CONFIG/AITOOLSDBG.TXT", O_WRONLY | O_CREAT | O_TRUNC);
            if (dfd >= 0) {
                const char *h = g_have_tools ? "HAVE_TOOLS\n" : "NONE_LOADED\n";
                sys_write(dfd, h, strlen(h));
                if (g_have_tools) sys_write(dfd, g_toollist, strlen(g_toollist));
                sys_close(dfd);
            }
        }
    }
}

// Build the system message that teaches Kimi the ACTION protocol + tool list.
// Stored as the first (role 3 = system) message in the history so it is sent on
// every request but never rendered in the transcript.
static const char *system_prompt(void) {
    static char sp[TOOLLIST_MAX + APPGEN_MAX + 2048];
    int sl = snprintf(sp, sizeof(sp),
        "You are Maytera AI, the built-in assistant for MayteraOS. You can call OS tools "
        "to read the filesystem, the weather, settings, disk usage, and to launch "
        "apps/games and open webpages.\n"
        "When you need data or want to perform an action, reply with EXACTLY ONE "
        "line and nothing else:\n"
        "ACTION <tool-id> <json-args>\n"
        "Example: ACTION files.list {\"path\":\"/APPS\"}\n"
        "The system will run the tool and reply with a line starting OBSERVATION "
        "containing the JSON result. Then continue: call another tool if needed, or "
        "give the user a final plain-language answer (no ACTION line). Use at most "
        "%d tools per question. Only use a tool when it helps; otherwise just "
        "answer.\n"
        "To launch ANY app or game (native apps, DOOM, and Win16 games like SkiFree, "
        "FreeCell, Tetris, Chips) use app.launch with a lowercase app_id, e.g. "
        "ACTION app.launch {\"app_id\":\"doom\"} or {\"app_id\":\"skifree\"}.\n"
        "For disk space use storage.free (no args). For reading settings use "
        "settings.get {\"key\":\"theme\"}. Setting keys are the Settings app's own "
        "contract names, e.g. clock.use_24hour, display.brightness, "
        "sound.master_volume, appearance.theme; the short leaf on its own "
        "(brightness) also works when it is unambiguous. If a key is unknown the "
        "OBSERVATION lists every key that exists - read that list and retry, do "
        "not guess a second time.\n"
        "Some tools are HIGH-RISK and permission-gated by the OS: settings.set "
        "(change a setting), fs.write {\"path\":\"/HOME/X.TXT\",\"content\":\"...\"} "
        "(write a file), and fs.delete. Just call them directly with the final "
        "arguments. The OPERATING SYSTEM will show the user a consent dialog and "
        "either grant or deny; you do NOT run your own yes/no handshake. If the "
        "OBSERVATION is an error with CAPABILITY_DENIED, TOKEN_EXPIRED, or "
        "TOKEN_EXHAUSTED, tell the user the action was not permitted and stop; do "
        "not retry the same action.\n"
        "You can REBUILD an existing userland app OR CREATE A BRAND-NEW APP from a "
        "chat description (#294/#327). When the user DESCRIBES an app they want "
        "(\"build me a tip calculator\", \"make a stopwatch\", \"a digital clock\"), "
        "you WRITE the whole app yourself:\n"
        "  1. Pick a short lowercase app_id in [a-z0-9_] (e.g. tipcalc, stopwatch, "
        "clockapp). For a NEW app use an id that is not an existing app.\n"
        "  2. Write the COMPLETE main.c using ONLY the real MayteraOS app API in the "
        "APP-GENERATION REFERENCE below (do not invent functions; integer math only, "
        "no %%f).\n"
        "  3. ACTION build.compile_app {\"app_id\":\"<id>\",\"source\":\"<the FULL "
        "main.c as one JSON string with \\n for newlines>\"}\n"
        "  4. If the compile OBSERVATION returns \"compile-failed\" with a \"log\", "
        "read the gcc errors and send ONE corrected FULL source (not a diff).\n"
        "  5. When it compiles, ACTION build.deploy_app {\"app_id\":\"<id>\"} to "
        "launch the app on screen.\n"
        "The build service creates the new app directory and a correct Makefile "
        "automatically; you only supply main.c. These build tools are HIGH-RISK and "
        "the OS will ask the user to consent first.\n"
        "Available tools (id: what it does):\n%s",
        MAX_ACTIONS, g_have_tools ? g_toollist : "  (none loaded)");
    // #327: append the app-generation RAG corpus so the model writes against the
    // REAL API. Only injected when present (aichat/terminal both benefit).
    if (g_have_appgen && sl > 0 && sl < (int)sizeof(sp) - 64) {
        snprintf(sp + sl, sizeof(sp) - sl,
                 "\n\n===== APP-GENERATION REFERENCE (real MayteraOS app API) =====\n%s",
                 g_appgen);
    }
    return sp;
}

// --- JSON arg extraction: pull a string value for "key" out of a flat JSON object.
static int json_get_str(const char *json, const char *key, char *out, int ocap) {
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

// Like json_get_str but DECODES JSON escapes (\n \t \r \" \\ \/ \uXXXX->byte).
// Needed for the build-tool "source" field: the AI emits the whole main.c as a
// single-line JSON string with escaped newlines, and the deterministic test
// driver does the same via json_escape_append; both must round-trip back to real
// C source. Returns 1 if the key was found.
static int json_get_str_full(const char *json, const char *key, char *out, int ocap) {
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
        if (*v == '\\' && v[1]) {
            char c = v[1];
            switch (c) {
                case 'n': out[o++] = '\n'; v += 2; break;
                case 't': out[o++] = '\t'; v += 2; break;
                case 'r': out[o++] = '\r'; v += 2; break;
                case 'b': out[o++] = '\b'; v += 2; break;
                case 'f': out[o++] = '\f'; v += 2; break;
                case '"': out[o++] = '"';  v += 2; break;
                case '\\':out[o++] = '\\'; v += 2; break;
                case '/': out[o++] = '/';  v += 2; break;
                case 'u': {
                    // \uXXXX -> single byte if <=0xFF, else '?'
                    int val = 0, ok = 1;
                    for (int d = 0; d < 4; d++) {
                        char h = v[2 + d];
                        int hv;
                        if (h >= '0' && h <= '9') hv = h - '0';
                        else if (h >= 'a' && h <= 'f') hv = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') hv = h - 'A' + 10;
                        else { ok = 0; break; }
                        val = val * 16 + hv;
                    }
                    if (ok) { out[o++] = (char)(val <= 0xFF ? val : '?'); v += 6; }
                    else    { out[o++] = *v++; }
                    break;
                }
                default: out[o++] = c; v += 2; break;
            }
        } else {
            out[o++] = *v++;
        }
    }
    out[o] = 0;
    return 1;
}

// Append a JSON-escaped string into a bounded buffer.
static int obs_escape(char *dst, int dlen, int dcap, const char *src) {
    return json_escape_append(dst, dlen, dcap, src);
}

// --- Executors ------------------------------------------------------------
// Each writes a JSON result (or {"error":"..."}) into obs[] and returns.

static void exec_files_list(const char *args, char *obs, int ocap) {
    char path[256];
    if (!json_get_str(args, "path", path, sizeof(path)) || !path[0]) {
        strlcpy(obs, "{\"error\":\"missing path\"}", ocap); return;
    }
    int fd = sys_open(path, 0);
    if (fd < 0) {
        snprintf(obs, ocap, "{\"error\":\"path-not-found\",\"path\":\"%s\"}", path);
        return;
    }
    int o = 0;
    o = str_append(obs, o, ocap, "{\"path\":\"");
    o = obs_escape(obs, o, ocap, path);
    o = str_append(obs, o, ocap, "\",\"entries\":[");
    dirent_t e;
    int count = 0, first = 1;
    while (count < 200) {
        int r = (int)syscall2(SYS_READDIR, fd, (long)&e);
        if (r != 0) break;   // 0 = entry filled; non-zero = EOD/err
        if (e.name[0] == 0) break;
        if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) { continue; }
        if (!first) o = str_append(obs, o, ocap, ",");
        first = 0;
        o = str_append(obs, o, ocap, "{\"name\":\"");
        o = obs_escape(obs, o, ocap, e.name);
        o = str_append(obs, o, ocap, "\",\"kind\":\"");
        o = str_append(obs, o, ocap, DIRENT_IS_DIR(e) ? "dir" : "file");
        o = str_append(obs, o, ocap, "\"}");
        count++;
        if (o > ocap - 256) break;
    }
    sys_close(fd);
    o = str_append(obs, o, ocap, "]}");
}

static void exec_files_read(const char *args, char *obs, int ocap) {
    char path[256];
    if (!json_get_str(args, "path", path, sizeof(path)) || !path[0]) {
        strlcpy(obs, "{\"error\":\"missing path\"}", ocap); return;
    }
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(obs, ocap, "{\"error\":\"path-not-found\",\"path\":\"%s\"}", path);
        return;
    }
    static char fbuf[4096];
    long n = sys_read(fd, fbuf, sizeof(fbuf) - 1);
    sys_close(fd);
    if (n < 0) { strlcpy(obs, "{\"error\":\"io-error\"}", ocap); return; }
    fbuf[n] = 0;
    int o = 0;
    o = str_append(obs, o, ocap, "{\"path\":\"");
    o = obs_escape(obs, o, ocap, path);
    o = str_append(obs, o, ocap, "\",\"content\":\"");
    o = obs_escape(obs, o, ocap, fbuf);
    o = str_append(obs, o, ocap, "\"}");
}

static void exec_files_open(const char *args, char *obs, int ocap) {
    (void)args;
    int pid = sys_spawn("/APPS/FILES");
    snprintf(obs, ocap, "{\"launched\":\"/APPS/FILES\",\"pid\":%d}", pid);
}

// --- HIGH-risk: write a file (#293). Gated by aicap before dispatch. ----------
// args: {"path":"/HOME/X.TXT","content":"...","mode":"overwrite|append"}
static void exec_fs_write(const char *args, char *obs, int ocap) {
    char path[256], mode[16];
    static char content[4096];
    if (!json_get_str(args, "path", path, sizeof(path)) || !path[0]) {
        strlcpy(obs, "{\"error\":\"missing path\"}", ocap); return;
    }
    if (!json_get_str(args, "content", content, sizeof(content))) content[0] = 0;
    if (!json_get_str(args, "mode", mode, sizeof(mode))) mode[0] = 0;
    int append = (!strcmp(mode, "append"));
    int flags = O_WRONLY | O_CREAT | (append ? 0 : O_TRUNC);
    int fd = sys_open(path, flags);
    if (fd < 0) {
        snprintf(obs, ocap, "{\"error\":\"io-error\",\"path\":\"%s\"}", path);
        return;
    }
    if (append) sys_seek(fd, 0, 2 /* SEEK_END */);
    long w = sys_write(fd, content, strlen(content));
    sys_close(fd);
    if (w < 0) { snprintf(obs, ocap, "{\"error\":\"io-error\",\"path\":\"%s\"}", path); return; }
    snprintf(obs, ocap, "{\"status\":\"success\",\"path\":\"%s\",\"bytes_written\":%ld,\"mode\":\"%s\"}",
             path, w, append ? "append" : "overwrite");
}

// --- HIGH-risk: delete a file (#293). Gated by aicap before dispatch. ---------
static void exec_fs_delete(const char *args, char *obs, int ocap) {
    char path[256];
    if (!json_get_str(args, "path", path, sizeof(path)) || !path[0]) {
        strlcpy(obs, "{\"error\":\"missing path\"}", ocap); return;
    }
    int r = sys_unlink(path);
    if (r < 0) { snprintf(obs, ocap, "{\"error\":\"delete-failed\",\"path\":\"%s\"}", path); return; }
    snprintf(obs, ocap, "{\"status\":\"success\",\"deleted\":\"%s\"}", path);
}

static void exec_web_open(const char *args, char *obs, int ocap) {
    char url[480];
    if (!json_get_str(args, "url", url, sizeof(url)) || !url[0]) {
        strlcpy(obs, "{\"error\":\"missing url\"}", ocap); return;
    }
    // Prefix a bare host with https:// (mirrors the browser's own behavior).
    char full[512];
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))
        snprintf(full, sizeof(full), "https://%s", url);
    else
        strlcpy(full, url, sizeof(full));
    int fd = sys_open(STARTURL_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { strlcpy(obs, "{\"error\":\"cannot-write-starturl\"}", ocap); return; }
    sys_write(fd, full, strlen(full));
    sys_write(fd, "\n", 1);
    sys_close(fd);
    int pid = sys_spawn("/APPS/BROWSER");
    snprintf(obs, ocap, "{\"opened\":\"%s\",\"pid\":%d}", full, pid);
}

static void exec_weather(const char *args, char *obs, int ocap) {
    (void)args;
    int fd = sys_open("/WEATHER.TXT", O_RDONLY);
    if (fd < 0) { strlcpy(obs, "{\"error\":\"not-available\"}", ocap); return; }
    char wb[256];
    long n = sys_read(fd, wb, sizeof(wb) - 1);
    sys_close(fd);
    if (n <= 0) { strlcpy(obs, "{\"error\":\"not-available\"}", ocap); return; }
    wb[n] = 0;
    // strip trailing newline
    while (n > 0 && (wb[n-1] == '\n' || wb[n-1] == '\r')) wb[--n] = 0;
    // split loc|cond|icon|now|min|max|hum|rain|mm
    char f[9][64]; int nf = 0;
    int fi = 0, fl = 0;
    for (int i = 0; wb[i] && nf < 9; i++) {
        if (wb[i] == '|') { f[nf][fl] = 0; nf++; fl = 0; fi = i + 1; (void)fi; }
        else if (fl < 63) f[nf][fl++] = wb[i];
    }
    if (nf < 9) { f[nf][fl] = 0; nf++; }
    for (int k = nf; k < 9; k++) f[k][0] = 0;
    snprintf(obs, ocap,
        "{\"location\":\"%s\",\"condition\":\"%s\",\"now_c\":\"%s\","
        "\"min_c\":\"%s\",\"max_c\":\"%s\",\"humidity_pct\":\"%s\","
        "\"rain_pct\":\"%s\",\"rain_mm\":\"%s\"}",
        f[0], f[1], f[3], f[4], f[5], f[6], f[7], f[8]);
}

static void exec_settings_theme(const char *args, char *obs, int ocap) {
    (void)args;
    int t = get_theme();
    const char *name = "Unknown";
    switch (t) {
        case 0: name = "Dark"; break;
        case 2: name = "Light"; break;
        case 4: name = "Classic"; break;
        case 5: name = "Ocean"; break;
        case 9: name = "Nord"; break;
        default: name = "Theme"; break;
    }
    snprintf(obs, ocap, "{\"theme_id\":%d,\"theme_name\":\"%s\"}", t, name);
}

static void exec_settings_open(const char *args, char *obs, int ocap) {
    (void)args;
    int pid = sys_spawn("/APPS/SETTINGS");
    snprintf(obs, ocap, "{\"launched\":\"/APPS/SETTINGS\",\"pid\":%d}", pid);
}

static void exec_terminal_open(const char *args, char *obs, int ocap) {
    (void)args;
    int pid = sys_spawn("/APPS/TERMINAL");
    snprintf(obs, ocap, "{\"launched\":\"/APPS/TERMINAL\",\"pid\":%d}", pid);
}

static void exec_calc_open(const char *args, char *obs, int ocap) {
    (void)args;
    int pid = sys_spawn("/APPS/CALC");
    snprintf(obs, ocap, "{\"launched\":\"/APPS/CALC\",\"pid\":%d}", pid);
}

// --- Generic app launcher (#292) -----------------------------------------
// Maps an app_id -> (on-disk path, launch kind) exactly as the compositor start
// menu does, then dispatches via the correct verb:
//   gui   -> sys_spawn(path)   (native ELF, incl. DOOM at /APPS/DOOM.ELF)
//   win16 -> win16_run(path)   (Win16 NE .EXE: SkiFree, FreeCell, Chips, Tetris)
//   dos   -> dos_run(path)     (MS-DOS .EXE: Tim, Commander Keen)
// This is the same launch mechanism the start menu uses, so games and Win16 apps
// start correctly (the old per-app executors blind-spawned /APPS/<name>, which is
// why DOOM and SkiFree failed).
enum { LK_GUI = 0, LK_WIN16 = 1, LK_DOS = 2 };
typedef struct { const char *id; const char *path; int kind; const char *name; } launch_ent_t;

// Table mirrors compositor/startmenu.c (LAUNCH_NATIVE/WIN16/DOS). Keep in sync.
static const launch_ent_t g_launch_tbl[] = {
    // --- native GUI apps + native games ---
    { "files",     "/APPS/FILES",       LK_GUI,   "Files" },
    { "terminal",  "/APPS/TERMINAL",    LK_GUI,   "Terminal" },
    { "calc",      "/APPS/CALC",        LK_GUI,   "Calculator" },
    { "calculator","/APPS/CALC",        LK_GUI,   "Calculator" },
    { "editor",    "/APPS/EDITOR",      LK_GUI,   "Text Editor" },
    { "settings",  "/APPS/SETTINGS",    LK_GUI,   "Settings" },
    { "browser",   "/APPS/BROWSER",     LK_GUI,   "Browser" },
    { "python",    "/APPS/PYTHON.ELF",  LK_GUI,   "Python" },
    { "doom",      "/GAMES/DOOM/DOOM.ELF", LK_GUI, "DOOM" },
    { "lemmings",  "/APPS/lemmings",    LK_GUI,   "Lemmings" },
    { "solitaire", "/APPS/SOLITAIRE",      LK_GUI,   "Solitaire" },
    { "solitr",    "/APPS/SOLITAIRE",      LK_GUI,   "Solitaire" },
    { "pong",      "/APPS/pong",        LK_GUI,   "Pong" },
    { "hello",     "/APPS/HELLO",       LK_GUI,   "Hello" },
    { "aidemo",    "/APPS/AIDEMO",      LK_GUI,   "AI Demo" },
    // --- Win16 NE apps (run by the Win16 interpreter) ---
    { "skifree",   "/WIN16/EP3/SKI.EXE",        LK_WIN16, "SkiFree" },
    { "tetris",    "/WIN16/MSEP/TETRIS.EXE",    LK_WIN16, "Tetris" },
    { "chips",     "/WIN16/MSEP/CHIPS.EXE",     LK_WIN16, "Chip's Challenge" },
    { "freecell",  "/WIN16/MSEP/FREECELL.EXE",  LK_WIN16, "FreeCell" },
    { "golf",      "/WIN16/MSEP/GOLF.EXE",      LK_WIN16, "Golf" },
    { "jezzball",  "/WIN16/MSEP/JEZZBALL.EXE",  LK_WIN16, "JezzBall" },
    { "tetravex",  "/WIN16/MSEP/TETRAVEX.EXE",  LK_WIN16, "TetraVex" },
    { "rodent",    "/WIN16/MSEP/RODENT.EXE",    LK_WIN16, "Rodent's Revenge" },
    { "tutstomb",  "/WIN16/MSEP/TUTSTOMB.EXE",  LK_WIN16, "Tut's Tomb" },
    // --- DOS games (run by the DOS emulator) ---
    { "tim",       "/DOS/TIM/TIM.EXE",          LK_DOS,   "The Incredible Machine" },
    { "keen5",     "/DOS/KEEN5/KEEN5E.EXE",     LK_DOS,   "Commander Keen 5" },
};
#define LAUNCH_TBL_N ((int)(sizeof(g_launch_tbl)/sizeof(g_launch_tbl[0])))

// case-insensitive id match (model may say "DOOM" / "SkiFree")
static int id_eq_ci(const char *a, const char *b) {
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

static void exec_app_launch(const char *args, char *obs, int ocap) {
    char id[64];
    if (!json_get_str(args, "app_id", id, sizeof(id)) || !id[0]) {
        // tolerate "app" or "name" as the key too
        if (!json_get_str(args, "app", id, sizeof(id)) || !id[0])
            if (!json_get_str(args, "name", id, sizeof(id)) || !id[0]) {
                strlcpy(obs, "{\"error\":\"missing app_id\"}", ocap); return;
            }
    }
    const launch_ent_t *e = 0;
    for (int i = 0; i < LAUNCH_TBL_N; i++)
        if (id_eq_ci(g_launch_tbl[i].id, id)) { e = &g_launch_tbl[i]; break; }
    if (!e) {
        snprintf(obs, ocap, "{\"error\":\"unknown-app\",\"app_id\":\"%s\"}", id);
        return;
    }
    int rc;
    const char *kind;
    switch (e->kind) {
        case LK_WIN16: kind = "win16"; rc = win16_run(e->path); break;
        case LK_DOS:   kind = "dos";   rc = dos_run(e->path);   break;
        default:       kind = "gui";   rc = sys_spawn(e->path); break;
    }
    if (rc < 0)
        snprintf(obs, ocap, "{\"error\":\"launch-failed\",\"app_id\":\"%s\",\"path\":\"%s\",\"rc\":%d}",
                 e->id, e->path, rc);
    else
        snprintf(obs, ocap, "{\"launched\":\"%s\",\"name\":\"%s\",\"kind\":\"%s\",\"path\":\"%s\",\"rc\":%d}",
                 e->id, e->name, kind, e->path, rc);
}

// --- Storage / disk-free (#292) -------------------------------------------
// sys_get_disk_total()/sys_get_disk_free() return whole MEGABYTES (derived in
// the kernel from FAT cluster_count/free_cluster_count * cluster_size). Report
// free/total/used MB + percent so the AI can answer "how much disk is free".
static void exec_storage_info(const char *args, char *obs, int ocap) {
    (void)args;
    long total = sys_get_disk_total();   // MB
    long free  = sys_get_disk_free();    // MB
    if (total <= 0) { strlcpy(obs, "{\"error\":\"disk-not-mounted\"}", ocap); return; }
    if (free < 0) free = 0;
    if (free > total) free = total;
    long used = total - free;
    int pct = (int)(used * 100L / total);
    snprintf(obs, ocap,
        "{\"total_mb\":%ld,\"free_mb\":%ld,\"used_mb\":%ld,\"percent_used\":%d}",
        total, free, used, pct);
}

// ===========================================================================
// Settings get/set (#292) + capability-token permission gate (#293)
//                     + #235: DELEGATION, not a second implementation
// ===========================================================================
// WHAT THIS USED TO BE, so nobody re-adds it.
//
// A hardcoded if-else chain over theme / font_size / transparency / volume /
// mute / brightness / night_light / screensaver / cursor / ip, calling the
// kernel setters directly. It was real and it worked, and it was ALSO the
// fourth partial description of the Settings surface (contract.h "WHAT WAS
// HERE BEFORE"), covering a DIFFERENT key set from the real one: it had no
// clock format, no double-click speed, no pointer speed, no accent colour, and
// its theme ids were KERNEL ids while the Settings app indexes its own
// file-based g_th[] list - the two did not even share an index space. It was
// reachable only from AI chat, so a setting the AI could change was not
// necessarily a setting the UI could, and vice versa.
//
// It now DELEGATES: /APPS/SETTINGS --contract get|set|call, through the one
// libc helper contract_invoke() (#235), which is the same path the ctl debug
// client takes. The Settings app answers from SETTINGS_ITEMS[] - the same table
// that persists /CONFIG/SETTINGS.CFG and that tools/contract-lint gates - so
// what the AI can reach and what the UI can reach are the same set by
// construction, and adding a control adds it to both in one edit.
//
// PERMISSION GATE (#293) is unchanged and still runs HERE, before this
// executor: settings.get is LOW-risk (ungated, audited), settings.set is
// HIGH-risk and aicap_authorize() has already found a token or shown the user a
// consent dialog by the time we are called. The Settings process then applies
// its OWN row-level risk class as well (CT_SAFE / CT_GUARDED / CT_DENIED), so a
// GUARDED row still fails closed for a headless caller. That is two gates on
// the same call by design, not by accident: this one bounds the AI, that one
// bounds every caller.
//
// WHAT IS DELIBERATELY NOT DELEGATED: the network keys below. Settings' Network
// panel has 3 controls and ZERO contract rows (docs/CONTRACT_API.md section 6),
// and its IP configuration is a MODAL whose fields need rows of their own
// (section 9). So network here reads and writes the NETWORK stack, not a
// Settings control, and it is labelled that way in its own error text rather
// than pretending to be part of the Settings surface.

#define AISET_APP "settings"

// The app's reply line, verbatim. "ok name=X type=bool value=1" /
// "err code=out-of-range name=X value=9 min=0 max=3". Pull one field out.
// Fields are space-separated key=value with no quoting, which is why this is
// eight lines and not a parser.
static int ct_field(const char *line, const char *key, char *out, int ocap) {
    int kl = 0; while (key[kl]) kl++;
    for (const char *p = line; *p; p++) {
        if ((p == line || p[-1] == ' ' || p[-1] == '\n') &&
            !strncmp(p, key, (size_t)kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            int o = 0;
            while (*v && *v != ' ' && *v != '\n' && o < ocap - 1) out[o++] = *v++;
            out[o] = 0;
            return o > 0;
        }
    }
    if (ocap > 0) out[0] = 0;
    return 0;
}

// The FIRST line of the app's reply (probe/get/set/call all answer in one).
static void ct_first_line(char *buf) {
    for (char *p = buf; *p; p++) if (*p == '\n') { *p = 0; return; }
}

// Cached `--contract list` for this process. One spawn per chat session, not
// one per tool call. The list is the app's own enumeration; we never hold a
// copy of what the names MEAN, only of what they ARE.
static char s_ct_names[2048];
static int  s_ct_listed = 0;

static const char *settings_item_list(void) {
    if (!s_ct_listed) {
        s_ct_listed = 1;
        char *av[1]; av[0] = (char *)"list";
        contract_invoke(AISET_APP, 1, av, s_ct_names, (int)sizeof(s_ct_names));
    }
    return s_ct_names;
}

// Resolve a caller's key to a contract item name, using ONLY the app's own
// list: exact match, then the leaf after the last dot ("brightness" ->
// "display.brightness"), then a unique substring. An ambiguous or absent key
// resolves to nothing and the caller reports the live list, so the AI corrects
// itself from what the app says it has rather than from a table here - a table
// here is precisely what this ticket removed.
static int settings_resolve(const char *key, char *out, int ocap) {
    const char *L = settings_item_list();
    if (!L[0]) return 0;
    int kl = 0; while (key[kl]) kl++;
    if (!kl) return 0;

    const char *best = 0; int bestlen = 0, nhit = 0;
    for (int pass = 0; pass < 3 && nhit != 1; pass++) {
        best = 0; nhit = 0;
        for (const char *p = L; *p; ) {
            const char *e = p; while (*e && *e != '\n') e++;
            int len = (int)(e - p);
            if (len > 0) {
                int hit = 0;
                if (pass == 0) {
                    hit = (len == kl && !strncmp(p, key, (size_t)kl));
                } else if (pass == 1) {
                    const char *leaf = p; 
                    for (const char *q = p; q < e; q++) if (*q == '.') leaf = q + 1;
                    hit = ((int)(e - leaf) == kl && !strncmp(leaf, key, (size_t)kl));
                } else {
                    for (int i = 0; i + kl <= len; i++)
                        if (!strncmp(p + i, key, (size_t)kl)) { hit = 1; break; }
                }
                if (hit) { nhit++; best = p; bestlen = len; }
            }
            p = *e ? e + 1 : e;
        }
    }
    if (nhit != 1 || !best) return 0;
    if (bestlen >= ocap) bestlen = ocap - 1;
    for (int i = 0; i < bestlen; i++) out[i] = best[i];
    out[bestlen] = 0;
    return 1;
}

// Emit "here is what actually exists" instead of a bare not-supported. The list
// is the app's, so it cannot be stale.
static void settings_unknown(const char *key, char *obs, int ocap) {
    const char *L = settings_item_list();
    int o = snprintf(obs, ocap,
        "{\"error\":\"unknown-setting\",\"key\":\"%s\",\"note\":\"ask "
        "/APPS/SETTINGS for the live list\",\"available\":\"", key);
    for (const char *p = L; *p && o < ocap - 4; p++)
        obs[o++] = (*p == '\n') ? ',' : *p;
    while (o > 0 && obs[o-1] == ',') o--;
    snprintf(obs + o, ocap - o, "\"}");
}

// Turn the app's own reply line into the JSON observation shape the tool loop
// expects. We pass its text through rather than paraphrasing it: a client that
// reworded the answer would be a second description of the result.
static void settings_reply(const char *name, char *line, char *obs, int ocap) {
    ct_first_line(line);
    char v[128], t[32], prev[64], code[48], detail[160];
    if (!strncmp(line, "ok ", 3)) {
        ct_field(line, "value", v, sizeof(v));
        ct_field(line, "type",  t, sizeof(t));
        ct_field(line, "prev",  prev, sizeof(prev));
        ct_field(line, "result", detail, sizeof(detail));
        if (prev[0])
            snprintf(obs, ocap,
                "{\"applied\":true,\"setting\":\"%s\",\"value\":%s,\"previous\":%s%s}",
                name, v[0] ? v : "null", prev,
                strstr(line, "note=clamped-or-ignored-by-app")
                    ? ",\"note\":\"the app clamped or ignored this value\"" : "");
        else if (v[0])
            snprintf(obs, ocap, "{\"setting\":\"%s\",\"type\":\"%s\",\"value\":\"%s\"}",
                     name, t[0] ? t : "?", v);
        else
            snprintf(obs, ocap, "{\"applied\":true,\"setting\":\"%s\",\"result\":\"%s\"}",
                     name, detail[0] ? detail : "ok");
        return;
    }
    ct_field(line, "code",   code,   sizeof(code));
    ct_field(line, "detail", detail, sizeof(detail));
    snprintf(obs, ocap, "{\"error\":\"%s\",\"setting\":\"%s\",\"detail\":\"%s\"}",
             code[0] ? code : "settings-app-error", name,
             detail[0] ? detail : line);
}

// The network branch, kept whole. See the header note: this is the NETWORK
// stack's surface, not a Settings control, and it says so.
static int settings_is_network_key(const char *cat, const char *key) {
    return !strcmp(cat, "network") || !strcmp(key, "ip") || !strcmp(key, "static_ip") ||
           !strcmp(key, "gateway") || !strcmp(key, "dns") || !strcmp(key, "netmask") ||
           !strcmp(key, "dhcp");
}

// settings.get(category,key) -> {value:...} or typed error. READ-only, ungated.
static void exec_settings_get(const char *args, char *obs, int ocap) {
    char cat[48], key[48];
    json_get_str(args, "category", cat, sizeof(cat));
    if (!json_get_str(args, "key", key, sizeof(key)) || !key[0]) {
        strlcpy(obs, "{\"error\":\"missing key\"}", ocap); return;
    }

    if (settings_is_network_key(cat, key)) {
        net_info_t ni;
        if (get_net_info(&ni, sizeof(ni)) < 0) { strlcpy(obs, "{\"error\":\"network-unavailable\"}", ocap); return; }
        snprintf(obs, ocap,
            "{\"source\":\"network-stack\",\"ip\":\"%s\",\"gateway\":\"%s\","
            "\"netmask\":\"%s\",\"dns\":\"%s\",\"mac\":\"%s\",\"connected\":%d}",
            ni.ip, ni.gateway, ni.netmask, ni.dns, ni.mac, ni.connected);
        return;
    }

    char name[CT_NAME_MAX];
    if (!settings_resolve(key, name, sizeof(name))) { settings_unknown(key, obs, ocap); return; }

    static char line[1024];
    char *av[2]; av[0] = (char *)"get"; av[1] = name;
    contract_invoke(AISET_APP, 2, av, line, (int)sizeof(line));
    settings_reply(name, line, obs, ocap);
}

// Apply a verified setting write. Returns 1 in every case that produced a typed
// answer (the old contract of this function), 0 only when nothing could answer.
static int settings_apply(const char *cat, const char *key, const char *val, char *obs, int ocap) {
    if (settings_is_network_key(cat, key)) {
        if (!strcmp(key, "dhcp")) {
            net_dhcp();
            snprintf(obs, ocap, "{\"applied\":true,\"source\":\"network-stack\","
                     "\"setting\":\"network.dhcp\",\"note\":\"DHCP requested\"}");
            return 1;
        }
        // "ip[,mask,gw]"; missing parts keep the current ones.
        char ip[20]={0}, mask[20]={0}, gw[20]={0}; int part=0, pi=0;
        for (const char *p = val; *p; p++) {
            if (*p == ',') { if (part==0) ip[pi]=0; else if (part==1) mask[pi]=0; else gw[pi]=0; part++; pi=0; continue; }
            if (part==0 && pi<19) ip[pi++]=*p; else if (part==1 && pi<19) mask[pi++]=*p; else if (part==2 && pi<19) gw[pi++]=*p;
        }
        if (part==0) ip[pi]=0; else if (part==1) mask[pi]=0; else gw[pi]=0;
        if (!mask[0] || !gw[0]) {
            net_info_t ni; if (get_net_info(&ni, sizeof(ni)) >= 0) {
                if (!mask[0]) strlcpy(mask, ni.netmask, sizeof(mask));
                if (!gw[0])   strlcpy(gw, ni.gateway, sizeof(gw));
            }
        }
        if (!ip[0]) { strlcpy(obs, "{\"error\":\"bad-value\",\"hint\":\"ip[,mask,gw]\"}", ocap); return 1; }
        net_set_static(ip, mask[0]?mask:"255.255.255.0", gw[0]?gw:ip);
        snprintf(obs, ocap, "{\"applied\":true,\"source\":\"network-stack\",\"setting\":\"network.ip\","
                 "\"ip\":\"%s\",\"mask\":\"%s\",\"gateway\":\"%s\"}", ip, mask, gw);
        return 1;
    }

    char name[CT_NAME_MAX];
    if (!settings_resolve(key, name, sizeof(name))) { settings_unknown(key, obs, ocap); return 1; }

    static char line[1024];
    char *av[3];
    av[0] = (char *)"set"; av[1] = name; av[2] = (char *)val;
    contract_invoke(AISET_APP, 3, av, line, (int)sizeof(line));

    // ONE routing rule, not a name table: a theme is the value the caller is
    // most likely to give by NAME rather than by index, and appearance.theme is
    // a CT_INT because its index space is the app's own /THEMES list. Settings
    // exposes appearance.set_theme_by_name for exactly that case, and the list
    // of names lives THERE. We only decide which verb to use; we never decide
    // what the names are.
    if (strstr(line, "unparseable-value") && !strcmp(name, "appearance.theme")) {
        char *cv[3];
        cv[0] = (char *)"call"; cv[1] = (char *)"appearance.set_theme_by_name"; cv[2] = (char *)val;
        contract_invoke(AISET_APP, 3, cv, line, (int)sizeof(line));
        settings_reply("appearance.set_theme_by_name", line, obs, ocap);
        return 1;
    }
    settings_reply(name, line, obs, ocap);
    return 1;
}

static void exec_settings_set(const char *args, char *obs, int ocap) {
    char cat[48], key[48], val[256];
    json_get_str(args, "category", cat, sizeof(cat));
    if (!json_get_str(args, "key", key, sizeof(key)) || !key[0]) {
        strlcpy(obs, "{\"error\":\"missing key\"}", ocap); return;
    }
    if (!json_get_str(args, "value", val, sizeof(val))) {
        // value may be numeric/unquoted; try a loose grab
        val[0] = 0;
    }
    // #293: consent + capability-token enforcement happens in aicap_authorize
    // BEFORE this executor is ever reached (the runtime shows the user a consent
    // dialog and mints a token). By the time we get here the write is authorized
    // as far as the AI gate is concerned; the Settings process then applies its
    // own row-level risk class as well.
    settings_apply(cat, key, val, obs, ocap);
}

// A tiny recursive-descent arithmetic evaluator (+ - * / parens, doubles).
static const char *g_ce_p;
static double ce_expr(void);
static void ce_ws(void) { while (*g_ce_p == ' ' || *g_ce_p == '\t') g_ce_p++; }
static double ce_num(void) {
    ce_ws();
    double sign = 1;
    if (*g_ce_p == '+') g_ce_p++;
    else if (*g_ce_p == '-') { sign = -1; g_ce_p++; }
    ce_ws();
    if (*g_ce_p == '(') {
        g_ce_p++;
        double v = ce_expr();
        ce_ws();
        if (*g_ce_p == ')') g_ce_p++;
        return sign * v;
    }
    double v = 0; int any = 0;
    while (*g_ce_p >= '0' && *g_ce_p <= '9') { v = v * 10 + (*g_ce_p - '0'); g_ce_p++; any++; }
    if (*g_ce_p == '.') {
        g_ce_p++; double f = 0.1;
        while (*g_ce_p >= '0' && *g_ce_p <= '9') { v += (*g_ce_p - '0') * f; f *= 0.1; g_ce_p++; any++; }
    }
    if (!any) return 0;
    return sign * v;
}
static double ce_term(void) {
    double v = ce_num();
    for (;;) {
        ce_ws();
        char op = *g_ce_p;
        if (op == '*') { g_ce_p++; v *= ce_num(); }
        else if (op == '/') { g_ce_p++; double d = ce_num(); v = (d != 0) ? v / d : 0; }
        else break;
    }
    return v;
}
static double ce_expr(void) {
    double v = ce_term();
    for (;;) {
        ce_ws();
        char op = *g_ce_p;
        if (op == '+') { g_ce_p++; v += ce_term(); }
        else if (op == '-') { g_ce_p++; v -= ce_term(); }
        else break;
    }
    return v;
}
static void exec_calc_eval(const char *args, char *obs, int ocap) {
    char expr[256];
    if (!json_get_str(args, "expr", expr, sizeof(expr)) || !expr[0]) {
        strlcpy(obs, "{\"error\":\"missing expr\"}", ocap); return;
    }
    g_ce_p = expr;
    double v = ce_expr();
    // format with up to a few decimals, trimming trailing zeros
    char num[64];
    long whole = (long)v;
    if (v == (double)whole) snprintf(num, sizeof(num), "%ld", whole);
    else {
        long scaled = (long)(v * 10000 + (v >= 0 ? 0.5 : -0.5));
        long ip = scaled / 10000; long fp = scaled % 10000; if (fp < 0) fp = -fp;
        snprintf(num, sizeof(num), "%ld.%04ld", ip, fp);
    }
    snprintf(obs, ocap, "{\"result\":\"%s\"}", num);
}

// Dispatch a tool id + json args. Returns 1 if the tool was recognized.
// ===========================================================================
// #294: AI-driven userland compiler (build.compile_app / build.deploy_app)
// ===========================================================================
// These are HIGH-risk tools: aicap_authorize() (consent + capability token)
// runs BEFORE the executor in the tool loop, so only an authorized call reaches
// here, and every call is audit-logged. compile_app POSTs the patched source to
// the build service, receives the ELF, and writes it to /APPS/<app_id>;
// deploy_app relaunches the app so the new binary runs. On a compile failure the
// service returns the compiler log, which we surface as the OBSERVATION so the
// AI can read the errors and iterate.

// Validate + normalize an app_id to [a-z0-9_], lowercasing. Returns 1 if ok.
static int sanitize_app_id(const char *in, char *out, int ocap) {
    int o = 0;
    for (const char *p = in; *p && o < ocap - 1; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
            out[o++] = c;
        else { out[0] = 0; return 0; }   // reject any other char (path-safety)
    }
    out[o] = 0;
    return o > 0;
}

// Async HTTPS POST to url with a JSON body; the (small JSON) response is written
// to resp[]. Returns the HTTP status (>0) on completion, negative on net error.
static int build_http_post(const char *url, const char *body,
                           char *resp, int respcap, int *resplen) {
    static char headers[64];
    snprintf(headers, sizeof(headers), "Content-Type: application/json\r\n");
    *resplen = 0;
    int job = http_post_start(url, headers, body);
    if (job < 0) return -100 + job;
    int status = 0;
    unsigned long t0 = uptime_ms();
    const unsigned long TIMEOUT_MS = 120000;   // a clean build is fast, but allow slack
    for (;;) {
        unsigned int plen = 0;
        int pstate = http_post_poll(job, &status, &plen);
        if (pstate < 0) { http_post_cancel(job); return -1; }
        if (pstate == 1) {                       // done
            int n = http_post_read(job, resp, respcap - 1);
            if (n < 0) n = 0;
            resp[n] = 0; *resplen = n;
            return status > 0 ? status : 200;
        }
        if (pstate == 2) { http_post_read(job, resp, respcap - 1); return -2; }  // net/TLS error
        if (uptime_ms() - t0 > TIMEOUT_MS) { http_post_cancel(job); return -3; }
        sys_sleep(50);
    }
}

// Async plain-HTTP GET (wget path, same as the browser's binary downloads) to
// fetch the compiled ELF. Body -> resp[], length -> *resplen. Returns HTTP
// status (>0), negative on net error.
static int build_http_get(const char *url, char *resp, int respcap, int *resplen) {
    *resplen = 0;
    int job = http_fetch_start(url);
    if (job < 0) return -100 + job;
    int status = 0;
    unsigned long t0 = uptime_ms();
    const unsigned long TIMEOUT_MS = 60000;
    for (;;) {
        unsigned int plen = 0;
        int pstate = http_fetch_poll(job, &status, &plen);
        if (pstate < 0) { http_fetch_cancel(job); return -1; }
        if (pstate == 1) {
            int n = http_fetch_read(job, resp, respcap);
            if (n < 0) n = 0;
            *resplen = n;
            return status > 0 ? status : 200;
        }
        if (pstate == 2) { http_fetch_read(job, resp, respcap); return -2; }
        if (uptime_ms() - t0 > TIMEOUT_MS) { http_fetch_cancel(job); return -3; }
        sys_sleep(30);
    }
}

// Derive the plain-HTTP artifact base "http://<host>:8898" from the (https)
// compile URL, so the ELF is fetched over the proven wget/browser GET path.
static void build_artifact_url(const char *app, char *out, int ocap) {
    // extract host from g_buildsvc = "https://<host>:<port>/compile"
    const char *p = g_buildsvc;
    if (!strncmp(p, "https://", 8)) p += 8;
    else if (!strncmp(p, "http://", 7)) p += 7;
    char host[128]; int h = 0;
    while (*p && *p != ':' && *p != '/' && h < (int)sizeof(host) - 1) host[h++] = *p++;
    host[h] = 0;
    snprintf(out, ocap, "http://%s:8898/%s.elf", host[0] ? host : "127.0.0.1", app);
}

// build.compile_app {"app_id":"hello","source":"<full main.c>"}
// Two-step: (1) HTTPS POST the source -> small JSON {"ok":true,"len":N} or
// {"ok":false,"log":...}; (2) on ok, plain-HTTP GET the ELF and write /APPS/<app>.
static void exec_build_compile(const char *args, char *obs, int ocap) {
    char app_id[64], app[64];
    if (!json_get_str(args, "app_id", app_id, sizeof(app_id)) || !app_id[0]) {
        strlcpy(obs, "{\"error\":\"missing app_id\"}", ocap); return;
    }
    if (!sanitize_app_id(app_id, app, sizeof(app))) {
        strlcpy(obs, "{\"error\":\"invalid app_id\"}", ocap); return;
    }
    static char source[BUILD_RESP_MAX];
    if (!json_get_str_full(args, "source", source, sizeof(source)) || !source[0]) {
        strlcpy(obs, "{\"error\":\"missing source\"}", ocap); return;
    }
    // Build the request body: {"app_id":"...","source":"<escaped>"}.
    static char reqbody[BUILD_RESP_MAX + 1024];
    int b = 0;
    b = str_append(reqbody, b, sizeof(reqbody), "{\"app_id\":\"");
    b = obs_escape(reqbody, b, sizeof(reqbody), app);
    b = str_append(reqbody, b, sizeof(reqbody), "\",\"source\":\"");
    b = json_escape_append(reqbody, b, sizeof(reqbody), source);
    b = str_append(reqbody, b, sizeof(reqbody), "\"}");

    // Step 1: POST source -> small JSON status.
    static char meta[4096];
    int mlen = 0;
    int st = build_http_post(g_buildsvc, reqbody, meta, sizeof(meta), &mlen);
    if (st < 0) {
        snprintf(obs, ocap, "{\"error\":\"build-service-unreachable\",\"detail\":%d,\"url\":\"%s\"}",
                 st, g_buildsvc);
        return;
    }
    if (st != 200 || strstr(meta, "\"ok\":true") == 0) {
        // Compile failed: surface the compiler log so the AI can iterate.
        char logbuf[1400];
        if (!json_get_str_full(meta, "log", logbuf, sizeof(logbuf)) &&
            !json_get_str(meta, "error", logbuf, sizeof(logbuf)))
            strlcpy(logbuf, "compile failed", sizeof(logbuf));
        int o = 0;
        o = str_append(obs, o, ocap, "{\"error\":\"compile-failed\",\"app_id\":\"");
        o = obs_escape(obs, o, ocap, app);
        o = str_append(obs, o, ocap, "\",\"log\":\"");
        o = obs_escape(obs, o, ocap, logbuf);
        o = str_append(obs, o, ocap, "\"}");
        return;
    }
    // Step 2: fetch the ELF over plain HTTP (wget path).
    char aurl[160];
    build_artifact_url(app, aurl, sizeof(aurl));
    static char resp[BUILD_RESP_MAX];
    int rlen = 0;
    int gs = build_http_get(aurl, resp, sizeof(resp), &rlen);
    if (gs < 0 || gs != 200) {
        snprintf(obs, ocap, "{\"error\":\"artifact-fetch-failed\",\"detail\":%d,\"status\":%d,\"url\":\"%s\"}",
                 gs, gs, aurl);
        return;
    }
    if (rlen < 4 || resp[0] != 0x7f || resp[1] != 'E' || resp[2] != 'L' || resp[3] != 'F') {
        snprintf(obs, ocap, "{\"error\":\"bad-artifact\",\"bytes\":%d}", rlen);
        return;
    }
    char path[96];
    snprintf(path, sizeof(path), "/APPS/%s", app);
    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { snprintf(obs, ocap, "{\"error\":\"cannot-write\",\"path\":\"%s\"}", path); return; }
    long w = sys_write(fd, resp, rlen);
    sys_close(fd);
    if (w != rlen) { snprintf(obs, ocap, "{\"error\":\"short-write\",\"wrote\":%ld,\"of\":%d}", w, rlen); return; }
    snprintf(obs, ocap,
        "{\"status\":\"compiled\",\"app_id\":\"%s\",\"path\":\"%s\",\"elf_bytes\":%d}",
        app, path, rlen);
}

// build.deploy_app {"app_id":"hello"} -> (re)launch the freshly built binary.
static void exec_build_deploy(const char *args, char *obs, int ocap) {
    char app_id[64], app[64];
    if (!json_get_str(args, "app_id", app_id, sizeof(app_id)) || !app_id[0]) {
        strlcpy(obs, "{\"error\":\"missing app_id\"}", ocap); return;
    }
    if (!sanitize_app_id(app_id, app, sizeof(app))) {
        strlcpy(obs, "{\"error\":\"invalid app_id\"}", ocap); return;
    }
    char path[96];
    snprintf(path, sizeof(path), "/APPS/%s", app);
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) { snprintf(obs, ocap, "{\"error\":\"not-built\",\"path\":\"%s\"}", path); return; }
    sys_close(fd);
    int pid = sys_spawn(path);
    if (pid < 0) { snprintf(obs, ocap, "{\"error\":\"launch-failed\",\"path\":\"%s\",\"rc\":%d}", path, pid); return; }
    snprintf(obs, ocap, "{\"status\":\"deployed\",\"app_id\":\"%s\",\"path\":\"%s\",\"pid\":%d}",
             app, path, pid);
}

static int dispatch_tool(const char *id, const char *args, char *obs, int ocap) {
    if      (!strcmp(id, "files.list"))        exec_files_list(args, obs, ocap);
    else if (!strcmp(id, "files.read"))        exec_files_read(args, obs, ocap);
    else if (!strcmp(id, "files.open"))        exec_files_open(args, obs, ocap);
    else if (!strcmp(id, "web.open"))          exec_web_open(args, obs, ocap);
    else if (!strcmp(id, "weather.current"))   exec_weather(args, obs, ocap);
    else if (!strcmp(id, "settings.get_theme"))exec_settings_theme(args, obs, ocap);
    else if (!strcmp(id, "settings.open"))     exec_settings_open(args, obs, ocap);
    else if (!strcmp(id, "terminal.open"))     exec_terminal_open(args, obs, ocap);
    else if (!strcmp(id, "calc.open"))         exec_calc_open(args, obs, ocap);
    else if (!strcmp(id, "calc.eval"))         exec_calc_eval(args, obs, ocap);
    else if (!strcmp(id, "app.launch"))        exec_app_launch(args, obs, ocap);
    else if (!strcmp(id, "storage.free") ||
             !strcmp(id, "system.storage.info")) exec_storage_info(args, obs, ocap);
    else if (!strcmp(id, "settings.get"))      exec_settings_get(args, obs, ocap);
    else if (!strcmp(id, "settings.set"))      exec_settings_set(args, obs, ocap);
    else if (!strcmp(id, "fs.write") ||
             !strcmp(id, "files.write"))       exec_fs_write(args, obs, ocap);
    else if (!strcmp(id, "fs.delete") ||
             !strcmp(id, "files.delete"))      exec_fs_delete(args, obs, ocap);
    else if (!strcmp(id, "build.compile_app")) exec_build_compile(args, obs, ocap);
    else if (!strcmp(id, "build.deploy_app"))  exec_build_deploy(args, obs, ocap);
    else { snprintf(obs, ocap, "{\"error\":\"unknown-tool\",\"id\":\"%s\"}", id); return 0; }
    return 1;
}

// Public: run ONE tool through the full #293 gate (classify -> token/consent
// check -> dispatch -> audit), writing the OBSERVATION JSON into obs[]. Returns
// the aicap_authorize() outcome (AICAP_ALLOW on success). The ReAct loop uses
// this for every model ACTION, and the headless build self-test driver (#294)
// uses it to drive build.compile_app/deploy_app deterministically (still through
// the real consent + audit path).
int aiclient_run_action(const char *id, const char *args, char *obs, int ocap) {
    char cap[40]; int risk = 0;
    aicap_classify(id, cap, sizeof(cap), &risk);
    char reason[256]; char how[80];
    int az = aicap_authorize(id, args, reason, sizeof(reason), how, sizeof(how));
    if (az == AICAP_ALLOW) {
        dispatch_tool(id, args, obs, ocap);
        int err = (strstr(obs, "\"error\"") != 0);
        aicap_audit(id, cap, args, err ? "error" : "ok", how);
    } else {
        snprintf(obs, ocap,
            "{\"error\":\"%s\",\"capability\":\"%s\",\"reason\":\"%s\"}",
            aicap_code(az), cap, reason);
        aicap_audit(id, cap, args, "denied", aicap_code(az));
    }
    return az;
}

// Parse a leading "ACTION <id> <json>" out of a model reply. Returns 1 if found,
// filling id[] and args[] (args is the rest of the line, expected to be JSON).
static int parse_action(const char *reply, char *id, int idcap, char *args, int argcap) {
    const char *p = reply;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    // tolerate a leading code fence or quotes the model might add
    if (!strncmp(p, "```", 3)) { p += 3; while (*p && *p != '\n') p++; while (*p=='\n')p++; }
    if (strncmp(p, "ACTION", 6)) return 0;
    p += 6;
    if (*p != ' ' && *p != '\t') return 0;
    while (*p == ' ' || *p == '\t') p++;
    int o = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && o < idcap - 1) id[o++] = *p++;
    id[o] = 0;
    while (*p == ' ' || *p == '\t') p++;
    o = 0;
    while (*p && *p != '\n' && *p != '\r' && o < argcap - 1) args[o++] = *p++;
    args[o] = 0;
    // if no args given, default to empty object
    if (args[0] == 0) strlcpy(args, "{}", argcap);
    return id[0] ? 1 : 0;
}

// Perform a single Kimi POST against the current g_msgs history; on success the
// assistant reply text is written into out[]. Returns: 0 ok, <0 net error,
// >0 = HTTP status (when status != 200). msg on failure goes into out[] too.
static int kimi_post_once(char *out, int ocap);
// #264: retry the network attempt a few times on a transient net failure
// (cold TLS handshake, a momentarily-full async POST worker pool, a worker that
// hit a net/TLS error). HTTP status errors and successful parses are NOT retried.
// A net failure is any negative return from kimi_post_once (job<0 / poll<0 /
// worker error / timeout all map to <0). Between tries we back off briefly so a
// busy worker pool or a TCP TIME_WAIT slot can drain.
static int kimi_post(char *out, int ocap) {
    int rc = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        rc = kimi_post_once(out, ocap);
        if (rc >= 0) return rc;            // success or HTTP-status error: done
        sys_sleep(700);                    // transient net error: brief backoff
    }
    return rc;                             // exhausted retries: report last error
}

static int kimi_post_once(char *out, int ocap) {
    // #745: refuse to send a conversation that add_msg() flagged at HIGH.
    // Returns 1, which every caller already treats as "not a network problem,
    // show this text": aichat renders it as a local message, terminal and msh
    // print it. The user is told, by name, what was refused and why. There is
    // no "send anyway", no remembered choice and no timeout that allows it.
    if (g_guard_block) {
        g_guard_block = 0;          // one-shot; the poisoned text is not in the
                                    // history, so the next turn is clean
        strlcpy(out, g_guard_note, ocap);
        return 1;
    }
    static char headers[512];
    if (g_api_style == AI_STYLE_ANTHROPIC)
        snprintf(headers, sizeof(headers),
                 "x-api-key: %s\r\nanthropic-version: 2023-06-01\r\nContent-Type: application/json\r\n",
                 g_apikey);
    else
        snprintf(headers, sizeof(headers),
                 "Authorization: Bearer %s\r\nContent-Type: application/json\r\n",
                 g_apikey);
    build_body();
    int status = 0;
    g_resp[0] = 0;
    // #264: async POST via a kernel worker proc. The Ring-3 app never runs net
    // code or blocks in a net syscall (that path hard-wedged the OS); it only
    // POLLs the worker, then READs the body. Mirrors the reliable browser GET.
    int job = http_post_start(g_endpoint, headers, g_body);
    if (job < 0) { snprintf(out, ocap, "Network error (POST start returned %d).", job); return job; }
    int r = -1;
    {
        unsigned long t0 = uptime_ms();
        // #327: generating a FULL app main.c is a large completion; Moonshot can
        // take well over a minute for it (the app-generation prompt is big). The
        // old 60s ceiling killed slow-but-valid generations mid-flight (then each
        // retry re-hit the same slow request and failed). Allow 180s per POST.
        const unsigned long TIMEOUT_MS = 180000; // 180s ceiling per POST (#327 app-gen)
        int pstate = 0;
        for (;;) {
            unsigned int plen = 0;
            pstate = http_post_poll(job, &status, &plen);
            if (pstate < 0) { http_post_cancel(job); snprintf(out, ocap, "Network error (POST poll returned %d).", pstate); return pstate; }
            if (pstate == 1) {            // done
                int n = http_post_read(job, g_resp, RESP_MAX - 1);
                if (n < 0) n = 0;
                g_resp[n] = 0;
                r = n;
                break;
            }
            if (pstate == 2) {            // worker hit a net/TLS error
                http_post_read(job, g_resp, RESP_MAX - 1);  // frees the job slot
                r = -1;
                break;
            }
            if (uptime_ms() - t0 > TIMEOUT_MS) { http_post_cancel(job); snprintf(out, ocap, "Network error (POST timed out)."); return -1; }
            sys_sleep(20);               // yield ~20ms between polls
        }
    }
    if (r < 0) { snprintf(out, ocap, "Network error (POST returned %d).", r); return r; }
    if (status != 200) {
        char emsg[1024]; emsg[0] = 0;
        if (g_resp[0] && extract_error(g_resp, emsg, sizeof(emsg)) && emsg[0])
            snprintf(out, ocap, "API error %d: %s", status, emsg);
        else
            snprintf(out, ocap, "API error: HTTP %d", status);
        return status;
    }
    if (!g_resp[0]) { strlcpy(out, "Empty response from server.", ocap); return 1; }
    if (g_api_style == AI_STYLE_ANTHROPIC) {
        if (extract_content_anthropic(g_resp, out, ocap) && out[0]) return 0;
    } else {
        if (extract_content(g_resp, out, ocap) && out[0]) return 0;
    }
    strlcpy(out, "Could not parse assistant reply from response.", ocap);
    return 1;
}

// Run the ReAct tool loop for the current conversation. Posts to Kimi; while the
// reply is an ACTION line, runs the tool, appends an OBSERVATION message, and
// re-posts. Caps at MAX_ACTIONS. The final non-ACTION reply text is left in
// final[]. If verbose != 0, each ACTION/OBSERVATION step is also printed to the
// serial console (used by the headless harness). Returns the kimi_post status of
// the last call.
static int run_tool_loop(char *final, int fcap, int verbose) {
    static char reply[RESP_MAX];
    static char obs[OBS_MAX];
    // #294: args holds a full ACTION json arg object; the build tools carry the
    // app source inline, so this is generous (a single-line escaped main.c).
    static char id[64];
    static char args[8192];

    int rc = kimi_post(reply, sizeof(reply));
    int actions = 0;
    while (rc == 0 && actions < MAX_ACTIONS &&
           parse_action(reply, id, sizeof(id), args, sizeof(args))) {
        actions++;
        if (verbose) printf("ACTION %s %s\n", id, args);
        // #293/#294: capability-token + consent gate BEFORE dispatch, then audit.
        // LOW-risk tools pass straight through; HIGH-risk tools (incl. build.*)
        // need a valid token or a user consent grant. Every outcome is appended
        // to /CONFIG/AIAUDIT.LOG inside aiclient_run_action().
        aiclient_run_action(id, args, obs, sizeof(obs));
        if (verbose) printf("OBSERVATION %s\n", obs);
        // record the action+observation in the history so Kimi sees the result,
        // using internal roles (4=assistant action, 5=tool observation) that are
        // sent to the API but never rendered in the transcript.
        add_msg(4, reply);
        char obuf[OBS_MAX + 16];
        snprintf(obuf, sizeof(obuf), "OBSERVATION %s", obs);
        add_msg(5, obuf);
        rc = kimi_post(reply, sizeof(reply));
    }
    // If we hit the cap while still emitting an ACTION, force a final answer.
    if (rc == 0 && actions >= MAX_ACTIONS &&
        parse_action(reply, id, sizeof(id), args, sizeof(args))) {
        add_msg(4, reply);
        add_msg(5, "OBSERVATION {\"note\":\"tool-budget-exhausted; answer now using what you have\"}");
        rc = kimi_post(reply, sizeof(reply));
    }
    strlcpy(final, reply, fcap);
    return rc;
}

// ===========================================================================
// Public API (#224)
// ===========================================================================
int aiclient_have_key(void)   { return g_have_key; }
int aiclient_have_tools(void) { return g_have_tools; }
int aiclient_count(void)      { return g_nmsgs; }
const ai_msg_t *aiclient_get(int i) {
    if (i < 0 || i >= g_nmsgs) return 0;
    return &g_msgs[i];
}
void aiclient_add(int role, const char *text) { add_msg(role, text); }

int aiclient_init(void) {
    if (!g_resp) g_resp = (char *)malloc(RESP_MAX);
    if (!g_body) g_body = (char *)malloc(BODY_MAX);
    if (g_resp) g_resp[0] = 0;
    if (g_body) g_body[0] = 0;
    load_aisvc();     // #367: provider-agnostic endpoint/model/key/style (overrides defaults)
    load_buildsvc();  // #294: build-service URL override
    load_appgen();    // #327: app-generation RAG corpus for chat-to-app
    load_tools();
    aicap_init();   // #293: load persisted capability grants
    return g_have_key;
}

void aiclient_reset(void) {
    for (int i = 0; i < g_nmsgs; i++) { if (g_msgs[i].text) free(g_msgs[i].text); g_msgs[i].text = 0; }
    g_nmsgs = 0;
    g_guard_block = 0;              // #745: a new conversation starts clean
    g_guard_note[0] = 0;
    add_msg(3, system_prompt());
}

int aiclient_run_turn(char *out, int outcap, int verbose) {
    if (!g_resp || !g_body) { strlcpy(out, "aiclient: not initialized", outcap); return -1; }
    if (!g_have_key)        { strlcpy(out, "aiclient: no API key. Set one in Settings > AI.", outcap); return -1; }
    return run_tool_loop(out, outcap, verbose);
}

int aiclient_ask(const char *prompt, char *out, int outcap, int verbose) {
    if (!g_resp || !g_body) aiclient_init();
    if (!g_have_key)        { strlcpy(out, "aiclient: no API key. Set one in Settings > AI.", outcap); return -1; }
    aiclient_reset();
    aiclient_add(0, prompt);
    return aiclient_run_turn(out, outcap, verbose);
}
