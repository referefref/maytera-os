// ip.c - Internet Protocol (IPv4) implementation
#include "ip.h"
#include "ethernet.h"
#include "arp.h"
#include "../serial.h"
#include "../string.h"

// Network configuration
static uint32_t our_ip = 0;
static uint32_t gateway_ip = 0;
static uint32_t netmask = 0;

// IP identification counter
static uint16_t ip_id = 0;

// Protocol handlers
#define MAX_IP_HANDLERS 8
static struct {
    uint8_t protocol;
    ip_handler_t handler;
} ip_handlers[MAX_IP_HANDLERS];
static int ip_handler_count = 0;

// Byte swap
static inline uint16_t htons(uint16_t h) {
    return ((h & 0xFF) << 8) | ((h >> 8) & 0xFF);
}

static inline uint16_t ntohs(uint16_t n) {
    return htons(n);
}

static inline uint32_t htonl(uint32_t h) {
    return ((h & 0xFF) << 24) | ((h & 0xFF00) << 8) |
           ((h >> 8) & 0xFF00) | ((h >> 24) & 0xFF);
}

// Rust port of the checksum lives in rustkern.rs (#404 / #479 Phase B). It is a
// drop-in for ip_checksum_c: same ones-complement RFC 1071 result, proven
// byte-for-byte identical over 207,507 vectors in the PoC and re-proven on this
// build by ip_checksum_rust_selftest() below. `*const u8` in Rust and
// `const void *` here are ABI-identical (both a pointer), and u16 == uint16_t.
extern uint16_t ip_checksum_rs(const void *data, uint16_t length);

// Original C implementation, renamed ip_checksum_c so BOTH the C and the Rust
// versions stay callable in the same build. This is the trivial rollback: if
// RUST_IP_CHECKSUM is undefined the live symbol falls straight back to this.
uint16_t ip_checksum_c(const void *data, uint16_t length);

// Live callers still call ip_checksum(). With -DRUST_IP_CHECKSUM (set in the
// Makefile CFLAGS for build 792), the real symbol routes to the Rust impl;
// otherwise it routes to the original C. Callers are unchanged: the strangler
// pattern proven in the PoC (net/ip.c:49-55). Rollback = drop the flag, rebuild.
uint16_t ip_checksum(const void *data, uint16_t length) {
#ifdef RUST_IP_CHECKSUM
    return ip_checksum_rs(data, length);
#else
    return ip_checksum_c(data, length);
#endif
}

// Calculate IP checksum (original C, RFC 1071 ones-complement). Kept as the
// rollback path and the differential reference for the boot-time self-test.
uint16_t ip_checksum_c(const void *data, uint16_t length) {
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;

    while (length > 1) {
        sum += *ptr++;
        length -= 2;
    }

    if (length > 0) {
        sum += *(uint8_t *)ptr;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

// ---------------------------------------------------------------------------
// Boot-time differential self-test (#404 / #479 Phase B).
// Asserts ip_checksum_rs == ip_checksum_c over a bounded corpus on THIS exact
// build, then logs ONE [RUST-DIFF] line to serial (kprintf) and the persistent
// /BOOTLOG.TXT (bootlog_write). Bounded and fast: every length 0..MAXLEN once
// with a deterministic pattern, plus a fixed number of random-length/random-
// content vectors. No busy-wait / no blocking (#426): a straight compute loop
// that runs once at boot. Any mismatch => FAIL (reported, does not proceed to
// trust the Rust path silently). Runs regardless of RUST_IP_CHECKSUM so the
// two impls are always compared; it also force-references ip_checksum_rs so the
// Rust archive member is pulled into the link.
#define RUSTDIFF_MAXLEN 1500
static uint8_t rustdiff_buf[RUSTDIFF_MAXLEN];

// Small deterministic PRNG (xorshift32) so the corpus is reproducible build to
// build and the [RUST-DIFF] result is not luck-of-the-seed.
static inline uint32_t rustdiff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

void ip_checksum_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);

    uint32_t seed = 0x1a2b3c4d;
    uint32_t vectors = 0;
    uint32_t mismatches = 0;
    int first_bad_len = -1;
    uint16_t first_bad_rs = 0, first_bad_c = 0;

    // Pass 1: every length 0..MAXLEN exactly once, filled with a pseudo-random
    // byte pattern (covers the odd-trailing-byte path and full carry folding at
    // every length boundary, including the empty and 1-byte edge cases).
    for (int len = 0; len <= RUSTDIFF_MAXLEN; len++) {
        for (int i = 0; i < len; i++) {
            rustdiff_buf[i] = (uint8_t)(rustdiff_rng(&seed) & 0xFF);
        }
        uint16_t rs = ip_checksum_rs(rustdiff_buf, (uint16_t)len);
        uint16_t c = ip_checksum_c(rustdiff_buf, (uint16_t)len);
        vectors++;
        if (rs != c) {
            mismatches++;
            if (first_bad_len < 0) {
                first_bad_len = len;
                first_bad_rs = rs;
                first_bad_c = c;
            }
        }
    }

    // Pass 2: random length in [0,MAXLEN] with fresh random content, a few
    // thousand vectors so the differential is not just monotone-length driven.
    for (int n = 0; n < 4096; n++) {
        int len = (int)(rustdiff_rng(&seed) % (RUSTDIFF_MAXLEN + 1));
        for (int i = 0; i < len; i++) {
            rustdiff_buf[i] = (uint8_t)(rustdiff_rng(&seed) & 0xFF);
        }
        uint16_t rs = ip_checksum_rs(rustdiff_buf, (uint16_t)len);
        uint16_t c = ip_checksum_c(rustdiff_buf, (uint16_t)len);
        vectors++;
        if (rs != c) {
            mismatches++;
            if (first_bad_len < 0) {
                first_bad_len = len;
                first_bad_rs = rs;
                first_bad_c = c;
            }
        }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] ip_checksum: %u vectors, %u mismatches -> %s\n",
            vectors, mismatches, verdict);
    bootlog_write("[RUST-DIFF] ip_checksum: %u vectors, %u mismatches -> %s",
                  vectors, mismatches, verdict);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] FIRST MISMATCH len=%d rs=0x%04x c=0x%04x\n",
                first_bad_len, first_bad_rs, first_bad_c);
        bootlog_write("[RUST-DIFF] FIRST MISMATCH len=%d rs=0x%04x c=0x%04x",
                      first_bad_len, first_bad_rs, first_bad_c);
    }
}

// Initialize IP
void ip_init(void) {
    our_ip = 0;
    gateway_ip = 0;
    netmask = 0;
    ip_id = 0;
    ip_handler_count = 0;

    // Register with ethernet layer
    eth_register_handler(ETH_TYPE_IPV4, ip_handle);

    kprintf("[IP] IP initialized\n");
}

// Set our IP address
void ip_set_address(uint32_t ip) {
    our_ip = ip;
    uint8_t *p = (uint8_t *)&ip;
    kprintf("[IP] Address set to %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);
}

// Get our IP address
uint32_t ip_get_address(void) {
    return our_ip;
}

// Set gateway
void ip_set_gateway(uint32_t gateway) {
    gateway_ip = gateway;
    uint8_t *p = (uint8_t *)&gateway;
    kprintf("[IP] Gateway set to %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);
}

// Get gateway
uint32_t ip_get_gateway(void) {
    return gateway_ip;
}

// Set subnet mask
void ip_set_netmask(uint32_t mask) {
    netmask = mask;
    uint8_t *p = (uint8_t *)&mask;
    kprintf("[IP] Netmask set to %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);
}

// Get subnet mask
uint32_t ip_get_netmask(void) {
    return netmask;
}

// Check if IP is on local network
static int ip_is_local(uint32_t ip) {
    if (netmask == 0) return 1;  // No netmask, assume local
    return (ip & netmask) == (our_ip & netmask);
}

// Send IP packet
int ip_send(uint32_t dest_ip, uint8_t protocol, const void *data, uint16_t length) {
    extern void serial_puts(uint16_t port, const char *str);

    // kprintf("[IP] ip_send: dest_ip=0x%08x proto=%d len=%d\n", dest_ip, protocol, length);

    if (!data) {
        serial_puts(0x3F8, "[IP] ip_send: null data\r\n");
        return -1;
    }

    // Allow broadcast even without IP (for DHCP)
    if (dest_ip == 0xFFFFFFFF) {
        return ip_send_broadcast(protocol, data, length);
    }

    if (our_ip == 0) {
        serial_puts(0x3F8, "[IP] ip_send: no IP set\r\n");
        return -1;  // Need IP for unicast
    }

    if (length > IP_MTU - IP_HEADER_SIZE) {
        serial_puts(0x3F8, "[IP] ip_send: too large\r\n");
        return -1;  // Too large (fragmentation not supported)
    }

    // Build IP packet (static to reduce stack usage)
    static uint8_t packet[IP_MTU];
    ip_header_t *header = (ip_header_t *)packet;

    header->version_ihl = 0x45;  // IPv4, 5 32-bit words (no options)
    header->tos = 0;
    header->total_length = htons(IP_HEADER_SIZE + length);
    header->id = htons(ip_id++);
    header->flags_frag = htons(IP_FLAG_DF);  // Don't fragment
    header->ttl = 64;
    header->protocol = protocol;
    header->checksum = 0;
    // Both our_ip and dest_ip are in host byte order
    // Convert both to network byte order for the IP header
    header->src_ip = htonl(our_ip);
    header->dest_ip = htonl(dest_ip);

    // Calculate checksum
    header->checksum = ip_checksum(header, IP_HEADER_SIZE);

    // Copy data
    memcpy(packet + IP_HEADER_SIZE, data, length);

    // Determine next hop (gateway or direct) - all in host byte order
    uint32_t next_hop_host = ip_is_local(dest_ip) ? dest_ip : gateway_ip;
    if (next_hop_host == 0) {
        next_hop_host = dest_ip;  // No gateway, try direct
    }

    // uint8_t *nhp = (uint8_t *)&next_hop_host;
    // kprintf("[IP] ip_send: next_hop=0x%08x (%d.%d.%d.%d)\n",
    //         next_hop_host, nhp[3], nhp[2], nhp[1], nhp[0]);

    // ARP resolve
    uint8_t dest_mac[6];
    if (!arp_resolve(next_hop_host, dest_mac)) {
        // #333: ARP request has been sent. Instead of DROPPING this packet (which
        // silently lost the first SYN to a cold LAN host such as the Home
        // Assistant server), HOLD it and let arp_flush_pending() put it on the
        // wire the instant the reply lands. We still return -1 so the caller's
        // existing in-progress/retransmit handling is unchanged.
        arp_queue_pending(next_hop_host, ETH_TYPE_IPV4, packet, IP_HEADER_SIZE + length);
        return -1;
    }

    // kprintf("[IP] ip_send: dest_mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
    //         dest_mac[0], dest_mac[1], dest_mac[2], dest_mac[3], dest_mac[4], dest_mac[5]);

    // Send via ethernet
    int result = eth_send(dest_mac, ETH_TYPE_IPV4, packet, IP_HEADER_SIZE + length);
    // kprintf("[IP] ip_send: eth_send returned %d\n", result);
    return result;
}

// Handle incoming IP packet
void ip_handle(const uint8_t *src_mac, const void *data, uint16_t length) {
    extern void serial_puts(uint16_t port, const char *str);
    (void)src_mac;

    // serial_puts(0x3F8, "[IP] ip_handle called\r\n");

    if (length < IP_HEADER_SIZE) {
        serial_puts(0x3F8, "[IP] REJECT: too short\r\n");
        return;
    }

    ip_header_t *header = (ip_header_t *)data;

    // Validate version
    if ((header->version_ihl >> 4) != 4) {
        serial_puts(0x3F8, "[IP] REJECT: not IPv4\r\n");
        return;  // Not IPv4
    }

    // Get header length
    uint8_t ihl = (header->version_ihl & 0x0F) * 4;
    if (ihl < IP_HEADER_SIZE || ihl > length) {
        serial_puts(0x3F8, "[IP] REJECT: bad IHL\r\n");
        return;
    }

    // Verify checksum
    uint16_t original_checksum = header->checksum;
    header->checksum = 0;
    uint16_t computed_checksum = ip_checksum(header, ihl);
    if (computed_checksum != original_checksum) {
        header->checksum = original_checksum;
        kprintf("[IP] REJECT: bad checksum (got 0x%04x, expected 0x%04x)\n",
                computed_checksum, original_checksum);
        return;  // Bad checksum
    }
    header->checksum = original_checksum;

    // Check destination
    // Accept if:
    // - Packet is for our IP
    // - Packet is broadcast (255.255.255.255)
    // - We have no IP yet (DHCP mode) - accept any packet to our MAC
    // Note: our_ip is stored in the format passed to ip_set_address,
    // and header->dest_ip is in network byte order from the packet.
    // We need to convert header->dest_ip to compare with our_ip.
    uint32_t dest_ip_host = htonl(header->dest_ip);  // Convert network->host (htonl == ntohl on x86)

    // Debug: show first few packets
    static int ip_rx_debug = 0;
    if (ip_rx_debug < 10) {
        ip_rx_debug++;
        kprintf("[IP] rx: dest_ip_raw=0x%08x, dest_ip_host=0x%08x, our_ip=0x%08x\n",
                header->dest_ip, dest_ip_host, our_ip);
    }

    if (dest_ip_host != our_ip &&
        header->dest_ip != 0xFFFFFFFF &&
        our_ip != 0) {
        // serial_puts(0x3F8, "[IP] REJECT: not for us\r\n");
        return;  // Not for us
    }

    // serial_puts(0x3F8, "[IP] ACCEPT: packet for us!\r\n");

    // Get payload
    uint16_t total_length = ntohs(header->total_length);
    if (total_length > length) {
        serial_puts(0x3F8, "[IP] REJECT: truncated\r\n");
        return;
    }
    // SECURITY (MAYTERA-SEC-2026-00XX, task #5XX): total_length counts the IP
    // HEADER as well as the payload, so it MUST be >= ihl. The guards above
    // bound ihl (<= length) and total_length (<= length) but NEVER relate the
    // two, so a header declaring IHL=15 (60 bytes) with total_length=20 made
    // the uint16 subtraction below WRAP: payload_length = 20 - 60 = 65496. That
    // length was then handed to EVERY registered protocol handler (icmp_handle,
    // udp_handle, tcp_handle) on a frame only 60 bytes long, i.e. a ~64 KB
    // over-read of adjacent heap from a single crafted LAN packet.
    // CWE-191 (integer underflow) -> CWE-125 (out-of-bounds read). Plain C, and
    // correct regardless of any Rust seam: net/ip.c is not ported, so this is
    // the root cause and not something -DRUST_ICMP can confine.
    if (total_length < ihl) {
        serial_puts(0x3F8, "[IP] REJECT: total_length < IHL\r\n");
        return;
    }

    const uint8_t *payload = (const uint8_t *)data + ihl;
    uint16_t payload_length = total_length - ihl;

    // kprintf("[IP] protocol=%d payload_len=%d\n", header->protocol, payload_length);

    // Find handler
    for (int i = 0; i < ip_handler_count; i++) {
        if (ip_handlers[i].protocol == header->protocol && ip_handlers[i].handler) {
            // kprintf("[IP] Calling handler for protocol %d\n", header->protocol);
            ip_handlers[i].handler(header->src_ip, payload, payload_length);
            return;
        }
    }
    // serial_puts(0x3F8, "[IP] No handler for proto!\r\n");
}

// Register protocol handler
void ip_register_handler(uint8_t protocol, ip_handler_t handler) {
    if (ip_handler_count >= MAX_IP_HANDLERS) {
        return;
    }
    ip_handlers[ip_handler_count].protocol = protocol;
    ip_handlers[ip_handler_count].handler = handler;
    ip_handler_count++;
}

// Print IP address (host byte order - high byte is first octet)
void ip_print(uint32_t ip) {
    uint8_t *p = (uint8_t *)&ip;
    // On little-endian, bytes are stored reversed, so print backwards
    kprintf("%d.%d.%d.%d", p[3], p[2], p[1], p[0]);
}

// Send broadcast IP packet (for DHCP when we don't have IP yet)
int ip_send_broadcast(uint8_t protocol, const void *data, uint16_t length) {
    static uint8_t packet[IP_MTU];  // Static to reduce stack usage

    if (length > IP_MTU - IP_HEADER_SIZE) {
        return -1;  // Too large
    }

    ip_header_t *header = (ip_header_t *)packet;

    header->version_ihl = 0x45;  // IPv4, 5 32-bit words
    header->tos = 0;
    header->total_length = htons(IP_HEADER_SIZE + length);
    header->id = htons(ip_id++);
    header->flags_frag = 0;
    header->ttl = 64;
    header->protocol = protocol;
    header->checksum = 0;
    header->src_ip = htonl(our_ip);  // Convert to network byte order
    header->dest_ip = 0xFFFFFFFF;  // Broadcast

    // Calculate checksum
    header->checksum = ip_checksum(header, IP_HEADER_SIZE);

    // Copy data
    memcpy(packet + IP_HEADER_SIZE, data, length);

    // Send to broadcast MAC
    uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return eth_send(broadcast_mac, ETH_TYPE_IPV4, packet, IP_HEADER_SIZE + length);
}
