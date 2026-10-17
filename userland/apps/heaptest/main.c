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
// #245 domscript: the guards report through TWO channels. A corruption caught
// by a value guard (range, size sanity, "this block is not in its own bin")
// never touches the walk-overrun counter, and after the size-binned free list
// landed, some shapes are ALWAYS caught that way. Counting only one channel
// would score a loudly reported corruption as a silent continue.
extern unsigned __malloc_diag_report_count(void);
static unsigned reports(void) {
    return __malloc_walk_report_count() + __malloc_diag_report_count();
}

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
    struct block_header *prev;     // #245 domscript
} block_header_t;
#define HDR(p) ((block_header_t *)((char *)(p) - BLK_HDR))

// #245 domscript: the free-list links live in the FREE block's payload, so a
// bin-list cycle is poked through the returned pointer itself. Mirror of
// stdlib.c's free_link_t / FLINK().
typedef struct free_link {
    block_header_t *fnext;
    block_header_t *fprev;
} free_link_t;
#define FL(p) ((free_link_t *)(p))

// Big enough to land in the catch-all bin (stdlib.c: HEAP_BIN_MAX_EXACT 1024),
// the only bin find_free_block() has to SEARCH and so the only one with a walk.
#define BIG_DATA 4096

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

// #245 domscript: DRAIN DAMAGE BETWEEN SCENARIOS, AND PROVE THE HEAP WENT QUIET.
//
// Each scenario asserts "the report count went up", so a scenario that inherits
// a still-noisy heap from the previous one can be credited with a report it did
// not cause, which is a test that can pass for the wrong reason. The allocator
// reports a damaged free bin ONCE and then drops it, so one pass of ordinary
// traffic is enough to flush it; a second pass must then be silent. settle()
// does both and returns whether the heap really did go quiet, and every
// scenario asserts that before it takes its baseline.
static void traffic(void) {
    void *v[8];
    for (int i = 0; i < 8; i++) v[i] = malloc(64 + (unsigned)i * 192);
    for (int i = 0; i < 8; i++) free(v[i]);
    void *b = malloc(8192);
    if (b) free(b);
}

static int settle(void) {
    traffic();
    unsigned q = reports();
    traffic();
    return reports() == q;
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
        unsigned base = reports();
        void *v[128];
        for (int i = 0; i < 128; i++) v[i] = malloc(48 + (unsigned)i * 8);
        for (int i = 0; i < 128; i += 2) free(v[i]);
        for (int i = 0; i < 128; i += 2) v[i] = malloc(32);
        for (int i = 0; i < 128; i++) v[i] = realloc(v[i], 200 + (unsigned)i);
        for (int i = 0; i < 128; i++) free(v[i]);
        ws("  (control did ~500 malloc/realloc/free with a clean list)\n");
        ck("control: clean heap produces ZERO corruption reports",
           reports() == base);
    }

    // ---- 1. free()'s FORWARD MERGE: a block whose ->next points at itself.
    // #245 domscript: this used to reach coalesce_all()'s outer walk, which no
    // longer exists (backward coalescing is now O(1) through ->prev). The same
    // one-word corruption is now met by the merge loop in free(), which absorbs
    // the self-referential successor, doubles the block's size doing so, and is
    // stopped by block_size_sane() once the block would run past heap_end.
    // Being bounded by a VALUE guard rather than by the step budget is why the
    // report arrives on the diag channel.
    {
        ck("heap is quiet before scenario 1", settle());
        void *a = malloc(96), *b = malloc(96), *c = malloc(96);
        block_header_t *ha = HDR(a);
        unsigned long sa = ha->size;
        block_header_t *sv = ha->next;
        unsigned before = reports();
        ha->next = ha;                       // self-cycle
        free(a);                             // -> free()'s forward merge
        unsigned after = reports();
        ha->next = sv;                       // repair before continuing
        ha->size = sa;                       // the runaway merge inflated it
        ha->free = 0;
        ck("free() forward merge: self-cycle reported, free() returned",
           after > before);
        free(b); free(c);
    }

    // ---- 2. find_free_block()'s BIN WALK: a cycle in the free list itself.
    // #245 domscript: find_free_block() no longer walks the all-blocks list, so
    // the corruption has to go where the walk now looks, which is the free
    // block's own payload. A block sitting in the catch-all bin that is too
    // small for the request is the only case that steps at all, and the walk's
    // step budget comes from that bin's recorded length, so a loop of one is
    // caught in a handful of steps however large the heap is.
    {
        ck("heap is quiet before scenario 2", settle());
        // MEASURED IN THE OS (the first version of this scenario FAILED here):
        // free() coalesces in BOTH directions now, so on a busy heap the block
        // being freed can be absorbed into a free neighbour, at which point
        // HDR(big) is no longer a bin node and poking its payload corrupts
        // nothing. The fixture then proves nothing while looking like a
        // regression. The two allocated spacers make the merge impossible, so
        // `big` is guaranteed to enter a bin as itself, and as the most recently
        // freed block of its class it is guaranteed to be that bin's head.
        void *lo = malloc(96);
        void *big = malloc(BIG_DATA);
        void *hi = malloc(96);
        void *keep = lo;                     // freed with the rest below
        free(big);                           // -> catch-all bin, intact
        FL(big)->fnext = HDR(big);           // bin link points at its own block
        unsigned before = reports();
        void *huge = malloc(BIG_DATA * 8);   // too big for `big`: must step
        unsigned after = reports();
        FL(big)->fnext = 0;                  // repair the bin immediately: the
                                             // budget BREAKS out of the scan
                                             // rather than dropping the bin, so
                                             // without this the cycle would
                                             // still be there for scenario 3
        ck("find_free_block bin walk: bin cycle reported, malloc() returned",
           after > before);
        ck("malloc() still returned usable memory on a cyclic bin", huge != 0);
        if (huge) {
            for (int i = 0; i < BIG_DATA * 8; i++) ((char *)huge)[i] = (char)i;
            ck("the memory malloc() returned on a cyclic bin is writable",
               ((char *)huge)[BIG_DATA * 8 - 1] == (char)(BIG_DATA * 8 - 1));
            free(huge);
        }
        free(keep);
        free(hi);
    }

    // ---- 3. request_space()'s tail walk, reached by closing the loop AT THE
    // TAIL. #245 domscript, both halves MEASURED IN THE OS rather than assumed:
    //
    //   - It used to fire TWICE because find_free_block() walked the same
    //     chain. It now walks only the free bins, so ONE report is the correct
    //     expectation and demanding two would fail on a better allocator.
    //   - Self-cycling a block in the MIDDLE of the list no longer reaches this
    //     walk at all once the heap is big, because request_space() takes its
    //     O(1) cached-tail append and never walks. That fast path is guarded by
    //     `heap_last->next == NULL`, so the corruption has to go on the TAIL:
    //     then the fast path is correctly refused and the validated walk runs,
    //     finds the loop and reports. The first version of this scenario poked
    //     a middle block, reached nothing, and FAILED in the OS while passing
    //     on a host heap small enough to miss the fast path.
    //
    // malloc() must still SUCCEED afterwards.
    {
        ck("heap is quiet before scenario 3", settle());
        void *k = malloc(96);
        block_header_t *tail = HDR(k);
        int guard = 0;
        while (tail->next && guard++ < 100000) tail = tail->next;
        ck("found the tail of the block list to corrupt", guard < 100000);
        // The request has to be one that find_free_block() CANNOT satisfy, or
        // malloc() returns from a bin and request_space() is never called.
        // MEASURED: with a fixed 4096 this scenario silently reused the 32 KB
        // block scenario 2 had freed and reported nothing. Asking for more than
        // the whole arena has ever been makes that impossible by construction:
        // no free block can be larger than the arena that contains it.
        size_t want = malloc_heap_highwater() + 65536;
        unsigned before = reports();
        tail->next = tail;                   // self-cycle AT THE TAIL
        void *big = malloc(want);            // -> request_space tail walk
        unsigned after = reports();
        ck("request_space tail walk: cycle reported, malloc() returned",
           after >= before + 1);
        ck("malloc() still returned usable memory on a cyclic list", big != 0);
        if (big) {
            // Actually touch it; a "returned non-NULL" claim is worthless if
            // the pages are not there. Both ends plus the middle, since this
            // allocation is now megabytes rather than one page.
            char *bb = (char *)big;
            bb[0] = 0x5A; bb[want / 2] = 0x5A; bb[want - 1] = 0x5A;
            ck("the memory malloc() returned on a cyclic list is writable",
               bb[0] == 0x5A && bb[want / 2] == 0x5A && bb[want - 1] == 0x5A);
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
    ws(" guard_reports=");
    wnum((unsigned long)__malloc_diag_report_count());
    ws(g_fail == 0 ? "  RESULT=OK\n" : "  RESULT=FAILED\n");

    // If the guard did not exist, control never reaches here at all.
    return g_fail == 0 ? 0 : 1;
}
