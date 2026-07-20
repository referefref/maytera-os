// bt_ctrl.h - Bluetooth control API for the UI / syscalls (#372)
//
// Owned by the ARCHITECT (implemented in bt.c). This is the high-level surface
// the Settings app / a future SYS_BT_* syscall group drives. It hides the
// HCI/L2CAP/profile machinery behind power / scan / pair / connect / forget.
#ifndef BT_CTRL_H
#define BT_CTRL_H

#include "../types.h"
#include "bt.h"

#define BT_NAME_MAX     32
#define BT_MAX_DEVICES  16

// Coarse device class for a friendly icon in the UI.
typedef enum {
    BT_DEV_UNKNOWN = 0,
    BT_DEV_KEYBOARD,
    BT_DEV_MOUSE,
    BT_DEV_AUDIO,
    BT_DEV_PHONE,
    BT_DEV_COMPUTER,
} bt_dev_class_t;

// Per-device link status shown in the device list.
typedef enum {
    BT_LINK_NONE = 0,
    BT_LINK_FOUND,       // seen during scan, not paired
    BT_LINK_PAIRED,      // bonded, not currently connected
    BT_LINK_CONNECTED,   // active link
} bt_link_state_t;

typedef struct {
    bt_addr_t        addr;
    char             name[BT_NAME_MAX];
    bt_dev_class_t   cls;
    bt_link_state_t  link;
    int8_t           rssi;        // dBm, 0 if unknown
    uint8_t          is_le;       // 1 = BLE, 0 = classic BR/EDR
    uint8_t          paired;
    uint8_t          connected;
} bt_device_t;

// --- Radio power. bt_power(1) sets g_bt_enable and runs bt_init(); bt_power(0)
//     tears the stack down. ---
int  bt_power(int on);
int  bt_is_powered(void);

// --- Discovery ---
int  bt_scan_start(void);
int  bt_scan_stop(void);
int  bt_scan_active(void);
// Snapshot the current device list into out[0..max). Returns the count written.
int  bt_get_devices(bt_device_t *out, int max);

// --- Pairing / connection lifecycle (all addressed by bt_addr_t) ---
int  bt_pair(const bt_addr_t *addr);
int  bt_connect(const bt_addr_t *addr);
int  bt_disconnect_dev(const bt_addr_t *addr);
int  bt_forget(const bt_addr_t *addr);

// --- Status ---
bt_state_t bt_status(void);
int  bt_get_device(const bt_addr_t *addr, bt_device_t *out);

// -----------------------------------------------------------------------------
// Pairing confirmation callback. The UI registers this so it can prompt the
// user for SSP numeric-comparison / SMP passkey. Return non-zero to accept.
// If no callback is registered, the stack uses a Just-Works / auto-accept
// policy (phase-1 convenience; tightened later).
// -----------------------------------------------------------------------------
typedef int (*bt_pair_confirm_cb_t)(const bt_addr_t *addr, uint32_t passkey);
void bt_set_pair_confirm_cb(bt_pair_confirm_cb_t cb);

#endif // BT_CTRL_H
