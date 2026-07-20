// wget.h - HTTP/1.1 client for MayteraOS
// Supports: keep-alive, chunked encoding, redirects, content-type parsing
#ifndef WGET_H
#define WGET_H

#include "../types.h"

// Maximum URL/path lengths
#define WGET_MAX_URL        1024
#define WGET_MAX_HOST       256
#define WGET_MAX_PATH       1024

// HTTP response buffer size.
// 1MB. The b820 bump to 16MB (so one async fetch could hold the whole ~14.5MB
// OTA kernel) is reverted (#492): it was bloat that did NOT fix the download,
// and sized EVERY transient fetch buffer to the largest possible body. The real
// large-download fix is chunked in-regime Range requests streamed to disk (see
// userland/apps/otaupd), where every single response stays well under 1MB.
#define WGET_BUFFER_SIZE    1048576   // 1MB (reverted from the b820 16MB demo bump, #492)

// Maximum redirects to follow
#define HTTP_MAX_REDIRECTS  10

// Connection pool settings
#define HTTP_MAX_CONNECTIONS    8
#define HTTP_CONN_TIMEOUT       300     // ~16 seconds at 18.2 ticks/sec
#define HTTP_IDLE_TIMEOUT       1800    // ~100 seconds idle before close

// Wget result codes
#define WGET_SUCCESS            0
#define WGET_ERR_INVALID_URL    -1
#define WGET_ERR_NO_NETWORK     -2
#define WGET_ERR_DNS_FAILED     -3
#define WGET_ERR_CONNECT_FAILED -4
#define WGET_ERR_SEND_FAILED    -5
#define WGET_ERR_TIMEOUT        -6
#define WGET_ERR_NO_MEMORY      -7
#define WGET_ERR_HTTP_ERROR     -8
#define WGET_ERR_FILE_ERROR     -9
#define WGET_ERR_TOO_MANY_REDIRECTS -10
#define WGET_ERR_TRUNCATED      -11  // body shorter than advertised Content-Length,
                                     // or chunked encoding never reached its
                                     // terminating chunk (fix-ssrf-contentlength)
#define WGET_ERR_SSRF_BLOCKED   -12  // redirect target refused: private/loopback/
                                     // link-local host from a public origin, or a
                                     // scheme downgrade (https -> http)

// Wget state for non-blocking operation
typedef enum {
    WGET_STATE_IDLE = 0,
    WGET_STATE_CONNECTING,
    WGET_STATE_SENDING,
    WGET_STATE_RECEIVING_HEADERS,
    WGET_STATE_RECEIVING_BODY,
    WGET_STATE_COMPLETE,
    WGET_STATE_ERROR
} wget_state_t;

// ============================================================================
// HTTP/1.1 Response Structure
// ============================================================================

typedef struct {
    int status_code;                // HTTP status code (200, 301, 404, etc.)
    char status_text[64];           // Status text (e.g., "OK", "Not Found")
    char content_type[128];         // Content-Type header value
    char charset[32];               // Charset from Content-Type (e.g., "utf-8")
    size_t content_length;          // Content-Length (-1 if chunked/unknown)
    bool chunked;                   // Transfer-Encoding: chunked
    bool keep_alive;                // Connection should be kept alive
    char location[512];             // Location header (for redirects)
    char server[64];                // Server header
    char etag[128];                 // ETag header (for caching)
    char last_modified[64];         // Last-Modified header
} http_response_t;

// ============================================================================
// HTTP/1.1 Request Structure
// ============================================================================

typedef struct {
    char method[16];                // HTTP method: GET, POST, HEAD, PUT, DELETE
    char path[1024];                // Request path (e.g., "/index.html")
    char host[256];                 // Host header value
    uint16_t port;                  // Port number (80 for HTTP, 443 for HTTPS)
    char headers[2048];             // Additional headers (already formatted)
    const char *body;               // Request body (for POST/PUT)
    size_t body_length;             // Body length
    bool keep_alive;                // Request keep-alive connection
    int timeout_ms;                 // Request timeout in milliseconds (0 = default)
} http_request_t;

// ============================================================================
// Connection Pool Structure
// ============================================================================

typedef struct {
    int active;                     // Is this connection slot in use?
    int socket;                     // TCP socket descriptor
    char host[256];                 // Connected host
    uint16_t port;                  // Connected port
    uint32_t ip;                    // Resolved IP address
    uint64_t last_used;             // Timer tick when last used
    bool keep_alive;                // Connection supports keep-alive
} http_connection_t;

// ============================================================================
// Legacy wget_http_info_t (for backward compatibility)
// ============================================================================

typedef struct {
    int status_code;                // HTTP status (200, 404, etc.)
    uint32_t content_length;        // Content-Length header value (0 if unknown)
    uint32_t bytes_received;        // Bytes received so far
    char content_type[64];          // Content-Type header
    int chunked;                    // Transfer-Encoding: chunked
    int headers_complete;           // All headers received
} wget_http_info_t;

// ============================================================================
// Wget Context Structure (for non-blocking operations)
// ============================================================================

typedef struct {
    wget_state_t state;
    int socket;                     // TCP socket descriptor

    // URL components
    char host[WGET_MAX_HOST];       // Hostname or IP address
    char path[WGET_MAX_PATH];       // Request path (e.g., "/file.txt")
    uint16_t port;                  // Port (default 80)
    uint32_t host_ip;               // Resolved IP address

    // Response handling
    wget_http_info_t http;
    uint8_t *buffer;                // Response buffer
    uint32_t buffer_size;           // Buffer allocated size
    uint32_t buffer_len;            // Data in buffer

    // For saving to file
    char save_path[256];            // Local save path (empty = display)
    int save_to_file;               // Save to file flag

    // Error tracking
    int error;                      // Last error code
    char error_msg[64];             // Error message

    // Progress callback (optional)
    void (*progress_cb)(uint32_t received, uint32_t total);
} wget_ctx_t;

// ============================================================================
// Initialization
// ============================================================================

// Initialize wget/HTTP subsystem
void wget_init(void);

// ============================================================================
// Legacy API (backward compatible)
// ============================================================================

// Execute wget command (blocking) - simplified interface
// Returns: 0 on success, negative error code on failure
// url: "http://ip.address/path" or just "ip.address/path"
// save_path: NULL to display content, otherwise save to file
int wget_execute(const char *url, const char *save_path);

// Execute wget and return response body
// Returns: 0 on success, negative error code on failure
// body_out: Pointer to receive allocated body buffer (caller must kfree)
// body_len_out: Pointer to receive body length
// status_out: Pointer to receive HTTP status code (optional, can be NULL)
int wget_fetch(const char *url, uint8_t **body_out, uint32_t *body_len_out, int *status_out);
// #414 GET/POST with caller-supplied extra headers (Authorization: Bearer ...).
int wget_fetch_hdr(const char *url, const char *extra_headers, uint8_t **body_out, uint32_t *body_len_out, int *status_out);
int wget_post_hdr(const char *url, const char *extra_headers, const char *reqbody, uint8_t **body_out, uint32_t *body_len_out, int *status_out);

// ============================================================================
// New HTTP/1.1 API
// ============================================================================

// Perform HTTP request with full control
// Returns: 0 on success, negative error code on failure
// req: Request parameters
// resp: Response headers (filled on return)
// body_buf: Buffer to receive response body
// body_buf_size: Size of body buffer
// body_len_out: Actual body length received
int http_request(http_request_t *req, http_response_t *resp,
                 uint8_t *body_buf, size_t body_buf_size, size_t *body_len_out);

// Simplified GET request
// Returns: 0 on success, negative error code on failure
int http_get(const char *url, http_response_t *resp,
             uint8_t *body_buf, size_t body_buf_size, size_t *body_len_out);

// Simplified POST request
// Returns: 0 on success, negative error code on failure
int http_post(const char *url, const char *content_type,
              const void *body, size_t body_len,
              http_response_t *resp,
              uint8_t *resp_buf, size_t resp_buf_size, size_t *resp_len_out);

// HEAD request (get headers only)
// Returns: 0 on success, negative error code on failure
int http_head(const char *url, http_response_t *resp);

// ============================================================================
// URL Parsing
// ============================================================================

// Parse URL into components
// Returns: 0 on success, -1 on invalid URL
// Supports: http://host/path, http://host:port/path, //host/path, /path
int wget_parse_url(const char *url, char *host, char *path, uint16_t *port);

// Parse full URL into request structure
// Also handles https:// (port 443, but actual TLS must be handled separately)
int http_parse_url(const char *url, http_request_t *req);

// Build absolute URL from base URL and relative reference
// Returns: 0 on success, -1 on error
int http_resolve_url(const char *base_url, const char *relative, char *result, size_t result_size);

// ============================================================================
// Connection Pool Management
// ============================================================================

// Get a connection from the pool (or create new one)
// Returns socket descriptor or negative error
int http_get_connection(const char *host, uint16_t port, uint32_t ip);

// Return a connection to the pool (keep-alive) or close it
void http_release_connection(int sock, bool keep_alive);

// Close all idle connections
void http_close_idle_connections(void);

// Close all connections (cleanup)
void http_close_all_connections(void);

// ============================================================================
// Utility Functions
// ============================================================================

// Parse IP address string (e.g., "192.0.2.1")
// Returns: IP address in host byte order, 0 on failure
uint32_t wget_parse_ip(const char *ip_str);

// Get error string for error code
const char *wget_strerror(int error);

// Initialize http_request_t with defaults
void http_request_init(http_request_t *req);

// Initialize http_response_t
void http_response_init(http_response_t *resp);

// Add a header to request (appends to req->headers)
// Returns: 0 on success, -1 if headers buffer full
int http_add_header(http_request_t *req, const char *name, const char *value);

// ============================================================================
// Internal Helpers (exposed for browser use)
// ============================================================================

// Parse HTTP response headers from buffer
// Returns: offset to body start, or -1 if headers incomplete
int http_parse_response_headers(const uint8_t *buf, size_t len, http_response_t *resp);

// Decode chunked transfer encoding
// Returns: decoded body length, or -1 on error
// Decodes in-place (output overwrites input starting at body_start)
ssize_t http_decode_chunked(uint8_t *buf, size_t len);

// Parse Content-Type header and extract charset
void http_parse_content_type(const char *header, char *content_type, size_t ct_size,
                             char *charset, size_t cs_size);

#endif // WGET_H
