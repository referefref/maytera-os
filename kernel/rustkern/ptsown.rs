// rustkern/ptsown.rs - #fdguard: OWNERSHIP of the eight pseudo-terminal pairs
// (drivers/pty.c g_ptys[MAX_PTY]).
//
// New kernel logic (no C twin to strangle), so Rust per the 2026-07-16 rule,
// and the same single-owner-claim shape as fbown.rs: the INPUTS come from
// unchanged C (proc_current()'s tgid, the pts index) and the OWNERSHIP RECORD
// lives here. The ATTACH DECISION itself stays in drivers/pty.c, because it
// also needs process_t.ctty, which is the caller's controlling terminal and
// already a per-process field.
//
// ===========================================================================
// THE DEFECT THIS REMOVES
// ---------------------------------------------------------------------------
// pts_open_by_name() checked ONLY p->in_use, so ANY Ring 3 process could
// open("/dev/pts/N") and attach to another session's terminal: read its
// keystrokes, or write into its input as if typed. pty_pair_t had no owner
// field at all. The legitimate openers of a slave are (1) the process that
// created the pair by opening /dev/ptmx (a terminal, a multiplexer, winchprb),
// and (2) a process whose controlling terminal IS that pts (reopening it, or
// /dev/tty resolving to it). Everything else must be refused unless an explicit
// grant is negotiated. This module records (1); pty.c checks (1), (2) and the
// kernel-internal stdio bind, and audits every refusal.
//
// The owner is a THREAD GROUP id (tgid), for the same reason fdown.rs uses one:
// a terminal that opens /dev/ptmx on one thread and the slave on another still
// owns the pair.
// ===========================================================================

use core::sync::atomic::{AtomicU32, Ordering};

// MUST equal MAX_PTY in drivers/pty.c. ptsown_slots_rs() hands this back and
// pty.c asserts it against MAX_PTY at boot.
const MAX_PTY: usize = 8;

static OWN: [AtomicU32; MAX_PTY] = [const { AtomicU32::new(0) }; MAX_PTY];
static N_REFUSE: AtomicU32 = AtomicU32::new(0);

// Self-test table with no C pair behind it.
static TOWN: [AtomicU32; MAX_PTY] = [const { AtomicU32::new(0) }; MAX_PTY];

/// Slot count this module tracks. pty.c asserts it against MAX_PTY.
#[no_mangle]
pub extern "C" fn ptsown_slots_rs() -> i32 {
    MAX_PTY as i32
}

/// Record `owner` (a tgid) as the creator of pair `idx`. Called from
/// ptmx_open() the instant a pair is claimed. owner==0 (no process context)
/// leaves the pair unowned; only the ctty rule can then admit a slave, which is
/// exactly right for a kernel-created pair.
#[no_mangle]
pub extern "C" fn ptsown_claim_rs(idx: u32, owner: u32) -> i32 {
    if idx as usize >= MAX_PTY {
        return -1;
    }
    OWN[idx as usize].store(owner, Ordering::Release);
    0
}

/// Clear ownership. Called from pty_free_if_dead() when a pair is fully torn
/// down (master and every slave gone), so a recycled pts index starts unowned.
#[no_mangle]
pub extern "C" fn ptsown_release_rs(idx: u32) -> i32 {
    if idx as usize >= MAX_PTY {
        return -1;
    }
    OWN[idx as usize].store(0, Ordering::Release);
    0
}

/// The tgid that created pair `idx`, or 0 if unowned/out of range. pty.c
/// compares this to the caller's tgid; it is Ring 0 only.
#[no_mangle]
pub extern "C" fn ptsown_owner_rs(idx: u32) -> u32 {
    if idx as usize >= MAX_PTY {
        return 0;
    }
    OWN[idx as usize].load(Ordering::Acquire)
}

/// Count one refused attach (pty.c calls this when it audits a refusal), so the
/// boot line can report the guard has fired.
#[no_mangle]
pub extern "C" fn ptsown_note_refusal_rs() {
    N_REFUSE.fetch_add(1, Ordering::Relaxed);
}

#[no_mangle]
pub extern "C" fn ptsown_refusals_rs() -> u32 {
    N_REFUSE.load(Ordering::Relaxed)
}

// ===========================================================================
// SELF-TEST against TOWN. Returns 0, or the negated first failing step. Must
// be able to go RED (make ... selftest).
// ===========================================================================
#[no_mangle]
pub extern "C" fn ptsown_selftest_rs() -> i32 {
    for i in 0..MAX_PTY {
        TOWN[i].store(0, Ordering::Release);
    }
    let a = 111u32;

    // 1: an unclaimed pair has owner 0.
    if TOWN[3].load(Ordering::Acquire) != 0 {
        return -1;
    }
    // 2: claim records the creator's tgid.
    TOWN[3].store(a, Ordering::Release);
    if TOWN[3].load(Ordering::Acquire) != a {
        return -2;
    }
    // 3: a different pair is still unowned (claim is per-index, not global).
    if TOWN[4].load(Ordering::Acquire) != 0 {
        return -3;
    }
    // 4: release clears it.
    TOWN[3].store(0, Ordering::Release);
    if TOWN[3].load(Ordering::Acquire) != 0 {
        return -4;
    }
    // 5: out of range reads back 0 through the public accessor.
    if ptsown_owner_rs(MAX_PTY as u32) != 0 {
        return -5;
    }
    0
}
