// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// spawn.h - POSIX posix_spawn() for MayteraOS userland.
//
// ============================================================================
// WHY THIS EXISTS, AND WHAT IT DELIBERATELY DOES NOT DO. READ THIS FIRST.
// ============================================================================
//
// MayteraOS creates processes by SPAWN, not by fork-then-exec. fork() is real
// (it calls proc_fork(), which copies the whole PCB and address space), but
// there is no exec: SYS_EXECVE (103) is declared in syscall.h and has NO case
// in the kernel dispatcher, so the classic POSIX idiom cannot complete. The
// kernel primitives that DO work are:
//
//   SYS_SPAWN_ARGS  (198)  path, argv, argc
//   SYS_SPAWN_REDIR (247)  path, argv, argc, infile, outfile, append
//
// posix_spawn is the POSIX-blessed API that maps onto exactly that shape, so
// this is the call to reach for. It covers the overwhelmingly common shape of
// "fork, adjust a couple of file descriptors, exec".
//
// IT DOES NOT COVER, AND IS NOT INTENDED TO COVER:
//
//   * Code that forks and then does SUBSTANTIAL WORK IN THE CHILD before
//     exec'ing (setrlimit, chdir, chroot, custom signal wiring, arbitrary
//     computation). posix_spawn has no "run this code in the child" hook by
//     design, and neither does the kernel spawn.
//
//   * Code that forks and NEVER exec's, i.e. uses the child as a copy of the
//     parent (the classic pre-forking server, or a worker that inherits the
//     parent's heap). Use fork() directly for that; it genuinely works here.
//
//   * Replacing the CURRENT process image. That is execve(), and it is a
//     DELIBERATE NON-GOAL for now. A real execve has to tear down and rebuild
//     the calling process's address space while executing on the very page
//     tables it is dismantling, retarget the ELF loader at an existing
//     process_t rather than a fresh one, and implement FD_CLOEXEC. That is
//     weeks of work in the most dangerous part of the kernel, and posix_spawn
//     buys most of the value without touching any of it.
//
// ============================================================================
// FILE ACTIONS: WHAT THE KERNEL CAN AND CANNOT EXPRESS
// ============================================================================
//
// The whole point of posix_spawn is that the caller can say "in the child,
// open this as fd 1" without a fork to do it in. SYS_SPAWN_REDIR gives us
// exactly TWO fd slots and no more, because kernel/proc/syscall.c
// spawn_impl() installs at most one file_t on the child's fds[1] and one on
// its fds[0] after proc_create_user_as() returns. Everything else about the
// child's fd table is fixed: fds[0..2] are inherited from the caller.
//
// EXPRESSIBLE (this library maps it onto SYS_SPAWN_REDIR and it really happens)
//   addopen(0, path, O_RDONLY, mode)
//   addopen(1, path, O_WRONLY|O_CREAT|O_TRUNC,  mode)
//   addopen(1, path, O_WRONLY|O_CREAT|O_APPEND, mode)
//
// NOT EXPRESSIBLE (posix_spawn returns ENOSYS; it does NOT quietly proceed)
//   addclose(fd)            no kernel primitive closes a child's fd at spawn
//   adddup2(fd, newfd)      no kernel primitive dups within the child
//   addopen on fd 2 or any fd >= 2   only two redirect slots exist
//   addopen(0, ...) for writing, or addopen(1, ...) for reading
//   addopen(1, ...) without O_CREAT  the kernel creates unconditionally
//   addopen(1, ...) without O_TRUNC or O_APPEND   the kernel truncates
//   O_EXCL, or O_RDWR, on either slot
//   two open actions targeting the SAME fd: the first one's create/truncate
//     side effect would be silently lost, and losing it silently is the bug
//     this whole header is trying to avoid
//
// A spawn that silently ignored a file action would be WORSE than one that
// fails: a shell pipeline built on it would run with the wrong descriptors and
// produce wrong output with no error anywhere. So every case above is a hard
// ENOSYS from posix_spawn(), never a best-effort.
//
// THE SMALLEST KERNEL CHANGE THAT WOULD LIFT THIS (not implemented here):
// one new syscall, SYS_SPAWN_FDACT(path, argv, argc, acts, nacts), where acts
// is an array of {op, fd, newfd, oflag, path[]} applied IN ORDER to the child
// process_t's fds[] between proc_create_user_as() and the return, reusing the
// open_redir_file() helper that spawn_impl() already has and file_get/file_put
// for dup2/close. It needs no new address-space work, no change to the ELF
// loader, and no change to the calling process, which is exactly why it is the
// small change and execve is not. Per project policy it should be written in
// Rust, and its number has to be registered in all five syscall-number
// locations that repo-guard --strict checks.
//
// ============================================================================
// ATTRIBUTES: WHAT IS HONOURED, AND WHY
// ============================================================================
//
// A spawned child is a FRESH process image, not a copy of the caller:
// kernel/proc/process.c memsets the entire process_t under the table lock
// before filling it in. So some attributes are satisfied BY CONSTRUCTION and
// are honoured as no-ops, while others cannot be expressed at all.
//
//   POSIX_SPAWN_SETSIGDEF     HONOURED. The child's sig_handlers[] are all
//                             NULL (SIG_DFL) already, so resetting any subset
//                             of them to default is a no-op that is genuinely
//                             satisfied, not one that is being papered over.
//   POSIX_SPAWN_SETSIGMASK    HONOURED ONLY FOR AN EMPTY MASK. The child's
//                             sig_mask is 0. We cannot make the child start
//                             with signals blocked, so a non-empty requested
//                             mask returns ENOSYS.
//   POSIX_SPAWN_RESETIDS      HONOURED ONLY WHEN uid == euid AND gid == egid.
//                             The child runs as the caller (proc_as_caller()),
//                             so when the caller is not set-id the request is
//                             already satisfied; otherwise ENOSYS.
//   POSIX_SPAWN_SETPGROUP     ENOSYS. SYS_SETPGID is declared in syscall.h and
//                             has no case in the kernel dispatcher, so there
//                             is nothing to call even after the fact. The
//                             child inherits the caller's pgrp.
//   POSIX_SPAWN_SETSCHEDULER  ENOSYS. No scheduling-policy syscall exists.
//   POSIX_SPAWN_SETSCHEDPARAM ENOSYS. Same.
//   POSIX_SPAWN_SETSID        ENOSYS. The child inherits the caller's session.
//
// ENVIRONMENT. envp cannot be honoured, because MayteraOS has NO cross-process
// environment at all: environ lives in libc's heap (stdlib.c), the kernel
// spawn carries argv and nothing else, and every process starts with an empty
// environ that it or its shell repopulates. Passing NULL or environ is
// accepted, because on this OS both mean the same thing and refusing them
// would reject every portable caller for a limitation of the OS rather than of
// this call. Passing a DIFFERENT, deliberately constructed envp returns
// ENOSYS, because that caller is asking for something specific that will not
// happen.
//
// FILE MODE. The mode argument to addopen() is the one parameter this library
// knowingly ignores: the kernel's open_redir_file() calls
// perms_on_create(path, euid, egid, 0), so a created redirect target gets the
// filesystem default rather than the requested mode. It is recorded in the
// action and documented here rather than rejected, because rejecting it would
// fail every caller that passes the customary 0644.
//
// RETURN CONVENTION. posix_spawn() and posix_spawnp() return 0 on success or
// the ERROR NUMBER directly, exactly as POSIX specifies. They do NOT return -1
// and they do NOT set errno. Check the return value.
//
// ============================================================================

#ifndef LIBC_SPAWN_H
#define LIBC_SPAWN_H

#include "types.h"
#include "signal.h"

#ifndef __mode_t_defined_spawn
typedef unsigned int __spawn_mode_t;
#endif

// posix_spawnattr_t flags. Values match glibc so that ported code comparing
// against literals behaves the same.
#define POSIX_SPAWN_RESETIDS        0x01
#define POSIX_SPAWN_SETPGROUP       0x02
#define POSIX_SPAWN_SETSIGDEF       0x04
#define POSIX_SPAWN_SETSIGMASK      0x08
#define POSIX_SPAWN_SETSCHEDPARAM   0x10
#define POSIX_SPAWN_SETSCHEDULER    0x20
#define POSIX_SPAWN_SETSID          0x80

// Every flag this implementation knows about. setflags() rejects anything else
// with EINVAL rather than storing a bit posix_spawn() would then ignore.
#define __POSIX_SPAWN_ALL_FLAGS \
    (POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | \
     POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSCHEDPARAM | \
     POSIX_SPAWN_SETSCHEDULER | POSIX_SPAWN_SETSID)

// Caps. These objects are routinely declared on the stack, and a MayteraOS
// user thread stack is small, so the storage is fixed and bounded rather than
// malloc'ed. addopen/addclose/adddup2 return ENOMEM past the action cap and
// ENAMETOOLONG past the path cap; neither ever truncates.
#define POSIX_SPAWN_MAX_FILE_ACTIONS 8
#define POSIX_SPAWN_PATH_MAX         128

#define __SPAWN_OP_OPEN   1
#define __SPAWN_OP_CLOSE  2
#define __SPAWN_OP_DUP2   3

struct __spawn_action {
    int          __op;      // __SPAWN_OP_*
    int          __fd;      // target fd (open, close) or source fd (dup2)
    int          __newfd;   // dup2 destination
    int          __oflag;   // open flags
    unsigned int __mode;    // open mode (recorded, see FILE MODE above)
    char         __path[POSIX_SPAWN_PATH_MAX];
};

typedef struct {
    int                   __magic;
    int                   __n;
    struct __spawn_action __acts[POSIX_SPAWN_MAX_FILE_ACTIONS];
} posix_spawn_file_actions_t;

typedef struct {
    int      __magic;
    short    __flags;
    pid_t    __pgroup;
    sigset_t __sigdefault;
    sigset_t __sigmask;
    int      __policy;
    int      __priority;
} posix_spawnattr_t;

// ---------------------------------------------------------------------------
// The calls. All of them return 0 or an error number; none sets errno.
// ---------------------------------------------------------------------------

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]);

// posix_spawnp: as posix_spawn, but a path with no '/' is searched for along
// PATH (defaulting to "/APPS"). Each element is tried as "<dir>/<name>",
// "<dir>/<NAME>", "<dir>/<name>.ELF" and "<dir>/<NAME>.ELF", which are the
// same four forms msh's resolve_path() uses, because FAT names are uppercase
// 8.3 and installed apps carry a .ELF suffix.
int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]);

int posix_spawnattr_init(posix_spawnattr_t *attr);
int posix_spawnattr_destroy(posix_spawnattr_t *attr);
int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags);
int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags);
int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup);
int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *set);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *set);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *set);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *set);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd,
                                     const char *path, int oflag,
                                     unsigned int mode);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd,
                                     int newfd);

#endif // LIBC_SPAWN_H
