// net/sntp.c - #797 SNTP (RFC 4330) one-shot time client: transport layer.
//
// WHAT THIS REPLACES, AND WHY IT IS NOT A GREENFIELD FILE.
//
// This tree already had an NTP client. It was ~60 lines inline in
// proc/syscall.c (ntp_udp_cb + sys_ntp_sync, syscall 147, reachable from the
// Settings date/time panel). It worked, in the narrow sense that it usually set
// the clock. What it did NOT do:
//
//   * It hardcoded ONE server (216.239.35.0, time.google.com) with no way for a
//     caller to name another. The first-boot wizard has an "NTP server" field
//     and there was nothing underneath it to wire up. That is the gap that
//     prompted this work.
//   * It VALIDATED NOTHING. Any datagram of >= 48 bytes arriving on the fixed
//     port 12300, from ANY source address, set the system clock to whatever
//     bytes 40..44 said. No mode check, no stratum check, no kiss-o'-death
//     check, no zero-timestamp check, no request/reply binding, no date sanity
//     check. Setting the clock is a security-relevant action - a clock rolled
//     backwards re-validates expired and revoked certificates - so this was an
//     unauthenticated remote primitive for breaking TLS validation.
//   * It would have broken on 2036-02-07. `ntp_sec - 2208988800UL` in uint32_t
//     arithmetic wraps at the era boundary and yields a date around 1900.
//   * Its local port was a fixed constant, so an off-path attacker needed to
//     guess nothing at all.
//
// The validation now lives in rustkern/sntp.rs (new kernel code, so Rust per
// the 2026-07-16 rule; no hot path, so no performance justification for C).
// This file is transport only: resolve, bind, send, wait, apply. syscall 147
// keeps its number and its behaviour-on-success but now routes through here, so
// the existing Settings caller gains every check above without changing.
//
// HOW WE WAIT, AND WHY IT IS NOT A BARE wait_event_timeout.
//
// The rule is that all waiting goes through the wait queue, and this file obeys
// it: the actual sleep is wait_event_deadline() on g_sntp_wq, woken from the UDP
// receive callback. But a bare blocking wait CANNOT work here, and the reason is
// structural rather than a matter of taste:
//
//   net_poll() is the ONLY receive path in this stack. e1000_irq_handler() has
//   ZERO callers tree-wide and its own body says "RX interrupt handled by
//   polling in receive()", so no packet is ever delivered from an ISR. The
//   background net worker (net/net.c net_worker) pumps net_poll() only while
//   DHCP is unbound; once bound it sleeps 1000ms a pass and pumps nothing. The
//   only other pump is the compositor's flip path (gui/fb_syscall.c).
//
// So a thread that blocks and waits for someone else to run net_poll() is
// depending on the compositor being alive and flipping. On a serial/headless
// boot, or with the compositor blocked, nothing would ever drain the NIC and the
// wait would ALWAYS time out. That is correctness by coincidence, which is what
// the existing code's own #512 comment already says about this exact site.
//
// The shape here is therefore: an outer loop bounded by ONE absolute deadline,
// which pumps net_poll() itself and then sleeps on the wait queue for a short
// pump interval. Both wake sources are live and neither is required:
//   (a) our own net_poll() drains the reply and the loop's condition re-check
//       sees it immediately;
//   (b) if some OTHER context (the compositor) drains it first, the callback's
//       wake_up() kicks us out of the wait instantly instead of leaving us
//       asleep for the rest of the pump interval.
// That redundancy is the preferred pattern (an always-armed wake source), and
// the timeout on top of it is the genuinely-correct kind: the wake we are
// waiting for belongs to a remote peer who may simply never answer.
//
// The per-wait deadline is short and the LOOP deadline is absolute and computed
// once, which is exactly the discipline sync/waitq.h asks for: a spurious wake
// re-arms only the short inner wait, never the overall budget.

#include "sntp.h"
#include "net.h"
#include "udp.h"
#include "dns.h"
#include "../string.h"
#include "../serial.h"
#include "../sync/waitq.h"
#include "../crypto/crypto.h"    // rng_get_u32
#include "../proc/syscall.h"     // sys_set_rtc_time / sys_set_rtc_date (owning header)

// Rust validator (rustkern/sntp.rs). Layout of sntp_result_t is locked below.
int sntp_build_request_rs(uint8_t *buf, uint32_t len, uint32_t nonce_hi, uint32_t nonce_lo);
int sntp_parse_reply_rs(const uint8_t *pkt, uint32_t len,
                        uint32_t nonce_hi, uint32_t nonce_lo, sntp_result_t *out);
int sntp_selftest_rs(uint32_t *out_checks);

// If this fires, the #[repr(C)] SntpResult in rustkern/sntp.rs has drifted from
// sntp_result_t and the two are no longer the same struct.
_Static_assert(sizeof(sntp_result_t) == 40, "sntp_result_t must be 40 bytes for the Rust FFI");

// ---------------------------------------------------------------------------
// In-flight request state.
//
// One sync at a time. A second concurrent caller gets SNTP_E_BUSY rather than
// racing the first one's nonce and reply buffer. g_sntp_busy is claimed with an
// atomic test-and-set so two Ring-3 callers cannot both win.
// ---------------------------------------------------------------------------
static wait_queue_head_t g_sntp_wq;
static int      g_sntp_wq_ready = 0;
static int      g_sntp_busy = 0;

static volatile int      g_sntp_have_reply = 0;
static volatile uint16_t g_sntp_reply_len = 0;
static uint8_t           g_sntp_reply[SNTP_PKT_LEN];
// NETWORK byte order, deliberately named so. See sntp_udp_cb().
static uint32_t          g_sntp_expect_ip_net = 0;
static uint32_t          g_sntp_nonce_hi = 0;
static uint32_t          g_sntp_nonce_lo = 0;

// Self-test runs once, on the first sync. It is cheap (twelve 48-byte vectors)
// and it means a validator that has been broken by an edit is caught before it
// is trusted with the clock, rather than silently accepting everything.
static int g_sntp_selftest_state = 0;   // 0 = not run, 1 = passed, -1 = failed

// Host -> network byte order. There is no shared htonl() header in this tree;
// net/dns.c and friends each carry their own static inline, so this matches the
// local convention rather than inventing a new global.
static inline uint32_t sntp_htonl(uint32_t h) {
    return ((h & 0xFFu) << 24) | ((h & 0xFF00u) << 8) |
           ((h >> 8) & 0xFF00u) | ((h >> 24) & 0xFFu);
}

const char *sntp_strerror(int status) {
    switch (status) {
        case SNTP_OK:          return "ok";
        case SNTP_E_ARG:       return "bad argument";
        case SNTP_E_SHORT:     return "reply too short";
        case SNTP_E_LI:        return "server not synchronised (LI=3)";
        case SNTP_E_VERSION:   return "bad NTP version";
        case SNTP_E_MODE:      return "not a server-mode reply";
        case SNTP_E_STRATUM:   return "bad stratum (kiss-o'-death or unsynchronised)";
        case SNTP_E_NONCE:     return "reply did not echo our request (spoof or stale)";
        case SNTP_E_ZEROTS:    return "zero transmit timestamp";
        case SNTP_E_RANGE:     return "date outside the sanity window";
        case SNTP_E_NOLINK:    return "no network carrier";
        case SNTP_E_NONET:     return "no IP address configured";
        case SNTP_E_RESOLVE:   return "could not resolve server name";
        case SNTP_E_BIND:      return "could not bind a local UDP port";
        case SNTP_E_SEND:      return "send failed";
        case SNTP_E_TIMEOUT:   return "no reply (timeout)";
        case SNTP_E_BUSY:      return "another sync is in flight";
        case SNTP_E_SELFTEST:  return "validator self-test failed";
        default:               return "unknown error";
    }
}

// Receive callback. Runs from net_poll(), i.e. potentially with interrupts off
// under net_lock. It does NOTHING that can block: a bounded 48-byte copy and a
// wake_up() (which is documented safe from IRQ context). It must never call
// wait_event or udp_send.
static void sntp_udp_cb(uint32_t src_ip, uint16_t src_port,
                        const void *data, uint16_t len) {
    if (!g_sntp_busy) return;
    if (g_sntp_have_reply) return;          // first acceptable reply wins
    // Source checks the old implementation did not do at all. These are cheap
    // and they are the difference between "a reply" and "the reply".
    // BYTE ORDER TRAP, found by booting this and getting zero replies:
    // net/ip.c hands IP handlers `header->src_ip` RAW, i.e. NETWORK byte order,
    // while dns_resolve() and udp_send() both speak HOST order. Comparing the
    // two drops every reply and looks exactly like an unreachable server.
    // net/dhcp.c never noticed because its callback does `(void)src_ip;`.
    if (src_ip != g_sntp_expect_ip_net) return; // must be the server we asked
    if (src_port != SNTP_PORT) return;      // and from the NTP port
    if (len < SNTP_PKT_LEN) return;

    const uint8_t *p = (const uint8_t *)data;
    for (int i = 0; i < SNTP_PKT_LEN; i++) g_sntp_reply[i] = p[i];
    g_sntp_reply_len = len;
    __sync_synchronize();                   // publish the buffer before the flag
    g_sntp_have_reply = 1;
    wake_up(&g_sntp_wq);
}

// Dotted-quad parse. Returns the address in host byte order, or 0 if `s` is not
// a literal (in which case the caller resolves it as a name). Deliberately
// strict: exactly four octets, each 0..255, digits only.
static uint32_t sntp_parse_ipv4(const char *s) {
    uint32_t octets[4];
    int idx = 0;
    if (!s || !*s) return 0;
    while (*s && idx < 4) {
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            if (v > 255) return 0;
            digits++;
            s++;
            if (digits > 3) return 0;
        }
        if (digits == 0) return 0;
        octets[idx++] = v;
        if (*s == '.') { s++; if (!*s) return 0; }
        else if (*s) return 0;
    }
    if (idx != 4 || *s) return 0;
    return (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
}

// Query ONE server. `server` must be non-NULL and non-empty here; the
// default-list handling lives in sntp_sync() below.
static int sntp_sync_one(const char *server, uint32_t timeout_ms, sntp_result_t *out) {
    sntp_result_t res;
    int rc;

    if (out) {
        for (unsigned i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;
    }

    // Prove the validator before trusting it with the clock.
    if (g_sntp_selftest_state == 0) {
        uint32_t checks = 0;
        int fails = sntp_selftest_rs(&checks);
        if (fails == 0) {
            g_sntp_selftest_state = 1;
            kprintf("[SNTP] validator self-test PASSED (%u vectors) "
                    "SNTP-797-RFC4330-VALIDATED\n", checks);
        } else {
            g_sntp_selftest_state = -1;
            kprintf("[SNTP] validator self-test FAILED, mask=0x%x (%u vectors)\n",
                    (unsigned)fails, checks);
        }
    }
    if (g_sntp_selftest_state < 0) return SNTP_E_SELFTEST;

    // OFFLINE MUST BE AN INSTANT NO-OP, NEVER A MULTI-SECOND STALL (#381).
    // Checked before anything that can take time: no DNS, no bind, no wait.
    if (!nic_link_up()) {
        kprintf("[SNTP] no carrier; sync skipped (instant no-op)\n");
        return SNTP_E_NOLINK;
    }
    if (!net_is_up()) {
        kprintf("[SNTP] link up but no IP configured; sync skipped\n");
        return SNTP_E_NONET;
    }

    if (!server || !server[0]) return SNTP_E_ARG;

    // Resolve. A literal skips DNS entirely, which also means a caller can sync
    // against a LAN server with no working resolver.
    uint32_t srv_ip = sntp_parse_ipv4(server);
    if (srv_ip == 0) {
        if (dns_resolve(server, &srv_ip) != 0 || srv_ip == 0) {
            kprintf("[SNTP] could not resolve '%s'\n", server);
            return SNTP_E_RESOLVE;
        }
    }

    // Claim the single in-flight slot.
    if (__sync_lock_test_and_set(&g_sntp_busy, 1) != 0) return SNTP_E_BUSY;

    if (!g_sntp_wq_ready) { wait_queue_head_init(&g_sntp_wq); g_sntp_wq_ready = 1; }

    g_sntp_have_reply = 0;
    g_sntp_reply_len = 0;
    g_sntp_expect_ip_net = sntp_htonl(srv_ip);   // callback compares in NETWORK order

    // Randomise BOTH the nonce and the local port. The old code used a fixed
    // port and no nonce, so an off-path attacker had to guess nothing; now a
    // forgery must match 64 bits of nonce and the ephemeral port, on top of
    // spoofing the server's source address.
    g_sntp_nonce_hi = rng_get_u32();
    g_sntp_nonce_lo = rng_get_u32();
    if (g_sntp_nonce_hi == 0 && g_sntp_nonce_lo == 0) g_sntp_nonce_hi = 0xA5A5A5A5u;

    uint16_t lport = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        uint16_t p = (uint16_t)(20000u + (rng_get_u32() % 40000u));
        if (udp_bind(p, sntp_udp_cb) == 0) { lport = p; break; }
    }
    if (lport == 0) {
        __sync_lock_release(&g_sntp_busy);
        return SNTP_E_BIND;
    }

    uint8_t pkt[SNTP_PKT_LEN];
    if (sntp_build_request_rs(pkt, SNTP_PKT_LEN, g_sntp_nonce_hi, g_sntp_nonce_lo) != SNTP_OK) {
        udp_unbind(lport);
        __sync_lock_release(&g_sntp_busy);
        return SNTP_E_ARG;
    }

    kprintf("[SNTP] querying %s (%u.%u.%u.%u:123) from port %u, budget %ums\n",
            server, (srv_ip >> 24) & 0xFF, (srv_ip >> 16) & 0xFF,
            (srv_ip >> 8) & 0xFF, srv_ip & 0xFF, lport, timeout_ms);

    // ONE absolute deadline for the whole exchange, computed once.
    uint64_t overall = wq_deadline_in(wq_ms_to_ticks(timeout_ms));
    // UDP is lossy and a single dropped request should not cost the user the
    // whole budget, so retransmit up to SNTP_TRIES times inside it. The nonce is
    // reused across retries on purpose: a reply to ANY of them is a valid reply
    // to this exchange.
    const int SNTP_TRIES = 3;
    const uint32_t SNTP_PUMP_MS = 10;       // inner wait; see the header comment
    int sends_ok = 0;

    for (int try_i = 0; try_i < SNTP_TRIES && !g_sntp_have_reply; try_i++) {
        if (udp_send(srv_ip, lport, SNTP_PORT, pkt, SNTP_PKT_LEN) >= 0) sends_ok++;

        // Per-attempt sub-deadline, clamped to the overall budget so the last
        // attempt cannot run past it.
        uint64_t sub = wq_deadline_in(wq_ms_to_ticks(timeout_ms / (uint32_t)SNTP_TRIES));
        if ((int64_t)(sub - overall) > 0) sub = overall;

        while (!g_sntp_have_reply && !wq_deadline_expired(sub)) {
            net_poll();                     // (a) we drive the RX engine ourselves
            if (g_sntp_have_reply) break;
            // (b) and sleep on the queue meanwhile, so a drain by any other
            // context wakes us at once. WAIT_OK here means, and only means,
            // that g_sntp_have_reply became true.
            (void)wait_event_deadline(&g_sntp_wq, g_sntp_have_reply,
                                      wq_deadline_in(wq_ms_to_ticks(SNTP_PUMP_MS)));
        }
        if (wq_deadline_expired(overall)) break;
    }

    udp_unbind(lport);

    if (!g_sntp_have_reply) {
        __sync_lock_release(&g_sntp_busy);
        if (sends_ok == 0) {
            kprintf("[SNTP] send failed on all %d attempts\n", SNTP_TRIES);
            return SNTP_E_SEND;
        }
        kprintf("[SNTP] no reply from %s within %ums\n", server, timeout_ms);
        return SNTP_E_TIMEOUT;
    }

    uint16_t rlen = g_sntp_reply_len;
    rc = sntp_parse_reply_rs(g_sntp_reply, rlen, g_sntp_nonce_hi, g_sntp_nonce_lo, &res);
    __sync_lock_release(&g_sntp_busy);

    if (rc != SNTP_OK) {
        kprintf("[SNTP] reply REJECTED: %s (status=%d)\n", sntp_strerror(rc), rc);
        return rc;   // RTC untouched
    }

    kprintf("[SNTP] accepted %04d-%02d-%02d %02d:%02d:%02d UTC "
            "(unix=%u, NTP era %d) from %s\n",
            res.year, res.month, res.day, res.hour, res.minute, res.second,
            (uint32_t)res.unix_ts, res.era, server);

    sys_set_rtc_time(((uint64_t)res.hour << 16) | ((uint64_t)res.minute << 8) |
                     (uint64_t)res.second);
    sys_set_rtc_date(((uint64_t)res.year << 16) | ((uint64_t)res.month << 8) |
                     (uint64_t)res.day);

    if (out) *out = res;
    return SNTP_OK;
}

// Public entry point. NULL/"" means "use the built-in default list"; anything
// else is queried on its own. See SNTP_FALLBACK_* in sntp.h for why the default
// is a list.
//
// The caller's budget is the budget: with a list, each server gets an equal
// share of it, so a wizard that asked for 5 seconds waits 5 seconds even if
// every server is dead. The floor keeps a share from becoming uselessly short
// if someone passes a tiny timeout.
int sntp_sync(const char *server, uint32_t timeout_ms, sntp_result_t *out) {
    if (timeout_ms == 0) timeout_ms = SNTP_DEFAULT_TIMEOUT_MS;
    // Cap: a Ring-3 caller does not get to park a kernel thread indefinitely.
    if (timeout_ms > 30000u) timeout_ms = 30000u;

    if (server && server[0]) return sntp_sync_one(server, timeout_ms, out);

    static const char *const defaults[SNTP_DEFAULT_COUNT] = {
        SNTP_DEFAULT_SERVER, SNTP_FALLBACK_1, SNTP_FALLBACK_2
    };
    uint32_t share = timeout_ms / SNTP_DEFAULT_COUNT;
    if (share < 1200u) share = 1200u;

    int last = SNTP_E_TIMEOUT;
    for (int i = 0; i < SNTP_DEFAULT_COUNT; i++) {
        int rc = sntp_sync_one(defaults[i], share, out);
        if (rc == SNTP_OK) return SNTP_OK;
        last = rc;
        // A local failure (no carrier, no IP, busy, broken validator) is about
        // US, not about this server: trying the next one cannot help and would
        // just burn the user's budget. Only a per-server failure is worth
        // moving on from.
        if (rc == SNTP_E_NOLINK || rc == SNTP_E_NONET ||
            rc == SNTP_E_BUSY   || rc == SNTP_E_SELFTEST) break;
    }
    return last;
}
