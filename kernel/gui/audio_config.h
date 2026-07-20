// audio_config.h - Audio Configuration Application for MayteraOS
// Comprehensive audio settings: output, input, equalizer, and advanced options
// Uses fixed-point math for kernel compatibility (no SSE/FPU)
#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

#include "../types.h"
#include "window.h"

// ============================================================================
// Window Dimensions
// ============================================================================

#define AC_WINDOW_WIDTH     520
#define AC_WINDOW_HEIGHT    450

// ============================================================================
// Layout Constants
// ============================================================================

#define AC_TAB_HEIGHT       30
#define AC_TAB_WIDTH        100
#define AC_PADDING          15
#define AC_LINE_HEIGHT      22
#define AC_SLIDER_HEIGHT    16
#define AC_SLIDER_TRACK_H   6
#define AC_BUTTON_HEIGHT    28
#define AC_BUTTON_WIDTH     90
#define AC_DROPDOWN_HEIGHT  24
#define AC_VU_METER_WIDTH   300
#define AC_VU_METER_HEIGHT  16
#define AC_EQ_SLIDER_WIDTH  30
#define AC_EQ_SLIDER_HEIGHT 100
#define AC_EQ_BAND_SPACING  42

// ============================================================================
// Tab Indices
// ============================================================================

typedef enum {
    AC_TAB_OUTPUT = 0,
    AC_TAB_INPUT,
    AC_TAB_EQUALIZER,
    AC_TAB_ADVANCED,
    AC_TAB_COUNT
} ac_tab_t;

// ============================================================================
// Output Devices
// ============================================================================

typedef enum {
    AC_OUTPUT_SPEAKERS = 0,
    AC_OUTPUT_HEADPHONES,
    AC_OUTPUT_HDMI,
    AC_OUTPUT_SPDIF,
    AC_OUTPUT_USB,
    AC_OUTPUT_BLUETOOTH,
    AC_OUTPUT_COUNT
} ac_output_device_t;

// ============================================================================
// Input Devices
// ============================================================================

typedef enum {
    AC_INPUT_MICROPHONE = 0,
    AC_INPUT_LINE_IN,
    AC_INPUT_USB_MIC,
    AC_INPUT_BLUETOOTH,
    AC_INPUT_COUNT
} ac_input_device_t;

// ============================================================================
// Equalizer Presets
// ============================================================================

typedef enum {
    AC_EQ_FLAT = 0,
    AC_EQ_ROCK,
    AC_EQ_POP,
    AC_EQ_CLASSICAL,
    AC_EQ_JAZZ,
    AC_EQ_BASS_BOOST,
    AC_EQ_TREBLE_BOOST,
    AC_EQ_VOCAL,
    AC_EQ_CUSTOM,
    AC_EQ_PRESET_COUNT
} ac_eq_preset_t;

// ============================================================================
// Sample Rates
// ============================================================================

typedef enum {
    AC_RATE_44100 = 0,
    AC_RATE_48000,
    AC_RATE_96000,
    AC_RATE_192000,
    AC_RATE_COUNT
} ac_sample_rate_t;

// ============================================================================
// Bit Depths
// ============================================================================

typedef enum {
    AC_BITS_16 = 0,
    AC_BITS_24,
    AC_BITS_32,
    AC_BITS_COUNT
} ac_bit_depth_t;

// ============================================================================
// Buffer Sizes
// ============================================================================

typedef enum {
    AC_BUFFER_64 = 0,
    AC_BUFFER_128,
    AC_BUFFER_256,
    AC_BUFFER_512,
    AC_BUFFER_1024,
    AC_BUFFER_2048,
    AC_BUFFER_COUNT
} ac_buffer_size_t;

// ============================================================================
// EQ Band Definitions
// ============================================================================

#define AC_EQ_BANDS         10
#define AC_EQ_MIN_DB        (-12)
#define AC_EQ_MAX_DB        12

// ============================================================================
// Fixed-point math (16.16 format)
// ============================================================================

typedef int32_t ac_fixed_t;

#define AC_FP_SHIFT     16
#define AC_FP_ONE       (1 << AC_FP_SHIFT)
#define AC_FP_HALF      (AC_FP_ONE >> 1)

// ============================================================================
// VU Meter Structure (fixed-point)
// ============================================================================

typedef struct {
    int32_t current_level;      // Current level (0 to AC_FP_ONE)
    int32_t peak_level;         // Peak hold level
    uint32_t peak_hold_time;    // Ticks until peak decay
    int32_t decay_rate;         // Level decay rate per frame
} ac_vu_meter_t;

// ============================================================================
// Biquad Filter Coefficients (fixed-point)
// ============================================================================

typedef struct {
    int32_t b0, b1, b2;     // Numerator coefficients (fixed-point)
    int32_t a1, a2;         // Denominator coefficients
    int32_t x1, x2;         // Input delay line
    int32_t y1, y2;         // Output delay line
    int32_t gain;           // Simple gain factor for EQ band
} ac_biquad_t;

// ============================================================================
// Audio Configuration Settings
// ============================================================================

typedef struct {
    // Output settings
    ac_output_device_t output_device;
    int master_volume;
    int balance;
    bool output_muted;

    // Input settings
    ac_input_device_t input_device;
    int input_gain;
    bool input_muted;

    // Equalizer
    ac_eq_preset_t eq_preset;
    int eq_bands[AC_EQ_BANDS];
    bool eq_enabled;

    // Advanced
    ac_sample_rate_t sample_rate;
    ac_bit_depth_t bit_depth;
    ac_buffer_size_t buffer_size;
} ac_settings_t;

// ============================================================================
// Audio Config Application Structure
// ============================================================================

typedef struct {
    window_t *window;
    ac_tab_t current_tab;
    bool running;

    // Settings
    ac_settings_t settings;

    // VU meters
    ac_vu_meter_t output_left;
    ac_vu_meter_t output_right;
    ac_vu_meter_t input_level;

    // Biquad filters for EQ
    ac_biquad_t eq_filters_left[AC_EQ_BANDS];
    ac_biquad_t eq_filters_right[AC_EQ_BANDS];

    // UI state
    bool dragging_slider;
    int active_slider;
    int dropdown_open;

    // Hover states
    int hover_tab;
    int hover_eq_band;
    bool hover_mute_output;
    bool hover_mute_input;
    bool hover_test;
    bool hover_reset_eq;
    bool hover_apply;

    // Animation state
    uint32_t last_update_tick;
    bool test_playing;

    // Window manager integration
    int app_id;
    int dock_index;
} audio_config_app_t;

// ============================================================================
// Public API
// ============================================================================

audio_config_app_t *audio_config_create(void);
void audio_config_destroy(audio_config_app_t *ac);
void audio_config_run(audio_config_app_t *ac);
void audio_config_draw(audio_config_app_t *ac);
void audio_config_launch(void);

// Window manager callbacks
void audio_config_on_event(void *app_data, gui_event_t *event);
void audio_config_on_draw(void *app_data);
void audio_config_on_destroy(void *app_data);

// Settings management
void audio_config_apply(audio_config_app_t *ac);
void audio_config_load_settings(audio_config_app_t *ac);
void audio_config_save_settings(audio_config_app_t *ac);
void audio_config_reset_defaults(audio_config_app_t *ac);

// Equalizer functions
void audio_config_apply_preset(audio_config_app_t *ac, ac_eq_preset_t preset);
void audio_config_reset_eq(audio_config_app_t *ac);
void audio_config_calc_eq_coeffs(ac_biquad_t *filter, int32_t freq, int32_t gain_db, int32_t sample_rate);
void audio_config_process_eq(audio_config_app_t *ac, int16_t *samples, uint32_t count, bool stereo);

// VU meter functions
void audio_config_update_vu(ac_vu_meter_t *vu, int32_t level);
void audio_config_decay_vu(ac_vu_meter_t *vu, int32_t dt_fp);

// Test functions
void audio_config_play_test_tone(audio_config_app_t *ac);
void audio_config_stop_test(audio_config_app_t *ac);

#endif // AUDIO_CONFIG_H
