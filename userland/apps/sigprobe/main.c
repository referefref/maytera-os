// sigprobe - #SMPGLOBALS instrument for the shared-syscall-frame defect, and
// the SYSCALL COST yardstick the fix has to be measured against.
//
// THIS EXISTS TO BE ABLE TO FAIL. The kernel it runs on carries both arms of
// the fix (make SIGFRAMEDIFF=1): with /SIGFRAMEBUG.TXT on the ESP,
// sys_rt_sigreturn() goes back to reading the single global
// g_syscall_saved_frame; without it, it derives the frame from the calling
// task's own ring-0 stack. This app's only job is to generate rt_sigreturn
// traffic while other processes are making syscalls, which is the condition
// under which the global names somebody else's frame.
//
// THE DEFECT, in one sentence: g_syscall_saved_frame held the frame of the
// last syscall to finish ANYWHERE in the system, every task has its own ring-0
// stack so that address is per-task, and rt_sigreturn rewrote whatever it
// pointed at. That is not an SMP-only bug: one core plus a context switch
// between two tasks is enough.
//
// WHAT TO READ. The verdict is the kernel's [SIGFRAME] line on serial and in
// /BOOTLOG.TXT, not this app's output. This app makes the event happen and
// reports that it happened; the kernel counts how often the old global was
// pointing at the wrong task and names that task.
//
// MODES
//   (no argv)  signaller: install SIGUSR1, raise it N times, verify the
//              process comes back intact each time. Then measure syscall cost.
//   victim     spin on a cheap syscall verifying its result and a register
//              canary, so a frame rewritten underneath us is visible here and
//              not only in a counter. Run one of these alongside the
//              signaller.
//
// No em-dashes per repo style.

#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "signal.h"
#include "syscall.h"
#include "spawn.h"

#define TAG        "[SIGPROBE]"
#define SELF       "/APPS/SIGPROBE"
#define RAISES     2000       /* rt_sigreturn round trips */
#define BATCH      10000      /* syscalls per rdtsc bracket */
#define BRACKETS   20
#define SLOW_RAISES 300
#define BURN_RAISES 120      /* phase 3: handler ends in pure userland work */      /* phase 2: handler yields inside the window */

/* ---- output. sys_bootlog is the durable sink: it mirrors to serial AND to
   /BOOTLOG.TXT on the ext2 root, which is the only way to read a result off a
   machine with no serial port. One call is one line; control characters are
   replaced by the kernel, so do not embed newlines. ---------------------- */
static void emit(const char *s) { printf("%s\n", s); sys_bootlog(s); }

/* No trustworthy snprintf in this freestanding subset; format by hand. */
static char  g_line[240];
static int   g_len;
static void put(const char *s) { while (*s && g_len < 230) g_line[g_len++] = *s++; g_line[g_len] = 0; }
static void putu(unsigned long long v) {
    char t[24]; int n = 0;
    if (!v) { put("0"); return; }
    while (v && n < 23) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) { if (g_len < 230) g_line[g_len++] = t[n]; }
    g_line[g_len] = 0;
}
static void line_start(void) { g_len = 0; g_line[0] = 0; put(TAG); put(" "); }
static void line_emit(void) { emit(g_line); }

static inline unsigned long long rdtsc_serial(void) {
    unsigned lo, hi;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((unsigned long long)hi << 32) | lo;
}

/* ---- signaller ---------------------------------------------------------- */

static volatile sig_atomic_t g_hits = 0;
static volatile int g_handler_sleeps = 0;

/* WHY THE HANDLER CAN SLEEP, AND WHY THAT IS THE INTERESTING CASE.
   Phase 1 raises the signal in a tight loop. The window that matters runs from
   the raise syscall publishing the frame on its way out, to rt_sigreturn
   reading it, and in a tight loop that is roughly a hundred instructions with
   nothing else scheduled: MEASURED, 2000 tight raises produced legacy_wrong=0,
   so the old global happened to be right every time.

   Phase 2 makes the handler do what a real handler does - anything at all that
   yields the CPU. One sleep is enough for another process to enter and leave a
   syscall, and the old global then names THAT process's ring-0 frame, which is
   the frame rt_sigreturn was about to rewrite. This is not a contrived
   widening: a handler that logs, writes, or reads is in exactly this state. */
static volatile int g_handler_burns = 0;
static volatile unsigned long g_burn_sink = 1;

static void on_usr1(int s) {
    (void)s;
    g_hits++;
    if (g_handler_sleeps) sys_sleep(2);
    /* PHASE 3. The burn goes AFTER the sleep, and that ordering is the whole
       point. MEASURED in phase 2: sleeping in the handler produced
       legacy_wrong=0, because sys_sleep is ITSELF a syscall and its return
       re-published OUR frame into the global, wiping whatever ran during the
       sleep. The global is only observably wrong if this task is preempted
       after its LAST syscall and before rt_sigreturn, so the handler has to end
       in pure userland work. A fixed iteration count, not a clock, because
       reading a clock is a syscall and would defeat the same way. */
    if (g_handler_burns) {
        unsigned long x = g_burn_sink;
        for (unsigned long i = 0; i < 6000000UL; i++) x = x * 1103515245UL + 12345UL;
        g_burn_sink = x;
    }
}

/* WHAT THIS CAN AND CANNOT SEE FROM RING 3.
   A first draft tried to pin known values into rbx/r12/r13/r14 with inline asm
   and check them after the raise. It was WRONG: the asm listed r12/r13/r14 as
   both outputs and clobbers, which is not a legal combination, so the compiler
   never wrote the outputs at all and the check compared uninitialised locals.
   It would have reported a clean run on a corrupt kernel. Removed rather than
   patched, because a detector that cannot be trusted is worse than none.

   What IS checkable from here without asm: that the handler ran, that raise()
   returned success, and that this frame's own stack is intact afterwards. The
   register-level evidence lives in the kernel, where sys_rt_sigreturn can
   compare the frame it was about to rewrite against the caller's own ring-0
   stack and name the task it would have hit. That is the [SIGFRAME] line. */
static int raise_and_check(void)
{
    volatile unsigned long canary[8];
    for (int i = 0; i < 8; i++) canary[i] = 0xC0FFEE00UL + (unsigned long)i;
    int before = g_hits;
    if (raise(SIGUSR1) != 0) return -1;
    if (g_hits == before) return -2;            /* handler never ran */
    for (int i = 0; i < 8; i++)
        if (canary[i] != 0xC0FFEE00UL + (unsigned long)i) return -3;  /* stack clobbered */
    return 0;
}

static void run_signaller(void)
{
    int mypid = (int)getpid();
    line_start(); put("signaller pid="); putu((unsigned)mypid);
    put(" raises="); putu(RAISES); line_emit();

    if (signal(SIGUSR1, on_usr1) == SIG_ERR) { emit(TAG " FATAL: signal(SIGUSR1) failed"); return; }

    /* THE REPRODUCER'S SECOND HALF, and the part three earlier attempts were
       missing. The deleted global was written ONLY by a Ring-3 syscall
       RETURNING, so to see it hold somebody else's frame there has to BE
       somebody else completing syscalls. On an idle headless desktop there is
       not: the compositor is parked in a wait_event, which never reaches the
       return path, and the periodic services fire milliseconds apart. Widening
       the window in THIS process therefore did nothing, three times.
       So spawn a second copy of ourselves in a tight getpid() loop. */
    {
        char *a[3];
        pid_t vp = 0;
        a[0] = (char *)SELF; a[1] = (char *)"victim"; a[2] = NULL;
        extern char **environ;
        if (posix_spawn(&vp, SELF, NULL, NULL, a, environ) == 0) {
            line_start(); put("spawned victim pid="); putu((unsigned)vp);
            put(" (tight syscall loop, the other half of the reproducer)");
            line_emit();
            sys_sleep(300);   /* let it get going before phase 1 */
        } else {
            emit(TAG " WARNING: could not spawn the victim. Phases 1-3 will run "
                     "with NO competing syscall load and legacy_wrong is then "
                     "expected to be 0 for a reason that has nothing to do with "
                     "the fix.");
        }
    }

    /* SELFTEST FIRST: prove a signal is actually being delivered, before any
       number below is believed. A loop that silently delivers nothing would
       report a clean run and prove nothing at all. */
    g_hits = 0;
    if (raise(SIGUSR1) != 0 || g_hits == 0) {
        emit(TAG " SELFTEST FAILED: SIGUSR1 was not delivered. Everything below is meaningless.");
        return;
    }
    emit(TAG " selftest: SIGUSR1 delivered, handler ran. rt_sigreturn is live.");

    int bad = 0, firstbad = 0;

    /* Phase 1: tight loop, nothing else gets in between. */
    emit(TAG " PHASE 1 BEGIN (tight loop, handler does nothing)");
    g_hits = 0; g_handler_sleeps = 0;
    for (int i = 0; i < RAISES; i++) {
        int rc = raise_and_check();
        if (rc != 0) { if (!bad) firstbad = rc; bad++; }
    }
    line_start(); put("PHASE 1 END: handler_ran="); putu((unsigned)g_hits);
    put("/"); putu(RAISES); put(" corrupted="); putu((unsigned)bad);
    if (bad) { put(" first_rc="); putu((unsigned)(-firstbad)); }
    line_emit();

    /* Phase 2: the handler yields, so another process runs inside the window.
       Read the kernel [SIGFRAME] line before and after this phase. */
    emit(TAG " PHASE 2 BEGIN (handler sleeps 2ms, so another task runs in the window)");
    int bad2 = 0, firstbad2 = 0;
    g_hits = 0; g_handler_sleeps = 1;
    for (int i = 0; i < SLOW_RAISES; i++) {
        int rc = raise_and_check();
        if (rc != 0) { if (!bad2) firstbad2 = rc; bad2++; }
    }
    g_handler_sleeps = 0;
    line_start(); put("PHASE 2 END: handler_ran="); putu((unsigned)g_hits);
    put("/"); putu(SLOW_RAISES); put(" corrupted="); putu((unsigned)bad2);
    if (bad2) { put(" first_rc="); putu((unsigned)(-firstbad2)); }
    line_emit();
    bad += bad2;

    /* Phase 3: the handler ends in pure userland compute, so a timer
       preemption lands AFTER this task's last syscall and BEFORE
       rt_sigreturn. That is the window in which the old global names another
       task's ring-0 frame. Read the kernel [SIGFRAME] legacy_wrong counter
       across this phase. */
    emit(TAG " PHASE 3 BEGIN (handler sleeps then BURNS, so a preemption lands after our last syscall)");
    int bad3 = 0, firstbad3 = 0;
    g_hits = 0; g_handler_sleeps = 1; g_handler_burns = 1;
    for (int i = 0; i < BURN_RAISES; i++) {
        int rc = raise_and_check();
        if (rc != 0) { if (!bad3) firstbad3 = rc; bad3++; }
    }
    g_handler_sleeps = 0; g_handler_burns = 0;
    line_start(); put("PHASE 3 END: handler_ran="); putu((unsigned)g_hits);
    put("/"); putu(BURN_RAISES); put(" corrupted="); putu((unsigned)bad3);
    if (bad3) { put(" first_rc="); putu((unsigned)(-firstbad3)); }
    line_emit();
    bad += bad3;
    line_start();
    put(bad ? "VERDICT: CORRUPTION SEEN in this process"
            : "VERDICT: this process survived every rt_sigreturn");
    put(" (the kernel [SIGFRAME] line is the primary evidence)");
    line_emit();
}

/* ---- syscall cost ------------------------------------------------------- */

static void run_cost(void)
{
    unsigned long long best = ~0ULL, sum = 0, base_best = ~0ULL;
    volatile int sink = 0;

    /* Baseline: the same bracket with no syscall, so the rdtsc pair and the
       loop overhead are subtracted rather than reported as syscall cost. */
    for (int r = 0; r < BRACKETS; r++) {
        unsigned long long t0 = rdtsc_serial();
        for (int i = 0; i < BATCH; i++) sink += i;
        unsigned long long t1 = rdtsc_serial();
        unsigned long long per = (t1 - t0) / BATCH;
        if (per < base_best) base_best = per;
    }
    for (int r = 0; r < BRACKETS; r++) {
        unsigned long long t0 = rdtsc_serial();
        for (int i = 0; i < BATCH; i++) sink += sys_getpid();
        unsigned long long t1 = rdtsc_serial();
        unsigned long long per = (t1 - t0) / BATCH;
        if (per < best) best = per;
        sum += per;
    }
    line_start(); put("SYS_GETPID cycles: min="); putu(best);
    put(" mean="); putu(sum / BRACKETS);
    put(" baseline="); putu(base_best);
    put(" net_min="); putu(best > base_best ? best - base_best : 0);
    put(" (batch="); putu(BATCH); put(" x "); putu(BRACKETS); put(")");
    line_emit();
    emit(TAG " cost note: cycles are the primary data. The 2026-08-07 reference is "
             "338 raw / 334 net on kvm64; compare net_min, not ns.");
}

/* ---- victim ------------------------------------------------------------- */

static void run_victim(void)
{
    int mypid = (int)getpid();
    line_start(); put("victim pid="); putu((unsigned)mypid); line_emit();
    unsigned long long iters = 0, wrong = 0;
    unsigned long deadline = uptime_ms() + 180000;
    while (uptime_ms() < deadline) {
        for (int i = 0; i < 20000; i++) {
            if (sys_getpid() != mypid) wrong++;
            iters++;
        }
    }
    line_start(); put("victim done: syscalls="); putu(iters);
    put(" wrong_result="); putu(wrong); line_emit();
    line_start();
    put(wrong ? "VERDICT: victim's syscall returns were CORRUPTED"
              : "VERDICT: victim saw no corrupted syscall return");
    line_emit();
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1] && strcmp(argv[1], "victim") == 0) { run_victim(); return 0; }
    run_signaller();
    run_cost();
    return 0;
}
