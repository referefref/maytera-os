// hid.h - Bluetooth HID profile API (#372)
//
// Owned by the PROTOCOL agent (hid.c). Two transports for keyboard/mouse:
//   - CLASSIC HIDP over L2CAP: control PSM 0x0011 + interrupt PSM 0x0013.
//     Input reports arrive as HIDP DATA/INPUT transactions on the interrupt
//     channel. Used by older BT keyboards/mice.
//   - BLE HOGP over GATT/ATT: input reports arrive as ATT notifications on the
//     HID Report characteristic. Used by most modern BT input devices.
//
// Either way, the parsed 8-byte boot keyboard report / 3-4 byte boot mouse
// report is funnelled through bt_hid_input_report(), which reuses the SAME
// downstream path as USB HID:
//   - keyboard: HID usage -> PS/2 set-1 scancode -> keyboard_process_scancode()
//   - mouse:    mouse_inject_hid(dx, dy, buttons, wheel)
// so Bluetooth input is byte-identical to USB/PS2 input to the compositor.
#ifndef BT_HID_H
#define BT_HID_H

#include "../types.h"
#include "bt.h"
#include "hci.h"
#include "l2cap.h"

// --- HIDP (classic) transaction header: high nibble = type, low nibble = param.
#define HIDP_HDR_HANDSHAKE    0x00
#define HIDP_HDR_CONTROL      0x10
#define HIDP_HDR_GET_REPORT   0x40
#define HIDP_HDR_SET_REPORT   0x50
#define HIDP_HDR_DATA         0xA0
// Report type (low nibble of DATA/GET/SET headers).
#define HIDP_RTYPE_OTHER      0x00
#define HIDP_RTYPE_INPUT      0x01
#define HIDP_RTYPE_OUTPUT     0x02
#define HIDP_RTYPE_FEATURE    0x03

typedef enum {
    BT_HID_OTHER    = 0,
    BT_HID_KEYBOARD = 1,
    BT_HID_MOUSE    = 2,
} bt_hid_kind_t;

typedef enum {
    BT_HID_TRANSPORT_CLASSIC = 0,   // HIDP over L2CAP
    BT_HID_TRANSPORT_BLE     = 1,   // HOGP over GATT
} bt_hid_transport_t;

#define BT_HID_MAX_DEVICES  4

typedef struct {
    hci_handle_t        handle;
    bt_addr_t           addr;
    bt_hid_kind_t       kind;
    bt_hid_transport_t  transport;
    uint8_t             active;
    l2cap_chan_t       *ctrl;   // classic HIDP control channel (NULL for BLE)
    l2cap_chan_t       *intr;   // classic HIDP interrupt channel (NULL for BLE)
} bt_hid_dev_t;

int  bt_hid_init(void);
void bt_hid_poll(void);

// --- CLASSIC: called once L2CAP control + interrupt channels are OPEN. The HID
//     profile then puts the device in boot protocol and starts consuming input.
int  bt_hid_attach_classic(hci_handle_t h, const bt_addr_t *addr,
                           l2cap_chan_t *ctrl, l2cap_chan_t *intr);

// --- BLE: called by the HOGP/GATT layer once the HID service is discovered and
//     report notifications are enabled.
int  bt_hid_attach_ble(hci_handle_t h, const bt_addr_t *addr, bt_hid_kind_t kind);

void bt_hid_detach(hci_handle_t h);
int  bt_hid_device_count(void);

// -----------------------------------------------------------------------------
// Feed a raw boot-protocol INPUT report into the system input queue. This is
// the single choke point that both transports call and the ONLY place that
// touches keyboard_process_scancode() / mouse_inject_hid(). kind selects the
// parser; report/len is the raw HID input report payload.
// -----------------------------------------------------------------------------
void bt_hid_input_report(bt_hid_kind_t kind, const uint8_t *report, uint16_t len);

#endif // BT_HID_H
