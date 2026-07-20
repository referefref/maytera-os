// Suppress warnings
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
// Suppress warnings
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
// panel_datetime.c - Date & Time Panel for MayteraOS Unified Settings
// Provides time/date configuration, timezone selection, and display format options

#include "settings_panel.h"
#include "settings_widgets.h"
#include "../window.h"
#include "../desktop.h"
#include "../clock.h"
#include "../../types.h"
#include "../../serial.h"
#include "../../mm/heap.h"
#include "../../string.h"
#include "../../video/framebuffer.h"
#include "../../video/font.h"
#include "../themes.h"

// External timer
extern volatile uint64_t timer_ticks;

// ============================================================================
// Date/Time Structures
// ============================================================================

typedef enum {
    DATE_FORMAT_MDY = 0,  // MM/DD/YYYY
    DATE_FORMAT_DMY,      // DD/MM/YYYY
    DATE_FORMAT_YMD,      // YYYY-MM-DD
    DATE_FORMAT_LONG      // Month Day, Year
} date_format_t;

typedef enum {
    TIME_FORMAT_12H = 0,
    TIME_FORMAT_24H
} time_format_t;

typedef struct {
    const char *name;
    int offset_hours;
    int offset_minutes;
} timezone_info_t;

// Panel data
typedef struct {
    int hour, minute, second;
    int day, month, year;
    int weekday;

    int timezone_index;
    date_format_t date_format;
    time_format_t time_format;
    int first_day;  // 0=Sunday, 1=Monday

    bool show_seconds;
    bool show_date_taskbar;
    bool show_weekday;
    bool sync_ntp;
    bool auto_timezone;

    int sub_section;  // 0=Time, 1=Timezone, 2=Format
    uint64_t last_tick;
} datetime_panel_data_t;

// Timezone definitions
static const timezone_info_t g_timezones[] = {
    { "UTC",                  0,  0 },
    { "Pacific Time (US)",   -8,  0 },
    { "Mountain Time (US)",  -7,  0 },
    { "Central Time (US)",   -6,  0 },
    { "Eastern Time (US)",   -5,  0 },
    { "London",               0,  0 },
    { "Paris/Berlin",         1,  0 },
    { "Moscow",               3,  0 },
    { "Dubai",                4,  0 },
    { "India",                5, 30 },
    { "Singapore",            8,  0 },
    { "Tokyo",                9,  0 },
    { "Sydney",              10,  0 },
};
#define NUM_TIMEZONES 13

static const char *month_names[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const char *day_names[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

// ============================================================================
// Layout Constants
// ============================================================================

#define SECTION_TAB_WIDTH   70
#define SECTION_TAB_HEIGHT  24
#define TZ_ITEM_HEIGHT      22
#define TZ_LIST_HEIGHT      140
#define CLOCK_RADIUS        45

// Sine table for clock hands
static const int sin_table[60] = {
       0,  105,  208,  309,  407,  500,  588,  669,  743,  809,
     866,  914,  951,  978,  995, 1000,  995,  978,  951,  914,
     866,  809,  743,  669,  588,  500,  407,  309,  208,  105,
       0, -105, -208, -309, -407, -500, -588, -669, -743, -809,
    -866, -914, -951, -978, -995,-1000, -995, -978, -951, -914,
    -866, -809, -743, -669, -588, -500, -407, -309, -208, -105
};

static int isin(int a) { while (a < 0) a += 60; while (a >= 60) a -= 60; return sin_table[a]; }
static int icos(int a) { return isin(a + 15); }

// ============================================================================
// Helper Functions
// ============================================================================

static void draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    while (*text) {
        const uint8_t *glyph = font_get_glyph(*text);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(x + col, y + row, color);
                    }
                }
            }
        }
        x += FONT_WIDTH;
        text++;
    }
}

static void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
    int dx = x1 - x0; int dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1; int sy = dy > 0 ? 1 : -1;
    if (dx < 0) dx = -dx; if (dy < 0) dy = -dy;
    int err = (dx > dy ? dx : -dy) / 2;
    while (1) {
        fb_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 < dy) { err += dx; y0 += sy; }
    }
}

static void draw_thick_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color, int t) {
    for (int i = -t/2; i <= t/2; i++) {
        draw_line(x0 + i, y0, x1 + i, y1, color);
        draw_line(x0, y0 + i, x1, y1 + i, color);
    }
}

static void draw_analog_clock(int32_t cx, int32_t cy, int32_t radius, datetime_panel_data_t *data) {
    // Face
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy <= radius * radius) {
                fb_put_pixel(cx + dx, cy + dy, THEME_TEXTBOX_BG);
            }
        }
    }

    // Border
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int d = dx * dx + dy * dy;
            if (d <= radius * radius && d > (radius - 3) * (radius - 3)) {
                fb_put_pixel(cx + dx, cy + dy, 0x4A90C2);
            }
        }
    }

    // Hour markers
    for (int i = 0; i < 12; i++) {
        int a = i * 5;
        int r1 = radius - 8, r2 = radius - 3;
        int x1 = cx + (isin(a) * r1) / 1000;
        int y1 = cy - (icos(a) * r1) / 1000;
        int x2 = cx + (isin(a) * r2) / 1000;
        int y2 = cy - (icos(a) * r2) / 1000;
        draw_thick_line(x1, y1, x2, y2, THEME_LABEL_TEXT, 2);
    }

    // Hour hand
    int ha = ((data->hour % 12) * 5 + data->minute / 12);
    int hlen = radius * 50 / 100;
    draw_thick_line(cx, cy, cx + (isin(ha) * hlen) / 1000, cy - (icos(ha) * hlen) / 1000, THEME_LABEL_TEXT, 3);

    // Minute hand
    int mlen = radius * 70 / 100;
    draw_thick_line(cx, cy, cx + (isin(data->minute) * mlen) / 1000,
                    cy - (icos(data->minute) * mlen) / 1000, 0x4A90C2, 2);

    // Second hand
    if (data->show_seconds) {
        int slen = radius * 80 / 100;
        draw_line(cx, cy, cx + (isin(data->second) * slen) / 1000,
                  cy - (icos(data->second) * slen) / 1000, 0xE74C3C);
    }

    // Center
    for (int dy = -3; dy <= 3; dy++) {
        for (int dx = -3; dx <= 3; dx++) {
            if (dx * dx + dy * dy <= 9) fb_put_pixel(cx + dx, cy + dy, 0x2ECC71);
        }
    }
}

static void draw_section_tab(int32_t x, int32_t y, const char *text, bool active) {
    uint32_t bg = active ? 0x4A90C2 : 0xE0E0E0;
    uint32_t fg = active ? THEME_TEXTBOX_BG : THEME_LABEL_TEXT;
    fb_fill_rect(x, y, SECTION_TAB_WIDTH, SECTION_TAB_HEIGHT, bg);
    int len = strlen(text);
    draw_text(x + (SECTION_TAB_WIDTH - len * FONT_WIDTH) / 2,
              y + (SECTION_TAB_HEIGHT - FONT_HEIGHT) / 2, text, fg);
}

// ============================================================================
// Panel Lifecycle
// ============================================================================

static void datetime_panel_init(settings_panel_t *panel) {
    kprintf("[DateTime Panel] Initializing...\n");

    datetime_panel_data_t *data = (datetime_panel_data_t *)kzalloc(sizeof(datetime_panel_data_t));
    if (!data) return;

    rtc_read_time(&data->hour, &data->minute, &data->second);
    rtc_read_date(&data->day, &data->month, &data->year, &data->weekday);

    data->timezone_index = 0;
    data->date_format = DATE_FORMAT_YMD;
    data->time_format = TIME_FORMAT_24H;
    data->first_day = 1;  // Monday

    data->show_seconds = true;
    data->show_date_taskbar = true;
    data->show_weekday = false;
    data->sync_ntp = false;
    data->auto_timezone = false;
    data->sub_section = 0;
    data->last_tick = timer_ticks;

    panel->user_data = data;
    kprintf("[DateTime Panel] Initialized, time: %02d:%02d:%02d\n",
            data->hour, data->minute, data->second);
}

static void datetime_update(datetime_panel_data_t *data) {
    if (timer_ticks - data->last_tick < 100) return;
    data->last_tick = timer_ticks;
    rtc_read_time(&data->hour, &data->minute, &data->second);
    rtc_read_date(&data->day, &data->month, &data->year, &data->weekday);
}

static void datetime_panel_draw(settings_panel_t *panel, int32_t x, int32_t y,
                                int32_t width, int32_t height) {
    datetime_panel_data_t *data = (datetime_panel_data_t *)panel->user_data;
    if (!data) return;

    datetime_update(data);

    int32_t cx = x;
    int32_t cy = y;

    // Tabs
    draw_section_tab(cx, cy, "Time", data->sub_section == 0);
    draw_section_tab(cx + SECTION_TAB_WIDTH + 2, cy, "Zone", data->sub_section == 1);
    draw_section_tab(cx + 2 * (SECTION_TAB_WIDTH + 2), cy, "Format", data->sub_section == 2);

    cy += SECTION_TAB_HEIGHT + 16;

    if (data->sub_section == 0) {
        // Time section
        draw_text(cx, cy, "Current Time", 0x4A90C2);
        cy += CONTENT_LINE_HEIGHT + 8;

        // Analog clock
        draw_analog_clock(cx + CLOCK_RADIUS + 10, cy + CLOCK_RADIUS, CLOCK_RADIUS, data);

        // Digital display
        int32_t dig_x = cx + 2 * CLOCK_RADIUS + 40;
        fb_fill_rect(dig_x, cy + 20, 140, 36, 0x1A1A1A);
        fb_draw_rect(dig_x, cy + 20, 140, 36, THEME_LABEL_TEXT);

        char time_str[32];
        if (data->time_format == TIME_FORMAT_12H) {
            int h = data->hour % 12; if (h == 0) h = 12;
            const char *ap = data->hour >= 12 ? "PM" : "AM";
            if (data->show_seconds)
                snprintf(time_str, sizeof(time_str), "%2d:%02d:%02d %s", h, data->minute, data->second, ap);
            else
                snprintf(time_str, sizeof(time_str), "%2d:%02d %s", h, data->minute, ap);
        } else {
            if (data->show_seconds)
                snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", data->hour, data->minute, data->second);
            else
                snprintf(time_str, sizeof(time_str), "%02d:%02d", data->hour, data->minute);
        }
        int tlen = strlen(time_str);
        draw_text(dig_x + (140 - tlen * FONT_WIDTH) / 2, cy + 30, time_str, 0x2ECC71);

        // Date display
        char date_str[32];
        switch (data->date_format) {
            case DATE_FORMAT_MDY:
                snprintf(date_str, sizeof(date_str), "%02d/%02d/%04d", data->month, data->day, data->year);
                break;
            case DATE_FORMAT_DMY:
                snprintf(date_str, sizeof(date_str), "%02d/%02d/%04d", data->day, data->month, data->year);
                break;
            case DATE_FORMAT_YMD:
                snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", data->year, data->month, data->day);
                break;
            case DATE_FORMAT_LONG:
                snprintf(date_str, sizeof(date_str), "%s %d, %d",
                         month_names[data->month - 1], data->day, data->year);
                break;
        }
        draw_text(dig_x, cy + 66, date_str, THEME_LABEL_TEXT);

        if (data->weekday >= 0 && data->weekday < 7) {
            draw_text(dig_x, cy + 82, day_names[data->weekday], THEME_MENU_TEXT_DISABLED);
        }

        cy += 2 * CLOCK_RADIUS + 20;

        // Buttons
        fb_fill_rect(cx, cy, 100, 24, 0x4A90C2);
        draw_text(cx + 8, cy + 4, "Set Manually", THEME_TEXTBOX_BG);

        cy += 36;

        sw_draw_checkbox(cx, cy, data->sync_ntp, "Sync with NTP server");

    } else if (data->sub_section == 1) {
        // Timezone section
        draw_text(cx, cy, "Timezone", 0x4A90C2);
        cy += CONTENT_LINE_HEIGHT + 8;

        sw_draw_checkbox(cx, cy, data->auto_timezone, "Detect timezone automatically");
        cy += 28;

        draw_text(cx, cy, "Select timezone:", THEME_LABEL_TEXT);
        cy += CONTENT_LINE_HEIGHT;

        fb_fill_rect(cx, cy, 280, TZ_LIST_HEIGHT, THEME_TEXTBOX_BG);
        fb_draw_rect(cx, cy, 280, TZ_LIST_HEIGHT, 0xCCCCCC);

        int32_t list_y = cy + 2;
        for (int i = 0; i < NUM_TIMEZONES && (i + 1) * TZ_ITEM_HEIGHT < TZ_LIST_HEIGHT; i++) {
            bool selected = (i == data->timezone_index);
            if (selected) {
                fb_fill_rect(cx + 2, list_y, 276, TZ_ITEM_HEIGHT - 2, 0x4A90C2);
            }

            uint32_t fg = selected ? THEME_TEXTBOX_BG : THEME_LABEL_TEXT;
            uint32_t fg2 = selected ? 0xCCCCCC : THEME_MENU_TEXT_DISABLED;

            draw_text(cx + 8, list_y + 3, g_timezones[i].name, fg);

            char offset[16];
            if (g_timezones[i].offset_minutes == 0)
                snprintf(offset, sizeof(offset), "UTC%+d", g_timezones[i].offset_hours);
            else
                snprintf(offset, sizeof(offset), "UTC%+d:%02d",
                         g_timezones[i].offset_hours, g_timezones[i].offset_minutes);
            draw_text(cx + 200, list_y + 3, offset, fg2);

            list_y += TZ_ITEM_HEIGHT;
        }

        cy += TZ_LIST_HEIGHT + 12;

        char info[64];
        snprintf(info, sizeof(info), "Selected: %s", g_timezones[data->timezone_index].name);
        draw_text(cx, cy, info, THEME_LABEL_TEXT);

    } else {
        // Format section
        draw_text(cx, cy, "Date & Time Format", 0x4A90C2);
        cy += CONTENT_LINE_HEIGHT + 12;

        // Time format
        draw_text(cx, cy, "Time Format:", THEME_LABEL_TEXT);
        cy += CONTENT_LINE_HEIGHT;

        sw_draw_radio(cx, cy, data->time_format == TIME_FORMAT_12H, "12-hour (AM/PM)");
        cy += 22;
        sw_draw_radio(cx, cy, data->time_format == TIME_FORMAT_24H, "24-hour");
        cy += 28;

        // Date format
        draw_text(cx, cy, "Date Format:", THEME_LABEL_TEXT);
        cy += CONTENT_LINE_HEIGHT;

        sw_draw_radio(cx, cy, data->date_format == DATE_FORMAT_MDY, "MM/DD/YYYY (US)");
        cy += 22;
        sw_draw_radio(cx, cy, data->date_format == DATE_FORMAT_DMY, "DD/MM/YYYY (EU)");
        cy += 22;
        sw_draw_radio(cx, cy, data->date_format == DATE_FORMAT_YMD, "YYYY-MM-DD (ISO)");
        cy += 22;
        sw_draw_radio(cx, cy, data->date_format == DATE_FORMAT_LONG, "Long format");
        cy += 28;

        // First day of week
        draw_text(cx, cy, "First Day of Week:", THEME_LABEL_TEXT);
        cy += CONTENT_LINE_HEIGHT;

        sw_draw_radio(cx, cy, data->first_day == 0, "Sunday");
        sw_draw_radio(cx + 100, cy, data->first_day == 1, "Monday");
        cy += 28;

        // Clock options
        draw_text(cx, cy, "Clock Display:", THEME_LABEL_TEXT);
        cy += CONTENT_LINE_HEIGHT;

        sw_draw_checkbox(cx, cy, data->show_seconds, "Show seconds");
        cy += 22;
        sw_draw_checkbox(cx, cy, data->show_date_taskbar, "Show date in taskbar");
        cy += 22;
        sw_draw_checkbox(cx, cy, data->show_weekday, "Show day of week");
    }
}

static void datetime_panel_event(settings_panel_t *panel, gui_event_t *event) {
    datetime_panel_data_t *data = (datetime_panel_data_t *)panel->user_data;
    if (!data) return;

    if (event->type != EVENT_MOUSE_UP) return;

    int32_t mx = event->mouse_x;
    int32_t my = event->mouse_y;
    int32_t cx = panel->content_x;
    int32_t cy = panel->content_y;

    // Tab clicks
    if (my >= cy && my < cy + SECTION_TAB_HEIGHT) {
        for (int i = 0; i < 3; i++) {
            int32_t tx = cx + i * (SECTION_TAB_WIDTH + 2);
            if (mx >= tx && mx < tx + SECTION_TAB_WIDTH) {
                data->sub_section = i;
                settings_panel_mark_dirty(panel);
                return;
            }
        }
    }

    int32_t content_y = cy + SECTION_TAB_HEIGHT + 16;

    if (data->sub_section == 0) {
        // NTP checkbox
        int32_t ntp_y = content_y + 2 * CLOCK_RADIUS + 20 + 36;
        if (my >= ntp_y && my < ntp_y + 20 && mx >= cx && mx < cx + 200) {
            data->sync_ntp = !data->sync_ntp;
            settings_panel_mark_dirty(panel);
        }
    } else if (data->sub_section == 1) {
        // Auto timezone checkbox
        int32_t auto_y = content_y + CONTENT_LINE_HEIGHT + 8;
        if (my >= auto_y && my < auto_y + 20 && mx >= cx && mx < cx + 250) {
            data->auto_timezone = !data->auto_timezone;
            settings_panel_mark_dirty(panel);
            return;
        }

        // Timezone list
        int32_t list_y = auto_y + 28 + CONTENT_LINE_HEIGHT + 2;
        if (mx >= cx && mx < cx + 280 && my >= list_y && my < list_y + TZ_LIST_HEIGHT) {
            int idx = (my - list_y) / TZ_ITEM_HEIGHT;
            if (idx >= 0 && idx < NUM_TIMEZONES) {
                data->timezone_index = idx;
                kprintf("[DateTime Panel] Selected timezone: %s\n", g_timezones[idx].name);
                settings_panel_mark_dirty(panel);
            }
        }
    } else if (data->sub_section == 2) {
        int32_t ypos = content_y + CONTENT_LINE_HEIGHT + 12;

        // Time format radios
        ypos += CONTENT_LINE_HEIGHT;
        if (my >= ypos && my < ypos + 18 && mx >= cx && mx < cx + 150) {
            data->time_format = TIME_FORMAT_12H;
            settings_panel_mark_dirty(panel);
            return;
        }
        ypos += 22;
        if (my >= ypos && my < ypos + 18 && mx >= cx && mx < cx + 100) {
            data->time_format = TIME_FORMAT_24H;
            settings_panel_mark_dirty(panel);
            return;
        }
        ypos += 28;

        // Date format radios
        ypos += CONTENT_LINE_HEIGHT;
        for (int i = 0; i < 4; i++) {
            if (my >= ypos && my < ypos + 18 && mx >= cx && mx < cx + 180) {
                data->date_format = (date_format_t)i;
                settings_panel_mark_dirty(panel);
                return;
            }
            ypos += 22;
        }
        ypos += 6;

        // First day radios
        ypos += CONTENT_LINE_HEIGHT;
        if (my >= ypos && my < ypos + 18) {
            if (mx >= cx && mx < cx + 80) {
                data->first_day = 0;
                settings_panel_mark_dirty(panel);
                return;
            } else if (mx >= cx + 100 && mx < cx + 180) {
                data->first_day = 1;
                settings_panel_mark_dirty(panel);
                return;
            }
        }
        ypos += 28;

        // Clock display checkboxes
        ypos += CONTENT_LINE_HEIGHT;
        if (my >= ypos && my < ypos + 18 && mx >= cx && mx < cx + 150) {
            data->show_seconds = !data->show_seconds;
            settings_panel_mark_dirty(panel);
            return;
        }
        ypos += 22;
        if (my >= ypos && my < ypos + 18 && mx >= cx && mx < cx + 200) {
            data->show_date_taskbar = !data->show_date_taskbar;
            settings_panel_mark_dirty(panel);
            return;
        }
        ypos += 22;
        if (my >= ypos && my < ypos + 18 && mx >= cx && mx < cx + 180) {
            data->show_weekday = !data->show_weekday;
            settings_panel_mark_dirty(panel);
            return;
        }
    }
}

static void datetime_panel_apply(settings_panel_t *panel) {
    kprintf("[DateTime Panel] Applying changes...\n");
}

static void datetime_panel_cleanup(settings_panel_t *panel) {
    if (panel->user_data) {
        kfree(panel->user_data);
        panel->user_data = NULL;
    }
    kprintf("[DateTime Panel] Cleaned up\n");
}

// ============================================================================
// Panel Registration
// ============================================================================

static settings_panel_def_t datetime_panel_def = {
    .name = "Date & Time",
    .icon = "clock",
    .category = SETTINGS_CAT_SYSTEM,
    .priority = 40,
    .init = datetime_panel_init,
    .draw = datetime_panel_draw,
    .handle_event = datetime_panel_event,
    .apply = datetime_panel_apply,
    .cleanup = datetime_panel_cleanup
};

void datetime_panel_register(void) {
    settings_register_panel(&datetime_panel_def);
}
