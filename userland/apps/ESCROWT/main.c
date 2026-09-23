// ESCROWT - #246/#305 verifier for the KERNEL-ENFORCED AI escrow (Stage 1,
// docs/CONTRACT_ENFORCEMENT_PLAN.md). Modelled on FDXTEST: a Ring-3 app,
// launched at boot by the gated proc/escrow_vmtest.c launcher, whose verdict
// goes to stdout (serial via /dev/console) AND to /BOOTLOG.TXT via sys_bootlog,
// so a headless VM can capture it either way.
//
// It proves, in one run, the four things the plan claims:
//   (a) NO REGRESSION FIRST: as an ORDINARY (unmarked) process it can freely
//       create / write / rename / delete, in scope and out. THE GATE.
//   (b) IN SCOPE under a grant: a marked actor can mkdir and move within scope.
//   (c) ENFORCEMENT: the SAME marked actor is refused, BY THE KERNEL, a delete
//       (no-delete invariant) and an out-of-scope write, issuing the RAW
//       syscall directly - the userland aicap/escrow layer is never called.
//   (d) AFTER CLOSE: escrow_exit() clears the marker and the same ops work again.
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/syscall.h"

#define SC   "/ESCROWSC"         // the granted scope prefix (ext2 root)
#define OUTF "/ESCROWOUT2.TMP"   // an OUT-OF-SCOPE path (root, not under SC)
#define OUTD "/ESCROWOUT_D"      // an OUT-OF-SCOPE dir

static void say(const char *s) { printf("%s", s); sys_bootlog(s); }
static const char *verdict(int ok) { return ok ? "PASS" : "FAIL"; }

// Raw, ungated syscalls (bypass the aicap advisory layer entirely).
static long r_mkdir(const char *p)  { return syscall2(SYS_MKDIR, (long)p, 0755); }
static long r_rmdir(const char *p)  { return syscall1(SYS_RMDIR, (long)p); }
static long r_unlink(const char *p) { return syscall1(SYS_UNLINK, (long)p); }
static long r_rename(const char *a, const char *b) { return syscall2(SYS_RENAME, (long)a, (long)b); }
static long r_open_w(const char *p) { return syscall2(SYS_OPEN, (long)p, (long)(O_CREAT | O_WRONLY | O_TRUNC)); }
static long r_creat(const char *p)  {
    long fd = r_open_w(p);
    if (fd >= 0) { const char *m = "x\n"; syscall3(SYS_WRITE, fd, (long)m, 2); syscall1(SYS_CLOSE, fd); }
    return fd;
}
static int exists(const char *p) {
    long fd = syscall2(SYS_OPEN, (long)p, (long)O_RDONLY);
    if (fd >= 0) { syscall1(SYS_CLOSE, fd); return 1; }
    return 0;
}
static void line(const char *label, int ok) {
    char l[192]; snprintf(l, sizeof(l), "ESCROWT: %s %s\n", label, verdict(ok)); say(l);
}

int main(void) {
    say("\n========== ESCROWT (#246 kernel escrow enforcement) ==========\n");

    // -- Phase A: NO REGRESSION as an ORDINARY process. THE GATE. ------------
    // best-effort clean slate from any prior boot
    r_unlink(SC "/a.txt"); r_unlink(SC "/orig.txt"); r_unlink(SC "/moved.txt");
    r_unlink(SC "/todel.txt"); r_unlink(SC "/2026/photo.jpg"); r_rmdir(SC "/2026");
    r_unlink(SC "/photo.jpg"); r_rmdir(SC "/edir"); r_unlink(OUTF); r_rmdir(OUTD);

    int a_mkdir  = (r_mkdir(SC) == 0) || exists(SC);
    int a_create = (r_creat(SC "/a.txt") >= 0);
    int a_orig   = (r_creat(SC "/orig.txt") >= 0);
    int a_rename = (r_rename(SC "/orig.txt", SC "/moved.txt") == 0);
    int a_unlink = (r_unlink(SC "/moved.txt") == 0);
    int a_out    = (r_creat(OUTF) >= 0);        // out-of-scope OK for a normal proc
    int a_outrm  = (r_unlink(OUTF) == 0);
    int noreg = a_mkdir && a_create && a_orig && a_rename && a_unlink && a_out && a_outrm;
    line("A.mkdir",  a_mkdir);
    line("A.create", a_create);
    line("A.rename", a_rename);
    line("A.unlink", a_unlink);
    line("A.out-of-scope-write(normal proc)", a_out);
    line("NO-REGRESSION(normal FS untouched)", noreg);

    // -- Phase B: set up fixtures for the escrow run (as ordinary proc) ------
    if (!exists(SC)) r_mkdir(SC);
    r_creat(SC "/photo.jpg");     // to be moved in-scope
    r_creat(SC "/todel.txt");     // actor will try to delete -> must be DENIED
    r_mkdir(SC "/edir");          // actor will try to rmdir -> must be DENIED

    // -- Phase C: ENTER escrow-actor mode, scoped to SC --------------------
    int e = escrow_enter(SC, 3600000UL);
    line("ENTER(rc==0)", e == 0);

    // -- Phase D: IN-SCOPE mutations must SUCCEED under the grant -----------
    int d_mkdir = (r_mkdir(SC "/2026") == 0);
    int d_move  = (r_rename(SC "/photo.jpg", SC "/2026/photo.jpg") == 0);
    int inscope = d_mkdir && d_move;
    line("IN-SCOPE.mkdir",  d_mkdir);
    line("IN-SCOPE.move",   d_move);
    line("IN-SCOPE(actor may write in scope)", inscope);

    // -- Phase E: ENFORCEMENT. The kernel MUST refuse these RAW syscalls. ---
    int e_del   = (r_unlink(SC "/todel.txt") < 0);        // no-delete invariant
    int e_rmdir = (r_rmdir(SC "/edir") < 0);              // no-delete invariant
    int e_outw  = (r_open_w(OUTF) < 0);                   // out-of-scope write
    int e_outmk = (r_mkdir(OUTD) < 0);                    // out-of-scope mkdir
    int e_outmv = (r_rename(SC "/2026/photo.jpg", "/ESCROWOUT_ph.jpg") < 0); // dst out of scope
    int denied  = e_del && e_rmdir && e_outw && e_outmk && e_outmv;
    // and the refused ops must NOT have taken effect
    int intact  = exists(SC "/todel.txt") && exists(SC "/edir") &&
                  !exists(OUTF) && exists(SC "/2026/photo.jpg");
    int enforce = denied && intact;
    line("ENFORCE.delete-refused",        e_del);
    line("ENFORCE.rmdir-refused",         e_rmdir);
    line("ENFORCE.out-of-scope-write-refused", e_outw);
    line("ENFORCE.out-of-scope-mkdir-refused", e_outmk);
    line("ENFORCE.out-of-scope-move-refused",  e_outmv);
    line("ENFORCE.refused-ops-had-no-effect",  intact);
    line("ENFORCEMENT(kernel refused raw delete+out-of-scope write)", enforce);

    // -- Phase F: EXIT (contract close) ------------------------------------
    int x = escrow_exit();
    line("EXIT(rc==0)", x == 0);

    // -- Phase G: after close, the marker is cleared -> ordinary proc again -
    int g_del  = (r_unlink(SC "/todel.txt") == 0);   // delete works again
    long gfd   = r_open_w(OUTF);
    int g_outw = (gfd >= 0);                          // out-of-scope write works again
    if (gfd >= 0) syscall1(SYS_CLOSE, gfd);
    r_unlink(OUTF);
    int postexit = g_del && g_outw;
    line("POST-EXIT.delete-ok",    g_del);
    line("POST-EXIT.out-of-scope-write-ok", g_outw);
    line("POST-EXIT(marker cleared, ordinary proc again)", postexit);

    // -- Phase G: Stage 5B ABORT auto-reverts a contract's reversible move ---
    r_unlink(SC "/g_src.txt"); r_unlink(SC "/g_dst.txt");
    r_creat(SC "/g_src.txt");                                   // ordinary create (pre-contract)
    int g_enter2 = (escrow_enter(SC, 3600000UL) == 0);
    int g_move2  = (r_rename(SC "/g_src.txt", SC "/g_dst.txt") == 0);   // recorded by the guard
    int g_moved  = exists(SC "/g_dst.txt") && !exists(SC "/g_src.txt");
    // Stage 5C: also mkdir in the SAME contract, captured live (confirmed) and
    // reverted by the same abort as the move.
    int g_md     = (r_mkdir(SC "/g_dir") == 0);
    int g_mddir  = exists(SC "/g_dir");
    int g_abort  = (escrow_abort() >= 0);                       // rollback + close (move + mkdir)
    int g_revert = exists(SC "/g_src.txt") && !exists(SC "/g_dst.txt"); // move undone
    int g_mdrev  = !exists(SC "/g_dir");                        // created dir removed by rollback
    int abrt = g_enter2 && g_move2 && g_moved && g_md && g_mddir && g_abort && g_revert && g_mdrev;
    line("ABORT.enter+in-scope-move+mkdir",  g_enter2 && g_move2 && g_moved && g_md && g_mddir);
    line("ABORT.auto-rollback(move reverted)", g_revert);
    line("ABORT.auto-rollback(mkdir reverted)", g_mdrev);
    line("ABORT(overall)",                   abrt);
    r_unlink(SC "/g_src.txt"); r_unlink(SC "/g_dst.txt"); r_rmdir(SC "/g_dir");

    // -- Cleanup ------------------------------------------------------------
    r_unlink(SC "/a.txt"); r_unlink(SC "/2026/photo.jpg"); r_rmdir(SC "/2026");
    r_rmdir(SC "/edir"); r_unlink(SC "/todel.txt"); r_rmdir(SC);

    int overall = noreg && (e == 0) && inscope && enforce && (x == 0) && postexit && abrt;
    line("SUMMARY.no-regression", noreg);
    line("SUMMARY.in-scope",      inscope);
    line("SUMMARY.enforcement",   enforce);
    line("SUMMARY.post-exit",     postexit);
    line("SUMMARY.abort-rollback", abrt);
    say(overall ? "ESCROWT: OVERALL PASS\n" : "ESCROWT: OVERALL FAIL\n");
    say("========== ESCROWT END ==========\n");
    return overall ? 0 : 1;
}
