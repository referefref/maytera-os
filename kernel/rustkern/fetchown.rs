// rustkern/fetchown.rs - #745 (task #36): OWNERSHIP AND LIFETIME of the async
// HTTP job slots (proc/syscall.c: g_async_fetch[6], g_async_post[4]).
//
// New kernel logic (there is no C twin to strangle), so Rust per the
// 2026-07-16 rule, and the same shape as elevate.rs/spawnid.rs: the INPUTS
// come from existing, unchanged C (proc_current(), the job tables themselves)
// and the DECISION lives here.
//
// ===========================================================================
// THE TWO DEFECTS THIS REMOVES, BOTH MEASURED ON dev BEFORE THE CHANGE
// ---------------------------------------------------------------------------
// 1. NO OWNERSHIP CHECK. sys_http_fetch_poll/read/cancel/progress and
//    sys_http_post_poll/read/cancel validated ONLY `id in range` and
//    `in_use != 0`. Any Ring 3 process could read another process's in-flight
//    response body (the App Store's SIGNED MANIFEST, every LLM POST reply) or
//    destroy its transfer, by passing an index it never allocated. It was a
//    missing check on an INDEX, so it did not care about uid: it stayed
//    exploitable between processes at any privilege.
//
// 2. SLOTS LEAKED ON PROCESS DEATH. Nothing released a slot when its owner
//    died. Six crashed fetches exhausted a six-slot table until reboot: a
//    one-line denial of service from an unprivileged app.
//
// WHY THE STATE MACHINE IS HERE AND NOT A PAIR OF C IF-STATEMENTS. The slot
// lifetime has three concurrent participants (the owning Ring 3 process, the
// kernel worker thread, and now process teardown on another core), and the C
// it replaces expressed that with two separate `volatile int`s, `in_use` and
// `detached`, updated in five places with no atomic between them. Two of the
// transitions are genuinely racy: an owner that CANCELs at the instant the
// worker publishes its result, and an allocator that scanned for a free slot
// and then set `in_use` AFTER a copy_from_user that can fault (so two callers
// could be handed the SAME slot). Here the whole lifetime is one AtomicU64 per
// slot and every transition is a single compare-exchange, so "two owners of
// one slot" and "a slot freed while its worker still writes to it" are not
// states the type can hold.
//
// SLOT WORD LAYOUT (one AtomicU64 per slot):
//   bits  0..31  owner: the OWNING PROCESS's thread-group id. 0 = nobody.
//                A thread group, not a raw pid, so an app that starts a fetch
//                on one pthread and polls it from another still owns it
//                (kernel/proc/process.c: a thread is a process_t with
//                shares_vm=1 whose tgid names the leader).
//   bit   32     WLIVE:  a worker is still going to touch this job's record.
//   bit   33     ORPHAN: the owner went away while the job was still running;
//                the worker frees the response body when it finishes. This is
//                the old `detached` flag, and it is deliberately NOT inferred
//                from `owner == 0`: a normal READ also clears the owner, and
//                THAT path frees the body itself. Conflating the two is a
//                double free.
//
// A slot is allocatable iff its whole word is 0 (no owner AND no live worker).
// ===========================================================================

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

// Table ids, mirrored by FETCHOWN_TAB_* in proc/fetchown.h.
const TAB_FETCH: usize = 0; // g_async_fetch, 6 slots
const TAB_POST: usize = 1; // g_async_post, 4 slots
const TAB_TEST: usize = 2; // self-test only; no C table behind it
const NTAB: usize = 3;
const SLOTS_MAX: usize = 6;

// Slot counts per table. Locked against the C tables by fetchown_slots_rs(),
// which proc/syscall.c checks with a _Static_assert-backed boot assertion.
const SLOTS: [u32; NTAB] = [6, 4, 6];

const OWNER_MASK: u64 = 0xFFFF_FFFF;
const F_WLIVE: u64 = 1 << 32;
const F_ORPHAN: u64 = 1 << 33;

// Refusal codes returned to C. C maps BOTH refusals to ONE value at the
// syscall boundary (see proc/syscall.c) so userland cannot tell "no such job"
// from "somebody else's job"; the distinction exists only for the Ring 0
// audit line.
const R_OK: i32 = 0;
const R_NOSLOT: i32 = -1; // out of range, or nobody owns it
const R_NOTOWNER: i32 = -2; // live slot, different owner

// One word per slot. Written out per table rather than with a repeat
// expression: an array repeat of a non-Copy element needs the inline-const
// form, and AtomicU64 is deliberately not Copy.
static TABLES: [[AtomicU64; SLOTS_MAX]; NTAB] = [
    [const { AtomicU64::new(0) }; SLOTS_MAX],
    [const { AtomicU64::new(0) }; SLOTS_MAX],
    [const { AtomicU64::new(0) }; SLOTS_MAX],
];

// Audit counters. Read back by fetchown_stats_rs() and printed on the serial
// console, which is how the guard is OBSERVED FIRING rather than assumed to
// (blame.md: a guard that has never been watched fire is indistinguishable
// from one that is switched off).
static N_REFUSE_NOTOWNER: AtomicU32 = AtomicU32::new(0);
static N_REFUSE_NOSLOT: AtomicU32 = AtomicU32::new(0);
static N_EXIT_RELEASED: AtomicU32 = AtomicU32::new(0);

/// Per-table snapshot handed to C. Mirrored by fetchown_stats_t in
/// proc/fetchown.h and sizeof-locked on both sides.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FetchownStats {
    pub slots: u32,             // slots in this table
    pub owned: u32,             // slots with a live owner
    pub orphaned: u32,          // owner gone, worker still running
    pub refused_notowner: u32,  // global: ownership violations refused
    pub refused_noslot: u32,    // global: bad/free index refused
    pub exit_released: u32,     // global: slots reclaimed by the exit hook
}
const _: () = assert!(core::mem::size_of::<FetchownStats>() == 24);

#[inline]
fn slot_ref(tab: u32, slot: u32) -> Option<&'static AtomicU64> {
    let t = tab as usize;
    if t >= NTAB {
        return None;
    }
    if slot >= SLOTS[t] {
        return None;
    }
    Some(&TABLES[t][slot as usize])
}

/// Number of slots this module believes table `tab` has. proc/syscall.c
/// asserts this against its own ASYNC_FETCH_MAX / ASYNC_POST_MAX at boot, so
/// the two can never silently disagree about the table size.
#[no_mangle]
pub extern "C" fn fetchown_slots_rs(tab: u32) -> i32 {
    if (tab as usize) >= NTAB {
        return -1;
    }
    SLOTS[tab as usize] as i32
}

/// Claim a free slot for `owner`, marking a worker as live on it. Returns the
/// slot index, or -1 if the table is full (or `owner` is 0, which is refused:
/// 0 is the "nobody" sentinel, so an unowned caller must not be able to
/// allocate a slot no one can ever authenticate against).
///
/// The scan-and-claim is a single compare_exchange per slot, so two callers
/// racing on the last free slot cannot both win. The C this replaces chose a
/// slot, then ran a faultable copy_from_user, and only THEN set `in_use`.
#[no_mangle]
pub extern "C" fn fetchown_claim_rs(tab: u32, owner: u32) -> i32 {
    let t = tab as usize;
    if t >= NTAB || owner == 0 {
        return -1;
    }
    for i in 0..SLOTS[t] as usize {
        let w = &TABLES[t][i];
        if w.compare_exchange(0, owner as u64 | F_WLIVE, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            return i as i32;
        }
    }
    -1
}

/// The claim never became a job (the URL copy faulted, the worker thread could
/// not be created, the request was refused downstream). Return the slot to the
/// pool. Safe to call only before any worker has been started on it.
#[no_mangle]
pub extern "C" fn fetchown_abandon_rs(tab: u32, slot: u32) -> i32 {
    match slot_ref(tab, slot) {
        Some(w) => {
            w.store(0, Ordering::Release);
            R_OK
        }
        None => R_NOSLOT,
    }
}

/// THE GUARD. 0 if `owner` owns this live slot; R_NOTOWNER if somebody else
/// does; R_NOSLOT if the index is out of range or the slot has no owner.
/// Counts every refusal.
#[no_mangle]
pub extern "C" fn fetchown_check_rs(tab: u32, slot: u32, owner: u32) -> i32 {
    let w = match slot_ref(tab, slot) {
        Some(w) => w,
        None => {
            N_REFUSE_NOSLOT.fetch_add(1, Ordering::Relaxed);
            return R_NOSLOT;
        }
    };
    let cur = (w.load(Ordering::Acquire) & OWNER_MASK) as u32;
    if cur == 0 {
        N_REFUSE_NOSLOT.fetch_add(1, Ordering::Relaxed);
        return R_NOSLOT;
    }
    if owner == 0 || cur != owner {
        N_REFUSE_NOTOWNER.fetch_add(1, Ordering::Relaxed);
        return R_NOTOWNER;
    }
    R_OK
}

/// Is `slot` owned by `owner` right now? 1/0. Used by the process-exit sweep,
/// which must not count as a refusal (it is not a Ring 3 access).
#[no_mangle]
pub extern "C" fn fetchown_owned_by_rs(tab: u32, slot: u32, owner: u32) -> i32 {
    if owner == 0 {
        return 0;
    }
    match slot_ref(tab, slot) {
        Some(w) => {
            if (w.load(Ordering::Acquire) & OWNER_MASK) as u32 == owner {
                1
            } else {
                0
            }
        }
        None => 0,
    }
}

/// The owning process is finished with a job that has ALREADY completed: drop
/// the ownership. The caller has freed the response body itself. The slot
/// becomes allocatable once the worker has also cleared WLIVE.
#[no_mangle]
pub extern "C" fn fetchown_release_rs(tab: u32, slot: u32) -> i32 {
    match slot_ref(tab, slot) {
        Some(w) => {
            w.fetch_and(!OWNER_MASK, Ordering::AcqRel);
            R_OK
        }
        None => R_NOSLOT,
    }
}

/// The owner is gone (CANCEL, or process exit) while the job is STILL RUNNING.
/// Clear the owner and mark ORPHAN, so the worker frees the response body when
/// it finishes and the slot returns to the pool then. Returns 1 if the slot
/// was actually orphaned.
#[no_mangle]
pub extern "C" fn fetchown_orphan_rs(tab: u32, slot: u32) -> i32 {
    let w = match slot_ref(tab, slot) {
        Some(w) => w,
        None => return R_NOSLOT,
    };
    loop {
        let cur = w.load(Ordering::Acquire);
        if cur & F_WLIVE == 0 {
            // No worker will ever run again: nothing to hand over. Just drop
            // the owner; the caller's own teardown frees the body.
            w.fetch_and(!OWNER_MASK, Ordering::AcqRel);
            return 0;
        }
        let next = (cur & !OWNER_MASK) | F_ORPHAN;
        if w.compare_exchange(cur, next, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            return 1;
        }
    }
}

/// The worker has stopped touching this job's record. Clears WLIVE (and
/// ORPHAN), which is what finally returns an orphaned slot to the pool.
/// Returns 1 if the slot was ORPHANed, meaning the WORKER must free the
/// response body because no owner is left to read it.
#[no_mangle]
pub extern "C" fn fetchown_worker_done_rs(tab: u32, slot: u32) -> i32 {
    let w = match slot_ref(tab, slot) {
        Some(w) => w,
        None => return 0,
    };
    loop {
        let cur = w.load(Ordering::Acquire);
        let orphan = (cur & F_ORPHAN) != 0;
        let next = cur & !(F_WLIVE | F_ORPHAN);
        if w.compare_exchange(cur, next, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            return if orphan { 1 } else { 0 };
        }
    }
}

/// Diagnostics: the owner of a slot (0 = none). Ring 0 only; never returned to
/// userland, because "who owns slot 3" is exactly the fact the single refusal
/// code exists to withhold.
#[no_mangle]
pub extern "C" fn fetchown_owner_rs(tab: u32, slot: u32) -> u32 {
    match slot_ref(tab, slot) {
        Some(w) => (w.load(Ordering::Acquire) & OWNER_MASK) as u32,
        None => 0,
    }
}

/// Count this exit-hook reclamation (for the audit line).
#[no_mangle]
pub extern "C" fn fetchown_note_exit_release_rs() {
    N_EXIT_RELEASED.fetch_add(1, Ordering::Relaxed);
}

/// Snapshot for the serial audit line. Returns 0, or -1 on a bad table id.
#[no_mangle]
pub extern "C" fn fetchown_stats_rs(tab: u32, out: *mut FetchownStats) -> i32 {
    let t = tab as usize;
    if t >= NTAB || out.is_null() {
        return -1;
    }
    let mut s = FetchownStats {
        slots: SLOTS[t],
        owned: 0,
        orphaned: 0,
        refused_notowner: N_REFUSE_NOTOWNER.load(Ordering::Relaxed),
        refused_noslot: N_REFUSE_NOSLOT.load(Ordering::Relaxed),
        exit_released: N_EXIT_RELEASED.load(Ordering::Relaxed),
    };
    for i in 0..SLOTS[t] as usize {
        let w = TABLES[t][i].load(Ordering::Acquire);
        if (w & OWNER_MASK) != 0 {
            s.owned += 1;
        }
        if (w & F_ORPHAN) != 0 {
            s.orphaned += 1;
        }
    }
    // SAFETY: `out` is a caller-provided fetchown_stats_t; the C side always
    // passes the address of a stack object of exactly this type, and the two
    // layouts are locked by the _Static_assert in proc/fetchown.h against the
    // const assert above. One write of a Copy struct, no aliasing.
    unsafe { core::ptr::write(out, s) };
    0
}

// ===========================================================================
// SELF-TEST. Runs at boot against TAB_TEST, a table with no C job array behind
// it, so it can exercise every transition (including the ones that only occur
// under a race) without touching a live download. Returns 0 on success or the
// negated number of the first failing step, which main.c prints.
//
// This exists because the two properties being added are both NEGATIVE
// (a non-owner is refused; a dead owner's slot comes back), and a negative
// property that is never observed to hold is indistinguishable from an absent
// one.
// ===========================================================================
#[no_mangle]
pub extern "C" fn fetchown_selftest_rs() -> i32 {
    let t = TAB_TEST as u32;
    for i in 0..SLOTS[TAB_TEST] {
        TABLES[TAB_TEST][i as usize].store(0, Ordering::Release);
    }
    let a = 100u32; // stand-in owner A
    let b = 200u32; // stand-in owner B

    // 1: claim hands out distinct slots and fills the table exactly once.
    let mut got = [0i32; SLOTS_MAX];
    for i in 0..SLOTS[TAB_TEST] as usize {
        got[i] = fetchown_claim_rs(t, a);
        if got[i] != i as i32 {
            return -1;
        }
    }
    if fetchown_claim_rs(t, a) != -1 {
        return -2; // full table must refuse
    }
    // 2: owner passes, non-owner is refused with the DISTINCT internal code,
    //    and an unowned caller (owner 0) is refused too.
    if fetchown_check_rs(t, 0, a) != R_OK {
        return -3;
    }
    if fetchown_check_rs(t, 0, b) != R_NOTOWNER {
        return -4;
    }
    if fetchown_check_rs(t, 0, 0) != R_NOTOWNER {
        return -5;
    }
    if fetchown_check_rs(t, SLOTS[TAB_TEST], a) != R_NOSLOT {
        return -6; // out of range
    }
    // 3: a completed job released by its owner frees the slot only after the
    //    worker has also finished with it.
    fetchown_worker_done_rs(t, 0);
    fetchown_release_rs(t, 0);
    if fetchown_check_rs(t, 0, a) != R_NOSLOT {
        return -7;
    }
    if fetchown_claim_rs(t, b) != 0 {
        return -8; // slot 0 must be reusable, now owned by B
    }
    if fetchown_check_rs(t, 0, a) != R_NOTOWNER {
        return -9; // A must not inherit B's slot
    }
    // 4: an owner that dies mid-transfer orphans the slot; it is NOT
    //    allocatable while the worker still runs, and IS once it finishes.
    //    Slot 5 is returned to the pool first, so "the allocator skipped the
    //    orphan" is proven by it choosing 5, not merely by the table being
    //    full and every claim failing for an unrelated reason.
    fetchown_worker_done_rs(t, 5);
    fetchown_release_rs(t, 5);
    if fetchown_orphan_rs(t, 1) != 1 {
        return -10;
    }
    if fetchown_check_rs(t, 1, a) != R_NOSLOT {
        return -11; // the dead owner can no longer reach it
    }
    if fetchown_claim_rs(t, a) != 5 {
        return -12; // must skip the orphaned slot 1 and take the free 5
    }
    if fetchown_worker_done_rs(t, 1) != 1 {
        return -13; // the worker must be told to free the body
    }
    if fetchown_claim_rs(t, a) != 1 {
        return -14; // and now the slot is back in the pool
    }
    // 5: release on a slot whose worker already finished frees it at once, and
    //    worker_done on a normally-released slot does NOT ask for a free (that
    //    would be the double free the ORPHAN bit exists to prevent).
    fetchown_release_rs(t, 2);
    if fetchown_worker_done_rs(t, 2) != 0 {
        return -15;
    }
    if fetchown_claim_rs(t, b) != 2 {
        return -16;
    }

    for i in 0..SLOTS[TAB_TEST] {
        TABLES[TAB_TEST][i as usize].store(0, Ordering::Release);
    }
    let _ = TAB_FETCH;
    let _ = TAB_POST;
    0
}
