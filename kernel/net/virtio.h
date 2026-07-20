// virtio.h - VirtIO common definitions
#ifndef VIRTIO_H
#define VIRTIO_H

#include "../types.h"

// VirtIO PCI vendor ID
#define VIRTIO_PCI_VENDOR_ID    0x1AF4

// VirtIO device types (transitional device IDs: 0x1000-0x103F)
#define VIRTIO_DEV_NET          0x1000  // Network device
#define VIRTIO_DEV_BLOCK        0x1001  // Block device
#define VIRTIO_DEV_CONSOLE      0x1003  // Console
#define VIRTIO_DEV_ENTROPY      0x1005  // Entropy source
#define VIRTIO_DEV_BALLOON      0x1002  // Memory balloon

// Modern device IDs (1.0+): device type + 0x1040
#define VIRTIO_DEV_NET_MODERN   0x1041

// VirtIO PCI capability types
#define VIRTIO_PCI_CAP_COMMON   1   // Common configuration
#define VIRTIO_PCI_CAP_NOTIFY   2   // Notifications
#define VIRTIO_PCI_CAP_ISR      3   // ISR status
#define VIRTIO_PCI_CAP_DEVICE   4   // Device specific config
#define VIRTIO_PCI_CAP_PCI      5   // PCI configuration access

// Legacy (transitional) register offsets from BAR0
#define VIRTIO_PCI_HOST_FEATURES    0x00  // R: Features offered by device
#define VIRTIO_PCI_GUEST_FEATURES   0x04  // RW: Features activated by driver
#define VIRTIO_PCI_QUEUE_PFN        0x08  // RW: Queue physical address
#define VIRTIO_PCI_QUEUE_SIZE       0x0C  // R: Queue size (number of entries)
#define VIRTIO_PCI_QUEUE_SELECT     0x0E  // RW: Queue selector
#define VIRTIO_PCI_QUEUE_NOTIFY     0x10  // W: Notification (queue index)
#define VIRTIO_PCI_DEVICE_STATUS    0x12  // RW: Device status
#define VIRTIO_PCI_ISR_STATUS       0x13  // R: ISR status
#define VIRTIO_PCI_CONFIG           0x14  // Device-specific config starts here

// Device status bits
#define VIRTIO_STATUS_ACKNOWLEDGE   0x01  // Guest OS has found the device
#define VIRTIO_STATUS_DRIVER        0x02  // Guest OS knows how to drive the device
#define VIRTIO_STATUS_DRIVER_OK     0x04  // Driver is set up and ready to drive
#define VIRTIO_STATUS_FEATURES_OK   0x08  // Driver has acknowledged all features
#define VIRTIO_STATUS_FAILED        0x80  // Something went wrong

// Feature bits common to all devices
#define VIRTIO_F_NOTIFY_ON_EMPTY    (1 << 24)
#define VIRTIO_F_ANY_LAYOUT         (1 << 27)
#define VIRTIO_F_RING_INDIRECT_DESC (1 << 28)
#define VIRTIO_F_RING_EVENT_IDX     (1 << 29)

// Virtqueue descriptor flags
#define VIRTQ_DESC_F_NEXT       1   // Descriptor continues via next field
#define VIRTQ_DESC_F_WRITE      2   // Buffer is write-only (for device)
#define VIRTQ_DESC_F_INDIRECT   4   // Buffer contains indirect descriptors

// Virtqueue available ring flags
#define VIRTQ_AVAIL_F_NO_INTERRUPT  1

// Virtqueue used ring flags
#define VIRTQ_USED_F_NO_NOTIFY      1

// Virtqueue descriptor
typedef struct {
    uint64_t addr;      // Physical address of buffer
    uint32_t len;       // Length of buffer
    uint16_t flags;     // Flags
    uint16_t next;      // Next descriptor index (if VIRTQ_DESC_F_NEXT set)
} __attribute__((packed)) virtq_desc_t;

// Virtqueue available ring
typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];    // Available descriptor indices (variable length)
} __attribute__((packed)) virtq_avail_t;

// Virtqueue used ring element
typedef struct {
    uint32_t id;        // Index of start of descriptor chain
    uint32_t len;       // Total bytes written to descriptor buffers
} __attribute__((packed)) virtq_used_elem_t;

// Virtqueue used ring
typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];   // Used descriptor elements (variable length)
} __attribute__((packed)) virtq_used_t;

// Complete virtqueue structure (allocated by driver)
typedef struct {
    uint32_t num;           // Queue size
    uint32_t notify_offset; // Notification offset

    virtq_desc_t  *desc;    // Descriptor table
    virtq_avail_t *avail;   // Available ring
    virtq_used_t  *used;    // Used ring

    uint16_t free_head;     // Head of free descriptor list
    uint16_t num_free;      // Number of free descriptors
    uint16_t last_used_idx; // Last used index we've seen

    void *buffer;           // Allocated buffer for the entire queue
} virtqueue_t;

// Calculate required size for virtqueue structures
static inline uint32_t virtq_size(uint32_t num) {
    // Descriptor table: num * 16 bytes
    // Available ring: 4 + num * 2 bytes (align to 2)
    // Used ring: 4 + num * 8 bytes (align to 4096)

    uint32_t desc_size = num * sizeof(virtq_desc_t);
    uint32_t avail_size = 4 + num * 2;
    uint32_t used_size = 4 + num * sizeof(virtq_used_elem_t);

    // Align used ring to 4096 boundary
    uint32_t used_offset = ((desc_size + avail_size) + 4095) & ~4095;

    return used_offset + used_size;
}

#endif // VIRTIO_H
