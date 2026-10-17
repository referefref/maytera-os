// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// cpython_alloc_preload.c - #245: make the REAL MayteraOS userland allocator be
// malloc/free/realloc/calloc for a host CPython 3.11, so that the only thing
// differing between two runs of one Python workload is the allocator revision.
// Same method as the #245 browser rig. Driven by run_cpython_alloc.sh; see that
// file's header for what the measurement means and what makes it trustworthy.
//
// POINTER OWNERSHIP IS DECIDED BY ADDRESS, not by a tag. The MayteraOS heap is at
// a fixed HEAP_START 0x8040000000 and nothing else is, so a pointer outside that
// window belongs to glibc and is handed straight back to it. That is what lets
// posix_memalign/aligned_alloc stay UNINTERPOSED and still correct: the allocator
// has no aligned entry point, so interposing them would mean inventing one, and
// a free() that could not tell the two heaps apart would corrupt instead.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/mman.h>

#define SYS_WRITE 13
#define SYS_MMAP  21

#define MH_HEAP_START 0x8040000000ULL
#define MH_HEAP_END   (MH_HEAP_START + (512ULL * 1024 * 1024))

// Unit under test, symbol-prefixed by build.sh's objcopy pass.
void *mh_malloc(unsigned long);
void  mh_free(void *);
void *mh_realloc(void *, unsigned long);
void *mh_calloc(unsigned long, unsigned long);
uint64_t mh___malloc_walk_steps(void);

// ---- host backing for everything the freestanding allocator calls ---------
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

// ---- glibc passthrough for pointers we did not hand out ------------------
static void (*real_free)(void *);
static void *(*real_realloc)(void *, size_t);
static size_t (*real_usable)(void *);

static void resolve(void) {
    if (real_free) return;
    real_free    = dlsym(RTLD_NEXT, "free");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    real_usable  = dlsym(RTLD_NEXT, "malloc_usable_size");
}

static int mine(const void *p) {
    uint64_t a = (uint64_t)p;
    return a >= MH_HEAP_START && a < MH_HEAP_END;
}

// ---- counters ------------------------------------------------------------
static uint64_t n_malloc, n_free, n_realloc, n_calloc, n_fwd_free;

void *malloc(size_t n) { n_malloc++; return mh_malloc(n); }

void *calloc(size_t a, size_t b) { n_calloc++; return mh_calloc(a, b); }

void free(void *p) {
    if (!p) return;
    if (mine(p)) { n_free++; mh_free(p); return; }
    resolve();
    n_fwd_free++;
    if (real_free) real_free(p);
}

void *realloc(void *p, size_t n) {
    n_realloc++;
    if (!p) return mh_malloc(n);
    if (mine(p)) return mh_realloc(p, n);
    resolve();
    return real_realloc ? real_realloc(p, n) : NULL;
}

void *reallocarray(void *p, size_t a, size_t b) {
    if (b && a > (size_t)-1 / b) return NULL;
    return realloc(p, a * b);
}

size_t malloc_usable_size(void *p) {
    if (!p) return 0;
    if (mine(p)) return *(size_t *)((char *)p - 32);   // block_header_t.size
    resolve();
    return real_usable ? real_usable(p) : 0;
}

// cfree was removed from glibc but some code still references it.
void cfree(void *p) { free(p); }

__attribute__((destructor))
static void report(void) {
    // Deterministic, run-to-run identical. The gate quantity; wall clock is
    // measured inside the Python workload and decides nothing here.
    fprintf(stderr, "[RIG] mallocs=%llu callocs=%llu reallocs=%llu frees=%llu "
                    "fwd_frees=%llu walk_steps=%llu\n",
            (unsigned long long)n_malloc, (unsigned long long)n_calloc,
            (unsigned long long)n_realloc, (unsigned long long)n_free,
            (unsigned long long)n_fwd_free,
            (unsigned long long)mh___malloc_walk_steps());
}
