// bt_transport.h - HCI transport abstraction (#372)
//
// CONTRACT between the USB BT controller driver (hci_usb.c, owned by the
// TRANSPORT agent) and the HCI layer (hci.c, owned by the PROTOCOL agent).
//
// A USB Bluetooth dongle exposes four endpoints:
//   - EP0 control          -> HCI commands  (send_cmd)
//   - Interrupt IN         -> HCI events    (delivered via on_event upcall)
//   - Bulk OUT             -> ACL data out  (send_acl)
//   - Bulk IN              -> ACL data in   (delivered via on_acl upcall)
//   - Isochronous IN/OUT   -> SCO audio     (NOT used in phase 1)
//
// The transport driver enumerates the dongle, then calls
// bt_transport_register() with its ops. The HCI layer calls
// bt_transport_set_rx() with the callbacks the transport must invoke on
// inbound event/ACL data. Neither side knows the other concretely: this header
// is the only coupling.
#ifndef BT_TRANSPORT_H
#define BT_TRANSPORT_H

#include "../types.h"
#include "bt.h"

// HCI packet type indicators (H:4). USB puts each class on its own endpoint,
// but the callbacks are tagged so the HCI layer stays transport-agnostic and a
// future UART/H4 transport can multiplex on one pipe.
#define BT_HCI_PKT_CMD    0x01
#define BT_HCI_PKT_ACL    0x02
#define BT_HCI_PKT_SCO    0x03
#define BT_HCI_PKT_EVENT  0x04

// -----------------------------------------------------------------------------
// Downcalls: HCI -> transport. Implemented by hci_usb.c.
// send_cmd / send_acl copy the buffer (may return before the xfer completes)
// and return BT_OK on success. poll() pumps the RX endpoints (called from the
// bt worker); it invokes the RX upcalls for any completed transfers.
// -----------------------------------------------------------------------------
typedef struct {
    const char *name;
    int  (*send_cmd)(const uint8_t *data, uint16_t len);
    int  (*send_acl)(const uint8_t *data, uint16_t len);
    void (*poll)(void);
    int  (*get_bdaddr)(bt_addr_t *out);   // optional (may be NULL)
} bt_transport_ops_t;

// -----------------------------------------------------------------------------
// Upcalls: transport -> HCI. Registered by hci_init() via bt_transport_set_rx().
// on_event delivers ONE complete HCI event packet (evt + plen + params, no H4
// type byte). on_acl delivers ONE complete HCI ACL packet (handle+flags, dlen,
// payload).
// -----------------------------------------------------------------------------
typedef struct {
    void (*on_event)(const uint8_t *data, uint16_t len);
    void (*on_acl)(const uint8_t *data, uint16_t len);
} bt_transport_rx_t;

// --- Registration (transport side) ---------------------------------------
int  bt_transport_register(const bt_transport_ops_t *ops);
void bt_transport_unregister(void);
const bt_transport_ops_t *bt_transport_get(void);
int  bt_transport_present(void);

// --- RX wiring (HCI side) -------------------------------------------------
void bt_transport_set_rx(const bt_transport_rx_t *rx);

// --- Inbound delivery helpers (transport calls these from its poll()) -----
// These forward to the registered RX callbacks; safe to call before HCI has
// wired RX (they no-op), so the transport driver never needs a NULL check.
void bt_transport_deliver_event(const uint8_t *data, uint16_t len);
void bt_transport_deliver_acl(const uint8_t *data, uint16_t len);

#endif // BT_TRANSPORT_H
