// rustkern/proc_mem.rs - #487/#349 Task Manager accessor: per-process memory accounting
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #487/#349 Task Manager kernel accessor tier 1: per-process MEMORY ACCOUNTING
// (proc_mem_account).
//
// WHY THIS EXISTS: proc_snapshot() computed mem_kb as ONLY p->user_stack_size,
// so EVERY user process reported a flat ~2 MB (USER_STACK_SIZE) regardless of
// real usage: the Task Manager "Memory" column was decorative. This seam
// computes a real breakdown (working set / private / virtual / heap) from the
// mm_struct_t the demand-paging layer already maintains (resident_pages is
// incremented per fault in mm/demand.c, total_mapped per VMA add/remove), plus
// the brk heap and the committed user stack.
//
// SAFETY VALUE (why Rust, not C): the interesting part is the VMA walk. vma_t
// is a kernel-heap LINKED LIST (mm->vma_list). The pre-existing C walkers
// (gui/taskmanager.c, mm/demand.c) follow `v = v->next` with NO iteration bound
// and NO cycle guard, so a corrupted or cyclic list is an infinite loop in Ring
// 0 with interrupts on: a hang, not a fault. This walk is HARD-BOUNDED at
// VMA_WALK_MAX nodes and reports truncation via a flag instead of spinning. It
// also rejects inverted/absurd extents (end <= start) rather than computing a
// wrapped length. Nothing here trusts mm->vma_count.
//
// Routed live under -DRUST_PROC_MEM (proc/process.c keeps proc_mem_account_c
// as the reference twin + rollback). Boot [RUST-DIFF] proc_mem proves rs == c.
// ===========================================================================

// Mirror of vma_t (mm/demand.h). Layout is ABI-locked by a _Static_assert on
// the C side (sizeof(vma_t) == 72) and the const assert below; `next` MUST sit
// at offset 56 or this walk reads garbage.
#[repr(C)]
pub struct VmaNode {
    pub start: u64,
    pub end: u64,
    pub flags: u32,
    pub prot: u32,
    pub file: *const core::ffi::c_void,
    pub file_offset: u64,
    pub file_size: u64,
    pub ref_count: u32,
    // 4 bytes tail padding here (C inserts it before the pointer)
    pub next: *const VmaNode,
    pub prev: *const VmaNode,
}
const _: () = assert!(core::mem::size_of::<VmaNode>() == 72);

#[repr(C)]
pub struct ProcMemIn {
    pub cr3: u64,
    pub user_stack_size: u64,
    pub brk: u64,
    pub brk_start: u64,
    pub resident_pages: u64,
    pub total_mapped: u64,
    pub vma_head: *const VmaNode,
    pub vma_count: u32,
    pub has_mm: u8,
    pub shares_vm: u8,
    pub privilege: u8,
    pub _pad: u8,
}
const _: () = assert!(core::mem::size_of::<ProcMemIn>() == 64);

#[repr(C)]
pub struct ProcMemOut {
    pub working_set_kb: u32,
    pub private_kb: u32,
    pub virt_kb: u32,
    pub heap_kb: u32,
    pub vma_walked: u32,
    pub flags: u32,
}
const _: () = assert!(core::mem::size_of::<ProcMemOut>() == 24);

/// Hard bound on the VMA walk. MAX_PROCESSES is 64 and a process's map is a
/// handful of regions; 4096 is ~64x any legitimate map and caps the walk cost
/// at a fixed, interrupt-friendly ceiling. Reaching it means the list is
/// corrupt or cyclic -> report PROC_MEM_F_TRUNC, never spin.
const VMA_WALK_MAX: u32 = 4096;
/// bit0: the VMA walk hit VMA_WALK_MAX (list corrupt/cyclic) and was cut short.
const PROC_MEM_F_TRUNC: u32 = 1;
/// bit1: at least one VMA had an inverted or zero extent and was skipped.
const PROC_MEM_F_BADVMA: u32 = 2;
/// A page is 4 KB on this kernel; resident_pages counts 4 KB pages.
const PAGE_KB: u64 = 4;

#[no_mangle]
pub extern "C" fn proc_mem_account_rs(inp: *const ProcMemIn, out: *mut ProcMemOut) -> i32 {
    if inp.is_null() || out.is_null() {
        return -1;
    }
    // SAFETY: both checked non-null; the caller (proc/process.c) passes stack
    // objects it owns for the duration of this call, and this function stores
    // no pointer beyond it.
    let i = unsafe { &*inp };
    let o = unsafe { &mut *out };
    o.working_set_kb = 0;
    o.private_kb = 0;
    o.virt_kb = 0;
    o.heap_kb = 0;
    o.vma_walked = 0;
    o.flags = 0;

    // Kernel processes (privilege 0) have no user address space: cr3 == 0 and
    // no user stack. Report zero rather than inventing a number.
    if i.cr3 == 0 {
        return 0;
    }

    // Committed user stack. Always counted: it is mapped at exec time and is
    // NOT represented in mm (mm is created lazily by brk/mmap only).
    let stack_kb = i.user_stack_size / 1024;

    // brk heap. brk_start is 0 when the process never called brk, in which case
    // there is no heap. saturating_sub: a brk below brk_start would otherwise
    // wrap to a nonsense multi-exabyte heap.
    let heap_bytes = if i.brk_start != 0 && i.brk > i.brk_start {
        i.brk - i.brk_start
    } else {
        0
    };
    let heap_kb = heap_bytes / 1024;

    // Bounded VMA walk. Sums the virtual extent actually described by the list
    // rather than trusting mm->total_mapped.
    let mut vma_bytes: u64 = 0;
    let mut walked: u32 = 0;
    if i.has_mm != 0 && !i.vma_head.is_null() {
        let mut cur = i.vma_head;
        while !cur.is_null() {
            if walked >= VMA_WALK_MAX {
                o.flags |= PROC_MEM_F_TRUNC; // corrupt/cyclic list: stop, do not spin
                break;
            }
            // SAFETY: `cur` is a kernel-heap vma_t from mm->vma_list, whose
            // layout this VmaNode mirrors (sizeof-locked both sides). The list
            // is only mutated under the caller's context and enumeration here
            // is read-only. The VMA_WALK_MAX bound above means a corrupted
            // `next` chain terminates instead of running forever.
            let v = unsafe { &*cur };
            if v.end > v.start {
                vma_bytes = vma_bytes.saturating_add(v.end - v.start);
            } else {
                o.flags |= PROC_MEM_F_BADVMA; // inverted/empty extent: skip it
            }
            walked += 1;
            cur = v.next;
        }
    }
    o.vma_walked = walked;

    // Working set = what is actually resident in RAM. mm->resident_pages counts
    // only demand-paged frames, so add the eagerly-mapped user stack: without
    // it a process that never faulted would report a 0 KB working set.
    let resident_kb = i.resident_pages.saturating_mul(PAGE_KB);
    o.working_set_kb = clamp_kb(resident_kb.saturating_add(stack_kb));

    // Private (commit) = stack + heap: the pages this process alone owns.
    o.private_kb = clamp_kb(stack_kb.saturating_add(heap_kb));

    // Virtual = everything reserved in the address space.
    o.virt_kb = clamp_kb(
        stack_kb
            .saturating_add(heap_kb)
            .saturating_add(vma_bytes / 1024),
    );
    o.heap_kb = clamp_kb(heap_kb);
    1
}

/// Saturate a KB quantity into the u32 the snapshot/FFI carries. A u64 KB value
/// above u32::MAX (4 TB) cannot be real on this kernel and means the inputs are
/// corrupt; pinning to u32::MAX keeps the column monotone and truthful-ish
/// rather than silently truncating to a small wrong number.
#[inline]
fn clamp_kb(v: u64) -> u32 {
    if v > u32::MAX as u64 { u32::MAX } else { v as u32 }
}
