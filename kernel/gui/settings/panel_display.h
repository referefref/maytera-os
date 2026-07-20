// panel_display.h - Display Settings Panel for MayteraOS Settings
// Part of the unified Settings application framework (Task #59)
//
// Features implemented:
// - Resolution: Available resolutions list with current highlighted
// - Apply/Revert: 15-second revert timer for safe resolution changes
// - Refresh Rate: Available rates for current resolution
// - Monitor Arrangement: Drag monitors to arrange (multi-monitor support)
// - Primary Display: Select which monitor is primary

#ifndef PANEL_DISPLAY_H
#define PANEL_DISPLAY_H

#include "settings_panel.h"
#include "../../types.h"

// ============================================================================
// Panel Drawing
// ============================================================================

/**
 * Draw the display panel content
 * @param panel_x       X position of the panel area
 * @param panel_y       Y position of the panel area
 * @param panel_width   Width of the panel area
 * @param panel_height  Height of the panel area
 */
void display_panel_draw(int32_t panel_x, int32_t panel_y,
                        int32_t panel_width, int32_t panel_height);

// ============================================================================
// Event Handling
// ============================================================================

/**
 * Handle mouse click event in the display panel
 * @param panel_x       X position of the panel area
 * @param panel_y       Y position of the panel area
 * @param panel_width   Width of the panel area
 * @param panel_height  Height of the panel area
 * @param click_x       X position of the click
 * @param click_y       Y position of the click
 * @return              true if click was handled, false otherwise
 */
bool display_panel_handle_click(int32_t panel_x, int32_t panel_y,
                                int32_t panel_width, int32_t panel_height,
                                int32_t click_x, int32_t click_y);

/**
 * Handle mouse move event for hover effects and monitor dragging
 * @param panel_x       X position of the panel area
 * @param panel_y       Y position of the panel area
 * @param panel_width   Width of the panel area
 * @param panel_height  Height of the panel area
 * @param mouse_x       Current mouse X position
 * @param mouse_y       Current mouse Y position
 */
void display_panel_handle_mouse_move(int32_t panel_x, int32_t panel_y,
                                     int32_t panel_width, int32_t panel_height,
                                     int32_t mouse_x, int32_t mouse_y);

/**
 * Handle scroll event for scrolling through resolution list
 * @param delta         Scroll wheel delta (positive = up, negative = down)
 */
void display_panel_handle_scroll(int32_t delta);

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * Initialize the display panel module
 */
void display_panel_init(void);

/**
 * Cleanup the display panel module
 */
void display_panel_cleanup(void);

// ============================================================================
// Resolution Change Control
// ============================================================================

/**
 * Apply the selected resolution (starts 15-second confirmation timer)
 */
void display_panel_apply_resolution(void);

/**
 * Confirm the resolution change (stops revert timer)
 */
void display_panel_confirm_change(void);

/**
 * Revert the resolution change to previous setting
 */
void display_panel_revert_change(void);

/**
 * Update the revert countdown timer (call once per second)
 * Automatically reverts if countdown reaches zero
 */
void display_panel_update_timer(void);

/**
 * Check if a resolution change is pending confirmation
 * @return  true if waiting for user confirmation
 */
bool display_panel_is_change_pending(void);

/**
 * Get the revert countdown remaining time
 * @return  Seconds remaining, or 0 if no change pending
 */
int display_panel_get_revert_countdown(void);

// ============================================================================
// State Getters
// ============================================================================

/**
 * Get the currently selected resolution index
 * @return  Resolution index (may differ from active resolution until applied)
 */
int display_get_selected_resolution(void);

/**
 * Get the current resolution as width and height
 * @param width         Pointer to receive width
 * @param height        Pointer to receive height
 */
void display_get_current_resolution(uint32_t *width, uint32_t *height);

/**
 * Get the currently selected refresh rate index
 * @return  Refresh rate index
 */
int display_get_selected_refresh_rate(void);

/**
 * Get the number of connected monitors
 * @return  Monitor count
 */
int display_get_monitor_count(void);

/**
 * Get the primary monitor index
 * @return  Primary monitor index (0-based)
 */
int display_get_primary_monitor(void);

// ============================================================================
// Settings Framework Integration
// ============================================================================

/**
 * Register the display panel with the settings framework
 * Call this during settings application initialization
 * @return  Panel index on success, -1 on failure
 */
int display_panel_register(void);

/**
 * Get the panel definition structure
 * @return  Pointer to the static panel definition
 */
const settings_panel_def_t *display_panel_get_def(void);

#endif // PANEL_DISPLAY_H
