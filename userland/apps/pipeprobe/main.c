// pipeprobe - #111 measurement instrument for the three pipe-layer defects.
//
// THIS EXISTS TO BE ABLE TO FAIL. Every check below is run FIRST on an
// unmodified golden, where the ones it targets must report FAIL, and only then
// on a fixed kernel, where they must report PASS. A check that has only ever
// been seen passing proves nothing about the thing it points at.
//
// The three defects, as stated in #111:
//   (a) no blocking write   a producer that outruns its consumer gets 0 back
//                           from write(2) and spins instead of sleeping.
//   (b) no SIGPIPE          a write to a pipe with no reader returns an error
//                           and nothing is raised, so a producer that ignores
//                           its write result never terminates.
//   (c) two fd number spaces sharing one range.
//
// WHY CHECK A MEASURES CPU AND NOT WALL CLOCK: a spin and a sleep both finish.
// The reader child deliberately idles before draining, so in BOTH worlds the
// parent takes about the same wall time. Only the parent's own cpu_ticks tells
// the two apart, which is why SELFTEST 2 below has to prove that counter both
// moves AND discriminates before any Check A number is believed.
//
// No em-dashes per repo style.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "spawn.h"
#include "signal.h"
#include "errno.h"
#include "sys/wait.h"
#include "fcntl.h"
#include "syscall.h"

#define TAG  "[T111]"
#define SELF "/APPS/PIPEPROB"

#define PIPE_CAP    65536      /* kernel/fs/pipe.c PIPE_BUF_SIZE */
#define CHUNK       4096
#define OVERRUN     (4 * PIPE_CAP)   /* write 4x the ring, so it MUST fill */
#define READER_IDLE 1500       /* ms the reader stalls before draining */

static unsigned long long self_cpu_ticks(void)
{
    proc_detail_t d;
    memset(&d, 0, sizeof d);
    if (sys_proc_detail((unsigned)getpid(), &d) != 1) return 0ULL;
    return d.cpu_ticks;
}

/* A burn that the compiler cannot delete, used only to prove the cpu_ticks
   counter responds to real CPU consumption. */
static volatile unsigned long g_sink;
static void burn_ms(unsigned long ms)
{
    unsigned long end = uptime_ms() + ms;
    unsigned long x = 1;
    while (uptime_ms() < end) {
        for (int i = 0; i < 2000; i++) x = x * 1103515245UL + 12345UL;
        g_sink = x;
    }
}

/* ------------------------------------------------------------------ roles */

/* Child role for Check A. Stalls, THEN drains, so the parent is guaranteed to
   hit a full ring with a reader that is alive but not yet consuming. */
static int role_drain(unsigned long idle_ms, unsigned long total)
{
    static char buf[CHUNK];
    sys_sleep((unsigned)idle_ms);
    unsigned long got = 0;
    while (got < total) {
        long r = read(0, buf, sizeof buf);
        if (r <= 0) break;
        got += (unsigned long)r;
    }
    printf("%s   drain-child: read %lu of %lu bytes\n", TAG, got, total);
    fflush(stdout);
    return (got == total) ? 0 : 1;
}

/* Child role for Check B. Writes to a pipe it has closed the only read end of.
   If SIGPIPE is delivered with its POSIX default action this process never
   reaches the print, and its parent sees a terminated child. If it survives it
   says so and exits 7, which is the pre-fix result. */
static int role_sigpipe(int ignore_it)
{
    int p[2];
    if (pipe(p) != 0) { printf("%s   sigpipe-child: pipe() failed\n", TAG); fflush(stdout); return 9; }
    if (ignore_it) signal(SIGPIPE, SIG_IGN);
    close(p[0]);                       /* the only reader is now gone */
    long r = write(p[1], "x", 1);
    printf("%s   sigpipe-child(ignore=%d): SURVIVED, write returned %ld\n",
           TAG, ignore_it, r);
    fflush(stdout);
    /* Encode the return value so the parent can see EPIPE vs the old -1. */
    if (ignore_it) return (r == -EPIPE || r == -32) ? 0 : 7;
    return 7;
}

/* Child role for Check C2. Reads from an fd number it never opened. */
static int role_peek(int fd)
{
    char buf[64];
    memset(buf, 0, sizeof buf);
    long r = read(fd, buf, 32);
    if (r > 0) {
        for (long i = 0; i < r; i++) if (buf[i] < 32 || buf[i] > 126) buf[i] = '.';
        printf("%s   peek-child: read(%d) returned %ld: '%s'\n", TAG, fd, r, buf);
        fflush(stdout);
        return 0;   /* 0 == it got somebody else's data */
    }
    printf("%s   peek-child: read(%d) returned %ld (no data)\n", TAG, fd, r);
    fflush(stdout);
    return 1;
}

/* ------------------------------------------------------------------ helper */

static int run_wait(char **args, int *raw_status)
{
    pid_t pid = 0;
    int st = 0;
    if (posix_spawnp(&pid, args[0], NULL, NULL, args, environ) != 0) {
        printf("%s   spawn '%s' failed\n", TAG, args[0]);
        return -1;
    }
    if (waitpid(pid, &st, 0) < 0) { printf("%s   waitpid failed\n", TAG); return -1; }
    if (raw_status) *raw_status = st;
    return st;
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "all";

    if (!strcmp(mode, "drain"))
        return role_drain(argc > 2 ? (unsigned long)atoi(argv[2]) : READER_IDLE,
                          argc > 3 ? (unsigned long)atoi(argv[3]) : OVERRUN);
    if (!strcmp(mode, "sigpipe"))  return role_sigpipe(0);
    if (!strcmp(mode, "sigpipei")) return role_sigpipe(1);
    if (!strcmp(mode, "peek"))     return role_peek(argc > 2 ? atoi(argv[2]) : 3);
    if (strcmp(mode, "all")) { printf("usage: pipeprobe [all|drain|sigpipe|sigpipei|peek]\n"); return 2; }

    int fails = 0;
    printf("%s ==== #111 pipe layer probe ====\n", TAG);
    fflush(stdout);

    /* ================= SELFTEST 1: does a pipe work at all? ============== */
    {
        int p[2];
        char in[64], out[64];
        for (int i = 0; i < 64; i++) in[i] = (char)('A' + (i % 26));
        if (pipe(p) != 0) { printf("%s INSTRUMENT BROKEN: pipe() failed\n", TAG); return 2; }
        long w = write(p[1], in, sizeof in);
        long r = read(p[0], out, sizeof out);
        if (w != 64 || r != 64 || memcmp(in, out, 64) != 0) {
            printf("%s INSTRUMENT BROKEN: pipe round-trip w=%ld r=%ld\n", TAG, w, r);
            return 2;
        }
        close(p[0]); close(p[1]);
        printf("%s selftest1: pipe 64-byte round-trip OK\n", TAG);
        fflush(stdout);
    }

    /* ========== SELFTEST 2: does cpu_ticks move, AND discriminate? ========
     * Check A's whole conclusion rests on this counter. If it were stuck at
     * zero, "the writer used no CPU" would look exactly like a perfect fix.
     * So: burn 600ms and require the counter to rise; then sleep 600ms and
     * require it to rise STRICTLY LESS. Both halves, or the instrument is not
     * measuring what Check A claims it measures. */
    unsigned long long burn_d, sleep_d;
    {
        unsigned long long a = self_cpu_ticks();
        burn_ms(600);
        unsigned long long b = self_cpu_ticks();
        burn_d = b - a;

        a = self_cpu_ticks();
        sys_sleep((unsigned)600);
        b = self_cpu_ticks();
        sleep_d = b - a;

        printf("%s selftest2: cpu_ticks burn600=%llu sleep600=%llu\n",
               TAG, burn_d, sleep_d);
        if (burn_d == 0) {
            printf("%s INSTRUMENT BROKEN: cpu_ticks did not move over a 600ms burn\n", TAG);
            return 2;
        }
        if (sleep_d >= burn_d) {
            printf("%s INSTRUMENT BROKEN: cpu_ticks does not discriminate sleep from spin\n", TAG);
            return 2;
        }
        printf("%s selftest2: instrument moves and discriminates OK\n", TAG);
        fflush(stdout);
    }
    /* Anything at or below this is "asleep"; anything near burn_d is "spinning". */
    unsigned long long spin_floor = burn_d / 4;

    /* ============ CHECK A: does a full-pipe write sleep or spin? ========= */
    {
        int p[2];
        if (pipe(p) != 0) { printf("%s CHECK A: pipe() failed\n", TAG); return 2; }

        char *a[5];
        char idle[16], tot[16];
        snprintf(idle, sizeof idle, "%d", READER_IDLE);
        snprintf(tot,  sizeof tot,  "%d", OVERRUN);
        a[0] = (char *)SELF; a[1] = (char *)"drain"; a[2] = idle; a[3] = tot; a[4] = NULL;

        /* The child inherits the READ end on its stdin; the parent keeps the
           write end and drops its own copy of the read end, so the child is
           the only reader. */
        int saved = dup(0);
        dup2(p[0], 0);
        pid_t pid = 0;
        int spawned = (posix_spawnp(&pid, a[0], NULL, NULL, a, environ) == 0);
        if (saved >= 0) { dup2(saved, 0); close(saved); }
        close(p[0]);

        if (!spawned) {
            printf("%s CHECK A: could not spawn the drain child\n", TAG);
            close(p[1]);
            fails++;
        } else {
            static char buf[CHUNK];
            memset(buf, 'Z', sizeof buf);
            unsigned long written = 0, zero_rets = 0, neg_rets = 0, calls = 0;
            unsigned long t0 = uptime_ms();
            unsigned long long c0 = self_cpu_ticks();

            while (written < OVERRUN) {
                long w = write(p[1], buf, CHUNK);
                calls++;
                if (w > 0)      written += (unsigned long)w;
                else if (w == 0) zero_rets++;
                else { neg_rets++; break; }
            }

            unsigned long long c1 = self_cpu_ticks();
            unsigned long t1 = uptime_ms();
            close(p[1]);

            int st = 0;
            waitpid(pid, &st, 0);

            unsigned long long cpu_d = c1 - c0;
            printf("%s CHECK A: wrote %lu/%d in %lu calls, "
                   "write()==0 seen %lu times, write()<0 %lu\n",
                   TAG, written, OVERRUN, calls, zero_rets, neg_rets);
            printf("%s CHECK A: wall=%lums parent_cpu_ticks=%llu (spin_floor=%llu)\n",
                   TAG, t1 - t0, cpu_d, spin_floor);

            /* Two independent conditions, both required:
             *   1. write(2) must NEVER return 0 for a non-zero count. That is
             *      not a legal blocking-write result and it is what makes the
             *      libc flush loop spin.
             *   2. the parent must have SLEPT through the reader's stall. */
            int a_ok = (zero_rets == 0) && (written == OVERRUN) && (cpu_d < spin_floor);
            printf("%s CHECK A blocking-write: %s\n", TAG, a_ok ? "PASS" : "FAIL");
            if (!a_ok) fails++;
        }
        fflush(stdout);
    }

    /* ================= CHECK B: SIGPIPE, delivery and default =========== */
    {
        int raw = 0;
        char *a[3];
        a[0] = (char *)SELF; a[1] = (char *)"sigpipe"; a[2] = NULL;
        int st = run_wait(a, &raw);
        int b_ok = (st == 141) || (WIFEXITED(raw) && WEXITSTATUS(raw) == 141) ||
                   (WIFSIGNALED(raw) && WTERMSIG(raw) == SIGPIPE);
        printf("%s CHECK B: child status raw=%d (WIFEXITED=%d WEXITSTATUS=%d "
               "WIFSIGNALED=%d WTERMSIG=%d); want terminate-by-SIGPIPE (128+13=141)\n",
               TAG, raw, WIFEXITED(raw), WEXITSTATUS(raw),
               WIFSIGNALED(raw), WTERMSIG(raw));
        printf("%s CHECK B sigpipe-default-kills: %s\n", TAG, b_ok ? "PASS" : "FAIL");
        if (!b_ok) fails++;
        fflush(stdout);
    }

    /* CHECK B2: SIG_IGN must SUPPRESS the kill and yield EPIPE instead. This is
     * what stops a "fix" that just kills unconditionally from passing Check B. */
    {
        int raw = 0;
        char *a[3];
        a[0] = (char *)SELF; a[1] = (char *)"sigpipei"; a[2] = NULL;
        int st = run_wait(a, &raw);
        int b2_ok = (st == 0) || (WIFEXITED(raw) && WEXITSTATUS(raw) == 0);
        printf("%s CHECK B2: child status raw=%d; want survive with write()==-EPIPE\n",
               TAG, raw);
        printf("%s CHECK B2 sigpipe-ignorable: %s\n", TAG, b2_ok ? "PASS" : "FAIL");
        if (!b2_ok) fails++;
        fflush(stdout);
    }

    /* ============= CHECK C: do the two fd spaces overlap? =============== */
    /* MEASURED on golden 1993: open("/BUILDINFO.TXT") returned 3 and the very
     * next pipe() ALSO returned 3 for its read end, in the same process, with
     * both objects live. No dup2 trickery is needed to produce the collision:
     * fd_alloc() scans the per-process file_t table and the legacy open scans
     * the system-wide fd_used[] table, and neither consults the other. */
    {
        int ffd = open("/BUILDINFO.TXT", O_RDONLY);
        if (ffd < 0) {
            printf("%s CHECK C: could not open a file to test with\n", TAG);
            fails++;
        } else {
            int p[2];
            if (pipe(p) != 0) {
                printf("%s CHECK C: pipe() failed\n", TAG);
                fails++;
            } else {
                printf("%s CHECK C: legacy file fd=%d, pipe fds=%d,%d\n",
                       TAG, ffd, p[0], p[1]);
                int collide = (p[0] == ffd) || (p[1] == ffd);
                printf("%s CHECK C1 fd-numbers-distinct: %s\n",
                       TAG, collide ? "FAIL (one number, two objects)" : "PASS");
                if (collide) fails++;

                /* If they collided, show WHICH object the number now names.
                 * A marker byte in the pipe makes the two answers unmistakable:
                 * "MayteraOS golden..." is the file, "#" is the pipe. Written
                 * FIRST so the read can never block (an empty pipe with a live
                 * writer blocks forever, which is correct pipe behaviour and
                 * cost this probe its first run). */
                if (collide) {
                    write(p[1], "#", 1);
                    char b1[24]; memset(b1, 0, sizeof b1);
                    long r1 = read(ffd, b1, 8);
                    for (long i = 0; i < r1 && i < 8; i++)
                        if (b1[i] < 32 || b1[i] > 126) b1[i] = '.';
                    printf("%s CHECK C1: read(%d) returned %ld '%s' -> the %s won; "
                           "the other object is unreachable and its slot leaks\n",
                           TAG, ffd, r1, b1,
                           (r1 == 1 && b1[0] == '#') ? "PIPE" : "FILE");
                }
                close(p[0]); close(p[1]);
            }

            /* C2: a DIFFERENT process reads an fd number it never opened. The
             * legacy table is SYSTEM-WIDE and its read path checks only
             * fd_used[fd], never who owns it. This is the #36 shape. */
            char num[16]; snprintf(num, sizeof num, "%d", ffd);
            char *a[4];
            a[0] = (char *)SELF; a[1] = (char *)"peek"; a[2] = num; a[3] = NULL;
            int raw = 0;
            int st = run_wait(a, &raw);
            int leaked = (st == 0);
            printf("%s CHECK C2: parent holds fd=%d; unrelated child read it -> %s\n",
                   TAG, ffd, leaked ? "GOT OUR DATA (cross-process fd leak)"
                                    : "no data (isolated)");
            printf("%s CHECK C2 fd-space-isolated: %s\n", TAG, leaked ? "FAIL" : "PASS");
            if (leaked) fails++;
            close(ffd);
        }
        fflush(stdout);
    }

    printf("%s ==== RESULT: %d check(s) FAILED ====\n", TAG, fails);
    fflush(stdout);
    return fails ? 1 : 0;
}
