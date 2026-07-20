// panel_sound.h - Sound Settings Panel for MayteraOS Settings
// Part of the unified Settings application framework (Task #59)
//
// Features implemented:
// - Output Device: Selection of audio output device
// - Master Volume: Slider with percentage display
// - Balance: Left/Right balance slider
// - Mute: Toggle to mute all audio
// - Test Sound: Button to play test audio
// - System Sounds: Enable/disable system sounds
// - Alert Sound: Selection of alert/notification sound
// - Event Sounds: Per-event sound configuration
// - Application Volumes: Per-app volume control (if supported)
// - Device Info: Display audio device information

#ifndef PANEL_SOUND_H
#define PANEL_SOUND_H

#include "settings_panel.h"
#include "../../types.h"

// ============================================================================
// Panel Drawing
// ============================================================================

/**
 * Draw the sound panel content
 * @param panel_x       X position of the panel area
 * @param panel_y       Y position of the panel area
 * @param panel_width   Width of the panel area
 * @param panel_height  Height of the panel area
 */
void sound_panel_draw(int32_t panel_x, int32_t panel_y,
                      int32_t panel_width, int32_t panel_height);

// ============================================================================
// Event Handling
// ============================================================================

/**
 * Handle mouse click event in the sound panel
 * @param panel_x       X position of the panel area
 * @param panel_y       Y position of the panel area
 * @param panel_width   Width of the panel area
 * @param panel_height  Height of the panel area
 * @param click_x       X position of the click
 * @param click_y       Y position of the click
 * @return              true if click was handled, false otherwise
 */
bool sound_panel_handle_click(int32_t panel_x, int32_t panel_y,
                              int32_t panel_width, int32_t panel_height,
                              int32_t click_x, int32_t click_y);

/**
 * Handle mouse move event for slider dragging and hover effects
 * @param panel_x       X position of the panel area
 * @param panel_y       Y position of the panel area
 * @param panel_width   Width of the panel area
 * @param panel_height  Height of the panel area
 * @param mouse_x       Current mouse X position
 * @param mouse_y       Current mouse Y position
 */
void sound_panel_handle_mouse_move(int32_t panel_x, int32_t panel_y,
                                   int32_t panel_width, int32_t panel_height,
                                   int32_t mouse_x, int32_t mouse_y);

/**
 * Handle scroll event for scrolling through content
 * @param delta         Scroll wheel delta (positive = up, negative = down)
 */
void sound_panel_handle_scroll(int32_t delta);

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * Initialize the sound panel module
 */
void sound_panel_init(void);

/**
 * Cleanup the sound panel module
 */
void sound_panel_cleanup(void);

// ============================================================================
// Volume Control
// ============================================================================

/**
 * Apply the current volume settings to the audio subsystem
 */
void sound_panel_apply_volume(void);

/**
 * Play a test sound to verify audio output
 */
void sound_panel_test(void);

/**
 * Set the master volume
 * @param volume        Volume level (0-100)
 */
void sound_panel_set_volume(int volume);

/**
 * Set the mute state
 * @param mute          true to mute, false to unmute
 */
void sound_panel_set_mute(bool mute);

// ============================================================================
// State Getters
// ============================================================================

/**
 * Get the current master volume level
 * @return  Volume level (0-100)
 */
int sound_panel_get_volume(void);

/**
 * Check if audio is muted
 * @return  true if muted
 */
bool sound_panel_is_muted(void);

/**
 * Get the current balance setting
 * @return  Balance (-50 to +50, 0 = center, negative = left, positive = right)
 */
int sound_panel_get_balance(void);

/**
 * Check if system sounds are enabled
 * @return  true if system sounds are on
 */
bool sound_panel_are_system_sounds_enabled(void);

/**
 * Get the selected alert sound index
 * @return  Alert sound index
 */
int sound_panel_get_alert_sound(void);

/**
 * Check if audio hardware is available
 * @return  true if audio device detected
 */
bool sound_panel_is_audio_available(void);

// ============================================================================
// Settings Framework Integration
// ============================================================================

/**
 * Register the sound panel with the settings framework
 * Call this during settings application initialization
 * @return  Panel index on success, -1 on failure
 */
int sound_panel_register(void);

/**
 * Get the panel definition structure
 * @return  Pointer to the static panel definition
 */
const settings_panel_def_t *sound_panel_get_def(void);

#endif // PANEL_SOUND_H
