// desktop.h - Desktop Manager for Userland Compositor
#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Desktop Icons
// ============================================================================

#define MAX_DESKTOP_ICONS       64
#define ICON_WIDTH              48
#define ICON_HEIGHT             48
#define ICON_SPACING            20
#define ICON_LABEL_HEIGHT       20

typedef struct {
    uint32_t id;
    char name[64];              // Display name
    char exec_path[256];        // Path to executable
    int32_t x, y;               // Position on desktop
    uint32_t *icon_data;        // Icon bitmap (48x48 ARGB)
    bool selected;              // Is selected
    bool visible;               // Is visible
} desktop_icon_t;

// ============================================================================
// Desktop State
// ============================================================================

typedef struct {
    // Wallpaper
    uint32_t *wallpaper;        // Wallpaper bitmap
    int32_t wp_width, wp_height;
    uint32_t bg_color;          // Solid color if no wallpaper
    
    // Icons
    desktop_icon_t icons[MAX_DESKTOP_ICONS];
    int icon_count;
    int selected_icon;          // Currently selected icon (-1 = none)
    
    // Double-click tracking
    uint32_t last_click_icon;
    uint64_t last_click_time;
    
    // Selection rectangle
    bool selecting;
    int32_t sel_start_x, sel_start_y;
    int32_t sel_end_x, sel_end_y;
} desktop_state_t;

// ============================================================================
// API
// ============================================================================

// Initialize desktop
int desktop_init(void);

// Shutdown desktop
void desktop_shutdown(void);

// Update desktop (animations, etc.)
void desktop_update(void);

// Render desktop to framebuffer
void desktop_render(uint32_t *fb, int32_t width, int32_t height);

// Handle mouse input
// Returns true if event was consumed
bool desktop_handle_mouse(int32_t x, int32_t y, uint32_t buttons);

// Add a desktop icon
int desktop_add_icon(const char *name, const char *exec_path, int32_t x, int32_t y);

// Remove a desktop icon
void desktop_remove_icon(uint32_t icon_id);

// Load wallpaper from file
int desktop_load_wallpaper(const char *path);

// Set solid background color
void desktop_set_bg_color(uint32_t color);

// Auto-arrange icons
void desktop_arrange_icons(void);

#endif // DESKTOP_H
