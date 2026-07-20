// rpc.c - Sun RPC (Remote Procedure Call) Implementation
// Implements RPC/XDR for NFS client support

#include "rpc.h"
#include "udp.h"
#include "tcp.h"
#include "ip.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../serial.h"

// ============================================================================
// Global State
// ============================================================================

static rpc_client_t rpc_clients[RPC_MAX_CLIENTS];
static rpc_transaction_t rpc_transactions[RPC_MAX_CLIENTS];
static uint32_t rpc_global_xid = 1;
static bool rpc_debug = false;

// ---------------------------------------------------------------------------
// task #317 pass 3: drive the kernel net stack the same way net/smb.c does.
// The original RPC transport spun on busy-wait loops and NEVER pumped the NIC
// (net_poll) or yielded to the scheduler, so no reply was ever delivered, and
// its timeouts assumed a 18.2 Hz tick when the PIT actually runs at g_timer_hz
// (250 Hz). These helpers fix both: pump RX/TX + TCP timers each iteration and
// yield 1 ms (the only way TCP/RX advances in this kernel), with tick-accurate
// timeouts. See cl_smb.md "Root causes fixed".
// ---------------------------------------------------------------------------
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;            // PIT frequency (default 250 Hz)
extern void net_poll(void);            // pump NIC RX/TX + DHCP
extern void tcp_timer(void);           // drive TCP retransmit/timeouts
extern void proc_sleep(uint32_t ms);   // yield to scheduler
extern int arp_resolve(uint32_t ip, uint8_t *mac);
extern void ip_print(uint32_t ip);

static inline uint64_t rpc_ms_to_ticks(int ms) {
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t t = ((uint64_t)ms * hz) / 1000;
    return t ? t : 1;
}
static inline void rpc_net_pump(void) {
    net_poll();
    tcp_timer();
    proc_sleep(1);
}

// Response buffer for UDP callbacks
static volatile bool rpc_response_ready = false;
static volatile uint32_t rpc_response_xid = 0;
static volatile uint8_t rpc_response_buffer[RPC_MAX_MSG_SIZE];
static volatile size_t rpc_response_len = 0;

// ============================================================================
// Byte Order Conversion (Network = Big Endian)
// ============================================================================

static inline uint32_t htonl(uint32_t val) {
    return ((val & 0xFF000000) >> 24) |
           ((val & 0x00FF0000) >> 8) |
           ((val & 0x0000FF00) << 8) |
           ((val & 0x000000FF) << 24);
}

static inline uint32_t ntohl(uint32_t val) {
    return htonl(val);  // Same operation
}

static inline uint16_t htons(uint16_t val) {
    return ((val & 0xFF00) >> 8) | ((val & 0x00FF) << 8);
}

static inline uint16_t ntohs(uint16_t val) {
    return htons(val);  // Same operation
}

// ============================================================================
// XDR Implementation
// ============================================================================

void xdr_init_encode(xdr_t *xdr, uint8_t *buffer, size_t size) {
    xdr->data = buffer;
    xdr->size = size;
    xdr->pos = 0;
    xdr->encoding = true;
    xdr->error = false;
}

void xdr_init_decode(xdr_t *xdr, const uint8_t *buffer, size_t size) {
    xdr->data = (uint8_t *)buffer;
    xdr->size = size;
    xdr->pos = 0;
    xdr->encoding = false;
    xdr->error = false;
}

void xdr_reset(xdr_t *xdr) {
    xdr->pos = 0;
    xdr->error = false;
}

size_t xdr_getpos(xdr_t *xdr) {
    return xdr->pos;
}

bool xdr_setpos(xdr_t *xdr, size_t pos) {
    if (pos > xdr->size) {
        xdr->error = true;
        return false;
    }
    xdr->pos = pos;
    return true;
}

bool xdr_error(xdr_t *xdr) {
    return xdr->error;
}

size_t xdr_remaining(xdr_t *xdr) {
    if (xdr->pos >= xdr->size) return 0;
    return xdr->size - xdr->pos;
}

// XDR aligns to 4-byte boundaries
static size_t xdr_aligned_len(size_t len) {
    return (len + 3) & ~3;
}

// =============================================================================
// #404 batch-3 (LAST parser-tier seam): verbatim decode-branch reference twins
// of the XDR primitives, kept for the boot [RUST-DIFF] differential and for a
// trivial rollback (they are the exact logic the shipping kernel runs when
// -DRUST_XDR is undefined). The LIVE dispatchers below route the DECODE branch to
// the Rust ports (xdr_decode_*_rs, rustkern.rs) under -DRUST_XDR; the ENCODE
// branch stays C. Defense-in-depth (source-bounded on 64-bit, NO reachable OOB).
// The untrusted input is XDR-encoded RPC/NFS server replies (remotely mountable
// via fs/netfs.c nfs://host/export). NOTE: the source-bounded XDR seam does NOT
// confine the SEPARATE net/nfs.c destination over-write (MAYTERA-SEC-2026-0012).
// =============================================================================
static bool xdr_uint32_c(xdr_t *xdr, uint32_t *val) {
    if (xdr->pos + 4 > xdr->size) { xdr->error = true; return false; }
    uint32_t nval; memcpy(&nval, xdr->data + xdr->pos, 4);
    *val = ntohl(nval); xdr->pos += 4; return true;
}
static bool xdr_uint64_c(xdr_t *xdr, uint64_t *val) {
    uint32_t high, low;
    if (!xdr_uint32_c(xdr, &high)) return false;
    if (!xdr_uint32_c(xdr, &low)) return false;
    *val = ((uint64_t)high << 32) | low; return true;
}
static bool xdr_opaque_c(xdr_t *xdr, uint8_t *data, size_t len) {
    size_t aligned = xdr_aligned_len(len);
    if (xdr->pos + aligned > xdr->size) { xdr->error = true; return false; }
    memcpy(data, xdr->data + xdr->pos, len);
    xdr->pos += aligned; return true;
}
static bool xdr_string_c(xdr_t *xdr, char *str, uint32_t maxlen) {
    uint32_t len;
    if (!xdr_uint32_c(xdr, &len)) return false;
    if (len >= maxlen) { xdr->error = true; return false; }
    if (!xdr_opaque_c(xdr, (uint8_t *)str, len)) return false;
    str[len] = '\0'; return true;
}
static bool xdr_nfs_fh3_c(xdr_t *xdr, uint8_t *fh_data, uint32_t *fh_len) {
    if (!xdr_uint32_c(xdr, fh_len)) return false;
    if (*fh_len > 64) { xdr->error = true; return false; }
    if (!xdr_opaque_c(xdr, fh_data, *fh_len)) return false;
    return true;
}

bool xdr_uint32(xdr_t *xdr, uint32_t *val) {
    if (xdr->error) return false;
    
    if (xdr->encoding) {
        if (xdr->pos + 4 > xdr->size) {
            xdr->error = true;
            return false;
        }
        uint32_t nval = htonl(*val);
        memcpy(xdr->data + xdr->pos, &nval, 4);
        xdr->pos += 4;
    } else {
#ifdef RUST_XDR
        extern bool xdr_decode_uint32_rs(xdr_t *xdr, uint32_t *val);
        return xdr_decode_uint32_rs(xdr, val);
#else
        if (xdr->pos + 4 > xdr->size) {
            xdr->error = true;
            return false;
        }
        uint32_t nval;
        memcpy(&nval, xdr->data + xdr->pos, 4);
        *val = ntohl(nval);
        xdr->pos += 4;
#endif
    }
    return true;
}

bool xdr_int32(xdr_t *xdr, int32_t *val) {
    return xdr_uint32(xdr, (uint32_t *)val);
}

bool xdr_uint64(xdr_t *xdr, uint64_t *val) {
    if (xdr->error) return false;
    
    if (xdr->encoding) {
        uint32_t high = (uint32_t)(*val >> 32);
        uint32_t low = (uint32_t)(*val & 0xFFFFFFFF);
        if (!xdr_uint32(xdr, &high)) return false;
        if (!xdr_uint32(xdr, &low)) return false;
    } else {
#ifdef RUST_XDR
        extern bool xdr_decode_uint64_rs(xdr_t *xdr, uint64_t *val);
        return xdr_decode_uint64_rs(xdr, val);
#else
        uint32_t high, low;
        if (!xdr_uint32(xdr, &high)) return false;
        if (!xdr_uint32(xdr, &low)) return false;
        *val = ((uint64_t)high << 32) | low;
#endif
    }
    return true;
}

bool xdr_int64(xdr_t *xdr, int64_t *val) {
    return xdr_uint64(xdr, (uint64_t *)val);
}

bool xdr_bool(xdr_t *xdr, bool *val) {
    uint32_t uval = *val ? 1 : 0;
    if (!xdr_uint32(xdr, &uval)) return false;
    if (!xdr->encoding) {
        *val = (uval != 0);
    }
    return true;
}

bool xdr_enum(xdr_t *xdr, int *val) {
    uint32_t uval = (uint32_t)*val;
    if (!xdr_uint32(xdr, &uval)) return false;
    if (!xdr->encoding) {
        *val = (int)uval;
    }
    return true;
}

bool xdr_opaque(xdr_t *xdr, uint8_t *data, size_t len) {
    if (xdr->error) return false;
    
    size_t aligned = xdr_aligned_len(len);
    
    if (xdr->encoding) {
        if (xdr->pos + aligned > xdr->size) {
            xdr->error = true;
            return false;
        }
        memcpy(xdr->data + xdr->pos, data, len);
        // Zero padding
        if (aligned > len) {
            memset(xdr->data + xdr->pos + len, 0, aligned - len);
        }
        xdr->pos += aligned;
    } else {
#ifdef RUST_XDR
        extern bool xdr_decode_opaque_rs(xdr_t *xdr, uint8_t *data, size_t len);
        return xdr_decode_opaque_rs(xdr, data, len);
#else
        if (xdr->pos + aligned > xdr->size) {
            xdr->error = true;
            return false;
        }
        memcpy(data, xdr->data + xdr->pos, len);
        xdr->pos += aligned;
#endif
    }
    return true;
}

bool xdr_bytes(xdr_t *xdr, uint8_t **data, uint32_t *len, uint32_t maxlen) {
    if (xdr->error) return false;
    
    if (xdr->encoding) {
        if (*len > maxlen) {
            xdr->error = true;
            return false;
        }
        if (!xdr_uint32(xdr, len)) return false;
        if (!xdr_opaque(xdr, *data, *len)) return false;
    } else {
#ifdef RUST_XDR
        extern bool xdr_decode_bytes_rs(xdr_t *xdr, uint8_t **data,
                                        uint32_t *len, uint32_t maxlen);
        return xdr_decode_bytes_rs(xdr, data, len, maxlen);
#else
        if (!xdr_uint32(xdr, len)) return false;
        if (*len > maxlen) {
            xdr->error = true;
            return false;
        }
        // For decoding, caller provides buffer
        if (*data == NULL) {
            xdr->error = true;
            return false;
        }
        if (!xdr_opaque(xdr, *data, *len)) return false;
#endif
    }
    return true;
}

bool xdr_string(xdr_t *xdr, char *str, uint32_t maxlen) {
    if (xdr->error) return false;
    
    if (xdr->encoding) {
        uint32_t len = strlen(str);
        if (len > maxlen) {
            xdr->error = true;
            return false;
        }
        if (!xdr_uint32(xdr, &len)) return false;
        if (!xdr_opaque(xdr, (uint8_t *)str, len)) return false;
    } else {
#ifdef RUST_XDR
        extern bool xdr_decode_string_rs(xdr_t *xdr, char *str, uint32_t maxlen);
        return xdr_decode_string_rs(xdr, str, maxlen);
#else
        uint32_t len;
        if (!xdr_uint32(xdr, &len)) return false;
        if (len >= maxlen) {  // Need room for null terminator
            xdr->error = true;
            return false;
        }
        if (!xdr_opaque(xdr, (uint8_t *)str, len)) return false;
        str[len] = '\0';
#endif
    }
    return true;
}

bool xdr_string_len(xdr_t *xdr, char *str, uint32_t *len, uint32_t maxlen) {
    if (xdr->error) return false;
    
    if (xdr->encoding) {
        if (*len > maxlen) {
            xdr->error = true;
            return false;
        }
        if (!xdr_uint32(xdr, len)) return false;
        if (!xdr_opaque(xdr, (uint8_t *)str, *len)) return false;
    } else {
#ifdef RUST_XDR
        extern bool xdr_decode_string_len_rs(xdr_t *xdr, char *str,
                                             uint32_t *len, uint32_t maxlen);
        return xdr_decode_string_len_rs(xdr, str, len, maxlen);
#else
        if (!xdr_uint32(xdr, len)) return false;
        if (*len >= maxlen) {
            xdr->error = true;
            return false;
        }
        if (!xdr_opaque(xdr, (uint8_t *)str, *len)) return false;
        str[*len] = '\0';
#endif
    }
    return true;
}

bool xdr_skip(xdr_t *xdr, size_t len) {
    if (xdr->error) return false;
#ifdef RUST_XDR
    if (!xdr->encoding) {
        extern bool xdr_decode_skip_rs(xdr_t *xdr, size_t len);
        return xdr_decode_skip_rs(xdr, len);
    }
#endif
    size_t aligned = xdr_aligned_len(len);
    if (xdr->pos + aligned > xdr->size) {
        xdr->error = true;
        return false;
    }
    xdr->pos += aligned;
    return true;
}

// NFS file handle
bool xdr_nfs_fh3(xdr_t *xdr, uint8_t *fh_data, uint32_t *fh_len) {
    if (xdr->encoding) {
        if (!xdr_uint32(xdr, fh_len)) return false;
        if (!xdr_opaque(xdr, fh_data, *fh_len)) return false;
    } else {
#ifdef RUST_XDR
        extern bool xdr_decode_nfs_fh3_rs(xdr_t *xdr, uint8_t *fh_data,
                                          uint32_t *fh_len);
        return xdr_decode_nfs_fh3_rs(xdr, fh_data, fh_len);
#else
        if (!xdr_uint32(xdr, fh_len)) return false;
        if (*fh_len > 64) {  // NFS3_FHSIZE
            xdr->error = true;
            return false;
        }
        if (!xdr_opaque(xdr, fh_data, *fh_len)) return false;
#endif
    }
    return true;
}

// NFS time
bool xdr_nfs_time3(xdr_t *xdr, uint32_t *seconds, uint32_t *nseconds) {
    if (!xdr_uint32(xdr, seconds)) return false;
    if (!xdr_uint32(xdr, nseconds)) return false;
    return true;
}

// ============================================================================
// RPC UDP Callback
// ============================================================================

static void rpc_udp_callback(uint32_t src_ip, uint16_t src_port,
                              const void *data, uint16_t length) {
    (void)src_ip;
    (void)src_port;
    
    if (length < 8) return;  // Minimum RPC reply header
    
    // Get XID from response
    uint32_t xid;
    memcpy(&xid, data, 4);
    xid = ntohl(xid);
    
    // Store response
    if (1) {
        memcpy((void *)rpc_response_buffer, data, length);
        rpc_response_len = length;
        rpc_response_xid = xid;
        rpc_response_ready = true;
    }
}

// ============================================================================
// RPC Initialization
// ============================================================================

void rpc_init(void) {
    memset(rpc_clients, 0, sizeof(rpc_clients));
    memset(rpc_transactions, 0, sizeof(rpc_transactions));
    rpc_global_xid = 1;
    rpc_response_ready = false;
    
    kprintf("[RPC] Subsystem initialized\n");
}

// ============================================================================
// RPC Client Management
// ============================================================================

int rpc_create_client(uint32_t server_ip, uint32_t program, uint32_t version, bool use_tcp) {
    // Find free slot
    int idx = -1;
    for (int i = 0; i < RPC_MAX_CLIENTS; i++) {
        if (!rpc_clients[i].active) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        kprintf("[RPC] No free client slots\n");
        return -1;
    }
    
    rpc_client_t *client = &rpc_clients[idx];
    memset(client, 0, sizeof(rpc_client_t));
    
    client->active = true;
    client->server_ip = server_ip;
    client->port = 0;  // Will be set via portmapper or explicitly
    client->program = program;
    client->version = version;
    client->xid = rpc_global_xid++;
    client->use_tcp = use_tcp;
    client->tcp_socket = -1;
    
    // Default AUTH_UNIX credentials
    client->auth.stamp = 0;
    strcpy(client->auth.machine, "maytera");
    client->auth.uid = 0;  // root
    client->auth.gid = 0;
    client->auth.gids_len = 0;
    
    if (rpc_debug) {
        kprintf("[RPC] Created client %d for program %u version %u\n",
                     idx, program, version);
    }
    
    return idx;
}

void rpc_destroy_client(int client_idx) {
    if (client_idx < 0 || client_idx >= RPC_MAX_CLIENTS) return;
    
    rpc_client_t *client = &rpc_clients[client_idx];
    if (!client->active) return;
    
    if (client->tcp_socket >= 0) {
        tcp_close(client->tcp_socket);
    }
    
    memset(client, 0, sizeof(rpc_client_t));
}

void rpc_set_auth_unix(int client_idx, uint32_t uid, uint32_t gid, const char *machine) {
    if (client_idx < 0 || client_idx >= RPC_MAX_CLIENTS) return;
    
    rpc_client_t *client = &rpc_clients[client_idx];
    if (!client->active) return;
    
    client->auth.uid = uid;
    client->auth.gid = gid;
    if (machine) {
        strncpy(client->auth.machine, machine, sizeof(client->auth.machine) - 1);
        client->auth.machine[sizeof(client->auth.machine) - 1] = '\0';
    }
}

void rpc_set_port(int client_idx, uint16_t port) {
    if (client_idx < 0 || client_idx >= RPC_MAX_CLIENTS) return;
    
    rpc_client_t *client = &rpc_clients[client_idx];
    if (!client->active) return;
    
    client->port = port;
}

// ============================================================================
// RPC Call Building
// ============================================================================

static void encode_auth_unix(xdr_t *xdr, rpc_auth_unix_t *auth) {
    // AUTH_UNIX flavor
    uint32_t flavor = AUTH_UNIX;
    xdr_uint32(xdr, &flavor);
    
    // Calculate credential body length
    size_t start = xdr_getpos(xdr);
    uint32_t dummy = 0;
    xdr_uint32(xdr, &dummy);  // Placeholder for length
    
    size_t body_start = xdr_getpos(xdr);
    
    // Encode AUTH_UNIX body
    xdr_uint32(xdr, &auth->stamp);
    xdr_string(xdr, auth->machine, 64);
    xdr_uint32(xdr, &auth->uid);
    xdr_uint32(xdr, &auth->gid);
    xdr_uint32(xdr, &auth->gids_len);
    for (uint32_t i = 0; i < auth->gids_len; i++) {
        xdr_uint32(xdr, &auth->gids[i]);
    }
    
    // Go back and fill in length
    size_t body_len = xdr_getpos(xdr) - body_start;
    size_t end = xdr_getpos(xdr);
    xdr_setpos(xdr, start);
    uint32_t len32 = (uint32_t)body_len;
    xdr_uint32(xdr, &len32);
    xdr_setpos(xdr, end);
}

static void encode_auth_none(xdr_t *xdr) {
    uint32_t flavor = AUTH_NONE;
    uint32_t length = 0;
    xdr_uint32(xdr, &flavor);
    xdr_uint32(xdr, &length);
}

static xdr_t call_xdr;

xdr_t *rpc_call_begin(int client_idx, uint32_t procedure) {
    if (client_idx < 0 || client_idx >= RPC_MAX_CLIENTS) return NULL;
    
    rpc_client_t *client = &rpc_clients[client_idx];
    if (!client->active) return NULL;
    
    rpc_transaction_t *tx = &rpc_transactions[client_idx];
    
    // Initialize XDR for encoding
    xdr_init_encode(&call_xdr, tx->send_buf, sizeof(tx->send_buf));
    
    // Generate transaction ID
    tx->xid = rpc_global_xid++;
    
    // Encode RPC call header
    uint32_t val;
    
    val = tx->xid;
    xdr_uint32(&call_xdr, &val);          // xid
    val = RPC_CALL;
    xdr_uint32(&call_xdr, &val);          // msg_type
    val = RPC_VERSION;
    xdr_uint32(&call_xdr, &val);          // rpc_version
    val = client->program;
    xdr_uint32(&call_xdr, &val);          // program
    val = client->version;
    xdr_uint32(&call_xdr, &val);          // version
    val = procedure;
    xdr_uint32(&call_xdr, &val);          // procedure
    
    // Encode credentials (AUTH_UNIX)
    encode_auth_unix(&call_xdr, &client->auth);
    
    // Encode verifier (AUTH_NONE)
    encode_auth_none(&call_xdr);
    
    // Return XDR for caller to encode procedure arguments
    return &call_xdr;
}

// ============================================================================
// RPC Call Sending
// ============================================================================

static xdr_t reply_xdr;

xdr_t *rpc_call_send(int client_idx, int timeout_ms) {
    if (client_idx < 0 || client_idx >= RPC_MAX_CLIENTS) return NULL;
    
    rpc_client_t *client = &rpc_clients[client_idx];
    if (!client->active) return NULL;
    
    rpc_transaction_t *tx = &rpc_transactions[client_idx];
    tx->send_len = xdr_getpos(&call_xdr);
    
    if (client->port == 0) {
        kprintf("[RPC] Error: Port not set for client %d\n", client_idx);
        return NULL;
    }
    
    if (rpc_debug) {
        kprintf("[RPC] Sending %u bytes to %u.%u.%u.%u:%u (xid=%u)\n",
                     (unsigned)tx->send_len,
                     (client->server_ip >> 24) & 0xFF,
                     (client->server_ip >> 16) & 0xFF,
                     (client->server_ip >> 8) & 0xFF,
                     client->server_ip & 0xFF,
                     client->port, tx->xid);
    }
    
    if (client->use_tcp) {
        // TCP path (task #317 pass 3: pump the NIC + yield, ARP pre-resolve,
        // connect with retries; mirrors net/smb.c smb_mount_auth).
        if (client->tcp_socket < 0) {
            // Pre-resolve ARP so the very first SYN goes out with a known MAC;
            // TCP gives up after ~5 SYN retransmits before ARP would resolve.
            {
                uint8_t pmac[6];
                uint64_t astart = timer_ticks;
                while (!arp_resolve(client->server_ip, pmac)) {
                    rpc_net_pump();
                    if (timer_ticks - astart > rpc_ms_to_ticks(5000)) break;
                }
            }
            int connected = 0;
            for (int attempt = 0; attempt < 4 && !connected; attempt++) {
                client->tcp_socket = tcp_socket();
                if (client->tcp_socket < 0) {
                    kprintf("[RPC] Failed to create TCP socket\n");
                    return NULL;
                }
                int cr = tcp_connect(client->tcp_socket, client->server_ip, client->port);
                if (cr < 0 && cr != TCP_ERR_IN_PROGRESS) {
                    tcp_close(client->tcp_socket);
                    client->tcp_socket = -1;
                    for (int k = 0; k < 50; k++) rpc_net_pump();
                    continue;
                }
                uint64_t cstart = timer_ticks;
                uint64_t ctimeout = rpc_ms_to_ticks(6000);
                while (!tcp_is_connected(client->tcp_socket)) {
                    rpc_net_pump();
                    if (tcp_get_state(client->tcp_socket) == TCP_STATE_CLOSED) break;
                    if (timer_ticks - cstart > ctimeout) break;
                }
                if (tcp_get_state(client->tcp_socket) == TCP_STATE_ESTABLISHED) {
                    connected = 1;
                    break;
                }
                tcp_close(client->tcp_socket);
                client->tcp_socket = -1;
                for (int k = 0; k < 50; k++) rpc_net_pump();
            }
            if (!connected) {
                kprintf("[RPC] TCP connect failed (all retries)\n");
                return NULL;
            }
        }

        // For TCP, prepend 4-byte record marker (MSB=1 for last fragment).
        // Build in a static scratch (RPC_MAX_MSG_SIZE+4 is too big for the stack).
        static uint8_t rpc_tcp_sendscratch[RPC_MAX_MSG_SIZE + 4];
        uint8_t *tcp_buf = rpc_tcp_sendscratch;
        uint32_t rm = htonl(0x80000000 | (uint32_t)tx->send_len);
        memcpy(tcp_buf, &rm, 4);
        memcpy(tcp_buf + 4, tx->send_buf, tx->send_len);

        // Robust send-all with pump on WOULD_BLOCK / partial sends.
        {
            uint32_t total = (uint32_t)tx->send_len + 4;
            uint32_t sent = 0;
            uint64_t sstart = timer_ticks;
            uint64_t sdl = rpc_ms_to_ticks(10000);
            while (sent < total) {
                uint16_t chunk = (total - sent) > 1400 ? 1400 : (uint16_t)(total - sent);
                int s = tcp_send(client->tcp_socket, tcp_buf + sent, chunk);
                if (s > 0) { sent += s; sstart = timer_ticks; rpc_net_pump(); }
                else if (s == TCP_ERR_WOULD_BLOCK) { rpc_net_pump(); }
                else { kprintf("[RPC] TCP send failed\n"); return NULL; }
                if (timer_ticks - sstart > sdl) { kprintf("[RPC] TCP send timeout\n"); return NULL; }
            }
        }

        // Wait for response, pumping the stack each iteration.
        tx->recv_len = 0;
        uint64_t rstart = timer_ticks;
        uint64_t rdl = rpc_ms_to_ticks(timeout_ms);
        while (1) {
            rpc_net_pump();
            uint32_t want = (uint32_t)sizeof(tx->recv_buf) - (uint32_t)tx->recv_len;
            if (want > 32000) want = 32000;
            int n = tcp_recv(client->tcp_socket, tx->recv_buf + tx->recv_len, (uint16_t)want);
            if (n > 0) {
                tx->recv_len += n;
                rstart = timer_ticks;
                if (tx->recv_len >= 4) {
                    uint32_t rm2;
                    memcpy(&rm2, tx->recv_buf, 4);
                    rm2 = ntohl(rm2);
                    uint32_t msg_len = rm2 & 0x7FFFFFFF;
                    if (tx->recv_len >= msg_len + 4) {
                        memmove(tx->recv_buf, tx->recv_buf + 4, msg_len);
                        tx->recv_len = msg_len;
                        break;
                    }
                }
            } else if (n < 0 && n != TCP_ERR_WOULD_BLOCK) {
                kprintf("[RPC] TCP recv error %d\n", n);
                return NULL;
            }
            if (timer_ticks - rstart > rdl) {
                kprintf("[RPC] TCP receive timeout (%u bytes)\n", (unsigned)tx->recv_len);
                return NULL;
            }
        }

        if (tx->recv_len == 0) {
            kprintf("[RPC] TCP receive timeout\n");
            return NULL;
        }

    } else {
        // UDP path
        uint16_t local_port = 600 + client_idx;  // Ephemeral port
        
        // Bind UDP callback
        rpc_response_ready = false;
        udp_bind(local_port, rpc_udp_callback);
        
        // Send UDP packet
        if (udp_send(client->server_ip, local_port, client->port, 
                     tx->send_buf, tx->send_len) < 0) {
            kprintf("[RPC] UDP send failed\n");
            udp_unbind(local_port);
            return NULL;
        }
        
        // Wait for response with timeout (pump the NIC each iteration so the
        // UDP callback actually fires - task #317 pass 3).
        uint64_t ustart = timer_ticks;
        uint64_t udl = rpc_ms_to_ticks(timeout_ms);
        while (!rpc_response_ready) {
            rpc_net_pump();
            if (timer_ticks - ustart > udl) break;
        }

        udp_unbind(local_port);
        
        if (!rpc_response_ready) {
            kprintf("[RPC] UDP receive timeout\n");
            return NULL;
        }
        
        // Verify XID matches
        if (rpc_response_xid != tx->xid) {
            kprintf("[RPC] XID mismatch: expected %u, got %u\n",
                         tx->xid, rpc_response_xid);
            return NULL;
        }
        
        // Copy response
        memcpy(tx->recv_buf, (void *)rpc_response_buffer, rpc_response_len);
        tx->recv_len = rpc_response_len;
    }
    
    if (rpc_debug) {
        kprintf("[RPC] Received %u bytes\n", (unsigned)tx->recv_len);
    }
    
    // Initialize XDR for decoding response
    xdr_init_decode(&reply_xdr, tx->recv_buf, tx->recv_len);
    
    // Parse reply header
    uint32_t xid, msg_type, reply_stat;
    xdr_uint32(&reply_xdr, &xid);
    xdr_uint32(&reply_xdr, &msg_type);
    
    if (msg_type != RPC_REPLY) {
        kprintf("[RPC] Expected REPLY, got %u\n", msg_type);
        return NULL;
    }
    
    xdr_uint32(&reply_xdr, &reply_stat);
    
    if (reply_stat == RPC_MSG_ACCEPTED) {
        // Skip verifier
        uint32_t verf_flavor, verf_len;
        xdr_uint32(&reply_xdr, &verf_flavor);
        xdr_uint32(&reply_xdr, &verf_len);
        xdr_skip(&reply_xdr, verf_len);
        
        // Get accept status
        uint32_t accept_stat;
        xdr_uint32(&reply_xdr, &accept_stat);
        
        if (accept_stat != RPC_SUCCESS) {
            kprintf("[RPC] Call failed with status %u\n", accept_stat);
            return NULL;
        }
        
        // Return XDR positioned at procedure results
        return &reply_xdr;
        
    } else if (reply_stat == RPC_MSG_DENIED) {
        uint32_t reject_stat;
        xdr_uint32(&reply_xdr, &reject_stat);
        kprintf("[RPC] Message denied: %u\n", reject_stat);
        return NULL;
    }
    
    return NULL;
}

uint32_t rpc_get_status(int client_idx) {
    (void)client_idx;
    // TODO: Track status per client
    return 0;
}

// ============================================================================
// Portmapper
// ============================================================================

int portmap_getport(uint32_t server_ip, uint32_t program, uint32_t version, uint32_t proto) {
    // Create temporary client for portmapper. task #317 pass 3: talk to the
    // portmapper over TCP (port 111) - UDP datagram delivery in this stack is
    // far less reliable than the pumped TCP path, and rpcbind speaks TCP fine.
    int client = rpc_create_client(server_ip, PORTMAP_PROGRAM, 2, true);
    if (client < 0) return -1;
    
    rpc_set_port(client, PORTMAP_PORT);
    
    // Build GETPORT request
    xdr_t *xdr = rpc_call_begin(client, PMAPPROC_GETPORT);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Encode mapping: program, version, protocol, port (ignored)
    xdr_uint32(xdr, &program);
    xdr_uint32(xdr, &version);
    xdr_uint32(xdr, &proto);  // IPPROTO_UDP=17, IPPROTO_TCP=6
    uint32_t zero = 0;
    xdr_uint32(xdr, &zero);
    
    // Send and get response
    xdr_t *reply = rpc_call_send(client, 5000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Decode port number
    uint32_t port;
    if (!xdr_uint32(reply, &port)) {
        rpc_destroy_client(client);
        return -1;
    }
    
    rpc_destroy_client(client);
    
    if (rpc_debug) {
        kprintf("[RPC] Portmapper: program %u version %u -> port %u\n",
                     program, version, port);
    }
    
    return (int)port;
}

int rpc_get_port(uint32_t server_ip, uint32_t program, uint32_t version, bool tcp) {
    return portmap_getport(server_ip, program, version, tcp ? 6 : 17);
}

// ============================================================================
// Helper Functions
// ============================================================================

int rpc_call_null(int client_idx) {
    xdr_t *xdr = rpc_call_begin(client_idx, 0);  // Procedure 0 = NULL
    if (!xdr) return -1;
    
    xdr_t *reply = rpc_call_send(client_idx, 5000);
    return reply ? 0 : -1;
}

// ============================================================================
// Debug Functions
// ============================================================================

void rpc_print_client(int client_idx) {
    if (client_idx < 0 || client_idx >= RPC_MAX_CLIENTS) return;
    
    rpc_client_t *client = &rpc_clients[client_idx];
    if (!client->active) {
        kprintf("[RPC] Client %d: inactive\n", client_idx);
        return;
    }
    
    kprintf("[RPC] Client %d:\n", client_idx);
    kprintf("  Server: %u.%u.%u.%u:%u\n",
                 (client->server_ip >> 24) & 0xFF,
                 (client->server_ip >> 16) & 0xFF,
                 (client->server_ip >> 8) & 0xFF,
                 client->server_ip & 0xFF,
                 client->port);
    kprintf("  Program: %u, Version: %u\n", client->program, client->version);
    kprintf("  Transport: %s\n", client->use_tcp ? "TCP" : "UDP");
    kprintf("  Auth: uid=%u gid=%u machine=%s\n",
                 client->auth.uid, client->auth.gid, client->auth.machine);
}

const char *rpc_strerror(uint32_t status) {
    switch (status) {
        case RPC_SUCCESS:       return "Success";
        case RPC_PROG_UNAVAIL:  return "Program unavailable";
        case RPC_PROG_MISMATCH: return "Program version mismatch";
        case RPC_PROC_UNAVAIL:  return "Procedure unavailable";
        case RPC_GARBAGE_ARGS:  return "Garbage arguments";
        case RPC_SYSTEM_ERR:    return "System error";
        default:                return "Unknown error";
    }
}

void rpc_set_debug(bool enabled) {
    rpc_debug = enabled;
}

// =============================================================================
// #404 batch-3 (LAST parser-tier seam): boot-time [RUST-DIFF] XDR decode self-
// test. Builds a real NFS LOOKUP3resok-shaped XDR reply, decodes it via BOTH the
// LIVE path (xdr_decode_*_rs under -DRUST_XDR, else C) and the verbatim _c twins,
// and asserts they agree field-by-field + cursor-exact. Defense-in-depth
// (source-bounded on 64-bit, NO reachable OOB). One [RUST-DIFF] + one [RUST-PERF]
// xdr line. Bounded, runs once from main.c (#426, no busy-wait).
// =============================================================================
static inline uint64_t xdr_rdtsc(void) {
    uint32_t a, d;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(a), "=d"(d));
    return ((uint64_t)d << 32) | a;
}

void xdr_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    static uint8_t b[128]; size_t p = 0; uint8_t fh[16];
    for (int i = 0; i < 16; i++) fh[i] = 0x10 + i;
    #define XDR_PU32(v) do{uint32_t _v=(v);uint32_t n=htonl(_v);memcpy(b+p,&n,4);p+=4;}while(0)
    XDR_PU32(0); XDR_PU32(16); memcpy(b + p, fh, 16); p += 16;
    XDR_PU32(10); memcpy(b + p, "readme.txt", 10); p += 10; while (p & 3) b[p++] = 0;
    XDR_PU32(0644); XDR_PU32(1); XDR_PU32(1000); XDR_PU32(1000);
    for (int k = 0; k < 8; k++) XDR_PU32(0x01020304 + k);     // 4x u64 = 8x u32
    // Final field is a BARE fixed-length 5-byte opaque (matching the xdr_opaque
    // (,5) decode below): 5 data bytes + 3 align pad, NO 4-byte length prefix.
    memcpy(b + p, "HELLO", 5); p += 5; while (p & 3) b[p++] = 0;
    #undef XDR_PU32
    size_t total = p;

    // LIVE path (rs under -DRUST_XDR, else c).
    xdr_t live; xdr_init_decode(&live, b, total);
    uint32_t lu, lfl; uint8_t lfd[64]; char lnm[32]; uint64_t lq[4]; uint32_t lg[4]; uint8_t lo[8];
    xdr_uint32(&live, &lu);
    xdr_nfs_fh3(&live, lfd, &lfl);
    xdr_string(&live, lnm, 32);
    for (int k = 0; k < 4; k++) xdr_uint32(&live, &lg[k]);
    for (int k = 0; k < 4; k++) xdr_uint64(&live, &lq[k]);
    xdr_opaque(&live, lo, 5);

    // Pure-C oracle via the verbatim _c twins.
    xdr_t ref; xdr_init_decode(&ref, b, total);
    uint32_t ru, rfl; uint8_t rfd[64]; char rnm[32]; uint64_t rq[4]; uint32_t rg[4]; uint8_t ro[8];
    xdr_uint32_c(&ref, &ru);
    xdr_nfs_fh3_c(&ref, rfd, &rfl);
    xdr_string_c(&ref, rnm, 32);
    for (int k = 0; k < 4; k++) xdr_uint32_c(&ref, &rg[k]);
    for (int k = 0; k < 4; k++) xdr_uint64_c(&ref, &rq[k]);
    xdr_opaque_c(&ref, ro, 5);

    int mism = 0;
    if (live.error != ref.error || live.pos != ref.pos) mism++;
    if (lu != ru || lfl != rfl || memcmp(lfd, rfd, 16) || strcmp(lnm, rnm)) mism++;
    for (int k = 0; k < 4; k++) if (lg[k] != rg[k] || lq[k] != rq[k]) mism++;
    if (memcmp(lo, ro, 5)) mism++;
    int ok = (!live.error && live.pos == total && lfl == 16 && !strcmp(lnm, "readme.txt") && mism == 0);

    kprintf("[RUST-DIFF] xdr decode NFS-reply: pos=%u/%u fh=%u name=%s mism=%d %s (LIVE=%s)\n",
            (unsigned)live.pos, (unsigned)total, lfl, lnm, mism, ok ? "PASS" : "FAIL",
#ifdef RUST_XDR
            "rust");
#else
            "c");
#endif
    bootlog_write("[RUST-DIFF] xdr decode NFS-reply pos=%u/%u mism=%d %s",
                  (unsigned)live.pos, (unsigned)total, mism, ok ? "PASS" : "FAIL");

    // [RUST-PERF] full 14-field reply decode.
    const long IT = 200000; volatile int sink = 0;
    uint64_t t0 = xdr_rdtsc();
    for (long i = 0; i < IT; i++) {
        xdr_t x; xdr_init_decode(&x, b, total);
        uint32_t uu, fll; uint8_t fdd[64]; char nn[32]; uint64_t qq; uint8_t oo[8];
        xdr_uint32(&x, &uu); xdr_nfs_fh3(&x, fdd, &fll); xdr_string(&x, nn, 32);
        for (int k = 0; k < 4; k++) xdr_uint32(&x, &uu);
        for (int k = 0; k < 4; k++) xdr_uint64(&x, &qq);
        xdr_opaque(&x, oo, 5); sink += x.pos;
    }
    uint64_t t1 = xdr_rdtsc();
    kprintf("[RUST-PERF] xdr decode: %lu cyc / full-reply (sink=%d)\n",
            (unsigned long)((t1 - t0) / IT), sink);
}
