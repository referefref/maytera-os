// Suppress warnings
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
// panel_keyboard.c - Keyboard Panel for MayteraOS Unified Settings
// Provides keyboard layout selection, typing preferences, and shortcut customization

#include "settings_panel.h"
#include "settings_widgets.h"
#include "../window.h"
#include "../desktop.h"
#include "../../types.h"
#include "../../serial.h"
#include "../../mm/heap.h"
#include "../../string.h"
#include "../../video/framebuffer.h"
#include "../../video/font.h"
#include "../../cpu/isr.h"
#include "../themes.h"

// ============================================================================
// Keyboard Structures
// ============================================================================

#define MAX_SHORTCUTS           16
#define MAX_SHORTCUT_NAME_LEN   32

typedef enum {
    LAYOUT_US = 0,
    LAYOUT_UK,
    LAYOUT_DE,
    LAYOUT_FR,
    LAYOUT_ES,
    LAYOUT_DVORAK
} keyboard_layout_t;

typedef struct {
    keyboard_layout_t id;
    const char *name;
    const char *code;
} layout_info_t;

typedef struct {
    char name[MAX_SHORTCUT_NAME_LEN];
    uint8_t modifiers;
    uint8_t key;
} keyboard_shortcut_t;

#define MOD_CTRL    (1 << 0)
#define MOD_ALT     (1 << 1)
#define MOD_SHIFT   (1 << 2)

// Panel data
typedef struct {
    keyboard_layout_t current_layout;
    int repeat_delay;
    int repeat_rate;
    bool num_lock;
    bool caps_lock;
    bool show_indicator;

    keyboard_shortcut_t shortcuts[MAX_SHORTCUTS];
    int shortcut_count;
    int selected_shortcut;

    int sub_section;  // 0=Layout, 1=Typing, 2=Shortcuts
    char test_buffer[128];
    int test_cursor;
} keyboard_panel_data_t;

// Layout definitions
static const layout_info_t g_layouts[] = {
    { LAYOUT_US,     "US English",   "en-US" },
    { LAYOUT_UK,     "UK English",   "en-GB" },
    { LAYOUT_DE,     "German",       "de-DE" },
    { LAYOUT_FR,     "French",       "fr-FR" },
    { LAYOUT_ES,     "Spanish",      "es-ES" },
    { LAYOUT_DVORAK, "Dvorak",       "dvorak" },
};
#define NUM_LAYOUTS 6

// ============================================================================
// Layout Constants
// ============================================================================

#define SECTION_TAB_WIDTH   80
#define SECTION_TAB_HEIGHT  24
#define LAYOUT_ITEM_WIDTH   130
#define LAYOUT_ITEM_HEIGHT  28
#define SLIDER_WIDTH        200
#define SLIDER_HEIGHT       16
#define SHORTCUT_ITEM_HEIGHT 22

// Colors
#define COLOR_TAB_ACTIVE    0x4A90C2
#define COLOR_TAB_INACTIVE  0xE0E0E0
#define COLOR_LAYOUT_SEL    0x4A90C2
#define COLOR_LAYOUT_BG     0xF5F5F5
#define COLOR_SLIDER_TRACK  0xCCCCCC
#define COLOR_SLIDER_FILL   0x4A90C2
#define COLOR_SLIDER_KNOB   0x2ECC71

// ============================================================================
// Helper Functions
// ============================================================================

static void draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    while (*text) {
        const uint8_t *glyph = font_get_glyph(*text);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(x + col, y + row, color);
                    }
                }
            }
        }
        x += FONT_WIDTH;
        text++;
    }
}

static void draw_slider(int32_t x, int32_t y, int32_t width, int value, int min, int max) {
    int32_t track_y = y + (SLIDER_HEIGHT - 4) / 2;
    fb_fill_rect(x, track_y, width, 4, COLOR_SLIDER_TRACK);

    float ratio = (float)(value - min) / (float)(max - min);
    int32_t knob_x = x + (int32_t)(ratio * (width - 12));

    fb_fill_rect(x, track_y, knob_x - x, 4, COLOR_SLIDER_FILL);
    fb_fill_rect(knob_x, y, 12, SLIDER_HEIGHT, COLOR_SLIDER_KNOB);
    fb_draw_rect(knob_x, y, 12, SLIDER_HEIGHT, 0x27AE60);
}

static void draw_section_tab(int32_t x, int32_t y, const char *text, bool active) {
    uint32_t bg = active ? COLOR_TAB_ACTIVE : COLOR_TAB_INACTIVE;
    uint32_t fg = active ? THEME_TEXTBOX_BG : THEME_LABEL_TEXT;

    fb_fill_rect(x, y, SECTION_TAB_WIDTH, SECTION_TAB_HEIGHT, bg);

    int len = strlen(text);
    int32_t tx = x + (SECTION_TAB_WIDTH - len * FONT_WIDTH) / 2;
    int32_t ty = y + (SECTION_TAB_HEIGHT - FONT_HEIGHT) / 2;
    draw_text(tx, ty, text, fg);
}

static void format_shortcut(keyboard_shortcut_t *sc, char *buf, int size) {
    buf[0] = '\0';
    int pos = 0;
    if (sc->modifiers & MOD_CTRL) pos += snprintf(buf + pos, size - pos, "Ctrl+");
    if (sc->modifiers & MOD_ALT) pos += snprintf(buf + pos, size - pos, "Alt+");
    if (sc->modifiers & MOD_SHIFT) pos += snprintf(buf + pos, size - pos, "Shift+");

    if (sc->key >= 'A' && sc->key <= 'Z') {
        snprintf(buf + pos, size - pos, "%c", sc->key);
    } else {
        snprintf(buf + pos, size - pos, "0x%02X", sc->key);
    }
}

// ============================================================================
// Panel Lifecycle
// ============================================================================

static void keyboard_panel_init(settings_panel_t *panel) {
    kprintf("[Keyboard Panel] Initializing...\n");

    keyboard_panel_data_t *data = (keyboard_panel_data_t *)kzalloc(sizeof(keyboard_panel_data_t));
    if (!data) return;

    data->current_layout = LAYOUT_US;
    data->repeat_delay = 500;
    data->repeat_rate = 15;
    data->num_lock = true;
    data->caps_lock = false;
    data->show_indicator = true;
    data->sub_section = 0;

    // Default shortcuts
    strcpy(data->shortcuts[0].name, "Copy");
    data->shortcuts[0].modifiers = MOD_CTRL;
    data->shortcuts[0].key = 'C';

    strcpy(data->shortcuts[1].name, "Paste");
    data->shortcuts[1].modifiers = MOD_CTRL;
    data->shortcuts[1].key = 'V';

    strcpy(data->shortcuts[2].name, "Cut");
    data->shortcuts[2].modifiers = MOD_CTRL;
    data->shortcuts[2].key = 'X';

    strcpy(data->shortcuts[3].name, "Undo");
    data->shortcuts[3].modifiers = MOD_CTRL;
    data->shortcuts[3].key = 'Z';

    strcpy(data->shortcuts[4].name, "Save");
    data->shortcuts[4].modifiers = MOD_CTRL;
    data->shortcuts[4].key = 'S';

    strcpy(data->shortcuts[5].name, "Select All");
    data->shortcuts[5].modifiers = MOD_CTRL;
    data->shortcuts[5].key = 'A';

    data->shortcut_count = 6;

    panel->user_data = data;
    kprintf("[Keyboard Panel] Initialized\n");
}

static void keyboard_panel_draw(settings_panel_t *panel, int32_t x, int32_t y,
                                int32_t width, int32_t height) {
    keyboard_panel_data_t *data = (keyboard_panel_data_t *)panel->user_data;
    if (!data) return;

    int32_t cx = x;
    int32_t cy = y;

    // Sub-section tabs
    draw_section_tab(cx, cy, "Layout", data->sub_section == 0);
    draw_section_tab(cx + SECTION_TAB_WIDTH + 2, cy, "Typing", data->sub_section == 1);
    draw_section_tab(cx + 2 * (SECTION_TAB_WIDTH + 2), cy, "Shortcuts", data->sub_section == 2);

    cy += SECTION_TAB_HEIGHT + 16;

    // Section content
    if (data->sub_section == 0) {
        // Layout section
        draw_text(cx, cy, "Keyboard Layout", 0x4A90C2);
        cy += CONTENT_LINE_HEIGHT + 8;

        draw_text(cx, cy, "Select layout:", THEME_LABEL_TEXT);
        cy += CONTENT_LINE_HEIGHT;

        for (int i = 0; i < NUM_LAYOUTS; i++) {
            int col = i % 2;
            int row = i / 2;
            int32_t item_x = cx + col * (LAYOUT_ITEM_WIDTH + 10);
            int32_t item_y = cy + row * (LAYOUT_ITEM_HEIGHT + 4);

            bool selected = (data->current_layout == g_layouts[i].id);
            uint32_t bg = selected ? COLOR_LAYOUT_SEL : COLOR_LAYOUT_BG;
            uint32_t fg = selected ? THEME_TEXTBOX_BG : THEME_LABEL_TEXT;

            fb_fill_rect(item_x, item_y, LAYOUT_ITEM_WIDTH, LAYOUT_ITEM_HEIGHT, bg);
            draw_text(item_x + 8, item_y + 6, g_layouts[i].name, fg);
        }

        cy += ((NUM_LAYOUTS + 1) / 2) * (LAYOUT_ITEM_HEIGHT + 4) + 20;

        sw_draw_checkbox(cx, cy, data->show_indicator, "Show layout indicator in taskbar");

    } else if (data->sub_section == 1) {
        // Typing section
        char buf[32];

        draw_text(cx, cy, "Typing Settings", 0x4A90C2);
        cy += CONTENT_LINE_HEIGHT + 12;

        // Repeat delay
        draw_text(cx, cy, "Key Repeat Delay:", THEME_LABEL_TEXT);
        snprintf(buf, sizeof(buf), "%d ms", data->repeat_delay);
        draw_text(cx + 180, cy, buf, THEME_MENU_TEXT_DISABLED);
        cy += CONTENT_LINE_HEIGHT;

        draw_slider(cx, cy, SLIDER_WIDTH, data->repeat_delay, 250, 1000);
        draw_text(cx, cy + SLIDER_HEIGHT + 2, "Short", 0x888888);
        draw_text(cx + SLIDER_WIDTH - 30, cy + SLIDER_HEIGHT + 2, "Long", 0x888888);
        cy += SLIDER_HEIGHT + CONTENT_LINE_HEIGHT + 12;

        // Repeat rate
        draw_text(cx, cy, "Key Repeat Rate:", THEME_LABEL_TEXT);
        snprintf(buf, sizeof(buf), "%d/sec", data->repeat_rate);
        draw_text(cx + 180, cy, buf, THEME_MENU_TEXT_DISABLED);
        cy += CONTENT_LINE_HEIGHT;

        draw_slider(cx, cy, SLIDER_WIDTH, data->repeat_rate, 2, 30);
        draw_text(cx, cy + SLIDER_HEIGHT + 2, "Slow", 0x888888);
        draw_text(cx + SLIDER_WIDTH - 30, cy + SLIDER_HEIGHT + 2, "Fast", 0x888888);
        cy += SLIDER_HEIGHT + CONTENT_LINE_HEIGHT + 16;

        // Test area
        draw_text(cx, cy, "Test your settings:", THEME_LABEL_TEXT);
        cy += CONTENT_LINE_HEIGHT;

        fb_fill_rect(cx, cy, 280, 36, THEME_TEXTBOX_BG);
        fb_draw_rect(cx, cy, 280, 36, 0xAAAAAA);

        if (data->test_buffer[0]) {
            draw_text(cx + 8, cy + 10, data->test_buffer, THEME_LABEL_TEXT);
        } else {
            draw_text(cx + 8, cy + 10, "Click here and type...", 0xAAAAAA);
        }
        cy += 48;

        // Lock keys
        draw_text(cx, cy, "Lock Keys:", THEME_LABEL_TEXT);
        cy += CONTENT_LINE_HEIGHT;

        sw_draw_checkbox(cx, cy, data->num_lock, "Num Lock");
        sw_draw_checkbox(cx + 120, cy, data->caps_lock, "Caps Lock");

    } else {
        // Shortcuts section
        draw_text(cx, cy, "Keyboard Shortcuts", 0x4A90C2);
        cy += CONTENT_LINE_HEIGHT + 8;

        // Column headers
        draw_text(cx, cy, "Action", THEME_MENU_TEXT_DISABLED);
        draw_text(cx + 130, cy, "Shortcut", THEME_MENU_TEXT_DISABLED);
        cy += CONTENT_LINE_HEIGHT;

        fb_fill_rect(cx, cy, 280, 1, 0xCCCCCC);
        cy += 4;

        // Shortcut list
        fb_fill_rect(cx, cy, 280, data->shortcut_count * SHORTCUT_ITEM_HEIGHT + 4, THEME_TEXTBOX_BG);
        fb_draw_rect(cx, cy, 280, data->shortcut_count * SHORTCUT_ITEM_HEIGHT + 4, 0xCCCCCC);

        int32_t list_y = cy + 2;
        for (int i = 0; i < data->shortcut_count; i++) {
            keyboard_shortcut_t *sc = &data->shortcuts[i];
            bool selected = (i == data->selected_shortcut);

            if (selected) {
                fb_fill_rect(cx + 2, list_y, 276, SHORTCUT_ITEM_HEIGHT - 2, COLOR_LAYOUT_SEL);
            }

            uint32_t fg = selected ? THEME_TEXTBOX_BG : THEME_LABEL_TEXT;
            uint32_t fg2 = selected ? 0xCCCCCC : 0x4A90C2;

            draw_text(cx + 8, list_y + 3, sc->name, fg);

            char shortcut_str[32];
            format_shortcut(sc, shortcut_str, sizeof(shortcut_str));
            draw_text(cx + 130, list_y + 3, shortcut_str, fg2);

            list_y += SHORTCUT_ITEM_HEIGHT;
        }

        cy += data->shortcut_count * SHORTCUT_ITEM_HEIGHT + 16;

        // Buttons
        fb_fill_rect(cx, cy, 70, 24, 0x4A90C2);
        draw_text(cx + 18, cy + 4, "Edit", THEME_TEXTBOX_BG);

        fb_fill_rect(cx + 80, cy, 80, 24, 0x7F8C8D);
        draw_text(cx + 92, cy + 4, "Reset", THEME_TEXTBOX_BG);
    }
}

static void keyboard_panel_event(settings_panel_t *panel, gui_event_t *event) {
    keyboard_panel_data_t *data = (keyboard_panel_data_t *)panel->user_data;
    if (!data) return;

    int32_t mx = event->mouse_x;
    int32_t my = event->mouse_y;
    int32_t cx = panel->content_x;
    int32_t cy = panel->content_y;

    if (event->type == EVENT_MOUSE_UP) {
        // Check tab clicks
        if (my >= cy && my < cy + SECTION_TAB_HEIGHT) {
            for (int i = 0; i < 3; i++) {
                int32_t tab_x = cx + i * (SECTION_TAB_WIDTH + 2);
                if (mx >= tab_x && mx < tab_x + SECTION_TAB_WIDTH) {
                    data->sub_section = i;
                    settings_panel_mark_dirty(panel);
                    return;
                }
            }
        }

        int32_t content_y = cy + SECTION_TAB_HEIGHT + 16;

        if (data->sub_section == 0) {
            // Layout selection
            int32_t list_y = content_y + 2 * CONTENT_LINE_HEIGHT + 8;
            for (int i = 0; i < NUM_LAYOUTS; i++) {
                int col = i % 2;
                int row = i / 2;
                int32_t item_x = cx + col * (LAYOUT_ITEM_WIDTH + 10);
                int32_t item_y = list_y + row * (LAYOUT_ITEM_HEIGHT + 4);

                if (mx >= item_x && mx < item_x + LAYOUT_ITEM_WIDTH &&
                    my >= item_y && my < item_y + LAYOUT_ITEM_HEIGHT) {
                    data->current_layout = g_layouts[i].id;
                    kprintf("[Keyboard Panel] Selected layout: %s\n", g_layouts[i].name);
                    settings_panel_mark_dirty(panel);
                    return;
                }
            }
        } else if (data->sub_section == 1) {
            // Slider clicks
            int32_t delay_y = content_y + 2 * CONTENT_LINE_HEIGHT + 12;
            if (my >= delay_y && my < delay_y + SLIDER_HEIGHT &&
                mx >= cx && mx < cx + SLIDER_WIDTH) {
                float ratio = (float)(mx - cx) / SLIDER_WIDTH;
                data->repeat_delay = 250 + (int)(ratio * 750);
                if (data->repeat_delay > 1000) data->repeat_delay = 1000;
                if (data->repeat_delay < 250) data->repeat_delay = 250;
                settings_panel_mark_dirty(panel);
                return;
            }

            int32_t rate_y = delay_y + SLIDER_HEIGHT + 3 * CONTENT_LINE_HEIGHT + 12;
            if (my >= rate_y && my < rate_y + SLIDER_HEIGHT &&
                mx >= cx && mx < cx + SLIDER_WIDTH) {
                float ratio = (float)(mx - cx) / SLIDER_WIDTH;
                data->repeat_rate = 2 + (int)(ratio * 28);
                if (data->repeat_rate > 30) data->repeat_rate = 30;
                if (data->repeat_rate < 2) data->repeat_rate = 2;
                settings_panel_mark_dirty(panel);
                return;
            }
        } else if (data->sub_section == 2) {
            // Shortcut list clicks
            int32_t list_y = content_y + 2 * CONTENT_LINE_HEIGHT + 8 + 4 + 2;
            for (int i = 0; i < data->shortcut_count; i++) {
                if (my >= list_y && my < list_y + SHORTCUT_ITEM_HEIGHT &&
                    mx >= cx && mx < cx + 280) {
                    data->selected_shortcut = i;
                    kprintf("[Keyboard Panel] Selected shortcut: %s\n", data->shortcuts[i].name);
                    settings_panel_mark_dirty(panel);
                    return;
                }
                list_y += SHORTCUT_ITEM_HEIGHT;
            }
        }
    }

    // Handle keyboard input for test area
    if (event->type == EVENT_KEY_DOWN && data->sub_section == 1) {
        char c = event->key_char;
        int len = strlen(data->test_buffer);

        if (c == '\b') {
            if (len > 0) data->test_buffer[len - 1] = '\0';
        } else if (c >= 32 && c < 127 && len < 120) {
            data->test_buffer[len] = c;
            data->test_buffer[len + 1] = '\0';
        }
        settings_panel_mark_dirty(panel);
    }
}

static void keyboard_panel_apply(settings_panel_t *panel) {
    kprintf("[Keyboard Panel] Applying changes...\n");
}

static void keyboard_panel_cleanup(settings_panel_t *panel) {
    if (panel->user_data) {
        kfree(panel->user_data);
        panel->user_data = NULL;
    }
    kprintf("[Keyboard Panel] Cleaned up\n");
}

// ============================================================================
// Panel Registration
// ============================================================================

static settings_panel_def_t keyboard_panel_def = {
    .name = "Keyboard",
    .icon = "keyboard",
    .category = SETTINGS_CAT_SYSTEM,
    .priority = 30,
    .init = keyboard_panel_init,
    .draw = keyboard_panel_draw,
    .handle_event = keyboard_panel_event,
    .apply = keyboard_panel_apply,
    .cleanup = keyboard_panel_cleanup
};

void keyboard_panel_register(void) {
    settings_register_panel(&keyboard_panel_def);
}
