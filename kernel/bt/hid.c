// hid.c - Bluetooth HID profile (#372, PROTOCOL agent).
//
// Classic HIDP over L2CAP, TWO roles:
//   - SERVER (inbound): a BT keyboard/mouse pages us and opens the HID control
//     (PSM 0x0011) and interrupt (PSM 0x0013) channels TO us. l2cap.c accepts on
//     the registered servers and fires our on_connect callbacks.
//   - CLIENT (outbound, [no-ticket] bthidp): for a device WE paged (discovered
//     is_hid, then hci_classic_connect), the device will NOT open the HID
//     channels; the HOST must. Once such an outbound link is authenticated and
//     encrypted, hid_encrypt_event() drives the L2CAP client: connect control
//     (0x0011), then on control OPEN connect interrupt (0x0013), reusing l2cap.c's
//     existing client signalling (l2cap_connect + sig_conn_rsp/sig_config_rsp).
// Either role converges on the same channels, so the report path below is shared.
// Boot-protocol INPUT reports arrive on
// the interrupt channel and are funnelled, byte-for-byte the same as USB HID,
// into the kernel input queue via bt_hid_input_report():
//   - mouse:    mouse_inject_hid(dx,dy,buttons,wheel)         [drivers/mouse.c]
//   - keyboard: HID usage -> PS/2 set-1 scancode -> keyboard_process_scancode()
//               [cpu/isr.c]
//
// bt_hid_input_report() is the SINGLE choke point that touches those sinks.
//
// #763: the usage->set-1 table used to be DUPLICATED here from
// drivers/usb_hid.c, under a note asking for it to be factored out one day.
// It has been: rustkern/hidmap.rs is the one table and both drivers call it.
// The duplicate also carried two real bugs (keypad / emitted as a bare 0x35,
// and no numeric keypad at all), which is what a copy always eventually does.
#include "hid.h"
#include "l2cap.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/bootlog.h"

extern void keyboard_process_scancode(uint8_t scancode);              // cpu/isr.c
extern void mouse_inject_hid(int dx, int dy, uint8_t buttons, int wheel); // drivers/mouse.c

// HIDP SET_PROTOCOL transaction type (not in hid.h's subset).
#define HIDP_HDR_SET_PROTOCOL  0x70

// ---------------------------------------------------------------------------
// HID usage -> PS/2 set-1 scancode: the SHARED translator (rustkern/hidmap.rs)
// ---------------------------------------------------------------------------
extern uint8_t hid_usage_to_set1_rs(uint8_t usage, uint8_t *out_ext);

// #148: shared cooked-key ring push (cpu/isr.h) for HID usages with no honest
// single-(code,extended) PS/2 set-1 encoding. See drivers/usb_hid.c's
// matching comment; a Bluetooth keyboard's PrintScreen key gets the same fix
// as a USB one, through the same one-line special case, not a private copy.
extern void keyboard_push_cooked_key(uint8_t code);   // cpu/isr.c
#define BT_HID_USAGE_PRINTSCREEN 0x46   // HID_KEY_PRINTSCREEN, drivers/usb_hid.h

static void emit_set1(uint8_t code, int extended, int pressed) {
    if (!code) return;
    if (extended) keyboard_process_scancode(0xE0);
    keyboard_process_scancode(pressed ? code : (uint8_t)(code | 0x80));
}

// usage 0xE0..0xE7 are the eight modifier bits; everything else is a usage
// from the report's keycode array. The shared translator handles both.
static void emit_key(uint8_t usage, int pressed) {
    // #148: see drivers/usb_hid.c's hid_kernel_key_cb for the full rationale.
    if (usage == BT_HID_USAGE_PRINTSCREEN) {
        if (pressed) keyboard_push_cooked_key(0x9D /* KEY_PRINTSCREEN, cpu/isr.h */);
        return;
    }
    uint8_t ext = 0;
    uint8_t code = hid_usage_to_set1_rs(usage, &ext);
    emit_set1(code, ext, pressed);
}

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------
static bt_hid_dev_t g_hid[BT_HID_MAX_DEVICES];

static bt_hid_dev_t *dev_by_handle(hci_handle_t h) {
    for (int i = 0; i < BT_HID_MAX_DEVICES; i++)
        if (g_hid[i].active && g_hid[i].handle == h) return &g_hid[i];
    return NULL;
}

static bt_hid_dev_t *dev_alloc(hci_handle_t h) {
    bt_hid_dev_t *d = dev_by_handle(h);
    if (d) return d;
    for (int i = 0; i < BT_HID_MAX_DEVICES; i++)
        if (!g_hid[i].active) {
            memset(&g_hid[i], 0, sizeof(g_hid[i]));
            g_hid[i].active = 1;
            g_hid[i].handle = h;
            g_hid[i].transport = BT_HID_TRANSPORT_CLASSIC;
            g_hid[i].kind = BT_HID_OTHER;
            return &g_hid[i];
        }
    return NULL;
}

// ---------------------------------------------------------------------------
// The input choke point (only place touching the kernel input sinks)
// ---------------------------------------------------------------------------
// btbootlog: persist proof-of-input to /BOOTLOG.TXT WITHOUT flooding. A held
// key or a moving mouse produces a continuous report stream, so log only the
// FIRST report of each kind (with its leading bytes, the decisive "it typed /
// moved" proof) and thereafter a periodic running count.
static void bt_hid_report_bootlog(bt_hid_kind_t kind, const uint8_t *report, uint16_t len) {
    static uint32_t kb = 0, ms = 0, oth = 0;
    uint32_t *cnt = (kind == BT_HID_KEYBOARD) ? &kb :
                    (kind == BT_HID_MOUSE)    ? &ms : &oth;
    const char *what = (kind == BT_HID_KEYBOARD) ? "keyboard" :
                       (kind == BT_HID_MOUSE)    ? "mouse" : "other";
    uint32_t c = ++(*cnt);
    if (c == 1) {
        uint8_t b[8];
        for (uint16_t i = 0; i < 8; i++) b[i] = (i < len) ? report[i] : 0;
        bootlog_write("[BT-HID] FIRST %s input report len=%u bytes=%02x %02x %02x %02x %02x %02x %02x %02x",
                      what, len, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    } else if ((c & 0x7F) == 0) {   // every 128th report: running count, no flood
        bootlog_write("[BT-HID] %s input reports received: %u (still receiving)", what, c);
    }
}

void bt_hid_input_report(bt_hid_kind_t kind, const uint8_t *report, uint16_t len) {
    if (!report || len == 0) return;
    bt_hid_report_bootlog(kind, report, len);

    if (kind == BT_HID_MOUSE) {
        uint8_t buttons = report[0];
        int dx    = (len > 1) ? (int)(int8_t)report[1] : 0;
        int dy    = (len > 2) ? (int)(int8_t)report[2] : 0;
        int wheel = (len > 3) ? (int)(int8_t)report[3] : 0;
        mouse_inject_hid(dx, dy, buttons, wheel);
        return;
    }

    if (kind == BT_HID_KEYBOARD) {
        // Boot keyboard report: [mods][resv][k0..k5]. Diff against the previous
        // report to synthesise make/break scancodes. Single funnel => single
        // previous-report state (phase 1 supports one active BT keyboard).
        static uint8_t prev[8];
        uint8_t cur[8];
        memset(cur, 0, sizeof(cur));
        uint16_t n = len < 8 ? len : 8;
        memcpy(cur, report, n);

        // Modifier transitions (byte 0, bits map to usage 0xE0..0xE7).
        uint8_t changed = (uint8_t)(cur[0] ^ prev[0]);
        for (int i = 0; i < 8; i++)
            if (changed & (1 << i))
                emit_key((uint8_t)(0xE0 + i), (cur[0] & (1 << i)) ? 1 : 0);

        // Key releases: in prev but not in cur.
        for (int i = 2; i < 8; i++) {
            uint8_t k = prev[i];
            if (!k) continue;
            int still = 0;
            for (int j = 2; j < 8; j++) if (cur[j] == k) { still = 1; break; }
            if (!still) emit_key(k, 0);
        }
        // Key presses: in cur but not in prev.
        for (int i = 2; i < 8; i++) {
            uint8_t k = cur[i];
            if (!k || k == 0x01) continue;   // 0x01 = ErrorRollOver
            int had = 0;
            for (int j = 2; j < 8; j++) if (prev[j] == k) { had = 1; break; }
            if (!had) emit_key(k, 1);
        }
        memcpy(prev, cur, sizeof(prev));
        return;
    }
}

// ---------------------------------------------------------------------------
// Classic HIDP: L2CAP server callbacks (device connects to us)
// ---------------------------------------------------------------------------
// Forward decls for the host-initiated (client) path, defined after the callback
// tables they reference.
static void hid_client_connect_intr(bt_hid_dev_t *d);
static void hid_client_busy_clear(void);

static void hid_try_attach(bt_hid_dev_t *d) {
    if (d && d->ctrl && d->intr) {
        bt_hid_attach_classic(d->handle, &d->addr, d->ctrl, d->intr);
    }
}

static void hid_on_connect_ctrl(l2cap_chan_t *c) {
    bt_hid_dev_t *d = dev_alloc(c->handle);
    if (!d) return;
    d->ctrl = c;
    c->user = d;
    hci_conn_t *hc = hci_conn_by_handle(c->handle);
    if (hc) d->addr = hc->peer;
    kprintf("[BT-HID] control channel open (handle 0x%04x)\n", c->handle);
    bootlog_write("[BT-HID] Classic control channel open (handle 0x%04x)", c->handle);
    // Host-initiated (client) path: control is now OPEN, so bring up the
    // interrupt channel ourselves (spec order: control before interrupt). On the
    // inbound/server path the device opens interrupt to us, so d->initiator is 0
    // there and we must not initiate. Guard on !d->intr so an interrupt that
    // already arrived (or a duplicate on_connect) does not double-issue.
    if (d->initiator && !d->intr)
        hid_client_connect_intr(d);
    hid_try_attach(d);
}

static void hid_on_connect_intr(l2cap_chan_t *c) {
    bt_hid_dev_t *d = dev_alloc(c->handle);
    if (!d) return;
    d->intr = c;
    c->user = d;
    hci_conn_t *hc = hci_conn_by_handle(c->handle);
    if (hc) d->addr = hc->peer;
    kprintf("[BT-HID] interrupt channel open (handle 0x%04x)\n", c->handle);
    bootlog_write("[BT-HID] Classic interrupt channel open (handle 0x%04x)", c->handle);
    hid_try_attach(d);
}

static void hid_on_disconnect(l2cap_chan_t *c) {
    bt_hid_dev_t *d = dev_by_handle(c->handle);
    if (!d) return;
    if (d->ctrl == c) d->ctrl = NULL;
    if (d->intr == c) d->intr = NULL;
    if (!d->ctrl && !d->intr) {
        kprintf("[BT-HID] device detached (handle 0x%04x)\n", c->handle);
        bootlog_write("[BT-HID] HID device detached (handle 0x%04x)", c->handle);
        // Release the one-target-at-a-time client guard if this was our
        // host-initiated device, so the next discovered HID can be brought up.
        if (d->initiator) hid_client_busy_clear();
        d->initiator = 0;
        d->active = 0;
    }
}

// Control channel: HIDP HANDSHAKE / control responses (just logged).
static void hid_ctrl_data(l2cap_chan_t *c, const uint8_t *d, uint16_t n) {
    (void)c;
    if (n < 1) return;
    uint8_t type = d[0] & 0xF0;
    if (type == HIDP_HDR_HANDSHAKE)
        kprintf("[BT-HID] handshake result=%u\n", d[0] & 0x0F);
    else if (type == HIDP_HDR_CONTROL && (d[0] & 0x0F) == 0x05)
        kprintf("[BT-HID] virtual cable unplug\n");
}

// Interrupt channel: HIDP DATA/INPUT reports -> input funnel.
static void hid_intr_data(l2cap_chan_t *c, const uint8_t *d, uint16_t n) {
    if (n < 2) return;
    uint8_t hdr = d[0];
    if ((hdr & 0xF0) != HIDP_HDR_DATA) return;
    if ((hdr & 0x0F) != HIDP_RTYPE_INPUT) return;

    const uint8_t *report = d + 1;
    uint16_t rlen = (uint16_t)(n - 1);

    bt_hid_dev_t *dev = (bt_hid_dev_t *)c->user;
    bt_hid_kind_t kind = dev ? dev->kind : BT_HID_OTHER;
    if (kind == BT_HID_OTHER) {
        // Infer from boot-report length: 8 bytes = keyboard, 3-4 = mouse.
        kind = (rlen >= 8) ? BT_HID_KEYBOARD : BT_HID_MOUSE;
        if (dev) dev->kind = kind;
    }
    bt_hid_input_report(kind, report, rlen);
}

static const l2cap_callbacks_t hid_ctrl_cb = { hid_on_connect_ctrl, hid_ctrl_data, hid_on_disconnect };
static const l2cap_callbacks_t hid_intr_cb = { hid_on_connect_intr, hid_intr_data, hid_on_disconnect };

// ---------------------------------------------------------------------------
// Classic HIDP L2CAP CLIENT (host-initiated, [no-ticket] bthidp)
//
// For a device WE paged outbound (discovered is_hid, then hci_classic_connect),
// the device sits with an authenticated + encrypted ACL link but never opens the
// HID L2CAP channels; the HOST must initiate them. This is the client role
// mirroring the server side above: instead of accepting CONN_REQ on 0x11/0x13,
// we ISSUE CONN_REQ via l2cap.c's l2cap_connect(), whose existing signalling
// (sig_conn_rsp handles CONN_RSP 0x03 -> CONFIG_REQ 0x04; sig_config_rsp handles
// CONFIG_RSP 0x05) drives the config handshake and fires the SAME on_connect
// callbacks. So no parallel signalling is added: we only sequence the two
// connects and reuse hid_ctrl_cb / hid_intr_cb unchanged.
//
// Sequence: encryption complete on an outbound is_hid Classic link
//   -> l2cap_connect(control 0x0011, hid_ctrl_cb)
//   -> hid_on_connect_ctrl (control OPEN) -> hid_client_connect_intr()
//   -> l2cap_connect(interrupt 0x0013, hid_intr_cb)
//   -> hid_on_connect_intr (interrupt OPEN) -> hid_try_attach -> boot protocol.
//
// One HID target at a time (g_hid_client_busy), like gatt.c's g_autoconnect_busy.
// Event-driven throughout: no busy/poll wait, driven off L2CAP signalling events.
// ---------------------------------------------------------------------------
static int g_hid_client_busy = 0;

static void hid_client_busy_clear(void) { g_hid_client_busy = 0; }

static void hid_client_connect_intr(bt_hid_dev_t *d) {
    if (!d) return;
    kprintf("[BT-HID] control up (outbound); connecting interrupt PSM 0x%04x (handle 0x%04x)\n",
            L2CAP_PSM_HID_INTERRUPT, d->handle);
    l2cap_connect(d->handle, L2CAP_PSM_HID_INTERRUPT, &hid_intr_cb, d);
}

// Encryption-change observer. Fires for every link (LE included), so filter hard:
// Classic only (LE/HOGP is gatt.c's), outbound only (role 0 = we are master; an
// inbound link is role 1 and the device opens the channels itself), is_hid only.
static void hid_encrypt_event(hci_handle_t h, uint8_t enabled) {
    if (!enabled) return;
    hci_conn_t *c = hci_conn_by_handle(h);
    if (!c) return;
    if (c->type != HCI_LINK_ACL_CLASSIC) return;   // LE/HOGP handled by gatt.c
    if (c->role != 0) return;                       // outbound (master) only
    hci_disc_dev_t dd;
    if (hci_disc_find(&c->peer, &dd) != BT_OK || !dd.is_hid) return;  // is_hid only

    // Guard against double-initiation: this handle already being brought up/up, or
    // another HID target already in flight.
    bt_hid_dev_t *d = dev_by_handle(h);
    if (d && (d->initiator || d->ctrl || d->intr)) return;
    if (g_hid_client_busy) return;

    d = dev_alloc(h);
    if (!d) return;
    d->transport = BT_HID_TRANSPORT_CLASSIC;
    d->addr = c->peer;
    d->initiator = 1;
    g_hid_client_busy = 1;
    kprintf("[BT-HID] outbound Classic HID link encrypted (handle 0x%04x); initiating HIDP L2CAP client, control PSM 0x%04x\n",
            h, L2CAP_PSM_HID_CONTROL);
    bootlog_write("[BT-HID] outbound Classic HID link encrypted (handle 0x%04x); initiating HIDP L2CAP client", h);
    if (!l2cap_connect(h, L2CAP_PSM_HID_CONTROL, &hid_ctrl_cb, d)) {
        kprintf("[BT-HID] control connect could not start (no L2CAP channel)\n");
        d->initiator = 0;
        d->active = 0;
        hid_client_busy_clear();
    }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
int bt_hid_attach_classic(hci_handle_t h, const bt_addr_t *addr,
                          l2cap_chan_t *ctrl, l2cap_chan_t *intr) {
    bt_hid_dev_t *d = dev_alloc(h);
    if (!d) return BT_ERR_NOMEM;
    d->transport = BT_HID_TRANSPORT_CLASSIC;
    d->ctrl = ctrl;
    d->intr = intr;
    if (addr) d->addr = *addr;
    // Force boot protocol so the interrupt channel carries boot reports.
    uint8_t setproto = HIDP_HDR_SET_PROTOCOL | 0x00;   // 0 = boot protocol
    if (ctrl) l2cap_send(ctrl, &setproto, 1);
    kprintf("[BT-HID] attached classic HID (handle 0x%04x), boot protocol set\n", h);
    bootlog_write("[BT-HID] attached Classic HID (handle 0x%04x), boot protocol set", h);
    return BT_OK;
}

int bt_hid_attach_ble(hci_handle_t h, const bt_addr_t *addr, bt_hid_kind_t kind) {
    bt_hid_dev_t *d = dev_alloc(h);
    if (!d) return BT_ERR_NOMEM;
    d->transport = BT_HID_TRANSPORT_BLE;
    d->kind = kind;
    if (addr) d->addr = *addr;
    kprintf("[BT-HID] attached BLE HID (handle 0x%04x kind %d)\n", h, (int)kind);
    bootlog_write("[BT-HID] attached BLE HID (handle 0x%04x kind %d)", h, (int)kind);
    return BT_OK;
}

void bt_hid_detach(hci_handle_t h) {
    bt_hid_dev_t *d = dev_by_handle(h);
    if (d) d->active = 0;
}

int bt_hid_device_count(void) {
    int n = 0;
    for (int i = 0; i < BT_HID_MAX_DEVICES; i++) if (g_hid[i].active) n++;
    return n;
}

void bt_hid_poll(void) { /* input is push-driven via l2cap on_data */ }

int bt_hid_init(void) {
    for (int i = 0; i < BT_HID_MAX_DEVICES; i++) g_hid[i].active = 0;
    g_hid_client_busy = 0;
    l2cap_register_server(L2CAP_PSM_HID_CONTROL,   &hid_ctrl_cb);
    l2cap_register_server(L2CAP_PSM_HID_INTERRUPT, &hid_intr_cb);
    // Host-initiated client: bring up HID channels for a device we paged outbound
    // once its link is encrypted (the server registrations above cover inbound).
    hci_add_encrypt_observer(hid_encrypt_event);
    kprintf("[BT-HID] init: HID control/interrupt servers + outbound-encrypt observer registered\n");
    return BT_OK;
}
