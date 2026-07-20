// bt_hci.c - Bluetooth Host Controller Interface (HCI) Layer
#include "bt_hci.h"
#include "bt_l2cap.h"
#include "bluetooth.h"
#include "../../serial.h"
#include "../../string.h"
#include "../../mm/heap.h"

// ============================================================================
// HCI State
// ============================================================================

static struct {
    int initialized;
    int num_cmd_packets;        // Available command slots
    uint16_t pending_opcode;    // Opcode of pending command
    int command_pending;        // Command awaiting response
    int command_complete;       // Command completed flag
    uint8_t command_status;     // Status from command complete
    uint8_t response_buffer[256];
    int response_len;

    // Inquiry state
    int inquiry_active;

    // Event mask
    uint64_t event_mask;
} hci_state;

// Command buffer
static uint8_t hci_cmd_buffer[260] __attribute__((aligned(4)));

// External USB transport functions
extern int bt_usb_send_command(const uint8_t *cmd, int len);

// ============================================================================
// HCI Command Sending
// ============================================================================

// Send raw HCI command
int bt_hci_send_command(uint16_t opcode, const void *params, uint8_t param_len) {
    if (!hci_state.initialized && opcode != HCI_CMD_RESET) {
        kprintf("[HCI] Not initialized\n");
        return -1;
    }

    // Build command packet
    hci_cmd_header_t *hdr = (hci_cmd_header_t *)hci_cmd_buffer;
    hdr->opcode = opcode;
    hdr->param_len = param_len;

    if (param_len > 0 && params) {
        memcpy(hci_cmd_buffer + sizeof(hci_cmd_header_t), params, param_len);
    }

    // Mark command as pending
    hci_state.pending_opcode = opcode;
    hci_state.command_pending = 1;
    hci_state.command_complete = 0;

    // Send via USB
    int total_len = sizeof(hci_cmd_header_t) + param_len;

    kprintf("[HCI] Sending command: opcode=0x%04x len=%d\n", opcode, total_len);

    int result = bt_usb_send_command(hci_cmd_buffer, total_len);
    if (result < 0) {
        kprintf("[HCI] Failed to send command\n");
        hci_state.command_pending = 0;
        return -1;
    }

    return 0;
}

// Send command and wait for completion
int bt_hci_send_command_wait(uint16_t opcode, const void *params, uint8_t param_len,
                              uint8_t *response, int max_response) {
    int result = bt_hci_send_command(opcode, params, param_len);
    if (result < 0) {
        return result;
    }

    // Poll for response (with timeout)
    extern void bt_usb_poll(void);
    for (int i = 0; i < 1000 && !hci_state.command_complete; i++) {
        bt_usb_poll();
        // Small delay
        for (volatile int j = 0; j < 10000; j++);
    }

    if (!hci_state.command_complete) {
        kprintf("[HCI] Command timeout: opcode=0x%04x\n", opcode);
        hci_state.command_pending = 0;
        return -1;
    }

    if (hci_state.command_status != HCI_SUCCESS) {
        kprintf("[HCI] Command failed: status=0x%02x\n", hci_state.command_status);
        return -hci_state.command_status;
    }

    // Copy response if requested
    if (response && max_response > 0) {
        int copy_len = hci_state.response_len < max_response ?
                       hci_state.response_len : max_response;
        memcpy(response, hci_state.response_buffer, copy_len);
        return copy_len;
    }

    return 0;
}

// ============================================================================
// HCI Event Processing
// ============================================================================

// Handle Command Complete event
static void hci_handle_cmd_complete(const uint8_t *params, int len) {
    if (len < 4) return;

    hci_evt_cmd_complete_t *evt = (hci_evt_cmd_complete_t *)params;

    kprintf("[HCI] Command Complete: opcode=0x%04x status=0x%02x\n",
            evt->opcode, evt->status);

    hci_state.num_cmd_packets = evt->num_cmd_packets;

    // Check if this is response to our pending command
    if (hci_state.command_pending && evt->opcode == hci_state.pending_opcode) {
        hci_state.command_status = evt->status;
        hci_state.command_complete = 1;
        hci_state.command_pending = 0;

        // Store response parameters (after status)
        int resp_len = len - 4;  // Skip header fields
        if (resp_len > 0 && resp_len < (int)sizeof(hci_state.response_buffer)) {
            memcpy(hci_state.response_buffer, params + 4, resp_len);
            hci_state.response_len = resp_len;
        } else {
            hci_state.response_len = 0;
        }
    }
}

// Handle Command Status event
static void hci_handle_cmd_status(const uint8_t *params, int len) {
    if (len < 4) return;

    hci_evt_cmd_status_t *evt = (hci_evt_cmd_status_t *)params;

    kprintf("[HCI] Command Status: opcode=0x%04x status=0x%02x\n",
            evt->opcode, evt->status);

    hci_state.num_cmd_packets = evt->num_cmd_packets;

    // For some commands, Command Status indicates command was received
    // and the actual result will come in another event
    if (hci_state.command_pending && evt->opcode == hci_state.pending_opcode) {
        if (evt->status != HCI_SUCCESS) {
            // Command failed immediately
            hci_state.command_status = evt->status;
            hci_state.command_complete = 1;
            hci_state.command_pending = 0;
            hci_state.response_len = 0;
        }
        // Otherwise wait for the actual completion event
    }
}

// Handle Inquiry Complete event
static void hci_handle_inquiry_complete(const uint8_t *params, int len) {
    if (len < 1) return;

    uint8_t status = params[0];
    kprintf("[HCI] Inquiry Complete: status=0x%02x\n", status);

    hci_state.inquiry_active = 0;
}

// Handle Inquiry Result event
static void hci_handle_inquiry_result(const uint8_t *params, int len) {
    if (len < 1) return;

    uint8_t num_responses = params[0];
    kprintf("[HCI] Inquiry Result: %d device(s) found\n", num_responses);

    // Each response is 14 bytes: BD_ADDR(6) + PSR(1) + Reserved(2) + CoD(3) + ClkOff(2)
    const uint8_t *p = params + 1;

    for (int i = 0; i < num_responses && (p - params) + 14 <= len; i++) {
        bt_addr_t addr;
        memcpy(addr.addr, p, 6);
        p += 6;

        uint8_t page_scan_rep = *p++;
        p += 2;  // Skip reserved

        uint32_t class_of_device = p[0] | (p[1] << 8) | (p[2] << 16);
        p += 3;

        uint16_t clock_offset = p[0] | (p[1] << 8);
        p += 2;

        kprintf("[HCI]   Device %d: %02x:%02x:%02x:%02x:%02x:%02x CoD=0x%06x\n",
                i + 1,
                addr.addr[5], addr.addr[4], addr.addr[3],
                addr.addr[2], addr.addr[1], addr.addr[0],
                class_of_device);

        // Add to discovered devices list
        if (bt_adapter.device_count < BT_MAX_DEVICES) {
            bt_device_t *dev = &bt_adapter.devices[bt_adapter.device_count];
            bt_addr_copy(&dev->addr, &addr);
            dev->class_of_device = class_of_device;
            dev->state = BT_STATE_DISCOVERED;
            dev->name[0] = '\0';
            bt_adapter.device_count++;
        }

        (void)page_scan_rep;
        (void)clock_offset;
    }
}

// Handle Connection Complete event
static void hci_handle_connection_complete(const uint8_t *params, int len) {
    if ((size_t)len < sizeof(hci_evt_connection_complete_t)) return;

    hci_evt_connection_complete_t *evt = (hci_evt_connection_complete_t *)params;

    kprintf("[HCI] Connection Complete: status=0x%02x handle=0x%04x\n",
            evt->status, evt->connection_handle);
    kprintf("[HCI]   BD_ADDR: %02x:%02x:%02x:%02x:%02x:%02x\n",
            evt->bd_addr.addr[5], evt->bd_addr.addr[4], evt->bd_addr.addr[3],
            evt->bd_addr.addr[2], evt->bd_addr.addr[1], evt->bd_addr.addr[0]);

    if (evt->status == HCI_SUCCESS) {
        // Find device in list and update state
        for (int i = 0; i < bt_adapter.device_count; i++) {
            if (bt_addr_cmp(&bt_adapter.devices[i].addr, &evt->bd_addr) == 0) {
                bt_adapter.devices[i].state = BT_STATE_CONNECTED;
                bt_adapter.devices[i].connection_handle = evt->connection_handle;
                break;
            }
        }

        // Mark pending command as complete if waiting for connection
        if (hci_state.command_pending &&
            hci_state.pending_opcode == HCI_CMD_CREATE_CONNECTION) {
            hci_state.command_status = HCI_SUCCESS;
            hci_state.command_complete = 1;
            hci_state.command_pending = 0;
            // Store connection handle in response
            memcpy(hci_state.response_buffer, &evt->connection_handle, 2);
            hci_state.response_len = 2;
        }
    }
}

// Handle Connection Request event
static void hci_handle_connection_request(const uint8_t *params, int len) {
    if ((size_t)len < sizeof(hci_evt_connection_request_t)) return;

    hci_evt_connection_request_t *evt = (hci_evt_connection_request_t *)params;

    kprintf("[HCI] Connection Request from: %02x:%02x:%02x:%02x:%02x:%02x\n",
            evt->bd_addr.addr[5], evt->bd_addr.addr[4], evt->bd_addr.addr[3],
            evt->bd_addr.addr[2], evt->bd_addr.addr[1], evt->bd_addr.addr[0]);

    uint32_t cod = evt->class_of_device[0] | (evt->class_of_device[1] << 8) |
                   (evt->class_of_device[2] << 16);
    kprintf("[HCI]   CoD: 0x%06x, Link Type: %d\n", cod, evt->link_type);

    // Auto-accept ACL connections (link_type 0x01)
    if (evt->link_type == 0x01) {
        bt_hci_accept_connection((bt_addr_t *)&evt->bd_addr, 0x01);  // Role: Slave
    }
}

// Handle Disconnection Complete event
static void hci_handle_disconnection_complete(const uint8_t *params, int len) {
    if ((size_t)len < sizeof(hci_evt_disconnection_complete_t)) return;

    hci_evt_disconnection_complete_t *evt = (hci_evt_disconnection_complete_t *)params;

    kprintf("[HCI] Disconnection Complete: handle=0x%04x reason=0x%02x\n",
            evt->connection_handle, evt->reason);

    // Find device by handle and update state
    for (int i = 0; i < bt_adapter.device_count; i++) {
        if (bt_adapter.devices[i].connection_handle == evt->connection_handle) {
            bt_adapter.devices[i].state = BT_STATE_DISCONNECTED;
            bt_adapter.devices[i].connection_handle = 0;
            break;
        }
    }
}

// Handle Remote Name Request Complete event
static void hci_handle_remote_name_complete(const uint8_t *params, int len) {
    if (len < 7) return;  // At least status + addr

    hci_evt_remote_name_t *evt = (hci_evt_remote_name_t *)params;

    if (evt->status == HCI_SUCCESS) {
        kprintf("[HCI] Remote Name: %02x:%02x:%02x:%02x:%02x:%02x = \"%s\"\n",
                evt->bd_addr.addr[5], evt->bd_addr.addr[4], evt->bd_addr.addr[3],
                evt->bd_addr.addr[2], evt->bd_addr.addr[1], evt->bd_addr.addr[0],
                evt->name);

        // Update device name in list
        for (int i = 0; i < bt_adapter.device_count; i++) {
            if (bt_addr_cmp(&bt_adapter.devices[i].addr, &evt->bd_addr) == 0) {
                strncpy(bt_adapter.devices[i].name, evt->name, BT_MAX_NAME_LEN - 1);
                bt_adapter.devices[i].name[BT_MAX_NAME_LEN - 1] = '\0';
                break;
            }
        }
    } else {
        kprintf("[HCI] Remote Name Request failed: status=0x%02x\n", evt->status);
    }
}

// Handle PIN Code Request event
static void hci_handle_pin_code_request(const uint8_t *params, int len) {
    if (len < 6) return;

    bt_addr_t *addr = (bt_addr_t *)params;

    kprintf("[HCI] PIN Code Request from: %02x:%02x:%02x:%02x:%02x:%02x\n",
            addr->addr[5], addr->addr[4], addr->addr[3],
            addr->addr[2], addr->addr[1], addr->addr[0]);

    // Auto-reply with default PIN "0000"
    // In a real system, this would prompt the user
    uint8_t reply[23];  // BD_ADDR(6) + PIN_length(1) + PIN(16)
    memcpy(reply, addr->addr, 6);
    reply[6] = 4;  // PIN length
    reply[7] = '0';
    reply[8] = '0';
    reply[9] = '0';
    reply[10] = '0';
    memset(&reply[11], 0, 12);  // Padding

    bt_hci_send_command(HCI_CMD_PIN_CODE_REQUEST_REPLY, reply, 23);
}

// Handle Link Key Request event
static void hci_handle_link_key_request(const uint8_t *params, int len) {
    if (len < 6) return;

    bt_addr_t *addr = (bt_addr_t *)params;

    kprintf("[HCI] Link Key Request from: %02x:%02x:%02x:%02x:%02x:%02x\n",
            addr->addr[5], addr->addr[4], addr->addr[3],
            addr->addr[2], addr->addr[1], addr->addr[0]);

    // We don't have stored link keys, so send negative reply
    bt_hci_send_command(HCI_OPCODE(HCI_OGF_LINK_CONTROL, HCI_OCF_LINK_KEY_REQUEST_NEG_REPLY),
                        addr->addr, 6);
}

// Process incoming HCI event
void bt_hci_process_event(const uint8_t *data, int len) {
    if (len < 2) return;

    hci_event_header_t *hdr = (hci_event_header_t *)data;

    if (len < 2 + hdr->param_len) {
        kprintf("[HCI] Event truncated: expected %d, got %d\n",
                2 + hdr->param_len, len);
        return;
    }

    const uint8_t *params = data + 2;
    int param_len = hdr->param_len;

    switch (hdr->event_code) {
        case HCI_EVT_COMMAND_COMPLETE:
            hci_handle_cmd_complete(params, param_len);
            break;

        case HCI_EVT_COMMAND_STATUS:
            hci_handle_cmd_status(params, param_len);
            break;

        case HCI_EVT_INQUIRY_COMPLETE:
            hci_handle_inquiry_complete(params, param_len);
            break;

        case HCI_EVT_INQUIRY_RESULT:
        case HCI_EVT_INQUIRY_RESULT_WITH_RSSI:
        case HCI_EVT_EXTENDED_INQUIRY_RESULT:
            hci_handle_inquiry_result(params, param_len);
            break;

        case HCI_EVT_CONNECTION_COMPLETE:
            hci_handle_connection_complete(params, param_len);
            break;

        case HCI_EVT_CONNECTION_REQUEST:
            hci_handle_connection_request(params, param_len);
            break;

        case HCI_EVT_DISCONNECTION_COMPLETE:
            hci_handle_disconnection_complete(params, param_len);
            break;

        case HCI_EVT_REMOTE_NAME_REQ_COMPLETE:
            hci_handle_remote_name_complete(params, param_len);
            break;

        case HCI_EVT_PIN_CODE_REQUEST:
            hci_handle_pin_code_request(params, param_len);
            break;

        case HCI_EVT_LINK_KEY_REQUEST:
            hci_handle_link_key_request(params, param_len);
            break;

        case HCI_EVT_NUM_COMPLETED_PACKETS:
            // ACK for sent packets, just ignore for now
            break;

        default:
            kprintf("[HCI] Unhandled event: 0x%02x len=%d\n",
                    hdr->event_code, param_len);
            break;
    }
}

// Process incoming ACL data
void bt_hci_process_acl(const uint8_t *data, int len) {
    if (len < 4) return;

    hci_acl_header_t *hdr = (hci_acl_header_t *)data;
    uint16_t handle = hdr->handle & 0x0FFF;
    uint16_t flags = hdr->handle & 0xF000;
    uint16_t data_len = hdr->length;

    if (len < 4 + data_len) {
        kprintf("[HCI] ACL packet truncated\n");
        return;
    }

    kprintf("[HCI] ACL Data: handle=0x%03x flags=0x%x len=%d\n",
            handle, flags >> 12, data_len);

    // Pass to L2CAP layer
    bt_l2cap_process_acl(handle, data + 4, data_len);
}

// Send ACL data
int bt_hci_send_acl(uint16_t handle, uint16_t flags, const void *data, uint16_t len) {
    static uint8_t acl_buffer[1028] __attribute__((aligned(4)));

    if (len > 1024) {
        return -1;
    }

    hci_acl_header_t *hdr = (hci_acl_header_t *)acl_buffer;
    hdr->handle = (handle & 0x0FFF) | (flags & 0xF000);
    hdr->length = len;

    memcpy(acl_buffer + 4, data, len);

    extern int bt_usb_send_acl(const uint8_t *data, int len);
    return bt_usb_send_acl(acl_buffer, 4 + len);
}

// ============================================================================
// HCI Convenience Commands
// ============================================================================

int bt_hci_reset(void) {
    kprintf("[HCI] Sending Reset command\n");
    return bt_hci_send_command_wait(HCI_CMD_RESET, NULL, 0, NULL, 0);
}

int bt_hci_set_event_mask(uint64_t mask) {
    kprintf("[HCI] Setting event mask: 0x%016llx\n", (unsigned long long)mask);
    hci_state.event_mask = mask;
    return bt_hci_send_command_wait(HCI_CMD_SET_EVENT_MASK, &mask, 8, NULL, 0);
}

int bt_hci_write_scan_enable(uint8_t scan) {
    kprintf("[HCI] Setting scan enable: 0x%02x\n", scan);
    return bt_hci_send_command_wait(HCI_CMD_WRITE_SCAN_ENABLE, &scan, 1, NULL, 0);
}

int bt_hci_write_class_of_device(uint32_t cod) {
    uint8_t cod_bytes[3] = { cod & 0xFF, (cod >> 8) & 0xFF, (cod >> 16) & 0xFF };
    kprintf("[HCI] Setting Class of Device: 0x%06x\n", cod);
    return bt_hci_send_command_wait(HCI_CMD_WRITE_CLASS_OF_DEVICE, cod_bytes, 3, NULL, 0);
}

int bt_hci_write_local_name(const char *name) {
    char name_buf[248];
    memset(name_buf, 0, sizeof(name_buf));
    strncpy(name_buf, name, 247);
    kprintf("[HCI] Setting local name: \"%s\"\n", name);
    return bt_hci_send_command_wait(HCI_CMD_WRITE_LOCAL_NAME, name_buf, 248, NULL, 0);
}

int bt_hci_read_bd_addr(bt_addr_t *addr) {
    uint8_t response[7];  // status(1) + BD_ADDR(6)
    int result = bt_hci_send_command_wait(HCI_CMD_READ_BD_ADDR, NULL, 0, response, 7);
    if (result < 0) {
        return result;
    }
    if (addr) {
        memcpy(addr->addr, response, 6);
    }
    kprintf("[HCI] Local BD_ADDR: %02x:%02x:%02x:%02x:%02x:%02x\n",
            response[5], response[4], response[3],
            response[2], response[1], response[0]);
    return 0;
}

int bt_hci_read_local_version(uint8_t *hci_version, uint16_t *hci_revision,
                               uint8_t *lmp_version, uint16_t *manufacturer,
                               uint16_t *lmp_subversion) {
    uint8_t response[9];
    int result = bt_hci_send_command_wait(HCI_CMD_READ_LOCAL_VERSION, NULL, 0, response, 9);
    if (result < 0) {
        return result;
    }
    if (hci_version) *hci_version = response[0];
    if (hci_revision) *hci_revision = response[1] | (response[2] << 8);
    if (lmp_version) *lmp_version = response[3];
    if (manufacturer) *manufacturer = response[4] | (response[5] << 8);
    if (lmp_subversion) *lmp_subversion = response[6] | (response[7] << 8);
    return 0;
}

int bt_hci_inquiry(uint8_t duration, uint8_t max_responses) {
    // LAP for GIAC (General Inquiry Access Code): 0x9E8B33
    uint8_t params[5];
    params[0] = 0x33;  // LAP low byte
    params[1] = 0x8B;
    params[2] = 0x9E;  // LAP high byte
    params[3] = duration;      // Inquiry length (1.28s units)
    params[4] = max_responses; // Max responses (0 = unlimited)

    kprintf("[HCI] Starting Inquiry (duration=%d, max=%d)\n", duration, max_responses);

    // Clear device list
    bt_adapter.device_count = 0;
    hci_state.inquiry_active = 1;

    return bt_hci_send_command(HCI_CMD_INQUIRY, params, 5);
}

int bt_hci_inquiry_cancel(void) {
    kprintf("[HCI] Canceling Inquiry\n");
    hci_state.inquiry_active = 0;
    return bt_hci_send_command_wait(HCI_CMD_INQUIRY_CANCEL, NULL, 0, NULL, 0);
}

int bt_hci_create_connection(bt_addr_t *addr, uint16_t pkt_type,
                              uint8_t page_scan_mode, uint16_t clock_offset,
                              uint8_t allow_role_switch) {
    uint8_t params[13];
    memcpy(params, addr->addr, 6);
    params[6] = pkt_type & 0xFF;
    params[7] = (pkt_type >> 8) & 0xFF;
    params[8] = page_scan_mode;
    params[9] = 0;  // Reserved
    params[10] = clock_offset & 0xFF;
    params[11] = (clock_offset >> 8) & 0xFF;
    params[12] = allow_role_switch;

    kprintf("[HCI] Creating connection to %02x:%02x:%02x:%02x:%02x:%02x\n",
            addr->addr[5], addr->addr[4], addr->addr[3],
            addr->addr[2], addr->addr[1], addr->addr[0]);

    return bt_hci_send_command(HCI_CMD_CREATE_CONNECTION, params, 13);
}

int bt_hci_disconnect(uint16_t handle, uint8_t reason) {
    uint8_t params[3];
    params[0] = handle & 0xFF;
    params[1] = (handle >> 8) & 0xFF;
    params[2] = reason;

    kprintf("[HCI] Disconnecting handle 0x%04x, reason 0x%02x\n", handle, reason);
    return bt_hci_send_command(HCI_CMD_DISCONNECT, params, 3);
}

int bt_hci_accept_connection(bt_addr_t *addr, uint8_t role) {
    uint8_t params[7];
    memcpy(params, addr->addr, 6);
    params[6] = role;

    kprintf("[HCI] Accepting connection from %02x:%02x:%02x:%02x:%02x:%02x (role=%d)\n",
            addr->addr[5], addr->addr[4], addr->addr[3],
            addr->addr[2], addr->addr[1], addr->addr[0], role);

    return bt_hci_send_command(HCI_CMD_ACCEPT_CONNECTION_REQUEST, params, 7);
}

int bt_hci_remote_name_request(bt_addr_t *addr) {
    uint8_t params[10];
    memcpy(params, addr->addr, 6);
    params[6] = 0x02;  // Page Scan Repetition Mode R2
    params[7] = 0x00;  // Reserved
    params[8] = 0x00;  // Clock offset (none)
    params[9] = 0x00;

    kprintf("[HCI] Requesting name from %02x:%02x:%02x:%02x:%02x:%02x\n",
            addr->addr[5], addr->addr[4], addr->addr[3],
            addr->addr[2], addr->addr[1], addr->addr[0]);

    return bt_hci_send_command(HCI_CMD_REMOTE_NAME_REQUEST, params, 10);
}

int bt_hci_auth_request(uint16_t handle) {
    uint8_t params[2];
    params[0] = handle & 0xFF;
    params[1] = (handle >> 8) & 0xFF;

    kprintf("[HCI] Requesting authentication for handle 0x%04x\n", handle);
    return bt_hci_send_command(HCI_CMD_AUTH_REQUESTED, params, 2);
}

// ============================================================================
// HCI State Functions
// ============================================================================

int bt_hci_is_initialized(void) {
    return hci_state.initialized;
}

int bt_hci_get_num_cmd_packets(void) {
    return hci_state.num_cmd_packets;
}

// ============================================================================
// HCI Initialization
// ============================================================================

int bt_hci_init(void) {
    kprintf("\n[HCI] Initializing HCI layer...\n");

    memset(&hci_state, 0, sizeof(hci_state));
    hci_state.num_cmd_packets = 1;  // Assume we can send 1 command initially

    // Reset the controller
    if (bt_hci_reset() < 0) {
        kprintf("[HCI] Reset failed\n");
        return -1;
    }

    hci_state.initialized = 1;

    // Read local BD_ADDR
    if (bt_hci_read_bd_addr(&bt_adapter.local_addr) < 0) {
        kprintf("[HCI] Failed to read BD_ADDR\n");
    }

    // Read local version
    uint8_t hci_ver, lmp_ver;
    uint16_t hci_rev, manufacturer, lmp_sub;
    if (bt_hci_read_local_version(&hci_ver, &hci_rev, &lmp_ver, &manufacturer, &lmp_sub) == 0) {
        kprintf("[HCI] HCI Version: %d.%d, LMP Version: %d.%d, Manufacturer: 0x%04x\n",
                hci_ver >> 4, hci_ver & 0xF, lmp_ver >> 4, lmp_ver & 0xF, manufacturer);
    }

    // Set event mask to receive all standard events
    uint64_t event_mask = 0x00001FFFFFFFFFFF;  // Enable most events
    bt_hci_set_event_mask(event_mask);

    // Set local name
    bt_hci_write_local_name("MayteraOS Bluetooth");

    // Set class of device: Computer (desktop)
    uint32_t cod = (BT_CLASS_COMPUTER << 8) | 0x04;  // Desktop workstation
    bt_hci_write_class_of_device(cod);

    // Make discoverable and connectable
    bt_hci_write_scan_enable(HCI_SCAN_INQUIRY_PAGE);

    bt_adapter.hci_initialized = 1;

    kprintf("[HCI] HCI layer initialized\n");
    return 0;
}

void bt_hci_shutdown(void) {
    if (!hci_state.initialized) return;

    kprintf("[HCI] Shutting down HCI layer\n");

    // Disable scan
    bt_hci_write_scan_enable(HCI_SCAN_DISABLED);

    // Reset controller
    bt_hci_reset();

    hci_state.initialized = 0;
    bt_adapter.hci_initialized = 0;
}
