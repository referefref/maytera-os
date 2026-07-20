// l2cap.c - L2CAP signalling + connection-oriented channels (#372, PROTOCOL).
//
// Implements the BR/EDR signalling channel (CID 0x0001): connection request/
// response, configuration request/response (MTU only), disconnect, info. Data
// channels for dynamic PSMs (HID control/interrupt, SDP) are routed to their
// registered server callbacks. Registers as the HCI ACL sink and as a
// connection observer so channels are torn down when their ACL link drops.
//
// LE fixed channels (ATT 0x0004 / SMP 0x0006) are recognised and logged; their
// GATT/SMP handlers are a later phase (classic HIDP is the phase-1 target).
#include "l2cap.h"
#include "hci.h"
#include "../serial.h"
#include "../string.h"

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }

static l2cap_chan_t g_chans[L2CAP_MAX_CHANNELS];
static uint8_t      g_local_cfg_done[L2CAP_MAX_CHANNELS];
static uint8_t      g_remote_cfg_done[L2CAP_MAX_CHANNELS];

typedef struct { uint16_t psm; const l2cap_callbacks_t *cb; } l2cap_server_t;
static l2cap_server_t g_servers[8];
static int g_server_count = 0;

static l2cap_cid_t g_next_scid = L2CAP_CID_DYN_BASE;
static uint8_t     g_next_id   = 1;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static int chan_index(const l2cap_chan_t *c) { return (int)(c - g_chans); }

static l2cap_chan_t *chan_alloc(void) {
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        if (!g_chans[i].active) {
            memset(&g_chans[i], 0, sizeof(g_chans[i]));
            g_chans[i].active = 1;
            g_chans[i].scid = g_next_scid++;
            if (g_next_scid < L2CAP_CID_DYN_BASE || g_next_scid == 0) g_next_scid = L2CAP_CID_DYN_BASE;
            g_chans[i].mtu = L2CAP_DEFAULT_MTU;
            g_chans[i].state = L2CAP_CLOSED;
            g_local_cfg_done[i] = 0;
            g_remote_cfg_done[i] = 0;
            return &g_chans[i];
        }
    }
    return NULL;
}

static void chan_free(l2cap_chan_t *c) {
    if (!c) return;
    c->active = 0;
    c->state = L2CAP_CLOSED;
}

l2cap_chan_t *l2cap_chan_by_scid(hci_handle_t h, l2cap_cid_t scid) {
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++)
        if (g_chans[i].active && g_chans[i].handle == h && g_chans[i].scid == scid)
            return &g_chans[i];
    return NULL;
}

static const l2cap_callbacks_t *server_for(uint16_t psm) {
    for (int i = 0; i < g_server_count; i++)
        if (g_servers[i].psm == psm) return g_servers[i].cb;
    return NULL;
}

static uint8_t next_id(void) {
    uint8_t id = g_next_id++;
    if (id == 0) id = g_next_id++;
    return id;
}

static uint16_t sig_cid_for(hci_handle_t h) {
    hci_conn_t *c = hci_conn_by_handle(h);
    if (c && c->type == HCI_LINK_LE) return L2CAP_CID_LE_SIGNALING;
    return L2CAP_CID_SIGNALING;
}

// Build + send an L2CAP signalling command.
static int send_sig(hci_handle_t h, uint8_t code, uint8_t id,
                    const uint8_t *data, uint16_t dlen) {
    uint8_t pdu[64];
    if ((int)dlen + 8 > (int)sizeof(pdu)) return BT_ERR_PARAM;
    wr16(pdu + 0, (uint16_t)(4 + dlen));   // L2CAP length = sig header + data
    wr16(pdu + 2, sig_cid_for(h));
    pdu[4] = code;
    pdu[5] = id;
    wr16(pdu + 6, dlen);
    if (dlen && data) memcpy(pdu + 8, data, dlen);
    return hci_send_acl(h, pdu, (uint16_t)(8 + dlen));
}

static void maybe_open(l2cap_chan_t *c) {
    int i = chan_index(c);
    if (c->state != L2CAP_OPEN && g_local_cfg_done[i] && g_remote_cfg_done[i]) {
        c->state = L2CAP_OPEN;
        kprintf("[BT-L2CAP] channel scid=0x%04x psm=0x%04x OPEN (mtu %u)\n",
                c->scid, c->psm, c->mtu);
        if (c->cb && c->cb->on_connect) c->cb->on_connect(c);
    }
}

static void send_config_req(l2cap_chan_t *c) {
    uint8_t d[8];
    wr16(d + 0, c->dcid);          // destination = remote CID
    wr16(d + 2, 0);                // flags
    d[4] = 0x01;                   // option: MTU
    d[5] = 0x02;
    wr16(d + 6, L2CAP_DEFAULT_MTU);
    send_sig(c->handle, L2CAP_SIG_CONFIG_REQ, next_id(), d, 8);
}

// ---------------------------------------------------------------------------
// Signalling handlers
// ---------------------------------------------------------------------------
static void sig_conn_req(hci_handle_t h, uint8_t id, const uint8_t *d, uint16_t len) {
    if (len < 4) return;
    uint16_t psm = rd16(d);
    uint16_t rscid = rd16(d + 2);
    const l2cap_callbacks_t *cb = server_for(psm);
    uint8_t rsp[8];
    if (!cb) {
        wr16(rsp + 0, 0);            // dcid
        wr16(rsp + 2, rscid);
        wr16(rsp + 4, 0x0002);       // result: PSM not supported
        wr16(rsp + 6, 0);
        send_sig(h, L2CAP_SIG_CONN_RSP, id, rsp, 8);
        return;
    }
    l2cap_chan_t *c = chan_alloc();
    if (!c) {
        wr16(rsp + 0, 0); wr16(rsp + 2, rscid);
        wr16(rsp + 4, 0x0004); wr16(rsp + 6, 0);   // result: no resources
        send_sig(h, L2CAP_SIG_CONN_RSP, id, rsp, 8);
        return;
    }
    c->handle = h;
    c->psm = psm;
    c->dcid = rscid;
    c->cb = cb;
    c->state = L2CAP_WAIT_CONFIG;
    wr16(rsp + 0, c->scid);          // our local CID
    wr16(rsp + 2, rscid);
    wr16(rsp + 4, 0x0000);           // success
    wr16(rsp + 6, 0x0000);
    send_sig(h, L2CAP_SIG_CONN_RSP, id, rsp, 8);
    send_config_req(c);
}

static void sig_conn_rsp(hci_handle_t h, const uint8_t *d, uint16_t len) {
    if (len < 8) return;
    uint16_t dcid = rd16(d);
    uint16_t scid = rd16(d + 2);
    uint16_t result = rd16(d + 4);
    l2cap_chan_t *c = l2cap_chan_by_scid(h, scid);
    if (!c) return;
    if (result == 0x0000) {
        c->dcid = dcid;
        c->state = L2CAP_WAIT_CONFIG;
        send_config_req(c);
    } else if (result != 0x0001) {   // not "pending"
        chan_free(c);
    }
}

static void sig_config_req(hci_handle_t h, uint8_t id, const uint8_t *d, uint16_t len) {
    if (len < 4) return;
    uint16_t dcid = rd16(d);         // our local CID
    l2cap_chan_t *c = l2cap_chan_by_scid(h, dcid);
    if (!c) return;
    // Parse options (MTU only).
    const uint8_t *o = d + 4;
    int ol = (int)len - 4;
    while (ol >= 2) {
        uint8_t type = o[0], olen = o[1];
        if (ol < 2 + (int)olen) break;
        if ((type & 0x7F) == 0x01 && olen >= 2)
            c->mtu = rd16(o + 2);    // remote's receive MTU = our send MTU
        o += 2 + olen;
        ol -= 2 + olen;
    }
    // Accept the configuration.
    uint8_t rsp[6];
    wr16(rsp + 0, c->dcid);          // remote CID
    wr16(rsp + 2, 0);                // flags
    wr16(rsp + 4, 0x0000);           // result: success
    send_sig(h, L2CAP_SIG_CONFIG_RSP, id, rsp, 6);
    g_remote_cfg_done[chan_index(c)] = 1;
    maybe_open(c);
}

static void sig_config_rsp(hci_handle_t h, const uint8_t *d, uint16_t len) {
    if (len < 6) return;
    uint16_t scid = rd16(d);         // our local CID
    uint16_t result = rd16(d + 4);
    l2cap_chan_t *c = l2cap_chan_by_scid(h, scid);
    if (!c) return;
    if (result == 0x0000) {
        g_local_cfg_done[chan_index(c)] = 1;
        maybe_open(c);
    }
}

static void sig_disc_req(hci_handle_t h, uint8_t id, const uint8_t *d, uint16_t len) {
    if (len < 4) return;
    uint16_t dcid = rd16(d);         // our local CID
    uint16_t scid = rd16(d + 2);     // remote CID
    l2cap_chan_t *c = l2cap_chan_by_scid(h, dcid);
    uint8_t rsp[4];
    wr16(rsp + 0, dcid);
    wr16(rsp + 2, scid);
    send_sig(h, L2CAP_SIG_DISCONN_RSP, id, rsp, 4);
    if (c) {
        if (c->cb && c->cb->on_disconnect) c->cb->on_disconnect(c);
        chan_free(c);
    }
}

static void sig_disc_rsp(hci_handle_t h, const uint8_t *d, uint16_t len) {
    if (len < 4) return;
    uint16_t scid = rd16(d + 2);     // our local CID
    l2cap_chan_t *c = l2cap_chan_by_scid(h, scid);
    if (c) {
        if (c->cb && c->cb->on_disconnect) c->cb->on_disconnect(c);
        chan_free(c);
    }
}

static void sig_info_req(hci_handle_t h, uint8_t id, const uint8_t *d, uint16_t len) {
    if (len < 2) return;
    uint16_t type = rd16(d);
    uint8_t rsp[8];
    wr16(rsp + 0, type);
    wr16(rsp + 2, 0x0001);           // result: not supported
    send_sig(h, L2CAP_SIG_INFO_RSP, id, rsp, 4);
}

static void process_signalling(hci_handle_t h, const uint8_t *p, uint16_t len) {
    while (len >= 4) {
        uint8_t code = p[0], id = p[1];
        uint16_t clen = rd16(p + 2);
        if (len < 4 + clen) break;
        const uint8_t *cd = p + 4;
        switch (code) {
            case L2CAP_SIG_CONN_REQ:    sig_conn_req(h, id, cd, clen); break;
            case L2CAP_SIG_CONN_RSP:    sig_conn_rsp(h, cd, clen); break;
            case L2CAP_SIG_CONFIG_REQ:  sig_config_req(h, id, cd, clen); break;
            case L2CAP_SIG_CONFIG_RSP:  sig_config_rsp(h, cd, clen); break;
            case L2CAP_SIG_DISCONN_REQ: sig_disc_req(h, id, cd, clen); break;
            case L2CAP_SIG_DISCONN_RSP: sig_disc_rsp(h, cd, clen); break;
            case L2CAP_SIG_INFO_REQ:    sig_info_req(h, id, cd, clen); break;
            default:
                kprintf("[BT-L2CAP] unhandled signal 0x%02x\n", code);
                break;
        }
        p += 4 + clen;
        len = (uint16_t)(len - (4 + clen));
    }
}

// ---------------------------------------------------------------------------
// Inbound PDU (HCI ACL sink)
// ---------------------------------------------------------------------------
void l2cap_input(hci_handle_t h, const uint8_t *pdu, uint16_t len) {
    if (len < 4) return;
    uint16_t l2len = rd16(pdu);
    uint16_t cid = rd16(pdu + 2);
    const uint8_t *payload = pdu + 4;
    uint16_t paylen = (uint16_t)(len - 4);
    if (l2len < paylen) paylen = l2len;

    if (cid == L2CAP_CID_SIGNALING || cid == L2CAP_CID_LE_SIGNALING) {
        process_signalling(h, payload, paylen);
        return;
    }
    if (cid == L2CAP_CID_LE_ATT) {
        extern void gatt_att_input(hci_handle_t h, const uint8_t *data, uint16_t len);
        gatt_att_input(h, payload, paylen);
        return;
    }
    if (cid == L2CAP_CID_LE_SMP) {
        extern void smp_input(hci_handle_t h, const uint8_t *data, uint16_t len);
        smp_input(h, payload, paylen);
        return;
    }
    l2cap_chan_t *c = l2cap_chan_by_scid(h, cid);
    if (c && c->state == L2CAP_OPEN && c->cb && c->cb->on_data)
        c->cb->on_data(c, payload, paylen);
    else if (!c)
        kprintf("[BT-L2CAP] data for unknown cid 0x%04x\n", cid);
}

// ---------------------------------------------------------------------------
// Connection observer: tear down channels when a link drops.
// ---------------------------------------------------------------------------
static void l2cap_conn_event(const hci_conn_t *c, int up) {
    if (up) return;
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        if (g_chans[i].active && g_chans[i].handle == c->handle) {
            if (g_chans[i].cb && g_chans[i].cb->on_disconnect)
                g_chans[i].cb->on_disconnect(&g_chans[i]);
            chan_free(&g_chans[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
int l2cap_register_server(uint16_t psm, const l2cap_callbacks_t *cb) {
    if (!cb || g_server_count >= (int)(sizeof(g_servers)/sizeof(g_servers[0])))
        return BT_ERR_NOMEM;
    g_servers[g_server_count].psm = psm;
    g_servers[g_server_count].cb  = cb;
    g_server_count++;
    kprintf("[BT-L2CAP] server registered PSM 0x%04x\n", psm);
    return BT_OK;
}

l2cap_chan_t *l2cap_connect(hci_handle_t h, uint16_t psm,
                            const l2cap_callbacks_t *cb, void *user) {
    l2cap_chan_t *c = chan_alloc();
    if (!c) return NULL;
    c->handle = h;
    c->psm = psm;
    c->cb = cb;
    c->user = user;
    c->state = L2CAP_WAIT_CONNECT;
    uint8_t d[4];
    wr16(d + 0, psm);
    wr16(d + 2, c->scid);
    send_sig(h, L2CAP_SIG_CONN_REQ, next_id(), d, 4);
    return c;
}

int l2cap_send(l2cap_chan_t *c, const uint8_t *data, uint16_t len) {
    if (!c || c->state != L2CAP_OPEN) return BT_ERR_STATE;
    if (len > c->mtu) return BT_ERR_PARAM;
    uint8_t pdu[L2CAP_DEFAULT_MTU + 8];
    if ((int)len + 4 > (int)sizeof(pdu)) return BT_ERR_PARAM;
    wr16(pdu + 0, len);
    wr16(pdu + 2, c->dcid);
    memcpy(pdu + 4, data, len);
    return hci_send_acl(c->handle, pdu, (uint16_t)(4 + len));
}

int l2cap_send_fixed(hci_handle_t h, uint16_t cid, const uint8_t *data, uint16_t len) {
    uint8_t pdu[L2CAP_DEFAULT_MTU + 8];
    if ((int)len + 4 > (int)sizeof(pdu)) return BT_ERR_PARAM;
    wr16(pdu + 0, len);
    wr16(pdu + 2, cid);
    if (len && data) memcpy(pdu + 4, data, len);
    return hci_send_acl(h, pdu, (uint16_t)(4 + len));
}

int l2cap_disconnect(l2cap_chan_t *c) {
    if (!c || !c->active) return BT_ERR_STATE;
    if (c->state == L2CAP_OPEN || c->state == L2CAP_WAIT_CONFIG) {
        uint8_t d[4];
        wr16(d + 0, c->dcid);
        wr16(d + 2, c->scid);
        send_sig(c->handle, L2CAP_SIG_DISCONN_REQ, next_id(), d, 4);
        c->state = L2CAP_WAIT_DISCONNECT;
    }
    return BT_OK;
}

void l2cap_poll(void) { /* no timers yet */ }

int l2cap_init(void) {
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) g_chans[i].active = 0;
    g_next_scid = L2CAP_CID_DYN_BASE;
    g_next_id = 1;
    hci_set_acl_sink(l2cap_input);
    hci_add_conn_observer(l2cap_conn_event);
    kprintf("[BT-L2CAP] init: ACL sink + conn observer registered\n");
    return BT_OK;
}
