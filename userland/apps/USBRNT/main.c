// USBRNT - [renamevol] #246: can an ORDINARY, UNMARKED user change the
// directory of a hot-plugged removable volume at all, and does the
// destination-removal PERMISSION CHECK in sys_rename() actually run there?
//
// WHY THIS IS A SEPARATE PROVER FROM ESCROW4T
// ==========================================================================
// ESCROW4T is spawned ALREADY MARKED as an AI escrow actor, so every
// destructive syscall it makes is intercepted by the escrow guard before it
// reaches the filesystem. That makes it the wrong instrument for two questions:
//
//   1. "does rename() work on a USB stick for a NORMAL user" - a marked
//      actor's refusal and a filesystem's failure both come back as -1, so a
//      marked prover cannot tell them apart;
//   2. "does the PERMISSION check run" - a marked actor is uid 0 and the gate
//      under test is skipped entirely for uid 0 (perms_check returns 0 on its
//      first line for root), so the check can only be exercised by a prover
//      that drops to an unprivileged uid.
//
// So this one is launched UNMARKED (proc/usbrnt_vmtest.c uses the ordinary
// proc_create_user_as, not escrow_spawn_marked) and it drops to uid 1000 for
// its last two arms. It is TEST SCAFFOLDING: it is on
// build/unshipped-apps.list and its launcher is a no-op unless
// /CONFIG/USBRNT.TEST exists, so it is present on no shipping golden.
//
// WHAT EACH ARM MEASURES, AND WHY A CONTROL IS NOT OPTIONAL
// ==========================================================================
//   A1  rename to a NEW name on the volume.          The plain functional bug.
//   A2  rename OVER an existing file on the volume.  Replace, not duplicate.
//   A3  THE CONTROL: the same two renames on the ROOT filesystem. Without it,
//       a prover that was itself broken (bad syscall number, unwritable paths,
//       a stick that never mounted) would report A1/A2 red and look exactly
//       like the kernel defect. A3 must be GREEN in every run, before and
//       after any fix, or nothing else in this transcript means anything.
//   A4  mkdir on the volume.     The same volume-blindness, single-path form.
//   A5  unlink on the volume.    Likewise.
//   A6  rmdir on the volume.     Likewise.
//   A7  a CROSS-DEVICE rename (volume -> root) must be REFUSED with BOTH ends
//       untouched. A cross-device rename that half-succeeds is worse than one
//       that fails, because the user believes the file moved.
//   A8  THE SECURITY ARM. As uid 1000, in a STICKY directory on the volume
//       owned by uid 1001:
//         A8a  rename over ANOTHER user's file  ->  must be REFUSED, victim
//              byte-for-byte intact;
//         A8b  rename over ITS OWN file         ->  must be ALLOWED.
//       BOTH halves are required. fs/perms.c's own comment records why: a
//       canonicalization bug once denied EVERY removal in EVERY sticky
//       directory before the rule was consulted, and "it looked exactly like
//       the feature working" from the deny side alone. An arm that only
//       asserts the refusal also passes on a kernel where rename is simply
//       broken, which is the kernel this prover was written to measure.
//
// It finds the removable volume itself through vol_list() (SYS_VOL_LIST), the
// same enumeration the Files app and the tray use, so it cannot be looking at
// a different volume than the UI would. The sticky directory and its two
// pre-owned victims are prepared by the launcher and by the harness that builds
// the synthetic stick; this prover only exercises them.
//
// Its verdict goes to stdout (serial via /dev/console) AND /BOOTLOG.TXT.
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/syscall.h"

// The known pre-image. 64 bytes, the same pattern ESCROW4T uses and the same
// pattern the harness writes into the stick's pre-owned victims, so "the bytes
// survived" is a byte comparison and never a size comparison.
#define PRELEN 64
static char g_pre[PRELEN + 1];

// The unprivileged identities the security arm uses. 1000 is the prover after
// it drops privilege; 1001 is "another user" and owns the sticky directory and
// one of the two victims. Both are written into /CONFIG/PERMS.DB by the
// launcher, which runs as kernel code, so Ring 3 never has to be trusted to
// set up the facts the gate is judged on.
#define UID_ME    1000u
#define UID_OTHER 1001u

static int g_fails = 0;

static void say(const char *s) { printf("%s", s); sys_bootlog(s); }
static const char *verdict(int ok) { return ok ? "PASS" : "FAIL"; }

// Report one arm. Everything this prover claims goes through here, so a failure
// cannot be reported in a shape the harness does not grep for.
static int arm(const char *label, int ok, const char *detail) {
    char l[256];
    snprintf(l, sizeof(l), "USBRNT: %s %s %s\n", label, detail ? detail : "", verdict(ok));
    say(l);
    if (!ok) g_fails++;
    return ok;
}

static long r_unlink(const char *p) { return syscall1(SYS_UNLINK, (long)p); }

static int exists(const char *p) {
    long fd = syscall2(SYS_OPEN, (long)p, (long)O_RDONLY);
    if (fd >= 0) { syscall1(SYS_CLOSE, fd); return 1; }
    return 0;
}

static long slurp(const char *p, char *buf, long cap) {
    long fd = syscall2(SYS_OPEN, (long)p, (long)O_RDONLY);
    if (fd < 0) return -1;
    long n = syscall3(SYS_READ, fd, (long)buf, cap);
    syscall1(SYS_CLOSE, fd);
    if (n < 0) n = 0;
    return n;
}

// Lay down a file holding the known 64-byte pre-image. Returns 1 on success.
static int plant(const char *p) {
    (void)r_unlink(p);
    long fd = syscall2(SYS_OPEN, (long)p, (long)(O_CREAT | O_WRONLY | O_TRUNC));
    if (fd < 0) return 0;
    long w = syscall3(SYS_WRITE, fd, (long)g_pre, PRELEN);
    syscall1(SYS_CLOSE, fd);
    if (w != PRELEN) return 0;
    char back[PRELEN + 8];
    return slurp(p, back, PRELEN) == PRELEN && !memcmp(back, g_pre, PRELEN);
}

// Lay down a file holding `tag`, so that after a rename the DESTINATION can be
// proved to hold the SOURCE's bytes and not merely to exist.
static int plant_tag(const char *p, const char *tag) {
    (void)r_unlink(p);
    long fd = syscall2(SYS_OPEN, (long)p, (long)(O_CREAT | O_WRONLY | O_TRUNC));
    if (fd < 0) return 0;
    long n = (long)strlen(tag);
    long w = syscall3(SYS_WRITE, fd, (long)tag, n);
    syscall1(SYS_CLOSE, fd);
    return w == n;
}

static int holds_tag(const char *p, const char *tag) {
    char back[96];
    long n = (long)strlen(tag);
    if (n > (long)sizeof(back)) return 0;
    if (slurp(p, back, n) != n) return 0;
    return memcmp(back, tag, (unsigned long)n) == 0;
}

static int holds_pre(const char *p) {
    char back[PRELEN + 8];
    if (slurp(p, back, PRELEN) != PRELEN) return 0;
    return memcmp(back, g_pre, PRELEN) == 0;
}

// ---------------------------------------------------------------------------
// THE TWO-PART RENAME TEST, run once against a removable volume and once
// against the root filesystem. ONE function, so the control and the subject
// cannot drift apart: if the control passes and the subject fails, the only
// difference is the medium.
//
// `newname` is a rename to a name that does not exist. `over` is a rename onto
// a name that DOES exist, which both fat_rename() and ext2_rename() perform as
// a REPLACE, so the destination must end up holding the source's bytes and the
// source must be gone.
// ---------------------------------------------------------------------------
static int rename_pair(const char *tag, const char *a, const char *b,
                       const char *c, const char *d) {
    char det[224];

    // (i) a -> b, where b does not exist.
    int ok1 = 0;
    long rc1 = -1;
    if (!plant_tag(a, "SRC-ONE")) {
        snprintf(det, sizeof(det), "could-not-plant=%s", a);
    } else {
        (void)r_unlink(b);
        rc1 = syscall2(SYS_RENAME, (long)a, (long)b);
        int gone = !exists(a);
        int landed = holds_tag(b, "SRC-ONE");
        ok1 = (rc1 == 0) && gone && landed;
        snprintf(det, sizeof(det), "rc=%ld src-gone=%s dst-has-src-bytes=%s",
                 rc1, gone ? "yes" : "no", landed ? "yes" : "no");
    }
    {
        char l[288];
        snprintf(l, sizeof(l), "%s rename to a NEW name", tag);
        arm(l, ok1, det);
    }

    // (ii) c -> d, where d already exists and holds the known pre-image. The
    // replace must happen: d holding its OLD bytes afterwards means the rename
    // did nothing, and d holding BOTH names' bytes is impossible, but d
    // existing alongside a still-present c would mean a copy and not a move.
    int ok2 = 0;
    long rc2 = -1;
    if (!plant(d) || !plant_tag(c, "SRC-TWO")) {
        snprintf(det, sizeof(det), "could-not-plant=%s/%s", c, d);
    } else {
        rc2 = syscall2(SYS_RENAME, (long)c, (long)d);
        int gone = !exists(c);
        int replaced = holds_tag(d, "SRC-TWO");
        int still_old = holds_pre(d);
        ok2 = (rc2 == 0) && gone && replaced && !still_old;
        snprintf(det, sizeof(det),
                 "rc=%ld src-gone=%s dst-replaced=%s dst-still-old-bytes=%s",
                 rc2, gone ? "yes" : "no", replaced ? "yes" : "no",
                 still_old ? "yes" : "no");
    }
    {
        char l[288];
        snprintf(l, sizeof(l), "%s rename OVER an existing file", tag);
        arm(l, ok2, det);
    }

    return ok1 && ok2;
}

int main(void) {
    say("\n========== USBRNT (#246 renamevol: ordinary rename on a removable volume) ==========\n");

    for (int i = 0; i < PRELEN; i++) g_pre[i] = (char)('A' + (i % 26));
    g_pre[PRELEN] = '\0';

    // ---- find the removable volume through the SAME enumeration the UI uses.
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
        char l[192];
        snprintf(l, sizeof(l), "USBRNT: volume %s fs=%s flags=0x%x (from vol_list, %d volumes total)\n",
                 mnt, vols[i].fsname, f, nv);
        say(l);
        break;
    }
    if (!mnt[0]) {
        char l[128];
        snprintf(l, sizeof(l), "USBRNT: NO mounted writable removable volume (vol_list returned %d)\n", nv);
        say(l);
        say("USBRNT: OVERALL FAIL\n");
        say("========== USBRNT END ==========\n");
        return 1;
    }

    // ---- the paths. 8.3 upper case: the synthetic stick is FAT16 and a short
    // name removes long-name handling from every question being asked.
    char v1[64], v2[64], v3[64], v4[64], v5[64], v6[64], vd[64];
    snprintf(v1, sizeof(v1), "%s/RN1.TXT", mnt);
    snprintf(v2, sizeof(v2), "%s/RN2.TXT", mnt);
    snprintf(v3, sizeof(v3), "%s/RN3.TXT", mnt);
    snprintf(v4, sizeof(v4), "%s/RN4.TXT", mnt);
    snprintf(v5, sizeof(v5), "%s/RN5.TXT", mnt);
    snprintf(v6, sizeof(v6), "%s/RN6.TXT", mnt);
    snprintf(vd, sizeof(vd), "%s/RND", mnt);

    // ---- A3 FIRST, the control, because a red control invalidates the rest
    // and the reader should meet it before the subject.
    int a3 = rename_pair("A3 CONTROL on the ROOT filesystem:",
                         "/USBRNT1.TMP", "/USBRNT2.TMP",
                         "/USBRNT3.TMP", "/USBRNT4.TMP");
    (void)r_unlink("/USBRNT2.TMP"); (void)r_unlink("/USBRNT4.TMP");

    // ---- A1 + A2 on the removable volume.
    int a12 = rename_pair("A1/A2 on the removable volume:", v1, v2, v3, v4);

    // ---- A4 mkdir on the volume.
    {
        long rc = syscall2(SYS_MKDIR, (long)vd, (long)0755);
        // A directory opens as a descriptor on this system, so exists() is the
        // right question; its CONTENT is not.
        int present = exists(vd);
        char det[160];
        snprintf(det, sizeof(det), "rc=%ld present=%s", rc, present ? "yes" : "no");
        arm("A4 mkdir on the removable volume", rc == 0 && present, det);
    }

    // ---- A5 unlink on the volume.
    {
        int planted = plant(v5);
        long rc = planted ? r_unlink(v5) : -1;
        int gone = !exists(v5);
        char det[160];
        snprintf(det, sizeof(det), "planted=%s rc=%ld gone=%s",
                 planted ? "yes" : "no", rc, gone ? "yes" : "no");
        arm("A5 unlink on the removable volume", planted && rc == 0 && gone, det);
    }

    // ---- A6 rmdir on the volume (the directory A4 made; if A4 failed this is
    // reported as a dependency, not as a second independent failure).
    {
        int there = exists(vd);
        long rc = there ? syscall1(SYS_RMDIR, (long)vd) : -1;
        int gone = !exists(vd);
        char det[160];
        snprintf(det, sizeof(det), "dir-was-there=%s rc=%ld gone=%s",
                 there ? "yes" : "no", rc, gone ? "yes" : "no");
        arm("A6 rmdir on the removable volume", there && rc == 0 && gone, det);
    }

    // ---- A7 cross-device rename must be refused with BOTH ends untouched.
    {
        const char *root_dst = "/USBRNX.TMP";
        (void)r_unlink(root_dst);
        int planted = plant_tag(v6, "XDEV-SRC");
        long rc = planted ? syscall2(SYS_RENAME, (long)v6, (long)root_dst) : 0;
        int src_intact = holds_tag(v6, "XDEV-SRC");
        int dst_absent = !exists(root_dst);
        char det[192];
        snprintf(det, sizeof(det), "rc=%ld src-intact=%s dst-not-created=%s",
                 rc, src_intact ? "yes" : "no", dst_absent ? "yes" : "no");
        arm("A7 cross-device rename (volume -> root) REFUSED",
            planted && rc != 0 && src_intact && dst_absent, det);
        (void)r_unlink(root_dst);
    }

    // =======================================================================
    // A8 THE SECURITY ARM. Everything above ran as uid 0, for which the gate
    // under test is skipped by design. From here on the prover is uid 1000 and
    // CANNOT GO BACK (sys_setuid lets root become anyone and lets nobody become
    // root), so this is deliberately last.
    //
    // The sticky directory <mnt>/SH and its two victims are prepared outside
    // this process:
    //   <mnt>/SH          owned by uid 1001, mode 01777 (all-users + sticky)
    //   <mnt>/SH/VIC.TXT  owned by uid 1001 - ANOTHER user's file
    //   <mnt>/SH/OWN.TXT  owned by uid 1000 - THIS prover's own file
    // Both victims hold the known 64-byte pre-image.
    // =======================================================================
    char sh[64], vic[80], own[80], src1[80], src2[80];
    snprintf(sh,   sizeof(sh),   "%s/SH", mnt);
    snprintf(vic,  sizeof(vic),  "%s/SH/VIC.TXT", mnt);
    snprintf(own,  sizeof(own),  "%s/SH/OWN.TXT", mnt);
    snprintf(src1, sizeof(src1), "%s/SH/S1.TXT", mnt);
    snprintf(src2, sizeof(src2), "%s/SH/S2.TXT", mnt);

    int setup_ok = exists(sh) && holds_pre(vic) && holds_pre(own);
    {
        char det[224];
        snprintf(det, sizeof(det), "dir=%s present=%s vic-bytes=%s own-bytes=%s",
                 sh, exists(sh) ? "yes" : "no",
                 holds_pre(vic) ? "ok" : "bad", holds_pre(own) ? "ok" : "bad");
        arm("A8 SETUP sticky directory and its two pre-owned victims", setup_ok, det);
    }

    long surc = syscall1(SYS_SETUID, (long)UID_ME);
    long nowuid = syscall0(SYS_GETUID);
    {
        char det[160];
        snprintf(det, sizeof(det), "setuid(%u) rc=%ld getuid=%ld",
                 UID_ME, surc, nowuid);
        arm("A8 dropped to an unprivileged uid", surc == 0 && nowuid == (long)UID_ME, det);
    }

    if (setup_ok && surc == 0 && nowuid == (long)UID_ME) {
        // A8a: over ANOTHER user's file in a sticky directory. MUST be refused,
        // and the victim's 64 bytes must still be there.
        int p1 = plant_tag(src1, "ATTACK-BYTES");
        long rc_a = p1 ? syscall2(SYS_RENAME, (long)src1, (long)vic) : 0;
        int vic_intact = holds_pre(vic);
        char det[224];
        snprintf(det, sizeof(det),
                 "planted=%s rc=%ld victim-intact=%s victim-holds-attack-bytes=%s",
                 p1 ? "yes" : "no", rc_a, vic_intact ? "yes" : "no",
                 holds_tag(vic, "ATTACK-BYTES") ? "YES" : "no");
        arm("A8a uid 1000 rename over ANOTHER user's file in a sticky dir REFUSED",
            p1 && rc_a != 0 && vic_intact, det);

        // A8b: over ITS OWN file in the SAME directory. MUST be allowed. This
        // is the half that proves A8a passed because of the rule and not
        // because rename is broken or everything is refused.
        int p2 = plant_tag(src2, "MY-OWN-BYTES");
        long rc_b = p2 ? syscall2(SYS_RENAME, (long)src2, (long)own) : -1;
        int replaced = holds_tag(own, "MY-OWN-BYTES");
        char det2[224];
        snprintf(det2, sizeof(det2), "planted=%s rc=%ld own-file-replaced=%s",
                 p2 ? "yes" : "no", rc_b, replaced ? "yes" : "no");
        arm("A8b uid 1000 rename over ITS OWN file in the same sticky dir ALLOWED",
            p2 && rc_b == 0 && replaced, det2);
    } else {
        arm("A8a uid 1000 rename over ANOTHER user's file in a sticky dir REFUSED",
            0, "skipped: setup or setuid failed");
        arm("A8b uid 1000 rename over ITS OWN file in the same sticky dir ALLOWED",
            0, "skipped: setup or setuid failed");
    }

    {
        char l[192];
        snprintf(l, sizeof(l), "USBRNT: control=%s subject=%s failing-arms=%d\n",
                 a3 ? "GREEN" : "RED", a12 ? "GREEN" : "RED", g_fails);
        say(l);
    }
    say(g_fails == 0 ? "USBRNT: OVERALL PASS\n" : "USBRNT: OVERALL FAIL\n");
    say("========== USBRNT END ==========\n");
    return g_fails == 0 ? 0 : 1;
}
