// ESCROW4T - #246/#305 end-to-end verifier for STAGE 4 (CAP_SCOPE_DEVICE) of the
// kernel-enforced AI escrow (docs/CONTRACT_ENFORCEMENT_PLAN.md, Stage 4).
//
// It is spawned ALREADY MARKED + LOCKED + DEVICE-BOUND by the kernel trusted
// spawner (escrow_spawn_marked, from proc/escrow4_vmtest.c), scoped to a REAL
// mounted removable volume. So from its first instruction it is a mandatory
// escrow actor whose grant is bound to a validated device identity, and it
// proves the end-to-end ALLOW:
//   (b) in-scope write on the CORRECT device SUCCEEDS - a live, device-scoped
//       grant it never asked for permits a write inside the removable volume;
//       an out-of-scope write (root fs, off the device) is REFUSED (both the
//       path scope AND the device scope forbid it), and a delete is REDIRECTED
//       into the Recycle Bin rather than destroying the file.
// The DIFFERENT-device and BadUSB fail-closed verdicts are deterministic and are
// proven by the in-kernel escdev_selftest(); a Ring-3 app cannot swap the
// physical device under itself.
//
// [usbescrow] THE FOUR NON-DELETE DESTRUCTION ROUTES, ON THE REMOVABLE VOLUME.
//
// The delete arm above has been proven on a stick since 2026-10-03. A delete is
// only ONE of the ways a marked actor can destroy a file, and the other four do
// not look like deletes at any layer:
//
//   a truncating open  open(victim, O_WRONLY|O_TRUNC)   every byte goes
//   an ftruncate       ftruncate(fd, shorter)           the tail goes
//   a rename over      rename(other, victim)            the victim goes
//   a partial write    write() into existing bytes      those bytes go
//
// Each arm below CREATES its own victim on the device with known contents,
// performs exactly one of those four operations, and asserts that THE
// DESTRUCTION LANDED. It deliberately does NOT assert the capture itself: that
// is the KERNEL's statement, and the kernel makes it on the serial line
// ("[ESCROW] PRE-IMAGE kept ... route=... path=..."), which is what
// tools/escrow-vmtest/run-escrow-vmtests.sh judges. An arm that passed because
// the operation was refused would prove nothing about the pre-image, so each
// arm requires the destruction to have actually happened.
//
// THE BIN-INDEX ORACLE (the second, independent check) answers the question the
// owner actually asked: can the user get the file back. Each arm also asserts
// that the Recycle Bin index carries a row naming the victim's ABSOLUTE path on
// the device, and that the kept copy still holds the original bytes. The
// absolute path matters on its own: a row filed under a volume-RELATIVE name
// ("/UT.TXT") would restore a thumbdrive file into the root directory.
//
// It uses the SHARED index primitive (libc/rbindex.h), which is where "the ONE
// definition of where the Recycle Bin lives and what a row means" already
// lives, so this prover cannot disagree with Files or the Recycle Bin app about
// either. rbindex_use_session_home() first, because this runs as uid 0 on
// behalf of a session and its own uid's answer would be the wrong bin.
//
// Its verdict goes to stdout (serial via /dev/console) AND /BOOTLOG.TXT.
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/syscall.h"
#include "../../libc/rbindex.h"

#define OUTF "/ESCROW4_OFF.TMP"    // OUT-OF-SCOPE + off-device (root fs)

// The known pre-image every non-delete arm writes, so "the bytes survived" is a
// byte comparison and not a size comparison. 64 bytes: comfortably more than
// one FAT16 cluster worth of nothing, small enough to compare in a stack buffer.
#define PRELEN 64
static char g_pre[PRELEN + 1];

static void say(const char *s) { printf("%s", s); sys_bootlog(s); }
static const char *verdict(int ok) { return ok ? "PASS" : "FAIL"; }
static void line(const char *label, int ok) {
    char l[192]; snprintf(l, sizeof(l), "ESCROW4T: %s %s\n", label, verdict(ok)); say(l);
}

static long r_open_w(const char *p) {
    return syscall2(SYS_OPEN, (long)p, (long)(O_CREAT | O_WRONLY | O_TRUNC));
}
static long r_creat(const char *p) {
    long fd = r_open_w(p);
    if (fd >= 0) { const char *m = "e4\n"; syscall3(SYS_WRITE, fd, (long)m, 3); syscall1(SYS_CLOSE, fd); }
    return fd;
}
static long r_unlink(const char *p) { return syscall1(SYS_UNLINK, (long)p); }
static int exists(const char *p) {
    long fd = syscall2(SYS_OPEN, (long)p, (long)O_RDONLY);
    if (fd >= 0) { syscall1(SYS_CLOSE, fd); return 1; }
    return 0;
}

// Read a whole small file. Returns the byte count, or -1 if it would not open.
static long slurp(const char *p, char *buf, long cap) {
    long fd = syscall2(SYS_OPEN, (long)p, (long)O_RDONLY);
    if (fd < 0) return -1;
    long n = syscall3(SYS_READ, fd, (long)buf, cap);
    syscall1(SYS_CLOSE, fd);
    if (n < 0) n = 0;
    return n;
}

// Lay down a victim with the known pre-image. Returns 1 on success.
// O_CREAT|O_TRUNC on a path that does not exist destroys nothing, so this
// setup step never itself trips a capture hook.
static int plant(const char *p) {
    (void)r_unlink(p);                       // a leftover would skew the arm
    long fd = syscall2(SYS_OPEN, (long)p, (long)(O_CREAT | O_WRONLY | O_TRUNC));
    if (fd < 0) return 0;
    long w = syscall3(SYS_WRITE, fd, (long)g_pre, PRELEN);
    syscall1(SYS_CLOSE, fd);
    if (w != PRELEN) return 0;
    char back[PRELEN + 8];
    return slurp(p, back, PRELEN) == PRELEN && !memcmp(back, g_pre, PRELEN);
}

// ---------------------------------------------------------------------------
// THE BIN-INDEX ORACLE
//
// A row in the bin index is "<name in the bin>|<the file's ABSOLUTE original
// path>|<deleted_unix>|<actor>". Finding the row whose original path is exactly
// the victim's absolute path, then reading the kept copy back and comparing it
// to the bytes that were there, is the whole of "the user can get it back",
// checked from Ring 3 with no privileged help.
//
// rbindex_find_by_original() is the lookup the shared primitive already
// provides for precisely this question ("a caller knows where the file used to
// be, not what the bin called it"), so there is no parser here.
// ---------------------------------------------------------------------------
static int bin_holds(const char *abspath) {
    rb_row_t row;
    if (rbindex_load() < 0) return 0;
    if (!rbindex_find_by_original(abspath, &row)) return 0;
    if (!row.name[0]) return 0;
    char kept[RB_PATH_MAX + RB_NAME_MAX + 2];
    snprintf(kept, sizeof(kept), "%s/%s", rbindex_dir(), row.name);
    char back[PRELEN + 8];
    if (slurp(kept, back, PRELEN) != PRELEN) return 0;
    if (memcmp(back, g_pre, PRELEN) != 0) return 0;
    // The actor token must say an AI did it: that is the join key to the
    // GraphFS journal entry for the same event, and a row filed as "user"
    // would mean the kernel attributed an AI destruction to the human.
    return (row.actor[0] == 'a' && row.actor[1] == 'i' && row.actor[2] == ':');
}

// Report an arm: the destruction landed, AND the bytes are recoverable under
// the right absolute path, attributed to an AI actor.
static int arm(const char *label, int destroyed, const char *abspath) {
    int kept = bin_holds(abspath);
    char l[256];
    snprintf(l, sizeof(l), "ESCROW4T: %s destroyed=%s recoverable=%s %s\n",
             label, destroyed ? "yes" : "no", kept ? "yes" : "no",
             verdict(destroyed && kept));
    say(l);
    return destroyed && kept;
}

int main(void) {
    say("\n========== ESCROW4T (#246 Stage 4 CAP_SCOPE_DEVICE) ==========\n");

    for (int i = 0; i < PRELEN; i++) g_pre[i] = (char)('A' + (i % 26));
    g_pre[PRELEN] = '\0';

    // Read the device-scoped grant's scope (the removable mount point), which
    // the launcher published. Reads are not gated.
    char scope[64]; scope[0] = '\0';
    long fd = syscall2(SYS_OPEN, (long)"/CONFIG/ESCROW4.SCOPE", (long)O_RDONLY);
    if (fd >= 0) {
        long n = syscall3(SYS_READ, fd, (long)scope, (long)(sizeof(scope) - 1));
        syscall1(SYS_CLOSE, fd);
        if (n < 0) n = 0;
        scope[n] = '\0';
        // trim trailing whitespace/newline
        while (n > 0 && (scope[n - 1] == '\n' || scope[n - 1] == '\r' || scope[n - 1] == ' '))
            scope[--n] = '\0';
    }
    if (!scope[0]) {
        say("ESCROW4T: could not read /CONFIG/ESCROW4.SCOPE\n");
        say("ESCROW4T: OVERALL FAIL\n");
        return 1;
    }
    { char l[128]; snprintf(l, sizeof(l), "ESCROW4T: device-scoped to %s\n", scope); say(l); }

    // uid 0 on behalf of a session: resolve the bin against the SESSION's home,
    // not this process's own, which is what the shared primitive documents for
    // exactly this case. Must precede the first rbindex_dir()/rbindex_path().
    rbindex_use_session_home();
    { char l[256];
      snprintf(l, sizeof(l), "ESCROW4T: bin-index oracle at %s\n", rbindex_path());
      say(l); }

    char inpath[96];
    snprintf(inpath, sizeof(inpath), "%s/e4.txt", scope);

    // (b) in-scope write on the CORRECT device -> ALLOWED.
    int b_in = (r_creat(inpath) >= 0);
    int b_here = exists(inpath);
    line("in-scope write on the bound device ALLOWED", b_in && b_here);

    // out-of-scope + off-device write -> REFUSED (path scope and device scope).
    int b_out = (r_open_w(OUTF) < 0) && !exists(OUTF);
    line("out-of-scope/off-device write REFUSED", b_out);

    // -----------------------------------------------------------------------
    // [usbescrow] the four non-delete destruction routes, on the device.
    // 8.3 upper-case names: the synthetic stick is FAT16 and a short name
    // removes any long-name handling from the question being asked.
    // -----------------------------------------------------------------------
    char p_t[112], p_s[112], p_w[112], p_r1[112], p_r2[112];
    snprintf(p_t,  sizeof(p_t),  "%s/UT.TXT",  scope);
    snprintf(p_s,  sizeof(p_s),  "%s/US.TXT",  scope);
    snprintf(p_w,  sizeof(p_w),  "%s/UW.TXT",  scope);
    snprintf(p_r1, sizeof(p_r1), "%s/UR1.TXT", scope);
    snprintf(p_r2, sizeof(p_r2), "%s/UR2.TXT", scope);

    // ARM T: a TRUNCATING OPEN. O_TRUNC without O_CREAT, so the file must
    // already be there: this is the one destruction that is fully declared at
    // open time. Destroyed = the file is now empty.
    int t_ok = 0;
    if (!plant(p_t)) say("ESCROW4T: could not plant UT.TXT\n");
    else {
        long tfd = syscall2(SYS_OPEN, (long)p_t, (long)(O_WRONLY | O_TRUNC));
        if (tfd >= 0) syscall1(SYS_CLOSE, tfd);
        char back[PRELEN + 8];
        long left = slurp(p_t, back, PRELEN);
        t_ok = arm("truncating open on the bound device", tfd >= 0 && left == 0, p_t);
    }

    // ARM S: an ftruncate() SHRINK on an already-open descriptor. The open
    // carries no O_TRUNC at all, so nothing at open time could have seen it
    // coming. Destroyed = the file is now 8 bytes.
    int s_ok = 0;
    if (!plant(p_s)) say("ESCROW4T: could not plant US.TXT\n");
    else {
        long sfd = syscall2(SYS_OPEN, (long)p_s, (long)O_WRONLY);
        long frc = -1;
        if (sfd >= 0) {
            frc = syscall2(SYS_FTRUNCATE, sfd, (long)8);
            syscall1(SYS_CLOSE, sfd);
        }
        char back[PRELEN + 8];
        long left = slurp(p_s, back, PRELEN);
        s_ok = arm("ftruncate shrink on the bound device", frc == 0 && left == 8, p_s);
    }

    // ARM W: a PARTIAL WRITE into bytes that already exist. No O_TRUNC, seek
    // into the middle, write fewer bytes than the file holds. Destroyed = the
    // overwritten window changed and the rest did not.
    int w_ok = 0;
    if (!plant(p_w)) say("ESCROW4T: could not plant UW.TXT\n");
    else {
        long wfd = syscall2(SYS_OPEN, (long)p_w, (long)O_WRONLY);
        long wrc = -1;
        if (wfd >= 0) {
            if (syscall3(SYS_SEEK, wfd, (long)8, (long)SEEK_SET) >= 0)
                wrc = syscall3(SYS_WRITE, wfd, (long)"zzzz", 4);
            syscall1(SYS_CLOSE, wfd);
        }
        char back[PRELEN + 8];
        long left = slurp(p_w, back, PRELEN);
        int changed = (left == PRELEN) && !memcmp(back + 8, "zzzz", 4) &&
                      !memcmp(back, g_pre, 8);
        w_ok = arm("partial write on the bound device", wrc == 4 && changed, p_w);
    }

    // ARM R: a RENAME OVER an existing file. The victim is the DESTINATION,
    // which the rename silently deletes; by the time it reaches the filesystem
    // nothing in the call looks like a deletion.
    //
    // [renamevol] 2026-10-04: THIS IS NOW A CAPTURE ARM LIKE THE OTHER THREE,
    // AND IT USED TO BE A NON-DESTRUCTION ARM. The reason it could not be one
    // is written in blame.md and was measured: sys_rename() performed every
    // rename as fat_rename(&g_fat_fs, old, new), and g_fat_fs does not route a
    // hot-plugged volume, so a rename on a stick never reached the medium.
    // There was therefore nothing to capture, and this arm could only assert
    // that the victim's bytes were still there - which it did, honestly,
    // rather than implying an end-to-end proof it did not have.
    //
    // sys_rename now routes both ends through hotplug_volume_for_path() and
    // performs the rename on the volume that serves them. So a rename over a
    // file on a stick DOES destroy the destination, which makes it the SIXTH
    // destruction route a marked actor has, and this arm is what proves the
    // escrow capture covers it: the destruction must land AND the displaced
    // file must come back out of the Recycle Bin under its absolute path on
    // the device with an AI actor token, exactly as for the other four.
    int r_ok = 0;
    if (!plant(p_r2)) say("ESCROW4T: could not plant UR2.TXT\n");
    else {
        (void)r_unlink(p_r1);
        long rfd = syscall2(SYS_OPEN, (long)p_r1, (long)(O_CREAT | O_WRONLY | O_TRUNC));
        int planted_src = 0;
        if (rfd >= 0) {
            planted_src = (syscall3(SYS_WRITE, rfd, (long)"src-bytes", 9) == 9);
            syscall1(SYS_CLOSE, rfd);
        }
        long rrc = planted_src ? syscall2(SYS_RENAME, (long)p_r1, (long)p_r2) : -1;
        // DESTROYED means three things at once, because any one of them alone
        // has a false-pass: the call succeeded, the source name is gone (a
        // COPY would leave it), and the destination now holds the SOURCE's
        // bytes rather than its own (a rename that silently did nothing would
        // leave the 64-byte pre-image in place and still return 0 on a kernel
        // that reported success without acting).
        char back[PRELEN + 8];
        long left = slurp(p_r2, back, PRELEN);
        int replaced = (left == 9) && !memcmp(back, "src-bytes", 9);
        int src_gone = !exists(p_r1);
        int destroyed = planted_src && rrc == 0 && src_gone && replaced;
        char l[256];
        snprintf(l, sizeof(l),
                 "ESCROW4T: rename over detail rc=%ld src-gone=%s "
                 "dst-has-src-bytes=%s dst-bytes=%ld\n",
                 rrc, src_gone ? "yes" : "no", replaced ? "yes" : "no", left);
        say(l);
        r_ok = arm("rename over a file on the bound device", destroyed, p_r2);
    }

    // any delete -> REFUSED (no-delete invariant), and the in-scope file remains.
    // 2026-10-03: a delete SUCCEEDS and the kernel redirects the bytes into the
    // user's Recycle Bin. On a REMOVABLE device the bin still lives in the
    // session user's home on the ROOT filesystem, so the capture crosses from
    // the device to the root volume; that is deliberate (the bin is one place
    // the user already knows) and it is why the delete can succeed here at all.
    int b_del = (r_unlink(inpath) == 0) && !exists(inpath);
    line("delete on the bound device REDIRECTED to the recycle bin", b_del);

    int overall = b_in && b_here && b_out && b_del && t_ok && s_ok && w_ok && r_ok;
    say(overall ? "ESCROW4T: OVERALL PASS\n" : "ESCROW4T: OVERALL FAIL\n");
    say("========== ESCROW4T END ==========\n");
    return overall ? 0 : 1;
}
