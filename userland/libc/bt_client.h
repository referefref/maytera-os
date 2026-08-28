// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// bt_client.h - userland Bluetooth client API + HONEST STUB backend (#237)
// =============================================================================
// The Settings "Bluetooth" panel and the tray indicator code against THIS
// header. It MIRRORS the architect's kernel contract (bt/bt_ctrl.h + bt/bt.h)
// one-to-one -- same struct layout, same enum names/values, same function
// names/semantics -- so that when the SYS_BT_* syscall group lands, swapping
// this stub for the real stack is a one-line change PER FUNCTION:
//
//     int bt_power(int on)            { return (int)syscall1(SYS_BT_POWER, on); }
//     int bt_get_devices(bt_device_t *o,int m){ return (int)syscall2(SYS_BT_GET_DEVICES,(long)o,m); }
//     ... etc.
//
// Nothing in the UI touches this file's internals; it only calls the
// functions declared here. There is no real Bluetooth driver/stack yet, so
// every function below is a STUB: it returns a fixed, honest "not available"
// result. It used to be a MOCK (BT_MOCK_IMPL, #372) that invented an adapter
// and a handful of devices (a Magic Keyboard, a BT mouse, headphones, ...)
// with simulated scan/pair/connect transitions - #237 deleted that fiction.
// See docs/SETTINGS_CONTROL_AUDIT.md's Bluetooth section and blame.md #230/
// #237 for the history.
//
// Single-header library: exactly ONE translation unit per program defines
// BT_STUB_IMPL before including this file; every other TU includes it plainly.
// =============================================================================
#ifndef BT_CLIENT_H
#define BT_CLIENT_H

// Types/values below are byte-for-byte the architect's bt/bt.h + bt/bt_ctrl.h,
// re-declared here for userland (which cannot include the kernel headers).

#define BT_NAME_MAX     32
#define BT_MAX_DEVICES  16

// 48-bit device address (matches bt.h bt_addr_t; little-endian LAP-first).
typedef struct { unsigned char b[6]; } bt_addr_t;

// Top-level stack state (matches bt.h bt_state_t).
typedef enum {
    BT_STATE_OFF = 0,
    BT_STATE_INIT,
    BT_STATE_READY,
    BT_STATE_SCANNING,
    BT_STATE_CONNECTING,
    BT_STATE_CONNECTED,
    BT_STATE_ERROR,
} bt_state_t;

// Coarse device class for a friendly icon (matches bt_ctrl.h bt_dev_class_t).
typedef enum {
    BT_DEV_UNKNOWN = 0,
    BT_DEV_KEYBOARD,
    BT_DEV_MOUSE,
    BT_DEV_AUDIO,
    BT_DEV_PHONE,
    BT_DEV_COMPUTER,
} bt_dev_class_t;

// Per-device link status (matches bt_ctrl.h bt_link_state_t).
typedef enum {
    BT_LINK_NONE = 0,
    BT_LINK_FOUND,
    BT_LINK_PAIRED,
    BT_LINK_CONNECTED,
} bt_link_state_t;

// Device record (matches bt_ctrl.h bt_device_t exactly; do not reorder).
typedef struct {
    bt_addr_t        addr;
    char             name[BT_NAME_MAX];
    bt_dev_class_t   cls;
    bt_link_state_t  link;
    signed char      rssi;        // dBm, 0 if unknown
    unsigned char    is_le;       // 1 = BLE, 0 = classic BR/EDR
    unsigned char    paired;
    unsigned char    connected;
} bt_device_t;

// ---- Radio power (bt_ctrl.h) ---------------------------------------------
int  bt_power(int on);       // 1 = on, 0 = off. Returns 0 (BT_OK) on success.
int  bt_is_powered(void);    // 1 if powered on

// ---- Discovery (bt_ctrl.h) -----------------------------------------------
int  bt_scan_start(void);
int  bt_scan_stop(void);
int  bt_scan_active(void);
int  bt_get_devices(bt_device_t *out, int max);   // count written

// ---- Pairing / connection lifecycle (bt_ctrl.h) --------------------------
int  bt_pair(const bt_addr_t *addr);
int  bt_connect(const bt_addr_t *addr);
int  bt_disconnect_dev(const bt_addr_t *addr);
int  bt_forget(const bt_addr_t *addr);

// ---- Status (bt_ctrl.h) --------------------------------------------------
bt_state_t bt_status(void);
int  bt_get_device(const bt_addr_t *addr, bt_device_t *out);

// ---- Address helpers (bt.h) ----------------------------------------------
int  bt_addr_eq(const bt_addr_t *a, const bt_addr_t *b);
void bt_addr_fmt(const bt_addr_t *a, char *out);   // "00:00:5E:00:53:00" (18 bytes)

// ---- Frontend-only helpers (NOT part of the kernel contract) -------------
// bt_tick(): currently a no-op (no driver state to pump). Under the REAL
// stack the kernel bt worker pumps bt_poll(), so this becomes a cheap status
// re-read; the UI keeps calling it either way.
void bt_tick(void);
// bt_tray_state(): 0 = off, 1 = on/idle, 2 = on/connected. Cheap; the tray
// calls it each frame. Always 0 until a real driver exists.
int  bt_tray_state(void);

// =============================================================================
// STUB IMPLEMENTATION (#237) - no driver/stack exists. Every call below
// reports the one true fact this build has: there is no Bluetooth stack, so
// the radio is never on, nothing is ever discovered, and nothing is ever
// paired or connected. None of this reads or writes a state file: unlike the
// mock it replaces, there is no per-process state to keep in sync, because
// nothing here ever changes.
// =============================================================================
#ifdef BT_STUB_IMPL

int bt_addr_eq(const bt_addr_t *a, const bt_addr_t *b) {
    for (int i = 0; i < 6; i++) if (a->b[i] != b->b[i]) return 0;
    return 1;
}
static char bt_hex(int v) { v &= 0xf; return (char)(v < 10 ? '0' + v : 'A' + v - 10); }
void bt_addr_fmt(const bt_addr_t *a, char *out) {
    // Display MSB-first (b[] is stored LAP-first / little-endian).
    int o = 0;
    for (int i = 5; i >= 0; i--) {
        out[o++] = bt_hex(a->b[i] >> 4);
        out[o++] = bt_hex(a->b[i]);
        if (i) out[o++] = ':';
    }
    out[o] = 0;
}

int bt_power(int on)                { (void)on; return -1; }   // no stack to power
int bt_is_powered(void)              { return 0; }               // never on

int bt_scan_start(void)              { return -8; }   // BT_ERR_DISABLED: no stack
int bt_scan_stop(void)               { return 0; }
int bt_scan_active(void)             { return 0; }
int bt_get_devices(bt_device_t *out, int max) { (void)out; (void)max; return 0; }
int bt_get_device(const bt_addr_t *addr, bt_device_t *out) { (void)addr; (void)out; return -2; } // BT_ERR_NODEV

int bt_pair(const bt_addr_t *addr)          { (void)addr; return -6; }
int bt_connect(const bt_addr_t *addr)       { (void)addr; return -6; }
int bt_disconnect_dev(const bt_addr_t *addr){ (void)addr; return -2; }
int bt_forget(const bt_addr_t *addr)        { (void)addr; return -2; }

bt_state_t bt_status(void)           { return BT_STATE_OFF; }

void bt_tick(void)                   { }   // nothing to pump; no state, no timers
int bt_tray_state(void)              { return 0; }   // always "off": honestly, it is

#endif // BT_STUB_IMPL
#endif // BT_CLIENT_H
