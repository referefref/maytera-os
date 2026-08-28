// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// stdlib.c - Standard library implementation
#include "stdlib.h"
#include "syscall.h"
#include "string.h"
#include "errno.h"
#include "pthread.h"
#include "stdio.h"   // #745: exit() must fflush() buffered stdio, see below
#define errno_set(e) (errno = (e))

// ============================================================================
// Memory Allocation (simple bump allocator with free list)
// ============================================================================
//
// #421 (AssaultCube port) phase 7: this allocator had ZERO locking around
// heap_start/heap_end/heap_mapped or the block_header_t singly-linked list,
// even though MayteraOS userland threads (clone()-based, sharing one address
// space, see userland/libc/pthread.c) are real. AssaultCube is the first app
// in this tree to call malloc()/free()/realloc() from more than one thread
// under real concurrent load (it clones worker threads during map load, see
// the [PROC] "Cloned thread of 'ASSAULTCUBE'" trace right before the crash
// this fixes). Two threads racing inside coalesce_all()/request_space() can
// lose an update to a block's `next` pointer (classic concurrent-linked-list
// corruption), which silently plants garbage into a block header. The
// corruption always surfaces LATER, on a totally unrelated free() that walks
// into the clobbered `next` pointer - it dereferenced a non-canonical
// address (RDX=0x9696040302100000, not just unmapped) and took a real #GP
// (General Protection Fault, not #PF) inside coalesce_all() at
// userland/libc/stdlib.c, called from AssaultCube's worldio.cpp
// `delete[] smallworld;` after `createservworld()`, tens of allocations
// after the actual race. Root-caused by symbolizing the crash RIP
// (0x801650f3) with addr2line + objdump against the real crashing ELF,
// confirming the faulting instruction was `mov ecx,[rdx+8]` (b->free) with
// rdx loaded from a corrupted b->next, then reading this file and finding
// the allocator had no synchronization at all - not guessed.
//
// Fix: reuse the EXISTING shared pthread_mutex_t primitive (userland/libc/
// pthread.h, futex-backed, already used throughout this tree) to serialize
// the whole allocator, per this repo's mandatory "reuse the canonical
// primitive, never hand-roll a lock" rule. The lock is acquired once per
// public entry point (malloc/free/calloc's internal malloc call/realloc);
// realloc does its own direct block-header work AND may need to allocate a
// replacement block, so it uses the internal non-locking *_locked() helpers
// directly instead of nesting a second lock acquisition (this mutex type is
// PTHREAD_MUTEX_NORMAL, non-recursive - a nested lock from the same thread
// would deadlock, not just be redundant).
// ============================================================================

static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;

// #522 stage 3: the libc heap lives in the DEDICATED USER WINDOW, not PDPT[2].
//
// It used to be 0x90000000, and the comment that stood here argued that
// 0x90000000 + 512MB = 0xB0000000 was "safely inside the documented 2-3GB
// PDPT[2] fixed-base window". That reasoning was wrong in a way nothing on
// QEMU could reveal: PDPT[2] is shared with DEVICE MMIO, and the framebuffer's
// address is chosen by FIRMWARE. On the user's real iMac14,4 the UEFI GOP
// framebuffer sits at 0x90000000 for 8MB - the exact base of this heap. The
// heap and the display were the same virtual address, and only
// vmm_punch_demand_range() (#511) stood between a Ring-0 write to an
// unfaulted heap page and pixels on the screen. QEMU's std-VGA lands at
// 0x80000000, which is why every VM boot looked fine.
//
// The heap is runtime-allocated, so moving it needs a RECOMPILE but NOT a
// relink: no ELF load address changes.
//
// MIRRORS kernel/mm/vmm.h USER_WIN_HEAP_BASE. The kernel does not enforce this
// value (an explicit-address mmap is honoured wherever it points), so the two
// definitions can drift; the static assertion below is what catches the drift
// that would actually hurt, namely the heap growing into the mmap arena.
#define HEAP_START      0x8040000000ULL         // USER_WIN_BASE + 1GB
#define USER_WIN_MMAP_BASE_MIRROR 0x8100000000ULL   // kernel USER_WIN_MMAP_BASE

#define HEAP_MAX_SIZE   (512 * 1024 * 1024)     // 512 MB max heap

// #522 stage 3: the heap must not be able to grow into the anonymous-mmap
// arena. Compile-time, so a future bump to either constant fails the build
// instead of silently overlapping two arenas at runtime.
_Static_assert(HEAP_START + HEAP_MAX_SIZE <= USER_WIN_MMAP_BASE_MIRROR,
               "#522: libc heap ceiling overlaps the kernel mmap arena");

typedef struct block_header {
    size_t size;                    // Size of data area
    int free;                       // 1 if free, 0 if allocated
    struct block_header *next;      // Next block in list
} block_header_t;

// Physical header size used for pointer arithmetic. Padded up to 16 so that,
// since HEAP_START is 16-aligned and every allocation size is rounded to a
// multiple of 16, EVERY pointer malloc() returns is 16-byte aligned. glibc and
// mlibc both guarantee max_align_t (16 on x86-64); CPython, SSE and doubles
// rely on it. (Previously data started at sizeof()==24 -> only 8-aligned.)
#define BLK_HDR 32

static block_header_t *heap_start = NULL;
static uint64_t heap_end = HEAP_START;
// First byte NOT yet backed by a mapped RW page. Always page-aligned.
// The allocator only ever maps fresh pages beyond this watermark, so it never
// re-maps (and thus never zeroes/leaks) a page that already holds live data,
// and it always maps enough pages to fully cover an allocation that straddles
// a page boundary from an unaligned heap_end.
static uint64_t heap_mapped = HEAP_START;

// #COMPRESPAWN: SAY WHEN THE HEAP IS GROWING WITHOUT BOUND, WHILE IT STILL CAN.
//
// MEASURED 2026-08-25 on the owner's VM <vmid> (golden 2054): the compositor took
// a page fault writing through an address 288 MB into this heap, after 8.8
// hours. Reconstructing that number needed the CR2 from a kernel exception line
// and arithmetic against HEAP_START. Nothing in the process had ever said "my
// heap is 288 MB", so the leak was invisible right up to the moment it killed
// the desktop, and the only evidence it left was the crash itself.
//
// heap_end never shrinks, so this watermark IS the leak indicator. Reported at
// 32 MB granularity, so a healthy process (the compositor sits far below the
// first threshold) never emits a line at all, and a leaking one emits at most 15
// before it hits the 512 MB ceiling. That granularity is deliberate: the report
// goes through a syscall that writes /BOOTLOG.TXT, which is real disk I/O, and
// this is called with the heap lock held. Fifteen writes over the life of a
// process that is already in trouble is a price worth paying; one per 4 KB
// would not be. 32 rather than 64 because the compositor's measured growth rate
// is ~33 MB/h, so 32 MB puts the first line inside the first hour and makes the
// LINEARITY visible within one working session, which is what distinguishes a
// leak from a one-off allocation.
static uint64_t s_heap_hw_reported_mb = 0;

static void heap_growth_milestone(void) {
    uint64_t mb = (heap_mapped - HEAP_START) >> 20;
    if (mb < s_heap_hw_reported_mb + 32) return;
    s_heap_hw_reported_mb = mb - (mb % 32);

    // No printf, no malloc: this runs inside the allocator, under its lock.
    static const char pre[]  = "libc: heap high-water is now ";
    static const char post[] = " MB of a 512 MB ceiling. heap_end never shrinks, "
                               "so this is a LEAK INDICATOR, not normal growth.";
    char msg[160];
    unsigned k = 0;
    for (unsigned i = 0; pre[i] && k < sizeof(msg) - 1; i++)  msg[k++] = pre[i];
    char num[24]; int nd = 0;
    uint64_t v = s_heap_hw_reported_mb;
    if (v == 0) num[nd++] = '0';
    while (v) { num[nd++] = (char)('0' + (v % 10)); v /= 10; }
    while (nd > 0 && k < sizeof(msg) - 1) msg[k++] = num[--nd];
    for (unsigned i = 0; post[i] && k < sizeof(msg) - 1; i++) msg[k++] = post[i];
    msg[k] = 0;
    sys_bootlog(msg);
}

// Ensure every byte in [HEAP_START, up_to) is backed by a writable page.
// Maps only the page-aligned gap (heap_mapped .. roundup(up_to)).
static int ensure_mapped(uint64_t up_to) {
    if (up_to <= heap_mapped) {
        return 0;
    }
    uint64_t need_end = (up_to + 0xFFF) & ~0xFFFULL;   // page-align up
    uint64_t map_len = need_end - heap_mapped;          // multiple of 4096
    void *mem = sys_mmap((void *)heap_mapped, map_len, 3, 0);  // PROT_READ|PROT_WRITE
    if (!mem || (uint64_t)mem == (uint64_t)-1) {
        return -1;
    }
    heap_mapped = need_end;
    heap_growth_milestone();
    return 0;
}

// --- Heap corruption diagnostics (#421 phase 7 follow-up) -----------------
//
// The threading-race fix above (the g_heap_lock mutex) did NOT stop a
// second, independent crash in this same coalesce_all() walk: a block's
// `next` pointer was found holding a non-canonical garbage value
// (0x9696040300100000-ish) even with every malloc()/free()/realloc() call
// now fully serialized. That means something is writing PAST the end of an
// allocated block's usable bytes, directly into the FOLLOWING block's
// header (size/free/next) - a real heap buffer overflow somewhere in
// AssaultCube's (or this port's shim) code, not a race. Blindly following a
// corrupted `next` pointer turns that overflow into an instant, contextless
// #GP with no indication of which allocation was clobbered. Two cheap,
// permanent, general-purpose (not AssaultCube-specific) diagnostics:
//
//  1. A redzone: every allocation gets an extra 8-byte magic canary placed
//     at the very end of its (internally rounded-up) block, i.e. the last
//     8 bytes immediately before the next block's header. Checked on every
//     free(). A mismatch reports the corrupted block's address/size/owner
//     and the garbage value actually found there, via a raw sys_write (no
//     malloc/stdio dependency, safe to call with a possibly-corrupt heap).
//  2. A bounds check before EVER dereferencing a `next` pointer in
//     coalesce_all()/find_free_block(): a `next` outside [HEAP_START,
//     heap_end) is reported and the walk stops there instead of faulting,
//     so a corruption is a diagnosable, recoverable event instead of a
//     silent process kill that erases the evidence.
#define HEAP_CANARY 0xC0DEC0DEC0DEC0DEULL

static void heap_write_str(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    sys_write(2, s, n);
}

static void heap_write_hex(uint64_t v) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int nib = (int)((v >> ((15 - i) * 4)) & 0xF);
        buf[2 + i] = (char)(nib < 10 ? ('0' + nib) : ('a' + nib - 10));
    }
    buf[18] = '\0';
    heap_write_str(buf);
}

static int block_in_range(block_header_t *b) {
    uint64_t a = (uint64_t)b;
    return a >= HEAP_START && a < heap_end;
}

// #421 phase 8 (AssaultCube port): a block's `size` field is corruption-
// prone in exactly the same way `next` already was (block_in_range() above
// only ever validated the POINTER, never the size VALUE it points at). A
// real, reproduced crash traced this: find_free_block() returned a block
// whose pointer was in-range but whose `size` had been clobbered to a huge
// garbage value; `block->size >= size` trivially passed, and
// malloc_locked()'s split path then computed a new block address and a
// canary-write offset FROM that garbage size, producing a wild address far
// outside the heap - a #GP INSIDE malloc_locked() itself (RSI=HEAP_CANARY,
// i.e. caught mid-write of the redzone). A block's data area can never
// legitimately exceed the whole heap, nor extend past the current heap
// watermark; check both before trusting `size` for any arithmetic.
static int block_size_sane(block_header_t *b) {
    if (!block_in_range(b)) return 0;
    if (b->size == 0 || b->size > HEAP_MAX_SIZE) return 0;
    uint64_t end = (uint64_t)b + BLK_HDR + b->size;
    return end <= heap_end;
}

// --- #631: bound every free-list walk ------------------------------------
//
// block_in_range()/block_size_sane() validate WHERE a `next` points and HOW
// BIG the block it points at claims to be, but neither says anything about
// the SHAPE of the chain. A single self-referential node (b->next == b, the
// natural result of a heap overflow writing one aligned word into the
// FOLLOWING block's header - exactly the corruption shape the #421 comments
// above describe) passes every existing check, so all three walks in this
// file spin forever at 100% CPU with no output at all. That is an
// unkillable, undiagnosable hang, and the worst of the three is inside
// coalesce_all(), reached from free() on EVERY free, i.e. reachable by every
// app in the tree, not just the one that first hit it. Userland is NOT on
// the kernel's concurrency-lint gate path, so no userland spin can ever fail
// a build; this class has no automated backstop, which is precisely why the
// guard belongs in the code.
//
// The bound is the live block count, maintained at the ONLY four sites that
// can change it: request_space() and malloc_locked()'s split each create a
// block; coalesce_all()'s merge and realloc()'s absorb each destroy one.
// Nothing else adds or removes a node (free() only sets ->free = 1). The
// count can therefore only ever OVER-count - a walk that aborts on
// corruption orphans blocks that stay counted - and never under-count, so a
// well-formed list can never exceed it and accounting drift alone cannot
// produce a false positive. A cycle is caught in about one list length
// rather than arena/48 steps, which matters because free() pays this cost
// on every single call.
static uint64_t g_block_count = 0;

// ============================================================================
// #245: the allocator was O(live-block-count) PER CALL, three separate ways.
//
// MEASURED, on a 1,000,090-byte HTML page in the browser (rdtsc buckets inside
// the layout walk): 6,580 elements cost 442,293 Mcyc inside libcss's style
// computation and 114,071 Mcyc inside ONE dom_element_get_attribute() per
// element - roughly 27ms and 7ms respectively for work that allocates a
// handful of small objects. Nothing in libcss or libdom is that slow. What was
// slow is that a 1MB document's DOM leaves hundreds of thousands of blocks on
// this list, and:
//
//   1. free() called coalesce_all(), which walks the WHOLE list, EVERY time;
//   2. malloc() called find_free_block(), a first-fit scan from heap_start,
//      which walks the whole list every time nothing fits (i.e. constantly
//      while a document is being built);
//   3. request_space() walked the whole list to find the tail before appending.
//
// So the browser was O(n^2) in DOM size, and every layer above it looked slow.
// This is also why removing the per-token parser logging (real, and fixed) only
// bought 12%: the logging was never the dominant term.
//
// The fix keeps the SAME data structure and every corruption guard; it only
// removes the three linear walks:
//
//   heap_last   - tail pointer, so request_space() appends in O(1). Maintained
//                 at the only sites that can unlink a node.
//   g_free_hint - an UPPER BOUND on the largest free data area. It is only ever
//                 raised outside coalesce_all(), and set to the exact maximum by
//                 a completed sweep, so it can be stale-HIGH (a wasted scan,
//                 harmless) but never stale-LOW (a missed block, which would
//                 leak). find_free_block() returns immediately when no free
//                 block can possibly be large enough.
//   g_free_cur  - next-fit cursor, so a scan that CAN succeed starts near the
//                 recently touched end of the list instead of at heap_start.
//                 The scan still wraps, so it remains complete.
//   the sweep   - free() now merges forward from the block being freed in O(1)
//                 and runs the full coalesce_all() once per HEAP_SWEEP_EVERY
//                 frees, which is what keeps backward fragmentation bounded and
//                 recomputes g_free_hint exactly. Amortised O(1).
// ============================================================================
static block_header_t *heap_last = NULL;   // tail of the block list
static block_header_t *g_free_cur = NULL;  // next-fit search cursor
static size_t   g_free_hint = 0;           // upper bound on largest free size
static uint64_t g_frees_since_sweep = 0;
#define HEAP_SWEEP_EVERY 256

// A SHORT list is walked exactly as before - every walk, every time, with every
// corruption diagnostic firing on the very call that meets the damage. That is
// deliberate and it is not a performance compromise: a walk over a few hundred
// blocks is free, and it is the regime every normal app lives in, so their
// behaviour here is bit-for-bit unchanged (userland/libc/tests/run_malloc.sh,
// the #631 cycle-detection regression test, exercises exactly this regime and
// must keep passing - it caught the first version of this patch, which had
// amortised the sweep unconditionally and thereby made a corrupt list return
// quietly instead of reporting).
//
// Only once the list is genuinely long do the walks amortise. A cycle is then
// still detected and reported, within at most HEAP_SWEEP_EVERY frees rather
// than on the first one.
#define HEAP_BIGLIST 512
static int heap_big(void) { return g_block_count > (uint64_t) HEAP_BIGLIST; }

// A node is being unlinked from the list. Keep the two cached pointers honest;
// a stale one would be a wild dereference later, which is exactly the class of
// bug the guards in this file exist for.
static void heap_note_unlinked(block_header_t *removed, block_header_t *keep) {
    if (heap_last == removed)  heap_last = keep;
    if (g_free_cur == removed) g_free_cur = keep;
}

// Rate-limited and reported through the same [malloc] channel the allocator
// already uses for an invalid free, so a corrupt heap produces a
// diagnosable event instead of drowning the log on every subsequent free().
// Same de-duplication shape as the kernel's [WQBLOCK] assert.
#define HEAP_WALK_REPORT_MAX 8
static unsigned g_walk_reports = 0;

// #631 follow-up: EVERY corruption diagnostic in this file shares one rate
// limit. The step-budget report above was rate-limited from the start, but the
// out-of-range / implausible-size / invalid-free reports were not, so a heap
// that is already damaged printed on essentially every subsequent malloc() and
// free(). Measured, not assumed: scenario 5 of
// userland/libc/tests/malloc_cycle_test.c emitted 32 lines from 2000
// allocations. On a real machine those go to a 115200-baud serial line at
// roughly a millisecond per line, from inside the allocator, so the diagnostic
// becomes the hang it was meant to explain.
//
// Deliberately a SEPARATE counter from g_walk_reports. That one is not a log
// counter: heap_walk_limit() reads it to decide which step budget to use, so
// folding unrelated reports into it would silently loosen the bound this task
// exists to tighten.
#define HEAP_DIAG_REPORT_MAX 16
static unsigned g_diag_reports = 0;

// Returns non-zero if the caller may print one corruption report. Prints the
// suppression notice exactly once, on the first call past the cap.
static int heap_diag_ok(void) {
    if (g_diag_reports > HEAP_DIAG_REPORT_MAX) return 0;
    g_diag_reports++;
    if (g_diag_reports > HEAP_DIAG_REPORT_MAX) {
        heap_write_str("[malloc] further heap corruption reports suppressed\n");
        return 0;
    }
    return 1;
}

static void heap_report_walk_overrun(const char *where, block_header_t *b,
                                     uint64_t steps) {
    // COUNT always, PRINT only up to the cap: a test (and any future
    // telemetry) can still see that the guard fired after the log has been
    // rate-limited. A counter that stops counting is a counter that lies.
    g_walk_reports++;
    if (g_walk_reports > HEAP_WALK_REPORT_MAX) return;
    heap_write_str("[malloc] heap corruption: ");
    heap_write_str(where);
    heap_write_str(" exceeded the block-list step budget (cycle or looped next chain) at block ");
    heap_write_hex((uint64_t)b);
    heap_write_str(" steps="); heap_write_hex(steps);
    heap_write_str(" blocks="); heap_write_hex(g_block_count);
    heap_write_str("\n");
    if (g_walk_reports == HEAP_WALK_REPORT_MAX) {
        heap_write_str("[malloc] further block-list walk overruns will be suppressed\n");
    }
}

static uint64_t heap_walk_limit(void) {
    uint64_t lim = g_block_count + 8;
    if (g_walk_reports) {
        // Once an overrun HAS been reported, the node accounting is itself no
        // longer trustworthy: the runaway merge that tripped the budget also
        // ran g_block_count down on its way there, which would leave every
        // later walk with an absurdly tight budget and turn one real
        // corruption into a stream of false ones. From that point on, fall
        // back to the arena's HARD physical bound - the most nodes the heap
        // could possibly hold, given the smallest block a split can leave
        // (BLK_HDR + 16). Still strictly bounded, just no longer tight, so
        // the allocator keeps working on a heap it already knows is damaged.
        uint64_t hard = ((heap_end - HEAP_START) / (BLK_HDR + 16)) + 8;
        if (lim < hard) lim = hard;
    }
    return lim;
}

// Test-only introspection for the #631 guard's proof-of-firing test
// (userland/apps/heaptest). Deliberately NOT declared in stdlib.h: it exists
// so a test can assert the guard actually fired, which is the whole point of
// a guard. Returns the number of overrun reports emitted so far.
unsigned __malloc_walk_report_count(void) { return g_walk_reports; }

static block_header_t *find_free_block(size_t size) {
    // #245: O(1) reject. g_free_hint is an upper bound on the largest free
    // block, so if it is smaller than the request no scan can succeed. This is
    // the common case while a document is being BUILT (allocate, never free),
    // and it is what turned the whole scan from per-allocation into never.
    if (heap_big() && size > g_free_hint) return NULL;

    // #245: next-fit. Start where the last successful search left off and wrap
    // once, instead of always restarting at heap_start. Still a complete scan,
    // so no reusable block is missed; it just does not re-walk the long
    // already-allocated prefix of the list on every call.
    block_header_t *start = (heap_big() && g_free_cur && block_in_range(g_free_cur))
                            ? g_free_cur : heap_start;
    block_header_t *block = start;
    int wrapped = 0;
    // #631: bounded walk - see heap_walk_limit() above.
    uint64_t steps = 0, limit = heap_walk_limit();
    while (1) {
        if (block == NULL) {
            if (wrapped || start == heap_start) break;
            wrapped = 1;
            block = heap_start;
            continue;
        }
        if (wrapped && block == start) break;
        if (++steps > limit) {
            heap_report_walk_overrun("find_free_block", block, steps);
            // Fall back to growing the heap: malloc_locked() treats NULL as
            // "no reusable block found" and calls request_space(), so the
            // allocation still succeeds instead of the app hanging.
            return NULL;
        }
        // Validate BEFORE dereferencing - block may itself be the corrupted
        // value left behind by a previous merge step, not just block->next.
        if (!block_in_range(block)) {
            if (heap_diag_ok()) {
                heap_write_str("[malloc] heap corruption: find_free_block landed on an out-of-range block pointer ");
                heap_write_hex((uint64_t)block);
                heap_write_str("\n");
            }
            return NULL;
        }
        if (block->free) {
            if (!block_size_sane(block)) {
                if (heap_diag_ok()) {
                    heap_write_str("[malloc] heap corruption: find_free_block found a block with an implausible size ");
                    heap_write_hex((uint64_t)block);
                    heap_write_str(" size="); heap_write_hex((uint64_t)block->size);
                    heap_write_str("\n");
                }
                // Do not trust this block's size for split/canary math - it
                // would compute a wild address (the exact crash this fix is
                // for). Skip it and keep walking; a well-formed block further
                // down the list (or request_space() as the final fallback)
                // still satisfies the allocation.
                block = block->next;
                continue;
            }
            if (block->size >= size) {
                g_free_cur = block;   // #245: next-fit cursor
                return block;
            }
        }
        block = block->next;
    }
    // Scanned the whole list and nothing fit, so the hint was stale-high.
    // Lower it to just under this request: still an upper bound on what is
    // actually there, and it stops the next same-sized request re-scanning.
    if (g_free_hint >= size) g_free_hint = size - 1;
    return NULL;
}

// `needed_data` is the FINAL data-area size to allocate, already 16-aligned
// and already including room for the trailing 8-byte redzone canary -
// callers (malloc_locked) compute that once so there is a single place that
// decides how much canary padding an allocation gets.
static block_header_t *request_space(size_t needed_data) {
    size_t total = BLK_HDR + needed_data;

    // Check heap limit
    if (heap_end + total > HEAP_START + HEAP_MAX_SIZE) {
        return NULL;
    }

    // Back the bytes [heap_end, heap_end+total) with writable pages. Maps whole
    // pages and only beyond the watermark, so allocations that straddle a page
    // boundary are fully mapped and existing data is never re-zeroed.
    if (ensure_mapped(heap_end + total) != 0) {
        return NULL;
    }

    block_header_t *block = (block_header_t *)heap_end;
    block->size = needed_data;
    block->free = 0;
    block->next = NULL;
    *(uint64_t *)((uint8_t *)block + BLK_HDR + needed_data - 8) = HEAP_CANARY;

    if (heap_start) {
        // #421 phase 8: this "find last block" walk was the ONE list-walk
        // loop left with ZERO validation - find_free_block()/coalesce_all()
        // already got a block_in_range() guard, but this one did not, and
        // it is reached on every allocation that does not fit an existing
        // free block (i.e. constantly, any time the heap grows). A real,
        // reproduced crash: `next` corrupted somewhere earlier in the list
        // (the still-open root cause tracked in PORT-STATUS.md/blame.md)
        // made this walk dereference a wild address with NO diagnostic at
        // all, unlike the other two walks. Guard it the same way; if the
        // walk hits a corrupted `next`, stop and link the new block onto
        // the last KNOWN-GOOD node instead of crashing. Whatever sits past
        // the corruption point becomes unreachable either way (the same
        // already-true limitation for find_free_block/coalesce_all, which
        // both already bail out at the same spot) - this fix's job is only
        // to stop a debuggable corruption from being a contextless #GP.
        // #245: O(1) append via the cached tail. The validated walk below is
        // kept as the fallback, so a heap_last that has somehow gone stale or
        // out of range degrades to the old (correct) behaviour rather than
        // corrupting the list.
        block_header_t *last = heap_start;
        if (heap_big() && heap_last && block_in_range(heap_last) &&
            heap_last->next == NULL) {
            heap_last->next = block;
            heap_last = block;
            heap_end += total;
            g_block_count++;
            return block;
        }
        // #631: bounded walk - see heap_walk_limit() above.
        uint64_t steps = 0, limit = heap_walk_limit();
        while (last->next) {
            if (++steps > limit) {
                heap_report_walk_overrun("request_space tail walk", last, steps);
                break;
            }
            if (!block_in_range(last->next)) {
                if (heap_diag_ok()) {
                    heap_write_str("[malloc] heap corruption: request_space's tail walk hit an out-of-range next pointer ");
                    heap_write_hex((uint64_t)last->next);
                    heap_write_str(" from block "); heap_write_hex((uint64_t)last);
                    heap_write_str("\n");
                }
                break;
            }
            last = last->next;
        }
        last->next = block;
        heap_last = block;
    } else {
        heap_start = block;
        heap_last = block;
    }

    heap_end += total;
    g_block_count++;          // #631: one more node on the list
    return block;
}

// Verify a block's trailing redzone canary is intact. Called before a block
// is freed/coalesced; a mismatch means something wrote past the end of the
// bytes malloc() actually handed back for this block. Reports and continues
// (does not crash) so the caller can decide what to do.
static void check_canary(block_header_t *b) {
    // #421 phase 8: also reject an implausibly-LARGE size, not just size<8 -
    // a huge corrupted size passed the old check (it is not < 8) and then
    // read the redzone at a wild offset. See block_size_sane() above.
    if (!block_in_range(b) || b->size < 8 || !block_size_sane(b)) return;
    uint64_t got = *(uint64_t *)((uint8_t *)b + BLK_HDR + b->size - 8);
    if (got != HEAP_CANARY) {
        if (heap_diag_ok()) {
            heap_write_str("[malloc] HEAP OVERFLOW detected: block ");
            heap_write_hex((uint64_t)b);
            heap_write_str(" size="); heap_write_hex((uint64_t)b->size);
            heap_write_str(" redzone="); heap_write_hex(got);
            heap_write_str(" expected="); heap_write_hex(HEAP_CANARY);
            heap_write_str("\n");
        }
    }
}

// Merge every run of adjacent free blocks in the list. Cheap O(n) sweep run on
// free() so fragmentation from a stream of alloc/free (very common in CPython)
// gets reclaimed instead of only coalescing the single following block.
static void coalesce_all(void) {
    block_header_t *b = heap_start;
    // #631: one step budget shared by BOTH loops. Every iteration of either
    // loop either advances past a node that is never revisited (outer) or
    // removes a node from the list (inner merge), so a well-formed list
    // costs at most g_block_count steps in total.
    uint64_t steps = 0, limit = heap_walk_limit();
    size_t maxfree = 0;   // #245: exact largest free data area, this sweep
    while (b) {
        if (++steps > limit) {
            heap_report_walk_overrun("coalesce_all", b, steps);
            return;
        }
        // Validate BEFORE dereferencing - b may itself be the garbage value
        // a previous `b->next = b->next->next` merge step left behind
        // (its OWN validity is only proven by the block_in_range() check
        // inside the merge loop's condition below, one step removed from
        // where it gets assigned). Checking only b->next (as an earlier
        // version of this function did) misses exactly that case.
        if (!block_in_range(b)) {
            if (heap_diag_ok()) {
                heap_write_str("[malloc] heap corruption: coalesce_all landed on an out-of-range block pointer ");
                heap_write_hex((uint64_t)b);
                heap_write_str("\n");
            }
            return;
        }
        if (b->free) {
            // #421 phase 8: also require the neighbor's size to be sane
            // before folding it into b->size - absorbing a corrupted size
            // here would silently propagate the garbage into b itself,
            // turning b into the next call's implausible-size victim
            // instead of stopping the corruption where it is first seen.
            while (b->next && block_in_range(b->next) && b->next->free && block_size_sane(b->next)) {
                if (++steps > limit) {
                    heap_report_walk_overrun("coalesce_all merge", b, steps);
                    return;
                }
                heap_note_unlinked(b->next, b);       // #245
                b->size += BLK_HDR + b->next->size;
                b->next = b->next->next;
                if (g_block_count) g_block_count--;   // #631: node removed
            }
            if (b->size > maxfree) maxfree = b->size;   // #245
        }
        if (b->next == NULL) heap_last = b;             // #245
        b = b->next;
    }
    // #245: a COMPLETED sweep is the only place that knows the exact largest
    // free block, so it is the only place allowed to LOWER the hint. Every
    // early return above leaves the previous (conservatively high) value.
    g_free_hint = maxfree;
    g_free_cur = NULL;
}

// Internal, NON-locking bodies. Callers must hold g_heap_lock.
static void *malloc_locked(size_t size) {
    if (size == 0) return NULL;

    // Align the caller-visible minimum size, then reserve 8 more bytes for
    // the trailing redzone canary (also 16-aligned). `needed` is the ONE
    // place that decides how much data-area a block actually gets; both
    // find_free_block() and request_space() are handed this final value so
    // the canary's position (block->size - 8) is always past every byte the
    // caller is entitled to write.
    size = (size + 15) & ~15;
    size_t needed = (size + 8 + 15) & ~15;

    // Try to find a free block
    block_header_t *block = find_free_block(needed);

    if (block) {
        // Split block if the remainder can hold a header plus a useful chunk.
        if (block->size >= needed + BLK_HDR + 16) {
            block_header_t *new_block = (block_header_t *)((uint8_t *)block +
                                        BLK_HDR + needed);
            new_block->size = block->size - needed - BLK_HDR;
            new_block->free = 1;
            new_block->next = block->next;
            *(uint64_t *)((uint8_t *)new_block + BLK_HDR + new_block->size - 8) = HEAP_CANARY;

            if (heap_last == block) heap_last = new_block;   // #245
            if (new_block->size > g_free_hint)                // #245
                g_free_hint = new_block->size;
            block->size = needed;
            block->next = new_block;
            g_block_count++;  // #631: the split created a node
        }
        block->free = 0;
        // Re-arm the canary at this block's (possibly unchanged) size - cheap
        // and keeps a reused free block's redzone valid even if a previous
        // occupant's canary check already fired to report a corruption.
        *(uint64_t *)((uint8_t *)block + BLK_HDR + block->size - 8) = HEAP_CANARY;
    } else {
        // Request new space
        block = request_space(needed);
        if (!block) return NULL;
    }

    return (void *)((uint8_t *)block + BLK_HDR);
}

static void free_locked(void *ptr) {
    if (!ptr) return;

    block_header_t *block = (block_header_t *)((uint8_t *)ptr - BLK_HDR);
    // #421 phase 8: check_canary() already validates block_in_range(block)
    // internally and safely no-ops if it is out of range, but that does NOT
    // stop the code below from unconditionally writing block->free = 1 on
    // a block pointer that was never valid in the first place - a real,
    // reproduced #GP: some caller (e.g. libc_gap.cpp's fclose() shim, via
    // AssaultCube's own crash-handler cleanup path passing a bogus/
    // uninitialized FILE*) called free() with a pointer that does not point
    // at a real allocation at all, and this function crashed trying to mark
    // it free anyway. A NULL check is not enough - a non-NULL garbage
    // pointer is just as unsafe. Validate BEFORE the write, not just before
    // the diagnostic read.
    if (!block_in_range(block)) {
        if (heap_diag_ok()) {
            heap_write_str("[malloc] heap corruption: free() called with a pointer that does not resolve to a valid block ");
            heap_write_hex((uint64_t)ptr);
            heap_write_str("\n");
        }
        return;
    }
    check_canary(block);
    block->free = 1;

    // #245: a short list keeps the original behaviour EXACTLY - full sweep on
    // every free, every diagnostic firing on the offending call.
    if (!heap_big()) {
        coalesce_all();
        return;
    }

    // #245: merge forward from THIS block only - O(1) in the common case,
    // bounded by the same step budget as every other walk in this file. The
    // full sweep below is what still reclaims a free block that PRECEDES this
    // one; running it on every free() was the single most expensive thing the
    // browser did (see the block comment at g_block_count).
    {
        uint64_t steps = 0, limit = heap_walk_limit();
        while (block->next && block_in_range(block->next) &&
               block->next->free && block_size_sane(block->next)) {
            if (++steps > limit) {
                heap_report_walk_overrun("free forward merge", block, steps);
                break;
            }
            block_header_t *victim = block->next;
            heap_note_unlinked(victim, block);
            block->size += BLK_HDR + victim->size;
            block->next = victim->next;
            if (g_block_count) g_block_count--;
        }
    }
    if (block->size > g_free_hint) g_free_hint = block->size;
    if (block->next == NULL) heap_last = block;
    // Reuse this block next: it is warm, and it stops the cursor drifting to
    // the far end of the list.
    g_free_cur = block;

    if (++g_frees_since_sweep >= HEAP_SWEEP_EVERY) {
        g_frees_since_sweep = 0;
        coalesce_all();
    }
}

// #613: high-water mark of the heap arena, in bytes. heap_end only ever grows
// (request_space bumps it; free() never gives pages back), so this is exactly
// the peak memory this process has taken for its heap. Cheap, always-on, and
// the honest way to ANSWER "is this algorithm's memory bounded?" with a
// measured number instead of an assertion - which is why it lives in the shared
// libc rather than being re-derived per app.
size_t malloc_heap_highwater(void) {
    return (size_t)(heap_end - HEAP_START);
}

void *malloc(size_t size) {
    pthread_mutex_lock(&g_heap_lock);
    void *p = malloc_locked(size);
    pthread_mutex_unlock(&g_heap_lock);
    return p;
}

void free(void *ptr) {
    if (!ptr) return;
    pthread_mutex_lock(&g_heap_lock);
    free_locked(ptr);
    pthread_mutex_unlock(&g_heap_lock);
}

void *calloc(size_t nmemb, size_t size) {
    // Guard the multiply against overflow (CVE-class bug in naive callocs).
    if (nmemb && size > (size_t)-1 / nmemb) return NULL;
    size_t total = nmemb * size;
    void *ptr = malloc(total);   // locks internally
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    pthread_mutex_lock(&g_heap_lock);

    block_header_t *block = (block_header_t *)((uint8_t *)ptr - BLK_HDR);
    // #421 phase 8: same class of bug as free_locked() above - check_canary()
    // safely no-ops on an out-of-range block, but the code below still went
    // on to READ block->size unconditionally. Validate first; a `ptr` that
    // does not resolve to a real allocation cannot be honored, so report and
    // fail closed (NULL) rather than crash - the caller already has to
    // handle realloc() failure per its normal contract.
    if (!block_in_range(block)) {
        if (heap_diag_ok()) {
            heap_write_str("[malloc] heap corruption: realloc() called with a pointer that does not resolve to a valid block ");
            heap_write_hex((uint64_t)ptr);
            heap_write_str("\n");
        }
        pthread_mutex_unlock(&g_heap_lock);
        return NULL;
    }
    check_canary(block);
    size_t asize = (size + 15) & ~15;
    size_t needed = (asize + 8 + 15) & ~15;   // + redzone, see malloc_locked

    if (block->size >= needed) {
        pthread_mutex_unlock(&g_heap_lock);
        return ptr;  // Already big enough (including room for the canary)
    }

    // Try to grow in place by absorbing an adjacent free block.
    // #421 phase 8: same block_size_sane() guard as find_free_block/
    // coalesce_all - do not fold a corrupted neighbor size into `block`.
    if (block->next && block_in_range(block->next) && block->next->free &&
        block_size_sane(block->next) &&
        block->size + BLK_HDR + block->next->size >= needed) {
        heap_note_unlinked(block->next, block);       // #245
        block->size += BLK_HDR + block->next->size;
        block->next = block->next->next;
        if (block->next == NULL) heap_last = block;   // #245
        if (g_block_count) g_block_count--;           // #631: node removed
        *(uint64_t *)((uint8_t *)block + BLK_HDR + block->size - 8) = HEAP_CANARY;
        pthread_mutex_unlock(&g_heap_lock);
        return ptr;
    }

    // Allocate new block and copy the old contents. Use the *_locked()
    // helpers directly (the lock is already held; pthread_mutex_t here is
    // non-recursive, so calling the public malloc()/free() would deadlock).
    void *new_ptr = malloc_locked(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size);
        free_locked(ptr);
    }
    pthread_mutex_unlock(&g_heap_lock);
    return new_ptr;
}

// ============================================================================
// Process Control
// ============================================================================

// AssaultCube port phase 3 (docs/ASSAULTCUBE_PORT_PLAN.md): real atexit(),
// a genuinely missing, generally useful libc primitive (not app-specific),
// found because server.cpp calls it directly (not via a C++ static
// destructor, which already goes through cxxsupp.cpp's separate
// __cxa_atexit no-op). A small fixed table is plenty: this is the same
// scale every other libc uses it at (a handful of process-lifetime cleanup
// hooks), not a hot path.
#define MOS_ATEXIT_MAX 32
static void (*atexit_fns[MOS_ATEXIT_MAX])(void);
static int atexit_count = 0;

int atexit(void (*func)(void)) {
    if (!func || atexit_count >= MOS_ATEXIT_MAX) return -1;
    atexit_fns[atexit_count++] = func;
    return 0;
}

// #745: printf()/putchar() now buffer through stdout (see stdio.c/stdio_file.c)
// when stdout is not a real terminal, so a process that calls exit() directly
// (rather than returning from main(), which crt0.S already routes through
// __libc_fini() -> fflush(0)) could otherwise lose whatever was written since
// the last '\n' or the last full buffer. Real C libraries flush all open
// streams as part of exit(); do the same here so this is a property of exit()
// itself, not something every caller must remember. Run AFTER the atexit
// hooks (their own output needs flushing too) and BEFORE the syscall, so
// nothing buffered here can be lost to the process actually going away.
// abort() deliberately does NOT flush (see stdlib.c's abort(), a few lines
// down): the codebase's own crash-diagnostic convention is to write directly
// to fd 2 (unbuffered stderr, or a raw write()), never to rely on a flush
// happening after something has already gone wrong enough to abort().
void exit(int status) {
    while (atexit_count > 0) atexit_fns[--atexit_count]();
    fflush(NULL);
    sys_exit(status);
    __builtin_unreachable();
}

// _exit is defined in crt0.S - do not define it here

void abort(void) {
    // #307: abort() and __stack_chk_fail() both exit 127, and so does every
    // Rust panic_handler in the tree (they all call this function). On a
    // machine with no serial port that number was the ONLY evidence, which is
    // how the owner's iMac produced two identical "COMPOSIT exiting, code 127"
    // lines that could not be told apart. Stamp which one this was.
    //
    // Deliberately NOT a flush and NOT a printf: abort()'s contract in this
    // tree is to write directly and go (see exit() above for why). This is one
    // syscall over a string constant.
    (void)sys_bootlog("libc: abort() called (assert failure or Rust panic); "
                      "process terminating with exit 127");
    sys_exit(127);
    __builtin_unreachable();
}

// ============================================================================
// String Conversion
// ============================================================================

int atoi(const char *str) {
    int result = 0;
    int sign = 1;
    
    while (*str == ' ' || *str == '\t') str++;
    
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return sign * result;
}

long atol(const char *str) {
    long result = 0;
    int sign = 1;
    
    while (*str == ' ' || *str == '\t') str++;
    
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return sign * result;
}

// ============================================================================
// Random Numbers
// ============================================================================

static unsigned int rand_seed = 1;

int rand(void) {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (rand_seed / 65536) % (RAND_MAX + 1);
}

void srand(unsigned int seed) {
    rand_seed = seed;
}

// ============================================================================
// Absolute Value
// ============================================================================

int abs(int n) {
    return (n < 0) ? -n : n;
}

long labs(long n) {
    return (n < 0) ? -n : n;
}

// File I/O wrappers
int open(const char *path, int flags, ...) {
    /* #359 Phase 2: propagate the kernel's negative errno so callers (and
       CPython's import machinery) see ENOENT/EACCES instead of a bare -1.
       The optional POSIX mode argument is accepted for 3-arg compatibility;
       the kernel sys_open is 2-arg so it is not forwarded. */
    int r = sys_open(path, flags);
    if (r < 0) { errno = -r; return -1; }
    return r;
}

int close(int fd) {
    return sys_close(fd);
}

// #695: see the contract in stdlib.h. errno is set from the kernel's negative
// return so a caller can distinguish "the disk is full" (retry after freeing
// space, on THIS SAME still-open fd) from "the medium failed" (retrying will
// not help).
int fsync(int fd) {
    int r = sys_fsync(fd);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

// #227: a negative kernel return here used to reach the caller UNCHANGED
// (neither normalised to -1 nor translated into errno), unlike open()/fsync()
// a few lines above, which already do the right thing. That silently broke
// the POSIX EINTR-retry contract for every caller in the tree: a blocking
// read() interrupted by a signal (drivers/tty.c's tty_read() returns -4, i.e.
// -EINTR, straight out of wait_event_interruptible on WAIT_EINTR) came back
// here as the raw value -4, with errno left at whatever it was before the
// call. A well-behaved POSIX caller that checks `errno == EINTR` to decide
// whether to retry never sees EINTR, because nothing ever wrote it, so an
// ordinary interrupted read reads exactly like a hard I/O error.
//
// This is bug (a) of #227's two-bugs-stacked report, PROVEN by a headless
// pty harness (userland/apps/winchprb): resizing the Terminal window while
// `vi` runs raises SIGWINCH at vi's foreground process group (#220's fix,
// completely correct and NOT touched here); vi has
// ENABLE_FEATURE_VI_USE_SIGNALS=0 (compat/libbb.h) so it installs no signal
// handler at all - the signal's only effect is that sig_raise() unblocks
// vi's in-flight blocking read() on stdin via wake_up_process(), exactly as
// #220 intended. vi's own readit() (vendor/busybox/vi.c:1122) already does
// the correct thing for that: safe_read_key() retries on `errno == EINTR`.
// But because read() below never set errno, retry never triggered: readit()
// saw an unrecognised failure, called cookmode() and
// bb_simple_error_msg_and_die("can't read user input") -> _exit(1). The
// probe's kernel log confirms this precisely: vi's own zombie exit code was
// 1 (a normal, voluntary _exit(1) from vi.c/libbb_compat.c), with NO
// [EXCEPTION]/[#PF]/[CrashHandler] line anywhere near it - not a fault, not
// an async-signal-unsafe handler (there is no handler to be unsafe), and not
// a kernel signal-return-path bug. The kernel's SIGWINCH delivery is exactly
// right; this libc function broke the contract underneath it.
//
// Every read()/write() caller in the tree (audited before this change) only
// ever tests the return value's SIGN (<0, ==0, >0, != count), never a
// specific negative magnitude, so normalising to -1 changes no caller's
// control flow; it only makes errno finally carry real information.
long read(int fd, void *buf, size_t count) {
    long r = sys_read(fd, buf, count);
    if (r < 0) { errno = (int)-r; return -1; }
    return r;
}

long write(int fd, const void *buf, size_t count) {
    long r = sys_write(fd, buf, count);
    if (r < 0) { errno = (int)-r; return -1; }
    return r;
}

// Time functions
long clock(void) {
    return sys_clock();
}

// ============================================================================
// Environment
// ============================================================================
// #112: THE ENVIRONMENT IS NOW INHERITED. This comment used to read "crt0 does
// not pass envp, so the process starts with an empty environment", and that was
// true of every process on the system: the kernel's setup_user_argv() wrote a
// NULL where envp should be and crt0 never looked at it.
//
// crt0.S now computes envp from the initial stack and calls __libc_init_env()
// below before main(). environ is a NULL-terminated array of "NAME=VALUE"
// strings, grown on demand; setenv/putenv malloc their entries, while the
// INHERITED entries are pointers into the initial stack, which is mapped for
// the life of the process and never freed. Both kinds therefore stay valid for
// as long as environ does, and neither is ever freed, which is why unsetenv()
// only unlinks.

char **environ = NULL;
static int env_count = 0;
static int env_cap = 0;

static int env_find(const char *name, size_t nlen) {
    if (!environ) return -1;
    for (int i = 0; i < env_count; i++) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=')
            return i;
    }
    return -1;
}

char *getenv(const char *name) {
    if (!name || !*name) return NULL;
    size_t nlen = strlen(name);
    int i = env_find(name, nlen);
    if (i < 0) return NULL;
    return environ[i] + nlen + 1;
}

static int env_grow(void) {
    if (env_count + 1 < env_cap) return 0;
    int ncap = env_cap ? env_cap * 2 : 8;
    char **na = (char **)malloc((size_t)ncap * sizeof(char *));
    if (!na) return -1;
    for (int i = 0; i < env_count; i++) na[i] = environ[i];
    na[env_count] = NULL;
    // old array is intentionally leaked (small, avoids freeing putenv strings)
    environ = na;
    env_cap = ncap;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) { errno_set(EINVAL); return -1; }
    if (!value) value = "";
    size_t nlen = strlen(name);
    int i = env_find(name, nlen);
    if (i >= 0 && !overwrite) return 0;
    size_t vlen = strlen(value);
    char *entry = (char *)malloc(nlen + vlen + 2);
    if (!entry) { errno_set(ENOMEM); return -1; }
    memcpy(entry, name, nlen);
    entry[nlen] = '=';
    memcpy(entry + nlen + 1, value, vlen);
    entry[nlen + 1 + vlen] = '\0';
    if (i >= 0) {
        environ[i] = entry;         // old entry leaked (may be a putenv string)
    } else {
        if (env_grow() < 0) { free(entry); errno_set(ENOMEM); return -1; }
        environ[env_count++] = entry;
        environ[env_count] = NULL;
    }
    return 0;
}

int unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) { errno_set(EINVAL); return -1; }
    size_t nlen = strlen(name);
    int i = env_find(name, nlen);
    if (i < 0) return 0;
    for (int j = i; j < env_count; j++) environ[j] = environ[j + 1];
    env_count--;
    return 0;
}

// ---------------------------------------------------------------------------
// #112: THE ONE PLACE THAT ATTACHES `environ` TO A SPAWN.
//
// sys_spawn_args() and sys_spawn_redir() in syscall.h are inline wrappers that
// something like a hundred call sites across the tree use directly. Teaching
// each of them to build an sc_spawn_req_t would be a hundred chances to get it
// wrong and a hundred places to forget. Instead both inlines now route here,
// so a child inherits its parent's environment WITHOUT its parent being
// changed at all. That is what made msh's `export`, the terminal's PATH/HOME
// and every ported tool's getenv() start working in one change.
//
// It lives in stdlib.c because that is where `environ` lives; putting it in
// syscall.h would make a header depend on a symbol that only this file
// defines.
//
// environ == NULL (nothing was ever inherited or set) sends envc = -1, which
// the kernel answers with its default block rather than with an empty
// environment. See sc_spawn_req_t in kernel/proc/syscall.h.
int __spawn_with_env(const char *path, char **argv, int argc,
                     const char *infile, const char *outfile, int append) {
    sc_spawn_req_t req;
    int n = -1;
    if (environ) {
        n = 0;
        while (environ[n] && n < 64) n++;
    }
    req.path     = path;
    req.argv     = argv;
    req.argc     = argc;
    req.envc     = n;
    req.envp     = environ;
    req.infile   = infile;
    req.outfile  = outfile;
    req.append   = append;
    req.reserved = 0;
    return sys_spawn_env(&req);
}

// Called by crt0.S with the envp vector the kernel left on the initial stack,
// before main(). Stores the pointers as they are: the strings are on that
// stack and outlive every use of them.
//
// A malformed entry is SKIPPED here rather than refused, and this is the one
// place in the #112 chain that is permissive on purpose. The kernel already
// refuses to build a block containing an entry with no '=' (see
// rustkern/envblock.rs), so an entry that reaches this point without one came
// from a kernel that predates that rule or from a hand-built stack; aborting a
// process's startup over it would be a worse answer than ignoring it.
void __libc_init_env(char **envp) {
    if (!envp) return;
    for (int i = 0; envp[i]; i++) {
        if (!strchr(envp[i], '=')) continue;
        if (env_grow() < 0) return;
        environ[env_count++] = envp[i];
        environ[env_count] = NULL;
    }
}

int putenv(char *string) {
    // string must contain '='; the pointer becomes part of the environment.
    if (!string) { errno_set(EINVAL); return -1; }
    char *eq = strchr(string, '=');
    if (!eq) return unsetenv(string);
    size_t nlen = (size_t)(eq - string);
    int i = env_find(string, nlen);
    if (i >= 0) { environ[i] = string; return 0; }
    if (env_grow() < 0) { errno_set(ENOMEM); return -1; }
    environ[env_count++] = string;
    environ[env_count] = NULL;
    return 0;
}

// ============================================================================
// Sorting
// ============================================================================
// Median-of-three quicksort with an insertion-sort cutoff, iterative fallback
// unnecessary at these sizes. Matches C89 qsort() contract.

static void qs_swap(char *a, char *b, size_t size) {
    while (size--) { char t = *a; *a++ = *b; *b++ = t; }
}

static void qsort_r_impl(char *base, size_t n, size_t size,
                         int (*cmp)(const void *, const void *)) {
    while (n > 12) {
        char *lo = base;
        char *hi = base + (n - 1) * size;
        char *mid = base + (n / 2) * size;
        // median-of-three -> mid
        if (cmp(mid, lo) < 0) qs_swap(mid, lo, size);
        if (cmp(hi, lo) < 0) qs_swap(hi, lo, size);
        if (cmp(hi, mid) < 0) qs_swap(hi, mid, size);
        // partition around pivot value at mid
        qs_swap(mid, hi - size, size);     // stash pivot at hi-1
        char *pivot = hi - size;
        char *i = lo;
        char *j = hi - size;
        for (;;) {
            do { i += size; } while (cmp(i, pivot) < 0);
            do { j -= size; } while (cmp(pivot, j) < 0);
            if (i >= j) break;
            qs_swap(i, j, size);
        }
        qs_swap(i, hi - size, size);       // restore pivot
        // recurse into the smaller side, loop on the larger (bounded stack)
        size_t left_n = (size_t)(i - lo) / size;
        size_t right_n = n - left_n - 1;
        if (left_n < right_n) {
            qsort_r_impl(lo, left_n, size, cmp);
            base = i + size;
            n = right_n;
        } else {
            qsort_r_impl(i + size, right_n, size, cmp);
            n = left_n;
        }
    }
    // insertion sort for the small tail
    for (size_t a = 1; a < n; a++) {
        for (char *p = base + a * size; p > base && cmp(p - size, p) > 0; p -= size)
            qs_swap(p - size, p, size);
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb < 2 || size == 0) return;
    qsort_r_impl((char *)base, nmemb, size, compar);
}

// ============================================================================
// Wide integer conversion / absolute value
// ============================================================================

long long llabs(long long n) { return (n < 0) ? -n : n; }

long long strtoll(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    while (*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\f'||*s=='\v') s++;
    int neg = 0;
    if (*s=='+') s++; else if (*s=='-') { neg=1; s++; }
    if ((base==0||base==16) && s[0]=='0' && (s[1]=='x'||s[1]=='X')) { s+=2; base=16; }
    else if (base==0 && s[0]=='0') base=8;
    else if (base==0) base=10;
    long long acc=0; int any=0;
    for (;;) {
        int c=(unsigned char)*s, d;
        if (c>='0'&&c<='9') d=c-'0';
        else if (c>='a'&&c<='z') d=c-'a'+10;
        else if (c>='A'&&c<='Z') d=c-'A'+10;
        else break;
        if (d>=base) break;
        acc = acc*base + d; any=1; s++;
    }
    if (endptr) *endptr=(char*)(any?s:nptr);
    return neg ? -acc : acc;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    while (*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\f'||*s=='\v') s++;
    int neg = 0;
    if (*s=='+') s++; else if (*s=='-') { neg=1; s++; }
    if ((base==0||base==16) && s[0]=='0' && (s[1]=='x'||s[1]=='X')) { s+=2; base=16; }
    else if (base==0 && s[0]=='0') base=8;
    else if (base==0) base=10;
    unsigned long long acc=0; int any=0;
    for (;;) {
        int c=(unsigned char)*s, d;
        if (c>='0'&&c<='9') d=c-'0';
        else if (c>='a'&&c<='z') d=c-'a'+10;
        else if (c>='A'&&c<='Z') d=c-'A'+10;
        else break;
        if (d>=base) break;
        acc = acc*(unsigned long long)base + (unsigned long long)d; any=1; s++;
    }
    if (endptr) *endptr=(char*)(any?s:nptr);
    return neg ? (unsigned long long)(-(long long)acc) : acc;
}

// ============================================================================
// Floating point parsing (strtod / strtof / atof)
// ============================================================================
// Decimal and hex-float parsing with correct end-pointer semantics. Uses
// power-of-ten scaling: accurate to ~1 ULP for typical inputs, which is all a
// C-side parser needs (CPython parses its own float literals via _Py_dg_strtod).

// 10^0 .. 10^22 are ALL exactly representable as IEEE doubles.
static const double p10_exact[23] = {
    1e0,1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,1e11,
    1e12,1e13,1e14,1e15,1e16,1e17,1e18,1e19,1e20,1e21,1e22
};

// Scale mant by 10^e. When the mantissa was accumulated exactly (<= 15 decimal
// digits, so it fits in the 53-bit significand) and |e| <= 22, a single
// multiply/divide by an exact power of ten yields the correctly-rounded result
// (Clinger's fast path). Larger exponents chunk through 10^22 (a few ULP);
// truly extreme/subnormal magnitudes fall back to repeated squaring. This
// matches glibc for typical inputs; CPython parses its own literals via dtoa.
static double pow10_scale_digits(double x, int e, int ndig) {
    if (ndig <= 15) {
        while (e > 22)  { x *= 1e22; e -= 22; }
        while (e < -22) { x /= 1e22; e += 22; }
        if (e >= 0) return x * p10_exact[e];
        return x / p10_exact[-e];
    }
    double base = (e < 0) ? 0.1 : 10.0;
    int n = (e < 0) ? -e : e;
    while (n) {
        if (n & 1) x *= base;
        base *= base;
        n >>= 1;
    }
    return x;
}

double strtod(const char *nptr, char **endptr) {
    const char *s = nptr;
    while (*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\f'||*s=='\v') s++;

    int neg = 0;
    if (*s=='+') s++; else if (*s=='-') { neg=1; s++; }

    // inf / nan
    if ((s[0]=='i'||s[0]=='I') && (s[1]=='n'||s[1]=='N') && (s[2]=='f'||s[2]=='F')) {
        s += 3;
        if ((s[0]=='i'||s[0]=='I')&&(s[1]=='n'||s[1]=='N')&&(s[2]=='i'||s[2]=='I')&&
            (s[3]=='t'||s[3]=='T')&&(s[4]=='y'||s[4]=='Y')) s += 5;
        if (endptr) *endptr=(char*)s;
        double inf = 1e308*10.0;
        return neg ? -inf : inf;
    }
    if ((s[0]=='n'||s[0]=='N') && (s[1]=='a'||s[1]=='A') && (s[2]=='n'||s[2]=='N')) {
        s += 3;
        if (endptr) *endptr=(char*)s;
        double z = 0.0;
        return z/z; // NaN
    }

    // hex float 0x...
    if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) {
        const char *hs = s + 2;
        double val = 0.0; int anydig = 0; int bexp = 0;
        for (; ; hs++) {
            int c=(unsigned char)*hs, d;
            if (c>='0'&&c<='9') d=c-'0';
            else if (c>='a'&&c<='f') d=c-'a'+10;
            else if (c>='A'&&c<='F') d=c-'A'+10;
            else break;
            val = val*16.0 + d; anydig=1;
        }
        if (*hs=='.') {
            hs++;
            for (; ; hs++) {
                int c=(unsigned char)*hs, d;
                if (c>='0'&&c<='9') d=c-'0';
                else if (c>='a'&&c<='f') d=c-'a'+10;
                else if (c>='A'&&c<='F') d=c-'A'+10;
                else break;
                val = val*16.0 + d; bexp -= 4; anydig=1;
            }
        }
        if (!anydig) { if (endptr) *endptr=(char*)nptr; return 0.0; }
        if (*hs=='p'||*hs=='P') {
            const char *es=hs+1; int esign=0;
            if (*es=='+') es++; else if (*es=='-'){esign=1;es++;}
            int e=0, ed=0;
            while (*es>='0'&&*es<='9'){ e=e*10+(*es-'0'); es++; ed=1; }
            if (ed){ bexp += esign?-e:e; hs=es; }
        }
        // scale by 2^bexp
        double p2 = 1.0; int be = bexp<0?-bexp:bexp; double b=2.0;
        while (be){ if(be&1)p2*=b; b*=b; be>>=1; }
        val = bexp<0 ? val/p2 : val*p2;
        if (endptr) *endptr=(char*)hs;
        return neg?-val:val;
    }

    // decimal
    double mant = 0.0;
    int anydig = 0;
    int exp10 = 0;
    int ndig = 0;   // digits accumulated into mant (exactness budget)
    while (*s>='0'&&*s<='9') { mant = mant*10.0 + (*s-'0'); s++; anydig=1; ndig++; }
    if (*s=='.') {
        s++;
        while (*s>='0'&&*s<='9') { mant = mant*10.0 + (*s-'0'); exp10--; s++; anydig=1; ndig++; }
    }
    if (!anydig) { if (endptr) *endptr=(char*)nptr; return 0.0; }
    if (*s=='e'||*s=='E') {
        const char *es=s+1; int esign=0;
        if (*es=='+') es++; else if (*es=='-'){esign=1;es++;}
        int e=0, ed=0;
        while (*es>='0'&&*es<='9'){ e=e*10+(*es-'0'); es++; ed=1; }
        if (ed){ exp10 += esign?-e:e; s=es; }
    }
    double val = pow10_scale_digits(mant, exp10, ndig);
    if (endptr) *endptr=(char*)s;
    return neg?-val:val;
}

float strtof(const char *nptr, char **endptr) {
    return (float)strtod(nptr, endptr);
}

double atof(const char *str) {
    return strtod(str, (char **)0);
}


// ===========================================================================
// strtol / strtoul / bsearch  (added for the NetSurf browser engine port #245)
// ===========================================================================
unsigned long strtoul(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    while (*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\f'||*s=='\v') s++;
    int neg = 0;
    if (*s=='+') s++; else if (*s=='-') { neg=1; s++; }
    if ((base==0||base==16) && s[0]=='0' && (s[1]=='x'||s[1]=='X')) { s+=2; base=16; }
    else if (base==0 && s[0]=='0') base=8;
    else if (base==0) base=10;
    unsigned long acc=0; int any=0;
    for (;;) {
        int c=(unsigned char)*s, d;
        if (c>='0'&&c<='9') d=c-'0';
        else if (c>='a'&&c<='z') d=c-'a'+10;
        else if (c>='A'&&c<='Z') d=c-'A'+10;
        else break;
        if (d>=base) break;
        acc = acc*(unsigned long)base + (unsigned long)d; any=1; s++;
    }
    if (endptr) *endptr=(char*)(any?s:nptr);
    return neg ? (unsigned long)(-(long)acc) : acc;
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    while (*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\f'||*s=='\v') s++;
    int neg = 0;
    if (*s=='+') s++; else if (*s=='-') { neg=1; s++; }
    if ((base==0||base==16) && s[0]=='0' && (s[1]=='x'||s[1]=='X')) { s+=2; base=16; }
    else if (base==0 && s[0]=='0') base=8;
    else if (base==0) base=10;
    long acc=0; int any=0;
    for (;;) {
        int c=(unsigned char)*s, d;
        if (c>='0'&&c<='9') d=c-'0';
        else if (c>='a'&&c<='z') d=c-'a'+10;
        else if (c>='A'&&c<='Z') d=c-'A'+10;
        else break;
        if (d>=base) break;
        acc = acc*base + d; any=1; s++;
    }
    if (endptr) *endptr=(char*)(any?s:nptr);
    return neg ? -acc : acc;
}

void *bsearch(const void *key, const void *base, unsigned long nmemb,
              unsigned long size, int (*compar)(const void *, const void *)) {
    unsigned long lo=0, hi=nmemb;
    const char *b=(const char*)base;
    while (lo<hi) {
        unsigned long mid = lo + (hi-lo)/2;
        const void *p = b + mid*size;
        int r = compar(key, p);
        if (r<0) hi=mid;
        else if (r>0) lo=mid+1;
        else return (void*)p;
    }
    return (void*)0;
}
