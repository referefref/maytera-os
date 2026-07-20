// bt_l2cap.c - Bluetooth Logical Link Control and Adaptation Protocol (L2CAP)
#include "bt_l2cap.h"
#include "bt_hci.h"
#include "bt_hid.h"
#include "bluetooth.h"
#include "../../serial.h"
#include "../../string.h"
#include "../../mm/heap.h"

// ============================================================================
// L2CAP State
// ============================================================================

static struct {
    int initialized;

    // Channels
    l2cap_channel_t channels[L2CAP_MAX_CHANNELS];
    uint16_t next_local_cid;

    // Signaling state
    uint8_t next_identifier;

    // PSM handlers
    struct {
        uint16_t psm;
        l2cap_recv_callback_t callback;
    } psm_handlers[8];
    int num_psm_handlers;
} l2cap_state;

// Transmit buffer
static uint8_t l2cap_tx_buffer[L2CAP_MAX_MTU + 16] __attribute__((aligned(4)));

// ============================================================================
// Channel Management
// ============================================================================

// Allocate a new channel
static l2cap_channel_t *l2cap_alloc_channel(void) {
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        if (!l2cap_state.channels[i].active) {
            l2cap_channel_t *ch = &l2cap_state.channels[i];
            memset(ch, 0, sizeof(l2cap_channel_t));
            ch->active = 1;
            ch->local_cid = l2cap_state.next_local_cid++;
            ch->local_mtu = L2CAP_DEFAULT_MTU;
            ch->remote_mtu = L2CAP_DEFAULT_MTU;

            // Wrap around CID range (note: uint16_t can hold L2CAP_CID_DYN_END=0xFFFF)
            // But we wrap earlier to avoid overflow issues
            if (l2cap_state.next_local_cid >= 0xFF00) {
                l2cap_state.next_local_cid = L2CAP_CID_DYN_START;
            }

            return ch;
        }
    }
    return NULL;
}

// Free a channel
static void l2cap_free_channel(l2cap_channel_t *ch) {
    if (ch) {
        ch->active = 0;
        ch->state = L2CAP_STATE_CLOSED;
    }
}

// Find channel by local CID
l2cap_channel_t *bt_l2cap_get_channel(uint16_t cid) {
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        if (l2cap_state.channels[i].active &&
            l2cap_state.channels[i].local_cid == cid) {
            return &l2cap_state.channels[i];
        }
    }
    return NULL;
}

// Find channel by remote CID and handle
__attribute__((unused))
__attribute__((unused)) static l2cap_channel_t *l2cap_find_by_remote(uint16_t handle, uint16_t remote_cid) {
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        if (l2cap_state.channels[i].active &&
            l2cap_state.channels[i].handle == handle &&
            l2cap_state.channels[i].remote_cid == remote_cid) {
            return &l2cap_state.channels[i];
        }
    }
    return NULL;
}

// Find PSM handler
static l2cap_recv_callback_t l2cap_find_psm_handler(uint16_t psm) {
    for (int i = 0; i < l2cap_state.num_psm_handlers; i++) {
        if (l2cap_state.psm_handlers[i].psm == psm) {
            return l2cap_state.psm_handlers[i].callback;
        }
    }
    return NULL;
}

// ============================================================================
// L2CAP Signaling
// ============================================================================

// Get next identifier for signaling commands
static uint8_t l2cap_next_id(void) {
    uint8_t id = l2cap_state.next_identifier++;
    if (id == 0) id = l2cap_state.next_identifier++;  // Skip 0
    return id;
}

// Send L2CAP signaling command
static int l2cap_send_signal(uint16_t handle, uint8_t code, uint8_t id,
                             const void *data, uint16_t len) {
    // Build L2CAP packet
    l2cap_header_t *l2hdr = (l2cap_header_t *)l2cap_tx_buffer;
    l2hdr->length = sizeof(l2cap_sig_header_t) + len;
    l2hdr->cid = L2CAP_CID_SIGNALING;

    l2cap_sig_header_t *sig = (l2cap_sig_header_t *)(l2cap_tx_buffer + sizeof(l2cap_header_t));
    sig->code = code;
    sig->identifier = id;
    sig->length = len;

    if (len > 0 && data) {
        memcpy(l2cap_tx_buffer + sizeof(l2cap_header_t) + sizeof(l2cap_sig_header_t),
               data, len);
    }

    int total = sizeof(l2cap_header_t) + sizeof(l2cap_sig_header_t) + len;

    kprintf("[L2CAP] Sending signal: code=0x%02x id=%d len=%d\n", code, id, len);

    return bt_hci_send_acl(handle, HCI_ACL_PB_FIRST_FLUSH, l2cap_tx_buffer, total);
}

// Send connection request
static int l2cap_send_conn_req(uint16_t handle, uint16_t psm, uint16_t scid) {
    l2cap_conn_req_t req;
    req.psm = psm;
    req.scid = scid;

    return l2cap_send_signal(handle, L2CAP_CMD_CONN_REQ, l2cap_next_id(),
                             &req, sizeof(req));
}

// Send connection response
static int l2cap_send_conn_rsp(uint16_t handle, uint8_t id,
                               uint16_t dcid, uint16_t scid,
                               uint16_t result, uint16_t status) {
    l2cap_conn_rsp_t rsp;
    rsp.dcid = dcid;
    rsp.scid = scid;
    rsp.result = result;
    rsp.status = status;

    return l2cap_send_signal(handle, L2CAP_CMD_CONN_RSP, id, &rsp, sizeof(rsp));
}

// Send configuration request
static int l2cap_send_config_req(uint16_t handle, uint16_t dcid, uint16_t mtu) {
    uint8_t data[8];
    l2cap_config_req_t *req = (l2cap_config_req_t *)data;
    req->dcid = dcid;
    req->flags = 0;

    // Add MTU option
    l2cap_conf_opt_t *opt = (l2cap_conf_opt_t *)(data + sizeof(l2cap_config_req_t));
    opt->type = L2CAP_CONF_OPT_MTU;
    opt->length = 2;
    data[6] = mtu & 0xFF;
    data[7] = (mtu >> 8) & 0xFF;

    return l2cap_send_signal(handle, L2CAP_CMD_CONFIG_REQ, l2cap_next_id(),
                             data, 8);
}

// Send configuration response
static int l2cap_send_config_rsp(uint16_t handle, uint8_t id,
                                  uint16_t scid, uint16_t result) {
    l2cap_config_rsp_t rsp;
    rsp.scid = scid;
    rsp.flags = 0;
    rsp.result = result;

    return l2cap_send_signal(handle, L2CAP_CMD_CONFIG_RSP, id, &rsp, sizeof(rsp));
}

// Send disconnection request
static int l2cap_send_disc_req(uint16_t handle, uint16_t dcid, uint16_t scid) {
    l2cap_disc_req_t req;
    req.dcid = dcid;
    req.scid = scid;

    return l2cap_send_signal(handle, L2CAP_CMD_DISC_REQ, l2cap_next_id(),
                             &req, sizeof(req));
}

// Send disconnection response
static int l2cap_send_disc_rsp(uint16_t handle, uint8_t id,
                               uint16_t dcid, uint16_t scid) {
    l2cap_disc_rsp_t rsp;
    rsp.dcid = dcid;
    rsp.scid = scid;

    return l2cap_send_signal(handle, L2CAP_CMD_DISC_RSP, id, &rsp, sizeof(rsp));
}

// ============================================================================
// Signaling Command Handlers
// ============================================================================

static void l2cap_handle_conn_req(uint16_t handle, uint8_t id,
                                   const uint8_t *data, uint16_t len) {
    if (len < sizeof(l2cap_conn_req_t)) return;

    l2cap_conn_req_t *req = (l2cap_conn_req_t *)data;
    kprintf("[L2CAP] Connection Request: PSM=0x%04x SCID=0x%04x\n",
            req->psm, req->scid);

    // Check if we have a handler for this PSM
    l2cap_recv_callback_t handler = l2cap_find_psm_handler(req->psm);
    if (!handler) {
        kprintf("[L2CAP] No handler for PSM 0x%04x\n", req->psm);
        l2cap_send_conn_rsp(handle, id, 0, req->scid,
                            L2CAP_CONN_REFUSED_PSM, 0);
        return;
    }

    // Allocate channel
    l2cap_channel_t *ch = l2cap_alloc_channel();
    if (!ch) {
        kprintf("[L2CAP] No free channels\n");
        l2cap_send_conn_rsp(handle, id, 0, req->scid,
                            L2CAP_CONN_REFUSED_RESOURCE, 0);
        return;
    }

    ch->handle = handle;
    ch->psm = req->psm;
    ch->remote_cid = req->scid;
    ch->state = L2CAP_STATE_CONFIG;
    ch->recv_callback = handler;

    // Send connection response
    l2cap_send_conn_rsp(handle, id, ch->local_cid, req->scid,
                        L2CAP_CONN_SUCCESS, 0);

    // Send configuration request
    l2cap_send_config_req(handle, req->scid, ch->local_mtu);
}

static void l2cap_handle_conn_rsp(uint16_t handle, uint8_t id,
                                   const uint8_t *data, uint16_t len) {
    (void)id;
    if (len < sizeof(l2cap_conn_rsp_t)) return;

    l2cap_conn_rsp_t *rsp = (l2cap_conn_rsp_t *)data;
    kprintf("[L2CAP] Connection Response: DCID=0x%04x SCID=0x%04x result=%d\n",
            rsp->dcid, rsp->scid, rsp->result);

    // Find our channel
    l2cap_channel_t *ch = bt_l2cap_get_channel(rsp->scid);
    if (!ch) {
        kprintf("[L2CAP] No channel for SCID 0x%04x\n", rsp->scid);
        return;
    }

    if (rsp->result == L2CAP_CONN_SUCCESS) {
        ch->remote_cid = rsp->dcid;
        ch->state = L2CAP_STATE_CONFIG;

        // Send configuration request
        l2cap_send_config_req(handle, rsp->dcid, ch->local_mtu);
    } else if (rsp->result == L2CAP_CONN_PENDING) {
        kprintf("[L2CAP] Connection pending, status=%d\n", rsp->status);
    } else {
        kprintf("[L2CAP] Connection refused, result=%d\n", rsp->result);
        l2cap_free_channel(ch);
    }
}

static void l2cap_handle_config_req(uint16_t handle, uint8_t id,
                                     const uint8_t *data, uint16_t len) {
    if (len < sizeof(l2cap_config_req_t)) return;

    l2cap_config_req_t *req = (l2cap_config_req_t *)data;
    kprintf("[L2CAP] Config Request: DCID=0x%04x flags=0x%04x\n",
            req->dcid, req->flags);

    // Find channel
    l2cap_channel_t *ch = bt_l2cap_get_channel(req->dcid);
    if (!ch) {
        kprintf("[L2CAP] No channel for DCID 0x%04x\n", req->dcid);
        return;
    }

    // Parse options
    const uint8_t *opt_data = data + sizeof(l2cap_config_req_t);
    int opt_len = len - sizeof(l2cap_config_req_t);

    while (opt_len >= 2) {
        l2cap_conf_opt_t *opt = (l2cap_conf_opt_t *)opt_data;
        if (opt_len < 2 + opt->length) break;

        if (opt->type == L2CAP_CONF_OPT_MTU) {
            if (opt->length >= 2) {
                ch->remote_mtu = opt_data[2] | (opt_data[3] << 8);
                kprintf("[L2CAP] Remote MTU: %d\n", ch->remote_mtu);
            }
        }

        opt_data += 2 + opt->length;
        opt_len -= 2 + opt->length;
    }

    // Send config response
    l2cap_send_config_rsp(handle, id, ch->remote_cid, L2CAP_CONF_SUCCESS);

    ch->remote_config_done = 1;

    // Check if configuration is complete
    if (ch->local_config_done && ch->remote_config_done) {
        ch->state = L2CAP_STATE_OPEN;
        kprintf("[L2CAP] Channel 0x%04x OPEN\n", ch->local_cid);
    }
}

static void l2cap_handle_config_rsp(uint16_t handle, uint8_t id,
                                     const uint8_t *data, uint16_t len) {
    (void)handle;
    (void)id;
    if (len < sizeof(l2cap_config_rsp_t)) return;

    l2cap_config_rsp_t *rsp = (l2cap_config_rsp_t *)data;
    kprintf("[L2CAP] Config Response: SCID=0x%04x result=%d\n",
            rsp->scid, rsp->result);

    // Find channel by remote CID
    l2cap_channel_t *ch = NULL;
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        if (l2cap_state.channels[i].active &&
            l2cap_state.channels[i].remote_cid == rsp->scid) {
            ch = &l2cap_state.channels[i];
            break;
        }
    }

    if (!ch) {
        kprintf("[L2CAP] No channel for SCID 0x%04x\n", rsp->scid);
        return;
    }

    if (rsp->result == L2CAP_CONF_SUCCESS) {
        ch->local_config_done = 1;

        // Check if configuration is complete
        if (ch->local_config_done && ch->remote_config_done) {
            ch->state = L2CAP_STATE_OPEN;
            kprintf("[L2CAP] Channel 0x%04x OPEN\n", ch->local_cid);
        }
    } else {
        kprintf("[L2CAP] Config rejected, result=%d\n", rsp->result);
    }
}

static void l2cap_handle_disc_req(uint16_t handle, uint8_t id,
                                   const uint8_t *data, uint16_t len) {
    if (len < sizeof(l2cap_disc_req_t)) return;

    l2cap_disc_req_t *req = (l2cap_disc_req_t *)data;
    kprintf("[L2CAP] Disconnect Request: DCID=0x%04x SCID=0x%04x\n",
            req->dcid, req->scid);

    // Find channel
    l2cap_channel_t *ch = bt_l2cap_get_channel(req->dcid);
    if (ch) {
        // Send disconnect response
        l2cap_send_disc_rsp(handle, id, req->dcid, req->scid);
        l2cap_free_channel(ch);
    }
}

static void l2cap_handle_disc_rsp(uint16_t handle, uint8_t id,
                                   const uint8_t *data, uint16_t len) {
    (void)handle;
    (void)id;
    if (len < sizeof(l2cap_disc_rsp_t)) return;

    l2cap_disc_rsp_t *rsp = (l2cap_disc_rsp_t *)data;
    kprintf("[L2CAP] Disconnect Response: DCID=0x%04x SCID=0x%04x\n",
            rsp->dcid, rsp->scid);

    l2cap_channel_t *ch = bt_l2cap_get_channel(rsp->scid);
    if (ch) {
        l2cap_free_channel(ch);
    }
}

// Process signaling command
static void l2cap_process_signaling(uint16_t handle, const uint8_t *data, uint16_t len) {
    while (len >= sizeof(l2cap_sig_header_t)) {
        l2cap_sig_header_t *sig = (l2cap_sig_header_t *)data;

        if (len < sizeof(l2cap_sig_header_t) + sig->length) {
            kprintf("[L2CAP] Signaling packet truncated\n");
            break;
        }

        const uint8_t *cmd_data = data + sizeof(l2cap_sig_header_t);
        uint16_t cmd_len = sig->length;

        switch (sig->code) {
            case L2CAP_CMD_CONN_REQ:
                l2cap_handle_conn_req(handle, sig->identifier, cmd_data, cmd_len);
                break;

            case L2CAP_CMD_CONN_RSP:
                l2cap_handle_conn_rsp(handle, sig->identifier, cmd_data, cmd_len);
                break;

            case L2CAP_CMD_CONFIG_REQ:
                l2cap_handle_config_req(handle, sig->identifier, cmd_data, cmd_len);
                break;

            case L2CAP_CMD_CONFIG_RSP:
                l2cap_handle_config_rsp(handle, sig->identifier, cmd_data, cmd_len);
                break;

            case L2CAP_CMD_DISC_REQ:
                l2cap_handle_disc_req(handle, sig->identifier, cmd_data, cmd_len);
                break;

            case L2CAP_CMD_DISC_RSP:
                l2cap_handle_disc_rsp(handle, sig->identifier, cmd_data, cmd_len);
                break;

            default:
                kprintf("[L2CAP] Unknown signaling command: 0x%02x\n", sig->code);
                break;
        }

        data += sizeof(l2cap_sig_header_t) + sig->length;
        len -= sizeof(l2cap_sig_header_t) + sig->length;
    }
}

// ============================================================================
// L2CAP Data Processing
// ============================================================================

void bt_l2cap_process_acl(uint16_t handle, const uint8_t *data, uint16_t len) {
    if (len < sizeof(l2cap_header_t)) {
        kprintf("[L2CAP] Packet too short\n");
        return;
    }

    l2cap_header_t *hdr = (l2cap_header_t *)data;

    kprintf("[L2CAP] Packet: handle=0x%03x len=%d cid=0x%04x\n",
            handle, hdr->length, hdr->cid);

    if (len < sizeof(l2cap_header_t) + hdr->length) {
        kprintf("[L2CAP] Packet truncated\n");
        return;
    }

    const uint8_t *payload = data + sizeof(l2cap_header_t);
    uint16_t payload_len = hdr->length;

    // Handle based on CID
    switch (hdr->cid) {
        case L2CAP_CID_SIGNALING:
            l2cap_process_signaling(handle, payload, payload_len);
            break;

        default:
            // Dynamic channel - find handler
            if (hdr->cid >= L2CAP_CID_DYN_START) {
                l2cap_channel_t *ch = bt_l2cap_get_channel(hdr->cid);
                if (ch && ch->recv_callback) {
                    ch->recv_callback(hdr->cid, payload, payload_len);
                } else {
                    kprintf("[L2CAP] No handler for CID 0x%04x\n", hdr->cid);
                }
            } else {
                kprintf("[L2CAP] Unhandled CID 0x%04x\n", hdr->cid);
            }
            break;
    }
}

// ============================================================================
// L2CAP API
// ============================================================================

int bt_l2cap_connect(uint16_t handle, uint16_t psm) {
    l2cap_channel_t *ch = l2cap_alloc_channel();
    if (!ch) {
        kprintf("[L2CAP] No free channels\n");
        return -1;
    }

    ch->handle = handle;
    ch->psm = psm;
    ch->state = L2CAP_STATE_WAIT_CONNECT_RSP;

    kprintf("[L2CAP] Connecting to PSM 0x%04x, local CID 0x%04x\n",
            psm, ch->local_cid);

    l2cap_send_conn_req(handle, psm, ch->local_cid);

    return ch->local_cid;
}

int bt_l2cap_disconnect(uint16_t cid) {
    l2cap_channel_t *ch = bt_l2cap_get_channel(cid);
    if (!ch) {
        return -1;
    }

    if (ch->state == L2CAP_STATE_OPEN) {
        ch->state = L2CAP_STATE_WAIT_DISCONNECT;
        l2cap_send_disc_req(ch->handle, ch->remote_cid, ch->local_cid);
    }

    return 0;
}

int bt_l2cap_send(uint16_t cid, const void *data, uint16_t len) {
    l2cap_channel_t *ch = bt_l2cap_get_channel(cid);
    if (!ch || ch->state != L2CAP_STATE_OPEN) {
        return -1;
    }

    if (len > ch->remote_mtu) {
        kprintf("[L2CAP] Data too large: %d > %d\n", len, ch->remote_mtu);
        return -1;
    }

    // Build L2CAP packet
    l2cap_header_t *hdr = (l2cap_header_t *)l2cap_tx_buffer;
    hdr->length = len;
    hdr->cid = ch->remote_cid;

    memcpy(l2cap_tx_buffer + sizeof(l2cap_header_t), data, len);

    return bt_hci_send_acl(ch->handle, HCI_ACL_PB_FIRST_FLUSH,
                           l2cap_tx_buffer, sizeof(l2cap_header_t) + len);
}

int bt_l2cap_register_psm(uint16_t psm, l2cap_recv_callback_t callback) {
    if (l2cap_state.num_psm_handlers >= 8) {
        return -1;
    }

    l2cap_state.psm_handlers[l2cap_state.num_psm_handlers].psm = psm;
    l2cap_state.psm_handlers[l2cap_state.num_psm_handlers].callback = callback;
    l2cap_state.num_psm_handlers++;

    kprintf("[L2CAP] Registered PSM 0x%04x\n", psm);
    return 0;
}

void bt_l2cap_unregister_psm(uint16_t psm) {
    for (int i = 0; i < l2cap_state.num_psm_handlers; i++) {
        if (l2cap_state.psm_handlers[i].psm == psm) {
            // Shift remaining handlers
            for (int j = i; j < l2cap_state.num_psm_handlers - 1; j++) {
                l2cap_state.psm_handlers[j] = l2cap_state.psm_handlers[j + 1];
            }
            l2cap_state.num_psm_handlers--;
            break;
        }
    }
}

l2cap_state_t bt_l2cap_get_state(uint16_t cid) {
    l2cap_channel_t *ch = bt_l2cap_get_channel(cid);
    return ch ? ch->state : L2CAP_STATE_CLOSED;
}

void bt_l2cap_print_status(void) {
    kprintf("\n[L2CAP] Status:\n");
    kprintf("  Initialized: %s\n", l2cap_state.initialized ? "Yes" : "No");
    kprintf("  Next CID: 0x%04x\n", l2cap_state.next_local_cid);
    kprintf("  PSM Handlers: %d\n", l2cap_state.num_psm_handlers);

    for (int i = 0; i < l2cap_state.num_psm_handlers; i++) {
        kprintf("    PSM 0x%04x\n", l2cap_state.psm_handlers[i].psm);
    }

    kprintf("  Channels:\n");
    int active_count = 0;
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        l2cap_channel_t *ch = &l2cap_state.channels[i];
        if (ch->active) {
            const char *state_names[] = {
                "CLOSED", "WAIT_CONNECT", "WAIT_CONNECT_RSP",
                "CONFIG", "OPEN", "WAIT_DISCONNECT"
            };
            kprintf("    CID 0x%04x -> 0x%04x: PSM=0x%04x state=%s\n",
                    ch->local_cid, ch->remote_cid, ch->psm,
                    state_names[ch->state]);
            active_count++;
        }
    }
    if (active_count == 0) {
        kprintf("    (none)\n");
    }
}

// ============================================================================
// L2CAP Initialization
// ============================================================================

int bt_l2cap_init(void) {
    kprintf("\n[L2CAP] Initializing L2CAP layer...\n");

    memset(&l2cap_state, 0, sizeof(l2cap_state));
    l2cap_state.next_local_cid = L2CAP_CID_DYN_START;
    l2cap_state.next_identifier = 1;

    // Register HID PSM handlers
    bt_l2cap_register_psm(L2CAP_PSM_HID_CTRL, bt_hid_l2cap_recv);
    bt_l2cap_register_psm(L2CAP_PSM_HID_INTR, bt_hid_l2cap_recv);

    l2cap_state.initialized = 1;

    kprintf("[L2CAP] L2CAP layer initialized\n");
    return 0;
}

void bt_l2cap_shutdown(void) {
    if (!l2cap_state.initialized) return;

    kprintf("[L2CAP] Shutting down L2CAP layer\n");

    // Disconnect all channels
    for (int i = 0; i < L2CAP_MAX_CHANNELS; i++) {
        if (l2cap_state.channels[i].active) {
            l2cap_free_channel(&l2cap_state.channels[i]);
        }
    }

    l2cap_state.initialized = 0;
}
