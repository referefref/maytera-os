// demand.h - Demand Paging Support for MayteraOS
// Implements lazy allocation, page fault handling, and copy-on-write
#ifndef DEMAND_H
#define DEMAND_H

#include "../types.h"

// Page fault error codes (from CPU)
#define PF_PRESENT      (1 << 0)    // Page was present (protection violation)
#define PF_WRITE        (1 << 1)    // Write access caused the fault
#define PF_USER         (1 << 2)    // Fault occurred in user mode
#define PF_RESERVED     (1 << 3)    // Reserved bit violation
#define PF_INSTRUCTION  (1 << 4)    // Instruction fetch (NX violation)

// Demand paging flags for VMAs (Virtual Memory Areas)
#define VMA_READ        (1 << 0)    // Readable
#define VMA_WRITE       (1 << 1)    // Writable
#define VMA_EXEC        (1 << 2)    // Executable
#define VMA_SHARED      (1 << 3)    // Shared mapping
#define VMA_PRIVATE     (1 << 4)    // Private mapping (copy-on-write)
#define VMA_ANONYMOUS   (1 << 5)    // Anonymous mapping (not file-backed)
#define VMA_FILE        (1 << 6)    // File-backed mapping
#define VMA_STACK       (1 << 7)    // Stack region (grows down)
#define VMA_HEAP        (1 << 8)    // Heap region (grows up)
#define VMA_COW         (1 << 9)    // Copy-on-write (waiting to be copied)
#define VMA_LAZY        (1 << 10)   // Lazy allocation (not yet allocated)

// Page states in the page table
#define PAGE_STATE_UNMAPPED     0   // Not in page table
#define PAGE_STATE_PRESENT      1   // Present and valid
#define PAGE_STATE_SWAPPED      2   // Swapped to disk
#define PAGE_STATE_COW          3   // Copy-on-write (shared read-only)
#define PAGE_STATE_LAZY         4   // Lazy allocation (fault will allocate)
#define PAGE_STATE_FILE         5   // File-backed (fault will load)

// Virtual Memory Area (VMA) - describes a contiguous region of virtual memory
typedef struct vma {
    uint64_t start;             // Start virtual address (page-aligned)
    uint64_t end;               // End virtual address (exclusive, page-aligned)
    uint32_t flags;             // VMA flags (VMA_*)
    uint32_t prot;              // Protection flags for mmap compatibility

    // File backing (if VMA_FILE is set)
    void *file;                 // File handle (fat_file_t*)
    uint64_t file_offset;       // Offset in file
    uint64_t file_size;         // Size of file mapping

    // COW reference counting
    uint32_t ref_count;         // Number of processes sharing this VMA

    struct vma *next;           // Next VMA in process's list
    struct vma *prev;           // Previous VMA in process's list
} vma_t;

// Process memory map (collection of VMAs)
typedef struct {
    vma_t *vma_list;            // Linked list of VMAs
    uint32_t vma_count;         // Number of VMAs

    uint64_t brk_start;         // Start of heap (after data segment)
    uint64_t brk_current;       // Current heap end

    uint64_t stack_start;       // Stack bottom (highest address)
    uint64_t stack_end;         // Stack top (lowest address, grows down)

    uint64_t mmap_base;         // Base address for mmap allocations

    // Statistics
    uint64_t total_mapped;      // Total bytes mapped
    uint64_t resident_pages;    // Pages currently in memory
    uint64_t swapped_pages;     // Pages swapped to disk
    uint64_t cow_pages;         // Pages marked copy-on-write
    uint64_t lazy_pages;        // Pages with lazy allocation

    // Page fault statistics
    uint64_t minor_faults;      // Faults satisfied without disk I/O
    uint64_t major_faults;      // Faults requiring disk I/O
    uint64_t cow_faults;        // Copy-on-write faults
} mm_struct_t;

// Swap slot descriptor
typedef struct {
    uint32_t slot_index;        // Index in swap file/partition
    uint32_t ref_count;         // Reference count for COW
    uint64_t page_hash;         // Hash for verification
} swap_slot_t;

// Swap subsystem state
typedef struct {
    int enabled;                // Swap is enabled
    char *swap_path;            // Path to swap file/partition
    void *swap_file;            // File handle if file-backed
    uint32_t total_slots;       // Total swap slots
    uint32_t free_slots;        // Available swap slots
    uint8_t *slot_bitmap;       // Bitmap of used slots
    uint64_t swap_reads;        // Total pages read from swap
    uint64_t swap_writes;       // Total pages written to swap
} swap_state_t;

// ============================================
// Initialization
// ============================================

// Initialize demand paging subsystem
void demand_init(void);

// Initialize swap support (optional)
// Returns 0 on success, -1 on failure
int swap_init(const char *swap_path, uint64_t size_bytes);

// ============================================
// Page Fault Handling
// ============================================

// Main page fault handler - called from interrupt handler
// Returns 0 if fault was handled, -1 if process should be killed
int demand_page_fault(uint64_t fault_addr, uint64_t error_code, void *context);

// #429: per-process page-fault resolver used by the real #PF handler
// (mm/fault.c). Resolves a COW write, a demand-zero / lazy mmap page or a
// file-backed page for process `p`. Returns 0 if handled, -1 if the fault is
// invalid and the process should get SIGSEGV. Works for user- and kernel-mode
// faults (the latter only for a write to a user COW page: copy_to_user).
struct process;
int mm_fault(struct process *p, uint64_t fault_addr, uint64_t error_code);

// #429: copy-on-write a single page on a write fault. Copies (or, if this is
// the last owner, simply re-enables write on) the page mapped at page_addr in
// p's address space. Returns 0 on success, -1 on OOM.
int demand_cow_write(struct process *p, uint64_t page_addr);

// #429: fork COW bookkeeping. cow_trackable() reports whether a physical page
// falls inside the COW refcount table (all PMM-allocatable pages do).
// cow_fork_share() records that one more address space now shares `phys`.
int  cow_trackable(uint64_t phys_addr);
void cow_fork_share(uint64_t phys_addr);

// #429: COW-aware free of a user leaf page. If the page is COW-shared this
// decrements its refcount and frees only when it reaches zero; otherwise it
// frees immediately. vmm_destroy_user_space()/vmm_free_user_pages() route
// every user leaf page through this so a shared page is not freed out from
// under a sibling that still references it.
void vmm_free_user_page_cow(uint64_t phys_addr);

// #429: duplicate a memory map's VMA list for fork (shallow copy, no COW
// mutation - the physical COW is handled by the page-table clone). Returns a
// new mm or NULL.
mm_struct_t *mm_dup(mm_struct_t *src);

// #429: COW flag stored in the available bits of a leaf PTE (bit 9).
#define PTE_COW_FLAG   (1ULL << 9)

// Handle different types of page faults
int handle_lazy_fault(mm_struct_t *mm, vma_t *vma, uint64_t fault_addr);
int handle_cow_fault(mm_struct_t *mm, vma_t *vma, uint64_t fault_addr);
int handle_file_fault(mm_struct_t *mm, vma_t *vma, uint64_t fault_addr);
int handle_swap_fault(mm_struct_t *mm, vma_t *vma, uint64_t fault_addr);

// ============================================
// VMA Management
// ============================================

// Create a new VMA
vma_t *vma_create(uint64_t start, uint64_t end, uint32_t flags);

// Add VMA to process memory map
int vma_add(mm_struct_t *mm, vma_t *vma);

// Find VMA containing the given address
vma_t *vma_find(mm_struct_t *mm, uint64_t addr);

// Find VMA for a range (checks overlap)
vma_t *vma_find_range(mm_struct_t *mm, uint64_t start, uint64_t end);

// Split VMA at the given address
int vma_split(mm_struct_t *mm, vma_t *vma, uint64_t addr);

// Merge adjacent VMAs if possible
int vma_merge(mm_struct_t *mm, vma_t *vma);

// Remove and free VMA
void vma_remove(mm_struct_t *mm, vma_t *vma);

// Free all VMAs in memory map
void vma_free_all(mm_struct_t *mm);

// ============================================
// Memory Mapping (mmap)
// ============================================

// Map a region of virtual memory
// Returns start address on success, (uint64_t)-1 on failure
uint64_t do_mmap(mm_struct_t *mm, uint64_t addr, uint64_t length,
                 uint32_t prot, uint32_t flags, void *file, uint64_t offset);

// Unmap a region of virtual memory
int do_munmap(mm_struct_t *mm, uint64_t addr, uint64_t length);

// Change protection of a memory region
int do_mprotect(mm_struct_t *mm, uint64_t addr, uint64_t length, uint32_t prot);

// Synchronize memory-mapped file
int do_msync(mm_struct_t *mm, uint64_t addr, uint64_t length, int flags);

// ============================================
// Heap Management (brk/sbrk)
// ============================================

// Set program break (heap end)
// Returns new break on success, (uint64_t)-1 on failure
uint64_t do_brk(mm_struct_t *mm, uint64_t addr);

// Increment program break
void *do_sbrk(mm_struct_t *mm, int64_t increment);

// ============================================
// Copy-on-Write Support
// ============================================

// Mark all user pages as COW for fork
int cow_mark_all(mm_struct_t *mm, uint64_t pml4_phys);

// Clone memory map with COW
mm_struct_t *mm_clone_cow(mm_struct_t *src);

// Increment COW reference count for a page
void cow_page_ref(uint64_t phys_addr);

// Decrement COW reference count, free if zero
void cow_page_unref(uint64_t phys_addr);

// Check if page is COW-shared
int cow_page_shared(uint64_t phys_addr);

// ============================================
// Swap Operations
// ============================================

// Write page to swap
// Returns swap slot index on success, (uint32_t)-1 on failure
uint32_t swap_out_page(uint64_t phys_addr);

// Read page from swap
// Returns physical address on success, 0 on failure
uint64_t swap_in_page(uint32_t slot_index);

// Free a swap slot
void swap_free_slot(uint32_t slot_index);

// Check if swap is enabled
int swap_enabled(void);

// Get swap statistics
void swap_get_stats(uint32_t *total, uint32_t *free, uint64_t *reads, uint64_t *writes);

// ============================================
// Memory Map Management
// ============================================

// Create new memory map structure
mm_struct_t *mm_create(void);

// Destroy memory map and free all resources
void mm_destroy(mm_struct_t *mm);

// Clone memory map (for fork)
mm_struct_t *mm_clone(mm_struct_t *src);

// Print memory map for debugging
void mm_print(mm_struct_t *mm);

// ============================================
// Page Table Manipulation
// ============================================

// Mark page as COW (read-only, will copy on write)
int pte_mark_cow(uint64_t pml4_phys, uint64_t virt_addr);

// Mark page as lazy (not present, will allocate on access)
int pte_mark_lazy(uint64_t pml4_phys, uint64_t virt_addr);

// Mark page as swapped (store swap slot in PTE)
int pte_mark_swapped(uint64_t pml4_phys, uint64_t virt_addr, uint32_t swap_slot);

// Get page state from PTE
int pte_get_state(uint64_t pml4_phys, uint64_t virt_addr);

// Get swap slot from swapped PTE
uint32_t pte_get_swap_slot(uint64_t pml4_phys, uint64_t virt_addr);

// ============================================
// Statistics and Debugging
// ============================================

// Get demand paging statistics
void demand_get_stats(uint64_t *minor_faults, uint64_t *major_faults,
                      uint64_t *cow_faults, uint64_t *lazy_allocs);

// Print demand paging statistics
void demand_print_stats(void);

// Dump VMA list for debugging
void demand_dump_vmas(mm_struct_t *mm);

#endif // DEMAND_H
