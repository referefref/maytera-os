// bt.c - MayteraOS Bluetooth stack top-level, transport registry, control API.
// ARCHITECT-owned (#372). Keeps the tree green while the transport/protocol
// agents fill in the layers. Everything here is gated behind g_bt_enable so
// boot is never affected while the stack is incomplete.
#include "bt.h"
#include "bt_transport.h"
#include "bt_ctrl.h"
#include "hci.h"
#include "l2cap.h"
#include "sdp.h"
#include "hid.h"
#include "pair.h"
#include "gatt.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"
#include "../mm/heap.h"

extern fat_fs_t g_fat_fs;

// Safe kernel default when no settings marker is present. The boot-time FAT
// marker read is unreliable under the #375 TO-RAM root (fat_read_file returns
// NULL at boot), so the enable decision is made post-boot on the bt worker
// (bt_autostart), where the filesystem is mounted. Default ON so the shipped
// RTL8761BU demo brings Bluetooth up; a /BTOFF.TXT marker disables it, and
// bt_power() lets the Settings app toggle it at runtime.
#define BT_DEFAULT_ENABLE  1

// -----------------------------------------------------------------------------
// Master enable flag. DEFAULT 0 (OFF). main.c only calls bt_init() when this is
// non-zero, so an incomplete stack can never break boot.
// -----------------------------------------------------------------------------
volatile int g_bt_enable = 0;

static bt_state_t g_bt_state = BT_STATE_OFF;
static int        g_bt_scanning = 0;

// -----------------------------------------------------------------------------
// Transport registry (glue between hci_usb.c and hci.c).
// -----------------------------------------------------------------------------
static const bt_transport_ops_t *g_transport = NULL;
static bt_transport_rx_t         g_transport_rx = { NULL, NULL };

int bt_transport_register(const bt_transport_ops_t *ops) {
    if (!ops) return BT_ERR_PARAM;
    g_transport = ops;
    kprintf("[BT] transport registered: %s\n", ops->name ? ops->name : "?");
    return BT_OK;
}

void bt_transport_unregister(void) {
    g_transport = NULL;
}

const bt_transport_ops_t *bt_transport_get(void) {
    return g_transport;
}

int bt_transport_present(void) {
    return g_transport != NULL;
}

void bt_transport_set_rx(const bt_transport_rx_t *rx) {
    if (rx) g_transport_rx = *rx;
    else { g_transport_rx.on_event = NULL; g_transport_rx.on_acl = NULL; }
}

void bt_transport_deliver_event(const uint8_t *data, uint16_t len) {
    if (g_transport_rx.on_event) g_transport_rx.on_event(data, len);
}

void bt_transport_deliver_acl(const uint8_t *data, uint16_t len) {
    if (g_transport_rx.on_acl) g_transport_rx.on_acl(data, len);
}

// -----------------------------------------------------------------------------
// State helpers.
// -----------------------------------------------------------------------------
bt_state_t bt_get_state(void) { return g_bt_state; }
void       bt_set_state(bt_state_t s) { g_bt_state = s; }

const char *bt_state_str(bt_state_t s) {
    switch (s) {
        case BT_STATE_OFF:        return "off";
        case BT_STATE_INIT:       return "init";
        case BT_STATE_READY:      return "ready";
        case BT_STATE_SCANNING:   return "scanning";
        case BT_STATE_CONNECTING: return "connecting";
        case BT_STATE_CONNECTED:  return "connected";
        case BT_STATE_ERROR:      return "error";
        default:                  return "?";
    }
}

int bt_addr_eq(const bt_addr_t *a, const bt_addr_t *b) {
    if (!a || !b) return 0;
    for (int i = 0; i < 6; i++) if (a->b[i] != b->b[i]) return 0;
    return 1;
}

int bt_addr_is_zero(const bt_addr_t *a) {
    if (!a) return 1;
    for (int i = 0; i < 6; i++) if (a->b[i]) return 0;
    return 1;
}

// -----------------------------------------------------------------------------
// Lifecycle. bt_init() brings up every layer in order. It is GATED: returns
// BT_ERR_DISABLED without touching anything if g_bt_enable == 0.
// -----------------------------------------------------------------------------
int bt_init(void) {
    if (!g_bt_enable) {
        // Silent no-op. Boot path calls this unconditionally; the gate lives
        // here so callers never have to special-case the disabled build.
        return BT_ERR_DISABLED;
    }

    kprintf("[BT] init: bringing up Bluetooth stack\n");
    bt_set_state(BT_STATE_INIT);

    if (!bt_transport_present()) {
        kprintf("[BT] no HCI transport registered (no dongle?)\n");
        bt_set_state(BT_STATE_OFF);
        return BT_ERR_NODEV;
    }

    // Bring up the layers bottom-up. Each is a stub for now (returns
    // BT_ERR_NOTIMPL); we log but do not abort so partial progress is visible.
    (void)hci_init();
    (void)l2cap_init();
    (void)sdp_init();
    (void)bt_hid_init();
    (void)pair_init();
    (void)gatt_init();

    bt_set_state(BT_STATE_READY);
    kprintf("[BT] init complete (skeleton: layers are stubs)\n");
    return BT_OK;
}

void bt_shutdown(void) {
    if (g_bt_state == BT_STATE_OFF) return;
    kprintf("[BT] shutdown\n");
    g_bt_scanning = 0;
    bt_set_state(BT_STATE_OFF);
}

void bt_poll(void) {
    if (!g_bt_enable || g_bt_state == BT_STATE_OFF) return;
    if (g_transport && g_transport->poll) g_transport->poll();
    hci_poll();
    l2cap_poll();
    sdp_poll();
    bt_hid_poll();
    pair_poll();
    gatt_poll();
}

// -----------------------------------------------------------------------------
// Diagnostics driven from the bt worker so the live chain is observable without
// a real keyboard: log the discovered-device list, and (once) attempt an LE
// connect to exercise the connect -> ATT -> GATT discovery path when no HID
// device is in range. HID devices are auto-targeted by gatt.c on discovery.
// -----------------------------------------------------------------------------
void bt_debug_scan_summary(void) {
    hci_disc_dev_t d[24];
    int n = hci_disc_snapshot(d, 24);
    int hid = 0;
    for (int i = 0; i < n; i++) if (d[i].is_hid) hid++;
    kprintf("[BT] scan: %d device(s) discovered (%d HID)\n", n, hid);
    for (int i = 0; i < n && i < 12; i++)
        kprintf("[BT]   %02x:%02x:%02x:%02x:%02x:%02x %s rssi=%d hid=%d adv=0x%02x '%s'\n",
                d[i].addr.b[5], d[i].addr.b[4], d[i].addr.b[3],
                d[i].addr.b[2], d[i].addr.b[1], d[i].addr.b[0],
                d[i].addr_type ? "rand" : "pub", d[i].rssi, d[i].is_hid,
                d[i].adv_type, d[i].name);
}

// One-shot diagnostic connect: if no HID device was found, connect to the
// strongest-RSSI connectable advertiser to prove LE_Create_Connection ->
// LE Connection Complete -> ATT MTU exchange -> GATT service discovery live.
// Read-only (no pairing/writes) and disconnected shortly after by bt_forget of
// the attempt; a genuine keyboard would instead be auto-targeted + paired.
int bt_debug_try_connect(void) {
    if (!bt_is_powered()) return BT_ERR_STATE;
    if (hci_conn_count() > 0) return BT_OK;   // already connected/connecting
    hci_disc_dev_t d[24];
    int n = hci_disc_snapshot(d, 24);
    int best = -1; int8_t bestrssi = -128;
    for (int i = 0; i < n; i++) {
        if (d[i].is_hid) return BT_OK;                 // gatt.c handles HID targets
        if (d[i].adv_type != 0x00) continue;           // connectable undirected only
        if (d[i].rssi >= bestrssi) { bestrssi = d[i].rssi; best = i; }
    }
    if (best < 0) { kprintf("[BT] diag-connect: no connectable advertiser found\n"); return BT_ERR_NODEV; }
    kprintf("[BT] diag-connect: exercising connect->ATT path to %02x:%02x:%02x:%02x:%02x:%02x rssi=%d\n",
            d[best].addr.b[5], d[best].addr.b[4], d[best].addr.b[3],
            d[best].addr.b[2], d[best].addr.b[1], d[best].addr.b[0], d[best].rssi);
    return hci_le_connect(&d[best].addr, d[best].addr_type);
}

// -----------------------------------------------------------------------------
// Post-boot autostart, called once from the bt worker (hci_usb.c bt_worker) on
// its first iteration. The filesystem is mounted here (unlike the boot path
// under the #375 to-RAM root), so the settings marker read is reliable. This is
// the FIX for the previous "g_bt_enable forced to 1 in main.c" hack.
// -----------------------------------------------------------------------------
int bt_autostart(void) {
    static int done = 0;
    if (done) return g_bt_enable;
    done = 1;

    int enable = BT_DEFAULT_ENABLE;
    uint32_t sz = 0;
    // Explicit disable marker wins.
    void *off = fat_read_file(&g_fat_fs, "/BTOFF.TXT", &sz);
    if (off) { kfree(off); enable = 0; }
    // Explicit enable marker forces on.
    sz = 0;
    void *on = fat_read_file(&g_fat_fs, "/BTENABLE.TXT", &sz);
    if (on) { kfree(on); enable = 1; }

    kprintf("[BT] autostart: Bluetooth %s (default=%d, markers checked post-boot)\n",
            enable ? "ENABLED" : "disabled", BT_DEFAULT_ENABLE);
    if (enable) return bt_power(1);   // sets g_bt_enable + bt_init()
    g_bt_enable = 0;
    return BT_OK;
}

// -----------------------------------------------------------------------------
// Control API (bt_ctrl.h). Thin stubs today; wired to the layers as they land.
// -----------------------------------------------------------------------------
static bt_pair_confirm_cb_t g_pair_confirm_cb = NULL;

int bt_power(int on) {
    if (on) {
        g_bt_enable = 1;
        return bt_init();
    }
    bt_shutdown();
    g_bt_enable = 0;
    return BT_OK;
}

int bt_is_powered(void) {
    return g_bt_enable && g_bt_state != BT_STATE_OFF;
}

int bt_scan_start(void) {
    if (!bt_is_powered()) return BT_ERR_STATE;
    g_bt_scanning = 1;
    hci_disc_clear();
    return hci_le_scan(1);   // LE active scan (HOGP keyboards/mice)
}

int bt_scan_stop(void) {
    g_bt_scanning = 0;
    hci_le_scan(0);
    if (g_bt_state == BT_STATE_SCANNING) bt_set_state(BT_STATE_READY);
    return BT_OK;
}

int bt_scan_active(void) { return g_bt_scanning; }

static bt_dev_class_t disc_class(const hci_disc_dev_t *d) {
    if (d->appearance == 1) return BT_DEV_KEYBOARD;
    if (d->appearance == 2) return BT_DEV_MOUSE;
    if (d->is_hid)          return BT_DEV_KEYBOARD;   // HID, exact kind unknown
    return BT_DEV_UNKNOWN;
}

int bt_get_devices(bt_device_t *out, int max) {
    if (!out || max <= 0) return 0;
    hci_disc_dev_t tmp[24];
    int n = hci_disc_snapshot(tmp, 24);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].addr  = tmp[i].addr;
        out[i].is_le = tmp[i].is_le;
        out[i].rssi  = tmp[i].rssi;
        for (int k = 0; k < BT_NAME_MAX - 1 && tmp[i].name[k]; k++) out[i].name[k] = tmp[i].name[k];
        out[i].cls   = disc_class(&tmp[i]);
        hci_conn_t *c = hci_conn_by_addr(&tmp[i].addr);
        out[i].connected = (c && c->active) ? 1 : 0;
        out[i].paired    = pair_is_bonded(&tmp[i].addr) ? 1 : 0;
        out[i].link = out[i].connected ? BT_LINK_CONNECTED :
                      out[i].paired    ? BT_LINK_PAIRED : BT_LINK_FOUND;
    }
    return n;
}

int bt_connect(const bt_addr_t *addr) {
    if (!bt_is_powered() || !addr) return BT_ERR_STATE;
    hci_disc_dev_t d;
    if (hci_disc_find(addr, &d) != BT_OK) return BT_ERR_NODEV;
    if (d.is_le) return hci_le_connect(&d.addr, d.addr_type);
    return BT_ERR_NOTIMPL;   // classic connect: phase B
}

int bt_pair(const bt_addr_t *addr) {
    if (!addr) return BT_ERR_PARAM;
    hci_conn_t *c = hci_conn_by_addr(addr);
    if (!c) return bt_connect(addr);   // must be connected first
    if (c->type == HCI_LINK_LE) return pair_start_le(c->handle, addr);
    return pair_start_classic(c->handle, addr);
}

int bt_disconnect_dev(const bt_addr_t *addr) {
    if (!addr) return BT_ERR_PARAM;
    hci_conn_t *c = hci_conn_by_addr(addr);
    if (!c) return BT_ERR_NODEV;
    return hci_disconnect(c->handle, 0x13 /* remote user terminated */);
}

int bt_forget(const bt_addr_t *addr) { return pair_forget(addr); }

bt_state_t bt_status(void) { return g_bt_state; }

int bt_get_device(const bt_addr_t *addr, bt_device_t *out) {
    if (!addr || !out) return BT_ERR_PARAM;
    hci_disc_dev_t d;
    if (hci_disc_find(addr, &d) != BT_OK) return BT_ERR_NODEV;
    memset(out, 0, sizeof(*out));
    out->addr = d.addr; out->is_le = d.is_le; out->rssi = d.rssi;
    for (int k = 0; k < BT_NAME_MAX - 1 && d.name[k]; k++) out->name[k] = d.name[k];
    out->cls = disc_class(&d);
    return BT_OK;
}

void bt_set_pair_confirm_cb(bt_pair_confirm_cb_t cb) {
    g_pair_confirm_cb = cb;
}

// Exposed for pair.c so it can consult the UI confirm policy. Not in the public
// header (internal glue): declared extern where needed.
int bt_pair_confirm_policy(const bt_addr_t *addr, uint32_t passkey) {
    if (g_pair_confirm_cb) return g_pair_confirm_cb(addr, passkey);
    return 1;   // Just-Works auto-accept until a UI is wired
}
