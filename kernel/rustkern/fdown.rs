// rustkern/fdown.rs - #fdguard: OWNERSHIP of the SYSTEM-WIDE legacy file
// descriptor tables (proc/fdlayer.c: fd_table[]/e2fd[]/smbfd[], the FAT / ext2
// / SMB / NFS regular-file handles that live in the 256..383 range #FDNS gave
// them).
//
// New kernel logic (there is no C twin to strangle), so Rust per the
// 2026-07-16 rule, and the same shape as fetchown.rs / fbown.rs / sinkown.rs:
// the INPUTS come from unchanged C (proc_current()'s thread-group id, the slot
// index the open scan already picked) and the OWNERSHIP DECISION lives here.
//
// ===========================================================================
// THE DEFECT THIS REMOVES, MEASURED ON dev BEFORE THE CHANGE
// ---------------------------------------------------------------------------
// The three legacy tables are ONE pool shared by every process on the box, and
// every fd-consuming syscall (sys_read/write/close/seek/fsync/ftruncate/
// readdir/fstat) indexed them by lfd_idx(fd) and checked ONLY the per-slot
// used flag. There was NO record of WHICH process opened a slot, so a Ring 3
// process could read, write or close a file another process had open just by
// passing that fd number. #FDNS moved the legacy range to 256..383, so the
// guess is no longer a small integer, but the range is small and fixed and a
// loop over it lands on whatever anyone else has open. The owner's stated
// principle is that cross-process file access should only be possible with a
// negotiated contract; a shared table with no owner is the opposite of that.
//
// WHY THE OWNER IS A THREAD GROUP, NOT A RAW PID. pthreads share one address
// space and legitimately share open files: a process may open a file on one
// thread and read it on another. A thread here is a process_t with
// shares_vm=1 whose tgid names the group leader (proc/process.c), so the owner
// is the tgid: threads of one process all match, and a DIFFERENT process (a
// fork child gets its own pid as tgid, an unrelated app its own) does not.
// This is the identical reasoning fetchown.rs uses for the async HTTP slots.
//
// A slot's owner word is 0 when free, a tgid when owned, or the SAME tgid with
// a high state bit set once the owning process exited without closing it (see
// fdown_mark_dead_if_owner_rs): a real tgid can never carry those bits, so a
// leaked slot can never be inherited by a future process that happens to be
// handed the dead owner's pid.
//
// ===========================================================================
// #fdreclaim (2026-09-28): POISONING A SLOT IS NOT RECLAIMING IT.
// ---------------------------------------------------------------------------
// Until this change, a process that died without close()ing burned its slots
// PERMANENTLY. fdown_proc_exit() marked them dead so nobody could reach them,
// and nothing ever handed them back: no code cleared the C side's fd_used[i]
// or e2fd[i].used, and nothing kfree'd e2fd[i].rbuf, the #572 read window of up
// to 128 KiB. legacy_fd_release() was reached only from FD_FAIL and sys_close.
// So every fatal()/abort exit shrank a SYSTEM-WIDE 128-slot pool for the rest
// of the boot, and leaked up to 128 KiB of kernel heap per slot with it. That
// is monotonic, which is exactly the shape of "works on the first launch,
// fails on the third".
//
// WHY THE OWNER IS KEPT IN THE SAME WORD, NOT IN A SECOND ARRAY. Reclaim has to
// answer "which dead slots were THIS group's", and the poison used to overwrite
// the tgid with a single DEAD sentinel, destroying that answer. A parallel
// dead-owner array cannot restore it safely: storing the owner beside the
// compare_exchange is not atomic with it, so two groups exiting concurrently
// can interleave a store with the other's CAS and one group can be handed the
// other's slot to free, which is a use-after-free with extra steps. Encoding
// the state in the TOP BITS of the one word makes every transition a single
// compare_exchange on a value that names both the state and the owner, so the
// race is not merely unlikely, it is unrepresentable.
//
//   0                       free
//   tgid                    live, owned by tgid
//   tgid | DEAD_BIT         owner exited; unreachable, not yet reclaimed
//   tgid | DEAD_BIT|REAP_BIT  a reclaimer owns the teardown of this slot
//
// A tgid is bounded by OWNER_MASK, so a live owner can never collide with a
// dead or reaping encoding, and arr_check() needs no new cases: anything that
// is not 0 and not exactly the caller's own tgid is refused, as before.
// ===========================================================================

use core::sync::atomic::{AtomicU32, Ordering};

// MUST equal LEGACY_MAX_FDS in proc/fdlayer.c. fdown_slots_rs() hands this back
// and fdlayer.c asserts it against LEGACY_MAX_FDS at boot, so the two cannot
// silently disagree about the table size (a smaller Rust table would leave
// high slots unguarded; a larger one would index past the C array on nothing).
const LEGACY_SLOTS: usize = 128;

// State bits carried in the TOP of a slot's owner word (see the #fdreclaim
// block above). A real tgid must fit in OWNER_MASK, which every pid in this
// kernel does by many orders of magnitude, so a live owner can never be
// mistaken for a dead or reaping one.
const DEAD_BIT: u32   = 0x8000_0000;   // owner exited without closing
const REAP_BIT: u32   = 0x4000_0000;   // a reclaimer owns the teardown
const OWNER_MASK: u32 = 0x3FFF_FFFF;   // the bits a tgid may occupy

// A tgid that may legitimately own a slot: nonzero and free of the state bits.
#[inline]
fn valid_owner(owner: u32) -> bool {
    owner != 0 && (owner & !OWNER_MASK) == 0
}

// Returned to C. The C guard maps these to an action: R_OK proceeds, R_FREE
// falls through to the existing used-flag check (so an ordinary closed-fd
// access still returns EBADF with no audit noise), R_NOTOWNER is refused and
// audited. A live slot owned by someone else and a DEAD slot both give
// R_NOTOWNER: the caller must not be able to tell them apart.
const R_OK: i32 = 0;
const R_FREE: i32 = -1;
const R_NOTOWNER: i32 = -2;

static OWN: [AtomicU32; LEGACY_SLOTS] = [const { AtomicU32::new(0) }; LEGACY_SLOTS];

// Self-test table: a separate array with no C slots behind it, so the state
// machine can be exercised at boot without disturbing a live open file. The
// live path never touches it.
const TEST_SLOTS: usize = 8;
static TOWN: [AtomicU32; TEST_SLOTS] = [const { AtomicU32::new(0) }; TEST_SLOTS];

// Audit counters, read back for the boot/exit log lines so the guard is
// OBSERVED FIRING rather than assumed present (blame.md: a guard nobody has
// watched fire is indistinguishable from one switched off).
static N_REFUSE: AtomicU32 = AtomicU32::new(0);
static N_EXIT_MARK: AtomicU32 = AtomicU32::new(0);
// #fdreclaim: slots actually HANDED BACK to the pool after their owner died, as
// opposed to merely poisoned. The two counters are printed side by side so the
// difference (poisoned but never reclaimed) is visible rather than inferred.
static N_REAPED: AtomicU32 = AtomicU32::new(0);

// ---- ONE implementation, shared by the live path and the self-test ----------
#[inline]
fn arr_claim(arr: &[AtomicU32], slot: u32, owner: u32) -> i32 {
    if slot as usize >= arr.len() || !valid_owner(owner) {
        return R_FREE;
    }
    arr[slot as usize].store(owner, Ordering::Release);
    R_OK
}

#[inline]
fn arr_release(arr: &[AtomicU32], slot: u32) -> i32 {
    if slot as usize >= arr.len() {
        return R_FREE;
    }
    arr[slot as usize].store(0, Ordering::Release);
    R_OK
}

// #fdreclaim: ONE implementation of "claim the teardown of a dead slot",
// shared by the live path and the self-test, for the same reason arr_claim and
// arr_release are shared: a self-test that exercises a second copy of the logic
// proves nothing about the first.
#[inline]
fn arr_take(arr: &[AtomicU32], slot: u32, owner: u32) -> i32 {
    if slot as usize >= arr.len() || !valid_owner(owner) {
        return 0;
    }
    if arr[slot as usize]
        .compare_exchange(owner | DEAD_BIT, owner | DEAD_BIT | REAP_BIT,
                          Ordering::AcqRel, Ordering::Acquire)
        .is_ok()
    {
        1
    } else {
        0
    }
}

#[inline]
fn arr_check(arr: &[AtomicU32], slot: u32, owner: u32, count: bool) -> i32 {
    if slot as usize >= arr.len() {
        return R_FREE;
    }
    let cur = arr[slot as usize].load(Ordering::Acquire);
    if cur == 0 {
        // Nobody owns it: not a cross-process access, just a free/closed slot.
        // Let the C side's existing used check answer, and do NOT audit.
        return R_FREE;
    }
    if valid_owner(owner) && cur == owner {
        return R_OK;
    }
    if count {
        N_REFUSE.fetch_add(1, Ordering::Relaxed);
    }
    R_NOTOWNER
}

// ---- the C-facing surface (always the LIVE table) ---------------------------

/// Slot count this module guards. fdlayer.c asserts it against LEGACY_MAX_FDS.
#[no_mangle]
pub extern "C" fn fdown_slots_rs() -> i32 {
    LEGACY_SLOTS as i32
}

/// Stamp `slot` as owned by `owner` (a tgid). Called at the ONE place fdlayer.c
/// claims a legacy slot under g_legacy_fd_lock. owner==0 (no process context,
/// i.e. an early-boot kernel open) leaves the slot free; such opens are
/// synchronous and closed before any Ring 3 process could reach the slot.
#[no_mangle]
pub extern "C" fn fdown_claim_rs(slot: u32, owner: u32) -> i32 {
    arr_claim(&OWN, slot, owner)
}

/// Clear ownership. Called at the ONE place fdlayer.c releases a legacy slot
/// (legacy_fd_release), so claim and release are the same chokepoint pair as
/// fd_used[i]=1 / fd_used[fd]=0.
#[no_mangle]
pub extern "C" fn fdown_release_rs(slot: u32) -> i32 {
    arr_release(&OWN, slot)
}

/// THE GUARD. R_OK if `owner` owns this live slot, R_FREE if nobody does (the
/// caller is looking at a free/closed slot; let the existing check answer),
/// R_NOTOWNER if someone else does or it is DEAD (refuse and audit). Counts
/// every refusal.
#[no_mangle]
pub extern "C" fn fdown_check_rs(slot: u32, owner: u32) -> i32 {
    arr_check(&OWN, slot, owner, true)
}

/// The slot's owning tgid with the state bits MASKED OFF, so a dead slot still
/// reports who leaked it. Diagnostics only, Ring 0: "who owns slot N" is exactly
/// what the single refusal path withholds from userland, so this is never
/// returned to Ring 3. Pair it with fdown_state_rs() for the state.
#[no_mangle]
pub extern "C" fn fdown_owner_rs(slot: u32) -> u32 {
    if slot as usize >= LEGACY_SLOTS {
        return 0;
    }
    OWN[slot as usize].load(Ordering::Acquire) & OWNER_MASK
}

/// proc_exit backstop: if `owner` still owns `slot`, poison it (set DEAD_BIT,
/// KEEPING the owner in the low bits) so a future process handed the same pid
/// cannot inherit a leaked slot, and so fdown_take_for_reap_rs() can later tell
/// whose leaked slot it is. Returns 1 if it marked one. A single
/// compare_exchange, no allocation, no block: callable under cli() from
/// proc_exit() beside the fbown/fetchown hooks.
#[no_mangle]
pub extern "C" fn fdown_mark_dead_if_owner_rs(slot: u32, owner: u32) -> i32 {
    if slot as usize >= LEGACY_SLOTS || !valid_owner(owner) {
        return 0;
    }
    if OWN[slot as usize]
        .compare_exchange(owner, owner | DEAD_BIT, Ordering::AcqRel, Ordering::Acquire)
        .is_ok()
    {
        N_EXIT_MARK.fetch_add(1, Ordering::Relaxed);
        1
    } else {
        0
    }
}

/// #fdreclaim: CLAIM THE TEARDOWN of a slot this group leaked. Returns 1 only if
/// `slot` is dead AND was owned by `owner` AND this call is the one that moved it
/// to REAPING; the caller must then free the slot's buffers and finish with
/// fdown_release_rs(), which returns the slot to the free pool.
///
/// Exactly one caller can ever get a 1 for a given death, because the transition
/// is one compare_exchange on a value that names both the state and the owner. A
/// slot in REAPING is still refused to everyone by fdown_check_rs() (it is
/// neither 0 nor any live tgid), so no Ring 3 caller can reach a buffer that is
/// being freed.
#[no_mangle]
pub extern "C" fn fdown_take_for_reap_rs(slot: u32, owner: u32) -> i32 {
    if arr_take(&OWN, slot, owner) == 1 {
        N_REAPED.fetch_add(1, Ordering::Relaxed);
        return 1;
    }
    0
}

/// Diagnostics only: 0 free, 1 live, 2 dead (leaked, not yet reclaimed),
/// 3 being reclaimed. Lets the audit line say WHY a refusal happened instead of
/// printing a bit-encoded owner word at a reader.
#[no_mangle]
pub extern "C" fn fdown_state_rs(slot: u32) -> i32 {
    if slot as usize >= LEGACY_SLOTS {
        return 0;
    }
    let cur = OWN[slot as usize].load(Ordering::Acquire);
    if cur == 0 {
        0
    } else if (cur & REAP_BIT) != 0 {
        3
    } else if (cur & DEAD_BIT) != 0 {
        2
    } else {
        1
    }
}

#[no_mangle]
pub extern "C" fn fdown_reaped_rs() -> u32 {
    N_REAPED.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn fdown_refusals_rs() -> u32 {
    N_REFUSE.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn fdown_exit_marks_rs() -> u32 {
    N_EXIT_MARK.load(Ordering::Relaxed)
}

// ===========================================================================
// SELF-TEST. Runs at boot against TOWN (no C table behind it). Returns 0, or
// the negated number of the first failing step, which fdlayer.c prints. The
// property being added is NEGATIVE (a non-owner is refused), and a negative
// property never observed to hold is indistinguishable from an absent one, so
// this must be able to go RED and be watched doing so (make ... selftest).
// ===========================================================================
#[no_mangle]
pub extern "C" fn fdown_selftest_rs() -> i32 {
    for i in 0..TEST_SLOTS {
        TOWN[i].store(0, Ordering::Release);
    }
    let a = 111u32; // stand-in owner A (a tgid)
    let b = 222u32; // stand-in owner B

    // 1: a free slot is R_FREE for anyone, and it is NOT a refusal (an ordinary
    //    closed-fd probe must not audit).
    if arr_check(&TOWN, 0, a, false) != R_FREE {
        return -1;
    }
    // 2: claim stamps the owner; the owner passes.
    if arr_claim(&TOWN, 0, a) != R_OK {
        return -2;
    }
    if arr_check(&TOWN, 0, a, false) != R_OK {
        return -3;
    }
    // 3: a DIFFERENT owner is refused, and an unowned caller (owner 0) too.
    if arr_check(&TOWN, 0, b, false) != R_NOTOWNER {
        return -4;
    }
    if arr_check(&TOWN, 0, 0, false) != R_NOTOWNER {
        return -5;
    }
    // 4: out of range is R_FREE, never a match.
    if arr_check(&TOWN, TEST_SLOTS as u32, a, false) != R_FREE {
        return -6;
    }
    // 5: release frees it; the old owner no longer matches.
    if arr_release(&TOWN, 0) != R_OK {
        return -7;
    }
    if arr_check(&TOWN, 0, a, false) != R_FREE {
        return -8;
    }
    // 6: a re-claim by B is B's; A does not inherit it.
    if arr_claim(&TOWN, 0, b) != R_OK {
        return -9;
    }
    if arr_check(&TOWN, 0, a, false) != R_NOTOWNER {
        return -10;
    }
    // 7: DEAD poison: after marking, even the original owner is refused, and it
    //    is not free (so no pass-through can serve it).
    if TOWN[0]
        .compare_exchange(b, b | DEAD_BIT, Ordering::AcqRel, Ordering::Acquire)
        .is_err()
    {
        return -11;
    }
    if arr_check(&TOWN, 0, b, false) != R_NOTOWNER {
        return -12;
    }
    if arr_check(&TOWN, 0, 0, false) != R_NOTOWNER {
        return -13;
    }
    // 8 (#fdreclaim): THE RECLAIM CYCLE, which is the property being added and
    //    so the one that must be watched going red. A dead slot may be taken for
    //    teardown by its OWN dead owner and by nobody else; while it is being
    //    torn down it is still refused to everyone; and once released it is FREE
    //    and claimable again, which is the whole point (a poisoned-only slot was
    //    never claimable again, which is the leak).
    if arr_take(&TOWN, 0, a) != 0 {
        return -14;                  // A did not leak it; A must not free it
    }
    if arr_take(&TOWN, 0, b) != 1 {
        return -15;                  // B leaked it; B tears it down
    }
    if arr_take(&TOWN, 0, b) != 0 {
        return -16;                  // and exactly once: no double free
    }
    if arr_check(&TOWN, 0, b, false) != R_NOTOWNER {
        return -17;                  // refused WHILE its buffers are freed
    }
    if arr_release(&TOWN, 0) != R_OK {
        return -18;
    }
    if arr_check(&TOWN, 0, b, false) != R_FREE {
        return -19;                  // back in the pool
    }
    if arr_claim(&TOWN, 0, a) != R_OK || arr_check(&TOWN, 0, a, false) != R_OK {
        return -20;                  // and a NEW owner can really have it
    }
    // 9: a malformed owner (one carrying the state bits) is never an owner, so a
    //    caller cannot forge the dead or reaping encoding and be handed a slot.
    if arr_claim(&TOWN, 1, DEAD_BIT | 5) != R_FREE {
        return -21;
    }
    if arr_check(&TOWN, 0, a | DEAD_BIT, false) != R_NOTOWNER {
        return -22;
    }

    for i in 0..TEST_SLOTS {
        TOWN[i].store(0, Ordering::Release);
    }
    0
}
