// rustkern/presentscale.rs - pure decision logic for integer PRESENT-SCALE
// compositing (#halfres).
//
// TASK: one owner's panel is 3840x2160 with only seven firmware modes, of
// which the sole 16:9 one is native 4K (the rest are 1920x1440, 1600x1200,
// 1280x1024, 1024x768, 800x600, 640x480 - all 4:3/5:4). Runtime GOP mode
// setting is unavailable (SetMode is gone after ExitBootServices) and a
// native Intel display driver was scoped and rejected as multi-month. He is
// ALREADY running the UI at 200% scale, i.e. every widget is already drawn
// at double size into the full 8.29 Mpx panel. Compositing at 1920x1080
// (scale 100%) and presenting with an EXACT integer 2x replication produces
// the identical apparent picture on screen for a quarter of the compositing
// work - sharp, because it is replication, not the resampled "virtual
// resolution" idea a prior investigation measured and rejected as both
// slower and soft.
//
// THIS FILE ANSWERS EXACTLY ONE QUESTION: given a physical panel of
// `phys_w`x`phys_h` and a requested integer factor `n`, is `n` usable? The
// FS/config plumbing (kernel/gui/presentscale.c, mirroring uiscale.c) and the
// actual per-pixel replication (kernel/video/framebuffer.c, mirroring its own
// display-rotation present path) live elsewhere. This split mirrors uiscale.c
// (C plumbing) / uiscale.rs (Rust arithmetic) one file over, for the same
// reason: the decision is pure integer arithmetic with a property self-test,
// with no FS I/O and no entanglement with the cli/CR3-switch present
// chokepoint that keeps the REPLICATION loop in C (see framebuffer.c's
// comment on fb_present_rect_scaled for that half of the justification).
//
// WHY RUST: no stated performance or entanglement reason not to be - the
// 2026-07-16 standing rule's default applies cleanly here, exactly as it did
// for uiscale.rs's own arithmetic.
//
// THE RULES, EVERY ONE STATED BECAUSE EACH ONE IS A WAY THIS FEATURE COULD
// HAVE SHIPPED WRONG:
//
//   n <= 1            always valid: this is the OFF state, and a no-op must
//                     never be refused or every machine that never asked for
//                     this feature could somehow fail to have it off.
//   NOT an exact       REFUSED, not resampled. Resampling softness is exactly
//   divisor            what this feature exists to avoid (see the module
//                     comment above); a factor that does not divide evenly
//                     would require it, so it is refused instead.
//   below the floor    REFUSED. Re-exports uiscale.rs's OWN MIN_LOGICAL_W/H
//                     rather than copying the numbers, because a logical
//                     floor that could drift between the two features that
//                     both depend on it is exactly the "one value, several
//                     places" fault this project's worst bugs come from
//                     (dock_opacity.h's seven copies; uiscale.rs's own
//                     header makes the same argument for itself).
//   rotation active    REFUSED unconditionally. The 90/270 present-scale
//                     transpose and this integer-replication present have
//                     never been exercised together, rotation is a genuine
//                     one-off (a single physically-mounted-rotated panel),
//                     and this feature does not need to be the one that
//                     finds a combination bug live on a machine no test can
//                     reach.

use crate::uiscale::{MIN_LOGICAL_H, MIN_LOGICAL_W};

/// Is factor `n` usable on a physical panel of `phys_w` x `phys_h`, given
/// whether display rotation is currently active? `rotation_active` is a C
/// bool passed in (0/1) rather than this module reading fb_rotation itself,
/// keeping this file free of any FFI beyond the pure inputs it needs.
#[no_mangle]
pub extern "C" fn presentscale_valid_rs(
    phys_w: i32,
    phys_h: i32,
    n: i32,
    rotation_active: i32,
) -> i32 {
    if n <= 1 {
        return 1;
    }
    if phys_w <= 0 || phys_h <= 0 {
        return 0;
    }
    if rotation_active != 0 {
        return 0;
    }
    if phys_w % n != 0 || phys_h % n != 0 {
        return 0;
    }
    let lw = phys_w / n;
    let lh = phys_h / n;
    if lw < MIN_LOGICAL_W || lh < MIN_LOGICAL_H {
        return 0;
    }
    1
}

/// The logical width/height a factor would produce. Callers MUST check
/// presentscale_valid_rs first; these do not re-validate (kept as a single
/// obviously-correct divide each, rather than duplicating the validator's
/// branches here where the two could drift apart).
#[no_mangle]
pub extern "C" fn presentscale_logical_w_rs(phys_w: i32, n: i32) -> i32 {
    if n <= 1 { phys_w } else { phys_w / n }
}
#[no_mangle]
pub extern "C" fn presentscale_logical_h_rs(phys_h: i32, n: i32) -> i32 {
    if n <= 1 { phys_h } else { phys_h / n }
}

// ===========================================================================
// SELF-TEST. Runs on every boot (presentscale_init() in presentscale.c calls
// this BEFORE trusting the validator for a real decision - same posture
// uiscale_init() takes with uiscale_selftest_rs()). Returns 0 on pass;
// otherwise a bitmask naming which property failed.
// ===========================================================================

pub const PST_OFF_ALWAYS_VALID: u32 = 1 << 0;
pub const PST_EXACT_DIVISOR: u32 = 1 << 1;
pub const PST_FLOOR: u32 = 1 << 2;
pub const PST_ROTATION_REFUSED: u32 = 1 << 3;
pub const PST_OWNER_CASE: u32 = 1 << 4;

#[no_mangle]
pub extern "C" fn presentscale_selftest_rs() -> u32 {
    let mut bad: u32 = 0;

    // OFF is always valid, for any dims including garbage - a no-op must
    // never be refused, or a machine that never touched this feature could
    // somehow end up unable to boot with it off.
    if presentscale_valid_rs(0, 0, 1, 0) != 1 { bad |= PST_OFF_ALWAYS_VALID; }
    if presentscale_valid_rs(0, 0, 0, 0) != 1 { bad |= PST_OFF_ALWAYS_VALID; }
    if presentscale_valid_rs(3840, 2160, 1, 1) != 1 { bad |= PST_OFF_ALWAYS_VALID; }

    // EXACT DIVISOR: the entire point of "integer factors only, refuse rather
    // than resample".
    if presentscale_valid_rs(3840, 2160, 2, 0) != 1 { bad |= PST_EXACT_DIVISOR; }
    if presentscale_valid_rs(2560, 1600, 2, 0) != 1 { bad |= PST_EXACT_DIVISOR; }
    if presentscale_valid_rs(1920, 1081, 2, 0) != 0 { bad |= PST_EXACT_DIVISOR; } // odd height
    if presentscale_valid_rs(3841, 2160, 2, 0) != 0 { bad |= PST_EXACT_DIVISOR; } // odd width
    if presentscale_valid_rs(3840, 2160, 7, 0) != 0 { bad |= PST_EXACT_DIVISOR; } // not a divisor

    // FLOOR: a factor that divides exactly but leaves too little logical room
    // must still be refused. 1920x1080 at 2x is 960x540 - both axes under the
    // 1024x600 floor.
    if presentscale_valid_rs(1920, 1080, 2, 0) != 0 { bad |= PST_FLOOR; }
    // 2048x1200 at 2x is EXACTLY 1024x600: at the floor, not under it - valid.
    // Must match uiscale.rs's own ">= floor" convention, not "> floor".
    if presentscale_valid_rs(2048, 1200, 2, 0) != 1 { bad |= PST_FLOOR; }
    if presentscale_valid_rs(2046, 1200, 2, 0) != 0 { bad |= PST_FLOOR; } // 1023 wide: just under
    if presentscale_valid_rs(2048, 1198, 2, 0) != 0 { bad |= PST_FLOOR; } // 599 tall: just under

    // ROTATION: refused unconditionally while a rotated panel is active, even
    // for an otherwise-perfect factor.
    if presentscale_valid_rs(3840, 2160, 2, 1) != 0 { bad |= PST_ROTATION_REFUSED; }
    if presentscale_valid_rs(2560, 1600, 3, 1) != 0 { bad |= PST_ROTATION_REFUSED; }

    // THE OWNER'S ACTUAL CASE, asserted by its own numbers: a native
    // 3840x2160 panel, factor 2, no rotation -> valid, logical EXACTLY
    // 1920x1080 - the number this whole ticket exists to produce.
    if presentscale_valid_rs(3840, 2160, 2, 0) != 1 { bad |= PST_OWNER_CASE; }
    if presentscale_logical_w_rs(3840, 2) != 1920 { bad |= PST_OWNER_CASE; }
    if presentscale_logical_h_rs(2160, 2) != 1080 { bad |= PST_OWNER_CASE; }
    // The demo shape used for verification because QEMU/OVMF cannot present
    // real 4K (confirmed repeatedly: it stops at 2560x1600) - same mechanism,
    // honestly a different physical size.
    if presentscale_valid_rs(2560, 1600, 2, 0) != 1 { bad |= PST_OWNER_CASE; }
    if presentscale_logical_w_rs(2560, 2) != 1280 { bad |= PST_OWNER_CASE; }
    if presentscale_logical_h_rs(1600, 2) != 800 { bad |= PST_OWNER_CASE; }

    bad
}
