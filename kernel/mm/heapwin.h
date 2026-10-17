// heapwin.h - #heapwin: WHO already owned a page inside the kernel heap's
// virtual window before heap_init() got there, and WHERE those pages sit.
//
// mm/heap.c's window reservation counts "ALREADY ALLOCATED (must be 0)" pages and
// warns that each one is "a live corruption this came too late to prevent". On a
// 576 MB VM that prints 1024 and is wrong about all 1024: they were reserved on
// purpose, inside vmm_init(), by vmm_reserve_boot_page_tables() (#647) and
// vmm_reserve_unwritable_identity_pages() (#procspawn), which are the only two
// things that run between pmm_init() and heap_init().
//
// This module records WHICH of those named owners claims each page, by ASKING the
// owners (vmm_is_boot_page_table / vmm_identity_write_refused) rather than
// inferring from the address, and keeps the pages as a coalesced run list so the
// heap can ask whether any of them fall inside the range it has actually mapped.
// The policy and the measurements live in rustkern/heapwin.rs; these are the
// declarations.
#ifndef MAYTERA_HEAPWIN_H
#define MAYTERA_HEAPWIN_H

#include "../types.h"

// Forget everything recorded. Called once, after the self-test, before the
// reservation loop.
void heapwin_reset_rs(void);

// Record one already-allocated window page and what the two existing owners say
// about it. `is_table` = vmm_is_boot_page_table(phys); `is_ro_block` =
// vmm_identity_write_refused(phys). Neither = UNEXPLAINED, which is the alarm.
void heapwin_note_rs(uint64_t phys, int32_t is_table, int32_t is_ro_block);

// Read the tallies back. Any pointer may be NULL. `unexplained` MUST be zero: a
// page no reservation in this kernel admits to owning, sitting inside the heap's
// window, is the case the original alarm was actually for.
void heapwin_counts_rs(uint64_t *table, uint64_t *ro_only, uint64_t *unexplained,
                       uint64_t *first_unexplained, uint32_t *runs,
                       uint32_t *overflow);

// Lowest / highest recorded page, 0 if none. The lowest sets the heap's headroom:
// it is the first one a growing heap would reach.
uint64_t heapwin_lowest_rs(void);
uint64_t heapwin_highest_rs(void);

// How many recorded pages fall in the half-open range [lo, hi)? Asked with
// [heap_start, heap_end) this is the live-hazard question; asked with
// [heap_start, heap_start + HEAP_MAX_SIZE) it is the latent one.
uint64_t heapwin_overlap_rs(uint64_t lo, uint64_t hi);

int32_t heapwin_selftest_rs(uint32_t *out_checks);

#endif // MAYTERA_HEAPWIN_H
