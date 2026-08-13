// heaptest - #631 proof-of-firing for the libc allocator's bounded free-list
// walk guard (userland/libc/stdlib.c).
//
// WHY THIS EXISTS. A guard that has never been SEEN to fire is not a guard;
// this project has shipped several of those (blk_stale_skips had zero
// readers, security_init has zero callers, increment_build.sh was `exit 0`).
// So this app does not test the guard by reading the code: it deliberately
// constructs, in the live heap of a real MayteraOS process, each of the four
// corrupt list shapes that used to spin the allocator forever, and asserts
// that the process CAME BACK from the operation with the guard's report
// counter incremented. If the guard were removed, this app would hang at
// 100% CPU instead of printing a tally - which is exactly the failure mode
// it is protecting every other app from.
//
// Deliberately does NOT use printf(): printf allocates, and half of this
// test runs with a knowingly corrupt heap. Everything here goes out through
// raw sys_write(), the same discipline heap_write_str() uses inside the
// allocator itself.
//
// Launched via /CONFIG/AUTORUN.CFG containing "/APPS/HEAPTEST"; output lands
// on the serial console (kernel sys_write routes fd 1/2 to kputc).

#include "syscall.h"
#include "stdlib.h"

// The allocator's own report counter. Not in stdlib.h on purpose: this is
// test-only introspection, see the comment at its definition in stdlib.c.
extern unsigned __malloc_walk_report_count(void);

// Mirror of stdlib.c's private block header. BLK_HDR is the PADDED header
// size the allocator actually uses for pointer arithmetic (32, not
// sizeof(block_header_t) == 24) - getting this wrong would poke the wrong
// words and the test would prove nothing, so it is spelled out here exactly
// as stdlib.c spells it.
#define BLK_HDR 32
typedef struct block_header {
    unsigned long size;
    int free;
    struct block_header *next;
} block_header_t;
#define HDR(p) ((block_header_t *)((char *)(p) - BLK_HDR))

static int g_pass = 0, g_fail = 0;

static void ws(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

static void wnum(unsigned long v) {
    char b[24];
    int i = 23;
    b[i] = '\0';
    if (v == 0) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + (v % 10)); v /= 10; }
    ws(&b[i]);
}

static void ck(const char *what, int cond) {
    if (cond) { g_pass++; ws("  PASS "); } else { g_fail++; ws("  FAIL "); }
    ws(what);
    ws("\n");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    ws("\n==== HEAPTEST_BEGIN (#631 bounded free-list walk) ====\n");

    // ---- 0. CONTROL: a heavy, entirely well-formed alloc/free workload must
    // report NOTHING. Without this the whole test would be satisfied by a
    // guard that simply fires all the time, which would be worse than no
    // guard at all.
    {
        unsigned base = __malloc_walk_report_count();
        void *v[128];
        for (int i = 0; i < 128; i++) v[i] = malloc(48 + (unsigned)i * 8);
        for (int i = 0; i < 128; i += 2) free(v[i]);
        for (int i = 0; i < 128; i += 2) v[i] = malloc(32);
        for (int i = 0; i < 128; i++) v[i] = realloc(v[i], 200 + (unsigned)i);
        for (int i = 0; i < 128; i++) free(v[i]);
        ws("  (control did ~500 malloc/realloc/free with a clean list)\n");
        ck("control: clean heap produces ZERO walk-overrun reports",
           __malloc_walk_report_count() == base);
    }

    // ---- 1. coalesce_all() OUTER walk: an allocated block whose ->next
    // points at itself. Reached from free() -> coalesce_all(); the outer loop
    // does b = b->next forever.
    {
        void *a = malloc(96), *b = malloc(96), *c = malloc(96);
        block_header_t *ha = HDR(a);
        block_header_t *sv = ha->next;
        unsigned before = __malloc_walk_report_count();
        ha->next = ha;                       // self-cycle
        free(b);                             // -> coalesce_all()
        unsigned after = __malloc_walk_report_count();
        ha->next = sv;                       // repair before continuing
        ck("coalesce_all outer walk: self-cycle reported, free() returned",
           after > before);
        free(a); free(c);
    }

    // ---- 2. coalesce_all() INNER merge loop: the exact shape from the
    // report - b->next == X and X->next == X, with X free. b absorbs X, sets
    // b->next = X->next == X, and absorbs it again, forever. Every existing
    // check passes on every iteration: X stays in range, stays free, and its
    // size never changes, so block_size_sane(X) is true for ever. p1 and p2
    // are separated by an allocated `gap` so free() does not legitimately
    // merge them before the corruption is planted.
    {
        void *p1 = malloc(96), *gap = malloc(96), *p2 = malloc(96), *tail = malloc(96);
        block_header_t *h1 = HDR(p1), *h2 = HDR(p2);
        free(p1);
        free(p2);
        unsigned long s1 = h1->size, s2 = h2->size;
        block_header_t *sv1 = h1->next, *sv2 = h2->next;
        unsigned before = __malloc_walk_report_count();
        h1->next = h2;                       // b   -> X
        h2->next = h2;                       // X   -> X
        free(tail);                          // -> coalesce_all() merge loop
        unsigned after = __malloc_walk_report_count();
        h1->next = sv1; h2->next = sv2;      // repair pointers AND the sizes
        h1->size = s1;  h2->size = s2;       // the runaway merge inflated
        ck("coalesce_all merge loop: b->next->next==b->next reported, free() returned",
           after > before);
        free(gap);
    }

    // ---- 3. find_free_block() AND request_space()'s tail walk. One cycle
    // exercises both: malloc() calls find_free_block() first (its guard fires
    // and it returns NULL, meaning "no reusable block"), then falls through
    // to request_space(), whose tail walk hits the same cycle and fires its
    // own guard. Two reports from one malloc() is the expected result, and it
    // is also the proof that malloc() still SUCCEEDS on a cyclic list.
    {
        void *k = malloc(96);
        block_header_t *hk = HDR(k);
        unsigned before = __malloc_walk_report_count();
        hk->next = hk;                       // self-cycle, block allocated
        void *big = malloc(4096);            // find_free_block + request_space
        unsigned after = __malloc_walk_report_count();
        ck("find_free_block + request_space tail walk: cycle reported, malloc() returned",
           after >= before + 2);
        ck("malloc() still returned usable memory on a cyclic list", big != 0);
        if (big) {
            // Actually touch it; a "returned non-NULL" claim is worthless if
            // the pages are not there.
            for (int i = 0; i < 4096; i++) ((char *)big)[i] = (char)i;
            ck("the memory malloc() returned on a cyclic list is writable",
               ((char *)big)[4095] == (char)4095);
        }
        // No repair: request_space() deliberately relinks the new block onto
        // the last known-good node, so hk->next now legitimately points at
        // fresh storage. The tail past the corruption is orphaned - the same
        // already-documented consequence the other two walks have.
    }

    ws("==== HEAPTEST_END pass=");
    wnum((unsigned long)g_pass);
    ws(" fail=");
    wnum((unsigned long)g_fail);
    ws(" walk_reports=");
    wnum((unsigned long)__malloc_walk_report_count());
    ws(g_fail == 0 ? "  RESULT=OK\n" : "  RESULT=FAILED\n");

    // If the guard did not exist, control never reaches here at all.
    return g_fail == 0 ? 0 : 1;
}
