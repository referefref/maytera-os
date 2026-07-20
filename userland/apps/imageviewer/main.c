// imageviewer - Image Viewer for MayteraOS (user-space version)
// Displays BMP images with zoom and pan support.
// UI uplifted to the modern libc style engine: themed background, a raised
// toolbar card with style-aware buttons, TTF labels, and a slim status card.
// This is a visual / layout pass only; all decode, navigation, zoom, fit, and
// keyboard / mouse behavior is preserved.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"
#include "../../libc/gui_style.h"

#define WIN_W 640
#define WIN_H 480
#define TOOLBAR_H 44
#define STATUSBAR_H 28
#define VIEW_X 0
#define VIEW_Y TOOLBAR_H
#define VIEW_W g_win_w
#define VIEW_H (g_win_h - TOOLBAR_H - STATUSBAR_H)

#define MAX_IMG_W 2048
#define MAX_IMG_H 2048

// Toolbar layout tokens (kept in one place so draw + hit-test agree).
#define TB_PAD       8     // inset from window edge to first control
#define TB_BTN_Y     7     // button top inside the toolbar
#define TB_BTN_H     30    // button height
#define TB_GAP       6     // gap between buttons
#define TB_BTN_SM    36    // small (icon-ish) button width
#define TB_BTN_MD    48    // medium (text label) button width

// Image data
static uint32_t image[MAX_IMG_W * MAX_IMG_H];

// #246: offscreen window-content framebuffer. draw_view() composes the scaled
// image here (pure memory, no syscalls) and pushes the whole content with ONE
// SYS_WIN_BLIT, instead of ~VIEW_W*VIEW_H per-pixel win_draw_pixel syscalls.
#define IV_MAXW 1280
#define IV_MAXH 800
static uint32_t g_fb[IV_MAXW * IV_MAXH];
static int img_width = 0;
static int img_height = 0;
static char current_file[256] = {0};

// View state
static int win = -1;

static inline void iv_blit(int w, int h) {
    if (w > IV_MAXW) w = IV_MAXW;
    if (h > IV_MAXH) h = IV_MAXH;
    syscall5(SYS_WIN_BLIT, win, 0, 0,
             (w & 0xFFFF) | ((h & 0xFFFF) << 16), (long)g_fb);
}
// Live window content size. Starts at the create size and is updated on
// EVENT_RESIZE so the whole layout (toolbar, view, status bar) reflows.
static int g_win_w = WIN_W, g_win_h = WIN_H;
static int zoom_level = 100;  // percentage
static int pan_x = 0, pan_y = 0;
static int dragging = 0;
static int drag_start_x = 0, drag_start_y = 0;
static int drag_pan_x = 0, drag_pan_y = 0;

// Pointer position over the toolbar, used for button hover states.
static int hover_x = -1, hover_y = -1;

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

// Geometry of a toolbar button.
typedef struct { int x, y, w, h; } rect_t;

// Compute the rect for a toolbar button id. Buttons are laid left to right;
// the zoom-percent label sits between the +/- pair and Fit / 1:1.
static rect_t toolbar_btn_rect(int id) {
    rect_t r;
    r.y = TB_BTN_Y;
    r.h = TB_BTN_H;
    switch (id) {
        case BTN_ZOOM_OUT:
            r.x = TB_PAD;                       r.w = TB_BTN_SM; break;
        case BTN_ZOOM_IN:
            r.x = TB_PAD + TB_BTN_SM + TB_GAP;  r.w = TB_BTN_SM; break;
        case BTN_FIT:
            // After the +/- pair and a wider gap that holds the zoom label.
            r.x = TB_PAD + (TB_BTN_SM + TB_GAP) * 2 + 56;  r.w = TB_BTN_MD; break;
        case BTN_ACTUAL:
            r.x = TB_PAD + (TB_BTN_SM + TB_GAP) * 2 + 56 + TB_BTN_MD + TB_GAP;
            r.w = TB_BTN_MD; break;
        default:
            r.x = 0; r.w = 0; break;
    }
    return r;
}

static int point_in_rect(rect_t r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// --------------------------------------------------------------------------
// Theme: build the style-engine palette from the live system theme so the
// viewer follows the active theme exactly like Settings does.
// --------------------------------------------------------------------------
static void apply_theme(void) {
    // Read semantic colors from the kernel theme. These honor whatever theme
    // the user picked in Settings (Midnight / Light / Classic / Ocean / Dark).
    uint32_t win_bg   = theme_color(THEME_COLOR_WINDOW_BG);
    uint32_t fg       = theme_color(THEME_COLOR_LABEL_TEXT);
    uint32_t accent   = theme_color(THEME_COLOR_ACCENT);
    uint32_t btn_face = theme_color(THEME_COLOR_BUTTON_FACE);
    uint32_t btn_lt   = theme_color(THEME_COLOR_BUTTON_LIGHT);
    uint32_t border   = theme_color(THEME_COLOR_WINDOW_BORDER);
    uint32_t field_bg = theme_color(THEME_COLOR_TEXTBOX_BG);
    uint32_t track    = theme_color(THEME_COLOR_SCROLLBAR_BG);

    // Classic (Win95) theme keeps the beveled renderer; everything else uses
    // the modern rounded / soft-shadow renderer.
    int active = get_theme();
    int classic = (active == 4);  // kernel theme id 4 = Classic
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

    (void)btn_face; (void)btn_lt;  // available if needed by custom chrome
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


// Draw one toolbar button in the style engine, with hover / pressed feedback.
static void draw_btn(int id, const char *label) {
    rect_t r = toolbar_btn_rect(id);
    gui_state_t st = GUI_ST_NORMAL;
    if (point_in_rect(r, hover_x, hover_y)) {
        st = (dragging == 0) ? GUI_ST_HOVER : GUI_ST_NORMAL;
    }
    gui_button(win, r.x, r.y, r.w, r.h, label, GUI_BTN_SECONDARY, st);
}

// Draw the modern toolbar: a raised card spanning the top, holding style-aware
// buttons and a TTF zoom-percent readout.
static void draw_toolbar(void) {
    gui_palette_t *pal = gui_pal();

    // Toolbar surface (a flush raised card, no rounded outer corners so it
    // sits cleanly against the window edges).
    win_draw_rect(win, 0, 0, g_win_w, TOOLBAR_H, pal->surface_raised);
    // 1px separator under the toolbar for a crisp edge.
    win_draw_rect(win, 0, TOOLBAR_H - 1, g_win_w, 1, pal->border);

    draw_btn(BTN_ZOOM_OUT, "-");
    draw_btn(BTN_ZOOM_IN, "+");

    // Zoom percentage label, centered in the gap between the +/- pair and Fit.
    char zoom_str[16] = {0};
    int z = zoom_level;
    int i = 0;
    if (z >= 100) { zoom_str[i++] = '0' + (z / 100); z %= 100; }
    zoom_str[i++] = '0' + (z / 10);
    zoom_str[i++] = '0' + (z % 10);
    zoom_str[i++] = '%';
    zoom_str[i] = '\0';

    rect_t zin = toolbar_btn_rect(BTN_ZOOM_IN);
    rect_t fit = toolbar_btn_rect(BTN_FIT);
    int label_x = zin.x + zin.w;
    int label_w = fit.x - label_x;
    gui_text_ttf_centered(win, label_x, TB_BTN_Y, label_w, TB_BTN_H,
                          zoom_str, pal->ink, GUI_TTF_SIZE);

    draw_btn(BTN_FIT, "Fit");
    draw_btn(BTN_ACTUAL, "1:1");
}

// Draw status bar
static void draw_statusbar(void) {
    gui_palette_t *pal = gui_pal();
    int sy = g_win_h - STATUSBAR_H;

    // Raised status card across the bottom with a top separator.
    win_draw_rect(win, 0, sy, g_win_w, STATUSBAR_H, pal->surface_raised);
    win_draw_rect(win, 0, sy, g_win_w, 1, pal->border);

    int text_y = sy + (STATUSBAR_H - GUI_TTF_SIZE) / 2;

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
        win_draw_text_ttf(win, TB_PAD, text_y, name, GUI_TTF_SIZE, pal->ink);

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

        int mw = gui_ttf_width(meta, GUI_TTF_SIZE);
        win_draw_text_ttf(win, g_win_w - TB_PAD - mw, text_y, meta,
                          GUI_TTF_SIZE, pal->ink_dim);
    } else {
        win_draw_text_ttf(win, TB_PAD, text_y, "No image loaded",
                          GUI_TTF_SIZE, pal->ink_dim);
    }
}

// Draw image view
static void draw_view(void) {
    gui_palette_t *pal = gui_pal();
    int W = g_win_w, H = g_win_h;
    if (W > IV_MAXW) W = IV_MAXW;
    if (H > IV_MAXH) H = IV_MAXH;
    uint32_t surf = pal->surface | 0xFF000000u;

    // Compose the whole window content offscreen (toolbar/statusbar bands are
    // filled with the surface color and then overdrawn with chrome on top).
    int vy0 = TOOLBAR_H;
    int vy1 = H - STATUSBAR_H;
    if (vy1 < vy0) vy1 = vy0;

    for (int R = 0; R < H; R++) {
        uint32_t *dst = g_fb + (uint32_t)R * (uint32_t)W;
        if (R < vy0 || R >= vy1 || img_width == 0 || img_height == 0) {
            for (int C = 0; C < W; C++) dst[C] = surf;
            continue;
        }
        int dy = R - TOOLBAR_H;
        int sy = ((dy - pan_y) * 100) / zoom_level;
        for (int C = 0; C < W; C++) {
            int sx = ((C - pan_x) * 100) / zoom_level;
            if (sx >= 0 && sx < img_width && sy >= 0 && sy < img_height) {
                dst[C] = image[(uint32_t)sy * MAX_IMG_W + sx] | 0xFF000000u;
            } else {
                int checker = (((C) / 16) + ((R) / 16)) & 1;
                dst[C] = checker ? 0xFF808080u : 0xFF606060u;
            }
        }
    }

    iv_blit(W, H);   // one syscall replaces the per-pixel storm

    // Empty-state hint, drawn as TTF text on top of the blitted background.
    if (img_width == 0 || img_height == 0) {
        gui_text_ttf_centered(win, VIEW_X, VIEW_Y, VIEW_W, VIEW_H,
                              "Open a BMP image to view it here",
                              pal->ink_dim, GUI_TTF_SIZE);
    }
}

// Full redraw
static void draw_all(void) {
    draw_view();        // composes + blits the whole window content
    draw_toolbar();     // chrome drawn on top of the blit
    draw_statusbar();
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

    // Adopt the active system theme for all style-engine primitives.
    apply_theme();

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
                draw_all();
                break;

            case EVENT_RESIZE:
                // New content size arrives in mouse_x (w) / mouse_y (h).
                if (event.mouse_x > 0 && event.mouse_y > 0) {
                    g_win_w = event.mouse_x;
                    g_win_h = event.mouse_y;
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

                    if (ly < TOOLBAR_H && handle_toolbar_click(lx, ly)) break;

                    // Start drag for panning
                    if (ly >= VIEW_Y && ly < VIEW_Y + VIEW_H) {
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
                    // Track pointer over the toolbar for button hover states.
                    int was_over = (hover_y >= 0 && hover_y < TOOLBAR_H);
                    int now_over = (ly >= 0 && ly < TOOLBAR_H);
                    hover_x = lx;
                    hover_y = ly;
                    if (now_over || was_over) draw_toolbar();
                    if (now_over || was_over) win_invalidate(win);
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
