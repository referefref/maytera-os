// pci.h - PCI Bus driver (Extended)
#ifndef PCI_H
#define PCI_H

#include "../types.h"

// PCI configuration space ports
#define PCI_CONFIG_ADDR     0xCF8
#define PCI_CONFIG_DATA     0xCFC

// PCI configuration space register offsets
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_CACHE_LINE      0x0C
#define PCI_LATENCY         0x0D
#define PCI_HEADER_TYPE     0x0E
#define PCI_BIST            0x0F
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_SUBSYS_VENDOR   0x2C
#define PCI_SUBSYS_ID       0x2E
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D
#define PCI_CAP_PTR         0x34  // Capabilities Pointer (valid iff PCI_STATUS_CAPLIST set)

// PCI status register bits
#define PCI_STATUS_CAPLIST  0x0010  // Capabilities List present

// PCI capability IDs
#define PCI_CAP_ID_MSI      0x05

// PCI command register bits
#define PCI_CMD_IO          0x0001  // I/O space enable
#define PCI_CMD_MEMORY      0x0002  // Memory space enable
#define PCI_CMD_BUS_MASTER  0x0004  // Bus master enable
#define PCI_CMD_INT_DISABLE 0x0400  // Interrupt disable

// BAR types
#define PCI_BAR_IO          0x01
#define PCI_BAR_MEM         0x00
#define PCI_BAR_MEM_32      0x00
#define PCI_BAR_MEM_64      0x04

// PCI Class codes
#define PCI_CLASS_STORAGE       0x01
#define PCI_CLASS_NETWORK       0x02
#define PCI_CLASS_DISPLAY       0x03
#define PCI_CLASS_MULTIMEDIA    0x04
#define PCI_CLASS_MEMORY        0x05
#define PCI_CLASS_BRIDGE        0x06
#define PCI_CLASS_SERIAL        0x0C

// PCI Subclass codes - Display
#define PCI_SUBCLASS_VGA        0x00
#define PCI_SUBCLASS_XGA        0x01
#define PCI_SUBCLASS_3D         0x02

// PCI Subclass codes - Multimedia
#define PCI_SUBCLASS_VIDEO      0x00
#define PCI_SUBCLASS_AUDIO      0x01
#define PCI_SUBCLASS_TELEPHONY  0x02
#define PCI_SUBCLASS_HDA        0x03    // HD Audio

// PCI Subclass codes - Storage
#define PCI_SUBCLASS_SCSI       0x00
#define PCI_SUBCLASS_IDE        0x01
#define PCI_SUBCLASS_SATA       0x06
#define PCI_SUBCLASS_NVME       0x08

// PCI Subclass codes - Serial Bus
#define PCI_SUBCLASS_USB        0x03

// Common vendor IDs
#define PCI_VENDOR_INTEL    0x8086
#define PCI_VENDOR_AMD      0x1002
#define PCI_VENDOR_NVIDIA   0x10DE
#define PCI_VENDOR_REALTEK  0x10EC
#define PCI_VENDOR_QEMU     0x1234
#define PCI_VENDOR_REDHAT   0x1AF4  // VirtIO

// Common device IDs
#define PCI_DEVICE_E1000    0x100E  // Intel 82540EM
#define PCI_DEVICE_E1000_2  0x100F  // Intel 82545EM
#define PCI_DEVICE_E1000_3  0x10D3  // Intel 82574L

// VirtIO devices
#define PCI_DEVICE_VIRTIO_NET   0x1000
#define PCI_DEVICE_VIRTIO_BLK   0x1001
#define PCI_DEVICE_VIRTIO_GPU   0x1050

// PCI device structure
typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    uint8_t  interrupt_line;
    uint8_t  interrupt_pin;
    uint32_t bar[6];
    // #418: set by pci_mark_claimed() once a driver successfully brings this
    // function up. Lets /DEVLOG.TXT show, for every enumerated PCI function,
    // whether ANY driver claimed it - closing out "does this device even
    // have a driver" (e.g. the iMac's Broadcom NIC/WiFi, which this kernel
    // has none for) with certainty instead of an absence-of-evidence
    // argument. Defaults to 0/empty (BSS-zeroed) for any device no driver
    // calls pci_mark_claimed() on.
    uint8_t  claimed;
    char     claimed_by[16];
} pci_device_t;

// Initialize PCI driver
void pci_init(void);

// #418: mark a PCI function as claimed by a driver (call on successful
// bring-up, not merely on a vendor:device ID match - a device can be FOUND
// but fail to initialize). driver_name is copied (truncated to 15 chars).
void pci_mark_claimed(pci_device_t *dev, const char *driver_name);

// Read/write PCI configuration space
uint8_t  pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value);
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

// Find device by vendor/device ID
pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id);

// Find device by class/subclass
pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass);

// Find device by vendor ID only (returns first match)
pci_device_t *pci_find_vendor(uint16_t vendor_id);

// Find device by vendor ID and class
pci_device_t *pci_find_vendor_class(uint16_t vendor_id, uint8_t class_code, uint8_t subclass);

// Find next device matching class (start_idx=-1 for first, returns -1 when done)
int pci_find_next_class(uint8_t class_code, uint8_t subclass, int start_idx);

// Enable bus mastering for a device
void pci_enable_bus_master(pci_device_t *dev);

// #71: walk the PCI capabilities list looking for cap_id (e.g. PCI_CAP_ID_MSI).
// Returns the config-space byte offset of the capability header, or 0 if the
// device has no capability list or doesn't implement that capability.
uint8_t pci_find_capability(pci_device_t *dev, uint8_t cap_id);

// #71: enable MSI (Message Signaled Interrupts) on a device, targeting a single
// vector on the given destination Local APIC ID (physical destination mode,
// fixed delivery, edge-triggered -- the simplest, most broadly-compatible MSI
// configuration). This is the reliable interrupt path on hardware where the
// legacy PCI_INTERRUPT_LINE was never programmed by firmware (interrupt_line
// reads 0, meaning "no legacy routing"), which MSI does not need since it
// targets the Local APIC directly instead of going through the 8259 PIC /
// I/O APIC pin routing. Returns 1 if the device has an MSI capability and it
// was programmed + enabled, 0 if the device has no MSI capability at all (the
// caller should keep using whatever legacy IRQ it had, or none).
int pci_enable_msi(pci_device_t *dev, uint8_t vector, uint32_t dest_apic_id);

// Get BAR address (returns physical address)
uint64_t pci_get_bar_address(pci_device_t *dev, int bar_num);

// Get BAR size
uint32_t pci_get_bar_size(pci_device_t *dev, int bar_num);

// Print all PCI devices
void pci_print_devices(void);

// Get total number of discovered PCI devices
int pci_get_device_count(void);

// Get device by index (0 to count-1), returns NULL if out of range
pci_device_t *pci_get_device(int index);

// Get human-readable class name for a device
const char *pci_get_class_name(uint8_t class_code, uint8_t subclass);

// Check if a device is Intel
static inline bool pci_is_intel(pci_device_t *dev) {
    return dev && dev->vendor_id == PCI_VENDOR_INTEL;
}

// Check if a device is VGA class
static inline bool pci_is_vga(pci_device_t *dev) {
    return dev && dev->class_code == PCI_CLASS_DISPLAY && dev->subclass == PCI_SUBCLASS_VGA;
}

// Check if a device is HD Audio class
static inline bool pci_is_hda(pci_device_t *dev) {
    return dev && dev->class_code == PCI_CLASS_MULTIMEDIA && dev->subclass == PCI_SUBCLASS_HDA;
}

#endif // PCI_H
