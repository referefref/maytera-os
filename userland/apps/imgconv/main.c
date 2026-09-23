// imgconv - Image Converter: batch convert and resize images on MayteraOS.
//
// THE GAP. The OS can decode BMP / PNG / JPEG anywhere (SYS_DECODE_IMAGE) and
// Maytera Studio can save ONE open document in another format, but nothing
// converted a folder: "turn these twenty PNG photos into 1280x800 BMPs so
// the wallpaper picker can see them" was a per-file Open / Export / Close
// loop in Studio, and the picker (libc wallpapers.h) only enumerates *.BMP
// at the filesystem root. Measured before building: no app or libc file
// mentions batch conversion, and apps/convert is a UNIT converter.
//
// What it does:
//   * Scans a folder for .BMP / .PNG / .JPG / .JPEG, lists them with a
//     checkbox each; select all / none; Files "Open With" a single file.
//   * Output format BMP (24-bit), PNG (RGB, stored deflate) or JPEG
//     (baseline, quality 60 / 75 / 85 / 95); resize None / Fit-within /
//     Exact / Scale-percent with presets for the real screen size (fb_info),
//     1280x800 and 640x480. Downscale is a box average, upscale bilinear.
//   * Writes to <source>/CONVERTED (or any folder typed in), never
//     overwriting the source file; optional "install as wallpaper" copies a
//     BMP result to / under a short uppercase name, which is exactly where
//     wp_enumerate() looks, so the result shows up in the picker at once.
//   * Converts ONE file per event-loop tick with a live progress bar and a
//     per-file OK / FAIL note; Cancel stops between files. The window stays
//     responsive and there is no busy-wait: idle it parks in win_get_event().
//
// Real syscalls: SYS_DECODE_IMAGE (decode), SYS_OPEN/READ/WRITE/MKDIR
// (files), SYS_FB_INFO (screen preset), SYS_WIN_* (UI). Encoders: imgenc.c
// here (BMP24 / PNG; shared with Sprite Studio) and apps/paint/jpegenc.c
// (a pure function, compiled BY PATH, not copied, so Studio and this app
// cannot drift apart on JPEG output).
//
// UI follows the shared style engine (docs/UI_STYLE_GUIDE.md): theme
// palette, cards, style-aware buttons and checkboxes, the shared scroll
// primitive, the shared caret/clipboard text field, TTF text, resizable.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"
#include "../../libc/gui_theme.h"
#include "../../libc/gui_style.h"
#include "../../libc/gui_scroll.h"
#include "../../libc/fcntl.h"
#include "../../libc/stdlib.h"
#include "../../libc/dirent.h"
#include "../../libc/userconf.h"   // userhome_root(): THE home lookup
#include "imgenc.h"

// apps/paint/jpegenc.c (see the Makefile): baseline JPEG from ARGB.
int jpeg_encode_argb(const uint32_t *src, int w, int h, int quality, int chroma444,
                     unsigned char **out, long *outlen);

// ---------------------------------------------------------------------------
// Layout tokens
// ---------------------------------------------------------------------------
#define WIN_TITLE   "Image Converter"
#define WIN_W       960
#define WIN_H       640
#define PANEL_W     312
#define STATUS_H    26
#define PAD         12
#define ROW_H       24
#define FIELD_H     28
#define BTN_H       28

#define MAX_FILES   512
#define NAME_MAX_L  64
#define FILE_CAP    (24L * 1024 * 1024)
#define DEC_MAX_W   2048
#define DEC_MAX_H   2048
#define OUT_MAX_PX  (2048L * 2048L)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static int win = -1;
static int g_win_w = WIN_W, g_win_h = WIN_H;
static int hover_x = -1, hover_y = -1;

typedef struct { char name[NAME_MAX_L]; int sel; int state; char note[48]; } item_t;
enum { ST_PENDING = 0, ST_OK, ST_FAIL, ST_BUSY };
static item_t g_items[MAX_FILES];
static int g_nitems = 0;
static gui_scroll_t g_sc;

enum { FMT_BMP = 0, FMT_PNG, FMT_JPEG };
static int g_fmt = FMT_BMP;
static const int g_qtab[4] = { 60, 75, 85, 95 };
static int g_qidx = 2;
enum { RS_NONE = 0, RS_FIT, RS_EXACT, RS_SCALE };
static int g_rs = RS_NONE;
static int g_wall = 0;

// Text fields (shared caret / selection / clipboard widget)
static char g_src[256] = "/";
static char g_out[256] = "";
static char g_w[8] = "1280", g_h[8] = "800", g_pct[8] = "50";
static textfield_t tf_src, tf_out, tf_w, tf_h, tf_pct;
enum { FOCUS_NONE = 0, FOCUS_SRC, FOCUS_OUT, FOCUS_W, FOCUS_H, FOCUS_PCT };
static int g_focus = FOCUS_NONE;

// Run state
static int g_run = 0, g_run_idx = 0, g_run_total = 0, g_run_done = 0, g_run_ok = 0, g_run_fail = 0;
static char g_status[160] = "Choose a folder and press Scan";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
typedef struct { int x, y, w, h; } rect_t;
static int point_in(rect_t r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static void set_status(const char *s) { strlcpy(g_status, s, sizeof(g_status)); }

static void apply_theme(void) {
    uint32_t win_bg   = theme_color(THEME_COLOR_WINDOW_BG);
    uint32_t fg       = theme_color(THEME_COLOR_LABEL_TEXT);
    uint32_t accent   = theme_color(THEME_COLOR_ACCENT);
    uint32_t border   = theme_color(THEME_COLOR_WINDOW_BORDER);
    uint32_t field_bg = theme_color(THEME_COLOR_TEXTBOX_BG);
    uint32_t track    = theme_color(THEME_COLOR_SCROLLBAR_BG);
    gui_set_style(gui_theme_is_classic() ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    gui_palette_t pal;
    pal.surface        = win_bg;
    pal.surface_raised = gui_lighten(win_bg, 14);
    pal.ink            = fg;
    pal.ink_dim        = gui_mix(fg, win_bg, 110);
    pal.accent         = accent;
    pal.accent_hover   = gui_lighten(accent, 28);
    pal.border         = border;
    pal.field_bg       = field_bg;
    pal.field_border   = border;
    pal.track          = track;
    gui_set_palette(&pal);
}

static int ci_eq(char a, char b) {
    if (a >= 'a' && a <= 'z') a = (char)(a - 32);
    if (b >= 'a' && b <= 'z') b = (char)(b - 32);
    return a == b;
}
static int ext_is(const char *s, const char *ext) {
    int n = (int)strlen(s), m = (int)strlen(ext);
    if (n < m) return 0;
    for (int i = 0; i < m; i++) if (!ci_eq(s[n - m + i], ext[i])) return 0;
    return 1;
}
static int is_image_name(const char *s) {
    return ext_is(s, ".BMP") || ext_is(s, ".PNG") || ext_is(s, ".JPG") || ext_is(s, ".JPEG");
}
static int parse_int(const char *s) {
    int v = 0, any = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; if (v > 99999) break; }
    return any ? v : 0;
}
static void join_path(char *out, unsigned long cap, const char *dir, const char *name) {
    int n = (int)strlen(dir);
    if (n > 0 && dir[n - 1] == '/') snprintf(out, cap, "%s%s", dir, name);
    else snprintf(out, cap, "%s/%s", dir, name);
}
static int file_exists(const char *p) { int fd = sys_open(p, 0); if (fd >= 0) { sys_close(fd); return 1; } return 0; }
static void fmt_size(long n, char *out, unsigned long cap) {
    if (n < 1024) snprintf(out, cap, "%ld B", n);
    else if (n < 1024 * 1024) snprintf(out, cap, "%ld KB", (n + 512) / 1024);
    else snprintf(out, cap, "%ld.%ld MB", n / (1024 * 1024), ((n % (1024 * 1024)) * 10) / (1024 * 1024));
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------
static void default_out_dir(void) {
    join_path(g_out, sizeof(g_out), g_src, "CONVERTED");
    tf_set_text(&tf_out, g_out);
}

static void scan_dir(void) {
    g_nitems = 0;
    if (g_run) return;
    DIR *d = opendir(g_src);
    if (!d) { set_status("Cannot open folder"); return; }
    struct dirent *de;
    while ((de = readdir(d)) != 0 && g_nitems < MAX_FILES) {
        if (de->d_type == DT_DIR) continue;
        if (de->d_name[0] == '.') continue;
        if (!is_image_name(de->d_name)) continue;
        if ((int)strlen(de->d_name) >= NAME_MAX_L) continue;
        // insertion sort by name for a deterministic list
        int pos = g_nitems;
        while (pos > 0 && strcmp(g_items[pos - 1].name, de->d_name) > 0) { g_items[pos] = g_items[pos - 1]; pos--; }
        strlcpy(g_items[pos].name, de->d_name, NAME_MAX_L);
        g_items[pos].sel = 1; g_items[pos].state = ST_PENDING; g_items[pos].note[0] = '\0';
        g_nitems++;
    }
    closedir(d);
    gui_scroll_set(&g_sc, 0);
    char msg[96];
    snprintf(msg, sizeof(msg), "%d image%s in folder%s", g_nitems, g_nitems == 1 ? "" : "s",
             g_nitems >= MAX_FILES ? " (list capped at 512)" : "");
    set_status(msg);
    default_out_dir();
}

static void dir_up(void) {
    int n = (int)strlen(g_src);
    while (n > 1 && g_src[n - 1] == '/') n--;
    while (n > 1 && g_src[n - 1] != '/') n--;
    while (n > 1 && g_src[n - 1] == '/') n--;
    if (n < 1) n = 1;
    g_src[n] = '\0';
    tf_set_text(&tf_src, g_src);
    scan_dir();
}

// ---------------------------------------------------------------------------
// Resampling
// ---------------------------------------------------------------------------
static void target_size(int sw, int sh, int *tw, int *th) {
    int W = parse_int(g_w), H = parse_int(g_h), P = parse_int(g_pct);
    *tw = sw; *th = sh;
    if (g_rs == RS_FIT && W > 0 && H > 0) {
        if (sw > W || sh > H) {
            long a = (long)W * sh, b = (long)H * sw;   // compare W/sw vs H/sh
            if (a < b) { *tw = W; *th = imax(1, (int)((long)sh * W / sw)); }
            else       { *th = H; *tw = imax(1, (int)((long)sw * H / sh)); }
        }
    } else if (g_rs == RS_EXACT && W > 0 && H > 0) { *tw = W; *th = H; }
    else if (g_rs == RS_SCALE && P > 0) { *tw = imax(1, (int)((long)sw * P / 100)); *th = imax(1, (int)((long)sh * P / 100)); }
    if (*tw > 4096) *tw = 4096;
    if (*th > 4096) *th = 4096;
}

static void resample(const uint32_t *s, int sw, int sh, uint32_t *d, int dw, int dh) {
    int down = (dw < sw) || (dh < sh);
    for (int y = 0; y < dh; y++) {
        uint32_t *drow = d + (long)y * dw;
        if (down) {
            int sy0 = (int)((long)y * sh / dh), sy1 = (int)((long)(y + 1) * sh / dh);
            if (sy1 <= sy0) sy1 = sy0 + 1;
            if (sy1 > sh) sy1 = sh;
            for (int x = 0; x < dw; x++) {
                int sx0 = (int)((long)x * sw / dw), sx1 = (int)((long)(x + 1) * sw / dw);
                if (sx1 <= sx0) sx1 = sx0 + 1;
                if (sx1 > sw) sx1 = sw;
                unsigned long r = 0, g = 0, b = 0, n = 0;
                for (int yy = sy0; yy < sy1; yy++) {
                    const uint32_t *sr = s + (long)yy * sw;
                    for (int xx = sx0; xx < sx1; xx++) {
                        uint32_t c = sr[xx];
                        r += (c >> 16) & 0xFF; g += (c >> 8) & 0xFF; b += c & 0xFF; n++;
                    }
                }
                if (n == 0) n = 1;
                drow[x] = 0xFF000000u | ((uint32_t)(r / n) << 16) | ((uint32_t)(g / n) << 8) | (uint32_t)(b / n);
            }
        } else {
            long cy = ((long)(2 * y + 1) * sh * 32768L) / dh - 32768L;
            if (cy < 0) cy = 0;
            int iy = (int)(cy >> 16), fy = (int)(cy & 0xFFFF);
            int iy1 = imin(iy + 1, sh - 1);
            const uint32_t *r0 = s + (long)iy * sw, *r1 = s + (long)iy1 * sw;
            for (int x = 0; x < dw; x++) {
                long cx = ((long)(2 * x + 1) * sw * 32768L) / dw - 32768L;
                if (cx < 0) cx = 0;
                int ix = (int)(cx >> 16), fx = (int)(cx & 0xFFFF);
                int ix1 = imin(ix + 1, sw - 1);
                uint32_t c00 = r0[ix], c01 = r0[ix1], c10 = r1[ix], c11 = r1[ix1];
                uint32_t out = 0xFF000000u;
                for (int sh8 = 0; sh8 <= 16; sh8 += 8) {
                    long a = (c00 >> sh8) & 0xFF, b2 = (c01 >> sh8) & 0xFF, c = (c10 >> sh8) & 0xFF, e = (c11 >> sh8) & 0xFF;
                    long top = a + ((b2 - a) * fx >> 16);
                    long bot = c + ((e - c) * fx >> 16);
                    long v = top + ((bot - top) * fy >> 16);
                    if (v < 0) v = 0;
                    if (v > 255) v = 255;
                    out |= (uint32_t)v << sh8;
                }
                drow[x] = out;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Convert one file. Fills it->note and it->state. Returns 0 on success.
// ---------------------------------------------------------------------------
static void wall_name(const char *base, char *out, unsigned long cap) {
    // Up to 8 uppercase alphanumerics of the base name + .BMP at the root.
    char n[12]; int k = 0;
    for (int i = 0; base[i] && base[i] != '.' && k < 8; i++) {
        char c = base[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) n[k++] = c;
    }
    if (k == 0) { n[k++] = 'W'; n[k++] = 'P'; }
    n[k] = '\0';
    snprintf(out, cap, "/%s.BMP", n);
}

static int convert_one(item_t *it) {
    char src[320], dst[320], base[NAME_MAX_L];
    join_path(src, sizeof(src), g_src, it->name);

    unsigned char *data = 0;
    long n = imgenc_read_file(src, &data, FILE_CAP);
    if (n <= 0) { strlcpy(it->note, n == -2 ? "FAIL: over 24 MB" : "FAIL: cannot read", sizeof(it->note)); return -1; }

    long cap = (long)DEC_MAX_W * DEC_MAX_H * 4;
    int bw = DEC_MAX_W, bh = DEC_MAX_H;
    uint32_t *pix = (uint32_t *)malloc((size_t)cap);
    if (!pix) { bw = 1280; bh = 1024; cap = (long)bw * bh * 4; pix = (uint32_t *)malloc((size_t)cap); }
    if (!pix) { free(data); strlcpy(it->note, "FAIL: out of memory", sizeof(it->note)); return -1; }
    int dims[2] = { 0, 0 };
    int r = decode_image(data, (unsigned int)n, bw, bh, pix, (unsigned int)cap, dims);
    free(data);
    if (r <= 0 || dims[0] <= 0 || dims[1] <= 0) { free(pix); strlcpy(it->note, "FAIL: decode", sizeof(it->note)); return -1; }
    int sw = dims[0], sh = dims[1];

    int tw, th;
    target_size(sw, sh, &tw, &th);
    if ((long)tw * th > OUT_MAX_PX) { free(pix); strlcpy(it->note, "FAIL: target over 4 Mpx", sizeof(it->note)); return -1; }
    uint32_t *outpx = pix;
    uint32_t *scaled = 0;
    if (tw != sw || th != sh) {
        scaled = (uint32_t *)malloc((size_t)tw * th * 4);
        if (!scaled) { free(pix); strlcpy(it->note, "FAIL: out of memory", sizeof(it->note)); return -1; }
        resample(pix, sw, sh, scaled, tw, th);
        outpx = scaled;
    }

    // Destination name: base + new extension, never the source file itself.
    strlcpy(base, it->name, sizeof(base));
    for (int i = (int)strlen(base) - 1; i > 0; i--) if (base[i] == '.') { base[i] = '\0'; break; }
    const char *ext = g_fmt == FMT_BMP ? ".BMP" : g_fmt == FMT_PNG ? ".PNG" : ".JPG";
    {
        char dir[256]; strlcpy(dir, g_out, sizeof(dir));
        if (dir[0] == '\0') strlcpy(dir, g_src, sizeof(dir));
        sys_mkdir(dir, 0755);   // idempotent, same idiom as userconf_open_write()
        char name[NAME_MAX_L + 8];
        snprintf(name, sizeof(name), "%s%s", base, ext);
        join_path(dst, sizeof(dst), dir, name);
        if (strcmp(dst, src) == 0) { snprintf(name, sizeof(name), "%s-2%s", base, ext); join_path(dst, sizeof(dst), dir, name); }
    }

    int wr = -1;
    if (g_fmt == FMT_BMP) wr = imgenc_write_bmp24(dst, outpx, tw, th, tw);
    else if (g_fmt == FMT_PNG) wr = imgenc_write_png(dst, outpx, tw, th, tw, 0);
    else {
        unsigned char *jb = 0; long jl = 0;
        if (jpeg_encode_argb(outpx, tw, th, g_qtab[g_qidx], 0, &jb, &jl) == 0 && jb) {
            int fd = sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd >= 0) { wr = imgenc_write_all(fd, jb, jl); sys_close(fd); }
            free(jb);
        }
    }
    if (wr != 0) { free(scaled); free(pix); strlcpy(it->note, "FAIL: write", sizeof(it->note)); return -1; }

    // Measure what actually landed, not what we meant to write.
    long written = 0;
    { int fd = sys_open(dst, 0); if (fd >= 0) { static unsigned char tmp[4096]; long k; while ((k = sys_read(fd, tmp, sizeof(tmp))) > 0) written += k; sys_close(fd); } }

    char wp[8] = "";
    if (g_wall && g_fmt == FMT_BMP) {
        char wpath[32];
        wall_name(base, wpath, sizeof(wpath));
        if (file_exists(wpath)) strlcpy(wp, " wp=", sizeof(wp));
        else if (imgenc_write_bmp24(wpath, outpx, tw, th, tw) == 0) strlcpy(wp, " +wp", sizeof(wp));
        else strlcpy(wp, " wp!", sizeof(wp));
    }
    char sz[24]; fmt_size(written, sz, sizeof(sz));
    snprintf(it->note, sizeof(it->note), "OK %dx%d %s%s", tw, th, sz, wp);
    free(scaled);
    free(pix);
    return 0;
}

// ---------------------------------------------------------------------------
// Run state machine (one file per event-loop tick)
// ---------------------------------------------------------------------------
static void run_start(void) {
    if (g_run) return;
    g_run_total = 0;
    for (int i = 0; i < g_nitems; i++) if (g_items[i].sel) { g_items[i].state = ST_PENDING; g_items[i].note[0] = '\0'; g_run_total++; }
    if (g_run_total == 0) { set_status("Nothing selected"); return; }
    if ((g_rs == RS_FIT || g_rs == RS_EXACT) && (parse_int(g_w) <= 0 || parse_int(g_h) <= 0)) { set_status("Width and height must be numbers"); return; }
    if (g_rs == RS_SCALE && parse_int(g_pct) <= 0) { set_status("Scale percent must be a number"); return; }
    g_run = 1; g_run_idx = 0; g_run_done = 0; g_run_ok = 0; g_run_fail = 0;
    g_focus = FOCUS_NONE;
    set_status("Converting...");
}

static void run_tick(void) {
    while (g_run_idx < g_nitems && !g_items[g_run_idx].sel) g_run_idx++;
    if (g_run_idx >= g_nitems) {
        g_run = 0;
        char msg[120];
        snprintf(msg, sizeof(msg), "Done: %d converted, %d failed, written to %s", g_run_ok, g_run_fail, g_out);
        set_status(msg);
        return;
    }
    item_t *it = &g_items[g_run_idx];
    it->state = ST_BUSY;
    if (convert_one(it) == 0) { it->state = ST_OK; g_run_ok++; } else { it->state = ST_FAIL; g_run_fail++; }
    g_run_done++;
    char msg[120];
    snprintf(msg, sizeof(msg), "%d / %d: %s  %s", g_run_done, g_run_total, it->name, it->note);
    set_status(msg);
    g_run_idx++;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
static int panel_x(void) { return g_win_w - PANEL_W; }
static rect_t list_rect(void) {
    rect_t r; r.x = PAD; r.y = PAD + FIELD_H + 10 + 28; r.w = panel_x() - 2 * PAD; r.h = g_win_h - STATUS_H - r.y - PAD; return r;
}

enum { W_SRC = 0, W_SCAN, W_UP, W_ALL, W_NONE,
       W_BMP, W_PNG, W_JPG, W_Q,
       W_RNONE, W_RFIT, W_REXACT, W_RSCALE, W_WF, W_HF, W_PF,
       W_PSCREEN, W_P1280, W_P640,
       W_OUT, W_WALL, W_CONVERT, W_CANCEL, W_COUNT };

static rect_t wrect(int id) {
    rect_t r; r.x = r.y = r.w = r.h = 0;
    int lw = panel_x() - 2 * PAD;
    int px = panel_x() + PAD, pw = PANEL_W - 2 * PAD;
    int b3 = (pw - 16) / 3, b4 = (pw - 24) / 4;
    int y = PAD;
    switch (id) {
        case W_SRC:  r.x = PAD + 60; r.y = y; r.w = lw - 60 - 2 * 58; r.h = FIELD_H; break;
        case W_SCAN: r.x = PAD + lw - 2 * 58 + 4; r.y = y; r.w = 54; r.h = FIELD_H; break;
        case W_UP:   r.x = PAD + lw - 54; r.y = y; r.w = 54; r.h = FIELD_H; break;
        case W_ALL:  r.x = PAD; r.y = y + FIELD_H + 10; r.w = 48; r.h = 24; break;
        case W_NONE: r.x = PAD + 52; r.y = y + FIELD_H + 10; r.w = 56; r.h = 24; break;
        default: break;
    }
    int py = PAD + 22;
    if (id >= W_BMP && id <= W_JPG) { r.x = px + (id - W_BMP) * (b3 + 8); r.y = py; r.w = b3; r.h = BTN_H; }
    if (id == W_Q) { r.x = px; r.y = py + BTN_H + 6; r.w = pw; r.h = 24; }
    py += BTN_H + 6 + 24 + 30;
    if (id >= W_RNONE && id <= W_RSCALE) { r.x = px + (id - W_RNONE) * (b4 + 8); r.y = py; r.w = b4; r.h = BTN_H; }
    py += BTN_H + 8;
    if (id == W_WF)  { r.x = px + 22; r.y = py; r.w = 70; r.h = FIELD_H; }
    if (id == W_HF)  { r.x = px + 22 + 70 + 30; r.y = py; r.w = 70; r.h = FIELD_H; }
    if (id == W_PF)  { r.x = px + 22 + 70 + 30 + 70 + 24; r.y = py; r.w = 54; r.h = FIELD_H; }
    py += FIELD_H + 8;
    if (id >= W_PSCREEN && id <= W_P640) { r.x = px + (id - W_PSCREEN) * (b3 + 8); r.y = py; r.w = b3; r.h = 24; }
    py += 24 + 30;
    if (id == W_OUT) { r.x = px; r.y = py; r.w = pw; r.h = FIELD_H; }
    py += FIELD_H + 8;
    if (id == W_WALL) { r.x = px; r.y = py; r.w = pw; r.h = 20; }
    int by = g_win_h - STATUS_H - PAD - BTN_H;
    if (id == W_CONVERT) { r.x = px; r.y = by; r.w = pw - 90 - 8; r.h = BTN_H; }
    if (id == W_CANCEL)  { r.x = px + pw - 90; r.y = by; r.w = 90; r.h = BTN_H; }
    return r;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void btn(int id, const char *lbl, int active, int enabled) {
    rect_t r = wrect(id);
    gui_state_t st = !enabled ? GUI_ST_DISABLED : point_in(r, hover_x, hover_y) ? GUI_ST_HOVER : GUI_ST_NORMAL;
    gui_button(win, r.x, r.y, r.w, r.h, lbl, active ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY, st);
}
static void field(int id, textfield_t *tf, int focus_id, const char *ph) {
    rect_t r = wrect(id);
    gui_textfield_tf(win, r.x, r.y, r.w, r.h, tf->buf, tf->len, tf->cursor, tf->sel_anchor,
                     g_focus == focus_id, ph);
}
static void label(int x, int y, const char *s, uint32_t c) { win_draw_text_ttf(win, x, y, s, GUI_TTF_SIZE, c); }

static void draw_list(void) {
    gui_palette_t *pal = gui_pal();
    rect_t L = list_rect();
    win_draw_rect(win, L.x, L.y, L.w, L.h, pal->field_bg);
    gui_draw_rect_outline(win, L.x, L.y, L.w, L.h, pal->border);
    gui_scroll_config(&g_sc, L.x + 1, L.y + 1, L.w - 2, L.h - 2, g_nitems * ROW_H, ROW_H);
    g_sc.snap = 1;
    int first = gui_scroll_first_item(&g_sc);
    int rows = (L.h - 2) / ROW_H + 1;
    int text_w = L.w - 2 - (gui_scroll_needed(&g_sc) ? GUI_SCROLL_W : 0);
    uint32_t ok_ink = gui_ensure_contrast(0x2FA84F, pal->field_bg, GUI_FLOOR_TEXT);
    uint32_t bad_ink = gui_ensure_contrast(theme_color(THEME_COLOR_CLOSE_BUTTON), pal->field_bg, GUI_FLOOR_TEXT);
    uint32_t ink = gui_ensure_contrast(pal->ink, pal->field_bg, GUI_FLOOR_TEXT);
    for (int i = 0; i < rows; i++) {
        int idx = first + i;
        if (idx >= g_nitems) break;
        int y = L.y + 1 + i * ROW_H - (g_sc.offset % ROW_H);
        if (y + ROW_H > L.y + L.h - 1) break;
        item_t *it = &g_items[idx];
        if (it->state == ST_BUSY) win_draw_rect(win, L.x + 1, y, text_w, ROW_H, gui_mix(pal->field_bg, pal->accent, 60));
        gui_checkbox(win, L.x + 8, y + 4, 16, it->sel ? true : false, "", GUI_ST_NORMAL);
        label(L.x + 32, y + (ROW_H - GUI_TTF_SIZE) / 2, it->name, ink);
        if (it->note[0]) {
            uint32_t c = it->state == ST_OK ? ok_ink : bad_ink;
            int nw = gui_ttf_width(it->note, GUI_TTF_SIZE);
            label(L.x + 1 + text_w - 8 - nw, y + (ROW_H - GUI_TTF_SIZE) / 2, it->note, c);
        }
    }
    if (g_nitems == 0) gui_text_ttf_centered(win, L.x, L.y, L.w, L.h, "No images listed. Type a folder and press Scan.", pal->ink_dim, GUI_TTF_SIZE);
    gui_scroll_draw_on(win, &g_sc, pal->field_bg);
}

static void draw_all(void) {
    if (win < 0) return;
    gui_palette_t *pal = gui_pal();
    win_draw_rect(win, 0, 0, g_win_w, g_win_h, pal->surface);

    // Source row
    label(PAD, PAD + (FIELD_H - GUI_TTF_SIZE) / 2, "Folder", pal->ink);
    field(W_SRC, &tf_src, FOCUS_SRC, "/");
    btn(W_SCAN, "Scan", 0, !g_run);
    btn(W_UP, "Up", 0, !g_run);
    btn(W_ALL, "All", 0, !g_run);
    btn(W_NONE, "None", 0, !g_run);
    {
        int nsel = 0; for (int i = 0; i < g_nitems; i++) nsel += g_items[i].sel;
        char s[48]; snprintf(s, sizeof(s), "%d of %d selected", nsel, g_nitems);
        label(PAD + 116, PAD + FIELD_H + 10 + (24 - GUI_TTF_SIZE) / 2, s, pal->ink_dim);
    }
    draw_list();

    // Panel
    int px = panel_x();
    win_draw_rect(win, px, 0, PANEL_W, g_win_h - STATUS_H, pal->surface_raised);
    win_draw_rect(win, px, 0, 1, g_win_h - STATUS_H, pal->border);
    int x = px + PAD;
    label(x, PAD, "Output format", pal->ink_dim);
    btn(W_BMP, "BMP", g_fmt == FMT_BMP, 1);
    btn(W_PNG, "PNG", g_fmt == FMT_PNG, 1);
    btn(W_JPG, "JPEG", g_fmt == FMT_JPEG, 1);
    char q[32]; snprintf(q, sizeof(q), "JPEG quality: %d  >", g_qtab[g_qidx]);
    btn(W_Q, q, 0, g_fmt == FMT_JPEG);

    rect_t rr = wrect(W_RNONE);
    label(x, rr.y - 22, "Resize", pal->ink_dim);
    btn(W_RNONE, "None", g_rs == RS_NONE, 1);
    btn(W_RFIT, "Fit", g_rs == RS_FIT, 1);
    btn(W_REXACT, "Exact", g_rs == RS_EXACT, 1);
    btn(W_RSCALE, "Scale", g_rs == RS_SCALE, 1);
    rect_t wf = wrect(W_WF), hf = wrect(W_HF), pf = wrect(W_PF);
    int fy = wf.y + (FIELD_H - GUI_TTF_SIZE) / 2;
    label(x, fy, "W", pal->ink);
    field(W_WF, &tf_w, FOCUS_W, "px");
    label(hf.x - 20, fy, "H", pal->ink);
    field(W_HF, &tf_h, FOCUS_H, "px");
    label(pf.x - 16, fy, "%", pal->ink);
    field(W_PF, &tf_pct, FOCUS_PCT, "50");
    btn(W_PSCREEN, "Screen", 0, 1);
    btn(W_P1280, "1280x800", 0, 1);
    btn(W_P640, "640x480", 0, 1);

    rect_t ro = wrect(W_OUT);
    label(x, ro.y - 22, "Output folder", pal->ink_dim);
    field(W_OUT, &tf_out, FOCUS_OUT, "<source>/CONVERTED");
    rect_t rw = wrect(W_WALL);
    gui_checkbox(win, rw.x, rw.y, 18, g_wall ? true : false, "Install BMP results as wallpapers (/)",
                 g_fmt == FMT_BMP ? GUI_ST_NORMAL : GUI_ST_DISABLED);

    // Progress + actions
    rect_t rc = wrect(W_CONVERT);
    int pct = g_run_total > 0 ? g_run_done * 100 / g_run_total : 0;
    gui_progress(win, x, rc.y - 10 - 16, PANEL_W - 2 * PAD, 16, pct);
    char pr[48];
    if (g_run || g_run_total > 0) snprintf(pr, sizeof(pr), "%d / %d  (%d ok, %d failed)", g_run_done, g_run_total, g_run_ok, g_run_fail);
    else snprintf(pr, sizeof(pr), "Ready");
    label(x, rc.y - 10 - 16 - 22, pr, pal->ink_dim);
    btn(W_CONVERT, g_run ? "Converting..." : "Convert selected", !g_run, !g_run && g_nitems > 0);
    btn(W_CANCEL, "Cancel", 0, g_run);

    // Status bar
    int sy = g_win_h - STATUS_H;
    win_draw_rect(win, 0, sy, g_win_w, STATUS_H, pal->surface_raised);
    win_draw_rect(win, 0, sy, g_win_w, 1, pal->border);
    label(PAD, sy + (STATUS_H - GUI_TTF_SIZE) / 2, g_status, pal->ink);
    win_invalidate(win);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static textfield_t *focus_tf(void) {
    switch (g_focus) {
        case FOCUS_SRC: return &tf_src; case FOCUS_OUT: return &tf_out;
        case FOCUS_W: return &tf_w; case FOCUS_H: return &tf_h; case FOCUS_PCT: return &tf_pct;
        default: return 0;
    }
}

static void preset(int w, int h) {
    snprintf(g_w, sizeof(g_w), "%d", w); snprintf(g_h, sizeof(g_h), "%d", h);
    tf_set_text(&tf_w, g_w); tf_set_text(&tf_h, g_h);
    if (g_rs == RS_NONE || g_rs == RS_SCALE) g_rs = RS_FIT;
}

static void handle_click(int x, int y) {
    // Text fields take focus; everything else drops it.
    if (point_in(wrect(W_SRC), x, y)) { g_focus = FOCUS_SRC; return; }
    if (point_in(wrect(W_OUT), x, y)) { g_focus = FOCUS_OUT; return; }
    if (point_in(wrect(W_WF), x, y))  { g_focus = FOCUS_W; tf_select_all(&tf_w); return; }
    if (point_in(wrect(W_HF), x, y))  { g_focus = FOCUS_H; tf_select_all(&tf_h); return; }
    if (point_in(wrect(W_PF), x, y))  { g_focus = FOCUS_PCT; tf_select_all(&tf_pct); return; }
    g_focus = FOCUS_NONE;

    if (!g_run) {
        if (point_in(wrect(W_SCAN), x, y)) { scan_dir(); return; }
        if (point_in(wrect(W_UP), x, y)) { dir_up(); return; }
        if (point_in(wrect(W_ALL), x, y)) { for (int i = 0; i < g_nitems; i++) g_items[i].sel = 1; return; }
        if (point_in(wrect(W_NONE), x, y)) { for (int i = 0; i < g_nitems; i++) g_items[i].sel = 0; return; }
        if (point_in(wrect(W_CONVERT), x, y)) { run_start(); return; }
    } else {
        if (point_in(wrect(W_CANCEL), x, y)) { g_run = 0; set_status("Cancelled"); return; }
    }
    if (point_in(wrect(W_BMP), x, y)) { g_fmt = FMT_BMP; return; }
    if (point_in(wrect(W_PNG), x, y)) { g_fmt = FMT_PNG; return; }
    if (point_in(wrect(W_JPG), x, y)) { g_fmt = FMT_JPEG; return; }
    if (point_in(wrect(W_Q), x, y) && g_fmt == FMT_JPEG) { g_qidx = (g_qidx + 1) % 4; return; }
    if (point_in(wrect(W_RNONE), x, y)) { g_rs = RS_NONE; return; }
    if (point_in(wrect(W_RFIT), x, y)) { g_rs = RS_FIT; return; }
    if (point_in(wrect(W_REXACT), x, y)) { g_rs = RS_EXACT; return; }
    if (point_in(wrect(W_RSCALE), x, y)) { g_rs = RS_SCALE; return; }
    if (point_in(wrect(W_PSCREEN), x, y)) {
        fb_info_t fi;
        if (fb_info(&fi) == 0 && fi.width > 0 && fi.height > 0) preset((int)fi.width, (int)fi.height);
        else set_status("Screen size unavailable");
        return;
    }
    if (point_in(wrect(W_P1280), x, y)) { preset(1280, 800); return; }
    if (point_in(wrect(W_P640), x, y)) { preset(640, 480); return; }
    if (point_in(wrect(W_WALL), x, y) && g_fmt == FMT_BMP) { g_wall = !g_wall; return; }

    // List rows: toggle selection
    rect_t L = list_rect();
    if (point_in(L, x, y) && !g_run) {
        if (gui_scroll_press(&g_sc, x, y)) return;
        int idx = gui_scroll_first_item(&g_sc) + (y - (L.y + 1) + (g_sc.offset % ROW_H)) / ROW_H;
        if (idx >= 0 && idx < g_nitems) g_items[idx].sel = !g_items[idx].sel;
    }
}

static void sync_field_buffers(void) {
    // textfield_t edits the caller's buffers in place; nothing to copy, but
    // the source/output strings drive scanning so keep them NUL-clean.
    g_src[sizeof(g_src) - 1] = '\0'; g_out[sizeof(g_out) - 1] = '\0';
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    // Default folder: the session user's home (root's home is "/").
    if (userhome_root(g_src, sizeof(g_src)) != 0 || g_src[0] == '\0') strlcpy(g_src, "/", sizeof(g_src));
    int single = 0;
    if (argc > 1 && argv[1][0] == '/') {
        // A file: list just it, from its own folder. A folder: scan it.
        const char *p = argv[1];
        if (is_image_name(p)) {
            int n = (int)strlen(p), cut = n;
            while (cut > 0 && p[cut - 1] != '/') cut--;
            if (cut <= 1) strlcpy(g_src, "/", sizeof(g_src));
            else { strlcpy(g_src, p, sizeof(g_src)); g_src[cut - 1] = '\0'; }
            strlcpy(g_items[0].name, p + cut, NAME_MAX_L);
            g_items[0].sel = 1; g_items[0].state = ST_PENDING; g_items[0].note[0] = '\0';
            g_nitems = 1; single = 1;
        } else strlcpy(g_src, p, sizeof(g_src));
    }
    tf_init(&tf_src, g_src, sizeof(g_src));
    tf_init(&tf_out, g_out, sizeof(g_out));
    tf_init(&tf_w, g_w, sizeof(g_w));
    tf_init(&tf_h, g_h, sizeof(g_h));
    tf_init(&tf_pct, g_pct, sizeof(g_pct));
    memset(&g_sc, 0, sizeof(g_sc));

    win = win_create(WIN_TITLE, 40, 20, WIN_W, WIN_H);
    if (win < 0) { printf("imgconv: failed to create window\n"); return 1; }
    apply_theme();
    if (single) { default_out_dir(); set_status("One file listed; choose a format and Convert"); }
    else scan_dir();
    draw_all();

    gui_event_t ev;
    int running = 1;
    while (running) {
        int got = win_get_event(win, &ev, g_run ? 1 : 250);
        if (got == 0) {
            if (g_run) { run_tick(); draw_all(); }
            continue;
        }
        switch (ev.type) {
            case EVENT_REDRAW: draw_all(); break;
            case EVENT_RESIZE:
                if (ev.mouse_x > 0 && ev.mouse_y > 0) { g_win_w = ev.mouse_x; g_win_h = ev.mouse_y; draw_all(); }
                break;
            case EVENT_WINDOW_CLOSE: running = 0; break;
            case EVENT_MOUSE_DOWN:
                if (!(ev.mouse_buttons & MOUSE_BUTTON_LEFT)) break;
                handle_click(ev.mouse_x, ev.mouse_y);
                sync_field_buffers();
                draw_all();
                break;
            case EVENT_MOUSE_MOVE: {
                hover_x = ev.mouse_x; hover_y = ev.mouse_y;
                int changed = gui_scroll_motion(&g_sc, ev.mouse_x, ev.mouse_y);
                if (changed || 1) draw_all();   // hover states on ~20 buttons; a redraw is cheap here
                break;
            }
            case EVENT_MOUSE_UP: gui_scroll_release(&g_sc); break;
            case EVENT_MOUSE_SCROLL:
                if (gui_scroll_wheel(&g_sc, ev.scroll_delta)) draw_all();
                break;
            case EVENT_KEY_DOWN: {
                char c = ev.key_char;
                textfield_t *tf = focus_tf();
                if (tf) {
                    if (c == 27) g_focus = FOCUS_NONE;
                    else if (c == '\t') g_focus = g_focus == FOCUS_PCT ? FOCUS_SRC : g_focus + 1;
                    else if (c == '\n' || c == '\r') {
                        if (g_focus == FOCUS_SRC) scan_dir();
                        g_focus = FOCUS_NONE;
                    } else tf_handle_key(tf, &ev);
                    sync_field_buffers();
                    draw_all();
                    break;
                }
                if (c == 27) { if (g_run) { g_run = 0; set_status("Cancelled"); } else running = 0; }
                else if (c == '\t') g_focus = FOCUS_SRC;
                else if (c == '\n' || c == '\r') run_start();
                else if (c == 'a' || c == 'A') { for (int i = 0; i < g_nitems; i++) g_items[i].sel = 1; }
                else if (c == 'n' || c == 'N') { for (int i = 0; i < g_nitems; i++) g_items[i].sel = 0; }
                else if (c == 'b' || c == 'B') g_fmt = FMT_BMP;
                else if (c == 'p' || c == 'P') g_fmt = FMT_PNG;
                else if (c == 'j' || c == 'J') g_fmt = FMT_JPEG;
                else if (c == 'r' || c == 'R') { if (!g_run) scan_dir(); }
                else if (c == 'u' || c == 'U') { if (!g_run) dir_up(); }
                else gui_scroll_key(&g_sc, ev.keycode);
                draw_all();
                break;
            }
            default: break;
        }
    }
    win_destroy(win);
    return 0;
}
