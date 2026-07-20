// e1000_mq.h - Intel E1000/82575/82576/I350 Multi-Queue Driver
// Task #54: Multi-queue support, RSS, offloading, interrupt coalescing
#ifndef E1000_MQ_H
#define E1000_MQ_H

#include "../types.h"
#include "net_perf.h"

// ============================================================================
// E1000 Advanced Register Definitions (for multi-queue NICs)
// ============================================================================

// Multi-queue transmit registers (82575+)
#define E1000_TXDCTL_MQ(n)      (0x03828 + 0x100 * (n))  // TX Descriptor Control
#define E1000_TDBAL_MQ(n)       (0x0E000 + 0x40 * (n))   // TX Desc Base Low
#define E1000_TDBAH_MQ(n)       (0x0E004 + 0x40 * (n))   // TX Desc Base High
#define E1000_TDLEN_MQ(n)       (0x0E008 + 0x40 * (n))   // TX Desc Length
#define E1000_TDH_MQ(n)         (0x0E010 + 0x40 * (n))   // TX Desc Head
#define E1000_TDT_MQ(n)         (0x0E018 + 0x40 * (n))   // TX Desc Tail

// Multi-queue receive registers (82575+)
#define E1000_RXDCTL_MQ(n)      (0x02828 + 0x100 * (n))  // RX Descriptor Control
#define E1000_RDBAL_MQ(n)       (0x0C000 + 0x40 * (n))   // RX Desc Base Low
#define E1000_RDBAH_MQ(n)       (0x0C004 + 0x40 * (n))   // RX Desc Base High
#define E1000_RDLEN_MQ(n)       (0x0C008 + 0x40 * (n))   // RX Desc Length
#define E1000_RDH_MQ(n)         (0x0C010 + 0x40 * (n))   // RX Desc Head
#define E1000_RDT_MQ(n)         (0x0C018 + 0x40 * (n))   // RX Desc Tail
#define E1000_SRRCTL_MQ(n)      (0x0C00C + 0x40 * (n))   // Split/Rep Recv Ctrl

// RSS registers
#define E1000_MRQC              0x05818  // Multiple RX Queues Command
#define E1000_RETA(n)           (0x05C00 + 4 * (n))  // RSS Redirection Table
#define E1000_RSSRK(n)          (0x05C80 + 4 * (n))  // RSS Random Key

// Interrupt moderation
#define E1000_EITR(n)           (0x01680 + 4 * (n))  // Extended Int Throttle
#define E1000_IVAR               0x000E4  // Interrupt Vector Allocation
#define E1000_IVAR_MISC          0x000E8  // Misc Interrupt Vector

// MSI-X registers
#define E1000_EICS              0x01520  // Ext Interrupt Cause Set
#define E1000_EIMS              0x01524  // Ext Interrupt Mask Set/Read
#define E1000_EIMC              0x01528  // Ext Interrupt Mask Clear
#define E1000_EIAC              0x0152C  // Ext Interrupt Auto Clear
#define E1000_EIAM              0x01530  // Ext Interrupt Auto Mask Enable
#define E1000_EICR              0x01580  // Ext Interrupt Cause Read

// Checksum offload registers
#define E1000_RXCSUM            0x05000  // RX Checksum Control
#define E1000_TXCW              0x00178  // TX Configuration Word

// TX offload context descriptor
#define E1000_TXD_CMD_TSE       0x04000000  // TCP Segmentation Enable
#define E1000_TXD_CMD_IP        0x02000000  // IP checksum offload
#define E1000_TXD_CMD_TCP       0x01000000  // TCP checksum offload
#define E1000_TXD_CMD_UDP       0x00000000  // UDP checksum (context desc)

// RXCSUM bits
#define E1000_RXCSUM_PCSS_MASK  0x000000FF  // Packet Checksum Start
#define E1000_RXCSUM_IPOFLD     0x00000100  // IP Checksum Offload
#define E1000_RXCSUM_TUOFLD     0x00000200  // TCP/UDP Checksum Offload
#define E1000_RXCSUM_CRCOFL     0x00000800  // CRC32 Offload
#define E1000_RXCSUM_IPPCSE     0x00001000  // IP Payload Checksum

// MRQC bits
#define E1000_MRQC_ENABLE_RSS   0x00000001  // RSS Enable
#define E1000_MRQC_RSS_FIELD_IPV4       0x00010000
#define E1000_MRQC_RSS_FIELD_TCPIPV4    0x00020000
#define E1000_MRQC_RSS_FIELD_IPV6       0x00040000
#define E1000_MRQC_RSS_FIELD_TCPIPV6    0x00100000
#define E1000_MRQC_RSS_FIELD_UDPIPV4    0x00400000
#define E1000_MRQC_RSS_FIELD_UDPIPV6    0x00800000

// SRRCTL bits
#define E1000_SRRCTL_BSIZEPKT_SHIFT     10
#define E1000_SRRCTL_BSIZEHDR_SHIFT     2
#define E1000_SRRCTL_DESCTYPE_LEGACY    0x00000000
#define E1000_SRRCTL_DESCTYPE_ADV       0x02000000
#define E1000_SRRCTL_DROP_EN            0x80000000

// TXDCTL bits
#define E1000_TXDCTL_ENABLE     0x02000000
#define E1000_TXDCTL_SWFLSH     0x04000000
#define E1000_TXDCTL_WTHRESH_SHIFT  16
#define E1000_TXDCTL_PTHRESH_SHIFT  0
#define E1000_TXDCTL_HTHRESH_SHIFT  8

// RXDCTL bits
#define E1000_RXDCTL_ENABLE     0x02000000

// Interrupt throttle rates (in 256ns units)
#define E1000_EITR_INTERVAL_20K     50     // 20000 int/sec
#define E1000_EITR_INTERVAL_10K     100    // 10000 int/sec
#define E1000_EITR_INTERVAL_5K      200    // 5000 int/sec

// ============================================================================
// Advanced Descriptor Formats (82575+)
// ============================================================================

// Advanced TX descriptor (context)
typedef struct {
    uint32_t vlan_macip_lens;
    uint32_t seqnum_seed;
    uint32_t type_tucmd_mlhl;
    uint32_t mss_l4len_idx;
} __attribute__((packed)) e1000_adv_tx_context_desc_t;

// Advanced TX descriptor (data)
typedef struct {
    uint64_t buffer_addr;
    uint32_t cmd_type_len;
    uint32_t olinfo_status;
} __attribute__((packed)) e1000_adv_tx_data_desc_t;

// Advanced RX descriptor (read format, for hardware)
typedef struct {
    uint64_t pkt_addr;
    uint64_t hdr_addr;
} __attribute__((packed)) e1000_adv_rx_desc_read_t;

// Advanced RX descriptor (write-back format, from hardware)
typedef struct {
    uint32_t rss_hash;
    uint16_t ip_id;
    uint16_t csum;
    uint32_t status_error;
    uint16_t length;
    uint16_t vlan_tag;
} __attribute__((packed)) e1000_adv_rx_desc_wb_t;

// Union for advanced RX descriptor
typedef union {
    e1000_adv_rx_desc_read_t read;
    e1000_adv_rx_desc_wb_t wb;
} e1000_adv_rx_desc_t;

// RX status bits in advanced descriptor
#define E1000_RXD_STAT_DD       0x00000001  // Descriptor Done
#define E1000_RXD_STAT_EOP      0x00000002  // End of Packet
#define E1000_RXD_STAT_IXSM     0x00000004  // Ignore Checksum
#define E1000_RXD_STAT_VP       0x00000008  // VLAN Packet
#define E1000_RXD_STAT_UDPCS    0x00000010  // UDP Checksum Valid
#define E1000_RXD_STAT_TCPCS    0x00000020  // TCP Checksum Valid
#define E1000_RXD_STAT_IPCS     0x00000040  // IP Checksum Valid

// ============================================================================
// E1000 Multi-Queue Device State
// ============================================================================

// Per-queue state
typedef struct {
    // Descriptors
    void *desc_ring;
    uint64_t desc_dma;
    uint32_t desc_count;
    
    // Buffer pool
    void **buffers;
    uint64_t *buffer_dma;
    
    // Ring indices
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t next_to_use;
    uint32_t next_to_clean;
    
    // NAPI
    napi_struct_t napi;
    
    // Statistics
    uint64_t packets;
    uint64_t bytes;
    uint64_t errors;
} e1000_queue_t;

// Device capabilities
#define E1000_CAP_MULTI_QUEUE   (1 << 0)
#define E1000_CAP_RSS           (1 << 1)
#define E1000_CAP_CSUM_OFFLOAD  (1 << 2)
#define E1000_CAP_TSO           (1 << 3)
#define E1000_CAP_MSIX          (1 << 4)
#define E1000_CAP_JUMBO         (1 << 5)

// Main device state
typedef struct {
    net_device_t netdev;        // Must be first (for casting)
    
    // Hardware info
    volatile uint8_t *mmio;
    uint16_t device_id;
    uint32_t capabilities;
    
    // Queues
    int num_rx_queues;
    int num_tx_queues;
    e1000_queue_t *rx_queues;
    e1000_queue_t *tx_queues;
    
    // MSI-X vectors
    int num_msix_vectors;
    int *msix_entries;
    
    // Interrupt coalescing
    uint32_t itr_setting;       // Interrupt throttle rate
    int adaptive_itr;
    
    // Current link state
    int link_up;
    int link_speed;             // Mbps
    int link_duplex;            // 0=half, 1=full
} e1000_mq_dev_t;

// ============================================================================
// API Functions
// ============================================================================

// Initialize multi-queue E1000 driver
int e1000_mq_init(void);

// Probe for advanced E1000 variants (82575, 82576, I350, I210, I225)
int e1000_mq_probe(uint16_t vendor_id, uint16_t device_id);

// Configure RSS
int e1000_mq_configure_rss(e1000_mq_dev_t *dev, int num_queues);

// Configure checksum offload
int e1000_mq_configure_csum(e1000_mq_dev_t *dev, int rx_csum, int tx_csum);

// Configure interrupt coalescing
int e1000_mq_configure_itr(e1000_mq_dev_t *dev, uint32_t itr_val);

// Set MTU (for jumbo frame support)
int e1000_mq_set_mtu(e1000_mq_dev_t *dev, int mtu);

// NAPI poll function
int e1000_mq_napi_poll(napi_struct_t *napi, int budget);

// IRQ handler (MSI-X aware)
void e1000_mq_irq_handler(int vector);

// Send packet on specific queue
int e1000_mq_xmit(e1000_mq_dev_t *dev, const void *data, uint16_t len, int queue);

// Get device by index
e1000_mq_dev_t *e1000_mq_get_device(int index);

// ============================================================================
// Supported Device IDs
// ============================================================================

// Intel 82575/82576 (multi-queue capable)
#define E1000_DEV_82575EB       0x10A7
#define E1000_DEV_82575GB       0x10A9
#define E1000_DEV_82576         0x10C9
#define E1000_DEV_82576_FIBER   0x10E6
#define E1000_DEV_82576_SERDES  0x10E7
#define E1000_DEV_82576_QUAD    0x10E8

// Intel I350 (4-port server adapter)
#define E1000_DEV_I350          0x1521
#define E1000_DEV_I350_FIBER    0x1522
#define E1000_DEV_I350_SERDES   0x1523

// Intel I210/I211 (single-port, low power)
#define E1000_DEV_I210          0x1533
#define E1000_DEV_I210_FIBER    0x1536
#define E1000_DEV_I210_SERDES   0x1537
#define E1000_DEV_I211          0x1539

// Intel I225/I226 (2.5Gbps)
#define E1000_DEV_I225_LM       0x15F2
#define E1000_DEV_I225_V        0x15F3
#define E1000_DEV_I225_I        0x15F8
#define E1000_DEV_I225_K        0x3100
#define E1000_DEV_I226_LM       0x125B
#define E1000_DEV_I226_V        0x125C

#endif // E1000_MQ_H
