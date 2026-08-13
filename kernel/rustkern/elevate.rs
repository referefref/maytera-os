// rustkern/elevate.rs - #745 privilege elevation for SYSTEM-WIDE package
// installs: the request state machine, the text sanitiser the trusted dialog
// draws from, and the grant-coverage test.
//
// New kernel logic (there is no C twin to strangle), so Rust per the
// 2026-07-16 rule, and the same shape as spawnid.rs: the INPUTS come from
// existing, unchanged C (proc_current(), users_authenticate(), sched_now_ms())
// and the DECISION lives here.
//
// ===========================================================================
// WHAT THIS IS AND WHY IT IS NOT A DIALOG
// ---------------------------------------------------------------------------
// MayteraOS has no sudo. Until #745 a non-root account simply could not put an
// application in /APPS, and the App Store reported that refusal as a disk
// fault. Per-user install (already shipped) fixed the ordinary case with NO
// prompt at all. This module adds the ONE path that is allowed to prompt.
//
// The prompt is drawn by the COMPOSITOR, never by the requesting app. That is
// the whole security property, and it is why the state lives HERE, in the
// kernel, rather than in either userland process:
//
//   * the App Store can only OPEN a request and READ a verdict. It never sees
//     a keystroke, never sees the password, and cannot fabricate the grant.
//   * the compositor can only PEEK the open request and RESOLVE it. It cannot
//     name the account: the kernel derives the username from the REQUESTER's
//     own uid, so there is no username field anywhere in the mechanism and
//     account enumeration is structurally impossible rather than merely
//     unadvertised.
//   * neither of them holds the grant. The grant is a bounded, path-scoped
//     field on the requester's process_t, writable only from the syscall the
//     compositor gate protects.
//
// ONE AT A TIME, REFUSED NOT QUEUED. A second open() while one is live returns
// E_BUSY. Queueing would let an app stack prompts until one is approved by
// fatigue, which is the failure mode the whole design exists to avoid.
//
// THREE ATTEMPTS PER INVOCATION, and the failures are counted against a
// SEPARATE per-account counter (users.c: elev_failed_attempts), never the
// shared login counter. See the block comment in users.c for why that is not
// optional.
// ===========================================================================

// ---- states, mirrored by ELEV_ST_* in proc/elevate.h ----------------------
const ST_IDLE: u32 = 0; // nothing outstanding
const ST_OPEN: u32 = 1; // raised; the compositor should be showing the modal
const ST_GRANTED: u32 = 2; // authenticated; the requester holds a grant
const ST_DENIED: u32 = 3; // cancelled, out of attempts, expired, or requester died

// ---- refusal codes, returned negative to C -------------------------------
const E_ARG: i64 = -1; // bad arguments
const E_BUSY: i64 = -2; // another elevation prompt is already open
const E_STALE: i64 = -3; // the seq named is not the live request

pub const ATTEMPTS_MAX: u32 = 3;

// A request must be resolved inside this long or the watchdog closes it and
// drops the compositor's grab. It is generous (a human typing a password) and
// it is a WATCHDOG, not a timeout-to-approve: expiry always DENIES.
const OPEN_TTL_MS: u64 = 120_000;

// The grant handed to the requester on success. Long enough for a large
// package to be unpacked file by file, short enough that it is not a standing
// privilege. It is additionally scoped to one path prefix and one process.
pub const GRANT_TTL_MS: u64 = 180_000;

// A prompt may only be raised in RESPONSE to input the compositor actually
// dispatched to the requesting app. This is that window. See
// elev_input_recent_rs().
pub const INPUT_WINDOW_MS: u64 = 10_000;

const NAME_MAX: usize = 64;
const VER_MAX: usize = 32;
const SRC_MAX: usize = 64;
const DEST_MAX: usize = 96;

// Package-supplied text is truncated to this many CHARACTERS before it is
// handed to the trusted surface. The design puts app-supplied strings in
// exactly one place (the fact rows); this is the length bound on them.
const DISPLAY_CHARS: usize = 40;

/// The view the compositor is given. #[repr(C)]; proc/elevate.h carries a
/// _Static_assert on its size so the two definitions cannot drift.
#[repr(C)]
pub struct ElevView {
    pub seq: u64,
    pub opened_ms: u64,
    pub state: u32,
    pub req_pid: u32,
    pub req_uid: u32,
    pub attempts_used: u32,
    pub attempts_max: u32,
    pub pad: u32,
    pub name: [u8; NAME_MAX],
    pub version: [u8; VER_MAX],
    pub source: [u8; SRC_MAX],
    pub dest: [u8; DEST_MAX],
}

struct Elev {
    seq: u64,
    opened_ms: u64,
    state: u32,
    req_pid: u32,
    req_uid: u32,
    attempts_used: u32,
    name: [u8; NAME_MAX],
    version: [u8; VER_MAX],
    source: [u8; SRC_MAX],
    dest: [u8; DEST_MAX],
}

static mut G: Elev = Elev {
    seq: 0,
    opened_ms: 0,
    state: ST_IDLE,
    req_pid: 0,
    req_uid: 0,
    attempts_used: 0,
    name: [0; NAME_MAX],
    version: [0; VER_MAX],
    source: [0; SRC_MAX],
    dest: [0; DEST_MAX],
};

// Monotonic, never reused, never zero for a real request. The App Store holds
// a seq and asks about it; a seq that is not the live one gets E_STALE, so a
// verdict can never be misread as belonging to a different request.
static mut NEXT_SEQ: u64 = 1;

// ---------------------------------------------------------------------------
// Sanitising app-supplied text.
//
// Everything the requester says about its package is attacker-controlled as
// far as this module is concerned. Three rules, applied here so no drawing
// code has to remember them:
//   1. every byte below 0x20 and 0x7F is dropped, so no newline, no NUL games,
//      no terminal escape reaches a trusted surface;
//   2. bytes >= 0x80 are dropped too. The dialog draws with the shipped TTF
//      path over ASCII; letting through a byte the font renders as nothing is
//      a way to make a name LOOK like a different name;
//   3. the result is truncated to DISPLAY_CHARS with a trailing "..." so a long
//      name cannot push chrome off the panel.
// ---------------------------------------------------------------------------
fn sanitize(dst: &mut [u8], src: *const u8) {
    for b in dst.iter_mut() {
        *b = 0;
    }
    if src.is_null() || dst.len() < 8 {
        return;
    }
    let cap = dst.len() - 1;
    let limit = if cap < DISPLAY_CHARS { cap } else { DISPLAY_CHARS };
    let mut o = 0usize;
    let mut i = 0usize;
    let mut truncated = false;
    loop {
        // Hard stop: never walk more than a screenful of source even if the
        // caller handed us something unterminated. The C side bounces these
        // strings into fixed kernel buffers first, so this is belt and braces.
        if i >= 512 {
            break;
        }
        let c = unsafe { *src.add(i) };
        i += 1;
        if c == 0 {
            break;
        }
        if c < 0x20 || c >= 0x7F {
            continue;
        }
        if o >= limit {
            truncated = true;
            break;
        }
        dst[o] = c;
        o += 1;
    }
    if truncated && o + 3 <= cap {
        dst[o] = b'.';
        dst[o + 1] = b'.';
        dst[o + 2] = b'.';
        o += 3;
    }
    dst[o] = 0;
}

fn cstr_eq_prefix(path: *const u8, prefix: &[u8]) -> bool {
    for (i, p) in prefix.iter().enumerate() {
        let c = unsafe { *path.add(i) };
        if c != *p {
            return false;
        }
    }
    true
}

// ---------------------------------------------------------------------------
// FFI
// ---------------------------------------------------------------------------

/// Open an elevation request. Returns the new seq (> 0), or E_BUSY / E_ARG.
/// Every caller-supplied string is sanitised HERE, once, on the way in, so the
/// stored copy is the only thing anything downstream can ever see.
///
/// # Safety
/// The four string pointers must be NUL-terminated KERNEL buffers (the syscall
/// case bounces them out of Ring 3 before calling).
#[no_mangle]
pub unsafe extern "C" fn elev_open_rs(
    pid: u32,
    uid: u32,
    now_ms: u64,
    name: *const u8,
    version: *const u8,
    source: *const u8,
    dest: *const u8,
) -> i64 {
    let g = unsafe { &mut *core::ptr::addr_of_mut!(G) };
    if g.state == ST_OPEN {
        return E_BUSY;
    }
    if pid == 0 {
        return E_ARG;
    }
    let seq = unsafe {
        let n = core::ptr::read_volatile(core::ptr::addr_of!(NEXT_SEQ));
        core::ptr::write_volatile(core::ptr::addr_of_mut!(NEXT_SEQ), n + 1);
        n
    };
    g.seq = seq;
    g.opened_ms = now_ms;
    g.state = ST_OPEN;
    g.req_pid = pid;
    g.req_uid = uid;
    g.attempts_used = 0;
    sanitize(&mut g.name, name);
    sanitize(&mut g.version, version);
    sanitize(&mut g.source, source);
    sanitize(&mut g.dest, dest);
    seq as i64
}

/// Copy the live request out for the trusted surface. Returns 1 if a request
/// is open, else 0. The compositor gets FACTS, never geometry and never a
/// username: there is nothing here it could use to name a different account.
///
/// # Safety
/// `out` must be a writable kernel `ElevView`.
#[no_mangle]
pub unsafe extern "C" fn elev_view_rs(out: *mut ElevView) -> i32 {
    if out.is_null() {
        return 0;
    }
    let g = unsafe { &*core::ptr::addr_of!(G) };
    if g.state != ST_OPEN {
        return 0;
    }
    let v = unsafe { &mut *out };
    v.seq = g.seq;
    v.opened_ms = g.opened_ms;
    v.state = g.state;
    v.req_pid = g.req_pid;
    v.req_uid = g.req_uid;
    v.attempts_used = g.attempts_used;
    v.attempts_max = ATTEMPTS_MAX;
    v.pad = 0;
    v.name = g.name;
    v.version = g.version;
    v.source = g.source;
    v.dest = g.dest;
    1
}

/// The requester asks about its OWN request. Returns ST_* for a matching seq,
/// or E_STALE (as i32) for anything else, so a stale verdict is never read as
/// a fresh one.
#[no_mangle]
pub extern "C" fn elev_state_rs(seq: u64) -> i32 {
    let g = unsafe { &*core::ptr::addr_of!(G) };
    if seq == 0 || g.seq != seq {
        return E_STALE as i32;
    }
    g.state as i32
}

/// Which pid raised the live request (0 if none). Used by the watchdog and by
/// the syscall gate that refuses a resolve aimed at a dead requester.
#[no_mangle]
pub extern "C" fn elev_owner_pid_rs() -> u32 {
    let g = unsafe { &*core::ptr::addr_of!(G) };
    if g.state == ST_OPEN {
        g.req_pid
    } else {
        0
    }
}

/// Which uid the live request belongs to (0xFFFFFFFF if none). The kernel maps
/// this to a username itself; the compositor never supplies one.
#[no_mangle]
pub extern "C" fn elev_owner_uid_rs() -> u32 {
    let g = unsafe { &*core::ptr::addr_of!(G) };
    if g.state == ST_OPEN {
        g.req_uid
    } else {
        0xFFFF_FFFF
    }
}

/// Record one authentication attempt against the live request.
/// `ok != 0` closes it GRANTED. A failure returns the number of attempts LEFT;
/// reaching zero closes it DENIED. Returns E_STALE for a seq that is not live.
#[no_mangle]
pub extern "C" fn elev_attempt_rs(seq: u64, ok: u32) -> i32 {
    let g = unsafe { &mut *core::ptr::addr_of_mut!(G) };
    if g.state != ST_OPEN || g.seq != seq {
        return E_STALE as i32;
    }
    if ok != 0 {
        g.state = ST_GRANTED;
        return 0;
    }
    g.attempts_used += 1;
    if g.attempts_used >= ATTEMPTS_MAX {
        g.state = ST_DENIED;
        return 0;
    }
    (ATTEMPTS_MAX - g.attempts_used) as i32
}

/// Close the live request without authenticating (Cancel, Esc, lockout-Close).
#[no_mangle]
pub extern "C" fn elev_cancel_rs(seq: u64) -> i32 {
    let g = unsafe { &mut *core::ptr::addr_of_mut!(G) };
    if g.state != ST_OPEN || g.seq != seq {
        return E_STALE as i32;
    }
    g.state = ST_DENIED;
    0
}

/// Drop the record once the requester has read its verdict, so a later
/// request is not refused by a corpse. Only the owning seq may do this.
#[no_mangle]
pub extern "C" fn elev_reap_rs(seq: u64) -> i32 {
    let g = unsafe { &mut *core::ptr::addr_of_mut!(G) };
    if g.seq != seq || g.state == ST_OPEN {
        return E_STALE as i32;
    }
    g.state = ST_IDLE;
    g.req_pid = 0;
    0
}

/// WATCHDOG. Called from the syscall paths that observe the request.
/// `requester_alive == 0` means the process that raised it is gone.
/// Returns 1 if this call closed the request (the compositor must then drop
/// its grab and scrim), else 0.
///
/// A crashed requester must never leave an undismissable scrim over an
/// unusable desktop, and an expiry must always DENY: there is deliberately no
/// timeout-to-approve anywhere in this mechanism.
#[no_mangle]
pub extern "C" fn elev_tick_rs(now_ms: u64, requester_alive: u32) -> i32 {
    let g = unsafe { &mut *core::ptr::addr_of_mut!(G) };
    if g.state != ST_OPEN {
        return 0;
    }
    if requester_alive == 0 || now_ms.wrapping_sub(g.opened_ms) > OPEN_TTL_MS {
        g.state = ST_DENIED;
        return 1;
    }
    0
}

/// Is `path` covered by a grant issued for `prefix`?
///
/// FAIL CLOSED, and deliberately cruder than a canonicaliser: any path that is
/// not a plain absolute path under `prefix` is refused. A "." or ".." element,
/// an empty element ("//"), a relative path or a non-NUL-terminated run is a
/// REFUSAL, not something to normalise. A grant is a narrow, short-lived
/// exception to the permission model; the one thing it must never do is be
/// talked into covering /CONFIG/SHADOW by a string trick.
///
/// # Safety
/// `path` must be a NUL-terminated kernel buffer.
#[no_mangle]
pub unsafe extern "C" fn elev_path_covered_rs(path: *const u8, prefix: *const u8) -> i32 {
    if path.is_null() || prefix.is_null() {
        return 0;
    }
    if unsafe { *path } != b'/' || unsafe { *prefix } != b'/' {
        return 0;
    }
    // Refuse anything with a dot element or an empty element, anywhere.
    let mut i = 0usize;
    let mut elem_len = 0usize;
    let mut dots = 0usize;
    loop {
        if i >= 512 {
            return 0; // unterminated: refuse
        }
        let c = unsafe { *path.add(i) };
        if c == 0 || c == b'/' {
            if i > 0 && elem_len == 0 {
                return 0; // "//" or a trailing "/"
            }
            if dots > 0 && dots == elem_len {
                return 0; // "." or ".."
            }
            if c == 0 {
                break;
            }
            elem_len = 0;
            dots = 0;
        } else {
            if c < 0x20 || c >= 0x7F {
                return 0;
            }
            if c == b'.' {
                dots += 1;
            }
            elem_len += 1;
        }
        i += 1;
    }
    let plen = {
        let mut n = 0usize;
        while n < 128 && unsafe { *prefix.add(n) } != 0 {
            n += 1;
        }
        n
    };
    if plen == 0 || plen > i {
        return 0;
    }
    let pslice = unsafe { core::slice::from_raw_parts(prefix, plen) };
    if !cstr_eq_prefix(path, pslice) {
        return 0;
    }
    // The prefix must end on a COMPONENT BOUNDARY, so "/APPS" covers "/APPS"
    // and "/APPS/x" and never "/APPSTORE".
    //
    // The prefix directory ITSELF is covered, and that is required rather than
    // generous: creating /APPS/FOO is a WRITE TO /APPS, and both
    // pkg_write_permit() (via sc_parent_of) and mkdir ask perms_check() about
    // the parent. A grant that covered only paths strictly BELOW the prefix
    // would have passed every member write and refused every create, which is
    // the shape of bug that looks like "the install half worked".
    let sep = unsafe { *path.add(plen) };
    if sep != 0 && sep != b'/' {
        return 0;
    }
    1
}

/// Boot self-test. Returns 0 on success, or the number of the failing case.
/// Wired into the kernel's Rust self-test sweep: a gate only ever seen green
/// is not evidence, so every refusal below is asserted, not assumed.
#[no_mangle]
pub extern "C" fn elevate_selftest_rs() -> u32 {
    // -- path coverage --------------------------------------------------
    let pfx = b"/APPS\0";
    let cases: [(&[u8], i32); 9] = [
        (b"/APPS/KRITA\0", 1),
        (b"/APPS/sub/dir/x\0", 1),
        (b"/APPS\0", 1),           // the prefix directory itself: creating in it IS writing it
        (b"/APPSTORE/X\0", 0),     // component boundary
        (b"/APPS/../CONFIG/SHADOW\0", 0),
        (b"/APPS/./X\0", 0),
        (b"/APPS//X\0", 0),
        (b"APPS/X\0", 0),
        (b"/CONFIG/SHADOW\0", 0),
    ];
    let mut n = 1u32;
    for (p, want) in cases.iter() {
        let got = unsafe { elev_path_covered_rs(p.as_ptr(), pfx.as_ptr()) };
        if got != *want {
            return n;
        }
        n += 1;
    }

    // -- sanitiser ------------------------------------------------------
    let mut buf = [0u8; NAME_MAX];
    sanitize(&mut buf, b"ok\x07\nname\0".as_ptr());
    if &buf[..7] != b"okname\0" {
        return 100;
    }
    let long = b"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\0"; // 52 A's
    sanitize(&mut buf, long.as_ptr());
    if buf[DISPLAY_CHARS] != b'.' || buf[DISPLAY_CHARS + 2] != b'.' || buf[DISPLAY_CHARS + 3] != 0 {
        return 101;
    }

    // -- state machine: one at a time, three attempts, expiry denies ----
    let saved_state = unsafe { core::ptr::addr_of!(G).read().state };
    if saved_state == ST_OPEN {
        return 0; // never disturb a live prompt to run a self-test
    }
    let s = unsafe {
        elev_open_rs(
            42,
            1000,
            1_000,
            b"Pkg\0".as_ptr(),
            b"1.0\0".as_ptr(),
            b"store\0".as_ptr(),
            b"/APPS\0".as_ptr(),
        )
    };
    if s <= 0 {
        return 200;
    }
    let s2 = unsafe {
        elev_open_rs(43, 1000, 1_000, b"P\0".as_ptr(), b"1\0".as_ptr(), b"s\0".as_ptr(), b"/APPS\0".as_ptr())
    };
    if s2 != E_BUSY {
        return 201;
    }
    if elev_attempt_rs(s as u64, 0) != 2 {
        return 202;
    }
    if elev_attempt_rs(s as u64, 0) != 1 {
        return 203;
    }
    if elev_attempt_rs(s as u64, 0) != 0 {
        return 204;
    }
    if elev_state_rs(s as u64) != ST_DENIED as i32 {
        return 205;
    }
    if elev_attempt_rs(s as u64, 1) != E_STALE as i32 {
        return 206; // a closed request cannot be re-opened by a late success
    }
    if elev_reap_rs(s as u64) != 0 {
        return 207;
    }
    // expiry denies, never approves
    let s3 = unsafe {
        elev_open_rs(44, 1000, 0, b"P\0".as_ptr(), b"1\0".as_ptr(), b"s\0".as_ptr(), b"/APPS\0".as_ptr())
    };
    if s3 <= 0 {
        return 208;
    }
    if elev_tick_rs(OPEN_TTL_MS + 1, 1) != 1 {
        return 209;
    }
    if elev_state_rs(s3 as u64) != ST_DENIED as i32 {
        return 210;
    }
    let _ = elev_reap_rs(s3 as u64);
    // a dead requester closes it too
    let s4 = unsafe {
        elev_open_rs(45, 1000, 0, b"P\0".as_ptr(), b"1\0".as_ptr(), b"s\0".as_ptr(), b"/APPS\0".as_ptr())
    };
    if s4 <= 0 {
        return 211;
    }
    if elev_tick_rs(1, 0) != 1 {
        return 212;
    }
    let _ = elev_reap_rs(s4 as u64);
    0
}
