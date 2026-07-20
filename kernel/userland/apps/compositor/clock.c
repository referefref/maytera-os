// clock.c - Floating clock widget for MayteraOS userland compositor.
// Renders a pill-shaped HH:MM:SS display in the top-right corner.

#include "compositor.h"
#include "../../libc/syscall.h"

// Format a value 0-99 into two ASCII digits at buf[0] and buf[1].
static void fmt_2digit(char *buf, int val) {
    buf[0] = '0' + (val / 10) % 10;
    buf[1] = '0' + (val % 10);
}

void clock_render(void) {
    // Read packed RTC value: bits 23-16 = hours, bits 15-8 = minutes, bits 7-0 = seconds.
    long rtc = sys_get_rtc_time();
    int hours   = (int)((rtc >> 16) & 0xFF);
    int minutes = (int)((rtc >>  8) & 0xFF);
    int seconds = (int)( rtc        & 0xFF);

    // Build "HH:MM:SS" into a fixed-size static buffer, no malloc needed.
    static char buf[9];
    fmt_2digit(buf + 0, hours);
    buf[2] = ':';
    fmt_2digit(buf + 3, minutes);
    buf[5] = ':';
    fmt_2digit(buf + 6, seconds);
    buf[8] = '\0';

    // Calculate widget geometry.
    int text_w   = text_width(buf);
    int widget_w = text_w  + CLOCK_PADDING_X * 2;
    int widget_h = FONT_CHAR_H + CLOCK_PADDING_Y * 2;

    // Position: flush to the top-right corner with configurable margins.
    int x = g_fb_width  - widget_w - CLOCK_MARGIN_RIGHT;
    int y = CLOCK_MARGIN_TOP;

    // Background pill shape, then text on top.
    draw_rounded_rect(x, y, widget_w, widget_h, CLOCK_CORNER_RADIUS, CLR_CLOCK_BG);
    draw_text(x + CLOCK_PADDING_X, y + CLOCK_PADDING_Y, buf, CLR_CLOCK_TEXT);
}
