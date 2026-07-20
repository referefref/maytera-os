// arp.h - Address Resolution Protocol
#ifndef ARP_H
#define ARP_H

#include "../types.h"

// ARP header
typedef struct {
    uint16_t hw_type;       // Hardware type (1 = Ethernet)
    uint16_t proto_type;    // Protocol type (0x0800 = IPv4)
    uint8_t  hw_len;        // Hardware address length (6 for MAC)
    uint8_t  proto_len;     // Protocol address length (4 for IPv4)
    uint16_t operation;     // Operation (1 = request, 2 = reply)
    uint8_t  sender_mac[6]; // Sender MAC address
    uint8_t  sender_ip[4];  // Sender IP address
    uint8_t  target_mac[6]; // Target MAC address
    uint8_t  target_ip[4];  // Target IP address
} __attribute__((packed)) arp_header_t;

// ARP operations
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

// ARP hardware types
#define ARP_HW_ETHERNET 1

// ---------------------------------------------------------------------------
// #404 / #495 Phase M (Tier 2, untrusted wire input): pure ARP parse seam.
//
// arp_parse() is the PURE parse/validate of an incoming ARP frame extracted out
// of arp_handle(). It does NOT send, mutate the ARP cache, run DAD, or touch the
// input buffer. The reply-send / cache-add / DAD bookkeeping stays in arp_handle
// (C). This is the untrusted-input surface: buf/len come off the wire
// (attacker-controlled), so length validation and every field read must be
// bounds-checked. Routed to arp_parse_rs (rustkern.rs) under -DRUST_ARP.
//
// Parsed, validated view of an incoming ARP frame. #[repr(C)] mirror lives in
// rustkern.rs (ArpParsed); size asserted == 28 in arp.c so the FFI layout can
// never silently drift. hw_type/proto_type/operation are decoded to HOST order
// (ntohs), sender_ip/target_ip to HOST order (ntohl) exactly as the old
// arp_handle read them; sender_ip_be keeps the RAW network bytes so the C reply
// path can copy them verbatim into reply.target_ip (as the old code did).
typedef struct {
    uint16_t hw_type;        // host order (ntohs of the wire field)
    uint16_t proto_type;     // host order
    uint16_t operation;      // host order (1 = request, 2 = reply)
    uint8_t  hw_len;         // raw (validated == 6)
    uint8_t  proto_len;      // raw (validated == 4)
    uint32_t sender_ip;      // host order (ntohl)
    uint32_t target_ip;      // host order (ntohl)
    uint8_t  sender_mac[6];  // raw sender hardware address
    uint8_t  sender_ip_be[4];// RAW network-order sender IP bytes (for the reply)
} arp_parsed_t;

// arp_parse return codes.
#define ARP_PARSE_OK          0    // structurally valid Ethernet/IPv4 ARP
#define ARP_PARSE_ETOOSHORT  -1    // len < sizeof(arp_header_t)
#define ARP_PARSE_EINVAL     -2    // hw/proto type or hlen/plen not Ethernet/IPv4

// Pure parse/validate dispatcher (routes to _rs under -DRUST_ARP, else _c).
int arp_parse(const uint8_t *buf, uint32_t len, arp_parsed_t *out);
// Verbatim C reference (current arp_handle logic, read-only) kept for the
// differential + rollback (undefine RUST_ARP -> arp_parse() falls back to it).
int arp_parse_c(const uint8_t *buf, uint32_t len, arp_parsed_t *out);
// Rust port (rustkern.rs): immutable slice over exactly len bytes, every field
// access bounds-checked; rejects truncated instead of reading past the frame.
extern int arp_parse_rs(const uint8_t *buf, uint32_t len, arp_parsed_t *out);

// Boot-time differential + perf + security self-test (logs [RUST-DIFF] arp,
// [RUST-PERF] arp, [RUST-SEC] arp). Runs regardless of -DRUST_ARP.
void arp_rust_selftest(void);

// Initialize ARP
void arp_init(void);

// Send ARP request for an IP
void arp_request(uint32_t ip);

// Resolve IP to MAC (returns 1 if found, 0 if pending)
int arp_resolve(uint32_t ip, uint8_t *mac);

// #333: hold a packet whose next-hop MAC is not yet resolved; it is flushed
// by arp_flush_ready() (called from net_poll) once the ARP reply lands. type is
// the ethertype.
void arp_queue_pending(uint32_t next_hop_ip, uint16_t type, const void *data, uint16_t len);

// #333/#747: flush held packets whose MAC is now known. Call ONLY at top level
// (net_poll), never from inside the ARP/receive callback.
void arp_flush_ready(void);

// Handle incoming ARP packet
void arp_handle(const uint8_t *src_mac, const void *data, uint16_t length);

// Add static ARP entry
void arp_add_entry(uint32_t ip, const uint8_t *mac);

// Print ARP table
void arp_print_table(void);
void arp_announce(void);

// #380 Duplicate Address Detection (RFC 5227).
void arp_probe(uint32_t ip);        // send a probe (sender IP 0.0.0.0)
int  arp_ip_in_use(uint32_t ip);    // synchronous probe+listen; 1 = in use
void arp_dad_arm(uint32_t ip);      // arm async watcher for `ip`
int  arp_dad_conflict(void);        // read async conflict flag
void arp_dad_disarm(void);          // disarm async watcher

#endif // ARP_H
