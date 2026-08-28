// snapshot - Maytera Snap: screen capture and annotation for MayteraOS.
//
// Fills a real gap in the media surface: the only way to take a screenshot
// was the msh `screenshot` builtin plus a manual file copy; there was no GUI
// tool at all, and no way to mark up a capture. Maytera Snap drives the
// existing compositor capture service (drop a /SCREENSHOT.REQ trigger file
// naming a target path; the compositor writes the composited frame there as
// a BMP once per frame), then loads the result and turns into a small
// annotation editor: pen, line, box, arrow, marker highlight, crop, six
// colors, three stroke widths, one-level undo, and numbered BMP export
// (/SNAP001.BMP, /SNAP002.BMP, ...).
//
// Capture options: immediate, 3 s or 10 s delay. While capturing, Snap
// minimizes its own window through the window-manager syscalls so the shot
// shows the desktop, then restores itself when the file lands. The whole
// flow is a small state machine driven from the event loop's timeout tick;
// there is no busy-wait anywhere.
//
// The compositor writes captures as 8-bit paletted BMPs (a deliberate size
// cap for remote pulls) which the kernel image decoder does not read, so
// Snap carries its own small BMP reader for 8 / 24 / 32-bit files and falls
// back to SYS_DECODE_IMAGE for PNG / JPEG when opening a file from argv.
// Annotated results are saved as standard 24-bit BMPs that Image Viewer,
// Gallery and Maytera Studio all open.
//
// UI follows the shared style engine (docs/UI_STYLE_GUIDE.md): live theme
// palette, raised toolbar / status cards, style-aware buttons, TTF text,
// resizable with reflow.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"
#include "../../libc/gui_theme.h"
#include "../../libc/gui_style.h"
#include "../../libc/fcntl.h"
#include "../../libc/userconf.h"  // #148 (local 164): userhome_path() - THE home join
#include "../../libc/tz.h"        // #148 (local 164): tz_local_stamp() - THE local-clock stamp

// ---------------------------------------------------------------------------
// Layout tokens
// ---------------------------------------------------------------------------
#define WIN_TITLE    "Maytera Snap"
#define WIN_W        820
#define WIN_H        560
#define TOOLBAR_H    44
#define STATUSBAR_H  28

#define TB_PAD       8
#define TB_BTN_Y     7
#define TB_BTN_H     30

// #148 (local 164, 2026-08-18): IMG_MAX_W/H were 1024x768, smaller than
// FB_MAX_W/H (1280x800) just below - a "screen capture" whose own working
// buffer cannot hold a screen-sized capture. That mismatch was latent until
// today: bmp_parse() (below) REJECTS anything wider/taller than IMG_MAX,
// so a full-resolution PrintScreen capture on this project's own common
// 1280x800 desktop silently failed to load for the new bottom-left preview
// (empirically found running this build - "No capture yet" with no error
// visible, load_image_file() returning -1). Raised to match FB_MAX_W/H,
// which already represents this file's own "the whole screen" ceiling (g_fb
// below), so a screenshot can never exceed the buffer meant to hold one.
#define IMG_MAX_W    1280
#define IMG_MAX_H    800
#define FB_MAX_W     1280
#define FB_MAX_H     800
// #148 (local 164, 2026-08-18): was 2MB, the SECOND buffer this same
// full-resolution PrintScreen preview blew past (see the IMG_MAX_W/H
// comment above). A 1280x800 24bpp uncompressed BMP is 54 + 1280*3*800 =
// 3,072,054 bytes - read_file() below refused anything at or past FBUF_CAP,
// so the file read itself failed silently before bmp_parse() ever ran,
// which is why raising IMG_MAX_W/H alone did not fix the empty preview
// (found by testing on a real VM, not by inspection - the first fix looked
// complete and still produced "No capture yet"). 4MB clears a full
// FB_MAX_W x FB_MAX_H 24bpp BMP with headroom for other formats.
#define FBUF_CAP     (4 * 1024 * 1024)

#define REQ_PATH     "/SCREENSHOT.REQ"
#define CAP_PATH     "/SNAPWORK.BMP"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static int win = -1;
static int g_win_w = WIN_W, g_win_h = WIN_H;

static uint32_t g_img[IMG_MAX_W * IMG_MAX_H];    // working image
static uint32_t g_undo[IMG_MAX_W * IMG_MAX_H];   // one-level undo
static int g_iw = 0, g_ih = 0;
static int g_undo_valid = 0;
static int g_undo_w = 0, g_undo_h = 0;           // dims saved with the undo copy

static uint8_t  g_fbuf[FBUF_CAP];                // raw file buffer
static uint32_t g_fb[FB_MAX_W * FB_MAX_H];       // offscreen window compose

// View mapping (image fitted into the view band between toolbar and status).
static int g_zoom = 100;      // percent
static int g_off_x = 0;       // view-space x of image origin
static int g_off_y = 0;       // view-space y (relative to view top)

// Tools
enum { TOOL_PEN = 0, TOOL_LINE, TOOL_BOX, TOOL_ARROW, TOOL_MARK, TOOL_CROP,
       TOOL_COUNT };
static int g_tool = TOOL_PEN;

static const uint32_t g_colors[6] = {
    0x00E03434,   // red
    0x00F2C230,   // yellow
    0x002FA84F,   // green
    0x002F6FE0,   // blue
    0x00FFFFFF,   // white
    0x00101010    // black
};
static int g_color = 0;

static const int g_sizes[3] = { 2, 4, 7 };
static const char *g_size_labels[3] = { "S", "M", "L" };
static int g_size = 0;

// Drag state (annotation)
static int g_drag = 0;
static int g_dx0, g_dy0, g_dx1, g_dy1;    // image-space drag anchors
static int g_pen_lx, g_pen_ly;            // last pen point

// Capture state machine
enum { CAP_IDLE = 0, CAP_COUNT, CAP_SETTLE, CAP_WAIT };
static int g_cap = CAP_IDLE;
static unsigned long g_cap_deadline = 0;   // CAP_COUNT: fire time (ms)
static unsigned long g_cap_t0 = 0;         // CAP_SETTLE / CAP_WAIT start
static int g_self_wm_id = -1;              // our WM window id while hidden
static int g_count_shown = -1;             // last countdown second drawn

static int hover_x = -1, hover_y = -1;
static char g_status[96] = "No capture yet";
static char g_saved[256] = "";             // last saved path (#148 local 164:
                                            // was char[32], too small for
                                            // <home>/SCREENSHOTS/SHOT-<stamp>.BMP)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
typedef struct { int x, y, w, h; } rect_t;

static int point_in(rect_t r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void set_status(const char *s) {
    int i = 0;
    while (s[i] && i < (int)sizeof(g_status) - 1) { g_status[i] = s[i]; i++; }
    g_status[i] = '\0';
}

static int iabs(int v) { return v < 0 ? -v : v; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static int isqrt32(int v) {
    if (v <= 0) return 0;
    int r = v, last = 0;
    // Newton iterations converge fast for our small magnitudes.
    for (int i = 0; i < 20 && r != last; i++) { last = r; r = (r + v / r) / 2; }
    return r < 1 ? 1 : r;
}

static unsigned int rd16(const uint8_t *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}
static unsigned int rd32(const uint8_t *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static int write_all(int fd, const void *buf, long n) {
    const uint8_t *p = (const uint8_t *)buf;
    long done = 0;
    while (done < n) {
        long w = sys_write(fd, p + done, n - done);
        if (w <= 0) return -1;
        done += w;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Theme (same pattern as Image Viewer / Settings)
// ---------------------------------------------------------------------------
static void apply_theme(void) {
    uint32_t win_bg   = theme_color(THEME_COLOR_WINDOW_BG);
    uint32_t fg       = theme_color(THEME_COLOR_LABEL_TEXT);
    uint32_t accent   = theme_color(THEME_COLOR_ACCENT);
    uint32_t border   = theme_color(THEME_COLOR_WINDOW_BORDER);
    uint32_t field_bg = theme_color(THEME_COLOR_TEXTBOX_BG);
    uint32_t track    = theme_color(THEME_COLOR_SCROLLBAR_BG);

    int classic = gui_theme_is_classic();
    gui_set_style(classic ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);

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

// ---------------------------------------------------------------------------
// BMP reader: 8-bit paletted (the compositor capture format) plus 24 / 32-bit.
// Returns 0 on success and fills g_img / g_iw / g_ih.
// ---------------------------------------------------------------------------
static int bmp_parse(const uint8_t *d, long n) {
    if (n < 54 || d[0] != 'B' || d[1] != 'M') return -1;
    unsigned int pix_off  = rd32(d + 10);
    unsigned int dib_size = rd32(d + 14);
    if (dib_size < 40) return -1;
    int w   = (int)rd32(d + 18);
    int hs  = (int)rd32(d + 22);
    int h   = hs < 0 ? -hs : hs;
    int bottom_up = hs > 0;
    unsigned int planes = rd16(d + 26);
    unsigned int bpp    = rd16(d + 28);
    unsigned int comp   = rd32(d + 30);
    if (planes != 1 || comp != 0) return -1;
    if (w <= 0 || h <= 0 || w > IMG_MAX_W || h > IMG_MAX_H) return -1;
    if (bpp != 8 && bpp != 24 && bpp != 32) return -1;

    // Palette for 8-bit (B, G, R, reserved entries right after the DIB header).
    uint32_t pal[256];
    if (bpp == 8) {
        unsigned int ncol = rd32(d + 46);
        if (ncol == 0 || ncol > 256) ncol = 256;
        const uint8_t *pp = d + 14 + dib_size;
        if ((long)(14 + dib_size + ncol * 4) > n) return -1;
        for (unsigned int i = 0; i < ncol; i++) {
            pal[i] = 0xFF000000u | ((uint32_t)pp[i * 4 + 2] << 16) |
                     ((uint32_t)pp[i * 4 + 1] << 8) | pp[i * 4 + 0];
        }
        for (unsigned int i = ncol; i < 256; i++) pal[i] = 0xFF000000u;
    }

    int bypp = (int)bpp / 8;
    long row_bytes = ((long)w * bypp + 3) & ~3L;
    if ((long)pix_off + row_bytes * h > n) return -1;

    for (int y = 0; y < h; y++) {
        int sy = bottom_up ? (h - 1 - y) : y;
        const uint8_t *row = d + pix_off + row_bytes * sy;
        uint32_t *dst = g_img + (long)y * w;
        if (bpp == 8) {
            for (int x = 0; x < w; x++) dst[x] = pal[row[x]];
        } else {
            for (int x = 0; x < w; x++) {
                const uint8_t *px = row + x * bypp;
                dst[x] = 0xFF000000u | ((uint32_t)px[2] << 16) |
                         ((uint32_t)px[1] << 8) | px[0];
            }
        }
    }
    g_iw = w;
    g_ih = h;
    return 0;
}

// ---------------------------------------------------------------------------
// File load / save
// ---------------------------------------------------------------------------
static long read_file(const char *path) {
    int fd = sys_open(path, 0);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long n = sys_read(fd, g_fbuf + total, FBUF_CAP - total);
        if (n < 0) { sys_close(fd); return -1; }
        if (n == 0) break;
        total += n;
        if (total >= (long)FBUF_CAP) { sys_close(fd); return -1; }
    }
    sys_close(fd);
    return total;
}

// Load any supported image into g_img: own BMP reader first (covers the
// 8-bit capture format), then the kernel decoder for PNG / JPEG / other BMPs.
static int load_image_file(const char *path) {
    long n = read_file(path);
    if (n <= 0) return -1;
    if (bmp_parse(g_fbuf, n) == 0) return 0;
    int dims[2] = {0, 0};
    int r = decode_image(g_fbuf, (unsigned int)n, IMG_MAX_W, IMG_MAX_H,
                         g_img, sizeof(g_img), dims);
    if (r <= 0 || dims[0] <= 0 || dims[1] <= 0) return -1;
    g_iw = dims[0];
    g_ih = dims[1];
    // Rows are packed at dims[0] stride already (decoder output), nothing to fix.
    return 0;
}

static int save_bmp24(const char *path) {
    if (g_iw <= 0 || g_ih <= 0) return -1;
    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;

    int row_bytes = (g_iw * 3 + 3) & ~3;
    unsigned int filesize = 54 + (unsigned int)row_bytes * (unsigned int)g_ih;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)filesize; hdr[3] = (uint8_t)(filesize >> 8);
    hdr[4] = (uint8_t)(filesize >> 16); hdr[5] = (uint8_t)(filesize >> 24);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (uint8_t)g_iw; hdr[19] = (uint8_t)(g_iw >> 8);
    hdr[20] = (uint8_t)(g_iw >> 16); hdr[21] = (uint8_t)(g_iw >> 24);
    hdr[22] = (uint8_t)g_ih; hdr[23] = (uint8_t)(g_ih >> 8);
    hdr[24] = (uint8_t)(g_ih >> 16); hdr[25] = (uint8_t)(g_ih >> 24);
    hdr[26] = 1;
    hdr[28] = 24;
    unsigned int isz = (unsigned int)row_bytes * (unsigned int)g_ih;
    hdr[34] = (uint8_t)isz; hdr[35] = (uint8_t)(isz >> 8);
    hdr[36] = (uint8_t)(isz >> 16); hdr[37] = (uint8_t)(isz >> 24);
    if (write_all(fd, hdr, 54) != 0) { sys_close(fd); return -1; }

    static uint8_t row[IMG_MAX_W * 3 + 4];
    for (int y = g_ih - 1; y >= 0; y--) {         // bottom-up
        const uint32_t *src = g_img + (long)y * g_iw;
        int o = 0;
        for (int x = 0; x < g_iw; x++) {
            uint32_t c = src[x];
            row[o++] = (uint8_t)(c & 0xFF);
            row[o++] = (uint8_t)((c >> 8) & 0xFF);
            row[o++] = (uint8_t)((c >> 16) & 0xFF);
        }
        while (o < row_bytes) row[o++] = 0;
        if (write_all(fd, row, row_bytes) != 0) { sys_close(fd); return -1; }
    }
    sys_close(fd);
    return 0;
}

// #148 (local 164, 2026-08-18): the owner's save-location decision, applied
// to Maytera Snap's own manual Save (was /SNAPnnn.BMP at the filesystem
// root, scattering captures across /). ONE directory for every screenshot,
// <home>/SCREENSHOTS, shared with the PrintScreen hotkey's own saves
// (compositor/screenshot.c) so there is one place to look, not two.
#define SAVE_SUB "SCREENSHOTS"

// Pick the first unused <home>/<dirsub>/SNAP-<local-stamp>.BMP slot (dirsub
// may be 0 for the bare <home>/SNAP-<stamp>.BMP fallback save_next() uses
// below). Same shape as the compositor's PrintScreen hotkey path
// (compositor/screenshot.c shot_hotkey_pick_name()), not a second design:
// timestamped via tz_local_stamp() (LOCAL time - every visible clock in this
// OS is local; see tz.h and the fuller rationale in screenshot.c), zero-
// padded so a lexical filename sort is also a chronological sort (a counter
// that reset to 1 on every boot never was), -2/-3 suffix on a same-second
// collision rather than an overwrite. Returns 0 and fills `out` (>= 256
// bytes) on success.
static int pick_save_name(const char *dirsub, char *out, unsigned long cap) {
    char stamp[TZ_STAMP_LEN];
    tz_local_stamp(stamp, sizeof(stamp));
    for (int suffix = 0; suffix <= 99; suffix++) {
        char name[40];
        if (suffix == 0) snprintf(name, sizeof(name), "SNAP-%s.BMP", stamp);
        else             snprintf(name, sizeof(name), "SNAP-%s-%d.BMP", stamp, suffix + 1);
        if (userhome_path(dirsub, name, out, cap) != 0) return -1;
        int fd = sys_open(out, 0);
        if (fd >= 0) { sys_close(fd); continue; }   // already taken this second
        return 0;
    }
    return -1;   // 100 saves in the same second without a gap: give up honestly
}

// Save the annotated capture to <home>/SCREENSHOTS. mkdir is idempotent (the
// same "create if missing, ignore if it's already there" idiom
// userconf_open_write() uses for <home>/CONFIG). On a genuine write failure
// (read-only media, full disk, a home tree mkdir could not actually create -
// deliverable: must not lose the capture or fail silently) falls back to a
// bare <home>/SNAP-<stamp>.BMP, no subdirectory required. Returns 0 and
// fills g_saved with wherever it actually landed.
static int save_next(void) {
    char dir[256];
    if (userhome_path(0, SAVE_SUB, dir, sizeof(dir)) == 0) {
        sys_mkdir(dir, 0755);
    }

    char path[256];
    if (pick_save_name(SAVE_SUB, path, sizeof(path)) == 0 &&
        save_bmp24(path) == 0) {
        strlcpy(g_saved, path, sizeof(g_saved));
        return 0;
    }
    if (pick_save_name(0, path, sizeof(path)) == 0 &&
        save_bmp24(path) == 0) {
        strlcpy(g_saved, path, sizeof(g_saved));   // fallback location took it
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// View mapping
// ---------------------------------------------------------------------------
static int view_h(void) { return g_win_h - TOOLBAR_H - STATUSBAR_H; }

static void compute_fit(void) {
    if (g_iw <= 0 || g_ih <= 0) { g_zoom = 100; g_off_x = g_off_y = 0; return; }
    int vw = g_win_w, vh = view_h();
    if (vw < 16) vw = 16;
    if (vh < 16) vh = 16;
    int zw = vw * 100 / g_iw;
    int zh = vh * 100 / g_ih;
    g_zoom = imin(zw, zh);
    if (g_zoom > 200) g_zoom = 200;
    if (g_zoom < 10) g_zoom = 10;
    g_off_x = (vw - g_iw * g_zoom / 100) / 2;
    g_off_y = (vh - g_ih * g_zoom / 100) / 2;
    if (g_off_x < 0) g_off_x = 0;
    if (g_off_y < 0) g_off_y = 0;
}

// View (window content) coords -> image coords. Returns 1 if inside image.
static int view_to_img(int vx, int vy, int *ix, int *iy) {
    if (g_iw <= 0) return 0;
    int x = (vx - g_off_x) * 100 / g_zoom;
    int y = (vy - TOOLBAR_H - g_off_y) * 100 / g_zoom;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= g_iw) x = g_iw - 1;
    if (y >= g_ih) y = g_ih - 1;
    *ix = x; *iy = y;
    return 1;
}

static int img_to_view_x(int ix) { return g_off_x + ix * g_zoom / 100; }
static int img_to_view_y(int iy) { return TOOLBAR_H + g_off_y + iy * g_zoom / 100; }

// ---------------------------------------------------------------------------
// Image-space drawing primitives (annotations render into g_img)
// ---------------------------------------------------------------------------
static void img_stamp(int cx, int cy, int r, uint32_t color) {
    uint32_t c = 0xFF000000u | color;
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= g_ih) continue;
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx;
            if (x < 0 || x >= g_iw) continue;
            if (dx * dx + dy * dy <= r * r) g_img[(long)y * g_iw + x] = c;
        }
    }
}

static void img_line(int x0, int y0, int x1, int y1, int r, uint32_t color) {
    int dx = iabs(x1 - x0), dy = -iabs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        img_stamp(x0, y0, r, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void img_box(int x0, int y0, int x1, int y1, int r, uint32_t color) {
    img_line(x0, y0, x1, y0, r, color);
    img_line(x1, y0, x1, y1, r, color);
    img_line(x1, y1, x0, y1, r, color);
    img_line(x0, y1, x0, y0, r, color);
}

static void img_arrow(int x0, int y0, int x1, int y1, int r, uint32_t color) {
    img_line(x0, y0, x1, y1, r, color);
    int dx = x1 - x0, dy = y1 - y0;
    int len = isqrt32(dx * dx + dy * dy);
    if (len < 2) return;
    int bx = -dx * 256 / len;     // unit vector (x256) pointing back
    int by = -dy * 256 / len;
    int hl = r * 4 + 10;          // head length
    // Rotate the back vector by +-30 degrees (cos = 222/256, sin = 128/256).
    int h1x = x1 + (bx * 222 - by * 128) / 256 * hl / 256;
    int h1y = y1 + (bx * 128 + by * 222) / 256 * hl / 256;
    int h2x = x1 + (bx * 222 + by * 128) / 256 * hl / 256;
    int h2y = y1 + (-bx * 128 + by * 222) / 256 * hl / 256;
    img_line(x1, y1, h1x, h1y, r, color);
    img_line(x1, y1, h2x, h2y, r, color);
}

// Marker: translucent highlight rectangle blended into the image.
static void img_mark(int x0, int y0, int x1, int y1, uint32_t color) {
    int ax = imax(0, imin(x0, x1)), bx = imin(g_iw - 1, imax(x0, x1));
    int ay = imax(0, imin(y0, y1)), by = imin(g_ih - 1, imax(y0, y1));
    for (int y = ay; y <= by; y++) {
        uint32_t *row = g_img + (long)y * g_iw;
        for (int x = ax; x <= bx; x++) {
            row[x] = 0xFF000000u | gui_mix(row[x] & 0x00FFFFFFu,
                                           color & 0x00FFFFFFu, 115);
        }
    }
}

static void img_crop(int x0, int y0, int x1, int y1) {
    int ax = imax(0, imin(x0, x1)), bx = imin(g_iw - 1, imax(x0, x1));
    int ay = imax(0, imin(y0, y1)), by = imin(g_ih - 1, imax(y0, y1));
    int nw = bx - ax + 1, nh = by - ay + 1;
    if (nw < 8 || nh < 8) return;                 // ignore accidental drags
    for (int y = 0; y < nh; y++) {
        memmove(g_img + (long)y * nw,
                g_img + (long)(ay + y) * g_iw + ax,
                (size_t)nw * 4);
    }
    g_iw = nw;
    g_ih = nh;
}

static void push_undo(void) {
    memcpy(g_undo, g_img, (size_t)g_iw * g_ih * 4);
    g_undo_w = g_iw;
    g_undo_h = g_ih;
    g_undo_valid = 1;
}

// Swap the working image and the undo copy, including their dimensions, so
// Undo also acts as Redo and stays correct across crops (the two buffers can
// legitimately hold different sizes).
static void undo_swap(void) {
    if (!g_undo_valid) return;
    long na = (long)g_iw * g_ih;
    long nb = (long)g_undo_w * g_undo_h;
    long n = na > nb ? na : nb;
    static uint32_t chunk[1024];
    long done = 0;
    while (done < n) {
        long c = n - done;
        if (c > 1024) c = 1024;
        memcpy(chunk, g_img + done, (size_t)c * 4);
        memcpy(g_img + done, g_undo + done, (size_t)c * 4);
        memcpy(g_undo + done, chunk, (size_t)c * 4);
        done += c;
    }
    int tw = g_iw, th = g_ih;
    g_iw = g_undo_w; g_ih = g_undo_h;
    g_undo_w = tw;   g_undo_h = th;
    compute_fit();
}

// ---------------------------------------------------------------------------
// Offscreen compose + blit (single SYS_WIN_BLIT like Image Viewer)
// ---------------------------------------------------------------------------
static void fb_pixel(int x, int y, int W, int H, uint32_t c) {
    if (x >= 0 && x < W && y >= 0 && y < H) g_fb[(long)y * W + x] = c;
}

static void fb_line(int x0, int y0, int x1, int y1, int W, int H, uint32_t c) {
    int dx = iabs(x1 - x0), dy = -iabs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        fb_pixel(x0, y0, W, H, c);
        fb_pixel(x0 + 1, y0, W, H, c);
        fb_pixel(x0, y0 + 1, W, H, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void fb_box(int x0, int y0, int x1, int y1, int W, int H, uint32_t c) {
    fb_line(x0, y0, x1, y0, W, H, c);
    fb_line(x1, y0, x1, y1, W, H, c);
    fb_line(x1, y1, x0, y1, W, H, c);
    fb_line(x0, y1, x0, y0, W, H, c);
}

// Compose the whole window content into g_fb and blit it in one syscall.
// Chrome (toolbar / status cards) is drawn on top afterwards.
static void compose_view(void) {
    gui_palette_t *pal = gui_pal();
    int W = imin(g_win_w, FB_MAX_W);
    int H = imin(g_win_h, FB_MAX_H);
    uint32_t surf = pal->surface | 0xFF000000u;
    uint32_t matte = gui_darken(pal->surface, 40) | 0xFF000000u;

    int vy0 = TOOLBAR_H;
    int vy1 = imax(vy0, H - STATUSBAR_H);

    for (int R = 0; R < H; R++) {
        uint32_t *dst = g_fb + (long)R * W;
        if (R < vy0 || R >= vy1) {
            for (int C = 0; C < W; C++) dst[C] = surf;
            continue;
        }
        if (g_iw <= 0) {
            for (int C = 0; C < W; C++) dst[C] = matte;
            continue;
        }
        int sy = (R - vy0 - g_off_y) * 100 / g_zoom;
        for (int C = 0; C < W; C++) {
            int sx = (C - g_off_x) * 100 / g_zoom;
            if (sx >= 0 && sx < g_iw && sy >= 0 && sy < g_ih &&
                (R - vy0 - g_off_y) >= 0 && (C - g_off_x) >= 0) {
                dst[C] = g_img[(long)sy * g_iw + sx] | 0xFF000000u;
            } else {
                dst[C] = matte;
            }
        }
    }

    // Rubber-band preview for shape tools, drawn in view space over the frame.
    if (g_drag && g_tool != TOOL_PEN && g_iw > 0) {
        uint32_t pc;
        if (g_tool == TOOL_CROP) pc = pal->accent | 0xFF000000u;
        else pc = g_colors[g_color] | 0xFF000000u;
        int vx0 = img_to_view_x(g_dx0), vy0i = img_to_view_y(g_dy0);
        int vx1 = img_to_view_x(g_dx1), vy1i = img_to_view_y(g_dy1);
        switch (g_tool) {
            case TOOL_LINE:
                fb_line(vx0, vy0i, vx1, vy1i, W, H, pc);
                break;
            case TOOL_ARROW:
                fb_line(vx0, vy0i, vx1, vy1i, W, H, pc);
                fb_box(vx1 - 3, vy1i - 3, vx1 + 3, vy1i + 3, W, H, pc);
                break;
            case TOOL_BOX:
            case TOOL_MARK:
            case TOOL_CROP:
                fb_box(vx0, vy0i, vx1, vy1i, W, H, pc);
                break;
            default:
                break;
        }
    }

    syscall5(SYS_WIN_BLIT, win, 0, 0,
             (W & 0xFFFF) | ((H & 0xFFFF) << 16), (long)g_fb);

    if (g_iw <= 0) {
        gui_text_ttf_centered(win, 0, TOOLBAR_H, g_win_w, view_h(),
                              "No capture yet. Snap grabs the screen; 3s / 10s add a delay.",
                              pal->ink_dim, GUI_TTF_SIZE);
    }
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------
enum {
    BTN_SNAP = 0, BTN_D3, BTN_D10,
    BTN_PEN, BTN_LINE, BTN_BOX, BTN_ARROW, BTN_MARK, BTN_CROP,
    BTN_SIZE, BTN_UNDO, BTN_SAVE,
    BTN_GALLERY,   // #148 (local 164): browse <home>/SCREENSHOTS in Gallery
    BTN_COUNT
};

#define SWATCH_SZ   18
#define SWATCH_Y    13

static rect_t toolbar_btn_rect(int id) {
    rect_t r; r.y = TB_BTN_Y; r.h = TB_BTN_H;
    switch (id) {
        case BTN_SNAP:  r.x = TB_PAD;        r.w = 52; break;
        case BTN_D3:    r.x = TB_PAD + 56;   r.w = 36; break;
        case BTN_D10:   r.x = TB_PAD + 96;   r.w = 40; break;
        case BTN_PEN:   r.x = TB_PAD + 148;  r.w = 44; break;
        case BTN_LINE:  r.x = TB_PAD + 196;  r.w = 44; break;
        case BTN_BOX:   r.x = TB_PAD + 244;  r.w = 44; break;
        case BTN_ARROW: r.x = TB_PAD + 292;  r.w = 44; break;
        case BTN_MARK:  r.x = TB_PAD + 340;  r.w = 48; break;
        case BTN_CROP:  r.x = TB_PAD + 392;  r.w = 48; break;
        case BTN_SIZE:  r.x = TB_PAD + 588;  r.w = 34; break;
        case BTN_UNDO:  r.x = TB_PAD + 626;  r.w = 50; break;
        case BTN_SAVE:    r.x = TB_PAD + 680;  r.w = 52; break;
        case BTN_GALLERY: r.x = TB_PAD + 736;  r.w = 72; break;
        default:          r.x = 0; r.w = 0; break;
    }
    return r;
}

static rect_t swatch_rect(int i) {
    rect_t r;
    r.x = TB_PAD + 448 + i * (SWATCH_SZ + 4);
    r.y = SWATCH_Y;
    r.w = SWATCH_SZ;
    r.h = SWATCH_SZ;
    return r;
}

static void draw_btn(int id, const char *label, int active) {
    rect_t r = toolbar_btn_rect(id);
    gui_state_t st = GUI_ST_NORMAL;
    if (point_in(r, hover_x, hover_y)) st = GUI_ST_HOVER;
    gui_button(win, r.x, r.y, r.w, r.h, label,
               active ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY, st);
}

static void draw_toolbar(void) {
    gui_palette_t *pal = gui_pal();
    win_draw_rect(win, 0, 0, g_win_w, TOOLBAR_H, pal->surface_raised);
    win_draw_rect(win, 0, TOOLBAR_H - 1, g_win_w, 1, pal->border);

    draw_btn(BTN_SNAP, "Snap", g_cap != CAP_IDLE);
    draw_btn(BTN_D3,   "3s",  0);
    draw_btn(BTN_D10,  "10s", 0);

    draw_btn(BTN_PEN,   "Pen",  g_tool == TOOL_PEN);
    draw_btn(BTN_LINE,  "Line", g_tool == TOOL_LINE);
    draw_btn(BTN_BOX,   "Box",  g_tool == TOOL_BOX);
    draw_btn(BTN_ARROW, "Arw",  g_tool == TOOL_ARROW);
    draw_btn(BTN_MARK,  "Mark", g_tool == TOOL_MARK);
    draw_btn(BTN_CROP,  "Crop", g_tool == TOOL_CROP);

    for (int i = 0; i < 6; i++) {
        rect_t s = swatch_rect(i);
        win_draw_rect(win, s.x, s.y, s.w, s.h, g_colors[i]);
        gui_draw_rect_outline(win, s.x, s.y, s.w, s.h, pal->border);
        if (i == g_color) {
            gui_draw_rect_outline(win, s.x - 2, s.y - 2, s.w + 4, s.h + 4,
                                  pal->accent);
        }
    }

    draw_btn(BTN_SIZE, g_size_labels[g_size], 0);
    draw_btn(BTN_UNDO, "Undo", 0);
    draw_btn(BTN_SAVE, "Save", 0);
    draw_btn(BTN_GALLERY, "Gallery", 0);
}

static void draw_statusbar(void) {
    gui_palette_t *pal = gui_pal();
    int sy = g_win_h - STATUSBAR_H;
    win_draw_rect(win, 0, sy, g_win_w, STATUSBAR_H, pal->surface_raised);
    win_draw_rect(win, 0, sy, g_win_w, 1, pal->border);
    int ty = sy + (STATUSBAR_H - GUI_TTF_SIZE) / 2;

    win_draw_text_ttf(win, TB_PAD, ty, g_status, GUI_TTF_SIZE, pal->ink);

    if (g_iw > 0) {
        char meta[64];
        char num[16];
        int m = 0;
        gui_itoa(g_iw, num, 8);
        for (int k = 0; num[k]; k++) meta[m++] = num[k];
        meta[m++] = ' '; meta[m++] = 'x'; meta[m++] = ' ';
        gui_itoa(g_ih, num, 8);
        for (int k = 0; num[k]; k++) meta[m++] = num[k];
        meta[m++] = ' '; meta[m++] = ' ';
        gui_itoa(g_zoom, num, 8);
        for (int k = 0; num[k]; k++) meta[m++] = num[k];
        meta[m++] = '%';
        meta[m] = '\0';
        int mw = gui_ttf_width(meta, GUI_TTF_SIZE);
        win_draw_text_ttf(win, g_win_w - TB_PAD - mw, ty, meta,
                          GUI_TTF_SIZE, pal->ink_dim);
    }
}

static void draw_all(void) {
    compose_view();
    draw_toolbar();
    draw_statusbar();
    win_invalidate(win);
}

// ---------------------------------------------------------------------------
// Capture state machine (driven from the event-loop timeout tick)
// ---------------------------------------------------------------------------
static int find_self_wm_id(void) {
    wm_window_info_t list[32];
    int n = wm_get_windows(list, 32);
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].title, WIN_TITLE) == 0) return list[i].id;
    }
    return -1;
}

static void start_capture(int delay_ms) {
    if (g_cap != CAP_IDLE) return;
    g_cap = CAP_COUNT;
    g_cap_deadline = uptime_ms() + (unsigned long)delay_ms;
    g_count_shown = -1;
    set_status(delay_ms > 0 ? "Capturing..." : "Capturing now...");
    draw_toolbar();
    draw_statusbar();
    win_invalidate(win);
}

static void capture_finish(int ok, const char *msg) {
    // Restore our window whatever happened.
    if (g_self_wm_id >= 0) {
        wm_focus(g_self_wm_id);
        g_self_wm_id = -1;
    }
    g_cap = CAP_IDLE;
    set_status(msg);
    if (ok) {
        g_undo_valid = 0;
        g_saved[0] = '\0';
        compute_fit();
    }
    draw_all();
}

static void capture_tick(void) {
    unsigned long now = uptime_ms();

    if (g_cap == CAP_COUNT) {
        if (now >= g_cap_deadline) {
            // Hide ourselves so the shot shows the desktop, then let the
            // compositor recompose a frame before we drop the trigger.
            g_self_wm_id = find_self_wm_id();
            if (g_self_wm_id >= 0) wm_minimize(g_self_wm_id);
            sys_unlink(CAP_PATH);
            g_cap = CAP_SETTLE;
            g_cap_t0 = now;
        } else {
            int remain = (int)((g_cap_deadline - now) / 1000) + 1;
            if (remain != g_count_shown) {
                g_count_shown = remain;
                char msg[32];
                int m = 0;
                const char *t = "Capturing in ";
                for (int k = 0; t[k]; k++) msg[m++] = t[k];
                if (remain >= 10) msg[m++] = (char)('0' + remain / 10);
                msg[m++] = (char)('0' + remain % 10);
                msg[m++] = '.'; msg[m++] = '.'; msg[m++] = '.';
                msg[m] = '\0';
                set_status(msg);
                draw_statusbar();
                win_invalidate(win);
            }
        }
        return;
    }

    if (g_cap == CAP_SETTLE) {
        if (now - g_cap_t0 < 300) return;   // one recomposed frame is enough
        int fd = sys_open(REQ_PATH, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            capture_finish(0, "Capture failed: cannot write request");
            return;
        }
        write_all(fd, CAP_PATH, (long)strlen(CAP_PATH));
        sys_close(fd);
        g_cap = CAP_WAIT;
        g_cap_t0 = now;
        return;
    }

    if (g_cap == CAP_WAIT) {
        // The compositor services the request once per frame; poll for the
        // finished file on our timeout cadence.
        long n = read_file(CAP_PATH);
        if (n > 54) {
            // Guard against catching the file mid-write: the header's file
            // size field must match what we actually read.
            unsigned int fsz = rd32(g_fbuf + 2);
            if ((long)fsz == n && bmp_parse(g_fbuf, n) == 0) {
                sys_unlink(CAP_PATH);
                char msg[48];
                int m = 0;
                const char *t = "Captured ";
                for (int k = 0; t[k]; k++) msg[m++] = t[k];
                char num[16];
                gui_itoa(g_iw, num, 8);
                for (int k = 0; num[k]; k++) msg[m++] = num[k];
                msg[m++] = 'x';
                gui_itoa(g_ih, num, 8);
                for (int k = 0; num[k]; k++) msg[m++] = num[k];
                msg[m] = '\0';
                capture_finish(1, msg);
                return;
            }
        }
        if (now - g_cap_t0 > 8000) {
            capture_finish(0, "Capture timed out (compositor not running?)");
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Toolbar interaction
// ---------------------------------------------------------------------------
static int handle_toolbar_click(int x, int y) {
    for (int i = 0; i < 6; i++) {
        if (point_in(swatch_rect(i), x, y)) {
            g_color = i;
            draw_toolbar();
            win_invalidate(win);
            return 1;
        }
    }
    if (point_in(toolbar_btn_rect(BTN_SNAP), x, y)) { start_capture(0);     return 1; }
    if (point_in(toolbar_btn_rect(BTN_D3), x, y))   { start_capture(3000);  return 1; }
    if (point_in(toolbar_btn_rect(BTN_D10), x, y))  { start_capture(10000); return 1; }

    static const int tool_btn[TOOL_COUNT] =
        { BTN_PEN, BTN_LINE, BTN_BOX, BTN_ARROW, BTN_MARK, BTN_CROP };
    for (int t = 0; t < TOOL_COUNT; t++) {
        if (point_in(toolbar_btn_rect(tool_btn[t]), x, y)) {
            g_tool = t;
            draw_toolbar();
            win_invalidate(win);
            return 1;
        }
    }

    if (point_in(toolbar_btn_rect(BTN_SIZE), x, y)) {
        g_size = (g_size + 1) % 3;
        draw_toolbar();
        win_invalidate(win);
        return 1;
    }
    if (point_in(toolbar_btn_rect(BTN_UNDO), x, y)) {
        if (g_undo_valid) {
            undo_swap();
            set_status("Undone");
            draw_all();
        }
        return 1;
    }
    if (point_in(toolbar_btn_rect(BTN_SAVE), x, y)) {
        if (g_iw > 0) {
            if (save_next() == 0) {
                // #148 (local 164): g_saved now holds a full <home>/SCREENSHOTS/...
                // path (up to 255 bytes), not a 12-byte /SNAPnnn.BMP - the old
                // manual, unbounded char-by-char copy into msg[48] was a real
                // stack overflow waiting for a long home path. snprintf into a
                // buffer sized for the worst case truncates safely instead.
                char msg[300];
                snprintf(msg, sizeof(msg), "Saved %s", g_saved);
                set_status(msg);
            } else {
                set_status("Save failed");
            }
            draw_statusbar();
            win_invalidate(win);
        }
        return 1;
    }
    // #148 (local 164): "Gallery" - the reuse decision for deliverable 3.
    // Gallery already supports being launched pointed at an arbitrary
    // directory (userland/apps/gallery/main.c: `if (argc > 1 && argv[1][0]
    // == '/') strlcpy(g_path, argv[1], ...)`), so this is the SAME
    // sys_spawn_args(path, av, 2) shape Files/desktop.c already use to open
    // an app on a specific folder - not a second image-grid browser built
    // inside Snapshot, which the ticket explicitly asked to avoid unless
    // reuse were "genuinely unworkable" (it wasn't).
    if (point_in(toolbar_btn_rect(BTN_GALLERY), x, y)) {
        char dir[256];
        if (userhome_path(0, SAVE_SUB, dir, sizeof(dir)) == 0) {
            char *av[2];
            av[0] = (char *)"/APPS/GALLERY";
            av[1] = dir;
            if (sys_spawn_args("/APPS/GALLERY", av, 2) < 0) {
                set_status("Could not open Gallery");
                draw_statusbar();
                win_invalidate(win);
            }
        }
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Annotation interaction
// ---------------------------------------------------------------------------
static void stroke_begin(int vx, int vy) {
    if (g_iw <= 0) return;
    int ix, iy;
    if (!view_to_img(vx, vy, &ix, &iy)) return;
    // Crop keeps the pre-crop image in undo; strokes keep the pre-stroke one.
    push_undo();
    g_drag = 1;
    g_dx0 = g_dx1 = ix;
    g_dy0 = g_dy1 = iy;
    if (g_tool == TOOL_PEN) {
        g_pen_lx = ix;
        g_pen_ly = iy;
        img_stamp(ix, iy, g_sizes[g_size], g_colors[g_color]);
        draw_all();
    }
}

static void stroke_move(int vx, int vy) {
    if (!g_drag || g_iw <= 0) return;
    int ix, iy;
    if (!view_to_img(vx, vy, &ix, &iy)) return;
    g_dx1 = ix;
    g_dy1 = iy;
    if (g_tool == TOOL_PEN) {
        img_line(g_pen_lx, g_pen_ly, ix, iy, g_sizes[g_size], g_colors[g_color]);
        g_pen_lx = ix;
        g_pen_ly = iy;
    }
    draw_all();   // pen: committed pixels; shapes: rubber-band preview
}

static void stroke_end(void) {
    if (!g_drag) return;
    g_drag = 0;
    int r = g_sizes[g_size];
    switch (g_tool) {
        case TOOL_LINE:  img_line(g_dx0, g_dy0, g_dx1, g_dy1, r, g_colors[g_color]); break;
        case TOOL_BOX:   img_box(g_dx0, g_dy0, g_dx1, g_dy1, r, g_colors[g_color]); break;
        case TOOL_ARROW: img_arrow(g_dx0, g_dy0, g_dx1, g_dy1, r, g_colors[g_color]); break;
        case TOOL_MARK:  img_mark(g_dx0, g_dy0, g_dx1, g_dy1, g_colors[g_color]); break;
        case TOOL_CROP:
            img_crop(g_dx0, g_dy0, g_dx1, g_dy1);
            compute_fit();
            set_status("Cropped");
            break;
        default: break;
    }
    draw_all();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
// #148 (local 164, 2026-08-18): the PrintScreen-on-a-normal-desktop path
// (owner spec: "open the screenshot app with the preview showing... bottom
// left of the screen... maintaining the current window focus"). A THIRD argv
// slot, not a second meaning for argv[1] - argv[1] alone already means
// "open this file for markup" (Files app's existing Open With path, unchanged
// by this), so this needs its own opt-in marker or every "Open With Snapshot"
// launch would also jump to the corner and refuse focus.
#define PREVIEW_ARG "--preview"

// No syscall today lets a userland app query the reserved taskbar/dock work
// area (SYS_WM_SET_WORK_AREA in syscall.h is a compositor-only SETTER, no
// GETTER exists) - a real, reported gap, not fudged here. PREVIEW_BOTTOM_GAP
// is a conservative fixed margin (compositor.h's default-dock TASKBAR_HEIGHT
// is 36px; this clears that plus room) that will not overlap the DEFAULT dock
// style but is not guaranteed against every one of the 5 dock styles/heights.
#define PREVIEW_LEFT_MARGIN  20
#define PREVIEW_BOTTOM_GAP   60

int main(int argc, char **argv) {
    int preview_mode = (argc > 2 && strcmp(argv[2], PREVIEW_ARG) == 0);

    if (preview_mode) {
        int px = PREVIEW_LEFT_MARGIN, py = 24;
        fb_info_t fi;
        if (fb_info(&fi) == 0 && fi.height > 0) {
            py = (int)fi.height - WIN_H - PREVIEW_BOTTOM_GAP;
            if (py < 0) py = 0;
        }
        // win_create_bg(): does NOT take keyboard focus (kernel/proc/
        // syscall.h SYS_WIN_CREATE_BG, #148 local 164) - whatever window the
        // user was typing into keeps it.
        win = win_create_bg(WIN_TITLE, px, py, WIN_W, WIN_H);
    } else {
        win = win_create(WIN_TITLE, 50, 24, WIN_W, WIN_H);
    }
    if (win < 0) {
        printf("snapshot: failed to create window\n");
        return 1;
    }

    apply_theme();

    if (argc > 1 && argv[1][0] == '/') {
        if (load_image_file(argv[1]) == 0) {
            compute_fit();
            set_status(preview_mode ? "Screenshot saved - preview" : "Opened image for markup");
        } else {
            set_status("Could not open image");
        }
    }

    // Show the physical screen size in the empty state; handy context for a
    // capture tool. fb_info is a read-only query open to any process.
    if (g_iw <= 0) {
        fb_info_t fi;
        if (fb_info(&fi) == 0 && fi.width > 0) {
            char msg[64];
            int m = 0;
            const char *t = "Ready. Screen ";
            for (int k = 0; t[k]; k++) msg[m++] = t[k];
            char num[16];
            gui_itoa((long)fi.width, num, 8);
            for (int k = 0; num[k]; k++) msg[m++] = num[k];
            msg[m++] = 'x';
            gui_itoa((long)fi.height, num, 8);
            for (int k = 0; num[k]; k++) msg[m++] = num[k];
            msg[m] = '\0';
            set_status(msg);
        }
    }

    draw_all();

    gui_event_t ev;
    int running = 1;
    while (running) {
        int got = win_get_event(win, &ev, 60);
        if (got == 0) {
            if (g_cap != CAP_IDLE) capture_tick();
            continue;
        }

        switch (ev.type) {
            case EVENT_REDRAW:
                draw_all();
                break;

            case EVENT_RESIZE:
                if (ev.mouse_x > 0 && ev.mouse_y > 0) {
                    g_win_w = ev.mouse_x;
                    g_win_h = ev.mouse_y;
                    compute_fit();
                    draw_all();
                }
                break;

            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;

            case EVENT_MOUSE_DOWN:
                if (!(ev.mouse_buttons & MOUSE_BUTTON_LEFT)) break;
                if (ev.mouse_y < TOOLBAR_H) {
                    handle_toolbar_click(ev.mouse_x, ev.mouse_y);
                } else if (ev.mouse_y < g_win_h - STATUSBAR_H) {
                    stroke_begin(ev.mouse_x, ev.mouse_y);
                }
                break;

            case EVENT_MOUSE_MOVE: {
                if (g_drag) {
                    stroke_move(ev.mouse_x, ev.mouse_y);
                } else {
                    int was_over = (hover_y >= 0 && hover_y < TOOLBAR_H);
                    int now_over = (ev.mouse_y >= 0 && ev.mouse_y < TOOLBAR_H);
                    hover_x = ev.mouse_x;
                    hover_y = ev.mouse_y;
                    if (now_over || was_over) {
                        draw_toolbar();
                        win_invalidate(win);
                    }
                }
                break;
            }

            case EVENT_MOUSE_UP:
                stroke_end();
                break;

            case EVENT_KEY_DOWN: {
                char c = ev.key_char;
                if (c == 27) running = 0;
                else if (c == 'c' || c == 'C') start_capture(0);
                else if (c == '3') start_capture(3000);
                else if (c == 't' || c == 'T') start_capture(10000);
                else if (c == 'p' || c == 'P') { g_tool = TOOL_PEN;   draw_toolbar(); win_invalidate(win); }
                else if (c == 'l' || c == 'L') { g_tool = TOOL_LINE;  draw_toolbar(); win_invalidate(win); }
                else if (c == 'b' || c == 'B') { g_tool = TOOL_BOX;   draw_toolbar(); win_invalidate(win); }
                else if (c == 'a' || c == 'A') { g_tool = TOOL_ARROW; draw_toolbar(); win_invalidate(win); }
                else if (c == 'm' || c == 'M') { g_tool = TOOL_MARK;  draw_toolbar(); win_invalidate(win); }
                else if (c == 'x' || c == 'X') { g_tool = TOOL_CROP;  draw_toolbar(); win_invalidate(win); }
                else if (c == 'z' || c == 'Z') {
                    rect_t r = toolbar_btn_rect(BTN_UNDO);
                    handle_toolbar_click(r.x + 1, r.y + 1);
                }
                else if (c == 's' || c == 'S') {
                    rect_t r = toolbar_btn_rect(BTN_SAVE);
                    handle_toolbar_click(r.x + 1, r.y + 1);
                }
                break;
            }

            default:
                break;
        }
    }

    win_destroy(win);
    return 0;
}
