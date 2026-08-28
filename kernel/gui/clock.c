// clock.c - Desktop Clock Widget for MayteraOS
#include "clock.h"
#include "window.h"
#include "icons.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../cpu/mono.h"   // #115: bound the RTC update-in-progress waits
#include "../cpu/wallclock.h" // #86: wallclock_now_unix + ktime_unix_to_civil_rs
#include "../fs/fat.h"        // #86: read /CONFIG/TZ.CFG (the SAME file userland writes)
#include "../fs/bootlog.h"    // #192: the absent-TZ note belongs in the persistent log too
#include "../fs/cfgread.h"    // #192: is this read outcome worth a log line?
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../video/graphics.h"
#include "syslog.h"

// External timer ticks
extern volatile uint64_t timer_ticks;

// RTC ACCESS MOVED TO drivers/rtc.c (#135).
//
// This widget used to host the port I/O for the whole kernel: rtc_read_time()
// and rtc_read_date() were defined here and called by syscalls 142/143,
// rustkern/ktime.rs's wall clock, gui/login.c and, through those, every
// userland clock. The write half lived in proc/syscall.c and encoded
// registers DIFFERENTLY, which is what shipped the 6h06m clock error.
// Both halves now share one codec (rustkern/rtcenc.rs) behind one driver.
// The declarations still come from clock.h, so callers are unchanged.

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

// ===========================================================================
// #86: the kernel-side reader for the configured timezone. See clock.h for why
// this exists and why it is a parser rather than a second zone list.
// ===========================================================================

// Same throttle as userland/libc/tz.c's TZ_REFRESH_MS, deliberately: a user who
// changes the zone in Settings should see the login clock agree within the same
// couple of seconds, not on a different schedule.
#define KTZ_REFRESH_MS 2000ULL

static int      g_ktz_off_min  = 0;
static int      g_ktz_from_file = 0;
static uint64_t g_ktz_last_ms  = 0;
static int      g_ktz_loaded   = 0;

// #192: named ONCE, so the existence probe, the read and the cfgread policy key
// cannot drift apart. The SAME path the first-run wizard and Settings write
// (root's home is "/", so userconf resolves TZ.CFG to exactly this for a root
// session, and the login screen is pre-session so the global file is right).
#define KTZ_CFG_PATH "/CONFIG/TZ.CFG"

static void ktz_reload(void) {
    extern fat_fs_t g_fat_fs;
    extern int32_t ktz_parse_offset_rs(const uint8_t *buf, int32_t len, int32_t *out_min);

    // #192: THIS FUNCTION IS THE LOOP THAT FLOODED THE BOOT LOG. It runs every
    // KTZ_REFRESH_MS for as long as the login clock is on screen, which is
    // forever on a machine sitting at the sign-in screen. It called
    // fat_read_file_retry(), the #307 bounded-retry reader built for a file
    // that IS present on flaky USB media. On a fresh image TZ.CFG is ABSENT,
    // the normal state before anyone picks a timezone, so every two seconds it
    // produced three "read FAILED/empty ... giving up" lines and burned about
    // 900,000 io_wait() iterations of pointless backoff. "Giving up" printed,
    // and then the whole cycle started again.
    //
    // Two changes. First, ASK WHETHER THE FILE IS THERE before reading it:
    // fat_exists() is routing-correct (ext2 root first, then the FAT ESP, the
    // same order fat_read_file() uses), allocates nothing and prints nothing.
    // Second, use the PLAIN reader, not the retry reader: a poll that comes
    // round again in two seconds has no use for a spin backoff, and a transient
    // miss costs one refresh interval instead of stalling the caller.
    //
    // THE POLL ITSELF STAYS. Settings and the first-run wizard write TZ.CFG
    // while this clock is on screen, and the clock must follow within a couple
    // of seconds without a reboot (clock.h states that contract). What is
    // removed is the noise and the spin, not the liveness.
    // #192: STORAGE MUST BE UP BEFORE "absent" MEANS "not configured".
    // main.c runs the [KTZ] boot self-test BEFORE the ext2 root is mounted (the
    // mount is ~60 lines later in the serial log), and /CONFIG lives on that
    // root. Without this guard the very first refresh reported "no timezone
    // configured yet" on a machine that HAD one, which is the same class of
    // dishonest line #192 exists to remove, just quieter. Asking whether the
    // /CONFIG DIRECTORY is there uses the same routing-correct primitive and
    // needs no new mount-state plumbing: no /CONFIG, no verdict, no line.
    if (fat_exists(&g_fat_fs, "/CONFIG") != 1) {
        return;
    }
    if (fat_exists(&g_fat_fs, KTZ_CFG_PATH) != 1) {
        if (cfgread_report_rs(KTZ_CFG_PATH, -1, CFG_OUTCOME_ABSENT) == CFG_LOG_NOTE) {
            kprintf("[KTZ] no timezone configured yet (%s absent); clock shows UTC\n",
                    KTZ_CFG_PATH);
            bootlog_write("[KTZ] no timezone configured yet (%s absent); clock shows UTC",
                          KTZ_CFG_PATH);
        }
        return;
    }

    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, KTZ_CFG_PATH, &size);
    if (!data || size == 0) {
        // Present a moment ago and unreadable now. That IS a fault, so it stays
        // loud; cfgread.rs rate-limits it rather than silencing it.
        int act = cfgread_report_rs(KTZ_CFG_PATH, -1, CFG_OUTCOME_IOERR);
        if (act == CFG_LOG_WARN) {
            kprintf("[KTZ] %s present but read FAILED; keeping previous offset\n",
                    KTZ_CFG_PATH);
            bootlog_write("[KTZ] %s present but read FAILED; keeping previous offset",
                          KTZ_CFG_PATH);
        } else if (act == CFG_LOG_SUPPRESSED) {
            bootlog_write("[KTZ] %s: further read failures will not be logged "
                          "until it reads successfully again", KTZ_CFG_PATH);
        }
        // UTC stays the honest answer until a read succeeds, and it is NOT
        // recorded as "configured", so a later successful read still wins.
        if (data) kfree(data);
        return;
    }
    if (cfgread_report_rs(KTZ_CFG_PATH, -1, CFG_OUTCOME_OK) == CFG_LOG_NOTE) {
        kprintf("[KTZ] timezone read from %s (%u bytes)\n", KTZ_CFG_PATH, size);
        bootlog_write("[KTZ] timezone read from %s (%u bytes)", KTZ_CFG_PATH, size);
    }
    int32_t off = 0;
    if (ktz_parse_offset_rs((const uint8_t *)data, (int32_t)size, &off) == 0) {
        g_ktz_off_min   = (int)off;
        g_ktz_from_file = 1;
    }
    // A parse failure deliberately leaves the previous value alone: a torn or
    // truncated read must not snap the clock to UTC for one frame.
    kfree(data);
}

static void ktz_refresh(void) {
    uint64_t now = mono_ms();
    if (!g_ktz_loaded || (now - g_ktz_last_ms) >= KTZ_REFRESH_MS) {
        g_ktz_last_ms = now;
        g_ktz_loaded  = 1;
        ktz_reload();
    }
}

int ktz_offset_minutes(void) { ktz_refresh(); return g_ktz_off_min; }
int ktz_is_set(void)         { ktz_refresh(); return g_ktz_from_file; }

int ktz_local_civil(int *out) {
    if (!out) return -1;
    int64_t utc = wallclock_now_unix();     // epoch seconds, 0 = RTC not sane
    if (utc <= 0) return -1;
    int64_t local = utc + (int64_t)ktz_offset_minutes() * 60;
    return (int)ktime_unix_to_civil_rs(local, out);
}
