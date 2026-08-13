// gallery - Maytera Gallery: contact-sheet image browser for MayteraOS.
//
// Fills a real gap in the media surface: the file browser lists image files
// by name only and the image viewer shows one BMP at a time; nothing gives a
// visual overview of a folder of pictures. Gallery scans a directory, decodes
// every BMP / PNG / JPEG through the kernel image decoder (SYS_DECODE_IMAGE,
// which point-samples straight to thumbnail size, so a 1280x800 wallpaper
// costs one small decode, not a full-size one), and lays the results out as
// a themed, scrollable contact sheet. Selecting a thumbnail opens a full
// preview with prev / next navigation and an optional timed slideshow.
//
// Thumbnails are decoded lazily, one per idle tick of the event loop, so the
// window is interactive immediately and there is no busy-wait anywhere: the
// cadence rides win_get_event's timeout exactly like every other app.
//
// UI follows the shared style engine (docs/UI_STYLE_GUIDE.md): themed
// palette pulled from the live system theme, raised toolbar / status cards,
// style-aware buttons and TTF text, resizable with reflow.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"
#include "../../libc/gui_theme.h"
#include "../../libc/gui_style.h"
#include "../../libc/dirent.h"

// ---------------------------------------------------------------------------
// Layout tokens
// ---------------------------------------------------------------------------
#define WIN_W        800
#define WIN_H        560
#define TOOLBAR_H    44
#define STATUSBAR_H  28

#define TB_PAD       8
#define TB_BTN_Y     7
#define TB_BTN_H     30
#define TB_GAP       6

#define GRID_PAD     10     // outer padding around the grid
#define CELL_W       152    // grid cell (thumb + frame)
#define CELL_H       132
#define TH_W         128    // thumbnail pixel box
#define TH_H         88
#define LABEL_H      18     // filename strip under the thumb

#define MAX_ENT      256
#define NAME_MAX_LEN 64
#define FBUF_CAP     (4 * 1024 * 1024)   // raw file read buffer
#define PREV_MAX_W   1280
#define PREV_MAX_H   800

#define SLIDESHOW_MS 3000

// Thumb states
enum { TH_PENDING = 0, TH_READY, TH_FAILED };

// UI modes
enum { MODE_GRID = 0, MODE_PREVIEW };

typedef struct {
    char name[NAME_MAX_LEN];
    int  is_dir;
    long size;          // bytes, -1 until the lazy loader fills it in
    int  tstate;        // TH_PENDING / TH_READY / TH_FAILED
    int  tw, th;        // actual thumbnail dims (aspect preserved)
} entry_t;

typedef struct { int x, y, w, h; } rect_t;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static int win = -1;
static int g_win_w = WIN_W, g_win_h = WIN_H;

static char    g_path[256] = "/";
static entry_t g_ent[MAX_ENT];
static int     g_count = 0;
static int     g_ndirs = 0;       // directories sort first
static int     g_nimages = 0;

// Thumbnail pixel cache (BGRA). ~11.5 MB of .bss, same order as the image
// viewer's static buffers; loaded lazily so startup stays instant.
static uint32_t g_thumb[MAX_ENT][TH_W * TH_H];

static uint8_t  g_fbuf[FBUF_CAP];                  // shared raw file buffer
static uint32_t g_prev[PREV_MAX_W * PREV_MAX_H];   // decoded preview
static int      g_prev_w = 0, g_prev_h = 0;
static int      g_prev_idx = -1;                    // entry shown in preview

static int g_mode     = MODE_GRID;
static int g_sel      = -1;       // selected entry
static int g_scroll   = 0;        // grid scroll offset in pixels
static int g_auto     = 0;        // slideshow running
static unsigned long g_auto_last = 0;

static int hover_x = -1, hover_y = -1;

// Double-click detection
static unsigned long g_last_click_ms = 0;
static int           g_last_click_idx = -1;

static char g_status[96] = "Scanning...";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static int point_in(rect_t r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ext_is(const char *path, const char *ext) {
    const char *dot = 0;
    for (const char *p = path; *p; p++) if (*p == '.') dot = p;
    if (!dot) return 0;
    dot++;
    while (*dot && *ext) {
        if (lower(*dot) != lower(*ext)) return 0;
        dot++; ext++;
    }
    return *dot == 0 && *ext == 0;
}

static int is_image_name(const char *n) {
    return ext_is(n, "bmp") || ext_is(n, "dib") || ext_is(n, "png") ||
           ext_is(n, "jpg") || ext_is(n, "jpeg");
}

// Case-insensitive name compare for the sort.
static int name_cmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = lower(*a), cb = lower(*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return lower(*a) - lower(*b);
}

// Join g_path + name into out (bounded).
static void path_join(char *out, int cap, const char *dir, const char *name) {
    int i = 0;
    while (dir[i] && i < cap - 2) { out[i] = dir[i]; i++; }
    if (i == 0 || out[i - 1] != '/') out[i++] = '/';
    int j = 0;
    while (name[j] && i < cap - 1) out[i++] = name[j++];
    out[i] = '\0';
}

static void set_status(const char *s) {
    int i = 0;
    while (s[i] && i < (int)sizeof(g_status) - 1) { g_status[i] = s[i]; i++; }
    g_status[i] = '\0';
}

// Format "123 KB" style size into buf.
static void fmt_size(long sz, char *buf, int cap) {
    char num[16];
    int m = 0;
    if (sz < 0) { buf[0] = '\0'; return; }
    if (sz >= 1024 * 1024) {
        gui_itoa(sz / (1024 * 1024), num, 8);
        for (int k = 0; num[k] && m < cap - 4; k++) buf[m++] = num[k];
        buf[m++] = ' '; buf[m++] = 'M'; buf[m++] = 'B';
    } else if (sz >= 1024) {
        gui_itoa(sz / 1024, num, 8);
        for (int k = 0; num[k] && m < cap - 4; k++) buf[m++] = num[k];
        buf[m++] = ' '; buf[m++] = 'K'; buf[m++] = 'B';
    } else {
        gui_itoa(sz, num, 8);
        for (int k = 0; num[k] && m < cap - 3; k++) buf[m++] = num[k];
        buf[m++] = ' '; buf[m++] = 'B';
    }
    buf[m] = '\0';
}

// ---------------------------------------------------------------------------
// Theme: identical approach to Image Viewer / Settings so Gallery follows the
// active system theme through the shared style engine.
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
// Directory scan
// ---------------------------------------------------------------------------
static void scan_dir(void) {
    g_count = 0;
    g_ndirs = 0;
    g_nimages = 0;
    g_sel = -1;
    g_scroll = 0;

    DIR *d = opendir(g_path);
    if (!d) {
        set_status("Cannot open directory");
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != 0 && g_count < MAX_ENT) {
        if (de->d_name[0] == '\0') continue;
        if (de->d_name[0] == '.' && (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0'))) continue;

        int dir = (de->d_type == DT_DIR);
        if (!dir && !is_image_name(de->d_name)) continue;

        entry_t *e = &g_ent[g_count];
        int i = 0;
        while (de->d_name[i] && i < NAME_MAX_LEN - 1) { e->name[i] = de->d_name[i]; i++; }
        e->name[i] = '\0';
        e->is_dir = dir;
        e->size   = -1;
        e->tstate = TH_PENDING;
        e->tw = e->th = 0;
        g_count++;
        if (dir) g_ndirs++; else g_nimages++;
    }
    closedir(d);

    // Insertion sort: directories first, then images, both A..Z.
    for (int i = 1; i < g_count; i++) {
        entry_t key = g_ent[i];
        int j = i - 1;
        while (j >= 0) {
            int before = 0;
            if (g_ent[j].is_dir != key.is_dir) before = key.is_dir;   // dirs first
            else before = (name_cmp(key.name, g_ent[j].name) < 0);
            if (!before) break;
            g_ent[j + 1] = g_ent[j];
            j--;
        }
        g_ent[j + 1] = key;
    }

    if (g_count > 0) g_sel = 0;

    char msg[96];
    char num[16];
    int m = 0;
    gui_itoa(g_nimages, num, 8);
    for (int k = 0; num[k]; k++) msg[m++] = num[k];
    const char *t1 = " images, ";
    for (int k = 0; t1[k]; k++) msg[m++] = t1[k];
    gui_itoa(g_ndirs, num, 8);
    for (int k = 0; num[k]; k++) msg[m++] = num[k];
    const char *t2 = " folders";
    for (int k = 0; t2[k]; k++) msg[m++] = t2[k];
    msg[m] = '\0';
    set_status(msg);
}

// ---------------------------------------------------------------------------
// File reading + decoding
// ---------------------------------------------------------------------------
// Read a whole file into g_fbuf. Returns byte count or -1.
static long read_file(const char *path) {
    int fd = sys_open(path, 0);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long n = sys_read(fd, g_fbuf + total, FBUF_CAP - total);
        if (n < 0) { sys_close(fd); return -1; }
        if (n == 0) break;
        total += n;
        if (total >= (long)FBUF_CAP) break;   // oversized: give up cleanly
    }
    sys_close(fd);
    if (total >= (long)FBUF_CAP) return -1;
    return total;
}

// Decode entry idx into its thumbnail slot. Returns 1 if state changed.
static int load_thumb(int idx) {
    entry_t *e = &g_ent[idx];
    if (e->is_dir || e->tstate != TH_PENDING) return 0;

    char full[256];
    path_join(full, sizeof(full), g_path, e->name);

    long n = read_file(full);
    if (n <= 0) { e->tstate = TH_FAILED; return 1; }
    e->size = n;

    int dims[2] = {0, 0};
    int r = decode_image(g_fbuf, (unsigned int)n, TH_W, TH_H,
                         g_thumb[idx], sizeof(g_thumb[0]), dims);
    if (r <= 0 || dims[0] <= 0 || dims[1] <= 0) {
        e->tstate = TH_FAILED;
        return 1;
    }
    e->tw = dims[0];
    e->th = dims[1];
    e->tstate = TH_READY;
    return 1;
}

// Load the full preview for entry idx, decoded to fit the current view box.
static int load_preview(int idx) {
    entry_t *e = &g_ent[idx];
    if (e->is_dir) return -1;

    char full[256];
    path_join(full, sizeof(full), g_path, e->name);
    long n = read_file(full);
    if (n <= 0) return -1;
    e->size = n;

    int vw = g_win_w;
    int vh = g_win_h - TOOLBAR_H - STATUSBAR_H;
    if (vw > PREV_MAX_W) vw = PREV_MAX_W;
    if (vh > PREV_MAX_H) vh = PREV_MAX_H;
    if (vw < 16) vw = 16;
    if (vh < 16) vh = 16;

    int dims[2] = {0, 0};
    int r = decode_image(g_fbuf, (unsigned int)n, vw, vh,
                         g_prev, sizeof(g_prev), dims);
    if (r <= 0 || dims[0] <= 0 || dims[1] <= 0) return -1;
    g_prev_w = dims[0];
    g_prev_h = dims[1];
    g_prev_idx = idx;
    return 0;
}

// ---------------------------------------------------------------------------
// Grid geometry
// ---------------------------------------------------------------------------
static int grid_cols(void) {
    int c = (g_win_w - GRID_PAD * 2) / CELL_W;
    return c < 1 ? 1 : c;
}

static int grid_rows(void) {
    int cols = grid_cols();
    return (g_count + cols - 1) / cols;
}

static int grid_content_h(void) { return grid_rows() * CELL_H + GRID_PAD * 2; }
static int grid_view_h(void)    { return g_win_h - TOOLBAR_H - STATUSBAR_H; }

static void clamp_scroll(void) {
    int maxs = grid_content_h() - grid_view_h();
    if (maxs < 0) maxs = 0;
    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > maxs) g_scroll = maxs;
}

// Cell rect for entry idx in window coordinates (already scrolled).
static rect_t cell_rect(int idx) {
    int cols = grid_cols();
    int row = idx / cols, col = idx % cols;
    // Center the used columns inside the window.
    int used = cols * CELL_W;
    int x0 = (g_win_w - used) / 2;
    if (x0 < GRID_PAD) x0 = GRID_PAD;
    rect_t r;
    r.x = x0 + col * CELL_W;
    r.y = TOOLBAR_H + GRID_PAD + row * CELL_H - g_scroll;
    r.w = CELL_W - 8;
    r.h = CELL_H - 8;
    return r;
}

static int cell_visible(int idx) {
    rect_t r = cell_rect(idx);
    return r.y + r.h > TOOLBAR_H && r.y < g_win_h - STATUSBAR_H;
}

// Make sure the selected cell is scrolled into view.
static void ensure_visible(int idx) {
    if (idx < 0) return;
    int cols = grid_cols();
    int row = idx / cols;
    int top = GRID_PAD + row * CELL_H;            // content coords
    int bot = top + CELL_H;
    if (top - g_scroll < 0) g_scroll = top;
    else if (bot - g_scroll > grid_view_h()) g_scroll = bot - grid_view_h();
    clamp_scroll();
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------
enum {
    BTN_UP = 0, BTN_REFRESH, BTN_VIEW,            // grid mode
    BTN_BACK, BTN_PREV, BTN_NEXT, BTN_AUTO,       // preview mode
    BTN_COUNT
};

static rect_t toolbar_btn_rect(int id) {
    rect_t r; r.y = TB_BTN_Y; r.h = TB_BTN_H; r.x = 0; r.w = 0;
    if (g_mode == MODE_GRID) {
        switch (id) {
            case BTN_UP:      r.x = TB_PAD;                    r.w = 44; break;
            case BTN_REFRESH: r.x = TB_PAD + 50;               r.w = 66; break;
            case BTN_VIEW:    r.x = TB_PAD + 122;              r.w = 54; break;
            default: break;
        }
    } else {
        switch (id) {
            case BTN_BACK: r.x = TB_PAD;        r.w = 56; break;
            case BTN_PREV: r.x = TB_PAD + 62;   r.w = 44; break;
            case BTN_NEXT: r.x = TB_PAD + 112;  r.w = 44; break;
            case BTN_AUTO: r.x = TB_PAD + 162;  r.w = 56; break;
            default: break;
        }
    }
    return r;
}

static void draw_btn(int id, const char *label, int active) {
    rect_t r = toolbar_btn_rect(id);
    if (r.w == 0) return;
    gui_state_t st = GUI_ST_NORMAL;
    if (point_in(r, hover_x, hover_y)) st = GUI_ST_HOVER;
    gui_button(win, r.x, r.y, r.w, r.h, label,
               active ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY, st);
}

static void draw_toolbar(void) {
    gui_palette_t *pal = gui_pal();
    win_draw_rect(win, 0, 0, g_win_w, TOOLBAR_H, pal->surface_raised);
    win_draw_rect(win, 0, TOOLBAR_H - 1, g_win_w, 1, pal->border);

    int label_x;
    if (g_mode == MODE_GRID) {
        draw_btn(BTN_UP, "Up", 0);
        draw_btn(BTN_REFRESH, "Rescan", 0);
        draw_btn(BTN_VIEW, "View", 0);
        label_x = TB_PAD + 182;
        // Current path, right of the buttons.
        char shown[64];
        int len = (int)strlen(g_path);
        int start = (len > 44) ? len - 44 : 0;
        int i = 0;
        while (g_path[start + i] && i < 62) { shown[i] = g_path[start + i]; i++; }
        shown[i] = '\0';
        win_draw_text_ttf(win, label_x, TB_BTN_Y + (TB_BTN_H - GUI_TTF_SIZE) / 2,
                          shown, GUI_TTF_SIZE, pal->ink);
    } else {
        draw_btn(BTN_BACK, "Back", 0);
        draw_btn(BTN_PREV, "<", 0);
        draw_btn(BTN_NEXT, ">", 0);
        draw_btn(BTN_AUTO, g_auto ? "Stop" : "Auto", g_auto);
        label_x = TB_PAD + 224;
        if (g_prev_idx >= 0) {
            win_draw_text_ttf(win, label_x, TB_BTN_Y + (TB_BTN_H - GUI_TTF_SIZE) / 2,
                              g_ent[g_prev_idx].name, GUI_TTF_SIZE, pal->ink);
        }
    }
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------
static void draw_statusbar(void) {
    gui_palette_t *pal = gui_pal();
    int sy = g_win_h - STATUSBAR_H;
    win_draw_rect(win, 0, sy, g_win_w, STATUSBAR_H, pal->surface_raised);
    win_draw_rect(win, 0, sy, g_win_w, 1, pal->border);
    int ty = sy + (STATUSBAR_H - GUI_TTF_SIZE) / 2;

    win_draw_text_ttf(win, TB_PAD, ty, g_status, GUI_TTF_SIZE, pal->ink_dim);

    // Right side: selected entry name + size.
    int idx = (g_mode == MODE_PREVIEW) ? g_prev_idx : g_sel;
    if (idx >= 0 && idx < g_count) {
        entry_t *e = &g_ent[idx];
        char meta[96];
        int m = 0;
        for (int k = 0; e->name[k] && m < 60; k++) meta[m++] = e->name[k];
        if (!e->is_dir && e->size >= 0) {
            meta[m++] = ' '; meta[m++] = '(';
            char szs[16];
            fmt_size(e->size, szs, sizeof(szs));
            for (int k = 0; szs[k] && m < 90; k++) meta[m++] = szs[k];
            meta[m++] = ')';
        }
        meta[m] = '\0';
        int mw = gui_ttf_width(meta, GUI_TTF_SIZE);
        win_draw_text_ttf(win, g_win_w - TB_PAD - mw, ty, meta, GUI_TTF_SIZE, pal->ink);
    }
}

// ---------------------------------------------------------------------------
// Grid drawing
// ---------------------------------------------------------------------------
static void draw_folder_icon(rect_t r) {
    gui_palette_t *pal = gui_pal();
    int cx = r.x + r.w / 2, cy = r.y + 8 + TH_H / 2;
    int fw = 56, fh = 40;
    uint32_t body = pal->accent;
    uint32_t tab  = gui_lighten(pal->accent, 40);
    win_draw_rect(win, cx - fw / 2, cy - fh / 2 - 6, fw / 3, 8, tab);
    win_draw_rect(win, cx - fw / 2, cy - fh / 2, fw, fh, body);
    win_draw_rect(win, cx - fw / 2, cy - fh / 2, fw, 2, gui_lighten(body, 60));
}

static void draw_cell(int idx) {
    if (!cell_visible(idx)) return;
    rect_t r = cell_rect(idx);
    gui_palette_t *pal = gui_pal();
    entry_t *e = &g_ent[idx];

    // Card + selection ring.
    gui_card(win, r.x, r.y, r.w, r.h);
    if (idx == g_sel) {
        // (#745) keyboard focus ring: pal->focus, not pal->accent. Byte-for-byte
        // the same two-pass construction Settings uses, so it had the same defect.
        gui_draw_rect_outline(win, r.x - 2, r.y - 2, r.w + 4, r.h + 4, pal->focus);
        gui_draw_rect_outline(win, r.x - 1, r.y - 1, r.w + 2, r.h + 2, pal->focus);
    }

    // Thumb area.
    int tx = r.x + (r.w - TH_W) / 2;
    int ty = r.y + 8;
    if (e->is_dir) {
        draw_folder_icon(r);
    } else if (e->tstate == TH_READY) {
        int ox = tx + (TH_W - e->tw) / 2;
        int oy = ty + (TH_H - e->th) / 2;
        win_draw_image(win, ox, oy, e->tw, e->th, g_thumb[idx]);
    } else {
        // Pending / failed placeholder.
        win_draw_rect(win, tx, ty, TH_W, TH_H, gui_darken(pal->surface_raised, 12));
        gui_text_ttf_centered(win, tx, ty, TH_W, TH_H,
                              e->tstate == TH_FAILED ? "no preview" : "...",
                              pal->ink_dim, GUI_TTF_SIZE);
    }

    // Filename strip (centered, clipped).
    char label[24];
    int i = 0;
    while (e->name[i] && i < 18) { label[i] = e->name[i]; i++; }
    if (e->name[i]) { label[i++] = '~'; }
    label[i] = '\0';
    gui_text_ttf_centered(win, r.x, r.y + 8 + TH_H, r.w, LABEL_H,
                          label, idx == g_sel ? pal->ink : pal->ink_dim,
                          GUI_TTF_SIZE - 1);
}

static void draw_scrollbar(void) {
    gui_palette_t *pal = gui_pal();
    int vh = grid_view_h();
    int ch = grid_content_h();
    if (ch <= vh) return;
    int track_x = g_win_w - 8;
    win_draw_rect(win, track_x, TOOLBAR_H, 6, vh, pal->track);
    int th = vh * vh / ch;
    if (th < 24) th = 24;
    int maxs = ch - vh;
    int ty = TOOLBAR_H + (vh - th) * g_scroll / (maxs > 0 ? maxs : 1);
    win_draw_rect(win, track_x, ty, 6, th, pal->accent);
}

static void draw_grid(void) {
    gui_palette_t *pal = gui_pal();
    win_draw_rect(win, 0, TOOLBAR_H, g_win_w, grid_view_h(), pal->surface);
    if (g_count == 0) {
        gui_text_ttf_centered(win, 0, TOOLBAR_H, g_win_w, grid_view_h(),
                              "No images in this folder",
                              pal->ink_dim, GUI_TTF_SIZE);
    } else {
        for (int i = 0; i < g_count; i++) draw_cell(i);
    }
    draw_scrollbar();
}

// ---------------------------------------------------------------------------
// Preview drawing
// ---------------------------------------------------------------------------
static void draw_preview(void) {
    gui_palette_t *pal = gui_pal();
    int vy = TOOLBAR_H;
    int vh = grid_view_h();
    // Dark matte behind the photo.
    win_draw_rect(win, 0, vy, g_win_w, vh, gui_darken(pal->surface, 40));
    if (g_prev_idx >= 0 && g_prev_w > 0) {
        int ox = (g_win_w - g_prev_w) / 2;
        int oy = vy + (vh - g_prev_h) / 2;
        if (oy < vy) oy = vy;
        win_draw_image(win, ox, oy, g_prev_w, g_prev_h, g_prev);
    } else {
        gui_text_ttf_centered(win, 0, vy, g_win_w, vh,
                              "Could not decode image", pal->ink_dim, GUI_TTF_SIZE);
    }
}

// ---------------------------------------------------------------------------
// Full redraw
// ---------------------------------------------------------------------------
static void draw_all(void) {
    if (g_mode == MODE_GRID) draw_grid();
    else draw_preview();
    draw_toolbar();
    draw_statusbar();
    win_invalidate(win);
}

// ---------------------------------------------------------------------------
// Navigation actions
// ---------------------------------------------------------------------------
static void go_up(void) {
    int len = (int)strlen(g_path);
    if (len <= 1) return;
    // Strip trailing slash, then cut at the last one.
    if (g_path[len - 1] == '/') g_path[--len] = '\0';
    while (len > 1 && g_path[len - 1] != '/') g_path[--len] = '\0';
    if (len > 1) g_path[len - 1] = '\0';
    if (g_path[0] == '\0') { g_path[0] = '/'; g_path[1] = '\0'; }
    scan_dir();
    draw_all();
}

static void enter_dir(int idx) {
    char next[256];
    path_join(next, sizeof(next), g_path, g_ent[idx].name);
    strlcpy(g_path, next, sizeof(g_path));
    scan_dir();
    draw_all();
}

// Next / previous image entry from idx (skips folders). dir = +1 / -1.
static int step_image(int idx, int dir) {
    for (int i = idx + dir; i >= 0 && i < g_count; i += dir) {
        if (!g_ent[i].is_dir) return i;
    }
    return -1;
}

static void open_preview(int idx) {
    if (idx < 0 || idx >= g_count) return;
    if (g_ent[idx].is_dir) { enter_dir(idx); return; }
    set_status("Loading...");
    draw_statusbar();
    win_invalidate(win);
    if (load_preview(idx) == 0) {
        g_mode = MODE_PREVIEW;
        g_sel = idx;
        char msg[96];
        char num[16];
        int m = 0;
        gui_itoa(g_prev_w, num, 8);
        for (int k = 0; num[k]; k++) msg[m++] = num[k];
        msg[m++] = ' '; msg[m++] = 'x'; msg[m++] = ' ';
        gui_itoa(g_prev_h, num, 8);
        for (int k = 0; num[k]; k++) msg[m++] = num[k];
        const char *t = " shown";
        for (int k = 0; t[k]; k++) msg[m++] = t[k];
        msg[m] = '\0';
        set_status(msg);
    } else {
        set_status("Decode failed");
    }
    draw_all();
}

static void close_preview(void) {
    g_mode = MODE_GRID;
    g_auto = 0;
    if (g_prev_idx >= 0) g_sel = g_prev_idx;
    ensure_visible(g_sel);
    char msg[64];
    char num[16];
    int m = 0;
    gui_itoa(g_nimages, num, 8);
    for (int k = 0; num[k]; k++) msg[m++] = num[k];
    const char *t = " images";
    for (int k = 0; t[k]; k++) msg[m++] = t[k];
    msg[m] = '\0';
    set_status(msg);
    draw_all();
}

static void preview_step(int dir) {
    int nxt = step_image(g_prev_idx, dir);
    if (nxt >= 0) open_preview(nxt);
}

// Hit test: which entry is under (x, y) in grid mode. Returns -1 for none.
static int hit_cell(int x, int y) {
    for (int i = 0; i < g_count; i++) {
        if (!cell_visible(i)) continue;
        rect_t r = cell_rect(i);
        if (point_in(r, x, y)) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Idle work: decode one pending thumbnail per idle tick, visible cells first.
// ---------------------------------------------------------------------------
static int idle_tick(void) {
    if (g_mode != MODE_GRID) return 0;
    int pick = -1;
    for (int i = 0; i < g_count; i++) {
        if (g_ent[i].is_dir || g_ent[i].tstate != TH_PENDING) continue;
        if (cell_visible(i)) { pick = i; break; }
        if (pick < 0) pick = i;
    }
    if (pick < 0) return 0;
    if (load_thumb(pick)) {
        if (cell_visible(pick)) {
            draw_cell(pick);
            win_invalidate(win);
        }
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Toolbar click handling
// ---------------------------------------------------------------------------
static int handle_toolbar_click(int x, int y) {
    if (g_mode == MODE_GRID) {
        if (point_in(toolbar_btn_rect(BTN_UP), x, y))      { go_up(); return 1; }
        if (point_in(toolbar_btn_rect(BTN_REFRESH), x, y)) { scan_dir(); draw_all(); return 1; }
        if (point_in(toolbar_btn_rect(BTN_VIEW), x, y))    { open_preview(g_sel); return 1; }
    } else {
        if (point_in(toolbar_btn_rect(BTN_BACK), x, y)) { close_preview(); return 1; }
        if (point_in(toolbar_btn_rect(BTN_PREV), x, y)) { preview_step(-1); return 1; }
        if (point_in(toolbar_btn_rect(BTN_NEXT), x, y)) { preview_step(1); return 1; }
        if (point_in(toolbar_btn_rect(BTN_AUTO), x, y)) {
            g_auto = !g_auto;
            g_auto_last = uptime_ms();
            draw_toolbar();
            win_invalidate(win);
            return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == '/') strlcpy(g_path, argv[1], sizeof(g_path));

    win = win_create("Maytera Gallery", 60, 30, WIN_W, WIN_H);
    if (win < 0) {
        printf("gallery: failed to create window\n");
        return 1;
    }

    apply_theme();
    scan_dir();
    draw_all();

    gui_event_t ev;
    int running = 1;
    while (running) {
        int got = win_get_event(win, &ev, 60);
        if (got == 0) {
            // Idle: decode thumbnails one at a time; run the slideshow timer.
            if (g_auto && g_mode == MODE_PREVIEW) {
                unsigned long now = uptime_ms();
                if (now - g_auto_last >= SLIDESHOW_MS) {
                    g_auto_last = now;
                    int nxt = step_image(g_prev_idx, 1);
                    if (nxt < 0) nxt = step_image(-1, 1);   // wrap to first
                    if (nxt >= 0 && nxt != g_prev_idx) open_preview(nxt);
                }
            }
            idle_tick();
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
                    clamp_scroll();
                    if (g_mode == MODE_PREVIEW && g_prev_idx >= 0)
                        load_preview(g_prev_idx);   // re-fit to the new view
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
                    break;
                }
                if (g_mode == MODE_GRID) {
                    int idx = hit_cell(ev.mouse_x, ev.mouse_y);
                    if (idx >= 0) {
                        unsigned long now = uptime_ms();
                        int dbl = (idx == g_last_click_idx &&
                                   now - g_last_click_ms < 450);
                        g_last_click_ms = now;
                        g_last_click_idx = idx;
                        if (dbl) {
                            open_preview(idx);
                        } else if (idx != g_sel) {
                            g_sel = idx;
                            draw_grid();
                            draw_statusbar();
                            win_invalidate(win);
                        }
                    }
                } else {
                    // Click in the preview area advances; simple and handy.
                    preview_step(1);
                }
                break;

            case EVENT_MOUSE_MOVE: {
                int was_over = (hover_y >= 0 && hover_y < TOOLBAR_H);
                int now_over = (ev.mouse_y >= 0 && ev.mouse_y < TOOLBAR_H);
                hover_x = ev.mouse_x;
                hover_y = ev.mouse_y;
                if (now_over || was_over) {
                    draw_toolbar();
                    win_invalidate(win);
                }
                break;
            }

            case EVENT_MOUSE_SCROLL:
                if (g_mode == MODE_GRID) {
                    g_scroll -= ev.scroll_delta * 48;
                    clamp_scroll();
                    draw_all();
                }
                break;

            case EVENT_KEY_DOWN: {
                unsigned int kc = ev.keycode;
                char c = ev.key_char;
                if (g_mode == MODE_GRID) {
                    int cols = grid_cols();
                    int ns = g_sel;
                    if (c == 27) { running = 0; }
                    else if (kc == 0x82 && ns > 0) ns--;                       // left
                    else if (kc == 0x83 && ns < g_count - 1) ns++;             // right
                    else if (kc == 0x80 && ns - cols >= 0) ns -= cols;         // up
                    else if (kc == 0x81 && ns + cols < g_count) ns += cols;    // down
                    else if (kc == 0x49) { g_scroll -= grid_view_h(); clamp_scroll(); draw_all(); }
                    else if (kc == 0x51) { g_scroll += grid_view_h(); clamp_scroll(); draw_all(); }
                    else if (c == '\n' || c == '\r' || kc == 0x1C) { open_preview(g_sel); }
                    else if (c == '\b' || kc == 0x0E) { go_up(); }
                    else if (c == 'r' || c == 'R') { scan_dir(); draw_all(); }
                    if (ns != g_sel && ns >= 0 && ns < g_count) {
                        g_sel = ns;
                        ensure_visible(ns);
                        draw_all();
                    }
                } else {
                    if (c == 27 || c == '\b' || kc == 0x0E) close_preview();
                    else if (kc == 0x82) preview_step(-1);
                    else if (kc == 0x83) preview_step(1);
                    else if (c == ' ') {
                        g_auto = !g_auto;
                        g_auto_last = uptime_ms();
                        draw_toolbar();
                        win_invalidate(win);
                    }
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
