// bt_l2cap.h - Bluetooth Logical Link Control and Adaptation Protocol (L2CAP)
#ifndef BT_L2CAP_H
#define BT_L2CAP_H

#include "bluetooth.h"

// ============================================================================
// L2CAP Constants
// ============================================================================

// Fixed channel IDs
#define L2CAP_CID_NULL          0x0000  // Invalid
#define L2CAP_CID_SIGNALING     0x0001  // Signaling channel
#define L2CAP_CID_CONNLESS      0x0002  // Connectionless reception
#define L2CAP_CID_AMP_MANAGER   0x0003  // AMP Manager
#define L2CAP_CID_ATT           0x0004  // Attribute Protocol (LE)
#define L2CAP_CID_LE_SIGNALING  0x0005  // LE Signaling
#define L2CAP_CID_SMP           0x0006  // Security Manager (LE)
#define L2CAP_CID_BR_EDR_SM     0x0007  // BR/EDR Security Manager

// Dynamic CID range
#define L2CAP_CID_DYN_START     0x0040
#define L2CAP_CID_DYN_END       0xFFFF

// L2CAP MTU
#define L2CAP_MIN_MTU           48
#define L2CAP_DEFAULT_MTU       672
#define L2CAP_MAX_MTU           1024

// L2CAP Signaling Command Codes
#define L2CAP_CMD_REJECT            0x01
#define L2CAP_CMD_CONN_REQ          0x02
#define L2CAP_CMD_CONN_RSP          0x03
#define L2CAP_CMD_CONFIG_REQ        0x04
#define L2CAP_CMD_CONFIG_RSP        0x05
#define L2CAP_CMD_DISC_REQ          0x06
#define L2CAP_CMD_DISC_RSP          0x07
#define L2CAP_CMD_ECHO_REQ          0x08
#define L2CAP_CMD_ECHO_RSP          0x09
#define L2CAP_CMD_INFO_REQ          0x0A
#define L2CAP_CMD_INFO_RSP          0x0B

// Connection Response Results
#define L2CAP_CONN_SUCCESS          0x0000
#define L2CAP_CONN_PENDING          0x0001
#define L2CAP_CONN_REFUSED_PSM      0x0002
#define L2CAP_CONN_REFUSED_SECURITY 0x0003
#define L2CAP_CONN_REFUSED_RESOURCE 0x0004

// Connection Response Status (when result = pending)
#define L2CAP_CONN_STATUS_NONE          0x0000
#define L2CAP_CONN_STATUS_AUTH_PENDING  0x0001
#define L2CAP_CONN_STATUS_AUTH_PENDING2 0x0002

// Configuration Response Results
#define L2CAP_CONF_SUCCESS          0x0000
#define L2CAP_CONF_UNACCEPTABLE     0x0001
#define L2CAP_CONF_REJECTED         0x0002
#define L2CAP_CONF_UNKNOWN_OPTION   0x0003
#define L2CAP_CONF_PENDING          0x0004
#define L2CAP_CONF_FLOW_SPEC_REJ    0x0005

// Configuration Option Types
#define L2CAP_CONF_OPT_MTU          0x01
#define L2CAP_CONF_OPT_FLUSH_TO     0x02
#define L2CAP_CONF_OPT_QOS          0x03
#define L2CAP_CONF_OPT_RFC          0x04  // Retransmission & Flow Control

// Protocol/Service Multiplexer (PSM) values
#define L2CAP_PSM_SDP               0x0001  // Service Discovery Protocol
#define L2CAP_PSM_RFCOMM            0x0003  // RFCOMM
#define L2CAP_PSM_TCS_BIN           0x0005  // TCS Binary
#define L2CAP_PSM_TCS_BIN_CORDLESS  0x0007  // TCS Binary Cordless
#define L2CAP_PSM_BNEP              0x000F  // Bluetooth Network Encapsulation
#define L2CAP_PSM_HID_CTRL          0x0011  // HID Control
#define L2CAP_PSM_HID_INTR          0x0013  // HID Interrupt
#define L2CAP_PSM_UPNP              0x0015  // UPnP/ESDP
#define L2CAP_PSM_AVCTP             0x0017  // AVCTP
#define L2CAP_PSM_AVDTP             0x0019  // AVDTP
#define L2CAP_PSM_AVCTP_BROWSE      0x001B  // AVCTP Browsing
#define L2CAP_PSM_ATT               0x001F  // ATT

// ============================================================================
// L2CAP Packet Structures
// ============================================================================

// L2CAP Basic header (4 bytes)
typedef struct {
    uint16_t length;    // Payload length
    uint16_t cid;       // Channel ID
    // Payload follows
} __attribute__((packed)) l2cap_header_t;

// L2CAP Signaling Command header
typedef struct {
    uint8_t  code;
    uint8_t  identifier;
    uint16_t length;
    // Data follows
} __attribute__((packed)) l2cap_sig_header_t;

// Connection Request
typedef struct {
    uint16_t psm;
    uint16_t scid;  // Source CID
} __attribute__((packed)) l2cap_conn_req_t;

// Connection Response
typedef struct {
    uint16_t dcid;      // Destination CID
    uint16_t scid;      // Source CID
    uint16_t result;
    uint16_t status;
} __attribute__((packed)) l2cap_conn_rsp_t;

// Configuration Request
typedef struct {
    uint16_t dcid;
    uint16_t flags;
    // Options follow
} __attribute__((packed)) l2cap_config_req_t;

// Configuration Response
typedef struct {
    uint16_t scid;
    uint16_t flags;
    uint16_t result;
    // Options follow
} __attribute__((packed)) l2cap_config_rsp_t;

// Disconnection Request
typedef struct {
    uint16_t dcid;
    uint16_t scid;
} __attribute__((packed)) l2cap_disc_req_t;

// Disconnection Response
typedef struct {
    uint16_t dcid;
    uint16_t scid;
} __attribute__((packed)) l2cap_disc_rsp_t;

// Configuration Option header
typedef struct {
    uint8_t type;
    uint8_t length;
    // Value follows
} __attribute__((packed)) l2cap_conf_opt_t;

// ============================================================================
// L2CAP Channel State
// ============================================================================

typedef enum {
    L2CAP_STATE_CLOSED = 0,
    L2CAP_STATE_WAIT_CONNECT,       // Waiting for connection response
    L2CAP_STATE_WAIT_CONNECT_RSP,   // Connection request sent
    L2CAP_STATE_CONFIG,             // Configuration in progress
    L2CAP_STATE_OPEN,               // Channel open
    L2CAP_STATE_WAIT_DISCONNECT,    // Waiting for disconnect response
} l2cap_state_t;

// ============================================================================
// L2CAP Channel
// ============================================================================

#define L2CAP_MAX_CHANNELS  16

typedef struct {
    int             active;
    l2cap_state_t   state;

    uint16_t        local_cid;      // Local channel ID
    uint16_t        remote_cid;     // Remote channel ID
    uint16_t        psm;            // Protocol Service Multiplexer
    uint16_t        handle;         // HCI connection handle

    uint16_t        local_mtu;
    uint16_t        remote_mtu;

    // Configuration flags
    int             local_config_done;
    int             remote_config_done;

    // Callback for received data
    void (*recv_callback)(uint16_t cid, const void *data, uint16_t len);
} l2cap_channel_t;

// ============================================================================
// L2CAP API
// ============================================================================

// Initialize L2CAP layer
int bt_l2cap_init(void);

// Shutdown L2CAP layer
void bt_l2cap_shutdown(void);

// Process incoming ACL data (called by HCI)
void bt_l2cap_process_acl(uint16_t handle, const uint8_t *data, uint16_t len);

// Create L2CAP connection to remote PSM
// Returns local CID on success, negative on error
int bt_l2cap_connect(uint16_t handle, uint16_t psm);

// Disconnect L2CAP channel
int bt_l2cap_disconnect(uint16_t cid);

// Send data on L2CAP channel
int bt_l2cap_send(uint16_t cid, const void *data, uint16_t len);

// Register PSM handler
// callback receives (cid, data, length) when data arrives on this PSM
typedef void (*l2cap_recv_callback_t)(uint16_t cid, const void *data, uint16_t len);
int bt_l2cap_register_psm(uint16_t psm, l2cap_recv_callback_t callback);

// Unregister PSM handler
void bt_l2cap_unregister_psm(uint16_t psm);

// Get channel by CID
l2cap_channel_t *bt_l2cap_get_channel(uint16_t cid);

// Get channel state
l2cap_state_t bt_l2cap_get_state(uint16_t cid);

// Print L2CAP status
void bt_l2cap_print_status(void);

#endif // BT_L2CAP_H
