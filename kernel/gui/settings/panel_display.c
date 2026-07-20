// Suppress warnings
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
// panel_display.c - Display Settings Panel for MayteraOS Settings App
// Part of the unified Settings application framework (Task #59)
#include "panel_display.h"
#include "settings_widgets.h"
#include "../window.h"
#include "../desktop.h"
#include "../../types.h"
#include "../../serial.h"
#include "../../mm/heap.h"
#include "../../string.h"
#include "../../video/framebuffer.h"
#include "../../video/font.h"
#include "../../boot_info.h"
#include "../themes.h"

extern boot_info_t *g_boot_info;

#define DISPLAY_PANEL_PADDING       16
#define DISPLAY_SECTION_SPACING     24
#define DISPLAY_LINE_HEIGHT         24
#define DISPLAY_BUTTON_HEIGHT       28
#define RESOLUTION_REVERT_TIMEOUT   15

typedef struct {
    uint32_t width;
    uint32_t height;
    const char *label;
} resolution_t;

static const resolution_t g_resolutions[] = {
    {640, 480, "640 x 480 (VGA)"},
    {800, 600, "800 x 600 (SVGA)"},
    {1024, 768, "1024 x 768 (XGA)"},
    {1280, 720, "1280 x 720 (720p)"},
    {1280, 800, "1280 x 800 (WXGA)"},
    {1280, 1024, "1280 x 1024 (SXGA)"},
    {1366, 768, "1366 x 768"},
    {1440, 900, "1440 x 900 (WXGA+)"},
    {1600, 900, "1600 x 900 (HD+)"},
    {1920, 1080, "1920 x 1080 (1080p)"},
    {2560, 1440, "2560 x 1440 (1440p)"},
    {3840, 2160, "3840 x 2160 (4K)"},
    {0, 0, NULL}
};

static const uint32_t g_refresh_rates[] = {30, 50, 60, 75, 85, 100, 120, 144, 0};

typedef struct {
    int id;
    char name[32];
    uint32_t width, height;
    int32_t pos_x, pos_y;
    bool is_primary, is_connected;
    int current_resolution, current_refresh;
} monitor_info_t;

#define MAX_MONITORS 4

typedef struct {
    bool initialized;
    monitor_info_t monitors[MAX_MONITORS];
    int monitor_count;
    int selected_monitor;
    int pending_resolution;
    int pending_refresh;
    bool change_pending;
    int revert_countdown;
    int scroll_offset;
    int max_scroll;
    bool dragging_monitor;
} display_panel_state_t;

static display_panel_state_t g_state = {0};

static void draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    while (*text) {
        const uint8_t *glyph = font_get_glyph(*text);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) fb_put_pixel(x + col, y + row, color);
                }
            }
        }
        x += FONT_WIDTH;
        text++;
    }
}

static void draw_button(int32_t x, int32_t y, int32_t w, int32_t h,
                       const char *text, bool pressed, bool enabled) {
    uint32_t bg = !enabled ? 0xA0A0A0 : (pressed ? 0x808080 : 0xC0C0C0);
    fb_fill_rect(x, y, w, h, bg);
    fb_draw_rect(x, y, w, h, !enabled ? 0x808080 : THEME_MENU_TEXT_DISABLED);
    int tx = x + (w - strlen(text) * FONT_WIDTH) / 2;
    int ty = y + (h - FONT_HEIGHT) / 2;
    draw_text(tx, ty, text, !enabled ? THEME_MENU_TEXT_DISABLED : THEME_LABEL_TEXT);
}

static void draw_list_item(int32_t x, int32_t y, int32_t w,
                          const char *text, bool selected, bool current) {
    uint32_t bg = selected ? 0x4A90C2 : (current ? 0xE0E8F0 : THEME_TEXTBOX_BG);
    fb_fill_rect(x, y, w, DISPLAY_LINE_HEIGHT, bg);
    draw_text(x + 8, y + 4, text, selected ? THEME_TEXTBOX_BG : THEME_LABEL_TEXT);
    if (current && !selected) draw_text(x + w - 20, y + 4, "*", 0x008800);
}

static void draw_radio(int32_t x, int32_t y, const char *label, bool selected) {
    fb_draw_rect(x, y, 14, 14, THEME_MENU_TEXT_DISABLED);
    fb_fill_rect(x + 1, y + 1, 12, 12, THEME_TEXTBOX_BG);
    if (selected) fb_fill_rect(x + 4, y + 4, 6, 6, 0x4A90C2);
    draw_text(x + 20, y + 1, label, THEME_LABEL_TEXT);
}

static void draw_monitor_arrangement(int32_t x, int32_t y, int32_t w, int32_t h) {
    fb_fill_rect(x, y, w, h, THEME_BUTTON_BG);
    fb_draw_rect(x, y, w, h, 0xA0A0A0);
    if (g_state.monitor_count == 0) {
        draw_text(x + 20, y + h / 2 - 8, "No monitors detected", 0x808080);
        return;
    }
    for (int i = 0; i < g_state.monitor_count; i++) {
        monitor_info_t *mon = &g_state.monitors[i];
        int mx = x + 20 + i * 100;
        int my = y + 30;
        int mw = 80, mh = 50;
        uint32_t bg = (i == g_state.selected_monitor) ? 0x4A90C2 : 0x6090B0;
        fb_fill_rect(mx, my, mw, mh, bg);
        fb_draw_rect(mx, my, mw, mh, mon->is_primary ? 0x008800 : 0x404040);
        char num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        draw_text(mx + 35, my + 18, num, THEME_TEXTBOX_BG);
    }
}

static int draw_resolution_section(int32_t x, int32_t y, int32_t w) {
    int32_t cy = y;
    draw_text(x, cy, "Resolution", 0x0066CC);
    cy += DISPLAY_LINE_HEIGHT + 8;

    uint32_t curr_w = fb_get_width();
    uint32_t curr_h = fb_get_height();
    int current_idx = -1;
    for (int i = 0; g_resolutions[i].label; i++) {
        if (g_resolutions[i].width == curr_w && g_resolutions[i].height == curr_h) {
            current_idx = i;
            break;
        }
    }

    int list_w = w - 120;
    fb_draw_rect(x, cy, list_w, 8 * DISPLAY_LINE_HEIGHT + 2, 0x808080);
    for (int i = 0; i < 8 && g_resolutions[i].label; i++) {
        draw_list_item(x + 1, cy + 1 + i * DISPLAY_LINE_HEIGHT, list_w - 2,
                      g_resolutions[i].label,
                      i == g_state.pending_resolution,
                      i == current_idx);
    }
    draw_button(x + w - 100, cy, 90, DISPLAY_BUTTON_HEIGHT, "Apply",
               false, g_state.pending_resolution != current_idx);
    cy += 8 * DISPLAY_LINE_HEIGHT + 15;

    if (g_state.change_pending) {
        char timer_str[64];
        snprintf(timer_str, sizeof(timer_str),
                "Reverting in %d seconds...", g_state.revert_countdown);
        draw_text(x, cy, timer_str, 0xCC0000);
        cy += DISPLAY_LINE_HEIGHT + 8;
        draw_button(x, cy, 100, DISPLAY_BUTTON_HEIGHT, "Keep", false, true);
        draw_button(x + 110, cy, 100, DISPLAY_BUTTON_HEIGHT, "Revert", false, true);
        cy += DISPLAY_BUTTON_HEIGHT;
    }
    return cy - y;
}

static int draw_refresh_section(int32_t x, int32_t y, int32_t w) {
    int32_t cy = y;
    draw_text(x, cy, "Refresh Rate", 0x0066CC);
    cy += DISPLAY_LINE_HEIGHT + 8;
    int col = 0;
    for (int i = 0; g_refresh_rates[i] != 0; i++) {
        char rate_str[16];
        snprintf(rate_str, sizeof(rate_str), "%d Hz", g_refresh_rates[i]);
        draw_radio(x + (col % 3) * 80, cy + (col / 3) * DISPLAY_LINE_HEIGHT,
                  rate_str, i == g_state.pending_refresh);
        col++;
    }
    return cy - y + (col / 3 + 1) * DISPLAY_LINE_HEIGHT;
}

static int draw_display_info(int32_t x, int32_t y, int32_t w) {
    int32_t cy = y;
    draw_text(x, cy, "Display Information", 0x0066CC);
    cy += DISPLAY_LINE_HEIGHT + 8;
    char info[64];
    snprintf(info, sizeof(info), "Current: %u x %u", fb_get_width(), fb_get_height());
    draw_text(x, cy, info, THEME_LABEL_TEXT);
    cy += DISPLAY_LINE_HEIGHT;
    if (g_boot_info) {
        snprintf(info, sizeof(info), "Color Depth: %u bits", g_boot_info->framebuffer.bpp);
        draw_text(x, cy, info, THEME_LABEL_TEXT);
    }
    return cy - y + DISPLAY_LINE_HEIGHT;
}

// Framework callbacks
static void display_panel_init_internal(settings_panel_t *panel) { display_panel_init(); }
static void display_panel_draw_internal(settings_panel_t *panel, int32_t x, int32_t y, int32_t w, int32_t h) {
    display_panel_draw(x, y, w, h);
}
static void display_panel_event_internal(settings_panel_t *panel, gui_event_t *event) {
    if (event->type == EVENT_MOUSE_UP) {
        display_panel_handle_click(panel->content_x, panel->content_y,
                                   panel->content_width, panel->content_height_visible,
                                   event->mouse_x, event->mouse_y);
    } else if (event->type == EVENT_MOUSE_SCROLL) {
        display_panel_handle_scroll(event->scroll_delta);
    }
}
static void display_panel_apply_internal(settings_panel_t *panel) {
    display_panel_apply_resolution();
}
static void display_panel_cleanup_internal(settings_panel_t *panel) { display_panel_cleanup(); }

static settings_panel_def_t g_display_panel_def = {
    .name = "Display",
    .icon = "monitor",
    .category = SETTINGS_CAT_APPEARANCE,
    .priority = 30,
    .init = display_panel_init_internal,
    .draw = display_panel_draw_internal,
    .handle_event = display_panel_event_internal,
    .apply = display_panel_apply_internal,
    .cleanup = display_panel_cleanup_internal
};

void display_panel_init(void) {
    if (g_state.initialized) return;
    g_state.monitor_count = 1;
    g_state.selected_monitor = 0;
    monitor_info_t *mon = &g_state.monitors[0];
    mon->id = 0;
    snprintf(mon->name, sizeof(mon->name), "Display 1");
    mon->width = fb_get_width();
    mon->height = fb_get_height();
    mon->pos_x = mon->pos_y = 0;
    mon->is_primary = true;
    mon->is_connected = true;
    mon->current_resolution = 0;
    for (int i = 0; g_resolutions[i].label; i++) {
        if (g_resolutions[i].width == mon->width && g_resolutions[i].height == mon->height) {
            mon->current_resolution = i;
            break;
        }
    }
    mon->current_refresh = 2;
    g_state.pending_resolution = mon->current_resolution;
    g_state.pending_refresh = mon->current_refresh;
    g_state.change_pending = false;
    g_state.initialized = true;
    kprintf("[DisplayPanel] Initialized\n");
}

void display_panel_draw(int32_t panel_x, int32_t panel_y, int32_t panel_width, int32_t panel_height) {
    if (!g_state.initialized) display_panel_init();
    int32_t cy = panel_y + DISPLAY_PANEL_PADDING;
    int32_t cx = panel_x + DISPLAY_PANEL_PADDING;
    int32_t cw = panel_width - DISPLAY_PANEL_PADDING * 2;

    if (g_state.monitor_count > 1) {
        draw_text(cx, cy, "Monitor Arrangement", 0x0066CC);
        cy += DISPLAY_LINE_HEIGHT + 8;
        draw_monitor_arrangement(cx, cy, cw, 100);
        cy += 100 + DISPLAY_SECTION_SPACING;
    }
    cy += draw_resolution_section(cx, cy, cw);
    cy += DISPLAY_SECTION_SPACING;
    cy += draw_refresh_section(cx, cy, cw);
    cy += DISPLAY_SECTION_SPACING;
    cy += draw_display_info(cx, cy, cw);
    g_state.max_scroll = (cy - panel_y > panel_height) ? (cy - panel_y - panel_height) : 0;
}

bool display_panel_handle_click(int32_t panel_x, int32_t panel_y,
                                int32_t panel_width, int32_t panel_height,
                                int32_t click_x, int32_t click_y) {
    int32_t local_x = click_x - panel_x;
    int32_t local_y = click_y - panel_y;
    int32_t cy = DISPLAY_PANEL_PADDING;
    if (g_state.monitor_count > 1) cy += DISPLAY_LINE_HEIGHT + 8 + 100 + DISPLAY_SECTION_SPACING;
    int32_t res_y = cy + DISPLAY_LINE_HEIGHT + 8;
    int list_w = panel_width - DISPLAY_PANEL_PADDING * 2 - 120;
    if (local_x >= DISPLAY_PANEL_PADDING && local_x < DISPLAY_PANEL_PADDING + list_w &&
        local_y >= res_y && local_y < res_y + 8 * DISPLAY_LINE_HEIGHT) {
        int item = (local_y - res_y) / DISPLAY_LINE_HEIGHT;
        if (item >= 0 && item < 8 && g_resolutions[item].label) {
            g_state.pending_resolution = item;
            return true;
        }
    }
    int btn_x = panel_width - DISPLAY_PANEL_PADDING - 100;
    if (local_x >= btn_x && local_x < btn_x + 90 &&
        local_y >= res_y && local_y < res_y + DISPLAY_BUTTON_HEIGHT) {
        display_panel_apply_resolution();
        return true;
    }
    return false;
}

void display_panel_handle_mouse_move(int32_t panel_x, int32_t panel_y,
                                     int32_t panel_width, int32_t panel_height,
                                     int32_t mouse_x, int32_t mouse_y) {}

void display_panel_handle_scroll(int32_t delta) {
    g_state.scroll_offset -= delta * 20;
    if (g_state.scroll_offset < 0) g_state.scroll_offset = 0;
    if (g_state.scroll_offset > g_state.max_scroll) g_state.scroll_offset = g_state.max_scroll;
}

void display_panel_apply_resolution(void) {
    kprintf("[DisplayPanel] Applying resolution: %ux%u\n",
           g_resolutions[g_state.pending_resolution].width,
           g_resolutions[g_state.pending_resolution].height);
    g_state.change_pending = true;
    g_state.revert_countdown = RESOLUTION_REVERT_TIMEOUT;
}

void display_panel_confirm_change(void) {
    kprintf("[DisplayPanel] Resolution change confirmed\n");
    g_state.monitors[g_state.selected_monitor].current_resolution = g_state.pending_resolution;
    g_state.change_pending = false;
}

void display_panel_revert_change(void) {
    kprintf("[DisplayPanel] Reverting resolution\n");
    g_state.pending_resolution = g_state.monitors[g_state.selected_monitor].current_resolution;
    g_state.change_pending = false;
}

void display_panel_update_timer(void) {
    if (!g_state.change_pending) return;
    if (g_state.revert_countdown > 0) {
        g_state.revert_countdown--;
        if (g_state.revert_countdown == 0) display_panel_revert_change();
    }
}

bool display_panel_is_change_pending(void) { return g_state.change_pending; }
int display_panel_get_revert_countdown(void) { return g_state.revert_countdown; }
int display_get_selected_resolution(void) { return g_state.pending_resolution; }
void display_get_current_resolution(uint32_t *w, uint32_t *h) {
    *w = fb_get_width(); *h = fb_get_height();
}
int display_get_selected_refresh_rate(void) { return g_state.pending_refresh; }
int display_get_monitor_count(void) { return g_state.monitor_count; }
int display_get_primary_monitor(void) {
    for (int i = 0; i < g_state.monitor_count; i++)
        if (g_state.monitors[i].is_primary) return i;
    return 0;
}
void display_panel_cleanup(void) { g_state.initialized = false; }

int display_panel_register(void) { return settings_register_panel(&g_display_panel_def); }
const settings_panel_def_t *display_panel_get_def(void) { return &g_display_panel_def; }
