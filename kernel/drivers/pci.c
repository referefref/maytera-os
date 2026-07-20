// pci.c - PCI Bus driver implementation
#include "pci.h"
#include "../serial.h"
#include "../string.h"

// Maximum devices to track
#define MAX_PCI_DEVICES 64

// Discovered devices
static pci_device_t pci_devices[MAX_PCI_DEVICES];
static int pci_device_count = 0;

// Forward declaration
static const char *pci_class_name(uint8_t class_code, uint8_t subclass);

// Build PCI address
static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (1U << 31) |  // Enable bit
           ((uint32_t)bus << 16) |
           ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) |
           (offset & 0xFC);
}

// Read 8-bit value from PCI config space
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, offset));
    return inb(PCI_CONFIG_DATA + (offset & 3));
}

// Read 16-bit value from PCI config space
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, offset));
    return inw(PCI_CONFIG_DATA + (offset & 2));
}

// Read 32-bit value from PCI config space
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

// Write 8-bit value to PCI config space
void pci_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, offset));
    outb(PCI_CONFIG_DATA + (offset & 3), value);
}

// Write 16-bit value to PCI config space
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, offset));
    outw(PCI_CONFIG_DATA + (offset & 2), value);
}

// Write 32-bit value to PCI config space
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

// Check and add a PCI device
static void pci_check_device(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor_id = pci_read16(bus, slot, func, PCI_VENDOR_ID);

    // Check if device exists
    if (vendor_id == 0xFFFF) return;

    if (pci_device_count >= MAX_PCI_DEVICES) return;

    pci_device_t *dev = &pci_devices[pci_device_count++];

    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    dev->vendor_id = vendor_id;
    dev->device_id = pci_read16(bus, slot, func, PCI_DEVICE_ID);
    dev->class_code = pci_read8(bus, slot, func, PCI_CLASS);
    dev->subclass = pci_read8(bus, slot, func, PCI_SUBCLASS);
    dev->prog_if = pci_read8(bus, slot, func, PCI_PROG_IF);
    dev->revision = pci_read8(bus, slot, func, PCI_REVISION_ID);
    dev->header_type = pci_read8(bus, slot, func, PCI_HEADER_TYPE);
    dev->interrupt_line = pci_read8(bus, slot, func, PCI_INTERRUPT_LINE);
    dev->interrupt_pin = pci_read8(bus, slot, func, PCI_INTERRUPT_PIN);

    // Read BARs (for type 0 headers only)
    if ((dev->header_type & 0x7F) == 0) {
        for (int i = 0; i < 6; i++) {
            dev->bar[i] = pci_read32(bus, slot, func, PCI_BAR0 + i * 4);
        }
    }
}

// Scan PCI bus
static void pci_scan_bus(uint8_t bus) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint16_t vendor_id = pci_read16(bus, slot, 0, PCI_VENDOR_ID);
        if (vendor_id == 0xFFFF) continue;

        pci_check_device(bus, slot, 0);

        // Check if multi-function device
        uint8_t header_type = pci_read8(bus, slot, 0, PCI_HEADER_TYPE);
        if (header_type & 0x80) {
            for (uint8_t func = 1; func < 8; func++) {
                pci_check_device(bus, slot, func);
            }
        }
    }
}

// Initialize PCI driver
void pci_init(void) {
    kprintf("[PCI] Scanning PCI bus...\n");

    pci_device_count = 0;

    // Scan bus 0 and check for additional buses
    for (int bus = 0; bus < 256; bus++) {
        pci_scan_bus(bus);
    }

    kprintf("[PCI] Found %d devices\n", pci_device_count);
}

// #418: record that a driver successfully claimed this PCI function. See
// pci.h for why this exists (closing out "no driver for this device" with
// certainty in /DEVLOG.TXT rather than an absence-of-evidence argument).
void pci_mark_claimed(pci_device_t *dev, const char *driver_name) {
    if (!dev) return;
    dev->claimed = 1;
    int i = 0;
    if (driver_name) {
        for (; i < (int)sizeof(dev->claimed_by) - 1 && driver_name[i]; i++) {
            dev->claimed_by[i] = driver_name[i];
        }
    }
    dev->claimed_by[i] = 0;
}

// Find device by vendor/device ID
pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id &&
            pci_devices[i].device_id == device_id) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

// Find device by class/subclass
pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass == subclass) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

// Find device by vendor ID only
pci_device_t *pci_find_vendor(uint16_t vendor_id) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

// Find device by vendor ID and class
pci_device_t *pci_find_vendor_class(uint16_t vendor_id, uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id &&
            pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass == subclass) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

// Find next device matching class (for iterating)
int pci_find_next_class(uint8_t class_code, uint8_t subclass, int start_idx) {
    for (int i = start_idx + 1; i < pci_device_count; i++) {
        if (pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass == subclass) {
            return i;
        }
    }
    return -1;
}

// Enable bus mastering
void pci_enable_bus_master(pci_device_t *dev) {
    uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEMORY | PCI_CMD_IO;
    pci_write16(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

// Get BAR address
uint64_t pci_get_bar_address(pci_device_t *dev, int bar_num) {
    if (bar_num < 0 || bar_num > 5) return 0;

    uint32_t bar = dev->bar[bar_num];

    if (bar & PCI_BAR_IO) {
        // I/O BAR
        return bar & ~0x3;
    } else {
        // Memory BAR
        if ((bar & 0x6) == PCI_BAR_MEM_64 && bar_num < 5) {
            // 64-bit BAR
            uint64_t addr = (bar & ~0xF);
            addr |= ((uint64_t)dev->bar[bar_num + 1] << 32);
            return addr;
        } else {
            // 32-bit BAR
            return bar & ~0xF;
        }
    }
}

// #71: walk the PCI capabilities list (PCI_STATUS bit 4 gates whether the
// Capabilities Pointer at PCI_CAP_PTR is even valid). Each capability is a
// {id, next-pointer} pair; next==0 terminates the list. Bounded to 48 hops so
// a malformed/cyclic list can never hang boot.
uint8_t pci_find_capability(pci_device_t *dev, uint8_t cap_id) {
    if (!dev) return 0;

    uint16_t status = pci_read16(dev->bus, dev->slot, dev->func, PCI_STATUS);
    if (!(status & PCI_STATUS_CAPLIST)) return 0;

    uint8_t ptr = pci_read8(dev->bus, dev->slot, dev->func, PCI_CAP_PTR) & 0xFC;
    for (int guard = 0; ptr != 0 && guard < 48; guard++) {
        uint8_t id = pci_read8(dev->bus, dev->slot, dev->func, ptr);
        if (id == cap_id) return ptr;
        ptr = pci_read8(dev->bus, dev->slot, dev->func, ptr + 1) & 0xFC;
    }
    return 0;
}

// #71: program + enable MSI on a device (see pci.h for the full rationale).
// Message Address = 0xFEE00000 | (dest_apic_id << 12) targets the Local APIC
// directly (RH=0 no redirection hint, DM=0 physical destination mode); Message
// Data = vector (delivery mode 0 = Fixed, trigger = edge). Handles both the
// 32-bit-only and 64-bit-capable MSI capability layouts. Also sets
// PCI_CMD_INT_DISABLE so the device does not ALSO keep asserting its (likely
// unrouted) legacy INTx line once MSI is live.
int pci_enable_msi(pci_device_t *dev, uint8_t vector, uint32_t dest_apic_id) {
    uint8_t cap = pci_find_capability(dev, PCI_CAP_ID_MSI);
    if (!cap) return 0;

    uint16_t msgctl = pci_read16(dev->bus, dev->slot, dev->func, cap + 2);
    int is64bit = (msgctl & (1 << 7)) != 0;
    int per_vector_mask = (msgctl & (1 << 8)) != 0;

    uint32_t addr_lo = 0xFEE00000u | ((dest_apic_id & 0xFF) << 12);
    uint16_t data = vector;   // fixed delivery (bits 10:8 = 0), edge trigger

    pci_write32(dev->bus, dev->slot, dev->func, cap + 4, addr_lo);
    if (is64bit) {
        pci_write32(dev->bus, dev->slot, dev->func, cap + 8, 0);           // address hi
        pci_write16(dev->bus, dev->slot, dev->func, cap + 12, data);
        if (per_vector_mask) {
            pci_write32(dev->bus, dev->slot, dev->func, cap + 16, 0);      // unmask vector 0
        }
    } else {
        pci_write16(dev->bus, dev->slot, dev->func, cap + 8, data);
        if (per_vector_mask) {
            pci_write32(dev->bus, dev->slot, dev->func, cap + 12, 0);      // unmask vector 0
        }
    }

    // Multiple Message Enable = 0 (we only ever use a single vector), MSI Enable = 1.
    msgctl &= (uint16_t)~(0x7 << 4);
    msgctl |= 0x1;
    pci_write16(dev->bus, dev->slot, dev->func, cap + 2, msgctl);

    uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_INT_DISABLE;
    pci_write16(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);

    return 1;
}

// Get BAR size (by writing all 1s and reading back)
uint32_t pci_get_bar_size(pci_device_t *dev, int bar_num) {
    if (bar_num < 0 || bar_num > 5) return 0;

    uint8_t offset = PCI_BAR0 + bar_num * 4;
    uint32_t original = pci_read32(dev->bus, dev->slot, dev->func, offset);

    // Write all 1s
    pci_write32(dev->bus, dev->slot, dev->func, offset, 0xFFFFFFFF);
    uint32_t size_mask = pci_read32(dev->bus, dev->slot, dev->func, offset);

    // Restore original value
    pci_write32(dev->bus, dev->slot, dev->func, offset, original);

    if (original & PCI_BAR_IO) {
        size_mask &= ~0x3;
    } else {
        size_mask &= ~0xF;
    }

    return ~size_mask + 1;
}

// Get total device count
int pci_get_device_count(void) {
    return pci_device_count;
}

// Get device by index
pci_device_t *pci_get_device(int index) {
    if (index < 0 || index >= pci_device_count) return NULL;
    return &pci_devices[index];
}

// Get class name (public version)
const char *pci_get_class_name(uint8_t class_code, uint8_t subclass) {
    return pci_class_name(class_code, subclass);
}

// Get class name (internal)
static const char *pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
        case 0x01:  // Storage
            switch (subclass) {
                case 0x00: return "SCSI Controller";
                case 0x01: return "IDE Controller";
                case 0x05: return "ATA Controller";
                case 0x06: return "SATA Controller";
                case 0x08: return "NVMe Controller";
                default: return "Storage Controller";
            }
        case 0x02:  // Network
            switch (subclass) {
                case 0x00: return "Ethernet Controller";
                case 0x80: return "Network Controller";
                default: return "Network Controller";
            }
        case 0x03:  // Display
            switch (subclass) {
                case 0x00: return "VGA Controller";
                case 0x01: return "XGA Controller";
                case 0x02: return "3D Controller";
                case 0x80: return "Display Controller";
                default: return "Display Controller";
            }
        case 0x04:  // Multimedia
            switch (subclass) {
                case 0x00: return "Video Controller";
                case 0x01: return "Audio Controller";
                case 0x02: return "Telephony";
                case 0x03: return "HD Audio Controller";
                default: return "Multimedia Controller";
            }
        case 0x05:  // Memory
            switch (subclass) {
                case 0x00: return "RAM Controller";
                case 0x01: return "Flash Controller";
                default: return "Memory Controller";
            }
        case 0x06:  // Bridge
            switch (subclass) {
                case 0x00: return "Host Bridge";
                case 0x01: return "ISA Bridge";
                case 0x02: return "EISA Bridge";
                case 0x03: return "MCA Bridge";
                case 0x04: return "PCI-PCI Bridge";
                case 0x05: return "PCMCIA Bridge";
                case 0x06: return "NuBus Bridge";
                case 0x07: return "CardBus Bridge";
                case 0x80: return "Other Bridge";
                default: return "Bridge Device";
            }
        case 0x07:  // Communication
            switch (subclass) {
                case 0x00: return "Serial Controller";
                case 0x01: return "Parallel Controller";
                case 0x03: return "Modem";
                default: return "Communication Controller";
            }
        case 0x08:  // System
            switch (subclass) {
                case 0x00: return "PIC";
                case 0x01: return "DMA Controller";
                case 0x02: return "Timer";
                case 0x03: return "RTC";
                case 0x80: return "System Peripheral";
                default: return "System Peripheral";
            }
        case 0x0C:  // Serial Bus
            switch (subclass) {
                case 0x00: return "FireWire Controller";
                case 0x03: return "USB Controller";
                case 0x05: return "SMBus Controller";
                case 0x07: return "IPMI";
                default: return "Serial Bus Controller";
            }
        case 0x0D:  // Wireless
            switch (subclass) {
                case 0x00: return "iRDA Controller";
                case 0x11: return "Bluetooth Controller";
                case 0x20: return "WiFi Controller";
                default: return "Wireless Controller";
            }
        default: return "Unknown Device";
    }
}

// Print all PCI devices
void pci_print_devices(void) {
    kprintf("\n[PCI] Device List:\n");
    kprintf("  Bus:Slot.Func  Vendor:Device  Class    Description\n");
    kprintf("  -------------  -------------  ------   -----------\n");

    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t *dev = &pci_devices[i];
        kprintf("  %02x:%02x.%x        %04x:%04x      %02x.%02x    %s\n",
                dev->bus, dev->slot, dev->func,
                dev->vendor_id, dev->device_id,
                dev->class_code, dev->subclass,
                pci_class_name(dev->class_code, dev->subclass));
    }
}
