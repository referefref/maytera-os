// virtio_init.c - VirtIO Subsystem Initialization
// MayteraOS Production VirtIO Driver System

#include "virtio.h"
#include "../../serial.h"
#include "../../string.h"

// ============================================================================
// Subsystem Initialization
// ============================================================================

int virtio_subsystem_init(void) {
    kprintf("[VIRTIO] ============================================\n");
    kprintf("[VIRTIO] MayteraOS VirtIO Framework v1.0\n");
    kprintf("[VIRTIO] Supports: Legacy + Modern PCI Transport\n");
    kprintf("[VIRTIO] Devices: Net, Block, Console, GPU, Input\n");
    kprintf("[VIRTIO] ============================================\n");
    
    // Initialize core VirtIO subsystem (scans PCI)
    int device_count = virtio_init();
    if (device_count == 0) {
        kprintf("[VIRTIO] No VirtIO devices found\n");
        return 0;
    }
    
    kprintf("[VIRTIO] Discovered %d VirtIO device(s)\n", device_count);
    kprintf("[VIRTIO]\n");
    
    // Initialize block devices
    kprintf("[VIRTIO] --- Block Devices ---\n");
    int blk_count = virtio_blk_init();
    if (blk_count > 0) {
        for (int i = 0; i < blk_count; i++) {
            virtio_blk_device_t *blk = virtio_blk_get_device(i);
            if (blk) {
                uint64_t size_mb = (virtio_blk_get_capacity(blk) * 512) / (1024 * 1024);
                kprintf("[VIRTIO]   blk%d: %llu MB %s\n", i, size_mb,
                       virtio_blk_is_read_only(blk) ? "(ro)" : "(rw)");
            }
        }
    } else {
        kprintf("[VIRTIO]   (none)\n");
    }
    kprintf("[VIRTIO]\n");
    
    // Initialize console devices
    kprintf("[VIRTIO] --- Console Devices ---\n");
    int con_count = virtio_console_init();
    if (con_count > 0) {
        for (int i = 0; i < con_count; i++) {
            virtio_console_device_t *con = virtio_console_get_device(i);
            if (con) {
                uint16_t cols, rows;
                virtio_console_get_size(con, &cols, &rows);
                kprintf("[VIRTIO]   console%d: %ux%u\n", i, cols, rows);
            }
        }
    } else {
        kprintf("[VIRTIO]   (none)\n");
    }
    kprintf("[VIRTIO]\n");
    
    // Initialize GPU devices
    kprintf("[VIRTIO] --- GPU Devices ---\n");
    int gpu_count = virtio_gpu_init();
    if (gpu_count > 0) {
        for (int i = 0; i < gpu_count; i++) {
            virtio_gpu_device_t *gpu = virtio_gpu_get_device(i);
            if (gpu) {
                kprintf("[VIRTIO]   gpu%d: ready\n", i);
            }
        }
    } else {
        kprintf("[VIRTIO]   (none)\n");
    }
    kprintf("[VIRTIO]\n");
    
    // Initialize input devices
    kprintf("[VIRTIO] --- Input Devices ---\n");
    int input_count = virtio_input_init();
    if (input_count > 0) {
        for (int i = 0; i < input_count; i++) {
            virtio_input_device_t *input = virtio_input_get_device(i);
            if (input) {
                const char *type_name = "unknown";
                switch (virtio_input_get_type(input)) {
                    case VIRTIO_INPUT_TYPE_KEYBOARD: type_name = "keyboard"; break;
                    case VIRTIO_INPUT_TYPE_MOUSE: type_name = "mouse"; break;
                    case VIRTIO_INPUT_TYPE_TABLET: type_name = "tablet"; break;
                    case VIRTIO_INPUT_TYPE_TOUCHSCREEN: type_name = "touchscreen"; break;
                    default: break;
                }
                kprintf("[VIRTIO]   input%d: %s (%s)\n", i, 
                       virtio_input_get_name(input), type_name);
            }
        }
    } else {
        kprintf("[VIRTIO]   (none)\n");
    }
    
    kprintf("[VIRTIO]\n");
    kprintf("[VIRTIO] ============================================\n");
    kprintf("[VIRTIO] VirtIO initialization complete\n");
    kprintf("[VIRTIO] ============================================\n");
    
    return device_count;
}

void virtio_print_status(void) {
    kprintf("\n[VIRTIO] === Device Status ===\n");
    
    // Block devices
    for (int i = 0; ; i++) {
        virtio_blk_device_t *blk = virtio_blk_get_device(i);
        if (!blk) break;
        
        kprintf("  blk%d: capacity=%llu sectors\n", i, virtio_blk_get_capacity(blk));
    }
    
    // Console devices
    for (int i = 0; ; i++) {
        virtio_console_device_t *con = virtio_console_get_device(i);
        if (!con) break;
        
        uint16_t cols, rows;
        virtio_console_get_size(con, &cols, &rows);
        kprintf("  console%d: %ux%u\n", i, cols, rows);
    }
    
    // GPU devices
    for (int i = 0; ; i++) {
        virtio_gpu_device_t *gpu = virtio_gpu_get_device(i);
        if (!gpu) break;
        
        kprintf("  gpu%d: fb=%p %ux%u\n", i, 
               virtio_gpu_get_framebuffer(gpu),
               virtio_gpu_get_fb_width(gpu),
               virtio_gpu_get_fb_height(gpu));
    }
    
    // Input devices
    int input_count = virtio_input_get_device_count();
    for (int i = 0; i < input_count; i++) {
        virtio_input_device_t *input = virtio_input_get_device(i);
        if (input) {
            kprintf("  input%d: %s (events=%llu)\n", i,
                   virtio_input_get_name(input), input->events_received);
        }
    }
    
    kprintf("\n");
}

int virtio_get_device_count(void) {
    // Sum all device types
    int count = 0;
    
    for (int i = 0; virtio_blk_get_device(i); i++) count++;
    for (int i = 0; virtio_console_get_device(i); i++) count++;
    for (int i = 0; virtio_gpu_get_device(i); i++) count++;
    count += virtio_input_get_device_count();
    
    return count;
}

bool virtio_has_device(virtio_device_type_t type) {
    switch (type) {
        case VIRTIO_TYPE_BLOCK:
            return virtio_blk_get_device(0) != NULL;
        case VIRTIO_TYPE_CONSOLE:
            return virtio_console_get_device(0) != NULL;
        case VIRTIO_TYPE_GPU:
            return virtio_gpu_get_device(0) != NULL;
        case VIRTIO_TYPE_INPUT:
            return virtio_input_get_device_count() > 0;
        default:
            return virtio_find_device(type) != NULL;
    }
}
