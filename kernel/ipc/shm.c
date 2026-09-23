// shm.c - Shared Memory IPC Implementation for MayteraOS
// Implements sys_shm_create, sys_shm_map, sys_shm_unmap, sys_shm_destroy

#include "shm.h"
#include "../proc/capgate.h"   // Stage 0: handle-ownership rule
#include "../proc/process.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"
#include "../serial.h"
#include "../string.h"
#include "../security/uaccess_smap.h"  // #19/#645: AC brackets for Ring-3 out-params

// Shared memory region table
static shm_region_t shm_regions[SHM_MAX_REGIONS];

// Base virtual address for shared memory mappings in user space
// We'll allocate from a specific range to avoid conflicts
#define SHM_VIRT_BASE       0x0000700000000000ULL
#define SHM_VIRT_SIZE       0x0000001000000000ULL  // 64GB range for shared memory

// Next available virtual address hint (per-process, simplified to global for now)
static uint64_t shm_next_vaddr = SHM_VIRT_BASE;

// ============================================================================
// Internal helpers
// ============================================================================

// Find a free virtual address range in the calling process
static uint64_t shm_find_vaddr(size_t size) {
    // Simple bump allocator - in a real implementation this would
    // track per-process allocations and find gaps
    uint64_t addr = shm_next_vaddr;
    shm_next_vaddr += (size + 0xFFF) & ~0xFFFULL;  // Page-align

    // Wrap around if we exceed the range
    if (shm_next_vaddr >= SHM_VIRT_BASE + SHM_VIRT_SIZE) {
        shm_next_vaddr = SHM_VIRT_BASE;
        addr = shm_next_vaddr;
        shm_next_vaddr += (size + 0xFFF) & ~0xFFFULL;
    }

    return addr;
}

// Find mapping for a specific process in a region
static shm_mapping_t *shm_find_mapping(shm_region_t *region, uint32_t pid) {
    shm_mapping_t *m = region->mappings;
    while (m) {
        if (m->pid == pid) {
            return m;
        }
        m = m->next;
    }
    return NULL;
}

// Add a mapping to a region
static shm_mapping_t *shm_add_mapping(shm_region_t *region, uint32_t pid, uint64_t vaddr) {
    shm_mapping_t *m = (shm_mapping_t *)kmalloc(sizeof(shm_mapping_t));
    if (!m) {
        return NULL;
    }

    m->pid = pid;
    m->virt_addr = vaddr;
    m->next = region->mappings;
    region->mappings = m;
    region->ref_count++;

    return m;
}

// Remove a mapping from a region
static void shm_remove_mapping(shm_region_t *region, uint32_t pid) {
    shm_mapping_t **pp = &region->mappings;
    while (*pp) {
        if ((*pp)->pid == pid) {
            shm_mapping_t *m = *pp;
            *pp = m->next;
            kfree(m);
            region->ref_count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

// ============================================================================
// Shared Memory API Implementation
// ============================================================================

void shm_init(void) {
    kprintf("[SHM] Initializing shared memory subsystem...\n");

    // Clear all regions
    memset(shm_regions, 0, sizeof(shm_regions));

    // Initialize region IDs
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        shm_regions[i].id = i;
        shm_regions[i].state = SHM_STATE_FREE;
    }

    kprintf("[SHM] Shared memory initialized (%d max regions, %dMB max size)\n",
            SHM_MAX_REGIONS, SHM_MAX_SIZE / (1024 * 1024));
}

int64_t sys_shm_create(size_t size, int flags) {
    process_t *p = proc_current();
    if (!p) {
        kprintf("[SHM] ERROR: sys_shm_create - no current process\n");
        return -1;
    }

    // Validate size
    if (size == 0 || size > SHM_MAX_SIZE) {
        kprintf("[SHM] ERROR: sys_shm_create - invalid size %lu\n", (uint64_t)size);
        return -1;
    }

    // Page-align the size
    size = (size + 0xFFF) & ~0xFFFULL;

    // Find a free region
    shm_region_t *region = NULL;
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        if (shm_regions[i].state == SHM_STATE_FREE) {
            region = &shm_regions[i];
            break;
        }
    }

    if (!region) {
        kprintf("[SHM] ERROR: sys_shm_create - no free regions\n");
        return -1;
    }

    // Allocate physical memory
    uint64_t num_pages = size / VMM_PAGE_SIZE_4K;
    uint64_t phys_addr = pmm_alloc_pages(num_pages);
    if (!phys_addr) {
        kprintf("[SHM] ERROR: sys_shm_create - failed to allocate %lu pages\n", num_pages);
        return -1;
    }

    // Zero the memory (important for security).
    //
    // FIXED (Stage 0, capstage0): this used a Linux higher-half direct-map
    // offset (phys_addr + 0xFFFF800000000000) that does NOT exist in
    // MayteraOS. This kernel keeps UEFI identity mapping, so physical ==
    // virtual in Ring 0 (CLAUDE.md: there is no PHYS_TO_VIRT macro), and the
    // PMM hands out pages below the 2GB identity cap. The higher-half pointer
    // was unmapped, so this memset took a kernel page fault and PANICKED on
    // the first sys_shm_create ever made in anger. It was latent only because
    // the two shm_create callers that exist (ipc_test, the dead
    // compositor_client double-buffer path) are not exercised on a normal
    // boot; the Stage 0 caphole harness is what first tripped it. Zero
    // through the identity map instead.
    void *temp_map = (void *)phys_addr;   // identity-mapped: phys == virt in Ring 0
    memset(temp_map, 0, size);

    // Initialize the region
    region->state = SHM_STATE_ALLOCATED;
    region->creator_pid = p->pid;
    region->phys_addr = phys_addr;
    region->size = size;
    region->flags = flags;
    region->ref_count = 0;
    region->mappings = NULL;

    // STAGE 0: stamp the creating thread group through the SAME normaliser the
    // map check uses, so the stamp and the check cannot drift.
    region->creator_tgid = capgate_tgid_of_rs(p->pid, p->tgid);

    kprintf("[SHM] Created region %d: size=%lu, phys=0x%lx, creator=%u tgid=%u\n",
            region->id, (uint64_t)size, phys_addr, p->pid,
            (unsigned)region->creator_tgid);

    return region->id;
}

int64_t sys_shm_map(int id, void **addr) {
    process_t *p = proc_current();
    if (!p) {
        kprintf("[SHM] ERROR: sys_shm_map - no current process\n");
        return -1;
    }

    // Validate ID
    if (id < 0 || id >= SHM_MAX_REGIONS) {
        kprintf("[SHM] ERROR: sys_shm_map - invalid id %d\n", id);
        return -1;
    }

    shm_region_t *region = &shm_regions[id];

    // Check region is valid
    if (region->state == SHM_STATE_FREE) {
        kprintf("[SHM] ERROR: sys_shm_map - region %d is not allocated\n", id);
        return -1;
    }

    // ======================================================================
    // STAGE 0 DEFECT 4 (docs/SYSTEM_CAPABILITY_API.md 1.7, section 12)
    // ======================================================================
    // OWNERSHIP. Before this, sys_shm_map() checked range, not-free,
    // not-already-mapped-by-me and SHM_FLAG_EXCLUSIVE, and NOTHING ELSE. There
    // was no credential check of any kind, so any Ring-3 process could map any
    // allocated region 0..63 and READ ANOTHER PROCESS'S MEMORY, and write it
    // too unless SHM_FLAG_READONLY was set (which only downgrades non-creators
    // to VMM_USER_RO). A 64-entry table is a loop, not a guess. That is a
    // straightforward cross-process memory-read primitive, which is why it is
    // in Stage 0 and not behind a later capability.
    //
    // WHY CREATOR-ONLY IS THE RIGHT RULE TODAY, AND WHAT IT WOULD COST IF IT
    // WERE NOT. Complete census of sys_shm_map() callers in the tree:
    //   userland/libc/compositor_client.c:283,447  window backing buffers
    //   userland/apps/ipc_test/main.c:24           the IPC demo
    // BOTH create the region and then map it THEMSELVES, in the same process.
    // The compositor never maps a client's region: `shm_id` appears nowhere in
    // userland/apps/compositor/. So there is no cross-process mapper in this
    // system today, and a creator-group rule breaks nothing that exists.
    //
    // NO SHARE LIST IS ADDED HERE, ON PURPOSE. The design sketched "an explicit
    // share list for the legitimate cross-process cases"; the census above says
    // there are none. Principle 7 of that document is that a capability with no
    // consumer becomes fiction, and this tree has the receipts: SVC_PERM_NET
    // and SVC_PERM_INPUT were defined, parsed, granted, printed and never
    // checked. When a real cross-process consumer appears, the share list is a
    // small addition to THIS function with a caller to justify it.
    //
    // A region whose creator_pid is 0 (kernel-created, or a slot allocated but
    // not yet stamped) is refused to every Ring-3 caller by capgate, so the
    // default is closed rather than open.
    if (!capgate_caller_owns(region->creator_pid, region->creator_tgid)) {
        capgate_note_refusal_rs(CAPGATE_K_SHM_OWNER);
        kprintf("[SHM] DENY: pid %u may not map region %d (creator %u)\n",
                p->pid, id, (unsigned)region->creator_pid);
        return -1;
    }
    capgate_note_allowed_rs(CAPGATE_K_SHM_OWNER);

    // Check if already mapped by this process
    if (shm_find_mapping(region, p->pid)) {
        kprintf("[SHM] ERROR: sys_shm_map - region %d already mapped by pid %u\n", id, p->pid);
        return -1;
    }

    // Check exclusive flag
    if ((region->flags & SHM_FLAG_EXCLUSIVE) && region->ref_count > 0) {
        kprintf("[SHM] ERROR: sys_shm_map - region %d is exclusive and already mapped\n", id);
        return -1;
    }

    // Find virtual address for mapping
    uint64_t vaddr = shm_find_vaddr(region->size);

    // Determine mapping flags
    uint64_t vmm_flags = VMM_USER_RW;
    if ((region->flags & SHM_FLAG_READONLY) && region->creator_pid != p->pid) {
        vmm_flags = VMM_USER_RO;
    }

    // Map the physical pages into the process's address space
    uint64_t num_pages = region->size / VMM_PAGE_SIZE_4K;
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_vaddr = vaddr + (i * VMM_PAGE_SIZE_4K);
        uint64_t page_paddr = region->phys_addr + (i * VMM_PAGE_SIZE_4K);

        if (vmm_map_page_in(p->cr3, page_vaddr, page_paddr, vmm_flags) != 0) {
            // Rollback any pages we mapped
            for (uint64_t j = 0; j < i; j++) {
                vmm_unmap_page_in(p->cr3, vaddr + (j * VMM_PAGE_SIZE_4K));
            }
            kprintf("[SHM] ERROR: sys_shm_map - failed to map page %lu\n", i);
            return -1;
        }
    }

    // Add mapping record
    shm_mapping_t *mapping = shm_add_mapping(region, p->pid, vaddr);
    if (!mapping) {
        // Rollback
        for (uint64_t i = 0; i < num_pages; i++) {
            vmm_unmap_page_in(p->cr3, vaddr + (i * VMM_PAGE_SIZE_4K));
        }
        kprintf("[SHM] ERROR: sys_shm_map - failed to create mapping record\n");
        return -1;
    }

    // Update region state
    if (region->state == SHM_STATE_ALLOCATED) {
        region->state = SHM_STATE_MAPPED;
    }

    // Return virtual address to caller
    // #19/#645: one store into a Ring-3 out-param.
    if (addr) {
        uaccess_ac_t __ac = uaccess_begin();
        *addr = (void *)vaddr;
        uaccess_end(__ac);
    }

    kprintf("[SHM] Mapped region %d at vaddr 0x%lx for pid %u (flags=0x%lx)\n",
            id, vaddr, p->pid, vmm_flags);

    return 0;
}

int64_t sys_shm_unmap(int id) {
    process_t *p = proc_current();
    if (!p) {
        kprintf("[SHM] ERROR: sys_shm_unmap - no current process\n");
        return -1;
    }

    // Validate ID
    if (id < 0 || id >= SHM_MAX_REGIONS) {
        kprintf("[SHM] ERROR: sys_shm_unmap - invalid id %d\n", id);
        return -1;
    }

    shm_region_t *region = &shm_regions[id];

    // Find mapping for this process
    shm_mapping_t *mapping = shm_find_mapping(region, p->pid);
    if (!mapping) {
        kprintf("[SHM] ERROR: sys_shm_unmap - region %d not mapped by pid %u\n", id, p->pid);
        return -1;
    }

    // Unmap the pages
    uint64_t num_pages = region->size / VMM_PAGE_SIZE_4K;
    for (uint64_t i = 0; i < num_pages; i++) {
        vmm_unmap_page_in(p->cr3, mapping->virt_addr + (i * VMM_PAGE_SIZE_4K));
    }

    // Remove mapping record
    uint64_t vaddr = mapping->virt_addr;
    shm_remove_mapping(region, p->pid);

    // Update state if no more mappings
    if (region->ref_count == 0) {
        region->state = SHM_STATE_ALLOCATED;
    }

    kprintf("[SHM] Unmapped region %d from vaddr 0x%lx for pid %u\n",
            id, vaddr, p->pid);

    return 0;
}

int64_t sys_shm_destroy(int id) {
    process_t *p = proc_current();
    if (!p) {
        kprintf("[SHM] ERROR: sys_shm_destroy - no current process\n");
        return -1;
    }

    // Validate ID
    if (id < 0 || id >= SHM_MAX_REGIONS) {
        kprintf("[SHM] ERROR: sys_shm_destroy - invalid id %d\n", id);
        return -1;
    }

    shm_region_t *region = &shm_regions[id];

    // Check region is valid
    if (region->state == SHM_STATE_FREE) {
        kprintf("[SHM] ERROR: sys_shm_destroy - region %d is not allocated\n", id);
        return -1;
    }

    // Only creator can destroy
    if (region->creator_pid != p->pid) {
        kprintf("[SHM] ERROR: sys_shm_destroy - pid %u is not creator of region %d (creator=%u)\n",
                p->pid, id, region->creator_pid);
        return -1;
    }

    // Unmap from all processes
    while (region->mappings) {
        shm_mapping_t *m = region->mappings;
        process_t *mp = proc_get(m->pid);

        if (mp) {
            // Unmap from this process
            uint64_t num_pages = region->size / VMM_PAGE_SIZE_4K;
            for (uint64_t i = 0; i < num_pages; i++) {
                vmm_unmap_page_in(mp->cr3, m->virt_addr + (i * VMM_PAGE_SIZE_4K));
            }
        }

        // Remove mapping record
        region->mappings = m->next;
        kfree(m);
        region->ref_count--;
    }

    // Free physical memory
    uint64_t num_pages = region->size / VMM_PAGE_SIZE_4K;
    pmm_free_pages(region->phys_addr, num_pages);

    kprintf("[SHM] Destroyed region %d (was %lu bytes at phys 0x%lx)\n",
            id, (uint64_t)region->size, region->phys_addr);

    // Mark region as free
    region->state = SHM_STATE_FREE;
    region->creator_pid = 0;
    region->phys_addr = 0;
    region->size = 0;
    region->flags = 0;
    region->ref_count = 0;
    region->mappings = NULL;

    return 0;
}

int64_t sys_shm_info(int id, size_t *size, uint32_t *ref_count) {
    // Validate ID
    if (id < 0 || id >= SHM_MAX_REGIONS) {
        return -1;
    }

    shm_region_t *region = &shm_regions[id];

    if (region->state == SHM_STATE_FREE) {
        return -1;
    }

    // #19/#645: two stores into Ring-3 out-params.
    {
        uaccess_ac_t __ac = uaccess_begin();
        if (size) {
            *size = region->size;
        }
        if (ref_count) {
            *ref_count = region->ref_count;
        }
        uaccess_end(__ac);
    }

    return 0;
}

void shm_cleanup_process(uint32_t pid) {
    kprintf("[SHM] Cleaning up shared memory for pid %u\n", pid);

    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        shm_region_t *region = &shm_regions[i];

        if (region->state == SHM_STATE_FREE) {
            continue;
        }

        // Remove any mappings by this process
        shm_mapping_t *mapping = shm_find_mapping(region, pid);
        if (mapping) {
            // Note: pages are already unmapped when process address space is destroyed
            shm_remove_mapping(region, pid);

            if (region->ref_count == 0) {
                region->state = SHM_STATE_ALLOCATED;
            }
        }

        // If this process created the region and it has no mappings, destroy it
        if (region->creator_pid == pid && region->ref_count == 0) {
            uint64_t num_pages = region->size / VMM_PAGE_SIZE_4K;
            pmm_free_pages(region->phys_addr, num_pages);

            kprintf("[SHM] Auto-destroyed orphan region %d (creator exited)\n", i);

            region->state = SHM_STATE_FREE;
            region->creator_pid = 0;
            region->phys_addr = 0;
            region->size = 0;
            region->flags = 0;
        }
    }
}
