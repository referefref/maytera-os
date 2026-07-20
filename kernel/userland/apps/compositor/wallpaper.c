// wallpaper.c - Wallpaper system for the MayteraOS userland compositor
// Loads BMP files from the FAT filesystem, scales them to the screen,
// and provides an interactive picker dialog for the user.
// No dynamic allocation: all buffers are static and sized at compile time.

#include "compositor.h"
#include "../../libc/syscall.h"

// ============================================================================
// Wallpaper list
// ============================================================================

static const wallpaper_entry_t g_wallpapers[] = {
    {"Maytera Modern",  "MAYTERA.BMP"},
    {"Maytera Cyber",   "CYBER.BMP"},
    {"Maytera Desert",  "DESERT.BMP"},
    {"Maytera Green",   "GREEN.BMP"},
    {"Maytera Mark",    "MARK.BMP"},
    {"Default Blue",    "BACK.BMP"},
    {"Mountain Vista",  "EBERG01.BMP"},
    {"Alpine Lake",     "EBERG02.BMP"},
    {"Snow Peaks",      "EBERG03.BMP"},
    {"Valley View",     "EBERG04.BMP"},
    {"Morning Mist",    "EBERG05.BMP"},
    {"Ocean Waves",     "OCEAN01.BMP"},
    {"Sunset Beach",    "OCEAN02.BMP"},
    {"Tropical Coast",  "OCEAN03.BMP"},
    {"Macro Nature 1",  "MACRO01.BMP"},
    {"Macro Nature 2",  "MACRO02.BMP"},
    {"Classic",         "CLASSIC.BMP"},
    {"Dark Mode",       "DARKMODE.BMP"},
    {"Retro",           "RETRO.BMP"},
    {"Mountain Maytera",   "MTNMAY1.BMP"},
    {"Mountain Maytera 2", "MTNMAY2.BMP"},
    {"Gradient (Blue)", NULL},
};
#define WALLPAPER_COUNT (sizeof(g_wallpapers) / sizeof(g_wallpapers[0]))

// ============================================================================
// Static pixel buffer (holds the decoded wallpaper image)
// ============================================================================

static uint32_t g_wp_pixels[MAX_SCREEN_W * MAX_SCREEN_H];
static int      g_wp_width;
static int      g_wp_height;
static bool     g_wp_loaded;

// ============================================================================
// Picker state
// ============================================================================

static int g_current_wallpaper;
static int g_picker_scroll;
static int g_picker_hover;

// ============================================================================
// File read buffer (4 MB; fits the largest expected BMP wallpaper)
// ============================================================================

static uint8_t g_file_buf[4 * 1024 * 1024];

// ============================================================================
// Internal: read a 2-byte little-endian value from a byte array
// ============================================================================

static uint32_t read_u16_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

// ============================================================================
// Internal: read a 4-byte little-endian value from a byte array
// ============================================================================

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

// ============================================================================
// parse_bmp
// Decodes an uncompressed 24-bit or 32-bit BMP from the in-memory buffer.
// On success the pixels are stored in g_wp_pixels and 0 is returned.
// On failure -1 is returned and g_wp_loaded is left unchanged.
// ============================================================================

static int parse_bmp(const uint8_t *data, uint32_t size)
{
    // Minimum viable BMP header is 54 bytes.
    if (size < 54) return -1;

    // Check BMP signature.
    if (data[0] != 'B' || data[1] != 'M') return -1;

    // Offset to the first pixel byte.
    uint32_t pixel_offset = read_u32_le(data + 10);
    if (pixel_offset >= size) return -1;

    // DIB header size (determines which variant of the DIB header is in use).
    // We only need fields common to BITMAPINFOHEADER (size >= 40).
    uint32_t dib_size = read_u32_le(data + 14);
    if (dib_size < 40) return -1;   // older OS/2 format; not supported

    int32_t  width  = (int32_t)read_u32_le(data + 18);
    int32_t  height = (int32_t)read_u32_le(data + 22); // positive = bottom-up
    uint32_t bpp    = read_u16_le(data + 28);

    // Only 24-bit and 32-bit uncompressed formats are supported.
    if (bpp != 24 && bpp != 32) return -1;

    // Reject pathological sizes.
    if (width  <= 0 || width  > MAX_SCREEN_W) return -1;
    if (height <= 0 || height > MAX_SCREEN_H) return -1;

    // Row stride in the file must be 4-byte aligned.
    uint32_t bytes_per_pixel = bpp / 8;
    uint32_t stride = ((uint32_t)(width) * bytes_per_pixel + 3) & ~(uint32_t)3;

    // Verify the file is large enough to hold all pixel data.
    if (pixel_offset + stride * (uint32_t)height > size) return -1;

    // Decode rows: BMP stores bottom-up, so row 0 in the file is the
    // bottom row of the image. We reverse this when writing to g_wp_pixels.
    for (int32_t row = 0; row < height; row++) {
        // Source row: (height - 1 - row) because of bottom-up storage.
        uint32_t src_row_idx = (uint32_t)(height - 1 - row);
        const uint8_t *src = data + pixel_offset + src_row_idx * stride;

        // Destination row: top-down in our pixel buffer.
        uint32_t *dst = g_wp_pixels + (uint32_t)row * (uint32_t)width;

        for (int32_t col = 0; col < width; col++) {
            uint8_t b = src[col * (int32_t)bytes_per_pixel + 0];
            uint8_t g = src[col * (int32_t)bytes_per_pixel + 1];
            uint8_t r = src[col * (int32_t)bytes_per_pixel + 2];
            // Alpha: always opaque regardless of channel presence.
            dst[col] = 0xFF000000u
                     | ((uint32_t)r << 16)
                     | ((uint32_t)g <<  8)
                     | (uint32_t)b;
        }
    }

    g_wp_width  = (int)width;
    g_wp_height = (int)height;
    return 0;
}

// ============================================================================
// wallpaper_load
// Opens the BMP file for wallpaper[index], reads it in 64 KB chunks into
// g_file_buf, then calls parse_bmp.  On success g_wp_loaded is set to true.
// ============================================================================

void wallpaper_load(int index)
{
    // Validate index and check that a filename is provided.
    if (index < 0 || index >= (int)WALLPAPER_COUNT
            || g_wallpapers[index].filename == NULL) {
        g_wp_loaded = false;
        return;
    }

    const char *filename = g_wallpapers[index].filename;

    int fd = sys_open(filename, 0);
    if (fd < 0) {
        g_wp_loaded = false;
        return;
    }

    // Read the entire file in 64 KB chunks.
    uint32_t total = 0;
    uint32_t max   = sizeof(g_file_buf);
    while (total < max) {
        uint32_t chunk = 64 * 1024;
        if (chunk > max - total) chunk = max - total;
        long n = sys_read(fd, g_file_buf + total, chunk);
        if (n <= 0) break;
        total += (uint32_t)n;
    }

    sys_close(fd);

    if (total < 54) {
        // File too small to be a valid BMP.
        g_wp_loaded = false;
        return;
    }

    if (parse_bmp(g_file_buf, total) == 0) {
        g_wp_loaded         = true;
        g_current_wallpaper = index;
    } else {
        g_wp_loaded = false;
    }
}

// ============================================================================
// wallpaper_init
// Must be called once before any other wallpaper function.
// ============================================================================

// Returns the index of the currently loaded wallpaper (for cross-app sync).
int wallpaper_current(void)
{
    return g_current_wallpaper;
}

void wallpaper_init(void)
{
    g_wp_loaded         = false;
    g_wp_width          = 0;
    g_wp_height         = 0;
    g_current_wallpaper = 0;
    g_picker_scroll     = 0;
    g_picker_hover      = -1;

    // Attempt to load the default wallpaper; if it fails the gradient is used.
    wallpaper_load(0);
}

// ============================================================================
// wallpaper_render_background
// Draws either the loaded BMP (scaled if necessary) or a vertical gradient.
// ============================================================================

void wallpaper_render_background(void)
{
    if (!g_wp_loaded || g_wp_width <= 0 || g_wp_height <= 0) {
        // Fall back to a vertical gradient.
        draw_gradient_v(0, 0, g_fb_width, g_fb_height,
                        CLR_WP_GRAD_TOP, CLR_WP_GRAD_BOT);
        return;
    }

    if (g_wp_width == g_fb_width && g_wp_height == g_fb_height) {
        // Exact match: blit row by row. MUST honor g_fb_pitch (pixels per
        // framebuffer row), which can exceed g_fb_width when the GPU pads
        // scanlines for alignment. A single flat memcpy of width*height
        // pixels (ignoring pitch) shifts every row progressively, shearing
        // the image into a "several images merged, wrong colors" mess and
        // leaving a corruption seam. The wallpaper buffer is packed at
        // g_wp_width stride.
        for (int32_t y = 0; y < g_fb_height; y++) {
            memcpy(g_fb        + (uint32_t)y * (uint32_t)g_fb_pitch,
                   g_wp_pixels + (uint32_t)y * (uint32_t)g_wp_width,
                   (unsigned long)g_fb_width * sizeof(uint32_t));
        }
        return;
    }

    // Nearest-neighbor scale to fill the screen.
    int32_t sw = g_fb_width;
    int32_t sh = g_fb_height;
    for (int32_t dy = 0; dy < sh; dy++) {
        // Source row index, scaled.
        int32_t sy = (dy * g_wp_height) / sh;
        if (sy >= g_wp_height) sy = g_wp_height - 1;

        const uint32_t *src_row = g_wp_pixels + (uint32_t)sy * (uint32_t)g_wp_width;
        uint32_t       *dst_row = g_fb + (uint32_t)dy * (uint32_t)g_fb_pitch;

        for (int32_t dx = 0; dx < sw; dx++) {
            int32_t sx = (dx * g_wp_width) / sw;
            if (sx >= g_wp_width) sx = g_wp_width - 1;
            dst_row[dx] = src_row[sx];
        }
    }
}

// ============================================================================
// wallpaper_picker_open / wallpaper_picker_close
// ============================================================================

void wallpaper_picker_open(void)
{
    g_wallpaper_picker_open = true;
    g_picker_scroll         = 0;
    g_picker_hover          = -1;
}

void wallpaper_picker_close(void)
{
    g_wallpaper_picker_open = false;
}

// ============================================================================
// wallpaper_render_picker
// Draws the wallpaper-chooser dialog centered on the screen.
//
// Layout:
//   Title bar (PICKER_TITLE_H px) with "Choose Wallpaper" and a close button.
//   A scrollable grid of THUMB_COLS thumbnails per row, each THUMB_CELL_W x
//   THUMB_CELL_H.  The name is drawn inside each cell.  The currently active
//   wallpaper is highlighted with a CLR_PICKER_SEL border.
//   Scroll indicators are drawn at the bottom when content overflows.
// ============================================================================

void wallpaper_render_picker(void)
{
    if (!g_wallpaper_picker_open) return;

    int32_t dlg_x = (g_fb_width  - PICKER_WIDTH)  / 2;
    int32_t dlg_y = (g_fb_height - PICKER_HEIGHT) / 2;

    // Background panel.
    draw_fill_rect(dlg_x, dlg_y, PICKER_WIDTH, PICKER_HEIGHT, CLR_PICKER_BG);
    draw_rect_outline(dlg_x, dlg_y, PICKER_WIDTH, PICKER_HEIGHT, CLR_PICKER_BORDER);

    // Title bar.
    draw_fill_rect(dlg_x, dlg_y, PICKER_WIDTH, PICKER_TITLE_H, CLR_PICKER_TITLE);

    // Title text: centered vertically in the title bar.
    int32_t title_text_y = dlg_y + (PICKER_TITLE_H - FONT_CHAR_H) / 2;
    draw_text_centered(dlg_x + PICKER_WIDTH / 2, title_text_y,
                       "Choose Wallpaper", CLR_TEXT_WHITE);

    // Close button: 16 x (PICKER_TITLE_H) in the top-right corner.
    int32_t close_w = PICKER_TITLE_H;
    int32_t close_x = dlg_x + PICKER_WIDTH - close_w;
    int32_t close_y = dlg_y;
    draw_fill_rect(close_x, close_y, close_w, PICKER_TITLE_H, 0xFF662222);
    draw_rect_outline(close_x, close_y, close_w, PICKER_TITLE_H, CLR_PICKER_BORDER);
    // "X" glyph, centered.
    int32_t x_gx = close_x + (close_w - FONT_CHAR_W) / 2;
    int32_t x_gy = close_y + (PICKER_TITLE_H - FONT_CHAR_H) / 2;
    draw_char(x_gx, x_gy, 'X', CLR_TEXT_WHITE);

    // Separator line below title bar.
    draw_hline(dlg_x, dlg_y + PICKER_TITLE_H, PICKER_WIDTH, CLR_PICKER_BORDER);

    // Grid content area.
    int32_t grid_x    = dlg_x + THUMB_PADDING;
    int32_t grid_y    = dlg_y + PICKER_TITLE_H + THUMB_PADDING;
    int32_t grid_h    = PICKER_HEIGHT - PICKER_TITLE_H - THUMB_PADDING * 2 - 16;
    int32_t rows_vis  = grid_h / THUMB_CELL_H;
    if (rows_vis < 1) rows_vis = 1;

    int32_t total_rows = ((int32_t)WALLPAPER_COUNT + THUMB_COLS - 1) / THUMB_COLS;
    // Clamp scroll.
    int32_t max_scroll = total_rows - rows_vis;
    if (max_scroll < 0) max_scroll = 0;
    if (g_picker_scroll > max_scroll) g_picker_scroll = max_scroll;
    if (g_picker_scroll < 0)         g_picker_scroll = 0;

    // Draw each visible thumbnail cell.
    for (int row = g_picker_scroll; row < g_picker_scroll + rows_vis; row++) {
        int32_t vis_row = row - g_picker_scroll;  // 0-based visible row index
        for (int col = 0; col < THUMB_COLS; col++) {
            int idx = row * THUMB_COLS + col;
            if (idx >= (int)WALLPAPER_COUNT) break;

            int32_t cx = grid_x + col * THUMB_CELL_W;
            int32_t cy = grid_y + vis_row * THUMB_CELL_H;

            // Hover highlight background.
            uint32_t cell_bg = CLR_PICKER_THUMB;
            if (idx == g_picker_hover) {
                cell_bg = 0xFF484848;
            }

            // Thumbnail background.
            draw_fill_rect(cx, cy, THUMB_WIDTH, THUMB_HEIGHT, cell_bg);

            // Draw a small gradient preview inside the thumbnail area.
            if (g_wallpapers[idx].filename == NULL) {
                // Gradient entry: show the actual gradient colors.
                draw_gradient_v(cx, cy, THUMB_WIDTH, THUMB_HEIGHT,
                                CLR_WP_GRAD_TOP, CLR_WP_GRAD_BOT);
            } else {
                // BMP entry: draw a stylized placeholder using a dim rectangle
                // with a small "image" icon hint.
                draw_gradient_v(cx, cy, THUMB_WIDTH, THUMB_HEIGHT,
                                0xFF404858, 0xFF283040);
                // Small picture-frame indicator in the center of the thumbnail.
                int32_t frame_w = 20, frame_h = 14;
                int32_t frame_x = cx + (THUMB_WIDTH  - frame_w) / 2;
                int32_t frame_y = cy + (THUMB_HEIGHT - frame_h) / 2;
                draw_fill_rect(frame_x, frame_y, frame_w, frame_h, 0xFF505868);
                draw_rect_outline(frame_x, frame_y, frame_w, frame_h, 0xFF8090A0);
            }

            // Selection border for the currently loaded wallpaper.
            if (idx == g_current_wallpaper) {
                draw_rect_outline(cx - 1, cy - 1,
                                  THUMB_WIDTH + 2, THUMB_HEIGHT + 2,
                                  CLR_PICKER_SEL);
                draw_rect_outline(cx - 2, cy - 2,
                                  THUMB_WIDTH + 4, THUMB_HEIGHT + 4,
                                  CLR_PICKER_SEL);
            } else {
                draw_rect_outline(cx, cy, THUMB_WIDTH, THUMB_HEIGHT,
                                  CLR_PICKER_BORDER);
            }

            // Hover border (drawn on top of selection so it is always visible).
            if (idx == g_picker_hover && idx != g_current_wallpaper) {
                draw_rect_outline(cx, cy, THUMB_WIDTH, THUMB_HEIGHT, 0xFF8090C0);
            }

            // Name label below thumbnail, truncated to fit the cell.
            const char *name = g_wallpapers[idx].name;
            int32_t    label_y = cy + THUMB_HEIGHT + 2;
            // Truncate to THUMB_CELL_W characters.
            char  label_buf[16];
            int   max_chars = (THUMB_CELL_W - 2) / FONT_CHAR_W;
            if (max_chars > 15) max_chars = 15;
            int ni = 0;
            while (name[ni] && ni < max_chars) {
                label_buf[ni] = name[ni];
                ni++;
            }
            label_buf[ni] = '\0';
            draw_text(cx, label_y, label_buf, CLR_PICKER_LABEL);
        }
    }

    // Scroll indicator at the bottom of the dialog (only if content overflows).
    if (total_rows > rows_vis) {
        int32_t ind_y  = dlg_y + PICKER_HEIGHT - 14;
        int32_t ind_x  = dlg_x + PICKER_WIDTH / 2;
        // Up arrow indicator.
        if (g_picker_scroll > 0) {
            draw_text(ind_x - 20, ind_y, "^", CLR_PICKER_LABEL);
        }
        // Down arrow indicator.
        if (g_picker_scroll < max_scroll) {
            draw_text(ind_x + 12, ind_y, "v", CLR_PICKER_LABEL);
        }
        // Page position text.
        char pos_buf[16];
        int p = g_picker_scroll + 1;
        int t = total_rows;
        // Build "p/t" string without sprintf.
        int bi = 0;
        if (p >= 10) pos_buf[bi++] = (char)('0' + p / 10);
        pos_buf[bi++] = (char)('0' + p % 10);
        pos_buf[bi++] = '/';
        if (t >= 10) pos_buf[bi++] = (char)('0' + t / 10);
        pos_buf[bi++] = (char)('0' + t % 10);
        pos_buf[bi] = '\0';
        draw_text(ind_x - (bi * FONT_CHAR_W) / 2, ind_y, pos_buf, CLR_PICKER_LABEL);
    }
}

// ============================================================================
// wallpaper_picker_handle_mouse
// Returns true if the event was consumed (picker is open and point is inside).
// ============================================================================

bool wallpaper_picker_handle_mouse(int32_t x, int32_t y, bool clicked)
{
    if (!g_wallpaper_picker_open) return false;

    int32_t dlg_x = (g_fb_width  - PICKER_WIDTH)  / 2;
    int32_t dlg_y = (g_fb_height - PICKER_HEIGHT) / 2;

    // If the cursor is outside the dialog, do not consume the event.
    if (x < dlg_x || x >= dlg_x + PICKER_WIDTH  ||
        y < dlg_y || y >= dlg_y + PICKER_HEIGHT) {
        return false;
    }

    // Close button hit test.
    int32_t close_w = PICKER_TITLE_H;
    int32_t close_x = dlg_x + PICKER_WIDTH - close_w;
    int32_t close_y = dlg_y;
    if (x >= close_x && x < close_x + close_w &&
        y >= close_y && y < close_y + PICKER_TITLE_H) {
        if (clicked) {
            wallpaper_picker_close();
            g_needs_redraw = true;
        }
        return true;
    }

    // Grid content area.
    int32_t grid_x   = dlg_x + THUMB_PADDING;
    int32_t grid_y   = dlg_y + PICKER_TITLE_H + THUMB_PADDING;
    int32_t grid_h   = PICKER_HEIGHT - PICKER_TITLE_H - THUMB_PADDING * 2 - 16;
    int32_t rows_vis = grid_h / THUMB_CELL_H;
    if (rows_vis < 1) rows_vis = 1;

    // Scroll indicator zone at the bottom.
    int32_t ind_y = dlg_y + PICKER_HEIGHT - 14;
    if (y >= ind_y) {
        // Click on the scroll indicator row.
        if (clicked) {
            int32_t total_rows = ((int32_t)WALLPAPER_COUNT + THUMB_COLS - 1) / THUMB_COLS;
            int32_t max_scroll = total_rows - rows_vis;
            if (max_scroll < 0) max_scroll = 0;
            int32_t mid = dlg_x + PICKER_WIDTH / 2;
            if (x < mid) {
                // Left side: scroll up.
                if (g_picker_scroll > 0) {
                    g_picker_scroll--;
                    g_needs_redraw = true;
                }
            } else {
                // Right side: scroll down.
                if (g_picker_scroll < max_scroll) {
                    g_picker_scroll++;
                    g_needs_redraw = true;
                }
            }
        }
        return true;
    }

    // Check if cursor is inside the grid.
    if (x < grid_x || y < grid_y) return true;

    int32_t rel_x = x - grid_x;
    int32_t rel_y = y - grid_y;

    int col = (int)(rel_x / THUMB_CELL_W);
    int row = (int)(rel_y / THUMB_CELL_H) + g_picker_scroll;

    if (col < 0 || col >= THUMB_COLS) return true;

    // Verify the cursor is within the actual thumbnail area (not the padding).
    int32_t cell_local_x = rel_x - col * THUMB_CELL_W;
    int32_t cell_local_y = rel_y - ((int)(rel_y / THUMB_CELL_H)) * THUMB_CELL_H;
    if (cell_local_x > THUMB_WIDTH || cell_local_y > THUMB_CELL_H) return true;

    int idx = row * THUMB_COLS + col;
    if (idx < 0 || idx >= (int)WALLPAPER_COUNT) return true;

    // Update hover state.
    if (g_picker_hover != idx) {
        g_picker_hover = idx;
        g_needs_redraw = true;
    }

    if (clicked) {
        wallpaper_load(idx);
        set_wallpaper(idx);   // sync shared index so Settings reflects the choice
        wallpaper_picker_close();
        g_needs_redraw = true;
    }

    return true;
}
