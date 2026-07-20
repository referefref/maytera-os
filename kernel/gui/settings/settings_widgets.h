// settings_widgets.h - Common UI widgets for Settings panels
// MayteraOS Unified Settings Framework
#ifndef SETTINGS_WIDGETS_H
#define SETTINGS_WIDGETS_H

#include "../../types.h"
#include "../window.h"

// ============================================================================
// Widget Constants
// ============================================================================

// Toggle switch dimensions
#define SW_TOGGLE_WIDTH         44
#define SW_TOGGLE_HEIGHT        22
#define SW_TOGGLE_KNOB_SIZE     18
#define SW_TOGGLE_PADDING       2

// Slider dimensions
#define SW_SLIDER_WIDTH         200
#define SW_SLIDER_HEIGHT        20
#define SW_SLIDER_TRACK_HEIGHT  4
#define SW_SLIDER_THUMB_SIZE    16

// Dropdown dimensions
#define SW_DROPDOWN_WIDTH       180
#define SW_DROPDOWN_HEIGHT      24
#define SW_DROPDOWN_MAX_ITEMS   20
#define SW_DROPDOWN_ITEM_HEIGHT 22
#define SW_DROPDOWN_MAX_TEXT    64

// Color picker dimensions
#define SW_COLOR_PICKER_SIZE    28
#define SW_COLOR_SWATCH_SIZE    20
#define SW_COLOR_PALETTE_COLS   8
#define SW_COLOR_PALETTE_ROWS   4

// List view dimensions
#define SW_LISTVIEW_ITEM_HEIGHT 24
#define SW_LISTVIEW_MAX_ITEMS   100
#define SW_LISTVIEW_MAX_TEXT    64

// Tab container dimensions
#define SW_TAB_HEIGHT           28
#define SW_TAB_MAX_TABS         8
#define SW_TAB_MAX_NAME         24

// Progress bar dimensions
#define SW_PROGRESS_HEIGHT      16

// Separator dimensions
#define SW_SEPARATOR_HEIGHT     1
#define SW_SEPARATOR_MARGIN     8

// Section header dimensions
#define SW_SECTION_HEIGHT       20
#define SW_SECTION_MARGIN       12

// Widget maximum label length
#define SW_LABEL_MAX            64

// ============================================================================
// Widget Types
// ============================================================================

typedef enum {
    SW_TYPE_TOGGLE = 0,
    SW_TYPE_SLIDER,
    SW_TYPE_DROPDOWN,
    SW_TYPE_COLORPICKER,
    SW_TYPE_LISTVIEW,
    SW_TYPE_TABCONTAINER,
    SW_TYPE_PROGRESS,
    SW_TYPE_SEPARATOR,
    SW_TYPE_SECTION
} settings_widget_type_t;

// ============================================================================
// Callback Types
// ============================================================================

// Toggle switch changed callback
typedef void (*sw_toggle_changed_fn)(bool new_value, void *user_data);

// Slider value changed callback
typedef void (*sw_slider_changed_fn)(int value, void *user_data);

// Dropdown selection changed callback
typedef void (*sw_dropdown_changed_fn)(int index, const char *item, void *user_data);

// Color picker changed callback
typedef void (*sw_color_changed_fn)(uint32_t color, void *user_data);

// List view item selected callback
typedef void (*sw_listview_selected_fn)(int index, void *user_data);

// Tab container tab changed callback
typedef void (*sw_tab_changed_fn)(int tab_index, void *user_data);

// ============================================================================
// Widget Structures
// ============================================================================

// Toggle switch widget (on/off)
typedef struct sw_toggle {
    char label[SW_LABEL_MAX];
    int32_t x, y;
    bool value;
    bool enabled;
    bool hovered;
    sw_toggle_changed_fn on_change;
    void *user_data;
} sw_toggle_t;

// Slider widget
typedef struct sw_slider {
    char label[SW_LABEL_MAX];
    int32_t x, y;
    int width;
    int min_value;
    int max_value;
    int value;
    int step;
    bool enabled;
    bool dragging;
    sw_slider_changed_fn on_change;
    void *user_data;
    char suffix[16];        // e.g., "%", "ms", "px"
    bool show_value;        // Show current value next to slider
} sw_slider_t;

// Dropdown item
typedef struct sw_dropdown_item {
    char text[SW_DROPDOWN_MAX_TEXT];
    int id;                 // Optional item identifier
    void *data;             // Optional item data
} sw_dropdown_item_t;

// Dropdown widget
typedef struct sw_dropdown {
    char label[SW_LABEL_MAX];
    int32_t x, y;
    int width;
    sw_dropdown_item_t items[SW_DROPDOWN_MAX_ITEMS];
    int item_count;
    int selected_index;
    bool enabled;
    bool expanded;          // Is dropdown list visible?
    bool hovered;
    int hover_index;        // Item being hovered (-1 if none)
    sw_dropdown_changed_fn on_change;
    void *user_data;
} sw_dropdown_t;

// Color picker widget
typedef struct sw_colorpicker {
    char label[SW_LABEL_MAX];
    int32_t x, y;
    uint32_t color;
    bool enabled;
    bool expanded;          // Is palette visible?
    bool hovered;
    sw_color_changed_fn on_change;
    void *user_data;
} sw_colorpicker_t;

// List view item
typedef struct sw_listview_item {
    char text[SW_LISTVIEW_MAX_TEXT];
    char secondary[SW_LISTVIEW_MAX_TEXT];   // Secondary text (optional)
    int id;
    void *data;
    bool selected;
} sw_listview_item_t;

// List view widget
typedef struct sw_listview {
    char label[SW_LABEL_MAX];
    int32_t x, y;
    int width, height;
    sw_listview_item_t items[SW_LISTVIEW_MAX_ITEMS];
    int item_count;
    int selected_index;
    int scroll_offset;      // First visible item index
    bool enabled;
    bool multi_select;      // Allow multiple selection
    int hover_index;
    sw_listview_selected_fn on_select;
    void *user_data;
} sw_listview_t;

// Tab in tab container
typedef struct sw_tab {
    char name[SW_TAB_MAX_NAME];
    int id;
    void *content;          // Tab-specific content pointer
} sw_tab_t;

// Tab container widget
typedef struct sw_tabcontainer {
    int32_t x, y;
    int width, height;
    sw_tab_t tabs[SW_TAB_MAX_TABS];
    int tab_count;
    int active_tab;
    bool enabled;
    sw_tab_changed_fn on_change;
    void *user_data;
} sw_tabcontainer_t;

// Progress bar widget (read-only display)
typedef struct sw_progress {
    char label[SW_LABEL_MAX];
    int32_t x, y;
    int width;
    int value;              // 0-100
    char text[32];          // Optional text to show on bar
    uint32_t fill_color;    // Custom fill color (0 for theme default)
} sw_progress_t;

// Section header (visual separator with title)
typedef struct sw_section {
    char title[SW_LABEL_MAX];
    int32_t x, y;
    int width;
} sw_section_t;

// ============================================================================
// Toggle Switch API
// ============================================================================

// Create a toggle switch
void sw_toggle_init(sw_toggle_t *toggle, const char *label, int32_t x, int32_t y, bool value);

// Draw toggle switch
void sw_toggle_draw(sw_toggle_t *toggle);

// Handle mouse event (returns true if handled)
bool sw_toggle_handle_event(sw_toggle_t *toggle, gui_event_t *event, int32_t offset_x, int32_t offset_y);

// Set toggle value
void sw_toggle_set_value(sw_toggle_t *toggle, bool value);

// Get toggle value
bool sw_toggle_get_value(sw_toggle_t *toggle);

// ============================================================================
// Slider API
// ============================================================================

// Create a slider
void sw_slider_init(sw_slider_t *slider, const char *label, int32_t x, int32_t y,
                    int width, int min_val, int max_val, int value);

// Draw slider
void sw_slider_draw(sw_slider_t *slider);

// Handle mouse event (returns true if handled)
bool sw_slider_handle_event(sw_slider_t *slider, gui_event_t *event, int32_t offset_x, int32_t offset_y);

// Set slider value
void sw_slider_set_value(sw_slider_t *slider, int value);

// Get slider value
int sw_slider_get_value(sw_slider_t *slider);

// ============================================================================
// Dropdown API
// ============================================================================

// Create a dropdown
void sw_dropdown_init(sw_dropdown_t *dropdown, const char *label, int32_t x, int32_t y, int width);

// Add item to dropdown
void sw_dropdown_add_item(sw_dropdown_t *dropdown, const char *text, int id, void *data);

// Clear all items
void sw_dropdown_clear(sw_dropdown_t *dropdown);

// Draw dropdown
void sw_dropdown_draw(sw_dropdown_t *dropdown);

// Handle mouse event (returns true if handled)
bool sw_dropdown_handle_event(sw_dropdown_t *dropdown, gui_event_t *event, int32_t offset_x, int32_t offset_y);

// Set selected index
void sw_dropdown_set_selected(sw_dropdown_t *dropdown, int index);

// Get selected index
int sw_dropdown_get_selected(sw_dropdown_t *dropdown);

// Get selected item text
const char *sw_dropdown_get_selected_text(sw_dropdown_t *dropdown);

// ============================================================================
// Color Picker API
// ============================================================================

// Create a color picker
void sw_colorpicker_init(sw_colorpicker_t *picker, const char *label, int32_t x, int32_t y, uint32_t color);

// Draw color picker
void sw_colorpicker_draw(sw_colorpicker_t *picker);

// Handle mouse event (returns true if handled)
bool sw_colorpicker_handle_event(sw_colorpicker_t *picker, gui_event_t *event, int32_t offset_x, int32_t offset_y);

// Set color
void sw_colorpicker_set_color(sw_colorpicker_t *picker, uint32_t color);

// Get color
uint32_t sw_colorpicker_get_color(sw_colorpicker_t *picker);

// ============================================================================
// List View API
// ============================================================================

// Create a list view
void sw_listview_init(sw_listview_t *list, const char *label, int32_t x, int32_t y,
                      int width, int height);

// Add item to list
void sw_listview_add_item(sw_listview_t *list, const char *text, const char *secondary,
                          int id, void *data);

// Remove item at index
void sw_listview_remove_item(sw_listview_t *list, int index);

// Clear all items
void sw_listview_clear(sw_listview_t *list);

// Draw list view
void sw_listview_draw(sw_listview_t *list);

// Handle mouse event (returns true if handled)
bool sw_listview_handle_event(sw_listview_t *list, gui_event_t *event, int32_t offset_x, int32_t offset_y);

// Get selected index
int sw_listview_get_selected(sw_listview_t *list);

// Set selected index
void sw_listview_set_selected(sw_listview_t *list, int index);

// ============================================================================
// Tab Container API
// ============================================================================

// Create a tab container
void sw_tabcontainer_init(sw_tabcontainer_t *tabs, int32_t x, int32_t y, int width, int height);

// Add a tab
void sw_tabcontainer_add_tab(sw_tabcontainer_t *tabs, const char *name, int id, void *content);

// Draw tab container (tabs only, content is handled separately)
void sw_tabcontainer_draw(sw_tabcontainer_t *tabs);

// Handle mouse event (returns true if handled)
bool sw_tabcontainer_handle_event(sw_tabcontainer_t *tabs, gui_event_t *event, int32_t offset_x, int32_t offset_y);

// Get active tab index
int sw_tabcontainer_get_active(sw_tabcontainer_t *tabs);

// Set active tab
void sw_tabcontainer_set_active(sw_tabcontainer_t *tabs, int index);

// Get content area bounds for drawing tab content
void sw_tabcontainer_get_content_bounds(sw_tabcontainer_t *tabs, int32_t *x, int32_t *y,
                                        int32_t *width, int32_t *height);

// ============================================================================
// Progress Bar API
// ============================================================================

// Create a progress bar
void sw_progress_init(sw_progress_t *progress, const char *label, int32_t x, int32_t y, int width);

// Draw progress bar
void sw_progress_draw(sw_progress_t *progress);

// Set progress value (0-100)
void sw_progress_set_value(sw_progress_t *progress, int value);

// Set progress text
void sw_progress_set_text(sw_progress_t *progress, const char *text);

// ============================================================================
// Section Header API
// ============================================================================

// Create a section header
void sw_section_init(sw_section_t *section, const char *title, int32_t x, int32_t y, int width);

// Draw section header
void sw_section_draw(sw_section_t *section);

// ============================================================================
// Utility Functions
// ============================================================================

// Draw a separator line
void sw_draw_separator(int32_t x, int32_t y, int width);

// Draw a label with optional description
void sw_draw_label(int32_t x, int32_t y, const char *text, uint32_t color);

// Draw a label with description below
void sw_draw_label_with_desc(int32_t x, int32_t y, const char *label, const char *description);

// Draw info text (smaller, muted color)
void sw_draw_info(int32_t x, int32_t y, const char *text);

// Draw a horizontal rule
void sw_draw_rule(int32_t x, int32_t y, int width);

// Check if point is within bounds
bool sw_point_in_rect(int32_t px, int32_t py, int32_t rx, int32_t ry, int32_t rw, int32_t rh);

#endif // SETTINGS_WIDGETS_H
