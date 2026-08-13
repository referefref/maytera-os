// rustkern/loginmode.rs - #745 the /CONFIG/LOGIN.CFG contract: how the sign-in
// screen mode is parsed out of that file, and how the file is COMPOSED so that
// writing one of its two keys can never erase the other.
//
// New kernel code, so Rust per the 2026-07-16 rule. There is no C twin to
// strangle: `login_mode` did not exist before this change, and the composer
// replaces two inline snprintf() calls rather than porting a function.
//
// ===========================================================================
// WHY THE COMPOSER IS ONE FUNCTION AND NOT TWO WRITERS
// ---------------------------------------------------------------------------
// sys_set_autologin() built the whole file from a single snprintf:
//
//     snprintf(buf, sizeof(buf), "autologin=%s\n", target->username)
//     ... or the literal "# autologin disabled\n"
//
// That is correct for a one-key file and silently destructive for a two-key
// one: every startup-mode change from the wizard, from Settings, from anywhere,
// would rewrite the file WITHOUT the sign-in mode line, and the mode would
// revert to the fallback with nothing logged and nothing failing. A writer and
// a reader disagreeing about one file has already shipped in this tree once.
//
// So there is exactly ONE function that decides what bytes /CONFIG/LOGIN.CFG
// contains, and both syscalls go through it. Each caller supplies the key it
// owns and passes the sentinel for the key it does not, and the composer reads
// the other one back out of the file it is about to overwrite. A new key added
// later has one place to be added, not N.
//
// THE PARSE RULE, stated once. `login_mode` is LIST if and only if the value is
// exactly the four bytes `list`. Absent, empty, truncated, mis-spelled, or
// carrying trailing junk all mean TYPED. One comparison, one direction, and no
// second literal that has to be kept in sync with the first. TYPED is also the
// safe direction: showing every account name on the machine is a disclosure, so
// it happens only on an explicit, successfully-parsed decision, never because a
// file was missing or corrupt.
//
// LINE ANCHORING IS THE POINT, not a detail. userland/apps/compositor/login.c
// (dead code, never called) searched for its key as an UNANCHORED SUBSTRING, so
// it would have honoured `#autologin=admin` inside a comment. The scan below
// walks line by line exactly as the two live C parsers in proc/syscall.c and
// gui/login.c do, so a commented-out line is a comment here too.
// ===========================================================================

/// Sign-in screen mode, returned to C and to Ring 3 by SYS_GET_LOGIN_MODE.
/// Locked against the C defines in proc/syscall.h by _Static_assert in
/// proc/syscall_argtab_lock.c (C half) and by loginmode_selftest_rs (Rust half).
pub const LOGIN_MODE_LIST: u32 = 0;
pub const LOGIN_MODE_TYPED: u32 = 1;

const KEY_MODE: &[u8] = b"login_mode=";
const KEY_AUTO: &[u8] = b"autologin=";
const VAL_LIST: &[u8] = b"list";
const VAL_TYPED: &[u8] = b"typed";
const DISABLED_LINE: &[u8] = b"# autologin disabled\n";

/// Hard cap on how far a NUL scan will run in a caller-supplied C string. The
/// caller always hands us a kernel buffer that was already bounced out of user
/// space (USERNAME_MAX is 64 there), but a bound that does not depend on the
/// caller being right is the whole reason this is in Rust.
const NAME_CAP: usize = 64;

/// SAFETY: `p` is either null or points to at least `n` readable bytes. Both
/// call sites pass a kernel heap buffer from fat_read_file*() together with the
/// size that read returned, so the length is the filesystem's, not a guess.
unsafe fn as_slice<'a>(p: *const u8, n: u32) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(p, n as usize) }
    }
}

/// SAFETY: `p` is either null or points to a NUL-terminated byte string. The
/// scan stops at `cap` regardless, so a missing terminator cannot run off the
/// end of the allocation.
unsafe fn cstr<'a>(p: *const u8, cap: usize) -> &'a [u8] {
    if p.is_null() {
        return &[];
    }
    let mut n = 0usize;
    while n < cap && unsafe { *p.add(n) } != 0 {
        n += 1;
    }
    unsafe { core::slice::from_raw_parts(p, n) }
}

/// Line-anchored lookup of `key`, returning the rest of that line (no newline).
/// Deliberately the same walk the live C parsers use: skip leading whitespace
/// and newlines, test the key at the start of what is left, otherwise advance to
/// the next line. The first match wins, exactly as `break` does in the C.
fn find_value<'a>(buf: &'a [u8], key: &[u8]) -> Option<&'a [u8]> {
    let mut i = 0usize;
    while i < buf.len() {
        while i < buf.len()
            && (buf[i] == b' ' || buf[i] == b'\n' || buf[i] == b'\r' || buf[i] == b'\t')
        {
            i += 1;
        }
        if i >= buf.len() {
            break;
        }
        if buf.len() - i >= key.len() && &buf[i..i + key.len()] == key {
            let s = i + key.len();
            let mut e = s;
            while e < buf.len() && buf[e] != b'\n' && buf[e] != b'\r' {
                e += 1;
            }
            return Some(&buf[s..e]);
        }
        while i < buf.len() && buf[i] != b'\n' {
            i += 1;
        }
    }
    None
}

/// Parse the sign-in screen mode out of a /CONFIG/LOGIN.CFG image.
///
/// `buf` / `len` are the raw file bytes (null / zero length is legal and means
/// "could not be read"). Returns LOGIN_MODE_LIST only for an exact `list`
/// value; every other outcome, including a null buffer, is LOGIN_MODE_TYPED.
#[no_mangle]
pub unsafe extern "C" fn login_mode_parse_rs(buf: *const u8, len: u32) -> u32 {
    let b = unsafe { as_slice(buf, len) };
    match find_value(b, KEY_MODE) {
        Some(v) if v == VAL_LIST => LOGIN_MODE_LIST,
        _ => LOGIN_MODE_TYPED,
    }
}

// Append helper: returns false (and leaves `n` where it was) if it would not
// fit, so the caller reports failure instead of writing a truncated config.
fn push(out: &mut [u8], n: &mut usize, s: &[u8]) -> bool {
    if *n + s.len() > out.len() {
        return false;
    }
    out[*n..*n + s.len()].copy_from_slice(s);
    *n += s.len();
    true
}

/// Compose the complete contents of /CONFIG/LOGIN.CFG.
///
/// This is the ONLY place that decides what that file contains.
///
/// `old` / `old_len`   the file as it is on disk right now, so the key this
///                     caller does not own can be carried across. Null / zero
///                     is legal: it means there was nothing to preserve.
/// `autologin`         NUL-terminated username to enable autologin for; the
///                     EMPTY string means "disabled"; NULL means "preserve
///                     whatever `old` said", which is what the login-mode
///                     syscall passes.
/// `mode`              0 = list, 1 = typed, NEGATIVE = "preserve whatever `old`
///                     said", which is what the autologin syscall passes. A
///                     preserved-but-absent mode is OMITTED, not defaulted:
///                     writing `login_mode=typed` into a file where nobody ever
///                     chose would present a fallback as a decision, and it
///                     would also change the byte content of every existing
///                     one-key file for no reason.
/// `out` / `out_cap`   destination.
///
/// Returns the number of bytes written, or -1 if the buffer is too small or the
/// username contains a byte that would forge a second line (see below).
#[no_mangle]
pub unsafe extern "C" fn login_cfg_compose_rs(
    old: *const u8,
    old_len: u32,
    autologin: *const u8,
    mode: i32,
    out: *mut u8,
    out_cap: u32,
) -> i32 {
    if out.is_null() || out_cap == 0 {
        return -1;
    }
    let oldb = unsafe { as_slice(old, old_len) };
    // SAFETY: `out` points to at least `out_cap` writable bytes; both callers
    // pass a stack array together with its own sizeof.
    let outb = unsafe { core::slice::from_raw_parts_mut(out, out_cap as usize) };

    let auto_val: &[u8] = if autologin.is_null() {
        find_value(oldb, KEY_AUTO).unwrap_or(&[])
    } else {
        unsafe { cstr(autologin, NAME_CAP) }
    };

    // A username carrying a newline, a carriage return or a NUL would let a
    // caller who controls the account name write a SECOND key into this file,
    // which is a privilege boundary and not a formatting nicety: `ada\nautologin
    // =root` is a working escalation if the value is pasted in unchecked.
    // proc/users.c already refuses such names, and this refuses them again,
    // because the composer must be correct on its own terms.
    for &c in auto_val {
        if c == b'\n' || c == b'\r' || c == 0 {
            return -1;
        }
    }

    let mode_val: Option<&[u8]> = if mode == 0 {
        Some(VAL_LIST)
    } else if mode > 0 {
        Some(VAL_TYPED)
    } else {
        // Preserve. Note this NORMALISES: a junk value on disk is rewritten as
        // `typed`, which is what the parse rule already says it means. The file
        // ends up saying what it always meant, and nothing that reads it can
        // now disagree about a value neither of them recognised.
        match find_value(oldb, KEY_MODE) {
            Some(v) if v == VAL_LIST => Some(VAL_LIST),
            Some(_) => Some(VAL_TYPED),
            None => None,
        }
    };

    let mut n = 0usize;
    if auto_val.is_empty() {
        if !push(outb, &mut n, DISABLED_LINE) {
            return -1;
        }
    } else {
        if !push(outb, &mut n, KEY_AUTO) {
            return -1;
        }
        if !push(outb, &mut n, auto_val) {
            return -1;
        }
        if !push(outb, &mut n, b"\n") {
            return -1;
        }
    }
    if let Some(m) = mode_val {
        if !push(outb, &mut n, KEY_MODE) {
            return -1;
        }
        if !push(outb, &mut n, m) {
            return -1;
        }
        if !push(outb, &mut n, b"\n") {
            return -1;
        }
    }
    n as i32
}

// ===========================================================================
// SELF-TEST
// ---------------------------------------------------------------------------
// Run once at boot (kernel/main.c, beside sessionid_selftest_rs) so the
// contract is proven LIVE on this build rather than merely compiled in.
// Returns a bit mask of failures; 0 is a pass and anything else is loud.
//
// The two cases that carry the most weight are bits 9 and 10: bit 9 asserts
// that a one-key file survives an autologin write BYTE FOR BYTE (so this change
// cannot regress an image that has never chosen a mode), and bit 10 asserts
// that DISABLING autologin keeps the mode line, which is precisely the erase
// this module exists to prevent.
// ===========================================================================
#[no_mangle]
pub unsafe extern "C" fn loginmode_selftest_rs() -> u32 {
    let mut fails: u32 = 0;

    // ---- parse ----------------------------------------------------------
    let p = |s: &[u8]| -> u32 { unsafe { login_mode_parse_rs(s.as_ptr(), s.len() as u32) } };

    // 0. The shipped golden file has no mode key at all: TYPED.
    if p(b"autologin=root\n") != LOGIN_MODE_TYPED {
        fails |= 1 << 0;
    }
    // 1. The one and only spelling that selects LIST.
    if p(b"autologin=root\nlogin_mode=list\n") != LOGIN_MODE_LIST {
        fails |= 1 << 1;
    }
    // 2. Explicit typed.
    if p(b"login_mode=typed\n") != LOGIN_MODE_TYPED {
        fails |= 1 << 2;
    }
    // 3. Trailing junk is NOT list. `listx` must not pass a prefix test.
    if p(b"login_mode=listx\n") != LOGIN_MODE_TYPED {
        fails |= 1 << 3;
    }
    // 4. Empty value, and 5. a truncated final line with no terminator.
    if p(b"login_mode=\n") != LOGIN_MODE_TYPED {
        fails |= 1 << 4;
    }
    if p(b"login_mode=lis") != LOGIN_MODE_TYPED {
        fails |= 1 << 5;
    }
    // 6. A COMMENTED-OUT line must not be honoured. This is the unanchored
    //    substring bug that the dead compositor parser shipped with.
    if p(b"#login_mode=list\n") != LOGIN_MODE_TYPED {
        fails |= 1 << 6;
    }
    // 7. Nothing at all, and 8. a null buffer, both fall back to TYPED.
    if p(b"") != LOGIN_MODE_TYPED {
        fails |= 1 << 7;
    }
    if unsafe { login_mode_parse_rs(core::ptr::null(), 0) } != LOGIN_MODE_TYPED {
        fails |= 1 << 8;
    }

    // ---- compose --------------------------------------------------------
    let mut buf: [u8; 128] = [0; 128];
    let compose = |old: &[u8], auto: Option<&[u8]>, mode: i32, out: &mut [u8; 128]| -> i32 {
        let ap = match auto {
            Some(a) => a.as_ptr(),
            None => core::ptr::null(),
        };
        unsafe {
            login_cfg_compose_rs(
                old.as_ptr(),
                old.len() as u32,
                ap,
                mode,
                out.as_mut_ptr(),
                out.len() as u32,
            )
        }
    };

    // 9. NO-REGRESSION: a file with no mode key comes back byte-identical.
    {
        let n = compose(b"autologin=root\n", Some(b"root\0"), -1, &mut buf);
        if n != 15 || &buf[..15] != b"autologin=root\n" {
            fails |= 1 << 9;
        }
    }
    // 10. THE BUG THIS MODULE EXISTS FOR: disabling autologin must NOT erase
    //     the mode line.
    {
        let n = compose(b"autologin=root\nlogin_mode=list\n", Some(b"\0"), -1, &mut buf);
        let want = b"# autologin disabled\nlogin_mode=list\n";
        if n != want.len() as i32 || &buf[..want.len()] != want {
            fails |= 1 << 10;
        }
    }
    // 11. Changing WHO autologs in also keeps the mode.
    {
        let n = compose(b"autologin=root\nlogin_mode=list\n", Some(b"ada\0"), -1, &mut buf);
        let want = b"autologin=ada\nlogin_mode=list\n";
        if n != want.len() as i32 || &buf[..want.len()] != want {
            fails |= 1 << 11;
        }
    }
    // 12. The mirror image: setting the MODE must keep the autologin line.
    {
        let n = compose(b"autologin=ada\n", None, LOGIN_MODE_TYPED as i32, &mut buf);
        let want = b"autologin=ada\nlogin_mode=typed\n";
        if n != want.len() as i32 || &buf[..want.len()] != want {
            fails |= 1 << 12;
        }
    }
    // 13. ... including when autologin is off.
    {
        let n = compose(
            b"# autologin disabled\nlogin_mode=typed\n",
            None,
            LOGIN_MODE_LIST as i32,
            &mut buf,
        );
        let want = b"# autologin disabled\nlogin_mode=list\n";
        if n != want.len() as i32 || &buf[..want.len()] != want {
            fails |= 1 << 13;
        }
    }
    // 14. A preserved junk mode is normalised to what the parse rule already
    //     says it means, so the file stops carrying a value nothing accepts.
    {
        let n = compose(b"autologin=ada\nlogin_mode=wat\n", Some(b"ada\0"), -1, &mut buf);
        let want = b"autologin=ada\nlogin_mode=typed\n";
        if n != want.len() as i32 || &buf[..want.len()] != want {
            fails |= 1 << 14;
        }
    }
    // 15. A username carrying a newline is REFUSED, not written. Without this
    //     the account name is a line-injection vector into this same file.
    {
        if compose(b"", Some(b"ada\nautologin=root\0"), -1, &mut buf) != -1 {
            fails |= 1 << 15;
        }
    }
    // 16. A destination too small fails loudly rather than truncating.
    {
        let mut small: [u8; 8] = [0; 8];
        let n = unsafe {
            login_cfg_compose_rs(
                core::ptr::null(),
                0,
                b"averylongusername\0".as_ptr(),
                -1,
                small.as_mut_ptr(),
                small.len() as u32,
            )
        };
        if n != -1 {
            fails |= 1 << 16;
        }
    }
    // 17. The ABI numbers themselves. proc/syscall.h's LOGIN_MODE_* are locked
    //     to 0/1 by _Static_assert; this is the other half of that lock, on the
    //     side a C _Static_assert cannot see.
    if LOGIN_MODE_LIST != 0 || LOGIN_MODE_TYPED != 1 {
        fails |= 1 << 17;
    }

    fails
}
