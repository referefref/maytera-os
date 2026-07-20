// icmp.h - Internet Control Message Protocol
#ifndef ICMP_H
#define ICMP_H

#include "../types.h"

// ICMP header
typedef struct {
    uint8_t  type;          // ICMP type
    uint8_t  code;          // ICMP code
    uint16_t checksum;      // Checksum
    uint16_t id;            // Identifier
    uint16_t sequence;      // Sequence number
} __attribute__((packed)) icmp_header_t;

// ICMP types
#define ICMP_ECHO_REPLY     0
#define ICMP_ECHO_REQUEST   8

// ---------------------------------------------------------------------------
// #404 / #494 Phase L (Tier 2, untrusted wire input): pure ICMP parse seam.
//
// icmp_parse() is the PURE parse/validate of an incoming ICMP packet extracted
// out of icmp_handle(). It does NOT send, allocate, or (in the Rust path)
// mutate the input buffer. The reply-send / echo-reply construction stays in
// icmp_handle() (C). This is the untrusted-input surface: buf/len come from the
// wire (attacker-controlled), so length validation and every field read must be
// bounds-checked. Routed to icmp_parse_rs (rustkern.rs) under -DRUST_ICMP.

// Parsed, validated view of an incoming ICMP packet. #[repr(C)] mirror lives in
// rustkern.rs (IcmpParsed); size asserted == 16 in icmp.c so the FFI layout can
// never silently drift. All multi-byte fields are the RAW packet values in
// native (little-endian on x86-64) order, exactly as the C struct read them.
typedef struct {
    uint8_t  type;          // ICMP type   (buf[0])
    uint8_t  code;          // ICMP code   (buf[1])
    uint16_t checksum;      // checksum field as received (raw)
    uint16_t id;            // id field    (raw, network order)
    uint16_t sequence;      // sequence    (raw, network order)
    uint16_t total_len;     // validated total ICMP length (== len)
    uint16_t payload_off;   // payload offset (== sizeof(icmp_header_t) == 8)
    uint16_t payload_len;   // len - 8
    uint8_t  checksum_ok;   // 1 if the ones-complement checksum verifies
} icmp_parsed_t;

// icmp_parse return codes.
#define ICMP_PARSE_OK          0    // structurally valid (check checksum_ok too)
#define ICMP_PARSE_ETOOSHORT  -1    // len < sizeof(icmp_header_t)
#define ICMP_PARSE_EOVERSIZE  -2    // len > ICMP_MAX_LEN (Rust-confined; C ref has no upper bound)

// Upper bound on a live ICMP length: Ethernet MTU.
//
// CORRECTION (2026-07-16, #404 drift audit 2 + task #5XX). This comment used to
// say "IP delivers ICMP payloads bounded by total_length <= frame len <= IP_MTU
// (1500), so a real echo can never exceed this", and the ledger rated this bound
// DEFENSE-IN-DEPTH against a NOT-currently-reachable OOB. That reasoning was
// FALSE and the rating was wrong.
//
// ip_handle guarded `ihl <= length` and `total_length <= length` but never
// related the two, so `total_length - ihl` UNDERFLOWED: a crafted 60-byte frame
// with IHL=15 and total_length=20 delivered len=65496 to this parser. MEASURED,
// not argued: driving the real verbatim ip_handle hands icmp_handle, udp_handle
// AND tcp_handle len=65496 from a 60-byte frame. So ICMP_MAX_LEN was confining a
// REACHABLE, LAN-triggerable OOB (the unbounded checksum span + the downstream
// uint8_t reply[length] ~64 KB VLA on a 16 KB kernel stack), and a rollback of
// -DRUST_ICMP was NOT safe.
//
// The root cause is now fixed in plain C at the source (net/ip.c's
// `total_length < ihl` guard), so this bound is genuinely defense-in-depth
// again. It stays: it is cheap, and it is the second layer that made the
// difference here.
#define ICMP_MAX_LEN        1500

// Pure parse/validate dispatcher (routes to _rs under -DRUST_ICMP, else _c).
int icmp_parse(const uint8_t *buf, uint32_t len, icmp_parsed_t *out);
// Verbatim C reference (current logic, INCLUDING the missing upper bound and the
// in-place checksum-field mutation) kept for the differential + rollback.
int icmp_parse_c(const uint8_t *buf, uint32_t len, icmp_parsed_t *out);
// Rust port (rustkern.rs): immutable slice over exactly len bytes, every access
// bounds-checked, rejects truncated AND oversize, never mutates the buffer.
extern int icmp_parse_rs(const uint8_t *buf, uint32_t len, icmp_parsed_t *out);

// Boot-time differential + perf + security self-test (logs [RUST-DIFF] icmp,
// [RUST-PERF] icmp, [RUST-SEC] icmp). Runs regardless of -DRUST_ICMP.
void icmp_rust_selftest(void);

// Initialize ICMP
void icmp_init(void);

// Send ping (echo request)
int icmp_ping(uint32_t dest_ip);

// Handle incoming ICMP packet
void icmp_handle(uint32_t src_ip, const void *data, uint16_t length);

// Get ping statistics
int icmp_get_ping_reply(uint32_t *src_ip, uint16_t *seq, uint16_t *time_ms);

#endif // ICMP_H
