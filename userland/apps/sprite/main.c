// sprite - Sprite Studio: a pixel-art and sprite-animation editor for MayteraOS.
//
// WHY A SECOND RASTER EDITOR. Maytera Studio (apps/paint) is a free-size
// image editor: brushes, filters, layers, JPEG export. Pixel art wants the
// opposite tool: a tiny fixed canvas magnified 8-40x with a hard grid,
// single-pixel tools that never antialias, an indexed palette, per-pixel
// transparency, a horizontal-mirror mode, and FRAMES with onion skin and a
// live playback preview so a walk cycle can be drawn and checked in place.
// None of that existed anywhere in the tree (measured: no app or libc source
// mentions a sprite sheet, onion skin or frame strip before this one).
//
// What it does:
//   * Canvas 8x8 .. 128x128, up to 32 frames, alpha-0 transparency drawn
//     over a checkerboard. Pencil, eraser, flood fill, line, rectangle,
//     filled box, eyedropper; right button always erases; X-mirror; grid.
//   * Four built-in 16-colour palettes (PICO-8, DB16, Sweetie 16, Endesga
//     16) plus any colour by hex entry. Sixteen-deep undo across frames.
//   * Frame strip with add / duplicate / delete, onion skin of the previous
//     frame, playback at 4 / 8 / 12 / 24 fps into a 4x preview, all driven
//     from the event loop's timeout tick. There is no busy-wait anywhere:
//     idle the app parks in win_get_event().
//   * Save writes a PNG SPRITE SHEET (frames left to right, RGBA, so the
//     transparency survives) under <home>/SPRITES; Export writes the current
//     frame as a 24-bit BMP with transparent pixels flattened to the classic
//     magenta key colour (FF00FF), which is what a sprite blitter expects.
//     Opening a PNG/BMP (argv[1], e.g. Files "Open With") splits a sheet
//     whose width is a whole multiple of its height back into frames.
//
// Real syscalls used: SYS_WIN_* (window, blit, TTF text), SYS_DECODE_IMAGE
// (open), SYS_OPEN/READ/WRITE/MKDIR (save), SYS_UPTIME (playback clock).
// Encoders are ../imgconv/imgenc.c (shared with Image Converter).
//
// UI follows the shared style engine (docs/UI_STYLE_GUIDE.md): live theme
// palette, raised toolbar / cards, style-aware buttons, TTF text, resizable.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"
#include "../../libc/gui_theme.h"
#include "../../libc/gui_style.h"
#include "../../libc/fcntl.h"
#include "../../libc/stdlib.h"
#include "../../libc/userconf.h"   // userhome_path(): THE home join
#include "../../libc/tz.h"         // tz_local_stamp(): THE local-clock stamp
#include "../imgconv/imgenc.h"

// ---------------------------------------------------------------------------
// Layout tokens
// ---------------------------------------------------------------------------
#define WIN_TITLE    "Sprite Studio"
#define WIN_W        980
#define WIN_H        660
#define TOOLBAR_H    44
#define STATUS_H     26
#define STRIP_H      84
#define PANEL_W      236
#define TB_PAD       8
#define TB_BTN_Y     7
#define TB_BTN_H     30

#define FB_MAX_W     1280
#define FB_MAX_H     800

#define MAX_W        128
#define MAX_H        128
#define MAX_FRAMES   32
#define UNDO_DEPTH   16
#define THUMB        56
#define THUMB_STEP   64
#define FILE_CAP     (8L * 1024 * 1024)

#define SAVE_SUB     "SPRITES"
#define KEY_MAGENTA  0xFFFF00FFu

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static int win = -1;
static int g_win_w = WIN_W, g_win_h = WIN_H;

static uint32_t g_frames[MAX_FRAMES][MAX_W * MAX_H];   // 0xAARRGGBB, A=0 clear
static int g_nframes = 1, g_cur = 0;
static int g_cw = 16, g_ch = 16;

typedef struct { int frame; int w, h; uint32_t px[MAX_W * MAX_H]; } undo_t;
static undo_t g_undo[UNDO_DEPTH];
static int g_undo_n = 0, g_undo_top = 0;   // ring: top is next write slot

static uint32_t g_fb[FB_MAX_W * FB_MAX_H];  // whole-window offscreen compose

enum { TOOL_PEN = 0, TOOL_ERASE, TOOL_FILL, TOOL_LINE, TOOL_RECT, TOOL_BOX,
       TOOL_PICK, TOOL_COUNT };
static int g_tool = TOOL_PEN;
static int g_mirror = 0, g_grid = 1, g_onion = 1;
static uint32_t g_color = 0xFF000000u;

// Palettes: sixteen entries each, 0xRRGGBB.
typedef struct { const char *name; uint32_t c[16]; } palette_t;
static const palette_t g_palettes[] = {
    { "PICO-8", { 0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F,
                  0xC2C3C7, 0xFFF1E8, 0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436,
                  0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA } },
    { "DB16",   { 0x140C1C, 0x442434, 0x30346D, 0x4E4A4E, 0x854C30, 0x346524,
                  0xD04648, 0x757161, 0x597DCE, 0xD27D2C, 0x8595A1, 0x6DAA2C,
                  0xD2AA99, 0x6DC2CA, 0xDAD45E, 0xDEEED6 } },
    { "Sweetie 16", { 0x1A1C2C, 0x5D275D, 0xB13E53, 0xEF7D57, 0xFFCD75, 0xA7F070,
                      0x38B764, 0x257179, 0x29366F, 0x3B5DC9, 0x41A6F6, 0x73EFF7,
                      0xF4F4F4, 0x94B0C2, 0x566C86, 0x333C57 } },
    { "Endesga 16", { 0xE4A672, 0xB86F50, 0x743F39, 0x3F2832, 0x9E2835, 0xE53B44,
                      0xFB922B, 0xFFE762, 0x63C64D, 0x327345, 0x193D3F, 0x4F6781,
                      0xAFBFD2, 0xFFFFFF, 0x2CE8F4, 0x0484D1 } },
};
#define N_PALETTES ((int)(sizeof(g_palettes) / sizeof(g_palettes[0])))
static int g_pal = 0;

// View
static int g_zoom = 8;          // effective pixel size
static int g_zoom_manual = 0;   // 0 = auto-fit; otherwise the wheel's choice
static int g_cx0 = 0, g_cy0 = 0; // window coords of canvas origin
static int g_strip_first = 0;   // first thumbnail shown in the strip

// Interaction
static int g_drag = 0, g_drag_btn = 0;
static int g_dx0, g_dy0, g_dx1, g_dy1;   // cell anchors for shape tools
static int g_last_x = -1, g_last_y = -1;
static int hover_x = -1, hover_y = -1;
static int g_hover_cell_x = -1, g_hover_cell_y = -1;

// Hex colour field (shared caret/selection/clipboard widget from textfield.h)
static char g_hex_buf[12] = "000000";
static textfield_t g_hex_tf;
static int g_hex_focus = 0;

// Playback
static int g_playing = 0;
static int g_fps_idx = 1;
static const int g_fps_tab[4] = { 4, 8, 12, 24 };
static int g_play_frame = 0;
static unsigned long g_play_next = 0;

static char g_status[128] = "New 16x16 sprite";
static char g_doc_path[256] = "";
static int g_dirty = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
typedef struct { int x, y, w, h; } rect_t;
static int point_in(rect_t r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iabs(int v) { return v < 0 ? -v : v; }

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

static uint32_t *cur(void) { return g_frames[g_cur]; }

static void hex_from_color(void) {
    static const char *hx = "0123456789ABCDEF";
    uint32_t c = g_color & 0xFFFFFF;
    for (int i = 0; i < 6; i++) g_hex_buf[i] = hx[(c >> (20 - i * 4)) & 0xF];
    g_hex_buf[6] = '\0';
    tf_set_text(&g_hex_tf, g_hex_buf);
}

static int hex_apply(void) {
    const char *s = g_hex_buf;
    if (*s == '#') s++;
    uint32_t v = 0; int n = 0;
    for (; *s && n < 6; s++, n++) {
        char c = *s; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = (v << 4) | (uint32_t)d;
    }
    if (n == 3) {   // #RGB shorthand
        uint32_t r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
        v = (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b;
    } else if (n != 6) return -1;
    g_color = 0xFF000000u | v;
    hex_from_color();
    return 0;
}

// ---------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------
static void push_undo(void) {
    undo_t *u = &g_undo[g_undo_top];
    u->frame = g_cur; u->w = g_cw; u->h = g_ch;
    memcpy(u->px, cur(), (size_t)g_cw * g_ch * 4);
    g_undo_top = (g_undo_top + 1) % UNDO_DEPTH;
    if (g_undo_n < UNDO_DEPTH) g_undo_n++;
    g_dirty = 1;
}

static int pop_undo(void) {
    if (g_undo_n == 0) return 0;
    g_undo_top = (g_undo_top + UNDO_DEPTH - 1) % UNDO_DEPTH;
    g_undo_n--;
    undo_t *u = &g_undo[g_undo_top];
    if (u->frame >= g_nframes) u->frame = g_nframes - 1;
    if (u->w == g_cw && u->h == g_ch) {
        memcpy(g_frames[u->frame], u->px, (size_t)g_cw * g_ch * 4);
        g_cur = u->frame;
        return 1;
    }
    return 0;   // canvas was resized since; nothing sensible to restore
}

// ---------------------------------------------------------------------------
// Canvas editing (cell space)
// ---------------------------------------------------------------------------
static void plot1(uint32_t *f, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= g_cw || y >= g_ch) return;
    f[y * g_cw + x] = c;
}
static void plot(int x, int y, uint32_t c) {
    plot1(cur(), x, y, c);
    if (g_mirror) plot1(cur(), g_cw - 1 - x, y, c);
}

static void cell_line(int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = iabs(x1 - x0), dy = -iabs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void cell_rect(int x0, int y0, int x1, int y1, uint32_t c, int filled) {
    int ax = imin(x0, x1), bx = imax(x0, x1), ay = imin(y0, y1), by = imax(y0, y1);
    for (int y = ay; y <= by; y++)
        for (int x = ax; x <= bx; x++)
            if (filled || y == ay || y == by || x == ax || x == bx) plot(x, y, c);
}

static int g_fill_stack[MAX_W * MAX_H];
static void cell_fill(int sx, int sy, uint32_t c) {
    uint32_t *f = cur();
    if (sx < 0 || sy < 0 || sx >= g_cw || sy >= g_ch) return;
    uint32_t target = f[sy * g_cw + sx];
    if (target == c) return;
    int sp = 0;
    g_fill_stack[sp++] = sy * g_cw + sx;
    while (sp > 0) {
        int i = g_fill_stack[--sp];
        if (f[i] != target) continue;
        int x = i % g_cw, y = i / g_cw;
        f[i] = c;
        if (x > 0 && f[i - 1] == target) g_fill_stack[sp++] = i - 1;
        if (x < g_cw - 1 && f[i + 1] == target) g_fill_stack[sp++] = i + 1;
        if (y > 0 && f[i - g_cw] == target) g_fill_stack[sp++] = i - g_cw;
        if (y < g_ch - 1 && f[i + g_cw] == target) g_fill_stack[sp++] = i + g_cw;
        if (sp > MAX_W * MAX_H - 4) break;   // cannot happen with 4-connectivity, kept as a guard
    }
    if (g_mirror) {
        // Mirror the fill result across the axis so the two halves stay in step.
        for (int y = 0; y < g_ch; y++)
            for (int x = 0; x < g_cw / 2; x++) {
                if (f[y * g_cw + x] == c) f[y * g_cw + (g_cw - 1 - x)] = c;
                else if (f[y * g_cw + (g_cw - 1 - x)] == c) f[y * g_cw + x] = c;
            }
    }
}

static void resize_canvas(int nw, int nh) {
    nw = imax(1, imin(MAX_W, nw));
    nh = imax(1, imin(MAX_H, nh));
    if (nw == g_cw && nh == g_ch) return;
    static uint32_t tmp[MAX_W * MAX_H];
    for (int fi = 0; fi < g_nframes; fi++) {
        memset(tmp, 0, sizeof(tmp));
        for (int y = 0; y < imin(nh, g_ch); y++)
            for (int x = 0; x < imin(nw, g_cw); x++)
                tmp[y * nw + x] = g_frames[fi][y * g_cw + x];
        memcpy(g_frames[fi], tmp, (size_t)nw * nh * 4);
    }
    g_cw = nw; g_ch = nh;
    g_undo_n = 0;   // undo snapshots carry the old geometry; drop them honestly
    g_dirty = 1;
}

static void new_document(int w, int h) {
    memset(g_frames, 0, sizeof(g_frames));
    g_nframes = 1; g_cur = 0; g_cw = w; g_ch = h;
    g_undo_n = 0; g_undo_top = 0; g_dirty = 0; g_doc_path[0] = '\0';
    g_playing = 0; g_strip_first = 0;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------
static void frame_insert(int at, int dup_from) {
    if (g_nframes >= MAX_FRAMES) { set_status("Frame limit reached (32)"); return; }
    for (int i = g_nframes; i > at; i--) memcpy(g_frames[i], g_frames[i - 1], sizeof(g_frames[0]));
    if (dup_from >= 0) memcpy(g_frames[at], g_frames[dup_from >= at ? dup_from + 1 : dup_from], sizeof(g_frames[0]));
    else memset(g_frames[at], 0, sizeof(g_frames[0]));
    g_nframes++;
    g_cur = at;
    g_undo_n = 0;   // frame indices in the ring no longer line up
    g_dirty = 1;
}

static void frame_delete(int at) {
    if (g_nframes <= 1) {
        memset(g_frames[0], 0, sizeof(g_frames[0]));
        set_status("Cleared the only frame");
        g_dirty = 1;
        return;
    }
    for (int i = at; i < g_nframes - 1; i++) memcpy(g_frames[i], g_frames[i + 1], sizeof(g_frames[0]));
    g_nframes--;
    if (g_cur >= g_nframes) g_cur = g_nframes - 1;
    g_undo_n = 0;
    g_dirty = 1;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
static int ext_is(const char *path, const char *ext) {
    int n = (int)strlen(path), m = (int)strlen(ext);
    if (n < m) return 0;
    for (int i = 0; i < m; i++) {
        char a = path[n - m + i], b = ext[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    return 1;
}

// Open a PNG/BMP through the kernel decoder. A sheet (width a whole multiple
// of height, at most 32 frames) is split; anything else is one frame fitted
// to 128 high (the decoder only ever downsamples into its target box).
static int load_file(const char *path) {
    unsigned char *data = 0;
    long n = imgenc_read_file(path, &data, FILE_CAP);
    if (n <= 0) { set_status(n == -2 ? "File too large (8 MB cap)" : "Could not read file"); return -1; }
    long cap = (long)MAX_W * MAX_FRAMES * MAX_H * 4;
    uint32_t *out = (uint32_t *)malloc((size_t)cap);
    if (!out) { free(data); set_status("Out of memory"); return -1; }
    int dims[2] = { 0, 0 };
    int r = decode_image(data, (unsigned int)n, MAX_W * MAX_FRAMES, MAX_H, out, (unsigned int)cap, dims);
    free(data);
    if (r <= 0 || dims[0] <= 0 || dims[1] <= 0) { free(out); set_status("Decoder rejected the file"); return -1; }
    int dw = dims[0], dh = dims[1];
    int fw = dw, nf = 1;
    if (dh <= MAX_H && dw > dh && dw % dh == 0 && dw / dh <= MAX_FRAMES) { fw = dh; nf = dw / dh; }
    if (fw > MAX_W) fw = MAX_W;
    if (dh > MAX_H) dh = MAX_H;

    // Alpha: the decoder reports A in the top byte for RGBA sources; an
    // opaque source may come back with A = 0 everywhere, which must read as
    // opaque, not as an empty sprite.
    int any_alpha = 0;
    for (long i = 0; i < (long)dw * dims[1] && !any_alpha; i++) if (out[i] & 0xFF000000u) any_alpha = 1;
    int key = ext_is(path, ".BMP");

    new_document(fw, dh);
    g_nframes = nf;
    for (int f = 0; f < nf; f++) {
        for (int y = 0; y < dh; y++) {
            for (int x = 0; x < fw; x++) {
                uint32_t c = out[(long)y * dw + f * fw + x];
                if (!any_alpha) c |= 0xFF000000u;
                if (key && (c & 0xFFFFFF) == 0xFF00FF) c = 0;
                if ((c & 0xFF000000u) == 0) c = 0;   // fully clear: normalise
                g_frames[f][y * fw + x] = c;
            }
        }
    }
    free(out);
    strlcpy(g_doc_path, path, sizeof(g_doc_path));
    char msg[300];
    snprintf(msg, sizeof(msg), "Opened %s: %d frame%s %dx%d", path, nf, nf == 1 ? "" : "s", fw, dh);
    set_status(msg);
    return 0;
}

static int pick_save_name(const char *ext, const char *suffix, char *out, unsigned long cap) {
    char stamp[TZ_STAMP_LEN];
    tz_local_stamp(stamp, sizeof(stamp));
    for (int s = 0; s <= 99; s++) {
        char name[64];
        if (s == 0) snprintf(name, sizeof(name), "SPRITE-%s%s%s", stamp, suffix, ext);
        else        snprintf(name, sizeof(name), "SPRITE-%s%s-%d%s", stamp, suffix, s + 1, ext);
        if (userhome_path(SAVE_SUB, name, out, cap) != 0) return -1;
        int fd = sys_open(out, 0);
        if (fd >= 0) { sys_close(fd); continue; }
        return 0;
    }
    return -1;
}

static void ensure_save_dir(void) {
    char dir[256];
    if (userhome_path(0, SAVE_SUB, dir, sizeof(dir)) == 0) sys_mkdir(dir, 0755);
}

// Save the whole animation as one RGBA PNG sheet. Overwrites the opened
// document if it was a PNG; otherwise picks a fresh stamped name.
static void act_save(void) {
    int sw = g_cw * g_nframes;
    uint32_t *sheet = (uint32_t *)malloc((size_t)sw * g_ch * 4);
    if (!sheet) { set_status("Out of memory"); return; }
    for (int f = 0; f < g_nframes; f++)
        for (int y = 0; y < g_ch; y++)
            memcpy(sheet + (long)y * sw + f * g_cw, g_frames[f] + y * g_cw, (size_t)g_cw * 4);
    char path[256];
    if (g_doc_path[0] && ext_is(g_doc_path, ".PNG")) strlcpy(path, g_doc_path, sizeof(path));
    else { ensure_save_dir(); if (pick_save_name(".PNG", "", path, sizeof(path)) != 0) { free(sheet); set_status("Cannot pick a save name"); return; } }
    int r = imgenc_write_png(path, sheet, sw, g_ch, sw, 1);
    free(sheet);
    if (r == 0) {
        strlcpy(g_doc_path, path, sizeof(g_doc_path));
        g_dirty = 0;
        char msg[300];
        snprintf(msg, sizeof(msg), "Saved sheet %s (%d frame%s)", path, g_nframes, g_nframes == 1 ? "" : "s");
        set_status(msg);
    } else set_status("Save failed");
}

// Export the current frame as a 24-bit BMP with magenta as the key colour.
static void act_export_bmp(void) {
    static uint32_t flat[MAX_W * MAX_H];
    const uint32_t *f = cur();
    for (int i = 0; i < g_cw * g_ch; i++) flat[i] = (f[i] & 0xFF000000u) ? f[i] : KEY_MAGENTA;
    char suffix[16], path[256];
    snprintf(suffix, sizeof(suffix), "-F%d", g_cur + 1);
    ensure_save_dir();
    if (pick_save_name(".BMP", suffix, path, sizeof(path)) != 0) { set_status("Cannot pick a save name"); return; }
    if (imgenc_write_bmp24(path, flat, g_cw, g_ch, g_cw) == 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Exported frame %d to %s (magenta key)", g_cur + 1, path);
        set_status(msg);
    } else set_status("Export failed");
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
static rect_t canvas_area(void) {
    rect_t r;
    r.x = 0; r.y = TOOLBAR_H;
    r.w = g_win_w - PANEL_W;
    r.h = g_win_h - TOOLBAR_H - STRIP_H - STATUS_H;
    if (r.w < 16) r.w = 16;
    if (r.h < 16) r.h = 16;
    return r;
}
static int strip_y(void) { return g_win_h - STATUS_H - STRIP_H; }
static int panel_x(void) { return g_win_w - PANEL_W; }

static void compute_view(void) {
    rect_t a = canvas_area();
    if (g_zoom_manual > 0) g_zoom = g_zoom_manual;
    else {
        int zw = (a.w - 24) / g_cw, zh = (a.h - 24) / g_ch;
        g_zoom = imax(1, imin(imin(zw, zh), 40));
    }
    g_cx0 = a.x + (a.w - g_cw * g_zoom) / 2;
    g_cy0 = a.y + (a.h - g_ch * g_zoom) / 2;
}

static int win_to_cell(int wx, int wy, int *cx, int *cy) {
    int x = wx - g_cx0, y = wy - g_cy0;
    if (x < 0 || y < 0) return 0;
    x /= g_zoom; y /= g_zoom;
    if (x >= g_cw || y >= g_ch) return 0;
    *cx = x; *cy = y;
    return 1;
}

// ---------------------------------------------------------------------------
// Offscreen compose
// ---------------------------------------------------------------------------
static int g_W, g_H;
static void fb_fill(int x, int y, int w, int h, uint32_t c) {
    int x0 = imax(0, x), y0 = imax(0, y), x1 = imin(g_W, x + w), y1 = imin(g_H, y + h);
    for (int r = y0; r < y1; r++) {
        uint32_t *d = g_fb + (long)r * g_W;
        for (int col = x0; col < x1; col++) d[col] = c;
    }
}
static uint32_t over(uint32_t dst, uint32_t src) {   // src (with alpha) over dst
    unsigned a = src >> 24;
    if (a == 255) return src | 0xFF000000u;
    if (a == 0) return dst;
    return 0xFF000000u | gui_mix(dst & 0xFFFFFF, src & 0xFFFFFF, (int)a);
}
static uint32_t checker(int x, int y, int cell) {
    return (((x / cell) + (y / cell)) & 1) ? 0xFF9A9A9Au : 0xFFC8C8C8u;
}

// Draw one frame scaled `z` at (ox,oy) into the fb, over a checkerboard,
// optionally with the previous frame ghosted underneath.
static void fb_draw_frame(const uint32_t *f, const uint32_t *ghost, int ox, int oy, int z, int checkcell) {
    for (int cy = 0; cy < g_ch; cy++) {
        for (int cx = 0; cx < g_cw; cx++) {
            uint32_t s = f[cy * g_cw + cx];
            uint32_t g = ghost ? ghost[cy * g_cw + cx] : 0;
            int px0 = ox + cx * z, py0 = oy + cy * z;
            for (int yy = 0; yy < z; yy++) {
                int y = py0 + yy;
                if (y < 0 || y >= g_H) continue;
                uint32_t *d = g_fb + (long)y * g_W;
                for (int xx = 0; xx < z; xx++) {
                    int x = px0 + xx;
                    if (x < 0 || x >= g_W) continue;
                    uint32_t c = checker(xx + cx * z, yy + cy * z, checkcell);
                    if (g & 0xFF000000u) c = over(c, (g & 0xFFFFFF) | 0x60000000u);
                    c = over(c, s);
                    d[x] = c;
                }
            }
        }
    }
}

static void fb_hline(int x, int y, int w, uint32_t c) { fb_fill(x, y, w, 1, c); }
static void fb_vline(int x, int y, int h, uint32_t c) { fb_fill(x, y, 1, h, c); }
static void fb_box(int x, int y, int w, int h, uint32_t c) {
    fb_hline(x, y, w, c); fb_hline(x, y + h - 1, w, c); fb_vline(x, y, h, c); fb_vline(x + w - 1, y, h, c);
}

static void compose(void) {
    if (win < 0) return;
    gui_palette_t *pal = gui_pal();
    g_W = imin(g_win_w, FB_MAX_W);
    g_H = imin(g_win_h, FB_MAX_H);
    uint32_t surf = pal->surface | 0xFF000000u;
    uint32_t matte = gui_darken(pal->surface, 40) | 0xFF000000u;
    uint32_t raised = pal->surface_raised | 0xFF000000u;
    uint32_t border = pal->border | 0xFF000000u;
    uint32_t accent = pal->accent | 0xFF000000u;

    fb_fill(0, 0, g_W, g_H, surf);
    rect_t a = canvas_area();
    fb_fill(a.x, a.y, a.w, a.h, matte);

    // Canvas
    int checkcell = g_zoom >= 4 ? g_zoom : 8;
    const uint32_t *ghost = (g_onion && g_cur > 0 && !g_playing) ? g_frames[g_cur - 1] : 0;
    const uint32_t *shown = g_playing ? g_frames[g_play_frame] : cur();
    fb_draw_frame(shown, ghost, g_cx0, g_cy0, g_zoom, checkcell);
    if (g_grid && g_zoom >= 6) {
        uint32_t gl = 0xFF000000u | gui_mix(0x808080, pal->surface & 0xFFFFFF, 90);
        for (int x = 0; x <= g_cw; x++) fb_vline(g_cx0 + x * g_zoom, g_cy0, g_ch * g_zoom + 1, gl);
        for (int y = 0; y <= g_ch; y++) fb_hline(g_cx0, g_cy0 + y * g_zoom, g_cw * g_zoom + 1, gl);
    }
    fb_box(g_cx0 - 1, g_cy0 - 1, g_cw * g_zoom + 2, g_ch * g_zoom + 2, border);
    if (g_mirror) {
        int mx = g_cx0 + (g_cw * g_zoom) / 2;
        for (int y = g_cy0; y < g_cy0 + g_ch * g_zoom; y += 4) fb_vline(mx, y, 2, accent);
    }
    // Rubber band for shape tools
    if (g_drag && (g_tool == TOOL_LINE || g_tool == TOOL_RECT || g_tool == TOOL_BOX)) {
        int ax = imin(g_dx0, g_dx1), bx = imax(g_dx0, g_dx1), ay = imin(g_dy0, g_dy1), by = imax(g_dy0, g_dy1);
        if (g_tool == TOOL_LINE) {
            // preview line as cell blocks
            int x0 = g_dx0, y0 = g_dy0, x1 = g_dx1, y1 = g_dy1;
            int dx = iabs(x1 - x0), dy = -iabs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
            for (;;) {
                fb_box(g_cx0 + x0 * g_zoom, g_cy0 + y0 * g_zoom, g_zoom, g_zoom, accent);
                if (x0 == x1 && y0 == y1) break;
                int e2 = 2 * err;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
            }
        } else {
            fb_box(g_cx0 + ax * g_zoom, g_cy0 + ay * g_zoom, (bx - ax + 1) * g_zoom, (by - ay + 1) * g_zoom, accent);
        }
    }
    // Hover cell
    if (g_hover_cell_x >= 0 && !g_drag && !g_playing) {
        fb_box(g_cx0 + g_hover_cell_x * g_zoom, g_cy0 + g_hover_cell_y * g_zoom, g_zoom, g_zoom, accent);
    }

    // Frame strip background + thumbnails
    int sy = strip_y();
    fb_fill(0, sy, g_W, STRIP_H, raised);
    fb_hline(0, sy, g_W, border);
    int strip_w = panel_x() - 8 - 3 * 52;   // room for + Dup Del buttons
    int visible = imax(1, (strip_w - 8) / THUMB_STEP);
    if (g_cur < g_strip_first) g_strip_first = g_cur;
    if (g_cur >= g_strip_first + visible) g_strip_first = g_cur - visible + 1;
    if (g_strip_first < 0) g_strip_first = 0;
    int tz = imax(1, THUMB / imax(g_cw, g_ch));
    int tw = g_cw * tz, th = g_ch * tz;
    for (int i = 0; i < visible && g_strip_first + i < g_nframes; i++) {
        int fi = g_strip_first + i;
        int bx = 8 + i * THUMB_STEP, by = sy + 8;
        fb_fill(bx, by, THUMB + 4, THUMB + 4, (fi == g_cur) ? accent : border);
        fb_fill(bx + 2, by + 2, THUMB, THUMB, matte);
        int ox = bx + 2 + (THUMB - tw) / 2, oy = by + 2 + (THUMB - th) / 2;
        // small checker + frame, sampled at thumbnail scale
        for (int y = 0; y < th; y++) {
            uint32_t *d = g_fb + (long)(oy + y) * g_W;
            if (oy + y < 0 || oy + y >= g_H) continue;
            for (int x = 0; x < tw; x++) {
                int X = ox + x;
                if (X < 0 || X >= g_W) continue;
                uint32_t c = checker(x, y, 4);
                d[X] = over(c, g_frames[fi][(y / tz) * g_cw + x / tz]);
            }
        }
    }

    // Playback preview box in the panel (4x, clipped to 128 px)
    int pz = imax(1, imin(4, 128 / imax(g_cw, g_ch)));
    int pw = g_cw * pz, ph = g_ch * pz;
    int px = panel_x() + 12 + (PANEL_W - 24 - pw) / 2;
    int py = g_win_h - STATUS_H - 8 - 132 + (132 - ph) / 2;
    fb_fill(px - 2, py - 2, pw + 4, ph + 4, border);
    const uint32_t *pf = g_playing ? g_frames[g_play_frame] : cur();
    for (int y = 0; y < ph; y++) {
        if (py + y < 0 || py + y >= g_H) continue;
        uint32_t *d = g_fb + (long)(py + y) * g_W;
        for (int x = 0; x < pw; x++) {
            int X = px + x;
            if (X < 0 || X >= g_W) continue;
            d[X] = over(checker(x, y, 4), pf[(y / pz) * g_cw + x / pz]);
        }
    }

    syscall5(SYS_WIN_BLIT, win, 0, 0, (g_W & 0xFFFF) | ((g_H & 0xFFFF) << 16), (long)g_fb);
}

// ---------------------------------------------------------------------------
// Chrome: toolbar, panel, strip buttons, status
// ---------------------------------------------------------------------------
enum { BTN_PEN = 0, BTN_ERASE, BTN_FILL, BTN_LINE, BTN_RECT, BTN_BOX, BTN_PICK,
       BTN_MIRROR, BTN_GRID, BTN_ONION, BTN_UNDO, BTN_CLEAR,
       BTN_COUNT };

static rect_t tb_rect(int id) {
    rect_t r; r.y = TB_BTN_Y; r.h = TB_BTN_H; r.w = 50;
    static const int xs[BTN_COUNT] = { 0, 54, 108, 162, 216, 270, 324, 392, 446, 500, 568, 622 };
    r.x = TB_PAD + xs[id];
    if (id == BTN_MIRROR) r.w = 50;
    if (id == BTN_ONION) r.w = 56;
    return r;
}

static void draw_btn_r(rect_t r, const char *label, int active) {
    gui_state_t st = point_in(r, hover_x, hover_y) ? GUI_ST_HOVER : GUI_ST_NORMAL;
    gui_button(win, r.x, r.y, r.w, r.h, label, active ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY, st);
}

static void draw_toolbar(void) {
    gui_palette_t *pal = gui_pal();
    win_draw_rect(win, 0, 0, g_win_w, TOOLBAR_H, pal->surface_raised);
    win_draw_rect(win, 0, TOOLBAR_H - 1, g_win_w, 1, pal->border);
    draw_btn_r(tb_rect(BTN_PEN),   "Pen",   g_tool == TOOL_PEN);
    draw_btn_r(tb_rect(BTN_ERASE), "Erase", g_tool == TOOL_ERASE);
    draw_btn_r(tb_rect(BTN_FILL),  "Fill",  g_tool == TOOL_FILL);
    draw_btn_r(tb_rect(BTN_LINE),  "Line",  g_tool == TOOL_LINE);
    draw_btn_r(tb_rect(BTN_RECT),  "Rect",  g_tool == TOOL_RECT);
    draw_btn_r(tb_rect(BTN_BOX),   "Box",   g_tool == TOOL_BOX);
    draw_btn_r(tb_rect(BTN_PICK),  "Pick",  g_tool == TOOL_PICK);
    draw_btn_r(tb_rect(BTN_MIRROR), "Mirror", g_mirror);
    draw_btn_r(tb_rect(BTN_GRID),  "Grid",  g_grid);
    draw_btn_r(tb_rect(BTN_ONION), "Onion", g_onion);
    draw_btn_r(tb_rect(BTN_UNDO),  "Undo",  0);
    draw_btn_r(tb_rect(BTN_CLEAR), "Clear", 0);
    char z[32];
    snprintf(z, sizeof(z), "%dx%d  %dx", g_cw, g_ch, g_zoom);
    int zw = gui_ttf_width(z, GUI_TTF_SIZE);
    win_draw_text_ttf(win, g_win_w - TB_PAD - zw, (TOOLBAR_H - GUI_TTF_SIZE) / 2, z, GUI_TTF_SIZE, pal->ink_dim);
}

// Panel widgets, laid out top to bottom. Each has an id and a rect so the
// click handler and the painter walk the same table.
enum { PW_HEX = 0, PW_PALNAME, PW_SW0, PW_SW15 = PW_SW0 + 15,
       PW_S8, PW_S16, PW_S32, PW_S64, PW_WM, PW_WP, PW_HM, PW_HP,
       PW_SAVE, PW_EXPORT, PW_NEW,
       PW_PLAY, PW_FPS,
       PW_COUNT };

static rect_t pw_rect(int id) {
    rect_t r; r.x = 0; r.y = 0; r.w = 0; r.h = 0;
    int x0 = panel_x() + 12, y0 = TOOLBAR_H + 12;
    int bw = (PANEL_W - 24 - 8) / 2;
    if (id == PW_HEX)     { r.x = x0 + 52; r.y = y0 + 4; r.w = PANEL_W - 24 - 52; r.h = 28; }
    else if (id == PW_PALNAME) { r.x = x0; r.y = y0 + 44; r.w = PANEL_W - 24; r.h = 26; }
    else if (id >= PW_SW0 && id <= PW_SW15) {
        int i = id - PW_SW0;
        r.x = x0 + (i % 8) * 26; r.y = y0 + 76 + (i / 8) * 26; r.w = 24; r.h = 24;
    }
    else if (id >= PW_S8 && id <= PW_S64) { int i = id - PW_S8; r.x = x0 + i * 52; r.y = y0 + 154; r.w = 48; r.h = 26; }
    else if (id >= PW_WM && id <= PW_HP)  { int i = id - PW_WM; r.x = x0 + i * 52; r.y = y0 + 184; r.w = 48; r.h = 26; }
    else if (id == PW_SAVE)   { r.x = x0; r.y = y0 + 238; r.w = bw; r.h = 28; }
    else if (id == PW_EXPORT) { r.x = x0 + bw + 8; r.y = y0 + 238; r.w = bw; r.h = 28; }
    else if (id == PW_NEW)    { r.x = x0; r.y = y0 + 270; r.w = PANEL_W - 24; r.h = 26; }
    else if (id == PW_PLAY)   { r.x = x0; r.y = y0 + 324; r.w = bw; r.h = 28; }
    else if (id == PW_FPS)    { r.x = x0 + bw + 8; r.y = y0 + 324; r.w = bw; r.h = 28; }
    return r;
}

static void label(int x, int y, const char *s, uint32_t c) { win_draw_text_ttf(win, x, y, s, GUI_TTF_SIZE, c); }

static void draw_panel(void) {
    gui_palette_t *pal = gui_pal();
    int x0 = panel_x();
    win_draw_rect(win, x0, TOOLBAR_H, PANEL_W, g_win_h - TOOLBAR_H - STATUS_H, pal->surface);
    win_draw_rect(win, x0, TOOLBAR_H, 1, g_win_h - TOOLBAR_H - STATUS_H, pal->border);
    int y0 = TOOLBAR_H + 12, px = x0 + 12;

    // Colour
    win_draw_rect(win, px, y0, 40, 36, g_color);
    gui_draw_rect_outline(win, px, y0, 40, 36, pal->border);
    rect_t h = pw_rect(PW_HEX);
    gui_textfield2(win, h.x, h.y, h.w, h.h, g_hex_buf, g_hex_focus ? true : false);
    rect_t pn = pw_rect(PW_PALNAME);
    char pname[40];
    snprintf(pname, sizeof(pname), "%s  >", g_palettes[g_pal].name);
    draw_btn_r(pn, pname, 0);
    for (int i = 0; i < 16; i++) {
        rect_t s = pw_rect(PW_SW0 + i);
        uint32_t c = 0xFF000000u | g_palettes[g_pal].c[i];
        win_draw_rect(win, s.x, s.y, s.w, s.h, c);
        gui_draw_rect_outline(win, s.x, s.y, s.w, s.h, pal->border);
        if (c == g_color) gui_draw_rect_outline(win, s.x - 2, s.y - 2, s.w + 4, s.h + 4, pal->accent);
    }

    // Canvas size
    label(px, y0 + 134, "Canvas", pal->ink_dim);
    draw_btn_r(pw_rect(PW_S8),  "8",  g_cw == 8 && g_ch == 8);
    draw_btn_r(pw_rect(PW_S16), "16", g_cw == 16 && g_ch == 16);
    draw_btn_r(pw_rect(PW_S32), "32", g_cw == 32 && g_ch == 32);
    draw_btn_r(pw_rect(PW_S64), "64", g_cw == 64 && g_ch == 64);
    draw_btn_r(pw_rect(PW_WM), "W-", 0);
    draw_btn_r(pw_rect(PW_WP), "W+", 0);
    draw_btn_r(pw_rect(PW_HM), "H-", 0);
    draw_btn_r(pw_rect(PW_HP), "H+", 0);

    // File
    label(px, y0 + 218, g_doc_path[0] ? "File (sheet PNG)" : "File", pal->ink_dim);
    draw_btn_r(pw_rect(PW_SAVE), g_dirty ? "Save*" : "Save", 0);
    draw_btn_r(pw_rect(PW_EXPORT), "Export", 0);
    draw_btn_r(pw_rect(PW_NEW), "New sprite", 0);

    // Animation
    label(px, y0 + 304, "Animation", pal->ink_dim);
    draw_btn_r(pw_rect(PW_PLAY), g_playing ? "Stop" : "Play", g_playing);
    char fps[16];
    snprintf(fps, sizeof(fps), "%d fps", g_fps_tab[g_fps_idx]);
    draw_btn_r(pw_rect(PW_FPS), fps, 0);
    char fr[32];
    snprintf(fr, sizeof(fr), "Frame %d / %d", (g_playing ? g_play_frame : g_cur) + 1, g_nframes);
    label(px, y0 + 360, fr, pal->ink);
}

enum { SB_ADD = 0, SB_DUP, SB_DEL };
static rect_t sb_rect(int id) {
    rect_t r; r.w = 48; r.h = 26; r.y = strip_y() + (STRIP_H - 26) / 2;
    r.x = panel_x() - 8 - (3 - id) * 52;
    return r;
}
static rect_t thumb_rect(int i) {   // i = visible index
    rect_t r; r.x = 8 + i * THUMB_STEP; r.y = strip_y() + 8; r.w = THUMB + 4; r.h = THUMB + 4; return r;
}
static int strip_visible(void) {
    int strip_w = panel_x() - 8 - 3 * 52;
    return imax(1, (strip_w - 8) / THUMB_STEP);
}

static void draw_strip_chrome(void) {
    draw_btn_r(sb_rect(SB_ADD), "+", 0);
    draw_btn_r(sb_rect(SB_DUP), "Dup", 0);
    draw_btn_r(sb_rect(SB_DEL), "Del", 0);
}

static void draw_status(void) {
    gui_palette_t *pal = gui_pal();
    int sy = g_win_h - STATUS_H;
    win_draw_rect(win, 0, sy, g_win_w, STATUS_H, pal->surface_raised);
    win_draw_rect(win, 0, sy, g_win_w, 1, pal->border);
    win_draw_text_ttf(win, TB_PAD, sy + (STATUS_H - GUI_TTF_SIZE) / 2, g_status, GUI_TTF_SIZE, pal->ink);
    char pos[48];
    if (g_hover_cell_x >= 0) snprintf(pos, sizeof(pos), "(%d, %d)", g_hover_cell_x, g_hover_cell_y);
    else snprintf(pos, sizeof(pos), "%s", g_mirror ? "mirror X" : "");
    int pw = gui_ttf_width(pos, GUI_TTF_SIZE);
    win_draw_text_ttf(win, g_win_w - TB_PAD - pw, sy + (STATUS_H - GUI_TTF_SIZE) / 2, pos, GUI_TTF_SIZE, pal->ink_dim);
}

static void draw_all(void) {
    if (win < 0) return;
    compute_view();
    compose();
    draw_toolbar();
    draw_panel();
    draw_strip_chrome();
    draw_status();
    win_invalidate(win);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static void act_undo(void) { set_status(pop_undo() ? "Undone" : "Nothing to undo"); }
static void act_clear(void) { push_undo(); memset(cur(), 0, sizeof(g_frames[0])); set_status("Frame cleared"); }
static void toggle_play(void) {
    g_playing = !g_playing;
    if (g_playing) { g_play_frame = g_cur; g_play_next = uptime_ms() + (unsigned long)(1000 / g_fps_tab[g_fps_idx]); }
    set_status(g_playing ? "Playing" : "Stopped");
}

static int handle_toolbar_click(int x, int y) {
    for (int id = 0; id < BTN_COUNT; id++) {
        if (!point_in(tb_rect(id), x, y)) continue;
        switch (id) {
            case BTN_PEN: g_tool = TOOL_PEN; break;
            case BTN_ERASE: g_tool = TOOL_ERASE; break;
            case BTN_FILL: g_tool = TOOL_FILL; break;
            case BTN_LINE: g_tool = TOOL_LINE; break;
            case BTN_RECT: g_tool = TOOL_RECT; break;
            case BTN_BOX: g_tool = TOOL_BOX; break;
            case BTN_PICK: g_tool = TOOL_PICK; break;
            case BTN_MIRROR: g_mirror = !g_mirror; break;
            case BTN_GRID: g_grid = !g_grid; break;
            case BTN_ONION: g_onion = !g_onion; break;
            case BTN_UNDO: act_undo(); break;
            case BTN_CLEAR: act_clear(); break;
        }
        return 1;
    }
    return 0;
}

static int handle_panel_click(int x, int y) {
    for (int id = 0; id < PW_COUNT; id++) {
        if (!point_in(pw_rect(id), x, y)) continue;
        if (id == PW_HEX) { g_hex_focus = 1; tf_select_all(&g_hex_tf); return 1; }
        g_hex_focus = 0;
        if (id == PW_PALNAME) { g_pal = (g_pal + 1) % N_PALETTES; return 1; }
        if (id >= PW_SW0 && id <= PW_SW15) { g_color = 0xFF000000u | g_palettes[g_pal].c[id - PW_SW0]; hex_from_color(); return 1; }
        switch (id) {
            case PW_S8: resize_canvas(8, 8); break;
            case PW_S16: resize_canvas(16, 16); break;
            case PW_S32: resize_canvas(32, 32); break;
            case PW_S64: resize_canvas(64, 64); break;
            case PW_WM: resize_canvas(g_cw - 8 < 8 ? 8 : g_cw - 8, g_ch); break;
            case PW_WP: resize_canvas(g_cw + 8, g_ch); break;
            case PW_HM: resize_canvas(g_cw, g_ch - 8 < 8 ? 8 : g_ch - 8); break;
            case PW_HP: resize_canvas(g_cw, g_ch + 8); break;
            case PW_SAVE: act_save(); break;
            case PW_EXPORT: act_export_bmp(); break;
            case PW_NEW: new_document(16, 16); set_status("New 16x16 sprite"); break;
            case PW_PLAY: toggle_play(); break;
            case PW_FPS: g_fps_idx = (g_fps_idx + 1) % 4; break;
        }
        return 1;
    }
    return 0;
}

static int handle_strip_click(int x, int y) {
    if (point_in(sb_rect(SB_ADD), x, y)) { frame_insert(g_cur + 1, -1); set_status("Added frame"); return 1; }
    if (point_in(sb_rect(SB_DUP), x, y)) { frame_insert(g_cur + 1, g_cur); set_status("Duplicated frame"); return 1; }
    if (point_in(sb_rect(SB_DEL), x, y)) { frame_delete(g_cur); set_status("Deleted frame"); return 1; }
    int vis = strip_visible();
    for (int i = 0; i < vis; i++) {
        if (g_strip_first + i >= g_nframes) break;
        if (point_in(thumb_rect(i), x, y)) { g_cur = g_strip_first + i; g_playing = 0; return 1; }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Canvas interaction
// ---------------------------------------------------------------------------
static uint32_t stroke_color(void) {
    if (g_drag_btn == MOUSE_BUTTON_RIGHT || g_tool == TOOL_ERASE) return 0;
    return g_color;
}

static void stroke_begin(int wx, int wy, int btn) {
    int cx, cy;
    if (!win_to_cell(wx, wy, &cx, &cy)) return;
    if (g_playing) { g_playing = 0; }
    g_drag_btn = btn;
    if (g_tool == TOOL_PICK && btn == MOUSE_BUTTON_LEFT) {
        uint32_t c = cur()[cy * g_cw + cx];
        if (c & 0xFF000000u) { g_color = c | 0xFF000000u; hex_from_color(); set_status("Picked colour"); }
        else set_status("Transparent pixel");
        return;
    }
    push_undo();
    g_drag = 1;
    g_dx0 = g_dx1 = cx; g_dy0 = g_dy1 = cy;
    g_last_x = cx; g_last_y = cy;
    if (g_tool == TOOL_FILL && btn == MOUSE_BUTTON_LEFT) { cell_fill(cx, cy, g_color); g_drag = 0; return; }
    if (g_tool == TOOL_PEN || g_tool == TOOL_ERASE || g_tool == TOOL_PICK || btn == MOUSE_BUTTON_RIGHT || g_tool == TOOL_FILL) {
        plot(cx, cy, stroke_color());
    }
}

static void stroke_move(int wx, int wy) {
    int cx, cy;
    // Clamp to the canvas so a fast drag off the edge still lands its stroke.
    int x = (wx - g_cx0) / g_zoom, y = (wy - g_cy0) / g_zoom;
    if (wx < g_cx0) x = 0;
    if (wy < g_cy0) y = 0;
    cx = imax(0, imin(g_cw - 1, x)); cy = imax(0, imin(g_ch - 1, y));
    g_dx1 = cx; g_dy1 = cy;
    int freehand = (g_tool == TOOL_PEN || g_tool == TOOL_ERASE || g_tool == TOOL_PICK || g_tool == TOOL_FILL || g_drag_btn == MOUSE_BUTTON_RIGHT);
    if (freehand) {
        cell_line(g_last_x, g_last_y, cx, cy, stroke_color());
        g_last_x = cx; g_last_y = cy;
    }
}

static void stroke_end(void) {
    if (!g_drag) return;
    g_drag = 0;
    if (g_drag_btn == MOUSE_BUTTON_RIGHT) return;
    uint32_t c = stroke_color();
    switch (g_tool) {
        case TOOL_LINE: cell_line(g_dx0, g_dy0, g_dx1, g_dy1, c); break;
        case TOOL_RECT: cell_rect(g_dx0, g_dy0, g_dx1, g_dy1, c, 0); break;
        case TOOL_BOX:  cell_rect(g_dx0, g_dy0, g_dx1, g_dy1, c, 1); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    tf_init(&g_hex_tf, g_hex_buf, sizeof(g_hex_buf));
    win = win_create(WIN_TITLE, 40, 20, WIN_W, WIN_H);
    if (win < 0) { printf("sprite: failed to create window\n"); return 1; }
    apply_theme();
    hex_from_color();

    if (argc > 1 && argv[1][0] == '/') load_file(argv[1]);

    draw_all();

    gui_event_t ev;
    int running = 1;
    while (running) {
        int timeout = g_playing ? 15 : 250;
        int got = win_get_event(win, &ev, timeout);
        if (got == 0) {
            if (g_playing) {
                unsigned long now = uptime_ms();
                if (now >= g_play_next) {
                    g_play_frame = (g_play_frame + 1) % g_nframes;
                    g_play_next = now + (unsigned long)(1000 / g_fps_tab[g_fps_idx]);
                    draw_all();
                }
            }
            continue;
        }
        switch (ev.type) {
            case EVENT_REDRAW: draw_all(); break;
            case EVENT_RESIZE:
                if (ev.mouse_x > 0 && ev.mouse_y > 0) { g_win_w = ev.mouse_x; g_win_h = ev.mouse_y; draw_all(); }
                break;
            case EVENT_WINDOW_CLOSE: running = 0; break;

            case EVENT_MOUSE_DOWN: {
                int btn = (ev.mouse_buttons & MOUSE_BUTTON_RIGHT) ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT;
                if (!(ev.mouse_buttons & (MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT))) break;
                int x = ev.mouse_x, y = ev.mouse_y;
                if (y < TOOLBAR_H) { g_hex_focus = 0; if (btn == MOUSE_BUTTON_LEFT) handle_toolbar_click(x, y); }
                else if (x >= panel_x()) { if (btn == MOUSE_BUTTON_LEFT) handle_panel_click(x, y); }
                else if (y >= strip_y() && y < g_win_h - STATUS_H) { g_hex_focus = 0; if (btn == MOUSE_BUTTON_LEFT) handle_strip_click(x, y); }
                else if (y >= TOOLBAR_H && y < strip_y()) { g_hex_focus = 0; stroke_begin(x, y, btn); }
                draw_all();
                break;
            }
            case EVENT_MOUSE_MOVE: {
                hover_x = ev.mouse_x; hover_y = ev.mouse_y;
                int hcx, hcy;
                int over_canvas = win_to_cell(ev.mouse_x, ev.mouse_y, &hcx, &hcy);
                int changed = 0;
                if (g_drag) { stroke_move(ev.mouse_x, ev.mouse_y); changed = 1; }
                else {
                    if (!over_canvas) { hcx = -1; hcy = -1; }
                    if (hcx != g_hover_cell_x || hcy != g_hover_cell_y) { g_hover_cell_x = hcx; g_hover_cell_y = hcy; changed = 1; }
                    if (ev.mouse_y < TOOLBAR_H || ev.mouse_x >= panel_x() || ev.mouse_y >= strip_y()) changed = 1;
                }
                if (changed) draw_all();
                break;
            }
            case EVENT_MOUSE_UP:
                if (g_drag) { stroke_end(); draw_all(); }
                break;
            case EVENT_MOUSE_SCROLL: {
                int z = g_zoom + (ev.scroll_delta > 0 ? 1 : -1);
                g_zoom_manual = imax(1, imin(48, z));
                draw_all();
                break;
            }
            case EVENT_KEY_DOWN: {
                char c = ev.key_char;
                if (g_hex_focus) {
                    if (c == 27) { g_hex_focus = 0; hex_from_color(); }
                    else if (c == '\n' || c == '\r') { if (hex_apply() == 0) set_status("Colour set"); else set_status("Hex must be RRGGBB"); g_hex_focus = 0; }
                    else tf_handle_key(&g_hex_tf, &ev);
                    draw_all();
                    break;
                }
                if (c == 27) { running = 0; break; }
                if (c >= '1' && c <= '7') g_tool = c - '1';
                else if (c == 'm' || c == 'M') g_mirror = !g_mirror;
                else if (c == 'g' || c == 'G') g_grid = !g_grid;
                else if (c == 'o' || c == 'O') g_onion = !g_onion;
                else if (c == 'z' || c == 'Z' || c == 0x1A) act_undo();
                else if (c == 's' || c == 'S' || c == 0x13) act_save();
                else if (c == 'e' || c == 'E') act_export_bmp();
                else if (c == ' ') toggle_play();
                else if (c == '[' || ev.keycode == GUI_KEY_LEFT) { if (g_cur > 0) g_cur--; }
                else if (c == ']' || ev.keycode == GUI_KEY_RIGHT) { if (g_cur < g_nframes - 1) g_cur++; }
                else if (c == 'n' || c == 'N') { frame_insert(g_cur + 1, -1); set_status("Added frame"); }
                else if (c == 'd' || c == 'D') { frame_insert(g_cur + 1, g_cur); set_status("Duplicated frame"); }
                else if (c == '+' || c == '=') g_zoom_manual = imin(48, g_zoom + 1);
                else if (c == '-') g_zoom_manual = imax(1, g_zoom - 1);
                else if (c == '0') g_zoom_manual = 0;
                else if (c == '#') { g_hex_focus = 1; tf_select_all(&g_hex_tf); }
                draw_all();
                break;
            }
            default: break;
        }
    }
    win_destroy(win);
    return 0;
}
