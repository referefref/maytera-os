// procmem.h - #487/#349 per-process memory accounting for Task Manager /
// Process Explorer.
//
// BACKGROUND: proc_snapshot() used to report mem_kb = p->user_stack_size and
// nothing else, so every user process showed a flat ~2 MB (USER_STACK_SIZE) no
// matter what it actually used. That made the Task Manager "Memory" column
// decorative. This module computes a real breakdown from state the kernel
// already maintains:
//   - mm->resident_pages : incremented/decremented per demand-page fault
//                          (mm/demand.c), i.e. what is really in RAM;
//   - mm->vma_list       : the mapped regions (walked, not trusted via
//                          vma_count / total_mapped);
//   - p->brk             : the program break (heap top);
//   - p->user_stack_size : the eagerly-committed user stack, which is NOT
//                          represented in mm (mm is created lazily by the first
//                          brk/mmap, so most processes have p->mm == NULL).
//
// The aggregation + the VMA walk live in Rust (proc_mem_account_rs in
// rustkern.rs) per the all-new-kernel-code-in-Rust rule. The C twin
// proc_mem_account_c is kept VERBATIM as the reference for the boot [RUST-DIFF]
// differential and as a one-line rollback (drop -DRUST_PROC_MEM).
//
// HONEST LIMITATION: the ELF text/data segments are not tracked by any existing
// kernel structure, so they are not counted. "working set" here means resident
// demand-paged frames + the committed user stack. It is a large improvement on
// a hardcoded constant, not a page-table walk.
#ifndef PROCMEM_H
#define PROCMEM_H

// Deliberately depends on types.h ALONE. This header is included by
// proc/process.c, which carries its own local `extern void mm_destroy(void*)` /
// `mm_dup(void*)` shims; pulling mm/demand.h in here would conflict with them
// and fail the -Werror build. The vma_t layout assert that needs demand.h lives
// in proc/procmem.c instead, where demand.h is safe to include.
#include "../types.h"

// Input gathered by the C caller from process_t + mm_struct_t.
typedef struct {
    uint64_t cr3;              // 0 = kernel process, no user address space
    uint64_t user_stack_size;  // committed user stack (bytes)
    uint64_t brk;              // current program break, 0 if never set
    uint64_t brk_start;        // heap base, 0 if the process never called brk
    uint64_t resident_pages;   // mm->resident_pages (4 KB pages), 0 if no mm
    uint64_t total_mapped;     // mm->total_mapped (informational; not trusted)
    const void *vma_head;      // mm->vma_list, or NULL
    uint32_t vma_count;        // mm->vma_count (informational; NOT a walk bound)
    uint8_t  has_mm;           // p->mm != NULL
    uint8_t  shares_vm;        // 1 = this is a thread sharing a leader's cr3
    uint8_t  privilege;        // PRIV_KERNEL (0) / PRIV_USER (3)
    uint8_t  _pad;
} proc_mem_in_t;
_Static_assert(sizeof(proc_mem_in_t) == 64, "proc_mem_in_t sizeof lock (Rust ProcMemIn)");

#define PROC_MEM_F_TRUNC   1u  // VMA walk hit its bound (list corrupt/cyclic)
#define PROC_MEM_F_BADVMA  2u  // >=1 VMA had an inverted/empty extent, skipped

typedef struct {
    uint32_t working_set_kb;   // resident in RAM (faulted pages + user stack)
    uint32_t private_kb;       // committed to this process alone (stack + heap)
    uint32_t virt_kb;          // total virtual reserved (stack + heap + VMAs)
    uint32_t heap_kb;          // brk heap alone
    uint32_t vma_walked;       // VMAs actually visited by the bounded walk
    uint32_t flags;            // PROC_MEM_F_*
} proc_mem_out_t;
_Static_assert(sizeof(proc_mem_out_t) == 24, "proc_mem_out_t sizeof lock (Rust ProcMemOut)");

// Rust port (rustkern.rs) and the C reference twin. Both return 1 on success,
// 0 for a kernel process (no user memory), -1 on a NULL argument.
int proc_mem_account_rs(const proc_mem_in_t *in, proc_mem_out_t *out);
int proc_mem_account_c(const proc_mem_in_t *in, proc_mem_out_t *out);

#ifdef RUST_PROC_MEM
#define proc_mem_account(i, o) proc_mem_account_rs((i), (o))
#else
#define proc_mem_account(i, o) proc_mem_account_c((i), (o))
#endif

// Gather one process's accounting inputs from the live PCB + its mm. Defined in
// proc/process.c, where process_t is complete. `p` is a process_t*, typed void*
// here so this header stays independent of process.h's include order.
void proc_mem_fill_in(const void *p, proc_mem_in_t *in);

// Public accessor for the Task Manager: gather `pid`'s memory breakdown.
// Returns 1 on success, 0 if the pid is unknown or is a kernel process.
int proc_mem_info(uint32_t pid, proc_mem_out_t *out);

// #404 boot-time [RUST-DIFF] differential for this seam.
void proc_mem_selftest(void);

#endif // PROCMEM_H
