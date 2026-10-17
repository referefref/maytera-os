// heapshadow.h - #aicappersist: the physical window the kernel heap SHADOWS.
//
// The kernel assumes physical == virtual (UEFI identity map) and dereferences
// pmm_alloc_page()'s return value directly. mm/heap.c hands the virtual addresses
// in that window out as the heap's arena, so for a physical page inside it,
// `(uint64_t *)P` is a pointer INTO THE HEAP and clearing it as a page table
// overwrites a live heap object. See rustkern/heapshadow.rs for the measurement
// and the reasoning; the policy lives there, these are the declarations.
//
// CORRECTED 2026-10-01 (#deadpml4). This block said the reason was that mm/heap.c
// "replaces the identity entries for that window". IT DOES NOT, and this was the
// THIRD copy of that sentence: #heapwin corrected mm/heap.c and
// rustkern/heapshadow.rs the same day and missed this header. vmm_map_page()
// walks `kernel_pml4` (mm/vmm.c:14), a static .bss array vmm_init() zeroes and
// NEVER loads into CR3, so nothing it writes affects translation. The window
// keeps its UEFI identity mapping, which is WHY P aliases the heap: P is the
// heap's storage, not a frame the heap was remapped onto. Measured per page by
// heapframe_report_cost() in mm/heap.c.
#ifndef MAYTERA_HEAPSHADOW_H
#define MAYTERA_HEAPSHADOW_H

#include "../types.h"

// Publish the window (mm/heap.c owns the constants, so there is one definition).
void heapshadow_set_window_rs(uint64_t base, uint64_t max_size);

// 1 when the identity assumption is WRONG for this physical address.
int32_t heapshadow_is_shadowed_rs(uint64_t phys);

// Reservation accounting. `already` MUST be zero: a page inside the window that
// was already allocated is one the reservation came too late to protect.
void heapshadow_note_reserve_rs(int32_t rc);

// #heapdbl: the same, told whether the firmware types the page as usable RAM. A
// page that is "already used" because the firmware reserved it is NOT a corruption,
// and the one-bucket version called 2299 of them live corruptions on a 576 MB VM.
void heapshadow_note_reserve2_rs(int32_t rc, int32_t usable);
uint64_t heapshadow_firmware_count_rs(void);
void heapshadow_counts_rs(uint64_t *new_, uint64_t *already,
                          uint64_t *moot, uint64_t *viol);

// Count a site that used a shadowed physical address as a virtual one. Returns 1
// the first time only, so the caller logs once and loudly.
int32_t heapshadow_violation_rs(void);

// #heapdbl: does a RANGE overlap the window? The one definition of that question;
// fs/blockdev.c used to carry its own copy of the bounds.
int32_t heapshadow_range_overlaps_rs(uint64_t base, uint64_t len);

// A multi-page run that overlapped the window and was handed to a caller. NOT a
// violation: it is only a corruption if the caller dereferences it rather than
// rejecting it, which is what blk_alloc_chunks() does.
int32_t heapshadow_note_span_rs(void);
uint64_t heapshadow_span_count_rs(void);

int32_t heapshadow_selftest_rs(uint32_t *out_checks);

#endif // MAYTERA_HEAPSHADOW_H
