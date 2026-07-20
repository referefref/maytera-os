// udp.c - User Datagram Protocol implementation
#include "udp.h"
#include "ip.h"
#include "../serial.h"
#include "../string.h"

#define MAX_UDP_BINDINGS 16

// UDP binding entry
typedef struct {
    uint16_t port;
    udp_callback_t callback;
    int active;
} udp_binding_t;

static udp_binding_t bindings[MAX_UDP_BINDINGS];

// Byte swap
static inline uint16_t htons(uint16_t h) {
    return ((h & 0xFF) << 8) | ((h >> 8) & 0xFF);
}

static inline uint16_t ntohs(uint16_t n) {
    return htons(n);
}

static inline uint32_t udp_htonl(uint32_t h) {
    return ((h & 0xFF) << 24) | ((h & 0xFF00) << 8) |
           ((h >> 8) & 0xFF00) | ((h >> 24) & 0xFF);
}

// ---------------------------------------------------------------------------
// UDP checksum (#404 / #486 Phase D): Rust strangler, STAGED.
//
// IMPORTANT HONEST NOTE: the live udp_send() path below sets checksum = 0 (a
// UDP checksum of 0 is IPv4-optional and legally means "no checksum"), and this
// change does NOT alter that behavior - the C kernel never computed a UDP
// checksum, so there was no live udp_checksum symbol to strangle. What Phase D
// adds is a proven Rust UDP checksum (udp_checksum_rs in rustkern.rs) plus a C
// reference (udp_checksum_c) and a boot-time [RUST-DIFF] self-test that proves
// them byte-identical over a bounded corpus. udp_checksum() is the dispatcher
// (routes to Rust under -DRUST_UDP_CHECKSUM) ready for a FUTURE opt-in that
// would enable UDP checksums on transmit; it is deliberately NOT called from
// udp_send() yet, so this build preserves the exact current wire behavior.
// src_ip / dest_ip are network byte order (as the eventual caller would pass).
extern uint16_t udp_checksum_rs(uint32_t src_ip, uint32_t dest_ip,
                                const void *udp_data, uint16_t udp_length);

uint16_t udp_checksum_c(uint32_t src_ip, uint32_t dest_ip,
                        const void *udp_data, uint16_t udp_length) {
    uint32_t sum = 0;
    const uint16_t *ptr;

    // IPv4 pseudo-header (src/dst IP already network byte order).
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dest_ip >> 16) & 0xFFFF;
    sum += dest_ip & 0xFFFF;
    sum += htons(IP_PROTO_UDP);
    sum += htons(udp_length);

    // UDP header + data.
    ptr = (const uint16_t *)udp_data;
    uint16_t len = udp_length;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len > 0) {
        sum += *(const uint8_t *)ptr;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    uint16_t cs = (uint16_t)(~sum);
    // RFC 768: a computed 0 is transmitted as 0xFFFF (0 means "no checksum").
    return cs == 0 ? 0xFFFF : cs;
}

// Dispatcher (strangler): Rust under -DRUST_UDP_CHECKSUM, else C. NOT yet on the
// live transmit path (see note above); staged for a future checksum-on opt-in.
uint16_t udp_checksum(uint32_t src_ip, uint32_t dest_ip,
                      const void *udp_data, uint16_t udp_length) {
#ifdef RUST_UDP_CHECKSUM
    return udp_checksum_rs(src_ip, dest_ip, udp_data, udp_length);
#else
    return udp_checksum_c(src_ip, dest_ip, udp_data, udp_length);
#endif
}

// Boot-time differential self-test (#404 / #486 Phase D): udp_checksum_rs ==
// udp_checksum_c over a bounded corpus (every length 0..1500 once + random
// vectors + real DNS/DHCP-shaped header samples). Bounded, once, no busy-wait
// (#426). References udp_checksum_rs unconditionally so the Rust member links.
static inline uint32_t udp_rustdiff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

void udp_checksum_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    static uint8_t buf[1500];

    uint32_t seed = 0x51fa6d33;
    uint32_t vectors = 0, mismatches = 0;
    int first_bad_len = -1;
    uint16_t first_bad_rs = 0, first_bad_c = 0;

    // Pass 1: every length 0..1500 once, random src/dst IP + random content.
    for (int len = 0; len <= 1500; len++) {
        for (int i = 0; i < len; i++) {
            buf[i] = (uint8_t)(udp_rustdiff_rng(&seed) & 0xFF);
        }
        uint32_t s = udp_rustdiff_rng(&seed);
        uint32_t d = udp_rustdiff_rng(&seed);
        uint16_t rs = udp_checksum_rs(s, d, buf, (uint16_t)len);
        uint16_t c = udp_checksum_c(s, d, buf, (uint16_t)len);
        vectors++;
        if (rs != c) {
            mismatches++;
            if (first_bad_len < 0) { first_bad_len = len; first_bad_rs = rs; first_bad_c = c; }
        }
    }

    // Pass 2: random length in [0,1500] with fresh random content + IPs.
    for (int n = 0; n < 4096; n++) {
        int len = (int)(udp_rustdiff_rng(&seed) % 1501);
        for (int i = 0; i < len; i++) {
            buf[i] = (uint8_t)(udp_rustdiff_rng(&seed) & 0xFF);
        }
        uint32_t s = udp_rustdiff_rng(&seed);
        uint32_t d = udp_rustdiff_rng(&seed);
        uint16_t rs = udp_checksum_rs(s, d, buf, (uint16_t)len);
        uint16_t c = udp_checksum_c(s, d, buf, (uint16_t)len);
        vectors++;
        if (rs != c) {
            mismatches++;
            if (first_bad_len < 0) { first_bad_len = len; first_bad_rs = rs; first_bad_c = c; }
        }
    }

    // Pass 3: real UDP-shaped headers (DNS query :53, DHCP :67/:68) that also
    // exercise the RFC 768 0x0000 -> 0xFFFF special case naturally.
    {
        // 8-byte UDP header (src 0x0044=68, dst 0x0043=67, len, cksum=0) + body.
        int sizes[6] = { 8, 9, 20, 53, 300, 576 };
        uint32_t s = udp_htonl(0xC0A80102); // a private-range test address
        uint32_t d = udp_htonl(0xC0A80101); // a private-range test gateway
        for (int k = 0; k < 6; k++) {
            int len = sizes[k];
            buf[0] = 0x00; buf[1] = 0x44; buf[2] = 0x00; buf[3] = 0x43;
            buf[4] = (uint8_t)(len >> 8); buf[5] = (uint8_t)(len & 0xFF);
            buf[6] = 0x00; buf[7] = 0x00;
            for (int i = 8; i < len; i++) buf[i] = (uint8_t)(0x30 + (i & 0x1F));
            uint16_t rs = udp_checksum_rs(s, d, buf, (uint16_t)len);
            uint16_t c = udp_checksum_c(s, d, buf, (uint16_t)len);
            vectors++;
            if (rs != c) {
                mismatches++;
                if (first_bad_len < 0) { first_bad_len = len; first_bad_rs = rs; first_bad_c = c; }
            }
        }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] udp_checksum: %u vectors, %u mismatches -> %s\n",
            vectors, mismatches, verdict);
    bootlog_write("[RUST-DIFF] udp_checksum: %u vectors, %u mismatches -> %s",
                  vectors, mismatches, verdict);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] udp FIRST MISMATCH len=%d rs=0x%04x c=0x%04x\n",
                first_bad_len, first_bad_rs, first_bad_c);
        bootlog_write("[RUST-DIFF] udp FIRST MISMATCH len=%d rs=0x%04x c=0x%04x",
                      first_bad_len, first_bad_rs, first_bad_c);
    }
}

// Initialize UDP
void udp_init(void) {
    for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
        bindings[i].active = 0;
    }

    // Register with IP layer
    ip_register_handler(IP_PROTO_UDP, udp_handle);

    kprintf("[UDP] UDP initialized\n");
}

// Bind a port to a callback
int udp_bind(uint16_t port, udp_callback_t callback) {
    // Check if already bound
    for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
        if (bindings[i].active && bindings[i].port == port) {
            return -1;  // Already bound
        }
    }

    // Find free slot
    for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
        if (!bindings[i].active) {
            bindings[i].port = port;
            bindings[i].callback = callback;
            bindings[i].active = 1;
            return 0;
        }
    }

    return -1;  // No free slots
}

// Unbind a port
void udp_unbind(uint16_t port) {
    for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
        if (bindings[i].active && bindings[i].port == port) {
            bindings[i].active = 0;
            return;
        }
    }
}

// Send UDP packet
int udp_send(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port,
             const void *data, uint16_t length) {
    // Build UDP packet (static to reduce stack usage)
    static uint8_t packet[1500];
    udp_header_t *header = (udp_header_t *)packet;

    uint16_t total_len = sizeof(udp_header_t) + length;
    if (total_len > 1500) {
        return -1;  // Too large
    }

    header->src_port = htons(src_port);
    header->dest_port = htons(dest_port);
    header->length = htons(total_len);
    header->checksum = 0;  // Optional in IPv4, set to 0

    // Copy data
    if (length > 0 && data != 0) {
        memcpy(packet + sizeof(udp_header_t), data, length);
    }

    // Send via IP
    return ip_send(dest_ip, IP_PROTO_UDP, packet, total_len);
}

// Handle incoming UDP packet
void udp_handle(uint32_t src_ip, const void *data, uint16_t length) {
    if (length < sizeof(udp_header_t)) {
        return;
    }

    udp_header_t *header = (udp_header_t *)data;
    uint16_t dest_port = ntohs(header->dest_port);
    uint16_t src_port = ntohs(header->src_port);
    uint16_t udp_len = ntohs(header->length);

    if (udp_len > length || udp_len < sizeof(udp_header_t)) {
        return;
    }

    uint16_t data_len = udp_len - sizeof(udp_header_t);
    const void *payload = (const uint8_t *)data + sizeof(udp_header_t);

    // Find binding for this port
    for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
        if (bindings[i].active && bindings[i].port == dest_port) {
            bindings[i].callback(src_ip, src_port, payload, data_len);
            return;
        }
    }

    // No handler for this port - silently drop
}
