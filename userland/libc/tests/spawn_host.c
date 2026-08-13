// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// spawn_host.c - the hosted half of the #745 posix_spawn test.
//
// It implements syscall0..syscall6, which is the ONLY seam between the
// shipping userland/libc/spawn.c and the kernel. spawn.c and sys/wait.c are
// compiled here exactly as the real libc build compiles them, unmodified, and
// linked against this file: nothing in the code under test is stubbed,
// rewritten or conditionally compiled for the test.
//
// THE MODEL IS FAITHFUL TO kernel/proc/syscall.c spawn_impl(), which is what
// makes the results mean anything:
//
//   * The file is read and its ELF magic validated BEFORE any process exists.
//     spawn_impl() does fat_read_file() then elf_validate() and returns -1 on
//     either, so a nonexistent or non-ELF path is a FAILED SPAWN, not a child
//     that dies a moment later. That is exactly the case the test cares about
//     most, so the model must get it right or the test proves nothing.
//   * Every failure collapses to -1 with no detail, as the real one does.
//   * SYS_SPAWN_REDIR installs at most ONE file on the child's fd 1, opened
//     O_WRONLY|O_CREAT|(append ? O_APPEND : O_TRUNC), and at most one on fd 0
//     opened O_RDONLY. There is no third slot, and no close or dup.
//   * SYS_WAITPID writes the child's RAW exit code, not the wait(2) encoding,
//     because proc_wait() does `if (status) *status = exit_code;`.
//
// This TU never includes a MayteraOS libc header, and spawn_test.c never
// includes a glibc one; MayteraOS sigset_t is an unsigned long while glibc's
// is a struct, so mixing the two in one translation unit does not compile.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

// MayteraOS syscall numbers are NOT hardcoded here. run_spawn.sh greps them
// out of userland/libc/syscall.h AND kernel/proc/syscall.h, fails if the two
// disagree, and passes them in as -D. A wrong number cannot silently pass.
#ifndef M_SYS_OPEN
#error "run_spawn.sh must define the syscall numbers"
#endif

// ---------------------------------------------------------------------------
// Observation of what the library asked the kernel to do
// ---------------------------------------------------------------------------
int  t_calls = 0;
long t_last_num = 0;
char t_last_path[512];
char t_last_infile[512];
char t_last_outfile[512];
int  t_last_argc = -1;
int  t_last_append = -1;
int  t_last_had_redir = -1;

void t_reset(void) {
    t_calls = 0;
    t_last_num = 0;
    t_last_path[0] = t_last_infile[0] = t_last_outfile[0] = '\0';
    t_last_argc = -1;
    t_last_append = -1;
    t_last_had_redir = -1;
}

// ---------------------------------------------------------------------------
// Services the freestanding test TU asks for
// ---------------------------------------------------------------------------
static char g_dir[512];

void t_out(const char *s) { fputs(s, stdout); fflush(stdout); }
void t_outd(long v)       { printf("%ld", v); fflush(stdout); }
const char *t_dir(void)   { return g_dir; }
void t_setenv(const char *k, const char *v) { setenv(k, v, 1); }

long t_slurp(const char *path, char *buf, long cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { if (cap > 0) buf[0] = '\0'; return -1; }
    long n = (long)read(fd, buf, (size_t)cap - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return n;
}

// glibc before 2.38 has neither of these, and the shipping libc string.h
// declares both, so the freestanding objects need them at link time.
unsigned long strlcpy(char *d, const char *s, unsigned long cap) {
    unsigned long sl = strlen(s);
    if (cap) { unsigned long n = sl < cap - 1 ? sl : cap - 1; memcpy(d, s, n); d[n] = '\0'; }
    return sl;
}
unsigned long strlcat(char *d, const char *s, unsigned long cap) {
    unsigned long dl = strnlen(d, cap);
    if (dl == cap) return cap + strlen(s);
    return dl + strlcpy(d + dl, s, cap - dl);
}

// ---------------------------------------------------------------------------
// The kernel model
// ---------------------------------------------------------------------------

// spawn_impl(): fat_read_file() then elf_validate(), both before the process
// is created. Either failing is a -1 return and no child.
static int model_file_ok(const char *path) {
    unsigned char hdr[4];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, hdr, 4);
    close(fd);
    if (n < 4) return 0;
    return hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F';
}

static long model_spawn(long num, const char *path, char **argv, int argc,
                        const char *infile, const char *outfile, int append) {
    t_calls++;
    t_last_num = num;
    snprintf(t_last_path, sizeof(t_last_path), "%s", path ? path : "");
    snprintf(t_last_infile, sizeof(t_last_infile), "%s", infile ? infile : "");
    snprintf(t_last_outfile, sizeof(t_last_outfile), "%s", outfile ? outfile : "");
    t_last_argc = argc;
    t_last_append = append;
    t_last_had_redir = (infile || outfile) ? 1 : 0;

    if (!path || argc < 0) return -1;
    if (!model_file_ok(path)) return -1;

    char *av[66];
    int n = argc > 64 ? 64 : argc;
    for (int i = 0; i < n; i++) av[i] = argv[i];
    av[n] = NULL;

    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        if (outfile && outfile[0]) {
            int fd = open(outfile, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
            if (fd >= 0) { dup2(fd, 1); if (fd != 1) close(fd); }
        }
        if (infile && infile[0]) {
            int fd = open(infile, O_RDONLY);
            if (fd >= 0) { dup2(fd, 0); if (fd != 0) close(fd); }
        }
        execv(path, av);
        _exit(127);
    }
    return (long)p;
}

// MayteraOS O_* values, taken from userland/libc/fcntl.h via run_spawn.sh, are
// not Linux's. Translate rather than assume.
static int model_oflags(int mos) {
    int lin = 0;
    int acc = mos & 0x3;
    lin |= (acc == 1) ? O_WRONLY : (acc == 2) ? O_RDWR : O_RDONLY;
    if (mos & 0x0040) lin |= O_CREAT;
    if (mos & 0x0200) lin |= O_TRUNC;
    if (mos & 0x0400) lin |= O_APPEND;
    return lin;
}

static void unexpected(const char *which, long num) {
    fprintf(stderr, "\nspawn_test: %s issued unexpected syscall %ld\n", which, num);
    exit(2);
}

long syscall0(long num) { unexpected("syscall0", num); return -1; }
long syscall4(long num, long a, long b, long c, long d) {
    (void)a; (void)b; (void)c; (void)d; unexpected("syscall4", num); return -1;
}
long syscall5(long num, long a, long b, long c, long d, long e) {
    (void)a; (void)b; (void)c; (void)d; (void)e; unexpected("syscall5", num); return -1;
}

long syscall1(long num, long a1) {
    if (num == M_SYS_CLOSE) return close((int)a1);
    unexpected("syscall1", num);
    return -1;
}

long syscall2(long num, long a1, long a2) {
    if (num == M_SYS_OPEN) return open((const char *)a1, model_oflags((int)a2), 0644);
    unexpected("syscall2", num);
    return -1;
}

long syscall3(long num, long a1, long a2, long a3) {
    if (num == M_SYS_READ) return (long)read((int)a1, (void *)a2, (size_t)a3);
    if (num == M_SYS_SPAWN_ARGS)
        return model_spawn(num, (const char *)a1, (char **)a2, (int)a3, NULL, NULL, 0);
    if (num == M_SYS_WAITPID) {
        int raw = 0;
        pid_t r = wait4((pid_t)a1, &raw, 0, NULL);
        if (r < 0) return -ECHILD;
        // proc_wait(): *status = child->exit_code, the RAW code.
        if (a2) *(int *)a2 = WIFEXITED(raw) ? WEXITSTATUS(raw) : 0;
        return (long)r;
    }
    unexpected("syscall3", num);
    return -1;
}

long syscall6(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    if (num == M_SYS_SPAWN_REDIR)
        return model_spawn(num, (const char *)a1, (char **)a2, (int)a3,
                           (const char *)a4, (const char *)a5, (int)a6);
    unexpected("syscall6", num);
    return -1;
}

int spawn_tests(void);

int main(void) {
    const char *d = getenv("SPAWNTEST_DIR");
    if (!d) { fprintf(stderr, "SPAWNTEST_DIR not set\n"); return 2; }
    snprintf(g_dir, sizeof(g_dir), "%s", d);
    printf("=== #745 posix_spawn test: shipping ../spawn.c over a model of "
           "kernel/proc/syscall.c spawn_impl() ===\n");
    int f = spawn_tests();
    printf("\n%s\n", f == 0 ? "posix_spawn test: PASS" : "posix_spawn test: FAIL");
    return f == 0 ? 0 : 1;
}
