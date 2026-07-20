// virtio_blk.c - VirtIO Block Device Driver Implementation
// MayteraOS Production VirtIO Block Storage

#include "virtio_blk.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../serial.h"
#include "../../string.h"

// ============================================================================
// Module State
// ============================================================================

#define MAX_BLK_DEVICES 8
static virtio_blk_device_t *blk_devices[MAX_BLK_DEVICES];
static int blk_device_count = 0;

// Request pool for async operations
#define REQUEST_POOL_SIZE 64
static virtio_blk_request_t *request_pool;
static bool request_pool_used[REQUEST_POOL_SIZE];

// ============================================================================
// Request Pool Management
// ============================================================================

static void init_request_pool(void) {
    // Allocate from physical memory for DMA safety
    uint64_t phys = pmm_alloc_pages((sizeof(virtio_blk_request_t) * REQUEST_POOL_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
    if (phys == 0) {
        kprintf("[VIRTIO-BLK] Failed to allocate request pool\n");
        return;
    }
    request_pool = (virtio_blk_request_t *)phys;
    memset(request_pool, 0, sizeof(virtio_blk_request_t) * REQUEST_POOL_SIZE);
    memset(request_pool_used, 0, sizeof(request_pool_used));
}

static virtio_blk_request_t *alloc_request(void) {
    for (int i = 0; i < REQUEST_POOL_SIZE; i++) {
        if (!request_pool_used[i]) {
            request_pool_used[i] = true;
            memset(&request_pool[i], 0, sizeof(virtio_blk_request_t));
            return &request_pool[i];
        }
    }
    return NULL;
}

static void free_request(virtio_blk_request_t *req) {
    if (!req || !request_pool) return;
    int idx = req - request_pool;
    if (idx >= 0 && idx < REQUEST_POOL_SIZE) {
        request_pool_used[idx] = false;
    }
}

// ============================================================================
// Completion Callback
// ============================================================================

static void blk_completion_callback(virtqueue_t *vq, void *token, uint32_t len) {
    (void)vq;
    (void)len;
    
    virtio_blk_request_t *req = (virtio_blk_request_t *)token;
    if (!req) return;
    
    // Mark as completed
    req->completed = true;
    
    // Call user callback if present
    if (req->callback) {
        int status = (req->status == VIRTIO_BLK_S_OK) ? 0 : -1;
        req->callback(req, status, req->callback_data);
    }
}

// ============================================================================
// Device Initialization
// ============================================================================

static int init_blk_device(virtio_device_t *vdev) {
    if (!vdev) return -1;
    
    virtio_blk_device_t *dev = kzalloc(sizeof(virtio_blk_device_t));
    if (!dev) {
        kprintf("[VIRTIO-BLK] Failed to allocate device structure\n");
        return -1;
    }
    
    dev->virtio_dev = vdev;
    vdev->driver_data = dev;
    
    // Initialize device
    if (virtio_device_init(vdev) != 0) {
        kfree(dev);
        return -1;
    }
    
    // Read device features and select what we support
    uint64_t features = virtio_get_features(vdev);
    kprintf("[VIRTIO-BLK] Device features: 0x%016llx\n", features);
    
    // Always accept these features if available
    if (features & VIRTIO_BLK_F_SIZE_MAX) {
        virtio_set_feature(vdev, VIRTIO_BLK_F_SIZE_MAX);
    }
    if (features & VIRTIO_BLK_F_SEG_MAX) {
        virtio_set_feature(vdev, VIRTIO_BLK_F_SEG_MAX);
    }
    if (features & VIRTIO_BLK_F_BLK_SIZE) {
        virtio_set_feature(vdev, VIRTIO_BLK_F_BLK_SIZE);
    }
    if (features & VIRTIO_BLK_F_FLUSH) {
        virtio_set_feature(vdev, VIRTIO_BLK_F_FLUSH);
        dev->flush_supported = true;
    }
    if (features & VIRTIO_BLK_F_RO) {
        dev->read_only = true;
    }
    
    // Finalize features
    if (virtio_device_finalize_features(vdev) != 0) {
        kfree(dev);
        return -1;
    }
    
    // Read configuration
    dev->capacity = virtio_read_config64(vdev, offsetof(virtio_blk_config_t, capacity));
    
    if (virtio_has_feature(vdev, VIRTIO_BLK_F_BLK_SIZE)) {
        dev->block_size = virtio_read_config32(vdev, offsetof(virtio_blk_config_t, blk_size));
    } else {
        dev->block_size = 512;  // Default
    }
    
    if (virtio_has_feature(vdev, VIRTIO_BLK_F_SIZE_MAX)) {
        dev->max_segment_size = virtio_read_config32(vdev, offsetof(virtio_blk_config_t, size_max));
    } else {
        dev->max_segment_size = PAGE_SIZE;
    }
    
    if (virtio_has_feature(vdev, VIRTIO_BLK_F_SEG_MAX)) {
        dev->max_segments = virtio_read_config32(vdev, offsetof(virtio_blk_config_t, seg_max));
    } else {
        dev->max_segments = 1;
    }
    
    kprintf("[VIRTIO-BLK] Capacity: %llu sectors (%llu MB)\n", 
            dev->capacity, (dev->capacity * 512) / (1024 * 1024));
    kprintf("[VIRTIO-BLK] Block size: %u, Max segment: %u, Max segments: %u\n",
            dev->block_size, dev->max_segment_size, dev->max_segments);
    if (dev->read_only) {
        kprintf("[VIRTIO-BLK] Device is READ-ONLY\n");
    }
    
    // Create request queue (queue 0)
    dev->request_queue = virtio_create_queue(vdev, 0, blk_completion_callback);
    if (!dev->request_queue) {
        kprintf("[VIRTIO-BLK] Failed to create request queue\n");
        kfree(dev);
        return -1;
    }
    
    dev->num_queues = 1;
    
    // Mark device as ready
    virtio_device_driver_ok(vdev);
    
    // Add to device list
    if (blk_device_count < MAX_BLK_DEVICES) {
        blk_devices[blk_device_count++] = dev;
    }
    
    kprintf("[VIRTIO-BLK] Device %d initialized successfully\n", blk_device_count - 1);
    return 0;
}

// ============================================================================
// Public API - Initialization
// ============================================================================

int virtio_blk_init(void) {
    kprintf("[VIRTIO-BLK] Initializing VirtIO block driver...\n");
    
    blk_device_count = 0;
    memset(blk_devices, 0, sizeof(blk_devices));
    
    // Initialize request pool
    init_request_pool();
    
    // Find all VirtIO block devices
    virtio_device_t *vdev;
    int found = 0;
    
    // Try to find block devices (may be multiple)
    while ((vdev = virtio_find_device(VIRTIO_TYPE_BLOCK)) != NULL) {
        // Check if already initialized
        if (vdev->initialized) {
            // Try next device - need to iterate through all
            // For now just break after first
            break;
        }
        
        if (init_blk_device(vdev) == 0) {
            found++;
        }
        break;  // Exit after first for now
    }
    
    kprintf("[VIRTIO-BLK] Found %d block device(s)\n", found);
    return found;
}

virtio_blk_device_t *virtio_blk_get_device(int index) {
    if (index < 0 || index >= blk_device_count) {
        return NULL;
    }
    return blk_devices[index];
}

// ============================================================================
// I/O Operations
// ============================================================================

static int submit_request(virtio_blk_device_t *dev, uint32_t type,
                          uint64_t sector, uint32_t count, void *buffer,
                          void (*callback)(virtio_blk_request_t*, int, void*),
                          void *callback_data) {
    if (!dev || !dev->request_queue) {
        return -1;
    }
    
    // Check read-only for writes
    if (dev->read_only && type == VIRTIO_BLK_T_OUT) {
        kprintf("[VIRTIO-BLK] Cannot write to read-only device\n");
        return -1;
    }
    
    // Allocate request
    virtio_blk_request_t *req = alloc_request();
    if (!req) {
        kprintf("[VIRTIO-BLK] No free requests\n");
        return -1;
    }
    
    // Set up request header
    req->header.type = type;
    req->header.reserved = 0;
    req->header.sector = sector;
    req->status = 0xFF;  // Not completed
    req->completed = false;
    req->callback = callback;
    req->callback_data = callback_data;
    req->buffer = buffer;
    req->buffer_size = count * 512;
    
    // Build scatter-gather list
    // Layout: [header (out)] [data (in/out)] [status (in)]
    void *bufs[3];
    uint32_t lens[3];
    int out_num, in_num;
    
    bufs[0] = &req->header;
    lens[0] = sizeof(virtio_blk_req_header_t);
    
    bufs[1] = buffer;
    lens[1] = count * 512;
    
    bufs[2] = &req->status;
    lens[2] = 1;
    
    if (type == VIRTIO_BLK_T_IN) {
        // Read: header out, data+status in
        out_num = 1;
        in_num = 2;
    } else {
        // Write: header+data out, status in
        out_num = 2;
        in_num = 1;
    }
    
    // Submit to virtqueue
    if (virtq_add_buffer(dev->request_queue, bufs, lens, out_num, in_num, req) != 0) {
        kprintf("[VIRTIO-BLK] Failed to add buffer\n");
        free_request(req);
        return -1;
    }
    
    // Notify device
    virtq_kick(dev->request_queue, dev->virtio_dev);
    
    return 0;
}

// Synchronous read
int virtio_blk_read(virtio_blk_device_t *dev, 
                    uint64_t sector, uint32_t count, void *buffer) {
    if (!dev || !buffer || count == 0) {
        return -1;
    }
    
    // Check bounds
    if (sector + count > dev->capacity) {
        kprintf("[VIRTIO-BLK] Read beyond device capacity\n");
        return -1;
    }
    
    // Allocate request
    virtio_blk_request_t *req = alloc_request();
    if (!req) {
        return -1;
    }
    
    // Set up request
    req->header.type = VIRTIO_BLK_T_IN;
    req->header.reserved = 0;
    req->header.sector = sector;
    req->status = 0xFF;
    req->completed = false;
    req->callback = NULL;
    req->buffer = buffer;
    req->buffer_size = count * 512;
    
    // Build buffer chain
    void *bufs[3];
    uint32_t lens[3];
    
    bufs[0] = &req->header;
    lens[0] = sizeof(virtio_blk_req_header_t);
    bufs[1] = buffer;
    lens[1] = count * 512;
    bufs[2] = &req->status;
    lens[2] = 1;
    
    if (virtq_add_buffer(dev->request_queue, bufs, lens, 1, 2, req) != 0) {
        free_request(req);
        return -1;
    }
    
    virtq_kick(dev->request_queue, dev->virtio_dev);
    
    // Poll for completion
    int timeout = 1000000;
    while (!req->completed && timeout > 0) {
        virtio_blk_poll(dev);
        timeout--;
        if (timeout % 10000 == 0) {
            io_wait();
        }
    }
    
    int result = 0;
    if (!req->completed) {
        kprintf("[VIRTIO-BLK] Read timeout\n");
        result = -1;
    } else if (req->status != VIRTIO_BLK_S_OK) {
        kprintf("[VIRTIO-BLK] Read failed with status %d\n", req->status);
        result = -1;
    } else {
        dev->reads_completed++;
        dev->bytes_read += count * 512;
    }
    
    free_request(req);
    return result;
}

// Synchronous write
int virtio_blk_write(virtio_blk_device_t *dev,
                     uint64_t sector, uint32_t count, const void *buffer) {
    if (!dev || !buffer || count == 0) {
        return -1;
    }
    
    if (dev->read_only) {
        kprintf("[VIRTIO-BLK] Device is read-only\n");
        return -1;
    }
    
    // Check bounds
    if (sector + count > dev->capacity) {
        kprintf("[VIRTIO-BLK] Write beyond device capacity\n");
        return -1;
    }
    
    // Allocate request
    virtio_blk_request_t *req = alloc_request();
    if (!req) {
        return -1;
    }
    
    // Set up request
    req->header.type = VIRTIO_BLK_T_OUT;
    req->header.reserved = 0;
    req->header.sector = sector;
    req->status = 0xFF;
    req->completed = false;
    req->callback = NULL;
    req->buffer = (void *)buffer;
    req->buffer_size = count * 512;
    
    // Build buffer chain
    void *bufs[3];
    uint32_t lens[3];
    
    bufs[0] = &req->header;
    lens[0] = sizeof(virtio_blk_req_header_t);
    bufs[1] = (void *)buffer;
    lens[1] = count * 512;
    bufs[2] = &req->status;
    lens[2] = 1;
    
    if (virtq_add_buffer(dev->request_queue, bufs, lens, 2, 1, req) != 0) {
        free_request(req);
        return -1;
    }
    
    virtq_kick(dev->request_queue, dev->virtio_dev);
    
    // Poll for completion
    int timeout = 1000000;
    while (!req->completed && timeout > 0) {
        virtio_blk_poll(dev);
        timeout--;
        if (timeout % 10000 == 0) {
            io_wait();
        }
    }
    
    int result = 0;
    if (!req->completed) {
        kprintf("[VIRTIO-BLK] Write timeout\n");
        result = -1;
    } else if (req->status != VIRTIO_BLK_S_OK) {
        kprintf("[VIRTIO-BLK] Write failed with status %d\n", req->status);
        result = -1;
    } else {
        dev->writes_completed++;
        dev->bytes_written += count * 512;
    }
    
    free_request(req);
    return result;
}

// Async read
int virtio_blk_read_async(virtio_blk_device_t *dev,
                          uint64_t sector, uint32_t count, void *buffer,
                          void (*callback)(virtio_blk_request_t*, int, void*),
                          void *callback_data) {
    return submit_request(dev, VIRTIO_BLK_T_IN, sector, count, buffer, callback, callback_data);
}

// Async write
int virtio_blk_write_async(virtio_blk_device_t *dev,
                           uint64_t sector, uint32_t count, const void *buffer,
                           void (*callback)(virtio_blk_request_t*, int, void*),
                           void *callback_data) {
    return submit_request(dev, VIRTIO_BLK_T_OUT, sector, count, (void *)buffer, callback, callback_data);
}

// Flush
int virtio_blk_flush(virtio_blk_device_t *dev) {
    if (!dev || !dev->flush_supported) {
        return -1;
    }
    
    virtio_blk_request_t *req = alloc_request();
    if (!req) {
        return -1;
    }
    
    req->header.type = VIRTIO_BLK_T_FLUSH;
    req->header.reserved = 0;
    req->header.sector = 0;
    req->status = 0xFF;
    req->completed = false;
    
    void *bufs[2];
    uint32_t lens[2];
    
    bufs[0] = &req->header;
    lens[0] = sizeof(virtio_blk_req_header_t);
    bufs[1] = &req->status;
    lens[1] = 1;
    
    if (virtq_add_buffer(dev->request_queue, bufs, lens, 1, 1, req) != 0) {
        free_request(req);
        return -1;
    }
    
    virtq_kick(dev->request_queue, dev->virtio_dev);
    
    // Wait for completion
    int timeout = 1000000;
    while (!req->completed && timeout > 0) {
        virtio_blk_poll(dev);
        timeout--;
    }
    
    int result = (req->completed && req->status == VIRTIO_BLK_S_OK) ? 0 : -1;
    free_request(req);
    return result;
}

// ============================================================================
// Device Information
// ============================================================================

uint64_t virtio_blk_get_capacity(virtio_blk_device_t *dev) {
    return dev ? dev->capacity : 0;
}

uint32_t virtio_blk_get_block_size(virtio_blk_device_t *dev) {
    return dev ? dev->block_size : 0;
}

bool virtio_blk_is_read_only(virtio_blk_device_t *dev) {
    return dev ? dev->read_only : true;
}

// ============================================================================
// Interrupt and Polling
// ============================================================================

void virtio_blk_irq_handler(virtio_blk_device_t *dev) {
    if (!dev || !dev->virtio_dev) return;
    virtio_irq_handler(dev->virtio_dev);
}

void virtio_blk_poll(virtio_blk_device_t *dev) {
    if (!dev || !dev->request_queue) return;
    
    // Process completed requests
    void *token;
    uint32_t len;
    
    while ((token = virtq_get_buffer(dev->request_queue, &len)) != NULL) {
        virtio_blk_request_t *req = (virtio_blk_request_t *)token;
        req->completed = true;
        
        if (req->callback) {
            int status = (req->status == VIRTIO_BLK_S_OK) ? 0 : -1;
            req->callback(req, status, req->callback_data);
        }
    }
}
