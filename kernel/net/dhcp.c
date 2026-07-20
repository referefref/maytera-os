// dhcp.c - Dynamic Host Configuration Protocol client
#include "dhcp.h"
#include "udp.h"
#include "ip.h"
#include "ethernet.h"
#include "arp.h"
#include "../serial.h"
#include "../string.h"
#include "../cpu/isr.h"
#include "../sync/spinlock.h"   // #522: atomic_cas64() for the timeout claim

extern uint32_t g_timer_hz;

// #380: internal state entered after a DHCP ACK, while we run RFC 5227
// duplicate-address detection on the offered address BEFORE committing to it.
#define DHCP_STATE_DAD 4

// DHCP state
static int dhcp_state = DHCP_STATE_IDLE;
static uint32_t dhcp_xid = 0x12345678;

// #520: DHCP tracing, DEFAULT OFF (same precedent as g_tcp_dbg, #225). These
// traces are diagnostic only and MUST stay off in shipping builds: kprintf is
// serial-bound and blocks for milliseconds, which is enough to re-open the
// check-then-act window in dhcp_poll's timeout block (see the #520 note there).
// Flip to 1 only for a local DHCP debug session, never in a fold to golden.
int g_dhcp_dbg = 0;

// ---------------------------------------------------------------------------
// #522: LOCK-FREE DHCP EVENT RING. Always on, costs ~20ns, NEVER prints.
//
// kprintf is NOT a passive observer on this path, and that is not a theory:
//   1) A [DHCPDIAG] kprintf placed between the expiry check and the claim
//      re-opened the very race #519 had closed (#520). The instrument created
//      the bug it was measuring.
//   2) The serial log SILENTLY DROPS AND INTERLEAVES lines when several threads
//      print at once (observed: `oth ENABLE[FSD (de] Crfaulteated[DHCPDIAG] TO
//      ticks=7413` = three messages shredded into one). "[DHCP] Starting
//      discovery" had ZERO occurrences in a 296KB capture while that exact
//      function demonstrably ran, proven by its randomized xid reaching the wire.
//      So serial line-ABSENCE is not evidence of anything.
// Therefore concurrency evidence on this path must be recorded, not printed:
// a fixed-size ring, one atomic increment per event, no lock, no serial, no
// allocation. Dump it out-of-band AFTER the fact (dhcp_trace_dump), never from
// inside the window under study.
#define DHCP_TRC_N 64
typedef struct {
    uint64_t ticks;      // timer_ticks at the event
    uint32_t ev;         // DHCP_TRC_* below
    uint32_t arg;        // event-specific (state, retries, ...)
} dhcp_trc_t;
#define DHCP_TRC_POLL_EXPIRED 1   // a context saw now > deadline
#define DHCP_TRC_CLAIM_WON    2   // ...and won the CAS (owns this timeout)
#define DHCP_TRC_CLAIM_LOST   3   // ...and lost the CAS (someone else owns it)
#define DHCP_TRC_SEND         4   // a DISCOVER/REQUEST actually hit udp_send
#define DHCP_TRC_GIVEUP       5   // retries exhausted -> IDLE
#define DHCP_TRC_RX           6   // dhcp_handle entered (arg = msg_type)
static dhcp_trc_t       dhcp_trc[DHCP_TRC_N];
static volatile uint32_t dhcp_trc_seq = 0;

static inline void dhcp_trace(uint32_t ev, uint32_t arg) {
    uint32_t i = __sync_fetch_and_add(&dhcp_trc_seq, 1);
    dhcp_trc_t *s = &dhcp_trc[i & (DHCP_TRC_N - 1)];
    s->ticks = timer_ticks;
    s->ev    = ev;
    s->arg   = arg;
}

// Dump the ring. Call from top-level context ONLY (it kprintfs), never from
// dhcp_poll/dhcp_handle. Proves, after the fact, how many contexts hit one
// expiry and which single one won it.
void dhcp_trace_dump(void) {
    uint32_t n = dhcp_trc_seq;
    uint32_t start = (n > DHCP_TRC_N) ? (n - DHCP_TRC_N) : 0;
    kprintf("[DHCPTRC] %u events (showing last %u)\n", n, n - start);
    for (uint32_t k = start; k < n; k++) {
        dhcp_trc_t *s = &dhcp_trc[k & (DHCP_TRC_N - 1)];
        kprintf("[DHCPTRC] #%u t=%u ev=%u arg=%u\n",
                k, (unsigned)s->ticks, s->ev, s->arg);
    }
}
static uint32_t dhcp_offered_ip = 0;
static uint32_t dhcp_server_ip = 0;
static uint32_t dhcp_gateway = 0;
static uint32_t dhcp_netmask = 0;
static uint32_t dhcp_dns = 0;
// #524: BURST GUARD. timer_ticks IS NOT A WALL CLOCK ON A VM.
//
// PROVEN, not theorised. Under KVM the PIT default lost-tick policy is to REPLAY
// missed ticks: when the vCPU stalls (busy host, USB-MSC boot I/O) the accumulated
// ticks are reinjected in a burst, so timer_ticks can leap ~1250 (a nominal FIVE
// SECONDS at 250Hz) in ~15ms of real time, while the long-run average stays a
// perfectly innocent 250Hz. Every `timer_ticks + N` deadline in the kernel is
// therefore blown "instantly" in wall-clock terms.
//
// That single fact reconciles everything that looked contradictory here:
//   - the serial trace showed each timeout firing EXACTLY at its deadline (a
//     correct 5s in tick terms: `ticks=7413 to=7408`), yet
//   - tcpdump showed the 3 DISCOVERs 16-42ms apart in REAL time, and
//   - the #522 CAS did not change the wire at all, because there is no race to
//     win: each timeout is LEGITIMATE per the counter. The counter is the liar.
//   - a 300s (75000-tick) deadline survived, because a burst is ~1250 ticks and
//     cannot leap 75000.
// Proof with ZERO kernel changes: booting the same kernel with
// `-global kvm-pit.lost_tick_policy=discard` makes the retry spacing snap from
// 19ms to a correct 5.07s and DORA completes (DISCOVER/OFFER/REQUEST/ACK, lease
// <LAB_HOST> bound and used). The hypervisor flag is a DIAGNOSTIC, not our fix:
// it costs guest clock accuracy under load, and MayteraOS must not depend on the
// hypervisor being configured a particular way.
//
// The guard: a retry requires real POLL PROGRESS as well as an expired tick
// deadline. dhcp_poll() is driven at >=250Hz from several live contexts, so a
// burst that fabricates 5 seconds of ticks in 15ms cannot also fabricate hundreds
// of poll calls. This is the same belt-and-braces already used in this file for
// DAD (dhcp_dad_calls), not a new primitive.
#define DHCP_MIN_POLLS_PER_RETRY 200
static volatile uint32_t dhcp_poll_calls = 0;   // bumped once per dhcp_poll()
static volatile uint32_t dhcp_last_send_call = 0;

// #522: volatile + only ever mutated through atomic_cas64() in dhcp_poll's expiry
// path (sync/spinlock.h, the same primitive cpu/smp.c's BKL uses). See the long
// note in dhcp_poll(). The arming sites (dhcp_discover/dhcp_handle) are plain
// stores on purpose: they run when we OWN the transaction and are publishing a
// fresh, far-future deadline, not racing to consume an expired one.
static volatile uint64_t dhcp_timeout = 0;
static int dhcp_retries = 0;

// #519: the retransmit deadline in TICKS, derived from the live tick rate.
//
// Every site here used a hardcoded `timer_ticks + 500` commented "5 second
// timeout". g_timer_hz is 250 (cpu/pic.c), so 500 ticks is really 2 SECONDS, not
// 5 - the same tick-rate-vs-milliseconds confusion as #420 (wget/https 18.2Hz vs
// 250Hz). The DAD path in this very file already did the right thing
// (`hz = g_timer_hz ? g_timer_hz : 100`), so the file disagreed with itself.
// Derive it once, honour the documented 5s, and never hardcode a tick count.
static inline uint64_t dhcp_timeout_ticks(void) {
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    return (uint64_t)hz * 5;   // a real 5 seconds, at whatever rate we tick
}
// #380 DAD-before-bind bookkeeping.
static uint64_t dhcp_dad_deadline = 0;
static int      dhcp_dad_probes = 0;
// Fallback progress counter: the boot blocking loop distrusts timer_ticks, so
// DAD also advances after a bounded number of dhcp_poll() calls, guaranteeing
// it can never hang the DHCP path even if the timer is not advancing.
static int      dhcp_dad_calls = 0;

// DHCP magic cookie (99.130.83.99 in network byte order)
#define DHCP_MAGIC  0x63825363

// Byte swap
static inline uint16_t htons16(uint16_t h) {
    return ((h & 0xFF) << 8) | ((h >> 8) & 0xFF);
}

static inline uint32_t htonl(uint32_t h) {
    return ((h & 0xFF) << 24) | ((h & 0xFF00) << 8) |
           ((h >> 8) & 0xFF00) | ((h >> 24) & 0xFF);
}

// ntohl is the same as htonl on little-endian
static inline uint32_t ntohl(uint32_t n) {
    return htonl(n);
}

// Build DHCP discover/request packet
static int dhcp_build_packet(uint8_t *buffer, int msg_type) {
    dhcp_packet_t *pkt = (dhcp_packet_t *)buffer;
    memset(pkt, 0, sizeof(dhcp_packet_t));

    // BOOTP header
    pkt->op = 1;        // Request
    pkt->htype = 1;     // Ethernet
    pkt->hlen = 6;      // MAC address length
    pkt->hops = 0;
    pkt->xid = htonl(dhcp_xid);
    pkt->secs = 0;
    pkt->flags = htons16(0x8000);  // Broadcast flag

    // Get our MAC address
    uint8_t mac[6];
    eth_get_mac(mac);
    memcpy(pkt->chaddr, mac, 6);

    // Magic cookie: network byte order on the wire (63 82 53 63). Writing
    // the host-order constant directly put a byte-swapped cookie on the wire
    // (#362 fix; see dhcp_handle for the matching receive-side fix).
    pkt->magic = htonl(DHCP_MAGIC);

    // Build options
    uint8_t *opt = pkt->options;
    int opt_len = 0;

    // Message type
    opt[opt_len++] = DHCP_OPT_MSG_TYPE;
    opt[opt_len++] = 1;
    opt[opt_len++] = msg_type;

    // If requesting (or declining), include requested IP and server ID.
    // #380: a DHCP DECLINE (RFC 2131 4.4.4) MUST carry the declined address in
    // the Requested IP option and the Server Identifier of the offering server.
    if ((msg_type == DHCP_REQUEST || msg_type == DHCP_DECLINE) && dhcp_offered_ip != 0) {
        // Requested IP - convert back to network byte order for packet
        opt[opt_len++] = DHCP_OPT_REQUESTED_IP;
        opt[opt_len++] = 4;
        uint32_t requested_ip_net = htonl(dhcp_offered_ip);
        memcpy(&opt[opt_len], &requested_ip_net, 4);
        opt_len += 4;

        // Server ID - convert back to network byte order for packet
        opt[opt_len++] = DHCP_OPT_SERVER_ID;
        opt[opt_len++] = 4;
        uint32_t server_ip_net = htonl(dhcp_server_ip);
        memcpy(&opt[opt_len], &server_ip_net, 4);
        opt_len += 4;
    }

    // Parameter request list
    opt[opt_len++] = DHCP_OPT_PARAM_LIST;
    opt[opt_len++] = 3;
    opt[opt_len++] = DHCP_OPT_SUBNET_MASK;
    opt[opt_len++] = DHCP_OPT_ROUTER;
    opt[opt_len++] = DHCP_OPT_DNS;

    // End
    opt[opt_len++] = DHCP_OPT_END;

    return sizeof(dhcp_packet_t) - 308 + opt_len;
}

// ---------------------------------------------------------------------------
// #404 / #497 Phase O: pure DHCP reply-parse seam (Tier 2, untrusted wire input).
// Extracted out of the old dhcp_handle()/dhcp_parse_options() so the C reference
// preserves the EXACT current behavior INCLUDING the option-walk's central
// weakness, which the Rust port (dhcp_parse_rs) removes by construction:
//
//   THE OPTION WALK IGNORES THE RECEIVED LENGTH. dhcp_handle only length-gates
//   the fixed BOOTP header (length >= sizeof(dhcp_packet_t) - 308 == 240). The
//   walk below then runs over the FIXED 308-byte options[] field, driven only by
//   the attacker-supplied option LENGTH bytes, WITHOUT ever consulting `len`:
//     - `opt[i+1]` (the length byte) is read with only `i < 308` proven, so at
//       i==307 it reads opt[308], one byte PAST the options[] array (past the
//       dhcp_packet_t struct).
//     - a 4-byte option value is read (`memcpy(&tmp,&opt[i+2],4)`) after checking
//       only `len >= 4`, NOT `i+2+4 <= 308`, so near the tail it reads up to 4
//       bytes past options[].
//     - `i += 2 + len` advances by the attacker length; the next `i < 308` catches
//       it, but the current iteration's reads already ran.
//   Net effect: on a runt / non-END-terminated / lying-length OFFER (all
//   attacker-spoofable on the LAN), the parse reads BEYOND the received packet
//   bytes - a REACHABLE over-read. Live it usually stays inside the >=1514-byte
//   NIC RX buffer (so it reads STALE / adjacent bytes and can derive netmask/
//   gateway/dns/server-id from non-packet memory rather than fault), but with a
//   tightly-sized buffer the tail reads past the allocation (offline ASan proves
//   this). This is STRONGER than the arp/dns latent cases (which were fully
//   length-gated) and weaker than the ext2 #476 guaranteed heap crash.
//
// dhcp_parse_rs (rustkern.rs) slices exactly `len` bytes, walks options within
// [240, min(len, 548)) == THIS function's own fixed 308-byte window intersected
// with the received bytes, and bounds-checks every read against `len` so a length
// running past the packet TERMINATES the walk instead of reading past it.
//
// The Rust seam is a CONFINEMENT of this function in BOTH directions: it never
// reads a byte this C did not read, and it never honors an option this C ignored.
// The second half is easy to get wrong and DID ship wrong in b806: because this
// C's window is FIXED at 308 bytes and does NOT grow with `len`, a port bounded
// only by `[240, len)` HONORS options past offset 548 that this C skips entirely,
// which reads MORE, not less. Fixed 2026-07-16 by the #404 3-way drift audit; the
// boot self-test now emits >548-byte vectors so the class cannot hide again.
//
// Static-assert the FFI struct layout so the #[repr(C)] DhcpParsed in
// rustkern.rs can never silently drift from dhcp_parsed_t.
_Static_assert(sizeof(dhcp_parsed_t) == 36, "dhcp_parsed_t must be 36 bytes for the Rust FFI");

int dhcp_parse_c(const uint8_t *buf, uint32_t len, dhcp_parsed_t *out) {
    // Header gate (verbatim `if (length < sizeof(dhcp_packet_t) - 308) return;`).
    if (len < (uint32_t)(sizeof(dhcp_packet_t) - 308)) {
        return DHCP_PARSE_ETOOSHORT;
    }
    const dhcp_packet_t *pkt = (const dhcp_packet_t *)buf;
    // Verbatim `if (pkt->op != 2) return;`.
    if (pkt->op != 2) {
        return DHCP_PARSE_ENOTREPLY;
    }
    // Verbatim `if (pkt->magic != htonl(DHCP_MAGIC)) return;` (network order wire).
    if (pkt->magic != htonl(DHCP_MAGIC)) {
        return DHCP_PARSE_EBADMAGIC;
    }

    uint8_t  msg_type = 0;
    uint32_t subnet = 0, router = 0, dns = 0, lease = 0, server_id = 0;
    uint8_t  hsub = 0, hrtr = 0, hdns = 0, hlease = 0, hsid = 0;

    // VERBATIM option walk (old dhcp_parse_options): FIXED 308-byte window that
    // does NOT consult `len` - this is the reachable over-read the Rust confines.
    // The lease (option 51) case is new (the old walk did not extract it) but is
    // structurally identical to the other 4-byte options and does not change the
    // bounds behavior; it is parsed identically by both the C ref and the Rust.
    const uint8_t *opt = pkt->options;
    int i = 0;
    while (i < 308 && opt[i] != DHCP_OPT_END) {
        if (opt[i] == DHCP_OPT_PAD) {
            i++;
            continue;
        }
        uint8_t type = opt[i];
        uint8_t olen = opt[i + 1];
        switch (type) {
            case DHCP_OPT_MSG_TYPE:
                if (olen >= 1) msg_type = opt[i + 2];
                break;
            case DHCP_OPT_SUBNET_MASK:
                if (olen >= 4) { uint32_t t; memcpy(&t, &opt[i + 2], 4); subnet    = ntohl(t); hsub = 1; }
                break;
            case DHCP_OPT_ROUTER:
                if (olen >= 4) { uint32_t t; memcpy(&t, &opt[i + 2], 4); router    = ntohl(t); hrtr = 1; }
                break;
            case DHCP_OPT_DNS:
                if (olen >= 4) { uint32_t t; memcpy(&t, &opt[i + 2], 4); dns       = ntohl(t); hdns = 1; }
                break;
            case DHCP_OPT_LEASE_TIME:
                if (olen >= 4) { uint32_t t; memcpy(&t, &opt[i + 2], 4); lease     = ntohl(t); hlease = 1; }
                break;
            case DHCP_OPT_SERVER_ID:
                if (olen >= 4) { uint32_t t; memcpy(&t, &opt[i + 2], 4); server_id = ntohl(t); hsid = 1; }
                break;
        }
        i += 2 + olen;
    }

    if (out) {
        out->xid           = ntohl(pkt->xid);
        out->yiaddr        = ntohl(pkt->yiaddr);
        out->subnet        = subnet;
        out->router        = router;
        out->dns           = dns;
        out->lease         = lease;
        out->server_id     = server_id;
        out->msg_type      = msg_type;
        out->have_subnet   = hsub;
        out->have_router   = hrtr;
        out->have_dns      = hdns;
        out->have_lease    = hlease;
        out->have_server_id = hsid;
        out->_pad[0] = 0;
        out->_pad[1] = 0;
    }
    return DHCP_PARSE_OK;
}

// Live dispatcher. With -DRUST_DHCP (set in the Makefile) the incoming DHCP reply
// parse runs in Rust; drop the flag + rebuild to roll straight back to C.
int dhcp_parse(const uint8_t *buf, uint32_t len, dhcp_parsed_t *out) {
#ifdef RUST_DHCP
    return dhcp_parse_rs(buf, len, out);
#else
    return dhcp_parse_c(buf, len, out);
#endif
}

// Handle incoming DHCP packet
static void dhcp_handle(uint32_t src_ip, uint16_t src_port,
                        const void *data, uint16_t length) {
    (void)src_ip;
    (void)src_port;

    if (length < sizeof(dhcp_packet_t) - 308) {
        if (g_dhcp_dbg) kprintf("[DHCP] rx drop: short len=%d\n", (int)length);
        return;
    }

    // Pure parse/validate (op==2 reply + magic cookie + option TLV walk). Routes
    // to dhcp_parse_rs (Rust) under -DRUST_DHCP, else the verbatim dhcp_parse_c.
    // The op/magic checks that used to live here inline are now inside the seam.
    dhcp_parsed_t pr;
    int _pres = dhcp_parse((const uint8_t *)data, length, &pr);
    if (_pres != DHCP_PARSE_OK) {
        if (g_dhcp_dbg) kprintf("[DHCP] rx drop: parse=%d len=%d\n", _pres, (int)length);
        return;
    }

    // #522: record EVERY accepted reply in the lock-free ring. This is the
    // evidence "0 [DHCPDIAG] rx lines in the serial log" could never be, since
    // that log drops lines under concurrent kprintf.
    dhcp_trace(DHCP_TRC_RX, (uint32_t)pr.msg_type);

    if (g_dhcp_dbg) {
        kprintf("[DHCP] rx len=%d mt=%d xid=%x mine=%x state=%d\n",
                (int)length, (int)pr.msg_type, (unsigned)pr.xid,
                (unsigned)dhcp_xid, dhcp_state);
    }

    // Verify transaction ID (STATEFUL - stays in the caller). Equivalent to the
    // old `pkt->xid != htonl(dhcp_xid)`: pr.xid == ntohl(pkt->xid).
    if (pr.xid != dhcp_xid) {
        if (g_dhcp_dbg) kprintf("[DHCP] rx drop: xid\n");
        return;
    }

    // Apply the extracted options into the module globals EXACTLY as the old
    // dhcp_parse_options() did: only when the option was present with a valid
    // length, last-occurrence wins, for every accepted reply regardless of state
    // or message type (so dhcp_server_ip is set before an OFFER builds a REQUEST).
    if (pr.have_subnet)    dhcp_netmask   = pr.subnet;
    if (pr.have_router)    dhcp_gateway   = pr.router;
    if (pr.have_dns)       dhcp_dns       = pr.dns;
    if (pr.have_server_id) dhcp_server_ip = pr.server_id;

    uint8_t msg_type = pr.msg_type;
    uint8_t *p;

    switch (dhcp_state) {
        case DHCP_STATE_DISCOVERING:
            if (msg_type == DHCP_OFFER) {
                // Got an offer - yiaddr already converted to host byte order.
                dhcp_offered_ip = pr.yiaddr;
                p = (uint8_t *)&dhcp_offered_ip;
                kprintf("[DHCP] Offer received: %d.%d.%d.%d\n",
                        p[3], p[2], p[1], p[0]);

                // Send request. #519: arm the deadline and reset the retry count
                // BEFORE publishing REQUESTING, for the same reason as
                // dhcp_discover() - a concurrent dhcp_poll() that sees the new
                // state must also see a live deadline, never the expired one we
                // were just using for DISCOVER (which would instantly burn the
                // retries and drop the ACK).
                dhcp_timeout = timer_ticks + dhcp_timeout_ticks();
                dhcp_retries = 0;
                dhcp_state = DHCP_STATE_REQUESTING;

                uint8_t request[576];
                int len = dhcp_build_packet(request, DHCP_REQUEST);

                // Send via UDP to broadcast
                udp_send(0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                         request, len);

                dhcp_timeout = timer_ticks + dhcp_timeout_ticks();
            }
            break;

        case DHCP_STATE_IDLE:
            // #523: A LATE OFFER IS STILL A VALID OFFER. DO NOT DROP IT.
            //
            // This case did not exist. An OFFER arriving while IDLE fell through
            // the switch and was silently discarded, which is precisely how the
            // #522 retry race turned into "no DHCP at all": the burst drove
            // dhcp_retries to 3 within ~40ms, dhcp_state went IDLE, and the real
            // OFFER (this LAN answers in ~0.8ms) then landed here and vanished.
            // The box fell back to the hardcoded .200-.209 static, which on a LAN
            // that happens to match the guess LOOKS like a working network and is
            // exactly what disguised this failure as success for so long.
            //
            // Accepting a late OFFER is correct and safe: we only act on one whose
            // xid matches our own transaction (checked above, and the xid is now
            // per-transaction random, MAYTERA-SEC-2026-0015), so it is a reply to
            // a DISCOVER *we* actually sent. Rather than throw away a lease the
            // server is holding for us, resume the transaction and REQUEST it.
            // Belt and braces for the race fix, not a substitute for it.
            if (msg_type == DHCP_OFFER && dhcp_offered_ip == 0) {
                dhcp_offered_ip = pr.yiaddr;
                p = (uint8_t *)&dhcp_offered_ip;
                kprintf("[DHCP] Late offer accepted: %d.%d.%d.%d\n",
                        p[3], p[2], p[1], p[0]);

                // Arm deadline and reset retries BEFORE publishing the state, for
                // the same reason as dhcp_discover() (#519).
                dhcp_timeout = timer_ticks + dhcp_timeout_ticks();
                dhcp_retries = 0;
                dhcp_state = DHCP_STATE_REQUESTING;

                uint8_t request[576];
                int len = dhcp_build_packet(request, DHCP_REQUEST);
                udp_send(0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                         request, len);
                dhcp_timeout = timer_ticks + dhcp_timeout_ticks();
            }
            break;

        case DHCP_STATE_REQUESTING:
            if (msg_type == DHCP_ACK) {
                // #380: got ACK, but do NOT adopt the address yet. Run RFC 5227
                // duplicate-address detection first (RFC 2131 SHOULD): probe the
                // offered IP and let dhcp_poll() decide bind-vs-DECLINE. We must
                // NOT pump RX synchronously here (this runs inside eth_receive);
                // dhcp_poll() drives the probes from top-level context.
                dhcp_state = DHCP_STATE_DAD;
                arp_dad_arm(dhcp_offered_ip);
                arp_probe(dhcp_offered_ip);
                dhcp_dad_probes = 1;
                dhcp_dad_calls = 0;
                {
                    uint64_t hz = g_timer_hz ? g_timer_hz : 100;
                    uint64_t per = hz / 5; if (per == 0) per = 1;  // ~200ms
                    dhcp_dad_deadline = timer_ticks + per;
                }
                p = (uint8_t *)&dhcp_offered_ip;
                kprintf("[DHCP] ACK for %d.%d.%d.%d; verifying it is free (DAD)...\n",
                        p[3], p[2], p[1], p[0]);
            } else if (msg_type == DHCP_NAK) {
                // Request rejected, restart
                kprintf("[DHCP] NAK received, restarting\n");
                dhcp_state = DHCP_STATE_IDLE;
                dhcp_offered_ip = 0;
            }
            break;
    }
}

// Initialize DHCP client
void dhcp_init(void) {
    dhcp_state = DHCP_STATE_IDLE;
    dhcp_xid = timer_ticks ^ 0xDEADBEEF;  // Random-ish transaction ID

    // Bind to DHCP client port
    udp_bind(DHCP_CLIENT_PORT, dhcp_handle);

    kprintf("[DHCP] DHCP client initialized\n");
}

// Start DHCP discovery
int dhcp_discover(void) {
    if (dhcp_state == DHCP_STATE_BOUND) {
        kprintf("[DHCP] Already bound\n");
        return 0;
    }

    kprintf("[DHCP] Starting discovery...\n");

    // Clear IP address during discovery
    ip_set_address(0);

    // #519 ROOT CAUSE FIX: ARM THE DEADLINE **BEFORE** PUBLISHING THE STATE.
    //
    // The old order was: set dhcp_state = DISCOVERING, build, send, print, and
    // only THEN set dhcp_timeout. dhcp_timeout is 0 on the first pass, so for the
    // whole build+send+4-slow-serial-kprintf window (~10ms) the state was
    // DISCOVERING while the deadline still read 0. dhcp_poll() is called from at
    // least five concurrent contexts (the shell loop at >=250Hz via main.c, the
    // compositor flip path via gui/fb_syscall.c, net_worker's own 4x net_poll
    // burst, SSH/FTP workers) on an SMP box, and neither this function nor the
    // retry block takes net_lock. Every one of those pollers evaluated
    // `timer_ticks > dhcp_timeout` against the STALE 0, so each counted a bogus
    // timeout: all 3 retries burned in ~16ms (vs the intended 5s), dhcp_state
    // went IDLE, and the real OFFER - which this LAN returns in 0.7ms - landed in
    // a switch() with no IDLE case and was silently discarded. DORA never
    // completed and the box fell back to the .200-.209 static address. Proven by
    // tcpdump on vmbr0 (DISCOVER out, OFFER back in 0.7ms, no REQUEST ever) plus
    // the serial log showing "[DHCP] Timeout, retrying" printing INSIDE this
    // function's own "Packet built, len=249, sending..." line.
    //
    // Arming the deadline first means any concurrent poll that observes
    // DISCOVERING also observes a live, in-the-future deadline.
    dhcp_timeout = timer_ticks + dhcp_timeout_ticks();
    dhcp_retries = 0;
    dhcp_last_send_call = dhcp_poll_calls;   // #524: start this send's poll window

    // MAYTERA-SEC-2026-0015 (#521): pick a FRESH RANDOM xid per DORA transaction
    // (RFC 2131 4.1: "chosen by the client"), not a bump of a constant seed.
    // dhcp_init() ran `dhcp_xid = timer_ticks ^ 0xDEADBEEF` BEFORE sti(), when
    // timer_ticks is always 0, so every MayteraOS box on earth started at
    // 0xDEADBEEF and sent its first DISCOVER as 0xDEADBEF0. OBSERVED on the build server
    // LAN: two independent MayteraOS VMs (bc:24:11:5f:d9:25 and bc:24:11:a9:3b:1e)
    // broadcasting DISCOVERs with the IDENTICAL xid 0xdeadbef0 at the same time.
    // Two consequences, both real:
    //   1) CORRECTNESS: OFFERs are broadcast (we set flags=0x8000), so box A's
    //      OFFER reaches box B, whose `pr.xid != dhcp_xid` check PASSES. B can
    //      adopt A's address -> the #380 IP-collision class that took the LAN down.
    //   2) SECURITY: a constant, publicly-known xid removes the only anti-spoof
    //      entropy in DHCP. An off-path attacker can blind-inject a forged
    //      OFFER/ACK (no need to see the DISCOVER) and set our gateway + DNS.
    //      CWE-330 (insufficiently random values).
    // rng_get_u32() is safe here: dhcp_discover() runs from net_worker, long after
    // crypto_init()/rng_init() (which run AFTER dhcp_init(), hence not seeding at
    // init time). Never leave the xid at 0 or the constant.
    {
        extern uint32_t rng_get_u32(void);
        uint32_t x = rng_get_u32();
        if (x == 0 || x == 0xDEADBEEF) x ^= (uint32_t)timer_ticks ^ 0x5A5A5A5Au;
        dhcp_xid = x;
    }

    dhcp_state = DHCP_STATE_DISCOVERING;   // publish LAST

    // Build and send discover
    // Note: dhcp_packet_t is 548 bytes (4+4+4+16+16+64+128+4+308)
    uint8_t packet[576];  // Enough for dhcp_packet_t (548 bytes) + padding
    int len = dhcp_build_packet(packet, DHCP_DISCOVER);

    // Send to broadcast
    int result = udp_send(0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                          packet, len);
    if (result < 0) {
        kprintf("[DHCP] Failed to send DISCOVER\n");
    }

    // Re-arm AFTER the send too: the send itself can take milliseconds, and the
    // deadline should be measured from when the request actually hit the wire.
    dhcp_timeout = timer_ticks + dhcp_timeout_ticks();

    return result;
}

// Start DHCP discovery and wait for completion (blocking)
int dhcp_discover_blocking(void) {
    extern int eth_receive(void);
    extern int nic_link_up(void);

    if (dhcp_state == DHCP_STATE_BOUND) {
        kprintf("[DHCP] Already bound\n");
        return 0;
    }

    // #381: carrier gate. With no cable there is nothing to wait for: the ~10s
    // link-wait below would just spin (and, on the UI/boot path, block). Fail
    // fast; the background net worker restarts DHCP on the carrier-up edge.
    if (!nic_link_up()) {
        kprintf("[DHCP] link down; DHCP deferred to net worker (carrier-up)\n");
        return -1;
    }

    // Wait for link to come up. #378: a real switch takes >3s to autonegotiate,
    // so wait up to ~10s (was ~2s). Critically, PUMP THE NIC RX PATH during the
    // wait: for the USB dongle, the PHY link state only refreshes when
    // usb_eth_receive() runs (usb_asix_refresh_link) and nic_link_up() itself
    // now actively re-reads the PHY, so a bare io_wait() spin (the old code)
    // could never observe the carrier coming up. eth_receive() is safe here
    // (net stack is initialised before this is called).
    kprintf("[DHCP] Waiting for link (pumping RX, up to ~10s)...\n");
    {
        extern volatile uint64_t timer_ticks;
        extern uint32_t g_timer_hz;
        uint64_t hz = g_timer_hz ? g_timer_hz : 100;
        uint64_t start = timer_ticks;
        uint64_t limit = hz * 10;          // ~10 seconds
        int guard = 0;
        while (!nic_link_up()) {
            eth_receive();                 // pump RX so d->link can advance
            for (int j = 0; j < 200; j++) io_wait();
            if (timer_ticks != start && (timer_ticks - start) >= limit) break;
            if (++guard > 200000) break;   // hard cap if timer_ticks is stalled
        }
    }

    if (!nic_link_up()) {
        kprintf("[DHCP] Link still down after wait; DHCP will retry in background on link-up\n");
        return -1;
    }
    kprintf("[DHCP] Link is up\n");

    // Small delay for switch/network to be ready
    for (int i = 0; i < 50000; i++) io_wait();

    // Start discovery
    kprintf("[DHCP] Calling dhcp_discover()...\n");
    int disc_result = dhcp_discover();
    kprintf("[DHCP] dhcp_discover() returned %d\n", disc_result);
    if (disc_result < 0) {
        kprintf("[DHCP] Discovery failed!\n");
        return -1;
    }

    kprintf("[DHCP] Discovery succeeded, entering wait loop...\n");
    kprintf("[DHCP] About to access timer_ticks...\n");

    // Use iteration-based timeout (timer_ticks may not be updating)
    // ~10 seconds worth of iterations
    kprintf("[DHCP] Waiting for response (timer_ticks=%llu)...\n", timer_ticks);
    int max_iterations = 100000;  // ~10 seconds at ~100us per iteration
    int retries_done = 0;
    int retry_interval = 30000;   // ~3 seconds between retries

    for (int iter = 0; iter < max_iterations; iter++) {
        // Process incoming packets
        eth_receive();
        // #380: drive the ACK->DAD->BOUND transition (dhcp_poll owns it now).
        dhcp_poll();

        // Check if we got a response
        if (dhcp_state == DHCP_STATE_BOUND) {
            kprintf("[DHCP] Success! Bound after %d iterations\n", iter);
            return 0;
        }

        // Check for timeout/retry
        if ((iter % retry_interval) == (retry_interval - 1)) {
            retries_done++;
            if (retries_done >= 3) {
                kprintf("[DHCP] Max retries reached\n");
                break;
            }
            kprintf("[DHCP] No response, retry %d/3...\n", retries_done);

            // Re-send discover
            uint8_t packet[576];
            int len = dhcp_build_packet(packet, DHCP_DISCOVER);
            udp_send(0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, packet, len);
        }

        // Small delay (~100us)
        for (int i = 0; i < 100; i++) {
            io_wait();
        }
    }

    kprintf("[DHCP] Discovery timed out (timer_ticks=%llu)\n", timer_ticks);
    return -1;
}

// Get DHCP state
int dhcp_get_state(void) {
    return dhcp_state;
}

// Check if bound
int dhcp_is_bound(void) {
    return dhcp_state == DHCP_STATE_BOUND;
}

// Get assigned IP
uint32_t dhcp_get_ip(void) {
    return dhcp_offered_ip;
}

// Get assigned gateway
uint32_t dhcp_get_gateway(void) {
    return dhcp_gateway;
}

// Get assigned netmask
uint32_t dhcp_get_netmask(void) {
    return dhcp_netmask;
}

// Get DNS server
uint32_t dhcp_get_dns(void) {
    return dhcp_dns;
}

// Process DHCP (handle timeouts)
void dhcp_poll(void) {
    // #524: count real poll progress. This is the burst guard's clock: it cannot
    // be fabricated by reinjected PIT ticks, only by dhcp_poll actually running.
    __sync_fetch_and_add(&dhcp_poll_calls, 1);

    // #380: drive duplicate-address detection between ACK and bind.
    if (dhcp_state == DHCP_STATE_DAD) {
        uint8_t *p;
        uint64_t hz = g_timer_hz ? g_timer_hz : 100;
        uint64_t per = hz / 5; if (per == 0) per = 1;   // ~200ms per probe

        if (arp_dad_conflict()) {
            // Another host defends the offered address: DECLINE and restart.
            p = (uint8_t *)&dhcp_offered_ip;
            kprintf("[DHCP] Offered %d.%d.%d.%d is IN USE; sending DECLINE\n",
                    p[3], p[2], p[1], p[0]);
            uint8_t pkt[576];
            int len = dhcp_build_packet(pkt, DHCP_DECLINE);
            udp_send(0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, pkt, len);
            arp_dad_disarm();
            dhcp_offered_ip = 0;
            dhcp_state = DHCP_STATE_IDLE;   // do not adopt a conflicting address
            return;
        }

        dhcp_dad_calls++;
        // Advance when the ~200ms timer window elapses OR (timer-frozen safety)
        // after a bounded number of poll calls.
        if (timer_ticks >= dhcp_dad_deadline || dhcp_dad_calls >= 4000) {
            dhcp_dad_calls = 0;
            if (dhcp_dad_probes < 3) {
                arp_probe(dhcp_offered_ip);
                dhcp_dad_probes++;
                dhcp_dad_deadline = timer_ticks + per;
                return;
            }
            // No defender after 3 probes -> the address is ours to use.
            arp_dad_disarm();
            dhcp_state = DHCP_STATE_BOUND;
            ip_set_address(dhcp_offered_ip);
            ip_set_gateway(dhcp_gateway);
            ip_set_netmask(dhcp_netmask);

            p = (uint8_t *)&dhcp_offered_ip;
            kprintf("[DHCP] Bound to %d.%d.%d.%d (DAD passed)\n",
                    p[3], p[2], p[1], p[0]);
            p = (uint8_t *)&dhcp_netmask;
            kprintf("[DHCP] Netmask: %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);
            p = (uint8_t *)&dhcp_gateway;
            kprintf("[DHCP] Gateway: %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);
            if (dhcp_dns != 0) {
                p = (uint8_t *)&dhcp_dns;
                kprintf("[DHCP] DNS: %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);
            }
            // Announce our verified-unique address (RFC 5227 ANNOUNCE).
            arp_announce();
            arp_announce();
        }
        return;
    }

    if (dhcp_state == DHCP_STATE_IDLE || dhcp_state == DHCP_STATE_BOUND) {
        return;
    }

    // Check timeout.
    //
    // #522 ROOT CAUSE: THE CLAIM MUST BE ATOMIC, NOT MERELY FIRST.
    //
    // History. #519 moved the deadline store to the top of this block; #520 then
    // found a temporary kprintf had been inserted between the check and the store,
    // re-widening the window to milliseconds. Both were real, and BOTH WERE
    // INSUFFICIENT, because `if (now > deadline) { deadline = now + 1250; }` is a
    // check-then-act on a shared variable and is NOT atomic no matter how few
    // instructions separate the two halves. dhcp_poll() is driven by at least five
    // concurrent contexts (the shell loop at >=250Hz via main.c, the compositor
    // flip path via gui/fb_syscall.c, net_worker's 4x net_poll burst, SSH/FTP
    // workers) on an SMP box, and none of them holds net_lock here. When the
    // deadline genuinely expires, ALL of them pass the `now > deadline` test in the
    // same window, and each one then stores a new deadline, counts a retry, and
    // retransmits.
    //
    // That is what the wire actually showed and it reconciles the two measurements
    // that looked contradictory: the serial trace said the timeouts were a correct
    // 5 SECONDS apart (`ticks=7413 to=7408` = "now is 5 ticks past an expired
    // deadline"), while tcpdump showed 3 DISCOVERs 16-42ms apart sharing one xid
    // with IP IDs 0,1,2 (three genuine ip_send calls). Both are true: the 3
    // DISCOVERs are NOT 3 timeout intervals, they are N concurrent contexts all
    // consuming the SAME expired deadline before any one of them claims it.
    // dhcp_retries reaches 3 in one burst, discovery declares failure, dhcp_state
    // goes IDLE, and the OFFER (which this LAN returns in ~0.8ms) is then dropped.
    // Control that proves the mechanism: with the deadline pushed to 300s the
    // window never reopens and exactly ONE DISCOVER is sent.
    //
    // The fix is a CAS: read the deadline, test it, then swap ONLY from that exact
    // value. Exactly one context can win an expired deadline; every loser sees a
    // different old value and returns without counting a retry or retransmitting.
    // Uses the tree's existing atomic_cas64() (sync/spinlock.h), the same primitive
    // cpu/smp.c's BKL uses. NOT hand-rolled: CLAUDE.md is explicit, and
    // fs/blockdev.c:117's private `while (test_and_set) proc_yield()` lock is the
    // cautionary example in this same tree.
    uint64_t now_ticks = timer_ticks;
    uint64_t deadline  = dhcp_timeout;
    if (now_ticks > deadline) {
        dhcp_trace(DHCP_TRC_POLL_EXPIRED, (uint32_t)dhcp_state);

        // #524 BURST GUARD: an expired tick deadline is necessary but NOT
        // sufficient. Require real poll progress too, so a reinjected tick burst
        // cannot collapse the whole retry schedule into a few milliseconds.
        if ((dhcp_poll_calls - dhcp_last_send_call) < DHCP_MIN_POLLS_PER_RETRY) {
            return;
        }

        // Claim it. Only the winner of this CAS owns this timeout tick.
        if (atomic_cas64(&dhcp_timeout, deadline,
                         now_ticks + dhcp_timeout_ticks()) != deadline) {
            dhcp_trace(DHCP_TRC_CLAIM_LOST, (uint32_t)dhcp_state);
            return;   // another context claimed this expiry; it owns the retry
        }
        dhcp_trace(DHCP_TRC_CLAIM_WON, (uint32_t)dhcp_retries);
        dhcp_last_send_call = dhcp_poll_calls;   // this retry owns the next window

        // From here we are single-threaded for this timeout: safe to do slow work.
        if (g_dhcp_dbg) {
            kprintf("[DHCP] timeout ticks=%u hz=%u st=%d\n",
                    (unsigned)now_ticks, (unsigned)g_timer_hz, dhcp_state);
        }

        dhcp_retries++;
        if (dhcp_retries >= 3) {
            dhcp_trace(DHCP_TRC_GIVEUP, (uint32_t)dhcp_retries);
            kprintf("[DHCP] Discovery timed out after %d attempts\n",
                    dhcp_retries);
            dhcp_state = DHCP_STATE_IDLE;
            return;
        }

        // Retry
        kprintf("[DHCP] Timeout, retrying (%d/3)...\n", dhcp_retries);
        dhcp_trace(DHCP_TRC_SEND, (uint32_t)dhcp_state);

        if (dhcp_state == DHCP_STATE_DISCOVERING) {
            uint8_t packet[576];
            int len = dhcp_build_packet(packet, DHCP_DISCOVER);
            udp_send(0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                     packet, len);
        } else if (dhcp_state == DHCP_STATE_REQUESTING) {
            uint8_t packet[576];
            int len = dhcp_build_packet(packet, DHCP_REQUEST);
            udp_send(0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                     packet, len);
        }

        // Measure the deadline from when the retransmit actually hit the wire.
        dhcp_timeout = timer_ticks + dhcp_timeout_ticks();
    }
}

// ---------------------------------------------------------------------------
// #404 / #497 Phase O boot-time self-test: prove dhcp_parse_rs (Rust, live under
// -DRUST_DHCP) == dhcp_parse_c (verbatim reference) on the LIVE agreement domain
// (well-formed OFFER/ACK/NAK, END-terminated within len, with/without each
// option, PAD + unknown options), report the SECURITY posture HONESTLY (the
// reachable over-read the C never confined), and micro-benchmark both. LIGHT
// (#426, bounded, runs once): ~512 differential vectors + a crafted-runt attack
// sweep + a ~5k-iter RDTSC bench. The heavy fuzz (200k+ vectors + ASan/UBSan on
// the C reference incl. tight-allocation OOB) runs as the OFFLINE pre-flight.
// One [RUST-DIFF] dhcp, one [RUST-SEC] dhcp, one [RUST-PERF] dhcp line to serial
// + /BOOTLOG.
//
// NOTE (why Part 2 cannot fault boot): the crafted attack vectors are built into
// a large static arena and given a SHORT len; the verbatim C's fixed-308 walk
// reads PAST that len into arena bytes we placed (poison) - stays inside OUR
// arena, so demonstrating the over-read can never fault the boot, while the Rust
// (confined to len) never reads them. The real over-read past an allocation is
// proven by the offline ASan pre-flight.

static uint32_t dhcpdiff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

static inline uint64_t dhcp_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

// Write the fixed 240-byte BOOTP header (op, xid, yiaddr, magic cookie). All
// multi-byte wire fields are big-endian; the magic cookie is the wire order
// 63 82 53 63 at offset 236. Returns the options start offset (240).
static int dhcp_wr_header(uint8_t *b, uint8_t op, uint32_t xid, uint32_t yiaddr) {
    for (int i = 0; i < 240; i++) b[i] = 0;
    b[0] = op;                                    // op (2 = reply)
    b[1] = 1; b[2] = 6;                           // htype=Ethernet, hlen=6
    b[4] = xid >> 24; b[5] = (xid >> 16) & 0xFF;  // xid (network order)
    b[6] = (xid >> 8) & 0xFF; b[7] = xid & 0xFF;
    b[16] = yiaddr >> 24; b[17] = (yiaddr >> 16) & 0xFF;   // yiaddr (network order)
    b[18] = (yiaddr >> 8) & 0xFF; b[19] = yiaddr & 0xFF;
    b[236] = 0x63; b[237] = 0x82; b[238] = 0x53; b[239] = 0x63;  // magic cookie
    return 240;
}

// Append a [type][len][value...] option; value is a big-endian u32 (for the
// 4-byte options) or a single byte for msg-type. Returns the new offset.
static int dhcp_wr_opt4(uint8_t *b, int pos, uint8_t type, uint32_t v) {
    b[pos++] = type; b[pos++] = 4;
    b[pos++] = v >> 24; b[pos++] = (v >> 16) & 0xFF;
    b[pos++] = (v >> 8) & 0xFF; b[pos++] = v & 0xFF;
    return pos;
}

static int dhcp_parsed_eq(int rc_a, const dhcp_parsed_t *a, int rc_b, const dhcp_parsed_t *b) {
    if (rc_a != rc_b) return 1;
    if (rc_a != DHCP_PARSE_OK) return 0;   // both rejected identically: fields N/A
    if (a->xid != b->xid) return 1;
    if (a->yiaddr != b->yiaddr) return 1;
    if (a->msg_type != b->msg_type) return 1;
    if (a->have_subnet != b->have_subnet) return 1;
    if (a->have_router != b->have_router) return 1;
    if (a->have_dns != b->have_dns) return 1;
    if (a->have_lease != b->have_lease) return 1;
    if (a->have_server_id != b->have_server_id) return 1;
    if (a->have_subnet    && a->subnet    != b->subnet)    return 1;
    if (a->have_router    && a->router    != b->router)    return 1;
    if (a->have_dns       && a->dns       != b->dns)       return 1;
    if (a->have_lease     && a->lease     != b->lease)     return 1;
    if (a->have_server_id && a->server_id != b->server_id) return 1;
    return 0;
}

// Build one WELL-FORMED (agreement-domain) DHCP reply into buf; END-terminated
// within len, every option value fully present, so the fixed-308 C walk and the
// len-bounded Rust walk traverse identical bytes and MUST agree. Returns len.
static int dhcp_build_valid(uint8_t *buf, uint32_t *seed) {
    uint32_t xid    = dhcpdiff_rng(seed);
    uint32_t yiaddr = dhcpdiff_rng(seed);
    uint32_t bits   = dhcpdiff_rng(seed);
    // message type: OFFER(2) / ACK(5) / NAK(6)
    static const uint8_t mtypes[3] = { DHCP_OFFER, DHCP_ACK, DHCP_NAK };
    uint8_t mt = mtypes[bits % 3];

    int pos = dhcp_wr_header(buf, 2, xid, yiaddr);

    // Optional leading PAD bytes (exercise the PAD path).
    if (bits & 0x100) { buf[pos++] = DHCP_OPT_PAD; buf[pos++] = DHCP_OPT_PAD; }

    // Message type (always present).
    buf[pos++] = DHCP_OPT_MSG_TYPE; buf[pos++] = 1; buf[pos++] = mt;

    // Randomly include each 4-byte option (last-occurrence-wins is exercised by
    // occasionally writing an option twice).
    if (bits & 1)  pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_SUBNET_MASK, dhcpdiff_rng(seed));
    if (bits & 2)  pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_ROUTER,      dhcpdiff_rng(seed));
    if (bits & 4)  pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_DNS,         dhcpdiff_rng(seed));
    if (bits & 8)  pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_LEASE_TIME,  dhcpdiff_rng(seed));
    if (bits & 16) pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_SERVER_ID,   dhcpdiff_rng(seed));
    // An UNKNOWN option (type 12 hostname, variable) both must skip identically.
    if (bits & 32) { buf[pos++] = DHCP_OPT_HOSTNAME; buf[pos++] = 3; buf[pos++] = 'a'; buf[pos++] = 'b'; buf[pos++] = 'c'; }
    // A duplicate server-id to exercise last-wins.
    if (bits & 64) pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_SERVER_ID,   dhcpdiff_rng(seed));

    buf[pos++] = DHCP_OPT_END;
    // Optional trailing PAD after END, still inside len (C stops at END; Rust
    // stops at END; the pad must not change the verdict).
    if (bits & 128) { buf[pos++] = DHCP_OPT_PAD; buf[pos++] = DHCP_OPT_PAD; buf[pos++] = DHCP_OPT_PAD; }
    return pos;
}

// Build one WELL-FORMED but LONG (> 548-byte) DHCP reply into buf.
//
// WHY THIS EXISTS (#404 3-way drift audit, 2026-07-16). dhcp_build_valid() above
// only ever emits ~250-290 byte packets, so this self-test could not EXPRESS the
// state in which the shipped Rust diverged from the C, and it passed at every
// vector count while two real divergences shipped on the live path. Both need
// len > 548, because that is where the C's FIXED 308-byte options[] window ends:
//
//   shape 0  WALK-WINDOW WIDENING. The whole 308-byte window is PAD with NO END,
//            and the only real options sit PAST offset 548. The C ignores them
//            entirely (msg_type stays 0). A port bounded by [240, len) instead of
//            [240, min(len,548)) HONORS them and adopts an attacker's gateway/
//            dns/server_id. This is the audit's exact PoC.
//   shape 1  LYING DECLARED LENGTH whose value bytes ARE present. An option at
//            offset 540 declares olen=200 (running past len) but its 4 value
//            bytes are received. The C checks only `olen >= 4`, extracts, and
//            advances by 2+olen. A port that rejects on the DECLARED length, and
//            aborts the walk with it, drops an option the C accepted.
//   shape 2  BOTH, adversarially: real options inside the window, NO END inside
//            the window, then DECOY options past 548 carrying different values.
//            The C honors only the in-window set; a widened walk lets the decoys
//            win via last-occurrence-wins.
//
// All three are in the AGREEMENT domain: every byte either side reads is inside
// the received len, so the C never over-reads here and a correct confinement MUST
// reproduce it exactly. Reachability is real, not theoretical: udp_handle passes
// data_len = udp_len - 8 uncapped and dhcp_handle gates only `length < 240`, so a
// 600-1472 byte reply arrives in one ordinary frame. Returns len (< 700, so the
// C's own tail reads at 548..552 stay inside the received bytes AND the arena).
static int dhcp_build_long(uint8_t *buf, uint32_t *seed) {
    uint32_t xid    = dhcpdiff_rng(seed);
    uint32_t yiaddr = dhcpdiff_rng(seed);
    uint32_t bits   = dhcpdiff_rng(seed);
    static const uint8_t mtypes[3] = { DHCP_OFFER, DHCP_ACK, DHCP_NAK };
    uint8_t mt = mtypes[bits % 3];
    uint32_t shape = (bits >> 8) % 3;
    // A distinctive "attacker" value for anything placed past the C's window; it
    // must never legitimately appear, so any parse that adopts it has widened.
    const uint32_t decoy = 0x06060606u;

    int pos = dhcp_wr_header(buf, 2, xid, yiaddr);

    if (shape == 0) {
        // Fill the ENTIRE fixed window [240,548) with PAD and NO END terminator.
        while (pos < 548) buf[pos++] = DHCP_OPT_PAD;
        // Real-looking options, but PAST the window: the C never reaches them.
        buf[pos++] = DHCP_OPT_MSG_TYPE; buf[pos++] = 1; buf[pos++] = mt;
        pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_ROUTER,    decoy);
        pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_DNS,       decoy);
        pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_SERVER_ID, decoy);
        buf[pos++] = DHCP_OPT_END;
    } else if (shape == 1) {
        buf[pos++] = DHCP_OPT_MSG_TYPE; buf[pos++] = 1; buf[pos++] = mt;
        while (pos < 540) buf[pos++] = DHCP_OPT_PAD;
        // Declared length 200 runs past len; the 4 value bytes at 542..545 do NOT.
        uint32_t v = dhcpdiff_rng(seed) | 1u;
        buf[pos++] = DHCP_OPT_DNS; buf[pos++] = 200;
        buf[pos++] = (v >> 24) & 0xFF; buf[pos++] = (v >> 16) & 0xFF;
        buf[pos++] = (v >> 8) & 0xFF;  buf[pos++] = v & 0xFF;
        // Pad out past the window so this is a genuinely long packet.
        while (pos < 600) buf[pos++] = DHCP_OPT_PAD;
        buf[pos++] = DHCP_OPT_END;
    } else {
        // Real options INSIDE the window, then no END, then decoys past 548.
        uint32_t v = dhcpdiff_rng(seed) | 1u;
        buf[pos++] = DHCP_OPT_MSG_TYPE; buf[pos++] = 1; buf[pos++] = mt;
        pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_SUBNET_MASK, v);
        pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_ROUTER,      v);
        pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_DNS,         v);
        pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_SERVER_ID,   v);
        while (pos < 548) buf[pos++] = DHCP_OPT_PAD;     // NO END in the window
        buf[pos++] = DHCP_OPT_MSG_TYPE; buf[pos++] = 1; buf[pos++] = DHCP_NAK;
        pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_ROUTER,    decoy);
        pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_DNS,       decoy);
        pos = dhcp_wr_opt4(buf, pos, DHCP_OPT_SERVER_ID, decoy);
        buf[pos++] = DHCP_OPT_END;
    }
    return pos;
}

void dhcp_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    // 2 KiB arena: big enough that Part 2's fixed-308 C over-read stays inside
    // OUR allocation (options[] tail is at offset 240+308 == 548 < 2048), so
    // demonstrating the over-read can never fault the boot.
    static uint8_t buf[2048];
    uint32_t seed = 0x0d4c9a11;
    uint32_t vectors = 0, mismatches = 0;
    int first_bad = -1;

    // Force-reference the Rust symbol so its archive member is always linked
    // (matches the icmp/arp/dns pattern), regardless of -DRUST_DHCP.
    { dhcp_parsed_t t; dhcp_parse_rs(buf, 0, &t); }

    // Part 1: agreement domain (~512 well-formed OFFER/ACK/NAK vectors) + the
    // reject cases both handle identically (too-short len<240, bad op, bad magic).
    for (uint32_t iter = 0; iter < 512; iter++) {
        dhcp_parsed_t co, ro;
        int len;
        uint32_t k = iter & 3;
        if (k == 0) {
            // too short: len in [0,239]
            len = (int)(dhcpdiff_rng(&seed) % 240);
            for (int i = 0; i < len; i++) buf[i] = (uint8_t)(dhcpdiff_rng(&seed) & 0xFF);
        } else if (k == 1) {
            // valid header but op != 2 (a request) -> both ENOTREPLY
            len = dhcp_build_valid(buf, &seed);
            buf[0] = 1;
        } else if (k == 2) {
            // valid header but corrupted magic -> both EBADMAGIC
            len = dhcp_build_valid(buf, &seed);
            buf[238] ^= 0xFF;
        } else {
            len = dhcp_build_valid(buf, &seed);
        }
        int crc = dhcp_parse_c(buf, (uint32_t)len, &co);
        int rrc = dhcp_parse_rs(buf, (uint32_t)len, &ro);
        vectors++;
        if (dhcp_parsed_eq(crc, &co, rrc, &ro)) {
            mismatches++;
            if (first_bad < 0) first_bad = (int)iter;
        }
    }

    // Part 1b: the LONG (> 548-byte) agreement domain. Part 1's builder tops out
    // at ~290 bytes, which is why the b806 walk-window widening and lying-length
    // strictness divergences shipped invisibly: the corpus could not express them
    // (#404 3-way drift audit, 2026-07-16). These vectors are ALL well-formed and
    // fully received, so C and Rust MUST agree byte-for-byte; before the fix they
    // did not. `long_gt548` is reported so a future corpus regression that stops
    // reaching this domain is visible as a coverage collapse rather than a silent
    // PASS, which is the exact failure this whole class came from.
    uint32_t long_n = 0, long_gt548 = 0;
    {
        uint32_t s4 = 0x9e3779b9;
        for (uint32_t iter = 0; iter < 256; iter++) {
            dhcp_parsed_t co, ro;
            // Re-PAD the whole window + tail: no stale bytes from a prior vector.
            for (int i = 240; i < 720; i++) buf[i] = DHCP_OPT_PAD;
            int len = dhcp_build_long(buf, &s4);
            int crc = dhcp_parse_c(buf, (uint32_t)len, &co);
            int rrc = dhcp_parse_rs(buf, (uint32_t)len, &ro);
            vectors++;
            long_n++;
            if (len > 548) long_gt548++;
            if (dhcp_parsed_eq(crc, &co, rrc, &ro)) {
                mismatches++;
                if (first_bad < 0) first_bad = (int)(1000 + iter);
            }
        }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] dhcp: %u vectors, %u mismatches -> %s\n", vectors, mismatches, verdict);
    bootlog_write("[RUST-DIFF] dhcp: %u vectors, %u mismatches -> %s", vectors, mismatches, verdict);
    kprintf("[RUST-DIFF] dhcp coverage: %u long vectors, %u with len>548 (walk-window + lying-length classes)\n",
            long_n, long_gt548);
    bootlog_write("[RUST-DIFF] dhcp coverage: %u long vectors, %u with len>548", long_n, long_gt548);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] dhcp FIRST MISMATCH iter=%d\n", first_bad);
        bootlog_write("[RUST-DIFF] dhcp FIRST MISMATCH iter=%d", first_bad);
    }

    // Part 2: SECURITY posture - the REACHABLE over-read the C never confined.
    // Craft attacker-spoofable runt OFFERs whose options are NOT END-terminated
    // within the received len, with POISON options placed BEYOND len (but inside
    // the arena). The verbatim C's fixed-308 walk IGNORES len: it reads past the
    // received bytes and picks up the poison as config (netmask/router/dns/
    // server-id). The Rust, confined to len, never sees it -> the two DIVERGE.
    // Count the over-reads. Expected NON-ZERO (this is stronger than arp/dns's
    // 0-divergence latent cases: here the C genuinely over-reads live-spoofable
    // input). No hang: every walk's index strictly increases and is arena-bounded.
    {
        uint32_t sec_n = 0, overreads = 0;
        uint32_t s2 = 0x51ee7c33;
        for (uint32_t r = 0; r < 400; r++) {
            // fresh arena so stale poison from a prior iter never leaks in
            for (int i = 240; i < 560; i++) buf[i] = 0;
            uint32_t xid    = dhcpdiff_rng(&s2);
            uint32_t yiaddr = dhcpdiff_rng(&s2);
            int pos = dhcp_wr_header(buf, 2, xid, yiaddr);
            // received options: just the message type, NO END terminator.
            buf[pos++] = DHCP_OPT_MSG_TYPE; buf[pos++] = 1; buf[pos++] = DHCP_OFFER;
            uint32_t kind = dhcpdiff_rng(&s2) % 3;
            uint32_t poison = 0xC0A80101u + r;   // a distinctive "config" value
            int runt_len;
            if (kind == 0) {
                // runt: len ends right after msg-type, no END. Poison sits beyond.
                runt_len = pos;
                dhcp_wr_opt4(buf, pos, DHCP_OPT_SUBNET_MASK, poison);
                buf[pos + 6] = DHCP_OPT_END;
            } else if (kind == 1) {
                // lying length: a subnet option claims 4 bytes but len cuts after
                // the type+len bytes, so the value is entirely past len.
                buf[pos++] = DHCP_OPT_ROUTER; buf[pos++] = 4;
                runt_len = pos;                 // len ends before the 4 value bytes
                buf[pos++] = (poison >> 24) & 0xFF; buf[pos++] = (poison >> 16) & 0xFF;
                buf[pos++] = (poison >> 8) & 0xFF;  buf[pos++] = poison & 0xFF;
                buf[pos++] = DHCP_OPT_END;
            } else {
                // truncated mid-value: len cuts 2 bytes into a 4-byte dns option.
                buf[pos++] = DHCP_OPT_DNS; buf[pos++] = 4;
                buf[pos++] = (poison >> 24) & 0xFF; buf[pos++] = (poison >> 16) & 0xFF;
                runt_len = pos;                 // len ends mid-value
                buf[pos++] = (poison >> 8) & 0xFF;  buf[pos++] = poison & 0xFF;
                buf[pos++] = DHCP_OPT_END;
            }
            dhcp_parsed_t co, ro;
            int crc = dhcp_parse_c(buf, (uint32_t)runt_len, &co);
            int rrc = dhcp_parse_rs(buf, (uint32_t)runt_len, &ro);
            sec_n++;
            // The C over-reads iff it (accepts and) extracts an option the Rust,
            // confined to len, did not - i.e. the parsed views differ on the
            // poison-derived field.
            if (crc == DHCP_PARSE_OK && rrc == DHCP_PARSE_OK && dhcp_parsed_eq(crc, &co, rrc, &ro)) {
                overreads++;
            }
        }
        kprintf("[RUST-SEC] dhcp: verbatim C over-read past received len on %u/%u crafted runt/lying-length OFFERs "
                "(fixed-308 walk ignores len -> attacker-adjacent bytes parsed as config); Rust confined %u/%u\n",
                overreads, sec_n, sec_n, sec_n);
        bootlog_write("[RUST-SEC] dhcp: C over-read past len on %u/%u crafted OFFERs (fixed-308 ignores len); Rust confined %u/%u (REACHABLE over-read removed)",
                      overreads, sec_n, sec_n, sec_n);
    }

    // Part 3: RDTSC micro-benchmark over a fixed well-formed OFFER. LIGHT: 5k
    // iters. The big counts are the offline pre-flight.
    {
        const int iters = 5000;
        dhcp_parsed_t o;
        uint32_t s3 = 0x6f2a13b8;
        int len = dhcp_build_valid(buf, &s3);

        for (int i = 0; i < 300; i++) {
            dhcp_parse_c(buf, (uint32_t)len, &o);
            dhcp_parse_rs(buf, (uint32_t)len, &o);
        }

        uint64_t t0 = dhcp_tsc_serialized();
        for (int i = 0; i < iters; i++) dhcp_parse_c(buf, (uint32_t)len, &o);
        uint64_t t1 = dhcp_tsc_serialized();
        for (int i = 0; i < iters; i++) dhcp_parse_rs(buf, (uint32_t)len, &o);
        uint64_t t2 = dhcp_tsc_serialized();

        uint64_t c_cyc = (t1 - t0) / iters;
        uint64_t r_cyc = (t2 - t1) / iters;
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        kprintf("[RUST-PERF] dhcp: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
        bootlog_write("[RUST-PERF] dhcp: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}
