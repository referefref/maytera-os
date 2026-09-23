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
//       path scope AND the device scope forbid it), and any delete is REFUSED.
// The DIFFERENT-device and BadUSB fail-closed verdicts are deterministic and are
// proven by the in-kernel escdev_selftest(); a Ring-3 app cannot swap the
// physical device under itself.
//
// Its verdict goes to stdout (serial via /dev/console) AND /BOOTLOG.TXT.
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/syscall.h"

#define OUTF "/ESCROW4_OFF.TMP"    // OUT-OF-SCOPE + off-device (root fs)

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

int main(void) {
    say("\n========== ESCROW4T (#246 Stage 4 CAP_SCOPE_DEVICE) ==========\n");

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

    char inpath[96];
    snprintf(inpath, sizeof(inpath), "%s/e4.txt", scope);

    // (b) in-scope write on the CORRECT device -> ALLOWED.
    int b_in = (r_creat(inpath) >= 0);
    int b_here = exists(inpath);
    line("in-scope write on the bound device ALLOWED", b_in && b_here);

    // out-of-scope + off-device write -> REFUSED (path scope and device scope).
    int b_out = (r_open_w(OUTF) < 0) && !exists(OUTF);
    line("out-of-scope/off-device write REFUSED", b_out);

    // any delete -> REFUSED (no-delete invariant), and the in-scope file remains.
    int b_del = (r_unlink(inpath) < 0) && exists(inpath);
    line("delete on the bound device REFUSED (no-delete)", b_del);

    int overall = b_in && b_here && b_out && b_del;
    say(overall ? "ESCROW4T: OVERALL PASS\n" : "ESCROW4T: OVERALL FAIL\n");
    say("========== ESCROW4T END ==========\n");
    return overall ? 0 : 1;
}
