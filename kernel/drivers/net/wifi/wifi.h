// drivers/net/wifi/wifi.h - MayteraOS 802.11 WiFi core scaffold (#383).
//
// This is the small, chip-agnostic layer the RTL8812BU (RTL8822B-family)
// USB driver plugs into. Phase 1 only needs a device registry + state enum so
// the desktop/Settings tray (see #384) and later MLME/supplicant phases have a
// stable handle. The register-level chip work lives in rtl8812bu.c.
//
// Phased plan (see PLAN.md): (1) enum+fw  (2) chip/RF init  (3) scan
// (4) auth+assoc  (5) WPA2/3 supplicant  (6) data path  (7) DHCP + UI.
#ifndef MAYTERA_WIFI_H
#define MAYTERA_WIFI_H

#include "../../../types.h"

// High-level lifecycle state of a WiFi netdevice.
typedef enum {
    WIFI_STATE_ABSENT = 0,     // no adapter bound
    WIFI_STATE_PROBED,         // USB device matched + bound
    WIFI_STATE_FW_LOADED,      // firmware uploaded, MCU running (phase 1 goal)
    WIFI_STATE_INIT,           // MAC/RF/PHY brought up (phase 2)
    WIFI_STATE_SCANNING,       // active scan in progress (phase 3)
    WIFI_STATE_AUTHENTICATING, // MLME auth/assoc (phase 4)
    WIFI_STATE_4WAY,           // WPA2/3 EAPOL handshake (phase 5)
    WIFI_STATE_CONNECTED,      // data path up (phase 6)
} wifi_state_t;

// One WiFi device. Kept deliberately small for phase 1; RF/PHY/MLME context
// is added by later phases.
typedef struct wifi_dev {
    int          present;
    wifi_state_t state;
    uint16_t     vid, pid;
    char         chip[16];     // e.g. "RTL8822B"
    uint8_t      cut;          // chip cut/revision (A/B/C ...)
    uint8_t      rf_type;      // 0 = 1T1R, 1 = 2T2R
    uint8_t      mac[6];       // station MAC (efuse), filled in phase 2
    void        *drv;          // driver private (rtl8812bu_dev_t)
} wifi_dev_t;

// Registry (phase 1: a single adapter).
wifi_dev_t *wifi_core_register(uint16_t vid, uint16_t pid, const char *chip);
wifi_dev_t *wifi_core_get(int index);
int         wifi_core_count(void);
void        wifi_core_set_state(wifi_dev_t *w, wifi_state_t st);
const char *wifi_state_name(wifi_state_t st);

#endif // MAYTERA_WIFI_H
