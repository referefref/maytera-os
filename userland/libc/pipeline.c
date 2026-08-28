// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// pipeline.c - the ONE shell-pipeline runner. Read pipeline.h first: it holds
// the design, and the MEASURED list of what the kernel pipe layer does and
// does not provide.
//
// KEEP THIS FILE TO THIS ONE FEATURE. A static archive member is pulled in as
// a WHOLE OBJECT, so anything else defined here would be dragged into every
// app that runs a pipeline, and would collide with any app carrying its own
// copy of that name (blame.md, local 91: tmpfile/system in stdio_file.c broke
// three shipped game ports).
#include "pipeline.h"
#include "unistd.h"     // dup, dup2, pipe
#include "stdlib.h"     // close
#include "syscall.h"    // sys_spawn_args, sys_spawn_redir, sys_waitpid

static const char *g_err = "no error";

const char *mpipe_error(void) { return g_err; }

// Point FD at the description SAVED refers to, or close FD if there is no save.
//
// The unchecked form of this is a real bug this project has already paid for
// (blame.md, local 99): `dup2(saved, fd)` with saved == -1 does NOT mean
// "restore failed, fd unchanged"; it means fd KEEPS WHATEVER THE BODY OF THE
// FUNCTION PUT THERE, which here is a pipe end. The Terminal held a pty slave
// reference for the rest of its life that way and never printed another
// prompt. So a failed save closes the fd instead: the bad state cannot be
// represented regardless of what the caller handed us.
static void point_at(int saved, int fd)
{
    if (saved >= 0) dup2(saved, fd);
    else            close(fd);
}

int mpipe_run(const mpipe_stage_t *st, int n, int in_fd, int out_fd, int *statuses)
{
    int pids[MPIPE_MAX_STAGES];
    int bin[MPIPE_MAX_STAGES], bout[MPIPE_MAX_STAGES];
    int save0 = -1, save1 = -1, prev_read = -1;
    int failed = 0, last = -1;
    int i;

    g_err = "no error";

    if (!st || n < 1 || n > MPIPE_MAX_STAGES) {
        g_err = "pipeline stage count out of range";
        return -1;
    }
    for (i = 0; i < n; i++) {
        pids[i] = -1;
        bin[i] = -1;
        bout[i] = -1;
        if (statuses) statuses[i] = -1;
        if (st[i].builtin) continue;
        if (!st[i].path || !st[i].argv || st[i].argc < 1) {
            g_err = "empty pipeline stage";
            return -1;
        }
    }

    save0 = dup(0);
    save1 = dup(1);

    for (i = 0; i < n; i++) {
        int P[2] = { -1, -1 };

        if (i < n - 1) {
            if (pipe(P) != 0) { g_err = "pipe() failed"; failed = 1; break; }
            // fd 0/1/2 are supposed to be open in anything that runs a
            // pipeline. If a pipe end landed on one of them they were not,
            // and every dup2 below would be juggling the caller's own stdio.
            // Refuse loudly rather than produce a pipeline that half works.
            if (P[0] < 3 || P[1] < 3) {
                close(P[0]); close(P[1]);
                g_err = "stdio fds are not open; refusing to build a pipeline on them";
                failed = 1;
                break;
            }
        }

        // ---- this stage's stdin -------------------------------------------
        if (i == 0) {
            // in_fd < 0 means "leave fd 0 alone", and on the first stage fd 0
            // is still the caller's, so there is nothing to do.
            if (in_fd >= 0 && dup2(in_fd, 0) < 0) {
                g_err = "dup2 of the pipeline input failed"; failed = 1;
                if (P[0] >= 0) { close(P[0]); close(P[1]); }
                break;
            }
        } else if (dup2(prev_read, 0) < 0) {
            g_err = "dup2 of a pipe read end failed"; failed = 1;
            if (P[0] >= 0) { close(P[0]); close(P[1]); }
            break;
        }

        // ---- this stage's stdout ------------------------------------------
        if (i == n - 1) {
            if (out_fd >= 0) {
                if (dup2(out_fd, 1) < 0) {
                    g_err = "dup2 of the pipeline output failed"; failed = 1; break;
                }
            } else if (n > 1) {
                // Earlier iterations clobbered fd 1 with a pipe write end.
                point_at(save1, 1);
            }
        } else if (dup2(P[1], 1) < 0) {
            g_err = "dup2 of a pipe write end failed"; failed = 1;
            close(P[0]); close(P[1]);
            break;
        }

        // ---- run it --------------------------------------------------------
        if (st[i].builtin) {
            // Deferred: remember the two ends and run it once every external
            // stage exists (see pipeline.h, BUILTINS).
            bin[i]  = dup(0);
            bout[i] = dup(1);
            if (bin[i] < 0 || bout[i] < 0) {
                g_err = "could not capture a builtin stage's descriptors";
                failed = 1;
                if (P[0] >= 0) { close(P[0]); close(P[1]); }
                break;
            }
        } else if (st[i].infile || st[i].outfile) {
            // A per-stage redirection WINS over the pipe, which is what a
            // POSIX shell does. sys_spawn_redir installs it in the child AFTER
            // the fd inheritance, so the existing kernel path expresses this
            // exactly; there is no reason to open the file here.
            pids[i] = sys_spawn_redir(st[i].path, st[i].argv, st[i].argc,
                                      st[i].infile, st[i].outfile, st[i].append);
        } else {
            pids[i] = sys_spawn_args(st[i].path, st[i].argv, st[i].argc);
        }

        // The child (or the saved builtin descriptors) now holds its own
        // references, so drop ours. Dropping the WRITE end is what eventually
        // lets the downstream reader see EOF.
        if (prev_read >= 0) { close(prev_read); prev_read = -1; }
        if (i < n - 1) { close(P[1]); prev_read = P[0]; }

        if (!st[i].builtin && pids[i] < 0) {
            g_err = "spawn failed";
            failed = 1;
            break;
        }
    }

    // Release every descriptor we still hold, so that the stages that DID
    // start observe a correct EOF rather than waiting on us. This must happen
    // before the wait loop below or a partially-built pipeline deadlocks.
    if (prev_read >= 0) { close(prev_read); prev_read = -1; }
    point_at(save0, 0);
    point_at(save1, 1);

    // ---- deferred builtin stages ------------------------------------------
    // Skipped entirely when the pipeline could not be built: running a stage
    // into a pipe whose consumer was never spawned is the "produced output for
    // a pipeline that did not run" shape this whole ticket is about.
    for (i = 0; i < n && !failed; i++) {
        int rc;
        if (!st[i].builtin || bin[i] < 0) continue;
        dup2(bin[i], 0);
        dup2(bout[i], 1);
        rc = st[i].builtin(st[i].builtin_ctx);
        point_at(save0, 0);
        point_at(save1, 1);
        close(bin[i]);
        close(bout[i]);     // drops the write end: the consumer now sees EOF
        bin[i] = bout[i] = -1;
        if (statuses) statuses[i] = rc;
        if (i == n - 1) last = rc;
    }
    for (i = 0; i < n; i++) {           // any builtin we never got to
        if (bin[i]  >= 0) close(bin[i]);
        if (bout[i] >= 0) close(bout[i]);
    }

    if (save0 >= 0) close(save0);
    if (save1 >= 0) close(save1);

    // ---- reap ---------------------------------------------------------------
    for (i = 0; i < n; i++) {
        int s = 0;
        if (pids[i] <= 0) continue;
        sys_waitpid(pids[i], &s, 0);
        if (statuses) statuses[i] = s;
        if (i == n - 1) last = s;
    }

    return failed ? -1 : last;
}
