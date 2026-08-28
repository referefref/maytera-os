// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// pipeline.h - the ONE shell-pipeline runner for MayteraOS userland.
//
// ============================================================================
// WHY THIS EXISTS
// ============================================================================
//
// `a | b` is the mechanism every command-line tool composes through, so a shell
// whose pipes do not connect caps the value of every other tool. Two programs
// in this tree run pipelines: the Terminal (which had a WORKING two-stage
// implementation) and msh (which split on '|' and then ran the stages
// SEQUENTIALLY AND UNCONNECTED, exit 0, comment "run sequentially for now").
// Rather than copy the Terminal's version into msh - the second-copy defect
// this project keeps paying for - the working mechanism moved HERE, gained
// N stages, and both programs call it. See the owner rule in blame.md:
// "NEVER REINVENT A WHEEL INSIDE OUR OWN PROJECT."
//
// ============================================================================
// HOW A PIPELINE IS BUILT WITHOUT fork()
// ============================================================================
//
// MayteraOS creates processes by SPAWN, not fork-then-exec (see spawn.h). The
// classic "fork, dup2 in the child, exec" idiom is therefore unavailable. What
// makes a real pipeline possible anyway is one property of the kernel spawn,
// in kernel/proc/syscall.c spawn_impl(): for an argv spawn the child inherits
// the CALLER'S fds[0..2] by reference (file_get on the parent's file_t). So the
// parent points its OWN fd 0/1 at the right pipe ends, spawns, and puts them
// back. That is what this file does, once, for everybody.
//
// The kernel primitives it stands on, all of which already existed:
//   SYS_PIPE  (92)  kernel/fs/pipe.c: a real 64 KB ring with a wait queue, a
//                   blocking read, EOF when the last writer's file_t is
//                   released, and poll ops.
//   SYS_DUP   (90) / SYS_DUP2 (91)   kernel/fs/vfs.c refcounted fd table.
//   SYS_SPAWN_ARGS (198) / SYS_SPAWN_REDIR (247)  spawn + fd inheritance.
//
// ============================================================================
// WHAT THE KERNEL DOES NOT PROVIDE, MEASURED - READ BEFORE RELYING ON THIS
// ============================================================================
//
//  1. FIXED in build 1994 (#111a). THE WRITE NOW BLOCKS. This entry used to
//     read "THERE IS NO BLOCKING WRITE": pipe_write_fn() wrote what fitted and
//     returned 0 for a full ring, and because libc's flush_writes() loops on a
//     short write, a producer that outran its consumer busy-spun in the
//     shell's stead. MEASURED on golden 1993 at 4,616,023 zero-returns to push
//     256 KB through the 64 KB ring, burning CPU at 97% of the rate of a
//     deliberate busy loop. kernel/fs/pipe.c now has a write wait queue,
//     symmetric with the read one #511 added, woken by every read that frees
//     space and by the last reader closing. Re-measured on the same probe and
//     the same machine: 64 write() calls, ZERO zero-returns, zero measurable
//     producer CPU. write(2) no longer returns 0 for a non-zero count.
//
//  2. FIXED in build 1994 (#111b). THERE IS NOW SIGPIPE. This entry used to
//     read "THERE IS NO SIGPIPE". Writing to a pipe whose readers are all gone
//     now raises SIGPIPE at the writing process and returns -EPIPE (-32, was a
//     bare -1). The POSIX default action applies, so a producer that IGNORES
//     its write result is terminated with the conventional 128+13 = 141 exit
//     code, and `yes | head -1` terminates on this OS. A process that installs
//     SIG_IGN or a handler survives and gets -EPIPE back instead, so the
//     error-checking idiom below still works unchanged.
//
//     STILL TRUE, and it is why the tools keep checking their writes: a tool
//     that wants to exit 0 rather than be killed should keep doing so. Being
//     terminated by a signal is a correct end for a producer, not a graceful
//     one.
//
//  2b. NOT IMPLEMENTED: PIPE_BUF write atomicity. A write of at most PIPE_BUF
//     bytes is NOT all-or-nothing, so two concurrent writers to one pipe can
//     interleave. Every pipeline stage here has a single writer, so nothing in
//     the tree depends on it. See rustkern/pipewr.rs for why a half-atomic
//     pipe would be worse than an honestly non-atomic one.
//
//  3. There is no job control on a pipeline: every stage is spawned into the
//     caller's process group, and this call waits for all of them.
//
// ============================================================================
// BUILTINS
// ============================================================================
//
// A stage may be a shell BUILTIN instead of a program. With no fork(), a
// builtin stage necessarily runs IN THE SHELL PROCESS, where a POSIX shell
// would use a subshell; a `cd` or `exit` in a pipeline therefore affects the
// shell here and would not elsewhere. It is executed with fd 0/1 pointed at
// its pipe ends, and it is DEFERRED until every external stage has been
// spawned, so that a builtin cannot fill the 64 KB ring while the process that
// would drain it does not exist yet.
// ============================================================================

#ifndef LIBC_PIPELINE_H
#define LIBC_PIPELINE_H

// Eight is msh's own MAX_PIPES and the Terminal never needed more than two.
#define MPIPE_MAX_STAGES 8

// A builtin stage: return value is that stage's exit status. It is called with
// fd 0 and fd 1 already pointed at this stage's ends of the pipeline.
typedef int (*mpipe_builtin_fn)(void *ctx);

typedef struct {
    const char       *path;        // resolved absolute program path
    char            **argv;        // argv[0..argc-1]; argv[0] is normally path
    int               argc;
    const char       *infile;      // per-stage "< file"  (NULL = use the pipe)
    const char       *outfile;     // per-stage "> file"  (NULL = use the pipe)
    int               append;      // outfile opened for append rather than truncate
    mpipe_builtin_fn  builtin;     // non-NULL: run in-process, do not spawn
    void             *builtin_ctx;
} mpipe_stage_t;

// Run stages[0..n-1] connected by real kernel pipes.
//
//   in_fd   fd to use as stage 0's stdin,       or -1 to leave fd 0 alone
//   out_fd  fd to use as the last stage's stdout, or -1 to leave fd 1 alone
//   statuses  optional, n entries, each stage's exit status (-1 = never ran)
//
// Returns the LAST stage's exit status, or -1 if the pipeline could not be
// built, in which case mpipe_error() names the reason. It NEVER reports
// success for a pipeline it did not run: that silent-wrong-answer shape is the
// whole defect class this work exists to remove.
//
// The caller's fd 0 and fd 1 are restored before this returns, including on
// every failure path.
int mpipe_run(const mpipe_stage_t *stages, int n,
              int in_fd, int out_fd, int *statuses);

// Reason for the last -1 from mpipe_run(). Never NULL.
const char *mpipe_error(void);

#endif // LIBC_PIPELINE_H
