// proc/caps.h - Stage 1 of the system capability API.
// C view of rustkern/caps.rs. See that file for the grant model (why a grant on
// process_t, not a signed bearer token), the syscall->capability requirement
// table, and the consent state machine (the #745 elevate single-slot shape
// applied to a capability decision).
//
// docs/SYSTEM_CAPABILITY_API.md sections 4-8, Stage 1.

#ifndef PROC_CAPS_H
#define PROC_CAPS_H

#include "../types.h"
#include "process.h"   // cap_grant_t, CAP_MAX_GRANTS

// --- capability classes (MUST match caps.rs CAP_* ) ------------------------
#define CAP_NONE            0u
#define CAP_INPUT_INJECT    1u
#define CAP_INPUT_OBSERVE   2u
#define CAP_SCREEN_CAPTURE  3u
#define CAP_SCREEN_STREAM   4u
#define CAP_AUDIO_OUTPUT    5u
#define CAP_SERIAL_PORT     6u
#define CAP_NET_CONNECT     7u
#define CAP_NET_LISTEN      8u
#define CAP_CLASS_MAX       9u

// --- scope kinds -----------------------------------------------------------
#define CAP_SCOPE_NONE      0u
#define CAP_SCOPE_PATH      1u
#define CAP_SCOPE_PORT      2u
#define CAP_SCOPE_WINDOW    3u   // input.inject: a window the caller owns (token "self")
#define CAP_SCOPE_WINDOW_TARGET 4u   // input.inject: a consented window the caller does NOT own (Stage 4)

// --- input provenance (design section 10) ---------------------------------
// A synthetic event MUST be distinguishable from a real one INSIDE the kernel,
// and Ring 3 MUST NOT be able to forge the "real" flag. The bit is used at
// exactly one place: user_window_queue_event() stamps the elevation input
// credit (process_t.elev_last_input_ms) ONLY for INPUT_SRC_HW. Every hardware /
// compositor-relay delivery passes INPUT_SRC_HW; the capability-gated
// SYS_CAP_INJECT_* path passes INPUT_SRC_SYNTHETIC and CANNOT reach the stamp,
// so an app holding input.inject cannot manufacture the credit its own next
// grant or elevation requires. Ring 3 never chooses this value: the syscall
// number fixes it. It does NOT cross to userland (gui_event_t is unchanged).
#define INPUT_SRC_HW        0u
#define INPUT_SRC_SYNTHETIC 1u

// --- consent-request states (MUST match caps.rs CAP_ST_*) ------------------
#define CAP_ST_IDLE     0u
#define CAP_ST_OPEN     1u
#define CAP_ST_GRANTED  2u
#define CAP_ST_DENIED   3u

// --- refusal codes (MUST match caps.rs, all negative) ----------------------
#define CAP_EARG        (-1)
#define CAP_EBUSY       (-2)
#define CAP_EDENIED     (-3)
#define CAP_ENOINPUT    (-4)
#define CAP_ENOCONSENT  (-5)
#define CAP_ESCOPE      (-6)
#define CAP_EPOLICY     (-7)
#define CAP_EMAX        (-8)
#define CAP_ESTALE      (-9)
#define CAP_EPERM       (-10)   // a compositor-only cap syscall from a non-compositor

// --- SYS_CAP_RESOLVE actions (compositor only) -----------------------------
#define CAP_ACT_DENY     0
#define CAP_ACT_APPROVE  1
// #capalways: approve AND remember, so a later identical request from the
// same (uid, app, cap, scope) skips both the prompt and the input-credit
// requirement. Only the compositor can send it, only while its own prompt
// is open, so a standing consent can still only be born from a real human
// decision at the trusted surface.
#define CAP_ACT_APPROVE_ALWAYS  2

#define CAP_USES_UNLIMITED 0xFFFFFFFFu

// --- ABI structs (crossing the syscall boundary) --------------------------
// SYS_CAP_QUERY out. Mirrors CapState in caps.rs; size locked in
// syscall_argtab_lock.c.
typedef struct {
    uint32_t cap;
    uint32_t held;
    uint64_t expires_ms;
    uint32_t uses_left;
    uint32_t scope_kind;
    uint64_t granted_seq;
    char     scope[64];
} cap_state_t;

// SYS_CAP_REQUEST in. Mirrors cap_req_t layout the app fills. reason is display
// text, sanitised on the way in; scope is the noun, validated against the
// kernel's terms before it means anything. There is no field the app controls
// that decides where the privilege applies except the scope, which is checked.
typedef struct {
    uint32_t cap;
    uint32_t duration_ms;   // 0 = one use; clamped to GRANT_MAX_TTL_MS
    uint32_t scope_kind;
    uint32_t reason_len;
    char     reason[120];
    char     scope[64];
} cap_req_t;

// SYS_CAP_VIEW out (compositor). Mirrors CapView in caps.rs.
typedef struct {
    uint64_t seq;
    uint64_t opened_ms;
    uint32_t state;
    uint32_t req_pid;
    uint32_t req_uid;
    uint32_t cap;
    uint32_t duration_ms;
    uint32_t scope_kind;
    char     app[64];
    char     reason[120];
    char     scope[64];
} cap_view_t;

// Kernel-internal (never crosses the syscall boundary). Mirrors CapApproveInfo.
typedef struct {
    uint32_t req_pid;
    uint32_t req_uid;
    uint32_t cap;
    uint32_t duration_ms;
    uint32_t scope_kind;
    uint32_t pad;
    char     scope[64];
} cap_approve_info_t;

typedef struct { uint32_t c[6]; } cap_ledger_t;

// Ledger kinds (MUST match caps.rs LEDGER_*).
#define CAP_LEDGER_REFUSED   0u
#define CAP_LEDGER_ALLOWED   1u
#define CAP_LEDGER_REQUESTED 2u
#define CAP_LEDGER_GRANTED   3u
#define CAP_LEDGER_DENIED    4u
#define CAP_LEDGER_REVOKED   5u

// --- Rust FFI (rustkern/caps.rs) -------------------------------------------
uint32_t cap_required_for_syscall(uint64_t num);
int      cap_is_issuable(uint32_t cap);
int      cap_scope_valid(uint32_t cap, uint32_t scope_kind, const char *scope);
int      cap_find_live(const cap_grant_t *grants, uint32_t len, uint32_t cap, uint64_t now_ms);
int      cap_covers_path(const cap_grant_t *grants, uint32_t len, uint32_t cap,
                         const char *path, uint64_t now_ms);
int      cap_covers_port(const cap_grant_t *grants, uint32_t len, uint32_t cap,
                         const char *port, uint64_t now_ms);
int      cap_find_scope(const cap_grant_t *grants, uint32_t len, uint32_t cap,
                        uint32_t scope_kind, uint64_t now_ms);
int      cap_covers_window_target(const cap_grant_t *grants, uint32_t len, uint32_t cap,
                                  uint64_t target_id, uint64_t now_ms);
int      cap_issue(cap_grant_t *grants, uint32_t len, uint32_t cap, uint32_t scope_kind,
                   const char *scope, uint64_t expires_ms, uint32_t uses, uint64_t seq);
int64_t  cap_consume_use(cap_grant_t *grants, uint32_t len, uint32_t idx);
uint64_t cap_revoke(cap_grant_t *grants, uint32_t len, uint32_t cap);
int      cap_query(cap_grant_t *grants, uint32_t len, uint32_t cap, uint64_t now_ms,
                   cap_state_t *out);
void     cap_ledger_note(uint32_t kind);
int      cap_ledger_get(cap_ledger_t *out);

int64_t  cap_req_open_rs(uint32_t pid, uint32_t uid, uint64_t now_ms, uint32_t cap,
                         uint32_t duration_ms, uint32_t scope_kind, const char *reason,
                         const char *scope, const char *app, uint32_t elev_open);
int      cap_req_view_rs(cap_view_t *out);
int      cap_req_state_rs(uint64_t seq);
uint32_t cap_req_owner_pid_rs(void);
uint32_t cap_req_busy_rs(void);
int      cap_req_approve_info_rs(cap_approve_info_t *out);
int      cap_req_resolve_rs(uint64_t seq, uint32_t approve);
int      cap_req_reap_rs(uint64_t seq);
int      cap_req_tick_rs(uint64_t now_ms, uint32_t requester_alive);
uint32_t caps_selftest_rs(void);

// --- C glue (proc/caps.c) --------------------------------------------------
// THE DISPATCHER CHOKEPOINT GATE. Called once in syscall_dispatch_inner(),
// immediately after syscall_validate_args(), before the switch. Returns 0 if
// the syscall is not gated OR the caller holds the required capability; else a
// negative CAP_E* which the dispatcher returns. CLASS level: "does the caller
// hold this capability at all"; the fine-grained scope match is done in the
// gated handler (sys_screenshot_request) where the path is already bounced.
int64_t syscall_cap_check(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3);

// Syscall bodies.
int64_t sys_cap_query(uint32_t cap, cap_state_t *u_out);
int64_t sys_cap_request(const cap_req_t *u_req);
int64_t sys_cap_status(uint64_t seq);
int64_t sys_cap_view(cap_view_t *u_out);       // compositor only
int64_t sys_cap_resolve(uint64_t seq, int action); // compositor only
int64_t sys_cap_revoke(uint32_t cap);

// Stage 3/4 input.inject syscall bodies. Gated by the chokepoint (input.inject)
// before they run; each then does the window scope match (self OR a consented
// cross-app WINDOW_TARGET), the consent-surface / lock-screen guard, one use
// consume, and posts a synthetic (credit-free) event to the authorized window.
int64_t sys_cap_inject_key(int win, int keycode);
// #469 AI-VISION: the RELEASE half (same gate, same guard, EVENT_KEY_UP).
int64_t sys_cap_inject_key_up(int win, int keycode);
int64_t sys_cap_inject_mouse(int win, int x, int y, int type, uint32_t button);

// Does the CURRENT process hold a live grant of `cap` covering `path`, and
// consume one use if so? 1 = permitted (a use consumed), 0 = refused. Used by
// the gated screenshot handler.
int  caps_current_covers_path(uint32_t cap, const char *path);

// Serial twin: does the CURRENT process hold a live serial.port grant naming
// EXACTLY "port", and consume one use if so? 1 = permitted, 0 = refused.
int  caps_current_covers_port(uint32_t cap, const char *port);

// input.inject authorization (Stage 3 self + Stage 4 cross-app target). Given
// whether the CURRENT process OWNS the target window and that window's stable
// id, decide and CONSUME one grant use:
//   0           authorized: a self (CAP_SCOPE_WINDOW) grant if owns_target,
//               else a CAP_SCOPE_WINDOW_TARGET grant whose bound id==target_id;
//   CAP_ESCOPE  holds an input.inject grant, but none covers this window;
//   CAP_EDENIED holds no input.inject grant at all.
// The self and target grants are told apart by scope kind (cap_find_scope), so a
// target-only grant can never authorize a self-inject and vice versa. The
// concrete window liveness / compositor-surface exclusion is done by the
// SYS_CAP_INJECT_* handlers against the live window table before calling this.
int64_t caps_inject_authorize(int owns_target, uint64_t target_id);

// Boot self-test driver + observability ledger printer (mirrors capgate).
void caps_selftest(void);
void caps_report(void);


// --- standing consent ("Always allow"), rustkern/caps.rs -------------------
// Policy + table live in Rust; only the /CONFIG file I/O is C (the same split
// the rest of this file uses).
void     cap_always_reset_rs(void);
uint32_t cap_always_count_rs(void);
int      cap_always_match_rs(uint32_t uid, uint32_t cap, uint32_t scope_kind,
                             const char *app, const char *scope);
int64_t  cap_always_add_rs(uint32_t uid, uint32_t cap, uint32_t scope_kind,
                           const char *app, const char *scope);
int      cap_always_parse_rs(const void *buf, uint32_t len);
int      cap_always_serialize_rs(void *out, uint32_t cap_len);
int64_t  cap_req_autogrant_rs(uint32_t pid, uint32_t uid, uint64_t now_ms, uint32_t cap,
                              uint32_t duration_ms, uint32_t scope_kind, const char *reason,
                              const char *scope, const char *app, uint32_t elev_open);
// Load /CONFIG/CAPALLOW.CFG into the table. Safe to call before the FS is up.
void     cap_always_load(void);

#endif // PROC_CAPS_H
