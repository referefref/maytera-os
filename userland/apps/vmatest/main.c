// vmatest - #245: does the cost of growing the heap depend on how much of it is
// ALREADY mapped? Measured inside a real MayteraOS process, on serial, with no
// GUI and no compositor in the picture.
//
// WHY THIS EXISTS. b93cab6d fixed a genuine O(n^2) in the userland allocator and
// proved it with a host rig. The same measurement then showed a SECOND
// superlinear term above roughly 100 MB of heap that the allocator had been
// cleared of: a native rig running the SAME allocator over the same range was
// linear. 6255bf25 recorded it with the next step to take. The mechanism turned
// out to be in the KERNEL: userland/libc/stdlib.c ensure_mapped() grows the heap
// by mmap()ing only the page-aligned gap it needs, at an explicit address
// adjacent to the previous mapping, and do_mmap() created a separate vma_t for
// every one of those calls because nothing in the kernel ever coalesced. Every
// mmap and every first-touch page fault then walked that list from the head.
//
// WHAT THIS APP MEASURES, and why it is a fair test on a contended host. It
// grows the heap in equal INCREMENTS and times each one. The question is not
// "how many microseconds" (that varies with the host), it is "does increment 8
// cost more than increment 1". Under the defect the Kth increment walks a list
// that is K times longer, so the times rise roughly linearly and the RATIO
// last/first is the signal; the ratio cancels host speed, which a single
// absolute number cannot. Printed per increment so the shape is visible rather
// than inferred from two endpoints, which is the explicit lesson of the first
// half of this ticket.
//
// PHASE B is a proof-of-firing for the kernel's own instrument
// (rustkern/vmascale.rs). After the fix a heap-growing process coalesces to one
// VMA, so the [VMASCALE] warning line never appears, and an instrument whose
// only observed behaviour is silence cannot be told apart from a broken one. So
// this app also builds a LEGITIMATELY long VMA list (single pages with gaps
// between them, which cannot be coalesced because they are not adjacent) and the
// kernel line must appear on serial. That is a real condition, not a synthetic
// hook: a process really does pay a list walk for that shape.
//
// Deliberately does NOT use printf(): printf allocates, and this app's whole
// subject is the allocator's interaction with the kernel. Everything goes out
// through raw sys_write(), the same discipline heaptest uses.
//
// Launched via /CONFIG/AUTORUN.CFG containing "/APPS/VMATEST"; output lands on
// the serial console (the kernel routes fd 1 to kputc).

#include "syscall.h"
#include "stdlib.h"

#define INCREMENTS      8
#define INCREMENT_MB    8
#define CHUNK           512         // retained allocation size, bytes

// ONE sys_write PER LINE, COMPOSED IN A BUFFER FIRST.
//
// The first run of this app emitted each fragment with its own sys_write(), and
// the serial log came out unreadable: the kernel's own syslog echoes fd-1 writes
// with a timestamp prefix, so every fragment appeared twice and a number printed
// as three writes arrived as three separate interleaved lines
// ("increment", "3", " (heap now"). A measurement nobody can read off the log is
// not a measurement. Everything below appends into `lb` and flushes exactly once,
// at the newline.
static char lb[256];
static unsigned lbn = 0;

static void ac(char c) { if (lbn < sizeof(lb)) lb[lbn++] = c; }

static void as(const char *s) { while (*s) ac(*s++); }

static void an(unsigned long long v) {
    char b[24];
    int i = 23;
    b[i] = '\0';
    if (v == 0) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + (v % 10)); v /= 10; }
    as(&b[i]);
}

// One decimal place, integer arithmetic only. The kernel is soft-float and this
// app does not need float either; a ratio printed as "7.9" says everything a
// double would and cannot drift.
static void aratio_x10(unsigned long long num, unsigned long long den) {
    if (den == 0) { as("n/a"); return; }
    unsigned long long r10 = (num * 10ULL + den / 2ULL) / den;
    an(r10 / 10);
    ac('.');
    an(r10 % 10);
}

// Flush the composed line, newline included, as ONE write.
static void flush_line(void) {
    ac('\n');
    sys_write(1, lb, lbn);
    lbn = 0;
}

int main(void) {
    as("[VMATEST] #245 kernel VMA scaling, measured in the OS."); flush_line();
    as("[VMATEST] "); an(INCREMENTS); as(" increments of "); an(INCREMENT_MB);
    as(" MB each, retained "); an(CHUNK); as("-byte allocations."); flush_line();
    as("[VMATEST] The question is whether the LAST increment costs more than the"); flush_line();
    as("[VMATEST] FIRST. Equal cost means per-mmap and per-fault work does not"); flush_line();
    as("[VMATEST] depend on how much is already mapped."); flush_line();

    unsigned long long first_us = 0, last_us = 0;
    unsigned long per_inc = ((unsigned long)INCREMENT_MB << 20) / CHUNK;

    for (int k = 1; k <= INCREMENTS; k++) {
        unsigned long long t0 = mono_us();
        unsigned long failed = 0;
        for (unsigned long i = 0; i < per_inc; i++) {
            // Retained on purpose: the list must GROW. Never freed, so the
            // allocator always takes the heap-growth path and ensure_mapped()
            // issues a fresh mmap every time it crosses a page.
            volatile char *p = (volatile char *)malloc(CHUNK);
            if (!p) { failed++; break; }
            // Touch the first and last byte so the page is really faulted in by
            // Ring 3, which is the path mm_fault() -> vma_find() serves.
            p[0] = (char)k;
            p[CHUNK - 1] = (char)i;
        }
        unsigned long long t1 = mono_us();
        unsigned long long dt = (t1 > t0) ? (t1 - t0) : 0;

        as("[VMATEST] increment "); an((unsigned long long)k);
        as(" heap=");             an((unsigned long long)k * INCREMENT_MB);
        as("MB us=");             an(dt);
        if (failed) { as(" ALLOCATION-FAILED-after="); an(failed); }
        flush_line();

        if (k == 1) first_us = dt;
        if (k == INCREMENTS) last_us = dt;
    }

    as("[VMATEST] RATIO last/first = "); aratio_x10(last_us, first_us);
    as("   (1.0 = flat; ~"); an((unsigned long long)INCREMENTS);
    as(".0 = linear in heap size, i.e. quadratic overall)"); flush_line();

    // ---------------------------------------------------------------- phase B
    // Build a list the kernel CANNOT coalesce: one page mapped, one page skipped.
    // Well inside the anonymous arena (USER_WIN_MMAP_BASE = 512GB + 4GB) and far
    // enough in that it cannot collide with whatever the libc has already placed
    // at the arena base.
    as("[VMATEST] phase B: 400 NON-adjacent single-page mappings. The kernel's own"); flush_line();
    as("[VMATEST] [VMASCALE] line MUST appear below; it is the instrument that"); flush_line();
    as("[VMATEST] would catch a return of this defect, and an instrument only ever"); flush_line();
    as("[VMATEST] seen silent is indistinguishable from a broken one."); flush_line();
    unsigned long mapped = 0, refused = 0;
    unsigned long long base = 0x8100000000ULL + 0x10000000ULL;
    for (unsigned long i = 0; i < 400; i++) {
        void *want = (void *)(unsigned long)(base + i * 8192ULL);
        void *got = sys_mmap(want, 4096, 3 /* PROT_READ|PROT_WRITE */, 0);
        if (got == want) mapped++;
        else refused++;
    }
    as("[VMATEST] phase B mapped="); an(mapped);
    as(" refused=");                an(refused); flush_line();
    as("[VMATEST] done"); flush_line();

    sys_exit(0);
    return 0;
}
