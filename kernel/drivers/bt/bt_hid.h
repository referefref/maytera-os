// bt_hid.h - Bluetooth Human Interface Device (HID) Profile
#ifndef BT_HID_H
#define BT_HID_H

#include "bluetooth.h"

// ============================================================================
// HID Protocol Constants
// ============================================================================

// HID Transaction Types (upper nibble)
#define BT_HID_HANDSHAKE            0x00
#define BT_HID_HID_CONTROL          0x10
#define BT_HID_GET_REPORT           0x40
#define BT_HID_SET_REPORT           0x50
#define BT_HID_GET_PROTOCOL         0x60
#define BT_HID_SET_PROTOCOL         0x70
#define BT_HID_GET_IDLE             0x80
#define BT_HID_SET_IDLE             0x90
#define BT_HID_DATA                 0xA0

// HID Handshake Results
#define BT_HID_HANDSHAKE_SUCCESS        0x00
#define BT_HID_HANDSHAKE_NOT_READY      0x01
#define BT_HID_HANDSHAKE_ERR_INVALID_ID 0x02
#define BT_HID_HANDSHAKE_ERR_UNSUPP_REQ 0x03
#define BT_HID_HANDSHAKE_ERR_INVALID_PARAM 0x04
#define BT_HID_HANDSHAKE_ERR_UNKNOWN    0x0E
#define BT_HID_HANDSHAKE_ERR_FATAL      0x0F

// HID Control Operations
#define BT_HID_CTRL_NOP             0x00
#define BT_HID_CTRL_HARD_RESET      0x01
#define BT_HID_CTRL_SOFT_RESET      0x02
#define BT_HID_CTRL_SUSPEND         0x03
#define BT_HID_CTRL_EXIT_SUSPEND    0x04
#define BT_HID_CTRL_VC_UNPLUG       0x05

// Report Types (lower nibble of header)
#define BT_HID_REPORT_OTHER         0x00
#define BT_HID_REPORT_INPUT         0x01
#define BT_HID_REPORT_OUTPUT        0x02
#define BT_HID_REPORT_FEATURE       0x03

// Protocol Modes
#define BT_HID_PROTOCOL_BOOT        0x00
#define BT_HID_PROTOCOL_REPORT      0x01

// ============================================================================
// HID Report Descriptors (Boot Protocol)
// ============================================================================

// Boot Protocol Keyboard Report (8 bytes)
// Byte 0: Modifier keys
// Byte 1: Reserved
// Bytes 2-7: Up to 6 key codes

// Modifier key bits
#define BT_HID_MOD_LEFT_CTRL    0x01
#define BT_HID_MOD_LEFT_SHIFT   0x02
#define BT_HID_MOD_LEFT_ALT     0x04
#define BT_HID_MOD_LEFT_GUI     0x08
#define BT_HID_MOD_RIGHT_CTRL   0x10
#define BT_HID_MOD_RIGHT_SHIFT  0x20
#define BT_HID_MOD_RIGHT_ALT    0x40
#define BT_HID_MOD_RIGHT_GUI    0x80

// Boot Protocol Mouse Report (3 bytes minimum)
// Byte 0: Buttons
// Byte 1: X movement (signed)
// Byte 2: Y movement (signed)
// Byte 3: Wheel (optional)

// Mouse button bits
#define BT_HID_BTN_LEFT         0x01
#define BT_HID_BTN_RIGHT        0x02
#define BT_HID_BTN_MIDDLE       0x04

// ============================================================================
// HID Device State
// ============================================================================

typedef enum {
    BT_HID_STATE_DISCONNECTED = 0,
    BT_HID_STATE_CONNECTING,
    BT_HID_STATE_CONNECTED,
    BT_HID_STATE_READY,
} bt_hid_state_t;

typedef enum {
    BT_HID_TYPE_UNKNOWN = 0,
    BT_HID_TYPE_KEYBOARD,
    BT_HID_TYPE_MOUSE,
    BT_HID_TYPE_COMBO,
} bt_hid_device_type_t;

// ============================================================================
// HID Device Structure
// ============================================================================

#define BT_HID_MAX_DEVICES  4

typedef struct {
    int                 active;
    bt_hid_state_t      state;
    bt_hid_device_type_t type;

    bt_addr_t           addr;
    uint16_t            hci_handle;

    // L2CAP channels
    uint16_t            ctrl_cid;       // Control channel
    uint16_t            intr_cid;       // Interrupt channel

    // Protocol mode
    uint8_t             protocol;       // Boot or Report protocol

    // Last reports
    bt_hid_keyboard_report_t last_keyboard;
    bt_hid_mouse_report_t    last_mouse;
} bt_hid_device_t;

// ============================================================================
// HID API
// ============================================================================

// Initialize HID profile
int bt_hid_init(void);

// Shutdown HID profile
void bt_hid_shutdown(void);

// Connect to HID device
// Returns device index on success, negative on error
int bt_hid_connect(bt_addr_t *addr, uint16_t hci_handle);

// Disconnect HID device
int bt_hid_disconnect(int device_index);

// Get HID device by address
bt_hid_device_t *bt_hid_find_device(bt_addr_t *addr);

// Get HID device by index
bt_hid_device_t *bt_hid_get_device(int index);

// Get number of connected HID devices
int bt_hid_get_device_count(void);

// Set protocol mode (Boot or Report)
int bt_hid_set_protocol(int device_index, uint8_t protocol);

// Send Output report (e.g., keyboard LEDs)
int bt_hid_send_report(int device_index, uint8_t report_type,
                       const void *data, uint16_t len);

// Register callbacks
void bt_hid_register_keyboard_callback(bt_hid_keyboard_callback_t callback);
void bt_hid_register_mouse_callback(bt_hid_mouse_callback_t callback);

// L2CAP data handler (called by L2CAP layer)
void bt_hid_l2cap_recv(uint16_t cid, const void *data, uint16_t len);

// Print HID status
void bt_hid_print_status(void);

// ============================================================================
// Keyboard Scancode to ASCII Conversion
// ============================================================================

// Convert HID keyboard scancode to ASCII
// Returns 0 if no printable character
char bt_hid_scancode_to_ascii(uint8_t scancode, uint8_t modifiers);

// Get key name for scancode
const char *bt_hid_scancode_name(uint8_t scancode);

#endif // BT_HID_H
