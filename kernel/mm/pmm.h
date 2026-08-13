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

// ---------------------------------------------------------------------------
// #647: after-the-fact reservation of LIVE memory that has no linker symbol.
// pmm_init() can only re-reserve objects it can NAME. The UEFI page tables that
// vmm_init() adopts are reachable only through CR3, so nothing in pmm_init()
// could ever have covered them, and they sit in memory the bootloader typed as
// MEMORY_TYPE_USABLE / MEMORY_TYPE_BOOTLOADER, which is exactly what pmm_init()
// frees. See vmm_reserve_boot_page_tables().
// ---------------------------------------------------------------------------

// Mark a page used. 1 = newly reserved (it was free), 0 = already used,
// -1 = outside the allocator's range (moot).
int pmm_reserve_page(uint64_t phys_addr);

// 1 if the allocator would currently hand this page out.
int pmm_page_is_free(uint64_t phys_addr);

// Which boot memory-map entry covers this address, and what type is it?
// Returns MEMORY_TYPE_* or 0 if uncovered.
uint32_t pmm_mmap_type_of(uint64_t phys_addr, uint32_t *out_index,
                          uint64_t *out_base, uint64_t *out_len);
const char *pmm_mmap_type_name(uint32_t type);

// One past the highest physical address in the boot memory map.
uint64_t pmm_phys_limit(void);

// Diagnostic: dump the boot memory map (see the note in pmm.c on why descriptor
// ADJACENCY is the only raw-UEFI-type signal that survives the bootloader).
void pmm_dump_mmap(void);

#ifdef PTWALK_SELFTEST
// #647 self-test support, DEBUG BUILDS ONLY (`make PTWALKTEST=1`).
void pmm_selftest_snapshot(void);
void pmm_selftest_restore(void);
uint64_t pmm_selftest_drain(void (*cb)(uint64_t phys, uint64_t ordinal, void *ctx),
                            void *ctx);
#endif

#endif // PMM_H
