// rustkern/ptwalk.rs - #647: enumerate every TABLE page of a live x86-64
// 4-level paging hierarchy.
//
// WHY THIS EXISTS
// ---------------
// vmm_init() ADOPTS the firmware's page tables and runs on them forever
// ("this is the safest approach"). CR3 therefore points at memory the firmware
// allocated, and the bootloader maps the UEFI types that memory carries
// (EfiConventionalMemory / EfiBootServicesCode / EfiBootServicesData ->
// MEMORY_TYPE_USABLE, EfiLoaderCode / EfiLoaderData -> MEMORY_TYPE_BOOTLOADER)
// onto exactly the two classes pmm_init() FREES. Reclaiming those classes is
// correct in isolation and wrong here, because we never stopped using the
// tables. A page table has no linker symbol, so pmm_init()'s hardcoded
// re-reservations (kernel text, __data_start..__kernel_end, the bitmap) cannot
// possibly cover it: the tables are reachable only through CR3.
//
// This walk is the missing enumeration. It is pure pointer-chasing logic over
// identity-mapped memory (physical == virtual on this OS, UEFI identity map),
// which is why it is Rust: no privileged instruction is involved. The only C/asm
// left on the C side is the `mov %cr3` read itself (types.h read_cr3()) and the
// kprintf reporting, per the Rust-first policy.
//
// Bounds-safety notes, because this walks ATTACKER-IRRELEVANT but
// FIRMWARE-AUTHORED data that we did not create:
//   * every dereference is bounds-checked against `phys_limit` first, so a
//     garbage entry pointing past RAM cannot fault us;
//   * the recursion depth is fixed at 4 by the architecture, so a self-
//     referential entry (the recursive-mapping trick) cannot loop forever;
//   * duplicates are collapsed, because firmware routinely shares one table
//     page between several parent entries.

// PTE bits. Deliberately re-declared here rather than imported so this module
// has no dependency on the C header layout.
const PTE_PRESENT: u64 = 1 << 0;
const PTE_HUGE: u64 = 1 << 7;
const PTE_ADDR: u64 = 0x000F_FFFF_FFFF_F000;

/// Outcome of offering a table page to the collected set.
#[derive(PartialEq)]
enum Push {
    /// Newly recorded. The caller SHOULD walk its children.
    Added,
    /// Already recorded, so its subtree has already been walked. The caller MUST
    /// NOT walk it again: firmware may point several parent entries at one
    /// shared table, and re-walking turns the fixed 3-level nest into up to
    /// 512^3 iterations. A self-referential entry (the recursive-mapping trick)
    /// is the extreme case of the same thing.
    Dup,
    /// The array is full. The caller must stop and report an overflow: a
    /// truncated list means live tables we never saw and therefore never
    /// reserved, so "not in the list" stops meaning "not a page table".
    Full,
}

/// Append `v` to the collected set if it is not already there.
///
/// Linear scan: n is the number of TABLE pages in one hierarchy, which is a few
/// hundred to a few thousand, so O(n^2) here is a handful of milliseconds once
/// at boot. A sorted insert would be faster and harder to audit; this runs once.
fn push_unique(list: &mut [u64], n: &mut usize, v: u64) -> Push {
    for i in 0..*n {
        if list[i] == v {
            return Push::Dup;
        }
    }
    if *n < list.len() {
        list[*n] = v;
        *n += 1;
        return Push::Added;
    }
    Push::Full
}

/// Read the 512 entries of the table page at physical address `tbl`.
///
/// SAFETY: the caller has already checked `tbl` against `phys_limit` and
/// verified `tbl` is 4KB-aligned. Physical == virtual on this OS (UEFI identity
/// map, no PHYS_TO_VIRT), and the page is by construction part of the live
/// hierarchy the CPU itself is reading, so it is present RAM.
unsafe fn entries(tbl: u64) -> &'static [u64; 512] {
    &*(tbl as *const [u64; 512])
}

/// Collect the physical address of every table page reachable from `cr3`,
/// including the top-level PML4 page itself.
///
/// `out` / `max` is the caller's array. `phys_limit` is one past the highest
/// physical address it is safe to dereference (the top of the memory map).
///
/// Returns the number of DISTINCT table pages written to `out`. `*overflow` is
/// set non-zero if the array filled up (the result is then a prefix, not the
/// whole hierarchy, and the caller MUST NOT treat "not in the list" as "not a
/// table page"). `*skipped` counts child pointers rejected as out of range.
#[no_mangle]
pub extern "C" fn ptwalk_collect_rs(
    cr3: u64,
    out: *mut u64,
    max: u32,
    phys_limit: u64,
    overflow: *mut u32,
    skipped: *mut u32,
) -> u32 {
    if out.is_null() || max == 0 {
        return 0;
    }
    // SAFETY: caller guarantees `out` points to `max` writable u64s.
    let list: &mut [u64] = unsafe { core::slice::from_raw_parts_mut(out, max as usize) };
    let mut n: usize = 0;
    let mut ovf: u32 = 0;
    let mut skip: u32 = 0;

    let root = cr3 & PTE_ADDR;

    let ok = |p: u64| -> bool { p != 0 && p < phys_limit && (p & 0xFFF) == 0 };

    if !ok(root) {
        if !skipped.is_null() {
            unsafe { *skipped = 1 };
        }
        return 0;
    }

    if push_unique(list, &mut n, root) == Push::Full {
        ovf = 1;
    }

    // Level 4 (PML4) -> 3 (PDPT) -> 2 (PD) -> 1 (PT). A HUGE entry at level 3
    // (1GB page) or level 2 (2MB page) is a LEAF: its address field is the data
    // frame, not a table, and must not be followed or reserved. Following one
    // would reserve up to 512 unrelated data pages and, worse, would report
    // ordinary RAM as a page table.
    //
    // Written as three explicit nested loops rather than recursion so the depth
    // bound is structural and visible.
    let pml4 = unsafe { entries(root) };
    for i in 0..512 {
        let e3 = pml4[i];
        if e3 & PTE_PRESENT == 0 {
            continue;
        }
        let pdpt_pa = e3 & PTE_ADDR;
        if !ok(pdpt_pa) {
            skip += 1;
            continue;
        }
        match push_unique(list, &mut n, pdpt_pa) {
            Push::Added => {}
            Push::Dup => continue,          // subtree already walked
            Push::Full => { ovf = 1; continue; }
        }
        let pdpt = unsafe { entries(pdpt_pa) };
        for j in 0..512 {
            let e2 = pdpt[j];
            if e2 & PTE_PRESENT == 0 || e2 & PTE_HUGE != 0 {
                continue; // absent, or a 1GB leaf frame
            }
            let pd_pa = e2 & PTE_ADDR;
            if !ok(pd_pa) {
                skip += 1;
                continue;
            }
            match push_unique(list, &mut n, pd_pa) {
                Push::Added => {}
                Push::Dup => continue,      // subtree already walked
                Push::Full => { ovf = 1; continue; }
            }
            let pd = unsafe { entries(pd_pa) };
            for k in 0..512 {
                let e1 = pd[k];
                if e1 & PTE_PRESENT == 0 || e1 & PTE_HUGE != 0 {
                    continue; // absent, or a 2MB leaf frame
                }
                let pt_pa = e1 & PTE_ADDR;
                if !ok(pt_pa) {
                    skip += 1;
                    continue;
                }
                if push_unique(list, &mut n, pt_pa) == Push::Full {
                    ovf = 1;
                }
                // Level 1 entries are all leaves; nothing below to walk.
            }
        }
    }

    if !overflow.is_null() {
        unsafe { *overflow = ovf };
    }
    if !skipped.is_null() {
        unsafe { *skipped = skip };
    }
    n as u32
}

/// Is `phys` (any address within the page) one of the `n` collected table pages?
/// Returns 1 / 0. Used by the self-test and by any later auditor.
#[no_mangle]
pub extern "C" fn ptwalk_contains_rs(list: *const u64, n: u32, phys: u64) -> i32 {
    if list.is_null() || n == 0 {
        return 0;
    }
    let page = phys & !0xFFFu64;
    // SAFETY: caller guarantees `list` points to `n` readable u64s.
    let l: &[u64] = unsafe { core::slice::from_raw_parts(list, n as usize) };
    for i in 0..l.len() {
        if l[i] == page {
            return 1;
        }
    }
    0
}
