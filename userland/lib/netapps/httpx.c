// httpx.c - async HTTP/1.1 client for http:// over the non-blocking TCP and
// poll-split DNS syscalls. See httpx.h for the why. Freestanding C; the only
// libc used is string/stdio formatting and the syscall wrappers.
#include "../../libc/maytera.h"
#include "httpx.h"

// Kernel socket return codes (kernel/net/tcp.h; the userland header names the
// states but not every error value, so the two we branch on are spelled here).
#define HX_TCP_ERR_CLOSED       (-6)
#define HX_TCP_ERR_IN_PROGRESS  (-8)
#define HX_TCP_ERR_WOULD_BLOCK  (-9)

// Deadlines. These are the "wake source outside our control" case (a remote
// peer), so a timeout is the correct semantics, not a workaround.
#define HX_DNS_SLOT_WAIT_US   20000000ULL
#define HX_DNS_US             10000000ULL
#define HX_CONNECT_US         15000000ULL
#define HX_SEND_US            30000000ULL
#define HX_IDLE_US            30000000ULL

#define HX_SEND_SEG   1400
#define HX_RECV_ROUNDS_PER_POLL 32
#define HX_SEND_ROUNDS_PER_POLL 8

enum { CS_SIZE = 0, CS_DATA, CS_CRLF };

static httpx_t *g_dns_owner;     // the ONE in-flight dns_start/dns_poll lookup

static unsigned long long hx_now(void) { return mono_us(); }

static int hx_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int hx_ci_prefix(const char *s, const char *pfx) {
    while (*pfx) {
        if (!*s || hx_lower(*s) != hx_lower(*pfx)) return 0;
        s++; pfx++;
    }
    return 1;
}

static int hx_ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (hx_lower(*a) != hx_lower(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void hx_release_dns(httpx_t *h) {
    if (g_dns_owner == h) g_dns_owner = 0;
}

static void hx_close(httpx_t *h) {
    if (h->sock >= 0) { tcp_close(h->sock); h->sock = -1; }
    hx_release_dns(h);
}

static void hx_fail(httpx_t *h, const char *msg) {
    snprintf(h->err, sizeof(h->err), "%s", msg);
    hx_close(h);
    h->t_done_us = hx_now();
    h->phase = HTTPX_PH_ERROR;
    h->result = HTTPX_ERROR;
}

static void hx_finish(httpx_t *h) {
    hx_close(h);
    h->t_done_us = hx_now();
    h->phase = HTTPX_PH_DONE;
    h->result = HTTPX_DONE;
}

// "a.b.c.d" -> (a<<24)|(b<<16)|(c<<8)|d, the form tcp_connect() takes.
static int hx_parse_ipv4(const char *s, unsigned int *out) {
    unsigned int v = 0;
    int part = 0;
    while (part < 4) {
        if (*s < '0' || *s > '9') return 0;
        int n = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) { n = n * 10 + (*s - '0'); s++; digits++; }
        if (n > 255) return 0;
        v = (v << 8) | (unsigned int)n;
        part++;
        if (part < 4) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    *out = v;
    return 1;
}

int httpx_parse_url(const char *url, char *host, int hostcap, int *port,
                    char *path, int pathcap) {
    if (!url || !hx_ci_prefix(url, "http://")) return -1;
    const char *p = url + 7;
    const char *e = p;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    // host[:port]
    const char *colon = 0;
    for (const char *q = p; q < e; q++) if (*q == ':') colon = q;
    int hl = (int)((colon ? colon : e) - p);
    if (hl <= 0 || hl >= hostcap) return -1;
    memcpy(host, p, (unsigned)hl);
    host[hl] = 0;
    *port = 80;
    if (colon) {
        int pv = 0;
        for (const char *q = colon + 1; q < e; q++) {
            if (*q < '0' || *q > '9') return -1;
            pv = pv * 10 + (*q - '0');
            if (pv > 65535) return -1;
        }
        if (pv == 0) return -1;
        *port = pv;
    }
    // path[?query] (fragment dropped)
    int n = 0;
    if (*e == 0 || *e == '#') {
        path[n++] = '/';
    } else {
        if (*e == '?') path[n++] = '/';
        while (*e && *e != '#' && n < pathcap - 1) path[n++] = *e++;
    }
    path[n] = 0;
    return 0;
}

// Does the caller's extra header block already carry this header name?
static int hx_extra_has(const char *extra, const char *name) {
    const char *p = extra;
    while (*p) {
        while (*p == '\r' || *p == '\n') p++;
        if (!*p) break;
        if (hx_ci_prefix(p, name) && p[strlen(name)] == ':') return 1;
        while (*p && *p != '\n') p++;
    }
    return 0;
}

static int hx_has_body_method(const char *m) {
    return strcmp(m, "POST") == 0 || strcmp(m, "PUT") == 0 || strcmp(m, "PATCH") == 0;
}

static int hx_build_request(httpx_t *h) {
    char *o = (char *)h->req;
    unsigned cap = HTTPX_REQ_MAX;
    unsigned n = 0;
    int w;
#define HX_APPEND(...) do { \
        w = snprintf(o + n, cap - n, __VA_ARGS__); \
        if (w < 0 || (unsigned)w >= cap - n) return -1; \
        n += (unsigned)w; } while (0)

    HX_APPEND("%s %s HTTP/1.1\r\n", h->method, h->path);
    if (!hx_extra_has(h->extra, "Host")) {
        if (h->port == 80) HX_APPEND("Host: %s\r\n", h->host);
        else HX_APPEND("Host: %s:%d\r\n", h->host, h->port);
    }
    if (!hx_extra_has(h->extra, "User-Agent")) HX_APPEND("User-Agent: MayteraOS-httpx/1.0\r\n");
    if (!hx_extra_has(h->extra, "Accept"))     HX_APPEND("Accept: */*\r\n");
    HX_APPEND("Connection: close\r\n");
    if (h->range_from > 0) HX_APPEND("Range: bytes=%lu-\r\n", h->range_from);
    if ((h->body_len > 0 || hx_has_body_method(h->method)) &&
        !hx_extra_has(h->extra, "Content-Length"))
        HX_APPEND("Content-Length: %u\r\n", h->body_len);
    // Caller lines, each normalised to end in CRLF; blank lines dropped so a
    // stray empty line cannot terminate the header block early.
    {
        const char *p = h->extra;
        while (*p) {
            const char *s = p;
            while (*p && *p != '\n') p++;
            const char *e = p;
            if (*p == '\n') p++;
            while (e > s && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
            while (s < e && (*s == ' ' || *s == '\t')) s++;
            if (e > s) {
                unsigned l = (unsigned)(e - s);
                if (n + l + 2 >= cap) return -1;
                memcpy(o + n, s, l); n += l;
                o[n++] = '\r'; o[n++] = '\n';
            }
        }
    }
    HX_APPEND("\r\n");
#undef HX_APPEND
    if (h->body_len > 0) {
        if (n + h->body_len > cap) return -1;
        memcpy(o + n, h->body, h->body_len);
        n += h->body_len;
    }
    h->req_len = n;
    h->req_sent = 0;
    return 0;
}

static void hx_reset_response(httpx_t *h) {
    h->status = 0;
    h->content_len = -1;
    h->bytes_body = 0;
    h->chunked = 0;
    h->chunk_state = CS_SIZE;
    h->chunk_remain = 0;
    h->chunk_line_len = 0;
    h->hdr_len = 0;
    h->hdr[0] = 0;
    h->hdr_done = 0;
    h->location[0] = 0;
}

int httpx_start(httpx_t *h, const char *method, const char *url,
                const char *extra_headers, const void *body, unsigned body_len,
                unsigned long range_from, httpx_sink_fn sink, void *sink_ctx) {
    if (h->result == HTTPX_RUNNING && h->phase != HTTPX_PH_IDLE) httpx_cancel(h);
    memset(h, 0, sizeof(*h));
    h->sock = -1;
    h->content_len = -1;
    h->result = HTTPX_ERROR;     // until we are actually started
    h->phase = HTTPX_PH_ERROR;
    h->sink = sink;
    h->sink_ctx = sink_ctx;

    if (!url || !url[0]) { snprintf(h->err, sizeof(h->err), "empty URL"); return -1; }
    if (hx_ci_prefix(url, "https://")) {
        snprintf(h->err, sizeof(h->err), "https:// needs the kernel TLS fetch (no userland TLS socket)");
        return -1;
    }
    if (httpx_parse_url(url, h->host, sizeof(h->host), &h->port, h->path, sizeof(h->path)) != 0) {
        snprintf(h->err, sizeof(h->err), "not a valid http:// URL");
        return -1;
    }
    snprintf(h->url, sizeof(h->url), "%s", url);
    if (!method || !method[0]) method = "GET";
    {
        int i = 0;
        while (method[i] && i < HTTPX_METHOD_MAX - 1) { h->method[i] = (char)((method[i] >= 'a' && method[i] <= 'z') ? method[i] - 32 : method[i]); i++; }
        h->method[i] = 0;
    }
    if (extra_headers) snprintf(h->extra, sizeof(h->extra), "%s", extra_headers);
    if (body_len > HTTPX_BODY_MAX) { snprintf(h->err, sizeof(h->err), "request body larger than %u bytes", (unsigned)HTTPX_BODY_MAX); return -1; }
    if (body && body_len) { memcpy(h->body, body, body_len); h->body_len = body_len; }
    h->range_from = range_from;
    if (hx_build_request(h) != 0) { snprintf(h->err, sizeof(h->err), "request too large"); return -1; }

    h->t_start_us = h->t_phase_us = h->t_last_data_us = hx_now();
    h->phase = HTTPX_PH_DNS_WAIT;
    h->result = HTTPX_RUNNING;
    return 0;
}

void httpx_cancel(httpx_t *h) {
    hx_close(h);
    if (h->result == HTTPX_RUNNING) {
        snprintf(h->err, sizeof(h->err), "cancelled");
        h->result = HTTPX_ERROR;
        h->phase = HTTPX_PH_ERROR;
        h->t_done_us = hx_now();
    }
}

static void hx_begin_connect(httpx_t *h) {
    hx_release_dns(h);
    h->sock = tcp_socket();
    if (h->sock < 0) { hx_fail(h, "could not create a TCP socket"); return; }
    int r = tcp_connect(h->sock, h->ip, h->port);
    if (r < 0 && r != HX_TCP_ERR_IN_PROGRESS) {
        char m[96];
        snprintf(m, sizeof(m), "connect failed (tcp error %d)", r);
        hx_fail(h, m);
        return;
    }
    h->phase = HTTPX_PH_CONNECT;
    h->t_phase_us = hx_now();
}

static void hx_emit(httpx_t *h, const unsigned char *d, unsigned len) {
    if (!len) return;
    h->bytes_body += len;
    if (h->sink) h->sink(h->sink_ctx, d, len);
}

static void hx_feed_body(httpx_t *h, const unsigned char *d, unsigned len) {
    if (!h->chunked) {
        if (h->content_len >= 0) {
            unsigned long want = (unsigned long)h->content_len - h->bytes_body;
            if ((unsigned long)len > want) len = (unsigned)want;
        }
        hx_emit(h, d, len);
        if (h->content_len >= 0 && h->bytes_body >= (unsigned long)h->content_len) hx_finish(h);
        return;
    }
    // chunked transfer coding
    while (len > 0 && h->result == HTTPX_RUNNING) {
        if (h->chunk_state == CS_SIZE) {
            char c = (char)*d; d++; len--;
            if (c == '\n') {
                h->chunk_line[h->chunk_line_len] = 0;
                unsigned long sz = 0;
                for (int i = 0; i < h->chunk_line_len; i++) {
                    char x = h->chunk_line[i];
                    int v;
                    if (x >= '0' && x <= '9') v = x - '0';
                    else if (x >= 'a' && x <= 'f') v = x - 'a' + 10;
                    else if (x >= 'A' && x <= 'F') v = x - 'A' + 10;
                    else break;                       // ';ext' or CR
                    sz = (sz << 4) | (unsigned long)v;
                }
                h->chunk_line_len = 0;
                if (sz == 0) { hx_finish(h); return; }  // terminal chunk; trailers ignored
                h->chunk_remain = sz;
                h->chunk_state = CS_DATA;
            } else if (h->chunk_line_len < (int)sizeof(h->chunk_line) - 1) {
                h->chunk_line[h->chunk_line_len++] = c;
            }
        } else if (h->chunk_state == CS_DATA) {
            unsigned take = len;
            if ((unsigned long)take > h->chunk_remain) take = (unsigned)h->chunk_remain;
            hx_emit(h, d, take);
            d += take; len -= take; h->chunk_remain -= take;
            if (h->chunk_remain == 0) h->chunk_state = CS_CRLF;
        } else { // CS_CRLF: swallow through the newline after the chunk data
            char c = (char)*d; d++; len--;
            if (c == '\n') h->chunk_state = CS_SIZE;
        }
    }
}

static void hx_redirect(httpx_t *h) {
    char nurl[HTTPX_URL_MAX];
    const char *loc = h->location;
    if (hx_ci_prefix(loc, "https://")) {
        hx_fail(h, "redirected to an https:// URL; this client has no TLS (use the kernel fetch path)");
        return;
    }
    if (hx_ci_prefix(loc, "http://")) {
        snprintf(nurl, sizeof(nurl), "%s", loc);
    } else if (loc[0] == '/') {
        if (h->port == 80) snprintf(nurl, sizeof(nurl), "http://%s%s", h->host, loc);
        else snprintf(nurl, sizeof(nurl), "http://%s:%d%s", h->host, h->port, loc);
    } else {
        // relative to the directory of the current path
        char base[HTTPX_URL_MAX];
        snprintf(base, sizeof(base), "%s", h->path);
        char *q = base; char *slash = base;
        for (; *q; q++) { if (*q == '?') { *q = 0; break; } if (*q == '/') slash = q; }
        slash[1] = 0;
        if (h->port == 80) snprintf(nurl, sizeof(nurl), "http://%s%s%s", h->host, base, loc);
        else snprintf(nurl, sizeof(nurl), "http://%s:%d%s%s", h->host, h->port, base, loc);
    }
    int st = h->status;
    hx_close(h);
    h->redirects++;
    if (st == 303 || ((st == 301 || st == 302) && strcmp(h->method, "GET") != 0 && strcmp(h->method, "HEAD") != 0)) {
        snprintf(h->method, sizeof(h->method), "GET");
        h->body_len = 0;
    }
    if (httpx_parse_url(nurl, h->host, sizeof(h->host), &h->port, h->path, sizeof(h->path)) != 0) {
        hx_fail(h, "redirect Location is not a usable http:// URL");
        return;
    }
    snprintf(h->url, sizeof(h->url), "%s", nurl);
    if (hx_build_request(h) != 0) { hx_fail(h, "redirected request too large"); return; }
    hx_reset_response(h);
    h->phase = HTTPX_PH_DNS_WAIT;
    h->t_phase_us = h->t_last_data_us = hx_now();
}

// Returns 1 when a 100 Continue block was consumed (keep reading headers).
static int hx_parse_headers(httpx_t *h) {
    char *p = h->hdr;
    if (!hx_ci_prefix(p, "HTTP/")) { hx_fail(h, "not an HTTP response"); return 0; }
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    h->status = atoi(p);
    if (h->status < 100 || h->status > 999) { hx_fail(h, "malformed HTTP status line"); return 0; }
    if (h->status == 100) return 1;
    // header lines
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        char *end = p;
        if (*p == '\n') p++;
        while (end > line && (end[-1] == '\r' || end[-1] == ' ')) end--;
        if (end == line) break;
        char *colon = line;
        while (colon < end && *colon != ':') colon++;
        if (colon >= end) continue;
        char name[48];
        int nl = (int)(colon - line);
        if (nl >= (int)sizeof(name)) nl = (int)sizeof(name) - 1;
        memcpy(name, line, (unsigned)nl); name[nl] = 0;
        char *v = colon + 1;
        while (v < end && (*v == ' ' || *v == '\t')) v++;
        char saved = *end; *end = 0;
        if (hx_ci_eq(name, "Content-Length")) {
            long cl = 0; const char *q = v;
            while (*q >= '0' && *q <= '9') { cl = cl * 10 + (*q - '0'); q++; }
            h->content_len = cl;
        } else if (hx_ci_eq(name, "Transfer-Encoding")) {
            for (const char *q = v; *q; q++) if (hx_ci_prefix(q, "chunked")) { h->chunked = 1; break; }
        } else if (hx_ci_eq(name, "Location")) {
            snprintf(h->location, sizeof(h->location), "%s", v);
        }
        *end = saved;
    }
    h->t_first_byte_us = hx_now();

    int st = h->status;
    if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && h->location[0]) {
        if (h->redirects >= HTTPX_MAX_REDIRECTS) { hx_fail(h, "too many redirects"); return 0; }
        hx_redirect(h);
        return 0;
    }
    if (strcmp(h->method, "HEAD") == 0 || st == 204 || st == 304 || (st >= 100 && st < 200)) {
        hx_finish(h);
        return 0;
    }
    if (h->chunked) h->content_len = -1;      // chunked wins over any length
    h->phase = HTTPX_PH_BODY;
    h->hdr_done = 1;
    if (!h->chunked && h->content_len == 0) hx_finish(h);
    return 0;
}

static void hx_feed(httpx_t *h, const unsigned char *d, unsigned len) {
    while (len > 0 && h->result == HTTPX_RUNNING) {
        if (h->hdr_done) { hx_feed_body(h, d, len); return; }
        unsigned room = HTTPX_HDR_CAP - h->hdr_len;
        unsigned take = len < room ? len : room;
        memcpy(h->hdr + h->hdr_len, d, take);
        h->hdr_len += take;
        h->hdr[h->hdr_len] = 0;
        // terminator: CRLFCRLF (or bare LFLF from sloppy servers)
        unsigned term = 0, tl = 0;
        for (unsigned i = 0; i + 1 < h->hdr_len; i++) {
            if (h->hdr[i] == '\n' && h->hdr[i + 1] == '\n') { term = i; tl = 2; break; }
            if (i + 3 < h->hdr_len && h->hdr[i] == '\r' && h->hdr[i + 1] == '\n' &&
                h->hdr[i + 2] == '\r' && h->hdr[i + 3] == '\n') { term = i; tl = 4; break; }
        }
        if (!tl) {
            if (h->hdr_len >= HTTPX_HDR_CAP) hx_fail(h, "response headers larger than 8 KB");
            return;
        }
        unsigned hdr_bytes = term + tl;
        unsigned extra_copied = h->hdr_len - hdr_bytes;   // body bytes that slipped into hdr
        unsigned consumed = take - extra_copied;           // bytes of d that were header
        h->hdr[hdr_bytes] = 0;
        h->hdr_len = hdr_bytes;
        int cont = hx_parse_headers(h);
        d += consumed; len -= consumed;
        if (cont) { h->hdr_len = 0; h->hdr[0] = 0; continue; }   // 100 Continue: next block
        if (h->result != HTTPX_RUNNING || h->phase != HTTPX_PH_BODY) return;
        // loop continues with hdr_done = 1 and feeds the leftover as body
    }
}

static void hx_on_eof(httpx_t *h) {
    if (h->phase == HTTPX_PH_HEADERS) { hx_fail(h, "server closed the connection before sending headers"); return; }
    if (h->chunked) { hx_fail(h, "connection closed inside a chunked body"); return; }
    if (h->content_len < 0 || h->bytes_body >= (unsigned long)h->content_len) { hx_finish(h); return; }
    char m[120];
    snprintf(m, sizeof(m), "connection closed early (%lu of %ld bytes)", h->bytes_body, h->content_len);
    hx_fail(h, m);
}

static void hx_do_send(httpx_t *h, unsigned long long t) {
    for (int i = 0; i < HX_SEND_ROUNDS_PER_POLL; i++) {
        unsigned rem = h->req_len - h->req_sent;
        if (rem == 0) {
            h->phase = HTTPX_PH_HEADERS;
            h->t_phase_us = h->t_last_data_us = t;
            return;
        }
        unsigned seg = rem < HX_SEND_SEG ? rem : HX_SEND_SEG;
        int n = tcp_send(h->sock, h->req + h->req_sent, (int)seg);
        if (n > 0) { h->req_sent += (unsigned)n; continue; }
        if (n == 0 || n == HX_TCP_ERR_WOULD_BLOCK) break;   // window full; next tick
        char m[64];
        snprintf(m, sizeof(m), "send failed (tcp error %d)", n);
        hx_fail(h, m);
        return;
    }
    if (t - h->t_phase_us > HX_SEND_US) hx_fail(h, "timed out sending the request");
}

static void hx_do_recv(httpx_t *h, unsigned long long t) {
    for (int i = 0; i < HX_RECV_ROUNDS_PER_POLL && h->result == HTTPX_RUNNING; i++) {
        int n = tcp_recv(h->sock, h->rx, (int)sizeof(h->rx));
        if (n > 0) { h->t_last_data_us = t; hx_feed(h, h->rx, (unsigned)n); continue; }
        if (n == 0) break;                                    // nothing queued yet
        if (n == HX_TCP_ERR_CLOSED) { hx_on_eof(h); return; }
        char m[64];
        snprintf(m, sizeof(m), "receive failed (tcp error %d)", n);
        hx_fail(h, m);
        return;
    }
    if (h->result == HTTPX_RUNNING && t - h->t_last_data_us > HX_IDLE_US)
        hx_fail(h, "timed out waiting for the server");
}

int httpx_poll(httpx_t *h) {
    if (h->result != HTTPX_RUNNING) return h->result;
    unsigned long long t = hx_now();
    switch (h->phase) {
    case HTTPX_PH_DNS_WAIT: {
        if (hx_parse_ipv4(h->host, &h->ip)) { hx_begin_connect(h); break; }
        if (g_dns_owner && g_dns_owner != h) {
            if (t - h->t_phase_us > HX_DNS_SLOT_WAIT_US) hx_fail(h, "DNS slot busy for too long");
            break;
        }
        g_dns_owner = h;
        int r = dns_start(h->host, &h->ip);
        if (r == 1 && h->ip) { hx_begin_connect(h); }
        else if (r == 0) { h->phase = HTTPX_PH_DNS; h->t_phase_us = t; }
        else hx_fail(h, "DNS lookup failed");
        break;
    }
    case HTTPX_PH_DNS: {
        int r = dns_poll(&h->ip);
        if (r == 1 && h->ip) hx_begin_connect(h);
        else if (r < 0 || (r == 1 && !h->ip)) hx_fail(h, "DNS: no such host");
        else if (t - h->t_phase_us > HX_DNS_US) hx_fail(h, "DNS lookup timed out");
        break;
    }
    case HTTPX_PH_CONNECT: {
        int st = tcp_get_state(h->sock);
        if (st == TCP_STATE_ESTABLISHED) {
            h->phase = HTTPX_PH_SEND; h->t_phase_us = t;
            hx_do_send(h, t);
        } else if (st == TCP_STATE_CLOSED || st < 0) {
            hx_fail(h, "connection refused");
        } else if (t - h->t_phase_us > HX_CONNECT_US) {
            hx_fail(h, "connect timed out");
        }
        break;
    }
    case HTTPX_PH_SEND:
        hx_do_send(h, t);
        if (h->result == HTTPX_RUNNING && h->phase == HTTPX_PH_HEADERS) hx_do_recv(h, t);
        break;
    case HTTPX_PH_HEADERS:
    case HTTPX_PH_BODY:
        hx_do_recv(h, t);
        break;
    default:
        break;
    }
    return h->result;
}

const char *httpx_phase_name(const httpx_t *h) {
    switch (h->phase) {
    case HTTPX_PH_IDLE:     return "Idle";
    case HTTPX_PH_DNS_WAIT: return "Waiting for DNS";
    case HTTPX_PH_DNS:      return "Resolving host";
    case HTTPX_PH_CONNECT:  return "Connecting";
    case HTTPX_PH_SEND:     return "Sending request";
    case HTTPX_PH_HEADERS:  return "Waiting for response";
    case HTTPX_PH_BODY:     return "Receiving";
    case HTTPX_PH_DONE:     return "Done";
    default:                return "Failed";
    }
}

const char *httpx_status_text(int s) {
    switch (s) {
    case 200: return "OK";            case 201: return "Created";
    case 202: return "Accepted";      case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently"; case 302: return "Found";
    case 303: return "See Other";     case 304: return "Not Modified";
    case 307: return "Temporary Redirect"; case 308: return "Permanent Redirect";
    case 400: return "Bad Request";   case 401: return "Unauthorized";
    case 403: return "Forbidden";     case 404: return "Not Found";
    case 405: return "Method Not Allowed"; case 408: return "Request Timeout";
    case 409: return "Conflict";      case 410: return "Gone";
    case 413: return "Payload Too Large"; case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable"; case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error"; case 501: return "Not Implemented";
    case 502: return "Bad Gateway";   case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default:  return "";
    }
}

int httpx_header_get(const httpx_t *h, const char *name, char *out, int cap) {
    const char *p = h->hdr;
    while (*p && *p != '\n') p++;          // skip status line
    if (*p == '\n') p++;
    unsigned nl = (unsigned)strlen(name);
    while (*p) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        const char *end = p;
        if (*p == '\n') p++;
        while (end > line && (end[-1] == '\r' || end[-1] == ' ')) end--;
        if ((unsigned)(end - line) > nl && line[nl] == ':' && hx_ci_prefix(line, name) &&
            hx_lower(line[nl - 1]) == hx_lower(name[nl - 1])) {
            const char *v = line + nl + 1;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            int l = (int)(end - v);
            if (l >= cap) l = cap - 1;
            if (l < 0) l = 0;
            memcpy(out, v, (unsigned)l);
            out[l] = 0;
            return l;
        }
    }
    if (cap > 0) out[0] = 0;
    return -1;
}

unsigned long long httpx_elapsed_us(const httpx_t *h) {
    unsigned long long end = (h->result == HTTPX_RUNNING) ? hx_now() : h->t_done_us;
    return end > h->t_start_us ? end - h->t_start_us : 0;
}
