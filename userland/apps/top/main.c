// top - live process monitor for MayteraOS (#354)
// Usage: top [-n COUNT] [-d SECS]
//   -n COUNT  refresh COUNT times then exit (batch mode; good for scripts/`run`)
//   -d SECS   seconds between refreshes (default 2)
// With no -n, refreshes forever (interactive; the terminal's Ctrl+C stops it).
#include "../../libc/maytera.h"
#include "../../libc/syscall.h"
#include "../../libc/string.h"
#include "../../libc/stdlib.h"

#define MAXP 192

static char state_char(unsigned s) {
    switch (s) { case 1: case 2: return 'R'; case 3: return 'S';
                 case 4: return 'D'; case 5: return 'Z'; default: return '?'; }
}

// previous cpu_ticks keyed by pid, to compute per-interval CPU delta
static unsigned      prev_pid[MAXP];
static unsigned long long prev_cpu[MAXP];
static int prev_n = 0;

static unsigned long long prev_of(unsigned pid) {
    for (int i = 0; i < prev_n; i++) if (prev_pid[i] == pid) return prev_cpu[i];
    return 0;
}

int main(int argc, char **argv) {
    int count = -1;      // -1 = forever
    int delay = 2;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) count = (int)strtol(argv[++i], 0, 10);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) delay = (int)strtol(argv[++i], 0, 10);
    }
    if (delay < 1) delay = 1;

    static proc_info_t p[MAXP];
    int iter = 0;
    for (;;) {
        int n = sys_proc_list(p, MAXP);
        if (n < 0) { printf("top: SYS_PROC_LIST failed\n"); return 1; }

        // total CPU delta this interval, for percentage denominator
        unsigned long long total_delta = 0;
        for (int i = 0; i < n; i++) {
            unsigned long long d = p[i].cpu_ticks - prev_of(p[i].pid);
            total_delta += d;
        }
        if (total_delta == 0) total_delta = 1;

        printf("\033[2J\033[H");   // clear screen + home (VT100)
        printf("top - %d processes\n", n);
        printf("  PID  PPID S   MEM(KB)  CPU%%  NAME\n");
        for (int i = 0; i < n; i++) {
            unsigned long long d = p[i].cpu_ticks - prev_of(p[i].pid);
            unsigned pct = (unsigned)((d * 100ULL) / total_delta);
            printf("%5u %5u %c %9u %4u  %s\n",
                   p[i].pid, p[i].ppid, state_char(p[i].state),
                   p[i].mem_kb, pct, p[i].name);
        }

        // stash this snapshot as the baseline for the next interval
        prev_n = n < MAXP ? n : MAXP;
        for (int i = 0; i < prev_n; i++) { prev_pid[i] = p[i].pid; prev_cpu[i] = p[i].cpu_ticks; }

        iter++;
        if (count > 0 && iter >= count) break;
        sys_sleep((unsigned)delay * 1000u);
    }
    return 0;
}
