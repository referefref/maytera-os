// rustkern/heapshadow.rs - #aicappersist: the kernel heap SHADOWS a range of
// physical addresses, and the rest of the kernel does not know it.
//
// THE INVARIANT THE WHOLE KERNEL RELIES ON. UEFI identity-maps memory at boot,
// so "physical address == virtual address" in Ring 0, and the kernel takes that
// literally: mm/vmm.c does `uint64_t *pd = (uint64_t *)pmm_alloc_page();` and
// writes a page table through the number the allocator returned. There is no
// PHYS_TO_VIRT macro in this tree because there has never needed to be one.
//
// THE PLACE IT STOPS BEING TRUE. mm/heap.c uses the virtual addresses in
// [0x10000000, 0x10000000+HEAP_MAX_SIZE) as its arena. So for any physical page P
// inside that window, `(uint64_t *)P` is a pointer INTO THE KERNEL HEAP. Writing
// a page table there does not initialise the page table; it overwrites a live
// heap object, and CR3 is left pointing at a page nobody initialised.
//
// CORRECTED 2026-10-01 (#heapwin). This paragraph used to say the reason was that
// "vmm_map_page(virt, phys) installs a PTE for that virtual address, which
// REPLACES the UEFI identity entry". IT DOES NOT. vmm_map_page() walks
// `kernel_pml4`, a static .bss array vmm_init() zeroes and NEVER loads into CR3,
// because vmm_init() keeps the firmware's PML4 forever. The heap's mapping is
// inert for translation; the identity mapping is intact and the heap's arena IS
// the physical memory of the same address. MEASURED on a 576 MB and a 4096 MB VM,
// two independent ways (a walk of the live CR3, and a sentinel written through the
// heap and looked for in the frame vmm_map_page claimed): see
// heapwin_report_where() in mm/heap.c and rustkern/heapwin.rs.
//
// The predicate below, and the window reservation that uses it, are correct under
// either mechanism; only the EXPLANATION was wrong. It mattered enough to fix
// because the wrong one makes the heap look like it shadows other people's pages
// (harmless if nobody dereferences them) when in fact it would WRITE to them.
//
// MEASURED, golden 2519 and 2520, two independent boots. The PMM starts handing
// out pages above 256 MB once earlier allocations have walked past it, and the
// late-boot services are where that happens:
//
//     [PROC] Created user process 'netinfo'   CR3=0x10001000   <- inside the window
//     [PROC] Created user process 'haservice' CR3=0x10232000   <- inside the window
//
// and from the next instant:
//
//     [FSENOENT] site=parentguard verdict=PARENT-NOT-A-DIRECTORY path=/UIPROFIL.YML
//                parent=/ mounted=1 root_ino=2 parent_ino=2/2 mode=0 inode_rc=0 uid=0
//
// The root inode reads back with SUCCESS and i_mode == 0, because the ext2 block
// cache's kmalloc'd buffers are heap objects and one of them has just been
// overwritten with a freshly cleared page table. e2fsck on the same disk is
// clean: the zeros were never on the medium. After that every ext2 lookup fails,
// every create under an existing directory is refused ENOENT, and the AI
// capability grant store write is refused rc=-2, which is the symptom this
// investigation started from.
//
// WHAT THIS MODULE DOES. It owns the predicate "is this physical address one the
// identity assumption can no longer be trusted for", so that:
//   1. mm/heap.c can RESERVE that window in the PMM before it maps anything, so
//      the allocator never hands out a page the kernel cannot address; and
//   2. any site that dereferences a PMM page as identity can ASSERT it, so a
//      reintroduction is caught in one line instead of becoming another week of
//      mystery ENOENTs.
// The cost of (1) is stated plainly where it is paid: HEAP_MAX_SIZE of physical
// memory becomes unallocatable (256 MB of 2047 MB here). That is the price of
// an architecture with no PHYS_TO_VIRT; the cheaper long-term fix is to make the
// heap's mapping IDENTITY, or to move its window above the PMM's range, and both
// are bigger changes than a live data-corruption bug should have to wait for.
//
// RUST per the standing directive: new kernel logic, no performance argument. The
// predicate is two comparisons and is safe to call from any context: it takes no
// lock, allocates nothing and cannot block.

use core::sync::atomic::{AtomicU64, Ordering};

// The shadowed window, in BYTES, published by mm/heap.c at heap_init(). Zero
// until then, which makes the predicate answer "not shadowed" for the whole of
// early boot. That is CORRECT, not a fallback: before the heap maps anything the
// identity mapping really is intact everywhere.
static LO: AtomicU64 = AtomicU64::new(0);
static HI: AtomicU64 = AtomicU64::new(0);

// Accounting for the reservation, so "the window is reserved" is a number
// somebody can read rather than a claim in a comment.
static RES_NEW: AtomicU64 = AtomicU64::new(0);
static RES_ALREADY: AtomicU64 = AtomicU64::new(0);
static RES_MOOT: AtomicU64 = AtomicU64::new(0);
// #heapdbl: pages that were already used because the FIRMWARE reserved them.
// Counted apart from RES_ALREADY, which is the real alarm.
static RES_FIRMWARE: AtomicU64 = AtomicU64::new(0);
static VIOLATIONS: AtomicU64 = AtomicU64::new(0);
// #heapdbl: multi-page runs that overlapped the window. NOT violations; see
// heapshadow_note_span_rs().
static SPANS: AtomicU64 = AtomicU64::new(0);

/// Publish the heap's virtual window. `base` and `max_size` come from
/// mm/heap.c's own HEAP_VIRT_BASE / HEAP_MAX_SIZE, so there is ONE definition of
/// where the heap lives and this module cannot drift from it.
#[no_mangle]
pub extern "C" fn heapshadow_set_window_rs(base: u64, max_size: u64) {
    LO.store(base, Ordering::Relaxed);
    HI.store(base.wrapping_add(max_size), Ordering::Relaxed);
}

/// Is `phys` inside the window whose identity mapping the heap has replaced?
/// 1 = yes, the identity assumption is WRONG for this address.
#[no_mangle]
pub extern "C" fn heapshadow_is_shadowed_rs(phys: u64) -> i32 {
    let lo = LO.load(Ordering::Relaxed);
    let hi = HI.load(Ordering::Relaxed);
    if hi == 0 || lo >= hi {
        return 0; // window not published yet: identity is intact
    }
    if phys >= lo && phys < hi {
        1
    } else {
        0
    }
}

/// Does the range `[base, base+len)` overlap the shadowed window at all?
///
/// #heapdbl: this exists because `fs/blockdev.c` was asking the same question with
/// its OWN hard-coded copy of the bounds (`BLK_HEAP_WIN_LO`/`BLK_HEAP_WIN_HI` =
/// 0x10000000/0x20000000) while `mm/heap.c` owns the real `HEAP_VIRT_BASE` /
/// `HEAP_MAX_SIZE` and this module already held the predicate. Two definitions of
/// one window, in the one place that depends on the window being right: raise
/// HEAP_MAX_SIZE and the block layer would keep handing itself heap memory while
/// believing it had checked. There is now ONE definition.
///
/// A multi-page run can start below the window and end inside it, so this is a
/// true overlap test, not two endpoint tests.
#[no_mangle]
pub extern "C" fn heapshadow_range_overlaps_rs(base: u64, len: u64) -> i32 {
    if len == 0 {
        return 0;
    }
    let lo = LO.load(Ordering::Relaxed);
    let hi = HI.load(Ordering::Relaxed);
    if hi == 0 || lo >= hi {
        return 0; // window not published yet: identity is intact
    }
    let end = base.saturating_add(len);
    if base < hi && end > lo {
        1
    } else {
        0
    }
}

/// Count one multi-page run that OVERLAPPED the window and was handed to a caller.
/// Deliberately NOT counted as a violation: the one caller that receives these
/// (blk_alloc_chunks) rejects and parks them, so a run is only a corruption if the
/// caller dereferences it. Returns 1 the first time only.
#[no_mangle]
pub extern "C" fn heapshadow_note_span_rs() -> i32 {
    if SPANS.fetch_add(1, Ordering::Relaxed) == 0 { 1 } else { 0 }
}

/// How many such runs this boot.
#[no_mangle]
pub extern "C" fn heapshadow_span_count_rs() -> u64 {
    SPANS.load(Ordering::Relaxed)
}

/// Record the outcome of one pmm_reserve_page() call made by the reservation
/// loop. `rc` is the PMM's own return: 1 newly reserved, 0 already used, -1 out
/// of the allocator's range.
#[no_mangle]
pub extern "C" fn heapshadow_note_reserve_rs(rc: i32) {
    heapshadow_note_reserve2_rs(rc, 1)
}

/// #heapdbl: as above, but `usable` says whether the firmware's memory map types
/// this page as usable RAM (1) or not (0).
///
/// WHY THIS SPLIT EXISTS. "already used" was one bucket and the boot line called it
/// "(must be 0)", warning that any such page is "a live corruption this came too
/// late to prevent". On a 576 MB VM that printed 2299 and was WRONG about every one
/// of them: the PMM's managed range there ends at 0x20000000, the top of the heap's
/// own window, so the firmware's non-usable regions fall inside it, pmm_init()
/// marks them used exactly as it should, and nothing is corrupted because such a
/// page was never going to be handed out. A 4 GB VM reports 0 and the alarm never
/// fired, which is how it survived.
///
/// A detector that cannot tell "somebody already owns this" from "the firmware says
/// this is not RAM" has to shout on both, and then it gets ignored on the one that
/// matters.
#[no_mangle]
pub extern "C" fn heapshadow_note_reserve2_rs(rc: i32, usable: i32) {
    match rc {
        1 => { RES_NEW.fetch_add(1, Ordering::Relaxed); }
        0 => {
            if usable != 0 {
                RES_ALREADY.fetch_add(1, Ordering::Relaxed);
            } else {
                RES_FIRMWARE.fetch_add(1, Ordering::Relaxed);
            }
        }
        _ => { RES_MOOT.fetch_add(1, Ordering::Relaxed); }
    }
}

/// How many window pages were already used because the firmware reserved them.
#[no_mangle]
pub extern "C" fn heapshadow_firmware_count_rs() -> u64 {
    RES_FIRMWARE.load(Ordering::Relaxed)
}

/// Read the accounting back. `already` is the one that matters: a page inside
/// the window that was ALREADY allocated when the reservation ran is a page the
/// kernel cannot address and somebody else already owns, so it is a live
/// corruption that the reservation came too late to prevent. It must be ZERO.
#[no_mangle]
pub extern "C" fn heapshadow_counts_rs(new: *mut u64, already: *mut u64,
                                       moot: *mut u64, viol: *mut u64) {
    unsafe {
        if !new.is_null()     { *new     = RES_NEW.load(Ordering::Relaxed); }
        if !already.is_null() { *already = RES_ALREADY.load(Ordering::Relaxed); }
        if !moot.is_null()    { *moot    = RES_MOOT.load(Ordering::Relaxed); }
        if !viol.is_null()    { *viol    = VIOLATIONS.load(Ordering::Relaxed); }
    }
}

/// Count one site that dereferenced a shadowed physical address as if it were a
/// virtual one. Returns 1 the FIRST time only, so the caller can log once and
/// loudly instead of flooding; later occurrences still increment the counter.
#[no_mangle]
pub extern "C" fn heapshadow_violation_rs() -> i32 {
    if VIOLATIONS.fetch_add(1, Ordering::Relaxed) == 0 { 1 } else { 0 }
}

/// Self-test. PROVES the predicate discriminates, against a window published
/// into the live statics and then restored, and proves it fails CLOSED (answers
/// "not shadowed") before any window exists, because answering "shadowed" for
/// everything would make the early-boot assertion fire on every page.
#[no_mangle]
pub extern "C" fn heapshadow_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut n: u32 = 0;
    let mut ok = true;
    let mut chk = |cond: bool| {
        n += 1;
        if !cond {
            ok = false;
        }
    };

    let save_lo = LO.load(Ordering::Relaxed);
    let save_hi = HI.load(Ordering::Relaxed);

    // No window: nothing is shadowed. The identity map really is intact then.
    LO.store(0, Ordering::Relaxed);
    HI.store(0, Ordering::Relaxed);
    chk(heapshadow_is_shadowed_rs(0x1000_0000) == 0);
    chk(heapshadow_is_shadowed_rs(0) == 0);

    // The real shape: base 256 MB, max 256 MB, so [0x10000000, 0x20000000).
    heapshadow_set_window_rs(0x1000_0000, 0x1000_0000);
    chk(heapshadow_is_shadowed_rs(0x0FFF_F000) == 0); // the page below: fine
    chk(heapshadow_is_shadowed_rs(0x1000_0000) == 1); // first page: shadowed
    chk(heapshadow_is_shadowed_rs(0x1023_2000) == 1); // the MEASURED haservice PML4
    chk(heapshadow_is_shadowed_rs(0x1000_1000) == 1); // the MEASURED netinfo PML4
    chk(heapshadow_is_shadowed_rs(0x1FFF_F000) == 1); // last page: shadowed
    chk(heapshadow_is_shadowed_rs(0x2000_0000) == 0); // one past the end: fine
    // The RANGE predicate, on the shape blk_alloc_chunks actually asks about: a
    // 2 MB run, below / straddling / inside / above the window. The straddle cases
    // are the reason this is not two endpoint tests.
    chk(heapshadow_range_overlaps_rs(0x0FE0_0000, 0x0020_0000) == 0); // ends EXACTLY
                                                     // at lo: ADJACENT, not an
                                                     // overlap. This assertion
                                                     // said 1 and turned the
                                                     // self-test red on a real
                                                     // boot; the predicate was
                                                     // right and the test was
                                                     // wrong, which is why a
                                                     // half-open interval gets an
                                                     // explicit case at each edge.
    chk(heapshadow_range_overlaps_rs(0x0FF0_0000, 0x0020_0000) == 1); // straddles lo
    chk(heapshadow_range_overlaps_rs(0x1000_0000, 0x0020_0000) == 1); // at lo
    chk(heapshadow_range_overlaps_rs(0x1FF0_0000, 0x0020_0000) == 1); // straddles hi
    chk(heapshadow_range_overlaps_rs(0x2000_0000, 0x0020_0000) == 0); // starts AT hi
    chk(heapshadow_range_overlaps_rs(0x4000_0000, 0x0020_0000) == 0); // well above
    chk(heapshadow_range_overlaps_rs(0x1000_0000, 0) == 0);           // empty range
    // A run so large it contains the whole window still overlaps it.
    chk(heapshadow_range_overlaps_rs(0x0800_0000, 0x4000_0000) == 1);
    chk(heapshadow_is_shadowed_rs(0x0D5A_6000) == 0); // an EARLIER boot's PML4,
                                                     // which worked, and must
                                                     // keep reading as fine or
                                                     // the predicate proves
                                                     // nothing about the bad one

    // The reserve-outcome split. Counters are global and additive, so test by
    // DELTA and restore nothing: a self-test that reset them would erase the real
    // reservation's numbers, which are printed on the very next line.
    {
        let a0 = RES_ALREADY.load(Ordering::Relaxed);
        let f0 = RES_FIRMWARE.load(Ordering::Relaxed);
        heapshadow_note_reserve2_rs(0, 1);   // already used, and it IS usable RAM
        heapshadow_note_reserve2_rs(0, 0);   // already used because firmware said so
        chk(RES_ALREADY.load(Ordering::Relaxed) == a0 + 1);
        chk(RES_FIRMWARE.load(Ordering::Relaxed) == f0 + 1);
        // Undo, so the printed reservation accounting stays true.
        RES_ALREADY.store(a0, Ordering::Relaxed);
        RES_FIRMWARE.store(f0, Ordering::Relaxed);
    }

    LO.store(save_lo, Ordering::Relaxed);
    HI.store(save_hi, Ordering::Relaxed);

    if !out_checks.is_null() {
        unsafe { *out_checks = n };
    }
    if ok { 0 } else { -1 }
}
