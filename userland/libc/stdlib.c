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
#include "fcntl.h"   // O_RDWR/O_CREAT/O_EXCL for mkstemp (#745)
#include "time.h"    // time() seed for mkstemp (#745)

// #portstack: these are provided by libc for general use, but a port that
// ships its own (e.g. the CPython wsupp compat layer) must win at its own
// link. Marking them weak lets a strong app/port definition override, while
// libc still serves every app that has none. Fixes CPython relink dup-symbols.
#pragma weak strtoimax
#pragma weak strtoumax

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
    struct block_header *next;      // Next block in list (address order)
    // #245 domscript: PREVIOUS block in address order. This costs NOTHING:
    // the three fields above occupy 24 bytes and BLK_HDR was already padded
    // to 32 for 16-byte alignment, so `prev` lands in bytes that were pure
    // padding. It buys O(1) BACKWARD coalescing, which is the only reason
    // free() ever had to walk the whole list (see the block comment on the
    // free bins below).
    struct block_header *prev;
} block_header_t;

// Physical header size used for pointer arithmetic. Padded up to 16 so that,
// since HEAP_START is 16-aligned and every allocation size is rounded to a
// multiple of 16, EVERY pointer malloc() returns is 16-byte aligned. glibc and
// mlibc both guarantee max_align_t (16 on x86-64); CPython, SSE and doubles
// rely on it. (Previously data started at sizeof()==24 -> only 8-aligned.)
#define BLK_HDR 32

// #245 domscript: the header must keep FITTING the padding it was given. If a
// future field pushes it past 32 the data area silently stops being 16-aligned
// and every SSE store through a malloc'd pointer becomes a landmine, so fail
// the build instead. userland/libc/tests/malloc_cycle_test.c and
// userland/apps/heaptest/main.c both mirror this layout and assert the same
// thing; change all three together.
_Static_assert(sizeof(block_header_t) <= BLK_HDR,
               "block_header_t no longer fits the padded BLK_HDR");

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
// MEASURED 2026-08-25 on the owner's a test VM (golden 2054): the compositor took
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
// block; free_locked()'s two merges (forward and, since #245 domscript,
// backward) and realloc()'s absorb each destroy one.
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
// That round of fixes kept the SAME data structure and every corruption guard;
// it removed three of the four linear walks:
//
//   heap_last   - tail pointer, so request_space() appends in O(1). Maintained
//                 at the only sites that can unlink a node. STILL HERE.
//   g_free_hint - a single scalar UPPER BOUND on the largest free data area,
//                 letting find_free_block() reject in O(1) when no free block
//                 could possibly be large enough. SUPERSEDED, see the next
//                 block comment: a scalar bound that every free() raises does
//                 not reject the case that actually mattered, and proving it
//                 wrong cost a complete list scan.
//   g_free_cur  - next-fit cursor. SUPERSEDED by the free bins, which do not
//                 walk allocated blocks at all, so there is nothing to skip.
//   the sweep   - free() merged forward in O(1) and ran the full coalesce_all()
//                 once per 256 frees. SUPERSEDED: `prev` makes the backward
//                 merge O(1) too, so no periodic full sweep is needed at all.
// ============================================================================
// ============================================================================
// #245 domscript: ...AND IT WAS STILL O(live-block-count) PER malloc().
//
// The three fixes above removed three walks and left the fourth, which turned
// out to be the expensive one. MEASURED, not inferred, with the shipping
// allocator algorithm driven by the real browser engine and the real Duktape
// binding (one DOM element plus one JS wrapper per loop iteration, nothing
// inserted, so no DOM or GC cost is in the number):
//
//   nodes   blocks   malloc calls   find_free_block steps   script time
//     625     2528          7148              4,954,332       0.029 s
//    1250     4414         12148             13,115,742       0.078 s
//    2500     8121         22148             39,983,617       0.242 s
//    5000    15671         42148            135,906,867       1.305 s
//   10000    30659         82148            496,501,543       5.788 s
//
// 6044 list steps per malloc() against a 30659-block list, and 4x the steps
// for 2x the nodes: quadratic. The SAME loop allocating plain JS objects
// instead of DOM elements, so that the blocks are freed again and the list
// stays 555 long, is LINEAR and 500x cheaper. The list LENGTH is the cost.
//
// WHY g_free_hint DID NOT STOP IT. The hint is one scalar upper bound over all
// sizes and EVERY free() raises it to the freed block's size. So a free of a
// 4 KB block makes a 32-byte malloc believe a fit might exist; proving it does
// not costs a COMPLETE scan of the block list (next-fit wraps, so it is still
// complete), and only then is the hint lowered. The next free raises it again.
// A single global bound cannot be both sound and cheap, and lowering it per
// failed scan just moves the cost to the next size requested.
//
// THE FIX IS STRUCTURAL: an EXPLICIT, SIZE-BINNED FREE LIST. find_free_block()
// now walks only blocks that are actually FREE, bucketed by size, so the long
// already-allocated prefix is never touched. The links live in the free block's
// own PAYLOAD (a free block's data area is by definition unused), so the bins
// cost zero bytes of header; and `prev` in the header, which was padding, makes
// backward coalescing O(1), which is what removes the periodic full sweep
// entirely rather than making it rarer.
//
// The guard set is unchanged in kind: every remaining walk still carries the
// #631 step budget and still reports through heap_report_walk_overrun(), and
// the redzone / range / size-sanity checks are untouched.
// ============================================================================
static block_header_t *heap_last = NULL;   // tail of the block list

// Free-list links, stored IN the free block's data area. A free block's payload
// is unused by definition, so this is free of charge; it is also why every
// block is forced to carry at least HEAP_MIN_DATA bytes of payload (below), so
// there is always room for the two links without touching the trailing redzone.
typedef struct free_link {
    block_header_t *fnext;
    block_header_t *fprev;
} free_link_t;
#define FLINK(b) ((free_link_t *)((uint8_t *)(b) + BLK_HDR))

// Smallest data area any block may have. 16 bytes would already hold the two
// links, but the redzone canary sits at payload[size-8], so 16 would make the
// links and the canary the same bytes. 32 keeps them disjoint:
// links at [0,16), canary at [size-8, size).
#define HEAP_MIN_DATA 32
_Static_assert(HEAP_MIN_DATA >= (int) sizeof(free_link_t) + 8,
               "#245: a free block must hold both links and its redzone");

// Bins 0..HEAP_NBIN-2 are EXACT size classes in 16-byte steps (every block size
// is a multiple of 16), so the common small allocation is an O(1) pop. The last
// bin is the catch-all for everything larger and is the only one that has to be
// searched for a fit.
#define HEAP_BIN_STEP      16
#define HEAP_BIN_MAX_EXACT 1024
#define HEAP_NBIN          ((HEAP_BIN_MAX_EXACT / HEAP_BIN_STEP) - 1)  /* 63 */
static block_header_t *free_bins[HEAP_NBIN];

// Length of each bin, maintained at the only two sites that change it. It is
// what makes a CYCLE in a bin detectable independently of how big the heap is:
// a well-formed bin of length L cannot be walked for more than L steps, so the
// step budget for a bin walk is L-derived, not heap-derived. Like g_block_count
// it can only ever OVER-count (a dropped bin orphans its nodes), never under,
// so it cannot produce a false corruption report.
static uint32_t bin_count[HEAP_NBIN];

// How far into the catch-all bin a single request may look before giving up and
// growing the heap instead. A bound is required here for the same reason every
// other walk in this file has one, and unlike those it is also a performance
// bound: without it one request can walk every large free block. Giving up is
// SOUND, not a fallback to wrong behaviour: request_space() then satisfies the
// caller from fresh arena, which costs address space, never correctness.
//
// 512 IS MEASURED, not picked. It is the point where giving up stops costing
// memory. 3,000,000 random malloc/realloc/free operations with sizes from 1 to
// 20,000 bytes (peak 3143 live blocks), heap arena high-water:
//
//      cap   8    43,634 KB
//      cap  64    15,578 KB
//      cap 512    12,564 KB
//      cap 4096   12,373 KB      (diminishing: 1.5% better for 8x the work)
//      pre-fix    14,558 KB      (unbounded first-fit over the WHOLE list)
//
// So 512 uses 14% LESS arena than the allocator this replaces while bounding
// the work, i.e. this is not a memory-for-time trade at all on that workload.
// A cap of 64 would have been one, which is why the number was measured.
// The cap is a CONSTANT, so no choice of it changes the complexity; it only
// decides how good the reuse is.
#define HEAP_BIN_SCAN_MAX 512

// The cached tail is only trusted once the list is long enough for the walk it
// replaces to matter; a short list still takes the validated walk, so a small
// app's behaviour (and every corruption report it would have produced) is
// unchanged. userland/libc/tests/run_malloc.sh, the #631 cycle-detection
// regression test, lives in exactly this regime and must keep passing.
#define HEAP_BIGLIST 512
static int heap_big(void) { return g_block_count > (uint64_t) HEAP_BIGLIST; }

// A node is being unlinked from the address-ordered list. Keep the cached tail
// honest; a stale one would be a wild dereference later, which is exactly the
// class of bug the guards in this file exist for. The free bins are NOT fixed
// up here: a node leaving the list must be unlinked from its bin explicitly by
// whoever removes it, because "which bin" depends on the size it had BEFORE any
// merge changed it.
static void heap_note_unlinked(block_header_t *removed, block_header_t *keep) {
    if (heap_last == removed)  heap_last = keep;
}

// Rate-limited and reported through the same [malloc] channel the allocator
// already uses for an invalid free, so a corrupt heap produces a
// diagnosable event instead of drowning the log on every subsequent free().
// Same de-duplication shape as the kernel's [WQBLOCK] assert.
#define HEAP_WALK_REPORT_MAX 8
static unsigned g_walk_reports = 0;

// #245 domscript: TOTAL steps taken by every bounded walk in this file, for the
// lifetime of the process. It exists so that "malloc() does not do work that
// grows with the number of live blocks" is a MEASURED, deterministic property
// rather than a wall-clock impression: see userland/libc/tests/run_malloc_scale.sh,
// which asserts on this counter at two workload sizes and has a RED arm against
// the pre-fix allocator. It is free in the fixed allocator precisely because the
// walks it counts no longer run: a hit in an exact size bin takes zero steps.
static uint64_t g_walk_steps = 0;

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
// #245 domscript: COUNT ALWAYS, PRINT ONLY UP TO THE CAP. This used to stop
// INCREMENTING once the cap was passed, which is the exact defect the counter
// beside it (g_walk_reports) carries a comment warning against: a counter that
// stops counting is a counter that lies. MEASURED IN THE OS, not inferred:
// userland/apps/heaptest runs all its scenarios in ONE process, so the first
// scenario exhausted the cap and scenarios 2 and 3 then scored a corruption the
// allocator HAD detected and printed as "terminated but reported nothing". The
// host test (userland/libc/tests/run_malloc.sh) could not see it because it runs
// each scenario in a separate process. Printing is still capped at
// HEAP_DIAG_REPORT_MAX and the suppression notice is still emitted exactly once;
// only the arithmetic changed.
static int heap_diag_ok(void) {
    g_diag_reports++;
    if (g_diag_reports > HEAP_DIAG_REPORT_MAX) {
        if (g_diag_reports == HEAP_DIAG_REPORT_MAX + 1) {
            heap_write_str("[malloc] further heap corruption reports suppressed\n");
        }
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
        // (BLK_HDR + HEAP_MIN_DATA). Still strictly bounded, just no longer tight, so
        // the allocator keeps working on a heap it already knows is damaged.
        uint64_t hard = ((heap_end - HEAP_START) / (BLK_HDR + HEAP_MIN_DATA)) + 8;
        if (lim < hard) lim = hard;
    }
    return lim;
}

// Test-only introspection for the #631 guard's proof-of-firing test
// (userland/apps/heaptest). Deliberately NOT declared in stdlib.h: it exists
// so a test can assert the guard actually fired, which is the whole point of
// a guard. Returns the number of overrun reports emitted so far.
unsigned __malloc_walk_report_count(void) { return g_walk_reports; }

// #245 domscript: the OTHER report channel, for the same reason. Not every
// corruption is a walk that ran too long: a value guard (range, size sanity,
// a block that is not in the bin its size selects) can catch the same damage
// without any walk overrunning, and after this change some shapes are ALWAYS
// caught that way. A test that could only see the step-budget counter read
// "terminated but reported nothing" on a corruption that was in fact reported
// loudly. Returns the number of guard reports emitted so far.
unsigned __malloc_diag_report_count(void) { return g_diag_reports; }

// #245 domscript: see g_walk_steps. Deliberately NOT in stdlib.h for the same
// reason as the two counters above: it exists for a test, and a test is the
// only honest way to keep a complexity claim true.
uint64_t __malloc_walk_steps(void) { return g_walk_steps; }

// --- #245 domscript: the explicit, size-binned free list -------------------
//
// THE ONE INVARIANT. A block sits in exactly one bin if and only if
// block->free, and that bin is bin_index(block->size). Every site that flips
// ->free or changes ->size must bin_unlink() BEFORE and bin_insert() AFTER,
// because the bin a block belongs to is a function of the size it had at the
// time it was linked. Nothing anywhere else may read or write a free block's
// first 16 payload bytes.

// The class a block of this size BELONGS to (rounds DOWN).
static int bin_index(size_t size) {
    size_t i = size / HEAP_BIN_STEP;
    if (i < HEAP_MIN_DATA / HEAP_BIN_STEP) i = HEAP_MIN_DATA / HEAP_BIN_STEP;
    i -= HEAP_MIN_DATA / HEAP_BIN_STEP;                 /* HEAP_MIN_DATA -> 0 */
    if (i >= (size_t) HEAP_NBIN) i = (size_t) HEAP_NBIN - 1;
    return (int) i;
}

// The smallest class that can SATISFY this request (rounds UP). Identical to
// bin_index() for every size the allocator actually asks for, since those are
// all multiples of HEAP_BIN_STEP; written separately so that the "a block in a
// higher exact bin always fits" argument below holds by construction rather
// than by relying on that fact staying true.
static int bin_index_fit(size_t size) {
    return bin_index((size + HEAP_BIN_STEP - 1) & ~(size_t)(HEAP_BIN_STEP - 1));
}

static void bin_insert(block_header_t *b) {
    int i = bin_index(b->size);
    free_link_t *l = FLINK(b);
    l->fprev = NULL;
    l->fnext = free_bins[i];
    if (free_bins[i]) FLINK(free_bins[i])->fprev = b;
    free_bins[i] = b;
    bin_count[i]++;
}

static void bin_unlink(block_header_t *b) {
    int i = bin_index(b->size);
    free_link_t *l = FLINK(b);
    block_header_t *p = l->fprev, *n = l->fnext;
    if (p && block_in_range(p)) FLINK(p)->fnext = n;
    if (n && block_in_range(n)) FLINK(n)->fprev = p;
    if (free_bins[i] == b) {
        free_bins[i] = n;
        if (bin_count[i]) bin_count[i]--;
    } else if (p) {
        if (bin_count[i]) bin_count[i]--;
    } else {
        // b claims to be a bin head but is not the head of the bin its size
        // selects, so either its size changed while it was linked (a bug at
        // one of the sites named in the invariant above) or the links are
        // damaged. Nothing was written through a wrong head, so the bins stay
        // consistent; say so rather than continuing silently.
        if (heap_diag_ok()) {
            heap_write_str("[malloc] heap corruption: bin_unlink on a block that is not in its own bin ");
            heap_write_hex((uint64_t)b);
            heap_write_str(" size="); heap_write_hex((uint64_t)b->size);
            heap_write_str("\n");
        }
    }
    l->fnext = NULL;
    l->fprev = NULL;
}

// Drop a whole bin after finding damage in it. The blocks become unreachable
// (an address-space leak, bounded by the heap ceiling) instead of a wild
// dereference, which is the same trade every other guard in this file makes.
static void bin_drop(int i, const char *why, block_header_t *b) {
    if (heap_diag_ok()) {
        heap_write_str("[malloc] heap corruption: ");
        heap_write_str(why);
        heap_write_str(" at block ");
        heap_write_hex((uint64_t)b);
        heap_write_str("; dropping free bin ");
        heap_write_hex((uint64_t)i);
        heap_write_str("\n");
    }
    free_bins[i] = NULL;
    bin_count[i] = 0;
}

// Returns a free block of at least `size` bytes of data area, ALREADY UNLINKED
// from its bin and still marked free; the caller owns it. NULL means "nothing
// reusable", on which malloc_locked() grows the heap, so NULL is never a
// failure the caller sees.
//
// This walks FREE blocks only. That is the whole point: the previous version
// walked the address-ordered list of ALL blocks, so it paid for every live DOM
// node ever allocated (see the measurement table above).
static block_header_t *find_free_block(size_t size) {
    int want = bin_index_fit(size);

    // Exact classes first. Every block in a class at or above `want` is at
    // least `size` bytes by construction, so the head of the first non-empty
    // one fits and there is nothing to search.
    for (int i = want; i < HEAP_NBIN - 1; i++) {
        block_header_t *b = free_bins[i];
        if (!b) continue;
        if (!block_in_range(b)) { bin_drop(i, "free-bin head out of range", b); continue; }
        if (!b->free || !block_size_sane(b) || b->size < size) {
            bin_drop(i, "free-bin head disagrees with its bin", b);
            continue;
        }
        bin_unlink(b);
        return b;
    }

    // The catch-all class is the only one that can hold blocks SMALLER than the
    // request, so it is the only one that is searched. Two independent bounds:
    // the #631 step budget catches a cycle in the links, and HEAP_BIN_SCAN_MAX
    // caps the work on a long but perfectly well-formed bin.
    int last = HEAP_NBIN - 1;
    // Two INDEPENDENT bounds, and the order matters. The cycle bound comes from
    // the bin's own recorded length, so a loop of four nodes is caught in twelve
    // steps whether the heap holds ten blocks or ten million; deriving it from
    // heap_walk_limit() alone would let the effort cap below silently preempt
    // the cycle detector on any large heap, which is the #631 failure mode
    // wearing a different hat. heap_walk_limit() is still applied as the
    // backstop for a bin_count that corruption has driven too high.
    uint64_t steps = 0;
    uint64_t limit = (uint64_t) bin_count[last] + 8;
    uint64_t hard = heap_walk_limit();
    if (limit > hard) limit = hard;
    int looked = 0;
    block_header_t *b = free_bins[last];
    while (b) {
        if (g_walk_steps++, ++steps > limit) {
            heap_report_walk_overrun("free-bin scan", b, steps);
            break;
        }
        if (!block_in_range(b)) { bin_drop(last, "free-bin node out of range", b); break; }
        if (!b->free || !block_size_sane(b)) {
            bin_drop(last, "free-bin node is not a sane free block", b);
            break;
        }
        if (b->size >= size) {
            bin_unlink(b);
            return b;
        }
        if (++looked >= HEAP_BIN_SCAN_MAX) break;   // grow the heap instead
        b = FLINK(b)->fnext;
    }
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
    block->prev = NULL;        // #245 domscript: set by whichever append wins
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
            block->prev = heap_last;          // #245 domscript
            heap_last->next = block;
            heap_last = block;
            heap_end += total;
            g_block_count++;
            return block;
        }
        // #631: bounded walk - see heap_walk_limit() above.
        uint64_t steps = 0, limit = heap_walk_limit();
        while (last->next) {
            if (g_walk_steps++, ++steps > limit) {
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
        block->prev = last;                   // #245 domscript
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

// #245 domscript: coalesce_all() IS GONE, and that is the point.
//
// Its job was to merge a freed block with its free PREDECESSOR, which the
// address-ordered singly-linked list could not find without walking from
// heap_start. That walk ran once per 256 frees and was therefore still a term
// that grows with the live block count. `prev` in the block header, which cost
// nothing because BLK_HDR was already padded to 32, makes the backward merge a
// single O(1) step inside free_locked(), so there is no sweep to amortise.
//
// Two consequences worth stating. The no-two-adjacent-free-blocks invariant is
// now maintained EAGERLY on every free rather than within 256 frees, so
// fragmentation is strictly better, not merely as good. And the periodic sweep
// was also the only place that recomputed g_free_hint exactly; the size bins
// answer that question structurally, so the hint is gone with it.


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
    // #245 domscript: every block, including a split remainder, must be able to
    // hold the two free-list links in its payload without touching its redzone.
    if (needed < HEAP_MIN_DATA) needed = HEAP_MIN_DATA;

    // Try to find a free block. It comes back ALREADY UNLINKED from its bin.
    block_header_t *block = find_free_block(needed);

    if (block) {
        // Split block if the remainder can hold a header plus a usable chunk.
        // #245 domscript: the threshold is HEAP_MIN_DATA, not 16, because a
        // 16-byte remainder could not carry the free-list links clear of its
        // own redzone.
        if (block->size >= needed + BLK_HDR + HEAP_MIN_DATA) {
            block_header_t *new_block = (block_header_t *)((uint8_t *)block +
                                        BLK_HDR + needed);
            new_block->size = block->size - needed - BLK_HDR;
            new_block->free = 1;
            new_block->next = block->next;
            new_block->prev = block;                         // #245 domscript
            if (block->next) block->next->prev = new_block;   // #245 domscript
            *(uint64_t *)((uint8_t *)new_block + BLK_HDR + new_block->size - 8) = HEAP_CANARY;

            if (heap_last == block) heap_last = new_block;   // #245
            block->size = needed;
            block->next = new_block;
            g_block_count++;  // #631: the split created a node
            bin_insert(new_block);                           // #245 domscript
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

    // #245 domscript: a DOUBLE FREE must be refused, not absorbed. An already
    // free block is already in a bin; running the rest of this function on it
    // would link one node into a bin twice, which is a cycle in the bin list
    // and corrupts every later allocation. The old code could not detect this
    // and merely re-coalesced. Refusing is strictly safer and names the bug at
    // the call that commits it rather than at some unrelated malloc() later.
    if (block->free) {
        if (heap_diag_ok()) {
            heap_write_str("[malloc] invalid free: block is already free (double free) ");
            heap_write_hex((uint64_t)ptr);
            heap_write_str("\n");
        }
        return;
    }
    block->free = 1;

    // #245 domscript: merge FORWARD. One step is enough while the
    // no-two-adjacent-free-blocks invariant holds, which this function now
    // maintains eagerly; the loop and its #631 step budget stay because a
    // corrupted heap can break the invariant and a cycle must still terminate
    // with a report rather than spin.
    {
        uint64_t steps = 0, limit = heap_walk_limit();
        while (block->next && block_in_range(block->next) &&
               block->next->free && block_size_sane(block->next)) {
            if (g_walk_steps++, ++steps > limit) {
                heap_report_walk_overrun("free forward merge", block, steps);
                break;
            }
            block_header_t *victim = block->next;
            bin_unlink(victim);                   // #245 domscript: before size changes
            heap_note_unlinked(victim, block);
            block->size += BLK_HDR + victim->size;
            block->next = victim->next;
            if (block->next) block->next->prev = block;
            if (g_block_count) g_block_count--;
        }
    }

    // #245 domscript: merge BACKWARD in O(1). This is what coalesce_all() used
    // a full list walk to do. The ->next cross-check is the cheap way to say
    // "these two really are neighbours" before folding one into the other.
    if (block->prev && block_in_range(block->prev) && block->prev->free &&
        block_size_sane(block->prev) && block->prev->next == block) {
        block_header_t *keep = block->prev;
        bin_unlink(keep);                         // its size is about to change
        heap_note_unlinked(block, keep);
        keep->size += BLK_HDR + block->size;
        keep->next = block->next;
        if (keep->next) keep->next->prev = keep;
        if (g_block_count) g_block_count--;
        block = keep;
    }

    if (block->next == NULL) heap_last = block;
    bin_insert(block);
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
        bin_unlink(block->next);                      // #245 domscript
        heap_note_unlinked(block->next, block);       // #245
        block->size += BLK_HDR + block->next->size;
        block->next = block->next->next;
        if (block->next) block->next->prev = block;   // #245 domscript
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

// div/ldiv/lldiv: C-standard division returning quotient and remainder.
// Added for the libarchive port (#745, archive_time.c). C99 requires quot to
// truncate toward zero, which is exactly what C integer / and % already do.
div_t div(int numer, int denom) { div_t r; r.quot = numer / denom; r.rem = numer % denom; return r; }
ldiv_t ldiv(long numer, long denom) { ldiv_t r; r.quot = numer / denom; r.rem = numer % denom; return r; }
lldiv_t lldiv(long long numer, long long denom) { lldiv_t r; r.quot = numer / denom; r.rem = numer % denom; return r; }

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

// strtoimax/strtoumax: the <inttypes.h> intmax_t parsers. They were DECLARED in
// inttypes.h but defined nowhere (a latent gap surfaced by the libarchive port,
// #745). On x86-64 intmax_t is long long, so these are thin forwarders. Purely
// additive: nothing else in the tree defined them, so no multiple-definition.
// intmax_t is "long" on x86-64 (gcc stdint.h); match that width exactly.
long strtoimax(const char *nptr, char **endptr, int base) {
    return (long)strtoll(nptr, endptr, base);
}
unsigned long strtoumax(const char *nptr, char **endptr, int base) {
    return (unsigned long)strtoull(nptr, endptr, base);
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

// mkstemp: create and open a unique temporary file, added for the libedit port
// (#745). The template's trailing "XXXXXX" is replaced with a filename that did
// not previously exist; the file is created O_EXCL with 0600 permissions.
int mkstemp(char *tmpl)
{
	static const char set[] =
	    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	size_t n = strlen(tmpl);
	if (n < 6) {
		errno = EINVAL;
		return -1;
	}
	char *x = tmpl + n - 6;
	for (int i = 0; i < 6; i++) {
		if (x[i] != 'X') {
			errno = EINVAL;
			return -1;
		}
	}
	// A cheap changing seed: mix an incrementing counter with a clock tick.
	static unsigned counter = 0;
	unsigned seed = (unsigned)(uintptr_t)tmpl ^ (counter += 0x9e3779b9u);
	seed ^= (unsigned)time((void *)0) * 2654435761u;
	for (int attempt = 0; attempt < 4096; attempt++) {
		seed = seed * 1103515245u + 12345u;
		unsigned r = seed;
		for (int i = 0; i < 6; i++) {
			x[i] = set[r % 62u];
			r /= 62u;
			r += seed >> (i + 1);
		}
		int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0)
			return fd;
		if (errno != EEXIST)
			return -1;
	}
	errno = EEXIST;
	return -1;
}
