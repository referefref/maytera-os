// otaupd - MayteraOS signed kernel OTA security-update client (#492 Stage 1b).
//
// Runs as a registered background SERVICE (is_service=1) holding the new
// SVC_PERM_SELFUPDATE capability, so it - and only it - may invoke
// SYS_KERNEL_SELFUPDATE. Arbitrary Ring-3 apps cannot reach that syscall.
//
// Each run it:
//   1. Post-reboot: if /CONFIG/PENDING_UPDATE.TXT says STATUS=APPLIED and its
//      BUILD matches the running build, show a "patched" confirmation (once).
//   2. Fetch the signed security manifest from the update server.
//   3. Verify the manifest's RSA signature over the kernel sha256 against the
//      baked-in public key (SYS_OTA_VERIFY_SIG). A tampered/unsigned manifest is
//      REFUSED here - no dialog, no action.
//   4. If latest_secure_build > running build, show a Yes/No dialog naming the
//      fixed advisory (e.g. MAYTERA-SEC-2026-0008).
//   5. On YES: download the kernel, hand image+sha+signature to
//      SYS_KERNEL_SELFUPDATE (which re-verifies sha256 AND the RSA signature
//      before writing), then reboot. On NO: dismiss.
//
// argv[1]: "check" (default, shows the dialog) | "apply" (headless auto-apply,
// the honest test-trigger used when a real click cannot be injected) |
// "checkonly" (verify + compare + log, no dialog, no apply).

#include "../../libc/syscall.h"
#include "../../libc/string.h"
#include "../../libc/stdlib.h"
#include "../../libc/stdio.h"
#include "../../libc/gui.h"
#include "../../libc/notify.h"
#include "../../libc/fcntl.h"
#include "../../libc/unistd.h"

// Update-server base URL. Externalized (#492): read from /CONFIG/OTA_SERVER.CFG
// so a public build is not pinned to an internal IP. Falls back to this default
// if the config file is absent/empty. The manifest URL and the kernel URL are
// both derived from this base (the kernel path comes from the signed manifest,
// but its host is rebased onto the configured server so a relocated repo works).
#define OTA_SERVER_DEFAULT "http://<UPDATE_SERVER>"
#define OTA_SERVER_CFG     "/CONFIG/OTA_SERVER.CFG"
static char g_server_base[192] = OTA_SERVER_DEFAULT;

#ifndef SYS_KERNEL_SELFUPDATE
#define SYS_KERNEL_SELFUPDATE 313
#endif
#ifndef SYS_OTA_VERIFY_SIG
#define SYS_OTA_VERIFY_SIG 314
#endif

static int ota_verify_sig(const uint8_t *dig, const uint8_t *sig, unsigned len) {
    return (int)syscall3(SYS_OTA_VERIFY_SIG, (long)dig, (long)sig, (long)len);
}
static int kernel_selfupdate(const void *img, unsigned len, const uint8_t *sha,
                             unsigned build, const uint8_t *sig, unsigned siglen) {
    return (int)syscall6(SYS_KERNEL_SELFUPDATE, (long)img, (long)len, (long)sha,
                         (long)build, (long)sig, (long)siglen);
}

static char g_manifest[64 * 1024];

// Debug log to /OTA_DEBUG.TXT (pkg_write overwrites, so keep the full log and
// rewrite it each time). Diagnostics for the headless service path.
static char g_log[4096]; static int g_logn = 0;
static void dlog(const char *s) {
    while (*s && g_logn < 4094) g_log[g_logn++] = *s++;
    if (g_logn < 4094) g_log[g_logn++] = '\n';
    g_log[g_logn] = 0;
    syscall3(301, (long)"/OTA_DEBUG.TXT", (long)g_log, (long)g_logn);
}

// ---- tiny JSON scan (tolerant; same style as apps/updated) ----------------
static const char *find_key(const char *o, const char *e, const char *k) {
    int kl = strlen(k);
    for (const char *p = o; p + kl + 2 < e; p++)
        if (p[0] == '"' && strncmp(p + 1, k, kl) == 0 && p[1 + kl] == '"') {
            const char *q = p + 1 + kl + 1;
            while (q < e && (*q == ' ' || *q == ':')) q++;
            return q;
        }
    return 0;
}
static void jstr(const char *o, const char *e, const char *k, char *out, int cap) {
    out[0] = 0; const char *v = find_key(o, e, k);
    if (!v || v >= e || *v != '"') return; v++;
    int i = 0; while (v < e && *v != '"' && i < cap - 1) out[i++] = *v++;
    out[i] = 0;
}
static long jint(const char *o, const char *e, const char *k) {
    const char *v = find_key(o, e, k);
    if (!v) return -1;
    long n = 0; int any = 0;
    while (v < e && *v >= '0' && *v <= '9') { n = n * 10 + (*v - '0'); v++; any = 1; }
    return any ? n : -1;
}
// first string element of an array field: "advisories": ["X", ...]
static void jarr0(const char *o, const char *e, const char *k, char *out, int cap) {
    out[0] = 0; const char *v = find_key(o, e, k);
    if (!v) return;
    while (v < e && *v != '[' && *v != '"') v++;
    if (v < e && *v == '[') v++;
    while (v < e && *v != '"' && *v != ']') v++;
    if (v >= e || *v != '"') return; v++;
    int i = 0; while (v < e && *v != '"' && i < cap - 1) out[i++] = *v++;
    out[i] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// decode up to `max` bytes; returns count decoded.
static int hexdec(const char *s, uint8_t *out, int max) {
    int n = 0;
    while (n < max) {
        int hi = hexval(s[n * 2]); if (hi < 0) break;
        int lo = hexval(s[n * 2 + 1]); if (lo < 0) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

// ---- async HTTP GET (same worker the updated app uses) --------------------
static int http_get(const char *url, uint8_t *buf, int cap) {
    int job = http_fetch_start(url);
    if (job < 0) return -1;
    for (int i = 0; i < 12000; i++) {   // up to ~600s (large kernel download)
        int status = 0; unsigned int len = 0;
        int st = http_fetch_poll(job, &status, &len);
        if (st < 0) { http_fetch_cancel(job); return -1; }
        if (st == 1) return http_fetch_read(job, (char *)buf, (unsigned)cap);
        if (st == 2) { http_fetch_read(job, (char *)buf, (unsigned)cap); return -1; }
        usleep(50000);
    }
    http_fetch_cancel(job);
    return -1;
}

// STREAM a large HTTP body to a local file via small in-regime Range requests
// (#492 large-download fix). ROOT CAUSE of the old stall: a single multi-MB
// response overruns the tiny receive path (e1000 RX ring = 32 desc x 2KB = 64KB,
// drained by net_poll only at compositor-flip cadence) and, worse, any body
// larger than TCP_RECV_BUFFER_SIZE (32KB) forces the zero-window / window-update
// recovery repeatedly, which is where big transfers stall. Small fetches (the
// manifest) never hit that. FIX: request the image in OTA_STREAM_CHUNK pieces,
// each well UNDER 32KB so every single response stays entirely inside the recv
// buffer (the reliable "small response" regime), via the SYNCHRONOUS fetch path
// which self-pumps net_poll + recv-with-timeout PER CHUNK (so a large transfer
// that keeps making progress never idle-times-out). Each chunk is written
// straight to `path` on disk (no 14.5MB RAM buffer). Returns total bytes, or -1.
// This is the general fix (helps browser large pages + package downloads too).
#define OTA_STREAM_CHUNK  (16u * 1024u)   // < TCP_RECV_BUFFER_SIZE (32KB) reliable regime
static uint8_t g_chunk[OTA_STREAM_CHUNK + 1024];   // + slack for 206 framing
static int stream_download_to_file(const char *url, const char *path, unsigned total) {
    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { dlog("stream: open stage failed"); return -1; }
    unsigned off = 0;
    while (total == 0 || off < total) {
        unsigned want = OTA_STREAM_CHUNK;
        if (total && off + want > total) want = total - off;
        unsigned end = off + want - 1;
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "Range: bytes=%u-%u\r\n", off, end);
        unsigned got = 0; int status = 0; int r = -1;
        for (int attempt = 0; attempt < 4 && got == 0; attempt++) {
            if (attempt) usleep(200000);
            r = sys_http_fetch_hdr(url, hdr, (char *)g_chunk, sizeof(g_chunk), &got, &status);
        }
        if (r <= 0 || got == 0 || (status != 206 && status != 200)) {
            char m[96]; snprintf(m, sizeof(m), "stream FAIL off=%u r=%d got=%u st=%d", off, r, got, status);
            dlog(m); printf("[ota] stream fail off=%u r=%d got=%u st=%d\n", off, r, got, status);
            sys_close(fd); return -1;
        }
        if (sys_write(fd, (char *)g_chunk, got) != (long)got) {
            dlog("stream: disk write short"); sys_close(fd); return -1;
        }
        off += got;
        if (status == 200) break;   // server ignored Range and sent whole (small) body
        if ((off % (1024u * 1024u)) < OTA_STREAM_CHUNK) {
            char m[48]; snprintf(m, sizeof(m), "stream %u/%u", off, total); dlog(m);
        }
    }
    sys_close(fd);
    { char m[48]; snprintf(m, sizeof(m), "stream done %u bytes", off); dlog(m); }
    return (int)off;
}

// ---- update-server URL externalization (#492) ------------------------------
// Load the server base URL from OTA_SERVER_CFG (first non-comment line), trimmed
// of trailing slash/whitespace. Keeps the default if the file is absent/empty.
static void load_server_base(void) {
    int fd = sys_open(OTA_SERVER_CFG, O_RDONLY);
    if (fd < 0) return;
    char b[192]; int n = sys_read(fd, b, sizeof(b) - 1); sys_close(fd);
    if (n <= 0) return; b[n] = 0;
    int i = 0; while (b[i] == ' ' || b[i] == '\t') i++;
    if (b[i] == '#' || b[i] == 0) return;
    int j = 0; while (b[i] && b[i] != '\n' && b[i] != '\r' && j < (int)sizeof(g_server_base) - 1)
        g_server_base[j++] = b[i++];
    while (j > 0 && (g_server_base[j-1] == '/' || g_server_base[j-1] == ' ' || g_server_base[j-1] == '\t')) j--;
    g_server_base[j] = 0;
    if (g_server_base[0] == 0) strcpy(g_server_base, OTA_SERVER_DEFAULT);
}
// Path portion (from the first '/' after scheme://host) of an absolute http URL.
static const char *url_path(const char *u) {
    const char *p = strstr(u, "://");
    if (!p) return (u && u[0] == '/') ? u : "/";
    p += 3;
    while (*p && *p != '/') p++;
    return *p ? p : "/";
}

static unsigned running_build(void) {
    char v[64]; v[0] = 0;
    get_version(v, sizeof(v));
    char *b = strstr(v, "build ");
    if (!b) return 0;
    b += 6; unsigned n = 0;
    while (*b >= '0' && *b <= '9') { n = n * 10 + (*b - '0'); b++; }
    return n;
}

// ---- post-reboot "patched" confirmation ------------------------------------
static void show_applied_window(unsigned build, const char *adv) {
    int W = 440, H = 170;
    int win = gui_window_create("Security update applied", 300, 220, W, H);
    if (win < 0) return;
    char l1[96], l2[96];
    snprintf(l1, sizeof(l1), "Updated to build %u.", build);
    snprintf(l2, sizeof(l2), "%s is now patched.", adv[0] ? adv : "The advisory");
    for (int frame = 0; frame < 100000; frame++) {
        gui_fill_rect(win, 0, 0, W, H, 0x00ECE9D8);
        gui_fill_rect(win, 0, 0, W, 34, 0x00107C10);
        gui_draw_text(win, 12, 10, "Update complete", 0x00FFFFFF);
        gui_draw_text(win, 16, 52, l1, 0x00202020);
        gui_draw_text(win, 16, 74, l2, 0x00202020);
        gui_draw_text(win, 16, 100, "Your system is protected against the fixed", 0x00404040);
        gui_draw_text(win, 16, 118, "remote out-of-bounds write.", 0x00404040);
        gui_draw_button(win, W - 90, H - 40, 74, 26, "OK", 0x00C0C0C0, 0x00000000, false, false);
        gui_event_t ev;
        int r = gui_get_event(win, &ev, 200);
        if (r > 0) {
            if (ev.type == EVENT_WINDOW_CLOSE) break;
            if (ev.type == EVENT_MOUSE_UP || ev.type == EVENT_MOUSE_DOWN) {
                if (gui_point_in_rect(ev.mouse_x, ev.mouse_y, W - 90, H - 40, 74, 26)) break;
            }
        }
    }
    gui_window_destroy(win);
}

static void post_reboot_confirm(unsigned rbuild) {
    int fd = sys_open("/CONFIG/PENDING_UPDATE.TXT", O_RDONLY);
    if (fd < 0) return;
    char buf[256]; int n = sys_read(fd, buf, sizeof(buf) - 1); sys_close(fd);
    if (n <= 0) return; buf[n] = 0;
    if (!strstr(buf, "STATUS=APPLIED")) return;
    char *bp = strstr(buf, "BUILD="); if (!bp) return;
    unsigned mb = 0; bp += 6; while (*bp >= '0' && *bp <= '9') { mb = mb * 10 + (*bp - '0'); bp++; }
    if (mb != rbuild) return;   // marker is for a different build; ignore
    // one-shot: skip if already acknowledged for this build
    int af = sys_open("/CONFIG/OTA_ACK.TXT", O_RDONLY);
    if (af >= 0) { char a[32]; int an = sys_read(af, a, sizeof(a) - 1); sys_close(af);
        if (an > 0) { a[an] = 0; unsigned ab = 0; for (int i = 0; a[i] >= '0' && a[i] <= '9'; i++) ab = ab * 10 + (a[i] - '0');
            if (ab == mb) return; } }
    char body[96];
    snprintf(body, sizeof(body), "Updated to build %u, MAYTERA-SEC-2026-0008 patched", mb);
    notify_post("Security update applied", body, NOTIFY_SUCCESS);
    show_applied_window(mb, "MAYTERA-SEC-2026-0008");
    char ack[16]; int al = snprintf(ack, sizeof(ack), "%u\n", mb);
    // pkg_write is the FAT ESP writer (SYS_PKG_WRITE 301); best-effort ack.
    syscall3(301, (long)"/CONFIG/OTA_ACK.TXT", (long)ack, (long)al);
}

// ---- the Yes/No update dialog ---------------------------------------------
// Returns 1 = YES (apply), 0 = NO/dismiss.
#define BTN_YES_X 250
#define BTN_NO_X  340
#define BTN_Y     172
#define BTN_W     84
#define BTN_H     28
static int update_dialog(unsigned newb, unsigned oldb, const char *adv,
                         const char *severity) {
    int W = 440, H = 214;
    int win = gui_window_create("Security update available", 280, 200, W, H);
    if (win < 0) return 0;
    char l1[96], l2[96];
    snprintf(l1, sizeof(l1), "A security update (build %u) is available.", newb);
    snprintf(l2, sizeof(l2), "You are on build %u.  Severity: %s", oldb, severity[0] ? severity : "High");
    int hover_yes = 0, hover_no = 0;
    for (;;) {
        gui_fill_rect(win, 0, 0, W, H, 0x00ECE9D8);
        gui_fill_rect(win, 0, 0, W, 34, 0x00A02020);
        gui_draw_text(win, 12, 10, "MayteraOS Security Update", 0x00FFFFFF);
        gui_draw_text(win, 16, 48, l1, 0x00202020);
        gui_draw_text(win, 16, 70, l2, 0x00404040);
        char al[80]; snprintf(al, sizeof(al), "Fixes %s:", adv[0] ? adv : "a security advisory");
        gui_draw_text(win, 16, 100, al, 0x00202020);
        gui_draw_text(win, 16, 120, "a remote out-of-bounds write in HTTP", 0x00404040);
        gui_draw_text(win, 16, 138, "chunked decoding. Apply and reboot now?", 0x00404040);
        gui_draw_button(win, BTN_YES_X, BTN_Y, BTN_W, BTN_H, "Yes", 0x0040A040, 0x00FFFFFF, hover_yes, false);
        gui_draw_button(win, BTN_NO_X, BTN_Y, BTN_W, BTN_H, "No", 0x00C0C0C0, 0x00000000, hover_no, false);
        gui_event_t ev;
        int r = gui_get_event(win, &ev, 150);
        if (r <= 0) continue;
        if (ev.type == EVENT_WINDOW_CLOSE) { gui_window_destroy(win); return 0; }
        if (ev.type == EVENT_MOUSE_MOVE) {
            hover_yes = gui_point_in_rect(ev.mouse_x, ev.mouse_y, BTN_YES_X, BTN_Y, BTN_W, BTN_H);
            hover_no  = gui_point_in_rect(ev.mouse_x, ev.mouse_y, BTN_NO_X,  BTN_Y, BTN_W, BTN_H);
        }
        if (ev.type == EVENT_MOUSE_UP || ev.type == EVENT_MOUSE_DOWN) {
            if (gui_point_in_rect(ev.mouse_x, ev.mouse_y, BTN_YES_X, BTN_Y, BTN_W, BTN_H)) {
                gui_window_destroy(win); return 1;
            }
            if (gui_point_in_rect(ev.mouse_x, ev.mouse_y, BTN_NO_X, BTN_Y, BTN_W, BTN_H)) {
                gui_window_destroy(win); return 0;
            }
        }
        if (ev.type == EVENT_KEY_DOWN) {
            // Keyboard: Enter/Y = Yes, Esc/N = No. Lets the update be applied by a
            // genuine keystroke through the normal input path.
            char c = ev.key_char;
            if (c == '\n' || c == '\r' || c == 'y' || c == 'Y') { gui_window_destroy(win); return 1; }
            if (c == 27 || c == 'n' || c == 'N') { gui_window_destroy(win); return 0; }
        }
    }
}

// Resolve the run mode: explicit argv[1] wins; else /CONFIG/OTA_MODE.TXT
// ("apply"/"checkonly"); else the default "check" (interactive dialog).
static void resolve_mode(int argc, char **argv, char *out, int cap) {
    if (argc > 1) { strncpy(out, argv[1], cap - 1); out[cap - 1] = 0; return; }
    strncpy(out, "check", cap - 1); out[cap - 1] = 0;
    int fd = sys_open("/CONFIG/OTA_MODE.TXT", O_RDONLY);
    if (fd < 0) return;
    char b[32]; int n = sys_read(fd, b, sizeof(b) - 1); sys_close(fd);
    if (n <= 0) return; b[n] = 0;
    int i = 0; while (b[i] && b[i] != '\n' && b[i] != '\r' && b[i] != ' ') i++;
    b[i] = 0;
    if (b[0]) { strncpy(out, b, cap - 1); out[cap - 1] = 0; }
}

// Download + verify + apply. Returns SELFUPD rc from the kernel (0 == OK, will
// reboot). Negative on client-side failure.
static int do_apply(const char *kurl, const uint8_t sha[32], unsigned newb,
                    const uint8_t sig[256], unsigned total) {
    notify_post("Security update", "Downloading verified kernel...", NOTIFY_INFO);
    unsigned cap = 24u * 1024u * 1024u;
    if (total == 0 || total > cap) total = cap;
    { char m[48]; snprintf(m, sizeof(m), "apply: streaming total=%u", total); dlog(m); }

    // STREAM the kernel to disk in small in-regime Range requests (#492): each
    // response stays under the 32KB recv buffer so it never triggers the zero-
    // window stall that killed the old single 14.5MB fetch. No 14.5MB RAM buffer
    // is held during the download; each 16KB chunk is written straight to
    // /OTASTAGE.ELF. The transfer only fails if a chunk truly stops making
    // progress (per-chunk recv-with-timeout in the kernel), not on total size.
    int dn = stream_download_to_file(kurl, "/OTASTAGE.ELF", total);
    printf("[ota] streamed kernel to disk: %d bytes (expected %u)\n", dn, total);
    { char m[48]; snprintf(m, sizeof(m), "streamed %d bytes", dn); dlog(m); }
    if (dn <= 0) {
        // The wire download did not complete. If a verified image was already
        // pre-staged at /OTASTAGE.ELF (e.g. a prior partial-but-complete stream,
        // or an out-of-band stage), fall through to applying whatever is on disk;
        // the kernel still re-verifies sha256 + RSA sig, so the trust check is
        // unchanged and a truncated/tampered stage is REFUSED, not applied.
        dlog("stream download failed; will try whatever is at /OTASTAGE.ELF");
    }

    // Read the completed on-disk image into RAM ONCE for the verify+apply
    // syscall (selfupdate takes a RAM pointer). This is a one-shot read of an
    // already-downloaded local file, not a network buffer that can stall.
    uint8_t *img = (uint8_t *)sys_mmap(0, cap, 0x1 | 0x2, 0);
    { char m[48]; snprintf(m, sizeof(m), "mmap %u -> %p", cap, (void*)img); dlog(m); }
    if (!img || img == (uint8_t *)-1) { notify_post("Security update", "Out of memory", NOTIFY_ERROR); return -100; }
    for (unsigned p = 0; p < cap; p += 4096) img[p] = 0;   // pre-commit demand pages
    int fd = sys_open("/OTASTAGE.ELF", O_RDONLY);
    if (fd < 0) { notify_post("Security update", "Download failed", NOTIFY_ERROR); return -101; }
    unsigned off = 0; int r;
    while (off < cap && (r = sys_read(fd, (char *)img + off, 65536)) > 0) off += (unsigned)r;
    sys_close(fd);
    int n = (int)off;
    { char m[64]; snprintf(m, sizeof(m), "stage read %d bytes", n); dlog(m); }
    if (n <= 0) { notify_post("Security update", "Download failed", NOTIFY_ERROR); return -101; }
    // The kernel re-verifies sha256 AND the RSA signature before writing.
    int rc = kernel_selfupdate(img, (unsigned)n, sha, newb, sig, 256);
    printf("[ota] SYS_KERNEL_SELFUPDATE rc=%d\n", rc);
    { char m[48]; snprintf(m, sizeof(m), "selfupdate rc=%d", rc); dlog(m); }
    if (rc == 0) {
        notify_post("Security update", "Verified. Applying and rebooting...", NOTIFY_SUCCESS);
        for (volatile int d = 0; d < 20000000; d++) {}
        reboot();   // does not return
    } else {
        char m[64]; snprintf(m, sizeof(m), "Update refused (rc=%d)", rc);
        notify_post("Security update", m, NOTIFY_ERROR);
    }
    return rc;
}

int main(int argc, char **argv) {
    char mode[32]; resolve_mode(argc, argv, mode, sizeof(mode));
    unsigned rbuild = running_build();
    printf("[ota] running build=%u mode=%s\n", rbuild, mode);
    { char m[64]; snprintf(m, sizeof(m), "start build=%u mode=%s", rbuild, mode); dlog(m); }

    // 1. Post-reboot confirmation (independent of the network).
    post_reboot_confirm(rbuild);

    // Externalized update-server base (#492): default, overridable via config.
    load_server_base();
    { char m[224]; snprintf(m, sizeof(m), "server base=%s", g_server_base); dlog(m); }
    char manifest_url[224];
    snprintf(manifest_url, sizeof(manifest_url), "%s/security/manifest.json", g_server_base);

    // 2. Fetch the signed security manifest.
    for (int w = 0; w < 30 && !sys_net_is_up(); w++) usleep(500000);
    int n = -1;
    for (int a = 0; a < 5 && n <= 0; a++) { if (a) usleep(1000000);
        n = http_get(manifest_url, (uint8_t *)g_manifest, sizeof(g_manifest) - 1); }
    if (n <= 0) { printf("[ota] manifest fetch failed\n"); dlog("manifest fetch FAILED"); return 1; }
    g_manifest[n] = 0;
    const char *e = g_manifest + n;
    { char m[48]; snprintf(m, sizeof(m), "manifest fetched %d bytes", n); dlog(m); }

    long newb = jint(g_manifest, e, "latest_secure_build");
    long ksize = jint(g_manifest, e, "size");
    char shahex[80], sighex[600], kurl[160], sev[24], adv[48], summary[256];
    jstr(g_manifest, e, "sha256", shahex, sizeof(shahex));
    jstr(g_manifest, e, "signature", sighex, sizeof(sighex));
    jstr(g_manifest, e, "kernel_url", kurl, sizeof(kurl));
    jstr(g_manifest, e, "severity", sev, sizeof(sev));
    jstr(g_manifest, e, "summary", summary, sizeof(summary));
    jarr0(g_manifest, e, "advisories", adv, sizeof(adv));
    printf("[ota] manifest: latest=%ld adv=%s sev=%s\n", newb, adv, sev);

    uint8_t sha[32], sig[256];
    int shn = hexdec(shahex, sha, 32);
    int sgn = hexdec(sighex, sig, 256);
    if (shn != 32 || sgn != 256 || newb < 0) {
        printf("[ota] malformed manifest (sha=%d sig=%d build=%ld)\n", shn, sgn, newb);
        return 1;
    }

    // 3. AUTHENTICATE the manifest against the baked-in pubkey. Refuse if bad.
    { char m[80]; snprintf(m, sizeof(m), "parsed latest=%ld size=%ld shn=%d sgn=%d", newb, ksize, shn, sgn); dlog(m); }
    int vr = ota_verify_sig(sha, sig, 256);
    printf("[ota] manifest signature verify rc=%d\n", vr);
    { char m[48]; snprintf(m, sizeof(m), "sig verify rc=%d", vr); dlog(m); }
    if (vr != 0) {
        printf("[ota] REFUSED: manifest signature does not verify; ignoring update\n");
        notify_post("Security update", "Update rejected: signature invalid", NOTIFY_ERROR);
        return 2;
    }

    // 4. Compare builds.
    if ((unsigned)newb <= rbuild) {
        printf("[ota] up to date (running %u >= secure %ld)\n", rbuild, newb);
        dlog("up to date");
        return 0;
    }
    printf("[ota] UPDATE AVAILABLE: %u -> %ld (fixes %s)\n", rbuild, newb, adv);
    { char m[48]; snprintf(m, sizeof(m), "UPDATE AVAILABLE %u->%ld", rbuild, newb); dlog(m); }

    if (strcmp(mode, "checkonly") == 0) return 0;

    unsigned total = (ksize > 0) ? (unsigned)ksize : 0;
    // Rebase the manifest's kernel path onto the configured server base (#492) so
    // the download follows OTA_SERVER.CFG, not the IP baked into the manifest.
    char kurl_use[224];
    snprintf(kurl_use, sizeof(kurl_use), "%s%s", g_server_base, url_path(kurl));
    { char m[224]; snprintf(m, sizeof(m), "kernel url=%s", kurl_use); dlog(m); }
    if (strcmp(mode, "apply") == 0) {
        // Headless auto-apply (honest test-trigger path).
        return do_apply(kurl_use, sha, (unsigned)newb, sig, total);
    }

    // 5. Interactive: show the Yes/No dialog.
    int yes = update_dialog((unsigned)newb, rbuild, adv, sev);
    if (!yes) { printf("[ota] user declined\n"); return 0; }
    do_apply(kurl_use, sha, (unsigned)newb, sig, total);
    return 0;
}
