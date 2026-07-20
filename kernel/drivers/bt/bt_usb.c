// bt_usb.c - USB Transport for Bluetooth HCI
// Handles communication with USB Bluetooth adapters
#include "bluetooth.h"
#include "bt_hci.h"
#include "../usb.h"
#include "../pci.h"
#include "../../serial.h"
#include "../../string.h"
#include "../../mm/heap.h"

// ============================================================================
// USB Bluetooth Constants
// ============================================================================

// USB Bluetooth interface endpoints
#define BT_USB_EP_CONTROL       0x00    // Control transfers (HCI commands)
#define BT_USB_EP_INTERRUPT     0x81    // Interrupt IN (HCI events)
#define BT_USB_EP_BULK_IN       0x82    // Bulk IN (ACL data)
#define BT_USB_EP_BULK_OUT      0x02    // Bulk OUT (ACL data)

// USB request types for HCI
#define BT_USB_HCI_CMD_REQ_TYPE     0x20    // Class, Host-to-Device, Interface
#define BT_USB_HCI_CMD_REQUEST      0x00    // Send HCI command

// Buffer sizes
#define BT_USB_CMD_BUFFER_SIZE      256
#define BT_USB_EVENT_BUFFER_SIZE    256
#define BT_USB_ACL_BUFFER_SIZE      1024

// ============================================================================
// Global State
// ============================================================================

// Global Bluetooth adapter
bt_usb_adapter_t bt_adapter;

// USB controller used for Bluetooth
static usb_controller_t *bt_usb_controller = NULL;

// Buffers for USB communication
static uint8_t bt_cmd_buffer[BT_USB_CMD_BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t bt_event_buffer[BT_USB_EVENT_BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t bt_acl_in_buffer[BT_USB_ACL_BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t bt_acl_out_buffer[BT_USB_ACL_BUFFER_SIZE] __attribute__((aligned(16)));

// ============================================================================
// USB Bluetooth Adapter Detection
// ============================================================================

// Check if a USB device is a Bluetooth adapter
static int bt_usb_is_bluetooth_device(usb_device_t *dev) {
    if (!dev) return 0;

    // Check device class (Wireless Controller)
    if (dev->desc.bDeviceClass == BT_USB_CLASS &&
        dev->desc.bDeviceSubClass == BT_USB_SUBCLASS &&
        dev->desc.bDeviceProtocol == BT_USB_PROTOCOL) {
        return 1;
    }

    // Some adapters report class at interface level, not device level
    // We'd need to check interface descriptors, but for now assume device-level
    return 0;
}

// Find USB Bluetooth adapter
static int bt_usb_find_adapter(void) {
    kprintf("[BT-USB] Searching for Bluetooth adapter...\n");

    // Iterate through USB controllers
    for (int c = 0; c < usb_controller_count; c++) {
        usb_controller_t *ctrl = &usb_controllers[c];
        if (!ctrl->initialized) continue;

        kprintf("[BT-USB] Checking controller %d (%s)...\n", c, usb_type_name(ctrl->type));

        // Check devices on this controller
        for (int d = 0; d < ctrl->num_devices; d++) {
            usb_device_t *dev = &ctrl->devices[d];
            if (dev->state == USB_STATE_DISCONNECTED) continue;

            // Check vendor/product for known Bluetooth adapters
            // Common Bluetooth adapter vendor IDs:
            // 0x0A5C - Broadcom
            // 0x0CF3 - Qualcomm Atheros
            // 0x8087 - Intel
            // 0x0A12 - Cambridge Silicon Radio (CSR)

            kprintf("[BT-USB] Device %d: VID=%04x PID=%04x Class=%02x/%02x/%02x\n",
                    d, dev->desc.idVendor, dev->desc.idProduct,
                    dev->desc.bDeviceClass, dev->desc.bDeviceSubClass,
                    dev->desc.bDeviceProtocol);

            if (bt_usb_is_bluetooth_device(dev)) {
                kprintf("[BT-USB] Found Bluetooth adapter!\n");
                bt_adapter.usb_controller_index = c;
                bt_adapter.usb_device_slot = d;
                bt_usb_controller = ctrl;
                return 0;
            }

            // Check for known vendor IDs even if class doesn't match
            uint16_t vid = dev->desc.idVendor;
            if (vid == 0x0A5C || vid == 0x0CF3 || vid == 0x8087 || vid == 0x0A12) {
                kprintf("[BT-USB] Found likely Bluetooth adapter (known vendor)\n");
                bt_adapter.usb_controller_index = c;
                bt_adapter.usb_device_slot = d;
                bt_usb_controller = ctrl;
                return 0;
            }
        }
    }

    kprintf("[BT-USB] No Bluetooth adapter found\n");
    return -1;
}

// ============================================================================
// USB Transport Operations
// ============================================================================

// Send HCI command over USB control endpoint
int bt_usb_send_command(const uint8_t *cmd, int len) {
    if (!bt_adapter.active || !bt_usb_controller) {
        return -1;
    }

    if (len > BT_USB_CMD_BUFFER_SIZE) {
        kprintf("[BT-USB] Command too large: %d bytes\n", len);
        return -1;
    }

    // Copy command to aligned buffer
    memcpy(bt_cmd_buffer, cmd, len);

    // USB control transfer for HCI command
    // bmRequestType: 0x20 (Class, Host-to-Device, Interface)
    // bRequest: 0x00
    // wValue: 0x0000
    // wIndex: 0x0000 (interface 0)
    // wLength: command length

    usb_device_t *dev = &bt_usb_controller->devices[bt_adapter.usb_device_slot];

    int result = usb_control_transfer(bt_usb_controller, dev,
                                       BT_USB_HCI_CMD_REQ_TYPE,
                                       BT_USB_HCI_CMD_REQUEST,
                                       0x0000,      // wValue
                                       0x0000,      // wIndex (interface)
                                       bt_cmd_buffer,
                                       len);

    if (result < 0) {
        kprintf("[BT-USB] Failed to send command\n");
    }

    return result;
}

// Receive HCI event from USB interrupt endpoint
int bt_usb_receive_event(uint8_t *buffer, int max_len) {
    if (!bt_adapter.active || !bt_usb_controller) {
        return -1;
    }

    usb_device_t *dev = &bt_usb_controller->devices[bt_adapter.usb_device_slot];

    // Read from interrupt endpoint
    if (bt_usb_controller->interrupt_transfer) {
        int result = bt_usb_controller->interrupt_transfer(
            bt_usb_controller, dev,
            bt_adapter.ep_interrupt,
            bt_event_buffer,
            max_len < BT_USB_EVENT_BUFFER_SIZE ? max_len : BT_USB_EVENT_BUFFER_SIZE);

        if (result > 0) {
            memcpy(buffer, bt_event_buffer, result);
        }
        return result;
    }

    return -1;
}

// Send ACL data over USB bulk OUT endpoint
int bt_usb_send_acl(const uint8_t *data, int len) {
    if (!bt_adapter.active || !bt_usb_controller) {
        return -1;
    }

    if (len > BT_USB_ACL_BUFFER_SIZE) {
        kprintf("[BT-USB] ACL data too large: %d bytes\n", len);
        return -1;
    }

    memcpy(bt_acl_out_buffer, data, len);

    usb_device_t *dev = &bt_usb_controller->devices[bt_adapter.usb_device_slot];

    if (bt_usb_controller->bulk_transfer) {
        return bt_usb_controller->bulk_transfer(
            bt_usb_controller, dev,
            bt_adapter.ep_bulk_out,
            bt_acl_out_buffer,
            len,
            0);  // direction: out
    }

    return -1;
}

// Receive ACL data from USB bulk IN endpoint
int bt_usb_receive_acl(uint8_t *buffer, int max_len) {
    if (!bt_adapter.active || !bt_usb_controller) {
        return -1;
    }

    usb_device_t *dev = &bt_usb_controller->devices[bt_adapter.usb_device_slot];

    if (bt_usb_controller->bulk_transfer) {
        int result = bt_usb_controller->bulk_transfer(
            bt_usb_controller, dev,
            bt_adapter.ep_bulk_in,
            bt_acl_in_buffer,
            max_len < BT_USB_ACL_BUFFER_SIZE ? max_len : BT_USB_ACL_BUFFER_SIZE,
            1);  // direction: in

        if (result > 0) {
            memcpy(buffer, bt_acl_in_buffer, result);
        }
        return result;
    }

    return -1;
}

// ============================================================================
// USB Adapter Initialization
// ============================================================================

// Configure USB endpoints for Bluetooth
static int bt_usb_configure_endpoints(void) {
    // Standard USB Bluetooth endpoints
    bt_adapter.ep_control = BT_USB_EP_CONTROL;
    bt_adapter.ep_interrupt = BT_USB_EP_INTERRUPT;
    bt_adapter.ep_bulk_in = BT_USB_EP_BULK_IN;
    bt_adapter.ep_bulk_out = BT_USB_EP_BULK_OUT;

    kprintf("[BT-USB] Configured endpoints:\n");
    kprintf("[BT-USB]   Control:   0x%02x\n", bt_adapter.ep_control);
    kprintf("[BT-USB]   Interrupt: 0x%02x\n", bt_adapter.ep_interrupt);
    kprintf("[BT-USB]   Bulk IN:   0x%02x\n", bt_adapter.ep_bulk_in);
    kprintf("[BT-USB]   Bulk OUT:  0x%02x\n", bt_adapter.ep_bulk_out);

    return 0;
}

// Initialize USB Bluetooth transport
int bt_usb_init(void) {
    kprintf("\n[BT-USB] Initializing USB Bluetooth transport...\n");

    // Clear adapter state
    memset(&bt_adapter, 0, sizeof(bt_adapter));

    // Find Bluetooth adapter
    if (bt_usb_find_adapter() < 0) {
        kprintf("[BT-USB] No Bluetooth adapter found\n");
        return -1;
    }

    // Configure endpoints
    if (bt_usb_configure_endpoints() < 0) {
        kprintf("[BT-USB] Failed to configure endpoints\n");
        return -1;
    }

    // Mark adapter as active
    bt_adapter.active = 1;

    // Initialize HCI layer
    if (bt_hci_init() < 0) {
        kprintf("[BT-USB] Failed to initialize HCI\n");
        bt_adapter.active = 0;
        return -1;
    }

    kprintf("[BT-USB] USB Bluetooth transport initialized\n");
    return 0;
}

// Shutdown USB Bluetooth transport
void bt_usb_shutdown(void) {
    if (!bt_adapter.active) return;

    kprintf("[BT-USB] Shutting down USB Bluetooth transport\n");

    // Shutdown HCI
    bt_hci_shutdown();

    bt_adapter.active = 0;
    bt_usb_controller = NULL;
}

// Poll for incoming data
void bt_usb_poll(void) {
    if (!bt_adapter.active) return;

    // Poll for HCI events
    uint8_t event_buffer[256];
    int len = bt_usb_receive_event(event_buffer, sizeof(event_buffer));
    if (len > 0) {
        bt_hci_process_event(event_buffer, len);
    }

    // Poll for ACL data
    uint8_t acl_buffer[1024];
    len = bt_usb_receive_acl(acl_buffer, sizeof(acl_buffer));
    if (len > 0) {
        bt_hci_process_acl(acl_buffer, len);
    }
}

// Print USB transport status
void bt_usb_print_status(void) {
    kprintf("\n[BT-USB] USB Transport Status:\n");
    kprintf("  Active: %s\n", bt_adapter.active ? "Yes" : "No");

    if (bt_adapter.active) {
        kprintf("  USB Controller: %d\n", bt_adapter.usb_controller_index);
        kprintf("  USB Device Slot: %d\n", bt_adapter.usb_device_slot);
        kprintf("  Endpoints: Ctrl=0x%02x Int=0x%02x BulkIn=0x%02x BulkOut=0x%02x\n",
                bt_adapter.ep_control, bt_adapter.ep_interrupt,
                bt_adapter.ep_bulk_in, bt_adapter.ep_bulk_out);
    }
}
