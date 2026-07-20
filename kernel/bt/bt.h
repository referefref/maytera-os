// bt.h - MayteraOS Bluetooth stack, top-level init/state (#372)
//
// This is the ARCHITECT-owned contract header. The layered stack is:
//
//   USB HCI transport (hci_usb.c)  -- bt_transport.h
//        |
//   HCI  (hci.c)                   -- hci.h / hci_defs.h  (cmd/event/ACL)
//        |
//   L2CAP (l2cap.c)                -- l2cap.h             (channels/PSM/CID)
//        |
//   +-- SDP (sdp.c)                -- sdp.h
//   +-- HID profile (hid.c)        -- hid.h  (HIDP classic + HOGP/BLE)
//   +-- Pairing (pair.c)           -- pair.h (SSP classic + SMP BLE)
//        |
//   Control API (bt.c)             -- bt_ctrl.h (power/scan/pair/connect/forget)
//
// SAFETY: the whole stack is gated behind g_bt_enable (default 0). Boot never
// enters the stack unless that flag is 1, so an incomplete stack can NEVER
// break boot. See main.c for the gated bt_init() call.
#ifndef BT_H
#define BT_H

#include "../types.h"

// -----------------------------------------------------------------------------
// Master enable flag. Defined in bt.c, default 0 (OFF). Set to 1 (e.g. from a
// syscall / settings toggle via bt_power(1)) to bring the radio up.
// -----------------------------------------------------------------------------
extern volatile int g_bt_enable;

// -----------------------------------------------------------------------------
// Shared return codes across the whole stack. BT_OK == 0, errors negative.
// -----------------------------------------------------------------------------
#define BT_OK             0
#define BT_ERR_NOTIMPL   -1   // stub not implemented yet
#define BT_ERR_NODEV     -2   // no controller / device
#define BT_ERR_NOMEM     -3
#define BT_ERR_TIMEOUT   -4
#define BT_ERR_STATE     -5   // wrong state for this operation
#define BT_ERR_PARAM     -6
#define BT_ERR_HCI       -7   // controller returned a non-zero HCI status
#define BT_ERR_DISABLED  -8   // g_bt_enable == 0

// -----------------------------------------------------------------------------
// 48-bit Bluetooth device address. Stored little-endian (LAP byte first), the
// order the HCI wire format uses.
// -----------------------------------------------------------------------------
typedef struct { uint8_t b[6]; } bt_addr_t;

// -----------------------------------------------------------------------------
// Top-level stack state.
// -----------------------------------------------------------------------------
typedef enum {
    BT_STATE_OFF = 0,       // radio off / stack not initialised
    BT_STATE_INIT,          // transport bound, HCI reset in progress
    BT_STATE_READY,         // controller up, idle
    BT_STATE_SCANNING,      // inquiry (classic) / LE scan running
    BT_STATE_CONNECTING,    // ACL/LE connection being established
    BT_STATE_CONNECTED,     // at least one active link
    BT_STATE_ERROR,         // unrecoverable
} bt_state_t;

// -----------------------------------------------------------------------------
// Lifecycle. bt_init() is gated: it returns BT_ERR_DISABLED immediately if
// g_bt_enable == 0. bt_poll() is the pump, driven from a dedicated bt worker
// thread (see INTEGRATION.md); it fans out to transport->poll / hci_poll /
// l2cap_poll / sdp_poll.
// -----------------------------------------------------------------------------
int         bt_init(void);
void        bt_shutdown(void);
void        bt_poll(void);
bt_state_t  bt_get_state(void);
void        bt_set_state(bt_state_t s);   // used by layers to publish progress
const char *bt_state_str(bt_state_t s);

// Convenience: compare / format addresses.
int  bt_addr_eq(const bt_addr_t *a, const bt_addr_t *b);
int  bt_addr_is_zero(const bt_addr_t *a);

#endif // BT_H
