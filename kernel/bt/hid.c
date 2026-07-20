// hid.c - Bluetooth HID profile (#372, PROTOCOL agent).
//
// Classic HIDP over L2CAP: we are the discoverable/connectable host; a BT
// keyboard/mouse (re)connects and opens the HID control (PSM 0x0011) and
// interrupt (PSM 0x0013) channels to us. Boot-protocol INPUT reports arrive on
// the interrupt channel and are funnelled, byte-for-byte the same as USB HID,
// into the kernel input queue via bt_hid_input_report():
//   - mouse:    mouse_inject_hid(dx,dy,buttons,wheel)         [drivers/mouse.c]
//   - keyboard: HID usage -> PS/2 set-1 scancode -> keyboard_process_scancode()
//               [cpu/isr.c]
//
// bt_hid_input_report() is the SINGLE choke point that touches those sinks.
//
// NOTE: the usage->set-1 table below mirrors drivers/usb_hid.c. INTEGRATION.md
// asks for it to be factored into a shared drivers/hid_keymap.h; that refactor
// touches drivers/ (shared) and must be coordinated with the architect, so for
// now it is duplicated here with this pointer to the canonical copy.
#include "hid.h"
#include "l2cap.h"
#include "../serial.h"
#include "../string.h"

extern void keyboard_process_scancode(uint8_t scancode);              // cpu/isr.c
extern void mouse_inject_hid(int dx, int dy, uint8_t buttons, int wheel); // drivers/mouse.c

// HIDP SET_PROTOCOL transaction type (not in hid.h's subset).
#define HIDP_HDR_SET_PROTOCOL  0x70

// ---------------------------------------------------------------------------
// HID usage -> PS/2 set-1 scancode (mirror of drivers/usb_hid.c)
// ---------------------------------------------------------------------------
static const uint8_t hid_to_set1[0x74] = {
    [0x04]=0x1E,[0x05]=0x30,[0x06]=0x2E,[0x07]=0x20,[0x08]=0x12,[0x09]=0x21,
    [0x0A]=0x22,[0x0B]=0x23,[0x0C]=0x17,[0x0D]=0x24,[0x0E]=0x25,[0x0F]=0x26,
    [0x10]=0x32,[0x11]=0x31,[0x12]=0x18,[0x13]=0x19,[0x14]=0x10,[0x15]=0x13,
    [0x16]=0x1F,[0x17]=0x14,[0x18]=0x16,[0x19]=0x2F,[0x1A]=0x11,[0x1B]=0x2D,
    [0x1C]=0x15,[0x1D]=0x2C,
    [0x1E]=0x02,[0x1F]=0x03,[0x20]=0x04,[0x21]=0x05,[0x22]=0x06,[0x23]=0x07,
    [0x24]=0x08,[0x25]=0x09,[0x26]=0x0A,[0x27]=0x0B,
    [0x28]=0x1C,[0x29]=0x01,[0x2A]=0x0E,[0x2B]=0x0F,[0x2C]=0x39,
    [0x2D]=0x0C,[0x2E]=0x0D,[0x2F]=0x1A,[0x30]=0x1B,[0x31]=0x2B,
    [0x33]=0x27,[0x34]=0x28,[0x35]=0x29,[0x36]=0x33,[0x37]=0x34,[0x38]=0x35,
    [0x39]=0x3A,
    [0x3A]=0x3B,[0x3B]=0x3C,[0x3C]=0x3D,[0x3D]=0x3E,[0x3E]=0x3F,[0x3F]=0x40,
    [0x40]=0x41,[0x41]=0x42,[0x42]=0x43,[0x43]=0x44,[0x44]=0x57,[0x45]=0x58,
    [0x53]=0x45, [0x54]=0x35, [0x55]=0x37, [0x56]=0x4A, [0x57]=0x4E,
};

static uint8_t hid_ext_set1(uint8_t usage) {
    switch (usage) {
        case 0x49: return 0x52; // Insert
        case 0x4A: return 0x47; // Home
        case 0x4B: return 0x49; // Page Up
        case 0x4C: return 0x53; // Delete
        case 0x4D: return 0x4F; // End
        case 0x4E: return 0x51; // Page Down
        case 0x4F: return 0x4D; // Right
        case 0x50: return 0x4B; // Left
        case 0x51: return 0x50; // Down
        case 0x52: return 0x48; // Up
        case 0x58: return 0x1C; // Keypad Enter
        default:   return 0x00;
    }
}

static void emit_set1(uint8_t code, int extended, int pressed) {
    if (!code) return;
    if (extended) keyboard_process_scancode(0xE0);
    keyboard_process_scancode(pressed ? code : (uint8_t)(code | 0x80));
}

static void emit_key(uint8_t usage, int pressed) {
    if (usage >= 0xE0 && usage <= 0xE7) {
        switch (usage) {
            case 0xE0: emit_set1(0x1D, 0, pressed); break; // L Ctrl
            case 0xE1: emit_set1(0x2A, 0, pressed); break; // L Shift
            case 0xE2: emit_set1(0x38, 0, pressed); break; // L Alt
            case 0xE3: emit_set1(0x5B, 1, pressed); break; // L GUI
            case 0xE4: emit_set1(0x1D, 1, pressed); break; // R Ctrl
            case 0xE5: emit_set1(0x36, 0, pressed); break; // R Shift
            case 0xE6: emit_set1(0x38, 1, pressed); break; // R Alt
            case 0xE7: emit_set1(0x5C, 1, pressed); break; // R GUI
        }
        return;
    }
    uint8_t ext = hid_ext_set1(usage);
    if (ext) { emit_set1(ext, 1, pressed); return; }
    if (usage < sizeof(hid_to_set1)) emit_set1(hid_to_set1[usage], 0, pressed);
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
void bt_hid_input_report(bt_hid_kind_t kind, const uint8_t *report, uint16_t len) {
    if (!report || len == 0) return;

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
    hid_try_attach(d);
}

static void hid_on_disconnect(l2cap_chan_t *c) {
    bt_hid_dev_t *d = dev_by_handle(c->handle);
    if (!d) return;
    if (d->ctrl == c) d->ctrl = NULL;
    if (d->intr == c) d->intr = NULL;
    if (!d->ctrl && !d->intr) {
        kprintf("[BT-HID] device detached (handle 0x%04x)\n", c->handle);
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
    return BT_OK;
}

int bt_hid_attach_ble(hci_handle_t h, const bt_addr_t *addr, bt_hid_kind_t kind) {
    bt_hid_dev_t *d = dev_alloc(h);
    if (!d) return BT_ERR_NOMEM;
    d->transport = BT_HID_TRANSPORT_BLE;
    d->kind = kind;
    if (addr) d->addr = *addr;
    kprintf("[BT-HID] attached BLE HID (handle 0x%04x kind %d)\n", h, (int)kind);
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
    l2cap_register_server(L2CAP_PSM_HID_CONTROL,   &hid_ctrl_cb);
    l2cap_register_server(L2CAP_PSM_HID_INTERRUPT, &hid_intr_cb);
    kprintf("[BT-HID] init: HID control/interrupt servers registered\n");
    return BT_OK;
}
