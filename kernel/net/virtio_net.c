// virtio_net.c - VirtIO Network device driver implementation
#include "virtio_net.h"
#include "virtio.h"
#include "../drivers/pci.h"
#include "../mm/heap.h"
#include "../serial.h"
#include "../string.h"

// VirtIO network device state
static uint16_t virtio_iobase = 0;
static uint8_t virtio_mac[6];
static int virtio_initialized = 0;
static uint32_t virtio_features = 0;

// Virtqueues
static virtqueue_t *rxq = NULL;  // Receive queue
static virtqueue_t *txq = NULL;  // Transmit queue

// RX buffers (pre-allocated)
static uint8_t *rx_buffers[VIRTIO_NET_NUM_RX_BUFFERS];
static uint16_t rx_buffer_idx[VIRTIO_NET_NUM_RX_BUFFERS];  // Descriptor indices

// TX buffers (pre-allocated)
static uint8_t *tx_buffers[VIRTIO_NET_NUM_TX_BUFFERS];
static uint16_t tx_cur = 0;

// I/O port access functions
static inline uint8_t virtio_read8(uint16_t offset) {
    return inb(virtio_iobase + offset);
}

static inline uint16_t virtio_read16(uint16_t offset) {
    return inw(virtio_iobase + offset);
}

static inline uint32_t virtio_read32(uint16_t offset) {
    return inl(virtio_iobase + offset);
}

static inline void virtio_write8(uint16_t offset, uint8_t value) {
    outb(virtio_iobase + offset, value);
}

static inline void virtio_write16(uint16_t offset, uint16_t value) {
    outw(virtio_iobase + offset, value);
}

static inline void virtio_write32(uint16_t offset, uint32_t value) {
    outl(virtio_iobase + offset, value);
}

// Forward declaration for PMM
extern uint64_t pmm_alloc_page(void);

// Allocate and initialize a virtqueue using physical memory (DMA-safe)
static virtqueue_t *virtqueue_alloc(uint16_t queue_index) {
    // Select queue
    virtio_write16(VIRTIO_PCI_QUEUE_SELECT, queue_index);

    // Get queue size
    uint16_t queue_size = virtio_read16(VIRTIO_PCI_QUEUE_SIZE);
    if (queue_size == 0) {
        kprintf("[VIRTIO-NET] Queue %d has size 0\n", queue_index);
        return NULL;
    }

    kprintf("[VIRTIO-NET] Queue %d size: %d\n", queue_index, queue_size);

    // Allocate virtqueue structure (this can be from heap, not DMA)
    virtqueue_t *vq = kzalloc(sizeof(virtqueue_t));
    if (!vq) {
        kprintf("[VIRTIO-NET] Failed to allocate virtqueue struct\n");
        return NULL;
    }

    vq->num = queue_size;

    // Calculate required memory size (must be page-aligned)
    uint32_t size = virtq_size(queue_size);
    size = (size + 4095) & ~4095;  // Page align
    uint32_t pages_needed = (size + 4095) / 4096;

    kprintf("[VIRTIO-NET] Queue %d needs %u bytes (%u pages)\n", queue_index, size, pages_needed);

    // Allocate physical pages for DMA buffer (identity-mapped via UEFI)
    uint64_t buffer_phys = pmm_alloc_page();
    for (uint32_t i = 1; i < pages_needed; i++) {
        pmm_alloc_page();  // Allocate contiguous pages
    }

    if (buffer_phys == 0) {
        kprintf("[VIRTIO-NET] Failed to allocate virtqueue buffer\n");
        kfree(vq);
        return NULL;
    }

    // Use physical address as virtual (identity-mapped)
    vq->buffer = (void *)buffer_phys;
    memset(vq->buffer, 0, size);

    kprintf("[VIRTIO-NET] Queue %d buffer at phys 0x%lx\n", queue_index, buffer_phys);

    // Set up pointers within buffer
    // Layout: descriptors | available ring | (padding) | used ring
    vq->desc = (virtq_desc_t *)vq->buffer;
    vq->avail = (virtq_avail_t *)((uint8_t *)vq->desc + queue_size * sizeof(virtq_desc_t));

    // Used ring starts at page boundary after avail
    uint32_t avail_end = (uint64_t)vq->avail + 4 + queue_size * 2;
    uint32_t used_offset = (avail_end + 4095) & ~4095;
    vq->used = (virtq_used_t *)((uint8_t *)vq->buffer +
               (used_offset - (uint64_t)vq->buffer));

    // Initialize free descriptor list
    vq->free_head = 0;
    vq->num_free = queue_size;
    for (uint16_t i = 0; i < queue_size - 1; i++) {
        vq->desc[i].next = i + 1;
    }
    vq->desc[queue_size - 1].next = 0xFFFF;  // End of list

    vq->last_used_idx = 0;
    vq->avail->flags = 0;
    vq->avail->idx = 0;

    // Tell device about the queue (physical address >> 12 = page frame number)
    // Now buffer_phys IS the physical address (identity-mapped)
    uint64_t pfn = buffer_phys / 4096;
    virtio_write32(VIRTIO_PCI_QUEUE_PFN, (uint32_t)pfn);

    kprintf("[VIRTIO-NET] Queue %d PFN: 0x%lx\n", queue_index, pfn);

    return vq;
}

// Allocate a descriptor from the free list
static int16_t virtqueue_alloc_desc(virtqueue_t *vq) {
    if (vq->num_free == 0) {
        return -1;
    }

    uint16_t idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    return idx;
}

// Free a descriptor back to the free list
static void virtqueue_free_desc(virtqueue_t *vq, uint16_t idx) {
    vq->desc[idx].next = vq->free_head;
    vq->free_head = idx;
    vq->num_free++;
}

// Add buffer to available ring
static void virtqueue_add_avail(virtqueue_t *vq, uint16_t desc_idx) {
    uint16_t avail_idx = vq->avail->idx % vq->num;
    vq->avail->ring[avail_idx] = desc_idx;

    // Memory barrier
    __asm__ volatile("mfence" ::: "memory");

    vq->avail->idx++;
}

// Notify device about new available buffers
static void virtqueue_notify(uint16_t queue_index) {
    virtio_write16(VIRTIO_PCI_QUEUE_NOTIFY, queue_index);
}

// Add RX buffer to receive queue
static void virtio_net_add_rx_buffer(int buffer_idx) {
    int16_t desc_idx = virtqueue_alloc_desc(rxq);
    if (desc_idx < 0) {
        kprintf("[VIRTIO-NET] No free RX descriptors\n");
        return;
    }

    // Set up descriptor
    rxq->desc[desc_idx].addr = (uint64_t)rx_buffers[buffer_idx];
    rxq->desc[desc_idx].len = VIRTIO_NET_RX_BUFFER_SIZE;
    rxq->desc[desc_idx].flags = VIRTQ_DESC_F_WRITE;  // Device writes to this buffer
    rxq->desc[desc_idx].next = 0;

    // Track which buffer this descriptor points to
    rx_buffer_idx[desc_idx % VIRTIO_NET_NUM_RX_BUFFERS] = buffer_idx;

    // Add to available ring
    virtqueue_add_avail(rxq, desc_idx);
}

// Initialize VirtIO network driver
int virtio_net_init(void) {
    kprintf("[VIRTIO-NET] Scanning for VirtIO network device...\n");

    // Find VirtIO network device (try both transitional and modern IDs)
    pci_device_t *dev = pci_find_device(VIRTIO_PCI_VENDOR_ID, VIRTIO_DEV_NET);
    kprintf("[VIRTIO-NET] pci_find_device returned %p\n", (void*)dev);
    if (!dev) {
        dev = pci_find_device(VIRTIO_PCI_VENDOR_ID, VIRTIO_DEV_NET_MODERN);
    }

    if (!dev) {
        kprintf("[VIRTIO-NET] No VirtIO network device found\n");
        return -1;
    }

    kprintf("[VIRTIO-NET] Found VirtIO-net at %02x:%02x.%x (Device ID: 0x%04x)\n",
            dev->bus, dev->slot, dev->func, dev->device_id);

    // Enable bus mastering and I/O space
    pci_enable_bus_master(dev);
    uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_IO;
    pci_write16(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);

    // Get I/O base address from BAR0
    uint32_t bar0 = pci_get_bar_address(dev, 0);
    if (bar0 == 0) {
        kprintf("[VIRTIO-NET] Failed to get I/O base address\n");
        return -1;
    }

    // Check if this is I/O space (bit 0 set)
    if (!(dev->bar[0] & 1)) {
        kprintf("[VIRTIO-NET] BAR0 is not I/O space, MMIO not supported yet\n");
        return -1;
    }

    virtio_iobase = bar0 & 0xFFFC;  // Mask off low bits
    kprintf("[VIRTIO-NET] I/O base: 0x%04x\n", virtio_iobase);

    // Reset device
    virtio_write8(VIRTIO_PCI_DEVICE_STATUS, 0);
    for (int i = 0; i < 1000; i++) io_wait();

    // Acknowledge device
    virtio_write8(VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    // Tell device we're a driver
    virtio_write8(VIRTIO_PCI_DEVICE_STATUS,
                  VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // Read device features
    uint32_t device_features = virtio_read32(VIRTIO_PCI_HOST_FEATURES);
    kprintf("[VIRTIO-NET] Device features: 0x%08x\n", device_features);

    // Select features we want
    virtio_features = 0;
    if (device_features & VIRTIO_NET_F_MAC) {
        virtio_features |= VIRTIO_NET_F_MAC;
    }
    if (device_features & VIRTIO_NET_F_STATUS) {
        virtio_features |= VIRTIO_NET_F_STATUS;
    }


    virtio_write32(VIRTIO_PCI_GUEST_FEATURES, virtio_features);
    kprintf("[VIRTIO-NET] Negotiated features: 0x%08x\n", virtio_features);

    // Read MAC address from config space
    if (virtio_features & VIRTIO_NET_F_MAC) {
        for (int i = 0; i < 6; i++) {
            virtio_mac[i] = virtio_read8(VIRTIO_PCI_CONFIG + i);
        }
    } else {
        // Generate a default MAC
        virtio_mac[0] = 0x52;
        virtio_mac[1] = 0x54;
        virtio_mac[2] = 0x00;
        virtio_mac[3] = 0x12;
        virtio_mac[4] = 0x34;
        virtio_mac[5] = 0x56;
    }

    kprintf("[VIRTIO-NET] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
            virtio_mac[0], virtio_mac[1], virtio_mac[2],
            virtio_mac[3], virtio_mac[4], virtio_mac[5]);

    // Initialize virtqueues
    rxq = virtqueue_alloc(VIRTIO_NET_QUEUE_RX);
    if (!rxq) {
        kprintf("[VIRTIO-NET] Failed to allocate RX queue\n");
        virtio_write8(VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    txq = virtqueue_alloc(VIRTIO_NET_QUEUE_TX);
    if (!txq) {
        kprintf("[VIRTIO-NET] Failed to allocate TX queue\n");
        virtio_write8(VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    // Allocate RX buffers using physical memory (DMA-safe)
    // Each buffer is 2KB, use one 4KB page per buffer for simplicity
    kprintf("[VIRTIO-NET] Allocating %d RX buffers (physical memory, size=%d each)\n",
            VIRTIO_NET_NUM_RX_BUFFERS, VIRTIO_NET_RX_BUFFER_SIZE);
    for (int i = 0; i < VIRTIO_NET_NUM_RX_BUFFERS; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            kprintf("[VIRTIO-NET] Failed to allocate RX buffer %d\n", i);
            virtio_write8(VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
            return -1;
        }
        rx_buffers[i] = (uint8_t *)phys;  // Identity-mapped
        memset(rx_buffers[i], 0, VIRTIO_NET_RX_BUFFER_SIZE);
        if (i < 3) {
            kprintf("[VIRTIO-NET] RX buffer %d at 0x%lx\n", i, phys);
        }
    }

    // Allocate TX buffers using physical memory (DMA-safe)
    kprintf("[VIRTIO-NET] Allocating %d TX buffers (physical memory)\n", VIRTIO_NET_NUM_TX_BUFFERS);
    for (int i = 0; i < VIRTIO_NET_NUM_TX_BUFFERS; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            kprintf("[VIRTIO-NET] Failed to allocate TX buffer %d\n", i);
            virtio_write8(VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
            return -1;
        }
        tx_buffers[i] = (uint8_t *)phys;  // Identity-mapped
        memset(tx_buffers[i], 0, VIRTIO_NET_TX_BUFFER_SIZE);
    }

    // Add RX buffers to queue
    int added = 0;
    for (int i = 0; i < VIRTIO_NET_NUM_RX_BUFFERS && i < (int)rxq->num; i++) {
        virtio_net_add_rx_buffer(i);
        added++;
    }
    kprintf("[VIRTIO-NET] Added %d RX buffers, avail_idx=%d, num_free=%d\n",
            added, rxq->avail->idx, rxq->num_free);

    // Show first few descriptor addresses for debugging
    kprintf("[VIRTIO-NET] RX descriptors:\n");
    for (int i = 0; i < 3; i++) {
        kprintf("[VIRTIO-NET]   desc[%d]: addr=0x%lx len=%d flags=%d\n",
                i, rxq->desc[i].addr, rxq->desc[i].len, rxq->desc[i].flags);
    }

    // Notify device about available RX buffers
    virtqueue_notify(VIRTIO_NET_QUEUE_RX);
    kprintf("[VIRTIO-NET] Notified device about RX buffers\n");
    kprintf("[VIRTIO-NET] RX queue used ring at %p\n", (void*)rxq->used);

    // Set DRIVER_OK to indicate we're ready
    virtio_write8(VIRTIO_PCI_DEVICE_STATUS,
                  VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    virtio_initialized = 1;
    kprintf("[VIRTIO-NET] Driver initialized (link %s)\n",
            virtio_net_link_up() ? "up" : "down");

    pci_mark_claimed(dev, "virtio-net");  // #418: /DEVLOG.TXT PCI-claim tracking
    return 0;
}

// Get MAC address
void virtio_net_get_mac(uint8_t *mac) {
    if (mac) {
        memcpy(mac, virtio_mac, 6);
    }
}

// Send a packet
int virtio_net_send(const void *data, uint16_t length) {
    // kprintf("[VIRTIO-NET] send: len=%d, init=%d\n", length, virtio_initialized);

    if (!virtio_initialized || !data || length == 0) {
        kprintf("[VIRTIO-NET] send: invalid params\n");
        return -1;
    }

    if (length > VIRTIO_NET_TX_BUFFER_SIZE - sizeof(virtio_net_hdr_t)) {
        kprintf("[VIRTIO-NET] send: packet too large\n");
        return -1;
    }

    // Wait for a free TX descriptor
    // kprintf("[VIRTIO-NET] send: waiting for free desc (num_free=%d)\n", txq->num_free);
    int timeout = 10000;
    while (txq->num_free < 1 && timeout > 0) {
        // Process completed TX
        while (txq->last_used_idx != txq->used->idx) {
            uint16_t used_idx = txq->last_used_idx % txq->num;
            uint16_t desc_idx = txq->used->ring[used_idx].id;
            virtqueue_free_desc(txq, desc_idx);
            txq->last_used_idx++;
        }
        timeout--;
    }

    if (txq->num_free < 1) {
        kprintf("[VIRTIO-NET] TX queue full after timeout\n");
        return -1;
    }

    // Get buffer
    uint8_t *buf = tx_buffers[tx_cur];
    tx_cur = (tx_cur + 1) % VIRTIO_NET_NUM_TX_BUFFERS;

    // Set up virtio-net header (no checksum, no GSO)
    virtio_net_hdr_t *hdr = (virtio_net_hdr_t *)buf;
    memset(hdr, 0, sizeof(virtio_net_hdr_t));

    // Copy packet data after header
    memcpy(buf + sizeof(virtio_net_hdr_t), data, length);

    // Allocate descriptor
    int16_t desc_idx = virtqueue_alloc_desc(txq);
    if (desc_idx < 0) {
        kprintf("[VIRTIO-NET] send: failed to alloc desc\n");
        return -1;
    }

    // Set up descriptor (buf is physical address, identity-mapped)
    txq->desc[desc_idx].addr = (uint64_t)buf;
    txq->desc[desc_idx].len = sizeof(virtio_net_hdr_t) + length;
    // kprintf("[VIRTIO-NET] send: buf=0x%lx desc_addr=0x%lx\n", (uint64_t)buf, txq->desc[desc_idx].addr);
    txq->desc[desc_idx].flags = 0;  // Device reads from this buffer
    txq->desc[desc_idx].next = 0;

    // Add to available ring
    virtqueue_add_avail(txq, desc_idx);

    // Notify device
    virtqueue_notify(VIRTIO_NET_QUEUE_TX);
    // kprintf("[VIRTIO-NET] send: packet queued OK, desc=%d\n", desc_idx);

    return length;
}

// Receive a packet
static int rx_debug_counter = 0;
static int rx_unique_init = 0;
int virtio_net_receive(void *buffer, uint16_t buffer_size) {
    extern void serial_puts(uint16_t port, const char *str);
    if (!rx_unique_init) {
        rx_unique_init = 1;
        serial_puts(0x3F8, "###UNIQUE_CODE_VERSION_20260124_0144###\r\n");
    }
    if (!virtio_initialized || !buffer) {
        return 0;
    }

    // Check for received packets
    // Note: used->idx is in RAM, updated by device via DMA
    uint16_t device_idx = rxq->used->idx;
    if (rxq->last_used_idx == device_idx) {
        // Print debug less frequently but with more info
        if ((rx_debug_counter++ % 10000) == 0) {
            //             kprintf("[VIRTIO-NET] rx poll #%d: no pkts (last=%d, device_idx=%d, avail_idx=%d)\n",
            //                     rx_debug_counter, rxq->last_used_idx, device_idx,
            //                     rxq->avail->idx);
            // Check if used ring is at expected physical address
            //             kprintf("[VIRTIO-NET] used ring addr=%p, first entry id=%d len=%d\n",
            //                     (void*)rxq->used, rxq->used->ring[0].id, rxq->used->ring[0].len);
        }
        return 0;  // No packets
    }
    // Direct serial for debugging
    extern void serial_puts(uint16_t port, const char *str);
    //     serial_puts(0x3F8, "[VIRTIO-NET] RX PACKET RECEIVED!\r\n");

    // Get the used buffer
    uint16_t used_idx = rxq->last_used_idx % rxq->num;
    uint16_t desc_idx = rxq->used->ring[used_idx].id;
    uint32_t total_len = rxq->used->ring[used_idx].len;

    //     serial_puts(0x3F8, "[VIRTIO-NET] RX GOT USED BUFFER!\r\n");

    rxq->last_used_idx++;

    // Get buffer index from our tracking array
    uint16_t buffer_idx = rx_buffer_idx[desc_idx % VIRTIO_NET_NUM_RX_BUFFERS];
    uint8_t *rx_buf = rx_buffers[buffer_idx];

    //     serial_puts(0x3F8, "[VIRTIO-NET] RX GOT RX_BUF!\r\n");

    // Skip the virtio-net header
    if (total_len <= sizeof(virtio_net_hdr_t)) {
        // No data, just header
        serial_puts(0x3F8, "[VIRTIO-NET] rx: header only, no data\r\n");
        virtqueue_free_desc(rxq, desc_idx);
        virtio_net_add_rx_buffer(buffer_idx);
        virtqueue_notify(VIRTIO_NET_QUEUE_RX);
        return 0;
    }

    uint16_t data_len = total_len - sizeof(virtio_net_hdr_t);
    //     serial_printf(0x3F8, "[VIRTIO-NET] rx: data_len=%u\r\n", data_len);

    if (data_len > buffer_size) {
        data_len = buffer_size;
    }

    // Copy data (skip header)
    memcpy(buffer, rx_buf + sizeof(virtio_net_hdr_t), data_len);

    // Debug: show we're returning data
    //     serial_puts(0x3F8, "[VIRTIO-NET] rx: returning data!\r\n");

    // Return descriptor to free list and re-add to RX queue
    virtqueue_free_desc(rxq, desc_idx);
    virtio_net_add_rx_buffer(buffer_idx);
    virtqueue_notify(VIRTIO_NET_QUEUE_RX);

    return data_len;
}

// Check link status
int virtio_net_link_up(void) {
    if (!virtio_initialized) return 0;

    // If device supports status feature, read it
    if (virtio_features & VIRTIO_NET_F_STATUS) {
        uint16_t status = virtio_read16(VIRTIO_PCI_CONFIG + 6);  // After MAC
        return (status & VIRTIO_NET_S_LINK_UP) != 0;
    }

    // Otherwise assume link is up
    return 1;
}

// Handle interrupt
void virtio_net_irq_handler(void) {
    if (!virtio_initialized) return;

    // Read and acknowledge ISR
    uint8_t isr = virtio_read8(VIRTIO_PCI_ISR_STATUS);
    (void)isr;

    // TX completion and RX are handled by polling
}
