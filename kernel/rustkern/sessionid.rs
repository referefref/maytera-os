// rustkern/sessionid.rs - #745 Stage 2 session identity policy: who the desktop
// session IS, whether it may lock itself, and which uid a new account gets.
//
// New kernel logic (there is no C twin to strangle), so Rust per the
// 2026-07-16 rule, in the same shape as spawnid.rs: this module holds the
// DECISIONS, and calls no C at all. Every input is passed in by the C glue that
// already owns the tables (users.c shadow/user tables, the LOGIN.CFG parse in
// proc/syscall.c). That keeps the policy testable in isolation, which is what
// sessionid_selftest_rs() does at boot.
//
// ===========================================================================
// WHY THE LOCK POLICY LIVES HERE
// ---------------------------------------------------------------------------
// Two separate faults made the lock screen the showstopper for the non-root
// desktop flip, and they pull in opposite directions:
//
//   1. sys_session_lock() silently no-opped for ANY autologin session. The lock
//      path was therefore never exercised, so nothing downstream of it was ever
//      tested. Locking is the mechanism by which a user protects a machine they
//      walk away from; refusing an EXPLICIT lock request is not a safety
//      feature, it is a missing feature.
//
//   2. The compositor sent a hardcoded "root" to the unlock syscall. Post-flip
//      the session is uid 1000, the names do not match, and the session cannot
//      be unlocked except by rebooting.
//
// Fixing (1) alone would have made (2) reachable and bricked sessions for real.
// So the policy here refuses to create an unlockable session BY CONSTRUCTION:
//
//   A SESSION WHOSE USER CANNOT AUTHENTICATE IS NEVER LOCKED.
//
// That is not a workaround for the "root" bug (which is fixed separately, at
// the source: the kernel now resolves the session user itself and the caller
// does not supply a name at all). It is the standing invariant that makes the
// whole class impossible, including the cases nobody has written yet:
//
//   - the `ref` account shipped at uid 1002 with NO shadow record at all,
//     because Settings' Add User discarded the password (fixed in this same
//     change, but the invariant must not depend on that fix being correct);
//   - any account whose hash is "*" (explicit no-login);
//   - a session established before the account tables loaded.
//
// A session that cannot lock is an inconvenience with a visible message. A
// session that locks and cannot unlock costs the user everything they had open
// and is only recoverable by power-cycling the machine. The asymmetry decides
// the default.
// ===========================================================================

// Lock request reasons. MUST match the SESSION_LOCK_* defines in
// proc/syscall.h and libc/syscall.h.
//
// 0 IS DELIBERATELY "IDLE", not "explicit". A stale userland binary that calls
// SYS_SESSION_LOCK with no argument lands on 0 with whatever register garbage
// the ABI left, so 0 has to be the CONSERVATIVE value: idle is the reason that
// autologin declines, which is exactly today's behaviour. A stale caller
// therefore keeps the semantics it was compiled against, and only a caller that
// deliberately passes EXPLICIT gets the new one. Any unknown reason is also
// treated as idle, for the same reason.
const REASON_IDLE: u32 = 0;
const REASON_EXPLICIT: u32 = 1;

// Lock decisions, returned to C.
pub const LOCK_ALLOW: u32 = 0;
pub const LOCK_DENY_NO_CREDENTIAL: u32 = 1; // session user cannot authenticate
pub const LOCK_DENY_AUTOLOGIN_IDLE: u32 = 2; // autologin box, idle timer fired

/// Decide whether a session lock request is honoured.
///
/// `reason`             one of REASON_*; anything unknown is treated as idle.
/// `autologin_active`   1 when boot autologin is configured FOR THIS session's
///                      user (proc/syscall.c session_autologin_active()).
/// `session_can_auth`   1 when the session user exists AND holds a usable
///                      credential (users_can_authenticate() in proc/users.c).
///
/// Returns LOCK_ALLOW, or one of the LOCK_DENY_* reasons. The caller logs the
/// reason; a denial is never silent, which is the other half of fault (1).
#[no_mangle]
pub extern "C" fn session_lock_decide_rs(
    reason: u32,
    autologin_active: u32,
    session_can_auth: u32,
) -> u32 {
    // The invariant, checked FIRST and applying to every reason including an
    // explicit user request. There is no override: an explicit lock of a
    // session that cannot unlock is precisely the brick this exists to stop.
    if session_can_auth == 0 {
        return LOCK_DENY_NO_CREDENTIAL;
    }
    // An explicit request from a user who CAN unlock is always honoured, on an
    // autologin box as much as anywhere else (macOS behaves the same way: Lock
    // Screen works even when the box logs itself in at boot).
    if reason == REASON_EXPLICIT {
        return LOCK_ALLOW;
    }
    // Idle, or an unknown/stale reason treated as idle: an autologin session
    // must not lock ITSELF out on a timer the user never asked for (#566).
    if autologin_active != 0 {
        return LOCK_DENY_AUTOLOGIN_IDLE;
    }
    LOCK_ALLOW
}

// ===========================================================================
// ACCOUNT UID POLICY
// ---------------------------------------------------------------------------
// users_create_first_admin() minted the first interactive account at uid 0, so
// a fresh install could not produce a non-root desktop no matter what anything
// downstream did. That is the root cause the flip is blocked on, and it is a
// POLICY decision (which uid does a human get?) sitting in the middle of
// account-creation plumbing, so it belongs here.
//
// The first human account now gets FIRST_ADMIN_UID. A separate `root` account
// still exists at uid 0 and still owns the system files; the difference is that
// nobody is SITTING IN IT. Reversing this decision is a one-line change to the
// constant below, which is deliberate: it is the single switch the flip turns.
// ===========================================================================

/// uid/gid granted to the first interactive account created at first boot.
/// 1000 is the conventional first-human uid and matches the `admin` account the
/// internal asset base already carries, so the two agree.
pub const FIRST_ADMIN_UID: u32 = 1000;

/// Lowest uid handed to a human account, and the first uid this policy will
/// allocate. Below it is reserved for root and system accounts.
const UID_MIN: u32 = 1000;
/// One past the highest allocatable uid. 60000 matches the usual convention and
/// leaves the top of the range for nobody/overflow sentinels.
const UID_MAX_EXCL: u32 = 60000;
/// Returned when no uid is free.
pub const UID_NONE: u32 = 0xFFFF_FFFF;

#[no_mangle]
pub extern "C" fn first_admin_uid_rs() -> u32 {
    FIRST_ADMIN_UID
}

/// Allocate the next free human uid given the uids already in use.
///
/// Settings' Add User computed `1000 + user_count`, which is not an allocator:
/// with root(0), admin(1000) and ref(1002) present it happens to return 1003,
/// but DELETING admin drops the count to 2 and it returns 1002, which collides
/// with ref, and user_create() refuses. The user sees "Failed to add user" with
/// no way to proceed. Counting is not allocating; this scans.
///
/// `uids` points to `n` uids currently in use (any order, may include system
/// uids, which are ignored). Returns the lowest free uid >= UID_MIN, or
/// UID_NONE if the range is exhausted.
///
/// Lowest-free rather than highest-plus-one so that deleting an account makes
/// its uid available again. That is a deliberate trade: it means a NEW account
/// can inherit a deleted account's uid, and therefore its ownership of any file
/// left behind. The caller is responsible for not leaving orphaned files, which
/// is the same contract every UNIX has. Highest-plus-one only postpones the
/// problem to uid wrap and silently exhausts the range on churn.
#[no_mangle]
pub unsafe extern "C" fn next_user_uid_rs(uids: *const u32, n: u32) -> u32 {
    let mut candidate = UID_MIN;
    while candidate < UID_MAX_EXCL {
        let mut taken = false;
        if !uids.is_null() {
            let mut i: u32 = 0;
            while i < n {
                // SAFETY: caller passes a readable array of `n` u32. Bounded by
                // n on every iteration; no arithmetic on the pointer beyond it.
                if unsafe { *uids.add(i as usize) } == candidate {
                    taken = true;
                    break;
                }
                i += 1;
            }
        }
        if !taken {
            return candidate;
        }
        candidate += 1;
    }
    UID_NONE
}

// ===========================================================================
// SELF-TEST
// ---------------------------------------------------------------------------
// Run once at boot so the policy is proven LIVE on this build rather than
// merely compiled in (same discipline as spawnid_selftest_rs). Returns a bit
// mask of failures; 0 is a pass and anything else is loud.
// ===========================================================================
#[no_mangle]
pub unsafe extern "C" fn sessionid_selftest_rs() -> u32 {
    let mut fails: u32 = 0;

    // 1. No credential denies EVERY reason, including an explicit request, and
    //    including when autologin is off. This is the brick-prevention
    //    invariant and it is checked first because it overrides everything.
    if session_lock_decide_rs(REASON_EXPLICIT, 0, 0) != LOCK_DENY_NO_CREDENTIAL {
        fails |= 1 << 0;
    }
    if session_lock_decide_rs(REASON_IDLE, 0, 0) != LOCK_DENY_NO_CREDENTIAL {
        fails |= 1 << 1;
    }
    if session_lock_decide_rs(REASON_EXPLICIT, 1, 0) != LOCK_DENY_NO_CREDENTIAL {
        fails |= 1 << 2;
    }

    // 2. Explicit lock is honoured on an autologin box (the fault this change
    //    fixes: it used to be silently declined).
    if session_lock_decide_rs(REASON_EXPLICIT, 1, 1) != LOCK_ALLOW {
        fails |= 1 << 3;
    }

    // 3. Idle lock is still declined on an autologin box (#566 preserved).
    if session_lock_decide_rs(REASON_IDLE, 1, 1) != LOCK_DENY_AUTOLOGIN_IDLE {
        fails |= 1 << 4;
    }

    // 4. Idle lock is honoured on a normal, non-autologin session.
    if session_lock_decide_rs(REASON_IDLE, 0, 1) != LOCK_ALLOW {
        fails |= 1 << 5;
    }

    // 5. An unknown/stale reason code behaves as IDLE, not as EXPLICIT.
    if session_lock_decide_rs(99, 1, 1) != LOCK_DENY_AUTOLOGIN_IDLE {
        fails |= 1 << 6;
    }

    // 6. uid allocation: empty table gives UID_MIN.
    if unsafe { next_user_uid_rs(core::ptr::null(), 0) } != UID_MIN {
        fails |= 1 << 7;
    }

    // 7. The exact shipped table (root 0, admin 1000, ref 1002) must give 1001,
    //    NOT 1003. The count-based version returned 1003 here; both are free, so
    //    this asserts the ALLOCATOR ran, not merely that it avoided a collision.
    {
        let t: [u32; 3] = [0, 1000, 1002];
        if unsafe { next_user_uid_rs(t.as_ptr(), 3) } != 1001 {
            fails |= 1 << 8;
        }
    }

    // 8. THE REGRESSION THAT MOTIVATED THIS: root(0), ref(1002), admin deleted.
    //    Count-based gives 1000+2 = 1002, which COLLIDES with ref and makes Add
    //    User fail outright. The allocator must give 1000.
    {
        let t: [u32; 2] = [0, 1002];
        if unsafe { next_user_uid_rs(t.as_ptr(), 2) } != 1000 {
            fails |= 1 << 9;
        }
    }

    // 9. A dense run allocates past it rather than colliding.
    {
        let t: [u32; 4] = [1000, 1001, 1002, 0];
        if unsafe { next_user_uid_rs(t.as_ptr(), 4) } != 1003 {
            fails |= 1 << 10;
        }
    }

    // 10. System uids in the table never make a human uid look taken.
    {
        let t: [u32; 3] = [0, 1, 999];
        if unsafe { next_user_uid_rs(t.as_ptr(), 3) } != 1000 {
            fails |= 1 << 11;
        }
    }

    // 11. The first admin is NOT uid 0. This is the whole root cause in one
    //     assertion: if someone sets FIRST_ADMIN_UID back to 0 to "fix" a
    //     downstream breakage, the boot goes loud instead of quietly
    //     reintroducing a root desktop.
    if first_admin_uid_rs() == 0 {
        fails |= 1 << 12;
    }
    if first_admin_uid_rs() < UID_MIN {
        fails |= 1 << 13;
    }

    fails
}
