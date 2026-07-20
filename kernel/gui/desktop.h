// desktop.h - Desktop manager and dock for MayteraOS
#ifndef DESKTOP_H
#define DESKTOP_H

#include "../types.h"
#include "image.h"  // Use image_t from image.h

// Maximum number of dock apps
#define DOCK_MAX_APPS       16
#define DOCK_APP_NAME_LEN   32

// Desktop icon configuration
#define DESKTOP_ICON_MAX        16
#define DESKTOP_ICON_NAME_LEN   32
#define DESKTOP_ICON_SIZE       48      // Icon image size
#define DESKTOP_ICON_SPACING_X  100     // Horizontal spacing between icons
#define DESKTOP_ICON_SPACING_Y  90      // Vertical spacing between icons
#define DESKTOP_ICON_MARGIN_X   20      // Left margin from screen edge
#define DESKTOP_ICON_MARGIN_Y   20      // Top margin from screen edge
#define DESKTOP_ICON_LABEL_H    32      // Height reserved for label text

// Taskbar dimensions (Windows-style)
#define TASKBAR_HEIGHT      36
#define TASKBAR_PADDING     4
#define TASKBAR_ICON_SIZE   24
#define TASKBAR_ICON_SPACING 4
#define START_BTN_SIZE      28      // Square start button with icon
#define START_BTN_WIDTH     (START_BTN_SIZE + TASKBAR_PADDING * 2)  // For layout
#define PROCESS_BTN_SIZE    28      // Square app buttons
#define GAUGE_WIDTH         80
#define GAUGE_SPACING       4

// Legacy dock dimensions (for compatibility)
#define DOCK_ICON_SIZE      TASKBAR_ICON_SIZE
#define DOCK_ICON_SPACING   TASKBAR_ICON_SPACING
#define DOCK_HEIGHT         TASKBAR_HEIGHT
#define DOCK_PADDING        TASKBAR_PADDING
#define DOCK_LABEL_HEIGHT   20
#define DOCK_CORNER_RADIUS  0   // Square edges

// Taskbar colors (0xAARRGGBB format) - Modern grey/white theme
#define TASKBAR_BG_COLOR    0xFF2D2D2D  // Dark grey background
#define TASKBAR_HOVER_COLOR 0xFF3D3D3D  // Slightly lighter for hover
#define TASKBAR_ACTIVE_COLOR 0xFF4A4A4A // Active process
#define START_BTN_COLOR     0xFF3A3A3A  // Dark grey start button (not green)
#define GAUGE_BG_COLOR      0xFF1A1A1A  // Very dark gauge background
#define GAUGE_FG_COLOR      0xFF6A6A6A  // Medium grey gauge fill

// Legacy dock color (for compatibility)
#define DOCK_BG_COLOR       TASKBAR_BG_COLOR
#define DOCK_ICON_TERMINAL  0xFF2D2D2D
#define DOCK_ICON_FILES     0xFF3498DB
#define DOCK_ICON_NOTES     0xFFF39C12
#define DOCK_ICON_CALC      0xFF9B59B6
#define DOCK_ICON_SETTINGS  0xFF7F8C8D
#define DOCK_ICON_ABOUT     0xFF1ABC9C

// Default desktop background color
#define DESKTOP_BG_COLOR    0xFF2E5A88  // Pleasant blue

// Desktop icon entry (icons on desktop surface)
typedef struct desktop_icon {
    char name[DESKTOP_ICON_NAME_LEN];   // Icon label
    int icon_id;                         // Icon ID from icons.h
    void (*launch)(void);                // Launch callback function
    bool active;                         // Is this slot in use?
    int32_t grid_x;                      // Grid column (0-based)
    int32_t grid_y;                      // Grid row (0-based)
    bool selected;                       // Is icon selected?
} desktop_icon_t;

// Dock application entry
typedef struct dock_app {
    char name[DOCK_APP_NAME_LEN];   // App display name
    uint32_t icon_color;             // Icon color (fallback)
    int icon_id;                     // Icon ID from icons.h (-1 for none)
    void (*launch)(void);            // Launch callback function
    bool active;                     // Is this slot in use?
    int32_t x;                       // Calculated x position on dock
    int32_t y;                       // Calculated y position on dock
} dock_app_t;

// Dock state
typedef struct dock {
    dock_app_t apps[DOCK_MAX_APPS];  // Application slots
    uint32_t app_count;              // Number of apps in dock
    int32_t x;                       // Dock x position (top-left)
    int32_t y;                       // Dock y position (top-left)
    uint32_t width;                  // Dock width (calculated)
    uint32_t height;                 // Dock height
    int32_t hover_index;             // Currently hovered app (-1 if none)
    bool visible;                    // Is dock visible?
} dock_t;

// Desktop state
typedef struct desktop {
    uint32_t bg_color;               // Background color
    image_t *bg_image;               // Background image (NULL if none)
    dock_t dock;                     // The dock
    desktop_icon_t icons[DESKTOP_ICON_MAX];  // Desktop icons
    uint32_t icon_count;             // Number of active desktop icons
    uint32_t screen_width;           // Screen width
    uint32_t screen_height;          // Screen height
    bool initialized;                // Is desktop initialized?
} desktop_t;

// ============================================================================
// Desktop API
// ============================================================================

// Initialize the desktop manager
void desktop_init(void);

// Run the desktop main loop (handles mouse, keyboard, redraws)
// Returns when user exits (ESC or Ctrl+C)
void desktop_run(void);

// Draw the entire desktop (background + dock)
void desktop_draw(void);

// Set background color (ARGB format)
void desktop_set_background_color(uint32_t color);

// Set background image (pass NULL to clear)
void desktop_set_background_image(image_t *img);

// Handle a click event at screen coordinates
void desktop_handle_click(int x, int y);

// Handle right-click (context menu)
void desktop_handle_right_click(int x, int y);

// Handle mouse movement for hover effects
void desktop_handle_mouse_move(int x, int y);

// Draw mouse cursor at position
void desktop_draw_cursor(int32_t x, int32_t y);

// ============================================================================
// Dock API
// ============================================================================

// Add an application to the dock with icon
// Returns index of added app, or -1 on failure
int dock_add_app_with_icon(const char *name, uint32_t color, int icon_id, void (*launch)(void));

// Add an application to the dock (auto-assigns icon based on name)
// Returns index of added app, or -1 on failure
int dock_add_app(const char *name, uint32_t color, void (*launch)(void));

// Remove an application from the dock by index
void dock_remove_app(int index);

// Clear all apps from the dock
void dock_clear(void);

// Draw just the dock
void dock_draw(void);

// Check if a point is within the dock bounds
bool dock_hit_test(int x, int y);

// Get the dock app index at a point, or -1 if none
int dock_get_app_at(int x, int y);

// Get desktop state (for external access)
desktop_t *desktop_get_state(void);

// ============================================================================
// Desktop Icons API
// ============================================================================

// Add a desktop icon
// Returns index of added icon, or -1 on failure
int desktop_add_icon(const char *name, int icon_id, int grid_x, int grid_y, void (*launch)(void));

// Remove a desktop icon by index
void desktop_remove_icon(int index);

// Draw desktop icons
void desktop_draw_icons(void);

// Get desktop icon at screen coordinates, or -1 if none
int desktop_get_icon_at(int x, int y);

// Handle double-click on desktop icon
void desktop_icon_activate(int index);


// Session user identity (multi-user model)
void desktop_set_session(uint32_t uid, uint32_t gid);
uint32_t desktop_get_session_uid(void);
uint32_t desktop_get_session_gid(void);

#endif // DESKTOP_H
