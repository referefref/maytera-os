// heap.c - Kernel Heap Allocator implementation
// Simple linked-list based allocator with first-fit strategy

#include "heap.h"
#include "pmm.h"
#include "heapshadow.h"   // #aicappersist: the window the heap shadows
#include "heapzero.h"     // #heapdbl: which cause zeroed a block header
#include "heapwin.h"      // #heapwin: WHICH reservation already owned a window page
#include "heapframe.h"    // #deadpml4: what the heap's page mappings cost, and the ceiling
#include "../boot_info.h"  // #heapdbl: MEMORY_TYPE_USABLE, to tell a firmware-reserved window page from an already-allocated one
#include "vmm.h"
#include "../serial.h"
#include "mmlog.h"   // #dosmem: mm anomalies must reach a serial-less machine
#include "../string.h"
#include "../sync/spinlock.h"   // #114/#75: the SHARED irqsave spinlock (was a private copy)

// Heap configuration
#define HEAP_INITIAL_SIZE   (128 * MB)     // Initial heap size - needs 64MB+ for NetHack ELF (8.4MB)
#define HEAP_MAX_SIZE       (256 * MB)    // Maximum heap size
#define HEAP_BLOCK_MAGIC    0x48454150    // "HEAP"
#define HEAP_MIN_BLOCK_SIZE 32            // Minimum block size (including header)

// Heap virtual address range
// We'll use the virtual address range starting at 0x10000000 (256 MB)
#define HEAP_VIRT_BASE      0x10000000ULL

// Block header structure
typedef struct heap_block {
    uint32_t magic;              // Magic number for validation
    uint32_t flags;              // Flags (bit 0 = used)
    size_t   size;               // Size of data area (not including header)
    struct heap_block *next;     // Next block in free list (only valid if free)
    struct heap_block *prev;     // Previous block in free list
} __attribute__((packed)) heap_block_t;

#define BLOCK_HEADER_SIZE   sizeof(heap_block_t)
#define BLOCK_FLAG_USED     (1 << 0)

// Heap state
static uint64_t heap_start = 0;
static uint64_t heap_end = 0;
static uint64_t heap_size = 0;

// #137 (see heap_expand below)
#ifdef HEAPFRAMES_REVERT
static void heap_expand_rollback(uint64_t mapped);
#endif
static int  heap_expand_once(size_t expand_size);
extern uint64_t vmm_get_physical(uint64_t virt_addr);
extern void     vmm_unmap_page(uint64_t virt_addr);
extern uint64_t vmm_get_physical_in(uint64_t pml4_phys, uint64_t virt_addr);
// #deadpml4: both are defined below heap_reserve_shadowed_window(), which calls
// the first of them.
static void heap_scan_window_safety(void);
static void heapframe_report_cost(void);
static heap_block_t *free_list = NULL;

// Statistics
static size_t total_allocated = 0;
static size_t total_freed = 0;
static size_t allocation_count = 0;
static size_t free_count = 0;

// #heapinvptr: HOW MANY TIMES THE ALLOCATOR HAS REFUSED A POINTER.
//
// The MM_ANOMALY lines below already NAME each refusal, which is what a human
// reading a serial log needs. A self-test needs a NUMBER it can compare before
// and after a piece of work, and neither a kernel self-test nor the owner's two
// machines (no serial port, see mmlog.h) can grep a log. These two counters are
// the machine-readable half of the same evidence, nothing more: they change no
// behaviour and are never used to decide anything.
static uint64_t invalid_free_count = 0;   // kfree()/krealloc() on a bad pointer
static uint64_t double_free_count  = 0;   // a block freed while already free

// Spinlock for thread safety.
//
// #347: heap_lock used to be a plain busy-wait spinlock that never disabled
// interrupts. Any interrupt handler that calls kmalloc()/kfree() on the same
// CPU while foreground code already holds heap_lock (e.g. mid-split/coalesce)
// would spin on this lock from inside the ISR - forever, since the CPU that
// could release it is the one now stuck servicing the interrupt. That is a
// self-deadlock with IF effectively pinned by the interrupt gate, silently
// stopping the timer tick and every other interrupt on that core. The fix was
// to save/clear IF around the critical section so an ISR on this core can never
// observe heap_lock held mid-update, and so the critical section itself can't
// be interrupted and re-entered.
//
// #114/#75: that fix was a hand-rolled irqsave lock, a PRIVATE copy of a
// primitive that already exists in sync/spinlock.h. The identical private lock
// ten files away in mm/pmm.c was not converted alongside it and went on to
// deadlock two cores at #75. So heap_lock is now the SHARED spinlock, exactly
// as mm/pmm.c, fs/blockdev.c, fs/ext2.c and drivers/ata.c use it. The IF-masking
// semantics #347 established are preserved unchanged: spinlock_acquire_irqsave()
// does pushfq; popq; cli (identical to the old acquire), and
// spinlock_release_irqrestore() does popfq, which restores IF to its saved value
// (same net effect as the old conditional sti). The critical sections, their
// scope, and the returned-flags convention are all unchanged; only the mechanism
// is now the one shared implementation. spinlock_acquire_irqsave() itself does no
// allocation, so there is no kmalloc recursion.
static spinlock_t heap_lock = SPINLOCK_INIT_NAMED("heap");

static uint64_t heap_acquire_lock(void) {
    return spinlock_acquire_irqsave(&heap_lock);
}

static void heap_release_lock(uint64_t rflags) {
    spinlock_release_irqrestore(&heap_lock, rflags);
}

// Expand the heap by allocating more pages.
//
// #137: this used to LEAK EVERY PAGE IT HAD ALREADY TAKEN whenever it gave up
// partway. The loop below allocates and maps one page at a time, and both
// failure exits simply `return -1`: the pages taken on earlier iterations were
// never unmapped and never handed back to the PMM, and because heap_end and
// heap_size were only advanced AFTER the loop, the heap did not know about them
// either. Nothing in the system could ever reclaim them. A failure at the last
// page of a 128 MB expansion orphaned 128 MB of RAM, and the failure is
// REACHABLE FROM RING 3 (an unvalidated win_create() size, see
// rustkern/winbuf.rs), so an unprivileged app could drain physical memory until
// every kernel allocation failed - which presents as the machine stopping with
// no panic, because the thing that fails next is whatever asks for memory next.
//
// Two changes:
//   1. ROLL BACK on failure. Unmap and free exactly the pages this call took,
//      leaving the PMM and the page tables as they were found. A failed
//      expansion is now genuinely free of side effects.
//   2. DO NOT OVER-ASK. The old code always tried for HEAP_INITIAL_SIZE
//      (128 MB) even to satisfy a 4 KB allocation, so one large-ish request on
//      a machine with less than 128 MB free failed even though the memory it
//      actually needed was there. Try the big chunk first (large chunks keep
//      the free list short, which matters because find_free_block is O(n)),
//      then fall back to the smallest expansion that would actually satisfy
//      the caller before reporting failure.
static int heap_expand_once(size_t expand_size) {
    if (expand_size == 0) return -1;
    if (heap_size + expand_size > HEAP_MAX_SIZE) {
        if (heap_size >= HEAP_MAX_SIZE) {
            return -1;  // Cannot expand further
        }
        expand_size = HEAP_MAX_SIZE - heap_size;
    }
    expand_size &= ~((size_t)PMM_PAGE_SIZE - 1);
    if (expand_size < PMM_PAGE_SIZE) return -1;

#ifdef HEAPFRAMES_REVERT
    // #deadpml4: `make HEAPFRAMESREVERT=1`. THE BEFORE ARM, byte for byte the
    // pre-#deadpml4 expansion: a frame per heap page, mapped into a hierarchy the
    // CPU never walks. It exists so the reclaim's benefit is a MEASURED
    // difference between two kernels built from one source, not a claim, and so a
    // rollback is one make variable rather than a revert. Same argument and same
    // shape as HEAPSHADOWREVERT above. NEVER for a golden.
    uint64_t pages_needed = expand_size / PMM_PAGE_SIZE;

    // Allocate physical pages and map them
    for (uint64_t i = 0; i < pages_needed; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            heap_expand_rollback(i);
            return -1;
        }

        uint64_t virt = heap_end + i * PMM_PAGE_SIZE;
        if (vmm_map_page(virt, phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE) != 0) {
            pmm_free_page(phys);
            heap_expand_rollback(i);
            MM_ANOMALY("[HEAP] failed to MAP a kernel heap page: the physical page was "
                       "obtained but vmm_map_page_in refused it");
            kprintf("[HEAP] ERROR: Failed to map heap page\n");
            return -1;
        }
    }
#else
    // #deadpml4: ADOPT THE WINDOW, DO NOT ALLOCATE A FRAME PER PAGE.
    //
    // WHY THE LOOP THAT WAS HERE BOUGHT NOTHING. It called pmm_alloc_page() once
    // per heap page and mapped the frame with vmm_map_page(), which walks
    // `kernel_pml4` (mm/vmm.c:14) - a static .bss array vmm_init() zeroes and
    // NEVER loads into CR3. The CPU keeps translating this window through the
    // firmware's identity map, so the heap's arena is the physical memory at its
    // own addresses, which heap_reserve_shadowed_window() has already withheld
    // from the PMM. Every frame that loop took was allocated and then addressed
    // by nothing. MEASURED per page, not inferred from one: see
    // heapframe_report_cost() below, which walks BOTH hierarchies and writes a
    // per-page sentinel through the heap for every page of the arena.
    //
    // WHAT THIS CHANGES AND WHAT IT DOES NOT. Not one byte of heap storage moves:
    // the arena was, and remains, [heap_end, heap_end + expand_size) of identity
    // -mapped physical memory. The only difference is that a second, unrelated
    // frame is no longer withheld per page. On a 576 MB VM that is 128 MB handed
    // back out of a 506 MB managed range.
    //
    // AND IT REQUIRES THE CEILING, which is the part that is easy to miss. The
    // old code could not grow past 128 MB because the PMM was empty by then; the
    // frames were an accidental brake. With them reclaimed the heap can afford to
    // creep to HEAP_MAX_SIZE, and at 576 MB the firmware's write-protected blocks
    // sit 232 MB into the window, where the first write is a Ring-0 #PF err=0x3.
    // heapframe_clamp_rs() refuses to cross the lowest window page measured as
    // unwritable or not-RAM. See rustkern/heapframe.rs.
#ifdef HEAPFRAME_NO_CLAMP
    // #deadpml4 `make HEAPFRAMENOCLAMP=1`: THE CONTROL ARM. Reclaim the frames and
    // REMOVE the ceiling, which is the state the reclaim would have shipped in if
    // the ceiling had been treated as a nicety rather than a precondition. Only
    // meaningful with HEAPFRAMECLAMPTEST=1, which drives the heap at the ceiling
    // on purpose; on its own it changes nothing at boot, because the heap only
    // grows on demand. NEVER for a golden.
#else
    {
        uint64_t grant = heapframe_clamp_rs(heap_end, (uint64_t)expand_size);
        if (grant < PMM_PAGE_SIZE) {
            static int told = 0;
            if (!told) {
                told = 1;
                kprintf("[HEAPFRAME] expansion REFUSED: heap_end 0x%llx has reached the "
                        "safe ceiling 0x%llx (cause %d: 0=window end, 1=a page Ring 0 "
                        "cannot write, 2=a page that is not RAM). The heap stops here "
                        "instead of faulting on it.\n",
                        (unsigned long long)heap_end,
                        (unsigned long long)heapframe_ceiling_rs(),
                        (int)heapframe_ceiling_cause_rs());
            }
            return -1;
        }
        if (grant < (uint64_t)expand_size) {
            kprintf("[HEAPFRAME] expansion CLAMPED from %llu to %llu KB: the safe "
                    "ceiling is 0x%llx (cause %d)\n",
                    (unsigned long long)(expand_size / KB),
                    (unsigned long long)(grant / KB),
                    (unsigned long long)heapframe_ceiling_rs(),
                    (int)heapframe_ceiling_cause_rs());
        }
        expand_size = (size_t)grant;
    }
#endif // HEAPFRAME_NO_CLAMP
#endif

    // Create a new free block for the expanded area
    heap_block_t *new_block = (heap_block_t *)heap_end;
    new_block->magic = HEAP_BLOCK_MAGIC;
    new_block->flags = 0;  // Free
    new_block->size = expand_size - BLOCK_HEADER_SIZE;
    new_block->next = free_list;
    new_block->prev = NULL;

    if (free_list) {
        free_list->prev = new_block;
    }
    free_list = new_block;

    heap_end += expand_size;
    heap_size += expand_size;

    return 0;
}

#ifdef HEAPFRAMES_REVERT
// #137: undo a partial expansion. `mapped` is the number of pages this call
// had already mapped at [heap_end, heap_end + mapped*PAGE). heap_end has NOT
// been advanced yet, so this range is not part of the heap and nothing else can
// be looking at it; unmapping and freeing it is safe and is the only way those
// pages ever get back to the PMM.
static void heap_expand_rollback(uint64_t mapped) {
    for (uint64_t j = 0; j < mapped; j++) {
        uint64_t virt = heap_end + j * PMM_PAGE_SIZE;
        uint64_t phys = vmm_get_physical(virt);
        vmm_unmap_page(virt);
        if (phys) pmm_free_page(phys);
    }
}
#endif // HEAPFRAMES_REVERT

// Expand the heap for a caller that needs at least `min_size` contiguous bytes.
// Prefers a large chunk (short free list) but never fails while a smaller
// expansion would have worked.
static int heap_expand(size_t min_size) {
    size_t want = (min_size + BLOCK_HEADER_SIZE + PMM_PAGE_SIZE - 1)
                  & ~((size_t)PMM_PAGE_SIZE - 1);
    size_t big  = HEAP_INITIAL_SIZE;
    if (want > big) big = want;

    if (heap_expand_once(big) == 0) return 0;
    if (want < big && heap_expand_once(want) == 0) return 0;

    kprintf("[HEAP] ERROR: expansion for %lu bytes failed (heap %lu of %lu KB)\n",
            (unsigned long)min_size, (unsigned long)(heap_size / KB),
            (unsigned long)(HEAP_MAX_SIZE / KB));
    return -1;
}

// Initialize the kernel heap
// #aicappersist: THE HEAP SHADOWS A RANGE OF PHYSICAL ADDRESSES, AND THE REST OF
// THE KERNEL DOES NOT KNOW IT. Reserve that range in the PMM so the allocator can
// never hand out a page the kernel cannot address.
//
// WHY. This kernel takes "physical == virtual" literally: mm/vmm.c does
// `uint64_t *pd = (uint64_t *)pmm_alloc_page();` and initialises a page table
// through the number the allocator returned, because UEFI identity-maps memory
// and there is no PHYS_TO_VIRT in this tree. heap_expand_once() below hands the
// virtual addresses in [HEAP_VIRT_BASE, HEAP_VIRT_BASE+HEAP_MAX_SIZE) out as the
// heap's arena, so for any physical page P inside the window, `(uint64_t *)P` is
// a pointer INTO THE HEAP, and clearing it as a page table overwrites a live heap
// object.
//
// CORRECTED 2026-10-01 (#heapwin). THIS BLOCK AND rustkern/heapshadow.rs BOTH
// SAID THE REASON WAS THAT "vmm_map_page() REPLACES the identity entries for that
// window". IT DOES NOT. vmm_map_page() (mm/vmm.c:383) walks `kernel_pml4`, a
// static .bss array that vmm_init() memsets to zero and NEVER loads into CR3;
// vmm_init() deliberately keeps the firmware's PML4 forever
// (current_pml4_phys = g_kernel_cr3 = uefi_cr3). The heap's mapping is therefore
// INERT for translation and the window keeps its UEFI identity mapping, which is
// why `(uint64_t *)P` aliases the heap: P IS the heap's storage, not a frame the
// heap was remapped onto. MEASURED two ways on a 576 MB and a 4096 MB VM, see
// heapwin_report_where() below:
//
//   [HEAPWIN] translation of heap base 0x10000000: live CR3 0x1fc01000 -> phys
//             0x10000000; the kernel_pml4 vmm_map_page() writes -> phys 0x847000
//   [HEAPWIN] sentinel: ... the frame vmm_map_page() claims (0x847800) read
//             0x8bd931c289d13109 before and 0x8bd931c289d13109 after ->
//             DIFFERENT MEMORY ... that mapping is inert
//
// The reservation below is correct and necessary under EITHER mechanism, which is
// how the wrong one survived in the comments. Two consequences do depend on which
// is true, so they are written down here rather than left to be rediscovered:
//   1. heap_expand_once() USED TO call pmm_alloc_page() once per heap page and map
//      the frame into the dead hierarchy, so those frames were allocated and NEVER
//      ADDRESSED: 256 MB of reserved window PLUS 128 MB of unreachable frames out
//      of a 506 MB managed range on a 576 MB VM, and the boot ran the PMM to zero
//      ("[PMM] ERROR: Out of physical memory!").
//      FIXED 2026-10-01 (#deadpml4), and the per-page measurement that justified it
//      is heapframe_report_cost() below. All 32769 arena pages: identity in the live
//      CR3, dead-hierarchy frame outside the window, sentinel absent. PMM free after
//      heap_init() went 10920 -> 43756 pages and the 20 "Out of physical memory"
//      lines went to 0. The heap now costs HEAP_MAX_SIZE of reserved window and
//      NOTHING per page, and `make HEAPFRAMESREVERT=1` restores the old loop.
//   2. A page inside the window that the FIRMWARE still owns is not merely
//      shadowed, it is memory the heap would write to directly. See
//      heapwin_report_where().
//
// MEASURED, three boots of three images (goldens 2519, 2520, 2521), always the
// same shape: the first user address space allocated at or above HEAP_VIRT_BASE
//
//     [VMM] Created user address space: PML4=0x10230000
//
// is followed, eight log lines later, by the ext2 group-descriptor block reading
// back as ZEROS
//
//     [FSENFOR] ino=2 group=0 index=1 itab_rc=0 itab=0 -> block=0 off=256
//
// (healthy: itab=100). Clearing that "page table" wrote 4 KB into the kernel heap
// and the 4 KB it landed on was the ext2 block cache's buffer for the group
// descriptor table. Every inode lookup then computes inode-table block 0, so
// inode 2 reads back all zeros WITH SUCCESS, i_mode == 0, and
// ext2_parent_dir_exists() correctly reports that the root directory is not a
// directory. Every create under it is then correctly refused ENOENT, which is
// where "[FSDENY-U] aicap grants REFUSED ... rc=-2" came from: the guard, the
// permission model and the caller were all right and the metadata was corrupt.
//
// THE COST IS STATED, NOT HIDDEN: HEAP_MAX_SIZE of physical memory becomes
// unallocatable (256 MB of the 2047 MB the PMM manages). That is the price of an
// architecture with no PHYS_TO_VIRT, and since #deadpml4 it is the WHOLE price: the
// heap no longer also spends a frame per mapped page. "Make the heap's own mapping
// IDENTITY" was one of the two cheaper fixes named here, and the measurement in
// heapframe_report_cost() shows it ALREADY IS identity and always was; the mapping
// vmm_map_page() wrote was simply inert. What remains of that idea is to move the
// window above the PMM's range, which would reclaim the other 256 MB.
//
// RESERVED FOR THE WHOLE WINDOW, not just the part mapped so far, because the
// heap expands later in the boot and a page inside a not-yet-mapped part could
// by then already belong to a user process, which is a corruption the
// reservation would have arrived too late to prevent. `already` in the line
// below counts exactly that case and MUST be zero.
static void heap_reserve_shadowed_window(void) {
    heapshadow_set_window_rs(HEAP_VIRT_BASE, HEAP_MAX_SIZE);
    // #deadpml4: how high this arena may grow, measured before anything grows.
    // Outside the HEAPSHADOW_REVERT split on purpose: the ceiling is a property of
    // the machine, not of the reservation.
    heap_scan_window_safety();
#ifdef HEAPSHADOW_REVERT
    // #heapdbl: `make HEAPSHADOWREVERT=1`. THE NEGATIVE CONTROL for the
    // reservation. It restores the pre-069121a4 behaviour exactly - the window is
    // published to the predicate but NOT reserved in the PMM - so the eleven
    // `kfree() on an INVALID pointer ... magic=0x0` refusals in the field capture
    // can be reproduced on demand and then watched to disappear. A fix that has
    // only ever been observed working proves nothing about what was wrong, which
    // is the same argument as KSTACKREGTEST / NOBLOCKTEST / SMAPTEST. The
    // reservation cannot be gated on an ESP file instead: heap_init() runs long
    // before any block device or filesystem exists. NEVER for a golden.
    kprintf("[HEAPSHADOW] *** HEAPSHADOWREVERT=1: the window is NOT reserved. This "
            "kernel DELIBERATELY reproduces the #aicappersist heap corruption. "
            "NEVER SHIP. ***\n");
#else
    // #heapwin: the provenance classifier's own self-test runs FIRST, on
    // synthetic runs, and resets the recorder on its way out. A classifier that
    // has only ever been asked about real data has never been shown to
    // discriminate; see rustkern/heapwin.rs.
    uint32_t wchecks = 0;
    int wst = heapwin_selftest_rs(&wchecks);
    heapwin_reset_rs();

    for (uint64_t p = HEAP_VIRT_BASE; p < HEAP_VIRT_BASE + HEAP_MAX_SIZE;
         p += PMM_PAGE_SIZE) {
        // #heapdbl: ask the firmware map what this page IS before calling an
        // "already used" answer a corruption. See heapshadow_note_reserve2_rs().
        int rc = pmm_reserve_page(p);
        int usable = 1;
        if (rc == 0) {
            uint32_t ty = pmm_mmap_type_of(p, 0, 0, 0);
            usable = (ty == MEMORY_TYPE_USABLE);
            // #heapwin: and then ask WHO already owns it, of the two reservations
            // that run between pmm_init() and heap_init(). Both answers come from
            // the owning subsystem's own accessor, not from the address, because
            // "most likely the UEFI page tables" was a guess that turned out to be
            // true for 73 of 1024 pages and false for the other 951.
            if (usable)
                heapwin_note_rs(p, vmm_is_boot_page_table(p),
                                vmm_identity_write_refused(p));
        }
        heapshadow_note_reserve2_rs(rc, usable);
    }
#endif
    uint64_t nnew = 0, already = 0, moot = 0, viol = 0;
    heapshadow_counts_rs(&nnew, &already, &moot, &viol);
    uint32_t checks = 0;
    int st = heapshadow_selftest_rs(&checks);
    uint64_t fw = heapshadow_firmware_count_rs();
    kprintf("[HEAPSHADOW] window 0x%llx-0x%llx reserved: %llu newly, %llu ALREADY "
            "ALLOCATED (must be 0), %llu firmware-reserved (benign), %llu outside the "
            "allocator; predicate self-test %s (%u checks)\n",
            (unsigned long long)HEAP_VIRT_BASE,
            (unsigned long long)(HEAP_VIRT_BASE + HEAP_MAX_SIZE),
            (unsigned long long)nnew, (unsigned long long)already,
            (unsigned long long)fw,
            (unsigned long long)moot, st == 0 ? "PASS" : "FAIL", (unsigned)checks);
#ifndef HEAPSHADOW_REVERT
    // #heapwin: WHO those pages belong to. Until this split existed the line below
    // called every one of them a live corruption, and on a 576 MB VM all 1024 of
    // them were reserved ON PURPOSE inside vmm_init(), which is the only thing that
    // runs between pmm_init() and heap_init(). An alarm that is wrong about 100% of
    // its hits is an alarm nobody reads on the day it is right.
    {
        uint64_t wtab = 0, wro = 0, wunx = 0, wfirst = 0;
        uint32_t wruns = 0, wovf = 0;
        heapwin_counts_rs(&wtab, &wro, &wunx, &wfirst, &wruns, &wovf);
        kprintf("[HEAPWIN] provenance of the %llu already-allocated USABLE window "
                "page(s): %llu live adopted UEFI page-table page(s) "
                "(vmm_reserve_boot_page_tables, #647), %llu non-table page(s) inside "
                "the firmware's write-protected blocks "
                "(vmm_reserve_unwritable_identity_pages, #procspawn), %llu UNEXPLAINED "
                "(must be 0); %u run(s) 0x%llx..0x%llx overflow=%u; classifier "
                "self-test %s (%u checks)\n",
                (unsigned long long)already, (unsigned long long)wtab,
                (unsigned long long)wro, (unsigned long long)wunx,
                (unsigned)wruns, (unsigned long long)heapwin_lowest_rs(),
                (unsigned long long)heapwin_highest_rs(), (unsigned)wovf,
                wst == 0 ? "PASS" : "FAIL", (unsigned)wchecks);
        if (wovf)
            kprintf("[HEAPWIN] ERROR: the run list OVERFLOWED, so the WHERE answer "
                    "below is incomplete. Raise RUNS in rustkern/heapwin.rs.\n");
        if (wunx != 0)
            kprintf("[HEAPWIN] WARNING: %llu page(s) of USABLE RAM in the heap's "
                    "window (first 0x%llx) were ALREADY allocated and NO reservation "
                    "in this kernel admits to owning them; those are the live "
                    "corruptions this came too late to prevent\n",
                    (unsigned long long)wunx, (unsigned long long)wfirst);
    }
#else
    if (already != 0)
        kprintf("[HEAPSHADOW] WARNING: %llu page(s) of USABLE RAM in the heap's window "
                "were ALREADY allocated when the reservation ran\n",
                (unsigned long long)already);
#endif
    // #heapdbl: firmware-reserved pages inside the window are NOT an alarm, but they
    // are worth naming, because they mean the heap is mapped over addresses the
    // firmware owns and the PMM's managed range reaches the top of the window. A
    // 576 MB VM reports 2299 of these; a 4 GB VM reports none.
    if (fw != 0)
        kprintf("[HEAPSHADOW] %llu page(s) in the window were already used because the "
                "FIRMWARE reserved them (not usable RAM), which is benign: such a page "
                "was never going to be handed out. Reported, not warned about.\n",
                (unsigned long long)fw);
}

// #heapwin: WHERE the already-allocated window pages sit RELATIVE TO THE PART OF
// THE WINDOW THE HEAP ACTUALLY USES. Called once, after the first expansion, so
// heap_end is real.
//
// WHY THE ANSWER IS NOT WHAT THE SURROUNDING COMMENTS SAY. The block above
// heap_reserve_shadowed_window(), and rustkern/heapshadow.rs, both state that
// vmm_map_page() "REPLACES the UEFI identity entries" for the window, so that
// `(uint64_t *)P` for a physical P inside it points into the heap. That is NOT
// what this kernel does. vmm_map_page() (mm/vmm.c:383) walks `kernel_pml4`, a
// static .bss array that vmm_init() memsets to zero and NEVER loads into CR3:
// vmm_init() keeps the firmware's PML4 (current_pml4_phys = g_kernel_cr3 =
// uefi_cr3) forever. So the heap's mapping is INERT for translation, the window
// keeps its UEFI identity mapping, and the heap's arena IS the physical memory at
// the same addresses.
//
// The practical consequence of the #aicappersist bug is IDENTICAL either way, and
// the window reservation is right either way, which is how the wrong mechanism
// survived in the comments: a page the PMM hands out from inside the window is
// heap storage, so clearing it as a page table corrupts the heap. But the WHERE
// question has a different answer under the two mechanisms, so this prints the
// translation it actually measures instead of restating either story.
static void heapwin_report_where(void) {
    uint64_t lowest = heapwin_lowest_rs();
    uint64_t win_end = HEAP_VIRT_BASE + HEAP_MAX_SIZE;
    uint64_t now = heapwin_overlap_rs(heap_start, heap_end);
    uint64_t maxwin = heapwin_overlap_rs(HEAP_VIRT_BASE, win_end);

    kprintf("[HEAPWIN] mapped heap 0x%llx-0x%llx (%llu MB); %llu already-allocated "
            "page(s) fall INSIDE it (must be 0); %llu fall inside the window's "
            "CONFIGURED MAXIMUM 0x%llx-0x%llx (%llu MB)\n",
            (unsigned long long)heap_start, (unsigned long long)heap_end,
            (unsigned long long)(heap_size / MB), (unsigned long long)now,
            (unsigned long long)maxwin, (unsigned long long)HEAP_VIRT_BASE,
            (unsigned long long)win_end,
            (unsigned long long)(HEAP_MAX_SIZE / MB));

    if (now != 0) {
        // The heap is using, as its own storage, physical pages the firmware
        // still owns. Under the identity mapping that is a direct overwrite of a
        // live paging structure (or a #PF, because #procspawn measured those
        // pages as NOT writable from Ring 0). Loud, unconditionally.
        kprintf("[HEAPWIN] HAZARD: %llu page(s) the firmware still owns are inside "
                "the heap's mapped arena (first 0x%llx). The heap will write to "
                "them.\n",
                (unsigned long long)now, (unsigned long long)lowest);
    } else if (maxwin != 0) {
        kprintf("[HEAPWIN] LATENT: those %llu page(s) are ABOVE the mapped heap, in "
                "the inert part of the window, nearest 0x%llx = %llu MB of headroom. "
                "HEAP_MAX_SIZE (%llu MB) would let the heap reach them at %llu MB of "
                "heap; it is at %llu MB.\n",
                (unsigned long long)maxwin, (unsigned long long)lowest,
                (unsigned long long)((lowest - heap_end) / MB),
                (unsigned long long)(HEAP_MAX_SIZE / MB),
                (unsigned long long)((lowest - HEAP_VIRT_BASE) / MB),
                (unsigned long long)(heap_size / MB));
    }

    // The translation, MEASURED, through both hierarchies, so neither story has
    // to be believed. `live` walks the PML4 the CPU is actually using; `dead`
    // walks the one vmm_map_page() writes into.
    {
        uint64_t cr3 = read_cr3();
        uint64_t live = vmm_get_physical_in(cr3, heap_start);
        uint64_t dead = vmm_get_physical(heap_start);
        kprintf("[HEAPWIN] translation of heap base 0x%llx: live CR3 0x%llx -> phys "
                "0x%llx; the kernel_pml4 vmm_map_page() writes -> phys 0x%llx. %s\n",
                (unsigned long long)heap_start, (unsigned long long)cr3,
                (unsigned long long)live, (unsigned long long)dead,
                live == heap_start
                  ? "IDENTITY INTACT: the window is NOT remapped; the heap's arena "
                    "is the physical memory of the same address"
                  : "REMAPPED: the window no longer points at itself");

        // An INDEPENDENT oracle for the same claim, because the line above is
        // only as good as vmm_get_physical_in()'s own walk and the whole point is
        // that two hierarchies disagree. If the window really were remapped to
        // `dead`, then heap_start and `dead` would be THE SAME MEMORY, so a
        // sentinel written through one would be readable through the other.
        // `dead` is far below the window, where identity is not in question.
        //
        // The sentinel goes in the first free block's DATA area (heap_start +
        // 0x800), never its header, and is restored byte for byte.
        if (dead != 0 && dead + 0x800 < HEAP_VIRT_BASE && heap_size > 0x1000) {
            volatile uint64_t *hp = (volatile uint64_t *)(heap_start + 0x800);
            volatile uint64_t *dp = (volatile uint64_t *)(dead + 0x800);
            uint64_t save_h = *hp;
            uint64_t sent = 0x48454150574E0001ULL;  // "HEAPWN" + 1
            uint64_t seen_d_before = *dp;
            *hp = sent;
            uint64_t seen_d_after = *dp;
            *hp = save_h;
            kprintf("[HEAPWIN] sentinel: wrote 0x%llx at heap 0x%llx; the frame "
                    "vmm_map_page() claims (0x%llx) read 0x%llx before and 0x%llx "
                    "after -> %s\n",
                    (unsigned long long)sent,
                    (unsigned long long)(heap_start + 0x800),
                    (unsigned long long)(dead + 0x800),
                    (unsigned long long)seen_d_before,
                    (unsigned long long)seen_d_after,
                    seen_d_after == sent
                      ? "SAME MEMORY: the window IS remapped onto that frame"
                      : "DIFFERENT MEMORY: the heap does NOT live on the frame it "
                        "mapped; that mapping is inert");
        }
    }

#ifdef HEAPWIN_FAULT_TEST
    // #heapwin `make HEAPWINFAULT=1`: the detector above has only ever been
    // watched saying 0, and a detector nobody has seen go red is
    // indistinguishable from one that is not wired up (the NOBLOCKTEST /
    // PTROFAULTTEST argument). This does, ON PURPOSE AT A KNOWN ADDRESS, what a
    // heap grown past the firmware's blocks would do at a random one: it asks
    // the detector about an extent that reaches the lowest recorded page, and
    // then writes a heap free-block header there, exactly as heap_expand_once()
    // writes one at heap_end.
    //
    // #procspawn measured those pages as NOT writable from Ring 0, so the write
    // MUST fault (err=0x3, CR2 = that address). The RED line is printed BEFORE
    // the write so it survives the panic. NEVER for a golden.
    if (lowest != 0) {
        uint64_t hit = heapwin_overlap_rs(heap_start, lowest + PMM_PAGE_SIZE);
        kprintf("[HEAPWIN] FAULTTEST: asked about 0x%llx-0x%llx (the heap grown to "
                "reach the first firmware-owned page): %llu page(s) INSIDE -> "
                "%s\n",
                (unsigned long long)heap_start,
                (unsigned long long)(lowest + PMM_PAGE_SIZE),
                (unsigned long long)hit, hit != 0 ? "RED" : "GREEN");
        kprintf("[HEAPWIN] FAULTTEST: now writing a heap block header to 0x%llx, "
                "which #procspawn says Ring 0 cannot write. Expect #PF err=0x3.\n",
                (unsigned long long)lowest);
        heap_block_t *b = (heap_block_t *)lowest;
        b->magic = HEAP_BLOCK_MAGIC;
        b->flags = 0;
        b->size = PMM_PAGE_SIZE - BLOCK_HEADER_SIZE;
        b->next = NULL;
        b->prev = NULL;
        kprintf("[HEAPWIN] FAULTTEST: the write did NOT fault. That page is "
                "WRITABLE, which contradicts the [PTRO] measurement.\n");
    }
#endif
}

// #deadpml4: THE CEILING SCAN. How high the heap's arena may grow before it
// reaches a page of its window that Ring 0 cannot write, or that is not RAM at
// all.
//
// This is a precondition of not allocating a frame per heap page, not a bonus
// beside it. Until #deadpml4 the heap could not grow much past its initial
// 128 MB because every page cost a frame and the PMM was exhausted; the waste was
// acting as a brake. Reclaim it and HEAP_MAX_SIZE (256 MB) becomes reachable, and
// on a 576 MB VM the firmware's write-protected 2 MB blocks sit at 0x1e800000 and
// 0x1fc00000, i.e. 232 MB and 252 MB in. #procspawn measured those pages as NOT
// writable from Ring 0, so the first heap block header written at one is a
// #PF err=0x3 in the kernel.
//
// It runs in BOTH arms of HEAPSHADOWREVERT, because the ceiling is a property of
// the machine's memory map and not of the reservation, and because a negative
// control that also silently disabled the ceiling would be two changes at once.
// Each page is asked of the OWNING accessor: the firmware memory type the PMM
// itself uses to decide what to free, and vmm_identity_write_refused() (the
// #procspawn range list). Neither is inferred from the address.
static void heap_scan_window_safety(void) {
    uint32_t checks = 0;
    int st = heapframe_selftest_rs(&checks);

    heapframe_reset_rs();
    heapframe_set_window_rs(HEAP_VIRT_BASE, HEAP_MAX_SIZE);
    // The clamp may never leave an arena smaller than the heap boots with today.
    heapframe_set_floor_rs(HEAP_INITIAL_SIZE);
    for (uint64_t p = HEAP_VIRT_BASE; p < HEAP_VIRT_BASE + HEAP_MAX_SIZE;
         p += PMM_PAGE_SIZE) {
        uint32_t ty = pmm_mmap_type_of(p, 0, 0, 0);
        // The same two types pmm_init() frees (mm/pmm.c:167). Anything else is
        // memory the kernel has no business writing, including type 0, which means
        // the address is in no firmware map entry at all.
        int32_t ram = (ty == MEMORY_TYPE_USABLE || ty == MEMORY_TYPE_BOOTLOADER);
        heapframe_note_window_page_rs(p, ram, vmm_identity_write_refused(p));
    }

    uint64_t scanned = 0, ro = 0, notram = 0, first_ro = 0, first_notram = 0;
    heapframe_scan_counts_rs(&scanned, &ro, &notram, &first_ro, &first_notram);
    uint64_t ceil = heapframe_ceiling_rs();
    kprintf("[HEAPFRAME] window safety: %llu page(s) scanned, %llu NOT writable from "
            "Ring 0 (lowest 0x%llx), %llu not writable RAM (lowest 0x%llx); safe "
            "ceiling 0x%llx = %llu MB of heap (cause %d); policy self-test %s "
            "(%u checks)\n",
            (unsigned long long)scanned, (unsigned long long)ro,
            (unsigned long long)first_ro, (unsigned long long)notram,
            (unsigned long long)first_notram, (unsigned long long)ceil,
            (unsigned long long)((ceil - HEAP_VIRT_BASE) / MB),
            (int)heapframe_ceiling_cause_rs(),
            st == 0 ? "PASS" : "FAIL", (unsigned)checks);

    if (heapframe_floor_defeated_rs()) {
        // The measured ceiling is below HEAP_INITIAL_SIZE, so the clamp is OFF and
        // this machine keeps its pre-#deadpml4 behaviour: the arena reaches a page
        // the heap should not be writing. Enforcing the ceiling here would cut the
        // heap to a few MB, and mm/heap.c's own comment says it needs 64 MB+. This
        // needs a fix of its own; it must not be a side effect of the reclaim.
        kprintf("[HEAPFRAME] CEILING BELOW THE INITIAL HEAP: the lowest unsafe page "
                "in the window is 0x%llx, only %llu MB in, but the heap boots with "
                "%llu MB. The clamp is DISABLED on this machine and the arena will "
                "reach that page, exactly as it did before. This is a REAL hazard "
                "that needs its own fix: the heap writes memory the firmware owns.\n",
                (unsigned long long)heapframe_raw_ceiling_rs(),
                (unsigned long long)((heapframe_raw_ceiling_rs() - HEAP_VIRT_BASE) / MB),
                (unsigned long long)(HEAP_INITIAL_SIZE / MB));
    }
}

// #deadpml4: THE COST, PER PAGE, FOR EVERY PAGE OF THE ARENA.
//
// #heapwin established that vmm_map_page()'s hierarchy is not the one in CR3, and
// proved it for the heap's FIRST page with a sentinel. The 128 MB figure then
// rested on "the mechanism is identical for the other 32767", which is a sound
// argument and still not a measurement. This asks, for every page of the mapped
// arena:
//
//   1. does the LIVE hierarchy translate it to itself (so the arena is the
//      identity-mapped physical memory of its own address)?
//   2. what frame does the DEAD hierarchy claim, and is that frame outside the
//      reserved window (so nothing else can ever be given it either)?
//   3. does a value written through the heap's virtual address appear in that
//      frame? A per-page sentinel value, so a stale read cannot pass.
//
// After the reclaim there are no dead mappings left, so dead_none == pages and
// the inert-frame count is 0. That is the whole before/after, on one observable.
static void heapframe_report_cost(void) {
    uint64_t cr3 = read_cr3();
    uint64_t win_end = HEAP_VIRT_BASE + HEAP_MAX_SIZE;

    for (uint64_t v = heap_start; v < heap_end; v += PMM_PAGE_SIZE) {
        uint64_t live = vmm_get_physical_in(cr3, v);
        uint64_t dead = vmm_get_physical(v);
        int32_t sent = -1;

        // The sentinel is only meaningful when the dead hierarchy names a frame
        // OUTSIDE the window: inside it, identity would make the two addresses
        // the same page and the comparison would be vacuously true. The low 1 MB
        // is excluded because nothing legitimate is there and it is the one place
        // a stray read could hit firmware structures.
        if (live == v && dead >= 0x100000 && (dead < HEAP_VIRT_BASE || dead >= win_end)) {
            // v + 0x800 is inside the arena and past the only live object in it
            // (the free-block header at heap_start), and it is restored byte for
            // byte. The sentinel is derived from the address, so no two pages
            // share a value and a stale copy cannot be mistaken for a hit.
            volatile uint64_t *hp = (volatile uint64_t *)(v + 0x800);
            volatile uint64_t *dp = (volatile uint64_t *)(dead + 0x800);
            uint64_t want = 0x48454150574E0000ULL ^ v;
            uint64_t save = *hp;
            uint64_t before = *dp;
            *hp = want;
            uint64_t after = *dp;
            *hp = save;
            if (before == want) {
                sent = -1;   // inconclusive: it was already that value
            } else {
                sent = (after == want) ? 1 : 0;
            }
        }
        heapframe_note_page_rs(v, live, dead, sent);
    }

    uint64_t pages = 0, iok = 0, ibad = 0, fib = 0, dout = 0, din = 0, dnone = 0;
    uint64_t sdiff = 0, ssame = 0, sskip = 0, fss = 0;
    heapframe_page_counts_rs(&pages, &iok, &ibad, &fib, &dout, &din, &dnone,
                             &sdiff, &ssame, &sskip, &fss);
    kprintf("[HEAPFRAME] arena 0x%llx-0x%llx: %llu page(s); live CR3 identity %llu "
            "of %llu (first non-identity 0x%llx, must be 0); dead-hierarchy frames: "
            "%llu outside the window, %llu inside, %llu none\n",
            (unsigned long long)heap_start, (unsigned long long)heap_end,
            (unsigned long long)pages, (unsigned long long)iok,
            (unsigned long long)pages, (unsigned long long)fib,
            (unsigned long long)dout, (unsigned long long)din,
            (unsigned long long)dnone);
    kprintf("[HEAPFRAME] sentinels: %llu DIFFERENT MEMORY, %llu SAME MEMORY (first "
            "0x%llx, must be 0), %llu not tested -> %llu frame(s) = %llu KB "
            "allocated and addressed by NOTHING\n",
            (unsigned long long)sdiff, (unsigned long long)ssame,
            (unsigned long long)fss, (unsigned long long)sskip,
            (unsigned long long)heapframe_inert_frames_rs(),
            (unsigned long long)(heapframe_inert_frames_rs() * PMM_PAGE_SIZE / KB));
    if (ibad != 0 || ssame != 0) {
        // Either would mean the window IS remapped after all, and that the
        // reclaim's premise is false on this machine. Loud, unconditionally.
        kprintf("[HEAPFRAME] HAZARD: the heap's window is NOT purely identity-mapped "
                "on this machine (%llu non-identity page(s), %llu page(s) whose "
                "mapped frame IS their storage). The #deadpml4 reclaim assumes the "
                "opposite. Build with HEAPFRAMESREVERT=1 and investigate.\n",
                (unsigned long long)ibad, (unsigned long long)ssame);
    }
    kprintf("[HEAPFRAME] PMM free after heap_init: %llu of %llu page(s) (%llu of "
            "%llu MB)\n",
            (unsigned long long)pmm_get_free_pages(),
            (unsigned long long)pmm_get_total_pages(),
            (unsigned long long)(pmm_get_free_pages() * PMM_PAGE_SIZE / MB),
            (unsigned long long)(pmm_get_total_pages() * PMM_PAGE_SIZE / MB));
}

void heap_init(void) {
    kprintf("[HEAP] Initializing kernel heap...\n");

    // BEFORE the first expansion: the reservation must precede any allocation
    // that could land in the window, including the heap's own.
    heap_reserve_shadowed_window();

    heap_start = HEAP_VIRT_BASE;
    heap_end = heap_start;
    heap_size = 0;

    // Allocate initial heap space
    if (heap_expand(HEAP_INITIAL_SIZE) != 0) {
        kprintf("[HEAP] ERROR: Failed to allocate initial heap space!\n");
        return;
    }

    kprintf("[HEAP] Heap initialized at 0x%lx - 0x%lx (%lu KB)\n",
            heap_start, heap_end, heap_size / KB);

    heapwin_report_where();
    // #deadpml4: the per-page cost of the arena's mappings, and what the PMM has
    // left once the heap is up. The second number is the whole point of the
    // change, so it is printed whichever arm is built.
    heapframe_report_cost();

#ifdef HEAPFRAME_CLAMP_TEST
    // #deadpml4 `make HEAPFRAMECLAMPTEST=1`: PROVE THE CEILING IS WIRED IN, not
    // merely self-tested.
    //
    // heapframe_selftest_rs() proves the clamp ARITHMETIC on synthetic data, which
    // says nothing about whether heap_expand_once() consults it. And the ceiling
    // can never be reached by accident on a healthy boot: the heap stops at 128 MB
    // because nothing asks for more. So this asks for the whole rest of the window
    // at a known moment and checks WHERE the arena stopped.
    //
    // TWO INDEPENDENT ORACLES, one of which is the pre-existing #heapwin detector:
    //   * heap_end must land EXACTLY on the ceiling, not one page either side.
    //   * heapwin_report_where() must still say LATENT. Build the control arm
    //     (HEAPFRAMENOCLAMP=1) and the same line says HAZARD with 1024 pages the
    //     firmware owns inside the arena, which is the state a write then faults in
    //     (`make HEAPWINFAULT=1` already measured that write as #PF err=0x3 at
    //     0x1e800000).
    //
    // Same argument and the same never-for-a-golden rule as HEAPWINFAULT /
    // NOBLOCKTEST: a guard nobody has watched fire is indistinguishable from one
    // that is not wired up.
    {
        uint64_t ceil = heapframe_ceiling_rs();
        kprintf("[HEAPFRAME] CLAMPTEST: heap_end 0x%llx, ceiling 0x%llx (%llu MB of "
                "headroom). Asking heap_expand_once() for the whole rest of the "
                "window.\n",
                (unsigned long long)heap_end, (unsigned long long)ceil,
                (unsigned long long)((ceil > heap_end) ? (ceil - heap_end) / MB : 0));
        int rc = heap_expand_once(HEAP_MAX_SIZE);
        kprintf("[HEAPFRAME] CLAMPTEST: rc=%d, arena is now 0x%llx-0x%llx (%llu MB) "
                "-> %s\n",
                rc, (unsigned long long)heap_start, (unsigned long long)heap_end,
                (unsigned long long)(heap_size / MB),
                heap_end == ceil
                  ? "GREEN: stopped EXACTLY at the ceiling"
                  : (heap_end > ceil
                       ? "RED: the arena is PAST the ceiling and now contains pages "
                         "Ring 0 cannot write"
                       : "RED: stopped SHORT of the ceiling (the clamp is too strict, "
                         "or the expansion failed for another reason)"));
        int rc2 = heap_expand_once(PMM_PAGE_SIZE);
        kprintf("[HEAPFRAME] CLAMPTEST: one further page: rc=%d -> %s\n", rc2,
                rc2 != 0 ? "GREEN: refused" : "RED: allowed past the ceiling");
        // The #heapwin detector's verdict on the arena this test just created.
        heapwin_report_where();
    }
#endif

    // #heapdbl: the magic=0x0 classifier, proven on synthetic counts (including
    // the negative controls) before it is ever asked about a real refusal.
    {
        uint32_t checks = 0;
        int st = heapzero_selftest_rs(&checks);
        kprintf("[HEAPZERO] magic=0x0 classifier self-test %s (%u checks)\n",
                st == 0 ? "PASS" : "FAIL", (unsigned)checks);
    }
}

// Find a suitable free block (first-fit strategy)
static heap_block_t *find_free_block(size_t size) {
    heap_block_t *block = free_list;

    while (block) {
        if (block->size >= size) {
            return block;
        }
        block = block->next;
    }

    return NULL;
}

// Remove a block from the free list
static void remove_from_free_list(heap_block_t *block) {
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        free_list = block->next;
    }

    if (block->next) {
        block->next->prev = block->prev;
    }

    block->next = NULL;
    block->prev = NULL;
}

// Add a block to the free list (at the beginning)
static void add_to_free_list(heap_block_t *block) {
    block->next = free_list;
    block->prev = NULL;

    if (free_list) {
        free_list->prev = block;
    }

    free_list = block;
}

// Split a block if it's much larger than needed
static void split_block(heap_block_t *block, size_t size) {
    // Only split when the leftover can hold a header + a minimum data block.
    // CRITICAL: guard against size_t underflow on a near-exact fit. If
    // block->size is within BLOCK_HEADER_SIZE of `size`, the old
    // `block->size - size - BLOCK_HEADER_SIZE` wrapped to a ~2^64 value, passed
    // the "large enough" test, and published a bogus giant free block. Later
    // allocations then overlapped live memory (kernel stacks, other buffers),
    // causing #GP / silent corruption (e.g. launching the 315 KB Settings ELF).
    if (block->size < size + BLOCK_HEADER_SIZE + HEAP_MIN_BLOCK_SIZE) {
        return;  // keep the whole block with the allocation (internal frag)
    }

    // remaining = data bytes left after carving `size` + one new header.
    size_t remaining = block->size - size - BLOCK_HEADER_SIZE;

    // Create new block from the remaining space
    heap_block_t *new_block = (heap_block_t *)((uint8_t *)block + BLOCK_HEADER_SIZE + size);
    new_block->magic = HEAP_BLOCK_MAGIC;
    new_block->flags = 0;  // Free
    new_block->size = remaining;   // FIX: was remaining - BLOCK_HEADER_SIZE (leaked a header each split)

    // Update original block size
    block->size = size;

    // Add new block to free list
    add_to_free_list(new_block);
}

// Allocate memory from the heap
void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    // Align size to 16 bytes
    size = (size + 15) & ~15ULL;

    uint64_t irqf = heap_acquire_lock();

    // Find a free block
    heap_block_t *block = find_free_block(size);

    // If no suitable block found, try to expand the heap
    if (!block) {
        if (heap_expand(size + BLOCK_HEADER_SIZE) != 0) {
            heap_release_lock(irqf);
            MM_ANOMALY("[HEAP] kmalloc(%lu) FAILED: the kernel heap is exhausted. Every "
                   "caller that does not check its return now writes through NULL.", size);
        kprintf("[HEAP] ERROR: Out of memory! Requested %lu bytes\n", size);
            return NULL;
        }
        block = find_free_block(size);
        if (!block) {
            heap_release_lock(irqf);
            return NULL;
        }
    }

    // Remove from free list
    remove_from_free_list(block);

    // Split if necessary
    split_block(block, size);

    // Mark as used
    block->flags |= BLOCK_FLAG_USED;

    // Update statistics
    total_allocated += block->size;
    allocation_count++;

    heap_release_lock(irqf);

    // Return pointer to data area (after header)
    return (void *)((uint8_t *)block + BLOCK_HEADER_SIZE);
}

// ===========================================================================
// #dosmem: THE ALIGNED ALLOCATOR HAD NO FREE, AND TWENTY CALL SITES.
//
// FOUND BY MEASUREMENT, not by reading. Promoting mm/'s anomaly lines to
// /BOOTLOG.TXT put this on disk on a HEALTHY 4 GB boot of build 2287:
//
//   [MM#1] [HEAP] kfree() on an INVALID pointer 0x11395300
//          (magic=0x52415453 want 0x48454150) from ra=0x478cdd
//   [MM#2] [HEAP] kfree() on an INVALID pointer 0x11395780
//          (magic=0x0 want 0x48454150) from ra=0x478cf9
//
// addr2line: drivers/hda.c:1015 and :1019, i.e. hda_free_dma_buffers()'s
// kfree(hda_state.bdl) and kfree(hda_state.dma_buffer). Both were allocated
// with kzalloc_aligned(). 0x52415453 is not a corrupted magic, it is the ASCII
// "STAR" - the four bytes that happen to live 16 bytes below an address in the
// MIDDLE of somebody else's allocation, which is exactly where an aligned
// pointer points.
//
// THE MECHANISM: kmalloc_aligned() returned an offset INTO a kmalloc block and
// stashed the real base at aligned[-1], and NOTHING IN THE TREE EVER READ THAT
// BACK. There was no kfree_aligned(), in this header or anywhere else. So every
// one of the twenty kmalloc_aligned/kzalloc_aligned sites (drivers/hda.c,
// drivers/intel_hda.c, drivers/ac97.c, drivers/intel_gpu.c, io/io_ring.c,
// exec/elf.c, video/framebuffer.c) that is ever freed leaks its whole block,
// including a PAGE_SIZE-aligned DMA buffer per audio teardown.
//
// It was ALSO silently inconsistent: `alignment <= 16` returned a plain
// kmalloc() pointer, which kfree() frees correctly. So whether kfree(p) worked
// depended on an alignment argument at a completely different call site. Half
// the callers were right by accident.
//
// THE FIX IS IN THE MECHANISM, NOT THE TWENTY INSTANCES. Adding kfree_aligned()
// and editing twenty call sites is a fix that can be half-applied, and the next
// person to add a kmalloc_aligned() has no way to learn the rule. Instead the
// aligned block now carries a TAG immediately below the pointer it hands out,
// and kfree() itself recognises it. Every existing kfree(aligned_ptr) in the
// tree becomes correct with no caller change, and so does every future one.
// kfree_aligned() exists as an explicit spelling for new code; it is a direct
// alias, not a second implementation.
//
// WHY THE TAG IS SAFE TO TRUST. kfree() consults it ONLY after the ordinary
// block magic has already failed, i.e. only on a pointer it was about to
// reject outright. It then requires all three of: a 64-bit magic, a recorded
// base inside the live heap, and that base's OWN block header carrying
// HEAP_BLOCK_MAGIC. A wild pointer that satisfies all three is not
// distinguishable from a real aligned allocation by any means available here,
// and the probability is negligible; a wild pointer that satisfies none is
// reported exactly as it was before.
// ===========================================================================
#define HEAP_ALIGN_TAG_MAGIC  0x4B414C4E5F544147ULL   // "KALN_TAG"

typedef struct {
    uint64_t magic;     // HEAP_ALIGN_TAG_MAGIC
    void    *raw;       // the kmalloc() base this aligned pointer sits inside
} heap_align_tag_t;

// Sits immediately below the returned pointer. 16 bytes, so an aligned pointer
// stays aligned for every alignment this allocator supports (>= 16).
#define HEAP_ALIGN_TAG_SIZE  ((size_t)sizeof(heap_align_tag_t))

// Allocate aligned memory. See the block comment above for why the result is
// tagged and why kfree() knows about it.
void *kmalloc_aligned(size_t size, size_t alignment) {
    // <= 16 is the natural alignment of every kmalloc() result (BLOCK_HEADER_SIZE
    // and the size rounding are both multiples of 16), so a plain block is
    // already correct AND is already freeable by kfree() with no tag.
    if (alignment <= 16) {
        return kmalloc(size);
    }
    // Reject a non-power-of-two alignment rather than computing nonsense with
    // the ~(alignment-1) mask below. Previously this produced a wrong address
    // silently.
    if (alignment & (alignment - 1)) return NULL;

    // Room to slide up to `alignment-1` bytes AND to carry the tag underneath.
    size_t extra = alignment - 1 + HEAP_ALIGN_TAG_SIZE;
    if (size + extra < size) return NULL;          // overflow
    void *raw = kmalloc(size + extra);
    if (!raw) return NULL;

    uintptr_t aligned = ((uintptr_t)raw + extra) & ~(uintptr_t)(alignment - 1);
    // aligned >= raw + HEAP_ALIGN_TAG_SIZE by construction (the mask can move
    // the address down by at most alignment-1), so the tag is inside our block.
    heap_align_tag_t *tag = (heap_align_tag_t *)(aligned - HEAP_ALIGN_TAG_SIZE);
    tag->magic = HEAP_ALIGN_TAG_MAGIC;
    tag->raw   = raw;

    return (void *)aligned;
}

// Explicit spelling for new code. kfree() handles an aligned pointer on its
// own, so this is an alias and NOT a second implementation: a private copy is
// exactly how this defect would come back.
void kfree_aligned(void *ptr) { kfree(ptr); }

// Recover the kmalloc base from an aligned pointer, or NULL if `ptr` does not
// carry a valid tag. Called ONLY from kfree(), and only after the ordinary
// block magic has failed. Must not fault: it reads at most 16 bytes below a
// pointer the caller already dereferenced (kfree read its block header there),
// and validates everything it finds before returning it.
static void *heap_align_tag_base(void *ptr) {
    if (!ptr) return NULL;
    uintptr_t a = (uintptr_t)ptr;
    if (a < heap_start + HEAP_ALIGN_TAG_SIZE) return NULL;
    if (a >= heap_start + heap_size) return NULL;

    const heap_align_tag_t *tag = (const heap_align_tag_t *)(a - HEAP_ALIGN_TAG_SIZE);
    if (tag->magic != HEAP_ALIGN_TAG_MAGIC) return NULL;

    uintptr_t raw = (uintptr_t)tag->raw;
    if (raw < heap_start + BLOCK_HEADER_SIZE) return NULL;
    if (raw >= a) return NULL;                       // the base must be BELOW us
    if (raw >= heap_start + heap_size) return NULL;

    // Third and strongest check: the recorded base must itself be a live
    // kmalloc block. A wild pointer does not get to free arbitrary memory.
    const heap_block_t *rb = (const heap_block_t *)(raw - BLOCK_HEADER_SIZE);
    if (rb->magic != HEAP_BLOCK_MAGIC) return NULL;
    if (!(rb->flags & BLOCK_FLAG_USED)) return NULL;  // already free: not ours to free again

    return (void *)raw;
}

// Allocate zeroed memory
void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

// Allocate zeroed aligned memory
void *kzalloc_aligned(size_t size, size_t alignment) {
    void *ptr = kmalloc_aligned(size, alignment);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

// Reallocate memory
void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) {
        return kmalloc(new_size);
    }

    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    // Get block header
    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - BLOCK_HEADER_SIZE);

    // Validate block
    if (block->magic != HEAP_BLOCK_MAGIC) {
        MM_ANOMALY("[HEAP] krealloc() on an INVALID pointer: heap metadata corruption "
                   "or a wild pointer");
        invalid_free_count++;
        kprintf("[HEAP] ERROR: krealloc() called on invalid pointer!\n");
        return NULL;
    }

    // If the block is already large enough, just return it
    if (block->size >= new_size) {
        return ptr;
    }

    // Allocate new block and copy data
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) {
        return NULL;
    }

    memcpy(new_ptr, ptr, block->size);
    kfree(ptr);

    return new_ptr;
}

// Merge adjacent free blocks (forward AND backward).
//
// #347: this used to only merge `block` with its next physical neighbor.
// Blocks freed in an order that leaves them physically adjacent to a free
// block that comes BEFORE them in memory never got merged with that
// predecessor, so long-running processes with steady alloc/free churn
// (observed live: a Home Assistant polling client doing an HTTP GET every
// few seconds, hours of uptime) fragment the heap into many small
// unmergeable free blocks. That reproduces exactly what we saw on a live
// serial capture: sustained "[HEAP] ERROR: Out of memory! Requested 1048576
// bytes" (and even 4096-byte) failures despite the heap having plenty of
// free bytes in aggregate - just never as one contiguous run. Worse, any
// caller of kmalloc() that doesn't check for NULL (this codebase has had at
// least one prior heap-corruption bug of this shape - see the split_block()
// underflow comment above, which caused "#GP / silent corruption") turns
// that OOM into a kernel-mode NULL/bad-pointer dereference, which is caught
// by the fault handler and converted into a permanent cli+hlt of that core
// (cpu/idt.c isr_handler_impl) - fatal because interrupts (the timer tick)
// never resume, and the whole box appears to "hang idle forever".
//
// There's no back-pointer to the physical predecessor in heap_block_t (that
// would need a footer/boundary tag, a bigger layout change), so scan the
// free list for a free block whose end address equals `block`'s start. This
// is O(free-list length) but only runs on the kfree() path, and correctness
// against fragmentation matters far more than micro-optimizing free().
// Returns the block that should actually be added to the free list (it may
// be the absorbing predecessor rather than `block` itself).
static heap_block_t *coalesce_blocks(heap_block_t *block) {
    // Try to merge with the next physical block.
    heap_block_t *next = (heap_block_t *)((uint8_t *)block + BLOCK_HEADER_SIZE + block->size);
    if ((uint64_t)next < heap_end &&
        next->magic == HEAP_BLOCK_MAGIC &&
        !(next->flags & BLOCK_FLAG_USED)) {
        remove_from_free_list(next);
        block->size += BLOCK_HEADER_SIZE + next->size;
        next->magic = 0;
    }

    // Try to merge with the previous physical block.
    for (heap_block_t *prev = free_list; prev; prev = prev->next) {
        if (prev == block) continue;
        uint64_t prev_end = (uint64_t)prev + BLOCK_HEADER_SIZE + prev->size;
        if (prev_end == (uint64_t)block) {
            remove_from_free_list(prev);
            prev->size += BLOCK_HEADER_SIZE + block->size;
            block->magic = 0;
            return prev;
        }
    }

    return block;
}

// ===========================================================================
// #heapdbl: WHICH of the two causes zeroed this header? See mm/heapzero.h and
// rustkern/heapzero.rs.
//
// The message above prints `magic=0x0` for two completely different defects and
// blame.md recorded only one of them, so a whole investigation went looking for
// a double free that was not there. `coalesce_blocks()` sets ONE u32 to zero;
// a 4 KB clear written through a shadowed physical==virtual pointer
// (#aicappersist) sets 4096 bytes to zero. Count, and say which.
//
// SAFETY. kfree() can be handed any pointer at all, so this reads NOTHING until
// the pointer is known to be inside the live heap, and every length is clamped
// to heap_end. block->size is deliberately not consulted: its bytes are inside
// the damage being classified.
// ===========================================================================
static void heap_classify_zero_header(void *ptr) {
    uint64_t p = (uint64_t)ptr;
    if (p < heap_start + BLOCK_HEADER_SIZE || p >= heap_end) return;

    uint64_t page = p & ~((uint64_t)PMM_PAGE_SIZE - 1);
    if (page < heap_start) page = heap_start;
    uint32_t page_len = (uint32_t)PMM_PAGE_SIZE;
    if (page + page_len > heap_end) page_len = (uint32_t)(heap_end - page);

    uint32_t hdr_len  = (uint32_t)BLOCK_HEADER_SIZE;
    uint32_t data_len = 64;
    if (p + data_len > heap_end) data_len = (uint32_t)(heap_end - p);

    uint32_t hz = heapzero_count_zeros_rs((const uint8_t *)(p - BLOCK_HEADER_SIZE), hdr_len);
    uint32_t dz = heapzero_count_zeros_rs((const uint8_t *)p, data_len);
    uint32_t pz = heapzero_count_zeros_rs((const uint8_t *)page, page_len);
    // The DECISIVE one: a 4 KB memset leaves no valid block header in the page.
    uint32_t pm = heapzero_count_magics_rs((const uint8_t *)page, page_len,
                                           HEAP_BLOCK_MAGIC);
    uint32_t v  = heapzero_verdict_rs(hz, hdr_len, dz, data_len, pz, page_len, pm);

    // Cross-check from an INDEPENDENT source: was the heap's virtual window ever
    // reserved in the PMM on this boot? If resnew is 0 the #aicappersist
    // mechanism is live and a WIPE verdict has a mechanism to point at; if it is
    // non-zero the mechanism is closed and a WIPE verdict means something new.
    uint64_t resnew = 0, already = 0, moot = 0, viol = 0;
    heapshadow_counts_rs(&resnew, &already, &moot, &viol);

    MM_ANOMALY("[HEAPZERO] %p hdr=%u/%u data=%u/%u page=0x%llx pagezeros=%u/%u "
               "pagemagics=%u shadow_reserved=%llu shadow_already=%llu "
               "shadow_violations=%llu -> %s",
               ptr, hz, hdr_len, dz, data_len, (unsigned long long)page,
               pz, page_len, pm, (unsigned long long)resnew,
               (unsigned long long)already, (unsigned long long)viol,
               heapzero_verdict_name_rs(v));
}

// Free allocated memory
void kfree(void *ptr) {
    if (!ptr) return;

    uint64_t irqf = heap_acquire_lock();

    // Get block header
    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - BLOCK_HEADER_SIZE);

    // Validate block
    if (block->magic != HEAP_BLOCK_MAGIC) {
        heap_release_lock(irqf);
        // #dosmem: before calling this a bad pointer, ask whether it is an
        // ALIGNED one. See the block comment above kmalloc_aligned(). The lock
        // is already released, so the recursive kfree() below is a normal
        // acquire, not a re-entry.
        {
            void *base = heap_align_tag_base(ptr);
            if (base) {
                static int reported = 0;
                if (!reported) {
                    reported = 1;
                    MM_ANOMALY("[HEAP] aligned-free path ENGAGED: kfree(%p) recovered the "
                               "kmalloc base %p from its tag and freed that instead. Before "
                               "this existed the whole block LEAKED and the call was "
                               "reported as an invalid free. Reported once per boot.",
                               ptr, base);
                }
                kfree(base);
                return;
            }
        }
        // #dosmem: NAME THE CALLER. "There is an invalid free somewhere" is not
        // actionable; an image offset an addr2line can resolve is. This line is
        // MEASURED to fire twice on a HEALTHY 4 GB boot of golden 2286 + this
        // instrument (/BOOTLOG.TXT: 0x11201a00 and 0x11201f00, 1280 bytes
        // apart), which means the kernel heap has a live pointer defect that
        // nobody could see, because kprintf goes to serial and the owner's two
        // machines have no serial port. The magic word actually found is
        // printed too: 0 says the header was zeroed (a double free that already
        // coalesced, or a freed-then-reused block), anything else says the
        // header was overwritten with data, and those are different bugs.
        invalid_free_count++;
        MM_ANOMALY("[HEAP] kfree() on an INVALID pointer %p (magic=0x%x want 0x%x) "
                   "from ra=%p - addr2line this against kernel.dbg.elf",
                   ptr, block->magic, HEAP_BLOCK_MAGIC,
                   __builtin_return_address(0));
        kprintf("[HEAP] ERROR: kfree() called on invalid pointer 0x%p!\n", ptr);
        // #heapdbl: and SAY WHICH defect zeroed it, when it reads as zero.
        if (block->magic == 0) heap_classify_zero_header(ptr);
        return;
    }

    if (!(block->flags & BLOCK_FLAG_USED)) {
        heap_release_lock(irqf);
        MM_ANOMALY("[HEAP] DOUBLE FREE at %p (size=%lu) from ra=%p - addr2line "
                   "this against kernel.dbg.elf", ptr, (unsigned long)block->size,
                   __builtin_return_address(0));
        double_free_count++;
        kprintf("[HEAP] ERROR: Double free detected at 0x%p!\n", ptr);
        return;
    }

    // Mark as free
    block->flags &= ~BLOCK_FLAG_USED;

    // Update statistics
    total_freed += block->size;
    free_count++;

    // Try to coalesce with adjacent blocks (forward and backward)
    block = coalesce_blocks(block);

    // Add to free list
    add_to_free_list(block);

    heap_release_lock(irqf);
}

// Get heap statistics
size_t heap_get_total_size(void) {
    return heap_size;
}

size_t heap_get_used_size(void) {
    return total_allocated - total_freed;
}

// #heapinvptr: see the counters' declaration for why these exist.
uint64_t heap_invalid_free_count(void) { return invalid_free_count; }
uint64_t heap_double_free_count(void)  { return double_free_count; }

size_t heap_get_free_size(void) {
    return heap_size - heap_get_used_size();
}

// Print heap statistics
void heap_print_stats(void) {
    kprintf("[HEAP] Heap Statistics:\n");
    kprintf("  Total size:  %lu KB\n", heap_size / KB);
    kprintf("  Used:        %lu KB\n", heap_get_used_size() / KB);
    kprintf("  Free:        %lu KB\n", heap_get_free_size() / KB);
    kprintf("  Allocations: %lu\n", allocation_count);
    kprintf("  Frees:       %lu\n", free_count);
}
