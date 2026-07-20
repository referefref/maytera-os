// calculator.c - Calculator GUI application for MayteraOS
#include "calculator.h"
#include "window.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../drivers/mouse.h"
#include "../cpu/isr.h"
#include "desktop.h"
#include "syslog.h"

// Button colors
#define BTN_COLOR_DIGIT     0x00404040  // Dark gray for digits
#define BTN_COLOR_OP        0x00FF8C00  // Orange for operations
#define BTN_COLOR_CLEAR     0x00FF4040  // Red for clear
#define BTN_COLOR_EQUALS    0x0040A040  // Green for equals
#define BTN_COLOR_HOVER     0x00606060  // Hover highlight
#define BTN_COLOR_TEXT      0x00FFFFFF  // White text

// Display colors
#define DISPLAY_BG          0x00202020  // Dark background
#define DISPLAY_FG          0x00FFFFFF  // White text

// Button layout (4 columns x 4 rows)
// Row 1: 7 8 9 /
// Row 2: 4 5 6 *
// Row 3: 1 2 3 -
// Row 4: C 0 = +
static calc_button_t g_buttons[16] = {
    // Row 1
    { 10, 60, 40, 35, "7", BTN_COLOR_DIGIT },
    { 55, 60, 40, 35, "8", BTN_COLOR_DIGIT },
    { 100, 60, 40, 35, "9", BTN_COLOR_DIGIT },
    { 145, 60, 40, 35, "/", BTN_COLOR_OP },
    // Row 2
    { 10, 100, 40, 35, "4", BTN_COLOR_DIGIT },
    { 55, 100, 40, 35, "5", BTN_COLOR_DIGIT },
    { 100, 100, 40, 35, "6", BTN_COLOR_DIGIT },
    { 145, 100, 40, 35, "*", BTN_COLOR_OP },
    // Row 3
    { 10, 140, 40, 35, "1", BTN_COLOR_DIGIT },
    { 55, 140, 40, 35, "2", BTN_COLOR_DIGIT },
    { 100, 140, 40, 35, "3", BTN_COLOR_DIGIT },
    { 145, 140, 40, 35, "-", BTN_COLOR_OP },
    // Row 4
    { 10, 180, 40, 35, "C", BTN_COLOR_CLEAR },
    { 55, 180, 40, 35, "0", BTN_COLOR_DIGIT },
    { 100, 180, 40, 35, "=", BTN_COLOR_EQUALS },
    { 145, 180, 40, 35, "+", BTN_COLOR_OP }
};

// Convert integer to string (handles negative numbers)
static void calc_itoa(int64_t num, char *buf, int max_len) {
    bool negative = false;
    char tmp[24];
    int i = 0;

    if (num < 0) {
        negative = true;
        num = -num;
    }

    // Handle zero
    if (num == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    // Convert digits
    while (num > 0 && i < 20) {
        tmp[i++] = '0' + (num % 10);
        num /= 10;
    }

    // Reverse and add sign
    int j = 0;
    if (negative && j < max_len - 1) {
        buf[j++] = '-';
    }
    while (i > 0 && j < max_len - 1) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

// Draw a single character at position
static void calc_draw_char(int32_t x, int32_t y, char c, uint32_t color) {
    const uint8_t *glyph = font_get_glyph(c);
    if (!glyph) return;

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                fb_put_pixel(x + col, y + row, color);
            }
        }
    }
}

// Draw text at position
static void calc_draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    while (*text) {
        calc_draw_char(x, y, *text, color);
        x += FONT_WIDTH;
        text++;
    }
}

// Get string width in pixels
static uint32_t calc_string_width(const char *str) {
    uint32_t len = 0;
    while (*str++) len++;
    return len * FONT_WIDTH;
}

// Draw the display area
static void calc_draw_display(calculator_t *calc) {
    if (!calc || !calc->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(calc->window, &wx, &wy, &ww, &wh);

    // Draw display background
    fb_fill_rect(wx + CALC_DISPLAY_X, wy + CALC_DISPLAY_Y,
                 CALC_DISPLAY_W, CALC_DISPLAY_H, DISPLAY_BG);

    // Draw border
    fb_draw_rect(wx + CALC_DISPLAY_X, wy + CALC_DISPLAY_Y,
                 CALC_DISPLAY_W, CALC_DISPLAY_H, 0x00808080);

    // Draw display text (right-aligned)
    uint32_t text_w = calc_string_width(calc->display);
    int32_t text_x = wx + CALC_DISPLAY_X + CALC_DISPLAY_W - text_w - 8;
    int32_t text_y = wy + CALC_DISPLAY_Y + (CALC_DISPLAY_H - FONT_HEIGHT) / 2;

    calc_draw_text(text_x, text_y, calc->display, DISPLAY_FG);
}

// Draw a single button
static void calc_draw_button(calculator_t *calc, calc_button_t *btn, bool hovered) {
    if (!calc || !calc->window || !btn) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(calc->window, &wx, &wy, &ww, &wh);

    int32_t bx = wx + btn->x;
    int32_t by = wy + btn->y;

    // Draw button background with hover effect
    uint32_t bg_color = hovered ? BTN_COLOR_HOVER : btn->color;
    fb_fill_rect(bx, by, btn->w, btn->h, bg_color);

    // Draw button border (3D effect)
    // Top and left edges (lighter)
    uint32_t light = 0x00808080;
    fb_fill_rect(bx, by, btn->w, 2, light);
    fb_fill_rect(bx, by, 2, btn->h, light);

    // Bottom and right edges (darker)
    uint32_t dark = 0x00202020;
    fb_fill_rect(bx, by + btn->h - 2, btn->w, 2, dark);
    fb_fill_rect(bx + btn->w - 2, by, 2, btn->h, dark);

    // Draw button label (centered)
    uint32_t text_w = calc_string_width(btn->label);
    int32_t text_x = bx + (btn->w - text_w) / 2;
    int32_t text_y = by + (btn->h - FONT_HEIGHT) / 2;

    calc_draw_text(text_x, text_y, btn->label, BTN_COLOR_TEXT);
}

// Draw all buttons
static void calc_draw_buttons(calculator_t *calc, int hover_index) {
    for (int i = 0; i < 16; i++) {
        calc_draw_button(calc, &g_buttons[i], (i == hover_index));
    }
}

// Find button at point (content-relative coordinates)
static int calc_button_at_point(int32_t x, int32_t y) {
    for (int i = 0; i < 16; i++) {
        calc_button_t *btn = &g_buttons[i];
        rect_t btn_rect = { btn->x, btn->y, btn->w, btn->h };
        if (rect_contains_point(&btn_rect, x, y)) {
            return i;
        }
    }
    return -1;
}

// Redraw the calculator (internal helper)
static void calc_redraw(calculator_t *calc, int hover_index) {
    if (!calc || !calc->window) return;

    // Draw the window frame
    window_draw(calc->window);

    // Draw calculator content
    calc_draw_display(calc);
    calc_draw_buttons(calc, hover_index);
}

// Public draw function (called by window manager)
void calculator_draw(calculator_t *calc) {
    if (!calc) return;
    calc_redraw(calc, calc->hover_index);
}

// Update the display string from current value
void calculator_update_display(calculator_t *calc) {
    if (!calc) return;
    calc_itoa(calc->current_value, calc->display, CALC_MAX_DIGITS + 1);
}

// Handle a button press
void calculator_handle_button(calculator_t *calc, const char *label) {
    if (!calc || !label) return;

    char c = label[0];

    // Handle digits
    if (c >= '0' && c <= '9') {
        int digit = c - '0';

        if (calc->new_input) {
            calc->current_value = digit;
            calc->new_input = false;
        } else {
            // Prevent overflow
            if (calc->current_value < 999999999999LL && calc->current_value > -999999999999LL) {
                calc->current_value = calc->current_value * 10 + (calc->current_value < 0 ? -digit : digit);
            }
        }
        calculator_update_display(calc);
        return;
    }

    // Handle clear
    if (c == 'C') {
        calc->current_value = 0;
        calc->stored_value = 0;
        calc->pending_op = CALC_OP_NONE;
        calc->new_input = true;
        calculator_update_display(calc);
        return;
    }

    // Handle equals
    if (c == '=') {
        if (calc->pending_op != CALC_OP_NONE) {
            int64_t result = calc->stored_value;

            switch (calc->pending_op) {
                case CALC_OP_ADD:
                    result += calc->current_value;
                    break;
                case CALC_OP_SUB:
                    result -= calc->current_value;
                    break;
                case CALC_OP_MUL:
                    result *= calc->current_value;
                    break;
                case CALC_OP_DIV:
                    if (calc->current_value != 0) {
                        result /= calc->current_value;
                    } else {
                        // Division by zero - just keep current value
                        result = 0;
                    }
                    break;
                default:
                    break;
            }

            calc->current_value = result;
            calc->pending_op = CALC_OP_NONE;
            calc->new_input = true;
            calculator_update_display(calc);
        }
        return;
    }

    // Handle operations
    calc_op_t new_op = CALC_OP_NONE;
    switch (c) {
        case '+': new_op = CALC_OP_ADD; break;
        case '-': new_op = CALC_OP_SUB; break;
        case '*': new_op = CALC_OP_MUL; break;
        case '/': new_op = CALC_OP_DIV; break;
    }

    if (new_op != CALC_OP_NONE) {
        // If we have a pending operation, compute it first
        if (calc->pending_op != CALC_OP_NONE && !calc->new_input) {
            // Compute pending operation
            int64_t result = calc->stored_value;
            switch (calc->pending_op) {
                case CALC_OP_ADD:
                    result += calc->current_value;
                    break;
                case CALC_OP_SUB:
                    result -= calc->current_value;
                    break;
                case CALC_OP_MUL:
                    result *= calc->current_value;
                    break;
                case CALC_OP_DIV:
                    if (calc->current_value != 0) {
                        result /= calc->current_value;
                    }
                    break;
                default:
                    break;
            }
            calc->current_value = result;
            calculator_update_display(calc);
        }

        calc->stored_value = calc->current_value;
        calc->pending_op = new_op;
        calc->new_input = true;
    }
}

// Create calculator window
calculator_t *calculator_create(void) {
    calculator_t *calc = (calculator_t *)kmalloc(sizeof(calculator_t));
    if (!calc) {
        kprintf("[Calc] Failed to allocate calculator\n");
        return NULL;
    }

    memset(calc, 0, sizeof(calculator_t));

    // Center window on screen
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();
    int32_t x = (screen_w - CALC_WIDTH) / 2;
    int32_t y = (screen_h - CALC_HEIGHT) / 2 - 50;

    // Create window
    calc->window = window_create("Calculator", x, y, CALC_WIDTH, CALC_HEIGHT);
    if (!calc->window) {
        kprintf("[Calc] Failed to create window\n");
        kfree(calc);
        return NULL;
    }

    // Set window background color
    calc->window->bg_color = 0x00303030;
    // Calculator has fixed layout, disable resizing
    calc->window->flags &= ~WINDOW_FLAG_RESIZABLE;

    // Initialize state
    calc->current_value = 0;
    calc->stored_value = 0;
    calc->pending_op = CALC_OP_NONE;
    calc->new_input = true;
    calc->running = true;

    // Initialize display
    calc->display[0] = '0';
    calc->display[1] = '\0';

    kprintf("[Calc] Calculator created\n");
    return calc;
}

// Destroy calculator
void calculator_destroy(calculator_t *calc) {
    if (!calc) return;

    if (calc->window) {
        window_destroy(calc->window);
    }
    kfree(calc);

    kprintf("[Calc] Calculator destroyed\n");
}

// Run calculator main loop
void calculator_run(calculator_t *calc) {
    if (!calc) return;

    kprintf("[Calc] Running calculator...\n");

    // Initial draw
    calc_redraw(calc, -1);

    // Track mouse state
    int32_t last_mouse_x = 0, last_mouse_y = 0;
    uint8_t last_buttons = 0;
    int hover_index = -1;
    mouse_get_position(&last_mouse_x, &last_mouse_y);

    while (calc->running) {
        // Poll mouse
        mouse_poll();

        int32_t mouse_x, mouse_y;
        uint8_t buttons;
        mouse_get_position(&mouse_x, &mouse_y);
        buttons = mouse_get_buttons();

        // Get content area bounds
        int32_t wx, wy, ww, wh;
        window_get_content_bounds(calc->window, &wx, &wy, &ww, &wh);

        // Convert to content-relative coordinates
        int32_t local_x = mouse_x - wx;
        int32_t local_y = mouse_y - wy;

        // Check for window dragging on title bar click
        if ((buttons & MOUSE_LEFT_BTN) && !(last_buttons & MOUSE_LEFT_BTN)) {
            wm_handle_mouse_down(mouse_x, mouse_y, MOUSE_LEFT_BTN);
        }

        // Handle mouse movement
        if (mouse_x != last_mouse_x || mouse_y != last_mouse_y) {
            wm_handle_mouse_move(mouse_x, mouse_y);

            // Check hover over buttons
            int new_hover = calc_button_at_point(local_x, local_y);
            hover_index = new_hover;

            // Always redraw when mouse moves to prevent cursor trails
            desktop_draw();
            calc_redraw(calc, hover_index);
            desktop_draw_cursor(mouse_x, mouse_y);

            last_mouse_x = mouse_x;
            last_mouse_y = mouse_y;
        }

        // Handle mouse button release (stop dragging)
        if (!(buttons & MOUSE_LEFT_BTN) && (last_buttons & MOUSE_LEFT_BTN)) {
            wm_handle_mouse_up(mouse_x, mouse_y, MOUSE_LEFT_BTN);

            // Check if released on a button
            int btn_index = calc_button_at_point(local_x, local_y);
            if (btn_index >= 0) {
                calculator_handle_button(calc, g_buttons[btn_index].label);
                calc_redraw(calc, hover_index);
            }
        }

        last_buttons = buttons;

        // Check for keyboard input
        if (keyboard_has_char()) {
            char c = keyboard_get_char();

            if (c == 27) {  // ESC - close calculator
                calc->running = false;
                break;
            }

            // Handle keyboard input for calculator
            if (c >= '0' && c <= '9') {
                char label[2] = { c, '\0' };
                calculator_handle_button(calc, label);
                calc_redraw(calc, hover_index);
            } else if (c == '+' || c == '-' || c == '*' || c == '/') {
                char label[2] = { c, '\0' };
                calculator_handle_button(calc, label);
                calc_redraw(calc, hover_index);
            } else if (c == '=' || c == '\n' || c == '\r') {
                calculator_handle_button(calc, "=");
                calc_redraw(calc, hover_index);
            } else if (c == 'c' || c == 'C') {
                calculator_handle_button(calc, "C");
                calc_redraw(calc, hover_index);
            }
        }

        // Draw mouse cursor
        static const uint8_t cursor[] = {
            0b10000000, 0b00000000, 0b11000000, 0b00000000,
            0b11100000, 0b00000000, 0b11110000, 0b00000000,
            0b11111000, 0b00000000, 0b11111100, 0b00000000,
            0b11111110, 0b00000000, 0b11111111, 0b00000000,
            0b11111111, 0b10000000, 0b11111111, 0b11000000,
            0b11111100, 0b00000000, 0b11101100, 0b00000000,
            0b11000110, 0b00000000, 0b10000110, 0b00000000,
            0b00000011, 0b00000000, 0b00000011, 0b00000000,
        };
        for (int row = 0; row < 16; row++) {
            uint16_t bits = (cursor[row*2] << 8) | cursor[row*2 + 1];
            for (int col = 0; col < 12; col++) {
                if (bits & (0x8000 >> col)) {
                    fb_put_pixel(mouse_x + col, mouse_y + row, 0xFFFFFF);
                    fb_put_pixel(mouse_x + col + 1, mouse_y + row, 0x000000);
                }
            }
        }

        // Small delay
        for (int i = 0; i < 1000; i++) {
            __asm__ volatile("pause");
        }
    }

    kprintf("[Calc] Calculator closed\n");
}

// ============================================================================
// Window Manager Callback Functions (non-blocking model)
// ============================================================================

// Event callback - handles mouse/keyboard events from window manager
void calculator_on_event(void *app_data, gui_event_t *event) {
    calculator_t *calc = (calculator_t *)app_data;
    if (!calc || !calc->window || !event) return;

    // Get content area bounds for coordinate conversion
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(calc->window, &wx, &wy, &ww, &wh);
    int32_t local_x = event->mouse_x - wx;
    int32_t local_y = event->mouse_y - wy;

    switch (event->type) {
        case EVENT_MOUSE_MOVE:
            // Update hover state
            {
                int new_hover = calc_button_at_point(local_x, local_y);
                if (new_hover != calc->hover_index) {
                    calc->hover_index = new_hover;
                    wm_invalidate_rect(&calc->window->bounds);
                }
            }
            break;

        case EVENT_MOUSE_UP:
            // Check if released on a calculator button
            if (event->mouse_buttons & MOUSE_BUTTON_LEFT) {
                int btn_index = calc_button_at_point(local_x, local_y);
                if (btn_index >= 0) {
                    calculator_handle_button(calc, g_buttons[btn_index].label);
                    wm_invalidate_rect(&calc->window->bounds);
                }
            }
            break;

        case EVENT_KEY_DOWN:
            {
                char c = event->key_char;

                // ESC closes the calculator
                if (c == 27) {
                    kprintf("[Calc] ESC pressed, closing calculator\n");
                    // Unregister from window manager
                    wm_unregister_app(calc->app_id);
                    // Remove from dock
                    if (calc->dock_index >= 0) {
                        dock_remove_app(calc->dock_index);
                    }
                    // Hide and destroy window
                    window_hide(calc->window);
                    wm_invalidate_all();
                    return;
                }

                // Handle digit keys
                if (c >= '0' && c <= '9') {
                    char label[2] = { c, '\0' };
                    calculator_handle_button(calc, label);
                    wm_invalidate_rect(&calc->window->bounds);
                }
                // Handle operator keys
                else if (c == '+' || c == '-' || c == '*' || c == '/') {
                    char label[2] = { c, '\0' };
                    calculator_handle_button(calc, label);
                    wm_invalidate_rect(&calc->window->bounds);
                }
                // Handle equals (Enter key)
                else if (c == '=' || c == '\n' || c == '\r') {
                    calculator_handle_button(calc, "=");
                    wm_invalidate_rect(&calc->window->bounds);
                }
                // Handle clear
                else if (c == 'c' || c == 'C') {
                    calculator_handle_button(calc, "C");
                    wm_invalidate_rect(&calc->window->bounds);
                }
            }
            break;

        case EVENT_WINDOW_CLOSE:
            kprintf("[Calc] Close button clicked\n");
            // Unregister from window manager
            wm_unregister_app(calc->app_id);
            // Remove from dock
            if (calc->dock_index >= 0) {
                dock_remove_app(calc->dock_index);
            }
            // Hide window (destroy will happen in on_destroy)
            window_hide(calc->window);
            wm_invalidate_all();
            break;

        default:
            break;
    }
}

// Draw callback - redraws the calculator
void calculator_on_draw(void *app_data) {
    calculator_t *calc = (calculator_t *)app_data;
    if (calc) {
        calculator_draw(calc);
    }
}

// Destroy callback - cleans up resources
void calculator_on_destroy(void *app_data) {
    calculator_t *calc = (calculator_t *)app_data;
    if (calc) {
        kprintf("[Calc] Destroying calculator instance\n");
        calculator_destroy(calc);
    }
}

// Launch callback for dock (non-blocking, registers with window manager)
void calculator_launch(void) {
    LOG_INFO("[Calculator] Application launched");
    kprintf("[Calc] Launching calculator (non-blocking)...\n");

    calculator_t *calc = calculator_create();
    if (!calc) {
        LOG_ERROR("[Calculator] Failed to create window");
        kprintf("[Calc] Failed to create calculator\n");
        return;
    }

    // Initialize new fields
    calc->app_id = -1;
    calc->dock_index = -1;
    calc->hover_index = -1;

    // Add to taskbar as active process
    calc->dock_index = dock_add_app("Calculator", DOCK_ICON_CALC, NULL);

    // Register with window manager (non-blocking)
    calc->app_id = wm_register_app(
        calc->window,
        calc,
        calculator_on_event,
        calculator_on_draw,
        calculator_on_destroy
    );

    if (calc->app_id < 0) {
        kprintf("[Calc] Failed to register with window manager\n");
        if (calc->dock_index >= 0) {
            dock_remove_app(calc->dock_index);
        }
        calculator_destroy(calc);
        return;
    }

    // Mark screen as dirty to show the new window
    wm_invalidate_all();

    kprintf("[Calc] Calculator registered as app %d\n", calc->app_id);
    // Control returns immediately - window manager handles events
}
