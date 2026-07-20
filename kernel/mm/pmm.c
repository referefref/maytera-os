// pmm.c - Physical Memory Manager implementation
// Uses a bitmap to track 4KB page frames

#include "pmm.h"
#include "../boot_info.h"
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

// Spinlock for thread safety (simple implementation)
static volatile int pmm_lock = 0;

static void pmm_acquire_lock(void) {
    while (__sync_lock_test_and_set(&pmm_lock, 1)) {
        __asm__ volatile("pause");
    }
}

static void pmm_release_lock(void) {
    __sync_lock_release(&pmm_lock);
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

// Allocate a single physical page
uint64_t pmm_alloc_page(void) {
    pmm_acquire_lock();

    // Search for a free page
    for (uint64_t page = memory_start; page < memory_end; page++) {
        if (!bitmap_test(page)) {
            bitmap_set(page);
            free_pages--;
            used_pages++;
            pmm_release_lock();
            uint64_t pa = page * PMM_PAGE_SIZE;
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

    pmm_release_lock();
    kprintf("[PMM] ERROR: Out of physical memory!\n");
    return 0;
}

// Allocate multiple contiguous physical pages
uint64_t pmm_alloc_pages(uint64_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_page();

    pmm_acquire_lock();

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
            pmm_release_lock();
            return start_page * PMM_PAGE_SIZE;
        }
    }

    pmm_release_lock();
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

    pmm_acquire_lock();

    if (!bitmap_test(page)) {
        pmm_release_lock();
        kprintf("[PMM] WARNING: Double free of page 0x%lx\n", phys_addr);
        return;
    }

    bitmap_clear(page);
    free_pages++;
    used_pages--;

    pmm_release_lock();
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
