// tlbflush.h - cross-CPU TLB shootdown (#404)
//
// WHAT THIS REPLACES. cpu/smp.c carried smp_tlb_shootdown() with a definition,
// a declaration in cpu/smp.h, and ZERO call sites. Its own audit comment listed
// the three things missing: no receiver (IPI_VECTOR_TLB 0xF2 had no IDT gate
// and no assembly stub), no address transport (an IPI carries a vector and
// nothing else), and no acknowledgement. All three are here.
//
// THE INVARIANT THIS EXISTS TO KEEP: after tlb_flush_*() returns, no CPU in the
// machine holds a cached translation for the range. That is what makes it safe
// for the caller to then free the physical frame back to the PMM. Without it,
// a peer with a stale entry keeps writing through it into whatever the frame is
// reallocated to, which is silent corruption rather than a fault.
//
// DELIVERY IS REDUNDANT ON PURPOSE, which is the project's stated best pattern
// (CLAUDE.md: "a redundant, always-armed wake source, so no wake can ever be
// lost"). A request is delivered by BOTH:
//   1. a fixed-vector IPI on 0xF2, with a real IDT gate and a DEDICATED asm
//      stub, and
//   2. a cooperative poll, tlb_service_local(), placed in every loop where a
//      core can sit unable to take an interrupt: spinlock_acquire()'s spin
//      (which covers every irqsave spinlock in the kernel, mm_lock included)
//      and the Big Kernel Lock's contended spin.
// Either alone is sufficient; a lost IPI costs latency, not correctness.
//
// WHY THE STUB IS DEDICATED AND DOES NOT jmp isr_common. Every other vector
// enters isr_handler() (cpu/idt.c), which calls bkl_acquire() BEFORE dispatching
// to the registered C handler whenever g_smp_bkl_full is set. A shootdown IPI
// arriving at a core that is spinning for the BKL would therefore block in the
// wrapper and never reach the handler, while the sender - which may itself hold
// the BKL - spins for an acknowledgement that can never come. That is a
// guaranteed deadlock, and it is very probably also the mechanism behind #75's
// recorded-as-unexplained "1 of 3 cores stopped": the first core to take the
// stop IPI acquired the BKL in the wrapper and then halted holding it, after
// which every other core's stop-IPI handler blocked in bkl_acquire(). See the
// CHANGELOG entry. This handler takes NO lock of any kind and touches nothing
// but its own TLB and one shared word.
//
// SAFE FROM ANY CONTEXT. Invalidating a TLB entry is never semantically wrong,
// only occasionally wasted work, so servicing a request needs no lock, no
// allocation and no scheduler. That is the property that lets the cooperative
// poll sit inside the kernel's innermost lock primitive.

#ifndef MAYTERA_TLBFLUSH_H
#define MAYTERA_TLBFLUSH_H

#include "../types.h"

// Invalidate one 4 KiB page on every online CPU, and do not return until every
// other CPU has confirmed it. Call AFTER the page-table write, BEFORE the frame
// is freed or reused.
void tlb_flush_page(uint64_t virt_addr);

// Invalidate [start, end) on every online CPU. Ends are page-aligned outward.
void tlb_flush_range(uint64_t start, uint64_t end);

// Invalidate everything (non-global) on every online CPU. Used when an entire
// address space is torn down or a whole page table is replaced.
void tlb_flush_all(void);

// Cooperative delivery. Cheap when there is nothing pending: one load of one
// already-hot word and a predicted branch. Called from spin loops.
void tlb_service_local(void);

// The C half of the 0xF2 receiver. Called only from irq_smp_tlb (cpu/idt.asm).
void tlb_ipi_handler(void);

// One-line statistics, printed at boot after the self-test and on demand.
void tlb_report(const char *why);

// Boot-time proof that the receiver actually fires. Returns 0 on success.
// Prints what it measured either way; never silently passes.
int tlb_selftest(void);

// The stale-TLB stress test and its negative control. See tlbtest.c.
void tlb_stress_start(void);

// Gates, set from main.c after the FAT ESP is mounted.
//   g_tlb_shootdown_enable == 0 : NEGATIVE CONTROL. The local invalidation
//     still happens (so the initiating core is correct), but no IPI is sent and
//     no acknowledgement is waited for, which is exactly the pre-#404 kernel.
extern int g_tlb_shootdown_enable;
extern int g_tlb_verbose;
//   g_tlb_no_ipi == 1 : send NO IPI but still publish the request and wait for
//     it. Only the cooperative poll points can then complete a shootdown, which
//     is how the backstop arm is proven to work rather than assumed.
extern int g_tlb_no_ipi;

#endif
