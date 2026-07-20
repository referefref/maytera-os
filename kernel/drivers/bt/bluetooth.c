// bluetooth.c - Main Bluetooth Stack API Implementation
// Provides high-level API for Bluetooth functionality
#include "bluetooth.h"
#include "bt_hci.h"
#include "bt_l2cap.h"
#include "bt_hid.h"
#include "../../serial.h"
#include "../../string.h"
#include "../../mm/heap.h"

// ============================================================================
// Bluetooth Stack State
// ============================================================================

static struct {
    int initialized;
    int scan_active;
    int scan_duration;
} bt_state;

// External USB transport functions
extern int bt_usb_init(void);
extern void bt_usb_shutdown(void);
extern void bt_usb_poll(void);
extern void bt_usb_print_status(void);

// ============================================================================
// Main Bluetooth API
// ============================================================================

int bt_init(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("[BT] Initializing Bluetooth Stack\n");
    kprintf("========================================\n");

    memset(&bt_state, 0, sizeof(bt_state));

    // Initialize USB transport (also initializes HCI)
    if (bt_usb_init() < 0) {
        kprintf("[BT] USB transport initialization failed\n");
        return -1;
    }

    // Initialize L2CAP
    if (bt_l2cap_init() < 0) {
        kprintf("[BT] L2CAP initialization failed\n");
        bt_usb_shutdown();
        return -1;
    }

    // Initialize HID profile
    if (bt_hid_init() < 0) {
        kprintf("[BT] HID initialization failed\n");
        bt_l2cap_shutdown();
        bt_usb_shutdown();
        return -1;
    }

    bt_state.initialized = 1;

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("[BT] Bluetooth Stack Initialized\n");
    kprintf("========================================\n\n");

    return 0;
}

void bt_shutdown(void) {
    if (!bt_state.initialized) return;

    kprintf("[BT] Shutting down Bluetooth stack\n");

    bt_hid_shutdown();
    bt_l2cap_shutdown();
    bt_usb_shutdown();

    bt_state.initialized = 0;
}

int bt_is_available(void) {
    return bt_state.initialized && bt_adapter.hci_initialized;
}

int bt_get_local_addr(bt_addr_t *addr) {
    if (!bt_state.initialized || !addr) {
        return -1;
    }
    bt_addr_copy(addr, &bt_adapter.local_addr);
    return 0;
}

// ============================================================================
// Device Discovery
// ============================================================================

int bt_scan_start(int duration) {
    if (!bt_state.initialized) {
        return -1;
    }

    if (bt_state.scan_active) {
        kprintf("[BT] Scan already in progress\n");
        return -1;
    }

    if (duration < 1) duration = 1;
    if (duration > 30) duration = 30;

    kprintf("[BT] Starting device scan (%d seconds)\n", duration);

    // Clear previous results
    bt_adapter.device_count = 0;

    // Start HCI inquiry
    // Duration is in units of 1.28 seconds
    uint8_t hci_duration = (duration * 10) / 13;  // Convert to HCI units
    if (hci_duration < 1) hci_duration = 1;
    if (hci_duration > 48) hci_duration = 48;  // Max ~61 seconds

    int result = bt_hci_inquiry(hci_duration, 0);  // 0 = unlimited responses
    if (result < 0) {
        return result;
    }

    bt_state.scan_active = 1;
    bt_state.scan_duration = duration;

    return 0;
}

void bt_scan_stop(void) {
    if (!bt_state.initialized || !bt_state.scan_active) {
        return;
    }

    kprintf("[BT] Stopping device scan\n");
    bt_hci_inquiry_cancel();
    bt_state.scan_active = 0;
}

int bt_scan_in_progress(void) {
    return bt_state.scan_active;
}

int bt_get_discovered_devices(bt_device_t *devices, int max_devices) {
    if (!devices || max_devices <= 0) {
        return 0;
    }

    int count = bt_adapter.device_count;
    if (count > max_devices) {
        count = max_devices;
    }

    for (int i = 0; i < count; i++) {
        memcpy(&devices[i], &bt_adapter.devices[i], sizeof(bt_device_t));
    }

    return count;
}

// ============================================================================
// Connection Management
// ============================================================================

int bt_connect(bt_addr_t *addr) {
    if (!bt_state.initialized || !addr) {
        return -1;
    }

    kprintf("[BT] Connecting to %02x:%02x:%02x:%02x:%02x:%02x\n",
            addr->addr[5], addr->addr[4], addr->addr[3],
            addr->addr[2], addr->addr[1], addr->addr[0]);

    // Find device in discovered list
    bt_device_t *dev = NULL;
    for (int i = 0; i < bt_adapter.device_count; i++) {
        if (bt_addr_cmp(&bt_adapter.devices[i].addr, addr) == 0) {
            dev = &bt_adapter.devices[i];
            break;
        }
    }

    if (!dev) {
        // Device not in list, add it
        if (bt_adapter.device_count < BT_MAX_DEVICES) {
            dev = &bt_adapter.devices[bt_adapter.device_count++];
            bt_addr_copy(&dev->addr, addr);
            dev->state = BT_STATE_DISCOVERED;
        } else {
            kprintf("[BT] Device list full\n");
            return -1;
        }
    }

    dev->state = BT_STATE_CONNECTING;

    // Create HCI connection
    // Packet type: DM1, DH1, DM3, DH3, DM5, DH5 (0xCC18)
    int result = bt_hci_create_connection(addr, 0xCC18, 0x02, 0x0000, 0x01);
    if (result < 0) {
        dev->state = BT_STATE_DISCONNECTED;
        return result;
    }

    // Poll for connection completion (with timeout)
    for (int i = 0; i < 500; i++) {
        bt_usb_poll();
        if (dev->state == BT_STATE_CONNECTED) {
            kprintf("[BT] Connected!\n");

            // Check if this is a HID device
            uint8_t major = (dev->class_of_device >> 8) & 0x1F;
            if (major == BT_CLASS_PERIPHERAL) {
                // Connect HID profile
                bt_hid_connect(addr, dev->connection_handle);
            }

            return 0;
        }
        if (dev->state == BT_STATE_DISCONNECTED) {
            kprintf("[BT] Connection failed\n");
            return -1;
        }
        for (volatile int j = 0; j < 10000; j++);
    }

    kprintf("[BT] Connection timeout\n");
    dev->state = BT_STATE_DISCONNECTED;
    return -1;
}

int bt_disconnect(bt_addr_t *addr) {
    if (!bt_state.initialized || !addr) {
        return -1;
    }

    // Find device
    for (int i = 0; i < bt_adapter.device_count; i++) {
        if (bt_addr_cmp(&bt_adapter.devices[i].addr, addr) == 0) {
            bt_device_t *dev = &bt_adapter.devices[i];
            if (dev->state == BT_STATE_CONNECTED ||
                dev->state == BT_STATE_CONFIGURED) {

                // Disconnect HID if applicable
                bt_hid_device_t *hid = bt_hid_find_device(addr);
                if (hid) {
                    for (int j = 0; j < BT_HID_MAX_DEVICES; j++) {
                        if (bt_hid_get_device(j) == hid) {
                            bt_hid_disconnect(j);
                            break;
                        }
                    }
                }

                // HCI disconnect
                bt_hci_disconnect(dev->connection_handle, 0x13);  // Remote user terminated
                dev->state = BT_STATE_DISCONNECTED;
                return 0;
            }
            break;
        }
    }

    return -1;
}

int bt_pair(bt_addr_t *addr) {
    if (!bt_state.initialized || !addr) {
        return -1;
    }

    // Find device
    for (int i = 0; i < bt_adapter.device_count; i++) {
        if (bt_addr_cmp(&bt_adapter.devices[i].addr, addr) == 0) {
            bt_device_t *dev = &bt_adapter.devices[i];

            if (dev->state != BT_STATE_CONNECTED) {
                kprintf("[BT] Device not connected\n");
                return -1;
            }

            // Request authentication
            return bt_hci_auth_request(dev->connection_handle);
        }
    }

    kprintf("[BT] Device not found\n");
    return -1;
}

// ============================================================================
// Polling
// ============================================================================

// Poll for Bluetooth events (call periodically)
void bt_poll(void) {
    if (!bt_state.initialized) return;

    bt_usb_poll();
}

// ============================================================================
// Debug/Status
// ============================================================================

void bt_print_status(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("[BT] Bluetooth Stack Status\n");
    kprintf("========================================\n");

    kprintf("\nGeneral:\n");
    kprintf("  Initialized: %s\n", bt_state.initialized ? "Yes" : "No");
    kprintf("  Scan active: %s\n", bt_state.scan_active ? "Yes" : "No");

    if (bt_state.initialized) {
        kprintf("  Local Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
                bt_adapter.local_addr.addr[5], bt_adapter.local_addr.addr[4],
                bt_adapter.local_addr.addr[3], bt_adapter.local_addr.addr[2],
                bt_adapter.local_addr.addr[1], bt_adapter.local_addr.addr[0]);
    }

    // USB transport status
    bt_usb_print_status();

    // L2CAP status
    bt_l2cap_print_status();

    // HID status
    bt_hid_print_status();

    kprintf("\n");
}

void bt_print_devices(void) {
    kprintf("\n[BT] Discovered Devices (%d):\n", bt_adapter.device_count);

    if (bt_adapter.device_count == 0) {
        kprintf("  (none)\n");
        return;
    }

    const char *state_names[] = {
        "Disconnected", "Discovered", "Connecting",
        "Connected", "Authenticated", "Configured"
    };

    for (int i = 0; i < bt_adapter.device_count; i++) {
        bt_device_t *dev = &bt_adapter.devices[i];

        kprintf("  %d: %02x:%02x:%02x:%02x:%02x:%02x\n", i,
                dev->addr.addr[5], dev->addr.addr[4], dev->addr.addr[3],
                dev->addr.addr[2], dev->addr.addr[1], dev->addr.addr[0]);

        if (dev->name[0]) {
            kprintf("     Name: %s\n", dev->name);
        }

        kprintf("     CoD: 0x%06x", dev->class_of_device);

        // Decode Class of Device
        uint8_t major = (dev->class_of_device >> 8) & 0x1F;
        const char *major_names[] = {
            "Misc", "Computer", "Phone", "LAN", "A/V", "Peripheral",
            "Imaging", "Wearable", "Toy", "Health"
        };
        if (major < 10) {
            kprintf(" (%s)", major_names[major]);
        }
        kprintf("\n");

        kprintf("     State: %s\n", state_names[dev->state]);

        if (dev->connection_handle) {
            kprintf("     Handle: 0x%04x\n", dev->connection_handle);
        }
    }
}
