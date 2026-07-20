// kill - send a signal to a process (#427/#430: real SYS_KILL, was a placeholder)
// Usage: kill [-SIG] <pid> [pid...]   (default signal 15/SIGTERM; e.g. `kill -9 42`)
#include "../../libc/maytera.h"
#include "../../libc/syscall.h"

int main(int argc, char **argv) {
    int sig = 15;   // SIGTERM
    int argi = 1;
    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] != '\0') {
        sig = atoi(argv[1] + 1);
        if (sig <= 0) { printf("kill: invalid signal '%s'\n", argv[1]); return 1; }
        argi = 2;
    }
    if (argi >= argc) { printf("usage: kill [-SIG] <pid> [pid...]\n"); return 1; }
    int rc = 0;
    for (; argi < argc; argi++) {
        int pid = atoi(argv[argi]);
        if (pid <= 0) { printf("kill: invalid pid '%s'\n", argv[argi]); rc = 1; continue; }
        long r = syscall2(SYS_KILL, (long)pid, (long)sig);
        if (r != 0) { printf("kill: (%d) - no such process or operation not permitted\n", pid); rc = 1; }
    }
    return rc;
}
