// virtio_input.h - VirtIO Input Driver
// MayteraOS Production VirtIO Keyboard/Mouse

#ifndef VIRTIO_INPUT_H
#define VIRTIO_INPUT_H

#include "virtio_core.h"

// ============================================================================
// VirtIO Input Configuration Select Values
// ============================================================================

#define VIRTIO_INPUT_CFG_UNSET      0x00
#define VIRTIO_INPUT_CFG_ID_NAME    0x01
#define VIRTIO_INPUT_CFG_ID_SERIAL  0x02
#define VIRTIO_INPUT_CFG_ID_DEVIDS  0x03
#define VIRTIO_INPUT_CFG_PROP_BITS  0x10
#define VIRTIO_INPUT_CFG_EV_BITS    0x11
#define VIRTIO_INPUT_CFG_ABS_INFO   0x12

// ============================================================================
// Linux Input Event Types (compatible)
// ============================================================================

#define EV_SYN      0x00    // Synchronization
#define EV_KEY      0x01    // Key/button
#define EV_REL      0x02    // Relative movement
#define EV_ABS      0x03    // Absolute movement
#define EV_MSC      0x04    // Miscellaneous
#define EV_SW       0x05    // Switch
#define EV_LED      0x11    // LED
#define EV_SND      0x12    // Sound
#define EV_REP      0x14    // Repeat
#define EV_FF       0x15    // Force feedback
#define EV_PWR      0x16    // Power
#define EV_FF_STATUS 0x17   // Force feedback status

// Key codes (subset)
#define KEY_RESERVED    0
#define KEY_ESC         1
#define KEY_1           2
#define KEY_2           3
#define KEY_3           4
#define KEY_4           5
#define KEY_5           6
#define KEY_6           7
#define KEY_7           8
#define KEY_8           9
#define KEY_9           10
#define KEY_0           11
#define KEY_MINUS       12
#define KEY_EQUAL       13
#define KEY_BACKSPACE   14
#define KEY_TAB         15
#define KEY_Q           16
#define KEY_W           17
#define KEY_E           18
#define KEY_R           19
#define KEY_T           20
#define KEY_Y           21
#define KEY_U           22
#define KEY_I           23
#define KEY_O           24
#define KEY_P           25
#define KEY_LEFTBRACE   26
#define KEY_RIGHTBRACE  27
#define KEY_ENTER       28
#define KEY_LEFTCTRL    29
#define KEY_A           30
#define KEY_S           31
#define KEY_D           32
#define KEY_F           33
#define KEY_G           34
#define KEY_H           35
#define KEY_J           36
#define KEY_K           37
#define KEY_L           38
#define KEY_SEMICOLON   39
#define KEY_APOSTROPHE  40
#define KEY_GRAVE       41
#define KEY_LEFTSHIFT   42
#define KEY_BACKSLASH   43
#define KEY_Z           44
#define KEY_X           45
#define KEY_C           46
#define KEY_V           47
#define KEY_B           48
#define KEY_N           49
#define KEY_M           50
#define KEY_COMMA       51
#define KEY_DOT         52
#define KEY_SLASH       53
#define KEY_RIGHTSHIFT  54
#define KEY_KPASTERISK  55
#define KEY_LEFTALT     56
#define KEY_SPACE       57
#define KEY_CAPSLOCK    58
#define KEY_F1          59
#define KEY_F2          60
#define KEY_F3          61
#define KEY_F4          62
#define KEY_F5          63
#define KEY_F6          64
#define KEY_F7          65
#define KEY_F8          66
#define KEY_F9          67
#define KEY_F10         68
#define KEY_F11         87
#define KEY_F12         88

#define KEY_UP          103
#define KEY_LEFT        105
#define KEY_RIGHT       106
#define KEY_DOWN        108

#define KEY_DELETE      111

// Mouse button codes
#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112
#define BTN_SIDE        0x113
#define BTN_EXTRA       0x114

// Relative axes
#define REL_X           0x00
#define REL_Y           0x01
#define REL_Z           0x02
#define REL_WHEEL       0x08
#define REL_HWHEEL      0x06

// Absolute axes
#define ABS_X           0x00
#define ABS_Y           0x01
#define ABS_Z           0x02

// ============================================================================
// VirtIO Input Structures
// ============================================================================

// Input event (sent from device)
typedef struct virtio_input_event {
    uint16_t type;      // Event type (EV_*)
    uint16_t code;      // Event code
    uint32_t value;     // Event value
} __attribute__((packed)) virtio_input_event_t;

// Configuration structure
typedef struct virtio_input_config {
    uint8_t select;
    uint8_t subsel;
    uint8_t size;
    uint8_t reserved[5];
    union {
        char string[128];
        uint8_t bitmap[128];
        struct {
            uint32_t min;
            uint32_t max;
            uint32_t fuzz;
            uint32_t flat;
            uint32_t res;
        } abs;
        struct {
            uint16_t bustype;
            uint16_t vendor;
            uint16_t product;
            uint16_t version;
        } ids;
    } u;
} __attribute__((packed)) virtio_input_config_t;

// ============================================================================
// VirtIO Input Device Types
// ============================================================================

typedef enum {
    VIRTIO_INPUT_TYPE_UNKNOWN,
    VIRTIO_INPUT_TYPE_KEYBOARD,
    VIRTIO_INPUT_TYPE_MOUSE,
    VIRTIO_INPUT_TYPE_TABLET,
    VIRTIO_INPUT_TYPE_TOUCHSCREEN,
} virtio_input_type_t;

// ============================================================================
// VirtIO Input Device
// ============================================================================

#define VIRTIO_INPUT_EVENT_BUFFER_SIZE  64

typedef struct virtio_input_device {
    virtio_device_t *virtio_dev;
    
    // Device info
    char name[128];
    char serial[128];
    virtio_input_type_t input_type;
    
    // Queues
    virtqueue_t *event_queue;
    virtqueue_t *status_queue;
    
    // Event buffers (DMA safe)
    virtio_input_event_t *event_buffers[VIRTIO_INPUT_EVENT_BUFFER_SIZE];
    int event_buffer_count;
    
    // Event ring buffer
    virtio_input_event_t event_ring[256];
    volatile uint32_t event_head;
    volatile uint32_t event_tail;
    
    // Callback
    void (*event_callback)(struct virtio_input_device *dev, virtio_input_event_t *event);
    
    // Statistics
    uint64_t events_received;
} virtio_input_device_t;

// ============================================================================
// API Functions
// ============================================================================

// Initialize VirtIO input subsystem
int virtio_input_init(void);

// Get number of input devices
int virtio_input_get_device_count(void);

// Get input device by index
virtio_input_device_t *virtio_input_get_device(int index);

// Get keyboard device (first keyboard found)
virtio_input_device_t *virtio_input_get_keyboard(void);

// Get mouse device (first mouse found)
virtio_input_device_t *virtio_input_get_mouse(void);

// Check if events are available
bool virtio_input_has_event(virtio_input_device_t *dev);

// Get next event (non-blocking, returns false if no event)
bool virtio_input_get_event(virtio_input_device_t *dev, virtio_input_event_t *event);

// Set event callback
void virtio_input_set_callback(virtio_input_device_t *dev,
                                void (*callback)(virtio_input_device_t*, virtio_input_event_t*));

// Poll for events
void virtio_input_poll(virtio_input_device_t *dev);

// Poll all devices
void virtio_input_poll_all(void);

// Get device name
const char *virtio_input_get_name(virtio_input_device_t *dev);

// Get device type
virtio_input_type_t virtio_input_get_type(virtio_input_device_t *dev);

// IRQ handler
void virtio_input_irq_handler(virtio_input_device_t *dev);

#endif // VIRTIO_INPUT_H
