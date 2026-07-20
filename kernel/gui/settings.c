// settings.c - GUI Settings application for MayteraOS
#include "settings.h"
#include "window.h"
#include "desktop.h"
#include "themes.h"
#include "syslog.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../drivers/mouse.h"
#include "../cpu/isr.h"
#include "../boot_info.h"
#include "../drivers/pci.h"
#include "../version.h"

// Explicit declaration (draw_string_small is outside #endif guard in font.h)
extern void draw_string_small(int32_t x, int32_t y, const char *str, uint32_t color);

// ============================================================================
// Theme-aware color helpers
// ============================================================================

static inline uint32_t settings_bg_color(void) {
    return theme_get_current()->window_bg;
}

static inline uint32_t settings_text_color(void) {
    return theme_get_current()->label_text;
}

static inline uint32_t settings_header_color(void) {
    return theme_get_current()->selection_bg;
}

static inline uint32_t settings_tab_active_color(void) {
    return theme_get_current()->titlebar_active;
}

static inline uint32_t settings_tab_inactive_color(void) {
    return theme_get_current()->button_bg;
}

static inline uint32_t settings_tab_active_text(void) {
    return theme_get_current()->titlebar_text;
}

static inline uint32_t settings_tab_inactive_text(void) {
    return theme_get_current()->button_text;
}

// Lighten a color by blending toward white
static inline uint32_t lighten_color(uint32_t c, int amount) {
    int r = (c >> 16) & 0xFF;
    int g = (c >> 8) & 0xFF;
    int b = c & 0xFF;
    r = r + amount > 255 ? 255 : r + amount;
    g = g + amount > 255 ? 255 : g + amount;
    b = b + amount > 255 ? 255 : b + amount;
    return (r << 16) | (g << 8) | b;
}

// Darken a color by blending toward black
static inline uint32_t darken_color(uint32_t c, int amount) {
    int r = (c >> 16) & 0xFF;
    int g = (c >> 8) & 0xFF;
    int b = c & 0xFF;
    r = r - amount < 0 ? 0 : r - amount;
    g = g - amount < 0 ? 0 : g - amount;
    b = b - amount < 0 ? 0 : b - amount;
    return (r << 16) | (g << 8) | b;
}

static inline uint32_t settings_border_highlight(void) {
    return lighten_color(theme_get_current()->button_bg, 60);
}

static inline uint32_t settings_border_shadow(void) {
    return darken_color(theme_get_current()->button_bg, 60);
}

// ============================================================================
// Beveled drawing helpers (CDE/Motif 3D style)
// ============================================================================

// Raised rect: highlight on top/left, shadow on bottom/right
static void draw_raised_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                              uint32_t face, uint32_t highlight, uint32_t shadow) {
    fb_fill_rect(x, y, w, h, face);
    // Top highlight
    fb_fill_rect(x, y, w, 1, highlight);
    // Left highlight
    fb_fill_rect(x, y, 1, h, highlight);
    // Bottom shadow
    fb_fill_rect(x, y + h - 1, w, 1, shadow);
    // Right shadow
    fb_fill_rect(x + w - 1, y, 1, h, shadow);
}

// Sunken rect: shadow on top/left, highlight on bottom/right
static void draw_sunken_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                              uint32_t face, uint32_t highlight, uint32_t shadow) {
    fb_fill_rect(x, y, w, h, face);
    // Top shadow
    fb_fill_rect(x, y, w, 1, shadow);
    // Left shadow
    fb_fill_rect(x, y, 1, h, shadow);
    // Bottom highlight
    fb_fill_rect(x, y + h - 1, w, 1, highlight);
    // Right highlight
    fb_fill_rect(x + w - 1, y, 1, h, highlight);
}

// Global settings for launch callback
static settings_t *g_active_settings = NULL;

// Section tab names
static const char *section_names[SETTINGS_NUM_SECTIONS] = {
    "Display",
    "Themes",
    "Devices",
    "System",
    "About"
};

// Helper: convert number to string
static void settings_itoa(uint64_t num, char *buf) {
    char tmp[32];
    int i = 0;
    if (num == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (num > 0) {
        tmp[i++] = '0' + (num % 10);
        num /= 10;
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

// Draw text at a position in the window content area
static void settings_draw_text(settings_t *settings, int32_t x, int32_t y,
                                const char *text, uint32_t color) {
    if (!settings || !settings->window || !text) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(settings->window, &wx, &wy, &ww, &wh);

    // Adjust for tab bar
    int32_t base_y = wy + SETTINGS_TAB_HEIGHT;

    // Draw each character
    int32_t px = wx + x;
    int32_t py = base_y + y;

    while (*text) {
        const uint8_t *glyph = font_get_glyph(*text);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(px + col, py + row, color);
                    }
                }
            }
        }
        px += FONT_WIDTH;
        text++;
    }
}

// Draw the section tabs (3D beveled style)
static void settings_draw_tabs(settings_t *settings) {
    if (!settings || !settings->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(settings->window, &wx, &wy, &ww, &wh);

    int tab_width = ww / SETTINGS_NUM_SECTIONS;
    uint32_t hi = settings_border_highlight();
    uint32_t sh = settings_border_shadow();

    for (int i = 0; i < SETTINGS_NUM_SECTIONS; i++) {
        int32_t tab_x = wx + (i * tab_width);
        bool active = (i == settings->current_section);

        uint32_t bg = active ? settings_tab_active_color() : settings_tab_inactive_color();
        uint32_t text_col = active ? settings_tab_active_text() : settings_tab_inactive_text();

        int32_t tab_y = active ? wy : wy + 2;
        int32_t tab_h = active ? SETTINGS_TAB_HEIGHT : SETTINGS_TAB_HEIGHT - 2;

        // Draw raised 3D tab
        draw_raised_rect(tab_x, tab_y, tab_width - 2, tab_h, bg, hi, sh);

        // Active tab merges with content area: erase bottom border
        if (active) {
            fb_fill_rect(tab_x + 1, wy + SETTINGS_TAB_HEIGHT - 1, tab_width - 4, 1, settings_bg_color());
        }

        // Draw tab text centered
        const char *name = section_names[i];
        int text_len = strlen(name);
        int text_x = tab_x + (tab_width - (text_len * FONT_WIDTH)) / 2;
        int text_y = tab_y + (tab_h - FONT_HEIGHT) / 2;

        while (*name) {
            const uint8_t *glyph = font_get_glyph(*name);
            if (glyph) {
                for (int row = 0; row < FONT_HEIGHT; row++) {
                    uint8_t bits = glyph[row];
                    for (int col = 0; col < FONT_WIDTH; col++) {
                        if (bits & (0x80 >> col)) {
                            fb_put_pixel(text_x + col, text_y + row, text_col);
                        }
                    }
                }
            }
            text_x += FONT_WIDTH;
            name++;
        }
    }
}

// Draw Display section content
static void settings_draw_display(settings_t *settings) {
    char buf[64];
    int y = SETTINGS_PADDING;

    // Header
    settings_draw_text(settings, SETTINGS_PADDING, y, "Display Settings",
                       settings_header_color());
    y += SETTINGS_LINE_HEIGHT + 10;

    // Screen resolution
    uint32_t width = fb_get_width();
    uint32_t height = fb_get_height();

    settings_draw_text(settings, SETTINGS_PADDING, y, "Screen Resolution:",
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT;

    // Build resolution string: WIDTHxHEIGHT
    char res_str[32];
    settings_itoa(width, buf);
    int pos = 0;
    for (int i = 0; buf[i]; i++) res_str[pos++] = buf[i];
    res_str[pos++] = 'x';
    settings_itoa(height, buf);
    for (int i = 0; buf[i]; i++) res_str[pos++] = buf[i];
    res_str[pos++] = ' ';
    res_str[pos++] = 'p';
    res_str[pos++] = 'i';
    res_str[pos++] = 'x';
    res_str[pos++] = 'e';
    res_str[pos++] = 'l';
    res_str[pos++] = 's';
    res_str[pos] = '\0';

    settings_draw_text(settings, SETTINGS_PADDING + 20, y, res_str,
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT + 10;

    // Bits per pixel
    extern boot_info_t *g_boot_info;
    if (g_boot_info) {
        settings_draw_text(settings, SETTINGS_PADDING, y, "Color Depth:",
                           settings_text_color());
        y += SETTINGS_LINE_HEIGHT;

        settings_itoa(g_boot_info->framebuffer.bpp, buf);
        char bpp_str[32];
        pos = 0;
        for (int i = 0; buf[i]; i++) bpp_str[pos++] = buf[i];
        bpp_str[pos++] = ' ';
        bpp_str[pos++] = 'b';
        bpp_str[pos++] = 'i';
        bpp_str[pos++] = 't';
        bpp_str[pos++] = 's';
        bpp_str[pos] = '\0';

        settings_draw_text(settings, SETTINGS_PADDING + 20, y, bpp_str,
                           settings_text_color());
    }
}

// Draw System section content
static void settings_draw_system(settings_t *settings) {
    extern boot_info_t *g_boot_info;
    char buf[64];
    int y = SETTINGS_PADDING;

    // Header
    settings_draw_text(settings, SETTINGS_PADDING, y, "System Information",
                       settings_header_color());
    y += SETTINGS_LINE_HEIGHT + 10;

    // Memory information
    settings_draw_text(settings, SETTINGS_PADDING, y, "Memory:",
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT;

    if (g_boot_info) {
        // Total RAM in MB
        uint64_t total_mb = g_boot_info->total_memory / (1024 * 1024);
        settings_itoa(total_mb, buf);

        char mem_str[64];
        int pos = 0;
        mem_str[pos++] = ' ';
        mem_str[pos++] = ' ';
        mem_str[pos++] = 'T';
        mem_str[pos++] = 'o';
        mem_str[pos++] = 't';
        mem_str[pos++] = 'a';
        mem_str[pos++] = 'l';
        mem_str[pos++] = ' ';
        mem_str[pos++] = 'R';
        mem_str[pos++] = 'A';
        mem_str[pos++] = 'M';
        mem_str[pos++] = ':';
        mem_str[pos++] = ' ';
        for (int i = 0; buf[i]; i++) mem_str[pos++] = buf[i];
        mem_str[pos++] = ' ';
        mem_str[pos++] = 'M';
        mem_str[pos++] = 'B';
        mem_str[pos] = '\0';

        settings_draw_text(settings, SETTINGS_PADDING, y, mem_str,
                           settings_text_color());
        y += SETTINGS_LINE_HEIGHT;

        // #235: real used/free physical RAM from the PMM (not fabricated).
        {
            extern uint64_t pmm_get_used_pages(void);
            extern uint64_t pmm_get_free_pages(void);
            uint64_t mb_used = (pmm_get_used_pages() * 4096ULL) / (1024 * 1024);
            uint64_t mb_free = (pmm_get_free_pages() * 4096ULL) / (1024 * 1024);
            char nbuf[32]; char line[64]; int lp;
            const char *ul = "  Used RAM: "; const char *fl = "  Free RAM: ";
            settings_itoa(mb_used, nbuf);
            lp = 0;
            for (int i = 0; ul[i]; i++) line[lp++] = ul[i];
            for (int i = 0; nbuf[i]; i++) line[lp++] = nbuf[i];
            line[lp++] = ' '; line[lp++] = 'M'; line[lp++] = 'B'; line[lp] = '\0';
            settings_draw_text(settings, SETTINGS_PADDING, y, line, settings_text_color());
            y += SETTINGS_LINE_HEIGHT;
            settings_itoa(mb_free, nbuf);
            lp = 0;
            for (int i = 0; fl[i]; i++) line[lp++] = fl[i];
            for (int i = 0; nbuf[i]; i++) line[lp++] = nbuf[i];
            line[lp++] = ' '; line[lp++] = 'M'; line[lp++] = 'B'; line[lp] = '\0';
            settings_draw_text(settings, SETTINGS_PADDING, y, line, settings_text_color());
            y += SETTINGS_LINE_HEIGHT;
        }
        y += 10;

        // Kernel info
        settings_draw_text(settings, SETTINGS_PADDING, y, "Kernel:",
                           settings_text_color());
        y += SETTINGS_LINE_HEIGHT;

        uint64_t kernel_kb = g_boot_info->kernel_size / 1024;
        settings_itoa(kernel_kb, buf);

        char kern_str[64];
        pos = 0;
        kern_str[pos++] = ' ';
        kern_str[pos++] = ' ';
        kern_str[pos++] = 'S';
        kern_str[pos++] = 'i';
        kern_str[pos++] = 'z';
        kern_str[pos++] = 'e';
        kern_str[pos++] = ':';
        kern_str[pos++] = ' ';
        for (int i = 0; buf[i]; i++) kern_str[pos++] = buf[i];
        kern_str[pos++] = ' ';
        kern_str[pos++] = 'K';
        kern_str[pos++] = 'B';
        kern_str[pos] = '\0';

        settings_draw_text(settings, SETTINGS_PADDING, y, kern_str,
                           settings_text_color());
    } else {
        settings_draw_text(settings, SETTINGS_PADDING + 20, y, "Boot info unavailable",
                           settings_text_color());
    }
}

// Draw About section content
static void settings_draw_about(settings_t *settings) {
    int y = SETTINGS_PADDING;

    // Header
    settings_draw_text(settings, SETTINGS_PADDING, y, "About MayteraOS",
                       settings_header_color());
    y += SETTINGS_LINE_HEIGHT + 15;

    // Version info from version.h
    settings_draw_text(settings, SETTINGS_PADDING, y, "MayteraOS",
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT;

    char version_str[128];
    snprintf(version_str, sizeof(version_str), "Version %s (64-bit) - Build %d",
             MAYTERA_VERSION_STRING, MAYTERA_BUILD_NUMBER);
    settings_draw_text(settings, SETTINGS_PADDING, y, version_str,
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT;

    char build_date[128];
    snprintf(build_date, sizeof(build_date), "Built: %s %s",
             MAYTERA_BUILD_DATE, MAYTERA_BUILD_TIME);
    settings_draw_text(settings, SETTINGS_PADDING, y, build_date,
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT + 15;

    // Description
    settings_draw_text(settings, SETTINGS_PADDING, y, "A custom operating system",
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT;

    settings_draw_text(settings, SETTINGS_PADDING, y, "with graphical desktop",
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT;

    settings_draw_text(settings, SETTINGS_PADDING, y, "environment.",
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT + 15;

    // Features
    settings_draw_text(settings, SETTINGS_PADDING, y, "Features:",
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT;

    settings_draw_text(settings, SETTINGS_PADDING + 10, y, "- UEFI Boot",
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT;

    settings_draw_text(settings, SETTINGS_PADDING + 10, y, "- GUI Desktop",
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT;

    settings_draw_text(settings, SETTINGS_PADDING + 10, y, "- FAT Filesystem",
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT;

    settings_draw_text(settings, SETTINGS_PADDING + 10, y, "- Network Stack",
                       settings_text_color());
}

// Helper to convert hex value to string
static void settings_hex16(uint16_t val, char *buf) {
    const char hex[] = "0123456789ABCDEF";
    buf[0] = hex[(val >> 12) & 0xF];
    buf[1] = hex[(val >> 8) & 0xF];
    buf[2] = hex[(val >> 4) & 0xF];
    buf[3] = hex[val & 0xF];
    buf[4] = '\0';
}

static void settings_hex8(uint8_t val, char *buf) {
    const char hex[] = "0123456789ABCDEF";
    buf[0] = hex[(val >> 4) & 0xF];
    buf[1] = hex[val & 0xF];
    buf[2] = '\0';
}

// Draw Devices section content (PCI device list)
static void settings_draw_devices(settings_t *settings) {
    char buf[64];
    int y = SETTINGS_PADDING;

    // Header
    settings_draw_text(settings, SETTINGS_PADDING, y, "PCI Devices",
                       settings_header_color());
    y += SETTINGS_LINE_HEIGHT + 5;

    // Get device count
    int count = pci_get_device_count();
    settings_itoa(count, buf);
    char count_str[64];
    int pos = 0;
    for (int i = 0; buf[i]; i++) count_str[pos++] = buf[i];
    count_str[pos++] = ' ';
    count_str[pos++] = 'd';
    count_str[pos++] = 'e';
    count_str[pos++] = 'v';
    count_str[pos++] = 'i';
    count_str[pos++] = 'c';
    count_str[pos++] = 'e';
    if (count != 1) count_str[pos++] = 's';
    count_str[pos++] = ' ';
    count_str[pos++] = 'f';
    count_str[pos++] = 'o';
    count_str[pos++] = 'u';
    count_str[pos++] = 'n';
    count_str[pos++] = 'd';
    count_str[pos] = '\0';

    settings_draw_text(settings, SETTINGS_PADDING, y, count_str,
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT + 10;

    // Column headers
    settings_draw_text(settings, SETTINGS_PADDING, y, "VID:DID  Class  Description",
                       settings_text_color());
    y += SETTINGS_LINE_HEIGHT + 2;

    // Draw sunken separator line (2px: shadow then highlight)
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(settings->window, &wx, &wy, &ww, &wh);
    int32_t sep_x = wx + SETTINGS_PADDING;
    int32_t sep_y = wy + SETTINGS_TAB_HEIGHT + y;
    int32_t sep_w = ww - 2 * SETTINGS_PADDING;
    fb_fill_rect(sep_x, sep_y, sep_w, 1, settings_border_shadow());
    fb_fill_rect(sep_x, sep_y + 1, sep_w, 1, settings_border_highlight());
    y += SETTINGS_LINE_HEIGHT;

    // Get content area bounds to limit display
    int max_y = wh - SETTINGS_TAB_HEIGHT - 20;

    // List devices (limited by window height)
    int max_devices = (max_y - y) / SETTINGS_LINE_HEIGHT;
    if (max_devices > count) max_devices = count;

    for (int i = 0; i < max_devices; i++) {
        pci_device_t *dev = pci_get_device(i);
        if (!dev) continue;

        // Build device line: VVVV:DDDD  CC.SS  Description
        char line[80];
        pos = 0;

        // Vendor ID
        char hex[5];
        settings_hex16(dev->vendor_id, hex);
        for (int j = 0; hex[j]; j++) line[pos++] = hex[j];
        line[pos++] = ':';

        // Device ID
        settings_hex16(dev->device_id, hex);
        for (int j = 0; hex[j]; j++) line[pos++] = hex[j];
        line[pos++] = ' ';
        line[pos++] = ' ';

        // Class.Subclass
        char hex2[3];
        settings_hex8(dev->class_code, hex2);
        for (int j = 0; hex2[j]; j++) line[pos++] = hex2[j];
        line[pos++] = '.';
        settings_hex8(dev->subclass, hex2);
        for (int j = 0; hex2[j]; j++) line[pos++] = hex2[j];
        line[pos++] = ' ';
        line[pos++] = ' ';

        // Description
        const char *desc = pci_get_class_name(dev->class_code, dev->subclass);
        for (int j = 0; desc[j] && pos < 75; j++) line[pos++] = desc[j];
        line[pos] = '\0';

        settings_draw_text(settings, SETTINGS_PADDING, y, line,
                           settings_text_color());
        y += SETTINGS_LINE_HEIGHT;
    }

    // Show ellipsis if more devices
    if (count > max_devices) {
        char more[32];
        pos = 0;
        more[pos++] = '.';
        more[pos++] = '.';
        more[pos++] = '.';
        more[pos++] = ' ';
        more[pos++] = 'a';
        more[pos++] = 'n';
        more[pos++] = 'd';
        more[pos++] = ' ';
        settings_itoa(count - max_devices, buf);
        for (int i = 0; buf[i]; i++) more[pos++] = buf[i];
        more[pos++] = ' ';
        more[pos++] = 'm';
        more[pos++] = 'o';
        more[pos++] = 'r';
        more[pos++] = 'e';
        more[pos] = '\0';
        settings_draw_text(settings, SETTINGS_PADDING, y, more,
                           settings_text_color());
    }
}

// ============================================================================
// Theme preview cards
// ============================================================================

// Y offset where card grid starts (below header + subheader text)
#define THEME_GRID_START_Y  60

// Compute a contrasting text color (white on dark, black on light)
static uint32_t contrast_text_color(uint32_t bg) {
    int r = (bg >> 16) & 0xFF;
    int g = (bg >> 8) & 0xFF;
    int b = bg & 0xFF;
    int luma = (r * 299 + g * 587 + b * 114) / 1000;
    return luma < 128 ? 0xFFFFFF : 0x000000;
}

// Draw a single theme preview card showing a mini window mockup
static void settings_draw_theme_card(settings_t *settings, int theme_id, int col, int row) {
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(settings->window, &wx, &wy, &ww, &wh);

    int32_t base_y = wy + SETTINGS_TAB_HEIGHT;

    // Center the card grid horizontally
    int32_t grid_w = THEME_CARDS_PER_ROW * THEME_CARD_WIDTH + (THEME_CARDS_PER_ROW - 1) * THEME_CARD_GAP_X;
    int32_t margin_x = (ww - grid_w) / 2;

    int32_t card_x = wx + margin_x + col * (THEME_CARD_WIDTH + THEME_CARD_GAP_X);
    int32_t card_y = base_y + THEME_GRID_START_Y + row * (THEME_CARD_HEIGHT + THEME_CARD_GAP_Y);

    // Clip: don't draw cards that fall outside the content area
    if (card_y + THEME_CARD_HEIGHT > wy + wh - 4) return;

    const theme_t *theme = theme_get_by_id(theme_id);
    if (!theme) return;

    int current_id = theme_get_current_id();
    bool is_selected = (theme_id == current_id);

    // Selection border (drawn inside card area to avoid residual pixels)
    uint32_t border_color = is_selected ? 0x3399FF : 0x808080;
    int bw = is_selected ? 2 : 1;
    fb_fill_rect(card_x, card_y, THEME_CARD_WIDTH, THEME_CARD_HEIGHT, border_color);

    // Card background: theme's desktop color (inset by border width)
    fb_fill_rect(card_x + bw, card_y + bw,
                 THEME_CARD_WIDTH - 2 * bw, THEME_CARD_HEIGHT - 2 * bw,
                 theme->desktop_bg);

    // Mini window mockup, inset from card edges
    int32_t win_x = card_x + 8;
    int32_t win_y = card_y + 5;
    int32_t win_w = THEME_CARD_WIDTH - 16;  // 144px
    int32_t win_h = 46;

    // Window border (1px)
    fb_draw_rect(win_x, win_y, win_w, win_h, theme->window_border);

    // Titlebar (12px tall)
    int32_t tb_x = win_x + 1;
    int32_t tb_y = win_y + 1;
    int32_t tb_w = win_w - 2;
    int32_t tb_h = 12;
    fb_fill_rect(tb_x, tb_y, tb_w, tb_h, theme->titlebar_active);

    // Titlebar text using small font
    draw_string_small(tb_x + 2, tb_y + 2, "Window", theme->titlebar_text);

    // Close button square (8x8) in top-right of titlebar
    int32_t close_x = tb_x + tb_w - 10;
    int32_t close_y = tb_y + 2;
    fb_fill_rect(close_x, close_y, 8, 8, theme->close_button);

    // Window body
    int32_t body_x = win_x + 1;
    int32_t body_y = win_y + 1 + tb_h;
    int32_t body_w = win_w - 2;
    int32_t body_h = win_h - 2 - tb_h;
    fb_fill_rect(body_x, body_y, body_w, body_h, theme->window_bg);

    // Mini button (30x10) with button face and border
    int32_t btn_x = body_x + 4;
    int32_t btn_y = body_y + body_h - 14;
    fb_fill_rect(btn_x, btn_y, 30, 10, theme->button_bg);
    fb_draw_rect(btn_x, btn_y, 30, 10, theme->button_border);
    draw_string_small(btn_x + 11, btn_y + 1, "OK", theme->button_text);

    // Sample text "Abc" in body
    draw_string_small(body_x + 4, body_y + 3, "Abc", theme->label_text);

    // Taskbar strip (12px tall at bottom of card)
    int32_t bar_x = card_x + bw;
    int32_t bar_y = card_y + THEME_CARD_HEIGHT - bw - 12;
    fb_fill_rect(bar_x, bar_y, THEME_CARD_WIDTH - 2 * bw, 12, theme->taskbar_bg);

    // Theme name centered on taskbar, using contrasting color for readability
    const char *name = theme->name;
    int name_len = strlen(name);
    int32_t name_x = bar_x + (THEME_CARD_WIDTH - 2 * bw - name_len * FONT_SMALL_WIDTH) / 2;
    draw_string_small(name_x, bar_y + 2, name, contrast_text_color(theme->taskbar_bg));
}

// Draw Themes section content
static void settings_draw_themes(settings_t *settings) {
    int y = SETTINGS_PADDING;

    // Header
    settings_draw_text(settings, SETTINGS_PADDING, y, "Color Themes",
                       settings_header_color());
    y += SETTINGS_LINE_HEIGHT + 4;

    // Subheader
    settings_draw_text(settings, SETTINGS_PADDING, y, "Click a preview to apply:",
                       settings_text_color());

    // Draw theme preview cards in a grid (3 columns)
    int theme_count = theme_get_count();
    for (int i = 0; i < theme_count; i++) {
        int col = i % THEME_CARDS_PER_ROW;
        int row = i / THEME_CARDS_PER_ROW;
        settings_draw_theme_card(settings, i, col, row);
    }

    // Draw "Active theme:" label below the cards (only if it fits)
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(settings->window, &wx, &wy, &ww, &wh);

    int num_rows = (theme_count + THEME_CARDS_PER_ROW - 1) / THEME_CARDS_PER_ROW;
    int32_t label_y = THEME_GRID_START_Y + num_rows * (THEME_CARD_HEIGHT + THEME_CARD_GAP_Y) + 4;
    int32_t max_content_y = wh - SETTINGS_TAB_HEIGHT - 20;

    if (label_y + FONT_HEIGHT < max_content_y) {
        settings_draw_text(settings, SETTINGS_PADDING, label_y, "Active theme:",
                           settings_text_color());

        const char *current_name = theme_get_name(theme_get_current_id());
        settings_draw_text(settings, SETTINGS_PADDING + 112, label_y, current_name,
                           settings_header_color());
    }
}

// Handle theme card click
static void settings_handle_theme_click(settings_t *settings, int32_t x, int32_t y) {
    if (settings->current_section != SETTINGS_SECTION_THEMES) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(settings->window, &wx, &wy, &ww, &wh);

    int32_t base_y = wy + SETTINGS_TAB_HEIGHT;

    // Match card grid layout
    int32_t grid_w = THEME_CARDS_PER_ROW * THEME_CARD_WIDTH + (THEME_CARDS_PER_ROW - 1) * THEME_CARD_GAP_X;
    int32_t margin_x = (ww - grid_w) / 2;

    int theme_count = theme_get_count();
    for (int i = 0; i < theme_count; i++) {
        int col = i % THEME_CARDS_PER_ROW;
        int row = i / THEME_CARDS_PER_ROW;

        int32_t card_x = wx + margin_x + col * (THEME_CARD_WIDTH + THEME_CARD_GAP_X);
        int32_t card_y = base_y + THEME_GRID_START_Y + row * (THEME_CARD_HEIGHT + THEME_CARD_GAP_Y);

        if (x >= card_x && x < card_x + THEME_CARD_WIDTH &&
            y >= card_y && y < card_y + THEME_CARD_HEIGHT) {
            // Theme card clicked
            theme_set(i);
            kprintf("[Settings] Theme changed to: %s\n", theme_get_name(i));

            // Force full redraw to apply theme
            wm_invalidate_all();
            break;
        }
    }
}

// Draw the settings window content
static void settings_redraw(settings_t *settings) {
    if (!settings || !settings->window) return;

    // Get content area
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(settings->window, &wx, &wy, &ww, &wh);

    // Fill content area background
    fb_fill_rect(wx, wy, ww, wh, settings_bg_color());

    // Draw tabs
    settings_draw_tabs(settings);

    // Draw content area below tabs with sunken 3D border
    int32_t content_y = wy + SETTINGS_TAB_HEIGHT;
    int32_t content_h = wh - SETTINGS_TAB_HEIGHT;
    draw_sunken_rect(wx, content_y, ww, content_h,
                     settings_bg_color(),
                     settings_border_highlight(),
                     settings_border_shadow());

    // Draw current section content
    switch (settings->current_section) {
        case SETTINGS_SECTION_DISPLAY:
            settings_draw_display(settings);
            break;
        case SETTINGS_SECTION_THEMES:
            settings_draw_themes(settings);
            break;
        case SETTINGS_SECTION_DEVICES:
            settings_draw_devices(settings);
            break;
        case SETTINGS_SECTION_SYSTEM:
            settings_draw_system(settings);
            break;
        case SETTINGS_SECTION_ABOUT:
            settings_draw_about(settings);
            break;
    }
}

// Create and show the settings window
settings_t *settings_create(void) {
    settings_t *settings = (settings_t *)kmalloc(sizeof(settings_t));
    if (!settings) {
        kprintf("[Settings] Failed to allocate settings\n");
        return NULL;
    }

    memset(settings, 0, sizeof(settings_t));

    // Center on screen
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();
    int x = (screen_w - SETTINGS_WIDTH) / 2;
    int y = (screen_h - SETTINGS_HEIGHT) / 2 - 30;

    // Create window
    settings->window = window_create("Settings", x, y, SETTINGS_WIDTH, SETTINGS_HEIGHT);
    if (!settings->window) {
        kprintf("[Settings] Failed to create window\n");
        kfree(settings);
        return NULL;
    }

    // Set window colors from theme
    settings->window->bg_color = theme_get_current()->window_bg;

    settings->current_section = SETTINGS_SECTION_DISPLAY;
    settings->running = true;

    kprintf("[Settings] Settings window created\n");

    return settings;
}

// Destroy settings window
void settings_destroy(settings_t *settings) {
    if (!settings) return;
    if (settings->window) {
        window_destroy(settings->window);
    }
    kfree(settings);
    kprintf("[Settings] Settings window destroyed\n");
}

// Handle tab click
static void settings_handle_tab_click(settings_t *settings, int32_t x, int32_t y) {
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(settings->window, &wx, &wy, &ww, &wh);

    // Check if click is in tab area
    if (y >= wy && y < wy + SETTINGS_TAB_HEIGHT) {
        int tab_width = ww / SETTINGS_NUM_SECTIONS;
        int tab_index = (x - wx) / tab_width;

        if (tab_index >= 0 && tab_index < SETTINGS_NUM_SECTIONS) {
            settings->current_section = tab_index;
            kprintf("[Settings] Switched to section: %s\n", section_names[tab_index]);
        }
    }
}

// Run settings main loop
void settings_run(settings_t *settings) {
    if (!settings) return;

    kprintf("[Settings] Running settings window...\n");

    // Draw initial state
    window_draw(settings->window);
    settings_redraw(settings);

    // Track mouse state
    int32_t last_mouse_x = 0, last_mouse_y = 0;
    uint8_t last_buttons = 0;
    mouse_get_position(&last_mouse_x, &last_mouse_y);

    while (settings->running) {
        // Poll mouse
        mouse_poll();
        int32_t mouse_x, mouse_y;
        uint8_t buttons;
        mouse_get_position(&mouse_x, &mouse_y);
        buttons = mouse_get_buttons();

        // Handle mouse button press
        if ((buttons & MOUSE_LEFT_BTN) && !(last_buttons & MOUSE_LEFT_BTN)) {
            // Check if click is on the window
            if (mouse_x >= settings->window->bounds.x &&
                mouse_x < settings->window->bounds.x + settings->window->bounds.width &&
                mouse_y >= settings->window->bounds.y &&
                mouse_y < settings->window->bounds.y + settings->window->bounds.height) {

                // Handle tab clicks
                settings_handle_tab_click(settings, mouse_x, mouse_y);

                // Redraw
                window_draw(settings->window);
                settings_redraw(settings);
            }

            wm_handle_mouse_down(mouse_x, mouse_y, MOUSE_LEFT_BTN);
        }

        // Handle mouse movement
        if (mouse_x != last_mouse_x || mouse_y != last_mouse_y) {
            wm_handle_mouse_move(mouse_x, mouse_y);

            // Always redraw to prevent cursor trails
            desktop_draw();
            window_draw(settings->window);
            settings_redraw(settings);
            desktop_draw_cursor(mouse_x, mouse_y);

            last_mouse_x = mouse_x;
            last_mouse_y = mouse_y;
        }

        // Handle mouse button release
        if (!(buttons & MOUSE_LEFT_BTN) && (last_buttons & MOUSE_LEFT_BTN)) {
            wm_handle_mouse_up(mouse_x, mouse_y, MOUSE_LEFT_BTN);
        }

        last_buttons = buttons;

        // Check for keyboard input
        if (keyboard_has_char()) {
            char c = keyboard_get_char();

            if (c == 27) {  // ESC to close
                settings->running = false;
                break;
            } else if (c >= '1' && c <= '5') {
                settings->current_section = c - '1';
                window_draw(settings->window);
                settings_redraw(settings);
            }
        }

        // Draw cursor
        static const uint8_t cursor[] = {
            0b10000000, 0b00000000, 0b11000000, 0b00000000,
            0b11100000, 0b00000000, 0b11110000, 0b00000000,
            0b11111000, 0b00000000, 0b11111100, 0b00000000,
            0b11111110, 0b00000000, 0b11111111, 0b00000000,
            0b11111111, 0b10000000, 0b11111111, 0b11000000,
            0b11111100, 0b00000000, 0b11101100, 0b00000000,
            0b11000110, 0b00000000, 0b10000110, 0b00000000,
            0b00000011, 0b00000000, 0b00000011, 0b00000000,
        };
        for (int row = 0; row < 16; row++) {
            uint16_t bits = (cursor[row*2] << 8) | cursor[row*2 + 1];
            for (int col = 0; col < 12; col++) {
                if (bits & (0x8000 >> col)) {
                    fb_put_pixel(mouse_x + col, mouse_y + row, 0xFFFFFF);
                    fb_put_pixel(mouse_x + col + 1, mouse_y + row, 0x000000);
                }
            }
        }

        // Small delay
        for (int i = 0; i < 1000; i++) {
            __asm__ volatile("pause");
        }
    }

    kprintf("[Settings] Settings window closed\n");
}

// Public draw function
void settings_draw(settings_t *settings) {
    if (!settings || !settings->window) return;
    window_draw(settings->window);
    settings_redraw(settings);
}

// ============================================================================
// Window Manager Callback Functions (non-blocking model)
// ============================================================================

void settings_on_event(void *app_data, gui_event_t *event) {
    settings_t *settings = (settings_t *)app_data;
    if (!settings || !settings->window || !event) return;

    // Get window content bounds
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(settings->window, &wx, &wy, &ww, &wh);
    int32_t local_x = event->mouse_x - wx;
    int32_t local_y = event->mouse_y - wy;

    switch (event->type) {
        case EVENT_MOUSE_UP:
            if (event->mouse_buttons & MOUSE_BUTTON_LEFT) {
                // Check tab clicks
                if (local_y >= 0 && local_y < SETTINGS_TAB_HEIGHT) {
                    int tab_width = ww / SETTINGS_NUM_SECTIONS;
                    int clicked_tab = local_x / tab_width;
                    if (clicked_tab >= 0 && clicked_tab < SETTINGS_NUM_SECTIONS) {
                        settings->current_section = clicked_tab;
                        wm_invalidate_rect(&settings->window->bounds);
                    }
                } else {
                    // Check theme card clicks (in Themes section)
                    settings_handle_theme_click(settings, event->mouse_x, event->mouse_y);
                }
            }
            break;

        case EVENT_KEY_DOWN:
            {
                char c = event->key_char;
                if (c == 27) {  // ESC - close settings
                    kprintf("[Settings] ESC pressed, closing\n");
                    wm_unregister_app(settings->app_id);
                    if (settings->dock_index >= 0) {
                        dock_remove_app(settings->dock_index);
                    }
                    if (g_active_settings == settings) {
                        g_active_settings = NULL;
                    }
                    window_hide(settings->window);
                    wm_invalidate_all();
                    return;
                }

                // Tab navigation
                if (c == '\t') {
                    settings->current_section = (settings->current_section + 1) % SETTINGS_NUM_SECTIONS;
                    wm_invalidate_rect(&settings->window->bounds);
                }
            }
            break;

        case EVENT_WINDOW_CLOSE:
            kprintf("[Settings] Close button clicked\n");
            wm_unregister_app(settings->app_id);
            if (settings->dock_index >= 0) {
                dock_remove_app(settings->dock_index);
            }
            if (g_active_settings == settings) {
                g_active_settings = NULL;
            }
            window_hide(settings->window);
            wm_invalidate_all();
            break;

        default:
            break;
    }
}

void settings_on_draw(void *app_data) {
    settings_t *settings = (settings_t *)app_data;
    if (settings) {
        settings_draw(settings);
    }
}

void settings_on_destroy(void *app_data) {
    settings_t *settings = (settings_t *)app_data;
    if (settings) {
        kprintf("[Settings] Destroying settings instance\n");
        if (g_active_settings == settings) {
            g_active_settings = NULL;
        }
        settings_destroy(settings);
    }
}

// Launch callback for dock (non-blocking)
void settings_launch(void) {
    LOG_INFO("[Settings] Application launched");
    kprintf("[Settings] Launching settings (non-blocking)...\n");

    settings_t *settings = settings_create();
    if (!settings) {
        LOG_ERROR("[Settings] Failed to create window");
        kprintf("[Settings] Failed to create settings\n");
        return;
    }

    // Initialize WM integration fields
    settings->app_id = -1;
    settings->dock_index = -1;

    // Add to taskbar
    settings->dock_index = dock_add_app("Settings", DOCK_ICON_SETTINGS, NULL);

    // Register with window manager
    settings->app_id = wm_register_app(
        settings->window,
        settings,
        settings_on_event,
        settings_on_draw,
        settings_on_destroy
    );

    if (settings->app_id < 0) {
        kprintf("[Settings] Failed to register with window manager\n");
        if (settings->dock_index >= 0) {
            dock_remove_app(settings->dock_index);
        }
        settings_destroy(settings);
        return;
    }

    g_active_settings = settings;
    wm_invalidate_all();

    kprintf("[Settings] Settings registered as app %d\n", settings->app_id);
}
