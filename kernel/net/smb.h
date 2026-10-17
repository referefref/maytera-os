// smb.h - SMB/CIFS Protocol Definitions for MayteraOS
// Implements SMB2/SMB3 client (MS-SMB2) with SMB1 fallback
#ifndef SMB_H
#define SMB_H

#include "../types.h"

// ============================================================================
// SMB Port and Version Definitions
// ============================================================================

#define SMB_PORT            445     // Direct SMB over TCP
#define SMB_NETBIOS_PORT    139     // SMB over NetBIOS (legacy)

// SMB protocol versions
#define SMB1_PROTOCOL       0x0100  // SMB 1.0 / CIFS
#define SMB2_PROTOCOL       0x0202  // SMB 2.0.2
#define SMB21_PROTOCOL      0x0210  // SMB 2.1
#define SMB30_PROTOCOL      0x0300  // SMB 3.0
#define SMB302_PROTOCOL     0x0302  // SMB 3.0.2
#define SMB311_PROTOCOL     0x0311  // SMB 3.1.1

// Protocol negotiation special value
#define SMB2_WILDCARD       0x02FF  // SMB2 any version

// ============================================================================
// SMB1 Protocol Constants (for negotiation fallback)
// ============================================================================

#define SMB1_MAGIC          0x424D53FF  // "\xFFSMB"
#define SMB1_HEADER_SIZE    32

// SMB1 Commands
#define SMB1_COM_NEGOTIATE          0x72
#define SMB1_COM_SESSION_SETUP      0x73
#define SMB1_COM_TREE_CONNECT       0x75
#define SMB1_COM_TREE_DISCONNECT    0x71
#define SMB1_COM_CLOSE              0x04
#define SMB1_COM_READ_ANDX          0x2E
#define SMB1_COM_WRITE_ANDX         0x2F
#define SMB1_COM_TRANSACTION2       0x32
#define SMB1_COM_FIND_CLOSE2        0x34
#define SMB1_COM_NT_CREATE_ANDX     0xA2

// SMB1 Flags
#define SMB1_FLAGS_REPLY            0x80
#define SMB1_FLAGS_CASE_INSENSITIVE 0x08

// SMB1 Flags2
#define SMB1_FLAGS2_LONG_NAMES      0x0001
#define SMB1_FLAGS2_EAS             0x0002
#define SMB1_FLAGS2_SECURITY_SIG    0x0004
#define SMB1_FLAGS2_NT_STATUS       0x4000
#define SMB1_FLAGS2_UNICODE         0x8000

// ============================================================================
// SMB2 Protocol Magic and Constants
// ============================================================================

#define SMB2_MAGIC              0x424D53FE  // "\xFESMB"
#define SMB2_HEADER_SIZE        64
#define SMB2_TRANSFORM_MAGIC    0x424D53FD  // "\xFDSMB" (encrypted)
// MS-SMB2 2.2.41: an encrypted message is prefixed by a 52-byte
// SMB2_TRANSFORM_HEADER. SMB2_TRANSFORM_MAGIC above was defined when this
// client was first written and was read by NO .c file in the tree until
// encryption landed: the constant for the feature existed, so a reader
// could reasonably believe the feature did.
#define SMB2_TRANSFORM_HEADER_SIZE  52

// SMB2 Header Flags
#define SMB2_FLAGS_SERVER_TO_REDIR      0x00000001
#define SMB2_FLAGS_ASYNC_COMMAND        0x00000002
#define SMB2_FLAGS_RELATED_OPERATIONS   0x00000004
#define SMB2_FLAGS_SIGNED               0x00000008
#define SMB2_FLAGS_PRIORITY_MASK        0x00000070
#define SMB2_FLAGS_DFS_OPERATIONS       0x10000000
#define SMB2_FLAGS_REPLAY_OPERATION     0x20000000

// ============================================================================
// SMB2 Command Codes
// ============================================================================

typedef enum {
    SMB2_NEGOTIATE          = 0x0000,
    SMB2_SESSION_SETUP      = 0x0001,
    SMB2_LOGOFF             = 0x0002,
    SMB2_TREE_CONNECT       = 0x0003,
    SMB2_TREE_DISCONNECT    = 0x0004,
    SMB2_CREATE             = 0x0005,
    SMB2_CLOSE              = 0x0006,
    SMB2_FLUSH              = 0x0007,
    SMB2_READ               = 0x0008,
    SMB2_WRITE              = 0x0009,
    SMB2_LOCK               = 0x000A,
    SMB2_IOCTL              = 0x000B,
    SMB2_CANCEL             = 0x000C,
    SMB2_ECHO               = 0x000D,
    SMB2_QUERY_DIRECTORY    = 0x000E,
    SMB2_CHANGE_NOTIFY      = 0x000F,
    SMB2_QUERY_INFO         = 0x0010,
    SMB2_SET_INFO           = 0x0011,
    SMB2_OPLOCK_BREAK       = 0x0012
} smb2_command_t;

// ============================================================================
// SMB2 Status Codes (NT Status)
// ============================================================================

typedef enum {
    STATUS_SUCCESS                  = 0x00000000,
    STATUS_MORE_PROCESSING_REQUIRED = 0xC0000016,
    STATUS_INVALID_PARAMETER        = 0xC000000D,
    STATUS_NO_SUCH_FILE             = 0xC000000F,
    STATUS_END_OF_FILE              = 0xC0000011,
    STATUS_ACCESS_DENIED            = 0xC0000022,
    STATUS_OBJECT_NAME_NOT_FOUND    = 0xC0000034,
    STATUS_OBJECT_NAME_COLLISION    = 0xC0000035,
    STATUS_OBJECT_PATH_NOT_FOUND    = 0xC000003A,
    STATUS_SHARING_VIOLATION        = 0xC0000043,
    STATUS_FILE_LOCK_CONFLICT       = 0xC0000054,
    STATUS_LOGON_FAILURE            = 0xC000006D,
    STATUS_ACCOUNT_DISABLED         = 0xC0000072,
    STATUS_WRONG_PASSWORD           = 0xC000006A,
    STATUS_NO_SUCH_USER             = 0xC0000064,
    STATUS_PASSWORD_EXPIRED         = 0xC0000071,
    STATUS_INSUFFICIENT_RESOURCES   = 0xC000009A,
    STATUS_NOT_SUPPORTED            = 0xC00000BB,
    STATUS_NETWORK_NAME_DELETED     = 0xC00000C9,
    STATUS_BAD_NETWORK_NAME         = 0xC00000CC,
    STATUS_REQUEST_NOT_ACCEPTED     = 0xC00000D0,
    STATUS_BUFFER_TOO_SMALL         = 0xC0000023,
    STATUS_DIRECTORY_NOT_EMPTY      = 0xC0000101,
    STATUS_NOT_A_DIRECTORY          = 0xC0000103,
    STATUS_CANCELLED                = 0xC0000120,
    STATUS_FILE_CLOSED              = 0xC0000128,
    STATUS_NOTIFY_ENUM_DIR          = 0x0000010C,
    STATUS_NO_MORE_FILES            = 0x80000006,
    // #749: MS-SMB2 3.3.4.2 interim response. A server that cannot answer
    // within its own deadline sends this with SMB2_FLAGS_ASYNC_COMMAND set and
    // the SAME MessageId, then the real answer later. It is NOT a failure, and
    // a client that treats it as one reports a phantom error on a request the
    // server is still working on.
    STATUS_PENDING                  = 0x00000103
} smb2_status_t;

// ============================================================================
// SMB2 Header Structures
// ============================================================================

// SMB2 Packet Header (64 bytes)
typedef struct {
    uint32_t protocol_id;       // 0xFE 'S' 'M' 'B'
    uint16_t structure_size;    // Always 64
    uint16_t credit_charge;     // Credits consumed
    uint32_t status;            // NT status code (response)
    uint16_t command;           // Command code
    uint16_t credit_req_resp;   // Credits requested/granted
    uint32_t flags;             // SMB2_FLAGS_*
    uint32_t next_command;      // Offset to next command (compound)
    uint64_t message_id;        // Message sequence number
    uint32_t process_id;        // Process ID (async: high bits of async_id)
    uint32_t tree_id;           // Tree ID
    uint64_t session_id;        // Session ID
    uint8_t  signature[16];     // Message signature (if signed)
} __attribute__((packed)) smb2_header_t;

// SMB1 Header (32 bytes) for negotiation
typedef struct {
    uint32_t protocol_id;       // 0xFF 'S' 'M' 'B'
    uint8_t  command;           // Command code
    uint32_t status;            // NT status
    uint8_t  flags;             // Flags
    uint16_t flags2;            // Flags2
    uint16_t pid_high;          // High part of PID
    uint8_t  security[8];       // Security features
    uint16_t reserved;
    uint16_t tid;               // Tree ID
    uint16_t pid_low;           // Low part of PID
    uint16_t uid;               // User ID
    uint16_t mid;               // Multiplex ID
} __attribute__((packed)) smb1_header_t;

// ============================================================================
// SMB2 Negotiate Structures
// ============================================================================

// Negotiate Request
typedef struct {
    uint16_t structure_size;    // 36
    uint16_t dialect_count;
    uint16_t security_mode;     // SMB2_NEGOTIATE_SIGNING_*
    uint16_t reserved;
    uint32_t capabilities;      // SMB2_GLOBAL_CAP_*
    uint8_t  client_guid[16];
    uint32_t negotiate_context_offset;
    uint16_t negotiate_context_count;
    uint16_t reserved2;
    // Followed by dialect array (uint16_t[])
} __attribute__((packed)) smb2_negotiate_req_t;

// Negotiate Response
typedef struct {
    uint16_t structure_size;    // 65
    uint16_t security_mode;
    uint16_t dialect_revision;
    uint16_t negotiate_context_count;
    uint8_t  server_guid[16];
    uint32_t capabilities;
    uint32_t max_transact_size;
    uint32_t max_read_size;
    uint32_t max_write_size;
    uint64_t system_time;
    uint64_t server_start_time;
    uint16_t security_buffer_offset;
    uint16_t security_buffer_length;
    uint32_t negotiate_context_offset;
    // Followed by security buffer (SPNEGO)
} __attribute__((packed)) smb2_negotiate_resp_t;

// Security Mode Flags
#define SMB2_NEGOTIATE_SIGNING_ENABLED  0x0001
#define SMB2_NEGOTIATE_SIGNING_REQUIRED 0x0002

// Global Capabilities
#define SMB2_GLOBAL_CAP_DFS             0x00000001
#define SMB2_GLOBAL_CAP_LEASING         0x00000002
#define SMB2_GLOBAL_CAP_LARGE_MTU       0x00000004
#define SMB2_GLOBAL_CAP_MULTI_CHANNEL   0x00000008
#define SMB2_GLOBAL_CAP_PERSISTENT_HANDLES 0x00000010
#define SMB2_GLOBAL_CAP_DIRECTORY_LEASING  0x00000020
#define SMB2_GLOBAL_CAP_ENCRYPTION      0x00000040

// ============================================================================
// SMB2 Session Setup Structures
// ============================================================================

// Session Setup Request
typedef struct {
    uint16_t structure_size;    // 25
    uint8_t  flags;             // SMB2_SESSION_FLAG_*
    uint8_t  security_mode;
    uint32_t capabilities;
    uint32_t channel;
    uint16_t security_buffer_offset;
    uint16_t security_buffer_length;
    uint64_t previous_session_id;
    // Followed by security buffer (NTLM/SPNEGO)
} __attribute__((packed)) smb2_session_setup_req_t;

// Session Setup Response
typedef struct {
    uint16_t structure_size;    // 9
    uint16_t session_flags;
    uint16_t security_buffer_offset;
    uint16_t security_buffer_length;
    // Followed by security buffer
} __attribute__((packed)) smb2_session_setup_resp_t;

// Session Flags
#define SMB2_SESSION_FLAG_BINDING       0x01
#define SMB2_SESSION_FLAG_IS_GUEST      0x0001
#define SMB2_SESSION_FLAG_IS_NULL       0x0002
#define SMB2_SESSION_FLAG_ENCRYPT_DATA  0x0004

// ============================================================================
// NTLM Authentication Structures
// ============================================================================

#define NTLMSSP_SIGNATURE "NTLMSSP\0"

// NTLM Message Types
#define NTLM_NEGOTIATE      1
#define NTLM_CHALLENGE      2
#define NTLM_AUTHENTICATE   3

// NTLM Flags
#define NTLMSSP_NEGOTIATE_UNICODE           0x00000001
#define NTLMSSP_NEGOTIATE_OEM               0x00000002
#define NTLMSSP_REQUEST_TARGET              0x00000004
#define NTLMSSP_NEGOTIATE_SIGN              0x00000010
#define NTLMSSP_NEGOTIATE_SEAL              0x00000020
#define NTLMSSP_NEGOTIATE_DATAGRAM          0x00000040
#define NTLMSSP_NEGOTIATE_LM_KEY            0x00000080
#define NTLMSSP_NEGOTIATE_NTLM              0x00000200
#define NTLMSSP_NEGOTIATE_OEM_DOMAIN        0x00001000
#define NTLMSSP_NEGOTIATE_OEM_WORKSTATION   0x00002000
#define NTLMSSP_NEGOTIATE_ALWAYS_SIGN       0x00008000
#define NTLMSSP_TARGET_TYPE_DOMAIN          0x00010000
#define NTLMSSP_TARGET_TYPE_SERVER          0x00020000
#define NTLMSSP_NEGOTIATE_EXTENDED_SESSIONSECURITY 0x00080000
#define NTLMSSP_NEGOTIATE_IDENTIFY          0x00100000
#define NTLMSSP_NEGOTIATE_TARGET_INFO       0x00800000
#define NTLMSSP_NEGOTIATE_VERSION           0x02000000
#define NTLMSSP_NEGOTIATE_128               0x20000000
#define NTLMSSP_NEGOTIATE_KEY_EXCH          0x40000000
#define NTLMSSP_NEGOTIATE_56                0x80000000

// NTLM Negotiate Message
typedef struct {
    char     signature[8];      // "NTLMSSP\0"
    uint32_t message_type;      // 1
    uint32_t negotiate_flags;
    uint16_t domain_len;
    uint16_t domain_max_len;
    uint32_t domain_offset;
    uint16_t workstation_len;
    uint16_t workstation_max_len;
    uint32_t workstation_offset;
    // Optional version and payload
} __attribute__((packed)) ntlm_negotiate_t;

// NTLM Challenge Message
typedef struct {
    char     signature[8];      // "NTLMSSP\0"
    uint32_t message_type;      // 2
    uint16_t target_name_len;
    uint16_t target_name_max_len;
    uint32_t target_name_offset;
    uint32_t negotiate_flags;
    uint8_t  server_challenge[8];
    uint8_t  reserved[8];
    uint16_t target_info_len;
    uint16_t target_info_max_len;
    uint32_t target_info_offset;
    // Optional version and target info
} __attribute__((packed)) ntlm_challenge_t;

// NTLM Authenticate Message
typedef struct {
    char     signature[8];      // "NTLMSSP\0"
    uint32_t message_type;      // 3
    uint16_t lm_response_len;
    uint16_t lm_response_max_len;
    uint32_t lm_response_offset;
    uint16_t nt_response_len;
    uint16_t nt_response_max_len;
    uint32_t nt_response_offset;
    uint16_t domain_len;
    uint16_t domain_max_len;
    uint32_t domain_offset;
    uint16_t user_len;
    uint16_t user_max_len;
    uint32_t user_offset;
    uint16_t workstation_len;
    uint16_t workstation_max_len;
    uint32_t workstation_offset;
    uint16_t encrypted_random_len;
    uint16_t encrypted_random_max_len;
    uint32_t encrypted_random_offset;
    uint32_t negotiate_flags;
    // Followed by payload data
} __attribute__((packed)) ntlm_authenticate_t;

// ============================================================================
// SMB2 Tree Connect Structures
// ============================================================================

// Tree Connect Request
typedef struct {
    uint16_t structure_size;    // 9
    uint16_t flags;             // SMB2_TREE_CONNECT_FLAG_*
    uint16_t path_offset;
    uint16_t path_length;
    // Followed by path string (Unicode)
} __attribute__((packed)) smb2_tree_connect_req_t;

// Tree Connect Response
typedef struct {
    uint16_t structure_size;    // 16
    uint8_t  share_type;        // SMB2_SHARE_TYPE_*
    uint8_t  reserved;
    uint32_t share_flags;
    uint32_t capabilities;
    uint32_t maximal_access;
} __attribute__((packed)) smb2_tree_connect_resp_t;

// Share Types
#define SMB2_SHARE_TYPE_DISK    0x01
#define SMB2_SHARE_TYPE_PIPE    0x02
#define SMB2_SHARE_TYPE_PRINT   0x03

// Share Flags
#define SMB2_SHAREFLAG_MANUAL_CACHING               0x00000000
#define SMB2_SHAREFLAG_AUTO_CACHING                 0x00000010
#define SMB2_SHAREFLAG_VDO_CACHING                  0x00000020
#define SMB2_SHAREFLAG_NO_CACHING                   0x00000030
#define SMB2_SHAREFLAG_DFS                          0x00000001
#define SMB2_SHAREFLAG_DFS_ROOT                     0x00000002
#define SMB2_SHAREFLAG_RESTRICT_EXCLUSIVE_OPENS     0x00000100
#define SMB2_SHAREFLAG_FORCE_SHARED_DELETE          0x00000200
#define SMB2_SHAREFLAG_ALLOW_NAMESPACE_CACHING      0x00000400
#define SMB2_SHAREFLAG_ACCESS_BASED_DIRECTORY_ENUM  0x00000800
#define SMB2_SHAREFLAG_FORCE_LEVELII_OPLOCK         0x00001000
#define SMB2_SHAREFLAG_ENABLE_HASH_V1               0x00002000
#define SMB2_SHAREFLAG_ENABLE_HASH_V2               0x00004000
#define SMB2_SHAREFLAG_ENCRYPT_DATA                 0x00008000

// ============================================================================
// SMB2 Create (Open) Structures
// ============================================================================

// Create Request
typedef struct {
    uint16_t structure_size;    // 57
    uint8_t  security_flags;    // Reserved
    uint8_t  requested_oplock_level;
    uint32_t impersonation_level;
    uint64_t smb_create_flags;
    uint64_t reserved;
    uint32_t desired_access;
    uint32_t file_attributes;
    uint32_t share_access;
    uint32_t create_disposition;
    uint32_t create_options;
    uint16_t name_offset;
    uint16_t name_length;
    uint32_t create_contexts_offset;
    uint32_t create_contexts_length;
    // Followed by filename (Unicode) and create contexts
} __attribute__((packed)) smb2_create_req_t;

// Create Response
typedef struct {
    uint16_t structure_size;    // 89
    uint8_t  oplock_level;
    uint8_t  flags;
    uint32_t create_action;
    uint64_t creation_time;
    uint64_t last_access_time;
    uint64_t last_write_time;
    uint64_t change_time;
    uint64_t allocation_size;
    uint64_t end_of_file;
    uint32_t file_attributes;
    uint32_t reserved2;
    uint64_t file_id_persistent;    // File handle part 1
    uint64_t file_id_volatile;      // File handle part 2
    uint32_t create_contexts_offset;
    uint32_t create_contexts_length;
} __attribute__((packed)) smb2_create_resp_t;

// Oplock Levels
#define SMB2_OPLOCK_LEVEL_NONE      0x00
#define SMB2_OPLOCK_LEVEL_II        0x01
#define SMB2_OPLOCK_LEVEL_EXCLUSIVE 0x08
#define SMB2_OPLOCK_LEVEL_BATCH     0x09
#define SMB2_OPLOCK_LEVEL_LEASE     0xFF

// Impersonation Levels
#define SMB2_IMPERSONATION_ANONYMOUS        0x00000000
#define SMB2_IMPERSONATION_IDENTIFICATION   0x00000001
#define SMB2_IMPERSONATION_IMPERSONATION    0x00000002
#define SMB2_IMPERSONATION_DELEGATE         0x00000003

// Desired Access Flags
#define FILE_READ_DATA          0x00000001
#define FILE_WRITE_DATA         0x00000002
#define FILE_APPEND_DATA        0x00000004
#define FILE_READ_EA            0x00000008
#define FILE_WRITE_EA           0x00000010
#define FILE_EXECUTE            0x00000020
#define FILE_DELETE_CHILD       0x00000040
#define FILE_READ_ATTRIBUTES    0x00000080
#define FILE_WRITE_ATTRIBUTES   0x00000100
#define DELETE                  0x00010000
#define READ_CONTROL            0x00020000
#define WRITE_DAC               0x00040000
#define WRITE_OWNER             0x00080000
#define SYNCHRONIZE             0x00100000
#define ACCESS_SYSTEM_SECURITY  0x01000000
#define MAXIMUM_ALLOWED         0x02000000
#define GENERIC_ALL             0x10000000
#define GENERIC_EXECUTE         0x20000000
#define GENERIC_WRITE           0x40000000
#define GENERIC_READ            0x80000000

// File Attributes
#define FILE_ATTRIBUTE_READONLY             0x00000001
#define FILE_ATTRIBUTE_HIDDEN               0x00000002
#define FILE_ATTRIBUTE_SYSTEM               0x00000004
#define FILE_ATTRIBUTE_DIRECTORY            0x00000010
#define FILE_ATTRIBUTE_ARCHIVE              0x00000020
#define FILE_ATTRIBUTE_NORMAL               0x00000080
#define FILE_ATTRIBUTE_TEMPORARY            0x00000100
#define FILE_ATTRIBUTE_SPARSE_FILE          0x00000200
#define FILE_ATTRIBUTE_REPARSE_POINT        0x00000400
#define FILE_ATTRIBUTE_COMPRESSED           0x00000800
#define FILE_ATTRIBUTE_OFFLINE              0x00001000
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED  0x00002000
#define FILE_ATTRIBUTE_ENCRYPTED            0x00004000

// Share Access Flags
#define FILE_SHARE_READ     0x00000001
#define FILE_SHARE_WRITE    0x00000002
#define FILE_SHARE_DELETE   0x00000004

// Create Disposition
#define FILE_SUPERSEDE      0x00000000
#define FILE_OPEN           0x00000001
#define FILE_CREATE         0x00000002
#define FILE_OPEN_IF        0x00000003
#define FILE_OVERWRITE      0x00000004
#define FILE_OVERWRITE_IF   0x00000005

// Create Options
#define FILE_DIRECTORY_FILE             0x00000001
#define FILE_WRITE_THROUGH              0x00000002
#define FILE_SEQUENTIAL_ONLY            0x00000004
#define FILE_NO_INTERMEDIATE_BUFFERING  0x00000008
#define FILE_SYNCHRONOUS_IO_ALERT       0x00000010
#define FILE_SYNCHRONOUS_IO_NONALERT    0x00000020
#define FILE_NON_DIRECTORY_FILE         0x00000040
#define FILE_COMPLETE_IF_OPLOCKED       0x00000100
#define FILE_NO_EA_KNOWLEDGE            0x00000200
#define FILE_RANDOM_ACCESS              0x00000800
#define FILE_DELETE_ON_CLOSE            0x00001000
#define FILE_OPEN_BY_FILE_ID            0x00002000
#define FILE_OPEN_FOR_BACKUP_INTENT     0x00004000
#define FILE_NO_COMPRESSION             0x00008000

// Create Action
#define FILE_SUPERSEDED     0x00000000
#define FILE_OPENED         0x00000001
#define FILE_CREATED        0x00000002
#define FILE_OVERWRITTEN    0x00000003

// ============================================================================
// SMB2 Close Structure
// ============================================================================

// Close Request
typedef struct {
    uint16_t structure_size;    // 24
    uint16_t flags;             // SMB2_CLOSE_FLAG_*
    uint32_t reserved;
    uint64_t file_id_persistent;
    uint64_t file_id_volatile;
} __attribute__((packed)) smb2_close_req_t;

// Close Response
typedef struct {
    uint16_t structure_size;    // 60
    uint16_t flags;
    uint32_t reserved;
    uint64_t creation_time;
    uint64_t last_access_time;
    uint64_t last_write_time;
    uint64_t change_time;
    uint64_t allocation_size;
    uint64_t end_of_file;
    uint32_t file_attributes;
} __attribute__((packed)) smb2_close_resp_t;

#define SMB2_CLOSE_FLAG_POSTQUERY_ATTRIB    0x0001

// ============================================================================
// SMB2 Read Structures
// ============================================================================

// Read Request
typedef struct {
    uint16_t structure_size;    // 49
    uint8_t  padding;
    uint8_t  flags;
    uint32_t length;            // Bytes to read
    uint64_t offset;            // File offset
    uint64_t file_id_persistent;
    uint64_t file_id_volatile;
    uint32_t minimum_count;
    uint32_t channel;
    uint32_t remaining_bytes;
    uint16_t read_channel_info_offset;
    uint16_t read_channel_info_length;
    uint8_t  buffer[1];         // Variable length
} __attribute__((packed)) smb2_read_req_t;

// Read Response
typedef struct {
    uint16_t structure_size;    // 17
    uint8_t  data_offset;
    uint8_t  reserved;
    uint32_t data_length;
    uint32_t data_remaining;
    uint32_t reserved2;
    // Followed by data
} __attribute__((packed)) smb2_read_resp_t;

// ============================================================================
// SMB2 Write Structures
// ============================================================================

// Write Request
typedef struct {
    uint16_t structure_size;    // 49
    uint16_t data_offset;
    uint32_t length;            // Bytes to write
    uint64_t offset;            // File offset
    uint64_t file_id_persistent;
    uint64_t file_id_volatile;
    uint32_t channel;
    uint32_t remaining_bytes;
    uint16_t write_channel_info_offset;
    uint16_t write_channel_info_length;
    uint32_t flags;
    // Followed by data
} __attribute__((packed)) smb2_write_req_t;

// Write Response
typedef struct {
    uint16_t structure_size;    // 17
    uint16_t reserved;
    uint32_t count;             // Bytes written
    uint32_t remaining;
    uint16_t write_channel_info_offset;
    uint16_t write_channel_info_length;
} __attribute__((packed)) smb2_write_resp_t;

// Write Flags
#define SMB2_WRITEFLAG_WRITE_THROUGH    0x00000001
#define SMB2_WRITEFLAG_WRITE_UNBUFFERED 0x00000002

// ============================================================================
// SMB2 Query Directory Structures
// ============================================================================

// Query Directory Request
typedef struct {
    uint16_t structure_size;    // 33
    uint8_t  file_information_class;
    uint8_t  flags;
    uint32_t file_index;
    uint64_t file_id_persistent;
    uint64_t file_id_volatile;
    uint16_t file_name_offset;
    uint16_t file_name_length;
    uint32_t output_buffer_length;
    // Followed by search pattern (Unicode)
} __attribute__((packed)) smb2_query_directory_req_t;

// Query Directory Response
typedef struct {
    uint16_t structure_size;    // 9
    uint16_t output_buffer_offset;
    uint32_t output_buffer_length;
    // Followed by file information entries
} __attribute__((packed)) smb2_query_directory_resp_t;

// File Information Classes
#define FileDirectoryInformation        1
#define FileFullDirectoryInformation    2
#define FileBothDirectoryInformation    3
#define FileNamesInformation            12
#define FileIdBothDirectoryInformation  37
#define FileIdFullDirectoryInformation  38

// Query Directory Flags
#define SMB2_RESTART_SCANS      0x01
#define SMB2_RETURN_SINGLE_ENTRY 0x02
#define SMB2_INDEX_SPECIFIED    0x04
#define SMB2_REOPEN             0x10

// File Both Directory Information (most common)
typedef struct {
    uint32_t next_entry_offset;
    uint32_t file_index;
    uint64_t creation_time;
    uint64_t last_access_time;
    uint64_t last_write_time;
    uint64_t change_time;
    uint64_t end_of_file;
    uint64_t allocation_size;
    uint32_t file_attributes;
    uint32_t file_name_length;
    uint32_t ea_size;
    uint8_t  short_name_length;
    uint8_t  reserved;
    uint16_t short_name[12];    // 8.3 name in Unicode
    // Followed by file_name (Unicode)
} __attribute__((packed)) file_both_dir_info_t;

// ============================================================================
// SMB2 Query Info Structures
// ============================================================================

// Query Info Request
typedef struct {
    uint16_t structure_size;    // 41
    uint8_t  info_type;
    uint8_t  file_info_class;
    uint32_t output_buffer_length;
    uint16_t input_buffer_offset;
    uint16_t reserved;
    uint32_t input_buffer_length;
    uint32_t additional_information;
    uint32_t flags;
    uint64_t file_id_persistent;
    uint64_t file_id_volatile;
    // Followed by input buffer
} __attribute__((packed)) smb2_query_info_req_t;

// Query Info Response
typedef struct {
    uint16_t structure_size;    // 9
    uint16_t output_buffer_offset;
    uint32_t output_buffer_length;
    // Followed by output buffer
} __attribute__((packed)) smb2_query_info_resp_t;

// Info Types
#define SMB2_0_INFO_FILE        0x01
#define SMB2_0_INFO_FILESYSTEM  0x02
#define SMB2_0_INFO_SECURITY    0x03
#define SMB2_0_INFO_QUOTA       0x04

// File Info Classes for Query Info
#define FileBasicInformation    4
#define FileStandardInformation 5
#define FileAllInformation      18

// FileBasicInformation structure
typedef struct {
    uint64_t creation_time;
    uint64_t last_access_time;
    uint64_t last_write_time;
    uint64_t change_time;
    uint32_t file_attributes;
    uint32_t reserved;
} __attribute__((packed)) file_basic_info_t;

// FileStandardInformation structure
typedef struct {
    uint64_t allocation_size;
    uint64_t end_of_file;
    uint32_t number_of_links;
    uint8_t  delete_pending;
    uint8_t  directory;
    uint16_t reserved;
} __attribute__((packed)) file_standard_info_t;

// ============================================================================
// SMB Client State
// ============================================================================

// Maximum concurrent SMB connections/mounts
#define SMB_MAX_CONNECTIONS     8
#define SMB_MAX_OPEN_FILES      64
#define SMB_MAX_PATH            260
#define SMB_MAX_NAME            256

// ============================================================================
// #749: the SMB2 message sequence and credit window
// ============================================================================
//
// MS-SMB2 2.2.1.2: MessageId is scoped to the SMB2 TRANSPORT CONNECTION. The
// server keeps a window [seq_low, seq_low + granted) and TERMINATES the
// connection when a request falls outside it. The test Samba server's own log
// (source3/smbd/smb2_server.c) recorded our client doing both halves of it:
//
//   bad message_id 0 (sequence id 0) (granted = 1, low = 2, range = 1)
//   bad message_id 3 (sequence id 3) (granted = 1, low = 1, range = 1)
//
// and `granted = 1` is not a quirk of that run: measured on the wire on
// 2026-10-03, the server grants exactly ONE credit for NEGOTIATE and one for
// the first SESSION_SETUP before it grants 31. During the handshake the window
// is one message wide, so a single slipped id is immediately fatal. The client
// then read whatever came back on the dying connection as an answer about the
// password, and reported STATUS_LOGON_FAILURE (0xC000006D).
//
// The logic lives in Rust (rustkern/smbseq.rs) per the 2026-07-16 rule: it is a
// protocol state machine and an admission policy, and it is NEW code with no C
// twin to strangle. This struct mirrors `SmbSeq` there field for field; the
// size is locked below so a change on either side fails the build instead of
// reading the wrong offsets.
//
// A ZEROED smb_seq_t IS THE CORRECT "unusable" STATE (epoch == 0). That is
// deliberate: `smb_init()` used to memset the whole connection table from three
// runtime mount paths, and failing closed is what turns that from a silent
// counter restart on a live connection into a refusal with a reason.
typedef struct {
    uint64_t epoch;          // 0 = never bound to a connection = unusable
    uint64_t next_id;        // MessageId the next request will carry
    uint64_t last_sent_id;   // MessageId of the most recent request issued
    int32_t  bound_socket;   // the ONE socket this sequence may number for
    uint16_t granted;        // credits the server last said we hold (min 1)
    uint16_t outstanding;    // issued and not yet answered
    uint16_t fault;          // SMB_SEQ_FAULT_*; non-zero = connection is bad
    uint16_t _pad;
} smb_seq_t;

_Static_assert(sizeof(smb_seq_t) == 40,
               "smb_seq_t must match SmbSeq in rustkern/smbseq.rs");
_Static_assert(_Alignof(smb_seq_t) == 8,
               "smb_seq_t alignment must match SmbSeq in rustkern/smbseq.rs");

#define SMB_SEQ_FAULT_NONE      0
#define SMB_SEQ_FAULT_UNBOUND   1
#define SMB_SEQ_FAULT_SOCKET    2
#define SMB_SEQ_FAULT_WINDOW    3
#define SMB_SEQ_FAULT_REPLY_ID  4
// A response failed its MESSAGE SIGNATURE (rustkern/smbsign.rs). Its own code,
// not a reuse of REPLY_ID: the reason text is the point of this enum, and
// reporting a tampered response as a sequencing desync is the misdiagnosis
// #749 exists to have stopped happening.
#define SMB_SEQ_FAULT_SIGNATURE 5
// A response FAILED ITS DECRYPTION, or arrived unencrypted on a session
// that is encrypted (rustkern/smbcrypt.rs). Its own code for the same
// reason SIGNATURE has one.
#define SMB_SEQ_FAULT_ENCRYPTION 6

// rustkern/smbseq.rs. smbseq_bind() is the ONLY thing that makes a sequence
// able to issue an id, and it always restarts at 0 with a 1-credit window, so
// "the connection was re-established" and "the sequence was reset" are one
// event that cannot drift apart.
int      smbseq_bind(smb_seq_t *s, int socket);
void     smbseq_unbind(smb_seq_t *s);
int64_t  smbseq_next(smb_seq_t *s, int socket);     // >=0 id, else -(fault)
void     smbseq_note_grant(smb_seq_t *s, uint16_t granted);
void     smbseq_condemn(smb_seq_t *s, uint16_t code);
int      smbseq_check_reply(smb_seq_t *s, uint64_t reply_id,
                            uint32_t resp_flags, uint32_t resp_status);
                            // 0 = final reply, 1 = unsolicited notification,
                            // 2 = interim (keep receiving), <0 = -(fault)
uint16_t smbseq_fault(const smb_seq_t *s);
int      smbseq_is_healthy(const smb_seq_t *s);
uint64_t smbseq_last_sent(const smb_seq_t *s);
const char *smbseq_fault_text(uint16_t code);
uint32_t smbseq_selftest_rs(void);

// rustkern/smbident.rs - #749 pass 2: the SESSION IDENTITY policy.
//
// "Did the SESSION_SETUP succeed" and "am I who I asked to be" are different
// questions, and this client only ever asked the first. A server may answer an
// NTLM authentication for a named user with STATUS_SUCCESS *and*
// SMB2_SESSION_FLAG_IS_GUEST, which means it declined the identity and
// substituted an anonymous one; Samba's "map to guest = Bad Password" does
// exactly that for any wrong password. SMB2_SESSION_FLAG_IS_NULL was not read
// by any .c file in the kernel at all.
//
// DELIBERATELY NO SMB_IDENT_* CONSTANTS HERE. smbident_check() returns 0 to
// allow and an opaque non-zero reason code to refuse, and the only thing C
// does with a non-zero code is hand it back to smbident_verdict_text(). A
// constant mirrored on both sides is a thing that can drift, and no
// _Static_assert can catch a mirrored constant holding the WRONG VALUE.
uint16_t smbident_check(const char *user, uint16_t session_flags);
int      smbident_name_is_anonymous(const char *user);
int      smbident_same(const char *dom_a, const char *user_a,
                       const char *dom_b, const char *user_b);
const char *smbident_verdict_text(uint16_t code);
uint32_t smbident_selftest_rs(void);

// ============================================================================
// SMB2 MESSAGE SIGNING
//
// The implementation is Rust (rustkern/smbsign.rs); read that file's header
// before changing anything here. It explains, with measurements, why the
// dialect this client actually negotiates (3.0.2) signs with AES-128-CMAC and
// NOT with the HMAC-SHA256 that "SMB 2.x through 3.0.2" is usually said to
// use, and why the shipping default is to REQUIRE signing.
//
// NO CRYPTO IS IMPLEMENTED THERE EITHER. It calls the hmac_sha256() and
// aes_cmac() that crypto/crypto.h already provides.
//
// WHAT THIS REPLACES: `signing_required` above was written once, out of the
// server's NEGOTIATE response, and read by nothing in any file. Nothing
// authenticated the file server to MayteraOS. It is still recorded, and is now
// actually consulted, through smbsign_requires().
// ============================================================================

// Mirrors `SmbSign` in rustkern/smbsign.rs field for field. The size is locked
// below so a change on either side fails the build instead of reading the
// wrong offsets. A ZEROED smb_sign_t is the correct "no key, sign nothing,
// require nothing" state, which is what NEGOTIATE needs.
typedef struct {
    uint8_t  key[16];          // the DERIVED signing key, never the session key
    uint8_t  algo;             // 0 none, 1 HMAC-SHA256, 2 AES-128-CMAC
    uint8_t  armed;            // 1 once a key exists for this session
    uint8_t  policy;           // SMB_SIGN_POLICY_*, captured at NEGOTIATE
    uint8_t  server_requires;  // server said SIGNING_REQUIRED in NEGOTIATE
    uint32_t signed_out;       // evidence counters, so a claim about signing
    uint32_t verified_in;      // can be checked against a number rather than
    uint32_t rejected_in;      // against a log line that may not be reached
    // Responses ACCEPTED UNSIGNED under signing=enabled. Counted separately
    // from verified_in deliberately: lumping them together would let a mount
    // that was never protected report the same number as one that was.
    uint32_t unsigned_in;
} smb_sign_t;

_Static_assert(sizeof(smb_sign_t) == 36,
               "smb_sign_t must match SmbSign in rustkern/smbsign.rs");
_Static_assert(_Alignof(smb_sign_t) == 4,
               "smb_sign_t alignment must match SmbSign in rustkern/smbsign.rs");

// The ONLY two constants mirrored into C, because C is what sets the policy.
// smbsign_policy_abi_check() is handed both at boot and refuses to agree if
// they ever drift from the Rust side's copies.
#define SMB_SIGN_POLICY_ENABLED   0u   // sign when offered; always VERIFY
#define SMB_SIGN_POLICY_REQUIRED  1u   // additionally refuse an unsignable mount

// Where a user overrules the default. See rustkern/smbsign.rs.
#define SMB_SIGN_CFG_PATH "/CONFIG/SMB.CFG"

void     smbsign_policy_set(uint32_t policy);
uint32_t smbsign_policy(void);
int32_t  smbsign_policy_parse(const uint8_t *buf, int32_t len);
int32_t  smbsign_policy_abi_check(uint32_t c_enabled, uint32_t c_required);

void     smbsign_reset(smb_sign_t *st);
uint16_t smbsign_client_security_mode(void);
uint16_t smbsign_note_negotiate(smb_sign_t *st, uint16_t dialect,
                                uint16_t server_security_mode);
uint16_t smbsign_begin(smb_sign_t *st, uint16_t dialect,
                       const uint8_t *session_key);
int      smbsign_is_armed(const smb_sign_t *st);
int      smbsign_requires(const smb_sign_t *st);
uint16_t smbsign_algo(const smb_sign_t *st);
const char *smbsign_algo_text(uint16_t algo);
void     smbsign_counts(const smb_sign_t *st, uint32_t *out_signed,
                        uint32_t *out_verified, uint32_t *out_rejected,
                        uint32_t *out_unsigned);

// THE TWO CHOKEPOINTS. Both return 0 to proceed and a non-zero reason code to
// refuse, and C must NEVER compare that code against a value: hand it to
// smbsign_reason_text() and refuse. Every judgement about which refusals are
// survivable (an unsigned async interim response is; a missing signature on a
// real answer is not) lives in rustkern/smbsign.rs, so there is one copy of it.
//
// The one exception, and it is spelled out at the call site: smbsign_sign()
// returns 1 to mean "this connection has no key yet", which is the correct and
// expected answer for NEGOTIATE and SESSION_SETUP rather than a failure.
uint16_t smbsign_sign(smb_sign_t *st, uint8_t *pdu, size_t len);
uint16_t smbsign_check_response(smb_sign_t *st, uint8_t *pdu, size_t len);

uint64_t smbsign_pdu_session_id(const uint8_t *pdu, size_t len);
const char *smbsign_reason_text(uint16_t code);
// Returns 0 on pass, else the number of the FIRST FAILING CASE, and writes
// the number of cases that actually RAN to *ran_out. It stops at the first
// failure, so the count is the point: a stale expectation on case 25 once
// hid 55 unexecuted cases behind a line that said only "first failing case
// 25". smbsign_selftest_cases_rs() is how many cases it declares.
uint32_t smbsign_selftest_rs(uint32_t *ran_out);
uint32_t smbsign_selftest_cases_rs(void);

// ============================================================================
// SMB3 ENCRYPTION
//
// The implementation is Rust (rustkern/smbcrypt.rs); read that file's header
// before changing anything here. It explains, with measurements, why the
// dialect this client negotiates (3.0.2) encrypts with AES-128-CCM and NOT
// with the AES-128-GCM that "SMB3 encryption" is usually said to mean and
// that this tree already had, why the shipping default is to REQUIRE
// encryption, and what that default refuses.
//
// NO CIPHER IS IMPLEMENTED THERE EITHER. It calls aes_ccm_encrypt() /
// aes_ccm_decrypt() (crypto/crypto.h, rustkern/aesccm.rs, the one new
// primitive this work needed) and derives its keys through the same
// SP800-108 KDF the signing module uses, shared rather than copied.
//
// WHAT THIS REPLACES: nothing. Before this, every byte of every file read or
// written over SMB crossed the network in plain text. MEASURED on golden
// build 2576 with tcpdump: 89 SMB2 messages, all signed, none encrypted, and
// the test file's contents readable three times over in the capture.
// ============================================================================

// Mirrors `SmbCrypt` in rustkern/smbcrypt.rs field for field. The size is
// locked below so a change on either side fails the build instead of reading
// the wrong offsets. A ZEROED smb_crypt_t is the correct "no keys, encrypt
// nothing, require nothing" state, which is what NEGOTIATE needs.
typedef struct {
    uint8_t  enc_key[16];      // client -> server, KDF(..., "ServerIn \0")
    uint8_t  dec_key[16];      // server -> client, KDF(..., "ServerOut\0")
    uint64_t nonce_ctr;        // monotonic; a repeat would break the cipher
    uint64_t session_id;       // the session these keys belong to
    uint32_t sealed_out;       // evidence counters, so a claim about this
    uint32_t unsealed_in;      // mount can be checked against a number
    uint32_t rejected_in;
    uint32_t clear_out;        // post-session messages sent IN THE CLEAR
    uint32_t clear_in;         // post-session messages received IN THE CLEAR
    uint8_t  armed;            // 1 once both directional keys exist
    uint8_t  active;           // 1 once every post-session message is encrypted
    uint8_t  policy;           // SMB_CRYPT_POLICY_*, captured at NEGOTIATE
    uint8_t  server_capable;   // server advertised CAP_ENCRYPTION
    uint8_t  server_requires;  // SessionFlags 0x0004 or ShareFlags 0x8000
    uint8_t  algo;             // 0 none, 1 AES-128-CCM
    uint8_t  pad[2];
} smb_crypt_t;

_Static_assert(sizeof(smb_crypt_t) == 80,
               "smb_crypt_t must match SmbCrypt in rustkern/smbcrypt.rs");
_Static_assert(_Alignof(smb_crypt_t) == 8,
               "smb_crypt_t alignment must match SmbCrypt in rustkern/smbcrypt.rs");

// ============================================================================
// SMB 3.1.1 PRE-AUTHENTICATION INTEGRITY (rustkern/smbpreauth.rs)
// ============================================================================
//
// THE GAP: nothing protected the OPENING HANDSHAKE. The NEGOTIATE exchange
// decides the protocol version, whether messages get signed, whether they get
// encrypted and with which cipher, and it happens in the clear, before any key
// exists, authenticated by nothing. An attacker able to modify packets edited
// it and neither side could tell afterwards.
//
// Message signing (smb_sign_t above) proves who sent each message AFTER the
// handshake and by construction cannot protect the handshake itself. Encryption
// (smb_crypt_t above) was, until this, the only thing blocking a version
// downgrade, AND ONLY AS A SIDE EFFECT of being required; relaxing `encrypt=`
// silently removed downgrade protection, which the setting's name gives no hint
// of.
//
// WHAT NOW PROTECTS WHAT, because the two downgrade attacks need DIFFERENT
// defences and conflating them is how encryption ended up holding up version
// security:
//
//   VERSION DOWNGRADE (0x0311 deleted from our dialect list): the server then
//   legitimately picks 3.0.2, where no pre-auth hash exists on either side, so
//   the hash CANNOT detect it. The defence is the client-side DIALECT FLOOR:
//   we offered 3.1.1, so we require 3.1.1. Default SMB_PREAUTH_FLOOR_311.
//
//   FEATURE DOWNGRADE WITHIN 3.1.1 (a capability bit cleared, a cipher choice
//   swapped, a salt bit flipped, the NTLM exchange altered): every such byte is
//   folded into the running SHA-512, so the two sides' hashes diverge, every
//   derived key diverges, and the signature on the final SESSION_SETUP response
//   fails to verify. This works whatever the encryption policy is set to.
//
// So `encrypt=enabled` or even `encrypt=off` no longer costs you handshake
// integrity. Encryption is responsible for confidentiality, which is what its
// name says.
//
// ONE DEPENDENCY IS DELIBERATE: the hash is only ever CHECKED by a signature,
// so at 3.1.1 the final SESSION_SETUP response MUST be signed and MUST verify
// whatever the signing policy says. An unsigned final response under
// `signing=enabled` would leave the hash computed and never compared, which is
// the shape of a control that looks present and does nothing. net/smb.c refuses
// that case by name.

// Mirrors `SmbPreauth` in rustkern/smbpreauth.rs field for field. The size is
// locked below so a change on either side fails the build instead of reading
// the wrong offsets. A ZEROED smb_preauth_t is the correct starting state AND
// the correct seeding: the running hash begins as sixty-four zero bytes, so
// "never folded" and "correctly seeded" are the same state and there is no
// third one to get wrong.
typedef struct {
    uint8_t  hash[64];         // the running SHA-512 pre-auth integrity value
    uint8_t  salt[32];         // the 32 random bytes we put in our own context
    uint32_t folded;           // how many handshake messages went in; want 5
    uint8_t  offered;          // 1 once 0x0311 actually went on the wire
    uint8_t  active;           // 1 once the server selected 0x0311
    uint8_t  frozen;           // 1 once keys were derived; later folds refused
    uint8_t  hash_alg_ok;      // 1 once the server's context said SHA-512
    uint16_t cipher;           // server-chosen cipher, 0xFFFF = no context
    uint16_t signalg;          // server-chosen signing alg, 0xFFFF = none
    // 1 once the final SESSION_SETUP response VERIFIED under a key derived
    // from this hash. THIS, not `active`, is the flag that means the handshake
    // was actually authenticated rather than merely hashed.
    uint8_t  verified;
    uint8_t  pad[3];
} smb_preauth_t;

_Static_assert(sizeof(smb_preauth_t) == 112,
               "smb_preauth_t must match SmbPreauth in rustkern/smbpreauth.rs");
_Static_assert(_Alignof(smb_preauth_t) == 4,
               "smb_preauth_t alignment must match SmbPreauth in rustkern/smbpreauth.rs");

// rustkern/smbpreauth.rs keeps a local scratch of this size for one
// sha512_ctx_t. Locking it here means a growth of the hash context fails the
// BUILD rather than overwriting the Rust side's stack at run time.
// The matching _Static_assert lives in net/smb.c, not here, because this
// header deliberately does not pull in crypto/crypto.h. It is still a BUILD
// failure, just one file along.
#define SMB_PREAUTH_SHA512_CTX_ROOM 256

// The TWO constants mirrored into C, because C is what sets the policy.
// smbpreauth_floor_abi_check() is handed both at boot and refuses to agree if
// they ever drift from the Rust side's copies.
#define SMB_PREAUTH_FLOOR_NONE 0u  // no dialect floor beyond the signing policy
#define SMB_PREAUTH_FLOOR_311  1u  // require SMB 3.1.1: authenticated handshake

// How many handshake messages belong in the hash: NEGOTIATE request and
// response, SESSION_SETUP request and response #1, and the FINAL SESSION_SETUP
// request. The final RESPONSE is deliberately NOT folded; it is the message
// whose signature the derived key has to verify.
#define SMB_PREAUTH_EXPECT_FOLDS 5u

void        smbpreauth_floor_set(uint32_t floor);
uint32_t    smbpreauth_floor(void);
const char *smbpreauth_floor_text(uint32_t floor);
int32_t     smbpreauth_floor_parse(const uint8_t *buf, int32_t len);
int32_t     smbpreauth_floor_abi_check(uint32_t c_none, uint32_t c_311);
void        smbpreauth_reset(smb_preauth_t *st);
int32_t     smbpreauth_is_active(const smb_preauth_t *st);
int32_t     smbpreauth_is_verified(const smb_preauth_t *st);
uint32_t    smbpreauth_folded(const smb_preauth_t *st);
uint16_t    smbpreauth_cipher(const smb_preauth_t *st);
uint16_t    smbpreauth_signalg(const smb_preauth_t *st);
int32_t     smbpreauth_hash_copy(const smb_preauth_t *st, uint8_t *out,
                                 size_t out_len);
const char *smbpreauth_reason_text(uint16_t code);
int32_t     smbpreauth_offer_311(void);
int32_t     smbpreauth_build_contexts(smb_preauth_t *st,
                                      const uint8_t *salt, size_t salt_len,
                                      size_t base_len,
                                      uint8_t *out, size_t out_cap,
                                      uint32_t *out_pad, uint16_t *out_count);
uint16_t    smbpreauth_fold(smb_preauth_t *st, const uint8_t *pdu, size_t len);
uint16_t    smbpreauth_floor_check(uint16_t dialect);
uint16_t    smbpreauth_note_negotiate(smb_preauth_t *st, uint16_t dialect,
                                      const uint8_t *pdu, size_t len);
uint16_t    smbpreauth_derive(smb_preauth_t *st, const uint8_t *session_key,
                              uint8_t *out_sign, uint8_t *out_enc,
                              uint8_t *out_dec, uint8_t *out_app);
uint16_t    smbpreauth_note_verified(smb_preauth_t *st);
uint32_t    smbpreauth_selftest_rs(void);

// The 3.1.1 entry points on the two existing modules. Both take keys that were
// ALREADY derived from the pre-auth hash, which is why neither takes a dialect:
// there is exactly one dialect whose keys look like this.
uint16_t    smbsign_begin311(smb_sign_t *st, const uint8_t *signing_key);
uint16_t    smbcrypt_begin311(smb_crypt_t *st, const uint8_t *enc_key,
                              const uint8_t *dec_key, uint64_t session_id);

// The THREE constants mirrored into C, because C is what sets the policy.
// smbcrypt_policy_abi_check() is handed all three at boot and refuses to agree
// if they ever drift from the Rust side's copies.
#define SMB_CRYPT_POLICY_OFF       0u  // never encrypt
#define SMB_CRYPT_POLICY_ENABLED   1u  // encrypt when the server can; allow clear
#define SMB_CRYPT_POLICY_REQUIRED  2u  // refuse a mount that will not encrypt

void        smbcrypt_policy_set(uint32_t policy);
uint32_t    smbcrypt_policy(void);
const char *smbcrypt_policy_text(uint32_t policy);
int32_t     smbcrypt_policy_parse(const uint8_t *buf, int32_t len);
int32_t     smbcrypt_policy_abi_check(uint32_t c_off, uint32_t c_enabled,
                                      uint32_t c_required);
uint32_t    smbcrypt_client_capabilities(void);

void     smbcrypt_reset(smb_crypt_t *st);
uint16_t smbcrypt_note_negotiate(smb_crypt_t *st, uint16_t dialect,
                                 uint32_t server_caps);
uint16_t smbcrypt_begin(smb_crypt_t *st, uint16_t dialect,
                        const uint8_t *session_key, uint64_t session_id);
uint16_t smbcrypt_note_session_flags(smb_crypt_t *st, uint16_t session_flags);
uint16_t smbcrypt_note_share_flags(smb_crypt_t *st, uint32_t share_flags);
int      smbcrypt_is_armed(const smb_crypt_t *st);
int      smbcrypt_is_active(const smb_crypt_t *st);
int      smbcrypt_server_capable(const smb_crypt_t *st);
int      smbcrypt_server_requires(const smb_crypt_t *st);
const char *smbcrypt_algo_text(const smb_crypt_t *st);
void     smbcrypt_counts(const smb_crypt_t *st, uint32_t *out_sealed,
                         uint32_t *out_unsealed, uint32_t *out_rejected,
                         uint32_t *out_clear_out, uint32_t *out_clear_in);

// THE THREE CHOKEPOINTS. As with signing, C must NEVER compare a returned
// reason code against a value: hand it to smbcrypt_reason_text() and refuse.
// Every judgement about what is survivable lives in rustkern/smbcrypt.rs so
// there is one copy of it.
//
// smbcrypt_should_seal() returns 1/0 rather than a reason, because "do not
// encrypt this one" is the correct answer for NEGOTIATE and SESSION_SETUP
// rather than a refusal. It COUNTS a post-session message that goes out in
// the clear, which is what makes "nothing left this mount unencrypted" a
// number instead of a hope.
int      smbcrypt_should_seal(smb_crypt_t *st, const uint8_t *pdu, size_t len);
uint16_t smbcrypt_seal(smb_crypt_t *st, uint8_t *pdu, size_t len,
                       uint8_t *hdr_out);
int      smbcrypt_pdu_is_transform(const uint8_t *pdu, size_t len);
uint16_t smbcrypt_unseal(smb_crypt_t *st, uint8_t *buf, size_t len,
                         uint32_t *out_len);
uint16_t smbcrypt_check_clear_response(smb_crypt_t *st, const uint8_t *pdu,
                                       size_t len);

uint64_t smbcrypt_pdu_session_id(const uint8_t *pdu, size_t len);
const char *smbcrypt_reason_text(uint16_t code);
uint32_t smbcrypt_selftest_rs(void);

// SMB connection state
typedef struct {
    bool active;
    int tcp_socket;             // TCP socket descriptor
    uint32_t server_ip;
    uint16_t server_port;

    // Negotiated parameters
    uint16_t dialect;           // Negotiated SMB version
    uint32_t max_transact_size;
    uint32_t max_read_size;
    uint32_t max_write_size;
    uint32_t capabilities;
    uint8_t  server_guid[16];
    bool signing_required;

    // Session state
    uint64_t session_id;
    bool session_established;
    uint16_t session_flags;

    // Tree connect state
    uint32_t tree_id;
    bool tree_connected;
    char share_path[SMB_MAX_PATH];
    uint8_t share_type;

    // Message tracking. #749: the MessageId and the credit window are per
    // TRANSPORT CONNECTION (MS-SMB2 2.2.1.2), so they live in a sequence that
    // is BOUND to the socket and is unusable until it is. There is no bare
    // `message_id` counter here any more, on purpose: a plain field can be
    // incremented by anyone and reset by no one, which is how a reconnect came
    // to carry the old count onto a connection whose server-side window had
    // restarted at 0.
    smb_seq_t seq;

    // Authentication
    char domain[64];
    char username[64];
    // #smbnt: the NT HASH, never the password. SMB auth derives nt_hash =
    // MD4(UTF-16LE(password)) once and every later step (HMAC-MD5, the NTLMv2
    // response) needs only those 16 bytes, so a connection has no reason to
    // keep the password and every reason not to: this struct lives for the
    // whole mount and landed in any crash dump or memory image.
    //
    // HONEST LIMIT, so nobody mistakes this for more than it is: the NT hash is
    // password-equivalent FOR SMB (pass-the-hash). This does not make a stolen
    // hash harmless. What it removes is PASSWORD REUSE blast radius, which is
    // the realistic harm, because a share password is very often also the
    // user's password somewhere that matters more.
    uint8_t nt_hash[16];

    // THE SESSION KEY IS DELIBERATELY NOT HERE ANY MORE.
    //
    // This struct used to carry `uint8_t session_key[16]`, which was DECLARED
    // and never written and never read, in any file in the tree. That was not
    // merely dead weight: it was the missing prerequisite for message signing,
    // because the NTLMv2 SessionBaseKey was thrown away at the point it was
    // computed, so there was no key with which a signature could have been
    // made or checked.
    //
    // It is now computed in smb2_session_setup(), consumed immediately by
    // smbsign_begin() to derive the per-dialect signing key, and zeroized in
    // the same function. Only the DERIVED key lives for the length of the
    // connection, inside `sign` below. That is one fewer long-lived
    // password-equivalent secret in a struct that survives into any crash
    // dump, which is the same reasoning recorded above for keeping the NT hash
    // instead of the password.
    smb_sign_t sign;

    // SMB3 encryption state, derived from the SAME session key in the same
    // window and, like the signing key, outliving it. See smb_crypt_t above.
    smb_crypt_t crypt;
    // SMB 3.1.1 pre-authentication integrity. Reset at the same point as
    // `sign` and `crypt`, i.e. when a TCP connection becomes usable, because
    // the running hash is a property of ONE connection's handshake and
    // carrying it across a reconnect would authenticate the wrong exchange.
    smb_preauth_t preauth;

    // Mount point info
    char mount_point[SMB_MAX_PATH];

    // #749: the share this connection is a mount OF, kept so the connection can
    // be re-established without the caller that first mounted it. Without this
    // a dropped session could only be recovered by whoever still happened to
    // know the share name, which in practice was nobody.
    char share_name[64];
} smb_connection_t;

// SMB open file handle
typedef struct {
    bool active;
    smb_connection_t *conn;
    uint64_t file_id_persistent;
    uint64_t file_id_volatile;
    uint64_t position;          // Current read/write position
    uint64_t file_size;
    uint32_t access;
    uint32_t attributes;
    char path[SMB_MAX_PATH];
    bool is_directory;
} smb_file_t;

// SMB directory entry for enumeration
typedef struct {
    char name[SMB_MAX_NAME];
    uint64_t size;
    uint64_t creation_time;
    uint64_t last_access_time;
    uint64_t last_write_time;
    uint32_t attributes;
    bool is_directory;
} smb_dirent_t;

// ============================================================================
// SMB Client API
// ============================================================================

// Initialize SMB client subsystem
void smb_init(void);

// Connect to SMB server and mount share
// url format: smb://[user:pass@]server/share
// Returns connection index on success, -1 on failure
int smb_mount(const char *url, const char *mount_point);

// Mount with explicit credentials
int smb_mount_auth(uint32_t server_ip, const char *share,
                   const char *domain, const char *username,
                   const char *password, const char *mount_point);

// Unmount SMB share
int smb_unmount(const char *mount_point);

// Find connection by mount point
smb_connection_t *smb_find_connection(const char *path);

// List available shares on server (net view equivalent)
// Returns array of share names, sets count. Caller must free.
char **smb_list_shares(uint32_t server_ip, int *count);

// Free share list
void smb_free_shares(char **shares, int count);

// ============================================================================
// SMB File Operations
// ============================================================================

// Open a file
// Returns file index on success, -1 on failure
int smb_open(const char *path, uint32_t desired_access, uint32_t disposition);

// Close a file
int smb_close(int fd);

// Read from file
// Returns bytes read, -1 on error
ssize_t smb_read(int fd, void *buffer, size_t count);

// Write to file
// Returns bytes written, -1 on error
ssize_t smb_write(int fd, const void *buffer, size_t count);

// Seek to position
int64_t smb_seek(int fd, int64_t offset, int whence);

#define SMB_SEEK_SET    0
#define SMB_SEEK_CUR    1
#define SMB_SEEK_END    2

// Get file information
int smb_stat(const char *path, smb_dirent_t *info);

// ============================================================================
// SMB Directory Operations
// ============================================================================

// Open directory for enumeration
// Returns directory handle index, -1 on failure
int smb_opendir(const char *path);

// Read next directory entry
// Returns 0 on success, -1 on error, 1 on end of directory
int smb_readdir(int dirfd, smb_dirent_t *entry);

// Close directory
int smb_closedir(int dirfd);

// Create a directory
int smb_mkdir(const char *path);

// Remove a directory
int smb_rmdir(const char *path);

// ============================================================================
// SMB File Management
// ============================================================================

// Delete a file
int smb_delete(const char *path);

// Rename a file or directory
int smb_rename(const char *oldpath, const char *newpath);

// ============================================================================
// SMB Utility Functions
// ============================================================================

// Convert SMB status to string
const char *smb_strerror(smb2_status_t status);

// Check if path is on an SMB mount
bool smb_is_smb_path(const char *path);

// Print mount info for debugging
void smb_print_mounts(void);

// task #317: VFS routing layer for the "/SMB/<server>/<share>/<path>" prefix.
bool  smb_vfs_is_smb_path(const char *path);
void *smb_vfs_read_whole(const char *path, uint32_t *size_out);
int   smb_vfs_opendir(const char *path);
// task #317 pass 2: on-demand mount + whole-file upload for the syscall layer.
int   smb_vfs_ensure_mount(const char *path);
int   smb_vfs_write_whole(const char *path, const void *data, uint32_t len);
// #smbdisk: `secret` is EITHER a cleartext password OR the persisted
// `nt$<32 hex>` NT hash. This is the shared chokepoint where both forms are
// understood, so no caller has to know which one it is holding.
int   smb_vfs_mount_creds(const char *server, const char *share,
                          const char *user, const char *secret);
uint32_t smb_resolve_ip(const char *host);
void  smb_vfs_set_default_creds(const char *domain, const char *user, const char *secret);
// #smbdisk: NT-hash derivation (MD4 of UTF-16LE(password)). The ONE derivation
// in the tree; crypto/ntlm_nthash_kat.sh pins it to the published MS-NLMP
// 4.2.4 vectors. Exposed for SYS_NET_CRED_TAG so userland can persist a hash
// instead of the password the user typed, WITHOUT a second MD4 in userland.
void  smb_nt_hash(const char *password, uint8_t *out16);
// Boot self-test for the on-disk nt$ encoding (rustkern/ntsecret.rs).
void  nt_secret_selftest(void);
void  smb_run_selftest(void);
void  smb_start_deferred_selftest(void);

// Convert Windows time to Unix timestamp
uint64_t smb_filetime_to_unix(uint64_t filetime);

// Convert Unix timestamp to Windows time
uint64_t smb_unix_to_filetime(uint64_t unix_time);

// ============================================================================
// Internal Functions (exposed for testing)
// ============================================================================

// Low-level protocol operations
int smb2_negotiate(smb_connection_t *conn);
int smb2_session_setup(smb_connection_t *conn);
int smb2_tree_connect(smb_connection_t *conn, const char *share);
int smb2_tree_disconnect(smb_connection_t *conn);
int smb2_logoff(smb_connection_t *conn);

// NTLM helpers
void ntlm_compute_response(const uint8_t *nt_hash, const uint8_t *challenge,
                           uint8_t *lm_response, uint8_t *nt_response);
// `session_key` (16 bytes, may be NULL) receives the NTLMv2 ExportedSessionKey,
// which is what SMB2 message signing is keyed from. Without it there is no
// signing at all, which is the state this client shipped in: the key was
// computed as an intermediate value and discarded.
//
// With NTLMSSP_NEGOTIATE_KEY_EXCH not negotiated (smb2_session_setup does not
// set it) the ExportedSessionKey IS the SessionBaseKey, per MS-NLMP 3.1.5.1.2,
// and for NTLMv2 the SessionBaseKey is HMAC-MD5(NTOWFv2, NTProofStr).
void ntlm_compute_ntlmv2_response(const char *domain, const char *username,
                                   const uint8_t *nt_hash, const uint8_t *challenge,
                                   const uint8_t *target_info, uint32_t target_info_len,
                                   uint8_t *response, uint32_t *response_len,
                                   uint8_t *session_key);

#endif // SMB_H
