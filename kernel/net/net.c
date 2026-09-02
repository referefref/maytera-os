// net.c - Network stack initialization
#include "net.h"
#include "e1000.h"
#include "virtio_net.h"
#include "ethernet.h"
#include "arp.h"
#include "ip.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "dhcp.h"
#include "dns.h"
#include "firewall.h"   // #238: packet filter boot load
#include "https.h"
#include "ftp.h"
#include "wget.h"
#include "../crypto/crypto.h"
#include "../drivers/pci.h"
#include "../drivers/usb_net.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"   // hot-plug attach evidence goes to the DURABLE log
#include "../mm/heap.h"
#include "../proc/process.h"   // #381: proc_create/proc_sleep/PRIO_* for net worker
#include "../cpu/dlprof.h"
#include "../cpu/mono.h"   // #549: mono_ms() paces the FAULTY re-probe
#include "irqwin.h"        // #69: per-site interrupts-off window accounting

// #69: the NIC CR3 window. THIRD copy of this idiom in the tree (proc/syscall.c
// net_cr3_enter/exit and net/socket.c sk_cr3_enter/exit are the other two); the
// chunked drain below is the one that unifies the drain callers onto it.
// NIC MMIO/DMA lives in the kernel lower-half identity map, which a user
// process CR3 does not have, so a drain reached from a syscall must run on the
// kernel pml4. A caller already on the kernel pml4 rewrites the same value.
extern uint64_t vmm_get_pml4(void);
static inline uint64_t net_drain_cr3_enter(void) {
    uint64_t saved;
    __asm__ volatile("mov %%cr3, %0" : "=r"(saved));
    uint64_t kcr3 = vmm_get_pml4();
    __asm__ volatile("mov %0, %%cr3" : : "r"(kcr3) : "memory");
    return saved;
}
static inline void net_drain_cr3_exit(uint64_t saved) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(saved) : "memory");
}

static int net_initialized = 0;
static net_driver_type_t active_driver = NET_DRIVER_NONE;

// Set to 1 when a valid /CONFIG/NETIP.CFG was applied during net_init().
// main.c checks this so it can skip the default static/DHCP path and leave
// the file-provided static configuration in place.
int g_net_static_configured = 0;

// ---------------------------------------------------------------------------
// #549: CONNECTIVITY CIRCUIT-BREAKER  (fail safe and quiet, never busy-retry)
//
// THE BUG (golden 860, measured on the real iMac): a BARE desktop burned ~60% of
// core 0. The iMac's DHCP found no server, so net_worker adopted the static
// fallback 192.0.2.1 / gw 192.0.2.1 (that exact pair == the .200-.209 /
// gw .1 fallback below, NOT a lease). That gateway resolves at L2 but nothing
// beyond it answers - the real internet is only via a different ICS box. The two
// autostarted background fetchers (haservice polling Home Assistant every 10s,
// netinfo polling weather/crypto) therefore never reach their targets. Each
// failed fetch drives DNS/SYN retransmits; on the iMac's USB Ethernet dongle
// every single send busy-polls the xHCI up to 40ms (usbnet_bulk_out), so the
// retry storm pegged a core. (On an e1000 VM the same sends are cheap MMIO, which
// is why VM <vmid> and an e1000 repro stay idle - the spin is USB-amplified.)
//
// THE FIX (the user's stated design): DETECT persistent unreachability, then
// FAIL SAFE AND QUIET - mark the interface NET_STATE_FAULTY, make net_is_up()
// report DOWN, and stop. Every background client already gates on net_is_up()
// (haservice sleeps 5s, netinfo backs off 30..480s), so they quiesce to ~0 CPU
// with no client-side change. Recovery is explicit and non-bricking: a carrier
// down->up, Settings applying a static IP, an explicit reconnect, or any fetch
// that actually completes, all clear the fault.
//
// The trip signal is TRANSPORT-level reach failures (no server ever answered),
// counted globally with an OK reset. It does NOT trip on HTTP 4xx/5xx (the server
// WAS reached) and it does NOT trip when ANY connectivity exists: on a box with
// working internet, netinfo's fetches succeed and reset the streak even if HA is
// down, so "internet up, only HA down" never false-faults. It trips only when
// EVERY remote attempt fails NET_FAIL_STREAK_MAX times in a row.
//
// Kept in C (this file is the net hot path; no float, tiny integer state machine)
// - a Rust port here would buy nothing and cross the FFI on every fetch.
// ---------------------------------------------------------------------------
#define NET_FAIL_STREAK_MAX 6      // consecutive transport failures -> FAULTY

// #549 (recovery fixed 2026-08-10): while FAULTY, exactly ONE fetch per this
// interval is let onto the wire as a re-probe. See net_fetch_probe_take().
#define NET_PROBE_INTERVAL_MS 30000

static volatile net_conn_state_t g_net_conn_state = NET_STATE_UP;
static volatile uint32_t g_net_fail_streak = 0;
static volatile uint64_t g_net_probe_next_ms = 0;   // earliest permitted re-probe

// ---------------------------------------------------------------------------
// #549 OBSERVABILITY (browsenet, 2026-09-01). MEASURED MOTIVE, not a nicety.
//
// Every transition in this breaker was kprintf-only, i.e. SERIAL ONLY. The
// machine it matters on is the owner's ASUS laptop, which has no serial port.
// Golden 2317's /BOOTLOG.TXT from that laptop records state=FAULTY twice and
// NOT ONE byte about WHEN it tripped, WHAT failed, whether a re-probe was ever
// granted, or to whom. So the single most likely explanation for "the browser
// cannot load example.com while ping 1.1.1.1 works" was un-diagnosable from the
// only evidence that machine can produce.
//
// This is the third diagnostic this quarter that could not reach the machine it
// was written for; net/dhcp.c and main.c both carry the same lesson in prose.
// The counters below are rendered by net_diag_line(), which main.c ALREADY
// persists to /BOOTLOG.TXT on every qualitative change and on a slow periodic
// sample, so they cost no additional write and no new plumbing. trip= sits in
// the QUALITATIVE half deliberately: it changes exactly once per trip, which is
// precisely when a durable line is worth spending.
static volatile uint32_t g_net_fail_total    = 0;  // reach-failures ever counted
static volatile uint32_t g_net_trip_count    = 0;  // times FAULTY was entered
static volatile uint32_t g_net_recover_count = 0;  // times a transfer cleared it
static volatile uint32_t g_net_probe_grants  = 0;  // re-probes let onto the wire
static volatile uint32_t g_net_probe_refused = 0;  // fetches refused while FAULTY
static char g_net_fail_host[40];                   // most recent failing host
static char g_net_trip_host[40];                   // host that COMPLETED the streak
// browsenet 2026-09-01: the FAILURE CODE too, because the classes are not
// equivalent and the fix differs by class. wget.h's codes: -1 invalid URL,
// -2 no network, -3 DNS failed, -4 connect failed, -5 send failed, -6 timeout,
// -7 no memory, -12 SSRF-blocked. https_get collapses most of its failures to
// a bare -1. Several of those (-1, -7, and https.c:214 "Failed to create
// socket" under TCP socket-pool exhaustion) are evidence about US, not about
// the remote, and #549's own 2026-08-10 note says such a failure must not be
// counted; that fix excluded only the no-carrier/no-address case. Recording
// the code is the cheap half: it turns "what tripped it" from a guess into a
// number in the next boot log, without changing any behaviour.
static volatile int g_net_fail_rc = 0;             // rc of the most recent failure
static volatile int g_net_trip_rc = 0;             // rc that COMPLETED the streak

// Copy the host out of a URL ("scheme://host[:port]/...") into dst. No strstr
// here (freestanding); this is a bounded hand walk on purpose.
static void net_note_host(char *dst, unsigned long cap, const char *url) {
    unsigned long n = 0;
    if (!dst || cap < 2) return;
    if (!url || !url[0]) { dst[0] = 0; return; }
    const char *h = url;
    for (const char *q = url; q[0] && q[1] && q[2]; q++) {
        if (q[0] == ':' && q[1] == '/' && q[2] == '/') { h = q + 3; break; }
    }
    while (*h && *h != '/' && *h != ':' && *h != '?' && n < cap - 1) dst[n++] = *h++;
    dst[n] = 0;
}

// Read-only accessors for net_diag_line() and any future Settings panel.
uint32_t net_fault_fail_total(void)    { return g_net_fail_total; }
uint32_t net_fault_trip_count(void)    { return g_net_trip_count; }
uint32_t net_fault_recover_count(void) { return g_net_recover_count; }
uint32_t net_fault_probe_grants(void)  { return g_net_probe_grants; }
uint32_t net_fault_probe_refused(void) { return g_net_probe_refused; }
int      net_fault_trip_rc(void)       { return g_net_trip_rc; }
const char *net_fault_trip_host(void)  { return g_net_trip_host[0] ? g_net_trip_host : "-"; }


void net_report_reach_ok(void) {
    g_net_fail_streak = 0;
    g_net_probe_next_ms = 0;
    if (g_net_conn_state != NET_STATE_UP) {
        g_net_conn_state = NET_STATE_UP;
        g_net_recover_count++;
        kprintf("[NET] a transfer completed; interface re-enabled (was NET_FAULTY)\n");
    }
}

void net_report_reach_fail_rc(const char *url, int rc) {
    g_net_fail_rc = rc;
    net_report_reach_fail_url(url);
}

void net_report_reach_fail_url(const char *url) {
    // browsenet 2026-09-01: remember WHICH remote failed. Without this the trip
    // is a bare state change and the next reader has to guess whether the box
    // has no internet or one background poller has an unreachable target.
    net_note_host(g_net_fail_host, sizeof(g_net_fail_host), url);
    if (g_net_conn_state == NET_STATE_FAULTY) return;   // already tripped; stay quiet
    // #549 FIX (2026-08-10): only count a failure that actually REACHED THE WIRE.
    // wget_fetch()/https_connect() return WGET_ERR_NO_NETWORK the instant
    // net_is_up() is false (no carrier, no address yet, stack not initialised),
    // and net_fetch_report() turned that r<0 into a reach-failure. That is
    // evidence about US, not about the remote: it let a box accumulate the whole
    // trip streak during the pre-DHCP window without ever emitting a packet, and
    // the resulting FAULTY state then outlived the condition that caused it.
    if (!net_wire_usable()) return;
    if (g_net_fail_streak < 0xFFFFFFFFu) g_net_fail_streak++;
    if (g_net_fail_total < 0xFFFFFFFFu) g_net_fail_total++;
    if (g_net_fail_streak >= NET_FAIL_STREAK_MAX) {
        g_net_conn_state = NET_STATE_FAULTY;
        g_net_probe_next_ms = mono_ms() + NET_PROBE_INTERVAL_MS;
        g_net_trip_count++;
        for (unsigned i = 0; i < sizeof(g_net_trip_host); i++)
            g_net_trip_host[i] = g_net_fail_host[i];
        g_net_trip_host[sizeof(g_net_trip_host) - 1] = 0;
        g_net_trip_rc = g_net_fail_rc;
        kprintf("[NET] %u consecutive unreachable-remote failures (last host '%s' rc=%d): "
                "marking interface NET_FAULTY. Bulk traffic stopped; one re-probe "
                "every %ums until a transfer completes.\n",
                (unsigned)g_net_fail_streak,
                g_net_trip_host[0] ? g_net_trip_host : "?",
                g_net_trip_rc,
                (unsigned)NET_PROBE_INTERVAL_MS);
    }
}

// Back-compat entry point for call sites with no URL in scope.
void net_report_reach_fail(void) { net_report_reach_fail_url(0); }

net_conn_state_t net_get_conn_state(void) { return g_net_conn_state; }
int net_is_faulty(void) { return g_net_conn_state == NET_STATE_FAULTY; }

// #549 FIX (2026-08-10): the one way out of NET_FAULTY that needs no human.
//
// THE BUG THIS CLOSES. The FAULTY gate in sys_http_fetch*/sys_http_post* refused
// every fetch before it started, but the only automatic clear
// (net_report_reach_ok) fires when a fetch COMPLETES. The gate therefore
// suppressed the only evidence that could clear the gate: the state was
// SELF-LATCHING. With the link continuously up, no path back existed short of a
// reboot or a manual Settings reconfigure, even though the header comment above
// claimed "any fetch that actually completes" would clear it. That claim was
// false by construction. Meanwhile ICMP and cached DNS were never gated, so the
// box looked healthy (link, IP, gateway, DNS all green) while every HTTP request
// was refused: exactly the first-boot wizard report that prompted this fix.
//
// THIS IS NOT "A TIMER THAT HOPES". The timer paces only how often we are
// allowed to GATHER evidence. The fault clears solely in net_report_reach_ok(),
// i.e. only when a transfer actually completed. A probe that fails changes
// nothing and the interface stays FAULTY, so a genuinely dead uplink still
// stays quiesced forever.
//
// IT KEEPS #549's PROTECTION. What #549 fixed was a RETRY STORM: background
// pollers retrying continuously, where every send busy-polls the iMac dongle's
// xHCI up to 40ms in usbnet_bulk_out and pegged a core. One probe per 30s is a
// ~1000x smaller duty cycle than that storm, so the CPU win is preserved while
// the dead end is removed.
//
// Returns 1 if the caller may put this request on the wire (consuming the probe
// budget when FAULTY), 0 if it must be refused.
int net_fetch_probe_take(void) {
    if (g_net_conn_state != NET_STATE_FAULTY) return 1;
    uint64_t now = mono_ms();
    if (now < g_net_probe_next_ms) {
        // browsenet 2026-09-01: COUNT the refusals. This is the number that
        // says whether the user's browser request is losing a race for the one
        // 30s token against the background pollers, or whether it is getting
        // onto the wire and failing there. Those are different tickets and
        // nothing distinguished them before.
        if (g_net_probe_refused < 0xFFFFFFFFu) g_net_probe_refused++;
        return 0;
    }
    g_net_probe_next_ms = now + NET_PROBE_INTERVAL_MS;
    if (g_net_probe_grants < 0xFFFFFFFFu) g_net_probe_grants++;
    kprintf("[NET] NET_FAULTY: allowing one re-probe onto the wire (grant #%u)\n",
            (unsigned)g_net_probe_grants);
    return 1;
}

void net_clear_fault(void) {
    g_net_fail_streak = 0;
    g_net_probe_next_ms = 0;
    if (g_net_conn_state != NET_STATE_UP) {
        g_net_conn_state = NET_STATE_UP;
        kprintf("[NET] fault cleared; interface re-enabled (manual reconnect)\n");
    }
}

// Driver abstraction - function pointers
static void (*driver_get_mac)(uint8_t *mac) = NULL;
static int (*driver_send)(const void *data, uint16_t length) = NULL;
static int (*driver_receive)(void *buffer, uint16_t buffer_size) = NULL;
static int (*driver_link_up)(void) = NULL;

// Forward declaration
extern void kprintf_set_dual_output(int enable);
extern fat_fs_t g_fat_fs;
static int nic_refresh_link(void);   // #381: defined below; used by net_init()

// ---------------------------------------------------------------------------
// HOT-PLUG NIC ATTACH (no ticket, 2026-08-28).
//
// net_init() binds a NIC exactly once, during boot. A USB Ethernet adapter
// plugged in later enumerated fine and the CDC-ECM driver claimed it and read
// its MAC, but the network stack never learned it existed: no DISCOVER, no
// OFFER, no lease, ever, because nothing on the xHCI re-scan path reaches
// net_init(). The measured artifact was an owner BOOTLOG with
// "[USB-ECM] CDC-ECM NIC attached" and NO "[USB-NET] ... active as NIC" line,
// which is usb_eth_start() never being called.
//
// The handoff is deliberately split across two threads:
//
//   ARM   drivers/usb_ecm.c usb_net_probe(), on the xhci_rescan worker, inside
//         device enumeration. Three atomic ops, nothing else. That context is
//         one control transfer away from every other device on the bus and
//         must never block (#549).
//
//   BIND  net_late_attach_service(), called at the top of net_worker()'s loop.
//         net_worker is the single owner of carrier polling and DHCP; it is a
//         normal PRIO_NORMAL thread and is allowed to be slow. Everything
//         expensive (usb_eth_start, the protocol-stack bring-up, the config
//         file reads) happens HERE.
//
// DHCP then starts with NO extra plumbing: net_worker's next statement is
// nic_refresh_link(), and a NIC that was absent has left prev_link at 0, so the
// existing #431 carrier down->up branch runs dhcp_reset() + dhcp_discover() in
// the SAME iteration. Carrier is handled by the same pre-existing code: an ASIX
// dongle with no cable reports link 0, so the worker takes its fast no-op path
// (#381) and DHCP waits for the cable rather than spinning or failing.
// ---------------------------------------------------------------------------
extern void netattach_boot_done_rs(void);
extern int  netattach_on_probe_rs(int nic_bound);
extern int  netattach_take_rs(void);
extern void netattach_on_detach_rs(void);
extern int  netattach_take_detach_rs(void);
extern void netattach_rearm_rs(void);
extern void netattach_stats_rs(uint64_t *probe, uint64_t *armed, uint64_t *taken,
                               uint64_t *skip_boot, uint64_t *skip_bound,
                               uint64_t *coalesced, uint64_t *pending,
                               uint64_t *detach, uint64_t *detach_taken);

// Is a NIC currently carrying the stack? The ONE definition of that fact; the
// Rust side is passed it rather than keeping its own copy.
int net_has_nic(void) {
    return active_driver != NET_DRIVER_NONE;
}

// Minimal, robust dotted-quad parser. Returns 1 on success and writes the
// address in host byte order (a.b.c.d -> (a<<24)|(b<<16)|(c<<8)|d), 0 on any
// malformed input. Does not depend on libc.
static int net_parse_ip(const char *s, uint32_t *out) {
    if (!s || !out) return 0;
    uint32_t parts[4];
    int pi = 0;
    int have_digit = 0;
    uint32_t cur = 0;
    while (1) {
        char c = *s++;
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (uint32_t)(c - '0');
            if (cur > 255) return 0;
            have_digit = 1;
        } else if (c == '.') {
            if (!have_digit || pi >= 3) return 0;
            parts[pi++] = cur;
            cur = 0;
            have_digit = 0;
        } else if (c == 0 || c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            // End of field/line: finish the final octet.
            if (!have_digit || pi != 3) return 0;
            parts[pi++] = cur;
            *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
            return 1;
        } else {
            return 0;
        }
    }
}

// Find a line "key=value" in a config buffer and parse its value as an IP.
// Tolerates leading whitespace and CRLF. Returns 1 if found+parsed.
static int net_cfg_find_ip(const char *buf, const char *key, uint32_t *out) {
    if (!buf || !key) return 0;
    size_t klen = 0;
    while (key[klen]) klen++;
    const char *p = buf;
    while (*p) {
        // Skip leading whitespace on the line.
        while (*p == ' ' || *p == '\t') p++;
        // Compare key at start of line followed by '='.
        size_t i = 0;
        while (i < klen && p[i] && p[i] == key[i]) i++;
        if (i == klen && p[klen] == '=') {
            return net_parse_ip(p + klen + 1, out);
        }
        // Advance to next line.
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
}

// Read a static network configuration file and, if it contains a valid config,
// apply it via ip_set_* (+ dns_set_server). Returns 1 if static IP was applied
// (DHCP/default should be skipped), 0 otherwise (file absent or invalid).
//
// Two locations are checked, in order (#549 - authoritative path first):
//   1. /CONFIG/NETIP.CFG (AUTHORITATIVE: written by Settings > Network on Apply;
//                          must win so a saved static config always takes effect)
//   2. /NETCFG.TXT       (legacy per-image fallback, only if no NETIP.CFG)
//
// Format (plain text, one key=value per line; mask/gw/dns optional):
//   ip=192.0.2.50
//   mask=255.255.255.0
//   gw=192.0.2.1
//   dns=1.1.1.1
//
// The address is set directly (no gratuitous ARP), so this is safe on any
// segment: it never announces or DAD-probes, so it cannot hijack a neighbor
// (the #380 concern was the AUTO .200 fallback claiming an in-use LAN address;
// a user-provided static assignment is trusted). The default DAD/carrier-gated
// .200 fallback in main.c/net_worker only runs when NO config file is present.
static int net_apply_static_config(void) {
    if (!g_fat_fs.mounted) return 0;
    uint32_t size = 0;
    // #549: /CONFIG/NETIP.CFG is the AUTHORITATIVE path and is checked FIRST.
    // Settings > Network writes this file on Apply (commit d4b9c16), so it MUST
    // win; the old order checked /NETCFG.TXT first, which would let a stale FAT
    // file silently override a user's saved static config. /NETCFG.TXT stays as a
    // legacy per-image fallback only.
    const char *src = "/CONFIG/NETIP.CFG";
    char *data = (char *)fat_read_file(&g_fat_fs, "/CONFIG/NETIP.CFG", &size);
    if (!data) {
        src = "/NETCFG.TXT";
        data = (char *)fat_read_file(&g_fat_fs, "/NETCFG.TXT", &size);
    }
    if (!data) {
        return 0;  // No config file: normal DHCP/default behavior.
    }

    uint32_t ip = 0, mask = 0, gw = 0, dns = 0;
    int have_ip   = net_cfg_find_ip(data, "ip", &ip);
    int have_mask = net_cfg_find_ip(data, "mask", &mask);
    int have_gw   = net_cfg_find_ip(data, "gw", &gw);
    int have_dns  = net_cfg_find_ip(data, "dns", &dns);

    kfree(data);

    // #786: a dns= line WITHOUT an ip= line is a legitimate, useful file: it
    // means "keep using DHCP for the address, but resolve through THIS
    // server". That is what Settings writes when the user changes only the
    // DNS on a DHCP machine, and it must NOT be discarded as malformed the way
    // the old "no ip= line; ignoring" path did. Apply the resolver, then return
    // 0 so the caller still runs DHCP.
    if (!have_ip) {
        if (have_dns) {
            dns_set_server(dns);
            uint8_t *pd0 = (uint8_t *)&dns;
            kprintf("[NET] %s: dns-only config, resolver %d.%d.%d.%d (DHCP still runs)\n",
                    src, pd0[3], pd0[2], pd0[1], pd0[0]);
        } else {
            kprintf("[NET] %s present but no valid ip= or dns= line; ignoring\n", src);
        }
        return 0;
    }
    if (!have_mask) mask = 0xFFFFFF00;          // 255.255.255.0
    if (!have_gw)   gw   = (ip & mask) | 1;     // x.x.x.1 fallback

    ip_set_address(ip);
    ip_set_netmask(mask);
    ip_set_gateway(gw);
    if (have_dns) dns_set_server(dns);
    net_clear_fault();   // #549: a fresh explicit config is a recovery event

    uint8_t *pi = (uint8_t *)&ip;
    uint8_t *pm = (uint8_t *)&mask;
    uint8_t *pg = (uint8_t *)&gw;
    uint8_t *pd = (uint8_t *)&dns;
    kprintf("[NET] static IP from %s: %d.%d.%d.%d mask %d.%d.%d.%d gw %d.%d.%d.%d",
            src,
            pi[3], pi[2], pi[1], pi[0],
            pm[3], pm[2], pm[1], pm[0],
            pg[3], pg[2], pg[1], pg[0]);
    if (have_dns)
        kprintf(" dns %d.%d.%d.%d", pd[3], pd[2], pd[1], pd[0]);
    kprintf("\n");
    return 1;
}

// ---------------------------------------------------------------------------
// #786: persist the LIVE configuration to /CONFIG/NETIP.CFG.
// ---------------------------------------------------------------------------
// See net.h for why this lives in Ring 0 rather than in Settings.
//
// Format is the one net_apply_static_config() above already parses, and it is
// generated from the SAME live accessors that NETDIAG and SYS_NET_STATUS read,
// so "what the panel shows", "what the stack is doing" and "what boots next
// time" cannot drift apart.
//
// This is a plain buffer format and a single fat_write_file(); it does not
// wait, take a lock, or touch the NIC, so it is safe from a syscall context.
// It is deliberately NOT called from the net worker or any IRQ path.
static void netcfg_put_quad(char **pp, uint32_t v) {
    // Host order: (a<<24)|(b<<16)|(c<<8)|d, matching net_cfg_find_ip().
    char *p = *pp;
    for (int shift = 24; shift >= 0; shift -= 8) {
        uint32_t byte = (v >> shift) & 0xFF;
        if (byte >= 100) *p++ = (char)('0' + byte / 100);
        if (byte >= 10)  *p++ = (char)('0' + (byte / 10) % 10);
        *p++ = (char)('0' + byte % 10);
        if (shift) *p++ = '.';
    }
    *pp = p;
}

static void netcfg_put_kv(char **pp, const char *key, uint32_t v) {
    char *p = *pp;
    while (*key) *p++ = *key++;
    *p++ = '=';
    *pp = p;
    netcfg_put_quad(pp, v);
    p = *pp;
    *p++ = '\n';
    *pp = p;
}

int net_persist_netcfg(void) {
    extern uint32_t ip_get_address(void);
    extern uint32_t ip_get_netmask(void);
    extern uint32_t ip_get_gateway(void);
    if (!g_fat_fs.mounted) return -2;

    char buf[128];
    char *p = buf;
    uint32_t dns = dns_get_server();

    // ONLY write an address block when the machine is actually statically
    // configured. On DHCP this writes a dns-only file (see net.h).
    if (g_net_static_configured) {
        uint32_t ip = ip_get_address();
        if (ip == 0) return -1;             // nothing coherent to persist
        netcfg_put_kv(&p, "ip",   ip);
        netcfg_put_kv(&p, "mask", ip_get_netmask());
        netcfg_put_kv(&p, "gw",   ip_get_gateway());
    }
    if (dns) netcfg_put_kv(&p, "dns", dns);

    if (p == buf) return -1;                // neither static nor a resolver
    if (fat_write_file(&g_fat_fs, "/CONFIG/NETIP.CFG", buf,
                       (uint32_t)(p - buf)) != 0) {
        kprintf("[NET] FAILED to persist /CONFIG/NETIP.CFG\n");
        return -1;
    }
    kprintf("[NET] persisted /CONFIG/NETIP.CFG (%d bytes, %s)\n",
            (int)(p - buf), g_net_static_configured ? "static+dns" : "dns-only");
    return 0;
}

// Initialize network stack
int net_init(void) {
    // Enable serial output for network debugging
    kprintf_set_dual_output(1);
    kprintf("\n[NET] Initializing network stack...\n");

    // #524: initialize the BSD socket layer (wait queue + descriptor table).
    extern void socket_init(void);
    socket_init();

    // PCI is already initialized by main.c

    // Try VirtIO-net first (preferred for VMs)
    kprintf("[NET] Checking for VirtIO network... (calling virtio_net_init)\n");
    int virtio_result = virtio_net_init();
    kprintf("[NET] virtio_net_init returned %d\n", virtio_result);
    if (virtio_result == 0) {
        kprintf("[NET] Using VirtIO network driver\n");
        active_driver = NET_DRIVER_VIRTIO;
        driver_get_mac = virtio_net_get_mac;
        driver_send = virtio_net_send;
        driver_receive = virtio_net_receive;
        driver_link_up = virtio_net_link_up;
    }
    // Fall back to E1000
    else {
        kprintf("[NET] Checking for E1000 network... (calling e1000_init)\n");
        int e1000_result = e1000_init();
        kprintf("[NET] e1000_init returned %d\n", e1000_result);
        if (e1000_result == 0) {
            kprintf("[NET] Using E1000 network driver\n");
            active_driver = NET_DRIVER_E1000;
            driver_get_mac = e1000_get_mac;
            driver_send = e1000_send;
            driver_receive = e1000_receive;
            driver_link_up = e1000_link_up;
        }
        else {
            // #362: no PCI NIC. Fall back to a USB Ethernet device (CDC-ECM
            // class or ASIX AX88772/AX88179 dongle) if the xHCI enumeration
            // attached one. This is the real-hardware (iMac) path; VMs with
            // e1000/virtio never reach here, so their behavior is unchanged.
            kprintf("[NET] Checking for USB Ethernet... (present=%d)\n",
                    usb_eth_present());
            if (usb_eth_present() && usb_eth_start() == 0) {
                kprintf("[NET] Using USB Ethernet driver (%s)\n", usb_eth_name());
                active_driver = NET_DRIVER_USB;
                driver_get_mac = usb_eth_get_mac;
                driver_send = usb_eth_send;
                driver_receive = usb_eth_receive;
                driver_link_up = usb_eth_link_up;
            } else {
                kprintf("[NET] Network initialization failed (no supported NIC found)\n");
                // Boot-time binding is OVER even though it found nothing. From
                // here on a USB NIC that enumerates is a HOT-PLUG and must be
                // armed for the net worker, not left for a net_init() that has
                // already run. This is the exact case the owner hit.
                netattach_boot_done_rs();
                bootlog_write("[NET] no NIC at boot; hot-plug attach armed");
                return -1;
            }
        }
    }

    // Initialize protocol stack
    kprintf("[NET] Initializing protocol stack...\n");
    eth_init();
    kprintf("[NET] eth_init done\n");
    ip_init();
    kprintf("[NET] ip_init done\n");
    arp_init();
    kprintf("[NET] arp_init done\n");
    icmp_init();
    kprintf("[NET] icmp_init done\n");
    udp_init();
    kprintf("[NET] udp_init done\n");
    tcp_init();
    kprintf("[NET] tcp_init done\n");
    dhcp_init();
    kprintf("[NET] dhcp_init done\n");
    dns_init();
    kprintf("[NET] dns_init done\n");
    crypto_init();
    kprintf("[NET] crypto_init done\n");
    https_init();
    kprintf("[NET] https_init done\n");
    wget_init();
    kprintf("[NET] wget_init done\n");
    ftp_init();
    kprintf("[NET] ftp_init done\n");

    net_initialized = 1;
    kprintf("[NET] Network stack initialized successfully!\n");

    // #381: prime the cached carrier state with one read now (one-shot at boot,
    // not on any periodic path) so nic_link_up()/net_is_up() are meaningful
    // immediately; the background net worker keeps it fresh from here on.
    nic_refresh_link();

    // Boot-time static IP override: if /CONFIG/NETIP.CFG exists and parses,
    // apply it now and signal main.c to skip its default static/DHCP path.
    // The NIC is up and the protocol stack is initialized at this point.
    // #238: load the packet-filter policy BEFORE any address is configured
    // and therefore before the first DHCP DISCOVER leaves the machine. The
    // compiled-in fail-safe policy is already in force from the first packet
    // of boot (fwfilter.rs DEFAULT_CFG), so this only ever REPLACES a working
    // policy with the operator's; it can never open a window where nothing is
    // filtering. The root filesystem is mounted well before net_init()
    // (main.c ext2_mount at boot, net_init much later), which is the same
    // assumption net_apply_static_config() on the next line already makes.
    fw_boot_load();

    g_net_static_configured = net_apply_static_config();

    // Send test ARP packet to verify TX works
    kprintf("[NET] Sending test ARP packet...\n");
    uint8_t test_mac[6];
    nic_get_mac(test_mac);
    uint32_t test_ip = 0xC0000201;  // 192.0.2.1
    arp_request(test_ip);
    kprintf("[NET] Test ARP request sent for 192.0.2.1\n");
    kprintf("[NET] net_init complete, disabling dual output\n");
    kprintf_set_dual_output(0); // Disable dual output after init

    // Boot-time binding is over. A later USB NIC probe is now a hot-plug; with
    // a NIC already bound netattach_on_probe_rs() will refuse to arm, so the
    // live interface and its lease are never disturbed.
    netattach_boot_done_rs();

    return 0;
}

// Debug: track when we've shown debug

// Poll network

// ---------------------------------------------------------------------------
// #297: GLOBAL NETWORK SERIALIZATION LOCK.
// The packet path is single-threaded by design: eth_receive() uses a shared
// static rx_buffer, e1000_receive() walks a single RX descriptor ring (rx_cur /
// RDT), e1000_send() walks a single TX ring (tx_cur / TDT), and tcp_handle()
// mutates one global connection table. Before #297 two contexts could run net
// code at once: the RC service process (pumping net for its open :2323 session)
// and the httppost worker driving an AI POST. Concurrent eth_receive() corrupted
// the RX ring / rx_buffer -> dropped SYN-ACKs (connect timeouts) and, on repeated
// POSTs, a permanently wedged stack (ping + RC dead until reboot). A recursive
// per-owner spinlock serializes ALL NIC ring + connection-table access. It is
// recursive because RX processing (under the lock) sends ACKs back through the
// same TX path; nesting must not self-deadlock. It is held only across the
// non-yielding ring/table critical sections (net_poll drain, a single tcp_send /
// tcp_recv burst), never across proc_sleep(), so it cannot starve the system.
#include "../sync/spinlock.h"
extern uint32_t smp_this_cpu(void);
static spinlock_t g_net_lock = SPINLOCK_INIT;
static volatile uint32_t g_net_lock_owner = 0xFFFFFFFF;
static volatile int g_net_lock_depth = 0;
static volatile uint64_t g_net_lock_saved_flags = 0;

// IRQs are disabled while the lock is held: a spinlock holder must not be
// preemptible, or a single-CPU spinner (or another CPU) waiting on it would
// deadlock while the holder sits preempted. The ring busy-waits inside are
// bounded (microseconds), so masking IRQs briefly is safe.
// #745 (task #62) INSTRUMENTATION. How long does a context spend SPINNING for
// this lock with interrupts already off? That number is the direct measure of
// "the network stalled something else", and until now nothing recorded it.
volatile uint64_t g_net_lock_wait_max_cyc = 0;   // longest blocking acquire spin
volatile uint64_t g_net_lock_wait_tot_cyc = 0;   // total blocking acquire spin
volatile uint64_t g_net_lock_contended    = 0;   // net_trylock() refusals

// ---------------------------------------------------------------------------
// #745 (task #69): HOW LONG IS THE LOCK **HELD**, AND BY WHOM.
//
// task #62 measured the WAIT (g_net_lock_wait_max_cyc: how long a loser spins).
// That is the wrong half of the problem, and measuring it alone is why the
// answer to "why did the machine stop for 30 seconds" still had to be argued
// from source. A waiter's spin is bounded BY THE HOLDER: nothing can spin for
// 30s unless something HELD it for 30s. The hold is the primary quantity and
// nothing recorded it.
//
// Worse, on a single core the wait counter cannot even see the fault. net_lock
// keeps IF clear for the whole hold, so the holder is not preemptible; a second
// thread on the same core can therefore never be running to spin, and lockwait=
// reads 0 while the machine is frozen solid. lockwait= is a MULTI-CORE
// contention metric. holdmax= is the freeze metric.
//
// holdra= is the part that makes this actionable on a machine with no serial
// console: the return address of whoever took the lock for holdmax. addr2line
// that against the build's kernel.elf and the offending call site names itself,
// instead of a reader having to re-derive the whole net_lock caller table.
//
// Budget: a hold longer than NET_LOCK_HOLD_BUDGET_US is a defect by definition,
// because for that whole window the timer tick cannot fire, so THE SCHEDULER
// DOES NOT RUN and no priority scheme in the kernel can preempt it. 2 ms is
// half a tick at 250 Hz - generous for a ring drain and a TRB write, and far
// under anything a user could perceive.
#define NET_LOCK_HOLD_BUDGET_US 2000ULL

volatile uint64_t g_net_lock_hold_max_cyc = 0;   // longest single hold (read+reset by [NETSTARVE])
volatile uint64_t g_net_lock_hold_max_hb  = 0;   // same, second owner, for the [HB] record
volatile uint64_t g_net_lock_hold_max_ra  = 0;   // caller RA that produced hold_max_hb
volatile uint64_t g_net_lock_hold_tot_cyc = 0;   // total time the lock was held
volatile uint64_t g_net_lock_holds        = 0;   // outermost acquisitions
volatile uint64_t g_net_lock_over_budget  = 0;   // holds beyond NET_LOCK_HOLD_BUDGET_US

// Per-hold state. Written only by the owner, between its outermost acquire and
// its outermost release, so no lock is needed to protect it.
static volatile uint64_t g_net_lock_t0 = 0;
static volatile uint64_t g_net_lock_ra = 0;

static inline uint64_t net_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void net_lock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint32_t cpu = smp_this_cpu();
    if (g_net_lock_owner == cpu) { g_net_lock_depth++; return; }  // recursive re-entry
    uint64_t _w0 = net_rdtsc();
    spinlock_acquire(&g_net_lock);
    uint64_t _w = net_rdtsc() - _w0;
    g_net_lock_wait_tot_cyc += _w;
    if (_w > g_net_lock_wait_max_cyc) g_net_lock_wait_max_cyc = _w;
    g_net_lock_owner = cpu;
    g_net_lock_depth = 1;
    g_net_lock_saved_flags = flags;   // remember caller IF for the outermost release
    g_net_lock_t0 = net_rdtsc();      // #745 (task #69): hold clock starts here
    g_net_lock_ra = (uint64_t)__builtin_return_address(0);
}

// ---------------------------------------------------------------------------
// #745 (task #62): THE COMPOSITOR MUST NEVER BLOCK ON THE NETWORK.
//
// net_lock() does `cli` BEFORE it spins on the global spinlock, so a context
// that loses the race waits with interrupts DISABLED and cannot even be
// preempted out of the wait. sys_fb_flip() - the compositor's frame present,
// the syscall that literally puts pixels on the screen - called net_poll(),
// which takes this lock. That made the compositor's worst-case frame time the
// worst case of EVERY network context in the kernel: every client pump loop
// (kernel/net/https.c's https_tcp_send has NO deadline at all and retries
// forever - see the [DEBT]/HARMFUL entry in tools/concurrency-lint/
// allowlist.txt), the net worker, the SSH server, and every TX. Those loops run
// HARDEST when the network is failing, which is exactly when the desktop must
// stay responsive. A user on the real iMac saw the cursor move one frame every
// few seconds while the App Store and example.com both failed to load.
//
// net_trylock() is the SAME lock with the SAME recursion semantics, except it
// never waits: if another CPU/context owns it we restore the caller's IF and
// return 0. Built on the existing spinlock_try_acquire() primitive - no new
// locking scheme, per the "reuse the shared primitive" rule.
//
// Kept in C rather than Rust: this is the hottest path in the kernel (every
// frame present and every packet), it is pure `cli`/`pushfq`/spinlock
// manipulation that must stay inlined next to the existing net_lock() it is a
// variant of, and an FFI crossing per present is exactly the cost this change
// exists to remove. Same justification the #549 breaker in this file already
// carries.
// ---------------------------------------------------------------------------
int net_trylock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint32_t cpu = smp_this_cpu();
    if (g_net_lock_owner == cpu) { g_net_lock_depth++; return 1; }  // recursive
    if (!spinlock_try_acquire(&g_net_lock)) {
        g_net_lock_contended++;
        if (flags & 0x200) __asm__ volatile("sti");   // restore caller IF
        return 0;
    }
    g_net_lock_owner = cpu;
    g_net_lock_depth = 1;
    g_net_lock_saved_flags = flags;
    g_net_lock_t0 = net_rdtsc();      // #745 (task #69): trylock holds too
    g_net_lock_ra = (uint64_t)__builtin_return_address(0);
    return 1;
}

void net_unlock(void) {
    uint32_t cpu = smp_this_cpu();
    if (g_net_lock_owner != cpu) return;   // defensive: not the owner
    if (--g_net_lock_depth > 0) return;
    // #745 (task #69): close the hold measurement BEFORE releasing, so the
    // number covers exactly the interrupts-off window and nothing else.
    {
        uint64_t _h = net_rdtsc() - g_net_lock_t0;
        uint64_t _ra = g_net_lock_ra;
        g_net_lock_hold_tot_cyc += _h;
        g_net_lock_holds++;
        if (_h > g_net_lock_hold_max_cyc) g_net_lock_hold_max_cyc = _h;
        if (_h > g_net_lock_hold_max_hb) {
            g_net_lock_hold_max_hb = _h;
            g_net_lock_hold_max_ra = _ra;   // paired: the RA always describes hb
        }
        uint64_t _khz = mono_tsc_khz();
        uint64_t _us  = _khz ? (_h * 1000ULL / _khz) : 0;
        if (_us >= NET_LOCK_HOLD_BUDGET_US) {
            g_net_lock_over_budget++;
            // ONE loud line per boot, naming the caller. Serial only and
            // rate-limited to a single occurrence: we are still inside the
            // cli region here, and a report that can storm is a second
            // source of the stall it is reporting (#373's lesson).
            static int _told = 0;
            if (!_told) {
                _told = 1;
                kprintf("[NETHOLD] #745/#69: net_lock held %luus with IRQs OFF "
                        "(ra=%p). The scheduler could not run for that whole "
                        "window, so no priority can preempt it. addr2line this "
                        "address against this build's kernel.elf.\n",
                        (unsigned long)_us, (void *)_ra);
            }
#ifdef NET_LOCK_HOLD_PANIC
            // make NETHOLDPANIC=1: turn the first over-budget hold into a
            // panic, so the offending stack is captured instead of inferred.
            extern void kpanic(const char *msg);
            kpanic("#745/#69: net_lock held past its interrupts-off budget");
#endif
        }
    }
    uint64_t flags = g_net_lock_saved_flags;
    g_net_lock_owner = 0xFFFFFFFF;
    spinlock_release(&g_net_lock);
    if (flags & 0x200) __asm__ volatile("sti");
}

// #615: RX drain accounting, so "how many packets did one poll actually get?"
// is a measured number instead of an assumption. Serial-reported by the HTTP
// profile line; no cost beyond two increments.
uint64_t g_net_poll_calls = 0, g_net_poll_pkts = 0, g_net_poll_max = 0;

// ---------------------------------------------------------------------------
// #69: THE SHARED CHUNKED RX DRAIN, and the one place the NIC CR3 window is
// opened for a drain.
//
// WHAT WAS MEASURED (golden 1963 + this instrumentation, e1000, VM <vmid>, a
// userland load of 671 x 512 KB bulk TCP transfers over BOTH syscall families):
//
//   trecv (whole tcp_recv_kcr3 window)   max  890us   avg 34us
//   _drain (the 64-frame loop ALONE)     max  889us   avg 33us
//   _trcv (tcp_recv itself)              max   84us   avg  3us
//   _tmr  (tcp_timer)                    max   20us   avg  0us
//   dfr   (frames in the deepest drain)       26
//
// The drain IS the window: 889 of 890us, and 33 of 34us. Everything else in
// there is noise. So this is a "shrink the critical section" problem, not a
// "restructure the lock" problem, and net_lock (holdmax 20-53us idle) was never
// the thing to fix.
//
// AND THE OBVIOUS FIX IS THE WRONG ONE. The natural move is to lower the 64
// bound. dfr=26 says the bound is NEVER REACHED, so lowering it to 32 would
// change nothing at all and lowering it to 8 would change the tail by dropping
// work, not by shortening a window. The cost is ~35us PER FRAME (890us / 26
// frames), so the tail is simply "a burst of 26 frames arrived and we processed
// all of them with interrupts off".
//
// THE FIX IS TO CLOSE THE WINDOW BETWEEN CHUNKS. Same total work, same frames,
// same order, but the interrupts-off window is one CHUNK instead of one BURST.
//
// WHY THIS DOES NOT REGRESS #549, BY CONSTRUCTION RATHER THAN BY TESTING. The
// window is reopened with the CALLER's saved RFLAGS.IF, never with an
// unconditional `sti`. A caller that already had interrupts off - every
// pre-scheduler boot path, and any context that reached here under an outer
// `cli` - gets IF=0 restored at every chunk boundary, so the loop collapses to
// exactly the old single-window behaviour for them. TX stays fire-and-forget
// and nothing here can sleep: there is no wait_event, no proc_sleep and no
// allocation on this path. The no-block regime cannot tell this change from its
// predecessor.
//
// WHY IT TAKES net_lock WHEN THE CALLERS DID NOT. The five socket.c sites and
// the eleven *_kcr3 helpers reach the RX ring holding NO lock at all; their
// only mutual exclusion is the `cli`, which is exclusion on ONE core and
// nothing at all on two. Opening the window between chunks would weaken even
// that, so the chunk takes the lock the ring is supposed to be under. net_lock
// is recursive per-owner, so a caller that already holds it nests harmlessly.
// This makes these paths strictly better protected than before, not worse.
//
// COST. Reopening the window costs one CR3 write (a TLB flush) per chunk. That
// is paid ONLY on a burst: the measured average drain takes ~1 frame, so the
// common case is a single chunk and a single CR3 write, exactly as today. A
// full 26-frame burst pays three extra CR3 writes to remove ~600us of
// interrupts-off time.
//
// KEPT IN C: it is the `cli`/`pushfq`/`mov %cr3` window itself, the same
// irreducible paging+asm entanglement that net_lock(), net_trylock() and the
// existing net_cr3_enter() shims in this tree already state as their reason.
// CHUNK SIZE, CHOSEN BY MEASUREMENT, NOT BY FEEL. With the empty-chunk cost
// split out into its own site (_dry), the two costs are no longer confounded:
//
//   _dry   (chunk that found the ring empty)  max 4-9us    avg 0us   380,231 calls
//   _drain (chunk that consumed >=1 frame)    max 558-812us avg 194us  71,058 calls
//
// So reopening the window - cli, two CR3 writes, net_lock, one failed
// eth_receive - costs UNDER 10us worst case and rounds to 0 on average, while a
// frame costs ~24us on average and ~100us in the tail. The window is made of
// FRAMES, and the per-chunk overhead is close to free.
//
// That is measured refutation of the intuition this started from. The first
// reading (drain avg 31us over ALL calls) looked like a large fixed per-call
// cost, which would have argued for FEWER, BIGGER chunks. It was an average
// over 380k nearly-free empty chunks and 71k expensive full ones, and it
// pointed the opposite way to the truth.
//
// Because overhead is ~free, the chunk should be as small as the throughput
// will tolerate. 4 frames puts the worst case at roughly 4 x 100us.
#define NET_DRAIN_CHUNK  4      // frames per interrupts-off window
#define NET_DRAIN_TOTAL 64      // unchanged overall bound (was the loop's 64)

unsigned net_rx_drain_chunked(void) {
    // Bounded by construction: NET_DRAIN_TOTAL/NET_DRAIN_CHUNK iterations max,
    // and the loop also exits the moment the ring runs dry. No progress
    // condition is being polled, so this is not the shape the concurrency lint
    // exists to refuse.
    int done = 0;
    for (; done < NET_DRAIN_TOTAL; ) {
        IRQWIN_DECL;
        IRQWIN_ENTER();                       // cli, remembering the caller's IF
        uint64_t saved = net_drain_cr3_enter();
        net_lock();
        int n = 0;
        while (n < NET_DRAIN_CHUNK && (done + n) < NET_DRAIN_TOTAL) {
            if (!eth_receive()) break;
            n++;
        }
        net_unlock();
        net_drain_cr3_exit(saved);
        // Charge an empty chunk to its own site: a drain that found nothing
        // is measuring the FIXED cost of asking, not the cost of a frame.
        IRQWIN_EXIT(n ? IRQWIN_SUB_DRAIN : IRQWIN_SUB_DRAIN0);
        if ((unsigned)n > g_irqwin_drain_max_frames)
            g_irqwin_drain_max_frames = (unsigned)n;
        done += n;
        if (n < NET_DRAIN_CHUNK) break;       // ring dry: nothing left to chunk
    }
    return (unsigned)done;
}

void net_poll(void) {
    if (!net_initialized) return;
    uint64_t _dp_t0 = dp_tsc();

    // #69: THE DRAIN RUNS FIRST, OUTSIDE THIS FUNCTION'S net_lock, IN CHUNKS.
    //
    // It used to run inside the net_lock below, which meant the whole 64-frame
    // burst was one interrupts-off window. With the syscall-side drains chunked
    // and this one left alone, this became the longest remaining window on the
    // machine by a wide margin: measured holdmax=1521us here against 549us for
    // the chunked drain on the same boot, with holdra= resolving to this exact
    // function. Fixing fifteen call sites and leaving the sixteenth is not a
    // fix, it just moves which name is on the worst number.
    //
    // Hoisting is safe in the direction that matters: arp_flush_ready() must
    // run AFTER the drain has cached freshly resolved MACs and must not be
    // nested inside eth_receive() (#333/#747), and draining first then taking
    // the lock preserves exactly that order. The drain takes net_lock itself,
    // per chunk, so the ring is still accessed under the lock throughout - more
    // consistently than the syscall paths managed before this change, since
    // those took no lock at all.
    uint32_t _got = net_rx_drain_chunked();
    net_lock();
    g_net_poll_calls++; g_net_poll_pkts += _got;
    if (_got > g_net_poll_max) g_net_poll_max = _got;
    // #333/#747: now that the RX drain has cached any freshly resolved MACs,
    // flush packets held for cold LAN hosts. Runs HERE (top level), not from the
    // ARP receive callback, so the send is never nested inside eth_receive.
    extern void arp_flush_ready(void);
    arp_flush_ready();
    dhcp_poll();

    // #381: USB dongle carrier polling + late link-up DHCP (re)start is now owned
    // entirely by the background net worker (net_worker), NOT here. net_poll()
    // runs under net_lock() (interrupts off) on the compositor frame path, so it
    // must never touch the (slow, cable-less-stalling) USB PHY. dhcp_poll() above
    // is a non-blocking state machine and safely drives DORA->BOUND from here.

    // #380: refresh neighbor caches with a SLOW gratuitous ARP (every 180s).
    // The old 10s cadence meant that if our address ever collided with another
    // host we re-poisoned every neighbor's (and the router's) ARP cache six
    // times a minute. A good LAN citizen announces on acquiring the address
    // (net_adopt_static / DHCP bind do that) and only refreshes rarely.
    extern volatile uint64_t timer_ticks;
    extern uint32_t g_timer_hz;
    static uint64_t last_garp_tick = 0;
    uint64_t garp_interval = (uint64_t)g_timer_hz * 180;
    if (garp_interval > 0 && timer_ticks - last_garp_tick >= garp_interval) {
        last_garp_tick = timer_ticks;
        arp_announce();
    }
    net_unlock();
    g_dp_poll_cyc += dp_tsc() - _dp_t0; g_dp_poll_calls++;
}

// ---------------------------------------------------------------------------
// #745 (task #62): the NON-BLOCKING, BOUNDED pump, for callers that must never
// wait on the network - today exactly one: the compositor's sys_fb_flip().
//
// Returns 1 if it serviced the stack, 0 if it DECLINED because another context
// held net_lock. Declining is correct and cheap: the dedicated net pump thread
// (net_pump_worker, below) is the always-armed service source, so a skipped
// frame costs the network at most one pump interval, while a blocked frame
// costs the user a visible freeze.
//
// The work it does is a strict SUBSET of net_poll(): the same RX drain (with a
// caller-supplied smaller bound), the same arp_flush_ready(), the same
// dhcp_poll() state machine. It deliberately does NOT carry net_poll()'s
// 180-second gratuitous-ARP announce - that is a TX on a shared LAN (#380) and
// has no business on the frame path; net_poll() on the pump thread still does
// it on schedule.
// ---------------------------------------------------------------------------
int net_poll_try(int max_pkts) {
    if (!net_initialized) return 0;
    if (max_pkts <= 0 || max_pkts > 64) max_pkts = 64;
    uint64_t _dp_t0 = dp_tsc();
    if (!net_trylock()) return 0;          // contended: skip, never wait
    uint32_t _got = 0;
    for (int i = 0; i < max_pkts; i++) { if (!eth_receive()) break; _got++; }
    g_net_poll_calls++; g_net_poll_pkts += _got;
    if (_got > g_net_poll_max) g_net_poll_max = _got;
    extern void arp_flush_ready(void);
    arp_flush_ready();
    dhcp_poll();
    net_unlock();
    g_dp_poll_cyc += dp_tsc() - _dp_t0; g_dp_poll_calls++;
    return 1;
}

// Start DHCP discovery
int net_dhcp(void) {
    if (!net_initialized) return -1;
    return dhcp_discover();
}

// Start DHCP discovery and wait for completion (blocking)
int net_dhcp_blocking(void) {
    if (!net_initialized) return -1;
    return dhcp_discover_blocking();
}

// Configure network
void net_configure(uint32_t ip, uint32_t gateway, uint32_t netmask) {
    ip_set_address(ip);
    ip_set_gateway(gateway);
    ip_set_netmask(netmask);
}

// #380: adopt a STATIC IPv4 address ONLY after RFC 5227 duplicate-address
// detection confirms it is free. This is the guardrail that stops MayteraOS
// from claiming (and then gratuitous-ARP announcing) an address already in use
// on the LAN - the exact behavior that hijacked a co-host's ARP entry and took
// the user's network down. Must be called from top-level boot context (it
// pumps RX synchronously), NEVER from inside a receive callback.
// Returns 0 if the address was verified free and adopted; -1 if it is in use
// (the caller should try another address or stay unconfigured).
int net_adopt_static(uint32_t ip, uint32_t gateway, uint32_t netmask) {
    if (!net_initialized || ip == 0) return -1;
    uint8_t *p = (uint8_t *)&ip;
    // #381: DAD requires a live carrier - arp_ip_in_use() sends probes and polls
    // the RX ring for seconds. With no cable that is pure wasted wire + a
    // multi-second block for an answer that can never arrive. Never probe (or
    // claim) an address while the link is down; the net worker retries on
    // carrier-up. Uses the cheap cached carrier state.
    if (!nic_link_up()) {
        kprintf("[NET] DAD: link down; not probing/adopting %d.%d.%d.%d\n",
                p[3], p[2], p[1], p[0]);
        return -1;
    }
    if (arp_ip_in_use(ip)) {
        kprintf("[NET] DAD: %d.%d.%d.%d already in use on LAN; NOT adopting\n",
                p[3], p[2], p[1], p[0]);
        return -1;
    }
    // #504: arp_ip_in_use() above ran RFC 5227 DAD, which PUMPS the RX/DHCP path
    // for seconds; a concurrent DHCP DORA (whose OFFER/ACK were already in flight)
    // can reach BOUND during that window and commit its lease via ip_set_address()
    // from dhcp_poll(). Without this guard the static fallback then OVERWROTE the
    // fresh lease (observed: "Bound to 192.0.2.1" immediately followed by
    // "adopted static 192.0.2.1"), which made the DHCP fix look broken.
    // dhcp_poll() commits under net_lock (net_poll holds it), so take net_lock here
    // to make "did DHCP bind?" and "commit the static IP" ATOMIC against it: if a
    // lease landed during DAD, abandon the static adoption and keep the lease. The
    // static fallback must never clobber a DHCP lease.
    net_lock();
    if (dhcp_is_bound() || ip_get_address() != 0) {
        net_unlock();
        kprintf("[NET] DHCP lease acquired during DAD; keeping it, not adopting "
                "static %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);
        return -1;
    }
    ip_set_address(ip);
    ip_set_gateway(gateway);
    ip_set_netmask(netmask);
    net_unlock();
    kprintf("[NET] DAD OK: adopted static %d.%d.%d.%d (verified unique)\n",
            p[3], p[2], p[1], p[0]);
    // Announce our now-verified-unique address a couple of times (RFC 5227).
    arp_announce();
    arp_announce();
    return 0;
}

// Get active driver type
net_driver_type_t net_get_driver_type(void) {
    return active_driver;
}

// Print network status
void net_print_status(void) {
    kprintf("\n[NET] Network Status:\n");

    const char *driver_name = "None";
    switch (active_driver) {
        case NET_DRIVER_E1000:  driver_name = "E1000"; break;
        case NET_DRIVER_VIRTIO: driver_name = "VirtIO"; break;
        case NET_DRIVER_USB:    driver_name = "USB-ETH"; break;
        default: break;
    }
    kprintf("  Driver:       %s\n", driver_name);

    uint8_t mac[6];
    nic_get_mac(mac);
    kprintf("  MAC Address:  %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    uint32_t ip = ip_get_address();
    uint32_t gw = ip_get_gateway();
    uint32_t nm = ip_get_netmask();
    uint8_t *p;

    p = (uint8_t *)&ip;
    kprintf("  IP Address:   %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);

    p = (uint8_t *)&nm;
    kprintf("  Netmask:      %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);

    p = (uint8_t *)&gw;
    kprintf("  Gateway:      %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);

    uint32_t dns = dns_get_server();
    p = (uint8_t *)&dns;
    kprintf("  DNS Server:   %d.%d.%d.%d\n", p[3], p[2], p[1], p[0]);

    kprintf("  Link Status:  %s\n", nic_link_up() ? "Up" : "Down");
}

// Driver abstraction implementations
void nic_get_mac(uint8_t *mac) {
    if (driver_get_mac) {
        driver_get_mac(mac);
    } else if (mac) {
        memset(mac, 0, 6);
    }
}

// #745 (task #62) INSTRUMENTATION: per-packet TRANSMIT COST, the number the
// #549 investigation had to infer. On the real iMac dongle the old
// usbnet_bulk_out() busy-polled the xHCI up to ~40ms PER PACKET; #549 made TX
// fire-and-forget, but nothing ever measured the result on the hardware where
// the cost lived. g_nic_tx_max_cyc answers it directly and is reported on the
// [NETSTARVE] serial line: tens of MICROseconds means TX is async as intended,
// tens of MILLIseconds means something reintroduced a wait on the wire.
uint64_t g_nic_tx_ok = 0, g_nic_tx_fail = 0;
// Two owners, same rule as the present gap above: g_nic_tx_max_cyc is read and
// reset by [NETSTARVE], g_nic_tx_max_hb by the /HEARTBEAT.TXT record.
uint64_t g_nic_tx_tot_cyc = 0, g_nic_tx_max_cyc = 0, g_nic_tx_max_hb = 0;

int nic_send(const void *data, uint16_t length) {
    if (driver_send) {
        net_lock();
        uint64_t _t0 = net_rdtsc();
        int r = driver_send(data, length);
        uint64_t _d = net_rdtsc() - _t0;
        g_nic_tx_tot_cyc += _d;
        if (_d > g_nic_tx_max_cyc) g_nic_tx_max_cyc = _d;
        if (_d > g_nic_tx_max_hb)  g_nic_tx_max_hb  = _d;
        if (r < 0) g_nic_tx_fail++; else g_nic_tx_ok++;
        net_unlock();
        return r;
    }
    return -1;
}

int nic_receive(void *buffer, uint16_t buffer_size) {
    if (driver_receive) {
        net_lock();
        int r = driver_receive(buffer, buffer_size);
        net_unlock();
        return r;
    }
    return 0;
}

// #381: cached carrier state. For a USB Ethernet dongle, reading the PHY link
// is a (possibly multi-second, when there is no cable) chain of MII control
// transfers. nic_link_up() is called from the compositor + net_poll hot path
// (net_poll runs under net_lock == interrupts off) and from net_is_up() on every
// DNS/HTTP/wget call, so it MUST be cheap and non-blocking. For USB we therefore
// return this cached value, refreshed off the UI path by the background net
// worker (usb_eth_poll_link). For PCI NICs (e1000/virtio) driver_link_up() is a
// single cheap MMIO register read, so those keep reading it directly (real-time,
// byte-identical to before - VMs unchanged).
static volatile int g_link_cached = 0;

int nic_link_up(void) {
    if (active_driver == NET_DRIVER_USB) {
        return g_link_cached;          // cheap cached; worker refreshes it
    }
    if (driver_link_up) {
        return driver_link_up();       // e1000/virtio: cheap MMIO, real-time
    }
    return 0;
}

// Refresh the cached carrier state. For USB this issues the real PHY read and is
// called ONLY from the background net worker (off the UI/net_poll path). Returns
// the fresh carrier state.
static int nic_refresh_link(void) {
    int link;
    if (active_driver == NET_DRIVER_USB) {
        link = usb_eth_poll_link();    // active PHY read (may block; off UI path)
    } else {
        link = driver_link_up ? driver_link_up() : 0;
    }
    g_link_cached = link;
    return link;
}

// #374: authoritative "is the network usable?" predicate. Consulted by the DNS
// resolver and the HTTP/HTTPS/wget clients BEFORE any DNS/ARP/TCP/TLS work, so a
// machine with no working NIC (stack not initialised, link down, or no IP) never
// starts a call that would burn a multi-second connect/resolve timeout and freeze
// the desktop. Requires: stack initialised AND link carrier AND a configured IP.
// "Could a packet actually have left the box?" This is net_is_up() WITHOUT the
// FAULTY clause: it answers about the hardware and configuration, not about
// whether policy currently lets us try. Exported because the transport clients
// (wget/https/dns) must ask THIS, not net_is_up() - see net.h.
int net_wire_usable(void) {
    if (!net_initialized) return 0;
    if (!nic_link_up()) return 0;
    if (ip_get_address() == 0) return 0;
    return 1;
}

int net_is_up(void) {
    if (!net_wire_usable()) return 0;
    // #549: a persistently-unreachable interface reports DOWN so every client
    // that gates on net_is_up() (haservice, netinfo, browser) stops retrying.
    if (g_net_conn_state == NET_STATE_FAULTY) return 0;
    return 1;
}

// ---------------------------------------------------------------------------
// #381: background net worker. Runs at PRIO_NORMAL ~1 Hz and is the ONLY place
// that (a) reads the USB PHY carrier (a slow, cable-less-stalling chain of MII
// control transfers) and (b) drives DHCP acquisition + the RFC 5227 static-IP
// DAD fallback. This keeps a cable-less dongle from ever blocking the compositor
// or boot: when the carrier is DOWN the worker is a fast no-op (zero DHCP, zero
// DAD, zero fetches, zero wire); on a down->up transition it kicks DHCP in the
// background; if DHCP does not bind it adopts a DAD-verified static address.
// It NEVER blocks the UI thread - all the slow work lives here on its own thread.
// ---------------------------------------------------------------------------
extern int  dhcp_is_bound(void);
extern int  dhcp_discover(void);

// Unbind a NIC that was UNPLUGGED. Runs ONLY on net_worker, for the same reason
// the bind does: the USB side must not touch the network stack from inside slot
// teardown. Returns 1 if it unbound one.
//
// The STACK STAYS UP (net_initialized keeps its value). Only the driver binding
// and the address go; every handler stays registered, which is what makes the
// replug path cheap and is why eth_refresh_mac() exists rather than a second
// eth_init().
static int net_late_detach_service(void) {
    if (!netattach_take_detach_rs()) return 0;
    if (active_driver != NET_DRIVER_USB) return 0;
    if (usb_eth_present()) return 0;      // it came back before we got here

    // Clear the function pointers under net_lock so this cannot race a
    // transmit that has already passed nic_send()'s NULL check. net_lock is
    // cli + a spinlock: four stores under it, no allocation, no wait (#549).
    net_lock();
    active_driver  = NET_DRIVER_NONE;
    driver_get_mac = NULL;
    driver_send    = NULL;
    driver_receive = NULL;
    driver_link_up = NULL;
    net_unlock();
    g_link_cached = 0;

    // Drop the lease. The address belonged to a NIC that is gone, and keeping
    // it would make net_wire_usable() lie and would stop a replug re-running
    // DORA (dhcp_discover() early-returns while BOUND).
    dhcp_reset();
    ip_set_address(0);

    bootlog_write("[NETATTACH] USB NIC unbound after unplug; address dropped, "
                  "stack left up for a replug");
    kprintf("[NETATTACH] USB NIC unbound after unplug\n");
    return 1;
}

// Bind a NIC that appeared after net_init() ran. Runs ONLY on net_worker.
// Returns 1 if it bound one, 0 if there was nothing to do.
static int net_late_attach_service(void) {
    if (!netattach_take_rs()) return 0;

    // Second guard, on net.c's own state rather than the flag. The flag can only
    // have been armed while unbound, but the bind is the irreversible step and
    // it costs one compare to be sure.
    if (active_driver != NET_DRIVER_NONE) {
        bootlog_write("[NETATTACH] attach ignored: driver type %d already bound",
                      (int)active_driver);
        return 0;
    }
    if (!usb_eth_present()) {
        bootlog_write("[NETATTACH] attach taken but no USB NIC present; dropped");
        return 0;
    }
    if (usb_eth_start() != 0) {
        // Re-arm rather than lose the device: the next pass retries, so a
        // transient submit failure does not require the user to replug.
        netattach_rearm_rs();
        bootlog_write("[NETATTACH] usb_eth_start() failed; will retry");
        return 0;
    }

    // No net_lock here, unlike the UNBIND above, and the asymmetry is
    // deliberate. Unbinding NULLs a pointer another context may be about to
    // call, so it has to exclude nic_send(); binding only ever makes a NULL
    // pointer valid, each pointer is read on its own and never as a set, and
    // the interface has no address until DORA runs later on THIS thread, so
    // there is nothing in flight to tear.
    active_driver   = NET_DRIVER_USB;
    driver_get_mac  = usb_eth_get_mac;
    driver_send     = usb_eth_send;
    driver_receive  = usb_eth_receive;
    driver_link_up  = usb_eth_link_up;

    if (!net_initialized) {
        // Nothing was ever brought up: net_init() returned -1 at boot because
        // there was no NIC, so it never reached eth_init(). Run the same
        // sequence it runs, in the same order.
        eth_init();
        ip_init();
        arp_init();
        icmp_init();
        udp_init();
        tcp_init();
        dhcp_init();
        dns_init();
        crypto_init();
        https_init();
        wget_init();
        ftp_init();
        net_initialized = 1;
        // #238: the packet filter is loaded BEFORE any address is configured and
        // therefore before the first DISCOVER leaves the machine, exactly as in
        // net_init(). The compiled-in fail-safe policy is already in force.
        fw_boot_load();
        g_net_static_configured = net_apply_static_config();
    } else {
        // A stack that is already up gets the new MAC and keeps its handler
        // table. eth_init() would zero eth_handler_count and make the stack
        // deaf, which is why eth_refresh_mac() exists.
        eth_refresh_mac();
    }

    // #155 bulk-IN reaper. Idempotent; the boot path starts it from
    // net_start_worker() when the USB NIC is bound at boot instead.
    usb_eth_start_rx_worker();

    uint8_t m[6];
    nic_get_mac(m);
    bootlog_write("[NETATTACH] hot-plug NIC bound: %s MAC %02x:%02x:%02x:%02x:%02x:%02x "
                  "stack=%s static=%d",
                  usb_eth_name(), m[0], m[1], m[2], m[3], m[4], m[5],
                  net_initialized ? "up" : "DOWN", g_net_static_configured);
    kprintf("[NETATTACH] hot-plug NIC bound: %s\n", usb_eth_name());
    return 1;
}

// Boot self-test for the hot-plug attach handoff. Proves, destructively and
// against the real statics, the four properties the fix rests on: a boot-time
// probe arms nothing; a hot-plug probe with no NIC arms exactly one bind; a
// probe while a NIC is already bound arms NOTHING (so a second adapter cannot
// rebind the stack or start a second DHCP client); and repeated probes coalesce
// into one bind while an unplug/replug pair produces two. It is a real assert
// on the take count, not a compile check, and it writes to the DURABLE log
// because the owner's machine has no serial port.
void netattach_selftest(void) {
    extern int netattach_selftest_rs(void);
    int fails = netattach_selftest_rs();
    bootlog_write("[NETATTACH-SELFTEST] %s (%d failing case(s) of 30)",
                  fails == 0 ? "PASS" : "FAIL", fails);
}

// One-line census of the attach handoff, for the durable log.
void netattach_log_stats(const char *why) {
    uint64_t probe = 0, armed = 0, taken = 0, sb = 0, sn = 0, co = 0, pend = 0;
    uint64_t det = 0, dett = 0;
    netattach_stats_rs(&probe, &armed, &taken, &sb, &sn, &co, &pend, &det, &dett);
    bootlog_write("[NETATTACH] %s: probes=%llu armed=%llu taken=%llu "
                  "skip_boot=%llu skip_bound=%llu coalesced=%llu pending=%llu "
                  "unplugs=%llu unbinds=%llu",
                  why ? why : "census",
                  (unsigned long long)probe, (unsigned long long)armed,
                  (unsigned long long)taken, (unsigned long long)sb,
                  (unsigned long long)sn, (unsigned long long)co,
                  (unsigned long long)pend, (unsigned long long)det,
                  (unsigned long long)dett);
}

static void net_worker(void *arg) {
    (void)arg;
    int prev_link = -1;
    int up_secs = 0;      // seconds since carrier came up (static-fallback timer)
    int dad_done = 0;     // static DAD fallback already attempted for this link-up

    kprintf("[NET] background net worker running\n");
    for (;;) {
        // Hot-plug bind FIRST, so nic_refresh_link() below already sees the new
        // NIC and the existing carrier down->up branch starts DHCP in this very
        // iteration. prev_link is 0 (the no-NIC passes took the !link path), so
        // the #431 relink branch is the one that fires.
        // Unbind BEFORE bind, so a fast unplug/replug inside one worker period
        // is processed in the order it happened rather than leaving the new
        // device refused by the old one's binding.
        if (net_late_detach_service()) {
            netattach_log_stats("unbound");
            prev_link = 0;     // the next attach is a genuine down->up edge
        }
        if (net_late_attach_service()) {
            netattach_log_stats("bound");
        }

        // Durable DORA evidence. Here, on the worker, because it writes a file
        // and every DHCP event site runs under net_lock (cli + spinlock) where
        // that is forbidden. Cheap when nothing happened: one seq compare.
        dhcp_durable_drain();

        int link = nic_refresh_link();   // slow USB PHY read here, OFF the UI path

        if (!link) {
            // Carrier DOWN: fast no-op. No DHCP, no DAD, no fetches, no wire.
            prev_link = 0;
            up_secs = 0;
            dad_done = 0;
            proc_sleep(1000);
            continue;
        }

        // Carrier UP.
        if (prev_link != 1) {
            // down->up (or first observation). Start a fresh acquisition unless
            // we already hold a lease / static address / file-provided config.
            up_secs = 0;
            dad_done = 0;
            // #549: a physical reconnect (unplug/replug) is a natural recovery
            // action - clear any NET_FAULTY so a genuinely-restored uplink comes
            // back without needing a manual reconfigure.
            net_clear_fault();
            if (g_net_static_configured) {
                // File-provided static config persists across link changes;
                // nothing to re-acquire on a relink.
            } else if (prev_link == 0) {
                // #431: a GENUINE carrier down->up transition (cable replug or a
                // move to a different network). prev_link==0 means we actually
                // saw the link go down first; the first-ever boot observation is
                // prev_link==-1 and is handled by the branch below. Re-run DHCP
                // from scratch even if we still hold an old lease, so a network
                // change re-acquires a fresh lease WITHOUT a reboot. dhcp_reset()
                // clears the BOUND state that would otherwise make dhcp_discover()
                // an early-return no-op; both run here on the worker thread (top
                // level, never a receive callback) and dhcp_discover() drops the
                // stale IP itself (ip_set_address(0)).
                kprintf("[NET] carrier relink (down->up); re-running DHCP for a fresh lease\n");
                dhcp_reset();
                dhcp_discover();
            } else if (!dhcp_is_bound() && ip_get_address() == 0) {
                // First observation at boot with nothing acquired yet.
                kprintf("[NET] carrier up; starting DHCP (background)\n");
                dhcp_discover();
            }
        }
        prev_link = 1;

        // While unbound with no address, drive DORA to completion in the
        // background. net_poll() is a bounded, non-blocking drain (eth_receive +
        // the dhcp_poll state machine), safe to call here.
        if (!dhcp_is_bound() && !g_net_static_configured && ip_get_address() == 0) {
            for (int k = 0; k < 4; k++) net_poll();
            up_secs++;
            // No lease after ~12s: fall back to a DAD-verified static address
            // (.200-.209), exactly like the old boot path but async and only
            // while the carrier is actually up (net_adopt_static is carrier-gated
            // too, so this can never probe/claim on a dead link).
            //
            // #504: RE-CHECK dhcp_is_bound()/ip_get_address() HERE, not just at the
            // loop-top gate. net_poll() above drives the DHCP state machine, so DORA
            // can reach BOUND within THIS SAME iteration (the offer/ack landed during
            // the 4x net_poll drain). The outer gate was evaluated BEFORE that drain
            // and is now stale: without this re-check the static fallback fired right
            // after a successful bind and OVERWROTE the good DHCP lease with .200
            // (observed: "Bound to 192.0.2.1" immediately followed by "adopted
            // static 192.0.2.1"), making the DHCP fix look broken. The static
            // fallback must apply ONLY when DHCP genuinely did not succeed.
            if (!dad_done && up_secs >= 12 && !dhcp_is_bound() && ip_get_address() == 0) {
                dad_done = 1;
                // #522: DHCP failed to bind in 12s. Dump the lock-free DHCP event
                // ring HERE, from the net worker thread (top-level, allowed to be
                // slow), never from inside dhcp_poll's timing window. This is the
                // evidence that shows how many concurrent contexts consumed one
                // expired deadline and which single one won the CAS claim.
                extern void dhcp_trace_dump(void);
                dhcp_trace_dump();

                int adopted = 0;
                for (uint32_t h = 200; h <= 209; h++) {
                    // #504: bail the instant DHCP wins, so a lease landing mid-
                    // sweep is never clobbered and we run no pointless DAD.
                    if (dhcp_is_bound() || ip_get_address() != 0) break;
                    if (net_adopt_static(0xC0000201u | h, 0xC0000201u,
                                         0xFFFFFF00u) == 0) { adopted = 1; break; }
                }
                if (!adopted) {
                    if (dhcp_is_bound() || ip_get_address() != 0) {
                        // #504: DHCP won during the sweep; the interface IS
                        // configured (a real lease, not the static fallback). This
                        // is the SUCCESS path, not "unconfigured" - say so, so the
                        // log never misreads a good DHCP lease as a failure.
                        kprintf("[NET] DHCP configured the interface during the "
                                "static-fallback window; keeping the lease\n");
                    } else {
                        kprintf("[NET] static fallback .200-.209 all taken; "
                                "interface left unconfigured\n");
                    }
                }
            }
        }

        proc_sleep(1000);
    }
}

// ---------------------------------------------------------------------------
// #745 (task #62): DEDICATED NETWORK SERVICE THREAD.
//
// Before this, sys_fb_flip() was the ONLY thing pumping net_poll() once the
// userland compositor owned the screen. #632 MEASURED that and recorded this
// exact end state as work still owed: "take net_poll() off the frame path
// entirely ... then the compositor's frame rate and the network's service rate
// stop being the same variable". It had two bad consequences, one in each
// direction: network throughput was a function of frame rate (measured at
// ~31-46 pps with presents stalled versus ~500 pps with them running), and
// frame rate was a function of network cost.
//
// This thread breaks both. It is the always-armed service source. The flip pump
// stays as a cheap REDUNDANT second source - CLAUDE.md's preference-1 shape
// (the hda_space_wq pattern: two independent sources, so starving either one
// cannot stop the work) - but it is now non-blocking via net_poll_try(), so it
// can never cost the compositor a frame.
//
// PACED ON mono_ms(), NOT ON THE SLEEP RETURNING. timer_ticks is not a wall
// clock: KVM replays lost ticks in bursts, so proc_sleep(N) can return
// immediately under vCPU starvation and a naive `for(;;){ work(); sleep(N); }`
// collapses into a net_lock storm - the exact residual risk #577 left open.
// Gating the WORK on real elapsed monotonic milliseconds makes a collapsed
// sleep cost one compare instead of a lock acquisition.
// ---------------------------------------------------------------------------
#ifndef FLIP_NET_BLOCKING
#define FLIP_NET_BLOCKING 0
#endif

#define NET_PUMP_INTERVAL_MS 10

uint64_t g_net_pump_runs = 0;    // pumps actually performed
uint64_t g_net_pump_early = 0;   // wakeups that were too early to do work

static void net_pump_worker(void *arg) __attribute__((unused));
static void net_pump_worker(void *arg) {
    (void)arg;
    uint64_t last = 0;
    kprintf("[NET] net pump thread running (%ums cadence, mono-paced)\n",
            (unsigned)NET_PUMP_INTERVAL_MS);
    for (;;) {
        uint64_t now = mono_ms();
        if (now - last >= NET_PUMP_INTERVAL_MS) {
            last = now;
            net_poll();
            g_net_pump_runs++;
        } else {
            g_net_pump_early++;
        }
        proc_sleep(NET_PUMP_INTERVAL_MS);
    }
}

// Start the background net worker. Called from kernel_main after preemption is
// enabled (same place as the USB HID poll worker / heartbeat).
void net_start_worker(void) {
    int pid = proc_create("netmon", net_worker, NULL, PRIO_NORMAL);
    kprintf("[NET] background net worker started, pid=%d\n", pid);
    // #155: the USB-Ethernet bulk-IN reaper. Started HERE, not at attach,
    // because it must not exist before the scheduler does: until it runs, RX
    // stays at one TD in flight serviced inline from net_poll(), which is
    // exactly the pre-#155 behaviour, so the pre-scheduler boot/DHCP path gains
    // no new variables. Only started when the USB NIC is actually the active
    // NIC (usb_eth_start_rx_worker no-ops otherwise).
    if (net_get_driver_type() == NET_DRIVER_USB) {
        extern void usb_eth_start_rx_worker(void);
        usb_eth_start_rx_worker();
    }
#if FLIP_NET_BLOCKING
    // #745 (task #62) A/B arm: the pre-fix build has no dedicated pump thread,
    // because before this change sys_fb_flip() was the only thing pumping the
    // stack once the compositor owned the screen (#632).
    (void)net_pump_worker;
    kprintf("[NET] net pump thread DISABLED (FLIP_NET_BLOCKING measurement arm)\n");
#else
    int ppid = proc_create("netpump", net_pump_worker, NULL, PRIO_NORMAL);
    kprintf("[NET] net pump thread started, pid=%d\n", ppid);
#endif
#ifdef RTC_LIVE_TEST
    // #135: `make RTCBINTEST=1` only. One-shot SNTP probe in its own thread,
    // proving end-to-end correction when there is a network and a FAST,
    // clock-untouched failure when there is not. See net/sntp.c.
    extern void sntp_probe_start(void);
    sntp_probe_start();
#endif
}

// ---------------------------------------------------------------------------
// #745 (task #62): net_diag_line - ONE LINE that says WHY the network is down.
//
// The four candidate explanations for "no network on the real iMac" (no
// carrier, DHCP never bound, the dongle never enumerated, a route/DNS failure)
// are distinguishable from facts the kernel already holds, but nothing ever
// printed them together, so every report had to be re-diagnosed from scratch.
// This is the instrument: driver+carrier answers "dongle enumerated / cable
// in", dhcp= answers "did DORA complete", ip/gw/dns answer "is it configured",
// gwarp= answers "does the gateway exist at layer 2" (CACHE-ONLY lookup: it
// must not transmit), and rx/tx answer "did anything ever reach the wire".
// Written into a caller-supplied buffer so the heartbeat owns the printing.
// ---------------------------------------------------------------------------
int net_diag_line(char *buf, unsigned long len) {
    extern int arp_lookup_cached(uint32_t ip, uint8_t *mac);
    if (!buf || len < 96) return 0;
    if (!net_initialized) return snprintf(buf, len, "stack=NOT-INITIALISED");

    uint32_t ip = ip_get_address(), gw = ip_get_gateway(), ds = dns_get_server();
    uint8_t *pi = (uint8_t *)&ip;
    uint8_t *pg = (uint8_t *)&gw;
    uint8_t *pd = (uint8_t *)&ds;
    uint8_t gwmac[6];
    int gwarp = (gw != 0) ? arp_lookup_cached(gw, gwmac) : 0;
    net_driver_type_t dt = net_get_driver_type();
    const char *drv = (dt == NET_DRIVER_E1000)  ? "e1000"  :
                      (dt == NET_DRIVER_VIRTIO) ? "virtio" :
                      (dt == NET_DRIVER_USB)    ? usb_eth_name() : "NONE";
    int carrier = nic_link_up() ? 1 : 0;
    // #NETDROP: distinguish "the link is down" from "the driver has switched
    // its own MMIO off". e1000_link_up() returns 0 for BOTH, and reporting the
    // second as NO-CARRIER is an actively misleading diagnostic: it points at
    // the cable, the switch port and the DHCP lease, none of which are involved.
    int crashctx = (dt == NET_DRIVER_E1000) ? e1000_crash_context_active() : 0;
    const char *state = (g_net_conn_state == NET_STATE_FAULTY) ? "FAULTY" :
                        crashctx  ? "NIC-DISABLED-BY-CRASHCTX" :
                        !carrier  ? "NO-CARRIER" :
                        (ip == 0) ? "NO-ADDRESS" : "UP";
    // #362: usbagg/usbsplit/usbdesync are the USB-Ethernet RX de-framer's own
    // health, on the line that already answers "did anything reach the wire".
    // usbagg is the largest bulk-IN transfer the chip has handed us: it says
    // directly whether the RX buffer is the throughput limit (a value pinned at
    // rx_buf_len means the buffer is the cap, which is exactly the bug the 2048
    // -> 16384 change fixed). usbsplit/usbdesync exist so the split-frame
    // carry-over path is OBSERVABLE rather than a defensive path nobody can
    // tell has never run; on an AX88772B both measured ZERO.
    extern uint64_t g_asix_rx_split_frames, g_asix_rx_desync, g_asix_rx_max_xfer;
    // #155: the PIPELINE's own health, because "how fast is RX" is now a
    // question about queue depth and reap rate, not about buffer size.
    //   usbq    - bulk-IN TDs kept in flight. 1 = the pre-#155 single-TD
    //             behaviour (reaper not started, or /USBNETQ.OFF present),
    //             32 = ramped. This is the FIRST thing to read if throughput
    //             is back at ~3 Mbit.
    //   usbrx   - completions reaped. Divided by uptime this is transfers/s,
    //             the number that was 85/s (one per net_poll tick) before.
    //   usbdry  - service passes that found nothing. A large ratio against
    //             usbrx means the reaper is waking faster than the wire.
    //   usbfd   - frames dropped because the frame FIFO was full, i.e. the
    //             reaper is outrunning net_poll()'s 64-per-pass RX drain.
    //   usbrsy  - full pipe restarts after an endpoint halt / lost completion.
    //             Anything other than 0 wants investigating.
    //   usbref  - passes that had to top the queue back up after a failed TD
    //             submit. Growing steadily means the transfer ring is refusing
    //             enqueues, which would silently drain the depth to zero.
    extern uint64_t g_usbnet_rx_reaped, g_usbnet_rx_dry, g_usbnet_rx_resyncs;
    extern uint64_t g_usbnet_rx_refills;
    extern uint64_t g_usbnet_fifo_drops;
    return snprintf(buf, len,
        "drv=%s carrier=%d state=%s cfg=%s dhcp=%s ip=%d.%d.%d.%d "
        "gw=%d.%d.%d.%d gwarp=%s dns=%d.%d.%d.%d trip=%s/%d "
        "rx=%lu tx=%lu txfail=%lu "
        "nfail=%u ntrip=%u nrec=%u nprobe=%u/%u "
        "usbagg=%lu usbsplit=%lu usbdesync=%lu "
        "usbq=%d usbrx=%lu usbdry=%lu usbfd=%lu usbrsy=%lu usbref=%lu",
        drv, carrier, state,
        g_net_static_configured ? "static" : "dhcp",
        dhcp_is_bound() ? "BOUND" : "unbound",
        pi[3], pi[2], pi[1], pi[0],
        pg[3], pg[2], pg[1], pg[0],
        gwarp ? "RESOLVED" : "UNRESOLVED",
        pd[3], pd[2], pd[1], pd[0],
        g_net_trip_host[0] ? g_net_trip_host : "-",
        g_net_trip_rc,
        (unsigned long)g_net_poll_pkts,
        (unsigned long)g_nic_tx_ok,
        (unsigned long)g_nic_tx_fail,
        (unsigned)g_net_fail_total,
        (unsigned)g_net_trip_count,
        (unsigned)g_net_recover_count,
        (unsigned)g_net_probe_grants,
        (unsigned)g_net_probe_refused,
        (unsigned long)g_asix_rx_max_xfer,
        (unsigned long)g_asix_rx_split_frames,
        (unsigned long)g_asix_rx_desync,
        g_usbnet.rx_qlen,
        (unsigned long)g_usbnet_rx_reaped,
        (unsigned long)g_usbnet_rx_dry,
        (unsigned long)g_usbnet_fifo_drops,
        (unsigned long)g_usbnet_rx_resyncs,
        (unsigned long)g_usbnet_rx_refills);
}

// ---------------------------------------------------------------------------
// net_format_info - build a verbose, human-readable network status report into
// `buf`. Backs the userland `ip` command (SYS_NET_INFO). Returns bytes written.
// ---------------------------------------------------------------------------
int net_format_info(char *buf, unsigned long len) {
    extern uint64_t net_total_bytes(void);
    extern volatile uint64_t timer_ticks;
    extern uint32_t g_timer_hz;
    extern int g_net_static_configured;

    if (!buf || len < 64) return 0;
    unsigned long o = 0;
    #define EMIT(...) do { if (o < len) o += snprintf(buf + o, len - o, __VA_ARGS__); } while (0)

    uint8_t mac[6]; nic_get_mac(mac);
    net_driver_type_t dt = net_get_driver_type();
    const char *drv = (dt == NET_DRIVER_E1000) ? "Intel E1000 (e1000)" :
                      (dt == NET_DRIVER_VIRTIO) ? "VirtIO-net (virtio)" :
                      (dt == NET_DRIVER_USB) ? "USB Ethernet (usb_net)" : "none";
    const char *speed = (dt == NET_DRIVER_E1000) ? "1000 Mb/s (Gigabit, emulated)" :
                        (dt == NET_DRIVER_VIRTIO) ? "paravirtual (host-limited)" :
                        (dt == NET_DRIVER_USB) ? "USB dongle (10/100/1000)" : "n/a";
    int up = nic_link_up();

    uint32_t ip = ip_get_address();
    uint32_t nm = ip_get_netmask();
    uint32_t gw = ip_get_gateway();
    uint32_t dns = dns_get_server();
    uint32_t bc = ip | (~nm);
    int prefix = 0; for (int i = 0; i < 32; i++) if (nm & (1u << i)) prefix++;

    uint8_t *pi = (uint8_t *)&ip, *pn = (uint8_t *)&nm, *pg = (uint8_t *)&gw;
    uint8_t *pd = (uint8_t *)&dns, *pb = (uint8_t *)&bc;

    uint64_t hz = g_timer_hz ? g_timer_hz : 100;
    uint64_t secs = timer_ticks / hz;
    uint64_t d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60, s = secs % 60;
    uint64_t bytes = net_total_bytes();

    EMIT("Network Interface: eth0\n");
    EMIT("=================================================\n");
    EMIT("  Driver:        %s\n", drv);
    EMIT("  MAC Address:   %02x:%02x:%02x:%02x:%02x:%02x\n",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    // #549: a persistently-unreachable interface is reported FAULTY here so the
    // tray icon + Settings Network tab can show "manual configuration required".
    int faulty = (g_net_conn_state == NET_STATE_FAULTY);
    EMIT("  Physical Link: %s\n", up ? "UP (carrier detected)" : "DOWN (no carrier)");
    EMIT("  State:         %s\n",
         faulty ? "FAULTY (unreachable - manual configuration required)" :
         up     ? "RUNNING" : "NO-CARRIER");
    EMIT("  Fault:         %s\n", faulty ? "yes" : "no");   // machine-readable flag for the UI
    EMIT("  Config Method: %s\n", g_net_static_configured ? "static (/CONFIG/NETIP.CFG)" : "DHCP / default");
    EMIT("  IPv4 Address:  %d.%d.%d.%d/%d\n", pi[3], pi[2], pi[1], pi[0], prefix);
    EMIT("  Netmask:       %d.%d.%d.%d\n", pn[3], pn[2], pn[1], pn[0]);
    EMIT("  Broadcast:     %d.%d.%d.%d\n", pb[3], pb[2], pb[1], pb[0]);
    EMIT("  Gateway:       %d.%d.%d.%d\n", pg[3], pg[2], pg[1], pg[0]);
    EMIT("  DNS Server:    %d.%d.%d.%d\n", pd[3], pd[2], pd[1], pd[0]);
    EMIT("  MTU:           1500 bytes (Ethernet II)\n");
    EMIT("  Link Speed:    %s\n", speed);
    EMIT("  TX Queue Len:  1000 packets\n");
    EMIT("  Max Segment:   1460 bytes (TCP MSS)\n");
    EMIT("  Total Traffic: %lu bytes\n", (unsigned long)bytes);
    EMIT("  Uptime:        %lud %luh %lum %lus (since boot)\n",
         (unsigned long)d, (unsigned long)h, (unsigned long)m, (unsigned long)s);

    #undef EMIT
    return (int)o;
}
