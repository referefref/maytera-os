// rustkern/userconf.rs - #683: where a PER-USER preference lives.
//
// WHY THIS EXISTS
// ---------------
// Measured under #674/#679 on a uid-1000 session, the desktop was refused five
// writes: /CONFIG/THEME.CFG, /CONFIG/AICHAT.CFG, /CONFIG/NOTIFY.TXT, /CONFIG
// itself, and /DOCKSTYL.CFG. Every one of those is a PER-USER PREFERENCE (the
// theme, the dock style, notification state, AI chat settings) that was being
// stored in /etc, or loose in /.
//
// The wrong fix is to give the desktop authority over /etc, whether by relaxing
// the mode, adding a setuid helper, or special-casing the compositor. The right
// fix is that the files are in the wrong place: /etc is SYSTEM configuration,
// and a user's preferences belong to the user. Moving them REMOVES the need for
// the privilege instead of GRANTING it, which is the pattern to prefer
// everywhere: a permission that nobody has to ask for cannot be misused.
//
// THE CONVENTION, one sentence: a per-user preference NAME lives at
// <home>/CONFIG/<NAME>, where <home> is the user's home directory from the
// passwd table. It deliberately mirrors the existing /CONFIG/<NAME> layout, so
// the mapping is mechanical and there is no second naming scheme to learn. It
// follows the home-directory convention already in the tree (the per-user
// profile at /HOME/ADMIN/UIPROFIL.YML, and the home skeleton that
// users_make_home_skeleton() already creates with the user's own ownership).
//
// MIGRATION IS BUILT INTO THE RULE, NOT BOLTED ON. Existing installs have these
// files in /CONFIG, and a kernel that simply stopped reading the old location
// would silently lose the user's theme and dock on upgrade. So the rule is
// asymmetric, which is the whole trick:
//
//     READ  -> per-user path first, then FALL BACK to the legacy path.
//     WRITE -> always the per-user path.
//
// A user who has never changed their theme keeps reading the shipped
// /CONFIG/THEME.CFG. The first time they change it, the write lands in their
// home and every later read finds it there. Migration therefore needs no
// one-shot copy step, no "have I migrated yet" flag, and no first-boot pass
// that could half-complete; it is a consequence of the lookup order. It also
// degrades correctly for a user with no home (uid 0's home is "/"), and for a
// system-wide default that an administrator edits in /CONFIG, which continues
// to apply to every user who has not overridden it.
//
// WHY RUST: this is new kernel code and there is no performance argument
// against it (it runs when a preference is loaded or saved, not in any loop).
// The work is bounded string joining of a table-supplied home with a
// caller-supplied name into a fixed buffer, which is precisely the
// CWE-120/CWE-170 shape that keeps producing kernel bugs in C. The C side keeps
// only the passwd-table lookup, which is entangled with the existing C
// user_entry_t table, exactly the same split as rustkern/permpath.rs.

/// Join `home` and `name` into `<home>/CONFIG/<NAME>`, writing a NUL-terminated
/// result into `out`. Returns the length written, or -1 if it does not fit or
/// the inputs are unusable.
///
/// A trailing '/' on `home` is collapsed, so the root user's home of "/" yields
/// "/CONFIG/<NAME>" rather than "//CONFIG/<NAME>". That is deliberate and is
/// the correct answer for root: root's per-user config location IS the system
/// one, so root's reads and writes stay exactly where they are today and this
/// change is a no-op for the shipping root session.
#[no_mangle]
pub extern "C" fn userconf_join_rs(home: *const u8, name: *const u8,
                                   out: *mut u8, cap: u32) -> i32 {
    if home.is_null() || name.is_null() || out.is_null() || cap == 0 {
        return -1;
    }
    let capu = cap as usize;
    // SAFETY: caller guarantees `out` spans at least `cap` writable bytes; every
    // write below goes through this exactly-cap slice.
    let d: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, capu) };

    let h = match unsafe { cstr_bounded(home, 256) } { Some(s) => s, None => return -1 };
    let n = match unsafe { cstr_bounded(name, 256) } { Some(s) => s, None => return -1 };
    if n.is_empty() {
        return -1;
    }

    // Trim any trailing slashes off the home component, so "/" and "/HOME/ADMIN/"
    // both behave. hlen == 0 means the home was "/" (or empty): the join then
    // produces a correctly rooted "/CONFIG/<NAME>".
    let mut hlen = h.len();
    while hlen > 0 && h[hlen - 1] == b'/' {
        hlen -= 1;
    }
    // A relative home would produce a relative key; perms_check canonicalizes
    // relative paths to root-relative, so this would still be well defined, but
    // a passwd entry with a relative home is a configuration error, not
    // something to paper over.
    if hlen > 0 && h[0] != b'/' {
        return -1;
    }
    // Skip any leading '/' on the name: the caller passes either "THEME.CFG" or
    // "/CONFIG/THEME.CFG"-style leaf names, and both should land in one place.
    let mut ns = 0usize;
    while ns < n.len() && n[ns] == b'/' {
        ns += 1;
    }
    if ns >= n.len() {
        return -1;
    }

    const MID: &[u8] = b"/CONFIG/";
    let total = hlen + MID.len() + (n.len() - ns);
    if total + 1 > capu {
        return -1; // fail rather than truncate: a truncated path is a DIFFERENT file
    }

    let mut w = 0usize;
    let mut i = 0usize;
    while i < hlen { d[w] = h[i]; w += 1; i += 1; }
    i = 0;
    while i < MID.len() { d[w] = MID[i]; w += 1; i += 1; }
    i = ns;
    while i < n.len() { d[w] = n[i]; w += 1; i += 1; }
    d[w] = 0;
    w as i32
}

// ===========================================================================
// [noaccount] THE DECISION, not the bits: WHERE DOES THIS SESSION'S
// PER-ACCOUNT STATE LIVE.
// ===========================================================================
//
// #747 recorded the lesson this function exists to apply: a shared decoder is
// only as shared as its OUTPUT is. `userconf_join_rs` above shares the JOIN,
// and every caller was still left to work out for itself WHICH HOME to join
// onto - which is the part with a wrong answer in it.
//
// THE WRONG ANSWER, measured on golden 2564 (commit 0fe362a4) over one boot
// with no user input: the first-boot bootstrap session (uid 1000, NO passwd
// entry at all) resolves its home to "/", so every per-account path collapses
// onto a root-owned location. Ring 0 never notices, because perms_check()
// returns allow on its first line for the kernel; Ring 3 is refused -13 on all
// of them. The Recycle Bin is the clearest case: the kernel CREATED
// /CONFIG/RECYCLE through its own accessor and the recycler daemon could not
// even READ it back, let alone create /CONFIG/RCYCLOG.TXT beside it.
//
// THE RULE, and it needs no uid special case:
//     a session WITH a passwd entry (root's says "/")  -> that home
//     a session with NO entry at all                   -> "/TMP" (01777)
//
// ASK FOR THE ENTRY, NEVER COMPARE THE STRING. "home == \"/\"" cannot tell
// root from a bootstrap session, so a fix built on it needs an explicit uid-0
// escape hatch bolted on, and a rule with an escape hatch is a rule somebody
// eventually applies without it.
//
// WHY RUST, and it is the same answer as the join above: this is new kernel
// code, it runs when a preference is opened and in no loop, and it is bounded
// copying of a table-supplied string into a fixed buffer. The C side keeps
// only the user-table lookup, which is entangled with the existing C
// `user_entry_t`.
//
// THE USERLAND TWIN is `uc_state_root()` in userland/libc/userconf.c. Ring 3
// cannot link rustkern, so the literal is spelled out there too; the self-test
// below asserts the exact bytes, so a change on one side that is not made on
// the other turns the boot line RED instead of silently splitting every
// per-account file in two.
pub const NOACCOUNT_ROOT: &[u8] = b"/TMP";

/// Resolve this session's per-account STATE ROOT into `out`.
///
/// `has_entry` is non-zero iff the session uid has a real user-table entry.
/// `home` is that entry's home (ignored when `has_entry` is 0; may be NULL).
/// Returns the length written, or -1 if it does not fit or the inputs are
/// unusable. Fails rather than truncating: a truncated path is a DIFFERENT
/// directory, and silently writing a user's files into one is the bug.
#[no_mangle]
pub extern "C" fn userconf_state_root_rs(has_entry: i32, home: *const u8,
                                         out: *mut u8, cap: u32) -> i32 {
    if out.is_null() || cap == 0 { return -1; }
    let capu = cap as usize;
    // SAFETY: caller guarantees `out` spans at least `cap` writable bytes.
    let d: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, capu) };

    let src: &[u8] = if has_entry != 0 {
        if home.is_null() { return -1; }
        match unsafe { cstr_bounded(home, 256) } { Some(s) => s, None => return -1 }
    } else {
        NOACCOUNT_ROOT
    };

    // Trim trailing separators, but never the root itself: "" is not a path
    // and would make every join below it relative.
    let mut n = src.len();
    while n > 1 && src[n - 1] == b'/' { n -= 1; }
    if n == 0 || src[0] != b'/' { return -1; }   // empty or relative: refuse
    if n + 1 > capu { return -1; }

    let mut i = 0usize;
    while i < n { d[i] = src[i]; i += 1; }
    d[n] = 0;
    n as i32
}

/// Absolute assertions over `userconf_state_root_rs`. Returns the number of
/// failing cases, 0 = PASS. Called from main.c's boot self-test block.
///
/// ABSOLUTE, not a differential against the C that used to do this: the thing
/// being removed is two sites each deciding the home for themselves, so a test
/// that only asserted they AGREE would have passed while both were wrong.
/// Case 1 is the whole point and is the one a naive implementation fails.
#[no_mangle]
pub extern "C" fn userconf_state_root_selftest_rs() -> u32 {
    let mut bad: u32 = 0;
    let mut buf = [0u8; 64];

    fn eq(buf: &[u8], rc: i32, want: &[u8]) -> bool {
        rc >= 0 && (rc as usize) == want.len() && &buf[..want.len()] == want
            && buf[want.len()] == 0
    }

    // 1. NO ENTRY (the first-boot bootstrap session): /TMP, NOT "/". This is
    //    the case that was broken, and it is also the cross-check on the
    //    userland copy of the literal.
    let rc = userconf_state_root_rs(0, core::ptr::null(), buf.as_mut_ptr(), 64);
    if !eq(&buf, rc, b"/TMP") { bad += 1; }
    { let want: &[u8] = b"/TMP"; if NOACCOUNT_ROOT != want { bad += 1; } }

    // 2. ROOT: its entry says "/", and it must stay "/". A rule built on
    //    "home != /" gets this one wrong and moves root's files.
    let rc = userconf_state_root_rs(1, b"/\0".as_ptr(), buf.as_mut_ptr(), 64);
    if !eq(&buf, rc, b"/") { bad += 1; }

    // 3. An ordinary account: unchanged, trailing slash collapsed.
    let rc = userconf_state_root_rs(1, b"/HOME/ADMIN\0".as_ptr(), buf.as_mut_ptr(), 64);
    if !eq(&buf, rc, b"/HOME/ADMIN") { bad += 1; }
    let rc = userconf_state_root_rs(1, b"/HOME/ADMIN/\0".as_ptr(), buf.as_mut_ptr(), 64);
    if !eq(&buf, rc, b"/HOME/ADMIN") { bad += 1; }

    // 4. REFUSE rather than truncate. "/HOME/ADMIN" needs 12 bytes with the
    //    NUL; 11 must fail, not hand back "/HOME/ADMI".
    if userconf_state_root_rs(1, b"/HOME/ADMIN\0".as_ptr(), buf.as_mut_ptr(), 11) != -1 { bad += 1; }

    // 5. A relative or empty home is a configuration error, not something to
    //    paper over by making every path below it relative.
    if userconf_state_root_rs(1, b"HOME\0".as_ptr(), buf.as_mut_ptr(), 64) != -1 { bad += 1; }
    if userconf_state_root_rs(1, b"\0".as_ptr(), buf.as_mut_ptr(), 64) != -1 { bad += 1; }

    // 6. A no-entry session with a non-NULL home must STILL get /TMP: the home
    //    is meaningless when there is no entry behind it, and honouring it
    //    would reintroduce exactly the collapse this fixes.
    let rc = userconf_state_root_rs(0, b"/\0".as_ptr(), buf.as_mut_ptr(), 64);
    if !eq(&buf, rc, b"/TMP") { bad += 1; }

    bad
}

/// Bounded scan for a C string's terminator. Returns None if unterminated
/// within `max`, so a corrupt table entry cannot drive an unbounded read.
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
