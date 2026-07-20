// gui.h - GUI library for MayteraOS user-space applications
// Provides window protocol client and drawing helpers
#ifndef _GUI_H
#define _GUI_H

#include "types.h"
#include "syscall.h"

// ============================================================================
// Event Types (must match kernel's window.h)
// ============================================================================

typedef enum {
    EVENT_NONE = 0,
    EVENT_MOUSE_MOVE,
    EVENT_MOUSE_DOWN,
    EVENT_MOUSE_UP,
    EVENT_MOUSE_SCROLL,
    EVENT_KEY_DOWN,
    EVENT_KEY_UP,
    EVENT_WINDOW_CLOSE,
    EVENT_WINDOW_FOCUS,
    EVENT_WINDOW_BLUR,
    EVENT_BUTTON_CLICK,
    EVENT_REDRAW,
    EVENT_RESIZE           // param: mouse_x = new width, mouse_y = new height
} event_type_t;

// Mouse button flags
#define MOUSE_BUTTON_LEFT   (1 << 0)
#define MOUSE_BUTTON_RIGHT  (1 << 1)
#define MOUSE_BUTTON_MIDDLE (1 << 2)

// Event structure (must match kernel's gui_event_t)
typedef struct {
    event_type_t type;
    uint32_t target_id;     // Window or widget ID
    int32_t mouse_x;        // Mouse X position (screen coords)
    int32_t mouse_y;        // Mouse Y position (screen coords)
    uint32_t mouse_buttons; // Mouse button state
    int8_t scroll_delta;    // Scroll wheel delta
    uint32_t keycode;       // Keyboard keycode
    char key_char;          // Printable character
} gui_event_t;

// ============================================================================
// Common Colors
// ============================================================================

#define COLOR_BLACK         0x00000000
#define COLOR_WHITE         0x00FFFFFF
#define COLOR_RED           0x00FF0000
#define COLOR_GREEN         0x0000FF00
#define COLOR_BLUE          0x000000FF
#define COLOR_DARK_GRAY     0x00404040
#define COLOR_GRAY          0x00808080
#define COLOR_LIGHT_GRAY    0x00C0C0C0
#define COLOR_YELLOW        0x00FFFF00
#define COLOR_CYAN          0x0000FFFF
#define COLOR_MAGENTA       0x00FF00FF
#define COLOR_ORANGE        0x00FF8C00

// Calculator-specific colors
#define BTN_COLOR_DIGIT     0x00404040  // Dark gray for digits
#define BTN_COLOR_OP        0x00FF8C00  // Orange for operations
#define BTN_COLOR_CLEAR     0x00FF4040  // Red for clear
#define BTN_COLOR_EQUALS    0x0040A040  // Green for equals
#define BTN_COLOR_HOVER     0x00606060  // Hover highlight
#define BTN_COLOR_TEXT      0x00FFFFFF  // White text

// Display colors
#define DISPLAY_BG          0x00202020  // Dark background
#define DISPLAY_FG          0x00FFFFFF  // White text

// Terminal colors (ANSI standard 16 colors)
#define TERM_COLOR_BLACK        0x00000000
#define TERM_COLOR_RED          0x00AA0000
#define TERM_COLOR_GREEN        0x0000AA00
#define TERM_COLOR_YELLOW       0x00AAAA00
#define TERM_COLOR_BLUE         0x000000AA
#define TERM_COLOR_MAGENTA      0x00AA00AA
#define TERM_COLOR_CYAN         0x0000AAAA
#define TERM_COLOR_WHITE        0x00AAAAAA
#define TERM_COLOR_BRIGHT_BLACK 0x00555555
#define TERM_COLOR_BRIGHT_RED   0x00FF5555
#define TERM_COLOR_BRIGHT_GREEN 0x0055FF55
#define TERM_COLOR_BRIGHT_YELLOW 0x00FFFF55
#define TERM_COLOR_BRIGHT_BLUE  0x005555FF
#define TERM_COLOR_BRIGHT_MAGENTA 0x00FF55FF
#define TERM_COLOR_BRIGHT_CYAN  0x0055FFFF
#define TERM_COLOR_BRIGHT_WHITE 0x00FFFFFF

// Default terminal colors
#define TERM_FG_COLOR   TERM_COLOR_BRIGHT_WHITE
#define TERM_BG_COLOR   0x001E1E1E  // Dark gray

// Editor colors
#define EDITOR_BG_COLOR     0x001E1E1E
#define EDITOR_TEXT_COLOR   0x00D4D4D4
#define EDITOR_LINE_COLOR   0x00858585
#define EDITOR_SEL_COLOR    0x00264F78
#define EDITOR_CURSOR_COLOR 0x00AEAFAD

// ============================================================================
// Font Dimensions (8x16 regular font)
// ============================================================================

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

// ============================================================================
// Window Protocol Client Functions
// ============================================================================

// Create a window with specified title and dimensions
// Returns: window handle (>=0) on success, -1 on failure
int gui_window_create(const char *title, int x, int y, int width, int height);

// Destroy a window
int gui_window_destroy(int handle);

// ============================================================================
// Drawing Functions
// ============================================================================

// Fill a rectangle with solid color
void gui_fill_rect(int handle, int x, int y, int width, int height, uint32_t color);

// Draw rectangle outline (not filled)
void gui_draw_rect(int handle, int x, int y, int width, int height, uint32_t color);

// Draw a single pixel
void gui_draw_pixel(int handle, int x, int y, uint32_t color);

// Draw text at position
void gui_draw_text(int handle, int x, int y, const char *text, uint32_t color);

// Draw text with background color
void gui_draw_text_bg(int handle, int x, int y, const char *text,
                      uint32_t fg_color, uint32_t bg_color);

// Draw a character at position
void gui_draw_char(int handle, int x, int y, char c, uint32_t color);

// Draw a character with background
void gui_draw_char_bg(int handle, int x, int y, char c,
                      uint32_t fg_color, uint32_t bg_color);

// Draw a 3D-style button
void gui_draw_button_3d(int handle, int x, int y, int width, int height,
                        uint32_t bg_color, bool pressed);

// Draw centered text within a rectangle
void gui_draw_text_centered(int handle, int x, int y, int width, int height,
                            const char *text, uint32_t color);

// ============================================================================
// Widget Drawing Helpers
// ============================================================================

// Draw a labeled button
void gui_draw_button(int handle, int x, int y, int width, int height,
                     const char *label, uint32_t bg_color, uint32_t text_color,
                     bool hovered, bool pressed);

// Draw a text input field
void gui_draw_textfield(int handle, int x, int y, int width, int height,
                        const char *text, uint32_t bg_color, uint32_t text_color,
                        uint32_t border_color);

// Draw a checkbox
void gui_draw_checkbox(int handle, int x, int y, bool checked,
                       const char *label, uint32_t color);

// Draw a progress bar
void gui_draw_progressbar(int handle, int x, int y, int width, int height,
                          int percent, uint32_t bg_color, uint32_t fg_color);

// Draw a vertical scrollbar
void gui_draw_scrollbar_v(int handle, int x, int y, int height,
                          int thumb_pos, int thumb_size, uint32_t bg_color);

// ============================================================================
// Event Handling
// ============================================================================

// Get window event with timeout
// timeout: 0 = non-blocking, >0 = wait up to timeout ms, -1 = wait forever
// Returns: event type, fills event structure
int gui_get_event(int handle, gui_event_t *event, int timeout);

// Request window redraw
void gui_invalidate(int handle);

// ============================================================================
// Utility Functions
// ============================================================================

// Get string width in pixels
static inline int gui_string_width(const char *str) {
    int len = 0;
    while (*str++) len++;
    return len * FONT_WIDTH;
}

// Check if point is within rectangle
static inline bool gui_point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

// Integer to string conversion
void gui_itoa(long num, char *buf, int max_len);
void gui_utoa(unsigned long num, char *buf, int max_len);
void gui_itoa_hex(unsigned long num, char *buf, int digits);

// ============================================================================
// Inline Drawing Helpers (using syscalls directly)
// ============================================================================

// Draw a horizontal line
static inline void gui_draw_hline(int handle, int x, int y, int width, uint32_t color) {
    win_draw_rect(handle, x, y, width, 1, color);
}

// Draw a vertical line
static inline void gui_draw_vline(int handle, int x, int y, int height, uint32_t color) {
    win_draw_rect(handle, x, y, 1, height, color);
}

// Draw rectangle outline (inline version)
static inline void gui_draw_rect_outline(int handle, int x, int y, int w, int h, uint32_t color) {
    gui_draw_hline(handle, x, y, w, color);           // Top
    gui_draw_hline(handle, x, y + h - 1, w, color);   // Bottom
    gui_draw_vline(handle, x, y, h, color);           // Left
    gui_draw_vline(handle, x + w - 1, y, h, color);   // Right
}

// Shared widget-style engine (style-aware primitives)
#include "gui_style.h"

// Reusable caret-aware single-line text input (task #244)
#include "textfield.h"

#endif // _GUI_H
