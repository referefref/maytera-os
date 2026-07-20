// gatt.c - Minimal ATT/GATT client + HOGP (BLE HID) for MayteraOS (#372).
//
// State machine per BLE link, driven by ATT responses (strictly one outstanding
// ATT request at a time on the ATT bearer, CID 0x0004):
//
//   GS_MTU   -> Exchange MTU
//   GS_SVC   -> Read_By_Group_Type(0x2800) to find the HID service (0x1812)
//   GS_CHAR  -> Read_By_Type(0x2803) inside the HID range: collect report /
//               boot-report / protocol-mode characteristics
//   GS_DESC  -> Find_Info inside the HID range: collect CCCD (0x2902) handles,
//               map each to its report characteristic
//   GS_ENABLE-> write Protocol Mode = Boot, then write each report CCCD = 0x0001
//               (enable notifications). If a write returns Insufficient
//               Authentication/Encryption, kick SMP pairing and resume on the
//               encryption-change event.
//   GS_READY -> Handle-Value-Notifications (0x1B) carry input reports, funnelled
//               through bt_hid_input_report() (same sink as USB HID).
#include "gatt.h"
#include "hci.h"
#include "l2cap.h"
#include "hid.h"
#include "pair.h"
#include "../serial.h"
#include "../string.h"

// --- ATT opcodes ---
#define ATT_ERROR_RSP         0x01
#define ATT_EXCHANGE_MTU_REQ  0x02
#define ATT_EXCHANGE_MTU_RSP  0x03
#define ATT_FIND_INFO_REQ     0x04
#define ATT_FIND_INFO_RSP     0x05
#define ATT_READ_BY_TYPE_REQ  0x08
#define ATT_READ_BY_TYPE_RSP  0x09
#define ATT_READ_REQ          0x0A
#define ATT_READ_RSP          0x0B
#define ATT_READ_BY_GROUP_REQ 0x10
#define ATT_READ_BY_GROUP_RSP 0x11
#define ATT_WRITE_REQ         0x12
#define ATT_WRITE_RSP         0x13
#define ATT_HANDLE_VALUE_NTF  0x1B
#define ATT_WRITE_CMD         0x52

// --- GATT/HOGP UUIDs (16-bit) ---
#define UUID_PRIMARY_SVC   0x2800
#define UUID_CHARACTERISTIC 0x2803
#define UUID_CCCD          0x2902
#define UUID_HID_SERVICE   0x1812
#define UUID_REPORT        0x2A4D
#define UUID_BOOT_KB_IN    0x2A22
#define UUID_BOOT_MOUSE_IN 0x2A33
#define UUID_PROTOCOL_MODE 0x2A4E

// --- ATT error codes ---
#define ATT_ERR_INVALID_HANDLE   0x01
#define ATT_ERR_INSUFF_AUTHEN    0x05
#define ATT_ERR_INSUFF_ENCRYPT   0x0F
#define ATT_ERR_ATTR_NOT_FOUND   0x0A

// --- HID report kinds (mirror hid.h bt_hid_kind_t) ---
#define REP_UNKNOWN  0
#define REP_KEYBOARD 1
#define REP_MOUSE    2

typedef enum {
    GS_IDLE = 0, GS_MTU, GS_SVC, GS_CHAR, GS_DESC, GS_ENABLE, GS_READY, GS_FAILED
} gatt_state_t;

#define GATT_MAX_REPORTS 8
#define GATT_MAX_CCCD    8
#define GATT_MAX_SESS    2

typedef struct {
    int          active;
    hci_handle_t handle;
    bt_addr_t    addr;
    uint8_t      addr_type;
    gatt_state_t state;
    uint16_t     mtu;
    uint16_t     hid_start, hid_end;
    uint16_t     proto_mode_handle;
    struct { uint16_t val; uint16_t cccd; uint8_t kind; uint8_t is_boot; } rep[GATT_MAX_REPORTS];
    int          nrep;
    uint16_t     cccd_tmp[GATT_MAX_CCCD];
    int          ncccd_tmp;
    uint16_t     disc_cursor;
    int          enable_idx;     // -1 = protocol mode step, then 0..nrep-1 = CCCDs
    uint8_t      last_req;
    int          pairing_kicked;
    int          attached;
} gatt_sess_t;

static gatt_sess_t g_sess[GATT_MAX_SESS];

static gatt_sess_t *sess_by_handle(hci_handle_t h) {
    for (int i = 0; i < GATT_MAX_SESS; i++)
        if (g_sess[i].active && g_sess[i].handle == h) return &g_sess[i];
    return NULL;
}

static gatt_sess_t *sess_alloc(hci_handle_t h) {
    gatt_sess_t *s = sess_by_handle(h);
    if (s) return s;
    for (int i = 0; i < GATT_MAX_SESS; i++)
        if (!g_sess[i].active) {
            memset(&g_sess[i], 0, sizeof(g_sess[i]));
            g_sess[i].active = 1;
            g_sess[i].handle = h;
            g_sess[i].mtu = 23;
            return &g_sess[i];
        }
    return NULL;
}

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void att_send(gatt_sess_t *s, const uint8_t *pdu, uint16_t len) {
    s->last_req = pdu[0];
    l2cap_send_fixed(s->handle, L2CAP_CID_LE_ATT, pdu, len);
}

// ---------------------------------------------------------------------------
// Request senders (each advances / re-issues one discovery step)
// ---------------------------------------------------------------------------
static void send_mtu(gatt_sess_t *s) {
    uint8_t p[3]; p[0] = ATT_EXCHANGE_MTU_REQ; wr16(p + 1, 247);
    s->state = GS_MTU; att_send(s, p, 3);
}

static void send_disc_svc(gatt_sess_t *s, uint16_t start) {
    uint8_t p[7];
    p[0] = ATT_READ_BY_GROUP_REQ;
    wr16(p + 1, start); wr16(p + 3, 0xFFFF); wr16(p + 5, UUID_PRIMARY_SVC);
    s->state = GS_SVC; att_send(s, p, 7);
}

static void send_disc_char(gatt_sess_t *s, uint16_t start) {
    uint8_t p[7];
    p[0] = ATT_READ_BY_TYPE_REQ;
    wr16(p + 1, start); wr16(p + 3, s->hid_end); wr16(p + 5, UUID_CHARACTERISTIC);
    s->state = GS_CHAR; att_send(s, p, 7);
}

static void send_disc_desc(gatt_sess_t *s, uint16_t start) {
    uint8_t p[5];
    p[0] = ATT_FIND_INFO_REQ;
    wr16(p + 1, start); wr16(p + 3, s->hid_end);
    s->state = GS_DESC; att_send(s, p, 5);
}

static void send_write(gatt_sess_t *s, uint16_t handle, const uint8_t *val, int vlen) {
    uint8_t p[8];
    p[0] = ATT_WRITE_REQ;
    wr16(p + 1, handle);
    for (int i = 0; i < vlen && i < 4; i++) p[3 + i] = val[i];
    att_send(s, p, (uint16_t)(3 + vlen));
}

// Map collected CCCD handles to their report characteristics (each CCCD belongs
// to the report characteristic with the greatest value handle below it).
static void map_cccds(gatt_sess_t *s) {
    for (int c = 0; c < s->ncccd_tmp; c++) {
        uint16_t ch = s->cccd_tmp[c];
        int best = -1; uint16_t bestval = 0;
        for (int r = 0; r < s->nrep; r++)
            if (s->rep[r].val < ch && s->rep[r].val >= bestval) { best = r; bestval = s->rep[r].val; }
        if (best >= 0 && s->rep[best].cccd == 0) s->rep[best].cccd = ch;
    }
}

// Begin (or resume) enabling notifications: protocol mode then each report CCCD.
static void start_enable(gatt_sess_t *s) {
    s->state = GS_ENABLE;
    s->enable_idx = -1;   // protocol mode first
    // Decide boot vs report mode: boot if any boot report characteristic exists.
    int have_boot = 0;
    for (int r = 0; r < s->nrep; r++) if (s->rep[r].is_boot) have_boot = 1;
    if (s->proto_mode_handle) {
        uint8_t mode = have_boot ? 0x00 : 0x01;   // 0 = Boot, 1 = Report
        send_write(s, s->proto_mode_handle, &mode, 1);
    } else {
        // no protocol mode char: jump straight to CCCD writes
        s->enable_idx = 0;
        if (s->nrep > 0 && s->rep[0].cccd) {
            uint8_t en[2] = { 0x01, 0x00 };
            send_write(s, s->rep[0].cccd, en, 2);
        } else {
            s->state = GS_READY;
            kprintf("[BT-GATT] no CCCDs to subscribe; HID device idle\n");
        }
    }
}

static void enable_next(gatt_sess_t *s) {
    // advance to the next report CCCD to subscribe
    for (int r = s->enable_idx + 1; r < s->nrep; r++) {
        if (s->rep[r].cccd) {
            s->enable_idx = r;
            uint8_t en[2] = { 0x01, 0x00 };
            send_write(s, s->rep[r].cccd, en, 2);
            return;
        }
    }
    // done
    s->state = GS_READY;
    kprintf("[BT-GATT] HOGP ready: subscribed %d report notification(s) on handle 0x%04x\n",
            s->nrep, s->handle);
    bt_hid_attach_ble(s->handle, &s->addr, BT_HID_OTHER);
    s->attached = 1;
}

// ---------------------------------------------------------------------------
// ATT response handlers
// ---------------------------------------------------------------------------
static void on_group_rsp(gatt_sess_t *s, const uint8_t *p, int n) {
    // p[0]=0x11, p[1]=len (per-entry), then entries {handle(2), end(2), uuid}
    if (n < 2) return;
    int elen = p[1];
    int i = 2;
    uint16_t last_end = 0;
    while (i + elen <= n) {
        uint16_t h   = rd16(p + i);
        uint16_t end = rd16(p + i + 2);
        last_end = end;
        if (elen == 6) {
            uint16_t uuid = rd16(p + i + 4);
            if (uuid == UUID_HID_SERVICE) {
                s->hid_start = h; s->hid_end = end;
                kprintf("[BT-GATT] HID service 0x1812 found: handles 0x%04x..0x%04x\n", h, end);
            }
        }
        i += elen;
    }
    if (s->hid_start) { s->nrep = 0; send_disc_char(s, s->hid_start); return; }
    if (last_end < 0xFFFF) { send_disc_svc(s, (uint16_t)(last_end + 1)); return; }
    kprintf("[BT-GATT] no HID service on this device; GATT idle\n");
    s->state = GS_FAILED;
}

static void on_type_rsp(gatt_sess_t *s, const uint8_t *p, int n) {
    // characteristic declaration: entry {decl_handle(2), props(1), val_handle(2), uuid}
    if (n < 2) return;
    int elen = p[1];
    int i = 2;
    uint16_t last = 0;
    while (i + elen <= n) {
        uint16_t decl = rd16(p + i);
        uint8_t  props = p[i + 2];
        uint16_t val = rd16(p + i + 3);
        last = decl;
        if (elen == 7) {
            uint16_t uuid = rd16(p + i + 5);
            if (uuid == UUID_PROTOCOL_MODE) {
                s->proto_mode_handle = val;
            } else if ((uuid == UUID_REPORT || uuid == UUID_BOOT_KB_IN || uuid == UUID_BOOT_MOUSE_IN)
                       && (props & 0x10) && s->nrep < GATT_MAX_REPORTS) {
                s->rep[s->nrep].val = val;
                s->rep[s->nrep].cccd = 0;
                s->rep[s->nrep].is_boot = (uuid != UUID_REPORT);
                s->rep[s->nrep].kind = (uuid == UUID_BOOT_KB_IN)  ? REP_KEYBOARD :
                                       (uuid == UUID_BOOT_MOUSE_IN) ? REP_MOUSE : REP_UNKNOWN;
                s->nrep++;
                kprintf("[BT-GATT] input report char uuid=0x%04x val_handle=0x%04x boot=%d\n",
                        uuid, val, uuid != UUID_REPORT);
            }
        }
        i += elen;
    }
    if (last && last < s->hid_end) { send_disc_char(s, (uint16_t)(last + 1)); return; }
    // characteristics done -> discover descriptors (CCCDs)
    s->ncccd_tmp = 0;
    send_disc_desc(s, s->hid_start);
}

static void on_find_info_rsp(gatt_sess_t *s, const uint8_t *p, int n) {
    // p[1]=format (1 = 16-bit uuid, 2 = 128-bit), entries {handle(2), uuid}
    if (n < 2) return;
    int fmt = p[1];
    int esz = (fmt == 1) ? 4 : 18;
    int i = 2;
    uint16_t last = 0;
    while (i + esz <= n) {
        uint16_t h = rd16(p + i);
        last = h;
        if (fmt == 1) {
            uint16_t uuid = rd16(p + i + 2);
            if (uuid == UUID_CCCD && s->ncccd_tmp < GATT_MAX_CCCD)
                s->cccd_tmp[s->ncccd_tmp++] = h;
        }
        i += esz;
    }
    if (last && last < s->hid_end) { send_disc_desc(s, (uint16_t)(last + 1)); return; }
    // descriptors done -> map CCCDs and start enabling
    map_cccds(s);
    kprintf("[BT-GATT] discovery complete: %d report char(s), %d CCCD(s)\n",
            s->nrep, s->ncccd_tmp);
    start_enable(s);
}

static void on_write_rsp(gatt_sess_t *s) {
    if (s->state != GS_ENABLE) return;
    if (s->enable_idx == -1) {
        // protocol mode written; start CCCD subscriptions
        s->enable_idx = -1;
        enable_next(s);
    } else {
        enable_next(s);
    }
}

static void on_error_rsp(gatt_sess_t *s, const uint8_t *p, int n) {
    if (n < 5) return;
    uint8_t req = p[1];
    uint8_t err = p[4];
    kprintf("[BT-GATT] ATT error: req=0x%02x handle=0x%04x err=0x%02x\n",
            req, rd16(p + 2), err);
    if (err == ATT_ERR_ATTR_NOT_FOUND) {
        // benign "end of discovery" - advance the phase that hit the end.
        if (s->state == GS_SVC) {
            if (s->hid_start) { s->nrep = 0; send_disc_char(s, s->hid_start); }
            else { kprintf("[BT-GATT] no HID service; GATT idle\n"); s->state = GS_FAILED; }
        } else if (s->state == GS_CHAR) {
            s->ncccd_tmp = 0; send_disc_desc(s, s->hid_start);
        } else if (s->state == GS_DESC) {
            map_cccds(s); start_enable(s);
        }
        return;
    }
    if (err == ATT_ERR_INSUFF_AUTHEN || err == ATT_ERR_INSUFF_ENCRYPT) {
        if (!s->pairing_kicked) {
            s->pairing_kicked = 1;
            kprintf("[BT-GATT] write needs encryption; starting SMP pairing\n");
            pair_start_le(s->handle, &s->addr);
        }
        // resume happens on the encryption-change observer
        return;
    }
    // other errors: give up on the failing step
    s->state = GS_FAILED;
}

static void on_notification(gatt_sess_t *s, const uint8_t *p, int n) {
    if (n < 3) return;
    uint16_t vh = rd16(p + 1);
    const uint8_t *report = p + 3;
    int rlen = n - 3;
    if (rlen <= 0) return;

    uint8_t kind = REP_UNKNOWN;
    for (int r = 0; r < s->nrep; r++)
        if (s->rep[r].val == vh) { kind = s->rep[r].kind; break; }
    if (kind == REP_UNKNOWN) kind = (rlen >= 7) ? REP_KEYBOARD : REP_MOUSE;

    bt_hid_input_report(kind == REP_KEYBOARD ? BT_HID_KEYBOARD : BT_HID_MOUSE,
                        report, (uint16_t)rlen);
}

void gatt_att_input(hci_handle_t h, const uint8_t *data, uint16_t len) {
    if (len < 1) return;
    gatt_sess_t *s = sess_by_handle(h);
    if (!s) { s = sess_alloc(h); if (!s) return; }
    uint8_t op = data[0];
    switch (op) {
        case ATT_EXCHANGE_MTU_RSP:
            if (len >= 3) {
                uint16_t peer = rd16(data + 1);
                s->mtu = peer < 247 ? peer : 247;
            }
            kprintf("[BT-GATT] MTU = %u; discovering services\n", s->mtu);
            send_disc_svc(s, 0x0001);
            break;
        case ATT_READ_BY_GROUP_RSP: on_group_rsp(s, data, len); break;
        case ATT_READ_BY_TYPE_RSP:  on_type_rsp(s, data, len); break;
        case ATT_FIND_INFO_RSP:     on_find_info_rsp(s, data, len); break;
        case ATT_WRITE_RSP:         on_write_rsp(s); break;
        case ATT_ERROR_RSP:         on_error_rsp(s, data, len); break;
        case ATT_HANDLE_VALUE_NTF:  on_notification(s, data, len); break;
        default:
            // We are a GATT client. An inbound ATT *request* (even opcode, not a
            // command/notification) means the peer is discovering us; reply with
            // Error "Request Not Supported" so it does not hang. Commands (bit6)
            // and notifications/indications are just dropped.
            if (!(op & 0x40) && op != ATT_HANDLE_VALUE_NTF && op != 0x1D && op != 0x1E) {
                uint16_t req_handle = (len >= 3) ? rd16(data + 1) : 0;
                uint8_t err[5] = { ATT_ERROR_RSP, op,
                                   (uint8_t)req_handle, (uint8_t)(req_handle >> 8), 0x06 };
                l2cap_send_fixed(h, L2CAP_CID_LE_ATT, err, 5);
            }
            kprintf("[BT-GATT] inbound ATT opcode 0x%02x (peer-initiated; answered/ignored)\n", op);
            break;
    }
}

// ---------------------------------------------------------------------------
// Connection / encryption observers + HID auto-target
// ---------------------------------------------------------------------------
static void gatt_conn_event(const hci_conn_t *c, int up) {
    if (c->type != HCI_LINK_LE) return;
    if (up) {
        gatt_sess_t *s = sess_alloc(c->handle);
        if (!s) return;
        s->addr = c->peer;
        s->addr_type = c->peer_addr_type;
        kprintf("[BT-GATT] LE link up (handle 0x%04x); exchanging ATT MTU\n", c->handle);
        send_mtu(s);
    } else {
        gatt_sess_t *s = sess_by_handle(c->handle);
        if (s) { if (s->attached) bt_hid_detach(c->handle); s->active = 0; }
    }
}

static void gatt_encrypt_event(hci_handle_t h, uint8_t enabled) {
    gatt_sess_t *s = sess_by_handle(h);
    if (!s || !enabled) return;
    if (s->state == GS_ENABLE || s->state == GS_FAILED) {
        kprintf("[BT-GATT] link encrypted; retrying notification enable\n");
        s->pairing_kicked = 0;
        start_enable(s);
    }
}

// Auto-target: when a HID (0x1812 / kbd/mouse appearance) device is discovered,
// connect to it so a real keyboard/mouse "just works" without a UI.
static int g_autoconnect_busy = 0;
static void gatt_hid_found(const hci_disc_dev_t *d) {
    if (g_autoconnect_busy) return;
    g_autoconnect_busy = 1;
    kprintf("[BT-GATT] auto-connecting to HID device %02x:%02x:%02x:%02x:%02x:%02x\n",
            d->addr.b[5], d->addr.b[4], d->addr.b[3], d->addr.b[2], d->addr.b[1], d->addr.b[0]);
    hci_le_connect(&d->addr, d->addr_type);
}

void gatt_poll(void) { /* event-driven */ }

int gatt_init(void) {
    memset(g_sess, 0, sizeof(g_sess));
    g_autoconnect_busy = 0;
    hci_add_conn_observer(gatt_conn_event);
    hci_add_encrypt_observer(gatt_encrypt_event);
    hci_set_hid_found_cb(gatt_hid_found);
    kprintf("[BT-GATT] init: GATT/HOGP client ready (conn+encrypt observers, HID auto-target)\n");
    return BT_OK;
}
