// rpc.h - Sun RPC (Remote Procedure Call) Protocol Implementation
// Implements RPC for use with NFS (RFC 1831, RFC 4506)
#ifndef RPC_H
#define RPC_H

#include "../types.h"

// ============================================================================
// RPC Constants
// ============================================================================

#define RPC_VERSION         2       // RPC protocol version
#define PORTMAP_PORT        111     // Portmapper/rpcbind port

// RPC program numbers
#define PORTMAP_PROGRAM     100000  // Portmapper
#define NFS_PROGRAM         100003  // NFS
#define MOUNT_PROGRAM       100005  // Mount daemon

// RPC message types
#define RPC_CALL            0
#define RPC_REPLY           1

// RPC reply status
#define RPC_MSG_ACCEPTED    0
#define RPC_MSG_DENIED      1

// Accept status (when MSG_ACCEPTED)
#define RPC_SUCCESS         0       // RPC executed successfully
#define RPC_PROG_UNAVAIL    1       // Program not available
#define RPC_PROG_MISMATCH   2       // Program version mismatch
#define RPC_PROC_UNAVAIL    3       // Procedure not available
#define RPC_GARBAGE_ARGS    4       // Bad arguments
#define RPC_SYSTEM_ERR      5       // System error on server

// Reject status (when MSG_DENIED)
#define RPC_RPC_MISMATCH    0       // RPC version mismatch
#define RPC_AUTH_ERROR      1       // Authentication failure

// Authentication flavors
#define AUTH_NONE           0       // No authentication
#define AUTH_UNIX           1       // Unix/SYS authentication
#define AUTH_SHORT          2       // Short hand unix auth
#define AUTH_DES            3       // DES-based authentication

// Buffer sizes
#define RPC_MAX_MSG_SIZE    65536   // Max RPC message size
#define RPC_MAX_CRED_LEN    400     // Max credential length
#define RPC_MAX_VERF_LEN    400     // Max verifier length
#define XDR_MAX_STRING_LEN  1024    // Max XDR string length

// Portmapper procedures
#define PMAPPROC_NULL       0
#define PMAPPROC_SET        1
#define PMAPPROC_UNSET      2
#define PMAPPROC_GETPORT    3
#define PMAPPROC_DUMP       4
#define PMAPPROC_CALLIT     5

// ============================================================================
// XDR (External Data Representation) Types
// ============================================================================

// XDR buffer for encoding/decoding
typedef struct {
    uint8_t *data;          // Data buffer
    size_t   size;          // Buffer size
    size_t   pos;           // Current position
    bool     encoding;      // True if encoding, false if decoding
    bool     error;         // Error flag
} xdr_t;

// ============================================================================
// RPC Authentication Structures
// ============================================================================

// Auth credential/verifier header
typedef struct {
    uint32_t flavor;        // Auth flavor (AUTH_NONE, AUTH_UNIX, etc.)
    uint32_t length;        // Length of opaque auth data
} rpc_auth_t;

// AUTH_UNIX credential data
typedef struct {
    uint32_t stamp;         // Timestamp
    char     machine[64];   // Machine name
    uint32_t uid;           // User ID
    uint32_t gid;           // Group ID
    uint32_t gids_len;      // Number of auxiliary GIDs
    uint32_t gids[16];      // Auxiliary group IDs
} rpc_auth_unix_t;

// ============================================================================
// RPC Message Structures
// ============================================================================

// RPC Call message header
typedef struct {
    uint32_t xid;           // Transaction ID
    uint32_t msg_type;      // Message type (RPC_CALL)
    uint32_t rpc_version;   // RPC version (2)
    uint32_t program;       // Program number
    uint32_t version;       // Program version
    uint32_t procedure;     // Procedure number
    rpc_auth_t cred;        // Credentials
    rpc_auth_t verf;        // Verifier
} rpc_call_header_t;

// RPC Reply message header
typedef struct {
    uint32_t xid;           // Transaction ID
    uint32_t msg_type;      // Message type (RPC_REPLY)
    uint32_t reply_status;  // MSG_ACCEPTED or MSG_DENIED
} rpc_reply_header_t;

// Accepted reply body
typedef struct {
    rpc_auth_t verf;        // Verifier
    uint32_t accept_stat;   // Accept status
} rpc_accepted_reply_t;

// ============================================================================
// RPC Client State
// ============================================================================

#define RPC_MAX_CLIENTS     8

typedef struct {
    bool     active;            // Is this client active?
    uint32_t server_ip;         // Server IP address
    uint16_t port;              // Server port
    uint32_t program;           // RPC program number
    uint32_t version;           // Program version
    uint32_t xid;               // Transaction ID counter
    int      tcp_socket;        // TCP socket (-1 for UDP)
    bool     use_tcp;           // Use TCP (true) or UDP (false)
    rpc_auth_unix_t auth;       // AUTH_UNIX credentials
} rpc_client_t;

// ============================================================================
// RPC Transaction Buffer
// ============================================================================

typedef struct {
    uint8_t  send_buf[RPC_MAX_MSG_SIZE];    // Send buffer
    size_t   send_len;                       // Send buffer length
    uint8_t  recv_buf[RPC_MAX_MSG_SIZE];    // Receive buffer  
    size_t   recv_len;                       // Receive buffer length
    uint32_t xid;                            // Transaction ID
} rpc_transaction_t;

// ============================================================================
// XDR Functions
// ============================================================================

// Initialize XDR for encoding
void xdr_init_encode(xdr_t *xdr, uint8_t *buffer, size_t size);

// Initialize XDR for decoding
void xdr_init_decode(xdr_t *xdr, const uint8_t *buffer, size_t size);

// Reset XDR position
void xdr_reset(xdr_t *xdr);

// Get current position
size_t xdr_getpos(xdr_t *xdr);

// Set position
bool xdr_setpos(xdr_t *xdr, size_t pos);

// Check for error
bool xdr_error(xdr_t *xdr);

// Get remaining bytes
size_t xdr_remaining(xdr_t *xdr);

// ============================================================================
// XDR Primitive Encoding/Decoding
// ============================================================================

// Integer types
bool xdr_uint32(xdr_t *xdr, uint32_t *val);
bool xdr_int32(xdr_t *xdr, int32_t *val);
bool xdr_uint64(xdr_t *xdr, uint64_t *val);
bool xdr_int64(xdr_t *xdr, int64_t *val);
bool xdr_bool(xdr_t *xdr, bool *val);

// Enum (same as uint32)
bool xdr_enum(xdr_t *xdr, int *val);

// Opaque data (fixed length)
bool xdr_opaque(xdr_t *xdr, uint8_t *data, size_t len);

// Opaque data (variable length)
bool xdr_bytes(xdr_t *xdr, uint8_t **data, uint32_t *len, uint32_t maxlen);

// String (null-terminated)
bool xdr_string(xdr_t *xdr, char *str, uint32_t maxlen);

// String with length prefix
bool xdr_string_len(xdr_t *xdr, char *str, uint32_t *len, uint32_t maxlen);

// Skip bytes (for decoding)
bool xdr_skip(xdr_t *xdr, size_t len);

// ============================================================================
// XDR NFS-Specific Types
// ============================================================================

// File handle (variable length opaque)
bool xdr_nfs_fh3(xdr_t *xdr, uint8_t *fh_data, uint32_t *fh_len);

// Time value
bool xdr_nfs_time3(xdr_t *xdr, uint32_t *seconds, uint32_t *nseconds);

// File attributes
bool xdr_nfs_fattr3(xdr_t *xdr, void *attrs);

// ============================================================================
// RPC Client API
// ============================================================================

// Initialize RPC subsystem
void rpc_init(void);

// Create an RPC client
// Returns client index on success, -1 on failure
int rpc_create_client(uint32_t server_ip, uint32_t program, uint32_t version, bool use_tcp);

// Destroy an RPC client
void rpc_destroy_client(int client);

// Set authentication credentials
void rpc_set_auth_unix(int client, uint32_t uid, uint32_t gid, const char *machine);

// Set client port explicitly (otherwise uses portmapper)
void rpc_set_port(int client, uint16_t port);

// Get port from portmapper
int rpc_get_port(uint32_t server_ip, uint32_t program, uint32_t version, bool tcp);

// ============================================================================
// RPC Call Functions
// ============================================================================

// Begin an RPC call (starts building request)
// Returns pointer to XDR buffer for encoding arguments
xdr_t *rpc_call_begin(int client, uint32_t procedure);

// Send the RPC call and wait for reply
// Returns pointer to XDR buffer for decoding response, NULL on error
xdr_t *rpc_call_send(int client, int timeout_ms);

// Get the status of last RPC call
uint32_t rpc_get_status(int client);

// ============================================================================
// RPC Helper Functions
// ============================================================================

// Make a simple RPC call with no arguments
// Returns 0 on success, -1 on failure
int rpc_call_null(int client);

// Portmapper: get port for program/version
int portmap_getport(uint32_t server_ip, uint32_t program, uint32_t version, uint32_t proto);

// ============================================================================
// RPC Debug Functions
// ============================================================================

// Print RPC client info
void rpc_print_client(int client);

// Convert RPC error to string
const char *rpc_strerror(uint32_t status);

// Enable/disable RPC debug output
void rpc_set_debug(bool enabled);

#endif // RPC_H
