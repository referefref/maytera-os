// rustkern/hidmap.rs - #763: THE HID-usage -> PS/2 set-1 scancode translation.
// One table, for every HID transport we have.
//
// WHY THIS EXISTS AS A SHARED MODULE
// ==========================================================================
// USB HID and Bluetooth HID keyboards report boot-protocol USAGE codes. The
// whole rest of MayteraOS consumes PS/2 set-1 SCANCODES: the cooked key ring,
// the modifier state, the Win16 layer's VK translation and the DOS guest's
// INT 9 / BDA key ring all speak set 1. Something has to translate, and until
// this module there were TWO somethings:
//
//   drivers/usb_hid.c   hid_to_set1[] + hid_ext_set1() + emit_set1()
//   bt/hid.c            hid_to_set1[] + hid_ext_set1() + emit_set1()
//
// byte-for-byte copies of each other, the second carrying the comment "the
// usage->set-1 table below mirrors drivers/usb_hid.c ... for now it is
// duplicated here with this pointer to the canonical copy". That is the same
// shape as the four handle tables (#772) and the two Task Managers: a copy
// with a note promising a merge is still a copy, and it drifts. They had
// ALREADY drifted in formatting, and both carried the same bug (below).
//
// WHAT WAS WRONG WITH THE COPIES, and it is not cosmetic:
//
//   * KEYPAD / (usage 0x54) was mapped to a BARE 0x35, the main-block '/'.
//     On real set-1 hardware keypad / is E0 35. A DOS game that reads raw
//     scancodes therefore could not tell the keypad slash from the main one,
//     and neither could anything else downstream.
//   * THE ENTIRE NUMERIC KEYPAD (usages 0x59..0x63, keypad 1-9, 0 and '.')
//     was ABSENT, so it produced no scancode at all. DOS-era games use the
//     keypad for movement constantly; that is not a corner case for us.
//   * Scroll Lock (0x47), the non-US backslash (0x64), the Application/Menu
//     key (0x65) and the non-US '#' (0x32) were also absent.
//
// New shared kernel logic with no C twin to strangle, so Rust per the
// 2026-07-16 rule. There is no `_c` reference and no [RUST-DIFF] differential
// because there is nothing to differ FROM: the two C copies are DELETED by
// this change, not kept as a reference arm. What proves it instead is
// hidmap_selftest_rs(), a vector test over the cases that actually decide
// behaviour, run on every boot with one line of output. Say which kind of
// evidence you have: a differential proves "same as the C", a vector test
// proves "matches the set-1 spec on the cases named". This is the second kind,
// and for three of those vectors the old C was WRONG, so a differential would
// have been the wrong instrument anyway.
//
// PRINT SCREEN AND PAUSE ARE DELIBERATELY UNMAPPED. In set 1 they are not a
// prefix plus a code, they are fixed multi-byte sequences (E0 2A E0 37 ... and
// E1 1D 45 E1 9D C5), which do not fit a (code, extended) pair and which
// nothing in this OS consumes. Returning 0 for them is honest; inventing a
// single-byte code for them would not be.

/// Translate a HID keyboard usage code to a PS/2 set-1 MAKE code.
///
/// `usage` is a boot-protocol usage from the report's keycode array, or one of
/// the eight modifier pseudo-usages 0xE0..=0xE7 that the callers synthesise
/// from the report's modifier byte (bit i -> usage 0xE0 + i).
///
/// Returns the set-1 make code, or 0 when there is no mapping. `*out_ext` is
/// set to 1 when the code must be emitted behind a 0xE0 prefix and 0 when it
/// must not. The break code is the caller's job: set 1 forms it as
/// `make | 0x80`, after the same prefix.
///
/// # Safety
/// `out_ext` must be a valid, writable `*mut u8`. It is written exactly once
/// and read never. Passing null is a caller bug and is checked for anyway.
#[no_mangle]
pub extern "C" fn hid_usage_to_set1_rs(usage: u8, out_ext: *mut u8) -> u8 {
    let (code, ext) = map(usage);
    if !out_ext.is_null() {
        // SAFETY: checked non-null; a single u8 store into caller-owned memory.
        unsafe { *out_ext = ext };
    }
    code
}

// (set-1 make code, extended). 0 means "no mapping"; ext is meaningless then.
fn map(usage: u8) -> (u8, u8) {
    match usage {
        // ---- letters a..z (usage 0x04..0x1D) ----------------------------
        0x04 => (0x1E, 0), 0x05 => (0x30, 0), 0x06 => (0x2E, 0), 0x07 => (0x20, 0),
        0x08 => (0x12, 0), 0x09 => (0x21, 0), 0x0A => (0x22, 0), 0x0B => (0x23, 0),
        0x0C => (0x17, 0), 0x0D => (0x24, 0), 0x0E => (0x25, 0), 0x0F => (0x26, 0),
        0x10 => (0x32, 0), 0x11 => (0x31, 0), 0x12 => (0x18, 0), 0x13 => (0x19, 0),
        0x14 => (0x10, 0), 0x15 => (0x13, 0), 0x16 => (0x1F, 0), 0x17 => (0x14, 0),
        0x18 => (0x16, 0), 0x19 => (0x2F, 0), 0x1A => (0x11, 0), 0x1B => (0x2D, 0),
        0x1C => (0x15, 0), 0x1D => (0x2C, 0),

        // ---- digit row 1..9 0 (0x1E..0x27) ------------------------------
        0x1E => (0x02, 0), 0x1F => (0x03, 0), 0x20 => (0x04, 0), 0x21 => (0x05, 0),
        0x22 => (0x06, 0), 0x23 => (0x07, 0), 0x24 => (0x08, 0), 0x25 => (0x09, 0),
        0x26 => (0x0A, 0), 0x27 => (0x0B, 0),

        // ---- the main block's control and punctuation keys ---------------
        0x28 => (0x1C, 0),   // Enter
        0x29 => (0x01, 0),   // Escape
        0x2A => (0x0E, 0),   // Backspace
        0x2B => (0x0F, 0),   // Tab
        0x2C => (0x39, 0),   // Space
        0x2D => (0x0C, 0),   // - _
        0x2E => (0x0D, 0),   // = +
        0x2F => (0x1A, 0),   // [ {
        0x30 => (0x1B, 0),   // ] }
        0x31 => (0x2B, 0),   // \ |
        // Non-US # and ~. Physically the same key as \ on the keyboards that
        // have it at all, and every other OS maps it to the same code.
        0x32 => (0x2B, 0),
        0x33 => (0x27, 0),   // ; :
        0x34 => (0x28, 0),   // ' "
        0x35 => (0x29, 0),   // ` ~
        0x36 => (0x33, 0),   // , <
        0x37 => (0x34, 0),   // . >
        0x38 => (0x35, 0),   // / ?
        0x39 => (0x3A, 0),   // Caps Lock

        // ---- F1..F12 -----------------------------------------------------
        0x3A => (0x3B, 0), 0x3B => (0x3C, 0), 0x3C => (0x3D, 0), 0x3D => (0x3E, 0),
        0x3E => (0x3F, 0), 0x3F => (0x40, 0), 0x40 => (0x41, 0), 0x41 => (0x42, 0),
        0x42 => (0x43, 0), 0x43 => (0x44, 0), 0x44 => (0x57, 0), 0x45 => (0x58, 0),

        // 0x46 Print Screen: multi-byte in set 1, see the header note.
        0x47 => (0x46, 0),   // Scroll Lock
        // 0x48 Pause: multi-byte in set 1, see the header note.

        // ---- the E0-prefixed navigation island ---------------------------
        0x49 => (0x52, 1),   // Insert
        0x4A => (0x47, 1),   // Home
        0x4B => (0x49, 1),   // Page Up
        0x4C => (0x53, 1),   // Delete
        0x4D => (0x4F, 1),   // End
        0x4E => (0x51, 1),   // Page Down
        0x4F => (0x4D, 1),   // Right Arrow
        0x50 => (0x4B, 1),   // Left Arrow
        0x51 => (0x50, 1),   // Down Arrow
        0x52 => (0x48, 1),   // Up Arrow

        // ---- the numeric keypad ------------------------------------------
        0x53 => (0x45, 0),   // Num Lock
        // Keypad / is E0 35, NOT a bare 0x35. The bare form is the main
        // block's '/' and the two C copies conflated them.
        0x54 => (0x35, 1),   // Keypad /
        0x55 => (0x37, 0),   // Keypad *
        0x56 => (0x4A, 0),   // Keypad -
        0x57 => (0x4E, 0),   // Keypad +
        0x58 => (0x1C, 1),   // Keypad Enter (E0 1C)
        // The keypad digits share their make codes with the navigation island
        // above and are told apart from it by the ABSENCE of the E0 prefix.
        // That is exactly how set 1 works, and it is why these must not be
        // marked extended.
        0x59 => (0x4F, 0),   // Keypad 1 / End
        0x5A => (0x50, 0),   // Keypad 2 / Down
        0x5B => (0x51, 0),   // Keypad 3 / PgDn
        0x5C => (0x4B, 0),   // Keypad 4 / Left
        0x5D => (0x4C, 0),   // Keypad 5
        0x5E => (0x4D, 0),   // Keypad 6 / Right
        0x5F => (0x47, 0),   // Keypad 7 / Home
        0x60 => (0x48, 0),   // Keypad 8 / Up
        0x61 => (0x49, 0),   // Keypad 9 / PgUp
        0x62 => (0x52, 0),   // Keypad 0 / Insert
        0x63 => (0x53, 0),   // Keypad . / Delete

        0x64 => (0x56, 0),   // Non-US \ and | (the extra key on ISO layouts)
        0x65 => (0x5D, 1),   // Application / Menu
        // 0x66 Power, 0x67 Keypad =: no set-1 code exists. Left unmapped.

        // ---- the eight modifiers, passed in as 0xE0 + bit index ----------
        0xE0 => (0x1D, 0),   // Left Ctrl
        0xE1 => (0x2A, 0),   // Left Shift
        0xE2 => (0x38, 0),   // Left Alt
        0xE3 => (0x5B, 1),   // Left GUI
        0xE4 => (0x1D, 1),   // Right Ctrl
        0xE5 => (0x36, 0),   // Right Shift
        0xE6 => (0x38, 1),   // Right Alt / AltGr
        0xE7 => (0x5C, 1),   // Right GUI

        _ => (0x00, 0),
    }
}

// ---------------------------------------------------------------------------
// Boot self-test. Vectors chosen for the cases that DECIDE behaviour, not for
// coverage arithmetic: everything the 0xE0 prefix distinguishes, everything
// the old C copies got wrong, and the unmapped codes that must stay 0.
// Returns the number of FAILURES (0 = pass) and writes the number of checks.
// ---------------------------------------------------------------------------
const VECTORS: [(u8, u8, u8); 34] = [
    // plain main-block keys must NOT be extended
    (0x04, 0x1E, 0),   // a
    (0x1D, 0x2C, 0),   // z
    (0x1E, 0x02, 0),   // 1
    (0x27, 0x0B, 0),   // 0
    (0x28, 0x1C, 0),   // Enter
    (0x29, 0x01, 0),   // Escape
    (0x2C, 0x39, 0),   // Space
    (0x38, 0x35, 0),   // main-block '/'
    (0x3A, 0x3B, 0),   // F1
    (0x45, 0x58, 0),   // F12
    // the navigation island must ALL be extended
    (0x49, 0x52, 1),   // Insert
    (0x4A, 0x47, 1),   // Home
    (0x4B, 0x49, 1),   // Page Up
    (0x4C, 0x53, 1),   // Delete
    (0x4D, 0x4F, 1),   // End
    (0x4E, 0x51, 1),   // Page Down
    (0x4F, 0x4D, 1),   // Right
    (0x50, 0x4B, 1),   // Left
    (0x51, 0x50, 1),   // Down
    (0x52, 0x48, 1),   // Up
    // the keypad: Enter and / extended, digits NOT
    (0x54, 0x35, 1),   // Keypad / (the bug in both C copies)
    (0x58, 0x1C, 1),   // Keypad Enter
    (0x55, 0x37, 0),   // Keypad *
    (0x59, 0x4F, 0),   // Keypad 1 (absent from both C copies)
    (0x5C, 0x4B, 0),   // Keypad 4
    (0x62, 0x52, 0),   // Keypad 0
    (0x63, 0x53, 0),   // Keypad .
    // modifiers: the right-hand ones that are extended, the left ones that are not
    (0xE0, 0x1D, 0),   // Left Ctrl
    (0xE4, 0x1D, 1),   // Right Ctrl
    (0xE2, 0x38, 0),   // Left Alt
    (0xE6, 0x38, 1),   // Right Alt
    (0xE1, 0x2A, 0),   // Left Shift
    (0xE5, 0x36, 0),   // Right Shift
    (0xE7, 0x5C, 1),   // Right GUI
];

// Usages that must stay unmapped, so a caller never emits a scancode for them.
const UNMAPPED: [u8; 8] = [0x00, 0x01, 0x02, 0x03, 0x46, 0x48, 0x66, 0x67];

/// Run the vector test. Returns the failure count; `*out_checks` gets the
/// number of assertions made. Called once from usb_hid_init().
///
/// # Safety
/// `out_checks` must be a valid writable `*mut u32` or null.
#[no_mangle]
pub extern "C" fn hidmap_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut fails: i32 = 0;
    let mut checks: u32 = 0;

    let mut i = 0;
    while i < VECTORS.len() {
        let (usage, want_code, want_ext) = VECTORS[i];
        let (code, ext) = map(usage);
        checks += 2;
        if code != want_code { fails += 1; }
        if ext != want_ext { fails += 1; }
        i += 1;
    }

    let mut j = 0;
    while j < UNMAPPED.len() {
        let (code, _) = map(UNMAPPED[j]);
        checks += 1;
        if code != 0 { fails += 1; }
        j += 1;
    }

    // A structural invariant, not a vector: a (code, extended) pair must
    // identify at most one usage, EXCEPT for the two documented aliases (the
    // non-US '#' shares the backslash key, and each keypad digit deliberately
    // shares its make code with the navigation key it doubles for, told apart
    // by the prefix). Anything else colliding means two different keys became
    // indistinguishable downstream, which is precisely the keypad-/ bug.
    let mut u: u16 = 0;
    while u < 256 {
        let a = u as u8;
        let (ca, ea) = map(a);
        if ca != 0 && a != 0x32 {
            let mut v: u16 = u + 1;
            while v < 256 {
                let b = v as u8;
                let (cb, eb) = map(b);
                if cb != 0 && b != 0x32 && ca == cb && ea == eb {
                    // keypad digit vs its own navigation twin is impossible
                    // here because those differ in `ext`; any hit is a real
                    // collision.
                    fails += 1;
                }
                v += 1;
            }
            checks += 1;
        }
        u += 1;
    }

    if !out_checks.is_null() {
        // SAFETY: checked non-null; single u32 store into caller-owned memory.
        unsafe { *out_checks = checks };
    }
    fails
}
