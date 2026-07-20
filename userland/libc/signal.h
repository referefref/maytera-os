// signal.h - POSIX signals for MayteraOS userland
#ifndef LIBC_SIGNAL_H
#define LIBC_SIGNAL_H

#include "types.h"

#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGWINCH  28
#define NSIG      32

#define SIG_DFL   ((void (*)(int))0)
#define SIG_IGN   ((void (*)(int))1)
#define SIG_ERR   ((void (*)(int))-1)

// sa_flags
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

// how for sigprocmask
#define SIG_BLOCK     0
#define SIG_UNBLOCK   1
#define SIG_SETMASK   2

typedef unsigned long sigset_t;
typedef volatile int sig_atomic_t;

struct sigaction {
    void     (*sa_handler)(int);
    sigset_t   sa_mask;
    int        sa_flags;
    void     (*sa_restorer)(void);
};

typedef void (*sighandler_t)(int);

// POSIX API
sighandler_t signal(int signum, sighandler_t handler);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oset);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigdelset(sigset_t *set, int signum);
int sigismember(const sigset_t *set, int signum);
int kill(int pid, int sig);
int raise(int sig);
unsigned int alarm(unsigned int sec);
int pause(void);

// Signal trampoline (called by kernel; invokes handler then sys_rt_sigreturn)
extern void __sig_trampoline(void);

#endif // LIBC_SIGNAL_H
