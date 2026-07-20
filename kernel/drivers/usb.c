// usb.c - USB Host Controller Driver
// Unified interface for UHCI, OHCI, EHCI, and xHCI controllers
#include "usb.h"
#include "xhci.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"

// Global USB state
usb_controller_t usb_controllers[4];
int usb_controller_count = 0;

// Get USB controller type name
const char *usb_type_name(int type) {
    switch (type) {
        case USB_TYPE_UHCI: return "UHCI";
        case USB_TYPE_OHCI: return "OHCI";
        case USB_TYPE_EHCI: return "EHCI";
        case USB_TYPE_XHCI: return "xHCI";
        default: return "Unknown";
    }
}

// USB speed names
__attribute__((unused)) static const char *usb_speed_name(int speed) {
    switch (speed) {
        case 0: return "Full-Speed (12 Mbps)";
        case 1: return "Low-Speed (1.5 Mbps)";
        case 2: return "High-Speed (480 Mbps)";
        case 3: return "Super-Speed (5 Gbps)";
        default: return "Unknown";
    }
}

// ============================================================================
// xHCI Controller Support (USB 3.0) - Uses dedicated xhci.c driver
// ============================================================================

static int usb_xhci_init(usb_controller_t *ctrl) {
    // Use the full xHCI driver
    if (xhci_init(ctrl->pci) < 0) {
        ctrl->initialized = 0;
        return -1;
    }

    ctrl->initialized = 1;

    // Get xHCI controller info
    xhci_controller_t *xhc = xhci_get_controller(xhci_get_controller_count() - 1);
    if (xhc) {
        ctrl->num_ports = xhc->max_ports;
    }

    return 0;
}

// ============================================================================
// EHCI Controller Support (USB 2.0) - Stub
// ============================================================================

static int ehci_init(usb_controller_t *ctrl) {
    kprintf("[EHCI] Controller detected but not yet implemented\n");
    kprintf("[EHCI]   Base address: %016lx\n", ctrl->base_addr);
    ctrl->initialized = 0;
    return -1;
}

// ============================================================================
// UHCI Controller Support (USB 1.x) - Stub
// ============================================================================

static int uhci_init(usb_controller_t *ctrl) {
    kprintf("[UHCI] Controller detected but not yet implemented\n");
    kprintf("[UHCI]   Base address: %016lx\n", ctrl->base_addr);
    ctrl->initialized = 0;
    return -1;
}

// ============================================================================
// OHCI Controller Support (USB 1.x) - Stub
// ============================================================================

static int ohci_init(usb_controller_t *ctrl) {
    kprintf("[OHCI] Controller detected but not yet implemented\n");
    kprintf("[OHCI]   Base address: %016lx\n", ctrl->base_addr);
    ctrl->initialized = 0;
    return -1;
}

// ============================================================================
// USB Subsystem
// ============================================================================

// Initialize USB subsystem
void usb_init(void) {
    kprintf("\n[USB] Initializing USB subsystem...\n");

    usb_controller_count = 0;
    memset(usb_controllers, 0, sizeof(usb_controllers));

    // Scan PCI for USB controllers
    int device_count = pci_get_device_count();
    for (int i = 0; i < device_count && usb_controller_count < 4; i++) {
        pci_device_t *dev = pci_get_device(i);
        if (!dev) continue;

        // Check for USB controller (class 0x0C, subclass 0x03)
        if (dev->class_code == USB_PCI_CLASS && dev->subclass == USB_PCI_SUBCLASS) {
            usb_controller_t *ctrl = &usb_controllers[usb_controller_count];

            ctrl->type = dev->prog_if;
            ctrl->pci = dev;
            ctrl->initialized = 0;

            kprintf("[USB] Found %s controller at PCI %02x:%02x.%x\n",
                    usb_type_name(ctrl->type), dev->bus, dev->slot, dev->func);

            // Enable bus mastering and memory space
            pci_enable_bus_master(dev);

            // Get base address from BAR0 (EHCI/xHCI use memory-mapped I/O)
            // UHCI uses I/O ports (BAR4)
            if (ctrl->type == USB_TYPE_UHCI) {
                // UHCI uses I/O ports from BAR4
                ctrl->base_addr = dev->bar[4] & ~0x3;
                kprintf("[USB]   I/O Base: 0x%04lx\n", ctrl->base_addr);
            } else {
                // OHCI, EHCI, xHCI use memory-mapped BAR0
                ctrl->base_addr = pci_get_bar_address(dev, 0);
                kprintf("[USB]   MMIO Base: 0x%016lx\n", ctrl->base_addr);
            }

            // Initialize based on controller type
            switch (ctrl->type) {
                case USB_TYPE_XHCI:
                    usb_xhci_init(ctrl);
                    break;
                case USB_TYPE_EHCI:
                    ehci_init(ctrl);
                    break;
                case USB_TYPE_UHCI:
                    uhci_init(ctrl);
                    break;
                case USB_TYPE_OHCI:
                    ohci_init(ctrl);
                    break;
                default:
                    kprintf("[USB]   Unknown controller type: 0x%02x\n", ctrl->type);
                    break;
            }

            usb_controller_count++;
        }
    }

    if (usb_controller_count == 0) {
        kprintf("[USB] No USB controllers found\n");
    } else {
        kprintf("[USB] Found %d USB controller(s)\n", usb_controller_count);

        // Enumerate devices on initialized controllers
        for (int i = 0; i < usb_controller_count; i++) {
            if (usb_controllers[i].initialized) {
                usb_enumerate_devices(&usb_controllers[i]);
            }
        }
    }
}

// Print USB controller info
void usb_print_controllers(void) {
    kprintf("\n[USB] Controller Summary:\n");
    for (int i = 0; i < usb_controller_count; i++) {
        usb_controller_t *ctrl = &usb_controllers[i];
        kprintf("  %d: %s at PCI %02x:%02x.%x, %s\n",
                i, usb_type_name(ctrl->type),
                ctrl->pci->bus, ctrl->pci->slot, ctrl->pci->func,
                ctrl->initialized ? "initialized" : "not initialized");
        if (ctrl->initialized && ctrl->num_ports > 0) {
            kprintf("     Ports: %d\n", ctrl->num_ports);
        }
    }
}

// Enumerate devices on a controller
int usb_enumerate_devices(usb_controller_t *ctrl) {
    if (!ctrl || !ctrl->initialized) {
        return -1;
    }

    // Use xHCI enumeration for xHCI controllers
    if (ctrl->type == USB_TYPE_XHCI) {
        xhci_controller_t *xhc = xhci_get_controller(0);
        if (xhc) {
            return xhci_enumerate_devices(xhc);
        }
    }

    // TODO: Implement for other controller types
    return 0;
}

// Control transfer (unified interface)
int usb_control_transfer(usb_controller_t *ctrl, usb_device_t *dev,
                         uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index,
                         void *data, uint16_t length) {
    if (!ctrl || !ctrl->initialized) {
        return -1;
    }

    if (ctrl->type == USB_TYPE_XHCI) {
        xhci_controller_t *xhc = xhci_get_controller(0);
        if (xhc && dev) {
            return xhci_control_transfer(xhc, dev->slot,
                                         request_type, request,
                                         value, index,
                                         data, length);
        }
    }

    // TODO: Implement for other controller types
    return -1;
}

int usb_get_device_descriptor(usb_controller_t *ctrl, usb_device_t *dev) {
    return usb_control_transfer(ctrl, dev,
                                USB_REQ_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
                                USB_REQ_GET_DESCRIPTOR,
                                (USB_DESC_DEVICE << 8) | 0,
                                0,
                                &dev->desc,
                                sizeof(usb_device_desc_t));
}

int usb_set_address(usb_controller_t *ctrl, usb_device_t *dev, uint8_t address) {
    return usb_control_transfer(ctrl, dev,
                                USB_REQ_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
                                USB_REQ_SET_ADDRESS,
                                address,
                                0,
                                NULL,
                                0);
}

int usb_set_configuration(usb_controller_t *ctrl, usb_device_t *dev, uint8_t config) {
    return usb_control_transfer(ctrl, dev,
                                USB_REQ_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
                                USB_REQ_SET_CONFIG,
                                config,
                                0,
                                NULL,
                                0);
}
