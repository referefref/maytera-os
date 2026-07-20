#ifndef DNS_H
#define DNS_H

#include "../types.h"

// DNS configuration
#define DNS_PORT 53
#define DNS_CACHE_SIZE 64

// Timer tick rate (PIT ~18.2 Hz). Used to convert seconds/ms to ticks.
#define DNS_HZ 18

// TTL handling (modern: honor the record's own TTL, but clamp it).
//  - Floor avoids re-querying upstream too often for records with tiny TTLs
//    (some CDNs advertise a few seconds); this is the main anti-ban lever.
//  - Cap avoids pinning a stale address forever.
#define DNS_MIN_TTL    60        // seconds
#define DNS_MAX_TTL    86400     // seconds (1 day)
#define DNS_NEG_TTL    30        // negative-cache TTL (RFC 2308 style)
#define DNS_NEG_TTL_SOFT 3        // #333 short neg-TTL for transient timeouts/link-down

// Retry/backoff. First attempt waits DNS_TIMEOUT_MS; each retry doubles the
// wait and adds jitter, so we never burst-retransmit (which gets you rate
// limited / banned by public resolvers).
#define DNS_TIMEOUT_MS      1500 // initial per-attempt response wait
#define DNS_MAX_RETRIES     3
#define DNS_BACKOFF_BASE_MS 200  // base spacing between attempts (+jitter)
#define DNS_SEND_RETRIES    4    // transient send (ARP-not-ready) retries

// DNS record types
#define DNS_TYPE_A 1
#define DNS_TYPE_AAAA 28
#define DNS_TYPE_CNAME 5

// DNS response codes
#define DNS_RCODE_OK 0
#define DNS_RCODE_FORMAT_ERROR 1
#define DNS_RCODE_SERVER_FAIL 2
#define DNS_RCODE_NAME_ERROR 3

// ---------------------------------------------------------------------------
// #404 / #496 Phase N (Tier 2, untrusted wire input): pure DNS response parse.
//
// dns_parse_response() is the PURE parse/validate of an incoming DNS response
// MESSAGE (name-skip over compression pointers + question-skip + answer-record
// walk for the first A record) extracted out of dns_handle_response(). It does
// NOT do network I/O, touch the cache, read/write any global, or mutate the
// input buffer. The UDP receive, transaction-id / QR match, cache-put, and the
// dns_query bookkeeping stay in C (dns_handle_response). This is the untrusted
// surface: msg/msglen come straight off a spoofable UDP datagram, so length
// validation and every label / pointer / record-field access must be bounds-
// checked. Routed to dns_parse_response_rs (rustkern.rs) under -DRUST_DNS.
//
// Parsed result of a DNS response. #[repr(C)] mirror lives in rustkern.rs
// (DnsResult); size asserted == 16 in dns.c so the FFI layout can never silently
// drift. ip/ttl are host order (valid only when status == DNS_PARSE_A_FOUND);
// rcode is the DNS response code (valid only when status == DNS_PARSE_RCODE_ERR).
typedef struct {
    int32_t  status;   // DNS_PARSE_* below
    int32_t  rcode;    // DNS rcode (meaningful only for DNS_PARSE_RCODE_ERR)
    uint32_t ip;       // resolved IPv4, host order (A_FOUND only)
    uint32_t ttl;      // record TTL in seconds (A_FOUND only)
} dns_result_t;

// dns_parse_response status codes (returned AND written to out->status).
#define DNS_PARSE_A_FOUND       0   // first A record found; ip + ttl valid
#define DNS_PARSE_NOT_RESPONSE  1   // QR bit clear (not a response) - drop
#define DNS_PARSE_RCODE_ERR     2   // header rcode != 0 (rcode field set)
#define DNS_PARSE_NO_ANSWER     3   // ancount == 0
#define DNS_PARSE_FORMAT_ERR    4   // malformed (short header / bad question name)
#define DNS_PARSE_NO_A          5   // answers present but no A record

// Pure parse/validate dispatcher (routes to _rs under -DRUST_DNS, else _c).
int dns_parse_response(const uint8_t *msg, uint32_t msglen, dns_result_t *out);
// Verbatim C reference (the old dns_handle_response walk, read-only) kept for
// the differential + rollback (undefine RUST_DNS -> dns_parse_response() -> _c).
int dns_parse_response_c(const uint8_t *msg, uint32_t msglen, dns_result_t *out);
// Rust port (rustkern.rs): immutable slice over exactly msglen bytes, every
// label / pointer / record-field access bounds-checked; rejects (never reads
// past msg) instead of OOB, and cannot loop on a crafted compression pointer.
extern int dns_parse_response_rs(const uint8_t *msg, uint32_t msglen, dns_result_t *out);

// Boot-time differential + perf + security self-test (logs [RUST-DIFF] dns,
// [RUST-PERF] dns, [RUST-SEC] dns). Runs regardless of -DRUST_DNS.
void dns_rust_selftest(void);

// Initialize DNS subsystem
void dns_init(void);

// Set DNS server IP (host byte order)
void dns_set_server(uint32_t server_ip);

// Resolve hostname to IPv4 address (host byte order)
// Returns 0 on success, negative on error
int dns_resolve(const char *hostname, uint32_t *ip_out);
int dns_resolve_start(const char *hostname, uint32_t *ip_out);
int dns_resolve_check(uint32_t *ip_out);

// Clear DNS cache
void dns_cache_clear(void);

// Get current DNS server
uint32_t dns_get_server(void);

// Cache statistics (for diagnostics / a future `dns` shell command).
void dns_cache_stats(uint32_t *entries, uint32_t *hits, uint32_t *misses,
                     uint32_t *neg_hits);

#endif
