// allocost - #245/#275: WHERE does the per-allocation time go in a MayteraOS
// Ring 3 process? Measured in the OS, on serial, with no GUI and no browser.
//
// WHY THIS EXISTS. The browser's script-built DOM path is now LINEAR in the
// number of nodes: both superlinear terms (the allocator's first-fit walk,
// b93cab6d; the kernel's uncoalesced VMA list, ad00bf9f) are fixed and shipping,
// and 20 equal batches building 500,000 DOM elements come out flat. What is left
// is a CONSTANT FACTOR, and it is large: the same engine sources, the same
// Duktape binding and the same fixture run about 20x slower inside MayteraOS
// than in a hosted glibc build of the identical code. A constant factor is not a
// complexity bug, so the honest move is to find out what it is made of.
//
// The control that makes the answer attributable was measured first, in the
// browser: a JS loop that allocates NOTHING runs at 0.018 us/iteration in the OS
// against 0.02 us/iteration hosted, i.e. the SAME SPEED. So neither the
// interpreter nor the CPU is the gap. Everything that allocates is 20x. The gap
// is inside malloc/free, and this app prices each layer of it separately.
//
// THE ROWS, and what each one is for:
//   nop            loop overhead, so every other row can be read net of it
//   gettid         ONE syscall, the cheapest syscall there is: the price unit
//   gettid2        TWO syscalls, the PREDICTION for the next row
//   lockpair       the SHIPPED pthread_mutex_lock + unlock, uncontended
//   lockpair_nosys the SAME Drepper CAS sequence with the thread-id lookup
//                  removed: the A/B arm, in the same process, in the same run
//   mallocfree     malloc(64) + free(), nothing retained, so the heap never
//                  grows and no page is ever mapped: pure allocator cost
//   mallocret      malloc(64) retained, so the kernel mmap and demand-fault
//                  paths are included too
//
// THE A/B IS IN ONE PROCESS ON PURPOSE. This host's load moves by a factor of
// ten inside a single session (measured: the same browser fixture came out at
// 17 us/item and at 150 us/item on two boots an hour apart), so two arms in two
// boots is not a comparison. lockpair and lockpair_nosys run microseconds apart
// in the same loop, so whatever the host is doing, it is doing it to both.
//
// Deliberately no printf(): printf allocates, and the allocator is the subject.
// Launched via /CONFIG/AUTORUN.CFG containing "/APPS/ALLOCOST".

#include "syscall.h"
#include "stdlib.h"
#include "pthread.h"
#include "errno.h"

#define ITERS    200000
#define REPEATS  5
#define NCASE    7

static char lb[256];
static unsigned lbn = 0;
static void ac(char c) { if (lbn < sizeof(lb)) lb[lbn++] = c; }
static void as(const char *s) { while (*s) ac(*s++); }
static void an(unsigned long long v) {
    char b[24]; int i = 23; b[i] = '\0';
    if (v == 0) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + (v % 10)); v /= 10; }
    as(&b[i]);
}
static void a_x10(unsigned long long num, unsigned long long den) {
    if (den == 0) { as("n/a"); return; }
    unsigned long long r = (num * 10ULL + den / 2ULL) / den;
    an(r / 10); ac('.'); an(r % 10);
}
static void flush_line(void) { ac('\n'); sys_write(1, lb, lbn); lbn = 0; }

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_m2 = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned long g_sink = 0;

// The A/B arm. This is pthread_mutex_lock/unlock's own uncontended fast path,
// copied from userland/libc/pthread.c with ONE thing removed: the unconditional
// pthread_self() at the top of each. Everything else, including the 3-state
// Drepper protocol and the contended branch, is the same code. It is NOT a
// proposed replacement and nothing else in this app uses it; it exists so the
// cost of that one call can be read off directly instead of inferred.
static inline unsigned int ab_cmpxchg(volatile unsigned int *p, unsigned int expect, unsigned int want) {
    unsigned int old;
    __asm__ volatile("lock cmpxchgl %2, %1"
                     : "=a"(old), "+m"(*p)
                     : "r"(want), "0"(expect)
                     : "memory");
    return old;
}
static inline unsigned int ab_xchg(volatile unsigned int *p, unsigned int v) {
    __asm__ volatile("xchgl %0, %1" : "+r"(v), "+m"(*p) : : "memory");
    return v;
}
static inline int ab_fetch_add(volatile unsigned int *p, int d) {
    int r = d;
    __asm__ volatile("lock xaddl %0, %1" : "+r"(r), "+m"(*p) : : "memory");
    return r;
}
static void ab_lock(pthread_mutex_t *m) {
    unsigned int c = ab_cmpxchg(&m->lock, 0, 1);
    if (c != 0) {
        if (c != 2) c = ab_xchg(&m->lock, 2);
        while (c != 0) { futex_wait(&m->lock, 2, 0); c = ab_xchg(&m->lock, 2); }
    }
}
static void ab_unlock(pthread_mutex_t *m) {
    if (ab_fetch_add(&m->lock, -1) != 1) { m->lock = 0; futex_wake(&m->lock, 1); }
}

static unsigned long long run_case(int which) {
    unsigned long long t0 = mono_us();
    switch (which) {
    case 0:
        for (long i = 0; i < ITERS; i++) g_sink += (unsigned long) i;
        break;
    case 1:
        for (long i = 0; i < ITERS; i++) g_sink += (unsigned long) syscall0(SYS_GETTID);
        break;
    case 2:
        for (long i = 0; i < ITERS; i++) {
            g_sink += (unsigned long) syscall0(SYS_GETTID);
            g_sink += (unsigned long) syscall0(SYS_GETTID);
        }
        break;
    case 3:
        for (long i = 0; i < ITERS; i++) {
            pthread_mutex_lock(&g_m); g_sink += 1; pthread_mutex_unlock(&g_m);
        }
        break;
    case 4:
        for (long i = 0; i < ITERS; i++) {
            ab_lock(&g_m2); g_sink += 1; ab_unlock(&g_m2);
        }
        break;
    case 5:
        for (long i = 0; i < ITERS; i++) {
            void *p = malloc(64);
            if (p) { *(volatile char *) p = (char) i; free(p); g_sink += 1; }
        }
        break;
    case 6:
        // Retained, so the heap GROWS: ITERS/8 keeps the arena to a few MB per
        // repeat while still exercising ensure_mapped() and the fault path.
        for (long i = 0; i < ITERS / 8; i++) {
            void *p = malloc(64);
            if (p) { *(volatile char *) p = (char) i; g_sink += 1; }
        }
        break;
    default: break;
    }
    unsigned long long t1 = mono_us();
    return (t1 > t0) ? (t1 - t0) : 0;
}

static const char *NAMES[NCASE] = {
    "nop", "gettid", "gettid2", "lockpair", "lockpair_nosys", "mallocfree", "mallocret"
};
static const long COUNTS[NCASE] = {
    ITERS, ITERS, ITERS, ITERS, ITERS, ITERS, ITERS / 8
};

// ------------------------------------------------------------------ correctness
//
// The speed fix in userland/libc/pthread.c stops fetching the thread id on
// mutex paths that never consult it. That is only safe if the paths that DO
// consult it still work, so this section exercises all three mutex types.
//
// IT CARRIES ITS OWN PROOF THAT IT CAN FAIL, which is the whole point: the
// SAME ownership assertion is made against an ERRORCHECK mutex (where it must
// hold) and against a NORMAL mutex (where, after this change, it must NOT).
// If both came out the same, the assertion would be measuring nothing. The
// NORMAL line is therefore reported as an OBSERVATION with both arms' expected
// values spelled out, not as a pass/fail: on the pre-fix libc it reads
// owner=<tid>, on the fixed one owner=0, and that difference IS the change.
//
// NO SECOND THREAD. The first version of this used pthread_create() to prove
// that a cross-thread unlock of an ERRORCHECK mutex is refused. It could not:
// pthread_create() returns EAGAIN (11) in an app launched from
// /CONFIG/AUTORUN.CFG, which means the kernel refused the clone, twice out of
// twice, on golden 2571. That is a real and separate finding, recorded in
// CHANGELOG/blame and NOT chased here. A forged owner value exercises the same
// branch deterministically and with no dependency on being allowed a thread.
//
// A deadlock would be a HANG rather than a wrong answer, so every step prints
// what it is about to do BEFORE doing it: the last line on serial names the
// step that hung.

static pthread_mutex_t g_ec;        // ERRORCHECK
static pthread_mutex_t g_rec;       // RECURSIVE
static pthread_mutex_t g_norm;      // NORMAL

static int g_fail = 0;
static void check(const char *what, long got, long want) {
    as("[ALLOCOST] CHECK "); as(what); as(" got="); an((unsigned long long) got);
    as(" want="); an((unsigned long long) want);
    if (got == want) as("  PASS"); else { as("  FAIL"); g_fail++; }
    flush_line();
}
static void observe(const char *what, unsigned long long v, const char *note) {
    as("[ALLOCOST] OBSERVE "); as(what); as("="); an(v); as("  "); as(note);
    flush_line();
}

static void correctness(void) {
    pthread_mutexattr_t a;
    unsigned int tid = (unsigned int) syscall0(SYS_GETTID);

    as("[ALLOCOST] correctness: NORMAL lock/unlock x1000"); flush_line();
    pthread_mutex_init(&g_norm, 0);
    for (int i = 0; i < 1000; i++) { pthread_mutex_lock(&g_norm); pthread_mutex_unlock(&g_norm); }
    check("normal_roundtrip_completed", 1, 1);

    as("[ALLOCOST] correctness: RECURSIVE double lock (a hang here is the bug)"); flush_line();
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_rec, &a);
    check("recursive_lock1", pthread_mutex_lock(&g_rec), 0);
    check("recursive_owner_is_self", (long)(g_rec.owner == tid), 1);
    check("recursive_lock2", pthread_mutex_lock(&g_rec), 0);
    check("recursive_unlock1", pthread_mutex_unlock(&g_rec), 0);
    check("recursive_unlock2", pthread_mutex_unlock(&g_rec), 0);
    check("recursive_relock", pthread_mutex_lock(&g_rec), 0);
    check("recursive_relock_unlock", pthread_mutex_unlock(&g_rec), 0);
    check("recursive_trylock", pthread_mutex_trylock(&g_rec), 0);
    check("recursive_trylock_nested", pthread_mutex_trylock(&g_rec), 0);
    check("recursive_trylock_unlock1", pthread_mutex_unlock(&g_rec), 0);
    check("recursive_trylock_unlock2", pthread_mutex_unlock(&g_rec), 0);

    as("[ALLOCOST] correctness: ERRORCHECK ownership"); flush_line();
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&g_ec, &a);
    check("errorcheck_unlock_unheld", pthread_mutex_unlock(&g_ec), EPERM);
    check("errorcheck_lock", pthread_mutex_lock(&g_ec), 0);
    // THE ASSERTION THE FIX COULD HAVE BROKEN: lock() must still record the
    // owner for an ERRORCHECK mutex, because unlock() compares against it.
    check("errorcheck_owner_is_self", (long)(g_ec.owner == tid), 1);
    // Forge a different owner: unlock must refuse. This is what a cross-thread
    // unlock looks like to the one branch that reads the field.
    g_ec.owner = tid ^ 0x5a5au;
    check("errorcheck_unlock_foreign_owner", pthread_mutex_unlock(&g_ec), EPERM);
    g_ec.owner = tid;
    check("errorcheck_unlock_owner", pthread_mutex_unlock(&g_ec), 0);

    // THE ARM THAT PROVES THE ASSERTION ABOVE IS LIVE. Same question, NORMAL
    // mutex. Pre-fix this reads the caller's tid; post-fix it reads 0, because
    // nothing consults it and the syscall that produced it is gone. Reported,
    // not graded, because the two arms legitimately differ.
    as("[ALLOCOST] correctness: same question on a NORMAL mutex"); flush_line();
    pthread_mutex_lock(&g_norm);
    observe("normal_owner_after_lock", (unsigned long long) g_norm.owner,
            "(pre-fix: the caller's tid; post-fix: 0. This difference IS the change.)");
    observe("this_tid", (unsigned long long) tid, "");
    pthread_mutex_unlock(&g_norm);

    as("[ALLOCOST] correctness: "); an((unsigned long long) g_fail);
    as(" failing check(s)"); flush_line();
}

int main(void) {
    as("[ALLOCOST] #245 where the per-allocation time goes, measured in the OS."); flush_line();
    as("[ALLOCOST] "); an(ITERS); as(" iterations per row, "); an(REPEATS);
    as(" repeats, MINIMUM reported (the host is shared)."); flush_line();

    unsigned long long best[NCASE], worst[NCASE];
    for (int c = 0; c < NCASE; c++) { best[c] = 0; worst[c] = 0; }
    for (int r = 0; r < REPEATS; r++) {
        for (int c = 0; c < NCASE; c++) {
            unsigned long long us = run_case(c);
            if (r == 0 || us < best[c]) best[c] = us;
            if (us > worst[c]) worst[c] = us;
        }
    }
    for (int c = 0; c < NCASE; c++) {
        as("[ALLOCOST] "); as(NAMES[c]);
        as(" min_us="); an(best[c]);
        as(" max_us="); an(worst[c]);
        as(" ns_per_op="); an((best[c] * 1000ULL) / (unsigned long long) COUNTS[c]);
        if (c > 0) {
            as(" x_gettid=");
            a_x10(best[c] * (unsigned long long) COUNTS[1],
                  best[1] * (unsigned long long) COUNTS[c]);
        }
        flush_line();
    }
    as("[ALLOCOST] heap_highwater_KB="); an((unsigned long long)(malloc_heap_highwater() >> 10));
    flush_line();
    correctness();
    as("[ALLOCOST] done sink="); an((unsigned long long) g_sink);
    as(" fail="); an((unsigned long long) g_fail); flush_line();
    sys_exit(0);
    return 0;
}
