// proc/caps.c - Stage 1 capability API glue. See proc/caps.h for the C view and
// rustkern/caps.rs for the DECISIONS. This file is the part the 2026-07-16 rule
// allows to be C: it reaches into process_t for the caller's grant array,
// bounces syscall arguments out of Ring 3, and calls the journal / seclog /
// bootlog surfaces. Nothing here decides policy; every rule is in caps.rs.
#include "caps.h"
#include "process.h"
#include "syscall.h"
#include "../security/seclog.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/bootlog.h"
#include "../fs/fat.h"      // #capalways: /CONFIG/CAPALLOW.CFG persistence
#include "../mm/heap.h"     // #capalways: kfree for the fat_read_file buffer
#include "../fs/graphfs/journal.h"   // GFSJ_OP_EDGE_ADD / EDGE_REVOKE / ACTOR_PID

extern uint64_t sched_now_ms(void);
extern process_t *proc_get(uint32_t pid);
extern int fb_owner_is(uint32_t pid);       // gui/fbown.h: the framebuffer latch
extern uint32_t fbown_owner_rs(void);       // 0 if unclaimed (no compositor yet)
extern int copy_from_user(void *dst, const void *usrc, unsigned long n);
extern int copy_to_user(void *udst, const void *src, unsigned long n);
extern uint32_t elev_owner_pid_rs(void);    // #745: an elevation prompt open?
extern int serialport_is_published(const char *name); // drivers/serialport.c
// Stage 4: the kernel window registry resolver (proc/syscall.c). Given a window
// HANDLE, returns 0 and fills the window's STABLE id + sanitized title when the
// handle names a live, targetable (non-compositor) user window; negative else.
extern int userwin_target_resolve(int handle, uint64_t *out_id, char *out_title,
                                  unsigned titlesz);

// Input-credit window, same value and meaning as INPUT_WINDOW_MS in caps.rs and
// elevate.rs: a consent prompt may only be raised in RESPONSE to real input the
// window manager delivered to a window this process owns. After Stage 0 gated
// SYS_INJECT_KEY to the compositor, an ordinary app can no longer forge this.
#define CAP_INPUT_WINDOW_MS 10000ULL
// A one-use grant (duration_ms == 0) still needs a moment to be used. This
// bounds it: a use, then it is gone, and if never used it expires.
#define CAP_ONEUSE_TTL_MS   60000ULL

static const char *cap_name(uint32_t cap)
{
    switch (cap) {
    case CAP_INPUT_INJECT:   return "input.inject";
    case CAP_INPUT_OBSERVE:  return "input.observe";
    case CAP_SCREEN_CAPTURE: return "screen.capture";
    case CAP_SCREEN_STREAM:  return "screen.stream";
    case CAP_AUDIO_OUTPUT:   return "audio.output";
    case CAP_SERIAL_PORT:    return "serial.port";
    case CAP_NET_CONNECT:    return "net.connect";
    case CAP_NET_LISTEN:     return "net.listen";
    default:                 return "cap?";
    }
}

// Parse a bare decimal window HANDLE from a scope string (shape already checked
// by cap_scope_valid: leading decimal). -1 if there is no leading digit.
static int cap_parse_handle(const char *sc)
{
    int v = 0, any = 0;
    for (const char *pp = sc; *pp; pp++) {
        if (*pp < '0' || *pp > '9') break;
        v = v * 10 + (*pp - '0'); any = 1;
        if (v > 100000) break;   // absurd: MAX_USER_WINDOWS is tiny
    }
    return any ? v : -1;
}

// NOT claiming: fb_owner_is reads the latch and never sets it (the capgate.rs
// note explains why claiming here would be a privilege escalation). 0 while
// unclaimed: deny.
static int caller_is_compositor(void)
{
    process_t *p = proc_current();
    if (!p) return 0;
    return fb_owner_is(p->pid);
}

// Run the consent watchdog against the live request. Called from every
// observation path so a crashed requester can never leave a scrim open.
static void cap_watchdog(void)
{
    uint32_t owner = cap_req_owner_pid_rs();
    if (!owner) return;
    uint32_t alive = proc_get(owner) ? 1u : 0u;
    if (cap_req_tick_rs(sched_now_ms(), alive))
        (void)bootlog_write("[CAP] request closed by watchdog (pid=%u alive=%u)",
                            (unsigned)owner, (unsigned)alive);
}

// ---------------------------------------------------------------------------
// THE DISPATCHER CHOKEPOINT GATE (design section 5.3). One call in
// syscall_dispatch_inner(), after syscall_validate_args(), before the switch.
// CLASS level only: does the caller hold the required capability at all. The
// fine-grained scope match + use consumption happens in the gated handler
// (caps_current_covers_path), where the path argument is already bounced.
// ---------------------------------------------------------------------------
int64_t syscall_cap_check(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg1; (void)arg2; (void)arg3;
    uint32_t need = cap_required_for_syscall(num);
    if (need == CAP_NONE) return 0;   // not a gated syscall: the common path

    process_t *p = proc_current();
    if (!p) return CAP_EDENIED;       // no current process: refuse a gated syscall

    uint64_t now = sched_now_ms();
    int idx = cap_find_live(p->cap_grants, CAP_MAX_GRANTS, need, now);
    if (idx >= 0) {
        // A grant of the class exists. Let the handler do the scope check and
        // note ALLOWED at the actual use, so the ledger's allowed count tracks
        // real uses rather than class-level entries.
        return 0;
    }

    // Refused: no grant of the required class. Durable, rate-limited (a gated
    // syscall in a hot loop must not fill the log; the ledger counts them all).
    cap_ledger_note(CAP_LEDGER_REFUSED);
    static int refuse_reports = 0;
    if (refuse_reports < 64) {
        refuse_reports++;
        char d[96];
        snprintf(d, sizeof(d), "REFUSED %s: no grant (syscall %u)",
                 cap_name(need), (unsigned)num);
        seclog_report_capability((unsigned)p->pid, d);
    }
    return CAP_EDENIED;
}

// ---------------------------------------------------------------------------
// Scope-level check for a gated handler. Does the CURRENT process hold a live
// grant of `cap` whose scope covers `path`, and consume one use if so?
// ---------------------------------------------------------------------------
int caps_current_covers_path(uint32_t cap, const char *path)
{
    process_t *p = proc_current();
    if (!p || !path) return 0;
    uint64_t now = sched_now_ms();
    int idx = cap_covers_path(p->cap_grants, CAP_MAX_GRANTS, cap, path, now);
    if (idx < 0) {
        cap_ledger_note(CAP_LEDGER_REFUSED);
        char d[128];
        snprintf(d, sizeof(d), "REFUSED %s: grant does not cover %s",
                 cap_name(cap), path);
        seclog_report_capability((unsigned)p->pid, d);
        return 0;
    }
    (void)cap_consume_use(p->cap_grants, CAP_MAX_GRANTS, (uint32_t)idx);
    cap_ledger_note(CAP_LEDGER_ALLOWED);
    return 1;
}

// ---------------------------------------------------------------------------
// Serial twin of caps_current_covers_path. Does the CURRENT process hold a live
// serial.port grant naming EXACTLY `port`, consuming one use if so? Called by
// the gated SYS_SERIAL_OPEN handler once the class-level chokepoint has passed.
// ---------------------------------------------------------------------------
int caps_current_covers_port(uint32_t cap, const char *port)
{
    process_t *p = proc_current();
    if (!p || !port) return 0;
    uint64_t now = sched_now_ms();
    int idx = cap_covers_port(p->cap_grants, CAP_MAX_GRANTS, cap, port, now);
    if (idx < 0) {
        cap_ledger_note(CAP_LEDGER_REFUSED);
        char d[128];
        snprintf(d, sizeof(d), "REFUSED %s: grant does not cover port %s",
                 cap_name(cap), port);
        seclog_report_capability((unsigned)p->pid, d);
        return 0;
    }
    (void)cap_consume_use(p->cap_grants, CAP_MAX_GRANTS, (uint32_t)idx);
    cap_ledger_note(CAP_LEDGER_ALLOWED);
    return 1;
}

// ---------------------------------------------------------------------------
// input.inject authorization (Stage 3 self + Stage 4 cross-app target). The
// per-window liveness and compositor-surface exclusion are done by the
// SYS_CAP_INJECT_* handlers (they know the live window table); this decides
// WHICH grant authorizes the inject and consumes exactly one of its uses.
//
//   owns_target != 0  -> the caller OWNS the target window: a self
//                        (CAP_SCOPE_WINDOW) grant authorizes it.
//   otherwise         -> a CAP_SCOPE_WINDOW_TARGET grant whose bound stable id
//                        EXACTLY equals target_id authorizes it.
//
// The two are told apart by scope kind (cap_find_scope), so a target-only grant
// can NEVER authorize a self-inject, nor a self grant a cross-app one. Refusals
// are precise: CAP_ESCOPE if an input.inject grant is held but none covers this
// window, CAP_EDENIED if none is held. A use is consumed only on the 0 path.
// ---------------------------------------------------------------------------
int64_t caps_inject_authorize(int owns_target, uint64_t target_id)
{
    process_t *p = proc_current();
    if (!p) return CAP_EDENIED;
    uint64_t now = sched_now_ms();

    if (owns_target) {
        int si = cap_find_scope(p->cap_grants, CAP_MAX_GRANTS, CAP_INPUT_INJECT,
                                CAP_SCOPE_WINDOW, now);
        if (si >= 0) {
            (void)cap_consume_use(p->cap_grants, CAP_MAX_GRANTS, (uint32_t)si);
            cap_ledger_note(CAP_LEDGER_ALLOWED);
            return 0;
        }
    }
    int ti = cap_covers_window_target(p->cap_grants, CAP_MAX_GRANTS, CAP_INPUT_INJECT,
                                      target_id, now);
    if (ti >= 0) {
        (void)cap_consume_use(p->cap_grants, CAP_MAX_GRANTS, (uint32_t)ti);
        cap_ledger_note(CAP_LEDGER_ALLOWED);
        return 0;
    }
    cap_ledger_note(CAP_LEDGER_REFUSED);
    int any = cap_find_live(p->cap_grants, CAP_MAX_GRANTS, CAP_INPUT_INJECT, now);
    if (any < 0) {
        seclog_report_capability((unsigned)p->pid, "REFUSED input.inject: no grant");
        return CAP_EDENIED;
    }
    seclog_report_capability((unsigned)p->pid,
                             "REFUSED input.inject: grant does not cover this window");
    return CAP_ESCOPE;
}

// ---------------------------------------------------------------------------
// SYS_CAP_QUERY. What do I hold, and until when.
// ---------------------------------------------------------------------------
int64_t sys_cap_query(uint32_t cap, cap_state_t *u_out)
{
    process_t *p = proc_current();
    if (!p) return CAP_EARG;
    cap_state_t st;
    memset(&st, 0, sizeof(st));
    cap_query(p->cap_grants, CAP_MAX_GRANTS, cap, sched_now_ms(), &st);
    if (!u_out) return CAP_EARG;
    if (copy_to_user(u_out, &st, sizeof(st)) != 0) return CAP_EARG;
    return 0;
}

// ---------------------------------------------------------------------------
// SYS_CAP_REQUEST. Ask for a capability; may raise a consent prompt. Refusals
// are distinct and named (design section 8.1). Order matters: cheapest and
// least informative first.
// ---------------------------------------------------------------------------

// ===========================================================================
// #capalways: STANDING CONSENT ("Always allow")
// ---------------------------------------------------------------------------
// See rustkern/caps.rs for the rules and the identity argument. Here: only the
// file. /CONFIG/CAPALLOW.CFG is written with fat_write_file, the same call
// cron.c uses for /CONFIG/CRON.CFG (it routes to the ext2 root; vfs_write_file
// is NETFS-only, which is a trap this tree has hit before).
// g_fat_fs is DEFINED in main.c; every user declares its own extern (the
// convention in fs/netfs.c, fs/devlog.c, fs/fat_vfs.c...). fat.h does not
// declare it, which the syntax check caught.
extern fat_fs_t g_fat_fs;

// Defined below, next to sys_cap_resolve; sys_cap_request (above it) calls it.
static int64_t cap_issue_approved(const cap_approve_info_t *pinfo);

#define CAPALLOW_PATH "/CONFIG/CAPALLOW.CFG"
#define CAPALLOW_BUF  4096

static int g_always_loaded = 0;

void cap_always_load(void)
{
    // Mirrors cron_load(): fat_read_file ALLOCATES and returns the buffer, so
    // the caller owns it and must kfree it on every path.
    g_always_loaded = 1;
    cap_always_reset_rs();
    if (!g_fat_fs.mounted) {
        g_always_loaded = 0;   // retry once the root is actually mounted
        return;
    }
    uint32_t sz = 0;
    char *data = (char *)fat_read_file(&g_fat_fs, CAPALLOW_PATH, &sz);
    if (!data || sz == 0) {
        if (data) kfree(data);
        return;
    }
    int n = cap_always_parse_rs(data, sz);
    kfree(data);
    if (n > 0)
        (void)bootlog_write("[CAP] standing consents loaded: %d from " CAPALLOW_PATH, n);
}

static void cap_always_save(void)
{
    static char out[CAPALLOW_BUF];
    int n = cap_always_serialize_rs(out, sizeof(out));
    if (n <= 0) {
        (void)bootlog_write("[CAP] standing consent NOT saved: serialize failed (%d)", n);
        return;
    }
    int rc = fat_write_file(&g_fat_fs, CAPALLOW_PATH, out, (uint32_t)n);
    if (rc != 0)
        (void)bootlog_write("[CAP] FAILED to write " CAPALLOW_PATH " (rc=%d): the consent "
                            "holds for this boot only", rc);
}

// Record an approved request as standing. Never fatal: if it cannot be stored
// the user still got the one-off grant they approved.
static void cap_always_remember(const cap_approve_info_t *info)
{
    process_t *rp = proc_get(info->req_pid);
    const char *app = rp ? rp->name : "";
    int64_t r = cap_always_add_rs(info->req_uid, info->cap, info->scope_kind,
                                  app, info->scope);
    if (r < 0) {
        (void)bootlog_write("[CAP] standing consent REFUSED (rc=%lld) app=%s %s scope=%s",
                            (long long)r, app, cap_name(info->cap), info->scope);
        return;
    }
    if (r == 1) {
        char d[160];
        snprintf(d, sizeof(d), "ALWAYS-ALLOW stored: %s scope=%s app=%s uid=%u",
                 cap_name(info->cap), info->scope, app, (unsigned)info->req_uid);
        seclog_report_capability((unsigned)info->req_pid, d);
        (void)bootlog_write("[CAP] %s", d);
        cap_always_save();
    }
}

int64_t sys_cap_request(const cap_req_t *u_req)
{
    process_t *p = proc_current();
    if (!p) return CAP_EARG;
    if (p->privilege != PRIV_USER) return CAP_EARG;  // Ring 0 has no business here

    cap_req_t req;
    memset(&req, 0, sizeof(req));
    if (!u_req) return CAP_EARG;
    if (copy_from_user(&req, u_req, sizeof(req)) != 0) return CAP_EARG;
    req.reason[sizeof(req.reason) - 1] = 0;
    req.scope[sizeof(req.scope) - 1] = 0;

    // Only issuable capabilities can be requested. The others are DEFINED so
    // enumeration is honest, but refused with EPOLICY until a first-party
    // consumer exists (principle 7). This is CT_DENIED, not silence.
    if (!cap_is_issuable(req.cap)) {
        cap_ledger_note(CAP_LEDGER_DENIED);
        char d[96];
        snprintf(d, sizeof(d), "POLICY refused %s (not issuable in stage 1)",
                 cap_name(req.cap));
        seclog_report_capability((unsigned)p->pid, d);
        return CAP_EPOLICY;
    }

    // The scope is validated against the KERNEL's terms before it means
    // anything. A "." / ".." / relative / non-plain path is refused, not
    // normalised, so a grant can never be talked into covering /CONFIG/SHADOW.
    if (!cap_scope_valid(req.cap, req.scope_kind, req.scope))
        return CAP_ESCOPE;

    // input.inject WINDOW_TARGET (Stage 4): the app names WHICH window to drive
    // by its HANDLE (a bare decimal, shape-checked above). Resolve it HERE, from
    // the kernel's own window registry, to the window's STABLE id + title, and
    // REWRITE the scope to the bound "<winid>:<title>" form. The app supplies
    // only the handle: it does not author the id or the title, so it cannot make
    // the consent name one window while a different one is driven, and it cannot
    // name the compositor's own surface (the resolver refuses a compositor-owned
    // window). A handle that is not a live targetable user window is refused
    // (CAP_ESCOPE) before any prompt is raised.
    if (req.cap == CAP_INPUT_INJECT && req.scope_kind == CAP_SCOPE_WINDOW_TARGET) {
        int handle = cap_parse_handle(req.scope);
        uint64_t wid = 0;
        char title[40];
        if (handle < 0 ||
            userwin_target_resolve(handle, &wid, title, sizeof(title)) != 0) {
            cap_ledger_note(CAP_LEDGER_DENIED);
            seclog_report_capability((unsigned)p->pid,
                "POLICY refused input.inject WINDOW_TARGET: not a live targetable window");
            (void)bootlog_write("[CAP] REFUSED input.inject WINDOW_TARGET: pid=%u handle=%d not targetable",
                                (unsigned)p->pid, handle);
            return CAP_ESCOPE;
        }
        snprintf(req.scope, sizeof(req.scope), "%llu:%s",
                 (unsigned long long)wid, title);
        req.scope[sizeof(req.scope) - 1] = 0;
    }

    // serial.port: the scope must name a port the kernel actually publishes. The
    // scope SHAPE was checked above; this is the kernel-owned ENUMERATION check
    // (design 4.3), so a prompt can never be raised for a port that is not there
    // and a grant can never name a device that does not exist.
    if (req.cap == CAP_SERIAL_PORT && !serialport_is_published(req.scope)) {
        cap_ledger_note(CAP_LEDGER_DENIED);
        seclog_report_capability((unsigned)p->pid,
                                 "POLICY refused serial.port: no such published port");
        return CAP_ESCOPE;
    }

    // Consent needs a principal that can safely draw the prompt. Before the
    // compositor latch there is none: deny, do not queue and do not auto-grant
    // (principle 5, design section 6.3). is_service before-login policy grants
    // are Stage 5 and deliberately absent here.
    if (fbown_owner_rs() == 0)
        return CAP_ENOCONSENT;

    // A prompt may only follow real input to a window this process owns. Sound
    // here because Stage 0 gated SYS_INJECT_KEY, so an ordinary app can no
    // longer manufacture the input credit.
    uint64_t now = sched_now_ms();

    // #capalways: a STANDING consent the user previously chose "Always allow"
    // for. Checked before the input-credit requirement, because that credit
    // exists only to stop an app raising a prompt nobody asked for, and here no
    // prompt is raised. Deliberately placed AFTER the compositor-latch check
    // above so this does not quietly become a before-login grant path (that is
    // Stage 5 and stays absent). The issued grant is still time-bounded.
    if (!g_always_loaded) cap_always_load();
    if (cap_always_match_rs(p->euid, req.cap, req.scope_kind, p->name, req.scope) == 1) {
        uint32_t elev_open_a = elev_owner_pid_rs() ? 1u : 0u;
        int64_t aseq = cap_req_autogrant_rs(p->pid, p->euid, now, req.cap, req.duration_ms,
                                            req.scope_kind, req.reason, req.scope,
                                            p->name, elev_open_a);
        if (aseq > 0) {
            cap_approve_info_t ai;
            memset(&ai, 0, sizeof(ai));
            ai.req_pid = p->pid;
            ai.req_uid = p->euid;
            ai.cap = req.cap;
            ai.duration_ms = req.duration_ms;
            ai.scope_kind = req.scope_kind;
            strncpy(ai.scope, req.scope, sizeof(ai.scope) - 1);
            if (cap_issue_approved(&ai) == 0) {
                (void)bootlog_write("[CAP] AUTO-GRANT (always-allow): pid=%u uid=%u %s scope=%s",
                                    (unsigned)p->pid, (unsigned)p->euid,
                                    cap_name(req.cap), req.scope);
                return aseq;
            }
        }
        // fall through to the normal prompt if the auto-grant could not be made
    }

    if (p->elev_last_input_ms == 0 ||
        now - p->elev_last_input_ms > CAP_INPUT_WINDOW_MS) {
        cap_ledger_note(CAP_LEDGER_DENIED);
        (void)bootlog_write("[CAP] REFUSED spontaneous request: pid=%u %s (no input in %llums)",
                            (unsigned)p->pid, cap_name(req.cap),
                            (unsigned long long)CAP_INPUT_WINDOW_MS);
        return CAP_ENOINPUT;
    }

    cap_watchdog();

    // ONE prompt at a time, system-wide: cap_req_open_rs refuses if its own
    // slot is open OR an elevation prompt is open (the cross-exclusion).
    uint32_t elev_open = elev_owner_pid_rs() ? 1u : 0u;
    int64_t seq = cap_req_open_rs(p->pid, p->euid, now, req.cap, req.duration_ms,
                                  req.scope_kind, req.reason, req.scope, p->name,
                                  elev_open);
    if (seq > 0) {
        cap_ledger_note(CAP_LEDGER_REQUESTED);
        char d[160];
        snprintf(d, sizeof(d), "request raised: %s scope=%s dur=%ums by '%s'",
                 cap_name(req.cap), req.scope, (unsigned)req.duration_ms, p->name);
        seclog_report_capability((unsigned)p->pid, d);
        (void)bootlog_write("[CAP] prompt raised: pid=%u uid=%u %s scope=%s seq=%llu",
                            (unsigned)p->pid, (unsigned)p->euid, cap_name(req.cap),
                            req.scope, (unsigned long long)seq);
    }
    return seq;
}

// The requester polls its OWN request. A stale seq returns CAP_ESTALE.
int64_t sys_cap_status(uint64_t seq)
{
    cap_watchdog();
    int st = cap_req_state_rs(seq);
    if (st == (int)CAP_ST_GRANTED || st == (int)CAP_ST_DENIED) {
        cap_req_reap_rs(seq);   // verdict read: drop the record for the next request
    }
    return st;
}

// COMPOSITOR ONLY. The facts the trusted surface draws from.
int64_t sys_cap_view(cap_view_t *u_out)
{
    if (!caller_is_compositor()) return CAP_EPERM;
    cap_watchdog();
    cap_view_t v;
    memset(&v, 0, sizeof(v));
    if (!cap_req_view_rs(&v)) return 0;
    if (!u_out) return CAP_EARG;
    if (copy_to_user(u_out, &v, sizeof(v)) != 0) return CAP_EARG;
    return 1;
}

// COMPOSITOR ONLY. Approve or deny. On approve the grant is issued on the
// REQUESTER'S process_t (never the compositor's) and recorded as a
// GFSJ_OP_EDGE_ADD edge whose seq becomes the grant's granted_seq.
// #capalways: the grant-issuing tail, shared by the compositor's resolve and
// by the standing-consent auto-grant, so there is exactly ONE place that turns
// an approved request into a grant + journal edge + audit line.
static int64_t cap_issue_approved(const cap_approve_info_t *pinfo)
{
    cap_approve_info_t info = *pinfo;
    process_t *rp = proc_get(info.req_pid);
    if (!rp) return CAP_ESTALE;

    uint64_t now = sched_now_ms();
    uint64_t expires;
    uint32_t uses;
    if (info.duration_ms == 0) {
        expires = now + CAP_ONEUSE_TTL_MS;
        uses = 1;                       // one use, consumed on first covered use
    } else {
        expires = now + (uint64_t)info.duration_ms;
        uses = CAP_USES_UNLIMITED;      // unlimited WITHIN the bounded window
    }

    // Issue first (so a table-full is caught before we write a journal edge
    // for a grant that did not happen), then journal, then stamp the seq.
    int slot = cap_issue(rp->cap_grants, CAP_MAX_GRANTS, info.cap, info.scope_kind,
                         info.scope, expires, uses, 0);
    if (slot < 0) {
        cap_ledger_note(CAP_LEDGER_DENIED);
        (void)bootlog_write("[CAP] grant refused: pid=%u table full", (unsigned)info.req_pid);
        seclog_report_capability((unsigned)info.req_pid, "grant refused: table full");
        return CAP_EMAX;
    }

    uint64_t jseq = 0;
    if (gfs_journal_ready()) {
        char payload[96];
        int pl = snprintf(payload, sizeof(payload), "GRANT %s uid=%u pid=%u dur=%ums %s",
                          cap_name(info.cap), (unsigned)info.req_uid,
                          (unsigned)info.req_pid, (unsigned)info.duration_ms, info.scope);
        if (pl > 0)
            (void)gfs_journal_append_seq(GFSJ_ACTOR_PID, info.req_pid,
                                         GFSJ_OP_EDGE_ADD, GFSJ_EFFECT_COMPENSATABLE,
                                         payload, (uint32_t)pl, &jseq);
    }
    rp->cap_grants[slot].granted_seq = jseq;

    cap_ledger_note(CAP_LEDGER_GRANTED);
    char d[160];
    snprintf(d, sizeof(d), "GRANTED %s scope=%s dur=%ums seq=%llu",
             cap_name(info.cap), info.scope, (unsigned)info.duration_ms,
             (unsigned long long)jseq);
    seclog_report_capability((unsigned)info.req_pid, d);
    (void)bootlog_write("[CAP] GRANTED: pid=%u uid=%u %s scope=%s expires=%llums edge=%llu",
                        (unsigned)info.req_pid, (unsigned)info.req_uid,
                        cap_name(info.cap), info.scope,
                        (unsigned long long)expires, (unsigned long long)jseq);
    return 0;
}

int64_t sys_cap_resolve(uint64_t seq, int action)
{
    if (!caller_is_compositor()) return CAP_EPERM;
    cap_watchdog();

    uint32_t rpid = cap_req_owner_pid_rs();
    if (!rpid) return CAP_ESTALE;

    if (action == CAP_ACT_DENY) {
        int r = cap_req_resolve_rs(seq, 0);
        if (r == 0) {
            cap_ledger_note(CAP_LEDGER_DENIED);
            (void)bootlog_write("[CAP] DENIED by user: pid=%u", (unsigned)rpid);
            seclog_report_capability((unsigned)rpid, "DENIED by user");
        }
        return r;
    }
    if (action != CAP_ACT_APPROVE && action != CAP_ACT_APPROVE_ALWAYS)
        return CAP_EARG;

    cap_approve_info_t info;
    memset(&info, 0, sizeof(info));
    if (!cap_req_approve_info_rs(&info)) return CAP_ESTALE;

    // Mark the request granted (closes it, so a second approve is stale).
    if (cap_req_resolve_rs(seq, 1) != 0) return CAP_ESTALE;

    int64_t rc = cap_issue_approved(&info);
    if (rc == 0 && action == CAP_ACT_APPROVE_ALWAYS)
        cap_always_remember(&info);
    return rc;
}

// SYS_CAP_REVOKE. A process drops its own grant of `cap`. The Settings manager
// that revokes OTHER processes' grants is Stage 2; this self-revoke is enough
// to prove revocation bites in flight. Clears the field, so the holder's next
// gated syscall is refused, and records a GFSJ_OP_EDGE_REVOKE naming the edge.
int64_t sys_cap_revoke(uint32_t cap)
{
    process_t *p = proc_current();
    if (!p) return CAP_EARG;
    uint64_t revoked_seq = cap_revoke(p->cap_grants, CAP_MAX_GRANTS, cap);
    if (revoked_seq == 0 && cap_find_live(p->cap_grants, CAP_MAX_GRANTS, cap, sched_now_ms()) < 0) {
        // Nothing was held (revoked_seq 0 could also be a grant made before the
        // journal was up; treat "no live grant now" as the signal it is gone).
    }
    cap_ledger_note(CAP_LEDGER_REVOKED);
    if (gfs_journal_ready()) {
        char payload[64];
        int pl = snprintf(payload, sizeof(payload), "REVOKE %s pid=%u edge=%llu",
                          cap_name(cap), (unsigned)p->pid, (unsigned long long)revoked_seq);
        if (pl > 0)
            (void)gfs_journal_append(GFSJ_ACTOR_PID, p->pid, GFSJ_OP_EDGE_REVOKE,
                                     GFSJ_EFFECT_REVERSIBLE, payload, (uint32_t)pl);
    }
    char d[96];
    snprintf(d, sizeof(d), "REVOKED %s (edge=%llu)", cap_name(cap),
             (unsigned long long)revoked_seq);
    seclog_report_capability((unsigned)p->pid, d);
    (void)bootlog_write("[CAP] REVOKED: pid=%u %s edge=%llu", (unsigned)p->pid,
                        cap_name(cap), (unsigned long long)revoked_seq);
    return 0;
}

// ---------------------------------------------------------------------------
// Boot self-test + observability.
// ---------------------------------------------------------------------------
void caps_selftest(void)
{
    uint32_t r = caps_selftest_rs();
    if (r == 0) {
        kprintf("[CAP] selftest OK (requirement table, policy, scope validation,"
                " grant issue/find/cover/consume/revoke, table-full, consent SM)\n");
    } else {
        kprintf("[CAP] SELFTEST FAILED case %u\n", (unsigned)r);
        (void)bootlog_write("[CAP] SELFTEST FAILED case %u", (unsigned)r);
    }
}

void caps_report(void)
{
    cap_ledger_t l;
    if (cap_ledger_get(&l) != 0) return;
    char b[192];
    snprintf(b, sizeof(b),
             "[CAP] use=%u/%u (refused/allowed) req=%u grant=%u deny=%u revoke=%u",
             (unsigned)l.c[CAP_LEDGER_REFUSED], (unsigned)l.c[CAP_LEDGER_ALLOWED],
             (unsigned)l.c[CAP_LEDGER_REQUESTED], (unsigned)l.c[CAP_LEDGER_GRANTED],
             (unsigned)l.c[CAP_LEDGER_DENIED], (unsigned)l.c[CAP_LEDGER_REVOKED]);
    kprintf("%s\n", b);
    bootlog_heartbeat_note(b);
}
