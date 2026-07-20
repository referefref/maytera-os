// virtio_input.c - VirtIO Input Driver Implementation
// MayteraOS Production VirtIO Keyboard/Mouse

#include "virtio_input.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../serial.h"
#include "../../string.h"
#include "../pci.h"

// ============================================================================
// Module State
// ============================================================================

#define MAX_INPUT_DEVICES 16
static virtio_input_device_t *input_devices[MAX_INPUT_DEVICES];
static int input_device_count = 0;

// Queue indices
#define VIRTIO_INPUT_QUEUE_EVENT  0
#define VIRTIO_INPUT_QUEUE_STATUS 1

// ============================================================================
// Event Buffer Management
// ============================================================================

static void add_event_buffer(virtio_input_device_t *dev, int buf_idx) {
    if (!dev || !dev->event_queue || buf_idx < 0 || buf_idx >= VIRTIO_INPUT_EVENT_BUFFER_SIZE) {
        return;
    }
    
    virtio_input_event_t *buf = dev->event_buffers[buf_idx];
    if (!buf) return;
    
    memset(buf, 0, sizeof(virtio_input_event_t));
    
    if (virtq_add_single_buffer(dev->event_queue, buf, sizeof(virtio_input_event_t),
                                 true, buf) == 0) {
        // Success
    }
}

static void process_event(virtio_input_device_t *dev, virtio_input_event_t *event) {
    if (!dev || !event) return;
    
    // Add to ring buffer
    uint32_t next_head = (dev->event_head + 1) % 256;
    if (next_head != dev->event_tail) {
        dev->event_ring[dev->event_head] = *event;
        dev->event_head = next_head;
    }
    
    dev->events_received++;
    
    // Call callback if set
    if (dev->event_callback) {
        dev->event_callback(dev, event);
    }
}

// ============================================================================
// Configuration Reading
// ============================================================================

static void read_config_string(virtio_device_t *vdev, uint8_t select, 
                                char *buffer, size_t max_len) {
    // Write select value
    virtio_write_config8(vdev, offsetof(virtio_input_config_t, select), select);
    virtio_write_config8(vdev, offsetof(virtio_input_config_t, subsel), 0);
    virtio_mb();
    
    // Read size
    uint8_t size = virtio_read_config8(vdev, offsetof(virtio_input_config_t, size));
    if (size == 0 || size > 128) {
        buffer[0] = '\0';
        return;
    }
    
    // Read string
    size_t copy_len = (size < max_len - 1) ? size : max_len - 1;
    for (size_t i = 0; i < copy_len; i++) {
        buffer[i] = virtio_read_config8(vdev, 
            offsetof(virtio_input_config_t, u.string) + i);
    }
    buffer[copy_len] = '\0';
}

static bool has_event_type(virtio_device_t *vdev, uint8_t type) {
    virtio_write_config8(vdev, offsetof(virtio_input_config_t, select), 
                         VIRTIO_INPUT_CFG_EV_BITS);
    virtio_write_config8(vdev, offsetof(virtio_input_config_t, subsel), type);
    virtio_mb();
    
    uint8_t size = virtio_read_config8(vdev, offsetof(virtio_input_config_t, size));
    return size > 0;
}

static virtio_input_type_t detect_input_type(virtio_device_t *vdev) {
    bool has_key = has_event_type(vdev, EV_KEY);
    bool has_rel = has_event_type(vdev, EV_REL);
    bool has_abs = has_event_type(vdev, EV_ABS);
    
    // Check for keyboard by looking for key events
    if (has_key && !has_rel && !has_abs) {
        return VIRTIO_INPUT_TYPE_KEYBOARD;
    }
    
    // Mouse has relative movement
    if (has_rel) {
        return VIRTIO_INPUT_TYPE_MOUSE;
    }
    
    // Tablet/touchscreen has absolute positioning
    if (has_abs) {
        return VIRTIO_INPUT_TYPE_TABLET;
    }
    
    if (has_key) {
        return VIRTIO_INPUT_TYPE_KEYBOARD;
    }
    
    return VIRTIO_INPUT_TYPE_UNKNOWN;
}

// ============================================================================
// Device Initialization
// ============================================================================

static int init_input_device(virtio_device_t *vdev) {
    if (!vdev) return -1;
    
    virtio_input_device_t *dev = kzalloc(sizeof(virtio_input_device_t));
    if (!dev) {
        kprintf("[VIRTIO-INPUT] Failed to allocate device\n");
        return -1;
    }
    
    dev->virtio_dev = vdev;
    vdev->driver_data = dev;
    dev->event_head = 0;
    dev->event_tail = 0;
    
    // Initialize device
    if (virtio_device_init(vdev) != 0) {
        kfree(dev);
        return -1;
    }
    
    // No special features for input
    if (virtio_device_finalize_features(vdev) != 0) {
        kfree(dev);
        return -1;
    }
    
    // Read device name
    read_config_string(vdev, VIRTIO_INPUT_CFG_ID_NAME, dev->name, sizeof(dev->name));
    read_config_string(vdev, VIRTIO_INPUT_CFG_ID_SERIAL, dev->serial, sizeof(dev->serial));
    
    // Detect input type
    dev->input_type = detect_input_type(vdev);
    
    const char *type_name = "unknown";
    switch (dev->input_type) {
        case VIRTIO_INPUT_TYPE_KEYBOARD: type_name = "keyboard"; break;
        case VIRTIO_INPUT_TYPE_MOUSE: type_name = "mouse"; break;
        case VIRTIO_INPUT_TYPE_TABLET: type_name = "tablet"; break;
        case VIRTIO_INPUT_TYPE_TOUCHSCREEN: type_name = "touchscreen"; break;
        default: break;
    }
    
    kprintf("[VIRTIO-INPUT] Device: %s (%s)\n", dev->name, type_name);
    
    // Create event queue
    dev->event_queue = virtio_create_queue(vdev, VIRTIO_INPUT_QUEUE_EVENT, NULL);
    if (!dev->event_queue) {
        kprintf("[VIRTIO-INPUT] Failed to create event queue\n");
        kfree(dev);
        return -1;
    }
    
    // Create status queue (optional)
    dev->status_queue = virtio_create_queue(vdev, VIRTIO_INPUT_QUEUE_STATUS, NULL);
    // Not fatal if this fails
    
    // Allocate event buffers
    for (int i = 0; i < VIRTIO_INPUT_EVENT_BUFFER_SIZE; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            kprintf("[VIRTIO-INPUT] Failed to allocate event buffer %d\n", i);
            break;
        }
        dev->event_buffers[i] = (virtio_input_event_t *)phys;
        dev->event_buffer_count++;
    }
    
    // Add event buffers to queue
    for (int i = 0; i < dev->event_buffer_count; i++) {
        add_event_buffer(dev, i);
    }
    virtq_kick(dev->event_queue, vdev);
    
    // Mark device ready
    virtio_device_driver_ok(vdev);
    
    // Add to device list
    if (input_device_count < MAX_INPUT_DEVICES) {
        input_devices[input_device_count++] = dev;
    }
    
    kprintf("[VIRTIO-INPUT] Device %d initialized (%d event buffers)\n", 
            input_device_count - 1, dev->event_buffer_count);
    
    return 0;
}

// ============================================================================
// Public API
// ============================================================================

int virtio_input_init(void) {
    kprintf("[VIRTIO-INPUT] Initializing VirtIO input driver...\n");
    
    input_device_count = 0;
    memset(input_devices, 0, sizeof(input_devices));
    
    // VirtIO input devices may use transitional or modern device IDs
    // Scan all PCI devices for input types
    int pci_count = pci_get_device_count();
    for (int i = 0; i < pci_count && input_device_count < MAX_INPUT_DEVICES; i++) {
        pci_device_t *pci_dev = pci_get_device(i);
        if (!pci_dev || pci_dev->vendor_id != VIRTIO_PCI_VENDOR_ID) {
            continue;
        }
        
        // Check for input device ID
        if (pci_dev->device_id != VIRTIO_DEV_INPUT_TRANS &&
            pci_dev->device_id != VIRTIO_DEV_INPUT_MODERN) {
            continue;
        }
        
        // Try to initialize
        virtio_device_t *vdev = virtio_find_device_by_pci(pci_dev->bus, pci_dev->slot, pci_dev->func);
        if (vdev && !vdev->initialized) {
            init_input_device(vdev);
        }
    }
    
    kprintf("[VIRTIO-INPUT] Found %d input device(s)\n", input_device_count);
    return input_device_count;
}

int virtio_input_get_device_count(void) {
    return input_device_count;
}

virtio_input_device_t *virtio_input_get_device(int index) {
    if (index < 0 || index >= input_device_count) {
        return NULL;
    }
    return input_devices[index];
}

virtio_input_device_t *virtio_input_get_keyboard(void) {
    for (int i = 0; i < input_device_count; i++) {
        if (input_devices[i] && input_devices[i]->input_type == VIRTIO_INPUT_TYPE_KEYBOARD) {
            return input_devices[i];
        }
    }
    return NULL;
}

virtio_input_device_t *virtio_input_get_mouse(void) {
    for (int i = 0; i < input_device_count; i++) {
        if (input_devices[i] && 
            (input_devices[i]->input_type == VIRTIO_INPUT_TYPE_MOUSE ||
             input_devices[i]->input_type == VIRTIO_INPUT_TYPE_TABLET)) {
            return input_devices[i];
        }
    }
    return NULL;
}

bool virtio_input_has_event(virtio_input_device_t *dev) {
    if (!dev) return false;
    virtio_input_poll(dev);
    return dev->event_tail != dev->event_head;
}

bool virtio_input_get_event(virtio_input_device_t *dev, virtio_input_event_t *event) {
    if (!dev || !event) return false;
    
    virtio_input_poll(dev);
    
    if (dev->event_tail == dev->event_head) {
        return false;
    }
    
    *event = dev->event_ring[dev->event_tail];
    dev->event_tail = (dev->event_tail + 1) % 256;
    return true;
}

void virtio_input_set_callback(virtio_input_device_t *dev,
                                void (*callback)(virtio_input_device_t*, virtio_input_event_t*)) {
    if (dev) {
        dev->event_callback = callback;
    }
}

void virtio_input_poll(virtio_input_device_t *dev) {
    if (!dev || !dev->event_queue) return;
    
    void *token;
    uint32_t len;
    
    while ((token = virtq_get_buffer(dev->event_queue, &len)) != NULL) {
        virtio_input_event_t *event = (virtio_input_event_t *)token;
        
        // Process event
        if (len >= sizeof(virtio_input_event_t)) {
            process_event(dev, event);
        }
        
        // Find buffer index and re-add
        for (int i = 0; i < dev->event_buffer_count; i++) {
            if (dev->event_buffers[i] == event) {
                add_event_buffer(dev, i);
                break;
            }
        }
    }
    
    // Kick to ensure buffers are available
    virtq_kick(dev->event_queue, dev->virtio_dev);
}

void virtio_input_poll_all(void) {
    for (int i = 0; i < input_device_count; i++) {
        if (input_devices[i]) {
            virtio_input_poll(input_devices[i]);
        }
    }
}

const char *virtio_input_get_name(virtio_input_device_t *dev) {
    return dev ? dev->name : "Unknown";
}

virtio_input_type_t virtio_input_get_type(virtio_input_device_t *dev) {
    return dev ? dev->input_type : VIRTIO_INPUT_TYPE_UNKNOWN;
}

void virtio_input_irq_handler(virtio_input_device_t *dev) {
    if (!dev || !dev->virtio_dev) return;
    virtio_irq_handler(dev->virtio_dev);
}
