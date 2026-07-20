// mouse.h - PS/2 Mouse driver
#ifndef MOUSE_H
#define MOUSE_H

#include "../types.h"

// Mouse button states
#define MOUSE_LEFT_BTN   0x01
#define MOUSE_RIGHT_BTN  0x02
#define MOUSE_MIDDLE_BTN 0x04

// Mouse state structure
typedef struct {
    int32_t x;          // Current X position
    int32_t y;          // Current Y position
    int32_t dx;         // Delta X since last read
    int32_t dy;         // Delta Y since last read
    int8_t  scroll;     // Scroll wheel delta (positive=up, negative=down)
    uint8_t buttons;    // Button state
    uint8_t prev_buttons; // Previous button state
} mouse_state_t;

// Initialize PS/2 mouse
int mouse_init(void);

// Get current mouse state
void mouse_get_state(mouse_state_t *state);

// Get current mouse state and clear deltas atomically
void mouse_get_state_and_clear(mouse_state_t *state);

// Get mouse position
void mouse_get_position(int32_t *x, int32_t *y);

// Set mouse position (for bounds clamping)
void mouse_set_position(int32_t x, int32_t y);

// Set mouse bounds
void mouse_set_bounds(int32_t min_x, int32_t min_y, int32_t max_x, int32_t max_y);
void mouse_set_sensitivity(int s);  // 1=slow, 5=normal, 10=fast
int  mouse_get_sensitivity(void);

// Check if button is pressed
int mouse_button_pressed(uint8_t button);

// Get current button state (raw value)
uint8_t mouse_get_buttons(void);

// Check if button was just clicked (transition from up to down)
int mouse_button_clicked(uint8_t button);

// Check if button was just released
int mouse_button_released(uint8_t button);

// Get scroll wheel delta (positive=up, negative=down)
// Returns delta and clears it
int8_t mouse_get_scroll(void);

// Check if scroll wheel is supported
int mouse_has_scroll_wheel(void);

// #307: inject a USB-HID boot mouse report (relative dx/dy, button bitmap, wheel)
void mouse_inject_hid(int dx, int dy, uint8_t buttons, int wheel);

// Poll mouse (called from interrupt or polling loop)
void mouse_poll(void);

// Check if mouse data available
int mouse_has_data(void);

#endif // MOUSE_H
