// net.h - Network stack initialization
#ifndef NET_H
#define NET_H

#include "../types.h"

// Network driver types
typedef enum {
    NET_DRIVER_NONE = 0,
    NET_DRIVER_E1000,
    NET_DRIVER_VIRTIO,
    NET_DRIVER_USB          // #362: USB Ethernet (CDC-ECM / ASIX dongle)
} net_driver_type_t;

// Initialize network stack
int net_init(void);

// Poll network (call regularly to receive packets). BLOCKS on net_lock.
void net_poll(void);

// #745 (task #62): NON-BLOCKING, BOUNDED pump. Returns 1 if it serviced the
// stack, 0 if it declined because net_lock was held by another context. This
// is what the compositor's frame present must call: net_lock() disables
// interrupts BEFORE it spins, so a blocking net_poll() on the present path
// makes the compositor's worst-case frame time the worst case of every network
// context in the kernel. See the long comment in net.c.
int net_poll_try(int max_pkts);

// Non-waiting acquire of the global network lock. 1 = acquired (caller must
// net_unlock()), 0 = held elsewhere, nothing acquired, caller IF restored.
int net_trylock(void);

// #745 (task #62): one-line "why is the network down" report, into buf.
// Distinguishes no-carrier / not-enumerated / DHCP-never-bound / no-route.
int net_diag_line(char *buf, unsigned long len);

// Network configuration
void net_configure(uint32_t ip, uint32_t gateway, uint32_t netmask);

// #380: adopt a static IP only after RFC 5227 duplicate-address detection.
// Returns 0 if adopted (verified free), -1 if the address is already in use.
int net_adopt_static(uint32_t ip, uint32_t gateway, uint32_t netmask);

// Print network status
void net_print_status(void);

// Start DHCP discovery
int net_dhcp(void);

// Start DHCP discovery and wait for completion (blocking)
int net_dhcp_blocking(void);

// Get active driver type
net_driver_type_t net_get_driver_type(void);

// Driver abstraction functions (used by higher layers)
void nic_get_mac(uint8_t *mac);
int nic_send(const void *data, uint16_t length);
int nic_receive(void *buffer, uint16_t buffer_size);
int nic_link_up(void);
// #374: network-up gate (stack up + link + IP). See net.c.
// #549: also returns 0 once the interface has been marked NET_FAULTY by the
// connectivity circuit-breaker (persistent unreachability), so every client that
// already gates on this (haservice, netinfo, ...) quiesces automatically.
int net_is_up(void);

// ---------------------------------------------------------------------------
// #549: connectivity circuit-breaker. Detects a persistently unreachable uplink
// (an IP + link that cannot actually carry traffic - the iMac ICS case where a
// static/DHCP gateway resolves at L2 but nothing beyond it answers), so the OS
// FAILS SAFE AND QUIET instead of busy-retrying forever. On a USB dongle each
// send busy-polls the xHCI up to 40ms (usbnet_bulk_out), so a background
// retry storm (haservice/netinfo) pegged a core; on a reachable net the same
// code is idle. See the long comment block in net.c.
// ---------------------------------------------------------------------------
typedef enum {
    NET_STATE_UP = 0,     // normal: reachable, or no failure streak yet
    NET_STATE_FAULTY = 1  // persistently unreachable: quiesced, manual config req'd
} net_conn_state_t;

// A remote fetch/connect COMPLETED (reached a server: any HTTP status, or a TCP
// handshake). Clears the failure streak and any NET_FAULTY state (auto-recover
// on real connectivity). Cheap; safe to call from the fetch worker threads.
void net_report_reach_ok(void);

// A remote fetch/connect FAILED at the transport level (DNS/connect/recv timeout,
// no server ever reached). After NET_FAIL_STREAK_MAX consecutive failures with no
// intervening success, trips NET_STATE_FAULTY.
void net_report_reach_fail(void);

// browsenet 2026-09-01: same, but records WHICH remote failed, so a trip names
// its cause. Prefer this at any call site that has the URL in scope.
void net_report_reach_fail_url(const char *url);

// Same, plus the transport's own return code, so a trip can say WHICH failure
// class completed the streak. See net.c for why the class matters.
void net_report_reach_fail_rc(const char *url, int rc);

// #549 breaker observability. Read by net_diag_line() (the [NETDIAG] line,
// which main.c already persists to /BOOTLOG.TXT), so these reach a machine
// with no serial port.
uint32_t    net_fault_fail_total(void);
uint32_t    net_fault_trip_count(void);
uint32_t    net_fault_recover_count(void);
uint32_t    net_fault_probe_grants(void);
uint32_t    net_fault_probe_refused(void);
int         net_fault_trip_rc(void);
const char *net_fault_trip_host(void);

// Current connectivity state (for the tray icon + Settings Network tab).
net_conn_state_t net_get_conn_state(void);
int  net_is_faulty(void);   // convenience: net_get_conn_state()==NET_STATE_FAULTY

// #549 FIX: is the WIRE usable (stack up + carrier + address)? This is
// net_is_up() minus the NET_FAULTY clause.
//
// WHICH ONE TO CALL. net_is_up() answers "should a client START background
// work", and it reports DOWN while NET_FAULTY so haservice/netinfo/browser
// quiesce: that is where #549's CPU win comes from, and it stays. The
// TRANSPORT clients (wget_fetch, https_connect, dns_resolve_start) must ask
// net_wire_usable() instead, because they run BELOW the initiation gate that
// already enforced the FAULTY policy. When they tested net_is_up() they
// re-applied that policy a second time and vetoed the very re-probe
// net_fetch_probe_take() had just authorised, which made the paced probe a
// no-op: it reached the syscall and died one frame later with
// WGET_ERR_NO_NETWORK, never emitting a packet. Rate limiting still holds,
// because the outer gate admits at most one request per 30s while FAULTY.
int net_wire_usable(void);

// #549 FIX: may THIS request go on the wire? 1 = yes (and, while NET_FAULTY,
// consumes the one-re-probe-per-30s budget); 0 = refuse. Returns 1 unmodified
// when the interface is healthy, so the healthy path costs one compare. The
// budget exists because the FAULTY gate used to suppress the only evidence
// (a completed transfer) that clears FAULTY, making the state self-latching.
int net_fetch_probe_take(void);

// Manual recovery: clear NET_FAULTY + the failure streak and re-enable the
// interface. Called on an explicit reconnect, on Settings applying a static IP,
// on a fresh DHCP bind, and on a carrier down->up transition.
void net_clear_fault(void);

// #786: PERSIST the live IPv4 configuration to /CONFIG/NETIP.CFG, the file
// net_apply_static_config() reads at boot. Returns 0 on success, -1 on a write
// failure, and -2 when there is no FAT/ext2 root to write to.
//
// WHY THE KERNEL OWNS THIS FILE AND RING 3 DOES NOT (the whole point of #786).
// Settings used to write it itself, and the write SILENTLY FAILED for every
// non-root user: /CONFIG is root-owned mode 0711 in /CONFIG/PERMS.DB, so
// sys_open(O_CREAT) on a path under it is refused for uid != 0. Worse, the
// app's own failure breadcrumb (/SETLOG.TXT) is under "/" (root, 0755) and was
// refused too, so the failure left NO trace anywhere: the panel updated, the
// file never appeared, and nothing was logged. Measured on VM <vmid> after a
// real click on OK in Settings > Network > Configure IP.
//
// Persisting from Ring 0, in the SAME syscall that applies the change, makes
// "what is running" and "what boots" the same thing by construction, and there
// is exactly ONE writer of the file instead of two that can disagree.
//
// IT DOES NOT INVENT A STATIC CONFIG. If the machine is on DHCP it writes ONLY
// the dns= line, so the next boot still runs DHCP with the chosen resolver.
// Writing ip=/mask=/gw= from a DHCP lease would pin a borrowed address as a
// static one, which is precisely the trap that left a golden hard-coded to
// 192.0.2.1 and unable to reach any other LAN (blame.md 2026, #549 note).
int net_persist_netcfg(void);

// #381: start the background net worker (USB carrier polling + async DHCP/DAD).
// Call once after preemption is enabled. See net.c.
void net_start_worker(void);

// Hot-plug NIC attach (no ticket, 2026-08-28). net_has_nic() is the single
// definition of "a NIC is bound", asked by the USB probe path before it arms an
// attach for the background net worker. netattach_log_stats() writes the attach
// census to the DURABLE bootlog.
int  net_has_nic(void);
void netattach_log_stats(const char *why);
// Boot self-test for the attach handoff (durable [NETATTACH-SELFTEST] line).
void netattach_selftest(void);

#endif // NET_H
