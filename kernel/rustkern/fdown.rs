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
// A slot's owner word is 0 when free, a tgid when owned, or DEAD after the
// owning process exited without closing it (see fdown_mark_dead_if_owner_rs):
// DEAD is neither 0 nor any live tgid, so a leaked slot can never be inherited
// by a future process that happens to be handed the dead owner's pid.
// ===========================================================================

use core::sync::atomic::{AtomicU32, Ordering};

// MUST equal LEGACY_MAX_FDS in proc/fdlayer.c. fdown_slots_rs() hands this back
// and fdlayer.c asserts it against LEGACY_MAX_FDS at boot, so the two cannot
// silently disagree about the table size (a smaller Rust table would leave
// high slots unguarded; a larger one would index past the C array on nothing).
const LEGACY_SLOTS: usize = 128;

// The poison value for a slot whose owner exited without closing it. Not 0 (a
// free slot) and not any real tgid (pids are far below this), so no live
// process ever matches it and the pass-through for a free slot never applies.
const DEAD: u32 = 0xFFFF_FFFF;

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

// ---- ONE implementation, shared by the live path and the self-test ----------
#[inline]
fn arr_claim(arr: &[AtomicU32], slot: u32, owner: u32) -> i32 {
    if slot as usize >= arr.len() || owner == 0 || owner == DEAD {
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
    if owner != 0 && owner != DEAD && cur == owner {
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

/// Diagnostics only, Ring 0. "who owns slot N" is exactly what the single
/// refusal path withholds from userland, so this is never returned to Ring 3.
#[no_mangle]
pub extern "C" fn fdown_owner_rs(slot: u32) -> u32 {
    if slot as usize >= LEGACY_SLOTS {
        return 0;
    }
    OWN[slot as usize].load(Ordering::Acquire)
}

/// proc_exit backstop: if `owner` still owns `slot`, poison it to DEAD so a
/// future process handed the same pid cannot inherit a leaked slot. Returns 1
/// if it marked one. A single compare_exchange, no allocation, no block:
/// callable under cli() from proc_exit() beside the fbown/fetchown hooks.
#[no_mangle]
pub extern "C" fn fdown_mark_dead_if_owner_rs(slot: u32, owner: u32) -> i32 {
    if slot as usize >= LEGACY_SLOTS || owner == 0 || owner == DEAD {
        return 0;
    }
    if OWN[slot as usize]
        .compare_exchange(owner, DEAD, Ordering::AcqRel, Ordering::Acquire)
        .is_ok()
    {
        N_EXIT_MARK.fetch_add(1, Ordering::Relaxed);
        1
    } else {
        0
    }
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
    if TOWN[0].compare_exchange(b, DEAD, Ordering::AcqRel, Ordering::Acquire).is_err() {
        return -11;
    }
    if arr_check(&TOWN, 0, b, false) != R_NOTOWNER {
        return -12;
    }
    if arr_check(&TOWN, 0, 0, false) != R_NOTOWNER {
        return -13;
    }

    for i in 0..TEST_SLOTS {
        TOWN[i].store(0, Ordering::Release);
    }
    0
}
