// widget.c - Widget implementation for MayteraOS GUI
#include "widget.h"
#include "window.h"
#include "themes.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../video/font.h"

// Static widget ID counter
static uint32_t next_widget_id = 1;

// Draw a character at a specific position
static void widget_draw_char(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font_get_glyph(c);
    if (!glyph) return;

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            fb_put_pixel(x + col, y + row, color);
        }
    }
}

// Draw text with background
__attribute__((unused))
static void widget_draw_text_bg(int32_t x, int32_t y, const char *text, uint32_t fg, uint32_t bg) {
    while (*text) {
        widget_draw_char(x, y, *text, fg, bg);
        x += FONT_WIDTH;
        text++;
    }
}

// Draw text transparently (no background)
static void widget_draw_text(int32_t x, int32_t y, const char *text, uint32_t fg) {
    while (*text) {
        const uint8_t *glyph = font_get_glyph(*text);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(x + col, y + row, fg);
                    }
                }
            }
        }
        x += FONT_WIDTH;
        text++;
    }
}

// Helper to get screen coordinates from widget
static void widget_to_screen(widget_t *widget, int32_t *sx, int32_t *sy) {
    if (!widget || !widget->parent) {
        *sx = widget->bounds.x;
        *sy = widget->bounds.y;
        return;
    }

    int32_t content_x, content_y, content_w, content_h;
    window_get_content_bounds(widget->parent, &content_x, &content_y, &content_w, &content_h);

    *sx = content_x + widget->bounds.x;
    *sy = content_y + widget->bounds.y;
}

// Create base widget (internal helper)
static widget_t *widget_create_base(window_t *win, widget_type_t type,
                                    int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!win) {
        kprintf("widget_create_base: NULL window\n");
        return NULL;
    }

    widget_t *widget = (widget_t *)kzalloc(sizeof(widget_t));
    if (!widget) {
        kprintf("Failed to allocate widget\n");
        return NULL;
    }

    widget->id = next_widget_id++;
    widget->type = type;
    widget->bounds.x = x;
    widget->bounds.y = y;
    widget->bounds.width = w;
    widget->bounds.height = h;
    widget->flags = WIDGET_FLAGS_DEFAULT;
    widget->parent = win;
    widget->text[0] = '\0';

    // Add to window's widget list
    widget->next = win->widgets;
    win->widgets = widget;
    win->widget_count++;

    return widget;
}

// Create a label widget
widget_t *widget_create_label(window_t *win, const char *text, int32_t x, int32_t y) {
    size_t text_len = strlen(text);
    int32_t w = text_len * FONT_WIDTH;
    int32_t h = FONT_HEIGHT;

    widget_t *label = widget_create_base(win, WIDGET_LABEL, x, y, w, h);
    if (!label) return NULL;

    strncpy(label->text, text, MAX_WIDGET_TEXT - 1);
    label->text[MAX_WIDGET_TEXT - 1] = '\0';

    label->text_color = COLOR_LABEL_TEXT;
    label->bg_color = 0;  // Transparent background

    kprintf("Created label '%s' at (%d, %d)\n", text, x, y);
    return label;
}

// Create a button widget
widget_t *widget_create_button(window_t *win, const char *text,
                               int32_t x, int32_t y, int32_t width, int32_t height) {
    widget_t *button = widget_create_base(win, WIDGET_BUTTON, x, y, width, height);
    if (!button) return NULL;

    strncpy(button->text, text, MAX_WIDGET_TEXT - 1);
    button->text[MAX_WIDGET_TEXT - 1] = '\0';

    button->bg_color = COLOR_BUTTON_BG;
    button->text_color = COLOR_BUTTON_TEXT;
    button->border_color = COLOR_BUTTON_BORDER;

    button->button.on_click = NULL;
    button->button.user_data = NULL;

    kprintf("Created button '%s' at (%d, %d) size %dx%d\n", text, x, y, width, height);
    return button;
}

// Create a checkbox widget
widget_t *widget_create_checkbox(window_t *win, const char *text, int32_t x, int32_t y, bool checked) {
    // Checkbox is 14x14 pixels plus text
    int32_t box_size = 14;
    size_t text_len = strlen(text);
    int32_t w = box_size + 4 + text_len * FONT_WIDTH;
    int32_t h = (box_size > FONT_HEIGHT) ? box_size : FONT_HEIGHT;

    widget_t *checkbox = widget_create_base(win, WIDGET_CHECKBOX, x, y, w, h);
    if (!checkbox) return NULL;

    strncpy(checkbox->text, text, MAX_WIDGET_TEXT - 1);
    checkbox->text[MAX_WIDGET_TEXT - 1] = '\0';

    checkbox->bg_color = COLOR_CHECKBOX_BG;
    checkbox->text_color = COLOR_LABEL_TEXT;
    checkbox->border_color = COLOR_CHECKBOX_BORDER;

    checkbox->checkbox.checked = checked;
    if (checked) {
        checkbox->flags |= WIDGET_FLAG_CHECKED;
    }

    kprintf("Created checkbox '%s' at (%d, %d)\n", text, x, y);
    return checkbox;
}

// Create a textbox widget
widget_t *widget_create_textbox(window_t *win, int32_t x, int32_t y,
                                int32_t width, uint32_t max_length) {
    int32_t h = FONT_HEIGHT + 6;  // Padding

    widget_t *textbox = widget_create_base(win, WIDGET_TEXTBOX, x, y, width, h);
    if (!textbox) return NULL;

    textbox->bg_color = COLOR_TEXTBOX_BG;
    textbox->text_color = COLOR_TEXTBOX_TEXT;
    textbox->border_color = COLOR_TEXTBOX_BORDER;

    textbox->textbox.cursor_pos = 0;
    textbox->textbox.max_length = (max_length < MAX_WIDGET_TEXT) ? max_length : MAX_WIDGET_TEXT - 1;

    kprintf("Created textbox at (%d, %d) size %dx%d\n", x, y, width, h);
    return textbox;
}

// Destroy a widget
void widget_destroy(widget_t *widget) {
    if (!widget) return;

    window_t *win = widget->parent;
    if (!win) {
        kfree(widget);
        return;
    }

    // Remove from window's widget list
    widget_t *prev = NULL;
    widget_t *curr = win->widgets;
    while (curr) {
        if (curr == widget) {
            if (prev) {
                prev->next = curr->next;
            } else {
                win->widgets = curr->next;
            }
            win->widget_count--;
            break;
        }
        prev = curr;
        curr = curr->next;
    }

    kfree(widget);
}

// Destroy all widgets in a window
void widgets_destroy_all(window_t *win) {
    if (!win) return;

    widget_t *widget = win->widgets;
    while (widget) {
        widget_t *next = widget->next;
        kfree(widget);
        widget = next;
    }

    win->widgets = NULL;
    win->widget_count = 0;
}

// Set widget text
void widget_set_text(widget_t *widget, const char *text) {
    if (!widget || !text) return;
    strncpy(widget->text, text, MAX_WIDGET_TEXT - 1);
    widget->text[MAX_WIDGET_TEXT - 1] = '\0';
}

// Set widget visibility
void widget_set_visible(widget_t *widget, bool visible) {
    if (!widget) return;
    if (visible) {
        widget->flags |= WIDGET_FLAG_VISIBLE;
    } else {
        widget->flags &= ~WIDGET_FLAG_VISIBLE;
    }
}

// Set widget enabled state
void widget_set_enabled(widget_t *widget, bool enabled) {
    if (!widget) return;
    if (enabled) {
        widget->flags |= WIDGET_FLAG_ENABLED;
    } else {
        widget->flags &= ~WIDGET_FLAG_ENABLED;
    }
}

// Set widget position
void widget_set_position(widget_t *widget, int32_t x, int32_t y) {
    if (!widget) return;
    widget->bounds.x = x;
    widget->bounds.y = y;
}

// Set widget size
void widget_set_size(widget_t *widget, int32_t width, int32_t height) {
    if (!widget) return;
    widget->bounds.width = width;
    widget->bounds.height = height;
}

// Set button click handler
void button_set_click_handler(widget_t *button, button_handler_t handler, void *user_data) {
    if (!button || button->type != WIDGET_BUTTON) return;
    button->button.on_click = handler;
    button->button.user_data = user_data;
}

// Set checkbox checked state
void checkbox_set_checked(widget_t *checkbox, bool checked) {
    if (!checkbox || checkbox->type != WIDGET_CHECKBOX) return;
    checkbox->checkbox.checked = checked;
    if (checked) {
        checkbox->flags |= WIDGET_FLAG_CHECKED;
    } else {
        checkbox->flags &= ~WIDGET_FLAG_CHECKED;
    }
}

// Get checkbox checked state
bool checkbox_is_checked(widget_t *checkbox) {
    if (!checkbox || checkbox->type != WIDGET_CHECKBOX) return false;
    return checkbox->checkbox.checked;
}

// Get textbox text
const char *textbox_get_text(widget_t *textbox) {
    if (!textbox || textbox->type != WIDGET_TEXTBOX) return "";
    return textbox->text;
}

// Set textbox text
void textbox_set_text(widget_t *textbox, const char *text) {
    if (!textbox || textbox->type != WIDGET_TEXTBOX || !text) return;
    size_t len = strlen(text);
    if (len > textbox->textbox.max_length) {
        len = textbox->textbox.max_length;
    }
    strncpy(textbox->text, text, len);
    textbox->text[len] = '\0';
    textbox->textbox.cursor_pos = len;
}

// Clear textbox
void textbox_clear(widget_t *textbox) {
    if (!textbox || textbox->type != WIDGET_TEXTBOX) return;
    textbox->text[0] = '\0';
    textbox->textbox.cursor_pos = 0;
}

// Widget state queries
bool widget_is_visible(widget_t *widget) {
    return widget && (widget->flags & WIDGET_FLAG_VISIBLE);
}

bool widget_is_enabled(widget_t *widget) {
    return widget && (widget->flags & WIDGET_FLAG_ENABLED);
}

bool widget_is_focused(widget_t *widget) {
    return widget && (widget->flags & WIDGET_FLAG_FOCUSED);
}

bool widget_is_hovered(widget_t *widget) {
    return widget && (widget->flags & WIDGET_FLAG_HOVERED);
}

// Draw label widget
void widget_draw_label(widget_t *label) {
    if (!label || !(label->flags & WIDGET_FLAG_VISIBLE)) return;

    int32_t sx, sy;
    widget_to_screen(label, &sx, &sy);

    // Use theme label text color
    widget_draw_text(sx, sy, label->text, THEME_LABEL_TEXT);
}

// Draw button widget
void widget_draw_button(widget_t *button) {
    if (!button || !(button->flags & WIDGET_FLAG_VISIBLE)) return;

    int32_t sx, sy;
    widget_to_screen(button, &sx, &sy);
    int32_t w = button->bounds.width;
    int32_t h = button->bounds.height;

    // Determine background color based on state (use theme colors)
    uint32_t bg_color = THEME_BUTTON_BG;
    if (!(button->flags & WIDGET_FLAG_ENABLED)) {
        bg_color = THEME_BUTTON_DISABLED;
    } else if (button->flags & WIDGET_FLAG_PRESSED) {
        bg_color = THEME_BUTTON_BG_PRESSED;
    } else if (button->flags & WIDGET_FLAG_HOVERED) {
        bg_color = THEME_BUTTON_BG_HOVER;
    }

    // Draw button background
    fb_fill_rect(sx, sy, w, h, bg_color);

    // Draw border (simple 3D effect)
    uint32_t light = 0x00FFFFFF;
    uint32_t dark = 0x00404040;

    if (button->flags & WIDGET_FLAG_PRESSED) {
        // Swap light and dark for pressed state
        uint32_t tmp = light;
        light = dark;
        dark = tmp;
    }

    // Top and left edges (light)
    fb_draw_line(sx, sy, sx + w - 1, sy, light);
    fb_draw_line(sx, sy, sx, sy + h - 1, light);

    // Bottom and right edges (dark)
    fb_draw_line(sx, sy + h - 1, sx + w - 1, sy + h - 1, dark);
    fb_draw_line(sx + w - 1, sy, sx + w - 1, sy + h - 1, dark);

    // Draw text (centered)
    size_t text_len = strlen(button->text);
    int32_t text_w = text_len * FONT_WIDTH;
    int32_t text_h = FONT_HEIGHT;
    int32_t tx = sx + (w - text_w) / 2;
    int32_t ty = sy + (h - text_h) / 2;

    // Offset text when pressed
    if (button->flags & WIDGET_FLAG_PRESSED) {
        tx++;
        ty++;
    }

    uint32_t text_color = (button->flags & WIDGET_FLAG_ENABLED) ?
                          THEME_BUTTON_TEXT : THEME_BUTTON_DISABLED;
    widget_draw_text(tx, ty, button->text, text_color);
}

// Draw checkbox widget
void widget_draw_checkbox(widget_t *checkbox) {
    if (!checkbox || !(checkbox->flags & WIDGET_FLAG_VISIBLE)) return;

    int32_t sx, sy;
    widget_to_screen(checkbox, &sx, &sy);

    int32_t box_size = 14;
    int32_t box_y = sy + (checkbox->bounds.height - box_size) / 2;

    // Draw checkbox box (use theme colors)
    fb_fill_rect(sx, box_y, box_size, box_size, THEME_CHECKBOX_BG);
    fb_draw_rect(sx, box_y, box_size, box_size, THEME_CHECKBOX_BORDER);

    // Draw checkmark if checked
    if (checkbox->checkbox.checked) {
        // Draw a simple checkmark
        int32_t cx = sx + 3;
        int32_t cy = box_y + box_size / 2;

        // Draw checkmark as two lines (use theme colors)
        for (int i = 0; i < 4; i++) {
            fb_put_pixel(cx + i, cy + i, THEME_CHECKBOX_CHECK);
            fb_put_pixel(cx + i, cy + i + 1, THEME_CHECKBOX_CHECK);
        }
        for (int i = 0; i < 7; i++) {
            fb_put_pixel(cx + 3 + i, cy + 3 - i, THEME_CHECKBOX_CHECK);
            fb_put_pixel(cx + 3 + i, cy + 4 - i, THEME_CHECKBOX_CHECK);
        }
    }

    // Draw label text
    int32_t tx = sx + box_size + 4;
    int32_t ty = sy + (checkbox->bounds.height - FONT_HEIGHT) / 2;
    widget_draw_text(tx, ty, checkbox->text, THEME_LABEL_TEXT);
}

// Draw textbox widget
void widget_draw_textbox(widget_t *textbox) {
    if (!textbox || !(textbox->flags & WIDGET_FLAG_VISIBLE)) return;

    int32_t sx, sy;
    widget_to_screen(textbox, &sx, &sy);
    int32_t w = textbox->bounds.width;
    int32_t h = textbox->bounds.height;

    // Draw background (use theme colors)
    fb_fill_rect(sx, sy, w, h, THEME_TEXTBOX_BG);

    // Draw border
    fb_draw_rect(sx, sy, w, h, THEME_TEXTBOX_BORDER);

    // Draw text
    int32_t tx = sx + 3;
    int32_t ty = sy + (h - FONT_HEIGHT) / 2;
    widget_draw_text(tx, ty, textbox->text, THEME_TEXTBOX_TEXT);

    // Draw cursor if focused
    if (textbox->flags & WIDGET_FLAG_FOCUSED) {
        int32_t cursor_x = tx + textbox->textbox.cursor_pos * FONT_WIDTH;
        fb_draw_line(cursor_x, sy + 2, cursor_x, sy + h - 2, THEME_TEXTBOX_CURSOR);
    }
}

// Main widget draw function
void widget_draw(widget_t *widget) {
    if (!widget) return;

    switch (widget->type) {
        case WIDGET_LABEL:
            widget_draw_label(widget);
            break;
        case WIDGET_BUTTON:
            widget_draw_button(widget);
            break;
        case WIDGET_CHECKBOX:
            widget_draw_checkbox(widget);
            break;
        case WIDGET_TEXTBOX:
            widget_draw_textbox(widget);
            break;
    }
}

// Handle mouse move on widget
void widget_handle_mouse_move(widget_t *widget, int32_t x, int32_t y) {
    if (!widget) return;

    // Update hover state
    if (rect_contains_point(&widget->bounds, x, y)) {
        widget->flags |= WIDGET_FLAG_HOVERED;
    } else {
        widget->flags &= ~WIDGET_FLAG_HOVERED;
    }
}

// Handle mouse down on widget
void widget_handle_mouse_down(widget_t *widget, int32_t x, int32_t y, uint32_t button) {
    if (!widget || !(widget->flags & WIDGET_FLAG_ENABLED)) return;
    UNUSED(x);
    UNUSED(y);
    UNUSED(button);

    widget->flags |= WIDGET_FLAG_PRESSED;

    // Handle checkbox toggle
    if (widget->type == WIDGET_CHECKBOX) {
        checkbox_set_checked(widget, !widget->checkbox.checked);
    }
}

// Handle mouse up on widget
void widget_handle_mouse_up(widget_t *widget, int32_t x, int32_t y, uint32_t button) {
    if (!widget) return;
    UNUSED(button);

    bool was_pressed = (widget->flags & WIDGET_FLAG_PRESSED) != 0;
    widget->flags &= ~WIDGET_FLAG_PRESSED;

    // Check if mouse is still over widget (click completed)
    if (was_pressed && rect_contains_point(&widget->bounds, x, y)) {
        if (widget->type == WIDGET_BUTTON && widget->button.on_click) {
            widget->button.on_click(widget, widget->button.user_data);
        }
    }
}

// Handle key input on widget
void widget_handle_key(widget_t *widget, uint32_t keycode, char key_char) {
    if (!widget || !(widget->flags & WIDGET_FLAG_ENABLED)) return;
    UNUSED(keycode);

    // Handle textbox input
    if (widget->type == WIDGET_TEXTBOX) {
        size_t len = strlen(widget->text);

        if (key_char == '\b') {
            // Backspace
            if (widget->textbox.cursor_pos > 0) {
                widget->textbox.cursor_pos--;
                // Shift characters left
                for (size_t i = widget->textbox.cursor_pos; i < len; i++) {
                    widget->text[i] = widget->text[i + 1];
                }
            }
        } else if (key_char >= ' ' && key_char <= '~') {
            // Printable character
            if (len < widget->textbox.max_length) {
                // Shift characters right
                for (size_t i = len + 1; i > widget->textbox.cursor_pos; i--) {
                    widget->text[i] = widget->text[i - 1];
                }
                widget->text[widget->textbox.cursor_pos] = key_char;
                widget->textbox.cursor_pos++;
            }
        }
    }
}

// Find widget at point (window-relative coordinates)
widget_t *widget_find_at_point(window_t *win, int32_t x, int32_t y) {
    if (!win) return NULL;

    widget_t *widget = win->widgets;
    while (widget) {
        if ((widget->flags & WIDGET_FLAG_VISIBLE) &&
            rect_contains_point(&widget->bounds, x, y)) {
            return widget;
        }
        widget = widget->next;
    }

    return NULL;
}
