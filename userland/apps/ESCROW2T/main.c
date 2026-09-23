// ESCROW2T - #246/#305 verifier for the STAGE 2 (MANDATORY / always-marked)
// kernel-enforced AI escrow (docs/CONTRACT_ENFORCEMENT_PLAN.md, Stage 2).
//
// Unlike ESCROWT (Stage 1), this app does NOT enter escrow itself. It is
// spawned ALREADY MARKED + LOCKED by the kernel trusted spawner
// (escrow_spawn_marked, launched from proc/escrow2_vmtest.c). So from its VERY
// FIRST instruction it is a mandatory escrow actor, and it proves, in one run,
// the four Stage 2 properties:
//   (b) MARKED FROM FIRST INSTRUCTION: its first FS syscalls are already
//       enforced - an in-scope write SUCCEEDS (a live grant it never asked for)
//       while an out-of-scope write and any delete are refused BY THE KERNEL.
//   (c) NO SELF-EXIT: escrow_exit() (raw SYS_ESCROW_EXIT) is REFUSED, and the
//       mark still holds afterwards (a delete is still denied).
//   (d) FORK INHERITANCE: a forked child is ALSO enforced - the kernel refuses
//       the child's out-of-scope write and delete via raw syscall, while the
//       child's in-scope write under the inherited grant succeeds.
// Its verdict goes to stdout (serial via /dev/console) AND to /BOOTLOG.TXT via
// sys_bootlog, so a headless VM can capture it.
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/syscall.h"

#define SC   "/ESCROW2SC"          // the granted scope prefix (pre-created)
#define OUTF "/ESCROW2_OUT.TMP"    // an OUT-OF-SCOPE path (root, not under SC)

static void say(const char *s) { printf("%s", s); sys_bootlog(s); }
static const char *verdict(int ok) { return ok ? "PASS" : "FAIL"; }

// Raw, ungated syscalls (bypass the userland aicap/escrow advisory layer).
static long r_unlink(const char *p) { return syscall1(SYS_UNLINK, (long)p); }
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
    char l[192]; snprintf(l, sizeof(l), "ESCROW2T: %s %s\n", label, verdict(ok)); say(l);
}
static void cline(const char *label, int ok) {
    char l[192]; snprintf(l, sizeof(l), "ESCROW2T-CHILD: %s %s\n", label, verdict(ok)); say(l);
}

int main(void) {
    say("\n========== ESCROW2T (#246 Stage 2 MANDATORY escrow) ==========\n");

    // -- Phase B: MARKED FROM FIRST INSTRUCTION. -----------------------------
    // No escrow_enter() call: the kernel spawned us already marked+locked. Our
    // very first mutations are therefore already enforced.
    int b_inscope = (r_creat(SC "/mark.txt") >= 0);        // in-scope -> ALLOW
    int b_outw    = (r_open_w(OUTF) < 0);                  // out-of-scope -> DENY
    int b_del     = (r_unlink(SC "/mark.txt") < 0);        // any delete -> DENY
    int b_intact  = exists(SC "/mark.txt") && !exists(OUTF);
    int marked    = b_inscope && b_outw && b_del && b_intact;
    line("MARKED.in-scope-write-ok(first instruction)", b_inscope);
    line("MARKED.out-of-scope-write-refused",           b_outw);
    line("MARKED.delete-refused(no-delete invariant)",  b_del);
    line("MARKED.refused-ops-had-no-effect",            b_intact);
    line("MARKED-FROM-FIRST-INSTRUCTION",               marked);

    // -- Phase C: NO SELF-EXIT. The task cannot free ITSELF. -----------------
    int x         = escrow_exit();          // raw SYS_ESCROW_EXIT
    int c_refused = (x != 0);               // must be refused (ESCROW_E_LOCKED)
    // and enforcement must STILL hold after the refused exit attempt
    int c_still   = (r_unlink(SC "/mark.txt") < 0) && (r_open_w(OUTF) < 0);
    int noexit    = c_refused && c_still;
    line("NO-SELF-EXIT.escrow_exit-refused", c_refused);
    line("NO-SELF-EXIT.still-enforced-after", c_still);
    line("NO-SELF-EXIT(cannot unshackle itself)", noexit);

    // -- Phase D: FORK INHERITANCE. The child is ALSO enforced. --------------
    int inherit = 0;
    long kid = fork();
    if (kid == 0) {
        // CHILD. We inherited the mark + the same grant binding.
        int k_out = (r_open_w(OUTF) < 0);                 // out-of-scope -> DENY
        int k_del = (r_unlink(SC "/mark.txt") < 0);       // delete -> DENY
        int k_in  = (r_creat(SC "/child.txt") >= 0);      // in-scope -> ALLOW
        int k_intact = exists(SC "/mark.txt") && !exists(OUTF);
        int k_ok = k_out && k_del && k_in && k_intact;
        cline("out-of-scope-write-refused", k_out);
        cline("delete-refused",             k_del);
        cline("in-scope-write-ok(inherited grant)", k_in);
        cline("INHERITED-AND-ENFORCED",     k_ok);
        say(k_ok ? "ESCROW2T-CHILD: RESULT PASS\n" : "ESCROW2T-CHILD: RESULT FAIL\n");
        _exit(k_ok ? 0 : 1);
    } else if (kid > 0) {
        int status = 0;
        (void)sys_waitpid((int)kid, &status, 0);   // let the child finish first
        // Child success is encoded in its exit status.
        inherit = (status == 0);
        line("FORK-INHERITANCE(child inherited mark + enforced)", inherit);
    } else {
        line("FORK-INHERITANCE(fork failed)", 0);
    }

    int overall = marked && noexit && inherit;
    line("SUMMARY.marked-from-first-instruction", marked);
    line("SUMMARY.no-self-exit",                  noexit);
    line("SUMMARY.fork-inheritance",              inherit);
    say(overall ? "ESCROW2T: OVERALL PASS\n" : "ESCROW2T: OVERALL FAIL\n");
    say("========== ESCROW2T END ==========\n");
    return overall ? 0 : 1;
}
