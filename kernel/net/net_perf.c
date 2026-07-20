// net_perf.c - Network Performance Infrastructure Implementation
// Task #54: Multi-queue, RSS, offloading, NAPI-style polling
#include "net_perf.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"

// External PMM for DMA-safe allocations
extern uint64_t pmm_alloc_page(void);
extern void pmm_free_page(uint64_t addr);

// List of registered network devices
static net_device_t *netdev_list = NULL;

// List of scheduled NAPI contexts
static napi_struct_t *napi_scheduled_list = NULL;
static volatile uint32_t napi_lock = 0;

// Simple spinlock helpers
static inline void spin_lock(volatile uint32_t *lock) {
    while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause" ::: "memory");
    }
}

static inline void spin_unlock(volatile uint32_t *lock) {
    __atomic_clear(lock, __ATOMIC_RELEASE);
}

// ============================================================================
// Multi-queue Support
// ============================================================================

int netdev_alloc_queues(net_device_t *dev, int num_rx, int num_tx) {
    if (!dev || num_rx <= 0 || num_tx <= 0) {
        return -1;
    }
    
    if (num_rx > MAX_NET_QUEUES || num_tx > MAX_NET_QUEUES) {
        kprintf("[NET_PERF] Queue count exceeds maximum (%d)\n", MAX_NET_QUEUES);
        return -1;
    }
    
    // Allocate RX queues
    dev->rx_queues = kzalloc(sizeof(net_rx_queue_t) * num_rx);
    if (!dev->rx_queues) {
        kprintf("[NET_PERF] Failed to allocate RX queues\n");
        return -1;
    }
    
    // Allocate TX queues
    dev->tx_queues = kzalloc(sizeof(net_tx_queue_t) * num_tx);
    if (!dev->tx_queues) {
        kfree(dev->rx_queues);
        dev->rx_queues = NULL;
        kprintf("[NET_PERF] Failed to allocate TX queues\n");
        return -1;
    }
    
    dev->num_rx_queues = num_rx;
    dev->num_tx_queues = num_tx;
    
    // Initialize queue structures
    for (int i = 0; i < num_rx; i++) {
        dev->rx_queues[i].queue_id = i;
        dev->rx_queues[i].dev = dev;
        dev->rx_queues[i].cpu_id = i % 4;  // Round-robin CPU assignment
        dev->rx_queues[i].state = 0;
    }
    
    for (int i = 0; i < num_tx; i++) {
        dev->tx_queues[i].queue_id = i;
        dev->tx_queues[i].dev = dev;
        dev->tx_queues[i].cpu_id = i % 4;
        dev->tx_queues[i].state = 0;
        dev->tx_queues[i].lock = 0;
    }
    
    kprintf("[NET_PERF] Allocated %d RX and %d TX queues for %s\n",
            num_rx, num_tx, dev->name);
    
    return 0;
}

void netdev_free_queues(net_device_t *dev) {
    if (!dev) return;
    
    // Free RX queue buffers
    if (dev->rx_queues) {
        for (uint32_t i = 0; i < dev->num_rx_queues; i++) {
            net_rx_queue_t *rxq = &dev->rx_queues[i];
            if (rxq->rx_buffers) {
                for (uint32_t j = 0; j < rxq->num_descriptors; j++) {
                    if (rxq->rx_buffer_dma && rxq->rx_buffer_dma[j]) {
                        pmm_free_page(rxq->rx_buffer_dma[j]);
                    }
                }
                kfree(rxq->rx_buffers);
                kfree(rxq->rx_buffer_dma);
            }
        }
        kfree(dev->rx_queues);
        dev->rx_queues = NULL;
    }
    
    // Free TX queue buffers
    if (dev->tx_queues) {
        for (uint32_t i = 0; i < dev->num_tx_queues; i++) {
            net_tx_queue_t *txq = &dev->tx_queues[i];
            if (txq->tx_buffers) {
                for (uint32_t j = 0; j < txq->num_descriptors; j++) {
                    if (txq->tx_buffer_dma && txq->tx_buffer_dma[j]) {
                        pmm_free_page(txq->tx_buffer_dma[j]);
                    }
                }
                kfree(txq->tx_buffers);
                kfree(txq->tx_buffer_dma);
            }
        }
        kfree(dev->tx_queues);
        dev->tx_queues = NULL;
    }
    
    dev->num_rx_queues = 0;
    dev->num_tx_queues = 0;
}

// Simple hash for queue selection (Toeplitz-like)
static uint32_t simple_hash(const uint8_t *data, int len) {
    uint32_t hash = 0;
    for (int i = 0; i < len; i++) {
        hash = hash * 31 + data[i];
    }
    return hash;
}

int netdev_select_queue(net_device_t *dev, const void *data, uint16_t len) {
    if (!dev || dev->num_tx_queues == 0) {
        return 0;
    }
    
    // If device has custom queue selection, use it
    if (dev->ops && dev->ops->select_queue) {
        return dev->ops->select_queue(dev, data, len);
    }
    
    // Default: hash based on packet data (typically IP/TCP headers)
    const uint8_t *pkt = (const uint8_t *)data;
    
    // Skip Ethernet header (14 bytes) to get to IP header
    if (len >= 34) {  // Ethernet + IP minimum
        // Hash on IP src/dst addresses (bytes 12-19 of IP header = bytes 26-33 of frame)
        uint32_t hash = simple_hash(pkt + 26, 8);
        return hash % dev->num_tx_queues;
    }
    
    // Fallback: use packet length as hash seed
    return len % dev->num_tx_queues;
}

// ============================================================================
// NAPI Implementation
// ============================================================================

void napi_init(napi_struct_t *napi, net_device_t *dev,
               int (*poll)(napi_struct_t *, int), int weight) {
    if (!napi) return;
    
    memset(napi, 0, sizeof(napi_struct_t));
    napi->dev = dev;
    napi->poll = poll;
    napi->weight = weight > 0 ? weight : NAPI_POLL_WEIGHT;
    napi->state = NAPI_STATE_DISABLE;
    napi->next = NULL;
}

void napi_enable(napi_struct_t *napi) {
    if (!napi) return;
    
    __atomic_and_fetch(&napi->state, ~NAPI_STATE_DISABLE, __ATOMIC_SEQ_CST);
    kprintf("[NAPI] Enabled NAPI context for %s\n", 
            napi->dev ? napi->dev->name : "unknown");
}

void napi_disable(napi_struct_t *napi) {
    if (!napi) return;
    
    // Wait for any in-progress poll to complete
    while (napi->state & NAPI_STATE_SCHED) {
        __asm__ volatile("pause" ::: "memory");
    }
    
    __atomic_or_fetch(&napi->state, NAPI_STATE_DISABLE, __ATOMIC_SEQ_CST);
}

void napi_schedule(napi_struct_t *napi) {
    if (!napi) return;
    
    // Check if already scheduled or disabled
    uint32_t old_state = __atomic_fetch_or(&napi->state, NAPI_STATE_SCHED, 
                                            __ATOMIC_SEQ_CST);
    if (old_state & (NAPI_STATE_SCHED | NAPI_STATE_DISABLE)) {
        return;  // Already scheduled or disabled
    }
    
    // Add to scheduled list
    spin_lock(&napi_lock);
    napi->next = napi_scheduled_list;
    napi_scheduled_list = napi;
    spin_unlock(&napi_lock);
}

void napi_complete(napi_struct_t *napi) {
    if (!napi) return;
    
    // Remove from scheduled list
    spin_lock(&napi_lock);
    napi_struct_t **pp = &napi_scheduled_list;
    while (*pp) {
        if (*pp == napi) {
            *pp = napi->next;
            break;
        }
        pp = &(*pp)->next;
    }
    spin_unlock(&napi_lock);
    
    // Clear scheduled flag
    __atomic_and_fetch(&napi->state, ~NAPI_STATE_SCHED, __ATOMIC_SEQ_CST);
}

void napi_poll_all(void) {
    napi_struct_t *napi;
    
    // Process all scheduled NAPI contexts
    spin_lock(&napi_lock);
    napi = napi_scheduled_list;
    spin_unlock(&napi_lock);
    
    while (napi) {
        napi_struct_t *next = napi->next;
        
        if (!(napi->state & NAPI_STATE_DISABLE) && napi->poll) {
            napi->budget = napi->weight;
            int work_done = napi->poll(napi, napi->budget);
            
            // If we didn't exhaust budget, we're done
            if (work_done < napi->budget) {
                napi_complete(napi);
                
                // Re-enable interrupts for this queue
                // (Device-specific, would be handled by driver)
            }
        }
        
        napi = next;
    }
}

// ============================================================================
// RSS Implementation
// ============================================================================

// Default Toeplitz hash key (from Microsoft RSS spec)
static const uint8_t default_rss_key[40] = {
    0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2,
    0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0,
    0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4,
    0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c,
    0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa
};

int rss_init_default(net_device_t *dev, int num_queues) {
    if (!dev || num_queues <= 0 || num_queues > MAX_NET_QUEUES) {
        return -1;
    }
    
    // Set default hash types
    dev->rss.hash_types = RSS_HASH_TYPE_IPV4 | RSS_HASH_TYPE_TCPV4 | 
                          RSS_HASH_TYPE_UDPV4;
    
    // Copy default key
    memcpy(dev->rss.key, default_rss_key, sizeof(default_rss_key));
    
    // Initialize indirection table (round-robin)
    for (int i = 0; i < RSS_INDIR_TABLE_SIZE; i++) {
        dev->rss.indir_table[i] = i % num_queues;
    }
    
    dev->rss.num_queues = num_queues;
    
    kprintf("[RSS] Initialized RSS with %d queues for %s\n", 
            num_queues, dev->name);
    
    return 0;
}

// Toeplitz hash implementation
uint32_t rss_hash_packet(const void *data, uint16_t len, const uint8_t *key) {
    if (!data || !key) return 0;
    
    const uint8_t *pkt = (const uint8_t *)data;
    uint32_t result = 0;
    uint32_t key_part = 0;
    int key_idx = 0;
    
    // Load first 4 bytes of key
    for (int i = 0; i < 4; i++) {
        key_part = (key_part << 8) | key[key_idx++];
    }
    
    // Process each byte of input
    for (uint16_t i = 0; i < len && key_idx < 40; i++) {
        uint8_t byte = pkt[i];
        
        for (int bit = 7; bit >= 0; bit--) {
            if (byte & (1 << bit)) {
                result ^= key_part;
            }
            
            // Shift key_part and bring in next bit
            int next_key_bit = (key_idx < 40) ? 
                               ((key[key_idx] >> (7 - (i * 8 + (7 - bit)) % 8)) & 1) : 0;
            key_part = (key_part << 1) | next_key_bit;
            
            if ((i * 8 + (7 - bit) + 1) % 8 == 0 && key_idx < 40) {
                key_idx++;
            }
        }
    }
    
    return result;
}

int rss_select_queue(net_device_t *dev, uint32_t hash) {
    if (!dev || dev->rss.num_queues == 0) {
        return 0;
    }
    
    // Use lower bits of hash to index into indirection table
    int idx = hash & (RSS_INDIR_TABLE_SIZE - 1);
    return dev->rss.indir_table[idx];
}

// ============================================================================
// Checksum Offload
// ============================================================================

int csum_tx_offload_available(net_device_t *dev, int proto) {
    if (!dev) return 0;
    
    switch (proto) {
        case 6:  // TCP
            return (dev->features.csum_offload & CSUM_OFFLOAD_TX_TCP) != 0;
        case 17: // UDP
            return (dev->features.csum_offload & CSUM_OFFLOAD_TX_UDP) != 0;
        default:
            return (dev->features.csum_offload & CSUM_OFFLOAD_TX_IPV4) != 0;
    }
}

int csum_rx_verified(net_device_t *dev, uint32_t status) {
    // Status bits are device-specific, this is a placeholder
    // Real implementation would check device's RX status flags
    (void)dev;
    (void)status;
    return 0;
}

uint16_t csum_fold(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

uint32_t csum_partial(const void *data, int len, uint32_t sum) {
    const uint16_t *ptr = (const uint16_t *)data;
    
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    
    if (len > 0) {
        sum += *(const uint8_t *)ptr;
    }
    
    return sum;
}

uint16_t csum_tcpudp(uint32_t saddr, uint32_t daddr, int len,
                     int proto, uint32_t sum) {
    // Add pseudo-header
    sum += (saddr >> 16) & 0xFFFF;
    sum += saddr & 0xFFFF;
    sum += (daddr >> 16) & 0xFFFF;
    sum += daddr & 0xFFFF;
    sum += (uint16_t)(((len >> 8) & 0xFF) | ((len & 0xFF) << 8));  // htons
    sum += (uint16_t)(proto << 8);  // htons
    
    return csum_fold(sum);
}

// ============================================================================
// Interrupt Coalescing
// ============================================================================

int netdev_set_coalesce(net_device_t *dev, int rx_usecs, int tx_usecs,
                        int rx_frames, int tx_frames) {
    if (!dev) return -1;
    
    // Validate parameters
    if (rx_usecs < 0 || rx_usecs > INT_DELAY_MAX_US) return -1;
    if (tx_usecs < 0 || tx_usecs > INT_DELAY_MAX_US) return -1;
    
    dev->coalesce.rx_usecs = rx_usecs;
    dev->coalesce.tx_usecs = tx_usecs;
    dev->coalesce.rx_max_frames = rx_frames > 0 ? rx_frames : 0;
    dev->coalesce.tx_max_frames = tx_frames > 0 ? tx_frames : 0;
    
    // Apply to hardware if device supports it
    if (dev->ops && dev->ops->set_coalesce) {
        return dev->ops->set_coalesce(dev, &dev->coalesce);
    }
    
    kprintf("[NET_PERF] Set coalescing: RX %d us/%d frames, TX %d us/%d frames\n",
            rx_usecs, rx_frames, tx_usecs, tx_frames);
    
    return 0;
}

int netdev_set_adaptive_coalesce(net_device_t *dev, int enable_rx, int enable_tx) {
    if (!dev) return -1;
    
    dev->coalesce.adaptive_rx = enable_rx;
    dev->coalesce.adaptive_tx = enable_tx;
    
    kprintf("[NET_PERF] Adaptive coalescing: RX=%d TX=%d\n", 
            enable_rx, enable_tx);
    
    return 0;
}

// ============================================================================
// Buffer Management
// ============================================================================

void *netdev_alloc_buffer(net_device_t *dev, size_t size, uint64_t *dma_addr) {
    (void)dev;
    
    // Allocate page-aligned memory for DMA
    // For sizes <= 4KB, use single page
    // For larger (jumbo frames), may need multiple pages
    
    if (size <= 4096) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            return NULL;
        }
        if (dma_addr) {
            *dma_addr = phys;
        }
        return (void *)phys;  // Identity-mapped
    }
    
    // For larger allocations, use heap (not DMA-safe in current implementation)
    // TODO: Implement contiguous physical page allocation
    kprintf("[NET_PERF] Warning: Large buffer allocation may not be DMA-safe\n");
    void *buf = kzalloc(size);
    if (dma_addr) {
        *dma_addr = (uint64_t)buf;  // Assumes identity mapping
    }
    return buf;
}

void netdev_free_buffer(net_device_t *dev, void *buffer, uint64_t dma_addr) {
    (void)dev;
    
    if (dma_addr && dma_addr < 0x100000000ULL) {
        // Looks like a physical page
        pmm_free_page(dma_addr);
    } else if (buffer) {
        kfree(buffer);
    }
}

// ============================================================================
// Device Registration
// ============================================================================

int netdev_register(net_device_t *dev) {
    if (!dev) return -1;
    
    // Add to device list
    dev->next = netdev_list;
    netdev_list = dev;
    
    kprintf("[NET_PERF] Registered network device: %s\n", dev->name);
    kprintf("  MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
            dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
            dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);
    kprintf("  Queues: %d RX, %d TX\n", dev->num_rx_queues, dev->num_tx_queues);
    
    return 0;
}

void netdev_unregister(net_device_t *dev) {
    if (!dev) return;
    
    // Remove from device list
    net_device_t **pp = &netdev_list;
    while (*pp) {
        if (*pp == dev) {
            *pp = dev->next;
            break;
        }
        pp = &(*pp)->next;
    }
    
    // Free resources
    netdev_free_queues(dev);
    
    kprintf("[NET_PERF] Unregistered network device: %s\n", dev->name);
}

net_device_t *netdev_get_by_name(const char *name) {
    if (!name) return NULL;
    
    net_device_t *dev = netdev_list;
    while (dev) {
        if (strcmp(dev->name, name) == 0) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

net_device_t *netdev_get_first(void) {
    return netdev_list;
}
