// kstackid.rs - #heapinvptr: is an UNREGISTERED kernel-stack pointer actually a
// kernel stack?
//
// WHY THIS DECISION IS WORTH MAKING EXPLICITLY.
//
// kstack_free() looks its argument up in the live-block registry (mm/kstack.c). A
// miss used to mean exactly one thing, "this came from the counted kmalloc
// fallback", and the code acted on that single reading by calling kfree(). There
// is a SECOND way to miss, and it is a bug rather than a fallback: the block WAS a
// guarded kernel stack whose registry row is already gone, i.e. it is being freed
// a second time, or its registration was lost. kfree() then receives a pointer
// that is not in the heap at all, reads the poisoned guard band under it as a
// block header, and reports
//     [HEAP] ERROR: kfree() called on invalid pointer 0x... (magic=0xbaadb4dd)
// naming the HEAP for a defect that has nothing to do with the heap. That message
// sent one investigation at the allocator and cost a day. The two readings now get
// two different answers.
//
// FAIL-SAFE DIRECTION. Every uncertain case answers FOREIGN, which preserves the
// pre-existing behaviour exactly (fall through to kfree(), which does its own
// validation and will refuse a bad pointer as loudly as before). Only a case this
// predicate is positively sure about is answered ORPHAN, because ORPHAN makes the
// caller REFUSE to free, and refusing to free something that really was a kmalloc
// fallback would leak it.
//
// LANGUAGE: Rust, per the 2026-07-16 rule. This is new logic with no C twin to
// strangle, and it is a pure predicate over integers, so none of the stated
// exemptions (measured performance, entanglement with paging or an FPU-free hot
// path) applies. The caller in mm/kstack.c stays C for the reason that file's own
// header already gives.
#![allow(dead_code)]

/// Not one of ours: let the existing kmalloc-fallback path handle it.
pub const KSTACK_VERDICT_FOREIGN: i32 = 0;
/// A guarded kernel stack with no registry row: a double free or a lost
/// registration. The caller must refuse, not free.
pub const KSTACK_VERDICT_ORPHAN: i32 = 1;

/// Decide what an unregistered `usable` pointer is.
///
/// * `usable` - the pointer `kstack_free()` was handed.
/// * `gran`   - `KSTACK_GRAN`; must be a non-zero power of two.
/// * `band`   - `KSTACK_BAND`; must be non-zero and smaller than `gran`.
/// * `below`  - the 64-bit word read from `usable - 8`, which is the TOP word of
///              what would be the guard band if this were a kernel stack.
/// * `poison` - `KSTACK_POISON`.
/// * `recent` - non-zero if `usable` appears in the caller's ring of recently
///              freed stacks.
///
/// Two independent routes to ORPHAN, because each covers the other's blind spot.
///
/// SHAPE: `kstack_alloc()` hands out `block + band` where `block` is
/// `gran`-aligned, and fills the whole band with `poison`. Both must hold.
/// Alignment alone matches plenty of ordinary allocations, and a poison match
/// alone could be data that happens to carry that value. The shape route's blind
/// spot is that the frames go back to the PMM on the first free, so by the time
/// of a second free they may have been re-issued and the poison overwritten.
///
/// RECENT: the caller's own record of what it freed. Exact while the entry is
/// still in the ring, and silent once it has rotated out, which is the opposite
/// blind spot.
#[no_mangle]
pub extern "C" fn kstack_foreign_verdict_rs(usable: u64, gran: u64, band: u64,
                                            below: u64, poison: u64,
                                            recent: i32) -> i32 {
    // `make KSTACKIDTEST=1`. THE NEGATIVE CONTROL for the boot self-test below:
    // with this arm compiled in every answer is FOREIGN, so the two positive
    // cases must come back wrong and kstackid_selftest_rs() must report a
    // non-zero mask. A self-test nobody has watched FAIL is a self-test nobody
    // knows CAN fail, which is the exact shape of several verifiers in this tree
    // that reported PASS while asking nothing (blame.md). Never for a golden.
    if cfg!(kstackid_test_fail) {
        return KSTACK_VERDICT_FOREIGN;
    }
    if recent != 0 {
        return KSTACK_VERDICT_ORPHAN;
    }
    // Degenerate parameters answer FOREIGN rather than guessing: a predicate
    // asked a nonsense question must not claim a pointer.
    if gran == 0 || !gran.is_power_of_two() {
        return KSTACK_VERDICT_FOREIGN;
    }
    if band == 0 || band >= gran {
        return KSTACK_VERDICT_FOREIGN;
    }
    if usable < band {
        return KSTACK_VERDICT_FOREIGN;
    }
    let block = usable - band;
    if block & (gran - 1) != 0 {
        return KSTACK_VERDICT_FOREIGN;
    }
    if below != poison {
        return KSTACK_VERDICT_FOREIGN;
    }
    KSTACK_VERDICT_ORPHAN
}

/// Self-test. Returns 0 when every case behaved, otherwise a BITMASK of the cases
/// that did not, so a failure names WHICH arm broke rather than only that
/// something did. Called from `kstack_selftest()` on every boot including the
/// golden; it allocates nothing and touches no memory.
#[no_mangle]
pub extern "C" fn kstackid_selftest_rs() -> u32 {
    const GRAN: u64 = 64 * 1024;
    const BAND: u64 = 4096;
    const POISON: u64 = 0xBAAD_B4DD_BAAD_B4DD;
    let f = kstack_foreign_verdict_rs;
    let mut bad = 0u32;

    // POSITIVE, by shape. 0x0fdb1000 is the pointer from the measured failure
    // (see proc/slotdbl.c's header and blame.md), kept verbatim so this test is
    // anchored to a real observation rather than to an invented address.
    if f(0x0fdb_1000, GRAN, BAND, POISON, POISON, 0) != KSTACK_VERDICT_ORPHAN {
        bad |= 1 << 0;
    }
    // POSITIVE, by the recent-free ring, with the poison already overwritten.
    // This is the case the shape route cannot see.
    if f(0x0fdb_1000, GRAN, BAND, 0xdead_beef, POISON, 1) != KSTACK_VERDICT_ORPHAN {
        bad |= 1 << 1;
    }

    // NEGATIVE CONTROLS. A predicate that says yes to everything protects
    // nothing. Right shape, wrong contents: a kmalloc fallback that happens to
    // sit a band above a granule boundary must NOT be claimed.
    if f(0x0fdb_1000, GRAN, BAND, 0, POISON, 0) != KSTACK_VERDICT_FOREIGN {
        bad |= 1 << 2;
    }
    // Right contents, wrong alignment.
    if f(0x0fdb_1040, GRAN, BAND, POISON, POISON, 0) != KSTACK_VERDICT_FOREIGN {
        bad |= 1 << 3;
    }
    // Degenerate parameters must fail CLOSED.
    if f(0x0fdb_1000, 0, BAND, POISON, POISON, 0) != KSTACK_VERDICT_FOREIGN {
        bad |= 1 << 4;
    }
    if f(0x0fdb_1000, GRAN + 1, BAND, POISON, POISON, 0) != KSTACK_VERDICT_FOREIGN {
        bad |= 1 << 5;
    }
    if f(0x0fdb_1000, GRAN, 0, POISON, POISON, 0) != KSTACK_VERDICT_FOREIGN {
        bad |= 1 << 6;
    }
    if f(0x0fdb_1000, GRAN, GRAN, POISON, POISON, 0) != KSTACK_VERDICT_FOREIGN {
        bad |= 1 << 7;
    }
    // A pointer below one band cannot be a usable base, and the subtraction must
    // not wrap on the way to finding that out.
    if f(8, GRAN, BAND, POISON, POISON, 0) != KSTACK_VERDICT_FOREIGN {
        bad |= 1 << 8;
    }

    bad
}
