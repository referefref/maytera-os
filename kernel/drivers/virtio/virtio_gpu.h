// virtio_gpu.h - VirtIO GPU Driver
// MayteraOS Production VirtIO Graphics

#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include "virtio_core.h"

// ============================================================================
// VirtIO GPU Feature Bits
// ============================================================================

#define VIRTIO_GPU_F_VIRGL           (1ULL << 0)   // Virgl 3D support
#define VIRTIO_GPU_F_EDID            (1ULL << 1)   // EDID support
#define VIRTIO_GPU_F_RESOURCE_UUID   (1ULL << 2)   // Resource UUID
#define VIRTIO_GPU_F_RESOURCE_BLOB   (1ULL << 3)   // Blob resources
#define VIRTIO_GPU_F_CONTEXT_INIT    (1ULL << 4)   // Context initialization

// ============================================================================
// VirtIO GPU Configuration
// ============================================================================

typedef struct virtio_gpu_config {
    uint32_t events_read;       // Events pending (read)
    uint32_t events_clear;      // Events to clear (write)
    uint32_t num_scanouts;      // Number of scanouts
    uint32_t num_capsets;       // Number of capability sets
} __attribute__((packed)) virtio_gpu_config_t;

// ============================================================================
// VirtIO GPU Command Types
// ============================================================================

// 2D commands
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING  0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO          0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET               0x0109
#define VIRTIO_GPU_CMD_GET_EDID                 0x010A

// Cursor commands
#define VIRTIO_GPU_CMD_UPDATE_CURSOR            0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR              0x0301

// Response types
#define VIRTIO_GPU_RESP_OK_NODATA               0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO         0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO          0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET               0x1103
#define VIRTIO_GPU_RESP_OK_EDID                 0x1104

// Error responses
#define VIRTIO_GPU_RESP_ERR_UNSPEC              0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY       0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID  0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID  0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER   0x1205

// ============================================================================
// VirtIO GPU Formats
// ============================================================================

#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM    1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM    2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM    3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM    4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM    67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM    68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM    121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM    134

// ============================================================================
// VirtIO GPU Structures
// ============================================================================

// Control message header
typedef struct virtio_gpu_ctrl_hdr {
    uint32_t type;          // Command type
    uint32_t flags;         // Flags
    uint64_t fence_id;      // Fence ID (for synchronization)
    uint32_t ctx_id;        // Context ID
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_ctrl_hdr_t;

// Rectangle
typedef struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_rect_t;

// Display info for one scanout
typedef struct virtio_gpu_display_one {
    virtio_gpu_rect_t r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed)) virtio_gpu_display_one_t;

// Response for GET_DISPLAY_INFO
#define VIRTIO_GPU_MAX_SCANOUTS 16
typedef struct virtio_gpu_resp_display_info {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_display_one_t pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed)) virtio_gpu_resp_display_info_t;

// Create 2D resource
typedef struct virtio_gpu_resource_create_2d {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_resource_create_2d_t;

// Unref resource
typedef struct virtio_gpu_resource_unref {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_unref_t;

// Set scanout
typedef struct virtio_gpu_set_scanout {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed)) virtio_gpu_set_scanout_t;

// Flush resource
typedef struct virtio_gpu_resource_flush {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_flush_t;

// Transfer to host 2D
typedef struct virtio_gpu_transfer_to_host_2d {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_transfer_to_host_2d_t;

// Memory entry for backing store
typedef struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_mem_entry_t;

// Attach backing
typedef struct virtio_gpu_resource_attach_backing {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    // Followed by virtio_gpu_mem_entry_t entries[]
} __attribute__((packed)) virtio_gpu_resource_attach_backing_t;

// Detach backing
typedef struct virtio_gpu_resource_detach_backing {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_detach_backing_t;

// Cursor update
typedef struct virtio_gpu_cursor_pos {
    uint32_t scanout_id;
    uint32_t x;
    uint32_t y;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_cursor_pos_t;

typedef struct virtio_gpu_update_cursor {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_cursor_pos_t pos;
    uint32_t resource_id;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_update_cursor_t;

// ============================================================================
// VirtIO GPU Device
// ============================================================================

typedef struct virtio_gpu_resource {
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    void *backing;
    size_t backing_size;
    bool attached;
} virtio_gpu_resource_t;

typedef struct virtio_gpu_device {
    virtio_device_t *virtio_dev;
    
    // Configuration
    uint32_t num_scanouts;
    uint32_t num_capsets;
    
    // Queues
    virtqueue_t *ctrl_queue;
    virtqueue_t *cursor_queue;
    
    // Display info
    virtio_gpu_display_one_t displays[VIRTIO_GPU_MAX_SCANOUTS];
    
    // Resource management
    uint32_t next_resource_id;
    virtio_gpu_resource_t *primary_resource;
    
    // Framebuffer
    void *framebuffer;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_stride;
    uint32_t fb_format;
    
    // Command buffers (DMA safe)
    void *cmd_buffer;
    void *resp_buffer;
    
    // Statistics
    uint64_t frames_rendered;
} virtio_gpu_device_t;

// ============================================================================
// API Functions
// ============================================================================

// Initialize VirtIO GPU subsystem
int virtio_gpu_init(void);

// Get GPU device
virtio_gpu_device_t *virtio_gpu_get_device(int index);

// Get display info
int virtio_gpu_get_display_info(virtio_gpu_device_t *dev);

// Create 2D resource
int virtio_gpu_resource_create_2d(virtio_gpu_device_t *dev,
                                   uint32_t resource_id,
                                   uint32_t format,
                                   uint32_t width, uint32_t height);

// Attach backing store to resource
int virtio_gpu_resource_attach_backing(virtio_gpu_device_t *dev,
                                        uint32_t resource_id,
                                        void *backing, size_t size);

// Set scanout (associate resource with display)
int virtio_gpu_set_scanout(virtio_gpu_device_t *dev,
                           uint32_t scanout_id,
                           uint32_t resource_id,
                           uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height);

// Transfer data to host
int virtio_gpu_transfer_to_host_2d(virtio_gpu_device_t *dev,
                                    uint32_t resource_id,
                                    uint32_t x, uint32_t y,
                                    uint32_t width, uint32_t height);

// Flush resource to display
int virtio_gpu_resource_flush(virtio_gpu_device_t *dev,
                               uint32_t resource_id,
                               uint32_t x, uint32_t y,
                               uint32_t width, uint32_t height);

// Set up primary framebuffer (convenience function)
int virtio_gpu_setup_framebuffer(virtio_gpu_device_t *dev,
                                  uint32_t width, uint32_t height);

// Get framebuffer pointer
void *virtio_gpu_get_framebuffer(virtio_gpu_device_t *dev);
uint32_t virtio_gpu_get_fb_width(virtio_gpu_device_t *dev);
uint32_t virtio_gpu_get_fb_height(virtio_gpu_device_t *dev);
uint32_t virtio_gpu_get_fb_stride(virtio_gpu_device_t *dev);

// Update display (transfer and flush)
int virtio_gpu_update_display(virtio_gpu_device_t *dev);

// Update cursor
int virtio_gpu_update_cursor(virtio_gpu_device_t *dev,
                              uint32_t resource_id,
                              uint32_t x, uint32_t y,
                              uint32_t hot_x, uint32_t hot_y);

// Move cursor
int virtio_gpu_move_cursor(virtio_gpu_device_t *dev,
                           uint32_t x, uint32_t y);

// Poll for completion
void virtio_gpu_poll(virtio_gpu_device_t *dev);

#endif // VIRTIO_GPU_H
