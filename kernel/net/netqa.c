// netqa.c - gated end-to-end network census smoke test (harness).
//
// No-op unless /CONFIG/NETQA.RUN is present on the boot filesystem. When armed,
// runs ONCE in its own worker (spawned from net_start_worker after the
// scheduler is live), exercising the stack top to bottom and logging a [NETQA]
// block to serial and the durable /BOOTLOG.TXT:
//     DNS resolve -> ICMP echo (gateway + public) -> HTTP GET -> HTTPS GET.
// This is a permanent, reusable smoke test that produces grounded evidence for
// a QA census without GUI mouse automation (#334) or the serial shell (which is
// unreachable once a framebuffer session exists, main.c login gate).
//
// SAME-SUBNET NON-GATEWAY PEER TEST (net-onlink): if /CONFIG/NETQA.RUN carries a
// line "peer=A.B.C.D:PORT", the census also drives an ICMP echo AND a raw TCP
// connect to that on-link, non-gateway host. This grounds the outbound path for
// a same-subnet peer whose MAC is cold at boot (only the gateway MAC is warm):
// route/next-hop decision -> ARP-for-the-peer -> SYN on the wire. It logs the
// on-link classification, the ARP cache state before and after, the ICMP RTT,
// and the TCP connect result, so a packet capture on the guest NIC can be tied
// to a serial verdict. No hardcoded LAN address ships: the peer comes from the
// armed file only.
//
// STACK SIZE (learned the hard way): the worker runs the full https_get / TLS
// handshake / X.509 chain / crypto call tree, which is deep. The default 16 KB
// proc_create() kernel stack OVERFLOWED inside the TLS handshake, corrupting
// rsp below the stack base and tripping the #75 SCHEDRACE detector (panic). It
// is created with a 256 KB stack via proc_create_ex(), the same size the
// desktop session uses for its comparably heavy WM+decoder call tree.
//
// LANGUAGE (Rust-first policy, CLAUDE.md): C, deliberately. This is diagnostic
// scaffolding that calls the C net API directly (dns_resolve / icmp_ping /
// icmp_get_ping_reply / wget_fetch / https_get, all C string+pointer
// interfaces) and is gated to no-op on every shipping golden, exactly like the
// existing C self-tests wakeloss.c, sync/waitq_test.c and the fdguard launcher.
// A Rust port would wrap every C net entry point in FFI for zero benefit on a
// harness that never runs in production.
//
// WAITING (CLAUDE.md #426, concurrency-lint): no poll loops. dns_resolve(),
// wget_fetch() and https_get() block correctly on their own wait-queues. ICMP
// has no block-until-reply primitive, so each echo is a send + a SINGLE fixed
// settle sleep + one reply check (unrolled, never a proc_sleep loop). The
// net-up settle is two sequential fixed proc_sleep() calls, not a condition
// poll. The TCP connect settle is a fixed unrolled count of driver-pump +
// sleep steps with the state read ONCE afterwards, so there is no condition
// checked inside a loop either.

#include "net.h"
#include "ip.h"
#include "dns.h"
#include "icmp.h"
#include "https.h"
#include "wget.h"
#include "tcp.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"
#include "../mm/heap.h"
#include "../proc/process.h"

extern fat_fs_t g_fat_fs;
extern int arp_lookup_cached(uint32_t ip, uint8_t *mac);
extern void net_poll(void);

// Peer parsed from /CONFIG/NETQA.RUN "peer=A.B.C.D:PORT" (0 => not armed).
static uint32_t netqa_peer_ip = 0;
static uint16_t netqa_peer_port = 0;
// Optional ASCOM Alpaca stub host from "alpaca=A.B.C.D:PORT" (0 => not armed).
static uint32_t netqa_alpaca_ip = 0;
static uint16_t netqa_alpaca_port = 0;

// One ICMP echo with a bounded, loop-free reply wait. Returns rtt ms (>=0) on a
// reply, -1 on timeout. Two unrolled attempts; each is send + fixed settle +
// single check, so no proc_sleep poll loop exists for the lint to flag.
static int netqa_ping_once(uint32_t ip) {
    uint32_t src; uint16_t seq, tms;
    if (icmp_ping(ip) >= 0) {
        proc_sleep(700);
        if (icmp_get_ping_reply(&src, &seq, &tms)) return (int)tms;
    } else {
        proc_sleep(500);   // ARP may be pending; settle then retry the send once
    }
    if (icmp_ping(ip) >= 0) {
        proc_sleep(900);
        if (icmp_get_ping_reply(&src, &seq, &tms)) return (int)tms;
    }
    return -1;
}

// Fetch via the plain-HTTP path (wget_fetch, port 80) or the TLS path
// (https_get, port 443) depending on tls. Both share the (url,body,len,status)
// C signature.
static void netqa_fetch(const char *url, const char *label, int tls) {
    uint8_t *body = 0; uint32_t len = 0; int status = 0;
    int rc = tls ? https_get(url, &body, &len, &status)
                 : wget_fetch(url, &body, &len, &status);
    if (rc == 0) {
        char first[81]; uint32_t n = 0;
        if (body) {
            while (n < len && n < 80 && body[n] != 0x0a && body[n] != 0x0d) {
                char c = (char)body[n];
                first[n] = (c >= 32 && c < 127) ? c : 0x2e;
                n++;
            }
        }
        first[n] = 0;
        kprintf("[NETQA] %s %s -> OK HTTP %d, %u bytes, first=\"%s\"\n",
                label, url, status, (unsigned)len, first);
        bootlog_write("[NETQA] %s -> OK HTTP %d %u bytes", label, status, (unsigned)len);
    } else {
        const char *es = tls ? https_strerror(rc) : "see wget";
        kprintf("[NETQA] %s %s -> FAIL rc=%d (%s)\n", label, url, rc, es);
        bootlog_write("[NETQA] %s -> FAIL rc=%d", label, rc);
    }
    if (body) kfree(body);
}

// Whether the raw-connect probe ever observed ESTABLISHED. Latched across the
// unrolled settle steps so a short-lived connection (peer replies then closes
// immediately) is still reported as a success rather than as the CLOSED/closing
// state it has raced into by the time the last step runs.
static int netqa_conn_established_seen = 0;

// One UNROLLED settle step for the raw TCP connect probe (called explicitly, not
// in a loop): drive the RX drain (arp_flush_ready runs inside net_poll once a
// MAC is freshly cached) and the TCP retransmit timer, latch an ESTABLISHED
// sighting, then yield.
static void netqa_conn_settle(int sock) {
    net_poll();
    tcp_timer();
    if (tcp_get_state(sock) == TCP_STATE_ESTABLISHED) netqa_conn_established_seen = 1;
    proc_sleep(250);
}

// net-onlink: drive ICMP + a raw outbound TCP connect to a same-subnet,
// NON-gateway peer whose MAC is cold at boot. Emits a [NETQA-ONLINK] verdict.
static void netqa_onlink_test(void) {
    if (netqa_peer_ip == 0 || netqa_peer_port == 0) {
        kprintf("[NETQA-ONLINK] no peer armed (NETQA.RUN has no peer= line); skipping\n");
        bootlog_write("[NETQA-ONLINK] no peer armed");
        return;
    }
    uint32_t peer = netqa_peer_ip;
    uint8_t *pp = (uint8_t *)&peer;
    uint32_t me = ip_get_address();
    uint32_t nm = ip_get_netmask();
    uint32_t gw = ip_get_gateway();
    int onlink = (nm == 0) ? 1 : ((peer & nm) == (me & nm));
    int is_gw = (peer == gw);
    uint8_t mac[6];
    int cached_before = arp_lookup_cached(peer, mac);
    kprintf("[NETQA-ONLINK] peer=%d.%d.%d.%d:%u onlink=%d is_gw=%d nm=0x%08x cached_before=%d\n",
            pp[3], pp[2], pp[1], pp[0], (unsigned)netqa_peer_port,
            onlink, is_gw, nm, cached_before);
    bootlog_write("[NETQA-ONLINK] peer=%d.%d.%d.%d:%u onlink=%d is_gw=%d cached_before=%d",
            pp[3], pp[2], pp[1], pp[0], (unsigned)netqa_peer_port, onlink, is_gw, cached_before);

    // ===== A) COLD raw TCP connect (#809 cold/warm reconciliation) =====
    // The SYN is the VERY FIRST packet this stack sends toward the peer: NO
    // ICMP ping, NO fetch, nothing has warmed the ARP cache. This is the exact
    // case the Alpaca client hits on a fresh boot (SYS_CONNECT -> tcp_connect)
    // and the case the earlier netqa probe could NOT see, because it ran the
    // ICMP echo below FIRST and that echo resolved (and cached) the peer's MAC,
    // so the "TCP connect" leg was always warm. Here the connect drives the
    // on-demand ARP itself: ip_send() -> arp_resolve() emits who-has, queues the
    // SYN (arp_queue_pending, #333), and arp_flush_ready()/tcp_timer put it on
    // the wire once the reply lands. cached_before MUST read 0 for this to be a
    // genuine cold test; the verdict is logged next to cached_after so a warm
    // false-negative is impossible to mistake for a pass.
    {
        uint8_t cmac[6];
        int cold_cached_before = arp_lookup_cached(peer, cmac);
        kprintf("[NETQA-ONLINK] COLD-TCP cached_before_connect=%d (0=genuinely cold)\n",
                cold_cached_before);
        bootlog_write("[NETQA-ONLINK] COLD-TCP cached_before=%d", cold_cached_before);
        int cs = tcp_socket();
        if (cs >= 0) {
            int ccr = tcp_connect(cs, peer, netqa_peer_port);
            kprintf("[NETQA-ONLINK] COLD-TCP tcp_connect rc=%d state0=%d\n",
                    ccr, (int)tcp_get_state(cs));
            netqa_conn_established_seen = 0;
            netqa_conn_settle(cs); netqa_conn_settle(cs); netqa_conn_settle(cs);
            netqa_conn_settle(cs); netqa_conn_settle(cs); netqa_conn_settle(cs);
            netqa_conn_settle(cs); netqa_conn_settle(cs); netqa_conn_settle(cs);
            netqa_conn_settle(cs); netqa_conn_settle(cs); netqa_conn_settle(cs);
            tcp_state_t cst = tcp_get_state(cs);
            int cold_est = netqa_conn_established_seen || (cst == TCP_STATE_ESTABLISHED);
            int cold_cached_after = arp_lookup_cached(peer, cmac);
            kprintf("[NETQA-ONLINK] COLD-TCP verdict: state=%d established=%s cached_after=%d\n",
                    (int)cst, cold_est ? "YES" : "NO", cold_cached_after);
            bootlog_write("[NETQA-ONLINK] COLD-TCP state=%d established=%d cached_after=%d",
                    (int)cst, cold_est, cold_cached_after);
            tcp_close(cs);
        } else {
            kprintf("[NETQA-ONLINK] COLD-TCP tcp_socket failed (%d)\n", cs);
            bootlog_write("[NETQA-ONLINK] COLD-TCP tcp_socket failed");
        }
    }

    // 1) ICMP echo to the on-link non-gateway peer. Exercises the exact
    //    ip_send() -> next-hop -> arp_resolve() chokepoint the SYN uses. NOTE:
    //    the COLD-TCP probe above has now (if it worked) warmed the peer's ARP,
    //    so this leg and the WARM-TCP leg below are the A/B WARM comparison.
    int rtt = netqa_ping_once(peer);
    int cached_after = arp_lookup_cached(peer, mac);
    kprintf("[NETQA-ONLINK] ICMP peer rtt=%d ms | ARP cached_after=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
            rtt, cached_after, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    bootlog_write("[NETQA-ONLINK] ICMP peer rtt=%d arp_cached_after=%d", rtt, cached_after);

    // 2) WARM raw outbound TCP connect to the on-link non-gateway peer:port,
    //    now that the ICMP echo (and the COLD-TCP probe) have resolved the peer.
    //    This is the leg the ORIGINAL netqa reported, and it is warm by
    //    construction; kept as the A/B control against COLD-TCP above.
    int s = tcp_socket();
    if (s >= 0) {
        int cr = tcp_connect(s, peer, netqa_peer_port);
        kprintf("[NETQA-ONLINK] WARM-TCP tcp_connect rc=%d state0=%d\n", cr, (int)tcp_get_state(s));
        netqa_conn_established_seen = 0;
        netqa_conn_settle(s); netqa_conn_settle(s); netqa_conn_settle(s);
        netqa_conn_settle(s); netqa_conn_settle(s); netqa_conn_settle(s);
        netqa_conn_settle(s); netqa_conn_settle(s); netqa_conn_settle(s);
        netqa_conn_settle(s); netqa_conn_settle(s); netqa_conn_settle(s);
        tcp_state_t st = tcp_get_state(s);
        int est = netqa_conn_established_seen || (st == TCP_STATE_ESTABLISHED);
        kprintf("[NETQA-ONLINK] WARM-TCP connect verdict: state=%d established=%s\n",
                (int)st, est ? "YES" : "NO");
        bootlog_write("[NETQA-ONLINK] WARM-TCP state=%d established=%d", (int)st, est);
        tcp_close(s);
    } else {
        kprintf("[NETQA-ONLINK] WARM-TCP tcp_socket failed (%d)\n", s);
        bootlog_write("[NETQA-ONLINK] WARM-TCP tcp_socket failed");
    }

    // 3) COLD Alpaca-shaped HTTP round-trip (optional). If NETQA.RUN carried an
    //    "alpaca=A.B.C.D:PORT" line, GET the ASCOM Alpaca discovery endpoint on a
    //    FRESH socket. Uses wget_fetch (the userland HTTP path the Alpaca panel
    //    drives). The alpaca host is normally the SAME same-subnet peer; its ARP
    //    is warm by now, so this proves the HTTP request/response layer, not cold
    //    ARP (COLD-TCP above owns the cold-ARP proof).
    if (netqa_alpaca_ip) {
        uint8_t *ap = (uint8_t *)&netqa_alpaca_ip;
        char url[96];
        snprintf(url, sizeof(url), "http://%d.%d.%d.%d:%u/management/apiversions",
                 ap[3], ap[2], ap[1], ap[0], (unsigned)netqa_alpaca_port);
        netqa_fetch(url, "ALPACA-APIVER", 0);
        snprintf(url, sizeof(url), "http://%d.%d.%d.%d:%u/api/v1/telescope/0/name",
                 ap[3], ap[2], ap[1], ap[0], (unsigned)netqa_alpaca_port);
        netqa_fetch(url, "ALPACA-NAME", 0);
    }
}

static void netqa_worker(void *arg) {
    (void)arg;
    kprintf("[NETQA] ===== network census smoke test starting =====\n");
    bootlog_write("[NETQA] census start");

    // Net-up settle: DHCP DORA completes a few seconds into boot. Two fixed
    // sleeps (no condition poll). If still down after both, report and abort.
    proc_sleep(12000);
    if (!(net_is_up() && ip_get_address() != 0)) proc_sleep(12000);

    uint32_t ip = ip_get_address(), gw = ip_get_gateway();
    uint8_t *pi = (uint8_t *)&ip, *pg = (uint8_t *)&gw;
    int up = (net_is_up() && ip != 0);
    kprintf("[NETQA] link=%s net_is_up=%d ip=%d.%d.%d.%d gw=%d.%d.%d.%d\n",
            nic_link_up() ? "UP" : "DOWN", net_is_up(),
            pi[3], pi[2], pi[1], pi[0], pg[3], pg[2], pg[1], pg[0]);
    bootlog_write("[NETQA] link=%s ip=%d.%d.%d.%d gw=%d.%d.%d.%d",
            nic_link_up() ? "UP" : "DOWN",
            pi[3], pi[2], pi[1], pi[0], pg[3], pg[2], pg[1], pg[0]);
    if (!up) {
        kprintf("[NETQA] interface never came up; aborting (rig/link issue, not a stack bug)\n");
        bootlog_write("[NETQA] ABORT interface down");
        return;
    }

    // 1) DNS (blocking)
    uint32_t dip = 0;
    int dr = dns_resolve("example.com", &dip);
    uint8_t *pd = (uint8_t *)&dip;
    if (dr == 0) {
        kprintf("[NETQA] DNS example.com -> %d.%d.%d.%d OK\n", pd[3], pd[2], pd[1], pd[0]);
        bootlog_write("[NETQA] DNS example.com -> %d.%d.%d.%d", pd[3], pd[2], pd[1], pd[0]);
    } else {
        kprintf("[NETQA] DNS example.com -> FAIL rc=%d\n", dr);
        bootlog_write("[NETQA] DNS example.com -> FAIL rc=%d", dr);
    }

    // 2) ICMP: gateway (on-net) then 1.1.1.1 (routed via gateway)
    int gms = netqa_ping_once(gw);
    int cms = netqa_ping_once(0x01010101u); // 1.1.1.1 in host byte order
    kprintf("[NETQA] PING gateway %d.%d.%d.%d -> %d ms | PING 1.1.1.1 -> %d ms (-1=timeout)\n",
            pg[3], pg[2], pg[1], pg[0], gms, cms);
    bootlog_write("[NETQA] PING gw=%dms 1.1.1.1=%dms", gms, cms);

    // 2b) net-onlink: same-subnet NON-gateway peer (ICMP + raw TCP connect).
    netqa_onlink_test();

    // 3) HTTP (plain, port 80, wget path)  4) HTTPS (port 443, TLS path).
    // Small responses only, per the large-response-is-the-enemy rule.
    netqa_fetch("http://example.com/", "HTTP ", 0);
    netqa_fetch("https://example.com/", "HTTPS", 1);

    kprintf("[NETQA] ===== network census smoke test complete =====\n");
    bootlog_write("[NETQA] census complete");
}

// Parse an optional "peer=A.B.C.D:PORT" directive out of the armed file's
// contents. Tolerant: ignores everything else; leaves netqa_peer_* zero if
// absent or malformed. host byte order for the IP (high byte = first octet),
// matching ip_get_address() and the shell's ping parser.
// Parse "A.B.C.D:PORT" out of [q,le). Returns 1 and sets *ip_out (host order)
// and *port_out on success; 0 if malformed.
static int netqa_parse_ipport(const char *q, const char *le,
                              uint32_t *ip_out, uint16_t *port_out) {
    uint32_t oct[4] = {0,0,0,0};
    int oi = 0, val = 0, seen = 0;
    uint32_t port = 0; int in_port = 0;
    while (q < le) {
        char c = *q++;
        if (c >= '0' && c <= '9') {
            if (in_port) port = port*10 + (uint32_t)(c-'0');
            else { val = val*10 + (c-'0'); seen = 1; }
        } else if (c == '.') {
            if (oi < 3) oct[oi++] = (uint32_t)(val & 0xFF);
            val = 0;
        } else if (c == ':') {
            if (oi == 3) oct[oi++] = (uint32_t)(val & 0xFF);
            val = 0; in_port = 1;
        } else {
            break;
        }
    }
    if (!in_port && oi == 3 && seen) oct[oi++] = (uint32_t)(val & 0xFF);
    if (oi != 4) return 0;
    *ip_out = (oct[0]<<24)|(oct[1]<<16)|(oct[2]<<8)|oct[3];
    *port_out = (uint16_t)(port ? port : 9999);
    return 1;
}

static void netqa_parse_peer(const char *buf, uint32_t len) {
    const char *end = buf + len;
    const char *p = buf;
    while (p < end) {
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        const char *le = p;
        if (p < end) p++;   // skip newline
        if (le - ls >= 5 && ls[0]=='p' && ls[1]=='e' && ls[2]=='e' && ls[3]=='r' && ls[4]=='=') {
            netqa_parse_ipport(ls + 5, le, &netqa_peer_ip, &netqa_peer_port);
        } else if (le - ls >= 7 && ls[0]=='a' && ls[1]=='l' && ls[2]=='p' && ls[3]=='a' &&
                   ls[4]=='c' && ls[5]=='a' && ls[6]=='=') {
            netqa_parse_ipport(ls + 7, le, &netqa_alpaca_ip, &netqa_alpaca_port);
        }
    }
}

// Armed only by the presence of /CONFIG/NETQA.RUN. Called from
// net_start_worker() after the scheduler is live. The worker gets a 256 KB
// stack (proc_create_ex) because the https_get/TLS call tree overflows the
// 16 KB proc_create default (see the file header).
void netqa_start(void) {
    uint32_t sz = 0;
    if (!g_fat_fs.mounted) return;
    void *m = fat_read_file(&g_fat_fs, "/CONFIG/NETQA.RUN", &sz);
    if (!m) return;
    netqa_parse_peer((const char *)m, sz);
    kfree(m);
    if (netqa_peer_ip) {
        uint8_t *pp = (uint8_t *)&netqa_peer_ip;
        kprintf("[NETQA] armed with on-link peer %d.%d.%d.%d:%u\n",
                pp[3], pp[2], pp[1], pp[0], (unsigned)netqa_peer_port);
    }
    extern int proc_create_ex(const char *name, void (*entry)(void *), void *arg,
                              process_priority_t priority, uint32_t stack_size);
    int pid = proc_create_ex("netqa", netqa_worker, (void *)0, PRIO_LOW, 256 * 1024);
    kprintf("[NETQA] gate ON (/CONFIG/NETQA.RUN present); worker pid=%d (256KB stack)\n", pid);
    bootlog_write("[NETQA] gate ON worker pid=%d", pid);
}
