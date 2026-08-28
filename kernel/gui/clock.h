// clock.h - Desktop Clock Widget for MayteraOS
#ifndef CLOCK_H
#define CLOCK_H

#include "window.h"

// Clock display modes
typedef enum {
    CLOCK_DIGITAL,      // Digital display (HH:MM:SS)
    CLOCK_ANALOG,       // Analog clock face
    CLOCK_BOTH,         // Both digital and analog
    CLOCK_CALENDAR      // Monthly calendar view
} clock_mode_t;

// Clock widget state
typedef struct {
    window_t *window;       // Widget window
    clock_mode_t mode;      // Display mode
    bool show_seconds;      // Show seconds in digital mode
    bool show_date;         // Show date below time
    bool show_ampm;         // 12-hour format with AM/PM
    bool always_on_top;     // Keep on top of other windows

    // Current time (cached from RTC)
    int hour;
    int minute;
    int second;
    int day;
    int month;
    int year;
    int weekday;

    // Last update tick (to refresh once per second)
    uint64_t last_update;
} clock_widget_t;

// Create clock widget
clock_widget_t *clock_create(void);

// Destroy clock widget
void clock_destroy(clock_widget_t *clk);

// Update time from RTC
void clock_update(clock_widget_t *clk);

// Toggle display mode
void clock_toggle_mode(clock_widget_t *clk);

// Toggle seconds display
void clock_toggle_seconds(clock_widget_t *clk);

// Toggle date display
void clock_toggle_date(clock_widget_t *clk);

// Toggle 12/24 hour format
void clock_toggle_ampm(clock_widget_t *clk);

// Event handling
void clock_handle_event(clock_widget_t *clk, gui_event_t *event);

// Drawing
void clock_draw(clock_widget_t *clk);

// Launch clock widget
void clock_launch(void);

// Read RTC time
// #135: these now live in drivers/rtc.c (with the matching WRITE half and a
// shared codec). Re-exported here so the many existing callers that include
// clock.h keep compiling unchanged.
#include "../drivers/rtc.h"

// ==========================================================================
// #86: THE KERNEL'S VIEW OF THE CHOSEN TIMEZONE.
//
// The RTC on this OS holds UTC (net/sntp.c writes UTC into it; see
// userland/libc/tz.h for the tree-wide convention). Any clock the KERNEL draws
// must therefore add the user's offset itself, and before this existed the
// login screen did not: it printed raw UTC while every userland clock, which
// goes through libc/tz.c, printed local time.
//
// This is NOT a second timezone implementation. It reads the SAME file
// (/CONFIG/TZ.CFG) written by the first-run wizard and by Settings, and it
// parses the zone ID token, which ENCODES ITS OWN OFFSET ("UTC+09:30"). There
// is no zone table in the kernel to drift out of step with userland's.
//
// Ring 0 cannot link userland's libc/tz.c, which is the only reason a kernel
// reader exists at all.
// ==========================================================================

// Minutes east of UTC for the configured zone; 0 if nothing is configured
// (which is also a legitimate configured value: UTC). Re-reads TZ.CFG at most
// once every KTZ_REFRESH_MS and serves a cache in between, so a draw loop may
// call it every frame. Never blocks.
int ktz_offset_minutes(void);

// 1 if the offset above came out of TZ.CFG, 0 while the UTC fallback is in
// force. Distinct from "the offset is 0", because UTC is a zone a user can
// actually choose, so the offset alone cannot answer "has anyone set this?".
int ktz_is_set(void);

// LOCAL civil time for a kernel-drawn clock. `out` receives 7 ints:
//   [0] year [1] month 1-12 [2] day [3] hour [4] minute [5] second
//   [6] weekday, 0 = Sunday
// Returns 0 on success, -1 if the RTC does not present a plausible date (in
// which case NOTHING is written and the caller must not draw a made-up time).
//
// It shifts the EPOCH and re-derives the civil fields, rather than adding
// hours to the hour field. That is not fussiness: a +09:30 offset moves the
// date, the weekday and possibly the month, so patching only the hour would
// render "Thursday, August 20  01:30" when it is actually Friday the 21st.
int ktz_local_civil(int *out);

#endif // CLOCK_H
