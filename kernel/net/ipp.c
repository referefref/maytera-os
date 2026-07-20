// ipp.c - Internet Printing Protocol client + print manager (#318)
//
// Network printing for MayteraOS. The IPP request is an application/ipp binary
// body POSTed over HTTP to http://<server>:631/printers/<queue>. We reuse the
// kernel HTTP/1.1 client (net/wget.c http_post), which already drives the TCP
// stack (net_poll/tcp_timer/proc_sleep) the same way the browser/LLM clients
// do. Documents are emitted as PostScript, which CUPS filters for any printer.
#include "ipp.h"
#include "wget.h"
#include "tcp.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../gui/image.h"

extern fat_fs_t g_fat_fs;
extern void proc_sleep(uint32_t ms);
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;
extern void net_poll(void);
extern void tcp_timer(void);
extern void net_lock(void);
extern void net_unlock(void);

#define PRINTERS_CFG  "/CONFIG/PRINTERS.CFG"

// ===========================================================================
// In-memory printer table
// ===========================================================================
static printer_cfg_t g_printers[PRINT_MAX_PRINTERS];
static int g_printer_count = 0;
static int g_print_inited = 0;

// ===========================================================================
// Big-endian wire writers
// ===========================================================================
static void put_u8(uint8_t **p, uint8_t v)   { *(*p)++ = v; }
static void put_u16(uint8_t **p, uint16_t v) { *(*p)++ = (v >> 8) & 0xFF; *(*p)++ = v & 0xFF; }
static void put_u32(uint8_t **p, uint32_t v) {
    *(*p)++ = (v >> 24) & 0xFF; *(*p)++ = (v >> 16) & 0xFF;
    *(*p)++ = (v >> 8) & 0xFF;  *(*p)++ = v & 0xFF;
}

// Emit one IPP attribute: value-tag, name, string value.
static void put_attr_str(uint8_t **p, uint8_t tag, const char *name, const char *val) {
    uint16_t nl = (uint16_t)strlen(name);
    uint16_t vl = (uint16_t)strlen(val);
    put_u8(p, tag);
    put_u16(p, nl); memcpy(*p, name, nl); *p += nl;
    put_u16(p, vl); memcpy(*p, val, vl); *p += vl;
}

// Build the printer-uri "ipp://host:port/printers/queue".
static void build_printer_uri(char *out, int cap, const char *host, uint16_t port,
                              const char *queue) {
    snprintf(out, cap, "ipp://%s:%u/printers/%s", host, (unsigned)port, queue);
}

// Build a printer-uri from an explicit resource path, e.g.
// "ipp://192.0.2.55:631/ipp/print".
static void build_printer_uri_res(char *out, int cap, const char *host, uint16_t port,
                                  const char *resource) {
    snprintf(out, cap, "ipp://%s:%u%s", host, (unsigned)port, resource);
}

// ===========================================================================
// IPP response parsing / logging
// ===========================================================================
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Walk the attribute groups of an IPP response and log each attribute to serial.
// If summary != NULL, accumulate a couple of headline attrs into it.
static void ipp_walk_attributes(const uint8_t *buf, uint32_t len,
                                char *summary, int summary_cap) {
    if (summary && summary_cap > 0) summary[0] = '\0';
    if (len < 9) return;
    uint32_t off = 8;  // skip version(2)+status(2)+request-id(4)
    char curname[128] = {0};

    while (off < len) {
        uint8_t tag = buf[off++];
        if (tag == IPP_TAG_END) break;
        if (tag <= IPP_TAG_UNSUPPORTED_GRP) {
            // delimiter / begin-attribute-group tag: no payload
            continue;
        }
        // value tag: name-length(2), name, value-length(2), value
        if (off + 2 > len) break;
        uint16_t nl = rd_u16(buf + off); off += 2;
        if (off + nl > len) break;
        if (nl > 0) {
            uint16_t cn = nl < (uint16_t)(sizeof(curname) - 1) ? nl : (uint16_t)(sizeof(curname) - 1);
            memcpy(curname, buf + off, cn);
            curname[cn] = '\0';
        }
        off += nl;
        if (off + 2 > len) break;
        uint16_t vl = rd_u16(buf + off); off += 2;
        if (off + vl > len) break;
        const uint8_t *val = buf + off;
        off += vl;

        // Decode the value by tag for the serial log.
        if (tag == IPP_TAG_INTEGER || tag == IPP_TAG_ENUM) {
            int iv = (vl == 4) ? (int)rd_u32(val) : 0;
            kprintf("[IPP]   %s = %d\n", curname, iv);
            if (summary && summary[0] == '\0' &&
                (strcmp(curname, "printer-state") == 0))
                snprintf(summary, summary_cap, "%s=%d", curname, iv);
        } else if (tag == IPP_TAG_BOOLEAN) {
            kprintf("[IPP]   %s = %s\n", curname, (vl && val[0]) ? "true" : "false");
        } else {
            // string-like (charset/keyword/uri/name/text/mimeType)
            char sv[160];
            uint16_t c = vl < (uint16_t)(sizeof(sv) - 1) ? vl : (uint16_t)(sizeof(sv) - 1);
            memcpy(sv, val, c); sv[c] = '\0';
            kprintf("[IPP]   %s = %s\n", curname, sv);
            if (summary && strcmp(curname, "printer-make-and-model") == 0)
                snprintf(summary, summary_cap, "%s", sv);
        }
    }
}

// ===========================================================================
// IPP transport: build request, POST, return IPP status-code (>=0) or <0.
// ===========================================================================
// header_buf: pre-built IPP header+attrs (ends just past end-of-attributes-tag)
// doc/doc_len: optional document data appended after the IPP attrs (Print-Job)
// Pre-resolve ARP for an on-link target so the first TCP SYN is not dropped.
// The kernel HTTP connect window (~1.2s) is shorter than a SYN retransmit after
// ARP resolves; remote hosts work because the gateway MAC is already cached, but
// a LAN printer needs its own ARP entry first (see #297 / SMB notes).
static void ipp_warm_arp(const char *host) {
    extern uint32_t wget_parse_ip(const char *s);
    extern int arp_resolve(uint32_t ip, uint8_t *mac);
    int is_ip = (host[0] != 0);
    for (const char *q = host; *q; q++)
        if (!((*q >= '0' && *q <= '9') || *q == '.')) { is_ip = 0; break; }
    if (!is_ip) return;
    uint32_t ip = wget_parse_ip(host);
    if (!ip) return;
    uint8_t mac[6];
    uint64_t start = timer_ticks;
    uint64_t hz = g_timer_hz ? g_timer_hz : 250;
    while (!arp_resolve(ip, mac)) {
        net_poll();
        tcp_timer();
        if (timer_ticks - start > hz * 4) {
            kprintf("[IPP] ARP warm-up for %s timed out\n", host);
            return;
        }
        proc_sleep(2);
    }
    kprintf("[IPP] ARP resolved for %s (%02x:%02x:%02x:%02x:%02x:%02x)\n",
            host, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Send all of [data,len) on a connected socket, serialized against net_poll.
static int ipp_send_all(int sock, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sent = 0;
    uint64_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t start = timer_ticks;
    while (sent < len) {
        uint32_t want = len - sent;
        if (want > 16384) want = 16384;
        net_lock();
        int s = tcp_send(sock, p + sent, (uint16_t)want);
        net_unlock();
        if (s > 0) { sent += s; start = timer_ticks; }
        else if (s == TCP_ERR_WOULD_BLOCK || s == 0) { net_poll(); tcp_timer(); proc_sleep(2); }
        else return -1;
        if (timer_ticks - start > hz * 10) return -1;
    }
    net_poll(); tcp_timer();
    return (int)sent;
}

// One IPP request/response over a fresh TCP connection. Returns the IPP
// status-code (>=0) or <0 on transport error. The IPP response body is left at
// resp_buf[0..*resp_len_out).
static int ipp_exchange(const char *host, uint16_t port, const char *resource,
                        const uint8_t *ipp_req, uint32_t ipp_len,
                        uint8_t *resp_buf, uint32_t resp_cap, uint32_t *resp_len_out) {
    extern uint32_t wget_parse_ip(const char *s);
    if (resp_len_out) *resp_len_out = 0;
    uint32_t ip = wget_parse_ip(host);
    if (!ip) { kprintf("[IPP] cannot parse host '%s'\n", host); return -1; }
    uint64_t hz = g_timer_hz ? g_timer_hz : 250;

    ipp_warm_arp(host);
    kprintf("[IPP] POST http://%s:%u%s (%u byte IPP body)\n",
            host, (unsigned)port, resource, ipp_len);

    for (int attempt = 0; attempt < 3; attempt++) {
        int sock = tcp_socket();
        if (sock < 0) { kprintf("[IPP] tcp_socket failed\n"); proc_sleep(200); continue; }

        net_lock();
        int cr = tcp_connect(sock, ip, port);
        net_unlock();
        if (cr < 0 && cr != TCP_ERR_IN_PROGRESS) {
            net_lock(); tcp_close(sock); net_unlock();
            kprintf("[IPP] connect rc=%d, retrying\n", cr);
            proc_sleep(300); continue;
        }

        // Wait for the 3-way handshake.
        uint64_t start = timer_ticks;
        int connected = 0;
        while (timer_ticks - start < hz * 5) {
            net_poll(); tcp_timer();
            if (tcp_is_connected(sock)) { connected = 1; break; }
            if (tcp_get_state(sock) == TCP_STATE_CLOSED) break;
            proc_sleep(2);
        }
        if (!connected) {
            net_lock(); tcp_close(sock); net_unlock();
            kprintf("[IPP] connect attempt %d timed out\n", attempt + 1);
            proc_sleep(300); continue;
        }

        // Build + send the HTTP request (Connection: close -> server closes after
        // the full response, so we just read to EOF; no chunked/length parsing).
        char hdr[512];
        int hlen = snprintf(hdr, sizeof(hdr),
            "POST %s HTTP/1.1\r\n"
            "Host: %s:%u\r\n"
            "User-Agent: MayteraOS-IPP/1.0\r\n"
            "Content-Type: application/ipp\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "\r\n",
            resource, host, (unsigned)port, ipp_len);
        if (ipp_send_all(sock, hdr, (uint32_t)hlen) < 0 ||
            ipp_send_all(sock, ipp_req, ipp_len) < 0) {
            net_lock(); tcp_close(sock); net_unlock();
            kprintf("[IPP] send failed, retrying\n");
            proc_sleep(300); continue;
        }
        kprintf("[IPP] request sent (%d hdr + %u body), waiting for response\n",
                hlen, ipp_len);

        // Receive the whole HTTP response until the peer closes (or timeout).
        uint32_t total = 0;
        uint64_t rstart = timer_ticks;
        int got_close = 0;
        while (timer_ticks - rstart < hz * 20 && total < resp_cap - 1) {
            net_poll(); tcp_timer();
            uint32_t space = resp_cap - 1 - total;
            if (space > 16384) space = 16384;
            net_lock();
            int n = tcp_recv(sock, resp_buf + total, (uint16_t)space);
            net_unlock();
            if (n > 0) { total += n; rstart = timer_ticks; continue; }
            if (n == TCP_ERR_CLOSED) { got_close = 1; break; }
            tcp_state_t st = tcp_get_state(sock);
            if (st == TCP_STATE_CLOSED || st == TCP_STATE_TIME_WAIT) { got_close = 1; break; }
            proc_sleep(2);
        }
        net_lock(); tcp_close(sock); net_unlock();
        (void)got_close;

        if (total < 12) {
            kprintf("[IPP] short response (%u bytes), retrying\n", total);
            proc_sleep(300); continue;
        }
        resp_buf[total] = 0;

        // Parse the HTTP status line: "HTTP/1.x <code> ...".
        int http_code = 0;
        {
            uint32_t i = 0;
            while (i < total && resp_buf[i] != ' ') i++;
            while (i < total && resp_buf[i] == ' ') i++;
            while (i < total && resp_buf[i] >= '0' && resp_buf[i] <= '9')
                http_code = http_code * 10 + (resp_buf[i++] - '0');
        }
        // Find the body (first CRLFCRLF).
        uint32_t body = 0;
        for (uint32_t i = 0; i + 3 < total; i++) {
            if (resp_buf[i] == '\r' && resp_buf[i+1] == '\n' &&
                resp_buf[i+2] == '\r' && resp_buf[i+3] == '\n') { body = i + 4; break; }
        }
        kprintf("[IPP] HTTP %d, %u bytes total, body at %u\n", http_code, total, body);
        if (http_code != 200) { kprintf("[IPP] server HTTP %d\n", http_code); return -1; }
        if (body == 0 || total - body < 8) {
            kprintf("[IPP] no/short IPP body\n"); return -1;
        }
        uint32_t blen = total - body;
        // Shift the IPP body to the front of resp_buf for the caller.
        memmove(resp_buf, resp_buf + body, blen);
        uint16_t ipp_status = rd_u16(resp_buf + 2);
        kprintf("[IPP] version=%u.%u status-code=0x%04x request-id=%u (%u byte IPP body)\n",
                resp_buf[0], resp_buf[1], ipp_status, rd_u32(resp_buf + 4), blen);
        if (resp_len_out) *resp_len_out = blen;
        return (int)ipp_status;
    }
    kprintf("[IPP] all attempts failed\n");
    return -1;
}

int ipp_get_printer_attributes(const char *host, uint16_t port, const char *queue,
                               char *info_out, int info_cap) {
    if (!host || !queue) return -1;
    if (port == 0) port = 631;

    static uint32_t s_reqid = 1;
    uint8_t *buf = (uint8_t *)kmalloc(1024);
    if (!buf) return -1;
    uint8_t *p = buf;
    char uri[256];
    build_printer_uri(uri, sizeof(uri), host, port, queue);

    put_u8(&p, IPP_VERSION_MAJOR);
    put_u8(&p, IPP_VERSION_MINOR);
    put_u16(&p, IPP_OP_GET_PRINTER_ATTRIBUTES);
    put_u32(&p, s_reqid++);
    put_u8(&p, IPP_TAG_OPERATION);
    put_attr_str(&p, IPP_TAG_CHARSET,  "attributes-charset",          "utf-8");
    put_attr_str(&p, IPP_TAG_LANGUAGE, "attributes-natural-language", "en-us");
    put_attr_str(&p, IPP_TAG_URI,      "printer-uri",                 uri);
    put_attr_str(&p, IPP_TAG_NAME,     "requesting-user-name",        "maytera");
    put_u8(&p, IPP_TAG_END);
    uint32_t len = (uint32_t)(p - buf);

    uint32_t resp_cap = 65536;
    uint8_t *resp = (uint8_t *)kmalloc(resp_cap);
    if (!resp) { kfree(buf); return -1; }

    char resource[128];
    snprintf(resource, sizeof(resource), "/printers/%s", queue);
    uint32_t rlen = 0;
    int status = ipp_exchange(host, port, resource, buf, len, resp, resp_cap, &rlen);
    if (status >= 0 && (status == IPP_STATUS_OK || status == IPP_STATUS_OK_IGNORED)) {
        kprintf("[IPP] printer attributes:\n");
        ipp_walk_attributes(resp, rlen, info_out, info_cap);
    } else if (info_out && info_cap > 0) {
        info_out[0] = '\0';
    }
    kfree(resp);
    kfree(buf);
    return status;
}

// Print-Job to an explicit HTTP/IPP resource path (e.g. "/ipp/print" for a
// direct AirPrint printer, or "/printers/<queue>" for a CUPS queue). The
// printer-uri attribute is derived from the same resource. This is the general
// path; ipp_print_job() below is the CUPS-queue convenience wrapper.
int ipp_print_job_res(const char *host, uint16_t port, const char *resource,
                      const char *job_name, const char *doc_format,
                      const void *doc, uint32_t doc_len) {
    if (!host || !resource || !doc || doc_len == 0) return -1;
    if (port == 0) port = 631;
    if (!doc_format) doc_format = "application/postscript";
    if (!job_name)   job_name = "MayteraOS";

    static uint32_t s_reqid = 100;
    char uri[256];
    build_printer_uri_res(uri, sizeof(uri), host, port, resource);

    // header (attrs) + document data in one contiguous body
    uint32_t cap = 1024 + doc_len;
    uint8_t *buf = (uint8_t *)kmalloc(cap);
    if (!buf) { kprintf("[IPP] Print-Job: kmalloc(%u) failed\n", cap); return -1; }
    uint8_t *p = buf;

    put_u8(&p, IPP_VERSION_MAJOR);
    put_u8(&p, IPP_VERSION_MINOR);
    put_u16(&p, IPP_OP_PRINT_JOB);
    put_u32(&p, s_reqid++);
    put_u8(&p, IPP_TAG_OPERATION);
    put_attr_str(&p, IPP_TAG_CHARSET,  "attributes-charset",          "utf-8");
    put_attr_str(&p, IPP_TAG_LANGUAGE, "attributes-natural-language", "en-us");
    put_attr_str(&p, IPP_TAG_URI,      "printer-uri",                 uri);
    put_attr_str(&p, IPP_TAG_NAME,     "requesting-user-name",        "maytera");
    put_attr_str(&p, IPP_TAG_NAME,     "job-name",                    job_name);
    put_attr_str(&p, IPP_TAG_MIMETYPE, "document-format",             doc_format);
    put_u8(&p, IPP_TAG_END);
    // append the document
    memcpy(p, doc, doc_len); p += doc_len;
    uint32_t len = (uint32_t)(p - buf);

    uint32_t resp_cap = 16384;
    uint8_t *resp = (uint8_t *)kmalloc(resp_cap);
    if (!resp) { kfree(buf); return -1; }

    uint32_t rlen = 0;
    int status = ipp_exchange(host, port, resource, buf, len, resp, resp_cap, &rlen);
    if (status == IPP_STATUS_OK || status == IPP_STATUS_OK_IGNORED) {
        kprintf("[IPP] Print-Job accepted, job attributes:\n");
        ipp_walk_attributes(resp, rlen, 0, 0);
    }
    kfree(resp);
    kfree(buf);
    return status;
}

int ipp_print_job(const char *host, uint16_t port, const char *queue,
                  const char *job_name, const char *doc_format,
                  const void *doc, uint32_t doc_len) {
    if (!host || !queue) return -1;
    char resource[128];
    snprintf(resource, sizeof(resource), "/printers/%s", queue);
    return ipp_print_job_res(host, port, resource, job_name, doc_format, doc, doc_len);
}

// ===========================================================================
// PostScript generation
// ===========================================================================
// Append str to buf (bounded), advancing *pos. Returns space remaining.
static void ps_emit(char *buf, int cap, int *pos, const char *s) {
    while (*s && *pos < cap - 1) buf[(*pos)++] = *s++;
    if (*pos < cap) buf[*pos] = '\0';
}

// Emit one PostScript string literal with ( ) \ escaped.
static void ps_emit_escaped(char *buf, int cap, int *pos, const char *s) {
    for (; *s && *pos < cap - 2; s++) {
        if (*s == '(' || *s == ')' || *s == '\\') buf[(*pos)++] = '\\';
        buf[(*pos)++] = *s;
    }
    if (*pos < cap) buf[*pos] = '\0';
}

int ps_generate_text_page(const char *title, const char *text, char *buf, int cap) {
    // Sizing pass not separately supported: require a buffer.
    if (!buf || cap < 256) return -1;
    if (!title) title = "MayteraOS Test Page";
    if (!text)  text = "";

    int pos = 0;
    char line[128];

    ps_emit(buf, cap, &pos, "%!PS-Adobe-3.0\n");
    snprintf(line, sizeof(line), "%%%%Title: %s\n", title);
    ps_emit(buf, cap, &pos, line);
    ps_emit(buf, cap, &pos, "%%Creator: MayteraOS IPP client (#318)\n");
    ps_emit(buf, cap, &pos, "%%Pages: 1\n%%EndComments\n%%Page: 1 1\n");

    // Title in 18pt Helvetica-Bold near the top of US Letter (612x792).
    ps_emit(buf, cap, &pos, "/Helvetica-Bold findfont 18 scalefont setfont\n");
    ps_emit(buf, cap, &pos, "72 740 moveto (");
    ps_emit_escaped(buf, cap, &pos, title);
    ps_emit(buf, cap, &pos, ") show\n");
    // Rule under the title.
    ps_emit(buf, cap, &pos, "72 734 moveto 468 0 rlineto stroke\n");

    // Body lines in 12pt Helvetica, stepping down 16pt each.
    ps_emit(buf, cap, &pos, "/Helvetica findfont 12 scalefont setfont\n");
    int y = 710;
    const char *s = text;
    while (*s && pos < cap - 64) {
        int n = 0;
        while (s[n] && s[n] != '\n' && n < (int)sizeof(line) - 1) n++;
        memcpy(line, s, n); line[n] = '\0';
        snprintf(buf + pos, cap - pos, "72 %d moveto (", y);
        pos += (int)strlen(buf + pos);
        ps_emit_escaped(buf, cap, &pos, line);
        ps_emit(buf, cap, &pos, ") show\n");
        y -= 16;
        s += n;
        if (*s == '\n') s++;
        if (y < 72) break;  // bottom margin
    }

    ps_emit(buf, cap, &pos, "showpage\n%%EOF\n");
    return pos;
}

// ===========================================================================
// Image printing (#318)
//   Two paths:
//     1) DIRECT: for a .jpg/.jpeg file, send the raw bytes as document-format
//        image/jpeg straight to an AirPrint/IPP-Everywhere printer (it
//        rasterizes JPEG itself: no decode/encode on our side).
//     2) VIA CUPS / PostScript: decode any image (jpeg/png/bmp) to RGB with the
//        existing gui decoders, downscale, and embed it in a PostScript page
//        (the colorimage operator) for a CUPS queue to rasterize.
// ===========================================================================

// Case-insensitive suffix test.
static int str_ends_ci(const char *s, const char *suf) {
    int ls = (int)strlen(s), lf = (int)strlen(suf);
    if (lf > ls) return 0;
    const char *a = s + ls - lf;
    for (int i = 0; i < lf; i++) {
        char c1 = a[i], c2 = suf[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
    }
    return 1;
}

// Map a filename extension to an IPP document-format (NULL if not an image we
// send raw). Only image/jpeg is truly native to typical AirPrint hardware.
static const char *image_doc_format(const char *path) {
    if (str_ends_ci(path, ".jpg") || str_ends_ci(path, ".jpeg")) return "image/jpeg";
    if (str_ends_ci(path, ".png"))  return "image/png";
    return 0;
}

// Emit one byte as two lowercase hex chars.
static void ps_emit_hex_byte(char *buf, int cap, int *pos, uint8_t v) {
    static const char *hx = "0123456789abcdef";
    if (*pos < cap - 2) { buf[(*pos)++] = hx[v >> 4]; buf[(*pos)++] = hx[v & 0xF]; }
}

// Render decoded RGB pixels (0x00RRGGBB, framebuffer format) as a PostScript
// page with the colorimage operator, downscaled so the larger pixel dimension
// is <= maxdim (bounds the POST body). Returns bytes written or <0.
// Caller must size buf for ~ out_w*out_h*6 + a few hundred bytes.
int ps_generate_image_page(const uint32_t *px, uint32_t sw, uint32_t sh,
                           int maxdim, char *buf, int cap) {
    if (!px || !buf || sw == 0 || sh == 0 || cap < 512) return -1;
    if (maxdim < 16) maxdim = 16;

    // Output pixel resolution (nearest-neighbor downscale, keep aspect).
    uint32_t ow = sw, oh = sh;
    if ((int)sw > maxdim || (int)sh > maxdim) {
        if (sw >= sh) { ow = (uint32_t)maxdim; oh = (sh * (uint32_t)maxdim) / sw; }
        else          { oh = (uint32_t)maxdim; ow = (sw * (uint32_t)maxdim) / sh; }
        if (ow == 0) ow = 1;
        if (oh == 0) oh = 1;
    }

    // Printed size in points: fit inside US Letter with 72pt margins,
    // preserving the source aspect ratio.
    double avail_w = 468.0, avail_h = 648.0;
    double s = avail_w / (double)sw;
    if ((double)sh * s > avail_h) s = avail_h / (double)sh;
    int pw = (int)((double)sw * s), ph = (int)((double)sh * s);
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;
    int tx = (612 - pw) / 2, ty = (792 - ph) / 2;

    int pos = 0;
    char line[160];
    ps_emit(buf, cap, &pos, "%!PS-Adobe-3.0\n");
    ps_emit(buf, cap, &pos, "%%Title: MayteraOS Image Print (#318)\n");
    ps_emit(buf, cap, &pos, "%%Creator: MayteraOS IPP client\n");
    ps_emit(buf, cap, &pos, "%%Pages: 1\n%%EndComments\n%%Page: 1 1\n");
    snprintf(line, sizeof(line), "/picstr %u 3 mul string def\n", ow);
    ps_emit(buf, cap, &pos, line);
    ps_emit(buf, cap, &pos, "gsave\n");
    snprintf(line, sizeof(line), "%d %d translate %d %d scale\n", tx, ty, pw, ph);
    ps_emit(buf, cap, &pos, line);
    snprintf(line, sizeof(line), "%u %u 8 [%u 0 0 -%u 0 %u]\n", ow, oh, ow, oh, oh);
    ps_emit(buf, cap, &pos, line);
    ps_emit(buf, cap, &pos, "{ currentfile picstr readhexstring pop } false 3 colorimage\n");

    // Emit RGB hex rows top-to-bottom (matrix has -oh so row 0 is top).
    for (uint32_t oy = 0; oy < oh; oy++) {
        uint32_t syi = (oy * sh) / oh;
        if (syi >= sh) syi = sh - 1;
        for (uint32_t ox = 0; ox < ow; ox++) {
            uint32_t sxi = (ox * sw) / ow;
            if (sxi >= sw) sxi = sw - 1;
            uint32_t p = px[syi * sw + sxi];
            ps_emit_hex_byte(buf, cap, &pos, (uint8_t)((p >> 16) & 0xFF)); // R
            ps_emit_hex_byte(buf, cap, &pos, (uint8_t)((p >> 8)  & 0xFF)); // G
            ps_emit_hex_byte(buf, cap, &pos, (uint8_t)(p & 0xFF));         // B
        }
        if (pos < cap - 1) buf[pos++] = '\n';
    }
    ps_emit(buf, cap, &pos, "grestore\nshowpage\n%%EOF\n");
    buf[pos < cap ? pos : cap - 1] = '\0';
    kprintf("[PRINT] image PS: src %ux%u -> raster %ux%u, %d bytes\n", sw, sh, ow, oh, pos);
    return pos;
}

// Decode an image file to RGB and print it as PostScript to a resource path.
// maxdim bounds the raster resolution to keep the POST body small.
static int ipp_print_image_ps(const char *host, uint16_t port, const char *resource,
                              const char *path, int maxdim) {
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, path, &sz);
    if (!data) { kprintf("[PRINT] cannot read %s\n", path); return -1; }

    image_t img;
    int ir = image_load(data, sz, &img);
    kfree(data);
    if (ir != IMAGE_SUCCESS || !img.pixels) {
        kprintf("[PRINT] image decode of %s failed (%d)\n", path, ir);
        return -1;
    }
    kprintf("[PRINT] decoded %s: %ux%u\n", path, img.width, img.height);

    // Bound the raster then size the PS buffer for it.
    uint32_t ow = img.width, oh = img.height;
    if ((int)ow > maxdim || (int)oh > maxdim) {
        if (ow >= oh) { oh = (oh * (uint32_t)maxdim) / ow; ow = (uint32_t)maxdim; }
        else          { ow = (ow * (uint32_t)maxdim) / oh; oh = (uint32_t)maxdim; }
    }
    uint32_t cap = 1024 + ow * oh * 6 + oh + 64;
    char *ps = (char *)kmalloc(cap);
    if (!ps) { image_free(&img); kprintf("[PRINT] kmalloc(%u) for PS failed\n", cap); return -1; }

    int n = ps_generate_image_page(img.pixels, img.width, img.height, maxdim, ps, (int)cap);
    image_free(&img);
    if (n <= 0) { kfree(ps); return -1; }

    int st = ipp_print_job_res(host, port, resource, "MayteraOS Image",
                               "application/postscript", ps, (uint32_t)n);
    kfree(ps);
    return st;
}

// High-level: print an image FILE to a printer addressed by host/port/resource.
//   .jpg/.jpeg -> raw image/jpeg (direct AirPrint rasterization).
//   otherwise  -> decode + PostScript colorimage (needs CUPS to rasterize).
// Returns the IPP status-code (>=0) or <0.
int ipp_print_image(const char *host, uint16_t port, const char *resource,
                    const char *path) {
    if (!host || !resource || !path) return -1;
    const char *fmt = image_doc_format(path);
    if (fmt && strcmp(fmt, "image/jpeg") == 0) {
        uint32_t sz = 0;
        void *data = fat_read_file(&g_fat_fs, path, &sz);
        if (!data) { kprintf("[PRINT] cannot read %s\n", path); return -1; }
        kprintf("[PRINT] direct image/jpeg: %s (%u bytes) -> %s%s\n",
                path, sz, host, resource);
        int st = ipp_print_job_res(host, port, resource, "MayteraOS Image",
                                   "image/jpeg", data, sz);
        kfree(data);
        return st;
    }
    // Non-JPEG: decode and embed as PostScript (CUPS rasterizes).
    return ipp_print_image_ps(host, port, resource, path, 480);
}

// ===========================================================================
// Print manager: config persistence (/CONFIG/PRINTERS.CFG)
// ===========================================================================
// File format (one printer per line):
//   name=<n> host=<ip> port=<p> queue=<q> default=<0|1>
static void printers_save(void) {
    char out[PRINT_MAX_PRINTERS * 256];
    int pos = 0;
    out[0] = '\0';
    for (int i = 0; i < g_printer_count; i++) {
        printer_cfg_t *pr = &g_printers[i];
        if (!pr->valid) continue;
        snprintf(out + pos, sizeof(out) - pos,
                 "name=%s host=%s port=%u queue=%s default=%d\n",
                 pr->name, pr->host, (unsigned)pr->port, pr->queue, pr->is_default);
        pos += (int)strlen(out + pos);
    }
    int r = fat_write_file(&g_fat_fs, PRINTERS_CFG, out, (uint32_t)pos);
    if (r < 0) kprintf("[PRINT] warning: could not save %s (%d)\n", PRINTERS_CFG, r);
}

// Pull "key=value" (whitespace-delimited value) from a line. Returns 1 if found.
static int cfg_field(const char *line, const char *key, char *out, int cap) {
    int kl = (int)strlen(key);
    const char *p = line;
    while (*p) {
        // match key at a token boundary followed by '='
        if ((p == line || p[-1] == ' ' || p[-1] == '\t') &&
            strncmp(p, key, kl) == 0 && p[kl] == '=') {
            p += kl + 1;
            int n = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && n < cap - 1)
                out[n++] = *p++;
            out[n] = '\0';
            return 1;
        }
        p++;
    }
    return 0;
}

void print_init(void) {
    if (g_print_inited) return;
    g_print_inited = 1;
    g_printer_count = 0;

    uint32_t sz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, PRINTERS_CFG, &sz);
    if (!cfg) {
        kprintf("[PRINT] no %s (no printers configured)\n", PRINTERS_CFG);
        return;
    }
    char *line = cfg;
    while (line && *line && g_printer_count < PRINT_MAX_PRINTERS) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char namev[PRINT_NAME_LEN], portv[16];
        printer_cfg_t *pr = &g_printers[g_printer_count];
        memset(pr, 0, sizeof(*pr));
        if (cfg_field(line, "name", namev, sizeof(namev)) && namev[0]) {
            strncpy(pr->name, namev, PRINT_NAME_LEN - 1);
            cfg_field(line, "host", pr->host, PRINT_HOST_LEN);
            cfg_field(line, "queue", pr->queue, PRINT_QUEUE_LEN);
            pr->port = 631;
            if (cfg_field(line, "port", portv, sizeof(portv)) && portv[0]) {
                int pv = 0; for (char *q = portv; *q >= '0' && *q <= '9'; q++) pv = pv * 10 + (*q - '0');
                if (pv > 0) pr->port = (uint16_t)pv;
            }
            char defv[8];
            pr->is_default = (cfg_field(line, "default", defv, sizeof(defv)) && defv[0] == '1');
            pr->valid = 1;
            g_printer_count++;
        }
        if (!nl) break;
        line = nl + 1;
    }
    kfree(cfg);
    kprintf("[PRINT] loaded %d printer(s) from %s\n", g_printer_count, PRINTERS_CFG);
}

int print_list(printer_cfg_t *out, int max) {
    if (!g_print_inited) print_init();
    int n = 0;
    for (int i = 0; i < g_printer_count && n < max; i++) {
        if (g_printers[i].valid) {
            if (out) out[n] = g_printers[i];
            n++;
        }
    }
    return n;
}

int print_add(const char *name, const char *host, uint16_t port,
               const char *queue, int make_default) {
    if (!g_print_inited) print_init();
    if (!name || !host || !queue) return -1;
    if (port == 0) port = 631;

    printer_cfg_t *pr = 0;
    // replace if name already exists
    for (int i = 0; i < g_printer_count; i++)
        if (g_printers[i].valid && strcmp(g_printers[i].name, name) == 0) { pr = &g_printers[i]; break; }
    if (!pr) {
        if (g_printer_count >= PRINT_MAX_PRINTERS) return -1;
        pr = &g_printers[g_printer_count++];
    }
    memset(pr, 0, sizeof(*pr));
    strncpy(pr->name, name, PRINT_NAME_LEN - 1);
    strncpy(pr->host, host, PRINT_HOST_LEN - 1);
    strncpy(pr->queue, queue, PRINT_QUEUE_LEN - 1);
    pr->port = port;
    pr->is_default = make_default ? 1 : 0;
    pr->valid = 1;
    if (make_default)
        for (int i = 0; i < g_printer_count; i++)
            if (&g_printers[i] != pr) g_printers[i].is_default = 0;
    printers_save();
    return 0;
}

int print_remove(const char *name) {
    if (!g_print_inited) print_init();
    if (!name) return -1;
    for (int i = 0; i < g_printer_count; i++) {
        if (g_printers[i].valid && strcmp(g_printers[i].name, name) == 0) {
            for (int j = i; j < g_printer_count - 1; j++) g_printers[j] = g_printers[j + 1];
            g_printer_count--;
            printers_save();
            return 0;
        }
    }
    return -1;
}

// Resolve a printer by name (NULL/empty -> default, else first).
static printer_cfg_t *find_printer(const char *name) {
    if (!g_print_inited) print_init();
    if (g_printer_count == 0) return 0;
    if (name && name[0]) {
        for (int i = 0; i < g_printer_count; i++)
            if (g_printers[i].valid && strcmp(g_printers[i].name, name) == 0) return &g_printers[i];
        return 0;
    }
    for (int i = 0; i < g_printer_count; i++)
        if (g_printers[i].valid && g_printers[i].is_default) return &g_printers[i];
    for (int i = 0; i < g_printer_count; i++)
        if (g_printers[i].valid) return &g_printers[i];
    return 0;
}

int print_job_doc(const char *printer_name, const char *job_name,
                  const char *doc_format, const void *doc, uint32_t len) {
    printer_cfg_t *pr = find_printer(printer_name);
    if (!pr) { kprintf("[PRINT] no such printer\n"); return -1; }
    int st = ipp_print_job(pr->host, pr->port, pr->queue, job_name, doc_format, doc, len);
    return (st == IPP_STATUS_OK || st == IPP_STATUS_OK_IGNORED) ? 0 : -1;
}

int print_job_text(const char *printer_name, const char *title, const char *text) {
    int cap = 8192 + (text ? (int)strlen(text) * 2 : 0);
    char *ps = (char *)kmalloc(cap);
    if (!ps) return -1;
    int n = ps_generate_text_page(title, text, ps, cap);
    if (n <= 0) { kfree(ps); return -1; }
    int r = print_job_doc(printer_name, title ? title : "MayteraOS",
                          "application/postscript", ps, (uint32_t)n);
    kfree(ps);
    return r;
}

// Print an image file to a CONFIGURED printer (NULL/empty = default).
// A .jpg/.jpeg is sent as raw image/jpeg (CUPS + AirPrint both rasterize it);
// other formats are decoded and embedded as PostScript. Returns 0 or -1.
int print_job_image(const char *printer_name, const char *path) {
    printer_cfg_t *pr = find_printer(printer_name);
    if (!pr) { kprintf("[PRINT] no such printer\n"); return -1; }
    char resource[128];
    snprintf(resource, sizeof(resource), "/printers/%s", pr->queue);
    int st = ipp_print_image(pr->host, pr->port, resource, path);
    return (st == IPP_STATUS_OK || st == IPP_STATUS_OK_IGNORED) ? 0 : -1;
}

// Stretch (#318): print the current screen (framebuffer) as a PostScript image
// to a configured printer. We already have raw RGB, so no encoder is needed.
int print_job_screen(const char *printer_name) {
    extern uint32_t fb_get_width(void);
    extern uint32_t fb_get_height(void);
    extern uint32_t *fb_get_back_buffer(void);
    uint32_t w = fb_get_width(), h = fb_get_height();
    uint32_t *fb = fb_get_back_buffer();
    if (!fb || w == 0 || h == 0) { kprintf("[PRINT] no framebuffer\n"); return -1; }

    printer_cfg_t *pr = find_printer(printer_name);
    if (!pr) { kprintf("[PRINT] no such printer\n"); return -1; }

    int maxdim = 640;
    uint32_t ow = w, oh = h;
    if ((int)ow > maxdim || (int)oh > maxdim) {
        if (ow >= oh) { oh = (oh * (uint32_t)maxdim) / ow; ow = (uint32_t)maxdim; }
        else          { ow = (ow * (uint32_t)maxdim) / oh; oh = (uint32_t)maxdim; }
    }
    uint32_t cap = 1024 + ow * oh * 6 + oh + 64;
    char *ps = (char *)kmalloc(cap);
    if (!ps) { kprintf("[PRINT] kmalloc(%u) for screen PS failed\n", cap); return -1; }
    int n = ps_generate_image_page(fb, w, h, maxdim, ps, (int)cap);
    if (n <= 0) { kfree(ps); return -1; }
    char resource[128];
    snprintf(resource, sizeof(resource), "/printers/%s", pr->queue);
    int st = ipp_print_job_res(pr->host, pr->port, resource, "MayteraOS Screen",
                               "application/postscript", ps, (uint32_t)n);
    kfree(ps);
    return (st == IPP_STATUS_OK || st == IPP_STATUS_OK_IGNORED) ? 0 : -1;
}

// ===========================================================================
// Boot self-test (task #318) - gated on /CONFIG/PRINTTEST.CFG
//   PRINTTEST.CFG fields:  ip=<server> queue=<name> [port=<p>]
// ===========================================================================
void print_run_selftest(void) {
    uint32_t sz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/PRINTTEST.CFG", &sz);
    if (!cfg) return;  // silent if not present

    char ip[64] = {0}, queue[64] = {0}, portv[16] = {0};
    cfg_field(cfg, "ip", ip, sizeof(ip));
    cfg_field(cfg, "queue", queue, sizeof(queue));
    cfg_field(cfg, "port", portv, sizeof(portv));
    kfree(cfg);

    uint16_t port = 631;
    if (portv[0]) { int pv = 0; for (char *q = portv; *q >= '0' && *q <= '9'; q++) pv = pv * 10 + (*q - '0'); if (pv) port = (uint16_t)pv; }

    if (!ip[0] || !queue[0]) {
        kprintf("[PRINTTEST] config present but missing ip/queue\n");
        return;
    }

    kprintf("\n========== PRINT SELFTEST (task #318) ==========\n");
    kprintf("[PRINTTEST] target=%s:%u queue=%s\n", ip, (unsigned)port, queue);

    // 1) Get-Printer-Attributes: discover/validate.
    char info[160] = {0};
    int st = ipp_get_printer_attributes(ip, port, queue, info, sizeof(info));
    if (st == IPP_STATUS_OK || st == IPP_STATUS_OK_IGNORED) {
        kprintf("[PRINTTEST] Get-Printer-Attributes OK (status 0x%04x) %s\n", st, info);
    } else {
        kprintf("[PRINTTEST] Get-Printer-Attributes FAILED (status %d)\n", st);
        kprintf("========== PRINT SELFTEST: FAIL ==========\n");
        return;
    }

    // 2) Register the printer (persists to /CONFIG/PRINTERS.CFG) and print.
    print_add("selftest", ip, port, queue, 1);

    const char *body =
        "MayteraOS network printing self-test (#318)\n"
        "\n"
        "This page was generated in-kernel as PostScript and submitted\n"
        "via the IPP Print-Job operation over HTTP to a CUPS server.\n"
        "\n"
        "IPP client: net/ipp.c    HTTP: net/wget.c    Format: application/postscript\n"
        "If you can read this, MayteraOS printed successfully.\n";

    int pr = print_job_text("selftest", "MayteraOS Print Test Page (#318)", body);
    if (pr == 0) {
        kprintf("[PRINTTEST] Print-Job OK - document submitted\n");
        kprintf("========== PRINT SELFTEST: PASS ==========\n");
    } else {
        kprintf("[PRINTTEST] Print-Job FAILED\n");
        kprintf("========== PRINT SELFTEST: FAIL ==========\n");
    }
}

// ---------------------------------------------------------------------------
// Image print self-test (#318) - gated on /CONFIG/PRINTIMG.CFG
//   Fields: mode=<direct|cups> host=<ip> port=<p> resource=<path>
//           queue=<name> path=<image file on disk>
//   direct: send raw image/jpeg to host:port<resource> (e.g. an AirPrint
//           printer at /ipp/print). cups: PostScript-embed to a CUPS queue.
// ---------------------------------------------------------------------------
void print_run_image_selftest(void) {
    uint32_t sz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/PRINTIMG.CFG", &sz);
    if (!cfg) return;  // silent if not present

    char mode[16] = {0}, host[64] = {0}, portv[16] = {0}, resource[96] = {0};
    char queue[64] = {0}, path[96] = {0};
    cfg_field(cfg, "mode", mode, sizeof(mode));
    cfg_field(cfg, "host", host, sizeof(host));
    cfg_field(cfg, "ip", host, sizeof(host));   // "ip" accepted as alias for host
    cfg_field(cfg, "port", portv, sizeof(portv));
    cfg_field(cfg, "resource", resource, sizeof(resource));
    cfg_field(cfg, "queue", queue, sizeof(queue));
    cfg_field(cfg, "path", path, sizeof(path));
    kfree(cfg);

    uint16_t port = 631;
    if (portv[0]) { int pv = 0; for (char *q = portv; *q >= '0' && *q <= '9'; q++) pv = pv * 10 + (*q - '0'); if (pv) port = (uint16_t)pv; }

    if (!host[0] || !path[0]) {
        kprintf("[PRINTIMG] config present but missing host/path\n");
        return;
    }

    kprintf("\n========== IMAGE PRINT SELFTEST (task #318) ==========\n");

    int st;
    if (queue[0] && (mode[0] == 'c' || mode[0] == 'C')) {
        // CUPS PostScript-embed path via a registered printer.
        print_add("imgtest", host, port, queue, 1);
        kprintf("[PRINTIMG] mode=cups host=%s:%u queue=%s path=%s\n",
                host, (unsigned)port, queue, path);
        st = print_job_image("imgtest", path);
        st = (st == 0) ? IPP_STATUS_OK : -1;
    } else {
        // Direct: raw image/jpeg (or PS for non-jpeg) to an explicit resource.
        if (!resource[0]) strncpy(resource, "/ipp/print", sizeof(resource) - 1);
        kprintf("[PRINTIMG] mode=direct host=%s:%u resource=%s path=%s\n",
                host, (unsigned)port, resource, path);
        st = ipp_print_image(host, port, resource, path);
    }

    if (st == IPP_STATUS_OK || st == IPP_STATUS_OK_IGNORED) {
        kprintf("[PRINTIMG] Print-Job OK (status 0x%04x) - image submitted\n", st);
        kprintf("========== IMAGE PRINT SELFTEST: PASS ==========\n");
    } else {
        kprintf("[PRINTIMG] Print-Job FAILED (status %d)\n", st);
        kprintf("========== IMAGE PRINT SELFTEST: FAIL ==========\n");
    }
}

static void print_selftest_worker(void *arg) {
    (void)arg;
    proc_sleep(13000);  // let desktop + net stack settle (mirror SMB test)
    kprintf_set_dual_output(1);
    print_run_selftest();
    print_run_image_selftest();
    kprintf_set_dual_output(0);
}

void print_start_deferred_selftest(void) {
    extern int proc_create_ex(const char *name, void (*entry)(void *), void *arg,
                              int priority, uint32_t stack_size);
    // Only spawn if a gate file exists, to avoid an idle worker.
    uint32_t sz = 0;
    void *cfg = fat_read_file(&g_fat_fs, "/CONFIG/PRINTTEST.CFG", &sz);
    if (cfg) { kfree(cfg); }
    else {
        cfg = fat_read_file(&g_fat_fs, "/CONFIG/PRINTIMG.CFG", &sz);
        if (!cfg) return;
        kfree(cfg);
    }
    proc_create_ex("printtest", print_selftest_worker, 0, 1, 256 * 1024);
}
