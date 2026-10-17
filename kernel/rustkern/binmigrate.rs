// rustkern/binmigrate.rs - [binmigrate] WHAT HAPPENS TO A BOOTSTRAP SESSION'S
// PER-ACCOUNT STATE WHEN THAT SESSION FINALLY GETS AN ACCOUNT.
//
// ===========================================================================
// THE DEFECT THIS EXISTS FOR, AND IT STRANDS USER DATA
// ===========================================================================
// [noaccount] gave the kernel ONE answer to "where does this session's
// per-account state live" (rustkern/userconf.rs, `userconf_state_root_rs`):
//
//     a session WITH a passwd entry (root's says "/")  -> that home
//     a session with NO entry at all                   -> "/TMP"
//
// That rule is right, and it has a consequence nobody followed through. The
// first-boot bootstrap session (#OOBEAUTH) runs at uid FIRST_ADMIN_UID with NO
// passwd entry, so every per-account file it writes lands under "/TMP/CONFIG".
// The Recycle Bin is one of them: "/TMP/CONFIG/RECYCLE" plus its index
// "/TMP/CONFIG/RBINDEX.TXT".
//
// Then the setup wizard creates the account. `users_create_first_admin()`
// mints it at the SAME uid the bootstrap session is already running as, with a
// home of "/HOME/<NAME8>". From that instant the identical accessor answers
// "/HOME/<NAME8>/CONFIG/RECYCLE" for the SAME LIVE SESSION, and everything the
// user deleted during setup is in a directory that nothing will ever look in
// again. Nothing moved it, because nothing was ever written to move it: the
// kernel tree had no migration logic at any account-creation site.
//
// WHY IT IS WORTH A SUBSYSTEM RATHER THAN A LINE. The owner's requirement on
// the AI delete path is that "AI can delete files but they must be
// undeletable". An item sitting in a bin that no app resolves is not
// recoverable, it is just not visibly destroyed, which is the worse of the two
// because the UI claims otherwise. And the bin is not special here: the same
// root change moves EVERY per-account file at once, and [noaccount] counted 67
// accessors in the defective family. So the unit of migration is the state
// ROOT, not the bin.
//
// ===========================================================================
// THE DESIGN, AND WHY NOT THE OTHER ONE
// ===========================================================================
// REJECTED: "put the bin somewhere system-wide until first login". That makes
// one directory serve two users, which is the exact property
// `userconf_state_root_rs` exists to destroy: the bin is derived from a
// per-session root precisely so that two users can never be handed each
// other's deleted files. It would also need a privilege Ring 3 does not have
// in order to write there, and [noaccount] already records that the right
// answer is to MOVE the file rather than GRANT the permission.
//
// CHOSEN: at account creation, MOVE the bootstrap state root's CONFIG tree
// into the new account's CONFIG. It cannot strand data (the data ends up where
// the one resolver says to look), it needs no new privilege, it preserves the
// index rows verbatim so the "By: AI" actor column, the original location and
// the deletion timestamp all survive, and it is a one-shot event with an
// obvious trigger.
//
// THE TRIGGER IS AN ARMED SIGNAL, NOT A STRING COMPARISON. [noaccount]'s own
// lesson: "a rule with an escape hatch is a rule somebody eventually applies
// without the hatch". The fact that matters is "THIS session was established
// with no user-table entry", which is known exactly once, at the moment the
// session is established (main.c, beside `recycle_session_changed()`). It is
// recorded here and consumed by whichever account-creation path later mints
// that uid. Deriving it afterwards is impossible: the entry exists by then.
//
// WHY IT IS NOT HOOKED IN `user_create()`, WHICH WOULD BE ONE SITE INSTEAD OF
// TWO. `users_create_first_admin()` ROLLS BACK with `user_delete()` if either
// password fails to set, and `sys_user_create_pw()` rolls back if the new
// account cannot authenticate. A migration performed inside `user_create()`
// would have moved the user's files into the home of an account that then
// ceased to exist, which is a worse outcome than the bug. So the claim happens
// only after the account is COMMITTED.
//
// WHY RUST: this is new kernel code (2026-07-16 rule) and there is no
// performance argument: it runs once per machine, at account creation, never
// in a loop. What is here is the part with a failure shape: a bounded path
// join of two caller-supplied strings into a fixed buffer (CWE-120/CWE-170,
// the shape that keeps producing kernel bugs in C) and a four-way disposition
// whose wrong answers are "clobber the user's file" and "leave it stranded".
// The filesystem walk stays in C in fs/userconf.c because every line of it is
// a call into the C fat_* / perms_* primitives, which have no Rust surface:
// exactly the split fs/recycle_fs.c has against rustkern/recycle.rs.

use core::sync::atomic::{AtomicU32, Ordering};

// ===========================================================================
// THE DISPOSITION CODES. A WIRE FORMAT SHARED WITH C.
// ===========================================================================
// fs/userconf.h repeats these as BM_ACT_* and locks the numbering with a
// _Static_assert, so a renumbering here fails the C compile rather than
// silently turning every MOVE into an APPEND.
pub const BM_ACT_MOVE: i32 = 0;     // the destination is free: rename it across
pub const BM_ACT_APPEND: i32 = 1;   // both exist and the file is a row log: concatenate
pub const BM_ACT_SKIP: i32 = 2;     // leave it where it is, and SAY SO
pub const BM_ACT_RECURSE: i32 = 3;  // a directory: descend one level

/// How deep below the state root this walk may go.
///
/// 1, not "unbounded". The only subdirectory this tree has ever had is the
/// Recycle Bin, and a bounded walk cannot be made to recurse forever by a
/// crafted directory, cannot blow the kernel stack, and makes the SKIP of
/// anything deeper a reported fact instead of an unnoticed omission.
pub const BM_MAX_DEPTH: u32 = 1;

/// What to do with one entry of the bootstrap state root's CONFIG tree.
///
/// `is_dir`      non-zero for a directory entry.
/// `dest_exists` non-zero if a name with the same leaf already exists at the
///               destination.
/// `mergeable`   non-zero if this file is an APPEND-ONLY ROW LOG whose two
///               halves can simply be concatenated. The caller decides which
///               names those are, because the one definition of the Recycle
///               Bin index leaf lives in fs/recycle_fs.c and repeating the
///               literal here would be a second spelling of it.
/// `depth`       0 at the state root's CONFIG directory itself.
///
/// NEVER RETURNS "OVERWRITE". The destination is the account's own home, which
/// `users_make_home_skeleton()` has already populated; a migration that
/// clobbers a file the new account already has is a data loss caused by a data
/// rescue. The honest answer for an unmergeable collision is to leave the
/// source alone and report it, which loses nothing and is visible.
#[no_mangle]
pub extern "C" fn binmig_plan_rs(is_dir: i32, dest_exists: i32,
                                 mergeable: i32, depth: u32) -> i32 {
    if is_dir != 0 {
        // A directory is DESCENDED, never renamed wholesale. The destination
        // "<home>/CONFIG" already exists on every path that reaches here, so a
        // directory rename would have to merge anyway, and a merge done one
        // file at a time is the only version in which a collision has a
        // per-file answer.
        return if depth < BM_MAX_DEPTH { BM_ACT_RECURSE } else { BM_ACT_SKIP };
    }
    if dest_exists == 0 {
        return BM_ACT_MOVE;
    }
    if mergeable != 0 {
        return BM_ACT_APPEND;
    }
    BM_ACT_SKIP
}

// ===========================================================================
// THE ARMED SIGNAL
// ===========================================================================
// AtomicU32 rather than plain statics for the reason firstrun.rs gives for the
// same shape: a syscall on another core can reach the claim while this core is
// still in the login path, and the exactly-once property has to hold under
// that, not just read well.
static ARMED: AtomicU32 = AtomicU32::new(0);
static ARMED_UID: AtomicU32 = AtomicU32::new(0);

/// Record whether the session just established has a user-table entry.
///
/// Called from main.c the moment the session identity becomes known, with
/// `has_entry` taken from a real `user_lookup_uid()`, NEVER from a comparison
/// of the home string against "/": root's own entry SAYS its home is "/", so
/// the string cannot tell root from a bootstrap session. That is [noaccount]'s
/// lesson and this is the second site it applies to.
///
/// A session that HAS an entry DISARMS, so a lock-screen re-authentication as
/// a real user cannot leave a stale arm behind for a later account creation.
/// Returns 1 if the signal is now armed, else 0.
#[no_mangle]
pub extern "C" fn binmig_arm_rs(has_entry: i32, uid: u32) -> i32 {
    if has_entry != 0 {
        ARMED.store(0, Ordering::SeqCst);
        return 0;
    }
    ARMED_UID.store(uid, Ordering::SeqCst);
    ARMED.store(1, Ordering::SeqCst);
    1
}

/// The armed uid, or -1 when nothing is armed. Diagnostics only: the boot line
/// that says whether this machine is in the window at all.
#[no_mangle]
pub extern "C" fn binmig_armed_uid_rs() -> i64 {
    if ARMED.load(Ordering::SeqCst) == 0 {
        -1
    } else {
        ARMED_UID.load(Ordering::SeqCst) as i64
    }
}

/// Claim the migration for `uid`, EXACTLY ONCE. Returns 1 iff the signal was
/// armed for this uid; the signal is consumed, so a second caller gets 0.
///
/// Exactly-once matters because two commit sites call it
/// (`users_create_first_admin()` and `sys_user_create_pw()`), and on the
/// shipped-image path the wizard's syscall reaches BOTH. Two migrations over
/// the same files would have the second one walking a directory the first one
/// emptied, which is harmless today and is the kind of harmless that stops
/// being harmless when someone adds a third caller.
#[no_mangle]
pub extern "C" fn binmig_claim_rs(uid: u32) -> i32 {
    if ARMED.load(Ordering::SeqCst) == 0 {
        return 0;
    }
    if ARMED_UID.load(Ordering::SeqCst) != uid {
        return 0;
    }
    match ARMED.compare_exchange(1, 0, Ordering::SeqCst, Ordering::SeqCst) {
        Ok(_) => 1,
        Err(_) => 0,
    }
}

/// Join `dir` and `name` into "<dir>/<name>" with exactly one separator,
/// writing a NUL-terminated result into `out`. Returns the length written, or
/// -1 if it does not fit or the inputs are unusable.
///
/// REFUSES RATHER THAN TRUNCATES. A truncated path is a DIFFERENT file, and in
/// this subsystem a different file is either a clobber of something innocent
/// or a lost original. fs/recycle_fs.c's own rcy_join() refuses for the same
/// reason and says so in the same words; this is the Rust one because this is
/// new code.
#[no_mangle]
pub extern "C" fn binmig_join_rs(dir: *const u8, name: *const u8,
                                 out: *mut u8, cap: u32) -> i32 {
    if dir.is_null() || name.is_null() || out.is_null() || cap == 0 {
        return -1;
    }
    let capu = cap as usize;
    // SAFETY: the caller guarantees `out` spans at least `cap` writable bytes;
    // every write below goes through this exactly-cap slice, so no index can
    // reach past it.
    let d: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, capu) };

    // SAFETY: both are NUL-terminated C strings supplied by the kernel's own
    // path code; cstr_bounded refuses an unterminated one within 256 bytes
    // rather than reading on.
    let dd = match unsafe { cstr_bounded(dir, 256) } { Some(s) => s, None => return -1 };
    let nn = match unsafe { cstr_bounded(name, 256) } { Some(s) => s, None => return -1 };

    // Trim trailing separators off the directory, but never below one byte: a
    // dir of "/" must still produce "/NAME" and not "NAME".
    let mut dlen = dd.len();
    while dlen > 1 && dd[dlen - 1] == b'/' { dlen -= 1; }
    if dlen == 0 || dd[0] != b'/' { return -1; }   // empty or relative: refuse
    // A name with a separator in it is not a leaf, and treating it as one is
    // how a walk escapes the directory it is walking.
    if nn.is_empty() { return -1; }
    let mut i = 0usize;
    while i < nn.len() {
        if nn[i] == b'/' { return -1; }
        i += 1;
    }

    let sep = if dd[dlen - 1] == b'/' { 0usize } else { 1usize };
    let total = dlen + sep + nn.len();
    if total + 1 > capu { return -1; }

    let mut w = 0usize;
    i = 0;
    while i < dlen { d[w] = dd[i]; w += 1; i += 1; }
    if sep == 1 { d[w] = b'/'; w += 1; }
    i = 0;
    while i < nn.len() { d[w] = nn[i]; w += 1; i += 1; }
    d[w] = 0;
    w as i32
}

/// Absolute assertions over the disposition table, the join and the armed
/// signal. `*out_checks` receives the number of cases that actually RAN,
/// counted at runtime; returns the number that FAILED, 0 = PASS.
///
/// CASES RAN, NOT CASES DECLARED, and the count is incremented beside each
/// assertion rather than written as a literal: a self-test that returns only a
/// verdict cannot distinguish "all passed" from "none executed", and a literal
/// count goes stale the first time a case is added.
///
/// IT RESTORES THE LIVE SIGNAL. main.c's self-test block runs AFTER login, so
/// the arm has already happened by the time this executes on a first boot.
/// Trampling it would mean this self-test DISABLED the fix it is testing, on
/// precisely the one boot that needs it. firstrun.rs's self-test documents the
/// same hazard for the same reason.
#[no_mangle]
pub extern "C" fn binmig_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut bad: i32 = 0;
    let mut ran: u32 = 0;

    // Save the live signal before touching it, and put it back at the end.
    let saved_armed = ARMED.load(Ordering::SeqCst);
    let saved_uid = ARMED_UID.load(Ordering::SeqCst);

    // ---- the disposition table -------------------------------------------
    // 1. A free destination: move it. This is the whole normal case.
    ran += 1; if binmig_plan_rs(0, 0, 0, 0) != BM_ACT_MOVE { bad += 1; }
    // 2. An occupied destination that is NOT a row log: leave the source
    //    alone. The one answer that must never appear here is a clobber.
    ran += 1; if binmig_plan_rs(0, 1, 0, 0) != BM_ACT_SKIP { bad += 1; }
    // 3. An occupied destination that IS a row log: concatenate, so neither
    //    set of index rows is lost and every "By: AI" attribution survives.
    ran += 1; if binmig_plan_rs(0, 1, 1, 0) != BM_ACT_APPEND { bad += 1; }
    // 4. "mergeable" must not override a FREE destination into an append: a
    //    plain move keeps the bytes identical, an append would depend on the
    //    destination read returning nothing.
    ran += 1; if binmig_plan_rs(0, 0, 1, 0) != BM_ACT_MOVE { bad += 1; }
    // 5. A directory at the root is descended, whether or not the destination
    //    directory already exists (it does: the home skeleton makes it).
    ran += 1; if binmig_plan_rs(1, 0, 0, 0) != BM_ACT_RECURSE { bad += 1; }
    ran += 1; if binmig_plan_rs(1, 1, 0, 0) != BM_ACT_RECURSE { bad += 1; }
    // 6. A directory one level down is NOT descended: the walk is bounded.
    ran += 1; if binmig_plan_rs(1, 0, 0, BM_MAX_DEPTH) != BM_ACT_SKIP { bad += 1; }
    ran += 1; if binmig_plan_rs(1, 0, 0, 9) != BM_ACT_SKIP { bad += 1; }

    // ---- the join ---------------------------------------------------------
    {
        let mut buf = [0u8; 64];
        fn eq(buf: &[u8], rc: i32, want: &[u8]) -> bool {
            rc >= 0 && (rc as usize) == want.len() && &buf[..want.len()] == want
                && buf[want.len()] == 0
        }
        ran += 1;
        let rc = binmig_join_rs(b"/TMP/CONFIG\0".as_ptr(), b"RBINDEX.TXT\0".as_ptr(),
                                buf.as_mut_ptr(), 64);
        if !eq(&buf, rc, b"/TMP/CONFIG/RBINDEX.TXT") { bad += 1; }
        // A trailing separator on the directory must not double up.
        ran += 1;
        let rc = binmig_join_rs(b"/TMP/CONFIG/\0".as_ptr(), b"A\0".as_ptr(),
                                buf.as_mut_ptr(), 64);
        if !eq(&buf, rc, b"/TMP/CONFIG/A") { bad += 1; }
        // The filesystem root is a legal directory and must not lose its slash.
        ran += 1;
        let rc = binmig_join_rs(b"/\0".as_ptr(), b"A\0".as_ptr(), buf.as_mut_ptr(), 64);
        if !eq(&buf, rc, b"/A") { bad += 1; }
        // REFUSE rather than truncate: "/A/B" needs 5 bytes with the NUL.
        ran += 1;
        if binmig_join_rs(b"/A\0".as_ptr(), b"B\0".as_ptr(), buf.as_mut_ptr(), 4) != -1 { bad += 1; }
        // A name that is not a leaf is refused: a walk that accepts "../X" is
        // a walk that writes outside the directory it is migrating.
        ran += 1;
        if binmig_join_rs(b"/A\0".as_ptr(), b"../X\0".as_ptr(), buf.as_mut_ptr(), 64) != -1 { bad += 1; }
        // A relative or empty directory is a programming error, not something
        // to paper over by producing a relative path.
        ran += 1;
        if binmig_join_rs(b"TMP\0".as_ptr(), b"A\0".as_ptr(), buf.as_mut_ptr(), 64) != -1 { bad += 1; }
        ran += 1;
        if binmig_join_rs(b"\0".as_ptr(), b"A\0".as_ptr(), buf.as_mut_ptr(), 64) != -1 { bad += 1; }
        ran += 1;
        if binmig_join_rs(b"/A\0".as_ptr(), b"\0".as_ptr(), buf.as_mut_ptr(), 64) != -1 { bad += 1; }
    }

    // ---- the armed signal -------------------------------------------------
    // Nothing armed: nobody may claim.
    ARMED.store(0, Ordering::SeqCst);
    ran += 1; if binmig_claim_rs(1000) != 0 { bad += 1; }
    ran += 1; if binmig_armed_uid_rs() != -1 { bad += 1; }
    // A session WITH an entry does not arm. This is the root case: root has an
    // entry, so a root session must never claim a migration.
    ran += 1; if binmig_arm_rs(1, 0) != 0 { bad += 1; }
    ran += 1; if binmig_claim_rs(0) != 0 { bad += 1; }
    // A session with NO entry arms, for its own uid only.
    ran += 1; if binmig_arm_rs(0, 1000) != 1 { bad += 1; }
    ran += 1; if binmig_armed_uid_rs() != 1000 { bad += 1; }
    ran += 1; if binmig_claim_rs(1001) != 0 { bad += 1; }   // a DIFFERENT uid: no
    ran += 1; if binmig_claim_rs(1000) != 1 { bad += 1; }   // the armed uid: yes
    // EXACTLY ONCE: the second claim of the same uid is refused.
    ran += 1; if binmig_claim_rs(1000) != 0 { bad += 1; }
    // Arming for a real account afterwards disarms rather than re-arming.
    ran += 1; if binmig_arm_rs(0, 1000) != 1 { bad += 1; }
    ran += 1; if binmig_arm_rs(1, 1000) != 0 { bad += 1; }
    ran += 1; if binmig_claim_rs(1000) != 0 { bad += 1; }

    ARMED_UID.store(saved_uid, Ordering::SeqCst);
    ARMED.store(saved_armed, Ordering::SeqCst);

    if !out_checks.is_null() {
        // SAFETY: a single u32 the caller supplied by address.
        unsafe { *out_checks = ran; }
    }
    bad
}

/// Bounded scan for a C string's terminator. Returns None if unterminated
/// within `max`, so a corrupt entry cannot drive an unbounded read.
unsafe fn cstr_bounded<'a>(p: *const u8, max: usize) -> Option<&'a [u8]> {
    let mut n = 0usize;
    while n < max {
        if *p.add(n) == 0 {
            return Some(core::slice::from_raw_parts(p, n));
        }
        n += 1;
    }
    None
}
