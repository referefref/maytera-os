// aicap.h - MayteraOS AI capability tokens + consent + audit (#293).
//
// This is the SECURITY GUARDRAIL that gates the AI tool loop (aiclient.c) before
// any powerful write/execute tool runs. It implements the temporal capability
// TOKEN model from aitools/PROTOCOL.md (layer 3) and the internal LLM contracts design notes:
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

#endif // AICAP_H
