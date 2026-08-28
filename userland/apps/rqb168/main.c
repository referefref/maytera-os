// rqb168 - #168 Job 1: DOES RING-3 THROUGHPUT SCALE WITH CORES?
//
// WHY THIS EXISTS. The #143 part-2 re-measure recommended narrowing the BKL
// before re-enabling AP user scheduling (#67), and named the caveat that could
// overturn its own recommendation: its userland arm was a NULL RESULT (2% busy,
// 263,231 vs 264,643 syscalls), so it never loaded the one regime where AP
// scheduling could pay WITHOUT touching the BKL at all - a CPU-bound,
// multi-process Ring-3 load. Ring-3 execution does not hold the BKL, so N
// compute-bound user processes on N cores is the case with nothing for the
// giant lock to serialise.
//
// The in-kernel comparison collapsed: 567,437 -> 85,981 syscalls gate-on, and
// 1.00 -> 0.83 cores of useful work. If Ring-3 shows the OPPOSITE, #67 is worth
// doing before any BKL work.
//
// WHAT IT MEASURES, AND WHY NOT THE OBVIOUS THING. Not CPU busy%: a core
// spinning on the BKL is 100% busy, so [SCHEDCORE] cpuN=% cannot tell work from
// waiting, and that is the exact trap this subsystem keeps setting. Not
// syscalls/sec either: the load under test deliberately makes almost none. It
// counts COMPLETED WORK UNITS, where a unit is a fixed, identical, register-
// resident integer mix. Units are therefore comparable across arms by
// construction, and aggregate units/sec across all workers is throughput.
//
// The mix is register-resident on purpose. Keeping it out of memory maximises
// the chance of observing a clean 4x, so a failure to scale cannot be blamed on
// memory bandwidth. It is integer-only because the kernel is built soft-float;
// userland is hardware SSE2 and could use doubles, but integers keep the unit
// bit-exact across arms.
//
// OUTPUT GOES TO fd 2, one write() per line, same shape as lockprobe/nrprobe.
// An autorun-spawned process's fd 1 does NOT reach the serial console (recorded
// in lockprobe/main.c); fd 2 does.
//
// MODES
//   (no args)   parent: read /RQB168.CFG, spawn N workers, wait for them all
//   w <slot>    worker: compute for the configured duration, report periodically
//
// The child is THIS SAME BINARY, so nothing about the measurement depends on a
// second program's argument parsing or buffering.

#include "../../libc/maytera.h"   // types, syscall (uptime_ms/sys_sleep), stdlib (open/atoi)
#include "../../libc/unistd.h"     // read/write/close/getpid
#include "../../libc/fcntl.h"      // O_RDONLY
#include "../../libc/spawn.h"
#include "../../libc/sys/wait.h"

#define SELF_PATH "/APPS/rqb168"
#define CFG_PATH  "/RQB168.CFG"

// One unit = UNIT_ITERS rounds of the mix. Sized so a unit is a few
// milliseconds on this class of vCPU: small enough that the run ends promptly,
// large enough that the clock check below is not itself a syscall load.
#define UNIT_ITERS   1000000u
// Read the clock only every CLOCK_EVERY units. At ~4ms/unit that is one
// uptime_ms() syscall roughly every 32ms per worker, about 31/sec - three
// orders of magnitude below the loads #143 measured, so the Ring-3 arm stays
// Ring-3.
#define CLOCK_EVERY  8u

#define MAX_WORKERS  16

// ---------------------------------------------------------------------------
// fd-2 line output (same shape as lockprobe, deliberately, so one grep reads
// both)
// ---------------------------------------------------------------------------
static char g_line[256];
static int  g_len;

static void put_s(const char *s) {
    while (*s && g_len < (int)sizeof(g_line) - 1) g_line[g_len++] = *s++;
}
static void put_u(unsigned long v) {
    char b[24]; int i = 23;
    b[i--] = 0;
    if (v == 0) b[i--] = '0';
    while (v > 0) { b[i--] = (char)('0' + (v % 10)); v /= 10; }
    put_s(&b[i + 1]);
}
static void flush_line(void) {
    if (g_len < (int)sizeof(g_line) - 1) g_line[g_len++] = '\n';
    write(2, g_line, (unsigned long)g_len);
    g_len = 0;
}

// ---------------------------------------------------------------------------
// The work unit. Marked noinline and fed a global sink so no arm can have it
// optimised away or hoisted: an arm that skipped the work would look infinitely
// fast, which is the one failure mode that would look like the result I am
// hoping for.
// ---------------------------------------------------------------------------
unsigned long g_sink;

static __attribute__((noinline)) unsigned long work_unit(unsigned long s) {
    for (unsigned int i = 0; i < UNIT_ITERS; i++) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        s += 0x9E3779B97F4A7C15UL;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Config: "<nworkers> <run_ms> <settle_ms>", whitespace separated, on the ext2
// root. Absent or unreadable falls back to the defaults, and SAYS SO on fd 2 -
// a test that silently used different parameters than the ones I think I set is
// worse than one that failed.
// ---------------------------------------------------------------------------
static int g_nworkers = 4;
static unsigned long g_run_ms = 90000;
static unsigned long g_settle_ms = 20000;
// #168 Job 2 sizing: syscalls to make per work unit. 0 = the pure-compute load
// that answered Job 1. Non-zero turns the SAME binary into a syscall-bound
// Ring-3 load, which is the case that MUST take the BKL on every kernel entry
// and is therefore the case a BKL held by an in-kernel thread can throttle.
// Same binary and same work unit on purpose: a separate benchmark would make
// the compute and syscall arms incomparable.
static unsigned long g_sysper = 0;

static void read_cfg(void) {
    int fd = open(CFG_PATH, O_RDONLY);
    if (fd < 0) {
        g_len = 0; put_s("[RQB] cfg ABSENT, using defaults"); flush_line();
        return;
    }
    char buf[64];
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        g_len = 0; put_s("[RQB] cfg EMPTY, using defaults"); flush_line();
        return;
    }
    buf[n] = 0;
    unsigned long v[4] = { 0, 0, 0, 0 };
    int f = 0, seen = 0;
    for (long i = 0; i <= n && f < 4; i++) {
        char c = buf[i];
        if (c >= '0' && c <= '9') { v[f] = v[f] * 10 + (unsigned long)(c - '0'); seen = 1; }
        else if (seen) { f++; seen = 0; }
    }
    if (v[0] >= 1 && v[0] <= MAX_WORKERS) g_nworkers = (int)v[0];
    if (v[1] >= 1000) g_run_ms = v[1];
    if (v[2] > 0)     g_settle_ms = v[2];
    g_sysper = v[3];
}

static void banner(const char *role, int slot) {
    g_len = 0;
    put_s("[RQB] "); put_s(role);
    put_s(" slot="); put_u((unsigned long)slot);
    put_s(" n=");    put_u((unsigned long)g_nworkers);
    put_s(" run_ms="); put_u(g_run_ms);
    put_s(" settle_ms="); put_u(g_settle_ms);
    put_s(" sysper="); put_u(g_sysper);
    put_s(" pid=");  put_u((unsigned long)getpid());
    flush_line();
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------
static int worker(int slot) {
    banner("worker", slot);

    // Seed differs per slot so the arms cannot be accidentally comparing the
    // same instruction stream in a way that lets the branch predictor differ
    // between them; the WORK is identical either way (fixed iteration count).
    unsigned long s = 0x243F6A8885A308D3UL + (unsigned long)slot * 0x9E3779B9UL;

    unsigned long t0 = uptime_ms();
    unsigned long t_end = t0 + g_run_ms;
    unsigned long next_report = t0 + 10000;
    unsigned long units = 0;

    for (;;) {
        for (unsigned int k = 0; k < CLOCK_EVERY; k++) {
            s = work_unit(s);
            // The syscall burst is charged to the SAME unit as the compute, so
            // "units" stays the unit of comparison across both loads.
            for (unsigned long q = 0; q < g_sysper; q++) s += uptime_ms();
            units++;
        }
        unsigned long now = uptime_ms();
        if (now >= next_report) {
            g_len = 0;
            put_s("[RQB] t slot="); put_u((unsigned long)slot);
            put_s(" ms=");    put_u(now - t0);
            put_s(" units="); put_u(units);
            flush_line();
            next_report += 10000;
        }
        if (now >= t_end) {
            g_sink ^= s;
            g_len = 0;
            put_s("[RQB] FINAL slot="); put_u((unsigned long)slot);
            put_s(" ms=");    put_u(now - t0);
            put_s(" units="); put_u(units);
            put_s(" sink=");  put_u(g_sink & 0xFFFFFFFFUL);
            flush_line();
            return 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Parent
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    read_cfg();

    if (argc >= 3 && argv[1] && argv[1][0] == 'w')
        return worker(atoi(argv[2]));

    banner("parent", -1);

    // Let the desktop, compositor and any first-boot work finish before the
    // measurement window opens. Boot-time activity is not the load under test,
    // and an arm that measured a different slice of the boot would not be
    // comparable with the other arm.
    if (g_settle_ms) sys_sleep((unsigned int)g_settle_ms);

    g_len = 0; put_s("[RQB] SETTLED, spawning "); put_u((unsigned long)g_nworkers);
    flush_line();

    pid_t kids[MAX_WORKERS];
    int nk = 0;
    for (int i = 0; i < g_nworkers; i++) {
        char slot[8];
        int sl = 0;
        int v = i;
        if (v == 0) slot[sl++] = '0';
        else { char t[8]; int ti = 0; while (v) { t[ti++] = (char)('0' + v % 10); v /= 10; }
               while (ti) slot[sl++] = t[--ti]; }
        slot[sl] = 0;

        char *av[4];
        av[0] = (char *)SELF_PATH;
        av[1] = (char *)"w";
        av[2] = slot;
        av[3] = 0;

        pid_t p = 0;
        int rc = posix_spawn(&p, SELF_PATH, 0, 0, av, 0);
        g_len = 0;
        put_s("[RQB] spawn slot="); put_u((unsigned long)i);
        put_s(" rc="); put_u((unsigned long)rc);
        put_s(" pid="); put_u((unsigned long)p);
        flush_line();
        if (rc == 0 && p > 0) kids[nk++] = p;
    }

    g_len = 0; put_s("[RQB] SPAWNED "); put_u((unsigned long)nk);
    put_s(" of "); put_u((unsigned long)g_nworkers); flush_line();

    for (int i = 0; i < nk; i++) {
        int st = 0;
        waitpid(kids[i], &st, 0);
        g_len = 0; put_s("[RQB] reaped pid="); put_u((unsigned long)kids[i]); flush_line();
    }

    g_len = 0; put_s("[RQB] DONE"); flush_line();
    return 0;
}
