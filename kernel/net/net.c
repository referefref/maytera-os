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
#include "https.h"
#include "ftp.h"
#include "wget.h"
#include "../crypto/crypto.h"
#include "../drivers/pci.h"
#include "../drivers/usb_net.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"
#include "../mm/heap.h"
#include "../proc/process.h"   // #381: proc_create/proc_sleep/PRIO_* for net worker

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
// fallback <LAB_HOST> / gw <LAB_GATEWAY> (that exact pair == the .200-.209 /
// gw .1 fallback below, NOT a lease). That gateway resolves at L2 but nothing
// beyond it answers - the real internet is only via a different ICS box. The two
// autostarted background fetchers (haservice polling Home Assistant every 10s,
// netinfo polling weather/crypto) therefore never reach their targets. Each
// failed fetch drives DNS/SYN retransmits; on the iMac's USB Ethernet dongle
// every single send busy-polls the xHCI up to 40ms (usbnet_bulk_out), so the
// retry storm pegged a core. (On an e1000 VM the same sends are cheap MMIO, which
// is why VM 2200 and an e1000 repro stay idle - the spin is USB-amplified.)
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
static volatile net_conn_state_t g_net_conn_state = NET_STATE_UP;
static volatile uint32_t g_net_fail_streak = 0;

void net_report_reach_ok(void) {
    g_net_fail_streak = 0;
    if (g_net_conn_state != NET_STATE_UP) {
        g_net_conn_state = NET_STATE_UP;
        kprintf("[NET] connectivity restored; interface re-enabled (was FAULTY)\n");
    }
}

void net_report_reach_fail(void) {
    if (g_net_conn_state == NET_STATE_FAULTY) return;   // already tripped; stay quiet
    if (g_net_fail_streak < 0xFFFFFFFFu) g_net_fail_streak++;
    if (g_net_fail_streak >= NET_FAIL_STREAK_MAX) {
        g_net_conn_state = NET_STATE_FAULTY;
        kprintf("[NET] %u consecutive unreachable-remote failures: marking interface "
                "NET_FAULTY (manual configuration required). Retries stopped; "
                "net_is_up() now reports DOWN so background fetchers quiesce.\n",
                (unsigned)g_net_fail_streak);
    }
}

net_conn_state_t net_get_conn_state(void) { return g_net_conn_state; }
int net_is_faulty(void) { return g_net_conn_state == NET_STATE_FAULTY; }

void net_clear_fault(void) {
    g_net_fail_streak = 0;
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

    // Require at least a valid IP. Default mask/gateway if omitted.
    if (!have_ip) {
        kprintf("[NET] %s present but no valid ip= line; ignoring\n", src);
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

// Initialize network stack
int net_init(void) {
    // Enable serial output for network debugging
    kprintf_set_dual_output(1);
    kprintf("\n[NET] Initializing network stack...\n");

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
    g_net_static_configured = net_apply_static_config();

    // Send test ARP packet to verify TX works
    kprintf("[NET] Sending test ARP packet...\n");
    uint8_t test_mac[6];
    nic_get_mac(test_mac);
    uint32_t test_ip = 0xC0A80101;  // 192.0.2.1
    arp_request(test_ip);
    kprintf("[NET] Test ARP request sent for 192.0.2.1\n");
    kprintf("[NET] net_init complete, disabling dual output\n");
    kprintf_set_dual_output(0); // Disable dual output after init

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
void net_lock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint32_t cpu = smp_this_cpu();
    if (g_net_lock_owner == cpu) { g_net_lock_depth++; return; }  // recursive re-entry
    spinlock_acquire(&g_net_lock);
    g_net_lock_owner = cpu;
    g_net_lock_depth = 1;
    g_net_lock_saved_flags = flags;   // remember caller IF for the outermost release
}

void net_unlock(void) {
    uint32_t cpu = smp_this_cpu();
    if (g_net_lock_owner != cpu) return;   // defensive: not the owner
    if (--g_net_lock_depth > 0) return;
    uint64_t flags = g_net_lock_saved_flags;
    g_net_lock_owner = 0xFFFFFFFF;
    spinlock_release(&g_net_lock);
    if (flags & 0x200) __asm__ volatile("sti");
}

void net_poll(void) {
    if (!net_initialized) return;

    net_lock();
    // Drain up to 64 packets per poll (bounded to prevent desktop starvation)
    for (int i = 0; i < 64; i++) { if (!eth_receive()) break; }
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
    ip_set_address(ip);
    ip_set_gateway(gateway);
    ip_set_netmask(netmask);
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

int nic_send(const void *data, uint16_t length) {
    if (driver_send) {
        net_lock();
        int r = driver_send(data, length);
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
int net_is_up(void) {
    if (!net_initialized) return 0;
    if (!nic_link_up()) return 0;
    if (ip_get_address() == 0) return 0;
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

static void net_worker(void *arg) {
    (void)arg;
    int prev_link = -1;
    int up_secs = 0;      // seconds since carrier came up (static-fallback timer)
    int dad_done = 0;     // static DAD fallback already attempted for this link-up

    kprintf("[NET] background net worker running\n");
    for (;;) {
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
            if (!dhcp_is_bound() && !g_net_static_configured && ip_get_address() == 0) {
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
            if (!dad_done && up_secs >= 12) {
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
                    if (net_adopt_static(0xC0A80100u | h, 0xC0A80101u,
                                         0xFFFFFF00u) == 0) { adopted = 1; break; }
                }
                if (!adopted)
                    kprintf("[NET] static fallback .200-.209 all taken; "
                            "interface left unconfigured\n");
            }
        }

        proc_sleep(1000);
    }
}

// Start the background net worker. Called from kernel_main after preemption is
// enabled (same place as the USB HID poll worker / heartbeat).
void net_start_worker(void) {
    int pid = proc_create("netmon", net_worker, NULL, PRIO_NORMAL);
    kprintf("[NET] background net worker started, pid=%d\n", pid);
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
