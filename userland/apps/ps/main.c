// ps - process listing for MayteraOS (#354, real: uses SYS_PROC_LIST 238)
#include "../../libc/maytera.h"
#include "../../libc/syscall.h"

static char state_char(unsigned s) {
    switch (s) {
        case 1: return 'R';  // READY
        case 2: return 'R';  // RUNNING
        case 3: return 'S';  // SLEEPING
        case 4: return 'D';  // BLOCKED (uninterruptible-ish)
        case 5: return 'Z';  // ZOMBIE
        default: return '?';
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    static proc_info_t p[192];
    int n = sys_proc_list(p, 192);
    if (n < 0) {
        printf("ps: SYS_PROC_LIST failed\n");
        return 1;
    }
    printf("  PID  PPID S    MEM(KB)        CPU CPU# NAME\n");
    for (int i = 0; i < n; i++) {
        printf("%5u %5u %c %10u %10llu %4d %s\n",
               p[i].pid, p[i].ppid, state_char(p[i].state),
               p[i].mem_kb, p[i].cpu_ticks, p[i].running_cpu,
               p[i].name);
    }
    printf("(%d processes)\n", n);
    return 0;
}
