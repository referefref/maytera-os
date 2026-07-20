// net_perf.h - Network Performance Infrastructure
// Task #54: Multi-queue, RSS, offloading, NAPI-style polling
#ifndef NET_PERF_H
#define NET_PERF_H

#include "../types.h"

// Maximum number of hardware queues per NIC
#define MAX_NET_QUEUES          16
#define MAX_NAPI_BUDGET         64      // Max packets per NAPI poll
#define DEFAULT_NAPI_BUDGET     32
#define NAPI_POLL_WEIGHT        64

// Interrupt coalescing parameters
#define INT_DELAY_MIN_US        0       // Microseconds
#define INT_DELAY_MAX_US        1000
#define INT_DELAY_DEFAULT_US    100

// Buffer sizes for jumbo frames
#define NET_BUFFER_STANDARD     2048
#define NET_BUFFER_JUMBO        9216    // 9KB MTU support
#define NET_BUFFER_ALIGNMENT    64      // Cache line alignment

// RSS (Receive Side Scaling) hash types
#define RSS_HASH_TYPE_IPV4      (1 << 0)
#define RSS_HASH_TYPE_TCPV4     (1 << 1)
#define RSS_HASH_TYPE_UDPV4     (1 << 2)
#define RSS_HASH_TYPE_IPV6      (1 << 3)
#define RSS_HASH_TYPE_TCPV6     (1 << 4)
#define RSS_HASH_TYPE_UDPV6     (1 << 5)

// RSS indirection table size (must be power of 2)
#define RSS_INDIR_TABLE_SIZE    128

// Checksum offload flags
#define CSUM_OFFLOAD_TX_IPV4    (1 << 0)
#define CSUM_OFFLOAD_TX_TCP     (1 << 1)
#define CSUM_OFFLOAD_TX_UDP     (1 << 2)
#define CSUM_OFFLOAD_RX_IPV4    (1 << 3)
#define CSUM_OFFLOAD_RX_TCP     (1 << 4)
#define CSUM_OFFLOAD_RX_UDP     (1 << 5)

// TSO (TCP Segmentation Offload) flags
#define TSO_IPV4                (1 << 0)
#define TSO_IPV6                (1 << 1)
#define TSO_ECN                 (1 << 2)

// Queue state flags
#define QUEUE_STATE_RUNNING     (1 << 0)
#define QUEUE_STATE_FROZEN      (1 << 1)
#define QUEUE_STATE_FULL        (1 << 2)

// NAPI state flags
#define NAPI_STATE_SCHED        (1 << 0)    // Scheduled for polling
#define NAPI_STATE_DISABLE      (1 << 1)    // Disabled
#define NAPI_STATE_NPSVC        (1 << 2)    // In poll service context

// Forward declarations
struct net_device;
struct net_queue;
struct napi_struct;

// Receive queue descriptor
typedef struct net_rx_queue {
    uint32_t queue_id;
    uint32_t state;
    
    // Ring buffer
    void **rx_buffers;
    uint64_t *rx_buffer_dma;    // Physical addresses for DMA
    uint32_t num_descriptors;
    uint32_t head;              // Consumer index (driver reads)
    uint32_t tail;              // Producer index (hardware writes)
    
    // Statistics
    uint64_t packets;
    uint64_t bytes;
    uint64_t dropped;
    uint64_t errors;
    uint64_t alloc_failed;
    
    // NAPI association
    struct napi_struct *napi;
    
    // CPU affinity for this queue
    int cpu_id;
    
    // Back pointer to device
    struct net_device *dev;
    
    // Interrupt vector for this queue (MSI-X)
    int irq_vector;
} net_rx_queue_t;

// Transmit queue descriptor
typedef struct net_tx_queue {
    uint32_t queue_id;
    uint32_t state;
    
    // Ring buffer
    void **tx_buffers;
    uint64_t *tx_buffer_dma;    // Physical addresses for DMA
    uint32_t num_descriptors;
    uint32_t head;              // Producer index (driver writes)
    uint32_t tail;              // Consumer index (hardware reads)
    
    // Statistics
    uint64_t packets;
    uint64_t bytes;
    uint64_t dropped;
    uint64_t errors;
    uint64_t restarts;
    
    // CPU affinity for this queue
    int cpu_id;
    
    // Back pointer to device
    struct net_device *dev;
    
    // Interrupt vector for this queue (MSI-X)
    int irq_vector;
    
    // Spinlock for multi-producer safety
    volatile uint32_t lock;
} net_tx_queue_t;

// NAPI (New API) structure for interrupt mitigation
typedef struct napi_struct {
    uint32_t state;
    int weight;                 // Max work per poll
    int budget;                 // Remaining work this poll
    
    // Poll function - returns work done, < budget means done
    int (*poll)(struct napi_struct *napi, int budget);
    
    // Associated RX queue
    net_rx_queue_t *rx_queue;
    
    // Device
    struct net_device *dev;
    
    // Linked list of scheduled NAPI contexts
    struct napi_struct *next;
    
    // CPU that owns this NAPI context
    int cpu_id;
} napi_struct_t;

// RSS configuration
typedef struct rss_config {
    uint32_t hash_types;        // Enabled hash types
    uint8_t key[40];            // Hash key (Toeplitz)
    uint8_t indir_table[RSS_INDIR_TABLE_SIZE];  // Indirection table
    uint32_t num_queues;        // Number of RX queues for RSS
} rss_config_t;

// Interrupt coalescing configuration
typedef struct int_coalesce_config {
    uint32_t rx_usecs;          // RX interrupt delay
    uint32_t rx_max_frames;     // Max RX frames before interrupt
    uint32_t tx_usecs;          // TX interrupt delay
    uint32_t tx_max_frames;     // Max TX frames before interrupt
    int adaptive_rx;            // Enable adaptive RX coalescing
    int adaptive_tx;            // Enable adaptive TX coalescing
} int_coalesce_config_t;

// Offload features
typedef struct offload_features {
    uint32_t csum_offload;      // Checksum offload flags
    uint32_t tso_flags;         // TSO flags
    uint32_t max_tso_size;      // Max TSO segment size
    int scatter_gather;         // Scatter-gather support
    int rx_hash;                // RX hash offload
    int rx_vlan_strip;          // VLAN stripping
    int tx_vlan_insert;         // VLAN insertion
} offload_features_t;

// Network device statistics
typedef struct net_device_stats {
    // Basic counters
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_errors;
    uint64_t tx_errors;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
    
    // Extended counters
    uint64_t multicast;
    uint64_t collisions;
    
    // Detailed RX errors
    uint64_t rx_length_errors;
    uint64_t rx_over_errors;
    uint64_t rx_crc_errors;
    uint64_t rx_frame_errors;
    uint64_t rx_fifo_errors;
    uint64_t rx_missed_errors;
    
    // Detailed TX errors
    uint64_t tx_aborted_errors;
    uint64_t tx_carrier_errors;
    uint64_t tx_fifo_errors;
    uint64_t tx_heartbeat_errors;
    uint64_t tx_window_errors;
} net_device_stats_t;

// Network device operations
typedef struct net_device_ops {
    // Basic operations
    int (*open)(struct net_device *dev);
    int (*close)(struct net_device *dev);
    int (*start_xmit)(struct net_device *dev, const void *data, 
                      uint16_t len, int queue_id);
    
    // Configuration
    int (*set_mac_addr)(struct net_device *dev, const uint8_t *addr);
    int (*set_mtu)(struct net_device *dev, int mtu);
    int (*set_features)(struct net_device *dev, offload_features_t *feat);
    
    // Multi-queue operations
    int (*setup_tc)(struct net_device *dev, int num_queues);
    int (*select_queue)(struct net_device *dev, const void *data, uint16_t len);
    
    // RSS operations
    int (*set_rss_config)(struct net_device *dev, rss_config_t *cfg);
    int (*get_rss_config)(struct net_device *dev, rss_config_t *cfg);
    
    // Interrupt coalescing
    int (*set_coalesce)(struct net_device *dev, int_coalesce_config_t *cfg);
    int (*get_coalesce)(struct net_device *dev, int_coalesce_config_t *cfg);
    
    // Statistics
    void (*get_stats)(struct net_device *dev, net_device_stats_t *stats);
    
    // NAPI
    int (*napi_poll)(napi_struct_t *napi, int budget);
} net_device_ops_t;

// Main network device structure
typedef struct net_device {
    char name[16];
    uint8_t mac_addr[6];
    uint32_t mtu;
    uint32_t flags;
    
    // Hardware info
    uint16_t vendor_id;
    uint16_t device_id;
    void *mmio_base;
    uint16_t io_base;
    int irq;
    
    // Multi-queue support
    uint32_t num_rx_queues;
    uint32_t num_tx_queues;
    net_rx_queue_t *rx_queues;
    net_tx_queue_t *tx_queues;
    
    // NAPI contexts (one per RX queue typically)
    napi_struct_t *napi_list;
    uint32_t num_napi;
    
    // RSS configuration
    rss_config_t rss;
    
    // Offload capabilities (hardware supports)
    offload_features_t hw_features;
    // Offload features (currently enabled)
    offload_features_t features;
    
    // Interrupt coalescing
    int_coalesce_config_t coalesce;
    
    // Statistics
    net_device_stats_t stats;
    
    // Operations
    net_device_ops_t *ops;
    
    // Driver private data
    void *priv;
    
    // Link to next device
    struct net_device *next;
} net_device_t;

// ============================================================================
// Multi-queue API
// ============================================================================

// Initialize multi-queue support for a device
int netdev_alloc_queues(net_device_t *dev, int num_rx, int num_tx);

// Free queue resources
void netdev_free_queues(net_device_t *dev);

// Select TX queue for a packet (based on hash or CPU)
int netdev_select_queue(net_device_t *dev, const void *data, uint16_t len);

// ============================================================================
// NAPI API
// ============================================================================

// Initialize NAPI context
void napi_init(napi_struct_t *napi, net_device_t *dev, 
               int (*poll)(napi_struct_t *, int), int weight);

// Enable NAPI polling
void napi_enable(napi_struct_t *napi);

// Disable NAPI polling
void napi_disable(napi_struct_t *napi);

// Schedule NAPI polling (called from interrupt handler)
void napi_schedule(napi_struct_t *napi);

// Complete NAPI polling (called when no more work)
void napi_complete(napi_struct_t *napi);

// Run NAPI polling for all scheduled contexts
void napi_poll_all(void);

// ============================================================================
// RSS API
// ============================================================================

// Initialize RSS with default settings
int rss_init_default(net_device_t *dev, int num_queues);

// Calculate RSS hash for a packet
uint32_t rss_hash_packet(const void *data, uint16_t len, const uint8_t *key);

// Get queue index from RSS hash
int rss_select_queue(net_device_t *dev, uint32_t hash);

// ============================================================================
// Checksum Offload API
// ============================================================================

// Check if TX checksum offload is available
int csum_tx_offload_available(net_device_t *dev, int proto);

// Check if RX checksum was validated by hardware
int csum_rx_verified(net_device_t *dev, uint32_t status);

// Calculate software checksum as fallback
uint16_t csum_fold(uint32_t sum);
uint32_t csum_partial(const void *data, int len, uint32_t sum);
uint16_t csum_tcpudp(uint32_t saddr, uint32_t daddr, int len, 
                     int proto, uint32_t sum);

// ============================================================================
// Interrupt Coalescing API
// ============================================================================

// Set interrupt coalescing parameters
int netdev_set_coalesce(net_device_t *dev, int rx_usecs, int tx_usecs,
                        int rx_frames, int tx_frames);

// Enable adaptive interrupt coalescing
int netdev_set_adaptive_coalesce(net_device_t *dev, int enable_rx, int enable_tx);

// ============================================================================
// Buffer Management
// ============================================================================

// Allocate DMA-safe network buffer
void *netdev_alloc_buffer(net_device_t *dev, size_t size, uint64_t *dma_addr);

// Free network buffer
void netdev_free_buffer(net_device_t *dev, void *buffer, uint64_t dma_addr);

// ============================================================================
// Device Registration
// ============================================================================

// Register a network device
int netdev_register(net_device_t *dev);

// Unregister a network device
void netdev_unregister(net_device_t *dev);

// Find device by name
net_device_t *netdev_get_by_name(const char *name);

// Iterate all devices
net_device_t *netdev_get_first(void);

#endif // NET_PERF_H
