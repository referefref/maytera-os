// vmm.h - Virtual Memory Manager (Paging)
#ifndef VMM_H
#define VMM_H

#include "../types.h"

// Page table entry flags
#define VMM_FLAG_PRESENT    (1ULL << 0)   // Page is present in memory
#define VMM_FLAG_WRITABLE   (1ULL << 1)   // Page is writable
#define VMM_FLAG_USER       (1ULL << 2)   // Page is accessible from user mode
#define VMM_FLAG_PWT        (1ULL << 3)   // Page-level write-through
#define VMM_FLAG_PCD        (1ULL << 4)   // Page-level cache disable
#define VMM_FLAG_ACCESSED   (1ULL << 5)   // Page has been accessed
#define VMM_FLAG_DIRTY      (1ULL << 6)   // Page has been written to
#define VMM_FLAG_HUGE       (1ULL << 7)   // Huge page (2MB/1GB)
#define VMM_FLAG_GLOBAL     (1ULL << 8)   // Global page (not flushed on CR3 switch)
#define VMM_FLAG_NX         (1ULL << 63)  // No-execute (NX bit)

// Write-combining for framebuffer memory (use PWT+PCD for uncached)\n#define VMM_WRITE_COMBINING (VMM_FLAG_PWT)

// Page sizes
#define VMM_PAGE_SIZE_4K    4096ULL
#define VMM_PAGE_SIZE_2M    (2ULL * 1024 * 1024)
#define VMM_PAGE_SIZE_1G    (1ULL * 1024 * 1024 * 1024)

// Address masks
#define VMM_ADDR_MASK       0x000FFFFFFFFFF000ULL  // Physical address mask
#define VMM_PML4_INDEX(a)   (((a) >> 39) & 0x1FF)
#define VMM_PDPT_INDEX(a)   (((a) >> 30) & 0x1FF)
#define VMM_PD_INDEX(a)     (((a) >> 21) & 0x1FF)
#define VMM_PT_INDEX(a)     (((a) >> 12) & 0x1FF)
#define VMM_PAGE_OFFSET(a)  ((a) & 0xFFF)

// Page table structure (512 entries of 8 bytes each = 4KB)
typedef uint64_t page_table_t[512] __attribute__((aligned(4096)));

// Initialize virtual memory manager
void vmm_init(void);

// Map a virtual address to a physical address
int vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);

// Map multiple contiguous pages
int vmm_map_pages(uint64_t virt_addr, uint64_t phys_addr, uint64_t count, uint64_t flags);

// Unmap a virtual address
void vmm_unmap_page(uint64_t virt_addr);

// Get the physical address for a virtual address (returns 0 if not mapped)
uint64_t vmm_get_physical(uint64_t virt_addr);

// Check if a virtual address is mapped
int vmm_is_mapped(uint64_t virt_addr);

// Invalidate TLB entry for a virtual address
static inline void vmm_invlpg(uint64_t virt_addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

// Flush entire TLB (reload CR3)
void vmm_flush_tlb(void);

// Get current PML4 physical address
uint64_t vmm_get_pml4(void);

// Switch to a different address space (CR3)
void vmm_switch_pml4(uint64_t pml4_phys);

// ============================================
// User-space memory management
// ============================================

// User-space virtual address ranges
#define USER_SPACE_START    0x0000000000400000ULL  // Start at 4MB
#define USER_SPACE_END      0x00007FFFFFFFFFFFULL  // End of user space (lower half)
#define USER_STACK_TOP      0x00000000BFFF0000ULL  // User stack at top of 2-3GB range
#define USER_STACK_SIZE     (2 * 1024 * 1024)       // 2MB default stack

// Common user-space flag combinations
#define VMM_USER_RO         (VMM_FLAG_PRESENT | VMM_FLAG_USER)
#define VMM_USER_RW         (VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE)
#define VMM_USER_RX         (VMM_FLAG_PRESENT | VMM_FLAG_USER)
#define VMM_USER_RWX        (VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE)

// Write combining for framebuffer memory
#define VMM_WRITE_COMBINING (VMM_FLAG_PWT)

// Create a new address space for a user process
// Returns physical address of new PML4, or 0 on failure
// The new address space includes kernel mappings in upper half
uint64_t vmm_create_user_space(void);

// Clone an address space (for fork)
// Creates copy-on-write mappings where possible
uint64_t vmm_clone_user_space(uint64_t src_pml4_phys);

// #429: real copy-on-write clone for fork. Instead of deep-copying every user
// page, the parent and child share the same physical pages marked read-only
// with the COW bit; the #PF handler copies a page on the first write from
// either side. The page-table STRUCTURE (PDPT/PD/PT pages) is still copied so
// the two address spaces are independent. Returns the child PML4, or 0.
uint64_t vmm_clone_user_space_cow(uint64_t src_pml4_phys);

// #429: return the raw leaf PTE (including flags such as PRESENT, WRITABLE, NX
// and the COW bit) mapping virt_addr in the given address space, or 0 if there
// is no present 4KB leaf entry. Used by the fault handler to distinguish a COW
// write from a genuine protection fault.
uint64_t vmm_get_pte_in(uint64_t pml4_phys, uint64_t virt_addr);

// Destroy a user address space and free all associated pages
void vmm_destroy_user_space(uint64_t pml4_phys);

// Map a page in a specific address space (not necessarily current)
int vmm_map_page_in(uint64_t pml4_phys, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);

// Unmap a page in a specific address space
void vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt_addr);

// Get physical address in a specific address space
uint64_t vmm_get_physical_in(uint64_t pml4_phys, uint64_t virt_addr);

// #500 / MAYTERA-SEC-2026-0016.
// Return the EFFECTIVE access rights of virt_addr in the address space rooted
// at pml4_phys, computed the way the hardware computes them: the U/S and R/W
// bits are ANDed across ALL four levels, and NX is ORed. Returns 0 if the page
// is not present at any level; otherwise VMM_FLAG_PRESENT plus the effective
// VMM_FLAG_USER / VMM_FLAG_WRITABLE / VMM_FLAG_NX bits.
//
// vmm_get_physical_in() is NOT a substitute: it reports only presence, so it
// says "mapped" for kernel memory that Ring 3 can never touch. MayteraOS
// identity-maps the kernel into the LOWER half (KERNEL_PHYS_BASE = 0x400000)
// and copies PML4[0] into every user address space, so a presence-only check
// cannot tell user memory from kernel memory. The U/S bit can; that is the
// only thing that can. Any user-pointer check MUST use this, not the other.
uint64_t vmm_get_effective_flags_in(uint64_t pml4_phys, uint64_t virt_addr);

// Allocate and map user pages at a virtual address
// Allocates physical memory and maps it with user-accessible flags
int vmm_alloc_user_pages(uint64_t pml4_phys, uint64_t virt_addr, uint64_t count, uint64_t flags);

// Free user pages and their physical memory
void vmm_free_user_pages(uint64_t pml4_phys, uint64_t virt_addr, uint64_t count);

#endif // VMM_H
