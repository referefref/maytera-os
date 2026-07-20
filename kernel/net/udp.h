// udp.h - User Datagram Protocol
#ifndef UDP_H
#define UDP_H

#include "../types.h"

// UDP header
typedef struct {
    uint16_t src_port;      // Source port
    uint16_t dest_port;     // Destination port
    uint16_t length;        // Length (header + data)
    uint16_t checksum;      // Checksum (optional in IPv4)
} __attribute__((packed)) udp_header_t;

// UDP callback function type
typedef void (*udp_callback_t)(uint32_t src_ip, uint16_t src_port,
                               const void *data, uint16_t length);

// Initialize UDP
void udp_init(void);

// Register a handler for a port
int udp_bind(uint16_t port, udp_callback_t callback);

// Unbind a port
void udp_unbind(uint16_t port);

// Send UDP packet
int udp_send(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port,
             const void *data, uint16_t length);

// Handle incoming UDP packet (called by IP layer)
void udp_handle(uint32_t src_ip, const void *data, uint16_t length);

#endif // UDP_H
