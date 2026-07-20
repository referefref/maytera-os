// virtio_console.h - VirtIO Console (Serial) Driver
// MayteraOS Production VirtIO Console

#ifndef VIRTIO_CONSOLE_H
#define VIRTIO_CONSOLE_H

#include "virtio_core.h"

// ============================================================================
// VirtIO Console Feature Bits
// ============================================================================

#define VIRTIO_CONSOLE_F_SIZE           (1ULL << 0)   // Console size available
#define VIRTIO_CONSOLE_F_MULTIPORT      (1ULL << 1)   // Multiple ports support
#define VIRTIO_CONSOLE_F_EMERG_WRITE    (1ULL << 2)   // Emergency write support

// ============================================================================
// VirtIO Console Configuration
// ============================================================================

typedef struct virtio_console_config {
    uint16_t cols;          // Console width (if F_SIZE)
    uint16_t rows;          // Console height (if F_SIZE)
    uint32_t max_nr_ports;  // Max ports (if F_MULTIPORT)
    uint32_t emerg_wr;      // Emergency write value
} __attribute__((packed)) virtio_console_config_t;

// ============================================================================
// VirtIO Console Control Messages (for multiport)
// ============================================================================

#define VIRTIO_CONSOLE_DEVICE_READY     0
#define VIRTIO_CONSOLE_DEVICE_ADD       1
#define VIRTIO_CONSOLE_DEVICE_REMOVE    2
#define VIRTIO_CONSOLE_PORT_READY       3
#define VIRTIO_CONSOLE_CONSOLE_PORT     4
#define VIRTIO_CONSOLE_RESIZE           5
#define VIRTIO_CONSOLE_PORT_OPEN        6
#define VIRTIO_CONSOLE_PORT_NAME        7

typedef struct virtio_console_control {
    uint32_t id;        // Port number
    uint16_t event;     // Control event
    uint16_t value;     // Event value
} __attribute__((packed)) virtio_console_control_t;

// ============================================================================
// VirtIO Console Device
// ============================================================================

#define VIRTIO_CONSOLE_RX_BUFFER_SIZE   4096
#define VIRTIO_CONSOLE_TX_BUFFER_SIZE   4096
#define VIRTIO_CONSOLE_NUM_BUFFERS      16

typedef struct virtio_console_device {
    virtio_device_t *virtio_dev;
    
    // Configuration
    uint16_t cols;
    uint16_t rows;
    bool multiport;
    uint32_t max_ports;
    
    // Queues
    virtqueue_t *rx_queue;
    virtqueue_t *tx_queue;
    virtqueue_t *ctrl_rx_queue;  // For multiport
    virtqueue_t *ctrl_tx_queue;  // For multiport
    
    // RX buffers
    uint8_t *rx_buffers[VIRTIO_CONSOLE_NUM_BUFFERS];
    int rx_buffer_count;
    
    // TX buffers
    uint8_t *tx_buffers[VIRTIO_CONSOLE_NUM_BUFFERS];
    int tx_buffer_idx;
    
    // Ring buffer for received data
    uint8_t rx_ring[VIRTIO_CONSOLE_RX_BUFFER_SIZE];
    volatile uint32_t rx_head;
    volatile uint32_t rx_tail;
    
    // Callback for received data
    void (*rx_callback)(struct virtio_console_device *dev, const uint8_t *data, size_t len);
    
    // Statistics
    uint64_t bytes_rx;
    uint64_t bytes_tx;
} virtio_console_device_t;

// ============================================================================
// API Functions
// ============================================================================

// Initialize VirtIO console subsystem
int virtio_console_init(void);

// Get console device
virtio_console_device_t *virtio_console_get_device(int index);

// Write data to console
int virtio_console_write(virtio_console_device_t *dev, const void *data, size_t len);

// Write string to console
int virtio_console_puts(virtio_console_device_t *dev, const char *str);

// Write single character
int virtio_console_putc(virtio_console_device_t *dev, char c);

// Read data from console (non-blocking, returns bytes read)
int virtio_console_read(virtio_console_device_t *dev, void *buffer, size_t max_len);

// Read single character (blocking)
int virtio_console_getc(virtio_console_device_t *dev);

// Check if data available
bool virtio_console_has_data(virtio_console_device_t *dev);

// Set receive callback
void virtio_console_set_rx_callback(virtio_console_device_t *dev,
                                     void (*callback)(virtio_console_device_t*, const uint8_t*, size_t));

// Get console size
void virtio_console_get_size(virtio_console_device_t *dev, uint16_t *cols, uint16_t *rows);

// Poll for data
void virtio_console_poll(virtio_console_device_t *dev);

// IRQ handler
void virtio_console_irq_handler(virtio_console_device_t *dev);

#endif // VIRTIO_CONSOLE_H
