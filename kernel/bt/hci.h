// hci.h - HCI layer API (#372)
//
// Owned by the PROTOCOL agent (hci.c). Sits above bt_transport and below
// L2CAP. Responsibilities:
//   - reset + configure the controller (event mask, SSP, scan enable)
//   - issue HCI commands and match command-complete/status
//   - own the connection table (ACL handle <-> bt_addr, classic vs LE)
//   - reassemble ACL fragments into L2CAP PDUs and push them up
//   - fan out selected raw events to observers (pairing layer)
#ifndef HCI_H
#define HCI_H

#include "../types.h"
#include "bt.h"
#include "hci_defs.h"

typedef uint16_t hci_handle_t;
#define HCI_HANDLE_INVALID   0xFFFF
#define HCI_MAX_CONNECTIONS  8

typedef enum {
    HCI_LINK_ACL_CLASSIC = 0,   // BR/EDR
    HCI_LINK_LE,                // Bluetooth Low Energy
} hci_link_type_t;

typedef struct {
    hci_handle_t     handle;
    bt_addr_t        peer;
    hci_link_type_t  type;
    uint8_t          active;      // slot in use
    uint8_t          encrypted;   // link encryption enabled
    uint8_t          role;        // 0 = central/master, 1 = peripheral/slave
    uint8_t          peer_addr_type;  // LE: 0 public, 1 random (needed by SMP c1)
} hci_conn_t;

// -----------------------------------------------------------------------------
// Discovered-device cache (populated from LE advertising reports + classic
// inquiry results). The control layer (bt.c) snapshots this for bt_get_devices;
// bt_connect() uses addr_type to issue LE_Create_Connection.
// -----------------------------------------------------------------------------
typedef struct {
    bt_addr_t addr;
    uint8_t   addr_type;   // LE: 0 public, 1 random
    uint8_t   is_le;       // 1 = BLE, 0 = classic BR/EDR
    int8_t    rssi;        // dBm (0 unknown)
    uint8_t   is_hid;      // advertises HID service 0x1812 / HID appearance
    uint8_t   appearance;  // 1 = keyboard, 2 = mouse, 0 = other/unknown
    uint8_t   adv_type;    // LE adv event type (0x00 = connectable undirected)
    char      name[32];
} hci_disc_dev_t;

// --- Lifecycle ---
int  hci_init(void);          // wires transport RX, resets connection state
void hci_start_bringup(void); // begins the async controller bring-up (called by
                              // the transport once firmware, if any, is loaded)
void hci_poll(void);   // pump: drains pending events, advances reset sequence
int  hci_is_ready(void);

// --- LE scan / connect / encryption (used by bt_ctrl / gatt) ---
int  hci_le_scan(int enable);                 // active LE scan on/off
int  hci_le_connect(const bt_addr_t *addr, uint8_t addr_type);
int  hci_le_start_encryption(hci_handle_t h, const uint8_t rand8[8],
                             uint16_t ediv, const uint8_t ltk16[16]);

// --- Discovered-device cache ---
int  hci_disc_snapshot(hci_disc_dev_t *out, int max);  // returns count written
void hci_disc_clear(void);
int  hci_disc_find(const bt_addr_t *addr, hci_disc_dev_t *out);
// gatt.c registers a callback fired when a HID (0x1812 / kbd/mouse appearance)
// device is discovered, so the stack can auto-target a real keyboard/mouse.
void hci_set_hid_found_cb(void (*cb)(const hci_disc_dev_t *d));

// --- Command path (used by ctrl / pairing / L2CAP signalling) ---
// Queues an HCI command. params may be NULL when plen==0. Returns BT_OK once
// accepted by the transport; completion arrives asynchronously as an event.
int  hci_send_cmd(uint16_t opcode, const uint8_t *params, uint8_t plen);

// --- ACL data path (used by L2CAP) ---
// Sends an L2CAP PDU on an ACL link, fragmenting to the controller ACL MTU.
int  hci_send_acl(hci_handle_t handle, const uint8_t *data, uint16_t len);

// --- Connection table ---
hci_conn_t *hci_conn_by_handle(hci_handle_t h);
hci_conn_t *hci_conn_by_addr(const bt_addr_t *a);
int         hci_conn_count(void);
int         hci_disconnect(hci_handle_t h, uint8_t reason);

// --- Local controller info (valid after hci_is_ready()) ---
const bt_addr_t *hci_local_addr(void);

// -----------------------------------------------------------------------------
// Event observer hook. The pairing layer registers to see raw events it needs
// (IO capability request, user confirm request, encryption change, simple
// pairing complete, LE LTK request). Return 1 if the observer consumed it.
// -----------------------------------------------------------------------------
typedef int (*hci_evt_observer_t)(uint8_t evt, const uint8_t *params, uint8_t plen);
int  hci_add_evt_observer(hci_evt_observer_t cb);

// -----------------------------------------------------------------------------
// L2CAP receive sink. HCI reassembles ACL fragments and delivers each complete
// L2CAP PDU (starting at the L2CAP length field) via this sink. Registered by
// l2cap_init(). Exactly one sink.
// -----------------------------------------------------------------------------
typedef void (*hci_acl_sink_t)(hci_handle_t h, const uint8_t *l2cap_pdu, uint16_t len);
void hci_set_acl_sink(hci_acl_sink_t sink);

// -----------------------------------------------------------------------------
// Connection lifecycle notifier. Upper layers (L2CAP, ctrl) subscribe to learn
// when an ACL/LE link comes up or goes down so they can create/tear down
// channels. up==1 on connect, up==0 on disconnect.
// -----------------------------------------------------------------------------
typedef void (*hci_conn_cb_t)(const hci_conn_t *c, int up);
int  hci_add_conn_observer(hci_conn_cb_t cb);

// -----------------------------------------------------------------------------
// Encryption-change notifier. GATT/HOGP subscribes so it can (re)read/subscribe
// HID characteristics once the link is encrypted (after SMP pairing). enabled==1
// when link encryption is on.
// -----------------------------------------------------------------------------
typedef void (*hci_encrypt_cb_t)(hci_handle_t h, uint8_t enabled);
int  hci_add_encrypt_observer(hci_encrypt_cb_t cb);

#endif // HCI_H
