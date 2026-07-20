// calculator.h - Calculator GUI application for MayteraOS
#ifndef CALCULATOR_H
#define CALCULATOR_H

#include "../types.h"
#include "window.h"

// Calculator window dimensions
#define CALC_WIDTH      200
#define CALC_HEIGHT     300

// Display area
#define CALC_DISPLAY_X      10
#define CALC_DISPLAY_Y      10
#define CALC_DISPLAY_W      180
#define CALC_DISPLAY_H      40

// Button dimensions
#define CALC_BTN_W          40
#define CALC_BTN_H          35
#define CALC_BTN_SPACING    5
#define CALC_BTN_START_Y    60

// Maximum display digits
#define CALC_MAX_DIGITS     12

// Calculator operations
typedef enum {
    CALC_OP_NONE = 0,
    CALC_OP_ADD,
    CALC_OP_SUB,
    CALC_OP_MUL,
    CALC_OP_DIV
} calc_op_t;

// Calculator state
typedef struct {
    window_t *window;           // Calculator window
    int64_t current_value;      // Current displayed value
    int64_t stored_value;       // Value stored for operation
    calc_op_t pending_op;       // Pending operation
    bool new_input;             // Next digit starts new number
    bool running;               // Is calculator running (for legacy mode)
    char display[CALC_MAX_DIGITS + 2];  // Display buffer (+1 for minus, +1 for null)
    int app_id;                 // Window manager app registration ID
    int dock_index;             // Dock/taskbar index for this instance
    int hover_index;            // Currently hovered button (-1 if none)
} calculator_t;

// Button definition
typedef struct {
    int32_t x;          // X position in content area
    int32_t y;          // Y position in content area
    int32_t w;          // Width
    int32_t h;          // Height
    char label[4];      // Button label
    uint32_t color;     // Button background color
} calc_button_t;

// Create calculator window
calculator_t *calculator_create(void);

// Destroy calculator
void calculator_destroy(calculator_t *calc);

// Run calculator main loop (legacy blocking mode)
void calculator_run(calculator_t *calc);

// Handle button press
void calculator_handle_button(calculator_t *calc, const char *label);

// Update display
void calculator_update_display(calculator_t *calc);

// Draw calculator content (called by window manager)
void calculator_draw(calculator_t *calc);

// Launch callback for dock (non-blocking, registers with WM)
void calculator_launch(void);

// ============================================================================
// Window Manager Callbacks (internal use)
// ============================================================================

// Event callback - handles mouse/keyboard events
void calculator_on_event(void *app_data, gui_event_t *event);

// Draw callback - redraws the calculator
void calculator_on_draw(void *app_data);

// Destroy callback - cleans up resources
void calculator_on_destroy(void *app_data);

#endif // CALCULATOR_H
