// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aicap.h - MayteraOS AI capability tokens + consent + audit (#293).
//
// This is the SECURITY GUARDRAIL that gates the AI tool loop (aiclient.c) before
// any powerful write/execute tool runs. It implements the temporal capability
// TOKEN model from aitools/PROTOCOL.md (layer 3) and docs/LLM_CONTRACTS.md:
//
//   - A RISK TABLE classifies every wired tool into a capability namespace
//     (system.* / app.* / fs.* / media.*) and a risk level (LOW or HIGH).
//   - LOW-risk tools (read-only files/weather/storage/settings.get, curated
//     launches) always run, but are still AUDIT-LOGGED.
//   - HIGH-risk tools (fs.write/delete, settings.set, terminal/python.execute,
//     build.*) require a valid, unexpired, non-exhausted capability TOKEN whose
//     constraints (allowed_paths, denied_commands, max_uses, expires_at) permit
//     the specific arguments. Missing token -> the runtime raises CONSENT (the
//     app shows a dialog); on grant a token is minted, on deny the tool returns
//     CAPABILITY_DENIED. An out-of-scope / expired / exhausted token returns
//     CAPABILITY_DENIED / TOKEN_EXPIRED / TOKEN_EXHAUSTED.
//   - Every authorization decision is appended to the session user's own audit
//     trail, <home>/CONFIG/AIAUDIT.LOG (aicap_audit_path()). See the THREAT
//     MODEL block below for what that record is and is not evidence of.
//
// Enforcement currently lives in userland (at the aiclient dispatch boundary).
// Task #305 will move this into a protected, immutable kernel core. The API here
// is deliberately the single choke point so that move is mechanical.
#ifndef AICAP_H
#define AICAP_H

// Risk levels.
enum { AICAP_RISK_LOW = 0, AICAP_RISK_HIGH = 1 };

// Consent decisions returned by the consent callback the host app registers.
enum {
    AICAP_CONSENT_DENY    = 0,   // user denied -> CAPABILITY_DENIED
    AICAP_CONSENT_ONCE    = 1,   // allow exactly this one use (max_uses=1)
    AICAP_CONSENT_SESSION = 2,   // allow for the rest of this session
    AICAP_CONSENT_PERSIST = 3    // allow + persist the grant to /CONFIG/AICAPS.CFG
};

// Authorization outcomes from aicap_authorize().
enum {
    AICAP_ALLOW     = 0,   // run the tool (use recorded/consumed)
    AICAP_DENIED    = 1,   // CAPABILITY_DENIED (no token / user denied / out of scope)
    AICAP_EXPIRED   = 2,   // TOKEN_EXPIRED
    AICAP_EXHAUSTED = 3    // TOKEN_EXHAUSTED (max_uses reached)
};

// Consent callback: the host app describes the request to the user and returns
// one of AICAP_CONSENT_*. tool_id/cap/risk/args identify the request; target is
// the extracted sensitive operand (a path for fs.*, "category.key=value" for
// settings, the command/code for execute tools) for a precise prompt.
typedef int (*aicap_consent_fn)(const char *tool_id, const char *cap, int risk,
                                const char *args, const char *target);

// Initialize the subsystem: load persisted grants from this session's grant
// store (aicap_grants_path(); "/CONFIG/AICAPS.CFG" for a root session). Safe to
// call repeatedly (a second call is a no-op once loaded).
void aicap_init(void);

// Re-read the grant store, DISCARDING the cached grant table. aicap_init()
// caches on first use for the AI loop's sake, but a LONG-RUNNING process that
// gates on behalf of others (a live-contract app answering delivered calls,
// contract.c) must reflect a grant made mid-session, exactly as a freshly
// SPAWNED contract process would. This makes the live path's authorization
// equivalent to the spawn path's, not stuck on the table that was current at
// startup.
void aicap_reload(void);

// Register the consent callback. If none is registered, HIGH-risk tools with no
// valid token are DENIED (fail closed).
void aicap_set_consent_cb(aicap_consent_fn fn);

// Classify a tool id. Fills cap_out (the capability string) and *risk_out.
// Unknown tools default to capability == tool_id and risk HIGH (fail closed).
// Returns 1 if the tool was found in the table, 0 if it defaulted.
int  aicap_classify(const char *tool_id, char *cap_out, int capcap, int *risk_out);

// The capability string for a tool id (convenience for the audit caller).
const char *aicap_cap_of(const char *tool_id);

// English error code for a non-ALLOW authorization outcome.
const char *aicap_code(int outcome);

// The full gate: classify -> (LOW: allow) / (HIGH: token check -> consent).
// On a non-ALLOW outcome an English reason is written to reason_out. authmode_out
// (optional) receives how it was authorized ("low-risk" / "token:<id>" /
// "consent-granted"). This call DOES NOT audit; the caller audits with the
// result of actually running the tool so the log reflects ok/error/denied.
int  aicap_authorize(const char *tool_id, const char *args,
                     char *reason_out, int reasoncap,
                     char *authmode_out, int authmodecap);

// Read (and consume) a pre-seeded consent decision for tool_id/cap from
// /CONFIG/AICONSENT.CFG. Returns an AICAP_CONSENT_* value or -1 if none. A
// registered GUI consent callback can call this to auto-resolve in headless
// tests after it has shown the dialog (so a screendump still captures it).
int  aicap_preseed_consent(const char *tool_id, const char *cap);

// ---------------------------------------------------------------------------
// #aiaudit THREAT MODEL. Read this before citing the audit trail as a control.
//
// WHAT IT IS FOR: answering "what did the AI do on my behalf?" for the person
// whose session it is. Which tool ran, with which arguments, how it was
// authorized (low-risk / token / consent), and what came back. It is the
// user's own record of their own assistant's behaviour.
//
// WHAT IT IS NOT. It is NOT evidence against the machine's owner, and it is NOT
// evidence against a hostile Ring-3 process. Two separate reasons, and the
// second is the load-bearing one:
//
//  1. The file lives in the user's home, so the user can truncate or delete it.
//     That is deliberate. Making a record of a person's own AI conversations
//     un-expungeable by that person is a different product decision, with a
//     privacy cost, that nobody asked for. The threat model is "what did my
//     assistant do", not "prove it to a third party against a hostile owner".
//
//  2. MORE IMPORTANTLY, the integrity ceiling of this log is set by where the
//     ENFORCEMENT lives, not by where the FILE lives. aicap is pure userland:
//     a hostile app does not have to call aicap_authorize(), so it does not
//     have to call aicap_audit() either. Relocating the sink to a root-owned
//     file, or behind a new Ring-0 syscall, would not record one action of an
//     actor who simply declines to log. It would buy assurance the design
//     cannot deliver, and it would mix PROCESS CLAIMS into
//     /CONFIG/SECURITY.LOG, every line of which is currently something Ring 0
//     WITNESSED. A log in which a reader cannot tell a witnessed fact from a
//     claim is worth less than two logs. So: one sink, honestly scoped.
//     Tamper-evidence for AI actions arrives with #305 (the protected
//     immutable core), when the GATE moves, not before.
//
// What IS tamper-evident today is the subset Ring 0 witnesses for itself:
// proc/caps.c records every capability grant, refusal and revocation through
// seclog_report_capability() into /CONFIG/SECURITY.LOG. That covers the
// kernel-gated operations (input injection, screen capture) and NOT the
// majority of AI tools, which are userland dispatches.
// ---------------------------------------------------------------------------

// Append one record to the session user's audit trail:
//   <rtc-timestamp>|<tool>|<cap>|<args-truncated>|<result>|<how>
// A refused append is NEVER silent: see log_append() in aicap.c.
void aicap_audit(const char *tool_id, const char *cap, const char *args,
                 const char *result, const char *how);

// Resolve where THIS SESSION's audit trail / loop-metrics file lives, so that a
// reader (a test, a future viewer app) asks the same function the writer does
// instead of hardcoding a path that the writer has since moved. Writes an
// absolute path into `out`. Returns 0, or -1 if it will not fit.
//
// For a ROOT session, and for a session with no passwd entry at all, both
// resolve to the historic "/CONFIG/<NAME>".
int aicap_audit_path(char *out, unsigned long cap);
int aicap_metric_path(char *out, unsigned long cap);

// #peruserpath: where THIS SESSION's persisted GRANT STORE lives,
// "<home>/CONFIG/AICAPS.CFG" (so "/CONFIG/AICAPS.CFG" for root). Same rule and
// same reason as the two above, and there is NO legacy read fallback here: a
// grant is an authorization, so inheriting another principal's persisted grants
// would be an escalation, not an upgrade convenience. See the long comment at
// the AICAPS_NAME define in aicap.c for what was measured.
int aicap_grants_path(char *out, unsigned long cap);

// ---------------------------------------------------------------------------
// #469m AI LOOP INSTRUMENTATION. One record per phase-bearing step of the AI
// loop, written through the SAME bounded-tail append and the SAME RTC
// timestamp aicap_audit() uses (one implementation, in aicap.c), to a SEPARATE
// file.
//
// WHY A SEPARATE FILE AND NOT MORE COLUMNS ON AIAUDIT.LOG. The audit log is a
// SECURITY artifact with a bounded tail: a metrics record is emitted several
// times per turn, and folding telemetry into that same bounded buffer would let
// a chatty measurement run EVICT the authorization records the log exists to
// keep. Different retention needs, different file; the append implementation,
// the cap logic and the timestamp are shared so the two cannot drift.
//
// REDACTION BY CONSTRUCTION. This record carries NO request body, NO response
// body, NO HTTP headers, NO endpoint URL and NO tool arguments: only sizes,
// durations, counts, the configured MODEL NAME, and enumerated outcome strings.
// The bearer token exists in aiclient.c only inside the headers buffer handed
// to http_post_start(), which nothing here can reach. The one caller-supplied
// string, `tool`, is sanitized to [A-Za-z0-9._-] and truncated. A credential
// cannot appear in this file because no field that could carry one is written.
//
// Durations are MICROSECONDS from mono_us() (TSC-backed), never uptime_ms():
// blame.md's timer-ticks-is-not-a-wall-clock entry records that KVM replays a
// starved vCPU's lost tick IRQs in BURSTS, so a tick-derived interval misreports
// real elapsed time during exactly the stalls an instrument needs to see.
// 0 means NOT MEASURED for this record kind, never "took no time".
// ---------------------------------------------------------------------------
// The per-user metrics sink. Resolve it with aicap_metric_path(); this is only
// the leaf name and the pre-#aiaudit absolute path it falls back to.
#define AIMETRIC_NAME   "AIMETRIC.LOG"
#define AIMETRIC_LEGACY "/CONFIG/AIMETRIC.LOG"

typedef struct {
    const char *op;        // "chat" | "vision" | "action" | "capture"
    const char *model;     // configured model name (not a secret), "" if unknown
    const char *style;     // "bearer" | "anthropic" | ""
    const char *outcome;   // enumerated, see aiclient.c: ok / http_<n> / neterr /
                           // timeout / guard_block / no_content / denied / ...
    const char *tool;      // sanitized tool id for an "action" record, else ""
    int  turn;             // monotonic turn id within this process
    int  step;             // step within the turn (0 = first POST)
    int  attempt;          // transport attempt 1..3; retries are SEPARATE records
    long req_bytes;        // request body bytes actually sent
    long resp_bytes;       // response body bytes read
    long img_bytes;        // source image bytes before base64 (0 = none)
    long b64_bytes;        // base64-encoded image bytes (0 = none)
    long tok_in;           // prompt tokens the API reported, -1 = not reported
    long tok_out;          // completion tokens the API reported, -1 = not reported
    unsigned long us_build;     // request body marshalling
    unsigned long us_encode;    // base64 (chat/vision) or JPEG (capture) encode
    unsigned long us_poststart; // the http_post_start() call itself
    unsigned long us_ttfb;      // post_start return -> first poll reporting bytes
    unsigned long us_total;     // post_start -> response complete (or failure)
    unsigned long us_parse;     // reply content extraction
    unsigned long us_dispatch;  // tool gate + executor (action records)
    unsigned long us_capture;   // screenshot request + wait for the file
    unsigned long us_decode;    // BMP decode of the captured frame
    unsigned long us_scale;     // crop + downscale
    unsigned long us_io;        // reading/writing the frame file
    // #469m RECORD VERSION aim2. us_state is the cost of refreshing the
    // one-line running-app state (a wm_get_windows syscall) that the system
    // prompt now carries on every POST. It is here because that line is the
    // single largest measured accuracy lever in the AI loop and it is paid for
    // on EVERY request, so the claim "it is cheap" has to be a column in the
    // log rather than an assertion in a comment. A reader of an aim1 line will
    // simply find one fewer field; the version tag is what tells them which.
    unsigned long us_state;     // wm_get_windows() for the running-app line
} aicap_metric_t;

// Emit one instrumentation record. Never blocks on the network; a failed write
// is silently dropped, because losing a measurement must never change the
// behaviour of the thing being measured.
void aicap_metric(const aicap_metric_t *m);

// ---------------------------------------------------------------------------
// #712 escrow support. These expose the existing token mint/revoke and add a
// process-lifetime capability DENYLIST, so the escrow layer (escrow.c) can:
//   - mint a scoped grant (aicap_grant_scoped) instead of hand-rolling tokens,
//   - revoke the grant as a set for an early close (aicap_revoke_tag),
//   - forbid a capability outright for a contract's lifetime
//     (aicap_deny_capability).
//
// CORRECTED 2026-10-03. THIS COMMENT USED TO END "..., which is how the
// no-delete promise is enforced", AND THAT WAS NOT TRUE OF ANY SHIPPED BUILD.
// Measured: `aicap_deny_capability` had exactly three mentions in the whole
// tree, this comment, its declaration below, and its definition in aicap.c.
// NOTHING CALLED IT. The escrow grant/deny half moved to Ring 0 when the
// kernel chokepoint landed (escrow.c says so in as many words: "the old
// userland aicap grant/deny ... is GONE from this file"), and this sentence was
// not updated, so the file claiming to be the enforcement was the one place
// the enforcement provably was not.
//
// WHERE THE ENFORCEMENT ACTUALLY IS: kernel/fs/escrow_guard.c
// escrow_fs_guard(), called from every FS-mutation syscall. A Ring-3 process
// cannot bypass it by issuing raw syscalls, which is exactly why the userland
// version was not good enough. And since 2026-10-03 the rule it enforces is no
// longer "no delete": a delete by a marked process SUCCEEDS and is REDIRECTED
// into the user's Recycle Bin (kernel/fs/recycle_fs.c), announced to the user,
// and restorable. See docs/AI_ESCROW_PROMISE.md.
//
// THE DENYLIST BELOW IS STILL REAL CODE AND IS STILL WIRED INTO THE GATE: a
// denied capability is refused by aicap_authorize()/aicap_path_in_scope() ahead
// of any token or consent, and that path has a measured negative control. What
// it does NOT have is a shipping caller. Do not describe it as an enforcement
// mechanism for anything until one exists.
// A denied capability is refused by aicap_authorize()/aicap_path_in_scope()
// regardless of any token or consent. Denies are refcounted so nested contracts
// forbidding the same capability compose correctly.
// ---------------------------------------------------------------------------

// Mint a HIGH-risk token for `cap` (exact, e.g. "fs.write", or wildcard "fs.*")
// scoped to `allowed_paths` (comma-separated prefixes; empty = any), expiring in
// `ttl_secs` (<=0 = never), usable `max_uses` times (-1 = unlimited), tagged with
// `tag` (used by aicap_revoke_tag). NOT persisted. Returns the token id string
// (stable for the token's lifetime) or 0 on failure.
const char *aicap_grant_scoped(const char *cap, const char *allowed_paths,
                               long ttl_secs, int max_uses, const char *tag);

// Revoke every in-memory token whose audit_tag equals `tag`. Returns the count
// revoked. Used by escrow_close() to end a grant early.
int aicap_revoke_tag(const char *tag);

// Add / remove a capability to the process denylist (refcounted). While denied,
// aicap_authorize() and aicap_path_in_scope() refuse the capability outright.
void aicap_deny_capability(const char *cap);
void aicap_allow_capability(const char *cap);
// 1 if `cap` is currently denied by the denylist (for tests/introspection).
int  aicap_cap_denied(const char *cap);

// Non-consuming scope predicate (#711). Returns 1 if `path` would be permitted
// for the capability of `tool_id` by the current usable tokens (or the tool is
// LOW-risk / read-only), else 0. It does NOT consume a use, mint a token,
// prompt for consent, or audit. Multi-path tools (files.move) use it to require
// BOTH ends of a move to sit inside the granted scope: aicap_authorize() gates
// (and consumes on) the source, and this checks the destination.
int aicap_path_in_scope(const char *tool_id, const char *path);

#endif // AICAP_H
