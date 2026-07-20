// bt_hci.h - Bluetooth Host Controller Interface (HCI) Layer
#ifndef BT_HCI_H
#define BT_HCI_H

#include "bluetooth.h"

// ============================================================================
// HCI Packet Types
// ============================================================================

#define HCI_COMMAND_PKT     0x01    // Host -> Controller
#define HCI_ACL_DATA_PKT    0x02    // Bidirectional
#define HCI_SCO_DATA_PKT    0x03    // Bidirectional (voice)
#define HCI_EVENT_PKT       0x04    // Controller -> Host

// ============================================================================
// HCI Command OpCode Groups (OGF)
// ============================================================================

#define HCI_OGF_LINK_CONTROL        0x01
#define HCI_OGF_LINK_POLICY         0x02
#define HCI_OGF_CTRL_BASEBAND       0x03
#define HCI_OGF_INFO_PARAM          0x04
#define HCI_OGF_STATUS_PARAM        0x05
#define HCI_OGF_TESTING             0x06
#define HCI_OGF_LE_CTRL             0x08
#define HCI_OGF_VENDOR              0x3F

// Macro to create OpCode from OGF and OCF
#define HCI_OPCODE(ogf, ocf)    ((uint16_t)((ogf) << 10) | (ocf))

// ============================================================================
// Link Control Commands (OGF 0x01)
// ============================================================================

#define HCI_OCF_INQUIRY                     0x0001
#define HCI_OCF_INQUIRY_CANCEL              0x0002
#define HCI_OCF_CREATE_CONNECTION           0x0005
#define HCI_OCF_DISCONNECT                  0x0006
#define HCI_OCF_ACCEPT_CONNECTION_REQUEST   0x0009
#define HCI_OCF_REJECT_CONNECTION_REQUEST   0x000A
#define HCI_OCF_LINK_KEY_REQUEST_REPLY      0x000B
#define HCI_OCF_LINK_KEY_REQUEST_NEG_REPLY  0x000C
#define HCI_OCF_PIN_CODE_REQUEST_REPLY      0x000D
#define HCI_OCF_PIN_CODE_REQUEST_NEG_REPLY  0x000E
#define HCI_OCF_AUTH_REQUESTED              0x0011
#define HCI_OCF_SET_CONNECTION_ENCRYPT      0x0013
#define HCI_OCF_REMOTE_NAME_REQUEST         0x0019

#define HCI_CMD_INQUIRY                     HCI_OPCODE(HCI_OGF_LINK_CONTROL, HCI_OCF_INQUIRY)
#define HCI_CMD_INQUIRY_CANCEL              HCI_OPCODE(HCI_OGF_LINK_CONTROL, HCI_OCF_INQUIRY_CANCEL)
#define HCI_CMD_CREATE_CONNECTION           HCI_OPCODE(HCI_OGF_LINK_CONTROL, HCI_OCF_CREATE_CONNECTION)
#define HCI_CMD_DISCONNECT                  HCI_OPCODE(HCI_OGF_LINK_CONTROL, HCI_OCF_DISCONNECT)
#define HCI_CMD_ACCEPT_CONNECTION_REQUEST   HCI_OPCODE(HCI_OGF_LINK_CONTROL, HCI_OCF_ACCEPT_CONNECTION_REQUEST)
#define HCI_CMD_REMOTE_NAME_REQUEST         HCI_OPCODE(HCI_OGF_LINK_CONTROL, HCI_OCF_REMOTE_NAME_REQUEST)
#define HCI_CMD_AUTH_REQUESTED              HCI_OPCODE(HCI_OGF_LINK_CONTROL, HCI_OCF_AUTH_REQUESTED)
#define HCI_CMD_PIN_CODE_REQUEST_REPLY      HCI_OPCODE(HCI_OGF_LINK_CONTROL, HCI_OCF_PIN_CODE_REQUEST_REPLY)

// ============================================================================
// Controller & Baseband Commands (OGF 0x03)
// ============================================================================

#define HCI_OCF_RESET                       0x0003
#define HCI_OCF_SET_EVENT_FILTER            0x0005
#define HCI_OCF_WRITE_PIN_TYPE              0x000A
#define HCI_OCF_READ_STORED_LINK_KEY        0x000D
#define HCI_OCF_WRITE_STORED_LINK_KEY       0x0011
#define HCI_OCF_DELETE_STORED_LINK_KEY      0x0012
#define HCI_OCF_WRITE_LOCAL_NAME            0x0013
#define HCI_OCF_READ_LOCAL_NAME             0x0014
#define HCI_OCF_READ_CONN_ACCEPT_TIMEOUT    0x0015
#define HCI_OCF_WRITE_CONN_ACCEPT_TIMEOUT   0x0016
#define HCI_OCF_READ_SCAN_ENABLE            0x0019
#define HCI_OCF_WRITE_SCAN_ENABLE           0x001A
#define HCI_OCF_READ_CLASS_OF_DEVICE        0x0023
#define HCI_OCF_WRITE_CLASS_OF_DEVICE       0x0024
#define HCI_OCF_SET_EVENT_MASK              0x0001
#define HCI_OCF_WRITE_SIMPLE_PAIRING_MODE   0x0056

#define HCI_CMD_RESET                       HCI_OPCODE(HCI_OGF_CTRL_BASEBAND, HCI_OCF_RESET)
#define HCI_CMD_SET_EVENT_MASK              HCI_OPCODE(HCI_OGF_CTRL_BASEBAND, HCI_OCF_SET_EVENT_MASK)
#define HCI_CMD_WRITE_SCAN_ENABLE           HCI_OPCODE(HCI_OGF_CTRL_BASEBAND, HCI_OCF_WRITE_SCAN_ENABLE)
#define HCI_CMD_WRITE_CLASS_OF_DEVICE       HCI_OPCODE(HCI_OGF_CTRL_BASEBAND, HCI_OCF_WRITE_CLASS_OF_DEVICE)
#define HCI_CMD_WRITE_LOCAL_NAME            HCI_OPCODE(HCI_OGF_CTRL_BASEBAND, HCI_OCF_WRITE_LOCAL_NAME)
#define HCI_CMD_READ_LOCAL_NAME             HCI_OPCODE(HCI_OGF_CTRL_BASEBAND, HCI_OCF_READ_LOCAL_NAME)

// ============================================================================
// Informational Parameters Commands (OGF 0x04)
// ============================================================================

#define HCI_OCF_READ_LOCAL_VERSION          0x0001
#define HCI_OCF_READ_LOCAL_COMMANDS         0x0002
#define HCI_OCF_READ_LOCAL_FEATURES         0x0003
#define HCI_OCF_READ_BD_ADDR                0x0009

#define HCI_CMD_READ_LOCAL_VERSION          HCI_OPCODE(HCI_OGF_INFO_PARAM, HCI_OCF_READ_LOCAL_VERSION)
#define HCI_CMD_READ_BD_ADDR                HCI_OPCODE(HCI_OGF_INFO_PARAM, HCI_OCF_READ_BD_ADDR)
#define HCI_CMD_READ_LOCAL_FEATURES         HCI_OPCODE(HCI_OGF_INFO_PARAM, HCI_OCF_READ_LOCAL_FEATURES)

// ============================================================================
// Scan Enable values
// ============================================================================

#define HCI_SCAN_DISABLED                   0x00
#define HCI_SCAN_INQUIRY                    0x01    // Discoverable
#define HCI_SCAN_PAGE                       0x02    // Connectable
#define HCI_SCAN_INQUIRY_PAGE               0x03    // Both

// ============================================================================
// HCI Event Codes
// ============================================================================

#define HCI_EVT_INQUIRY_COMPLETE            0x01
#define HCI_EVT_INQUIRY_RESULT              0x02
#define HCI_EVT_CONNECTION_COMPLETE         0x03
#define HCI_EVT_CONNECTION_REQUEST          0x04
#define HCI_EVT_DISCONNECTION_COMPLETE      0x05
#define HCI_EVT_AUTH_COMPLETE               0x06
#define HCI_EVT_REMOTE_NAME_REQ_COMPLETE    0x07
#define HCI_EVT_ENCRYPT_CHANGE              0x08
#define HCI_EVT_CHANGE_CONN_LINK_KEY_COMPL  0x09
#define HCI_EVT_COMMAND_COMPLETE            0x0E
#define HCI_EVT_COMMAND_STATUS              0x0F
#define HCI_EVT_HARDWARE_ERROR              0x10
#define HCI_EVT_NUM_COMPLETED_PACKETS       0x13
#define HCI_EVT_PIN_CODE_REQUEST            0x16
#define HCI_EVT_LINK_KEY_REQUEST            0x17
#define HCI_EVT_LINK_KEY_NOTIFICATION       0x18
#define HCI_EVT_INQUIRY_RESULT_WITH_RSSI    0x22
#define HCI_EVT_EXTENDED_INQUIRY_RESULT     0x2F
#define HCI_EVT_IO_CAPABILITY_REQUEST       0x31
#define HCI_EVT_SIMPLE_PAIRING_COMPLETE     0x36

// ============================================================================
// HCI Error Codes
// ============================================================================

#define HCI_SUCCESS                         0x00
#define HCI_ERR_UNKNOWN_CMD                 0x01
#define HCI_ERR_UNKNOWN_CONNECTION          0x02
#define HCI_ERR_HARDWARE_FAILURE            0x03
#define HCI_ERR_PAGE_TIMEOUT                0x04
#define HCI_ERR_AUTH_FAILURE                0x05
#define HCI_ERR_PIN_MISSING                 0x06
#define HCI_ERR_MEMORY_EXCEEDED             0x07
#define HCI_ERR_CONNECTION_TIMEOUT          0x08
#define HCI_ERR_MAX_NUM_CONNECTIONS         0x09
#define HCI_ERR_COMMAND_DISALLOWED          0x0C
#define HCI_ERR_REPEATED_ATTEMPTS           0x17
#define HCI_ERR_PAIRING_NOT_ALLOWED         0x18

// ============================================================================
// HCI Packet Structures
// ============================================================================

// HCI Command packet header
typedef struct {
    uint16_t opcode;
    uint8_t  param_len;
    // Parameters follow
} __attribute__((packed)) hci_cmd_header_t;

// HCI Event packet header
typedef struct {
    uint8_t  event_code;
    uint8_t  param_len;
    // Parameters follow
} __attribute__((packed)) hci_event_header_t;

// HCI ACL data packet header
typedef struct {
    uint16_t handle;        // Connection handle (12 bits) + flags (4 bits)
    uint16_t length;        // Data length
    // Data follows
} __attribute__((packed)) hci_acl_header_t;

// ACL packet boundary flags (in handle field)
#define HCI_ACL_PB_FIRST_NON_FLUSH  0x0000  // First non-flushable packet
#define HCI_ACL_PB_CONTINUE         0x1000  // Continuing fragment
#define HCI_ACL_PB_FIRST_FLUSH      0x2000  // First flushable packet
#define HCI_ACL_BROADCAST_NONE      0x0000  // Point-to-point
#define HCI_ACL_BROADCAST_ACTIVE    0x4000  // Active broadcast
#define HCI_ACL_BROADCAST_PICONET   0x8000  // Piconet broadcast

// ============================================================================
// Command Complete Event Parameters
// ============================================================================

typedef struct {
    uint8_t  num_cmd_packets;
    uint16_t opcode;
    uint8_t  status;
    // Return parameters follow
} __attribute__((packed)) hci_evt_cmd_complete_t;

// Command Status Event Parameters
typedef struct {
    uint8_t  status;
    uint8_t  num_cmd_packets;
    uint16_t opcode;
} __attribute__((packed)) hci_evt_cmd_status_t;

// Connection Complete Event Parameters
typedef struct {
    uint8_t   status;
    uint16_t  connection_handle;
    bt_addr_t bd_addr;
    uint8_t   link_type;
    uint8_t   encryption_enabled;
} __attribute__((packed)) hci_evt_connection_complete_t;

// Connection Request Event Parameters
typedef struct {
    bt_addr_t bd_addr;
    uint8_t   class_of_device[3];
    uint8_t   link_type;
} __attribute__((packed)) hci_evt_connection_request_t;

// Disconnection Complete Event Parameters
typedef struct {
    uint8_t  status;
    uint16_t connection_handle;
    uint8_t  reason;
} __attribute__((packed)) hci_evt_disconnection_complete_t;

// Inquiry Result Event Parameters
typedef struct {
    uint8_t   num_responses;
    // For each response:
    // bt_addr_t bd_addr;
    // uint8_t page_scan_repetition_mode;
    // uint8_t reserved[2];
    // uint8_t class_of_device[3];
    // uint16_t clock_offset;
} __attribute__((packed)) hci_evt_inquiry_result_t;

// Remote Name Request Complete Event Parameters
typedef struct {
    uint8_t   status;
    bt_addr_t bd_addr;
    char      name[248];
} __attribute__((packed)) hci_evt_remote_name_t;

// ============================================================================
// HCI Layer Functions
// ============================================================================

// Initialize HCI layer
int bt_hci_init(void);

// Shutdown HCI layer
void bt_hci_shutdown(void);

// Process incoming HCI event
void bt_hci_process_event(const uint8_t *data, int len);

// Process incoming ACL data
void bt_hci_process_acl(const uint8_t *data, int len);

// Send HCI command (blocking, waits for response)
int bt_hci_send_command(uint16_t opcode, const void *params, uint8_t param_len);

// Send HCI command and wait for specific event
int bt_hci_send_command_wait(uint16_t opcode, const void *params, uint8_t param_len,
                              uint8_t *response, int max_response);

// Send ACL data packet
int bt_hci_send_acl(uint16_t handle, uint16_t flags, const void *data, uint16_t len);

// ============================================================================
// HCI Commands (Convenience Functions)
// ============================================================================

// Controller & Baseband
int bt_hci_reset(void);
int bt_hci_set_event_mask(uint64_t mask);
int bt_hci_write_scan_enable(uint8_t scan);
int bt_hci_write_class_of_device(uint32_t cod);
int bt_hci_write_local_name(const char *name);

// Informational Parameters
int bt_hci_read_bd_addr(bt_addr_t *addr);
int bt_hci_read_local_version(uint8_t *hci_version, uint16_t *hci_revision,
                               uint8_t *lmp_version, uint16_t *manufacturer,
                               uint16_t *lmp_subversion);

// Link Control
int bt_hci_inquiry(uint8_t duration, uint8_t max_responses);
int bt_hci_inquiry_cancel(void);
int bt_hci_create_connection(bt_addr_t *addr, uint16_t pkt_type,
                              uint8_t page_scan_mode, uint16_t clock_offset,
                              uint8_t allow_role_switch);
int bt_hci_disconnect(uint16_t handle, uint8_t reason);
int bt_hci_accept_connection(bt_addr_t *addr, uint8_t role);
int bt_hci_remote_name_request(bt_addr_t *addr);
int bt_hci_auth_request(uint16_t handle);

// ============================================================================
// HCI State
// ============================================================================

// Get current HCI state
int bt_hci_is_initialized(void);

// Get number of free command slots
int bt_hci_get_num_cmd_packets(void);

#endif // BT_HCI_H
