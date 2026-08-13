// mmtest - #522 mmap / mprotect / munmap verification (FOUNDATION 2/7).
//
// Autostarted as a service (/CONFIG/SERVICES.CFG on the test image only);
// prints MMTEST: lines to the serial console. This app is the EVIDENCE for
// #522 and for the C-reference defects filed as #628/#629: it is meant to be
// run against BOTH the unfixed and the fixed kernel so the before/after is a
// measurement rather than an assertion.
//
// Fault probes run IN-PROCESS via a SIGSEGV handler + longjmp, deliberately
// NOT via fork-per-probe. A forked probe is unsound for permission testing in
// this kernel: fork marks every shared page COW in the PTE, and mm_fault()
// honours the PTE COW bit WITHOUT consulting the VMA, so a write to a
// read-only mapping in a freshly forked child is silently upgraded to
// writable by the COW path instead of faulting. Probing in-process removes
// that confound. SA_NODEFER keeps SIGSEGV unblocked across the longjmp (the
// handler never returns through the trampoline that would restore the mask),
// so repeated probes work.
#include "../../libc/stdio.h"
#include "../../libc/syscall.h"
#include "../../libc/signal.h"
#include "../../libc/setjmp.h"
#include "../../libc/pthread.h"

#define PGSZ            4096UL

// mmap prot bits (kernel proc/syscall.c reads these)
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

// mmap flags
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20

#define MAP_FAILED      ((void *)-1)

// SYS_MPROTECT: added by #522. Declared here so this app also builds and runs
// against a kernel that does not have it yet (the call then returns -1 and the
// subtest reports NOT-IMPLEMENTED rather than failing to build).
#ifndef SYS_MPROTECT
#define SYS_MPROTECT    23
#endif

static int sys_mprotect(void *addr, unsigned long len, int prot) {
    return (int)syscall3(SYS_MPROTECT, (long)addr, (long)len, (long)prot);
}

// ---------------------------------------------------------------------------
// In-process fault probing
// ---------------------------------------------------------------------------

static jmp_buf g_jb;

static void segv_handler(int sig) {
    (void)sig;
    longjmp(g_jb, 1);
}

static void probe_init(void) {
    struct sigaction sa;
    char *z = (char *)&sa;
    for (unsigned i = 0; i < sizeof(sa); i++) z[i] = 0;
    sa.sa_handler = segv_handler;
    sa.sa_flags   = SA_NODEFER;   // keep SIGSEGV deliverable after the longjmp
    sigaction(SIGSEGV, &sa, 0);
}

// MEASURED THE HARD WAY (first baseline run, build 990): relying on SA_NODEFER
// alone is NOT enough. Delivering SIGSEGV blocks it for the duration of the
// handler, and longjmp()ing out of the handler skips the trampoline that would
// have restored the mask, so the SECOND fault in a run was never delivered and
// every later probe silently reported "no fault". That turned three real
// failures into false passes. Unblock explicitly after every longjmp, and
// prove the probe still works with test (0) before trusting any other result.
static void unblock_segv(void) {
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGSEGV);
    sigprocmask(SIG_UNBLOCK, &s, 0);
}

// Return 1 if the access faulted, 0 if it completed.
static int probe_read(volatile unsigned int *p, unsigned int *out) {
    if (setjmp(g_jb) == 0) {
        unsigned int v = *p;
        if (out) *out = v;
        return 0;
    }
    unblock_segv();
    return 1;
}

static int probe_write(volatile unsigned int *p, unsigned int v) {
    if (setjmp(g_jb) == 0) {
        *p = v;
        return 0;
    }
    unblock_segv();
    return 1;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void fill(volatile unsigned int *base, unsigned long bytes, unsigned int seed) {
    unsigned long n = bytes / 4;
    for (unsigned long i = 0; i < n; i++) base[i] = seed + (unsigned int)i;
}

// Count mismatching words; report the first bad index and what was seen.
static unsigned long check(volatile unsigned int *base, unsigned long bytes,
                           unsigned int seed, long *first_bad, unsigned int *seen) {
    unsigned long n = bytes / 4, bad = 0;
    *first_bad = -1;
    *seen = 0;
    for (unsigned long i = 0; i < n; i++) {
        unsigned int want = seed + (unsigned int)i;
        if (base[i] != want) {
            if (bad == 0) { *first_bad = (long)i; *seen = base[i]; }
            bad++;
        }
    }
    return bad;
}

#define PASS(t)  printf("MMTEST: %-28s PASS\n", t)
#define FAIL(t)  printf("MMTEST: %-28s ***FAIL***\n", t)
#define SKIP(t,r) printf("MMTEST: %-28s SKIP (%s)\n", t, r)

static int g_pass = 0, g_fail = 0;
static void verdict(const char *t, int ok) {
    if (ok) { PASS(t); g_pass++; } else { FAIL(t); g_fail++; }
}

// ---------------------------------------------------------------------------
// (D) #628: fork + munmap frees COW-SHARED physical pages with a raw
// pmm_free_page(), so the surviving process keeps mapping memory the PMM has
// already handed back out. Deterministic here because pmm_alloc_page() is
// first-fit from the lowest free page: pages freed below the allocation
// frontier are returned by the very next allocation, in order.
//
// Parent maps + stamps a region, forks, and does NOT touch the region again
// (touching it would COW-copy it and mask the bug). The child unmaps that same
// COW-shared region and then immediately maps a fresh one and stamps it with a
// different pattern. If the child's new pages are the parent's old physical
// pages, the parent reads back the CHILD's pattern.
// ---------------------------------------------------------------------------
static void test_cow_munmap_uaf(void) {
    const char *T = "(D) #628 fork+munmap UAF";
    const unsigned long LEN = 4 * PGSZ;
    const unsigned int PAR_SEED = 0xA5A50000u;
    const unsigned int CHI_SEED = 0x5A5A0000u;

    volatile unsigned int *p = (volatile unsigned int *)sys_mmap(0, LEN, PROT_READ | PROT_WRITE, 0);
    if (p == (volatile unsigned int *)MAP_FAILED || p == 0) { SKIP(T, "mmap failed"); return; }
    fill(p, LEN, PAR_SEED);

    long fb; unsigned int seen;
    if (check(p, LEN, PAR_SEED, &fb, &seen) != 0) { SKIP(T, "pre-fork readback bad"); return; }
    printf("MMTEST: (D) parent region %p..%p stamped\n", (void *)p, (void *)((char *)p + LEN));

    int pid = sys_fork();
    if (pid == 0) {
        // Child. Unmap the region the parent still maps COW-shared.
        int r = sys_munmap((void *)p, LEN);
        // Now take fresh anonymous pages and stamp them. With first-fit PMM
        // these are the pages just freed above.
        volatile unsigned int *q = (volatile unsigned int *)sys_mmap(0, LEN, PROT_READ | PROT_WRITE, 0);
        if (q != (volatile unsigned int *)MAP_FAILED && q != 0) fill(q, LEN, CHI_SEED);
        printf("MMTEST: (D) [child] munmap rc=%d, re-mmap %p stamped 0x%x\n",
               r, (void *)q, CHI_SEED);
        sys_exit(0);
    } else if (pid < 0) {
        SKIP(T, "fork failed");
        return;
    }

    int st = 0;
    sys_waitpid(pid, &st, 0);

    // Parent's FIRST touch of the region since the fork.
    unsigned long bad = check(p, LEN, PAR_SEED, &fb, &seen);
    printf("MMTEST: (D) [parent] corrupted_words=%lu/%lu first_bad_idx=%ld observed=0x%x\n",
           bad, LEN / 4, fb, seen);
    if (bad != 0 && (seen & 0xFFFF0000u) == CHI_SEED) {
        printf("MMTEST: (D) parent is reading the CHILD's pattern -> cross-process "
               "use-after-free CONFIRMED\n");
    }
    verdict(T, bad == 0);
}

// ---------------------------------------------------------------------------
// (0) PROBE SELF-TEST. Every "did not fault" result below is only meaningful if
// the probe can still detect a fault at that point in the run. Fault three
// times at a wild address and require all three to be caught. If this fails,
// treat every later no-fault result as UNKNOWN, not as a pass.
// ---------------------------------------------------------------------------
static int test_probe_harness(void) {
    const char *T = "(0) probe self-test";
    volatile unsigned int *wild = (volatile unsigned int *)0xDEADB000UL;
    int a = probe_read(wild, 0);
    int b = probe_read(wild, 0);
    int c = probe_write(wild, 1);
    printf("MMTEST: (0) wild-probe faults: read=%d read=%d write=%d (want 1 1 1)\n", a, b, c);
    int ok = a && b && c;
    if (!ok) printf("MMTEST: (0) WARNING - probe is DEAD; later 'no fault' results are UNRELIABLE\n");
    verdict(T, ok);
    return ok;
}

// ---------------------------------------------------------------------------
// (1) Anonymous map: allocate, write, read back.
// ---------------------------------------------------------------------------
static void test_anon(void) {
    const char *T = "(1) anonymous mmap";
    const unsigned long LEN = 4 * PGSZ;
    volatile unsigned int *p = (volatile unsigned int *)sys_mmap(0, LEN, PROT_READ | PROT_WRITE, 0);
    if (p == (volatile unsigned int *)MAP_FAILED || p == 0) { verdict(T, 0); return; }
    printf("MMTEST: (1) mmap(%lu) -> %p\n", LEN, (void *)p);
    fill(p, LEN, 0x11110000u);
    long fb; unsigned int seen;
    int ok = (check(p, LEN, 0x11110000u, &fb, &seen) == 0);
    sys_munmap((void *)p, LEN);
    verdict(T, ok);
}

// ---------------------------------------------------------------------------
// (2) Partial unmap in the MIDDLE of a VMA must SPLIT it, not free the whole
// thing: pages 0 and 3 must survive with their data, pages 1 and 2 must fault.
// ---------------------------------------------------------------------------
static void test_split_unmap(void) {
    const char *T = "(2) partial unmap splits VMA";
    const unsigned long LEN = 4 * PGSZ;
    char *p = (char *)sys_mmap(0, LEN, PROT_READ | PROT_WRITE, 0);
    if (p == (char *)MAP_FAILED || p == 0) { verdict(T, 0); return; }
    fill((volatile unsigned int *)p, LEN, 0x22220000u);

    int rc = sys_munmap(p + PGSZ, 2 * PGSZ);      // punch out pages 1 and 2

    unsigned int v0 = 0, v3 = 0;
    int f0 = probe_read((volatile unsigned int *)(p + 0 * PGSZ), &v0);
    int f1 = probe_read((volatile unsigned int *)(p + 1 * PGSZ), 0);
    int f2 = probe_read((volatile unsigned int *)(p + 2 * PGSZ), 0);
    int f3 = probe_read((volatile unsigned int *)(p + 3 * PGSZ), &v3);

    unsigned int want0 = 0x22220000u;
    unsigned int want3 = 0x22220000u + (unsigned int)(3 * PGSZ / 4);
    printf("MMTEST: (2) munmap rc=%d  fault[p0..p3]=%d%d%d%d  p0=0x%x(want 0x%x) p3=0x%x(want 0x%x)\n",
           rc, f0, f1, f2, f3, v0, want0, v3, want3);

    int ok = (rc == 0) && !f0 && f1 && f2 && !f3 && v0 == want0 && v3 == want3;
    sys_munmap(p, PGSZ);
    sys_munmap(p + 3 * PGSZ, PGSZ);
    verdict(T, ok);
}

// ---------------------------------------------------------------------------
// (3) mprotect on a SUB-RANGE must split the VMA and change only that range.
// ---------------------------------------------------------------------------
static void test_mprotect_subrange(void) {
    const char *T = "(3) mprotect sub-range";
    const unsigned long LEN = 4 * PGSZ;
    char *p = (char *)sys_mmap(0, LEN, PROT_READ | PROT_WRITE, 0);
    if (p == (char *)MAP_FAILED || p == 0) { verdict(T, 0); return; }
    fill((volatile unsigned int *)p, LEN, 0x33330000u);

    int rc = sys_mprotect(p + PGSZ, 2 * PGSZ, PROT_READ);   // pages 1,2 -> RO
    if (rc != 0) { SKIP(T, "mprotect unavailable"); sys_munmap(p, LEN); return; }

    // Reads must still work everywhere; writes must fault ONLY on pages 1,2.
    unsigned int v1 = 0;
    int rf1 = probe_read((volatile unsigned int *)(p + PGSZ), &v1);
    int w0 = probe_write((volatile unsigned int *)(p + 0 * PGSZ), 0xDEAD0000u);
    int w1 = probe_write((volatile unsigned int *)(p + 1 * PGSZ), 0xDEAD0001u);
    int w2 = probe_write((volatile unsigned int *)(p + 2 * PGSZ), 0xDEAD0002u);
    int w3 = probe_write((volatile unsigned int *)(p + 3 * PGSZ), 0xDEAD0003u);
    printf("MMTEST: (3) mprotect rc=%d  read_p1_fault=%d  write_fault[p0..p3]=%d%d%d%d  p1=0x%x\n",
           rc, rf1, w0, w1, w2, w3, v1);

    int ok = !rf1 && !w0 && w1 && w2 && !w3;

    // Restore RW on the sub-range and prove the write now lands.
    int rc2 = sys_mprotect(p + PGSZ, 2 * PGSZ, PROT_READ | PROT_WRITE);
    int w1b = probe_write((volatile unsigned int *)(p + PGSZ), 0xBEEF0001u);
    unsigned int back = 0;
    int rb = probe_read((volatile unsigned int *)(p + PGSZ), &back);
    printf("MMTEST: (3) restore rc=%d write_fault=%d readback=0x%x (want 0xbeef0001)\n",
           rc2, w1b, back);
    ok = ok && rc2 == 0 && !w1b && !rb && back == 0xBEEF0001u;

    sys_munmap(p, LEN);
    verdict(T, ok);
}

// ---------------------------------------------------------------------------
// (4) munmap of a range that was never mapped must SUCCEED (POSIX), and
// mprotect of an unmapped range must FAIL.
// ---------------------------------------------------------------------------
static void test_unmapped_range(void) {
    const char *T = "(4) unmapped-range semantics";
    // Take a region, then free it, so we have an address we know is legal-but-unmapped.
    char *p = (char *)sys_mmap(0, 4 * PGSZ, PROT_READ | PROT_WRITE, 0);
    if (p == (char *)MAP_FAILED || p == 0) { verdict(T, 0); return; }
    sys_munmap(p, 4 * PGSZ);

    int rc_un = sys_munmap(p, 4 * PGSZ);              // second unmap: must be 0
    int rc_mp = sys_mprotect(p, 4 * PGSZ, PROT_READ); // unmapped: must be != 0
    printf("MMTEST: (4) munmap-unmapped rc=%d (want 0)   mprotect-unmapped rc=%d (want !=0)\n",
           rc_un, rc_mp);
    verdict(T, rc_un == 0 && rc_mp != 0);
}

// ---------------------------------------------------------------------------
// (5) MAP_FIXED over an existing mapping must replace exactly that sub-range:
// the covered pages come back demand-zero, the surrounding pages keep data.
// ---------------------------------------------------------------------------
static void test_map_fixed(void) {
    const char *T = "(5) MAP_FIXED over mapping";
    const unsigned long LEN = 4 * PGSZ;
    char *p = (char *)sys_mmap(0, LEN, PROT_READ | PROT_WRITE, 0);
    if (p == (char *)MAP_FAILED || p == 0) { verdict(T, 0); return; }
    fill((volatile unsigned int *)p, LEN, 0x44440000u);

    void *r = sys_mmap(p + PGSZ, 2 * PGSZ, PROT_READ | PROT_WRITE,
                       MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE);
    if (r != (void *)(p + PGSZ)) {
        printf("MMTEST: (5) MAP_FIXED returned %p, wanted %p\n", r, (void *)(p + PGSZ));
        sys_munmap(p, LEN);
        verdict(T, 0);
        return;
    }

    unsigned int z1 = 0xFFFFFFFFu, z2 = 0xFFFFFFFFu, k0 = 0, k3 = 0;
    int f = 0;
    f |= probe_read((volatile unsigned int *)(p + 1 * PGSZ), &z1);
    f |= probe_read((volatile unsigned int *)(p + 2 * PGSZ), &z2);
    f |= probe_read((volatile unsigned int *)(p + 0 * PGSZ), &k0);
    f |= probe_read((volatile unsigned int *)(p + 3 * PGSZ), &k3);

    unsigned int want0 = 0x44440000u;
    unsigned int want3 = 0x44440000u + (unsigned int)(3 * PGSZ / 4);
    printf("MMTEST: (5) fixed=%p fault=%d  zeroed[p1,p2]=0x%x,0x%x  kept[p0,p3]=0x%x,0x%x "
           "(want 0x%x,0x%x)\n", r, f, z1, z2, k0, k3, want0, want3);

    int ok = !f && z1 == 0 && z2 == 0 && k0 == want0 && k3 == want3;
    sys_munmap(p, LEN);
    verdict(T, ok);
}

// ---------------------------------------------------------------------------
// (6) MAP_SHARED must be REJECTED honestly rather than silently downgraded to
// private semantics (the #523 seam).
// ---------------------------------------------------------------------------
static void test_shared_rejected(void) {
    const char *T = "(6) MAP_SHARED rejected";
    void *r = sys_mmap(0, PGSZ, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS);
    printf("MMTEST: (6) mmap(MAP_SHARED) -> %p (want MAP_FAILED)\n", r);
    if (r != MAP_FAILED && r != 0) sys_munmap(r, PGSZ);
    verdict(T, r == MAP_FAILED);
}

// ---------------------------------------------------------------------------
// (7) CONCURRENCY. CLONE_VM threads share the IDENTICAL mm_struct_t, so this
// hammers one VMA list from several CPUs at once: each worker repeatedly maps a
// region, stamps it, punches its MIDDLE (forcing a real VMA split), verifies
// its own data survived, then unmaps the remains. Splitting is what makes the
// unlocked list dangerous: a mutator can kfree() a vma_t while a sibling is
// walking it in mm_fault().
//
// This subtest is also what caught the placement bug: the mmap cursor used to
// live in process_t, which is PER-THREAD, so concurrent workers were handed
// overlapping addresses out of one address space.
// ---------------------------------------------------------------------------
#define NWORKERS 3
#define NITERS   40

static volatile int g_worker_fail  = 0;
static volatile int g_worker_maps  = 0;
static volatile int g_worker_bad   = 0;

static void *stress_worker(void *arg) {
    unsigned long id = (unsigned long)arg;
    for (int it = 0; it < NITERS; it++) {
        unsigned long LEN = 8 * PGSZ;
        char *p = (char *)sys_mmap(0, LEN, PROT_READ | PROT_WRITE, 0);
        if (p == (char *)MAP_FAILED || p == 0) { g_worker_fail++; continue; }
        g_worker_maps++;
        if (it == 0 || it == 1)
            printf("MMTEST: (7) worker %lu iter %d got %p\n", id, it, (void *)p);

        unsigned int seed = 0x70000000u + ((unsigned)id << 20) + (unsigned)it;
        fill((volatile unsigned int *)p, 2 * PGSZ, seed);

        // Punch the middle: a genuine split while siblings fault and allocate.
        sys_munmap(p + 3 * PGSZ, 2 * PGSZ);

        long fb; unsigned int seen;
        if (check((volatile unsigned int *)p, 2 * PGSZ, seed, &fb, &seen) != 0) g_worker_bad++;

        sys_munmap(p, 3 * PGSZ);
        sys_munmap(p + 5 * PGSZ, 3 * PGSZ);
    }
    return 0;
}

static void test_concurrency(void) {
    const char *T = "(7) threaded VMA stress";
    pthread_t th[NWORKERS];
    int made = 0;
    for (unsigned long i = 0; i < NWORKERS; i++) {
        if (pthread_create(&th[i], 0, stress_worker, (void *)i) == 0) made++;
        else break;
    }
    if (made == 0) { SKIP(T, "pthread_create failed"); return; }
    for (int i = 0; i < made; i++) pthread_join(th[i], 0);

    printf("MMTEST: (7) workers=%d iters=%d maps_ok=%d map_fail=%d data_corrupt=%d\n",
           made, NITERS, g_worker_maps, g_worker_fail, g_worker_bad);
    verdict(T, g_worker_fail == 0 && g_worker_bad == 0 &&
               g_worker_maps == made * NITERS);
}

int main(void) {
    printf("MMTEST: ===== #522 mmap/mprotect/munmap verification (pid=%d) =====\n",
           sys_getpid());
    probe_init();

    test_probe_harness();
    test_anon();
    test_split_unmap();
    test_mprotect_subrange();
    test_unmapped_range();
    test_map_fixed();
    test_shared_rejected();
    test_cow_munmap_uaf();
    test_concurrency();

    printf("MMTEST: ===== DONE pass=%d fail=%d =====\n", g_pass, g_fail);
    sys_exit(g_fail == 0 ? 0 : 1);
    return 0;
}
