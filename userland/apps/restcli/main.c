// restcli - HTTP Client: a REST request tester for MayteraOS (user-mode).
//
// Method + URL + up to four request headers + a body in, status + timing +
// response headers + body out, with a JSON pretty-printer, Copy to clipboard
// and a click-to-reload history. The kind of tool you reach for when poking a
// LAN service (Home Assistant, the App Store server, darkhttpd, a device's
// REST API) from inside the OS instead of from another machine.
//
// TRANSPORTS (all driven from the 50 ms event loop; the UI never blocks):
//   http://   any method, any headers, body, RESPONSE HEADERS and timing via the
//             userland async HTTP/1.1 client (lib/netapps/httpx.c) over the
//             non-blocking TCP syscalls.
//   https://  TLS exists only inside the kernel, so:
//               GET  without headers -> SYS_HTTP_FETCH_START/POLL/READ (async)
//               POST                 -> SYS_HTTP_POST_START/POLL/READ (async, headers + body)
//               GET  with headers    -> SYS_HTTP_FETCH_HDR, the kernel's BLOCKING
//                    header-capable GET, run on a worker pthread so the UI thread
//                    keeps servicing events (the App Store calls the same syscall
//                    on its main thread; this is the strictly-better variant).
//                    A blocked worker cannot be cancelled; a second HTTPS+headers
//                    send is refused until it returns (kernel timeouts bound it).
//               PUT/PATCH/DELETE/HEAD -> REFUSED with a message naming the gap:
//                    there is no userland TLS socket and the kernel exposes no
//                    other HTTPS verbs. Nothing here fabricates a response.
//             The kernel API returns status + body only, so the response-header
//             pane says so for HTTPS rather than inventing headers.
#include "../../lib/netapps/netui.h"
#include "../../lib/netapps/httpx.h"
#include "../../libc/gui_scroll.h"
#include "../../libc/gui_list.h"
#include "../../libc/pthread.h"

#define WIN_W 980
#define WIN_H 660
#define MIN_W 720
#define MIN_H 480

#define TOP_H     52
#define STATUS_H  24
#define PAD       10
#define LEFT_W    360
#define LINE_H    18
#define NHDR      4
#define HIST_N    16
#define HDR_LINES 6

#define RESP_CAP   (1024 * 1024 + 4096)
#define VIEW_CAP   (RESP_CAP + RESP_CAP / 2)
#define MAX_LINES  12000
#define BODY_CAP   HTTPX_BODY_MAX

static int  win = -1;
static int  g_w = WIN_W, g_h = WIN_H;
static int  g_theme_last = -1;
static int  g_hover = -1, g_pressed = -1;
static char g_status[200];

// ---- request fields ---------------------------------------------------------
static const char *g_methods[] = { "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD" };
#define NMETHODS 6
static int g_method = 0;

static char       g_urlbuf[HTTPX_URL_MAX];
static char       g_hdrbuf[NHDR][256];
static char       g_bodybuf[BODY_CAP];
static nu_field_t g_url, g_hdr[NHDR], g_body;
enum { F_URL = 0, F_H0, F_H1, F_H2, F_H3, F_BODY, F_COUNT };
static int g_focus = F_URL;
static int g_pretty = 1;

// ---- response ---------------------------------------------------------------
enum { RQ_IDLE = 0, RQ_KGET, RQ_KPOST, RQ_KHDR, RQ_RAW, RQ_DONE, RQ_ERROR };
static int  g_rq = RQ_IDLE;
static int  g_kjob = -1;
static unsigned long long g_t0_us, g_t1_us;
static char g_resp[RESP_CAP];
static unsigned g_resp_len;
static int  g_resp_trunc;
static int  g_resp_status;
static char g_resp_hdr[HTTPX_HDR_CAP + 1];   // raw block (http only)
static char g_via[64];
static char g_err[200];
static httpx_t g_hx;

// header-GET worker thread (https + request headers)
static volatile int g_thr_state;             // 0 idle, 1 running, 2 finished
static int          g_thr_rc, g_thr_status;
static unsigned int g_thr_bytes;
static char         g_thr_url[HTTPX_URL_MAX];
static char         g_thr_hdrs[NHDR * 256 + 16];
static pthread_t    g_thr;

// ---- body view ---------------------------------------------------------------
static char     g_view[VIEW_CAP];
static unsigned g_view_len;
static struct { unsigned off; unsigned short len; } g_lines[MAX_LINES];
static int g_nlines;
static int g_view_w_built = -1;
static gui_scroll_t g_vscroll;

// ---- history -----------------------------------------------------------------
typedef struct { char method[8]; char url[200]; int status; unsigned long ms; } hist_t;
static hist_t g_hist[HIST_N];
static int g_nhist;
static gui_list_t g_hlist;
static int g_hsel = -1;

static void set_status(const char *s) { snprintf(g_status, sizeof(g_status), "%s", s); }

static int ci_prefix(const char *s, const char *pfx) {
    while (*pfx) {
        char a = *s, b = *pfx;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (!a || a != b) return 0;
        s++; pfx++;
    }
    return 1;
}

// Join the non-empty header fields into CRLF lines. A field without ':' is
// ignored (reported once in the status bar) rather than sent malformed.
static int build_headers(char *out, int cap) {
    int n = 0, bad = 0;
    out[0] = 0;
    for (int i = 0; i < NHDR; i++) {
        const char *s = g_hdrbuf[i];
        while (*s == ' ') s++;
        if (!*s) continue;
        if (!strstr(s, ":")) { bad++; continue; }
        int w = snprintf(out + n, cap - n, "%s\r\n", s);
        if (w < 0 || w >= cap - n) break;
        n += w;
    }
    return bad;
}

static int is_json_start(const char *s, unsigned len) {
    unsigned i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    return i < len && (s[i] == '{' || s[i] == '[');
}

static int looks_binary(const char *s, unsigned len) {
    unsigned n = len < 512 ? len : 512, ctl = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0) return 1;
        if (c < 32 && c != '\r' && c != '\n' && c != '\t') ctl++;
    }
    return n > 0 && ctl * 10 > n;
}

// JSON re-indenter: structural characters outside strings get newlines and a
// two-space indent. Anything malformed still comes out readable; nothing is
// dropped except insignificant whitespace.
static void pretty_json(const char *s, unsigned len, char *out, unsigned cap, unsigned *outlen, int *trunc) {
    unsigned o = 0; int indent = 0, in_str = 0, esc = 0;
    *trunc = 0;
#define PUT(c) do { if (o + 1 >= cap) { *trunc = 1; goto done; } out[o++] = (c); } while (0)
#define NL() do { PUT('\n'); for (int k = 0; k < indent * 2; k++) PUT(' '); } while (0)
    for (unsigned i = 0; i < len; i++) {
        char c = s[i];
        if (in_str) {
            PUT(c);
            if (esc) esc = 0; else if (c == '\\') esc = 1; else if (c == '"') in_str = 0;
            continue;
        }
        switch (c) {
        case '"': in_str = 1; PUT(c); break;
        case '{': case '[': {
            // empty container stays on one line
            unsigned j = i + 1;
            while (j < len && (s[j] == ' ' || s[j] == '\n' || s[j] == '\r' || s[j] == '\t')) j++;
            if (j < len && (s[j] == '}' || s[j] == ']')) { PUT(c); PUT(s[j]); i = j; break; }
            PUT(c); indent++; NL(); break;
        }
        case '}': case ']': if (indent > 0) indent--; NL(); PUT(c); break;
        case ',': PUT(c); NL(); break;
        case ':': PUT(c); PUT(' '); break;
        case ' ': case '\t': case '\r': case '\n': break;
        default: PUT(c); break;
        }
    }
done:
    out[o] = 0;
    *outlen = o;
#undef PUT
#undef NL
}

static void hex_dump(const char *s, unsigned len, char *out, unsigned cap, unsigned *outlen) {
    static const char hx[] = "0123456789abcdef";
    unsigned o = 0, n = len < 4096 ? len : 4096;
    for (unsigned i = 0; i < n; i += 16) {
        if (o + 80 >= cap) break;
        int w = snprintf(out + o, cap - o, "%08x  ", i); o += (unsigned)w;
        for (unsigned k = 0; k < 16; k++) {
            if (i + k < n) { unsigned char c = (unsigned char)s[i + k]; out[o++] = hx[c >> 4]; out[o++] = hx[c & 15]; out[o++] = ' '; }
            else { out[o++] = ' '; out[o++] = ' '; out[o++] = ' '; }
            if (k == 7) out[o++] = ' ';
        }
        out[o++] = ' ';
        for (unsigned k = 0; k < 16 && i + k < n; k++) { unsigned char c = (unsigned char)s[i + k]; out[o++] = (c >= 32 && c < 127) ? (char)c : '.'; }
        out[o++] = '\n';
    }
    if (n < len && o + 64 < cap) { int w = snprintf(out + o, cap - o, "... %u more bytes not shown\n", len - n); o += (unsigned)w; }
    out[o] = 0;
    *outlen = o;
}

// Rebuild the display text (pretty / hex / raw) and the wrapped line index.
static void rebuild_view(int width_px) {
    g_view_w_built = width_px;
    int trunc = 0;
    if (g_resp_len == 0) { g_view[0] = 0; g_view_len = 0; }
    else if (looks_binary(g_resp, g_resp_len)) hex_dump(g_resp, g_resp_len, g_view, VIEW_CAP, &g_view_len);
    else if (g_pretty && is_json_start(g_resp, g_resp_len)) pretty_json(g_resp, g_resp_len, g_view, VIEW_CAP, &g_view_len, &trunc);
    else { unsigned n = g_resp_len < VIEW_CAP - 1 ? g_resp_len : VIEW_CAP - 1; memcpy(g_view, g_resp, n); g_view[n] = 0; g_view_len = n; }
    // wrap
    g_nlines = 0;
    unsigned i = 0;
    int maxw = width_px - 2 * PAD;
    if (maxw < 40) maxw = 40;
    while (i < g_view_len && g_nlines < MAX_LINES) {
        unsigned start = i; int w = 0;
        while (i < g_view_len && g_view[i] != '\n') {
            char c = g_view[i];
            int cw = (c == '\t') ? 4 * nu_char_w(' ') : nu_char_w(c);
            if (w + cw > maxw && i > start) break;
            w += cw; i++;
            if (i - start >= 500) break;
        }
        g_lines[g_nlines].off = start;
        g_lines[g_nlines].len = (unsigned short)(i - start);
        g_nlines++;
        if (i < g_view_len && g_view[i] == '\n') i++;
    }
    if (trunc || (i < g_view_len && g_nlines >= MAX_LINES)) g_resp_trunc = 1;
}

// ---- history -------------------------------------------------------------------
static void hist_push(const char *method, const char *url, int status, unsigned long ms) {
    if (g_nhist == HIST_N) { for (int i = HIST_N - 1; i > 0; i--) g_hist[i] = g_hist[i - 1]; g_nhist--; }
    else for (int i = g_nhist; i > 0; i--) g_hist[i] = g_hist[i - 1];
    hist_t *h = &g_hist[0];
    snprintf(h->method, sizeof(h->method), "%s", method);
    snprintf(h->url, sizeof(h->url), "%s", url);
    h->status = status; h->ms = ms;
    g_nhist++;
    g_hsel = -1;
}

static const char *hist_label(void *ctx, int index, char *buf, int cap) {
    (void)ctx;
    if (index < 0 || index >= g_nhist) return "";
    hist_t *h = &g_hist[index];
    const char *u = h->url;
    const char *sch = strstr(u, "://");
    if (sch) u = sch + 3;
    if (h->status > 0) snprintf(buf, cap, "%s %d  %lu ms  %s", h->method, h->status, h->ms, u);
    else snprintf(buf, cap, "%s failed  %s", h->method, u);
    return buf;
}

// ---- sending -------------------------------------------------------------------
static void raw_sink(void *ctx, const unsigned char *d, unsigned len) {
    (void)ctx;
    unsigned room = RESP_CAP - 1 - g_resp_len;
    if (len > room) { len = room; g_resp_trunc = 1; }
    if (len) { memcpy(g_resp + g_resp_len, d, len); g_resp_len += len; g_resp[g_resp_len] = 0; }
}

static void *hdr_worker(void *arg) {
    (void)arg;
    g_thr_bytes = 0; g_thr_status = 0;
    g_thr_rc = sys_http_fetch_hdr(g_thr_url, g_thr_hdrs, g_resp, RESP_CAP - 1, &g_thr_bytes, &g_thr_status);
    g_thr_state = 2;        // published last: the UI reads rc/status/bytes only after seeing 2
    return 0;
}

static void finish_ok(int status) {
    g_t1_us = mono_us();
    g_resp_status = status;
    g_rq = RQ_DONE;
    g_view_w_built = -1;
    g_vscroll.offset = 0;
    unsigned long ms = (unsigned long)((g_t1_us - g_t0_us) / 1000ULL);
    hist_push(g_methods[g_method], g_urlbuf, status, ms);
    char sz[24]; nu_fmt_bytes(g_resp_len, sz, sizeof(sz));
    char m[160];
    snprintf(m, sizeof(m), "%d %s in %lu ms, %s%s", status, httpx_status_text(status), ms, sz, g_resp_trunc ? " (truncated)" : "");
    set_status(m);
}

static void finish_err(const char *why) {
    g_t1_us = mono_us();
    snprintf(g_err, sizeof(g_err), "%s", why);
    g_rq = RQ_ERROR;
    g_resp_len = 0; g_resp[0] = 0; g_view_w_built = -1;
    hist_push(g_methods[g_method], g_urlbuf, 0, (unsigned long)((g_t1_us - g_t0_us) / 1000ULL));
    char m[220]; snprintf(m, sizeof(m), "Failed: %s", why); set_status(m);
}

static void cancel_inflight(void) {
    if (g_rq == RQ_KGET && g_kjob >= 0) { http_fetch_cancel(g_kjob); g_kjob = -1; }
    if (g_rq == RQ_KPOST && g_kjob >= 0) { http_post_cancel(g_kjob); g_kjob = -1; }
    if (g_rq == RQ_RAW) httpx_cancel(&g_hx);
    // RQ_KHDR: the worker is inside a blocking syscall; it cannot be interrupted.
}

static void send_request(void) {
    char url[HTTPX_URL_MAX];
    const char *s = g_urlbuf; while (*s == ' ') s++;
    snprintf(url, sizeof(url), "%s", s);
    int l = (int)strlen(url); while (l > 0 && url[l - 1] == ' ') url[--l] = 0;
    if (!l) { set_status("Enter a URL first"); return; }
    if (!ci_prefix(url, "http://") && !ci_prefix(url, "https://")) {
        char t[HTTPX_URL_MAX]; snprintf(t, sizeof(t), "http://%s", url); snprintf(url, sizeof(url), "%s", t);
        tf_set_text(&g_url.tf, url); g_url.start = 0;
    }
    if (g_rq == RQ_KHDR && g_thr_state == 1) {
        set_status("Previous HTTPS request is still inside the kernel (blocking header GET cannot be cancelled); wait for it");
        return;
    }
    if (g_rq == RQ_KGET || g_rq == RQ_KPOST || g_rq == RQ_RAW) cancel_inflight();
    if (!sys_net_is_up()) { g_t0_us = mono_us(); finish_err("Network is down"); return; }

    char hdrs[NHDR * 256 + 16];
    int bad = build_headers(hdrs, sizeof(hdrs));
    const char *method = g_methods[g_method];
    unsigned blen = (unsigned)strlen(g_bodybuf);

    g_resp_len = 0; g_resp[0] = 0; g_resp_trunc = 0; g_resp_hdr[0] = 0; g_resp_status = 0; g_err[0] = 0;
    g_t0_us = mono_us();
    g_view_w_built = -1;

    if (ci_prefix(url, "https://")) {
        if (strcmp(method, "POST") == 0) {
            g_kjob = http_post_start(url, hdrs, g_bodybuf);
            if (g_kjob < 0) {
                finish_err(g_kjob == NET_ERR_FAULTY ? "Network marked faulty by the kernel breaker; retry shortly"
                         : "Could not start the HTTPS POST");
                return;
            }
            g_rq = RQ_KPOST; snprintf(g_via, sizeof(g_via), "https via kernel async POST");
        } else if (strcmp(method, "GET") == 0 && hdrs[0] == 0) {
            g_kjob = http_fetch_start(url);
            if (g_kjob < 0) { finish_err(g_kjob == NET_ERR_FAULTY ? "Network marked faulty by the kernel breaker; retry shortly" : "Could not start the HTTPS fetch"); return; }
            g_rq = RQ_KGET; snprintf(g_via, sizeof(g_via), "https via kernel async GET");
        } else if (strcmp(method, "GET") == 0) {
            snprintf(g_thr_url, sizeof(g_thr_url), "%s", url);
            snprintf(g_thr_hdrs, sizeof(g_thr_hdrs), "%s", hdrs);
            g_thr_state = 1;
            if (pthread_create(&g_thr, 0, hdr_worker, 0) != 0) { g_thr_state = 0; finish_err("Could not start the worker thread"); return; }
            pthread_detach(g_thr);
            g_rq = RQ_KHDR; snprintf(g_via, sizeof(g_via), "https via kernel header GET (worker thread)");
        } else {
            char m[200];
            snprintf(m, sizeof(m), "%s over https:// is not available: no userland TLS socket, and the kernel exposes only GET and POST for HTTPS", method);
            finish_err(m);
            return;
        }
    } else {
        if (httpx_start(&g_hx, method, url, hdrs, g_bodybuf, blen, 0, raw_sink, 0) != 0) { finish_err(g_hx.err); return; }
        g_rq = RQ_RAW; snprintf(g_via, sizeof(g_via), "http via userland HTTP/1.1 (raw TCP)");
    }
    char m[120];
    snprintf(m, sizeof(m), "Sending %s%s", method, bad ? " (a header without ':' was skipped)" : "");
    set_status(m);
}

static void poll_request(void) {
    if (g_rq == RQ_KGET || g_rq == RQ_KPOST) {
        int status = 0; unsigned int len = 0;
        int st = (g_rq == RQ_KGET) ? http_fetch_poll(g_kjob, &status, &len) : http_post_poll(g_kjob, &status, &len);
        if (st == 0) return;
        if (st == 1) {
            int n = (g_rq == RQ_KGET) ? http_fetch_read(g_kjob, g_resp, RESP_CAP - 1) : http_post_read(g_kjob, g_resp, RESP_CAP - 1);
            g_kjob = -1;
            if (n < 0) n = 0;
            g_resp_len = (unsigned)n; g_resp[g_resp_len] = 0;
            if (len > (unsigned)n) g_resp_trunc = 1;
            finish_ok(status);
            return;
        }
        if (g_rq == RQ_KGET) http_fetch_read(g_kjob, g_resp, 0); else http_post_read(g_kjob, g_resp, 0);
        g_kjob = -1;
        char name[80] = ""; net_last_error(name, sizeof(name));
        char m[200];
        if (name[0]) snprintf(m, sizeof(m), "%s%s%s", name, net_error_advice(name)[0] ? ": " : "", net_error_advice(name));
        else snprintf(m, sizeof(m), st == 2 ? "request failed" : "kernel job lost");
        finish_err(m);
        return;
    }
    if (g_rq == RQ_KHDR) {
        if (g_thr_state != 2) return;
        g_thr_state = 0;
        if (g_thr_rc < 0) {
            char name[80] = ""; net_last_error(name, sizeof(name));
            char m[200]; snprintf(m, sizeof(m), "%s", name[0] ? name : "request failed");
            finish_err(m);
            return;
        }
        g_resp_len = g_thr_bytes < RESP_CAP - 1 ? g_thr_bytes : RESP_CAP - 1;
        g_resp[g_resp_len] = 0;
        finish_ok(g_thr_status);
        return;
    }
    if (g_rq == RQ_RAW) {
        int r = httpx_poll(&g_hx);
        if (r == HTTPX_RUNNING) return;
        if (r == HTTPX_DONE) {
            snprintf(g_resp_hdr, sizeof(g_resp_hdr), "%s", g_hx.hdr);
            finish_ok(g_hx.status);
        } else finish_err(g_hx.err);
    }
}

// ---- drawing ---------------------------------------------------------------------
enum { B_METHOD = 1, B_SEND, B_COPY, B_PRETTY, B_CLEAR };
static nu_btn_t g_btn[6];
static int g_nbtn;
static int resp_x, resp_y, resp_w, resp_h, body_y, body_h;

static void draw_all(void) {
    win_get_size(win, &g_w, &g_h);
    if (g_w < MIN_W) g_w = MIN_W;
    if (g_h < MIN_H) g_h = MIN_H;
    nu_apply_style();
    gui_palette_t *p = gui_pal();
    unsigned int content = nu_content(), toolbar = nu_toolbar();
    unsigned int ink = nu_fg(content), dim = nu_dim(content);
    int busy = (g_rq == RQ_KGET || g_rq == RQ_KPOST || g_rq == RQ_KHDR || g_rq == RQ_RAW);

    win_draw_rect(win, 0, 0, g_w, g_h, content);

    // toolbar: [METHOD] [url................] [Send] [Copy] [Pretty]
    win_draw_rect(win, 0, 0, g_w, TOP_H, toolbar);
    win_draw_rect(win, 0, TOP_H - 1, g_w, 1, nu_border());
    g_nbtn = 0;
    int by = (TOP_H - NU_BTN_H) / 2;
    int mw = 92;
    g_btn[g_nbtn++] = (nu_btn_t){ PAD, by, mw, NU_BTN_H, g_methods[g_method], GUI_BTN_SECONDARY, 1, B_METHOD };
    int sendw = nu_btn_w("Send"), copyw = nu_btn_w("Copy"), prw = nu_btn_w("Pretty: off"), clw = nu_btn_w("Clear");
    int x = g_w - PAD - clw;
    g_btn[g_nbtn++] = (nu_btn_t){ x, by, clw, NU_BTN_H, "Clear", GUI_BTN_GHOST, 1, B_CLEAR };
    x -= GUI_GAP + prw;
    g_btn[g_nbtn++] = (nu_btn_t){ x, by, prw, NU_BTN_H, g_pretty ? "Pretty: on" : "Pretty: off", GUI_BTN_SECONDARY, 1, B_PRETTY };
    x -= GUI_GAP + copyw;
    g_btn[g_nbtn++] = (nu_btn_t){ x, by, copyw, NU_BTN_H, "Copy", GUI_BTN_SECONDARY, g_rq == RQ_DONE && g_resp_len > 0, B_COPY };
    x -= GUI_GAP + sendw;
    g_btn[g_nbtn++] = (nu_btn_t){ x, by, sendw, NU_BTN_H, busy ? "..." : "Send", GUI_BTN_PRIMARY, 1, B_SEND };
    int fx = PAD + mw + GUI_GAP;
    nu_field_draw(win, &g_url, fx, (TOP_H - NU_FIELD_H) / 2, x - GUI_GAP - fx, NU_FIELD_H, g_focus == F_URL, "http://host/path or https://host/path");
    for (int i = 0; i < g_nbtn; i++) nu_btn_draw(win, &g_btn[i], g_hover, g_pressed);

    // left column
    int lx = PAD, ly = TOP_H + PAD, lw = LEFT_W;
    int hdr_card_h = 30 + NHDR * (NU_FIELD_H + 6) + 6;
    nu_card_titled(win, lx, ly, lw, hdr_card_h, "REQUEST HEADERS  (Name: value)");
    for (int i = 0; i < NHDR; i++) {
        static const char *ph[NHDR] = { "Authorization: Bearer ...", "Content-Type: application/json", "Accept: application/json", "X-Custom: value" };
        nu_field_draw(win, &g_hdr[i], lx + PAD, ly + 28 + i * (NU_FIELD_H + 6), lw - 2 * PAD, NU_FIELD_H, g_focus == F_H0 + i, ph[i]);
    }
    int byy = ly + hdr_card_h + PAD;
    int body_card_h = 30 + NU_FIELD_H + 24;
    nu_card_titled(win, lx, byy, lw, body_card_h, "BODY  (sent with POST, PUT, PATCH)");
    nu_field_draw(win, &g_body, lx + PAD, byy + 28, lw - 2 * PAD, NU_FIELD_H, g_focus == F_BODY, "{\"key\": \"value\"}");
    { char n[48]; snprintf(n, sizeof(n), "%u of %u bytes", (unsigned)strlen(g_bodybuf), (unsigned)BODY_CAP - 1);
      win_draw_text_ttf(win, lx + PAD, byy + 28 + NU_FIELD_H + 4, n, NU_TTF_SMALL, dim); }
    int hy = byy + body_card_h + PAD;
    int hh = g_h - STATUS_H - PAD - hy;
    if (hh < 60) hh = 60;
    nu_card_titled(win, lx, hy, lw, hh, "HISTORY  (click to reload method + URL)");
    gui_list_config(&g_hlist, lx + PAD, hy + 26, lw - 2 * PAD, hh - 26 - PAD, 20, g_nhist);
    if (g_nhist) gui_list_draw(win, &g_hlist, g_hsel, p->field_bg, p->border, gui_ink_on(p->field_bg), nu_sel(), nu_fg(nu_sel()), hist_label, 0);
    else { gui_fill_rounded(win, g_hlist.x, g_hlist.y, g_hlist.w, g_hlist.h, 0, p->field_bg); gui_rounded_border(win, g_hlist.x, g_hlist.y, g_hlist.w, g_hlist.h, 0, p->border);
           win_draw_text_ttf(win, g_hlist.x + PAD, g_hlist.y + 8, "No requests yet", NU_TTF_SMALL, dim); }

    // response column
    resp_x = lx + lw + PAD; resp_y = TOP_H + PAD;
    resp_w = g_w - resp_x - PAD; resp_h = g_h - STATUS_H - PAD - resp_y;
    gui_card(win, resp_x, resp_y, resp_w, resp_h);
    int ry = resp_y + 8;
    if (g_rq == RQ_IDLE) {
        win_draw_text_ttf(win, resp_x + PAD, ry, "RESPONSE", NU_TTF_SMALL, dim);
        ry += 26;
        const char *l[] = {
            "Pick a method, type a URL, press Send (or Enter).",
            "http://  any method, request and response headers, timing (userland HTTP/1.1).",
            "https://  GET and POST through the kernel TLS fetch; other verbs are not available.",
            "Tab moves between fields. Pretty re-indents JSON bodies. Copy puts the body on the clipboard.",
        };
        for (unsigned i = 0; i < sizeof(l) / sizeof(l[0]); i++) nu_text_fit(win, resp_x + PAD, ry + (int)i * 22, l[i], resp_w - 2 * PAD, i == 0 ? ink : dim);
        body_y = ry + 100; body_h = 0;
    } else if (busy) {
        win_draw_text_ttf(win, resp_x + PAD, ry, "RESPONSE", NU_TTF_SMALL, dim);
        char m[160];
        const char *ph = (g_rq == RQ_RAW) ? httpx_phase_name(&g_hx) : (g_rq == RQ_KHDR ? "Waiting for the kernel (blocking header GET)" : "Waiting for the kernel job");
        char el[24]; nu_fmt_ms(mono_us() - g_t0_us, el, sizeof(el));
        snprintf(m, sizeof(m), "%s...  %s  (%s)", ph, el, g_via);
        nu_text_fit(win, resp_x + PAD, ry + 26, m, resp_w - 2 * PAD, ink);
        if (g_rq == RQ_RAW && g_hx.bytes_body) { char b[24]; nu_fmt_bytes(g_hx.bytes_body, b, sizeof(b)); snprintf(m, sizeof(m), "%s received", b); win_draw_text_ttf(win, resp_x + PAD, ry + 48, m, NU_TTF_SMALL, dim); }
        body_h = 0;
    } else if (g_rq == RQ_ERROR) {
        win_draw_text_ttf(win, resp_x + PAD, ry, "RESPONSE", NU_TTF_SMALL, dim);
        char el[24]; nu_fmt_ms(g_t1_us - g_t0_us, el, sizeof(el));
        char m[64]; snprintf(m, sizeof(m), "Request failed after %s", el);
        win_draw_text_ttf(win, resp_x + PAD, ry + 26, m, NU_TTF_BODY, nu_on(NU_ERR_RAW, content));
        // wrap the error text roughly
        int maxw = resp_w - 2 * PAD; const char *e = g_err; int line = 0;
        while (*e && line < 6) {
            char buf[200]; int n = 0, w = 0, lastsp = -1;
            while (e[n] && n < (int)sizeof(buf) - 1) { int cw = nu_char_w(e[n]); if (w + cw > maxw) break; if (e[n] == ' ') lastsp = n; w += cw; n++; }
            if (e[n] && lastsp > 0) n = lastsp + 1;
            memcpy(buf, e, (unsigned)n); buf[n] = 0;
            win_draw_text_ttf(win, resp_x + PAD, ry + 52 + line * 20, buf, NU_TTF_BODY, ink);
            e += n; line++;
        }
        body_h = 0;
    } else { // RQ_DONE
        char st[80], el[24], sz[24];
        nu_fmt_ms(g_t1_us - g_t0_us, el, sizeof(el));
        nu_fmt_bytes(g_resp_len, sz, sizeof(sz));
        snprintf(st, sizeof(st), "%d %s", g_resp_status, httpx_status_text(g_resp_status));
        unsigned int scol = g_resp_status >= 500 ? nu_on(NU_ERR_RAW, content) : g_resp_status >= 400 ? nu_on(NU_WARN_RAW, content) :
                            g_resp_status >= 300 ? nu_on(nu_acc(), content) : nu_on(NU_OK_RAW, content);
        win_draw_text_ttf(win, resp_x + PAD, ry, st, 18, scol);
        int sx = resp_x + PAD + nu_text_w(st, 18) + 16;
        char meta[160]; snprintf(meta, sizeof(meta), "%s   %s%s   %s", el, sz, g_resp_trunc ? " (truncated)" : "", g_via);
        nu_text_fit(win, sx, ry + 4, meta, resp_x + resp_w - PAD - sx, dim);
        ry += 30;
        // response headers
        int hl = 0;
        if (g_resp_hdr[0]) {
            const char *q = g_resp_hdr;
            while (*q && *q != '\n') q++;                            // skip status line
            if (*q == '\n') q++;
            int total = 0; for (const char *c = q; *c; c++) if (*c == '\n') total++;
            while (*q && hl < HDR_LINES) {
                const char *e = q; while (*e && *e != '\n') e++;
                int n = (int)(e - q); while (n > 0 && (q[n - 1] == '\r')) n--;
                if (n > 0) { char buf[200]; if (n > 199) n = 199; memcpy(buf, q, (unsigned)n); buf[n] = 0;
                             nu_text_fit(win, resp_x + PAD, ry + hl * 16, buf, resp_w - 2 * PAD, dim); hl++; }
                q = *e ? e + 1 : e;
            }
            if (total > HDR_LINES && hl == HDR_LINES) { char more[48]; snprintf(more, sizeof(more), "... %d more headers", total - HDR_LINES); win_draw_text_ttf(win, resp_x + PAD, ry + hl * 16, more, NU_TTF_SMALL, dim); hl++; }
        } else {
            win_draw_text_ttf(win, resp_x + PAD, ry, "(response headers are not exposed by the kernel HTTPS fetch API; body only)", NU_TTF_SMALL, dim);
            hl = 1;
        }
        ry += hl * 16 + 8;
        win_draw_rect(win, resp_x + 1, ry, resp_w - 2, 1, nu_border());
        ry += 6;
        body_y = ry; body_h = resp_y + resp_h - ry - 6;
        int inner_w = resp_w - 2 - GUI_SCROLL_W;
        if (g_view_w_built != inner_w) rebuild_view(inner_w);
        gui_scroll_config(&g_vscroll, resp_x + resp_w - 1 - GUI_SCROLL_W, body_y, GUI_SCROLL_W, body_h, g_nlines * LINE_H, LINE_H);
        int first = g_vscroll.offset / LINE_H;
        int y = body_y;
        if (g_nlines == 0) win_draw_text_ttf(win, resp_x + PAD, y, "(empty body)", NU_TTF_BODY, dim);
        for (int i = first; i < g_nlines && y + LINE_H <= body_y + body_h; i++, y += LINE_H) {
            char buf[512]; unsigned n = g_lines[i].len; if (n > 511) n = 511;
            // expand tabs, mask control bytes
            unsigned o = 0; for (unsigned k = 0; k < n && o < 511; k++) { char c = g_view[g_lines[i].off + k]; if (c == '\t') { for (int t = 0; t < 4 && o < 511; t++) buf[o++] = ' '; } else buf[o++] = ((unsigned char)c < 32) ? '.' : c; }
            buf[o] = 0;
            win_draw_text_ttf(win, resp_x + PAD, y, buf, NU_TTF_BODY, ink);
        }
        if (g_nlines * LINE_H > body_h) gui_scroll_draw(win, &g_vscroll);
    }

    // status bar
    int sy = g_h - STATUS_H;
    win_draw_rect(win, 0, sy, g_w, STATUS_H, toolbar);
    win_draw_rect(win, 0, sy, g_w, 1, nu_border());
    nu_text_fit(win, PAD, sy + 4, g_status[0] ? g_status : "Enter: send   Tab: next field   Esc: quit", (g_w - 140) * 10 / 8, nu_fg(toolbar));
    const char *net = sys_net_is_up() ? "network up" : "network down";
    win_draw_text_ttf(win, g_w - PAD - nu_text_w(net, NU_TTF_SMALL) * 8 / 10 - 4, sy + 4, net, NU_TTF_SMALL,
                      sys_net_is_up() ? nu_on(NU_OK_RAW, toolbar) : nu_on(NU_ERR_RAW, toolbar));
}

// ---- input ------------------------------------------------------------------------
static nu_field_t *field_of(int f) {
    if (f == F_URL) return &g_url;
    if (f == F_BODY) return &g_body;
    return &g_hdr[f - F_H0];
}

static int button_at(int mx, int my) {
    for (int i = 0; i < g_nbtn; i++) if (nu_btn_hit(&g_btn[i], mx, my)) return g_btn[i].id;
    return -1;
}

static void do_copy(void) {
    if (g_rq != RQ_DONE || g_resp_len == 0) return;
    if (g_view_w_built < 0) rebuild_view(resp_w - 2 - GUI_SCROLL_W);
    int r = clipboard_set(g_view, (int)g_view_len);
    char m[80];
    if (r >= 0) snprintf(m, sizeof(m), "Copied %u bytes to the clipboard", g_view_len);
    else snprintf(m, sizeof(m), "Clipboard refused %u bytes", g_view_len);
    set_status(m);
}

static void do_button(int id) {
    switch (id) {
    case B_METHOD: g_method = (g_method + 1) % NMETHODS; break;
    case B_SEND:   send_request(); break;
    case B_COPY:   do_copy(); break;
    case B_PRETTY: g_pretty = !g_pretty; g_view_w_built = -1; g_vscroll.offset = 0; break;
    case B_CLEAR:
        cancel_inflight();
        if (g_rq != RQ_KHDR) g_rq = RQ_IDLE;
        g_resp_len = 0; g_resp[0] = 0; g_view_w_built = -1; g_status[0] = 0;
        break;
    default: break;
    }
}

static void handle_click(int mx, int my) {
    int id = button_at(mx, my);
    if (id > 0) { g_pressed = id; do_button(id); return; }
    for (int f = 0; f < F_COUNT; f++) {
        nu_field_t *fl = field_of(f);
        if (nu_field_hit(fl, mx, my)) { g_focus = f; nu_field_click(fl, mx); return; }
    }
    if (g_nhist) {
        int hit = gui_list_press(&g_hlist, mx, my);
        if (hit >= 0 && hit < g_nhist) {
            g_hsel = hit;
            hist_t *h = &g_hist[hit];
            for (int i = 0; i < NMETHODS; i++) if (strcmp(g_methods[i], h->method) == 0) g_method = i;
            tf_set_text(&g_url.tf, h->url); g_url.start = 0;
            g_focus = F_URL;
            return;
        }
        if (gui_list_hit(&g_hlist, mx, my)) return;   // scrollbar or empty row: consumed by the list
    }
    if (g_rq == RQ_DONE && body_h > 0) gui_scroll_press(&g_vscroll, mx, my);
}

int main(int argc, char **argv) {
    nu_field_init(&g_url, g_urlbuf, sizeof(g_urlbuf));
    for (int i = 0; i < NHDR; i++) nu_field_init(&g_hdr[i], g_hdrbuf[i], sizeof(g_hdrbuf[i]));
    nu_field_init(&g_body, g_bodybuf, sizeof(g_bodybuf));
    if (argc >= 2 && argv[1] && argv[1][0]) tf_set_text(&g_url.tf, argv[1]);

    win = win_create("HTTP Client", 80, 50, WIN_W, WIN_H);
    if (win < 0) return 1;
    g_theme_last = theme_get_active();
    draw_all(); win_invalidate(win);

    int running = 1;
    while (running) {
        int th = theme_get_active();
        int theme_changed = (th != g_theme_last);
        if (theme_changed) g_theme_last = th;

        gui_event_t ev;
        int ret = win_get_event(win, &ev, 50);
        int need_draw = ret > 0 || theme_changed;
        if (ret > 0) {
            switch (ev.type) {
            case EVENT_WINDOW_CLOSE: running = 0; break;
            case EVENT_KEY_DOWN: {
                nu_field_t *fl = field_of(g_focus);
                if (ev.keycode == 0x01) { running = 0; break; }                                    // Esc
                if (ev.keycode == 0x1C || ev.key_char == '\n' || ev.key_char == '\r') { send_request(); break; }
                if (ev.keycode == GUI_KEY_F5) { send_request(); break; }
                if (ev.keycode == 0x0F || ev.key_char == '\t') { g_focus = (g_focus + 1) % F_COUNT; break; }   // Tab
                if (ev.key_char == 12) { g_focus = F_URL; tf_select_all(&g_url.tf); break; }                 // Ctrl+L
                if (ev.keycode == GUI_KEY_PGUP || ev.keycode == GUI_KEY_PGDN) { if (g_rq == RQ_DONE) gui_scroll_key(&g_vscroll, ev.keycode); break; }
                if ((ev.keycode == GUI_KEY_UP || ev.keycode == GUI_KEY_DOWN) && g_rq == RQ_DONE) { gui_scroll_by(&g_vscroll, ev.keycode == GUI_KEY_UP ? -LINE_H : LINE_H); break; }
                tf_handle_key(&fl->tf, &ev);
                break;
            }
            case EVENT_MOUSE_DOWN: handle_click(ev.mouse_x, ev.mouse_y); break;
            case EVENT_MOUSE_UP:
                g_pressed = -1;
                gui_scroll_release(&g_vscroll);
                gui_list_release(&g_hlist);
                break;
            case EVENT_MOUSE_MOVE: {
                int hv = button_at(ev.mouse_x, ev.mouse_y);
                int moved = 0;
                if (g_rq == RQ_DONE) moved |= gui_scroll_motion(&g_vscroll, ev.mouse_x, ev.mouse_y);
                if (g_nhist) moved |= gui_list_motion(&g_hlist, ev.mouse_x, ev.mouse_y);
                if (hv == g_hover && !moved) need_draw = 0;
                g_hover = hv;
                break;
            }
            case EVENT_MOUSE_SCROLL:
                if (g_nhist && gui_list_wheel(&g_hlist, ev.mouse_x, ev.mouse_y, ev.scroll_delta)) break;
                if (g_rq == RQ_DONE && ev.mouse_x >= resp_x) gui_scroll_wheel(&g_vscroll, ev.scroll_delta);
                break;
            default: break;
            }
        }
        int busy = (g_rq == RQ_KGET || g_rq == RQ_KPOST || g_rq == RQ_KHDR || g_rq == RQ_RAW);
        if (busy) poll_request();
        if (need_draw || busy) { draw_all(); win_invalidate(win); }
    }

    cancel_inflight();
    win_destroy(win);
    return 0;
}
