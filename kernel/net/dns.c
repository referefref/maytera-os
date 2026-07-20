// dns.c - DNS resolver for MayteraOS
//
// UDP DNS client with A-record queries and a modern-style cache:
//   - Honors each record's own TTL (clamped to [DNS_MIN_TTL, DNS_MAX_TTL])
//     instead of a single fixed cache lifetime.
//   - Negative caching (RFC 2308 style): failed lookups (NXDOMAIN / SERVFAIL /
//     no-answer / timeout) are remembered for DNS_NEG_TTL seconds so a caller
//     stuck in a retry loop cannot hammer the upstream resolver (ban avoidance).
//   - Randomized 16-bit transaction IDs (anti-spoofing) and response validation
//     (QR bit + transaction id).
//   - Exponential backoff with jitter between retransmits, and transient
//     send-failure retries (so a not-yet-resolved gateway ARP doesn't fail the
//     whole lookup, which was the cause of intermittent CDN resolution misses).

#include "dns.h"
#include "udp.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../serial.h"
#include "../gui/syslog.h"

// External declarations
extern void net_poll(void);
extern int net_is_up(void);   // #374 network-up gate
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;   // real PIT frequency (250Hz), NOT the legacy 18.2Hz

// #333: DNS_HZ was hardcoded to 18 (legacy 18.2Hz PIT assumption) but the timer
// actually runs at g_timer_hz (250Hz). That made every ms->ticks conversion ~14x
// too small: dns_wait's DNS_TIMEOUT_MS=1500ms became ~112ms, so DNS gave up
// almost immediately under load and negative-cached the host (spurious "failed
// to resolve"); cache TTLs were likewise ~14x too short (excess re-resolves).
// Use the real timer frequency everywhere instead.
static inline uint64_t dns_hz(void) { return g_timer_hz ? g_timer_hz : 250; }
// Yield to the scheduler during DNS waits so the resolving process does not
// busy-spin and starve the rest of the system (e.g. the compositor) for the
// whole DNS timeout. Same pattern #180 applied to https.c / wget.c / dosexec.c.
extern void proc_sleep(uint32_t ms);
// Link state: skip DNS entirely when the NIC has no carrier so a dead link
// does not incur repeated multi-second timeouts (UI freeze on link-down VMs).
extern int nic_link_up(void);

// DNS local port for queries. NOTE: source-port randomization (a further
// anti-spoofing measure) would require the UDP layer to support per-query
// ephemeral binds; we randomize the transaction id instead.
#define DNS_LOCAL_PORT 10053

// DNS header structure
typedef struct {
    uint16_t id;            // Transaction ID
    uint16_t flags;         // Flags
    uint16_t qdcount;       // Question count
    uint16_t ancount;       // Answer count
    uint16_t nscount;       // Authority count
    uint16_t arcount;       // Additional count
} __attribute__((packed)) dns_header_t;

// DNS cache entry
typedef struct {
    char hostname[128];
    uint32_t ip;            // IP in host byte order (0 for negative entries)
    uint32_t ttl;           // honored TTL in seconds (diagnostics)
    uint64_t expiry;        // Expiry time in timer ticks
    int valid;
    int negative;           // 1 = cached failure (do not re-query until expiry)
} dns_cache_entry_t;

// DNS state
static uint32_t dns_server = 0;     // DNS server IP (host byte order)
static dns_cache_entry_t dns_cache[DNS_CACHE_SIZE];

// Cache statistics
static uint32_t stat_hits = 0, stat_misses = 0, stat_neg_hits = 0;

// xorshift PRNG for transaction ids and backoff jitter (seeded from the timer).
static uint32_t dns_rng = 0;
static uint32_t dns_rand(void) {
    uint32_t x = dns_rng;
    if (x == 0) x = (uint32_t)(timer_ticks ? timer_ticks : 0x2545F491) | 1u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    dns_rng = x;
    return x;
}

// Current query state for async handling
static struct {
    uint16_t pending_id;
    uint32_t result_ip;     // IP in host byte order
    int result_code;
    int complete;
    char hostname[128];
} dns_query;

// Byte order helpers
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
static inline uint32_t ntohl(uint32_t n) {
    return htonl(n);
}

// Look up hostname in cache (returns positive OR negative entries, unexpired).
static dns_cache_entry_t *dns_cache_lookup(const char *hostname) {
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid &&
            timer_ticks < dns_cache[i].expiry &&
            strcmp(dns_cache[i].hostname, hostname) == 0) {
            return &dns_cache[i];
        }
    }
    return NULL;
}

// Insert/refresh a cache entry. ttl_sec is the record's TTL (positive) or
// DNS_NEG_TTL (negative). Positive TTLs are clamped to [MIN,MAX].
static void dns_cache_put(const char *hostname, uint32_t ip,
                          uint32_t ttl_sec, int negative) {
    if (!negative) {
        if (ttl_sec < DNS_MIN_TTL) ttl_sec = DNS_MIN_TTL;
        if (ttl_sec > DNS_MAX_TTL) ttl_sec = DNS_MAX_TTL;
    } else {
        // #333: honor a short "soft" negative TTL for TRANSIENT failures (a query
        // timeout or a momentarily-down link), so a caller's retry is not blocked
        // for the full 30s DNS_NEG_TTL. Hard failures (NXDOMAIN/SERVFAIL/no-A)
        // still pass DNS_NEG_TTL. A zero/oversized request clamps to DNS_NEG_TTL.
        if (ttl_sec == 0 || ttl_sec > DNS_NEG_TTL) ttl_sec = DNS_NEG_TTL;
    }

    // Prefer: existing entry for this host (refresh) > empty slot > expired
    // slot > soonest-to-expire slot.
    int slot = -1;
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid && strcmp(dns_cache[i].hostname, hostname) == 0) {
            slot = i; break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < DNS_CACHE_SIZE; i++) {
            if (!dns_cache[i].valid || timer_ticks >= dns_cache[i].expiry) {
                slot = i; break;
            }
        }
    }
    if (slot < 0) {
        uint64_t oldest = dns_cache[0].expiry; slot = 0;
        for (int i = 1; i < DNS_CACHE_SIZE; i++) {
            if (dns_cache[i].expiry < oldest) { oldest = dns_cache[i].expiry; slot = i; }
        }
    }

    strncpy(dns_cache[slot].hostname, hostname, sizeof(dns_cache[slot].hostname) - 1);
    dns_cache[slot].hostname[sizeof(dns_cache[slot].hostname) - 1] = '\0';
    dns_cache[slot].ip = ip;
    dns_cache[slot].ttl = ttl_sec;
    dns_cache[slot].expiry = timer_ticks + (uint64_t)ttl_sec * dns_hz();
    dns_cache[slot].valid = 1;
    dns_cache[slot].negative = negative;
}

// Encode hostname as DNS name format (length-prefixed labels)
static int dns_encode_name(const char *hostname, uint8_t *buf, int max_len) {
    int pos = 0;
    while (*hostname && pos < max_len - 2) {
        const char *dot = hostname;
        while (*dot && *dot != '.') dot++;
        int label_len = dot - hostname;
        if (label_len > 63 || label_len == 0) return -1;
        if (pos + label_len + 1 >= max_len) return -1;
        buf[pos++] = (uint8_t)label_len;
        while (hostname < dot) buf[pos++] = *hostname++;
        if (*hostname == '.') hostname++;
    }
    buf[pos++] = 0;
    return pos;
}

// Skip a DNS name in response (handles compression pointers)
static int dns_skip_name(const uint8_t *buf, int pos, int max_len) {
    while (pos < max_len) {
        uint8_t len = buf[pos];
        if (len == 0) return pos + 1;
        if ((len & 0xC0) == 0xC0) return pos + 2;
        pos += len + 1;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// #404 / #496 Phase N (Tier 2, untrusted wire input): pure DNS response parse.
//
// dns_parse_response_c() is the PURE, verbatim extraction of the response-message
// walk that used to live inline in dns_handle_response(): validate the QR bit +
// rcode, read ancount, skip the question name (dns_skip_name, which handles a
// compression pointer by TERMINATING the name - it does NOT follow it), then walk
// the answer records looking for the first A record and honoring its TTL. It reads
// ONLY the msg[0..msglen) datagram, never mutates it, and touches no global / no
// I/O. The transaction-id / QR-source match, cache-put, kprintf, and dns_query
// bookkeeping stay in dns_handle_response (C). Result is returned through a small
// dns_result_t so the caller applies the same side effects it always did.
//
// This is the untrusted-input surface: msg/msglen come off a spoofable UDP
// datagram. The Rust port (dns_parse_response_rs, rustkern.rs) confines every
// label/pointer/record-field access to a bounds-checked slice over exactly msglen
// bytes. SECURITY (honest): the C reference here is ALREADY memory-safe on the
// wire - dns_skip_name is bounded (pos strictly increases by >=1 each iteration
// and a compression pointer returns immediately, so it can never loop or read
// past msglen), and every record-field read is length-gated (pos+4 / pos+10 /
// pos+rdlength <= length) before the access. So there is NO reachable OOB or hang,
// even on pointer LOOPS, OOB pointers, oversized rdlength, or a label running past
// the end - C and Rust reject/accept all of them identically. The genuine value
// (stronger than arp): the C is safe ONLY because it never FOLLOWS a compression
// pointer, i.e. it never actually decodes a name. The instant this parser is
// extended to decode names (the natural next feature: CNAME target extraction,
// PTR/reverse lookups, logging the answer owner name) the classic DNS pointer-loop
// / pointer-OOB class becomes reachable; the Rust form confines it BY CONSTRUCTION
// (slice bounds-checks + a strictly-decreasing visited budget), the raw-pointer C
// form would not. Defense-in-depth against a class that is one feature away from
// reachable, plus removal of the unchecked-wire-pointer-arithmetic class today.
//
// Static-assert the FFI struct layout so the #[repr(C)] DnsResult in rustkern.rs
// can never silently drift from dns_result_t.
_Static_assert(sizeof(dns_result_t) == 16, "dns_result_t must be 16 bytes for the Rust FFI");

int dns_parse_response_c(const uint8_t *msg, uint32_t msglen, dns_result_t *out) {
    if (out) { out->status = DNS_PARSE_FORMAT_ERR; out->rcode = 0; out->ip = 0; out->ttl = 0; }

    // Header gate (the old dns_handle_response `if (length < sizeof(dns_header_t))`
    // guard, hoisted into the seam so it is self-contained and short-msg-safe).
    if (msglen < sizeof(dns_header_t)) {
        if (out) out->status = DNS_PARSE_FORMAT_ERR;
        return DNS_PARSE_FORMAT_ERR;
    }

    const uint8_t *buf = msg;
    int length = (int)msglen;

    // Flags (buf[2..3], big-endian == ntohs(hdr->flags)).
    uint16_t flags = (uint16_t)((buf[2] << 8) | buf[3]);
    if (!(flags & 0x8000)) {                         // not a response
        if (out) out->status = DNS_PARSE_NOT_RESPONSE;
        return DNS_PARSE_NOT_RESPONSE;
    }

    int rcode = flags & 0x0F;
    if (rcode != DNS_RCODE_OK) {
        if (out) { out->status = DNS_PARSE_RCODE_ERR; out->rcode = rcode; }
        return DNS_PARSE_RCODE_ERR;
    }

    uint16_t ancount = (uint16_t)((buf[6] << 8) | buf[7]);  // ntohs(hdr->ancount)
    if (ancount == 0) {
        if (out) out->status = DNS_PARSE_NO_ANSWER;
        return DNS_PARSE_NO_ANSWER;
    }

    int pos = sizeof(dns_header_t);
    pos = dns_skip_name(buf, pos, length);   // skip question QNAME
    if (pos < 0 || pos + 4 > length) {
        if (out) out->status = DNS_PARSE_FORMAT_ERR;
        return DNS_PARSE_FORMAT_ERR;
    }
    pos += 4;  // QTYPE + QCLASS

    // Walk answers for the first A record; honor its TTL.
    for (int i = 0; i < ancount && pos < length; i++) {
        pos = dns_skip_name(buf, pos, length);
        if (pos < 0 || pos + 10 > length) break;

        uint16_t type = (buf[pos] << 8) | buf[pos + 1];
        uint32_t ttl = ((uint32_t)buf[pos + 4] << 24) | ((uint32_t)buf[pos + 5] << 16) |
                       ((uint32_t)buf[pos + 6] << 8)  |  (uint32_t)buf[pos + 7];
        uint16_t rdlength = (buf[pos + 8] << 8) | buf[pos + 9];
        pos += 10;

        if (pos + rdlength > length) break;

        if (type == DNS_TYPE_A && rdlength == 4) {
            uint32_t ip = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos + 1] << 16) |
                          ((uint32_t)buf[pos + 2] << 8) | (uint32_t)buf[pos + 3];
            if (out) { out->status = DNS_PARSE_A_FOUND; out->ip = ip; out->ttl = ttl; }
            return DNS_PARSE_A_FOUND;
        }
        pos += rdlength;
    }

    if (out) out->status = DNS_PARSE_NO_A;
    return DNS_PARSE_NO_A;
}

// Live dispatcher. With -DRUST_DNS (set in the Makefile) the incoming DNS
// response parse runs in Rust; drop the flag + rebuild to roll straight back to C.
int dns_parse_response(const uint8_t *msg, uint32_t msglen, dns_result_t *out) {
#ifdef RUST_DNS
    return dns_parse_response_rs(msg, msglen, out);
#else
    return dns_parse_response_c(msg, msglen, out);
#endif
}

// Handle DNS response (UDP callback). The untrusted message PARSE is now
// dns_parse_response() (Rust under -DRUST_DNS); the transaction-id / QR-source
// match, cache-put, and dns_query bookkeeping stay here in C.
static void dns_handle_response(uint32_t src_ip, uint16_t src_port,
                                 const void *data, uint16_t length) {
    (void)src_ip;
    (void)src_port;

    if (length < sizeof(dns_header_t)) return;

    const uint8_t *buf = (const uint8_t *)data;

    // Validate matching transaction id (anti-spoofing). Stays in C: needs the
    // dns_query global. buf[0..1] big-endian == ntohs(hdr->id).
    uint16_t id = (uint16_t)((buf[0] << 8) | buf[1]);
    if (id != dns_query.pending_id) return;

    dns_result_t r;
    dns_parse_response(buf, length, &r);

    switch (r.status) {
        case DNS_PARSE_NOT_RESPONSE:
            return;  // not a response (QR clear): drop, as the old code did

        case DNS_PARSE_RCODE_ERR:
            kprintf("[DNS] Error: RCODE=%d\n", r.rcode);
            dns_cache_put(dns_query.hostname, 0, DNS_NEG_TTL, 1);   // negative cache
            dns_query.result_code = -r.rcode;
            dns_query.complete = 1;
            return;

        case DNS_PARSE_NO_ANSWER:
            dns_cache_put(dns_query.hostname, 0, DNS_NEG_TTL, 1);
            dns_query.result_code = -DNS_RCODE_NAME_ERROR;
            dns_query.complete = 1;
            return;

        case DNS_PARSE_FORMAT_ERR:
            dns_query.result_code = -DNS_RCODE_FORMAT_ERROR;
            dns_query.complete = 1;
            return;

        case DNS_PARSE_A_FOUND:
            dns_query.result_ip = r.ip;
            dns_query.result_code = 0;
            dns_query.complete = 1;
            dns_cache_put(dns_query.hostname, r.ip, r.ttl, 0);   // TTL-honoring cache
            kprintf("[DNS] Resolved %s -> %d.%d.%d.%d (ttl=%us)\n",
                    dns_query.hostname, (r.ip >> 24) & 0xFF, (r.ip >> 16) & 0xFF,
                    (r.ip >> 8) & 0xFF, r.ip & 0xFF, r.ttl);
            return;

        case DNS_PARSE_NO_A:
        default:
            // Answer(s) present but no A record - treat as a (short) negative result.
            dns_cache_put(dns_query.hostname, 0, DNS_NEG_TTL, 1);
            dns_query.result_code = -DNS_RCODE_NAME_ERROR;
            dns_query.complete = 1;
            return;
    }
}

// Check if string is a dotted decimal IP address
static int is_ip_address(const char *str) {
    int dots = 0, digits = 0;
    for (const char *p = str; *p; p++) {
        if (*p == '.') {
            if (digits == 0) return 0;
            dots++; digits = 0;
        } else if (*p >= '0' && *p <= '9') {
            digits++; if (digits > 3) return 0;
        } else return 0;
    }
    return dots == 3 && digits > 0;
}

// Parse dotted decimal IP to host byte order
static uint32_t parse_ip(const char *str) {
    uint8_t octets[4] = {0};
    int idx = 0, val = 0;
    for (const char *p = str; idx < 4; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            if (val > 255) return 0;
        } else if (*p == '.' || *p == '\0') {
            octets[idx++] = (uint8_t)val; val = 0;
            if (*p == '\0') break;
        } else return 0;
    }
    if (idx != 4) return 0;
    return ((uint32_t)octets[0] << 24) | ((uint32_t)octets[1] << 16) |
           ((uint32_t)octets[2] << 8) | (uint32_t)octets[3];
}

// Poll the network for up to ms milliseconds, returning early if the pending
// query completed. Used both for response waits and inter-retry backoff.
static int dns_wait(uint32_t ms) {
    uint64_t until = timer_ticks + ((uint64_t)ms * dns_hz()) / 1000 + 1;
    while (timer_ticks < until) {
        net_poll();
        if (dns_query.complete) return 1;
        // Yield ~a couple ms to the scheduler each iteration instead of a
        // busy pause-spin. net_poll() still runs every loop, so responses are
        // still pumped, but the compositor and other procs get to run too.
        proc_sleep(2);
    }
    return dns_query.complete;
}

// Build an A-record query for hostname into query[512]; sets dns_query state and
// a fresh random transaction id. Returns the packet length or negative on error.
static int dns_build_query(const char *hostname, uint8_t *query) {
    memset(query, 0, 512);
    dns_header_t *hdr = (dns_header_t *)query;
    dns_query.pending_id = (uint16_t)(dns_rand() & 0xFFFF);
    hdr->id = htons(dns_query.pending_id);
    hdr->flags = htons(0x0100);   // RD (recursion desired)
    hdr->qdcount = htons(1);

    int qpos = sizeof(dns_header_t);
    int namelen = dns_encode_name(hostname, query + qpos, 512 - qpos - 4);
    if (namelen < 0) return -DNS_RCODE_FORMAT_ERROR;
    qpos += namelen;
    query[qpos++] = 0; query[qpos++] = DNS_TYPE_A;   // QTYPE = A
    query[qpos++] = 0; query[qpos++] = 1;            // QCLASS = IN

    strncpy(dns_query.hostname, hostname, sizeof(dns_query.hostname) - 1);
    dns_query.hostname[sizeof(dns_query.hostname) - 1] = '\0';
    dns_query.result_ip = 0;
    dns_query.result_code = -1;
    dns_query.complete = 0;
    return qpos;
}

// Send the query, tolerating transient failures (e.g. gateway ARP not resolved
// yet) by polling the stack and retrying briefly. Returns 0 on success.
static int dns_send(const uint8_t *query, int qlen) {
    for (int i = 0; i < DNS_SEND_RETRIES; i++) {
        if (udp_send(dns_server, DNS_LOCAL_PORT, DNS_PORT, query, qlen) >= 0)
            return 0;
        // Likely waiting on ARP; pump the stack and give it a moment.
        dns_wait(120);
    }
    return -1;
}

// Initialize DNS subsystem
void dns_init(void) {
    memset(dns_cache, 0, sizeof(dns_cache));
    memset(&dns_query, 0, sizeof(dns_query));
    dns_rng = (uint32_t)(timer_ticks ? timer_ticks : 0x2545F491) | 1u;
    udp_bind(DNS_LOCAL_PORT, dns_handle_response);
    dns_server = parse_ip("8.8.8.8");   // default; overridden by DHCP if wired
    kprintf("[DNS] resolver initialized (server 8.8.8.8, TTL-aware cache, neg-cache %ds)\n",
            DNS_NEG_TTL);
}

void dns_set_server(uint32_t server_ip) {
    dns_server = server_ip;
    uint8_t *ip = (uint8_t *)&server_ip;
    kprintf("[DNS] server set to %d.%d.%d.%d\n", ip[3], ip[2], ip[1], ip[0]);
}

uint32_t dns_get_server(void) { return dns_server; }

void dns_cache_clear(void) {
    for (int i = 0; i < DNS_CACHE_SIZE; i++) dns_cache[i].valid = 0;
    kprintf("[DNS] cache cleared\n");
}

void dns_cache_stats(uint32_t *entries, uint32_t *hits, uint32_t *misses,
                     uint32_t *neg_hits) {
    uint32_t n = 0;
    for (int i = 0; i < DNS_CACHE_SIZE; i++)
        if (dns_cache[i].valid && timer_ticks < dns_cache[i].expiry) n++;
    if (entries)  *entries  = n;
    if (hits)     *hits     = stat_hits;
    if (misses)   *misses   = stat_misses;
    if (neg_hits) *neg_hits = stat_neg_hits;
}

// Resolve hostname to IPv4 address (host byte order). Blocking.
int dns_resolve(const char *hostname, uint32_t *ip_out) {
    if (!hostname || !ip_out) return -1;

    if (is_ip_address(hostname)) {
        *ip_out = parse_ip(hostname);
        return *ip_out ? 0 : -DNS_RCODE_FORMAT_ERROR;
    }

    // Cache (positive + negative).
    dns_cache_entry_t *cached = dns_cache_lookup(hostname);
    if (cached) {
        if (cached->negative) {
            stat_neg_hits++;
            return -DNS_RCODE_NAME_ERROR;   // recent failure; don't re-query
        }
        *ip_out = cached->ip;
        stat_hits++;
        uint8_t *ip = (uint8_t *)&cached->ip;
        kprintf("[DNS] cache hit %s -> %d.%d.%d.%d\n",
                hostname, ip[3], ip[2], ip[1], ip[0]);
        return 0;
    }
    stat_misses++;

    // Fail fast on a dead link: with no carrier the query would just time out
    // after several seconds (DNS_TIMEOUT_MS * DNS_MAX_RETRIES), and on a
    // link-down VM that repeats for every lookup. Negative-cache and return
    // immediately so callers back off without ever entering the wait.
    if (!net_is_up()) {
        dns_cache_put(hostname, 0, DNS_NEG_TTL_SOFT, 1);   // #333/#374 transient: link/IP may return
        kprintf("[DNS] network down; skipping resolve of %s (soft-negative-cached %ds)\n",
                hostname, DNS_NEG_TTL_SOFT);
        return -1;
    }

    if (dns_server == 0) {
        kprintf("[DNS] no server configured\n");
        return -1;
    }

    uint8_t query[512];
    int qlen = dns_build_query(hostname, query);
    if (qlen < 0) return qlen;

    kprintf("[DNS] resolving %s\n", hostname);

    uint32_t attempt_ms = DNS_TIMEOUT_MS;
    for (int attempt = 0; attempt < DNS_MAX_RETRIES; attempt++) {
        if (dns_send(query, qlen) < 0) {
            kprintf("[DNS] send failed (attempt %d)\n", attempt + 1);
            // fall through to backoff and try again
        } else {
            if (dns_wait(attempt_ms) && dns_query.complete) {
                if (dns_query.result_code == 0) *ip_out = dns_query.result_ip;
                return dns_query.result_code;
            }
        }
        // Exponential backoff with jitter before retransmitting (ban-safe).
        uint32_t backoff = (DNS_BACKOFF_BASE_MS << attempt);
        uint32_t jitter  = dns_rand() % (DNS_BACKOFF_BASE_MS + 1);
        if (dns_wait(backoff + jitter) && dns_query.complete) {
            if (dns_query.result_code == 0) *ip_out = dns_query.result_ip;
            return dns_query.result_code;
        }
        attempt_ms <<= 1;   // double the response wait each retry
    }

    // Exhausted retries: this is usually a TRANSIENT loss (packet drop / load),
    // not a real NXDOMAIN, so use a SHORT soft negative TTL. Callers in a retry
    // loop still back off briefly but can re-query within a few seconds (#333).
    dns_cache_put(hostname, 0, DNS_NEG_TTL_SOFT, 1);
    kprintf("[DNS] failed to resolve %s (soft-negative-cached %ds)\n", hostname, DNS_NEG_TTL_SOFT);
    return -1;
}

// Non-blocking DNS for userland syscalls (paired with dns_resolve_check).
// 1 = resolved immediately, 0 = query sent, <0 = error (incl. negative cache).
int dns_resolve_start(const char *hostname, uint32_t *ip_out) {
    if (!hostname || !ip_out) return -1;
    if (is_ip_address(hostname)) {
        *ip_out = parse_ip(hostname);
        return *ip_out ? 1 : -1;
    }
    dns_cache_entry_t *cached = dns_cache_lookup(hostname);
    if (cached) {
        if (cached->negative) { stat_neg_hits++; return -1; }
        *ip_out = cached->ip; stat_hits++; return 1;
    }
    stat_misses++;
    // Fail fast on dead link (see dns_resolve): negative-cache + bail so the
    // userland caller does not retry into multi-second send/ARP waits.
    if (!nic_link_up()) {
        dns_cache_put(hostname, 0, DNS_NEG_TTL, 1);
        return -1;
    }
    if (dns_server == 0) return -1;

    uint8_t query[512];
    int qlen = dns_build_query(hostname, query);
    if (qlen < 0) return qlen;
    if (dns_send(query, qlen) < 0) return -1;
    return 0;
}

// Poll a pending dns_resolve_start. 1 = success, 0 = pending, -1 = failed.
int dns_resolve_check(uint32_t *ip_out) {
    if (!dns_query.complete) return 0;
    if (dns_query.result_code == 0) {
        if (ip_out) *ip_out = dns_query.result_ip;
        return 1;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// #404 / #496 Phase N boot-time self-test: prove dns_parse_response_rs (Rust,
// live under -DRUST_DNS) == dns_parse_response_c (verbatim reference) on the
// live agreement domain BEFORE any real DNS response is handled, report the
// SECURITY posture HONESTLY, and micro-benchmark both. LIGHT (#426, bounded,
// runs once): ~512 differential vectors (well-formed A / CNAME+A / AAAA-only /
// ancount=0 / rcode-error / not-a-response, plus MALFORMED that must TERMINATE:
// truncated header, truncated mid-answer, compression-pointer LOOP, OOB pointer,
// oversized rdlength, label-length-past-end, random) + a ~5k-iter RDTSC bench.
// The heavy fuzz (hundreds of thousands of vectors incl. every malformed class)
// runs as the OFFLINE pre-flight, not at boot. One [RUST-DIFF] dns, one
// [RUST-SEC] dns, one [RUST-PERF] dns line to serial + /BOOTLOG.
//
// NOTE on pointer LOOPS: the C dns_skip_name does NOT follow a compression
// pointer (it returns immediately at the first byte >= 0xC0), so a self-
// referential or 2-cycle pointer does NOT hang it - it terminates and both C
// and Rust agree. It is therefore SAFE to include loop vectors in the boot test
// (they can never wedge boot). The Rust additionally carries a strictly-
// decreasing visited budget so a FUTURE pointer-following extension still could
// not loop.

static uint32_t dnsdiff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

static inline uint64_t dns_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

// ---- DNS response message builders (all write big-endian wire order) --------
static int dns_wr_header(uint8_t *b, uint16_t id, uint16_t flags,
                         uint16_t qd, uint16_t an) {
    b[0] = id >> 8;    b[1] = id & 0xFF;
    b[2] = flags >> 8; b[3] = flags & 0xFF;
    b[4] = qd >> 8;    b[5] = qd & 0xFF;
    b[6] = an >> 8;    b[7] = an & 0xFF;
    b[8] = 0; b[9] = 0; b[10] = 0; b[11] = 0;   // nscount, arcount
    return 12;
}
// question: QNAME "test.com" + QTYPE A + QCLASS IN (name starts at offset 12).
static int dns_wr_question(uint8_t *b, int pos) {
    b[pos++] = 4; b[pos++] = 't'; b[pos++] = 'e'; b[pos++] = 's'; b[pos++] = 't';
    b[pos++] = 3; b[pos++] = 'c'; b[pos++] = 'o'; b[pos++] = 'm';
    b[pos++] = 0;
    b[pos++] = 0; b[pos++] = 1;   // QTYPE  = A
    b[pos++] = 0; b[pos++] = 1;   // QCLASS = IN
    return pos;
}
// answer whose NAME is a compression pointer to the question name (0xC0 0x0C),
// with the given type/ttl/rdlength/rdata.
static int dns_wr_answer_ptr(uint8_t *b, int pos, uint16_t type, uint32_t ttl,
                             uint16_t rdlen, const uint8_t *rdata) {
    b[pos++] = 0xC0; b[pos++] = 0x0C;                 // name -> offset 12 (question)
    b[pos++] = type >> 8;  b[pos++] = type & 0xFF;
    b[pos++] = 0; b[pos++] = 1;                        // CLASS = IN
    b[pos++] = ttl >> 24; b[pos++] = (ttl >> 16) & 0xFF;
    b[pos++] = (ttl >> 8) & 0xFF; b[pos++] = ttl & 0xFF;
    b[pos++] = rdlen >> 8; b[pos++] = rdlen & 0xFF;
    for (int i = 0; i < rdlen; i++) b[pos++] = rdata[i];
    return pos;
}

static int dns_result_eq(const dns_result_t *a, const dns_result_t *b) {
    if (a->status != b->status) return 1;
    if (a->status == DNS_PARSE_A_FOUND) {
        if (a->ip != b->ip)   return 1;
        if (a->ttl != b->ttl) return 1;
    }
    if (a->status == DNS_PARSE_RCODE_ERR) {
        if (a->rcode != b->rcode) return 1;
    }
    return 0;
}

// Build one differential vector of the given kind into buf; return total length.
static int dns_build_vector(uint8_t *buf, uint32_t kind, uint32_t *seed) {
    uint16_t id  = (uint16_t)(dnsdiff_rng(seed) & 0xFFFF);
    uint32_t ttl = dnsdiff_rng(seed) % 100000;
    uint8_t a4[4]; for (int i = 0; i < 4; i++) a4[i] = (uint8_t)(dnsdiff_rng(seed) & 0xFF);
    uint8_t a16[16]; for (int i = 0; i < 16; i++) a16[i] = (uint8_t)(dnsdiff_rng(seed) & 0xFF);
    int pos;

    switch (kind) {
    case 0: // well-formed single A record -> A_FOUND
        pos = dns_wr_header(buf, id, 0x8180, 1, 1);
        pos = dns_wr_question(buf, pos);
        pos = dns_wr_answer_ptr(buf, pos, DNS_TYPE_A, ttl, 4, a4);
        return pos;
    case 1: { // CNAME then A (two answers) -> A_FOUND (second)
        uint8_t cname[3] = { 0xC0, 0x0C, 0 }; // trivial rdata (not parsed)
        pos = dns_wr_header(buf, id, 0x8180, 1, 2);
        pos = dns_wr_question(buf, pos);
        pos = dns_wr_answer_ptr(buf, pos, DNS_TYPE_CNAME, ttl, 2, cname);
        pos = dns_wr_answer_ptr(buf, pos, DNS_TYPE_A, ttl, 4, a4);
        return pos;
    }
    case 2: // AAAA only -> NO_A
        pos = dns_wr_header(buf, id, 0x8180, 1, 1);
        pos = dns_wr_question(buf, pos);
        pos = dns_wr_answer_ptr(buf, pos, DNS_TYPE_AAAA, ttl, 16, a16);
        return pos;
    case 3: // ancount == 0 -> NO_ANSWER
        pos = dns_wr_header(buf, id, 0x8180, 1, 0);
        pos = dns_wr_question(buf, pos);
        return pos;
    case 4: // rcode = NXDOMAIN (3) -> RCODE_ERR
        pos = dns_wr_header(buf, id, 0x8183, 1, 0);
        pos = dns_wr_question(buf, pos);
        return pos;
    case 5: // QR bit clear (a query, not a response) -> NOT_RESPONSE
        pos = dns_wr_header(buf, id, 0x0100, 1, 1);
        pos = dns_wr_question(buf, pos);
        pos = dns_wr_answer_ptr(buf, pos, DNS_TYPE_A, ttl, 4, a4);
        return pos;
    case 6: { // short header (len 0..11) -> FORMAT_ERR
        uint32_t len = dnsdiff_rng(seed) % 12;
        for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(dnsdiff_rng(seed) & 0xFF);
        return (int)len;
    }
    case 7: { // valid A record then TRUNCATE mid-answer
        pos = dns_wr_header(buf, id, 0x8180, 1, 1);
        pos = dns_wr_question(buf, pos);
        pos = dns_wr_answer_ptr(buf, pos, DNS_TYPE_A, ttl, 4, a4);
        uint32_t cut = 13 + (dnsdiff_rng(seed) % (uint32_t)(pos - 13)); // keep header+question
        return (int)cut;
    }
    case 8: { // compression-pointer LOOP: answer name points to itself
        pos = dns_wr_header(buf, id, 0x8180, 1, 1);
        pos = dns_wr_question(buf, pos);
        int name_off = pos;
        buf[pos++] = 0xC0; buf[pos++] = (uint8_t)(name_off & 0xFF); // self-pointer
        buf[pos++] = 0; buf[pos++] = 1;   // type A
        buf[pos++] = 0; buf[pos++] = 1;   // class IN
        buf[pos++] = ttl >> 24; buf[pos++] = (ttl >> 16) & 0xFF;
        buf[pos++] = (ttl >> 8) & 0xFF; buf[pos++] = ttl & 0xFF;
        buf[pos++] = 0; buf[pos++] = 4;   // rdlength 4
        for (int i = 0; i < 4; i++) buf[pos++] = a4[i];
        return pos;
    }
    case 9: { // OOB / forward compression pointer (0xC0 0xFF -> offset 255)
        pos = dns_wr_header(buf, id, 0x8180, 1, 1);
        pos = dns_wr_question(buf, pos);
        buf[pos++] = 0xC0; buf[pos++] = 0xFF; // pointer to a far/OOB offset
        buf[pos++] = 0; buf[pos++] = 1; buf[pos++] = 0; buf[pos++] = 1;
        buf[pos++] = ttl >> 24; buf[pos++] = (ttl >> 16) & 0xFF;
        buf[pos++] = (ttl >> 8) & 0xFF; buf[pos++] = ttl & 0xFF;
        buf[pos++] = 0; buf[pos++] = 4;
        for (int i = 0; i < 4; i++) buf[pos++] = a4[i];
        return pos;
    }
    case 10: { // oversized rdlength (lies past end) -> break -> NO_A
        pos = dns_wr_header(buf, id, 0x8180, 1, 1);
        pos = dns_wr_question(buf, pos);
        buf[pos++] = 0xC0; buf[pos++] = 0x0C;
        buf[pos++] = 0; buf[pos++] = 1; buf[pos++] = 0; buf[pos++] = 1;
        buf[pos++] = ttl >> 24; buf[pos++] = (ttl >> 16) & 0xFF;
        buf[pos++] = (ttl >> 8) & 0xFF; buf[pos++] = ttl & 0xFF;
        buf[pos++] = 0xFF; buf[pos++] = 0xFF; // rdlength 65535
        buf[pos++] = a4[0]; buf[pos++] = a4[1]; // only 2 bytes of "rdata"
        return pos;
    }
    case 11: { // answer name label length runs past end -> skip fails -> NO_A
        pos = dns_wr_header(buf, id, 0x8180, 1, 1);
        pos = dns_wr_question(buf, pos);
        buf[pos++] = 0x3F;                    // label len 63, but only a few bytes follow
        buf[pos++] = 'x'; buf[pos++] = 'y';
        return pos;
    }
    default: { // fully random bytes, random length up to ~90
        uint32_t len = dnsdiff_rng(seed) % 90;
        for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(dnsdiff_rng(seed) & 0xFF);
        return (int)len;
    }
    }
}

void dns_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    static uint8_t buf[600];
    uint32_t seed = 0x1d0f5a3b;
    uint32_t vectors = 0, mismatches = 0;
    int first_bad = -1;

    // Force-reference the Rust symbol so its archive member is always linked
    // (matches the arp/icmp pattern), regardless of -DRUST_DNS.
    { dns_result_t t; dns_parse_response_rs(buf, 0, &t); }

    // Part 1: agreement + malformed domain (~512 vectors, 13 kinds).
    for (uint32_t iter = 0; iter < 512; iter++) {
        uint32_t kind = dnsdiff_rng(&seed) % 13;
        int len = dns_build_vector(buf, kind, &seed);
        if (len < 0) len = 0;
        dns_result_t co, ro;
        dns_parse_response_c(buf, (uint32_t)len, &co);
        dns_parse_response_rs(buf, (uint32_t)len, &ro);
        vectors++;
        if (dns_result_eq(&co, &ro)) {
            mismatches++;
            if (first_bad < 0) first_bad = (int)iter;
        }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] dns: %u vectors, %u mismatches -> %s\n", vectors, mismatches, verdict);
    bootlog_write("[RUST-DIFF] dns: %u vectors, %u mismatches -> %s", vectors, mismatches, verdict);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] dns FIRST MISMATCH iter=%d\n", first_bad);
        bootlog_write("[RUST-DIFF] dns FIRST MISMATCH iter=%d", first_bad);
    }

    // Part 2: SECURITY posture. Sweep the malformed / attack corpus (pointer
    // loops, OOB pointers, oversized rdlength, label-past-end, truncations, and
    // every short-header length 0..11) and count C-vs-Rust verdict DIVERGENCES.
    // Honest expectation: ZERO - the C is already bounded (dns_skip_name cannot
    // loop or read past msglen; every field read is length-gated), so both
    // reject/accept identically. This documents the port removes a CLASS (and
    // pre-confines the pointer-follow class one feature away from reachable), not
    // a live bug (divergence == 0 == no regression). Also asserts NO HANG.
    {
        uint32_t sec_n = 0, divergences = 0;
        uint32_t s2 = 0x77c1aa55;
        // every short-header length
        for (uint32_t len = 0; len < sizeof(dns_header_t); len++) {
            for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(dnsdiff_rng(&s2) & 0xFF);
            dns_result_t co, ro;
            int crc = dns_parse_response_c(buf, len, &co);
            int rrc = dns_parse_response_rs(buf, len, &ro);
            sec_n++;
            if (crc != rrc || dns_result_eq(&co, &ro)) divergences++;
        }
        // the dangerous malformed kinds (loops / OOB ptr / oversized rdlen /
        // label-past-end / truncation / random), many iterations each
        static const uint32_t evil[] = { 7, 8, 9, 10, 11, 12 };
        for (uint32_t r = 0; r < 800; r++) {
            uint32_t kind = evil[dnsdiff_rng(&s2) % 6];
            int len = dns_build_vector(buf, kind, &s2);
            if (len < 0) len = 0;
            dns_result_t co, ro;
            int crc = dns_parse_response_c(buf, (uint32_t)len, &co);
            int rrc = dns_parse_response_rs(buf, (uint32_t)len, &ro);
            sec_n++;
            if (crc != rrc || dns_result_eq(&co, &ro)) divergences++;
        }
        kprintf("[RUST-SEC] dns: C bounded (skip-name cannot loop/OOB, fields length-gated) so no reachable OOB; "
                "%u/%u malformed verdicts identical, %u divergences; Rust confines the class by construction "
                "(and pre-confines the pointer-FOLLOW loop/OOB class a name-decode feature would make reachable)\n",
                sec_n - divergences, sec_n, divergences);
        bootlog_write("[RUST-SEC] dns: no reachable OOB (C bounded); %u/%u malformed verdicts identical, %u divergences (class-elimination + pre-confines pointer-follow class, latent-not-reachable)",
                      sec_n - divergences, sec_n, divergences);
    }

    // Part 3: RDTSC micro-benchmark over a fixed well-formed single-A response.
    // LIGHT: 5k iters. The big counts are the offline pre-flight.
    {
        const int iters = 5000;
        dns_result_t o;
        uint32_t s3 = 0x9ab3cd12;
        int len = dns_build_vector(buf, 0, &s3);   // well-formed single A

        for (int i = 0; i < 300; i++) {
            dns_parse_response_c(buf, (uint32_t)len, &o);
            dns_parse_response_rs(buf, (uint32_t)len, &o);
        }

        uint64_t t0 = dns_tsc_serialized();
        for (int i = 0; i < iters; i++) dns_parse_response_c(buf, (uint32_t)len, &o);
        uint64_t t1 = dns_tsc_serialized();
        for (int i = 0; i < iters; i++) dns_parse_response_rs(buf, (uint32_t)len, &o);
        uint64_t t2 = dns_tsc_serialized();

        uint64_t c_cyc = (t1 - t0) / iters;
        uint64_t r_cyc = (t2 - t1) / iters;
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        kprintf("[RUST-PERF] dns: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
        bootlog_write("[RUST-PERF] dns: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}
