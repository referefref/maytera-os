// Suppress warnings
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
// panel_sound.c - Sound Settings Panel for MayteraOS Settings App
// Part of the unified Settings application framework (Task #59)
#include "panel_sound.h"
#include "settings_widgets.h"
#include "../window.h"
#include "../../types.h"
#include "../../serial.h"
#include "../../mm/heap.h"
#include "../../string.h"
#include "../../video/framebuffer.h"
#include "../../video/font.h"
#include "../../drivers/audio.h"
#include "../themes.h"

#define SOUND_PANEL_PADDING         16
#define SOUND_SECTION_SPACING       24
#define SOUND_LINE_HEIGHT           24
#define SOUND_BUTTON_HEIGHT         28
#define SLIDER_WIDTH                200
#define SLIDER_KNOB_HEIGHT          20
#define VOLUME_BAR_WIDTH            150
#define VOLUME_BAR_HEIGHT           8

typedef struct {
    const char *name;
    const char *filename;
} alert_sound_t;

static const alert_sound_t g_alert_sounds[] = {
    {"Default", "ALERT.WAV"}, {"Chime", "CHIME.WAV"}, {"Beep", "BEEP.WAV"},
    {"Ping", "PING.WAV"}, {"Click", "CLICK.WAV"}, {"None", NULL}, {NULL, NULL}
};

typedef struct {
    const char *name;
    bool enabled;
    int sound_index;
} event_sound_t;

static event_sound_t g_event_sounds[] = {
    {"Startup", true, 0}, {"Shutdown", true, 0}, {"Error", true, 3},
    {"Warning", true, 3}, {"Notification", true, 1}, {NULL, false, 0}
};

typedef struct {
    bool initialized;
    int output_device;
    int master_volume;
    int balance;
    bool muted;
    bool system_sounds_enabled;
    int alert_sound_index;
    struct { const char *name; int volume; bool active; } app_volumes[8];
    int app_count;
    audio_device_info_t device_info;
    bool audio_available;
    int scroll_offset;
    int max_scroll;
    bool dragging_volume;
    bool dragging_balance;
} sound_panel_state_t;

static sound_panel_state_t g_state = {0};

static void draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    while (*text) {
        const uint8_t *glyph = font_get_glyph(*text);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) fb_put_pixel(x + col, y + row, color);
                }
            }
        }
        x += FONT_WIDTH;
        text++;
    }
}

static void draw_button(int32_t x, int32_t y, int32_t w, int32_t h,
                       const char *text, bool pressed, bool enabled) {
    uint32_t bg = !enabled ? 0xA0A0A0 : (pressed ? 0x808080 : 0xC0C0C0);
    fb_fill_rect(x, y, w, h, bg);
    fb_draw_rect(x, y, w, h, !enabled ? 0x808080 : THEME_MENU_TEXT_DISABLED);
    draw_text(x + (w - strlen(text) * FONT_WIDTH) / 2, y + (h - FONT_HEIGHT) / 2,
             text, !enabled ? THEME_MENU_TEXT_DISABLED : THEME_LABEL_TEXT);
}

static void draw_checkbox(int32_t x, int32_t y, const char *label, bool checked) {
    fb_fill_rect(x, y, 16, 16, THEME_TEXTBOX_BG);
    fb_draw_rect(x, y, 16, 16, THEME_MENU_TEXT_DISABLED);
    if (checked) {
        for (int i = 0; i < 4; i++) { fb_put_pixel(x + 3 + i, y + 7 + i, THEME_LABEL_TEXT); fb_put_pixel(x + 4 + i, y + 7 + i, THEME_LABEL_TEXT); }
        for (int i = 0; i < 6; i++) { fb_put_pixel(x + 6 + i, y + 10 - i, THEME_LABEL_TEXT); fb_put_pixel(x + 7 + i, y + 10 - i, THEME_LABEL_TEXT); }
    }
    draw_text(x + 24, y + 2, label, THEME_LABEL_TEXT);
}

static void draw_dropdown(int32_t x, int32_t y, int32_t w, const char *value) {
    fb_fill_rect(x, y, w, SOUND_BUTTON_HEIGHT, THEME_TEXTBOX_BG);
    fb_draw_rect(x, y, w, SOUND_BUTTON_HEIGHT, THEME_MENU_TEXT_DISABLED);
    draw_text(x + 8, y + 7, value, THEME_LABEL_TEXT);
    for (int i = 0; i < 5; i++) { fb_put_pixel(x + w - 18 + i, y + 12 + i/2, 0x404040); fb_put_pixel(x + w - 10 - i, y + 12 + i/2, 0x404040); }
}

static void draw_slider(int32_t x, int32_t y, int32_t w, int value, bool enabled) {
    fb_fill_rect(x, y + 4, w, 12, enabled ? 0xC0C0C0 : 0xD0D0D0);
    fb_draw_rect(x, y + 4, w, 12, 0x808080);
    int fill_w = (value * (w - 12)) / 100;
    if (fill_w > 0) fb_fill_rect(x + 1, y + 5, fill_w, 10, enabled ? 0x4A90C2 : 0x808080);
    fb_fill_rect(x + fill_w, y, 12, SLIDER_KNOB_HEIGHT, enabled ? 0x505050 : 0x808080);
    fb_draw_rect(x + fill_w, y, 12, SLIDER_KNOB_HEIGHT, 0x404040);
}

static void draw_speaker_icon(int32_t x, int32_t y, bool muted) {
    uint32_t c = muted ? 0xA0A0A0 : 0x404040;
    fb_fill_rect(x, y + 4, 6, 8, c);
    fb_fill_rect(x + 6, y + 2, 4, 12, c);
    if (!muted) for (int i = 0; i < 3; i++) fb_draw_rect(x + 14 + i * 4, y + 8 - (3 + i * 2), 2, 6 + i * 4, 0x4A90C2);
    else { for (int i = 0; i < 8; i++) { fb_put_pixel(x + 14 + i, y + 4 + i, 0xE04040); fb_put_pixel(x + 14 + i, y + 11 - i, 0xE04040); } }
}

static void draw_volume_bar(int32_t x, int32_t y, int value) {
    fb_fill_rect(x, y, VOLUME_BAR_WIDTH, VOLUME_BAR_HEIGHT, 0xE0E0E0);
    fb_draw_rect(x, y, VOLUME_BAR_WIDTH, VOLUME_BAR_HEIGHT, 0x808080);
    int fill_w = (value * (VOLUME_BAR_WIDTH - 2)) / 100;
    if (fill_w > 0) {
        uint32_t fill_c = (value > 80) ? 0xE04040 : ((value > 60) ? 0xE0A040 : 0x4A90C2);
        fb_fill_rect(x + 1, y + 1, fill_w, VOLUME_BAR_HEIGHT - 2, fill_c);
    }
}

static int draw_output_section(int32_t x, int32_t y, int32_t w) {
    int32_t cy = y;
    draw_text(x, cy, "Output", 0x0066CC);
    cy += SOUND_LINE_HEIGHT + 8;

    draw_text(x, cy + 5, "Device:", THEME_LABEL_TEXT);
    draw_dropdown(x + 60, cy, 200, g_state.audio_available ? g_state.device_info.name : "No audio device");
    cy += SOUND_LINE_HEIGHT + 15;

    draw_text(x, cy + 2, "Volume:", THEME_LABEL_TEXT);
    draw_speaker_icon(x + 60, cy - 2, g_state.muted);
    draw_slider(x + 90, cy, SLIDER_WIDTH, g_state.master_volume, !g_state.muted);
    char vol_str[8]; snprintf(vol_str, sizeof(vol_str), "%d%%", g_state.master_volume);
    draw_text(x + 90 + SLIDER_WIDTH + 10, cy + 3, vol_str, THEME_LABEL_TEXT);
    cy += SLIDER_KNOB_HEIGHT + 10;

    draw_checkbox(x + 90, cy, "Mute", g_state.muted);
    cy += SOUND_LINE_HEIGHT + 10;

    draw_text(x, cy + 2, "Balance:", THEME_LABEL_TEXT);
    draw_text(x + 60, cy + 2, "L", THEME_MENU_TEXT_DISABLED);
    draw_slider(x + 75, cy, SLIDER_WIDTH, g_state.balance + 50, true);
    draw_text(x + 75 + SLIDER_WIDTH + 5, cy + 2, "R", THEME_MENU_TEXT_DISABLED);
    cy += SLIDER_KNOB_HEIGHT + 10;

    draw_button(x, cy, 100, SOUND_BUTTON_HEIGHT, "Test Sound", false, g_state.audio_available);
    return cy - y + SOUND_BUTTON_HEIGHT;
}

static int draw_effects_section(int32_t x, int32_t y, int32_t w) {
    int32_t cy = y;
    draw_text(x, cy, "System Sounds", 0x0066CC);
    cy += SOUND_LINE_HEIGHT + 8;

    draw_checkbox(x, cy, "Enable system sounds", g_state.system_sounds_enabled);
    cy += SOUND_LINE_HEIGHT + 10;

    draw_text(x, cy + 5, "Alert sound:", THEME_LABEL_TEXT);
    draw_dropdown(x + 100, cy, 150, g_alert_sounds[g_state.alert_sound_index].name);
    draw_button(x + 260, cy, 50, SOUND_BUTTON_HEIGHT, "Play", false, g_state.audio_available);
    cy += SOUND_LINE_HEIGHT + 15;

    draw_text(x, cy, "Event Sounds:", THEME_LABEL_TEXT);
    cy += SOUND_LINE_HEIGHT + 4;
    for (int i = 0; g_event_sounds[i].name; i++) {
        draw_checkbox(x + 10, cy, g_event_sounds[i].name, g_event_sounds[i].enabled);
        if (g_event_sounds[i].enabled)
            draw_dropdown(x + 150, cy - 2, 100, g_alert_sounds[g_event_sounds[i].sound_index].name);
        cy += SOUND_LINE_HEIGHT + 4;
    }
    return cy - y;
}

static int draw_applications_section(int32_t x, int32_t y, int32_t w) {
    int32_t cy = y;
    draw_text(x, cy, "Application Volumes", 0x0066CC);
    cy += SOUND_LINE_HEIGHT + 8;
    if (g_state.app_count == 0) {
        draw_text(x, cy, "No applications playing audio", 0x808080);
        return cy - y + SOUND_LINE_HEIGHT;
    }
    for (int i = 0; i < g_state.app_count; i++) {
        if (!g_state.app_volumes[i].active) continue;
        draw_text(x, cy, g_state.app_volumes[i].name, THEME_LABEL_TEXT);
        draw_volume_bar(x + 100, cy + 3, g_state.app_volumes[i].volume);
        char vol_str[8]; snprintf(vol_str, sizeof(vol_str), "%d%%", g_state.app_volumes[i].volume);
        draw_text(x + 100 + VOLUME_BAR_WIDTH + 10, cy, vol_str, THEME_MENU_TEXT_DISABLED);
        cy += SOUND_LINE_HEIGHT + 6;
    }
    return cy - y;
}

static int draw_device_info(int32_t x, int32_t y, int32_t w) {
    int32_t cy = y;
    draw_text(x, cy, "Audio Device Information", 0x0066CC);
    cy += SOUND_LINE_HEIGHT + 8;
    if (!g_state.audio_available) {
        draw_text(x, cy, "No audio hardware detected", 0xCC0000);
        cy += SOUND_LINE_HEIGHT;
        draw_text(x, cy, "PC Speaker will be used for beeps", THEME_MENU_TEXT_DISABLED);
        return cy - y + SOUND_LINE_HEIGHT;
    }
    char info[128];
    snprintf(info, sizeof(info), "Device: %s", g_state.device_info.name);
    draw_text(x, cy, info, THEME_LABEL_TEXT); cy += SOUND_LINE_HEIGHT;
    if (g_state.device_info.description) { draw_text(x, cy, g_state.device_info.description, THEME_MENU_TEXT_DISABLED); cy += SOUND_LINE_HEIGHT; }
    snprintf(info, sizeof(info), "Sample Rates: %u - %u Hz", g_state.device_info.min_sample_rate, g_state.device_info.max_sample_rate);
    draw_text(x, cy, info, THEME_LABEL_TEXT); cy += SOUND_LINE_HEIGHT;
    snprintf(info, sizeof(info), "Max Channels: %u", g_state.device_info.max_channels);
    draw_text(x, cy, info, THEME_LABEL_TEXT);
    return cy - y + SOUND_LINE_HEIGHT;
}

// Framework callbacks
static void sound_panel_init_internal(settings_panel_t *panel) { sound_panel_init(); }
static void sound_panel_draw_internal(settings_panel_t *panel, int32_t x, int32_t y, int32_t w, int32_t h) {
    sound_panel_draw(x, y, w, h);
}
static void sound_panel_event_internal(settings_panel_t *panel, gui_event_t *event) {
    if (event->type == EVENT_MOUSE_UP) {
        sound_panel_handle_click(panel->content_x, panel->content_y,
                                panel->content_width, panel->content_height_visible,
                                event->mouse_x, event->mouse_y);
    } else if (event->type == EVENT_MOUSE_SCROLL) {
        sound_panel_handle_scroll(event->scroll_delta);
    }
}
static void sound_panel_apply_internal(settings_panel_t *panel) { sound_panel_apply_volume(); }
static void sound_panel_cleanup_internal(settings_panel_t *panel) { sound_panel_cleanup(); }

static settings_panel_def_t g_sound_panel_def = {
    .name = "Sound",
    .icon = "volume",
    .category = SETTINGS_CAT_SYSTEM,
    .priority = 10,
    .init = sound_panel_init_internal,
    .draw = sound_panel_draw_internal,
    .handle_event = sound_panel_event_internal,
    .apply = sound_panel_apply_internal,
    .cleanup = sound_panel_cleanup_internal
};

void sound_panel_init(void) {
    if (g_state.initialized) return;
    g_state.output_device = 0;
    g_state.master_volume = 80;
    g_state.balance = 0;
    g_state.muted = false;
    g_state.system_sounds_enabled = true;
    g_state.alert_sound_index = 0;
    g_state.app_count = 0;
    g_state.audio_available = audio_is_available();
    if (g_state.audio_available) {
        audio_get_device_info(&g_state.device_info);
        audio_volume_t vol;
        if (audio_get_volume(&vol) == AUDIO_OK) {
            g_state.master_volume = (vol.master_left + vol.master_right) / 2;
            g_state.balance = (vol.master_right - vol.master_left) / 2;
            g_state.muted = vol.master_mute;
        }
    }
    g_state.initialized = true;
    kprintf("[SoundPanel] Initialized, audio available: %s\n", g_state.audio_available ? "yes" : "no");
}

void sound_panel_draw(int32_t panel_x, int32_t panel_y, int32_t panel_width, int32_t panel_height) {
    if (!g_state.initialized) sound_panel_init();
    int32_t cy = panel_y + SOUND_PANEL_PADDING;
    int32_t cx = panel_x + SOUND_PANEL_PADDING;
    int32_t cw = panel_width - SOUND_PANEL_PADDING * 2;

    cy += draw_output_section(cx, cy, cw);
    cy += SOUND_SECTION_SPACING;
    fb_fill_rect(cx, cy - 12, cw, 1, 0xC0C0C0);

    cy += draw_effects_section(cx, cy, cw);
    cy += SOUND_SECTION_SPACING;
    fb_fill_rect(cx, cy - 12, cw, 1, 0xC0C0C0);

    cy += draw_applications_section(cx, cy, cw);
    cy += SOUND_SECTION_SPACING;
    fb_fill_rect(cx, cy - 12, cw, 1, 0xC0C0C0);

    cy += draw_device_info(cx, cy, cw);
    g_state.max_scroll = (cy - panel_y > panel_height) ? (cy - panel_y - panel_height) : 0;
}

bool sound_panel_handle_click(int32_t panel_x, int32_t panel_y,
                              int32_t panel_width, int32_t panel_height,
                              int32_t click_x, int32_t click_y) {
    int32_t lx = click_x - panel_x;
    int32_t ly = click_y - panel_y;
    int32_t cy = SOUND_PANEL_PADDING + SOUND_LINE_HEIGHT + 8 + SOUND_LINE_HEIGHT + 15;

    // Volume slider
    int32_t slider_x = SOUND_PANEL_PADDING + 90;
    if (lx >= slider_x && lx < slider_x + SLIDER_WIDTH && ly >= cy && ly < cy + SLIDER_KNOB_HEIGHT) {
        g_state.master_volume = ((lx - slider_x) * 100) / SLIDER_WIDTH;
        if (g_state.master_volume < 0) g_state.master_volume = 0;
        if (g_state.master_volume > 100) g_state.master_volume = 100;
        sound_panel_apply_volume();
        return true;
    }
    cy += SLIDER_KNOB_HEIGHT + 10;

    // Mute checkbox
    if (lx >= slider_x && lx < slider_x + 80 && ly >= cy && ly < cy + SOUND_LINE_HEIGHT) {
        g_state.muted = !g_state.muted;
        sound_panel_apply_volume();
        return true;
    }
    cy += SOUND_LINE_HEIGHT + 10;

    // Balance slider
    int32_t bal_x = SOUND_PANEL_PADDING + 75;
    if (lx >= bal_x && lx < bal_x + SLIDER_WIDTH && ly >= cy && ly < cy + SLIDER_KNOB_HEIGHT) {
        g_state.balance = ((lx - bal_x) * 100) / SLIDER_WIDTH - 50;
        if (g_state.balance < -50) g_state.balance = -50;
        if (g_state.balance > 50) g_state.balance = 50;
        sound_panel_apply_volume();
        return true;
    }
    cy += SLIDER_KNOB_HEIGHT + 10;

    // Test sound button
    if (lx >= SOUND_PANEL_PADDING && lx < SOUND_PANEL_PADDING + 100 && ly >= cy && ly < cy + SOUND_BUTTON_HEIGHT) {
        sound_panel_test();
        return true;
    }
    return false;
}

void sound_panel_handle_mouse_move(int32_t panel_x, int32_t panel_y,
                                   int32_t panel_width, int32_t panel_height,
                                   int32_t mouse_x, int32_t mouse_y) {}

void sound_panel_handle_scroll(int32_t delta) {
    g_state.scroll_offset -= delta * 20;
    if (g_state.scroll_offset < 0) g_state.scroll_offset = 0;
    if (g_state.scroll_offset > g_state.max_scroll) g_state.scroll_offset = g_state.max_scroll;
}

void sound_panel_apply_volume(void) {
    if (!g_state.audio_available) return;
    kprintf("[SoundPanel] Applying volume: %d%%, balance: %d, muted: %d\n",
           g_state.master_volume, g_state.balance, g_state.muted);
    int vol = g_state.master_volume;
    int left_vol = vol, right_vol = vol;
    if (g_state.balance < 0) right_vol = vol * (50 + g_state.balance) / 50;
    else if (g_state.balance > 0) left_vol = vol * (50 - g_state.balance) / 50;
    audio_volume_t new_vol = { .master_left = left_vol, .master_right = right_vol,
                               .pcm_left = left_vol, .pcm_right = right_vol,
                               .master_mute = g_state.muted, .pcm_mute = g_state.muted };
    audio_set_volume(&new_vol);
}

void sound_panel_test(void) {
    if (!g_state.audio_available) return;
    kprintf("[SoundPanel] Playing test sound\n");
    audio_beep(440, 200);
}

void sound_panel_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    g_state.master_volume = volume;
    sound_panel_apply_volume();
}

void sound_panel_set_mute(bool mute) {
    g_state.muted = mute;
    sound_panel_apply_volume();
}

int sound_panel_get_volume(void) { return g_state.master_volume; }
bool sound_panel_is_muted(void) { return g_state.muted; }
int sound_panel_get_balance(void) { return g_state.balance; }
bool sound_panel_are_system_sounds_enabled(void) { return g_state.system_sounds_enabled; }
int sound_panel_get_alert_sound(void) { return g_state.alert_sound_index; }
bool sound_panel_is_audio_available(void) { return g_state.audio_available; }
void sound_panel_cleanup(void) { g_state.initialized = false; }

int sound_panel_register(void) { return settings_register_panel(&g_sound_panel_def); }
const settings_panel_def_t *sound_panel_get_def(void) { return &g_sound_panel_def; }
