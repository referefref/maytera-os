// rustkern/volperm.rs - [openperm] #246: THE TWO POLICY DECISIONS that make the
// file-OPEN path and the DELETE path agree about permissions on a mounted
// removable volume.
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
// There is no performance argument for C: both functions are a handful of
// integer comparisons, one of them on a path that has already done a syscall
// entry and a string canonicalization, and the other once per volume mount. No
// float, no state, no allocation, no waiting.
//
// ===========================================================================
// THE DEFECT, MEASURED BEFORE THIS FILE WAS WRITTEN
// ===========================================================================
// proc/fdlayer.c sys_open_k() gated its ENTIRE permission block on
// `hotplug_resolve_path(path, 0) < 0`, a deliberate carve-out (#250) whose
// stated reasoning was that a FAT stick carries no POSIX ownership. Meanwhile
// sys_unlink / sys_rmdir / sys_rename have no such carve-out and have enforced
// on a volume since [renamevol] (716367de).
//
// Measured on a booted VM, kernel 940001, one unprivileged uid, one all-users
// sticky directory on a synthetic FAT16 stick, ten arms per medium:
//
//   unlink ANOTHER user's file on the stick   -> rc=-1, victim intact   REFUSED
//   O_RDONLY|O_TRUNC the same shape of file   -> fd=259, len-now=0      BLANKED
//   O_WRONLY (no trunc) and write 8 bytes     -> fd=259, bytes landed   DEFACED
//   read ANOTHER user's 0600 file             -> 64 bytes read          DISCLOSED
//
// The identical ten arms on the ROOT filesystem were all correct in the same
// boot. So the delete protection on removable media was decorative, and the
// hole was wider than the truncation it was reported as: the carve-out skipped
// the WHOLE block, so R_OK went unchecked too and it was a confidentiality
// failure as well as an integrity one.
//
// ===========================================================================
// WHICH SIDE IS RIGHT, AND WHY IT IS NOT A COIN TOSS
// ===========================================================================
// Both positions were defensible when #250 was written. One of them has since
// stopped being true, and the transcript says so rather than an argument:
//
//   #250's PREMISE: "a removable volume carries no POSIX ownership, so any
//   permission answer we give is invented." That was true in 2026. It is now
//   FALSE as a statement about THIS system. [renamevol]'s CREATE POINT 3 gives
//   a file created on a stick by an unprivileged user a real ownership row in
//   /CONFIG/PERMS.DB, and the delete path already acts on those rows well
//   enough to make a DISCRIMINATING decision: refuse another user's file,
//   permit your own. Measured in the same boot:
//   `perms_check("/USB0/PM/OVIC1.TXT", 1000, W_OK) = -1`. The table HAS an
//   opinion about a stick. The open path simply never asked for it.
//
//   #250's CONCERN: "leaving removable paths in would make a USB stick
//   read-only to the user who plugged it in." That is STILL TRUE and it is the
//   reason this file has a second function. perms_check()'s no-entry default
//   is root-owned 0755, and the mount point gets no row from anybody:
//   `[USBOPN-TEST] mount-point row /USB0: ABSENT`. So deleting the carve-out
//   ALONE would trade an integrity hole for an unusable stick, which is a
//   worse bug and would have shipped looking like a security win.
//
// So the enforcement side is right about the RULE and the carve-out was right
// about the CONSEQUENCE, and both halves are here, because fixing either alone
// is wrong. That is the [renamevol] lesson one gate over: it built a kernel
// with only half its fix on purpose and watched an unprivileged process destroy
// another user's file.
//
// ===========================================================================
// HOW THE TWO PATHS ARE MADE TO AGREE **BY CONSTRUCTION**
// ===========================================================================
// The failure mode to avoid was a THIRD, subtly different answer. So the fix
// adds no new comparison of its own: it gives the mount point an ordinary
// permission ROW and then lets both verbs read it from the same table, through
// the same canonicalizer, with each verb applying the rule POSIX gives it.
// There is no removable-media special case left in either path. Agreement is
// then structural rather than a coincidence two subsystems have to maintain.
//
// A CONSEQUENCE THAT LOOKS LIKE A DISAGREEMENT AND IS NOT, stated here because
// the next reader will find it and should not "fix" it: for a file on a stick
// with NO ownership row (one that arrived from another machine), truncating it
// is PERMITTED and unlinking it is REFUSED. That is not this bug returning. It
// is what POSIX does, and for the same reason: the sticky bit protects the
// NAME in the directory, not the CONTENT of the file, so in a 01777 directory
// a mode-0666 file you do not own can be emptied but not removed. Making the
// two identical would mean requiring write permission on the PARENT in order
// to truncate, which POSIX attaches to unlink and not to truncate, and would
// break legitimately editing a file in a directory you cannot write.
//
// ===========================================================================

// ---------------------------------------------------------------------------
// DECISION 1: IS THIS PATH EXEMPT FROM THE LOCAL PERMISSION TABLE AT ALL?
// ---------------------------------------------------------------------------
// This used to be a three-term boolean expression buried in a 2000-line C
// function, which is exactly how a carve-out grows a third member that nobody
// reviews. It is now a named decision with an exhaustive self-test over its
// whole input space, so adding an exemption means adding a vector and saying
// why in a place a reader will find.
//
// SMB and NFS are genuinely exempt and stay exempt: access on a network share
// is decided by the SERVER (NTLM/RPC authentication plus the share ACLs), and
// our local POSIX table, which default-denies W_OK to non-root, knows nothing
// about the remote identity. There is a real authority on the other side.
//
// A REMOVABLE VOLUME IS NOT EXEMPT, and that is the change. There is no other
// authority: the medium has no ACLs and no server. Either this kernel decides,
// or nobody does, and "nobody does" was measured above as an unprivileged
// process blanking, defacing and reading another user's files.
pub const OPENPERM_EXEMPT: i32 = 0; // skip the permission table entirely
pub const OPENPERM_ENFORCE: i32 = 1; // consult the permission table

/// `is_smb`, `is_nfs`, `is_removable`: 1 if the path is of that kind, else 0.
/// Any non-zero is treated as 1, so a C caller passing a raw predicate return
/// cannot produce a fourth answer.
#[no_mangle]
pub extern "C" fn openperm_gate_rs(is_smb: u32, is_nfs: u32, is_removable: u32) -> i32 {
    // `is_removable` is deliberately READ and deliberately IGNORED in the
    // decision. It is a parameter so that the self-test below can prove, as a
    // vector rather than as a comment, that a removable path is enforced in
    // every combination; and so that anyone re-introducing the carve-out has
    // to change this function and face its vectors, rather than adding a
    // `&& ...` to a condition at a call site.
    let _ = is_removable;
    if is_smb != 0 || is_nfs != 0 {
        OPENPERM_EXEMPT
    } else {
        OPENPERM_ENFORCE
    }
}

// ---------------------------------------------------------------------------
// DECISION 2: WHAT OWNERSHIP AND MODE DOES A FRESHLY MOUNTED REMOVABLE VOLUME
// GET FOR ITS MOUNT POINT?
// ---------------------------------------------------------------------------
// Without an answer here, decision 1 makes a stick read-only. With the WRONG
// answer here, it makes a stick a free-for-all and throws away the delete
// protection [renamevol] built. The tree already contains the argument, in
// fs/perms.c: "01777 AND NOT 0777. The sticky bit is the entire difference
// between a shared directory and a free-for-all."
//
// A WRITABLE VOLUME GETS root:root 01777, i.e. the /tmp contract:
//   - any user may CREATE on the stick, so it is usable by whoever plugged it
//     in, which is #250's concern met directly;
//   - a file they create gets THEIR ownership row ([renamevol] CREATE POINT 3),
//     so it is theirs to empty, overwrite and delete;
//   - S_ISVTX means they may NOT remove or replace a NAME belonging to someone
//     else, which is the protection that was decorative before this change.
//
// A READ-ONLY VOLUME GETS root:root 0555. Not 01777: offering write permission
// on a medium that will reject the write is a lie the permission system would
// be telling on the filesystem's behalf, and the honest failure is EACCES at
// the gate rather than an I/O error several layers down.
//
// WHY root:root AND NOT "THE USER WHO PLUGGED IT IN". That sounds friendlier
// and is worse. There is no sound answer to "who plugged it in" on a
// multi-user machine: the hotplug event arrives from an interrupt, not from a
// session. And if the mount point were owned by a user, the sticky rule's
// DIRECTORY-OWNER arm (STICKY_DIR_OWNER in rustkern/sticky.rs) would let that
// one user delete every other user's files on the stick, which is the hole
// again with a friendlier face. root owns the mount point and owns nothing on
// the volume, which is what makes the sticky rule do the work.
pub const VOLPERM_UID: u32 = 0;
pub const VOLPERM_GID: u32 = 0;
pub const VOLPERM_MODE_RW: u32 = 0o1777;
pub const VOLPERM_MODE_RO: u32 = 0o0555;

/// The mode to stamp on a mounted removable volume's mount point.
/// `writable`: 1 if the volume's filesystem can be written, else 0.
#[no_mangle]
pub extern "C" fn volperm_mount_mode_rs(writable: u32) -> u32 {
    if writable != 0 {
        VOLPERM_MODE_RW
    } else {
        VOLPERM_MODE_RO
    }
}

/// The uid and gid to stamp. Constant, and a function anyway so that C cannot
/// hard-code a different owner than the self-test checks.
#[no_mangle]
pub extern "C" fn volperm_mount_uid_rs() -> u32 {
    VOLPERM_UID
}

#[no_mangle]
pub extern "C" fn volperm_mount_gid_rs() -> u32 {
    VOLPERM_GID
}

// ---------------------------------------------------------------------------
// THE BOOT SELF-TEST. Exhaustive over the whole input space of both decisions,
// which is small enough to enumerate completely: 8 combinations for the gate
// and 2 for the mount mode, plus the constants.
//
// It runs on EVERY boot and the result is printed whether it passes or fails,
// for the reason rustkern/volblind.rs and sync/noblock.c both give: a
// self-test that only speaks on failure cannot be told apart from one that
// never runs. blame.md's most-repeated lesson is that in-tree prose lies.
//
// `out_checks` receives the number of checks actually performed, so the printed
// count is computed and not a constant in a format string. A self-test
// reporting "0 failing of 0" would otherwise read as a pass.
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn volperm_selftest_rs(out_checks: *mut u32) -> u32 {
    let mut checks: u32 = 0;
    let mut bad: u32 = 0;

    macro_rules! want {
        ($got:expr, $exp:expr) => {{
            checks += 1;
            if $got != $exp {
                bad += 1;
            }
        }};
    }

    // ---- DECISION 1, all 8 combinations of three binary inputs.
    //
    // THE FOUR ROWS THAT ARE THE WHOLE POINT are the ones with
    // is_removable = 1 and no network flag: they must say ENFORCE. Before this
    // change the live code said EXEMPT for exactly those, and an unprivileged
    // process blanked another user's file.
    for &smb in &[0u32, 1u32] {
        for &nfs in &[0u32, 1u32] {
            for &rem in &[0u32, 1u32] {
                let got = openperm_gate_rs(smb, nfs, rem);
                let exp = if smb != 0 || nfs != 0 {
                    OPENPERM_EXEMPT
                } else {
                    OPENPERM_ENFORCE
                };
                want!(got, exp);
            }
        }
    }

    // Stated again as four explicit, readable vectors rather than only as the
    // loop above. The loop's expectation is DERIVED from the same condition the
    // function uses, so on its own it would agree with whatever the function
    // does, which is the shape of a test that can never fail. These four are
    // written out literally.
    want!(openperm_gate_rs(0, 0, 1), OPENPERM_ENFORCE); // a removable path IS judged
    want!(openperm_gate_rs(0, 0, 0), OPENPERM_ENFORCE); // an ordinary local path
    want!(openperm_gate_rs(1, 0, 0), OPENPERM_EXEMPT); // SMB: the server decides
    want!(openperm_gate_rs(0, 1, 0), OPENPERM_EXEMPT); // NFS: the server decides

    // A non-1 truthy value must not produce a third answer.
    want!(openperm_gate_rs(7, 0, 0), OPENPERM_EXEMPT);
    want!(openperm_gate_rs(0, 0, 9), OPENPERM_ENFORCE);

    // ---- DECISION 2.
    want!(volperm_mount_mode_rs(1), 0o1777);
    want!(volperm_mount_mode_rs(0), 0o0555);
    want!(volperm_mount_mode_rs(3), 0o1777); // any non-zero is writable
    want!(volperm_mount_uid_rs(), 0u32);
    want!(volperm_mount_gid_rs(), 0u32);

    // ---- The properties the two constants have to HAVE, asserted as
    // properties and not as the literals above. A future edit that changed
    // 0o1777 to 0o0777 would pass the literal check only if someone also
    // changed the literal, and these three vectors are what would still fail.
    checks += 1;
    if VOLPERM_MODE_RW & 0o1000 == 0 {
        bad += 1; // the writable mode MUST carry S_ISVTX, or the stick is a free-for-all
    }
    checks += 1;
    if VOLPERM_MODE_RW & 0o002 == 0 {
        bad += 1; // ...and MUST grant write to other, or the stick is read-only
    }
    checks += 1;
    if VOLPERM_MODE_RO & 0o222 != 0 {
        bad += 1; // the read-only mode must grant write to nobody
    }
    checks += 1;
    if VOLPERM_MODE_RO & 0o111 == 0 {
        bad += 1; // ...but must still allow traversal, or the volume is unreadable
    }
    checks += 1;
    if VOLPERM_UID != 0 || VOLPERM_GID != 0 {
        bad += 1; // never a guessed user: see the comment on decision 2
    }

    if !out_checks.is_null() {
        unsafe { *out_checks = checks };
    }
    bad
}
