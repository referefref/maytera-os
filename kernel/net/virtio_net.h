// virtio_net.h - VirtIO Network device driver
#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include "../types.h"
#include "virtio.h"

// VirtIO network device feature bits
#define VIRTIO_NET_F_CSUM           (1 << 0)   // Device handles checksums
#define VIRTIO_NET_F_GUEST_CSUM     (1 << 1)   // Guest handles checksums
#define VIRTIO_NET_F_MAC            (1 << 5)   // Device has given MAC address
#define VIRTIO_NET_F_GSO            (1 << 6)   // Device handles GSO
#define VIRTIO_NET_F_GUEST_TSO4     (1 << 7)   // Guest can receive TSOv4
#define VIRTIO_NET_F_GUEST_TSO6     (1 << 8)   // Guest can receive TSOv6
#define VIRTIO_NET_F_GUEST_ECN      (1 << 9)   // Guest can receive TSO with ECN
#define VIRTIO_NET_F_GUEST_UFO      (1 << 10)  // Guest can receive UFO
#define VIRTIO_NET_F_HOST_TSO4      (1 << 11)  // Device can receive TSOv4
#define VIRTIO_NET_F_HOST_TSO6      (1 << 12)  // Device can receive TSOv6
#define VIRTIO_NET_F_HOST_ECN       (1 << 13)  // Device can receive TSO with ECN
#define VIRTIO_NET_F_HOST_UFO       (1 << 14)  // Device can receive UFO
#define VIRTIO_NET_F_MRG_RXBUF      (1 << 15)  // Guest can merge receive buffers
#define VIRTIO_NET_F_STATUS         (1 << 16)  // Configuration status available
#define VIRTIO_NET_F_CTRL_VQ        (1 << 17)  // Control channel available
#define VIRTIO_NET_F_CTRL_RX        (1 << 18)  // Control RX mode support
#define VIRTIO_NET_F_CTRL_VLAN      (1 << 19)  // Control VLAN filtering
#define VIRTIO_NET_F_CTRL_RX_EXTRA  (1 << 20)  // Extra RX mode supported
#define VIRTIO_NET_F_GUEST_ANNOUNCE (1 << 21)  // Guest can send gratuitous ARP
#define VIRTIO_NET_F_MQ             (1 << 22)  // Multi-queue support

// VirtIO network status bits
#define VIRTIO_NET_S_LINK_UP        1   // Link is up

// VirtIO network packet header (prepended to each packet)
typedef struct {
    uint8_t  flags;         // Flags
    uint8_t  gso_type;      // GSO type
    uint16_t hdr_len;       // Header length (for GSO)
    uint16_t gso_size;      // GSO segment size
    uint16_t csum_start;    // Checksum start offset
    uint16_t csum_offset;   // Checksum offset (from csum_start)

} __attribute__((packed)) virtio_net_hdr_t;

// VirtIO network header flags
#define VIRTIO_NET_HDR_F_NEEDS_CSUM     1   // Needs checksum calculation
#define VIRTIO_NET_HDR_F_DATA_VALID     2   // Data checksum verified

// GSO types
#define VIRTIO_NET_HDR_GSO_NONE         0   // Not a GSO packet
#define VIRTIO_NET_HDR_GSO_TCPV4        1   // TCP/IPv4
#define VIRTIO_NET_HDR_GSO_UDP          3   // UDP
#define VIRTIO_NET_HDR_GSO_TCPV6        4   // TCP/IPv6
#define VIRTIO_NET_HDR_GSO_ECN          0x80 // ECN flag

// VirtIO network device configuration (at BAR0 + VIRTIO_PCI_CONFIG for legacy)
typedef struct {
    uint8_t  mac[6];        // MAC address
    uint16_t status;        // Status flags
    uint16_t max_virtqueue_pairs;  // Max VQ pairs (if VIRTIO_NET_F_MQ)
} __attribute__((packed)) virtio_net_config_t;

// Queue indices
#define VIRTIO_NET_QUEUE_RX     0   // Receive queue
#define VIRTIO_NET_QUEUE_TX     1   // Transmit queue
#define VIRTIO_NET_QUEUE_CTRL   2   // Control queue (if available)

// Buffer sizes
#define VIRTIO_NET_RX_BUFFER_SIZE   2048
#define VIRTIO_NET_TX_BUFFER_SIZE   2048
#define VIRTIO_NET_NUM_RX_BUFFERS   256
#define VIRTIO_NET_NUM_TX_BUFFERS   32

// Initialize VirtIO network driver
int virtio_net_init(void);

// Get MAC address
void virtio_net_get_mac(uint8_t *mac);

// Send a packet
int virtio_net_send(const void *data, uint16_t length);

// Receive a packet (returns length, 0 if none available)
int virtio_net_receive(void *buffer, uint16_t buffer_size);

// Check link status
int virtio_net_link_up(void);

// Handle interrupt
void virtio_net_irq_handler(void);

#endif // VIRTIO_NET_H
