// hci.c - HCI command/event/ACL engine (#372, PROTOCOL agent).
//
// Sits on bt_transport (hci_usb.c) and drives the controller:
//   - controller bring-up sequence (reset -> read BD_ADDR / buffer size ->
//     event mask -> SSP -> class/name -> scan enable -> ready), advanced from
//     command-complete events (async, credit-based flow control)
//   - HCI command queue with num_cmd_packets credits
//   - event decode: command complete/status, connection request (auto-accept),
//     connection/disconnection complete, remote name, inquiry results, LE meta
//     (advertising report / connection complete / LTK request)
//   - connection table (ACL handle <-> addr, classic vs LE) + up/down observers
//   - ACL reassembly of L2CAP PDUs, delivered to the single ACL sink (l2cap.c)
//   - raw pairing/security events fanned out to event observers (pair.c)
//
// All packets are parsed by byte offset (no casts through packed structs) so
// the freestanding -Werror build stays clean. Gated by g_bt_enable via bt.c.
#include "hci.h"
#include "hci_defs.h"
#include "bt_transport.h"
#include "../serial.h"
#include "../string.h"

// ---------------------------------------------------------------------------
// Little-endian helpers
// ---------------------------------------------------------------------------
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static hci_conn_t         g_conns[HCI_MAX_CONNECTIONS];
static hci_acl_sink_t     g_acl_sink = NULL;
static hci_evt_observer_t g_evt_observers[4];
static int                g_evt_observer_count = 0;
static hci_conn_cb_t      g_conn_observers[4];
static int                g_conn_observer_count = 0;
static hci_encrypt_cb_t   g_encrypt_observers[4];
static int                g_encrypt_observer_count = 0;
static bt_addr_t          g_local_addr;
static int                g_ready = 0;

static int      g_cmd_credits = 1;         // controller num_cmd_packets credit
static uint16_t g_acl_mtu     = 1021;      // controller ACL data packet length

// Bring-up sequence marker: opcode we are currently waiting to complete.
static uint16_t g_bringup_wait = 0;

// Simple HCI command queue (built packets, sent as credits allow).
#define HCI_CMDQ_DEPTH 16
typedef struct { uint8_t buf[3 + HCI_MAX_CMD_PARAM_LEN]; uint16_t len; } hci_cmd_ent_t;
static hci_cmd_ent_t g_cmdq[HCI_CMDQ_DEPTH];
static int g_cmdq_head = 0, g_cmdq_tail = 0;   // ring; head==tail => empty

// ACL reassembly, one buffer per connection slot.
typedef struct {
    int      in_use;
    uint16_t handle;
    uint16_t want;     // total L2CAP PDU length (4 + l2cap payload)
    uint16_t have;     // bytes accumulated
    uint8_t  buf[HCI_MAX_ACL_LEN + 8];
} acl_reasm_t;
static acl_reasm_t g_reasm[HCI_MAX_CONNECTIONS];

// ---------------------------------------------------------------------------
// Discovered-device cache (LE adv reports + classic inquiry). Snapshotted by
// the control layer; addr_type is needed to LE_Create_Connection.
// ---------------------------------------------------------------------------
#define HCI_DISC_MAX 24
static hci_disc_dev_t g_disc[HCI_DISC_MAX];
static int            g_disc_count = 0;

static hci_disc_dev_t *disc_find(const bt_addr_t *a, int is_le) {
    for (int i = 0; i < g_disc_count; i++)
        if (g_disc[i].is_le == is_le && bt_addr_eq(&g_disc[i].addr, a))
            return &g_disc[i];
    return NULL;
}

static hci_disc_dev_t *disc_get_or_add(const bt_addr_t *a, int is_le) {
    hci_disc_dev_t *d = disc_find(a, is_le);
    if (d) return d;
    if (g_disc_count >= HCI_DISC_MAX) return NULL;
    d = &g_disc[g_disc_count++];
    memset(d, 0, sizeof(*d));
    d->addr = *a;
    d->is_le = (uint8_t)is_le;
    return d;
}

void hci_disc_clear(void) { g_disc_count = 0; }

int hci_disc_snapshot(hci_disc_dev_t *out, int max) {
    int n = 0;
    for (int i = 0; i < g_disc_count && n < max; i++) out[n++] = g_disc[i];
    return n;
}

int hci_disc_find(const bt_addr_t *addr, hci_disc_dev_t *out) {
    for (int i = 0; i < g_disc_count; i++)
        if (bt_addr_eq(&g_disc[i].addr, addr)) { if (out) *out = g_disc[i]; return BT_OK; }
    return BT_ERR_NODEV;
}

// Auto-connect policy hook (implemented in gatt.c): called when a device that
// advertises the HID service / HID appearance is discovered, so the stack can
// target a real keyboard/mouse without a UI. Weak-linked via a registration so
// hci.c does not hard-depend on gatt.c.
static void (*g_hid_found_cb)(const hci_disc_dev_t *d) = NULL;
void hci_set_hid_found_cb(void (*cb)(const hci_disc_dev_t *d)) { g_hid_found_cb = cb; }

// ---------------------------------------------------------------------------
// Connection table
// ---------------------------------------------------------------------------
static hci_conn_t *conn_alloc(void) {
    for (int i = 0; i < HCI_MAX_CONNECTIONS; i++)
        if (!g_conns[i].active) { memset(&g_conns[i], 0, sizeof(g_conns[i])); return &g_conns[i]; }
    return NULL;
}

hci_conn_t *hci_conn_by_handle(hci_handle_t h) {
    for (int i = 0; i < HCI_MAX_CONNECTIONS; i++)
        if (g_conns[i].active && g_conns[i].handle == h) return &g_conns[i];
    return NULL;
}

hci_conn_t *hci_conn_by_addr(const bt_addr_t *a) {
    for (int i = 0; i < HCI_MAX_CONNECTIONS; i++)
        if (g_conns[i].active && bt_addr_eq(&g_conns[i].peer, a)) return &g_conns[i];
    return NULL;
}

int hci_conn_count(void) {
    int n = 0;
    for (int i = 0; i < HCI_MAX_CONNECTIONS; i++) if (g_conns[i].active) n++;
    return n;
}

static void notify_conn(const hci_conn_t *c, int up) {
    for (int i = 0; i < g_conn_observer_count; i++)
        if (g_conn_observers[i]) g_conn_observers[i](c, up);
}

// ---------------------------------------------------------------------------
// Command queue / send
// ---------------------------------------------------------------------------
static void cmdq_pump(void) {
    while (g_cmd_credits > 0 && g_cmdq_head != g_cmdq_tail) {
        hci_cmd_ent_t *e = &g_cmdq[g_cmdq_head];
        const bt_transport_ops_t *t = bt_transport_get();
        if (!t || !t->send_cmd) return;
        if (t->send_cmd(e->buf, e->len) != BT_OK) return;  // retry next pump
        g_cmd_credits--;
        g_cmdq_head = (g_cmdq_head + 1) % HCI_CMDQ_DEPTH;
    }
}

int hci_send_cmd(uint16_t opcode, const uint8_t *params, uint8_t plen) {
    if (!bt_transport_present()) return BT_ERR_NODEV;
    int next = (g_cmdq_tail + 1) % HCI_CMDQ_DEPTH;
    if (next == g_cmdq_head) return BT_ERR_NOMEM;   // queue full
    hci_cmd_ent_t *e = &g_cmdq[g_cmdq_tail];
    e->buf[0] = (uint8_t)(opcode & 0xFF);
    e->buf[1] = (uint8_t)((opcode >> 8) & 0xFF);
    e->buf[2] = plen;
    if (plen && params) memcpy(e->buf + 3, params, plen);
    e->len = (uint16_t)(3 + plen);
    g_cmdq_tail = next;
    cmdq_pump();
    return BT_OK;
}

// ---------------------------------------------------------------------------
// ACL send (fragment an L2CAP PDU to the controller ACL MTU)
// ---------------------------------------------------------------------------
int hci_send_acl(hci_handle_t handle, const uint8_t *data, uint16_t len) {
    const bt_transport_ops_t *t = bt_transport_get();
    if (!t || !t->send_acl) return BT_ERR_NODEV;
    static uint8_t pkt[HCI_MAX_ACL_LEN + 8];
    uint16_t off = 0;
    int first = 1;
    while (off < len) {
        uint16_t chunk = (uint16_t)(len - off);
        if (chunk > g_acl_mtu) chunk = g_acl_mtu;
        uint16_t hf = HCI_ACL_MK(handle, first ? HCI_PB_FIRST_FLUSH : HCI_PB_CONTINUING, 0);
        pkt[0] = (uint8_t)(hf & 0xFF);
        pkt[1] = (uint8_t)((hf >> 8) & 0xFF);
        pkt[2] = (uint8_t)(chunk & 0xFF);
        pkt[3] = (uint8_t)((chunk >> 8) & 0xFF);
        memcpy(pkt + 4, data + off, chunk);
        if (t->send_acl(pkt, (uint16_t)(4 + chunk)) != BT_OK) return BT_ERR_HCI;
        off = (uint16_t)(off + chunk);
        first = 0;
    }
    return BT_OK;
}

// ---------------------------------------------------------------------------
// Bring-up sequence
// ---------------------------------------------------------------------------
static void bringup_send(uint16_t opcode, const uint8_t *params, uint8_t plen) {
    g_bringup_wait = opcode;
    hci_send_cmd(opcode, params, plen);
}

static void bringup_start(void) {
    bringup_send(HCI_CMD_RESET, NULL, 0);
}

// Advance the bring-up chain when the awaited command completes.
static void bringup_advance(uint16_t opcode, uint8_t status, const uint8_t *ret, int retlen) {
    if (opcode != g_bringup_wait) return;
    if (status != HCI_SUCCESS) {
        kprintf("[BT-HCI] bring-up cmd 0x%04x failed status=0x%02x\n", opcode, status);
        // keep going where sensible; a failed optional step should not stall.
    }
    switch (opcode) {
        case HCI_CMD_RESET:
            bringup_send(HCI_CMD_READ_BD_ADDR, NULL, 0);
            break;
        case HCI_CMD_READ_BD_ADDR:
            if (retlen >= 7) memcpy(g_local_addr.b, ret + 1, 6);  // ret[0]=status
            kprintf("[BT-HCI] local BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x\n",
                    g_local_addr.b[5], g_local_addr.b[4], g_local_addr.b[3],
                    g_local_addr.b[2], g_local_addr.b[1], g_local_addr.b[0]);
            bringup_send(HCI_CMD_READ_BUFFER_SIZE, NULL, 0);
            break;
        case HCI_CMD_READ_BUFFER_SIZE:
            if (retlen >= 3) {
                uint16_t mtu = rd16(ret + 1);   // ret[0]=status, [1..2]=ACL len
                if (mtu >= 27 && mtu <= HCI_MAX_ACL_LEN) g_acl_mtu = mtu;
            }
            kprintf("[BT-HCI] ACL MTU %u\n", g_acl_mtu);
            {
                // Enable all events (incl. SSP 0x31-0x36 and LE meta 0x3E).
                uint8_t mask[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
                bringup_send(HCI_CMD_SET_EVENT_MASK, mask, 8);
            }
            break;
        case HCI_CMD_SET_EVENT_MASK: {
            uint8_t ssp = 1;
            bringup_send(HCI_CMD_WRITE_SSP_MODE, &ssp, 1);
            break;
        }
        case HCI_CMD_WRITE_SSP_MODE: {
            uint8_t cod[3] = { 0x04, 0x01, 0x00 };   // desktop computer (0x000104)
            bringup_send(HCI_CMD_WRITE_CLASS_OF_DEV, cod, 3);
            break;
        }
        case HCI_CMD_WRITE_CLASS_OF_DEV: {
            uint8_t name[248];
            memset(name, 0, sizeof(name));
            const char *n = "MayteraOS";
            for (int i = 0; n[i] && i < 247; i++) name[i] = (uint8_t)n[i];
            bringup_send(HCI_CMD_WRITE_LOCAL_NAME, name, 248);
            break;
        }
        case HCI_CMD_WRITE_LOCAL_NAME: {
            uint8_t scan = HCI_SCAN_BOTH;   // discoverable + connectable
            bringup_send(HCI_CMD_WRITE_SCAN_ENABLE, &scan, 1);
            break;
        }
        case HCI_CMD_WRITE_SCAN_ENABLE: {
            // Enable LE host support so the dual-mode controller will do BLE
            // (HOGP keyboards/mice). {LE_Supported=1, Simultaneous=0}.
            uint8_t le[2] = { 0x01, 0x00 };
            bringup_send(HCI_CMD_WRITE_LE_HOST_SUPPORT, le, 2);
            break;
        }
        case HCI_CMD_WRITE_LE_HOST_SUPPORT: {
            // Unmask the LE meta subevents we act on (conn complete, adv report,
            // conn update, remote features, LTK request).
            uint8_t lem[8] = { 0x1F, 0, 0, 0, 0, 0, 0, 0 };
            bringup_send(HCI_CMD_LE_SET_EVENT_MASK, lem, 8);
            break;
        }
        case HCI_CMD_LE_SET_EVENT_MASK:
            bringup_send(HCI_CMD_LE_READ_BUFFER_SIZE, NULL, 0);
            break;
        case HCI_CMD_LE_READ_BUFFER_SIZE:
            g_bringup_wait = 0;
            g_ready = 1;
            bt_set_state(BT_STATE_READY);
            kprintf("[BT-HCI] controller ready (BR/EDR + LE, discoverable + connectable)\n");
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Event fan-out to observers (pairing / security)
// ---------------------------------------------------------------------------
static int fan_out(uint8_t evt, const uint8_t *params, uint8_t plen) {
    for (int i = 0; i < g_evt_observer_count; i++)
        if (g_evt_observers[i] && g_evt_observers[i](evt, params, plen)) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Event decode
// ---------------------------------------------------------------------------
static void ev_cmd_complete(const uint8_t *p, int plen) {
    if (plen < 3) return;
    g_cmd_credits = p[0];
    uint16_t opcode = rd16(p + 1);
    uint8_t status = (plen >= 4) ? p[3] : HCI_SUCCESS;
    if (opcode != 0x0000)
        bringup_advance(opcode, status, p + 3, plen - 3);
    cmdq_pump();
}

static void ev_cmd_status(const uint8_t *p, int plen) {
    if (plen < 4) return;
    uint8_t status = p[0];
    g_cmd_credits = p[1];
    uint16_t opcode = rd16(p + 2);
    if (status != HCI_SUCCESS && opcode == g_bringup_wait)
        bringup_advance(opcode, status, NULL, 0);
    cmdq_pump();
}

static void ev_conn_request(const uint8_t *p, int plen) {
    if (plen < 10) return;
    bt_addr_t addr; memcpy(addr.b, p, 6);
    uint8_t link_type = p[9];
    kprintf("[BT-HCI] connection request %02x:%02x:%02x:%02x:%02x:%02x link=%u\n",
            addr.b[5], addr.b[4], addr.b[3], addr.b[2], addr.b[1], addr.b[0], link_type);
    if (link_type == 0x01) {          // ACL
        uint8_t par[7];
        memcpy(par, addr.b, 6);
        par[6] = 0x01;                // remain peripheral (device is central)
        hci_send_cmd(HCI_CMD_ACCEPT_CONN_REQ, par, 7);
    }
}

static void ev_conn_complete(const uint8_t *p, int plen) {
    if (plen < 11) return;
    uint8_t status = p[0];
    uint16_t handle = rd16(p + 1);
    bt_addr_t addr; memcpy(addr.b, p + 3, 6);
    kprintf("[BT-HCI] connection complete status=0x%02x handle=0x%04x\n", status, handle);
    if (status != HCI_SUCCESS) return;
    hci_conn_t *c = hci_conn_by_addr(&addr);
    if (!c) c = conn_alloc();
    if (!c) return;
    c->active = 1;
    c->handle = handle;
    c->peer = addr;
    c->type = HCI_LINK_ACL_CLASSIC;
    c->role = 1;
    bt_set_state(BT_STATE_CONNECTED);
    notify_conn(c, 1);
}

static void ev_disconn_complete(const uint8_t *p, int plen) {
    if (plen < 4) return;
    uint16_t handle = rd16(p + 1);
    uint8_t reason = p[3];
    kprintf("[BT-HCI] disconnect handle=0x%04x reason=0x%02x\n", handle, reason);
    hci_conn_t *c = hci_conn_by_handle(handle);
    if (c) {
        notify_conn(c, 0);
        c->active = 0;
        c->handle = HCI_HANDLE_INVALID;
    }
    for (int i = 0; i < HCI_MAX_CONNECTIONS; i++)
        if (g_reasm[i].in_use && g_reasm[i].handle == handle) g_reasm[i].in_use = 0;
    if (hci_conn_count() == 0) bt_set_state(BT_STATE_READY);
}

// Parse the AD structures in an LE advertising report payload, filling name /
// appearance / HID-service flags on the discovered-device record.
static void parse_adv_data(hci_disc_dev_t *d, const uint8_t *ad, int adlen) {
    int i = 0;
    while (i + 2 <= adlen) {
        int len = ad[i];
        if (len == 0) break;
        if (i + 1 + len > adlen) break;
        uint8_t type = ad[i + 1];
        const uint8_t *val = ad + i + 2;
        int vlen = len - 1;
        switch (type) {
            case 0x08:   // shortened local name
            case 0x09: { // complete local name
                int n = vlen < (int)sizeof(d->name) - 1 ? vlen : (int)sizeof(d->name) - 1;
                for (int k = 0; k < n; k++) d->name[k] = (char)val[k];
                d->name[n] = 0;
                break;
            }
            case 0x02:   // incomplete list of 16-bit UUIDs
            case 0x03: { // complete list of 16-bit UUIDs
                for (int k = 0; k + 1 < vlen; k += 2) {
                    uint16_t u = (uint16_t)(val[k] | (val[k + 1] << 8));
                    if (u == 0x1812) d->is_hid = 1;   // HID service
                }
                break;
            }
            case 0x19: { // appearance
                if (vlen >= 2) {
                    uint16_t ap = (uint16_t)(val[0] | (val[1] << 8));
                    if (ap == 0x03C1) { d->is_hid = 1; d->appearance = 1; }      // keyboard
                    else if (ap == 0x03C2) { d->is_hid = 1; d->appearance = 2; } // mouse
                    else if ((ap & 0xFFC0) == 0x03C0) d->is_hid = 1;             // HID category
                }
                break;
            }
            default: break;
        }
        i += 1 + len;
    }
}

static void ev_adv_report(const uint8_t *p, int plen) {
    // p[0]=subevent, p[1]=num_reports, then per report:
    //   evt_type(1) addr_type(1) addr(6) data_len(1) data(N) rssi(1)
    if (plen < 2) return;
    int num = p[1];
    int off = 2;
    for (int r = 0; r < num; r++) {
        if (off + 9 > plen) break;
        uint8_t adv_type = p[off];
        uint8_t addr_type = p[off + 1];
        bt_addr_t addr; memcpy(addr.b, p + off + 2, 6);
        int dlen = p[off + 8];
        if (off + 9 + dlen + 1 > plen) break;
        const uint8_t *ad = p + off + 9;
        int8_t rssi = (int8_t)p[off + 9 + dlen];
        off += 9 + dlen + 1;

        hci_disc_dev_t *d = disc_get_or_add(&addr, 1);
        if (!d) continue;
        int was_hid = d->is_hid;
        d->addr_type = addr_type;
        d->adv_type = adv_type;
        d->rssi = rssi;
        parse_adv_data(d, ad, dlen);
        if (d->is_hid && !was_hid) {
            kprintf("[BT-HCI] HID device found %02x:%02x:%02x:%02x:%02x:%02x '%s' (type=%s ap=%u)\n",
                    addr.b[5], addr.b[4], addr.b[3], addr.b[2], addr.b[1], addr.b[0],
                    d->name, addr_type ? "random" : "public", d->appearance);
            if (g_hid_found_cb) g_hid_found_cb(d);
        }
    }
}

static void ev_le_meta(const uint8_t *p, int plen) {
    if (plen < 1) return;
    uint8_t sub = p[0];
    if (sub == HCI_LE_SUBEVT_CONN_COMPLETE && plen >= 18) {
        uint8_t status = p[1];
        uint16_t handle = rd16(p + 2);
        uint8_t peer_type = p[5];
        bt_addr_t addr; memcpy(addr.b, p + 6, 6);
        kprintf("[BT-HCI] LE connection complete status=0x%02x handle=0x%04x peer=%02x:%02x:%02x:%02x:%02x:%02x\n",
                status, handle, addr.b[5], addr.b[4], addr.b[3], addr.b[2], addr.b[1], addr.b[0]);
        if (status != HCI_SUCCESS) { bt_set_state(BT_STATE_READY); return; }
        hci_conn_t *c = hci_conn_by_addr(&addr);
        if (!c) c = conn_alloc();
        if (!c) return;
        c->active = 1; c->handle = handle; c->peer = addr; c->type = HCI_LINK_LE;
        c->role = (p[4] == 0x00) ? 0 : 1;
        c->peer_addr_type = peer_type;
        bt_set_state(BT_STATE_CONNECTED);
        notify_conn(c, 1);
    } else if (sub == HCI_LE_SUBEVT_ADV_REPORT) {
        ev_adv_report(p, plen);
    } else if (sub == HCI_LE_SUBEVT_LTK_REQUEST) {
        // hand to pairing (SMP) observers.
        fan_out(HCI_EVT_LE_META, p, (uint8_t)plen);
    }
}

static void hci_event(const uint8_t *data, int len) {
    if (len < 2) return;
    uint8_t evt = data[0];
    uint8_t plen = data[1];
    if (len < 2 + (int)plen) return;
    const uint8_t *p = data + 2;

    switch (evt) {
        case HCI_EVT_CMD_COMPLETE:  ev_cmd_complete(p, plen); break;
        case HCI_EVT_CMD_STATUS:    ev_cmd_status(p, plen); break;
        case HCI_EVT_CONN_REQUEST:  ev_conn_request(p, plen); break;
        case HCI_EVT_CONN_COMPLETE: ev_conn_complete(p, plen); break;
        case HCI_EVT_DISCONN_COMPLETE: ev_disconn_complete(p, plen); break;
        case HCI_EVT_LE_META:       ev_le_meta(p, plen); break;
        case HCI_EVT_ENCRYPT_CHANGE: {
            if (plen >= 4) {
                uint16_t h = rd16(p + 1);
                uint8_t enabled = p[3];
                hci_conn_t *c = hci_conn_by_handle(h);
                if (c) c->encrypted = enabled ? 1 : 0;
                kprintf("[BT-HCI] encryption change handle=0x%04x status=0x%02x enabled=%u\n",
                        h, p[0], enabled);
                for (int i = 0; i < g_encrypt_observer_count; i++)
                    if (g_encrypt_observers[i]) g_encrypt_observers[i](h, enabled);
            }
            fan_out(HCI_EVT_ENCRYPT_CHANGE, p, plen);
            break;
        }
        case HCI_EVT_NUM_COMPLETED_PKTS: break;  // ACL flow-control ack
        case HCI_EVT_REMOTE_NAME_REQ_COMPLETE: break;
        case HCI_EVT_INQUIRY_COMPLETE: break;
        // Pairing / security events go to observers (pair.c).
        case HCI_EVT_IO_CAP_REQUEST:
        case HCI_EVT_IO_CAP_RESPONSE:
        case HCI_EVT_USER_CONFIRM_REQUEST:
        case HCI_EVT_USER_PASSKEY_REQUEST:
        case HCI_EVT_SIMPLE_PAIRING_COMPLETE:
        case HCI_EVT_LINK_KEY_REQUEST:
        case HCI_EVT_LINK_KEY_NOTIFICATION:
        case HCI_EVT_PIN_CODE_REQUEST:
        case HCI_EVT_AUTH_COMPLETE:
            if (!fan_out(evt, p, plen))
                kprintf("[BT-HCI] pairing event 0x%02x unhandled\n", evt);
            break;
        default:
            // Give observers a chance at anything else, then drop quietly.
            fan_out(evt, p, plen);
            break;
    }
}

// Transport RX upcall for events.
static void hci_on_event(const uint8_t *data, uint16_t len) {
    hci_event(data, (int)len);
}

// ---------------------------------------------------------------------------
// ACL reassembly -> L2CAP PDUs
// ---------------------------------------------------------------------------
static acl_reasm_t *reasm_for(uint16_t handle) {
    for (int i = 0; i < HCI_MAX_CONNECTIONS; i++)
        if (g_reasm[i].in_use && g_reasm[i].handle == handle) return &g_reasm[i];
    for (int i = 0; i < HCI_MAX_CONNECTIONS; i++)
        if (!g_reasm[i].in_use) {
            g_reasm[i].in_use = 1; g_reasm[i].handle = handle;
            g_reasm[i].want = 0; g_reasm[i].have = 0;
            return &g_reasm[i];
        }
    return NULL;
}

static void hci_on_acl(const uint8_t *data, uint16_t len) {
    if (len < 4) return;
    uint16_t hf = rd16(data);
    uint16_t handle = HCI_ACL_HANDLE(hf);
    uint8_t pb = HCI_ACL_PB(hf);
    uint16_t dlen = rd16(data + 2);
    if (len < 4 + (int)dlen) return;
    const uint8_t *payload = data + 4;

    acl_reasm_t *r = reasm_for(handle);
    if (!r) return;

    if (pb == HCI_PB_FIRST_FLUSH || pb == HCI_PB_FIRST_NONFLUSH) {
        r->have = 0;
        r->want = 0;
        if (dlen >= 2) {
            uint16_t l2len = rd16(payload);   // L2CAP payload length
            r->want = (uint16_t)(4 + l2len);  // + 4-byte L2CAP header
        }
        if (r->want == 0 || r->want > sizeof(r->buf)) { r->in_use = 1; return; }
    }
    if (r->have + dlen > sizeof(r->buf)) { r->have = 0; r->want = 0; return; }
    memcpy(r->buf + r->have, payload, dlen);
    r->have = (uint16_t)(r->have + dlen);

    if (r->want && r->have >= r->want) {
        if (g_acl_sink) g_acl_sink(handle, r->buf, r->want);
        r->have = 0;
        r->want = 0;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
int hci_disconnect(hci_handle_t h, uint8_t reason) {
    uint8_t par[3] = { (uint8_t)(h & 0xFF), (uint8_t)((h >> 8) & 0xFF), reason };
    return hci_send_cmd(HCI_CMD_DISCONNECT, par, 3);
}

const bt_addr_t *hci_local_addr(void) { return &g_local_addr; }

int hci_add_evt_observer(hci_evt_observer_t cb) {
    if (!cb || g_evt_observer_count >= (int)(sizeof(g_evt_observers)/sizeof(g_evt_observers[0])))
        return BT_ERR_NOMEM;
    g_evt_observers[g_evt_observer_count++] = cb;
    return BT_OK;
}

void hci_set_acl_sink(hci_acl_sink_t sink) { g_acl_sink = sink; }

int hci_add_conn_observer(hci_conn_cb_t cb) {
    if (!cb || g_conn_observer_count >= (int)(sizeof(g_conn_observers)/sizeof(g_conn_observers[0])))
        return BT_ERR_NOMEM;
    g_conn_observers[g_conn_observer_count++] = cb;
    return BT_OK;
}

int hci_add_encrypt_observer(hci_encrypt_cb_t cb) {
    if (!cb || g_encrypt_observer_count >= (int)(sizeof(g_encrypt_observers)/sizeof(g_encrypt_observers[0])))
        return BT_ERR_NOMEM;
    g_encrypt_observers[g_encrypt_observer_count++] = cb;
    return BT_OK;
}

// ---------------------------------------------------------------------------
// LE scan / connect / encryption
// ---------------------------------------------------------------------------
static int g_le_scanning = 0;

int hci_le_scan(int enable) {
    if (enable) {
        // Active scan (type=1) so we also collect scan responses (names).
        // interval=0x0030, window=0x0030, own_addr=public(0), filter=accept-all(0)
        uint8_t sp[7] = { 0x01, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00 };
        hci_send_cmd(HCI_CMD_LE_SET_SCAN_PARAMS, sp, 7);
        uint8_t en[2] = { 0x01, 0x00 };   // enable, no filter-dup (see all reports)
        hci_send_cmd(HCI_CMD_LE_SET_SCAN_ENABLE, en, 2);
        g_le_scanning = 1;
        bt_set_state(BT_STATE_SCANNING);
        kprintf("[BT-HCI] LE active scan enabled\n");
    } else {
        uint8_t dis[2] = { 0x00, 0x00 };
        hci_send_cmd(HCI_CMD_LE_SET_SCAN_ENABLE, dis, 2);
        g_le_scanning = 0;
        if (bt_get_state() == BT_STATE_SCANNING) bt_set_state(BT_STATE_READY);
        kprintf("[BT-HCI] LE scan disabled\n");
    }
    return BT_OK;
}

int hci_le_connect(const bt_addr_t *addr, uint8_t addr_type) {
    if (!addr) return BT_ERR_PARAM;
    if (g_le_scanning) hci_le_scan(0);   // must stop scanning before connecting
    // LE_Create_Connection: scan_interval(2)=0x0060, scan_window(2)=0x0030,
    // filter_policy=0, peer_addr_type(1), peer_addr(6), own_addr_type=public(0),
    // conn_interval_min(2)=0x0018, max(2)=0x0028, latency(2)=0, timeout(2)=0x002A,
    // min_ce(2)=0, max_ce(2)=0.
    uint8_t p[25];
    int i = 0;
    p[i++] = 0x60; p[i++] = 0x00;    // scan interval
    p[i++] = 0x30; p[i++] = 0x00;    // scan window
    p[i++] = 0x00;                   // init filter policy: use peer addr
    p[i++] = addr_type;              // peer addr type
    memcpy(p + i, addr->b, 6); i += 6;
    p[i++] = 0x00;                   // own addr type: public
    p[i++] = 0x18; p[i++] = 0x00;    // conn interval min
    p[i++] = 0x28; p[i++] = 0x00;    // conn interval max
    p[i++] = 0x00; p[i++] = 0x00;    // latency
    p[i++] = 0x2A; p[i++] = 0x00;    // supervision timeout
    p[i++] = 0x00; p[i++] = 0x00;    // min CE length
    p[i++] = 0x00; p[i++] = 0x00;    // max CE length
    bt_set_state(BT_STATE_CONNECTING);
    kprintf("[BT-HCI] LE_Create_Connection -> %02x:%02x:%02x:%02x:%02x:%02x (type=%s)\n",
            addr->b[5], addr->b[4], addr->b[3], addr->b[2], addr->b[1], addr->b[0],
            addr_type ? "random" : "public");
    return hci_send_cmd(HCI_CMD_LE_CREATE_CONNECTION, p, (uint8_t)i);
}

int hci_le_start_encryption(hci_handle_t h, const uint8_t rand8[8],
                            uint16_t ediv, const uint8_t ltk16[16]) {
    uint8_t p[28];
    p[0] = (uint8_t)(h & 0xFF);
    p[1] = (uint8_t)((h >> 8) & 0xFF);
    memcpy(p + 2, rand8, 8);
    p[10] = (uint8_t)(ediv & 0xFF);
    p[11] = (uint8_t)((ediv >> 8) & 0xFF);
    memcpy(p + 12, ltk16, 16);
    kprintf("[BT-HCI] LE_Start_Encryption handle=0x%04x\n", h);
    return hci_send_cmd(HCI_CMD_LE_START_ENCRYPTION, p, 28);
}

int hci_is_ready(void) { return g_ready; }

void hci_poll(void) {
    // Transport RX is push (bt_transport_deliver_*). Here we only nudge the
    // command queue in case credits freed up between events.
    cmdq_pump();
}

int hci_init(void) {
    for (int i = 0; i < HCI_MAX_CONNECTIONS; i++) {
        g_conns[i].active = 0;
        g_conns[i].handle = HCI_HANDLE_INVALID;
        g_reasm[i].in_use = 0;
    }
    g_cmd_credits = 1;
    g_cmdq_head = g_cmdq_tail = 0;
    g_ready = 0;
    g_bringup_wait = 0;

    g_disc_count = 0;

    static const bt_transport_rx_t rx = { hci_on_event, hci_on_acl };
    bt_transport_set_rx(&rx);

    if (!bt_transport_present()) {
        kprintf("[BT-HCI] no transport; init deferred\n");
        return BT_ERR_NODEV;
    }
    // NOTE: the async controller bring-up (reset -> ... -> LE init -> ready) is
    // NOT started here. The transport must first load vendor firmware (Realtek
    // RTL8761BU) and then call hci_start_bringup(). This avoids the two halves
    // fighting over the interrupt-IN event stream (the transport reaps events
    // synchronously during firmware upload; if hci.c also queued a RESET here,
    // the two consumers would steal each other's Command Complete events).
    kprintf("[BT-HCI] init: RX wired; awaiting transport-ready to start bring-up\n");
    return BT_OK;
}

// Called by the transport (hci_usb.c) once the controller is ready to speak HCI
// (after any vendor firmware upload). Starts the async bring-up chain.
void hci_start_bringup(void) {
    if (!bt_transport_present()) return;
    g_cmd_credits = 1;
    g_ready = 0;
    g_bringup_wait = 0;
    kprintf("[BT-HCI] transport ready: starting controller bring-up\n");
    bringup_start();
}
