// rustkern/advlock.rs - #404 Stage 6: POSIX (fcntl) + BSD (flock) advisory
// file locking MANAGER: the lock table, byte-range overlap math, conflict
// detection, merge/split/replace of same-owner ranges, and a simple wait-for
// cycle (deadlock) check.
//
// NEW kernel logic (there was no advisory locking of any kind before, see the
// pre-change userland sys/file.h note), so Rust per the 2026-07-16 standing
// rule, and the same shape as fdown.rs / fetchown.rs / dnstx.rs: the INPUTS
// come from unchanged C (proc_current()'s thread-group id, the fd->path the
// open layer already resolved, the byte range the caller passed) and the pure
// DECISION lives here.
//
// ===========================================================================
// WHY THE PURE LOGIC IS HERE AND THE PLUMBING IS IN proc/advlock.c
// ---------------------------------------------------------------------------
// Everything in this file is a decision over an in-memory table: given the
// current set of locks, does a proposed lock conflict, and if not, what does
// the table look like after it is applied (with the POSIX merge/split rules).
// That is exactly the kind of self-contained, heavily-branchy logic that Rust
// makes safe: no raw pointer walks off the end of the fixed table, and the
// range arithmetic (u64 with u64::MAX = EOF) cannot silently wrap into a bug.
//
// The C glue (proc/advlock.c) owns the parts genuinely entangled with the C
// kernel and deliberately NOT reimplemented here: copy_from_user of struct
// flock, fd -> path -> file_id resolution, l_whence resolution against the
// file position/size, the fcntl/flock syscall dispatch, the close/exit release
// hooks, and the wait_event_interruptible sleep for F_SETLKW.
//
// ===========================================================================
// CONCURRENCY CONTRACT (read before touching any function below)
// ---------------------------------------------------------------------------
// The lock table and the wait-for edge table are plain (non-atomic) memory
// behind an UnsafeCell. EVERY #[no_mangle] entry point in this file assumes
// THE CALLER HOLDS the single global C spinlock g_advlock_lock (proc/advlock.c)
// across the whole call. That is what makes the &mut borrow of the table sound:
// no two CPUs are ever inside this module at once. The one exception is
// advlock_fileid_rs(), which touches no shared state and needs no lock.
//
// This mirrors how fdlayer.c already serialises its shared legacy fd table with
// g_legacy_fd_lock; it is the established pattern in this tree for a shared
// kernel table that the pure-logic layer must mutate.
//
// Build (pinned): rustc 1.97.0, target x86_64-unknown-none, -C panic=abort,
// soft-float. Pure integer logic; no float, no alloc, no I/O, no printing.
// ===========================================================================

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicU32, Ordering};

// Lock types. These MUST match libc fcntl.h F_RDLCK/F_WRLCK/F_UNLCK, which the
// C glue passes straight through from the userland struct flock.
const RDLCK: u8 = 0; // F_RDLCK: shared
const WRLCK: u8 = 1; // F_WRLCK: exclusive
const UNLCK: u8 = 2; // F_UNLCK

// Result codes returned to the C glue. Kept distinct from errno: the glue maps
// WOULDBLOCK to EAGAIN (F_SETLK) or to a sleep (F_SETLKW), DEADLK to EDEADLK,
// NOSPACE to ENOLCK, BADARG to EINVAL.
const ADV_GRANTED: i32 = 0;
const ADV_WOULDBLOCK: i32 = 1;
const ADV_DEADLK: i32 = 2;
const ADV_NOSPACE: i32 = 3;
const ADV_BADARG: i32 = 4;

// Table sizes. Fixed static storage (no alloc in-kernel). 256 concurrent lock
// records and 128 wait-for edges are far beyond what a handful of lock-using
// apps (moon-buggy, Angband) will ever hold at once; the boot self-test proves
// the NOSPACE path exists rather than pretending the table cannot fill.
const MAX_LOCKS: usize = 256;
const MAX_EDGES: usize = 128;

#[derive(Clone, Copy)]
struct Lk {
    used: bool,
    is_flock: bool, // true = BSD flock (whole-file, per fd slot); false = POSIX
    ltype: u8,      // RDLCK or WRLCK (UNLCK is never STORED, only applied)
    file_id: u64,   // FNV-1a of the canonical path (advlock_fileid_rs)
    owner: u32,     // owning thread-group id (tgid); the POSIX lock owner
    fd_slot: i32,   // flock: the owning legacy fd slot; POSIX: -1
    start: u64,     // inclusive byte offset
    end: u64,       // EXCLUSIVE end; u64::MAX = to EOF ("l_len == 0")
}

const LK_EMPTY: Lk = Lk {
    used: false,
    is_flock: false,
    ltype: 0,
    file_id: 0,
    owner: 0,
    fd_slot: -1,
    start: 0,
    end: 0,
};

#[derive(Clone, Copy)]
struct Edge {
    used: bool,
    waiter: u32,  // tgid blocked
    blocker: u32, // tgid it is blocked behind
}
const EDGE_EMPTY: Edge = Edge { used: false, waiter: 0, blocker: 0 };

struct Table {
    locks: UnsafeCell<[Lk; MAX_LOCKS]>,
    edges: UnsafeCell<[Edge; MAX_EDGES]>,
}
// SAFETY: every access is serialised by the C-held g_advlock_lock (see the
// CONCURRENCY CONTRACT above). No entry point takes two live &mut borrows of
// either array: each FFI function borrows once and passes the slice down.
unsafe impl Sync for Table {}

static TBL: Table = Table {
    locks: UnsafeCell::new([LK_EMPTY; MAX_LOCKS]),
    edges: UnsafeCell::new([EDGE_EMPTY; MAX_EDGES]),
};

// Observability counters (like fdown's N_REFUSE): a guard nobody has watched
// fire is indistinguishable from one switched off. Read back by the boot line
// and by advlock_stat_rs for the on-VM test.
static N_GRANT: AtomicU32 = AtomicU32::new(0);
static N_CONFLICT: AtomicU32 = AtomicU32::new(0);
static N_DEADLK: AtomicU32 = AtomicU32::new(0);
static N_UNLOCK: AtomicU32 = AtomicU32::new(0);

// ---- pure range helpers -----------------------------------------------------

// Two half-open ranges [s1,e1) and [s2,e2) overlap iff each starts before the
// other ends. u64::MAX as an end behaves as +infinity, which is exactly the
// "to EOF" semantics of l_len == 0.
#[inline]
fn ranges_overlap(s1: u64, e1: u64, s2: u64, e2: u64) -> bool {
    s1 < e2 && s2 < e1
}

// A well-formed range for a real lock has start < end. start == end is an empty
// range (no-op). start > end is malformed.
#[inline]
fn range_ok(s: u64, e: u64) -> bool {
    s <= e
}

// ---- table primitives (operate on the borrowed slice) -----------------------

fn add_raw(
    lk: &mut [Lk; MAX_LOCKS],
    file_id: u64,
    owner: u32,
    ltype: u8,
    start: u64,
    end: u64,
    is_flock: bool,
    fd_slot: i32,
) -> Option<usize> {
    for i in 0..MAX_LOCKS {
        if !lk[i].used {
            lk[i] = Lk { used: true, is_flock, ltype, file_id, owner, fd_slot, start, end };
            return Some(i);
        }
    }
    None
}

// The first POSIX lock (different owner) that conflicts with a proposed
// (ltype over [start,end)) on file_id, or None. Read/read never conflicts; a
// write on either side does. Same-owner locks never conflict (POSIX).
fn posix_conflict(
    lk: &[Lk; MAX_LOCKS],
    file_id: u64,
    owner: u32,
    ltype: u8,
    start: u64,
    end: u64,
) -> Option<usize> {
    for i in 0..MAX_LOCKS {
        let l = lk[i];
        if !l.used || l.is_flock || l.file_id != file_id || l.owner == owner {
            continue;
        }
        if !ranges_overlap(start, end, l.start, l.end) {
            continue;
        }
        if ltype == WRLCK || l.ltype == WRLCK {
            return Some(i);
        }
    }
    None
}

// Remove same-owner POSIX coverage of [start,end) on file_id, splitting any
// straddling lock into its left/right remainders (which keep the OLD type).
// Returns true if anything changed. The remainders are outside [start,end) by
// construction, so re-adding them mid-loop cannot re-match this range.
fn posix_clear_owner_range(
    lk: &mut [Lk; MAX_LOCKS],
    file_id: u64,
    owner: u32,
    start: u64,
    end: u64,
) -> bool {
    let mut changed = false;
    for i in 0..MAX_LOCKS {
        let l = lk[i];
        if !l.used || l.is_flock || l.file_id != file_id || l.owner != owner {
            continue;
        }
        if !ranges_overlap(start, end, l.start, l.end) {
            continue;
        }
        let (ls, le, lt) = (l.start, l.end, l.ltype);
        lk[i].used = false;
        changed = true;
        if ls < start {
            // left remainder [ls, start)
            let _ = add_raw(lk, file_id, owner, lt, ls, start, false, -1);
        }
        if le > end {
            // right remainder [end, le)
            let _ = add_raw(lk, file_id, owner, lt, end, le, false, -1);
        }
    }
    changed
}

// Coalesce same-owner, same-type, same-file POSIX locks that touch or overlap.
// After a clear+add the new lock cannot overlap another same-owner lock, but it
// may be exactly adjacent (a.end == b.start) to one of the same type; merge so
// the table does not grow without bound under repeated adjacent locking.
fn coalesce_posix(lk: &mut [Lk; MAX_LOCKS], file_id: u64, owner: u32, ltype: u8) {
    loop {
        let mut merged = false;
        'outer: for i in 0..MAX_LOCKS {
            let a = lk[i];
            if !a.used || a.is_flock || a.file_id != file_id || a.owner != owner || a.ltype != ltype {
                continue;
            }
            for j in (i + 1)..MAX_LOCKS {
                let b = lk[j];
                if !b.used || b.is_flock || b.file_id != file_id || b.owner != owner || b.ltype != ltype {
                    continue;
                }
                // Touch or overlap (exclusive ends: adjacent when a.end==b.start).
                if a.start <= b.end && b.start <= a.end {
                    lk[i].start = a.start.min(b.start);
                    lk[i].end = a.end.max(b.end);
                    lk[j].used = false;
                    merged = true;
                    break 'outer;
                }
            }
        }
        if !merged {
            break;
        }
    }
}

// Apply a POSIX set of (ltype over [start,end)) for owner: clear the range of
// this owner's old coverage, add the new lock, coalesce. Grant is assumed
// (conflict was checked by the caller). start==end is a no-op grant.
fn posix_apply(
    lk: &mut [Lk; MAX_LOCKS],
    file_id: u64,
    owner: u32,
    ltype: u8,
    start: u64,
    end: u64,
) -> i32 {
    if start >= end {
        return ADV_GRANTED;
    }
    posix_clear_owner_range(lk, file_id, owner, start, end);
    if add_raw(lk, file_id, owner, ltype, start, end, false, -1).is_none() {
        return ADV_NOSPACE;
    }
    coalesce_posix(lk, file_id, owner, ltype);
    N_GRANT.fetch_add(1, Ordering::Relaxed);
    ADV_GRANTED
}

// ---- wait-for edge helpers --------------------------------------------------

fn clear_edges_waiter(eg: &mut [Edge; MAX_EDGES], waiter: u32) {
    for e in eg.iter_mut() {
        if e.used && e.waiter == waiter {
            e.used = false;
        }
    }
}

fn add_edge(eg: &mut [Edge; MAX_EDGES], waiter: u32, blocker: u32) {
    // de-dup
    for e in eg.iter() {
        if e.used && e.waiter == waiter && e.blocker == blocker {
            return;
        }
    }
    for e in eg.iter_mut() {
        if !e.used {
            *e = Edge { used: true, waiter, blocker };
            return;
        }
    }
    // Edge table full: the graph is incomplete, so a deadlock might not be
    // detected, but F_SETLKW never hangs unbounded (a real unlock still wakes
    // it). Silently degrade rather than falsely report EDEADLK.
}

// Can `from` reach `target` by following wait-for edges? Iterative DFS over the
// fixed edge table with a bounded visited set. Used to detect that granting a
// new wait edge would close a cycle (a deadlock).
fn reachable(eg: &[Edge; MAX_EDGES], from: u32, target: u32) -> bool {
    if from == target {
        return true;
    }
    let mut stack = [0u32; MAX_EDGES];
    let mut sp = 0usize;
    let mut visited = [0u32; MAX_EDGES];
    let mut vn = 0usize;
    stack[sp] = from;
    sp += 1;
    while sp > 0 {
        sp -= 1;
        let cur = stack[sp];
        let mut seen = false;
        for k in 0..vn {
            if visited[k] == cur {
                seen = true;
                break;
            }
        }
        if seen {
            continue;
        }
        if vn < MAX_EDGES {
            visited[vn] = cur;
            vn += 1;
        }
        for e in eg.iter() {
            if e.used && e.waiter == cur {
                if e.blocker == target {
                    return true;
                }
                if sp < MAX_EDGES {
                    stack[sp] = e.blocker;
                    sp += 1;
                }
            }
        }
    }
    false
}

// ===========================================================================
// C-FACING FFI. Unless noted, THE CALLER HOLDS g_advlock_lock.
// ===========================================================================

/// FNV-1a 64-bit hash of the canonical path. This is the file identity POSIX
/// locks are keyed on. Two opens of the SAME resolved path (by any process)
/// produce the same id and therefore share a lock space; that is the
/// cross-process semantics advisory locking needs. Touches no shared state, so
/// it needs no lock. 0 is reserved for "no file" and never returned.
#[no_mangle]
pub extern "C" fn advlock_fileid_rs(path: *const u8, len: usize) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    if !path.is_null() {
        for i in 0..len {
            // SAFETY: caller passes a valid kernel buffer of `len` bytes (the
            // fd's stored path); we read only within [0,len).
            let b = unsafe { *path.add(i) };
            h ^= b as u64;
            h = h.wrapping_mul(0x0000_0100_0000_01b3);
        }
    }
    if h == 0 {
        1
    } else {
        h
    }
}

/// F_SETLK (non-blocking POSIX set). RDLCK/WRLCK only. Returns ADV_GRANTED
/// (applied) or ADV_WOULDBLOCK (a conflicting lock exists; C maps to EAGAIN),
/// or ADV_BADARG.
#[no_mangle]
pub extern "C" fn advlock_try_rs(file_id: u64, owner: u32, ltype: i32, start: u64, end: u64) -> i32 {
    let lt = ltype as u8;
    if !range_ok(start, end) || (lt != RDLCK && lt != WRLCK) {
        return ADV_BADARG;
    }
    // SAFETY: caller holds g_advlock_lock; single &mut borrow.
    let lk = unsafe { &mut *TBL.locks.get() };
    if posix_conflict(lk, file_id, owner, lt, start, end).is_some() {
        N_CONFLICT.fetch_add(1, Ordering::Relaxed);
        return ADV_WOULDBLOCK;
    }
    posix_apply(lk, file_id, owner, lt, start, end)
}

/// One attempt of F_SETLKW. Clears this waiter's stale edges, then: grants if
/// possible; else runs the deadlock check and either returns ADV_DEADLK (a
/// cycle would form) or records fresh wait edges and returns ADV_WOULDBLOCK.
/// The C glue then sleeps on the wait queue and calls this again on wake.
#[no_mangle]
pub extern "C" fn advlock_setw_step_rs(
    file_id: u64,
    owner: u32,
    ltype: i32,
    start: u64,
    end: u64,
) -> i32 {
    let lt = ltype as u8;
    if !range_ok(start, end) || (lt != RDLCK && lt != WRLCK) {
        return ADV_BADARG;
    }
    // SAFETY: caller holds g_advlock_lock; the two arrays are distinct.
    let lk = unsafe { &mut *TBL.locks.get() };
    let eg = unsafe { &mut *TBL.edges.get() };

    // Recompute this waiter's edges from scratch each attempt: the set of
    // blockers can change between wakes.
    clear_edges_waiter(eg, owner);

    if posix_conflict(lk, file_id, owner, lt, start, end).is_none() {
        return posix_apply(lk, file_id, owner, lt, start, end);
    }

    // Conflict. First decide deadlock WITHOUT adding our edges: for every
    // distinct blocker B, if B can already reach us, adding owner -> B closes a
    // cycle.
    for i in 0..MAX_LOCKS {
        let l = lk[i];
        if !l.used || l.is_flock || l.file_id != file_id || l.owner == owner {
            continue;
        }
        if !ranges_overlap(start, end, l.start, l.end) {
            continue;
        }
        if !(lt == WRLCK || l.ltype == WRLCK) {
            continue;
        }
        if reachable(eg, l.owner, owner) {
            N_DEADLK.fetch_add(1, Ordering::Relaxed);
            return ADV_DEADLK;
        }
    }
    // No cycle: record owner -> each blocker and report WOULDBLOCK.
    for i in 0..MAX_LOCKS {
        let l = lk[i];
        if !l.used || l.is_flock || l.file_id != file_id || l.owner == owner {
            continue;
        }
        if !ranges_overlap(start, end, l.start, l.end) {
            continue;
        }
        if !(lt == WRLCK || l.ltype == WRLCK) {
            continue;
        }
        add_edge(eg, owner, l.owner);
    }
    N_CONFLICT.fetch_add(1, Ordering::Relaxed);
    ADV_WOULDBLOCK
}

/// wait_event condition for F_SETLKW: 1 if the requested POSIX lock has no
/// conflict right now (grantable), else 0. Caller holds g_advlock_lock.
#[no_mangle]
pub extern "C" fn advlock_grantable_locked_rs(
    file_id: u64,
    owner: u32,
    ltype: i32,
    start: u64,
    end: u64,
) -> i32 {
    let lt = ltype as u8;
    if lt != RDLCK && lt != WRLCK {
        return 1;
    }
    // SAFETY: caller holds g_advlock_lock; read-only borrow.
    let lk = unsafe { &*TBL.locks.get() };
    if posix_conflict(lk, file_id, owner, lt, start, end).is_some() {
        0
    } else {
        1
    }
}

/// Drop this waiter's wait-for edges. Called by the C glue after the F_SETLKW
/// loop ends for ANY reason (granted, EDEADLK, EINTR).
#[no_mangle]
pub extern "C" fn advlock_wait_end_rs(owner: u32) -> i32 {
    // SAFETY: caller holds g_advlock_lock.
    let eg = unsafe { &mut *TBL.edges.get() };
    clear_edges_waiter(eg, owner);
    0
}

/// F_UNLCK over [start,end) for owner. Splits/removes this owner's POSIX
/// coverage. Returns 1 if the table changed (so the C glue wakes waiters), 0
/// otherwise.
#[no_mangle]
pub extern "C" fn advlock_unlock_rs(file_id: u64, owner: u32, start: u64, end: u64) -> i32 {
    if !range_ok(start, end) || start == end {
        return 0;
    }
    // SAFETY: caller holds g_advlock_lock.
    let lk = unsafe { &mut *TBL.locks.get() };
    if posix_clear_owner_range(lk, file_id, owner, start, end) {
        N_UNLOCK.fetch_add(1, Ordering::Relaxed);
        1
    } else {
        0
    }
}

/// F_GETLK: report a lock that WOULD conflict with (ltype over [start,end)).
/// Returns 1 and fills the out params with the conflicting holder, or 0 if the
/// range is free (C then sets l_type=F_UNLCK). out_end is the exclusive end
/// (u64::MAX = EOF); the C glue converts it to l_len.
#[no_mangle]
pub extern "C" fn advlock_getlk_rs(
    file_id: u64,
    owner: u32,
    ltype: i32,
    start: u64,
    end: u64,
    out_type: *mut i32,
    out_start: *mut u64,
    out_end: *mut u64,
    out_pid: *mut u32,
) -> i32 {
    let lt = ltype as u8;
    if !range_ok(start, end) {
        return 0;
    }
    // SAFETY: caller holds g_advlock_lock; read-only borrow.
    let lk = unsafe { &*TBL.locks.get() };
    if let Some(i) = posix_conflict(lk, file_id, owner, lt, start, end) {
        let l = lk[i];
        // SAFETY: the C caller passes valid kernel out pointers or null; we
        // write only through non-null ones.
        unsafe {
            if !out_type.is_null() {
                *out_type = l.ltype as i32;
            }
            if !out_start.is_null() {
                *out_start = l.start;
            }
            if !out_end.is_null() {
                *out_end = l.end;
            }
            if !out_pid.is_null() {
                *out_pid = l.owner;
            }
        }
        1
    } else {
        0
    }
}

/// BSD flock() attempt (whole-file, keyed on the fd slot). `want` is RDLCK
/// (LOCK_SH), WRLCK (LOCK_EX) or UNLCK (LOCK_UN). Returns ADV_GRANTED (applied,
/// replacing this fd slot's previous flock if any) or ADV_WOULDBLOCK. flock
/// records are a SEPARATE lock space from POSIX records (they do not conflict
/// with each other), matching Linux.
#[no_mangle]
pub extern "C" fn advlock_flock_try_rs(file_id: u64, fd_slot: i32, owner: u32, want: i32) -> i32 {
    let w = want as u8;
    // SAFETY: caller holds g_advlock_lock.
    let lk = unsafe { &mut *TBL.locks.get() };
    if w == UNLCK {
        for i in 0..MAX_LOCKS {
            if lk[i].used && lk[i].is_flock && lk[i].fd_slot == fd_slot {
                lk[i].used = false;
            }
        }
        return ADV_GRANTED;
    }
    if w != RDLCK && w != WRLCK {
        return ADV_BADARG;
    }
    for i in 0..MAX_LOCKS {
        let l = lk[i];
        if !l.used || !l.is_flock || l.file_id != file_id || l.fd_slot == fd_slot {
            continue;
        }
        if w == WRLCK || l.ltype == WRLCK {
            N_CONFLICT.fetch_add(1, Ordering::Relaxed);
            return ADV_WOULDBLOCK;
        }
    }
    // Grantable: replace this fd slot's existing flock (upgrade/downgrade).
    for i in 0..MAX_LOCKS {
        if lk[i].used && lk[i].is_flock && lk[i].fd_slot == fd_slot {
            lk[i].used = false;
        }
    }
    if add_raw(lk, file_id, owner, w, 0, u64::MAX, true, fd_slot).is_none() {
        return ADV_NOSPACE;
    }
    N_GRANT.fetch_add(1, Ordering::Relaxed);
    ADV_GRANTED
}

/// wait_event condition for a blocking flock: 1 if grantable now. Caller holds
/// g_advlock_lock.
#[no_mangle]
pub extern "C" fn advlock_flock_grantable_locked_rs(file_id: u64, fd_slot: i32, want: i32) -> i32 {
    let w = want as u8;
    if w != RDLCK && w != WRLCK {
        return 1;
    }
    // SAFETY: caller holds g_advlock_lock; read-only borrow.
    let lk = unsafe { &*TBL.locks.get() };
    for i in 0..MAX_LOCKS {
        let l = lk[i];
        if !l.used || !l.is_flock || l.file_id != file_id || l.fd_slot == fd_slot {
            continue;
        }
        if w == WRLCK || l.ltype == WRLCK {
            return 0;
        }
    }
    1
}

/// Release the flock held on a legacy fd slot (called on close of that slot).
/// Returns the number of records dropped.
#[no_mangle]
pub extern "C" fn advlock_release_fdslot_rs(fd_slot: i32) -> i32 {
    // SAFETY: caller holds g_advlock_lock.
    let lk = unsafe { &mut *TBL.locks.get() };
    let mut n = 0;
    for i in 0..MAX_LOCKS {
        if lk[i].used && lk[i].is_flock && lk[i].fd_slot == fd_slot {
            lk[i].used = false;
            n += 1;
        }
    }
    n
}

/// Release ALL of `owner`'s POSIX locks on one file (called when the process
/// closes ANY fd on that file, per POSIX). Returns the number dropped.
#[no_mangle]
pub extern "C" fn advlock_release_owner_file_rs(file_id: u64, owner: u32) -> i32 {
    // SAFETY: caller holds g_advlock_lock.
    let lk = unsafe { &mut *TBL.locks.get() };
    let mut n = 0;
    for i in 0..MAX_LOCKS {
        let l = lk[i];
        if l.used && !l.is_flock && l.file_id == file_id && l.owner == owner {
            lk[i].used = false;
            n += 1;
        }
    }
    n
}

/// Release EVERY lock (POSIX and flock) held by `owner` and drop every wait-for
/// edge mentioning it (called on process exit). Returns the number of lock
/// records dropped.
#[no_mangle]
pub extern "C" fn advlock_release_owner_all_rs(owner: u32) -> i32 {
    // SAFETY: caller holds g_advlock_lock.
    let lk = unsafe { &mut *TBL.locks.get() };
    let eg = unsafe { &mut *TBL.edges.get() };
    let mut n = 0;
    for i in 0..MAX_LOCKS {
        if lk[i].used && lk[i].owner == owner {
            lk[i].used = false;
            n += 1;
        }
    }
    clear_edges_waiter(eg, owner);
    for e in eg.iter_mut() {
        if e.used && e.blocker == owner {
            e.used = false;
        }
    }
    n
}

/// Read a counter for observability. sel: 0 grants, 1 conflicts, 2 deadlocks,
/// 3 unlocks, 4 = live lock records.
#[no_mangle]
pub extern "C" fn advlock_stat_rs(sel: i32) -> u32 {
    match sel {
        0 => N_GRANT.load(Ordering::Relaxed),
        1 => N_CONFLICT.load(Ordering::Relaxed),
        2 => N_DEADLK.load(Ordering::Relaxed),
        3 => N_UNLOCK.load(Ordering::Relaxed),
        4 => {
            // SAFETY: caller holds g_advlock_lock (the C stat wrapper takes it).
            let lk = unsafe { &*TBL.locks.get() };
            let mut c = 0u32;
            for i in 0..MAX_LOCKS {
                if lk[i].used {
                    c += 1;
                }
            }
            c
        }
        _ => 0,
    }
}

/// Table capacity, asserted against the C _Static_assert-free expectation in
/// the boot check the same way fdown_slots_rs is.
#[no_mangle]
pub extern "C" fn advlock_capacity_rs() -> i32 {
    MAX_LOCKS as i32
}

// ===========================================================================
// SELF-TEST. Runs at boot against the real table (empty at boot). Returns 0 on
// success or the negated number of the first failing step. It exercises the
// arithmetic that is easy to get wrong: overlap, split, replace, shared/shared
// compatibility, and a two-way deadlock cycle. Like fdown's, the deadlock
// property is NEGATIVE, so this must be able to go RED (proven by reasoning
// through each step; the on-VM test then re-proves the live path).
// ===========================================================================
#[no_mangle]
pub extern "C" fn advlock_selftest_rs() -> i32 {
    // SAFETY: boot context, single-threaded, no other CPU is in this module.
    let lk = unsafe { &mut *TBL.locks.get() };
    let eg = unsafe { &mut *TBL.edges.get() };
    for i in 0..MAX_LOCKS {
        lk[i].used = false;
    }
    for e in eg.iter_mut() {
        e.used = false;
    }

    let f: u64 = 0xABCD; // a file id
    let a = 111u32; // owner A
    let b = 222u32; // owner B

    // 1: overlap math.
    if !ranges_overlap(0, 10, 5, 15) {
        return -1;
    }
    if ranges_overlap(0, 10, 10, 20) {
        return -2; // exclusive ends: [0,10) and [10,20) do NOT overlap
    }
    if !ranges_overlap(0, u64::MAX, 1_000_000, 1_000_001) {
        return -3; // EOF end covers everything above start
    }

    // 2: A takes WRLCK [0,10). No conflict for A.
    if posix_conflict(lk, f, a, WRLCK, 0, 10).is_some() {
        return -4;
    }
    if posix_apply(lk, f, a, WRLCK, 0, 10) != ADV_GRANTED {
        return -5;
    }
    // 3: B's WRLCK [5,15) conflicts; B's RDLCK [5,15) conflicts too (A holds W).
    if posix_conflict(lk, f, b, WRLCK, 5, 15).is_none() {
        return -6;
    }
    if posix_conflict(lk, f, b, RDLCK, 5, 15).is_none() {
        return -7;
    }
    // 4: B's WRLCK [10,20) does NOT conflict (adjacent, no overlap).
    if posix_conflict(lk, f, b, WRLCK, 10, 20).is_some() {
        return -8;
    }

    // 5: split. A unlocks the middle [3,6) of its [0,10). Must leave [0,3) and
    //    [6,10), i.e. B's WRLCK [3,6) now free but [0,3) still conflicts.
    if !posix_clear_owner_range(lk, f, a, 3, 6) {
        return -9;
    }
    if posix_conflict(lk, f, b, WRLCK, 3, 6).is_some() {
        return -10; // hole is free
    }
    if posix_conflict(lk, f, b, WRLCK, 0, 3).is_none() {
        return -11; // left remnant still held
    }
    if posix_conflict(lk, f, b, WRLCK, 6, 10).is_none() {
        return -12; // right remnant still held
    }

    // 6: replace. A sets RDLCK over its whole [0,10). The two WRLCK remnants
    //    become one RDLCK [0,10); now B's RDLCK [0,10) is COMPATIBLE (shared),
    //    but B's WRLCK [0,10) still conflicts.
    if posix_apply(lk, f, a, RDLCK, 0, 10) != ADV_GRANTED {
        return -13;
    }
    if posix_conflict(lk, f, b, RDLCK, 0, 10).is_some() {
        return -14; // shared + shared OK
    }
    if posix_conflict(lk, f, b, WRLCK, 0, 10).is_none() {
        return -15; // shared blocks a writer
    }
    // 7: coalesce actually merged: exactly ONE live record for A on f.
    {
        let mut cnt = 0;
        for i in 0..MAX_LOCKS {
            if lk[i].used && lk[i].file_id == f && lk[i].owner == a {
                cnt += 1;
            }
        }
        if cnt != 1 {
            return -16;
        }
    }

    // 8: deadlock cycle. Model: A holds R1, waits on R2 (held by B): edge A->B.
    //    B now wants R1 (held by A): reachable(A, B)? A->B exists, so yes.
    for e in eg.iter_mut() {
        e.used = false;
    }
    add_edge(eg, a, b); // A waits behind B
    if !reachable(eg, a, b) {
        return -17;
    }
    if reachable(eg, b, a) {
        return -18; // no edge B->...->A yet, so no cycle from B alone
    }
    // The deadlock rule: B about to wait behind A closes the cycle iff
    // reachable(A, B) (A already waits behind B). It does.
    if !reachable(eg, a, b) {
        return -19;
    }
    // 9: a NON-deadlock: a third owner C waiting behind A is fine (no path back).
    let c = 333u32;
    if reachable(eg, a, c) {
        return -20;
    }

    // Clean up so the boot self-test leaves the live table pristine.
    for i in 0..MAX_LOCKS {
        lk[i].used = false;
    }
    for e in eg.iter_mut() {
        e.used = false;
    }
    N_GRANT.store(0, Ordering::Relaxed);
    N_CONFLICT.store(0, Ordering::Relaxed);
    N_DEADLK.store(0, Ordering::Relaxed);
    N_UNLOCK.store(0, Ordering::Relaxed);
    0
}
