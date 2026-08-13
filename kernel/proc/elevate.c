// proc/elevate.c - #745 elevation glue. See proc/elevate.h for the trust story.
//
// C AND NOT RUST, with the justification the 2026-07-16 rule asks for: every
// line below is FFI plumbing that has to touch C-only surfaces (process_t,
// copy_from_user/sc_bounce_str, the users.c authenticator chokepoint,
// kprintf/bootlog). The DECISIONS - one at a time, three attempts, what counts
// as a covered path, what a sanitised package name looks like, when the
// watchdog fires - are all in rustkern/elevate.rs and none of them is repeated
// here.
#include "elevate.h"
#include "process.h"
#include "syscall.h"
#include "users.h"
#include "../security/seclog.h"
#include "../serial.h"
#include "../string.h"
// The OWNING header, not a private extern: a hand-written `extern void
// (void)bootlog_write(...)` opts this whole file out of bootlog.h's MUST_CHECK and
// links against a signature this file made up. persist-extern-gate fails the
// build for exactly that, and it fired on the first build of this file.
#include "../fs/bootlog.h"

extern uint64_t sched_now_ms(void);
extern process_t *proc_get(uint32_t pid);
extern int fb_owner_is(uint32_t pid);   // gui/fbown.h: the framebuffer latch
extern int copy_from_user(void *dst, const void *usrc, unsigned long n);
extern int copy_to_user(void *udst, const void *src, unsigned long n);
extern uint32_t first_admin_uid_rs(void);

// GRANT_TTL_MS / INPUT_WINDOW_MS live in rustkern/elevate.rs. They are
// repeated here as the two values C needs to apply them, and the boot
// self-test in elevate.rs does not cover the duplication, so they are written
// once each and used from one place each.
#define ELEV_GRANT_TTL_MS  180000ULL
#define ELEV_INPUT_WINDOW_MS 10000ULL

// ---------------------------------------------------------------------------
// The compositor gate.
//
// NOT a uid check. The compositor's uid is not the point and could change; what
// makes it the trusted drawing surface is that it is THE process that owns the
// framebuffer. An app that tries to map the framebuffer to become the
// compositor is rejected there, so it cannot become the principal this function
// admits.
//
// #745 task #59 CHANGED WHAT THAT SENTENCE MEANS, so re-read it rather than
// trusting the old one. The latch used to be "first caller wins, never
// reassigned while the boot lives", which was ALSO why Switch User and Log Out
// broke the desktop for the rest of the boot. It is now an explicitly ARMED
// single-owner claim (rustkern/fbown.rs): the kernel opens the claim window
// immediately before it launches /APPS/COMPOSIT and narrows it to that pid, the
// claim closes the window, and proc_exit() releases and DISARMS it. So the
// principal admitted here is strictly narrower than before, not wider: between
// one compositor exiting and the next being launched, fb_owner_is() answers 0
// for everybody, and no Ring 3 process can make it answer otherwise.
// ---------------------------------------------------------------------------
static int caller_is_compositor(void)
{
    process_t *p = proc_current();
    if (!p) return 0;
    return fb_owner_is(p->pid);   // 0 while unclaimed: deny
}

// Run the watchdog against the live request. Called from every observation
// path, so a crashed requester can never leave the compositor holding a grab
// and a scrim over an unusable desktop.
// The audit answers "who installed this for everyone, and when". A pid does
// not answer that: it is reused and it names nobody. Every record carries the
// uid AND the account name.
static void elev_actor(char *out, unsigned long cap, uint32_t uid)
{
    out[0] = 0;
    user_entry_t *u = user_lookup_uid(uid);
    strncat(out, "uid=", cap - 1);
    char n[12]; int i = 0, t = 0; char tmp[12];
    uint32_t v = uid;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < 11) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t > 0) n[i++] = tmp[--t];
    n[i] = 0;
    strncat(out, n, cap - strlen(out) - 1);
    strncat(out, " user=", cap - strlen(out) - 1);
    strncat(out, u ? u->username : "?", cap - strlen(out) - 1);
}

static void elev_watchdog(void)
{
    uint32_t owner = elev_owner_pid_rs();
    if (!owner) return;
    uint32_t alive = proc_get(owner) ? 1u : 0u;
    if (elev_tick_rs(sched_now_ms(), alive))
        (void)bootlog_write("[ELEV] request closed by watchdog (pid=%u alive=%u)",
                      (unsigned)owner, (unsigned)alive);
}

// ---------------------------------------------------------------------------
// SYS_ELEV_REQUEST. The app side. Five refusals, in this order, and the order
// matters: the cheapest and least informative refusals come first.
// ---------------------------------------------------------------------------
int64_t sys_elev_request(const elev_request_t *u_req)
{
    process_t *p = proc_current();
    if (!p) return ELEV_EARG;
    if (p->privilege != PRIV_USER) return ELEV_EARG;   // Ring 0 has no business here

    // ROOT IS NEVER PROMPTED. root already IS the privilege; a prompt here
    // would be the most-fired and least-meaningful dialog in the OS, and on the
    // shipping golden (autologin root) it would fire on every install, which is
    // exactly how a click-through habit gets built.
    if (p->euid == 0) return ELEV_EROOT;

    // Authorisation is decided BEFORE anything is drawn. A user who is not in
    // the admin set never sees a password field at all (Surface C).
    if (!users_may_elevate(p->euid)) return ELEV_EPERM;

    // #745 REQUIREMENT 8: a prompt may only be raised in RESPONSE to input the
    // window manager actually delivered to a window this process owns. Without
    // it an app can raise a prompt spontaneously, and "a prompt is rare, so a
    // prompt is meaningful" - the strongest anti-phishing property this design
    // has - stops being true. The stamp is written in ONE place, at the WM's
    // own delivery chokepoint (user_window_queue_event in syscall.c), and only
    // for key-down / button-down / button-up. Pointer MOTION deliberately does
    // not count: a cursor resting over a window would otherwise keep that
    // window permanently "recently active" with no user intent at all.
    uint64_t now = sched_now_ms();
    if (p->elev_last_input_ms == 0 ||
        now - p->elev_last_input_ms > ELEV_INPUT_WINDOW_MS) {
        (void)bootlog_write("[ELEV] REFUSED spontaneous prompt: pid=%u uid=%u (no dispatched input in %llums)",
                      (unsigned)p->pid, (unsigned)p->euid,
                      (unsigned long long)ELEV_INPUT_WINDOW_MS);
        return ELEV_ENOINPUT;
    }

    elev_watchdog();

    elev_request_t req;
    memset(&req, 0, sizeof(req));
    if (!u_req) return ELEV_EARG;
    if (copy_from_user(&req, u_req, sizeof(req)) != 0) return ELEV_EARG;
    req.name[sizeof(req.name) - 1] = 0;
    req.version[sizeof(req.version) - 1] = 0;
    req.source[sizeof(req.source) - 1] = 0;

    // The destination is a KERNEL constant, never the caller's. What the app
    // says about its package is display data; where the privilege applies is
    // not negotiable.
    int64_t seq = elev_open_rs(p->pid, p->euid, now,
                               req.name, req.version, req.source,
                               ELEV_SYS_APPS_DIR);
    if (seq > 0) {
        char d[128];
        elev_actor(d, sizeof(d), p->euid);
        strncat(d, " raise: pkg=", sizeof(d) - strlen(d) - 1);
        strncat(d, req.name, sizeof(d) - strlen(d) - 1);
        seclog_report_elevation((unsigned)p->pid, d);
        (void)bootlog_write("[ELEV] prompt raised: pid=%u uid=%u pkg='%s' seq=%llu",
                      (unsigned)p->pid, (unsigned)p->euid, req.name,
                      (unsigned long long)seq);
    }
    return seq;
}

// The app polls its OWN request. A seq that is not live returns ELEV_ESTALE, so
// a verdict can never be misread as belonging to a different request.
int64_t sys_elev_status(uint64_t seq)
{
    elev_watchdog();
    int st = elev_state_rs(seq);
    if (st == ELEV_ST_GRANTED || st == ELEV_ST_DENIED) {
        // The requester has now seen its verdict; drop the record so the next
        // request is not refused by a corpse. Only the owning seq can do this.
        elev_reap_rs(seq);
    }
    return st;
}

// COMPOSITOR ONLY. Facts, never geometry, never a username.
int64_t sys_elev_view(elev_view_t *u_out)
{
    if (!caller_is_compositor()) return ELEV_EPERM;
    elev_watchdog();
    elev_view_t v;
    memset(&v, 0, sizeof(v));
    if (!elev_view_rs(&v)) return 0;
    if (!u_out) return ELEV_EARG;
    if (copy_to_user(u_out, &v, sizeof(v)) != 0) return ELEV_EARG;
    return 1;
}

// COMPOSITOR ONLY. The password enters the kernel HERE and nowhere else in this
// mechanism; it is bounced into a kernel buffer, handed straight to the
// elevation-scoped authenticator, and zeroed on the way out.
int64_t sys_elev_resolve(uint64_t seq, int action, const char k_pw[ELEV_PASSWORD_MAX])
{
    if (!caller_is_compositor()) return ELEV_EPERM;
    elev_watchdog();

    uint32_t uid = elev_owner_uid_rs();
    uint32_t rpid = elev_owner_pid_rs();
    if (!rpid || uid == 0xFFFFFFFFu) return ELEV_ESTALE;

    // The kernel resolves the account. The compositor cannot name one, so this
    // path can never be turned into an oracle for an account other than the one
    // that actually raised the request.
    user_entry_t *u = user_lookup_uid(uid);
    if (!u) return ELEV_ESTALE;

    if (action == ELEV_ACT_CANCEL) {
        int r = elev_cancel_rs(seq);
        if (r == 0) {
            char d[128]; elev_actor(d, sizeof(d), uid);
            strncat(d, " cancelled", sizeof(d) - strlen(d) - 1);
            seclog_report_elevation((unsigned)rpid, d);
            (void)bootlog_write("[ELEV] cancelled by user: pid=%u uid=%u", (unsigned)rpid, (unsigned)uid);
        }
        return r;
    }
    if (action == ELEV_ACT_LOCKSECS)
        return users_elev_lockout(u->username);
    if (action != ELEV_ACT_SUBMIT) return ELEV_EARG;

    if (!k_pw || !k_pw[0]) return ELEV_EARG;
    char pw[ELEV_PASSWORD_MAX];
    memcpy(pw, k_pw, sizeof(pw));
    pw[sizeof(pw) - 1] = 0;

    // The ELEVATION-scoped authenticator. It shares the password check and
    // nothing else with the login gate: see the block comment in users.c for
    // why an elevation failure must never touch shadow_entry_t.failed_attempts.
    int ar = users_authenticate_elev(u->username, pw);
    memset(pw, 0, sizeof(pw));

    if (ar == -2) {
        (void)bootlog_write("[ELEV] locked out: uid=%u (%d s)", (unsigned)uid,
                      users_elev_lockout(u->username));
        return ELEV_ELOCKED;
    }
    if (ar != 0) {
        int left = elev_attempt_rs(seq, 0);
        char d[128]; elev_actor(d, sizeof(d), uid);
        strncat(d, " wrong password", sizeof(d) - strlen(d) - 1);
        seclog_report_elevation((unsigned)rpid, d);
        (void)bootlog_write("[ELEV] wrong password: uid=%u attempts_left=%d", (unsigned)uid, left);
        if (left <= 0) return ELEV_EATTEMPTS;
        return left;
    }

    // AUTHENTICATED. Issue the grant on the REQUESTER, not on the compositor,
    // and make it as narrow as it can be: one process, one path prefix, one
    // bounded window. Ring 3 has no syscall that writes these fields.
    if (elev_attempt_rs(seq, 1) != 0) return ELEV_ESTALE;
    process_t *rp = proc_get(rpid);
    if (!rp) return ELEV_ESTALE;
    rp->elev_grant_until_ms = sched_now_ms() + ELEV_GRANT_TTL_MS;
    strncpy(rp->elev_grant_prefix[0], ELEV_SYS_APPS_DIR, sizeof(rp->elev_grant_prefix[0]) - 1);
    rp->elev_grant_prefix[0][sizeof(rp->elev_grant_prefix[0]) - 1] = 0;
    strncpy(rp->elev_grant_prefix[1], ELEV_SYS_MENU_DIR, sizeof(rp->elev_grant_prefix[1]) - 1);
    rp->elev_grant_prefix[1][sizeof(rp->elev_grant_prefix[1]) - 1] = 0;

    { char d[128]; elev_actor(d, sizeof(d), uid);
      strncat(d, " GRANTED system-wide install", sizeof(d) - strlen(d) - 1);
      seclog_report_elevation((unsigned)rpid, d); }
    (void)bootlog_write("[ELEV] GRANTED: pid=%u uid=%u user='%s' prefix=%s ttl=%llums",
                  (unsigned)rpid, (unsigned)uid, u->username,
                  ELEV_SYS_APPS_DIR, (unsigned long long)ELEV_GRANT_TTL_MS);
    return 0;
}

// May the CALLER's account elevate at all? One boolean, asked once, so the App
// Store can draw Surface C instead of letting the user start something that
// will be refused. Asks about the caller and nobody else, so it discloses
// nothing about which accounts exist.
int64_t sys_elev_may(void)
{
    process_t *p = proc_current();
    if (!p) return 0;
    return users_may_elevate(p->euid) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Enforcement. Called from perms_check().
// ---------------------------------------------------------------------------
int elev_grant_permits(const char *path, uint32_t proc_uid)
{
    process_t *p = proc_current();
    if (!p || !path) return 0;
    if (p->elev_grant_until_ms == 0) return 0;
    if (sched_now_ms() > p->elev_grant_until_ms) {
        p->elev_grant_until_ms = 0;          // expired: forget it
        p->elev_grant_prefix[0][0] = 0;
        p->elev_grant_prefix[1][0] = 0;
        return 0;
    }
    // The uid being checked must be the grant holder's own. perms_check() is
    // called with an explicit uid by paths acting on behalf of other accounts;
    // a grant must never widen one of those.
    if (proc_uid != p->euid) return 0;
    for (int i = 0; i < 2; i++) {
        if (!p->elev_grant_prefix[i][0]) continue;
        if (elev_path_covered_rs(path, p->elev_grant_prefix[i])) return 1;
    }
    return 0;
}

void elevate_selftest(void)
{
    uint32_t r = elevate_selftest_rs();
    if (r == 0) {
        kprintf("[ELEV] selftest OK (path coverage, sanitiser, state machine)\n");
    } else {
        kprintf("[ELEV] SELFTEST FAILED case %u\n", (unsigned)r);
        (void)bootlog_write("[ELEV] SELFTEST FAILED case %u", (unsigned)r);
    }
}
