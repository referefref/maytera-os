// pmm.c - Physical Memory Manager implementation
// Uses a bitmap to track 4KB page frames

#include "pmm.h"
#include "../sync/spinlock.h"   // #745 (#75): the SHARED irqsave spinlock
#include "../proc/schedrace.h"  // #745 (#75): SR_SITE_PMM_HOLD reproducer
#include "../boot_info.h"
#include "../cpu/scprof.h"    // #121: allocator phase attribution
#include "../serial.h"

// Linker-provided symbols for kernel memory bounds
extern char __text_start, __rodata_end, __bss_end, __kernel_end;
#include "../string.h"

// Bitmap for tracking page allocation
// Each bit represents one 4KB page frame
// 1 = free, 0 = used (inverted for easier free page search with bsf)
// Actually, let's use: 0 = free, 1 = used (more intuitive)

// Maximum supported physical memory: 64 GB (16M pages)
#define PMM_MAX_PAGES (16ULL * 1024 * 1024)
#define PMM_BITMAP_SIZE (PMM_MAX_PAGES / 8)  // 2 MB bitmap

// Page frame bitmap (statically allocated for now)
// In a more complete implementation, we'd allocate this dynamically
static uint8_t page_bitmap[PMM_BITMAP_SIZE] __attribute__((aligned(4096)));

// Statistics
static uint64_t total_pages = 0;
static uint64_t free_pages = 0;
static uint64_t used_pages = 0;

// DEBUG: provided by vmm.c. Returns a user VA in the live address space that
// maps `phys`, or 0. Used to detect alloc/free of pages still live in the
// running process (use-after-free). Set g_pmm_freewatch=1 to enable.
extern uint64_t vmm_dbg_user_va_for_phys(uint64_t phys);
int g_pmm_freewatch = 0;  // debug detector disabled (syscall reentrancy bug fixed in build 29)

// Memory bounds
static uint64_t memory_start = 0;  // First usable page
static uint64_t memory_end = 0;    // Last usable page + 1

// #647: the boot memory map, kept so later code can ask what TYPE a physical
// address was given. pmm_init() used to consume it and drop it, which meant the
// only record of "where did this page come from" died with the function.
static memory_map_entry_t *g_mmap = 0;
static uint32_t g_mmap_entries = 0;
static uint64_t g_phys_limit = 0;   // one past the highest address in the map

// #745 (#75) THE PHYSICAL ALLOCATOR'S LOCK MUST MASK INTERRUPTS.
//
// This was a hand-rolled `volatile int` + test-and-set spin that left
// RFLAGS.IF ALONE. That is the same defect #347 fixed in mm/heap.c and never
// carried across to the physical allocator sitting next to it, and on two
// scheduling cores it is not a self-deadlock but an AB-BA deadlock with the
// Big Kernel Lock:
//
//   cpu A: takes pmm_lock with IF=1 and WITHOUT the BKL, is then interrupted;
//          cpu/idt.c wraps every ISR in bkl_acquire(), so A now spins for the
//          BKL WHILE HOLDING pmm_lock.
//   cpu B: is inside a SYSCALL, which took the BKL at syscall_entry, and calls
//          pmm_alloc_page(); it spins for pmm_lock WHILE HOLDING the BKL.
//
// Neither can ever proceed, both spin with interrupts ENABLED, and no core is
// halted - which is why "the owner stops taking interrupts" and "a core halted
// holding the lock" were both the wrong picture.
//
// MEASURED, build 1878 on throwaway VM <vmid>, gate ON, six QMP samples over
// 30 s with every value identical:
//   bkl_owner=1 bkl_depth=1 bkl_word=1        (cpu1 owns the BKL)
//   cpu0 RIP=0x45d9c2 -> bkl_take_locked   cpu/smp.c:1112   HLT=0 IF=1
//   cpu1 RIP=0x456ea2 -> pmm_acquire_lock  mm/pmm.c:66      HLT=0 IF=1
//   pmm_lock=1  holder_cpu=0  holder_if=1  holder_bkl=0
//   holder_ra=0x4574b6 -> pmm_alloc_page   mm/pmm.c:300
//   ctxsw frozen at 48, both run queues empty, g_haltbkl all zero.
//
// The fix is the one the heap already has, taken through the SHARED primitive
// rather than a third private copy: mask IF for the whole hold. A holder can
// then never be interrupted, so it can never become a BKL waiter, and the only
// remaining order is BKL -> pmm_lock, which is total.
//
// The critical sections here are a bitmap scan and a few counters. They are
// bounded and short; this is not a place where masking interrupts costs.
static spinlock_t pmm_lock = SPINLOCK_INIT_NAMED("pmm");

// #745 (#75) PMM-HOLDER FORENSICS, kept. These two stores are what turned the
// wedge from a story into a measurement: read out of guest memory over QMP they
// name the core holding this lock and the call that took it, which is how the
// deadlock partner above was identified rather than guessed. Two per-hold
// stores on a path that already does an atomic read-modify-write.
volatile int32_t  g_pmm_holder_cpu = -1;
volatile uint64_t g_pmm_holder_ra  = 0;

static uint64_t pmm_acquire_lock(void) {
    uint64_t irqf = spinlock_acquire_irqsave(&pmm_lock);
    { extern uint32_t smp_this_cpu(void);
      g_pmm_holder_cpu = (int32_t)smp_this_cpu();
      g_pmm_holder_ra  = (uint64_t)__builtin_return_address(0); }
    // #745 (#75) REPRODUCER, `make SCHEDRACE=1` only, compiled out otherwise:
    // hold this lock open so the other core has time to arrive. Reuses #75's own
    // race-widening primitive rather than a private delay (SR_SITE_PMM_HOLD).
    //
    // ONE IN 64, not every allocation. A boot allocates tens of thousands of
    // pages; 40 us on each would make the reproducer arm slow enough to fail the
    // harness for a reason that is not the deadlock, and a confounded arm proves
    // nothing. One in 64 still opens thousands of windows per boot.
#ifdef SCHEDRACE
    { static volatile uint32_t sr_n; if ((++sr_n & 63u) == 0u) schedrace_delay(SR_SITE_PMM_HOLD); }
#endif
    return irqf;
}

static void pmm_release_lock(uint64_t irqf) {
    g_pmm_holder_cpu = -1;
    g_pmm_holder_ra  = 0;
    spinlock_release_irqrestore(&pmm_lock, irqf);
}

// Set a page as used in the bitmap
static inline void bitmap_set(uint64_t page) {
    if (page < PMM_MAX_PAGES) {
        page_bitmap[page / 8] |= (1 << (page % 8));
    }
}

// Set a page as free in the bitmap
static inline void bitmap_clear(uint64_t page) {
    if (page < PMM_MAX_PAGES) {
        page_bitmap[page / 8] &= ~(1 << (page % 8));
    }
}

// Check if a page is used
static inline int bitmap_test(uint64_t page) {
    if (page >= PMM_MAX_PAGES) return 1;  // Out of range = used
    return (page_bitmap[page / 8] >> (page % 8)) & 1;
}

// Initialize the physical memory manager
void pmm_init(uint64_t mem_map_addr, uint32_t mem_map_entries) {
    kprintf("[PMM] Initializing physical memory manager...\n");

    // Initially mark all pages as used
    memset(page_bitmap, 0xFF, sizeof(page_bitmap));

    memory_map_entry_t *entries = (memory_map_entry_t *)mem_map_addr;

    // #647: retain the map. See g_mmap above.
    g_mmap = entries;
    g_mmap_entries = mem_map_entries;
    for (uint32_t i = 0; i < mem_map_entries; i++) {
        uint64_t top = entries[i].base + entries[i].length;
        if (top > g_phys_limit) g_phys_limit = top;
    }

    // First pass: find total memory and mark usable regions as free
    for (uint32_t i = 0; i < mem_map_entries; i++) {
        memory_map_entry_t *entry = &entries[i];

        // Skip non-usable memory
        if (entry->type != MEMORY_TYPE_USABLE &&
            entry->type != MEMORY_TYPE_BOOTLOADER) {
            continue;
        }

        uint64_t base = entry->base;
        uint64_t length = entry->length;

        // Skip memory below 1 MB (reserved for legacy)
        if (base < 0x100000) {
            if (base + length <= 0x100000) {
                continue;
            }
            length -= (0x100000 - base);
            base = 0x100000;
        }

        // Skip memory above our bitmap range (64 GB)
        if (base >= PMM_MAX_PAGES * PMM_PAGE_SIZE) {
            continue;
        }

        // Clamp length to our maximum
        if (base + length > PMM_MAX_PAGES * PMM_PAGE_SIZE) {
            length = PMM_MAX_PAGES * PMM_PAGE_SIZE - base;
        }

        // Align to page boundaries
        uint64_t start_page = (base + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        uint64_t end_page = (base + length) / PMM_PAGE_SIZE;

        if (end_page <= start_page) {
            continue;
        }

        // Mark pages as free
        for (uint64_t page = start_page; page < end_page; page++) {
            bitmap_clear(page);
            free_pages++;
        }

        // Track memory bounds
        if (memory_start == 0 || start_page < memory_start) {
            memory_start = start_page;
        }
        if (end_page > memory_end) {
            memory_end = end_page;
        }
        
    }

    // Limit to identity-mapped range (2GB max)
    // The kernel assumes physical = virtual, so we can only access
    // physical memory that is identity-mapped by UEFI
    #define PMM_IDENTITY_MAP_LIMIT (0x80000000ULL / PMM_PAGE_SIZE)
    if (memory_end > PMM_IDENTITY_MAP_LIMIT) {
        // Re-mark pages above limit as used
        for (uint64_t page = PMM_IDENTITY_MAP_LIMIT; page < memory_end; page++) {
            if (!bitmap_test(page)) {
                bitmap_set(page);
                free_pages--;
            }
        }
        memory_end = PMM_IDENTITY_MAP_LIMIT;
        kprintf("[PMM] Limited to 2GB identity-mapped range\n");
    }

    total_pages = free_pages;

    // Reserve legacy kernel region (0x100000-0x400000)
    // The UEFI bootloader may have placed data here during boot
    for (uint64_t page = 0x100000 / PMM_PAGE_SIZE; page < 0x400000 / PMM_PAGE_SIZE; page++) {
        if (!bitmap_test(page)) {
            bitmap_set(page);
            free_pages--;
            used_pages++;
        }
    }

    // Reserve pages used by the kernel text + rodata
    uint64_t ktxt_start = (uint64_t)&__text_start / PMM_PAGE_SIZE;
    uint64_t ktxt_end = ((uint64_t)&__rodata_end + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    for (uint64_t page = ktxt_start; page < ktxt_end; page++) {
        if (!bitmap_test(page)) {
            bitmap_set(page);
            free_pages--;
            used_pages++;
        }
    }

    // Reserve pages used by the kernel data + BSS (separate region at 0x2000000)
    extern char __data_start;
    uint64_t kdata_start_page = (uint64_t)&__data_start / PMM_PAGE_SIZE;
    uint64_t kdata_end_page = ((uint64_t)&__kernel_end + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    if (kdata_start_page > ktxt_end) {
        for (uint64_t page = kdata_start_page; page < kdata_end_page; page++) {
            if (!bitmap_test(page)) {
                bitmap_set(page);
                free_pages--;
                used_pages++;
            }
        }
    }

    // Reserve bitmap itself if it falls in usable memory
    uint64_t bitmap_start = (uint64_t)page_bitmap;
    uint64_t bitmap_end = bitmap_start + sizeof(page_bitmap);
    uint64_t bitmap_start_page = bitmap_start / PMM_PAGE_SIZE;
    uint64_t bitmap_end_page = (bitmap_end + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    for (uint64_t page = bitmap_start_page; page < bitmap_end_page; page++) {
        if (page < PMM_MAX_PAGES && !bitmap_test(page)) {
            bitmap_set(page);
            free_pages--;
            used_pages++;
        }
    }

    kprintf("[PMM] Memory range: 0x%lx - 0x%lx\n",
            memory_start * PMM_PAGE_SIZE, memory_end * PMM_PAGE_SIZE);
    kprintf("[PMM] Total pages: %lu (%lu MB)\n",
            total_pages, (total_pages * PMM_PAGE_SIZE) / MB);
    kprintf("[PMM] Free pages: %lu (%lu MB)\n",
            free_pages, (free_pages * PMM_PAGE_SIZE) / MB);
    kprintf("[PMM] Used pages: %lu (%lu MB)\n",
            used_pages, (used_pages * PMM_PAGE_SIZE) / MB);
}

#ifdef PTWALK_SELFTEST
// #647 IN-SERVICE detector. DEBUG BUILDS ONLY (`make PTWALKTEST=1`).
//
// The boot self-test proves what the allocator WOULD do if drained. This proves
// what it ACTUALLY does under the real workload, which is the question that
// matters for #606 and #483: those bugs happen 20 seconds in, not at exhaustion.
// Armed only after the boot self-test finishes, so the test's own drains do not
// spam. g_pmm_hwm is the highest page ever handed out, which says how close the
// live system gets to the table region even when it never reaches it.
extern int vmm_is_boot_page_table(uint64_t phys);
int g_pmm_live_detect = 0;
uint64_t g_pmm_hwm = 0;

static void pmm_live_check(uint64_t pa, uint64_t count, uint64_t ret) {
    if (!g_pmm_live_detect) return;
    uint64_t top = pa + count * PMM_PAGE_SIZE;
    if (top > g_pmm_hwm) {
        // Report only when the mark crosses a 16MB boundary. Printing per PAGE
        // emits a serial line every 4KB, which floods the port badly enough to
        // dominate boot timing: the first version of this instrument left the
        // allocator at 9MB after 140 seconds of uptime, i.e. it was measuring
        // its own output rather than the system.
        if ((top >> 24) != (g_pmm_hwm >> 24)) {
            kprintf("[PT647-HWM] allocator high-water mark 0x%lx (%lu MB)\n",
                    top, top / MB);
        }
        g_pmm_hwm = top;
    }
    for (uint64_t i = 0; i < count; i++) {
        if (vmm_is_boot_page_table(pa + i * PMM_PAGE_SIZE)) {
            kprintf("[PT647-LIVE] *** ALLOC OF LIVE PAGE TABLE 0x%lx "
                    "(block 0x%lx x%lu) caller=0x%lx ***\n",
                    pa + i * PMM_PAGE_SIZE, pa, count, ret);
        }
    }
}
#endif

// Allocate a single physical page
static uint64_t pmm_alloc_page_inner(void);
uint64_t pmm_alloc_page(void) {
    scp_span_t __sp = scp_begin();   // #121
    uint64_t __r = pmm_alloc_page_inner();
    scp_end(SCP_PMMALLOC, __sp);
    return __r;
}
// #121: START THE SCAN WHERE THE LAST ONE FINISHED.
//
// MEASURED on build 1899, VM <vmid>, one 300 s run to DESKTOP_READY: the single
// longest UNBROKEN syscall in the system is SYS_SPAWN at 450207 us, and
// 423261 us of it - 94% - is inside this function. #118 reported that hold as a
// 446 ms Big Kernel Lock hold and #121 was opened to narrow the lock; the lock
// is not what makes it long. The work inside it is, and almost all of the work
// is here.
//
// WHY IT WAS SLOW. The loop below started at memory_start on EVERY call and
// walked the bitmap one page at a time until it found a free bit. Allocating N
// pages therefore costs O(N * used_pages), and it does so with the PMM lock
// held, which is spinlock_acquire_irqsave: interrupts are OFF for the whole
// scan. A spawn allocates the user stack and then every ELF segment page, which
// is why the cost showed up as one enormous critical section and why it grows
// with uptime rather than with the size of the binary.
//
// THE HINT VISITS THE SAME PAGES, IN A ROTATED ORDER. It starts at the page
// after the last successful allocation and wraps once, so the set of pages
// examined before giving up is EXACTLY the same set as before: this can never
// fail where the old loop would have succeeded, which is the only property that
// matters for an allocator. It is next-fit rather than first-fit, so a page
// freed below the cursor is reused after the wrap instead of immediately; that
// is standard and costs nothing that was being relied on. pmm_alloc_pages()
// (the CONTIGUOUS allocator, used for DMA buffers) is deliberately NOT changed
// and still scans from memory_start, so nothing about contiguity moves.
//
// C, not Rust, and stating the reason as the rule requires: this is not new
// code. It is a minimal in-place change to an existing hot-path C function,
// entangled with this file's static bitmap, its page counters and its irqsave
// lock. Porting the physical allocator to Rust is a real piece of work with a
// real risk budget and it is not this ticket.
static uint64_t alloc_hint = 0;

static uint64_t pmm_alloc_page_inner(void) {
    uint64_t irqf = pmm_acquire_lock();

    // Search for a free page, starting at the hint and wrapping once.
    uint64_t span  = memory_end - memory_start;
    uint64_t start = alloc_hint;
    if (start < memory_start || start >= memory_end) start = memory_start;
    for (uint64_t n = 0; n < span; n++) {
        uint64_t page = start + n;
        if (page >= memory_end) page -= span;
        if (!bitmap_test(page)) {
            bitmap_set(page);
            free_pages--;
            used_pages++;
            alloc_hint = (page + 1 < memory_end) ? (page + 1) : memory_start;
            pmm_release_lock(irqf);
            uint64_t pa = page * PMM_PAGE_SIZE;
#ifdef PTWALK_SELFTEST
            pmm_live_check(pa, 1, (uint64_t)__builtin_return_address(0));
#endif
            if (g_pmm_freewatch) {
                // If the page we just handed out is STILL mapped live (USER) in
                // the current address space, it was freed while in use: a
                // use-after-free that will corrupt the running process when the
                // new owner writes/zeroes it.
                uint64_t live_va = vmm_dbg_user_va_for_phys(pa);
                if (live_va) {
                    kprintf("[PMM-WATCH] ALLOC of LIVE user page phys=0x%lx va=0x%lx ret=0x%lx\n",
                            pa, live_va, (uint64_t)__builtin_return_address(0));
                }
            }
            return pa;
        }
    }

    pmm_release_lock(irqf);
    kprintf("[PMM] ERROR: Out of physical memory!\n");
    return 0;
}

// Allocate multiple contiguous physical pages
uint64_t pmm_alloc_pages(uint64_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_page();

    uint64_t irqf = pmm_acquire_lock();

    // Search for contiguous free pages
    uint64_t start_page = memory_start;
    while (start_page + count <= memory_end) {
        // Check if all pages in this range are free
        int all_free = 1;
        for (uint64_t i = 0; i < count; i++) {
            if (bitmap_test(start_page + i)) {
                all_free = 0;
                start_page += i + 1;  // Skip to after the used page
                break;
            }
        }

        if (all_free) {
            // Mark all pages as used
            for (uint64_t i = 0; i < count; i++) {
                bitmap_set(start_page + i);
            }
            free_pages -= count;
            used_pages += count;
            pmm_release_lock(irqf);
#ifdef PTWALK_SELFTEST
            pmm_live_check(start_page * PMM_PAGE_SIZE, count,
                           (uint64_t)__builtin_return_address(0));
#endif
            return start_page * PMM_PAGE_SIZE;
        }
    }

    pmm_release_lock(irqf);
    kprintf("[PMM] ERROR: Cannot allocate %lu contiguous pages!\n", count);
    return 0;
}

// Free a physical page
void pmm_free_page(uint64_t phys_addr) {
    uint64_t page = phys_addr / PMM_PAGE_SIZE;

    if (page < memory_start || page >= memory_end) {
        kprintf("[PMM] WARNING: Attempt to free invalid page 0x%lx\n", phys_addr);
        return;
    }

    if (g_pmm_freewatch) {
        uint64_t live_va = vmm_dbg_user_va_for_phys(phys_addr);
        if (live_va) {
            kprintf("[PMM-WATCH] FREE of LIVE user page phys=0x%lx va=0x%lx ret=0x%lx\n",
                    phys_addr, live_va,
                    (uint64_t)__builtin_return_address(0));
        }
    }

    uint64_t irqf = pmm_acquire_lock();

    if (!bitmap_test(page)) {
        pmm_release_lock(irqf);
        kprintf("[PMM] WARNING: Double free of page 0x%lx\n", phys_addr);
        return;
    }

    bitmap_clear(page);
    free_pages++;
    used_pages--;

    pmm_release_lock(irqf);
}

// Free multiple contiguous physical pages
void pmm_free_pages(uint64_t phys_addr, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        pmm_free_page(phys_addr + i * PMM_PAGE_SIZE);
    }
}

// Get statistics
uint64_t pmm_get_total_pages(void) {
    return total_pages;
}

uint64_t pmm_get_free_pages(void) {
    return free_pages;
}

uint64_t pmm_get_used_pages(void) {
    return used_pages;
}

// ===========================================================================
// #647: reservation of memory that is LIVE but has no linker symbol.
//
// pmm_init() re-reserves only what it can name: kernel text/rodata, the
// __data_start..__kernel_end region, the bitmap, and the legacy 1-4MB window.
// Anything live that is reachable only through a HARDWARE REGISTER (CR3, and in
// principle GDTR/IDTR) is invisible to it. vmm_init() adopts UEFI's page tables
// and never switches away from them, so the entire paging hierarchy is exactly
// that kind of object. These entry points let the owner of such an object hand
// it to the PMM after the fact.
// ===========================================================================

// Mark one page as USED. Idempotent. Returns:
//    1  newly reserved (it was free, i.e. it WOULD have been handed out)
//    0  already marked used (nothing to do)
//   -1  outside the range the allocator will ever consider, so moot
int pmm_reserve_page(uint64_t phys_addr) {
    uint64_t page = phys_addr / PMM_PAGE_SIZE;

    if (page < memory_start || page >= memory_end) {
        return -1;
    }

    uint64_t irqf = pmm_acquire_lock();
    if (bitmap_test(page)) {
        pmm_release_lock(irqf);
        return 0;
    }
    bitmap_set(page);
    free_pages--;
    used_pages++;
    pmm_release_lock(irqf);
    return 1;
}

// Is this page currently allocatable? 1 = free (would be handed out), 0 = not.
// Pages outside [memory_start, memory_end) report 0: the allocator's scan never
// reaches them.
int pmm_page_is_free(uint64_t phys_addr) {
    uint64_t page = phys_addr / PMM_PAGE_SIZE;
    if (page < memory_start || page >= memory_end) return 0;
    return !bitmap_test(page);
}

// #647 diagnostic: which boot memory-map entry does this physical address fall
// in, and what type did the bootloader give it? Returns the MEMORY_TYPE_* value
// or 0 if no entry covers it. Optionally reports the entry index and extent.
uint32_t pmm_mmap_type_of(uint64_t phys_addr, uint32_t *out_index,
                          uint64_t *out_base, uint64_t *out_len) {
    for (uint32_t i = 0; i < g_mmap_entries; i++) {
        if (phys_addr >= g_mmap[i].base &&
            phys_addr < g_mmap[i].base + g_mmap[i].length) {
            if (out_index) *out_index = i;
            if (out_base)  *out_base = g_mmap[i].base;
            if (out_len)   *out_len = g_mmap[i].length;
            return g_mmap[i].type;
        }
    }
    return 0;
}

uint64_t pmm_phys_limit(void) { return g_phys_limit; }

const char *pmm_mmap_type_name(uint32_t type) {
    switch (type) {
        case MEMORY_TYPE_USABLE:           return "USABLE(EfiConventional|EfiBootServicesCode|EfiBootServicesData)";
        case MEMORY_TYPE_RESERVED:         return "RESERVED";
        case MEMORY_TYPE_ACPI_RECLAIMABLE: return "ACPI_RECLAIMABLE";
        case MEMORY_TYPE_ACPI_NVS:         return "ACPI_NVS";
        case MEMORY_TYPE_BAD:              return "BAD";
        case MEMORY_TYPE_BOOTLOADER:       return "BOOTLOADER(EfiLoaderCode|EfiLoaderData)";
        case MEMORY_TYPE_KERNEL:           return "KERNEL";
        case MEMORY_TYPE_FRAMEBUFFER:      return "FRAMEBUFFER";
        default:                           return "<not in map>";
    }
}

// #647 diagnostic: dump the whole boot memory map. Adjacency matters when
// reading it: the bootloader copies UEFI descriptors 1:1 and never merges them,
// so TWO ABUTTING ENTRIES OF THE SAME converted type were necessarily DIFFERENT
// raw UEFI types (UEFI itself would have merged them otherwise). That is the
// only signal we have about the raw type, because convert_memory_type() in the
// bootloader collapses three raw types into MEMORY_TYPE_USABLE and the raw value
// is not carried anywhere in memory_map_entry_t.
void pmm_dump_mmap(void) {
    kprintf("[PMM-MAP] %u entries, phys_limit=0x%lx\n", g_mmap_entries, g_phys_limit);
    for (uint32_t i = 0; i < g_mmap_entries; i++) {
        uint64_t base = g_mmap[i].base;
        uint64_t len = g_mmap[i].length;
        int abuts_prev = (i > 0) &&
            (g_mmap[i - 1].base + g_mmap[i - 1].length == base) &&
            (g_mmap[i - 1].type == g_mmap[i].type);
        kprintf("[PMM-MAP] %3u 0x%012lx..0x%012lx %8lu KB type=%u attr=0x%x%s\n",
                i, base, base + len, len / 1024, g_mmap[i].type,
                g_mmap[i].attributes,
                abuts_prev ? "  <-SPLIT-FROM-PREV(same converted type, different raw UEFI type)" : "");
    }
}

#ifdef PTWALK_SELFTEST
// ---------------------------------------------------------------------------
// #647 self-test support. DEBUG BUILDS ONLY (`make PTWALKTEST=1`).
//
// The end-to-end proof of the fix is "drain the allocator and check that no page
// it hands out is a live page table". A literal drain via pmm_alloc_page() is
// O(n^2): the allocator is a first-fit linear scan from memory_start with NO
// cursor, so draining ~500k pages costs ~1.4e11 bit tests. pmm_selftest_drain()
// is the single-pass equivalent: pmm_alloc_page()'s selection rule is exactly
// "the lowest-index free page", so one ascending pass over the bitmap yields the
// same pages in the same order. vmm.c cross-checks that claim against a real
// bounded pmm_alloc_page() drain before trusting it.
//
// The bitmap is snapshotted and restored around the drain so the test leaves no
// trace. The drain never WRITES to the pages it "allocates"; it only compares
// their addresses.
// ---------------------------------------------------------------------------
static uint8_t pmm_test_backup[PMM_BITMAP_SIZE];
static uint64_t bk_free, bk_used, bk_total;

void pmm_selftest_snapshot(void) {
    memcpy(pmm_test_backup, page_bitmap, sizeof(page_bitmap));
    bk_free = free_pages; bk_used = used_pages; bk_total = total_pages;
}

void pmm_selftest_restore(void) {
    memcpy(page_bitmap, pmm_test_backup, sizeof(page_bitmap));
    free_pages = bk_free; used_pages = bk_used; total_pages = bk_total;
}

// Visit every page pmm_alloc_page() would hand out, in order, until exhaustion.
// Returns the number visited. Marks each used as it goes (so the sequence is the
// real one); pair with snapshot/restore.
uint64_t pmm_selftest_drain(void (*cb)(uint64_t phys, uint64_t ordinal, void *ctx),
                            void *ctx) {
    uint64_t n = 0;
    for (uint64_t page = memory_start; page < memory_end; page++) {
        if (!bitmap_test(page)) {
            bitmap_set(page);
            if (cb) cb(page * PMM_PAGE_SIZE, n, ctx);
            n++;
        }
    }
    return n;
}
#endif // PTWALK_SELFTEST

// Print memory statistics
void pmm_print_stats(void) {
    kprintf("[PMM] Memory Statistics:\n");
    kprintf("  Total: %lu pages (%lu MB)\n",
            total_pages, (total_pages * PMM_PAGE_SIZE) / MB);
    kprintf("  Free:  %lu pages (%lu MB)\n",
            free_pages, (free_pages * PMM_PAGE_SIZE) / MB);
    kprintf("  Used:  %lu pages (%lu MB)\n",
            used_pages, (used_pages * PMM_PAGE_SIZE) / MB);
}
