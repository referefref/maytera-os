// icmp.c - Internet Control Message Protocol implementation
#include "icmp.h"
#include "ip.h"
#include "../serial.h"
#include "../string.h"
#include "../cpu/isr.h"

// Ping state
static uint16_t ping_id = 0x1234;
static uint16_t ping_seq = 0;
static int ping_pending = 0;
static uint32_t ping_reply_ip = 0;
static uint16_t ping_reply_seq = 0;
static uint64_t ping_send_time = 0;
static uint16_t ping_reply_time = 0;
static int ping_reply_received = 0;

// Byte swap
static inline uint16_t htons(uint16_t h) {
    return ((h & 0xFF) << 8) | ((h >> 8) & 0xFF);
}

static inline uint16_t ntohs(uint16_t n) {
    return htons(n);
}

static inline uint32_t ntohl(uint32_t n) {
    return ((n & 0xFF) << 24) | ((n & 0xFF00) << 8) |
           ((n >> 8) & 0xFF00) | ((n >> 24) & 0xFF);
}

// Initialize ICMP
void icmp_init(void) {
    ping_seq = 0;
    ping_pending = 0;
    ping_reply_received = 0;

    // Register with IP layer
    ip_register_handler(IP_PROTO_ICMP, icmp_handle);

    kprintf("[ICMP] ICMP initialized\n");
}

// Send ping
int icmp_ping(uint32_t dest_ip) {
    uint8_t *dp = (uint8_t *)&dest_ip;
    kprintf("[ICMP] icmp_ping called: dest_ip=0x%08x (%d.%d.%d.%d)\n",
            dest_ip, dp[3], dp[2], dp[1], dp[0]);

    // Build ICMP echo request
    uint8_t packet[64];
    icmp_header_t *header = (icmp_header_t *)packet;

    header->type = ICMP_ECHO_REQUEST;
    header->code = 0;
    header->checksum = 0;
    header->id = htons(ping_id);
    header->sequence = htons(ping_seq++);

    // Add some payload data
    const char *payload = "MayteraOS ping!";
    uint16_t payload_len = strlen(payload);
    memcpy(packet + sizeof(icmp_header_t), payload, payload_len);

    uint16_t total_len = sizeof(icmp_header_t) + payload_len;

    // Calculate checksum
    header->checksum = ip_checksum(packet, total_len);

    // Record send time
    ping_send_time = timer_ticks;
    ping_pending = 1;
    ping_reply_received = 0;

    // Send via IP
    return ip_send(dest_ip, IP_PROTO_ICMP, packet, total_len);
}

// ---------------------------------------------------------------------------
// #404 / #494 Phase L: pure ICMP parse/validate seam (Tier 2, untrusted wire
// input). Extracted VERBATIM out of the old icmp_handle() so the C reference
// preserves the exact current behavior INCLUDING its two weaknesses, which the
// Rust port (icmp_parse_rs) removes by construction:
//
//   1. NO UPPER BOUND on the attacker-controlled length. The C only checks
//      len >= 8; a large len flows unchecked into (a) ip_checksum(buf, len)
//      (the checksum span) and (b) the downstream `uint8_t reply[length]` VLA
//      in icmp_handle (a kernel-stack allocation of up to len bytes -> a
//      stack-overflow class if len is ever large; live it is MTU-bounded by
//      ip.c's total_length<=length guard, so this is a latent/defense-in-depth
//      confinement, not a currently-reachable OOB). The Rust rejects len >
//      ICMP_MAX_LEN before any read.
//   2. IN-PLACE MUTATION of the untrusted, `const`-declared RX buffer: to
//      verify the checksum the C zeroes header->checksum (casting away const),
//      recomputes, then restores it. On a shared/DMA RX buffer that is a
//      transient corruption / aliasing hazard. The Rust computes the SAME
//      verdict over an immutable slice with zero writes.
//
// icmp_parse_c is byte-for-byte the old logic; keep it as the reference and the
// rollback (undefine RUST_ICMP -> icmp_parse() falls straight back to it).
//
// Static-assert the FFI struct layout so the #[repr(C)] IcmpParsed in
// rustkern.rs can never silently drift from icmp_parsed_t.
_Static_assert(sizeof(icmp_parsed_t) == 16, "icmp_parsed_t must be 16 bytes for the Rust FFI");

int icmp_parse_c(const uint8_t *buf, uint32_t len, icmp_parsed_t *out) {
    // Guard 1 (verbatim `if (length < sizeof(icmp_header_t)) return;`).
    if (len < sizeof(icmp_header_t)) {
        return ICMP_PARSE_ETOOSHORT;
    }
    // NOTE: verbatim - the current logic has NO upper bound here.

    // Verify checksum exactly as the old icmp_handle did: cast away const, zero
    // the field, recompute over `len`, restore. This is the in-place mutation
    // the Rust removes.
    icmp_header_t *header = (icmp_header_t *)buf;
    uint16_t original_checksum = header->checksum;
    header->checksum = 0;
    uint16_t computed = ip_checksum(buf, (uint16_t)len);
    header->checksum = original_checksum;

    if (out) {
        out->type        = header->type;
        out->code        = header->code;
        out->checksum    = original_checksum;
        out->id          = header->id;
        out->sequence    = header->sequence;
        out->total_len   = (uint16_t)len;
        out->payload_off = (uint16_t)sizeof(icmp_header_t);
        out->payload_len = (uint16_t)(len - sizeof(icmp_header_t));
        out->checksum_ok = (computed == original_checksum) ? 1 : 0;
    }
    return ICMP_PARSE_OK;
}

// Live dispatcher. With -DRUST_ICMP (set in the Makefile) the incoming ICMP
// parse runs in Rust; drop the flag + rebuild to roll straight back to C.
int icmp_parse(const uint8_t *buf, uint32_t len, icmp_parsed_t *out) {
#ifdef RUST_ICMP
    return icmp_parse_rs(buf, len, out);
#else
    return icmp_parse_c(buf, len, out);
#endif
}

// Handle incoming ICMP packet. The untrusted PARSE/validate is now icmp_parse()
// (Rust under -DRUST_ICMP); only the reply-send / echo-reply bookkeeping stays
// here in C.
void icmp_handle(uint32_t src_ip, const void *data, uint16_t length) {
    extern void serial_puts(uint16_t port, const char *str);
    serial_puts(0x3F8, "[ICMP] icmp_handle called!\r\n");

    icmp_parsed_t p;
    int rc = icmp_parse((const uint8_t *)data, length, &p);
    if (rc != ICMP_PARSE_OK) {
        if (rc == ICMP_PARSE_ETOOSHORT) {
            serial_puts(0x3F8, "[ICMP] REJECT: too short\r\n");
        } else {
            serial_puts(0x3F8, "[ICMP] REJECT: oversize\r\n");
        }
        return;
    }

    kprintf("[ICMP] type=%d code=%d len=%d\n", p.type, p.code, length);

    if (!p.checksum_ok) {
        kprintf("[ICMP] REJECT: bad checksum (recv 0x%04x)\n", p.checksum);
        return;
    }

    switch (p.type) {
        case ICMP_ECHO_REQUEST: {
            serial_puts(0x3F8, "[ICMP] Got ECHO REQUEST - sending reply!\r\n");
            // Send echo reply. `length` is MTU-bounded by ip.c (see ICMP_MAX_LEN);
            // the parse already rejected anything over the bound before we get here.
            uint8_t reply[length];
            memcpy(reply, data, length);
            icmp_header_t *reply_header = (icmp_header_t *)reply;
            reply_header->type = ICMP_ECHO_REPLY;
            reply_header->checksum = 0;
            reply_header->checksum = ip_checksum(reply, length);

            // src_ip is in network byte order from packet, convert to host order for ip_send
            uint32_t dest_host = ((src_ip & 0xFF) << 24) | ((src_ip & 0xFF00) << 8) |
                                 ((src_ip >> 8) & 0xFF00) | ((src_ip >> 24) & 0xFF);
            int result = ip_send(dest_host, IP_PROTO_ICMP, reply, length);
            kprintf("[ICMP] ip_send returned %d\n", result);
            break;
        }

        case ICMP_ECHO_REPLY: {
            serial_puts(0x3F8, "[ICMP] Got ECHO REPLY\r\n");
            // Check if this is reply to our ping
            if (ntohs(p.id) == ping_id && ping_pending) {
                // Convert src_ip from network byte order to host byte order
                ping_reply_ip = ntohl(src_ip);
                ping_reply_seq = ntohs(p.sequence);
                ping_reply_time = (uint16_t)(timer_ticks - ping_send_time) * 10;  // Approximate ms
                ping_reply_received = 1;
                ping_pending = 0;

                uint8_t *pp = (uint8_t *)&ping_reply_ip;
                kprintf("[ICMP] Reply from %d.%d.%d.%d: seq=%d time=%dms\n",
                        pp[3], pp[2], pp[1], pp[0], ping_reply_seq, ping_reply_time);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Boot-time differential + perf + security self-test for the ICMP parse.
// Proves icmp_parse_rs == icmp_parse_c on the LIVE agreement domain (valid +
// short + bad-checksum, len in [0,ICMP_MAX_LEN]) and reports the SECURITY WIN
// separately: on crafted OVERSIZE input the C reference accepts (unbounded),
// the Rust confines. One [RUST-DIFF] icmp, one [RUST-PERF] icmp, one [RUST-SEC]
// icmp line to serial + /BOOTLOG. Bounded, runs once (#426, no busy-wait).
static uint32_t icmpdiff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

// Serialized TSC read (cpuid fence + rdtsc), same pattern as the crypto ports.
static inline uint64_t icmp_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

// Build a well-formed ICMP echo packet of total length `len` (>=8) into buf with
// a correct checksum. Fills the 8-byte header + (len-8) pseudo-random payload.
static void icmp_build_valid(uint8_t *buf, uint32_t len, uint8_t type, uint32_t *seed) {
    icmp_header_t *h = (icmp_header_t *)buf;
    h->type = type;
    h->code = 0;
    h->checksum = 0;
    h->id = (uint16_t)(icmpdiff_rng(seed) & 0xFFFF);
    h->sequence = (uint16_t)(icmpdiff_rng(seed) & 0xFFFF);
    for (uint32_t i = sizeof(icmp_header_t); i < len; i++) {
        buf[i] = (uint8_t)(icmpdiff_rng(seed) & 0xFF);
    }
    h->checksum = ip_checksum(buf, (uint16_t)len);
}

// Build a packet in the ONES-COMPLEMENT NEGATIVE ZERO state: the sum with the
// checksum field zeroed folds to S == 0xFFFF, and the field itself is 0xFFFF.
//
// #404 drift audit 2 (2026-07-16): this generator exists because
// icmp_build_valid is STRUCTURALLY BLIND. It always writes
// `h->checksum = ip_checksum(buf)`, i.e. field = ~S by construction, so
// field == 0xFFFF would force S == 0x0000 and the S == 0xFFFF && field == 0xFFFF
// state is ALGEBRAICALLY UNREACHABLE for it. That state is the only one where
// the C's verdict (~S == field) and RFC 1071's (fold(S+field) == 0xFFFF)
// disagree, and it is exactly where icmp_parse_rs used to diverge from the C.
// The shipped "[RUST-DIFF] icmp PASS" was therefore true and worthless: it
// reported 0 mismatches over a corpus that could not express the only
// disagreement. Same failure mode as the dhcp finding in audit 1.
//
// A generator that cannot reach the divergent state cannot validate anything
// about it. This one can, so a future regression in either implementation
// FAILS the boot self-test instead of sailing past it.
static void icmp_build_negzero(uint8_t *buf, uint32_t len, uint8_t type, uint32_t *seed) {
    icmp_header_t *h = (icmp_header_t *)buf;
    h->type = type;
    h->code = 0;
    h->checksum = 0;
    h->id = (uint16_t)(icmpdiff_rng(seed) & 0xFFFF);
    h->sequence = (uint16_t)(icmpdiff_rng(seed) & 0xFFFF);
    for (uint32_t i = sizeof(icmp_header_t); i < len; i++) {
        buf[i] = (uint8_t)(icmpdiff_rng(seed) & 0xFF);
    }
    // Drive S to 0xFFFF. ip_checksum returns ~S, and ones-complement sums are
    // associative, so folding the current ~S back into the id word shifts S by
    // exactly that amount and lands S on 0xFFFF in one step:
    //   want  S' == 0xFFFF == 0 (mod 65535)
    //   S' = S - id0 + id1  =>  id1 = id0 - S = id0 + ~S = id0 + c0
    uint16_t c0 = ip_checksum(buf, (uint16_t)len);
    uint32_t t = (uint32_t)h->id + (uint32_t)c0;
    while (t >> 16) t = (t & 0xFFFF) + (t >> 16);
    h->id = (uint16_t)t;
    h->checksum = 0xFFFF;   // negative zero: C rejects this, RFC 1071 accepts it
}

// Compare two parse results field-by-field. Returns 0 if identical.
static int icmp_parsed_eq(int rc_a, const icmp_parsed_t *a, int rc_b, const icmp_parsed_t *b) {
    if (rc_a != rc_b) return 1;
    if (rc_a != ICMP_PARSE_OK) return 0;   // both rejected identically: fields N/A
    if (a->type != b->type) return 1;
    if (a->code != b->code) return 1;
    if (a->checksum != b->checksum) return 1;
    if (a->id != b->id) return 1;
    if (a->sequence != b->sequence) return 1;
    if (a->total_len != b->total_len) return 1;
    if (a->payload_off != b->payload_off) return 1;
    if (a->payload_len != b->payload_len) return 1;
    if (a->checksum_ok != b->checksum_ok) return 1;
    return 0;
}

void icmp_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    // 8 KiB scratch arena: big enough that the oversize SEC probe's (deliberately
    // over-claimed) length stays inside OUR allocation, so demonstrating the C's
    // unbounded read can never fault the boot. The real packet is much smaller.
    static uint8_t buf[8192];
    uint32_t seed = 0x1c3d5e7f;
    uint32_t vectors = 0, mismatches = 0;
    uint32_t negzero_vectors = 0, negzero_reached = 0;
    int first_bad_len = -1;

    // Force-reference the Rust symbol so its archive member is always linked.
    { icmp_parsed_t t; icmp_parse_rs(buf, 0, &t); }

    // Part 1: agreement domain (valid echo request + reply, every length
    // 8..ICMP_MAX_LEN), plus the malformed cases the C already handles: too
    // short (0..7), header-straddling (exactly 8), and bad checksum (one byte
    // flipped). On ALL of these the Rust MUST match the C bit-for-bit.
    for (uint32_t len = 0; len <= ICMP_MAX_LEN; len++) {
        icmp_parsed_t rc_out, cc_out;
        int rrc, crc;

        if (len >= sizeof(icmp_header_t)) {
            uint8_t type = (len & 1) ? ICMP_ECHO_REQUEST : ICMP_ECHO_REPLY;
            icmp_build_valid(buf, len, type, &seed);
        } else {
            for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(icmpdiff_rng(&seed) & 0xFF);
        }
        // C first (it mutates+restores the checksum field), then Rust on the
        // restored buffer.
        crc = icmp_parse_c(buf, len, &cc_out);
        rrc = icmp_parse_rs(buf, len, &rc_out);
        vectors++;
        if (icmp_parsed_eq(crc, &cc_out, rrc, &rc_out)) {
            mismatches++;
            if (first_bad_len < 0) first_bad_len = (int)len;
        }

        // Bad-checksum variant for the valid lengths: flip a payload byte so the
        // checksum no longer verifies; both must accept structurally (rc==OK)
        // and both must report checksum_ok==0.
        if (len >= sizeof(icmp_header_t)) {
            buf[len - 1] ^= 0xFF;
            crc = icmp_parse_c(buf, len, &cc_out);
            rrc = icmp_parse_rs(buf, len, &rc_out);
            vectors++;
            if (icmp_parsed_eq(crc, &cc_out, rrc, &rc_out) ||
                cc_out.checksum_ok != 0 || rc_out.checksum_ok != 0) {
                mismatches++;
                if (first_bad_len < 0) first_bad_len = (int)len;
            }
        }

        // ONES-COMPLEMENT NEGATIVE ZERO (#404 drift audit 2). icmp_build_valid
        // cannot express this state (it always writes field = ~S), so before
        // this arm existed the self-test reported PASS over a corpus blind to
        // the ONLY input class where the C and RFC 1071 disagree. Both arms MUST
        // now agree that checksum_ok == 0 (the C's verdict; see
        // icmp_ones_complement_zeroed in rustkern.rs).
        if (len >= sizeof(icmp_header_t)) {
            uint8_t type = (len & 1) ? ICMP_ECHO_REQUEST : ICMP_ECHO_REPLY;
            icmp_build_negzero(buf, len, type, &seed);
            crc = icmp_parse_c(buf, len, &cc_out);
            rrc = icmp_parse_rs(buf, len, &rc_out);
            vectors++;
            negzero_vectors++;
            if (icmp_parsed_eq(crc, &cc_out, rrc, &rc_out)) {
                mismatches++;
                if (first_bad_len < 0) first_bad_len = (int)len;
            }
            // Corpus-sensitivity assertion: if this ever stops holding, the
            // builder has drifted and stopped reaching the divergent state, so
            // the arm would be silently vacuous again.
            if (cc_out.checksum == 0xFFFF && cc_out.checksum_ok == 0) negzero_reached++;
        }
    }

    // A PASS is only meaningful if the corpus actually REACHED the divergent
    // state, so report the coverage next to the verdict rather than leaving
    // "0 mismatches" to speak for itself (that is precisely what made the old
    // PASS worthless). negzero_reached == 0 means the arm went vacuous.
    const char *verdict = (mismatches == 0 && negzero_reached > 0) ? "PASS"
                        : (mismatches == 0) ? "PASS(BLIND: negzero unreached)"
                        : "FAIL";
    kprintf("[RUST-DIFF] icmp: %u vectors, %u mismatches -> %s (negzero %u/%u reached)\n",
            vectors, mismatches, verdict, negzero_reached, negzero_vectors);
    bootlog_write("[RUST-DIFF] icmp: %u vectors, %u mismatches -> %s (negzero %u/%u reached)",
                  vectors, mismatches, verdict, negzero_reached, negzero_vectors);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] icmp FIRST MISMATCH len=%d\n", first_bad_len);
        bootlog_write("[RUST-DIFF] icmp FIRST MISMATCH len=%d", first_bad_len);
    }

    // Part 2: SECURITY probe. Craft a small (32-byte) valid packet but claim an
    // OVERSIZE length in (ICMP_MAX_LEN, 8192]. The verbatim C reference has no
    // upper bound: it ACCEPTS and reads/sums all `len` bytes (past the 32-byte
    // packet - an out-of-bounds read on any real MTU-sized RX buffer; here it
    // stays inside our 8 KiB arena so it cannot fault the boot) and would drive
    // a uint8_t reply[len] VLA of up to `len` bytes on the kernel stack. The
    // Rust rejects (EOVERSIZE) BEFORE dereferencing. Count the confinement.
    {
        uint32_t sec_n = 0, c_accepted = 0, rs_confined = 0;
        icmp_build_valid(buf, 32, ICMP_ECHO_REQUEST, &seed);   // a real 32-byte packet
        for (uint32_t len = ICMP_MAX_LEN + 1; len <= 8192; len += 137) {
            icmp_parsed_t co, ro;
            int crc = icmp_parse_c(buf, len, &co);   // no upper bound -> accepts, reads len bytes
            int rrc = icmp_parse_rs(buf, len, &ro);  // confined -> EOVERSIZE
            sec_n++;
            if (crc == ICMP_PARSE_OK) c_accepted++;
            if (rrc == ICMP_PARSE_EOVERSIZE) rs_confined++;
        }
        kprintf("[RUST-SEC] icmp: C accepts %u/%u oversize (unbounded len -> checksum span + reply VLA), Rust confined %u/%u\n",
                c_accepted, sec_n, rs_confined, sec_n);
        bootlog_write("[RUST-SEC] icmp: C accepts %u/%u oversize (unbounded), Rust confined %u/%u",
                      c_accepted, sec_n, rs_confined, sec_n);
    }

    // Part 3: RDTSC micro-benchmark over a fixed 64-byte valid packet.
    {
        const int iters = 100000;
        icmp_parsed_t o;
        uint32_t s2 = 0x2b4d6f81;
        icmp_build_valid(buf, 64, ICMP_ECHO_REQUEST, &s2);

        for (int i = 0; i < 500; i++) { icmp_parse_c(buf, 64, &o); icmp_parse_rs(buf, 64, &o); }

        uint64_t t0 = icmp_tsc_serialized();
        for (int i = 0; i < iters; i++) icmp_parse_c(buf, 64, &o);
        uint64_t t1 = icmp_tsc_serialized();
        for (int i = 0; i < iters; i++) icmp_parse_rs(buf, 64, &o);
        uint64_t t2 = icmp_tsc_serialized();

        uint64_t c_cyc = (t1 - t0) / iters;
        uint64_t r_cyc = (t2 - t1) / iters;
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        kprintf("[RUST-PERF] icmp: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
        bootlog_write("[RUST-PERF] icmp: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}

// Get ping statistics
int icmp_get_ping_reply(uint32_t *src_ip, uint16_t *seq, uint16_t *time_ms) {
    if (!ping_reply_received) {
        return 0;
    }

    if (src_ip) *src_ip = ping_reply_ip;
    if (seq) *seq = ping_reply_seq;
    if (time_ms) *time_ms = ping_reply_time;

    ping_reply_received = 0;
    return 1;
}
