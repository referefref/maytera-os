// PKGRDR - [provetwo] #246: the Ring-3 prover for the two [volaudit]
// permission fixes that shipped in golden 2575 PROVEN TO COMPILE AND ROUTE and
// NOT proven by a multi-user test on a volume.
//
//   SYS_PKG_WRITE (301)      -> pkg_write_permit()   kernel/proc/syscall.c
//   SYS_SPAWN_REDIR (247)    -> open_redir_file()    kernel/proc/syscall.c
//
// Both decided "create or overwrite?" with fat_exists(&g_fat_fs, path), which
// cannot see a hot-plugged volume, so on a stick both took the CREATE arm and
// authorized against the PARENT DIRECTORY instead of against the FILE. Anyone
// who could write into a folder on a stick could blank a file in it they had
// no right to touch. Both now ask recycle_victim_stat(), the one volume-aware
// existence oracle.
//
// ==========================================================================
// EVERY CHECK IS A PAIR, AND THAT IS NOT A STYLE PREFERENCE
// ==========================================================================
// This exact trap fired twice in two days in this subsystem. [renamevol]
// nearly shipped a false pass: its security test passed on the BROKEN kernel
// because the rename was refused by a check on the SOURCE file before it ever
// reached the one under test, and only a companion arm that had to SUCCEED
// exposed it. [openperm] then found that enforcement without the matching
// ownership change would have made a stick read-only to the person who plugged
// it in, caught only by its allow arms.
//
// So every arm here comes in a pair: the SAME uid, the SAME directory, the
// SAME medium, the SAME syscall with the SAME flags, differing ONLY in who
// owns the target.
//
//   P1 DENY  pkg_write over uid 1001's file   must FAIL  + victim byte-intact
//   P2 ALLOW pkg_write over uid 1000's file   must SUCCEED + new bytes land
//   R1 DENY  redirect over uid 1001's file    must FAIL  + victim byte-intact
//   R2 ALLOW redirect over uid 1000's file    must SUCCEED + helper tag lands
//
// P2 IS ALSO THE ANSWER TO THE WRONG-GATE HAZARD, which is why the previous
// two agents did not claim SYS_PKG_WRITE. That syscall sits behind three OTHER
// refusals, all of which also return -1:
//
//     escrow_fs_guard(ESCROW_OP_WRITE, ...)     the AI escrow scope decision
//     pkg_path_is_boot()                        /BOOT, /EFI, /KERNEL.ELF
//     escrow_replace_victim()                   the pre-image capture
//
// A test that only shows REFUSED cannot tell which of the four refused, so it
// is worth nothing as evidence about the one under test. P2 issues the SAME
// syscall from the SAME uid in the SAME directory on the SAME medium and must
// SUCCEED with its bytes byte-verified on the medium. A green P2 proves, by
// execution rather than by reading the code, that escrow allowed, the boot
// path check allowed, the existence oracle answered, the pre-image capture
// allowed and the writer wrote. Only after that does a red P1 locate itself:
// the sole thing that changed between the two calls is which uid owns the
// target, and pkg_write_permit() is the only code that reads that.
//
// AND THE REDIRECTION GATE IS RING-3 REACHABLE, which is the other reason it
// went unproven. open_redir_file() is NOT only reachable from the kernel
// shell. spawn_impl() calls it, and Ring 3 reaches spawn_impl() through
// SYS_SPAWN_REDIR (247) and SYS_SPAWN_ENV (394), both with libc wrappers.
// `cmd > file` in the kernel shell is one caller; a Ring-3 process asking for
// its child's stdout to be a file is another, and it carries the uid the gate
// is about. R1/R2 take that route.
//
// Each arm is also run against /PKR on the ROOT filesystem as a CONTROL. A red
// control means this prover or its fixture is broken, not the kernel.
//
// Verdicts go to stdout (serial via /dev/console) AND /BOOTLOG.TXT.
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/syscall.h"

// kernel/proc/syscall.h:242. No libc wrapper exists; appstore, updated and
// sgprobe each spell the number locally the same way. NOT guessed: grepped.
#ifndef SYS_PKG_WRITE
#define SYS_PKG_WRITE 301
#endif

// The 64-byte pre-image every fixture file holds, built by
// kernel/proc/pkgrdr_vmtest.c. "The victim is intact" is a BYTE comparison and
// never a size comparison: a file truncated to 0 and a file still holding its
// bytes differ in length, but a file whose bytes were REWRITTEN IN PLACE does
// not, and SYS_PKG_WRITE with a 64-byte payload would be exactly that case.
#define PRELEN 64
static char g_pre[PRELEN + 1];

// The identities the arms are judged on. Must match PKGRDR_UID_ME /
// PKGRDR_UID_OTHER in kernel/proc/pkgrdr_vmtest.c.
#define UID_ME    1000u
#define UID_OTHER 1001u

// What a successful pkg_write puts on the medium. Deliberately NOT 64 bytes
// and deliberately not a prefix of the pre-image, so "the new bytes landed"
// and "the old bytes survived" are mutually exclusive answers.
#define NEWLEN 16
static const char NEWBYTES[NEWLEN] = { 'P','K','G','W','R','I','T','E',
                                       '-','N','E','W','D','A','T','A' };

// Must match TAG in userland/apps/PKGRDRH/main.c.
#define REDIR_TAGLEN 16
static const char REDIR_TAG[REDIR_TAGLEN] = { 'P','K','G','R','D','R','H','-',
                                              'R','E','D','I','R','-','O','K' };

#define HELPER "/APPS/PKGRDRH"

static int g_fails = 0;

static void say(const char *s) { printf("%s", s); sys_bootlog(s); }
static const char *verdict(int ok) { return ok ? "PASS" : "FAIL"; }

// Report one arm. Everything this prover claims goes through here, so a
// failure cannot be reported in a shape the harness does not grep for.
static void arm(const char *label, int ok, const char *detail) {
    char l[384];
    snprintf(l, sizeof(l), "PKGRDR: %s %s %s\n", label, detail ? detail : "", verdict(ok));
    say(l);
    if (!ok) g_fails++;
}

static long r_open(const char *p, long flags) {
    return syscall2(SYS_OPEN, (long)p, flags);
}
static void r_close(long fd) { (void)syscall1(SYS_CLOSE, fd); }
static long r_unlink(const char *p) { return syscall1(SYS_UNLINK, (long)p); }

// Read up to `cap` bytes. Returns the count, or -1 if the file would not open.
// -1 and 0 are DIFFERENT answers here and are never collapsed.
static long slurp(const char *p, char *buf, long cap) {
    long fd = r_open(p, O_RDONLY);
    if (fd < 0) return -1;
    long n = syscall3(SYS_READ, fd, (long)buf, cap);
    r_close(fd);
    if (n < 0) n = 0;
    return n;
}

// Does this path still hold the exact 64-byte pre-image? Any other answer,
// including an unreadable file, is "no".
static int holds_pre(const char *p) {
    char back[PRELEN + 8];
    if (slurp(p, back, PRELEN) != PRELEN) return 0;
    return memcmp(back, g_pre, PRELEN) == 0;
}

// Does this path hold exactly `want`? Used by both allow arms, so "it
// succeeded" is a statement about the medium and not about a return code.
static int holds_exactly(const char *p, const char *want, long wlen) {
    char back[128];
    if (wlen > (long)sizeof(back)) return 0;
    long n = slurp(p, back, (long)sizeof(back));
    return (n == wlen) && (memcmp(back, want, (size_t)wlen) == 0);
}

static long pkg_write(const char *p, const void *d, long n) {
    return syscall3(SYS_PKG_WRITE, (long)p, (long)d, n);
}

// Spawn HELPER with its stdout redirected to `out`. Returns the spawn rc.
// argv is a real NULL-terminated vector; argc 1.
static long spawn_redir_to(const char *out) {
    char *argv[2];
    argv[0] = (char *)HELPER;
    argv[1] = 0;
    return (long)sys_spawn_redir_noenv(HELPER, argv, 1, 0, out, 0);
}

// ---------------------------------------------------------------------------
// THE SUITE. Run once against a directory on the ROOT filesystem (the control)
// and once against a directory on the removable volume (the subject). ONE
// function, so the two cannot drift.
//
// `dir` must be an all-users STICKY directory (01777) owned by UID_OTHER
// holding four 64-byte files whose ownership the kernel launcher has written:
// PVIC/RVIC owned by UID_OTHER and PMINE/RMINE owned by UID_ME.
// Returns the number of FAILING arms in this medium.
// ---------------------------------------------------------------------------
static int perm_suite(const char *tag, const char *dir) {
    int before = g_fails;
    char pvic[128], pmine[128], rvic[128], rmine[128], pnew[128];
    char label[256], det[320];

    snprintf(pvic,  sizeof(pvic),  "%s/PVIC.TXT",  dir);
    snprintf(pmine, sizeof(pmine), "%s/PMINE.TXT", dir);
    snprintf(rvic,  sizeof(rvic),  "%s/RVIC.TXT",  dir);
    snprintf(rmine, sizeof(rmine), "%s/RMINE.TXT", dir);
    snprintf(pnew,  sizeof(pnew),  "%s/PNEW.TMP",  dir);

    // =======================================================================
    // P GROUP: SYS_PKG_WRITE. A full-file replacement of a named path in one
    // call, which is a DELETE of every byte that was there.
    // =======================================================================

    // ---- P1 DENY: replace a file owned by uid 1001.
    //
    // The whole point of the medium: in the 01777 directory the CREATE branch
    // would ALLOW this (write+search on the parent) and the OVERWRITE branch
    // DENIES it (no write on the file). The launcher printed both numbers.
    {
        long rc = pkg_write(pvic, NEWBYTES, NEWLEN);
        int intact = holds_pre(pvic);
        long len = -1;
        { char b[PRELEN + 8]; len = slurp(pvic, b, (long)sizeof(b)); }
        int ok = (rc != 0) && intact;
        snprintf(det, sizeof(det),
                 "path=%s uid=%u owner=%u pkg_write-rc=%ld readable-len=%ld victim-intact=%s",
                 pvic, UID_ME, UID_OTHER, rc, len, intact ? "yes" : "no");
        snprintf(label, sizeof(label),
                 "%s P1 DENY  SYS_PKG_WRITE replacing another user's file", tag);
        arm(label, ok, det);
    }

    // ---- P2 ALLOW: the same syscall, same directory, same medium, on a file
    // uid 1000 OWNS. THIS IS THE ARM THAT MAKES P1 MEAN ANYTHING: it proves
    // the escrow guard, the boot-path refusal, the existence oracle, the
    // pre-image capture and the writer all permit this call, so a P1 refusal
    // has only pkg_write_permit()'s permission arm left to come from.
    {
        long rc = pkg_write(pmine, NEWBYTES, NEWLEN);
        int landed = holds_exactly(pmine, NEWBYTES, NEWLEN);
        long len = -1;
        { char b[PRELEN + 8]; len = slurp(pmine, b, (long)sizeof(b)); }
        int ok = (rc == 0) && landed;
        snprintf(det, sizeof(det),
                 "path=%s uid=%u owner=%u pkg_write-rc=%ld readable-len=%ld "
                 "new-bytes-landed=%s still-holds-pre-image=%s",
                 pmine, UID_ME, UID_ME, rc, len, landed ? "yes" : "no",
                 holds_pre(pmine) ? "yes" : "no");
        snprintf(label, sizeof(label),
                 "%s P2 ALLOW SYS_PKG_WRITE replacing its own file "
                 "(ALSO the proof the path is authorized)", tag);
        arm(label, ok, det);
    }

    // ---- P3 USABILITY: the CREATE branch still works for an ordinary user in
    // a directory it may write. Guards the OPPOSITE mistake from P1: a gate
    // that refuses everything would make P1 green and SYS_PKG_WRITE useless.
    {
        (void)r_unlink(pnew);
        long rc = pkg_write(pnew, NEWBYTES, NEWLEN);
        int landed = holds_exactly(pnew, NEWBYTES, NEWLEN);
        int ok = (rc == 0) && landed;
        snprintf(det, sizeof(det),
                 "path=%s uid=%u (file did not exist) pkg_write-rc=%ld new-bytes-landed=%s",
                 pnew, UID_ME, rc, landed ? "yes" : "no");
        snprintf(label, sizeof(label),
                 "%s P3 USABILITY SYS_PKG_WRITE CREATING a new file in a "
                 "directory it may write", tag);
        arm(label, ok, det);
        (void)r_unlink(pnew);
    }

    // =======================================================================
    // R GROUP: SHELL OUTPUT REDIRECTION, reached from Ring 3 through
    // SYS_SPAWN_REDIR. The flags open_redir_file() receives are fixed by
    // spawn_impl(): O_WRONLY|O_CREAT|O_TRUNC for a non-append redirect.
    // =======================================================================

    // ---- R1 DENY: redirect a child's stdout ONTO a file owned by uid 1001.
    //
    // A refused spawn returns -1 and the child never exists, so nothing is
    // written. An ALLOWED one runs PKGRDRH, which writes its 16-byte tag over
    // the victim: destruction, measured as a byte comparison.
    //
    // The spawn is waited for when it succeeds, so the verdict is never read
    // before the child has had the chance to do the damage. Reporting
    // "intact" on a child that simply had not run yet is the one way this arm
    // could pass for the wrong reason.
    {
        long rc = spawn_redir_to(rvic);
        if (rc > 0) { int st = 0; (void)sys_waitpid((int)rc, &st, 0); }
        int intact = holds_pre(rvic);
        int tagged = holds_exactly(rvic, REDIR_TAG, REDIR_TAGLEN);
        int ok = (rc < 0) && intact;
        snprintf(det, sizeof(det),
                 "helper=%s out=%s uid=%u owner=%u spawn-rc=%ld victim-intact=%s "
                 "helper-tag-landed-on-the-victim=%s",
                 HELPER, rvic, UID_ME, UID_OTHER, rc,
                 intact ? "yes" : "no", tagged ? "yes" : "no");
        snprintf(label, sizeof(label),
                 "%s R1 DENY  SYS_SPAWN_REDIR stdout onto another user's file", tag);
        arm(label, ok, det);
    }

    // ---- R2 ALLOW: the same spawn, same helper, same directory, same medium,
    // onto a file uid 1000 OWNS. Same role as P2: it proves the spawn path
    // itself works (the ELF read, the X_OK check on the helper, the fd
    // install), so an R1 refusal is attributable to the redirect gate and not
    // to any of those.
    {
        long rc = spawn_redir_to(rmine);
        int st = 0;
        if (rc > 0) (void)sys_waitpid((int)rc, &st, 0);
        int landed = holds_exactly(rmine, REDIR_TAG, REDIR_TAGLEN);
        long len = -1;
        { char b[PRELEN + 8]; len = slurp(rmine, b, (long)sizeof(b)); }
        int ok = (rc > 0) && landed;
        snprintf(det, sizeof(det),
                 "helper=%s out=%s uid=%u owner=%u spawn-rc=%ld child-status=%d "
                 "readable-len=%ld helper-tag-landed=%s still-holds-pre-image=%s",
                 HELPER, rmine, UID_ME, UID_ME, rc, st, len,
                 landed ? "yes" : "no", holds_pre(rmine) ? "yes" : "no");
        snprintf(label, sizeof(label),
                 "%s R2 ALLOW SYS_SPAWN_REDIR stdout onto its own file "
                 "(ALSO the proof the spawn path works)", tag);
        arm(label, ok, det);
    }

    return g_fails - before;
}

// Verify the fixture BEFORE the privilege drop, as uid 0, by byte comparison.
static int check_fixture(const char *tag, const char *dir) {
    static const char *NAMES[4] = { "PVIC.TXT", "PMINE.TXT", "RVIC.TXT", "RMINE.TXT" };
    char p[160], missing[128];
    int bad = 0;
    missing[0] = '\0';
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, NAMES[i]);
        if (!holds_pre(p)) {
            bad++;
            if (strlen(missing) + strlen(NAMES[i]) + 2 < sizeof(missing)) {
                strcat(missing, NAMES[i]);
                strcat(missing, " ");
            }
        }
    }
    char det[256], label[192];
    snprintf(det, sizeof(det), "dir=%s files-with-the-64-byte-pattern=%d/4 bad=[%s]",
             dir, 4 - bad, missing);
    snprintf(label, sizeof(label), "%s FIXTURE four 64-byte files present and byte-correct", tag);
    arm(label, bad == 0, det);
    return bad == 0;
}

int main(void) {
    say("\n========== PKGRDR (#246 provetwo: are the SYS_PKG_WRITE and shell-redirect permission gates real on a mounted volume?) ==========\n");

    for (int i = 0; i < PRELEN; i++) g_pre[i] = (char)('A' + (i % 26));
    g_pre[PRELEN] = '\0';

    // ---- the helper BOTH redirect arms spawn. If it is absent, R1 goes green
    // for the wrong reason (the spawn fails on the ELF read, nowhere near the
    // gate) and R2 goes red for a reason that is not the kernel's. Checked
    // here, as uid 0, and reported as its own arm.
    {
        char det[160];
        long hl = -1;
        { char b[8]; hl = slurp(HELPER, b, 4); }
        snprintf(det, sizeof(det), "path=%s first-bytes-readable=%ld", HELPER, hl);
        arm("HELPER present (both redirect arms spawn it)", hl == 4, det);
    }

    // ---- find the removable volume through the SAME enumeration the Files
    // app and the tray use, so this cannot be looking at a different volume
    // than the UI would.
    sc_volume_t vols[SC_VOL_MAX];
    int nv = vol_list(vols, SC_VOL_MAX);
    char mnt[32]; mnt[0] = '\0';
    for (int i = 0; i < nv; i++) {
        unsigned f = vols[i].flags;
        if (!(f & MOSVOL_MOUNTED))   continue;
        if (!(f & MOSVOL_REMOVABLE)) continue;
        if (!(f & MOSVOL_READABLE))  continue;
        if (f & MOSVOL_READONLY)     continue;
        if (!vols[i].mount[0])       continue;
        strncpy(mnt, vols[i].mount, sizeof(mnt) - 1);
        mnt[sizeof(mnt) - 1] = '\0';
        char l[224];
        snprintf(l, sizeof(l),
                 "PKGRDR: volume %s fs=%s flags=0x%x (from vol_list, %d volumes total)\n",
                 mnt, vols[i].fsname, f, nv);
        say(l);
        break;
    }
    if (!mnt[0]) {
        char l[160];
        snprintf(l, sizeof(l),
                 "PKGRDR: NO mounted writable removable volume (vol_list returned %d)\n", nv);
        say(l);
        say("PKGRDR: OVERALL FAIL\n");
        say("========== PKGRDR END ==========\n");
        return 1;
    }

    char vdir[64];
    snprintf(vdir, sizeof(vdir), "%s/PK", mnt);
    const char *rdir = "/PKR";

    int f_root = check_fixture("ROOT-CONTROL", rdir);
    int f_vol  = check_fixture("VOLUME", vdir);

    // ---- the one-way privilege drop. Everything above ran as uid 0, for
    // which every gate under test is skipped by design (perms_check returns 0
    // on its first line for root, and pkg_write_permit returns 0 immediately
    // for a Ring-0 caller). sys_setuid lets root become anyone and lets nobody
    // become root, so this cannot be undone and must come after all setup.
    long surc = syscall1(SYS_SETUID, (long)UID_ME);
    long nowuid = syscall0(SYS_GETUID);
    {
        char det[160];
        snprintf(det, sizeof(det), "setuid(%u) rc=%ld getuid=%ld", UID_ME, surc, nowuid);
        arm("DROPPED to an unprivileged uid", surc == 0 && nowuid == (long)UID_ME, det);
    }

    if (!(f_root && f_vol && surc == 0 && nowuid == (long)UID_ME)) {
        say("PKGRDR: fixture or privilege drop failed; the arms would be meaningless. "
            "NOT running them.\n");
        say("PKGRDR: OVERALL FAIL\n");
        say("========== PKGRDR END ==========\n");
        return 1;
    }

    // ---- THE CONTROL FIRST, because a red control invalidates everything
    // after it and the reader should meet it before the subject.
    int rfail = perm_suite("ROOT-CONTROL", rdir);

    // ---- THE SUBJECT.
    int vfail = perm_suite("VOLUME", vdir);

    {
        char l[320];
        snprintf(l, sizeof(l),
                 "PKGRDR: ROOT-CONTROL=%s (%d failing) VOLUME=%s (%d failing) total-failing-arms=%d\n",
                 rfail == 0 ? "GREEN" : "RED", rfail,
                 vfail == 0 ? "GREEN" : "RED", vfail, g_fails);
        say(l);
    }
    say(g_fails == 0 ? "PKGRDR: OVERALL PASS\n" : "PKGRDR: OVERALL FAIL\n");
    say("========== PKGRDR END ==========\n");
    return g_fails == 0 ? 0 : 1;
}
