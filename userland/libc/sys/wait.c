// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/wait.c
#include "wait.h"
#include "../syscall.h"
#include "../errno.h"

// #745 (local queue 92, found while implementing posix_spawn): this used to be
//
//     syscall2(SYS_WAIT, pid, (long)status)
//
// and it could never work. The kernel's dispatcher reads SYS_WAIT (3) as
//
//     case SYS_WAIT: return proc_wait(-1, (int *)arg1);
//
// so arg1 is the STATUS OUT-POINTER, not the pid. Every call therefore handed
// the kernel a small integer where a user pointer belonged. It did not corrupt
// anything, because the #503 argtab carries Desc { num: 3, args: [wf(4), ...] }
// and refused the bogus pointer, but that means waitpid() and wait() failed on
// EVERY call, never filled in *status, and ignored `pid` entirely.
//
// SYS_WAITPID (98) is the call that has always had the right shape, and it is
// what msh has been using directly all along:
//
//     case SYS_WAITPID: return proc_wait((int)arg1, (int *)arg2);
//
// posix_spawn() is not much use without a working way to collect the child's
// exit status, so this is fixed here rather than left for later.
//
// STILL TRUE, and deliberately not papered over: `options` is accepted and
// passed but the kernel ignores it, so WNOHANG does not poll. proc_wait()
// blocks until a matching child is a zombie.
//
// ALSO STILL TRUE: proc_wait() writes the child's RAW exit code, not the
// POSIX wait(2) encoding. WEXITSTATUS() in wait.h shifts down by 8 and will
// therefore report 0 for a child that exited 7. Compare *status directly
// until the kernel side is changed to encode it properly.
pid_t waitpid(pid_t pid, int *status, int options) {
    long r = syscall3(SYS_WAITPID, pid, (long)status, options);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (pid_t)r;
}

pid_t wait(int *status) {
    return waitpid(-1, status, 0);
}
