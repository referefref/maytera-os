// gatt.h - Minimal ATT/GATT client + HOGP (HID over GATT) for BLE (#372).
//
// Owned by the PROTOCOL agent (gatt.c). Drives the BLE HID path end to end:
//   LE link up -> Exchange MTU -> discover services (find HID 0x1812)
//   -> discover characteristics (report / boot report / protocol mode / CCCDs)
//   -> write Protocol Mode = Boot -> subscribe CCCDs (enable notifications)
//   -> Handle-Value-Notifications carry boot-protocol input reports, funnelled
//      through bt_hid_input_report() into the same input queue as USB HID.
//
// If an ATT operation returns Insufficient Authentication/Encryption, gatt.c
// kicks BLE SMP pairing (pair_start_le) and resumes once the link is encrypted.
#ifndef BT_GATT_H
#define BT_GATT_H

#include "../types.h"
#include "bt.h"
#include "hci.h"

int  gatt_init(void);   // registers conn/encrypt observers + HID auto-target
void gatt_poll(void);

// Inbound ATT PDU on the fixed ATT channel (CID 0x0004), from l2cap_input.
void gatt_att_input(hci_handle_t h, const uint8_t *data, uint16_t len);

#endif // BT_GATT_H
