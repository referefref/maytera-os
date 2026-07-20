// signal.h - POSIX-ish signal state and delivery hook (Phase D1)
//
// Phase D1 only lays the plumbing: a signal-pending bitmap and handler
// table on each process, a sig_raise() that queues a signal, and the
// return_work_handler() hook that syscall_return_path calls before SYSRET.
//
// Real frame building, sigaction(), sigprocmask(), kill(), alarm(), and
// rt_sigreturn live in Phase D2 (see proc/signal.c once it arrives).

#ifndef PROC_SIGNAL_H
#define PROC_SIGNAL_H

#include "../types.h"

struct process;

// POSIX signal numbers. We only promise semantics for the ones an
// interactive shell + vi use; the rest are reserved so userland can refer
// to them without fear.
#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGCHLD     17
#define SIGCONT     18
#define SIGSTOP     19
#define SIGTSTP     20
#define SIGTTIN     21
#define SIGTTOU     22
#define SIGURG      23
#define SIGWINCH    28

#define NSIG        64

// Special handler values.
#define SIG_DFL     ((void *)0)
#define SIG_IGN     ((void *)1)

// sigprocmask "how" values.
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// sigaction flags.
#define SA_RESTART   0x10000000
#define SA_SIGINFO   0x00000004
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

// A minimal kernel/userland-shared sigaction structure. Matches layout
// expected by the user libc's signal() and sigaction() wrappers.
// #430: layout MUST match the userland libc/signal.c struct __k_sigaction
// exactly, or the smuggled trampoline address is read from the wrong offset.
// Userland is: sa_handler(8) sa_mask(8) sa_flags(4) [pad4] sa_restorer(8)
// __reserved(8). The old kernel struct put __reserved at offset 20 (a 32-bit
// field) while userland writes the trampoline at offset 32 -> the kernel read
// 0 and reported "No trampoline", terminating any process that installed a
// real handler. These fields now line up byte-for-byte.
typedef struct k_sigaction {
    void    *sa_handler;     // @0  function pointer or SIG_DFL/SIG_IGN
    uint64_t sa_mask;        // @8  signals blocked while handler runs
    uint32_t sa_flags;       // @16 SA_* bits
    uint32_t __pad;          // @20 alignment (matches userland's implicit pad)
    void    *sa_restorer;    // @24 unused by the kernel; present for layout
    uint64_t __reserved;     // @32 libc smuggles the signal trampoline here
} k_sigaction_t;

// Layout of the signal frame we push on the user stack before redirecting
// the saved IRET frame to the handler. Order matches what sys_rt_sigreturn
// reads back (field-for-field).
typedef struct sigframe {
    uint64_t saved_rax, saved_rbx, saved_rcx, saved_rdx;
    uint64_t saved_rsi, saved_rdi, saved_rbp;
    uint64_t saved_r8, saved_r9, saved_r10, saved_r11;
    uint64_t saved_r12, saved_r13, saved_r14, saved_r15;
    uint64_t saved_rip, saved_rflags, saved_rsp;
    uint64_t saved_mask;  // sig_mask to restore
    uint32_t signo;
    uint32_t __pad;
} sigframe_t;

// ---- API ----

// Queue a signal for delivery to target. Sets the pending bit; if the
// target is interruptibly sleeping on a wait queue, wake it so that the
// syscall unwinds with -EINTR. Safe from interrupt context.
void sig_raise(struct process *target, int signo);

// Phase D4: raise a signal to every process in a process group (for TTY
// line-discipline Ctrl-C/Z and shell job control).
void sig_raise_pgrp(uint32_t pgrp, int signo);

// Returns a bitmask of signals that are pending-and-not-masked on target.
// Zero means nothing to do. Used by syscall_return_path before falling
// into the delivery path.
uint64_t sig_deliverable(struct process *target);

// Called from syscall.asm (and, later, the IRETQ path) when the current
// process's return_work field is non-zero. At Phase D1 this is a no-op
// stub; Phase D2 fills in signal frame construction and Phase G fills in
// the exec-pending swap. Safe to call with return_work == 0.
void return_work_handler(void *user_frame);

#endif // PROC_SIGNAL_H
