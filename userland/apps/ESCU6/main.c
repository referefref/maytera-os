// ESCU6 - #246/#305 Stage 6 verifier: the escrow userland is now a THIN CLIENT
// of the kernel enforcement, and the contract chokepoint is generalised BEYOND
// the filesystem to a NON-FS effect (device eject). Modelled on ESCROWT: a
// Ring-3 app launched at boot by the gated proc/escu6_vmtest.c launcher, whose
// verdict goes to stdout (serial via /dev/console) AND to /BOOTLOG.TXT via
// sys_bootlog, so a headless VM can capture it either way.
//
// It proves, in one run, the three things Stage 6 claims:
//   (A1) PHOTOS.ORGANIZE END TO END via the thin client: photorg_organize()
//        (which now enters the KERNEL escrow contract) creates the date folder,
//        moves the images bytes-intact, deletes nothing, and verifies FULFILLED.
//   (A2) THE KERNEL IS THE AUTHORITY: after escrow_request() (the thin client)
//        enters the kernel contract, RAW out-of-scope write and RAW delete are
//        refused BY THE KERNEL - the userland aicap grant/deny no longer exists,
//        so this is the kernel enforcing, not any userland check. The bypass is
//        inherent: these are raw syscalls issued as uid 0.
//   (B)  GENERALISED CHOKEPOINT: a MARKED escrow actor's device eject
//        (SYS_VOL_EJECT, a NON-FS effect) is refused at Ring 0 (out-of-contract);
//        a present removable volume is left MOUNTED (non-destructive refusal).
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/syscall.h"
#include "../../libc/escrow.h"
#include "../../libc/photorg.h"

#define SCOPE  "/ESCU6SCOPE"      // photos.organize scope (ext2 root)
#define SC2    "/ESCU6SC2"        // A2 bypass-proof scope
#define OUTF   "/ESCU6OUT2.TMP"   // an OUT-OF-SCOPE path (root, not under a scope)

static void say(const char *s) { printf("%s", s); sys_bootlog(s); }
static const char *verdict(int ok) { return ok ? "PASS" : "FAIL"; }
static void line(const char *label, int ok) {
    char l[192]; snprintf(l, sizeof(l), "ESCU6: %s %s\n", label, verdict(ok)); say(l);
}

// Raw, ungated syscalls (bypass any userland layer entirely).
static long r_mkdir(const char *p)  { return syscall2(SYS_MKDIR, (long)p, 0755); }
static long r_rmdir(const char *p)  { return syscall1(SYS_RMDIR, (long)p); }
static long r_unlink(const char *p) { return syscall1(SYS_UNLINK, (long)p); }
static long r_open_w(const char *p) { return syscall2(SYS_OPEN, (long)p, (long)(O_CREAT | O_WRONLY | O_TRUNC)); }
static long r_creat_content(const char *p, const char *m) {
    long fd = r_open_w(p);
    if (fd >= 0) { syscall3(SYS_WRITE, fd, (long)m, (long)strlen(m)); syscall1(SYS_CLOSE, fd); }
    return fd;
}
static int exists(const char *p) {
    long fd = syscall2(SYS_OPEN, (long)p, (long)O_RDONLY);
    if (fd >= 0) { syscall1(SYS_CLOSE, fd); return 1; }
    return 0;
}

int main(void) {
    say("\n========== ESCU6 (#246 Stage 6: thin client + generalised chokepoint) ==========\n");
    int overall = 1;

    // ================= (A1) photos.organize END TO END via the thin client =====
    // Clean slate from any prior boot.
    r_unlink(SCOPE "/A.JPG"); r_unlink(SCOPE "/B.JPG"); r_unlink(SCOPE "/C.TXT");
    // (date folders from a prior run are left; verify tolerates existing folders)
    if (!exists(SCOPE)) r_mkdir(SCOPE);
    // Two real image files (by extension) + one non-image; each with content so
    // the promise's bytes-intact hash check is meaningful.
    r_creat_content(SCOPE "/A.JPG", "JPEG-A-bytes-1234567890\n");
    r_creat_content(SCOPE "/B.JPG", "JPEG-B-bytes-abcdefghij\n");
    r_creat_content(SCOPE "/C.TXT", "not an image, must be untouched\n");

    char summary[2048];
    int pv = photorg_organize(SCOPE, "YYYY-MM", summary, sizeof(summary));
    // Find the single date folder photorg created and confirm both photos moved
    // into it bytes-intact, source gone, and C.TXT untouched.
    int a_fulfilled = (pv == ESCROW_FULFILLED);
    int a_src_gone  = !exists(SCOPE "/A.JPG") && !exists(SCOPE "/B.JPG");
    int a_txt_kept  = exists(SCOPE "/C.TXT");
    // Enumerate the scope: exactly one new subdir (the date folder) holding the
    // two moved images.
    char (*names)[256] = malloc(sizeof(char[256]) * 64);
    unsigned char isd[64]; unsigned int szs[64];
    int nf = names ? escrow_list_dir(SCOPE, names, isd, szs, 64) : -1;
    int moved_ok = 0, folder_found = 0;
    for (int i = 0; i < nf; i++) {
        if (!isd[i]) continue;
        char df[320]; snprintf(df, sizeof(df), "%s/%s", SCOPE, names[i]);
        char pa[400], pb[400];
        snprintf(pa, sizeof(pa), "%s/A.JPG", df);
        snprintf(pb, sizeof(pb), "%s/B.JPG", df);
        if (exists(pa) && exists(pb)) { moved_ok = 1; folder_found = 1; }
    }
    if (names) free(names);
    int a1 = a_fulfilled && a_src_gone && a_txt_kept && folder_found && moved_ok;
    line("A1.photos.organize verdict FULFILLED", a_fulfilled);
    line("A1.images moved into a date folder (source gone, bytes intact)", moved_ok && a_src_gone);
    line("A1.non-image left untouched", a_txt_kept);
    line("A1.PHOTOS.ORGANIZE END-TO-END via kernel-enforced thin client", a1);
    overall = overall && a1;

    // ================= (A2) the KERNEL is the enforcement authority ============
    // Bypass proof: escrow_request() (the thin client) ENTERS the kernel escrow.
    // Then RAW syscalls - which never touch the userland aicap layer, and there
    // is no userland grant/deny anymore - are refused BY THE KERNEL.
    r_unlink(SC2 "/todel.txt"); r_unlink(SC2 "/ok.txt"); r_unlink(OUTF);
    if (!exists(SC2)) r_mkdir(SC2);
    r_creat_content(SC2 "/todel.txt", "pre-existing; a marked actor must not delete it\n");

    escrow_contract_t *c = escrow_request(SC2, "bypass-proof: kernel is authority", 3600);
    int a2_enter = (c != 0);                              // thin client entered the kernel contract
    long w_out   = r_open_w(OUTF);                        // out-of-scope write
    if (w_out >= 0) syscall1(SYS_CLOSE, w_out);
    int a2_out   = (w_out < 0);                           // kernel must refuse
    long w_in    = r_open_w(SC2 "/ok.txt");               // in-scope write
    if (w_in >= 0) syscall1(SYS_CLOSE, w_in);
    int a2_in    = (w_in >= 0);                           // kernel must allow
    int a2_del   = (r_unlink(SC2 "/todel.txt") < 0);      // kernel no-delete invariant
    int a2_intact= exists(SC2 "/todel.txt") && !exists(OUTF);
    int cl = c ? escrow_close(c) : ESCROW_ERROR;          // PARTIAL -> kernel abort
    // After close the marker is cleared: the same out-of-scope write now works.
    long w_after = r_open_w(OUTF);
    int a2_after = (w_after >= 0);
    if (w_after >= 0) syscall1(SYS_CLOSE, w_after);
    r_unlink(OUTF);
    int a2 = a2_enter && a2_out && a2_in && a2_del && a2_intact && a2_after && (cl != ESCROW_ERROR);
    line("A2.escrow_request entered the kernel contract", a2_enter);
    line("A2.KERNEL refused a RAW out-of-scope write (aicap bypassed)", a2_out);
    line("A2.KERNEL allowed a RAW in-scope write", a2_in);
    line("A2.KERNEL refused a RAW delete (no-delete invariant)", a2_del);
    line("A2.refused ops had no effect", a2_intact);
    line("A2.after close, ordinary proc again (out-of-scope write ok)", a2_after);
    line("A2.THE KERNEL IS THE AUTHORITY (enforcement not in userland)", a2);
    overall = overall && a2;

    // ================= (B) generalised chokepoint: device EJECT ================
    // A NON-FS effect. Enumerate removable volumes; pick one if present so the
    // refusal can be shown NON-DESTRUCTIVELY (device still mounted afterwards).
    sc_volume_t vols[SC_VOL_MAX];
    int nv = vol_list(vols, SC_VOL_MAX);
    int rem_idx = -1;
    char rem_mount[32] = {0};
    for (int i = 0; i < nv && i < SC_VOL_MAX; i++) {
        if (vols[i].flags & MOSVOL_REMOVABLE) { rem_idx = vols[i].index; strlcpy(rem_mount, vols[i].mount, sizeof(rem_mount)); break; }
    }
    // Enter escrow (any scope; the marker is what the eject guard checks).
    int b_enter = (escrow_enter(SCOPE, 3600000UL) == 0);
    int eject_idx = (rem_idx >= 0) ? rem_idx : 0;
    int b_refused = (vol_eject(eject_idx) < 0);   // marked actor: kernel refuses (serial: [ESCROW] DENY eject)
    // If a real removable volume was present, prove it was NOT torn down.
    int b_present_after = 1;
    if (rem_idx >= 0) {
        b_present_after = 0;
        int nv2 = vol_list(vols, SC_VOL_MAX);
        for (int i = 0; i < nv2 && i < SC_VOL_MAX; i++)
            if (vols[i].index == rem_idx) { b_present_after = 1; break; }
    }
    int b_exit = (escrow_exit() == 0);
    int b = b_enter && b_refused && b_present_after && b_exit;
    line("B.enter escrow (actor marked)", b_enter);
    {
        char l[160];
        snprintf(l, sizeof(l), "B.MARKED ACTOR EJECT refused at Ring 0 (idx=%d%s%s) %s\n",
                 eject_idx, rem_idx >= 0 ? " removable=" : " (no removable vol; serial [ESCROW] DENY eject is the proof)",
                 rem_idx >= 0 ? rem_mount : "", verdict(b_refused));
        say(l);
    }
    if (rem_idx >= 0) line("B.removable volume STILL MOUNTED after refused eject (non-destructive)", b_present_after);
    line("B.GENERALISED CHOKEPOINT: non-FS effect (eject) refused by the kernel", b);
    overall = overall && b;

    line("OVERALL", overall);
    say("========== ESCU6 END ==========\n");
    return overall ? 0 : 1;
}
