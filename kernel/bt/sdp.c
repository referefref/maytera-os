// sdp.c - Service Discovery Protocol (#372, PROTOCOL agent).
//
// CLIENT: query a remote HID device's service record (needed only when WE
// initiate a connection and must learn the device's L2CAP PSMs / HID report
// descriptor). The common phase-1 flow is device-initiated reconnect, where the
// keyboard opens the HID PSMs to us directly and no SDP is required; so this
// client is provided as best-effort scaffolding (request built + response
// reassembled + a pragmatic PSM/descriptor scan) and is not yet exercised
// end-to-end.
//
// SERVER: publishing MayteraOS's own record is phase 3 (stubbed).
#include "sdp.h"
#include "l2cap.h"
#include "../serial.h"
#include "../string.h"

// ---------------------------------------------------------------------------
// One in-flight client query at a time.
// ---------------------------------------------------------------------------
static struct {
    int              active;
    hci_handle_t     handle;
    sdp_hid_result_t cb;
    l2cap_chan_t    *chan;
    uint8_t          rx[1024];
    uint16_t         rxlen;
} q;

static void wr16be(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v & 0xFF); }
static uint16_t rd16be(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void finish(int ok, uint16_t pc, uint16_t pi, const uint8_t *desc, uint16_t dlen) {
    sdp_hid_result_t cb = q.cb;
    hci_handle_t h = q.handle;
    if (q.chan) l2cap_disconnect(q.chan);
    q.active = 0;
    q.chan = NULL;
    q.cb = NULL;
    if (cb) cb(h, pc, pi, desc, dlen, ok);
}

// Best-effort scan of an SDP attribute-list blob for the two HID L2CAP PSMs and
// the HID report descriptor. Not a full data-element parser.
static void parse_response(const uint8_t *d, uint16_t n) {
    uint16_t psm_ctrl = L2CAP_PSM_HID_CONTROL;    // sensible defaults
    uint16_t psm_intr = L2CAP_PSM_HID_INTERRUPT;
    const uint8_t *desc = NULL;
    uint16_t desc_len = 0;

    for (uint16_t i = 0; i + 5 < n; i++) {
        // L2CAP protocol UUID (0x0100) as UUID16 -> next uint16 element is a PSM.
        if (d[i] == 0x19 && d[i+1] == 0x01 && d[i+2] == 0x00) {
            if (d[i+3] == 0x09) {                 // uint16 element
                uint16_t psm = rd16be(&d[i+4]);
                if (psm & 1) { psm_ctrl = psm; psm_intr = (uint16_t)(psm + 2); }
            }
        }
        // HID descriptor list often carries the report map as a string element
        // (0x25 = string, 1-byte length) tagged after attribute id 0x0206.
        if (d[i] == 0x25 && !desc) {
            uint8_t l = d[i+1];
            if (l > 8 && i + 2 + l <= n) { desc = &d[i+2]; desc_len = l; }
        }
    }
    finish(1, psm_ctrl, psm_intr, desc, desc_len);
}

// ---------------------------------------------------------------------------
// Client L2CAP callbacks
// ---------------------------------------------------------------------------
static void sdp_cli_connect(l2cap_chan_t *c) {
    // ServiceSearchAttributeRequest for the HID service (UUID 0x1124),
    // requesting all attributes (range 0x0000-0xFFFF).
    uint8_t req[32];
    int o = 0;
    req[o++] = SDP_PDU_SVC_SEARCH_ATTR_REQ;
    wr16be(&req[o], 0x0001); o += 2;              // transaction id
    int lenpos = o; o += 2;                        // param length (fill later)
    // ServiceSearchPattern: seq { UUID16 0x1124 }
    req[o++] = 0x35; req[o++] = 0x03;
    req[o++] = 0x19; req[o++] = 0x11; req[o++] = 0x24;
    // MaximumAttributeByteCount
    wr16be(&req[o], 0x03F0); o += 2;
    // AttributeIDList: seq { uint32 range 0x0000FFFF }
    req[o++] = 0x35; req[o++] = 0x05;
    req[o++] = 0x0A; req[o++] = 0x00; req[o++] = 0x00; req[o++] = 0xFF; req[o++] = 0xFF;
    // ContinuationState
    req[o++] = 0x00;
    wr16be(&req[lenpos], (uint16_t)(o - lenpos - 2));
    q.rxlen = 0;
    l2cap_send(c, req, (uint16_t)o);
}

static void sdp_cli_data(l2cap_chan_t *c, const uint8_t *d, uint16_t n) {
    (void)c;
    if (n < 5 || d[0] != SDP_PDU_SVC_SEARCH_ATTR_RSP) { finish(0, 0, 0, NULL, 0); return; }
    uint16_t alen = rd16be(&d[5]);                 // AttributeListsByteCount
    const uint8_t *lists = d + 7;
    uint16_t avail = (uint16_t)(n - 7);
    if (alen > avail) alen = avail;
    if (q.rxlen + alen > sizeof(q.rx)) alen = (uint16_t)(sizeof(q.rx) - q.rxlen);
    memcpy(q.rx + q.rxlen, lists, alen);
    q.rxlen = (uint16_t)(q.rxlen + alen);
    // ContinuationState is the trailing byte(s); if zero we have the whole record.
    uint8_t cont_len = d[n - 1];
    if (cont_len == 0) parse_response(q.rx, q.rxlen);
    // (Multi-fragment continuation not chased in phase 1; single response only.)
}

static void sdp_cli_disc(l2cap_chan_t *c) {
    (void)c;
    if (q.active) finish(0, 0, 0, NULL, 0);
}

static const l2cap_callbacks_t sdp_cli_cb = { sdp_cli_connect, sdp_cli_data, sdp_cli_disc };

// ---------------------------------------------------------------------------
// Server callbacks (phase 3 stub)
// ---------------------------------------------------------------------------
static void sdp_srv_connect(l2cap_chan_t *c)   { (void)c; }
static void sdp_srv_data(l2cap_chan_t *c, const uint8_t *d, uint16_t n) { (void)c; (void)d; (void)n; }
static void sdp_srv_disc(l2cap_chan_t *c)      { (void)c; }
static const l2cap_callbacks_t sdp_srv_cb = { sdp_srv_connect, sdp_srv_data, sdp_srv_disc };

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
int sdp_query_hid(hci_handle_t h, sdp_hid_result_t cb) {
    if (q.active) return BT_ERR_STATE;
    q.active = 1;
    q.handle = h;
    q.cb = cb;
    q.rxlen = 0;
    q.chan = l2cap_connect(h, L2CAP_PSM_SDP, &sdp_cli_cb, NULL);
    if (!q.chan) { q.active = 0; return BT_ERR_NOMEM; }
    return BT_OK;
}

void sdp_poll(void) { /* no client timeout yet */ }

int sdp_init(void) {
    memset(&q, 0, sizeof(q));
    l2cap_register_server(L2CAP_PSM_SDP, &sdp_srv_cb);
    kprintf("[BT-SDP] init: SDP server registered (client best-effort)\n");
    return BT_OK;
}
