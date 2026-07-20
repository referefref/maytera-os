// sdp.h - Service Discovery Protocol API (#372)
//
// Owned by the PROTOCOL agent (sdp.c). Runs over L2CAP PSM 0x0001. Two roles:
//   - CLIENT: query a remote HID device's service record to learn its L2CAP
//     PSMs and pull its HID report-descriptor blob (needed for classic HIDP).
//   - SERVER: publish a minimal record so a host can discover MayteraOS
//     (phase 3; the server callbacks are registered internally by sdp_init()).
#ifndef SDP_H
#define SDP_H

#include "../types.h"
#include "bt.h"
#include "l2cap.h"

// --- SDP PDU IDs ---
#define SDP_PDU_ERROR_RSP             0x01
#define SDP_PDU_SVC_SEARCH_REQ        0x02
#define SDP_PDU_SVC_SEARCH_RSP        0x03
#define SDP_PDU_SVC_ATTR_REQ          0x04
#define SDP_PDU_SVC_ATTR_RSP          0x05
#define SDP_PDU_SVC_SEARCH_ATTR_REQ   0x06
#define SDP_PDU_SVC_SEARCH_ATTR_RSP   0x07

// --- Data element type descriptors (top 5 bits) ---
#define SDP_DE_NIL     0x00
#define SDP_DE_UINT    0x08
#define SDP_DE_INT     0x10
#define SDP_DE_UUID    0x18
#define SDP_DE_STRING  0x20
#define SDP_DE_BOOL    0x28
#define SDP_DE_SEQ     0x30
#define SDP_DE_ALT     0x38
#define SDP_DE_URL     0x40

// --- Common 16-bit UUIDs ---
#define SDP_UUID_SDP           0x0001
#define SDP_UUID_L2CAP         0x0100
#define SDP_UUID_HIDP          0x0011
#define SDP_UUID_HID_SERVICE   0x1124   // Human Interface Device service class

// --- Common attribute IDs ---
#define SDP_ATTR_SVC_RECORD_HANDLE   0x0000
#define SDP_ATTR_SVC_CLASS_ID_LIST   0x0001
#define SDP_ATTR_PROTOCOL_DESC_LIST  0x0004
#define SDP_ATTR_HID_DESCRIPTOR_LIST 0x0206

int  sdp_init(void);
void sdp_poll(void);

// -----------------------------------------------------------------------------
// CLIENT: query a remote's HID service record over an existing ACL link. The
// result callback delivers the two HID L2CAP PSMs (control/interrupt, usually
// 0x0011/0x0013) plus the raw HID report descriptor. ok==0 on failure.
// -----------------------------------------------------------------------------
typedef void (*sdp_hid_result_t)(hci_handle_t h,
                                 uint16_t psm_ctrl, uint16_t psm_intr,
                                 const uint8_t *hid_desc, uint16_t desc_len,
                                 int ok);
int  sdp_query_hid(hci_handle_t h, sdp_hid_result_t cb);

#endif // SDP_H
