// httpx.h - minimal ASYNC HTTP/1.1 client for plain http:// URLs, built on the
// kernel's NON-BLOCKING socket syscalls (tcp_connect -> TCP_ERR_IN_PROGRESS,
// tcp_recv -> 0 when nothing is queued, TCP_ERR_CLOSED on EOF) and the
// poll-split DNS pair (dns_start/dns_poll).
//
// WHY THIS EXISTS. The kernel exposes exactly two async HTTP entry points to
// Ring 3: SYS_HTTP_FETCH_START (GET, url only, no request headers, body capped
// at WGET_BUFFER_SIZE = 1 MB) and SYS_HTTP_POST_START (POST with headers). There
// is no userland TLS socket, so https:// MUST go through those. For plain
// http:// nothing stops a Ring 3 client from speaking HTTP/1.1 itself over the
// raw TCP syscalls, and doing so buys what the kernel API cannot give:
//   * any method (PUT/PATCH/DELETE/HEAD), request headers, a request body
//   * the RESPONSE headers (the kernel API returns status + body only)
//   * a STREAMING body of any size, delivered to a sink as it arrives, so a
//     download manager can write to disk incrementally instead of holding a
//     1 MB kernel buffer
//   * Range resume
//
// ASYNC BY CONSTRUCTION. Nothing here blocks. httpx_start() only parses and
// builds the request; every network step happens inside httpx_poll(), which
// does a bounded amount of non-blocking work and returns. Drive it from the
// app's win_get_event(...,50) loop exactly like http_fetch_poll(). This is the
// #420/#549 rule: the UI thread never waits on a remote peer.
//
// ONE DNS SLOT. dns_start/dns_poll carry one in-flight lookup per process
// (dns_poll has no handle), so concurrent httpx jobs serialise on it here.
#ifndef NETAPPS_HTTPX_H
#define NETAPPS_HTTPX_H

#define HTTPX_RUNNING 0
#define HTTPX_DONE    1
#define HTTPX_ERROR   2

#define HTTPX_URL_MAX    1024
#define HTTPX_HOST_MAX   256
#define HTTPX_METHOD_MAX 12
#define HTTPX_EXTRA_MAX  2048     // caller-supplied request header lines
#define HTTPX_BODY_MAX   8192     // request body kept for redirect re-issue
#define HTTPX_REQ_MAX    (12 * 1024)
#define HTTPX_HDR_CAP    8192     // response header block capture
#define HTTPX_RX_CHUNK   (16 * 1024)
#define HTTPX_MAX_REDIRECTS 5

enum {
    HTTPX_PH_IDLE = 0,
    HTTPX_PH_DNS_WAIT,   // waiting for the process's single DNS slot
    HTTPX_PH_DNS,        // lookup in flight
    HTTPX_PH_CONNECT,    // SYN sent, polling tcp_get_state()
    HTTPX_PH_SEND,       // request bytes going out (window permitting)
    HTTPX_PH_HEADERS,    // status line + headers arriving
    HTTPX_PH_BODY,       // body arriving (identity or chunked)
    HTTPX_PH_DONE,
    HTTPX_PH_ERROR
};

// Body sink: called with each body fragment as it arrives, in order. May be
// NULL (body is counted but discarded). Response headers are already parsed
// when the first fragment is delivered, so h->status is valid inside the sink.
typedef void (*httpx_sink_fn)(void *ctx, const unsigned char *data, unsigned len);

typedef struct httpx {
    // request
    char url[HTTPX_URL_MAX];
    char method[HTTPX_METHOD_MAX];
    char host[HTTPX_HOST_MAX];
    char path[HTTPX_URL_MAX];
    int  port;
    unsigned int ip;
    char extra[HTTPX_EXTRA_MAX];
    unsigned char body[HTTPX_BODY_MAX];
    unsigned body_len;
    unsigned long range_from;
    int  redirects;
    unsigned char req[HTTPX_REQ_MAX];
    unsigned req_len, req_sent;

    // progress
    int  phase;
    int  result;
    int  sock;
    unsigned long long t_start_us, t_phase_us, t_last_data_us, t_first_byte_us, t_done_us;

    // response
    int  status;
    long content_len;          // -1 = unknown (read to EOF)
    unsigned long bytes_body;  // body bytes delivered so far (this response)
    int  chunked;
    int  chunk_state;
    unsigned long chunk_remain;
    char chunk_line[32];
    int  chunk_line_len;
    char hdr[HTTPX_HDR_CAP + 1];
    unsigned hdr_len;
    int  hdr_done;
    char location[HTTPX_URL_MAX];
    char err[160];

    httpx_sink_fn sink;
    void *sink_ctx;
    unsigned char rx[HTTPX_RX_CHUNK];
} httpx_t;

// Start a request. method: "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD".
// extra_headers: zero or more header lines separated by CRLF or LF (may be
// NULL). body/body_len: request body (copied; <= HTTPX_BODY_MAX). range_from:
// > 0 adds "Range: bytes=N-". Returns 0 and leaves the job RUNNING, or -1 with
// h->err set (bad URL, not http://, body too large).
int  httpx_start(httpx_t *h, const char *method, const char *url,
                 const char *extra_headers, const void *body, unsigned body_len,
                 unsigned long range_from, httpx_sink_fn sink, void *sink_ctx);

// Advance the job by a bounded amount of non-blocking work. Returns
// HTTPX_RUNNING / HTTPX_DONE / HTTPX_ERROR (h->err names the error).
int  httpx_poll(httpx_t *h);

// Abort a running job (closes the socket, releases the DNS slot). Sets
// result = HTTPX_ERROR with err "cancelled". Safe on an idle job.
void httpx_cancel(httpx_t *h);

// Human-readable phase for status bars ("Resolving host", ...).
const char *httpx_phase_name(const httpx_t *h);

// Reason phrase for common status codes ("OK", "Not Found", ...), "" otherwise.
const char *httpx_status_text(int status);

// Parse an http:// URL into host, port and path (path always begins with '/').
// Returns 0, or -1 when the URL is not http://.
int  httpx_parse_url(const char *url, char *host, int hostcap, int *port,
                     char *path, int pathcap);

// Look up a response header (case-insensitive) in the captured header block.
// Returns the value length, or -1 if absent.
int  httpx_header_get(const httpx_t *h, const char *name, char *out, int cap);

// Elapsed microseconds since httpx_start() (to done time once finished).
unsigned long long httpx_elapsed_us(const httpx_t *h);

#endif // NETAPPS_HTTPX_H
