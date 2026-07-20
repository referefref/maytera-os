
#ifndef COMPAT_SIGNAL_H
#define COMPAT_SIGNAL_H

typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

/* Signal numbers (not used but referenced by mdport) */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGIOT   6
#define SIGABRT  6
#define SIGEMT   7
#define SIGFPE   8
#define SIGBUS   10
#define SIGSEGV  11
#define SIGSYS   12
#define SIGTERM  15
#define SIGTSTP  18
#define SIGCONT  18
#define SIGWINCH 28

static inline sighandler_t signal(int sig, sighandler_t handler) {
    (void)sig; (void)handler; return SIG_DFL;
}

typedef unsigned long sigset_t;
static inline int sigprocmask(int h, const sigset_t *s, sigset_t *o) {
    (void)h;(void)s;(void)o; return 0;
}

#endif
