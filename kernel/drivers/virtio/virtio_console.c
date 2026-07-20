// virtio_console.c - VirtIO Console Driver Implementation
// MayteraOS Production VirtIO Console

#include "virtio_console.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../serial.h"
#include "../../string.h"

// ============================================================================
// Module State
// ============================================================================

#define MAX_CONSOLE_DEVICES 4
static virtio_console_device_t *console_devices[MAX_CONSOLE_DEVICES];
static int console_device_count = 0;

// Queue indices
#define VIRTIO_CONSOLE_QUEUE_RX_PORT0   0
#define VIRTIO_CONSOLE_QUEUE_TX_PORT0   1
#define VIRTIO_CONSOLE_QUEUE_CTRL_RX    2  // If multiport
#define VIRTIO_CONSOLE_QUEUE_CTRL_TX    3  // If multiport

// ============================================================================
// RX Buffer Management
// ============================================================================

static void add_rx_buffer(virtio_console_device_t *dev, int buf_idx) {
    if (!dev || !dev->rx_queue || buf_idx < 0 || buf_idx >= VIRTIO_CONSOLE_NUM_BUFFERS) {
        return;
    }
    
    uint8_t *buf = dev->rx_buffers[buf_idx];
    if (!buf) return;
    
    if (virtq_add_single_buffer(dev->rx_queue, buf, VIRTIO_CONSOLE_RX_BUFFER_SIZE,
                                 true, buf) == 0) {
        // Success
    }
}

static void rx_completion(virtqueue_t *vq, void *token, uint32_t len) {
    (void)vq;
    
    // Find which device this belongs to
    virtio_console_device_t *dev = NULL;
    for (int i = 0; i < console_device_count; i++) {
        if (console_devices[i] && console_devices[i]->rx_queue == vq) {
            dev = console_devices[i];
            break;
        }
    }
    
    if (!dev || !token || len == 0) return;
    
    uint8_t *data = (uint8_t *)token;
    
    // Copy to ring buffer
    for (uint32_t i = 0; i < len; i++) {
        uint32_t next_head = (dev->rx_head + 1) % VIRTIO_CONSOLE_RX_BUFFER_SIZE;
        if (next_head != dev->rx_tail) {
            dev->rx_ring[dev->rx_head] = data[i];
            dev->rx_head = next_head;
        }
        // else: buffer full, drop data
    }
    
    dev->bytes_rx += len;
    
    // Call callback if set
    if (dev->rx_callback) {
        dev->rx_callback(dev, data, len);
    }
    
    // Find buffer index and re-add it
    for (int i = 0; i < VIRTIO_CONSOLE_NUM_BUFFERS; i++) {
        if (dev->rx_buffers[i] == data) {
            add_rx_buffer(dev, i);
            virtq_kick(dev->rx_queue, dev->virtio_dev);
            break;
        }
    }
}

// ============================================================================
// Device Initialization
// ============================================================================

static int init_console_device(virtio_device_t *vdev) {
    if (!vdev) return -1;
    
    virtio_console_device_t *dev = kzalloc(sizeof(virtio_console_device_t));
    if (!dev) {
        kprintf("[VIRTIO-CONSOLE] Failed to allocate device\n");
        return -1;
    }
    
    dev->virtio_dev = vdev;
    vdev->driver_data = dev;
    dev->rx_head = 0;
    dev->rx_tail = 0;
    
    // Initialize device
    if (virtio_device_init(vdev) != 0) {
        kfree(dev);
        return -1;
    }
    
    // Check features
    uint64_t features = virtio_get_features(vdev);
    kprintf("[VIRTIO-CONSOLE] Device features: 0x%016llx\n", features);
    
    if (features & VIRTIO_CONSOLE_F_SIZE) {
        virtio_set_feature(vdev, VIRTIO_CONSOLE_F_SIZE);
    }
    
    if (features & VIRTIO_CONSOLE_F_MULTIPORT) {
        virtio_set_feature(vdev, VIRTIO_CONSOLE_F_MULTIPORT);
        dev->multiport = true;
    }
    
    // Finalize features
    if (virtio_device_finalize_features(vdev) != 0) {
        kfree(dev);
        return -1;
    }
    
    // Read configuration
    if (virtio_has_feature(vdev, VIRTIO_CONSOLE_F_SIZE)) {
        dev->cols = virtio_read_config16(vdev, offsetof(virtio_console_config_t, cols));
        dev->rows = virtio_read_config16(vdev, offsetof(virtio_console_config_t, rows));
        kprintf("[VIRTIO-CONSOLE] Size: %dx%d\n", dev->cols, dev->rows);
    } else {
        dev->cols = 80;
        dev->rows = 25;
    }
    
    if (dev->multiport) {
        dev->max_ports = virtio_read_config32(vdev, offsetof(virtio_console_config_t, max_nr_ports));
        kprintf("[VIRTIO-CONSOLE] Max ports: %u\n", dev->max_ports);
    }
    
    // Create RX queue
    dev->rx_queue = virtio_create_queue(vdev, VIRTIO_CONSOLE_QUEUE_RX_PORT0, rx_completion);
    if (!dev->rx_queue) {
        kprintf("[VIRTIO-CONSOLE] Failed to create RX queue\n");
        kfree(dev);
        return -1;
    }
    
    // Create TX queue
    dev->tx_queue = virtio_create_queue(vdev, VIRTIO_CONSOLE_QUEUE_TX_PORT0, NULL);
    if (!dev->tx_queue) {
        kprintf("[VIRTIO-CONSOLE] Failed to create TX queue\n");
        virtio_destroy_queue(dev->rx_queue);
        kfree(dev);
        return -1;
    }
    
    // Allocate RX buffers
    for (int i = 0; i < VIRTIO_CONSOLE_NUM_BUFFERS; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            kprintf("[VIRTIO-CONSOLE] Failed to allocate RX buffer %d\n", i);
            break;
        }
        dev->rx_buffers[i] = (uint8_t *)phys;
        memset(dev->rx_buffers[i], 0, PAGE_SIZE);
        dev->rx_buffer_count++;
    }
    
    // Allocate TX buffers
    for (int i = 0; i < VIRTIO_CONSOLE_NUM_BUFFERS; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            kprintf("[VIRTIO-CONSOLE] Failed to allocate TX buffer %d\n", i);
            break;
        }
        dev->tx_buffers[i] = (uint8_t *)phys;
        memset(dev->tx_buffers[i], 0, PAGE_SIZE);
    }
    
    // Add RX buffers to queue
    for (int i = 0; i < dev->rx_buffer_count; i++) {
        add_rx_buffer(dev, i);
    }
    virtq_kick(dev->rx_queue, vdev);
    
    // Mark device ready
    virtio_device_driver_ok(vdev);
    
    // Add to device list
    if (console_device_count < MAX_CONSOLE_DEVICES) {
        console_devices[console_device_count++] = dev;
    }
    
    kprintf("[VIRTIO-CONSOLE] Device %d initialized\n", console_device_count - 1);
    return 0;
}

// ============================================================================
// Public API
// ============================================================================

int virtio_console_init(void) {
    kprintf("[VIRTIO-CONSOLE] Initializing VirtIO console driver...\n");
    
    console_device_count = 0;
    memset(console_devices, 0, sizeof(console_devices));
    
    // Find console devices
    virtio_device_t *vdev = virtio_find_device(VIRTIO_TYPE_CONSOLE);
    if (vdev && !vdev->initialized) {
        init_console_device(vdev);
    }
    
    kprintf("[VIRTIO-CONSOLE] Found %d console device(s)\n", console_device_count);
    return console_device_count;
}

virtio_console_device_t *virtio_console_get_device(int index) {
    if (index < 0 || index >= console_device_count) {
        return NULL;
    }
    return console_devices[index];
}

int virtio_console_write(virtio_console_device_t *dev, const void *data, size_t len) {
    if (!dev || !dev->tx_queue || !data || len == 0) {
        return -1;
    }
    
    // Get a TX buffer
    uint8_t *buf = dev->tx_buffers[dev->tx_buffer_idx];
    dev->tx_buffer_idx = (dev->tx_buffer_idx + 1) % VIRTIO_CONSOLE_NUM_BUFFERS;
    
    // Copy data (limit to buffer size)
    size_t copy_len = (len > VIRTIO_CONSOLE_TX_BUFFER_SIZE) ? VIRTIO_CONSOLE_TX_BUFFER_SIZE : len;
    memcpy(buf, data, copy_len);
    
    // Add to TX queue
    if (virtq_add_single_buffer(dev->tx_queue, buf, copy_len, false, buf) != 0) {
        return -1;
    }
    
    virtq_kick(dev->tx_queue, dev->virtio_dev);
    dev->bytes_tx += copy_len;
    
    return (int)copy_len;
}

int virtio_console_puts(virtio_console_device_t *dev, const char *str) {
    if (!str) return -1;
    return virtio_console_write(dev, str, strlen(str));
}

int virtio_console_putc(virtio_console_device_t *dev, char c) {
    return virtio_console_write(dev, &c, 1);
}

int virtio_console_read(virtio_console_device_t *dev, void *buffer, size_t max_len) {
    if (!dev || !buffer || max_len == 0) {
        return -1;
    }
    
    // Poll for new data first
    virtio_console_poll(dev);
    
    uint8_t *buf = (uint8_t *)buffer;
    size_t count = 0;
    
    while (count < max_len && dev->rx_tail != dev->rx_head) {
        buf[count++] = dev->rx_ring[dev->rx_tail];
        dev->rx_tail = (dev->rx_tail + 1) % VIRTIO_CONSOLE_RX_BUFFER_SIZE;
    }
    
    return (int)count;
}

int virtio_console_getc(virtio_console_device_t *dev) {
    if (!dev) return -1;
    
    // Wait for data
    while (dev->rx_tail == dev->rx_head) {
        virtio_console_poll(dev);
        pause();
    }
    
    uint8_t c = dev->rx_ring[dev->rx_tail];
    dev->rx_tail = (dev->rx_tail + 1) % VIRTIO_CONSOLE_RX_BUFFER_SIZE;
    return c;
}

bool virtio_console_has_data(virtio_console_device_t *dev) {
    if (!dev) return false;
    virtio_console_poll(dev);
    return dev->rx_tail != dev->rx_head;
}

void virtio_console_set_rx_callback(virtio_console_device_t *dev,
                                     void (*callback)(virtio_console_device_t*, const uint8_t*, size_t)) {
    if (dev) {
        dev->rx_callback = callback;
    }
}

void virtio_console_get_size(virtio_console_device_t *dev, uint16_t *cols, uint16_t *rows) {
    if (!dev) return;
    if (cols) *cols = dev->cols;
    if (rows) *rows = dev->rows;
}

void virtio_console_poll(virtio_console_device_t *dev) {
    if (!dev) return;
    
    // Process RX queue
    if (dev->rx_queue) {
        void *token;
        uint32_t len;
        while ((token = virtq_get_buffer(dev->rx_queue, &len)) != NULL) {
            rx_completion(dev->rx_queue, token, len);
        }
    }
    
    // Process TX completions (free buffers)
    if (dev->tx_queue) {
        void *token;
        uint32_t len;
        while ((token = virtq_get_buffer(dev->tx_queue, &len)) != NULL) {
            // TX buffer completed, nothing to do
            (void)token;
        }
    }
}

void virtio_console_irq_handler(virtio_console_device_t *dev) {
    if (!dev || !dev->virtio_dev) return;
    virtio_irq_handler(dev->virtio_dev);
}
