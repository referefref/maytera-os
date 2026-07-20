// virtio_gpu.c - VirtIO GPU Driver Implementation
// MayteraOS Production VirtIO Graphics

#include "virtio_gpu.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../serial.h"
#include "../../string.h"

// ============================================================================
// Module State
// ============================================================================

#define MAX_GPU_DEVICES 4
static virtio_gpu_device_t *gpu_devices[MAX_GPU_DEVICES];
static int gpu_device_count = 0;

// Queue indices
#define VIRTIO_GPU_QUEUE_CTRL   0
#define VIRTIO_GPU_QUEUE_CURSOR 1

// ============================================================================
// Command Submission
// ============================================================================

static int submit_command(virtio_gpu_device_t *dev, void *cmd, size_t cmd_size,
                          void *resp, size_t resp_size) {
    if (!dev || !dev->ctrl_queue || !cmd || !resp) {
        return -1;
    }
    
    // Copy command to DMA-safe buffer
    memcpy(dev->cmd_buffer, cmd, cmd_size);
    memset(dev->resp_buffer, 0, resp_size);
    
    // Submit to queue
    void *bufs[2] = { dev->cmd_buffer, dev->resp_buffer };
    uint32_t lens[2] = { cmd_size, resp_size };
    
    volatile bool completed = false;
    
    if (virtq_add_buffer(dev->ctrl_queue, bufs, lens, 1, 1, (void *)&completed) != 0) {
        kprintf("[VIRTIO-GPU] Failed to add command buffer\n");
        return -1;
    }
    
    virtq_kick(dev->ctrl_queue, dev->virtio_dev);
    
    // Wait for completion
    int timeout = 1000000;
    while (!completed && timeout > 0) {
        void *token;
        uint32_t len;
        while ((token = virtq_get_buffer(dev->ctrl_queue, &len)) != NULL) {
            volatile bool *flag = (volatile bool *)token;
            *flag = true;
        }
        timeout--;
        if (timeout % 10000 == 0) {
            io_wait();
        }
    }
    
    if (!completed) {
        kprintf("[VIRTIO-GPU] Command timeout\n");
        return -1;
    }
    
    // Copy response
    memcpy(resp, dev->resp_buffer, resp_size);
    
    // Check response type
    virtio_gpu_ctrl_hdr_t *hdr = (virtio_gpu_ctrl_hdr_t *)resp;
    if (hdr->type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
        kprintf("[VIRTIO-GPU] Command error: 0x%x\n", hdr->type);
        return -1;
    }
    
    return 0;
}

// ============================================================================
// Device Initialization
// ============================================================================

static int init_gpu_device(virtio_device_t *vdev) {
    if (!vdev) return -1;
    
    virtio_gpu_device_t *dev = kzalloc(sizeof(virtio_gpu_device_t));
    if (!dev) {
        kprintf("[VIRTIO-GPU] Failed to allocate device\n");
        return -1;
    }
    
    dev->virtio_dev = vdev;
    vdev->driver_data = dev;
    dev->next_resource_id = 1;
    
    // Initialize device
    if (virtio_device_init(vdev) != 0) {
        kfree(dev);
        return -1;
    }
    
    // Check features
    uint64_t features = virtio_get_features(vdev);
    kprintf("[VIRTIO-GPU] Device features: 0x%016llx\n", features);
    
    // We support basic 2D for now
    if (features & VIRTIO_GPU_F_EDID) {
        virtio_set_feature(vdev, VIRTIO_GPU_F_EDID);
    }
    
    // Finalize features
    if (virtio_device_finalize_features(vdev) != 0) {
        kfree(dev);
        return -1;
    }
    
    // Read configuration
    dev->num_scanouts = virtio_read_config32(vdev, offsetof(virtio_gpu_config_t, num_scanouts));
    dev->num_capsets = virtio_read_config32(vdev, offsetof(virtio_gpu_config_t, num_capsets));
    
    kprintf("[VIRTIO-GPU] Scanouts: %u, Capsets: %u\n", dev->num_scanouts, dev->num_capsets);
    
    // Create control queue
    dev->ctrl_queue = virtio_create_queue(vdev, VIRTIO_GPU_QUEUE_CTRL, NULL);
    if (!dev->ctrl_queue) {
        kprintf("[VIRTIO-GPU] Failed to create control queue\n");
        kfree(dev);
        return -1;
    }
    
    // Create cursor queue
    dev->cursor_queue = virtio_create_queue(vdev, VIRTIO_GPU_QUEUE_CURSOR, NULL);
    if (!dev->cursor_queue) {
        kprintf("[VIRTIO-GPU] Warning: Failed to create cursor queue\n");
        // Not fatal
    }
    
    // Allocate DMA-safe command/response buffers
    uint64_t cmd_phys = pmm_alloc_page();
    uint64_t resp_phys = pmm_alloc_page();
    if (cmd_phys == 0 || resp_phys == 0) {
        kprintf("[VIRTIO-GPU] Failed to allocate command buffers\n");
        kfree(dev);
        return -1;
    }
    dev->cmd_buffer = (void *)cmd_phys;
    dev->resp_buffer = (void *)resp_phys;
    
    // Mark device ready
    virtio_device_driver_ok(vdev);
    
    // Add to device list
    if (gpu_device_count < MAX_GPU_DEVICES) {
        gpu_devices[gpu_device_count++] = dev;
    }
    
    kprintf("[VIRTIO-GPU] Device %d initialized\n", gpu_device_count - 1);
    return 0;
}

// ============================================================================
// Public API
// ============================================================================

int virtio_gpu_init(void) {
    kprintf("[VIRTIO-GPU] Initializing VirtIO GPU driver...\n");
    
    gpu_device_count = 0;
    memset(gpu_devices, 0, sizeof(gpu_devices));
    
    // Find GPU devices
    virtio_device_t *vdev = virtio_find_device(VIRTIO_TYPE_GPU);
    if (vdev && !vdev->initialized) {
        init_gpu_device(vdev);
    }
    
    kprintf("[VIRTIO-GPU] Found %d GPU device(s)\n", gpu_device_count);
    return gpu_device_count;
}

virtio_gpu_device_t *virtio_gpu_get_device(int index) {
    if (index < 0 || index >= gpu_device_count) {
        return NULL;
    }
    return gpu_devices[index];
}

int virtio_gpu_get_display_info(virtio_gpu_device_t *dev) {
    if (!dev) return -1;
    
    virtio_gpu_ctrl_hdr_t cmd = {
        .type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO,
        .flags = 0,
        .fence_id = 0,
        .ctx_id = 0,
        .padding = 0
    };
    
    virtio_gpu_resp_display_info_t resp;
    
    if (submit_command(dev, &cmd, sizeof(cmd), &resp, sizeof(resp)) != 0) {
        return -1;
    }
    
    // Copy display info
    memcpy(dev->displays, resp.pmodes, sizeof(dev->displays));
    
    // Log enabled displays
    for (uint32_t i = 0; i < dev->num_scanouts && i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (dev->displays[i].enabled) {
            kprintf("[VIRTIO-GPU] Display %u: %ux%u at (%u,%u)\n",
                    i, dev->displays[i].r.width, dev->displays[i].r.height,
                    dev->displays[i].r.x, dev->displays[i].r.y);
        }
    }
    
    return 0;
}

int virtio_gpu_resource_create_2d(virtio_gpu_device_t *dev,
                                   uint32_t resource_id,
                                   uint32_t format,
                                   uint32_t width, uint32_t height) {
    if (!dev) return -1;
    
    virtio_gpu_resource_create_2d_t cmd = {
        .hdr = {
            .type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
            .flags = 0,
            .fence_id = 0,
            .ctx_id = 0,
            .padding = 0
        },
        .resource_id = resource_id,
        .format = format,
        .width = width,
        .height = height
    };
    
    virtio_gpu_ctrl_hdr_t resp;
    
    if (submit_command(dev, &cmd, sizeof(cmd), &resp, sizeof(resp)) != 0) {
        return -1;
    }
    
    kprintf("[VIRTIO-GPU] Created resource %u: %ux%u format %u\n",
            resource_id, width, height, format);
    
    return 0;
}

int virtio_gpu_resource_attach_backing(virtio_gpu_device_t *dev,
                                        uint32_t resource_id,
                                        void *backing, size_t size) {
    if (!dev || !backing) return -1;
    
    // Build command with one memory entry
    struct {
        virtio_gpu_resource_attach_backing_t cmd;
        virtio_gpu_mem_entry_t entry;
    } __attribute__((packed)) req = {
        .cmd = {
            .hdr = {
                .type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
                .flags = 0,
                .fence_id = 0,
                .ctx_id = 0,
                .padding = 0
            },
            .resource_id = resource_id,
            .nr_entries = 1
        },
        .entry = {
            .addr = (uint64_t)backing,
            .length = size,
            .padding = 0
        }
    };
    
    virtio_gpu_ctrl_hdr_t resp;
    
    if (submit_command(dev, &req, sizeof(req), &resp, sizeof(resp)) != 0) {
        return -1;
    }
    
    kprintf("[VIRTIO-GPU] Attached backing %p (%zu bytes) to resource %u\n",
            backing, size, resource_id);
    
    return 0;
}

int virtio_gpu_set_scanout(virtio_gpu_device_t *dev,
                           uint32_t scanout_id,
                           uint32_t resource_id,
                           uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height) {
    if (!dev) return -1;
    
    virtio_gpu_set_scanout_t cmd = {
        .hdr = {
            .type = VIRTIO_GPU_CMD_SET_SCANOUT,
            .flags = 0,
            .fence_id = 0,
            .ctx_id = 0,
            .padding = 0
        },
        .r = {
            .x = x,
            .y = y,
            .width = width,
            .height = height
        },
        .scanout_id = scanout_id,
        .resource_id = resource_id
    };
    
    virtio_gpu_ctrl_hdr_t resp;
    
    if (submit_command(dev, &cmd, sizeof(cmd), &resp, sizeof(resp)) != 0) {
        return -1;
    }
    
    return 0;
}

int virtio_gpu_transfer_to_host_2d(virtio_gpu_device_t *dev,
                                    uint32_t resource_id,
                                    uint32_t x, uint32_t y,
                                    uint32_t width, uint32_t height) {
    if (!dev) return -1;
    
    virtio_gpu_transfer_to_host_2d_t cmd = {
        .hdr = {
            .type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
            .flags = 0,
            .fence_id = 0,
            .ctx_id = 0,
            .padding = 0
        },
        .r = {
            .x = x,
            .y = y,
            .width = width,
            .height = height
        },
        .offset = 0,
        .resource_id = resource_id,
        .padding = 0
    };
    
    virtio_gpu_ctrl_hdr_t resp;
    
    return submit_command(dev, &cmd, sizeof(cmd), &resp, sizeof(resp));
}

int virtio_gpu_resource_flush(virtio_gpu_device_t *dev,
                               uint32_t resource_id,
                               uint32_t x, uint32_t y,
                               uint32_t width, uint32_t height) {
    if (!dev) return -1;
    
    virtio_gpu_resource_flush_t cmd = {
        .hdr = {
            .type = VIRTIO_GPU_CMD_RESOURCE_FLUSH,
            .flags = 0,
            .fence_id = 0,
            .ctx_id = 0,
            .padding = 0
        },
        .r = {
            .x = x,
            .y = y,
            .width = width,
            .height = height
        },
        .resource_id = resource_id,
        .padding = 0
    };
    
    virtio_gpu_ctrl_hdr_t resp;
    
    return submit_command(dev, &cmd, sizeof(cmd), &resp, sizeof(resp));
}

int virtio_gpu_setup_framebuffer(virtio_gpu_device_t *dev,
                                  uint32_t width, uint32_t height) {
    if (!dev) return -1;
    
    // Get display info first
    if (virtio_gpu_get_display_info(dev) != 0) {
        kprintf("[VIRTIO-GPU] Failed to get display info\n");
        return -1;
    }
    
    // Use first enabled display if no size specified
    if (width == 0 || height == 0) {
        for (uint32_t i = 0; i < dev->num_scanouts; i++) {
            if (dev->displays[i].enabled) {
                width = dev->displays[i].r.width;
                height = dev->displays[i].r.height;
                break;
            }
        }
    }
    
    if (width == 0 || height == 0) {
        width = 1024;
        height = 768;
    }
    
    kprintf("[VIRTIO-GPU] Setting up framebuffer: %ux%u\n", width, height);
    
    // Calculate framebuffer size (32-bit color)
    dev->fb_width = width;
    dev->fb_height = height;
    dev->fb_stride = width * 4;
    dev->fb_format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    
    size_t fb_size = dev->fb_stride * height;
    uint32_t pages_needed = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    // Allocate framebuffer (DMA-safe, physically contiguous)
    uint64_t fb_phys = pmm_alloc_pages(pages_needed);
    if (fb_phys == 0) {
        kprintf("[VIRTIO-GPU] Failed to allocate framebuffer\n");
        return -1;
    }
    
    dev->framebuffer = (void *)fb_phys;
    memset(dev->framebuffer, 0, fb_size);
    
    // Create resource
    uint32_t resource_id = dev->next_resource_id++;
    if (virtio_gpu_resource_create_2d(dev, resource_id, dev->fb_format, width, height) != 0) {
        pmm_free_pages(fb_phys, pages_needed);
        return -1;
    }
    
    // Attach backing
    if (virtio_gpu_resource_attach_backing(dev, resource_id, dev->framebuffer, fb_size) != 0) {
        pmm_free_pages(fb_phys, pages_needed);
        return -1;
    }
    
    // Set scanout
    if (virtio_gpu_set_scanout(dev, 0, resource_id, 0, 0, width, height) != 0) {
        pmm_free_pages(fb_phys, pages_needed);
        return -1;
    }
    
    // Store primary resource info
    dev->primary_resource = kzalloc(sizeof(virtio_gpu_resource_t));
    if (dev->primary_resource) {
        dev->primary_resource->id = resource_id;
        dev->primary_resource->width = width;
        dev->primary_resource->height = height;
        dev->primary_resource->format = dev->fb_format;
        dev->primary_resource->backing = dev->framebuffer;
        dev->primary_resource->backing_size = fb_size;
        dev->primary_resource->attached = true;
    }
    
    kprintf("[VIRTIO-GPU] Framebuffer ready at %p\n", dev->framebuffer);
    
    return 0;
}

void *virtio_gpu_get_framebuffer(virtio_gpu_device_t *dev) {
    return dev ? dev->framebuffer : NULL;
}

uint32_t virtio_gpu_get_fb_width(virtio_gpu_device_t *dev) {
    return dev ? dev->fb_width : 0;
}

uint32_t virtio_gpu_get_fb_height(virtio_gpu_device_t *dev) {
    return dev ? dev->fb_height : 0;
}

uint32_t virtio_gpu_get_fb_stride(virtio_gpu_device_t *dev) {
    return dev ? dev->fb_stride : 0;
}

int virtio_gpu_update_display(virtio_gpu_device_t *dev) {
    if (!dev || !dev->primary_resource) return -1;
    
    uint32_t resource_id = dev->primary_resource->id;
    
    // Transfer to host
    if (virtio_gpu_transfer_to_host_2d(dev, resource_id, 0, 0, dev->fb_width, dev->fb_height) != 0) {
        return -1;
    }
    
    // Flush
    if (virtio_gpu_resource_flush(dev, resource_id, 0, 0, dev->fb_width, dev->fb_height) != 0) {
        return -1;
    }
    
    dev->frames_rendered++;
    return 0;
}

int virtio_gpu_update_cursor(virtio_gpu_device_t *dev,
                              uint32_t resource_id,
                              uint32_t x, uint32_t y,
                              uint32_t hot_x, uint32_t hot_y) {
    if (!dev || !dev->cursor_queue) return -1;
    
    virtio_gpu_update_cursor_t cmd = {
        .hdr = {
            .type = VIRTIO_GPU_CMD_UPDATE_CURSOR,
            .flags = 0,
            .fence_id = 0,
            .ctx_id = 0,
            .padding = 0
        },
        .pos = {
            .scanout_id = 0,
            .x = x,
            .y = y,
            .padding = 0
        },
        .resource_id = resource_id,
        .hot_x = hot_x,
        .hot_y = hot_y,
        .padding = 0
    };
    
    virtio_gpu_ctrl_hdr_t resp;
    return submit_command(dev, &cmd, sizeof(cmd), &resp, sizeof(resp));
}

int virtio_gpu_move_cursor(virtio_gpu_device_t *dev,
                           uint32_t x, uint32_t y) {
    if (!dev || !dev->cursor_queue) return -1;
    
    virtio_gpu_update_cursor_t cmd = {
        .hdr = {
            .type = VIRTIO_GPU_CMD_MOVE_CURSOR,
            .flags = 0,
            .fence_id = 0,
            .ctx_id = 0,
            .padding = 0
        },
        .pos = {
            .scanout_id = 0,
            .x = x,
            .y = y,
            .padding = 0
        },
        .resource_id = 0,
        .hot_x = 0,
        .hot_y = 0,
        .padding = 0
    };
    
    virtio_gpu_ctrl_hdr_t resp;
    return submit_command(dev, &cmd, sizeof(cmd), &resp, sizeof(resp));
}

void virtio_gpu_poll(virtio_gpu_device_t *dev) {
    if (!dev) return;
    
    if (dev->ctrl_queue) {
        void *token;
        uint32_t len;
        while ((token = virtq_get_buffer(dev->ctrl_queue, &len)) != NULL) {
            volatile bool *flag = (volatile bool *)token;
            *flag = true;
        }
    }
    
    if (dev->cursor_queue) {
        void *token;
        uint32_t len;
        while ((token = virtq_get_buffer(dev->cursor_queue, &len)) != NULL) {
            (void)token;
        }
    }
}
