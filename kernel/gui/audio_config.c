// audio_config.c - Audio Configuration Application for MayteraOS
// Uses fixed-point math throughout (no SSE/FPU required)

#include "audio_config.h"
#include "window.h"
#include "desktop.h"
#include "icons.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../drivers/audio.h"
#include "syslog.h"

// Fixed-point math helpers
static inline int32_t fp_mul(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * b) >> AC_FP_SHIFT);
}

// Colors
#define AC_BG_COLOR             0x2D2D2D
#define AC_PANEL_BG             0x3C3C3C
#define AC_TAB_ACTIVE           0x0078D7
#define AC_TAB_INACTIVE         0x404040
#define AC_TAB_HOVER            0x505050
#define AC_TEXT_COLOR           0xE0E0E0
#define AC_TEXT_DIM             0x808080
#define AC_TEXT_BRIGHT          0xFFFFFF
#define AC_SLIDER_TRACK         0x505050
#define AC_SLIDER_FILL          0x0078D7
#define AC_SLIDER_HANDLE        0xFFFFFF
#define AC_BUTTON_BG            0x404040
#define AC_BUTTON_HOVER         0x505050
#define AC_BUTTON_ACTIVE        0x0078D7
#define AC_DROPDOWN_BG          0x3A3A3A
#define AC_DROPDOWN_HOVER       0x4A4A4A
#define AC_VU_GREEN             0x00C853
#define AC_VU_YELLOW            0xFFD600
#define AC_VU_RED               0xFF1744
#define AC_VU_BG                0x252525
#define AC_VU_PEAK              0xFF5252
#define AC_MUTE_COLOR           0xFF4444
#define AC_EQ_BAND_BG           0x353535
#define AC_EQ_CENTER_LINE       0x505050
#define AC_BORDER_COLOR         0x555555

// String tables
static const char *ac_tab_names[AC_TAB_COUNT] = {
    "Output", "Input", "Equalizer", "Advanced"
};

static const char *ac_output_names[AC_OUTPUT_COUNT] = {
    "Speakers", "Headphones", "HDMI Audio", "S/PDIF", "USB Audio", "Bluetooth"
};

static const char *ac_input_names[AC_INPUT_COUNT] = {
    "Microphone", "Line In", "USB Microphone", "Bluetooth"
};

static const char *ac_eq_preset_names[AC_EQ_PRESET_COUNT] = {
    "Flat", "Rock", "Pop", "Classical", "Jazz", "Bass Boost", "Treble Boost", "Vocal", "Custom"
};

static const char *ac_rate_names[AC_RATE_COUNT] = {
    "44100 Hz", "48000 Hz", "96000 Hz", "192000 Hz"
};

static const uint32_t ac_rate_values[AC_RATE_COUNT] = {
    44100, 48000, 96000, 192000
};

static const char *ac_bits_names[AC_BITS_COUNT] = {
    "16-bit", "24-bit", "32-bit"
};

static const char *ac_buffer_names[AC_BUFFER_COUNT] = {
    "64 (~1.5ms)", "128 (~3ms)", "256 (~6ms)", "512 (~12ms)", "1024 (~23ms)", "2048 (~46ms)"
};

static const uint32_t ac_eq_freqs[AC_EQ_BANDS] = {
    31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
};

static const char *ac_eq_labels[AC_EQ_BANDS] = {
    "31", "62", "125", "250", "500", "1K", "2K", "4K", "8K", "16K"
};

// EQ presets (gain values in dB for each band)
static const int ac_eq_presets[AC_EQ_PRESET_COUNT - 1][AC_EQ_BANDS] = {
    {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 },  // Flat
    {  5,  4,  3,  0, -1, -1,  1,  3,  4,  5 },  // Rock
    { -1,  1,  4,  5,  3,  0,  0,  1,  2,  3 },  // Pop
    {  3,  2,  1,  0,  0,  0, -1, -1,  1,  2 },  // Classical
    {  3,  2,  1,  2,  0, -1,  0,  1,  2,  3 },  // Jazz
    {  8,  6,  4,  2,  0,  0,  0,  0,  0,  0 },  // Bass Boost
    {  0,  0,  0,  0,  0,  1,  3,  5,  6,  8 },  // Treble Boost
    { -2, -1,  0,  2,  4,  4,  3,  1,  0, -1 }   // Vocal
};

static audio_config_app_t *g_audio_config = NULL;
extern uint64_t timer_get_ticks(void);

// Helper Functions
static void ac_itoa(int val, char *buf) {
    char *p = buf;
    if (val < 0) { *p++ = '-'; val = -val; }
    if (val == 0) { *p++ = '0'; *p = '\0'; return; }
    char tmp[12];
    int i = 0;
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    while (i > 0) { *p++ = tmp[--i]; }
    *p = '\0';
}

static void ac_draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    if (!text) return;
    for (int i = 0; text[i]; i++) {
        const uint8_t *glyph = font_get_glyph(text[i]);
        if (glyph) {
            for (int row = 0; row < 16; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(x + i * 8 + col, y + row, color);
                    }
                }
            }
        }
    }
}

static void ac_draw_text_centered(int32_t x, int32_t y, int32_t w, const char *text, uint32_t color) {
    int len = strlen(text);
    int text_w = len * 8;
    int cx = x + (w - text_w) / 2;
    ac_draw_text(cx, y, text, color);
}

static void ac_draw_slider_h(int32_t x, int32_t y, int32_t w, int value, int min_val, int max_val,
                              uint32_t track_color, uint32_t fill_color, uint32_t handle_color) {
    int range = max_val - min_val;
    if (range <= 0) range = 1;
    int fill_w = ((value - min_val) * (w - 10)) / range;
    if (fill_w < 0) fill_w = 0;
    if (fill_w > w - 10) fill_w = w - 10;

    fb_fill_rect(x, y + (AC_SLIDER_HEIGHT - AC_SLIDER_TRACK_H) / 2, w, AC_SLIDER_TRACK_H, track_color);
    fb_fill_rect(x, y + (AC_SLIDER_HEIGHT - AC_SLIDER_TRACK_H) / 2, fill_w + 5, AC_SLIDER_TRACK_H, fill_color);
    fb_fill_rect(x + fill_w, y, 10, AC_SLIDER_HEIGHT, handle_color);
}

static void ac_draw_slider_v(int32_t x, int32_t y, int32_t h, int value, int min_val, int max_val,
                              uint32_t track_color, uint32_t fill_color, uint32_t handle_color, bool highlight) {
    int center_y = y + h / 2;
    int handle_h = 8;
    int track_w = 6;
    int tx = x + (AC_EQ_SLIDER_WIDTH - track_w) / 2;
    int max_val_abs = (max_val > 0) ? max_val : -max_val;
    (void)min_val;

    fb_fill_rect(tx, y, track_w, h, track_color);
    fb_fill_rect(tx - 4, center_y - 1, track_w + 8, 2, AC_EQ_CENTER_LINE);

    int value_offset = (value * (h / 2 - handle_h / 2)) / max_val_abs;
    int fill_y, fill_h;
    if (value >= 0) { fill_y = center_y - value_offset; fill_h = value_offset; }
    else { fill_y = center_y; fill_h = -value_offset; }
    if (fill_h > 0) fb_fill_rect(tx, fill_y, track_w, fill_h, fill_color);

    int handle_y = center_y - value_offset - handle_h / 2;
    uint32_t hc = highlight ? 0xFFFF00 : handle_color;
    fb_fill_rect(x, handle_y, AC_EQ_SLIDER_WIDTH, handle_h, hc);
}

static void ac_draw_button(int32_t x, int32_t y, int32_t w, int32_t h,
                            const char *text, bool hover, bool active) {
    uint32_t bg = active ? AC_BUTTON_ACTIVE : (hover ? AC_BUTTON_HOVER : AC_BUTTON_BG);
    fb_fill_rect(x, y, w, h, bg);
    fb_fill_rect(x, y, w, 1, AC_BORDER_COLOR);
    fb_fill_rect(x, y + h - 1, w, 1, AC_BORDER_COLOR);
    fb_fill_rect(x, y, 1, h, AC_BORDER_COLOR);
    fb_fill_rect(x + w - 1, y, 1, h, AC_BORDER_COLOR);
    ac_draw_text_centered(x, y + (h - 16) / 2, w, text, AC_TEXT_BRIGHT);
}

static void ac_draw_dropdown(int32_t x, int32_t y, int32_t w, const char *text, bool open, bool hover) {
    uint32_t bg = open ? AC_DROPDOWN_HOVER : (hover ? AC_BUTTON_HOVER : AC_DROPDOWN_BG);
    fb_fill_rect(x, y, w, AC_DROPDOWN_HEIGHT, bg);
    fb_fill_rect(x, y, w, 1, AC_BORDER_COLOR);
    fb_fill_rect(x, y + AC_DROPDOWN_HEIGHT - 1, w, 1, AC_BORDER_COLOR);
    fb_fill_rect(x, y, 1, AC_DROPDOWN_HEIGHT, AC_BORDER_COLOR);
    fb_fill_rect(x + w - 1, y, 1, AC_DROPDOWN_HEIGHT, AC_BORDER_COLOR);
    ac_draw_text(x + 8, y + (AC_DROPDOWN_HEIGHT - 16) / 2, text, AC_TEXT_COLOR);
    int ax = x + w - 16, ay = y + AC_DROPDOWN_HEIGHT / 2 - 2;
    for (int i = 0; i < 5; i++) fb_fill_rect(ax + i, ay + i, 9 - i * 2, 1, AC_TEXT_COLOR);
}

static void ac_draw_dropdown_items(int32_t x, int32_t y, int32_t w,
                                    const char **items, int count, int selected, int hover_idx) {
    int item_h = AC_DROPDOWN_HEIGHT;
    fb_fill_rect(x, y, w, count * item_h, AC_DROPDOWN_BG);
    fb_fill_rect(x, y, w, 1, AC_BORDER_COLOR);
    fb_fill_rect(x, y + count * item_h - 1, w, 1, AC_BORDER_COLOR);
    fb_fill_rect(x, y, 1, count * item_h, AC_BORDER_COLOR);
    fb_fill_rect(x + w - 1, y, 1, count * item_h, AC_BORDER_COLOR);
    for (int i = 0; i < count; i++) {
        int iy = y + i * item_h;
        if (i == hover_idx) fb_fill_rect(x + 1, iy, w - 2, item_h, AC_DROPDOWN_HOVER);
        ac_draw_text(x + 8, iy + (item_h - 16) / 2, items[i], (i == selected) ? AC_TAB_ACTIVE : AC_TEXT_COLOR);
    }
}

static void ac_draw_vu_meter(int32_t x, int32_t y, int32_t w, int32_t h, int32_t level_fp, int32_t peak_fp, const char *label) {
    fb_fill_rect(x, y, w, h, AC_VU_BG);
    int bar_w = (int)((int64_t)level_fp * (w - 4) / AC_FP_ONE);
    if (bar_w < 0) bar_w = 0;
    if (bar_w > w - 4) bar_w = w - 4;
    int green_end = (w - 4) * 60 / 100;
    int yellow_end = (w - 4) * 85 / 100;
    for (int i = 0; i < bar_w; i++) {
        uint32_t color = (i < green_end) ? AC_VU_GREEN : ((i < yellow_end) ? AC_VU_YELLOW : AC_VU_RED);
        fb_fill_rect(x + 2 + i, y + 2, 1, h - 4, color);
    }
    int peak_x = x + 2 + (int)((int64_t)peak_fp * (w - 4) / AC_FP_ONE);
    if (peak_x > x + 2 && peak_x < x + w - 3) fb_fill_rect(peak_x, y + 1, 2, h - 2, AC_VU_PEAK);
    fb_fill_rect(x, y, w, 1, AC_BORDER_COLOR);
    fb_fill_rect(x, y + h - 1, w, 1, AC_BORDER_COLOR);
    fb_fill_rect(x, y, 1, h, AC_BORDER_COLOR);
    fb_fill_rect(x + w - 1, y, 1, h, AC_BORDER_COLOR);
    if (label) ac_draw_text(x - 20, y + (h - 16) / 2, label, AC_TEXT_DIM);
}

static void ac_draw_mute_button(int32_t x, int32_t y, bool muted, bool hover) {
    int size = AC_BUTTON_HEIGHT;
    uint32_t bg = hover ? AC_BUTTON_HOVER : AC_BUTTON_BG;
    uint32_t icon_color = muted ? AC_MUTE_COLOR : AC_TEXT_COLOR;
    fb_fill_rect(x, y, size, size, bg);
    fb_fill_rect(x, y, size, 1, AC_BORDER_COLOR);
    fb_fill_rect(x, y + size - 1, size, 1, AC_BORDER_COLOR);
    fb_fill_rect(x, y, 1, size, AC_BORDER_COLOR);
    fb_fill_rect(x + size - 1, y, 1, size, AC_BORDER_COLOR);
    int sx = x + 6, sy = y + 8;
    fb_fill_rect(sx, sy, 4, 12, icon_color);
    for (int i = 0; i < 6; i++) fb_fill_rect(sx + 4, sy - i, 1, 12 + i * 2, icon_color);
    if (muted) {
        for (int i = 0; i < 8; i++) {
            fb_put_pixel(x + size - 12 + i, y + 8 + i, AC_MUTE_COLOR);
            fb_put_pixel(x + size - 12 + i, y + size - 9 - i, AC_MUTE_COLOR);
        }
    }
}

// Tab Drawing
static void ac_draw_tabs(audio_config_app_t *ac, int32_t x, int32_t y, int32_t w) {
    int tab_w = w / AC_TAB_COUNT;
    for (int i = 0; i < AC_TAB_COUNT; i++) {
        int tx = x + i * tab_w;
        uint32_t bg, tc;
        if ((ac_tab_t)i == ac->current_tab) { bg = AC_TAB_ACTIVE; tc = AC_TEXT_BRIGHT; }
        else if (i == ac->hover_tab) { bg = AC_TAB_HOVER; tc = AC_TEXT_COLOR; }
        else { bg = AC_TAB_INACTIVE; tc = AC_TEXT_DIM; }
        fb_fill_rect(tx, y, tab_w - 2, AC_TAB_HEIGHT, bg);
        ac_draw_text_centered(tx, y + (AC_TAB_HEIGHT - 16) / 2, tab_w - 2, ac_tab_names[i], tc);
    }
}

static void ac_draw_output_tab(audio_config_app_t *ac, int32_t wx, int32_t wy, int32_t ww, int32_t wh) {
    (void)wh;
    int y = wy + AC_TAB_HEIGHT + AC_PADDING;
    int x = wx + AC_PADDING;
    int content_w = ww - AC_PADDING * 2;

    ac_draw_text(x, y, "Output Device:", AC_TEXT_COLOR);
    y += AC_LINE_HEIGHT;
    ac_draw_dropdown(x, y, 250, ac_output_names[ac->settings.output_device], ac->dropdown_open == 0, false);
    y += AC_DROPDOWN_HEIGHT + AC_LINE_HEIGHT;

    ac_draw_text(x, y, "Master Volume:", AC_TEXT_COLOR);
    y += AC_LINE_HEIGHT;
    ac_draw_slider_h(x, y, content_w - 100, ac->settings.master_volume, 0, 100,
                     AC_SLIDER_TRACK, AC_SLIDER_FILL, AC_SLIDER_HANDLE);
    char vol_str[8];
    ac_itoa(ac->settings.master_volume, vol_str);
    strcat(vol_str, "%");
    ac_draw_text(x + content_w - 90, y, vol_str, AC_TEXT_BRIGHT);
    ac_draw_mute_button(x + content_w - 40, y - 6, ac->settings.output_muted, ac->hover_mute_output);
    y += AC_SLIDER_HEIGHT + AC_LINE_HEIGHT + 5;

    ac_draw_text(x, y, "Balance:", AC_TEXT_COLOR);
    y += AC_LINE_HEIGHT;
    ac_draw_text(x, y, "L", AC_TEXT_DIM);
    ac_draw_slider_h(x + 20, y, content_w - 80, ac->settings.balance + 50, 0, 100,
                     AC_SLIDER_TRACK, AC_SLIDER_FILL, AC_SLIDER_HANDLE);
    ac_draw_text(x + content_w - 50, y, "R", AC_TEXT_DIM);
    y += AC_SLIDER_HEIGHT + AC_LINE_HEIGHT + 10;

    ac_draw_text(x, y, "Output Level:", AC_TEXT_COLOR);
    y += AC_LINE_HEIGHT;
    ac_draw_vu_meter(x + 25, y, AC_VU_METER_WIDTH, AC_VU_METER_HEIGHT,
                     ac->output_left.current_level, ac->output_left.peak_level, "L");
    y += AC_VU_METER_HEIGHT + 4;
    ac_draw_vu_meter(x + 25, y, AC_VU_METER_WIDTH, AC_VU_METER_HEIGHT,
                     ac->output_right.current_level, ac->output_right.peak_level, "R");
    y += AC_VU_METER_HEIGHT + AC_LINE_HEIGHT + 15;

    ac_draw_button(x, y, AC_BUTTON_WIDTH, AC_BUTTON_HEIGHT, "Test Sound", ac->hover_test, ac->test_playing);

    if (ac->dropdown_open == 0) {
        int dy = wy + AC_TAB_HEIGHT + AC_PADDING + AC_LINE_HEIGHT + AC_DROPDOWN_HEIGHT;
        ac_draw_dropdown_items(x, dy, 250, ac_output_names, AC_OUTPUT_COUNT, ac->settings.output_device, -1);
    }
}

static void ac_draw_input_tab(audio_config_app_t *ac, int32_t wx, int32_t wy, int32_t ww, int32_t wh) {
    (void)wh;
    int y = wy + AC_TAB_HEIGHT + AC_PADDING;
    int x = wx + AC_PADDING;
    int content_w = ww - AC_PADDING * 2;

    ac_draw_text(x, y, "Input Device:", AC_TEXT_COLOR);
    y += AC_LINE_HEIGHT;
    ac_draw_dropdown(x, y, 250, ac_input_names[ac->settings.input_device], ac->dropdown_open == 1, false);
    y += AC_DROPDOWN_HEIGHT + AC_LINE_HEIGHT;

    ac_draw_text(x, y, "Input Gain:", AC_TEXT_COLOR);
    y += AC_LINE_HEIGHT;
    ac_draw_slider_h(x, y, content_w - 100, ac->settings.input_gain, 0, 100,
                     AC_SLIDER_TRACK, AC_SLIDER_FILL, AC_SLIDER_HANDLE);
    char gain_str[8];
    ac_itoa(ac->settings.input_gain, gain_str);
    strcat(gain_str, "%");
    ac_draw_text(x + content_w - 90, y, gain_str, AC_TEXT_BRIGHT);
    ac_draw_mute_button(x + content_w - 40, y - 6, ac->settings.input_muted, ac->hover_mute_input);
    y += AC_SLIDER_HEIGHT + AC_LINE_HEIGHT + 20;

    ac_draw_text(x, y, "Input Level (Real-time):", AC_TEXT_COLOR);
    y += AC_LINE_HEIGHT;
    ac_draw_vu_meter(x + 25, y, AC_VU_METER_WIDTH, AC_VU_METER_HEIGHT + 4,
                     ac->input_level.current_level, ac->input_level.peak_level, "IN");
    y += AC_VU_METER_HEIGHT + AC_LINE_HEIGHT + 20;

    ac_draw_text(x, y, "Note: Enable input monitoring in your", AC_TEXT_DIM);
    y += AC_LINE_HEIGHT;
    ac_draw_text(x, y, "recording application for live preview.", AC_TEXT_DIM);

    if (ac->dropdown_open == 1) {
        int dy = wy + AC_TAB_HEIGHT + AC_PADDING + AC_LINE_HEIGHT + AC_DROPDOWN_HEIGHT;
        ac_draw_dropdown_items(x, dy, 250, ac_input_names, AC_INPUT_COUNT, ac->settings.input_device, -1);
    }
}

static void ac_draw_equalizer_tab(audio_config_app_t *ac, int32_t wx, int32_t wy, int32_t ww, int32_t wh) {
    (void)wh;
    int y = wy + AC_TAB_HEIGHT + AC_PADDING;
    int x = wx + AC_PADDING;
    int content_w = ww - AC_PADDING * 2;

    ac_draw_text(x, y, "Preset:", AC_TEXT_COLOR);
    ac_draw_dropdown(x + 60, y - 4, 180, ac_eq_preset_names[ac->settings.eq_preset], ac->dropdown_open == 2, false);
    ac_draw_button(x + content_w - AC_BUTTON_WIDTH, y - 4, AC_BUTTON_WIDTH, AC_DROPDOWN_HEIGHT, "Reset", ac->hover_reset_eq, false);
    y += AC_DROPDOWN_HEIGHT + AC_LINE_HEIGHT;

    ac_draw_text(x, y + 10, "+12", AC_TEXT_DIM);
    ac_draw_text(x + 4, y + AC_EQ_SLIDER_HEIGHT / 2 - 8, "0", AC_TEXT_DIM);
    ac_draw_text(x, y + AC_EQ_SLIDER_HEIGHT - 20, "-12", AC_TEXT_DIM);

    int eq_start_x = x + 35;
    for (int i = 0; i < AC_EQ_BANDS; i++) {
        int bx = eq_start_x + i * AC_EQ_BAND_SPACING;
        bool highlight = (ac->hover_eq_band == i);
        ac_draw_slider_v(bx, y, AC_EQ_SLIDER_HEIGHT, ac->settings.eq_bands[i],
                         AC_EQ_MIN_DB, AC_EQ_MAX_DB, AC_EQ_BAND_BG, AC_SLIDER_FILL, AC_SLIDER_HANDLE, highlight);
        ac_draw_text_centered(bx, y + AC_EQ_SLIDER_HEIGHT + 5, AC_EQ_SLIDER_WIDTH, ac_eq_labels[i], AC_TEXT_DIM);
        char db_str[8];
        if (ac->settings.eq_bands[i] > 0) { db_str[0] = '+'; ac_itoa(ac->settings.eq_bands[i], db_str + 1); }
        else { ac_itoa(ac->settings.eq_bands[i], db_str); }
        ac_draw_text_centered(bx, y + AC_EQ_SLIDER_HEIGHT + 20, AC_EQ_SLIDER_WIDTH, db_str, highlight ? AC_TEXT_BRIGHT : AC_TEXT_DIM);
    }
    y += AC_EQ_SLIDER_HEIGHT + 45;

    ac_draw_text(x, y, "Equalizer:", AC_TEXT_COLOR);
    ac_draw_button(x + 80, y - 4, 80, AC_DROPDOWN_HEIGHT, ac->settings.eq_enabled ? "Enabled" : "Disabled", false, ac->settings.eq_enabled);

    if (ac->dropdown_open == 2) {
        int dy = wy + AC_TAB_HEIGHT + AC_PADDING + AC_DROPDOWN_HEIGHT;
        ac_draw_dropdown_items(x + 60, dy, 180, ac_eq_preset_names, AC_EQ_PRESET_COUNT - 1, ac->settings.eq_preset, -1);
    }
}

static void ac_draw_advanced_tab(audio_config_app_t *ac, int32_t wx, int32_t wy, int32_t ww, int32_t wh) {
    int y = wy + AC_TAB_HEIGHT + AC_PADDING;
    int x = wx + AC_PADDING;
    int content_w = ww - AC_PADDING * 2;

    ac_draw_text(x, y, "Sample Rate:", AC_TEXT_COLOR);
    y += AC_LINE_HEIGHT;
    ac_draw_dropdown(x, y, 200, ac_rate_names[ac->settings.sample_rate], ac->dropdown_open == 3, false);
    y += AC_DROPDOWN_HEIGHT + AC_LINE_HEIGHT;

    ac_draw_text(x, y, "Bit Depth:", AC_TEXT_COLOR);
    y += AC_LINE_HEIGHT;
    ac_draw_dropdown(x, y, 200, ac_bits_names[ac->settings.bit_depth], ac->dropdown_open == 4, false);
    y += AC_DROPDOWN_HEIGHT + AC_LINE_HEIGHT;

    ac_draw_text(x, y, "Buffer Size:", AC_TEXT_COLOR);
    y += AC_LINE_HEIGHT;
    ac_draw_dropdown(x, y, 200, ac_buffer_names[ac->settings.buffer_size], ac->dropdown_open == 5, false);
    y += AC_DROPDOWN_HEIGHT + AC_LINE_HEIGHT + 10;

    fb_fill_rect(x, y, content_w, 1, AC_BORDER_COLOR);
    y += AC_LINE_HEIGHT;
    ac_draw_text(x, y, "Audio Driver Information", AC_TEXT_BRIGHT);
    y += AC_LINE_HEIGHT + 5;

    audio_device_info_t info;
    if (audio_get_device_info(&info) == AUDIO_OK) {
        ac_draw_text(x, y, "Device Type:", AC_TEXT_DIM);
        const char *type_str = "Unknown";
        if (info.type == AUDIO_DEVICE_AC97) type_str = "AC97";
        else if (info.type == AUDIO_DEVICE_HDA) type_str = "Intel HDA";
        else if (info.type == AUDIO_DEVICE_SB16) type_str = "SB16";
        ac_draw_text(x + 120, y, type_str, AC_TEXT_COLOR);
        y += AC_LINE_HEIGHT;
        ac_draw_text(x, y, "Device Name:", AC_TEXT_DIM);
        ac_draw_text(x + 120, y, info.name ? info.name : "N/A", AC_TEXT_COLOR);
    } else {
        ac_draw_text(x, y, "No audio device detected", AC_TEXT_DIM);
    }

    y = wy + wh - AC_PADDING - AC_BUTTON_HEIGHT;
    ac_draw_button(x + content_w - AC_BUTTON_WIDTH - 10, y, AC_BUTTON_WIDTH, AC_BUTTON_HEIGHT, "Apply", ac->hover_apply, false);

    if (ac->dropdown_open == 3) {
        int dy = wy + AC_TAB_HEIGHT + AC_PADDING + AC_LINE_HEIGHT + AC_DROPDOWN_HEIGHT;
        ac_draw_dropdown_items(x, dy, 200, ac_rate_names, AC_RATE_COUNT, ac->settings.sample_rate, -1);
    }
    if (ac->dropdown_open == 4) {
        int dy = wy + AC_TAB_HEIGHT + AC_PADDING + AC_LINE_HEIGHT * 2 + AC_DROPDOWN_HEIGHT * 2 + AC_LINE_HEIGHT;
        ac_draw_dropdown_items(x, dy, 200, ac_bits_names, AC_BITS_COUNT, ac->settings.bit_depth, -1);
    }
    if (ac->dropdown_open == 5) {
        int dy = wy + AC_TAB_HEIGHT + AC_PADDING + AC_LINE_HEIGHT * 3 + AC_DROPDOWN_HEIGHT * 3 + AC_LINE_HEIGHT * 2;
        ac_draw_dropdown_items(x, dy, 200, ac_buffer_names, AC_BUFFER_COUNT, ac->settings.buffer_size, -1);
    }
}

void audio_config_draw(audio_config_app_t *ac) {
    if (!ac || !ac->window) return;
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(ac->window, &wx, &wy, &ww, &wh);
    fb_fill_rect(wx, wy, ww, wh, AC_BG_COLOR);
    ac_draw_tabs(ac, wx, wy, ww);
    fb_fill_rect(wx, wy + AC_TAB_HEIGHT, ww, wh - AC_TAB_HEIGHT, AC_PANEL_BG);

    switch (ac->current_tab) {
        case AC_TAB_OUTPUT: ac_draw_output_tab(ac, wx, wy, ww, wh); break;
        case AC_TAB_INPUT: ac_draw_input_tab(ac, wx, wy, ww, wh); break;
        case AC_TAB_EQUALIZER: ac_draw_equalizer_tab(ac, wx, wy, ww, wh); break;
        case AC_TAB_ADVANCED: ac_draw_advanced_tab(ac, wx, wy, ww, wh); break;
        default: break;
    }
}

// Event Handling
static void ac_handle_click(audio_config_app_t *ac, int32_t mx, int32_t my) {
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(ac->window, &wx, &wy, &ww, &wh);
    int rx = mx - wx, ry = my - wy;

    if (ry >= 0 && ry < AC_TAB_HEIGHT) {
        int tab_w = ww / AC_TAB_COUNT;
        int tab = rx / tab_w;
        if (tab >= 0 && tab < AC_TAB_COUNT) {
            ac->current_tab = (ac_tab_t)tab;
            ac->dropdown_open = -1;
            window_invalidate(ac->window);
            return;
        }
    }

    if (ac->dropdown_open >= 0) {
        ac->dropdown_open = -1;
        window_invalidate(ac->window);
        return;
    }

    int content_y = AC_TAB_HEIGHT + AC_PADDING;
    int content_w = ww - AC_PADDING * 2;

    switch (ac->current_tab) {
        case AC_TAB_OUTPUT:
            if (ry >= content_y + AC_LINE_HEIGHT && ry < content_y + AC_LINE_HEIGHT + AC_DROPDOWN_HEIGHT &&
                rx >= AC_PADDING && rx < AC_PADDING + 250) {
                ac->dropdown_open = 0;
                window_invalidate(ac->window);
                return;
            }
            {
                int mute_x = content_w + AC_PADDING - 40;
                int mute_y = content_y + AC_LINE_HEIGHT + AC_DROPDOWN_HEIGHT + AC_LINE_HEIGHT * 2;
                if (rx >= mute_x && rx < mute_x + AC_BUTTON_HEIGHT && ry >= mute_y - 6 && ry < mute_y - 6 + AC_BUTTON_HEIGHT) {
                    ac->settings.output_muted = !ac->settings.output_muted;
                    audio_mute(ac->settings.output_muted);
                    window_invalidate(ac->window);
                    return;
                }
                int test_y = content_y + AC_LINE_HEIGHT * 7 + AC_DROPDOWN_HEIGHT + AC_VU_METER_HEIGHT * 2 + 50;
                if (ry >= test_y && ry < test_y + AC_BUTTON_HEIGHT && rx >= AC_PADDING && rx < AC_PADDING + AC_BUTTON_WIDTH) {
                    if (!ac->test_playing) audio_config_play_test_tone(ac);
                    else audio_config_stop_test(ac);
                    window_invalidate(ac->window);
                    return;
                }
            }
            break;

        case AC_TAB_INPUT:
            if (ry >= content_y + AC_LINE_HEIGHT && ry < content_y + AC_LINE_HEIGHT + AC_DROPDOWN_HEIGHT &&
                rx >= AC_PADDING && rx < AC_PADDING + 250) {
                ac->dropdown_open = 1;
                window_invalidate(ac->window);
                return;
            }
            {
                int mute_x = content_w + AC_PADDING - 40;
                int mute_y = content_y + AC_LINE_HEIGHT + AC_DROPDOWN_HEIGHT + AC_LINE_HEIGHT * 2;
                if (rx >= mute_x && rx < mute_x + AC_BUTTON_HEIGHT && ry >= mute_y - 6 && ry < mute_y - 6 + AC_BUTTON_HEIGHT) {
                    ac->settings.input_muted = !ac->settings.input_muted;
                    window_invalidate(ac->window);
                    return;
                }
            }
            break;

        case AC_TAB_EQUALIZER:
            if (ry >= content_y - 4 && ry < content_y - 4 + AC_DROPDOWN_HEIGHT &&
                rx >= AC_PADDING + 60 && rx < AC_PADDING + 240) {
                ac->dropdown_open = 2;
                window_invalidate(ac->window);
                return;
            }
            {
                int reset_x = content_w + AC_PADDING - AC_BUTTON_WIDTH;
                if (ry >= content_y - 4 && ry < content_y - 4 + AC_DROPDOWN_HEIGHT &&
                    rx >= reset_x && rx < reset_x + AC_BUTTON_WIDTH) {
                    audio_config_reset_eq(ac);
                    window_invalidate(ac->window);
                    return;
                }
                int toggle_y = content_y + AC_DROPDOWN_HEIGHT + AC_LINE_HEIGHT + AC_EQ_SLIDER_HEIGHT + 45;
                if (ry >= toggle_y - 4 && ry < toggle_y - 4 + AC_DROPDOWN_HEIGHT &&
                    rx >= AC_PADDING + 80 && rx < AC_PADDING + 160) {
                    ac->settings.eq_enabled = !ac->settings.eq_enabled;
                    window_invalidate(ac->window);
                    return;
                }
            }
            break;

        case AC_TAB_ADVANCED:
            if (ry >= content_y + AC_LINE_HEIGHT && ry < content_y + AC_LINE_HEIGHT + AC_DROPDOWN_HEIGHT &&
                rx >= AC_PADDING && rx < AC_PADDING + 200) {
                ac->dropdown_open = 3;
                window_invalidate(ac->window);
                return;
            }
            {
                int bd_y = content_y + AC_LINE_HEIGHT * 2 + AC_DROPDOWN_HEIGHT + AC_LINE_HEIGHT;
                if (ry >= bd_y && ry < bd_y + AC_DROPDOWN_HEIGHT && rx >= AC_PADDING && rx < AC_PADDING + 200) {
                    ac->dropdown_open = 4;
                    window_invalidate(ac->window);
                    return;
                }
                int buf_y = bd_y + AC_DROPDOWN_HEIGHT + AC_LINE_HEIGHT * 2;
                if (ry >= buf_y && ry < buf_y + AC_DROPDOWN_HEIGHT && rx >= AC_PADDING && rx < AC_PADDING + 200) {
                    ac->dropdown_open = 5;
                    window_invalidate(ac->window);
                    return;
                }
                int apply_y = wh - AC_PADDING - AC_BUTTON_HEIGHT;
                int apply_x = content_w + AC_PADDING - AC_BUTTON_WIDTH - 10;
                if (ry >= apply_y && ry < apply_y + AC_BUTTON_HEIGHT && rx >= apply_x && rx < apply_x + AC_BUTTON_WIDTH) {
                    audio_config_apply(ac);
                    window_invalidate(ac->window);
                    return;
                }
            }
            break;

        default: break;
    }
}

static void ac_handle_drag(audio_config_app_t *ac, int32_t mx, int32_t my) {
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(ac->window, &wx, &wy, &ww, &wh);
    int rx = mx - wx, ry = my - wy;
    int content_w = ww - AC_PADDING * 2;

    switch (ac->current_tab) {
        case AC_TAB_OUTPUT: {
            int vol_y = AC_TAB_HEIGHT + AC_PADDING + AC_LINE_HEIGHT * 2 + AC_DROPDOWN_HEIGHT;
            if (ac->active_slider == 1 || (ry >= vol_y && ry < vol_y + AC_SLIDER_HEIGHT &&
                rx >= AC_PADDING && rx < AC_PADDING + content_w - 100)) {
                ac->active_slider = 1;
                int slider_w = content_w - 100 - 10;
                int val = ((rx - AC_PADDING) * 100) / slider_w;
                if (val < 0) val = 0; else if (val > 100) val = 100;
                ac->settings.master_volume = val;
                audio_set_master_volume(val);
                window_invalidate(ac->window);
            }
            int bal_y = vol_y + AC_SLIDER_HEIGHT + AC_LINE_HEIGHT * 2 + 5;
            if (ac->active_slider == 2 || (ry >= bal_y && ry < bal_y + AC_SLIDER_HEIGHT &&
                rx >= AC_PADDING + 20 && rx < AC_PADDING + content_w - 60)) {
                ac->active_slider = 2;
                int slider_w = content_w - 80 - 10;
                int val = ((rx - AC_PADDING - 20) * 100) / slider_w;
                if (val < 0) val = 0; else if (val > 100) val = 100;
                ac->settings.balance = val - 50;
                window_invalidate(ac->window);
            }
            break;
        }
        case AC_TAB_INPUT: {
            int gain_y = AC_TAB_HEIGHT + AC_PADDING + AC_LINE_HEIGHT * 2 + AC_DROPDOWN_HEIGHT;
            if (ac->active_slider == 3 || (ry >= gain_y && ry < gain_y + AC_SLIDER_HEIGHT &&
                rx >= AC_PADDING && rx < AC_PADDING + content_w - 100)) {
                ac->active_slider = 3;
                int slider_w = content_w - 100 - 10;
                int val = ((rx - AC_PADDING) * 100) / slider_w;
                if (val < 0) val = 0; else if (val > 100) val = 100;
                ac->settings.input_gain = val;
                window_invalidate(ac->window);
            }
            break;
        }
        case AC_TAB_EQUALIZER: {
            int eq_y = AC_TAB_HEIGHT + AC_PADDING + AC_DROPDOWN_HEIGHT + AC_LINE_HEIGHT;
            int eq_start_x = AC_PADDING + 35;
            for (int i = 0; i < AC_EQ_BANDS; i++) {
                int bx = eq_start_x + i * AC_EQ_BAND_SPACING;
                if (ac->active_slider == 10 + i || (rx >= bx && rx < bx + AC_EQ_SLIDER_WIDTH &&
                    ry >= eq_y && ry < eq_y + AC_EQ_SLIDER_HEIGHT)) {
                    ac->active_slider = 10 + i;
                    int center_y = eq_y + AC_EQ_SLIDER_HEIGHT / 2;
                    int offset = center_y - ry;
                    int val = (offset * AC_EQ_MAX_DB) / (AC_EQ_SLIDER_HEIGHT / 2 - 4);
                    if (val < AC_EQ_MIN_DB) val = AC_EQ_MIN_DB;
                    if (val > AC_EQ_MAX_DB) val = AC_EQ_MAX_DB;
                    ac->settings.eq_bands[i] = val;
                    ac->settings.eq_preset = AC_EQ_CUSTOM;
                    audio_config_calc_eq_coeffs(&ac->eq_filters_left[i], ac_eq_freqs[i], val, ac_rate_values[ac->settings.sample_rate]);
                    audio_config_calc_eq_coeffs(&ac->eq_filters_right[i], ac_eq_freqs[i], val, ac_rate_values[ac->settings.sample_rate]);
                    window_invalidate(ac->window);
                    break;
                }
            }
            break;
        }
        default: break;
    }
}

static void ac_handle_hover(audio_config_app_t *ac, int32_t mx, int32_t my) {
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(ac->window, &wx, &wy, &ww, &wh);
    int rx = mx - wx, ry = my - wy;

    int old_hover_tab = ac->hover_tab;
    int old_hover_eq = ac->hover_eq_band;
    bool old_hover_mute_o = ac->hover_mute_output;
    bool old_hover_test = ac->hover_test;
    bool old_hover_reset = ac->hover_reset_eq;
    bool old_hover_apply = ac->hover_apply;

    ac->hover_tab = -1;
    ac->hover_eq_band = -1;
    ac->hover_mute_output = false;
    ac->hover_mute_input = false;
    ac->hover_test = false;
    ac->hover_reset_eq = false;
    ac->hover_apply = false;

    if (ry >= 0 && ry < AC_TAB_HEIGHT) {
        int tab_w = ww / AC_TAB_COUNT;
        ac->hover_tab = rx / tab_w;
        if (ac->hover_tab >= AC_TAB_COUNT) ac->hover_tab = -1;
    }

    int content_y = AC_TAB_HEIGHT + AC_PADDING;
    int content_w = ww - AC_PADDING * 2;

    switch (ac->current_tab) {
        case AC_TAB_OUTPUT: {
            int mute_x = content_w + AC_PADDING - 40;
            int mute_y = content_y + AC_LINE_HEIGHT + AC_DROPDOWN_HEIGHT + AC_LINE_HEIGHT * 2 - 6;
            if (rx >= mute_x && rx < mute_x + AC_BUTTON_HEIGHT && ry >= mute_y && ry < mute_y + AC_BUTTON_HEIGHT)
                ac->hover_mute_output = true;
            int test_y = content_y + AC_LINE_HEIGHT * 7 + AC_DROPDOWN_HEIGHT + AC_VU_METER_HEIGHT * 2 + 50;
            if (ry >= test_y && ry < test_y + AC_BUTTON_HEIGHT && rx >= AC_PADDING && rx < AC_PADDING + AC_BUTTON_WIDTH)
                ac->hover_test = true;
            break;
        }
        case AC_TAB_EQUALIZER: {
            int reset_x = content_w + AC_PADDING - AC_BUTTON_WIDTH;
            if (ry >= content_y - 4 && ry < content_y - 4 + AC_DROPDOWN_HEIGHT && rx >= reset_x && rx < reset_x + AC_BUTTON_WIDTH)
                ac->hover_reset_eq = true;
            int eq_y = content_y + AC_DROPDOWN_HEIGHT + AC_LINE_HEIGHT;
            int eq_start_x = AC_PADDING + 35;
            for (int i = 0; i < AC_EQ_BANDS; i++) {
                int bx = eq_start_x + i * AC_EQ_BAND_SPACING;
                if (rx >= bx && rx < bx + AC_EQ_SLIDER_WIDTH && ry >= eq_y && ry < eq_y + AC_EQ_SLIDER_HEIGHT) {
                    ac->hover_eq_band = i;
                    break;
                }
            }
            break;
        }
        case AC_TAB_ADVANCED: {
            int apply_y = wh - AC_PADDING - AC_BUTTON_HEIGHT;
            int apply_x = content_w + AC_PADDING - AC_BUTTON_WIDTH - 10;
            if (ry >= apply_y && ry < apply_y + AC_BUTTON_HEIGHT && rx >= apply_x && rx < apply_x + AC_BUTTON_WIDTH)
                ac->hover_apply = true;
            break;
        }
        default: break;
    }

    if (ac->hover_tab != old_hover_tab || ac->hover_eq_band != old_hover_eq ||
        ac->hover_mute_output != old_hover_mute_o || ac->hover_test != old_hover_test ||
        ac->hover_reset_eq != old_hover_reset || ac->hover_apply != old_hover_apply) {
        window_invalidate(ac->window);
    }
}

void audio_config_on_event(void *app_data, gui_event_t *event) {
    audio_config_app_t *ac = (audio_config_app_t *)app_data;
    if (!ac || !ac->window) return;

    switch (event->type) {
        case EVENT_MOUSE_MOVE:
            if (ac->dragging_slider) ac_handle_drag(ac, event->mouse_x, event->mouse_y);
            else ac_handle_hover(ac, event->mouse_x, event->mouse_y);
            break;
        case EVENT_MOUSE_DOWN:
            if (event->mouse_buttons & MOUSE_BUTTON_LEFT) {
                ac->dragging_slider = true;
                ac_handle_drag(ac, event->mouse_x, event->mouse_y);
                if (ac->active_slider == 0) {
                    ac->dragging_slider = false;
                    ac_handle_click(ac, event->mouse_x, event->mouse_y);
                }
            }
            break;
        case EVENT_MOUSE_UP:
            ac->dragging_slider = false;
            ac->active_slider = 0;
            break;
        case EVENT_WINDOW_CLOSE:
            ac->running = false;
            break;
        case EVENT_REDRAW:
            audio_config_draw(ac);
            break;
        default: break;
    }
}

void audio_config_on_draw(void *app_data) {
    audio_config_app_t *ac = (audio_config_app_t *)app_data;
    if (!ac) return;
    uint64_t now = timer_get_ticks();
    int32_t dt = (int32_t)((now - ac->last_update_tick) * AC_FP_ONE / 1000);
    ac->last_update_tick = now;
    if (ac->test_playing) {
        ac->output_left.current_level = AC_FP_HALF + (int32_t)(((now % 200) * AC_FP_ONE / 10) / 200);
        ac->output_right.current_level = AC_FP_HALF + (int32_t)(((now % 230) * AC_FP_ONE / 10) / 230);
    } else {
        audio_config_decay_vu(&ac->output_left, dt);
        audio_config_decay_vu(&ac->output_right, dt);
    }
    audio_config_decay_vu(&ac->input_level, dt);
    audio_config_draw(ac);
}

void audio_config_on_destroy(void *app_data) {
    audio_config_app_t *ac = (audio_config_app_t *)app_data;
    if (ac) {
        audio_config_stop_test(ac);
        kfree(ac);
    }
    g_audio_config = NULL;
}

// Creation and Lifecycle
audio_config_app_t *audio_config_create(void) {
    if (g_audio_config) {
        window_bring_to_front(g_audio_config->window);
        return g_audio_config;
    }

    audio_config_app_t *ac = (audio_config_app_t *)kmalloc(sizeof(audio_config_app_t));
    if (!ac) {
        serial_printf(0x3F8, "[AUDIO_CONFIG] Failed to allocate memory\n");
        return NULL;
    }

    memset(ac, 0, sizeof(audio_config_app_t));
    ac->window = window_create("Audio Configuration", 100, 100, AC_WINDOW_WIDTH, AC_WINDOW_HEIGHT);
    if (!ac->window) {
        serial_printf(0x3F8, "[AUDIO_CONFIG] Failed to create window\n");
        kfree(ac);
        return NULL;
    }

    audio_config_reset_defaults(ac);
    audio_config_load_settings(ac);

    ac->output_left.decay_rate = AC_FP_ONE * 3;
    ac->output_right.decay_rate = AC_FP_ONE * 3;
    ac->input_level.decay_rate = AC_FP_ONE * 3;

    ac->current_tab = AC_TAB_OUTPUT;
    ac->dropdown_open = -1;
    ac->hover_tab = -1;
    ac->hover_eq_band = -1;
    ac->running = true;
    ac->last_update_tick = timer_get_ticks();

    ac->app_id = wm_register_app(ac->window, ac, audio_config_on_event, audio_config_on_draw, audio_config_on_destroy);
    if (ac->app_id < 0) {
        serial_printf(0x3F8, "[AUDIO_CONFIG] Failed to register with WM\n");
        window_destroy(ac->window);
        kfree(ac);
        return NULL;
    }

    ac->dock_index = -1;
    g_audio_config = ac;
    serial_printf(0x3F8, "[AUDIO_CONFIG] Created successfully\n");
    return ac;
}

void audio_config_destroy(audio_config_app_t *ac) {
    if (!ac) return;
    audio_config_stop_test(ac);
    audio_config_save_settings(ac);
    if (ac->app_id >= 0) wm_unregister_app(ac->app_id);
    if (ac->window) window_destroy(ac->window);
    kfree(ac);
    g_audio_config = NULL;
}

void audio_config_run(audio_config_app_t *ac) {
    if (!ac) return;
    while (ac->running) wm_process_frame();
}

void audio_config_launch(void) {
    audio_config_create();
}

// Settings Management
void audio_config_reset_defaults(audio_config_app_t *ac) {
    if (!ac) return;
    ac->settings.output_device = AC_OUTPUT_SPEAKERS;
    ac->settings.master_volume = 75;
    ac->settings.balance = 0;
    ac->settings.output_muted = false;
    ac->settings.input_device = AC_INPUT_MICROPHONE;
    ac->settings.input_gain = 50;
    ac->settings.input_muted = false;
    ac->settings.eq_preset = AC_EQ_FLAT;
    ac->settings.eq_enabled = true;
    for (int i = 0; i < AC_EQ_BANDS; i++) ac->settings.eq_bands[i] = 0;
    ac->settings.sample_rate = AC_RATE_48000;
    ac->settings.bit_depth = AC_BITS_16;
    ac->settings.buffer_size = AC_BUFFER_512;

    int32_t sr = ac_rate_values[ac->settings.sample_rate];
    for (int i = 0; i < AC_EQ_BANDS; i++) {
        audio_config_calc_eq_coeffs(&ac->eq_filters_left[i], ac_eq_freqs[i], 0, sr);
        audio_config_calc_eq_coeffs(&ac->eq_filters_right[i], ac_eq_freqs[i], 0, sr);
    }
}

void audio_config_load_settings(audio_config_app_t *ac) { (void)ac; }
void audio_config_save_settings(audio_config_app_t *ac) { (void)ac; }

void audio_config_apply(audio_config_app_t *ac) {
    if (!ac) return;
    audio_set_master_volume(ac->settings.output_muted ? 0 : ac->settings.master_volume);
    audio_mute(ac->settings.output_muted);
    serial_printf(0x3F8, "[AUDIO_CONFIG] Settings applied\n");
}

// Equalizer Functions (Fixed-point)
void audio_config_apply_preset(audio_config_app_t *ac, ac_eq_preset_t preset) {
    if (!ac || preset >= AC_EQ_PRESET_COUNT - 1) return;
    ac->settings.eq_preset = preset;
    for (int i = 0; i < AC_EQ_BANDS; i++) ac->settings.eq_bands[i] = ac_eq_presets[preset][i];
    int32_t sr = ac_rate_values[ac->settings.sample_rate];
    for (int i = 0; i < AC_EQ_BANDS; i++) {
        audio_config_calc_eq_coeffs(&ac->eq_filters_left[i], ac_eq_freqs[i], ac->settings.eq_bands[i], sr);
        audio_config_calc_eq_coeffs(&ac->eq_filters_right[i], ac_eq_freqs[i], ac->settings.eq_bands[i], sr);
    }
    window_invalidate(ac->window);
}

void audio_config_reset_eq(audio_config_app_t *ac) {
    audio_config_apply_preset(ac, AC_EQ_FLAT);
}

void audio_config_calc_eq_coeffs(ac_biquad_t *filter, int32_t freq, int32_t gain_db, int32_t sample_rate) {
    if (!filter) return;
    (void)freq; (void)sample_rate;
    filter->gain = AC_FP_ONE + (gain_db * AC_FP_ONE) / 10;
    filter->b0 = filter->gain;
    filter->b1 = 0; filter->b2 = 0;
    filter->a1 = 0; filter->a2 = 0;
    filter->x1 = 0; filter->x2 = 0;
    filter->y1 = 0; filter->y2 = 0;
}

static int32_t biquad_process_fp(ac_biquad_t *f, int32_t x) {
    return fp_mul(f->gain, x);
}

void audio_config_process_eq(audio_config_app_t *ac, int16_t *samples, uint32_t count, bool stereo) {
    if (!ac || !samples || count == 0 || !ac->settings.eq_enabled) return;
    if (stereo) {
        for (uint32_t i = 0; i < count; i += 2) {
            int32_t left = (int32_t)samples[i] << 8;
            int32_t right = (int32_t)samples[i + 1] << 8;
            for (int b = 0; b < AC_EQ_BANDS; b++) {
                if (ac->settings.eq_bands[b] != 0) {
                    left = biquad_process_fp(&ac->eq_filters_left[b], left);
                    right = biquad_process_fp(&ac->eq_filters_right[b], right);
                }
            }
            left >>= 8; right >>= 8;
            if (left > 32767) left = 32767; else if (left < -32768) left = -32768;
            if (right > 32767) right = 32767; else if (right < -32768) right = -32768;
            samples[i] = (int16_t)left;
            samples[i + 1] = (int16_t)right;
        }
    } else {
        for (uint32_t i = 0; i < count; i++) {
            int32_t mono = (int32_t)samples[i] << 8;
            for (int b = 0; b < AC_EQ_BANDS; b++) {
                if (ac->settings.eq_bands[b] != 0) mono = biquad_process_fp(&ac->eq_filters_left[b], mono);
            }
            mono >>= 8;
            if (mono > 32767) mono = 32767; else if (mono < -32768) mono = -32768;
            samples[i] = (int16_t)mono;
        }
    }
}

// VU Meter Functions (Fixed-point)
void audio_config_update_vu(ac_vu_meter_t *vu, int32_t level) {
    if (!vu) return;
    if (level < 0) level = 0;
    if (level > AC_FP_ONE) level = AC_FP_ONE;
    vu->current_level = level;
    if (level > vu->peak_level) {
        vu->peak_level = level;
        vu->peak_hold_time = 30;
    }
}

void audio_config_decay_vu(ac_vu_meter_t *vu, int32_t dt_fp) {
    if (!vu) return;
    vu->current_level -= fp_mul(vu->decay_rate, dt_fp);
    if (vu->current_level < 0) vu->current_level = 0;
    if (vu->peak_hold_time > 0) vu->peak_hold_time--;
    else {
        vu->peak_level -= fp_mul(vu->decay_rate, dt_fp) / 2;
        if (vu->peak_level < 0) vu->peak_level = 0;
    }
}

// Test Tone
static const int8_t sine_table[256] = {
    0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42, 45,
    48, 51, 54, 57, 59, 62, 65, 67, 70, 73, 75, 78, 80, 82, 85, 87,
    89, 91, 94, 96, 98, 100, 102, 103, 105, 107, 108, 110, 112, 113, 114, 116,
    117, 118, 119, 120, 121, 122, 123, 123, 124, 125, 125, 126, 126, 126, 126, 126,
    127, 126, 126, 126, 126, 126, 125, 125, 124, 123, 123, 122, 121, 120, 119, 118,
    117, 116, 114, 113, 112, 110, 108, 107, 105, 103, 102, 100, 98, 96, 94, 91,
    89, 87, 85, 82, 80, 78, 75, 73, 70, 67, 65, 62, 59, 57, 54, 51,
    48, 45, 42, 39, 36, 33, 30, 27, 24, 21, 18, 15, 12, 9, 6, 3,
    0, -3, -6, -9, -12, -15, -18, -21, -24, -27, -30, -33, -36, -39, -42, -45,
    -48, -51, -54, -57, -59, -62, -65, -67, -70, -73, -75, -78, -80, -82, -85, -87,
    -89, -91, -94, -96, -98, -100, -102, -103, -105, -107, -108, -110, -112, -113, -114, -116,
    -117, -118, -119, -120, -121, -122, -123, -123, -124, -125, -125, -126, -126, -126, -126, -126,
    -127, -126, -126, -126, -126, -126, -125, -125, -124, -123, -123, -122, -121, -120, -119, -118,
    -117, -116, -114, -113, -112, -110, -108, -107, -105, -103, -102, -100, -98, -96, -94, -91,
    -89, -87, -85, -82, -80, -78, -75, -73, -70, -67, -65, -62, -59, -57, -54, -51,
    -48, -45, -42, -39, -36, -33, -30, -27, -24, -21, -18, -15, -12, -9, -6, -3
};

static int16_t *g_test_tone_buffer = NULL;

void audio_config_play_test_tone(audio_config_app_t *ac) {
    if (!ac || ac->test_playing) return;
    uint32_t sample_rate = ac_rate_values[ac->settings.sample_rate];
    uint32_t duration_samples = sample_rate * 2;
    uint32_t buffer_size = duration_samples * 2 * sizeof(int16_t);

    if (!g_test_tone_buffer) {
        g_test_tone_buffer = (int16_t *)kmalloc(buffer_size);
        if (!g_test_tone_buffer) {
            serial_printf(0x3F8, "[AUDIO_CONFIG] Failed to allocate test tone buffer\n");
            return;
        }
    }

    uint32_t frequency = 440;
    uint32_t phase_increment = (frequency * 256) / sample_rate;
    uint32_t phase = 0;

    for (uint32_t i = 0; i < duration_samples; i++) {
        int16_t sample = (int16_t)sine_table[phase & 0xFF] * 200;
        sample = (sample * ac->settings.master_volume) / 100;
        int16_t left = sample, right = sample;
        if (ac->settings.balance < 0) right = (right * (50 + ac->settings.balance)) / 50;
        else if (ac->settings.balance > 0) left = (left * (50 - ac->settings.balance)) / 50;
        g_test_tone_buffer[i * 2] = left;
        g_test_tone_buffer[i * 2 + 1] = right;
        phase += phase_increment;
    }

    if (ac->settings.eq_enabled) audio_config_process_eq(ac, g_test_tone_buffer, duration_samples * 2, true);

    ac->test_playing = true;
    int result = audio_play_buffer(g_test_tone_buffer, buffer_size, AUDIO_FORMAT_S16_LE, sample_rate, 2);
    if (result != AUDIO_OK) {
        serial_printf(0x3F8, "[AUDIO_CONFIG] Failed to play test tone\n");
        ac->test_playing = false;
    } else {
        serial_printf(0x3F8, "[AUDIO_CONFIG] Playing test tone (440Hz)\n");
    }
}

void audio_config_stop_test(audio_config_app_t *ac) {
    if (!ac) return;
    ac->test_playing = false;
    if (g_test_tone_buffer) {
        kfree(g_test_tone_buffer);
        g_test_tone_buffer = NULL;
    }
    serial_printf(0x3F8, "[AUDIO_CONFIG] Test tone stopped\n");
}
