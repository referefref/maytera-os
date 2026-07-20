// settings.h - GUI Settings application for MayteraOS
#ifndef SETTINGS_H
#define SETTINGS_H

#include "../types.h"
#include "window.h"

// Settings window dimensions
#define SETTINGS_WIDTH      560
#define SETTINGS_HEIGHT     530

// Section tabs
#define SETTINGS_SECTION_DISPLAY    0
#define SETTINGS_SECTION_THEMES     1
#define SETTINGS_SECTION_DEVICES    2
#define SETTINGS_SECTION_SYSTEM     3
#define SETTINGS_SECTION_ABOUT      4
#define SETTINGS_NUM_SECTIONS       5

// Layout constants
#define SETTINGS_TAB_HEIGHT     28
#define SETTINGS_PADDING        15
#define SETTINGS_LINE_HEIGHT    22

// Theme card layout
#define THEME_CARD_WIDTH    160
#define THEME_CARD_HEIGHT   80
#define THEME_CARD_GAP_X    12
#define THEME_CARD_GAP_Y    8
#define THEME_CARDS_PER_ROW 3

// Settings structure
typedef struct {
    window_t *window;
    int current_section;
    bool running;

    // Window manager integration
    int app_id;                 // WM app registration ID
    int dock_index;             // Dock/taskbar index
} settings_t;

// Create and show the settings window
settings_t *settings_create(void);

// Destroy settings window
void settings_destroy(settings_t *settings);

// Run settings main loop (returns when closed)
void settings_run(settings_t *settings);

// Draw settings content
void settings_draw(settings_t *settings);

// Launch callback for dock (non-blocking)
void settings_launch(void);

// Window manager callbacks
void settings_on_event(void *app_data, gui_event_t *event);
void settings_on_draw(void *app_data);
void settings_on_destroy(void *app_data);

#endif // SETTINGS_H
