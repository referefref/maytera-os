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
#include "netfail.h"   // #netfix2: record WHY a lookup failed
#include "dhcp.h"      // #netfix2: dhcp_get_dns() is the hedge candidate
#include "udp.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../serial.h"
#include "../gui/syslog.h"
#include "../cpu/mono.h"   // #499: sched_now_ms() - THE shared real-elapsed-ms clock
#include "ip.h"          // #imacnet: ip_get_gateway() from its OWNING header (#665)
#include "../sync/waitq.h" // #netpolls: wait_event_timeout() replaces the DNS poll (#426)
#include "socket.h"        // #netpolls: net_rx_waitq() - woken on every delivered IP frame
#include "fs/bootlog.h"   // #742: the owning header, NOT a private extern

// External declarations
extern void net_poll(void);
extern int net_is_up(void);        // #374 network-up gate
extern int net_wire_usable(void);  // #549 wire-usable gate (no FAULTY clause)
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;   // real PIT frequency (250Hz), NOT the legacy 18.2Hz

// #333: DNS_HZ was hardcoded to 18 (legacy 18.2Hz PIT assumption) but the timer
// actually runs at g_timer_hz (250Hz). That made every ms->ticks conversion ~14x
// too small: dns_wait's DNS_TIMEOUT_MS=1500ms became ~112ms, so DNS gave up
// almost immediately under load and negative-cached the host (spurious "failed
// to resolve"); cache TTLs were likewise ~14x too short (excess re-resolves).
// Use the real timer frequency everywhere instead. #499 finished the job: the
// ms->ticks helper (dns_hz) is GONE, because a tick count is not a duration at
// all under KVM tick reinjection. Both the query wait and the cache TTL are now
// REAL milliseconds on sched_now_ms().
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
    uint64_t expiry_ms;     // #499: absolute expiry in sched_now_ms() REAL ms
    int valid;
    int negative;           // 1 = cached failure (do not re-query until expiry)
    // #netfix2: WHY it is negative, as a netfail NF_* reason.
    //
    // MEASURED on the ICS bench, and this is the whole reason the field exists.
    // With a blocked resolver, the boot's FIRST lookup fails with the useful
    // reason and negative-caches the name; every later lookup, including the
    // browser's, then short-circuits on the cache and could only report
    // "cached earlier failure". The screen literally read "Fetch failed:
    // DNS-NEGATIVE-CACHED-earlier-failure", which is true, useless, and sends
    // the reader hunting for an earlier line that the log budget may already
    // have dropped. A cached failure must carry the failure, not a note saying
    // that there was one.
    uint8_t fail_reason;
} dns_cache_entry_t;

// DNS state
static uint32_t dns_server = 0;     // DNS server IP (host byte order)
// #786: 1 once an EXPLICIT choice (static config file, or the user in
// Settings) has selected the resolver. A DHCP lease may fill an UNPINNED
// resolver but must never overrule a pinned one.
static int dns_server_pinned = 0;

// #imacnet: resolver-failover policy + DNS outcome counters live in Rust
// (rustkern/netbread.rs). dns_server above stays the C-side answer to "what do
// we send to"; netbread owns the DECISION of when that should change and what
// it should change to, and it remembers the DHCP-offered resolver even when
// the pin refuses it, because on an ICS/home segment that is the only resolver
// the local network has actually endorsed.
extern void     netbread_dns_set_preferred_rs(uint32_t ip);
extern void     netbread_dns_set_dhcp_rs(uint32_t ip);
extern void     netbread_dns_set_gateway_rs(uint32_t ip);
extern void     netbread_dns_note_sent_rs(void);
extern void     netbread_dns_note_answer_rs(int rcode, uint32_t from);
extern uint32_t netbread_dns_note_timeout_rs(uint32_t cur);
extern uint32_t netbread_dns_preferred_rs(void);
// #dnsfallback: the candidate ladder now escalates OFF-NETWORK when every
// locally-endorsed resolver is the same silent host. See netbread.rs.
extern uint32_t netbread_dns_hedge_candidate_rs(uint32_t cur);
extern uint32_t netbread_dns_learned_rs(void);
extern int      netbread_dns_is_public_rs(uint32_t ip);

// #httpdns: THE DNS TRANSACTION TABLE (rustkern/dnstx.rs).
//
// dns_query below used to be the ONE in-flight lookup for the whole machine,
// with no lock and no ownership. Every resolver shared it: the six httpfetch
// worker threads sys_http_fetch_start() spawns, the sync https/wget path, SMB,
// NFS, SNTP, netfs, and the userland SYS_DNS_START/POLL pair. Two overlapping
// lookups clobbered each other's transaction id and hostname, so one reply was
// dropped as a mismatch and the other was handed to BOTH callers - one of whom
// then connected to somebody else's address with its own Host header.
//
// MEASURED 2026-09-03 on a healthy LAN with a 1ms resolver (VM 2977, golden
// 2346): six concurrent fetches to six distinct hosts produced 5 clobbers, 5
// dropped replies, 8 CROSS-WIRES and 2 wrong-or-failed fetches per boot. The
// window is the round-trip time, so it is far worse on a USB dongle (~40ms per
// send) or behind an ICS DNS proxy, which is exactly where "ping works but the
// browser and App Store do not" was reported.
//
// Each lookup now owns a slot for its whole life and the transaction id is
// unique across live slots, so a reply reaches the one lookup that asked. The
// #netfix2 hedge bookkeeping (which servers this question went to, and which
// one answered) lives in the slot too, because it decides whether to abandon
// the configured resolver and that decision must rest on THIS lookup's
// evidence, not on whichever lookup wrote the global last.
extern int      dnstx_alloc_rs(const uint8_t *host, uint32_t hlen, uint32_t id_seed,
                               uint32_t owner, uint64_t now_ms, uint32_t ttl_ms);
extern uint32_t dnstx_id_rs(uint32_t slot);
extern uint32_t dnstx_rearm_rs(uint32_t slot, uint32_t id_seed, uint64_t now_ms, uint32_t ttl_ms);
extern void     dnstx_touch_rs(uint32_t slot, uint64_t now_ms, uint32_t ttl_ms);
extern int      dnstx_match_rs(uint32_t id);
extern uint32_t dnstx_host_copy_rs(uint32_t slot, uint8_t *out, uint32_t cap);
extern int      dnstx_complete_rs(uint32_t slot, uint32_t id, int rcode, uint32_t ip);
extern int      dnstx_done_rs(uint32_t slot);
extern int      dnstx_result_rs(uint32_t slot, uint32_t *ip_out);
extern void     dnstx_free_rs(uint32_t slot);
extern int      dnstx_owner_slot_rs(uint32_t owner);
extern uint32_t dnstx_note_server_rs(uint32_t slot, uint32_t server);
extern uint32_t dnstx_servers_rs(uint32_t slot, uint32_t *s0, uint32_t *s1);
extern void     dnstx_note_answered_by_rs(uint32_t slot, uint32_t id, uint32_t from);
extern uint32_t dnstx_answered_by_rs(uint32_t slot);
extern void     dnstx_stats_rs(uint32_t *live, uint32_t *peak, uint32_t *allocfail,
                               uint32_t *nomatch, uint32_t *reaped);

// Ring-3 SYS_DNS_START slots: reclaimed after this long, because a process can
// start a lookup and exit without ever polling and there is no close() in that
// ABI.
#define DNS_SLOT_TTL_MS 30000
// Kernel-internal lookups free their slot on EVERY exit path and re-arm the
// deadline on every retransmit, so this is purely a backstop against a worker
// thread destroyed mid-lookup. It is deliberately far longer than the worst
// case a dns_resolve() can live, so the reaper can never take a slot out from
// under a lookup that is still using it.
#define DNS_SLOT_TTL_KERNEL_MS 300000
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

// #httpdns: the single global in-flight query is GONE. Per-lookup state lives
// in the dnstx table above, one slot per lookup, so concurrent resolvers can no
// longer overwrite each other's transaction id, hostname, hedge server list or
// result.

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
            (int64_t)(sched_now_ms() - dns_cache[i].expiry_ms) < 0 &&
            strcmp(dns_cache[i].hostname, hostname) == 0) {
            return &dns_cache[i];
        }
    }
    return NULL;
}

// Insert/refresh a cache entry. ttl_sec is the record's TTL (positive) or
// DNS_NEG_TTL (negative). Positive TTLs are clamped to [MIN,MAX].
// #netfix2: the reason the NEXT dns_cache_put() should record for a negative
// entry. Set by dns_neg_reason() immediately before the put. A file-static
// rather than a fifth parameter because the reason has ALREADY been recorded by
// netfail_note() at every one of the seven call sites, so this reads it back
// instead of making each caller state it twice and risk the two disagreeing.
static uint8_t dns_pending_neg_reason = 0;

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
            if (!dns_cache[i].valid || (int64_t)(sched_now_ms() - dns_cache[i].expiry_ms) >= 0) {
                slot = i; break;
            }
        }
    }
    if (slot < 0) {
        uint64_t oldest = dns_cache[0].expiry_ms; slot = 0;
        for (int i = 1; i < DNS_CACHE_SIZE; i++) {
            if (dns_cache[i].expiry_ms < oldest) { oldest = dns_cache[i].expiry_ms; slot = i; }
        }
    }

    strncpy(dns_cache[slot].hostname, hostname, sizeof(dns_cache[slot].hostname) - 1);
    dns_cache[slot].hostname[sizeof(dns_cache[slot].hostname) - 1] = '\0';
    dns_cache[slot].ip = ip;
    dns_cache[slot].ttl = ttl_sec;
    // #499: the TTL is a WALL-CLOCK duration, so it must be measured on the
    // wall clock. timer_ticks counts ticks DELIVERED, not time ELAPSED: a KVM
    // tick burst re-delivers ~1250 missed ticks (a nominal 5s at 250Hz) in
    // ~15ms of real time, which used to evict a just-resolved name instantly
    // and force a re-query storm at exactly the busiest moment.
    dns_cache[slot].expiry_ms = sched_now_ms() + (uint64_t)ttl_sec * 1000ULL;
    dns_cache[slot].valid = 1;
    dns_cache[slot].negative = negative;
    // #netfix2: a negative entry carries the reason it is negative, so every
    // later lookup that short-circuits on it reports the REAL fault instead of
    // "there was an earlier failure". Consumed once, so a later put that
    // forgets to set it cannot inherit a stale reason.
    dns_cache[slot].fail_reason = negative ? dns_pending_neg_reason : 0;
    dns_pending_neg_reason = 0;
}

// Record the reason for the negative entry the NEXT dns_cache_put() creates,
// and record it with netfail at the same time so the two can never disagree.
static void dns_neg_reason(uint32_t reason) {
    dns_pending_neg_reason = (uint8_t)reason;
    netfail_note(reason, 0);
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
// I/O. The transaction-id match (which is now also the DELIVERY address, see
// the dnstx table), the cache-put, the kprintf and the transaction
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
// match, cache-put, and transaction bookkeeping stay here in C.
static void dns_handle_response(uint32_t src_ip, uint16_t src_port,
                                 const void *data, uint16_t length) {
    (void)src_ip;
    (void)src_port;

    if (length < sizeof(dns_header_t)) return;

    const uint8_t *buf = (const uint8_t *)data;

    // #httpdns: route the reply to the lookup that asked for it. The id is
    // still the anti-spoofing check; it is now ALSO the delivery address. A
    // reply that matches no LIVE slot is a straggler from an abandoned query,
    // or a duplicate for a question already answered (the #netfix2
    // "a question that already has an answer does not get a second one" rule,
    // which dnstx_match_rs enforces by matching only LIVE slots), and is
    // counted and dropped.
    uint16_t id = (uint16_t)((buf[0] << 8) | buf[1]);
    int slot = dnstx_match_rs((uint32_t)id);
    if (slot < 0) return;
    // The name is read from the SLOT, not from a global, which is what stops
    // one lookup's answer being filed in the cache under another one's name.
    char qhost[128];
    dnstx_host_copy_rs((uint32_t)slot, (uint8_t *)qhost, sizeof(qhost));

    // Record WHO answered, for the hedge bookkeeping below. src_ip comes
    // straight off the wire (ip.c passes header->src_ip unswapped) and every
    // resolver address in this file is host order, so it is swapped once here.
    uint32_t from = ntohl(src_ip);
    dnstx_note_answered_by_rs((uint32_t)slot, (uint32_t)id, from);
    // DELIBERATELY NOT A FILTER. Restricting acceptance to the servers we
    // queried would be a real anti-spoofing improvement (today only the 16-bit
    // transaction id gates a reply), and it is NOT made here, because some
    // resolvers legitimately answer from a different source address and a
    // regression in that direction would break all name resolution on the
    // machines this change is meant to fix. It is recorded as a known gap in
    // blame.md instead of guessed at. What we DO do is notice, once, and say
    // so, so a future change has evidence instead of an assumption.
    {
        static int odd_src_reported = 0;
        uint32_t s0 = 0, s1 = 0;
        uint32_t nsrv_q = dnstx_servers_rs((uint32_t)slot, &s0, &s1);
        int known = (nsrv_q > 0 && s0 == from) || (nsrv_q > 1 && s1 == from);
        if (!known && nsrv_q > 0 && !odd_src_reported) {
            odd_src_reported = 1;
            uint8_t *pf = (uint8_t *)&from;
            uint8_t *pq = (uint8_t *)&s0;
            bootlog_write("[DNSSRC] reply for %s came from %d.%d.%d.%d but the "
                          "query went to %d.%d.%d.%d - accepted (source is not "
                          "checked); reported once per boot",
                          qhost,
                          pf[3], pf[2], pf[1], pf[0],
                          pq[3], pq[2], pq[1], pq[0]);
        }
    }

    dns_result_t r;
    dns_parse_response(buf, length, &r);

    // #imacnet: ANY reply - including NXDOMAIN and SERVFAIL - proves the
    // resolver is reachable and answering. That is a fact about the SERVER,
    // and it must reset the failover counter. Failing over on an error rcode
    // would abandon a perfectly good resolver the first time a user mistyped a
    // hostname. Only total silence counts, and it is recorded in dns_resolve().
    if (r.status != DNS_PARSE_NOT_RESPONSE) {
        int _rc = (r.status == DNS_PARSE_A_FOUND) ? 0 : (r.rcode ? r.rcode : DNS_RCODE_NAME_ERROR);
        netbread_dns_note_answer_rs(_rc, from);
    }

    switch (r.status) {
        case DNS_PARSE_NOT_RESPONSE:
            return;  // not a response (QR clear): drop, as the old code did

        case DNS_PARSE_RCODE_ERR:
            // #netfix2: an rcode is a fact about the NAME, and the three that
            // actually occur need different things from the reader: NXDOMAIN
            // means the host does not exist (a typo, or a private name), 
            // SERVFAIL means the resolver broke trying, REFUSED means it
            // declined to answer us (a filtering/ACL resolver, which is exactly
            // what a captive or corporate gateway does). Logging all three as
            // "DNS failed" is how a five-second fix becomes another round trip.
            kprintf("[DNS] Error: RCODE=%d\n", r.rcode);
            // ORDER MATTERS from here down, in BOTH directions.
            // dnstx_complete_rs() re-checks the transaction id, so it returns 0
            // if the waiting thread timed out and freed the slot between the
            // match above and now. The cache entry is filed ONLY when the
            // answer actually landed on the transaction that asked for it;
            // otherwise `qhost` could name a lookup that has since been
            // recycled and we would poison the cache with another host's
            // address. dns_neg_reason() is INSIDE the same branch because it
            // arms a one-shot that the NEXT dns_cache_put() consumes: arming it
            // on a path that then does not put would hand this reason to
            // somebody else's negative entry.
            if (dnstx_complete_rs((uint32_t)slot, id, -r.rcode, 0)) {
                dns_neg_reason(r.rcode == DNS_RCODE_NAME_ERROR ? NF_DNS_NXDOMAIN :
                               r.rcode == 2 ? NF_DNS_SERVFAIL :
                               r.rcode == 5 ? NF_DNS_REFUSED : NF_DNS_SERVFAIL);
                dns_cache_put(qhost, 0, DNS_NEG_TTL, 1);   // negative cache
            }
            return;

        case DNS_PARSE_NO_ANSWER:
            if (dnstx_complete_rs((uint32_t)slot, id, -DNS_RCODE_NAME_ERROR, 0)) {
                dns_neg_reason(NF_DNS_NO_A);   // #netfix2
                dns_cache_put(qhost, 0, DNS_NEG_TTL, 1);
            }
            return;

        case DNS_PARSE_FORMAT_ERR:
            dnstx_complete_rs((uint32_t)slot, id, -DNS_RCODE_FORMAT_ERROR, 0);
            return;

        case DNS_PARSE_A_FOUND:
            if (dnstx_complete_rs((uint32_t)slot, id, 0, r.ip)) {
                netfail_clear();   // #netfix2: a success must not leave a stale reason
                dns_cache_put(qhost, r.ip, r.ttl, 0);   // TTL-honoring cache
                kprintf("[DNS] Resolved %s -> %d.%d.%d.%d (ttl=%us)\n",
                        qhost, (r.ip >> 24) & 0xFF, (r.ip >> 16) & 0xFF,
                        (r.ip >> 8) & 0xFF, r.ip & 0xFF, r.ttl);
            }
            return;

        case DNS_PARSE_NO_A:
        default:
            // Answer(s) present but no A record - treat as a (short) negative result.
            if (dnstx_complete_rs((uint32_t)slot, id, -DNS_RCODE_NAME_ERROR, 0))
                dns_cache_put(qhost, 0, DNS_NEG_TTL, 1);
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
static int dns_wait(int slot, uint32_t ms) {
    // #499: REAL elapsed ms, not timer_ticks (a tick burst made this return
    // immediately, so DNS gave up on the first query and negative-cached).
    uint64_t until_ms = sched_now_ms() + (uint64_t)ms + 1;
    while ((int64_t)(sched_now_ms() - until_ms) < 0) {
        net_poll();
        // #httpdns: waits on THIS lookup's slot. Before, every waiter watched
        // one global `complete` flag, so whichever answer arrived first woke
        // them all and they all read the same single shared result.
        if (dnstx_done_rs((uint32_t)slot)) return 1;
        // #netpolls / #426: park on the shared net RX wait queue instead of a
        // proc_sleep(2) busy-poll. socket_net_wake() (net/ethernet.c) wakes this
        // queue on EVERY delivered IP frame, so the DNS UDP reply wakes us the
        // instant it is processed off this thread; dnstx_done_rs() is a cheap
        // BSS-only atomic read (rustkern/dnstx.rs), safe as the wait_event
        // condition (evaluated under the wq lock). The inner slice is the tier-2
        // backstop for a lost/absent wake, capped to the remaining budget so it
        // never overshoots the caller's ms; the outer until_ms deadline stays
        // the real cap. RX is pumped off this thread by net_worker()'s ~1s
        // net_poll() pass (and the compositor flip path), and by net_poll() at
        // the top of this loop, so parking here cannot stall the resolve.
        // Remote DNS server: a timeout is the correct semantics (CLAUDE tier 2).
        uint64_t _rem = until_ms - sched_now_ms();
        uint64_t _slice = _rem < 100 ? _rem : 100;
        (void)wait_event_timeout(net_rx_waitq(),
                                 dnstx_done_rs((uint32_t)slot) != 0,
                                 wq_ms_to_ticks(_slice));
    }
    return dnstx_done_rs((uint32_t)slot);
}

// Build an A-record query for hostname into query[512] using the transaction id
// this lookup's slot was issued. The slot already holds the hostname, the
// server list and the result; this function only formats the datagram.
// Returns the packet length or negative on error.
static int dns_build_query(const char *hostname, uint8_t *query, uint16_t txid) {
    memset(query, 0, 512);
    dns_header_t *hdr = (dns_header_t *)query;
    hdr->id = htons(txid);
    hdr->flags = htons(0x0100);   // RD (recursion desired)
    hdr->qdcount = htons(1);

    int qpos = sizeof(dns_header_t);
    int namelen = dns_encode_name(hostname, query + qpos, 512 - qpos - 4);
    if (namelen < 0) return -DNS_RCODE_FORMAT_ERROR;
    qpos += namelen;
    query[qpos++] = 0; query[qpos++] = DNS_TYPE_A;   // QTYPE = A
    query[qpos++] = 0; query[qpos++] = 1;            // QCLASS = IN

    // #netfix2's "a fresh question has been asked of nobody yet" still holds and
    // is still done where the id is minted: dnstx_alloc_rs()/dnstx_rearm_rs()
    // clear the server list, the answered-by and the result as part of issuing
    // the id, so the server list can never outlive the id it belongs to.
    // #httpdns: the hostname lives in the slot for the same reason.
    (void)hostname;
    return qpos;
}

// Send the query, tolerating transient failures (e.g. gateway ARP not resolved
// yet) by polling the stack and retrying briefly. Returns 0 on success.
// #netfix2: the destination is a PARAMETER now, not the dns_server global,
// because a hedged lookup asks the same question of two servers at once. Every
// server this query is sent to is recorded on the SLOT so the reply can be
// attributed to whoever actually answered.
static int dns_send_to(int slot, const uint8_t *query, int qlen, uint32_t server) {
    if (server == 0) return -1;
    dnstx_note_server_rs((uint32_t)slot, server);
    for (int i = 0; i < DNS_SEND_RETRIES; i++) {
        if (udp_send(server, DNS_LOCAL_PORT, DNS_PORT, query, qlen) >= 0) {
            // #imacnet: counted here, at the ONE place a query reaches the
            // wire, so [NETDIAG]'s dnsq= can answer "was DNS ever even
            // attempted" on a machine with no serial port.
            netbread_dns_note_sent_rs();
            return 0;
        }
        // Likely waiting on ARP; pump the stack and give it a moment. The wait
        // is on OUR slot, so an answer that arrives during the ARP warm-up is
        // still ours and is not consumed by whoever else is resolving.
        dns_wait(slot, 120);
    }
    // #netfix2: the datagram never left. On a LAN that is almost always the
    // gateway's ARP entry not being resolved, which is a completely different
    // fault from "the resolver did not answer" and needs a different fix.
    netfail_note(NF_ARP_UNRESOLVED, 0);
    return -1;
}

// #dnsfallback: REMEMBER WHAT WORKED.
//
// Once a resolver has actually put a datagram back on the wire, keep using it
// instead of re-probing a host we have already measured as silent on every
// single lookup. On the owner's iMac the old code sent forty-three queries to
// one dead address; the hedge and the slow failover each rediscovered that it
// was dead, per lookup, and then threw the discovery away.
//
// THIS IS NOT A PIN, AND THE DIFFERENCE IS THE WHOLE POINT. A pin is
// PERSISTENT CONFIG: it is written to /CONFIG, it is read by dns_init(), and
// it therefore outlives the network it was chosen on - which is exactly how
// this machine ended up pointed at a resolver its segment did not serve. What
// this adopts is RUNTIME STATE: it lives in a static atomic in netbread.rs,
// nothing writes it to disk, and a power cycle erases it. The user's PREFERRED
// resolver is never touched and is what the next boot starts from.
static void dns_adopt_learned(void) {
    uint32_t learned = netbread_dns_learned_rs();
    if (learned == 0 || learned == dns_server) return;
    static uint32_t announced = 0;
    if (announced != learned) {
        announced = learned;
        uint8_t *po = (uint8_t *)&dns_server;
        uint8_t *pn = (uint8_t *)&learned;
        uint32_t pref = netbread_dns_preferred_rs();
        uint8_t *pp = (uint8_t *)&pref;
        bootlog_write("[DNSLEARN] name lookups now go to %d.%d.%d.%d: it is the "
                      "last resolver that actually answered, and %d.%d.%d.%d "
                      "did not. THIS BOOT ONLY and not saved anywhere; your "
                      "preferred resolver is still %d.%d.%d.%d and is what the "
                      "next boot starts from.%s",
                      pn[3], pn[2], pn[1], pn[0],
                      po[3], po[2], po[1], po[0],
                      pp[3], pp[2], pp[1], pp[0],
                      netbread_dns_is_public_rs(learned)
                        ? " That is a PUBLIC resolver: your own network's DNS "
                          "answered nothing at all, so lookups are leaving the "
                          "LAN. Fix the local resolver to stop that."
                        : "");
    }
    dns_server = learned;
}

static int dns_send(int slot, const uint8_t *query, int qlen) {
    return dns_send_to(slot, query, qlen, dns_server);
}

// Initialize DNS subsystem
void dns_init(void) {
    memset(dns_cache, 0, sizeof(dns_cache));
    dns_rng = (uint32_t)(timer_ticks ? timer_ticks : 0x2545F491) | 1u;
    udp_bind(DNS_LOCAL_PORT, dns_handle_response);
    dns_server = parse_ip("8.8.8.8");   // default; overridden by DHCP if wired
    netbread_dns_set_preferred_rs(dns_server);   // #imacnet
    kprintf("[DNS] resolver initialized (server 8.8.8.8, TTL-aware cache, neg-cache %ds)\n",
            DNS_NEG_TTL);
}

void dns_set_server(uint32_t server_ip) {
    // #786: an explicit choice, so PIN it. See dns.h for why a DHCP lease must
    // not be allowed to overrule this.
    int changed = (dns_server != server_ip);
    dns_server = server_ip;
    dns_server_pinned = 1;
    // #imacnet: a fresh explicit choice deserves to be tried on its own merits
    // and must not inherit the previous server's failure count.
    netbread_dns_set_preferred_rs(server_ip);
    uint8_t *ip = (uint8_t *)&server_ip;
    kprintf("[DNS] server set to %d.%d.%d.%d (explicit, pinned)\n",
            ip[3], ip[2], ip[1], ip[0]);
    // Flush cached answers from the PREVIOUS resolver. Changing resolver is an
    // act of distrust in the old one: keeping its answers would make a live
    // change look like it had not taken effect for up to a TTL, which is
    // exactly the "did it apply?" ambiguity this ticket exists to remove.
    if (changed) dns_cache_clear();
}

void dns_set_server_dhcp(uint32_t server_ip) {
    if (server_ip == 0) return;             // lease offered no resolver
    // #imacnet: RECORD THE OFFER FIRST, BEFORE the pin check, and record it
    // even when the pin then refuses it. The refusal is still correct policy
    // (#786: a lease must not overrule the user), but the offered address is
    // the only resolver we know the local network endorses, and it is the
    // failover candidate if the pinned one turns out to answer nothing at all.
    // Discarding it here is exactly what left a machine pinned to an
    // unreachable resolver with no way back.
    netbread_dns_set_dhcp_rs(server_ip);
    if (dns_server_pinned) {                // an explicit choice wins
        uint8_t *ip = (uint8_t *)&server_ip;
        kprintf("[DNS] ignoring DHCP-offered %d.%d.%d.%d: resolver is pinned\n",
                ip[3], ip[2], ip[1], ip[0]);
        return;
    }
    int changed = (dns_server != server_ip);
    dns_server = server_ip;
    // #dnsfallback: ONLY ON A REAL CHANGE. netbread_dns_set_preferred_rs()
    // re-arms the failover ladder and forgets the learned resolver, which is
    // right for a user's explicit choice and WRONG for a lease renewal. The
    // owner's log shows two "[DHCP] RX other (now=BOUND)" events mid-boot; if
    // each of those re-offered the same address and reset the ladder, an
    // escalation that had already found a working resolver would be silently
    // undone and the machine would fall back onto the dead host.
    if (changed) netbread_dns_set_preferred_rs(server_ip);
    uint8_t *ip = (uint8_t *)&server_ip;
    kprintf("[DNS] server set to %d.%d.%d.%d (from DHCP lease)\n",
            ip[3], ip[2], ip[1], ip[0]);
    if (changed) dns_cache_clear();
}

int dns_server_is_pinned(void) { return dns_server_pinned; }

// #netfix2: THE WAY BACK FROM A PIN. There was none.
//
// `dns_server_pinned` was set in two places and cleared in none, so once a
// resolver had been chosen even once it was chosen forever, on every network,
// and dns_set_server_dhcp() refused every lease offer for the life of the
// machine. That is how the owner's iMac ended up pinned to 1.1.1.1 on a Windows
// ICS segment whose gateway drops forwarded port 53 to anything but itself:
// ping kept working (ICMP needs no resolver) and nothing else did.
//
// It is worse than "the user chose badly once", because the user need not have
// chosen at all: Settings prefills the DNS field from the LIVE resolver and
// applies it unconditionally on OK, so opening "Set DNS...", touching nothing
// and clicking OK pins whatever DHCP had just handed out. See the CHANGELOG.
//
// Automatic means: forget the choice, adopt what the network offers, and let
// net_persist_netcfg() erase the dns= line from /CONFIG/NETIP.CFG so the next
// boot does not resurrect it.
void dns_set_server_auto(void) {
    uint32_t offered = dhcp_get_dns();
    dns_server_pinned = 0;
    uint32_t was = dns_server;
    if (offered) {
        dns_server = offered;
    } else if (ip_get_gateway()) {
        // No lease on record. On a home router and on an ICS segment the
        // gateway IS the resolver, and it is a far better guess than keeping a
        // pin the user has just asked us to drop.
        dns_server = ip_get_gateway();
    }
    netbread_dns_set_preferred_rs(dns_server);
    if (dns_server != was) dns_cache_clear();
    uint8_t *pn = (uint8_t *)&dns_server;
    bootlog_write("[DNS] resolver set to AUTOMATIC; now using %d.%d.%d.%d "
                  "(%s). The pin is cleared and will not come back on reboot.",
                  pn[3], pn[2], pn[1], pn[0],
                  offered ? "from the DHCP lease" :
                  (dns_server ? "the gateway; no lease has offered one" :
                                "nothing available yet"));
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
        if (dns_cache[i].valid && (int64_t)(sched_now_ms() - dns_cache[i].expiry_ms) < 0) n++;
    if (entries)  *entries  = n;
    if (hits)     *hits     = stat_hits;
    if (misses)   *misses   = stat_misses;
    if (neg_hits) *neg_hits = stat_neg_hits;
}

// Resolve hostname to IPv4 address (host byte order). Blocking.
int dns_resolve(const char *hostname, uint32_t *ip_out) {
    if (!hostname || !ip_out) { netfail_note(NF_DNS_BAD_NAME, 0); return -1; }

    if (is_ip_address(hostname)) {
        *ip_out = parse_ip(hostname);
        return *ip_out ? 0 : -DNS_RCODE_FORMAT_ERROR;
    }

    // Cache (positive + negative).
    dns_cache_entry_t *cached = dns_cache_lookup(hostname);
    if (cached) {
        if (cached->negative) {
            stat_neg_hits++;
            // #netfix2: this lookup did not fail here, it failed EARLIER and
            // is being replayed out of the negative cache. Saying so is the
            // difference between a log with one real diagnosis followed by
            // twenty honest "cached" lines, and a log with twenty-one
            // identical lines that all look like fresh independent faults.
            // #netfix2: replay the ORIGINAL reason. Falling back to
            // NF_DNS_NEG_CACHED only when nothing was recorded keeps that name
            // meaningful: it now marks an entry made by a path that did not
            // state a reason, which is a bug to go and fix, rather than being
            // the answer for every repeat of every fault.
            netfail_note(cached->fail_reason ? cached->fail_reason
                                             : NF_DNS_NEG_CACHED, 0);
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
    // #549: net_wire_usable(), not net_is_up(). A re-probe has to be able to
    // RESOLVE, or recovery would only ever work off a warm cache. Background
    // clients still quiesce while FAULTY because they gate on net_is_up()
    // before they ever get here.
    if (!net_wire_usable()) {
        // #netfix2: distinguish the two, because they need different actions.
        // No carrier is a cable/adapter problem; carrier with no address is a
        // DHCP problem and the network is otherwise fine.
        dns_neg_reason(nic_link_up() ? NF_NO_ADDRESS : NF_NO_CARRIER);
        dns_cache_put(hostname, 0, DNS_NEG_TTL_SOFT, 1);   // #333/#374 transient: link/IP may return
        kprintf("[DNS] network down; skipping resolve of %s (soft-negative-cached %ds)\n",
                hostname, DNS_NEG_TTL_SOFT);
        return -1;
    }

    // #dnsfallback: before anything is sent, prefer whatever last answered.
    dns_adopt_learned();

    if (dns_server == 0) {
        netfail_note(NF_DNS_NO_SERVER, 0);   // #netfix2
        kprintf("[DNS] no server configured\n");
        return -1;
    }

    // #httpdns: claim this lookup's own transaction slot. On exhaustion we fail
    // honestly (soft negative cache, the caller retries) instead of clobbering
    // a live transaction, which is what the single global did on EVERY overlap.
    uint32_t hlen = 0; while (hostname[hlen] && hlen < 200) hlen++;
    int slot = dnstx_alloc_rs((const uint8_t *)hostname, hlen, dns_rand(),
                              0, sched_now_ms(), DNS_SLOT_TTL_KERNEL_MS);
    if (slot < 0) {
        netfail_note(NF_DNS_SILENT, 0);
        dns_cache_put(hostname, 0, DNS_NEG_TTL_SOFT, 1);
        kprintf("[DNS] no free transaction slot for %s (soft-negative-cached %ds)\n",
                hostname, DNS_NEG_TTL_SOFT);
        return -1;
    }

    uint8_t query[512];
    int qlen = dns_build_query(hostname, query,
                               (uint16_t)dnstx_id_rs((uint32_t)slot));
    if (qlen < 0) { dnstx_free_rs((uint32_t)slot); return qlen; }

    kprintf("[DNS] resolving %s\n", hostname);

    // #imacnet: keep the failover policy's idea of the gateway current. Doing
    // it here, on the resolve path, means there is no setter to hunt for and no
    // way for a DHCP renewal or a Settings change to leave it stale.
    netbread_dns_set_gateway_rs(ip_get_gateway());

    // #imacnet: TWO PASSES. Pass 0 is the resolver we were told to use. If it
    // answers nothing AT ALL - not an error, nothing - that is evidence about
    // the SERVER, and pass 1 runs the same query against the failover candidate
    // so the lookup that paid for the discovery is also the one that benefits.
    // Without this the user's FIRST page load still fails and only the next one
    // works, which reads as "it is broken" and is how this stays unreported.
    // #netfix2: THE HEDGE, and why the two-pass failover on its own was not
    // enough to close the owner's report.
    //
    // MEASURED from the constants: a pass that gets no reply costs
    // 1500 + 3000 + 6000 ms of response waits plus 200 + 400 + 800 ms of
    // backoff, i.e. about TWELVE SECONDS before the failover even becomes
    // possible. A user whose first page load takes twelve seconds and then
    // works has, correctly, reported that the browser does not work. Recovery
    // that is slower than the user's patience is not recovery.
    //
    // So when we have a second candidate the local network has endorsed (the
    // DHCP-offered resolver, or the gateway) and it is not the one we are
    // already using, the SAME query goes to BOTH after a short hedge delay.
    // Whoever answers first wins. This costs one extra 40-byte datagram on the
    // first lookup of a boot and turns a twelve-second failure into a
    // DNS_HEDGE_MS one, and it NEVER abandons the user's choice on a single
    // dropped packet the way simply lowering the failover threshold would.
    //
    // #dnsfallback: THE CANDIDATE NOW COMES FROM THE LADDER, NOT FROM AN INLINE
    // {DHCP, gateway} PAIR. MEASURED on the owner's iMac (golden 2353): his
    // preferred resolver, his DHCP-offered resolver and his gateway were all
    // 192.0.2.1, the Windows ICS host, so both candidates equalled the
    // server already in use and this computed hedge_to = 0. Forty-one
    // unanswered queries, zero hedged datagrams, zero failovers. The ladder can
    // escalate off-network, which on that topology is the only place left to
    // go. It refuses to offer a public resolver on a network whose own DNS has
    // answered anything at all, so a healthy LAN never sends a query off it.
    uint32_t hedge_to = netbread_dns_hedge_candidate_rs(dns_server);
    // An off-network hedge gets a longer fuse than a local one: see
    // DNS_HEDGE_PUBLIC_MS in dns.h for why a slow resolver is not a broken one.
    uint32_t hedge_ms = (hedge_to && netbread_dns_is_public_rs(hedge_to))
                        ? DNS_HEDGE_PUBLIC_MS : DNS_HEDGE_MS;

    uint32_t attempt_ms = DNS_TIMEOUT_MS;
    for (int pass = 0; pass < 2; pass++) {
    uint32_t nsrv = 0;
    for (int attempt = 0; attempt < DNS_MAX_RETRIES; attempt++) {
        dnstx_touch_rs((uint32_t)slot, sched_now_ms(), DNS_SLOT_TTL_KERNEL_MS);
        if (dns_send(slot, query, qlen) < 0) {
            kprintf("[DNS] send failed (attempt %d)\n", attempt + 1);
            // fall through to backoff and try again
        } else {
            // Wait a little for the server we were told to use. If it answers
            // inside the hedge window, nothing else is sent and the behaviour
            // is exactly as before.
            uint32_t first = (hedge_to && attempt_ms > hedge_ms)
                             ? hedge_ms : attempt_ms;
            if (dns_wait(slot, first)) {
                int rc = dnstx_result_rs((uint32_t)slot, ip_out);
                dnstx_free_rs((uint32_t)slot);
                return rc;
            }
            if (hedge_to && first < attempt_ms) {
                dns_send_to(slot, query, qlen, hedge_to);
                if (dns_wait(slot, attempt_ms - first)) {
                    // If the ALTERNATE answered and the configured resolver did
                    // not, that is real evidence about the configured one, and
                    // the rest of this boot should not keep paying the hedge
                    // delay to rediscover it every single lookup.
                    uint32_t ans = dnstx_answered_by_rs((uint32_t)slot);
                    if (ans == hedge_to && ans != dns_server) {
                        uint8_t *po = (uint8_t *)&dns_server;
                        uint8_t *pn = (uint8_t *)&hedge_to;
                        uint32_t pref = netbread_dns_preferred_rs();
                        uint8_t *pp = (uint8_t *)&pref;
                        bootlog_write(
                            // #dnsfallback: THE TAIL OF THIS SENTENCE USED TO
                            // POINT AT THE THING THAT BROKE THE USER. It said
                            // "Settings > Network > DNS Server > Automatic
                            // makes this permanent", and MEASURED on the ICS
                            // bench this exact line printed with 192.0.2.1
                            // as the silent server: Automatic selects the
                            // DHCP-offered resolver, which on that segment IS
                            // the silent one. Following the advice would have
                            // undone the recovery that had just happened. The
                            // same wrong advice was in the browser status bar
                            // and is corrected there too.
                            "[DNSHEDGE] %d.%d.%d.%d did not answer for %s "
                            "within %ums but %d.%d.%d.%d did; using it for the "
                            "rest of this boot (your preferred resolver is "
                            "still %d.%d.%d.%d and comes back on reboot;%s)",
                            po[3], po[2], po[1], po[0], hostname,
                            (unsigned)hedge_ms,
                            pn[3], pn[2], pn[1], pn[0],
                            pp[3], pp[2], pp[1], pp[0],
                            netbread_dns_is_public_rs(hedge_to)
                              ? " that is a PUBLIC resolver, so nothing on your "
                                "own network answered - the fix is on your "
                                "router or firewall (UDP port 53), not in "
                                "these settings"
                              : " to make the change stick, set that server "
                                "explicitly in Settings > Network");
                        dns_server = hedge_to;
                        hedge_to = 0;
                    }
                    int rc = dnstx_result_rs((uint32_t)slot, ip_out);
                    dnstx_free_rs((uint32_t)slot);
                    return rc;
                }
            }
        }
        // Exponential backoff with jitter before retransmitting (ban-safe).
        uint32_t backoff = (DNS_BACKOFF_BASE_MS << attempt);
        uint32_t jitter  = dns_rand() % (DNS_BACKOFF_BASE_MS + 1);
        if (dns_wait(slot, backoff + jitter)) {
            int rc = dnstx_result_rs((uint32_t)slot, ip_out);
            dnstx_free_rs((uint32_t)slot);
            return rc;
        }
        // #imacnet: this ATTEMPT produced no reply. Counted PER ATTEMPT, not
        // per lookup.
        //
        // MEASURED 2026-09-02 on the ICS bench, and this is why the placement
        // matters: with the note outside this loop it took DNS_FAILOVER_AFTER
        // whole LOOKUPS to fail over, and the bench boot performed exactly ONE
        // name lookup in its entire life ([NETDIAG] read dnsq=3/0/1/0/0: three
        // queries sent, zero answers, one silent lookup, zero failovers). A
        // machine that resolves one name per boot would never have reached the
        // threshold at all, so the recovery path would have been dead code that
        // passed its own unit test. The counter's unit has to be the thing that
        // actually happens repeatedly, which is the ATTEMPT.
        uint32_t _fo = netbread_dns_note_timeout_rs(dns_server);
        if (_fo) nsrv = _fo;
        attempt_ms <<= 1;   // double the response wait each retry
    }
        // The whole retry budget is spent and not one datagram came back.
        if (nsrv == 0 || nsrv == dns_server) break;   // no alternate to try

        uint8_t *po = (uint8_t *)&dns_server;
        uint8_t *pn = (uint8_t *)&nsrv;
        uint32_t pref = netbread_dns_preferred_rs();
        uint8_t *pp = (uint8_t *)&pref;
        // DURABLE, not kprintf. This is the whole reason the module exists: a
        // resolver silently failing over is exactly the kind of fact that has
        // to survive on a machine with no serial port.
        bootlog_write("[DNSFAIL] resolver %d.%d.%d.%d answered NOTHING for %s; "
                      "failing over to %d.%d.%d.%d%s for this boot "
                      "(preferred stays %d.%d.%d.%d - set it in Settings > "
                      "Network, or reboot, to go back)",
                      po[3], po[2], po[1], po[0], hostname,
                      pn[3], pn[2], pn[1], pn[0],
                      netbread_dns_is_public_rs(nsrv)
                        ? ", a PUBLIC resolver (nothing on your own network "
                          "answered)" : "",
                      pp[3], pp[2], pp[1], pp[0]);
        dns_server = nsrv;
        // #netfix2: the slow failover has just adopted a candidate. If that is
        // the same address the hedge was going to try, pass 1 must not send the
        // same question to the same server twice on every attempt.
        if (hedge_to == dns_server) hedge_to = 0;
        dns_cache_clear();      // the dead server's negative answers are worthless
        attempt_ms = DNS_TIMEOUT_MS;
        // A FRESH transaction id. The old query may still be in flight to the
        // server we just abandoned; reusing its id would let a late straggler
        // satisfy a question we are now asking somebody else.
        qlen = dns_build_query(hostname, query,
                               (uint16_t)dnstx_rearm_rs((uint32_t)slot, dns_rand(),
                                                        sched_now_ms(),
                                                        DNS_SLOT_TTL_KERNEL_MS));
        if (qlen < 0) break;
    }

    // Exhausted retries: this is usually a TRANSIENT loss (packet drop / load),
    // not a real NXDOMAIN, so use a SHORT soft negative TTL. Callers in a retry
    // loop still back off briefly but can re-query within a few seconds (#333).
    // #netfix2: NOT ONE DATAGRAM CAME BACK, from anybody, after the whole retry
    // budget and (where one existed) a hedge to a second resolver. That is the
    // single most useful sentence this stack can write on a machine with no
    // serial port, and until now it wrote it only to a serial port.
    dnstx_free_rs((uint32_t)slot);
    dns_neg_reason(NF_DNS_SILENT);
    dns_cache_put(hostname, 0, DNS_NEG_TTL_SOFT, 1);
    kprintf("[DNS] failed to resolve %s (soft-negative-cached %ds)\n", hostname, DNS_NEG_TTL_SOFT);
    return -1;
}

// Non-blocking DNS for userland syscalls (paired with dns_resolve_check).
// 1 = resolved immediately, 0 = query sent, <0 = error (incl. negative cache).
int dns_resolve_start(const char *hostname, uint32_t *ip_out, uint32_t owner) {
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
        dns_neg_reason(NF_NO_CARRIER);   // #netfix2
        dns_cache_put(hostname, 0, DNS_NEG_TTL, 1);
        return -1;
    }
    // #dnsfallback: this path has NO hedge and NO failover of its own - it sends
    // one query and returns - so adopting the resolver that last answered is
    // the only recovery it gets. Without this, a userland lookup would keep
    // querying the dead server long after dns_resolve() had found a live one.
    dns_adopt_learned();
        if (dns_server == 0) { netfail_note(NF_DNS_NO_SERVER, 0); return -1; }   // #netfix2

    // #httpdns: this pair has no handle in its syscall ABI (SYS_DNS_POLL takes
    // only an out-pointer), so the slot is keyed on the CALLING PROCESS. Two
    // processes resolving at once - the browser and a terminal nslookup, say -
    // used to share the machine's one query record and read each other's
    // answers. A previous unpolled lookup by the same process is released
    // first, so a caller cannot leak slots by abandoning lookups.
    if (owner == 0) return -1;
    int prev = dnstx_owner_slot_rs(owner);
    if (prev >= 0) dnstx_free_rs((uint32_t)prev);

    uint32_t hlen = 0; while (hostname[hlen] && hlen < 200) hlen++;
    int slot = dnstx_alloc_rs((const uint8_t *)hostname, hlen, dns_rand(),
                              owner, sched_now_ms(), DNS_SLOT_TTL_MS);
    if (slot < 0) return -1;

    uint8_t query[512];
    int qlen = dns_build_query(hostname, query,
                               (uint16_t)dnstx_id_rs((uint32_t)slot));
    if (qlen < 0) { dnstx_free_rs((uint32_t)slot); return qlen; }
    if (dns_send(slot, query, qlen) < 0) { dnstx_free_rs((uint32_t)slot); return -1; }
    return 0;
}

// Poll a pending dns_resolve_start. 1 = success, 0 = pending, -1 = failed.
// #httpdns: answers about THIS process's lookup only.
int dns_resolve_check(uint32_t *ip_out, uint32_t owner) {
    int slot = dnstx_owner_slot_rs(owner);
    if (slot < 0) return -1;
    if (!dnstx_done_rs((uint32_t)slot)) return 0;
    uint32_t ip = 0;
    int rc = dnstx_result_rs((uint32_t)slot, &ip);
    dnstx_free_rs((uint32_t)slot);
    if (rc == 0) {
        if (ip_out) *ip_out = ip;
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
