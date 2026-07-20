// procmem.c - #487/#349 per-process memory accounting: C reference twin,
// the public accessor, and the boot-time [RUST-DIFF] differential.
//
// The live aggregation is Rust (proc_mem_account_rs, rustkern.rs) under
// -DRUST_PROC_MEM. proc_mem_account_c below is the VERBATIM reference twin:
// it exists to (a) prove the Rust matches it on this exact build at boot and
// (b) be the instant rollback when the flag is dropped. Keep the two
// semantically identical; if this twin must ever diverge, NAME the divergence
// here rather than letting it be inferred (blame.md, 2026-07-16).
#include "procmem.h"
#include "process.h"
#include "../serial.h"     // kprintf
#include "../mm/demand.h"  // vma_t / mm_struct_t (see procmem.h for why not there)
#include "../mm/vmm.h"     // USER_SPACE_START, for the drift guard below

// vma_t layout is mirrored by VmaNode in rustkern.rs, which walks the live list
// in Ring 0. If this fires, the Rust mirror MUST move with it or the walk reads
// garbage (notably `next` must stay at offset 56).
_Static_assert(sizeof(vma_t) == 72, "vma_t layout changed: update VmaNode in rustkern.rs");

// The heap base is spelled as a literal in process.h (which deliberately does
// not include vmm.h). This is the one place both are visible: if either moves,
// the build fails here rather than silently reporting a wrong heap size.
_Static_assert(PROC_DEFAULT_BRK_START == (USER_SPACE_START + 0x100000),
               "PROC_DEFAULT_BRK_START drifted from USER_SPACE_START + 1MB");

// Same bound as VMA_WALK_MAX in rustkern.rs. Kept in lockstep by the
// differential below, which drives cyclic lists through both implementations.
#define C_VMA_WALK_MAX 4096u
#define C_PAGE_KB      4ull

static uint32_t c_clamp_kb(uint64_t v) {
    return (v > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)v;
}

// Reference twin of proc_mem_account_rs. NOTE: this twin is itself BOUNDED and
// saturating. It is NOT a transcription of the old unbounded C walkers in
// gui/taskmanager.c / mm/demand.c: there was no prior C implementation of this
// accounting to be faithful to (mem_kb was a one-line constant), so the twin is
// written to the same contract as the Rust rather than to a legacy behavior.
// That means the differential proves rs == c, not rs == some historical bug.
int proc_mem_account_c(const proc_mem_in_t *in, proc_mem_out_t *out) {
    if (!in || !out) return -1;
    out->working_set_kb = 0;
    out->private_kb = 0;
    out->virt_kb = 0;
    out->heap_kb = 0;
    out->vma_walked = 0;
    out->flags = 0;
    if (in->cr3 == 0) return 0;   // kernel process: no user address space

    uint64_t stack_kb = in->user_stack_size / 1024;

    uint64_t heap_bytes = 0;
    if (in->brk_start != 0 && in->brk > in->brk_start)
        heap_bytes = in->brk - in->brk_start;
    uint64_t heap_kb = heap_bytes / 1024;

    uint64_t vma_bytes = 0;
    uint32_t walked = 0;
    if (in->has_mm && in->vma_head) {
        const vma_t *cur = (const vma_t *)in->vma_head;
        while (cur) {
            if (walked >= C_VMA_WALK_MAX) { out->flags |= PROC_MEM_F_TRUNC; break; }
            if (cur->end > cur->start) {
                uint64_t add = cur->end - cur->start;
                if (vma_bytes + add < vma_bytes) vma_bytes = 0xFFFFFFFFFFFFFFFFull;
                else vma_bytes += add;
            } else {
                out->flags |= PROC_MEM_F_BADVMA;
            }
            walked++;
            cur = cur->next;
        }
    }
    out->vma_walked = walked;

    uint64_t resident_kb = in->resident_pages * C_PAGE_KB;
    out->working_set_kb = c_clamp_kb(resident_kb + stack_kb);
    out->private_kb     = c_clamp_kb(stack_kb + heap_kb);
    out->virt_kb        = c_clamp_kb(stack_kb + heap_kb + vma_bytes / 1024);
    out->heap_kb        = c_clamp_kb(heap_kb);
    return 1;
}

// #487/#349: gather one process's memory-accounting inputs from the live PCB.
// Lives here rather than in proc/process.c because it needs the real
// mm_struct_t from mm/demand.h, which process.c cannot include (it carries its
// own conflicting void*-typed mm_destroy/mm_dup shims).
//
// See procmem.h for why mm->brk_start is deliberately NOT used: mm_create()
// memsets the mm to zero and nothing ever assigns brk_start, so it reads 0 for
// every process. p->brk is authoritative.
void proc_mem_fill_in(const void *pv, proc_mem_in_t *in) {
    if (!in) return;
    in->cr3 = 0; in->user_stack_size = 0; in->brk = 0; in->brk_start = 0;
    in->resident_pages = 0; in->total_mapped = 0; in->vma_head = 0;
    in->vma_count = 0; in->has_mm = 0; in->shares_vm = 0; in->privilege = 0;
    in->_pad = 0;
    const process_t *p = (const process_t *)pv;
    if (!p) return;
    in->cr3 = p->cr3;
    in->user_stack_size = p->user_stack_size;
    in->brk = p->brk;
    // The heap exists only once sys_brk has initialized p->brk; before that
    // there is no heap and brk_start must stay 0 so the seam reports 0 KB.
    in->brk_start = p->brk ? PROC_DEFAULT_BRK_START : 0;
    in->shares_vm = p->shares_vm;
    in->privilege = p->privilege;
    const mm_struct_t *mm = (const mm_struct_t *)p->mm;
    if (mm) {
        in->has_mm = 1;
        in->resident_pages = mm->resident_pages;
        in->total_mapped = mm->total_mapped;
        in->vma_head = (const void *)mm->vma_list;
        in->vma_count = mm->vma_count;
    }
}

// Gather `pid`'s inputs from the live tables and run the (Rust) accounting.
int proc_mem_info(uint32_t pid, proc_mem_out_t *out) {
    if (!out) return 0;
    process_t *p = proc_get(pid);
    if (!p) return 0;
    proc_mem_in_t in;
    proc_mem_fill_in(p, &in);
    return proc_mem_account(&in, out) == 1 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Boot-time [RUST-DIFF] differential.
//
// Corpus design (blame.md: "the generator is part of the test"; a corpus that
// cannot express the bug proves nothing at any vector count). This seam's
// interesting states are the ones a naive implementation gets WRONG, so the
// corpus is built to reach each of them deliberately, and each is counted so a
// corpus regression shows up as a coverage collapse instead of a silent PASS:
//   - cr3 == 0            (kernel process -> must report all-zero, rc 0)
//   - brk < brk_start     (must NOT wrap to a multi-exabyte heap)
//   - brk_start == 0      (never called brk -> no heap)
//   - has_mm but NULL head
//   - inverted VMA extent (end <= start -> BADVMA, must not wrap)
//   - a CYCLIC vma list   (must terminate via TRUNC, not hang)
//   - resident_pages huge (must saturate, not truncate)
// ---------------------------------------------------------------------------
static uint32_t pm_rng_state = 0x1337beefu;
static uint32_t pm_rand(void) {
    pm_rng_state ^= pm_rng_state << 13;
    pm_rng_state ^= pm_rng_state >> 17;
    pm_rng_state ^= pm_rng_state << 5;
    return pm_rng_state;
}

void proc_mem_selftest(void) {
    // A small arena of VMAs the vectors can wire into lists, including a cycle.
    static vma_t arena[8];
    int mism = 0, vecs = 0;
    // coverage counters
    int cov_kernel = 0, cov_brkwrap = 0, cov_nobrk = 0, cov_nullhead = 0;
    int cov_badvma = 0, cov_cycle = 0, cov_sat = 0;

    for (int iter = 0; iter < 600; iter++) {
        proc_mem_in_t in;
        in.cr3 = (iter % 7 == 0) ? 0 : 0x100000ull;      // every 7th = kernel proc
        in.user_stack_size = (uint64_t)(pm_rand() % 0x400000u);
        in.brk_start = (iter % 5 == 0) ? 0 : 0x40100000ull;
        // every 3rd vector drives brk BELOW brk_start (the wrap trap)
        in.brk = (iter % 3 == 0) ? 0x40000000ull
                                 : 0x40100000ull + (uint64_t)(pm_rand() % 0x800000u);
        in.resident_pages = (iter % 11 == 0) ? 0xFFFFFFFFFFFFull   // saturation trap
                                             : (uint64_t)(pm_rand() % 4096u);
        in.total_mapped = 0;
        in.vma_count = 0;
        in.has_mm = (iter % 4 != 0);
        in.shares_vm = 0;
        in.privilege = in.cr3 ? 3 : 0;
        in._pad = 0;
        in.vma_head = 0;

        int nv = (int)(pm_rand() % 5);   // 0..4 VMAs
        if (in.has_mm && nv > 0) {
            for (int k = 0; k < nv; k++) {
                arena[k].start = 0x10000000ull + (uint64_t)k * 0x10000ull;
                arena[k].end   = arena[k].start + 0x1000ull * (1 + (pm_rand() % 16));
                if (iter % 13 == 0 && k == 0) {          // inverted extent trap
                    arena[k].end = arena[k].start;
                }
                arena[k].flags = 0; arena[k].prot = 0; arena[k].file = 0;
                arena[k].file_offset = 0; arena[k].file_size = 0; arena[k].ref_count = 1;
                arena[k].prev = 0;
                arena[k].next = (k + 1 < nv) ? &arena[k + 1] : 0;
            }
            if (iter % 29 == 0 && nv >= 2) {             // CYCLE trap
                arena[nv - 1].next = &arena[0];
                cov_cycle++;
            }
            in.vma_head = &arena[0];
            in.vma_count = (uint32_t)nv;
        } else if (in.has_mm) {
            cov_nullhead++;
        }

        if (in.cr3 == 0) cov_kernel++;
        if (in.brk_start != 0 && in.brk < in.brk_start) cov_brkwrap++;
        if (in.brk_start == 0) cov_nobrk++;
        if (iter % 13 == 0 && in.has_mm && nv > 0) cov_badvma++;
        if (in.resident_pages > 0xFFFFFFFFull) cov_sat++;

        proc_mem_out_t oc, orr;
        int rc_c  = proc_mem_account_c(&in, &oc);
        int rc_rs = proc_mem_account_rs(&in, &orr);
        vecs++;
        // Field-by-field, never memcmp: C tail padding is indeterminate and a
        // whole-struct compare yields false MISMATCH on identical data
        // (blame.md, batch-2 lesson).
        if (rc_c != rc_rs ||
            oc.working_set_kb != orr.working_set_kb ||
            oc.private_kb     != orr.private_kb ||
            oc.virt_kb        != orr.virt_kb ||
            oc.heap_kb        != orr.heap_kb ||
            oc.vma_walked     != orr.vma_walked ||
            oc.flags          != orr.flags) {
            mism++;
        }
    }

    // NULL-argument contract, both sides.
    proc_mem_out_t tmp;
    if (proc_mem_account_c(0, &tmp) != -1 || proc_mem_account_rs(0, &tmp) != -1) mism++;
    vecs++;

    kprintf("[RUST-DIFF] proc_mem: %d vecs mism=%d %s (LIVE=%s)\n",
            vecs, mism, mism ? "MISMATCH" : "MATCH",
#ifdef RUST_PROC_MEM
            "rust"
#else
            "c"
#endif
    );
    kprintf("[RUST-DIFF] proc_mem coverage: kernel=%d brk_wrap=%d no_brk=%d "
            "null_head=%d bad_vma=%d cycle=%d saturate=%d\n",
            cov_kernel, cov_brkwrap, cov_nobrk, cov_nullhead,
            cov_badvma, cov_cycle, cov_sat);

    // [RUST-SEC] the cyclic-list confinement, witnessed rather than argued.
    // A list that points at itself is walked by BOTH implementations here, but
    // the pre-existing unbounded C walkers (gui/taskmanager.c, mm/demand.c)
    // would spin forever in Ring 0 on this exact input.
    {
        static vma_t loop2[2];
        loop2[0].start = 0x20000000ull; loop2[0].end = 0x20001000ull;
        loop2[0].flags = 0; loop2[0].prot = 0; loop2[0].file = 0;
        loop2[0].file_offset = 0; loop2[0].file_size = 0; loop2[0].ref_count = 1;
        loop2[0].prev = 0; loop2[0].next = &loop2[1];
        loop2[1] = loop2[0];
        loop2[1].next = &loop2[0];          // 2-node cycle
        proc_mem_in_t in;
        in.cr3 = 0x100000ull; in.user_stack_size = 0x200000ull;
        in.brk = 0; in.brk_start = 0; in.resident_pages = 0; in.total_mapped = 0;
        in.vma_head = &loop2[0]; in.vma_count = 2; in.has_mm = 1;
        in.shares_vm = 0; in.privilege = 3; in._pad = 0;
        proc_mem_out_t o;
        int rc = proc_mem_account_rs(&in, &o);
        kprintf("[RUST-SEC] proc_mem cyclic vma_list: rc=%d walked=%u trunc=%u "
                "(bounded, returned; unbounded C walkers would hang)\n",
                rc, o.vma_walked, (o.flags & PROC_MEM_F_TRUNC) ? 1u : 0u);
    }
}
