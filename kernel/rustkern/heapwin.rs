// rustkern/heapwin.rs - #heapwin: WHAT is already allocated inside the kernel
// heap's virtual window before heap_init() runs, and WHERE it sits.
//
// THE QUESTION THIS MODULE ANSWERS, AND WHY IT IS NOT THE ONE IT LOOKS LIKE.
//
// mm/heap.c reserves the heap's whole virtual window (0x10000000-0x20000000) in
// the PMM before it maps anything, and counts the pages that were ALREADY
// allocated when it got there. That counter is printed as "ALREADY ALLOCATED
// (must be 0)" and the warning beside it says such a page is "a live corruption
// this came too late to prevent". On a 576 MB VM it prints 1024.
//
// It is not a corruption, and nothing came too late. MEASURED, 576 MB VM, build
// from commit 63db8e4d:
//
//     [PT647] CR3=0x1fc01000 phys_limit=0xff000000: 73 table pages, ...
//     [PT647] reserved 73 newly (were FREE and would have been handed out), ...
//     [PTRO] 0x1e800000..0x1ea00000 (512 pages) NOT writable from Ring 0; 505 were FREE
//     [PTRO] 0x1fc00000..0x1fe00000 (512 pages) NOT writable from Ring 0; 446 were FREE
//     [PTRO] 2 range(s), 1024 pages unwritable, 951 were free, 951 newly reserved
//
// 73 + 951 = 1024, and both of those reservations happen inside vmm_init(),
// which is the ONLY thing that runs between pmm_init() and heap_init(). So
// every page heap_init() finds allocated was reserved on purpose, two function
// calls earlier, by a named fix: vmm_reserve_boot_page_tables() (#647, the
// adopted UEFI paging structures, reachable only through CR3) and
// vmm_reserve_unwritable_identity_pages() (#procspawn, the non-table pages
// sharing the firmware's write-protected 2 MB blocks).
//
// A counter that cannot tell "a correct earlier reservation owns this" from
// "somebody corrupted this" has to shout on both, and then it gets ignored on
// the one that matters. That is the SAME defect heapshadow.rs's
// heapshadow_note_reserve2_rs() was added to fix one layer up (firmware-reserved
// versus usable RAM); this is the next layer, and it is where the alarm's
// remaining 100% false-positive rate on a 576 MB VM lives.
//
// WHAT THIS MODULE DOES. It records, per page, WHICH named reservation owns it,
// asking the two existing accessors rather than inferring from the address:
// vmm_is_boot_page_table() (built from the CR3 walk, so it is the authoritative
// answer to "is this a live page table") and vmm_identity_write_refused() (the
// #procspawn range list). A page in neither is UNEXPLAINED, and THAT is the
// number worth an alarm.
//
// It also records the pages as a coalesced run list, so mm/heap.c can ask the
// WHERE question: do they fall inside the part of the window the heap has
// actually mapped, or in the inert upper half? See heapwin_overlap_rs().
//
// RUST per the standing directive: new kernel logic, no performance argument.
// Every entry point here is two comparisons and an atomic add: it takes no lock,
// allocates nothing and cannot block, so it is safe to call from the reservation
// loop, which runs before the scheduler exists.

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

// Provenance tallies. One per named reservation, plus the alarm bucket.
static N_TABLE: AtomicU64 = AtomicU64::new(0);
static N_RO_ONLY: AtomicU64 = AtomicU64::new(0);
static N_UNEXPLAINED: AtomicU64 = AtomicU64::new(0);
static FIRST_UNEXPLAINED: AtomicU64 = AtomicU64::new(0);

// The recorded pages, coalesced into runs. The real data on every machine
// measured so far is TWO runs (the firmware's two write-protected 2 MB blocks),
// so 64 is two orders of magnitude of headroom; an overflow is COUNTED and
// reported rather than silently truncating, which is the ptwalk.rs/ptro.rs rule:
// a truncated list means "not listed" stops meaning "not there".
const RUNS: usize = 64;
const PAGE: u64 = 4096;

#[allow(clippy::declare_interior_mutable_const)]
const ZERO: AtomicU64 = AtomicU64::new(0);
static RUN_LO: [AtomicU64; RUNS] = [ZERO; RUNS];
static RUN_HI: [AtomicU64; RUNS] = [ZERO; RUNS];
static RUN_N: AtomicU32 = AtomicU32::new(0);
static RUN_OVERFLOW: AtomicU32 = AtomicU32::new(0);

/// Forget everything recorded. Called once, by mm/heap.c, after the self-test
/// and before the reservation loop, so the self-test's synthetic notes can never
/// be mistaken for measurements of the real machine.
#[no_mangle]
pub extern "C" fn heapwin_reset_rs() {
    N_TABLE.store(0, Ordering::Relaxed);
    N_RO_ONLY.store(0, Ordering::Relaxed);
    N_UNEXPLAINED.store(0, Ordering::Relaxed);
    FIRST_UNEXPLAINED.store(0, Ordering::Relaxed);
    RUN_N.store(0, Ordering::Relaxed);
    RUN_OVERFLOW.store(0, Ordering::Relaxed);
}

/// Record one page that was ALREADY allocated when the heap's window reservation
/// reached it, together with what the two existing owners say about it.
///
/// `is_table`     = vmm_is_boot_page_table(phys), i.e. the CR3 walk found it.
/// `is_ro_block`  = vmm_identity_write_refused(phys), i.e. it is inside one of the
///                  firmware's write-protected blocks the #procspawn sweep withheld.
///
/// A page can be both (every live table measured so far is inside a protected
/// block); "table" wins, because it names the stronger fact. Neither means
/// UNEXPLAINED: no reservation in this kernel admits to owning it.
///
/// Pages are expected in ASCENDING address order, which is the order mm/heap.c's
/// loop walks the window. Out-of-order notes are still counted correctly and
/// simply start a new run, so the run list stays sound either way.
#[no_mangle]
pub extern "C" fn heapwin_note_rs(phys: u64, is_table: i32, is_ro_block: i32) {
    if is_table != 0 {
        N_TABLE.fetch_add(1, Ordering::Relaxed);
    } else if is_ro_block != 0 {
        N_RO_ONLY.fetch_add(1, Ordering::Relaxed);
    } else {
        if N_UNEXPLAINED.fetch_add(1, Ordering::Relaxed) == 0 {
            FIRST_UNEXPLAINED.store(phys, Ordering::Relaxed);
        }
    }

    let n = RUN_N.load(Ordering::Relaxed) as usize;
    if n > 0 && RUN_HI[n - 1].load(Ordering::Relaxed) == phys {
        RUN_HI[n - 1].store(phys.wrapping_add(PAGE), Ordering::Relaxed);
        return;
    }
    if n >= RUNS {
        RUN_OVERFLOW.fetch_add(1, Ordering::Relaxed);
        return;
    }
    RUN_LO[n].store(phys, Ordering::Relaxed);
    RUN_HI[n].store(phys.wrapping_add(PAGE), Ordering::Relaxed);
    RUN_N.store((n + 1) as u32, Ordering::Relaxed);
}

/// Read the provenance tallies back. Any pointer may be null.
#[no_mangle]
pub extern "C" fn heapwin_counts_rs(table: *mut u64, ro_only: *mut u64,
                                    unexplained: *mut u64,
                                    first_unexplained: *mut u64,
                                    runs: *mut u32, overflow: *mut u32) {
    // SAFETY: every write is guarded by a null check, and each destination is a
    // single scalar the C caller owns (locals in mm/heap.c). No aliasing claim is
    // needed because nothing here reads through these pointers.
    unsafe {
        if !table.is_null() { *table = N_TABLE.load(Ordering::Relaxed); }
        if !ro_only.is_null() { *ro_only = N_RO_ONLY.load(Ordering::Relaxed); }
        if !unexplained.is_null() { *unexplained = N_UNEXPLAINED.load(Ordering::Relaxed); }
        if !first_unexplained.is_null() {
            *first_unexplained = FIRST_UNEXPLAINED.load(Ordering::Relaxed);
        }
        if !runs.is_null() { *runs = RUN_N.load(Ordering::Relaxed); }
        if !overflow.is_null() { *overflow = RUN_OVERFLOW.load(Ordering::Relaxed); }
    }
}

/// The lowest recorded page, or 0 if none. This is the one the heap would reach
/// FIRST if it ever grew that far, so it is the number that sets the headroom.
#[no_mangle]
pub extern "C" fn heapwin_lowest_rs() -> u64 {
    let n = RUN_N.load(Ordering::Relaxed) as usize;
    let mut lowest = 0u64;
    for i in 0..n.min(RUNS) {
        let lo = RUN_LO[i].load(Ordering::Relaxed);
        if lowest == 0 || lo < lowest {
            lowest = lo;
        }
    }
    lowest
}

/// The highest recorded page (its first byte), or 0 if none.
#[no_mangle]
pub extern "C" fn heapwin_highest_rs() -> u64 {
    let n = RUN_N.load(Ordering::Relaxed) as usize;
    let mut highest = 0u64;
    for i in 0..n.min(RUNS) {
        let hi = RUN_HI[i].load(Ordering::Relaxed);
        if hi > PAGE && hi.wrapping_sub(PAGE) > highest {
            highest = hi.wrapping_sub(PAGE);
        }
    }
    highest
}

/// How many recorded pages fall inside the half-open range [lo, hi)?
///
/// THIS IS THE HAZARD QUESTION. The heap's arena occupies the virtual addresses
/// [HEAP_VIRT_BASE, heap_end), and under the UEFI identity map those ARE the
/// physical pages of the same number, so a recorded page inside that range is
/// one the heap is already using as its own storage: a live firmware paging
/// structure the heap would overwrite. A recorded page ABOVE heap_end is in the
/// part of the window the heap has not reached, which is inert.
///
/// Half-open on purpose, at both ends: heap_end is one past the last heap byte,
/// and a run ending exactly at `lo` is adjacent, not overlapping. Getting that
/// wrong in either direction is how a detector reports a hazard that is not
/// there, or misses one that is.
#[no_mangle]
pub extern "C" fn heapwin_overlap_rs(lo: u64, hi: u64) -> u64 {
    if hi <= lo {
        return 0;
    }
    let n = RUN_N.load(Ordering::Relaxed) as usize;
    let mut pages = 0u64;
    for i in 0..n.min(RUNS) {
        let a = RUN_LO[i].load(Ordering::Relaxed);
        let b = RUN_HI[i].load(Ordering::Relaxed);
        let s = if a > lo { a } else { lo };
        let e = if b < hi { b } else { hi };
        if e > s {
            pages += (e - s) / PAGE;
        }
    }
    pages
}

/// Self-test. PROVES the tally discriminates between the two named owners and
/// the alarm bucket, and PROVES the overlap test is half-open at both ends, on
/// the exact run shape measured on a 576 MB VM.
///
/// It runs BEFORE heapwin_reset_rs() in mm/heap.c, so it writes into the live
/// statics on purpose and the reset that follows clears them. That ordering is
/// the contract: a self-test that left synthetic notes behind would make the
/// provenance line a lie, and one that saved and restored would still have to be
/// trusted to restore correctly.
#[no_mangle]
pub extern "C" fn heapwin_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut n: u32 = 0;
    let mut ok = true;
    let mut chk = |cond: bool| {
        n += 1;
        if !cond {
            ok = false;
        }
    };

    heapwin_reset_rs();

    // Provenance: a live table, a protected non-table sibling, and a page no
    // reservation claims. The third is the only one that should alarm.
    heapwin_note_rs(0x1fc0_1000, 1, 1); // the MEASURED CR3 of a 576 MB VM
    heapwin_note_rs(0x1e80_0000, 0, 1); // a protected non-table sibling
    heapwin_note_rs(0x1800_0000, 0, 0); // unexplained
    let mut t = 0u64;
    let mut r = 0u64;
    let mut u = 0u64;
    let mut fu = 0u64;
    let mut runs = 0u32;
    let mut ovf = 0u32;
    heapwin_counts_rs(&mut t, &mut r, &mut u, &mut fu, &mut runs, &mut ovf);
    chk(t == 1);
    chk(r == 1);
    chk(u == 1);
    chk(fu == 0x1800_0000);
    chk(runs == 3); // three non-adjacent pages, noted out of order
    chk(ovf == 0);
    // A table page must NOT also be counted as a protected sibling: the buckets
    // partition the pages, so the three must sum to the total.
    chk(t + r + u == 3);

    // The run list and the overlap test, on the shape actually measured: two
    // 2 MB blocks, 512 pages each.
    heapwin_reset_rs();
    let mut p = 0x1e80_0000u64;
    while p < 0x1ea0_0000 {
        heapwin_note_rs(p, 0, 1);
        p += PAGE;
    }
    p = 0x1fc0_0000;
    while p < 0x1fe0_0000 {
        heapwin_note_rs(p, 0, 1);
        p += PAGE;
    }
    heapwin_counts_rs(&mut t, &mut r, &mut u, &mut fu, &mut runs, &mut ovf);
    chk(r == 1024); // the number the 576 MB VM reports
    chk(runs == 2); // coalesced into exactly the two firmware blocks
    chk(ovf == 0);
    chk(heapwin_lowest_rs() == 0x1e80_0000);
    chk(heapwin_highest_rs() == 0x1fdf_f000);

    // The MEASURED mapped heap: 0x10000000..0x18001000. Nothing overlaps.
    chk(heapwin_overlap_rs(0x1000_0000, 0x1800_1000) == 0);
    // The heap at its CONFIGURED MAXIMUM reaches every one of them.
    chk(heapwin_overlap_rs(0x1000_0000, 0x2000_0000) == 1024);
    // Half-open at the low end: a range ending EXACTLY at the first recorded
    // page is adjacent, not overlapping. (This is the edge heapshadow.rs's own
    // self-test got wrong and turned red on a real boot.)
    chk(heapwin_overlap_rs(0x1000_0000, 0x1e80_0000) == 0);
    // One byte further and it catches exactly one page.
    chk(heapwin_overlap_rs(0x1000_0000, 0x1e80_1000) == 1);
    // Half-open at the high end: starting AT the one-past-the-end of a run
    // catches nothing from that run.
    chk(heapwin_overlap_rs(0x1ea0_0000, 0x1fc0_0000) == 0);
    chk(heapwin_overlap_rs(0x1fc0_0000, 0x2000_0000) == 512);
    // A degenerate and an inverted range are not hazards.
    chk(heapwin_overlap_rs(0x1e80_0000, 0x1e80_0000) == 0);
    chk(heapwin_overlap_rs(0x2000_0000, 0x1000_0000) == 0);
    // A range that swallows both blocks whole.
    chk(heapwin_overlap_rs(0, u64::MAX) == 1024);

    // Overflow is DETECTED, not silent: 65 non-adjacent pages into a 64-run list.
    heapwin_reset_rs();
    for i in 0..(RUNS as u64 + 1) {
        heapwin_note_rs(0x3000_0000 + i * PAGE * 2, 0, 1); // every other page
    }
    heapwin_counts_rs(&mut t, &mut r, &mut u, &mut fu, &mut runs, &mut ovf);
    chk(runs == RUNS as u32);
    chk(ovf == 1);
    chk(r == RUNS as u64 + 1); // the TALLY is complete even when the run list is not

    heapwin_reset_rs();
    chk(heapwin_lowest_rs() == 0);
    chk(heapwin_highest_rs() == 0);
    chk(heapwin_overlap_rs(0, u64::MAX) == 0);

    if !out_checks.is_null() {
        // SAFETY: single scalar out-parameter owned by the C caller, null-checked.
        unsafe { *out_checks = n };
    }
    if ok { 0 } else { -1 }
}
