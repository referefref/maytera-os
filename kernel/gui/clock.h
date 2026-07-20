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
void rtc_read_time(int *hour, int *minute, int *second);
void rtc_read_date(int *day, int *month, int *year, int *weekday);

#endif // CLOCK_H
