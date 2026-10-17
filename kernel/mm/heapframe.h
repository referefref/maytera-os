// heapframe.h - #deadpml4: what the heap's page mappings COST and how far the
// heap may safely grow. The policy and the reasoning live in
// rustkern/heapframe.rs; these are the declarations.
#ifndef MAYTERA_HEAPFRAME_H
#define MAYTERA_HEAPFRAME_H

#include "../types.h"

// Publish the heap's virtual window (mm/heap.c owns the constants).
void heapframe_set_window_rs(uint64_t base, uint64_t max_size);
// The smallest arena the clamp may leave. A ceiling below it is reported, not
// enforced: see rustkern/heapframe.rs.
void heapframe_set_floor_rs(uint64_t min_arena);
int32_t  heapframe_floor_defeated_rs(void);
uint64_t heapframe_raw_ceiling_rs(void);
void heapframe_reset_rs(void);

// One page of the window. `ram` = the firmware types it as memory the kernel may
// write; `write_refused` = #procspawn measured Ring 0 as unable to write it.
void heapframe_note_window_page_rs(uint64_t virt, int32_t ram, int32_t write_refused);

// The highest address the heap's arena may reach, exclusive; 0 = window unknown.
uint64_t heapframe_ceiling_rs(void);
// 0 = the window's end, 1 = a write-refused page, 2 = a page that is not RAM,
// 3 = a page was found but it is below the floor, so the clamp is OFF.
int32_t  heapframe_ceiling_cause_rs(void);
void heapframe_scan_counts_rs(uint64_t *scanned, uint64_t *ro, uint64_t *notram,
                              uint64_t *first_ro, uint64_t *first_notram);

// How many bytes may be adopted at heap_end, page-aligned down; 0 = none.
uint64_t heapframe_clamp_rs(uint64_t heap_end, uint64_t want);

// Per-page cost verification. `sentinel`: 1 = the frame the dead hierarchy claims
// IS the heap's storage, 0 = it is not, -1 = not tested.
void heapframe_note_page_rs(uint64_t virt, uint64_t live_phys, uint64_t dead_phys,
                            int32_t sentinel);
void heapframe_page_counts_rs(uint64_t *pages, uint64_t *ident_ok,
                              uint64_t *ident_bad, uint64_t *first_ident_bad,
                              uint64_t *dead_out, uint64_t *dead_in,
                              uint64_t *dead_none, uint64_t *sent_diff,
                              uint64_t *sent_same, uint64_t *sent_skip,
                              uint64_t *first_sent_same);
// Frames allocated into the dead hierarchy that no virtual address reaches.
uint64_t heapframe_inert_frames_rs(void);

int32_t heapframe_selftest_rs(uint32_t *out_checks);

#endif // MAYTERA_HEAPFRAME_H
