// ftp.h - FTP Client for MayteraOS
// Implements RFC 959 File Transfer Protocol
#ifndef FTP_H
#define FTP_H

#include "../types.h"

// FTP default ports
#define FTP_CONTROL_PORT    21
#define FTP_DATA_PORT       20

// FTP buffer sizes
#define FTP_CMD_BUFFER_SIZE     512
#define FTP_RESP_BUFFER_SIZE    1024
#define FTP_DATA_BUFFER_SIZE    8192
#define FTP_PATH_MAX            256

// FTP response codes
#define FTP_RESP_READY              220     // Service ready
#define FTP_RESP_GOODBYE            221     // Service closing
#define FTP_RESP_DATA_CONN_OPEN     125     // Data connection open
#define FTP_RESP_FILE_OK            150     // File status okay
#define FTP_RESP_OK                 200     // Command okay
#define FTP_RESP_SYSTEM             215     // System type
#define FTP_RESP_WELCOME            220     // Welcome message
#define FTP_RESP_LOGGED_IN          230     // User logged in
#define FTP_RESP_FILE_ACTION_OK     250     // File action completed
#define FTP_RESP_PATHNAME           257     // Pathname created
#define FTP_RESP_NEED_PASSWORD      331     // User name okay, need password
#define FTP_RESP_NEED_ACCOUNT       332     // Need account for login
#define FTP_RESP_PENDING            350     // Requested file action pending
#define FTP_RESP_SERVICE_UNAVAIL    421     // Service not available
#define FTP_RESP_CANT_OPEN_DATA     425     // Can't open data connection
#define FTP_RESP_CONN_CLOSED        426     // Connection closed
#define FTP_RESP_ACTION_NOT_TAKEN   450     // Requested file action not taken
#define FTP_RESP_LOCAL_ERROR        451     // Local error
#define FTP_RESP_INSUFF_STORAGE     452     // Insufficient storage
#define FTP_RESP_SYNTAX_ERROR       500     // Syntax error
#define FTP_RESP_PARAM_ERROR        501     // Syntax error in parameters
#define FTP_RESP_NOT_IMPLEMENTED    502     // Command not implemented
#define FTP_RESP_BAD_SEQUENCE       503     // Bad sequence of commands
#define FTP_RESP_NOT_IMPL_PARAM     504     // Command not implemented for parameter
#define FTP_RESP_NOT_LOGGED_IN      530     // Not logged in
#define FTP_RESP_NEED_ACCT_STOR     532     // Need account for storing files
#define FTP_RESP_FILE_UNAVAIL       550     // File unavailable
#define FTP_RESP_EXCEEDED_STORAGE   552     // Exceeded storage allocation
#define FTP_RESP_NAME_NOT_ALLOWED   553     // File name not allowed
#define FTP_RESP_TRANSFER_OK        226     // Transfer complete
#define FTP_RESP_PASV_MODE          227     // Entering Passive Mode

// FTP error codes
#define FTP_SUCCESS                 0
#define FTP_ERR_NO_MEMORY           -1
#define FTP_ERR_CONNECT_FAILED      -2
#define FTP_ERR_TIMEOUT             -3
#define FTP_ERR_NOT_CONNECTED       -4
#define FTP_ERR_LOGIN_FAILED        -5
#define FTP_ERR_COMMAND_FAILED      -6
#define FTP_ERR_TRANSFER_FAILED     -7
#define FTP_ERR_PASV_FAILED         -8
#define FTP_ERR_NO_NETWORK          -9
#define FTP_ERR_INVALID_RESPONSE    -10
#define FTP_ERR_FILE_ERROR          -11
#define FTP_ERR_ALREADY_CONNECTED   -12

// FTP transfer types
typedef enum {
    FTP_TYPE_ASCII = 'A',
    FTP_TYPE_BINARY = 'I'
} ftp_transfer_type_t;

// FTP connection state
typedef enum {
    FTP_STATE_DISCONNECTED = 0,
    FTP_STATE_CONNECTING,
    FTP_STATE_CONNECTED,
    FTP_STATE_LOGGED_IN,
    FTP_STATE_TRANSFERRING
} ftp_state_t;

// FTP session structure
typedef struct {
    ftp_state_t state;

    // Control connection
    int ctrl_sock;
    uint32_t server_ip;
    uint16_t server_port;

    // Data connection (for PASV mode)
    int data_sock;
    uint32_t data_ip;
    uint16_t data_port;

    // Authentication
    char username[64];
    char password[64];
    int logged_in;

    // Transfer state
    ftp_transfer_type_t transfer_type;

    // Response handling
    char response_buffer[FTP_RESP_BUFFER_SIZE];
    int response_code;

    // Current working directory (from server)
    char cwd[FTP_PATH_MAX];

    // Error tracking
    int last_error;
    char error_msg[128];
} ftp_session_t;

// Initialize FTP subsystem
void ftp_init(void);

// Create a new FTP session
ftp_session_t *ftp_session_create(void);

// Destroy FTP session
void ftp_session_destroy(ftp_session_t *session);

// Connect to FTP server
// server_ip: IP address in host byte order
// port: Server port (usually 21)
int ftp_connect(ftp_session_t *session, uint32_t server_ip, uint16_t port);

// Disconnect from FTP server
void ftp_disconnect(ftp_session_t *session);

// Login to FTP server
int ftp_login(ftp_session_t *session, const char *username, const char *password);

// Anonymous login (convenience wrapper)
int ftp_login_anonymous(ftp_session_t *session);

// Change working directory
int ftp_cd(ftp_session_t *session, const char *path);

// Change to parent directory
int ftp_cdup(ftp_session_t *session);

// Get current working directory
int ftp_pwd(ftp_session_t *session, char *path_out, int max_len);

// List directory contents
// If callback is NULL, prints to kprintf
// Returns 0 on success, negative on error
typedef void (*ftp_list_callback_t)(const char *line, void *user_data);
int ftp_ls(ftp_session_t *session, const char *path, ftp_list_callback_t callback, void *user_data);

// Download file
// data_out: Will be allocated and must be freed by caller with kfree()
// size_out: Will contain the file size
int ftp_get(ftp_session_t *session, const char *remote_path, uint8_t **data_out, uint32_t *size_out);

// Upload file
int ftp_put(ftp_session_t *session, const char *remote_path, const uint8_t *data, uint32_t size);

// Set transfer type (ASCII or BINARY)
int ftp_set_type(ftp_session_t *session, ftp_transfer_type_t type);

// Get file size (uses SIZE command if supported)
int ftp_size(ftp_session_t *session, const char *path, uint32_t *size_out);

// Delete remote file
int ftp_delete(ftp_session_t *session, const char *path);

// Create remote directory
int ftp_mkdir(ftp_session_t *session, const char *path);

// Remove remote directory
int ftp_rmdir(ftp_session_t *session, const char *path);

// Rename remote file
int ftp_rename(ftp_session_t *session, const char *from, const char *to);

// Get system type
int ftp_syst(ftp_session_t *session, char *syst_out, int max_len);

// Send NOOP (keep-alive)
int ftp_noop(ftp_session_t *session);

// Send raw FTP command
int ftp_command(ftp_session_t *session, const char *cmd);

// Get last response code
int ftp_get_response_code(ftp_session_t *session);

// Get last response message
const char *ftp_get_response_msg(ftp_session_t *session);

// Get error string
const char *ftp_strerror(int error);

// Check if connected
int ftp_is_connected(ftp_session_t *session);

// Check if logged in
int ftp_is_logged_in(ftp_session_t *session);

// Parse IP address string to host byte order
uint32_t ftp_parse_ip(const char *ip_str);

#endif // FTP_H
