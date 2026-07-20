// widget.h - Basic widget structures for MayteraOS GUI
#ifndef WIDGET_H
#define WIDGET_H

#include "../types.h"
#include "window.h"

// Widget types
typedef enum {
    WIDGET_LABEL = 0,
    WIDGET_BUTTON,
    WIDGET_CHECKBOX,
    WIDGET_TEXTBOX
} widget_type_t;

// Widget flags
#define WIDGET_FLAG_VISIBLE     (1 << 0)
#define WIDGET_FLAG_ENABLED     (1 << 1)
#define WIDGET_FLAG_FOCUSED     (1 << 2)
#define WIDGET_FLAG_HOVERED     (1 << 3)
#define WIDGET_FLAG_PRESSED     (1 << 4)
#define WIDGET_FLAG_CHECKED     (1 << 5)

// Default widget flags
#define WIDGET_FLAGS_DEFAULT (WIDGET_FLAG_VISIBLE | WIDGET_FLAG_ENABLED)

// Maximum text length for widgets
#define MAX_WIDGET_TEXT     128

// Widget colors
#define COLOR_BUTTON_BG             0x00D0D0D0  // Light gray
#define COLOR_BUTTON_BG_HOVER       0x00E0E0E0  // Lighter gray
#define COLOR_BUTTON_BG_PRESSED     0x00A0A0A0  // Darker gray
#define COLOR_BUTTON_BORDER         0x00606060  // Medium gray
#define COLOR_BUTTON_TEXT           0x00000000  // Black
#define COLOR_BUTTON_DISABLED       0x00808080  // Gray

#define COLOR_LABEL_TEXT            0x00000000  // Black

#define COLOR_TEXTBOX_BG            0x00FFFFFF  // White
#define COLOR_TEXTBOX_BORDER        0x00606060  // Gray
#define COLOR_TEXTBOX_TEXT          0x00000000  // Black
#define COLOR_TEXTBOX_CURSOR        0x00000000  // Black

#define COLOR_CHECKBOX_BG           0x00FFFFFF  // White
#define COLOR_CHECKBOX_BORDER       0x00606060  // Gray
#define COLOR_CHECKBOX_CHECK        0x00000000  // Black

// Forward declaration
struct window;

// Button click handler type
typedef void (*button_handler_t)(struct widget *widget, void *user_data);

// Widget structure
typedef struct widget {
    uint32_t id;                        // Unique widget ID
    widget_type_t type;                 // Widget type
    char text[MAX_WIDGET_TEXT];         // Widget text/label
    rect_t bounds;                      // Position and size (window-relative)
    uint32_t flags;                     // Widget flags

    // Colors
    uint32_t bg_color;                  // Background color
    uint32_t text_color;                // Text color
    uint32_t border_color;              // Border color

    // Parent window
    struct window *parent;

    // Widget-specific data
    union {
        struct {
            button_handler_t on_click;  // Click handler
            void *user_data;            // User data for click handler
        } button;

        struct {
            bool checked;               // Checkbox state
        } checkbox;

        struct {
            uint32_t cursor_pos;        // Cursor position
            uint32_t max_length;        // Maximum text length
        } textbox;
    };

    // Linked list pointer
    struct widget *next;
} widget_t;

// Widget creation functions

// Create a label widget
widget_t *widget_create_label(window_t *win, const char *text, int32_t x, int32_t y);

// Create a button widget
widget_t *widget_create_button(window_t *win, const char *text, int32_t x, int32_t y, int32_t width, int32_t height);

// Create a checkbox widget
widget_t *widget_create_checkbox(window_t *win, const char *text, int32_t x, int32_t y, bool checked);

// Create a textbox widget
widget_t *widget_create_textbox(window_t *win, int32_t x, int32_t y, int32_t width, uint32_t max_length);

// Widget destruction
void widget_destroy(widget_t *widget);
void widgets_destroy_all(window_t *win);

// Widget property setters
void widget_set_text(widget_t *widget, const char *text);
void widget_set_visible(widget_t *widget, bool visible);
void widget_set_enabled(widget_t *widget, bool enabled);
void widget_set_position(widget_t *widget, int32_t x, int32_t y);
void widget_set_size(widget_t *widget, int32_t width, int32_t height);

// Button-specific functions
void button_set_click_handler(widget_t *button, button_handler_t handler, void *user_data);

// Checkbox-specific functions
void checkbox_set_checked(widget_t *checkbox, bool checked);
bool checkbox_is_checked(widget_t *checkbox);

// Textbox-specific functions
const char *textbox_get_text(widget_t *textbox);
void textbox_set_text(widget_t *textbox, const char *text);
void textbox_clear(widget_t *textbox);

// Widget state queries
bool widget_is_visible(widget_t *widget);
bool widget_is_enabled(widget_t *widget);
bool widget_is_focused(widget_t *widget);
bool widget_is_hovered(widget_t *widget);

// Widget drawing
void widget_draw(widget_t *widget);
void widget_draw_label(widget_t *label);
void widget_draw_button(widget_t *button);
void widget_draw_checkbox(widget_t *checkbox);
void widget_draw_textbox(widget_t *textbox);

// Widget event handling
void widget_handle_mouse_move(widget_t *widget, int32_t x, int32_t y);
void widget_handle_mouse_down(widget_t *widget, int32_t x, int32_t y, uint32_t button);
void widget_handle_mouse_up(widget_t *widget, int32_t x, int32_t y, uint32_t button);
void widget_handle_key(widget_t *widget, uint32_t keycode, char key_char);

// Find widget at point (window-relative coordinates)
widget_t *widget_find_at_point(window_t *win, int32_t x, int32_t y);

#endif // WIDGET_H
