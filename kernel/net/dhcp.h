// dhcp.h - Dynamic Host Configuration Protocol
#ifndef DHCP_H
#define DHCP_H

#include "../types.h"

// DHCP message types
#define DHCP_DISCOVER   1
#define DHCP_OFFER      2
#define DHCP_REQUEST    3
#define DHCP_DECLINE    4
#define DHCP_ACK        5
#define DHCP_NAK        6
#define DHCP_RELEASE    7

// DHCP ports
#define DHCP_SERVER_PORT    67
#define DHCP_CLIENT_PORT    68

// DHCP options
#define DHCP_OPT_PAD            0
#define DHCP_OPT_SUBNET_MASK    1
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_DNS            6
#define DHCP_OPT_HOSTNAME       12
#define DHCP_OPT_REQUESTED_IP   50
#define DHCP_OPT_LEASE_TIME     51
#define DHCP_OPT_MSG_TYPE       53
#define DHCP_OPT_SERVER_ID      54
#define DHCP_OPT_PARAM_LIST     55
#define DHCP_OPT_END            255

// DHCP header (BOOTP format)
typedef struct {
    uint8_t  op;            // Message type: 1=request, 2=reply
    uint8_t  htype;         // Hardware type: 1=Ethernet
    uint8_t  hlen;          // Hardware address length: 6 for Ethernet
    uint8_t  hops;          // Hops
    uint32_t xid;           // Transaction ID
    uint16_t secs;          // Seconds elapsed
    uint16_t flags;         // Flags
    uint32_t ciaddr;        // Client IP address
    uint32_t yiaddr;        // Your (client) IP address
    uint32_t siaddr;        // Server IP address
    uint32_t giaddr;        // Gateway IP address
    uint8_t  chaddr[16];    // Client hardware address
    uint8_t  sname[64];     // Server hostname
    uint8_t  file[128];     // Boot filename
    uint32_t magic;         // Magic cookie (0x63825363)
    uint8_t  options[308];  // Options
} __attribute__((packed)) dhcp_packet_t;

// DHCP state
#define DHCP_STATE_IDLE         0
#define DHCP_STATE_DISCOVERING  1
#define DHCP_STATE_REQUESTING   2
#define DHCP_STATE_BOUND        3

// ---------------------------------------------------------------------------
// #404 / #497 Phase O (Tier 2, untrusted wire input): pure DHCP reply-parse seam.
//
// dhcp_parse() is the PURE parse/validate of an incoming DHCP OFFER/ACK/NAK
// extracted out of dhcp_handle(): it checks the fixed BOOTP header (op==2 reply,
// magic cookie) and walks the option area (magic cookie then repeated
// [type][len][len bytes]; 0xFF = END, 0x00 = PAD), extracting the fields the
// kernel uses. It does NOT send, allocate, touch a global, or (in the Rust path)
// mutate the input buffer. The socket / state machine (DISCOVER/REQUEST send,
// xid match, DAD, bind) stays in dhcp.c. This is the untrusted-input surface:
// buf/len are an attacker-spoofable UDP payload off the LAN, so the option walk
// must bounds-check every length against len. Routed to dhcp_parse_rs
// (rustkern.rs) under -DRUST_DHCP.
//
// Parsed, validated view of an incoming DHCP reply. #[repr(C)] mirror lives in
// rustkern.rs (DhcpParsed); size asserted == 36 in dhcp.c so the FFI layout can
// never silently drift. All 32-bit address/lease fields are HOST byte order
// (== ntohl of the wire field / option value), exactly as the C code produced.
typedef struct {
    uint32_t xid;             // BOOTP xid, host order (caller compares to dhcp_xid)
    uint32_t yiaddr;          // "your" (offered) IP, host order
    uint32_t subnet;          // option 1  (host order), valid iff have_subnet
    uint32_t router;          // option 3  (host order), valid iff have_router
    uint32_t dns;             // option 6  (host order), valid iff have_dns
    uint32_t lease;           // option 51 (host order, seconds), valid iff have_lease
    uint32_t server_id;       // option 54 (host order), valid iff have_server_id
    uint8_t  msg_type;        // option 53 (DHCP message type), 0 if absent
    uint8_t  have_subnet;
    uint8_t  have_router;
    uint8_t  have_dns;
    uint8_t  have_lease;
    uint8_t  have_server_id;
    uint8_t  _pad[2];         // pad to 36 bytes (matches the Rust #[repr(C)])
} dhcp_parsed_t;

// dhcp_parse return codes.
#define DHCP_PARSE_OK          0    // structurally valid reply; *out filled
#define DHCP_PARSE_ETOOSHORT  -1    // len < sizeof(dhcp_packet_t) - 308 (BOOTP header + magic == 240)
#define DHCP_PARSE_ENOTREPLY  -2    // op != 2 (not a BOOTP reply)
#define DHCP_PARSE_EBADMAGIC  -3    // magic cookie != 63 82 53 63 on the wire

// Pure parse/validate dispatcher (routes to _rs under -DRUST_DHCP, else _c).
int dhcp_parse(const uint8_t *buf, uint32_t len, dhcp_parsed_t *out);
// Verbatim C reference (the current option-walk logic, INCLUDING the fixed-308
// walk that ignores `len`) kept for the differential + rollback.
int dhcp_parse_c(const uint8_t *buf, uint32_t len, dhcp_parsed_t *out);
// Rust port (rustkern.rs): immutable slice over exactly len bytes, the option
// walk is bounds-checked against len so a lying/oversized option length rejects
// instead of reading past the received packet; never mutates the buffer.
extern int dhcp_parse_rs(const uint8_t *buf, uint32_t len, dhcp_parsed_t *out);

// Boot-time differential + perf + security self-test (logs [RUST-DIFF] dhcp,
// [RUST-PERF] dhcp, [RUST-SEC] dhcp). Runs regardless of -DRUST_DHCP.
void dhcp_rust_selftest(void);

// Initialize DHCP client
void dhcp_init(void);

// Start DHCP discovery
int dhcp_discover(void);

// Start DHCP discovery and wait for completion (blocking)
int dhcp_discover_blocking(void);

// Get DHCP state
int dhcp_get_state(void);

// Check if DHCP is complete (bound)
int dhcp_is_bound(void);

// Get assigned IP address
uint32_t dhcp_get_ip(void);

// Get assigned gateway
uint32_t dhcp_get_gateway(void);

// Get assigned netmask
uint32_t dhcp_get_netmask(void);

// Get DNS server
uint32_t dhcp_get_dns(void);

// Process DHCP (call regularly)
void dhcp_poll(void);

#endif // DHCP_H
