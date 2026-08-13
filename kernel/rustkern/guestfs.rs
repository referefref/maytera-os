// rustkern/guestfs.rs - #708: THE IDENTITY A DOS / WIN16 GUEST'S FILESYSTEM
// ACCESS IS CHECKED AGAINST.
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
// It calls into EXISTING, unchanged C for every input (perms_check(), the
// spawn-identity resolver, the desktop session) rather than reimplementing any
// of it; the DECISION is what lives here.
//
// ===========================================================================
// WHY THIS EXISTS
// ---------------------------------------------------------------------------
// The whole permission system gates filesystem access by uid, and every Ring-3
// path reaches it through perms_check() in sys_open() and friends. A DOS or
// Win16 GUEST reaches the filesystem somewhere else entirely: the emulator's
// own INT 21h handlers and the Win16 KERNEL file APIs call fat_open() /
// fat_read_file() / fat_write_file() / fat_readdir() DIRECTLY. Measured on
// build 1742 there are 29 such guest-reachable entry points across three files
// and not one of them consulted perms_check().
//
// While the desktop autologins as root that grants nothing, because
// perms_check() returns 0 on its first line for uid 0 and there is no boundary
// to cross. The moment the session is not root, "launch a game and have it read
// /CONFIG/SHADOW" becomes a one-line privilege escalation. As with #692, the
// session flip does not merely fail to prevent this, it CREATES it, by being
// the thing that makes root mean something.
//
// ===========================================================================
// THE IDENTITY MODEL
// ---------------------------------------------------------------------------
// A guest is not a normal process. It is an interpreter running inside a
// KERNEL THREAD (proc_create("dos", ...) / proc_create("win16", ...)), so it
// has no Ring-3 credentials of its own and proc_current()->euid is 0 by
// construction. Reading the uid off the current process at the point of the
// filesystem call would therefore always answer "root", which is exactly the
// bug. So:
//
//   1. THE IDENTITY IS CAPTURED AT LAUNCH, IN THE LAUNCHER'S CONTEXT, NOT AT
//      THE FILESYSTEM CALL. dos_launch() / win16_launch() run on the SYSCALL
//      stack of the Ring-3 process that asked for the guest, which is the last
//      moment a real user identity is available. By the time the guest thread
//      exists, that information is gone.
//
//   2. IT IS CARRIED IN A PER-LAYER SLOT, NOT INFERRED. Each layer runs at
//      most one guest at a time (g_dos_busy / g_win16_busy enforce that), so
//      the credential lives in a slot armed at launch and disarmed at
//      teardown. Every gated call site names its slot as a compile-time
//      constant, so there is no ambient "which guest am I" guess to get wrong.
//
//   3. A GUEST NEVER EXCEEDS ITS LAUNCHER. The check is a plain perms_check()
//      with the launcher's (uid, gid). No extra confinement, no extra
//      authority. A guest launched by root still has root's reach, which is
//      why this change is invisible on today's autologin-as-root image, and is
//      the property that makes it safe to land ahead of the session flip.
//
//   4. A GUEST LAUNCHED BY A SERVICE RATHER THAN A USER runs as the LOGGED-IN
//      DESKTOP SESSION, and only if a session has actually authenticated.
//      The two service launchers are the boot harnesses that read
//      /CONFIG/DOSRUN.CFG and /CONFIG/WIN16PM.RUN. Both files live under
//      /CONFIG (root, 0700), so only root can schedule one; but "root wrote
//      the config" is not the same claim as "the guest should run as root",
//      and a boot-time test harness has no business holding more authority
//      than the person sitting at the machine. If NO session has authenticated
//      yet, the identity is unresolvable and the arm is REFUSED.
//
//   5. FAIL CLOSED, AND MEAN IT. An unarmed slot, an out-of-range slot, a
//      refused resolution and a null path all DENY. There is no branch in this
//      file that returns "allow" without a perms_check() having said so. The
//      zero value of ARMED is 0 = not armed, so a zeroed or never-initialised
//      slot denies rather than meaning root, the same by-construction property
//      PROC_AS_INVALID gives #692.
//
// ===========================================================================
// WHAT THIS IS NOT
// ---------------------------------------------------------------------------
// This is a uid/mode gate, not a sandbox. It stops a guest from exceeding the
// authority of the user who launched it. It does NOT stop a guest from doing
// anything that user could do anyway: a guest launched by admin can still read
// and write everything admin owns, including admin's own dotfiles and the
// /WINDIR drive tree. Confining a guest BELOW its launcher (a real jail) is a
// separate design and is not claimed here.
// ===========================================================================

// Access bits, matching fs/perms.h. Kept as local constants rather than
// imported so this file states the contract it is coded against.
const X_OK: i32 = 1;
const W_OK: i32 = 2;
const R_OK: i32 = 4;
const ACCESS_MASK: i32 = R_OK | W_OK | X_OK;

// Slot identifiers. MUST match the GUESTFS_SLOT_* defines in fs/guestfs.h.
pub const SLOT_DOS: u32 = 0;
pub const SLOT_WIN16: u32 = 1;
const NSLOTS: usize = 2;

// Arm kinds. MUST match PROC_AS_* in proc/process.h; they are passed straight
// through to the #692 resolver so there is ONE identity vocabulary in the
// kernel rather than two that can drift apart.
const KIND_INVALID: u32 = 0;
const KIND_CALLER: u32 = 1;
const KIND_SESSION: u32 = 2;
const KIND_UID: u32 = 3;

// Refusal / denial codes, returned negative to C. The C side turns these into
// the DOS or Win16 error the guest sees, and logs them.
pub const E_BAD_SLOT: i32 = -1; // slot out of range
pub const E_NOT_ARMED: i32 = -2; // no identity for this guest: FAIL CLOSED
pub const E_DENIED: i32 = -3; // perms_check() said no
pub const E_BAD_PATH: i32 = -4; // null or unterminated path
pub const E_NO_SESSION: i32 = -5; // KIND_SESSION with no authenticated session
pub const E_RESOLVE: i32 = -6; // the #692 resolver refused the identity

extern "C" {
    // fs/perms.c. Returns 0 to allow, negative to deny. Applies POSIX path
    // resolution (#674) and the uid-0 bypass. `path` must be NUL-terminated.
    fn perms_check(path: *const u8, uid: u32, gid: u32, access: i32) -> i32;

    // gui/desktop.c. Non-zero once ANY session has authenticated this boot.
    // Without this test, desktop_get_session_uid() answers 0 before login,
    // which is uid ROOT, which is precisely the fail-open this file exists to
    // prevent.
    fn desktop_session_authenticated() -> i32;
}

#[derive(Clone, Copy)]
struct Cred {
    armed: u32, // 0 = not armed. Zero is the DENY value on purpose.
    uid: u32,
    gid: u32,
}

static mut CREDS: [Cred; NSLOTS] = [Cred {
    armed: 0,
    uid: 0,
    gid: 0,
}; NSLOTS];

// Denial counters, read by the boot/teardown report on the C side. They make a
// gate that never fires distinguishable from a gate that is not wired in,
// which is the recurring "the prose says it is enforced" trap in blame.md.
static mut DENIES: [u32; NSLOTS] = [0; NSLOTS];
static mut CHECKS: [u32; NSLOTS] = [0; NSLOTS];

fn slot_ok(slot: u32) -> bool {
    (slot as usize) < NSLOTS
}

/// Arm slot `slot` with the identity described by (`kind`, `want_uid`).
///
/// Called from the LAUNCHER, in the launcher's context, before the guest
/// thread is created. Returns 0 on success; a negative E_* on refusal, in
/// which case the slot is left DISARMED (so a caller that ignores the return
/// value still gets a guest with no filesystem access rather than a root one).
///
/// # Safety
/// Touches this module's slot table only.
#[no_mangle]
pub unsafe extern "C" fn guestfs_arm_rs(slot: u32, kind: u32, want_uid: u32) -> i32 {
    if !slot_ok(slot) {
        return E_BAD_SLOT;
    }
    // Disarm FIRST. If anything below refuses, the slot is closed, not stale
    // from a previous guest that may have run as somebody else.
    unsafe {
        CREDS[slot as usize] = Cred {
            armed: 0,
            uid: 0,
            gid: 0,
        };
        DENIES[slot as usize] = 0;
        CHECKS[slot as usize] = 0;
    }

    if kind == KIND_INVALID {
        return E_RESOLVE;
    }
    // A service launch must not silently become a root launch just because
    // nobody has logged in yet. desktop_get_session_uid() returns 0 (root) in
    // that state, so the authentication test is load-bearing, not decorative.
    if kind == KIND_SESSION && unsafe { desktop_session_authenticated() } == 0 {
        return E_NO_SESSION;
    }

    let mut uid: u32 = 0;
    let mut gid: u32 = 0;
    // ONE identity vocabulary: this is the same resolver every Ring-3 spawn
    // goes through (#692), including its rule that a kernel thread is not a
    // legitimate thing to inherit an identity from.
    let r = unsafe { crate::spawnid::spawn_ident_resolve_rs(kind, want_uid, &mut uid, &mut gid) };
    if r != 0 {
        return E_RESOLVE;
    }

    unsafe {
        CREDS[slot as usize] = Cred {
            armed: 1,
            uid,
            gid,
        };
    }
    0
}

/// Disarm slot `slot`. Called at guest teardown. After this every check on the
/// slot denies, so a stale handle or a late worker cannot keep using a dead
/// guest's authority.
///
/// # Safety
/// Touches this module's slot table only.
#[no_mangle]
pub unsafe extern "C" fn guestfs_disarm_rs(slot: u32) {
    if !slot_ok(slot) {
        return;
    }
    unsafe {
        CREDS[slot as usize] = Cred {
            armed: 0,
            uid: 0,
            gid: 0,
        };
    }
}

/// Report the armed identity of a slot, for logging. Returns 0 and writes both
/// outputs if armed; negative and writes nothing otherwise.
///
/// # Safety
/// `out_uid` / `out_gid` must be valid writable u32s.
#[no_mangle]
pub unsafe extern "C" fn guestfs_cred_rs(slot: u32, out_uid: *mut u32, out_gid: *mut u32) -> i32 {
    if !slot_ok(slot) {
        return E_BAD_SLOT;
    }
    let c = unsafe { CREDS[slot as usize] };
    if c.armed == 0 {
        return E_NOT_ARMED;
    }
    unsafe {
        *out_uid = c.uid;
        *out_gid = c.gid;
    }
    0
}

/// Counters for the teardown report: how many checks the slot performed and
/// how many it denied. A gate reporting checks=0 is a gate that is not wired
/// in, however good its code reads.
///
/// # Safety
/// `out_checks` / `out_denies` must be valid writable u32s.
#[no_mangle]
pub unsafe extern "C" fn guestfs_stats_rs(slot: u32, out_checks: *mut u32, out_denies: *mut u32) {
    if !slot_ok(slot) {
        return;
    }
    unsafe {
        *out_checks = CHECKS[slot as usize];
        *out_denies = DENIES[slot as usize];
    }
}

/// THE GATE. May the guest in `slot` touch `path` with `access`?
///
/// Returns 0 to ALLOW. Any negative value DENIES, and the value says why.
/// `path` is a NUL-terminated NATIVE MayteraOS path (post drive-letter
/// mapping), because that is the name the filesystem and perms.c both use; a
/// gate applied to the pre-mapping DOS string would be checking a different
/// namespace from the one the access actually lands in.
///
/// # Safety
/// `path` must be a NUL-terminated C string or null.
#[no_mangle]
pub unsafe extern "C" fn guestfs_check_rs(slot: u32, path: *const u8, access: i32) -> i32 {
    if !slot_ok(slot) {
        return E_BAD_SLOT;
    }
    let idx = slot as usize;
    unsafe {
        CHECKS[idx] = CHECKS[idx].wrapping_add(1);
    }

    if path.is_null() || unsafe { *path } == 0 {
        unsafe {
            DENIES[idx] = DENIES[idx].wrapping_add(1);
        }
        return E_BAD_PATH;
    }
    // An access request of 0 would ask perms_check() "may I do nothing", which
    // it answers yes to. A gated call site that computed no access bits is a
    // bug in the call site, and it must not read as permission.
    if (access & ACCESS_MASK) == 0 || (access & !ACCESS_MASK) != 0 {
        unsafe {
            DENIES[idx] = DENIES[idx].wrapping_add(1);
        }
        return E_BAD_PATH;
    }

    let c = unsafe { CREDS[idx] };
    if c.armed == 0 {
        // FAIL CLOSED. No identity was captured for this guest, so it gets no
        // filesystem access. It explicitly does NOT get root's.
        unsafe {
            DENIES[idx] = DENIES[idx].wrapping_add(1);
        }
        return E_NOT_ARMED;
    }

    if unsafe { perms_check(path, c.uid, c.gid, access) } != 0 {
        unsafe {
            DENIES[idx] = DENIES[idx].wrapping_add(1);
        }
        return E_DENIED;
    }
    0
}

/// Boot self-test. Proves the policy is LIVE on this exact build rather than
/// merely compiled in, which is the trap blame.md keeps recording. Returns a
/// bitmask of FAILED checks; 0 means all passed. The C side prints it.
///
/// Only branches that are pure with respect to live kernel state are asserted
/// here: slot bounds, the fail-closed unarmed default, the rejection of an
/// empty access mask, and the fact that an explicit-uid arm round-trips. The
/// CALLER and SESSION branches depend on a live process and a live session and
/// are proven behaviourally on a booted VM instead.
///
/// # Safety
/// Saves and restores the slots it perturbs.
#[no_mangle]
pub unsafe extern "C" fn guestfs_selftest_rs() -> u32 {
    let mut fails: u32 = 0;
    let saved = unsafe { CREDS[SLOT_DOS as usize] };
    let saved_checks = unsafe { CHECKS[SLOT_DOS as usize] };
    let saved_denies = unsafe { DENIES[SLOT_DOS as usize] };

    let p = b"/CONFIG/SHADOW\0".as_ptr();

    // 1. An out-of-range slot denies.
    if unsafe { guestfs_check_rs(99, p, R_OK) } != E_BAD_SLOT {
        fails |= 1;
    }
    // 2. An UNARMED slot denies, and denies with the fail-closed code rather
    //    than falling through to a perms_check() as uid 0.
    unsafe { guestfs_disarm_rs(SLOT_DOS) };
    if unsafe { guestfs_check_rs(SLOT_DOS, p, R_OK) } != E_NOT_ARMED {
        fails |= 2;
    }
    // 3. A null path denies.
    if unsafe { guestfs_check_rs(SLOT_DOS, core::ptr::null(), R_OK) } != E_BAD_PATH {
        fails |= 4;
    }
    // 4. An empty access mask denies rather than reading as permission.
    unsafe { CREDS[SLOT_DOS as usize] = Cred { armed: 1, uid: 0, gid: 0 } };
    if unsafe { guestfs_check_rs(SLOT_DOS, p, 0) } != E_BAD_PATH {
        fails |= 8;
    }
    // 5. Armed as root, /CONFIG/SHADOW is allowed (perms_check's uid-0 bypass).
    //    This is the arm that proves the gate is calling perms_check at all
    //    rather than denying everything unconditionally.
    if unsafe { guestfs_check_rs(SLOT_DOS, p, R_OK) } != 0 {
        fails |= 16;
    }
    // 6. Armed as an ordinary uid, /CONFIG/SHADOW (root 0600, under /CONFIG
    //    root 0700) is DENIED. This is the whole point of the change, asserted
    //    at boot on every build.
    unsafe { CREDS[SLOT_DOS as usize] = Cred { armed: 1, uid: 1000, gid: 1000 } };
    if unsafe { guestfs_check_rs(SLOT_DOS, p, R_OK) } != E_DENIED {
        fails |= 32;
    }
    // 7. An explicit-uid arm round-trips through the #692 resolver.
    if unsafe { guestfs_arm_rs(SLOT_DOS, KIND_UID, 1000) } != 0 {
        fails |= 64;
    } else {
        let mut u: u32 = 0;
        let mut g: u32 = 0;
        if unsafe { guestfs_cred_rs(SLOT_DOS, &mut u, &mut g) } != 0 || u != 1000 {
            fails |= 128;
        }
    }
    // 8. A KIND_INVALID arm is refused and leaves the slot CLOSED.
    if unsafe { guestfs_arm_rs(SLOT_DOS, KIND_INVALID, 0) } != E_RESOLVE {
        fails |= 256;
    }
    if unsafe { guestfs_check_rs(SLOT_DOS, p, R_OK) } != E_NOT_ARMED {
        fails |= 512;
    }

    unsafe {
        CREDS[SLOT_DOS as usize] = saved;
        CHECKS[SLOT_DOS as usize] = saved_checks;
        DENIES[SLOT_DOS as usize] = saved_denies;
    }
    fails
}
