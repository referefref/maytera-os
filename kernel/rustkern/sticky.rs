// rustkern/sticky.rs - #prewrite: THE ONE DEFINITION of "may this principal
// remove or replace this name", for a directory that any user may write.
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
// There is no performance argument for C: every function here is a handful of
// integer comparisons on a path that has already done a syscall entry, a string
// canonicalization and a hash lookup.
//
// ===========================================================================
// WHY THIS FILE EXISTS
// ---------------------------------------------------------------------------
// The owner's decision was "an all users type prewritable directory". The naive
// implementation of that is a 0777 directory, and a 0777 directory in a system
// with more than one account is not a shared space, it is a space in which
// EVERY user may delete or overwrite EVERY other user's files. perms.c had no
// concept that could stop it: grep for sticky / S_ISVTX / 01777 across kernel/
// returned nothing but the word "sticky" meaning "latching flag", and
// perms_check_leaf() reads only bits 0..8 of the mode, so bit 9 was inert.
//
// This is the hole the UNIX sticky bit (S_ISVTX, 01000) exists to close, and
// this file is that bit's decision half.
//
// ===========================================================================
// THE RULE, AND WHY IT IS THREE PRINCIPALS AND NOT ONE
// ---------------------------------------------------------------------------
// On a directory WITHOUT the sticky bit nothing changes at all: write+search on
// the directory remains sufficient, which is what every caller already checks.
// This file can only ever SUBTRACT, and only on a directory somebody
// deliberately marked. That is what makes it safe to wire into five call sites
// at once.
//
// On a sticky directory a name may be removed or replaced by:
//
//   1. root (uid 0),
//   2. the OWNER OF THE VICTIM, or
//   3. the OWNER OF THE DIRECTORY.
//
// Principal 3 is not decoration and it is not merely POSIX compatibility: IT IS
// THE AUTOMATION ANSWER. An absolute owner-only veto is automation-hostile, and
// an automation-hostile rule does not produce security, it produces root: the
// workaround for "my cleanup job cannot delete that file" is to run the cleanup
// job as root, and once that is the habit the sticky bit protects nothing
// because everything that matters is privileged. Rule 3 means a service, an
// agent or a scheduled job that legitimately owns a workspace can clean the
// whole workspace as an ORDINARY unprivileged user, with no grant, no prompt
// and no elevation. The common case therefore never reaches for privilege.
//
// ===========================================================================
// FAIL CLOSED ON AN UNOWNED VICTIM, AND WHY THAT IS NOT PEDANTRY
// ---------------------------------------------------------------------------
// perms.c's no-entry default is "root-owned, 0755". A file with no PERMS.DB row
// is therefore root's, and rule 2 must NOT match it for a uid-1000 caller. The
// dangerous mis-implementation is to treat "no entry" as "no owner, so anyone
// may remove it", which would make the sticky bit vanish for exactly the files
// that were created before the row was flushed (perms_sync_if_dirty() is
// COALESCED onto the heartbeat, so a just-created file legitimately has no row
// on disk for a few seconds). So: NO VICTIM ENTRY => the victim is root's =>
// only root or the directory owner may remove it.
//
// ===========================================================================
// WHAT THIS FILE DELIBERATELY DOES NOT DO
// ---------------------------------------------------------------------------
// It does not look anything up. It is a PURE FUNCTION of facts the caller has
// already gathered from the perms hash table, which is why it needs no FFI over
// that table, cannot be affected by a lock it does not hold, and is exhaustively
// testable at boot with no filesystem. The lookups stay in fs/perms.c, which is
// the file that owns the table. That is the same split elevate.rs/elevate.c and
// caps.rs/caps.c already use: decision in Rust, storage access in C.

#![allow(dead_code)]

/// S_ISVTX. Bit 9 of the mode word, which perms_check_leaf() does not read and
/// which format_octal()/parse_octal() in fs/perms.c ALREADY round-trip through
/// /CONFIG/PERMS.DB as the leading digit of a 4-digit octal (measured, not
/// assumed: format_octal emits `(mode >> 9) & 7` and parse_octal is unbounded).
/// So no on-disk format change was needed to carry this.
pub const PERM_STICKY: u32 = 0o1000;

/// Outcomes, so the caller can say WHICH rule allowed a removal in the audit
/// line rather than only that one did. "It was allowed" is the message this
/// whole file exists to make more specific.
pub const STICKY_NOT_STICKY: i32 = 0; // the directory is not sticky: unchanged rules apply
pub const STICKY_ROOT: i32 = 1; // uid 0
pub const STICKY_VICTIM_OWNER: i32 = 2; // the caller owns the file
pub const STICKY_DIR_OWNER: i32 = 3; // the caller owns the directory
pub const STICKY_DENY: i32 = -1; // none of the above

/// Is this mode word's sticky bit set?
#[no_mangle]
pub extern "C" fn sticky_is_set_rs(mode: u32) -> i32 {
    if mode & PERM_STICKY != 0 {
        1
    } else {
        0
    }
}

/// THE DECISION. Every argument is a fact the caller looked up; nothing here
/// touches the filesystem or the perms table.
///
/// * `dir_has_entry`  non-zero if /CONFIG/PERMS.DB has a row for the directory.
///                    With NO row there is no mode, so there is no sticky bit
///                    and the answer is STICKY_NOT_STICKY.
/// * `dir_mode`       the directory's mode word (all 12 bits, not masked).
/// * `dir_uid`        the directory's owner.
/// * `victim_has_entry` non-zero if the file being removed has a row.
/// * `victim_uid`     the victim's owner; meaningless when it has no row.
/// * `caller_uid`     the removing process's euid.
///
/// Returns one of the STICKY_* constants above.
#[no_mangle]
pub extern "C" fn sticky_decide_rs(
    dir_has_entry: u32,
    dir_mode: u32,
    dir_uid: u32,
    victim_has_entry: u32,
    victim_uid: u32,
    caller_uid: u32,
) -> i32 {
    // No row for the directory, or the row does not carry the bit: this file
    // has no opinion and the caller's existing write+search check stands alone.
    // This is the branch that makes the change a no-op everywhere in the tree
    // except the directories that were deliberately marked.
    if dir_has_entry == 0 || (dir_mode & PERM_STICKY) == 0 {
        return STICKY_NOT_STICKY;
    }

    // Root. perms_check() already short-circuits on uid 0 before any caller
    // reaches here, so this arm is defence in depth rather than the live path:
    // it means this function is correct if it is ever called from somewhere
    // that has not applied that bypass.
    if caller_uid == 0 {
        return STICKY_ROOT;
    }

    // Rule 2: you own the thing you are removing. Requires a REAL row (see the
    // fail-closed argument in the header): a victim with no row is root's.
    if victim_has_entry != 0 && victim_uid == caller_uid {
        return STICKY_VICTIM_OWNER;
    }

    // Rule 3: you own the directory, so the whole workspace is yours to clean.
    if dir_uid == caller_uid {
        return STICKY_DIR_OWNER;
    }

    STICKY_DENY
}

// ===========================================================================
// BOOT SELF-TEST
// ---------------------------------------------------------------------------
// blame.md's most-repeated lesson is that in-tree prose lies: a control is only
// real if it was watched firing. These vectors assert BOTH directions, because
// a rule that only ever denies is an outage dressed up as a security win, and a
// rule that only ever allows is the vulnerability this change exists to avoid.
//
// Returns the number of FAILING vectors, so 0 is the pass. The caller logs it.
// ===========================================================================

struct V {
    dir_has: u32,
    dir_mode: u32,
    dir_uid: u32,
    vic_has: u32,
    vic_uid: u32,
    caller: u32,
    want: i32,
    #[allow(dead_code)]
    why: &'static str,
}

#[no_mangle]
pub extern "C" fn sticky_selftest_rs() -> u32 {
    const VECTORS: &[V] = &[
        // ---- the bit is not set: NOTHING changes, at any ownership ----------
        V { dir_has: 1, dir_mode: 0o777, dir_uid: 0, vic_has: 1, vic_uid: 1000, caller: 1001,
            want: STICKY_NOT_STICKY, why: "0777 shared dir: unchanged rules, this file abstains" },
        V { dir_has: 1, dir_mode: 0o755, dir_uid: 1000, vic_has: 1, vic_uid: 1000, caller: 1000,
            want: STICKY_NOT_STICKY, why: "an ordinary home: abstain" },
        V { dir_has: 0, dir_mode: 0o1777, dir_uid: 0, vic_has: 1, vic_uid: 1000, caller: 1001,
            want: STICKY_NOT_STICKY, why: "no row for the dir means no mode, so no bit" },

        // ---- THE ONE THAT MATTERS: a second user may not remove the first ---
        V { dir_has: 1, dir_mode: 0o1777, dir_uid: 0, vic_has: 1, vic_uid: 1000, caller: 1001,
            want: STICKY_DENY, why: "bob may NOT remove alice's file in /SHARED" },
        V { dir_has: 1, dir_mode: 0o1777, dir_uid: 0, vic_has: 1, vic_uid: 1001, caller: 1000,
            want: STICKY_DENY, why: "and the same in the other direction" },

        // ---- fail closed on a victim with no row ----------------------------
        V { dir_has: 1, dir_mode: 0o1777, dir_uid: 0, vic_has: 0, vic_uid: 1000, caller: 1000,
            want: STICKY_DENY, why: "no victim row means root owns it: refuse" },
        V { dir_has: 1, dir_mode: 0o1777, dir_uid: 0, vic_has: 0, vic_uid: 0, caller: 1001,
            want: STICKY_DENY, why: "an unowned name is not a free-for-all" },

        // ---- the three principals that ARE allowed --------------------------
        V { dir_has: 1, dir_mode: 0o1777, dir_uid: 0, vic_has: 1, vic_uid: 1000, caller: 0,
            want: STICKY_ROOT, why: "root always" },
        V { dir_has: 1, dir_mode: 0o1777, dir_uid: 0, vic_has: 1, vic_uid: 1000, caller: 1000,
            want: STICKY_VICTIM_OWNER, why: "alice removes her own file" },
        V { dir_has: 1, dir_mode: 0o1777, dir_uid: 1002, vic_has: 1, vic_uid: 1000, caller: 1002,
            want: STICKY_DIR_OWNER, why: "THE AUTOMATION PATH: the workspace owner cleans it, unprivileged" },
        V { dir_has: 1, dir_mode: 0o1777, dir_uid: 1002, vic_has: 0, vic_uid: 0, caller: 1002,
            want: STICKY_DIR_OWNER, why: "and the dir owner may remove an unowned name too" },

        // ---- the victim-owner rule must not be satisfied by uid COLLISION
        //      with the directory when the caller is neither -------------------
        V { dir_has: 1, dir_mode: 0o1777, dir_uid: 1000, vic_has: 1, vic_uid: 1000, caller: 1001,
            want: STICKY_DENY, why: "bob owns neither the dir nor the file" },

        // ---- setuid/setgid bits present must not confuse the sticky test -----
        V { dir_has: 1, dir_mode: 0o7777, dir_uid: 0, vic_has: 1, vic_uid: 1000, caller: 1001,
            want: STICKY_DENY, why: "04000|02000|01000|0777: the sticky bit is still read" },
        V { dir_has: 1, dir_mode: 0o6777, dir_uid: 0, vic_has: 1, vic_uid: 1000, caller: 1001,
            want: STICKY_NOT_STICKY, why: "setuid+setgid but NOT sticky: abstain" },
    ];

    let mut bad = 0u32;
    for v in VECTORS {
        let got = sticky_decide_rs(v.dir_has, v.dir_mode, v.dir_uid, v.vic_has, v.vic_uid, v.caller);
        if got != v.want {
            bad += 1;
        }
    }

    // And the predicate, which is trivial but is read by the seed path.
    if sticky_is_set_rs(0o1777) != 1 {
        bad += 1;
    }
    if sticky_is_set_rs(0o777) != 0 {
        bad += 1;
    }
    if sticky_is_set_rs(0o0) != 0 {
        bad += 1;
    }

    bad
}
