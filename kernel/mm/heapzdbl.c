// mm/heapzdbl.c - #heapdbl: PRODUCE BOTH CAUSES OF `magic=0x0` ON DEMAND, from
// known inputs, and show that the classifier tells them apart.
//
// WHY THIS EXISTS. `mm/heap.c`'s kfree() prints `magic=0x0` for two different
// defects and nothing distinguished them, so eleven refusals in one field capture
// (/root/co2600-fatapps-run.log, golden 2519) were filed as three unexplained
// heap double frees. They were not double frees. They were the #aicappersist
// corruption: a user page table allocated at a physical frame whose NUMBER lies
// inside the heap's own virtual window, then cleared through that number, writing
// 4096 zero bytes into the kernel heap. Every header in the victim page went to
// zero at once, which is why `ra=0x511ff8` names three file_t 192 bytes apart in
// ONE page and does it again in a second page.
//
// A DIAGNOSIS NOBODY CAN RE-RUN IS A STORY. This arm makes both causes happen on
// demand, in the live allocator, through the real kfree() path:
//
//   ARM A - COALESCED DOUBLE FREE. Two adjacent blocks; free the lower, then the
//           upper, so coalesce_blocks() absorbs the upper into the lower and sets
//           its magic to 0; then free the upper AGAIN. kfree() must refuse it
//           with magic=0x0, and the classifier must say DOUBLE FREE, because the
//           payload is still there.
//   ARM B - WHOLE-PAGE WIPE. A run of blocks the same size as file_t, long enough
//           that it fully contains one 4 KB page, every byte of which we own.
//           memset that page to zero, which is byte-for-byte the write
//           #aicappersist performed. Free the blocks whose headers are inside it.
//           kfree() must refuse each one with magic=0x0, 192 bytes apart, and the
//           classifier must say WHOLE-PAGE WIPE.
//
// THE POINT IS THAT ARM A AND ARM B PRODUCE THE SAME kfree() MESSAGE. If the
// [HEAPZERO] verdicts ever agree with each other, the classifier is worthless and
// the next person to read `magic=0x0` is back to guessing.
//
// OFF BY DEFAULT, armed only by /HEAPZDBL.TXT on the ESP, because it deliberately
// damages heap blocks: the blocks it wipes can never be freed (kfree correctly
// refuses them), so an armed run LEAKS the page it wiped plus the tail of its own
// run. That is bounded, stated, and the price of a reproducer that uses the real
// allocator instead of a mock of it. Same arming mechanism as /SLOTDBL.TXT.
//
// LANGUAGE NOTE (project rule: new kernel code is Rust unless there is a stated
// reason). The DECISION LOGIC IS RUST - rustkern/heapzero.rs holds the counting
// and the verdict, and this file does not duplicate a line of it. What is left
// here is C because it is deliberate manipulation of the C allocator's own blocks
// through kmalloc/kfree/memset, i.e. glue entangled with the thing under test,
// which is the same reason proc/slotdbl.c gives.

#include "heap.h"
#include "pmm.h"
#include "heapzero.h"
#include "../serial.h"
#include "../string.h"
#include "../security/selftest_registry.h"

int g_heapzdbl_gate = 0;   // set by main.c from /HEAPZDBL.TXT

// 48 * 192 = 9216 bytes of blocks, which contains a whole 4096-byte page however
// the run happens to land. 152 is sizeof(file_t) on this build, so the stride is
// the 192 the field capture shows, not a number chosen to look like it.
#define HZD_OBJ_BYTES   152
#define HZD_STRIDE      192
#define HZD_RUN         48

static uint64_t hzd_invalid_before, hzd_double_before;

static void hzd_mark(void) {
    hzd_invalid_before = heap_invalid_free_count();
    hzd_double_before  = heap_double_free_count();
}

static void hzd_report(const char *arm, uint64_t want_invalid) {
    uint64_t inv = heap_invalid_free_count() - hzd_invalid_before;
    uint64_t dbl = heap_double_free_count()  - hzd_double_before;
    kprintf("[HEAPZDBL] %s: kfree refused %llu pointer(s) (expected %llu), "
            "pre-coalesce double frees %llu\n",
            arm, (unsigned long long)inv, (unsigned long long)want_invalid,
            (unsigned long long)dbl);
}

// ARM A: a genuine double free, after coalescing. One refusal expected.
static void hzd_arm_a(void) {
    // Two blocks that are adjacent is the precondition, not an assumption: check
    // it and say NOTRUN rather than reporting a pass for a test that did not run.
    uint8_t *a = (uint8_t *)kmalloc(HZD_OBJ_BYTES);
    uint8_t *b = (uint8_t *)kmalloc(HZD_OBJ_BYTES);
    if (!a || !b) {
        selftest_notrun("HEAPZDBL-A", "kmalloc failed");
        if (a) kfree(a);
        if (b) kfree(b);
        return;
    }
    if ((uint64_t)b - (uint64_t)a != HZD_STRIDE) {
        kprintf("[HEAPZDBL] ARM A NOTRUN: the two blocks are %llu apart, not %u; "
                "the free list did not hand out a contiguous pair this boot\n",
                (unsigned long long)((uint64_t)b - (uint64_t)a), HZD_STRIDE);
        kfree(a); kfree(b);
        return;
    }
    // A payload that is NOT zero is the whole discriminator for arm A: a
    // coalesced double free leaves it alone, a page wipe does not.
    memset(b, 0x5a, HZD_OBJ_BYTES);

    kfree(a);               // a goes on the free list
    kfree(b);               // b's backward merge: a absorbs b, b->magic = 0
    hzd_mark();
    kprintf("[HEAPZDBL] ARM A: freeing %p a SECOND time. Expect one kfree refusal "
            "with magic=0x0 and a COALESCED DOUBLE FREE verdict.\n", b);
    kfree(b);               // THE DOUBLE FREE
    hzd_report("ARM A (coalesced double free)", 1);
}

// ARM B: the #aicappersist whole-page wipe. ~21 refusals expected.
static void hzd_arm_b(void) {
    static uint8_t *p[HZD_RUN];
    int n = 0;
    for (; n < HZD_RUN; n++) {
        p[n] = (uint8_t *)kmalloc(HZD_OBJ_BYTES);
        if (!p[n]) break;
    }
    if (n < HZD_RUN) {
        selftest_notrun("HEAPZDBL-B", "kmalloc ran out partway through the run");
        for (int i = 0; i < n; i++) kfree(p[i]);
        return;
    }
    // Contiguity is a PRECONDITION. If the allocator did not hand out one run,
    // the page we are about to wipe might contain somebody else's live data, and
    // the honest thing is to not do it.
    for (int i = 1; i < n; i++) {
        if ((uint64_t)p[i] - (uint64_t)p[i - 1] != HZD_STRIDE) {
            kprintf("[HEAPZDBL] ARM B NOTRUN: the run is not contiguous at index "
                    "%d (%llu apart); refusing to zero a page I may not own\n",
                    i, (unsigned long long)((uint64_t)p[i] - (uint64_t)p[i - 1]));
            for (int j = 0; j < n; j++) kfree(p[j]);
            return;
        }
    }
    uint64_t lo = (uint64_t)p[0] - 32;                       // our first header
    uint64_t hi = (uint64_t)p[n - 1] + HZD_OBJ_BYTES;        // end of our bytes
    uint64_t page = (lo + (PMM_PAGE_SIZE - 1)) & ~((uint64_t)PMM_PAGE_SIZE - 1);
    if (page + PMM_PAGE_SIZE > hi) {
        kprintf("[HEAPZDBL] ARM B NOTRUN: the run [0x%llx,0x%llx) does not contain "
                "a whole page\n", (unsigned long long)lo, (unsigned long long)hi);
        for (int j = 0; j < n; j++) kfree(p[j]);
        return;
    }

    // Free, FIRST, every block whose header is outside the page. They are still
    // valid, so they go back cleanly, and nothing we are about to damage can end
    // up on the free list. Only the blocks whose headers are inside the page are
    // kept, and those are the ones that leak.
    int inside = 0;
    for (int i = 0; i < n; i++) {
        uint64_t hdr = (uint64_t)p[i] - 32;
        if (hdr >= page && hdr + 32 <= page + PMM_PAGE_SIZE) { inside++; continue; }
        kfree(p[i]);
        p[i] = 0;
    }

    kprintf("[HEAPZDBL] ARM B: zeroing the whole 4096-byte page 0x%llx, which this "
            "run owns end to end. That is byte-for-byte the write a user page "
            "table performed through a shadowed physical==virtual pointer "
            "(#aicappersist). %d block header(s) are inside it.\n",
            (unsigned long long)page, inside);

    hzd_mark();
    memset((void *)page, 0, PMM_PAGE_SIZE);   // THE CORRUPTION, reproduced

    for (int i = 0; i < n; i++) {
        if (p[i]) kfree(p[i]);                // each must be REFUSED, magic=0x0
    }
    hzd_report("ARM B (whole-page wipe)", (uint64_t)inside);
    kprintf("[HEAPZDBL] ARM B leaked %d block(s) (~%d bytes) on purpose: kfree() "
            "correctly refuses a wiped header, so they can never be returned. "
            "That is the cost of reproducing it in the real allocator.\n",
            inside, inside * HZD_STRIDE);
}

void heapzdbl_start(void) {
    if (!g_heapzdbl_gate) return;
    kprintf("[HEAPZDBL] ARMED by /HEAPZDBL.TXT. Both causes of a `magic=0x0` kfree "
            "refusal follow. The two [HEAPZERO] verdicts MUST DIFFER; if they "
            "agree, the classifier is not discriminating and `magic=0x0` is back "
            "to being unreadable.\n");
    hzd_arm_a();
    hzd_arm_b();
    kprintf("[HEAPZDBL] done.\n");
}
