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

// Rich adapter/stack state for the Settings UI. Distinct from bt_state_t (the
// coarse state machine) because the UI needs the individual facts: is a dongle
// present, has the controller finished bring-up, is the master enable on, is a
// scan running, and the local BD_ADDR. Filled by bt_get_state_info().
typedef struct {
    uint8_t   present;      // 1 = HCI transport/dongle present
    uint8_t   driver_up;    // 1 = controller finished bring-up (hci_is_ready)
    uint8_t   enabled;      // 1 = g_bt_enable (master enable, the UI toggle)
    uint8_t   scanning;     // 1 = discovery running
    int32_t   state;        // bt_state_t value
    bt_addr_t local_addr;   // controller BD_ADDR (valid when driver_up)
} bt_state_info_t;

// -----------------------------------------------------------------------------
// SYS_BT subcommands (arg1 of the SYS_BT multiplexed syscall). Mirrored 1:1 in
// userland/libc/bt_client.h. arg2/arg3 carry the per-command operands; every
// user pointer is validated by the kernel handler via copy_from_user/
// copy_to_user, so this ABI stays flat and needs no argtab descriptor.
// -----------------------------------------------------------------------------
#define BT_CMD_POWER        1   // arg2 = on(1)/off(0)                 -> BT_OK/err
#define BT_CMD_IS_POWERED   2   //                                     -> 0/1
#define BT_CMD_SCAN_START   3   //                                     -> BT_OK/err
#define BT_CMD_SCAN_STOP    4   //                                     -> BT_OK/err
#define BT_CMD_SCAN_ACTIVE  5   //                                     -> 0/1
#define BT_CMD_GET_STATE    6   // arg2 = bt_state_info_t* (out)       -> BT_OK/err
#define BT_CMD_GET_DEVICES  7   // arg2 = bt_device_t* buf, arg3 = max -> count
#define BT_CMD_PAIR         8   // arg2 = bt_addr_t* (in)              -> BT_OK/err
#define BT_CMD_CONNECT      9   // arg2 = bt_addr_t* (in)              -> BT_OK/err
#define BT_CMD_DISCONNECT   10  // arg2 = bt_addr_t* (in)              -> BT_OK/err
#define BT_CMD_FORGET       11  // arg2 = bt_addr_t* (in)              -> BT_OK/err
#define BT_CMD_STATUS       12  //                                     -> bt_state_t

// --- Radio power. bt_power(1) sets g_bt_enable and runs bt_init(); bt_power(0)
//     tears the stack down. ---
int  bt_power(int on);
int  bt_is_powered(void);

// --- Discovery ---
int  bt_scan_start(void);
int  bt_scan_stop(void);
int  bt_scan_active(void);
// bt-settings-ux: true once bt_scan_stop() has run and no bt_scan_start() /
// bt_power() transition has happened since. The bt worker (hci_usb.c) checks
// this before its own opportunistic idle rescan so an explicit Stop actually
// stays stopped instead of being silently restarted a few ticks later.
int  bt_scan_user_stopped(void);
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
// Fill the rich adapter/stack state for the UI. Returns BT_OK (never gated: it
// reports the disabled/absent state honestly rather than failing).
int  bt_get_state_info(bt_state_info_t *out);

// -----------------------------------------------------------------------------
// Pairing confirmation callback. The UI registers this so it can prompt the
// user for SSP numeric-comparison / SMP passkey. Return non-zero to accept.
// If no callback is registered, the stack uses a Just-Works / auto-accept
// policy (phase-1 convenience; tightened later).
// -----------------------------------------------------------------------------
typedef int (*bt_pair_confirm_cb_t)(const bt_addr_t *addr, uint32_t passkey);
void bt_set_pair_confirm_cb(bt_pair_confirm_cb_t cb);

#endif // BT_CTRL_H
