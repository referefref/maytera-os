// dosprobe - #745 followup: demonstrate the cross-account login-lockout DoS
// (Claim 2) and the cross-account password oracle (Claim 1) from an ordinary
// UNPRIVILEGED (uid 1000) process, and show the fix closing them.
//
// Runs as a SERVICES.CFG service at uid 1000 (account "admin"), perms=all, so a
// permission denial can never be confused with the #95 service sandbox. On the
// golden, admin (uid 1000) and root (uid 0) both exist; admin's password is
// "maytera".
//
// The shared counter under test is shadow_entry_t.failed_attempts /
// lockout_until_ms, which the KERNEL login gate (kernel/gui/login.c) reads and
// enforces. SYS_AUTH_LOCKOUT (read-only, unprivileged) returns the seconds of
// lockout remaining for a named account, i.e. exactly what the login gate will
// refuse against. A non-zero value for "root" read from this uid-1000 process
// IS the DoS: root cannot log in until it clears.
//
// Nothing here writes a credential or a file. It only makes wrong guesses and
// reads lockout state.

#include "../../libc/maytera.h"

#define SESSION_USER "admin"
#define SESSION_PW   "maytera"

static char g_line[512];
static int  g_len;
static void put_s(const char *s) { while (*s && g_len < (int)sizeof(g_line) - 1) g_line[g_len++] = *s++; }
static void put_n(long v) {
    char b[24]; int i = 23; int neg = 0; b[i--] = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) b[i--] = '0';
    while (v > 0) { b[i--] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) b[i--] = '-';
    put_s(&b[i + 1]);
}
static void flush_line(void) {
    if (g_len < (int)sizeof(g_line) - 1) g_line[g_len++] = '\n';
    write(2, g_line, (unsigned long)g_len);
    g_len = 0;
}
static void msg(const char *s) { g_len = 0; put_s("[DOS] "); put_s(s); flush_line(); }
static void note(const char *what, long v) {
    g_len = 0; put_s("[DOS] "); put_s(what); put_s(" = "); put_n(v); flush_line();
}

int main(void)
{
    msg("===== #745 followup DoS/oracle probe (unprivileged) =====");
    note("inherited uid (must be 1000 for this to prove anything)", syscall0(SYS_GETUID));

    // Baseline: root must NOT be locked out before we start.
    note("A0 baseline SYS_AUTH_LOCKOUT(root) before any guess (expect 0)",
         syscall1(SYS_AUTH_LOCKOUT, (long)"root"));

    // ---- Timing: how fast can this uid make cross-account auth attempts?
    // Measures the syscall cost of a wrong guess (PBKDF2-dominated) BEFORE any
    // lockout trips, so it reports the achievable burst rate honestly.
    {
        unsigned long t0 = (unsigned long)syscall0(SYS_UPTIME_MS);
        int n = 4;   // stop before the 5th, which trips the 30s lockout
        for (int i = 0; i < n; i++)
            (void)syscall2(SYS_AUTHENTICATE, (long)"root", (long)"wrong-timing-x");
        unsigned long t1 = (unsigned long)syscall0(SYS_UPTIME_MS);
        note("B timing: ms for 4 wrong sys_authenticate(root) guesses", (long)(t1 - t0));
    }

    // ---- CLAIM 2, THE DoS, via SYS_AUTHENTICATE (no privilege check pre-fix).
    // Six wrong guesses at ROOT from uid 1000. If the counter is shared and
    // reachable, root is locked out of the login gate afterwards.
    msg("--- C1: uid 1000 attacks ROOT via SYS_AUTHENTICATE ---");
    for (int i = 1; i <= 6; i++) {
        long rc = syscall2(SYS_AUTHENTICATE, (long)"root", (long)"definitely-not-root-pw");
        g_len = 0; put_s("[DOS] C1 authenticate(root) attempt "); put_n(i);
        put_s(" rc="); put_n(rc);
        if (rc == -1) put_s("  refused (EPERM/bad-pw)");
        else if (rc == -2) put_s("  LOCKED OUT (fed the shared counter)");
        else { put_s("  <<< AUTHENTICATED WITH A WRONG PASSWORD uid="); put_n(rc); }
        flush_line();
    }
    long lk_root = syscall1(SYS_AUTH_LOCKOUT, (long)"root");
    note("C1 RESULT SYS_AUTH_LOCKOUT(root) seconds remaining", lk_root);
    if (lk_root > 0)
        msg("C1 <<< DoS CONFIRMED: uid 1000 locked ROOT out of the login gate");
    else
        msg("C1 root NOT locked by an unprivileged process (fix holds)");

    // ---- CLAIM 1/2 via SYS_SU as well (Stage 3 routed su through the counter).
    msg("--- C2: uid 1000 attacks ROOT via SYS_SU ---");
    for (int i = 1; i <= 6; i++) {
        long rc = syscall2(SYS_SU, (long)"root", (long)"definitely-not-root-pw");
        g_len = 0; put_s("[DOS] C2 su(root) attempt "); put_n(i);
        put_s(" rc="); put_n(rc);
        if (rc == -1) put_s("  refused");
        else if (rc == -2) put_s("  LOCKED OUT");
        else put_s("  <<< ESCALATED TO ROOT");
        flush_line();
    }
    note("C2 RESULT SYS_AUTH_LOCKOUT(root) seconds remaining", syscall1(SYS_AUTH_LOCKOUT, (long)"root"));

    // ---- POSITIVE CONTROL: the rate limiter must still work for the caller's
    // OWN account. If this does NOT lock admin, the limiter is broken (an
    // outage), not a fix. admin is uid 1000 = us, so this is allowed on both
    // kernels and proves the probe genuinely reaches the authenticator.
    msg("--- D: positive control: wrong guesses at OWN account (admin) ---");
    for (int i = 1; i <= 6; i++) {
        long rc = syscall2(SYS_AUTHENTICATE, (long)SESSION_USER, (long)"wrong-own-pw");
        g_len = 0; put_s("[DOS] D authenticate(admin=self) attempt "); put_n(i);
        put_s(" rc="); put_n(rc); flush_line();
    }
    long lk_self = syscall1(SYS_AUTH_LOCKOUT, (long)SESSION_USER);
    note("D RESULT SYS_AUTH_LOCKOUT(admin) seconds remaining (expect >0)", lk_self);
    if (lk_self > 0) msg("D control OK: own-account rate limiting is intact");
    else             msg("D <<< CONTROL BROKEN: own-account guesses did not throttle");

    msg("===== end of probe =====");
    return 0;
}
