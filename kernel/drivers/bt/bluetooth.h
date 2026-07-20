// bluetooth.h - Bluetooth Stack for MayteraOS
// Implements HCI, L2CAP, SDP, and HID profile over USB transport
#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include "../../types.h"

// ============================================================================
// Bluetooth Address
// ============================================================================

// Bluetooth device address (6 bytes, little-endian)
typedef struct {
    uint8_t addr[6];
} __attribute__((packed)) bt_addr_t;

// Compare two Bluetooth addresses
static inline int bt_addr_cmp(const bt_addr_t *a, const bt_addr_t *b) {
    for (int i = 0; i < 6; i++) {
        if (a->addr[i] != b->addr[i]) {
            return a->addr[i] - b->addr[i];
        }
    }
    return 0;
}

// Copy Bluetooth address
static inline void bt_addr_copy(bt_addr_t *dst, const bt_addr_t *src) {
    for (int i = 0; i < 6; i++) {
        dst->addr[i] = src->addr[i];
    }
}

// Zero address constant
#define BT_ADDR_ANY  ((bt_addr_t){{0, 0, 0, 0, 0, 0}})

// ============================================================================
// Bluetooth Device Classes
// ============================================================================

// USB Bluetooth device identification
#define BT_USB_CLASS        0xE0    // Wireless Controller
#define BT_USB_SUBCLASS     0x01    // RF Controller
#define BT_USB_PROTOCOL     0x01    // Bluetooth Primary Controller

// Bluetooth major device classes
#define BT_CLASS_MISC               0x00
#define BT_CLASS_COMPUTER           0x01
#define BT_CLASS_PHONE              0x02
#define BT_CLASS_LAN                0x03
#define BT_CLASS_AV                 0x04
#define BT_CLASS_PERIPHERAL         0x05
#define BT_CLASS_IMAGING            0x06
#define BT_CLASS_WEARABLE           0x07
#define BT_CLASS_TOY                0x08
#define BT_CLASS_HEALTH             0x09
#define BT_CLASS_UNCATEGORIZED      0x1F

// Peripheral minor device classes
#define BT_PERIPHERAL_KEYBOARD      0x40
#define BT_PERIPHERAL_POINTING      0x80
#define BT_PERIPHERAL_COMBO         0xC0

// ============================================================================
// Bluetooth Profiles
// ============================================================================

typedef enum {
    BT_PROFILE_NONE = 0,
    BT_PROFILE_HID,         // Human Interface Device
    BT_PROFILE_RFCOMM,      // Serial Port Profile
    BT_PROFILE_A2DP,        // Audio (future)
} bt_profile_t;

// ============================================================================
// Bluetooth Device State
// ============================================================================

typedef enum {
    BT_STATE_DISCONNECTED = 0,
    BT_STATE_DISCOVERED,    // Found during scan
    BT_STATE_CONNECTING,    // Connection in progress
    BT_STATE_CONNECTED,     // Connected but not authenticated
    BT_STATE_AUTHENTICATED, // Authenticated (paired)
    BT_STATE_CONFIGURED,    // Ready for use
} bt_state_t;

// ============================================================================
// Bluetooth Device Structure
// ============================================================================

#define BT_MAX_DEVICES      16
#define BT_MAX_NAME_LEN     64

typedef struct {
    bt_addr_t       addr;           // Device address
    char            name[BT_MAX_NAME_LEN]; // Device name (from inquiry)
    uint32_t        class_of_device;// Class of Device (CoD)
    bt_state_t      state;
    bt_profile_t    profile;        // Active profile

    // Connection handle (from HCI)
    uint16_t        connection_handle;

    // L2CAP channel IDs
    uint16_t        l2cap_cid;      // Local CID
    uint16_t        l2cap_remote_cid; // Remote CID

    // Driver-specific data
    void            *driver_data;
} bt_device_t;

// ============================================================================
// HID Callback Types
// ============================================================================

// HID keyboard event
typedef struct {
    uint8_t modifiers;      // Modifier keys (Ctrl, Shift, Alt, etc.)
    uint8_t reserved;
    uint8_t keys[6];        // Up to 6 simultaneous key codes
} bt_hid_keyboard_report_t;

// HID mouse event
typedef struct {
    uint8_t buttons;        // Button state
    int8_t  x;              // X movement
    int8_t  y;              // Y movement
    int8_t  wheel;          // Scroll wheel
} bt_hid_mouse_report_t;

// HID callback function type
typedef void (*bt_hid_keyboard_callback_t)(const bt_hid_keyboard_report_t *report);
typedef void (*bt_hid_mouse_callback_t)(const bt_hid_mouse_report_t *report);

// ============================================================================
// Bluetooth Stack API
// ============================================================================

// Initialize the Bluetooth stack
// Returns 0 on success, negative on error
int bt_init(void);

// Shutdown the Bluetooth stack
void bt_shutdown(void);

// Check if Bluetooth is available
int bt_is_available(void);

// Get local Bluetooth address
int bt_get_local_addr(bt_addr_t *addr);

// ============================================================================
// Device Discovery
// ============================================================================

// Start scanning for devices
// duration: scan time in seconds (1-30)
// Returns 0 on success
int bt_scan_start(int duration);

// Stop scanning
void bt_scan_stop(void);

// Check if scan is in progress
int bt_scan_in_progress(void);

// Get discovered devices
// devices: array to fill with discovered devices
// max_devices: maximum number of devices to return
// Returns number of devices found
int bt_get_discovered_devices(bt_device_t *devices, int max_devices);

// ============================================================================
// Connection Management
// ============================================================================

// Connect to a device
// Returns 0 on success
int bt_connect(bt_addr_t *addr);

// Disconnect from a device
int bt_disconnect(bt_addr_t *addr);

// Pair with a device (may require PIN entry)
int bt_pair(bt_addr_t *addr);

// ============================================================================
// HID Profile
// ============================================================================

// Register keyboard callback
void bt_hid_register_keyboard_callback(bt_hid_keyboard_callback_t callback);

// Register mouse callback
void bt_hid_register_mouse_callback(bt_hid_mouse_callback_t callback);

// Get HID device state
int bt_hid_get_state(bt_addr_t *addr);

// ============================================================================
// Debug/Info
// ============================================================================

// Print Bluetooth stack status
void bt_print_status(void);

// Print discovered devices
void bt_print_devices(void);

// ============================================================================
// Internal Types (used by sub-layers)
// ============================================================================

// USB Bluetooth adapter state
typedef struct bt_usb_adapter {
    int                 active;
    int                 usb_controller_index;
    int                 usb_device_slot;

    // USB endpoints
    uint8_t             ep_control;     // Control endpoint (EP0)
    uint8_t             ep_bulk_in;     // ACL data IN
    uint8_t             ep_bulk_out;    // ACL data OUT
    uint8_t             ep_interrupt;   // HCI events

    // Local Bluetooth address
    bt_addr_t           local_addr;

    // HCI state
    int                 hci_initialized;
    uint16_t            hci_handle_counter;

    // Discovered/connected devices
    bt_device_t         devices[BT_MAX_DEVICES];
    int                 device_count;
} bt_usb_adapter_t;

// Global adapter (single adapter support for now)
extern bt_usb_adapter_t bt_adapter;

#endif // BLUETOOTH_H
