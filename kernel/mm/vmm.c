// vmm.c - Virtual Memory Manager implementation
// Implements 4-level paging for x86_64

#include "vmm.h"
#include "pmm.h"
#include "../serial.h"
#include "../string.h"
#include "../gui/syslog.h"

// Kernel PML4 table (page map level 4)
static page_table_t kernel_pml4 __attribute__((aligned(4096)));

// Pointer to current PML4 (physical address)
static uint64_t current_pml4_phys = 0;

// Kernel CR3 for syscall handler (accessible from assembly)
uint64_t g_kernel_cr3 = 0;

// #429: COW-aware free of a user leaf page (mm/demand.c). A COW-shared page is
// only returned to the PMM when its last owner drops it; otherwise it is freed
// immediately. Routing user-page frees through this stops a fork-shared page
// from being freed out from under a sibling.
extern void vmm_free_user_page_cow(uint64_t phys_addr);

// Helper: Get or create a page table entry
static uint64_t* vmm_get_or_create_entry(uint64_t *table, int index, uint64_t flags) {
    if (!(table[index] & VMM_FLAG_PRESENT)) {
        // Allocate a new page table
        uint64_t new_table = pmm_alloc_page();
        if (new_table == 0) {
            kprintf("[VMM] ERROR: Failed to allocate page table! (pmm_alloc returned 0)\n");
            return NULL;
        }

        // Clear the new table
        memset((void*)new_table, 0, 4096);

        // Set the entry with default flags (present, writable)
        table[index] = new_table | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | (flags & VMM_FLAG_USER);
    }

    // Return pointer to the next level table
    return (uint64_t*)(table[index] & VMM_ADDR_MASK);
}

// Initialize the virtual memory manager
void vmm_init(void) {
    kprintf("[VMM] Initializing virtual memory manager...\n");

    // Get current CR3 (UEFI's page tables)
    uint64_t uefi_cr3 = read_cr3();
    kprintf("[VMM] Current CR3: 0x%lx\n", uefi_cr3);

    // For now, we'll keep using UEFI's page tables which already have
    // identity mapping set up. This is the safest approach.
    //
    // In a more complete implementation, we would:
    // 1. Copy UEFI's page tables to our own
    // 2. Modify them as needed (higher-half kernel, etc.)
    // 3. Switch to our own page tables
    //
    // For the current stage, UEFI's identity mapping is sufficient
    // and allows us to access all physical memory directly.

    current_pml4_phys = uefi_cr3;
    g_kernel_cr3 = uefi_cr3;  // Store for syscall CR3 switching

    // Clear our kernel PML4 for future use
    memset(kernel_pml4, 0, sizeof(kernel_pml4));

    kprintf("[VMM] Using UEFI page tables (identity mapped)\n");
    kprintf("[VMM] Virtual memory manager initialized\n");
}

// Map a virtual address to a physical address with 4KB pages
int vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    // Align addresses to page boundary
    virt_addr &= ~0xFFFULL;
    phys_addr &= ~0xFFFULL;

    // Get PML4 entry
    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    uint64_t *pdpt = vmm_get_or_create_entry(kernel_pml4, pml4_idx, flags);
    if (!pdpt) return -1;

    // Get PDPT entry
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    uint64_t *pd = vmm_get_or_create_entry(pdpt, pdpt_idx, flags);
    if (!pd) return -1;

    // Get PD entry
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);

    // Check if this is a huge page
    if (pd[pd_idx] & VMM_FLAG_HUGE) {
        // Cannot map a 4KB page over a 2MB huge page without splitting
        kprintf("[VMM] WARNING: Cannot map 4KB page over 2MB huge page at 0x%lx\n", virt_addr);
        return -1;
    }

    uint64_t *pt = vmm_get_or_create_entry(pd, pd_idx, flags);
    if (!pt) return -1;

    // Set PT entry
    uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
    pt[pt_idx] = phys_addr | flags | VMM_FLAG_PRESENT;

    // Invalidate TLB for this address
    vmm_invlpg(virt_addr);

    return 0;
}

// Map multiple contiguous pages
int vmm_map_pages(uint64_t virt_addr, uint64_t phys_addr, uint64_t count, uint64_t flags) {
    for (uint64_t i = 0; i < count; i++) {
        if (vmm_map_page(virt_addr + i * VMM_PAGE_SIZE_4K,
                         phys_addr + i * VMM_PAGE_SIZE_4K, flags) != 0) {
            return -1;
        }
    }
    return 0;
}

// Unmap a virtual address
void vmm_unmap_page(uint64_t virt_addr) {
    virt_addr &= ~0xFFFULL;

    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    if (!(kernel_pml4[pml4_idx] & VMM_FLAG_PRESENT)) return;

    uint64_t *pdpt = (uint64_t*)(kernel_pml4[pml4_idx] & VMM_ADDR_MASK);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) return;

    uint64_t *pd = (uint64_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return;

    // Check if huge page
    if (pd[pd_idx] & VMM_FLAG_HUGE) {
        // Clear huge page entry
        pd[pd_idx] = 0;
    } else {
        uint64_t *pt = (uint64_t*)(pd[pd_idx] & VMM_ADDR_MASK);
        uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
        pt[pt_idx] = 0;
    }

    vmm_invlpg(virt_addr);
}

// Get physical address for a virtual address
uint64_t vmm_get_physical(uint64_t virt_addr) {
    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    if (!(kernel_pml4[pml4_idx] & VMM_FLAG_PRESENT)) return 0;

    uint64_t *pdpt = (uint64_t*)(kernel_pml4[pml4_idx] & VMM_ADDR_MASK);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) return 0;

    // Check for 1GB huge page
    if (pdpt[pdpt_idx] & VMM_FLAG_HUGE) {
        return (pdpt[pdpt_idx] & VMM_ADDR_MASK) | (virt_addr & 0x3FFFFFFF);
    }

    uint64_t *pd = (uint64_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return 0;

    // Check for 2MB huge page
    if (pd[pd_idx] & VMM_FLAG_HUGE) {
        return (pd[pd_idx] & VMM_ADDR_MASK) | (virt_addr & 0x1FFFFF);
    }

    uint64_t *pt = (uint64_t*)(pd[pd_idx] & VMM_ADDR_MASK);
    uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
    if (!(pt[pt_idx] & VMM_FLAG_PRESENT)) return 0;

    return (pt[pt_idx] & VMM_ADDR_MASK) | VMM_PAGE_OFFSET(virt_addr);
}

// Check if a virtual address is mapped
int vmm_is_mapped(uint64_t virt_addr) {
    return vmm_get_physical(virt_addr) != 0;
}

// Flush entire TLB
void vmm_flush_tlb(void) {
    write_cr3(read_cr3());
}

// Get current PML4 physical address
uint64_t vmm_get_pml4(void) {
    return current_pml4_phys;
}

// Switch to a different address space
void vmm_switch_pml4(uint64_t pml4_phys) {
    current_pml4_phys = pml4_phys;
    write_cr3(pml4_phys);
}

// ============================================
// User-space memory management
// ============================================

// Helper: Get or create a page table entry in a specific address space
static uint64_t* vmm_get_or_create_entry_in(uint64_t *table, int index, uint64_t flags) {
    if (!(table[index] & VMM_FLAG_PRESENT)) {
        // Allocate a new page table
        uint64_t new_table = pmm_alloc_page();
        if (new_table == 0) {
            kprintf("[VMM] ERROR: Failed to allocate page table! (pmm_alloc returned 0)\n");
            return NULL;
        }

        // Clear the new table
        memset((void*)new_table, 0, 4096);

        // Set the entry with default flags
        table[index] = new_table | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | (flags & VMM_FLAG_USER);
    }

    // Return pointer to the next level table
    return (uint64_t*)(table[index] & VMM_ADDR_MASK);
}

// Create a new address space for a user process
uint64_t vmm_create_user_space(void) {
    // Allocate PML4
    uint64_t pml4_phys = pmm_alloc_page();
    if (pml4_phys == 0) {
        kprintf("[VMM] Failed to allocate PML4 for user space\n");
        return 0;
    }

    uint64_t *pml4 = (uint64_t*)pml4_phys;
    memset(pml4, 0, 4096);

    // Copy ONLY kernel mappings from current address space (upper half)
    // PML4 entries 256-511 are for kernel space (0xFFFF800000000000+)
    // DO NOT copy lower half - it contains 2MB huge pages that conflict with user 4KB pages
    uint64_t *current_pml4 = (uint64_t*)current_pml4_phys;

    // Only copy kernel entries (256-511), leave user space (0-255) empty
    // User pages will be mapped with 4KB granularity
    for (int i = 256; i < 512; i++) {
        pml4[i] = current_pml4[i];
    }
    
    // CRITICAL: Deep copy PML4[0] with new PDPT to avoid huge page conflicts
    // The kernel's PML4[0] contains a PDPT that has 2MB huge pages at 2GB range
    // We need to map 4KB user pages at 2GB, which conflicts with huge pages
    // Solution: Create a NEW PDPT, copy all entries EXCEPT entry 2 (2GB-3GB range)
    if (current_pml4[0] != 0) {
        uint64_t new_pdpt_phys = pmm_alloc_page();
        if (new_pdpt_phys == 0) {
            kprintf("[VMM] Failed to allocate new PDPT\n");
            pmm_free_page(pml4_phys);
            return 0;
        }
        
        uint64_t *new_pdpt = (uint64_t*)new_pdpt_phys;
        memset(new_pdpt, 0, 4096);
        
        uint64_t *kernel_pdpt = (uint64_t*)(current_pml4[0] & 0xFFFFFFFFFFFFF000ULL);
        
        // Copy PDPT entries EXCEPT entries 0-2
        // Keep entries 0-1 for kernel stack access during context switch
        // Clear only entry 2 (2-3GB) for user code/data
        // Entries 3+ are framebuffer/MMIO (keep from kernel)
        // We clear these to use fresh page tables (allocated from identity-mapped memory)
        // Entries 3+ point to framebuffer/MMIO which we can keep
        for (int i = 0; i < 512; i++) {
            if (i == 2) {
                // PDPT[2] covers the 2-3GB virtual/physical range.
                // Always create a fresh PD with 2MB huge pages for the identity
                // mapping. This avoids inheriting kernel PT pointers that may
                // reference physical pages above the PMM 2GB limit (which are
                // not writable via the kernel identity mapping).
                // When elf_load_user maps 4KB user pages here, vmm_map_page_in
                // will break individual 2MB entries into fresh PT pages.
                uint64_t new_pd_phys = pmm_alloc_page();
                if (new_pd_phys) {
                    uint64_t *new_pd = (uint64_t*)new_pd_phys;
                    for (int j = 0; j < 512; j++) {
                        new_pd[j] = (0x80000000ULL + (uint64_t)j * 0x200000ULL)
                            | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_HUGE;
                    }
                    new_pdpt[2] = new_pd_phys | VMM_FLAG_PRESENT
                        | VMM_FLAG_WRITABLE | VMM_FLAG_USER;
                } else {
                    new_pdpt[2] = 0;
                }
            } else {
                new_pdpt[i] = kernel_pdpt[i];
            }
        }
        
        // CRITICAL: Set PML4[0] with USER flag so user-mode can access
        pml4[0] = new_pdpt_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;
        kprintf("[VMM] Deep copied PML4[0], cleared PDPT[2] for user space at 2GB\n");
    }

    kprintf("[VMM] Created user address space: PML4=0x%lx\n", pml4_phys);
    return pml4_phys;
}

// Clone an address space (for fork)
uint64_t vmm_clone_user_space(uint64_t src_pml4_phys) {
    if (src_pml4_phys == 0) {
        return vmm_create_user_space();
    }

    // Allocate new PML4
    uint64_t dst_pml4_phys = pmm_alloc_page();
    if (dst_pml4_phys == 0) {
        return 0;
    }

    uint64_t *src_pml4 = (uint64_t*)src_pml4_phys;
    uint64_t *dst_pml4 = (uint64_t*)dst_pml4_phys;

    // CRITICAL: Zero the freshly allocated PML4. pmm_alloc_page() returns
    // pages with arbitrary stale contents (often the contents of a recently
    // freed page). The loops below only WRITE entries where src has a PRESENT
    // mapping, so any stale PRESENT-looking garbage in entries 0-255 (user
    // half) would survive into the child's PML4. vmm_destroy_user_space then
    // walks those entries, dereferences the stale non-canonical addresses, and
    // #GPs. Zero up front so only entries we explicitly copy are live.
    memset(dst_pml4, 0, 4096);

    // For now, do a simple deep copy of all user-space pages
    // TODO: Implement copy-on-write for efficiency
    for (int i = 0; i < 256; i++) {  // Only user space (lower half)
        if (src_pml4[i] & VMM_FLAG_PRESENT) {
            // Allocate new PDPT
            uint64_t *src_pdpt = (uint64_t*)(src_pml4[i] & VMM_ADDR_MASK);
            uint64_t new_pdpt_phys = pmm_alloc_page();
            if (!new_pdpt_phys) goto fail;

            uint64_t *dst_pdpt = (uint64_t*)new_pdpt_phys;
            memset(dst_pdpt, 0, 4096);

            for (int j = 0; j < 512; j++) {
                if (src_pdpt[j] & VMM_FLAG_PRESENT) {
                    if (src_pdpt[j] & VMM_FLAG_HUGE) {
                        // 1GB page - copy reference (kernel pages)
                        dst_pdpt[j] = src_pdpt[j];
                    } else {
                        // Need to copy PD
                        uint64_t *src_pd = (uint64_t*)(src_pdpt[j] & VMM_ADDR_MASK);
                        uint64_t new_pd_phys = pmm_alloc_page();
                        if (!new_pd_phys) goto fail;

                        uint64_t *dst_pd = (uint64_t*)new_pd_phys;
                        memset(dst_pd, 0, 4096);

                        for (int k = 0; k < 512; k++) {
                            if (src_pd[k] & VMM_FLAG_PRESENT) {
                                if (src_pd[k] & VMM_FLAG_HUGE) {
                                    // 2MB page - for user pages, allocate new
                                    if (src_pd[k] & VMM_FLAG_USER) {
                                        uint64_t new_page = pmm_alloc_pages(512);  // 2MB
                                        if (!new_page) goto fail;
                                        memcpy((void*)new_page,
                                               (void*)(src_pd[k] & VMM_ADDR_MASK),
                                               VMM_PAGE_SIZE_2M);
                                        dst_pd[k] = new_page | (src_pd[k] & 0xFFF);
                                    } else {
                                        dst_pd[k] = src_pd[k];  // Kernel page
                                    }
                                } else {
                                    // Need to copy PT
                                    uint64_t *src_pt = (uint64_t*)(src_pd[k] & VMM_ADDR_MASK);
                                    uint64_t new_pt_phys = pmm_alloc_page();
                                    if (!new_pt_phys) goto fail;

                                    uint64_t *dst_pt = (uint64_t*)new_pt_phys;
                                    memset(dst_pt, 0, 4096);

                                    for (int l = 0; l < 512; l++) {
                                        if (src_pt[l] & VMM_FLAG_PRESENT) {
                                            if (src_pt[l] & VMM_FLAG_USER) {
                                                // User page - allocate new
                                                uint64_t new_page = pmm_alloc_page();
                                                if (!new_page) goto fail;
                                                memcpy((void*)new_page,
                                                       (void*)(src_pt[l] & VMM_ADDR_MASK),
                                                       VMM_PAGE_SIZE_4K);
                                                dst_pt[l] = new_page | (src_pt[l] & 0xFFF);
                                            } else {
                                                dst_pt[l] = src_pt[l];  // Kernel page
                                            }
                                        }
                                    }

                                    dst_pd[k] = new_pt_phys | (src_pd[k] & 0xFFF);
                                }
                            }
                        }

                        dst_pdpt[j] = new_pd_phys | (src_pdpt[j] & 0xFFF);
                    }
                }
            }

            dst_pml4[i] = new_pdpt_phys | (src_pml4[i] & 0xFFF);
        }
    }

    // Copy kernel space entries (upper half, entries 256-511)
    for (int i = 256; i < 512; i++) {
        dst_pml4[i] = src_pml4[i];
    }

    return dst_pml4_phys;

fail:
    kprintf("[VMM] Failed to clone user space\n");
    vmm_destroy_user_space(dst_pml4_phys);
    return 0;
}

// #429: real copy-on-write clone for fork. Mirrors vmm_clone_user_space's
// page-table copy but at the leaf shares the physical page read-only with the
// COW bit set in BOTH parent and child; the #PF handler copies on first write.
uint64_t vmm_clone_user_space_cow(uint64_t src_pml4_phys) {
    // Declared in mm/demand.c.
    extern int  cow_trackable(uint64_t phys);
    extern void cow_fork_share(uint64_t phys);
    const uint64_t VMM_COW_BIT = (1ULL << 9);  // #429 leaf COW marker

    if (src_pml4_phys == 0) return vmm_create_user_space();

    uint64_t dst_pml4_phys = pmm_alloc_page();
    if (dst_pml4_phys == 0) return 0;

    uint64_t *src_pml4 = (uint64_t*)src_pml4_phys;
    uint64_t *dst_pml4 = (uint64_t*)dst_pml4_phys;
    memset(dst_pml4, 0, 4096);
    int parent_pte_changed = 0;

    for (int i = 0; i < 256; i++) {  // user half only
        if (!(src_pml4[i] & VMM_FLAG_PRESENT)) continue;

        uint64_t *src_pdpt = (uint64_t*)(src_pml4[i] & VMM_ADDR_MASK);
        uint64_t new_pdpt_phys = pmm_alloc_page();
        if (!new_pdpt_phys) goto fail;
        uint64_t *dst_pdpt = (uint64_t*)new_pdpt_phys;
        memset(dst_pdpt, 0, 4096);

        for (int j = 0; j < 512; j++) {
            if (!(src_pdpt[j] & VMM_FLAG_PRESENT)) continue;
            if (src_pdpt[j] & VMM_FLAG_HUGE) {
                dst_pdpt[j] = src_pdpt[j];   // 1GB kernel page, share by ref
                continue;
            }
            uint64_t *src_pd = (uint64_t*)(src_pdpt[j] & VMM_ADDR_MASK);
            uint64_t new_pd_phys = pmm_alloc_page();
            if (!new_pd_phys) goto fail;
            uint64_t *dst_pd = (uint64_t*)new_pd_phys;
            memset(dst_pd, 0, 4096);

            for (int k = 0; k < 512; k++) {
                if (!(src_pd[k] & VMM_FLAG_PRESENT)) continue;
                if (src_pd[k] & VMM_FLAG_HUGE) {
                    if (src_pd[k] & VMM_FLAG_USER) {
                        // 2MB user huge page: eager-copy (COW refcount is
                        // per-4KB; keep it simple and correct).
                        uint64_t np = pmm_alloc_pages(512);
                        if (!np) goto fail;
                        memcpy((void*)np, (void*)(src_pd[k] & VMM_ADDR_MASK),
                               VMM_PAGE_SIZE_2M);
                        dst_pd[k] = np | (src_pd[k] & 0xFFF);
                    } else {
                        dst_pd[k] = src_pd[k];   // kernel huge page
                    }
                    continue;
                }
                uint64_t *src_pt = (uint64_t*)(src_pd[k] & VMM_ADDR_MASK);
                uint64_t new_pt_phys = pmm_alloc_page();
                if (!new_pt_phys) goto fail;
                uint64_t *dst_pt = (uint64_t*)new_pt_phys;
                memset(dst_pt, 0, 4096);

                for (int l = 0; l < 512; l++) {
                    if (!(src_pt[l] & VMM_FLAG_PRESENT)) continue;
                    if (!(src_pt[l] & VMM_FLAG_USER)) {
                        dst_pt[l] = src_pt[l];   // kernel leaf, share by ref
                        continue;
                    }
                    uint64_t phys = src_pt[l] & VMM_ADDR_MASK;
                    if (cow_trackable(phys)) {
                        // Share read-only + COW in both address spaces.
                        uint64_t cow = (src_pt[l] & ~VMM_FLAG_WRITABLE) | VMM_COW_BIT;
                        dst_pt[l] = cow;
                        if (src_pt[l] != cow) {
                            src_pt[l] = cow;     // demote parent to read-only
                            parent_pte_changed = 1;
                        }
                        cow_fork_share(phys);
                    } else {
                        // Untrackable page: fall back to an eager private copy.
                        uint64_t np = pmm_alloc_page();
                        if (!np) goto fail;
                        memcpy((void*)np, (void*)phys, VMM_PAGE_SIZE_4K);
                        dst_pt[l] = np | (src_pt[l] & 0xFFF);
                    }
                }
                dst_pd[k] = new_pt_phys | (src_pd[k] & 0xFFF);
            }
            dst_pdpt[j] = new_pd_phys | (src_pdpt[j] & 0xFFF);
        }
        dst_pml4[i] = new_pdpt_phys | (src_pml4[i] & 0xFFF);
    }

    // Kernel half: share by value.
    for (int i = 256; i < 512; i++) dst_pml4[i] = src_pml4[i];

    // The parent's PTEs were demoted to read-only; flush the TLB so the
    // demotion takes effect before the parent runs again. fork() always runs in
    // the parent's context, so the live hardware CR3 (read_cr3()) IS the
    // parent's; flush unconditionally rather than trusting the software
    // current_pml4_phys shadow, which can lag the scheduler's context switches.
    // Without this the parent keeps a stale writable TLB entry, writes its
    // fork() return value straight into the still-shared page, and the child
    // then reads a NON-ZERO fork() return (runs the parent branch). #429.
    if (parent_pte_changed) {
        vmm_flush_tlb();
    }
    return dst_pml4_phys;

fail:
    kprintf("[VMM] COW clone failed\n");
    vmm_destroy_user_space(dst_pml4_phys);
    return 0;
}

// Destroy a user address space and free all associated pages
void vmm_destroy_user_space(uint64_t pml4_phys) {
    if (pml4_phys == 0 || pml4_phys == current_pml4_phys) {
        return;  // Don't destroy current or null address space
    }

    uint64_t *pml4 = (uint64_t*)pml4_phys;

    // Reference PDPT for PML4[0] taken from the CURRENT (kernel/parent) address
    // space. vmm_create_user_space copies the kernel's PDPT entries BY VALUE for
    // every slot except slot 2 (the 2-3GB user region), which it allocates
    // fresh. The copied slots therefore point at page tables that are SHARED
    // with the kernel and every other process. Freeing them here returns live
    // kernel/sibling page-table pages to the PMM, which then get handed back out
    // and corrupt unrelated memory (e.g. another process's stack). So we only
    // free page-table structures this address space actually OWNS: a PDPT slot
    // is owned when its value differs from the reference address space.
    uint64_t *ref_pml4 = (uint64_t*)current_pml4_phys;
    uint64_t *ref_pdpt0 = NULL;
    if (ref_pml4 && (ref_pml4[0] & VMM_FLAG_PRESENT)) {
        ref_pdpt0 = (uint64_t*)(ref_pml4[0] & VMM_ADDR_MASK);
    }

    // Free user space pages only (lower half, entries 0-255)
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & VMM_FLAG_PRESENT)) continue;

        uint64_t *pdpt = (uint64_t*)(pml4[i] & VMM_ADDR_MASK);
        uint64_t *ref_pdpt = (i == 0) ? ref_pdpt0 : NULL;

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & VMM_FLAG_PRESENT)) continue;

            // Determine ownership. With a reference PDPT, a slot is owned if its
            // value differs from the reference (shared kernel slots match). With
            // no reference available, only the fresh 2-3GB user slot (PML4[0],
            // PDPT[2]) is known to be owned.
            int owned;
            if (ref_pdpt) {
                owned = (pdpt[j] != ref_pdpt[j]);
            } else {
                owned = (i == 0 && j == 2);
            }
            if (!owned) continue;

            if (pdpt[j] & VMM_FLAG_HUGE) continue;  // 1GB huge: nothing to walk

            uint64_t *pd = (uint64_t*)(pdpt[j] & VMM_ADDR_MASK);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & VMM_FLAG_PRESENT)) continue;
                if (!(pd[k] & VMM_FLAG_HUGE)) {
                    uint64_t *pt = (uint64_t*)(pd[k] & VMM_ADDR_MASK);
                    for (int l = 0; l < 512; l++) {
                        if ((pt[l] & VMM_FLAG_PRESENT) &&
                            (pt[l] & VMM_FLAG_USER)) {
                            vmm_free_user_page_cow(pt[l] & VMM_ADDR_MASK);  // #429 COW-aware
                        }
                    }
                    pmm_free_page((uint64_t)pt);
                } else if (pd[k] & VMM_FLAG_USER) {
                    pmm_free_pages(pd[k] & VMM_ADDR_MASK, 512);
                }
            }

            pmm_free_page((uint64_t)pd);
        }

        // The PDPT page itself is freshly allocated per address space in
        // vmm_create_user_space, so it is always owned and safe to free.
        pmm_free_page((uint64_t)pdpt);
    }

    pmm_free_page(pml4_phys);
}

// Map a page in a specific address space
int vmm_map_page_in(uint64_t pml4_phys, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    if (pml4_phys == 0) {
        return -1;
    }

    uint64_t *pml4 = (uint64_t*)pml4_phys;

    virt_addr &= ~0xFFFULL;
    phys_addr &= ~0xFFFULL;

    // Get or create PML4 entry
    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    uint64_t *pdpt = vmm_get_or_create_entry_in(pml4, pml4_idx, flags);
    if (!pdpt) { kprintf("[VMM] Failed at PDPT creation, pml4_idx=%d\n", (int)pml4_idx); return -1; }
    if (!pdpt) return -1;

    // Get or create PDPT entry
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    uint64_t *pd = vmm_get_or_create_entry_in(pdpt, pdpt_idx, flags);
    if (!pd) { kprintf("[VMM] Failed at PD creation, pdpt_idx=%d\n", (int)pdpt_idx); return -1; }
    if (!pd) return -1;

    // Get or create PD entry
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    if (pd[pd_idx] & VMM_FLAG_HUGE) {
        // Break 2MB huge page into 512 individual 4KB pages
        uint64_t huge_phys = pd[pd_idx] & VMM_ADDR_MASK;
        uint64_t huge_flags = pd[pd_idx] & 0xFFFULL & ~VMM_FLAG_HUGE;
        uint64_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return -1;
        uint64_t *pt_new = (uint64_t *)pt_phys;
        for (int j = 0; j < 512; j++) {
            pt_new[j] = (huge_phys + j * 4096) | huge_flags | VMM_FLAG_PRESENT;
        }
        pd[pd_idx] = pt_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | (flags & VMM_FLAG_USER);
    }
    uint64_t *pt = vmm_get_or_create_entry_in(pd, pd_idx, flags);
    if (!pt) { kprintf("[VMM] Failed at PT creation, pd_idx=%d\n", (int)pd_idx); return -1; }
    if (!pt) return -1;

    // Set PT entry
    uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
    pt[pt_idx] = phys_addr | flags | VMM_FLAG_PRESENT;

    // Invalidate TLB if this is the current address space
    if (pml4_phys == current_pml4_phys) {
        vmm_invlpg(virt_addr);
    }

    return 0;
}

// Unmap a page in a specific address space
void vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt_addr) {
    if (pml4_phys == 0) return;

    uint64_t *pml4 = (uint64_t*)pml4_phys;
    virt_addr &= ~0xFFFULL;

    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    if (!(pml4[pml4_idx] & VMM_FLAG_PRESENT)) return;

    uint64_t *pdpt = (uint64_t*)(pml4[pml4_idx] & VMM_ADDR_MASK);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) return;

    uint64_t *pd = (uint64_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return;

    if (pd[pd_idx] & VMM_FLAG_HUGE) {
        pd[pd_idx] = 0;
    } else {
        uint64_t *pt = (uint64_t*)(pd[pd_idx] & VMM_ADDR_MASK);
        uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
        pt[pt_idx] = 0;
    }

    if (pml4_phys == current_pml4_phys) {
        vmm_invlpg(virt_addr);
    }
}

// Get physical address in a specific address space
// #500 / MAYTERA-SEC-2026-0016. See the contract note in vmm.h.
//
// This mirrors the CPU's own permission computation. For a Ring-3 access the
// hardware permits it only if the U/S bit is set at EVERY level of the walk,
// and permits a write only if R/W is set at every level. So we AND those bits
// down the walk and OR the NX bits. Checking only the leaf would be wrong in
// both directions, and checking only presence (as vmm_get_physical_in does) is
// what let a user pointer naming kernel memory pass validation.
uint64_t vmm_get_effective_flags_in(uint64_t pml4_phys, uint64_t virt_addr) {
    if (pml4_phys == 0) return 0;

    // Start permissive and AND the restrictions in, level by level.
    uint64_t eff_user = VMM_FLAG_USER;
    uint64_t eff_write = VMM_FLAG_WRITABLE;
    uint64_t eff_nx = 0;

    uint64_t *pml4 = (uint64_t *)pml4_phys;
    uint64_t e = pml4[VMM_PML4_INDEX(virt_addr)];
    if (!(e & VMM_FLAG_PRESENT)) return 0;
    eff_user &= e; eff_write &= e; eff_nx |= (e & VMM_FLAG_NX);

    uint64_t *pdpt = (uint64_t *)(e & VMM_ADDR_MASK);
    e = pdpt[VMM_PDPT_INDEX(virt_addr)];
    if (!(e & VMM_FLAG_PRESENT)) return 0;
    eff_user &= e; eff_write &= e; eff_nx |= (e & VMM_FLAG_NX);
    if (e & VMM_FLAG_HUGE) {   // 1GB page: the walk ends here
        return VMM_FLAG_PRESENT | eff_user | eff_write | eff_nx;
    }

    uint64_t *pd = (uint64_t *)(e & VMM_ADDR_MASK);
    e = pd[VMM_PD_INDEX(virt_addr)];
    if (!(e & VMM_FLAG_PRESENT)) return 0;
    eff_user &= e; eff_write &= e; eff_nx |= (e & VMM_FLAG_NX);
    if (e & VMM_FLAG_HUGE) {   // 2MB page: the walk ends here
        return VMM_FLAG_PRESENT | eff_user | eff_write | eff_nx;
    }

    uint64_t *pt = (uint64_t *)(e & VMM_ADDR_MASK);
    e = pt[VMM_PT_INDEX(virt_addr)];
    if (!(e & VMM_FLAG_PRESENT)) return 0;
    eff_user &= e; eff_write &= e; eff_nx |= (e & VMM_FLAG_NX);

    return VMM_FLAG_PRESENT | eff_user | eff_write | eff_nx;
}

uint64_t vmm_get_physical_in(uint64_t pml4_phys, uint64_t virt_addr) {
    if (pml4_phys == 0) return 0;

    uint64_t *pml4 = (uint64_t*)pml4_phys;

    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    if (!(pml4[pml4_idx] & VMM_FLAG_PRESENT)) return 0;

    uint64_t *pdpt = (uint64_t*)(pml4[pml4_idx] & VMM_ADDR_MASK);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) return 0;

    if (pdpt[pdpt_idx] & VMM_FLAG_HUGE) {
        return (pdpt[pdpt_idx] & VMM_ADDR_MASK) | (virt_addr & 0x3FFFFFFF);
    }

    uint64_t *pd = (uint64_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return 0;

    if (pd[pd_idx] & VMM_FLAG_HUGE) {
        return (pd[pd_idx] & VMM_ADDR_MASK) | (virt_addr & 0x1FFFFF);
    }

    uint64_t *pt = (uint64_t*)(pd[pd_idx] & VMM_ADDR_MASK);
    uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
    if (!(pt[pt_idx] & VMM_FLAG_PRESENT)) return 0;

    return (pt[pt_idx] & VMM_ADDR_MASK) | VMM_PAGE_OFFSET(virt_addr);
}

// #429: return the raw leaf PTE (flags + phys) mapping virt_addr, or 0 if there
// is no present 4KB leaf. Huge (2MB/1GB) mappings return 0 (no 4KB leaf).
uint64_t vmm_get_pte_in(uint64_t pml4_phys, uint64_t virt_addr) {
    if (pml4_phys == 0) return 0;
    uint64_t *pml4 = (uint64_t*)pml4_phys;

    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    if (!(pml4[pml4_idx] & VMM_FLAG_PRESENT)) return 0;

    uint64_t *pdpt = (uint64_t*)(pml4[pml4_idx] & VMM_ADDR_MASK);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) return 0;
    if (pdpt[pdpt_idx] & VMM_FLAG_HUGE) return 0;

    uint64_t *pd = (uint64_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return 0;
    if (pd[pd_idx] & VMM_FLAG_HUGE) return 0;

    uint64_t *pt = (uint64_t*)(pd[pd_idx] & VMM_ADDR_MASK);
    uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
    if (!(pt[pt_idx] & VMM_FLAG_PRESENT)) return 0;
    return pt[pt_idx];
}

// Allocate and map user pages at a virtual address
int vmm_alloc_user_pages(uint64_t pml4_phys, uint64_t virt_addr, uint64_t count, uint64_t flags) {
    virt_addr &= ~0xFFFULL;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t page = pmm_alloc_page();
        if (page == 0) {
            kprintf("[VMM] alloc_user_pages: pmm_alloc_page failed at page %llu\n", i);
            LOG_ERROR("[VMM] Out of physical memory for user pages");
            // Failed - free what we allocated
            vmm_free_user_pages(pml4_phys, virt_addr, i);
            return -1;
        }

        memset((void*)page, 0, VMM_PAGE_SIZE_4K);

        if (vmm_map_page_in(pml4_phys, virt_addr + i * VMM_PAGE_SIZE_4K,
                           page, flags | VMM_FLAG_USER) != 0) {
            kprintf("[VMM] alloc_user_pages: vmm_map_page_in failed for vaddr=0x%llX\n",
                    virt_addr + i * VMM_PAGE_SIZE_4K);
            LOG_ERROR("[VMM] Failed to map page in user address space");
            pmm_free_page(page);
            vmm_free_user_pages(pml4_phys, virt_addr, i);
            return -1;
        }
    }

    return 0;
}

// Free user pages and their physical memory
void vmm_free_user_pages(uint64_t pml4_phys, uint64_t virt_addr, uint64_t count) {
    virt_addr &= ~0xFFFULL;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t addr = virt_addr + i * VMM_PAGE_SIZE_4K;
        uint64_t phys = vmm_get_physical_in(pml4_phys, addr);
        if (phys) {
            vmm_unmap_page_in(pml4_phys, addr);
            vmm_free_user_page_cow(phys);  // #429 COW-aware
        }
    }
}

// ============================================================================
// DEBUG instrumentation: use-after-free detector for user pages.
// Scans the CURRENTLY-LIVE user address space (current_pml4_phys) for a
// USER-flagged mapping of `phys`. Returns the VA it is mapped at, or 0 if not
// mapped. pmm_free_page() calls this to catch a page that is still live in the
// running process (e.g. the terminal) being wrongly returned to the physical
// allocator during another process's teardown. Only walks PML4[0] -> PDPT[2]
// (the 2-3GB user region) where all user code/data/stack live, so it is cheap.
// ============================================================================
uint64_t vmm_dbg_user_va_for_phys(uint64_t phys) {
    uint64_t live = current_pml4_phys;
    if (live == 0) return 0;
    phys &= VMM_ADDR_MASK;

    // Structural pages of the live address space. Freeing any of these
    // corrupts the running process even though no leaf scan would find it.
    // Sentinels: 0xT1=PML4 page, 0xT2=PDPT page, 0xT3=PD page, 0xT4=PT page.
    if ((live & VMM_ADDR_MASK) == phys) return 0xD1000000000ULL; /* PML4 page */

    uint64_t *pml4 = (uint64_t*)live;
    if (!(pml4[0] & VMM_FLAG_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t*)(pml4[0] & VMM_ADDR_MASK);
    if ((pml4[0] & VMM_ADDR_MASK) == phys) return 0xD2000000000ULL; /* PDPT page */

    if (!(pdpt[2] & VMM_FLAG_PRESENT)) return 0;
    if (pdpt[2] & VMM_FLAG_HUGE) return 0;
    uint64_t *pd = (uint64_t*)(pdpt[2] & VMM_ADDR_MASK);
    if ((pdpt[2] & VMM_ADDR_MASK) == phys) return 0xD3000000000ULL; /* PD page */

    for (int k = 0; k < 512; k++) {
        if (!(pd[k] & VMM_FLAG_PRESENT)) continue;
        if (pd[k] & VMM_FLAG_HUGE) {
            if ((pd[k] & VMM_FLAG_USER) &&
                (pd[k] & VMM_ADDR_MASK) <= phys &&
                phys < ((pd[k] & VMM_ADDR_MASK) + 0x200000ULL)) {
                return 0x80000000ULL | ((uint64_t)k << 21);
            }
            continue;
        }
        uint64_t *pt = (uint64_t*)(pd[k] & VMM_ADDR_MASK);
        if ((pd[k] & VMM_ADDR_MASK) == phys) return 0xD4000000000ULL | ((uint64_t)k << 21); /* PT page */
        for (int l = 0; l < 512; l++) {
            if (!(pt[l] & VMM_FLAG_PRESENT)) continue;
            if (!(pt[l] & VMM_FLAG_USER)) continue;
            if ((pt[l] & VMM_ADDR_MASK) == phys) {
                return 0x80000000ULL | ((uint64_t)k << 21) | ((uint64_t)l << 12);
            }
        }
    }
    return 0;
}
