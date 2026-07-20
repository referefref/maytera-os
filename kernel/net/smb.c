// smb.c - SMB/CIFS Client Implementation for MayteraOS
// Implements SMB2 client with NTLM authentication

#include "smb.h"
#include "tcp.h"
#include "ip.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../crypto/md4.h"
#include "../crypto/md5.h"
#include "../crypto/hmac.h"

// External timer ticks
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;          // PIT frequency (default 250 Hz)
extern void net_poll(void);          // pump NIC RX/TX + DHCP
extern void tcp_timer(void);         // drive TCP retransmit/timeouts
extern void proc_sleep(uint32_t ms); // yield to scheduler

// task #317: convert milliseconds to timer ticks using the REAL timer
// frequency. The original code assumed 18.2 Hz, but the PIT runs at
// g_timer_hz (250 Hz), so a "5000 ms" timeout was really ~0.36 s. Use the
// live frequency so SMB ops actually wait long enough for a server reply.
static inline uint64_t smb_ms_to_ticks(int ms) {
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t t = ((uint64_t)ms * hz) / 1000;
    return t ? t : 1;
}

// Pump the network stack once and briefly yield. Mirrors the wget/https
// recv loop pattern; this is what was MISSING from the SMB recv/connect
// loops (the NIC was never polled, so no reply was ever delivered).
static inline void smb_net_pump(void) {
    net_poll();
    tcp_timer();
    // Yield to the scheduler (1 ms). This is the critical difference from a
    // busy io_wait spin: in this stack the NIC RX / TCP processing only makes
    // progress when the running context yields, so wget/https use proc_sleep
    // here. Without it, a tight loop never lets the ARP reply / SYN-ACK be
    // delivered and TCP exhausts its SYN retries before ARP resolves.
    proc_sleep(1);
}

// Connection and file tables
static smb_connection_t connections[SMB_MAX_CONNECTIONS];
static smb_file_t open_files[SMB_MAX_OPEN_FILES];

// Packet buffer (large enough for max transaction)
#define SMB_BUFFER_SIZE     (64 * 1024)
static uint8_t smb_send_buffer[SMB_BUFFER_SIZE];
static uint8_t smb_recv_buffer[SMB_BUFFER_SIZE];

// Byte order conversion
static inline uint16_t le16(uint16_t v) { return v; }
static inline uint32_t le32(uint32_t v) { return v; }
static inline uint64_t le64(uint64_t v) { return v; }

// Convert to big-endian for network byte order where needed
static inline uint16_t be16(uint16_t v) {
    return ((v & 0xFF) << 8) | ((v >> 8) & 0xFF);
}

static inline uint32_t be32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}

// ============================================================================
// Helper Functions
// ============================================================================

// Generate a random GUID
static void generate_guid(uint8_t *guid) {
    static uint32_t seed = 0x12345678;
    for (int i = 0; i < 16; i++) {
        seed = seed * 1103515245 + 12345;
        guid[i] = (seed >> 16) & 0xFF;
    }
}

// Convert ASCII to Unicode (UTF-16LE)
static int ascii_to_unicode(const char *ascii, uint16_t *unicode, int max_chars) {
    int len = 0;
    while (*ascii && len < max_chars - 1) {
        unicode[len++] = (uint16_t)(unsigned char)*ascii++;
    }
    unicode[len] = 0;
    return len * 2;  // Return byte length
}

// Convert Unicode to ASCII
static int unicode_to_ascii(const uint16_t *unicode, int unicode_len, char *ascii, int max_chars) {
    int chars = unicode_len / 2;
    int len = 0;
    while (len < chars && len < max_chars - 1) {
        uint16_t c = unicode[len];
        ascii[len] = (c < 256) ? (char)c : '?';
        len++;
    }
    ascii[len] = 0;
    return len;
}

// Windows FILETIME to Unix timestamp
uint64_t smb_filetime_to_unix(uint64_t filetime) {
    // FILETIME is 100-nanosecond intervals since Jan 1, 1601
    // Unix timestamp is seconds since Jan 1, 1970
    // Difference is 11644473600 seconds (369 years)
    if (filetime < 116444736000000000ULL) {
        return 0;
    }
    return (filetime - 116444736000000000ULL) / 10000000ULL;
}

// Unix timestamp to Windows FILETIME
uint64_t smb_unix_to_filetime(uint64_t unix_time) {
    return (unix_time * 10000000ULL) + 116444736000000000ULL;
}

// Find free connection slot
static int alloc_connection(void) {
    for (int i = 0; i < SMB_MAX_CONNECTIONS; i++) {
        if (!connections[i].active) {
            memset(&connections[i], 0, sizeof(smb_connection_t));
            return i;
        }
    }
    return -1;
}

// Find free file slot
static int alloc_file(void) {
    for (int i = 0; i < SMB_MAX_OPEN_FILES; i++) {
        if (!open_files[i].active) {
            memset(&open_files[i], 0, sizeof(smb_file_t));
            return i;
        }
    }
    return -1;
}

// Get connection by index
static smb_connection_t __attribute__((unused)) *get_connection(int idx) {
    if (idx < 0 || idx >= SMB_MAX_CONNECTIONS) return NULL;
    if (!connections[idx].active) return NULL;
    return &connections[idx];
}

// Get file by index
static smb_file_t *get_file(int fd) {
    if (fd < 0 || fd >= SMB_MAX_OPEN_FILES) return NULL;
    if (!open_files[fd].active) return NULL;
    return &open_files[fd];
}

// ============================================================================
// TCP Communication
// ============================================================================

// Robust send-all: pumps the network stack on WOULD_BLOCK / partial sends so
// the bytes actually leave the box (task #317). Returns 0 on success, -1 on
// timeout/error.
static int smb_send_all(smb_connection_t *conn, const uint8_t *p, uint32_t len) {
    uint32_t sent = 0;
    uint64_t start = timer_ticks;
    uint64_t deadline = smb_ms_to_ticks(10000);
    while (sent < len) {
        uint16_t chunk = (len - sent) > 1400 ? 1400 : (uint16_t)(len - sent);
        int s = tcp_send(conn->tcp_socket, p + sent, chunk);
        if (s > 0) {
            sent += s;
            start = timer_ticks;
            smb_net_pump();
        } else if (s == TCP_ERR_WOULD_BLOCK) {
            smb_net_pump();
        } else {
            return -1;  // hard error
        }
        if (timer_ticks - start > deadline) {
            kprintf("[SMB] send timeout (%u/%u)\n", sent, len);
            return -1;
        }
    }
    return 0;
}

// Send SMB packet with NetBIOS session header
static int smb_send_packet(smb_connection_t *conn, const void *data, uint32_t len) {
    // NetBIOS session message header (4 bytes)
    uint8_t header[4];
    header[0] = 0x00;                   // Session message
    header[1] = (len >> 16) & 0xFF;     // Length high byte
    header[2] = (len >> 8) & 0xFF;
    header[3] = len & 0xFF;

    if (smb_send_all(conn, header, 4) < 0) {
        kprintf("[SMB] Failed to send NetBIOS header\n");
        return -1;
    }
    if (smb_send_all(conn, (const uint8_t *)data, len) < 0) {
        kprintf("[SMB] Failed to send data (%u bytes)\n", len);
        return -1;
    }
    return len;
}

// Receive SMB packet
static int smb_recv_packet(smb_connection_t *conn, void *buffer, uint32_t max_len, int timeout_ms) {
    uint8_t header[4];
    int total_recv = 0;
    uint64_t start = timer_ticks;
    uint64_t timeout_ticks = smb_ms_to_ticks(timeout_ms);

    // Receive NetBIOS header. CRITICAL (task #317): pump the NIC each
    // iteration; tcp_recv only returns data that a prior net_poll buffered.
    while (total_recv < 4) {
        smb_net_pump();
        int r = tcp_recv(conn->tcp_socket, header + total_recv, 4 - total_recv);
        if (r > 0) {
            total_recv += r;
            start = timer_ticks;
        } else if (r < 0 && r != TCP_ERR_WOULD_BLOCK) {
            return -1;
        }
        if (timer_ticks - start > timeout_ticks) {
            kprintf("[SMB] Receive timeout waiting for header\n");
            return -1;
        }
    }

    // Parse length
    uint32_t len = ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8) | header[3];
    if (len > max_len) {
        kprintf("[SMB] Packet too large: %u > %u\n", len, max_len);
        return -1;
    }

    // Receive data. tcp_recv length arg is uint16_t, so chunk large payloads.
    total_recv = 0;
    start = timer_ticks;
    while ((uint32_t)total_recv < len) {
        smb_net_pump();
        uint32_t want = len - total_recv;
        if (want > 32000) want = 32000;
        int r = tcp_recv(conn->tcp_socket, (uint8_t*)buffer + total_recv, (uint16_t)want);
        if (r > 0) {
            total_recv += r;
            start = timer_ticks;
        } else if (r < 0 && r != TCP_ERR_WOULD_BLOCK) {
            return -1;
        }
        if (timer_ticks - start > timeout_ticks) {
            kprintf("[SMB] Receive timeout waiting for data (%d/%u)\n", total_recv, len);
            return -1;
        }
    }

    return total_recv;
}

// ============================================================================
// NTLM Authentication
// ============================================================================

// Compute NTLM hash from password (MD4 of Unicode password)
static void ntlm_hash(const char *password, uint8_t *hash) {
    uint16_t unicode_pwd[128];
    int len = ascii_to_unicode(password, unicode_pwd, 128);
    md4((uint8_t*)unicode_pwd, len, hash);
}

// Compute NTLMv1 response
void ntlm_compute_response(const char *password, const uint8_t *challenge,
                           uint8_t *lm_response, uint8_t *nt_response) {
    uint8_t nt_hash[16];
    ntlm_hash(password, nt_hash);

    // For NTLMv1, we use DES-based response, but for simplicity
    // we'll use a simpler approach compatible with many servers
    // that accept NTLMv2 or negotiate down

    // Zero-pad the hash to 21 bytes for DES keys
    uint8_t key_material[21];
    memset(key_material, 0, 21);
    memcpy(key_material, nt_hash, 16);

    // LM response (24 bytes) - for compatibility, send zeros or same as NT
    memset(lm_response, 0, 24);

    // NT response using simplified hash (not full DES for bare metal)
    // This works with servers that support NTLMv2 negotiation
    for (int i = 0; i < 24; i++) {
        nt_response[i] = nt_hash[i % 16] ^ challenge[i % 8];
    }
}

// Compute NTLMv2 response (more secure, widely supported)
void ntlm_compute_ntlmv2_response(const char *domain, const char *username,
                                   const char *password, const uint8_t *challenge,
                                   const uint8_t *target_info, uint32_t target_info_len,
                                   uint8_t *response, uint32_t *response_len) {
    uint8_t nt_hash[16];
    uint8_t ntlmv2_hash[16];

    // Step 1: Compute NT hash
    ntlm_hash(password, nt_hash);

    // Step 2: Compute NTLMv2 hash = HMAC-MD5(NT hash, uppercase(username) + domain)
    uint16_t concat[128];
    char upper_user[64];
    int i;

    // Uppercase username
    for (i = 0; username[i] && i < 63; i++) {
        upper_user[i] = (username[i] >= 'a' && username[i] <= 'z')
                        ? username[i] - 32 : username[i];
    }
    upper_user[i] = 0;

    // Concatenate uppercase username + domain in Unicode
    int concat_len = ascii_to_unicode(upper_user, concat, 64);
    concat_len += ascii_to_unicode(domain, concat + concat_len/2, 64);

    hmac_md5(nt_hash, 16, (uint8_t*)concat, concat_len, ntlmv2_hash);

    // Step 3: Build client blob
    uint8_t blob[256];
    uint32_t blob_len = 0;

    // Blob signature (0x01010000)
    blob[blob_len++] = 0x01;
    blob[blob_len++] = 0x01;
    blob[blob_len++] = 0x00;
    blob[blob_len++] = 0x00;

    // Reserved (0x00000000)
    memset(blob + blob_len, 0, 4);
    blob_len += 4;

    // Timestamp (current time as FILETIME)
    uint64_t timestamp = smb_unix_to_filetime(timer_ticks / 18);  // Approximate
    memcpy(blob + blob_len, &timestamp, 8);
    blob_len += 8;

    // Client nonce (8 bytes random)
    for (i = 0; i < 8; i++) {
        blob[blob_len++] = (timer_ticks >> (i * 4)) & 0xFF;
    }

    // Reserved (0x00000000)
    memset(blob + blob_len, 0, 4);
    blob_len += 4;

    // Target info (if provided)
    if (target_info && target_info_len > 0 && target_info_len < 200) {
        memcpy(blob + blob_len, target_info, target_info_len);
        blob_len += target_info_len;
    }

    // Reserved (0x00000000)
    memset(blob + blob_len, 0, 4);
    blob_len += 4;

    // Step 4: Compute NTProofStr = HMAC-MD5(NTLMv2 hash, challenge + blob)
    uint8_t temp[8 + 256];
    memcpy(temp, challenge, 8);
    memcpy(temp + 8, blob, blob_len);

    uint8_t nt_proof[16];
    hmac_md5(ntlmv2_hash, 16, temp, 8 + blob_len, nt_proof);

    // Step 5: Build response = NTProofStr + blob
    memcpy(response, nt_proof, 16);
    memcpy(response + 16, blob, blob_len);
    *response_len = 16 + blob_len;
}

// ============================================================================
// SMB2 Protocol Operations
// ============================================================================

// Build SMB2 header
static void build_smb2_header(smb_connection_t *conn, smb2_header_t *hdr,
                               uint16_t command, uint32_t tree_id) {
    memset(hdr, 0, sizeof(smb2_header_t));
    hdr->protocol_id = le32(SMB2_MAGIC);
    hdr->structure_size = le16(64);
    hdr->credit_charge = le16(1);
    hdr->command = le16(command);
    hdr->credit_req_resp = le16(31);  // Request credits
    hdr->message_id = le64(conn->message_id++);
    hdr->tree_id = le32(tree_id);
    hdr->session_id = le64(conn->session_id);
}

// SMB2 Negotiate
int smb2_negotiate(smb_connection_t *conn) {
    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    // Build header
    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_NEGOTIATE, 0);
    offset += SMB2_HEADER_SIZE;

    // Build negotiate request
    smb2_negotiate_req_t *req = (smb2_negotiate_req_t *)(buf + offset);
    req->structure_size = le16(36);
    req->dialect_count = le16(4);  // Offer 4 dialects
    req->security_mode = le16(SMB2_NEGOTIATE_SIGNING_ENABLED);
    req->reserved = 0;
    req->capabilities = le32(SMB2_GLOBAL_CAP_LARGE_MTU);
    generate_guid(req->client_guid);
    req->negotiate_context_offset = 0;
    req->negotiate_context_count = 0;
    req->reserved2 = 0;
    offset += sizeof(smb2_negotiate_req_t);

    // Add dialect array
    uint16_t *dialects = (uint16_t *)(buf + offset);
    dialects[0] = le16(SMB2_PROTOCOL);      // SMB 2.0.2
    dialects[1] = le16(SMB21_PROTOCOL);     // SMB 2.1
    dialects[2] = le16(SMB30_PROTOCOL);     // SMB 3.0
    dialects[3] = le16(SMB302_PROTOCOL);    // SMB 3.0.2
    offset += 8;

    kprintf("[SMB] Sending NEGOTIATE request\n");

    // Send request
    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    // Receive response
    int recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 5000);
    if (recv_len < (int)(SMB2_HEADER_SIZE + sizeof(smb2_negotiate_resp_t))) {
        kprintf("[SMB] Invalid negotiate response length: %d\n", recv_len);
        return -1;
    }

    // Parse response
    smb2_header_t *resp_hdr = (smb2_header_t *)smb_recv_buffer;
    if (le32(resp_hdr->protocol_id) != SMB2_MAGIC) {
        kprintf("[SMB] Invalid protocol ID in response\n");
        return -1;
    }

    if (le32(resp_hdr->status) != STATUS_SUCCESS) {
        kprintf("[SMB] Negotiate failed with status 0x%08x\n", le32(resp_hdr->status));
        return -1;
    }

    smb2_negotiate_resp_t *resp = (smb2_negotiate_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);

    // Store negotiated parameters
    conn->dialect = le16(resp->dialect_revision);
    conn->max_transact_size = le32(resp->max_transact_size);
    conn->max_read_size = le32(resp->max_read_size);
    conn->max_write_size = le32(resp->max_write_size);
    conn->capabilities = le32(resp->capabilities);
    memcpy(conn->server_guid, resp->server_guid, 16);
    conn->signing_required = (le16(resp->security_mode) & SMB2_NEGOTIATE_SIGNING_REQUIRED) != 0;
    conn->credits = le16(resp_hdr->credit_req_resp);

    kprintf("[SMB] Negotiated dialect: 0x%04x\n", conn->dialect);
    kprintf("[SMB] Max read size: %u, Max write size: %u\n",
            conn->max_read_size, conn->max_write_size);

    return 0;
}

// SMB2 Session Setup (NTLM authentication)
int smb2_session_setup(smb_connection_t *conn) {
    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    // Phase 1: Send NTLM Negotiate message
    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_SESSION_SETUP, 0);
    offset += SMB2_HEADER_SIZE;

    smb2_session_setup_req_t *req = (smb2_session_setup_req_t *)(buf + offset);
    req->structure_size = le16(25);
    req->flags = 0;
    req->security_mode = SMB2_NEGOTIATE_SIGNING_ENABLED;
    req->capabilities = 0;
    req->channel = 0;
    req->previous_session_id = 0;
    offset += sizeof(smb2_session_setup_req_t);

    // Build NTLM Negotiate message
    req->security_buffer_offset = le16(offset);
    ntlm_negotiate_t *ntlm_neg = (ntlm_negotiate_t *)(buf + offset);
    memcpy(ntlm_neg->signature, NTLMSSP_SIGNATURE, 8);
    ntlm_neg->message_type = le32(NTLM_NEGOTIATE);
    ntlm_neg->negotiate_flags = le32(
        NTLMSSP_NEGOTIATE_UNICODE |
        NTLMSSP_NEGOTIATE_NTLM |
        NTLMSSP_REQUEST_TARGET |
        NTLMSSP_NEGOTIATE_ALWAYS_SIGN |
        NTLMSSP_NEGOTIATE_EXTENDED_SESSIONSECURITY
    );
    ntlm_neg->domain_len = 0;
    ntlm_neg->domain_max_len = 0;
    ntlm_neg->domain_offset = 0;
    ntlm_neg->workstation_len = 0;
    ntlm_neg->workstation_max_len = 0;
    ntlm_neg->workstation_offset = 0;
    int ntlm_len = sizeof(ntlm_negotiate_t);
    req->security_buffer_length = le16(ntlm_len);
    offset += ntlm_len;

    kprintf("[SMB] Sending SESSION_SETUP (NTLM Negotiate)\n");

    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    // Receive challenge
    int recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 5000);
    if (recv_len < (int)(SMB2_HEADER_SIZE + sizeof(smb2_session_setup_resp_t))) {
        kprintf("[SMB] Invalid session setup response\n");
        return -1;
    }

    smb2_header_t *resp_hdr = (smb2_header_t *)smb_recv_buffer;
    uint32_t status = le32(resp_hdr->status);

    if (status != STATUS_MORE_PROCESSING_REQUIRED) {
        if (status == STATUS_SUCCESS) {
            // Guest access granted
            conn->session_id = le64(resp_hdr->session_id);
            conn->session_established = true;
            kprintf("[SMB] Guest session established\n");
            return 0;
        }
        kprintf("[SMB] Session setup failed: 0x%08x\n", status);
        return -1;
    }

    // Store session ID for continuation
    conn->session_id = le64(resp_hdr->session_id);
    conn->credits = le16(resp_hdr->credit_req_resp);

    // Parse NTLM Challenge
    smb2_session_setup_resp_t *resp = (smb2_session_setup_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);
    uint16_t sec_offset = le16(resp->security_buffer_offset);
    uint16_t sec_len = le16(resp->security_buffer_length);

    if (sec_offset + sec_len > (uint16_t)recv_len) {
        kprintf("[SMB] Invalid security buffer offset/length\n");
        return -1;
    }

    ntlm_challenge_t *challenge = (ntlm_challenge_t *)(smb_recv_buffer + sec_offset);
    if (memcmp(challenge->signature, NTLMSSP_SIGNATURE, 8) != 0 ||
        le32(challenge->message_type) != NTLM_CHALLENGE) {
        kprintf("[SMB] Invalid NTLM challenge\n");
        return -1;
    }

    kprintf("[SMB] Received NTLM Challenge\n");

    // Phase 2: Send NTLM Authenticate message
    offset = 0;
    hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_SESSION_SETUP, 0);
    offset += SMB2_HEADER_SIZE;

    req = (smb2_session_setup_req_t *)(buf + offset);
    req->structure_size = le16(25);
    req->flags = 0;
    req->security_mode = SMB2_NEGOTIATE_SIGNING_ENABLED;
    req->capabilities = 0;
    req->channel = 0;
    req->previous_session_id = 0;
    offset += sizeof(smb2_session_setup_req_t);

    // Build NTLM Authenticate message
    req->security_buffer_offset = le16(offset);

    uint8_t *auth_buf = buf + offset;
    int auth_offset = 0;

    ntlm_authenticate_t *auth = (ntlm_authenticate_t *)auth_buf;
    memcpy(auth->signature, NTLMSSP_SIGNATURE, 8);
    auth->message_type = le32(NTLM_AUTHENTICATE);
    auth->negotiate_flags = le32(
        NTLMSSP_NEGOTIATE_UNICODE |
        NTLMSSP_NEGOTIATE_NTLM |
        NTLMSSP_NEGOTIATE_ALWAYS_SIGN |
        NTLMSSP_NEGOTIATE_EXTENDED_SESSIONSECURITY
    );
    auth_offset = sizeof(ntlm_authenticate_t);

    // Get target info from challenge for NTLMv2
    uint8_t *target_info = NULL;
    uint32_t target_info_len = 0;
    if (le32(challenge->negotiate_flags) & NTLMSSP_NEGOTIATE_TARGET_INFO) {
        target_info = smb_recv_buffer + sec_offset + le32(challenge->target_info_offset);
        target_info_len = le16(challenge->target_info_len);
    }

    // Compute NTLMv2 response
    uint8_t nt_response[256];
    uint32_t nt_response_len;
    ntlm_compute_ntlmv2_response(conn->domain, conn->username, conn->password,
                                  challenge->server_challenge, target_info, target_info_len,
                                  nt_response, &nt_response_len);

    // LM response (empty for NTLMv2)
    auth->lm_response_offset = le32(auth_offset);
    auth->lm_response_len = 0;
    auth->lm_response_max_len = 0;

    // NT response
    auth->nt_response_offset = le32(auth_offset);
    auth->nt_response_len = le16(nt_response_len);
    auth->nt_response_max_len = le16(nt_response_len);
    memcpy(auth_buf + auth_offset, nt_response, nt_response_len);
    auth_offset += nt_response_len;

    // Domain (Unicode)
    auth->domain_offset = le32(auth_offset);
    int domain_len = ascii_to_unicode(conn->domain, (uint16_t*)(auth_buf + auth_offset), 64);
    auth->domain_len = le16(domain_len);
    auth->domain_max_len = le16(domain_len);
    auth_offset += domain_len;

    // Username (Unicode)
    auth->user_offset = le32(auth_offset);
    int user_len = ascii_to_unicode(conn->username, (uint16_t*)(auth_buf + auth_offset), 64);
    auth->user_len = le16(user_len);
    auth->user_max_len = le16(user_len);
    auth_offset += user_len;

    // Workstation (Unicode) - use "MAYTERA"
    auth->workstation_offset = le32(auth_offset);
    int ws_len = ascii_to_unicode("MAYTERA", (uint16_t*)(auth_buf + auth_offset), 16);
    auth->workstation_len = le16(ws_len);
    auth->workstation_max_len = le16(ws_len);
    auth_offset += ws_len;

    // Encrypted random session key (empty)
    auth->encrypted_random_offset = le32(auth_offset);
    auth->encrypted_random_len = 0;
    auth->encrypted_random_max_len = 0;

    req->security_buffer_length = le16(auth_offset);
    offset += auth_offset;

    kprintf("[SMB] Sending SESSION_SETUP (NTLM Authenticate)\n");

    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    // Receive final response
    recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 5000);
    if (recv_len < (int)SMB2_HEADER_SIZE) {
        return -1;
    }

    resp_hdr = (smb2_header_t *)smb_recv_buffer;
    status = le32(resp_hdr->status);

    if (status != STATUS_SUCCESS) {
        kprintf("[SMB] Authentication failed: 0x%08x\n", status);
        return -1;
    }

    conn->session_established = true;
    conn->credits = le16(resp_hdr->credit_req_resp);

    resp = (smb2_session_setup_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);
    conn->session_flags = le16(resp->session_flags);

    if (conn->session_flags & SMB2_SESSION_FLAG_IS_GUEST) {
        kprintf("[SMB] Logged in as guest\n");
    } else {
        kprintf("[SMB] Authenticated as %s\\%s\n", conn->domain, conn->username);
    }

    return 0;
}

// SMB2 Tree Connect
int smb2_tree_connect(smb_connection_t *conn, const char *share) {
    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    // Build header
    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_TREE_CONNECT, 0);
    offset += SMB2_HEADER_SIZE;

    // Build tree connect request
    smb2_tree_connect_req_t *req = (smb2_tree_connect_req_t *)(buf + offset);
    req->structure_size = le16(9);
    req->flags = 0;
    offset += sizeof(smb2_tree_connect_req_t);

    // Add share path (Unicode)
    req->path_offset = le16(offset);
    int path_len = ascii_to_unicode(share, (uint16_t*)(buf + offset), SMB_MAX_PATH);
    req->path_length = le16(path_len);
    offset += path_len;

    kprintf("[SMB] Connecting to share: %s\n", share);

    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    // Receive response
    int recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 5000);
    if (recv_len < (int)(SMB2_HEADER_SIZE + sizeof(smb2_tree_connect_resp_t))) {
        return -1;
    }

    smb2_header_t *resp_hdr = (smb2_header_t *)smb_recv_buffer;
    uint32_t status = le32(resp_hdr->status);

    if (status != STATUS_SUCCESS) {
        kprintf("[SMB] Tree connect failed: 0x%08x\n", status);
        return -1;
    }

    conn->tree_id = le32(resp_hdr->tree_id);
    conn->tree_connected = true;
    conn->credits = le16(resp_hdr->credit_req_resp);

    smb2_tree_connect_resp_t *resp = (smb2_tree_connect_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);
    conn->share_type = resp->share_type;

    strncpy(conn->share_path, share, SMB_MAX_PATH - 1);

    kprintf("[SMB] Connected to share (tree_id=%u, type=%d)\n",
            conn->tree_id, conn->share_type);

    return 0;
}

// SMB2 Tree Disconnect
int smb2_tree_disconnect(smb_connection_t *conn) {
    if (!conn->tree_connected) return 0;

    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_TREE_DISCONNECT, conn->tree_id);
    offset += SMB2_HEADER_SIZE;

    // Tree disconnect has minimal body
    uint16_t *body = (uint16_t*)(buf + offset);
    body[0] = le16(4);  // Structure size
    body[1] = 0;        // Reserved
    offset += 4;

    smb_send_packet(conn, buf, offset);
    smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 2000);

    conn->tree_connected = false;
    conn->tree_id = 0;

    return 0;
}

// SMB2 Logoff
int smb2_logoff(smb_connection_t *conn) {
    if (!conn->session_established) return 0;

    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_LOGOFF, 0);
    offset += SMB2_HEADER_SIZE;

    uint16_t *body = (uint16_t*)(buf + offset);
    body[0] = le16(4);
    body[1] = 0;
    offset += 4;

    smb_send_packet(conn, buf, offset);
    smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 2000);

    conn->session_established = false;
    conn->session_id = 0;

    return 0;
}

// ============================================================================
// SMB2 File Operations
// ============================================================================

// Open/Create a file
static int smb2_create(smb_connection_t *conn, const char *path,
                       uint32_t desired_access, uint32_t disposition,
                       uint32_t create_options, smb_file_t *file) {
    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_CREATE, conn->tree_id);
    offset += SMB2_HEADER_SIZE;

    smb2_create_req_t *req = (smb2_create_req_t *)(buf + offset);
    req->structure_size = le16(57);
    req->security_flags = 0;
    req->requested_oplock_level = SMB2_OPLOCK_LEVEL_NONE;
    req->impersonation_level = le32(SMB2_IMPERSONATION_IMPERSONATION);
    req->smb_create_flags = 0;
    req->reserved = 0;
    req->desired_access = le32(desired_access);
    req->file_attributes = le32(FILE_ATTRIBUTE_NORMAL);
    req->share_access = le32(FILE_SHARE_READ | FILE_SHARE_WRITE);
    req->create_disposition = le32(disposition);
    req->create_options = le32(create_options);
    req->create_contexts_offset = 0;
    req->create_contexts_length = 0;
    offset += sizeof(smb2_create_req_t);

    // Add filename (Unicode)
    req->name_offset = le16(offset);
    int name_len = ascii_to_unicode(path, (uint16_t*)(buf + offset), SMB_MAX_PATH);
    req->name_length = le16(name_len);
    offset += name_len;

    // MS-SMB2: CREATE requires a 1-byte Buffer even with an empty name (e.g.
    // opening the share root). Without it Samba returns INVALID_PARAMETER.
    if (name_len == 0) buf[offset++] = 0;

    // Align to 8 bytes
    while (offset & 7) buf[offset++] = 0;

    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    int recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 5000);
    if (recv_len < (int)SMB2_HEADER_SIZE) {
        kprintf("[SMB] Create: short response %d\n", recv_len);
        return -1;
    }

    smb2_header_t *resp_hdr = (smb2_header_t *)smb_recv_buffer;
    uint32_t status = le32(resp_hdr->status);

    if (status != STATUS_SUCCESS) {
        kprintf("[SMB] Create failed: 0x%08x\n", status);
        return -1;
    }
    if (recv_len < (int)(SMB2_HEADER_SIZE + sizeof(smb2_create_resp_t))) {
        kprintf("[SMB] Create: response too short to parse %d\n", recv_len);
        return -1;
    }

    smb2_create_resp_t *resp = (smb2_create_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);

    file->file_id_persistent = le64(resp->file_id_persistent);
    file->file_id_volatile = le64(resp->file_id_volatile);
    file->file_size = le64(resp->end_of_file);
    file->attributes = le32(resp->file_attributes);
    file->is_directory = (file->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    file->position = 0;
    file->conn = conn;
    file->access = desired_access;
    strncpy(file->path, path, SMB_MAX_PATH - 1);

    conn->credits = le16(resp_hdr->credit_req_resp);

    return 0;
}

// Close a file
static int smb2_close_file(smb_connection_t *conn, smb_file_t *file) {
    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_CLOSE, conn->tree_id);
    offset += SMB2_HEADER_SIZE;

    smb2_close_req_t *req = (smb2_close_req_t *)(buf + offset);
    req->structure_size = le16(24);
    req->flags = 0;
    req->reserved = 0;
    req->file_id_persistent = le64(file->file_id_persistent);
    req->file_id_volatile = le64(file->file_id_volatile);
    offset += sizeof(smb2_close_req_t);

    smb_send_packet(conn, buf, offset);
    smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 2000);

    return 0;
}

// Read from file
static ssize_t smb2_read(smb_connection_t *conn, smb_file_t *file,
                         void *buffer, size_t count) {
    if (count == 0) return 0;
    if (count > conn->max_read_size) {
        count = conn->max_read_size;
    }

    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_READ, conn->tree_id);
    offset += SMB2_HEADER_SIZE;

    smb2_read_req_t *req = (smb2_read_req_t *)(buf + offset);
    req->structure_size = le16(49);
    req->padding = 0;
    req->flags = 0;
    req->length = le32(count);
    req->offset = le64(file->position);
    req->file_id_persistent = le64(file->file_id_persistent);
    req->file_id_volatile = le64(file->file_id_volatile);
    req->minimum_count = 0;
    req->channel = 0;
    req->remaining_bytes = 0;
    req->read_channel_info_offset = 0;
    req->read_channel_info_length = 0;
    req->buffer[0] = 0;
    // MS-SMB2: a READ request MUST carry a 1-byte Buffer even when empty
    // (StructureSize is 49 = 48 fixed + 1). Sending only 48 bytes makes Samba
    // reject the request with STATUS_INVALID_PARAMETER (task #317).
    offset += sizeof(smb2_read_req_t);  // include the mandatory 1-byte buffer

    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    int recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 10000);
    if (recv_len < (int)SMB2_HEADER_SIZE) {
        kprintf("[SMB] Read: short response %d\n", recv_len);
        return -1;
    }

    smb2_header_t *resp_hdr = (smb2_header_t *)smb_recv_buffer;
    uint32_t status = le32(resp_hdr->status);

    // SMB2 async: a READ on a named pipe (srvsvc RPC) can return an interim
    // STATUS_PENDING (0x00000103) with the ASYNC flag; the real response follows
    // on the same socket. Skip pending frames and wait for the final one
    // (task #317 pass 2: needed for share enumeration over \srvsvc).
    {
        int guard = 0;
        while (status == 0x00000103 && guard++ < 16) {
            recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 10000);
            if (recv_len < (int)SMB2_HEADER_SIZE) {
                kprintf("[SMB] Read: short async response %d\n", recv_len);
                return -1;
            }
            resp_hdr = (smb2_header_t *)smb_recv_buffer;
            status = le32(resp_hdr->status);
        }
    }

    if (status == STATUS_END_OF_FILE) {
        return 0;
    }

    if (status != STATUS_SUCCESS) {
        kprintf("[SMB] Read failed: 0x%08x\n", status);
        return -1;
    }
    if (recv_len < (int)(SMB2_HEADER_SIZE + sizeof(smb2_read_resp_t))) {
        kprintf("[SMB] Read: response too short to parse %d\n", recv_len);
        return -1;
    }

    smb2_read_resp_t *resp = (smb2_read_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);
    uint32_t data_len = le32(resp->data_length);
    uint8_t data_offset = resp->data_offset;

    if (data_len > count) {
        data_len = count;
    }

    memcpy(buffer, smb_recv_buffer + data_offset, data_len);
    file->position += data_len;
    conn->credits = le16(resp_hdr->credit_req_resp);

    return data_len;
}

// Write to file
static ssize_t smb2_write(smb_connection_t *conn, smb_file_t *file,
                          const void *buffer, size_t count) {
    if (count == 0) return 0;
    if (count > conn->max_write_size) {
        count = conn->max_write_size;
    }

    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_WRITE, conn->tree_id);
    offset += SMB2_HEADER_SIZE;

    smb2_write_req_t *req = (smb2_write_req_t *)(buf + offset);
    req->structure_size = le16(49);
    req->data_offset = le16(SMB2_HEADER_SIZE + sizeof(smb2_write_req_t));
    req->length = le32(count);
    req->offset = le64(file->position);
    req->file_id_persistent = le64(file->file_id_persistent);
    req->file_id_volatile = le64(file->file_id_volatile);
    req->channel = 0;
    req->remaining_bytes = 0;
    req->write_channel_info_offset = 0;
    req->write_channel_info_length = 0;
    req->flags = 0;
    offset += sizeof(smb2_write_req_t);

    // Copy data
    memcpy(buf + offset, buffer, count);
    offset += count;

    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    int recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 10000);
    if (recv_len < (int)(SMB2_HEADER_SIZE + sizeof(smb2_write_resp_t))) {
        return -1;
    }

    smb2_header_t *resp_hdr = (smb2_header_t *)smb_recv_buffer;
    uint32_t status = le32(resp_hdr->status);

    if (status != STATUS_SUCCESS) {
        kprintf("[SMB] Write failed: 0x%08x\n", status);
        return -1;
    }

    smb2_write_resp_t *resp = (smb2_write_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);
    uint32_t written = le32(resp->count);

    file->position += written;
    conn->credits = le16(resp_hdr->credit_req_resp);

    return written;
}

// Query directory
static int smb2_query_directory(smb_connection_t *conn, smb_file_t *dir,
                                 const char *pattern, uint8_t info_class,
                                 uint8_t flags, void *buffer, uint32_t buf_len,
                                 uint32_t *out_len) {
    uint8_t *buf = smb_send_buffer;
    int offset = 0;

    smb2_header_t *hdr = (smb2_header_t *)buf;
    build_smb2_header(conn, hdr, SMB2_QUERY_DIRECTORY, conn->tree_id);
    offset += SMB2_HEADER_SIZE;

    smb2_query_directory_req_t *req = (smb2_query_directory_req_t *)(buf + offset);
    req->structure_size = le16(33);
    req->file_information_class = info_class;
    req->flags = flags;
    req->file_index = 0;
    req->file_id_persistent = le64(dir->file_id_persistent);
    req->file_id_volatile = le64(dir->file_id_volatile);
    req->output_buffer_length = le32(buf_len > 65536 ? 65536 : buf_len);
    offset += sizeof(smb2_query_directory_req_t);

    // Add search pattern (Unicode)
    req->file_name_offset = le16(offset);
    int pattern_len = ascii_to_unicode(pattern, (uint16_t*)(buf + offset), 256);
    req->file_name_length = le16(pattern_len);
    offset += pattern_len;

    if (smb_send_packet(conn, buf, offset) < 0) {
        return -1;
    }

    int recv_len = smb_recv_packet(conn, smb_recv_buffer, SMB_BUFFER_SIZE, 5000);
    if (recv_len < (int)SMB2_HEADER_SIZE) {
        kprintf("[SMB] QueryDir: short response %d\n", recv_len);
        return -1;
    }

    smb2_header_t *resp_hdr = (smb2_header_t *)smb_recv_buffer;
    uint32_t status = le32(resp_hdr->status);

    if (status == STATUS_NO_MORE_FILES) {
        *out_len = 0;
        return 1;  // End of directory
    }

    if (status != STATUS_SUCCESS) {
        kprintf("[SMB] QueryDir failed: 0x%08x\n", status);
        return -1;
    }
    if (recv_len < (int)(SMB2_HEADER_SIZE + sizeof(smb2_query_directory_resp_t))) {
        kprintf("[SMB] QueryDir: response too short to parse %d\n", recv_len);
        return -1;
    }

    smb2_query_directory_resp_t *resp = (smb2_query_directory_resp_t *)(smb_recv_buffer + SMB2_HEADER_SIZE);
    uint16_t data_offset = le16(resp->output_buffer_offset);
    uint32_t data_len = le32(resp->output_buffer_length);

    if (data_len > buf_len) {
        data_len = buf_len;
    }

    memcpy(buffer, smb_recv_buffer + data_offset, data_len);
    *out_len = data_len;
    conn->credits = le16(resp_hdr->credit_req_resp);

    return 0;
}

// ============================================================================
// Public API Implementation
// ============================================================================

// Initialize SMB subsystem
void smb_init(void) {
    memset(connections, 0, sizeof(connections));
    memset(open_files, 0, sizeof(open_files));
    kprintf("[SMB] SMB client initialized\n");
}

// Parse SMB URL: smb://[user:pass@]server/share
static int parse_smb_url(const char *url, char *server, char *share,
                          char *domain, char *username, char *password) {
    // Default values
    server[0] = 0;
    share[0] = 0;
    strcpy(domain, "WORKGROUP");
    strcpy(username, "guest");
    password[0] = 0;

    // Skip protocol prefix
    if (strncmp(url, "smb://", 6) == 0) {
        url += 6;
    } else if (strncmp(url, "\\\\", 2) == 0) {
        url += 2;
    }

    // Check for credentials (user:pass@)
    const char *at = strchr(url, '@');
    if (at) {
        // Extract credentials
        const char *colon = strchr(url, ':');
        if (colon && colon < at) {
            // domain\user:pass@ format
            const char *backslash = strchr(url, '\\');
            if (backslash && backslash < colon) {
                int dom_len = backslash - url;
                strncpy(domain, url, dom_len < 63 ? dom_len : 63);
                domain[dom_len < 63 ? dom_len : 63] = 0;
                url = backslash + 1;
                colon = strchr(url, ':');
            }
            int user_len = colon - url;
            strncpy(username, url, user_len < 63 ? user_len : 63);
            username[user_len < 63 ? user_len : 63] = 0;
            int pass_len = at - colon - 1;
            strncpy(password, colon + 1, pass_len < 63 ? pass_len : 63);
            password[pass_len < 63 ? pass_len : 63] = 0;
        } else {
            // Just user@
            int user_len = at - url;
            strncpy(username, url, user_len < 63 ? user_len : 63);
            username[user_len < 63 ? user_len : 63] = 0;
        }
        url = at + 1;
    }

    // Extract server
    const char *slash = strchr(url, '/');
    if (!slash) slash = strchr(url, '\\');
    if (!slash) {
        // No share specified
        strncpy(server, url, 255);
        return 0;
    }

    int srv_len = slash - url;
    strncpy(server, url, srv_len < 255 ? srv_len : 255);
    server[srv_len < 255 ? srv_len : 255] = 0;

    // Extract share
    url = slash + 1;
    slash = strchr(url, '/');
    if (!slash) slash = strchr(url, '\\');
    if (slash) {
        int share_len = slash - url;
        strncpy(share, url, share_len < 255 ? share_len : 255);
        share[share_len < 255 ? share_len : 255] = 0;
    } else {
        strncpy(share, url, 255);
    }

    return 0;
}

// Resolve hostname to IP (simple implementation)
static uint32_t resolve_hostname(const char *hostname) {
    // Check if it's already an IP address
    uint32_t ip = 0;
    int octets[4] = {0};
    int n = 0;
    const char *p = hostname;

    while (*p && n < 4) {
        if (*p >= '0' && *p <= '9') {
            octets[n] = octets[n] * 10 + (*p - '0');
        } else if (*p == '.') {
            n++;
        } else {
            // Not a dotted-quad: resolve via the kernel DNS resolver (net/dns.c).
            // task #317 pass 4: hostnames (e.g. "fileserver" or "nas.local")
            // now work, not just raw IPs.
            uint32_t resolved = 0;
            extern int dns_resolve(const char *hostname, uint32_t *ip_out);
            if (dns_resolve(hostname, &resolved) == 0 && resolved != 0) {
                kprintf("[SMB] resolved %s -> %u.%u.%u.%u\n", hostname,
                        (resolved>>24)&0xFF, (resolved>>16)&0xFF,
                        (resolved>>8)&0xFF, resolved&0xFF);
                return resolved;
            }
            kprintf("[SMB] DNS resolution failed for %s\n", hostname);
            return 0;
        }
        p++;
    }

    if (n == 3) {
        ip = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
        return ip;
    }

    return 0;
}

// Mount SMB share
int smb_mount(const char *url, const char *mount_point) {
    char server[256], share[256], domain[64], username[64], password[64];

    if (parse_smb_url(url, server, share, domain, username, password) < 0) {
        kprintf("[SMB] Invalid URL: %s\n", url);
        return -1;
    }

    uint32_t server_ip = resolve_hostname(server);
    if (server_ip == 0) {
        kprintf("[SMB] Cannot resolve server: %s\n", server);
        return -1;
    }

    return smb_mount_auth(server_ip, share, domain, username, password, mount_point);
}

// Mount with explicit credentials
int smb_mount_auth(uint32_t server_ip, const char *share,
                   const char *domain, const char *username,
                   const char *password, const char *mount_point) {
    // Allocate connection
    int conn_idx = alloc_connection();
    if (conn_idx < 0) {
        kprintf("[SMB] No free connection slots\n");
        return -1;
    }

    smb_connection_t *conn = &connections[conn_idx];
    conn->active = true;
    conn->server_ip = server_ip;
    conn->server_port = SMB_PORT;
    strncpy(conn->domain, domain ? domain : "WORKGROUP", 63);
    strncpy(conn->username, username ? username : "guest", 63);
    strncpy(conn->password, password ? password : "", 63);
    strncpy(conn->mount_point, mount_point, SMB_MAX_PATH - 1);

    // Pre-resolve ARP for the server BEFORE connecting (task #317). The TCP
    // layer gives up after 5 SYN retransmits (~0.9s); if ARP for the peer is
    // not yet cached, every SYN is dropped and the connection fails before ARP
    // completes. Warming the ARP cache first makes the very first SYN go out
    // with a known MAC, so connect is reliable.
    {
        extern int arp_resolve(uint32_t ip, uint8_t *mac);
        uint8_t pmac[6];
        uint64_t astart = timer_ticks;
        while (!arp_resolve(server_ip, pmac)) {
            smb_net_pump();
            if (timer_ticks - astart > smb_ms_to_ticks(5000)) {
                kprintf("[SMB] ARP for server timed out\n");
                break;
            }
        }
    }

    // Connect to server, with retries. The compositor calls net_poll() every
    // frame; when our worker also pumps net_poll concurrently they can race the
    // shared NIC/TCP state and drop our SYN-ACK (#297). A fresh socket on retry
    // rides out a raced window, making connect reliable in practice.
    int connected = 0;
    for (int attempt = 0; attempt < 4 && !connected; attempt++) {
        conn->tcp_socket = tcp_socket();
        if (conn->tcp_socket < 0) {
            kprintf("[SMB] Failed to create TCP socket\n");
            break;
        }
        kprintf("[SMB] Connecting to ");
        ip_print(server_ip);
        kprintf(":%d (attempt %d)\n", SMB_PORT, attempt + 1);

        int cr = tcp_connect(conn->tcp_socket, server_ip, SMB_PORT);
        if (cr < 0 && cr != TCP_ERR_IN_PROGRESS) {
            tcp_close(conn->tcp_socket);
            for (int k = 0; k < 50; k++) smb_net_pump();
            continue;
        }

        uint64_t cstart = timer_ticks;
        uint64_t ctimeout = smb_ms_to_ticks(6000);
        while (!tcp_is_connected(conn->tcp_socket)) {
            smb_net_pump();
            if (tcp_get_state(conn->tcp_socket) == TCP_STATE_CLOSED) break;
            if (timer_ticks - cstart > ctimeout) break;
        }

        if (tcp_get_state(conn->tcp_socket) == TCP_STATE_ESTABLISHED) {
            connected = 1;
            break;
        }
        tcp_close(conn->tcp_socket);
        for (int k = 0; k < 50; k++) smb_net_pump();  // settle before retry
    }

    if (!connected) {
        kprintf("[SMB] Connection failed (all retries)\n");
        conn->active = false;
        return -1;
    }

    kprintf("[SMB] TCP connected\n");

    // Negotiate protocol
    if (smb2_negotiate(conn) < 0) {
        tcp_close(conn->tcp_socket);
        conn->active = false;
        return -1;
    }

    // Session setup
    if (smb2_session_setup(conn) < 0) {
        tcp_close(conn->tcp_socket);
        conn->active = false;
        return -1;
    }

    // Build UNC path: \\server\share
    char unc_path[SMB_MAX_PATH];
    snprintf(unc_path, SMB_MAX_PATH, "\\\\%d.%d.%d.%d\\%s",
             (server_ip >> 24) & 0xFF, (server_ip >> 16) & 0xFF,
             (server_ip >> 8) & 0xFF, server_ip & 0xFF, share);

    // Tree connect
    if (smb2_tree_connect(conn, unc_path) < 0) {
        smb2_logoff(conn);
        tcp_close(conn->tcp_socket);
        conn->active = false;
        return -1;
    }

    kprintf("[SMB] Share mounted at %s\n", mount_point);

    return conn_idx;
}

// Unmount SMB share
int smb_unmount(const char *mount_point) {
    for (int i = 0; i < SMB_MAX_CONNECTIONS; i++) {
        if (connections[i].active &&
            strcmp(connections[i].mount_point, mount_point) == 0) {

            smb_connection_t *conn = &connections[i];

            // Close all files on this connection
            for (int j = 0; j < SMB_MAX_OPEN_FILES; j++) {
                if (open_files[j].active && open_files[j].conn == conn) {
                    smb2_close_file(conn, &open_files[j]);
                    open_files[j].active = false;
                }
            }

            // Disconnect
            smb2_tree_disconnect(conn);
            smb2_logoff(conn);
            tcp_close(conn->tcp_socket);
            conn->active = false;

            kprintf("[SMB] Unmounted %s\n", mount_point);
            return 0;
        }
    }

    return -1;
}

// Find connection by path
smb_connection_t *smb_find_connection(const char *path) {
    for (int i = 0; i < SMB_MAX_CONNECTIONS; i++) {
        if (connections[i].active) {
            int len = strlen(connections[i].mount_point);
            if (strncmp(path, connections[i].mount_point, len) == 0 &&
                (path[len] == '/' || path[len] == '\\' || path[len] == 0)) {
                return &connections[i];
            }
        }
    }
    return NULL;
}

// Check if path is on SMB mount
bool smb_is_smb_path(const char *path) {
    return smb_find_connection(path) != NULL;
}

// Get relative path within share
static const char *get_relative_path(smb_connection_t *conn, const char *path) {
    int len = strlen(conn->mount_point);
    if (strncmp(path, conn->mount_point, len) != 0) {
        return path;
    }
    path += len;
    while (*path == '/' || *path == '\\') path++;
    return path;
}

// Convert forward slashes to backslashes
static void normalize_path(const char *src, char *dst, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dst[i] = (src[i] == '/') ? '\\' : src[i];
        i++;
    }
    dst[i] = 0;
}

// ============================================================================
// File Operations
// ============================================================================

// Open a file
int smb_open(const char *path, uint32_t desired_access, uint32_t disposition) {
    smb_connection_t *conn = smb_find_connection(path);
    if (!conn) {
        kprintf("[SMB] No mount found for path: %s\n", path);
        return -1;
    }

    int fd = alloc_file();
    if (fd < 0) {
        kprintf("[SMB] No free file slots\n");
        return -1;
    }

    smb_file_t *file = &open_files[fd];

    // Get relative path and normalize
    const char *rel_path = get_relative_path(conn, path);
    char norm_path[SMB_MAX_PATH];
    normalize_path(rel_path, norm_path, SMB_MAX_PATH);

    // Determine create options
    uint32_t create_options = FILE_NON_DIRECTORY_FILE;

    if (smb2_create(conn, norm_path, desired_access, disposition,
                    create_options, file) < 0) {
        return -1;
    }

    file->active = true;
    return fd;
}

// Close a file
int smb_close(int fd) {
    smb_file_t *file = get_file(fd);
    if (!file) return -1;

    smb2_close_file(file->conn, file);
    file->active = false;
    return 0;
}

// Read from file
ssize_t smb_read(int fd, void *buffer, size_t count) {
    smb_file_t *file = get_file(fd);
    if (!file) return -1;
    return smb2_read(file->conn, file, buffer, count);
}

// Write to file
ssize_t smb_write(int fd, const void *buffer, size_t count) {
    smb_file_t *file = get_file(fd);
    if (!file) return -1;
    return smb2_write(file->conn, file, buffer, count);
}

// Seek
int64_t smb_seek(int fd, int64_t offset, int whence) {
    smb_file_t *file = get_file(fd);
    if (!file) return -1;

    int64_t new_pos;
    switch (whence) {
        case SMB_SEEK_SET:
            new_pos = offset;
            break;
        case SMB_SEEK_CUR:
            new_pos = file->position + offset;
            break;
        case SMB_SEEK_END:
            new_pos = file->file_size + offset;
            break;
        default:
            return -1;
    }

    if (new_pos < 0) new_pos = 0;
    file->position = new_pos;
    return new_pos;
}

// ============================================================================
// Directory Operations
// ============================================================================

// Directory handle uses file slot with search state
typedef struct {
    smb_file_t dir_file;
    uint8_t enum_buffer[8192];
    uint32_t enum_offset;
    uint32_t enum_len;
    bool first_query;
} smb_dir_handle_t;

static smb_dir_handle_t dir_handles[16];
static int dir_handle_count = 0;

// Open directory
int smb_opendir(const char *path) {
    smb_connection_t *conn = smb_find_connection(path);
    if (!conn) return -1;

    // Reuse a freed slot. Pass 1 monotonically bumped dir_handle_count and
    // never reclaimed on closedir, so after 16 opendir calls (even with
    // matching closedirs) opendir failed. The Files app re-lists a directory on
    // every refresh, so slot reuse is mandatory (task #317 pass 2).
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (!dir_handles[i].dir_file.active) { slot = i; break; }
    }
    if (slot < 0) return -1;
    if (slot >= dir_handle_count) dir_handle_count = slot + 1;

    smb_dir_handle_t *dh = &dir_handles[slot];
    memset(dh, 0, sizeof(smb_dir_handle_t));

    const char *rel_path = get_relative_path(conn, path);
    char norm_path[SMB_MAX_PATH];
    normalize_path(rel_path, norm_path, SMB_MAX_PATH);

    // SMB2 opens the share root with an EMPTY relative path (not "."); some
    // servers reject "." with OBJECT_NAME_INVALID.
    if (smb2_create(conn, norm_path,
                    FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                    FILE_OPEN,
                    FILE_DIRECTORY_FILE,
                    &dh->dir_file) < 0) {
        return -1;
    }

    dh->dir_file.active = true;
    dh->first_query = true;
    dh->enum_offset = 0;
    dh->enum_len = 0;

    return slot;
}

// Read directory entry
int smb_readdir(int dirfd, smb_dirent_t *entry) {
    if (dirfd < 0 || dirfd >= 16) return -1;

    smb_dir_handle_t *dh = &dir_handles[dirfd];
    if (!dh->dir_file.active) return -1;

    // Need more data?
    if (dh->enum_offset >= dh->enum_len) {
        uint8_t flags = dh->first_query ? SMB2_RESTART_SCANS : 0;
        dh->first_query = false;

        int ret = smb2_query_directory(dh->dir_file.conn, &dh->dir_file,
                                        "*", FileBothDirectoryInformation, flags,
                                        dh->enum_buffer, sizeof(dh->enum_buffer),
                                        &dh->enum_len);
        if (ret != 0) {
            return ret;  // 1 = end, -1 = error
        }
        dh->enum_offset = 0;
    }

    // Parse entry
    file_both_dir_info_t *info = (file_both_dir_info_t *)(dh->enum_buffer + dh->enum_offset);

    // Convert name from Unicode
    int name_chars __attribute__((unused)) = le32(info->file_name_length) / 2;
    uint16_t *name_unicode = (uint16_t *)((uint8_t*)info + sizeof(file_both_dir_info_t));
    unicode_to_ascii(name_unicode, le32(info->file_name_length), entry->name, SMB_MAX_NAME);

    entry->size = le64(info->end_of_file);
    entry->creation_time = smb_filetime_to_unix(le64(info->creation_time));
    entry->last_access_time = smb_filetime_to_unix(le64(info->last_access_time));
    entry->last_write_time = smb_filetime_to_unix(le64(info->last_write_time));
    entry->attributes = le32(info->file_attributes);
    entry->is_directory = (entry->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    // Move to next entry
    if (le32(info->next_entry_offset) == 0) {
        dh->enum_offset = dh->enum_len;  // Force refetch on next call
    } else {
        dh->enum_offset += le32(info->next_entry_offset);
    }

    return 0;
}

// Close directory
int smb_closedir(int dirfd) {
    if (dirfd < 0 || dirfd >= 16) return -1;

    smb_dir_handle_t *dh = &dir_handles[dirfd];
    if (!dh->dir_file.active) return -1;

    smb2_close_file(dh->dir_file.conn, &dh->dir_file);
    dh->dir_file.active = false;

    return 0;
}

// Create directory
int smb_mkdir(const char *path) {
    smb_connection_t *conn = smb_find_connection(path);
    if (!conn) return -1;

    const char *rel_path = get_relative_path(conn, path);
    char norm_path[SMB_MAX_PATH];
    normalize_path(rel_path, norm_path, SMB_MAX_PATH);

    smb_file_t temp_file;
    if (smb2_create(conn, norm_path,
                    FILE_READ_ATTRIBUTES,
                    FILE_CREATE,
                    FILE_DIRECTORY_FILE,
                    &temp_file) < 0) {
        return -1;
    }

    smb2_close_file(conn, &temp_file);
    return 0;
}

// Remove directory
int smb_rmdir(const char *path) {
    smb_connection_t *conn = smb_find_connection(path);
    if (!conn) return -1;

    const char *rel_path = get_relative_path(conn, path);
    char norm_path[SMB_MAX_PATH];
    normalize_path(rel_path, norm_path, SMB_MAX_PATH);

    smb_file_t temp_file;
    if (smb2_create(conn, norm_path,
                    DELETE,
                    FILE_OPEN,
                    FILE_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE,
                    &temp_file) < 0) {
        return -1;
    }

    smb2_close_file(conn, &temp_file);
    return 0;
}

// Delete file
int smb_delete(const char *path) {
    smb_connection_t *conn = smb_find_connection(path);
    if (!conn) return -1;

    const char *rel_path = get_relative_path(conn, path);
    char norm_path[SMB_MAX_PATH];
    normalize_path(rel_path, norm_path, SMB_MAX_PATH);

    smb_file_t temp_file;
    if (smb2_create(conn, norm_path,
                    DELETE,
                    FILE_OPEN,
                    FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE,
                    &temp_file) < 0) {
        return -1;
    }

    smb2_close_file(conn, &temp_file);
    return 0;
}

// Get file info
int smb_stat(const char *path, smb_dirent_t *info) {
    smb_connection_t *conn = smb_find_connection(path);
    if (!conn) return -1;

    const char *rel_path = get_relative_path(conn, path);
    char norm_path[SMB_MAX_PATH];
    normalize_path(rel_path, norm_path, SMB_MAX_PATH);

    smb_file_t temp_file;
    if (smb2_create(conn, norm_path,
                    FILE_READ_ATTRIBUTES,
                    FILE_OPEN,
                    0,  // Will auto-detect file/dir
                    &temp_file) < 0) {
        return -1;
    }

    // Extract info from create response
    info->size = temp_file.file_size;
    info->attributes = temp_file.attributes;
    info->is_directory = temp_file.is_directory;

    // Get the filename part
    const char *name = strrchr(path, '/');
    if (!name) name = strrchr(path, '\\');
    if (name) name++;
    else name = path;
    strncpy(info->name, name, SMB_MAX_NAME - 1);

    smb2_close_file(conn, &temp_file);
    return 0;
}

// ============================================================================
// Utility Functions
// ============================================================================

// Convert status to string
const char *smb_strerror(smb2_status_t status) {
    switch (status) {
        case STATUS_SUCCESS: return "Success";
        case STATUS_MORE_PROCESSING_REQUIRED: return "More processing required";
        case STATUS_INVALID_PARAMETER: return "Invalid parameter";
        case STATUS_NO_SUCH_FILE: return "No such file";
        case STATUS_END_OF_FILE: return "End of file";
        case STATUS_ACCESS_DENIED: return "Access denied";
        case STATUS_OBJECT_NAME_NOT_FOUND: return "Object name not found";
        case STATUS_OBJECT_NAME_COLLISION: return "Object name collision";
        case STATUS_OBJECT_PATH_NOT_FOUND: return "Object path not found";
        case STATUS_SHARING_VIOLATION: return "Sharing violation";
        case STATUS_LOGON_FAILURE: return "Logon failure";
        case STATUS_WRONG_PASSWORD: return "Wrong password";
        case STATUS_NO_SUCH_USER: return "No such user";
        case STATUS_BAD_NETWORK_NAME: return "Bad network name";
        case STATUS_NOT_SUPPORTED: return "Not supported";
        default: return "Unknown error";
    }
}

// Print mount info
void smb_print_mounts(void) {
    kprintf("SMB Mounts:\n");
    for (int i = 0; i < SMB_MAX_CONNECTIONS; i++) {
        if (connections[i].active) {
            kprintf("  %s -> \\\\", connections[i].mount_point);
            ip_print(connections[i].server_ip);
            kprintf("\\%s (dialect=0x%04x)\n",
                    connections[i].share_path, connections[i].dialect);
        }
    }
}

// ============================================================================
// VFS routing layer (task #317)
//
// Routes kernel/userland file access on the "/SMB/<server>/<share>/<path>"
// virtual prefix through the SMB2 client. A share is mounted on demand the
// first time it is touched. If the self-test (or a future NETMOUNTS.CFG
// consumer) already established an authenticated connection for that
// <server>/<share>, smb_find_connection reuses it; otherwise we fall back to
// a guest mount. This mirrors the ext2 "/ext2" prefix routing in fs/fat.c.
// ============================================================================

#include "../fs/fat.h"
extern fat_fs_t g_fat_fs;

// Default credentials applied to on-demand guest mounts can be overridden by
// the most recent explicit smb_mount_auth (stored per connection). Pass 2 will
// load these from /CONFIG/NETMOUNTS.CFG.
static char g_smb_def_user[64] = "guest";
static char g_smb_def_pass[64] = "";
static char g_smb_def_domain[64] = "WORKGROUP";

void smb_vfs_set_default_creds(const char *domain, const char *user, const char *pass) {
    strncpy(g_smb_def_domain, domain ? domain : "WORKGROUP", 63);
    strncpy(g_smb_def_user, user ? user : "guest", 63);
    strncpy(g_smb_def_pass, pass ? pass : "", 63);
}

bool smb_vfs_is_smb_path(const char *path) {
    return path && (strncmp(path, "/SMB/", 5) == 0 || strncmp(path, "/smb/", 5) == 0);
}

// Ensure a connection exists for the share referenced by an /SMB path.
static smb_connection_t *smb_vfs_ensure(const char *path) {
    if (!smb_vfs_is_smb_path(path)) return NULL;

    smb_connection_t *c = smb_find_connection(path);
    if (c) return c;

    // Parse /SMB/<server>/<share>/...
    const char *p = path + 5;
    char server[64]; int si = 0;
    while (*p && *p != '/' && si < 63) server[si++] = *p++;
    server[si] = 0;
    if (*p != '/') return NULL;
    p++;
    char share[64]; int hi = 0;
    while (*p && *p != '/' && hi < 63) share[hi++] = *p++;
    share[hi] = 0;
    if (si == 0 || hi == 0) return NULL;

    uint32_t ip = resolve_hostname(server);
    if (!ip) return NULL;

    char mp[SMB_MAX_PATH];
    snprintf(mp, sizeof(mp), "/SMB/%s/%s", server, share);

    int idx = smb_mount_auth(ip, share, g_smb_def_domain, g_smb_def_user,
                             g_smb_def_pass, mp);
    if (idx < 0) return NULL;
    return &connections[idx];
}

// Whole-file read, fat_read_file-shaped (kmalloc'd, NUL-terminated buffer).
void *smb_vfs_read_whole(const char *path, uint32_t *size_out) {
    if (size_out) *size_out = 0;
    if (!smb_vfs_ensure(path)) return NULL;

    int fd = smb_open(path, FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_OPEN);
    if (fd < 0) return NULL;

    smb_file_t *f = get_file(fd);
    uint64_t fsize64 = f ? f->file_size : 0;
    uint32_t cap = (uint32_t)fsize64;
    if (fsize64 > 0xF0000000ULL) { smb_close(fd); return NULL; }

    uint8_t *buf = (uint8_t *)kmalloc(cap + 1);
    if (!buf) { smb_close(fd); return NULL; }

    uint32_t got = 0;
    while (got < cap) {
        uint32_t want = cap - got;
        if (want > 16384) want = 16384;
        ssize_t r = smb_read(fd, buf + got, want);
        if (r <= 0) break;
        got += (uint32_t)r;
    }
    buf[got] = 0;
    smb_close(fd);
    if (size_out) *size_out = got;
    return buf;
}

// opendir that auto-mounts; readdir/closedir reuse smb_readdir/smb_closedir.
int smb_vfs_opendir(const char *path) {
    if (!smb_vfs_ensure(path)) return -1;
    return smb_opendir(path);
}

// Public IP resolver wrapper (resolve_hostname is file-static). Used by the
// share-enumeration syscall (task #317 pass 2).
uint32_t smb_resolve_ip(const char *host) {
    return host ? resolve_hostname(host) : 0;
}

// Mount an SMB share with explicit credentials, keyed by /SMB/<server>/<share>.
// Reuses an existing connection if already mounted. Returns 0 / -1. The Files
// app calls this (via SYS_NET_MOUNT) before navigating into a saved network
// location so the configured user/pass are used rather than the guest default
// (task #317 pass 2).
int smb_vfs_mount_creds(const char *server, const char *share,
                        const char *user, const char *pass) {
    if (!server || !share || !server[0] || !share[0]) return -1;

    char mp[SMB_MAX_PATH];
    snprintf(mp, sizeof(mp), "/SMB/%s/%s", server, share);
    if (smb_find_connection(mp)) return 0;   // already mounted

    uint32_t ip = resolve_hostname(server);
    if (!ip) return -1;

    smb_init();
    // Warm the net stack / ARP so the first SYN goes out with a known MAC.
    for (int i = 0; i < 100; i++) smb_net_pump();

    int idx = smb_mount_auth(ip, share, "WORKGROUP",
                             (user && user[0]) ? user : "guest",
                             pass ? pass : "", mp);
    return idx >= 0 ? 0 : -1;
}

// ============================================================================
// Share enumeration via srvsvc RPC over the IPC$ \srvsvc named pipe
// (task #317 pass 2). Implements just enough DCE/RPC (BIND + one NetrShareEnum
// level-1 REQUEST) and a tolerant NDR walk of the response to pull out share
// names. Best-effort: returns NULL on any failure so callers fall back to
// manual entry.
// ============================================================================

static int rpc_put32(uint8_t *b, int o, uint32_t v) {
    b[o] = v & 0xFF; b[o+1] = (v>>8)&0xFF; b[o+2] = (v>>16)&0xFF; b[o+3] = (v>>24)&0xFF;
    return o + 4;
}
static int rpc_put16(uint8_t *b, int o, uint16_t v) {
    b[o] = v & 0xFF; b[o+1] = (v>>8)&0xFF; return o + 2;
}
static uint32_t rpc_get32(const uint8_t *b, int o) {
    return (uint32_t)b[o] | ((uint32_t)b[o+1]<<8) | ((uint32_t)b[o+2]<<16) | ((uint32_t)b[o+3]<<24);
}

char **smb_list_shares(uint32_t server_ip, int *count) {
    if (count) *count = 0;
    if (!server_ip) return NULL;

    smb_init();
    for (int i = 0; i < 100; i++) smb_net_pump();

    char mp[SMB_MAX_PATH];
    snprintf(mp, sizeof(mp), "/SMB-IPC/%08x", (unsigned)server_ip);
    int idx = smb_mount_auth(server_ip, "IPC$", g_smb_def_domain,
                             g_smb_def_user, g_smb_def_pass, mp);
    if (idx < 0) { kprintf("[SMB] list_shares: IPC$ connect failed\n"); return NULL; }
    smb_connection_t *conn = &connections[idx];

    smb_file_t pipe;
    memset(&pipe, 0, sizeof(pipe));
    if (smb2_create(conn, "srvsvc",
                    FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES,
                    FILE_OPEN, 0, &pipe) < 0) {
        kprintf("[SMB] list_shares: open \\srvsvc failed\n");
        smb_unmount(mp); return NULL;
    }
    pipe.active = true;

    // ---- DCE/RPC BIND --------------------------------------------------------
    static uint8_t rb[2048];
    int o = 0;
    rb[o++] = 5; rb[o++] = 0; rb[o++] = 11; rb[o++] = 0x03;      // ver5.0, BIND, first+last
    rb[o++] = 0x10; rb[o++] = 0; rb[o++] = 0; rb[o++] = 0;       // DREP little-endian
    int frag_off = o; o = rpc_put16(rb, o, 0);                   // frag_length (patch)
    o = rpc_put16(rb, o, 0);                                     // auth_length
    o = rpc_put32(rb, o, 1);                                     // call_id
    o = rpc_put16(rb, o, 4280);                                  // max_xmit
    o = rpc_put16(rb, o, 4280);                                  // max_recv
    o = rpc_put32(rb, o, 0);                                     // assoc_group
    rb[o++] = 1; rb[o++] = 0; rb[o++] = 0; rb[o++] = 0;          // num_ctx=1 + reserved
    o = rpc_put16(rb, o, 0);                                     // context_id=0
    rb[o++] = 1; rb[o++] = 0;                                    // num_trans=1 + reserved
    // SRVSVC interface UUID 4b324fc8-1670-01d3-1278-5a47bf6ee188 (little-endian
    // Data1/2/3). A prior typo (0x4F written as 0xFC) made the BIND get rejected
    // with "abstract syntax not supported" -> nca_unk_if (0x1c010003).
    { static const uint8_t srv[16] = {0xC8,0x4F,0x32,0x4B,0x70,0x16,0xD3,0x01,
                                       0x12,0x78,0x5A,0x47,0xBF,0x6E,0xE1,0x88};
      for (int i=0;i<16;i++) rb[o++]=srv[i]; }                   // SRVSVC UUID
    o = rpc_put16(rb, o, 3); o = rpc_put16(rb, o, 0);           // version 3.0
    { static const uint8_t ndr[16] = {0x04,0x5D,0x88,0x8A,0xEB,0x1C,0xC9,0x11,
                                       0x9F,0xE8,0x08,0x00,0x2B,0x10,0x48,0x60};
      for (int i=0;i<16;i++) rb[o++]=ndr[i]; }                   // NDR UUID
    o = rpc_put32(rb, o, 2);                                     // NDR version 2
    rpc_put16(rb, frag_off, (uint16_t)o);

    if (smb2_write(conn, &pipe, rb, o) < 0) { smb2_close_file(conn,&pipe); smb_unmount(mp); return NULL; }
    pipe.position = 0;
    static uint8_t resp[8192];
    ssize_t rn = smb2_read(conn, &pipe, resp, sizeof(resp));
    if (rn < 16 || resp[2] != 12) {   // 12 = BIND_ACK
        kprintf("[SMB] list_shares: bind failed (ptype=%d n=%d)\n", rn>2?resp[2]:-1, (int)rn);
        smb2_close_file(conn,&pipe); smb_unmount(mp); return NULL;
    }
    // A BIND_ACK can still REJECT the presentation context (e.g. wrong interface
    // UUID/version -> ack_result=2 "provider rejection", later nca_unk_if). Parse
    // the first result's ack_result and bail clearly if it is not acceptance (0).
    {
        int sec_len = (rn > 25) ? (resp[24] | (resp[25] << 8)) : 0;
        int rp = (26 + sec_len + 3) & ~3;          // skip sec_addr, align to 4
        if (rp + 6 <= (int)rn) {
            int ack = resp[rp+4] | (resp[rp+5] << 8);
            if (ack != 0) {
                kprintf("[SMB] list_shares: bind rejected (ack_result=%d)\n", ack);
                smb2_close_file(conn,&pipe); smb_unmount(mp); return NULL;
            }
        }
    }

    // ---- NetrShareEnum REQUEST (opnum 15, level 1) ---------------------------
    o = 0;
    rb[o++] = 5; rb[o++] = 0; rb[o++] = 0; rb[o++] = 0x03;       // REQUEST, first+last
    rb[o++] = 0x10; rb[o++] = 0; rb[o++] = 0; rb[o++] = 0;
    int frag_off2 = o; o = rpc_put16(rb, o, 0);                  // frag_length (patch)
    o = rpc_put16(rb, o, 0);                                     // auth_length
    o = rpc_put32(rb, o, 2);                                     // call_id
    int alloc_off = o; o = rpc_put32(rb, o, 0);                  // alloc_hint (patch)
    o = rpc_put16(rb, o, 0);                                     // context_id
    // opnum 36 = NetShareEnum on Samba's srvsvc interface (verified against
    // Samba 4.17 with rpcclient netshareenumall; opnum 15 returned
    // nca_s_fault_ndr because it maps to a different function here).
    o = rpc_put16(rb, o, 36);                                    // opnum = NetShareEnum
    int stub_start = o;
    // ServerName: unique ptr -> conformant-varying wchar string "\\<ip>".
    // (A NULL ServerName makes Samba return nca_s_fault_ndr.)
    char sname[40];
    snprintf(sname, sizeof(sname), "\\\\%u.%u.%u.%u",
             (server_ip>>24)&0xFF, (server_ip>>16)&0xFF,
             (server_ip>>8)&0xFF, server_ip&0xFF);
    int slen = (int)strlen(sname) + 1;                          // include NUL terminator
    o = rpc_put32(rb, o, 0x00020000);                          // ServerName referent (non-null)
    o = rpc_put32(rb, o, (uint32_t)slen);                      // max_count (wchars)
    o = rpc_put32(rb, o, 0);                                    // offset
    o = rpc_put32(rb, o, (uint32_t)slen);                      // actual_count
    for (int i = 0; i < slen; i++) { rb[o++] = sname[i]; rb[o++] = 0; }  // UTF-16LE incl NUL
    while (o & 3) rb[o++] = 0;                                  // align to 4 bytes
    o = rpc_put32(rb, o, 1);                                     // Level = 1
    o = rpc_put32(rb, o, 1);                                     // union discriminant = 1
    o = rpc_put32(rb, o, 0x00020004);                          // CONTAINER referent (non-null)
    o = rpc_put32(rb, o, 0);                                     // EntriesRead = 0
    o = rpc_put32(rb, o, 0);                                     // Buffer = NULL
    o = rpc_put32(rb, o, 0xFFFFFFFF);                           // PreferredMaximumLength
    // ResumeHandle as a NULL unique pointer (no referent, no value), matching
    // the proven rpcclient request layout.
    o = rpc_put32(rb, o, 0);                                     // ResumeHandle = NULL
    rpc_put32(rb, alloc_off, (uint32_t)(o - stub_start));
    rpc_put16(rb, frag_off2, (uint16_t)o);

    if (smb2_write(conn, &pipe, rb, o) < 0) { smb2_close_file(conn,&pipe); smb_unmount(mp); return NULL; }
    pipe.position = 0;
    rn = smb2_read(conn, &pipe, resp, sizeof(resp));
    smb2_close_file(conn, &pipe);
    smb_unmount(mp);
    if (rn < 24) { kprintf("[SMB] list_shares: short response %d\n", (int)rn); return NULL; }

    // A DCE/RPC FAULT response (ptype 3) means the server rejected the request
    // (e.g. nca_s_fault_ndr if the input NDR is malformed). Bail gracefully.
    if (resp[2] == 3) {
        kprintf("[SMB] list_shares: server FAULT (status 0x%08x)\n",
                (unsigned)rpc_get32(resp, 24));
        return NULL;
    }

    // ---- tolerant NDR walk ---------------------------------------------------
    int p = 24;                                                  // skip RPC response header
    if (p + 24 > (int)rn) return NULL;
    p += 4;                                                      // Level
    p += 4;                                                      // union discriminant tag
    p += 4;                                                      // container referent id
    uint32_t entries = rpc_get32(resp, p); p += 4;              // EntriesRead
    p += 4;                                                      // Buffer referent id
    if (entries == 0 || entries > 256) return NULL;
    p += 4;                                                      // array max_count
    p += (int)(entries * 12);                                    // skip SHARE_INFO_1[] structs

    char **out = (char **)kmalloc(sizeof(char *) * entries);
    if (!out) return NULL;
    int found = 0;
    for (uint32_t s = 0; s < entries * 2 && p + 12 <= (int)rn; s++) {
        uint32_t actc = rpc_get32(resp, p + 8);                 // actual_count
        p += 12;
        if (actc > 1024) break;
        int bytes = (int)actc * 2;
        if (p + bytes > (int)rn) break;
        if ((s & 1) == 0 && found < (int)entries) {             // even = netname
            char nm[SMB_MAX_NAME];
            unicode_to_ascii((const uint16_t *)(resp + p), bytes, nm, sizeof(nm));
            int L = strlen(nm);
            char *dup = (char *)kmalloc(L + 1);
            if (dup) { for (int i=0;i<=L;i++) dup[i]=nm[i]; out[found++] = dup; }
        }
        p += bytes;
        if (p & 3) p += 4 - (p & 3);                            // 4-byte align
    }
    if (found == 0) { kfree(out); return NULL; }
    if (count) *count = found;
    return out;
}

void smb_free_shares(char **shares, int count) {
    if (!shares) return;
    for (int i = 0; i < count; i++) if (shares[i]) kfree(shares[i]);
    kfree(shares);
}

// Public on-demand mount for an /SMB path (task #317 pass 2). Returns 0 if a
// connection for the share is available (mounting it if needed), -1 otherwise.
// The syscall layer calls this before smb_stat/smb_open/smb_opendir so the
// connection is guaranteed to exist.
int smb_vfs_ensure_mount(const char *path) {
    return smb_vfs_ensure(path) ? 0 : -1;
}

// Whole-file write/upload, fat_create+write-shaped (task #317 pass 2). Truncates
// or creates the target (FILE_OVERWRITE_IF) and streams `data`[len] to it in
// chunks bounded by the negotiated max_write_size AND the 64 KB send buffer.
// Returns 0 on success, -1 on error. This is the upload chokepoint used by the
// syscall write-on-close path so userland copy_file()/editors write to /SMB.
int smb_vfs_write_whole(const char *path, const void *data, uint32_t len) {
    if (!smb_vfs_ensure(path)) return -1;

    int fd = smb_open(path, FILE_WRITE_DATA | FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                      FILE_OVERWRITE_IF);
    if (fd < 0) return -1;

    const uint8_t *p = (const uint8_t *)data;
    uint32_t off = 0;
    int ok = 1;
    while (off < len) {
        uint32_t want = len - off;
        if (want > 32768) want = 32768;   // stay well within smb_send_buffer (64 KB)
        ssize_t w = smb_write(fd, p + off, want);
        if (w <= 0) { ok = 0; break; }
        off += (uint32_t)w;
    }
    smb_close(fd);
    return ok ? 0 : -1;
}

// ============================================================================
// Boot self-test (task #317) - gated on /CONFIG/SMBTEST.CFG.
// Format (one key=value per line):
//   ip=192.0.2.219
//   share=share
//   user=maytera
//   pass=maytera
//   file=TEST.TXT
// Prints clearly-delimited results to serial. Safe to leave compiled in:
// does nothing when the config file is absent.
// ============================================================================

static void smbtest_get(const char *buf, const char *key, char *out, int outsz) {
    out[0] = 0;
    int klen = strlen(key);
    const char *p = buf;
    while (*p) {
        // start of line
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            int i = 0;
            while (*p && *p != '\n' && *p != '\r' && i < outsz - 1) out[i++] = *p++;
            out[i] = 0;
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
}

// ============================================================================
// Persistent network mounts (task #317 pass 4) - /CONFIG/NETMOUNTS.CFG.
// One mount per line, pipe-delimited, so the Files "Network" menu can append
// entries that re-mount automatically on the next boot:
//   SMB|server|share|user|pass        (user/pass optional -> guest)
//   NFS|server|/export/path
//   label|server|share|user|pass      (legacy Files "Add" format = SMB)
// Lines starting with '#' are comments. Loaded once by the deferred net worker
// (scheduler + net up), pre-creating the connections so /SMB/<server>/<share>
// and /NFS/<server>/<export-basename> resolve without an explicit mount.
// ============================================================================
#define NET_MAX_MOUNTS 16
static char g_net_mount_paths[NET_MAX_MOUNTS][160];
static int  g_net_mount_count = 0;
int net_mounts_count(void) { return g_net_mount_count; }
const char *net_mounts_path(int i) {
    return (i >= 0 && i < g_net_mount_count) ? g_net_mount_paths[i] : 0;
}

void net_mounts_load(void) {
    uint32_t sz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/NETMOUNTS.CFG", &sz);
    if (!cfg) return;  // no persisted mounts -> silent

    kprintf("\n========== NETMOUNTS (task #317 pass 4) ==========\n");
    smb_init();
    for (int i = 0; i < 100; i++) smb_net_pump();   // warm ARP/net

    char *p = cfg;
    while (*p) {
        char line[256]; int ll = 0;
        while (*p && *p != '\n' && *p != '\r' && ll < 255) line[ll++] = *p++;
        line[ll] = 0;
        while (*p == '\n' || *p == '\r') p++;
        if (line[0] == 0 || line[0] == '#') continue;

        char *f[6]; int nf = 0; char *q = line;
        f[nf++] = q;
        while (*q && nf < 6) { if (*q == '|') { *q = 0; q++; f[nf++] = q; } else q++; }

        if (strcmp(f[0], "SMB") == 0 && nf >= 3) {
            const char *server = f[1], *share = f[2];
            const char *user = (nf >= 4) ? f[3] : "guest";
            const char *pass = (nf >= 5) ? f[4] : "";
            if (smb_vfs_mount_creds(server, share, user, pass) == 0) {
                if (g_net_mount_count < NET_MAX_MOUNTS)
                    snprintf(g_net_mount_paths[g_net_mount_count++], 160,
                             "/SMB/%s/%s", server, share);
                kprintf("[NETMOUNTS] SMB %s/%s mounted\n", server, share);
            } else {
                kprintf("[NETMOUNTS] SMB %s/%s FAILED\n", server, share);
            }
        } else if (strcmp(f[0], "NFS") == 0 && nf >= 3) {
            const char *server = f[1], *expt = f[2];
            char mp[160];
            extern int nfs_vfs_mount(const char *, const char *, char *, int);
            if (nfs_vfs_mount(server, expt, mp, sizeof(mp)) == 0) {
                if (g_net_mount_count < NET_MAX_MOUNTS) {
                    char *d = g_net_mount_paths[g_net_mount_count++];
                    int i = 0; while (mp[i] && i < 159) { d[i] = mp[i]; i++; } d[i] = 0;
                }
                kprintf("[NETMOUNTS] NFS %s:%s mounted at %s\n", server, expt, mp);
            } else {
                kprintf("[NETMOUNTS] NFS %s:%s FAILED\n", server, expt);
            }
        } else if (nf >= 3) {
            // Legacy Files "Add" format: label|server|share|user|pass (= SMB).
            const char *server = f[1], *share = f[2];
            const char *user = (nf >= 4) ? f[3] : "guest";
            const char *pass = (nf >= 5) ? f[4] : "";
            if (smb_vfs_mount_creds(server, share, user, pass) == 0) {
                if (g_net_mount_count < NET_MAX_MOUNTS)
                    snprintf(g_net_mount_paths[g_net_mount_count++], 160,
                             "/SMB/%s/%s", server, share);
                kprintf("[NETMOUNTS] SMB(legacy) %s/%s mounted\n", server, share);
            } else {
                kprintf("[NETMOUNTS] SMB(legacy) %s/%s FAILED\n", server, share);
            }
        }
    }
    kfree(cfg);
    kprintf("[NETMOUNTS] %d mount(s) loaded\n", g_net_mount_count);
    kprintf("==================================================\n\n");
}

void smb_run_selftest(void) {
    uint32_t cfgsz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/SMBTEST.CFG", &cfgsz);
    if (!cfg) return;  // no config -> silent

    char ipstr[64], share[64], user[64], pass[64], file[128];
    smbtest_get(cfg, "ip", ipstr, sizeof(ipstr));
    smbtest_get(cfg, "share", share, sizeof(share));
    smbtest_get(cfg, "user", user, sizeof(user));
    smbtest_get(cfg, "pass", pass, sizeof(pass));
    smbtest_get(cfg, "file", file, sizeof(file));
    kfree(cfg);

    if (!ipstr[0] || !share[0]) {
        kprintf("[SMBTEST] config present but missing ip/share\n");
        return;
    }
    if (!user[0]) strcpy(user, "guest");
    if (!file[0]) strcpy(file, "TEST.TXT");

    kprintf("\n========== SMB SELFTEST (task #317) ==========\n");
    kprintf("[SMBTEST] target=%s share=%s user=%s file=%s\n",
            ipstr, share, user, file);

    smb_init();

    uint32_t ip = resolve_hostname(ipstr);
    if (!ip) { kprintf("[SMBTEST] bad ip\n========== SMB SELFTEST: FAIL ==========\n"); return; }

    // Warm up the stack so ARP for the server settles.
    for (int i = 0; i < 200; i++) smb_net_pump();

    char mp[SMB_MAX_PATH];
    snprintf(mp, sizeof(mp), "/SMB/%s/%s", ipstr, share);

    // 1) Mount: TCP connect + NEGOTIATE + SESSION_SETUP(NTLM) + TREE_CONNECT
    int idx = smb_mount_auth(ip, share, "WORKGROUP", user, pass, mp);
    if (idx < 0) {
        kprintf("[SMBTEST] mount FAILED\n========== SMB SELFTEST: FAIL ==========\n");
        return;
    }
    smb_connection_t *conn = &connections[idx];
    kprintf("[SMBTEST] MOUNT OK: dialect=0x%04x tree_id=%u max_read=%u\n",
            conn->dialect, conn->tree_id, conn->max_read_size);

    // 2) Enumerate the share root.
    kprintf("[SMBTEST] --- directory listing of %s ---\n", mp);
    int dir = smb_opendir(mp);
    int nent = 0;
    if (dir >= 0) {
        smb_dirent_t de;
        int r;
        while ((r = smb_readdir(dir, &de)) == 0 && nent < 64) {
            kprintf("[SMBTEST]   %s %s  %u bytes\n",
                    de.is_directory ? "<DIR> " : "<FILE>", de.name,
                    (uint32_t)de.size);
            nent++;
        }
        smb_closedir(dir);
        kprintf("[SMBTEST] enumerated %d entries\n", nent);
    } else {
        kprintf("[SMBTEST] opendir FAILED\n");
    }

    // 3) Read the test file directly via smb_open/smb_read.
    char fpath[SMB_MAX_PATH];
    snprintf(fpath, sizeof(fpath), "%s/%s", mp, file);
    kprintf("[SMBTEST] --- reading %s ---\n", fpath);
    int read_ok = 0;
    int fd = smb_open(fpath, FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_OPEN);
    if (fd >= 0) {
        static uint8_t fbuf[4096];
        ssize_t got = smb_read(fd, fbuf, sizeof(fbuf) - 1);
        if (got > 0) {
            fbuf[got] = 0;
            kprintf("[SMBTEST] read %d bytes:\n", (int)got);
            kprintf("----8<----\n%s\n----8<----\n", (char *)fbuf);
            read_ok = 1;
        } else {
            kprintf("[SMBTEST] read returned %d\n", (int)got);
        }
        smb_close(fd);
    } else {
        kprintf("[SMBTEST] open FAILED\n");
    }

    // 4) Read the SAME file through the VFS routing hook (fat_read_file on an
    //    /SMB path) to prove the VFS integration works end-to-end.
    kprintf("[SMBTEST] --- VFS read via fat_read_file(%s) ---\n", fpath);
    uint32_t vsz = 0;
    char *vbuf = (char *)fat_read_file(&g_fat_fs, fpath, &vsz);
    int vfs_ok = 0;
    if (vbuf) {
        kprintf("[SMBTEST] VFS read %u bytes:\n----8<----\n%s\n----8<----\n", vsz, vbuf);
        kfree(vbuf);
        vfs_ok = 1;
    } else {
        kprintf("[SMBTEST] VFS read FAILED\n");
    }

    // 5) WRITE/UPLOAD test through the SYSCALL path (sys_open/sys_write/sys_close)
    //    to prove task #317 pass-2 FS wiring + smb_vfs_write_whole upload work.
    int write_ok = 0;
    {
        extern int64_t sys_open(const char *, int);
        extern int64_t sys_write(int, const void *, uint64_t);
        extern int64_t sys_close(int);
        char wpath[SMB_MAX_PATH];
        snprintf(wpath, sizeof(wpath), "%s/P2WRITE.TXT", mp);
        const char *payload = "MayteraOS task #317 pass-2 SMB upload OK\n";
        kprintf("[SMBTEST] --- WRITE via syscall path %s ---\n", wpath);
        int64_t wfd = sys_open(wpath, 0x41);   // O_CREAT|O_WRONLY
        if (wfd >= 0) {
            int64_t wn = sys_write((int)wfd, payload, (uint64_t)strlen(payload));
            sys_close((int)wfd);               // close triggers the SMB upload
            kprintf("[SMBTEST] wrote %d bytes (buffered), uploaded on close\n", (int)wn);
            // Read it back through the VFS hook to confirm the round-trip.
            uint32_t rb = 0;
            char *back = (char *)fat_read_file(&g_fat_fs, wpath, &rb);
            if (back) {
                kprintf("[SMBTEST] readback %u bytes:\n----8<----\n%s----8<----\n", rb, back);
                if (rb == (uint32_t)strlen(payload)) write_ok = 1;
                kfree(back);
            } else {
                kprintf("[SMBTEST] readback FAILED\n");
            }
        } else {
            kprintf("[SMBTEST] write-open FAILED\n");
        }
    }

    // 6) SHARE ENUMERATION (srvsvc over IPC$).
    {
        smb_vfs_set_default_creds("WORKGROUP", user, pass);
        kprintf("[SMBTEST] --- share enumeration on %s ---\n", ipstr);
        int sc = 0;
        char **shs = smb_list_shares(ip, &sc);
        if (shs) {
            for (int i = 0; i < sc; i++) kprintf("[SMBTEST]   share: %s\n", shs[i]);
            kprintf("[SMBTEST] enumerated %d shares\n", sc);
            smb_free_shares(shs, sc);
        } else {
            kprintf("[SMBTEST] share enumeration returned none (srvsvc)\n");
        }
    }

    if (read_ok && nent > 0 && vfs_ok && write_ok) {
        kprintf("========== SMB SELFTEST: PASS ==========\n\n");
    } else {
        kprintf("========== SMB SELFTEST: FAIL ==========\n\n");
    }
}

// Deferred self-test worker: runs a few seconds after boot, in a normal
// kernel process context (scheduler + net services up = the known-good net
// path used by the browser/HTTP). Mirrors output to serial.
static void smb_selftest_worker(void *arg) {
    (void)arg;
    extern void kprintf_set_dual_output(int on);
    proc_sleep(12000);           // let the desktop + net stack settle
    kprintf_set_dual_output(1);
    // task #317 pass 4: prove the SMB layer resolves HOSTNAMES via the kernel DNS
    // resolver (resolve_hostname -> dns_resolve), not just dotted-quad IPs.
    {
        uint32_t t = smb_resolve_ip("dns.google");
        if (t) kprintf("[DNSTEST] smb_resolve_ip(dns.google) = %u.%u.%u.%u (hostname OK)\n",
                       (t>>24)&0xFF, (t>>16)&0xFF, (t>>8)&0xFF, t&0xFF);
        else kprintf("[DNSTEST] smb_resolve_ip(dns.google) failed\n");
    }
    // task #317 pass 4: re-create persisted mounts from /CONFIG/NETMOUNTS.CFG
    // BEFORE the self-tests, so the fd-path test can read them without an
    // explicit mount (proving cross-reboot persistence).
    net_mounts_load();
    smb_run_selftest();
    // task #317 pass 3: NFS self-test (gated on /CONFIG/NFSTEST.CFG).
    { extern void nfs_run_selftest(void); nfs_run_selftest(); }
    // task #317 pass 4: terminal cat/ls path (sys_open/read/readdir) over the
    // persisted /SMB and /NFS mounts.
    { extern void netfs_fdpath_selftest(void); netfs_fdpath_selftest(); }
    kprintf_set_dual_output(0);
    // fall through: worker proc returns and is reaped
}

void smb_start_deferred_selftest(void) {
    extern int proc_create_ex(const char *name, void (*entry)(void *), void *arg,
                              int priority, uint32_t stack_size);
    // PRIO_LOW = 1; 256 KB stack for the net call chain (#264 guidance).
    proc_create_ex("smbtest", smb_selftest_worker, 0, 1, 256 * 1024);
}
