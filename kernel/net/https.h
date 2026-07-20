// https.h - HTTPS client for MayteraOS
// Wraps TCP sockets with TLS for secure HTTP connections
#ifndef HTTPS_H
#define HTTPS_H

#include "../types.h"

// HTTPS error codes
#define HTTPS_SUCCESS           0
#define HTTPS_ERR_NO_MEMORY     -1
#define HTTPS_ERR_DNS           -2
#define HTTPS_ERR_CONNECT       -3
#define HTTPS_ERR_TLS           -4
#define HTTPS_ERR_TIMEOUT       -5
#define HTTPS_ERR_CLOSED        -6
#define HTTPS_ERR_TRUNCATED     -7   // body shorter than advertised Content-Length,
                                     // or chunked encoding never reached its
                                     // terminating chunk (fix-ssrf-contentlength)
#define HTTPS_ERR_SSRF_BLOCKED  -8   // redirect target refused: private/loopback/
                                     // link-local host from a public origin, or a
                                     // scheme downgrade (https -> http)

// Forward declaration
struct https_conn;
typedef struct https_conn https_conn_t;

// Create HTTPS connection to host:port
// hostname: Server hostname (used for DNS and SNI)
// port: Server port (usually 443)
// Returns: Connection handle, or NULL on error
https_conn_t *https_connect(const char *hostname, uint16_t port);

// Send data over HTTPS connection
// Returns: Number of bytes sent, or negative error code
int https_send(https_conn_t *conn, const void *data, size_t length);

// Receive data from HTTPS connection
// Returns: Number of bytes received, or negative error code
int https_recv(https_conn_t *conn, void *buffer, size_t length);

// Close HTTPS connection
void https_close(https_conn_t *conn);

// Check if connection is established
int https_is_connected(https_conn_t *conn);

// Get error string
const char *https_strerror(int error);

// =============================================================================
// High-level HTTPS request functions
// =============================================================================

// HTTPS GET request
// Returns: 0 on success, negative error code on failure
// body_out: Receives allocated body buffer (caller must kfree)
// body_len_out: Receives body length
// status_out: Receives HTTP status code
int https_get(const char *url, uint8_t **body_out, uint32_t *body_len_out, int *status_out);

// HTTPS POST: send `body` with caller-supplied extra `headers` (CRLF-terminated
// lines, e.g. "Authorization: Bearer X\r\nContent-Type: application/json\r\n").
// Returns the response body (chunked decoded). Caller kfree()s *body_out.
int https_post(const char *url, const char *headers, const char *body,
               uint8_t **body_out, uint32_t *body_len_out, int *status_out);

// Initialize HTTPS subsystem
void https_init(void);

#endif // HTTPS_H
