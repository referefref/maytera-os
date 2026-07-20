// panel_appearance.h - Appearance Panel for MayteraOS Settings
// Part of the unified Settings application framework (Task #59)
//
// Features implemented:
// - Theme Selection: Grid of theme thumbnails with instant preview
// - Accent Color: Color picker with preset palette
// - Fonts: System/Title/Monospace font selection with size slider
// - Icon Theme: Icon style and size selection
// - Window Style: Decoration style, button position, borders, shadows
// - Dark Mode: Toggle with optional auto-schedule
//
// Built-in Themes: Default, Dark Mode, Light Mode, High Contrast,
//                  Classic (Win95), Ocean, Sunset, Forest,
//                  Modern Light, Modern Dark, Fluent Light, Fluent Dark

#ifndef PANEL_APPEARANCE_H
#define PANEL_APPEARANCE_H

#include "settings_panel.h"
#include "../../types.h"

// ============================================================================
// Panel Drawing
// ============================================================================

/**
 * Draw the appearance panel content
 * @param panel_x       X position of the panel area
 * @param panel_y       Y position of the panel area
 * @param panel_width   Width of the panel area
 * @param panel_height  Height of the panel area
 */
void appearance_panel_draw(int32_t panel_x, int32_t panel_y,
                            int32_t panel_width, int32_t panel_height);

// ============================================================================
// Event Handling
// ============================================================================

/**
 * Handle mouse click event in the appearance panel
 * @param panel_x       X position of the panel area
 * @param panel_y       Y position of the panel area
 * @param panel_width   Width of the panel area
 * @param panel_height  Height of the panel area
 * @param click_x       X position of the click
 * @param click_y       Y position of the click
 * @return              true if click was handled, false otherwise
 */
bool appearance_panel_handle_click(int32_t panel_x, int32_t panel_y,
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
void appearance_panel_handle_mouse_move(int32_t panel_x, int32_t panel_y,
                                         int32_t panel_width, int32_t panel_height,
                                         int32_t mouse_x, int32_t mouse_y);

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * Initialize the appearance panel module
 */
void appearance_panel_init(void);

/**
 * Cleanup the appearance panel module
 */
void appearance_panel_cleanup(void);

// ============================================================================
// State Getters
// ============================================================================

/**
 * Get the currently selected theme ID in the panel
 * @return  Theme ID (may differ from active theme until applied)
 */
int appearance_get_selected_theme(void);

/**
 * Get the currently selected accent color
 * @return  Accent color in 0x00RRGGBB format
 */
uint32_t appearance_get_accent_color(void);

/**
 * Check if dark mode is enabled
 * @return  true if dark mode is active
 */
bool appearance_is_dark_mode(void);

// ============================================================================
// Settings Framework Integration
// ============================================================================

/**
 * Register the appearance panel with the settings framework
 * Call this during settings application initialization
 * @return  Panel index on success, -1 on failure
 */
int appearance_panel_register(void);

/**
 * Get the panel definition structure
 * @return  Pointer to the static panel definition
 */
const settings_panel_def_t *appearance_panel_get_def(void);

#endif // PANEL_APPEARANCE_H
