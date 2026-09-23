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
//   - Every authorization decision is appended to /CONFIG/AIAUDIT.LOG.
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

// Initialize the subsystem: load persisted grants from /CONFIG/AICAPS.CFG. Safe
// to call repeatedly (a second call is a no-op once loaded).
void aicap_init(void);

// Re-read /CONFIG/AICAPS.CFG, DISCARDING the cached grant table. aicap_init()
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

// Append one record to /CONFIG/AIAUDIT.LOG:
//   <rtc-timestamp>|<tool>|<cap>|<args-truncated>|<result>|<how>
void aicap_audit(const char *tool_id, const char *cap, const char *args,
                 const char *result, const char *how);

// ---------------------------------------------------------------------------
// #712 escrow support. These expose the existing token mint/revoke and add a
// process-lifetime capability DENYLIST, so the escrow layer (escrow.c) can:
//   - mint a scoped grant (aicap_grant_scoped) instead of hand-rolling tokens,
//   - revoke the grant as a set for an early close (aicap_revoke_tag),
//   - forbid a capability outright for a contract's lifetime
//     (aicap_deny_capability), which is how the no-delete promise is enforced.
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
