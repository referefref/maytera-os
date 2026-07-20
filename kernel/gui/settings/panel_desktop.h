// panel_desktop.h - Desktop Settings Panel for MayteraOS Settings
// Part of the unified Settings application framework (Task #59)
//
// Features implemented:
// - Wallpaper: Browse wallpapers from /WALLPAPERS/, preview thumbnail
// - Position: Center, Tile, Stretch, Fit, Fill
// - Solid Color: Color picker with solid background option
// - Gradient: Two colors with direction selection
// - Screensaver: Type list, preview button, timeout setting
// - Password on Wake: Security option for screensaver
// - Hot Corners: Assign actions to screen corners
// - Desktop Icons: Show/hide, size, auto-arrange, grid alignment

#ifndef PANEL_DESKTOP_H
#define PANEL_DESKTOP_H

#include "settings_panel.h"
#include "../../types.h"

// ============================================================================
// Panel Drawing
// ============================================================================

/**
 * Draw the desktop panel content
 * @param panel_x       X position of the panel area
 * @param panel_y       Y position of the panel area
 * @param panel_width   Width of the panel area
 * @param panel_height  Height of the panel area
 */
void desktop_panel_draw(int32_t panel_x, int32_t panel_y,
                        int32_t panel_width, int32_t panel_height);

// ============================================================================
// Event Handling
// ============================================================================

/**
 * Handle mouse click event in the desktop panel
 * @param panel_x       X position of the panel area
 * @param panel_y       Y position of the panel area
 * @param panel_width   Width of the panel area
 * @param panel_height  Height of the panel area
 * @param click_x       X position of the click
 * @param click_y       Y position of the click
 * @return              true if click was handled, false otherwise
 */
bool desktop_panel_handle_click(int32_t panel_x, int32_t panel_y,
                                int32_t panel_width, int32_t panel_height,
                                int32_t click_x, int32_t click_y);

/**
 * Handle mouse move event for hover effects
 * @param panel_x       X position of the panel area
 * @param panel_y       Y position of the panel area
 * @param panel_width   Width of the panel area
 * @param panel_height  Height of the panel area
 * @param mouse_x       Current mouse X position
 * @param mouse_y       Current mouse Y position
 */
void desktop_panel_handle_mouse_move(int32_t panel_x, int32_t panel_y,
                                     int32_t panel_width, int32_t panel_height,
                                     int32_t mouse_x, int32_t mouse_y);

/**
 * Handle scroll event for scrolling through content
 * @param delta         Scroll wheel delta (positive = up, negative = down)
 */
void desktop_panel_handle_scroll(int32_t delta);

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * Initialize the desktop panel module
 */
void desktop_panel_init(void);

/**
 * Cleanup the desktop panel module
 */
void desktop_panel_cleanup(void);

// ============================================================================
// Wallpaper Position Modes
// ============================================================================

#define WP_POSITION_CENTER          0
#define WP_POSITION_TILE            1
#define WP_POSITION_STRETCH         2
#define WP_POSITION_FIT             3
#define WP_POSITION_FILL            4

// ============================================================================
// Hot Corner Actions
// ============================================================================

#define HOT_CORNER_NONE             0
#define HOT_CORNER_SHOW_DESKTOP     1
#define HOT_CORNER_START_SS         2
#define HOT_CORNER_DISABLE_SS       3
#define HOT_CORNER_LOCK             4

// ============================================================================
// State Getters
// ============================================================================

/**
 * Get the currently selected wallpaper index
 * @return  Wallpaper index (may differ from active wallpaper until applied)
 */
int desktop_get_selected_wallpaper(void);

/**
 * Get the current wallpaper position mode
 * @return  Position mode (WP_POSITION_*)
 */
int desktop_get_position_mode(void);

/**
 * Get the solid background color
 * @return  Color in 0x00RRGGBB format
 */
uint32_t desktop_get_solid_color(void);

/**
 * Check if solid color mode is enabled
 * @return  true if using solid color instead of image
 */
bool desktop_is_solid_color_enabled(void);

/**
 * Get the selected screensaver type
 * @return  Screensaver type (SCREENSAVER_* enum)
 */
int desktop_get_screensaver_type(void);

/**
 * Get the screensaver timeout in minutes
 * @return  Timeout in minutes (0 = disabled)
 */
int desktop_get_screensaver_timeout(void);

/**
 * Get hot corner action for a specific corner
 * @param corner        Corner index (0=TL, 1=TR, 2=BL, 3=BR)
 * @return              Action ID (HOT_CORNER_*)
 */
int desktop_get_hot_corner_action(int corner);

/**
 * Check if desktop icons are shown
 * @return  true if icons are visible
 */
bool desktop_get_show_icons(void);

/**
 * Get the current icon size
 * @return  Icon size in pixels
 */
int desktop_get_icon_size(void);

// ============================================================================
// Settings Framework Integration
// ============================================================================

/**
 * Register the desktop panel with the settings framework
 * Call this during settings application initialization
 * @return  Panel index on success, -1 on failure
 */
int desktop_panel_register(void);

/**
 * Get the panel definition structure
 * @return  Pointer to the static panel definition
 */
const settings_panel_def_t *desktop_panel_get_def(void);

#endif // PANEL_DESKTOP_H
