// nrprobe - NON-ROOT SESSION probe (#674/#700 follow-on: the desktop-uid flip).
//
// WHY THIS IS NOT sgprobe. sgprobe calls setuid(1000) as its first action and
// asserts the drop, because the shipped image autologins as root and it needed
// SOME non-root uid to measure against. That makes it a test of the SYSCALLS.
// It cannot answer the question this change is about, which is a test of the
// SESSION: "what authority does a process inherit from the desktop the user is
// actually sitting in front of?"
//
// So this probe DELIBERATELY DOES NOT DROP PRIVILEGE. It reports whatever uid
// it inherited and then makes the same requests. That single difference is what
// makes it a red/green instrument for the flip:
//
//   autologin=root  (today)  -> inherits uid 0 -> every vector SUCCEEDS  = RED
//   autologin=admin (flipped)-> inherits uid 1000 -> vectors DENIED      = GREEN
//
// The RED half is the whole point of the exercise. It is not a bug in the
// syscalls (they are gated correctly since #700); it is the demonstration that
// the gating is VACUOUS while the session is uid 0. A probe that dropped
// privilege would print green on both sides and prove nothing.
//
// POSITIVE CONTROLS ARE MANDATORY HERE. A run consisting only of denials cannot
// be told apart from a system that is simply broken, which is the failure mode
// that matters most: the flip must leave a WORKING desktop, not a locked-out
// one. Every deny vector is therefore paired with an allow vector that MUST
// still succeed at the session uid (read PASSWD, write inside our own home).
// If the controls go red, the flip has broken the desktop and must not ship.
//
// NOTHING PRINTS FILE CONTENTS. Where a read succeeds, the evidence is a byte
// count and a 32-bit FNV-1a digest, never the bytes. A probe that dumped
// /CONFIG/SHADOW to the serial console would be the disclosure it is testing
// for, and the serial log is captured into build artefacts.
//
// EVERYTHING GOES TO fd 2. Measured on golden 1004 and re-recorded in
// apps/canarytest: an autorun-spawned process's fd 1 does not reach the serial
// console; fd 2 does. One write() per line: the kernel serial logger timestamps
// every write() as its own record, so a line assembled from several writes comes
// out fragmented and interleaved with concurrent kernel output.

#include "../../libc/maytera.h"

#define SYS_PKG_WRITE       301
#define SYS_CRON_ADD        276

#define CRON_TARGET_MAX 64
#define CRON_LABEL_MAX  32
#define CRON_TYPE_ONESHOT  0
#define CRON_ACT_LAUNCH    1

typedef struct {
    unsigned int   id;
    unsigned char  type;
    unsigned char  action;
    unsigned char  enabled;
    unsigned char  weekday;
    unsigned char  hour;
    unsigned char  minute;
    unsigned char  reserved[2];
    unsigned int   interval_ms;
    unsigned int   run_count;
    unsigned long long next_fire_tick;
    char           target[CRON_TARGET_MAX];
    char           label[CRON_LABEL_MAX];
} cron_job_t;

// --------------------------------------------------------------------------
// fd-2 line output (same shape as sgprobe, deliberately, so one grep reads both)
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
static void put_x(unsigned long v) {
    static const char h[] = "0123456789abcdef";
    char b[20]; int i = 19; b[i--] = 0;
    if (v == 0) b[i--] = '0';
    while (v > 0) { b[i--] = h[v & 0xF]; v >>= 4; }
    put_s(&b[i + 1]);
}
static void flush_line(void) {
    if (g_len < (int)sizeof(g_line) - 1) g_line[g_len++] = '\n';
    write(2, g_line, (unsigned long)g_len);
    g_len = 0;
}
static void msg(const char *what) {
    g_len = 0; put_s("[NRP] "); put_s(what); flush_line();
}
static void note(const char *what, long v) {
    g_len = 0; put_s("[NRP] "); put_s(what); put_s(" = "); put_n(v); flush_line();
}

// A result line. `expect_deny` records what this vector is FOR, so the log says
// whether a given outcome is the good one without the reader holding the whole
// design in their head.
static void res(const char *id, const char *what, long rc, int expect_deny) {
    g_len = 0;
    put_s("[NRP] "); put_s(id);
    put_s(" rc="); put_n(rc);
    put_s(rc >= 0 ? "  PERMITTED" : "  DENIED   ");
    // A vector that is supposed to be refused but was permitted is the finding.
    if (expect_deny) put_s(rc >= 0 ? "  <<< EXPOSURE" : "  (expected)");
    else             put_s(rc >= 0 ? "  (control OK)" : "  <<< CONTROL BROKEN");
    put_s("  "); put_s(what);
    flush_line();
}

static void nr_memset(void *d, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)d; while (n--) *p++ = (unsigned char)c;
}
static void nr_strcpy(char *d, const char *s) { while ((*d++ = *s++)) ; }

// Read a file WITHOUT ever emitting its bytes: length plus FNV-1a-32 only.
// Returns the fd result; sets *len and *dig when the read succeeded.
static long read_digest(const char *path, long *len, unsigned long *dig) {
    *len = 0; *dig = 2166136261UL;
    long fd = syscall2(SYS_OPEN, (long)path, 0 /* O_RDONLY */);
    if (fd < 0) return fd;
    unsigned char buf[256];
    for (;;) {
        long n = syscall3(SYS_READ, fd, (long)buf, (long)sizeof(buf));
        if (n <= 0) break;
        for (long i = 0; i < n; i++) { *dig ^= buf[i]; *dig *= 16777619UL; }
        *len += n;
    }
    syscall1(SYS_CLOSE, fd);
    return fd;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    msg("===== NON-ROOT SESSION probe: what did this process inherit? =====");
    msg("This probe does NOT call setuid. The uid below is the DESKTOP SESSION's.");

    long uid = syscall0(SYS_GETUID);
    long gid = syscall0(SYS_GETGID);
    note("inherited uid", uid);
    note("inherited gid", gid);

    if (uid == 0) {
        msg("VERDICT: session is uid 0 (ROOT). Every check below is VACUOUS:");
        msg("VERDICT: perms_check() short-circuits on uid 0 before reading policy.");
        msg("VERDICT: this run is the RED control for the desktop-uid flip.");
    } else {
        msg("VERDICT: session is NON-ROOT. The checks below are REAL.");
    }

    // ----------------------------------------------------------------------
    // D1  read the password hashes. The canonical confidentiality boundary.
    // ----------------------------------------------------------------------
    {
        long len; unsigned long dig;
        long rc = read_digest("/CONFIG/SHADOW", &len, &dig);
        res("D1", "open /CONFIG/SHADOW O_RDONLY (0600 root)", rc, 1);
        if (rc >= 0) {
            g_len = 0;
            put_s("[NRP] D1 DISCLOSED bytes="); put_n(len);
            put_s(" fnv1a=0x"); put_x(dig);
            put_s("  (digest only, never the bytes)");
            flush_line();
        }
    }

    // ----------------------------------------------------------------------
    // D2  ENUMERATE /CONFIG. This is the 0755-vs-0711 question, measured.
    //     open() of a directory takes R_OK (syscall.c sys_open_k), and readdir
    //     needs that fd, so at 0711 this must fail while D5 (traversal to a
    //     known 0644 path) keeps working. If BOTH work, 0711 was not applied;
    //     if BOTH fail, /CONFIG is not traversable and the session is broken.
    // ----------------------------------------------------------------------
    {
        long fd = syscall2(SYS_OPEN, (long)"/CONFIG", 0);
        res("D2", "open /CONFIG for enumeration (dir R_OK)", fd, 1);
        if (fd >= 0) {
            char de[512]; long n = 0;
            while (syscall2(SYS_READDIR, fd, (long)de) >= 0 && n < 4096) n++;
            note("D2 entries enumerated in /CONFIG", n);
            syscall1(SYS_CLOSE, fd);
        }
    }

    // ----------------------------------------------------------------------
    // D3  write a root-owned file. Integrity boundary.
    // ----------------------------------------------------------------------
    //
    // CREATE A NEW NAME rather than overwrite an existing file. Measured on the
    // first run of this probe (golden 1743, VM <vmid>, autologin=root): the
    // earlier version wrote 7 bytes over /CONFIG/PASSWD and over /BOOT.BMP, and
    // at uid 0 both SUCCEEDED, so the probe destroyed the account database and
    // the boot splash of the image it was measuring. The very next assertion
    // read /CONFIG/PASSWD back as 7 bytes instead of 108, i.e. the probe had
    // corrupted its own positive control.
    //
    // The boundary being tested is identical either way: since #676, creating a
    // NAME is a write to the PARENT DIRECTORY, which is exactly the rule
    // pkg_write_permit() applies. Creating /NRPTGT.TXT tests write access to
    // root-owned /, and /CONFIG/NRPTGT.TXT tests it on root-owned /CONFIG, with
    // nothing to destroy in either case. A probe must not be able to damage the
    // artifact it is measuring, least of all in the RED configuration, which is
    // precisely the run where every destructive vector succeeds.
    {
        static const char payload[] = "nrprobe";
        res("D3", "pkg_write /NRPTGT.TXT (create in root-owned /)",
            syscall3(SYS_PKG_WRITE, (long)"/NRPTGT.TXT", (long)payload, 7), 1);
        res("D3", "pkg_write /CONFIG/NRPTGT.TXT (create in root-owned /CONFIG)",
            syscall3(SYS_PKG_WRITE, (long)"/CONFIG/NRPTGT.TXT", (long)payload, 7), 1);
    }

    // ----------------------------------------------------------------------
    // D4  INSTALL A PACKAGE into the system app directory. This is the
    //     package-install decision made measurable: /APPS is 0:0:0755, so a
    //     non-root session must be refused here. It is listed as a DENY vector
    //     on purpose. If it is ever permitted from a normal session, /APPS has
    //     been loosened and any logged-in user can replace any installed
    //     binary, which is the outcome the whole change exists to prevent.
    // ----------------------------------------------------------------------
    {
        static const unsigned char elfmagic[16] =
            {0x7F,'E','L','F',2,1,1,0,0,0,0,0,0,0,0,0};
        res("D4", "pkg_write /APPS/NRPTEST (system app dir, must be DENIED)",
            syscall3(SYS_PKG_WRITE, (long)"/APPS/NRPTEST", (long)elfmagic, 16), 1);
    }

    // ----------------------------------------------------------------------
    // D6  forge a root-owned cron job by newline injection (the #692/B4 shape).
    //     The kernel stamps the CALLER's uid; the attack is to write bytes that
    //     a later legitimate parse of CRON.CFG reads as a second job at uid 0.
    // ----------------------------------------------------------------------
    {
        cron_job_t j;
        nr_memset(&j, 0, sizeof(j));
        j.type = CRON_TYPE_ONESHOT;
        j.action = CRON_ACT_LAUNCH;
        j.enabled = 1;
        j.interval_ms = 3600000;
        nr_strcpy(j.target, "X\nINTERVAL 10 launch /APPS/WHOAMI uid=0");
        nr_strcpy(j.label, "nrp-forge");
        res("D6", "cron_add newline-injected root job", 
            syscall1(SYS_CRON_ADD, (long)&j), 1);
    }

    // ======================================================================
    // POSITIVE CONTROLS. These MUST succeed at the session uid. If any of them
    // goes red, the flip has produced a locked-out desktop rather than a
    // secured one, and that is a blocking result, not a cosmetic one.
    // ======================================================================
    msg("----- positive controls: these MUST succeed or the desktop is broken -----");

    // D5  traverse /CONFIG to a deliberately-0644 file. Proves the directory is
    //     still SEARCHABLE, which is what separates 0711 from 0700. pwd.c/grp.c
    //     read these for every uid->name lookup in the system (Files owner
    //     column, msh prompt, whoami, id, and the compositor's own home
    //     resolution), so losing this breaks the desktop in a dozen places.
    {
        long len; unsigned long dig;
        long rc = read_digest("/CONFIG/PASSWD", &len, &dig);
        res("D5", "read /CONFIG/PASSWD through /CONFIG (0644, traversal control)", rc, 0);
        if (rc >= 0) note("D5 PASSWD bytes", len);
    }

    // D7  write inside our own home. Before #679's create-time ownership this
    //     failed for every non-root process anywhere, including its own home,
    //     because a path with no entry defaults to root-owned. That made "no
    //     non-root session can persist any state" the real blocker to this
    //     whole change, so it is checked on every run.
    {
        static const char payload[] = "nrprobe home write control\n";
        res("D7", "pkg_write /HOME/ADMIN/NRPOK.TXT (own home, must SUCCEED)",
            syscall3(SYS_PKG_WRITE, (long)"/HOME/ADMIN/NRPOK.TXT", (long)payload, 27), 0);
    }

    msg("===== end of probe =====");
    return 0;
}
