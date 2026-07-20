// sys/wait.c
#include "wait.h"
#include "../syscall.h"
#include "../errno.h"

pid_t waitpid(pid_t pid, int *status, int options) {
    (void)options;  // MVP ignores WNOHANG
    long r = syscall2(SYS_WAIT, pid, (long)status);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (pid_t)r;
}

pid_t wait(int *status) {
    return waitpid(-1, status, 0);
}
