// pmm.h - Physical Memory Manager
#ifndef PMM_H
#define PMM_H

#include "../types.h"

// Page size (4 KB)
#define PMM_PAGE_SIZE 4096
#define PMM_PAGE_SHIFT 12

// Physical memory manager functions
void pmm_init(uint64_t mem_map_addr, uint32_t mem_map_entries);

// Allocate a single physical page, returns physical address or 0 on failure
uint64_t pmm_alloc_page(void);

// Allocate multiple contiguous physical pages
uint64_t pmm_alloc_pages(uint64_t count);

// Free a physical page
void pmm_free_page(uint64_t phys_addr);

// Free multiple contiguous physical pages
void pmm_free_pages(uint64_t phys_addr, uint64_t count);

// Get memory statistics
uint64_t pmm_get_total_pages(void);
uint64_t pmm_get_free_pages(void);
uint64_t pmm_get_used_pages(void);

// Debug: print memory statistics
void pmm_print_stats(void);

#endif // PMM_H
