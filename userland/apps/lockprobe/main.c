// lockprobe - #745 Stage 2 instrument: can the session that is actually
// running LOCK itself and then UNLOCK itself with its own user's password?
//
// WHY A SECOND PROBE AND NOT MORE OF nrprobe. nrprobe answers "what AUTHORITY
// does the session inherit" and its vectors are file accesses. This answers a
// different question, "can the session PROVE WHO IT IS to the kernel", and its
// vectors are the three session syscalls. Bolting them onto nrprobe would have
// made a single pass/fail line mean two unrelated things.
//
// WHY IT EXISTS AT ALL, given the lock screen is a GUI. The lock/unlock cycle
// is a compositor UI, and headless pointer injection into this desktop does not
// reliably land clicks (#334), so a GUI-only proof would be a screenshot of a
// state nobody could show was reached deliberately. Every security-relevant
// decision in that UI is one of the three syscalls below, and the compositor is
// a thin wrapper over them: lockscreen.c do_unlock() is literally
// sys_session_unlock("", typed_password). Driving the syscalls measures the
// mechanism; the GUI run measures the wiring on top of it. Both are reported.
//
// RED / GREEN. The vectors are chosen so the SAME binary tells the two
// configurations apart, without recompiling and without setuid:
//
//   autologin=root   (today)     session uid 0    name "root"
//   autologin=admin  (flipped)   session uid 1000 name "admin"
//
//   L3 is the exact call the OLD compositor made: sys_session_unlock("root",
//   <the session user's own password>). At uid 0 it succeeds because the
//   hardcoded name happens to be right. At uid 1000 it FAILS, which is the
//   showstopper reproduced in one line: the user types their own correct
//   password and the session does not unlock.
//
//   L5 is the same unlock with the name left to the kernel. It must succeed in
//   BOTH configurations. L3 red + L5 green is the whole fix.
//
// THE PASSWORD IS NOT IN THIS FILE and must never be. It is read at runtime
// from /PROBEPW.TXT, which exists only on a throwaway test image and is never
// committed: the internal asset base's credentials must not reach the repo that
// seeds the public subset. If the file is absent the probe says so and skips
// the vectors that need it, rather than guessing.
//
// NOTHING DESTRUCTIVE TO AN EXISTING NAME. Recorded the hard way at #745 Stage
// 1: nrprobe's write vectors overwrote /CONFIG/PASSWD and, in the RED branch
// where every write SUCCEEDS, corrupted the account database it was measuring.
// The account-creation vectors here only ever create NEW names
// (lp_new_*), never touch an existing one, and never delete.
//
// EVERYTHING GOES TO fd 2 (an autorun-spawned process's fd 1 does not reach the
// serial console), one write() per line.

#include "../../libc/maytera.h"

#ifndef SESSION_LOCK_IDLE
#define SESSION_LOCK_IDLE      0
#define SESSION_LOCK_EXPLICIT  1
#endif

// --------------------------------------------------------------------------
// fd-2 line output (same shape as nrprobe/sgprobe, deliberately, so one grep
// reads all three)
// --------------------------------------------------------------------------
static char g_line[512];
static int  g_len;

static void put_s(const char *s) {
    while (*s && g_len < (int)sizeof(g_line) - 1) g_line[g_len++] = *s++;
}
static void put_n(long v) {
    char b[24]; int i = 23; int neg = 0;
    b[i--] = 0;
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
static void msg(const char *what) {
    g_len = 0; put_s("[LKP] "); put_s(what); flush_line();
}
static void note(const char *what, long v) {
    g_len = 0; put_s("[LKP] "); put_s(what); put_s(" = "); put_n(v); flush_line();
}
static void note_s(const char *what, const char *v) {
    g_len = 0; put_s("[LKP] "); put_s(what); put_s(" = '"); put_s(v); put_s("'"); flush_line();
}

// A vector result. `want_ok` says what SHOULD happen, so a reader does not have
// to hold the design in their head to see whether a line is good news.
static void res(const char *id, const char *what, long rc, int want_ok) {
    g_len = 0;
    put_s("[LKP] "); put_s(id);
    put_s(" rc="); put_n(rc);
    put_s(rc == 0 ? "  OK      " : "  FAILED  ");
    if (want_ok) put_s(rc == 0 ? "  (expected)   " : "  <<< BLOCKER  ");
    else         put_s(rc == 0 ? "  <<< UNEXPECTED SUCCESS " : "  (expected)   ");
    put_s("  "); put_s(what);
    flush_line();
}

static void lp_memset(void *d, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)d; while (n--) *p++ = (unsigned char)c;
}
static int lp_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void lp_strcpy(char *d, const char *s) { while ((*d++ = *s++)) ; }

// Read the test password from /PROBEPW.TXT. Returns 1 on success. The value is
// NEVER printed; only its length is, which is enough to tell "read a password"
// from "read an empty file" without disclosing it.
static int read_probe_password(char *out, int cap) {
    out[0] = '\0';
    long fd = syscall2(SYS_OPEN, (long)"/PROBEPW.TXT", 0 /* O_RDONLY */);
    if (fd < 0) return 0;
    long n = syscall3(SYS_READ, fd, (long)out, (long)(cap - 1));
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) { out[0] = '\0'; return 0; }
    out[n] = '\0';
    // Trim a trailing newline / CR so `echo pw > file` works as expected.
    for (long i = n - 1; i >= 0; i--) {
        if (out[i] == '\n' || out[i] == '\r' || out[i] == ' ') out[i] = '\0';
        else break;
    }
    return out[0] != '\0';
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    msg("===== #745 SESSION LOCK / IDENTITY probe =====");
    msg("No setuid. The uid below is the DESKTOP SESSION's, inherited as spawned.");

    long uid = syscall0(SYS_GETUID);
    long euid = (long)geteuid();
    note("inherited uid", uid);
    note("inherited euid", euid);

    // ----------------------------------------------------------------------
    // I. IDENTITY. What the compositor USED to believe vs what is true.
    // ----------------------------------------------------------------------
    char sessname[64];
    sessname[0] = '\0';
    {
        user_info_t ui[16];
        int n = sys_list_users(ui, 16);
        note("accounts visible via SYS_LIST_USERS", n);
        for (int i = 0; i < n; i++) {
            g_len = 0;
            put_s("[LKP]   account uid="); put_n((long)ui[i].uid);
            put_s(" name='"); put_s(ui[i].username); put_s("'");
            if ((long)ui[i].uid == euid) put_s("   <<< THIS SESSION");
            flush_line();
            if ((long)ui[i].uid == euid) {
                int j = 0;
                while (ui[i].username[j] && j < 63) { sessname[j] = ui[i].username[j]; j++; }
                sessname[j] = '\0';
            }
        }
    }
    note_s("session username resolved from the kernel", sessname[0] ? sessname : "(none)");
    note_s("name the OLD compositor hardcoded", "root");
    if (sessname[0] && !lp_streq(sessname, "root")) {
        msg("I1  the hardcoded name DISAGREES with the session  <<< this is the showstopper");
    } else {
        msg("I1  the hardcoded name happens to match (uid 0 session): the bug is INVISIBLE here");
    }

    char pw[128];
    int have_pw = read_probe_password(pw, (int)sizeof(pw));
    note("test password bytes read from /PROBEPW.TXT", have_pw ? (long)0 + (long)1 : 0);
    if (!have_pw) {
        msg("NO /PROBEPW.TXT: skipping every vector that needs a password.");
        msg("(Place the session user's own password there on the TEST image only.)");
    }

    // ----------------------------------------------------------------------
    // II. LOCK. Explicit lock must be honoured; before #745 it was silently
    //     discarded for any autologin session, so this whole path was dead.
    // ----------------------------------------------------------------------
    long was_locked = sys_session_is_locked();
    note("session locked before we touch anything", was_locked);

    long rc_idle = sys_session_lock(SESSION_LOCK_IDLE);
    long st_idle = sys_session_is_locked();
    g_len = 0;
    put_s("[LKP] L1 idle lock request: rc="); put_n(rc_idle);
    put_s(" locked="); put_n(st_idle);
    put_s("   (declined is CORRECT on an autologin box; #566)");
    flush_line();
    if (st_idle) { sys_session_unlock("", have_pw ? pw : "x"); }

    long rc_lock = sys_session_lock(SESSION_LOCK_EXPLICIT);
    long st_lock = sys_session_is_locked();
    res("L2", "explicit lock request (Start Menu / Super+L path)", st_lock ? 0 : -1, 1);
    note("  kernel lock rc", rc_lock);

    // NOT a return. The account-creation vectors in section IV are independent
    // of the lock, and returning here (as this probe first did) meant a BEFORE
    // image, where the lock always fails, silently skipped half its evidence.
    if (!st_lock) {
        msg("L2 did not lock, so the unlock vectors are SKIPPED (not run, not passed).");
        msg("(If the session user has no password, the kernel refusing to lock is");
        msg(" the DESIGNED behaviour: a session that cannot unlock is never locked.)");
    }

    if (st_lock && have_pw) {
        // ------------------------------------------------------------------
        // III. UNLOCK. L3 is the exact call the old compositor made.
        // ------------------------------------------------------------------
        long rc_root = sys_session_unlock("root", pw);
        long st_root = sys_session_is_locked();
        g_len = 0;
        put_s("[LKP] L3 unlock as the hardcoded \"root\" with the SESSION USER'S OWN password: rc=");
        put_n(rc_root); put_s(" stillLocked="); put_n(st_root);
        if (st_root) put_s("   <<< THE SHOWSTOPPER REPRODUCED (correct password refused)");
        else         put_s("   (uid 0 session: the hardcoded name was right by luck)");
        flush_line();

        if (st_root) {
            // ---- negative control: a WRONG password must be refused --------
            long rc_bad = sys_session_unlock("", "definitely-not-the-password");
            long st_bad = sys_session_is_locked();
            res("L4", "unlock with a WRONG password (must be refused)", st_bad ? 0 : -1, 1);
            note("  kernel unlock rc (negative, -2 = rate limited)", rc_bad);

            // ---- the fix: let the kernel resolve the session user ----------
            long rc_ok = sys_session_unlock("", pw);
            long st_ok = sys_session_is_locked();
            res("L5", "unlock with the session user's OWN password, name left to the kernel",
                st_ok ? -1 : 0, 1);
            note("  kernel unlock rc", rc_ok);
        } else {
            msg("L4/L5 skipped: L3 already unlocked (uid 0 session).");
        }
    }

    // Never leave the session locked. A probe that bricks the desktop it is
    // measuring is the Stage 1 lesson in a new costume.
    if (sys_session_is_locked() && have_pw) {
        long rc_fin = sys_session_unlock("", pw);
        note("cleanup unlock rc", rc_fin);
    }
    note("session locked at exit (must be 0)", sys_session_is_locked());

    // ----------------------------------------------------------------------
    // IV. ACCOUNT CREATION. Both paths, side by side, on NEW names only.
    // ----------------------------------------------------------------------
    if (euid == 0) {
        msg("--- account creation (root session, so both paths are reachable) ---");

        // A. the OLD path: adduser() with no password parameter.
        long ra = adduser("lp_old", 1500, 1500, "/HOME/LPOLD", "/APPS/MSH");
        note("A1 adduser('lp_old') rc", ra);
        if (ra == 0) {
            long auth = sys_authenticate("lp_old", "anything-at-all");
            res("A2", "can the adduser() account authenticate? (it never could)",
                auth == 0 ? 0 : -1, 0);
        }

        // B. the NEW path: create + password in one call, kernel-allocated uid.
        long rb = sys_user_create_pw("lp_new", "Probe-Passw0rd", 0, 0, 0);
        note("B1 sys_user_create_pw('lp_new') -> uid", rb);
        if (rb >= 0) {
            // SYS_AUTHENTICATE RETURNS THE UID ON SUCCESS, not 0, and is
            // negative on failure (-1 bad credentials, -2 locked out). Getting
            // that contract wrong made this probe print "<<< BLOCKER" against a
            // working system on its first run: it compared against 0 and read
            // uid 1001 as a failure. An instrument that asserts the wrong
            // success contract manufactures findings.
            long auth_ok  = sys_authenticate("lp_new", "Probe-Passw0rd");
            long auth_bad = sys_authenticate("lp_new", "wrong-password");
            note("  sys_authenticate(correct pw) -> uid", auth_ok);
            note("  sys_authenticate(wrong pw)   -> rc ", auth_bad);
            res("B2", "the new account authenticates with its own password",
                auth_ok >= 0 ? 0 : -1, 1);
            res("B3", "the new account REFUSES a wrong password",
                auth_bad < 0 ? -1 : 0, 0);
            if (rb == 0) msg("B1 <<< uid 0 allocated: the allocator is WRONG");
        }

        // C. the uid allocator: a second account must not collide with the first.
        long rc2 = sys_user_create_pw("lp_new2", "Probe-Passw0rd2", 0, 0, 0);
        note("C1 second create -> uid (must differ from B1)", rc2);
        if (rc2 >= 0 && rc2 == rb) msg("C1 <<< COLLISION: the allocator returned the same uid twice");
    } else {
        msg("--- account creation skipped: not root, and account creation is root-only ---");
        long rb = sys_user_create_pw("lp_nonroot", "Probe-Passw0rd", 0, 0, 0);
        res("A0", "a NON-ROOT session must not be able to create an account",
            rb >= 0 ? 0 : -1, 0);
    }

    msg("===== probe complete =====");
    (void)lp_memset; (void)lp_strcpy;
    return 0;
}
