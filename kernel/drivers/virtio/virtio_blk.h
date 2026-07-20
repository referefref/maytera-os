// virtio_blk.h - VirtIO Block Device Driver
// MayteraOS Production VirtIO Block Storage

#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include "virtio_core.h"

// ============================================================================
// VirtIO Block Feature Bits
// ============================================================================

#define VIRTIO_BLK_F_SIZE_MAX       (1ULL << 1)   // Max segment size
#define VIRTIO_BLK_F_SEG_MAX        (1ULL << 2)   // Max segments per request
#define VIRTIO_BLK_F_GEOMETRY       (1ULL << 4)   // Disk geometry available
#define VIRTIO_BLK_F_RO             (1ULL << 5)   // Device is read-only
#define VIRTIO_BLK_F_BLK_SIZE       (1ULL << 6)   // Block size available
#define VIRTIO_BLK_F_FLUSH          (1ULL << 9)   // Flush command support
#define VIRTIO_BLK_F_TOPOLOGY       (1ULL << 10)  // Topology information
#define VIRTIO_BLK_F_CONFIG_WCE     (1ULL << 11)  // Writeback config
#define VIRTIO_BLK_F_MQ             (1ULL << 12)  // Multi-queue support
#define VIRTIO_BLK_F_DISCARD        (1ULL << 13)  // Discard support
#define VIRTIO_BLK_F_WRITE_ZEROES   (1ULL << 14)  // Write zeroes support

// ============================================================================
// VirtIO Block Configuration
// ============================================================================

typedef struct virtio_blk_config {
    uint64_t capacity;          // Capacity in 512-byte sectors
    uint32_t size_max;          // Max segment size (if F_SIZE_MAX)
    uint32_t seg_max;           // Max segments per request (if F_SEG_MAX)
    
    struct {
        uint16_t cylinders;
        uint8_t  heads;
        uint8_t  sectors;
    } geometry;                 // Geometry (if F_GEOMETRY)
    
    uint32_t blk_size;          // Block size (if F_BLK_SIZE)
    
    struct {
        uint8_t  physical_block_exp;
        uint8_t  alignment_offset;
        uint16_t min_io_size;
        uint32_t opt_io_size;
    } topology;                 // Topology (if F_TOPOLOGY)
    
    uint8_t writeback;          // Writeback mode (if F_CONFIG_WCE)
    uint8_t unused0;
    uint16_t num_queues;        // Number of queues (if F_MQ)
    
    uint32_t max_discard_sectors;
    uint32_t max_discard_seg;
    uint32_t discard_sector_alignment;
    
    uint32_t max_write_zeroes_sectors;
    uint32_t max_write_zeroes_seg;
    uint8_t write_zeroes_may_unmap;
    uint8_t unused1[3];
} __attribute__((packed)) virtio_blk_config_t;

// ============================================================================
// VirtIO Block Request Types
// ============================================================================

#define VIRTIO_BLK_T_IN             0   // Read
#define VIRTIO_BLK_T_OUT            1   // Write
#define VIRTIO_BLK_T_FLUSH          4   // Flush
#define VIRTIO_BLK_T_GET_ID         8   // Get device ID
#define VIRTIO_BLK_T_DISCARD        11  // Discard
#define VIRTIO_BLK_T_WRITE_ZEROES   13  // Write zeroes

// Status values
#define VIRTIO_BLK_S_OK             0   // Success
#define VIRTIO_BLK_S_IOERR          1   // I/O error
#define VIRTIO_BLK_S_UNSUPP         2   // Unsupported request

// ============================================================================
// VirtIO Block Request Header
// ============================================================================

typedef struct virtio_blk_req_header {
    uint32_t type;      // Request type
    uint32_t reserved;  // Reserved
    uint64_t sector;    // Starting sector (512-byte units)
} __attribute__((packed)) virtio_blk_req_header_t;

// ============================================================================
// VirtIO Block Device Structure
// ============================================================================

typedef struct virtio_blk_device {
    virtio_device_t *virtio_dev;
    
    // Configuration
    uint64_t capacity;          // Total sectors
    uint32_t block_size;        // Logical block size
    uint32_t max_segment_size;
    uint32_t max_segments;
    bool read_only;
    bool flush_supported;
    bool discard_supported;
    
    // Request queue
    virtqueue_t *request_queue;
    
    // Multi-queue support
    virtqueue_t **queues;
    uint16_t num_queues;
    
    // Statistics
    uint64_t reads_completed;
    uint64_t writes_completed;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t errors;
} virtio_blk_device_t;

// ============================================================================
// Request Tracking
// ============================================================================

typedef struct virtio_blk_request {
    virtio_blk_req_header_t header;
    uint8_t status;
    
    // User callback
    void (*callback)(struct virtio_blk_request *req, int status, void *data);
    void *callback_data;
    
    // Buffer info
    void *buffer;
    uint32_t buffer_size;
    
    // For synchronous operations
    volatile bool completed;
} virtio_blk_request_t;

// ============================================================================
// API Functions
// ============================================================================

// Initialize VirtIO block subsystem
int virtio_blk_init(void);

// Get block device (by index, 0 = first)
virtio_blk_device_t *virtio_blk_get_device(int index);

// Synchronous read/write operations
int virtio_blk_read(virtio_blk_device_t *dev, 
                    uint64_t sector, uint32_t count, void *buffer);
int virtio_blk_write(virtio_blk_device_t *dev,
                     uint64_t sector, uint32_t count, const void *buffer);

// Asynchronous read/write operations
int virtio_blk_read_async(virtio_blk_device_t *dev,
                          uint64_t sector, uint32_t count, void *buffer,
                          void (*callback)(virtio_blk_request_t*, int, void*),
                          void *callback_data);
int virtio_blk_write_async(virtio_blk_device_t *dev,
                           uint64_t sector, uint32_t count, const void *buffer,
                           void (*callback)(virtio_blk_request_t*, int, void*),
                           void *callback_data);

// Flush (sync data to disk)
int virtio_blk_flush(virtio_blk_device_t *dev);

// Get device ID string
int virtio_blk_get_id(virtio_blk_device_t *dev, char *buffer, size_t size);

// Device information
uint64_t virtio_blk_get_capacity(virtio_blk_device_t *dev);
uint32_t virtio_blk_get_block_size(virtio_blk_device_t *dev);
bool virtio_blk_is_read_only(virtio_blk_device_t *dev);

// Interrupt handler
void virtio_blk_irq_handler(virtio_blk_device_t *dev);

// Poll for completion (for polling mode)
void virtio_blk_poll(virtio_blk_device_t *dev);

#endif // VIRTIO_BLK_H
