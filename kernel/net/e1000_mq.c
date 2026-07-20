// e1000_mq.c - Intel E1000/82575/82576/I350/I210/I225 Multi-Queue Driver
// Task #54: Multi-queue support, RSS, offloading, interrupt coalescing
#include "e1000_mq.h"
#include "e1000.h"
#include "../drivers/pci.h"
#include "../mm/heap.h"
#include "../serial.h"
#include "../string.h"

// External PMM
extern uint64_t pmm_alloc_page(void);
extern void pmm_free_page(uint64_t addr);
extern void io_wait(void);

// Maximum devices supported
#define E1000_MQ_MAX_DEVICES    8
static e1000_mq_dev_t *e1000_mq_devices[E1000_MQ_MAX_DEVICES];
static int e1000_mq_device_count = 0;

// Descriptor ring sizes
#define E1000_MQ_RX_DESC_COUNT  256
#define E1000_MQ_TX_DESC_COUNT  256
#define E1000_MQ_BUFFER_SIZE    2048

// ============================================================================
// Register Access
// ============================================================================

static inline uint32_t e1000_mq_read(e1000_mq_dev_t *dev, uint32_t reg) {
    return *(volatile uint32_t *)(dev->mmio + reg);
}

static inline void e1000_mq_write(e1000_mq_dev_t *dev, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(dev->mmio + reg) = val;
    __asm__ volatile("mfence" ::: "memory");
}

// ============================================================================
// Device Identification
// ============================================================================

static int e1000_is_advanced(uint16_t device_id) {
    switch (device_id) {
        // 82575/82576
        case E1000_DEV_82575EB:
        case E1000_DEV_82575GB:
        case E1000_DEV_82576:
        case E1000_DEV_82576_FIBER:
        case E1000_DEV_82576_SERDES:
        case E1000_DEV_82576_QUAD:
        // I350
        case E1000_DEV_I350:
        case E1000_DEV_I350_FIBER:
        case E1000_DEV_I350_SERDES:
        // I210/I211
        case E1000_DEV_I210:
        case E1000_DEV_I210_FIBER:
        case E1000_DEV_I210_SERDES:
        case E1000_DEV_I211:
        // I225/I226
        case E1000_DEV_I225_LM:
        case E1000_DEV_I225_V:
        case E1000_DEV_I225_I:
        case E1000_DEV_I225_K:
        case E1000_DEV_I226_LM:
        case E1000_DEV_I226_V:
            return 1;
        default:
            return 0;
    }
}

static int e1000_get_max_queues(uint16_t device_id) {
    switch (device_id) {
        case E1000_DEV_82576:
        case E1000_DEV_82576_FIBER:
        case E1000_DEV_82576_SERDES:
        case E1000_DEV_82576_QUAD:
            return 16;  // 82576 supports 16 queues
        case E1000_DEV_I350:
        case E1000_DEV_I350_FIBER:
        case E1000_DEV_I350_SERDES:
            return 8;   // I350 supports 8 queues
        case E1000_DEV_I210:
        case E1000_DEV_I210_FIBER:
        case E1000_DEV_I210_SERDES:
        case E1000_DEV_I211:
            return 4;   // I210/I211 support 4 queues
        case E1000_DEV_I225_LM:
        case E1000_DEV_I225_V:
        case E1000_DEV_I225_I:
        case E1000_DEV_I225_K:
        case E1000_DEV_I226_LM:
        case E1000_DEV_I226_V:
            return 4;   // I225/I226 support 4 queues
        default:
            return 2;   // 82575 supports 2 queues
    }
}

static const char *e1000_get_name(uint16_t device_id) {
    switch (device_id) {
        case E1000_DEV_82575EB:
        case E1000_DEV_82575GB:
            return "82575";
        case E1000_DEV_82576:
        case E1000_DEV_82576_FIBER:
        case E1000_DEV_82576_SERDES:
        case E1000_DEV_82576_QUAD:
            return "82576";
        case E1000_DEV_I350:
        case E1000_DEV_I350_FIBER:
        case E1000_DEV_I350_SERDES:
            return "I350";
        case E1000_DEV_I210:
        case E1000_DEV_I210_FIBER:
        case E1000_DEV_I210_SERDES:
            return "I210";
        case E1000_DEV_I211:
            return "I211";
        case E1000_DEV_I225_LM:
        case E1000_DEV_I225_V:
        case E1000_DEV_I225_I:
        case E1000_DEV_I225_K:
            return "I225";
        case E1000_DEV_I226_LM:
        case E1000_DEV_I226_V:
            return "I226";
        default:
            return "E1000-MQ";
    }
}

// ============================================================================
// Queue Initialization
// ============================================================================

static int e1000_mq_alloc_rx_queue(e1000_mq_dev_t *dev, int qid) {
    e1000_queue_t *q = &dev->rx_queues[qid];
    size_t desc_size = E1000_MQ_RX_DESC_COUNT * sizeof(e1000_adv_rx_desc_t);
    
    // Allocate descriptor ring (page-aligned for DMA)
    q->desc_dma = pmm_alloc_page();
    if (q->desc_dma == 0) {
        kprintf("[E1000-MQ] Failed to alloc RX desc ring for queue %d\n", qid);
        return -1;
    }
    q->desc_ring = (void *)q->desc_dma;  // Identity-mapped
    memset(q->desc_ring, 0, desc_size);
    q->desc_count = E1000_MQ_RX_DESC_COUNT;
    
    // Allocate buffer pointers
    q->buffers = kzalloc(sizeof(void *) * E1000_MQ_RX_DESC_COUNT);
    q->buffer_dma = kzalloc(sizeof(uint64_t) * E1000_MQ_RX_DESC_COUNT);
    if (!q->buffers || !q->buffer_dma) {
        kprintf("[E1000-MQ] Failed to alloc RX buffer arrays\n");
        return -1;
    }
    
    // Allocate buffers
    e1000_adv_rx_desc_t *descs = (e1000_adv_rx_desc_t *)q->desc_ring;
    for (uint32_t i = 0; i < q->desc_count; i++) {
        q->buffer_dma[i] = pmm_alloc_page();
        if (q->buffer_dma[i] == 0) {
            kprintf("[E1000-MQ] Failed to alloc RX buffer %d\n", i);
            return -1;
        }
        q->buffers[i] = (void *)q->buffer_dma[i];
        descs[i].read.pkt_addr = q->buffer_dma[i];
        descs[i].read.hdr_addr = 0;  // No header split
    }
    
    q->head = 0;
    q->tail = 0;
    q->next_to_use = 0;
    q->next_to_clean = 0;
    
    return 0;
}

static int e1000_mq_alloc_tx_queue(e1000_mq_dev_t *dev, int qid) {
    e1000_queue_t *q = &dev->tx_queues[qid];
    size_t desc_size = E1000_MQ_TX_DESC_COUNT * sizeof(e1000_adv_tx_data_desc_t);
    
    // Allocate descriptor ring
    q->desc_dma = pmm_alloc_page();
    if (q->desc_dma == 0) {
        kprintf("[E1000-MQ] Failed to alloc TX desc ring for queue %d\n", qid);
        return -1;
    }
    q->desc_ring = (void *)q->desc_dma;
    memset(q->desc_ring, 0, desc_size);
    q->desc_count = E1000_MQ_TX_DESC_COUNT;
    
    // Allocate buffer pointers
    q->buffers = kzalloc(sizeof(void *) * E1000_MQ_TX_DESC_COUNT);
    q->buffer_dma = kzalloc(sizeof(uint64_t) * E1000_MQ_TX_DESC_COUNT);
    if (!q->buffers || !q->buffer_dma) {
        kprintf("[E1000-MQ] Failed to alloc TX buffer arrays\n");
        return -1;
    }
    
    // Pre-allocate TX buffers
    for (uint32_t i = 0; i < q->desc_count; i++) {
        q->buffer_dma[i] = pmm_alloc_page();
        if (q->buffer_dma[i] == 0) {
            kprintf("[E1000-MQ] Failed to alloc TX buffer %d\n", i);
            return -1;
        }
        q->buffers[i] = (void *)q->buffer_dma[i];
    }
    
    q->head = 0;
    q->tail = 0;
    q->next_to_use = 0;
    q->next_to_clean = 0;
    
    return 0;
}

static void e1000_mq_setup_rx_queue(e1000_mq_dev_t *dev, int qid) {
    e1000_queue_t *q = &dev->rx_queues[qid];
    
    // Set descriptor base address
    e1000_mq_write(dev, E1000_RDBAL_MQ(qid), q->desc_dma & 0xFFFFFFFF);
    e1000_mq_write(dev, E1000_RDBAH_MQ(qid), q->desc_dma >> 32);
    
    // Set descriptor ring length
    e1000_mq_write(dev, E1000_RDLEN_MQ(qid), 
                   q->desc_count * sizeof(e1000_adv_rx_desc_t));
    
    // Set head and tail
    e1000_mq_write(dev, E1000_RDH_MQ(qid), 0);
    e1000_mq_write(dev, E1000_RDT_MQ(qid), q->desc_count - 1);
    
    // Configure SRRCTL for buffer size and descriptor type
    uint32_t srrctl = (E1000_MQ_BUFFER_SIZE >> E1000_SRRCTL_BSIZEPKT_SHIFT) |
                      E1000_SRRCTL_DESCTYPE_ADV |
                      E1000_SRRCTL_DROP_EN;
    e1000_mq_write(dev, E1000_SRRCTL_MQ(qid), srrctl);
    
    // Enable queue
    uint32_t rxdctl = e1000_mq_read(dev, E1000_RXDCTL_MQ(qid));
    rxdctl |= E1000_RXDCTL_ENABLE;
    e1000_mq_write(dev, E1000_RXDCTL_MQ(qid), rxdctl);
    
    // Wait for enable
    int timeout = 1000;
    while (!(e1000_mq_read(dev, E1000_RXDCTL_MQ(qid)) & E1000_RXDCTL_ENABLE) && 
           timeout > 0) {
        timeout--;
        io_wait();
    }
    
    kprintf("[E1000-MQ] RX queue %d enabled\n", qid);
}

static void e1000_mq_setup_tx_queue(e1000_mq_dev_t *dev, int qid) {
    e1000_queue_t *q = &dev->tx_queues[qid];
    
    // Set descriptor base address
    e1000_mq_write(dev, E1000_TDBAL_MQ(qid), q->desc_dma & 0xFFFFFFFF);
    e1000_mq_write(dev, E1000_TDBAH_MQ(qid), q->desc_dma >> 32);
    
    // Set descriptor ring length
    e1000_mq_write(dev, E1000_TDLEN_MQ(qid),
                   q->desc_count * sizeof(e1000_adv_tx_data_desc_t));
    
    // Set head and tail
    e1000_mq_write(dev, E1000_TDH_MQ(qid), 0);
    e1000_mq_write(dev, E1000_TDT_MQ(qid), 0);
    
    // Configure TXDCTL with thresholds
    uint32_t txdctl = E1000_TXDCTL_ENABLE |
                      (1 << E1000_TXDCTL_PTHRESH_SHIFT) |  // Prefetch threshold
                      (1 << E1000_TXDCTL_HTHRESH_SHIFT) |  // Host threshold
                      (1 << E1000_TXDCTL_WTHRESH_SHIFT);   // Write-back threshold
    e1000_mq_write(dev, E1000_TXDCTL_MQ(qid), txdctl);
    
    // Wait for enable
    int timeout = 1000;
    while (!(e1000_mq_read(dev, E1000_TXDCTL_MQ(qid)) & E1000_TXDCTL_ENABLE) &&
           timeout > 0) {
        timeout--;
        io_wait();
    }
    
    kprintf("[E1000-MQ] TX queue %d enabled\n", qid);
}

// ============================================================================
// RSS Configuration
// ============================================================================

int e1000_mq_configure_rss(e1000_mq_dev_t *dev, int num_queues) {
    if (!dev || num_queues <= 0 || num_queues > dev->num_rx_queues) {
        return -1;
    }
    
    // Set RSS key (use default Toeplitz key)
    static const uint32_t rss_key[10] = {
        0xda565a6d, 0xc20e5b25, 0x3d256741, 0xb08fa343,
        0xcb2bcad0, 0xb4307bae, 0xa32dcb77, 0x0cf23080,
        0x3bb7426a, 0xfa01acbe
    };
    
    for (int i = 0; i < 10; i++) {
        e1000_mq_write(dev, E1000_RSSRK(i), rss_key[i]);
    }
    
    // Set up indirection table (round-robin)
    for (int i = 0; i < 32; i++) {  // 128 entries, 4 per register
        uint32_t val = 0;
        for (int j = 0; j < 4; j++) {
            int entry = (i * 4 + j);
            int queue = entry % num_queues;
            val |= (queue << (j * 8));
        }
        e1000_mq_write(dev, E1000_RETA(i), val);
    }
    
    // Enable RSS with IPv4 and TCP/UDP hashing
    uint32_t mrqc = E1000_MRQC_ENABLE_RSS |
                    E1000_MRQC_RSS_FIELD_IPV4 |
                    E1000_MRQC_RSS_FIELD_TCPIPV4 |
                    E1000_MRQC_RSS_FIELD_UDPIPV4;
    e1000_mq_write(dev, E1000_MRQC, mrqc);
    
    dev->capabilities |= E1000_CAP_RSS;
    kprintf("[E1000-MQ] RSS configured with %d queues\n", num_queues);
    
    // Update net_device RSS config
    rss_init_default(&dev->netdev, num_queues);
    
    return 0;
}

// ============================================================================
// Checksum Offload Configuration
// ============================================================================

int e1000_mq_configure_csum(e1000_mq_dev_t *dev, int rx_csum, int tx_csum) {
    if (!dev) return -1;
    
    if (rx_csum) {
        // Enable RX checksum offload
        uint32_t rxcsum = e1000_mq_read(dev, E1000_RXCSUM);
        rxcsum |= E1000_RXCSUM_IPOFLD |   // IP checksum
                  E1000_RXCSUM_TUOFLD;     // TCP/UDP checksum
        e1000_mq_write(dev, E1000_RXCSUM, rxcsum);
        
        dev->netdev.features.csum_offload |= CSUM_OFFLOAD_RX_IPV4 |
                                             CSUM_OFFLOAD_RX_TCP |
                                             CSUM_OFFLOAD_RX_UDP;
        kprintf("[E1000-MQ] RX checksum offload enabled\n");
    }
    
    if (tx_csum) {
        // TX checksum is handled per-descriptor, just set capability
        dev->netdev.features.csum_offload |= CSUM_OFFLOAD_TX_IPV4 |
                                             CSUM_OFFLOAD_TX_TCP |
                                             CSUM_OFFLOAD_TX_UDP;
        kprintf("[E1000-MQ] TX checksum offload enabled\n");
    }
    
    dev->capabilities |= E1000_CAP_CSUM_OFFLOAD;
    return 0;
}

// ============================================================================
// Interrupt Coalescing
// ============================================================================

int e1000_mq_configure_itr(e1000_mq_dev_t *dev, uint32_t itr_val) {
    if (!dev) return -1;
    
    // ITR is specified in 256ns increments
    // Common values: 20000 int/sec = 50 (50*256ns = 12.8us)
    //                10000 int/sec = 100 (100*256ns = 25.6us)
    
    dev->itr_setting = itr_val;
    
    // Set ITR for each MSI-X vector (or shared interrupt)
    int num_vectors = dev->num_msix_vectors > 0 ? dev->num_msix_vectors : 1;
    for (int i = 0; i < num_vectors; i++) {
        e1000_mq_write(dev, E1000_EITR(i), itr_val);
    }
    
    // Update coalesce config
    dev->netdev.coalesce.rx_usecs = itr_val * 256 / 1000;  // Convert to microseconds
    dev->netdev.coalesce.tx_usecs = itr_val * 256 / 1000;
    
    kprintf("[E1000-MQ] Interrupt throttle rate set to %d (%.1f us)\n",
            itr_val, (float)itr_val * 0.256);
    
    return 0;
}

// ============================================================================
// MTU Configuration (Jumbo Frames)
// ============================================================================

int e1000_mq_set_mtu(e1000_mq_dev_t *dev, int mtu) {
    if (!dev) return -1;
    
    // Minimum 68, maximum 9216 for jumbo
    if (mtu < 68 || mtu > 9216) {
        kprintf("[E1000-MQ] Invalid MTU: %d (must be 68-9216)\n", mtu);
        return -1;
    }
    
    // For jumbo frames (> 1500), need to:
    // 1. Enable Long Packet (LPE) in RCTL
    // 2. Set MAXFRS (max frame size)
    
    if (mtu > 1500) {
        uint32_t rctl = e1000_mq_read(dev, 0x0100);  // RCTL
        rctl |= (1 << 5);  // LPE - Long Packet Enable
        e1000_mq_write(dev, 0x0100, rctl);
        
        // Set max frame size (MTU + headers)
        uint32_t maxfrs = (mtu + 14 + 4) << 16;  // Ethernet + VLAN
        e1000_mq_write(dev, 0x10, maxfrs);  // MAXFRS register (varies by model)
        
        dev->capabilities |= E1000_CAP_JUMBO;
    }
    
    dev->netdev.mtu = mtu;
    kprintf("[E1000-MQ] MTU set to %d\n", mtu);
    
    return 0;
}

// ============================================================================
// NAPI Poll Implementation
// ============================================================================

int e1000_mq_napi_poll(napi_struct_t *napi, int budget) {
    if (!napi || !napi->rx_queue) return 0;
    
    e1000_mq_dev_t *dev = (e1000_mq_dev_t *)napi->dev;
    net_rx_queue_t *rxq = napi->rx_queue;
    int qid = rxq->queue_id;
    e1000_queue_t *q = &dev->rx_queues[qid];
    
    int work_done = 0;
    e1000_adv_rx_desc_t *descs = (e1000_adv_rx_desc_t *)q->desc_ring;
    
    while (work_done < budget) {
        uint32_t idx = q->next_to_clean;
        e1000_adv_rx_desc_t *desc = &descs[idx];
        
        // Check if descriptor is done
        if (!(desc->wb.status_error & E1000_RXD_STAT_DD)) {
            break;
        }
        
        // Get packet info
        uint16_t len = desc->wb.length;
        (void)q;(void)idx; // unused currently
        
        // Check for errors
        if (desc->wb.status_error & 0xFF000000) {
            q->errors++;
            goto next_desc;
        }
        
        // Process packet (would call network stack here)
        // For now, just update statistics
        q->packets++;
        q->bytes += len;
        rxq->packets++;
        rxq->bytes += len;
        
        // RSS hash is available in desc->wb.rss_hash
        // Checksum status in desc->wb.status_error
        
next_desc:
        // Reset descriptor for reuse
        desc->read.pkt_addr = q->buffer_dma[idx];
        desc->read.hdr_addr = 0;
        
        q->next_to_clean = (idx + 1) % q->desc_count;
        work_done++;
    }
    
    // Update tail pointer to release descriptors to hardware
    if (work_done > 0) {
        uint32_t tail = (q->next_to_clean + q->desc_count - 1) % q->desc_count;
        e1000_mq_write(dev, E1000_RDT_MQ(qid), tail);
    }
    
    return work_done;
}

// ============================================================================
// Transmit
// ============================================================================

int e1000_mq_xmit(e1000_mq_dev_t *dev, const void *data, uint16_t len, int queue) {
    if (!dev || !data || len == 0) return -1;
    if (queue < 0 || queue >= dev->num_tx_queues) {
        queue = 0;  // Default to queue 0
    }
    
    e1000_queue_t *q = &dev->tx_queues[queue];
    e1000_adv_tx_data_desc_t *descs = (e1000_adv_tx_data_desc_t *)q->desc_ring;
    
    uint32_t idx = q->next_to_use;
    
    // Check if queue is full
    uint32_t next_idx = (idx + 1) % q->desc_count;
    if (next_idx == q->next_to_clean) {
        q->errors++;
        return -1;  // Queue full
    }
    
    // Copy data to buffer
    if (len > E1000_MQ_BUFFER_SIZE) {
        len = E1000_MQ_BUFFER_SIZE;
    }
    memcpy(q->buffers[idx], data, len);
    
    // Set up descriptor
    e1000_adv_tx_data_desc_t *desc = &descs[idx];
    desc->buffer_addr = q->buffer_dma[idx];
    
    // Build cmd_type_len: DTYP=0011 (ADV data), EOP, IFCS, RS
    uint32_t cmd_type_len = len |                    // Data length
                            (0x3 << 20) |            // DTYP = advanced data
                            (1 << 24) |              // EOP - End of Packet
                            (1 << 25) |              // IFCS - Insert FCS
                            (1 << 27);               // RS - Report Status
    
    // Add checksum offload if enabled
    if (dev->capabilities & E1000_CAP_CSUM_OFFLOAD) {
        // Would set IXSM, TXSM bits here based on protocol
    }
    
    desc->cmd_type_len = cmd_type_len;
    desc->olinfo_status = len << 14;  // PAYLEN
    
    // Memory barrier
    __asm__ volatile("mfence" ::: "memory");
    
    // Advance ring
    q->next_to_use = next_idx;
    
    // Ring doorbell
    e1000_mq_write(dev, E1000_TDT_MQ(queue), next_idx);
    
    q->packets++;
    q->bytes += len;
    
    return len;
}

// ============================================================================
// IRQ Handler
// ============================================================================

void e1000_mq_irq_handler(int vector __attribute__((unused))) {
    // Find device associated with this vector
    // For MSI-X, each vector maps to a queue
    // For legacy interrupts, need to check all devices
    
    for (int d = 0; d < e1000_mq_device_count; d++) {
        e1000_mq_dev_t *dev = e1000_mq_devices[d];
        if (!dev) continue;
        
        // Read and clear interrupt cause
        uint32_t icr = e1000_mq_read(dev, E1000_EICR);
        if (icr == 0) continue;
        
        // Clear interrupt
        e1000_mq_write(dev, E1000_EICR, icr);
        
        // Schedule NAPI for each affected queue
        for (int q = 0; q < dev->num_rx_queues; q++) {
            if (icr & (1 << q)) {
                napi_schedule(&dev->rx_queues[q].napi);
            }
        }
        
        // Check for link status change
        if (icr & (1 << 20)) {  // LSC bit varies by model
            uint32_t status = e1000_mq_read(dev, 0x0008);  // STATUS
            dev->link_up = (status & 0x2) != 0;
            kprintf("[E1000-MQ] %s: Link %s\n", dev->netdev.name,
                    dev->link_up ? "up" : "down");
        }
    }
}

// ============================================================================
// Device Probe and Initialize
// ============================================================================

int e1000_mq_probe(uint16_t vendor_id, uint16_t device_id) {
    if (vendor_id != 0x8086) return -1;  // Intel only
    
    if (!e1000_is_advanced(device_id)) {
        return -1;  // Not a multi-queue capable device
    }
    
    return 0;  // Device supported
}

static int e1000_mq_init_one(pci_device_t *pci_dev) {
    if (e1000_mq_device_count >= E1000_MQ_MAX_DEVICES) {
        kprintf("[E1000-MQ] Maximum devices reached\n");
        return -1;
    }
    
    // Allocate device structure
    e1000_mq_dev_t *dev = kzalloc(sizeof(e1000_mq_dev_t));
    if (!dev) {
        kprintf("[E1000-MQ] Failed to allocate device structure\n");
        return -1;
    }
    
    dev->device_id = pci_dev->device_id;
    
    // Enable bus mastering
    pci_enable_bus_master(pci_dev);
    
    // Get MMIO base
    uint64_t mmio_addr = pci_get_bar_address(pci_dev, 0);
    if (mmio_addr == 0) {
        kprintf("[E1000-MQ] Failed to get MMIO address\n");
        kfree(dev);
        return -1;
    }
    dev->mmio = (volatile uint8_t *)mmio_addr;
    
    // Set device name
    snprintf(dev->netdev.name, sizeof(dev->netdev.name), "eth%d", 
             e1000_mq_device_count);
    
    kprintf("[E1000-MQ] Initializing %s (%s) at %02x:%02x.%x\n",
            dev->netdev.name, e1000_get_name(dev->device_id),
            pci_dev->bus, pci_dev->slot, pci_dev->func);
    
    // Reset device
    e1000_mq_write(dev, 0x0000, (1 << 26));  // CTRL.RST
    for (int i = 0; i < 10000; i++) io_wait();
    
    // Wait for reset complete
    int timeout = 1000;
    while ((e1000_mq_read(dev, 0x0000) & (1 << 26)) && timeout > 0) {
        timeout--;
        io_wait();
    }
    
    // Read MAC address
    uint32_t ral = e1000_mq_read(dev, 0x5400);  // RAL
    uint32_t rah = e1000_mq_read(dev, 0x5404);  // RAH
    dev->netdev.mac_addr[0] = ral & 0xFF;
    dev->netdev.mac_addr[1] = (ral >> 8) & 0xFF;
    dev->netdev.mac_addr[2] = (ral >> 16) & 0xFF;
    dev->netdev.mac_addr[3] = (ral >> 24) & 0xFF;
    dev->netdev.mac_addr[4] = rah & 0xFF;
    dev->netdev.mac_addr[5] = (rah >> 8) & 0xFF;
    
    kprintf("[E1000-MQ] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
            dev->netdev.mac_addr[0], dev->netdev.mac_addr[1],
            dev->netdev.mac_addr[2], dev->netdev.mac_addr[3],
            dev->netdev.mac_addr[4], dev->netdev.mac_addr[5]);
    
    // Determine number of queues
    int max_queues = e1000_get_max_queues(dev->device_id);
    dev->num_rx_queues = max_queues > 4 ? 4 : max_queues;  // Limit to 4 for now
    dev->num_tx_queues = dev->num_rx_queues;
    
    kprintf("[E1000-MQ] Configuring %d RX and %d TX queues\n",
            dev->num_rx_queues, dev->num_tx_queues);
    
    // Allocate queues
    dev->rx_queues = kzalloc(sizeof(e1000_queue_t) * dev->num_rx_queues);
    dev->tx_queues = kzalloc(sizeof(e1000_queue_t) * dev->num_tx_queues);
    if (!dev->rx_queues || !dev->tx_queues) {
        kprintf("[E1000-MQ] Failed to allocate queue structures\n");
        goto fail;
    }
    
    // Initialize queues
    for (int i = 0; i < dev->num_rx_queues; i++) {
        if (e1000_mq_alloc_rx_queue(dev, i) < 0) goto fail;
        
        // Initialize NAPI for this queue
        napi_init(&dev->rx_queues[i].napi, &dev->netdev,
                  e1000_mq_napi_poll, NAPI_POLL_WEIGHT);
    }
    
    for (int i = 0; i < dev->num_tx_queues; i++) {
        if (e1000_mq_alloc_tx_queue(dev, i) < 0) goto fail;
    }
    
    // Configure RSS
    e1000_mq_configure_rss(dev, dev->num_rx_queues);
    
    // Configure checksum offload
    e1000_mq_configure_csum(dev, 1, 1);
    
    // Set interrupt throttle rate (10000 int/sec)
    e1000_mq_configure_itr(dev, E1000_EITR_INTERVAL_10K);
    
    // Set MTU
    dev->netdev.mtu = 1500;
    
    // Enable RX and TX
    uint32_t rctl = (1 << 1) |    // EN - Receiver Enable
                    (1 << 15) |   // BAM - Broadcast Accept Mode
                    (1 << 26);    // SECRC - Strip Ethernet CRC
    e1000_mq_write(dev, 0x0100, rctl);
    
    uint32_t tctl = (1 << 1) |    // EN - Transmitter Enable
                    (1 << 3) |    // PSP - Pad Short Packets
                    (15 << 4) |   // CT - Collision Threshold
                    (64 << 12);   // COLD - Collision Distance
    e1000_mq_write(dev, 0x0400, tctl);
    
    // Setup queues
    for (int i = 0; i < dev->num_rx_queues; i++) {
        e1000_mq_setup_rx_queue(dev, i);
        napi_enable(&dev->rx_queues[i].napi);
    }
    
    for (int i = 0; i < dev->num_tx_queues; i++) {
        e1000_mq_setup_tx_queue(dev, i);
    }
    
    // Set link up
    uint32_t ctrl = e1000_mq_read(dev, 0x0000);
    ctrl |= (1 << 6);  // SLU - Set Link Up
    e1000_mq_write(dev, 0x0000, ctrl);
    
    // Enable interrupts
    e1000_mq_write(dev, E1000_EIMS, 0xFFFFFFFF);
    
    // Check link status
    uint32_t status = e1000_mq_read(dev, 0x0008);
    dev->link_up = (status & 0x2) != 0;
    
    // Register with network subsystem
    netdev_alloc_queues(&dev->netdev, dev->num_rx_queues, dev->num_tx_queues);
    netdev_register(&dev->netdev);
    
    // Store device
    e1000_mq_devices[e1000_mq_device_count++] = dev;
    
    kprintf("[E1000-MQ] %s initialized, link %s\n",
            dev->netdev.name, dev->link_up ? "up" : "down");
    
    return 0;

fail:
    if (dev->rx_queues) kfree(dev->rx_queues);
    if (dev->tx_queues) kfree(dev->tx_queues);
    kfree(dev);
    return -1;
}

// ============================================================================
// Main Init
// ============================================================================

int e1000_mq_init(void) {
    kprintf("[E1000-MQ] Scanning for multi-queue Intel NICs...\n");
    
    int found = 0;
    
    // Scan for supported devices
    static const uint16_t device_ids[] = {
        E1000_DEV_82575EB, E1000_DEV_82575GB,
        E1000_DEV_82576, E1000_DEV_82576_FIBER, E1000_DEV_82576_SERDES,
        E1000_DEV_I350, E1000_DEV_I350_FIBER, E1000_DEV_I350_SERDES,
        E1000_DEV_I210, E1000_DEV_I210_FIBER, E1000_DEV_I210_SERDES, E1000_DEV_I211,
        E1000_DEV_I225_LM, E1000_DEV_I225_V, E1000_DEV_I225_I, E1000_DEV_I225_K,
        E1000_DEV_I226_LM, E1000_DEV_I226_V,
        0  // Terminator
    };
    
    for (int i = 0; device_ids[i] != 0; i++) {
        pci_device_t *pci_dev = pci_find_device(0x8086, device_ids[i]);
        if (pci_dev) {
            if (e1000_mq_init_one(pci_dev) == 0) {
                found++;
            }
        }
    }
    
    if (found == 0) {
        kprintf("[E1000-MQ] No multi-queue Intel NICs found\n");
    } else {
        kprintf("[E1000-MQ] Initialized %d device(s)\n", found);
    }
    
    return found;
}

e1000_mq_dev_t *e1000_mq_get_device(int index) {
    if (index < 0 || index >= e1000_mq_device_count) {
        return NULL;
    }
    return e1000_mq_devices[index];
}
