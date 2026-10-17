// USBOPN - [openperm] #246: DO THE FILE-OPEN PATH AND THE DELETE PATH AGREE
// ABOUT PERMISSIONS ON A MOUNTED REMOVABLE VOLUME?
//
// THE ESCALATION THIS MEASURES
// ==========================================================================
// Two subsystems disagreed, and the disagreement was found by reading code,
// not by measuring:
//
//   - proc/fdlayer.c sys_open_k() has a DELIBERATE carve-out (#250). The whole
//     permission block is gated on `hotplug_resolve_path(path, 0) < 0`, so for
//     a path on a mounted removable volume perms_check() is never called at
//     all. The stated reasoning is that a FAT stick carries no POSIX
//     ownership, and that leaving removable paths in would make a stick
//     read-only to the user who plugged it in.
//
//   - proc/syscall.c sys_unlink(), sys_rmdir() and sys_rename() have NO such
//     carve-out. Since [renamevol] (716367de) they also reach the volume, so
//     their permission checks are live there, sticky rule included.
//
// The reported shape is therefore: on a stick you may be REFUSED permission to
// DELETE another user's file and PERMITTED to OPEN it with O_TRUNC and blank
// it. If that is true, the delete protection on removable media is decorative.
// sys_open_k()'s own comment makes the argument: "a rule that stops you
// unlinking somebody's file but lets you blank it is not a rule."
//
// This prover settles it on a booted machine.
//
// WHY EVERY ARM COMES IN AN ALLOW-AND-DENY PAIR, AND WHY THAT IS NOT OPTIONAL
// ==========================================================================
// fs/perms.c records a canonicalization bug (`!= 0` where `< 0` was meant)
// that denied EVERY removal in EVERY sticky directory before the rule was ever
// consulted, and its own comment says it "looked exactly like the feature
// working" from the deny side alone. [renamevol] then nearly shipped a false
// pass the same way: a deny-only arm passes just as well on a kernel where the
// operation is simply broken for an unrelated reason.
//
// So nothing here is asserted in one direction. Every verb is exercised twice
// by the SAME code, as uid 1000, in the SAME directory, differing ONLY in who
// owns the target file:
//
//   on ANOTHER user's file (uid 1001) -> must be REFUSED
//   on THIS user's own file (uid 1000) -> must be ALLOWED
//
// A refusal with no matching allow proves nothing. An allow with no matching
// refusal proves nothing.
//
// WHY THE WHOLE SUITE RUNS TWICE, ON TWO MEDIA, FROM ONE FUNCTION
// ==========================================================================
// perm_suite() is called once with a directory on the ROOT filesystem and once
// with a directory on the removable volume. The root run is the CONTROL: the
// root filesystem has no carve-out, so it must be GREEN in every run, before
// and after any fix. If the control is red, the prover or the fixture is
// broken and nothing else in the transcript means anything. Because it is one
// function, the control and the subject cannot drift apart: if the control
// passes and the subject fails, the ONLY difference is the medium.
//
// THE TWO USABILITY ARMS, WHICH GUARD AGAINST THE OPPOSITE MISTAKE
// ==========================================================================
// The carve-out's concern is legitimate and is stated as an arm rather than
// argued about. perms_check()'s no-entry default is root-owned 0755, which
// denies W_OK to every non-root process, so simply deleting the carve-out
// would make a USB stick unwritable by the user who plugged it in. That would
// be a worse bug than the one being fixed.
//
//   V8  as uid 1000, CREATE a brand new file in the volume ROOT      -> ALLOW
//   V9  as uid 1000, create and then DELETE a file in the shared dir -> ALLOW
//
// Any fix that turns these red is the wrong fix.
//
// WHAT PREPARES THE FIXTURE, AND WHY NOT THIS PROCESS
// ==========================================================================
// Who owns each file, who owns the directory, and that the directory carries
// S_ISVTX are facts in /CONFIG/PERMS.DB, which Ring 3 cannot write. They are
// written by kernel/proc/usbopn_vmtest.c, as kernel code, so the setup cannot
// itself be a Ring-3 capability the arms are implicitly also testing. This
// process only exercises them, and it verifies the fixture before trusting it.
//
// TEST SCAFFOLDING. On build/unshipped-apps.list; its launcher is a no-op
// unless /CONFIG/USBOPN.TEST exists, so it is present on no shipping golden.
//
// Verdicts go to stdout (serial via /dev/console) AND /BOOTLOG.TXT.
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/syscall.h"

// The known pre-image. 64 bytes of 'A'+i%26, the same pattern ESCROW4T and
// USBRNT use and the same pattern the harness writes into every fixture file,
// so "the victim is intact" is a BYTE comparison and never a size comparison.
// A file truncated to 0 and a file still holding its bytes differ in length,
// but a file whose bytes were REWRITTEN in place does not, and the plain-write
// arm below is exactly that case.
#define PRELEN 64
static char g_pre[PRELEN + 1];

// The identities the arms are judged on. 1000 is this prover after its
// one-way privilege drop; 1001 is "another user" and owns the shared directory
// and every victim. These must match the constants in
// kernel/proc/usbopn_vmtest.c; a mismatch would make every deny arm pass for
// the wrong reason, so both sides name them explicitly.
#define UID_ME    1000u
#define UID_OTHER 1001u

static int g_fails = 0;

static void say(const char *s) { printf("%s", s); sys_bootlog(s); }
static const char *verdict(int ok) { return ok ? "PASS" : "FAIL"; }

// Report one arm. Everything this prover claims goes through here, so a
// failure cannot be reported in a shape the harness does not grep for.
static void arm(const char *label, int ok, const char *detail) {
    char l[320];
    snprintf(l, sizeof(l), "USBOPN: %s %s %s\n", label, detail ? detail : "", verdict(ok));
    say(l);
    if (!ok) g_fails++;
}

static long r_open(const char *p, long flags) {
    return syscall2(SYS_OPEN, (long)p, flags);
}
static void r_close(long fd) { (void)syscall1(SYS_CLOSE, fd); }
static long r_unlink(const char *p) { return syscall1(SYS_UNLINK, (long)p); }

// Read up to `cap` bytes. Returns the count, or -1 if the file would not open.
// -1 and 0 are DIFFERENT answers here and are never collapsed: "I was refused
// the read" and "the file is empty" are the two outcomes several arms have to
// tell apart.
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

// How long is this file, as far as an ordinary read can tell? -1 means it
// would not open at all.
static long readable_len(const char *p) {
    char back[PRELEN + 8];
    return slurp(p, back, (long)sizeof(back));
}

static int exists(const char *p) {
    long fd = r_open(p, O_RDONLY);
    if (fd >= 0) { r_close(fd); return 1; }
    return 0;
}

// ---------------------------------------------------------------------------
// THE SUITE. Run once against a directory on the ROOT filesystem (the control)
// and once against a directory on the removable volume (the subject). ONE
// function, so the two cannot drift.
//
// `dir` must be an all-users STICKY directory (01777) owned by UID_OTHER,
// holding ten 64-byte files whose ownership the kernel launcher has written.
// Returns the number of FAILING arms in this medium.
// ---------------------------------------------------------------------------
static int perm_suite(const char *tag, const char *dir) {
    int before = g_fails;
    char ovic1[128], omine1[128], ovic2[128], omine2[128];
    char uvic[128], umine[128], wvic[128], wmine[128], svic[128], smine[128];
    char label[224], det[288];

    snprintf(ovic1,  sizeof(ovic1),  "%s/OVIC1.TXT",  dir);
    snprintf(omine1, sizeof(omine1), "%s/OMINE1.TXT", dir);
    snprintf(ovic2,  sizeof(ovic2),  "%s/OVIC2.TXT",  dir);
    snprintf(omine2, sizeof(omine2), "%s/OMINE2.TXT", dir);
    snprintf(uvic,   sizeof(uvic),   "%s/UVIC.TXT",   dir);
    snprintf(umine,  sizeof(umine),  "%s/UMINE.TXT",  dir);
    snprintf(wvic,   sizeof(wvic),   "%s/WVIC.TXT",   dir);
    snprintf(wmine,  sizeof(wmine),  "%s/WMINE.TXT",  dir);
    snprintf(svic,   sizeof(svic),   "%s/SVIC.TXT",   dir);
    snprintf(smine,  sizeof(smine),  "%s/SMINE.TXT",  dir);

    // -----------------------------------------------------------------------
    // T GROUP: O_TRUNC, THE VERB THE ESCALATION IS ABOUT.
    //
    // Emptying a file is replacing it. Two flag shapes are exercised, each
    // with its own deny-and-allow pair, because they reach the gate by
    // different derivations:
    //
    //   O_RDONLY|O_TRUNC (0x200) - the shape #prewrite was written for. The
    //     access derivation reads O_WRONLY and O_RDWR, so without the explicit
    //     `access |= W_OK` for O_TRUNC this asks for R_OK alone and every
    //     merely-READABLE file in the system can be blanked.
    //   O_WRONLY|O_TRUNC (0x201) - the ordinary shape. W_OK is asked for
    //     either way, so this one isolates the carve-out from the derivation.
    //
    // A deny arm that only used one shape could pass because the OTHER shape
    // is the one that gets through.
    // -----------------------------------------------------------------------

    // T1 DENY: a READ-ONLY open that destroys, on another user's file.
    {
        long fd = r_open(ovic1, O_RDONLY | O_TRUNC);
        if (fd >= 0) r_close(fd);
        int intact = holds_pre(ovic1);
        long len = readable_len(ovic1);
        snprintf(det, sizeof(det),
                 "flags=O_RDONLY|O_TRUNC owner=%u open-rc=%ld victim-intact=%s len-now=%ld",
                 UID_OTHER, fd, intact ? "yes" : "no", len);
        snprintf(label, sizeof(label),
                 "%s T1 DENY  uid %u O_RDONLY|O_TRUNC on ANOTHER user's file", tag, UID_ME);
        arm(label, fd < 0 && intact, det);
    }

    // T2 ALLOW: the same flags, the same directory, my own file. This is the
    // half that proves T1 was refused by the RULE and not because a
    // read-only truncating open is simply broken or everything is refused.
    {
        long fd = r_open(omine1, O_RDONLY | O_TRUNC);
        if (fd >= 0) r_close(fd);
        long len = readable_len(omine1);
        snprintf(det, sizeof(det),
                 "flags=O_RDONLY|O_TRUNC owner=%u open-rc=%ld len-now=%ld (want 0: it truncated)",
                 UID_ME, fd, len);
        snprintf(label, sizeof(label),
                 "%s T2 ALLOW uid %u O_RDONLY|O_TRUNC on ITS OWN file", tag, UID_ME);
        arm(label, fd >= 0 && len == 0, det);
    }

    // T3 DENY: the ordinary write-truncate shape, on another user's file.
    {
        long fd = r_open(ovic2, O_WRONLY | O_TRUNC);
        if (fd >= 0) r_close(fd);
        int intact = holds_pre(ovic2);
        long len = readable_len(ovic2);
        snprintf(det, sizeof(det),
                 "flags=O_WRONLY|O_TRUNC owner=%u open-rc=%ld victim-intact=%s len-now=%ld",
                 UID_OTHER, fd, intact ? "yes" : "no", len);
        snprintf(label, sizeof(label),
                 "%s T3 DENY  uid %u O_WRONLY|O_TRUNC on ANOTHER user's file", tag, UID_ME);
        arm(label, fd < 0 && intact, det);
    }

    // T4 ALLOW: same flags, my own file.
    {
        long fd = r_open(omine2, O_WRONLY | O_TRUNC);
        if (fd >= 0) r_close(fd);
        long len = readable_len(omine2);
        snprintf(det, sizeof(det),
                 "flags=O_WRONLY|O_TRUNC owner=%u open-rc=%ld len-now=%ld (want 0: it truncated)",
                 UID_ME, fd, len);
        snprintf(label, sizeof(label),
                 "%s T4 ALLOW uid %u O_WRONLY|O_TRUNC on ITS OWN file", tag, UID_ME);
        arm(label, fd >= 0 && len == 0, det);
    }

    // -----------------------------------------------------------------------
    // U GROUP: unlink(). THE OTHER SIDE OF THE DISAGREEMENT.
    //
    // This is the verb that is already enforced on both media. It is measured
    // here, in the SAME boot and the SAME directory as the T group, so the
    // comparison the escalation is about is a side-by-side pair of lines in
    // one transcript rather than a claim about two subsystems.
    // -----------------------------------------------------------------------

    // U1 DENY: another user's file in a sticky directory.
    {
        long rc = r_unlink(uvic);
        int intact = holds_pre(uvic);
        snprintf(det, sizeof(det), "owner=%u unlink-rc=%ld victim-intact=%s",
                 UID_OTHER, rc, intact ? "yes" : "no");
        snprintf(label, sizeof(label),
                 "%s U1 DENY  uid %u unlink ANOTHER user's file", tag, UID_ME);
        arm(label, rc != 0 && intact, det);
    }

    // U2 ALLOW: my own file in the same directory. Without this, U1 also
    // passes on a kernel where unlink is broken on this medium, which is
    // precisely the kernel [renamevol] was written to measure.
    {
        long rc = r_unlink(umine);
        int gone = !exists(umine);
        snprintf(det, sizeof(det), "owner=%u unlink-rc=%ld gone=%s",
                 UID_ME, rc, gone ? "yes" : "no");
        snprintf(label, sizeof(label),
                 "%s U2 ALLOW uid %u unlink ITS OWN file", tag, UID_ME);
        arm(label, rc == 0 && gone, det);
    }

    // -----------------------------------------------------------------------
    // W GROUP: a PLAIN writable open, no O_TRUNC.
    //
    // The carve-out skips the WHOLE permission block, not only the O_TRUNC
    // contribution to it, so truncation is the loudest form of the hole and
    // not the only one: an O_WRONLY open with no O_TRUNC lets a caller
    // REWRITE another user's bytes in place. That leaves the file the same
    // length, so it is the one arm where "intact" has to be a byte
    // comparison, which is why every fixture file holds a known pattern.
    // -----------------------------------------------------------------------

    // W1 DENY: rewrite another user's bytes in place.
    {
        static const char ATTACK[8] = { 'P','W','N','E','D','!','!','!' };
        long fd = r_open(wvic, O_WRONLY);
        long w = -1;
        if (fd >= 0) { w = syscall3(SYS_WRITE, fd, (long)ATTACK, 8); r_close(fd); }
        int intact = holds_pre(wvic);
        char back[PRELEN + 8];
        long n = slurp(wvic, back, PRELEN);
        int defaced = (n >= 8) && !memcmp(back, ATTACK, 8);
        snprintf(det, sizeof(det),
                 "flags=O_WRONLY owner=%u open-rc=%ld wrote=%ld victim-intact=%s "
                 "victim-holds-attack-bytes=%s",
                 UID_OTHER, fd, w, intact ? "yes" : "no", defaced ? "YES" : "no");
        snprintf(label, sizeof(label),
                 "%s W1 DENY  uid %u O_WRONLY (no trunc) on ANOTHER user's file", tag, UID_ME);
        arm(label, fd < 0 && intact && !defaced, det);
    }

    // W2 ALLOW: rewrite my OWN bytes in place, same flags, same directory.
    {
        static const char MINE[8] = { 'M','I','N','E','-','O','K','!' };
        long fd = r_open(wmine, O_WRONLY);
        long w = -1;
        if (fd >= 0) { w = syscall3(SYS_WRITE, fd, (long)MINE, 8); r_close(fd); }
        char back[PRELEN + 8];
        long n = slurp(wmine, back, PRELEN);
        int took = (n >= 8) && !memcmp(back, MINE, 8);
        snprintf(det, sizeof(det),
                 "flags=O_WRONLY owner=%u open-rc=%ld wrote=%ld my-bytes-landed=%s len-now=%ld",
                 UID_ME, fd, w, took ? "yes" : "no", n);
        snprintf(label, sizeof(label),
                 "%s W2 ALLOW uid %u O_WRONLY (no trunc) on ITS OWN file", tag, UID_ME);
        arm(label, fd >= 0 && w == 8 && took, det);
    }

    // -----------------------------------------------------------------------
    // S GROUP: READING a file that is not world-readable.
    //
    // The carve-out skips R_OK too, so the hole is not only an integrity hole.
    // Every other fixture file is 0644 and world-readable, so no read arm
    // above can see this: these two are mode 0600. SVIC is another user's
    // private file and must not be readable; SMINE is mine and must be.
    //
    // NOTE on what this arm can and cannot assert: once the gate is enforced,
    // an unprivileged prover cannot read SVIC to prove it is intact, so the
    // assertion is on the open return code alone. S2 is what shows that a
    // 0600 file is readable BY ITS OWNER, i.e. that S1's refusal is the mode
    // being enforced and not reads being broken on this medium.
    // -----------------------------------------------------------------------

    // S1 DENY: another user's 0600 file.
    {
        long fd = r_open(svic, O_RDONLY);
        long n = -1;
        char back[PRELEN + 8];
        if (fd >= 0) { n = syscall3(SYS_READ, fd, (long)back, PRELEN); r_close(fd); }
        int leaked = (n == PRELEN) && !memcmp(back, g_pre, PRELEN);
        snprintf(det, sizeof(det),
                 "mode=0600 owner=%u open-rc=%ld bytes-read=%ld content-disclosed=%s",
                 UID_OTHER, fd, n, leaked ? "YES" : "no");
        snprintf(label, sizeof(label),
                 "%s S1 DENY  uid %u READ ANOTHER user's 0600 file", tag, UID_ME);
        arm(label, fd < 0 && !leaked, det);
    }

    // S2 ALLOW: my own 0600 file.
    {
        int ok = holds_pre(smine);
        snprintf(det, sizeof(det), "mode=0600 owner=%u read-back-64-bytes=%s",
                 UID_ME, ok ? "yes" : "no");
        snprintf(label, sizeof(label),
                 "%s S2 ALLOW uid %u READ ITS OWN 0600 file", tag, UID_ME);
        arm(label, ok, det);
    }

    return g_fails - before;
}

// ---------------------------------------------------------------------------
// Verify the fixture before trusting it, while still uid 0.
//
// A fixture file that is missing or 0 bytes would make a deny arm pass for the
// wrong reason: there would be nothing to destroy. So all ten files in each
// directory are byte-checked here, as root, before any privilege drop. uid 0
// can read the two 0600 files, which the unprivileged prover deliberately
// cannot.
// ---------------------------------------------------------------------------
static int check_fixture(const char *tag, const char *dir) {
    static const char *NAMES[10] = {
        "OVIC1.TXT", "OMINE1.TXT", "OVIC2.TXT", "OMINE2.TXT",
        "UVIC.TXT", "UMINE.TXT", "WVIC.TXT", "WMINE.TXT",
        "SVIC.TXT", "SMINE.TXT"
    };
    int bad = 0;
    char p[128], missing[192];
    missing[0] = '\0';
    for (int i = 0; i < 10; i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, NAMES[i]);
        if (!holds_pre(p)) {
            bad++;
            if (strlen(missing) + strlen(NAMES[i]) + 2 < sizeof(missing)) {
                strcat(missing, NAMES[i]);
                strcat(missing, " ");
            }
        }
    }
    char det[288], label[160];
    snprintf(det, sizeof(det), "dir=%s files-with-the-64-byte-pattern=%d/10 bad=[%s]",
             dir, 10 - bad, missing);
    snprintf(label, sizeof(label), "%s FIXTURE ten 64-byte files present and byte-correct", tag);
    arm(label, bad == 0, det);
    return bad == 0;
}

int main(void) {
    say("\n========== USBOPN (#246 openperm: does the OPEN path agree with the DELETE path on a removable volume?) ==========\n");

    for (int i = 0; i < PRELEN; i++) g_pre[i] = (char)('A' + (i % 26));
    g_pre[PRELEN] = '\0';

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
                 "USBOPN: volume %s fs=%s flags=0x%x (from vol_list, %d volumes total)\n",
                 mnt, vols[i].fsname, f, nv);
        say(l);
        break;
    }
    if (!mnt[0]) {
        char l[160];
        snprintf(l, sizeof(l),
                 "USBOPN: NO mounted writable removable volume (vol_list returned %d)\n", nv);
        say(l);
        say("USBOPN: OVERALL FAIL\n");
        say("========== USBOPN END ==========\n");
        return 1;
    }

    // The two fixture directories. /PMR is on the root filesystem, <mnt>/PM is
    // on the removable volume. Both are 01777 owned by UID_OTHER and hold the
    // same ten files; the kernel launcher made them.
    char vdir[64];
    snprintf(vdir, sizeof(vdir), "%s/PM", mnt);
    const char *rdir = "/PMR";

    int f_root = check_fixture("ROOT-CONTROL", rdir);
    int f_vol  = check_fixture("VOLUME", vdir);

    // ---- the one-way privilege drop. Everything above ran as uid 0, for
    // which every gate under test is skipped by design (perms_check returns 0
    // on its first line for root). sys_setuid lets root become anyone and lets
    // nobody become root, so this cannot be undone and must come after all
    // setup verification.
    long surc = syscall1(SYS_SETUID, (long)UID_ME);
    long nowuid = syscall0(SYS_GETUID);
    {
        char det[160];
        snprintf(det, sizeof(det), "setuid(%u) rc=%ld getuid=%ld", UID_ME, surc, nowuid);
        arm("DROPPED to an unprivileged uid", surc == 0 && nowuid == (long)UID_ME, det);
    }

    if (!(f_root && f_vol && surc == 0 && nowuid == (long)UID_ME)) {
        say("USBOPN: fixture or privilege drop failed; the arms would be meaningless. "
            "NOT running them.\n");
        say("USBOPN: OVERALL FAIL\n");
        say("========== USBOPN END ==========\n");
        return 1;
    }

    // ---- THE CONTROL FIRST, because a red control invalidates everything
    // after it and the reader should meet it before the subject.
    int rfail = perm_suite("ROOT-CONTROL", rdir);

    // ---- THE SUBJECT.
    int vfail = perm_suite("VOLUME", vdir);

    // =======================================================================
    // THE USABILITY ARMS. These guard against the OPPOSITE mistake: a fix that
    // enforces permissions on a stick by making the stick unusable. The
    // carve-out exists because perms_check()'s no-entry default is root-owned
    // 0755, which denies W_OK to every non-root process, so a mounted volume
    // with no ownership answer of its own is read-only to the person who
    // plugged it in. That is a worse bug than the one being fixed, and it is
    // measured here rather than argued about.
    // =======================================================================

    // V8: a brand new file in the VOLUME ROOT. Create, write, read back.
    {
        char p[64];
        snprintf(p, sizeof(p), "%s/OPNNEW.TMP", mnt);
        static const char TAG8[8] = { 'N','E','W','F','I','L','E','0' };
        (void)r_unlink(p);
        long fd = r_open(p, O_CREAT | O_WRONLY | O_TRUNC);
        long w = -1;
        if (fd >= 0) { w = syscall3(SYS_WRITE, fd, (long)TAG8, 8); r_close(fd); }
        char back[16];
        long n = slurp(p, back, 8);
        int ok = (fd >= 0) && (w == 8) && (n == 8) && !memcmp(back, TAG8, 8);
        char det[256];
        snprintf(det, sizeof(det),
                 "path=%s open-rc=%ld wrote=%ld read-back=%ld bytes-match=%s",
                 p, fd, w, n, (n == 8 && !memcmp(back, TAG8, 8)) ? "yes" : "no");
        arm("V8 USABILITY uid 1000 can CREATE a new file in the VOLUME ROOT", ok, det);
        (void)r_unlink(p);
    }

    // V9: create and then DELETE my own file in the shared directory on the
    // volume. The round trip, because a create that cannot be undone by its
    // own creator is the [renamevol] CREATE-POINT-3 defect returning: a file
    // made on a stick with no ownership row is judged root's and its own
    // creator cannot remove it.
    {
        char p[96];
        snprintf(p, sizeof(p), "%s/RT.TMP", vdir);
        (void)r_unlink(p);
        long fd = r_open(p, O_CREAT | O_WRONLY | O_TRUNC);
        long w = -1;
        if (fd >= 0) { w = syscall3(SYS_WRITE, fd, (long)g_pre, 8); r_close(fd); }
        long urc = r_unlink(p);
        int gone = !exists(p);
        int ok = (fd >= 0) && (w == 8) && (urc == 0) && gone;
        char det[256];
        snprintf(det, sizeof(det),
                 "path=%s open-rc=%ld wrote=%ld unlink-rc=%ld gone=%s",
                 p, fd, w, urc, gone ? "yes" : "no");
        arm("V9 USABILITY uid 1000 can CREATE then DELETE its own file on the volume", ok, det);
    }

    // ---- the verdict, with the control and the subject named separately so a
    // reader can tell "the kernel is wrong" from "the test is wrong".
    {
        char l[288];
        snprintf(l, sizeof(l),
                 "USBOPN: ROOT-CONTROL=%s (%d failing) VOLUME=%s (%d failing) total-failing-arms=%d\n",
                 rfail == 0 ? "GREEN" : "RED", rfail,
                 vfail == 0 ? "GREEN" : "RED", vfail, g_fails);
        say(l);
    }
    say(g_fails == 0 ? "USBOPN: OVERALL PASS\n" : "USBOPN: OVERALL FAIL\n");
    say("========== USBOPN END ==========\n");
    return g_fails == 0 ? 0 : 1;
}
