// clock.c - Desktop Clock Widget for MayteraOS
#include "clock.h"
#include "window.h"
#include "icons.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../video/graphics.h"
#include "syslog.h"

// External timer ticks
extern volatile uint64_t timer_ticks;

// RTC I/O ports
#define RTC_INDEX_PORT  0x70
#define RTC_DATA_PORT   0x71

// RTC registers
#define RTC_SECONDS     0x00
#define RTC_MINUTES     0x02
#define RTC_HOURS       0x04
#define RTC_WEEKDAY     0x06
#define RTC_DAY         0x07
#define RTC_MONTH       0x08
#define RTC_YEAR        0x09
#define RTC_STATUS_A    0x0A
#define RTC_STATUS_B    0x0B

// Sine lookup table (scaled by 1000, 60 entries for 6-degree increments)
// sin(i * 6 degrees) * 1000, starting from 0 degrees
static const int sin_table[60] = {
       0,  105,  208,  309,  407,  500,  588,  669,  743,  809,
     866,  914,  951,  978,  995, 1000,  995,  978,  951,  914,
     866,  809,  743,  669,  588,  500,  407,  309,  208,  105,
       0, -105, -208, -309, -407, -500, -588, -669, -743, -809,
    -866, -914, -951, -978, -995,-1000, -995, -978, -951, -914,
    -866, -809, -743, -669, -588, -500, -407, -309, -208, -105
};

// Get sine value (angle in 6-degree units, 0-59)
static int isin(int angle) {
    while (angle < 0) angle += 60;
    while (angle >= 60) angle -= 60;
    return sin_table[angle];
}

// Get cosine value (angle in 6-degree units, 0-59)
static int icos(int angle) {
    return isin(angle + 15);  // cos(x) = sin(x + 90), 90/6 = 15
}

// BCD to binary conversion
static int bcd_to_bin(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

// Read RTC register
static uint8_t rtc_read(uint8_t reg) {
    outb(RTC_INDEX_PORT, reg);
    return inb(RTC_DATA_PORT);
}

// Check if RTC update in progress
static bool rtc_update_in_progress(void) {
    outb(RTC_INDEX_PORT, RTC_STATUS_A);
    return (inb(RTC_DATA_PORT) & 0x80) != 0;
}

// Read time from RTC
void rtc_read_time(int *hour, int *minute, int *second) {
    // Wait for update to finish
    while (rtc_update_in_progress());

    uint8_t sec = rtc_read(RTC_SECONDS);
    uint8_t min = rtc_read(RTC_MINUTES);
    uint8_t hr = rtc_read(RTC_HOURS);

    // Check if BCD mode (bit 2 of status B = 0 means BCD)
    uint8_t status_b = rtc_read(RTC_STATUS_B);
    bool is_bcd = !(status_b & 0x04);
    bool is_24hr = (status_b & 0x02);

    if (is_bcd) {
        *second = bcd_to_bin(sec);
        *minute = bcd_to_bin(min);
        *hour = bcd_to_bin(hr & 0x7F);
    } else {
        *second = sec;
        *minute = min;
        *hour = hr & 0x7F;
    }

    // Handle 12-hour mode
    if (!is_24hr && (hr & 0x80)) {
        *hour = (*hour % 12) + 12;  // PM
    }
}

// Read date from RTC
void rtc_read_date(int *day, int *month, int *year, int *weekday) {
    while (rtc_update_in_progress());

    uint8_t d = rtc_read(RTC_DAY);
    uint8_t m = rtc_read(RTC_MONTH);
    uint8_t y = rtc_read(RTC_YEAR);
    uint8_t w = rtc_read(RTC_WEEKDAY);

    uint8_t status_b = rtc_read(RTC_STATUS_B);
    bool is_bcd = !(status_b & 0x04);

    if (is_bcd) {
        *day = bcd_to_bin(d);
        *month = bcd_to_bin(m);
        *year = bcd_to_bin(y) + 2000;  // Assume 20xx
        *weekday = bcd_to_bin(w);
    } else {
        *day = d;
        *month = m;
        *year = y + 2000;
        *weekday = w;
    }
}

// Day names
static const char *day_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

// Month names
static const char *month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

// Create clock widget
clock_widget_t *clock_create(void) {
    clock_widget_t *clk = (clock_widget_t *)kmalloc(sizeof(clock_widget_t));
    if (!clk) return NULL;

    memset(clk, 0, sizeof(clock_widget_t));

    // Create small widget window
    int screen_w = fb_get_width();
    clk->window = window_create("Clock", screen_w - 160, 40, 150, 80);
    if (!clk->window) {
        kfree(clk);
        return NULL;
    }

    // Default settings
    clk->mode = CLOCK_DIGITAL;
    clk->show_seconds = true;
    clk->show_date = true;
    clk->show_ampm = false;  // 24-hour format by default
    clk->always_on_top = true;
    clk->last_update = 0;

    // Initial time read
    clock_update(clk);

    return clk;
}

// Destroy clock widget
void clock_destroy(clock_widget_t *clk) {
    if (!clk) return;
    if (clk->window) {
        window_destroy(clk->window);
    }
    kfree(clk);
}

// Update time from RTC
void clock_update(clock_widget_t *clk) {
    if (!clk) return;

    rtc_read_time(&clk->hour, &clk->minute, &clk->second);
    rtc_read_date(&clk->day, &clk->month, &clk->year, &clk->weekday);
    clk->last_update = timer_ticks;
}

// Toggle functions
void clock_toggle_mode(clock_widget_t *clk) {
    if (!clk) return;
    clk->mode = (clk->mode + 1) % 4;  // 4 modes now

    // Resize window based on mode
    switch (clk->mode) {
        case CLOCK_DIGITAL:
            window_resize(clk->window, 150, 80);
            break;
        case CLOCK_ANALOG:
        case CLOCK_BOTH:
            window_resize(clk->window, 150, 160);
            break;
        case CLOCK_CALENDAR:
            window_resize(clk->window, 200, 200);  // Larger for calendar
            break;
    }
}

void clock_toggle_seconds(clock_widget_t *clk) {
    if (clk) clk->show_seconds = !clk->show_seconds;
}

void clock_toggle_date(clock_widget_t *clk) {
    if (clk) clk->show_date = !clk->show_date;
}

void clock_toggle_ampm(clock_widget_t *clk) {
    if (clk) clk->show_ampm = !clk->show_ampm;
}

// Helper to format 2-digit number
static void format_2digit(char *buf, int val) {
    buf[0] = '0' + (val / 10);
    buf[1] = '0' + (val % 10);
}

// Draw digital clock
static void draw_digital_clock(clock_widget_t *clk, int x, int y, int w) {
    char time_str[16];
    int hour = clk->hour;
    const char *ampm = "";

    if (clk->show_ampm) {
        ampm = (hour >= 12) ? " PM" : " AM";
        hour = hour % 12;
        if (hour == 0) hour = 12;
    }

    // Build time string manually
    char *p = time_str;
    format_2digit(p, hour); p += 2;
    *p++ = ':';
    format_2digit(p, clk->minute); p += 2;
    if (clk->show_seconds) {
        *p++ = ':';
        format_2digit(p, clk->second); p += 2;
    }
    // Append AM/PM if needed
    const char *ap = ampm;
    while (*ap) *p++ = *ap++;
    *p = '\0';

    // Draw large time
    int text_w = strlen(time_str) * 16;  // Large font
    int text_x = x + (w - text_w) / 2;

    // Draw with 2x scale
    for (int i = 0; time_str[i]; i++) {
        const uint8_t *glyph = font_get_glyph(time_str[i]);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    // 2x scale
                    fb_fill_rect(text_x + i * 16 + col * 2, y + row * 2, 2, 2, 0xFFFFFF);
                }
            }
        }
    }
}

// Draw date
static void draw_date(clock_widget_t *clk, int x, int y, int w) {
    char date_str[32];
    const char *day_name = (clk->weekday >= 0 && clk->weekday < 7) ? day_names[clk->weekday] : "???";
    const char *month_name = (clk->month >= 1 && clk->month <= 12) ? month_names[clk->month - 1] : "???";

    // Build date string manually: "Day, Mon DD, YYYY"
    char *p = date_str;
    const char *s = day_name;
    while (*s) *p++ = *s++;
    *p++ = ','; *p++ = ' ';
    s = month_name;
    while (*s) *p++ = *s++;
    *p++ = ' ';
    // Day (1-31)
    if (clk->day >= 10) *p++ = '0' + (clk->day / 10);
    *p++ = '0' + (clk->day % 10);
    *p++ = ','; *p++ = ' ';
    // Year (4 digits)
    int yr = clk->year;
    *p++ = '0' + (yr / 1000); yr %= 1000;
    *p++ = '0' + (yr / 100); yr %= 100;
    *p++ = '0' + (yr / 10); yr %= 10;
    *p++ = '0' + yr;
    *p = '\0';

    int text_w = strlen(date_str) * 8;
    int text_x = x + (w - text_w) / 2;

    for (int i = 0; date_str[i]; i++) {
        const uint8_t *glyph = font_get_glyph(date_str[i]);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    fb_put_pixel(text_x + i * 8 + col, y + row, 0xC0C0C0);
                }
            }
        }
    }
}

// Draw analog clock using integer math
// Angles are in 6-degree units (0-59 for 360 degrees)
// 12 o'clock = angle 0, but we subtract 15 to start at top (270 degrees = -90 degrees)
static void draw_analog_clock(clock_widget_t *clk, int cx, int cy, int radius) {
    // Draw clock face
    gfx_draw_circle(cx, cy, radius, 0xFFFFFF);
    gfx_draw_circle(cx, cy, radius - 1, 0xC0C0C0);

    // Draw hour markers (12 marks at 30-degree intervals = 5 units each)
    for (int i = 0; i < 12; i++) {
        int angle = i * 5 - 15;  // -15 to start at 12 o'clock (top)
        int c = icos(angle);
        int s = isin(angle);
        int x1 = cx + ((radius - 5) * c) / 1000;
        int y1 = cy + ((radius - 5) * s) / 1000;
        int x2 = cx + ((radius - 10) * c) / 1000;
        int y2 = cy + ((radius - 10) * s) / 1000;
        fb_draw_line(x1, y1, x2, y2, 0xFFFFFF);
    }

    // Second hand angle: second * 1 unit (60 units per full rotation)
    int sec_angle = clk->second - 15;

    // Minute hand angle: minute * 1 unit
    int min_angle = clk->minute - 15;

    // Hour hand angle: (hour * 5) + (minute / 12) for smooth movement
    int hr_angle = ((clk->hour % 12) * 5) + (clk->minute / 12) - 15;

    // Draw hour hand (short, thick)
    int hx = cx + (radius / 2 * icos(hr_angle)) / 1000;
    int hy = cy + (radius / 2 * isin(hr_angle)) / 1000;
    fb_draw_line(cx, cy, hx, hy, 0xFFFFFF);
    fb_draw_line(cx + 1, cy, hx + 1, hy, 0xFFFFFF);

    // Draw minute hand (long)
    int mx = cx + ((radius * 3 / 4) * icos(min_angle)) / 1000;
    int my = cy + ((radius * 3 / 4) * isin(min_angle)) / 1000;
    fb_draw_line(cx, cy, mx, my, 0xFFFFFF);

    // Draw second hand (thin, red)
    if (clk->show_seconds) {
        int sx = cx + ((radius * 4 / 5) * icos(sec_angle)) / 1000;
        int sy = cy + ((radius * 4 / 5) * isin(sec_angle)) / 1000;
        fb_draw_line(cx, cy, sx, sy, 0xFF4040);
    }

    // Center dot
    gfx_fill_circle(cx, cy, 3, 0xFFFFFF);
}

// Helper: Get number of days in a month
static int days_in_month(int month, int year) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    int d = days[month - 1];
    // Leap year check for February
    if (month == 2) {
        if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
            d = 29;
        }
    }
    return d;
}

// Helper: Get day of week for first of month (0=Sun, 1=Mon, ...)
// Using Zeller's congruence modified for Gregorian calendar
static int first_day_of_month(int month, int year) {
    if (month < 3) {
        month += 12;
        year--;
    }
    int k = year % 100;
    int j = year / 100;
    int q = 1;  // First day
    int h = (q + (13 * (month + 1)) / 5 + k + k / 4 + j / 4 - 2 * j) % 7;
    // Adjust: Zeller's gives 0=Sat, but we want 0=Sun
    h = (h + 6) % 7;
    return h;
}

// Draw helper text (small font)
static void cal_draw_text(const char *text, int x, int y, uint32_t color) {
    for (int i = 0; text[i]; i++) {
        const uint8_t *glyph = font_get_glyph(text[i]);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    fb_put_pixel(x + i * 8 + col, y + row, color);
                }
            }
        }
    }
}

// Draw calendar
static void draw_calendar(clock_widget_t *clk, int x, int y, int w, int h __attribute__((unused))) {
    int cell_w = w / 7;
    int cell_h = 18;

    // Header: Month Year
    char header[32];
    const char *month_name = (clk->month >= 1 && clk->month <= 12) ?
                             month_names[clk->month - 1] : "???";
    char *p = header;
    const char *s = month_name;
    while (*s) *p++ = *s++;
    *p++ = ' ';
    int yr = clk->year;
    *p++ = '0' + (yr / 1000); yr %= 1000;
    *p++ = '0' + (yr / 100); yr %= 100;
    *p++ = '0' + (yr / 10); yr %= 10;
    *p++ = '0' + yr;
    *p = '\0';

    int header_w = strlen(header) * 8;
    cal_draw_text(header, x + (w - header_w) / 2, y, 0xFFFFFF);
    y += 20;

    // Day names header
    const char *day_abbrs[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    for (int i = 0; i < 7; i++) {
        int dx = x + i * cell_w + (cell_w - 16) / 2;
        cal_draw_text(day_abbrs[i], dx, y, 0x8080C0);
    }
    y += 18;

    // Calculate first day of month and number of days
    int first_day = first_day_of_month(clk->month, clk->year);
    int num_days = days_in_month(clk->month, clk->year);

    // Draw days
    int day = 1;
    for (int row = 0; row < 6 && day <= num_days; row++) {
        for (int col = 0; col < 7 && day <= num_days; col++) {
            if (row == 0 && col < first_day) {
                continue;  // Skip cells before first day
            }

            int dx = x + col * cell_w + (cell_w - 16) / 2;
            int dy = y + row * cell_h;

            // Highlight current day
            bool is_today = (day == clk->day);
            if (is_today) {
                fb_fill_rect(dx - 2, dy - 1, 20, 16, 0x4060A0);
            }

            // Draw day number
            char day_str[4];
            if (day >= 10) {
                day_str[0] = '0' + (day / 10);
                day_str[1] = '0' + (day % 10);
                day_str[2] = '\0';
            } else {
                day_str[0] = '0' + day;
                day_str[1] = '\0';
            }
            cal_draw_text(day_str, dx, dy, is_today ? 0xFFFFFF : 0xC0C0C0);

            day++;
        }
    }
}

// Event handling
void clock_handle_event(clock_widget_t *clk, gui_event_t *event) {
    if (!clk || !event) return;

    if (event->type == EVENT_KEY_DOWN) {
        switch (event->keycode) {
            case 'm':
            case 'M':
                clock_toggle_mode(clk);
                break;
            case 's':
            case 'S':
                clock_toggle_seconds(clk);
                break;
            case 'd':
            case 'D':
                clock_toggle_date(clk);
                break;
            case 'a':
            case 'A':
                clock_toggle_ampm(clk);
                break;
            case 'c':
            case 'C':
                // Direct toggle to calendar mode
                if (clk->mode == CLOCK_CALENDAR) {
                    clk->mode = CLOCK_DIGITAL;
                    window_resize(clk->window, 150, 80);
                } else {
                    clk->mode = CLOCK_CALENDAR;
                    window_resize(clk->window, 200, 200);
                }
                break;
        }
    }
}

// Drawing
void clock_draw(clock_widget_t *clk) {
    if (!clk || !clk->window) return;

    // Update time if a second has passed
    if (timer_ticks - clk->last_update >= 1000) {  // 1 second at 1000Hz
        clock_update(clk);
    }

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(clk->window, &wx, &wy, &ww, &wh);

    // Background
    fb_fill_rect(wx, wy, ww, wh, 0x1E1E2E);

    int y = wy + 8;

    // Draw based on mode
    if (clk->mode == CLOCK_DIGITAL || clk->mode == CLOCK_BOTH) {
        draw_digital_clock(clk, wx, y, ww);
        y += 36;

        if (clk->show_date) {
            draw_date(clk, wx, y, ww);
            y += 20;
        }
    }

    if (clk->mode == CLOCK_ANALOG || clk->mode == CLOCK_BOTH) {
        int radius = (ww < wh - y + wy) ? (ww / 2 - 10) : ((wh - y + wy) / 2 - 10);
        int cx = wx + ww / 2;
        int cy = y + radius + 5;
        draw_analog_clock(clk, cx, cy, radius);
    }

    if (clk->mode == CLOCK_CALENDAR) {
        // Draw small digital time first
        draw_digital_clock(clk, wx, y, ww);
        y += 36;

        // Draw calendar below
        draw_calendar(clk, wx + 4, y, ww - 8, wh - (y - wy) - 4);
    }
}

// Launch clock widget
void clock_launch(void) {
    LOG_INFO("[Clock] Application launched");
    clock_widget_t *clk = clock_create();
    if (!clk) {
        LOG_ERROR("[Clock] Failed to create clock widget");
        kprintf("[Clock] Failed to create clock widget\n");
        return;
    }

    // Register with window manager
    wm_register_app(clk->window, clk,
                    (app_event_handler_t)clock_handle_event,
                    (app_draw_handler_t)clock_draw,
                    (app_destroy_handler_t)clock_destroy);

    kprintf("[Clock] Widget launched\n");
}
