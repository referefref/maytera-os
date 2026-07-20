// hci_defs.h - Bluetooth HCI wire definitions (#372)
//
// Opcodes, event codes, packet headers and error codes. Pure constants and
// packed structs, no functions. Shared by hci.c, l2cap.c, pair.c and the
// transport driver. Bluetooth Core spec v5.x, little-endian on the wire.
#ifndef HCI_DEFS_H
#define HCI_DEFS_H

#include "../types.h"

// -----------------------------------------------------------------------------
// Opcode = (OGF << 10) | OCF. OGF = opcode group field.
// -----------------------------------------------------------------------------
#define HCI_OGF_LINK_CTL       0x01
#define HCI_OGF_LINK_POLICY    0x02
#define HCI_OGF_CTRL_BASEBAND  0x03
#define HCI_OGF_INFO_PARAMS    0x04
#define HCI_OGF_STATUS         0x05
#define HCI_OGF_LE_CTL         0x08
#define HCI_OGF_VENDOR         0x3F

#define HCI_OPCODE(ogf, ocf)   ((uint16_t)(((ogf) << 10) | ((ocf) & 0x03FF)))
#define HCI_OPCODE_OGF(op)     ((uint8_t)((op) >> 10))
#define HCI_OPCODE_OCF(op)     ((uint16_t)((op) & 0x03FF))

// --- Link control (BR/EDR) ---
#define HCI_CMD_INQUIRY              HCI_OPCODE(0x01, 0x0001)
#define HCI_CMD_INQUIRY_CANCEL       HCI_OPCODE(0x01, 0x0002)
#define HCI_CMD_CREATE_CONNECTION    HCI_OPCODE(0x01, 0x0005)
#define HCI_CMD_DISCONNECT           HCI_OPCODE(0x01, 0x0006)
#define HCI_CMD_ACCEPT_CONN_REQ      HCI_OPCODE(0x01, 0x0009)
#define HCI_CMD_REJECT_CONN_REQ      HCI_OPCODE(0x01, 0x000A)
#define HCI_CMD_LINK_KEY_REPLY       HCI_OPCODE(0x01, 0x000B)
#define HCI_CMD_LINK_KEY_NEG_REPLY   HCI_OPCODE(0x01, 0x000C)
#define HCI_CMD_PIN_CODE_REPLY       HCI_OPCODE(0x01, 0x000D)
#define HCI_CMD_AUTH_REQUESTED       HCI_OPCODE(0x01, 0x0011)
#define HCI_CMD_SET_CONN_ENCRYPTION  HCI_OPCODE(0x01, 0x0013)
#define HCI_CMD_REMOTE_NAME_REQ      HCI_OPCODE(0x01, 0x0019)
#define HCI_CMD_IO_CAP_REQ_REPLY     HCI_OPCODE(0x01, 0x002B)
#define HCI_CMD_USER_CONFIRM_REPLY   HCI_OPCODE(0x01, 0x002C)
#define HCI_CMD_USER_CONFIRM_NEG     HCI_OPCODE(0x01, 0x002D)
#define HCI_CMD_USER_PASSKEY_REPLY   HCI_OPCODE(0x01, 0x002E)

// --- Controller & baseband ---
#define HCI_CMD_SET_EVENT_MASK       HCI_OPCODE(0x03, 0x0001)
#define HCI_CMD_RESET                HCI_OPCODE(0x03, 0x0003)
#define HCI_CMD_WRITE_LOCAL_NAME     HCI_OPCODE(0x03, 0x0013)
#define HCI_CMD_WRITE_SCAN_ENABLE    HCI_OPCODE(0x03, 0x001A)
#define HCI_CMD_WRITE_CLASS_OF_DEV   HCI_OPCODE(0x03, 0x0024)
#define HCI_CMD_WRITE_SSP_MODE       HCI_OPCODE(0x03, 0x0056)
#define HCI_CMD_LE_SET_EVENT_MASK    HCI_OPCODE(0x08, 0x0001)

// --- Informational ---
#define HCI_CMD_READ_LOCAL_VERSION   HCI_OPCODE(0x04, 0x0001)
#define HCI_CMD_READ_BUFFER_SIZE     HCI_OPCODE(0x04, 0x0005)
#define HCI_CMD_READ_BD_ADDR         HCI_OPCODE(0x04, 0x0009)

// --- Controller & baseband (LE host support) ---
#define HCI_CMD_WRITE_LE_HOST_SUPPORT HCI_OPCODE(0x03, 0x006D)

// --- LE ---
#define HCI_CMD_LE_READ_BUFFER_SIZE  HCI_OPCODE(0x08, 0x0002)
#define HCI_CMD_LE_SET_SCAN_PARAMS   HCI_OPCODE(0x08, 0x000B)
#define HCI_CMD_LE_SET_SCAN_ENABLE   HCI_OPCODE(0x08, 0x000C)
#define HCI_CMD_LE_CREATE_CONNECTION HCI_OPCODE(0x08, 0x000D)
#define HCI_CMD_LE_CONN_CANCEL       HCI_OPCODE(0x08, 0x000E)
#define HCI_CMD_LE_SET_ADV_PARAMS    HCI_OPCODE(0x08, 0x0006)
#define HCI_CMD_LE_SET_ADV_ENABLE    HCI_OPCODE(0x08, 0x000A)
#define HCI_CMD_LE_START_ENCRYPTION  HCI_OPCODE(0x08, 0x0019)
#define HCI_CMD_LE_LTK_REQ_REPLY     HCI_OPCODE(0x08, 0x001A)
#define HCI_CMD_LE_LTK_REQ_NEG_REPLY HCI_OPCODE(0x08, 0x001B)

// -----------------------------------------------------------------------------
// Event codes.
// -----------------------------------------------------------------------------
#define HCI_EVT_INQUIRY_COMPLETE          0x01
#define HCI_EVT_INQUIRY_RESULT            0x02
#define HCI_EVT_CONN_COMPLETE             0x03
#define HCI_EVT_CONN_REQUEST              0x04
#define HCI_EVT_DISCONN_COMPLETE          0x05
#define HCI_EVT_AUTH_COMPLETE             0x06
#define HCI_EVT_REMOTE_NAME_REQ_COMPLETE  0x07
#define HCI_EVT_ENCRYPT_CHANGE            0x08
#define HCI_EVT_CMD_COMPLETE              0x0E
#define HCI_EVT_CMD_STATUS                0x0F
#define HCI_EVT_HARDWARE_ERROR            0x10
#define HCI_EVT_NUM_COMPLETED_PKTS        0x13
#define HCI_EVT_LINK_KEY_REQUEST          0x17
#define HCI_EVT_LINK_KEY_NOTIFICATION     0x18
#define HCI_EVT_PIN_CODE_REQUEST          0x16
#define HCI_EVT_INQUIRY_RESULT_RSSI       0x22
#define HCI_EVT_EXT_INQUIRY_RESULT        0x2F
#define HCI_EVT_IO_CAP_REQUEST            0x31
#define HCI_EVT_IO_CAP_RESPONSE           0x32
#define HCI_EVT_USER_CONFIRM_REQUEST      0x33
#define HCI_EVT_USER_PASSKEY_REQUEST      0x34
#define HCI_EVT_SIMPLE_PAIRING_COMPLETE   0x36
#define HCI_EVT_LE_META                   0x3E

// LE meta subevent codes (first byte of the LE meta event payload).
#define HCI_LE_SUBEVT_CONN_COMPLETE       0x01
#define HCI_LE_SUBEVT_ADV_REPORT          0x02
#define HCI_LE_SUBEVT_CONN_UPDATE         0x03
#define HCI_LE_SUBEVT_LTK_REQUEST         0x05

// -----------------------------------------------------------------------------
// Packet headers (all little-endian, packed).
// -----------------------------------------------------------------------------
typedef struct {
    uint16_t opcode;
    uint8_t  plen;      // parameter length
} __attribute__((packed)) hci_cmd_hdr_t;

typedef struct {
    uint8_t  evt;
    uint8_t  plen;
} __attribute__((packed)) hci_evt_hdr_t;

// ACL header: bits [11:0] handle, [13:12] PB flag, [15:14] BC flag; then dlen.
typedef struct {
    uint16_t handle_flags;
    uint16_t dlen;
} __attribute__((packed)) hci_acl_hdr_t;

#define HCI_ACL_HANDLE(hf)       ((uint16_t)((hf) & 0x0FFF))
#define HCI_ACL_PB(hf)           ((uint8_t)(((hf) >> 12) & 0x3))
#define HCI_ACL_BC(hf)           ((uint8_t)(((hf) >> 14) & 0x3))
#define HCI_ACL_MK(handle,pb,bc) ((uint16_t)(((handle) & 0x0FFF) | \
                                  (((pb) & 0x3) << 12) | (((bc) & 0x3) << 14)))
#define HCI_PB_FIRST_NONFLUSH    0x00   // start of L2CAP PDU, non-flushable
#define HCI_PB_CONTINUING        0x01   // continuation fragment
#define HCI_PB_FIRST_FLUSH       0x02   // start of L2CAP PDU, flushable

// -----------------------------------------------------------------------------
// HCI status / error codes (subset).
// -----------------------------------------------------------------------------
#define HCI_SUCCESS                 0x00
#define HCI_ERR_UNKNOWN_CMD         0x01
#define HCI_ERR_NO_CONNECTION       0x02
#define HCI_ERR_PAGE_TIMEOUT        0x04
#define HCI_ERR_AUTH_FAILURE        0x05
#define HCI_ERR_PIN_MISSING         0x06
#define HCI_ERR_MEM_CAPACITY        0x07
#define HCI_ERR_CONN_TIMEOUT        0x08
#define HCI_ERR_CONN_LIMIT          0x09
#define HCI_ERR_ACL_EXISTS          0x0B
#define HCI_ERR_CMD_DISALLOWED      0x0C
#define HCI_ERR_REMOTE_USER_TERM    0x13
#define HCI_ERR_LOCAL_HOST_TERM     0x16
#define HCI_ERR_PAIRING_NOT_ALLOWED 0x18
#define HCI_ERR_UNSUPPORTED         0x1A

// Scan-enable bit values for HCI_CMD_WRITE_SCAN_ENABLE.
#define HCI_SCAN_DISABLED           0x00
#define HCI_SCAN_INQUIRY            0x01
#define HCI_SCAN_PAGE               0x02
#define HCI_SCAN_BOTH               0x03

// Sizing guidance for buffers.
#define HCI_MAX_CMD_PARAM_LEN       255
#define HCI_MAX_EVENT_LEN           257
#define HCI_MAX_ACL_LEN             1024

#endif // HCI_DEFS_H
