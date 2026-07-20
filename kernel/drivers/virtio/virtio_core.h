// virtio_core.h - VirtIO Core Framework
// MayteraOS Production VirtIO Driver Infrastructure
//
// This provides a unified abstraction for VirtIO devices supporting:
// - Legacy (transitional) PCI transport via I/O ports
// - Modern (1.0+) PCI transport via MMIO
// - Packed virtqueues (1.1+)
// - Split virtqueues (legacy)

#ifndef VIRTIO_CORE_H
#define VIRTIO_CORE_H

#include "../../types.h"

// ============================================================================
// VirtIO PCI Definitions
// ============================================================================

// VirtIO PCI vendor ID
#define VIRTIO_PCI_VENDOR_ID        0x1AF4

// Device IDs: Transitional (legacy compatible)
#define VIRTIO_DEV_NET_TRANS        0x1000  // Network card
#define VIRTIO_DEV_BLOCK_TRANS      0x1001  // Block device
#define VIRTIO_DEV_BALLOON_TRANS    0x1002  // Memory balloon
#define VIRTIO_DEV_CONSOLE_TRANS    0x1003  // Console
#define VIRTIO_DEV_ENTROPY_TRANS    0x1005  // Entropy source
#define VIRTIO_DEV_SCSI_TRANS       0x1004  // SCSI host
#define VIRTIO_DEV_9P_TRANS         0x1009  // 9P transport
#define VIRTIO_DEV_INPUT_TRANS      0x1052  // Input device

// Device IDs: Modern (1.0+) = type + 0x1040
#define VIRTIO_DEV_NET_MODERN       0x1041  // Network card
#define VIRTIO_DEV_BLOCK_MODERN     0x1042  // Block device
#define VIRTIO_DEV_CONSOLE_MODERN   0x1043  // Console
#define VIRTIO_DEV_ENTROPY_MODERN   0x1044  // Entropy source
#define VIRTIO_DEV_BALLOON_MODERN   0x1045  // Memory balloon
#define VIRTIO_DEV_SCSI_MODERN      0x1048  // SCSI host
#define VIRTIO_DEV_GPU_MODERN       0x1050  // GPU device
#define VIRTIO_DEV_INPUT_MODERN     0x1052  // Input device

// Device type enumeration
typedef enum {
    VIRTIO_TYPE_INVALID = 0,
    VIRTIO_TYPE_NET = 1,
    VIRTIO_TYPE_BLOCK = 2,
    VIRTIO_TYPE_CONSOLE = 3,
    VIRTIO_TYPE_ENTROPY = 4,
    VIRTIO_TYPE_BALLOON = 5,
    VIRTIO_TYPE_SCSI = 8,
    VIRTIO_TYPE_GPU = 16,
    VIRTIO_TYPE_INPUT = 18,
    VIRTIO_TYPE_9P = 9,
} virtio_device_type_t;

// ============================================================================
// Device Status Bits
// ============================================================================

#define VIRTIO_STATUS_RESET         0x00
#define VIRTIO_STATUS_ACKNOWLEDGE   0x01  // Guest OS has found device
#define VIRTIO_STATUS_DRIVER        0x02  // Guest OS knows driver
#define VIRTIO_STATUS_DRIVER_OK     0x04  // Driver ready
#define VIRTIO_STATUS_FEATURES_OK   0x08  // Features negotiated (1.0+)
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 0x40
#define VIRTIO_STATUS_FAILED        0x80  // Fatal error

// ============================================================================
// Common Feature Bits
// ============================================================================

// Bits 0-23: Device-specific features
// Bits 24-37: Reserved
// Bits 38+: VirtIO transport features

#define VIRTIO_F_RING_INDIRECT_DESC (1ULL << 28)  // Indirect descriptors
#define VIRTIO_F_RING_EVENT_IDX     (1ULL << 29)  // Event index notifications
#define VIRTIO_F_VERSION_1          (1ULL << 32)  // VirtIO 1.0 compliant
#define VIRTIO_F_ACCESS_PLATFORM    (1ULL << 33)  // Needs platform DMA access
#define VIRTIO_F_RING_PACKED        (1ULL << 34)  // Packed virtqueue (1.1+)
#define VIRTIO_F_IN_ORDER           (1ULL << 35)  // Buffers used in order
#define VIRTIO_F_ORDER_PLATFORM     (1ULL << 36)  // Platform ordering
#define VIRTIO_F_SR_IOV             (1ULL << 37)  // SR-IOV VF support
#define VIRTIO_F_NOTIFICATION_DATA  (1ULL << 38)  // Extra notification data

// ============================================================================
// Legacy PCI Register Offsets (from BAR0, I/O port based)
// ============================================================================

#define VIRTIO_PCI_LEGACY_HOST_FEATURES     0x00  // R: Device features
#define VIRTIO_PCI_LEGACY_GUEST_FEATURES    0x04  // RW: Driver features
#define VIRTIO_PCI_LEGACY_QUEUE_PFN         0x08  // RW: Queue PFN
#define VIRTIO_PCI_LEGACY_QUEUE_SIZE        0x0C  // R: Queue size (16-bit)
#define VIRTIO_PCI_LEGACY_QUEUE_SELECT      0x0E  // RW: Queue select (16-bit)
#define VIRTIO_PCI_LEGACY_QUEUE_NOTIFY      0x10  // W: Queue notify (16-bit)
#define VIRTIO_PCI_LEGACY_DEVICE_STATUS     0x12  // RW: Device status (8-bit)
#define VIRTIO_PCI_LEGACY_ISR_STATUS        0x13  // R: ISR status (8-bit)
#define VIRTIO_PCI_LEGACY_CONFIG            0x14  // Device-specific config

// With MSI-X, offsets shift by 4 bytes
#define VIRTIO_PCI_LEGACY_CONFIG_MSIX       0x18

// ============================================================================
// Modern PCI Capability Types
// ============================================================================

#define VIRTIO_PCI_CAP_COMMON_CFG   1   // Common configuration
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2   // Notifications
#define VIRTIO_PCI_CAP_ISR_CFG      3   // ISR status
#define VIRTIO_PCI_CAP_DEVICE_CFG   4   // Device configuration
#define VIRTIO_PCI_CAP_PCI_CFG      5   // PCI configuration access
#define VIRTIO_PCI_CAP_SHARED_MEM   8   // Shared memory

// ============================================================================
// Modern Common Configuration Structure (MMIO mapped)
// ============================================================================

typedef struct virtio_pci_common_cfg {
    // Device/driver features
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    
    // MSIX configuration
    uint16_t config_msix_vector;
    uint16_t num_queues;
    
    // Device status
    uint8_t device_status;
    uint8_t config_generation;
    
    // Queue configuration
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} __attribute__((packed)) virtio_pci_common_cfg_t;

// ============================================================================
// Virtqueue Descriptor Flags
// ============================================================================

#define VIRTQ_DESC_F_NEXT       (1 << 0)  // Buffer continues via next field
#define VIRTQ_DESC_F_WRITE      (1 << 1)  // Write-only for device
#define VIRTQ_DESC_F_INDIRECT   (1 << 2)  // Buffer contains indirect descs

// Available ring flags
#define VIRTQ_AVAIL_F_NO_INTERRUPT  (1 << 0)

// Used ring flags  
#define VIRTQ_USED_F_NO_NOTIFY      (1 << 0)

// ============================================================================
// Split Virtqueue Structures (Legacy)
// ============================================================================

// Descriptor in split virtqueue
typedef struct virtq_desc {
    uint64_t addr;      // Physical address of buffer
    uint32_t len;       // Length of buffer
    uint16_t flags;     // Descriptor flags
    uint16_t next;      // Next descriptor index (if F_NEXT set)
} __attribute__((packed)) virtq_desc_t;

// Available ring header
typedef struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];    // Variable length array of descriptor indices
    // Followed by used_event (if VIRTIO_F_EVENT_IDX negotiated)
} __attribute__((packed)) virtq_avail_t;

// Used ring element
typedef struct virtq_used_elem {
    uint32_t id;        // Index of start of used descriptor chain
    uint32_t len;       // Total bytes written
} __attribute__((packed)) virtq_used_elem_t;

// Used ring header
typedef struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];  // Variable length array
    // Followed by avail_event (if VIRTIO_F_EVENT_IDX negotiated)
} __attribute__((packed)) virtq_used_t;

// ============================================================================
// Packed Virtqueue Structures (VirtIO 1.1+)
// ============================================================================

// Packed descriptor flags
#define VIRTQ_PACKED_DESC_F_NEXT        (1 << 0)
#define VIRTQ_PACKED_DESC_F_WRITE       (1 << 1)
#define VIRTQ_PACKED_DESC_F_INDIRECT    (1 << 2)
#define VIRTQ_PACKED_DESC_F_AVAIL       (1 << 7)
#define VIRTQ_PACKED_DESC_F_USED        (1 << 15)

typedef struct virtq_packed_desc {
    uint64_t addr;      // Buffer physical address
    uint32_t len;       // Buffer length
    uint16_t id;        // Buffer ID
    uint16_t flags;     // Flags including avail/used wrap counters
} __attribute__((packed)) virtq_packed_desc_t;

// Event suppression structure for packed queues
typedef struct virtq_packed_event {
    uint16_t desc;      // Descriptor ring index
    uint16_t flags;     // Event flags
} __attribute__((packed)) virtq_packed_event_t;

// ============================================================================
// Virtqueue Structure (Driver-side)
// ============================================================================

typedef struct virtqueue {
    // Queue identity
    uint16_t queue_index;
    uint16_t num_entries;
    
    // Split queue structures (legacy)
    virtq_desc_t  *desc;
    virtq_avail_t *avail;
    virtq_used_t  *used;
    
    // Packed queue structures (modern)
    virtq_packed_desc_t *packed_desc;
    virtq_packed_event_t *driver_event;
    virtq_packed_event_t *device_event;
    bool packed;
    bool wrap_counter;
    
    // Descriptor management
    uint16_t free_head;         // Head of free descriptor list
    uint16_t num_free;          // Number of free descriptors
    uint16_t last_used_idx;     // Last processed used index
    uint16_t avail_idx;         // Next available slot
    
    // For packed queues
    uint16_t packed_free_head;
    uint16_t packed_last_used;
    
    // Physical addresses for DMA
    uint64_t desc_phys;
    uint64_t avail_phys;
    uint64_t used_phys;
    
    // Callback for buffer completion
    void (*callback)(struct virtqueue *vq, void *data, uint32_t len);
    void *callback_data;
    
    // Token storage for tracking in-flight buffers
    void **tokens;
    
    // Notification offset (modern only)
    uint32_t notify_offset;
    
    // Raw buffer for queue memory
    void *buffer;
    size_t buffer_size;
} virtqueue_t;

// ============================================================================
// Transport Abstraction
// ============================================================================

typedef enum {
    VIRTIO_TRANSPORT_LEGACY_PCI,    // Legacy I/O port based
    VIRTIO_TRANSPORT_MODERN_PCI,    // Modern MMIO based
} virtio_transport_type_t;

typedef struct virtio_device virtio_device_t;

// Transport operations
typedef struct virtio_transport_ops {
    // Read/write device status
    uint8_t (*get_status)(virtio_device_t *dev);
    void (*set_status)(virtio_device_t *dev, uint8_t status);
    
    // Feature negotiation
    uint64_t (*get_features)(virtio_device_t *dev);
    void (*set_features)(virtio_device_t *dev, uint64_t features);
    
    // Queue operations
    uint16_t (*get_queue_size)(virtio_device_t *dev, uint16_t queue_idx);
    void (*set_queue_size)(virtio_device_t *dev, uint16_t queue_idx, uint16_t size);
    void (*select_queue)(virtio_device_t *dev, uint16_t queue_idx);
    void (*activate_queue)(virtio_device_t *dev, uint16_t queue_idx, virtqueue_t *vq);
    void (*notify_queue)(virtio_device_t *dev, uint16_t queue_idx);
    
    // Configuration access
    void (*read_config)(virtio_device_t *dev, uint32_t offset, void *buf, size_t len);
    void (*write_config)(virtio_device_t *dev, uint32_t offset, const void *buf, size_t len);
    
    // ISR handling
    uint8_t (*read_isr)(virtio_device_t *dev);
    
    // Generation (modern only)
    uint8_t (*get_config_generation)(virtio_device_t *dev);
} virtio_transport_ops_t;

// ============================================================================
// VirtIO Device Structure
// ============================================================================

struct virtio_device {
    // Device identity
    virtio_device_type_t type;
    uint16_t pci_device_id;
    uint8_t pci_bus;
    uint8_t pci_slot;
    uint8_t pci_func;
    
    // Transport
    virtio_transport_type_t transport_type;
    const virtio_transport_ops_t *transport;
    
    // Legacy transport state
    uint16_t io_base;
    
    // Modern transport state
    volatile virtio_pci_common_cfg_t *common_cfg;
    volatile uint8_t *isr;
    volatile void *device_cfg;
    volatile uint16_t *notify;
    uint32_t notify_multiplier;
    
    // Queues
    virtqueue_t **queues;
    uint16_t num_queues;
    
    // Features
    uint64_t device_features;
    uint64_t driver_features;
    
    // Driver-specific data
    void *driver_data;
    
    // IRQ
    uint8_t irq_line;
    
    // Status
    bool initialized;
    bool failed;
};

// ============================================================================
// Core API Functions
// ============================================================================

// Device discovery and initialization
int virtio_init(void);
virtio_device_t *virtio_find_device(virtio_device_type_t type);
virtio_device_t *virtio_find_device_by_pci(uint8_t bus, uint8_t slot, uint8_t func);

// Device lifecycle
int virtio_device_init(virtio_device_t *dev);
void virtio_device_reset(virtio_device_t *dev);
int virtio_device_finalize_features(virtio_device_t *dev);
void virtio_device_driver_ok(virtio_device_t *dev);
void virtio_device_failed(virtio_device_t *dev);

// Feature negotiation
uint64_t virtio_get_features(virtio_device_t *dev);
bool virtio_has_feature(virtio_device_t *dev, uint64_t feature);
void virtio_set_feature(virtio_device_t *dev, uint64_t feature);
void virtio_clear_feature(virtio_device_t *dev, uint64_t feature);

// Virtqueue management
virtqueue_t *virtio_create_queue(virtio_device_t *dev, uint16_t queue_idx, 
                                  void (*callback)(virtqueue_t*, void*, uint32_t));
void virtio_destroy_queue(virtqueue_t *vq);
int virtio_enable_queue(virtio_device_t *dev, uint16_t queue_idx);

// Buffer operations (split virtqueue)
int virtq_add_buffer(virtqueue_t *vq, 
                     void *bufs[], uint32_t lens[], 
                     int out_num, int in_num,
                     void *token);
int virtq_add_single_buffer(virtqueue_t *vq, void *buf, uint32_t len, 
                            bool device_write, void *token);
void *virtq_get_buffer(virtqueue_t *vq, uint32_t *len);
void virtq_kick(virtqueue_t *vq, virtio_device_t *dev);

// Packed virtqueue operations
int virtq_packed_add_buffer(virtqueue_t *vq,
                            void *bufs[], uint32_t lens[],
                            int out_num, int in_num,
                            void *token);
void *virtq_packed_get_buffer(virtqueue_t *vq, uint32_t *len);

// Interrupt handling
void virtio_irq_handler(virtio_device_t *dev);
bool virtq_has_used(virtqueue_t *vq);

// Configuration access helpers
uint8_t virtio_read_config8(virtio_device_t *dev, uint32_t offset);
uint16_t virtio_read_config16(virtio_device_t *dev, uint32_t offset);
uint32_t virtio_read_config32(virtio_device_t *dev, uint32_t offset);
uint64_t virtio_read_config64(virtio_device_t *dev, uint32_t offset);
void virtio_write_config8(virtio_device_t *dev, uint32_t offset, uint8_t val);
void virtio_write_config16(virtio_device_t *dev, uint32_t offset, uint16_t val);
void virtio_write_config32(virtio_device_t *dev, uint32_t offset, uint32_t val);
void virtio_write_config64(virtio_device_t *dev, uint32_t offset, uint64_t val);

// Memory barriers
static inline void virtio_mb(void) {
    __asm__ volatile("mfence" ::: "memory");
}

static inline void virtio_rmb(void) {
    __asm__ volatile("lfence" ::: "memory");
}

static inline void virtio_wmb(void) {
    __asm__ volatile("sfence" ::: "memory");
}

// Utility functions
uint32_t virtq_size_bytes(uint16_t num_entries);
const char *virtio_type_name(virtio_device_type_t type);

#endif // VIRTIO_CORE_H
