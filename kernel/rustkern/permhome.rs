// rustkern/permhome.rs - #PERMSKIP: "is this permission-database key a user's
// home directory?", and nothing else.
//
// WHY THIS EXISTS
// ---------------
// fs/perms.c perms_selftest() decided that question by writing the literal
// string "/HOME/ADMIN" into the test. The first-boot wizard lets the owner
// name the account, users.c derives the home from that name (proc/users.c
// :1297), and the owner is very unlikely to be called "admin", so the test
// skipped forever on exactly the machines it was written to protect.
//
// The replacement has to answer the question by SHAPE instead of by name:
// a key is a user home if it is "/HOME/" followed by exactly one non-empty
// component with no further '/'. That is a small amount of string parsing on
// keys that ultimately derive from a Ring-3-supplied username, in Ring 0,
// which is the CWE-170 territory this project keeps moving into Rust. It is
// also a pure function, so it can be, and is, proven against vectors below
// rather than trusted.
//
// WHY RUST: CLAUDE.md's default. No performance argument applies; this runs
// once per hash-table entry at boot, over a table of at most 2048 keys.
//
// PATHS ARE UPPERCASE. fs/perms.c normalize_path() upper-cases every key
// before it is stored, so the prefix compared here is "/HOME/" and a
// lowercase "/home/x" is not a key this database can contain. The comparison
// is nonetheless case-insensitive on the prefix, because a future caller
// passing a raw path (rather than a stored key) would otherwise get a silent
// "not a home" and the self-test would go quiet again, which is the precise
// failure this module exists to end.

#![allow(dead_code)]

/// How far a caller-supplied C string is scanned for its NUL. perm_entry_t
/// keys cap at 255 bytes, so nothing reachable is longer.
const SCAN_MAX: usize = 512;

fn upper(b: u8) -> u8 {
    if b >= b'a' && b <= b'z' {
        b - 32
    } else {
        b
    }
}

/// The decision, on a byte slice with no NUL in it.
///
/// True for "/HOME/JAMES", "/HOME/a". False for "/HOME", "/HOME/",
/// "/HOME/JAMES/DESKTOP", "/HOMEWORK/X", "/CONFIG", "" and anything with a
/// trailing slash. A trailing slash is rejected rather than trimmed: the
/// database never stores one (normalize_path is fed already-canonical keys and
/// perms_canon_rs strips it), so a key that has one is not a key this function
/// is entitled to interpret.
pub fn is_user_home(path: &[u8]) -> bool {
    const PREFIX: &[u8] = b"/HOME/";
    if path.len() <= PREFIX.len() {
        return false;
    }
    let mut i = 0usize;
    while i < PREFIX.len() {
        if upper(path[i]) != PREFIX[i] {
            return false;
        }
        i += 1;
    }
    // Exactly one component after the prefix, non-empty, no '/' anywhere in it.
    let mut j = PREFIX.len();
    while j < path.len() {
        if path[j] == b'/' {
            return false;
        }
        j += 1;
    }
    true
}

/// C entry point. `path` is a NUL-terminated kernel string (a perm_entry_t
/// key). Returns 1 if it names a user's home directory, 0 otherwise. A NULL
/// pointer, or a string with no NUL within SCAN_MAX bytes, returns 0.
///
/// SAFETY: `path` is read one byte at a time for at most SCAN_MAX bytes.
#[no_mangle]
pub extern "C" fn perm_home_shape_rs(path: *const u8) -> i32 {
    if path.is_null() {
        return 0;
    }
    let mut buf = [0u8; SCAN_MAX];
    let mut n = 0usize;
    unsafe {
        while n < SCAN_MAX {
            let b = core::ptr::read_volatile(path.add(n));
            if b == 0 {
                break;
            }
            buf[n] = b;
            n += 1;
        }
    }
    if n >= SCAN_MAX {
        return 0; // unterminated: refuse rather than guess
    }
    if is_user_home(&buf[0..n]) {
        1
    } else {
        0
    }
}

/// Vectors. Returns the number of FAILING cases (0 = good). Called from
/// fs/perms.c perms_selftest(), so a drift in this predicate is a loud boot
/// failure and not something discovered when a security vector quietly stops
/// selecting anything.
///
/// The negative cases matter more than the positive ones here: the bug this
/// module replaces was a test that selected NOTHING, and a shape predicate
/// that returns false for every real home would recreate it exactly.
#[no_mangle]
pub extern "C" fn permhome_selftest_rs() -> i32 {
    let mut bad = 0i32;
    let yes: [&[u8]; 5] = [
        b"/HOME/ADMIN",
        b"/HOME/JAMES",
        b"/HOME/A",
        b"/HOME/USER.NAME",
        b"/home/james", // case-insensitive prefix, see module header
    ];
    let no: [&[u8]; 10] = [
        b"/HOME",
        b"/HOME/",
        b"/HOME/JAMES/",
        b"/HOME/JAMES/DESKTOP",
        b"/HOMEWORK/X",
        b"/CONFIG",
        b"/",
        b"",
        b"HOME/JAMES",
        b"/HOME/JAMES/UIPROFIL.YML",
    ];
    let mut i = 0;
    while i < yes.len() {
        if !is_user_home(yes[i]) {
            bad += 1;
        }
        i += 1;
    }
    let mut j = 0;
    while j < no.len() {
        if is_user_home(no[j]) {
            bad += 1;
        }
        j += 1;
    }
    bad
}
