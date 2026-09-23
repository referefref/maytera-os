// dlmgr - Downloads: a queued download manager for MayteraOS (user-mode).
//
// WHAT IT DOES. Paste a URL, press Add; the transfer is queued (two run at a
// time), shows live progress (bytes, total, rate, phase), and lands in the
// user's Downloads folder (<home>/DOWNLOAD, the skeleton folder Files already
// shows as "Downloads"). Pause / Resume / Retry / Remove, a persisted queue
// (DLMGR.QUE in that folder) so finished and failed entries survive a restart,
// and an "Open folder" button that opens Files on the folder.
//
// TWO TRANSPORTS, BOTH ASYNC, CHOSEN BY SCHEME.
//   https://  the kernel's async TLS fetch (SYS_HTTP_FETCH_START/POLL/PROGRESS/
//             READ, #277/#25). This is the ONLY Ring 3 path to TLS. The kernel
//             holds the whole body in one buffer capped at WGET_BUFFER_SIZE
//             (1 MB, kernel/net/wget.h), so an https download larger than that
//             is REFUSED UP FRONT with a message naming the ceiling, rather than
//             silently truncated. It also cannot resume (the job has no Range).
//   http://   userland HTTP/1.1 over the non-blocking TCP syscalls
//             (lib/netapps/httpx.c): the body streams to disk as it arrives,
//             any size, with Range resume after Pause or a dropped connection.
//
// NEVER BLOCKS THE UI. Every network step is a poll from the 50 ms
// win_get_event() loop: http_fetch_poll()/http_fetch_progress() for the kernel
// path, httpx_poll() (bounded non-blocking recv rounds) for the raw path. There
// is no wait, sleep or spin on a remote anywhere in this file (#420/#549).
#include "../../lib/netapps/netui.h"
#include "../../lib/netapps/httpx.h"
#include "../../libc/gui_scroll.h"
#include "../../libc/pwd.h"
#include "../../libc/unistd.h"
#include "../../libc/sys/stat.h"

#define WIN_W 820
#define WIN_H 540
#define MIN_W 560
#define MIN_H 340

#define TOP_H     52
#define ACT_H     40
#define STATUS_H  24
#define ROW_H     50
#define PAD       10

#define MAX_DL     24
#define MAX_ACTIVE 2          // kernel async table has 6 slots shared by every app; stay polite
#define RAW_POOL   MAX_ACTIVE

// One kernel fetch = one WGET_BUFFER_SIZE (1 MB) buffer INCLUDING headers; the
// App Store uses the same 1000 KB working ceiling (DL_SMALL_MAX).
#define KFETCH_CEILING (1000UL * 1024UL)
#define KBUF_CAP       (1024 * 1024 + 8192)
static char g_kbuf[KBUF_CAP];

enum { TR_KERNEL = 0, TR_RAW = 1 };
enum { DS_QUEUED = 0, DS_RUNNING, DS_PAUSED, DS_DONE, DS_ERROR };

typedef struct {
    int  used;
    char url[HTTPX_URL_MAX];
    char name[80];
    char path[256];
    int  state;
    int  transport;
    unsigned long bytes;      // bytes on disk / received so far
    unsigned long total;      // 0 = unknown
    int  kjob;                // kernel async job id, or -1
    int  kphase;              // HTTP_PHASE_* from http_fetch_progress
    int  hx;                  // index into g_hx, or -1
    FILE *fp;
    int  discard;             // raw sink: response is not a body we keep
    int  write_err;
    int  want_resume;         // next start should Range from `bytes`
    char msg[120];
    unsigned long long t0_us, t_rate_us;
    unsigned long rate_base, rate_bps;
} dl_t;

static dl_t    g_dl[MAX_DL];
static int     g_ndl;
static httpx_t g_hx[RAW_POOL];
static int     g_hx_busy[RAW_POOL];

static int  win = -1;
static int  g_w = WIN_W, g_h = WIN_H;
static char g_dir[200];
static char g_quefile[256];
static int  g_sel = -1;
static int  g_hover = -1, g_pressed = -1;
static int  g_theme_last = -1;
static char g_status[160];
static unsigned long long g_status_until_us;

static char       g_urlbuf[HTTPX_URL_MAX];
static nu_field_t g_url;
static gui_scroll_t g_scroll;
static int        g_last_finished = -1;

// kernel http_progress.h phases (mirrored by the browser the same way)
#define HTTP_PHASE_RESOLVING  1
#define HTTP_PHASE_CONNECTING 2
#define HTTP_PHASE_TLS        3
#define HTTP_PHASE_SENDING    4
#define HTTP_PHASE_RECEIVING  5

static const char *kphase_name(int ph) {
    switch (ph) {
    case HTTP_PHASE_RESOLVING:  return "Resolving host";
    case HTTP_PHASE_CONNECTING: return "Connecting";
    case HTTP_PHASE_TLS:        return "TLS handshake";
    case HTTP_PHASE_SENDING:    return "Sending request";
    case HTTP_PHASE_RECEIVING:  return "Receiving";
    default:                    return "Starting";
    }
}

static void set_status(const char *s) {
    snprintf(g_status, sizeof(g_status), "%s", s);
    g_status_until_us = mono_us() + 6000000ULL;
}

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

// ---- downloads folder -------------------------------------------------------
static void resolve_dir(void) {
    const char *home = "/HOME/ADMIN";
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0] && !(pw->pw_dir[0] == '/' && pw->pw_dir[1] == 0)) home = pw->pw_dir;
    snprintf(g_dir, sizeof(g_dir), "%s/DOWNLOAD", home);
    struct stat st;
    if (stat(g_dir, &st) != 0) {
        mkdir(home, 0755);
        mkdir(g_dir, 0755);
    }
    snprintf(g_quefile, sizeof(g_quefile), "%s/DLMGR.QUE", g_dir);
}

static int file_exists(const char *p, unsigned long *size) {
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    if (size) *size = (unsigned long)(st.st_size < 0 ? 0 : st.st_size);
    return 1;
}

// Derive a file name from the URL's last path segment; sanitise for the ext2
// root (case-sensitive, arbitrary length, but keep it tidy) and avoid clobbering
// an existing file by suffixing -1, -2, ...
static void derive_name(const char *url, char *out, int cap) {
    const char *p = url;
    const char *scheme = strstr(p, "://");
    if (scheme) p = scheme + 3;
    const char *q = p;
    while (*q && *q != '?' && *q != '#') q++;
    const char *end = q;
    const char *seg = end;
    while (seg > p && seg[-1] != '/') seg--;
    int n = 0;
    for (const char *c = seg; c < end && n < cap - 1; c++) {
        char ch = *c;
        if (ch == '%' && c + 2 < end) {   // %XX decode
            int hi = c[1], lo = c[2];
            int v = -1, w = -1;
            if (hi >= '0' && hi <= '9') v = hi - '0'; else if ((hi | 32) >= 'a' && (hi | 32) <= 'f') v = (hi | 32) - 'a' + 10;
            if (lo >= '0' && lo <= '9') w = lo - '0'; else if ((lo | 32) >= 'a' && (lo | 32) <= 'f') w = (lo | 32) - 'a' + 10;
            if (v >= 0 && w >= 0) { ch = (char)(v * 16 + w); c += 2; }
        }
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                 ch == '.' || ch == '_' || ch == '-' || ch == '+' || ch == '(' || ch == ')';
        out[n++] = ok ? ch : '_';
    }
    out[n] = 0;
    if (n == 0 || (n == 1 && out[0] == '.') || strcmp(out, "..") == 0)
        snprintf(out, cap, "%s", (end > p && end[-1] == '/') ? "index.html" : "download.bin");
}

static void unique_path(dl_t *j) {
    char base[80];
    snprintf(base, sizeof(base), "%s", j->name);
    snprintf(j->path, sizeof(j->path), "%s/%s", g_dir, j->name);
    for (int k = 1; k < 100 && file_exists(j->path, 0); k++) {
        // split extension
        char stem[80], ext[24];
        const char *dot = 0;
        for (const char *c = base; *c; c++) if (*c == '.') dot = c;
        if (dot && dot != base) {
            int sl = (int)(dot - base); if (sl > (int)sizeof(stem) - 1) sl = (int)sizeof(stem) - 1;
            memcpy(stem, base, (unsigned)sl); stem[sl] = 0;
            snprintf(ext, sizeof(ext), "%s", dot);
        } else { snprintf(stem, sizeof(stem), "%s", base); ext[0] = 0; }
        snprintf(j->name, sizeof(j->name), "%s-%d%s", stem, k, ext);
        snprintf(j->path, sizeof(j->path), "%s/%s", g_dir, j->name);
    }
}

// ---- queue persistence -------------------------------------------------------
static void save_queue(void) {
    FILE *f = fopen(g_quefile, "wb");
    if (!f) return;
    for (int i = 0; i < g_ndl; i++) {
        dl_t *j = &g_dl[i];
        int st = j->state;
        if (st == DS_RUNNING) st = (j->transport == TR_RAW && j->bytes > 0) ? DS_PAUSED : DS_QUEUED;
        char line[HTTPX_URL_MAX + 160];
        int n = snprintf(line, sizeof(line), "%d\t%lu\t%lu\t%s\t%s\n", st, j->bytes, j->total, j->name, j->url);
        if (n > 0) fwrite(line, 1, (unsigned)n, f);
    }
    fclose(f);
}

static void load_queue(void) {
    static char buf[32 * 1024];
    FILE *f = fopen(g_quefile, "rb");
    if (!f) return;
    unsigned n = (unsigned)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = buf;
    while (*p && g_ndl < MAX_DL) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') *p++ = 0;
        // state \t bytes \t total \t name \t url
        char *f1 = line, *f2 = 0, *f3 = 0, *f4 = 0, *f5 = 0;
        for (char *c = line; *c; c++) if (*c == '\t') { *c = 0; if (!f2) f2 = c + 1; else if (!f3) f3 = c + 1; else if (!f4) f4 = c + 1; else if (!f5) { f5 = c + 1; break; } }
        if (!f5 || !f5[0]) continue;
        dl_t *j = &g_dl[g_ndl];
        memset(j, 0, sizeof(*j));
        j->used = 1; j->kjob = -1; j->hx = -1;
        j->state = atoi(f1);
        j->bytes = (unsigned long)atol(f2);
        j->total = (unsigned long)atol(f3);
        snprintf(j->name, sizeof(j->name), "%s", f4);
        snprintf(j->url, sizeof(j->url), "%s", f5);
        snprintf(j->path, sizeof(j->path), "%s/%s", g_dir, j->name);
        j->transport = ci_prefix(j->url, "https://") ? TR_KERNEL : TR_RAW;
        if (j->state == DS_RUNNING) j->state = DS_QUEUED;
        if (j->state < DS_QUEUED || j->state > DS_ERROR) j->state = DS_ERROR;
        unsigned long onDisk = 0;
        int present = file_exists(j->path, &onDisk);
        if (j->state == DS_DONE) {
            if (!present) { j->state = DS_ERROR; snprintf(j->msg, sizeof(j->msg), "File no longer in the folder"); }
            else snprintf(j->msg, sizeof(j->msg), "Saved");
        } else if (j->state == DS_PAUSED || j->state == DS_ERROR) {
            j->bytes = (j->transport == TR_RAW && present) ? onDisk : 0;
            j->want_resume = (j->bytes > 0);
            snprintf(j->msg, sizeof(j->msg), j->state == DS_PAUSED ? "Paused" : "Failed earlier");
        } else {
            j->bytes = 0;
            snprintf(j->msg, sizeof(j->msg), "Queued");
        }
        g_ndl++;
    }
}

// ---- transfer engine ----------------------------------------------------------
static void raw_sink(void *ctx, const unsigned char *d, unsigned len) {
    dl_t *j = (dl_t *)ctx;
    httpx_t *h = &g_hx[j->hx];
    if (j->discard) return;
    if (!j->fp) {
        if (h->status == 206 && j->want_resume) {
            j->fp = fopen(j->path, "ab");
            j->total = (h->content_len >= 0) ? j->bytes + (unsigned long)h->content_len : 0;
        } else if (h->status == 200) {
            j->fp = fopen(j->path, "wb");
            j->bytes = 0;
            j->total = (h->content_len >= 0) ? (unsigned long)h->content_len : 0;
        } else {
            j->discard = 1;          // error body; judged when the job completes
            return;
        }
        if (!j->fp) { j->write_err = 1; j->discard = 1; return; }
    }
    if (fwrite(d, 1, len, j->fp) != len) { j->write_err = 1; j->discard = 1; return; }
    j->bytes += len;
}

static void job_fail(dl_t *j, const char *why) {
    j->state = DS_ERROR;
    snprintf(j->msg, sizeof(j->msg), "%s", why);
}

static void job_release_raw(dl_t *j) {
    if (j->fp) { fclose(j->fp); j->fp = 0; }
    if (j->hx >= 0) { g_hx_busy[j->hx] = 0; j->hx = -1; }
}

static int start_job(dl_t *j) {
    j->rate_bps = 0; j->rate_base = j->bytes; j->t0_us = j->t_rate_us = mono_us();
    j->write_err = 0; j->discard = 0;
    if (ci_prefix(j->url, "https://")) {
        j->transport = TR_KERNEL;
        if (!sys_net_is_up()) { job_fail(j, "Network is down"); return 0; }
        j->bytes = 0; j->want_resume = 0;      // the kernel job has no Range: always from byte 0
        j->kjob = http_fetch_start(j->url);
        if (j->kjob < 0) {
            job_fail(j, j->kjob == NET_ERR_FAULTY ? "Network marked faulty; retry shortly" : "Could not start the fetch");
            return 0;
        }
        j->kphase = 0;
        j->state = DS_RUNNING;
        snprintf(j->msg, sizeof(j->msg), "Starting (https via kernel TLS)");
        return 1;
    }
    if (!ci_prefix(j->url, "http://")) { job_fail(j, "Only http:// and https:// URLs are supported"); return 0; }
    int slot = -1;
    for (int i = 0; i < RAW_POOL; i++) if (!g_hx_busy[i]) { slot = i; break; }
    if (slot < 0) return 0;                    // stays queued; a slot frees soon
    if (!sys_net_is_up()) { job_fail(j, "Network is down"); return 0; }
    j->transport = TR_RAW;
    unsigned long from = 0;
    if (j->want_resume && j->bytes > 0 && file_exists(j->path, 0)) from = j->bytes;
    else { j->bytes = 0; j->want_resume = 0; }
    j->hx = slot; g_hx_busy[slot] = 1;
    j->fp = 0;
    if (httpx_start(&g_hx[slot], "GET", j->url, 0, 0, 0, from, raw_sink, j) != 0) {
        job_fail(j, g_hx[slot].err);
        job_release_raw(j);
        return 0;
    }
    j->state = DS_RUNNING;
    snprintf(j->msg, sizeof(j->msg), from ? "Resuming" : "Starting");
    return 1;
}

static void update_rate(dl_t *j) {
    unsigned long long t = mono_us();
    if (t - j->t_rate_us >= 1000000ULL) {
        unsigned long long dt = t - j->t_rate_us;
        unsigned long db = j->bytes >= j->rate_base ? j->bytes - j->rate_base : 0;
        j->rate_bps = (unsigned long)((unsigned long long)db * 1000000ULL / dt);
        j->rate_base = j->bytes;
        j->t_rate_us = t;
    }
}

static void poll_job(dl_t *j) {
    if (j->state != DS_RUNNING) return;
    if (j->transport == TR_KERNEL) {
        int status = 0; unsigned int len = 0;
        int st = http_fetch_poll(j->kjob, &status, &len);
        if (st == 0) {
            int ph = j->kphase; unsigned int br = 0, cl = 0;
            if (http_fetch_progress(j->kjob, &ph, &br, &cl) == 0) {
                j->kphase = ph; j->bytes = br; j->total = cl;
                snprintf(j->msg, sizeof(j->msg), "%s", kphase_name(ph));
                if (cl > KFETCH_CEILING) {
                    http_fetch_cancel(j->kjob); j->kjob = -1;
                    char sz[24]; nu_fmt_bytes(cl, sz, sizeof(sz));
                    char m[120];
                    snprintf(m, sizeof(m), "%s exceeds the 1 MB kernel HTTPS fetch ceiling (WGET_BUFFER_SIZE)", sz);
                    job_fail(j, m);
                    j->bytes = 0;
                }
            }
            update_rate(j);
            return;
        }
        if (st == 1) {
            unsigned long expected = j->total;
            int n = http_fetch_read(j->kjob, g_kbuf, KBUF_CAP);
            j->kjob = -1;
            if (n < 0) { job_fail(j, "Could not read the fetched body"); return; }
            if (status != 200 && status != 0) {
                char m[64]; snprintf(m, sizeof(m), "HTTP %d %s", status, httpx_status_text(status));
                job_fail(j, m); return;
            }
            FILE *f = fopen(j->path, "wb");
            if (!f) { job_fail(j, "Could not create the file"); return; }
            unsigned wrote = (n > 0) ? (unsigned)fwrite(g_kbuf, 1, (unsigned)n, f) : 0;
            fclose(f);
            if ((int)wrote != n) { job_fail(j, "Disk write failed"); return; }
            j->bytes = (unsigned long)n;
            if (expected && (unsigned long)n < expected) {
                char m[120];
                snprintf(m, sizeof(m), "Truncated at the 1 MB kernel HTTPS ceiling (%lu of %lu bytes)", (unsigned long)n, expected);
                job_fail(j, m);
                return;
            }
            j->total = j->bytes;
            j->state = DS_DONE;
            snprintf(j->msg, sizeof(j->msg), "Saved");
            return;
        }
        if (st == 2) {
            http_fetch_read(j->kjob, g_kbuf, 0);     // frees the job
            j->kjob = -1;
            char name[80] = "";
            net_last_error(name, sizeof(name));
            char m[120];
            if (name[0]) snprintf(m, sizeof(m), "Fetch failed: %s", name);
            else snprintf(m, sizeof(m), "Fetch failed");
            job_fail(j, m);
            return;
        }
        j->kjob = -1;
        job_fail(j, "Fetch job was lost");
        return;
    }
    // raw http:// path
    httpx_t *h = &g_hx[j->hx];
    int r = httpx_poll(h);
    if (r == HTTPX_RUNNING) {
        if (h->phase == HTTPX_PH_BODY) snprintf(j->msg, sizeof(j->msg), "Receiving");
        else snprintf(j->msg, sizeof(j->msg), "%s", httpx_phase_name(h));
        update_rate(j);
        return;
    }
    if (j->fp) { fclose(j->fp); j->fp = 0; }
    if (r == HTTPX_DONE) {
        if (j->write_err) job_fail(j, "Disk write failed");
        else if (h->status == 200 || h->status == 206) {
            j->total = j->bytes; j->state = DS_DONE; j->want_resume = 0;
            snprintf(j->msg, sizeof(j->msg), "Saved");
        } else if (h->status == 416) {
            j->bytes = 0; j->want_resume = 0;
            job_fail(j, "Range not satisfiable (file changed on the server); Retry restarts");
        } else {
            char m[64]; snprintf(m, sizeof(m), "HTTP %d %s", h->status, httpx_status_text(h->status));
            job_fail(j, m);
        }
    } else {
        if (strcmp(h->err, "cancelled") != 0) {
            char m[120];
            if (j->bytes > 0) { snprintf(m, sizeof(m), "%s; Retry resumes", h->err); j->want_resume = 1; }
            else snprintf(m, sizeof(m), "%s", h->err);
            job_fail(j, m);
        }
    }
    job_release_raw(j);
}

static void schedule(void) {
    int running = 0;
    for (int i = 0; i < g_ndl; i++) if (g_dl[i].state == DS_RUNNING) running++;
    for (int i = 0; i < g_ndl && running < MAX_ACTIVE; i++) {
        if (g_dl[i].state != DS_QUEUED) continue;
        if (start_job(&g_dl[i])) running++;
    }
}

static int any_active(void) {
    for (int i = 0; i < g_ndl; i++)
        if (g_dl[i].state == DS_RUNNING || g_dl[i].state == DS_QUEUED) return 1;
    return 0;
}

// ---- user actions --------------------------------------------------------------
static void add_url(void) {
    char url[HTTPX_URL_MAX];
    const char *s = g_urlbuf;
    while (*s == ' ' || *s == '\t') s++;
    snprintf(url, sizeof(url), "%s", s);
    int l = (int)strlen(url);
    while (l > 0 && (url[l - 1] == ' ' || url[l - 1] == '\r' || url[l - 1] == '\n')) url[--l] = 0;
    if (!l) { set_status("Enter or paste a URL first"); return; }
    if (!ci_prefix(url, "http://") && !ci_prefix(url, "https://")) {
        if (strstr(url, "://")) { set_status("Only http:// and https:// URLs can be downloaded"); return; }
        char t[HTTPX_URL_MAX]; snprintf(t, sizeof(t), "http://%s", url); snprintf(url, sizeof(url), "%s", t);
    }
    if (g_ndl >= MAX_DL) { set_status("Queue is full; remove finished entries first"); return; }
    dl_t *j = &g_dl[g_ndl];
    memset(j, 0, sizeof(*j));
    j->used = 1; j->kjob = -1; j->hx = -1;
    snprintf(j->url, sizeof(j->url), "%s", url);
    derive_name(url, j->name, sizeof(j->name));
    unique_path(j);
    j->transport = ci_prefix(url, "https://") ? TR_KERNEL : TR_RAW;
    j->state = DS_QUEUED;
    snprintf(j->msg, sizeof(j->msg), "Queued");
    g_sel = g_ndl;
    g_ndl++;
    tf_set_text(&g_url.tf, "");
    g_url.start = 0;
    save_queue();
    char m[160]; snprintf(m, sizeof(m), "Queued %s", j->name); set_status(m);
}

static void pause_job(dl_t *j) {
    if (j->state == DS_QUEUED) { j->state = DS_PAUSED; snprintf(j->msg, sizeof(j->msg), "Paused"); return; }
    if (j->state != DS_RUNNING) return;
    if (j->transport == TR_KERNEL) {
        if (j->kjob >= 0) { http_fetch_cancel(j->kjob); j->kjob = -1; }
        j->bytes = 0; j->want_resume = 0;
        j->state = DS_PAUSED;
        snprintf(j->msg, sizeof(j->msg), "Paused (https restarts from the beginning: kernel fetch has no Range)");
    } else {
        if (j->hx >= 0) httpx_cancel(&g_hx[j->hx]);
        job_release_raw(j);
        j->want_resume = (j->bytes > 0);
        j->state = DS_PAUSED;
        char sz[24]; nu_fmt_bytes(j->bytes, sz, sizeof(sz));
        snprintf(j->msg, sizeof(j->msg), "Paused at %s", sz);
    }
}

static void resume_job(dl_t *j) {
    if (j->state != DS_PAUSED && j->state != DS_ERROR) return;
    if (j->transport == TR_RAW && j->bytes > 0 && file_exists(j->path, 0)) j->want_resume = 1;
    else { j->want_resume = 0; j->bytes = 0; }
    j->state = DS_QUEUED;
    snprintf(j->msg, sizeof(j->msg), j->want_resume ? "Queued (resume)" : "Queued");
}

static void remove_job(int idx) {
    if (idx < 0 || idx >= g_ndl) return;
    dl_t *j = &g_dl[idx];
    if (j->state == DS_RUNNING) {
        if (j->transport == TR_KERNEL && j->kjob >= 0) { http_fetch_cancel(j->kjob); j->kjob = -1; }
        if (j->transport == TR_RAW && j->hx >= 0) httpx_cancel(&g_hx[j->hx]);
        job_release_raw(j);
    }
    if (j->state != DS_DONE && j->bytes > 0) remove(j->path);   // discard the partial file
    for (int i = idx; i < g_ndl - 1; i++) g_dl[i] = g_dl[i + 1];
    g_ndl--;
    // the sinks hold dl_t pointers; refresh them for jobs that moved down
    for (int i = idx; i < g_ndl; i++) if (g_dl[i].hx >= 0) g_hx[g_dl[i].hx].sink_ctx = &g_dl[i];
    if (g_sel >= g_ndl) g_sel = g_ndl - 1;
}

static void clear_done(void) {
    for (int i = g_ndl - 1; i >= 0; i--) if (g_dl[i].state == DS_DONE) remove_job(i);
}

static void open_folder(void) {
    char *argv[3];
    argv[0] = (char *)"/APPS/FILES";
    argv[1] = g_dir;
    argv[2] = 0;
    if (sys_spawn_args("/APPS/FILES", argv, 2) < 0) set_status("Could not launch Files");
}

// ---- drawing -------------------------------------------------------------------
enum { B_ADD = 1, B_PAUSE, B_RESUME, B_RETRY, B_REMOVE, B_CLEAR, B_OPEN };
static nu_btn_t g_btn[8];
static int g_nbtn;
static int list_x, list_y, list_w, list_h;

static void layout_buttons(void) {
    g_nbtn = 0;
    dl_t *s = (g_sel >= 0 && g_sel < g_ndl) ? &g_dl[g_sel] : 0;
    int has_done = 0;
    for (int i = 0; i < g_ndl; i++) if (g_dl[i].state == DS_DONE) has_done = 1;

    // top row: Add, right-aligned
    int addw = nu_btn_w("Add");
    g_btn[g_nbtn++] = (nu_btn_t){ g_w - PAD - addw, (TOP_H - NU_BTN_H) / 2, addw, NU_BTN_H, "Add", GUI_BTN_PRIMARY, 1, B_ADD };

    // action row
    int x = PAD, y = TOP_H + (ACT_H - NU_BTN_H) / 2;
    struct { const char *l; int id; int en; int var; } a[] = {
        { "Pause",  B_PAUSE,  s && (s->state == DS_RUNNING || s->state == DS_QUEUED), GUI_BTN_SECONDARY },
        { "Resume", B_RESUME, s && s->state == DS_PAUSED, GUI_BTN_SECONDARY },
        { "Retry",  B_RETRY,  s && s->state == DS_ERROR, GUI_BTN_SECONDARY },
        { "Remove", B_REMOVE, s != 0, GUI_BTN_DANGER },
        { "Clear finished", B_CLEAR, has_done, GUI_BTN_SECONDARY },
    };
    for (unsigned i = 0; i < sizeof(a) / sizeof(a[0]); i++) {
        int w = nu_btn_w(a[i].l);
        g_btn[g_nbtn++] = (nu_btn_t){ x, y, w, NU_BTN_H, a[i].l, a[i].var, a[i].en, a[i].id };
        x += w + GUI_GAP;
    }
    int ow = nu_btn_w("Open folder");
    g_btn[g_nbtn++] = (nu_btn_t){ g_w - PAD - ow, y, ow, NU_BTN_H, "Open folder", GUI_BTN_GHOST, 1, B_OPEN };
}

static unsigned int state_color(int st, unsigned int bg) {
    switch (st) {
    case DS_DONE:    return nu_on(NU_OK_RAW, bg);
    case DS_ERROR:   return nu_on(NU_ERR_RAW, bg);
    case DS_PAUSED:  return nu_on(NU_WARN_RAW, bg);
    case DS_RUNNING: return nu_on(nu_acc(), bg);
    default:         return nu_dim(bg);
    }
}

static void draw_row(dl_t *j, int idx, int x, int y, int w) {
    unsigned int bg = (idx == g_sel) ? nu_sel() : nu_content();
    unsigned int ink = nu_fg(bg), dim = nu_dim(bg);
    win_draw_rect(win, x, y, w, ROW_H, bg);
    win_draw_rect(win, x, y + ROW_H - 1, w, 1, nu_border());

    // line 1: name left, state/message right
    const char *state_txt = j->state == DS_QUEUED ? "Queued" : j->state == DS_RUNNING ? "Downloading" :
                            j->state == DS_PAUSED ? "Paused" : j->state == DS_DONE ? "Done" : "Failed";
    int sw = nu_text_w(state_txt, NU_TTF_BODY);
    win_draw_text_ttf(win, x + w - PAD - sw, y + 6, state_txt, NU_TTF_BODY, state_color(j->state, bg));
    nu_text_fit(win, x + PAD, y + 6, j->name, w - 3 * PAD - sw, ink);

    // line 2: progress bar + numbers
    int pct = 0;
    if (j->total > 0) { pct = (int)((unsigned long long)j->bytes * 100ULL / j->total); if (pct > 100) pct = 100; }
    else if (j->state == DS_DONE) pct = 100;
    int bar_w = w * 42 / 100;
    if (bar_w < 120) bar_w = 120;
    int by = y + 30;
    if (j->state == DS_RUNNING || j->state == DS_PAUSED || j->state == DS_DONE || (j->state == DS_ERROR && j->bytes > 0))
        gui_progress(win, x + PAD, by, bar_w, 10, pct);
    else
        win_draw_rect(win, x + PAD, by + 4, bar_w, 2, nu_border());

    char a[32], b[32], r[32], info[200];
    nu_fmt_bytes(j->bytes, a, sizeof(a));
    if (j->total > 0) nu_fmt_bytes(j->total, b, sizeof(b)); else snprintf(b, sizeof(b), "?");
    r[0] = 0;
    if (j->state == DS_RUNNING && j->rate_bps > 0) { char rr[24]; nu_fmt_bytes(j->rate_bps, rr, sizeof(rr)); snprintf(r, sizeof(r), "  %s/s", rr); }
    if (j->state == DS_DONE) snprintf(info, sizeof(info), "%s  %s  %s", a, j->msg, j->transport == TR_KERNEL ? "(https via kernel TLS)" : "(http streamed)");
    else if (j->state == DS_QUEUED) snprintf(info, sizeof(info), "%s", j->msg);
    else if (j->total > 0) snprintf(info, sizeof(info), "%s / %s (%d%%)%s  %s", a, b, pct, r, j->msg);
    else snprintf(info, sizeof(info), "%s%s  %s", a, r, j->msg);
    int ix = x + PAD + bar_w + PAD;
    // small text: truncate by estimated width (11pt is ~0.8 of body width)
    {
        char fit[200]; int n = 0, wpx = 0, maxw = x + w - PAD - ix;
        for (; info[n] && n < (int)sizeof(fit) - 1; n++) { int cw = nu_char_w(info[n]) * 8 / 10; if (wpx + cw > maxw) break; wpx += cw; }
        memcpy(fit, info, (unsigned)n); fit[n] = 0;
        win_draw_text_ttf(win, ix, by - 3, fit, NU_TTF_SMALL, j->state == DS_ERROR ? state_color(DS_ERROR, bg) : dim);
    }
}

static void draw_all(void) {
    win_get_size(win, &g_w, &g_h);
    if (g_w < MIN_W) g_w = MIN_W;
    if (g_h < MIN_H) g_h = MIN_H;
    nu_apply_style();
    layout_buttons();
    unsigned int content = nu_content(), toolbar = nu_toolbar();
    unsigned int ink = nu_fg(content), dim = nu_dim(content);

    win_draw_rect(win, 0, 0, g_w, g_h, content);

    // top bar: URL field + Add
    win_draw_rect(win, 0, 0, g_w, TOP_H, toolbar);
    win_draw_rect(win, 0, TOP_H - 1, g_w, 1, nu_border());
    win_draw_text_ttf(win, PAD, (TOP_H - NU_TTF_BODY - 4) / 2, "URL", NU_TTF_BODY, nu_fg(toolbar));
    int fx = PAD + nu_text_w("URL", NU_TTF_BODY) + PAD;
    int fw = g_btn[0].x - GUI_GAP - fx;
    nu_field_draw(win, &g_url, fx, (TOP_H - NU_FIELD_H) / 2, fw, NU_FIELD_H, true, "Paste an http:// or https:// link and press Enter");

    // action row
    win_draw_rect(win, 0, TOP_H, g_w, ACT_H, nu_panel());
    win_draw_rect(win, 0, TOP_H + ACT_H - 1, g_w, 1, nu_border());
    for (int i = 0; i < g_nbtn; i++) nu_btn_draw(win, &g_btn[i], g_hover, g_pressed);

    // list
    list_x = PAD; list_y = TOP_H + ACT_H + PAD;
    list_w = g_w - 2 * PAD; list_h = g_h - list_y - STATUS_H - PAD;
    gui_card(win, list_x, list_y, list_w, list_h);
    int inner_x = list_x + 1, inner_y = list_y + 1, inner_w = list_w - 2, inner_h = list_h - 2;
    int need_bar = g_ndl * ROW_H > inner_h;
    int rows_w = inner_w - (need_bar ? GUI_SCROLL_W : 0);
    gui_scroll_config(&g_scroll, inner_x + rows_w, inner_y, GUI_SCROLL_W, inner_h, g_ndl * ROW_H, ROW_H);
    if (g_ndl == 0) {
        const char *l1 = "No downloads yet.";
        const char *l2 = "Paste a link above and press Add or Enter.";
        const char *l3 = "http:// files stream to disk with pause and resume; https:// uses the kernel TLS fetch (1 MB per file).";
        int cy = inner_y + inner_h / 2 - 30;
        win_draw_text_ttf(win, inner_x + (inner_w - nu_text_w(l1, NU_TTF_BODY)) / 2, cy, l1, NU_TTF_BODY, ink);
        win_draw_text_ttf(win, inner_x + (inner_w - nu_text_w(l2, NU_TTF_BODY)) / 2, cy + 24, l2, NU_TTF_BODY, dim);
        win_draw_text_ttf(win, inner_x + (inner_w - nu_text_w(l3, NU_TTF_SMALL)) / 2, cy + 50, l3, NU_TTF_SMALL, dim);
    } else {
        // Whole-row scrolling: there is no clip rect in the window API, so a row
        // is drawn only when it fits entirely inside the card (same approach as
        // gui_list's first/span). The scroll offset is honoured at row granularity.
        int first = g_scroll.offset / ROW_H;
        int y = inner_y;
        for (int i = first; i < g_ndl && y + ROW_H <= inner_y + inner_h; i++, y += ROW_H)
            draw_row(&g_dl[i], i, inner_x, y, rows_w);
        if (need_bar) gui_scroll_draw(win, &g_scroll);
    }

    // status bar
    int sy = g_h - STATUS_H;
    win_draw_rect(win, 0, sy, g_w, STATUS_H, toolbar);
    win_draw_rect(win, 0, sy, g_w, 1, nu_border());
    int nrun = 0, nq = 0, ndone = 0, nerr = 0;
    for (int i = 0; i < g_ndl; i++) {
        if (g_dl[i].state == DS_RUNNING) nrun++; else if (g_dl[i].state == DS_QUEUED) nq++;
        else if (g_dl[i].state == DS_DONE) ndone++; else if (g_dl[i].state == DS_ERROR) nerr++;
    }
    char left[220];
    if (g_status[0] && mono_us() < g_status_until_us) snprintf(left, sizeof(left), "%s", g_status);
    else snprintf(left, sizeof(left), "%s   %d active, %d queued, %d done, %d failed", g_dir, nrun, nq, ndone, nerr);
    win_draw_text_ttf(win, PAD, sy + 4, left, NU_TTF_SMALL, nu_fg(toolbar));
    const char *net = sys_net_is_up() ? "network up" : "network down";
    win_draw_text_ttf(win, g_w - PAD - nu_text_w(net, NU_TTF_SMALL) * 8 / 10 - 4, sy + 4, net, NU_TTF_SMALL,
                      sys_net_is_up() ? nu_on(NU_OK_RAW, toolbar) : nu_on(NU_ERR_RAW, toolbar));
}

// ---- input -----------------------------------------------------------------------
static int button_at(int mx, int my) {
    for (int i = 0; i < g_nbtn; i++) if (nu_btn_hit(&g_btn[i], mx, my)) return g_btn[i].id;
    return -1;
}

static void do_button(int id) {
    dl_t *s = (g_sel >= 0 && g_sel < g_ndl) ? &g_dl[g_sel] : 0;
    switch (id) {
    case B_ADD:    add_url(); break;
    case B_PAUSE:  if (s) { pause_job(s); save_queue(); } break;
    case B_RESUME: if (s) { resume_job(s); save_queue(); } break;
    case B_RETRY:  if (s) { resume_job(s); save_queue(); } break;
    case B_REMOVE: if (s) { remove_job(g_sel); save_queue(); } break;
    case B_CLEAR:  clear_done(); save_queue(); break;
    case B_OPEN:   open_folder(); break;
    default: break;
    }
}

static void handle_click(int mx, int my) {
    int id = button_at(mx, my);
    if (id > 0) { g_pressed = id; do_button(id); return; }
    if (nu_field_hit(&g_url, mx, my)) { nu_field_click(&g_url, mx); return; }
    if (g_ndl && gui_scroll_press(&g_scroll, mx, my)) return;
    if (mx >= list_x && mx < list_x + list_w && my >= list_y && my < list_y + list_h) {
        int idx = (my - (list_y + 1) + g_scroll.offset) / ROW_H;
        if (idx >= 0 && idx < g_ndl) g_sel = idx; else g_sel = -1;
    }
}

int main(int argc, char **argv) {
    resolve_dir();
    for (int i = 0; i < MAX_DL; i++) { g_dl[i].kjob = -1; g_dl[i].hx = -1; }
    load_queue();
    nu_field_init(&g_url, g_urlbuf, sizeof(g_urlbuf));
    // launched with a URL argument (e.g. from another app): queue it at once
    if (argc >= 2 && argv[1] && (ci_prefix(argv[1], "http://") || ci_prefix(argv[1], "https://"))) {
        tf_set_text(&g_url.tf, argv[1]);
        add_url();
    }

    win = win_create("Downloads", 100, 70, WIN_W, WIN_H);
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
            case EVENT_KEY_DOWN:
                if (ev.keycode == 0x01) { running = 0; break; }                       // Esc
                if (ev.keycode == 0x1C || ev.key_char == '\n' || ev.key_char == '\r') { add_url(); break; }
                if (ev.keycode == GUI_KEY_UP || ev.keycode == GUI_KEY_DOWN) {
                    if (g_ndl) {
                        if (ev.keycode == GUI_KEY_UP) { if (g_sel > 0) g_sel--; else g_sel = 0; }
                        else if (g_sel < g_ndl - 1) g_sel++;
                        gui_scroll_reveal(&g_scroll, g_sel * ROW_H, ROW_H);
                    }
                    break;
                }
                if (ev.keycode == GUI_KEY_DEL && g_url.tf.len == 0) { if (g_sel >= 0) { remove_job(g_sel); save_queue(); } break; }
                if (ev.keycode == GUI_KEY_PGUP || ev.keycode == GUI_KEY_PGDN) { gui_scroll_key(&g_scroll, ev.keycode); break; }
                tf_handle_key(&g_url.tf, &ev);
                break;
            case EVENT_MOUSE_DOWN:
                handle_click(ev.mouse_x, ev.mouse_y);
                break;
            case EVENT_MOUSE_UP:
                g_pressed = -1;
                gui_scroll_release(&g_scroll);
                break;
            case EVENT_MOUSE_MOVE: {
                int hv = button_at(ev.mouse_x, ev.mouse_y);
                int moved = gui_scroll_motion(&g_scroll, ev.mouse_x, ev.mouse_y);
                if (hv == g_hover && !moved) need_draw = 0;   // pure pointer motion: nothing changed
                g_hover = hv;
                break;
            }
            case EVENT_MOUSE_SCROLL:
                gui_scroll_wheel(&g_scroll, ev.scroll_delta);
                break;
            default: break;
            }
        }

        int was_active = any_active();
        if (was_active) {
            schedule();
            for (int i = 0; i < g_ndl; i++) poll_job(&g_dl[i]);
            // persist when a transfer reached a terminal state this tick
            int fin = 0;
            for (int i = 0; i < g_ndl; i++) if (g_dl[i].state == DS_DONE || g_dl[i].state == DS_ERROR) fin++;
            if (fin != g_last_finished) { g_last_finished = fin; save_queue(); }
        }
        if (need_draw || was_active) { draw_all(); win_invalidate(win); }
    }

    // leave nothing running in the kernel table; partial raw files stay for resume
    for (int i = 0; i < g_ndl; i++) {
        dl_t *j = &g_dl[i];
        if (j->state != DS_RUNNING) continue;
        if (j->transport == TR_KERNEL && j->kjob >= 0) http_fetch_cancel(j->kjob);
        if (j->transport == TR_RAW && j->hx >= 0) { httpx_cancel(&g_hx[j->hx]); job_release_raw(j); }
    }
    save_queue();
    win_destroy(win);
    return 0;
}
