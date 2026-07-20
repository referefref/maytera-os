// arp.c - Address Resolution Protocol implementation
#include "arp.h"
#include "ethernet.h"
#include "ip.h"
#include "../serial.h"
#include "../string.h"
#include "../cpu/isr.h"

// ARP cache entry
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  valid;
} arp_entry_t;

// ARP cache
#define ARP_CACHE_SIZE 32
static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static int arp_cache_count = 0;

// ---- #333: ARP pending-packet queue --------------------------------------
// When ip_send() cannot resolve a next-hop MAC yet (a cold LAN host whose ARP
// is still pending), the packet used to be dropped and the caller got -1. For a
// LAN-local host that never had a cache entry (e.g. the Home Assistant server)
// the very first TCP SYN was lost and the fetch silently failed, while gateway-
// routed internet fetches worked because the gateway MAC is always warm. We now
// HOLD the packet and flush it the instant the ARP reply lands. This is non-
// blocking: the flush runs in the ARP receive path (arp_handle -> arp_cache_add
// -> arp_flush_pending), it never spins, sleeps, or yields (#426).
#define ARP_QUEUE_SIZE 8
#define ARP_QUEUE_BUF  1600   // >= IP_MTU (1500)
typedef struct {
    uint8_t  used;
    uint16_t type;            // ethertype (ETH_TYPE_IPV4)
    uint16_t len;
    uint32_t ip;              // next-hop IP (host order) we are waiting on
    uint64_t seq;             // insertion order, for oldest-eviction
    uint8_t  data[ARP_QUEUE_BUF];
} arp_pending_t;
static arp_pending_t arp_queue[ARP_QUEUE_SIZE];
static uint64_t arp_queue_seq = 0;

// #380: Duplicate Address Detection (RFC 5227) watcher. While g_dad_ip is
// non-zero we are probing that candidate address before adopting it; arp_handle
// sets g_dad_conflict if ANY other host is seen using it (its reply to our
// probe, or its own ARP traffic). Adopting an address only after this check
// stops MayteraOS from stealing an IP already in use on the LAN (the bug that
// hijacked a co-host's ARP entry and disrupted the real network in #380).
static volatile uint32_t g_dad_ip       = 0;
static volatile int      g_dad_conflict = 0;
static uint8_t           g_dad_ourmac[6];

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

static inline uint32_t ntohl(uint32_t n) {
    return htonl(n);
}

// Find entry in cache
static arp_entry_t *arp_find(uint32_t ip) {
    for (int i = 0; i < arp_cache_count; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            return &arp_cache[i];
        }
    }
    return NULL;
}

// Add entry to cache
static void arp_cache_add(uint32_t ip, const uint8_t *mac) {
    // Check if already exists
    arp_entry_t *entry = arp_find(ip);
    if (entry) {
        memcpy(entry->mac, mac, 6);
        return;
    }

    // Add new entry
    if (arp_cache_count < ARP_CACHE_SIZE) {
        entry = &arp_cache[arp_cache_count++];
    } else {
        // Replace oldest entry (simple LRU)
        entry = &arp_cache[0];
    }

    entry->ip = ip;
    memcpy(entry->mac, mac, 6);
    entry->valid = 1;
    // #333/#747: do NOT send held packets from here. arp_cache_add() runs inside
    // arp_handle() -> the eth_receive() RX drain, which executes under net_lock
    // (IRQs off, recursive spinlock) on the same TX ring the outer context holds.
    // b746 flushed (eth_send) from here and hung the box at boot (a send nested in
    // the receive callback = the #426 hazard). The queued packets are now flushed
    // by arp_flush_ready(), called at TOP LEVEL from net_poll() after the RX drain,
    // in the same context every other outbound packet is sent.
}

// Initialize ARP
void arp_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));
    arp_cache_count = 0;

    // Register with ethernet layer
    eth_register_handler(ETH_TYPE_ARP, arp_handle);

    kprintf("[ARP] ARP initialized\n");
}

// Send ARP request
void arp_request(uint32_t ip) {
    arp_header_t arp;

    arp.hw_type = htons(ARP_HW_ETHERNET);
    arp.proto_type = htons(ETH_TYPE_IPV4);
    arp.hw_len = 6;
    arp.proto_len = 4;
    arp.operation = htons(ARP_OP_REQUEST);

    // Sender info (our MAC and IP in network byte order)
    eth_get_mac(arp.sender_mac);
    uint32_t our_ip = ip_get_address();
    uint32_t our_ip_be = htonl(our_ip);  // Convert to network byte order
    memcpy(arp.sender_ip, &our_ip_be, 4);

    // Target info (zero MAC, requested IP in network byte order)
    memset(arp.target_mac, 0, 6);
    uint32_t target_ip_be = htonl(ip);  // Convert to network byte order
    memcpy(arp.target_ip, &target_ip_be, 4);

    // Send as broadcast
    eth_send(ETH_BROADCAST, ETH_TYPE_ARP, &arp, sizeof(arp));
}

// Resolve IP to MAC
int arp_resolve(uint32_t ip, uint8_t *mac) {
    // uint8_t *p = (uint8_t *)&ip;
    // kprintf("[ARP] arp_resolve: looking for 0x%08x (%d.%d.%d.%d)\n",
    //         ip, p[3], p[2], p[1], p[0]);

    arp_entry_t *entry = arp_find(ip);
    if (entry && mac) {
        // kprintf("[ARP] arp_resolve: FOUND in cache!\n");
        memcpy(mac, entry->mac, 6);
        return 1;
    }

    // Not found, send request
    arp_request(ip);
    return 0;
}

// #333: hold a packet whose next-hop MAC is not yet resolved. arp_resolve() has
// already emitted the ARP request; arp_flush_pending() (called from
// arp_cache_add when the reply lands) puts it on the wire. Bounded ring, evict
// oldest when full; no allocation, no spin, no sleep.
void arp_queue_pending(uint32_t next_hop_ip, uint16_t type, const void *data, uint16_t len) {
    if (!data || len == 0 || len > ARP_QUEUE_BUF) return;

    arp_pending_t *slot = 0;
    for (int i = 0; i < ARP_QUEUE_SIZE; i++) {
        if (!arp_queue[i].used) { slot = &arp_queue[i]; break; }
    }
    if (!slot) {
        // Queue full: evict the oldest entry.
        slot = &arp_queue[0];
        for (int i = 1; i < ARP_QUEUE_SIZE; i++)
            if (arp_queue[i].seq < slot->seq) slot = &arp_queue[i];
    }

    slot->used = 1;
    slot->type = type;
    slot->len  = len;
    slot->ip   = next_hop_ip;
    slot->seq  = ++arp_queue_seq;
    memcpy(slot->data, data, len);
}

// #333/#747: flush every held packet whose next-hop MAC is now resolved. Called
// at TOP LEVEL from net_poll() (never from arp_handle / the RX callback), so the
// eth_send() below runs in the normal outbound context -- exactly where dhcp_poll
// and all other sends already run -- not nested inside the receive drain. Each
// slot is dequeued (used = 0) BEFORE eth_send so a re-entry can never re-send the
// same packet. A guard makes a nested call (e.g. a recursive net_poll) a no-op.
void arp_flush_ready(void) {
    static volatile int flushing = 0;
    if (flushing) return;
    flushing = 1;
    for (int i = 0; i < ARP_QUEUE_SIZE; i++) {
        if (!arp_queue[i].used) continue;
        arp_entry_t *e = arp_find(arp_queue[i].ip);
        if (!e) continue;                       // MAC still unknown: keep holding
        uint8_t  mac[6];
        memcpy(mac, e->mac, 6);
        uint16_t type = arp_queue[i].type;
        uint16_t len  = arp_queue[i].len;
        arp_queue[i].used = 0;                  // dequeue BEFORE the send
        eth_send(mac, type, arp_queue[i].data, len);
    }
    flushing = 0;
}

// ---------------------------------------------------------------------------
// #404 / #495 Phase M: pure ARP parse/validate seam (Tier 2, untrusted wire
// input). Extracted VERBATIM out of the old arp_handle() so the C reference
// preserves the exact current behavior. Unlike the ICMP port (Phase L), the ARP
// parse in C is ALREADY memory-safe: it length-gates the frame (len >=
// sizeof(arp_header_t) == 28) BEFORE touching any field, and every address field
// lives at a FIXED offset inside those 28 bytes (the hw_len/proto_len are
// validated == 6/4 but NOT used to compute any offset), and the buffer is read
// ONLY (no in-place mutation, unlike ICMP's checksum zero+restore). So there is
// NO currently-reachable OOB read here.
//
// The Rust port (arp_parse_rs) removes the UNCHECKED-POINTER-ARITHMETIC-ON-WIRE-
// DATA class BY CONSTRUCTION (every access is a bounds-checked index into a slice
// of exactly len bytes) as defense-in-depth: if the fixed-offset assumption were
// ever broken (a struct change, or a future variable-length HW/proto address
// keyed off hw_len/proto_len), the C would silently gain an OOB while the Rust
// could not. This is HONEST latent-not-reachable hardening, not a live-bug fix;
// the boot self-test's [RUST-SEC] line reports that C and Rust reject/accept the
// malformed corpus IDENTICALLY (zero divergence), i.e. no live OOB exists to fix.
//
// arp_parse_c is byte-for-byte the old logic; keep it as the reference and the
// rollback (undefine RUST_ARP -> arp_parse() falls straight back to it).
//
// Static-assert the FFI struct layout so the #[repr(C)] ArpParsed in rustkern.rs
// can never silently drift from arp_parsed_t.
_Static_assert(sizeof(arp_parsed_t) == 28, "arp_parsed_t must be 28 bytes for the Rust FFI");

int arp_parse_c(const uint8_t *buf, uint32_t len, arp_parsed_t *out) {
    // Guard (verbatim `if (length < sizeof(arp_header_t)) return;`).
    if (len < sizeof(arp_header_t)) {
        return ARP_PARSE_ETOOSHORT;
    }

    const arp_header_t *arp = (const arp_header_t *)buf;

    // Validate (verbatim header check). Read-only; no const cast needed.
    if (ntohs(arp->hw_type) != ARP_HW_ETHERNET ||
        ntohs(arp->proto_type) != ETH_TYPE_IPV4 ||
        arp->hw_len != 6 || arp->proto_len != 4) {
        return ARP_PARSE_EINVAL;
    }

    if (out) {
        out->hw_type    = ntohs(arp->hw_type);
        out->proto_type = ntohs(arp->proto_type);
        out->operation  = ntohs(arp->operation);
        out->hw_len     = arp->hw_len;
        out->proto_len  = arp->proto_len;

        uint32_t sender_ip_be;
        memcpy(&sender_ip_be, arp->sender_ip, 4);
        out->sender_ip  = ntohl(sender_ip_be);

        uint32_t target_ip_be;
        memcpy(&target_ip_be, arp->target_ip, 4);
        out->target_ip  = ntohl(target_ip_be);

        memcpy(out->sender_mac, arp->sender_mac, 6);
        memcpy(out->sender_ip_be, arp->sender_ip, 4);  // raw network bytes
    }
    return ARP_PARSE_OK;
}

// Live dispatcher. With -DRUST_ARP (set in the Makefile) the incoming ARP parse
// runs in Rust; drop the flag + rebuild to roll straight back to C.
int arp_parse(const uint8_t *buf, uint32_t len, arp_parsed_t *out) {
#ifdef RUST_ARP
    return arp_parse_rs(buf, len, out);
#else
    return arp_parse_c(buf, len, out);
#endif
}

// Handle incoming ARP packet. The untrusted PARSE/validate is now arp_parse()
// (Rust under -DRUST_ARP); only the DAD check, cache-add, and reply-send
// bookkeeping stays here in C.
void arp_handle(const uint8_t *src_mac, const void *data, uint16_t length) {
    (void)src_mac;

    // kprintf("[ARP] arp_handle called, len=%d\n", length);

    arp_parsed_t p;
    int rc = arp_parse((const uint8_t *)data, length, &p);
    if (rc != ARP_PARSE_OK) {
        // Too short or not an Ethernet/IPv4 ARP: drop, exactly as the old
        // early-return branches did.
        return;
    }

    uint32_t sender_ip = p.sender_ip;   // host order

    // #380 DAD: if we are probing a candidate address, any packet from a
    // DIFFERENT host claiming that IP means it is already in use. Checked here,
    // before the "is this for us" gate, so it catches both a defender's reply
    // to our probe and the defender's own gratuitous/announce ARPs.
    if (g_dad_ip != 0 && sender_ip == g_dad_ip &&
        memcmp(p.sender_mac, g_dad_ourmac, 6) != 0) {
        g_dad_conflict = 1;
    }

    // Check if this is for us (target already in host order from the parse).
    uint32_t target_ip = p.target_ip;
    uint32_t our_ip = ip_get_address();

    if (target_ip != our_ip) {
        // kprintf("[ARP] Not for us (target != our)\n");
        return;  // Not for us
    }

    // Add sender to cache only for ARP packets addressed to us
    // (prevents random LAN broadcast ARPs from filling the 32-slot cache)
    arp_cache_add(sender_ip, p.sender_mac);

    uint16_t op = p.operation;   // host order
    // kprintf("[ARP] Operation: %d (1=request, 2=reply)\n", op);

    if (op == ARP_OP_REQUEST) {
        // kprintf("[ARP] Sending ARP reply!\n");
        // Send reply
        arp_header_t reply;
        reply.hw_type = htons(ARP_HW_ETHERNET);
        reply.proto_type = htons(ETH_TYPE_IPV4);
        reply.hw_len = 6;
        reply.proto_len = 4;
        reply.operation = htons(ARP_OP_REPLY);

        eth_get_mac(reply.sender_mac);
        uint32_t our_ip_be = htonl(our_ip);  // Convert to network order
        memcpy(reply.sender_ip, &our_ip_be, 4);
        memcpy(reply.target_mac, p.sender_mac, 6);
        memcpy(reply.target_ip, p.sender_ip_be, 4);  // Already in network order

        eth_send(p.sender_mac, ETH_TYPE_ARP, &reply, sizeof(reply));
    }
}

// Add static ARP entry
void arp_add_entry(uint32_t ip, const uint8_t *mac) {
    arp_cache_add(ip, mac);
}

// Print ARP table
void arp_print_table(void) {
    kprintf("\n[ARP] ARP Table:\n");
    kprintf("  IP Address        MAC Address\n");
    kprintf("  ---------------   -----------------\n");

    for (int i = 0; i < arp_cache_count; i++) {
        if (arp_cache[i].valid) {
            uint8_t *ip = (uint8_t *)&arp_cache[i].ip;
            kprintf("  %d.%d.%d.%d      %02x:%02x:%02x:%02x:%02x:%02x\n",
                    ip[3], ip[2], ip[1], ip[0],
                    arp_cache[i].mac[0], arp_cache[i].mac[1],
                    arp_cache[i].mac[2], arp_cache[i].mac[3],
                    arp_cache[i].mac[4], arp_cache[i].mac[5]);
        }
    }
}

// #380 DAD: arm/disarm the watcher and read the conflict flag.
void arp_dad_arm(uint32_t ip) {
    g_dad_ip = ip;
    g_dad_conflict = 0;
    eth_get_mac(g_dad_ourmac);
}
int  arp_dad_conflict(void) { return g_dad_conflict; }
void arp_dad_disarm(void) { g_dad_ip = 0; g_dad_conflict = 0; }

// Send an RFC 5227 ARP probe: sender IP = 0.0.0.0 (we do NOT yet own the
// address), target = candidate. A host already using it will reply, which
// arp_handle turns into a conflict. Broadcasting sender 0.0.0.0 cannot poison
// any neighbor's cache (nothing maps 0.0.0.0), unlike a gratuitous ARP.
void arp_probe(uint32_t ip) {
    arp_header_t p;
    p.hw_type    = htons(ARP_HW_ETHERNET);
    p.proto_type = htons(ETH_TYPE_IPV4);
    p.hw_len     = 6;
    p.proto_len  = 4;
    p.operation  = htons(ARP_OP_REQUEST);
    eth_get_mac(p.sender_mac);
    memset(p.sender_ip, 0, 4);          // 0.0.0.0 = probe, NOT a claim
    memset(p.target_mac, 0, 6);
    uint32_t ip_be = htonl(ip);
    memcpy(p.target_ip, &ip_be, 4);
    static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    eth_send((uint8_t *)bcast, ETH_TYPE_ARP, &p, sizeof(p));
}

// Synchronous DAD for the static-IP boot path. Runs in top-level boot context
// (never inside eth_receive), so pumping RX here is safe. Sends 2 probes and
// polls the RX ring for a defender's reply. Returns 1 if the address is already
// in use, 0 if it appears free. Do NOT call from inside a receive callback.
//
// NOTE: this runs very early (right after net_init), where timer_ticks does not
// advance yet (the periodic tick is not driving here). Incoming ARP still lands
// in the NIC RX ring by DMA and eth_receive() polls it directly, so DAD does not
// need the timer - we bound each probe's listen window by a fixed poll budget.
// A defender on the LAN replies within microseconds (observed ~12us on-wire),
// so this budget has a large safety margin over the reply round-trip while
// keeping worst-case (address free) adoption to a few seconds, not minutes.
int arp_ip_in_use(uint32_t ip) {
    extern int eth_receive(void);
    if (ip == 0) return 0;
    arp_dad_arm(ip);
    for (int probe = 0; probe < 2 && !g_dad_conflict; probe++) {
        arp_probe(ip);
        // Poll the RX ring for a reply. ~1500 polls is well over a second here
        // (plenty to catch an instant defender) yet bounded so a free address
        // costs only a couple of seconds total.
        for (int guard = 0; guard < 1500 && !g_dad_conflict; guard++) {
            eth_receive();
            for (int j = 0; j < 200; j++) io_wait();
        }
    }
    int r = g_dad_conflict;
    arp_dad_disarm();
    return r;
}

// Send gratuitous ARP to keep neighbor caches fresh on the LAN
void arp_announce(void) {
    uint32_t our_ip = ip_get_address();
    if (!our_ip) return;

    arp_header_t ann;
    ann.hw_type    = htons(ARP_HW_ETHERNET);
    ann.proto_type = htons(ETH_TYPE_IPV4);
    ann.hw_len     = 6;
    ann.proto_len  = 4;
    ann.operation  = htons(ARP_OP_REQUEST);  // GARP uses REQUEST

    uint8_t our_mac[6];
    eth_get_mac(our_mac);
    memcpy(ann.sender_mac, our_mac, 6);
    uint32_t ip_be = htonl(our_ip);
    memcpy(ann.sender_ip, &ip_be, 4);
    memset(ann.target_mac, 0, 6);
    memcpy(ann.target_ip, &ip_be, 4);  // sender_ip == target_ip for GARP

    static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    eth_send((uint8_t *)bcast, ETH_TYPE_ARP, &ann, sizeof(ann));
}

// ---------------------------------------------------------------------------
// #404 / #495 Phase M: boot-time differential + perf + security self-test for
// the ARP parse. Proves arp_parse_rs == arp_parse_c on the live agreement domain
// (well-formed Ethernet/IPv4 ARP + the malformed cases the C already rejects:
// too-short, bad hw/proto type, bad hlen/plen) and reports the SECURITY posture
// HONESTLY: the C is already length-gated + fixed-offset + read-only, so there
// is NO reachable OOB; C and Rust reject/accept the malformed corpus IDENTICALLY
// (the [RUST-SEC] line shows zero divergence). The Rust win is the elimination
// of the unchecked-wire-pointer-arithmetic CLASS (defense-in-depth), not a live
// fix. One [RUST-DIFF] arp, one [RUST-PERF] arp, one [RUST-SEC] arp line to
// serial + /BOOTLOG. LIGHT: ~512 differential vectors + ~5k-iter RDTSC bench,
// bounded, runs once (#426, no busy-wait).
static uint32_t arpdiff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

// Serialized TSC read (cpuid fence + rdtsc), same pattern as the crypto/icmp ports.
static inline uint64_t arp_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

// Build a well-formed Ethernet/IPv4 ARP frame of total length `len` (>=28) into
// buf: the fixed 28-byte header (valid hw/proto type, hlen=6, plen=4, given op,
// random addresses) plus any (len-28) trailing padding bytes (ignored by parse).
static void arp_build_valid(uint8_t *buf, uint32_t len, uint16_t op, uint32_t *seed) {
    arp_header_t *h = (arp_header_t *)buf;
    h->hw_type    = htons(ARP_HW_ETHERNET);
    h->proto_type = htons(ETH_TYPE_IPV4);
    h->hw_len     = 6;
    h->proto_len  = 4;
    h->operation  = htons(op);
    for (int i = 0; i < 6; i++) h->sender_mac[i] = (uint8_t)(arpdiff_rng(seed) & 0xFF);
    for (int i = 0; i < 4; i++) h->sender_ip[i]  = (uint8_t)(arpdiff_rng(seed) & 0xFF);
    for (int i = 0; i < 6; i++) h->target_mac[i] = (uint8_t)(arpdiff_rng(seed) & 0xFF);
    for (int i = 0; i < 4; i++) h->target_ip[i]  = (uint8_t)(arpdiff_rng(seed) & 0xFF);
    for (uint32_t i = sizeof(arp_header_t); i < len; i++)
        buf[i] = (uint8_t)(arpdiff_rng(seed) & 0xFF);
}

// Compare two parse results field-by-field. Returns 0 if identical.
static int arp_parsed_eq(int rc_a, const arp_parsed_t *a, int rc_b, const arp_parsed_t *b) {
    if (rc_a != rc_b) return 1;
    if (rc_a != ARP_PARSE_OK) return 0;   // both rejected identically: fields N/A
    if (a->hw_type != b->hw_type) return 1;
    if (a->proto_type != b->proto_type) return 1;
    if (a->operation != b->operation) return 1;
    if (a->hw_len != b->hw_len) return 1;
    if (a->proto_len != b->proto_len) return 1;
    if (a->sender_ip != b->sender_ip) return 1;
    if (a->target_ip != b->target_ip) return 1;
    if (memcmp(a->sender_mac, b->sender_mac, 6) != 0) return 1;
    if (memcmp(a->sender_ip_be, b->sender_ip_be, 4) != 0) return 1;
    return 0;
}

void arp_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    static uint8_t buf[128];
    uint32_t seed = 0xa3c1f025;
    uint32_t vectors = 0, mismatches = 0;
    int first_bad = -1;

    // Force-reference the Rust symbol so its archive member is always linked.
    { arp_parsed_t t; arp_parse_rs(buf, 0, &t); }

    // Part 1: agreement domain. ~512 vectors mixing well-formed Ethernet/IPv4
    // ARP (request + reply, header-exact len 28 and padded len up to 64) with the
    // malformed cases the C already rejects (too-short 0..27, bad hw type, bad
    // proto type, bad hlen, bad plen). On ALL of these the Rust MUST match the C.
    for (uint32_t iter = 0; iter < 512; iter++) {
        arp_parsed_t rc_out, cc_out;
        int rrc, crc;
        uint32_t kind = arpdiff_rng(&seed) % 7;

        if (kind == 0) {
            // truncated: len in [0,27]
            uint32_t len = arpdiff_rng(&seed) % sizeof(arp_header_t);
            for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(arpdiff_rng(&seed) & 0xFF);
            crc = arp_parse_c(buf, len, &cc_out);
            rrc = arp_parse_rs(buf, len, &rc_out);
        } else {
            // valid base frame, len 28..64
            uint32_t len = sizeof(arp_header_t) + (arpdiff_rng(&seed) % 37);
            uint16_t op = (arpdiff_rng(&seed) & 1) ? ARP_OP_REQUEST : ARP_OP_REPLY;
            arp_build_valid(buf, len, op, &seed);
            arp_header_t *h = (arp_header_t *)buf;
            if (kind == 2) h->hw_type    = htons(0x1234);   // bad hw type
            else if (kind == 3) h->proto_type = htons(0x86DD); // bad proto (IPv6)
            else if (kind == 4) h->hw_len   = 8;            // bad hlen
            else if (kind == 5) h->proto_len = 16;          // bad plen
            else if (kind == 6) h->hw_type  = htons(0), h->proto_len = 0; // both bad
            crc = arp_parse_c(buf, len, &cc_out);
            rrc = arp_parse_rs(buf, len, &rc_out);
        }
        vectors++;
        if (arp_parsed_eq(crc, &cc_out, rrc, &rc_out)) {
            mismatches++;
            if (first_bad < 0) first_bad = (int)iter;
        }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] arp: %u vectors, %u mismatches -> %s\n", vectors, mismatches, verdict);
    bootlog_write("[RUST-DIFF] arp: %u vectors, %u mismatches -> %s", vectors, mismatches, verdict);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] arp FIRST MISMATCH iter=%d\n", first_bad);
        bootlog_write("[RUST-DIFF] arp FIRST MISMATCH iter=%d", first_bad);
    }

    // Part 2: SECURITY posture. Sweep the malformed corpus (every truncation
    // length 0..27, plus bad hw/proto/hlen/plen on a full frame) and count where
    // the C and Rust verdicts DIVERGE. Honest expectation: ZERO, because the C is
    // already length-gated (rejects <28 before any read) and reads only fixed
    // header offsets read-only, so no OOB is reachable. The Rust confines the same
    // accesses by construction (defense-in-depth); the line documents that this
    // port removes a CLASS, not a live bug (divergence == 0 proves no regression).
    {
        uint32_t sec_n = 0, divergences = 0;
        uint32_t s2 = 0x55aa33cc;
        for (uint32_t len = 0; len < sizeof(arp_header_t); len++) {
            for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(arpdiff_rng(&s2) & 0xFF);
            arp_parsed_t co, ro;
            int crc = arp_parse_c(buf, len, &co);
            int rrc = arp_parse_rs(buf, len, &ro);
            sec_n++;
            if (crc != rrc) divergences++;
        }
        // full-frame malformed field variants
        for (uint32_t v = 0; v < 4; v++) {
            arp_build_valid(buf, sizeof(arp_header_t), ARP_OP_REQUEST, &s2);
            arp_header_t *h = (arp_header_t *)buf;
            if (v == 0) h->hw_type = htons(0x1234);
            else if (v == 1) h->proto_type = htons(0x86DD);
            else if (v == 2) h->hw_len = 8;
            else h->proto_len = 16;
            arp_parsed_t co, ro;
            int crc = arp_parse_c(buf, sizeof(arp_header_t), &co);
            int rrc = arp_parse_rs(buf, sizeof(arp_header_t), &ro);
            sec_n++;
            if (crc != rrc) divergences++;
        }
        kprintf("[RUST-SEC] arp: C already length-gated+fixed-offset+read-only (no reachable OOB); "
                "%u/%u malformed verdicts identical, %u divergences; Rust confines the class by construction\n",
                sec_n - divergences, sec_n, divergences);
        bootlog_write("[RUST-SEC] arp: no reachable OOB (C length-gated); %u/%u malformed verdicts identical, %u divergences (class-elimination, latent-not-reachable)",
                      sec_n - divergences, sec_n, divergences);
    }

    // Part 3: RDTSC micro-benchmark over a fixed 28-byte valid frame. LIGHT: 5k
    // iters (boot cost stays low; the offline pre-flight does the big counts).
    {
        const int iters = 5000;
        arp_parsed_t o;
        uint32_t s3 = 0x2b4d6f81;
        arp_build_valid(buf, sizeof(arp_header_t), ARP_OP_REQUEST, &s3);

        for (int i = 0; i < 300; i++) { arp_parse_c(buf, sizeof(arp_header_t), &o); arp_parse_rs(buf, sizeof(arp_header_t), &o); }

        uint64_t t0 = arp_tsc_serialized();
        for (int i = 0; i < iters; i++) arp_parse_c(buf, sizeof(arp_header_t), &o);
        uint64_t t1 = arp_tsc_serialized();
        for (int i = 0; i < iters; i++) arp_parse_rs(buf, sizeof(arp_header_t), &o);
        uint64_t t2 = arp_tsc_serialized();

        uint64_t c_cyc = (t1 - t0) / iters;
        uint64_t r_cyc = (t2 - t1) / iters;
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        kprintf("[RUST-PERF] arp: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
        bootlog_write("[RUST-PERF] arp: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}
