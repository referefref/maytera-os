// theme.c - Enhanced Theme Engine implementation for MayteraOS GUI
#include "theme.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../video/framebuffer.h"
#include "../video/font.h"

// External filesystem reference
extern fat_fs_t g_fat_fs;

// =============================================================================
// Module State
// =============================================================================

static theme_t g_themes[THEME_MAX_THEMES];
static int g_theme_count = 0;
static int g_active_theme = 0;
static bool g_initialized = false;

// Static name array for theme_get_names()
static const char *g_theme_names[THEME_MAX_THEMES];

// =============================================================================
// Forward Declarations
// =============================================================================

int theme_parse_ini(const char *data, size_t size, theme_t *theme);

// =============================================================================
// Built-in Themes
// =============================================================================

// Initialize a built-in theme with default values
static void init_builtin_theme_default(theme_t *t) {
    memset(t, 0, sizeof(theme_t));

    strcpy(t->name, "Default");
    strcpy(t->author, "MayteraOS Team");
    strcpy(t->version, "1.0");
    t->is_builtin = true;

    // Modern grey theme colors
    t->colors[THEME_COLOR_BACKGROUND] = 0xF5F5F5;
    t->colors[THEME_COLOR_FOREGROUND] = 0x000000;
    t->colors[THEME_COLOR_ACCENT] = 0x3399FF;
    t->colors[THEME_COLOR_SELECTION] = 0x3399FF;
    t->colors[THEME_COLOR_SELECTION_TEXT] = 0xFFFFFF;

    t->colors[THEME_COLOR_TITLEBAR_ACTIVE] = 0x3A3A3A;
    t->colors[THEME_COLOR_TITLEBAR_INACTIVE] = 0x2D2D2D;
    t->colors[THEME_COLOR_TITLEBAR_TEXT] = 0xE0E0E0;

    t->colors[THEME_COLOR_WINDOW_BG] = 0xF5F5F5;
    t->colors[THEME_COLOR_WINDOW_BORDER] = 0x505050;

    t->colors[THEME_COLOR_CLOSE_BUTTON] = 0x606060;
    t->colors[THEME_COLOR_CLOSE_BUTTON_HOVER] = 0xE04040;
    t->colors[THEME_COLOR_MINIMIZE_BUTTON] = 0x707070;
    t->colors[THEME_COLOR_MAXIMIZE_BUTTON] = 0x707070;

    t->colors[THEME_COLOR_BUTTON_FACE] = 0xD0D0D0;
    t->colors[THEME_COLOR_BUTTON_LIGHT] = 0xE0E0E0;
    t->colors[THEME_COLOR_BUTTON_SHADOW] = 0xA0A0A0;
    t->colors[THEME_COLOR_BUTTON_DARK] = 0x606060;
    t->colors[THEME_COLOR_BUTTON_TEXT] = 0x000000;
    t->colors[THEME_COLOR_BUTTON_DISABLED] = 0x808080;

    t->colors[THEME_COLOR_LABEL_TEXT] = 0x000000;
    t->colors[THEME_COLOR_TEXTBOX_BG] = 0xFFFFFF;
    t->colors[THEME_COLOR_TEXTBOX_BORDER] = 0x606060;
    t->colors[THEME_COLOR_TEXTBOX_TEXT] = 0x000000;
    t->colors[THEME_COLOR_TEXTBOX_CURSOR] = 0x000000;

    t->colors[THEME_COLOR_CHECKBOX_BG] = 0xFFFFFF;
    t->colors[THEME_COLOR_CHECKBOX_BORDER] = 0x606060;
    t->colors[THEME_COLOR_CHECKBOX_CHECK] = 0x000000;

    t->colors[THEME_COLOR_DESKTOP_BG] = 0x2E5A88;

    t->colors[THEME_COLOR_TASKBAR_BG] = 0x2D2D2D;
    t->colors[THEME_COLOR_TASKBAR_HOVER] = 0x3D3D3D;
    t->colors[THEME_COLOR_TASKBAR_ACTIVE] = 0x4A4A4A;
    t->colors[THEME_COLOR_START_BUTTON] = 0x3A3A3A;
    t->colors[THEME_COLOR_GAUGE_BG] = 0x1A1A1A;
    t->colors[THEME_COLOR_GAUGE_FG] = 0x6A6A6A;

    t->colors[THEME_COLOR_MENU_BG] = 0xFFFFFF;
    t->colors[THEME_COLOR_MENU_BORDER] = 0x909090;
    t->colors[THEME_COLOR_MENU_ITEM_HOVER] = 0x3399FF;
    t->colors[THEME_COLOR_MENU_TEXT] = 0x000000;
    t->colors[THEME_COLOR_MENU_TEXT_DISABLED] = 0x808080;
    t->colors[THEME_COLOR_MENU_SEPARATOR] = 0xC0C0C0;

    t->colors[THEME_COLOR_SCROLLBAR_BG] = 0xE0E0E0;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB] = 0x808080;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB_HOVER] = 0x606060;

    t->colors[THEME_COLOR_TAB_BG] = 0xE0E0E0;
    t->colors[THEME_COLOR_TAB_ACTIVE] = 0xFFFFFF;
    t->colors[THEME_COLOR_TAB_BORDER] = 0x808080;

    t->colors[THEME_COLOR_TOOLTIP_BG] = 0xFFFFE0;
    t->colors[THEME_COLOR_TOOLTIP_BORDER] = 0x000000;
    t->colors[THEME_COLOR_TOOLTIP_TEXT] = 0x000000;

    t->colors[THEME_COLOR_PROGRESS_BG] = 0xE0E0E0;
    t->colors[THEME_COLOR_PROGRESS_FG] = 0x3399FF;

    // Default fonts
    for (int i = 0; i < THEME_FONT_COUNT; i++) {
        strcpy(t->fonts[i].name, "fixed");
        t->fonts[i].size = 12;
        t->fonts[i].bold = false;
        t->fonts[i].italic = false;
    }
    t->fonts[THEME_FONT_TITLE].bold = true;

    // Default metrics
    t->metrics.border_width = 1;
    t->metrics.titlebar_height = 20;
    t->metrics.button_width = 16;
    t->metrics.button_height = 14;
    t->metrics.scrollbar_width = 16;
    t->metrics.menu_item_height = 20;
    t->metrics.icon_size = 32;
    t->metrics.corner_radius = 0;
    t->metrics.padding = 4;
    t->metrics.spacing = 4;
    t->metrics.tab_height = 24;

    // Default decorations
    t->decorations.style = THEME_STYLE_FLAT;
    t->decorations.corner_radius = 0;
    t->decorations.shadow_enabled = false;
}

static void init_builtin_theme_dark(theme_t *t) {
    memset(t, 0, sizeof(theme_t));

    strcpy(t->name, "Dark Mode");
    strcpy(t->author, "MayteraOS Team");
    strcpy(t->version, "1.0");
    t->is_builtin = true;

    // Dark theme colors
    t->colors[THEME_COLOR_BACKGROUND] = 0x2D2D2D;
    t->colors[THEME_COLOR_FOREGROUND] = 0xE0E0E0;
    t->colors[THEME_COLOR_ACCENT] = 0x405080;
    t->colors[THEME_COLOR_SELECTION] = 0x405080;
    t->colors[THEME_COLOR_SELECTION_TEXT] = 0xFFFFFF;

    t->colors[THEME_COLOR_TITLEBAR_ACTIVE] = 0x202020;
    t->colors[THEME_COLOR_TITLEBAR_INACTIVE] = 0x181818;
    t->colors[THEME_COLOR_TITLEBAR_TEXT] = 0xE0E0E0;

    t->colors[THEME_COLOR_WINDOW_BG] = 0x2D2D2D;
    t->colors[THEME_COLOR_WINDOW_BORDER] = 0x404040;

    t->colors[THEME_COLOR_CLOSE_BUTTON] = 0x404040;
    t->colors[THEME_COLOR_CLOSE_BUTTON_HOVER] = 0xC03030;
    t->colors[THEME_COLOR_MINIMIZE_BUTTON] = 0x505050;
    t->colors[THEME_COLOR_MAXIMIZE_BUTTON] = 0x505050;

    t->colors[THEME_COLOR_BUTTON_FACE] = 0x404040;
    t->colors[THEME_COLOR_BUTTON_LIGHT] = 0x505050;
    t->colors[THEME_COLOR_BUTTON_SHADOW] = 0x303030;
    t->colors[THEME_COLOR_BUTTON_DARK] = 0x606060;
    t->colors[THEME_COLOR_BUTTON_TEXT] = 0xE0E0E0;
    t->colors[THEME_COLOR_BUTTON_DISABLED] = 0x606060;

    t->colors[THEME_COLOR_LABEL_TEXT] = 0xE0E0E0;
    t->colors[THEME_COLOR_TEXTBOX_BG] = 0x383838;
    t->colors[THEME_COLOR_TEXTBOX_BORDER] = 0x606060;
    t->colors[THEME_COLOR_TEXTBOX_TEXT] = 0xE0E0E0;
    t->colors[THEME_COLOR_TEXTBOX_CURSOR] = 0xFFFFFF;

    t->colors[THEME_COLOR_CHECKBOX_BG] = 0x383838;
    t->colors[THEME_COLOR_CHECKBOX_BORDER] = 0x606060;
    t->colors[THEME_COLOR_CHECKBOX_CHECK] = 0x80C0FF;

    t->colors[THEME_COLOR_DESKTOP_BG] = 0x101820;

    t->colors[THEME_COLOR_TASKBAR_BG] = 0x181818;
    t->colors[THEME_COLOR_TASKBAR_HOVER] = 0x282828;
    t->colors[THEME_COLOR_TASKBAR_ACTIVE] = 0x383838;
    t->colors[THEME_COLOR_START_BUTTON] = 0x282828;
    t->colors[THEME_COLOR_GAUGE_BG] = 0x101010;
    t->colors[THEME_COLOR_GAUGE_FG] = 0x606060;

    t->colors[THEME_COLOR_MENU_BG] = 0x2D2D2D;
    t->colors[THEME_COLOR_MENU_BORDER] = 0x505050;
    t->colors[THEME_COLOR_MENU_ITEM_HOVER] = 0x405080;
    t->colors[THEME_COLOR_MENU_TEXT] = 0xE0E0E0;
    t->colors[THEME_COLOR_MENU_TEXT_DISABLED] = 0x606060;
    t->colors[THEME_COLOR_MENU_SEPARATOR] = 0x404040;

    t->colors[THEME_COLOR_SCROLLBAR_BG] = 0x303030;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB] = 0x505050;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB_HOVER] = 0x606060;

    t->colors[THEME_COLOR_TAB_BG] = 0x2D2D2D;
    t->colors[THEME_COLOR_TAB_ACTIVE] = 0x404040;
    t->colors[THEME_COLOR_TAB_BORDER] = 0x505050;

    t->colors[THEME_COLOR_TOOLTIP_BG] = 0x404040;
    t->colors[THEME_COLOR_TOOLTIP_BORDER] = 0x606060;
    t->colors[THEME_COLOR_TOOLTIP_TEXT] = 0xE0E0E0;

    t->colors[THEME_COLOR_PROGRESS_BG] = 0x303030;
    t->colors[THEME_COLOR_PROGRESS_FG] = 0x405080;

    // Default fonts (same as default theme)
    for (int i = 0; i < THEME_FONT_COUNT; i++) {
        strcpy(t->fonts[i].name, "fixed");
        t->fonts[i].size = 12;
        t->fonts[i].bold = false;
        t->fonts[i].italic = false;
    }
    t->fonts[THEME_FONT_TITLE].bold = true;

    // Default metrics
    t->metrics.border_width = 1;
    t->metrics.titlebar_height = 20;
    t->metrics.button_width = 16;
    t->metrics.button_height = 14;
    t->metrics.scrollbar_width = 16;
    t->metrics.menu_item_height = 20;
    t->metrics.icon_size = 32;
    t->metrics.corner_radius = 0;
    t->metrics.padding = 4;
    t->metrics.spacing = 4;
    t->metrics.tab_height = 24;

    // Default decorations
    t->decorations.style = THEME_STYLE_FLAT;
    t->decorations.corner_radius = 0;
    t->decorations.shadow_enabled = false;
}

static void init_builtin_theme_classic(theme_t *t) {
    memset(t, 0, sizeof(theme_t));

    strcpy(t->name, "Classic (Win95)");
    strcpy(t->author, "MayteraOS Team");
    strcpy(t->version, "1.0");
    t->is_builtin = true;

    // Classic Windows 95 colors
    t->colors[THEME_COLOR_BACKGROUND] = 0xC0C0C0;
    t->colors[THEME_COLOR_FOREGROUND] = 0x000000;
    t->colors[THEME_COLOR_ACCENT] = 0x000080;
    t->colors[THEME_COLOR_SELECTION] = 0x000080;
    t->colors[THEME_COLOR_SELECTION_TEXT] = 0xFFFFFF;

    t->colors[THEME_COLOR_TITLEBAR_ACTIVE] = 0x000080;
    t->colors[THEME_COLOR_TITLEBAR_INACTIVE] = 0x808080;
    t->colors[THEME_COLOR_TITLEBAR_TEXT] = 0xFFFFFF;

    t->colors[THEME_COLOR_WINDOW_BG] = 0xC0C0C0;
    t->colors[THEME_COLOR_WINDOW_BORDER] = 0x000000;

    t->colors[THEME_COLOR_CLOSE_BUTTON] = 0xC0C0C0;
    t->colors[THEME_COLOR_CLOSE_BUTTON_HOVER] = 0xE0E0E0;
    t->colors[THEME_COLOR_MINIMIZE_BUTTON] = 0xC0C0C0;
    t->colors[THEME_COLOR_MAXIMIZE_BUTTON] = 0xC0C0C0;

    t->colors[THEME_COLOR_BUTTON_FACE] = 0xC0C0C0;
    t->colors[THEME_COLOR_BUTTON_LIGHT] = 0xFFFFFF;
    t->colors[THEME_COLOR_BUTTON_SHADOW] = 0x808080;
    t->colors[THEME_COLOR_BUTTON_DARK] = 0x404040;
    t->colors[THEME_COLOR_BUTTON_TEXT] = 0x000000;
    t->colors[THEME_COLOR_BUTTON_DISABLED] = 0x808080;

    t->colors[THEME_COLOR_LABEL_TEXT] = 0x000000;
    t->colors[THEME_COLOR_TEXTBOX_BG] = 0xFFFFFF;
    t->colors[THEME_COLOR_TEXTBOX_BORDER] = 0x000000;
    t->colors[THEME_COLOR_TEXTBOX_TEXT] = 0x000000;
    t->colors[THEME_COLOR_TEXTBOX_CURSOR] = 0x000000;

    t->colors[THEME_COLOR_CHECKBOX_BG] = 0xFFFFFF;
    t->colors[THEME_COLOR_CHECKBOX_BORDER] = 0x000000;
    t->colors[THEME_COLOR_CHECKBOX_CHECK] = 0x000000;

    t->colors[THEME_COLOR_DESKTOP_BG] = 0x008080;

    t->colors[THEME_COLOR_TASKBAR_BG] = 0xC0C0C0;
    t->colors[THEME_COLOR_TASKBAR_HOVER] = 0xD0D0D0;
    t->colors[THEME_COLOR_TASKBAR_ACTIVE] = 0xA0A0A0;
    t->colors[THEME_COLOR_START_BUTTON] = 0xC0C0C0;
    t->colors[THEME_COLOR_GAUGE_BG] = 0x808080;
    t->colors[THEME_COLOR_GAUGE_FG] = 0x000080;

    t->colors[THEME_COLOR_MENU_BG] = 0xC0C0C0;
    t->colors[THEME_COLOR_MENU_BORDER] = 0x000000;
    t->colors[THEME_COLOR_MENU_ITEM_HOVER] = 0x000080;
    t->colors[THEME_COLOR_MENU_TEXT] = 0x000000;
    t->colors[THEME_COLOR_MENU_TEXT_DISABLED] = 0x808080;
    t->colors[THEME_COLOR_MENU_SEPARATOR] = 0x808080;

    t->colors[THEME_COLOR_SCROLLBAR_BG] = 0xC0C0C0;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB] = 0xC0C0C0;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB_HOVER] = 0xD0D0D0;

    t->colors[THEME_COLOR_TAB_BG] = 0xC0C0C0;
    t->colors[THEME_COLOR_TAB_ACTIVE] = 0xFFFFFF;
    t->colors[THEME_COLOR_TAB_BORDER] = 0x808080;

    t->colors[THEME_COLOR_TOOLTIP_BG] = 0xFFFFE0;
    t->colors[THEME_COLOR_TOOLTIP_BORDER] = 0x000000;
    t->colors[THEME_COLOR_TOOLTIP_TEXT] = 0x000000;

    t->colors[THEME_COLOR_PROGRESS_BG] = 0xC0C0C0;
    t->colors[THEME_COLOR_PROGRESS_FG] = 0x000080;

    // Default fonts
    for (int i = 0; i < THEME_FONT_COUNT; i++) {
        strcpy(t->fonts[i].name, "fixed");
        t->fonts[i].size = 12;
        t->fonts[i].bold = false;
        t->fonts[i].italic = false;
    }
    t->fonts[THEME_FONT_TITLE].bold = true;

    // Classic metrics
    t->metrics.border_width = 2;
    t->metrics.titlebar_height = 18;
    t->metrics.button_width = 16;
    t->metrics.button_height = 14;
    t->metrics.scrollbar_width = 16;
    t->metrics.menu_item_height = 18;
    t->metrics.icon_size = 32;
    t->metrics.corner_radius = 0;
    t->metrics.padding = 2;
    t->metrics.spacing = 2;
    t->metrics.tab_height = 20;

    // Classic decorations (Motif/Win95 3D style)
    t->decorations.style = THEME_STYLE_WIN95;
    t->decorations.corner_radius = 0;
    t->decorations.shadow_enabled = false;
}

static void init_builtin_theme_ocean(theme_t *t) {
    memset(t, 0, sizeof(theme_t));

    strcpy(t->name, "Ocean");
    strcpy(t->author, "MayteraOS Team");
    strcpy(t->version, "1.0");
    t->is_builtin = true;

    // Ocean blue theme
    t->colors[THEME_COLOR_BACKGROUND] = 0xE8F4FC;
    t->colors[THEME_COLOR_FOREGROUND] = 0x203040;
    t->colors[THEME_COLOR_ACCENT] = 0x205080;
    t->colors[THEME_COLOR_SELECTION] = 0x205080;
    t->colors[THEME_COLOR_SELECTION_TEXT] = 0xFFFFFF;

    t->colors[THEME_COLOR_TITLEBAR_ACTIVE] = 0x205080;
    t->colors[THEME_COLOR_TITLEBAR_INACTIVE] = 0x304050;
    t->colors[THEME_COLOR_TITLEBAR_TEXT] = 0xFFFFFF;

    t->colors[THEME_COLOR_WINDOW_BG] = 0xE8F4FC;
    t->colors[THEME_COLOR_WINDOW_BORDER] = 0x306090;

    t->colors[THEME_COLOR_CLOSE_BUTTON] = 0x408090;
    t->colors[THEME_COLOR_CLOSE_BUTTON_HOVER] = 0xC05050;
    t->colors[THEME_COLOR_MINIMIZE_BUTTON] = 0x508090;
    t->colors[THEME_COLOR_MAXIMIZE_BUTTON] = 0x508090;

    t->colors[THEME_COLOR_BUTTON_FACE] = 0x80B0D0;
    t->colors[THEME_COLOR_BUTTON_LIGHT] = 0x90C0E0;
    t->colors[THEME_COLOR_BUTTON_SHADOW] = 0x609090;
    t->colors[THEME_COLOR_BUTTON_DARK] = 0x306080;
    t->colors[THEME_COLOR_BUTTON_TEXT] = 0xFFFFFF;
    t->colors[THEME_COLOR_BUTTON_DISABLED] = 0x808080;

    t->colors[THEME_COLOR_LABEL_TEXT] = 0x203040;
    t->colors[THEME_COLOR_TEXTBOX_BG] = 0xFFFFFF;
    t->colors[THEME_COLOR_TEXTBOX_BORDER] = 0x608090;
    t->colors[THEME_COLOR_TEXTBOX_TEXT] = 0x203040;
    t->colors[THEME_COLOR_TEXTBOX_CURSOR] = 0x205080;

    t->colors[THEME_COLOR_CHECKBOX_BG] = 0xFFFFFF;
    t->colors[THEME_COLOR_CHECKBOX_BORDER] = 0x608090;
    t->colors[THEME_COLOR_CHECKBOX_CHECK] = 0x205080;

    t->colors[THEME_COLOR_DESKTOP_BG] = 0x306090;

    t->colors[THEME_COLOR_TASKBAR_BG] = 0x204060;
    t->colors[THEME_COLOR_TASKBAR_HOVER] = 0x305070;
    t->colors[THEME_COLOR_TASKBAR_ACTIVE] = 0x406080;
    t->colors[THEME_COLOR_START_BUTTON] = 0x305080;
    t->colors[THEME_COLOR_GAUGE_BG] = 0x102030;
    t->colors[THEME_COLOR_GAUGE_FG] = 0x60A0C0;

    t->colors[THEME_COLOR_MENU_BG] = 0xF0F8FF;
    t->colors[THEME_COLOR_MENU_BORDER] = 0x608090;
    t->colors[THEME_COLOR_MENU_ITEM_HOVER] = 0x205080;
    t->colors[THEME_COLOR_MENU_TEXT] = 0x203040;
    t->colors[THEME_COLOR_MENU_TEXT_DISABLED] = 0x808090;
    t->colors[THEME_COLOR_MENU_SEPARATOR] = 0xC0D0E0;

    t->colors[THEME_COLOR_SCROLLBAR_BG] = 0xD0E0F0;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB] = 0x80A0C0;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB_HOVER] = 0x609090;

    t->colors[THEME_COLOR_TAB_BG] = 0xC0D8E8;
    t->colors[THEME_COLOR_TAB_ACTIVE] = 0xFFFFFF;
    t->colors[THEME_COLOR_TAB_BORDER] = 0x608090;

    t->colors[THEME_COLOR_TOOLTIP_BG] = 0xF0F8FF;
    t->colors[THEME_COLOR_TOOLTIP_BORDER] = 0x608090;
    t->colors[THEME_COLOR_TOOLTIP_TEXT] = 0x203040;

    t->colors[THEME_COLOR_PROGRESS_BG] = 0xD0E0F0;
    t->colors[THEME_COLOR_PROGRESS_FG] = 0x205080;

    // Default fonts
    for (int i = 0; i < THEME_FONT_COUNT; i++) {
        strcpy(t->fonts[i].name, "fixed");
        t->fonts[i].size = 12;
        t->fonts[i].bold = false;
        t->fonts[i].italic = false;
    }
    t->fonts[THEME_FONT_TITLE].bold = true;

    // Default metrics
    t->metrics.border_width = 1;
    t->metrics.titlebar_height = 20;
    t->metrics.button_width = 16;
    t->metrics.button_height = 14;
    t->metrics.scrollbar_width = 16;
    t->metrics.menu_item_height = 20;
    t->metrics.icon_size = 32;
    t->metrics.corner_radius = 0;
    t->metrics.padding = 4;
    t->metrics.spacing = 4;
    t->metrics.tab_height = 24;

    // Default decorations
    t->decorations.style = THEME_STYLE_FLAT;
    t->decorations.corner_radius = 0;
    t->decorations.shadow_enabled = false;
}

// =============================================================================
// Additional Built-in Themes (Added by P14 - Theme Specialist)
// =============================================================================

static void init_builtin_theme_light(theme_t *t) {
    memset(t, 0, sizeof(theme_t));

    strcpy(t->name, "Light Mode");
    strcpy(t->author, "MayteraOS Team");
    strcpy(t->version, "1.0");
    t->is_builtin = true;

    // Light theme colors
    t->colors[THEME_COLOR_BACKGROUND] = 0xFFFFFF;
    t->colors[THEME_COLOR_FOREGROUND] = 0x000000;
    t->colors[THEME_COLOR_ACCENT] = 0x0078D7;
    t->colors[THEME_COLOR_SELECTION] = 0x0078D7;
    t->colors[THEME_COLOR_SELECTION_TEXT] = 0xFFFFFF;

    t->colors[THEME_COLOR_TITLEBAR_ACTIVE] = 0x0078D7;
    t->colors[THEME_COLOR_TITLEBAR_INACTIVE] = 0xE0E0E0;
    t->colors[THEME_COLOR_TITLEBAR_TEXT] = 0xFFFFFF;

    t->colors[THEME_COLOR_WINDOW_BG] = 0xFFFFFF;
    t->colors[THEME_COLOR_WINDOW_BORDER] = 0xCCCCCC;

    t->colors[THEME_COLOR_CLOSE_BUTTON] = 0xE0E0E0;
    t->colors[THEME_COLOR_CLOSE_BUTTON_HOVER] = 0xE04040;
    t->colors[THEME_COLOR_MINIMIZE_BUTTON] = 0xE0E0E0;
    t->colors[THEME_COLOR_MAXIMIZE_BUTTON] = 0xE0E0E0;

    t->colors[THEME_COLOR_BUTTON_FACE] = 0xE0E0E0;
    t->colors[THEME_COLOR_BUTTON_LIGHT] = 0xF0F0F0;
    t->colors[THEME_COLOR_BUTTON_SHADOW] = 0xC0C0C0;
    t->colors[THEME_COLOR_BUTTON_DARK] = 0xA0A0A0;
    t->colors[THEME_COLOR_BUTTON_TEXT] = 0x000000;
    t->colors[THEME_COLOR_BUTTON_DISABLED] = 0x909090;

    t->colors[THEME_COLOR_LABEL_TEXT] = 0x000000;
    t->colors[THEME_COLOR_TEXTBOX_BG] = 0xFFFFFF;
    t->colors[THEME_COLOR_TEXTBOX_BORDER] = 0xB0B0B0;
    t->colors[THEME_COLOR_TEXTBOX_TEXT] = 0x000000;
    t->colors[THEME_COLOR_TEXTBOX_CURSOR] = 0x000000;

    t->colors[THEME_COLOR_CHECKBOX_BG] = 0xFFFFFF;
    t->colors[THEME_COLOR_CHECKBOX_BORDER] = 0x909090;
    t->colors[THEME_COLOR_CHECKBOX_CHECK] = 0x0078D7;

    t->colors[THEME_COLOR_DESKTOP_BG] = 0xE8F0F8;

    t->colors[THEME_COLOR_TASKBAR_BG] = 0xF0F0F0;
    t->colors[THEME_COLOR_TASKBAR_HOVER] = 0xE0E0E0;
    t->colors[THEME_COLOR_TASKBAR_ACTIVE] = 0xD0D0D0;
    t->colors[THEME_COLOR_START_BUTTON] = 0xFFFFFF;
    t->colors[THEME_COLOR_GAUGE_BG] = 0xE0E0E0;
    t->colors[THEME_COLOR_GAUGE_FG] = 0x808080;

    t->colors[THEME_COLOR_MENU_BG] = 0xFFFFFF;
    t->colors[THEME_COLOR_MENU_BORDER] = 0xC0C0C0;
    t->colors[THEME_COLOR_MENU_ITEM_HOVER] = 0x0078D7;
    t->colors[THEME_COLOR_MENU_TEXT] = 0x000000;
    t->colors[THEME_COLOR_MENU_TEXT_DISABLED] = 0x909090;
    t->colors[THEME_COLOR_MENU_SEPARATOR] = 0xE0E0E0;

    t->colors[THEME_COLOR_SCROLLBAR_BG] = 0xF0F0F0;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB] = 0xC0C0C0;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB_HOVER] = 0xA0A0A0;

    t->colors[THEME_COLOR_TAB_BG] = 0xE8E8E8;
    t->colors[THEME_COLOR_TAB_ACTIVE] = 0xFFFFFF;
    t->colors[THEME_COLOR_TAB_BORDER] = 0xC0C0C0;

    t->colors[THEME_COLOR_TOOLTIP_BG] = 0xFFFFE0;
    t->colors[THEME_COLOR_TOOLTIP_BORDER] = 0x606060;
    t->colors[THEME_COLOR_TOOLTIP_TEXT] = 0x000000;

    t->colors[THEME_COLOR_PROGRESS_BG] = 0xE0E0E0;
    t->colors[THEME_COLOR_PROGRESS_FG] = 0x0078D7;

    for (int i = 0; i < THEME_FONT_COUNT; i++) {
        strcpy(t->fonts[i].name, "fixed");
        t->fonts[i].size = 12;
        t->fonts[i].bold = false;
        t->fonts[i].italic = false;
    }
    t->fonts[THEME_FONT_TITLE].bold = true;

    t->metrics.border_width = 1;
    t->metrics.titlebar_height = 24;
    t->metrics.button_width = 16;
    t->metrics.button_height = 14;
    t->metrics.scrollbar_width = 12;
    t->metrics.menu_item_height = 24;
    t->metrics.icon_size = 32;
    t->metrics.corner_radius = 4;
    t->metrics.padding = 6;
    t->metrics.spacing = 6;
    t->metrics.tab_height = 28;

    t->decorations.style = THEME_STYLE_FLAT;
    t->decorations.corner_radius = 4;
    t->decorations.shadow_enabled = true;
    t->decorations.shadow_blur = 8;
    t->decorations.shadow_color = 0x40000000;
}

static void init_builtin_theme_high_contrast(theme_t *t) {
    memset(t, 0, sizeof(theme_t));

    strcpy(t->name, "High Contrast");
    strcpy(t->author, "MayteraOS Team");
    strcpy(t->version, "1.0");
    t->is_builtin = true;

    // High contrast - black background, white/yellow text
    t->colors[THEME_COLOR_BACKGROUND] = 0x000000;
    t->colors[THEME_COLOR_FOREGROUND] = 0xFFFFFF;
    t->colors[THEME_COLOR_ACCENT] = 0x00FFFF;
    t->colors[THEME_COLOR_SELECTION] = 0x00FFFF;
    t->colors[THEME_COLOR_SELECTION_TEXT] = 0x000000;

    t->colors[THEME_COLOR_TITLEBAR_ACTIVE] = 0x000080;
    t->colors[THEME_COLOR_TITLEBAR_INACTIVE] = 0x000000;
    t->colors[THEME_COLOR_TITLEBAR_TEXT] = 0xFFFFFF;

    t->colors[THEME_COLOR_WINDOW_BG] = 0x000000;
    t->colors[THEME_COLOR_WINDOW_BORDER] = 0xFFFFFF;

    t->colors[THEME_COLOR_CLOSE_BUTTON] = 0xFF0000;
    t->colors[THEME_COLOR_CLOSE_BUTTON_HOVER] = 0xFF4040;
    t->colors[THEME_COLOR_MINIMIZE_BUTTON] = 0x00FF00;
    t->colors[THEME_COLOR_MAXIMIZE_BUTTON] = 0x00FF00;

    t->colors[THEME_COLOR_BUTTON_FACE] = 0x000080;
    t->colors[THEME_COLOR_BUTTON_LIGHT] = 0x0000C0;
    t->colors[THEME_COLOR_BUTTON_SHADOW] = 0x000040;
    t->colors[THEME_COLOR_BUTTON_DARK] = 0xFFFFFF;
    t->colors[THEME_COLOR_BUTTON_TEXT] = 0xFFFFFF;
    t->colors[THEME_COLOR_BUTTON_DISABLED] = 0x808080;

    t->colors[THEME_COLOR_LABEL_TEXT] = 0xFFFFFF;
    t->colors[THEME_COLOR_TEXTBOX_BG] = 0x000000;
    t->colors[THEME_COLOR_TEXTBOX_BORDER] = 0xFFFFFF;
    t->colors[THEME_COLOR_TEXTBOX_TEXT] = 0xFFFF00;
    t->colors[THEME_COLOR_TEXTBOX_CURSOR] = 0xFFFFFF;

    t->colors[THEME_COLOR_CHECKBOX_BG] = 0x000000;
    t->colors[THEME_COLOR_CHECKBOX_BORDER] = 0xFFFFFF;
    t->colors[THEME_COLOR_CHECKBOX_CHECK] = 0x00FF00;

    t->colors[THEME_COLOR_DESKTOP_BG] = 0x000000;

    t->colors[THEME_COLOR_TASKBAR_BG] = 0x000000;
    t->colors[THEME_COLOR_TASKBAR_HOVER] = 0x000080;
    t->colors[THEME_COLOR_TASKBAR_ACTIVE] = 0x0000C0;
    t->colors[THEME_COLOR_START_BUTTON] = 0x008000;
    t->colors[THEME_COLOR_GAUGE_BG] = 0x404040;
    t->colors[THEME_COLOR_GAUGE_FG] = 0x00FF00;

    t->colors[THEME_COLOR_MENU_BG] = 0x000000;
    t->colors[THEME_COLOR_MENU_BORDER] = 0xFFFFFF;
    t->colors[THEME_COLOR_MENU_ITEM_HOVER] = 0x000080;
    t->colors[THEME_COLOR_MENU_TEXT] = 0xFFFFFF;
    t->colors[THEME_COLOR_MENU_TEXT_DISABLED] = 0x808080;
    t->colors[THEME_COLOR_MENU_SEPARATOR] = 0xFFFFFF;

    t->colors[THEME_COLOR_SCROLLBAR_BG] = 0x404040;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB] = 0xFFFFFF;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB_HOVER] = 0x00FFFF;

    t->colors[THEME_COLOR_TAB_BG] = 0x000000;
    t->colors[THEME_COLOR_TAB_ACTIVE] = 0x000080;
    t->colors[THEME_COLOR_TAB_BORDER] = 0xFFFFFF;

    t->colors[THEME_COLOR_TOOLTIP_BG] = 0x000000;
    t->colors[THEME_COLOR_TOOLTIP_BORDER] = 0xFFFF00;
    t->colors[THEME_COLOR_TOOLTIP_TEXT] = 0xFFFF00;

    t->colors[THEME_COLOR_PROGRESS_BG] = 0x404040;
    t->colors[THEME_COLOR_PROGRESS_FG] = 0x00FF00;

    for (int i = 0; i < THEME_FONT_COUNT; i++) {
        strcpy(t->fonts[i].name, "fixed");
        t->fonts[i].size = 14;
        t->fonts[i].bold = true;
        t->fonts[i].italic = false;
    }

    t->metrics.border_width = 2;
    t->metrics.titlebar_height = 24;
    t->metrics.button_width = 20;
    t->metrics.button_height = 18;
    t->metrics.scrollbar_width = 20;
    t->metrics.menu_item_height = 24;
    t->metrics.icon_size = 48;
    t->metrics.corner_radius = 0;
    t->metrics.padding = 6;
    t->metrics.spacing = 6;
    t->metrics.tab_height = 28;

    t->decorations.style = THEME_STYLE_FLAT;
    t->decorations.corner_radius = 0;
    t->decorations.shadow_enabled = false;
}

static void init_builtin_theme_forest(theme_t *t) {
    memset(t, 0, sizeof(theme_t));

    strcpy(t->name, "Forest");
    strcpy(t->author, "MayteraOS Team");
    strcpy(t->version, "1.0");
    t->is_builtin = true;

    t->colors[THEME_COLOR_BACKGROUND] = 0xE8F0E8;
    t->colors[THEME_COLOR_FOREGROUND] = 0x203020;
    t->colors[THEME_COLOR_ACCENT] = 0x308040;
    t->colors[THEME_COLOR_SELECTION] = 0x308040;
    t->colors[THEME_COLOR_SELECTION_TEXT] = 0xFFFFFF;

    t->colors[THEME_COLOR_TITLEBAR_ACTIVE] = 0x306030;
    t->colors[THEME_COLOR_TITLEBAR_INACTIVE] = 0x405040;
    t->colors[THEME_COLOR_TITLEBAR_TEXT] = 0xFFFFFF;

    t->colors[THEME_COLOR_WINDOW_BG] = 0xE8F0E8;
    t->colors[THEME_COLOR_WINDOW_BORDER] = 0x506050;

    t->colors[THEME_COLOR_CLOSE_BUTTON] = 0x607060;
    t->colors[THEME_COLOR_CLOSE_BUTTON_HOVER] = 0xC05050;
    t->colors[THEME_COLOR_MINIMIZE_BUTTON] = 0x608060;
    t->colors[THEME_COLOR_MAXIMIZE_BUTTON] = 0x608060;

    t->colors[THEME_COLOR_BUTTON_FACE] = 0x80B080;
    t->colors[THEME_COLOR_BUTTON_LIGHT] = 0x90C090;
    t->colors[THEME_COLOR_BUTTON_SHADOW] = 0x608060;
    t->colors[THEME_COLOR_BUTTON_DARK] = 0x406040;
    t->colors[THEME_COLOR_BUTTON_TEXT] = 0xFFFFFF;
    t->colors[THEME_COLOR_BUTTON_DISABLED] = 0x808080;

    t->colors[THEME_COLOR_LABEL_TEXT] = 0x203020;
    t->colors[THEME_COLOR_TEXTBOX_BG] = 0xFAFFF8;
    t->colors[THEME_COLOR_TEXTBOX_BORDER] = 0x608060;
    t->colors[THEME_COLOR_TEXTBOX_TEXT] = 0x203020;
    t->colors[THEME_COLOR_TEXTBOX_CURSOR] = 0x308040;

    t->colors[THEME_COLOR_CHECKBOX_BG] = 0xFAFFF8;
    t->colors[THEME_COLOR_CHECKBOX_BORDER] = 0x608060;
    t->colors[THEME_COLOR_CHECKBOX_CHECK] = 0x308040;

    t->colors[THEME_COLOR_DESKTOP_BG] = 0x405840;

    t->colors[THEME_COLOR_TASKBAR_BG] = 0x304030;
    t->colors[THEME_COLOR_TASKBAR_HOVER] = 0x405040;
    t->colors[THEME_COLOR_TASKBAR_ACTIVE] = 0x506050;
    t->colors[THEME_COLOR_START_BUTTON] = 0x406040;
    t->colors[THEME_COLOR_GAUGE_BG] = 0x203020;
    t->colors[THEME_COLOR_GAUGE_FG] = 0x60A060;

    t->colors[THEME_COLOR_MENU_BG] = 0xF0F8F0;
    t->colors[THEME_COLOR_MENU_BORDER] = 0x608060;
    t->colors[THEME_COLOR_MENU_ITEM_HOVER] = 0x308040;
    t->colors[THEME_COLOR_MENU_TEXT] = 0x203020;
    t->colors[THEME_COLOR_MENU_TEXT_DISABLED] = 0x809080;
    t->colors[THEME_COLOR_MENU_SEPARATOR] = 0xC0D8C0;

    t->colors[THEME_COLOR_SCROLLBAR_BG] = 0xD0E0D0;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB] = 0x80A080;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB_HOVER] = 0x608060;

    t->colors[THEME_COLOR_TAB_BG] = 0xC8D8C8;
    t->colors[THEME_COLOR_TAB_ACTIVE] = 0xF0F8F0;
    t->colors[THEME_COLOR_TAB_BORDER] = 0x608060;

    t->colors[THEME_COLOR_TOOLTIP_BG] = 0xF0FFF0;
    t->colors[THEME_COLOR_TOOLTIP_BORDER] = 0x608060;
    t->colors[THEME_COLOR_TOOLTIP_TEXT] = 0x203020;

    t->colors[THEME_COLOR_PROGRESS_BG] = 0xD0E0D0;
    t->colors[THEME_COLOR_PROGRESS_FG] = 0x308040;

    for (int i = 0; i < THEME_FONT_COUNT; i++) {
        strcpy(t->fonts[i].name, "fixed");
        t->fonts[i].size = 12;
        t->fonts[i].bold = false;
        t->fonts[i].italic = false;
    }
    t->fonts[THEME_FONT_TITLE].bold = true;

    t->metrics.border_width = 1;
    t->metrics.titlebar_height = 20;
    t->metrics.button_width = 16;
    t->metrics.button_height = 14;
    t->metrics.scrollbar_width = 16;
    t->metrics.menu_item_height = 20;
    t->metrics.icon_size = 32;
    t->metrics.corner_radius = 2;
    t->metrics.padding = 4;
    t->metrics.spacing = 4;
    t->metrics.tab_height = 24;

    t->decorations.style = THEME_STYLE_FLAT;
    t->decorations.corner_radius = 2;
    t->decorations.shadow_enabled = false;
}

static void init_builtin_theme_sunset(theme_t *t) {
    memset(t, 0, sizeof(theme_t));

    strcpy(t->name, "Sunset");
    strcpy(t->author, "MayteraOS Team");
    strcpy(t->version, "1.0");
    t->is_builtin = true;

    t->colors[THEME_COLOR_BACKGROUND] = 0xFFF0E8;
    t->colors[THEME_COLOR_FOREGROUND] = 0x402020;
    t->colors[THEME_COLOR_ACCENT] = 0xCC6633;
    t->colors[THEME_COLOR_SELECTION] = 0xCC6633;
    t->colors[THEME_COLOR_SELECTION_TEXT] = 0xFFFFFF;

    t->colors[THEME_COLOR_TITLEBAR_ACTIVE] = 0x804020;
    t->colors[THEME_COLOR_TITLEBAR_INACTIVE] = 0x605040;
    t->colors[THEME_COLOR_TITLEBAR_TEXT] = 0xFFF0E0;

    t->colors[THEME_COLOR_WINDOW_BG] = 0xFFF0E8;
    t->colors[THEME_COLOR_WINDOW_BORDER] = 0x806050;

    t->colors[THEME_COLOR_CLOSE_BUTTON] = 0x806060;
    t->colors[THEME_COLOR_CLOSE_BUTTON_HOVER] = 0xE04040;
    t->colors[THEME_COLOR_MINIMIZE_BUTTON] = 0x806060;
    t->colors[THEME_COLOR_MAXIMIZE_BUTTON] = 0x806060;

    t->colors[THEME_COLOR_BUTTON_FACE] = 0xD09060;
    t->colors[THEME_COLOR_BUTTON_LIGHT] = 0xE0A070;
    t->colors[THEME_COLOR_BUTTON_SHADOW] = 0xA07050;
    t->colors[THEME_COLOR_BUTTON_DARK] = 0x604030;
    t->colors[THEME_COLOR_BUTTON_TEXT] = 0xFFFFFF;
    t->colors[THEME_COLOR_BUTTON_DISABLED] = 0x808080;

    t->colors[THEME_COLOR_LABEL_TEXT] = 0x402020;
    t->colors[THEME_COLOR_TEXTBOX_BG] = 0xFFFAF5;
    t->colors[THEME_COLOR_TEXTBOX_BORDER] = 0x806050;
    t->colors[THEME_COLOR_TEXTBOX_TEXT] = 0x402020;
    t->colors[THEME_COLOR_TEXTBOX_CURSOR] = 0x804020;

    t->colors[THEME_COLOR_CHECKBOX_BG] = 0xFFFAF5;
    t->colors[THEME_COLOR_CHECKBOX_BORDER] = 0x806050;
    t->colors[THEME_COLOR_CHECKBOX_CHECK] = 0xCC6633;

    t->colors[THEME_COLOR_DESKTOP_BG] = 0x705040;

    t->colors[THEME_COLOR_TASKBAR_BG] = 0x503020;
    t->colors[THEME_COLOR_TASKBAR_HOVER] = 0x604030;
    t->colors[THEME_COLOR_TASKBAR_ACTIVE] = 0x705040;
    t->colors[THEME_COLOR_START_BUTTON] = 0x604030;
    t->colors[THEME_COLOR_GAUGE_BG] = 0x302010;
    t->colors[THEME_COLOR_GAUGE_FG] = 0xB07050;

    t->colors[THEME_COLOR_MENU_BG] = 0xFFF8F0;
    t->colors[THEME_COLOR_MENU_BORDER] = 0x806050;
    t->colors[THEME_COLOR_MENU_ITEM_HOVER] = 0xCC6633;
    t->colors[THEME_COLOR_MENU_TEXT] = 0x402020;
    t->colors[THEME_COLOR_MENU_TEXT_DISABLED] = 0x908080;
    t->colors[THEME_COLOR_MENU_SEPARATOR] = 0xD8C0B0;

    t->colors[THEME_COLOR_SCROLLBAR_BG] = 0xE0D0C0;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB] = 0xA08070;
    t->colors[THEME_COLOR_SCROLLBAR_THUMB_HOVER] = 0x806050;

    t->colors[THEME_COLOR_TAB_BG] = 0xE8D8C8;
    t->colors[THEME_COLOR_TAB_ACTIVE] = 0xFFF8F0;
    t->colors[THEME_COLOR_TAB_BORDER] = 0x806050;

    t->colors[THEME_COLOR_TOOLTIP_BG] = 0xFFFFF0;
    t->colors[THEME_COLOR_TOOLTIP_BORDER] = 0x806050;
    t->colors[THEME_COLOR_TOOLTIP_TEXT] = 0x402020;

    t->colors[THEME_COLOR_PROGRESS_BG] = 0xE0D0C0;
    t->colors[THEME_COLOR_PROGRESS_FG] = 0xCC6633;

    for (int i = 0; i < THEME_FONT_COUNT; i++) {
        strcpy(t->fonts[i].name, "fixed");
        t->fonts[i].size = 12;
        t->fonts[i].bold = false;
        t->fonts[i].italic = false;
    }
    t->fonts[THEME_FONT_TITLE].bold = true;

    t->metrics.border_width = 1;
    t->metrics.titlebar_height = 20;
    t->metrics.button_width = 16;
    t->metrics.button_height = 14;
    t->metrics.scrollbar_width = 16;
    t->metrics.menu_item_height = 20;
    t->metrics.icon_size = 32;
    t->metrics.corner_radius = 0;
    t->metrics.padding = 4;
    t->metrics.spacing = 4;
    t->metrics.tab_height = 24;

    t->decorations.style = THEME_STYLE_FLAT;
    t->decorations.corner_radius = 0;
    t->decorations.shadow_enabled = false;
}

// =============================================================================
// Theme Persistence (Added by P14 - Theme Specialist)
// =============================================================================

#define THEME_CONFIG_PATH "/CONFIG/THEME.CFG"

// Callback type for theme change notification
typedef void (*theme_change_callback_t)(int new_theme_index);

#define MAX_THEME_CALLBACKS 16
static theme_change_callback_t g_theme_callbacks[MAX_THEME_CALLBACKS];
static int g_callback_count = 0;

// Register a callback to be notified when theme changes
int theme_register_callback(theme_change_callback_t callback) {
    if (!callback || g_callback_count >= MAX_THEME_CALLBACKS) return -1;
    g_theme_callbacks[g_callback_count++] = callback;
    return 0;
}

// Notify all callbacks that theme changed
static void theme_notify_change(int new_index) {
    for (int i = 0; i < g_callback_count; i++) {
        if (g_theme_callbacks[i]) {
            g_theme_callbacks[i](new_index);
        }
    }
}

// Save the current theme selection to config file
int theme_save_config(void) {
    if (!g_initialized) return -1;

    theme_t *t = theme_get_active();
    if (!t) return -1;

    char config_data[256];
    int len = snprintf(config_data, sizeof(config_data),
        "# MayteraOS Theme Configuration\n"
        "theme=%s\n"
        "index=%d\n",
        t->name, g_active_theme);

    int result = fat_write_file(&g_fat_fs, THEME_CONFIG_PATH,
                                (const uint8_t *)config_data, len);
    if (result < 0) {
        kprintf("[Theme] Warning: could not save config to %s\n", THEME_CONFIG_PATH);
        return -1;
    }

    kprintf("[Theme] Saved theme config: %s (index=%d)\n", t->name, g_active_theme);
    return 0;
}

// Load theme selection from config file
int theme_load_config(void) {
    if (!g_initialized) return -1;

    uint32_t file_size;
    char *data = (char *)fat_read_file(&g_fat_fs, THEME_CONFIG_PATH, &file_size);
    if (!data) {
        kprintf("[Theme] No config file found, using default theme\n");
        return -1;
    }

    int loaded_index = -1;
    char *line = data;
    while (*line) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#' || *line == '\n') {
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
            continue;
        }

        if (strncmp(line, "index=", 6) == 0) {
            loaded_index = atoi(line + 6);
        }
        else if (strncmp(line, "theme=", 6) == 0) {
            char theme_name[THEME_NAME_LEN];
            int i = 0;
            char *p = line + 6;
            while (*p && *p != '\n' && i < THEME_NAME_LEN - 1) {
                theme_name[i++] = *p++;
            }
            theme_name[i] = '\0';

            for (int j = 0; j < g_theme_count; j++) {
                if (strcmp(g_themes[j].name, theme_name) == 0) {
                    loaded_index = j;
                    break;
                }
            }
        }

        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }

    kfree(data);

    if (loaded_index >= 0 && loaded_index < g_theme_count) {
        g_active_theme = loaded_index;
        kprintf("[Theme] Loaded theme config: %s (index=%d)\n",
                g_themes[g_active_theme].name, g_active_theme);
        return 0;
    }

    return -1;
}

// Set active theme and notify (with persistence)
int theme_switch_to(int index) {
    if (index < 0 || index >= g_theme_count) return -1;

    int old_theme = g_active_theme;
    g_active_theme = index;

    kprintf("[Theme] Switched from '%s' to '%s'\n",
            g_themes[old_theme].name, g_themes[index].name);

    theme_notify_change(index);
    theme_save_config();

    return 0;
}

// =============================================================================
// Theme Engine Implementation
// =============================================================================

int theme_engine_init(void) {
    if (g_initialized) {
        return 0;  // Already initialized
    }

    memset(g_themes, 0, sizeof(g_themes));
    g_theme_count = 0;
    g_active_theme = 0;

    // Load built-in themes
    init_builtin_theme_default(&g_themes[0]);
    g_theme_count++;

    init_builtin_theme_dark(&g_themes[1]);
    g_theme_count++;

    init_builtin_theme_light(&g_themes[2]);
    g_theme_count++;

    init_builtin_theme_classic(&g_themes[3]);
    g_theme_count++;

    init_builtin_theme_ocean(&g_themes[4]);
    g_theme_count++;

    init_builtin_theme_forest(&g_themes[5]);
    g_theme_count++;

    init_builtin_theme_sunset(&g_themes[6]);
    g_theme_count++;

    init_builtin_theme_high_contrast(&g_themes[7]);
    g_theme_count++;

    g_initialized = true;

    // Try to load saved theme preference
    theme_load_config();

    kprintf("[Theme] Theme engine initialized with %d built-in themes, active: %s\n",
            g_theme_count, g_themes[g_active_theme].name);
    return 0;
}

void theme_shutdown(void) {
    if (!g_initialized) return;

    g_theme_count = 0;
    g_active_theme = 0;
    g_initialized = false;

    kprintf("[Theme] Theme engine shutdown\n");
}

int theme_load(const char *theme_name) {
    if (!theme_name) return -1;

    // Construct path: /System/Themes/<name>/theme.ini
    char path[THEME_PATH_LEN];
    snprintf(path, sizeof(path), "/System/Themes/%s/theme.ini", theme_name);

    return theme_load_from_path(path);
}

int theme_load_from_path(const char *path) {
    if (!path) return -1;
    if (!g_initialized) {
        kprintf("[Theme] Error: theme engine not initialized\n");
        return -1;
    }
    if (g_theme_count >= THEME_MAX_THEMES) {
        kprintf("[Theme] Error: maximum themes loaded\n");
        return -1;
    }

    // Read file
    uint32_t file_size;
    char *data = (char *)fat_read_file(&g_fat_fs, path, &file_size);
    if (!data) {
        kprintf("[Theme] Error: could not read theme file: %s\n", path);
        return -1;
    }

    // Parse INI data
    theme_t *new_theme = &g_themes[g_theme_count];
    int result = theme_parse_ini(data, file_size, new_theme);

    kfree(data);

    if (result < 0) {
        kprintf("[Theme] Error: failed to parse theme file: %s\n", path);
        return -1;
    }

    // Store path and mark as not built-in
    strncpy(new_theme->path, path, THEME_PATH_LEN - 1);
    new_theme->path[THEME_PATH_LEN - 1] = '\0';
    new_theme->is_builtin = false;

    int theme_index = g_theme_count;
    g_theme_count++;

    kprintf("[Theme] Loaded theme '%s' from %s (index=%d)\n",
            new_theme->name, path, theme_index);

    return theme_index;
}

int theme_set_active(const char *theme_name) {
    if (!theme_name) return -1;

    for (int i = 0; i < g_theme_count; i++) {
        if (strcmp(g_themes[i].name, theme_name) == 0) {
            g_active_theme = i;
            kprintf("[Theme] Active theme set to: %s\n", theme_name);
            return 0;
        }
    }

    kprintf("[Theme] Error: theme not found: %s\n", theme_name);
    return -1;
}

int theme_set_active_by_index(int index) {
    if (index < 0 || index >= g_theme_count) {
        kprintf("[Theme] Error: invalid theme index: %d\n", index);
        return -1;
    }

    g_active_theme = index;
    kprintf("[Theme] Active theme set to: %s (index=%d)\n",
            g_themes[index].name, index);
    return 0;
}

theme_t *theme_get_active(void) {
    if (!g_initialized || g_theme_count == 0) {
        return NULL;
    }
    // Defensive clamp: if the active index is somehow out of range (a stale or
    // oversized index from a saved profile, or a Settings theme list longer
    // than the kernel's actual theme set), fall back to a valid theme instead
    // of returning an uninitialized g_themes[] slot full of zero colors, which
    // makes every theme-aware app render unreadable.
    if (g_active_theme < 0 || g_active_theme >= g_theme_count) {
        g_active_theme = 0;
    }
    return &g_themes[g_active_theme];
}

theme_t *theme_get_by_name(const char *name) {
    if (!name) return NULL;

    for (int i = 0; i < g_theme_count; i++) {
        if (strcmp(g_themes[i].name, name) == 0) {
            return &g_themes[i];
        }
    }
    return NULL;
}

theme_t *theme_get_by_index(int index) {
    if (index < 0 || index >= g_theme_count) {
        return NULL;
    }
    return &g_themes[index];
}

int theme_get_count(void) {
    extern int themes_get_builtin_count(void);
    // Return count from legacy themes.c which has 12 built-in themes
    return 12;  // themes.c has 12 hardcoded themes
}

int theme_get_active_index(void) {
    return g_active_theme;
}

const char **theme_get_names(int *count) {
    if (count) *count = g_theme_count;

    for (int i = 0; i < g_theme_count; i++) {
        g_theme_names[i] = g_themes[i].name;
    }

    return g_theme_names;
}

// =============================================================================
// Color, Font, and Metric Query Functions
// =============================================================================

uint32_t theme_color(theme_color_id_t id) {
    theme_t *t = theme_get_active();
    if (!t || id >= THEME_COLOR_COUNT) {
        return 0;
    }
    return t->colors[id];
}

uint32_t theme_color_alpha(theme_color_id_t id, uint8_t alpha) {
    uint32_t color = theme_color(id);
    return (alpha << 24) | (color & 0x00FFFFFF);
}

uint32_t theme_blend_colors(uint32_t c1, uint32_t c2, float ratio) {
    if (ratio <= 0.0f) return c1;
    if (ratio >= 1.0f) return c2;

    int r1 = (c1 >> 16) & 0xFF;
    int g1 = (c1 >> 8) & 0xFF;
    int b1 = c1 & 0xFF;

    int r2 = (c2 >> 16) & 0xFF;
    int g2 = (c2 >> 8) & 0xFF;
    int b2 = c2 & 0xFF;

    int r = r1 + (int)((r2 - r1) * ratio);
    int g = g1 + (int)((g2 - g1) * ratio);
    int b = b1 + (int)((b2 - b1) * ratio);

    return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

const theme_font_def_t *theme_font(theme_font_id_t id) {
    theme_t *t = theme_get_active();
    if (!t || id >= THEME_FONT_COUNT) {
        return NULL;
    }
    return &t->fonts[id];
}

int theme_metric(theme_metric_id_t id) {
    theme_t *t = theme_get_active();
    if (!t) return 0;

    switch (id) {
        case THEME_METRIC_BORDER_WIDTH:     return t->metrics.border_width;
        case THEME_METRIC_TITLEBAR_HEIGHT:  return t->metrics.titlebar_height;
        case THEME_METRIC_BUTTON_WIDTH:     return t->metrics.button_width;
        case THEME_METRIC_BUTTON_HEIGHT:    return t->metrics.button_height;
        case THEME_METRIC_SCROLLBAR_WIDTH:  return t->metrics.scrollbar_width;
        case THEME_METRIC_MENU_ITEM_HEIGHT: return t->metrics.menu_item_height;
        case THEME_METRIC_ICON_SIZE:        return t->metrics.icon_size;
        case THEME_METRIC_CORNER_RADIUS:    return t->metrics.corner_radius;
        case THEME_METRIC_PADDING:          return t->metrics.padding;
        case THEME_METRIC_SPACING:          return t->metrics.spacing;
        case THEME_METRIC_TAB_HEIGHT:       return t->metrics.tab_height;
        default:                            return 0;
    }
}

theme_style_t theme_get_style(void) {
    theme_t *t = theme_get_active();
    if (!t) return THEME_STYLE_FLAT;
    return t->decorations.style;
}

// =============================================================================
// Color Utilities
// =============================================================================

uint32_t theme_lighten(uint32_t color, int percent) {
    if (percent <= 0) return color;
    if (percent > 100) percent = 100;

    int r = (color >> 16) & 0xFF;
    int g = (color >> 8) & 0xFF;
    int b = color & 0xFF;

    r = r + ((255 - r) * percent / 100);
    g = g + ((255 - g) * percent / 100);
    b = b + ((255 - b) * percent / 100);

    return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

uint32_t theme_darken(uint32_t color, int percent) {
    if (percent <= 0) return color;
    if (percent > 100) percent = 100;

    int r = (color >> 16) & 0xFF;
    int g = (color >> 8) & 0xFF;
    int b = color & 0xFF;

    r = r - (r * percent / 100);
    g = g - (g * percent / 100);
    b = b - (b * percent / 100);

    return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

// =============================================================================
// Drawing Helpers
// =============================================================================

// Draw text helper (internal)
static void draw_text(int x, int y, const char *text, uint32_t color) {
    if (!text) return;

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

// Draw centered text helper
static void draw_text_centered(int x, int y, int w, int h, const char *text, uint32_t color) {
    if (!text) return;

    int text_len = strlen(text);
    int text_w = text_len * FONT_WIDTH;
    int text_h = FONT_HEIGHT;

    int tx = x + (w - text_w) / 2;
    int ty = y + (h - text_h) / 2;

    draw_text(tx, ty, text, color);
}

void theme_draw_3d_border(int x, int y, int w, int h, bool raised) {
    theme_t *t = theme_get_active();
    if (!t) return;

    uint32_t light = t->colors[THEME_COLOR_BUTTON_LIGHT];
    uint32_t shadow = t->colors[THEME_COLOR_BUTTON_SHADOW];
    uint32_t dark = t->colors[THEME_COLOR_BUTTON_DARK];

    if (!raised) {
        // Swap for sunken effect
        uint32_t tmp = light;
        light = dark;
        dark = tmp;
    }

    // Outer light edge (top and left)
    fb_draw_line(x, y, x + w - 1, y, light);
    fb_draw_line(x, y, x, y + h - 1, light);

    // Inner light edge
    fb_draw_line(x + 1, y + 1, x + w - 2, y + 1, light);
    fb_draw_line(x + 1, y + 1, x + 1, y + h - 2, light);

    // Outer dark edge (bottom and right)
    fb_draw_line(x, y + h - 1, x + w - 1, y + h - 1, dark);
    fb_draw_line(x + w - 1, y, x + w - 1, y + h - 1, dark);

    // Inner shadow edge
    fb_draw_line(x + 1, y + h - 2, x + w - 2, y + h - 2, shadow);
    fb_draw_line(x + w - 2, y + 1, x + w - 2, y + h - 2, shadow);
}

void theme_draw_frame(int x, int y, int w, int h, frame_style_t style) {
    theme_t *t = theme_get_active();
    if (!t) return;

    switch (style) {
        case FRAME_STYLE_FLAT:
            fb_draw_rect(x, y, w, h, t->colors[THEME_COLOR_BUTTON_DARK]);
            break;

        case FRAME_STYLE_RAISED:
            theme_draw_3d_border(x, y, w, h, true);
            break;

        case FRAME_STYLE_SUNKEN:
            theme_draw_3d_border(x, y, w, h, false);
            break;

        case FRAME_STYLE_ETCHED:
            // Double sunken/raised effect
            theme_draw_3d_border(x, y, w, h, false);
            theme_draw_3d_border(x + 1, y + 1, w - 2, h - 2, true);
            break;

        case FRAME_STYLE_RIDGE:
            // Double raised/sunken effect
            theme_draw_3d_border(x, y, w, h, true);
            theme_draw_3d_border(x + 1, y + 1, w - 2, h - 2, false);
            break;
    }
}

void theme_draw_button(int x, int y, int w, int h, button_state_t state, const char *text) {
    theme_t *t = theme_get_active();
    if (!t) return;

    uint32_t bg_color;
    uint32_t text_color;
    bool raised = true;

    switch (state) {
        case BUTTON_STATE_NORMAL:
            bg_color = t->colors[THEME_COLOR_BUTTON_FACE];
            text_color = t->colors[THEME_COLOR_BUTTON_TEXT];
            break;

        case BUTTON_STATE_HOVER:
            bg_color = theme_lighten(t->colors[THEME_COLOR_BUTTON_FACE], 10);
            text_color = t->colors[THEME_COLOR_BUTTON_TEXT];
            break;

        case BUTTON_STATE_PRESSED:
            bg_color = t->colors[THEME_COLOR_BUTTON_SHADOW];
            text_color = t->colors[THEME_COLOR_BUTTON_TEXT];
            raised = false;
            break;

        case BUTTON_STATE_DISABLED:
            bg_color = t->colors[THEME_COLOR_BUTTON_FACE];
            text_color = t->colors[THEME_COLOR_BUTTON_DISABLED];
            break;

        case BUTTON_STATE_FOCUSED:
            bg_color = t->colors[THEME_COLOR_BUTTON_FACE];
            text_color = t->colors[THEME_COLOR_BUTTON_TEXT];
            break;

        default:
            bg_color = t->colors[THEME_COLOR_BUTTON_FACE];
            text_color = t->colors[THEME_COLOR_BUTTON_TEXT];
            break;
    }

    // Draw button background
    fb_fill_rect(x, y, w, h, bg_color);

    // Draw 3D border based on decoration style
    theme_style_t deco_style = t->decorations.style;
    if (deco_style == THEME_STYLE_WIN95 || deco_style == THEME_STYLE_MOTIF) {
        theme_draw_3d_border(x, y, w, h, raised);
    } else {
        // Flat style - just a simple border
        fb_draw_rect(x, y, w, h, t->colors[THEME_COLOR_BUTTON_DARK]);
    }

    // Draw text
    if (text) {
        int offset = (state == BUTTON_STATE_PRESSED) ? 1 : 0;
        draw_text_centered(x + offset, y + offset, w, h, text, text_color);
    }

    // Draw focus rectangle if focused
    if (state == BUTTON_STATE_FOCUSED) {
        // Dotted rectangle inside button
        for (int i = x + 3; i < x + w - 3; i += 2) {
            fb_put_pixel(i, y + 3, t->colors[THEME_COLOR_BUTTON_TEXT]);
            fb_put_pixel(i, y + h - 4, t->colors[THEME_COLOR_BUTTON_TEXT]);
        }
        for (int j = y + 3; j < y + h - 3; j += 2) {
            fb_put_pixel(x + 3, j, t->colors[THEME_COLOR_BUTTON_TEXT]);
            fb_put_pixel(x + w - 4, j, t->colors[THEME_COLOR_BUTTON_TEXT]);
        }
    }
}

void theme_draw_titlebar(int x, int y, int w, int h, const char *title, bool active) {
    theme_t *t = theme_get_active();
    if (!t) return;

    uint32_t bg_color = active ?
        t->colors[THEME_COLOR_TITLEBAR_ACTIVE] :
        t->colors[THEME_COLOR_TITLEBAR_INACTIVE];
    uint32_t text_color = t->colors[THEME_COLOR_TITLEBAR_TEXT];

    // Draw titlebar background
    fb_fill_rect(x, y, w, h, bg_color);

    // Draw title text (left aligned with padding)
    if (title) {
        int tx = x + 4;
        int ty = y + (h - FONT_HEIGHT) / 2;
        draw_text(tx, ty, title, text_color);
    }
}

void theme_draw_scrollbar_thumb(int x, int y, int w, int h, bool horizontal, bool hovered) {
    theme_t *t = theme_get_active();
    if (!t) return;

    UNUSED(horizontal);

    uint32_t thumb_color = hovered ?
        t->colors[THEME_COLOR_SCROLLBAR_THUMB_HOVER] :
        t->colors[THEME_COLOR_SCROLLBAR_THUMB];

    fb_fill_rect(x, y, w, h, thumb_color);

    // Draw 3D effect for classic styles
    if (t->decorations.style == THEME_STYLE_WIN95 || t->decorations.style == THEME_STYLE_MOTIF) {
        theme_draw_3d_border(x, y, w, h, true);
    }
}

void theme_draw_progress_bar(int x, int y, int w, int h, int percent) {
    theme_t *t = theme_get_active();
    if (!t) return;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    // Draw background
    fb_fill_rect(x, y, w, h, t->colors[THEME_COLOR_PROGRESS_BG]);

    // Draw sunken border
    theme_draw_3d_border(x, y, w, h, false);

    // Draw progress fill
    int fill_w = (w - 4) * percent / 100;
    if (fill_w > 0) {
        fb_fill_rect(x + 2, y + 2, fill_w, h - 4, t->colors[THEME_COLOR_PROGRESS_FG]);
    }
}

void theme_draw_menu_item(int x, int y, int w, int h, const char *text, bool selected, bool disabled) {
    theme_t *t = theme_get_active();
    if (!t) return;

    uint32_t bg_color = selected ?
        t->colors[THEME_COLOR_MENU_ITEM_HOVER] :
        t->colors[THEME_COLOR_MENU_BG];

    uint32_t text_color;
    if (disabled) {
        text_color = t->colors[THEME_COLOR_MENU_TEXT_DISABLED];
    } else if (selected) {
        text_color = t->colors[THEME_COLOR_SELECTION_TEXT];
    } else {
        text_color = t->colors[THEME_COLOR_MENU_TEXT];
    }

    // Draw background
    fb_fill_rect(x, y, w, h, bg_color);

    // Draw text (left aligned with padding)
    if (text) {
        int tx = x + 8;
        int ty = y + (h - FONT_HEIGHT) / 2;
        draw_text(tx, ty, text, text_color);
    }
}

void theme_draw_checkbox(int x, int y, bool checked, bool hovered, bool disabled) {
    theme_t *t = theme_get_active();
    if (!t) return;

    int box_size = 14;
    uint32_t bg_color = t->colors[THEME_COLOR_CHECKBOX_BG];
    uint32_t border_color = t->colors[THEME_COLOR_CHECKBOX_BORDER];
    uint32_t check_color = disabled ?
        t->colors[THEME_COLOR_BUTTON_DISABLED] :
        t->colors[THEME_COLOR_CHECKBOX_CHECK];

    if (hovered && !disabled) {
        bg_color = theme_lighten(bg_color, 10);
    }

    // Draw checkbox background
    fb_fill_rect(x, y, box_size, box_size, bg_color);

    // Draw border (sunken for classic style)
    if (t->decorations.style == THEME_STYLE_WIN95 || t->decorations.style == THEME_STYLE_MOTIF) {
        theme_draw_3d_border(x, y, box_size, box_size, false);
    } else {
        fb_draw_rect(x, y, box_size, box_size, border_color);
    }

    // Draw checkmark if checked
    if (checked) {
        int cx = x + 3;
        int cy = y + box_size / 2;

        // Draw checkmark as two lines
        for (int i = 0; i < 4; i++) {
            fb_put_pixel(cx + i, cy + i, check_color);
            fb_put_pixel(cx + i, cy + i + 1, check_color);
        }
        for (int i = 0; i < 7; i++) {
            fb_put_pixel(cx + 3 + i, cy + 3 - i, check_color);
            fb_put_pixel(cx + 3 + i, cy + 4 - i, check_color);
        }
    }
}

void theme_draw_radio_button(int x, int y, bool selected, bool hovered, bool disabled) {
    theme_t *t = theme_get_active();
    if (!t) return;

    int radius = 6;
    int cx = x + radius;
    int cy = y + radius;

    uint32_t bg_color = t->colors[THEME_COLOR_CHECKBOX_BG];
    uint32_t border_color = t->colors[THEME_COLOR_CHECKBOX_BORDER];
    uint32_t select_color = disabled ?
        t->colors[THEME_COLOR_BUTTON_DISABLED] :
        t->colors[THEME_COLOR_CHECKBOX_CHECK];

    if (hovered && !disabled) {
        bg_color = theme_lighten(bg_color, 10);
    }

    // Draw circle background (approximated with filled rect for simplicity)
    // In a real implementation, we'd use a proper circle drawing algorithm
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy <= radius * radius) {
                fb_put_pixel(cx + dx, cy + dy, bg_color);
            }
        }
    }

    // Draw border circle
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int dist_sq = dx * dx + dy * dy;
            if (dist_sq >= (radius - 1) * (radius - 1) && dist_sq <= radius * radius) {
                fb_put_pixel(cx + dx, cy + dy, border_color);
            }
        }
    }

    // Draw selection dot if selected
    if (selected) {
        int inner_r = 3;
        for (int dy = -inner_r; dy <= inner_r; dy++) {
            for (int dx = -inner_r; dx <= inner_r; dx++) {
                if (dx * dx + dy * dy <= inner_r * inner_r) {
                    fb_put_pixel(cx + dx, cy + dy, select_color);
                }
            }
        }
    }
}

void theme_draw_tab(int x, int y, int w, int h, const char *text, bool active) {
    theme_t *t = theme_get_active();
    if (!t) return;

    uint32_t bg_color = active ?
        t->colors[THEME_COLOR_TAB_ACTIVE] :
        t->colors[THEME_COLOR_TAB_BG];
    uint32_t text_color = t->colors[THEME_COLOR_LABEL_TEXT];
    uint32_t border_color = t->colors[THEME_COLOR_TAB_BORDER];

    // Draw tab background
    fb_fill_rect(x, y, w, h, bg_color);

    // Draw border (top, left, right - no bottom for active tab)
    fb_draw_line(x, y, x + w - 1, y, border_color);      // Top
    fb_draw_line(x, y, x, y + h - 1, border_color);      // Left
    fb_draw_line(x + w - 1, y, x + w - 1, y + h - 1, border_color); // Right

    if (!active) {
        fb_draw_line(x, y + h - 1, x + w - 1, y + h - 1, border_color); // Bottom
    }

    // Draw text centered
    if (text) {
        draw_text_centered(x, y, w, h, text, text_color);
    }
}

void theme_draw_tooltip(int x, int y, int w, int h) {
    theme_t *t = theme_get_active();
    if (!t) return;

    // Draw tooltip background
    fb_fill_rect(x, y, w, h, t->colors[THEME_COLOR_TOOLTIP_BG]);

    // Draw border
    fb_draw_rect(x, y, w, h, t->colors[THEME_COLOR_TOOLTIP_BORDER]);
}

// =============================================================================
// Theme Syscall Implementations (Added by P14 - Theme Specialist)
// These are called from syscall.c
// =============================================================================

int64_t sys_theme_get_active(void) {
    return theme_get_active_index();
}

int64_t sys_theme_get_color(uint64_t color_id) {
    if (color_id >= THEME_COLOR_COUNT) return 0;
    return theme_color((theme_color_id_t)color_id);
}

int64_t sys_theme_get_count(void) {
    return theme_get_count();
}

int64_t sys_theme_set_active(uint64_t index) {
    // TODO: Check capability CAP_SYSTEM_APPEARANCE
    return theme_switch_to((int)index);
}

int64_t sys_theme_get_name(uint64_t index, char *buf, uint64_t buf_size) {
    if (!buf || buf_size == 0) return -1;
    if ((int)index >= theme_get_count()) return -1;

    theme_t *t = theme_get_by_index((int)index);
    if (!t) return -1;

    size_t name_len = strlen(t->name);
    if (name_len >= buf_size) name_len = buf_size - 1;
    memcpy(buf, t->name, name_len);
    buf[name_len] = '\0';

    return name_len;
}

int64_t sys_theme_get_metric(uint64_t metric_id) {
    return theme_metric((theme_metric_id_t)metric_id);
}
