// vma_scale_shim.h - #245: the freestanding scaffolding that lets the REAL
// kernel VMA-list code (mm/demand.c) be compiled and measured on the build
// host, with no QEMU round trip.
//
// WHY A NATIVE RIG. The question is "does the per-mmap and per-fault cost grow
// with the number of existing mappings", and that is a property of a linked
// list walk, not of the hardware. Measuring it in the OS costs a golden build
// and a boot per data point, which is why the first half of this ticket
// (b93cab6d, the userland allocator) used exactly this shape and could then
// afford a four-point curve. The in-OS numbers remain the ones that matter and
// are reported separately; this rig is what makes the mechanism attributable.
//
// NOTHING HERE REIMPLEMENTS THE CODE UNDER TEST. run_vma_scale.sh extracts
// vma_add_nolock/vma_find/vma_find_range_nolock/vma_merge/mm_scan_gap_locked/
// mm_place_anon_locked/do_mmap verbatim out of a given mm/demand.c, and the VMA
// flag defines and the vma_t/mm_struct_t layouts verbatim out of mm/demand.h,
// so an imitation cannot drift into the measurement. Only the things those
// functions call that are genuinely kernel-only are stubbed: the lock, kmalloc,
// the page-table punch and the current-process accessor.
#ifndef VMA_SCALE_SHIM_H
#define VMA_SCALE_SHIM_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// mm_struct_t carries a spinlock_t. The rig is single-threaded by construction,
// so the type only has to exist.
typedef int spinlock_t;

// VMA_* defines plus the real vma_t / mm_struct_t, cut from mm/demand.h.
#include "vma_defs.inc"

typedef struct { uint64_t cr3; uint32_t pid; } process_t;
static process_t g_rig_proc = { 0x1000, 1 };
static process_t *proc_current(void) { return &g_rig_proc; }

// rustkern/vmascale.rs in the kernel. Here it only has to exist and stay quiet:
// the rig's gate metric is the node-visit counter below, not a serial line.
static uint32_t vmascale_note_mmap(uint32_t vma_count, uint32_t merged)
{ (void)vma_count; (void)merged; return 0; }

#define VMM_PAGE_SIZE_4K    4096ULL
#define USER_SPACE_START    0x0000000000400000ULL
#define USER_WIN_BASE       0x0000008000000000ULL
#define USER_WIN_END        0x000000C000000000ULL
#define USER_WIN_MMAP_BASE  (USER_WIN_BASE + 0x0100000000ULL)
#define USER_WIN_STACK_TOP  (USER_WIN_BASE + 0x0200000000ULL)
#define MM_ANON_ARENA_BASE  USER_WIN_MMAP_BASE
#define MM_ANON_ARENA_END   USER_WIN_STACK_TOP
#define MM_USER_ADDR_MIN    USER_SPACE_START
#define MM_USER_ADDR_MAX    USER_WIN_END

// THE INSTRUMENT. One increment per VMA node visited by any list walk. This is
// the deterministic quantity the gate judges; wall clock on this host varies
// several-fold between runs and decides nothing, exactly as in
// userland/libc/tests/run_malloc_scale.sh.
uint64_t g_vmaw_steps = 0;
#define VMAW_STEP() (g_vmaw_steps++)

#define MM_REQUIRE_LOCK(mm, what) ((void)0)
#define MM_ANOMALY(...)           ((void)0)
#define kprintf(...)              ((void)0)

static void *kmalloc(unsigned long n)                { return malloc(n); }
static void  kfree(void *p)                          { free(p); }
static uint64_t mm_lock(mm_struct_t *mm)             { (void)mm; return 0; }
static void mm_unlock(mm_struct_t *mm, uint64_t f)   { (void)mm; (void)f; }
static void vmm_punch_demand_range(uint64_t pml4, uint64_t s, uint64_t l)
                                                     { (void)pml4; (void)s; (void)l; }

#endif // VMA_SCALE_SHIM_H
