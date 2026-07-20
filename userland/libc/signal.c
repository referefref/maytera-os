// signal.c - POSIX signal wrappers for MayteraOS userland
#include "signal.h"
#include "syscall.h"
#include "errno.h"

// Trampoline (in asm): executes sys_rt_sigreturn after handler returns.
// Kernel reads __reserved field of the first sigaction call and stashes
// the trampoline address there as g_sig_trampoline.
extern void __sig_trampoline(void);

// sigaction structure passed to the kernel. We smuggle the trampoline
// address in the (otherwise unused) sa_restorer slot.
struct __k_sigaction {
    void     (*sa_handler)(int);
    unsigned long sa_mask;
    int        sa_flags;
    void     (*sa_restorer)(void);
    unsigned long __reserved;   // <= trampoline goes here on first call
};

int sigaction(int signum, const struct sigaction *act, struct sigaction *oact) {
    struct __k_sigaction k_act;
    struct __k_sigaction k_oact;
    struct __k_sigaction *pk_act = 0;
    struct __k_sigaction *pk_oact = 0;

    if (act) {
        k_act.sa_handler = act->sa_handler;
        k_act.sa_mask    = act->sa_mask;
        k_act.sa_flags   = act->sa_flags;
        k_act.sa_restorer = act->sa_restorer;
        k_act.__reserved = (unsigned long)&__sig_trampoline;
        pk_act = &k_act;
    }
    if (oact) {
        pk_oact = &k_oact;
    }

    long r = syscall3(SYS_SIGACTION, signum, (long)pk_act, (long)pk_oact);

    if (r < 0) {
        errno = (int)(-r);
        return -1;
    }

    if (oact) {
        oact->sa_handler = k_oact.sa_handler;
        oact->sa_mask    = k_oact.sa_mask;
        oact->sa_flags   = k_oact.sa_flags;
        oact->sa_restorer = k_oact.sa_restorer;
    }
    return 0;
}

sighandler_t signal(int signum, sighandler_t handler) {
    struct sigaction sa, osa;
    sa.sa_handler = handler;
    sa.sa_mask = 0;
    sa.sa_flags = SA_RESTART;
    sa.sa_restorer = 0;
    if (sigaction(signum, &sa, &osa) < 0) return SIG_ERR;
    return osa.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oset) {
    long r = syscall3(SYS_SIGPROCMASK, how, (long)set, (long)oset);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

int sigemptyset(sigset_t *set) { *set = 0; return 0; }
int sigfillset(sigset_t *set)  { *set = ~0UL; return 0; }
int sigaddset(sigset_t *set, int signum) {
    if (signum < 1 || signum >= NSIG) { errno = EINVAL; return -1; }
    *set |= (1UL << (signum - 1));
    return 0;
}
int sigdelset(sigset_t *set, int signum) {
    if (signum < 1 || signum >= NSIG) { errno = EINVAL; return -1; }
    *set &= ~(1UL << (signum - 1));
    return 0;
}
int sigismember(const sigset_t *set, int signum) {
    if (signum < 1 || signum >= NSIG) { errno = EINVAL; return -1; }
    return (*set & (1UL << (signum - 1))) ? 1 : 0;
}

int kill(int pid, int sig) {
    long r = syscall2(SYS_KILL, pid, sig);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

int raise(int sig) {
    long pid = syscall0(SYS_GETPID);
    return kill((int)pid, sig);
}

unsigned int alarm(unsigned int sec) {
    return (unsigned int)syscall1(SYS_ALARM, sec);
}

int pause(void) {
    long r = syscall0(SYS_PAUSE);
    if (r < 0) errno = (int)(-r);
    return -1;
}
