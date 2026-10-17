// rustkern/heapframe.rs - #deadpml4: what the kernel heap's page mappings COST,
// and how far the heap can safely grow once they stop being paid for.
//
// THE FACT THIS MODULE IS BUILT ON, measured by #heapwin and re-measured here per
// page. mm/heap.c's heap_expand_once() called pmm_alloc_page() once per heap page
// and mapped the frame with vmm_map_page(). vmm_map_page() walks `kernel_pml4`
// (mm/vmm.c:14), a static .bss array that vmm_init() zeroes and NEVER loads into
// CR3: vmm_init() keeps the firmware's PML4 forever (current_pml4_phys =
// g_kernel_cr3 = uefi_cr3). So those writes land in a hierarchy the CPU does not
// walk, the heap's arena is the UEFI IDENTITY mapping of its own virtual
// addresses, and the window reservation already withholds exactly those physical
// pages from the PMM. Every frame the expansion took was allocated and then never
// addressed through any virtual address at all.
//
// THE SENTINEL IN #heapwin PROVED THAT FOR ONE PAGE. The 128 MB number rested on
// "the mechanism is identical for the other 32767", which is a correct argument
// and still not a measurement. heapframe_note_page_rs() below is called once per
// mapped heap page with BOTH translations and the result of a per-page sentinel,
// so the count is a count.
//
// AND THE RECLAIM FORCES A CEILING, which is the part that is easy to miss.
// Nothing but an empty PMM stopped the heap growing past 128 MB: heap_expand()
// falls back to the smallest expansion that would serve the caller, so it creeps.
// On a 576 MB VM the firmware's write-protected 2 MB blocks sit at 0x1e800000 and
// 0x1fc00000, i.e. 232 MB and 252 MB into the window, and HEAP_MAX_SIZE is
// 256 MB. Give the heap its 128 MB of frames back and it can now afford to reach
// them, and the first write to one is a #PF err=0x3 in Ring 0 (#procspawn
// measured those pages as not writable). So the ceiling is not a nicety added
// alongside the reclaim, it is a precondition of it.
//
// The ceiling asks the two owning accessors, never the address:
// vmm_identity_write_refused() (the #procspawn range list, pages Ring 0 provably
// cannot write) and the firmware memory type (pages that are not RAM the kernel
// may write at all). The lowest page failing either is the ceiling.
//
// RUST per the standing directive: new kernel logic with no performance argument.
// Every entry point is a handful of comparisons and relaxed atomic adds. It takes
// no lock, allocates nothing, touches no float and cannot block, so it is safe
// from the reservation loop, which runs before the scheduler exists.

use core::sync::atomic::{AtomicU64, Ordering};

// ---------------------------------------------------------------------------
// The window, published by mm/heap.c so the constants have ONE definition.
// ---------------------------------------------------------------------------
static WIN_BASE: AtomicU64 = AtomicU64::new(0);
static WIN_END: AtomicU64 = AtomicU64::new(0);

// ---------------------------------------------------------------------------
// The safe ceiling. Two independent disqualifications, counted and located
// separately, because they have different severities and a reader needs to know
// which one set the ceiling.
// ---------------------------------------------------------------------------
static N_RO: AtomicU64 = AtomicU64::new(0);        // write-refused from Ring 0
static N_NOTRAM: AtomicU64 = AtomicU64::new(0);    // not RAM the kernel may write
static FIRST_RO: AtomicU64 = AtomicU64::new(0);
static FIRST_NOTRAM: AtomicU64 = AtomicU64::new(0);
static N_SCANNED: AtomicU64 = AtomicU64::new(0);

// THE FLOOR. The smallest arena the clamp is allowed to leave. 0 = no floor.
//
// A ceiling BELOW this is not a ceiling this change may enforce. On a machine
// whose firmware puts an unwritable or non-RAM page low in the heap's window, the
// clamp would cut the arena to a few megabytes, and the heap's own comment says it
// "needs 64MB+ for NetHack ELF". That trades a hazard this change did not create
// for a boot failure it did. So the clamp SWITCHES OFF and says so, leaving
// exactly today's behaviour plus a diagnostic, and that machine gets a fix of its
// own rather than a side effect of this one.
static FLOOR: AtomicU64 = AtomicU64::new(0);

// ---------------------------------------------------------------------------
// The per-page cost verification.
// ---------------------------------------------------------------------------
static V_PAGES: AtomicU64 = AtomicU64::new(0);
static V_IDENT_OK: AtomicU64 = AtomicU64::new(0);
static V_IDENT_BAD: AtomicU64 = AtomicU64::new(0);
static V_FIRST_IDENT_BAD: AtomicU64 = AtomicU64::new(0);
static V_DEAD_OUT: AtomicU64 = AtomicU64::new(0);  // dead frame outside the window
static V_DEAD_IN: AtomicU64 = AtomicU64::new(0);   // dead frame inside the window
static V_DEAD_NONE: AtomicU64 = AtomicU64::new(0); // no dead mapping at all
static V_SENT_DIFF: AtomicU64 = AtomicU64::new(0);
static V_SENT_SAME: AtomicU64 = AtomicU64::new(0);
static V_SENT_SKIP: AtomicU64 = AtomicU64::new(0);
static V_FIRST_SENT_SAME: AtomicU64 = AtomicU64::new(0);

const PAGE: u64 = 4096;

/// Publish the heap's virtual window. mm/heap.c owns HEAP_VIRT_BASE and
/// HEAP_MAX_SIZE, so they are passed in rather than duplicated here.
#[no_mangle]
pub extern "C" fn heapframe_set_window_rs(base: u64, max_size: u64) {
    WIN_BASE.store(base, Ordering::Relaxed);
    WIN_END.store(base.wrapping_add(max_size), Ordering::Relaxed);
}

/// Publish the smallest arena the clamp may leave. mm/heap.c owns the constant
/// (HEAP_INITIAL_SIZE), so it is passed in rather than duplicated here.
#[no_mangle]
pub extern "C" fn heapframe_set_floor_rs(min_arena: u64) {
    FLOOR.store(min_arena, Ordering::Relaxed);
}

/// 1 when the measured ceiling is below the floor, so the clamp is OFF. The
/// caller must say so loudly: it means this machine's heap window contains a page
/// the heap should not be writing and the arena reaches it anyway, exactly as it
/// did before #deadpml4.
#[no_mangle]
pub extern "C" fn heapframe_floor_defeated_rs() -> i32 {
    let base = WIN_BASE.load(Ordering::Relaxed);
    let floor = FLOOR.load(Ordering::Relaxed);
    if floor == 0 {
        return 0;
    }
    let c = raw_ceiling();
    if c == 0 {
        return 0;
    }
    if c < base.wrapping_add(floor) { 1 } else { 0 }
}

/// The ceiling as MEASURED, before the floor is applied. The reporting path wants
/// this (a reader needs to see the page that was found even when the clamp is off);
/// the clamp path wants heapframe_ceiling_rs(), which honours the floor.
fn raw_ceiling() -> u64 {
    let end = WIN_END.load(Ordering::Relaxed);
    if end == 0 {
        return 0;
    }
    let mut ceil = end;
    let ro = FIRST_RO.load(Ordering::Relaxed);
    let nr = FIRST_NOTRAM.load(Ordering::Relaxed);
    if ro != 0 && ro < ceil {
        ceil = ro;
    }
    if nr != 0 && nr < ceil {
        ceil = nr;
    }
    ceil
}

/// The ceiling as MEASURED, ignoring the floor. For reporting only.
#[no_mangle]
pub extern "C" fn heapframe_raw_ceiling_rs() -> u64 {
    raw_ceiling()
}

/// Forget everything. Used by the self-test on its way in and out, so the live
/// report is never contaminated by synthetic pages.
#[no_mangle]
pub extern "C" fn heapframe_reset_rs() {
    // FLOOR is deliberately NOT in this list: it is published policy from
    // mm/heap.c, not measurement, and clearing it would silently switch the floor
    // off for the live heap the first time the self-test ran.
    for a in [&N_RO, &N_NOTRAM, &FIRST_RO, &FIRST_NOTRAM, &N_SCANNED,
              &V_PAGES, &V_IDENT_OK, &V_IDENT_BAD, &V_FIRST_IDENT_BAD,
              &V_DEAD_OUT, &V_DEAD_IN, &V_DEAD_NONE,
              &V_SENT_DIFF, &V_SENT_SAME, &V_SENT_SKIP, &V_FIRST_SENT_SAME] {
        a.store(0, Ordering::Relaxed);
    }
}

/// Record one page of the heap's virtual window.
///
/// `ram` is 1 when the firmware types this page as memory the kernel may write
/// (the same test pmm_init() uses to decide what to free). `write_refused` is 1
/// when #procspawn measured Ring 0 as unable to write it. Both come from the
/// owning subsystem's accessor; neither is inferred from the address.
///
/// Pages may be offered in any order. `FIRST_*` track the LOWEST, because the
/// heap grows upwards and the lowest disqualified page is the one it reaches.
#[no_mangle]
pub extern "C" fn heapframe_note_window_page_rs(virt: u64, ram: i32, write_refused: i32) {
    N_SCANNED.fetch_add(1, Ordering::Relaxed);
    if write_refused != 0 {
        N_RO.fetch_add(1, Ordering::Relaxed);
        let cur = FIRST_RO.load(Ordering::Relaxed);
        if cur == 0 || virt < cur {
            FIRST_RO.store(virt, Ordering::Relaxed);
        }
    }
    if ram == 0 {
        N_NOTRAM.fetch_add(1, Ordering::Relaxed);
        let cur = FIRST_NOTRAM.load(Ordering::Relaxed);
        if cur == 0 || virt < cur {
            FIRST_NOTRAM.store(virt, Ordering::Relaxed);
        }
    }
}

/// THE POLICY. The highest address the heap's arena may reach, exclusive.
///
/// It is the lowest window page that is either not writable from Ring 0 or not
/// RAM the kernel may write, and the window's end when there is no such page.
/// Returns 0 if the window was never published, which a caller must read as
/// "unknown, do not clamp", not as "zero bytes".
#[no_mangle]
pub extern "C" fn heapframe_ceiling_rs() -> u64 {
    let end = WIN_END.load(Ordering::Relaxed);
    if heapframe_floor_defeated_rs() != 0 {
        // The floor wins: behave as if nothing were in the way, which is exactly
        // the pre-#deadpml4 behaviour. heapframe_floor_defeated_rs() is what makes
        // this visible rather than silent.
        return end;
    }
    raw_ceiling()
}

/// WHICH disqualification set the ceiling: 0 = nothing (the window's end),
/// 1 = a write-refused page, 2 = a page that is not writable RAM.
#[no_mangle]
pub extern "C" fn heapframe_ceiling_cause_rs() -> i32 {
    let end = WIN_END.load(Ordering::Relaxed);
    if heapframe_floor_defeated_rs() != 0 {
        return 3;   // measured, but below the floor, so NOT enforced
    }
    let c = heapframe_ceiling_rs();
    if c == 0 || c == end {
        return 0;
    }
    if c == FIRST_RO.load(Ordering::Relaxed) {
        1
    } else {
        2
    }
}

/// Read the window scan back. Any pointer may be null.
#[no_mangle]
pub extern "C" fn heapframe_scan_counts_rs(scanned: *mut u64, ro: *mut u64,
                                           notram: *mut u64, first_ro: *mut u64,
                                           first_notram: *mut u64) {
    // SAFETY: every write is null-checked and each destination is one scalar the
    // C caller owns (locals in mm/heap.c). Nothing here reads through them, so no
    // aliasing claim is needed.
    unsafe {
        if !scanned.is_null() { *scanned = N_SCANNED.load(Ordering::Relaxed); }
        if !ro.is_null() { *ro = N_RO.load(Ordering::Relaxed); }
        if !notram.is_null() { *notram = N_NOTRAM.load(Ordering::Relaxed); }
        if !first_ro.is_null() { *first_ro = FIRST_RO.load(Ordering::Relaxed); }
        if !first_notram.is_null() {
            *first_notram = FIRST_NOTRAM.load(Ordering::Relaxed);
        }
    }
}

/// Record the two translations of one MAPPED heap page, plus the result of a
/// per-page sentinel.
///
/// `live_phys` is what the hierarchy in CR3 says, `dead_phys` what the
/// `kernel_pml4` that vmm_map_page() writes into says (0 = not mapped there).
/// `sentinel`: 1 = a value written through the heap virtual address WAS visible
/// in the frame `dead_phys` names (so that frame IS the heap's storage),
/// 0 = it was not (the mapping is inert), -1 = not tested.
///
/// THE COST IS V_DEAD_OUT: a frame the dead hierarchy claims, outside the
/// window, for a page whose live translation is identity. That frame is marked
/// allocated in the PMM and no virtual address reaches it.
#[no_mangle]
pub extern "C" fn heapframe_note_page_rs(virt: u64, live_phys: u64, dead_phys: u64,
                                         sentinel: i32) {
    let base = WIN_BASE.load(Ordering::Relaxed);
    let end = WIN_END.load(Ordering::Relaxed);
    V_PAGES.fetch_add(1, Ordering::Relaxed);

    if live_phys == virt {
        V_IDENT_OK.fetch_add(1, Ordering::Relaxed);
    } else {
        V_IDENT_BAD.fetch_add(1, Ordering::Relaxed);
        let cur = V_FIRST_IDENT_BAD.load(Ordering::Relaxed);
        if cur == 0 || virt < cur {
            V_FIRST_IDENT_BAD.store(virt, Ordering::Relaxed);
        }
    }

    if dead_phys == 0 {
        V_DEAD_NONE.fetch_add(1, Ordering::Relaxed);
    } else if dead_phys >= base && dead_phys < end {
        V_DEAD_IN.fetch_add(1, Ordering::Relaxed);
    } else {
        V_DEAD_OUT.fetch_add(1, Ordering::Relaxed);
    }

    if sentinel < 0 {
        V_SENT_SKIP.fetch_add(1, Ordering::Relaxed);
    } else if sentinel == 0 {
        V_SENT_DIFF.fetch_add(1, Ordering::Relaxed);
    } else {
        V_SENT_SAME.fetch_add(1, Ordering::Relaxed);
        let cur = V_FIRST_SENT_SAME.load(Ordering::Relaxed);
        if cur == 0 || virt < cur {
            V_FIRST_SENT_SAME.store(virt, Ordering::Relaxed);
        }
    }
}

/// Read the per-page verification back. Any pointer may be null.
#[no_mangle]
pub extern "C" fn heapframe_page_counts_rs(pages: *mut u64, ident_ok: *mut u64,
                                           ident_bad: *mut u64,
                                           first_ident_bad: *mut u64,
                                           dead_out: *mut u64, dead_in: *mut u64,
                                           dead_none: *mut u64,
                                           sent_diff: *mut u64, sent_same: *mut u64,
                                           sent_skip: *mut u64,
                                           first_sent_same: *mut u64) {
    // SAFETY: as heapframe_scan_counts_rs above. Null-checked single-scalar
    // out-parameters owned by the C caller.
    unsafe {
        if !pages.is_null() { *pages = V_PAGES.load(Ordering::Relaxed); }
        if !ident_ok.is_null() { *ident_ok = V_IDENT_OK.load(Ordering::Relaxed); }
        if !ident_bad.is_null() { *ident_bad = V_IDENT_BAD.load(Ordering::Relaxed); }
        if !first_ident_bad.is_null() {
            *first_ident_bad = V_FIRST_IDENT_BAD.load(Ordering::Relaxed);
        }
        if !dead_out.is_null() { *dead_out = V_DEAD_OUT.load(Ordering::Relaxed); }
        if !dead_in.is_null() { *dead_in = V_DEAD_IN.load(Ordering::Relaxed); }
        if !dead_none.is_null() { *dead_none = V_DEAD_NONE.load(Ordering::Relaxed); }
        if !sent_diff.is_null() { *sent_diff = V_SENT_DIFF.load(Ordering::Relaxed); }
        if !sent_same.is_null() { *sent_same = V_SENT_SAME.load(Ordering::Relaxed); }
        if !sent_skip.is_null() { *sent_skip = V_SENT_SKIP.load(Ordering::Relaxed); }
        if !first_sent_same.is_null() {
            *first_sent_same = V_FIRST_SENT_SAME.load(Ordering::Relaxed);
        }
    }
}

/// THE VERDICT, as one number: frames allocated into the dead hierarchy that no
/// virtual address reaches. 0 after the reclaim.
#[no_mangle]
pub extern "C" fn heapframe_inert_frames_rs() -> u64 {
    V_DEAD_OUT.load(Ordering::Relaxed)
}

/// Clamp an expansion to the ceiling. Returns the number of bytes that may
/// actually be adopted at `heap_end`, page-aligned down, or 0 when the heap has
/// reached the ceiling and must not grow.
///
/// Kept here rather than in mm/heap.c so the clamp and the ceiling that defines
/// it cannot drift apart, and so the edges are covered by the self-test below
/// instead of only by a boot.
#[no_mangle]
pub extern "C" fn heapframe_clamp_rs(heap_end: u64, want: u64) -> u64 {
    let ceil = heapframe_ceiling_rs();
    if ceil == 0 {
        // The window was never published: refuse to invent a limit, and do not
        // silently allow an unbounded expansion either. Caller treats 0 as
        // "cannot expand", which fails safe.
        return 0;
    }
    if heap_end >= ceil {
        return 0;
    }
    let room = ceil - heap_end;
    let allowed = if want < room { want } else { room };
    allowed & !(PAGE - 1)
}

/// Self-test. PROVES the ceiling policy picks the LOWEST disqualified page
/// whichever reason produced it and whatever order the pages arrive in, that the
/// clamp is exact at both edges, and that the cost tally separates an inert frame
/// from a frame inside the window from no mapping at all.
///
/// It runs on synthetic data and resets on the way in and the way out, so the
/// live report cannot be contaminated. Same contract as heapwin_selftest_rs.
#[no_mangle]
pub extern "C" fn heapframe_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut n: u32 = 0;
    let mut ok = true;
    let mut chk = |cond: bool| {
        n += 1;
        if !cond {
            ok = false;
        }
    };

    // Save the live window so the test can publish its own and put it back.
    let save_base = WIN_BASE.load(Ordering::Relaxed);
    let save_end = WIN_END.load(Ordering::Relaxed);
    let save_floor = FLOOR.load(Ordering::Relaxed);
    heapframe_reset_rs();

    // The real geometry: 0x10000000 + 256 MB.
    heapframe_set_window_rs(0x1000_0000, 256 * 1024 * 1024);

    // An all-clean window: the ceiling is the window's end and nothing clamps.
    chk(heapframe_ceiling_rs() == 0x2000_0000);
    chk(heapframe_ceiling_cause_rs() == 0);
    chk(heapframe_clamp_rs(0x1000_0000, 128 * 1024 * 1024) == 128 * 1024 * 1024);

    // The MEASURED 576 MB shape: two write-protected 2 MB blocks, offered in
    // descending order so "lowest wins" cannot pass by accident of arrival.
    let mut p = 0x1fdf_f000u64;
    while p >= 0x1fc0_0000 {
        heapframe_note_window_page_rs(p, 1, 1);
        p -= PAGE;
    }
    p = 0x1e9f_f000;
    while p >= 0x1e80_0000 {
        heapframe_note_window_page_rs(p, 1, 1);
        p -= PAGE;
    }
    let mut scanned = 0u64;
    let mut ro = 0u64;
    let mut notram = 0u64;
    let mut fro = 0u64;
    let mut fnr = 0u64;
    heapframe_scan_counts_rs(&mut scanned, &mut ro, &mut notram, &mut fro, &mut fnr);
    chk(scanned == 1024);
    chk(ro == 1024);
    chk(notram == 0);
    chk(fro == 0x1e80_0000); // the lowest, not the first offered
    chk(fnr == 0);
    chk(heapframe_ceiling_rs() == 0x1e80_0000);
    chk(heapframe_ceiling_cause_rs() == 1);

    // The clamp at that ceiling. 0x1e800000 - 0x10000000 = 232 MB of room.
    let mb = 1024u64 * 1024;
    chk(heapframe_clamp_rs(0x1000_0000, 256 * mb) == 232 * mb);   // clamped
    chk(heapframe_clamp_rs(0x1000_0000, 128 * mb) == 128 * mb);   // untouched
    chk(heapframe_clamp_rs(0x1e80_0000, PAGE) == 0);              // AT the ceiling
    chk(heapframe_clamp_rs(0x1e80_1000, PAGE) == 0);              // past it
    chk(heapframe_clamp_rs(0x1e7f_f000, PAGE) == PAGE);           // the last page
    chk(heapframe_clamp_rs(0x1e7f_f000, 64 * mb) == PAGE);        // and only it
    // A request that is not a whole number of pages is rounded DOWN, never up.
    chk(heapframe_clamp_rs(0x1000_0000, PAGE + 1) == PAGE);
    chk(heapframe_clamp_rs(0x1000_0000, PAGE - 1) == 0);

    // A page that is not RAM sets the ceiling too, and the LOWER of the two wins
    // with the cause reported accordingly.
    heapframe_note_window_page_rs(0x1800_0000, 0, 0);
    chk(heapframe_ceiling_rs() == 0x1800_0000);
    chk(heapframe_ceiling_cause_rs() == 2);
    chk(heapframe_clamp_rs(0x1000_0000, 256 * mb) == 128 * mb);
    // ... and a HIGHER not-RAM page does not move a ceiling already set lower.
    heapframe_note_window_page_rs(0x1f00_0000, 0, 0);
    chk(heapframe_ceiling_rs() == 0x1800_0000);

    // The cost tally. One inert frame (outside the window), one frame that lands
    // inside the window, one page with no dead mapping at all.
    heapframe_reset_rs();
    heapframe_set_window_rs(0x1000_0000, 256 * 1024 * 1024);
    heapframe_note_page_rs(0x1000_0000, 0x1000_0000, 0x0084_7000, 0); // inert
    heapframe_note_page_rs(0x1000_1000, 0x1000_1000, 0x1800_0000, 1); // inside
    heapframe_note_page_rs(0x1000_2000, 0x1000_2000, 0, -1);          // reclaimed
    heapframe_note_page_rs(0x1000_3000, 0x2200_0000, 0, -1);          // NOT identity
    let mut pages = 0u64;
    let mut iok = 0u64;
    let mut ibad = 0u64;
    let mut fib = 0u64;
    let mut dout = 0u64;
    let mut din = 0u64;
    let mut dnone = 0u64;
    let mut sdiff = 0u64;
    let mut ssame = 0u64;
    let mut sskip = 0u64;
    let mut fss = 0u64;
    heapframe_page_counts_rs(&mut pages, &mut iok, &mut ibad, &mut fib,
                             &mut dout, &mut din, &mut dnone,
                             &mut sdiff, &mut ssame, &mut sskip, &mut fss);
    chk(pages == 4);
    chk(iok == 3);
    chk(ibad == 1);
    chk(fib == 0x1000_3000);
    chk(dout == 1);
    chk(din == 1);
    chk(dnone == 2);
    chk(sdiff == 1);
    chk(ssame == 1);
    chk(sskip == 2);
    chk(fss == 0x1000_1000);
    // The buckets PARTITION the pages, in both dimensions. A tally that double
    // counts would still look plausible on a real boot.
    chk(iok + ibad == pages);
    chk(dout + din + dnone == pages);
    chk(sdiff + ssame + sskip == pages);
    #[cfg(not(heapframe_test_fail))]
    chk(heapframe_inert_frames_rs() == 1);
    // `make HEAPFRAMETESTFAIL=1`: assert the WRONG inert-frame count, so the
    // [HEAPFRAME] boot line can be SEEN to say FAIL on an otherwise healthy
    // machine. A self-test nobody has watched go red is indistinguishable from
    // one that is not wired up (#514/#665).
    #[cfg(heapframe_test_fail)]
    chk(heapframe_inert_frames_rs() == 999);

    // THE FLOOR. A ceiling below the smallest arena the kernel boots with is
    // MEASURED and REPORTED but NOT enforced, because shrinking the arena to
    // 16 MB is a failure this change would have caused.
    heapframe_reset_rs();
    heapframe_set_window_rs(0x1000_0000, 256 * mb);
    heapframe_set_floor_rs(128 * mb);
    heapframe_note_window_page_rs(0x1100_0000, 1, 1);   // 16 MB in: far too low
    chk(heapframe_raw_ceiling_rs() == 0x1100_0000);     // still MEASURED
    chk(heapframe_floor_defeated_rs() == 1);
    chk(heapframe_ceiling_cause_rs() == 3);
    chk(heapframe_ceiling_rs() == 0x2000_0000);         // but NOT enforced
    chk(heapframe_clamp_rs(0x1000_0000, 128 * mb) == 128 * mb);
    // Exactly AT the floor is enough: 0x18000000 - 0x10000000 == 128 MB.
    heapframe_reset_rs();
    heapframe_set_window_rs(0x1000_0000, 256 * mb);
    heapframe_set_floor_rs(128 * mb);
    heapframe_note_window_page_rs(0x1800_0000, 1, 1);
    chk(heapframe_floor_defeated_rs() == 0);
    chk(heapframe_ceiling_rs() == 0x1800_0000);
    chk(heapframe_ceiling_cause_rs() == 1);
    // One page BELOW the floor is not.
    heapframe_reset_rs();
    heapframe_set_window_rs(0x1000_0000, 256 * mb);
    heapframe_set_floor_rs(128 * mb);
    heapframe_note_window_page_rs(0x17ff_f000, 1, 1);
    chk(heapframe_floor_defeated_rs() == 1);
    chk(heapframe_ceiling_rs() == 0x2000_0000);
    // With no floor published, nothing is defeated and the ceiling stands.
    heapframe_set_floor_rs(0);
    chk(heapframe_floor_defeated_rs() == 0);
    chk(heapframe_ceiling_rs() == 0x17ff_f000);
    heapframe_set_floor_rs(0);

    // A reset really does clear, including the ceiling (which would otherwise
    // keep clamping the live heap to a synthetic address).
    heapframe_reset_rs();
    chk(heapframe_inert_frames_rs() == 0);
    chk(heapframe_ceiling_rs() == 0x2000_0000);
    // With no window published at all the clamp refuses rather than guessing.
    heapframe_set_window_rs(0, 0);
    chk(heapframe_ceiling_rs() == 0);
    chk(heapframe_clamp_rs(0x1000_0000, 128 * mb) == 0);

    // Put the live window back; the caller publishes it again anyway, but a
    // self-test that leaves the policy pointing at nothing is a trap.
    WIN_BASE.store(save_base, Ordering::Relaxed);
    WIN_END.store(save_end, Ordering::Relaxed);
    FLOOR.store(save_floor, Ordering::Relaxed);

    if !out_checks.is_null() {
        // SAFETY: one scalar out-parameter owned by the C caller, null-checked.
        unsafe { *out_checks = n };
    }
    if ok { 0 } else { -1 }
}
