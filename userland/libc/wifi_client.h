// wifi_client.h - userland Wi-Fi client + MOCK backend for the UI (#384)
// =============================================================================
// The Settings "Wi-Fi" panel and the Network tray indicator code against THIS
// header. The real Wi-Fi driver (#383) is not built yet, so this ships a self-
// contained MOCK: scanning returns a few fake SSIDs with varied signal/lock,
// connect simulates success. Swapping in the real driver later is a one-line
// change PER FUNCTION (each body becomes a SYS_WIFI_* syscall) - the struct
// layout and signatures are the stable seam. Same single-header pattern as
// bt_client.h: exactly ONE TU per program defines WIFI_MOCK_IMPL.
//
// NOTE: the *wired* network status the tray shows is REAL (get_net_info /
// net_is_up); only the Wi-Fi scan/connect simulated here is mocked.
// =============================================================================
#ifndef WIFI_CLIENT_H
#define WIFI_CLIENT_H

#define WIFI_SSID_MAX     33
#define WIFI_MAX_NETWORKS 16

// Security type (anything != OPEN shows a lock glyph and needs a password).
enum { WIFI_SEC_OPEN = 0, WIFI_SEC_WPA2, WIFI_SEC_WPA3 };

typedef struct {
    char          ssid[WIFI_SSID_MAX];
    int           signal;      // 0..100 %
    int           security;    // WIFI_SEC_*
    unsigned char saved;       // known / auto-join
    unsigned char connected;   // active association
} wifi_network_t;

// ---- Radio power ---------------------------------------------------------
int  wifi_power(int on);
int  wifi_is_powered(void);

// ---- Discovery -----------------------------------------------------------
int  wifi_scan_start(void);
int  wifi_scan_stop(void);
int  wifi_scan_active(void);
int  wifi_get_networks(wifi_network_t *out, int max);   // count written

// ---- Association ---------------------------------------------------------
int  wifi_connect(const char *ssid, const char *password);  // 0 = accepted
int  wifi_disconnect(void);
int  wifi_forget(const char *ssid);
// Returns 1 and fills ssid_out (>= WIFI_SSID_MAX) if associated, else 0.
int  wifi_current(char *ssid_out);
int  wifi_is_secured(const char *ssid);   // 1 if the named SSID needs a key

// ---- Frontend helpers (not part of the future kernel contract) -----------
void wifi_tick(void);         // pump the mock + re-sync shared state (~10x/s)
int  wifi_tray_state(void);   // 0 = off/disconnected, 1 = on/idle, 2 = connected
int  wifi_tray_signal(void);  // 0..100 signal (shared, for the tray in the other process)
int  wifi_signal(void);       // 0..100 signal of the connected network

// =============================================================================
// MOCK IMPLEMENTATION
// =============================================================================
#ifdef WIFI_MOCK_IMPL

#include "syscall.h"

#define WIFI_STATE_FILE "/WIFI.STATE"

static int  g_wifi_powered   = -1;
static int  g_wifi_scanning  = 0;
static int  g_wifi_scanticks = 0;
static int  g_wifi_shared_conn = 0;
static int  g_wifi_shared_sig  = 0;
static unsigned long g_wifi_last_sync = 0;

typedef struct {
    const char   *ssid;
    int           signal, security;
    unsigned char init_saved, init_connected;
    int           discover_at;
    int           op_ticks;       // >0 while a connect is in flight
    unsigned char saved, connected, visible;
} wifi_rt_t;

static wifi_rt_t g_wifi_rt[] = {
    { "Maytera-5G",    92, WIFI_SEC_WPA3, 1, 1, 0,  0, 0,0,0 },
    { "Maytera-Guest", 78, WIFI_SEC_OPEN, 0, 0, 4,  0, 0,0,0 },
    { "HomeNet_2.4",   66, WIFI_SEC_WPA2, 1, 0, 7,  0, 0,0,0 },
    { "CoffeeShop",    47, WIFI_SEC_OPEN, 0, 0, 11, 0, 0,0,0 },
    { "NETGEAR-A7",    34, WIFI_SEC_WPA2, 0, 0, 15, 0, 0,0,0 },
    { "xfinitywifi",   21, WIFI_SEC_OPEN, 0, 0, 19, 0, 0,0,0 },
};
#define WIFI_RT_N ((int)(sizeof(g_wifi_rt)/sizeof(g_wifi_rt[0])))
static int g_wifi_rt_init = 0;

static int wifi_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int wifi_conn_idx(void) {
    for (int i = 0; i < WIFI_RT_N; i++) if (g_wifi_rt[i].connected) return i;
    return -1;
}
static void wifi_rt_seed(void) {
    if (g_wifi_rt_init) return;
    for (int i = 0; i < WIFI_RT_N; i++) {
        g_wifi_rt[i].saved     = g_wifi_rt[i].init_saved;
        g_wifi_rt[i].connected = g_wifi_rt[i].init_connected;
        g_wifi_rt[i].visible   = g_wifi_rt[i].init_connected || g_wifi_rt[i].init_saved;
        g_wifi_rt[i].op_ticks  = 0;
    }
    g_wifi_rt_init = 1;
}
static void wifi_state_save(void) {
    char buf[32]; int n = 0;
    int ci = wifi_conn_idx();
    int sig = (ci >= 0) ? g_wifi_rt[ci].signal : 0;
    buf[n++] = 'p'; buf[n++] = '='; buf[n++] = (char)('0' + (g_wifi_powered ? 1 : 0)); buf[n++] = '\n';
    buf[n++] = 'c'; buf[n++] = '='; buf[n++] = (char)('0' + (ci >= 0 ? 1 : 0)); buf[n++] = '\n';
    buf[n++] = 's'; buf[n++] = '='; buf[n++] = (char)('0' + (sig / 11)); buf[n++] = '\n';  // 0..9
    sys_unlink(WIFI_STATE_FILE);
    int fd = sys_open(WIFI_STATE_FILE, 0x41);
    if (fd < 0) return;
    sys_write(fd, buf, (unsigned long)n);
    sys_close(fd);
}
static int wifi_state_load(void) {
    int fd = sys_open(WIFI_STATE_FILE, 0);
    if (fd < 0) return 0;
    char b[32];
    long r = sys_read(fd, b, sizeof(b) - 1);
    sys_close(fd);
    if (r <= 0) return 0;
    b[r] = 0;
    for (int i = 0; b[i]; i++) {
        if (b[i] == 'p' && b[i+1] == '=') g_wifi_powered = (b[i+2] == '1') ? 1 : 0;
        if (b[i] == 'c' && b[i+1] == '=') g_wifi_shared_conn = (b[i+2] == '1') ? 1 : 0;
        if (b[i] == 's' && b[i+1] == '=') g_wifi_shared_sig  = (b[i+2] - '0') * 11;
    }
    return 1;
}
static void wifi_ensure(void) {
    wifi_rt_seed();
    if (g_wifi_powered < 0) {
        if (!wifi_state_load()) {
            g_wifi_powered = 1;
            wifi_state_save();
        }
    }
}

int wifi_power(int on) {
    wifi_ensure();
    g_wifi_powered = on ? 1 : 0;
    if (!on) {
        g_wifi_scanning = 0;
        for (int i = 0; i < WIFI_RT_N; i++) {
            g_wifi_rt[i].connected = 0;
            g_wifi_rt[i].op_ticks = 0;
            g_wifi_rt[i].visible = g_wifi_rt[i].saved;
        }
    }
    wifi_state_save();
    return 0;
}
int wifi_is_powered(void) { wifi_ensure(); return g_wifi_powered ? 1 : 0; }

int wifi_scan_start(void) {
    wifi_ensure();
    if (!g_wifi_powered) return -1;
    g_wifi_scanning = 1; g_wifi_scanticks = 0;
    for (int i = 0; i < WIFI_RT_N; i++)
        if (!g_wifi_rt[i].saved && !g_wifi_rt[i].connected) g_wifi_rt[i].visible = 0;
    return 0;
}
int wifi_scan_stop(void)   { g_wifi_scanning = 0; return 0; }
int wifi_scan_active(void) { return g_wifi_scanning; }

int wifi_get_networks(wifi_network_t *out, int max) {
    wifi_ensure();
    if (!g_wifi_powered) return 0;
    int n = 0;
    for (int i = 0; i < WIFI_RT_N && n < max; i++) {
        if (!g_wifi_rt[i].visible) continue;
        wifi_network_t *w = &out[n++];
        int k = 0;
        while (g_wifi_rt[i].ssid[k] && k < WIFI_SSID_MAX - 1) { w->ssid[k] = g_wifi_rt[i].ssid[k]; k++; }
        w->ssid[k]    = 0;
        w->signal     = g_wifi_rt[i].signal;
        w->security   = g_wifi_rt[i].security;
        w->saved      = g_wifi_rt[i].saved;
        w->connected  = g_wifi_rt[i].connected;
    }
    return n;
}
static int wifi_find(const char *ssid) {
    for (int i = 0; i < WIFI_RT_N; i++) if (wifi_streq(g_wifi_rt[i].ssid, ssid)) return i;
    return -1;
}
int wifi_is_secured(const char *ssid) {
    int i = wifi_find(ssid);
    return (i >= 0 && g_wifi_rt[i].security != WIFI_SEC_OPEN) ? 1 : 0;
}
int wifi_connect(const char *ssid, const char *password) {
    (void)password;   // mock accepts any key
    wifi_ensure();
    int i = wifi_find(ssid);
    if (i < 0 || !g_wifi_powered) return -1;
    for (int k = 0; k < WIFI_RT_N; k++) g_wifi_rt[k].op_ticks = 0;
    g_wifi_rt[i].op_ticks = 12;   // ~1.2s association
    return 0;
}
int wifi_disconnect(void) {
    wifi_ensure();
    for (int i = 0; i < WIFI_RT_N; i++) { g_wifi_rt[i].connected = 0; g_wifi_rt[i].op_ticks = 0; }
    wifi_state_save();
    return 0;
}
int wifi_forget(const char *ssid) {
    wifi_ensure();
    int i = wifi_find(ssid);
    if (i < 0) return -1;
    g_wifi_rt[i].saved = 0; g_wifi_rt[i].connected = 0; g_wifi_rt[i].op_ticks = 0;
    wifi_state_save();
    return 0;
}
int wifi_current(char *ssid_out) {
    wifi_ensure();
    int i = wifi_conn_idx();
    if (i < 0) { if (ssid_out) ssid_out[0] = 0; return 0; }
    if (ssid_out) { int k = 0; while (g_wifi_rt[i].ssid[k] && k < WIFI_SSID_MAX - 1) { ssid_out[k] = g_wifi_rt[i].ssid[k]; k++; } ssid_out[k] = 0; }
    return 1;
}
int wifi_signal(void) {
    wifi_ensure();
    int i = wifi_conn_idx();
    return (i >= 0) ? g_wifi_rt[i].signal : 0;
}

void wifi_tick(void) {
    wifi_ensure();
    unsigned long now = uptime_ms();
    if (now - g_wifi_last_sync > 400) {
        g_wifi_last_sync = now;
        int prev = g_wifi_powered;
        wifi_state_load();
        if (g_wifi_powered != prev && !g_wifi_powered) {
            g_wifi_scanning = 0;
            for (int i = 0; i < WIFI_RT_N; i++) {
                g_wifi_rt[i].connected = 0; g_wifi_rt[i].op_ticks = 0;
                g_wifi_rt[i].visible = g_wifi_rt[i].saved;
            }
        }
    }
    if (!g_wifi_powered) return;

    if (g_wifi_scanning) {
        g_wifi_scanticks++;
        for (int i = 0; i < WIFI_RT_N; i++)
            if (!g_wifi_rt[i].visible && g_wifi_scanticks >= g_wifi_rt[i].discover_at)
                g_wifi_rt[i].visible = 1;
        if (g_wifi_scanticks >= 26) g_wifi_scanning = 0;
    }

    int changed = 0;
    for (int i = 0; i < WIFI_RT_N; i++) {
        if (g_wifi_rt[i].op_ticks <= 0) continue;
        if (--g_wifi_rt[i].op_ticks > 0) continue;
        for (int k = 0; k < WIFI_RT_N; k++) g_wifi_rt[k].connected = 0;
        g_wifi_rt[i].connected = 1;
        g_wifi_rt[i].saved = 1;
        g_wifi_rt[i].visible = 1;
        changed = 1;
    }
    if (changed) wifi_state_save();
}

int wifi_tray_state(void) {
    wifi_ensure();
    if (!g_wifi_powered) return 0;
    return (g_wifi_shared_conn || wifi_conn_idx() >= 0) ? 2 : 1;
}
int wifi_tray_signal(void) {
    wifi_ensure();
    int local = wifi_signal();
    return local > 0 ? local : g_wifi_shared_sig;
}

#endif // WIFI_MOCK_IMPL
#endif // WIFI_CLIENT_H
