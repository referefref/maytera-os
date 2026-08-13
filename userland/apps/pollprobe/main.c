// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// pollprobe - artefact-level verification for the syscalls added in #745
// (local 82): poll(2) and the POSIX process-group / session family.
//
// WHY THIS EXISTS AND WHY IT IS NOT A UNIT TEST. Every one of these calls used
// to fall into the dispatcher's default case and return a flat -1. A kernel
// self-test proves the POLICY is right; it cannot prove the DISPATCHER routes
// the number, that the libc wrapper targets the right one, or that a real fd
// produces the right answer. Only a Ring-3 process making the actual syscall
// proves that, which is what this does. It prints one PASS/FAIL line per check
// to stdout, which /dev/console mirrors to serial.
//
// Launched via /CONFIG/AUTORUN.CFG on a throwaway VM. It is a verification aid,
// not a shipped app.
#include "stdio.h"
#include "unistd.h"
#include "poll.h"
#include "errno.h"
#include "time.h"
#include "string.h"
#include "fcntl.h"
#include "stdlib.h"
#include "sys/wait.h"

static int g_fail = 0;

static void check(const char *name, int ok, const char *detail) {
    if (!ok) g_fail++;
    printf("[POLLPROBE] %s %s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
}

static long now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(void) {
    char d[128];
    printf("[POLLPROBE] start pid=%d\n", (int)getpid());

    // --- A. The syscall dispatches at all, and it really sleeps -------------
    // A default-case hit returns -1 (errno EPERM). 0 with a real elapsed time
    // is the difference between "routed and blocked on the wait queue" and
    // "unknown syscall".
    {
        long t0 = now_ms();
        int r = poll(0, 0, 300);
        long dt = now_ms() - t0;
        snprintf(d, sizeof d, "(rc=%d errno=%d elapsed=%ldms)", r, errno, dt);
        check("A_dispatch_and_sleep", r == 0 && dt >= 250 && dt < 3000, d);
    }

    // --- B. A regular file is always ready (POSIX) --------------------------
    // Opened through SYS_OPEN, so it lives in the LEGACY kernel-wide fd table,
    // not the per-process file_t table: this is the fd_legacy_is_open() path.
    {
        int fd = open("/CONFIG/AUTORUN.CFG", O_RDONLY);
        if (fd < 0) fd = open("/BUILDINFO.TXT", O_RDONLY);
        struct pollfd p; p.fd = fd; p.events = POLLIN | POLLOUT; p.revents = 0;
        int r = poll(&p, 1, 0);
        snprintf(d, sizeof d, "(fd=%d rc=%d revents=0x%x)", fd, r, p.revents);
        check("B_regular_file_ready",
              fd >= 0 && r == 1 && p.revents == (POLLIN | POLLOUT), d);
        if (fd >= 0) close(fd);
    }

    // --- C. An fd that is not open is POLLNVAL, not an error return ---------
    {
        struct pollfd p; p.fd = 61; p.events = POLLIN; p.revents = 0;
        int r = poll(&p, 1, 0);
        snprintf(d, sizeof d, "(rc=%d revents=0x%x)", r, p.revents);
        check("C_badfd_POLLNVAL", r == 1 && p.revents == POLLNVAL, d);
    }

    // --- D. A negative fd is skipped and NOT counted ------------------------
    {
        struct pollfd p; p.fd = -1; p.events = POLLIN; p.revents = 0xff;
        int r = poll(&p, 1, 0);
        snprintf(d, sizeof d, "(rc=%d revents=0x%x)", r, p.revents);
        check("D_negative_fd_skipped", r == 0 && p.revents == 0, d);
    }

    // --- E. nfds above the per-process fd limit is EINVAL, not truncation ---
    // The array must REALLY be 65 entries. The #503 argtab choke point validates
    // nfds * 8 bytes of user memory BEFORE the handler runs, so passing 65 with a
    // one-element buffer is rejected as EFAULT (correctly: the caller asserted an
    // array that is not there) and the handler's own limit is never reached. This
    // check is about the LIMIT, so it hands over a buffer that is genuinely that
    // size and leaves EFAULT to be what it is.
    {
        struct pollfd big[65];
        for (int i = 0; i < 65; i++) { big[i].fd = -1; big[i].events = 0; big[i].revents = 0; }
        errno = 0;
        int r = poll(big, 65, 0);
        snprintf(d, sizeof d, "(rc=%d errno=%d)", r, errno);
        check("E_nfds_over_limit_EINVAL", r == -1 && errno == EINVAL, d);

        // And exactly at the limit it is accepted (all slots disabled, so 0).
        errno = 0;
        int r64 = poll(big, 64, 0);
        snprintf(d, sizeof d, "(rc=%d errno=%d)", r64, errno);
        check("E2_nfds_at_limit_accepted", r64 == 0, d);
    }

    // --- F. stdout is writable, not readable --------------------------------
    // fd 1 is a /dev/console file_t with a real ops->poll, so this is the
    // file_poll() path and it must FILTER by the request.
    {
        struct pollfd p; p.fd = 1; p.events = POLLOUT; p.revents = 0;
        int r = poll(&p, 1, 0);
        int wok = (r == 1 && (p.revents & POLLOUT));
        struct pollfd q; q.fd = 1; q.events = POLLIN; q.revents = 0;
        int r2 = poll(&q, 1, 0);
        snprintf(d, sizeof d, "(out rc=%d rev=0x%x | in rc=%d rev=0x%x)",
                 r, p.revents, r2, q.revents);
        check("F_console_writable_not_readable", wok && r2 == 0 && q.revents == 0, d);
    }

    // --- G. A pipe: the one file kind for which always-ready is a lie -------
    {
        int fds[2];
        if (pipe(fds) != 0) {
            check("G_pipe", 0, "(pipe() failed)");
        } else {
            struct pollfd p; p.fd = fds[0]; p.events = POLLIN; p.revents = 0;
            int empty = poll(&p, 1, 0);
            int empty_rev = p.revents;

            // The write end must report writable.
            struct pollfd w; w.fd = fds[1]; w.events = POLLOUT; w.revents = 0;
            int wr = poll(&w, 1, 0);

            // A timed poll on an EMPTY pipe must block for its full timeout and
            // then report nothing, not return instantly.
            long t0 = now_ms();
            p.revents = 0;
            int timed = poll(&p, 1, 250);
            long dt = now_ms() - t0;

            write(fds[1], "x", 1);
            p.revents = 0;
            int full = poll(&p, 1, 0);
            int full_rev = p.revents;

            snprintf(d, sizeof d,
                     "(empty rc=%d rev=0x%x | wr rc=%d rev=0x%x | timed rc=%d %ldms | full rc=%d rev=0x%x)",
                     empty, empty_rev, wr, w.revents, timed, dt, full, full_rev);
            check("G_pipe_readiness",
                  empty == 0 && empty_rev == 0 &&
                  wr == 1 && (w.revents & POLLOUT) &&
                  timed == 0 && dt >= 200 && dt < 3000 &&
                  full == 1 && (full_rev & POLLIN), d);

            // Closing the write end must surface POLLHUP on the read end, which
            // is how a poll()-driven reader learns to stop.
            close(fds[1]);
            struct pollfd h; h.fd = fds[0]; h.events = POLLIN; h.revents = 0;
            int hr = poll(&h, 1, 0);
            snprintf(d, sizeof d, "(rc=%d revents=0x%x)", hr, h.revents);
            check("G_pipe_hup_on_writer_close", (h.revents & POLLHUP) != 0, d);
            close(fds[0]);
        }
    }

    // =======================================================================
    // Process groups and sessions
    // =======================================================================
    int mypid = (int)getpid();

    // --- H. getpgid/getsid answer at all --------------------------------------
    {
        errno = 0;
        int pg = (int)getpgid(0);
        int sd = (int)getsid(0);
        snprintf(d, sizeof d, "(pid=%d pgrp=%d sid=%d)", mypid, pg, sd);
        check("H_getpgid_getsid_answer", pg > 0 && sd > 0, d);
    }

    // --- I. setpgid(0,0) makes the caller its own group leader ---------------
    {
        errno = 0;
        int rc = setpgid(0, 0);
        int pg = (int)getpgid(0);
        snprintf(d, sizeof d, "(rc=%d errno=%d pgrp=%d pid=%d)", rc, errno, pg, mypid);
        check("I_setpgid_self_leader", rc == 0 && pg == mypid, d);
    }

    // --- J. THE REFUSAL: a group leader may not setsid() ---------------------
    // I. just made us a group leader, so this must be EPERM. A permissive
    // answer here would let two distinct groups share a pgid, and
    // sig_raise_pgrp() would silently signal both.
    {
        errno = 0;
        int rc = (int)setsid();
        snprintf(d, sizeof d, "(rc=%d errno=%d)", rc, errno);
        check("J_setsid_denied_for_group_leader", rc == -1 && errno == EPERM, d);
    }

    // --- K. THE REFUSAL: joining a group that does not exist -----------------
    {
        errno = 0;
        int rc = setpgid(0, 61111);
        snprintf(d, sizeof d, "(rc=%d errno=%d)", rc, errno);
        check("K_setpgid_nonexistent_group_EPERM", rc == -1 && errno == EPERM, d);
    }

    // --- L. THE REFUSAL: a process that is neither us nor our child ----------
    // ESRCH, specifically not EPERM: a caller racing an exited child must be
    // able to tell "gone" from "refused".
    {
        errno = 0;
        int rc = setpgid(60001, 60001);
        snprintf(d, sizeof d, "(rc=%d errno=%d)", rc, errno);
        check("L_setpgid_unrelated_ESRCH", rc == -1 && errno == ESRCH, d);
    }

    // --- M. getpgid of a process that does not exist -------------------------
    {
        errno = 0;
        int rc = (int)getpgid(60002);
        snprintf(d, sizeof d, "(rc=%d errno=%d)", rc, errno);
        check("M_getpgid_missing_ESRCH", rc == -1 && errno == ESRCH, d);
    }

    // --- N. A child may setsid(), and its session becomes its own pid --------
    {
        int pid = (int)fork();
        if (pid == 0) {
            // Child: inherits the parent's pgrp, so it is NOT a group leader
            // and setsid() must succeed.
            int sid = (int)setsid();
            int me = (int)getpid();
            int sd = (int)getsid(0);
            int pg = (int)getpgid(0);
            printf("[POLLPROBE] %s N_child_setsid (sid=%d pid=%d getsid=%d getpgid=%d)\n",
                   (sid == me && sd == me && pg == me) ? "PASS" : "FAIL",
                   sid, me, sd, pg);
            _exit(0);
        } else if (pid > 0) {
            int st = 0;
            waitpid(pid, &st, 0);
        } else {
            check("N_child_setsid", 0, "(fork failed)");
        }
    }

    printf("[POLLPROBE] DONE failures=%d\n", g_fail);
    return 0;
}
