// userland/apps/secretprobe/main.c - #secretuse: END-TO-END proof of the
// secret-use proxy, on the real image, over serial.
//
// WHY THIS EXISTS AND WHAT IT PROVES THAT THE KATs DO NOT.
//
// crypto/secretuse_kat.sh proves the ARITHMETIC (RFC 6238, base32, the parser, the
// tier table) and crypto/secstore_kat.sh proves the ENVELOPE (tamper refusals).
// Neither one knows there is a syscall, a keyring, a login password, a file, or a
// session. Those are exactly the parts where a feature that passes every unit test
// still does nothing, and this tree has a name for it: a feature that renders, runs
// and does nothing.
//
// So this probe drives the real path on a booted machine:
//   C1  the enumerated surface is what the header says it is
//   C2  enrol a seed, and read back names WITHOUT a secret
//   C3  a live code: right length, and the countdown really counts down
//   C4  LOCK the session -> the key is wiped -> the store reports LOCKED, not empty
//   C5  a WRONG password does not unlock it (the key is not derived)
//   C6  the RIGHT password does, and the accounts come back intact
//   C7  delete, and the account is gone
//
// C4/C5/C6 are the load-bearing ones. They are the only evidence that the
// encryption is keyed by the login password at all: everything else would look
// identical if the "key" were a constant.
//
// It leaves NO state behind: C7 deletes what C2 enrolled. Run it with `--keep` to
// leave the account in place, which is what the tamper test needs (it flips a byte
// in the container between boots and expects the refusal).
//
// NOTE ON THE SEED: it is the RFC 6238 Appendix B test seed, which is published in
// the RFC and is not a credential. Nothing secret is written by this file.

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static int g_checks = 0, g_fails = 0;

// STATIC, deliberately. These total ~4.5KB and a userland app gets a 16KB stack
// (see blame.md on the Rust userland port's .bss-vs-stack trap). It would probably
// have fitted, and "probably fitted" is not something to discover by watching a
// probe fault on a VM and then not knowing whether the probe or the feature broke.
static secret_totp_info_t g_info[SECRET_MAX_ACCOUNTS];
static secret_totp_code_t g_codes[SECRET_MAX_ACCOUNTS];
static secret_totp_add_t  g_add, g_bad1, g_bad2;

// EVERY line goes to stdout AND to the bootlog. Not belt-and-braces: this probe is
// launched by CRON on a headless boot, where nothing is attached to stdout, and the
// iMac14,4 target has NO SERIAL PORT AT ALL. sys_bootlog() is the only channel that
// reaches both a serial console and /BOOTLOG.TXT on the image, so it is the only one
// that can carry evidence off either machine. A probe whose output goes nowhere is
// a probe that proves nothing.
static void say(const char *line) {
    printf("%s\n", line);
    sys_bootlog(line);
}

static void ck(const char *what, int ok, const char *detail) {
    char line[200];
    g_checks++;
    if (!ok) g_fails++;
    snprintf(line, sizeof(line), "[SECPROBE] %-4s %-46s %s",
             ok ? "ok" : "FAIL", what, detail ? detail : "");
    say(line);
}

// "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ" is base32 for the 20-byte ASCII string
// "12345678901234567890", the RFC 6238 Appendix B SHA-1 seed.
#define PROBE_B32   "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
#define PROBE_ISSUER "SecProbe"
#define PROBE_LABEL  "rfc6238"

static int find_probe(secret_totp_info_t *info, int n) {
    for (int i = 0; i < n; i++)
        if (strcmp(info[i].issuer, PROBE_ISSUER) == 0 &&
            strcmp(info[i].label, PROBE_LABEL) == 0) return i;
    return -1;
}

int main(int argc, char **argv) {
    int keep = 0;
    const char *user = "root";
    const char *pass = "maytera";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--keep") == 0) keep = 1;
        else if (strncmp(argv[i], "--user=", 7) == 0) user = argv[i] + 7;
        else if (strncmp(argv[i], "--pass=", 7) == 0) pass = argv[i] + 7;
    }

    say("[SECPROBE] ==== secret-use proxy, end-to-end on the booted image ====");

    // -------------------------------------------------------------- C1 surface
    // A compile-time fact, printed so the log records the ABI the kernel and this
    // app agreed on. A one-byte disagreement misreads every field after the first,
    // and the kernel _Static_asserts the same three numbers.
    {
        char d[96];
        snprintf(d, sizeof(d), "info=%d code=%d add=%d (kernel asserts 104/16/232)",
                 (int)sizeof(secret_totp_info_t), (int)sizeof(secret_totp_code_t),
                 (int)sizeof(secret_totp_add_t));
        ck("C1 ABI struct sizes agree",
           sizeof(secret_totp_info_t) == 104 && sizeof(secret_totp_code_t) == 16 &&
           sizeof(secret_totp_add_t) == 232, d);
    }

    int st = sys_secret_status();
    { char d[64]; snprintf(d, sizeof(d), "status=%d", st);
      ck("C1 STATUS answers", st >= 0 || st == SECRET_E_LOCKED, d); }
    if (st == SECRET_E_LOCKED) {
        { char l[120]; snprintf(l, sizeof(l),
            "[SECPROBE] store is LOCKED; unlocking as '%s' to run the rest", user); say(l); }
        int u = sys_session_unlock(user, pass);
        { char l[80]; snprintf(l, sizeof(l), "[SECPROBE] sys_session_unlock -> %d", u); say(l); }
        st = sys_secret_status();
    }
    if (st < 0) {
        { char l[140]; snprintf(l, sizeof(l),
            "[SECPROBE] cannot proceed: STATUS=%d. Give --user=/--pass= for this image.", st); say(l); }
        { char l[100]; snprintf(l, sizeof(l),
            "[SECPROBE] RESULT checks=%d fails=%d VERDICT=INCONCLUSIVE", g_checks, g_fails + 1); say(l); }
        return 2;
    }
    int before = st;

    // ----------------------------------------------------------------- C2 enrol
    secret_totp_add_t *a = &g_add;
    memset(a, 0, sizeof(*a));
    strlcpy(a->issuer, PROBE_ISSUER, sizeof(a->issuer));
    strlcpy(a->label, PROBE_LABEL, sizeof(a->label));
    strlcpy(a->b32, PROBE_B32, sizeof(a->b32));
    a->digits = 6; a->period = 30; a->alg = 1;
    int ar = sys_secret_add(a);
    { char d[64]; snprintf(d, sizeof(d), "rc=%d", ar);
      ck("C2 ADD accepts a valid seed", ar == 0, d); }

    // A BAD seed must be REFUSED, so C2 is not just "the call returns 0".
    g_bad1 = *a;
    strlcpy(g_bad1.b32, "MZXW1YTB", sizeof(g_bad1.b32));  // '1' is not in the alphabet
    int br = sys_secret_add(&g_bad1);
    { char d[64]; snprintf(d, sizeof(d), "rc=%d (want %d)", br, SECRET_E_FORMAT);
      ck("C2 ADD refuses invalid base32", br == SECRET_E_FORMAT, d); }
    // And an out-of-policy digit count.
    g_bad2 = *a;
    g_bad2.digits = 7;
    int b2 = sys_secret_add(&g_bad2);
    { char d[64]; snprintf(d, sizeof(d), "rc=%d", b2);
      ck("C2 ADD refuses digits outside {6,8}", b2 == SECRET_E_FORMAT, d); }

    secret_totp_info_t *info = g_info;
    int n = sys_secret_list(info, SECRET_MAX_ACCOUNTS);
    { char d[64]; snprintf(d, sizeof(d), "n=%d (was %d)", n, before);
      ck("C2 LIST shows exactly one more", n == before + 1, d); }
    int idx = find_probe(info, n > 0 ? n : 0);
    { char d[64]; snprintf(d, sizeof(d), "idx=%d", idx);
      ck("C2 LIST returns the names we enrolled", idx >= 0, d); }
    if (idx >= 0) {
        char d[80];
        snprintf(d, sizeof(d), "digits=%d period=%d alg=%d",
                 info[idx].digits, info[idx].period, info[idx].alg);
        ck("C2 LIST returns the geometry", info[idx].digits == 6 &&
           info[idx].period == 30 && info[idx].alg == 1, d);
    }

    // ------------------------------------------------------------------ C3 code
    secret_totp_code_t *codes = g_codes;
    int cn = sys_secret_codes(codes, SECRET_MAX_ACCOUNTS);
    { char d[64]; snprintf(d, sizeof(d), "n=%d", cn);
      ck("C3 CODES answers for every account", cn == n, d); }
    if (idx >= 0 && cn > idx) {
        int L = (int)strlen(codes[idx].code);
        int alldig = (L == 6);
        for (int i = 0; i < L; i++)
            if (codes[idx].code[i] < '0' || codes[idx].code[i] > '9') alldig = 0;
        char d[96];
        snprintf(d, sizeof(d), "code=%s len=%d remaining=%d period=%d",
                 codes[idx].code, L, codes[idx].remaining, codes[idx].period);
        ck("C3 a live 6-digit code, kernel clock", alldig, d);
        ck("C3 countdown is inside the window",
           codes[idx].remaining >= 1 && codes[idx].remaining <= 30, d);

        // THE COUNTDOWN MUST ACTUALLY MOVE. A constant would satisfy every check
        // above, which is the difference between "it returned a number" and "it
        // is tracking time".
        int r0 = codes[idx].remaining;
        unsigned long t0 = (unsigned long)sys_time();
        while ((unsigned long)sys_time() - t0 < 3) sys_sleep(100);
        int cn2 = sys_secret_codes(codes, SECRET_MAX_ACCOUNTS);
        int r1 = (cn2 > idx) ? codes[idx].remaining : -1;
        // Either it decreased, or the window rolled over (r1 jumped back up).
        int moved = (r1 >= 0) && (r1 != r0);
        snprintf(d, sizeof(d), "remaining %d -> %d after ~3s", r0, r1);
        ck("C3 the countdown moves with real time", moved, d);
    }

    // -------------------------------------------------- C4/C5/C6 the key is real
    // This is the part that proves the store is encrypted under the LOGIN
    // PASSWORD. Everything above would look identical if the key were a constant.
    int lr = sys_session_lock(1);
    { char l[80]; snprintf(l, sizeof(l), "[SECPROBE] sys_session_lock(1) -> %d", lr); say(l); }
    if (lr != 0) {
        ck("C4 session lock (needed for C4-C6)", 0, "lock declined; C4-C6 SKIPPED");
        say("[SECPROBE] NOTE: an autologin session or an account with no usable password");
        say("[SECPROBE]       declines the lock by policy (#745), so the key-lifecycle");
        say("[SECPROBE]       checks C4-C6 could not run here. That is NOT a pass.");
    } else {
        int s2 = sys_secret_status();
        { char d[72]; snprintf(d, sizeof(d), "status=%d (want %d LOCKED)", s2, SECRET_E_LOCKED);
          ck("C4 locking WIPES the key, store says LOCKED", s2 == SECRET_E_LOCKED, d); }
        // And LOCKED must not be reported as an empty list, which is the failure
        // the old app had for every cause.
        int n2 = sys_secret_list(info, SECRET_MAX_ACCOUNTS);
        { char d[72]; snprintf(d, sizeof(d), "list=%d (LOCKED, not 0)", n2);
          ck("C4 LOCKED is distinguishable from empty", n2 == SECRET_E_LOCKED, d); }

        int wrong = sys_session_unlock(user, "definitely-not-the-password");
        { char d[64]; snprintf(d, sizeof(d), "unlock rc=%d", wrong);
          ck("C5 a WRONG password does not unlock", wrong != 0, d); }
        int s3 = sys_secret_status();
        { char d[72]; snprintf(d, sizeof(d), "status=%d (still LOCKED)", s3);
          ck("C5 and derives no key: still LOCKED", s3 == SECRET_E_LOCKED, d); }

        int right = sys_session_unlock(user, pass);
        { char d[64]; snprintf(d, sizeof(d), "unlock rc=%d", right);
          ck("C6 the RIGHT password unlocks", right == 0, d); }
        int s4 = sys_secret_status();
        { char d[72]; snprintf(d, sizeof(d), "status=%d (want %d)", s4, before + 1);
          ck("C6 the accounts come back, intact", s4 == before + 1, d); }
        int n4 = sys_secret_list(info, SECRET_MAX_ACCOUNTS);
        int i4 = find_probe(info, n4 > 0 ? n4 : 0);
        ck("C6 the enrolled record survived a re-derive", i4 >= 0, NULL);
        int c4 = sys_secret_codes(codes, SECRET_MAX_ACCOUNTS);
        if (i4 >= 0 && c4 > i4) {
            char d[64]; snprintf(d, sizeof(d), "code=%s", codes[i4].code);
            ck("C6 and still generates a code", strlen(codes[i4].code) == 6, d);
        } else {
            ck("C6 and still generates a code", 0, "no code after unlock");
        }
        idx = i4;
    }

    // ---------------------------------------------------------------- C7 cleanup
    if (keep) {
        say("[SECPROBE] --keep: leaving the probe account enrolled");
    } else if (idx >= 0) {
        int dr = sys_secret_del((unsigned)idx);
        { char d[64]; snprintf(d, sizeof(d), "rc=%d", dr);
          ck("C7 DELETE removes it", dr == 0, d); }
        int n5 = sys_secret_list(info, SECRET_MAX_ACCOUNTS);
        { char d[64]; snprintf(d, sizeof(d), "n=%d (want %d)", n5, before);
          ck("C7 back to the starting count", n5 == before, d); }
        ck("C7 and it is really gone",
           find_probe(info, n5 > 0 ? n5 : 0) < 0, NULL);
    }

    { char l[100]; snprintf(l, sizeof(l), "[SECPROBE] RESULT checks=%d fails=%d VERDICT=%s",
        g_checks, g_fails, g_fails == 0 ? "PASS" : "FAIL"); say(l); }
    return g_fails == 0 ? 0 : 1;
}
