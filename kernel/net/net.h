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

// Poll network (call regularly to receive packets)
void net_poll(void);

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

// Current connectivity state (for the tray icon + Settings Network tab).
net_conn_state_t net_get_conn_state(void);
int  net_is_faulty(void);   // convenience: net_get_conn_state()==NET_STATE_FAULTY

// Manual recovery: clear NET_FAULTY + the failure streak and re-enable the
// interface. Called on an explicit reconnect, on Settings applying a static IP,
// on a fresh DHCP bind, and on a carrier down->up transition.
void net_clear_fault(void);

// #381: start the background net worker (USB carrier polling + async DHCP/DAD).
// Call once after preemption is enabled. See net.c.
void net_start_worker(void);

#endif // NET_H
