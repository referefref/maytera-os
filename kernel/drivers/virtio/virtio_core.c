// virtio_core.c - VirtIO Core Framework Implementation
// MayteraOS Production VirtIO Driver Infrastructure

#include "virtio_core.h"
#include "../pci.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../serial.h"
#include "../../string.h"

// ============================================================================
// Forward Declarations
// ============================================================================

void virtio_legacy_ops_init(void);
void virtio_modern_ops_init(void);

// ============================================================================
// Transport Operations (Legacy PCI)
// ============================================================================

static uint8_t legacy_get_status(virtio_device_t *dev) {
    return inb(dev->io_base + VIRTIO_PCI_LEGACY_DEVICE_STATUS);
}

static void legacy_set_status(virtio_device_t *dev, uint8_t status) {
    outb(dev->io_base + VIRTIO_PCI_LEGACY_DEVICE_STATUS, status);
}

static uint64_t legacy_get_features(virtio_device_t *dev) {
    return inl(dev->io_base + VIRTIO_PCI_LEGACY_HOST_FEATURES);
}

static void legacy_set_features(virtio_device_t *dev, uint64_t features) {
    outl(dev->io_base + VIRTIO_PCI_LEGACY_GUEST_FEATURES, (uint32_t)features);
}

static uint16_t legacy_get_queue_size(virtio_device_t *dev, uint16_t queue_idx) {
    outw(dev->io_base + VIRTIO_PCI_LEGACY_QUEUE_SELECT, queue_idx);
    return inw(dev->io_base + VIRTIO_PCI_LEGACY_QUEUE_SIZE);
}

static void legacy_set_queue_size(virtio_device_t *dev, uint16_t queue_idx, uint16_t size) {
    (void)dev; (void)queue_idx; (void)size;
    // Legacy devices don't support setting queue size
}

static void legacy_select_queue(virtio_device_t *dev, uint16_t queue_idx) {
    outw(dev->io_base + VIRTIO_PCI_LEGACY_QUEUE_SELECT, queue_idx);
}

static void legacy_activate_queue(virtio_device_t *dev, uint16_t queue_idx, virtqueue_t *vq) {
    outw(dev->io_base + VIRTIO_PCI_LEGACY_QUEUE_SELECT, queue_idx);
    // Write PFN (page frame number) - physical address >> 12
    outl(dev->io_base + VIRTIO_PCI_LEGACY_QUEUE_PFN, (uint32_t)(vq->desc_phys >> 12));
}

static void legacy_notify_queue(virtio_device_t *dev, uint16_t queue_idx) {
    outw(dev->io_base + VIRTIO_PCI_LEGACY_QUEUE_NOTIFY, queue_idx);
}

static void legacy_read_config(virtio_device_t *dev, uint32_t offset, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = inb(dev->io_base + VIRTIO_PCI_LEGACY_CONFIG + offset + i);
    }
}

static void legacy_write_config(virtio_device_t *dev, uint32_t offset, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        outb(dev->io_base + VIRTIO_PCI_LEGACY_CONFIG + offset + i, p[i]);
    }
}

static uint8_t legacy_read_isr(virtio_device_t *dev) {
    return inb(dev->io_base + VIRTIO_PCI_LEGACY_ISR_STATUS);
}

static uint8_t legacy_get_config_generation(virtio_device_t *dev) {
    (void)dev;
    return 0;  // Legacy doesn't have config generation
}

static const virtio_transport_ops_t legacy_transport_ops = {
    .get_status = legacy_get_status,
    .set_status = legacy_set_status,
    .get_features = legacy_get_features,
    .set_features = legacy_set_features,
    .get_queue_size = legacy_get_queue_size,
    .set_queue_size = legacy_set_queue_size,
    .select_queue = legacy_select_queue,
    .activate_queue = legacy_activate_queue,
    .notify_queue = legacy_notify_queue,
    .read_config = legacy_read_config,
    .write_config = legacy_write_config,
    .read_isr = legacy_read_isr,
    .get_config_generation = legacy_get_config_generation,
};

// ============================================================================
// Transport Operations (Modern PCI)
// ============================================================================

static uint8_t modern_get_status(virtio_device_t *dev) {
    return dev->common_cfg->device_status;
}

static void modern_set_status(virtio_device_t *dev, uint8_t status) {
    dev->common_cfg->device_status = status;
}

static uint64_t modern_get_features(virtio_device_t *dev) {
    uint64_t features = 0;
    dev->common_cfg->device_feature_select = 0;
    virtio_mb();
    features = dev->common_cfg->device_feature;
    dev->common_cfg->device_feature_select = 1;
    virtio_mb();
    features |= ((uint64_t)dev->common_cfg->device_feature << 32);
    return features;
}

static void modern_set_features(virtio_device_t *dev, uint64_t features) {
    dev->common_cfg->driver_feature_select = 0;
    virtio_mb();
    dev->common_cfg->driver_feature = (uint32_t)features;
    dev->common_cfg->driver_feature_select = 1;
    virtio_mb();
    dev->common_cfg->driver_feature = (uint32_t)(features >> 32);
}

static uint16_t modern_get_queue_size(virtio_device_t *dev, uint16_t queue_idx) {
    dev->common_cfg->queue_select = queue_idx;
    virtio_mb();
    return dev->common_cfg->queue_size;
}

static void modern_set_queue_size(virtio_device_t *dev, uint16_t queue_idx, uint16_t size) {
    dev->common_cfg->queue_select = queue_idx;
    virtio_mb();
    dev->common_cfg->queue_size = size;
}

static void modern_select_queue(virtio_device_t *dev, uint16_t queue_idx) {
    dev->common_cfg->queue_select = queue_idx;
    virtio_mb();
}

static void modern_activate_queue(virtio_device_t *dev, uint16_t queue_idx, virtqueue_t *vq) {
    dev->common_cfg->queue_select = queue_idx;
    virtio_mb();
    
    // Set queue addresses
    dev->common_cfg->queue_desc = vq->desc_phys;
    dev->common_cfg->queue_driver = vq->avail_phys;
    dev->common_cfg->queue_device = vq->used_phys;
    virtio_mb();
    
    // Enable the queue
    dev->common_cfg->queue_enable = 1;
}

static void modern_notify_queue(virtio_device_t *dev, uint16_t queue_idx) {
    // Calculate notify address
    volatile uint16_t *notify_addr = (volatile uint16_t *)
        ((uint8_t *)dev->notify + 
         dev->queues[queue_idx]->notify_offset * dev->notify_multiplier);
    *notify_addr = queue_idx;
}

static void modern_read_config(virtio_device_t *dev, uint32_t offset, void *buf, size_t len) {
    uint8_t generation;
    do {
        generation = dev->common_cfg->config_generation;
        virtio_rmb();
        memcpy(buf, (uint8_t *)dev->device_cfg + offset, len);
        virtio_rmb();
    } while (generation != dev->common_cfg->config_generation);
}

static void modern_write_config(virtio_device_t *dev, uint32_t offset, const void *buf, size_t len) {
    memcpy((uint8_t *)dev->device_cfg + offset, buf, len);
}

static uint8_t modern_read_isr(virtio_device_t *dev) {
    return *dev->isr;
}

static uint8_t modern_get_config_generation(virtio_device_t *dev) {
    return dev->common_cfg->config_generation;
}

static const virtio_transport_ops_t modern_transport_ops = {
    .get_status = modern_get_status,
    .set_status = modern_set_status,
    .get_features = modern_get_features,
    .set_features = modern_set_features,
    .get_queue_size = modern_get_queue_size,
    .set_queue_size = modern_set_queue_size,
    .select_queue = modern_select_queue,
    .activate_queue = modern_activate_queue,
    .notify_queue = modern_notify_queue,
    .read_config = modern_read_config,
    .write_config = modern_write_config,
    .read_isr = modern_read_isr,
    .get_config_generation = modern_get_config_generation,
};

// ============================================================================
// Device Registry
// ============================================================================

#define MAX_VIRTIO_DEVICES 32
static virtio_device_t *virtio_devices[MAX_VIRTIO_DEVICES];
static int virtio_device_count = 0;

// ============================================================================
// Utility Functions
// ============================================================================

uint32_t virtq_size_bytes(uint16_t num_entries) {
    // Descriptor table: num * 16 bytes
    // Available ring: 4 + num * 2 bytes + 2 (used_event)
    // Padding to 4096 boundary
    // Used ring: 4 + num * 8 bytes + 2 (avail_event)
    
    uint32_t desc_size = num_entries * sizeof(virtq_desc_t);
    uint32_t avail_size = sizeof(virtq_avail_t) + num_entries * sizeof(uint16_t) + sizeof(uint16_t);
    uint32_t used_start = ALIGN_UP(desc_size + avail_size, 4096);
    uint32_t used_size = sizeof(virtq_used_t) + num_entries * sizeof(virtq_used_elem_t) + sizeof(uint16_t);
    
    return used_start + used_size;
}

const char *virtio_type_name(virtio_device_type_t type) {
    switch (type) {
        case VIRTIO_TYPE_NET:      return "Network";
        case VIRTIO_TYPE_BLOCK:    return "Block";
        case VIRTIO_TYPE_CONSOLE:  return "Console";
        case VIRTIO_TYPE_ENTROPY:  return "Entropy";
        case VIRTIO_TYPE_BALLOON:  return "Balloon";
        case VIRTIO_TYPE_SCSI:     return "SCSI";
        case VIRTIO_TYPE_GPU:      return "GPU";
        case VIRTIO_TYPE_INPUT:    return "Input";
        case VIRTIO_TYPE_9P:       return "9P";
        default:                   return "Unknown";
    }
}

static virtio_device_type_t pci_id_to_type(uint16_t device_id) {
    // Transitional (legacy) device IDs
    if (device_id >= 0x1000 && device_id <= 0x103F) {
        switch (device_id) {
            case VIRTIO_DEV_NET_TRANS:     return VIRTIO_TYPE_NET;
            case VIRTIO_DEV_BLOCK_TRANS:   return VIRTIO_TYPE_BLOCK;
            case VIRTIO_DEV_BALLOON_TRANS: return VIRTIO_TYPE_BALLOON;
            case VIRTIO_DEV_CONSOLE_TRANS: return VIRTIO_TYPE_CONSOLE;
            case VIRTIO_DEV_ENTROPY_TRANS: return VIRTIO_TYPE_ENTROPY;
            case VIRTIO_DEV_SCSI_TRANS:    return VIRTIO_TYPE_SCSI;
            case VIRTIO_DEV_9P_TRANS:      return VIRTIO_TYPE_9P;
            case VIRTIO_DEV_INPUT_TRANS:   return VIRTIO_TYPE_INPUT;
            default: return VIRTIO_TYPE_INVALID;
        }
    }
    
    // Modern device IDs (type + 0x1040)
    if (device_id >= 0x1040 && device_id <= 0x107F) {
        return (virtio_device_type_t)(device_id - 0x1040);
    }
    
    return VIRTIO_TYPE_INVALID;
}

// ============================================================================
// Device Discovery
// ============================================================================

static virtio_device_t *virtio_probe_device(pci_device_t *pci_dev) {
    if (pci_dev->vendor_id != VIRTIO_PCI_VENDOR_ID) {
        return NULL;
    }
    
    virtio_device_type_t type = pci_id_to_type(pci_dev->device_id);
    if (type == VIRTIO_TYPE_INVALID) {
        return NULL;
    }
    
    virtio_device_t *dev = kzalloc(sizeof(virtio_device_t));
    if (!dev) {
        kprintf("[VIRTIO] Failed to allocate device structure\n");
        return NULL;
    }
    
    dev->type = type;
    dev->pci_device_id = pci_dev->device_id;
    dev->pci_bus = pci_dev->bus;
    dev->pci_slot = pci_dev->slot;
    dev->pci_func = pci_dev->func;
    dev->irq_line = pci_dev->interrupt_line;
    
    // Check if this is a modern (1.0+) device by looking for capabilities
    bool is_modern = false;
    
    // For now, assume transitional devices use legacy transport
    // Modern detection would require parsing PCI capabilities
    if (pci_dev->device_id >= 0x1040) {
        is_modern = true;
    }
    
    if (is_modern) {
        // Modern device - needs MMIO setup
        // TODO: Parse PCI capabilities to find common config, notify, ISR, device config
        dev->transport_type = VIRTIO_TRANSPORT_MODERN_PCI;
        dev->transport = &modern_transport_ops;
        kprintf("[VIRTIO] Modern device detected (not fully implemented)\n");
    } else {
        // Legacy device - I/O port based
        dev->transport_type = VIRTIO_TRANSPORT_LEGACY_PCI;
        dev->transport = &legacy_transport_ops;
        
        // Get I/O base from BAR0
        uint32_t bar0 = pci_dev->bar[0];
        if (!(bar0 & 0x1)) {
            kprintf("[VIRTIO] Legacy device BAR0 is not I/O space\n");
            kfree(dev);
            return NULL;
        }
        dev->io_base = bar0 & 0xFFFC;
    }
    
    // Enable bus mastering
    pci_enable_bus_master(pci_dev);
    
    // Enable I/O space
    uint16_t cmd = pci_read16(pci_dev->bus, pci_dev->slot, pci_dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_IO | PCI_CMD_MEMORY;
    pci_write16(pci_dev->bus, pci_dev->slot, pci_dev->func, PCI_COMMAND, cmd);
    
    kprintf("[VIRTIO] Found %s device at %02x:%02x.%x (ID: 0x%04x, IO: 0x%04x)\n",
            virtio_type_name(type), pci_dev->bus, pci_dev->slot, pci_dev->func,
            pci_dev->device_id, dev->io_base);
    
    return dev;
}

int virtio_init(void) {
    kprintf("[VIRTIO] Initializing VirtIO subsystem...\n");
    
    virtio_device_count = 0;
    memset(virtio_devices, 0, sizeof(virtio_devices));
    
    // Scan all PCI devices for VirtIO
    int pci_count = pci_get_device_count();
    for (int i = 0; i < pci_count && virtio_device_count < MAX_VIRTIO_DEVICES; i++) {
        pci_device_t *pci_dev = pci_get_device(i);
        if (!pci_dev) continue;
        
        virtio_device_t *dev = virtio_probe_device(pci_dev);
        if (dev) {
            virtio_devices[virtio_device_count++] = dev;
        }
    }
    
    kprintf("[VIRTIO] Found %d VirtIO device(s)\n", virtio_device_count);
    return virtio_device_count;
}

virtio_device_t *virtio_find_device(virtio_device_type_t type) {
    for (int i = 0; i < virtio_device_count; i++) {
        if (virtio_devices[i] && virtio_devices[i]->type == type) {
            return virtio_devices[i];
        }
    }
    return NULL;
}

virtio_device_t *virtio_find_device_by_pci(uint8_t bus, uint8_t slot, uint8_t func) {
    for (int i = 0; i < virtio_device_count; i++) {
        virtio_device_t *dev = virtio_devices[i];
        if (dev && dev->pci_bus == bus && dev->pci_slot == slot && dev->pci_func == func) {
            return dev;
        }
    }
    return NULL;
}

// ============================================================================
// Device Lifecycle
// ============================================================================

int virtio_device_init(virtio_device_t *dev) {
    if (!dev || !dev->transport) {
        return -1;
    }
    
    // Reset device
    virtio_device_reset(dev);
    
    // Acknowledge device
    dev->transport->set_status(dev, VIRTIO_STATUS_ACKNOWLEDGE);
    
    // Tell device we're a driver
    dev->transport->set_status(dev, 
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    
    // Get device features
    dev->device_features = dev->transport->get_features(dev);
    dev->driver_features = 0;
    
    kprintf("[VIRTIO] Device features: 0x%016llx\n", dev->device_features);
    
    return 0;
}

void virtio_device_reset(virtio_device_t *dev) {
    if (!dev || !dev->transport) return;
    
    dev->transport->set_status(dev, VIRTIO_STATUS_RESET);
    
    // Wait for reset to complete (legacy devices)
    for (int i = 0; i < 1000; i++) {
        if (dev->transport->get_status(dev) == 0) break;
        io_wait();
    }
    
    dev->initialized = false;
}

int virtio_device_finalize_features(virtio_device_t *dev) {
    if (!dev || !dev->transport) return -1;
    
    // Write selected features
    dev->transport->set_features(dev, dev->driver_features);
    
    kprintf("[VIRTIO] Negotiated features: 0x%016llx\n", dev->driver_features);
    
    // For modern devices, set FEATURES_OK
    if (dev->transport_type == VIRTIO_TRANSPORT_MODERN_PCI) {
        uint8_t status = dev->transport->get_status(dev);
        dev->transport->set_status(dev, status | VIRTIO_STATUS_FEATURES_OK);
        virtio_mb();
        
        // Verify FEATURES_OK is still set
        status = dev->transport->get_status(dev);
        if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
            kprintf("[VIRTIO] Device did not accept features\n");
            return -1;
        }
    }
    
    return 0;
}

void virtio_device_driver_ok(virtio_device_t *dev) {
    if (!dev || !dev->transport) return;
    
    uint8_t status = dev->transport->get_status(dev);
    dev->transport->set_status(dev, status | VIRTIO_STATUS_DRIVER_OK);
    dev->initialized = true;
    
    kprintf("[VIRTIO] Device ready\n");
}

void virtio_device_failed(virtio_device_t *dev) {
    if (!dev || !dev->transport) return;
    
    uint8_t status = dev->transport->get_status(dev);
    dev->transport->set_status(dev, status | VIRTIO_STATUS_FAILED);
    dev->failed = true;
    
    kprintf("[VIRTIO] Device failed\n");
}

// ============================================================================
// Feature Negotiation
// ============================================================================

uint64_t virtio_get_features(virtio_device_t *dev) {
    return dev ? dev->device_features : 0;
}

bool virtio_has_feature(virtio_device_t *dev, uint64_t feature) {
    return dev && (dev->device_features & feature);
}

void virtio_set_feature(virtio_device_t *dev, uint64_t feature) {
    if (dev && (dev->device_features & feature)) {
        dev->driver_features |= feature;
    }
}

void virtio_clear_feature(virtio_device_t *dev, uint64_t feature) {
    if (dev) {
        dev->driver_features &= ~feature;
    }
}

// ============================================================================
// Virtqueue Management
// ============================================================================

virtqueue_t *virtio_create_queue(virtio_device_t *dev, uint16_t queue_idx,
                                  void (*callback)(virtqueue_t*, void*, uint32_t)) {
    if (!dev || !dev->transport) {
        return NULL;
    }
    
    // Get queue size
    uint16_t size = dev->transport->get_queue_size(dev, queue_idx);
    if (size == 0) {
        kprintf("[VIRTIO] Queue %d has size 0\n", queue_idx);
        return NULL;
    }
    
    kprintf("[VIRTIO] Creating queue %d with %d entries\n", queue_idx, size);
    
    // Allocate virtqueue structure
    virtqueue_t *vq = kzalloc(sizeof(virtqueue_t));
    if (!vq) {
        kprintf("[VIRTIO] Failed to allocate virtqueue\n");
        return NULL;
    }
    
    vq->queue_index = queue_idx;
    vq->num_entries = size;
    vq->callback = callback;
    vq->packed = false;  // Use split queues for now
    
    // Allocate token tracking array
    vq->tokens = kzalloc(size * sizeof(void*));
    if (!vq->tokens) {
        kfree(vq);
        return NULL;
    }
    
    // Calculate buffer size needed
    uint32_t buffer_bytes = virtq_size_bytes(size);
    uint32_t pages_needed = ALIGN_UP(buffer_bytes, PAGE_SIZE) / PAGE_SIZE;
    
    kprintf("[VIRTIO] Queue needs %u bytes (%u pages)\n", buffer_bytes, pages_needed);
    
    // Allocate physically contiguous pages (DMA-safe)
    uint64_t phys = pmm_alloc_pages(pages_needed);
    if (phys == 0) {
        kprintf("[VIRTIO] Failed to allocate queue buffer\n");
        kfree(vq->tokens);
        kfree(vq);
        return NULL;
    }
    
    // Clear the buffer
    vq->buffer = (void *)phys;  // Identity-mapped
    vq->buffer_size = pages_needed * PAGE_SIZE;
    memset(vq->buffer, 0, vq->buffer_size);
    
    // Set up queue structure pointers
    // Layout: [descriptors][available ring + padding][used ring]
    vq->desc = (virtq_desc_t *)vq->buffer;
    vq->avail = (virtq_avail_t *)((uint8_t *)vq->desc + size * sizeof(virtq_desc_t));
    
    // Used ring starts on page boundary
    uint32_t avail_end = size * sizeof(virtq_desc_t) + 
                         sizeof(virtq_avail_t) + size * sizeof(uint16_t) + sizeof(uint16_t);
    uint32_t used_offset = ALIGN_UP(avail_end, PAGE_SIZE);
    vq->used = (virtq_used_t *)((uint8_t *)vq->buffer + used_offset);
    
    // Store physical addresses
    vq->desc_phys = phys;
    vq->avail_phys = phys + ((uint64_t)vq->avail - (uint64_t)vq->buffer);
    vq->used_phys = phys + used_offset;
    
    kprintf("[VIRTIO] Queue %d: desc=0x%lx avail=0x%lx used=0x%lx\n",
            queue_idx, vq->desc_phys, vq->avail_phys, vq->used_phys);
    
    // Initialize free descriptor list
    vq->free_head = 0;
    vq->num_free = size;
    for (uint16_t i = 0; i < size - 1; i++) {
        vq->desc[i].next = i + 1;
    }
    vq->desc[size - 1].next = 0xFFFF;  // End marker
    
    vq->last_used_idx = 0;
    vq->avail_idx = 0;
    vq->avail->flags = 0;
    vq->avail->idx = 0;
    
    // Activate queue with device
    dev->transport->activate_queue(dev, queue_idx, vq);
    
    // Store in device queue array
    if (!dev->queues) {
        dev->queues = kzalloc(32 * sizeof(virtqueue_t*));
        dev->num_queues = 0;
    }
    if (queue_idx >= 32) {
        kprintf("[VIRTIO] Queue index %d too large\n", queue_idx);
        // Continue anyway
    } else {
        dev->queues[queue_idx] = vq;
        if (queue_idx >= dev->num_queues) {
            dev->num_queues = queue_idx + 1;
        }
    }
    
    return vq;
}

void virtio_destroy_queue(virtqueue_t *vq) {
    if (!vq) return;
    
    if (vq->tokens) {
        kfree(vq->tokens);
    }
    
    if (vq->buffer) {
        uint32_t pages = vq->buffer_size / PAGE_SIZE;
        pmm_free_pages((uint64_t)vq->buffer, pages);
    }
    
    kfree(vq);
}

int virtio_enable_queue(virtio_device_t *dev, uint16_t queue_idx) {
    if (!dev || !dev->queues || queue_idx >= dev->num_queues) {
        return -1;
    }
    
    // For modern devices, queue_enable is set in activate_queue
    // For legacy devices, setting PFN activates the queue
    
    return 0;
}

// ============================================================================
// Buffer Operations (Split Virtqueue)
// ============================================================================

static int16_t alloc_desc(virtqueue_t *vq) {
    if (vq->num_free == 0) {
        return -1;
    }
    
    uint16_t idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    return idx;
}

static void free_desc(virtqueue_t *vq, uint16_t idx) {
    vq->desc[idx].next = vq->free_head;
    vq->free_head = idx;
    vq->num_free++;
}

int virtq_add_buffer(virtqueue_t *vq,
                     void *bufs[], uint32_t lens[],
                     int out_num, int in_num,
                     void *token) {
    if (!vq || (out_num + in_num) == 0) {
        return -1;
    }
    
    int total = out_num + in_num;
    
    // Check if we have enough descriptors
    if (vq->num_free < total) {
        kprintf("[VIRTIO] Not enough free descriptors (%d < %d)\n", vq->num_free, total);
        return -1;
    }
    
    // Allocate descriptor chain
    int16_t head = alloc_desc(vq);
    if (head < 0) {
        return -1;
    }
    
    uint16_t prev = head;
    
    // Add output buffers (device reads)
    for (int i = 0; i < out_num; i++) {
        uint16_t idx = (i == 0) ? head : alloc_desc(vq);
        if (idx == 0xFFFF) {
            // Free already allocated
            // TODO: proper cleanup
            return -1;
        }
        
        vq->desc[idx].addr = (uint64_t)bufs[i];
        vq->desc[idx].len = lens[i];
        vq->desc[idx].flags = 0;
        
        if (i > 0) {
            vq->desc[prev].flags |= VIRTQ_DESC_F_NEXT;
            vq->desc[prev].next = idx;
        }
        prev = idx;
    }
    
    // Add input buffers (device writes)
    for (int i = 0; i < in_num; i++) {
        uint16_t idx = alloc_desc(vq);
        if (idx == 0xFFFF) {
            return -1;
        }
        
        vq->desc[idx].addr = (uint64_t)bufs[out_num + i];
        vq->desc[idx].len = lens[out_num + i];
        vq->desc[idx].flags = VIRTQ_DESC_F_WRITE;
        
        vq->desc[prev].flags |= VIRTQ_DESC_F_NEXT;
        vq->desc[prev].next = idx;
        prev = idx;
    }
    
    // Clear next flag on last descriptor
    vq->desc[prev].flags &= ~VIRTQ_DESC_F_NEXT;
    
    // Store token for this chain
    vq->tokens[head] = token;
    
    // Add to available ring
    uint16_t avail_idx = vq->avail->idx % vq->num_entries;
    vq->avail->ring[avail_idx] = head;
    
    virtio_wmb();
    vq->avail->idx++;
    vq->avail_idx++;
    
    return 0;
}

int virtq_add_single_buffer(virtqueue_t *vq, void *buf, uint32_t len,
                            bool device_write, void *token) {
    void *bufs[1] = { buf };
    uint32_t lens[1] = { len };
    
    if (device_write) {
        return virtq_add_buffer(vq, bufs, lens, 0, 1, token);
    } else {
        return virtq_add_buffer(vq, bufs, lens, 1, 0, token);
    }
}

void *virtq_get_buffer(virtqueue_t *vq, uint32_t *len) {
    if (!vq) return NULL;
    
    virtio_rmb();
    
    // Check if there are used buffers
    if (vq->last_used_idx == vq->used->idx) {
        return NULL;
    }
    
    // Get the used element
    uint16_t used_idx = vq->last_used_idx % vq->num_entries;
    uint32_t desc_idx = vq->used->ring[used_idx].id;
    
    if (len) {
        *len = vq->used->ring[used_idx].len;
    }
    
    vq->last_used_idx++;
    
    // Get token
    void *token = vq->tokens[desc_idx];
    vq->tokens[desc_idx] = NULL;
    
    // Free descriptor chain
    uint16_t idx = desc_idx;
    while (true) {
        uint16_t next = vq->desc[idx].next;
        bool has_next = vq->desc[idx].flags & VIRTQ_DESC_F_NEXT;
        free_desc(vq, idx);
        if (!has_next) break;
        idx = next;
    }
    
    return token;
}

void virtq_kick(virtqueue_t *vq, virtio_device_t *dev) {
    if (!vq || !dev || !dev->transport) return;
    
    virtio_mb();
    
    // Check if we need to notify (could check used->flags for NO_NOTIFY)
    dev->transport->notify_queue(dev, vq->queue_index);
}

bool virtq_has_used(virtqueue_t *vq) {
    if (!vq) return false;
    virtio_rmb();
    return vq->last_used_idx != vq->used->idx;
}

// ============================================================================
// Interrupt Handling
// ============================================================================

void virtio_irq_handler(virtio_device_t *dev) {
    if (!dev || !dev->transport) return;
    
    // Read and acknowledge ISR
    uint8_t isr = dev->transport->read_isr(dev);
    
    if (isr & 0x01) {
        // Queue interrupt - process all queues
        for (uint16_t i = 0; i < dev->num_queues; i++) {
            virtqueue_t *vq = dev->queues[i];
            if (!vq) continue;
            
            // Process used buffers
            void *token;
            uint32_t len;
            while ((token = virtq_get_buffer(vq, &len)) != NULL) {
                if (vq->callback) {
                    vq->callback(vq, token, len);
                }
            }
        }
    }
    
    if (isr & 0x02) {
        // Configuration change - device specific handling needed
        kprintf("[VIRTIO] Configuration change notification\n");
    }
}

// ============================================================================
// Configuration Access Helpers
// ============================================================================

uint8_t virtio_read_config8(virtio_device_t *dev, uint32_t offset) {
    uint8_t val;
    dev->transport->read_config(dev, offset, &val, 1);
    return val;
}

uint16_t virtio_read_config16(virtio_device_t *dev, uint32_t offset) {
    uint16_t val;
    dev->transport->read_config(dev, offset, &val, 2);
    return val;
}

uint32_t virtio_read_config32(virtio_device_t *dev, uint32_t offset) {
    uint32_t val;
    dev->transport->read_config(dev, offset, &val, 4);
    return val;
}

uint64_t virtio_read_config64(virtio_device_t *dev, uint32_t offset) {
    uint64_t val;
    dev->transport->read_config(dev, offset, &val, 8);
    return val;
}

void virtio_write_config8(virtio_device_t *dev, uint32_t offset, uint8_t val) {
    dev->transport->write_config(dev, offset, &val, 1);
}

void virtio_write_config16(virtio_device_t *dev, uint32_t offset, uint16_t val) {
    dev->transport->write_config(dev, offset, &val, 2);
}

void virtio_write_config32(virtio_device_t *dev, uint32_t offset, uint32_t val) {
    dev->transport->write_config(dev, offset, &val, 4);
}

void virtio_write_config64(virtio_device_t *dev, uint32_t offset, uint64_t val) {
    dev->transport->write_config(dev, offset, &val, 8);
}
