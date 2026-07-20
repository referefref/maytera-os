// ethernet.c - Ethernet frame handling implementation
#include "ethernet.h"
#include "net.h"
#include "../serial.h"
#include "../string.h"

// Broadcast MAC address
const uint8_t ETH_BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Our MAC address
static uint8_t our_mac[6];

// Protocol handlers
#define MAX_ETH_HANDLERS 8
static struct {
    uint16_t type;
    eth_handler_t handler;
} eth_handlers[MAX_ETH_HANDLERS];
static int eth_handler_count = 0;

// Receive buffer
static uint8_t rx_buffer[ETH_MAX_FRAME_SIZE];

// Byte swap for 16-bit network order
static inline uint16_t htons(uint16_t h) {
    return ((h & 0xFF) << 8) | ((h >> 8) & 0xFF);
}

static inline uint16_t ntohs(uint16_t n) {
    return htons(n);  // Same operation
}

// Initialize ethernet
void eth_init(void) {
    nic_get_mac(our_mac);
    eth_handler_count = 0;
    kprintf("[ETH] Ethernet initialized, our MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
            our_mac[0], our_mac[1], our_mac[2],
            our_mac[3], our_mac[4], our_mac[5]);
}

// Send an ethernet frame
// Cumulative link byte counters for the taskbar network gauge (#102).
uint64_t g_net_tx_bytes = 0;
uint64_t g_net_rx_bytes = 0;
uint64_t net_total_bytes(void) { return g_net_tx_bytes + g_net_rx_bytes; }

int eth_send(const uint8_t *dest_mac, uint16_t type, const void *data, uint16_t length) {
    // kprintf("[ETH] eth_send: type=0x%04x len=%d dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
    //         type, length, dest_mac[0], dest_mac[1], dest_mac[2],
    //         dest_mac[3], dest_mac[4], dest_mac[5]);

    if (!dest_mac || !data || length > ETH_MTU) {
        // kprintf("[ETH] eth_send: invalid params\n");
        return -1;
    }

    // Build frame
    uint8_t frame[ETH_MAX_FRAME_SIZE];
    eth_header_t *header = (eth_header_t *)frame;

    // Set destination and source MAC
    memcpy(header->dest, dest_mac, 6);
    memcpy(header->src, our_mac, 6);
    header->type = htons(type);

    // Copy payload
    memcpy(frame + ETH_HEADER_SIZE, data, length);

    // Pad if necessary
    uint16_t frame_length = ETH_HEADER_SIZE + length;
    if (frame_length < ETH_MIN_FRAME_SIZE) {
        memset(frame + frame_length, 0, ETH_MIN_FRAME_SIZE - frame_length);
        frame_length = ETH_MIN_FRAME_SIZE;
    }

    // Send via NIC driver
    // kprintf("[ETH] calling nic_send with frame_length=%d\n", frame_length);
    int result = nic_send(frame, frame_length);
    if (result >= 0) g_net_tx_bytes += frame_length;
    return result;
}

// Debug counter to reduce log spam
static int eth_rx_debug_counter = 0;

// Direct serial output for debugging (bypasses kprintf settings)
extern void serial_puts(uint16_t port, const char *str);
#define SERIAL_COM1 0x3F8

// Receive and process ethernet frames. Returns 1 if packet processed, 0 if empty.
int eth_receive(void) {
    int length = nic_receive(rx_buffer, sizeof(rx_buffer));
    if (length > 0) g_net_rx_bytes += (uint64_t)length;

    // Debug: show what nic_receive returns (first 10 times with data)
    // static int early_debug = 0;
    // if (length > 0 && early_debug < 10) {
    //     early_debug++;
    //     serial_puts(SERIAL_COM1, "[ETH] Got data from nic_receive!\r\n");
    // }

    if (length < ETH_HEADER_SIZE) {
        return 0;  // No frame or too short
    }

    eth_header_t *header = (eth_header_t *)rx_buffer;
    uint16_t type = ntohs(header->type);

    // Debug: show every 10th received frame info
    eth_rx_debug_counter++;

    // Check if frame is for us (unicast, broadcast, or multicast)
    int for_us = 0;
    if (memcmp(header->dest, our_mac, 6) == 0) {
        for_us = 1;  // Unicast to us
        // kprintf("[ETH] Frame is unicast TO US!\n");
    } else if (memcmp(header->dest, ETH_BROADCAST, 6) == 0) {
        for_us = 1;  // Broadcast
    } else if (header->dest[0] & 0x01) {
        for_us = 1;  // Multicast
    }

    if (!for_us) {
        return 1;  // Received but not for us
    }

    // Find handler (type already extracted above for debug)
    for (int i = 0; i < eth_handler_count; i++) {
        if (eth_handlers[i].type == type && eth_handlers[i].handler) {
            // kprintf("[ETH] Calling handler for type 0x%04x\n", type);
            eth_handlers[i].handler(header->src, rx_buffer + ETH_HEADER_SIZE,
                                    length - ETH_HEADER_SIZE);
            return 1;
        }
    }
    // kprintf("[ETH] No handler found for type 0x%04x\n", type);
    return 1;  // Packet received but no handler
}

// Register protocol handler
void eth_register_handler(uint16_t type, eth_handler_t handler) {
    if (eth_handler_count >= MAX_ETH_HANDLERS) {
        return;
    }
    eth_handlers[eth_handler_count].type = type;
    eth_handlers[eth_handler_count].handler = handler;
    eth_handler_count++;
}

// Get our MAC address
void eth_get_mac(uint8_t *mac) {
    if (mac) {
        memcpy(mac, our_mac, 6);
    }
}
