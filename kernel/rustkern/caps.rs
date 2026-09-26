// rustkern/caps.rs - Stage 1 of the system capability API
// (docs/SYSTEM_CAPABILITY_API.md sections 4-8, Stage 1). THE ONE DEFINITION of
// the grant model, the syscall->capability REQUIREMENT TABLE, and the consent
// state machine, plus the self-test that proves every refusal fires.
//
// New kernel logic, so Rust per the 2026-07-16 rule. Stage 0 (capgate.rs)
// SUBTRACTED ambient power by closing five ungated holes at the dispatcher
// chokepoint. Stage 1 builds the POSITIVE machinery on that enforcement point:
// a capability is re-issued only under a grant that is explicit, scoped,
// time-bounded, revocable and recorded.
//
// ===========================================================================
// WHY A GRANT ON process_t, NOT A SIGNED BEARER TOKEN (design section 5.1)
// ---------------------------------------------------------------------------
// A grant is state the KERNEL writes and Ring 3 has NO syscall to write,
// exactly like elev_grant_until_ms (proc/process.h). It is unforgeable by
// construction rather than by cryptography (no key to store, no signature to
// get wrong, no repeat of #510), revocation bites IN FLIGHT (a field the kernel
// clears is revoked at the holder's next syscall), it cannot leak (there is no
// bearer artifact to paste into an LLM prompt), and it dies with the process
// for free because proc_create() already memset()s process_t. What it gives up,
// stated honestly: it cannot be delegated and cannot survive a reboot. Both are
// the right limitation - a capability that outlives the process without the
// user being asked again is one nobody remembers granting.
//
// ===========================================================================
// THE GRANT IS THE STORAGE, THE RULES ARE HERE
// ---------------------------------------------------------------------------
// The grant array lives ON process_t (C), like the #745 elevation grant. This
// file holds the DECISIONS about it (find/issue/cover/consume/revoke/query) and
// operates on a raw pointer to that array, so the storage is C and the policy
// is Rust - the same split elevate.rs uses (state machine in Rust, the field
// write in elevate.c).
//
// ===========================================================================
// CONSENT REUSES #745's PRIMITIVES (design section 6)
// ---------------------------------------------------------------------------
// The consent slot below is the elevate.rs single-slot shape applied to a
// capability decision, reusing the SAME trust primitives, unchanged:
//   * the COMPOSITOR draws the prompt and resolves it (fb_owner_is, the
//     unimpersonatable first-framebuffer-mapper latch). The requesting app
//     never draws it and cannot resolve it.
//   * a prompt may be raised only in RESPONSE to recent real input to a window
//     the requester owns (elev_last_input_ms). After Stage 0 gated
//     SYS_INJECT_KEY to the compositor, an ordinary app can no longer
//     manufacture that input credit, so the requirement is sound here.
//   * ONE prompt at a time, system-wide, refused not queued. This slot is
//     cross-excluded with the elevate slot in both directions (see
//     cap_req_open_rs and the elevate.c edit), so an install prompt and a
//     capability prompt cannot both be open.
// A capability consent is a CONSENT decision, not an AUTHENTICATION: unlike
// elevation it needs no password (the question is "allow Foo to see your
// screen", not "prove you are an admin"). Stage 2 (design section 12) merges
// this slot into a generalised elevate.rs with N scope kinds and the manager;
// Stage 1 keeps it minimal and does not touch the shipping install ABI.

#![allow(dead_code)]

use core::ptr;
use core::sync::atomic::{AtomicU32, Ordering};

// ---------------------------------------------------------------------------
// Capability classes (design section 4.2). Named for what they let you do to
// somebody else, not for the device. DEFINED so enumeration is honest that the
// surface exists and is refused (CT_DENIED), but only screen.capture is
// ISSUABLE in Stage 1 - the others return CAP_EPOLICY (principle 7: a
// capability with no consumer becomes fiction).
pub const CAP_NONE: u32 = 0;
pub const CAP_INPUT_INJECT: u32 = 1;
pub const CAP_INPUT_OBSERVE: u32 = 2;
pub const CAP_SCREEN_CAPTURE: u32 = 3;
pub const CAP_SCREEN_STREAM: u32 = 4;
pub const CAP_AUDIO_OUTPUT: u32 = 5;
pub const CAP_SERIAL_PORT: u32 = 6;
pub const CAP_NET_CONNECT: u32 = 7;
pub const CAP_NET_LISTEN: u32 = 8;
pub const CAP_CLASS_MAX: u32 = 9;

// Scope kinds. The noun the grant is bound to (design section 4.3).
pub const CAP_SCOPE_NONE: u32 = 0;
pub const CAP_SCOPE_PATH: u32 = 1; // screen.capture: the output file path
pub const CAP_SCOPE_PORT: u32 = 2; // serial.port: a published port name
pub const CAP_SCOPE_WINDOW: u32 = 3; // input.inject: a window the caller owns (token "self")
pub const CAP_SCOPE_WINDOW_TARGET: u32 = 4; // input.inject: a consented window the caller does NOT own (Stage 4)

// Grant array size on process_t. Small and fixed: no allocation, no lock, no
// cleanup path, no failure under memory pressure. 8 covers every case in
// section 4; a ninth is refused (CAP_EMAX) and the refusal is journalled.
pub const CAP_MAX_GRANTS: u32 = 8;

// Consent-request states, mirrored by CAP_ST_* in proc/caps.h.
pub const CAP_ST_IDLE: u32 = 0;
pub const CAP_ST_OPEN: u32 = 1;
pub const CAP_ST_GRANTED: u32 = 2;
pub const CAP_ST_DENIED: u32 = 3;

// Refusal codes, negative, all distinguishable (design section 8.1). "It
// failed" is the message this whole design exists to delete.
pub const CAP_EARG: i64 = -1; // malformed request
pub const CAP_EBUSY: i64 = -2; // a prompt is already open: REFUSED, never queued
pub const CAP_EDENIED: i64 = -3; // the user said no, or no grant is held
pub const CAP_ENOINPUT: i64 = -4; // spontaneous request: no recent dispatched input
pub const CAP_ENOCONSENT: i64 = -5; // no compositor exists to ask
pub const CAP_ESCOPE: i64 = -6; // scope not valid / not in the kernel's terms
pub const CAP_EPOLICY: i64 = -7; // refused by standing policy (not issuable in stage 1)
pub const CAP_EMAX: i64 = -8; // grant table full
pub const CAP_ESTALE: i64 = -9; // that seq is not the live request

// Consent-request watchdog / grant lifetimes. A WATCHDOG, never a
// timeout-to-approve: expiry always DENIES.
const OPEN_TTL_MS: u64 = 120_000;
// The longest a single grant may last. A request for longer is clamped, never
// refused, and there is no "forever": design section 5.1 and 8.2 (no "always"
// in stage 1).
pub const GRANT_MAX_TTL_MS: u64 = 900_000; // 15 minutes
// Input-credit window: a prompt may only follow real input inside this. Same
// value and meaning as elevate.rs INPUT_WINDOW_MS.
pub const INPUT_WINDOW_MS: u64 = 10_000;

const SCOPE_MAX: usize = 64;
const REASON_MAX: usize = 120;
const APP_MAX: usize = 64;

// The syscall number that requires screen.capture. Locked to
// SYS_SCREENSHOT_REQUEST in proc/syscall.h by syscall-cap-lint (which reads the
// number out of the header and diffs it against the [CAP] manifest line) and by
// the self-test below. A literal here that drifts from the header is exactly
// what the lint exists to catch.
const SYS_SCREENSHOT_REQUEST_NUM: u64 = 435;
// The syscall number that requires serial.port. Locked to SYS_SERIAL_OPEN in
// proc/syscall.h by syscall-cap-lint the same way SYS_SCREENSHOT_REQUEST_NUM is.
const SYS_SERIAL_OPEN_NUM: u64 = 438;
// The two syscall numbers that require input.inject (Stage 3). Locked to
// SYS_CAP_INJECT_KEY / SYS_CAP_INJECT_MOUSE in proc/syscall.h by
// syscall-cap-lint the same way the two above are.
const SYS_CAP_INJECT_KEY_NUM: u64 = 439;
const SYS_CAP_INJECT_MOUSE_NUM: u64 = 440;
// #469 AI-VISION: the RELEASE half of the same contract, gated identically.
const SYS_CAP_INJECT_KEY_UP_NUM: u64 = 460;

// ---------------------------------------------------------------------------
// The grant, mirrored by cap_grant_t in proc/process.h. #[repr(C)], size-locked
// on both sides (a drift silently mis-validates a security field).
// ---------------------------------------------------------------------------
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CapGrant {
    pub cap: u32,        // CAP_* class, 0 = empty slot
    pub scope_kind: u32, // CAP_SCOPE_*
    pub expires_ms: u64, // absolute deadline; a grant is always time-bounded
    pub uses_left: u32,  // remaining uses; U32_MAX = unlimited within the window
    pub pad: u32,
    pub granted_seq: u64, // the journal edge (GFSJ_OP_EDGE_ADD) that created it
    pub scope: [u8; SCOPE_MAX], // kernel-validated noun, never the app's raw string
}

pub const CAP_USES_UNLIMITED: u32 = 0xFFFF_FFFF;

// CAP_QUERY out, mirrored by cap_state_t in proc/caps.h.
#[repr(C)]
pub struct CapState {
    pub cap: u32,
    pub held: u32,
    pub expires_ms: u64,
    pub uses_left: u32,
    pub scope_kind: u32,
    pub granted_seq: u64,
    pub scope: [u8; SCOPE_MAX],
}

// CAP_VIEW out (compositor), mirrored by cap_view_t in proc/caps.h.
#[repr(C)]
pub struct CapView {
    pub seq: u64,
    pub opened_ms: u64,
    pub state: u32,
    pub req_pid: u32,
    pub req_uid: u32,
    pub cap: u32,
    pub duration_ms: u32,
    pub scope_kind: u32,
    pub app: [u8; APP_MAX],
    pub reason: [u8; REASON_MAX],
    pub scope: [u8; SCOPE_MAX],
}

// What the kernel needs to issue a grant when the compositor approves. Not an
// ABI struct: it never crosses the syscall boundary. Mirrored by
// cap_approve_info_t in proc/caps.h so the C resolver can read it.
#[repr(C)]
pub struct CapApproveInfo {
    pub req_pid: u32,
    pub req_uid: u32,
    pub cap: u32,
    pub duration_ms: u32,
    pub scope_kind: u32,
    pub pad: u32,
    pub scope: [u8; SCOPE_MAX],
}

// ---------------------------------------------------------------------------
// Requirement table + policy
// ---------------------------------------------------------------------------

/// THE REQUIREMENT TABLE. Which capability class a syscall requires, or
/// CAP_NONE. This is the positive twin of argtab.rs: the one place that says a
/// syscall is gated, read at the dispatcher chokepoint. Stage 1 gates exactly
/// one syscall (SYS_SCREENSHOT_REQUEST -> screen.capture); the syscall-cap-lint
/// makes a NEW syscall with no capability classification fail the build, so the
/// gate cannot silently omit one.
#[no_mangle]
pub extern "C" fn cap_required_for_syscall(num: u64) -> u32 {
    match num {
        SYS_SCREENSHOT_REQUEST_NUM => CAP_SCREEN_CAPTURE,
        SYS_SERIAL_OPEN_NUM => CAP_SERIAL_PORT,
        SYS_CAP_INJECT_KEY_NUM => CAP_INPUT_INJECT,
        SYS_CAP_INJECT_MOUSE_NUM => CAP_INPUT_INJECT,
        SYS_CAP_INJECT_KEY_UP_NUM => CAP_INPUT_INJECT,
        _ => CAP_NONE,
    }
}

/// Is `cap` issuable in Stage 1? Only screen.capture. Everything else is
/// defined (enumeration honesty) but refused with CAP_EPOLICY until a
/// first-party consumer exists (principle 7).
#[no_mangle]
pub extern "C" fn cap_is_issuable(cap: u32) -> i32 {
    // Stage 1 wired screen.capture. Stage 2 adds serial.port (a first-party
    // consumer exists: the mediated gateway drivers/serialport.c, and /APPS/
    // PRINT3D routes through it once the raw /dev/ttyACM0 seed is tightened).
    // Stage 3 wired input.inject (a first-party consumer exists: the mediated
    // SYS_CAP_INJECT_* path, scope kind CAP_SCOPE_WINDOW, restricted to a window
    // the grantee owns; cross-application CAP_TARGET_FOCUSED injection is
    // deliberately deferred, see the module note).
    if cap == CAP_SCREEN_CAPTURE || cap == CAP_SERIAL_PORT || cap == CAP_INPUT_INJECT {
        1
    } else {
        0
    }
}

// Is `s` a plain absolute path: starts '/', no "."/".." element, no empty
// element, printable ASCII only, NUL-terminated within `SCOPE_MAX`. This is
// elev_path_covered_rs()'s plain-path discipline, standalone, because a scope
// is a narrow exception to the permission model and must never be talked into
// covering /CONFIG/SHADOW by a string trick. FAIL CLOSED.
fn path_is_plain_abs(path: *const u8) -> bool {
    if path.is_null() {
        return false;
    }
    if unsafe { *path } != b'/' {
        return false;
    }
    let mut i = 0usize;
    let mut elem_len = 0usize;
    let mut dots = 0usize;
    loop {
        if i >= SCOPE_MAX {
            return false; // unterminated within the scope buffer: refuse
        }
        let c = unsafe { *path.add(i) };
        if c == 0 || c == b'/' {
            if i > 0 && elem_len == 0 {
                return false; // "//" or trailing "/"
            }
            if dots > 0 && dots == elem_len {
                return false; // "." or ".."
            }
            if c == 0 {
                if i < 2 {
                    return false; // "/" alone is not a capture target
                }
                break;
            }
            elem_len = 0;
            dots = 0;
        } else {
            if c < 0x20 || c >= 0x7F {
                return false;
            }
            if c == b'.' {
                dots += 1;
            }
            elem_len += 1;
        }
        i += 1;
    }
    true
}

// The input.inject window scope: the fixed token "self", meaning "windows this
// process owns". There is no per-window string in the grant: the concrete
// ownership check (owner_pid == caller) is done in C against the live window
// table at inject time, because a window handle is only meaningful while the
// window is alive and the grant may outlive any single window. Keeping the
// noun a fixed token makes the grant row displayable (scope=self) and keeps
// this a SHAPE check. Cross-app CAP_TARGET_FOCUSED is deferred (module note).
// FAIL CLOSED.
fn scope_is_self_token(scope: *const u8) -> bool {
    if scope.is_null() {
        return false;
    }
    let want = b"self";
    let mut i = 0usize;
    while i < want.len() {
        if unsafe { *scope.add(i) } != want[i] {
            return false;
        }
        i += 1;
    }
    unsafe { *scope.add(want.len()) == 0 } // exactly "self", NUL-terminated
}

// Cross-app target scope (Stage 4). The bound noun is "<winid>[:<title>]": one
// or more leading ASCII decimal digits (the stable window id), optionally a ':'
// then a printable-ASCII title. The app's RAW request scope is just "<handle>"
// (the leading-digits form, no title), which also passes; the C side rewrites
// it to the id:title form at request time. NUL-terminated within SCOPE_MAX.
// FAIL CLOSED.
fn window_target_shape_ok(scope: *const u8) -> bool {
    if scope.is_null() {
        return false;
    }
    let mut i = 0usize;
    let mut digits = 0usize;
    while i < SCOPE_MAX {
        let c = unsafe { *scope.add(i) };
        if c >= b'0' && c <= b'9' {
            digits += 1;
            i += 1;
            if digits > 20 {
                return false; // a u64 is at most 20 decimal digits
            }
        } else {
            break;
        }
    }
    if digits == 0 {
        return false; // must start with a window id
    }
    if i >= SCOPE_MAX {
        return false; // unterminated
    }
    let c = unsafe { *scope.add(i) };
    if c == 0 {
        return true; // bare "<winid>"
    }
    if c != b':' {
        return false;
    }
    i += 1;
    while i < SCOPE_MAX {
        let t = unsafe { *scope.add(i) };
        if t == 0 {
            return true;
        }
        if t < 0x20 || t >= 0x7F {
            return false;
        }
        i += 1;
    }
    false // unterminated title
}

// Parse the leading ASCII decimal id from a scope buffer, up to ':' or NUL.
// Returns 0 when there is no leading digit; a real window id starts at 1, so 0
// never matches a live window.
fn scope_leading_u64(scope: &[u8; SCOPE_MAX]) -> u64 {
    let mut v: u64 = 0;
    let mut i = 0usize;
    let mut any = false;
    while i < SCOPE_MAX {
        let c = scope[i];
        if c >= b'0' && c <= b'9' {
            v = v.wrapping_mul(10).wrapping_add((c - b'0') as u64);
            any = true;
            i += 1;
        } else {
            break;
        }
    }
    if any {
        v
    } else {
        0
    }
}

// A serial port name: 1..15 chars of [A-Za-z0-9], NUL-terminated within the
// SERIALPORT_NAME_MAX (16) buffer. NO '/', no '.', no path structure at all: a
// port is a bare token, not a path, so a name can never be talked into naming a
// device path or a traversal. The kernel-owned enumeration membership check
// (serialport_is_published) is done in C at request time; this validates SHAPE.
// FAIL CLOSED.
fn port_name_is_plain(name: *const u8) -> bool {
    if name.is_null() {
        return false;
    }
    let mut i = 0usize;
    loop {
        if i >= 16 {
            return false; // SERIALPORT_NAME_MAX: unterminated within it => refuse
        }
        let c = unsafe { *name.add(i) };
        if c == 0 {
            return i >= 1; // at least one character
        }
        let ok = (c >= b'a' && c <= b'z')
            || (c >= b'A' && c <= b'Z')
            || (c >= b'0' && c <= b'9');
        if !ok {
            return false;
        }
        i += 1;
    }
}

/// Validate the scope noun for `cap`. 1 = valid, 0 = refused. For screen.capture
/// the noun is a plain absolute PATH (the output file). Other caps are not
/// issuable in stage 1, so their scope is refused here too.
#[no_mangle]
pub extern "C" fn cap_scope_valid(cap: u32, scope_kind: u32, scope: *const u8) -> i32 {
    match cap {
        // screen.capture: the noun is a plain absolute PATH (the output file).
        CAP_SCREEN_CAPTURE => {
            if scope_kind != CAP_SCOPE_PATH {
                return 0;
            }
            if path_is_plain_abs(scope) {
                1
            } else {
                0
            }
        }
        // serial.port: the noun is a plain PORT NAME (a published port). The
        // membership of the kernel's live enumeration is confirmed in C
        // (serialport_is_published) before the prompt opens; here we refuse any
        // name that is not a bare token.
        CAP_SERIAL_PORT => {
            if scope_kind != CAP_SCOPE_PORT {
                return 0;
            }
            if port_name_is_plain(scope) {
                1
            } else {
                0
            }
        }
        // input.inject: the noun is the fixed window scope token "self"
        // (scope kind CAP_SCOPE_WINDOW). The per-window ownership check is done
        // in C at inject time; here we validate SHAPE only.
        CAP_INPUT_INJECT => {
            // Self scope (Stage 3): the fixed token "self", scope kind WINDOW.
            if scope_kind == CAP_SCOPE_WINDOW {
                return if scope_is_self_token(scope) { 1 } else { 0 };
            }
            // Cross-app target scope (Stage 4): a window HANDLE the app names (a
            // bare decimal), which the C side resolves to a stable window id and
            // title and REWRITES to the bound "<winid>:<title>" form. Both the
            // raw handle and the bound form pass this SHAPE check; the
            // kernel-owned validity/membership (a live, non-compositor window)
            // is confirmed in C at request time, exactly as serial.port's
            // published-port membership is.
            if scope_kind == CAP_SCOPE_WINDOW_TARGET {
                return if window_target_shape_ok(scope) { 1 } else { 0 };
            }
            0
        }
        // Not issuable in stage 1/2: scope refused too.
        _ => 0,
    }
}

// bytewise equality of a scope buffer against a NUL-terminated path, up to the
// first NUL, both bounded by SCOPE_MAX.
fn scope_eq(scope: &[u8; SCOPE_MAX], path: *const u8) -> bool {
    let mut i = 0usize;
    loop {
        if i >= SCOPE_MAX {
            return false;
        }
        let a = scope[i];
        let b = unsafe { *path.add(i) };
        if a != b {
            return false;
        }
        if a == 0 {
            return true;
        }
        i += 1;
    }
}

// ---------------------------------------------------------------------------
// Grant array operations. `grants` is process_t.cap_grants; `len` is
// CAP_MAX_GRANTS. All refuse an empty (cap==0) slot and a null pointer.
// ---------------------------------------------------------------------------

#[inline]
fn grant_is_live(g: &CapGrant, now_ms: u64) -> bool {
    if g.cap == 0 {
        return false;
    }
    if now_ms > g.expires_ms {
        return false;
    }
    g.uses_left != 0
}

/// Index of a live grant of `cap`, else -1. CLASS level: does the caller hold
/// this capability at all right now. This is what the dispatcher chokepoint
/// asks; the fine-grained scope match is cap_covers_path below.
///
/// # Safety
/// `grants` points at `len` contiguous CapGrant on the current process_t.
#[no_mangle]
pub unsafe extern "C" fn cap_find_live(
    grants: *const CapGrant,
    len: u32,
    cap: u32,
    now_ms: u64,
) -> i32 {
    if grants.is_null() || cap == 0 {
        return -1;
    }
    let mut i = 0u32;
    while i < len {
        let g = unsafe { &*grants.add(i as usize) };
        if g.cap == cap && grant_is_live(g, now_ms) {
            return i as i32;
        }
        i += 1;
    }
    -1
}

/// Index of a live grant of `cap` whose scope EXACTLY covers `path`, else -1.
/// Exact match is the tightest reading of "a path the caller could already
/// write": the app asked to capture to exactly this file and the user consented
/// to exactly it. A prefix match would let one grant cover a directory of
/// files; that is a widening the user did not agree to.
///
/// # Safety
/// `grants` and `path` are valid kernel pointers; `path` is NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn cap_covers_path(
    grants: *const CapGrant,
    len: u32,
    cap: u32,
    path: *const u8,
    now_ms: u64,
) -> i32 {
    if grants.is_null() || path.is_null() || cap == 0 {
        return -1;
    }
    let mut i = 0u32;
    while i < len {
        let g = unsafe { &*grants.add(i as usize) };
        if g.cap == cap
            && g.scope_kind == CAP_SCOPE_PATH
            && grant_is_live(g, now_ms)
            && scope_eq(&g.scope, path)
        {
            return i as i32;
        }
        i += 1;
    }
    -1
}

/// Index of a live grant of `cap` (scope kind CAP_SCOPE_PORT) whose scope names
/// exactly `port`, consuming nothing. The serial-noun twin of cap_covers_path:
/// EXACT match, never a prefix, so a grant of "ttyS1" never covers "ttyS10".
///
/// # Safety
/// `grants` points at `len` CapGrant; `port` is a NUL-terminated kernel buffer.
#[no_mangle]
pub unsafe extern "C" fn cap_covers_port(
    grants: *const CapGrant,
    len: u32,
    cap: u32,
    port: *const u8,
    now_ms: u64,
) -> i32 {
    if grants.is_null() || port.is_null() || cap == 0 {
        return -1;
    }
    let mut i = 0u32;
    while i < len {
        let g = unsafe { &*grants.add(i as usize) };
        if g.cap == cap
            && g.scope_kind == CAP_SCOPE_PORT
            && grant_is_live(g, now_ms)
            && scope_eq(&g.scope, port)
        {
            return i as i32;
        }
        i += 1;
    }
    -1
}

/// Index of a live grant of `cap` whose scope KIND is exactly `scope_kind`, else
/// -1. Stage 4 needs this to tell a self (WINDOW) grant apart from a cross-app
/// (WINDOW_TARGET) grant when a process holds BOTH: cap_find_live returns the
/// first live grant of the class regardless of scope kind, which would
/// mis-authorize a self-inject on the strength of a target-only grant.
///
/// # Safety
/// `grants` points at `len` contiguous CapGrant on the current process_t.
#[no_mangle]
pub unsafe extern "C" fn cap_find_scope(
    grants: *const CapGrant,
    len: u32,
    cap: u32,
    scope_kind: u32,
    now_ms: u64,
) -> i32 {
    if grants.is_null() || cap == 0 {
        return -1;
    }
    let mut i = 0u32;
    while i < len {
        let g = unsafe { &*grants.add(i as usize) };
        if g.cap == cap && g.scope_kind == scope_kind && grant_is_live(g, now_ms) {
            return i as i32;
        }
        i += 1;
    }
    -1
}

/// Index of a live CAP_SCOPE_WINDOW_TARGET grant of `cap` whose bound window id
/// EXACTLY equals `target_id`, else -1. The Stage 4 cross-app twin of
/// cap_covers_port: an exact id match, never a prefix, so a grant bound to
/// window 12 never covers window 120, and a grant whose target window closed
/// (its slot later reused by a NEW window with a fresh monotonic id) never
/// covers the new one. A `target_id` of 0 (a dead/absent window) matches nothing.
///
/// # Safety
/// `grants` points at `len` CapGrant on the current process_t.
#[no_mangle]
pub unsafe extern "C" fn cap_covers_window_target(
    grants: *const CapGrant,
    len: u32,
    cap: u32,
    target_id: u64,
    now_ms: u64,
) -> i32 {
    if grants.is_null() || cap == 0 || target_id == 0 {
        return -1;
    }
    let mut i = 0u32;
    while i < len {
        let g = unsafe { &*grants.add(i as usize) };
        if g.cap == cap
            && g.scope_kind == CAP_SCOPE_WINDOW_TARGET
            && grant_is_live(g, now_ms)
            && scope_leading_u64(&g.scope) == target_id
        {
            return i as i32;
        }
        i += 1;
    }
    -1
}

/// Issue a grant. Reuses an existing slot for the same (cap, scope) if one
/// exists (a re-grant refreshes rather than piling up), else the first free
/// slot. Returns the slot index, or -1 (CAP_EMAX: table full).
///
/// # Safety
/// `grants` points at `len` writable CapGrant; `scope` is NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn cap_issue(
    grants: *mut CapGrant,
    len: u32,
    cap: u32,
    scope_kind: u32,
    scope: *const u8,
    expires_ms: u64,
    uses: u32,
    seq: u64,
) -> i32 {
    if grants.is_null() || cap == 0 {
        return -1;
    }
    // First pass: an existing slot for the same cap+scope to refresh.
    let mut free = -1i32;
    let mut i = 0u32;
    while i < len {
        let g = unsafe { &*grants.add(i as usize) };
        if g.cap == cap && g.scope_kind == scope_kind && scope_eq(&g.scope, scope) {
            free = i as i32;
            break;
        }
        if g.cap == 0 && free < 0 {
            free = i as i32;
            // keep scanning for a same-scope match, which wins over a free slot
        }
        i += 1;
    }
    if free < 0 {
        return -1;
    }
    let g = unsafe { &mut *grants.add(free as usize) };
    g.cap = cap;
    g.scope_kind = scope_kind;
    g.expires_ms = expires_ms;
    g.uses_left = uses;
    g.pad = 0;
    g.granted_seq = seq;
    for b in g.scope.iter_mut() {
        *b = 0;
    }
    let mut k = 0usize;
    while k < SCOPE_MAX - 1 {
        let c = unsafe { *scope.add(k) };
        if c == 0 {
            break;
        }
        g.scope[k] = c;
        k += 1;
    }
    free
}

/// Consume one use of the grant at `idx`. If uses is unlimited, no change. If it
/// reaches zero the slot is cleared (a one-use grant is gone after its use).
/// Returns remaining uses (or CAP_USES_UNLIMITED), or -1 on a bad index/empty.
///
/// # Safety
/// `grants` points at `len` writable CapGrant.
#[no_mangle]
pub unsafe extern "C" fn cap_consume_use(grants: *mut CapGrant, len: u32, idx: u32) -> i64 {
    if grants.is_null() || idx >= len {
        return -1;
    }
    let g = unsafe { &mut *grants.add(idx as usize) };
    if g.cap == 0 {
        return -1;
    }
    if g.uses_left == CAP_USES_UNLIMITED {
        return CAP_USES_UNLIMITED as i64;
    }
    if g.uses_left > 0 {
        g.uses_left -= 1;
    }
    let left = g.uses_left;
    if left == 0 {
        clear_slot(g);
    }
    left as i64
}

fn clear_slot(g: &mut CapGrant) {
    g.cap = 0;
    g.scope_kind = 0;
    g.expires_ms = 0;
    g.uses_left = 0;
    g.pad = 0;
    g.granted_seq = 0;
    for b in g.scope.iter_mut() {
        *b = 0;
    }
}

/// Revoke every grant of `cap`. Returns the granted_seq of the first one cleared
/// (so the C side can name the edge in the GFSJ_OP_EDGE_REVOKE record), or 0 if
/// none were held. Revocation bites in flight: the very next syscall that asks
/// cap_find_live gets -1.
///
/// # Safety
/// `grants` points at `len` writable CapGrant.
#[no_mangle]
pub unsafe extern "C" fn cap_revoke(grants: *mut CapGrant, len: u32, cap: u32) -> u64 {
    if grants.is_null() || cap == 0 {
        return 0;
    }
    let mut first_seq = 0u64;
    let mut i = 0u32;
    while i < len {
        let g = unsafe { &mut *grants.add(i as usize) };
        if g.cap == cap {
            if first_seq == 0 {
                first_seq = g.granted_seq;
            }
            clear_slot(g);
        }
        i += 1;
    }
    first_seq
}

/// Fill `out` with the caller's live grant of `cap` (held=1) or held=0. Also
/// sweeps expired slots as a side effect of asking.
///
/// # Safety
/// `grants` and `out` are valid kernel pointers.
#[no_mangle]
pub unsafe extern "C" fn cap_query(
    grants: *mut CapGrant,
    len: u32,
    cap: u32,
    now_ms: u64,
    out: *mut CapState,
) -> i32 {
    if out.is_null() {
        return -1;
    }
    let mut st = CapState {
        cap,
        held: 0,
        expires_ms: 0,
        uses_left: 0,
        scope_kind: 0,
        granted_seq: 0,
        scope: [0u8; SCOPE_MAX],
    };
    if !grants.is_null() && cap != 0 {
        let mut i = 0u32;
        while i < len {
            let g = unsafe { &mut *grants.add(i as usize) };
            if g.cap != 0 && now_ms > g.expires_ms {
                clear_slot(g); // lazy expiry sweep
            }
            i += 1;
        }
        let idx = unsafe { cap_find_live(grants as *const CapGrant, len, cap, now_ms) };
        if idx >= 0 {
            let g = unsafe { &*grants.add(idx as usize) };
            st.held = 1;
            st.expires_ms = g.expires_ms;
            st.uses_left = g.uses_left;
            st.scope_kind = g.scope_kind;
            st.granted_seq = g.granted_seq;
            st.scope = g.scope;
        }
    }
    unsafe {
        ptr::write(out, st);
    }
    0
}

// ---------------------------------------------------------------------------
// Ledger: refused / allowed / requested / granted / denied, per the same
// reasoning as capgate.rs. "0 refusals" reads exactly like "not wired"; the
// pair says the gate is live. Never reset (a counter a caller can zero is one
// an attacker can zero).
// ---------------------------------------------------------------------------
pub const LEDGER_REFUSED: usize = 0; // gated use refused (no grant)
pub const LEDGER_ALLOWED: usize = 1; // gated use admitted (grant present)
pub const LEDGER_REQUESTED: usize = 2; // SYS_CAP_REQUEST reached consent
pub const LEDGER_GRANTED: usize = 3; // consent approved, grant issued
pub const LEDGER_DENIED: usize = 4; // consent denied / refused pre-prompt
pub const LEDGER_REVOKED: usize = 5;
pub const LEDGER_MAX: usize = 6;

static LEDGER: [AtomicU32; LEDGER_MAX] = [
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
    AtomicU32::new(0),
];

#[repr(C)]
pub struct CapLedger {
    pub c: [u32; LEDGER_MAX],
}

#[no_mangle]
pub extern "C" fn cap_ledger_note(kind: u32) {
    if (kind as usize) < LEDGER_MAX {
        LEDGER[kind as usize].fetch_add(1, Ordering::Relaxed);
    }
}

#[no_mangle]
pub extern "C" fn cap_ledger_get(out: *mut CapLedger) -> i32 {
    if out.is_null() {
        return -1;
    }
    let mut s = CapLedger { c: [0u32; LEDGER_MAX] };
    for i in 0..LEDGER_MAX {
        s.c[i] = LEDGER[i].load(Ordering::Relaxed);
    }
    unsafe {
        ptr::write(out, s);
    }
    0
}

// ---------------------------------------------------------------------------
// Consent state machine: the elevate.rs single-slot shape for a capability
// decision. ONE at a time, refused not queued, watchdog denies on expiry or a
// dead requester.
// ---------------------------------------------------------------------------
struct CapReq {
    seq: u64,
    opened_ms: u64,
    state: u32,
    req_pid: u32,
    req_uid: u32,
    cap: u32,
    duration_ms: u32,
    scope_kind: u32,
    scope: [u8; SCOPE_MAX],
    reason: [u8; REASON_MAX],
    app: [u8; APP_MAX],
}

static mut GREQ: CapReq = CapReq {
    seq: 0,
    opened_ms: 0,
    state: CAP_ST_IDLE,
    req_pid: 0,
    req_uid: 0,
    cap: 0,
    duration_ms: 0,
    scope_kind: 0,
    scope: [0; SCOPE_MAX],
    reason: [0; REASON_MAX],
    app: [0; APP_MAX],
};

static mut CAP_NEXT_SEQ: u64 = 1;

// Sanitise app-supplied display text: drop control bytes and >=0x80 (a byte the
// TTF path renders as nothing is a way to make a name LOOK like another), NUL
// terminate. Same three rules as elevate.rs sanitize(). This is display text
// only; the SCOPE is validated separately by path_is_plain_abs and is never
// drawn from the app's raw string.
fn sanitize(dst: &mut [u8], src: *const u8) {
    for b in dst.iter_mut() {
        *b = 0;
    }
    if src.is_null() || dst.len() < 4 {
        return;
    }
    let cap = dst.len() - 1;
    let mut o = 0usize;
    let mut i = 0usize;
    loop {
        if i >= 512 || o >= cap {
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
        dst[o] = c;
        o += 1;
    }
    dst[o] = 0;
}

fn copy_scope(dst: &mut [u8; SCOPE_MAX], src: *const u8) {
    for b in dst.iter_mut() {
        *b = 0;
    }
    if src.is_null() {
        return;
    }
    let mut i = 0usize;
    while i < SCOPE_MAX - 1 {
        let c = unsafe { *src.add(i) };
        if c == 0 {
            break;
        }
        dst[i] = c;
        i += 1;
    }
}

/// Open a capability consent request. Returns the new seq (>0) or a CAP_E*.
/// The C caller (sys_cap_request) has already: proven the caller is Ring 3,
/// checked input credit (CAP_ENOINPUT), checked a compositor exists
/// (CAP_ENOCONSENT), checked the cap is issuable (CAP_EPOLICY) and the scope is
/// valid (CAP_ESCOPE), and bounced every string into a kernel buffer. This
/// function enforces ONE-AT-A-TIME (its own slot AND the elevate slot) and
/// stores the sanitised request.
///
/// # Safety
/// `reason`, `scope`, `app` are NUL-terminated kernel buffers.
#[no_mangle]
pub unsafe extern "C" fn cap_req_open_rs(
    pid: u32,
    uid: u32,
    now_ms: u64,
    cap: u32,
    duration_ms: u32,
    scope_kind: u32,
    reason: *const u8,
    scope: *const u8,
    app: *const u8,
    elev_open: u32,
) -> i64 {
    if pid == 0 {
        return CAP_EARG;
    }
    let g = unsafe { &mut *core::ptr::addr_of_mut!(GREQ) };
    // ONE prompt at a time, SYSTEM-WIDE: our own slot, and the elevate slot
    // (elev_open is elev_owner_pid_rs() != 0, passed by the C caller). Queueing
    // would let an app stack prompts until one is approved by fatigue.
    if g.state == CAP_ST_OPEN || elev_open != 0 {
        return CAP_EBUSY;
    }
    let seq = unsafe {
        let n = core::ptr::read_volatile(core::ptr::addr_of!(CAP_NEXT_SEQ));
        core::ptr::write_volatile(core::ptr::addr_of_mut!(CAP_NEXT_SEQ), n + 1);
        n
    };
    g.seq = seq;
    g.opened_ms = now_ms;
    g.state = CAP_ST_OPEN;
    g.req_pid = pid;
    g.req_uid = uid;
    g.cap = cap;
    let dur = if duration_ms as u64 > GRANT_MAX_TTL_MS {
        GRANT_MAX_TTL_MS as u32
    } else {
        duration_ms
    };
    g.duration_ms = dur;
    g.scope_kind = scope_kind;
    copy_scope(&mut g.scope, scope);
    sanitize(&mut g.reason, reason);
    sanitize(&mut g.app, app);
    seq as i64
}

/// Copy the live request out for the compositor. 1 if open, else 0.
///
/// # Safety
/// `out` is a writable kernel CapView.
#[no_mangle]
pub unsafe extern "C" fn cap_req_view_rs(out: *mut CapView) -> i32 {
    if out.is_null() {
        return 0;
    }
    let g = unsafe { &*core::ptr::addr_of!(GREQ) };
    if g.state != CAP_ST_OPEN {
        return 0;
    }
    let v = unsafe { &mut *out };
    v.seq = g.seq;
    v.opened_ms = g.opened_ms;
    v.state = g.state;
    v.req_pid = g.req_pid;
    v.req_uid = g.req_uid;
    v.cap = g.cap;
    v.duration_ms = g.duration_ms;
    v.scope_kind = g.scope_kind;
    v.app = g.app;
    v.reason = g.reason;
    v.scope = g.scope;
    1
}

/// The requester asks about its OWN request. CAP_ST_* for a matching seq, or
/// CAP_ESTALE (as i32) for anything else.
#[no_mangle]
pub extern "C" fn cap_req_state_rs(seq: u64) -> i32 {
    let g = unsafe { &*core::ptr::addr_of!(GREQ) };
    if seq == 0 || g.seq != seq {
        return CAP_ESTALE as i32;
    }
    g.state as i32
}

#[no_mangle]
pub extern "C" fn cap_req_owner_pid_rs() -> u32 {
    let g = unsafe { &*core::ptr::addr_of!(GREQ) };
    if g.state == CAP_ST_OPEN {
        g.req_pid
    } else {
        0
    }
}

/// Is a capability prompt open? Used by the elevate cross-exclusion so an
/// install prompt is not raised over a live capability prompt.
#[no_mangle]
pub extern "C" fn cap_req_busy_rs() -> u32 {
    let g = unsafe { &*core::ptr::addr_of!(GREQ) };
    if g.state == CAP_ST_OPEN {
        1
    } else {
        0
    }
}

/// Fill the info the C resolver needs to issue the grant, for the live request
/// only. 1 on success, 0 if none open.
///
/// # Safety
/// `out` is a writable kernel CapApproveInfo.
#[no_mangle]
pub unsafe extern "C" fn cap_req_approve_info_rs(out: *mut CapApproveInfo) -> i32 {
    if out.is_null() {
        return 0;
    }
    let g = unsafe { &*core::ptr::addr_of!(GREQ) };
    if g.state != CAP_ST_OPEN {
        return 0;
    }
    let v = unsafe { &mut *out };
    v.req_pid = g.req_pid;
    v.req_uid = g.req_uid;
    v.cap = g.cap;
    v.duration_ms = g.duration_ms;
    v.scope_kind = g.scope_kind;
    v.pad = 0;
    v.scope = g.scope;
    1
}

/// Resolve the live request. `approve != 0` -> GRANTED, else DENIED. Returns 0,
/// or CAP_ESTALE (i32) for a seq that is not live. Only sets the state; the C
/// resolver reads cap_req_approve_info_rs and issues the grant, because issuing
/// touches process_t which is C.
#[no_mangle]
pub extern "C" fn cap_req_resolve_rs(seq: u64, approve: u32) -> i32 {
    let g = unsafe { &mut *core::ptr::addr_of_mut!(GREQ) };
    if g.state != CAP_ST_OPEN || g.seq != seq {
        return CAP_ESTALE as i32;
    }
    g.state = if approve != 0 { CAP_ST_GRANTED } else { CAP_ST_DENIED };
    0
}

/// Drop the record once the requester has read its verdict.
#[no_mangle]
pub extern "C" fn cap_req_reap_rs(seq: u64) -> i32 {
    let g = unsafe { &mut *core::ptr::addr_of_mut!(GREQ) };
    if g.seq != seq || g.state == CAP_ST_OPEN {
        return CAP_ESTALE as i32;
    }
    g.state = CAP_ST_IDLE;
    g.req_pid = 0;
    0
}

/// Watchdog. `requester_alive == 0` means the requester is gone. Returns 1 if
/// this call closed the request (DENIED), else 0. Expiry always DENIES.
#[no_mangle]
pub extern "C" fn cap_req_tick_rs(now_ms: u64, requester_alive: u32) -> i32 {
    let g = unsafe { &mut *core::ptr::addr_of_mut!(GREQ) };
    if g.state != CAP_ST_OPEN {
        return 0;
    }
    if requester_alive == 0 || now_ms.wrapping_sub(g.opened_ms) > OPEN_TTL_MS {
        g.state = CAP_ST_DENIED;
        return 1;
    }
    0
}

// ---------------------------------------------------------------------------
// Self-test. Every assertion is a REFUSAL or an exact-match, because a gate only
// ever seen green is not evidence (elevate.rs / capgate.rs say the same). 0 on
// pass, else the failing case number. Touches only local arrays and the request
// slot when it is idle, so it is safe on every boot including the golden.
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn caps_selftest_rs() -> u32 {
    // -- requirement table --------------------------------------------------
    if cap_required_for_syscall(SYS_SCREENSHOT_REQUEST_NUM) != CAP_SCREEN_CAPTURE {
        return 1;
    }
    if cap_required_for_syscall(SYS_SCREENSHOT_REQUEST_NUM + 1) != CAP_NONE {
        return 2;
    }
    if cap_required_for_syscall(1) != CAP_NONE {
        return 3;
    }

    // -- policy: screen.capture, serial.port, input.inject are issuable ----
    if cap_is_issuable(CAP_SCREEN_CAPTURE) != 1 {
        return 4;
    }
    if cap_is_issuable(CAP_INPUT_INJECT) != 1 {
        return 5; // Stage 3 wired it
    }
    if cap_is_issuable(CAP_NET_CONNECT) != 0 {
        return 6; // still deferred
    }

    // -- scope validation ---------------------------------------------------
    if cap_scope_valid(CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH, b"/HOME/shot.bmp\0".as_ptr()) != 1 {
        return 10;
    }
    if cap_scope_valid(CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH, b"/../CONFIG/SHADOW\0".as_ptr()) != 0 {
        return 11;
    }
    if cap_scope_valid(CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH, b"shot.bmp\0".as_ptr()) != 0 {
        return 12; // relative
    }
    if cap_scope_valid(CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH, b"/a/./b\0".as_ptr()) != 0 {
        return 13;
    }
    if cap_scope_valid(CAP_INPUT_INJECT, CAP_SCOPE_PATH, b"/x.bmp\0".as_ptr()) != 0 {
        return 14; // wrong cap
    }
    if cap_scope_valid(CAP_SCREEN_CAPTURE, CAP_SCOPE_NONE, b"/x.bmp\0".as_ptr()) != 0 {
        return 15; // wrong scope kind
    }
    // input.inject window scope: exactly the token "self", scope kind WINDOW.
    if cap_scope_valid(CAP_INPUT_INJECT, CAP_SCOPE_WINDOW, b"self\0".as_ptr()) != 1 {
        return 16;
    }
    if cap_scope_valid(CAP_INPUT_INJECT, CAP_SCOPE_WINDOW, b"other\0".as_ptr()) != 0 {
        return 17; // only the fixed token
    }
    if cap_scope_valid(CAP_INPUT_INJECT, CAP_SCOPE_WINDOW, b"selfx\0".as_ptr()) != 0 {
        return 18; // no prefix match
    }
    if cap_scope_valid(CAP_INPUT_INJECT, CAP_SCOPE_PORT, b"self\0".as_ptr()) != 0 {
        return 19; // wrong scope kind for input.inject
    }

    // -- grant array: issue / find / cover / consume / revoke ---------------
    let mut grants = [CapGrant {
        cap: 0,
        scope_kind: 0,
        expires_ms: 0,
        uses_left: 0,
        pad: 0,
        granted_seq: 0,
        scope: [0u8; SCOPE_MAX],
    }; CAP_MAX_GRANTS as usize];
    let gp = grants.as_mut_ptr();
    let n = CAP_MAX_GRANTS;
    let now = 1000u64;

    // fresh array holds nothing
    if unsafe { cap_find_live(gp, n, CAP_SCREEN_CAPTURE, now) } != -1 {
        return 20;
    }
    // issue a 2-use grant expiring at now+100
    let path = b"/HOME/shot.bmp\0";
    let idx = unsafe {
        cap_issue(gp, n, CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH, path.as_ptr(), now + 100, 2, 77)
    };
    if idx < 0 {
        return 21;
    }
    if unsafe { cap_find_live(gp, n, CAP_SCREEN_CAPTURE, now) } != idx {
        return 22;
    }
    // a DIFFERENT cap is not held
    if unsafe { cap_find_live(gp, n, CAP_AUDIO_OUTPUT, now) } != -1 {
        return 23;
    }
    // exact path covered; a different path is NOT
    if unsafe { cap_covers_path(gp, n, CAP_SCREEN_CAPTURE, path.as_ptr(), now) } != idx {
        return 24;
    }
    if unsafe { cap_covers_path(gp, n, CAP_SCREEN_CAPTURE, b"/HOME/other.bmp\0".as_ptr(), now) } != -1
    {
        return 25;
    }
    // expired: not live
    if unsafe { cap_find_live(gp, n, CAP_SCREEN_CAPTURE, now + 200) } != -1 {
        return 26;
    }
    // consume both uses; after the second the slot is gone
    if unsafe { cap_consume_use(gp, n, idx as u32) } != 1 {
        return 27;
    }
    if unsafe { cap_consume_use(gp, n, idx as u32) } != 0 {
        return 28;
    }
    if unsafe { cap_find_live(gp, n, CAP_SCREEN_CAPTURE, now) } != -1 {
        return 29; // one-use exhaustion clears it
    }

    // revoke bites: issue unlimited, confirm live, revoke, confirm gone
    let idx2 = unsafe {
        cap_issue(
            gp,
            n,
            CAP_SCREEN_CAPTURE,
            CAP_SCOPE_PATH,
            path.as_ptr(),
            now + 100,
            CAP_USES_UNLIMITED,
            88,
        )
    };
    if idx2 < 0 {
        return 30;
    }
    if unsafe { cap_find_live(gp, n, CAP_SCREEN_CAPTURE, now) } < 0 {
        return 31;
    }
    if unsafe { cap_revoke(gp, n, CAP_SCREEN_CAPTURE) } != 88 {
        return 32; // returns the revoked edge seq
    }
    if unsafe { cap_find_live(gp, n, CAP_SCREEN_CAPTURE, now) } != -1 {
        return 33; // revocation bites immediately
    }

    // table full -> EMAX. Fill all 8 with distinct scopes.
    let mut filled = 0u32;
    for i in 0..(CAP_MAX_GRANTS as usize) {
        let s = [
            b'/', b'a', b'/', (b'0' + i as u8), 0u8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        ];
        let r = unsafe {
            cap_issue(gp, n, CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH, s.as_ptr(), now + 100, 1, 100 + i as u64)
        };
        if r >= 0 {
            filled += 1;
        }
    }
    if filled != CAP_MAX_GRANTS {
        return 34;
    }
    // a NINTH distinct scope is refused
    let ninth = unsafe {
        cap_issue(
            gp,
            n,
            CAP_SCREEN_CAPTURE,
            CAP_SCOPE_PATH,
            b"/a/ninth\0".as_ptr(),
            now + 100,
            1,
            200,
        )
    };
    if ninth != -1 {
        return 35;
    }

    // -- consent slot: one at a time, expiry denies, dead requester denies --
    let saved = unsafe { core::ptr::addr_of!(GREQ).read().state };
    if saved == CAP_ST_OPEN {
        return 0; // never disturb a live prompt
    }
    let s = unsafe {
        cap_req_open_rs(
            42,
            1000,
            1000,
            CAP_SCREEN_CAPTURE,
            60000,
            CAP_SCOPE_PATH,
            b"see your screen\0".as_ptr(),
            b"/HOME/shot.bmp\0".as_ptr(),
            b"Recorder\0".as_ptr(),
            0,
        )
    };
    if s <= 0 {
        return 40;
    }
    // a second open while one is live -> EBUSY (its own slot)
    let s2 = unsafe {
        cap_req_open_rs(
            43,
            1000,
            1000,
            CAP_SCREEN_CAPTURE,
            0,
            CAP_SCOPE_PATH,
            b"x\0".as_ptr(),
            b"/HOME/y.bmp\0".as_ptr(),
            b"Z\0".as_ptr(),
            0,
        )
    };
    if s2 != CAP_EBUSY {
        return 41;
    }
    // resolve DENY, requester sees DENIED, then reap
    if cap_req_resolve_rs(s as u64, 0) != 0 {
        return 42;
    }
    if cap_req_state_rs(s as u64) != CAP_ST_DENIED as i32 {
        return 43;
    }
    if cap_req_reap_rs(s as u64) != 0 {
        return 44;
    }
    // cross-exclusion: an open elevate prompt (elev_open=1) refuses a cap open
    let s3 = unsafe {
        cap_req_open_rs(
            44,
            1000,
            1000,
            CAP_SCREEN_CAPTURE,
            0,
            CAP_SCOPE_PATH,
            b"x\0".as_ptr(),
            b"/HOME/y.bmp\0".as_ptr(),
            b"Z\0".as_ptr(),
            1,
        )
    };
    if s3 != CAP_EBUSY {
        return 45;
    }
    // expiry denies
    let s4 = unsafe {
        cap_req_open_rs(
            45,
            1000,
            0,
            CAP_SCREEN_CAPTURE,
            0,
            CAP_SCOPE_PATH,
            b"x\0".as_ptr(),
            b"/HOME/y.bmp\0".as_ptr(),
            b"Z\0".as_ptr(),
            0,
        )
    };
    if s4 <= 0 {
        return 46;
    }
    if cap_req_tick_rs(OPEN_TTL_MS + 1, 1) != 1 {
        return 47;
    }
    if cap_req_state_rs(s4 as u64) != CAP_ST_DENIED as i32 {
        return 48;
    }
    let _ = cap_req_reap_rs(s4 as u64);
    // dead requester denies
    let s5 = unsafe {
        cap_req_open_rs(
            46,
            1000,
            0,
            CAP_SCREEN_CAPTURE,
            0,
            CAP_SCOPE_PATH,
            b"x\0".as_ptr(),
            b"/HOME/y.bmp\0".as_ptr(),
            b"Z\0".as_ptr(),
            0,
        )
    };
    if s5 <= 0 {
        return 49;
    }
    if cap_req_tick_rs(1, 0) != 1 {
        return 50;
    }
    let _ = cap_req_reap_rs(s5 as u64);

    // -- serial.port (Stage 2): issuable, gated by the right syscall, scope
    // shape validated, and an EXACT port-name cover (never a prefix). ---------
    if cap_is_issuable(CAP_SERIAL_PORT) != 1 {
        return 60;
    }
    if cap_required_for_syscall(SYS_SERIAL_OPEN_NUM) != CAP_SERIAL_PORT {
        return 61;
    }
    if cap_scope_valid(CAP_SERIAL_PORT, CAP_SCOPE_PORT, b"ttyS1\0".as_ptr()) != 1 {
        return 62;
    }
    // wrong scope kind (a path kind on a serial cap) is refused
    if cap_scope_valid(CAP_SERIAL_PORT, CAP_SCOPE_PATH, b"ttyS1\0".as_ptr()) != 0 {
        return 63;
    }
    // a port name with path structure or a bad byte is refused
    if cap_scope_valid(CAP_SERIAL_PORT, CAP_SCOPE_PORT, b"tty/S1\0".as_ptr()) != 0 {
        return 64;
    }
    if cap_scope_valid(CAP_SERIAL_PORT, CAP_SCOPE_PORT, b"\0".as_ptr()) != 0 {
        return 65;
    }
    // the port scope kind does not validate for screen.capture
    if cap_scope_valid(CAP_SCREEN_CAPTURE, CAP_SCOPE_PORT, b"ttyS1\0".as_ptr()) != 0 {
        return 66;
    }
    let mut sgrants = [CapGrant {
        cap: 0,
        scope_kind: 0,
        expires_ms: 0,
        uses_left: 0,
        pad: 0,
        granted_seq: 0,
        scope: [0u8; SCOPE_MAX],
    }; CAP_MAX_GRANTS as usize];
    let sgp = sgrants.as_mut_ptr();
    let sidx = unsafe {
        cap_issue(sgp, n, CAP_SERIAL_PORT, CAP_SCOPE_PORT, b"ttyS1\0".as_ptr(), now + 100, 1, 900)
    };
    if sidx < 0 {
        return 67;
    }
    // exact name covered; a lookalike is NOT (no prefix match)
    if unsafe { cap_covers_port(sgp, n, CAP_SERIAL_PORT, b"ttyS1\0".as_ptr(), now) } != sidx {
        return 68;
    }
    if unsafe { cap_covers_port(sgp, n, CAP_SERIAL_PORT, b"ttyS10\0".as_ptr(), now) } != -1 {
        return 69;
    }
    // revocation bites
    if unsafe { cap_revoke(sgp, n, CAP_SERIAL_PORT) } != 900 {
        return 70;
    }
    if unsafe { cap_covers_port(sgp, n, CAP_SERIAL_PORT, b"ttyS1\0".as_ptr(), now) } != -1 {
        return 71;
    }

    // -- input.inject WINDOW_TARGET (Stage 4): scope shape, exact-id cover, and
    // the self/target scope-kind distinction. ------------------------------
    if cap_scope_valid(CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET, b"3\0".as_ptr()) != 1 {
        return 80;
    }
    if cap_scope_valid(CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET, b"42:DOOM\0".as_ptr()) != 1 {
        return 81;
    }
    if cap_scope_valid(CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET, b"self\0".as_ptr()) != 0 {
        return 82; // not a decimal id
    }
    if cap_scope_valid(CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET, b"\0".as_ptr()) != 0 {
        return 83; // empty
    }
    if cap_scope_valid(CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET, b":x\0".as_ptr()) != 0 {
        return 84; // no leading id
    }
    if cap_scope_valid(CAP_INPUT_INJECT, CAP_SCOPE_WINDOW, b"42:DOOM\0".as_ptr()) != 0 {
        return 85; // WINDOW_TARGET noun is not the self WINDOW token
    }
    let mut wgrants = [CapGrant {
        cap: 0, scope_kind: 0, expires_ms: 0, uses_left: 0, pad: 0, granted_seq: 0,
        scope: [0u8; SCOPE_MAX],
    }; CAP_MAX_GRANTS as usize];
    let wgp = wgrants.as_mut_ptr();
    let wself = unsafe {
        cap_issue(wgp, n, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW, b"self\0".as_ptr(), now + 100, CAP_USES_UNLIMITED, 910)
    };
    if wself < 0 {
        return 86;
    }
    let wtgt = unsafe {
        cap_issue(wgp, n, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET, b"42:DOOM\0".as_ptr(), now + 100, CAP_USES_UNLIMITED, 911)
    };
    if wtgt < 0 {
        return 87;
    }
    if unsafe { cap_find_scope(wgp, n, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW, now) } != wself {
        return 88;
    }
    if unsafe { cap_find_scope(wgp, n, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET, now) } != wtgt {
        return 89;
    }
    if unsafe { cap_covers_window_target(wgp, n, CAP_INPUT_INJECT, 42, now) } != wtgt {
        return 90;
    }
    if unsafe { cap_covers_window_target(wgp, n, CAP_INPUT_INJECT, 4, now) } != -1 {
        return 91; // a different id
    }
    if unsafe { cap_covers_window_target(wgp, n, CAP_INPUT_INJECT, 420, now) } != -1 {
        return 92; // no prefix/suffix match
    }
    if unsafe { cap_covers_window_target(wgp, n, CAP_INPUT_INJECT, 0, now) } != -1 {
        return 93; // id 0 matches nothing
    }
    if unsafe { cap_revoke(wgp, n, CAP_INPUT_INJECT) } == 0 {
        return 94;
    }
    if unsafe { cap_covers_window_target(wgp, n, CAP_INPUT_INJECT, 42, now) } != -1 {
        return 95; // revocation clears the target grant
    }
    if unsafe { cap_find_scope(wgp, n, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW, now) } != -1 {
        return 96; // and the self grant (same class)
    }

    // -- standing consent ("Always allow") round trip, codes 100-109 --------
    // WHY THIS EXISTS: the on-device test could not reach the interesting case.
    // input.inject's scope is "<window-id>:<title>", so a real title like
    // "POKEMON YELLOW" CONTAINS A SPACE, and the store's file format is space
    // separated. The end-to-end flow reaches its objective from the vision call
    // alone and never raises an input.inject prompt, so that consent is never
    // created and the space path is never walked. These checks walk it directly.
    //
    // NOT VACUOUS: against the pre-fix code, which validated the scope with
    // token_ok() (refusing every byte <= 0x20), the add at 100 returns CAP_EARG
    // and this goes RED immediately.
    cap_always_reset_rs();
    let spaced = b"12345:POKEMON YELLOW\0";
    let app = b"/APPS/FLOWRUN\0";
    if unsafe {
        cap_always_add_rs(1000, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET,
                          app.as_ptr(), spaced.as_ptr())
    } != 1 {
        return 100; // a scope with a space MUST be storable
    }
    if unsafe {
        cap_always_match_rs(1000, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET,
                            app.as_ptr(), spaced.as_ptr())
    } != 1 {
        return 101; // and must match back in memory
    }
    // Round trip through the FILE representation, which is where a space would
    // be lost: serialize, wipe, reparse, match again.
    let mut buf = [0u8; 512];
    let n_out = unsafe { cap_always_serialize_rs(buf.as_mut_ptr(), buf.len() as u32) };
    if n_out <= 0 {
        return 102;
    }
    cap_always_reset_rs();
    if cap_always_count_rs() != 0 {
        return 103;
    }
    if unsafe { cap_always_parse_rs(buf.as_ptr(), n_out as u32) } != 1 {
        return 104; // exactly one record must come back
    }
    if unsafe {
        cap_always_match_rs(1000, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET,
                            app.as_ptr(), spaced.as_ptr())
    } != 1 {
        return 105; // THE POINT: the space survived the file round trip
    }
    // A near miss must NOT match: every field is significant, no prefix rule.
    if unsafe {
        cap_always_match_rs(1000, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET,
                            app.as_ptr(), b"12345:POKEMON\0".as_ptr())
    } != 0 {
        return 106;
    }
    if unsafe {
        cap_always_match_rs(1001, CAP_INPUT_INJECT, CAP_SCOPE_WINDOW_TARGET,
                            app.as_ptr(), spaced.as_ptr())
    } != 0 {
        return 107; // a different uid is a different principal
    }
    // A control byte in the scope must STILL be refused: a newline would forge
    // a second record when the table is written back out.
    if unsafe {
        cap_always_add_rs(1000, CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH,
                          app.as_ptr(), b"/HOME/a\nb\0".as_ptr())
    } != CAP_EARG {
        return 108;
    }
    // The APP field is not last on the line, so a space there is still refused.
    if unsafe {
        cap_always_add_rs(1000, CAP_SCREEN_CAPTURE, CAP_SCOPE_PATH,
                          b"/APPS/MY APP\0".as_ptr(), b"/HOME/x\0".as_ptr())
    } != CAP_EARG {
        return 109;
    }
    cap_always_reset_rs();

    0
}

// ===========================================================================
// STANDING CONSENT ("Always allow") - #capalways, owner request 2026-09-25
// ---------------------------------------------------------------------------
// A normal grant is time-bounded (GRANT_MAX_TTL_MS, 15 min) and dies with the
// process, which is why a flow cannot re-arm itself and why the synthetic-key
// block is meaningful. That is correct for a one-off consent, but it makes a
// long-running unattended agent impossible: every 15 minutes a human must press
// a key. "Always allow" persists the DECISION, not the grant.
//
// WHAT IT CHANGES AND WHAT IT DOES NOT:
//   * The human still approves ONCE, at the real compositor prompt, with real
//     input credit. There is NO API that creates a standing consent; it can
//     only be born from CAP_ACT_APPROVE_ALWAYS, which only the compositor can
//     send (caller_is_compositor) while a prompt it drew is open.
//   * A matching standing consent then lets a later request skip the prompt AND
//     the input-credit requirement, because the input credit exists to stop an
//     app raising a prompt nobody asked for, and there is no prompt to raise.
//   * Each issued grant is STILL time-bounded and still consumed normally. The
//     standing record only removes the human from the re-issue, it does not
//     create an immortal grant.
//
// IDENTITY, AND WHY THE PROCESS NAME IS ENOUGH HERE:
// A record is keyed on (uid, app name, cap, scope_kind, exact scope) - all five
// must match, and the scope is the kernel-validated noun, never the app's raw
// string. The name is not a strong identity on its own, but /APPS is root-owned
// 0755, so substituting the binary behind a name already requires root, and
// root is not inside this threat model (it can issue itself anything). So
// name-keying adds NO capability an attacker did not already have. Hashing the
// image at exec would be strictly better and is the obvious hardening; it needs
// an exec-path/digest field on process_t, which this change deliberately does
// not add because touching the ELF loader risks boot for a convenience feature.
// Recorded here so the limitation is explicit rather than discovered later.
pub const CAP_ALWAYS_MAX: usize = 32;
pub const APPNAME_MAX: usize = 32;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CapAlways {
    pub used: u32,
    pub uid: u32,
    pub cap: u32,
    pub scope_kind: u32,
    pub app: [u8; APPNAME_MAX],
    pub scope: [u8; SCOPE_MAX],
}

const ALWAYS_EMPTY: CapAlways = CapAlways {
    used: 0,
    uid: 0,
    cap: 0,
    scope_kind: 0,
    app: [0u8; APPNAME_MAX],
    scope: [0u8; SCOPE_MAX],
};

static mut ALWAYS: [CapAlways; CAP_ALWAYS_MAX] = [ALWAYS_EMPTY; CAP_ALWAYS_MAX];

fn cstr_eq(a: &[u8], b: *const u8) -> bool {
    if b.is_null() {
        return false;
    }
    let mut i = 0usize;
    while i < a.len() {
        let c = unsafe { *b.add(i) };
        if a[i] != c {
            return false;
        }
        if c == 0 {
            return true;
        }
        i += 1;
    }
    // a ran out with no NUL: equal only if src also ends here
    unsafe { *b.add(i) == 0 }
}

// A stored field must be a single printable token: no spaces (the file is
// space-separated) and no control bytes. Refusing at STORE time means the
// parser never has to reason about a quoted or embedded-space field.
fn token_ok(src: *const u8, max: usize) -> bool {
    if src.is_null() {
        return false;
    }
    let mut i = 0usize;
    loop {
        if i >= max {
            return false; // unterminated
        }
        let c = unsafe { *src.add(i) };
        if c == 0 {
            break;
        }
        if c <= 0x20 || c >= 0x7F {
            return false;
        }
        i += 1;
    }
    i > 0
}

// The SCOPE may legitimately contain spaces and is therefore validated
// separately from `app`. input.inject's scope is built as "<window-id>:<title>"
// and a real window title such as "POKEMON YELLOW" has a space in it; rejecting
// that made input.inject impossible to remember, which the on-device test caught
// (screen.capture auto-granted, input.inject still prompted). Safe because scope
// is the LAST field on the line, so a space cannot be confused with a separator.
// Control bytes ARE still refused: a newline would forge a second record.
fn scope_token_ok(src: *const u8, max: usize) -> bool {
    if src.is_null() {
        return false;
    }
    let mut i = 0usize;
    loop {
        if i >= max {
            return false; // unterminated
        }
        let c = unsafe { *src.add(i) };
        if c == 0 {
            break;
        }
        if c < 0x20 || c == 0x7F {
            return false;
        }
        i += 1;
    }
    i > 0
}

fn copy_token(dst: &mut [u8], src: *const u8) {
    for b in dst.iter_mut() {
        *b = 0;
    }
    if src.is_null() {
        return;
    }
    let mut i = 0usize;
    while i < dst.len() - 1 {
        let c = unsafe { *src.add(i) };
        if c == 0 {
            break;
        }
        dst[i] = c;
        i += 1;
    }
}

/// Drop every standing record (used before a reload, and by the self-test).
#[no_mangle]
pub extern "C" fn cap_always_reset_rs() {
    let t = unsafe { &mut *core::ptr::addr_of_mut!(ALWAYS) };
    for e in t.iter_mut() {
        *e = ALWAYS_EMPTY;
    }
}

/// How many standing records are live.
#[no_mangle]
pub extern "C" fn cap_always_count_rs() -> u32 {
    let t = unsafe { &*core::ptr::addr_of!(ALWAYS) };
    let mut n = 0u32;
    for e in t.iter() {
        if e.used != 0 {
            n += 1;
        }
    }
    n
}

/// 1 if a standing consent covers exactly this (uid, app, cap, scope_kind,
/// scope), else 0. Every field must match; there is no wildcard and no prefix
/// match, because a standing privilege that widens silently is the thing this
/// whole design exists to avoid.
///
/// # Safety
/// `app` and `scope` are NUL-terminated kernel buffers.
#[no_mangle]
pub unsafe extern "C" fn cap_always_match_rs(
    uid: u32,
    cap: u32,
    scope_kind: u32,
    app: *const u8,
    scope: *const u8,
) -> i32 {
    if cap == CAP_NONE || app.is_null() || scope.is_null() {
        return 0;
    }
    let t = unsafe { &*core::ptr::addr_of!(ALWAYS) };
    for e in t.iter() {
        if e.used == 0 || e.uid != uid || e.cap != cap || e.scope_kind != scope_kind {
            continue;
        }
        if cstr_eq(&e.app, app) && cstr_eq(&e.scope, scope) {
            return 1;
        }
    }
    0
}

/// Record a standing consent. 1 = added, 0 = already present (idempotent),
/// CAP_EMAX = table full, CAP_EARG = a field that would corrupt the file.
///
/// # Safety
/// `app` and `scope` are NUL-terminated kernel buffers.
#[no_mangle]
pub unsafe extern "C" fn cap_always_add_rs(
    uid: u32,
    cap: u32,
    scope_kind: u32,
    app: *const u8,
    scope: *const u8,
) -> i64 {
    if cap == CAP_NONE || cap >= CAP_CLASS_MAX {
        return CAP_EARG;
    }
    // `app` stays a strict token because it is not the last field on the line.
    if !token_ok(app, APPNAME_MAX) || !scope_token_ok(scope, SCOPE_MAX) {
        return CAP_EARG;
    }
    if unsafe { cap_always_match_rs(uid, cap, scope_kind, app, scope) } == 1 {
        return 0;
    }
    let t = unsafe { &mut *core::ptr::addr_of_mut!(ALWAYS) };
    for e in t.iter_mut() {
        if e.used == 0 {
            e.used = 1;
            e.uid = uid;
            e.cap = cap;
            e.scope_kind = scope_kind;
            copy_token(&mut e.app, app);
            copy_token(&mut e.scope, scope);
            return 1;
        }
    }
    CAP_EMAX
}

// --- tiny decimal helpers (no_std, no core::fmt in the kernel) -------------
fn parse_u32(buf: &[u8], pos: &mut usize) -> Option<u32> {
    let mut v: u64 = 0;
    let mut any = false;
    while *pos < buf.len() {
        let c = buf[*pos];
        if c < b'0' || c > b'9' {
            break;
        }
        v = v * 10 + (c - b'0') as u64;
        if v > 0xFFFF_FFFF {
            return None;
        }
        any = true;
        *pos += 1;
    }
    if any {
        Some(v as u32)
    } else {
        None
    }
}

fn skip_spaces(buf: &[u8], pos: &mut usize) {
    while *pos < buf.len() && buf[*pos] == b' ' {
        *pos += 1;
    }
}

fn read_token(buf: &[u8], pos: &mut usize, dst: &mut [u8]) -> bool {
    for b in dst.iter_mut() {
        *b = 0;
    }
    let mut o = 0usize;
    while *pos < buf.len() {
        let c = buf[*pos];
        if c == b' ' || c == b'\n' || c == b'\r' {
            break;
        }
        if o + 1 >= dst.len() {
            return false;
        }
        dst[o] = c;
        o += 1;
        *pos += 1;
    }
    o > 0
}

fn emit(out: &mut [u8], o: &mut usize, s: &[u8]) -> bool {
    for &c in s {
        if *o + 1 >= out.len() {
            return false;
        }
        out[*o] = c;
        *o += 1;
    }
    true
}

fn emit_u32(out: &mut [u8], o: &mut usize, mut v: u32) -> bool {
    let mut d = [0u8; 10];
    let mut n = 0usize;
    if v == 0 {
        d[0] = b'0';
        n = 1;
    } else {
        while v > 0 {
            d[n] = b'0' + (v % 10) as u8;
            v /= 10;
            n += 1;
        }
    }
    while n > 0 {
        n -= 1;
        if *o + 1 >= out.len() {
            return false;
        }
        out[*o] = d[n];
        *o += 1;
    }
    true
}

/// Parse /CONFIG/CAPALLOW.CFG into the table, REPLACING it. Lines are
/// `<uid> <cap> <scope_kind> <app> <scope>`; `#` comments and blank lines are
/// skipped; a malformed line is skipped rather than aborting the load, so one
/// bad edit cannot lock every standing consent out. Returns records loaded.
///
/// # Safety
/// `buf` points to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn cap_always_parse_rs(buf: *const u8, len: u32) -> i32 {
    cap_always_reset_rs();
    if buf.is_null() || len == 0 {
        return 0;
    }
    let b = unsafe { core::slice::from_raw_parts(buf, len as usize) };
    let mut pos = 0usize;
    let mut loaded = 0i32;
    while pos < b.len() {
        // isolate one line
        let start = pos;
        while pos < b.len() && b[pos] != b'\n' {
            pos += 1;
        }
        let mut end = pos;
        if end > start && b[end - 1] == b'\r' {
            end -= 1;
        }
        if pos < b.len() {
            pos += 1; // step over '\n'
        }
        let line = &b[start..end];
        if line.is_empty() || line[0] == b'#' {
            continue;
        }
        let mut lp = 0usize;
        skip_spaces(line, &mut lp);
        let uid = match parse_u32(line, &mut lp) {
            Some(v) => v,
            None => continue,
        };
        skip_spaces(line, &mut lp);
        let cap = match parse_u32(line, &mut lp) {
            Some(v) => v,
            None => continue,
        };
        skip_spaces(line, &mut lp);
        let sk = match parse_u32(line, &mut lp) {
            Some(v) => v,
            None => continue,
        };
        skip_spaces(line, &mut lp);
        let mut app = [0u8; APPNAME_MAX];
        if !read_token(line, &mut lp, &mut app) {
            continue;
        }
        skip_spaces(line, &mut lp);
        // REST OF LINE, not a token: a scope may contain spaces (see
        // scope_token_ok). Trailing whitespace is trimmed so a stray space
        // before the newline cannot change what the record means.
        let mut scope = [0u8; SCOPE_MAX];
        {
            let mut end = line.len();
            while end > lp && (line[end - 1] == b' ' || line[end - 1] == b'\t') {
                end -= 1;
            }
            if end <= lp || end - lp >= SCOPE_MAX {
                continue;
            }
            let mut o = 0usize;
            while lp < end {
                scope[o] = line[lp];
                o += 1;
                lp += 1;
            }
        }
        if scope[0] == 0 {
            continue;
        }
        if cap == CAP_NONE || cap >= CAP_CLASS_MAX {
            continue;
        }
        let r = unsafe {
            cap_always_add_rs(uid, cap, sk, app.as_ptr(), scope.as_ptr())
        };
        if r == 1 {
            loaded += 1;
        }
    }
    loaded
}

/// Render the table back to file text. Returns bytes written, or CAP_EARG if
/// the buffer is too small (the caller then does NOT write, so a truncated
/// file can never replace a good one).
///
/// # Safety
/// `out` points to `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn cap_always_serialize_rs(out: *mut u8, cap_len: u32) -> i32 {
    if out.is_null() || cap_len == 0 {
        return CAP_EARG as i32;
    }
    let o_slice = unsafe { core::slice::from_raw_parts_mut(out, cap_len as usize) };
    let mut o = 0usize;
    if !emit(o_slice, &mut o, b"# MayteraOS standing capability consents (\"Always allow\").\n") {
        return CAP_EARG as i32;
    }
    if !emit(o_slice, &mut o, b"# <uid> <cap> <scope_kind> <app> <scope>. Delete a line to revoke.\n") {
        return CAP_EARG as i32;
    }
    let t = unsafe { &*core::ptr::addr_of!(ALWAYS) };
    for e in t.iter() {
        if e.used == 0 {
            continue;
        }
        if !emit_u32(o_slice, &mut o, e.uid) { return CAP_EARG as i32; }
        if !emit(o_slice, &mut o, b" ") { return CAP_EARG as i32; }
        if !emit_u32(o_slice, &mut o, e.cap) { return CAP_EARG as i32; }
        if !emit(o_slice, &mut o, b" ") { return CAP_EARG as i32; }
        if !emit_u32(o_slice, &mut o, e.scope_kind) { return CAP_EARG as i32; }
        if !emit(o_slice, &mut o, b" ") { return CAP_EARG as i32; }
        let mut i = 0usize;
        while i < e.app.len() && e.app[i] != 0 {
            if o + 1 >= o_slice.len() { return CAP_EARG as i32; }
            o_slice[o] = e.app[i];
            o += 1;
            i += 1;
        }
        if !emit(o_slice, &mut o, b" ") { return CAP_EARG as i32; }
        i = 0;
        while i < e.scope.len() && e.scope[i] != 0 {
            if o + 1 >= o_slice.len() { return CAP_EARG as i32; }
            o_slice[o] = e.scope[i];
            o += 1;
            i += 1;
        }
        if !emit(o_slice, &mut o, b"\n") { return CAP_EARG as i32; }
    }
    o as i32
}

/// Open-and-immediately-grant, for a request already covered by a STANDING
/// consent. Identical bookkeeping to `cap_req_open_rs` except the slot lands in
/// CAP_ST_GRANTED, never CAP_ST_OPEN, so `cap_req_view_rs` (which reports only
/// an OPEN slot) cannot show the compositor a prompt that nobody needs to
/// answer. The requester's existing poll loop then sees GRANTED unchanged.
///
/// # Safety
/// `reason`, `scope`, `app` are NUL-terminated kernel buffers.
#[no_mangle]
pub unsafe extern "C" fn cap_req_autogrant_rs(
    pid: u32,
    uid: u32,
    now_ms: u64,
    cap: u32,
    duration_ms: u32,
    scope_kind: u32,
    reason: *const u8,
    scope: *const u8,
    app: *const u8,
    elev_open: u32,
) -> i64 {
    if pid == 0 {
        return CAP_EARG;
    }
    let g = unsafe { &mut *core::ptr::addr_of_mut!(GREQ) };
    if g.state == CAP_ST_OPEN || elev_open != 0 {
        return CAP_EBUSY;
    }
    let seq = unsafe {
        let n = core::ptr::read_volatile(core::ptr::addr_of!(CAP_NEXT_SEQ));
        core::ptr::write_volatile(core::ptr::addr_of_mut!(CAP_NEXT_SEQ), n + 1);
        n
    };
    g.seq = seq;
    g.opened_ms = now_ms;
    g.state = CAP_ST_GRANTED;
    g.req_pid = pid;
    g.req_uid = uid;
    g.cap = cap;
    let dur = if duration_ms as u64 > GRANT_MAX_TTL_MS {
        GRANT_MAX_TTL_MS as u32
    } else {
        duration_ms
    };
    g.duration_ms = dur;
    g.scope_kind = scope_kind;
    copy_scope(&mut g.scope, scope);
    sanitize(&mut g.reason, reason);
    sanitize(&mut g.app, app);
    seq as i64
}
