// isr.c - Interrupt Service Routines implementation
#include "isr.h"
#include "mono.h"
#include "pic.h"
#include "../serial.h"
#include "../proc/process.h"
#include "../drivers/keymod.h"   // KEY_MOD_*: one definition, shared with drivers/keyboard.h
#include "../sync/spinlock.h"  // shared lock primitive: the DOS tap now has >1 writer

// Timer tick count
volatile uint64_t timer_ticks = 0;

// Interrupt flag (Ctrl+C)
volatile int interrupt_requested = 0;

// Keyboard buffer
#define KEYBOARD_BUFFER_SIZE 256
static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint16_t kb_read_idx = 0;
static volatile uint16_t kb_write_idx = 0;

// #334: total cooked keys drained by keyboard_get_char(), so the serial test-
// input channel can confirm an injected key was actually CONSUMED, not just
// queued. Monotonic; harmless to wrap.
volatile uint64_t g_kbd_consumed = 0;

// #334: raw scancode BYTES seen by the IRQ1 handler at port 0x60. If this does
// not advance under qm sendkey, QEMU never delivered the scancode (the drop is
// QEMU-side, not in our ring).
volatile uint64_t g_kbd_irq_scancodes = 0;

// ---- Raw scancode tap (#202 DOS games) -----------------------------------
// When a DOS task (dos/dosexec.c) is running it sets g_dos_scancode_tap=1.
// The keyboard ISR then mirrors every RAW scancode byte (press, release, and
// 0xE0 extended prefix) into this ring so the DOS layer can replay it through
// the guest's INT 9 handler / port 0x60 exactly like real hardware. This is
// what id Software's Galaxy engine (Commander Keen 4-6) needs: it installs its
// own INT 9 ISR that reads port 0x60 and maintains a Keyboard[] scancode array.
#define DOS_SC_RING 256
volatile int      g_dos_scancode_tap = 0;
static volatile uint8_t  dos_sc_ring[DOS_SC_RING];
static volatile uint16_t dos_sc_rd = 0, dos_sc_wr = 0;
// #763: the ring used to have exactly ONE producer (the IRQ1 ISR), so an
// unlocked SPSC push was sound. It now has several (see dos_scancode_tap
// below), and a producer running in a thread can be preempted mid-push by the
// IRQ1 ISR producing on the same ring. Guard the WRITE side only: the reader
// (dos_scancode_get, called from the DOS interpreter thread) is still the sole
// consumer, so reader-vs-writer stays exactly the SPSC pairing it always was.
// The critical section is four instructions and never sleeps.
static spinlock_t dos_sc_lock = SPINLOCK_INIT_NAMED("dos_sc_ring");
static void dos_sc_push(uint8_t b) {
    uint64_t fl = spinlock_acquire_irqsave(&dos_sc_lock);
    uint16_t nx = (uint16_t)((dos_sc_wr + 1) % DOS_SC_RING);
    if (nx != dos_sc_rd) { dos_sc_ring[dos_sc_wr] = b; dos_sc_wr = nx; }
    spinlock_release_irqrestore(&dos_sc_lock, fl);
}

// #763: THE ONE PLACE A SET-1 SCANCODE BYTE ENTERS THE DOS TAP.
//
// It used to be a single call inside keyboard_handler(), the PS/2 IRQ1 ISR,
// and that was the whole bug: on any machine whose keyboard is USB (which is
// EVERY real iMac, and the #307 target) IRQ1 never fires, so nothing ever
// pushed and every DOS guest waited forever for a key. Not just guests with
// their own INT 9 handler either: dos_keyq_pump() fills the guest's BDA
// keyboard ring from this same tap, so INT 16h, the INT 21h console input
// group and a direct BDA read were ALL dead on a USB keyboard.
//
// The fix is not "call it from usb_hid.c too", which would have left the
// Bluetooth HID path (bt/hid.c) and the #334 test-input channel broken in
// exactly the same way and invited a fourth copy. It is called from
// keyboard_process_scancode(), which is ALREADY the single function every
// scancode source in the kernel funnels through:
//
//   cpu/isr.c        keyboard_handler()      real PS/2 IRQ1
//   cpu/isr.c        keyboard_poll_i8042()   polled i8042 (early boot, IF=0)
//   drivers/usb_hid.c emit_set1()            USB HID keyboards
//   bt/hid.c          emit_set1()            Bluetooth HID keyboards
//   drivers/testinput.c                      the #334 host->guest test channel
//
// so a DOS guest now sees keys from all five, and any SIXTH source added later
// is tapped by construction rather than by someone remembering.
//
// THE CONTROLLER STATUS-BYTE FILTER IS NOT DUPLICATED HERE ON PURPOSE. 0xFA
// (ACK), 0xFE (resend), 0xFC/0x00/0xFF (self-test / error / overrun) and 0xEE
// (echo) are i8042 CONTROLLER responses, not scancodes; they cannot occur on
// the USB or Bluetooth paths at all. keyboard_process_scancode() already
// drops them at its top, and this is called AFTER that early return, so the
// bytes reaching the tap are byte-identical to what the PS/2 path pushed
// before this change, with one filter in the kernel instead of two copies.
static void dos_scancode_tap(uint8_t b) {
    if (!g_dos_scancode_tap) return;
    dos_sc_push(b);
}
// Drain one raw scancode byte. Returns -1 if empty.
int dos_scancode_get(void) {
    if (dos_sc_rd == dos_sc_wr) return -1;
    uint8_t b = dos_sc_ring[dos_sc_rd];
    dos_sc_rd = (uint16_t)((dos_sc_rd + 1) % DOS_SC_RING);
    return (int)b;
}
void dos_scancode_clear(void) { dos_sc_rd = dos_sc_wr = 0; }

// US keyboard scancode to ASCII mapping (set 1)
static const char scancode_to_ascii[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*',  0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,   0,
};

// Shifted characters
static const char scancode_to_ascii_shift[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*',  0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,   0,
};

// ---------------------------------------------------------------------------
// The two shared keyboard primitives. drivers/keyboard.h has DECLARED both
// since the driver was written and NOTHING EVER DEFINED THEM, so the first
// caller to use one got a link error rather than a working function. That is
// why the tables above have no other consumer: everything that needed a
// scancode-to-character mapping had no shared one to call.
//
// They are defined HERE, in the same translation unit as the tables and the
// modifier state, so a caller can never see a keymap that disagrees with the
// one the ISR itself uses. Declared in drivers/keyboard.h; the forward
// declarations of the statics below keep them where they already are.
// ---------------------------------------------------------------------------
static volatile uint8_t shift_pressed;
static volatile uint8_t ctrl_pressed;
static volatile uint8_t alt_pressed;
static volatile uint8_t caps_lock;

char keyboard_scancode_to_char(uint8_t scancode, uint32_t modifiers) {
    if (scancode >= 128) return 0;
    char c = (modifiers & KEY_MOD_SHIFT) ? scancode_to_ascii_shift[scancode]
                                         : scancode_to_ascii[scancode];
    if ((modifiers & KEY_MOD_CAPS) && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    else if ((modifiers & KEY_MOD_CAPS) && (modifiers & KEY_MOD_SHIFT) &&
             c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');
    if (modifiers & KEY_MOD_CTRL) {
        if (c >= 'a' && c <= 'z')      c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
    }
    return c;
}

uint32_t keyboard_get_modifiers(void) {
    uint32_t m = 0;
    if (shift_pressed) m |= KEY_MOD_SHIFT;
    if (ctrl_pressed)  m |= KEY_MOD_CTRL;
    if (alt_pressed)   m |= KEY_MOD_ALT;
    if (caps_lock)     m |= KEY_MOD_CAPS;
    return m;
}

// Modifier key states
static volatile uint8_t shift_pressed = 0;
static volatile uint64_t shift_press_tick = 0;  // tick when shift was last pressed
static volatile uint8_t ctrl_pressed = 0;
static volatile uint8_t alt_pressed = 0;
static volatile uint8_t caps_lock = 0;
static volatile uint8_t extended_scancode = 0;  // For 0xE0 extended keys

// Special key codes (above ASCII range)
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83
#define KEY_F5      0x84
#define KEY_F11     0x85
#define KEY_F12     0x86
#define KEY_F1      0x88
#define KEY_F2      0x89
#define KEY_F3      0x8B
#define KEY_F4      0x8C
#define KEY_F7      0x8D
#define KEY_F8      0x8E
#define KEY_F9      0x8F
#define KEY_F10     0x87
#ifndef KEY_F6
#define KEY_F6      0x8A
#endif
#ifndef KEY_SUPER
#define KEY_SUPER   0x9B
#endif

// #525 DIAGNOSTIC: measure how fast ticks are actually DELIVERED, in real time.
//
// This is the instrument that a heartbeat can never be. `uptime` is ticks/hz -
// circular, derived from the very counter under suspicion - and any sampling
// window longer than a burst averages the burst away, because tick REINJECTION
// CONSERVES the tick count: 1250 ticks crammed into 15ms still average an
// innocent 250Hz over 2 seconds. Only a per-tick, non-tick-clock measurement
// can see it. Cost: one rdtsc and a compare per tick, no polling loop.
//
// A tick arriving <1ms after the previous one (nominal spacing is 4ms at 250Hz)
// did not represent 4ms of elapsed time: it is a REINJECTED tick from a backlog
// KVM banked while the vCPU was starved. tickburst_max_run counts the longest
// consecutive run of such ticks, i.e. exactly how many ticks a `timer_ticks + N`
// deadline can be advanced by without N/hz seconds of wall clock passing.
uint64_t tickburst_max_run = 0;      // longest run of sub-1ms-spaced ticks
uint64_t tickburst_run_us  = 0;      // real microseconds that run occupied
uint64_t tickburst_min_gap = ~0ULL;  // smallest observed inter-tick gap (us)
static uint64_t tb_last_us = 0, tb_run = 0, tb_run_start_us = 0;

static void tickburst_sample(void) {
    if (!mono_ready()) return;
    uint64_t now = mono_us();
    if (tb_last_us == 0) { tb_last_us = now; return; }
    uint64_t gap = now - tb_last_us;
    tb_last_us = now;
    if (gap < tickburst_min_gap) tickburst_min_gap = gap;
    if (gap < 1000) {                 // reinjected: far tighter than 4ms nominal
        if (tb_run == 0) tb_run_start_us = now;
        tb_run++;
        if (tb_run > tickburst_max_run) {
            tickburst_max_run = tb_run;
            tickburst_run_us  = now - tb_run_start_us;
        }
    } else {
        tb_run = 0;
    }
}

// Timer interrupt handler
static void timer_handler(interrupt_frame_t *frame) {
    (void)frame;
    timer_ticks++;
    tickburst_sample();

    // Send EOI first to allow nested interrupts
    pic_send_eoi(0);

    // Call scheduler tick (handles preemption)
    sched_tick();
}

// Keyboard interrupt handler
// #307: scancode processing extracted from the IRQ1 handler so USB HID
// keyboards can push synthetic PS/2 set-1 scancodes through the EXACT same
// translation (shift/ctrl/caps state, key buffer encoding, arrow/F-key codes)
// as the real PS/2 keyboard. This runs in thread or IRQ context and must NOT
// touch the PIC/EOI (the caller owns that).
void keyboard_process_scancode(uint8_t scancode) {
    // Ignore keyboard controller response bytes (ACK, resend, self-test pass, etc.)
    // These arrive during keyboard initialization and are not real keypresses.
    if (scancode == 0xFA || scancode == 0xFE || scancode == 0xFC ||
        scancode == 0x00 || scancode == 0xFF ||
        scancode == 0xEE) {
        return;
    }

    // Raw scancode tap for DOS guests (#202, fixed for non-PS/2 keyboards by
    // #763). Mirror every byte VERBATIM, including 0xE0 extended prefixes and
    // release codes, before the cooked translation below: the DOS layer
    // replays these through the guest's INT 9 and fills its BDA key ring from
    // them. This is a MIRROR with no other consumer, so the guest does not
    // race the desktop for a keystroke.
    dos_scancode_tap(scancode);

    // Handle extended scancode prefix (0xE0)
    if (scancode == 0xE0) {
        extended_scancode = 1;
        return;
    }

    // Check for key release (bit 7 set)
    if (scancode & 0x80) {
        // Key release
        uint8_t key = scancode & 0x7F;
        if (extended_scancode) {
            // Extended key release - send KEY_UP for arrow keys
            extended_scancode = 0;
            char c = 0;
            switch (key) {
                case 0x48: c = KEY_UP + 0x10; break;    // Up release
                case 0x50: c = KEY_DOWN + 0x10; break;  // Down release
                case 0x4B: c = KEY_LEFT + 0x10; break;  // Left release
                case 0x4D: c = KEY_RIGHT + 0x10; break; // Right release
                // (Word6 divergence catalog #2, Alt-menu) Right Alt/AltGr
                // (HID 0xE6) and real PS/2 Right Alt both arrive as extended
                // (0xE0-prefixed) scancode 0x38, same byte as Left Alt's
                // non-extended release below. Route both to the same
                // KEY_ALT_UP so either Alt key drives WM_SYSKEYUP/menu-mnemonic
                // gating in exec/win16api.c.
                case 0x38: c = KEY_ALT_UP; break;       // Right Alt release
            }
            if (c != 0) {
                uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
                if (next_write != kb_read_idx) {
                    keyboard_buffer[kb_write_idx] = c;
                    kb_write_idx = next_write;
                }
            }
        } else if (key == 0x2A || key == 0x36) {
            shift_pressed = 0;  // Left or right shift released
            // Send key release event
            char c = (key == 0x2A) ? KEY_LSHIFT_UP : KEY_RSHIFT_UP;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (key == 0x1D) {
            ctrl_pressed = 0;   // Ctrl released
            // Send key release event
            char c = KEY_LCTRL_UP;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (key == 0x38) {
            alt_pressed = 0;    // Alt released
            // (Word6 divergence catalog #2, Alt-menu, ROOT CAUSE part 1) This
            // used to ONLY clear the local `alt_pressed` flag, which is a
            // file-static variable with ZERO external readers (grepped this
            // file: nothing outside keyboard_process_scancode ever reads it).
            // So Alt release was completely invisible to every consumer,
            // including exec/win16api.c: Alt was never tracked as a modifier
            // at all, and every Alt-chord degraded to a plain character
            // keystroke (Alt+F typed "f" into the document instead of opening
            // the File menu). Push KEY_ALT_UP exactly like Ctrl does above so
            // win16api.c can see real Alt up/down transitions.
            char c = KEY_ALT_UP;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else {
            // Regular key release - convert scancode to ASCII, then add release bit
            if (key < 128) {
                char base_char;
                if (shift_pressed) {
                    base_char = scancode_to_ascii_shift[key];
                } else {
                    base_char = scancode_to_ascii[key];
                }
                // Only emit release events for printable ASCII (0x20-0x7E).
                // Control chars (Backspace 0x08, Tab 0x09, Enter 0x0A, ESC 0x1B)
                // would produce release codes (0x88, 0x89, 0x8A, 0x9B) that
                // collide with the KEY_F* special-key range (0x80-0x8F).
                // In particular, Enter release 0x8A == KEY_F6 was triggering
                // a phantom terminal launch every time the user pressed Enter
                // in the shell.
                if (base_char >= 0x20 && base_char <= 0x7E) {
                    char c = base_char | 0x80;  // ASCII char with release bit
                    uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
                    if (next_write != kb_read_idx) {
                        keyboard_buffer[kb_write_idx] = c;
                        kb_write_idx = next_write;
                    }
                }
            }
        }
    } else {
        // Key press
        if (extended_scancode) {
            // Extended key - arrow keys plus the cursor-editing extended
            // keys (#299), plus the Super/GUI/Windows key (#552). Arrows are
            // remapped to KEY_UP/DOWN/LEFT/RIGHT (0x80-0x83); Home/End/Delete/
            // PgUp/PgDn/Insert are forwarded as their raw PS/2 make codes,
            // which is exactly what the shared libc textfield.h cursor helper
            // expects (Home=0x47, End=0x4F, Delete=0x53, PgUp=0x49, PgDn=0x51,
            // Insert=0x52). These values are all below 0x80 so they do not
            // collide with the KEY_* range. Left GUI (0x5B) and Right GUI
            // (0x5C) both map to KEY_SUPER: real PS/2 hardware sends these
            // set-1 extended make codes directly, and usb_hid.c's HID usage
            // 0xE3/0xE7 -> set-1 0x5B/0x5C translation (emit_set1) feeds the
            // exact same path, so one switch case covers both keyboard types.
            char c = 0;
            switch (scancode) {
                case 0x48: c = KEY_UP; break;
                case 0x50: c = KEY_DOWN; break;
                case 0x4B: c = KEY_LEFT; break;
                case 0x4D: c = KEY_RIGHT; break;
                case 0x47: c = 0x47; break;  // Home
                case 0x4F: c = 0x4F; break;  // End
                case 0x53: c = 0x53; break;  // Delete
                case 0x49: c = 0x49; break;  // Page Up
                case 0x51: c = 0x51; break;  // Page Down
                case 0x52: c = 0x52; break;  // Insert
                case 0x5B: c = KEY_SUPER; break;  // Left GUI/Windows/Command
                case 0x5C: c = KEY_SUPER; break;  // Right GUI/Windows/Command
                // #763: the two keypad keys that are E0-prefixed on real set-1
                // hardware. Neither had a case, so keypad Enter and keypad /
                // produced NOTHING on this desktop, for PS/2 keyboards as much
                // as USB ones. Map them to the same characters as their main
                // block twins, which is what every other OS does.
                case 0x1C: c = '\n'; break;      // Keypad Enter
                case 0x35: c = '/';  break;      // Keypad /
                // (Word6 divergence catalog #2, Alt-menu) Right Alt/AltGr: see
                // the matching comment on the release switch above.
                case 0x38: c = KEY_ALT; break;    // Right Alt press
            }
            extended_scancode = 0;

            if (c != 0) {
                uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
                if (next_write != kb_read_idx) {
                    keyboard_buffer[kb_write_idx] = c;
                    kb_write_idx = next_write;
                }
            }
        } else if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = 1;  // Shift pressed
            shift_press_tick = timer_ticks;
            // Also send as key event for games
            char c = (scancode == 0x2A) ? KEY_LSHIFT : KEY_RSHIFT;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x1D) {
            ctrl_pressed = 1;   // Ctrl pressed
            // Also send as key event for games
            char c = KEY_LCTRL;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x38) {
            alt_pressed = 1;    // Alt pressed
            // (Word6 divergence catalog #2, Alt-menu, ROOT CAUSE part 2) Push
            // KEY_ALT so exec/win16api.c can translate it to real VK_MENU and
            // generate WM_SYSKEYDOWN/WM_SYSCHAR (not WM_KEYDOWN/WM_CHAR) while
            // Alt is held, and drive Alt+mnemonic menu access. See the release
            // branch above for why this was previously a silent no-op.
            char c = KEY_ALT;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x3F) {
            // F5 key pressed
            char c = KEY_F5;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x40) {
            // F6 key pressed - reserved for GUI terminal launch (Phase J2)
            char c = KEY_F6;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x3B) {
            // F1 key pressed
            char c = KEY_F1;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x3C) {
            // F2 key pressed
            char c = KEY_F2;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x3D) {
            // F3 key pressed
            char c = KEY_F3;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x3E) {
            // F4 key pressed
            char c = KEY_F4;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x41) {
            // F7 key pressed
            char c = KEY_F7;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x42) {
            // F8 key pressed
            char c = KEY_F8;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x43) {
            // F9 key pressed
            char c = KEY_F9;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x44) {
            // F10 key pressed
            char c = KEY_F10;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x57) {
            // F11 key pressed - return special key code
            char c = KEY_F11;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x58) {
            // F12 key pressed - return special key code
            char c = KEY_F12;
            uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
            if (next_write != kb_read_idx) {
                keyboard_buffer[kb_write_idx] = c;
                kb_write_idx = next_write;
            }
        } else if (scancode == 0x3A) {
            // Caps Lock toggle
            caps_lock ^= 1;
        } else if (scancode < 128) {
            // Auto-release shift if stuck (e.g. VNC console drops release scancode)
            if (shift_pressed && (timer_ticks - shift_press_tick) > 500) {
                shift_pressed = 0;
            }
            // Convert scancode to ASCII
            char c;
            if (shift_pressed) {
                c = scancode_to_ascii_shift[scancode];
            } else {
                c = scancode_to_ascii[scancode];
            }
            // Apply Caps Lock: inverts case for alphabetic characters only
            if (caps_lock) {
                if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
                else if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
            }

            // Handle Ctrl combinations
            if (ctrl_pressed && c >= 'a' && c <= 'z') {
                c = c - 'a' + 1;  // Convert to control character
                // Ctrl+C (character 3) sets interrupt flag
                if (c == 3) {
                    interrupt_requested = 1;
                }
            }

            // Add to buffer if valid character
            if (c != 0) {
                uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
                if (next_write != kb_read_idx) {  // Buffer not full
                    keyboard_buffer[kb_write_idx] = c;
                    kb_write_idx = next_write;
                }
            }
        }
    }
}

// Real PS/2 keyboard IRQ1 handler: read the byte, run the shared scancode
// processor, then acknowledge the PIC.
//
// #763: this used to mirror the byte into the DOS tap itself, behind its own
// copy of the controller-status-byte filter. Both moved into
// keyboard_process_scancode(), which every keyboard source already calls, so
// a USB or Bluetooth keyboard feeds a DOS guest too. The bytes this path puts
// in the tap, and their order, are unchanged.
static void keyboard_handler(interrupt_frame_t *frame) {
    (void)frame;

    // Read scancode from keyboard controller
    uint8_t scancode = inb(0x60);
    g_kbd_irq_scancodes++;   // #334 delivery instrument

    keyboard_process_scancode(scancode);

    // Send EOI
    pic_send_eoi(1);
}

// Check if keyboard has a character
// #616: POLLED i8042 drain, for contexts that run with interrupts DISABLED.
//
// keyboard_has_char() below reads a ring buffer that ONLY keyboard_handler()
// (the IRQ1 ISR) fills, so it is permanently false anywhere RFLAGS.IF == 0.
// The whole early-boot window is such a context: main() does not sti() until
// long after the filesystem check, so the boot fsck's "press ESC to skip"
// prompt polled a buffer nothing could ever write to.
//
// The failure was doubly silent, and worth understanding before adding any
// other early-boot prompt. The i8042 output buffer holds exactly ONE byte:
// with nothing reading port 0x60, the first keypress latches in OBF and the
// controller then DROPS every subsequent scancode. So the prompt produced no
// effect AND no side effect no matter how many times the key was pressed
// (measured: 40 ESC presses over the check window, zero response), which reads
// exactly like a key-delivery problem and is not one.
//
// This drains pending bytes by hand and feeds them through the SAME scancode
// state machine and the SAME cooked ring buffer the ISR uses, so
// keyboard_has_char()/keyboard_get_char() keep working unchanged and no second,
// divergent keyboard path exists. keyboard_process_scancode() calls no other
// function and touches only this file's statics, so it is safe with interrupts
// in any state; a byte consumed here is simply one the ISR never sees.
//
// BOUNDED (#426): at most KBD_POLL_DRAIN_MAX bytes per call, so a wedged
// controller that permanently asserts OBF degrades to "we read 32 bytes and
// move on", never to a hang.
#define KBD_POLL_DRAIN_MAX 32
void keyboard_poll_i8042(void) {
    for (int i = 0; i < KBD_POLL_DRAIN_MAX; i++) {
        uint8_t st = inb(0x64);
        if (!(st & 0x01)) return;      // OBF clear: nothing pending
        uint8_t data = inb(0x60);
        if (st & 0x20) continue;       // bit 5 = AUX: a mouse byte, not ours
        keyboard_process_scancode(data);
    }
}

int keyboard_has_char(void) {
    return kb_read_idx != kb_write_idx;
}

// Get a character from keyboard buffer
// Returns int to handle special keys like KEY_UP (0x80+)
int keyboard_get_char(void) {
    if (kb_read_idx == kb_write_idx) {
        return 0;  // No character available
    }

    // Return as unsigned to preserve KEY_UP etc values
    int c = (unsigned char)keyboard_buffer[kb_read_idx];
    kb_read_idx = (kb_read_idx + 1) % KEYBOARD_BUFFER_SIZE;
    g_kbd_consumed++;   // #334: an injected or real key was actually CONSUMED
    return c;
}

// #334: current occupancy of the cooked-key ring (queued but not yet drained).
int keyboard_buffer_depth(void) {
    return (int)((kb_write_idx - kb_read_idx + KEYBOARD_BUFFER_SIZE) % KEYBOARD_BUFFER_SIZE);
}

// #334: reverse of the scancode->ASCII tables so the serial test-input channel
// can TYPE a string by synthesizing set-1 scancodes through the identical
// keyboard_process_scancode() path the PS/2 IRQ uses. Reuses the SAME tables as
// the forward path so the two can never drift. Returns the set-1 scancode and
// sets *need_shift, or -1 if the char is not typeable from these tables.
int keyboard_ascii_to_scancode(char c, int *need_shift) {
    for (int sc = 0; sc < 128; sc++) {
        if (scancode_to_ascii[sc] == c) { if (need_shift) *need_shift = 0; return sc; }
    }
    for (int sc = 0; sc < 128; sc++) {
        if (scancode_to_ascii_shift[sc] == c) { if (need_shift) *need_shift = 1; return sc; }
    }
    if (need_shift) *need_shift = 0;
    return -1;
}

// Clear interrupt flag
void clear_interrupt(void) {
    interrupt_requested = 0;
}

// Check if interrupt was requested (and clear it)
int check_interrupt(void) {
    if (interrupt_requested) {
        interrupt_requested = 0;
        return 1;
    }
    return 0;
}

// Initialize ISR handlers
void isr_init(void) {
    kprintf("[ISR] Registering interrupt handlers...\n");

    // Register timer handler (IRQ 0 = INT 32)
    idt_register_handler(32, timer_handler);

    // Register keyboard handler (IRQ 1 = INT 33)
    idt_register_handler(33, keyboard_handler);

    // #429: register the real page-fault (#PF, vector 14) handler. Before this
    // the demand-paging subsystem was dead and every fault was instantly fatal.
    // The handler resolves demand-zero / lazy mmap / COW faults and turns an
    // invalid user access into a per-process SIGSEGV.
    { extern void page_fault_handler(interrupt_frame_t *frame);
      idt_register_handler(14, page_fault_handler); }

    // #429: enable EFER.NXE on the BSP so the demand paths may mark writable
    // data pages no-execute (W^X). APs enable it in smp.c ap_entry().
    { extern void cpu_enable_nx(void); cpu_enable_nx(); }

    // Enable timer and keyboard IRQs
    pic_enable_irq(0);  // Timer
    pic_enable_irq(1);  // Keyboard

    kprintf("[ISR] Timer and keyboard handlers registered\n");
}
