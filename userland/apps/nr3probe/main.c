// nr3probe - #745 Stage 3 red/green instrument.
//
// SAME SHAPE AS nrprobe DELIBERATELY (fd 2, one write per line, no setuid, byte
// counts and digests instead of contents) so one grep reads both.
//
// v2 fixes three things the first run exposed about the PROBE rather than about
// the kernel, because a probe that prints wrong values trains people to distrust
// the right ones:
//
//   * struct stat is now the REAL layout, taken from libc <sys/stat.h>, which is
//     byte-for-byte the kernel's k_stat_t. v1 guessed offsets, so st_mode read 0
//     and every object reported type=REG including directories.
//   * modes print in OCTAL. v1 printed decimal behind a literal "0" prefix, so
//     0750 came out as "0488" and read as octal to anyone skimming.
//   * SYS_PLAY_WAV is now decided on the EXACT return code (-13 EACCES from the
//     permission check, versus -1 from the decoder) and the VM is given an audio
//     device so the positive control can actually pass. In v1 the control failed
//     the same way as the deny vector, so the -1 proved nothing.
//
// NOTHING DESTRUCTIVE, AND NOTHING THAT OVERWRITES A REAL NAME. Stage 1's probe
// overwrote /CONFIG/PASSWD and corrupted its own control. The one vector here
// that writes credential state (C3) sets admin's password to the value it
// already has, so it is idempotent: it proves the success path without changing
// anything. The lockout vectors are ordered LAST for the same reason.
//
// NOTHING PRINTS FILE CONTENTS.

#include "../../libc/maytera.h"
#include "../../libc/pthread.h"
#include "../../libc/sys/stat.h"

#define SYS_PLAY_WAV_N 192
#define SESSION_USER "admin"
#define SESSION_PW   "maytera"

struct nr_fsperm {  // k_fsperm_info_t, 16 bytes (kernel _Static_assert)
    unsigned char fs_type, is_dir, has_perm_entry, fat_attr;
    unsigned short mode, reserved;
    unsigned int uid, gid;
};

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
// v2: modes are OCTAL. Printing 0750 as "0488" is how a correct value gets
// quoted as a wrong one.
static void put_o(unsigned long v) {
    char b[26]; int i = 25; b[i--] = 0;
    if (v == 0) b[i--] = '0';
    while (v > 0) { b[i--] = (char)('0' + (v & 7)); v >>= 3; }
    put_s("0"); put_s(&b[i + 1]);
}
static void flush_line(void) {
    if (g_len < (int)sizeof(g_line) - 1) g_line[g_len++] = '\n';
    write(2, g_line, (unsigned long)g_len);
    g_len = 0;
}
static void msg(const char *what) { g_len = 0; put_s("[NR3] "); put_s(what); flush_line(); }
static void note(const char *what, long v) {
    g_len = 0; put_s("[NR3] "); put_s(what); put_s(" = "); put_n(v); flush_line();
}

static void res(const char *id, const char *what, long rc, int expect_deny) {
    g_len = 0;
    put_s("[NR3] "); put_s(id);
    put_s(" rc="); put_n(rc);
    put_s(rc >= 0 ? "  PERMITTED" : "  DENIED   ");
    if (expect_deny) put_s(rc >= 0 ? "  <<< EXPOSURE" : "  (expected)");
    else             put_s(rc >= 0 ? "  (control OK)" : "  <<< CONTROL BROKEN");
    put_s("  "); put_s(what);
    flush_line();
}

static void nr_memset(void *d, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)d; while (n--) *p++ = (unsigned char)c;
}
static void nr_strcpy_v(volatile char *d, const char *s) { while ((*d++ = *s++)) ; }
static int nr_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static volatile char *g_race_name;
static void *race_flipper(void *arg) {
    volatile int *stop = (volatile int *)arg;
    while (!*stop) {
        g_race_name[0]='r'; g_race_name[1]='o'; g_race_name[2]='o'; g_race_name[3]='t'; g_race_name[4]=0;
        g_race_name[0]='a'; g_race_name[1]='d'; g_race_name[2]='m'; g_race_name[3]='i'; g_race_name[4]='n'; g_race_name[5]=0;
    }
    return 0;
}

static long v_stat(const char *id, const char *path, int expect_deny) {
    struct stat st; nr_memset(&st, 0, sizeof(st));
    long rc = syscall2(SYS_STAT, (long)path, (long)&st);
    res(id, path, rc, expect_deny);
    if (rc == 0) {
        g_len = 0; put_s("[NR3]     disclosed: st_mode="); put_o(st.st_mode & 07777);
        put_s(" type="); put_s((st.st_mode & 0040000u) ? "DIR" : "REG");
        put_s(" st_size="); put_n(st.st_size); flush_line();
    }
    return rc;
}

static long v_fsperm(const char *id, const char *path, int expect_deny) {
    struct nr_fsperm fp; nr_memset(&fp, 0, sizeof(fp));
    long rc = syscall3(SYS_FS_PERM_INFO, (long)path, 0, (long)&fp);
    res(id, path, rc, expect_deny);
    if (rc == 0) {
        g_len = 0; put_s("[NR3]     disclosed: uid="); put_n(fp.uid);
        put_s(" gid="); put_n(fp.gid);
        put_s(" mode="); put_o(fp.mode & 07777);
        put_s(" is_dir="); put_n(fp.is_dir);
        put_s(" has_entry="); put_n(fp.has_perm_entry); flush_line();
    }
    return rc;
}

static void show_cwd(const char *id) {
    char buf[512]; nr_memset(buf, 0, sizeof(buf));
    long rc = syscall2(SYS_GETCWD, (long)buf, (long)sizeof(buf));
    g_len = 0; put_s("[NR3] "); put_s(id); put_s(" getcwd rc="); put_n(rc);
    put_s("  cwd='"); put_s(buf); put_s("'"); flush_line();
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    msg("===== #745 Stage 3 probe v2. No setuid: this is the SESSION's authority. =====");
    long uid = syscall0(SYS_GETUID);
    note("inherited uid", uid);
    note("inherited gid", syscall0(SYS_GETGID));

    // ---------------------------------------------------------------- A
    msg("--- A. metadata gate (SYS_STAT / SYS_FS_PERM_INFO) ---");
    // /HOME/REF is uid 1002 mode 0750: for uid 1000 the OTHER bits are 0, so
    // there is no search permission and POSIX says stat beneath it must fail.
    v_stat  ("A1 stat   ", "/HOME/REF/CONFIG", 1);
    v_fsperm("A2 fsperm ", "/HOME/REF/CONFIG", 1);
    // A3/A3b ARE NOT EXPOSURES AND v1 WAS WRONG TO CALL THEM THAT. The parent
    // of /HOME/REF is /HOME, which is 0755, so search IS granted and POSIX
    // permits stat of the directory itself: `stat /home/someoneelse` works for
    // any user on any UNIX and reports its mode, uid and gid. The gate protects
    // what is INSIDE a directory you cannot search (A1/A2), not the existence of
    // the directory itself. Same class as A8: a stated limit, not a hole.
    v_stat  ("A3 limit  ", "/HOME/REF", 0);
    v_fsperm("A3b limit ", "/HOME/REF", 0);

    // Positive controls: the Files app's and Start menu's REAL use of these two
    // syscalls. If any goes DENIED the desktop is broken and the gate is too
    // strict, which is the failure a deny-everything check would hide.
    v_stat  ("A4 ctrl   ", "/APPS/FILES", 0);
    v_fsperm("A5 ctrl   ", "/APPS/FILES", 0);
    v_stat  ("A6 ctrl   ", "/HOME/ADMIN", 0);
    v_fsperm("A7 ctrl   ", "/HOME/ADMIN", 0);

    // STATED LIMIT, measured rather than argued. /CONFIG is 0711, which grants
    // search to everyone, so POSIX stat of a KNOWN name under it succeeds and
    // SHOULD. 0711 defeats enumeration, not existence. If this ever prints
    // DENIED, the gate is stricter than POSIX and something else will break.
    v_stat  ("A8 limit  ", "/CONFIG/SHADOW", 0);

    // ---------------------------------------------------------------- B
    msg("--- B. sys_chdir ---");
    // B1: 0711 grants x but not r. POSIX chdir needs x. The old code asked for r
    // via sys_open_k(path, O_RDONLY), so this was DENIED before and must be
    // PERMITTED after: the fix makes the system MORE usable, not less.
    res("B1 chdir  ", "/CONFIG (0711: x yes, r no. POSIX chdir needs x)",
        syscall1(SYS_CHDIR, (long)"/CONFIG"), 0);
    show_cwd("B1");
    // B2: a regular FILE is not a directory. Succeeded before at every uid.
    res("B2 chdir  ", "/CONFIG/PASSWD (a FILE, must be refused)",
        syscall1(SYS_CHDIR, (long)"/CONFIG/PASSWD"), 1);
    show_cwd("B2 (must be unchanged by a refused chdir)");
    // B3: another user's 0750 home. Refused before and after.
    res("B3 chdir  ", "/HOME/REF (0750 uid 1002)",
        syscall1(SYS_CHDIR, (long)"/HOME/REF"), 1);
    // B4: control. Own home must remain reachable.
    res("B4 ctrl   ", "/HOME/ADMIN (own home)",
        syscall1(SYS_CHDIR, (long)"/HOME/ADMIN"), 0);
    show_cwd("B4");

    // B5: CANONICALIZATION. Before the fix cwd became the literal string
    // "/HOME/ADMIN/..", which is the growth path into B6's truncation.
    res("B5 chdir  ", ".. from /HOME/ADMIN (cwd must become /HOME, not /HOME/ADMIN/..)",
        syscall1(SYS_CHDIR, (long)".."), 0);
    show_cwd("B5");

    // B6: THE GROWTH PATH, exercised as the parent asked. 100 x `cd ..` from a
    // real directory. Canonical cwd converges on "/" and stays there. The old
    // code grew the string three bytes per call straight into the 256-byte
    // truncation of fault 3.
    {
        syscall1(SYS_CHDIR, (long)"/HOME/ADMIN");
        for (int i = 0; i < 100; i++) syscall1(SYS_CHDIR, (long)"..");
        char buf[512]; nr_memset(buf, 0, sizeof(buf));
        syscall2(SYS_GETCWD, (long)buf, (long)sizeof(buf));
        int ok = nr_streq(buf, "/");
        g_len = 0; put_s("[NR3] B6 100x 'cd ..' -> cwd='"); put_s(buf); put_s("'  ");
        put_s(ok ? "(control OK: canonical, converged on /)"
                 : "<<< EXPOSURE: cwd accumulated or truncated");
        flush_line();
    }

    // B7: OVERLONG PATH. Must be REFUSED, never silently truncated into a
    // different path. Build 400 chars of "/HOME/ADMIN/AAAA...".
    {
        static char big[600];
        int n = 0;
        const char *pfx = "/HOME/ADMIN/";
        while (pfx[n]) { big[n] = pfx[n]; n++; }
        while (n < 400) big[n++] = 'A';
        big[n] = 0;
        syscall1(SYS_CHDIR, (long)"/HOME/ADMIN");
        long rc = syscall1(SYS_CHDIR, (long)big);
        res("B7 chdir  ", "a 400-byte path (must refuse, never truncate)", rc, 1);
        show_cwd("B7 (must still be /HOME/ADMIN)");
    }

    // ---------------------------------------------------------------- D
    // Before the credential vectors, because C4 deliberately locks the account
    // out and everything after it would then be measuring the lockout.
    msg("--- D. sys_play_wav: arbitrary path into a Ring-0 decoder ---");
    // v2: decided on the EXACT code. -13 is the permission refusal; -1 is the
    // decoder declining the input, which is what v1 could not tell apart.
    {
        long rc = syscall1(SYS_PLAY_WAV_N, (long)"/CONFIG/SSHHOST.KEY");
        g_len = 0; put_s("[NR3] D1 playwav rc="); put_n(rc);
        put_s(rc == -13 ? "  DENIED by perms_check (expected after)"
                        : (rc < 0 ? "  failed, but NOT -13: decoder refusal, NOT the gate"
                                  : "  <<< EXPOSURE: Ring 0 read a root 0600 file"));
        put_s("  /CONFIG/SSHHOST.KEY"); flush_line();
    }
    res("D2 ctrl   ", "/SOUNDS/BOOTSND.MP3 (world readable; needs an audio device)",
        syscall1(SYS_PLAY_WAV_N, (long)"/SOUNDS/BOOTSND.MP3"), 0);

    // ---------------------------------------------------------------- E
    msg("--- E. sys_su TOCTOU: does the kernel re-read the username? ---");
    // Needs a REAL second thread: a single-threaded loop cannot express the bug
    // because the buffer is stable for the whole syscall. Only a HIT is
    // evidence; a miss proves nothing and is reported as a miss.
    // v2 FIRST ATTEMPT WAS INVALID AND THE GREEN RUN CAUGHT IT. It raced with
    // SESSION_PW, and root and admin SHARE the first-boot password (that is
    // exactly the item-4 problem), so su("root", SESSION_PW) SUCCEEDS on its own
    // merits. The "escalation" it reported was an ordinary authenticated su, and
    // the audit line this change added is what proved it:
    //     [AUTH] su OK: uid=1000 -> 'root' uid=0
    // A vector that passes for the wrong reason is worse than no vector.
    //
    // So: give admin a password root does NOT have, race with THAT, and put it
    // back. Now su("root", RACE_PW) can only ever succeed by winning the race
    // between the kernel's two reads of the name, which is the actual bug.
    {
        static volatile char name[16];
        static volatile int  stop = 0;
        const char *RACE_PW = "nr3-race-distinct";

        long setrc = syscall3(SYS_PASSWD_CHANGE, (long)SESSION_USER,
                              (long)SESSION_PW, (long)RACE_PW);
        note("E0 gave admin a password root does not share (0 = ready)", setrc);
        if (setrc != 0) {
            msg("E1 SKIPPED: could not establish distinct passwords, so a hit "
                "would not be attributable to the race");
        } else {
            nr_strcpy_v(name, SESSION_USER);
            g_race_name = name;
            pthread_t th;
            pthread_create(&th, 0, race_flipper, (void *)&stop);
            int hits = 0;
            for (int round = 0; round < 20000 && !hits; round++) {
                long rc = syscall2(SYS_SU, (long)name, (long)RACE_PW);
                if (rc == 0 && syscall0(SYS_GETUID) == 0) hits++;
            }
            stop = 1;
            note("E1 uid after the race (0 means ESCALATED to root)", syscall0(SYS_GETUID));
            note("E1 escalation hits", hits);
            if (hits) msg("E1 <<< EXPOSURE: authenticated as 'admin', granted uid 0 by a racing su");
            else      msg("E1 no escalation observed this run (a miss is not proof of closure)");
            // Put it back so the C vectors below measure what they think they do.
            long back = syscall3(SYS_PASSWD_CHANGE, (long)SESSION_USER,
                                 (long)RACE_PW, (long)SESSION_PW);
            note("E2 restored admin's original password (0 = restored)", back);
        }
    }

    // ---------------------------------------------------------------- C
    msg("--- C. credentials: success paths first, then the lockout ---");

    // C2 POSITIVE CONTROL: su with the CORRECT password must still work. A
    // rate limiter that refuses valid credentials is an outage, not a control.
    res("C2 ctrl   ", "su admin with the CORRECT password",
        syscall2(SYS_SU, (long)SESSION_USER, (long)SESSION_PW), 0);
    note("C2 uid after a legitimate su", syscall0(SYS_GETUID));

    // C3 POSITIVE CONTROL, IDEMPOTENT ON PURPOSE: change admin's password to the
    // value it already holds. Proves the success path through
    // users_authenticate() without altering any credential on the image.
    res("C3 ctrl   ", "passwd admin: correct old password, SAME new password (idempotent)",
        syscall3(SYS_PASSWD_CHANGE, (long)SESSION_USER, (long)SESSION_PW, (long)SESSION_PW), 0);

    // C1: six wrong guesses at ROOT. users_authenticate() escalates at 5, so
    // attempts 5 and 6 must return -2 once su routes through it. Before Stage 3
    // every attempt returned -1 forever: an unthrottled, unaudited password
    // oracle against root from an unprivileged process.
    {
        int saw_lockout = 0;
        for (int i = 1; i <= 6; i++) {
            long rc = syscall2(SYS_SU, (long)"root", (long)"definitely-not-the-password");
            g_len = 0; put_s("[NR3] C1 su root attempt "); put_n(i);
            put_s(" rc="); put_n(rc);
            put_s(rc == -2 ? "  LOCKED OUT (expected from 5)" : "  refused, no lockout");
            flush_line();
            if (rc == -2) saw_lockout = 1;
            if (rc == 0) { msg("C1 <<< su SUCCEEDED WITH A WRONG PASSWORD"); break; }
        }
        note("C1 su lockout observed (1 = rate limiting is live)", saw_lockout);
        if (!saw_lockout)
            msg("C1 <<< EXPOSURE: unlimited su guesses against root, no lockout, no audit");
    }

    // C4 LAST, because it locks this account out: the same question for passwd's
    // old-password check, which was the second unaccounted verify path.
    {
        int saw_lockout = 0;
        for (int i = 1; i <= 6; i++) {
            long rc = syscall3(SYS_PASSWD_CHANGE, (long)SESSION_USER,
                               (long)"wrong-old-password", (long)SESSION_PW);
            g_len = 0; put_s("[NR3] C4 passwd wrong-old attempt "); put_n(i);
            put_s(" rc="); put_n(rc);
            put_s(rc == -2 ? "  LOCKED OUT (expected from 5)" : "  refused, no lockout");
            flush_line();
            if (rc == -2) saw_lockout = 1;
        }
        note("C4 passwd lockout observed (1 = rate limiting is live)", saw_lockout);
    }

    msg("===== end of Stage 3 probe v2 =====");
    return 0;
}
