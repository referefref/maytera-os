// bt_hid.c - Bluetooth Human Interface Device (HID) Profile
#include "bt_hid.h"
#include "bt_l2cap.h"
#include "bt_hci.h"
#include "bluetooth.h"
#include "../../serial.h"
#include "../../string.h"
#include "../../mm/heap.h"

// ============================================================================
// HID State
// ============================================================================

static struct {
    int initialized;

    // Connected HID devices
    bt_hid_device_t devices[BT_HID_MAX_DEVICES];
    int device_count;

    // Callbacks
    bt_hid_keyboard_callback_t keyboard_callback;
    bt_hid_mouse_callback_t mouse_callback;
} hid_state;

// ============================================================================
// Scancode to ASCII Conversion
// ============================================================================

// USB HID Keyboard/Keypad Usage Page (0x07) scancode to ASCII
// Index = scancode, value = ASCII (lowercase)
static const char scancode_to_ascii_lower[128] = {
    0, 0, 0, 0,                     // 0x00-0x03: Reserved
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',  // 0x04-0x10
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',  // 0x11-0x1D
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',  // 0x1E-0x27
    '\n', 0x1B, '\b', '\t',         // 0x28-0x2B: Enter, Escape, Backspace, Tab
    ' ', '-', '=', '[', ']', '\\',  // 0x2C-0x31: Space, Minus, Equal, LBracket, RBracket, Backslash
    '#', ';', '\'', '`',            // 0x32-0x35: Non-US Hash, Semicolon, Quote, Grave
    ',', '.', '/',                  // 0x36-0x38: Comma, Period, Slash
    0,                              // 0x39: Caps Lock
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // 0x3A-0x43: F1-F10
    0, 0,                           // 0x44-0x45: F11, F12
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // 0x46-0x4F: PrintScr, ScrollLock, Pause, Insert, Home, PgUp, Del, End, PgDn, Right
    0, 0, 0, 0,                     // 0x50-0x53: Left, Down, Up, NumLock
    '/', '*', '-', '+', '\n',       // 0x54-0x58: Keypad /,*,-,+,Enter
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', // 0x59-0x62: Keypad 1-0
    '.', '\\',                      // 0x63-0x64: Keypad ., Non-US Backslash
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // 0x65-0x6E: Application, Power, Keypad =, F13-F18
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // 0x6F-0x78: F19-F24, Execute, Help, Menu, Select, Stop
    0, 0, 0, 0, 0, 0, 0             // 0x79-0x7F: Undo, Cut, Copy, Paste, Mute, Vol+, Vol-
};

// Shifted versions
static const char scancode_to_ascii_upper[128] = {
    0, 0, 0, 0,
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 0x1B, '\b', '\t',
    ' ', '_', '+', '{', '}', '|',
    '~', ':', '"', '~',
    '<', '>', '?',
    0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0,
    '/', '*', '-', '+', '\n',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '.', '|',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0
};

// Scancode names for debugging
static const char *scancode_names[128] = {
    "None", "ErrorRollOver", "POSTFail", "ErrorUndefined",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
    "Enter", "Escape", "Backspace", "Tab",
    "Space", "Minus", "Equal", "LeftBracket", "RightBracket", "Backslash",
    "NonUSHash", "Semicolon", "Quote", "Grave",
    "Comma", "Period", "Slash",
    "CapsLock",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10",
    "F11", "F12",
    "PrintScreen", "ScrollLock", "Pause", "Insert", "Home", "PageUp",
    "Delete", "End", "PageDown", "Right", "Left", "Down", "Up",
    "NumLock",
    "KP/", "KP*", "KP-", "KP+", "KPEnter",
    "KP1", "KP2", "KP3", "KP4", "KP5", "KP6", "KP7", "KP8", "KP9", "KP0",
    "KP.", "NonUSBackslash", "Application", "Power", "KP=",
    "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20",
    "F21", "F22", "F23", "F24",
    "Execute", "Help", "Menu", "Select", "Stop", "Again",
    "Undo", "Cut", "Copy", "Paste", "Find", "Mute"
};

char bt_hid_scancode_to_ascii(uint8_t scancode, uint8_t modifiers) {
    if (scancode >= 128) return 0;

    int shift = (modifiers & (BT_HID_MOD_LEFT_SHIFT | BT_HID_MOD_RIGHT_SHIFT)) != 0;

    if (shift) {
        return scancode_to_ascii_upper[scancode];
    } else {
        return scancode_to_ascii_lower[scancode];
    }
}

const char *bt_hid_scancode_name(uint8_t scancode) {
    if (scancode >= 128) return "Unknown";
    return scancode_names[scancode];
}

// ============================================================================
// HID Device Management
// ============================================================================

static bt_hid_device_t *hid_alloc_device(void) {
    for (int i = 0; i < BT_HID_MAX_DEVICES; i++) {
        if (!hid_state.devices[i].active) {
            bt_hid_device_t *dev = &hid_state.devices[i];
            memset(dev, 0, sizeof(bt_hid_device_t));
            dev->active = 1;
            hid_state.device_count++;
            return dev;
        }
    }
    return NULL;
}

static void hid_free_device(bt_hid_device_t *dev) {
    if (dev && dev->active) {
        dev->active = 0;
        hid_state.device_count--;
    }
}

bt_hid_device_t *bt_hid_find_device(bt_addr_t *addr) {
    for (int i = 0; i < BT_HID_MAX_DEVICES; i++) {
        if (hid_state.devices[i].active &&
            bt_addr_cmp(&hid_state.devices[i].addr, addr) == 0) {
            return &hid_state.devices[i];
        }
    }
    return NULL;
}

bt_hid_device_t *bt_hid_get_device(int index) {
    if (index < 0 || index >= BT_HID_MAX_DEVICES) {
        return NULL;
    }
    return hid_state.devices[index].active ? &hid_state.devices[index] : NULL;
}

int bt_hid_get_device_count(void) {
    return hid_state.device_count;
}

// Find device by L2CAP CID
static bt_hid_device_t *hid_find_by_cid(uint16_t cid) {
    for (int i = 0; i < BT_HID_MAX_DEVICES; i++) {
        if (hid_state.devices[i].active &&
            (hid_state.devices[i].ctrl_cid == cid ||
             hid_state.devices[i].intr_cid == cid)) {
            return &hid_state.devices[i];
        }
    }
    return NULL;
}

// ============================================================================
// HID Report Processing
// ============================================================================

static void hid_process_keyboard_report(bt_hid_device_t *dev,
                                         const uint8_t *data, uint16_t len) {
    if (len < 8) {
        kprintf("[HID] Keyboard report too short: %d bytes\n", len);
        return;
    }

    bt_hid_keyboard_report_t report;
    report.modifiers = data[0];
    report.reserved = data[1];
    for (int i = 0; i < 6; i++) {
        report.keys[i] = data[2 + i];
    }

    // Check for changes
    int changed = (report.modifiers != dev->last_keyboard.modifiers);
    for (int i = 0; i < 6 && !changed; i++) {
        changed = (report.keys[i] != dev->last_keyboard.keys[i]);
    }

    if (changed) {
        // Log new keys
        kprintf("[HID] Keyboard: mod=0x%02x keys=[", report.modifiers);
        for (int i = 0; i < 6; i++) {
            if (report.keys[i]) {
                kprintf(" %s", bt_hid_scancode_name(report.keys[i]));
            }
        }
        kprintf(" ]\n");

        // Save and call callback
        memcpy(&dev->last_keyboard, &report, sizeof(report));

        if (hid_state.keyboard_callback) {
            hid_state.keyboard_callback(&report);
        }
    }
}

static void hid_process_mouse_report(bt_hid_device_t *dev,
                                      const uint8_t *data, uint16_t len) {
    if (len < 3) {
        kprintf("[HID] Mouse report too short: %d bytes\n", len);
        return;
    }

    bt_hid_mouse_report_t report;
    report.buttons = data[0];
    report.x = (int8_t)data[1];
    report.y = (int8_t)data[2];
    report.wheel = (len >= 4) ? (int8_t)data[3] : 0;

    // Only log if movement or button change
    if (report.buttons != dev->last_mouse.buttons ||
        report.x != 0 || report.y != 0 || report.wheel != 0) {

        if (report.buttons != dev->last_mouse.buttons) {
            kprintf("[HID] Mouse buttons: 0x%02x\n", report.buttons);
        }
        if (report.x != 0 || report.y != 0 || report.wheel != 0) {
            kprintf("[HID] Mouse move: x=%d y=%d wheel=%d\n",
                    report.x, report.y, report.wheel);
        }

        memcpy(&dev->last_mouse, &report, sizeof(report));

        if (hid_state.mouse_callback) {
            hid_state.mouse_callback(&report);
        }
    }
}

static void hid_process_data(bt_hid_device_t *dev, uint8_t report_type,
                              const uint8_t *data, uint16_t len) {
    // Only process input reports
    if (report_type != BT_HID_REPORT_INPUT) {
        return;
    }

    switch (dev->type) {
        case BT_HID_TYPE_KEYBOARD:
            hid_process_keyboard_report(dev, data, len);
            break;

        case BT_HID_TYPE_MOUSE:
            hid_process_mouse_report(dev, data, len);
            break;

        case BT_HID_TYPE_COMBO:
            // Try to determine which type based on report length
            if (len >= 8) {
                hid_process_keyboard_report(dev, data, len);
            } else if (len >= 3 && len <= 4) {
                hid_process_mouse_report(dev, data, len);
            }
            break;

        default:
            kprintf("[HID] Unknown device type, data len=%d\n", len);
            break;
    }
}

// ============================================================================
// L2CAP Data Handler
// ============================================================================

void bt_hid_l2cap_recv(uint16_t cid, const void *data, uint16_t len) {
    if (len < 1) return;

    const uint8_t *pkt = (const uint8_t *)data;
    uint8_t header = pkt[0];
    uint8_t trans_type = header & 0xF0;
    uint8_t param = header & 0x0F;

    bt_hid_device_t *dev = hid_find_by_cid(cid);
    if (!dev) {
        kprintf("[HID] No device for CID 0x%04x\n", cid);
        return;
    }

    switch (trans_type) {
        case BT_HID_HANDSHAKE:
            kprintf("[HID] Handshake: result=%d\n", param);
            if (param == BT_HID_HANDSHAKE_SUCCESS) {
                dev->state = BT_HID_STATE_READY;
            }
            break;

        case BT_HID_HID_CONTROL:
            kprintf("[HID] HID Control: operation=%d\n", param);
            if (param == BT_HID_CTRL_VC_UNPLUG) {
                // Virtual cable unplug - disconnect
                dev->state = BT_HID_STATE_DISCONNECTED;
            }
            break;

        case BT_HID_DATA:
            // Data packet: param is report type
            if (len > 1) {
                hid_process_data(dev, param, pkt + 1, len - 1);
            }
            break;

        default:
            kprintf("[HID] Unknown transaction: 0x%02x\n", header);
            break;
    }
}

// ============================================================================
// HID API
// ============================================================================

int bt_hid_connect(bt_addr_t *addr, uint16_t hci_handle) {
    // Check if already connected
    bt_hid_device_t *existing = bt_hid_find_device(addr);
    if (existing) {
        kprintf("[HID] Device already connected\n");
        return -1;
    }

    // Allocate device
    bt_hid_device_t *dev = hid_alloc_device();
    if (!dev) {
        kprintf("[HID] No free device slots\n");
        return -1;
    }

    bt_addr_copy(&dev->addr, addr);
    dev->hci_handle = hci_handle;
    dev->state = BT_HID_STATE_CONNECTING;
    dev->protocol = BT_HID_PROTOCOL_BOOT;  // Start with boot protocol

    kprintf("[HID] Connecting to %02x:%02x:%02x:%02x:%02x:%02x\n",
            addr->addr[5], addr->addr[4], addr->addr[3],
            addr->addr[2], addr->addr[1], addr->addr[0]);

    // Open L2CAP control channel
    int ctrl_cid = bt_l2cap_connect(hci_handle, L2CAP_PSM_HID_CTRL);
    if (ctrl_cid < 0) {
        kprintf("[HID] Failed to connect control channel\n");
        hid_free_device(dev);
        return -1;
    }
    dev->ctrl_cid = ctrl_cid;

    // Open L2CAP interrupt channel
    int intr_cid = bt_l2cap_connect(hci_handle, L2CAP_PSM_HID_INTR);
    if (intr_cid < 0) {
        kprintf("[HID] Failed to connect interrupt channel\n");
        bt_l2cap_disconnect(ctrl_cid);
        hid_free_device(dev);
        return -1;
    }
    dev->intr_cid = intr_cid;

    // Determine device type from Class of Device
    // (Should have been stored during discovery)
    for (int i = 0; i < bt_adapter.device_count; i++) {
        if (bt_addr_cmp(&bt_adapter.devices[i].addr, addr) == 0) {
            uint32_t cod = bt_adapter.devices[i].class_of_device;
            uint8_t major = (cod >> 8) & 0x1F;
            uint8_t minor = (cod >> 2) & 0x3F;

            if (major == BT_CLASS_PERIPHERAL) {
                if (minor & 0x10) {  // Keyboard bit
                    dev->type = BT_HID_TYPE_KEYBOARD;
                    if (minor & 0x20) {  // Pointing device bit
                        dev->type = BT_HID_TYPE_COMBO;
                    }
                } else if (minor & 0x20) {
                    dev->type = BT_HID_TYPE_MOUSE;
                } else {
                    dev->type = BT_HID_TYPE_UNKNOWN;
                }
            }
            break;
        }
    }

    kprintf("[HID] Device type: %d\n", dev->type);

    dev->state = BT_HID_STATE_CONNECTED;

    // Find device index
    for (int i = 0; i < BT_HID_MAX_DEVICES; i++) {
        if (&hid_state.devices[i] == dev) {
            return i;
        }
    }

    return 0;
}

int bt_hid_disconnect(int device_index) {
    bt_hid_device_t *dev = bt_hid_get_device(device_index);
    if (!dev) {
        return -1;
    }

    kprintf("[HID] Disconnecting device %d\n", device_index);

    // Disconnect L2CAP channels
    if (dev->intr_cid) {
        bt_l2cap_disconnect(dev->intr_cid);
    }
    if (dev->ctrl_cid) {
        bt_l2cap_disconnect(dev->ctrl_cid);
    }

    hid_free_device(dev);
    return 0;
}

int bt_hid_set_protocol(int device_index, uint8_t protocol) {
    bt_hid_device_t *dev = bt_hid_get_device(device_index);
    if (!dev || dev->state < BT_HID_STATE_CONNECTED) {
        return -1;
    }

    uint8_t cmd[1];
    cmd[0] = BT_HID_SET_PROTOCOL | (protocol & 0x01);

    int result = bt_l2cap_send(dev->ctrl_cid, cmd, 1);
    if (result >= 0) {
        dev->protocol = protocol;
    }

    return result;
}

int bt_hid_send_report(int device_index, uint8_t report_type,
                       const void *data, uint16_t len) {
    bt_hid_device_t *dev = bt_hid_get_device(device_index);
    if (!dev || dev->state < BT_HID_STATE_CONNECTED) {
        return -1;
    }

    static uint8_t buf[64];
    if (len > sizeof(buf) - 1) {
        return -1;
    }

    buf[0] = BT_HID_DATA | (report_type & 0x03);
    memcpy(buf + 1, data, len);

    return bt_l2cap_send(dev->ctrl_cid, buf, 1 + len);
}

void bt_hid_register_keyboard_callback(bt_hid_keyboard_callback_t callback) {
    hid_state.keyboard_callback = callback;
}

void bt_hid_register_mouse_callback(bt_hid_mouse_callback_t callback) {
    hid_state.mouse_callback = callback;
}

void bt_hid_print_status(void) {
    kprintf("\n[HID] Status:\n");
    kprintf("  Initialized: %s\n", hid_state.initialized ? "Yes" : "No");
    kprintf("  Connected devices: %d\n", hid_state.device_count);

    const char *type_names[] = { "Unknown", "Keyboard", "Mouse", "Combo" };
    const char *state_names[] = { "Disconnected", "Connecting", "Connected", "Ready" };

    for (int i = 0; i < BT_HID_MAX_DEVICES; i++) {
        bt_hid_device_t *dev = &hid_state.devices[i];
        if (dev->active) {
            kprintf("  Device %d: %02x:%02x:%02x:%02x:%02x:%02x\n", i,
                    dev->addr.addr[5], dev->addr.addr[4], dev->addr.addr[3],
                    dev->addr.addr[2], dev->addr.addr[1], dev->addr.addr[0]);
            kprintf("    Type: %s, State: %s\n",
                    type_names[dev->type], state_names[dev->state]);
            kprintf("    Control CID: 0x%04x, Interrupt CID: 0x%04x\n",
                    dev->ctrl_cid, dev->intr_cid);
            kprintf("    Protocol: %s\n",
                    dev->protocol == BT_HID_PROTOCOL_BOOT ? "Boot" : "Report");
        }
    }
}

// ============================================================================
// HID Initialization
// ============================================================================

int bt_hid_init(void) {
    kprintf("\n[HID] Initializing HID profile...\n");

    memset(&hid_state, 0, sizeof(hid_state));

    hid_state.initialized = 1;

    kprintf("[HID] HID profile initialized\n");
    return 0;
}

void bt_hid_shutdown(void) {
    if (!hid_state.initialized) return;

    kprintf("[HID] Shutting down HID profile\n");

    // Disconnect all devices
    for (int i = 0; i < BT_HID_MAX_DEVICES; i++) {
        if (hid_state.devices[i].active) {
            bt_hid_disconnect(i);
        }
    }

    hid_state.initialized = 0;
}
