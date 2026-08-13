// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// malloc_cycle_test.c - #631 regression test for the MayteraOS userland
// allocator's free-list walks (userland/libc/stdlib.c).
//
// WHAT THIS PROVES
//   The REAL allocator is compiled in here (no mock, no reimplementation) with
//   its syscalls redirected to the host: SYS_MMAP -> mmap(2) at the very same
//   fixed heap base the OS uses, SYS_WRITE -> write(2). A corrupted `next`
//   pointer is then written directly into a block header, exactly the shape a
//   heap overflow produces (one aligned word into the FOLLOWING block's
//   header), and each of the allocator's free-list walks is driven over it.
//
//   Each scenario is a SEPARATE PROCESS (pick it with argv[1]) so the walks
//   start from a clean heap and one hang cannot mask another.
//
//   Termination is bounded EXTERNALLY, by run_malloc.sh's timeout(1), not by
//   anything inside this file: a test that decides for itself when it has
//   spun long enough is a test that can be wrong about it. Completing at all
//   is the pass condition; the negative arm (run_malloc.sh recompiles the
//   allocator with the step budget disabled) must NOT complete.
//
// Build/run: ./run_malloc.sh (in this directory).

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

#define SYS_WRITE 13
#define SYS_MMAP  21

// The unit under test, symbol-prefixed by run_malloc.sh's objcopy pass so it
// can be linked next to glibc's own allocator without colliding.
void    *mh_malloc(unsigned long);
void     mh_free(void *);
void    *mh_realloc(void *, unsigned long);
unsigned mh___malloc_walk_report_count(void);

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

// --- mirror of the allocator's block layout, for injecting corruption -----
// MUST track stdlib.c's block_header_t and BLK_HDR. If they ever diverge this
// test writes to the wrong offset and stops testing anything, so assert the
// one property that would catch it: the header must fit in BLK_HDR bytes.
#define BLK_HDR 32
typedef struct block_header {
    size_t size;
    int free;
    struct block_header *next;
} block_header_t;
_Static_assert(sizeof(block_header_t) <= BLK_HDR,
               "#631 test: block_header_t no longer fits BLK_HDR; sync with stdlib.c");

static block_header_t *hdr(void *p) {
    return (block_header_t *)((uint8_t *)p - BLK_HDR);
}

static int done(const char *what) {
    unsigned r = mh___malloc_walk_report_count();
    printf("RETURNED from %s; walk-overrun reports=%u\n", what, r);
    if (r == 0) {
        printf("FAIL: the walk returned but reported nothing (silent continue)\n");
        return 1;
    }
    return 0;
}

// 1. free() -> coalesce_all(): the walk every app pays on every free().
static int sc_coalesce(void) {
    void *a = mh_malloc(64), *b = mh_malloc(64), *c = mh_malloc(64);
    (void)c;
    mh_free(b);
    hdr(a)->next = hdr(a);              // one aligned word: a->next = a
    mh_free(a);
    return done("free()/coalesce_all");
}

// 2. malloc() -> find_free_block(): a FREE block pointing at itself.
static int sc_findfree(void) {
    void *p1 = mh_malloc(48), *p2 = mh_malloc(48), *p3 = mh_malloc(48);
    (void)p1; (void)p3;
    mh_free(p2);
    hdr(p2)->next = hdr(p2);
    void *q = mh_malloc(4096);
    int bad = done("malloc()/find_free_block");
    // Detecting the cycle must not turn into an allocation failure: NULL from
    // find_free_block() means "nothing reusable", and request_space() still
    // has to satisfy the caller.
    if (!q) { printf("FAIL: malloc() returned NULL instead of growing the heap\n"); bad = 1; }
    else printf("ok: malloc() still satisfied the request after detecting the cycle\n");
    return bad;
}

// 3. request_space() tail walk: only reached when the heap must grow.
static int sc_tailwalk(void) {
    void *x = mh_malloc(32);
    block_header_t *h = hdr(x), *tail = h;
    int guard = 0;
    while (tail->next && guard++ < 10000) tail = tail->next;
    tail->next = h;                     // close the loop at the end
    void *big = mh_malloc(256 * 1024);  // forces growth -> tail walk
    (void)big;
    return done("request_space tail walk");
}

// 4. A multi-node backward loop, not merely a self-reference: proves the
//    budget catches a CYCLE, not just the degenerate b->next == b case that a
//    single equality check would also catch.
static int sc_multiloop(void) {
    void *v[8];
    for (int i = 0; i < 8; i++) v[i] = mh_malloc(64);
    for (int i = 0; i < 8; i += 2) mh_free(v[i]);
    hdr(v[7])->next = hdr(v[1]);
    mh_free(v[3]);
    return done("multi-node backward loop");
}

// 5. After a corruption has been reported the allocator must keep WORKING:
//    the fallback bound must stay a bound, not collapse to zero and reject
//    every subsequent allocation.
static int sc_survive(void) {
    void *a = mh_malloc(64), *b = mh_malloc(64);
    (void)b;
    hdr(a)->next = hdr(a);
    mh_free(a);
    for (int i = 0; i < 2000; i++) {
        void *p = mh_malloc((i % 97) + 1);
        if (!p) { printf("FAIL: allocation %d failed on a damaged heap\n", i); return 1; }
        memset(p, 0x5A, (i % 97) + 1);
        if (i % 3 == 0) mh_free(p);
    }
    printf("ok: 2000 allocations completed on a heap known to be damaged\n");
    return 0;
}

int main(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 0;
    switch (n) {
        case 1: return sc_coalesce();
        case 2: return sc_findfree();
        case 3: return sc_tailwalk();
        case 4: return sc_multiloop();
        case 5: return sc_survive();
        default:
            fprintf(stderr, "usage: %s <1..5>\n", argv[0]);
            return 2;
    }
}
