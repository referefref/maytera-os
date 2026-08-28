// WINCHPRB - #227 headless reproduction harness for the vi-resize crash and
// the terminal/pty wedge that follows it.
//
// WHY THIS EXISTS. #227 says resizing a Terminal window while vi runs crashes
// vi and then permanently wedges the Terminal (no command ever runs again).
// Reproducing that through the GUI needs a mouse-driven window resize, which
// QMP mouse injection cannot reliably land on this rig (#334/#440), and two
// prior attempts to capture the actual kernel fault line both dead-ended
// (blame.md 2026-08-22 #227 entry). This harness reproduces the SAME kernel
// mechanism without any GUI: it opens its own /dev/ptmx pair exactly the way
// userland/apps/terminal/main.c's run_foreground_pty() does (same TIOCGPTN,
// same TIOCSWINSZ, same setpgid/TIOCSPGRP dance), spawns a real child on it,
// and issues the SAME ioctl(master, TIOCSWINSZ, ...) a window resize would
// have produced. Launched via /CONFIG/AUTORUN.CFG (detached, fd 1/2 -> kernel
// console -> serial), so the kernel's own [EXCEPTION]/[#PF]/[PROC] lines and
// this probe's own WINCHPRB: lines land in one ordered serial capture.
//
// TEST 0 (baseline, NOT vi, NO resize): spawn cat.elf on a pty, confirm it is
// blocked reading stdin, kill -9 it with no resize involved, and time how
// long the master takes to see EOF. This isolates "does ANY abnormal child
// death wedge the pty" from "does dying DURING signal handling wedge it".
//
// TEST 1 (vi + shrink): spawn /APPS/VI on a real multi-line scratch file,
// wait for its first paint, shrink the pty window (TIOCSWINSZ -> SIGWINCH),
// and report whether vi is still alive (waitpid WNOHANG), and if not, its
// exit status/signal, plus whether the master ever sees EOF.
//
// TEST 2 (vi + grow): same as TEST 1 but from a fresh vi instance, growing
// the window instead of shrinking it (#227 was reproduced in BOTH
// directions).

#include "../../libc/maytera.h"
#include "../../libc/fcntl.h"
#include "../../libc/termios.h"
#include "../../libc/sys/ioctl.h"
#include "../../libc/unistd.h"
#include "../../libc/sys/wait.h"
#include "../../libc/signal.h"

static void outf(const char *fmt, ...) {
    char buf[256];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    syscall3(SYS_WRITE, 1, (long)(uintptr_t)buf, (long)n);
}

// Open a fresh /dev/ptmx pair, spawn `path` on it exactly the way
// run_foreground_pty() does, and return the child pid (or <0) with *out_master
// set to the master fd on success.
static int spawn_on_pty(const char *path, char **argv, int argc,
                         int rows, int cols, int *out_master) {
    int master = open("/dev/ptmx", O_RDWR | O_NONBLOCK);
    if (master < 0) { outf("WINCHPRB: ptmx open failed\n"); return -1; }
    int idx = -1;
    if (ioctl(master, TIOCGPTN, &idx) != 0 || idx < 0) {
        outf("WINCHPRB: TIOCGPTN failed\n");
        close(master);
        return -1;
    }
    struct winsize ws;
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ws.ws_xpixel = 0; ws.ws_ypixel = 0;
    ioctl(master, TIOCSWINSZ, &ws);

    char slavepath[24];
    { const char *pre = "/dev/pts/"; int i = 0;
      while (pre[i]) { slavepath[i] = pre[i]; i++; }
      if (idx >= 10) slavepath[i++] = (char)('0' + idx / 10);
      slavepath[i++] = (char)('0' + idx % 10);
      slavepath[i] = '\0'; }

    int slave = open(slavepath, O_RDWR);
    if (slave < 0) {
        outf("WINCHPRB: slave open '%s' failed\n", slavepath);
        close(master);
        return -1;
    }

    int s0 = dup(0), s1 = dup(1), s2 = dup(2);
    dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
    int pid = sys_spawn_args(path, argv, argc);
    int sv[3] = { s0, s1, s2 };
    for (int i = 0; i < 3; i++) {
        if (sv[i] >= 0) { dup2(sv[i], i); close(sv[i]); }
        else            { close(i); }
    }
    close(slave);

    if (pid > 0) {
        setpgid(pid, pid);
        unsigned int pg = (unsigned int)pid;
        ioctl(master, TIOCSPGRP, &pg);
    } else {
        outf("WINCHPRB: spawn '%s' failed pid=%d\n", path, pid);
        close(master);
        return pid;
    }
    *out_master = master;
    return pid;
}

// Drain whatever is on master for up to budget_ms without blocking (master is
// O_NONBLOCK). Returns 1 the instant read() returns 0 (EOF: slave_refs hit
// zero and the ring is empty), 0 if budget_ms elapses with no EOF. *drained
// counts bytes discarded (vi's redraw traffic; we do not need the content).
static int wait_master_eof(int master, int budget_ms, long *drained) {
    char buf[512];
    int elapsed = 0;
    if (drained) *drained = 0;
    while (elapsed < budget_ms) {
        long n = read(master, buf, sizeof(buf));
        if (n > 0) { if (drained) *drained += n; continue; }
        if (n == 0) return 1;         // EOF
        usleep(20000);
        elapsed += 20;
    }
    return 0;
}

// Poll waitpid(WNOHANG) for up to budget_ms. Returns 1 and fills *status if
// the child has exited, 0 if it is still running after the budget.
static int wait_child_exit(int pid, int budget_ms, int *status) {
    int elapsed = 0;
    while (elapsed < budget_ms) {
        int st = 0;
        int r = sys_waitpid(pid, &st, WNOHANG);
        if (r == pid) { if (status) *status = st; return 1; }
        usleep(20000);
        elapsed += 20;
    }
    return 0;
}

static void report_status(const char *tag, int status) {
    if (WIFSIGNALED(status)) {
        outf("WINCHPRB: %s child died: WIFSIGNALED sig=%d (raw status=%d)\n",
             tag, WTERMSIG(status), status);
    } else if (WIFEXITED(status)) {
        outf("WINCHPRB: %s child exited normally: code=%d (raw status=%d)\n",
             tag, WEXITSTATUS(status), status);
    } else {
        outf("WINCHPRB: %s child status unrecognised: raw=%d\n", tag, status);
    }
}

static void write_scratch_file(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { outf("WINCHPRB: could not create %s\n", path); return; }
    const char *lines[] = {
        "WINCHPRB scratch file for #227 reproduction.\n",
        "line 02: the quick brown fox jumps over the lazy dog\n",
        "line 03: 0123456789 0123456789 0123456789 0123456789\n",
        "line 04: abcdefghijklmnopqrstuvwxyz\n",
        "line 05: ---------------------------------------------\n",
        "line 06: vi should reflow this buffer on a real resize\n",
        "line 07: without dying, per #220's original SIGWINCH fix\n",
        "line 08: filler filler filler filler filler filler\n",
        "line 09: filler filler filler filler filler filler\n",
        "line 10: filler filler filler filler filler filler\n",
        "line 11: filler filler filler filler filler filler\n",
        "line 12: filler filler filler filler filler filler\n",
        "line 13: filler filler filler filler filler filler\n",
        "line 14: filler filler filler filler filler filler\n",
        "line 15: filler filler filler filler filler filler\n",
        "line 16: filler filler filler filler filler filler\n",
        "line 17: filler filler filler filler filler filler\n",
        "line 18: filler filler filler filler filler filler\n",
        "line 19: filler filler filler filler filler filler\n",
        "line 20: last line\n",
    };
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        const char *l = lines[i];
        long len = 0; while (l[len]) len++;
        write(fd, l, len);
    }
    close(fd);
}

// ---- TEST 0: baseline, NOT vi, NO resize ----------------------------------
static void test0_baseline_kill(void) {
    outf("WINCHPRB: === TEST 0: baseline abnormal-death, no vi, no resize ===\n");
    char *argv0[] = { "CAT", 0 };
    int master = -1;
    int pid = spawn_on_pty("/APPS/CAT", argv0, 1, 24, 80, &master);
    if (pid <= 0) { outf("WINCHPRB: TEST0 spawn failed, skipping\n"); return; }
    outf("WINCHPRB: TEST0 spawned CAT pid=%d, waiting 300ms for it to block on read(0)\n", pid);
    usleep(300000);

    outf("WINCHPRB: TEST0 sending SIGKILL (no resize)\n");
    kill(pid, SIGKILL);

    int status = 0;
    int exited = wait_child_exit(pid, 2000, &status);
    if (!exited) {
        outf("WINCHPRB: TEST0 [FAIL] child did not exit within 2000ms of SIGKILL\n");
    } else {
        report_status("TEST0", status);
    }

    long drained = 0;
    int eof = wait_master_eof(master, 3000, &drained);
    outf("WINCHPRB: TEST0 %s master EOF (drained=%ld bytes while waiting)\n",
         eof ? "[PASS] observed" : "[FAIL] NEVER observed", drained);
    close(master);
    outf("WINCHPRB: TEST0 %s\n\n", eof && exited ? "RESULT=PASS (terminal would recover)"
                                                  : "RESULT=FAIL (terminal would WEDGE)");
}

// ---- TEST 1/2: vi + resize -------------------------------------------------
static void test_vi_resize(const char *tag, int start_rows, int start_cols,
                            int new_rows, int new_cols) {
    outf("WINCHPRB: === %s: vi %dx%d -> resize to %dx%d ===\n",
         tag, start_cols, start_rows, new_cols, new_rows);
    write_scratch_file("/WINCHPRB.TXT");
    char *argv1[] = { "VI", "/WINCHPRB.TXT", 0 };
    int master = -1;
    int pid = spawn_on_pty("/APPS/VI", argv1, 2, start_rows, start_cols, &master);
    if (pid <= 0) { outf("WINCHPRB: %s spawn failed, skipping\n", tag); return; }
    outf("WINCHPRB: %s spawned vi pid=%d, waiting 800ms for first paint\n", tag, pid);

    long drained = 0;
    // Drain vi's initial screen paint (discarded; we only care whether it is
    // alive and drawing, which nonzero drained bytes proves).
    (void)wait_master_eof(master, 800, &drained);
    outf("WINCHPRB: %s initial paint drained=%ld bytes\n", tag, drained);

    int status = 0;
    if (wait_child_exit(pid, 0, &status)) {
        outf("WINCHPRB: %s vi already dead BEFORE any resize (unexpected)\n", tag);
        report_status(tag, status);
        close(master);
        return;
    }

    outf("WINCHPRB: %s issuing TIOCSWINSZ %dx%d (raises SIGWINCH at vi's fg_pgrp)\n",
         tag, new_cols, new_rows);
    struct winsize ws;
    ws.ws_row = (unsigned short)new_rows;
    ws.ws_col = (unsigned short)new_cols;
    ws.ws_xpixel = 0; ws.ws_ypixel = 0;
    ioctl(master, TIOCSWINSZ, &ws);

    drained = 0;
    (void)wait_master_eof(master, 500, &drained);
    outf("WINCHPRB: %s post-resize drained=%ld bytes (nonzero = vi redrew)\n", tag, drained);

    int exited = wait_child_exit(pid, 1500, &status);
    if (!exited) {
        outf("WINCHPRB: %s [PASS] vi is STILL ALIVE after the resize\n", tag);
        // Clean shutdown for the next test: SIGKILL it, then confirm the pty
        // still recovers normally (this exercises TEST0's same check on a
        // process that DID receive a SIGWINCH earlier in its life).
        kill(pid, SIGKILL);
        wait_child_exit(pid, 1000, &status);
    } else {
        outf("WINCHPRB: %s [BUG-A] vi DIED as a result of the resize\n", tag);
        report_status(tag, status);
    }

    long drained2 = 0;
    int eof = wait_master_eof(master, 3000, &drained2);
    outf("WINCHPRB: %s %s master EOF (drained=%ld bytes while waiting)\n",
         tag, eof ? "[PASS] observed" : "[BUG-B] NEVER observed", drained2);
    close(master);
    outf("WINCHPRB: %s RESULT=%s\n\n", tag, eof ? "PASS" : "FAIL-WEDGE");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    outf("\nWINCHPRB: ==== #227 headless reproduction BEGIN ====\n");

    test0_baseline_kill();
    test_vi_resize("TEST1-SHRINK", 24, 80, 10, 40);
    test_vi_resize("TEST2-GROW",   24, 80, 40, 160);

    outf("WINCHPRB: ==== #227 headless reproduction END ====\n\n");
    return 0;
}
