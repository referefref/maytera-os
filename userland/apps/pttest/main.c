// pttest - #430 end-to-end verification of the newly-wired signal + pthread
// syscalls. Runs as a background service (see /CONFIG/SERVICES.CFG); all
// output goes to /dev/console which the kernel mirrors to the serial log, so
// grep the boot serial for the "PTTEST:" markers.
//
// Tests:
//   (a) pthreads: clone() a worker that increments a shared counter under a
//       futex-backed pthread_mutex, gettid() inside it, then pthread_join()
//       (join blocks in FUTEX_WAIT until the kernel clears the tid + wakes).
//   (b) signals: install a SIGUSR1 handler, raise() it, confirm it ran.
//   (c) SIGSEGV default action: fork a child that raises SIGSEGV; the DEFAULT
//       action terminates just that one child (exit 128+11=139) WITHOUT taking
//       down the kernel. (The fault-TRIGGERED SIGSEGV path needs the #PF
//       handler, #429; this proves the delivery + default-terminate half.)

#include "../../libc/maytera.h"     // sys_fork, sys_wait, sys_getpid, sys_sleep, vsnprintf
#include "../../libc/signal.h"      // signal(), raise(), SIGUSR1, SIGSEGV
#include "../../libc/pthread.h"     // pthread_create/join/mutex

#define ITERS 20000

// Emit a whole formatted line in ONE SYS_WRITE. The per-char putchar/printf
// path interleaves each byte with the kernel's syslog "[INF]" mirror, so a
// single write per line is what makes the "PTTEST:" markers contiguous and
// greppable in the serial log.
static void outf(const char *fmt, ...) {
    char buf[256];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    syscall3(SYS_WRITE, 1, (long)(uintptr_t)buf, (long)n);
}

static volatile int  g_sigusr1_got = 0;
static void usr1_handler(int s) { g_sigusr1_got = s; }

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile long   g_counter   = 0;
static volatile int    g_thread_ran = 0;
static volatile int    g_worker_tid = 0;

static void *worker(void *arg) {
    (void)arg;
    g_worker_tid = gettid();                 // gettid() from inside the thread
    for (int i = 0; i < ITERS; i++) {
        pthread_mutex_lock(&g_mtx);          // futex-backed under contention
        g_counter++;
        pthread_mutex_unlock(&g_mtx);
    }
    g_thread_ran = 1;
    return (void *)0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int pass = 1;

    outf("PTTEST: === #430 signals + pthreads verification ===\n");
    int main_tid = gettid();
    outf("PTTEST: main gettid=%d\n", main_tid);

    // -------- (a) pthreads + futex mutex + gettid + join --------
    pthread_t th;
    int rc = pthread_create(&th, 0, worker, 0);
    outf("PTTEST: pthread_create rc=%d handle=%p\n", rc, (void *)th);
    if (rc != 0) {
        outf("PTTEST: [FAIL] pthread_create returned %d\n", rc);
        pass = 0;
    } else {
        // Contend for the same mutex from the main thread.
        for (int i = 0; i < ITERS; i++) {
            pthread_mutex_lock(&g_mtx);
            g_counter++;
            pthread_mutex_unlock(&g_mtx);
        }
        // join blocks in FUTEX_WAIT until the worker exits and the kernel
        // (CLONE_CHILD_CLEARTID) clears the tid word + FUTEX_WAKEs us.
        pthread_join(th, 0);

        long expect = (long)ITERS * 2;
        outf("PTTEST: worker_tid=%d thread_ran=%d counter=%ld (expect %ld)\n",
               g_worker_tid, g_thread_ran, g_counter, expect);
        if (g_thread_ran == 1 && g_counter == expect &&
            g_worker_tid != 0 && g_worker_tid != main_tid) {
            outf("PTTEST: [PASS] clone+futex-mutex+gettid+join\n");
        } else {
            outf("PTTEST: [FAIL] pthread/futex test\n");
            pass = 0;
        }
    }

    // -------- (b) SIGUSR1 handler via raise() --------
    g_sigusr1_got = 0;
    signal(SIGUSR1, usr1_handler);
    raise(SIGUSR1);                          // delivered on return from the syscall
    if (g_sigusr1_got == SIGUSR1) {
        outf("PTTEST: [PASS] SIGUSR1 handler ran (got=%d)\n", g_sigusr1_got);
    } else {
        outf("PTTEST: [FAIL] SIGUSR1 handler did not run (got=%d)\n", g_sigusr1_got);
        pass = 0;
    }

    // -------- (c) SIGSEGV default action terminates ONE process --------
    int pid = sys_fork();
    if (pid == 0) {
        // Child: default action for SIGSEGV is terminate.
        raise(SIGSEGV);
        // Should never get here.
        sys_exit(0);
    } else if (pid > 0) {
        int status = -1;
        sys_wait(&status);
        // proc_wait returns the raw exit code; default-terminate uses 128+signo.
        outf("PTTEST: child exit status=%d (expect %d)\n", status, 128 + SIGSEGV);
        if (status == 128 + SIGSEGV) {
            outf("PTTEST: [PASS] SIGSEGV default-terminate (kernel survived)\n");
        } else {
            outf("PTTEST: [FAIL] SIGSEGV default-terminate status=%d\n", status);
            pass = 0;
        }
    } else {
        outf("PTTEST: [WARN] fork failed (%d); skipping SIGSEGV test\n", pid);
    }

    outf("PTTEST: RESULT %s\n", pass ? "ALL PASS" : "SOME FAIL");
    outf("PTTEST: (note: fault-triggered SIGSEGV depends on the #PF handler, #429)\n");
    return 0;
}
