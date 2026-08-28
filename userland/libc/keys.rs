// keys.rs - THE keycode table for MayteraOS Rust userland apps (#191).
//
// This is the Rust face of userland/libc/keys.h. Read that file for WHY these
// values are what they are: an app receives COOKED keycodes, not PS/2
// scancodes, and every app that declared its own copy of the arrow codes got
// them wrong by copying a neighbour that already had them wrong (#188, #191).
//
// build/keycode-gate.sh compares this file against keys.h NAME BY NAME AND
// VALUE BY VALUE and fails the golden build on any disagreement, so the two
// cannot drift. There is no third copy; do not create one.
//
// Include it the same way the Rust apps include their other shared modules:
//     #[path = "../../libc/keys.rs"]
//     mod keys;
// then refer to keys::GUI_KEY_UP and friends. Items are u32 because
// gui_event_t.keycode is u32 on this side of the FFI.
#![allow(dead_code)]

// --- REMAPPED keys: the scancode NEVER reaches an app. -----------------------
pub const GUI_KEY_UP: u32 = 0x80; // NOT 0x48 (0x48 is ASCII 'H')
pub const GUI_KEY_DOWN: u32 = 0x81; // NOT 0x50 (0x50 is ASCII 'P')
pub const GUI_KEY_LEFT: u32 = 0x82; // NOT 0x4B (0x4B is ASCII 'K')
pub const GUI_KEY_RIGHT: u32 = 0x83; // NOT 0x4D (0x4D is ASCII 'M')

pub const GUI_KEY_F1: u32 = 0x88;
pub const GUI_KEY_F2: u32 = 0x89;
pub const GUI_KEY_F3: u32 = 0x8B;
pub const GUI_KEY_F4: u32 = 0x8C;
pub const GUI_KEY_F5: u32 = 0x84;
pub const GUI_KEY_F6: u32 = 0x8A;
pub const GUI_KEY_F7: u32 = 0x8D;
pub const GUI_KEY_F8: u32 = 0x8E;
pub const GUI_KEY_F9: u32 = 0x8F;
pub const GUI_KEY_F10: u32 = 0x87;
pub const GUI_KEY_F11: u32 = 0x85;
pub const GUI_KEY_F12: u32 = 0x86;

pub const GUI_KEY_LSHIFT: u32 = 0x95;
pub const GUI_KEY_RSHIFT: u32 = 0x96;
pub const GUI_KEY_LCTRL: u32 = 0x99;
pub const GUI_KEY_ALT: u32 = 0x9A;
pub const GUI_KEY_SUPER: u32 = 0x9B;
pub const GUI_KEY_PRTSC: u32 = 0x9D;

// #243: RELEASE codes. Irregular (arrows are press+0x10, modifiers are not),
// so they are declared, not computed. Mirrors keys.h; the gate compares the
// two files name-for-name and value-for-value.
pub const GUI_KEY_UP_REL: u32 = 0x90;
pub const GUI_KEY_DOWN_REL: u32 = 0x91;
pub const GUI_KEY_LEFT_REL: u32 = 0x92;
pub const GUI_KEY_RIGHT_REL: u32 = 0x93;
pub const GUI_KEY_LCTRL_UP: u32 = 0x94;
pub const GUI_KEY_LSHIFT_UP: u32 = 0x97;
pub const GUI_KEY_RSHIFT_UP: u32 = 0x98;
pub const GUI_KEY_ALT_UP: u32 = 0x9C;

// #221 phase 0: the values a modifier release ACTUALLY arrives with in
// gui_event_t.keycode, after SYS_INJECT_KEY rewrites the pushed code above.
// #232: these now ROUND-TRIP to their own press code (they used to be 0x87 /
// 0x88 / 0x84 / 0x1C, of which the first three were the PRESS codes of F10, F1
// and F5). See the long note in keys.h. EVENT_KEY_UP only.
pub const GUI_KEY_LSHIFT_DELIVERED_REL: u32 = 0x95;
pub const GUI_KEY_RSHIFT_DELIVERED_REL: u32 = 0x96;
pub const GUI_KEY_LCTRL_DELIVERED_REL: u32 = 0x99;
pub const GUI_KEY_ALT_DELIVERED_REL: u32 = 0x9A;

// --- PASS-THROUGH keys: here the raw make code IS what arrives. --------------
pub const GUI_KEY_HOME: u32 = 0x100;
pub const GUI_KEY_PGUP: u32 = 0x102;
pub const GUI_KEY_END: u32 = 0x101;
pub const GUI_KEY_PGDN: u32 = 0x103;
pub const GUI_KEY_INS: u32 = 0x104;
pub const GUI_KEY_DEL: u32 = 0x105;

// --- ORDINARY keys arrive as their ASCII byte in keycode AND key_char. -------
pub const GUI_KEY_ESC: u32 = 0x1B;
pub const GUI_KEY_ENTER: u32 = 0x0A;
pub const GUI_KEY_TAB: u32 = 0x09;
pub const GUI_KEY_BKSP: u32 = 0x08;
