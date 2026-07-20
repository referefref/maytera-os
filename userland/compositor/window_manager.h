// window_manager.h - Window Manager for Userland Compositor
#ifndef WINDOW_MANAGER_H
#define WINDOW_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "compositor.h"

// ============================================================================
// Window Manager Configuration
// ============================================================================

typedef struct {
    // Animation settings
    bool enable_animations;
    uint32_t anim_duration_ms;
    
    // Snap settings
    bool enable_snap;
    int32_t snap_distance;
    
    // Focus settings
    bool focus_follows_mouse;
    bool raise_on_focus;
    
    // Decoration settings
    int32_t titlebar_height;
    int32_t border_width;
    uint32_t active_titlebar_color;
    uint32_t inactive_titlebar_color;
    uint32_t border_color;
} wm_config_t;

// ============================================================================
// Window Tiling
// ============================================================================

typedef enum {
    TILE_NONE = 0,
    TILE_LEFT,          // Left half of screen
    TILE_RIGHT,         // Right half of screen
    TILE_TOP_LEFT,      // Top-left quarter
    TILE_TOP_RIGHT,     // Top-right quarter
    TILE_BOTTOM_LEFT,   // Bottom-left quarter
    TILE_BOTTOM_RIGHT,  // Bottom-right quarter
    TILE_MAXIMIZE       // Full screen
} tile_mode_t;

// ============================================================================
// API
// ============================================================================

// Initialize window manager
int window_manager_init(void);

// Shutdown window manager
void window_manager_shutdown(void);

// Get configuration
wm_config_t *wm_get_config(void);

// Tile a window
void wm_tile_window(comp_window_t *win, tile_mode_t mode);

// Cascade all windows
void wm_cascade_windows(void);

// Tile all windows
void wm_tile_all_windows(void);

// Minimize all windows
void wm_minimize_all(void);

// Show desktop (minimize all)
void wm_show_desktop(void);

// Switch to next window (Alt+Tab)
void wm_cycle_windows(bool reverse);

// Close focused window
void wm_close_focused(void);

// Handle keyboard shortcuts
bool wm_handle_shortcut(uint32_t keycode, uint32_t modifiers);

#endif // WINDOW_MANAGER_H
