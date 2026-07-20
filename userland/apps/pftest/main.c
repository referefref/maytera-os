// pftest - #429 page-fault / demand-paging / COW / SIGSEGV verification.
// Autostarted as a service (SERVICES.CFG); prints PASS/FAIL to the serial
// console. Exercises: (a) demand-paged mmap, (b) copy-on-write fork,
// (c1) a recoverable SIGSEGV caught by a user handler, (c2) an uncaught wild
// write that default-terminates only that child while the kernel keeps running.
#include "../../libc/stdio.h"
#include "../../libc/syscall.h"
#include "../../libc/signal.h"

static void segv_handler(int sig) {
    printf("PFTEST: [segv-child] SIGSEGV handler ran (sig=%d) -> exit(42)\n", sig);
    sys_exit(42);
}

// COW test datum: a writable global lives in a .data page that fork shares COW.
static volatile int g_shared = 1000;

int main(void) {
    printf("PFTEST: ===== #429 page-fault verification start (pid=%d) =====\n", sys_getpid());

    // (a) DEMAND PAGING: mmap a 2-page region, touch it (each page faults in),
    // read the values back.
    unsigned char *m = (unsigned char *)sys_mmap(0, 8192, 0x1 | 0x2, 0); // R|W
    if (m == (void *)-1 || m == (void *)0) {
        printf("PFTEST: (a) mmap FAILED\n");
    } else {
        printf("PFTEST: (a) mmap -> %p (uncommitted; pages fault in on write)\n", m);
        m[0]    = 0xAB;    // demand-zero fault, page 0
        m[4096] = 0xCD;    // demand-zero fault, page 1
        m[8191] = 0xEF;
        if (m[0] == 0xAB && m[4096] == 0xCD && m[8191] == 0xEF)
            printf("PFTEST: (a) DEMAND-PAGING PASS (readback 0x%x 0x%x 0x%x)\n",
                   m[0], m[4096], m[8191]);
        else
            printf("PFTEST: (a) DEMAND-PAGING FAIL (readback wrong)\n");
    }

    // (b) COPY-ON-WRITE fork. Child writes g_shared; parent must NOT see it.
    g_shared = 1000;
    int pid = sys_fork();
    if (pid == 0) {
        int before = g_shared;
        g_shared = 2222;                       // triggers COW copy in the child
        printf("PFTEST: (b) [child] g_shared before=%d after=%d\n", before, g_shared);
        sys_exit(0);
    } else if (pid > 0) {
        int st = 0;
        sys_waitpid(pid, &st, 0);
        int observed = g_shared;               // parent's own copy, unaffected
        printf("PFTEST: (b) [parent] g_shared observed=%d (expect 1000)\n", observed);
        g_shared = 3333;
        if (observed == 1000)
            printf("PFTEST: (b) COW-FORK PASS (parent+child independent)\n");
        else
            printf("PFTEST: (b) COW-FORK FAIL (child write leaked to parent)\n");
    } else {
        printf("PFTEST: (b) fork FAILED\n");
    }

    // (c1) RECOVERABLE SIGSEGV: child installs a handler, faults, handler runs.
    int c1 = sys_fork();
    if (c1 == 0) {
        struct sigaction sa;
        char *z = (char *)&sa;
        for (unsigned i = 0; i < sizeof(sa); i++) z[i] = 0;
        sa.sa_handler = segv_handler;
        sigaction(SIGSEGV, &sa, 0);
        printf("PFTEST: (c1) [segv-child] wild write to 0xdeadb000 (handler installed)\n");
        volatile int *wild = (volatile int *)0xdeadb000UL;
        *wild = 1;                              // #PF -> SIGSEGV -> segv_handler
        printf("PFTEST: (c1) [segv-child] UNREACHABLE\n");
        sys_exit(1);
    } else if (c1 > 0) {
        int st = 0;
        sys_waitpid(c1, &st, 0);
        printf("PFTEST: (c1) [parent] segv-child status=0x%x (42 => handler-caught SIGSEGV)\n", st & 0xff);
    }

    // (c2) DEFAULT-TERMINATE: child faults with NO handler -> killed; kernel lives.
    int c2 = sys_fork();
    if (c2 == 0) {
        printf("PFTEST: (c2) [kill-child] wild write to 0xdeadc000 (NO handler)\n");
        volatile int *wild = (volatile int *)0xdeadc000UL;
        *wild = 1;                              // #PF -> SIGSEGV default -> terminate
        printf("PFTEST: (c2) [kill-child] UNREACHABLE\n");
        sys_exit(1);
    } else if (c2 > 0) {
        int st = 0;
        sys_waitpid(c2, &st, 0);
        printf("PFTEST: (c2) [parent] kill-child terminated (status=0x%x); PARENT SURVIVED\n", st);
    }

    // Kernel + parent heartbeat after all the faults.
    for (int i = 0; i < 3; i++) {
        printf("PFTEST: heartbeat %d - kernel and parent still alive\n", i);
        sys_sleep(300);
    }
    printf("PFTEST: ===== #429 ALL DONE =====\n");
    sys_exit(0);
    return 0;
}
