// rustkern/sysvol.rs - #162: THE system master-volume state machine.
//
// WHY THIS EXISTS AS ONE MODULE
// ==========================================================================
// #162 adds volume-up / volume-down / mute as system-global hotkeys. The
// obvious way to build that is a second volume-setting path next to the tray
// slider's. This project has been bitten by exactly that shape more times
// than is comfortable: two Task Managers, two `g_wallpapers[]` arrays, five
// `version.h` files, two copies of the HID usage table (#763). So the hotkey
// does NOT get its own path. This module is the ONE holder of the system
// volume, and everything drives it:
//
//   SYS_SET_VOLUME (tray slider, Settings, profile restore)  -> sysvol_set_rs
//   SYS_GET_VOLUME                                           -> sysvol_get_rs
//   SYS_SET_MUTE                                             -> sysvol_mute_rs
//   the media keys (cpu/isr.c, all transports)               -> sysvol_key_rs
//   SYS_VOL_STATE (compositor OSD + tray gauge)              -> sysvol_state_rs
//
// AND IT FIXES A LIVE BUG IT WAS STANDING ON
// ==========================================================================
// Before this, `sys_get_volume()` called `audio_get_volume()`, whose entire
// body is a comment saying "Return reasonable defaults - actual
// implementation would query hardware" followed by `vol->master_left = 80`.
// It returned 80 ALWAYS, whatever had been set. So:
//   - the tray slider snapped back to 80 every time it was reopened;
//   - `profile.c` persisted `volume=80` on every save, whatever the user
//     chose, silently discarding the setting across reboots;
//   - and a mute that "restores the previous level" was not implementable at
//     all, because nothing in the kernel remembered what the level was.
// The volume was WRITE-ONLY. #162 could not be built on top of that, so the
// state lives here and the read is real.
//
// WHY THE HARDWARE WRITE IS DEFERRED
// ==========================================================================
// A media key can arrive in HARD IRQ CONTEXT: the PS/2 path is IRQ1 ->
// keyboard_process_scancode(). Applying a volume change touches the codec
// (hda_set_volume issues verbs; ac97/uac touch their own registers), which is
// not work an interrupt handler may do. So sysvol_key_rs() only mutates
// atomics and raises a dirty flag - no locks, no waits, no MMIO - and a
// kernel worker applies it. NO WAKE CAN BE LOST: the dirty flag is set BEFORE
// wake_up(), and wait_event() re-tests the condition before it sleeps, so a
// wake that arrives "too early" is simply the condition already being true.
// That is the sound version of the pattern, not a timeout papering over a
// wake we forgot to arm.
//
// The SYSCALL path applies synchronously instead, from its own thread
// context, because a syscall is allowed to block and because that keeps the
// tray slider working byte-for-byte as before even if the worker never
// starts. Two CALLERS, one IMPLEMENTATION (sysvol_apply_rs): that is the
// distinction that matters, and it is not the thing this file's header warns
// against.
//
// New kernel logic with no C twin, so Rust per the 2026-07-16 rule, and no
// `-DRUST_*` strangler flag and no RUST_PORT_LEDGER row: there is no C
// original to differ from, so the rollback is reverting the commit. What
// stands in for a differential is sysvol_selftest_rs(), a vector test over
// the pure state-machine core, run on every boot with one line of output.

#![allow(dead_code)]

use core::sync::atomic::{AtomicU32, Ordering};

extern "C" {
    // drivers/audio.c. Thin C shims so the FFI is int-only: audio_mute() takes
    // a C `bool`, whose ABI is a single byte with undefined high bits, and
    // handing Rust an i32 to that is the kind of quiet mismatch that works on
    // one compiler and not the next.
    fn sysvol_hw_set_level(level: i32);
    fn sysvol_hw_set_mute(mute: i32);
}

// Step size, in percent. 5 gives 20 detents across the range, which is what
// the tray slider's own drag resolution already feels like.
const STEP: i32 = 5;

// Boot default. 80 deliberately matches the value audio_get_volume() used to
// return unconditionally, so a machine that never touches the volume behaves
// exactly as it did before this change.
const DEFAULT_LEVEL: u32 = 80;

static LEVEL: AtomicU32 = AtomicU32::new(DEFAULT_LEVEL);
static MUTED: AtomicU32 = AtomicU32::new(0);
static PRE_MUTE: AtomicU32 = AtomicU32::new(DEFAULT_LEVEL);
/// Bumped on every change from any source. The compositor mirrors the tray
/// gauge off this.
static SEQ: AtomicU32 = AtomicU32::new(0);
/// Bumped ONLY by a media-key action. The compositor shows the OSD off this,
/// so dragging the tray slider does not make an OSD appear on top of the
/// slider the user is already looking at.
static KEYSEQ: AtomicU32 = AtomicU32::new(0);
/// Hardware is behind the state above.
static DIRTY: AtomicU32 = AtomicU32::new(0);
/// Diagnostics: how many times the deferred worker has applied a change.
static APPLIED: AtomicU32 = AtomicU32::new(0);

// ---------------------------------------------------------------------------
// PURE CORE. No atomics, no FFI, no hardware. This is what the self-test
// drives, which is why the self-test can run at boot without moving the
// speaker volume.
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct VolState {
    pub level: i32,
    pub muted: bool,
    pub pre_mute: i32,
}

pub const ACT_UP: i32 = 0;
pub const ACT_DOWN: i32 = 1;
pub const ACT_MUTE: i32 = 2;

fn clamp(v: i32) -> i32 {
    if v < 0 {
        0
    } else if v > 100 {
        100
    } else {
        v
    }
}

/// Explicit set (tray slider, Settings, profile restore). Setting a level
/// UNMUTES, which is what every desktop does and what the user means when
/// they drag a slider on a muted machine.
fn set_core(s: VolState, v: i32) -> VolState {
    VolState {
        level: clamp(v),
        muted: false,
        pre_mute: s.pre_mute,
    }
}

/// Explicit mute set (SYS_SET_MUTE). Muting REMEMBERS the level; it does not
/// zero it. Unmuting restores exactly what was remembered.
fn mute_core(s: VolState, mute: bool) -> VolState {
    if mute {
        if s.muted {
            s
        } else {
            VolState {
                level: s.level,
                muted: true,
                pre_mute: s.level,
            }
        }
    } else if s.muted {
        VolState {
            level: s.pre_mute,
            muted: false,
            pre_mute: s.pre_mute,
        }
    } else {
        s
    }
}

/// One media-key press.
///
/// Volume up/down on a MUTED machine unmutes first and then steps from the
/// restored level. Reaching for volume-up while muted means "I want to hear
/// this", and leaving it muted while the number climbs is the behaviour that
/// makes people press the key five more times.
///
/// The step SNAPS to a multiple of STEP, so a level arrived at from a slider
/// drag (say 47) walks 50, 55, 60 rather than 52, 57, 62. Down from 47 goes
/// to 45, not 42.
fn key_core(s: VolState, action: i32) -> VolState {
    match action {
        ACT_MUTE => mute_core(s, !s.muted),
        ACT_UP => {
            let base = if s.muted { mute_core(s, false) } else { s };
            let next = (base.level / STEP) * STEP + STEP;
            VolState {
                level: clamp(next),
                muted: false,
                pre_mute: base.pre_mute,
            }
        }
        ACT_DOWN => {
            let base = if s.muted { mute_core(s, false) } else { s };
            let next = if base.level % STEP != 0 {
                (base.level / STEP) * STEP
            } else {
                base.level - STEP
            };
            VolState {
                level: clamp(next),
                muted: false,
                pre_mute: base.pre_mute,
            }
        }
        _ => s,
    }
}

// ---------------------------------------------------------------------------
// LIVE STATE
// ---------------------------------------------------------------------------

fn load() -> VolState {
    VolState {
        level: LEVEL.load(Ordering::SeqCst) as i32,
        muted: MUTED.load(Ordering::SeqCst) != 0,
        pre_mute: PRE_MUTE.load(Ordering::SeqCst) as i32,
    }
}

/// Store, and report whether anything the user can hear or see actually moved.
fn store(old: VolState, new: VolState) -> bool {
    let changed = old.level != new.level || old.muted != new.muted;
    LEVEL.store(new.level as u32, Ordering::SeqCst);
    MUTED.store(if new.muted { 1 } else { 0 }, Ordering::SeqCst);
    PRE_MUTE.store(new.pre_mute as u32, Ordering::SeqCst);
    if changed {
        SEQ.fetch_add(1, Ordering::SeqCst);
        DIRTY.store(1, Ordering::SeqCst);
    }
    changed
}

/// A media key. SAFE IN HARD IRQ CONTEXT: atomics only, no lock, no wait, no
/// MMIO. Returns 1 if the caller should wake the apply worker.
///
/// KEYSEQ is bumped even when the level did not move (volume-up at 100), so
/// the OSD still appears and tells the user they are already at the top. An
/// OSD that silently does not appear reads as a broken key.
#[no_mangle]
pub extern "C" fn sysvol_key_rs(action: i32) -> i32 {
    let old = load();
    let new = key_core(old, action);
    let changed = store(old, new);
    KEYSEQ.fetch_add(1, Ordering::SeqCst);
    if changed {
        1
    } else {
        0
    }
}

/// SYS_SET_VOLUME. Returns 1 if the hardware needs applying.
#[no_mangle]
pub extern "C" fn sysvol_set_rs(level: i32) -> i32 {
    let old = load();
    let new = set_core(old, level);
    if store(old, new) {
        1
    } else {
        0
    }
}

/// SYS_SET_MUTE. Returns 1 if the hardware needs applying.
#[no_mangle]
pub extern "C" fn sysvol_mute_rs(mute: i32) -> i32 {
    let old = load();
    let new = mute_core(old, mute != 0);
    if store(old, new) {
        1
    } else {
        0
    }
}

/// SYS_GET_VOLUME. The REAL current level, not a constant.
#[no_mangle]
pub extern "C" fn sysvol_get_rs() -> i32 {
    LEVEL.load(Ordering::SeqCst) as i32
}

#[no_mangle]
pub extern "C" fn sysvol_muted_rs() -> i32 {
    MUTED.load(Ordering::SeqCst) as i32
}

/// Packed state for SYS_VOL_STATE, so the compositor learns level, mute and
/// both change counters in ONE syscall per frame rather than four.
///
///   bits  0..7   level 0-100
///   bit   8      muted
///   bits 16..31  seq     (any change, from any source)
///   bits 32..47  keyseq  (media-key-originated changes only)
///
/// Both counters are 16-bit wrapping; the compositor compares for INEQUALITY
/// with the value it last saw, never for ordering, so a wrap is a non-event.
#[no_mangle]
pub extern "C" fn sysvol_state_rs() -> u64 {
    let level = (LEVEL.load(Ordering::SeqCst) & 0xFF) as u64;
    let muted = ((MUTED.load(Ordering::SeqCst) & 1) as u64) << 8;
    let seq = ((SEQ.load(Ordering::SeqCst) & 0xFFFF) as u64) << 16;
    let keyseq = ((KEYSEQ.load(Ordering::SeqCst) & 0xFFFF) as u64) << 32;
    level | muted | seq | keyseq
}

/// The wait-queue condition for the apply worker. A PURE READ: it takes no
/// lock and drains nothing, so it is legal to evaluate with interrupts off,
/// which is how wait_event() evaluates it.
#[no_mangle]
pub extern "C" fn sysvol_dirty_rs() -> i32 {
    DIRTY.load(Ordering::SeqCst) as i32
}

/// THE ONE PLACE that touches the audio hardware. Called from the apply
/// worker (media keys) and directly from the syscall path (tray slider,
/// Settings), both in thread context. Returns 1 if it applied anything.
///
/// The dirty flag is cleared BEFORE the hardware writes, not after: a change
/// racing in during the write then leaves DIRTY set and gets applied on the
/// next pass, whereas clearing afterwards would swallow it. Applying the same
/// level twice is harmless; dropping the last change the user made is not.
#[no_mangle]
pub extern "C" fn sysvol_apply_rs() -> i32 {
    if DIRTY.swap(0, Ordering::SeqCst) == 0 {
        return 0;
    }
    let s = load();
    unsafe {
        sysvol_hw_set_mute(if s.muted { 1 } else { 0 });
        // Push the level even while muted. The level is the state the machine
        // returns to on unmute, and some backends (the SB16 path) implement
        // mute AS a level of 0, so re-asserting the level after an unmute is
        // what actually brings the sound back.
        if !s.muted {
            sysvol_hw_set_level(s.level);
        }
    }
    APPLIED.fetch_add(1, Ordering::SeqCst);
    1
}

/// Diagnostics for the boot log: how many deferred applies have happened.
#[no_mangle]
pub extern "C" fn sysvol_applied_count_rs() -> u32 {
    APPLIED.load(Ordering::SeqCst)
}

// ===========================================================================
// SELF-TEST
// ===========================================================================
// Drives the PURE CORE only, so running it at boot cannot move the speaker
// volume, and a failure is a failure of the logic rather than of whatever
// audio device happens to be present.

fn st(level: i32, muted: bool, pre_mute: i32) -> VolState {
    VolState {
        level,
        muted,
        pre_mute,
    }
}

fn check(cond: bool, fails: &mut u32) {
    if !cond {
        *fails += 1;
    }
}

/// Returns the number of FAILED checks (0 = pass). `out_checks` receives the
/// number of assertions made, so "0 failures" can be told apart from "the
/// test did not run", which is the distinction this tree keeps rediscovering.
#[no_mangle]
pub unsafe extern "C" fn sysvol_selftest_rs(out_checks: *mut u32) -> u32 {
    let mut checks: u32 = 0;
    let mut fails: u32 = 0;

    // ---- THE HEADLINE REQUIREMENT: mute then unmute is EXACTLY lossless ----
    // Not "close to", not "back to the default": the same integer, including
    // levels that are not multiples of the step and levels at the rails.
    for lvl in [0, 1, 3, 7, 33, 47, 50, 99, 100] {
        let a = st(lvl, false, 0);
        let m = key_core(a, ACT_MUTE);
        let u = key_core(m, ACT_MUTE);
        checks += 3;
        check(m.muted, &mut fails);
        check(!u.muted, &mut fails);
        check(u.level == lvl, &mut fails);
    }

    // Muting must NOT zero the level: the OSD draws the bar at its real
    // position with a mute badge, and a mute that clobbered the level would
    // make the round trip above pass by accident on a 0 start only.
    checks += 1;
    check(key_core(st(63, false, 0), ACT_MUTE).level == 63, &mut fails);

    // SYS_SET_MUTE (explicit, not a toggle) round-trips the same way.
    checks += 2;
    let em = mute_core(st(41, false, 0), true);
    check(em.muted && em.pre_mute == 41, &mut fails);
    check(mute_core(em, false).level == 41, &mut fails);

    // Double-mute must be idempotent: a second mute must not overwrite
    // pre_mute with the (equal) current level in a way that loses it, and a
    // second unmute must not step the level.
    checks += 2;
    let dm = mute_core(mute_core(st(37, false, 0), true), true);
    check(mute_core(dm, false).level == 37, &mut fails);
    check(mute_core(st(37, false, 0), false).level == 37, &mut fails);

    // ---- STEPPING ----------------------------------------------------------
    checks += 4;
    check(key_core(st(50, false, 0), ACT_UP).level == 55, &mut fails);
    check(key_core(st(50, false, 0), ACT_DOWN).level == 45, &mut fails);
    // Snapping: an off-grid level walks onto the grid, in the direction asked.
    check(key_core(st(47, false, 0), ACT_UP).level == 50, &mut fails);
    check(key_core(st(47, false, 0), ACT_DOWN).level == 45, &mut fails);

    // Rails: never below 0, never above 100, and pressing at the rail is a
    // no-op rather than a wrap.
    checks += 4;
    check(key_core(st(0, false, 0), ACT_DOWN).level == 0, &mut fails);
    check(key_core(st(100, false, 0), ACT_UP).level == 100, &mut fails);
    check(key_core(st(2, false, 0), ACT_DOWN).level == 0, &mut fails);
    check(key_core(st(98, false, 0), ACT_UP).level == 100, &mut fails);

    // A full walk down from 100 must terminate at exactly 0 and a full walk
    // up from 0 at exactly 100 - no drift, no oscillation, no infinite tail.
    checks += 2;
    let mut s = st(100, false, 0);
    for _ in 0..40 {
        s = key_core(s, ACT_DOWN);
    }
    check(s.level == 0, &mut fails);
    let mut s2 = st(0, false, 0);
    for _ in 0..40 {
        s2 = key_core(s2, ACT_UP);
    }
    check(s2.level == 100, &mut fails);

    // ---- VOLUME KEYS WHILE MUTED -------------------------------------------
    // Up while muted unmutes and steps from the RESTORED level (60 -> 65),
    // not from 0 and not from the current-level-while-muted.
    checks += 3;
    let mu = key_core(st(60, true, 60), ACT_UP);
    check(!mu.muted, &mut fails);
    check(mu.level == 65, &mut fails);
    let md = key_core(st(60, true, 60), ACT_DOWN);
    check(!md.muted && md.level == 55, &mut fails);

    // ---- EXPLICIT SET ------------------------------------------------------
    checks += 4;
    check(set_core(st(10, false, 0), 73).level == 73, &mut fails);
    check(set_core(st(10, false, 0), -5).level == 0, &mut fails);
    check(set_core(st(10, false, 0), 250).level == 100, &mut fails);
    // Setting a level on a muted machine unmutes it, or the slider appears
    // to do nothing.
    check(!set_core(st(10, true, 10), 40).muted, &mut fails);

    // ---- An unknown action must be a no-op, not a silent mutation ----------
    checks += 1;
    let before = st(42, false, 7);
    let after = key_core(before, 99);
    check(after.level == 42 && !after.muted && after.pre_mute == 7, &mut fails);

    if !out_checks.is_null() {
        *out_checks = checks;
    }
    fails
}
