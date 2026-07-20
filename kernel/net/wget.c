// wget.c - HTTP/1.1 client for MayteraOS
// Supports: keep-alive connections, chunked encoding, redirects, content-type parsing
// Note: No floating point - uses integer math only

#include "wget.h"
#include "tcp.h"
#include "ip.h"
#include "dns.h"
#include "https.h"   // #497 fault 4: HTTPS_ERR_* codes for the http->https redirect re-dispatch
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../gui/syslog.h"

// External declarations
extern void net_poll(void);
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;            // #420: actual PIT frequency (250Hz), NOT the
                                        // classic 18.2Hz PC timer the old ticks math assumed
extern void proc_sleep(uint32_t ms);   // yield the CPU during network waits

// Forward declaration for snprintf replacement
int snprintf_simple(char *buf, int size, const char *fmt, ...);

// ============================================================================
// Connection Pool
// ============================================================================

static http_connection_t connection_pool[HTTP_MAX_CONNECTIONS];
static bool connection_pool_initialized = false;

// Initialize connection pool
static void init_connection_pool(void) {
    if (!connection_pool_initialized) {
        for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
            connection_pool[i].active = 0;
            connection_pool[i].socket = -1;
        }
        connection_pool_initialized = true;
    }
}

// Find existing connection to host:port
static int find_connection(const char *host, uint16_t port) {
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (connection_pool[i].active &&
            connection_pool[i].port == port &&
            strcmp(connection_pool[i].host, host) == 0) {

            // Check if connection is still valid
            tcp_state_t state = tcp_get_state(connection_pool[i].socket);
            if (state == TCP_STATE_ESTABLISHED) {
                return i;
            }

            // Connection no longer valid, clean up
            tcp_close(connection_pool[i].socket);
            connection_pool[i].active = 0;
            connection_pool[i].socket = -1;
        }
    }
    return -1;
}

// Find free slot in connection pool
static int find_free_slot(void) {
    // First, try to find an empty slot
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (!connection_pool[i].active) {
            return i;
        }
    }

    // All slots full, find oldest idle connection
    int oldest = -1;
    uint64_t oldest_time = timer_ticks;

    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (connection_pool[i].last_used < oldest_time) {
            oldest_time = connection_pool[i].last_used;
            oldest = i;
        }
    }

    if (oldest >= 0) {
        // Close the oldest connection
        tcp_close(connection_pool[oldest].socket);
        connection_pool[oldest].active = 0;
        connection_pool[oldest].socket = -1;
        return oldest;
    }

    return -1;
}

// Get a connection from the pool or create a new one
int http_get_connection(const char *host, uint16_t port, uint32_t ip) {
    init_connection_pool();

    // Look for existing connection
    int idx = find_connection(host, port);
    if (idx >= 0) {
        kprintf("[HTTP] Reusing connection to %s:%d\n", host, port);
        connection_pool[idx].last_used = timer_ticks;
        return connection_pool[idx].socket;
    }

    // Create new connection
    int sock = tcp_socket();
    if (sock < 0) {
        return WGET_ERR_NO_MEMORY;
    }

    // Initiate connection
    int result = tcp_connect(sock, ip, port);
    if (result != TCP_ERR_IN_PROGRESS && result < 0) {
        tcp_close(sock);
        return WGET_ERR_CONNECT_FAILED;
    }

    // Wait for connection
    // #420: HTTP_CONN_TIMEOUT (300) was defined assuming the classic 18.2
    // ticks/sec PC timer ("~16 seconds"), the same bug class as the recv-side
    // idle timeout above -- at the kernel's real 250Hz PIT rate that macro is
    // only ~1.2s, which can spuriously fail a slow-but-fine connect under
    // load. Recompute the intended ~16s from the real tick rate instead.
    uint64_t start_time = timer_ticks;
    uint32_t conn_hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t conn_timeout_ticks = conn_hz * 16;
    while (!tcp_is_connected(sock)) {
        net_poll();
        tcp_timer();

        if (timer_ticks - start_time > conn_timeout_ticks) {
            tcp_close(sock);
            return WGET_ERR_TIMEOUT;
        }

        tcp_state_t state = tcp_get_state(sock);
        if (state == TCP_STATE_CLOSED) {
            tcp_close(sock);
            return WGET_ERR_CONNECT_FAILED;
        }

        // Yield so the compositor and other processes run during the connect
        // wait (this loop is a synchronous syscall; busy-spinning froze the OS).
        proc_sleep(2);
    }

    // Store in pool
    idx = find_free_slot();
    if (idx >= 0) {
        connection_pool[idx].active = 1;
        connection_pool[idx].socket = sock;
        strncpy(connection_pool[idx].host, host, sizeof(connection_pool[idx].host) - 1);
        connection_pool[idx].host[sizeof(connection_pool[idx].host) - 1] = '\0';
        connection_pool[idx].port = port;
        connection_pool[idx].ip = ip;
        connection_pool[idx].last_used = timer_ticks;
        connection_pool[idx].keep_alive = true;
    }

    return sock;
}

// Release connection back to pool or close it
void http_release_connection(int sock, bool keep_alive) {
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (connection_pool[i].active && connection_pool[i].socket == sock) {
            if (keep_alive) {
                // Keep the connection in the pool
                connection_pool[i].last_used = timer_ticks;
                connection_pool[i].keep_alive = keep_alive;
                kprintf("[HTTP] Keeping connection alive\n");
            } else {
                // Close and remove from pool
                tcp_close(sock);
                connection_pool[i].active = 0;
                connection_pool[i].socket = -1;
                kprintf("[HTTP] Closed connection\n");
            }
            return;
        }
    }

    // Not in pool, just close it
    tcp_close(sock);
}

// Close all idle connections
void http_close_idle_connections(void) {
    uint64_t now = timer_ticks;
    // #420: same 18.2Hz-vs-250Hz tick-rate bug as HTTP_CONN_TIMEOUT -- the
    // macro's "~100 seconds" was really only ~7.2s at the real PIT rate,
    // closing keep-alive connections far sooner than intended.
    uint32_t idle_hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t idle_close_ticks = idle_hz * 100;

    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (connection_pool[i].active) {
            if (now - connection_pool[i].last_used > idle_close_ticks) {
                tcp_close(connection_pool[i].socket);
                connection_pool[i].active = 0;
                connection_pool[i].socket = -1;
                kprintf("[HTTP] Closed idle connection to %s:%d\n",
                       connection_pool[i].host, connection_pool[i].port);
            }
        }
    }
}

// Close all connections
void http_close_all_connections(void) {
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (connection_pool[i].active) {
            tcp_close(connection_pool[i].socket);
            connection_pool[i].active = 0;
            connection_pool[i].socket = -1;
        }
    }
}

// ============================================================================
// Initialization
// ============================================================================

void wget_init(void) {
    init_connection_pool();
    kprintf("[HTTP] HTTP/1.1 client initialized\n");
}

// ============================================================================
// String/Parsing Utilities
// ============================================================================

// Simple snprintf replacement (no floating point)
int snprintf_simple(char *buf, int size, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    int written = 0;
    while (*fmt && written < size - 1) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 's') {
                const char *s = __builtin_va_arg(ap, const char *);
                while (*s && written < size - 1) {
                    buf[written++] = *s++;
                }
            } else if (*fmt == 'd') {
                int val = __builtin_va_arg(ap, int);
                char tmp[16];
                int neg = 0;
                if (val < 0) {
                    neg = 1;
                    val = -val;
                }
                int i = 0;
                do {
                    tmp[i++] = '0' + (val % 10);
                    val /= 10;
                } while (val > 0);
                if (neg && written < size - 1) buf[written++] = '-';
                while (i > 0 && written < size - 1) {
                    buf[written++] = tmp[--i];
                }
            } else if (*fmt == 'u') {
                unsigned int val = __builtin_va_arg(ap, unsigned int);
                char tmp[16];
                int i = 0;
                do {
                    tmp[i++] = '0' + (val % 10);
                    val /= 10;
                } while (val > 0);
                while (i > 0 && written < size - 1) {
                    buf[written++] = tmp[--i];
                }
            } else if (*fmt == 'l' && *(fmt+1) == 'u') {
                // Handle %lu for size_t
                fmt++;
                unsigned long val = __builtin_va_arg(ap, unsigned long);
                char tmp[24];
                int i = 0;
                do {
                    tmp[i++] = '0' + (val % 10);
                    val /= 10;
                } while (val > 0);
                while (i > 0 && written < size - 1) {
                    buf[written++] = tmp[--i];
                }
            } else if (*fmt == 'z' && *(fmt+1) == 'u') {
                // Handle %zu for size_t
                fmt++;
                size_t val = __builtin_va_arg(ap, size_t);
                char tmp[24];
                int i = 0;
                do {
                    tmp[i++] = '0' + (val % 10);
                    val /= 10;
                } while (val > 0);
                while (i > 0 && written < size - 1) {
                    buf[written++] = tmp[--i];
                }
            } else if (*fmt == '%') {
                buf[written++] = '%';
            }
            fmt++;
        } else {
            buf[written++] = *fmt++;
        }
    }

    buf[written] = '\0';
    __builtin_va_end(ap);
    return written;
}

// Parse IP address string
uint32_t wget_parse_ip(const char *ip_str) {
    uint8_t octets[4] = {0};
    int octet_idx = 0;
    int value = 0;

    for (const char *c = ip_str; octet_idx < 4; c++) {
        if (*c >= '0' && *c <= '9') {
            value = value * 10 + (*c - '0');
            if (value > 255) return 0;
        } else if (*c == '.' || *c == '\0' || *c == '/' || *c == ':') {
            octets[octet_idx++] = (uint8_t)value;
            value = 0;
            if (*c != '.') break;
        } else {
            return 0;
        }
    }

    if (octet_idx != 4) return 0;

    return ((uint32_t)octets[0] << 24) | ((uint32_t)octets[1] << 16) |
           ((uint32_t)octets[2] << 8) | (uint32_t)octets[3];
}

// Convert character to lowercase
static char tolower_char(char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

// Case-insensitive string compare (for headers)
static int strcasecmp_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = tolower_char(a[i]);
        char cb = tolower_char(b[i]);
        if (ca != cb) return ca - cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

// Find header in buffer (case-insensitive)
static const char *find_header(const char *buf, size_t len, const char *header) {
    size_t header_len = strlen(header);
    for (size_t i = 0; i + header_len < len; i++) {
        if (strcasecmp_n(&buf[i], header, header_len) == 0) {
            return &buf[i];
        }
    }
    return NULL;
}

// ============================================================================
// #404 Phase Y: HTTP response length-parse seam routed to Rust (rustkern.rs)
// under -DRUST_HTTP_PARSE. The chunked decode, header-block CRLFCRLF framing,
// and Content-Length digit parse cross into the bounds-checked Rust seam; the
// socket recv + body buffering stay in C. Each C reference is kept verbatim as
// <fn>_c for the boot-time [RUST-DIFF] differential and a flag-off rollback.
// These wget-side seams are LATENT/defense-in-depth (already correctly bounded);
// the REACHABLE OOB (MAYTERA-SEC-2026-0008) is in net/https.c https_dechunk.
// ============================================================================
extern long   http_decode_chunked_rs(uint8_t *buf, unsigned long len);
extern int    http_find_header_end_rs(const uint8_t *buf, uint32_t len);
extern unsigned long http_parse_uint_rs(const uint8_t *s, unsigned long len);

// Find end of HTTP headers (verbatim reference; used when -DRUST_HTTP_PARSE is
// off, and mirrored by the boot-time differential in net/https.c).
static int __attribute__((unused)) find_header_end_c(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            return (int)(i + 4);
        }
    }
    return -1;
}
static int find_header_end(const uint8_t *buf, size_t len) {
#ifdef RUST_HTTP_PARSE
    return http_find_header_end_rs(buf, (uint32_t)len);
#else
    return find_header_end_c(buf, len);
#endif
}

// Parse unsigned integer from string (verbatim reference; used when
// -DRUST_HTTP_PARSE is off).
static size_t __attribute__((unused)) parse_uint_c(const char *s) {
    size_t val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val;
}
static size_t parse_uint(const char *s) {
#ifdef RUST_HTTP_PARSE
    // `s` points into the NUL-terminated response buffer (recv_buf[recv_len] =
    // '\0'); strlen bounds the Rust slice to the received bytes.
    size_t n = 0; while (s[n]) n++;
    return (size_t)http_parse_uint_rs((const uint8_t *)s, n);
#else
    return parse_uint_c(s);
#endif
}


// ============================================================================
// URL Parsing
// ============================================================================

// Parse URL into components
int wget_parse_url(const char *url, char *host, char *path, uint16_t *port) {
    const char *p = url;

    // Default values
    *port = 80;
    path[0] = '/';
    path[1] = '\0';
    host[0] = '\0';

    // Skip protocol prefix
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        *port = 443;  // Note: TLS not implemented yet, but set correct port
    } else if (strncmp(p, "//", 2) == 0) {
        p += 2;  // Protocol-relative URL
    }

    // Extract host
    int host_len = 0;
    while (*p && *p != ':' && *p != '/' && *p != '?' && host_len < WGET_MAX_HOST - 1) {
        host[host_len++] = *p++;
    }
    host[host_len] = '\0';

    if (host_len == 0) return -1;

    // Check for port
    if (*p == ':') {
        p++;
        int port_val = 0;
        while (*p >= '0' && *p <= '9') {
            port_val = port_val * 10 + (*p - '0');
            p++;
        }
        if (port_val > 0 && port_val < 65536) {
            *port = (uint16_t)port_val;
        }
    }

    // Extract path (including query string)
    if (*p == '/' || *p == '?') {
        int path_len = 0;
        while (*p && path_len < WGET_MAX_PATH - 1) {
            path[path_len++] = *p++;
        }
        path[path_len] = '\0';
    }

    return 0;
}

// Parse URL into request structure
int http_parse_url(const char *url, http_request_t *req) {
    return wget_parse_url(url, req->host, req->path, &req->port);
}

// Resolve relative URL against base
int http_resolve_url(const char *base_url, const char *relative, char *result, size_t result_size) {
    // If relative is absolute URL, use it directly
    if (strncmp(relative, "http://", 7) == 0 || strncmp(relative, "https://", 8) == 0) {
        strncpy(result, relative, result_size - 1);
        result[result_size - 1] = '\0';
        return 0;
    }

    // Parse base URL
    char base_host[256];
    char base_path[1024];
    uint16_t base_port;

    if (wget_parse_url(base_url, base_host, base_path, &base_port) < 0) {
        return -1;
    }

    // Protocol-relative URL
    if (strncmp(relative, "//", 2) == 0) {
        const char *proto = (base_port == 443) ? "https:" : "http:";
        snprintf_simple(result, result_size, "%s%s", proto, relative);
        return 0;
    }

    // Absolute path
    if (relative[0] == '/') {
        const char *proto = (base_port == 443) ? "https://" : "http://";
        if (base_port == 80 || base_port == 443) {
            snprintf_simple(result, result_size, "%s%s%s", proto, base_host, relative);
        } else {
            snprintf_simple(result, result_size, "%s%s:%d%s", proto, base_host, base_port, relative);
        }
        return 0;
    }

    // Relative path - resolve against base path
    // Find last / in base path
    int last_slash = -1;
    for (int i = 0; base_path[i]; i++) {
        if (base_path[i] == '/') last_slash = i;
    }

    char new_path[1024];
    if (last_slash >= 0) {
        // Copy base path up to and including last /
        int len = 0;
        for (int i = 0; i <= last_slash && len < (int)sizeof(new_path) - 1; i++) {
            new_path[len++] = base_path[i];
        }
        // Append relative path
        for (int i = 0; relative[i] && len < (int)sizeof(new_path) - 1; i++) {
            new_path[len++] = relative[i];
        }
        new_path[len] = '\0';
    } else {
        snprintf_simple(new_path, sizeof(new_path), "/%s", relative);
    }

    const char *proto = (base_port == 443) ? "https://" : "http://";
    if (base_port == 80 || base_port == 443) {
        snprintf_simple(result, result_size, "%s%s%s", proto, base_host, new_path);
    } else {
        snprintf_simple(result, result_size, "%s%s:%d%s", proto, base_host, base_port, new_path);
    }

    return 0;
}

// #fix-ssrf-contentlength: is this IPv4 address (host byte order -- high byte
// is the first octet, matching wget_parse_ip()/dns_resolve()'s convention
// here) private, loopback, or link-local? A public-origin redirect must
// never be allowed to steer the kernel HTTP client at one of these (classic
// SSRF via a 3xx Location header), including cloud-metadata addresses which
// live in the 169.254.0.0/16 link-local block.
static int wget_ip_is_private(uint32_t ip) {
    uint8_t a = (uint8_t)(ip >> 24);
    uint8_t b = (uint8_t)(ip >> 16);
    if (ip == 0) return 1;                          // 0.0.0.0/8
    if (a == 10) return 1;                          // 10.0.0.0/8
    if (a == 127) return 1;                         // 127.0.0.0/8 (loopback)
    if (a == 169 && b == 254) return 1;              // 169.254.0.0/16 (link-local/metadata)
    if (a == 172 && b >= 16 && b <= 31) return 1;    // 172.16.0.0/12
    if (a == 192 && b == 168) return 1;              // 192.168.0.0/16
    return 0;
}

// Resolve `host` (hostname or literal IP) and report whether it lands on a
// private/loopback/link-local address. Resolving (rather than only checking
// literal dotted-quad text) also closes DNS-rebinding: a hostname Location
// that resolves to 127.0.0.1 is blocked the same as a literal IP would be.
static int wget_host_is_private(const char *host) {
    uint32_t ip = 0;
    if (dns_resolve(host, &ip) != 0) return 0;   // unresolvable: let the normal
                                                  // connect path fail it on its own
    return wget_ip_is_private(ip);
}

// Is `url` an https:// URL? Used to refuse a redirect that downgrades the
// scheme from https to plain http (case-insensitive: schemes are).
static int wget_url_is_https(const char *url) {
    return strcasecmp_n(url, "https://", 8) == 0;
}

// #fix-ssrf-contentlength: gate a redirect target before following it.
// `orig_is_private` reflects the ORIGINAL request (set once, at redirect
// depth 0): if the caller already opted into a private/LAN destination
// (e.g. Home Assistant on 192.168.x.x), redirects among private hosts stay
// allowed; a public-origin request must never be redirected onto an
// internal address. A redirect from https to http is always refused
// (protocol downgrade), regardless of host.
static int wget_redirect_allowed(const char *from_url, const char *to_url, int orig_is_private) {
    if (wget_url_is_https(from_url) && !wget_url_is_https(to_url)) {
        kprintf("[HTTP] Redirect blocked (scheme downgrade https->http): %s\n", to_url);
        return 0;
    }
    char host[WGET_MAX_HOST]; char path[WGET_MAX_PATH]; uint16_t port;
    if (wget_parse_url(to_url, host, path, &port) < 0) {
        kprintf("[HTTP] Redirect blocked (invalid target): %s\n", to_url);
        return 0;
    }
    if (!orig_is_private && wget_host_is_private(host)) {
        kprintf("[HTTP] Redirect blocked (SSRF): target host '%s' is private/loopback/link-local\n", host);
        return 0;
    }
    return 1;
}

// ============================================================================
// HTTP Header Parsing
// ============================================================================

// Parse Content-Type header and extract charset
void http_parse_content_type(const char *header, char *content_type, size_t ct_size,
                             char *charset, size_t cs_size) {
    // Initialize outputs
    if (content_type && ct_size > 0) content_type[0] = '\0';
    if (charset && cs_size > 0) charset[0] = '\0';

    if (!header) return;

    // Skip "content-type:" prefix if present
    if (strcasecmp_n(header, "content-type:", 13) == 0) {
        header += 13;
    }

    // Skip whitespace
    while (*header == ' ' || *header == '\t') header++;

    // Extract MIME type (up to ; or newline)
    if (content_type && ct_size > 0) {
        size_t i = 0;
        while (*header && *header != ';' && *header != '\r' && *header != '\n' && i < ct_size - 1) {
            content_type[i++] = *header++;
        }
        // Trim trailing whitespace
        while (i > 0 && (content_type[i-1] == ' ' || content_type[i-1] == '\t')) {
            i--;
        }
        content_type[i] = '\0';
    }

    // Look for charset parameter
    if (charset && cs_size > 0) {
        const char *cs = header;
        while (*cs) {
            // Look for "charset="
            if (strcasecmp_n(cs, "charset=", 8) == 0) {
                cs += 8;
                // Skip optional quotes
                if (*cs == '"' || *cs == '\'') cs++;

                size_t i = 0;
                while (*cs && *cs != '"' && *cs != '\'' && *cs != ';' &&
                       *cs != '\r' && *cs != '\n' && *cs != ' ' && i < cs_size - 1) {
                    charset[i++] = tolower_char(*cs++);
                }
                charset[i] = '\0';
                break;
            }
            cs++;
        }
    }
}

// Initialize response structure
void http_response_init(http_response_t *resp) {
    resp->status_code = 0;
    resp->status_text[0] = '\0';
    resp->content_type[0] = '\0';
    resp->charset[0] = '\0';
    resp->content_length = (size_t)-1;  // Unknown
    resp->chunked = false;
    resp->keep_alive = true;  // Default for HTTP/1.1
    resp->location[0] = '\0';
    resp->server[0] = '\0';
    resp->etag[0] = '\0';
    resp->last_modified[0] = '\0';
}

// Parse HTTP response headers
int http_parse_response_headers(const uint8_t *buf, size_t len, http_response_t *resp) {
    // Find end of headers
    int header_end = find_header_end(buf, len);
    if (header_end < 0) {
        return -1;  // Headers incomplete
    }

    // Parse status line: HTTP/1.x NNN Status Text
    const char *line = (const char *)buf;

    // Skip HTTP/1.x
    while (*line && *line != ' ') line++;
    if (*line) line++;  // Skip space

    // Parse status code
    resp->status_code = 0;
    while (*line >= '0' && *line <= '9') {
        resp->status_code = resp->status_code * 10 + (*line - '0');
        line++;
    }

    // Skip space and copy status text
    if (*line == ' ') line++;
    int st_len = 0;
    while (*line && *line != '\r' && *line != '\n' && st_len < (int)sizeof(resp->status_text) - 1) {
        resp->status_text[st_len++] = *line++;
    }
    resp->status_text[st_len] = '\0';

    // Parse headers
    const char *headers = (const char *)buf;

    // Content-Length
    const char *cl = find_header(headers, header_end, "content-length:");
    if (cl) {
        cl += 15;  // Skip "content-length:"
        while (*cl == ' ') cl++;
        resp->content_length = parse_uint(cl);
    }

    // Transfer-Encoding
    const char *te = find_header(headers, header_end, "transfer-encoding:");
    if (te) {
        te += 18;
        while (*te == ' ') te++;
        if (strcasecmp_n(te, "chunked", 7) == 0) {
            resp->chunked = true;
            resp->content_length = (size_t)-1;  // Unknown with chunked
        }
    }

    // Content-Type
    const char *ct = find_header(headers, header_end, "content-type:");
    if (ct) {
        http_parse_content_type(ct, resp->content_type, sizeof(resp->content_type),
                                resp->charset, sizeof(resp->charset));
    }

    // Connection
    const char *conn = find_header(headers, header_end, "connection:");
    if (conn) {
        conn += 11;
        while (*conn == ' ') conn++;
        if (strcasecmp_n(conn, "close", 5) == 0) {
            resp->keep_alive = false;
        } else if (strcasecmp_n(conn, "keep-alive", 10) == 0) {
            resp->keep_alive = true;
        }
    }

    // Location (for redirects)
    const char *loc = find_header(headers, header_end, "location:");
    if (loc) {
        loc += 9;
        while (*loc == ' ') loc++;
        int loc_len = 0;
        while (loc[loc_len] && loc[loc_len] != '\r' && loc[loc_len] != '\n' &&
               loc_len < (int)sizeof(resp->location) - 1) {
            resp->location[loc_len] = loc[loc_len];
            loc_len++;
        }
        resp->location[loc_len] = '\0';
    }

    // Server
    const char *srv = find_header(headers, header_end, "server:");
    if (srv) {
        srv += 7;
        while (*srv == ' ') srv++;
        int srv_len = 0;
        while (srv[srv_len] && srv[srv_len] != '\r' && srv[srv_len] != '\n' &&
               srv_len < (int)sizeof(resp->server) - 1) {
            resp->server[srv_len] = srv[srv_len];
            srv_len++;
        }
        resp->server[srv_len] = '\0';
    }

    // ETag
    const char *etag = find_header(headers, header_end, "etag:");
    if (etag) {
        etag += 5;
        while (*etag == ' ') etag++;
        int etag_len = 0;
        while (etag[etag_len] && etag[etag_len] != '\r' && etag[etag_len] != '\n' &&
               etag_len < (int)sizeof(resp->etag) - 1) {
            resp->etag[etag_len] = etag[etag_len];
            etag_len++;
        }
        resp->etag[etag_len] = '\0';
    }

    // Last-Modified
    const char *lm = find_header(headers, header_end, "last-modified:");
    if (lm) {
        lm += 14;
        while (*lm == ' ') lm++;
        int lm_len = 0;
        while (lm[lm_len] && lm[lm_len] != '\r' && lm[lm_len] != '\n' &&
               lm_len < (int)sizeof(resp->last_modified) - 1) {
            resp->last_modified[lm_len] = lm[lm_len];
            lm_len++;
        }
        resp->last_modified[lm_len] = '\0';
    }

    return header_end;
}

// ============================================================================
// Chunked Transfer Encoding
// ============================================================================

// Decode chunked transfer encoding in-place.
//
// #fix-ssrf-contentlength: rewritten to be strictly bounds-checked against
// `len` and to distinguish "ran out of received data" from "saw a genuine
// terminating 0-size chunk". The old version parsed the chunk-size digits
// with no bound against `len` (able to read past the received bytes into
// whatever uninitialized memory followed in the caller's buffer) and, worse,
// treated "no hex digits found" the same as an intentional 0 -- so a buffer
// that simply ran out mid-stream (a stalled/truncated peer) fell out of the
// loop and returned the partial `total_decoded` as a COMPLETE, successful
// decode. Now any place data is insufficient returns -1 (the caller already
// treats a negative return as an error, not success).
// Verbatim reference; used when -DRUST_HTTP_PARSE is off, mirrored by the
// boot-time differential (hdc_ref) in net/https.c.
static ssize_t __attribute__((unused)) http_decode_chunked_c(uint8_t *buf, size_t len) {
    uint8_t *read_ptr = buf;
    uint8_t *write_ptr = buf;
    uint8_t *end = buf + len;
    size_t total_decoded = 0;

    while (read_ptr < end) {
        // Parse the chunk-size hex digits, never reading past `end`.
        size_t chunk_size = 0;
        int any_digit = 0;
        uint8_t *p = read_ptr;
        while (p < end) {
            uint8_t c = *p;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            chunk_size = chunk_size * 16 + (size_t)d;
            any_digit = 1;
            p++;
        }
        if (!any_digit) return -1;   // no size digits: incomplete or malformed

        // Skip any chunk-extension to the CRLF ending the size line.
        while (p < end && *p != '\n') p++;
        if (p >= end) return -1;      // size line not fully received yet
        p++;                           // skip the LF
        read_ptr = p;

        if (chunk_size == 0) {
            return (ssize_t)total_decoded;   // genuine terminating chunk (trailers ignored)
        }

        if ((size_t)(end - read_ptr) < chunk_size) {
            return -1;   // chunk payload not fully received
        }

        if (write_ptr != read_ptr) {
            memmove(write_ptr, read_ptr, chunk_size);
        }
        write_ptr += chunk_size;
        read_ptr += chunk_size;
        total_decoded += chunk_size;

        // Trailing CRLF after the chunk data.
        if (read_ptr < end && *read_ptr == '\r') read_ptr++;
        if (read_ptr < end && *read_ptr == '\n') read_ptr++;
    }

    return -1;   // ran out of data before the terminating 0-size chunk
}

// Live dispatcher (also called by net/https.c https_post via extern).
ssize_t http_decode_chunked(uint8_t *buf, size_t len) {
#ifdef RUST_HTTP_PARSE
    return (ssize_t)http_decode_chunked_rs(buf, (unsigned long)len);
#else
    return http_decode_chunked_c(buf, len);
#endif
}

// #fix-ssrf-contentlength: determine whether a chunked body currently held in
// buf[0..len) is FULLY framed, by walking real chunk framing (hex size +
// CRLF + payload + CRLF, repeated, ending at a 0-size chunk + its
// trailer-end blank line) -- not by scanning for the byte sequence "0\r\n"
// anywhere in the buffer, which false-triggers whenever a legitimate
// binary/text body happens to contain that sequence mid-payload and
// truncates it. Returns:
//   1  the terminating chunk (and its trailer-end blank line) have arrived
//   0  need more data (an incomplete chunk/line at the tail; keep receiving)
//  -1  malformed framing (not a recoverable "need more data" case)
static int wget_chunked_is_complete(const uint8_t *buf, size_t len) {
    size_t p = 0;
    while (1) {
        size_t sz = 0; int any = 0;
        while (p < len) {
            uint8_t c = buf[p]; int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            sz = sz * 16u + (size_t)d; any = 1; p++;
        }
        if (!any) return (p >= len) ? 0 : -1;    // no size digits: need more, or garbage
        while (p < len && buf[p] != '\n') p++;    // chunk-ext + CRLF ending the size line
        if (p >= len) return 0;                    // size line not finished yet
        p++;                                        // past the LF

        if (sz == 0) {
            // Trailer section: zero or more header lines, ended by a blank line.
            while (1) {
                if (p >= len) return 0;
                if (buf[p] == '\n') return 1;                          // bare-LF blank line
                if (buf[p] == '\r') {
                    if (p + 1 >= len) return 0;
                    if (buf[p + 1] == '\n') return 1;                  // CRLF blank line
                }
                while (p < len && buf[p] != '\n') p++;                 // skip a trailer line
                if (p >= len) return 0;
                p++;
            }
        }

        if (p + sz > len) return 0;               // chunk payload not fully arrived
        p += sz;
        if (p >= len) return 0;                    // need the trailing CRLF after the data
        if (buf[p] == '\r') p++;
        if (p >= len) return 0;
        if (buf[p] == '\n') p++; else return -1;
    }
}

// ============================================================================
// HTTP Request Building
// ============================================================================

// Initialize request structure with defaults
void http_request_init(http_request_t *req) {
    strcpy(req->method, "GET");
    req->path[0] = '/';
    req->path[1] = '\0';
    req->host[0] = '\0';
    req->port = 80;
    req->headers[0] = '\0';
    req->body = NULL;
    req->body_length = 0;
    req->keep_alive = true;
    req->timeout_ms = 0;
}

// Add header to request
int http_add_header(http_request_t *req, const char *name, const char *value) {
    size_t current_len = strlen(req->headers);
    size_t needed = strlen(name) + strlen(value) + 4;  // ": " + "\r\n"

    if (current_len + needed >= sizeof(req->headers)) {
        return -1;  // Buffer full
    }

    snprintf_simple(req->headers + current_len, sizeof(req->headers) - current_len,
                    "%s: %s\r\n", name, value);
    return 0;
}

// Build HTTP request string
static int build_http_request(http_request_t *req, char *buf, size_t buf_size) {
    int len = 0;

    // Request line
    len += snprintf_simple(buf + len, buf_size - len, "%s %s HTTP/1.1\r\n",
                           req->method, req->path);

    // Host header (required for HTTP/1.1)
    if (req->port == 80 || req->port == 443) {
        len += snprintf_simple(buf + len, buf_size - len, "Host: %s\r\n", req->host);
    } else {
        len += snprintf_simple(buf + len, buf_size - len, "Host: %s:%d\r\n", req->host, req->port);
    }

    // User-Agent
    len += snprintf_simple(buf + len, buf_size - len, "User-Agent: MayteraOS-HTTP/1.1\r\n");

    // Accept
    len += snprintf_simple(buf + len, buf_size - len, "Accept: */*\r\n");

    // Connection
    if (req->keep_alive) {
        len += snprintf_simple(buf + len, buf_size - len, "Connection: keep-alive\r\n");
    } else {
        len += snprintf_simple(buf + len, buf_size - len, "Connection: close\r\n");
    }

    // Content-Length for POST/PUT
    if (req->body && req->body_length > 0) {
        len += snprintf_simple(buf + len, buf_size - len, "Content-Length: %zu\r\n", req->body_length);
    }

    // Additional headers
    if (req->headers[0]) {
        len += snprintf_simple(buf + len, buf_size - len, "%s", req->headers);
    }

    // End of headers
    len += snprintf_simple(buf + len, buf_size - len, "\r\n");

    return len;
}

// ============================================================================
// HTTP Request Execution
// ============================================================================

// Internal: send data with retries
static int send_all(int sock, const void *data, size_t len) {
    const uint8_t *ptr = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < len) {
        int s = tcp_send(sock, ptr + sent, len - sent);
        if (s < 0) {
            if (s == TCP_ERR_WOULD_BLOCK) {
                net_poll();
                tcp_timer();
                continue;
            }
            return s;
        }
        sent += s;
        net_poll();
        tcp_timer();
    }

    return (int)sent;
}

// Internal: receive with timeout
//
// #420: two independent bounds, both real wall-clock (ticks * actual g_timer_hz,
// not the old bogus 18.2Hz-PC-timer assumption that made the "idle" gap below
// only ~1.6s in practice at the kernel's real 250Hz tick rate):
//   - idle_timeout_ticks: how long we'll wait with NO bytes arriving at all
//     before giving up on a genuinely stalled/dead connection. Reset every
//     time data arrives, so a large response that keeps trickling in (chunked
//     HA/API bodies under CPU load, slow LAN peers, etc.) is never punished
//     just for taking a while overall.
//   - overall_deadline: an absolute timer_ticks value bounding the WHOLE
//     fetch (headers+body) even if data keeps trickling in just under the
//     idle gap forever. 0 = no overall cap (caller doesn't want one).
static int recv_with_timeout(int sock, uint8_t *buf, size_t max_len, size_t *received,
                             uint64_t idle_timeout_ticks, uint64_t overall_deadline) {
    uint64_t start_time = timer_ticks;
    *received = 0;

    while (*received < max_len) {
        net_poll();
        tcp_timer();

        int r = tcp_recv(sock, buf + *received, max_len - *received);
        if (r > 0) {
            *received += r;
            start_time = timer_ticks;  // Reset idle-gap timer on data
        } else if (r == TCP_ERR_CLOSED) {
            return 0;  // Connection closed normally
        } else if (r < 0 && r != TCP_ERR_WOULD_BLOCK) {
            return r;  // Error
        } else {
            proc_sleep(1);  // no data yet: yield instead of busy-spinning
        }

        // Idle-gap timeout: only fires after a real stretch of total silence.
        if (timer_ticks - start_time > idle_timeout_ticks) {
            return WGET_ERR_TIMEOUT;
        }

        // Overall wall-clock deadline (#420): bounds worst case even when data
        // keeps trickling in just under the idle gap, so a live-but-glacial
        // peer can't hold the fetch open forever. Keep whatever we already
        // received rather than discarding a mostly-complete body.
        if (overall_deadline && (int64_t)(timer_ticks - overall_deadline) >= 0) {
            return (*received > 0) ? 0 : WGET_ERR_TIMEOUT;
        }

        // Check connection state
        tcp_state_t state = tcp_get_state(sock);
        if (state == TCP_STATE_CLOSED || state == TCP_STATE_CLOSE_WAIT ||
            state == TCP_STATE_TIME_WAIT) {
            return 0;  // Connection closed
        }

        proc_sleep(2);  // #212 yield during net wait (was busy pause-spin)
    }

    return 0;
}

// Perform HTTP request with full control
int http_request(http_request_t *req, http_response_t *resp,
                 uint8_t *body_buf, size_t body_buf_size, size_t *body_len_out) {
    int result = WGET_ERR_CONNECT_FAILED;
    int sock = -1;
    bool pool_connection = false;
    uint8_t *recv_buf = NULL;

    // Initialize response
    http_response_init(resp);
    if (body_len_out) *body_len_out = 0;

    // Check network
    if (ip_get_address() == 0) {
        kprintf("[HTTP] Error: No network address configured\n");
        return WGET_ERR_NO_NETWORK;
    }

    // Resolve hostname. IP literals (LAN repo at 192.168.x.x) are used directly
    // so HTTP works without DNS (apt-style local repos).
    uint32_t ip = 0;
    int is_ip = (req->host[0] != 0);
    for (const char *q = req->host; *q; q++)
        if (!((*q >= '0' && *q <= '9') || *q == '.')) { is_ip = 0; break; }
    if (is_ip) {
        ip = wget_parse_ip(req->host);
    } else {
        int dns_result = dns_resolve(req->host, &ip);
        if (dns_result != 0) {
            kprintf("[HTTP] Error: Cannot resolve host '%s': error %d\n", req->host, dns_result);
            return WGET_ERR_DNS_FAILED;
        }
    }
    if (ip == 0) { kprintf("[HTTP] host '%s' -> 0\n", req->host); return WGET_ERR_DNS_FAILED; }

    kprintf("[HTTP] %s %s://%s:%d%s\n", req->method,
           (req->port == 443) ? "https" : "http", req->host, req->port, req->path);

    // Get connection from pool
    sock = http_get_connection(req->host, req->port, ip);
    if (sock < 0) {
        return sock;  // Error code
    }
    pool_connection = true;

    // Build request
    static char request_buf[4096];
    int req_len = build_http_request(req, request_buf, sizeof(request_buf));

    kprintf("[HTTP] Sending request (%d bytes)\n", req_len);

    // Send request headers
    int sent = send_all(sock, request_buf, req_len);
    if (sent < 0) {
        kprintf("[HTTP] Send failed: %d\n", sent);
        result = WGET_ERR_SEND_FAILED;
        goto cleanup;
    }

    // Send body if present
    if (req->body && req->body_length > 0) {
        sent = send_all(sock, req->body, req->body_length);
        if (sent < 0) {
            kprintf("[HTTP] Send body failed: %d\n", sent);
            result = WGET_ERR_SEND_FAILED;
            goto cleanup;
        }
    }

    kprintf("[HTTP] Request sent, waiting for response...\n");

    // Allocate receive buffer
    size_t recv_buf_size = WGET_BUFFER_SIZE;
    recv_buf = (uint8_t *)kmalloc(recv_buf_size);
    if (!recv_buf) {
        result = WGET_ERR_NO_MEMORY;
        goto cleanup;
    }

    // Receive response
    size_t recv_len = 0;
    // #420: idle_ticks/overall_deadline are real wall-clock (g_timer_hz-based),
    // fixing a bug where this used to assume the classic 18.2 ticks/sec PC
    // timer (req->timeout_ms * 182 / 10000) while the kernel actually runs the
    // PIT at 250Hz -- so the "idle" gap was really only ~1.6s, aborting large
    // responses (HA /api/states ~991KB, big browser pages) that stall briefly
    // between chunks under load, well before headers/body ever finished.
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t idle_ticks = req->timeout_ms > 0
        ? ((uint64_t)req->timeout_ms * hz / 1000)
        : (uint64_t)hz * 60;                       // ~60 sec idle-gap default: a
                                                     // large REST response (e.g. HA
                                                     // rendering ~2000 entities) can
                                                     // take a long time to even start
                                                     // sending, not just have gaps
                                                     // between chunks, so the idle
                                                     // gap needs to be close to the
                                                     // overall budget, not much
                                                     // shorter than it (#420).
    // ~90 sec total budget: bounds the worst case even when data keeps trickling
    // in just under the idle gap. The b820 bump to 600s was a demo expedient for
    // the single-shot 14.5MB OTA fetch and is reverted (#492): the real large-
    // download fix is chunked in-regime Range requests streamed to disk (each
    // chunk is a small fetch well inside this budget), so no single fetch needs a
    // 10-minute ceiling. The 60s idle-gap above still aborts a stalled peer.
    uint64_t overall_deadline = timer_ticks + (uint64_t)hz * 90;

    int header_end = -1;

    // Receive headers first
    while (header_end < 0) {
        size_t chunk_received = 0;
        int r = recv_with_timeout(sock, recv_buf + recv_len, recv_buf_size - recv_len - 1,
                                  &chunk_received, idle_ticks, overall_deadline);
        if (r < 0) {
            result = r;
            goto cleanup;
        }

        recv_len += chunk_received;
        recv_buf[recv_len] = '\0';

        header_end = find_header_end(recv_buf, recv_len);

        if (chunk_received == 0 && header_end < 0) {
            kprintf("[HTTP] Connection closed before headers complete\n");
            result = WGET_ERR_HTTP_ERROR;
            goto cleanup;
        }
    }

    // Parse headers
    header_end = http_parse_response_headers(recv_buf, recv_len, resp);
    if (header_end < 0) {
        result = WGET_ERR_HTTP_ERROR;
        goto cleanup;
    }

    kprintf("[HTTP] Status: %d %s\n", resp->status_code, resp->status_text);
    if (resp->content_type[0]) {
        kprintf("[HTTP] Content-Type: %s", resp->content_type);
        if (resp->charset[0]) {
            kprintf("; charset=%s", resp->charset);
        }
        kprintf("\n");
    }
    if (resp->content_length != (size_t)-1) {
        kprintf("[HTTP] Content-Length: %zu\n", resp->content_length);
    }
    if (resp->chunked) {
        kprintf("[HTTP] Transfer-Encoding: chunked\n");
    }

    // Calculate body already received
    size_t body_in_buffer = recv_len - header_end;

    // Receive rest of body
    if (resp->chunked) {
        // #fix-ssrf-contentlength: keep receiving until REAL chunk framing
        // confirms the terminating chunk arrived, instead of scanning for
        // the raw bytes "0\r\n" anywhere in the received data -- that
        // substring can legitimately appear inside a chunk's own payload
        // and falsely signalled "done", truncating the body while still
        // reporting success below.
        int cc;
        while ((cc = wget_chunked_is_complete(recv_buf + header_end, recv_len - header_end)) == 0) {
            size_t chunk_received = 0;
            size_t space = recv_buf_size - recv_len - 1;
            if (space == 0) break;  // Buffer full
            int r = recv_with_timeout(sock, recv_buf + recv_len, space,
                                      &chunk_received, idle_ticks, overall_deadline);
            if (chunk_received == 0 || r < 0) {
                break;  // Connection closed or error
            }
            recv_len += chunk_received;
        }
        if (cc != 1) {
            kprintf("[HTTP] Truncated/malformed chunked body (ended before terminating chunk)\n");
            result = WGET_ERR_TRUNCATED;
            goto cleanup;
        }

        // Decode chunked data in place
        ssize_t decoded_len = http_decode_chunked(recv_buf + header_end, recv_len - header_end);
        if (decoded_len < 0) {
            kprintf("[HTTP] Chunked decode error\n");
            result = WGET_ERR_HTTP_ERROR;
            goto cleanup;
        }
        body_in_buffer = (size_t)decoded_len;

    } else if (resp->content_length != (size_t)-1) {
        // Known content length
        while (body_in_buffer < resp->content_length) {
            size_t chunk_received = 0;
            size_t space = recv_buf_size - recv_len - 1;
            if (space == 0) break;  // Buffer full

            int r = recv_with_timeout(sock, recv_buf + recv_len, space, &chunk_received, idle_ticks, overall_deadline);
            if (chunk_received == 0 || r < 0) {
                break;
            }
            recv_len += chunk_received;
            body_in_buffer = recv_len - header_end;
        }

        // #fix-ssrf-contentlength: a short body here means the loop above
        // gave up (peer stall/close, buffer full, or the idle/overall
        // deadline) before the advertised Content-Length was reached.
        // Returning that as WGET_SUCCESS silently corrupted JSON callers
        // (HA /api/states, pip metadata) that only check the status code.
        if (body_in_buffer < resp->content_length) {
            kprintf("[HTTP] Truncated body: got %zu of %zu advertised bytes\n",
                    body_in_buffer, resp->content_length);
            result = WGET_ERR_TRUNCATED;
            goto cleanup;
        }

    } else {
        // Unknown length, read until connection closes
        while (1) {
            size_t chunk_received = 0;
            size_t space = recv_buf_size - recv_len - 1;
            if (space == 0) break;

            int r = recv_with_timeout(sock, recv_buf + recv_len, space, &chunk_received, idle_ticks, overall_deadline);
            if (chunk_received == 0 || r < 0) {
                break;
            }
            recv_len += chunk_received;
            body_in_buffer = recv_len - header_end;
        }
    }

    // Copy body to output buffer
    if (body_buf && body_buf_size > 0) {
        size_t copy_len = (body_in_buffer < body_buf_size) ? body_in_buffer : body_buf_size - 1;
        memcpy(body_buf, recv_buf + header_end, copy_len);
        body_buf[copy_len] = '\0';
        if (body_len_out) *body_len_out = body_in_buffer;
    }

    kprintf("[HTTP] Received %zu bytes body\n", body_in_buffer);
    result = WGET_SUCCESS;

cleanup:
    if (recv_buf) kfree(recv_buf);

    if (pool_connection && sock >= 0) {
        // Return to pool if keep-alive and no error
        bool keep = (result == WGET_SUCCESS && resp->keep_alive && req->keep_alive);
        http_release_connection(sock, keep);
    } else if (sock >= 0) {
        tcp_close(sock);
    }

    return result;
}

// ============================================================================
// Simplified HTTP Functions
// ============================================================================

// Simplified GET request
int http_get(const char *url, http_response_t *resp,
             uint8_t *body_buf, size_t body_buf_size, size_t *body_len_out) {
    http_request_t req;
    http_request_init(&req);

    if (http_parse_url(url, &req) < 0) {
        return WGET_ERR_INVALID_URL;
    }

    strcpy(req.method, "GET");

    return http_request(&req, resp, body_buf, body_buf_size, body_len_out);
}

// Simplified POST request
int http_post(const char *url, const char *content_type,
              const void *body, size_t body_len,
              http_response_t *resp,
              uint8_t *resp_buf, size_t resp_buf_size, size_t *resp_len_out) {
    http_request_t req;
    http_request_init(&req);

    if (http_parse_url(url, &req) < 0) {
        return WGET_ERR_INVALID_URL;
    }

    strcpy(req.method, "POST");
    req.body = (const char *)body;
    req.body_length = body_len;

    if (content_type) {
        http_add_header(&req, "Content-Type", content_type);
    }

    return http_request(&req, resp, resp_buf, resp_buf_size, resp_len_out);
}

// HEAD request
int http_head(const char *url, http_response_t *resp) {
    http_request_t req;
    http_request_init(&req);

    if (http_parse_url(url, &req) < 0) {
        return WGET_ERR_INVALID_URL;
    }

    strcpy(req.method, "HEAD");
    req.keep_alive = false;  // HEAD typically doesn't need keep-alive

    return http_request(&req, resp, NULL, 0, NULL);
}

// ============================================================================
// Legacy wget API (with redirect support)
// ============================================================================

// Internal wget with redirect following. `orig_is_private` reflects the
// ORIGINAL request (fixed at redirect_count==0) -- see wget_redirect_allowed().
static int wget_execute_with_redirects(const char *url, const char *save_path,
                                       int redirect_count, int orig_is_private) {
    if (redirect_count > HTTP_MAX_REDIRECTS) {
        kprintf("[WGET] Error: Too many redirects\n");
        return WGET_ERR_TOO_MANY_REDIRECTS;
    }

    http_request_t req;
    http_request_init(&req);

    if (http_parse_url(url, &req) < 0) {
        kprintf("[WGET] Error: Invalid URL\n");
        return WGET_ERR_INVALID_URL;
    }

    if (redirect_count == 0) {
        orig_is_private = wget_host_is_private(req.host);
    }

    // For legacy wget, use close connection for simplicity
    req.keep_alive = false;

    // Allocate body buffer
    uint8_t *body_buf = (uint8_t *)kmalloc(WGET_BUFFER_SIZE);
    if (!body_buf) {
        return WGET_ERR_NO_MEMORY;
    }

    http_response_t resp;
    size_t body_len = 0;

    int result = http_request(&req, &resp, body_buf, WGET_BUFFER_SIZE - 1, &body_len);

    if (result != WGET_SUCCESS) {
        kfree(body_buf);
        return result;
    }

    // Handle redirects
    if (resp.status_code >= 300 && resp.status_code < 400 && resp.location[0]) {
        kprintf("[WGET] Redirect (%d) to: %s\n", resp.status_code, resp.location);

        // Build absolute URL if needed
        char redirect_url[WGET_MAX_URL];
        if (http_resolve_url(url, resp.location, redirect_url, sizeof(redirect_url)) < 0) {
            kfree(body_buf);
            return WGET_ERR_INVALID_URL;
        }

        if (!wget_redirect_allowed(url, redirect_url, orig_is_private)) {
            kfree(body_buf);
            return WGET_ERR_SSRF_BLOCKED;
        }

        kfree(body_buf);
        return wget_execute_with_redirects(redirect_url, save_path, redirect_count + 1, orig_is_private);
    }

    // Check for error status
    if (resp.status_code >= 400) {
        kprintf("[WGET] HTTP Error: %d %s\n", resp.status_code, resp.status_text);
        kfree(body_buf);
        return WGET_ERR_HTTP_ERROR;
    }

    // Handle response body
    if (save_path && save_path[0]) {
        kprintf("[WGET] Saving to: %s\n", save_path);
        kprintf("[WGET] Note: File save not yet implemented (FAT write pending)\n");
        kprintf("[WGET] Would save %zu bytes to %s\n", body_len, save_path);
    } else {
        // Display content
        kprintf("[WGET] Response body (%zu bytes):\n", body_len);
        kprintf("----------------------------------------\n");

        size_t display_len = (body_len > 2048) ? 2048 : body_len;
        for (size_t i = 0; i < display_len; i++) {
            char c = body_buf[i];
            if (c == '\r') continue;
            if (c >= ' ' || c == '\n' || c == '\t') {
                kputc(c);
            } else {
                kputc('.');
            }
        }
        if (body_len > 2048) {
            kprintf("\n... (truncated, %zu more bytes)\n", body_len - 2048);
        }
        kprintf("\n----------------------------------------\n");
    }

    kprintf("[WGET] Download complete: %zu bytes\n", body_len);
    kfree(body_buf);
    return WGET_SUCCESS;
}

// Execute wget command (blocking) - legacy interface
int wget_execute(const char *url, const char *save_path) {
    LOG_INFO("[WGET] Starting download");
    kprintf("[WGET] Fetching: %s\n", url);
    int result = wget_execute_with_redirects(url, save_path, 0, 0);
    if (result == WGET_SUCCESS) {
        LOG_INFO("[WGET] Download completed successfully");
    } else {
        LOG_ERROR("[WGET] Download failed");
    }
    return result;
}

// Fetch URL and return body - legacy interface. `orig_is_private` reflects
// the ORIGINAL request (fixed at redirect_count==0) -- see
// wget_redirect_allowed(). This used to recurse into the PUBLIC wget_fetch()
// with no redirect-count cap at all (an attacker-controlled redirect loop
// could recurse indefinitely); it now shares the same HTTP_MAX_REDIRECTS cap
// and SSRF gate as wget_execute_with_redirects().
static int wget_fetch_internal(const char *url, uint8_t **body_out, uint32_t *body_len_out,
                               int *status_out, int redirect_count, int orig_is_private) {
    if (body_out) *body_out = NULL;
    if (body_len_out) *body_len_out = 0;
    if (status_out) *status_out = 0;

    if (redirect_count > HTTP_MAX_REDIRECTS) {
        kprintf("[HTTP] Error: Too many redirects\n");
        return WGET_ERR_TOO_MANY_REDIRECTS;
    }

    // #374: hard network-up gate - do not attempt DNS/ARP/TCP when the link is
    // down; return immediately so the caller (and any UI it feeds) never blocks.
    extern int net_is_up(void);
    if (!net_is_up()) return WGET_ERR_NO_NETWORK;

    http_request_t req;
    http_request_init(&req);

    if (http_parse_url(url, &req) < 0) {
        return WGET_ERR_INVALID_URL;
    }

    if (redirect_count == 0) {
        orig_is_private = wget_host_is_private(req.host);
    }

    req.keep_alive = false;

    // Allocate buffer
    uint8_t *buf = (uint8_t *)kmalloc(WGET_BUFFER_SIZE);
    if (!buf) {
        return WGET_ERR_NO_MEMORY;
    }

    http_response_t resp;
    size_t body_len = 0;

    int result = http_request(&req, &resp, buf, WGET_BUFFER_SIZE - 1, &body_len);

    if (status_out) *status_out = resp.status_code;

    if (result != WGET_SUCCESS) {
        kfree(buf);
        return result;
    }

    // Handle redirects
    if (resp.status_code >= 300 && resp.status_code < 400 && resp.location[0]) {
        kfree(buf);

        // Build absolute URL
        char redirect_url[WGET_MAX_URL];
        if (http_resolve_url(url, resp.location, redirect_url, sizeof(redirect_url)) < 0) {
            return WGET_ERR_INVALID_URL;
        }

        if (!wget_redirect_allowed(url, redirect_url, orig_is_private)) {
            return WGET_ERR_SSRF_BLOCKED;
        }

        // #497 fault 4: RE-DISPATCH ON THE TARGET'S SCHEME, not the original's.
        // An http:// request that answers "Location: https://..." (Slashdot, and
        // most of the web's http->https canonical redirects) used to recurse back
        // into THIS plain-HTTP client, which then opened a bare TCP connection to
        // port 443 and wrote a plaintext GET at a TLS listener: the server drops
        // it or replies with a TLS alert, so the fetch failed and NO 443 session
        // was ever established. Hand the https target to the TLS client instead.
        //
        // Ordering matters: this sits AFTER wget_redirect_allowed() so the SSRF /
        // downgrade gate still vets the target before any dispatch. https_get()
        // then re-derives its own origin-privacy from the (already vetted) target,
        // so a later public->private hop inside the TLS client is still refused.
        //
        // Hop budget: https_get() caps its own redirect chain at 6, so the worst
        // case is HTTP_MAX_REDIRECTS(10) here plus 6 there. Both are bounded, so
        // an attacker-controlled redirect loop still terminates.
        if (wget_url_is_https(redirect_url)) {
            extern int https_get(const char *, uint8_t **, uint32_t *, int *);
            kprintf("[HTTP] Redirect http->https, handing to TLS client: %s\n",
                    redirect_url);
            int hrc = https_get(redirect_url, body_out, body_len_out, status_out);
            if (hrc == 0) return WGET_SUCCESS;
            // Map the https client's status onto this client's contract. Callers
            // only test `< 0`, but keep the cause legible rather than aliasing
            // onto an unrelated wget code (the two enums overlap numerically).
            switch (hrc) {
                case HTTPS_ERR_NO_MEMORY:    return WGET_ERR_NO_MEMORY;
                case HTTPS_ERR_DNS:          return WGET_ERR_DNS_FAILED;
                case HTTPS_ERR_CONNECT:      return WGET_ERR_CONNECT_FAILED;
                case HTTPS_ERR_TIMEOUT:      return WGET_ERR_TIMEOUT;
                case HTTPS_ERR_TRUNCATED:    return WGET_ERR_TRUNCATED;
                case HTTPS_ERR_SSRF_BLOCKED: return WGET_ERR_SSRF_BLOCKED;
                default:                     return WGET_ERR_HTTP_ERROR;
            }
        }

        // Recursive call for redirect
        return wget_fetch_internal(redirect_url, body_out, body_len_out, status_out,
                                   redirect_count + 1, orig_is_private);
    }

    if (resp.status_code >= 400) {
        kfree(buf);
        return WGET_ERR_HTTP_ERROR;
    }

    // Return body
    if (body_out && body_len > 0) {
        uint8_t *body_copy = (uint8_t *)kmalloc(body_len + 1);
        if (body_copy) {
            memcpy(body_copy, buf, body_len);
            body_copy[body_len] = '\0';
            *body_out = body_copy;
            if (body_len_out) *body_len_out = (uint32_t)body_len;
        } else {
            kfree(buf);
            return WGET_ERR_NO_MEMORY;
        }
    }

    kfree(buf);
    return WGET_SUCCESS;
}

int wget_fetch(const char *url, uint8_t **body_out, uint32_t *body_len_out, int *status_out) {
    return wget_fetch_internal(url, body_out, body_len_out, status_out, 0, 0);
}

// ============================================================================
// #414 Home Assistant / external services: HTTP GET/POST with caller-supplied
// extra headers (e.g. "Authorization: Bearer <token>\r\n"). Plain HTTP.
// The body is returned even for status >= 400 so callers can surface HA JSON
// errors. Used by the background haservice; never runs on a UI thread.
// ============================================================================
static int wget_request_hdr(const char *method, const char *url,
                            const char *extra_headers, const char *reqbody,
                            uint8_t **body_out, uint32_t *body_len_out, int *status_out) {
    if (body_out) *body_out = NULL;
    if (body_len_out) *body_len_out = 0;
    if (status_out) *status_out = 0;

    extern int net_is_up(void);
    if (!net_is_up()) return WGET_ERR_NO_NETWORK;

    http_request_t req;
    http_request_init(&req);
    if (http_parse_url(url, &req) < 0) return WGET_ERR_INVALID_URL;

    req.keep_alive = false;
    strcpy(req.method, method);

    if (extra_headers && extra_headers[0]) {
        size_t hl = strlen(extra_headers);
        if (hl >= sizeof(req.headers)) hl = sizeof(req.headers) - 1;
        memcpy(req.headers, extra_headers, hl);
        req.headers[hl] = '\0';
    }

    if (reqbody && reqbody[0]) {
        req.body = reqbody;
        req.body_length = strlen(reqbody);
        http_add_header(&req, "Content-Type", "application/json");
    }

    uint8_t *buf = (uint8_t *)kmalloc(WGET_BUFFER_SIZE);
    if (!buf) return WGET_ERR_NO_MEMORY;

    http_response_t resp;
    size_t body_len = 0;
    int result = http_request(&req, &resp, buf, WGET_BUFFER_SIZE - 1, &body_len);
    if (status_out) *status_out = resp.status_code;
    if (result != WGET_SUCCESS) { kfree(buf); return result; }

    if (body_out && body_len > 0) {
        uint8_t *cp = (uint8_t *)kmalloc(body_len + 1);
        if (!cp) { kfree(buf); return WGET_ERR_NO_MEMORY; }
        memcpy(cp, buf, body_len);
        cp[body_len] = '\0';
        *body_out = cp;
        if (body_len_out) *body_len_out = (uint32_t)body_len;
    }
    kfree(buf);
    return WGET_SUCCESS;
}

int wget_fetch_hdr(const char *url, const char *extra_headers,
                   uint8_t **body_out, uint32_t *body_len_out, int *status_out) {
    return wget_request_hdr("GET", url, extra_headers, (const char *)0,
                            body_out, body_len_out, status_out);
}

int wget_post_hdr(const char *url, const char *extra_headers, const char *reqbody,
                  uint8_t **body_out, uint32_t *body_len_out, int *status_out) {
    return wget_request_hdr("POST", url, extra_headers, reqbody,
                            body_out, body_len_out, status_out);
}

// ============================================================================
// Error Handling
// ============================================================================

const char *wget_strerror(int error) {
    switch (error) {
        case WGET_SUCCESS:              return "Success";
        case WGET_ERR_INVALID_URL:      return "Invalid URL";
        case WGET_ERR_NO_NETWORK:       return "No network configured";
        case WGET_ERR_DNS_FAILED:       return "DNS resolution failed";
        case WGET_ERR_CONNECT_FAILED:   return "Connection failed";
        case WGET_ERR_SEND_FAILED:      return "Send failed";
        case WGET_ERR_TIMEOUT:          return "Connection timeout";
        case WGET_ERR_NO_MEMORY:        return "Out of memory";
        case WGET_ERR_HTTP_ERROR:       return "HTTP error";
        case WGET_ERR_FILE_ERROR:       return "File error";
        case WGET_ERR_TOO_MANY_REDIRECTS: return "Too many redirects";
        case WGET_ERR_TRUNCATED:        return "Truncated body (Content-Length/chunked mismatch)";
        case WGET_ERR_SSRF_BLOCKED:     return "Redirect blocked (private/loopback host or scheme downgrade)";
        default:                        return "Unknown error";
    }
}
