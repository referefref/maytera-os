// demand.c - Demand Paging Implementation for MayteraOS
// Implements lazy allocation, page fault handling, copy-on-write, and swap

#include "demand.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "../serial.h"
#include "../string.h"
#include "../proc/process.h"
#include "../fs/fat.h"

#ifndef USER_STACK_SIZE
#define USER_STACK_SIZE (2 * 1024 * 1024)  // 2MB default
#endif

// ============================================
// Global State
// ============================================

// Swap subsystem state
static swap_state_t swap_state = {0};

// COW reference count table
// Maps physical page number to reference count. Sized to cover the whole
// PMM identity-mapped window (2GB -> 524288 pages, 1MB table) so EVERY
// PMM-allocatable page is COW-trackable; an untrackable COW page would be
// double-freed on exit (both owners think they own it). #429.
#define COW_TABLE_SIZE      (0x80000000ULL / VMM_PAGE_SIZE_4K)  // 524288 (2GB)
static uint16_t cow_refcount[COW_TABLE_SIZE];

// #429: set by cpu_enable_nx() (mm/fault.c) once EFER.NXE is on. The demand
// paths must NOT set the PTE NX bit before this, or accesses to those pages
// raise a reserved-bit #PF.
extern int g_nx_enabled;

// Statistics
static uint64_t stat_minor_faults = 0;
static uint64_t stat_major_faults = 0;
static uint64_t stat_cow_faults = 0;
static uint64_t stat_lazy_allocs = 0;

// Spinlock for COW table
static volatile int cow_lock = 0;

static void cow_acquire_lock(void) {
    while (__sync_lock_test_and_set(&cow_lock, 1)) {
        __asm__ volatile("pause");
    }
}

static void cow_release_lock(void) {
    __sync_lock_release(&cow_lock);
}

// ============================================
// Initialization
// ============================================

void demand_init(void) {
    kprintf("[DEMAND] Initializing demand paging subsystem...\n");

    // Clear COW reference counts
    memset(cow_refcount, 0, sizeof(cow_refcount));

    // Swap is disabled by default
    swap_state.enabled = 0;

    kprintf("[DEMAND] Demand paging initialized\n");
    kprintf("[DEMAND]   COW table size: %u entries\n", COW_TABLE_SIZE);
}

int swap_init(const char *swap_path, uint64_t size_bytes) {
    if (swap_state.enabled) {
        kprintf("[SWAP] Already initialized\n");
        return -1;
    }

    kprintf("[SWAP] Initializing swap: %s, size %lu MB\n",
            swap_path, size_bytes / (1024 * 1024));

    // Calculate number of slots (one per page)
    uint32_t slots = size_bytes / VMM_PAGE_SIZE_4K;
    if (slots == 0) {
        kprintf("[SWAP] Swap file too small\n");
        return -1;
    }

    // Allocate bitmap for slot tracking
    uint32_t bitmap_size = (slots + 7) / 8;
    swap_state.slot_bitmap = kmalloc(bitmap_size);
    if (!swap_state.slot_bitmap) {
        kprintf("[SWAP] Failed to allocate slot bitmap\n");
        return -1;
    }
    memset(swap_state.slot_bitmap, 0, bitmap_size);

    // Store path
    swap_state.swap_path = kmalloc(strlen(swap_path) + 1);
    if (!swap_state.swap_path) {
        kfree(swap_state.slot_bitmap);
        return -1;
    }
    strcpy(swap_state.swap_path, swap_path);

    swap_state.total_slots = slots;
    swap_state.free_slots = slots;
    swap_state.swap_reads = 0;
    swap_state.swap_writes = 0;
    swap_state.enabled = 1;

    kprintf("[SWAP] Initialized with %u slots (%lu MB)\n",
            slots, (uint64_t)slots * VMM_PAGE_SIZE_4K / (1024 * 1024));

    return 0;
}

// ============================================
// VMA Management
// ============================================

vma_t *vma_create(uint64_t start, uint64_t end, uint32_t flags) {
    vma_t *vma = kmalloc(sizeof(vma_t));
    if (!vma) return NULL;

    memset(vma, 0, sizeof(vma_t));
    vma->start = start & ~(VMM_PAGE_SIZE_4K - 1);  // Page-align
    vma->end = (end + VMM_PAGE_SIZE_4K - 1) & ~(VMM_PAGE_SIZE_4K - 1);
    vma->flags = flags;
    vma->ref_count = 1;

    return vma;
}

int vma_add(mm_struct_t *mm, vma_t *vma) {
    if (!mm || !vma) return -1;

    // Insert in sorted order by start address
    vma_t *prev = NULL;
    vma_t *curr = mm->vma_list;

    while (curr && curr->start < vma->start) {
        prev = curr;
        curr = curr->next;
    }

    // Check for overlap
    if (prev && prev->end > vma->start) {
        kprintf("[VMA] Overlap with previous VMA: 0x%lx-0x%lx vs 0x%lx-0x%lx\n",
                prev->start, prev->end, vma->start, vma->end);
        return -1;
    }
    if (curr && vma->end > curr->start) {
        kprintf("[VMA] Overlap with next VMA: 0x%lx-0x%lx vs 0x%lx-0x%lx\n",
                vma->start, vma->end, curr->start, curr->end);
        return -1;
    }

    // Insert
    vma->prev = prev;
    vma->next = curr;

    if (prev) {
        prev->next = vma;
    } else {
        mm->vma_list = vma;
    }

    if (curr) {
        curr->prev = vma;
    }

    mm->vma_count++;
    mm->total_mapped += (vma->end - vma->start);

    return 0;
}

vma_t *vma_find(mm_struct_t *mm, uint64_t addr) {
    if (!mm) return NULL;

    vma_t *vma = mm->vma_list;
    while (vma) {
        if (addr >= vma->start && addr < vma->end) {
            return vma;
        }
        if (addr < vma->start) {
            // Past where it could be (list is sorted)
            break;
        }
        vma = vma->next;
    }

    return NULL;
}

vma_t *vma_find_range(mm_struct_t *mm, uint64_t start, uint64_t end) {
    if (!mm) return NULL;

    vma_t *vma = mm->vma_list;
    while (vma) {
        // Check for any overlap
        if (start < vma->end && end > vma->start) {
            return vma;
        }
        if (start >= vma->end) {
            vma = vma->next;
            continue;
        }
        break;
    }

    return NULL;
}

int vma_split(mm_struct_t *mm, vma_t *vma, uint64_t addr) {
    if (!mm || !vma) return -1;
    if (addr <= vma->start || addr >= vma->end) return -1;

    // Create new VMA for upper half
    vma_t *upper = vma_create(addr, vma->end, vma->flags);
    if (!upper) return -1;

    // Copy file info if applicable
    if (vma->flags & VMA_FILE) {
        upper->file = vma->file;
        upper->file_offset = vma->file_offset + (addr - vma->start);
        upper->file_size = vma->file_size - (addr - vma->start);
        if (upper->file_size > vma->end - addr) {
            upper->file_size = vma->end - addr;
        }
    }

    // Adjust original VMA
    vma->end = addr;

    // Insert upper half after vma
    upper->prev = vma;
    upper->next = vma->next;
    if (vma->next) {
        vma->next->prev = upper;
    }
    vma->next = upper;

    mm->vma_count++;

    return 0;
}

int vma_merge(mm_struct_t *mm, vma_t *vma) {
    if (!mm || !vma) return -1;

    // Try to merge with next VMA
    vma_t *next = vma->next;
    if (next && vma->end == next->start && vma->flags == next->flags) {
        // Check file backing compatibility
        if ((vma->flags & VMA_FILE) && (next->flags & VMA_FILE)) {
            if (vma->file != next->file) return 0;  // Different files
            if (vma->file_offset + (vma->end - vma->start) != next->file_offset) {
                return 0;  // Non-contiguous file regions
            }
        }

        // Merge
        vma->end = next->end;
        vma->next = next->next;
        if (next->next) {
            next->next->prev = vma;
        }

        kfree(next);
        mm->vma_count--;
    }

    // Try to merge with previous VMA
    vma_t *prev = vma->prev;
    if (prev && prev->end == vma->start && prev->flags == vma->flags) {
        if ((vma->flags & VMA_FILE) && (prev->flags & VMA_FILE)) {
            if (vma->file != prev->file) return 0;
            if (prev->file_offset + (prev->end - prev->start) != vma->file_offset) {
                return 0;
            }
        }

        prev->end = vma->end;
        prev->next = vma->next;
        if (vma->next) {
            vma->next->prev = prev;
        }

        kfree(vma);
        mm->vma_count--;
    }

    return 0;
}

void vma_remove(mm_struct_t *mm, vma_t *vma) {
    if (!mm || !vma) return;

    // Unlink from list
    if (vma->prev) {
        vma->prev->next = vma->next;
    } else {
        mm->vma_list = vma->next;
    }

    if (vma->next) {
        vma->next->prev = vma->prev;
    }

    mm->vma_count--;
    mm->total_mapped -= (vma->end - vma->start);

    kfree(vma);
}

void vma_free_all(mm_struct_t *mm) {
    if (!mm) return;

    vma_t *vma = mm->vma_list;
    while (vma) {
        vma_t *next = vma->next;
        kfree(vma);
        vma = next;
    }

    mm->vma_list = NULL;
    mm->vma_count = 0;
    mm->total_mapped = 0;
}

// ============================================
// Page Fault Handling
// ============================================

int demand_page_fault(uint64_t fault_addr, uint64_t error_code, void *context __attribute__((unused))) {
    // Get current process
    extern process_t *current_process;
    if (!current_process) {
        kprintf("[DEMAND] Page fault with no current process at 0x%lx\n", fault_addr);
        return -1;
    }

    // For now, use a static mm_struct since process_t doesn't have one
    // In a complete implementation, process_t would have a mm field
    static mm_struct_t default_mm = {0};
    mm_struct_t *mm = &default_mm;

    // Initialize mm defaults if needed
    if (mm->brk_start == 0) {
        mm->brk_start = 0x20000000;  // 512MB
        mm->brk_current = mm->brk_start;
        mm->stack_start = 0x7FFFFFFFFFFF;
        mm->stack_end = mm->stack_start - USER_STACK_SIZE;
        mm->mmap_base = 0x40000000;  // 1GB
    }

    // Find VMA containing the fault address
    vma_t *vma = vma_find(mm, fault_addr);
    if (!vma) {
        // Check if this might be stack growth
        if (fault_addr >= mm->stack_end - VMM_PAGE_SIZE_4K * 16 &&
            fault_addr < mm->stack_start) {
            // Grow stack
            uint64_t new_stack_end = fault_addr & ~(VMM_PAGE_SIZE_4K - 1);
            if (new_stack_end < mm->stack_end) {
                vma_t *stack_vma = vma_find(mm, mm->stack_start - 1);
                if (stack_vma && (stack_vma->flags & VMA_STACK)) {
                    stack_vma->start = new_stack_end;
                    mm->stack_end = new_stack_end;
                    return handle_lazy_fault(mm, stack_vma, fault_addr);
                }
            }
        }

        kprintf("[DEMAND] Segmentation fault: no VMA for address 0x%lx\n", fault_addr);
        return -1;
    }

    // Check access permissions
    if ((error_code & PF_WRITE) && !(vma->flags & VMA_WRITE)) {
        // Write to read-only - might be COW
        if (vma->flags & VMA_COW) {
            return handle_cow_fault(mm, vma, fault_addr);
        }
        kprintf("[DEMAND] Write access denied to read-only region at 0x%lx\n", fault_addr);
        return -1;
    }

    if ((error_code & PF_USER) && !(vma->flags & (VMA_READ | VMA_WRITE | VMA_EXEC))) {
        kprintf("[DEMAND] User access denied at 0x%lx\n", fault_addr);
        return -1;
    }

    // Determine fault type and handle
    if (vma->flags & VMA_LAZY) {
        return handle_lazy_fault(mm, vma, fault_addr);
    }

    if (vma->flags & VMA_COW) {
        return handle_cow_fault(mm, vma, fault_addr);
    }

    if (vma->flags & VMA_FILE) {
        return handle_file_fault(mm, vma, fault_addr);
    }

    // Check if swapped
    uint64_t pml4 = current_process->cr3;
    int state = pte_get_state(pml4, fault_addr);
    if (state == PAGE_STATE_SWAPPED) {
        return handle_swap_fault(mm, vma, fault_addr);
    }

    // Anonymous demand fault - allocate new page
    return handle_lazy_fault(mm, vma, fault_addr);
}

int handle_lazy_fault(mm_struct_t *mm, vma_t *vma, uint64_t fault_addr) {
    extern process_t *current_process;
    uint64_t pml4 = current_process->cr3;

    // Allocate physical page
    uint64_t phys_page = pmm_alloc_page();
    if (phys_page == 0) {
        kprintf("[DEMAND] Out of memory for lazy allocation at 0x%lx\n", fault_addr);
        // TODO: Try to swap out pages
        return -1;
    }

    // Zero the page (security and for BSS)
    memset((void*)phys_page, 0, VMM_PAGE_SIZE_4K);

    // Build page flags
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (vma->flags & VMA_WRITE) {
        flags |= VMM_FLAG_WRITABLE;
    }
    if (g_nx_enabled && !(vma->flags & VMA_EXEC)) {
        flags |= VMM_FLAG_NX;   // #429 W^X: writable data pages are no-execute
    }

    // Map the page
    uint64_t page_addr = fault_addr & ~(VMM_PAGE_SIZE_4K - 1);
    if (vmm_map_page_in(pml4, page_addr, phys_page, flags) != 0) {
        pmm_free_page(phys_page);
        kprintf("[DEMAND] Failed to map lazy page at 0x%lx\n", page_addr);
        return -1;
    }

    mm->resident_pages++;
    mm->lazy_pages--;
    stat_lazy_allocs++;
    stat_minor_faults++;

    return 0;
}

int handle_cow_fault(mm_struct_t *mm, vma_t *vma, uint64_t fault_addr) {
    extern process_t *current_process;
    uint64_t pml4 = current_process->cr3;

    uint64_t page_addr = fault_addr & ~(VMM_PAGE_SIZE_4K - 1);

    // Get current physical page
    uint64_t old_phys = vmm_get_physical_in(pml4, page_addr);
    if (old_phys == 0) {
        // Page not present - just allocate
        return handle_lazy_fault(mm, vma, fault_addr);
    }

    // Check if we're the only reference
    cow_acquire_lock();
    uint32_t page_index = old_phys / VMM_PAGE_SIZE_4K;
    if (page_index < COW_TABLE_SIZE && cow_refcount[page_index] <= 1) {
        // We're the only reference - just make writable
        cow_release_lock();

        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE;
        if (!(vma->flags & VMA_EXEC)) {
            flags |= VMM_FLAG_NX;
        }

        if (vmm_map_page_in(pml4, page_addr, old_phys, flags) != 0) {
            return -1;
        }

        // Clear COW flag from VMA if all pages are copied
        // (simplified - in reality we'd track per-page)
        stat_cow_faults++;
        return 0;
    }
    cow_release_lock();

    // Multiple references - need to copy
    uint64_t new_phys = pmm_alloc_page();
    if (new_phys == 0) {
        kprintf("[DEMAND] Out of memory for COW at 0x%lx\n", fault_addr);
        return -1;
    }

    // Copy page contents
    memcpy((void*)new_phys, (void*)old_phys, VMM_PAGE_SIZE_4K);

    // Map new page as writable
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE;
    if (g_nx_enabled && !(vma->flags & VMA_EXEC)) {
        flags |= VMM_FLAG_NX;   // #429 W^X: writable data pages are no-execute
    }

    if (vmm_map_page_in(pml4, page_addr, new_phys, flags) != 0) {
        pmm_free_page(new_phys);
        return -1;
    }

    // Decrement old page reference
    cow_page_unref(old_phys);

    mm->cow_pages--;
    mm->resident_pages++;
    stat_cow_faults++;

    return 0;
}

int handle_file_fault(mm_struct_t *mm, vma_t *vma, uint64_t fault_addr) {
    extern process_t *current_process;
    uint64_t pml4 = current_process->cr3;

    if (!vma->file) {
        kprintf("[DEMAND] File fault but no file handle at 0x%lx\n", fault_addr);
        return -1;
    }

    // Allocate physical page
    uint64_t phys_page = pmm_alloc_page();
    if (phys_page == 0) {
        kprintf("[DEMAND] Out of memory for file fault at 0x%lx\n", fault_addr);
        return -1;
    }

    // Calculate file offset for this page
    uint64_t page_addr = fault_addr & ~(VMM_PAGE_SIZE_4K - 1);
    uint64_t page_offset_in_vma = page_addr - vma->start;
    uint64_t file_offset = vma->file_offset + page_offset_in_vma;

    // Read from file
    fat_file_t *file = (fat_file_t *)vma->file;

    // Seek and read
    if (fat_seek(file, file_offset) != 0) {
        pmm_free_page(phys_page);
        kprintf("[DEMAND] Failed to seek file at offset %lu\n", file_offset);
        return -1;
    }

    // Clear page first (in case read is partial)
    memset((void*)phys_page, 0, VMM_PAGE_SIZE_4K);

    // Read up to one page
    uint64_t bytes_to_read = VMM_PAGE_SIZE_4K;
    if (page_offset_in_vma + bytes_to_read > vma->file_size) {
        bytes_to_read = vma->file_size - page_offset_in_vma;
    }

    int bytes_read = fat_read(file, (void*)phys_page, bytes_to_read);
    if (bytes_read < 0) {
        pmm_free_page(phys_page);
        kprintf("[DEMAND] Failed to read file at offset %lu\n", file_offset);
        return -1;
    }

    // Build page flags
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (vma->flags & VMA_WRITE) {
        flags |= VMM_FLAG_WRITABLE;
    }
    if (g_nx_enabled && !(vma->flags & VMA_EXEC)) {
        flags |= VMM_FLAG_NX;   // #429 W^X: writable data pages are no-execute
    }

    // Map the page
    if (vmm_map_page_in(pml4, page_addr, phys_page, flags) != 0) {
        pmm_free_page(phys_page);
        return -1;
    }

    mm->resident_pages++;
    stat_major_faults++;

    return 0;
}

int handle_swap_fault(mm_struct_t *mm, vma_t *vma, uint64_t fault_addr) {
    extern process_t *current_process;
    uint64_t pml4 = current_process->cr3;

    if (!swap_state.enabled) {
        kprintf("[DEMAND] Swap fault but swap disabled at 0x%lx\n", fault_addr);
        return -1;
    }

    uint64_t page_addr = fault_addr & ~(VMM_PAGE_SIZE_4K - 1);

    // Get swap slot from PTE
    uint32_t slot = pte_get_swap_slot(pml4, page_addr);
    if (slot == (uint32_t)-1) {
        kprintf("[DEMAND] Invalid swap slot at 0x%lx\n", fault_addr);
        return -1;
    }

    // Read page from swap
    uint64_t phys_page = swap_in_page(slot);
    if (phys_page == 0) {
        kprintf("[DEMAND] Failed to swap in page at 0x%lx\n", fault_addr);
        return -1;
    }

    // Build page flags
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (vma->flags & VMA_WRITE) {
        flags |= VMM_FLAG_WRITABLE;
    }
    if (g_nx_enabled && !(vma->flags & VMA_EXEC)) {
        flags |= VMM_FLAG_NX;   // #429 W^X: writable data pages are no-execute
    }

    // Map the page
    if (vmm_map_page_in(pml4, page_addr, phys_page, flags) != 0) {
        pmm_free_page(phys_page);
        return -1;
    }

    // Free swap slot
    swap_free_slot(slot);

    mm->resident_pages++;
    mm->swapped_pages--;
    stat_major_faults++;

    return 0;
}

// ============================================
// Memory Mapping (mmap)
// ============================================

uint64_t do_mmap(mm_struct_t *mm, uint64_t addr, uint64_t length,
                 uint32_t prot, uint32_t flags, void *file, uint64_t offset) {
    if (!mm || length == 0) return (uint64_t)-1;

    // Round up length to page boundary
    length = (length + VMM_PAGE_SIZE_4K - 1) & ~(VMM_PAGE_SIZE_4K - 1);

    // Find suitable address if not specified or hint
    if (addr == 0 || (flags & 0x10) == 0) {  // MAP_FIXED check
        // Find free region starting from mmap_base
        addr = mm->mmap_base;
        vma_t *vma = mm->vma_list;
        while (vma) {
            if (addr + length <= vma->start) {
                // Found gap
                break;
            }
            if (addr < vma->end) {
                addr = vma->end;
            }
            vma = vma->next;
        }
    }

    // Align address
    addr = (addr + VMM_PAGE_SIZE_4K - 1) & ~(VMM_PAGE_SIZE_4K - 1);

    // Check for overlap
    if (vma_find_range(mm, addr, addr + length) != NULL) {
        kprintf("[MMAP] Address range 0x%lx-0x%lx overlaps existing VMA\n",
                addr, addr + length);
        return (uint64_t)-1;
    }

    // Build VMA flags
    uint32_t vma_flags = VMA_LAZY;
    if (prot & 0x1) vma_flags |= VMA_READ;    // PROT_READ
    if (prot & 0x2) vma_flags |= VMA_WRITE;   // PROT_WRITE
    if (prot & 0x4) vma_flags |= VMA_EXEC;    // PROT_EXEC

    if (flags & 0x1) {
        vma_flags |= VMA_SHARED;              // MAP_SHARED
    } else {
        vma_flags |= VMA_PRIVATE;             // MAP_PRIVATE
    }

    if (flags & 0x20) {
        vma_flags |= VMA_ANONYMOUS;           // MAP_ANONYMOUS
    }

    if (file) {
        vma_flags |= VMA_FILE;
        vma_flags &= ~VMA_ANONYMOUS;
    }

    // Create VMA
    vma_t *vma = vma_create(addr, addr + length, vma_flags);
    if (!vma) return (uint64_t)-1;

    vma->prot = prot;

    if (file) {
        vma->file = file;
        vma->file_offset = offset;
        vma->file_size = length;
    }

    // Add to memory map
    if (vma_add(mm, vma) != 0) {
        kfree(vma);
        return (uint64_t)-1;
    }

    mm->lazy_pages += length / VMM_PAGE_SIZE_4K;

    // Update mmap_base for next allocation
    if (addr + length > mm->mmap_base) {
        mm->mmap_base = addr + length + VMM_PAGE_SIZE_4K;
    }

    return addr;
}

int do_munmap(mm_struct_t *mm, uint64_t addr, uint64_t length) {
    if (!mm || length == 0) return -1;

    extern process_t *current_process;
    uint64_t pml4 = current_process->cr3;

    // Round to page boundaries
    uint64_t start = addr & ~(VMM_PAGE_SIZE_4K - 1);
    uint64_t end = (addr + length + VMM_PAGE_SIZE_4K - 1) & ~(VMM_PAGE_SIZE_4K - 1);

    // Find and process VMAs in range
    vma_t *vma = vma_find(mm, start);
    while (vma && vma->start < end) {
        vma_t *next = vma->next;

        if (vma->start >= start && vma->end <= end) {
            // Entirely contained - unmap all pages and remove
            for (uint64_t page = vma->start; page < vma->end; page += VMM_PAGE_SIZE_4K) {
                uint64_t phys = vmm_get_physical_in(pml4, page);
                if (phys) {
                    vmm_unmap_page_in(pml4, page);
                    if (vma->flags & VMA_COW) {
                        cow_page_unref(phys);
                    } else {
                        pmm_free_page(phys);
                    }
                    mm->resident_pages--;
                }
            }
            vma_remove(mm, vma);
        } else if (vma->start < start && vma->end > end) {
            // VMA spans the unmapped region - split into two
            vma_split(mm, vma, start);
            vma_t *middle = vma->next;
            if (middle) {
                vma_split(mm, middle, end);
                // Unmap middle
                for (uint64_t page = start; page < end; page += VMM_PAGE_SIZE_4K) {
                    uint64_t phys = vmm_get_physical_in(pml4, page);
                    if (phys) {
                        vmm_unmap_page_in(pml4, page);
                        pmm_free_page(phys);
                        mm->resident_pages--;
                    }
                }
                vma_remove(mm, middle);
            }
        } else if (vma->start < start) {
            // Unmap end of VMA
            vma_split(mm, vma, start);
            vma_t *tail = vma->next;
            if (tail) {
                for (uint64_t page = tail->start; page < tail->end && page < end;
                     page += VMM_PAGE_SIZE_4K) {
                    uint64_t phys = vmm_get_physical_in(pml4, page);
                    if (phys) {
                        vmm_unmap_page_in(pml4, page);
                        pmm_free_page(phys);
                        mm->resident_pages--;
                    }
                }
                if (tail->end <= end) {
                    vma_remove(mm, tail);
                } else {
                    tail->start = end;
                }
            }
        } else {
            // Unmap start of VMA
            vma_split(mm, vma, end);
            for (uint64_t page = vma->start; page < vma->end && page >= start;
                 page += VMM_PAGE_SIZE_4K) {
                uint64_t phys = vmm_get_physical_in(pml4, page);
                if (phys) {
                    vmm_unmap_page_in(pml4, page);
                    pmm_free_page(phys);
                    mm->resident_pages--;
                }
            }
            vma_remove(mm, vma);
        }

        vma = next;
    }

    return 0;
}

int do_mprotect(mm_struct_t *mm, uint64_t addr, uint64_t length, uint32_t prot) {
    if (!mm || length == 0) return -1;

    extern process_t *current_process;
    uint64_t pml4 = current_process->cr3;

    uint64_t start = addr & ~(VMM_PAGE_SIZE_4K - 1);
    uint64_t end = (addr + length + VMM_PAGE_SIZE_4K - 1) & ~(VMM_PAGE_SIZE_4K - 1);

    // Find VMA
    vma_t *vma = vma_find(mm, start);
    if (!vma || vma->start > start || vma->end < end) {
        return -1;  // Range not fully mapped
    }

    // Update VMA flags
    vma->flags &= ~(VMA_READ | VMA_WRITE | VMA_EXEC);
    if (prot & 0x1) vma->flags |= VMA_READ;
    if (prot & 0x2) vma->flags |= VMA_WRITE;
    if (prot & 0x4) vma->flags |= VMA_EXEC;
    vma->prot = prot;

    // Update page table entries
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (prot & 0x2) flags |= VMM_FLAG_WRITABLE;
    if (!(prot & 0x4)) flags |= VMM_FLAG_NX;

    for (uint64_t page = start; page < end; page += VMM_PAGE_SIZE_4K) {
        uint64_t phys = vmm_get_physical_in(pml4, page);
        if (phys) {
            vmm_map_page_in(pml4, page, phys, flags);
        }
    }

    return 0;
}

int do_msync(mm_struct_t *mm, uint64_t addr, uint64_t length, int flags) {
    // TODO: Implement file sync for memory-mapped files
    (void)mm;
    (void)addr;
    (void)length;
    (void)flags;
    return 0;
}

// ============================================
// Heap Management
// ============================================

uint64_t do_brk(mm_struct_t *mm, uint64_t addr) {
    if (!mm) return (uint64_t)-1;

    extern process_t *current_process;
    uint64_t pml4 = current_process->cr3;

    // Return current brk if addr is 0
    if (addr == 0) {
        return mm->brk_current;
    }

    // Align to page boundary
    uint64_t new_brk = (addr + VMM_PAGE_SIZE_4K - 1) & ~(VMM_PAGE_SIZE_4K - 1);

    // Check bounds
    if (new_brk < mm->brk_start) {
        return (uint64_t)-1;
    }

    if (new_brk > mm->brk_current) {
        // Expanding heap - find or create heap VMA
        vma_t *heap_vma = vma_find(mm, mm->brk_start);
        if (!heap_vma) {
            // Create heap VMA
            heap_vma = vma_create(mm->brk_start, new_brk,
                                  VMA_READ | VMA_WRITE | VMA_HEAP | VMA_LAZY);
            if (!heap_vma) return (uint64_t)-1;
            if (vma_add(mm, heap_vma) != 0) {
                kfree(heap_vma);
                return (uint64_t)-1;
            }
        } else {
            // Expand existing VMA
            heap_vma->end = new_brk;
        }

        uint64_t pages_added = (new_brk - mm->brk_current) / VMM_PAGE_SIZE_4K;
        mm->lazy_pages += pages_added;
    } else if (new_brk < mm->brk_current) {
        // Shrinking heap - unmap pages
        for (uint64_t page = new_brk; page < mm->brk_current; page += VMM_PAGE_SIZE_4K) {
            uint64_t phys = vmm_get_physical_in(pml4, page);
            if (phys) {
                vmm_unmap_page_in(pml4, page);
                pmm_free_page(phys);
                mm->resident_pages--;
            } else {
                mm->lazy_pages--;
            }
        }

        // Shrink heap VMA
        vma_t *heap_vma = vma_find(mm, mm->brk_start);
        if (heap_vma) {
            heap_vma->end = new_brk;
        }
    }

    mm->brk_current = new_brk;
    return new_brk;
}

void *do_sbrk(mm_struct_t *mm, int64_t increment) {
    if (!mm) return (void*)-1;

    uint64_t old_brk = mm->brk_current;
    uint64_t new_brk = old_brk + increment;

    if (do_brk(mm, new_brk) == (uint64_t)-1) {
        return (void*)-1;
    }

    return (void*)old_brk;
}

// ============================================
// Copy-on-Write Support
// ============================================

int cow_mark_all(mm_struct_t *mm, uint64_t pml4_phys) {
    if (!mm) return -1;

    vma_t *vma = mm->vma_list;
    while (vma) {
        // Skip shared mappings and non-writable regions
        if ((vma->flags & VMA_SHARED) || !(vma->flags & VMA_WRITE)) {
            vma = vma->next;
            continue;
        }

        // Mark pages as COW (read-only)
        for (uint64_t addr = vma->start; addr < vma->end; addr += VMM_PAGE_SIZE_4K) {
            uint64_t phys = vmm_get_physical_in(pml4_phys, addr);
            if (phys) {
                pte_mark_cow(pml4_phys, addr);
                cow_page_ref(phys);
                mm->cow_pages++;
            }
        }

        // Mark VMA as COW
        vma->flags |= VMA_COW;
        vma = vma->next;
    }

    return 0;
}

mm_struct_t *mm_clone_cow(mm_struct_t *src) {
    if (!src) return NULL;

    mm_struct_t *dst = mm_create();
    if (!dst) return NULL;

    // Copy basic fields
    dst->brk_start = src->brk_start;
    dst->brk_current = src->brk_current;
    dst->stack_start = src->stack_start;
    dst->stack_end = src->stack_end;
    dst->mmap_base = src->mmap_base;

    // Clone VMAs (share pages with COW)
    vma_t *src_vma = src->vma_list;
    while (src_vma) {
        vma_t *dst_vma = vma_create(src_vma->start, src_vma->end, src_vma->flags);
        if (!dst_vma) {
            mm_destroy(dst);
            return NULL;
        }

        dst_vma->prot = src_vma->prot;
        dst_vma->file = src_vma->file;
        dst_vma->file_offset = src_vma->file_offset;
        dst_vma->file_size = src_vma->file_size;

        // For writable private mappings, enable COW
        if ((src_vma->flags & VMA_WRITE) && !(src_vma->flags & VMA_SHARED)) {
            dst_vma->flags |= VMA_COW;
            src_vma->flags |= VMA_COW;
        }

        if (vma_add(dst, dst_vma) != 0) {
            kfree(dst_vma);
            mm_destroy(dst);
            return NULL;
        }

        src_vma = src_vma->next;
    }

    // Copy statistics
    dst->resident_pages = src->resident_pages;
    dst->cow_pages = src->cow_pages;
    dst->lazy_pages = src->lazy_pages;

    return dst;
}

void cow_page_ref(uint64_t phys_addr) {
    uint32_t page_index = phys_addr / VMM_PAGE_SIZE_4K;
    if (page_index >= COW_TABLE_SIZE) return;

    cow_acquire_lock();
    if (cow_refcount[page_index] < 0xFFFF) {
        cow_refcount[page_index]++;
    }
    cow_release_lock();
}

void cow_page_unref(uint64_t phys_addr) {
    uint32_t page_index = phys_addr / VMM_PAGE_SIZE_4K;
    if (page_index >= COW_TABLE_SIZE) return;

    cow_acquire_lock();
    if (cow_refcount[page_index] > 0) {
        cow_refcount[page_index]--;
        if (cow_refcount[page_index] == 0) {
            cow_release_lock();
            pmm_free_page(phys_addr);
            return;
        }
    }
    cow_release_lock();
}

int cow_page_shared(uint64_t phys_addr) {
    uint32_t page_index = phys_addr / VMM_PAGE_SIZE_4K;
    if (page_index >= COW_TABLE_SIZE) return 0;

    cow_acquire_lock();
    int shared = (cow_refcount[page_index] > 1);
    cow_release_lock();
    return shared;
}

// ============================================
// Swap Operations
// ============================================

static uint32_t swap_alloc_slot(void) {
    if (!swap_state.enabled || swap_state.free_slots == 0) {
        return (uint32_t)-1;
    }

    for (uint32_t i = 0; i < swap_state.total_slots; i++) {
        uint32_t byte = i / 8;
        uint32_t bit = i % 8;
        if (!(swap_state.slot_bitmap[byte] & (1 << bit))) {
            swap_state.slot_bitmap[byte] |= (1 << bit);
            swap_state.free_slots--;
            return i;
        }
    }

    return (uint32_t)-1;
}

uint32_t swap_out_page(uint64_t phys_addr __attribute__((unused))) {
    if (!swap_state.enabled) return (uint32_t)-1;

    uint32_t slot = swap_alloc_slot();
    if (slot == (uint32_t)-1) return (uint32_t)-1;

    // TODO: Write page to swap file
    // For now, we just track the slot
    // uint64_t offset = (uint64_t)slot * VMM_PAGE_SIZE_4K;
    // fat_seek(swap_state.swap_file, offset);
    // fat_write(swap_state.swap_file, (void*)phys_addr, VMM_PAGE_SIZE_4K);

    swap_state.swap_writes++;
    return slot;
}

uint64_t swap_in_page(uint32_t slot_index) {
    if (!swap_state.enabled || slot_index >= swap_state.total_slots) {
        return 0;
    }

    // Allocate physical page
    uint64_t phys_page = pmm_alloc_page();
    if (phys_page == 0) return 0;

    // TODO: Read page from swap file
    // uint64_t offset = (uint64_t)slot_index * VMM_PAGE_SIZE_4K;
    // fat_seek(swap_state.swap_file, offset);
    // fat_read(swap_state.swap_file, (void*)phys_page, VMM_PAGE_SIZE_4K);

    swap_state.swap_reads++;
    return phys_page;
}

void swap_free_slot(uint32_t slot_index) {
    if (!swap_state.enabled || slot_index >= swap_state.total_slots) {
        return;
    }

    uint32_t byte = slot_index / 8;
    uint32_t bit = slot_index % 8;
    if (swap_state.slot_bitmap[byte] & (1 << bit)) {
        swap_state.slot_bitmap[byte] &= ~(1 << bit);
        swap_state.free_slots++;
    }
}

int swap_enabled(void) {
    return swap_state.enabled;
}

void swap_get_stats(uint32_t *total, uint32_t *free, uint64_t *reads, uint64_t *writes) {
    if (total) *total = swap_state.total_slots;
    if (free) *free = swap_state.free_slots;
    if (reads) *reads = swap_state.swap_reads;
    if (writes) *writes = swap_state.swap_writes;
}

// ============================================
// Memory Map Management
// ============================================

mm_struct_t *mm_create(void) {
    mm_struct_t *mm = kmalloc(sizeof(mm_struct_t));
    if (!mm) return NULL;

    memset(mm, 0, sizeof(mm_struct_t));

    // Default mmap base (start at 512MB for user space)
    mm->mmap_base = 0x20000000;

    return mm;
}

void mm_destroy(mm_struct_t *mm) {
    if (!mm) return;

    // Free all VMAs
    vma_free_all(mm);

    kfree(mm);
}

mm_struct_t *mm_clone(mm_struct_t *src) {
    return mm_clone_cow(src);
}

void mm_print(mm_struct_t *mm) {
    if (!mm) {
        kprintf("[MM] NULL memory map\n");
        return;
    }

    kprintf("[MM] Memory Map:\n");
    kprintf("  Heap: 0x%lx - 0x%lx\n", mm->brk_start, mm->brk_current);
    kprintf("  Stack: 0x%lx - 0x%lx\n", mm->stack_end, mm->stack_start);
    kprintf("  mmap base: 0x%lx\n", mm->mmap_base);
    kprintf("  VMAs: %u, Total mapped: %lu bytes\n", mm->vma_count, mm->total_mapped);
    kprintf("  Resident: %lu, COW: %lu, Lazy: %lu, Swapped: %lu\n",
            mm->resident_pages, mm->cow_pages, mm->lazy_pages, mm->swapped_pages);
}

// ============================================
// Page Table Manipulation
// ============================================

// We use special bits in the PTE for demand paging:
// - Bit 9 (AVL): COW flag
// - Bit 10 (AVL): Swapped flag
// - Bits 12-51: Swap slot index (when swapped)

#define PTE_COW_BIT     (1ULL << 9)
#define PTE_SWAP_BIT    (1ULL << 10)

int pte_mark_cow(uint64_t pml4_phys, uint64_t virt_addr) {
    // Get current physical mapping
    uint64_t phys = vmm_get_physical_in(pml4_phys, virt_addr);
    if (phys == 0) return -1;

    // Remap as read-only with COW bit
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | PTE_COW_BIT;
    return vmm_map_page_in(pml4_phys, virt_addr, phys, flags);
}

int pte_mark_lazy(uint64_t pml4_phys, uint64_t virt_addr) {
    // Simply unmap - the VMA will handle the fault
    vmm_unmap_page_in(pml4_phys, virt_addr);
    return 0;
}

int pte_mark_swapped(uint64_t pml4_phys, uint64_t virt_addr, uint32_t swap_slot) {
    // Create a special PTE with swap slot encoded
    uint64_t pte = PTE_SWAP_BIT | ((uint64_t)swap_slot << 12);
    return vmm_map_page_in(pml4_phys, virt_addr, pte, 0);
}

int pte_get_state(uint64_t pml4_phys, uint64_t virt_addr) {
    uint64_t phys = vmm_get_physical_in(pml4_phys, virt_addr);

    if (phys == 0) {
        return PAGE_STATE_UNMAPPED;
    }

    // Check for swap marker
    if (phys & PTE_SWAP_BIT) {
        return PAGE_STATE_SWAPPED;
    }

    // Check for COW
    // Note: This is simplified - in reality we'd need to read the actual PTE
    return PAGE_STATE_PRESENT;
}

uint32_t pte_get_swap_slot(uint64_t pml4_phys, uint64_t virt_addr) {
    uint64_t phys = vmm_get_physical_in(pml4_phys, virt_addr);
    if (!(phys & PTE_SWAP_BIT)) {
        return (uint32_t)-1;
    }
    return (uint32_t)(phys >> 12);
}

// ============================================
// Statistics and Debugging
// ============================================

void demand_get_stats(uint64_t *minor_faults, uint64_t *major_faults,
                      uint64_t *cow_faults, uint64_t *lazy_allocs) {
    if (minor_faults) *minor_faults = stat_minor_faults;
    if (major_faults) *major_faults = stat_major_faults;
    if (cow_faults) *cow_faults = stat_cow_faults;
    if (lazy_allocs) *lazy_allocs = stat_lazy_allocs;
}

void demand_print_stats(void) {
    kprintf("[DEMAND] Statistics:\n");
    kprintf("  Minor faults: %lu\n", stat_minor_faults);
    kprintf("  Major faults: %lu\n", stat_major_faults);
    kprintf("  COW faults: %lu\n", stat_cow_faults);
    kprintf("  Lazy allocations: %lu\n", stat_lazy_allocs);

    if (swap_state.enabled) {
        kprintf("[SWAP] Statistics:\n");
        kprintf("  Total slots: %u\n", swap_state.total_slots);
        kprintf("  Free slots: %u\n", swap_state.free_slots);
        kprintf("  Pages read: %lu\n", swap_state.swap_reads);
        kprintf("  Pages written: %lu\n", swap_state.swap_writes);
    }
}

void demand_dump_vmas(mm_struct_t *mm) {
    if (!mm) return;

    kprintf("[DEMAND] VMA List:\n");
    vma_t *vma = mm->vma_list;
    int i = 0;
    while (vma) {
        kprintf("  [%d] 0x%lx - 0x%lx (%lu KB) flags=0x%x",
                i++, vma->start, vma->end,
                (vma->end - vma->start) / 1024, vma->flags);

        if (vma->flags & VMA_READ) kprintf(" R");
        if (vma->flags & VMA_WRITE) kprintf("W");
        if (vma->flags & VMA_EXEC) kprintf("X");
        if (vma->flags & VMA_SHARED) kprintf(" shared");
        if (vma->flags & VMA_PRIVATE) kprintf(" private");
        if (vma->flags & VMA_ANONYMOUS) kprintf(" anon");
        if (vma->flags & VMA_FILE) kprintf(" file");
        if (vma->flags & VMA_STACK) kprintf(" stack");
        if (vma->flags & VMA_HEAP) kprintf(" heap");
        if (vma->flags & VMA_COW) kprintf(" cow");
        if (vma->flags & VMA_LAZY) kprintf(" lazy");

        kprintf("\n");
        vma = vma->next;
    }
}

// ============================================
// #429: per-process fault resolver + real COW
// ============================================

int cow_trackable(uint64_t phys_addr) {
    return (phys_addr / VMM_PAGE_SIZE_4K) < COW_TABLE_SIZE;
}

// Record that one more address space now shares `phys` (called once per shared
// page as fork marks it COW). The first time a page becomes shared its count
// jumps to 2 (the parent that already mapped it + the new child).
void cow_fork_share(uint64_t phys_addr) {
    uint32_t idx = phys_addr / VMM_PAGE_SIZE_4K;
    if (idx >= COW_TABLE_SIZE) return;
    cow_acquire_lock();
    if (cow_refcount[idx] == 0) cow_refcount[idx] = 2;
    else if (cow_refcount[idx] < 0xFFFF) cow_refcount[idx]++;
    cow_release_lock();
}

// COW-aware free of a user leaf page (see demand.h).
void vmm_free_user_page_cow(uint64_t phys_addr) {
    uint32_t idx = phys_addr / VMM_PAGE_SIZE_4K;
    if (idx < COW_TABLE_SIZE) {
        cow_acquire_lock();
        if (cow_refcount[idx] > 0) {
            cow_refcount[idx]--;
            int last = (cow_refcount[idx] == 0);
            cow_release_lock();
            if (last) pmm_free_page(phys_addr);
            return;
        }
        cow_release_lock();
    }
    pmm_free_page(phys_addr);
}

// Copy-on-write a single page on a write fault.
int demand_cow_write(struct process *p, uint64_t page_addr) {
    uint64_t pml4 = p->cr3;
    uint64_t pte = vmm_get_pte_in(pml4, page_addr);
    if (!(pte & VMM_FLAG_PRESENT)) return -1;

    uint64_t old_phys = pte & VMM_ADDR_MASK;
    uint64_t keep_nx  = pte & VMM_FLAG_NX;   // preserve W^X on the fresh copy
    uint32_t idx = old_phys / VMM_PAGE_SIZE_4K;

    int shared = 0;
    cow_acquire_lock();
    if (idx < COW_TABLE_SIZE && cow_refcount[idx] > 1) shared = 1;
    cow_release_lock();

    uint64_t new_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE | keep_nx;

    if (!shared) {
        // Sole owner: drop the COW bit and re-enable write in place.
        if (vmm_map_page_in(pml4, page_addr, old_phys, new_flags) != 0) return -1;
        if (idx < COW_TABLE_SIZE) {
            cow_acquire_lock();
            if (cow_refcount[idx] == 1) cow_refcount[idx] = 0;  // now private
            cow_release_lock();
        }
        stat_cow_faults++;
        return 0;
    }

    // Shared: allocate a private copy.
    uint64_t new_phys = pmm_alloc_page();
    if (new_phys == 0) {
        kprintf("[DEMAND] COW OOM at 0x%lx\n", page_addr);
        return -1;
    }
    memcpy((void *)new_phys, (void *)old_phys, VMM_PAGE_SIZE_4K);
    if (vmm_map_page_in(pml4, page_addr, new_phys, new_flags) != 0) {
        pmm_free_page(new_phys);
        return -1;
    }
    // This address space no longer shares the old page. new_phys is private
    // (refcount stays 0). Drop one reference on the old page.
    cow_page_unref(old_phys);
    stat_cow_faults++;
    return 0;
}

// Per-process page-fault resolver (see demand.h). Called by the #PF handler.
int mm_fault(struct process *p, uint64_t fault_addr, uint64_t error_code) {
    if (!p || p->cr3 == 0) return -1;

    uint64_t page_addr = fault_addr & ~(VMM_PAGE_SIZE_4K - 1);
    uint64_t pml4 = p->cr3;
    uint64_t pte = vmm_get_pte_in(pml4, page_addr);

    if (pte & VMM_FLAG_PRESENT) {
        // Recoverable case 1: a write to a read-only COW page (works for user-
        // AND kernel-mode writes so copy_to_user into a fork-shared buffer is
        // handled too).
        if ((error_code & PF_WRITE) && (pte & PTE_COW_FLAG) &&
            !(pte & VMM_FLAG_WRITABLE)) {
            return demand_cow_write(p, page_addr);
        }
        // Recoverable case 2: a USER access faulted on a page that is present
        // but lacks the USER bit. This happens inside a demand region when an
        // earlier fault in the same 2MB span split a kernel identity huge page:
        // the sibling 4KB entries inherit PRESENT|WRITABLE but not USER. If a
        // demand VMA covers the address, map a real user page over the stale
        // kernel entry.
        if ((error_code & PF_USER) && !(pte & VMM_FLAG_USER)) {
            mm_struct_t *mm2 = (mm_struct_t *)p->mm;
            vma_t *v2 = mm2 ? vma_find(mm2, fault_addr) : (vma_t *)0;
            if (v2 && !(v2->flags & VMA_FILE)) {
                if ((error_code & PF_WRITE) && !(v2->flags & VMA_WRITE)) return -1;
                return handle_lazy_fault(mm2, v2, fault_addr);
            }
        }
        return -1;  // NX exec fault, or a genuine protection violation
    }

    // Not present: satisfy from the process's VMA list (demand-zero / lazy
    // mmap or file-backed). Requires a per-process mm.
    mm_struct_t *mm = (mm_struct_t *)p->mm;
    if (!mm) return -1;

    vma_t *vma = vma_find(mm, fault_addr);
    if (!vma) return -1;

    // A user access must be to a page the VMA actually permits.
    if ((error_code & PF_WRITE) && !(vma->flags & VMA_WRITE)) return -1;
    if ((error_code & PF_USER) &&
        !(vma->flags & (VMA_READ | VMA_WRITE | VMA_EXEC))) return -1;

    if (vma->flags & VMA_FILE) {
        return handle_file_fault(mm, vma, fault_addr);
    }
    // Anonymous / lazy: demand-zero a fresh page.
    return handle_lazy_fault(mm, vma, fault_addr);
}

// Duplicate a memory map's VMA list for fork (shallow; physical COW handled
// by the page-table clone). File-backed VMAs share the underlying handle.
mm_struct_t *mm_dup(mm_struct_t *src) {
    if (!src) return NULL;
    mm_struct_t *dst = mm_create();
    if (!dst) return NULL;

    dst->brk_start   = src->brk_start;
    dst->brk_current = src->brk_current;
    dst->stack_start = src->stack_start;
    dst->stack_end   = src->stack_end;
    dst->mmap_base   = src->mmap_base;

    for (vma_t *sv = src->vma_list; sv; sv = sv->next) {
        vma_t *dv = vma_create(sv->start, sv->end, sv->flags);
        if (!dv) { mm_destroy(dst); return NULL; }
        dv->prot        = sv->prot;
        dv->file        = sv->file;
        dv->file_offset = sv->file_offset;
        dv->file_size   = sv->file_size;
        if (vma_add(dst, dv) != 0) { kfree(dv); mm_destroy(dst); return NULL; }
    }
    return dst;
}
