// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// malloc_scale_test.c - #245 regression test: malloc() must not do work that
// grows with the number of LIVE blocks.
//
// WHY THIS EXISTS. The MayteraOS userland allocator was O(live-block-count) per
// malloc() for a workload that allocates, keeps most of it, and frees some of
// it: exactly what a browser does while building a DOM. find_free_block()
// scanned the address-ordered list of ALL blocks, so a page that reached 30,000
// live blocks paid 6,000 list steps per allocation, and a script building
// 40,000 DOM nodes pegged a core for over a minute. The size-binned explicit
// free list fixed it; this test is what stops it coming back.
//
// WHAT IT MEASURES, AND WHY NOT TIME. It asserts on __malloc_walk_steps(), the
// allocator's own cumulative count of bounded-walk steps, NOT on wall clock.
// Wall clock on a shared build host swings by several-fold between runs; the
// step counter is bit-identical across runs, so the gate is deterministic.
// The wall clock is printed anyway, because it is what a user feels, but
// nothing is decided by it.
//
// THE SHAPE OF THE ASSERTION. The same workload is run at N and at 4N in two
// separate processes. If per-allocation work is independent of the live block
// count, total steps grow at most ~4x. The pre-fix allocator grows them ~16x,
// so the threshold sits well clear of both: see run_malloc_scale.sh, whose RED
// arm runs this exact binary against the pre-fix allocator and requires it to
// FAIL.
//
// Build/run: ./run_malloc_scale.sh (in this directory).

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>

#define SYS_WRITE 13
#define SYS_MMAP  21

// The unit under test, symbol-prefixed by run_malloc_scale.sh's objcopy pass.
void     *mh_malloc(unsigned long);
void      mh_free(void *);
void     *mh_realloc(void *, unsigned long);
unsigned  mh___malloc_walk_report_count(void);
unsigned long mh_malloc_heap_highwater(void);
// Present only in the FIXED allocator. run_malloc_scale.sh links the RED arm
// with a fallback definition, so this file compiles against either.
uint64_t  mh___malloc_walk_steps(void);

// --- host-side backing for everything the allocator calls -----------------
long mh_syscall0(long n) { (void)n; return 0; }
long mh_syscall1(long n, long a) { (void)n; (void)a; return 0; }
long mh_syscall2(long n, long a, long b) { (void)n; (void)a; (void)b; return 0; }
long mh_syscall3(long n, long a, long b, long c) {
    if (n == SYS_WRITE) return write((int)a, (const void *)b, (size_t)c);
    return 0;
}
long mh_syscall4(long n, long a, long b, long c, long d) {
    (void)c; (void)d;
    if (n == SYS_MMAP) {
        void *p = mmap((void *)a, (size_t)b, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        return (p == MAP_FAILED) ? -1 : (long)p;
    }
    return 0;
}
void  *mh_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void  *mh_memset(void *d, int c, size_t n) { return memset(d, c, n); }
size_t mh_strlen(const char *s) { return strlen(s); }
char  *mh_strchr(const char *s, int c) { return strchr(s, c); }
int    mh_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
int   *mh___errno_location(void) { static int e; return &e; }
int    mh_pthread_mutex_lock(void *m) { (void)m; return 0; }
int    mh_pthread_mutex_unlock(void *m) { (void)m; return 0; }
int    mh_fflush(void *f) { (void)f; return 0; }
long   mh_time(void *t) { (void)t; return 0; }

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

// The DOM-build shape, reduced to the allocator: keep most allocations alive so
// the block list grows, while still freeing enough that a free block is always
// available (which is what made the old scan believe a fit might exist and walk
// the whole list to find out).
//
// Every retained block is also written and verified, so a "fast" run that has
// started handing out overlapping or already-live memory fails instead of
// looking like a win.
static int workload(long n, uint64_t *steps_out, double *secs_out) {
    void **keep = malloc((size_t) n * sizeof(void *));   // host malloc: scaffolding
    if (!keep) { printf("FAIL: host scaffolding allocation failed\n"); return 1; }
    double t0 = now();
    for (long i = 0; i < n; i++) {
        unsigned char *k = mh_malloc(48);
        if (!k) { printf("FAIL: allocation %ld returned NULL\n", i); return 1; }
        memset(k, (int) (i & 0xFF), 48);
        keep[i] = k;
        void *tmp = mh_malloc(64);
        if (!tmp) { printf("FAIL: scratch allocation %ld returned NULL\n", i); return 1; }
        memset(tmp, 0x5A, 64);
        mh_free(tmp);
    }
    double t1 = now();
    for (long i = 0; i < n; i++) {
        unsigned char *k = keep[i];
        for (int j = 0; j < 48; j++) {
            if (k[j] != (unsigned char) (i & 0xFF)) {
                printf("FAIL: retained block %ld byte %d was overwritten (%02x, want %02x)\n",
                       i, j, k[j], (unsigned char) (i & 0xFF));
                return 1;
            }
        }
    }
    *steps_out = mh___malloc_walk_steps();
    *secs_out = t1 - t0;
    free(keep);
    return 0;
}

int main(int argc, char **argv) {
    long n = (argc > 1) ? atol(argv[1]) : 20000;
    uint64_t steps = 0;
    double secs = 0;
    if (workload(n, &steps, &secs) != 0) return 1;
    unsigned r = mh___malloc_walk_report_count();
    if (r != 0) {
        printf("FAIL: %u heap-corruption walk report(s) on a clean workload\n", r);
        return 1;
    }
    printf("n=%ld steps=%llu heap=%luKB secs=%.4f\n",
           n, (unsigned long long) steps,
           mh_malloc_heap_highwater() >> 10, secs);
    return 0;
}
