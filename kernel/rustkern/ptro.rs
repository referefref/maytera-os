// rustkern/ptro.rs - #procspawn: find every physical page the PMM may hand out
// whose KERNEL IDENTITY MAPPING is not writable.
//
// WHY THIS EXISTS
// ---------------
// vmm_init() adopts the firmware's page tables and runs on them forever, so the
// kernel's "identity map" is not a map the kernel wrote: it carries the
// FIRMWARE's protection attributes. OVMF write-protects some of its own pages,
// and the bootloader maps EfiBootServicesCode/Data onto MEMORY_TYPE_USABLE,
// which pmm_init() frees. The result is free physical pages whose identity
// mapping is PRESENT but READ-ONLY.
//
// The first thing the kernel does with a freshly allocated page is zero it
// through that identity address (mm/vmm.c vmm_alloc_user_pages(), and the two
// page-table memsets in vmm_create_user_space()). On such a page that memset is
// a supervisor write to a present, read-only page: #PF with error code 0x3
// (P=1, W=1, U=0), CR2 equal to the physical address, RIP inside memset_fast,
// and a dead kernel.
//
// #647 removed one instance of this class by enumerating the live page TABLES
// from CR3 and reserving them. It could only see table pages: the firmware
// write-protects more than its tables. This module answers the general
// question instead, by asking the page tables what they actually permit.
//
// MEASURED on a 4 GB QEMU q35 / OVMF guest (golden 2480), QEMU `info mem`:
//     0x7aa00000-0x7ac00000  -r-   (2 MB, read-only, supervisor)
//     0x7bc00000-0x7be00000  -r-   (2 MB, read-only, supervisor; contains the
//                                   74 live table pages #647 already reserves)
// Both are inside the PMM's managed window 0x100000..0x80000000, and both were
// FREE. pmm_alloc_page()'s cursor (`alloc_hint`, mm/pmm.c) sweeps physical
// memory monotonically upward and wraps, so reaching them is a matter of how
// much has been allocated since boot, not of any race.
//
// This is pure pointer-chasing over identity-mapped memory, which is why it is
// Rust per the Rust-first policy: no privileged instruction is involved. The C
// side keeps only the CR3 read, the PMM bitmap calls and the reporting.

// PTE bits. Re-declared locally, like ptwalk.rs, so this module has no
// dependency on the C header layout.
const PTE_PRESENT: u64 = 1 << 0;
const PTE_WRITABLE: u64 = 1 << 1;
const PTE_HUGE: u64 = 1 << 7;
const PTE_ADDR: u64 = 0x000F_FFFF_FFFF_F000;

const SZ_4K: u64 = 0x1000;
const SZ_2M: u64 = 0x20_0000;
const SZ_1G: u64 = 0x4000_0000;
const SZ_512G: u64 = 0x80_0000_0000;

/// Read the 512 entries of the table page at physical address `tbl`.
///
/// SAFETY: the caller has checked `tbl` against `phys_limit` and that it is
/// 4KB-aligned. Physical == virtual on this OS (UEFI identity map, no
/// PHYS_TO_VIRT), and the page is part of the hierarchy the CPU is itself
/// walking, so it is present RAM.
unsafe fn entries(tbl: u64) -> &'static [u64; 512] {
    &*(tbl as *const [u64; 512])
}

#[inline]
fn align_down(v: u64, a: u64) -> u64 {
    v & !(a - 1)
}

/// What the hardware would permit for a SUPERVISOR WRITE to identity address
/// `va`, and how far that answer holds.
///
/// Returns `(writable, next)`, where `next` is the first address past the leaf
/// (or past the absent subtree) that produced the answer, so a caller can skip
/// a whole 2MB or 1GB leaf in one step instead of walking per page.
///
/// `writable` is the AND of the R/W bit down every level, which is what the CPU
/// computes when CR0.WP is set (it is: #647's own report notes the firmware's
/// write-protected tables faulted rather than being silently overwritten).
/// A not-present entry answers `false`, because a write there faults too; for
/// this module's purpose "the kernel cannot write it" is the single question.
fn probe(cr3: u64, va: u64, phys_limit: u64) -> (bool, u64) {
    let ok = |p: u64| -> bool { p != 0 && p < phys_limit && (p & 0xFFF) == 0 };

    let root = cr3 & PTE_ADDR;
    if !ok(root) {
        return (false, align_down(va, SZ_512G) + SZ_512G);
    }

    // Level 4
    let pml4 = unsafe { entries(root) };
    let e4 = pml4[((va >> 39) & 0x1FF) as usize];
    if e4 & PTE_PRESENT == 0 {
        return (false, align_down(va, SZ_512G) + SZ_512G);
    }
    let mut w = e4 & PTE_WRITABLE != 0;

    let pdpt_pa = e4 & PTE_ADDR;
    if !ok(pdpt_pa) {
        return (false, align_down(va, SZ_512G) + SZ_512G);
    }

    // Level 3
    let pdpt = unsafe { entries(pdpt_pa) };
    let e3 = pdpt[((va >> 30) & 0x1FF) as usize];
    if e3 & PTE_PRESENT == 0 {
        return (false, align_down(va, SZ_1G) + SZ_1G);
    }
    w = w && (e3 & PTE_WRITABLE != 0);
    if e3 & PTE_HUGE != 0 {
        return (w, align_down(va, SZ_1G) + SZ_1G); // 1GB leaf
    }

    let pd_pa = e3 & PTE_ADDR;
    if !ok(pd_pa) {
        return (false, align_down(va, SZ_1G) + SZ_1G);
    }

    // Level 2
    let pd = unsafe { entries(pd_pa) };
    let e2 = pd[((va >> 21) & 0x1FF) as usize];
    if e2 & PTE_PRESENT == 0 {
        return (false, align_down(va, SZ_2M) + SZ_2M);
    }
    w = w && (e2 & PTE_WRITABLE != 0);
    if e2 & PTE_HUGE != 0 {
        return (w, align_down(va, SZ_2M) + SZ_2M); // 2MB leaf
    }

    let pt_pa = e2 & PTE_ADDR;
    if !ok(pt_pa) {
        return (false, align_down(va, SZ_2M) + SZ_2M);
    }

    // Level 1
    let pt = unsafe { entries(pt_pa) };
    let e1 = pt[((va >> 12) & 0x1FF) as usize];
    if e1 & PTE_PRESENT == 0 {
        return (false, align_down(va, SZ_4K) + SZ_4K);
    }
    w = w && (e1 & PTE_WRITABLE != 0);
    (w, align_down(va, SZ_4K) + SZ_4K)
}

/// Collect the maximal runs of identity addresses in `[lo, hi)` that a Ring-0
/// write CANNOT reach (absent, or present and read-only).
///
/// `out` receives `start, end` pairs and must hold `2 * max` u64s. Returns the
/// number of RANGES written. `*overflow` is set non-zero if `max` was reached,
/// in which case the result is a prefix and the caller must NOT treat "not
/// listed" as "writable".
#[no_mangle]
pub extern "C" fn ptro_collect_rs(
    cr3: u64,
    lo: u64,
    hi: u64,
    phys_limit: u64,
    out: *mut u64,
    max: u32,
    overflow: *mut u32,
) -> u32 {
    if !overflow.is_null() {
        unsafe { *overflow = 0 };
    }
    if out.is_null() || max == 0 || hi <= lo {
        return 0;
    }
    // SAFETY: caller guarantees `out` points to `2 * max` writable u64s.
    let slots: &mut [u64] = unsafe { core::slice::from_raw_parts_mut(out, (max as usize) * 2) };

    // `phys_limit` bounds every table dereference, exactly as in ptwalk.rs:
    // `hi` is the top of the PMM WINDOW (2GB), which is not the top of RAM, and
    // a table may legitimately live above it, so the caller passes
    // pmm_phys_limit() rather than `hi`.
    let mut n: usize = 0;
    let mut va = align_down(lo, SZ_4K);
    let mut run_start: u64 = 0;
    let mut in_run = false;

    while va < hi {
        let (w, next) = probe(cr3, va, phys_limit);
        let step_end = if next > hi { hi } else { next };
        if !w {
            if !in_run {
                in_run = true;
                run_start = va;
            }
        } else if in_run {
            in_run = false;
            if n < max as usize {
                slots[n * 2] = run_start;
                slots[n * 2 + 1] = va;
                n += 1;
            } else {
                if !overflow.is_null() {
                    unsafe { *overflow = 1 };
                }
                return n as u32;
            }
        }
        va = step_end;
    }

    if in_run {
        if n < max as usize {
            slots[n * 2] = run_start;
            slots[n * 2 + 1] = hi;
            n += 1;
        } else if !overflow.is_null() {
            unsafe { *overflow = 1 };
        }
    }

    n as u32
}
