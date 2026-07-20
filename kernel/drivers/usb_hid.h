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
    HID_DEVICE_UNKNOWN
} hid_device_type_t;

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
int usb_hid_attach(xhci_controller_t *xhc, int slot_id, int iface_num,
                   int ep_in, int ep_in_mps, int b_interval, int speed,
                   uint8_t subclass, uint8_t protocol);

// #307: poll every attached HID device once (drain completed interrupt-IN TDs,
// feed the kernel input queue, resubmit). Called from the HID poll worker.
void usb_hid_poll_all(void);

// #307: spawn the kernel worker thread that continuously polls HID devices.
void usb_hid_start_poll_thread(void);
// #703 (#71 iMac): clear the one-shot poll-thread started-guard once, so the
// worker (proc_create()d early, then erased by proc_init()s proc_table memset)
// can be recreated in the correct post-proc_init order. See usb_hid.c / main.c.
void usb_hid_reset_poll_thread_guard(void);

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
