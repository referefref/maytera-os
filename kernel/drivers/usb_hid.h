// usb_hid.h - USB Human Interface Device (HID) Class Driver
// Supports keyboards, mice, and other input devices
#ifndef USB_HID_H
#define USB_HID_H

#include "../types.h"
#include "xhci.h"

// =============================================================================
// HID Class Constants
// =============================================================================

// USB HID Class Request Types
#define HID_REQ_GET_REPORT      0x01
#define HID_REQ_GET_IDLE        0x02
#define HID_REQ_GET_PROTOCOL    0x03
#define HID_REQ_SET_REPORT      0x09
#define HID_REQ_SET_IDLE        0x0A
#define HID_REQ_SET_PROTOCOL    0x0B

// HID Report Types
#define HID_REPORT_INPUT        0x01
#define HID_REPORT_OUTPUT       0x02
#define HID_REPORT_FEATURE      0x03

// HID Protocol Types
#define HID_PROTOCOL_BOOT       0
#define HID_PROTOCOL_REPORT     1

// HID Subclass
#define HID_SUBCLASS_NONE       0x00
#define HID_SUBCLASS_BOOT       0x01

// HID Boot Interface Protocol
#define HID_BOOT_PROTOCOL_KEYBOARD  0x01
#define HID_BOOT_PROTOCOL_MOUSE     0x02

// =============================================================================
// HID Keyboard
// =============================================================================

// Boot protocol keyboard report (8 bytes)
typedef struct {
    uint8_t modifiers;      // Modifier keys
    uint8_t reserved;       // Reserved byte
    uint8_t keycodes[6];    // Up to 6 simultaneous keypresses
} __attribute__((packed)) hid_keyboard_report_t;

// Keyboard modifier bits
#define HID_MOD_LEFT_CTRL   (1 << 0)
#define HID_MOD_LEFT_SHIFT  (1 << 1)
#define HID_MOD_LEFT_ALT    (1 << 2)
#define HID_MOD_LEFT_GUI    (1 << 3)
#define HID_MOD_RIGHT_CTRL  (1 << 4)
#define HID_MOD_RIGHT_SHIFT (1 << 5)
#define HID_MOD_RIGHT_ALT   (1 << 6)
#define HID_MOD_RIGHT_GUI   (1 << 7)

// HID keyboard usage codes (common keys)
#define HID_KEY_NONE        0x00
#define HID_KEY_A           0x04
#define HID_KEY_B           0x05
#define HID_KEY_C           0x06
#define HID_KEY_D           0x07
#define HID_KEY_E           0x08
#define HID_KEY_F           0x09
#define HID_KEY_G           0x0A
#define HID_KEY_H           0x0B
#define HID_KEY_I           0x0C
#define HID_KEY_J           0x0D
#define HID_KEY_K           0x0E
#define HID_KEY_L           0x0F
#define HID_KEY_M           0x10
#define HID_KEY_N           0x11
#define HID_KEY_O           0x12
#define HID_KEY_P           0x13
#define HID_KEY_Q           0x14
#define HID_KEY_R           0x15
#define HID_KEY_S           0x16
#define HID_KEY_T           0x17
#define HID_KEY_U           0x18
#define HID_KEY_V           0x19
#define HID_KEY_W           0x1A
#define HID_KEY_X           0x1B
#define HID_KEY_Y           0x1C
#define HID_KEY_Z           0x1D
#define HID_KEY_1           0x1E
#define HID_KEY_2           0x1F
#define HID_KEY_3           0x20
#define HID_KEY_4           0x21
#define HID_KEY_5           0x22
#define HID_KEY_6           0x23
#define HID_KEY_7           0x24
#define HID_KEY_8           0x25
#define HID_KEY_9           0x26
#define HID_KEY_0           0x27
#define HID_KEY_ENTER       0x28
#define HID_KEY_ESC         0x29
#define HID_KEY_BACKSPACE   0x2A
#define HID_KEY_TAB         0x2B
#define HID_KEY_SPACE       0x2C
#define HID_KEY_MINUS       0x2D
#define HID_KEY_EQUAL       0x2E
#define HID_KEY_LBRACKET    0x2F
#define HID_KEY_RBRACKET    0x30
#define HID_KEY_BACKSLASH   0x31
#define HID_KEY_SEMICOLON   0x33
#define HID_KEY_QUOTE       0x34
#define HID_KEY_GRAVE       0x35
#define HID_KEY_COMMA       0x36
#define HID_KEY_DOT         0x37
#define HID_KEY_SLASH       0x38
#define HID_KEY_CAPSLOCK    0x39
#define HID_KEY_F1          0x3A
#define HID_KEY_F2          0x3B
#define HID_KEY_F3          0x3C
#define HID_KEY_F4          0x3D
#define HID_KEY_F5          0x3E
#define HID_KEY_F6          0x3F
#define HID_KEY_F7          0x40
#define HID_KEY_F8          0x41
#define HID_KEY_F9          0x42
#define HID_KEY_F10         0x43
#define HID_KEY_F11         0x44
#define HID_KEY_F12         0x45
#define HID_KEY_PRINTSCREEN 0x46
#define HID_KEY_SCROLLLOCK  0x47
#define HID_KEY_PAUSE       0x48
#define HID_KEY_INSERT      0x49
#define HID_KEY_HOME        0x4A
#define HID_KEY_PAGEUP      0x4B
#define HID_KEY_DELETE      0x4C
#define HID_KEY_END         0x4D
#define HID_KEY_PAGEDOWN    0x4E
#define HID_KEY_RIGHT       0x4F
#define HID_KEY_LEFT        0x50
#define HID_KEY_DOWN        0x51
#define HID_KEY_UP          0x52

// =============================================================================
// HID Mouse
// =============================================================================

// Boot protocol mouse report (3+ bytes)
typedef struct {
    uint8_t buttons;        // Button states
    int8_t  x;              // X movement (-127 to 127)
    int8_t  y;              // Y movement (-127 to 127)
    int8_t  wheel;          // Wheel movement (optional, 4-byte report)
} __attribute__((packed)) hid_mouse_report_t;

// Mouse button bits
#define HID_MOUSE_LEFT      (1 << 0)
#define HID_MOUSE_RIGHT     (1 << 1)
#define HID_MOUSE_MIDDLE    (1 << 2)
#define HID_MOUSE_BUTTON4   (1 << 3)
#define HID_MOUSE_BUTTON5   (1 << 4)

// =============================================================================
// HID Device Structure
// =============================================================================

typedef enum {
    HID_DEVICE_KEYBOARD,
    HID_DEVICE_MOUSE,
    HID_DEVICE_GAMEPAD,
    // #162: an interface whose REPORT DESCRIPTOR says it carries Consumer-page
    // (0x0C) controls and no Mouse or Keyboard collection. Appended at the END
    // so every existing enumerator keeps its value; hid_device_type_t is
    // compared, logged and switched on in several files.
    HID_DEVICE_CONSUMER,
    HID_DEVICE_UNKNOWN
} hid_device_type_t;

// =============================================================================
// #162: Consumer-page (media key) map, produced by rustkern/hidrepd.rs
// =============================================================================
// Layout-locked against HidConsMap in rustkern/hidrepd.rs. This struct is
// OPAQUE to the C here in the sense that C never computes from its fields; it
// only stores one per device and hands the address back to the Rust decode.
// The lock exists anyway because it is embedded in hid_device_t, so a silent
// size change would shift every field after it.
typedef struct {
    uint8_t  have;          // HIDCONS_* bitmask; 0 = no volume controls here
    uint8_t  report_id;     // 0 = descriptor declared no Report ID at all
    uint8_t  kind;          // HIDCONS_KIND_*
    uint8_t  top_consumer;  // saw a top-level Consumer Control collection
    uint8_t  top_mouse;     // saw a top-level Generic Desktop Mouse collection
    uint8_t  top_keyboard;  // saw a top-level Generic Desktop Keyboard collection
    uint8_t  report_bytes;  // input report payload size, diagnostic only
    uint8_t  _pad;
    uint16_t bit_vol_up;    // VARIABLE kind: bit offsets within the payload
    uint16_t bit_vol_down;
    uint16_t bit_mute;
    uint16_t arr_bit_off;   // ARRAY kind: the usage-value field
    uint16_t arr_bit_size;
    uint16_t arr_count;
} hid_cons_map_t;

_Static_assert(sizeof(hid_cons_map_t) == 20,
               "hid_cons_map_t must stay layout-identical to HidConsMap in "
               "rustkern/hidrepd.rs (which asserts the same 20 bytes)");

// Bits in hid_cons_map_t::have and in hidrepd_decode_rs()'s return value.
#define HIDCONS_VOL_UP        (1 << 0)
#define HIDCONS_VOL_DOWN      (1 << 1)
#define HIDCONS_MUTE          (1 << 2)

#define HIDCONS_KIND_NONE     0
#define HIDCONS_KIND_VARIABLE 1
#define HIDCONS_KIND_ARRAY    2

// rustkern/hidrepd.rs
int hidrepd_parse_rs(const uint8_t *desc, uint32_t len, hid_cons_map_t *out);
uint8_t hidrepd_decode_rs(const hid_cons_map_t *map, const uint8_t *buf, uint32_t len);
int hidrepd_selftest_rs(uint32_t *out_checks, uint32_t *out_first_fail);

typedef struct {
    // USB device info
    xhci_controller_t *controller;
    int slot_id;
    int interface_num;
    int endpoint_in;        // Interrupt IN endpoint address (0x81 etc.)
    int endpoint_out;       // Interrupt OUT endpoint (optional)
    int max_packet_size;
    int poll_interval;      // Polling interval in ms
    // #307: xHCI interrupt-IN transfer state
    int dci;                // Device Context Index of the interrupt IN endpoint
    int speed;              // XHCI_SPEED_*
    uint8_t *report_buf;    // DMA report buffer (identity-mapped)
    uint64_t report_buf_phys;
    int report_len;         // bytes requested per interrupt IN TD
    int outstanding;        // an interrupt-IN TD is queued and pending
    uint32_t reports_seen;  // #307 bootlog: running count of reports actually received
    // #433 (re-scoped): interrupt-IN COMPLETION logging to /USBLOG.TXT. Bounded so
    // it proves the transfer path (does pressing a key produce a report on slot 3
    // DCI 3?) without flooding: log the first few completions unconditionally, then
    // only when the report bytes CHANGE, capped at usblog_cap total lines.
    uint32_t usblog_completions; // count of completion lines emitted for this device
    uint8_t  usblog_last[8];     // last logged report bytes (change detection)

    // HID device info
    hid_device_type_t type;
    uint8_t protocol;       // Boot or Report protocol

    // #162: Consumer-page media keys. `cons.have` is 0 on every device that
    // does not have them, which is the overwhelmingly common case, and every
    // consumer code path early-outs on it - so a machine with no media keys
    // pays one predictable branch per report and nothing else.
    hid_cons_map_t cons;
    uint8_t  cons_prev;       // last decoded mask, for press-edge detection
    uint16_t rd_len;          // wDescriptorLength from the HID class descriptor
    uint32_t cons_hold_us;    // mono_us when the current mask was first seen
    uint32_t cons_next_rep_us;// mono_us of the next auto-repeat, 0 = not armed

    // Keyboard state
    hid_keyboard_report_t keyboard_report;
    hid_keyboard_report_t prev_keyboard_report;

    // Mouse state
    hid_mouse_report_t mouse_report;
    int32_t mouse_x;        // Absolute position (accumulated)
    int32_t mouse_y;

    // Callbacks
    void (*key_callback)(uint8_t keycode, int pressed, uint8_t modifiers);
    void (*mouse_callback)(int32_t x, int32_t y, uint8_t buttons);

    // #134: entry lifecycle. hid_devices[] used to be APPEND-ONLY with no
    // detach path at all, so every replug consumed one of only 8 entries and
    // freed nothing; the real iMac hit the cap after two replug cycles of a
    // 2-interface keyboard and silently refused all further HID (total input
    // lockout, no serial, no sshd). Entries are now reusable: in_use == 0 means
    // the entry is FREE and may be re-bound by usb_hid_attach(). The DMA report
    // page is deliberately NOT freed on detach - it stays owned by the entry and
    // is reused, which bounds total report-page use at MAX_HID_DEVICES pages for
    // the life of the boot AND removes any use-after-free window against the
    // concurrently running poll worker (which never takes a lock).
    int in_use;             // 0 = free entry, 1 = bound to a live device
    int root_port;          // 0-based root port this device hangs off (-1 unknown)

    // #133: bounded endpoint-halt recovery. An interrupt-IN endpoint that
    // completes with Babble/Stall/Transaction-Error is left HALTED by the
    // controller and ignores every subsequent doorbell, so the device is dead
    // until reboot. usb_hid_poll() now runs the spec recovery (Reset Endpoint +
    // Set TR Dequeue Pointer). consecutive_halts caps how many times we do that
    // for one device before giving up, so a physically removed device cannot
    // make the SHARED poll worker issue command-ring ops forever and starve the
    // other HID devices. Any successful report resets it to 0.
    uint32_t consecutive_halts;
    int failed;             // recovery exhausted; parked (see revivals)
    // #133: how many times this entry has been UN-PARKED by the re-scan
    // worker. Before this existed, `failed` was terminal: nothing in the
    // kernel ever cleared it except a fresh usb_hid_attach(), which requires
    // the port to DISCONNECT. A wireless receiver never disconnects - it stays
    // plugged in while its radio link comes and goes - so eight consecutive
    // transaction errors killed the mouse until the next reboot while the
    // wired keyboard on the same controller carried on perfectly. That is the
    // reported "#133: the G7 degrades to unresponsive, the keyboard is fine".
    // Bounded so a device that is genuinely gone cannot make the re-scan
    // worker issue commands for ever.
    uint32_t revivals;
    uint32_t revive_skip;   // scans to wait before the next attempt (backoff)

    // #139 MEASUREMENT. Mouse lag has been reported across many builds and had
    // never been MEASURED: the poll rate, the controller's own endpoint
    // schedule and the compositor were all plausible and nobody knew which
    // dominated. These counters decompose the delay per device, in
    // microseconds off the shared monotonic clock (cpu/mono.h - never
    // timer_ticks, which a KVM tick burst can collapse):
    //   submit_us      - when we armed the interrupt-IN TD;
    //   observe (from xhci_xfer_done_us) - when a drainer saw its completion;
    //   inject         - when we fed the report to mouse_inject_hid().
    // submit->observe is the CONTROLLER's periodic schedule; observe->inject is
    // OUR poll cadence. Kept permanently (a few adds per report) because the
    // one machine that shows the fault has no serial console, so the only way
    // to know its real numbers is a summary line in /BOOTLOG.TXT.
    uint32_t submit_us;      // mono_us of the last interrupt-IN submit
    uint32_t last_inject_us; // mono_us of the previous report injection
    uint32_t st_n;           // samples accumulated in the current window
    uint32_t st_e2e_sum, st_e2e_max;   // submit  -> inject (whole pipeline)
    uint32_t st_lat_sum, st_lat_max;   // observe -> inject (our poll cadence)
    uint32_t st_gap_sum, st_gap_max;   // inject  -> inject (delivered rate)
    uint32_t st_windows;     // summaries emitted for this device
    uint16_t st_gap_hist[8]; // gap buckets: <2 <4 <8 <12 <16 <24 <40 >=40 ms
} hid_device_t;

// =============================================================================
// Function Prototypes
// =============================================================================

// Initialize HID subsystem
void usb_hid_init(void);

// Probe and attach HID device
int usb_hid_probe(xhci_controller_t *xhc, int slot_id,
                  uint8_t interface_class, uint8_t interface_subclass,
                  uint8_t interface_protocol);

// #307: full attach with the interrupt-IN endpoint already configured on the
// controller. Called from xHCI enumeration. Returns the device index or -1.
// #162: `rd_len` is wDescriptorLength from the interface's HID class
// descriptor (bDescriptorType 0x21), or 0 if the configuration descriptor did
// not carry one. Non-zero makes the driver fetch and parse the REPORT
// descriptor, which is the only thing that can tell a media-key interface from
// a mouse - bInterfaceProtocol demonstrably cannot (the owner's iMac keyboard
// declares its media interface as a boot MOUSE). rd_len == 0, or any failure
// along the way, leaves behaviour byte-identical to before #162.
int usb_hid_attach(xhci_controller_t *xhc, int slot_id, int iface_num,
                   int ep_in, int ep_in_mps, int b_interval, int speed,
                   uint8_t subclass, uint8_t protocol, int rd_len);

// #307: poll every attached HID device once (drain completed interrupt-IN TDs,
// feed the kernel input queue, resubmit). Called from the HID poll worker.
void usb_hid_poll_all(void);

// #307: spawn the kernel worker thread that continuously polls HID devices.
void usb_hid_start_poll_thread(void);
// #703 (#71 iMac): clear the one-shot poll-thread started-guard once, so the
// worker (proc_create()d early, then erased by proc_init()s proc_table memset)
// can be recreated in the correct post-proc_init order. See usb_hid.c / main.c.
void usb_hid_reset_poll_thread_guard(void);

// #134: release every hid_devices[] entry bound to (xhc, slot_id) because the
// device behind that slot has physically disconnected. Marks the entries FREE
// for reuse (see hid_device_t::in_use); does not free the DMA report page, by
// design. Returns the number of entries released. Called from the xHCI port
// re-scan worker when it observes a root port go from connected to empty.
int usb_hid_detach_slot(xhci_controller_t *xhc, int slot_id);

// #134: does this (controller, slot) currently have any HID interface bound?
// The re-scan teardown uses it to restrict slot teardown to HID-only slots, so
// a disconnected MSC / network slot keeps exactly its previous behaviour and
// the USB-boot path cannot regress.
int usb_hid_slot_has_device(xhci_controller_t *xhc, int slot_id);

// #133: un-park HID devices whose halt recovery was exhausted. Called from the
// xHCI re-scan worker (a thread that may block), NOT from the HID poll worker.
// Returns the number of devices revived on this pass.
int usb_hid_retry_failed(void);

// Set HID protocol (boot or report)
int usb_hid_set_protocol(hid_device_t *dev, uint8_t protocol);

// Set idle rate (for keyboards)
int usb_hid_set_idle(hid_device_t *dev, uint8_t report_id, uint8_t duration);

// Get HID report
int usb_hid_get_report(hid_device_t *dev, uint8_t type, uint8_t id, 
                       void *buf, uint16_t len);

// Poll HID device (reads interrupt endpoint)
int usb_hid_poll(hid_device_t *dev);

// Process keyboard report
void usb_hid_process_keyboard(hid_device_t *dev);

// Process mouse report
void usb_hid_process_mouse(hid_device_t *dev);

// Set keyboard LED state
int usb_hid_set_keyboard_leds(hid_device_t *dev, uint8_t leds);

// Convert HID keycode to ASCII
char hid_keycode_to_ascii(uint8_t keycode, uint8_t modifiers);

// Get HID device by type
hid_device_t *usb_hid_get_keyboard(void);
hid_device_t *usb_hid_get_mouse(void);

// Register callbacks
void usb_hid_set_key_callback(void (*callback)(uint8_t keycode, int pressed, uint8_t modifiers));
void usb_hid_set_mouse_callback(void (*callback)(int32_t x, int32_t y, uint8_t buttons));

// Debug
void usb_hid_print_devices(void);

// #447 Cybersecurity: keystroke-injection monitor stats (counts NEW keydowns,
// smallest inter-key gap in timer ticks, peak superhuman run, injection flag).
void usb_hid_get_kbd_security(uint32_t *total, uint32_t *min_gap_ticks,
                              uint32_t *peak_burst, int *injection);

#endif // USB_HID_H
