// l2cap.h - L2CAP channel API (#372)
//
// Owned by the PROTOCOL agent (l2cap.c). Provides connection-oriented channels
// (BR/EDR dynamic PSM) and fixed channels (LE ATT/SMP/signalling). Profiles
// (SDP, HID) sit on top and register servers / open channels.
#ifndef L2CAP_H
#define L2CAP_H

#include "../types.h"
#include "bt.h"
#include "hci.h"

// --- Well-known BR/EDR PSMs ---
#define L2CAP_PSM_SDP            0x0001
#define L2CAP_PSM_RFCOMM         0x0003
#define L2CAP_PSM_HID_CONTROL    0x0011
#define L2CAP_PSM_HID_INTERRUPT  0x0013

// --- Fixed channel IDs ---
#define L2CAP_CID_NULL           0x0000
#define L2CAP_CID_SIGNALING      0x0001   // BR/EDR signalling
#define L2CAP_CID_CONNLESS       0x0002
#define L2CAP_CID_LE_ATT         0x0004   // ATT (GATT/HOGP) over LE
#define L2CAP_CID_LE_SIGNALING   0x0005
#define L2CAP_CID_LE_SMP         0x0006   // Security Manager over LE
#define L2CAP_CID_DYN_BASE       0x0040   // first dynamically-allocated CID

#define L2CAP_DEFAULT_MTU        672
#define L2CAP_MAX_CHANNELS       16

// --- L2CAP signalling command codes ---
#define L2CAP_SIG_CMD_REJECT     0x01
#define L2CAP_SIG_CONN_REQ       0x02
#define L2CAP_SIG_CONN_RSP       0x03
#define L2CAP_SIG_CONFIG_REQ     0x04
#define L2CAP_SIG_CONFIG_RSP     0x05
#define L2CAP_SIG_DISCONN_REQ    0x06
#define L2CAP_SIG_DISCONN_RSP    0x07
#define L2CAP_SIG_INFO_REQ       0x0A
#define L2CAP_SIG_INFO_RSP       0x0B

typedef uint16_t l2cap_cid_t;

typedef struct l2cap_chan l2cap_chan_t;

// Channel state machine (values used internally by l2cap.c; exposed for debug).
typedef enum {
    L2CAP_CLOSED = 0,
    L2CAP_WAIT_CONNECT,
    L2CAP_WAIT_CONFIG,
    L2CAP_OPEN,
    L2CAP_WAIT_DISCONNECT,
} l2cap_state_t;

// Per-channel callbacks supplied by a profile (SDP / HID / SMP).
typedef struct {
    void (*on_connect)(l2cap_chan_t *c);                              // config done, channel OPEN
    void (*on_data)(l2cap_chan_t *c, const uint8_t *data, uint16_t len);
    void (*on_disconnect)(l2cap_chan_t *c);
} l2cap_callbacks_t;

struct l2cap_chan {
    hci_handle_t              handle;   // owning ACL/LE link
    l2cap_cid_t               scid;     // local (source) CID
    l2cap_cid_t               dcid;     // remote (destination) CID
    uint16_t                  psm;      // 0 for fixed channels
    uint16_t                  mtu;      // negotiated outbound MTU
    l2cap_state_t             state;
    uint8_t                   active;
    const l2cap_callbacks_t  *cb;
    void                     *user;     // profile-private pointer
};

// --- Lifecycle ---
int  l2cap_init(void);   // registers itself as HCI ACL sink + conn observer
void l2cap_poll(void);

// --- Server registration: incoming connects to `psm` create a channel + fire cb.
int  l2cap_register_server(uint16_t psm, const l2cap_callbacks_t *cb);

// --- Outbound connect on an existing link. Returns a channel (state != OPEN
//     until on_connect fires) or NULL.
l2cap_chan_t *l2cap_connect(hci_handle_t h, uint16_t psm,
                            const l2cap_callbacks_t *cb, void *user);

// --- Data ---
int  l2cap_send(l2cap_chan_t *c, const uint8_t *data, uint16_t len);
int  l2cap_disconnect(l2cap_chan_t *c);

// --- Fixed-channel send (ATT 0x0004 / SMP 0x0006 / signalling). No channel
//     object needed; builds the L2CAP basic header and hands to HCI. ---
int  l2cap_send_fixed(hci_handle_t h, uint16_t cid, const uint8_t *data, uint16_t len);

// --- Lookup ---
l2cap_chan_t *l2cap_chan_by_scid(hci_handle_t h, l2cap_cid_t scid);

// --- Inbound PDU from HCI (registered as hci_acl_sink_t) ---
void l2cap_input(hci_handle_t h, const uint8_t *pdu, uint16_t len);

#endif // L2CAP_H
