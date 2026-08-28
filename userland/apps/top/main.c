// top - live process monitor for MayteraOS (#354)
// Usage: top [-n COUNT] [-d SECS]
//   -n COUNT  refresh COUNT times then exit (batch mode; good for scripts/`run`)
//   -d SECS   seconds between refreshes (default 2)
// With no -n, refreshes forever (interactive; the terminal's Ctrl+C stops it).
// Rows are sorted by CPU% descending. Every screen, including the first one in
// batch mode, is a real interval measurement: see the priming sample in main().
#include "../../libc/maytera.h"
#include "../../libc/syscall.h"
#include "../../libc/string.h"
#include "../../libc/stdlib.h"
#include "../../libc/proccpu.h"   /* #178: the ONE CPU ranking, shared with taskmgr and sysmon */

// #178: one bound for every consumer, and it is the kernel's MAX_PROCESSES.
// This file used to say 192, the Task Manager 64 and sysmon 256; three guesses
// at one number, and a row outside a guess is a row with no baseline.
#define MAXP PROCCPU_MAX

static char state_char(unsigned s) {
    switch (s) { case 1: case 2: return 'R'; case 3: return 'S';
                 case 4: return 'D'; case 5: return 'Z'; default: return '?'; }
}

// #178: the previous snapshot, owned by libc. The hand-rolled version here also
// had no saturating guard on cpu_ticks - prev_of(pid), and no first-frame gate,
// so its opening screen ranked by LIFETIME CPU while its header said CPU%.
static proccpu_t cpu_state;
static proc_info_t p[MAXP];
static unsigned int pct[MAXP];

int main(int argc, char **argv) {
    int count = -1;      // -1 = forever
    int delay = 2;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) count = (int)strtol(argv[++i], 0, 10);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) delay = (int)strtol(argv[++i], 0, 10);
    }
    if (delay < 1) delay = 1;

    // #178: PRIMING SAMPLE. A CPU percentage is a difference between two
    // snapshots, so the first snapshot cannot produce one. This used to be
    // papered over by an absent baseline reading as zero, which silently turned
    // the first screen into a ranking by LIFETIME CPU under a "CPU%" header:
    // on a box with any uptime that named whoever had accumulated the most
    // since boot, not whoever is busy now, and `top -n 1` is documented for
    // scripts, so that WAS the whole output in batch mode. Now the first
    // screen printed is a real interval. It costs one -d interval of latency.
    {
        int n0 = sys_proc_list(p, MAXP);
        if (n0 < 0) { printf("top: SYS_PROC_LIST failed\n"); return 1; }
        proccpu_rank(&cpu_state, p, n0, pct);
        sys_sleep((unsigned)delay * 1000u);
    }

    int iter = 0;
    for (;;) {
        int n = sys_proc_list(p, MAXP);
        if (n < 0) { printf("top: SYS_PROC_LIST failed\n"); return 1; }

        // #178: the #145 invariants (idle in the denominator, idle out of the
        // list, baselines matched by pid) are in libc/proccpu.c, once. n is the
        // NON-IDLE row count after ranking; p[] and pct[] are compacted in step.
        n = proccpu_rank(&cpu_state, p, n, pct);
        // #178: and it now SORTS. A tool named `top` printed in raw process
        // table order, so the consumer it exists to surface could be anywhere
        // in the list. Same shared stable sort the Task Manager uses.
        proccpu_sort(p, pct, n);

        printf("\033[2J\033[H");   // clear screen + home (VT100)
        printf("top - %d processes\n", n);
        printf("  PID  PPID S   MEM(KB)  CPU%%  NAME\n");
        for (int i = 0; i < n; i++) {
            printf("%5u %5u %c %9u %4u  %s\n",
                   p[i].pid, p[i].ppid, state_char(p[i].state),
                   p[i].mem_kb, pct[i], p[i].name);
        }

        iter++;
        if (count > 0 && iter >= count) break;
        sys_sleep((unsigned)delay * 1000u);
    }
    return 0;
}
