// pair.h - Bluetooth pairing API (#372)
//
// Owned by the PROTOCOL agent (pair.c). Two pairing mechanisms:
//   - CLASSIC: Secure Simple Pairing (SSP). Driven by HCI events (IO capability
//     request, user confirmation request, simple pairing complete). Numeric
//     comparison / Just Works are the near-term association models.
//   - BLE: Security Manager Protocol (SMP) over L2CAP fixed CID 0x0006.
//
// The pairing layer subscribes to HCI events (via hci_add_evt_observer) and to
// the LE SMP L2CAP channel. UI confirmation is plumbed from bt_ctrl.h's
// bt_set_pair_confirm_cb(): pair.c invokes that callback to ask the user to
// accept a passkey / numeric-compare value.
#ifndef BT_PAIR_H
#define BT_PAIR_H

#include "../types.h"
#include "bt.h"
#include "hci.h"

// --- SSP IO capabilities (classic) ---
#define BT_IO_CAP_DISPLAY_ONLY      0x00
#define BT_IO_CAP_DISPLAY_YESNO     0x01
#define BT_IO_CAP_KEYBOARD_ONLY     0x02
#define BT_IO_CAP_NO_IO             0x03

typedef enum {
    BT_PAIR_IDLE = 0,
    BT_PAIR_SSP_IN_PROGRESS,   // classic Secure Simple Pairing
    BT_PAIR_SMP_IN_PROGRESS,   // BLE Security Manager
    BT_PAIR_BONDED,            // link key / LTK stored
    BT_PAIR_FAILED,
} bt_pair_state_t;

int  pair_init(void);   // registers HCI event observer + SMP channel handler
void pair_poll(void);

// Start pairing on an established link.
int  pair_start_classic(hci_handle_t h, const bt_addr_t *addr);   // SSP
int  pair_start_le(hci_handle_t h, const bt_addr_t *addr);        // SMP

bt_pair_state_t pair_state(const bt_addr_t *addr);

// Called (indirectly, from the UI via bt_ctrl) to accept/reject a pending
// numeric-comparison / passkey prompt. accept != 0 confirms.
int  pair_user_confirm(const bt_addr_t *addr, int accept);

// True if we hold a bond (link key / LTK) for this address.
int  pair_is_bonded(const bt_addr_t *addr);
int  pair_forget(const bt_addr_t *addr);   // drop stored bond

#endif // BT_PAIR_H
