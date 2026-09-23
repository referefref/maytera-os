// kstack.h - Ring-0 kernel stacks with a poisoned guard band and an
// O(1) overflow check that runs on every interrupt.
//
// WHY THIS EXISTS (2026-09-03, agent stackguard).
//
// Every Ring-0 kernel stack in this kernel was a plain kmalloc() with NOTHING
// below it. Overflowing one did not fault: it walked into whatever the heap had
// placed underneath and corrupted it silently. The rahang campaign caught the
// consequence 2 times in 5 boots, and a 10-boot baseline of the same rig
// reproduced it 3 times in 10 - the compositor kernel stack walked 304 bytes
// off its own base and overwrote its owner guard word with a return address,
// which surfaced far away as a [SCHEDRACE] panic naming AP scheduling. A
// mangled return address that manifests at random, in an unrelated subsystem,
// is close to the worst failure mode available, and it is what "no guard"
// buys you.
//
// WHY THIS IS A SOFTWARE GUARD AND NOT AN MMU GUARD PAGE. An unmapped page is
// the better mechanism and it was tried first. It does not work on this kernel,
// for a reason that is MEASURED rather than assumed: vmm_init() adopts the
// UEFI page tables and never builds its own (mm/vmm.c), the firmware maps its
// own page-table pages READ-ONLY, and CR0.WP is set. Punching a hole therefore
// means writing into the firmware PD, which faults: build 2350 died on every
// boot at mm/vmm.c:1240 (the huge-page split store) with
// "Page Fault (INT 14) KERNEL err=0x3 RIP=0x45cace CR2=0x7bc03350", where
// err=0x3 is Present + Write + Supervisor, i.e. a supervisor write to a
// read-only page, and CR3 that boot was 0x7bc01000 - the fault address is
// inside the firmware's own tables. Getting an MMU guard needs either clearing
// CR0.WP around every page-table write (a global protection downgrade, and the
// split path calls pmm_alloc_page() and a TLB shootdown inside the window) or
// the kernel building and installing its own page tables. Both are larger and
// riskier than the defect being fixed. See CHANGELOG for the follow-up.
//
// WHAT IS SHIPPED INSTEAD, and why it catches THIS defect earlier than a guard
// page would. Every kernel stack is a 64 KiB block aligned to 64 KiB, whose
// lowest 4 KiB is a poisoned guard band that the task never uses. Because the
// block is aligned to its own size, the band is derivable FROM RSP ALONE with
// one AND - no per-CPU state, no signature changes, correct on every core and
// every task automatically. isr_handler() tests it on every interrupt, so an
// overflow driven by nested interrupt frames (which is exactly this defect) is
// caught within ONE frame of crossing the boundary, roughly 400 bytes into a
// 4096-byte band, with the culprit still on the stack. A guard page would have
// caught it one frame later and via a #DF.
//
// THE RESIDUAL GAP, stated rather than glossed: a deep NON-interrupt recursion
// with frames larger than the band could step over the whole 4 KiB between two
// checks. An MMU guard page would catch that and this does not.
#ifndef MAYTERA_KSTACK_H
#define MAYTERA_KSTACK_H

#include "../types.h"

// One granule for every Ring-0 stack. Alignment == size is the whole trick:
// it is what makes the band derivable from RSP with a single mask.
#define KSTACK_GRAN  (64u * 1024u)
#define KSTACK_BAND  4096u                     // poisoned, never used by the task
#define KSTACK_USABLE (KSTACK_GRAN - KSTACK_BAND)

// A stack may span several granules (the async HTTPS workers ask for 128 KiB,
// the installer and cron for 256 KiB). The block is always aligned to ONE
// granule and the band always lives in the LOWEST one, which is what keeps the
// single-mask check below correct; see kstack_alloc(). The cap exists so a
// nonsense request degrades to a counted kmalloc rather than eating memory.
#define KSTACK_MAX_GRAN 16u

// Identifies a block as one of ours. Lives at block_base[0], the BOTTOM of the
// band, so an overflow crossing INTO the band from the top has to traverse the
// whole 4 KiB before it can destroy the thing that identifies the stack - and
// the check below fires long before that.
#define KSTACK_MAGIC 0x4B53544B475244ULL       // "KSTKGRD"

// Allocate a Ring-0 kernel stack. <size> is what the caller intends to use and
// must be <= KSTACK_USABLE; the block handed back is always KSTACK_GRAN and
// always KSTACK_GRAN-aligned. Returns the LOWEST usable byte, so the caller
// keeps using base + size as its top exactly as it did with kmalloc. Falls back
// to kmalloc when a granule cannot be obtained; the fallback is counted and
// reported, never silent.
void *kstack_alloc(size_t size);
void  kstack_free(void *base, size_t size);
int   kstack_is_guarded(const void *base);

// THE GUARD. One AND, one load, one compare. Returns non-zero when <rsp> is a
// kernel-stack address that has descended into its guard band.
//
// Reading block_base is always safe: it is at most KSTACK_GRAN below a valid
// kernel stack address, so it is inside identity-mapped RAM whether or not this
// particular stack came from here (the BSP boot stack in .bss and the AP
// per-CPU stacks from the PMM both read back a value that is not the magic).
static inline int kstack_rsp_in_band(uint64_t rsp) {
    uint64_t base = rsp & ~((uint64_t)KSTACK_GRAN - 1);
    if (*(volatile uint64_t *)base != KSTACK_MAGIC) return 0;   // not one of ours
    return (rsp - base) < (uint64_t)KSTACK_BAND;
}

// Called from isr_handler() when the check above fires. Reports and halts; does
// not return.
void kstack_overflow_panic(uint64_t rsp, uint64_t rip);

// Non-destructive structural check that a handed-out stack really is aligned,
// poisoned and identified. Cheap; runs on every boot including the golden.
void kstack_selftest(void);

// Only with `make KSTACKTEST=1`. Deliberately overflows a guarded stack and
// does not return. NEVER for a golden.
void kstack_overflow_selftest(void);

// The census: how many stacks are guarded, how many fell back and why.
void kstack_report(void);

#endif
