// kstackreg.rs - the LIVE-BLOCK REGISTRY of mm/kstack.c, arbitrated so two
// concurrent allocations cannot be handed the same slot.
//
// THE DEFECT THIS REMOVES (named and left unfixed at #heapinvptr, blame.md
// 2026-10-01, "NOT FIXED, named so a green boot is not mistaken for a clean
// one").
//
// kstack_alloc() used to do this, with no lock of any kind:
//
//     for (slot = 0; slot < KSTACK_MAX; slot++) if (g_ks[slot].usable == 0) break;
//     ... pmm_alloc_pages(), poison the band, stamp the magic ...
//     g_ks[slot].usable = block + KSTACK_BAND;      // <-- minutes later, in effect
//
// The scan and the write are separated by a PMM allocation and a 4 KiB poison
// loop. Two allocators that scan inside that window both see the same row as
// free and both write it. The second write wins; the first registration is GONE
// while its granule is live and in use. Nothing complains at the time.
//
// The bill arrives at the matching kstack_free(): the registry lookup misses, so
// the pointer takes the "not one of ours" path. Before #heapinvptr that path
// called kfree(), which read the poisoned guard band 32 bytes below the pointer
// as a heap block header and reported
//     [HEAP] ERROR: kfree() called on invalid pointer 0x... (magic=0xbaadb4dd)
// naming the HEAP for a defect in the kernel-stack allocator. #heapinvptr made
// that path REFUSE and say so instead, which means a lost registration now leaks
// a granule loudly rather than corrupting the heap quietly. Good, but the lost
// registration is still a bug and the leak is still a leak.
//
// WHY NOW RATHER THAN LATER. The Big Kernel Lock currently serialises almost
// every kstack_alloc() call site, which is exactly why this has not been seen:
// the window is real but almost nothing runs inside it. BKL narrowing is a
// standing requirement (SMP is a requirement, not a setting), and every stage of
// it widens this window. A latent race whose only protection is a lock being
// deliberately removed is not latent for much longer.
//
// WHAT CHANGES: RESERVE-THEN-COMMIT. claim() finds a free row and immediately
// marks it RESERVED, under the lock, in the SAME critical section as the scan.
// The slow work (PMM, poison, magic) then runs with no lock held, and commit()
// fills the row in afterwards. A second claim() can never see a reserved row as
// free, so the scan/write window is closed by construction rather than by being
// short.
//
// LANGUAGE: Rust, per the 2026-07-16 rule. mm/kstack.c's own header states why
// THAT file is C (it talks to the PMM in physical frames and feeds the one-AND
// check that runs inside isr_handler() on every interrupt), and none of that
// applies to deciding which row of an array is free. This module touches no
// hardware, allocates nothing, and is reachable only from the two allocator
// entry points, never from the interrupt hot path.
//
// STORAGE OWNERSHIP: C owns the array, Rust owns the decision, exactly as
// conring.rs/procreap.rs/proc_mem.rs already do. The layout is locked by a
// `_Static_assert(sizeof(kstack_ent_t) == 24)` in mm/kstack.c, so a field added
// on the C side cannot silently reinterpret these rows.
//
// LOCKING CONTRACT, and it is the CALLER'S. Every function here except the
// self-test must be called with mm/kstack.c's `g_ks_lock` held (the SHARED
// sync/spinlock.h irqsave lock, not a private copy). IRQSAVE is not optional:
// kstack_free() is reached from process teardown paths that run under cli(), so
// a plain spinlock here would be the exact non-irqsave hole that sync/noblock.h
// documents as the known gap its assert cannot see. Nothing in this file waits,
// blocks, allocates, prints or takes a second lock, so `g_ks_lock` is a LEAF
// lock and cannot take part in a deadlock cycle; that is the property that keeps
// it safe to hold with interrupts masked.
#![allow(dead_code)]

/// One live-block row. Mirrors `kstack_ent_t` in mm/kstack.c; the C side carries
/// the `_Static_assert` that locks the size.
#[repr(C)]
pub struct KstackEnt {
    pub usable: u64,
    pub block: u64,
    pub pages: u64,
}

/// A row that is claimed but not yet filled in. It must be a value no real
/// `usable` can take: `kstack_alloc()` returns `block + KSTACK_BAND` where
/// `block` is non-zero and 64 KiB-aligned, so every real `usable` is at least
/// 4096 and 4096-aligned. 1 is unreachable, and unlike a separate `reserved`
/// flag it cannot get out of step with the field the scan reads.
pub const KSTACKREG_RESERVED: u64 = 1;

/// `commit()` found a row already holding this `usable`. That is the lost-
/// registration symptom itself, so it is reported rather than merely avoided.
pub const KSTACKREG_DUP: u32 = 1 << 0;
/// `commit()` was handed a slot that was not the one `claim()` reserved.
pub const KSTACKREG_NOT_RESERVED: u32 = 1 << 1;

/// True for a row that is neither free nor merely reserved, i.e. one that names
/// a real block.
#[inline]
fn row_is_live(usable: u64) -> bool {
    usable != 0 && usable != KSTACKREG_RESERVED
}

/// A pointer that could never have come from `kstack_alloc()`. Lookups reject
/// these up front so a caller that passes 0 or the reservation sentinel cannot
/// be handed somebody else's row.
#[inline]
fn lookup_key_is_sane(usable: u64) -> bool {
    row_is_live(usable)
}

/// Reserve a free row. Returns its index, or -1 when every row is taken.
///
/// THE SCAN AND THE RESERVATION ARE ONE OPERATION. That is the entire fix: the
/// old code's scan returned an index and the write happened later, so two
/// callers could both be told the same index was free.
///
/// # Safety
/// `tab` must point to `max` contiguous, initialised `KstackEnt` rows, and the
/// caller must hold `g_ks_lock`.
#[no_mangle]
pub unsafe extern "C" fn kstackreg_claim_rs(tab: *mut KstackEnt, max: u32) -> i32 {
    if tab.is_null() || max == 0 {
        return -1;
    }
    for i in 0..max {
        let e = &mut *tab.add(i as usize);
        if e.usable == 0 {
            // `make KSTACKREGTEST=1`: the PRE-FIX behaviour, a scan that returns
            // an index without reserving it. The boot self-test's first case
            // then sees claim() hand out the same slot twice and must report a
            // non-zero mask. A self-test nobody has watched go red is
            // indistinguishable from one that is not wired up (#514/#665), and
            // for a race the red arm is the only way to show the test is
            // sensitive to the thing that was actually wrong. NEVER for a
            // golden.
            if cfg!(kstackreg_test_fail) {
                return i as i32;
            }
            e.usable = KSTACKREG_RESERVED;
            e.block = 0;
            e.pages = 0;
            return i as i32;
        }
    }
    -1
}

/// Fill in a row that `claim()` reserved. Returns 0, or a BITMASK of anomalies
/// so a failure names which one it was.
///
/// The duplicate scan is the lost-registration DETECTOR, not an optimisation: if
/// a second path ever manages to register the same block twice, this is where it
/// becomes a report instead of a silent future orphan free.
///
/// # Safety
/// As `kstackreg_claim_rs`. `slot` must be a value a previous `claim()` returned.
#[no_mangle]
pub unsafe extern "C" fn kstackreg_commit_rs(tab: *mut KstackEnt, max: u32, slot: i32,
                                             usable: u64, block: u64, pages: u64) -> u32 {
    if tab.is_null() || slot < 0 || (slot as u32) >= max {
        return KSTACKREG_NOT_RESERVED;
    }
    let mut flags = 0u32;
    for i in 0..max {
        if i as i32 == slot {
            continue;
        }
        if (*tab.add(i as usize)).usable == usable {
            flags |= KSTACKREG_DUP;
            break;
        }
    }
    let e = &mut *tab.add(slot as usize);
    if e.usable != KSTACKREG_RESERVED {
        flags |= KSTACKREG_NOT_RESERVED;
    }
    // Written even when an anomaly was flagged: the block IS live and its frames
    // ARE in use, so refusing to record it would guarantee the orphan free this
    // module exists to prevent. The caller reports; it does not drop the row.
    e.block = block;
    e.pages = pages;
    e.usable = usable;
    flags
}

/// Release a reservation whose slow path failed (no frames, bad alignment), so
/// the row becomes free again rather than being pinned as RESERVED for the life
/// of the boot.
///
/// A reservation leaked on a failure path would be invisible: the census would
/// still read "live=N", and the table would simply run out one row earlier every
/// time an allocation failed.
///
/// # Safety
/// As `kstackreg_claim_rs`.
#[no_mangle]
pub unsafe extern "C" fn kstackreg_abort_rs(tab: *mut KstackEnt, max: u32, slot: i32) {
    if tab.is_null() || slot < 0 || (slot as u32) >= max {
        return;
    }
    let e = &mut *tab.add(slot as usize);
    // Only a reservation is dropped. If the row has somehow become live, the
    // frames it names belong to a real stack and clearing it here would make
    // that stack's own free an orphan.
    if e.usable == KSTACKREG_RESERVED {
        e.usable = 0;
        e.block = 0;
        e.pages = 0;
    }
}

/// Find and REMOVE the row for `usable`, handing back its block and page count.
/// Returns 1 when a row was removed, 0 when there was none.
///
/// Find-and-remove is one operation for the same reason claim-and-reserve is:
/// two concurrent frees of the same pointer must not both be told "yes, it is
/// yours, go and hand the frames back to the PMM".
///
/// # Safety
/// As `kstackreg_claim_rs`. `block_out`/`pages_out` must be writable or null.
#[no_mangle]
pub unsafe extern "C" fn kstackreg_take_rs(tab: *mut KstackEnt, max: u32, usable: u64,
                                           block_out: *mut u64, pages_out: *mut u64) -> i32 {
    if tab.is_null() || !lookup_key_is_sane(usable) {
        return 0;
    }
    for i in 0..max {
        let e = &mut *tab.add(i as usize);
        if e.usable != usable {
            continue;
        }
        if !block_out.is_null() {
            *block_out = e.block;
        }
        if !pages_out.is_null() {
            *pages_out = e.pages;
        }
        e.usable = 0;
        e.block = 0;
        e.pages = 0;
        return 1;
    }
    0
}

/// Is `usable` a registered guarded stack? Read-only; the row stays.
///
/// # Safety
/// As `kstackreg_claim_rs`.
#[no_mangle]
pub unsafe extern "C" fn kstackreg_find_rs(tab: *const KstackEnt, max: u32, usable: u64) -> i32 {
    if tab.is_null() || !lookup_key_is_sane(usable) {
        return -1;
    }
    for i in 0..max {
        if (*tab.add(i as usize)).usable == usable {
            return i as i32;
        }
    }
    -1
}

/// Self-test. 0 when every case behaved, otherwise a BITMASK naming the cases
/// that did not. Runs from `kstack_selftest()` on EVERY boot including the
/// golden, which is the strongest form available (heapshadow_selftest_rs and
/// kstackid_selftest_rs set that bar); it touches only its own stack array and
/// allocates nothing.
///
/// Case 0 is THE DEFECT: two claims with no commit in between must get DIFFERENT
/// rows. The pre-fix scan returns the same row for both, which is the lost
/// registration, so this one case is red on the old design and green on the new.
/// `make KSTACKREGTEST=1` compiles that old design back in and must turn it red
/// here, on demand, from a kernel that is otherwise this one.
#[no_mangle]
pub extern "C" fn kstackreg_selftest_rs() -> u32 {
    const N: u32 = 4;
    let mut t = [
        KstackEnt { usable: 0, block: 0, pages: 0 },
        KstackEnt { usable: 0, block: 0, pages: 0 },
        KstackEnt { usable: 0, block: 0, pages: 0 },
        KstackEnt { usable: 0, block: 0, pages: 0 },
    ];
    let p = t.as_mut_ptr();
    let mut bad = 0u32;

    // Plausible values: 64 KiB-aligned blocks, usable = block + 4096, exactly
    // the shape kstack_alloc() hands out.
    const B1: u64 = 0x0fdb_0000;
    const B2: u64 = 0x0fdc_0000;
    const U1: u64 = B1 + 4096;
    const U2: u64 = B2 + 4096;

    unsafe {
        // ---- CASE 0: THE RACE. Claim twice before committing either. ----
        let a = kstackreg_claim_rs(p, N);
        let b = kstackreg_claim_rs(p, N);
        if a < 0 || b < 0 || a == b {
            bad |= 1 << 0;
        }

        // ---- CASE 1: commit + find round trip, on both reservations. ----
        if kstackreg_commit_rs(p, N, a, U1, B1, 16) != 0 {
            bad |= 1 << 1;
        }
        if kstackreg_commit_rs(p, N, b, U2, B2, 16) != 0 {
            bad |= 1 << 1;
        }
        if kstackreg_find_rs(p, N, U1) != a || kstackreg_find_rs(p, N, U2) != b {
            bad |= 1 << 1;
        }

        // ---- CASE 2: take() removes exactly one row and reports its block. ----
        let mut blk = 0u64;
        let mut pg = 0u64;
        if kstackreg_take_rs(p, N, U1, &mut blk, &mut pg) != 1 || blk != B1 || pg != 16 {
            bad |= 1 << 2;
        }
        if kstackreg_find_rs(p, N, U1) >= 0 {
            bad |= 1 << 2;
        }
        // The OTHER row must be untouched: a free that removed its neighbour
        // would be the lost registration wearing a different hat.
        if kstackreg_find_rs(p, N, U2) != b {
            bad |= 1 << 2;
        }

        // ---- CASE 3: a SECOND take() of the same pointer must find nothing. --
        // This is the orphan route: kstack_free() must be told "no row", so that
        // kstackid.rs gets the chance to call it a double free instead of the
        // registry silently handing the same frames back twice.
        blk = 0xdead;
        pg = 0xdead;
        if kstackreg_take_rs(p, N, U1, &mut blk, &mut pg) != 0 {
            bad |= 1 << 3;
        }
        if blk != 0xdead || pg != 0xdead {
            bad |= 1 << 3;   // a miss must not write the out-parameters
        }

        // ---- CASE 4: abort() frees a reservation; a live row survives it. ----
        let c = kstackreg_claim_rs(p, N);
        if c < 0 {
            bad |= 1 << 4;
        }
        kstackreg_abort_rs(p, N, c);
        if kstackreg_claim_rs(p, N) != c {
            // The row abort() released must be claimable again, and being the
            // lowest free row it is the one the next claim takes.
            bad |= 1 << 4;
        }
        kstackreg_abort_rs(p, N, c);
        // abort() on a LIVE row must do nothing at all.
        kstackreg_abort_rs(p, N, b);
        if kstackreg_find_rs(p, N, U2) != b {
            bad |= 1 << 4;
        }

        // ---- CASE 5: the duplicate detector fires. ----
        let d = kstackreg_claim_rs(p, N);
        if d < 0 || (kstackreg_commit_rs(p, N, d, U2, B2, 16) & KSTACKREG_DUP) == 0 {
            bad |= 1 << 5;
        }
        // Clean up both rows holding U2 before the table-full case below.
        // Two SEPARATE sinks: `&mut x, &mut x` in one call is two simultaneous
        // mutable borrows of the same local and rustc rejects it outright.
        let mut sink_b = 0u64;
        let mut sink_p = 0u64;
        kstackreg_take_rs(p, N, U2, &mut sink_b, &mut sink_p);
        kstackreg_take_rs(p, N, U2, &mut sink_b, &mut sink_p);

        // ---- CASE 6: a FULL table refuses, it does not collide. ----
        // Reserve every row, then ask for one more. The pre-fix scan would hand
        // back row 0 again (every row still reads usable == 0 to it), which is
        // the same lost registration as case 0 and is why both go red under
        // KSTACKREGTEST=1.
        let mut seen = [-1i32; 4];
        for k in 0..N as usize {
            seen[k] = kstackreg_claim_rs(p, N);
        }
        if kstackreg_claim_rs(p, N) != -1 {
            bad |= 1 << 6;
        }
        for k in 0..N as usize {
            for j in 0..k {
                if seen[k] == seen[j] || seen[k] < 0 {
                    bad |= 1 << 6;
                }
            }
        }
        for k in 0..N as usize {
            kstackreg_abort_rs(p, N, seen[k]);
        }

        // ---- CASE 7: NEGATIVE CONTROLS. A registry that says yes to anything
        // protects nothing. ----
        // Never-registered pointer.
        if kstackreg_find_rs(p, N, 0x1234_5000) >= 0 {
            bad |= 1 << 7;
        }
        // The reservation sentinel is not a pointer and must never be matched,
        // or a caller passing it would be handed a reserved row.
        let e = kstackreg_claim_rs(p, N);
        if kstackreg_find_rs(p, N, KSTACKREG_RESERVED) >= 0 {
            bad |= 1 << 7;
        }
        if kstackreg_take_rs(p, N, KSTACKREG_RESERVED, &mut sink_b, &mut sink_p) != 0 {
            bad |= 1 << 7;
        }
        if kstackreg_take_rs(p, N, 0, &mut sink_b, &mut sink_p) != 0 {
            bad |= 1 << 7;
        }
        kstackreg_abort_rs(p, N, e);
        // Degenerate parameters must fail CLOSED, not wander off the array.
        if kstackreg_claim_rs(core::ptr::null_mut(), N) != -1 {
            bad |= 1 << 7;
        }
        if kstackreg_claim_rs(p, 0) != -1 {
            bad |= 1 << 7;
        }
        if kstackreg_find_rs(core::ptr::null(), N, U1) >= 0 {
            bad |= 1 << 7;
        }
        if (kstackreg_commit_rs(p, N, -1, U1, B1, 16) & KSTACKREG_NOT_RESERVED) == 0 {
            bad |= 1 << 7;
        }
        if (kstackreg_commit_rs(p, N, N as i32, U1, B1, 16) & KSTACKREG_NOT_RESERVED) == 0 {
            bad |= 1 << 7;
        }
        // commit() on a row nobody reserved must SAY so.
        if (kstackreg_commit_rs(p, N, 0, U1, B1, 16) & KSTACKREG_NOT_RESERVED) == 0 {
            bad |= 1 << 7;
        }
    }

    bad
}
