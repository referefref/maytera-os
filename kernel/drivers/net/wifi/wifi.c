// drivers/net/wifi/wifi.c - chip-agnostic WiFi core registry (#383, phase 1).
#include "wifi.h"
#include "../../../serial.h"
#include "../../../string.h"

#define WIFI_MAX_DEV 2
static wifi_dev_t g_wifi[WIFI_MAX_DEV];
static int g_wifi_count = 0;

const char *wifi_state_name(wifi_state_t st) {
    switch (st) {
    case WIFI_STATE_ABSENT:         return "absent";
    case WIFI_STATE_PROBED:         return "probed";
    case WIFI_STATE_FW_LOADED:      return "fw-loaded";
    case WIFI_STATE_INIT:           return "init";
    case WIFI_STATE_SCANNING:       return "scanning";
    case WIFI_STATE_AUTHENTICATING: return "auth";
    case WIFI_STATE_4WAY:           return "4way";
    case WIFI_STATE_CONNECTED:      return "connected";
    default:                        return "?";
    }
}

wifi_dev_t *wifi_core_register(uint16_t vid, uint16_t pid, const char *chip) {
    if (g_wifi_count >= WIFI_MAX_DEV) return 0;
    wifi_dev_t *w = &g_wifi[g_wifi_count++];
    memset(w, 0, sizeof(*w));
    w->present = 1;
    w->state = WIFI_STATE_PROBED;
    w->vid = vid;
    w->pid = pid;
    if (chip) {
        int i = 0;
        for (; chip[i] && i < (int)sizeof(w->chip) - 1; i++) w->chip[i] = chip[i];
        w->chip[i] = 0;
    }
    kprintf("[WIFI] registered %s (%04x:%04x) state=%s\n",
            w->chip, vid, pid, wifi_state_name(w->state));
    return w;
}

wifi_dev_t *wifi_core_get(int index) {
    if (index < 0 || index >= g_wifi_count) return 0;
    return &g_wifi[index];
}

int wifi_core_count(void) { return g_wifi_count; }

void wifi_core_set_state(wifi_dev_t *w, wifi_state_t st) {
    if (!w) return;
    w->state = st;
    kprintf("[WIFI] %s -> state=%s\n", w->chip, wifi_state_name(st));
}
