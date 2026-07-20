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
    STATUS_NO_MORE_FILES            = 0x80000006
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

    // Message tracking
    uint64_t message_id;
    uint16_t credits;

    // Authentication
    char domain[64];
    char username[64];
    char password[64];
    uint8_t session_key[16];

    // Mount point info
    char mount_point[SMB_MAX_PATH];
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
int   smb_vfs_mount_creds(const char *server, const char *share,
                          const char *user, const char *pass);
uint32_t smb_resolve_ip(const char *host);
void  smb_vfs_set_default_creds(const char *domain, const char *user, const char *pass);
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
void ntlm_compute_response(const char *password, const uint8_t *challenge,
                           uint8_t *lm_response, uint8_t *nt_response);
void ntlm_compute_ntlmv2_response(const char *domain, const char *username,
                                   const char *password, const uint8_t *challenge,
                                   const uint8_t *target_info, uint32_t target_info_len,
                                   uint8_t *response, uint32_t *response_len);

#endif // SMB_H
