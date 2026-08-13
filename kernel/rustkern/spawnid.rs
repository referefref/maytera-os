// rustkern/spawnid.rs - #692 spawn identity policy: which uid/gid a new
// Ring-3 process runs as.
//
// New kernel logic (there is no C twin to strangle), so Rust per the
// 2026-07-16 rule. It calls into EXISTING, unchanged C for its inputs
// (proc_current()'s credentials, desktop_get_session_uid(), the /CONFIG/PASSWD
// table via user_lookup_uid) rather than reimplementing any of it; the DECISION
// is what lives here.
//
// ===========================================================================
// WHY THIS EXISTS
// ---------------------------------------------------------------------------
// proc_create_user() used to give the child whatever uid/gid init_proc() had
// copied from proc_current(). At three of the eight user-spawn sites the
// "parent" is a KERNEL THREAD, whose uid is 0 by definition:
//
//   proc/cron.c            the cron worker fires a user-created job
//   apps/nethack_launcher.c
//   apps/browser_launcher.c
//
// While the desktop autologs in as root that grants nothing, because there is
// no boundary to cross. The moment the session is not root, a USER-CREATED CRON
// JOB RUNS AS ROOT: the session flip does not merely fail to prevent that, it
// CREATES it, by being the thing that makes root mean something.
//
// The fix is not to patch those three sites. It is to make "spawn without an
// explicit identity" impossible to express: proc_create_user() no longer
// exists, proc_create_user_as() takes a mandatory proc_ident_t, and a forgotten
// or zero-initialised identity is KIND_INVALID, which this module REFUSES.
// Refusing is the whole point: a spawn whose identity nobody chose must fail
// loudly, never fall back to root.
//
// COHERENCE. The old proc/services.c stamped uid and euid but left gid
// inherited, producing the incoherent pair uid 0 / gid 1000. There is now ONE
// rule for gid, gid_for_uid() below, and every kind except CALLER goes through
// it. CALLER is the exception on purpose: a live Ring-3 caller's (euid, egid)
// pair is already coherent, because it came either from this same resolver or
// from a checked setgid, and copying it verbatim is what exec(2) means.
// ===========================================================================

// Kind discriminants. MUST match the PROC_AS_* defines in proc/process.h,
// which a _Static_assert there locks to this layout.
//
// 0 IS DELIBERATELY INVALID. A zero-initialised proc_ident_t, a memset
// structure, or a caller who forgot to fill the field therefore gets a
// REFUSAL, not uid 0. That is the by-construction half of this fix; the
// mandatory parameter is the other half.
const KIND_INVALID: u32 = 0;
const KIND_CALLER: u32 = 1;
const KIND_SESSION: u32 = 2;
const KIND_UID: u32 = 3;

// Refusal codes, returned negative to C.
const E_INVALID_KIND: i32 = -1; // kind 0, or a kind this build does not know
const E_NO_USER_CALLER: i32 = -2; // PROC_AS_CALLER with no Ring-3 caller
const E_NO_SESSION: i32 = -3; // PROC_AS_SESSION before anyone has logged in

// Sentinel from spawnid_gid_for_uid: "this uid is not in /CONFIG/PASSWD".
const GID_UNKNOWN: u32 = 0xFFFF_FFFF;

extern "C" {
    // proc/process.c. Writes the CURRENT process's euid/egid and returns 0 iff
    // proc_current() exists AND is a PRIV_USER (Ring-3) process. Returns
    // non-zero for a kernel thread or for no-current-process, and in that case
    // writes nothing.
    fn spawnid_caller_ident(uid_out: *mut u32, gid_out: *mut u32) -> i32;

    // proc/process.c. user_lookup_uid(uid)->gid, or GID_UNKNOWN if the uid has
    // no /CONFIG/PASSWD entry.
    fn spawnid_gid_for_uid(uid: u32) -> u32;

    // gui/desktop.c. The uid of the logged-in desktop session.
    fn desktop_get_session_uid() -> u32;

    // gui/desktop.c. Non-zero once desktop_set_session() has run, i.e. once a
    // session identity has actually been established. Distinguishes "the
    // session is root" from "there is no session", which g_session_uid alone
    // CANNOT do, because it initialises to 0 and 0 is also root's uid.
    fn desktop_session_authenticated() -> i32;
}

/// THE gid rule, in one place. A gid is never inherited and never defaulted to
/// 0: it is looked up from the user database, and a uid with no PASSWD entry
/// gets gid == uid (the per-user-group convention) rather than gid 0. An
/// unknown user must not land in root's group.
fn gid_for_uid(uid: u32) -> u32 {
    let g = unsafe { spawnid_gid_for_uid(uid) };
    if g == GID_UNKNOWN {
        uid
    } else {
        g
    }
}

/// Resolve a spawn identity to a concrete (uid, gid).
///
/// Returns 0 and writes both outputs on success; a negative E_* on refusal, in
/// which case NOTHING is written and the caller must abandon the spawn.
///
/// # Safety
/// `out_uid` and `out_gid` must be valid, writable u32s. They are written only
/// on success.
#[no_mangle]
pub unsafe extern "C" fn spawn_ident_resolve_rs(
    kind: u32,
    want_uid: u32,
    out_uid: *mut u32,
    out_gid: *mut u32,
) -> i32 {
    let (uid, gid) = match kind {
        KIND_CALLER => {
            let mut cu: u32 = 0;
            let mut cg: u32 = 0;
            // A kernel thread is NOT a legitimate identity to inherit from.
            // This is the exact case that made a cron job root, so it is a
            // hard refusal rather than a fallback to anything.
            if unsafe { spawnid_caller_ident(&mut cu, &mut cg) } != 0 {
                return E_NO_USER_CALLER;
            }
            (cu, cg)
        }
        KIND_SESSION => {
            // #745: THE GUARD BELONGS HERE, NOT IN ONE CALLER.
            //
            // g_session_uid (gui/desktop.c) initialises to 0, and 0 is also
            // root's uid, so the variable ALONE cannot tell "the session is
            // root" apart from "nobody has logged in yet". Every
            // PROC_AS_SESSION spawn that reaches this resolver before
            // desktop_set_session() runs therefore resolved to uid 0 and
            // produced a ROOT Ring-3 process. That is invisible today only
            // because the session genuinely is root; the moment the desktop
            // is uid 1000 it becomes a real escalation, and it is precisely
            // the "a zero means root" corner that the rest of #692 closed
            // everywhere else (PROC_AS_INVALID is 0 for exactly this reason).
            //
            // guestfs.rs ALREADY had this test, with a comment calling it
            // "load-bearing, not decorative". That was correct and it was in
            // the wrong place: it protected the one path whose author thought
            // of it, and left gui/desktop.c:161 (the compositor and every app
            // the desktop launches) and gui/terminal.c:781 unprotected. A
            // control that has to be remembered at each call site is the
            // failure mode this codebase keeps recording. Putting it in the
            // resolver every Ring-3 spawn already goes through makes the
            // unguarded spelling INEXPRESSIBLE rather than merely discouraged.
            //
            // Refusing is deliberately the whole behaviour: a spawn whose
            // identity cannot be determined gets NO authority, never root's.
            if unsafe { desktop_session_authenticated() } == 0 {
                return E_NO_SESSION;
            }
            let u = unsafe { desktop_get_session_uid() };
            (u, gid_for_uid(u))
        }
        KIND_UID => (want_uid, gid_for_uid(want_uid)),
        KIND_INVALID => return E_INVALID_KIND,
        _ => return E_INVALID_KIND,
    };

    unsafe {
        *out_uid = uid;
        *out_gid = gid;
    }
    0
}

/// Boot self-test. Proves this policy is LIVE on this exact build rather than
/// merely compiled in, which is the trap blame.md keeps recording. Returns a
/// bitmask of FAILED checks; 0 means all passed. proc/process.c prints it.
///
/// Only the two pure branches are asserted here: KIND_INVALID and an unknown
/// kind must refuse, and an out-of-range uid must get gid == uid rather than
/// gid 0. The CALLER and SESSION branches depend on live kernel state and are
/// proven behaviourally on a booted VM instead.
///
/// # Safety
/// Calls spawn_ident_resolve_rs with stack outputs only.
#[no_mangle]
pub unsafe extern "C" fn spawn_ident_selftest_rs() -> u32 {
    let mut fails: u32 = 0;
    let mut u: u32 = 0xDEAD;
    let mut g: u32 = 0xDEAD;

    // 1. A zero-initialised identity must be REFUSED, not treated as root.
    if unsafe { spawn_ident_resolve_rs(KIND_INVALID, 0, &mut u, &mut g) } != E_INVALID_KIND {
        fails |= 1;
    }
    // 2. It must not have written anything on refusal.
    if u != 0xDEAD || g != 0xDEAD {
        fails |= 2;
    }
    // 3. An unknown kind must also be refused.
    if unsafe { spawn_ident_resolve_rs(99, 0, &mut u, &mut g) } != E_INVALID_KIND {
        fails |= 4;
    }
    // 4. A uid with no PASSWD entry must get gid == uid, NEVER gid 0.
    //    65533 is outside every account the shipped PASSWD defines.
    if unsafe { spawn_ident_resolve_rs(KIND_UID, 65533, &mut u, &mut g) } != 0 {
        fails |= 8;
    } else if u != 65533 || g != 65533 {
        fails |= 16;
    }
    fails
}
