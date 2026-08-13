// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// spawn.c - posix_spawn() over the MayteraOS kernel spawn primitives.
//
// Read spawn.h first. It records WHY this exists (there is no execve), exactly
// which file actions the kernel can express, and which ones return ENOSYS.
// This file is the mapping; the header is the contract.
//
// THE TWO SYSCALLS THIS IS BUILT ON, taken from syscall.h and cross-checked
// against kernel/proc/syscall.c's dispatcher, not guessed:
//
//   SYS_SPAWN_ARGS  198  syscall3(path, argv, argc)
//   SYS_SPAWN_REDIR 247  syscall6(path, argv, argc, infile, outfile, append)
//
// Both land in spawn_impl() in kernel/proc/syscall.c. Both return the new pid
// on success and -1, with no further detail, on EVERY failure: missing file,
// bad ELF, no execute permission, process table full, service without
// SVC_PERM_SPAWN. Recovering a real errno from that is the job of
// spawn_diagnose() below, and it matters more than anything else here: an API
// that reports success for a program that never started is the failure mode to
// avoid.

#include "spawn.h"
#include "syscall.h"
#include "errno.h"
#include "string.h"
#include "stdlib.h"
#include "unistd.h"
#include "fcntl.h"

#ifndef O_ACCMODE
#define O_ACCMODE 0x0003
#endif

// Magic numbers, so that a posix_spawnattr_t or posix_spawn_file_actions_t
// that was never init'ed is EINVAL rather than a stack full of garbage that
// happens to translate into a plausible redirect.
#define FA_MAGIC 0x46414354   // 'FACT'
#define AT_MAGIC 0x41545452   // 'ATTR'

// Mirrored from spawn_impl(): it does `if (argc > 64) argc = 64;` and copies
// each argument with a 256-byte cap. BOTH TRUNCATE SILENTLY there. A silently
// halved argument is exactly as wrong as a silently dropped redirect, so this
// library refuses the call with E2BIG instead of passing it down.
#define SPAWN_MAX_ARGC   64
#define SPAWN_MAX_ARGLEN 255

extern char **environ;

// ---------------------------------------------------------------------------
// File actions
// ---------------------------------------------------------------------------

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa) {
    if (!fa) return EINVAL;
    memset(fa, 0, sizeof(*fa));
    fa->__magic = FA_MAGIC;
    fa->__n = 0;
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa) {
    if (!fa) return EINVAL;
    fa->__magic = 0;
    fa->__n = 0;
    return 0;
}

static struct __spawn_action *fa_next(posix_spawn_file_actions_t *fa, int *err) {
    if (!fa || fa->__magic != FA_MAGIC) { *err = EINVAL; return NULL; }
    if (fa->__n >= POSIX_SPAWN_MAX_FILE_ACTIONS) { *err = ENOMEM; return NULL; }
    *err = 0;
    return &fa->__acts[fa->__n++];
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd,
                                     const char *path, int oflag,
                                     unsigned int mode) {
    int err;
    if (fd < 0 || !path) return EBADF;
    if (strlen(path) >= POSIX_SPAWN_PATH_MAX) return ENAMETOOLONG;
    struct __spawn_action *a = fa_next(fa, &err);
    if (!a) return err;
    a->__op    = __SPAWN_OP_OPEN;
    a->__fd    = fd;
    a->__newfd = -1;
    a->__oflag = oflag;
    a->__mode  = mode;
    strlcpy(a->__path, path, sizeof(a->__path));
    return 0;
}

// addclose() and adddup2() RECORD THE ACTION AND THEN RETURN ENOSYS.
//
// That combination is deliberate and is not a mistake. The kernel spawn has no
// way to close or dup a descriptor in the child, so this can never be
// honoured. Returning ENOSYS here tells a caller that checks return values
// straight away, before it has built anything else. Recording the action
// anyway is what protects the caller that does NOT check: the action stays in
// the list, so posix_spawn() sees it and fails with ENOSYS too. Dropping it
// instead would leave posix_spawn() with nothing to object to, and it would
// cheerfully start a child whose descriptors are wrong.
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd) {
    int err;
    if (fd < 0) return EBADF;
    struct __spawn_action *a = fa_next(fa, &err);
    if (!a) return err;
    a->__op    = __SPAWN_OP_CLOSE;
    a->__fd    = fd;
    a->__newfd = -1;
    a->__oflag = 0;
    a->__mode  = 0;
    a->__path[0] = '\0';
    return ENOSYS;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd,
                                     int newfd) {
    int err;
    if (fd < 0 || newfd < 0) return EBADF;
    struct __spawn_action *a = fa_next(fa, &err);
    if (!a) return err;
    a->__op    = __SPAWN_OP_DUP2;
    a->__fd    = fd;
    a->__newfd = newfd;
    a->__oflag = 0;
    a->__mode  = 0;
    a->__path[0] = '\0';
    return ENOSYS;
}

// ---------------------------------------------------------------------------
// Attributes
// ---------------------------------------------------------------------------

int posix_spawnattr_init(posix_spawnattr_t *attr) {
    if (!attr) return EINVAL;
    memset(attr, 0, sizeof(*attr));
    attr->__magic = AT_MAGIC;
    return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *attr) {
    if (!attr) return EINVAL;
    attr->__magic = 0;
    return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags) {
    if (!attr || attr->__magic != AT_MAGIC) return EINVAL;
    // A bit we do not know about would be stored and then ignored, which is
    // the one thing this whole implementation is trying not to do.
    if (flags & ~((short)__POSIX_SPAWN_ALL_FLAGS)) return EINVAL;
    attr->__flags = flags;
    return 0;
}

int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags) {
    if (!attr || attr->__magic != AT_MAGIC || !flags) return EINVAL;
    *flags = attr->__flags;
    return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup) {
    if (!attr || attr->__magic != AT_MAGIC) return EINVAL;
    attr->__pgroup = pgroup;
    return 0;
}

int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup) {
    if (!attr || attr->__magic != AT_MAGIC || !pgroup) return EINVAL;
    *pgroup = attr->__pgroup;
    return 0;
}

int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *set) {
    if (!attr || attr->__magic != AT_MAGIC || !set) return EINVAL;
    attr->__sigmask = *set;
    return 0;
}

int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *set) {
    if (!attr || attr->__magic != AT_MAGIC || !set) return EINVAL;
    *set = attr->__sigmask;
    return 0;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *set) {
    if (!attr || attr->__magic != AT_MAGIC || !set) return EINVAL;
    attr->__sigdefault = *set;
    return 0;
}

int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *set) {
    if (!attr || attr->__magic != AT_MAGIC || !set) return EINVAL;
    *set = attr->__sigdefault;
    return 0;
}

// ---------------------------------------------------------------------------
// Translation
// ---------------------------------------------------------------------------

// Reduce the ordered action list to the only two things SYS_SPAWN_REDIR can
// carry: one input path for the child's fd 0 and one output path (plus an
// append bit) for its fd 1. Returns 0, or the errno to hand back to the
// caller. See spawn.h for the reasoning behind each rejection.
static int translate_actions(const posix_spawn_file_actions_t *fa,
                             const char **infile, const char **outfile,
                             int *append) {
    *infile = NULL;
    *outfile = NULL;
    *append = 0;
    if (!fa) return 0;
    if (fa->__magic != FA_MAGIC) return EINVAL;

    for (int i = 0; i < fa->__n; i++) {
        const struct __spawn_action *a = &fa->__acts[i];

        // close and dup2 have no kernel counterpart at all.
        if (a->__op != __SPAWN_OP_OPEN) return ENOSYS;

        int acc = a->__oflag & O_ACCMODE;

        if (a->__fd == 0) {
            // spawn_impl() opens infile with exactly O_RDONLY.
            if (acc != O_RDONLY) return ENOSYS;
            if (a->__oflag & ~(O_ACCMODE | O_CLOEXEC)) return ENOSYS;
            if (*infile) return ENOSYS;   // second open on fd 0, see spawn.h
            *infile = a->__path;
        } else if (a->__fd == 1) {
            // spawn_impl() opens outfile with
            //   O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC)
            // and nothing else, so anything the caller asks for that is not in
            // that set, or that contradicts it, has to be refused.
            if (acc != O_WRONLY) return ENOSYS;
            if (!(a->__oflag & O_CREAT)) return ENOSYS;  // kernel creates regardless
            if (a->__oflag & O_EXCL) return ENOSYS;      // kernel does not honour it
            int trunc = (a->__oflag & O_TRUNC) != 0;
            int app   = (a->__oflag & O_APPEND) != 0;
            if (trunc == app) return ENOSYS;             // neither, or both
            if (a->__oflag & ~(O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC))
                return ENOSYS;
            if (*outfile) return ENOSYS;  // second open on fd 1, see spawn.h
            *outfile = a->__path;
            *append = app;
        } else {
            // fd 2 and up: SYS_SPAWN_REDIR has two slots and no third.
            return ENOSYS;
        }
    }
    return 0;
}

// A spawned child is a fresh process image, not a copy of the caller, so some
// attributes are already true of it. See the ATTRIBUTES block in spawn.h for
// why each of these is honoured or refused; the short version is that the
// kernel memsets the whole process_t before filling it in, so sig_handlers[]
// and sig_mask start at zero.
static int check_attr(const posix_spawnattr_t *at) {
    if (!at) return 0;
    if (at->__magic != AT_MAGIC) return EINVAL;

    short f = at->__flags;

    if (f & (POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSCHEDPARAM |
             POSIX_SPAWN_SETSCHEDULER | POSIX_SPAWN_SETSID))
        return ENOSYS;

    // The child's sig_mask is 0. Asking for an empty mask is satisfied; asking
    // for a non-empty one is not, and cannot be.
    if ((f & POSIX_SPAWN_SETSIGMASK) && at->__sigmask != (sigset_t)0)
        return ENOSYS;

    // RESETIDS asks for the child's effective ids to be the caller's REAL ids.
    // The child runs as the caller (proc_as_caller()), so this is already true
    // when the caller is not running set-id, and unachievable when it is.
    if (f & POSIX_SPAWN_RESETIDS) {
        if (getuid() != geteuid() || getgid() != getegid()) return ENOSYS;
    }

    // POSIX_SPAWN_SETSIGDEF needs nothing: every handler in the child is
    // already SIG_DFL, so resetting any subset of them is genuinely satisfied.
    return 0;
}

// MayteraOS has no cross-process environment: environ lives in this libc's
// heap, and the kernel spawn carries argv and nothing else. NULL and environ
// both mean "whatever the child would have got anyway", so they are accepted;
// a deliberately constructed different environment is refused rather than
// silently discarded.
static int check_envp(char *const envp[]) {
    if (!envp) return 0;
    if ((char **)(void *)envp == environ) return 0;
    if (envp[0] == NULL) return 0;
    return ENOSYS;
}

// The kernel collapses every spawn failure to -1. Work out what actually went
// wrong, using the same filesystem the kernel just used, so the caller gets a
// real errno instead of a shrug. Only ever called on the failure path, so a
// successful spawn pays nothing for it.
static int spawn_diagnose(const char *path) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return ENOENT;

    unsigned char hdr[4] = { 0, 0, 0, 0 };
    long n = sys_read(fd, hdr, sizeof(hdr));
    sys_close(fd);

    if (n < 4) return ENOEXEC;
    if (!(hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F'))
        return ENOEXEC;

    // The file is there and it is an ELF, so the kernel refused it for a
    // reason we cannot see from here: no execute permission (spawn_impl checks
    // X_OK), a service without SVC_PERM_SPAWN, a full process table, or an ELF
    // the loader rejected. EACCES is the honest summary of "it exists, it
    // looks right, and the kernel would not run it".
    return EACCES;
}

// ---------------------------------------------------------------------------
// posix_spawn / posix_spawnp
// ---------------------------------------------------------------------------

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]) {
    const char *infile;
    const char *outfile;
    int append;
    int rc;

    if (!path || !path[0] || !argv || !argv[0]) return EINVAL;

    rc = check_attr(attrp);
    if (rc) return rc;
    rc = check_envp(envp);
    if (rc) return rc;
    rc = translate_actions(file_actions, &infile, &outfile, &append);
    if (rc) return rc;

    int argc = 0;
    while (argv[argc]) {
        if (argc >= SPAWN_MAX_ARGC) return E2BIG;
        if (strlen(argv[argc]) > SPAWN_MAX_ARGLEN) return E2BIG;
        argc++;
    }

    int r;
    if (infile || outfile)
        r = sys_spawn_redir(path, (char **)(void *)argv, argc,
                            infile, outfile, append);
    else
        r = sys_spawn_args(path, (char **)(void *)argv, argc);

    // A pid of 0 is not a valid child here either: proc pids start at 1, so
    // anything <= 0 is a failure however the kernel phrased it.
    if (r <= 0) return spawn_diagnose(path);

    if (pid) *pid = (pid_t)r;
    return 0;
}

static void upcase(const char *in, char *out, size_t cap) {
    size_t i = 0;
    for (; in[i] && i + 1 < cap; i++) {
        char c = in[i];
        out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    out[i] = '\0';
}

// Try one candidate path: if it opens, spawn it. Returns 0 on success, ENOENT
// if the candidate does not exist (keep searching), or the real error if the
// candidate exists but could not be run (stop searching, POSIX rule).
static int try_candidate(pid_t *pid, const char *cand,
                         const posix_spawn_file_actions_t *fa,
                         const posix_spawnattr_t *at,
                         char *const argv[], char *const envp[]) {
    int fd = sys_open(cand, O_RDONLY);
    if (fd < 0) return ENOENT;
    sys_close(fd);
    return posix_spawn(pid, cand, fa, at, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]) {
    if (!file || !file[0]) return EINVAL;

    // A name containing a slash is used as-is, exactly as execvp would.
    if (strchr(file, '/')) 
        return posix_spawn(pid, file, file_actions, attrp, argv, envp);

    char upper[POSIX_SPAWN_PATH_MAX];
    upcase(file, upper, sizeof(upper));

    const char *path = getenv("PATH");
    if (!path || !*path) path = "/APPS";

    // The four forms msh's resolve_path() uses, in the same order: FAT names
    // are uppercase 8.3, and installed apps carry a .ELF suffix.
    const char *p = path;
    while (*p) {
        char dir[POSIX_SPAWN_PATH_MAX];
        size_t di = 0;
        while (*p && *p != ':' && di + 1 < sizeof(dir)) dir[di++] = *p++;
        dir[di] = '\0';
        while (*p && *p != ':') p++;     // overlong element: skip its tail
        if (*p == ':') p++;
        if (!dir[0]) continue;

        for (int form = 0; form < 4; form++) {
            char cand[2 * POSIX_SPAWN_PATH_MAX];
            strlcpy(cand, dir, sizeof(cand));
            strlcat(cand, "/", sizeof(cand));
            strlcat(cand, (form & 1) ? upper : file, sizeof(cand));
            if (form >= 2) strlcat(cand, ".ELF", sizeof(cand));

            int rc = try_candidate(pid, cand, file_actions, attrp, argv, envp);
            if (rc != ENOENT) return rc;
        }
    }

    return ENOENT;
}
