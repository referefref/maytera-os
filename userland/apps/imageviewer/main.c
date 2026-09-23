// imageviewer - Image Viewer for MayteraOS (user-space version), glass edition
// Displays BMP images with zoom and pan support.
//
// (imgglass) The chrome is the shared dark-teal glass language
// (docs/UI_GLASS_DESIGN_SYSTEM.md sections 1, 10, 11), the fifth surface in
// that restyle after the App Repo, the browser chrome, the Task Manager
// (tmglass) and the Calculator (calcglass): a frosted-wallpaper backdrop in
// the margins, toolbar pills on the backdrop, and ONE rounded glass panel that
// holds the image well and a status row. Designed first as an HTML/CSS spec
// (the build host:/root/imgglass-proof/imgglass-spec.html), then ported here.
// Everything the viewer DOES (BMP decode, progressive load, zoom, fit, 1:1,
// drag-pan, scroll-zoom, keys, resize re-fit) is untouched: this is a visual
// pass only.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_style.h"    // gui_fill_rounded_aa / gui_rounded_border / gui_soft_shadow / gui_mix / gui_glass_backdrop_*

#define WIN_W 640
#define WIN_H 480
#define MIN_WIN_W 240
#define MIN_WIN_H 160

#define MAX_IMG_W 2048
#define MAX_IMG_H 2048

// ---------------------------------------------------------------------------
// (imgglass) Geometry. One definition each; draw and hit-test both call the
// rect functions below (glass doc section 8). PAD / TAB_H / TAB_GAP / PANEL_R
// / PANEL_IN and the panel top (PAD + TAB_H + 10) are the Task Manager's and
// the Calculator's, so the three windows share one geometry.
// ---------------------------------------------------------------------------
#define PAD        10                    // window margin: the backdrop shows here
#define TAB_H      26                    // toolbar pill height (radius TAB_H/2)
#define TAB_GAP    6                     // gap between pills
#define BTN_SM     36                    // "-" and "+" pills
#define BTN_MD     56                    // Fit / 1:1 pills and the zoom readout
#define PANEL_Y    (PAD + TAB_H + 10)    // panel top (46)
#define PANEL_R    12                    // panel corner radius
#define PANEL_IN   12                    // inset from the panel edge to the well
#define STATUS_H   18                    // status row inside the panel, under the well
#define STATUS_GAP 6                     // well bottom -> status row

// #301: all in-window text goes through the antialiased TrueType path.
#define TTF_TAB    12   // pill labels, zoom readout, status row
#define TTF_IND    11   // key hint
#define TTF_FUNC   14   // nominal size of the "-" / "+" pills (their faces are stroked, see draw_zoom_face)
#define TTF_EMPTY  14   // empty-state line in the well

// (imgglass) Glass tokens: docs/UI_GLASS_DESIGN_SYSTEM.md section 1, the
// exact names and values the Task Manager and Calculator use. Fixed dark
// glass regardless of the active theme (owner decision recorded at glasstm):
// the window carries its own backdrop, so there is no light-theme surface for
// these to sit on. Copy the NAMES as well as the values.
#define C_PANEL       0x00122420   // WEL_BG_MID: panel fill, pill fill, the outer colour every pill AA-blends toward
#define C_CARD        0x000E1D1B   // DK_CARD_FILL: the image well, the zoom readout pill
#define C_EDGE        0x002C4A44   // DK_STROKE_UNSEL: panel / well / pill 1px border
#define C_INK         0x00F3FBF9   // DK_HEADLINE: filename, zoom readout
#define C_INK_DIM     0x00A9D9CC   // DK_BODY: pill labels, meta, hint, empty state
#define C_ACCENT      0x006AE2CF   // DK_ACCENT: the selected pill (Fit / 1:1 when active)
#define C_ACCENT_INK  0x0004231A   // text on the accent
#define WEL_BG_TOP    0x000A1614   // backdrop gradient fallback, top stop
#define WEL_BG_BOTTOM 0x00050A09   // backdrop gradient fallback, bottom stop

// Image data
static uint32_t image[MAX_IMG_W * MAX_IMG_H];

// #246: offscreen buffer. draw_well() composes the well (card fill + the
// scaled image) here (pure memory, no syscalls) and publishes it with ONE
// win_draw_image, instead of ~w*h per-pixel win_draw_pixel syscalls.
#define IV_MAXW 1280
#define IV_MAXH 800
static uint32_t g_fb[IV_MAXW * IV_MAXH];
static int img_width = 0;
static int img_height = 0;
static char current_file[256] = {0};

// View state
static int win = -1;

// Live window content size. Starts at the create size and is re-synced from
// the compositor every frame (#436 idiom) and on EVENT_RESIZE, so the whole
// layout (pills, panel, well, status row) reflows.
static int g_win_w = WIN_W, g_win_h = WIN_H;
static int zoom_level = 100;  // percentage
static int pan_x = 0, pan_y = 0;
static int dragging = 0;
static int drag_start_x = 0, drag_start_y = 0;
static int drag_pan_x = 0, drag_pan_y = 0;

// Which toolbar pill the pointer is over (-1 = none); drives hover fills.
static int hover_btn = -1;

// BMP header structures
#pragma pack(push, 1)
typedef struct {
    uint16_t type;
    uint32_t size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offset;
} bmp_header_t;

typedef struct {
    uint32_t size;
    int32_t width;
    int32_t height;
    uint16_t planes;
    uint16_t bits_per_pixel;
    uint32_t compression;
    uint32_t image_size;
    int32_t x_ppm;
    int32_t y_ppm;
    uint32_t colors_used;
    uint32_t colors_important;
} bmp_info_t;
#pragma pack(pop)

// Toolbar button identifiers (used for both draw and hit-test).
enum {
    BTN_ZOOM_OUT = 0,
    BTN_ZOOM_IN,
    BTN_FIT,
    BTN_ACTUAL,
    BTN_COUNT
};

typedef struct { int x, y, w, h; } rect_t;

// Toolbar pills, left to right on the backdrop: "-" "+" [readout] "Fit" "1:1".
static rect_t toolbar_btn_rect(int id) {
    rect_t r;
    r.y = PAD;
    r.h = TAB_H;
    switch (id) {
        case BTN_ZOOM_OUT:
            r.x = PAD;                                  r.w = BTN_SM; break;
        case BTN_ZOOM_IN:
            r.x = PAD + BTN_SM + TAB_GAP;               r.w = BTN_SM; break;
        case BTN_FIT:
            // After the +/- pair and the readout pill.
            r.x = PAD + 2 * (BTN_SM + TAB_GAP) + BTN_MD + TAB_GAP;  r.w = BTN_MD; break;
        case BTN_ACTUAL:
            r.x = PAD + 2 * (BTN_SM + TAB_GAP) + 2 * (BTN_MD + TAB_GAP);
            r.w = BTN_MD; break;
        default:
            r.x = 0; r.w = 0; break;
    }
    return r;
}

// The zoom readout pill sits between "+" and "Fit". Not a hit target.
static rect_t readout_rect(void) {
    rect_t r;
    r.x = PAD + 2 * (BTN_SM + TAB_GAP);
    r.y = PAD; r.w = BTN_MD; r.h = TAB_H;
    return r;
}

static int point_in_rect(rect_t r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// The one glass panel: everything below the toolbar down to the bottom margin.
static rect_t panel_rect(void) {
    rect_t r;
    r.x = PAD; r.y = PANEL_Y;
    r.w = g_win_w - 2 * PAD;
    r.h = g_win_h - PAD - PANEL_Y;
    if (r.w < 2 * PANEL_IN + 1) r.w = 2 * PANEL_IN + 1;
    if (r.h < 2 * PANEL_IN + STATUS_H + STATUS_GAP + 1) r.h = 2 * PANEL_IN + STATUS_H + STATUS_GAP + 1;
    return r;
}

// The image well: the panel inset PANEL_IN, minus the status row under it.
static rect_t well_rect(void) {
    rect_t p = panel_rect();
    rect_t r;
    r.x = p.x + PANEL_IN;
    r.y = p.y + PANEL_IN;
    r.w = p.w - 2 * PANEL_IN;
    r.h = p.h - 2 * PANEL_IN - STATUS_H - STATUS_GAP;
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    return r;
}

// The status row inside the panel, under the well.
static rect_t status_rect(void) {
    rect_t p = panel_rect();
    rect_t r;
    r.x = p.x + PANEL_IN;
    r.y = p.y + p.h - PANEL_IN - STATUS_H;
    r.w = p.w - 2 * PANEL_IN;
    r.h = STATUS_H;
    return r;
}

// The view the zoom / pan / fit maths works in IS the well. pan_x/pan_y are
// offsets from the well's top-left, exactly as they were offsets from the old
// full-width view's top-left.
#define VIEW_X (well_rect().x)
#define VIEW_Y (well_rect().y)
#define VIEW_W (well_rect().w)
#define VIEW_H (well_rect().h)

// ---------------------------------------------------------------------------
// (glasslib) The frosted-wallpaper backdrop: now the shared libc recipe
// (userland/libc/gui_style.h gui_glass_backdrop_*), consolidated out of this
// file, the Task Manager, the Calculator and the Media Player, which all
// carried an identical copy (blame.md tmglass/calcglass/audglass/imgglass).
// This app owns only the two small persistent pieces the API asks for; the
// scratch (raw thumbnail bytes, blur temp plane) lives in gui.c.
// ---------------------------------------------------------------------------
static uint32_t g_bd[GUI_GLASS_BD_W * GUI_GLASS_BD_H];
static int g_bd_wi = GUI_GLASS_BD_NEVER;   // wallpaper index the backdrop was built for
static int g_chrome_dirty = 1;             // the ONLY thing that can make draw_all() blit the backdrop

// The chrome's self-committing call (see draw_all()): SYS_WIN_BLIT copies the
// backdrop into the window content scaled to the content rect (x/y are
// ignored by the kernel) and publishes that frame on its own.
static void bd_blit(void) {
    gui_glass_backdrop_blit(win, g_bd);
}

// The backdrop colour under content pixel (x, y): what the kernel's nearest-
// neighbour scale put there, to within the blur. Every AA edge and shadow
// drawn onto the backdrop takes its outer colour from here; a flat guess is
// what produces a square halo around a round corner (glass doc section 2).
static uint32_t bd_at(int x, int y) {
    return gui_glass_backdrop_at(g_bd, g_win_w, g_win_h, x, y);
}

// Rebuild the backdrop if the wallpaper changed. SYS_GET_WALLPAPER is the
// same poll the compositor makes; a changed index marks the chrome dirty.
static void sync_backdrop(void) {
    if (gui_glass_backdrop_sync(g_bd, &g_bd_wi, C_PANEL, 158, WEL_BG_TOP, WEL_BG_BOTTOM))
        g_chrome_dirty = 1;
}

// Load BMP file
static int load_bmp_step(const char *path, int step) {
    if (step < 1) step = 1;
    int fd = sys_open(path, 0);  // O_RDONLY = 0
    if (fd < 0) {
        printf("Failed to open file: %s\n", path);
        return -1;
    }

    // Read BMP header
    bmp_header_t hdr;
    if (sys_read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        sys_close(fd);
        return -1;
    }

    // Check BMP signature
    if (hdr.type != 0x4D42) {  // 'BM'
        printf("Not a BMP file\n");
        sys_close(fd);
        return -1;
    }

    // Read info header
    bmp_info_t info;
    if (sys_read(fd, &info, sizeof(info)) != sizeof(info)) {
        sys_close(fd);
        return -1;
    }

    // Check dimensions
    int w = info.width;
    int h = info.height < 0 ? -info.height : info.height;
    int bottom_up = info.height > 0;

    if (w > MAX_IMG_W || h > MAX_IMG_H || w <= 0 || h <= 0) {
        printf("Image too large or invalid: %dx%d\n", w, h);
        sys_close(fd);
        return -1;
    }

    // Support 24-bit and 32-bit BMP
    if (info.bits_per_pixel != 24 && info.bits_per_pixel != 32) {
        printf("Unsupported bit depth: %d\n", info.bits_per_pixel);
        sys_close(fd);
        return -1;
    }

    // Calculate row padding (BMP rows are 4-byte aligned)
    int bytes_per_pixel = info.bits_per_pixel / 8;
    int row_size = w * bytes_per_pixel;
    int padding = (4 - (row_size % 4)) % 4;

    // Read pixel data
    uint8_t row[MAX_IMG_W * 4 + 4];

    for (int y = 0; y < h; y++) {
        int dest_y = bottom_up ? (h - 1 - y) : y;

        if (sys_read(fd, row, row_size + padding) != row_size + padding) {
            sys_close(fd);
            return -1;
        }

        for (int x = 0; x < w; x += step) {
            uint8_t *p = &row[x * bytes_per_pixel];
            uint32_t px = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
            for (int k = 0; k < step && x + k < w; k++)
                image[dest_y * MAX_IMG_W + x + k] = px;
        }
    }

    sys_close(fd);

    img_width = w;
    img_height = h;

    // Copy filename
    int i = 0;
    while (path[i] && i < 255) {
        current_file[i] = path[i];
        i++;
    }
    current_file[i] = '\0';

    // Reset view
    zoom_level = 100;
    pan_x = 0;
    pan_y = 0;

    printf("Loaded image: %dx%d\n", w, h);
    return 0;
}

// The zoom "Fit" would pick for the current well (the fit_to_view() formula,
// without applying it). Drives the Fit pill's selected state.
static int fit_zoom(void) {
    if (img_width == 0 || img_height == 0) return -1;
    int zoom_w = (VIEW_W * 100) / img_width;
    int zoom_h = (VIEW_H * 100) / img_height;
    int z = zoom_w < zoom_h ? zoom_w : zoom_h;
    if (z < 10) z = 10;
    if (z > 500) z = 500;
    return z;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
// Centred TTF label. The rasterizer's y is the top of the LINE box, so the
// label is centred on the control by its nominal size (glass doc section 5).
static void text_center(int x, int y, int w, int h, const char *s, int size, uint32_t color) {
    int tw = gui_ttf_width(s, size);
    win_draw_text_ttf(win, x + (w - tw) / 2, y + (h - size) / 2 - 1, s, size, color);
}

// (imgglass) The glass panel: soft shadow, AA rounded fill, 1px border, 1px
// top highlight; the same layering the Task Manager's and the Calculator's
// draw_panel() use. Every outer colour is sampled from the backdrop under
// that edge, so the fringe and corners match what the blit put there. Drawn
// EVERY frame on purpose: the same inputs give the same pixels, so the
// repaint is idempotent (section 11: only the backdrop blit and the well's
// image draw are commits).
static void draw_panel(int x, int y, int w, int h) {
    uint32_t below = bd_at(x + w / 2, y + h + 3);
    // Mean of the four corner samples: gui_fill_rounded_aa takes ONE outer
    // colour, and the blur keeps the four within a few levels of each other.
    uint32_t c0 = bd_at(x + 4, y + 4),     c1 = bd_at(x + w - 4, y + 4),
             c2 = bd_at(x + 4, y + h - 4), c3 = bd_at(x + w - 4, y + h - 4);
    uint32_t outer = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t m = (((c0 >> sh) & 0xFF) + ((c1 >> sh) & 0xFF) + ((c2 >> sh) & 0xFF) + ((c3 >> sh) & 0xFF)) / 4;
        outer |= m << sh;
    }
    gui_soft_shadow(win, x, y + 2, w, h, PANEL_R, below);
    gui_fill_rounded_aa(win, x, y, w, h, PANEL_R, C_PANEL, outer);
    gui_rounded_border(win, x, y, w, h, PANEL_R, C_EDGE);
    win_draw_rect(win, x + PANEL_R, y + 1, w - 2 * PANEL_R, 1, gui_lighten(C_PANEL, 16));
}

// (imgglass) One toolbar pill on the frosted backdrop: the Task Manager /
// Calculator tab pill. Selected = accent fill with the accent ink; readout =
// card fill (a display, not a key); otherwise panel-coloured glass with the
// panel border, hover lightens. The AA edge takes the backdrop colour at the
// pill's own centre, and the rectangle is the one the click handler tests.
static void draw_pill(rect_t r, const char *label, int size, int sel, int hov, int readout) {
    uint32_t outer = bd_at(r.x + r.w / 2, r.y + r.h / 2);
    uint32_t fill = readout ? C_CARD : sel ? C_ACCENT : (hov ? gui_lighten(C_PANEL, 18) : C_PANEL);
    uint32_t ink  = readout ? C_INK  : sel ? C_ACCENT_INK : C_INK_DIM;
    gui_fill_rounded_aa(win, r.x, r.y, r.w, r.h, r.h / 2, fill, outer);
    if (!sel) gui_rounded_border(win, r.x, r.y, r.w, r.h, r.h / 2, C_EDGE);
    if (label[0]) text_center(r.x, r.y, r.w, r.h, label, size, ink);
}

// The "-" / "+" faces are STROKED with the shared gui_thick_line, not typed
// (calcglass: a typed 14px hyphen is a thin dash at 1:1). 10px arms, 2px.
static void draw_zoom_face(rect_t r, int plus) {
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    gui_thick_line(win, cx - 5, cy, cx + 5, cy, 2, C_INK_DIM);
    if (plus) gui_thick_line(win, cx, cy - 5, cx, cy + 5, 2, C_INK_DIM);
}

// The toolbar: "-" "+" [NNN%] "Fit" "1:1". Fit reads selected while the zoom
// is the fit zoom for the current well; 1:1 while the zoom is 100%.
static void draw_toolbar(void) {
    int loaded = (img_width > 0 && img_height > 0);
    draw_pill(toolbar_btn_rect(BTN_ZOOM_OUT), "", TTF_FUNC, 0, hover_btn == BTN_ZOOM_OUT, 0);
    draw_zoom_face(toolbar_btn_rect(BTN_ZOOM_OUT), 0);
    draw_pill(toolbar_btn_rect(BTN_ZOOM_IN),  "", TTF_FUNC, 0, hover_btn == BTN_ZOOM_IN, 0);
    draw_zoom_face(toolbar_btn_rect(BTN_ZOOM_IN), 1);

    char zoom_str[16];
    gui_itoa(zoom_level, zoom_str, 8);
    int n = 0; while (zoom_str[n]) n++;
    zoom_str[n++] = '%'; zoom_str[n] = '\0';
    draw_pill(readout_rect(), zoom_str, TTF_TAB, 0, 0, 1);

    draw_pill(toolbar_btn_rect(BTN_FIT),    "Fit", TTF_TAB, loaded && zoom_level == fit_zoom(), hover_btn == BTN_FIT, 0);
    draw_pill(toolbar_btn_rect(BTN_ACTUAL), "1:1", TTF_TAB, loaded && zoom_level == 100,        hover_btn == BTN_ACTUAL, 0);
}

// Static chrome: the key hint, right-aligned on the backdrop. Drawn ONLY on a
// chrome-dirty frame (it sits on the backdrop and never changes, and AA text
// redrawn over itself thickens), and only when it clears the last pill.
static void draw_hint(void) {
    static const char *hint = "drag to pan   +/- zoom   F fit   1 actual";
    int hw = gui_ttf_width(hint, TTF_IND);
    int hx = g_win_w - PAD - 2 - hw;
    rect_t last = toolbar_btn_rect(BTN_ACTUAL);
    if (hx < last.x + last.w + 16) return;
    win_draw_text_ttf(win, hx, PAD + (TAB_H - TTF_IND) / 2 - 1, hint, TTF_IND, C_INK_DIM);
}

// The status row inside the panel (the panel fill under it is repainted every
// frame, so a changed string never smears): filename left, meta right.
static void draw_status(void) {
    rect_t s = status_rect();
    int text_y = s.y + (STATUS_H - TTF_TAB) / 2 - 1;

    if (img_width > 0) {
        // Filename (abbreviated) on the left.
        char name[64] = {0};
        int len = 0;
        while (current_file[len]) len++;
        int start = (len > 40) ? (len - 40) : 0;
        int i = 0;
        int j = start;
        while (current_file[j] && i < 62) name[i++] = current_file[j++];
        name[i] = '\0';
        win_draw_text_ttf(win, s.x, text_y, name, TTF_TAB, C_INK);

        // Dimensions + zoom on the right, e.g. "640 x 480  -  100%".
        char meta[48] = {0};
        char num[16];
        int m = 0;
        gui_itoa(img_width, num, 8);
        for (int k = 0; num[k]; k++) meta[m++] = num[k];
        meta[m++] = ' '; meta[m++] = 'x'; meta[m++] = ' ';
        gui_itoa(img_height, num, 8);
        for (int k = 0; num[k]; k++) meta[m++] = num[k];
        meta[m++] = ' '; meta[m++] = ' '; meta[m++] = '-'; meta[m++] = ' '; meta[m++] = ' ';
        gui_itoa(zoom_level, num, 8);
        for (int k = 0; num[k]; k++) meta[m++] = num[k];
        meta[m++] = '%';
        meta[m] = '\0';

        int mw = gui_ttf_width(meta, TTF_TAB);
        win_draw_text_ttf(win, s.x + s.w - mw, text_y, meta, TTF_TAB, C_INK_DIM);
    } else {
        win_draw_text_ttf(win, s.x, text_y, "No image loaded", TTF_TAB, C_INK_DIM);
    }
}

// The image well: compose card fill + the zoomed / panned image + the 1px
// well border offscreen, then publish with ONE win_draw_image. This is the
// LAST draw of every frame (see draw_all()), so the frame it commits is
// complete.
static void draw_well(void) {
    rect_t v = well_rect();
    int W = v.w, H = v.h;
    if (W > IV_MAXW) W = IV_MAXW;
    if (H > IV_MAXH) H = IV_MAXH;
    uint32_t card = C_CARD | 0xFF000000u;
    uint32_t edge = C_EDGE | 0xFF000000u;

    for (int R = 0; R < H; R++) {
        uint32_t *dst = g_fb + (uint32_t)R * (uint32_t)W;
        if (img_width == 0 || img_height == 0) {
            for (int C = 0; C < W; C++) dst[C] = card;
            continue;
        }
        int sy = ((R - pan_y) * 100) / zoom_level;
        for (int C = 0; C < W; C++) {
            int sx = ((C - pan_x) * 100) / zoom_level;
            if (sx >= 0 && sx < img_width && sy >= 0 && sy < img_height) {
                dst[C] = image[(uint32_t)sy * MAX_IMG_W + sx] | 0xFF000000u;
            } else {
                dst[C] = card;
            }
        }
    }
    // 1px DK_STROKE_UNSEL border (section 6 nested card), in the buffer so it
    // is part of the same commit as the image.
    for (int C = 0; C < W; C++) { g_fb[C] = edge; g_fb[(uint32_t)(H - 1) * (uint32_t)W + C] = edge; }
    for (int R = 0; R < H; R++) { g_fb[(uint32_t)R * (uint32_t)W] = edge; g_fb[(uint32_t)R * (uint32_t)W + W - 1] = edge; }

    win_draw_image(win, v.x, v.y, W, H, g_fb);   // one syscall replaces the per-pixel storm

    // Empty-state hint, drawn as TTF text on top of the well.
    if (img_width == 0 || img_height == 0) {
        gui_text_ttf_centered(win, v.x, v.y, v.w, v.h,
                              "Open a BMP image to view it here",
                              C_INK_DIM, TTF_EMPTY);
    }
}

// Full redraw
static void draw_all(void) {
    // #436: re-sync the live content size from the compositor every frame. If
    // the window was granted a different size than requested, the layout
    // reflows to the ACTUAL window instead of clipping against it.
    { int w = g_win_w, h = g_win_h;
      win_get_size(win, &w, &h);
      if (w >= MIN_WIN_W && h >= MIN_WIN_H) {
          if (w != g_win_w || h != g_win_h) g_chrome_dirty = 1;
          g_win_w = w; g_win_h = h;
      }
    }
    // (imgglass) THE ANTI-FLASH CONTRACT (docs/UI_GLASS_DESIGN_SYSTEM.md
    // section 11). Two calls in this app self-commit: the backdrop blit and
    // the well's win_draw_image. The blit runs ONLY when the chrome is dirty
    // (start, EVENT_RESIZE, EVENT_REDRAW, wallpaper change), never on a hover
    // or a zoom step. The image draw is the LAST call of the frame, after the
    // panel, pills and text are already in the content buffer as plain
    // draws, so the frame it publishes is complete. The window is never
    // cleared with a flat fill: the panel and pills cover every pixel that
    // changes, and the margins are the backdrop.
    sync_backdrop();
    if (g_chrome_dirty) {
        bd_blit();
        draw_hint();
        g_chrome_dirty = 0;
    }
    { rect_t p = panel_rect(); draw_panel(p.x, p.y, p.w, p.h); }
    draw_toolbar();
    draw_status();
    draw_well();
    win_invalidate(win);
}

// Fit image to view
static void fit_to_view(void) {
    if (img_width == 0 || img_height == 0) return;

    int zoom_w = (VIEW_W * 100) / img_width;
    int zoom_h = (VIEW_H * 100) / img_height;
    zoom_level = zoom_w < zoom_h ? zoom_w : zoom_h;
    if (zoom_level < 10) zoom_level = 10;
    if (zoom_level > 500) zoom_level = 500;

    // Center image
    int scaled_w = (img_width * zoom_level) / 100;
    int scaled_h = (img_height * zoom_level) / 100;
    pan_x = (VIEW_W - scaled_w) / 2;
    pan_y = (VIEW_H - scaled_h) / 2;
}

// Which toolbar pill is under (x, y), or -1.
static int hit_btn(int x, int y) {
    for (int id = 0; id < BTN_COUNT; id++)
        if (point_in_rect(toolbar_btn_rect(id), x, y)) return id;
    return -1;
}

// Handle toolbar click. Returns 1 if a button was hit.
static int handle_toolbar_click(int x, int y) {
    if (point_in_rect(toolbar_btn_rect(BTN_ZOOM_OUT), x, y)) {
        if (zoom_level > 10) {
            zoom_level -= 10;
            draw_all();
        }
        return 1;
    }
    if (point_in_rect(toolbar_btn_rect(BTN_ZOOM_IN), x, y)) {
        if (zoom_level < 500) {
            zoom_level += 10;
            draw_all();
        }
        return 1;
    }
    if (point_in_rect(toolbar_btn_rect(BTN_FIT), x, y)) {
        fit_to_view();
        draw_all();
        return 1;
    }
    if (point_in_rect(toolbar_btn_rect(BTN_ACTUAL), x, y)) {
        zoom_level = 100;
        pan_x = (VIEW_W - img_width) / 2;
        pan_y = (VIEW_H - img_height) / 2;
        draw_all();
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    // Create window
    win = win_create("Image Viewer", 80, 40, WIN_W, WIN_H);
    if (win < 0) {
        printf("Failed to create window\n");
        return 1;
    }

    printf("Image Viewer window created (handle=%d)\n", win);

    // (imgglass) the palette is the fixed dark glass (see the token block), so
    // there is no theme poll: the window carries its own backdrop. draw_all()
    // starts with g_chrome_dirty set, so the first frame blits the backdrop.

    // Load image from command line if provided. Large images are loaded
    // progressively: a fast point-sampled (coarse) pass is drawn first so the
    // window fills immediately, then the full-resolution pass refines it.
    if (argc > 1) {
        load_bmp_step(argv[1], 4);   // coarse 1/4-sampled preview
        fit_to_view();
        draw_all();
        load_bmp_step(argv[1], 1);   // full resolution
        fit_to_view();
        draw_all();
    } else {
        // Initial draw (empty state)
        draw_all();
    }

    // Event loop
    gui_event_t event;
    int running = 1;

    while (running) {
        int event_type = win_get_event(win, &event, 100);
        if (event_type == 0) continue;

        switch (event.type) {
            case EVENT_REDRAW:
                g_chrome_dirty = 1;
                draw_all();
                break;

            case EVENT_RESIZE:
                // New content size arrives in mouse_x (w) / mouse_y (h). A
                // resize reallocates the content buffer, so the backdrop must
                // be blitted again: chrome dirty.
                if (event.mouse_x > 0 && event.mouse_y > 0) {
                    g_win_w = event.mouse_x;
                    g_win_h = event.mouse_y;
                    g_chrome_dirty = 1;
                    if (img_width > 0) fit_to_view();  // re-fit to the new view
                    draw_all();
                }
                break;

            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;

            case EVENT_KEY_DOWN:
                if (event.key_char == 27) {  // ESC
                    running = 0;
                } else if (event.key_char == '+' || event.key_char == '=') {
                    if (zoom_level < 500) {
                        zoom_level += 10;
                        draw_all();
                    }
                } else if (event.key_char == '-') {
                    if (zoom_level > 10) {
                        zoom_level -= 10;
                        draw_all();
                    }
                } else if (event.key_char == 'f' || event.key_char == 'F') {
                    fit_to_view();
                    draw_all();
                } else if (event.key_char == '1') {
                    zoom_level = 100;
                    pan_x = (VIEW_W - img_width) / 2;
                    pan_y = (VIEW_H - img_height) / 2;
                    draw_all();
                }
                break;

            case EVENT_MOUSE_DOWN:
                if (event.mouse_buttons & MOUSE_BUTTON_LEFT) {
                    int lx = event.mouse_x;
                    int ly = event.mouse_y;

                    if (ly < PANEL_Y && handle_toolbar_click(lx, ly)) break;

                    // Start drag for panning
                    if (point_in_rect(well_rect(), lx, ly)) {
                        dragging = 1;
                        drag_start_x = lx;
                        drag_start_y = ly;
                        drag_pan_x = pan_x;
                        drag_pan_y = pan_y;
                    }
                }
                break;

            case EVENT_MOUSE_MOVE: {
                int lx = event.mouse_x;
                int ly = event.mouse_y;

                if (dragging) {
                    pan_x = drag_pan_x + (lx - drag_start_x);
                    pan_y = drag_pan_y + (ly - drag_start_y);
                    draw_all();
                } else {
                    // Track the pointer over the toolbar pills for hover
                    // fills: pills only, plain draws + one invalidate, no blit.
                    int nh = hit_btn(lx, ly);
                    if (nh != hover_btn) {
                        hover_btn = nh;
                        draw_toolbar();
                        win_invalidate(win);
                    }
                }
                break;
            }

            case EVENT_MOUSE_UP:
                dragging = 0;
                break;

            case EVENT_MOUSE_SCROLL:
                if (event.scroll_delta > 0 && zoom_level < 500) {
                    zoom_level += 10;
                    draw_all();
                } else if (event.scroll_delta < 0 && zoom_level > 10) {
                    zoom_level -= 10;
                    draw_all();
                }
                break;

            default:
                break;
        }
    }

    win_destroy(win);
    printf("Image Viewer closed\n");

    return 0;
}
