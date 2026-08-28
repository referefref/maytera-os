// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// wifi_client.h - userland Wi-Fi client API + HONEST STUB backend (#237)
// =============================================================================
// The Settings "Wi-Fi" panel and the taskbar tray indicator code against THIS
// header. There is no real Wi-Fi driver yet (#383 not built), so every
// function below is a STUB: it returns a fixed, honest "not available"
// result and touches no fabricated state. It used to be a MOCK
// (WIFI_MOCK_IMPL, #384) that invented SSIDs, signal strengths and a
// successful join - #237 deleted that fiction after the owner's iMac14,4
// (which DOES have a real wireless adapter, so the mock's hardware gate
// opened) showed a fabricated successful network join with nothing to mark
// it as fake. See docs/SETTINGS_CONTROL_AUDIT.md item 4 and blame.md #230/
// #237 for the history.
//
// The struct layout and function signatures are the stable seam for a real
// driver (#383): swapping this stub for the real thing is a one-line change
// PER FUNCTION (each body becomes a SYS_WIFI_* syscall). Nothing in the UI
// touches this file's internals directly; it only calls the declared
// functions. Same single-header pattern as bt_client.h: exactly ONE
// translation unit per program defines WIFI_STUB_IMPL.
//
// NOTE: the *wired* network status the tray shows is REAL (get_net_info /
// net_is_up); only Wi-Fi (still driverless) is stubbed here.
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
void wifi_tick(void);         // pump the (currently no-op) driver state
int  wifi_tray_state(void);   // 0 = off/disconnected, 1 = on/idle, 2 = connected
int  wifi_tray_signal(void);  // 0..100 signal (shared, for the tray in the other process)
int  wifi_signal(void);       // 0..100 signal of the connected network

// =============================================================================
// STUB IMPLEMENTATION (#237) - no driver exists. Every call below reports the
// one true fact this build has: there is no Wi-Fi driver, so the radio is
// never on, nothing is ever scanned, and nothing is ever associated. None of
// this reads or writes a state file: unlike the mock it replaces, there is no
// per-process state to keep in sync, because nothing here ever changes.
// =============================================================================
#ifdef WIFI_STUB_IMPL

int wifi_power(int on)              { (void)on; return -1; }   // no driver to power
int wifi_is_powered(void)           { return 0; }               // never on

int wifi_scan_start(void)           { return -1; }              // cannot scan
int wifi_scan_stop(void)            { return 0; }
int wifi_scan_active(void)          { return 0; }                // never scanning
int wifi_get_networks(wifi_network_t *out, int max) { (void)out; (void)max; return 0; }

int wifi_connect(const char *ssid, const char *password) {
    (void)ssid; (void)password;
    return -1;   // cannot associate: no driver
}
int wifi_disconnect(void)           { return -1; }
int wifi_forget(const char *ssid)   { (void)ssid; return -1; }
int wifi_current(char *ssid_out)    { if (ssid_out) ssid_out[0] = 0; return 0; }
int wifi_is_secured(const char *ssid) { (void)ssid; return 0; }
int wifi_signal(void)               { return 0; }

void wifi_tick(void)                { }   // nothing to pump; no state, no timers
int wifi_tray_state(void)           { return 0; }   // always "off": honestly, it is
int wifi_tray_signal(void)          { return 0; }

#endif // WIFI_STUB_IMPL
#endif // WIFI_CLIENT_H
