// bt_client.h - userland Bluetooth client + MOCK backend for the UI (#372)
// =============================================================================
// The Settings "Bluetooth" panel and the tray indicator code against THIS
// header. It MIRRORS the architect's kernel contract (bt/bt_ctrl.h + bt/bt.h)
// one-to-one -- same struct layout, same enum names/values, same function
// names/semantics -- so that when the SYS_BT_* syscall group lands, swapping
// the mock for the real stack is a one-line change PER FUNCTION:
//
//     int bt_power(int on)            { return (int)syscall1(SYS_BT_POWER, on); }
//     int bt_get_devices(bt_device_t *o,int m){ return (int)syscall2(SYS_BT_GET_DEVICES,(long)o,m); }
//     ... etc.
//
// Nothing in the UI touches the mock internals; it only calls the functions
// declared here. Until the syscalls exist, BT_MOCK_IMPL provides a self-
// contained fake: an adapter + a few devices (a Magic Keyboard, a BT mouse, a
// pair of studio headphones, ...) with simulated scan/pair/connect transitions.
//
// Single-header library: exactly ONE translation unit per program defines
// BT_MOCK_IMPL before including this file; every other TU includes it plainly.
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
void bt_addr_fmt(const bt_addr_t *a, char *out);   // "AA:BB:CC:DD:EE:FF" (18 bytes)

// ---- Frontend-only helpers (NOT part of the kernel contract) -------------
// bt_tick(): pump the mock state machine + re-sync shared adapter state written
// by the other process. Call ~10x/sec from the app idle tick. Under the REAL
// stack the kernel bt worker pumps bt_poll(), so this becomes a cheap no-op
// (or a status re-read); the UI keeps calling it either way.
void bt_tick(void);
// bt_tray_state(): 0 = off, 1 = on/idle, 2 = on/connected. Cheap; the tray
// calls it each frame. Backed by the shared state file so it follows toggles
// made in the Settings window.
int  bt_tray_state(void);

// =============================================================================
// MOCK IMPLEMENTATION
// =============================================================================
#ifdef BT_MOCK_IMPL

#include "syscall.h"

// Shared cross-process state file: Settings (the controller) writes it on every
// power/connect/forget change; the tray (compositor process) reads it so its
// icon follows Settings and vice-versa. Only power + connected-count are shared
// (all the tray needs); the rich device list is owned per-process.
#define BT_STATE_FILE   "/BT.STATE"

static int  g_bt_powered      = -1;   // -1 = not loaded yet
static int  g_bt_scanning     = 0;
static int  g_bt_scan_ticks   = 0;
static int  g_bt_shared_conn  = 0;
static unsigned long g_bt_last_sync = 0;
static unsigned long g_bt_rng = 0x1234abcdUL;

typedef struct {
    const char     *name;
    unsigned char   addr[6];
    bt_dev_class_t  cls;
    unsigned char   is_le;
    signed char     base_rssi;
    unsigned char   init_paired, init_connected;
    int             discover_at;   // scan tick at which an unpaired device appears
    int             op_ticks;      // remaining ticks of an in-flight pair/connect
    int             op_target;     // BT_LINK_PAIRED or BT_LINK_CONNECTED while busy
    unsigned char   paired, connected, visible;
    signed char     rssi;
} bt_rt_t;

static bt_rt_t g_bt_rt[] = {
    { "Magic Keyboard",   {0x07,0x11,0x23,0x48,0xDE,0xAC}, BT_DEV_KEYBOARD, 0, -47, 1, 1, 0,  0,0, 0,0,0, 0 },
    { "BT Mouse",         {0xA2,0x44,0x1C,0x98,0x18,0xF0}, BT_DEV_MOUSE,    1, -55, 1, 0, 0,  0,0, 0,0,0, 0 },
    { "Studio Headphones",{0x12,0x33,0x9A,0x66,0x1B,0x00}, BT_DEV_AUDIO,    0, -62, 0, 0, 5,  0,0, 0,0,0, 0 },
    { "Pixel Buds Pro",   {0x01,0x88,0x44,0x6D,0x28,0x3C}, BT_DEV_AUDIO,    1, -71, 0, 0, 11, 0,0, 0,0,0, 0 },
    { "Galaxy S24",       {0x2B,0x9F,0x07,0xA3,0x55,0xD8}, BT_DEV_PHONE,    0, -78, 0, 0, 18, 0,0, 0,0,0, 0 },
};
#define BT_RT_N ((int)(sizeof(g_bt_rt)/sizeof(g_bt_rt[0])))
static int g_bt_rt_init = 0;

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
static int bt_rng_next(void) {
    g_bt_rng = g_bt_rng * 1103515245UL + 12345UL;
    return (int)((g_bt_rng >> 16) & 0x7fff);
}
static bt_link_state_t bt_rt_link(const bt_rt_t *r) {
    if (r->connected) return BT_LINK_CONNECTED;
    if (r->paired)    return BT_LINK_PAIRED;
    if (r->visible)   return BT_LINK_FOUND;
    return BT_LINK_NONE;
}

static void bt_rt_seed(void) {
    if (g_bt_rt_init) return;
    for (int i = 0; i < BT_RT_N; i++) {
        g_bt_rt[i].paired    = g_bt_rt[i].init_paired;
        g_bt_rt[i].connected = g_bt_rt[i].init_connected;
        g_bt_rt[i].visible   = g_bt_rt[i].init_paired;
        g_bt_rt[i].rssi      = g_bt_rt[i].base_rssi;
        g_bt_rt[i].op_ticks  = 0;
        g_bt_rt[i].op_target = 0;
    }
    g_bt_rt_init = 1;
}
static int bt_conn_count(void) {
    int n = 0;
    for (int i = 0; i < BT_RT_N; i++) if (g_bt_rt[i].connected) n++;
    return n;
}

static void bt_state_save(void) {
    char buf[32]; int n = 0;
    buf[n++] = 'p'; buf[n++] = '='; buf[n++] = (char)('0' + (g_bt_powered ? 1 : 0)); buf[n++] = '\n';
    int c = bt_conn_count(); if (c > 9) c = 9;
    buf[n++] = 'n'; buf[n++] = '='; buf[n++] = (char)('0' + c); buf[n++] = '\n';
    sys_unlink(BT_STATE_FILE);
    int fd = sys_open(BT_STATE_FILE, 0x41);   // O_WRONLY|O_CREAT
    if (fd < 0) return;
    sys_write(fd, buf, (unsigned long)n);
    sys_close(fd);
}
static int bt_state_load(void) {
    int fd = sys_open(BT_STATE_FILE, 0);
    if (fd < 0) return 0;
    char b[32];
    long r = sys_read(fd, b, sizeof(b) - 1);
    sys_close(fd);
    if (r <= 0) return 0;
    b[r] = 0;
    for (int i = 0; b[i]; i++) {
        if (b[i] == 'p' && b[i+1] == '=') g_bt_powered = (b[i+2] == '1') ? 1 : 0;
        if (b[i] == 'n' && b[i+1] == '=') { int v = b[i+2] - '0'; g_bt_shared_conn = (v < 0) ? 0 : v; }
    }
    return 1;
}
static void bt_ensure(void) {
    bt_rt_seed();
    if (g_bt_powered < 0) {
        if (!bt_state_load()) {
            g_bt_powered = 1;                 // ship powered on so a device shows
            g_bt_shared_conn = bt_conn_count();
            bt_state_save();
        }
    }
}

int bt_power(int on) {
    bt_ensure();
    g_bt_powered = on ? 1 : 0;
    if (!on) {
        g_bt_scanning = 0;
        for (int i = 0; i < BT_RT_N; i++) {
            g_bt_rt[i].connected = 0;
            g_bt_rt[i].op_ticks = 0; g_bt_rt[i].op_target = 0;
            g_bt_rt[i].visible = g_bt_rt[i].paired;
        }
    }
    bt_state_save();
    return 0;
}
int bt_is_powered(void) { bt_ensure(); return g_bt_powered ? 1 : 0; }

int bt_scan_start(void) {
    bt_ensure();
    if (!g_bt_powered) return -8;   // BT_ERR_DISABLED
    g_bt_scanning = 1; g_bt_scan_ticks = 0;
    for (int i = 0; i < BT_RT_N; i++)
        if (!g_bt_rt[i].paired) g_bt_rt[i].visible = 0;
    return 0;
}
int bt_scan_stop(void)   { g_bt_scanning = 0; return 0; }
int bt_scan_active(void) { return g_bt_scanning; }

static int bt_find(const bt_addr_t *addr) {
    for (int i = 0; i < BT_RT_N; i++) {
        int eq = 1;
        for (int k = 0; k < 6; k++) if (g_bt_rt[i].addr[k] != addr->b[k]) { eq = 0; break; }
        if (eq) return i;
    }
    return -1;
}
static void bt_fill(bt_device_t *d, int i) {
    for (int k = 0; k < 6; k++) d->addr.b[k] = g_bt_rt[i].addr[k];
    int k = 0;
    while (g_bt_rt[i].name[k] && k < BT_NAME_MAX - 1) { d->name[k] = g_bt_rt[i].name[k]; k++; }
    d->name[k]    = 0;
    d->cls        = g_bt_rt[i].cls;
    d->link       = bt_rt_link(&g_bt_rt[i]);
    d->rssi       = g_bt_rt[i].rssi;
    d->is_le      = g_bt_rt[i].is_le;
    d->paired     = g_bt_rt[i].paired;
    d->connected  = g_bt_rt[i].connected;
}

int bt_get_devices(bt_device_t *out, int max) {
    bt_ensure();
    if (!g_bt_powered) return 0;
    int n = 0;
    for (int i = 0; i < BT_RT_N && n < max; i++) {
        if (!g_bt_rt[i].visible) continue;
        bt_fill(&out[n++], i);
    }
    return n;
}
int bt_get_device(const bt_addr_t *addr, bt_device_t *out) {
    bt_ensure();
    int i = bt_find(addr);
    if (i < 0) return -2;   // BT_ERR_NODEV
    bt_fill(out, i);
    return 0;
}

int bt_pair(const bt_addr_t *addr) {
    bt_ensure();
    int i = bt_find(addr);
    if (i < 0 || !g_bt_powered) return -6;
    if (g_bt_rt[i].paired) return 0;
    g_bt_rt[i].op_ticks = 16; g_bt_rt[i].op_target = BT_LINK_PAIRED;   // ~1.6s @10Hz
    return 0;
}
int bt_connect(const bt_addr_t *addr) {
    bt_ensure();
    int i = bt_find(addr);
    if (i < 0 || !g_bt_powered) return -6;
    if (g_bt_rt[i].connected) return 0;
    g_bt_rt[i].op_ticks = 11; g_bt_rt[i].op_target = BT_LINK_CONNECTED;
    return 0;
}
int bt_disconnect_dev(const bt_addr_t *addr) {
    bt_ensure();
    int i = bt_find(addr);
    if (i < 0) return -2;
    g_bt_rt[i].connected = 0;
    g_bt_rt[i].op_ticks = 0; g_bt_rt[i].op_target = 0;
    bt_state_save();
    return 0;
}
int bt_forget(const bt_addr_t *addr) {
    bt_ensure();
    int i = bt_find(addr);
    if (i < 0) return -2;
    g_bt_rt[i].paired = 0; g_bt_rt[i].connected = 0;
    g_bt_rt[i].op_ticks = 0; g_bt_rt[i].op_target = 0;
    g_bt_rt[i].visible = 0;   // drops from Paired; rediscovered on next scan
    bt_state_save();
    return 0;
}

bt_state_t bt_status(void) {
    bt_ensure();
    if (!g_bt_powered) return BT_STATE_OFF;
    for (int i = 0; i < BT_RT_N; i++) if (g_bt_rt[i].op_ticks > 0) return BT_STATE_CONNECTING;
    if (g_bt_scanning) return BT_STATE_SCANNING;
    if (bt_conn_count() > 0) return BT_STATE_CONNECTED;
    return BT_STATE_READY;
}

void bt_tick(void) {
    bt_ensure();
    unsigned long now = uptime_ms();
    if (now - g_bt_last_sync > 400) {
        g_bt_last_sync = now;
        int prev = g_bt_powered;
        bt_state_load();
        if (g_bt_powered != prev && !g_bt_powered) {
            g_bt_scanning = 0;
            for (int i = 0; i < BT_RT_N; i++) {
                g_bt_rt[i].connected = 0;
                g_bt_rt[i].op_ticks = 0; g_bt_rt[i].op_target = 0;
                g_bt_rt[i].visible = g_bt_rt[i].paired;
            }
        }
    }
    if (!g_bt_powered) return;

    if (g_bt_scanning) {
        g_bt_scan_ticks++;
        for (int i = 0; i < BT_RT_N; i++)
            if (!g_bt_rt[i].paired && !g_bt_rt[i].visible &&
                g_bt_scan_ticks >= g_bt_rt[i].discover_at)
                g_bt_rt[i].visible = 1;
        if ((g_bt_scan_ticks & 3) == 0)
            for (int i = 0; i < BT_RT_N; i++) {
                int jitter = (bt_rng_next() % 5) - 2;
                g_bt_rt[i].rssi = (signed char)(g_bt_rt[i].base_rssi + jitter);
            }
        if (g_bt_scan_ticks >= 30) g_bt_scanning = 0;   // auto-stop after ~3s
    }

    int changed = 0;
    for (int i = 0; i < BT_RT_N; i++) {
        if (g_bt_rt[i].op_ticks <= 0) continue;
        if (--g_bt_rt[i].op_ticks > 0) continue;
        if (g_bt_rt[i].op_target == BT_LINK_PAIRED) {
            g_bt_rt[i].paired = 1; g_bt_rt[i].connected = 1;   // pairing a HID auto-connects
            g_bt_rt[i].visible = 1;
        } else if (g_bt_rt[i].op_target == BT_LINK_CONNECTED) {
            g_bt_rt[i].connected = 1;
        }
        g_bt_rt[i].op_target = 0;
        changed = 1;
    }
    if (changed) bt_state_save();
}

int bt_tray_state(void) {
    bt_ensure();
    if (!g_bt_powered) return 0;
    return (g_bt_shared_conn > 0 || bt_conn_count() > 0) ? 2 : 1;
}

#endif // BT_MOCK_IMPL
#endif // BT_CLIENT_H
