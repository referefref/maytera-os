// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// bt_client.c - REAL userland Bluetooth client (btui).
// =============================================================================
// Implements the bt_client.h contract against the SYS_BT multiplexed syscall
// (kernel/proc/syscall.c -> kernel/bt/bt.c bt_ctrl API). A translation unit that
// links THIS object gets the real stack (power/scan/state/device-list/pair/
// connect/forget) instead of the honest stub in the header's BT_STUB_IMPL block.
// The Settings app links this; the compositor keeps its own stub (bt_impl.c).
//
// This file must NOT define BT_STUB_IMPL: it takes only the declarations from
// bt_client.h and provides the real implementations below.
// =============================================================================
#include "bt_client.h"
#include "syscall.h"

// --- Radio power -----------------------------------------------------------
int bt_power(int on)      { return (int)syscall2(SYS_BT, BT_CMD_POWER, (long)on); }
int bt_is_powered(void)   { return (int)syscall1(SYS_BT, BT_CMD_IS_POWERED); }

// --- Discovery -------------------------------------------------------------
int bt_scan_start(void)   { return (int)syscall1(SYS_BT, BT_CMD_SCAN_START); }
int bt_scan_stop(void)    { return (int)syscall1(SYS_BT, BT_CMD_SCAN_STOP); }
int bt_scan_active(void)  { return (int)syscall1(SYS_BT, BT_CMD_SCAN_ACTIVE); }

int bt_get_devices(bt_device_t *out, int max) {
    if (!out || max <= 0) return 0;
    if (max > BT_MAX_DEVICES) max = BT_MAX_DEVICES;
    return (int)syscall3(SYS_BT, BT_CMD_GET_DEVICES, (long)out, (long)max);
}

// --- Pairing / connection lifecycle ---------------------------------------
int bt_pair(const bt_addr_t *addr)           { if (!addr) return -6; return (int)syscall2(SYS_BT, BT_CMD_PAIR,       (long)addr); }
int bt_connect(const bt_addr_t *addr)        { if (!addr) return -6; return (int)syscall2(SYS_BT, BT_CMD_CONNECT,    (long)addr); }
int bt_disconnect_dev(const bt_addr_t *addr) { if (!addr) return -6; return (int)syscall2(SYS_BT, BT_CMD_DISCONNECT, (long)addr); }
int bt_forget(const bt_addr_t *addr)         { if (!addr) return -6; return (int)syscall2(SYS_BT, BT_CMD_FORGET,     (long)addr); }

// --- Status ----------------------------------------------------------------
bt_state_t bt_status(void) { return (bt_state_t)syscall1(SYS_BT, BT_CMD_STATUS); }

int bt_get_state_info(bt_state_info_t *out) {
    if (!out) return -6;
    return (int)syscall2(SYS_BT, BT_CMD_GET_STATE, (long)out);
}

int bt_get_device(const bt_addr_t *addr, bt_device_t *out) {
    if (!addr || !out) return -6;
    bt_device_t tmp[BT_MAX_DEVICES];
    int n = bt_get_devices(tmp, BT_MAX_DEVICES);
    for (int i = 0; i < n; i++) {
        if (bt_addr_eq(&tmp[i].addr, addr)) { *out = tmp[i]; return 0; }
    }
    return -2;   // BT_ERR_NODEV
}

// --- Address helpers (pure; no syscall) -----------------------------------
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

// --- Frontend-only helpers -------------------------------------------------
// The kernel bt worker pumps bt_poll() on its own thread, so the client has
// nothing to pump; the UI still calls this each tick.
void bt_tick(void) { }

int bt_tray_state(void) {
    bt_state_info_t st;
    if (bt_get_state_info(&st) != 0) return 0;
    if (!st.enabled) return 0;
    return (st.state == BT_STATE_CONNECTED) ? 2 : 1;
}
