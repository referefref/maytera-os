// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// spawn_test.c - #745 test for userland/libc/spawn.c (posix_spawn).
//
// THIS TU IS COMPILED FREESTANDING (-nostdinc -I..), exactly as the shipping
// libc build compiles it, and it is linked against the REAL ../spawn.c object
// and the REAL ../sys/wait.c object. It is not a copy and not a reimplementation.
//
// The kernel is modelled in spawn_host.c, which implements syscall0..6. The
// model is not a rubber stamp: it mirrors what kernel/proc/syscall.c
// spawn_impl() actually does (validate the file and its ELF magic BEFORE any
// process exists, then install at most one redirect on fd 0 and one on fd 1),
// and it really forks and execs, so "the child ran" and "stdout went to the
// file" are observed facts rather than assertions about a recorded argument.
//
// What that buys, and what it does not: it proves this library's mapping and
// its refusals. It cannot prove the kernel's half of the contract. That is why
// the run script also cross-checks the syscall numbers against
// kernel/proc/syscall.h and the dispatcher's argument order against
// kernel/proc/syscall.c, and why a real on-VM spawn is the final word.

#include "../spawn.h"
#include "../syscall.h"
#include "../errno.h"
#include "../string.h"
#include "../fcntl.h"

// ---- services provided by spawn_host.c (hosted; no libc headers here) ------
void        t_out(const char *s);
void        t_outd(long v);
const char *t_dir(void);
long        t_slurp(const char *path, char *buf, long cap);
void        t_setenv(const char *k, const char *v);
int         mos_waitpid(int pid, int *status, int options);

// ---- what the kernel model recorded ---------------------------------------
extern int   t_calls;        // spawn syscalls issued since t_reset()
extern long  t_last_num;     // syscall number of the last spawn
extern char  t_last_path[];
extern char  t_last_infile[];
extern char  t_last_outfile[];
extern int   t_last_argc;
extern int   t_last_append;
extern int   t_last_had_redir;
void         t_reset(void);

static int fails = 0;
static int checks = 0;

static void ck(int cond, const char *what) {
    checks++;
    if (!cond) { fails++; t_out("FAIL "); t_out(what); t_out("\n"); }
    else       { t_out("  ok  "); t_out(what); t_out("\n"); }
}

static void ck_eq(int got, int want, const char *what) {
    checks++;
    if (got != want) {
        fails++;
        t_out("FAIL "); t_out(what);
        t_out(" (got "); t_outd(got); t_out(", want "); t_outd(want); t_out(")\n");
    } else {
        t_out("  ok  "); t_out(what); t_out(" = "); t_outd(got); t_out("\n");
    }
}

static void path_in_dir(char *out, unsigned long cap, const char *leaf) {
    strlcpy(out, t_dir(), cap);
    strlcat(out, "/", cap);
    strlcat(out, leaf, cap);
}

int spawn_tests(void);
int spawn_tests(void) {
    char p_exit7[256], p_say[256], p_cat[256], p_notelf[256];
    char p_out[256], p_in[256];
    char buf[256];
    pid_t pid;
    int rc, st;

    path_in_dir(p_exit7,  sizeof(p_exit7),  "helper_exit7");
    path_in_dir(p_say,    sizeof(p_say),    "helper_say");
    path_in_dir(p_cat,    sizeof(p_cat),    "helper_cat");
    path_in_dir(p_notelf, sizeof(p_notelf), "notanelf.bin");
    path_in_dir(p_out,    sizeof(p_out),    "redirected.txt");
    path_in_dir(p_in,     sizeof(p_in),     "input.txt");

    t_out("\n--- 1. the syscall numbers actually issued ---\n");
    t_out("      SYS_SPAWN_ARGS  from ../syscall.h = "); t_outd(SYS_SPAWN_ARGS);  t_out("\n");
    t_out("      SYS_SPAWN_REDIR from ../syscall.h = "); t_outd(SYS_SPAWN_REDIR); t_out("\n");
    t_out("      SYS_WAITPID     from ../syscall.h = "); t_outd(SYS_WAITPID);     t_out("\n");

    {
        char *argv[] = { p_exit7, 0 };
        t_reset();
        rc = posix_spawn(&pid, p_exit7, 0, 0, argv, 0);
        ck_eq(rc, 0, "plain posix_spawn returns 0");
        ck_eq(t_calls, 1, "issued exactly one spawn syscall");
        ck_eq((int)t_last_num, SYS_SPAWN_ARGS, "and it was SYS_SPAWN_ARGS");
        ck(strcmp(t_last_path, p_exit7) == 0, "arg1 was the path");
        ck_eq(t_last_argc, 1, "arg3 was argc");
        ck_eq(t_last_had_redir, 0, "no redirect on the plain path");

        t_out("\n--- 2. the child really ran and its exit status came back ---\n");
        st = -1;
        rc = mos_waitpid(pid, &st, 0);
        ck_eq(rc, pid, "waitpid returned the child pid");
        ck_eq(st, 7, "status is the child's exit code (raw, as proc_wait writes it)");
    }

    t_out("\n--- 3. a failed spawn returns a real errno, never a bogus success ---\n");
    {
        char nosuch[256];
        char *argv[] = { (char *)"nosuch", 0 };
        path_in_dir(nosuch, sizeof(nosuch), "definitely_not_here");
        pid = -424242;
        t_reset();
        rc = posix_spawn(&pid, nosuch, 0, 0, argv, 0);
        ck_eq(rc, ENOENT, "nonexistent binary -> ENOENT");
        ck_eq(pid, -424242, "and *pid was left alone");
    }
    {
        char *argv[] = { p_notelf, 0 };
        t_reset();
        rc = posix_spawn(&pid, p_notelf, 0, 0, argv, 0);
        ck_eq(rc, ENOEXEC, "existing non-ELF file -> ENOEXEC");
    }

    t_out("\n--- 4. a file action redirecting stdout REALLY redirects it ---\n");
    {
        posix_spawn_file_actions_t fa;
        char *argv[] = { p_say, 0 };
        ck_eq(posix_spawn_file_actions_init(&fa), 0, "file_actions_init");
        ck_eq(posix_spawn_file_actions_addopen(&fa, 1, p_out,
                                               O_WRONLY | O_CREAT | O_TRUNC, 0644),
              0, "addopen(1, ..., O_WRONLY|O_CREAT|O_TRUNC)");
        t_reset();
        rc = posix_spawn(&pid, p_say, &fa, 0, argv, 0);
        ck_eq(rc, 0, "spawn with the redirect succeeded");
        ck_eq((int)t_last_num, SYS_SPAWN_REDIR, "it used SYS_SPAWN_REDIR");
        ck(strcmp(t_last_outfile, p_out) == 0, "outfile arg was the redirect target");
        ck_eq(t_last_append, 0, "append arg was 0 for O_TRUNC");
        st = -1;
        mos_waitpid(pid, &st, 0);
        buf[0] = 0;
        t_slurp(p_out, buf, (long)sizeof(buf));
        ck(strcmp(buf, "hello-from-spawn\n") == 0,
           "the file contains what the child wrote to stdout");
        t_out("      file contents: ["); t_out(buf); t_out("]\n");
        posix_spawn_file_actions_destroy(&fa);
    }

    t_out("\n--- 5. O_APPEND maps to the append flag, and stdin redirects too ---\n");
    {
        posix_spawn_file_actions_t fa;
        char *argv[] = { p_cat, 0 };
        posix_spawn_file_actions_init(&fa);
        ck_eq(posix_spawn_file_actions_addopen(&fa, 0, p_in, O_RDONLY, 0), 0,
              "addopen(0, ..., O_RDONLY)");
        ck_eq(posix_spawn_file_actions_addopen(&fa, 1, p_out,
                                               O_WRONLY | O_CREAT | O_APPEND, 0644),
              0, "addopen(1, ..., O_APPEND)");
        t_reset();
        rc = posix_spawn(&pid, p_cat, &fa, 0, argv, 0);
        ck_eq(rc, 0, "spawn with both slots succeeded");
        ck_eq(t_last_append, 1, "append arg was 1 for O_APPEND");
        ck(strcmp(t_last_infile, p_in) == 0, "infile arg was the stdin source");
        mos_waitpid(pid, &st, 0);
        buf[0] = 0;
        t_slurp(p_out, buf, (long)sizeof(buf));
        ck(strcmp(buf, "hello-from-spawn\ngot:from-the-input-file\n") == 0,
           "child read stdin and APPENDED to the existing file");
        t_out("      file contents: ["); t_out(buf); t_out("]\n");
        posix_spawn_file_actions_destroy(&fa);
    }

    t_out("\n--- 6. everything the kernel cannot express is ENOSYS, and NO SPAWN HAPPENS ---\n");
    {
        posix_spawn_file_actions_t fa;
        char *argv[] = { p_exit7, 0 };

        posix_spawn_file_actions_init(&fa);
        ck_eq(posix_spawn_file_actions_addclose(&fa, 3), ENOSYS,
              "addclose returns ENOSYS at add time");
        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, &fa, 0, argv, 0), ENOSYS,
              "and posix_spawn refuses it too (the action was still recorded)");
        ck_eq(t_calls, 0, "no spawn syscall was issued");

        posix_spawn_file_actions_init(&fa);
        ck_eq(posix_spawn_file_actions_adddup2(&fa, 4, 2), ENOSYS,
              "adddup2 returns ENOSYS at add time");
        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, &fa, 0, argv, 0), ENOSYS, "posix_spawn refuses dup2");
        ck_eq(t_calls, 0, "no spawn syscall was issued");

        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 2, p_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, &fa, 0, argv, 0), ENOSYS,
              "stderr (fd 2) has no redirect slot -> ENOSYS");
        ck_eq(t_calls, 0, "no spawn syscall was issued");

        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, p_out, O_WRONLY | O_CREAT, 0644);
        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, &fa, 0, argv, 0), ENOSYS,
              "no O_TRUNC and no O_APPEND -> ENOSYS (the kernel would truncate anyway)");
        ck_eq(t_calls, 0, "no spawn syscall was issued");

        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, p_out,
                                         O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0644);
        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, &fa, 0, argv, 0), ENOSYS, "O_EXCL -> ENOSYS");

        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, p_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_addopen(&fa, 1, p_in,  O_WRONLY | O_CREAT | O_TRUNC, 0644);
        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, &fa, 0, argv, 0), ENOSYS,
              "two opens on the same fd -> ENOSYS (first truncate would be lost)");
        ck_eq(t_calls, 0, "no spawn syscall was issued");
    }

    t_out("\n--- 7. attributes: honoured where true by construction, ENOSYS otherwise ---\n");
    {
        posix_spawnattr_t at;
        sigset_t empty = 0, full = ~(sigset_t)0;
        char *argv[] = { p_exit7, 0 };

        ck_eq(posix_spawnattr_init(&at), 0, "spawnattr_init");
        ck_eq(posix_spawnattr_setflags(&at, (short)0x4000), EINVAL,
              "an unknown flag bit is EINVAL, not stored-and-ignored");

        posix_spawnattr_init(&at);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGDEF);
        posix_spawnattr_setsigdefault(&at, &full);
        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, 0, &at, argv, 0), 0,
              "SETSIGDEF is honoured (the child's handlers are all SIG_DFL already)");
        mos_waitpid(pid, &st, 0);

        posix_spawnattr_init(&at);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK);
        posix_spawnattr_setsigmask(&at, &empty);
        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, 0, &at, argv, 0), 0,
              "SETSIGMASK with an EMPTY mask is honoured");
        mos_waitpid(pid, &st, 0);

        posix_spawnattr_init(&at);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK);
        posix_spawnattr_setsigmask(&at, &full);
        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, 0, &at, argv, 0), ENOSYS,
              "SETSIGMASK with a non-empty mask -> ENOSYS");
        ck_eq(t_calls, 0, "no spawn syscall was issued");

        posix_spawnattr_init(&at);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&at, 0);
        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, 0, &at, argv, 0), ENOSYS,
              "SETPGROUP -> ENOSYS (SYS_SETPGID has no kernel dispatch case)");
        ck_eq(t_calls, 0, "no spawn syscall was issued");

        posix_spawnattr_init(&at);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);
        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, 0, &at, argv, 0), ENOSYS, "SETSID -> ENOSYS");
    }

    t_out("\n--- 8. envp, and the argument caps spawn_impl() would truncate silently ---\n");
    {
        char *argv[] = { p_exit7, 0 };
        char *myenv[] = { (char *)"FOO=bar", 0 };
        char big[400];
        char *bigargv[3];
        char *many[70];
        int i;

        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, 0, 0, argv, myenv), ENOSYS,
              "a deliberately constructed envp -> ENOSYS (nothing propagates here)");
        ck_eq(t_calls, 0, "no spawn syscall was issued");

        for (i = 0; i < 399; i++) big[i] = 'x';
        big[399] = 0;
        bigargv[0] = p_exit7; bigargv[1] = big; bigargv[2] = 0;
        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, 0, 0, bigargv, 0), E2BIG,
              "a 399-byte argument -> E2BIG (the kernel would cut it at 255)");
        ck_eq(t_calls, 0, "no spawn syscall was issued");

        for (i = 0; i < 65; i++) many[i] = p_exit7;
        many[65] = 0;
        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, 0, 0, many, 0), E2BIG,
              "65 arguments -> E2BIG (the kernel would clamp to 64)");
        ck_eq(t_calls, 0, "no spawn syscall was issued");
    }

    t_out("\n--- 9. posix_spawnp searches PATH in msh's four forms ---\n");
    {
        char *argv[] = { (char *)"helper", 0 };
        t_setenv("PATH", t_dir());
        t_reset();
        // Only <dir>/HELPER.ELF exists, so this can only succeed via the
        // fourth form (uppercased name plus the .ELF suffix).
        rc = posix_spawnp(&pid, "helper", 0, 0, argv, 0);
        ck_eq(rc, 0, "posix_spawnp found <dir>/HELPER.ELF from the bare name");
        st = -1;
        mos_waitpid(pid, &st, 0);
        ck_eq(st, 3, "and it was the right binary (exits 3)");
        t_reset();
        ck_eq(posix_spawnp(&pid, "no_such_helper_anywhere", 0, 0, argv, 0), ENOENT,
              "an unfindable name -> ENOENT");
    }

    t_out("\n--- 10. uninitialised objects are refused ---\n");
    {
        posix_spawn_file_actions_t fa;
        posix_spawnattr_t at;
        char *argv[] = { p_exit7, 0 };
        memset(&fa, 0xAA, sizeof(fa));
        memset(&at, 0xAA, sizeof(at));
        ck_eq(posix_spawn_file_actions_addopen(&fa, 1, p_out, O_WRONLY, 0), EINVAL,
              "addopen on an uninitialised file-actions object -> EINVAL");
        t_reset();
        ck_eq(posix_spawn(&pid, p_exit7, &fa, 0, argv, 0), EINVAL,
              "posix_spawn with an uninitialised file-actions object -> EINVAL");
        ck_eq(posix_spawn(&pid, p_exit7, 0, &at, argv, 0), EINVAL,
              "posix_spawn with an uninitialised attr object -> EINVAL");
        ck_eq(posix_spawn(&pid, p_exit7, 0, 0, 0, 0), EINVAL, "NULL argv -> EINVAL");
        ck_eq(t_calls, 0, "no spawn syscall was issued for any of them");
    }

    t_out("\n");
    t_out("checks: "); t_outd(checks);
    t_out("  failures: "); t_outd(fails); t_out("\n");
    return fails;
}
