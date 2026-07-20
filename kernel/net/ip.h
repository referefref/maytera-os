// ip.h - Internet Protocol (IPv4)
#ifndef IP_H
#define IP_H

#include "../types.h"

// IP header
typedef struct {
    uint8_t  version_ihl;   // Version (4 bits) + IHL (4 bits)
    uint8_t  tos;           // Type of Service
    uint16_t total_length;  // Total Length
    uint16_t id;            // Identification
    uint16_t flags_frag;    // Flags (3 bits) + Fragment Offset (13 bits)
    uint8_t  ttl;           // Time to Live
    uint8_t  protocol;      // Protocol
    uint16_t checksum;      // Header Checksum
    uint32_t src_ip;        // Source IP
    uint32_t dest_ip;       // Destination IP
} __attribute__((packed)) ip_header_t;

// IP protocols
#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

// IP flags
#define IP_FLAG_DF      0x4000  // Don't Fragment
#define IP_FLAG_MF      0x2000  // More Fragments

// IP header size (no options)
#define IP_HEADER_SIZE  20

// IP MTU
#define IP_MTU          1500

// Initialize IP
void ip_init(void);

// Set our IP address
void ip_set_address(uint32_t ip);

// Get our IP address
uint32_t ip_get_address(void);

// Set gateway
void ip_set_gateway(uint32_t gateway);

// Get gateway
uint32_t ip_get_gateway(void);

// Set subnet mask
void ip_set_netmask(uint32_t mask);

// Get subnet mask
uint32_t ip_get_netmask(void);

// Send IP packet
int ip_send(uint32_t dest_ip, uint8_t protocol, const void *data, uint16_t length);

// Handle incoming IP packet
void ip_handle(const uint8_t *src_mac, const void *data, uint16_t length);

// Register protocol handler
typedef void (*ip_handler_t)(uint32_t src_ip, const void *data, uint16_t length);
void ip_register_handler(uint8_t protocol, ip_handler_t handler);

// Helper: Make IP address from bytes
static inline uint32_t ip_make(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}

// Helper: Print IP address
void ip_print(uint32_t ip);

// Calculate IP checksum
uint16_t ip_checksum(const void *data, uint16_t length);

// Original C impl and the Rust port, both callable so the strangler flag
// (-DRUST_IP_CHECKSUM) can route ip_checksum() to either (#404 / #479 Phase B).
uint16_t ip_checksum_c(const void *data, uint16_t length);
extern uint16_t ip_checksum_rs(const void *data, uint16_t length);

// Boot-time differential self-test: asserts ip_checksum_rs == ip_checksum_c
// over a bounded corpus and logs one [RUST-DIFF] line (serial + /BOOTLOG).
void ip_checksum_rust_selftest(void);

// Send broadcast IP packet (for DHCP when we don't have IP yet)
int ip_send_broadcast(uint8_t protocol, const void *data, uint16_t length);

#endif // IP_H
