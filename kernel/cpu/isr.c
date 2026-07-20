// isr.c - Interrupt Service Routines implementation
#include "isr.h"
#include "mono.h"
#include "pic.h"
#include "../serial.h"
#include "../proc/process.h"

// Timer tick count
volatile uint64_t timer_ticks = 0;

// Interrupt flag (Ctrl+C)
volatile int interrupt_requested = 0;

// Keyboard buffer
#define KEYBOARD_BUFFER_SIZE 256
static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint16_t kb_read_idx = 0;
static volatile uint16_t kb_write_idx = 0;

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
static inline void dos_sc_push(uint8_t b) {
    uint16_t nx = (uint16_t)((dos_sc_wr + 1) % DOS_SC_RING);
    if (nx != dos_sc_rd) { dos_sc_ring[dos_sc_wr] = b; dos_sc_wr = nx; }
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
            // keys (#299). Arrows are remapped to KEY_UP/DOWN/LEFT/RIGHT
            // (0x80-0x83); Home/End/Delete/PgUp/PgDn/Insert are forwarded as
            // their raw PS/2 make codes, which is exactly what the shared libc
            // textfield.h cursor helper expects (Home=0x47, End=0x4F,
            // Delete=0x53, PgUp=0x49, PgDn=0x51, Insert=0x52). These values are
            // all below 0x80 so they do not collide with the KEY_* range.
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

// Real PS/2 keyboard IRQ1 handler: read the byte, mirror it to the DOS raw tap,
// run the shared scancode processor, then acknowledge the PIC.
static void keyboard_handler(interrupt_frame_t *frame) {
    (void)frame;

    // Read scancode from keyboard controller
    uint8_t scancode = inb(0x60);

    // Raw scancode tap for DOS games (#202). Mirror every byte verbatim,
    // including 0xE0 extended prefixes and release codes, before the normal
    // ASCII translation below. The DOS layer replays these via the guest INT 9.
    if (g_dos_scancode_tap) {
        if (scancode != 0xFA && scancode != 0xFE && scancode != 0xFC &&
            scancode != 0x00 && scancode != 0xFF && scancode != 0xEE)
            dos_sc_push(scancode);
    }

    keyboard_process_scancode(scancode);

    // Send EOI
    pic_send_eoi(1);
}

// Check if keyboard has a character
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
    return c;
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
