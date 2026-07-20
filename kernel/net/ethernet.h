// ethernet.h - Ethernet frame handling
#ifndef ETHERNET_H
#define ETHERNET_H

#include "../types.h"

// Ethernet header size
#define ETH_HEADER_SIZE     14
#define ETH_MIN_FRAME_SIZE  60
#define ETH_MAX_FRAME_SIZE  1518
#define ETH_MTU             1500

// EtherTypes
#define ETH_TYPE_IPV4       0x0800
#define ETH_TYPE_ARP        0x0806
#define ETH_TYPE_IPV6       0x86DD

// Broadcast MAC address
extern const uint8_t ETH_BROADCAST[6];

// Ethernet header structure
typedef struct {
    uint8_t  dest[6];       // Destination MAC
    uint8_t  src[6];        // Source MAC
    uint16_t type;          // EtherType (big-endian)
} __attribute__((packed)) eth_header_t;

// Initialize ethernet
void eth_init(void);

// Send an ethernet frame
int eth_send(const uint8_t *dest_mac, uint16_t type, const void *data, uint16_t length);

// Receive and process ethernet frames (call periodically)
int eth_receive(void);

// Register protocol handler
typedef void (*eth_handler_t)(const uint8_t *src_mac, const void *data, uint16_t length);
void eth_register_handler(uint16_t type, eth_handler_t handler);

// Get our MAC address
void eth_get_mac(uint8_t *mac);

#endif // ETHERNET_H
