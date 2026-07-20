// taskbar.h - Taskbar for Userland Compositor
#ifndef TASKBAR_H
#define TASKBAR_H

#include <stdint.h>
#include <stdbool.h>
#include "compositor.h"

// ============================================================================
// Constants
// ============================================================================

#define TASKBAR_HEIGHT          48
#define TASKBAR_BUTTON_WIDTH    150
#define TASKBAR_BUTTON_HEIGHT   38
#define TASKBAR_PADDING         5
#define START_BUTTON_WIDTH      80
#define SYSTRAY_WIDTH           150
#define CLOCK_WIDTH             80

// ============================================================================
// Start Menu Items
// ============================================================================

#define MAX_START_ITEMS         32

typedef struct {
    char name[64];
    char exec_path[256];
    uint32_t icon_id;
    bool is_separator;
} start_menu_item_t;

// ============================================================================
// Taskbar State
// ============================================================================

typedef struct {
    // Position
    int32_t x, y;
    int32_t width, height;
    
    // Colors
    uint32_t bg_color;
    uint32_t button_color;
    uint32_t button_hover_color;
    uint32_t button_active_color;
    uint32_t text_color;
    
    // Start menu
    bool start_menu_open;
    start_menu_item_t start_items[MAX_START_ITEMS];
    int start_item_count;
    int start_hover_item;
    
    // Window buttons
    int hover_button;       // Index of button being hovered (-1 = none)
    
    // System tray
    uint64_t last_clock_update;
    char clock_text[16];
    
    // Network status
    bool network_connected;
} taskbar_state_t;

// ============================================================================
// API
// ============================================================================

// Initialize taskbar
int taskbar_init(void);

// Shutdown taskbar
void taskbar_shutdown(void);

// Update taskbar (clock, etc.)
void taskbar_update(void);

// Render taskbar to framebuffer
void taskbar_render(uint32_t *fb, int32_t fb_width, int32_t fb_height);

// Handle mouse input
// Returns true if event was consumed
bool taskbar_handle_mouse(int32_t x, int32_t y, uint32_t buttons);

// Toggle start menu
void taskbar_toggle_start_menu(void);

// Add item to start menu
int taskbar_add_start_item(const char *name, const char *exec_path, uint32_t icon_id);

// Get taskbar height (for window placement)
int32_t taskbar_get_height(void);

#endif // TASKBAR_H
