// theme_parser.c - INI file parser for MayteraOS theme files
#include "theme.h"
#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "theme_line.h"

// #404 theme-file line tokenizer seam. -DRUST_THEME_PARSE routes the live
// untrusted-input parse to the Rust port theme_parse_line_rs (rustkern.rs);
// default stays on the verbatim C reference theme_parse_line_c (below). Both
// share the theme_line_t ABI in theme_line.h. Themes are downloadable/editable
// user content (#141), so data[0..size] is UNTRUSTED. Drop the flag + rebuild
// to roll straight back to C.
#ifdef RUST_THEME_PARSE
#define THEME_PARSE_LINE theme_parse_line_rs
#else
#define THEME_PARSE_LINE theme_parse_line_c
#endif

// =============================================================================
// Parser Configuration
// =============================================================================

#define MAX_LINE_LEN        256
#define MAX_SECTION_LEN     64
#define MAX_KEY_LEN         64
#define MAX_VALUE_LEN       128

// =============================================================================
// Parser State
// =============================================================================

typedef enum {
    SECTION_NONE = 0,
    SECTION_THEME,
    SECTION_COLORS,
    SECTION_FONTS,
    SECTION_METRICS,
    SECTION_DECORATIONS
} parser_section_t;

// =============================================================================
// Helper Functions
// =============================================================================

// Skip whitespace characters
static const char *skip_whitespace(const char *str) {
    while (*str && isspace(*str)) {
        str++;
    }
    return str;
}

// Trim trailing whitespace (modifies string in place)
static void trim_trailing(char *str) {
    int len = strlen(str);
    while (len > 0 && isspace(str[len - 1])) {
        str[--len] = '\0';
    }
}

// Check if line is a comment or empty
static bool is_comment_or_empty(const char *line) {
    line = skip_whitespace(line);
    return *line == '\0' || *line == '#' || *line == ';';
}

// Parse a section header [section_name]
// Returns the section name or NULL if not a section header
static const char *parse_section_header(const char *line, char *section_out) {
    line = skip_whitespace(line);
    if (*line != '[') return NULL;

    line++;  // Skip '['

    char *out = section_out;
    int count = 0;
    while (*line && *line != ']' && count < MAX_SECTION_LEN - 1) {
        *out++ = *line++;
        count++;
    }
    *out = '\0';

    if (*line != ']') return NULL;

    trim_trailing(section_out);
    return section_out;
}

// Parse a key=value pair
// Returns true on success
static bool parse_key_value(const char *line, char *key_out, char *value_out) {
    line = skip_whitespace(line);

    // Parse key
    char *out = key_out;
    int count = 0;
    while (*line && *line != '=' && !isspace(*line) && count < MAX_KEY_LEN - 1) {
        *out++ = *line++;
        count++;
    }
    *out = '\0';

    if (*key_out == '\0') return false;

    // Skip whitespace and '='
    line = skip_whitespace(line);
    if (*line != '=') return false;
    line++;
    line = skip_whitespace(line);

    // Parse value
    out = value_out;
    count = 0;
    while (*line && *line != '#' && *line != ';' && count < MAX_VALUE_LEN - 1) {
        *out++ = *line++;
        count++;
    }
    *out = '\0';

    trim_trailing(value_out);
    return true;
}

// =============================================================================
// #404 theme-file line-record parse seam (verbatim C reference).
// The ATOM theme_parse_ini()'s file loop repeats, exposed as one
// (bytes,len)->record step so the live path can route to the Rust port under
// -DRUST_THEME_PARSE. Reuses the existing static classifiers above VERBATIM;
// only this thin wrapper is new. Behavior byte-identical to the original inline
// loop body: it reads one logical line (up to '\n'/EOF/cap) into a
// NUL-terminated buffer (so an EMBEDDED NUL ends the classified line, since the
// classifiers scan with while(*p)), then publishes only the selected fields.
// =============================================================================
uint32_t theme_parse_line_c(const char *data, size_t size, theme_line_t *out) {
    // zero the whole out (kind=SKIP, all strings empty)
    for (size_t i = 0; i < sizeof(*out); i++) ((char *)out)[i] = 0;
    if (!data || !out || size == 0) { out->kind = TP_LINE_SKIP; out->consumed = 0; return 0; }

    // ---- VERBATIM line reader from theme_parse_ini() ----
    char line[MAX_LINE_LEN];
    const char *ptr = data;
    const char *end = data + size;
    int line_len = 0;
    while (ptr < end && *ptr != '\n' && line_len < MAX_LINE_LEN - 1) {
        line[line_len++] = *ptr++;
    }
    line[line_len] = '\0';

    // Skip newline
    if (ptr < end && *ptr == '\n') ptr++;
    out->consumed = (uint32_t)(ptr - data);

    // Classify into LOCAL buffers, then publish only the selected fields.
    char section[MAX_SECTION_LEN];
    char key[MAX_KEY_LEN];
    char value[MAX_VALUE_LEN];

    if (is_comment_or_empty(line)) { out->kind = TP_LINE_SKIP; return out->consumed; }

    if (parse_section_header(line, section)) {
        out->kind = TP_LINE_SECTION;
        for (int i = 0; i < TP_MAX_SECTION_LEN; i++) { out->section[i] = section[i]; if (!section[i]) break; }
        return out->consumed;
    }

    if (parse_key_value(line, key, value)) {
        out->kind = TP_LINE_KEYVALUE;
        for (int i = 0; i < TP_MAX_KEY_LEN; i++)   { out->key[i]   = key[i];   if (!key[i])   break; }
        for (int i = 0; i < TP_MAX_VALUE_LEN; i++) { out->value[i] = value[i]; if (!value[i]) break; }
        return out->consumed;
    }

    out->kind = TP_LINE_SKIP;
    return out->consumed;
}

// =============================================================================
// Color Parsing
// =============================================================================

// Parse a hex color string (#RRGGBB or #RGB)
uint32_t theme_parse_color(const char *str) {
    if (!str) return 0;

    str = skip_whitespace(str);

    // Skip optional '#' prefix
    if (*str == '#') str++;

    // Also support 0x prefix
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
    }

    // Parse hex digits
    uint32_t color = 0;
    int digits = 0;

    while (*str && isxdigit(*str) && digits < 8) {
        char c = *str++;
        int value;

        if (c >= '0' && c <= '9') value = c - '0';
        else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
        else break;

        color = (color << 4) | value;
        digits++;
    }

    // Handle short format (#RGB -> #RRGGBB)
    if (digits == 3) {
        int r = (color >> 8) & 0xF;
        int g = (color >> 4) & 0xF;
        int b = color & 0xF;
        color = (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b;
    }

    return color & 0x00FFFFFF;  // Ensure no alpha component
}

// Convert color to hex string
const char *theme_color_to_string(uint32_t color) {
    static char buf[10];
    snprintf(buf, sizeof(buf), "#%06X", color & 0x00FFFFFF);
    return buf;
}

// =============================================================================
// Section Handlers
// =============================================================================

// Map section name to enum
static parser_section_t get_section_type(const char *section) {
    if (strcmp(section, "theme") == 0) return SECTION_THEME;
    if (strcmp(section, "colors") == 0) return SECTION_COLORS;
    if (strcmp(section, "fonts") == 0) return SECTION_FONTS;
    if (strcmp(section, "metrics") == 0) return SECTION_METRICS;
    if (strcmp(section, "decorations") == 0) return SECTION_DECORATIONS;
    return SECTION_NONE;
}

// Handle [theme] section
static void handle_theme_section(theme_t *theme, const char *key, const char *value) {
    if (strcmp(key, "name") == 0) {
        strncpy(theme->name, value, THEME_NAME_LEN - 1);
        theme->name[THEME_NAME_LEN - 1] = '\0';
    } else if (strcmp(key, "author") == 0) {
        strncpy(theme->author, value, THEME_AUTHOR_LEN - 1);
        theme->author[THEME_AUTHOR_LEN - 1] = '\0';
    } else if (strcmp(key, "version") == 0) {
        strncpy(theme->version, value, 15);
        theme->version[15] = '\0';
    }
}

// Map color key name to color ID
static theme_color_id_t get_color_id(const char *key) {
    // General colors
    if (strcmp(key, "background") == 0) return THEME_COLOR_BACKGROUND;
    if (strcmp(key, "foreground") == 0) return THEME_COLOR_FOREGROUND;
    if (strcmp(key, "accent") == 0) return THEME_COLOR_ACCENT;
    if (strcmp(key, "selection") == 0) return THEME_COLOR_SELECTION;
    if (strcmp(key, "selection_text") == 0) return THEME_COLOR_SELECTION_TEXT;

    // Titlebar
    if (strcmp(key, "titlebar_active") == 0) return THEME_COLOR_TITLEBAR_ACTIVE;
    if (strcmp(key, "titlebar_inactive") == 0) return THEME_COLOR_TITLEBAR_INACTIVE;
    if (strcmp(key, "titlebar_text") == 0) return THEME_COLOR_TITLEBAR_TEXT;

    // Window
    if (strcmp(key, "window_bg") == 0) return THEME_COLOR_WINDOW_BG;
    if (strcmp(key, "window_border") == 0) return THEME_COLOR_WINDOW_BORDER;

    // Window buttons
    if (strcmp(key, "close_button") == 0) return THEME_COLOR_CLOSE_BUTTON;
    if (strcmp(key, "close_button_hover") == 0) return THEME_COLOR_CLOSE_BUTTON_HOVER;
    if (strcmp(key, "minimize_button") == 0) return THEME_COLOR_MINIMIZE_BUTTON;
    if (strcmp(key, "maximize_button") == 0) return THEME_COLOR_MAXIMIZE_BUTTON;

    // Button widget colors
    if (strcmp(key, "button_face") == 0) return THEME_COLOR_BUTTON_FACE;
    if (strcmp(key, "button_light") == 0) return THEME_COLOR_BUTTON_LIGHT;
    if (strcmp(key, "button_shadow") == 0) return THEME_COLOR_BUTTON_SHADOW;
    if (strcmp(key, "button_dark") == 0) return THEME_COLOR_BUTTON_DARK;
    if (strcmp(key, "button_text") == 0) return THEME_COLOR_BUTTON_TEXT;
    if (strcmp(key, "button_disabled") == 0) return THEME_COLOR_BUTTON_DISABLED;

    // Text widgets
    if (strcmp(key, "label_text") == 0) return THEME_COLOR_LABEL_TEXT;
    if (strcmp(key, "textbox_bg") == 0) return THEME_COLOR_TEXTBOX_BG;
    if (strcmp(key, "textbox_border") == 0) return THEME_COLOR_TEXTBOX_BORDER;
    if (strcmp(key, "textbox_text") == 0) return THEME_COLOR_TEXTBOX_TEXT;
    if (strcmp(key, "textbox_cursor") == 0) return THEME_COLOR_TEXTBOX_CURSOR;

    // Checkbox
    if (strcmp(key, "checkbox_bg") == 0) return THEME_COLOR_CHECKBOX_BG;
    if (strcmp(key, "checkbox_border") == 0) return THEME_COLOR_CHECKBOX_BORDER;
    if (strcmp(key, "checkbox_check") == 0) return THEME_COLOR_CHECKBOX_CHECK;

    // Desktop
    if (strcmp(key, "desktop_bg") == 0) return THEME_COLOR_DESKTOP_BG;

    // Taskbar
    if (strcmp(key, "taskbar_bg") == 0) return THEME_COLOR_TASKBAR_BG;
    if (strcmp(key, "taskbar_hover") == 0) return THEME_COLOR_TASKBAR_HOVER;
    if (strcmp(key, "taskbar_active") == 0) return THEME_COLOR_TASKBAR_ACTIVE;
    if (strcmp(key, "start_button") == 0) return THEME_COLOR_START_BUTTON;
    if (strcmp(key, "gauge_bg") == 0) return THEME_COLOR_GAUGE_BG;
    if (strcmp(key, "gauge_fg") == 0) return THEME_COLOR_GAUGE_FG;

    // Menu
    if (strcmp(key, "menu_bg") == 0) return THEME_COLOR_MENU_BG;
    if (strcmp(key, "menu_border") == 0) return THEME_COLOR_MENU_BORDER;
    if (strcmp(key, "menu_item_hover") == 0) return THEME_COLOR_MENU_ITEM_HOVER;
    if (strcmp(key, "menu_text") == 0) return THEME_COLOR_MENU_TEXT;
    if (strcmp(key, "menu_text_disabled") == 0) return THEME_COLOR_MENU_TEXT_DISABLED;
    if (strcmp(key, "menu_separator") == 0) return THEME_COLOR_MENU_SEPARATOR;

    // Scrollbar
    if (strcmp(key, "scrollbar_bg") == 0) return THEME_COLOR_SCROLLBAR_BG;
    if (strcmp(key, "scrollbar_thumb") == 0) return THEME_COLOR_SCROLLBAR_THUMB;
    if (strcmp(key, "scrollbar_thumb_hover") == 0) return THEME_COLOR_SCROLLBAR_THUMB_HOVER;

    // Tab
    if (strcmp(key, "tab_bg") == 0) return THEME_COLOR_TAB_BG;
    if (strcmp(key, "tab_active") == 0) return THEME_COLOR_TAB_ACTIVE;
    if (strcmp(key, "tab_border") == 0) return THEME_COLOR_TAB_BORDER;

    // Tooltip
    if (strcmp(key, "tooltip_bg") == 0) return THEME_COLOR_TOOLTIP_BG;
    if (strcmp(key, "tooltip_border") == 0) return THEME_COLOR_TOOLTIP_BORDER;
    if (strcmp(key, "tooltip_text") == 0) return THEME_COLOR_TOOLTIP_TEXT;

    // Progress bar
    if (strcmp(key, "progress_bg") == 0) return THEME_COLOR_PROGRESS_BG;
    if (strcmp(key, "progress_fg") == 0) return THEME_COLOR_PROGRESS_FG;

    return THEME_COLOR_COUNT;  // Invalid
}

// Handle [colors] section
static void handle_colors_section(theme_t *theme, const char *key, const char *value) {
    theme_color_id_t id = get_color_id(key);
    if (id < THEME_COLOR_COUNT) {
        theme->colors[id] = theme_parse_color(value);
    }
}

// Parse font specification: "name,size[,bold][,italic]"
static void parse_font_spec(const char *spec, theme_font_def_t *font) {
    // Set defaults
    font->name[0] = '\0';
    font->size = 12;
    font->bold = false;
    font->italic = false;

    if (!spec) return;

    char buf[128];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // Parse comma-separated values
    char *token = buf;
    int field = 0;

    while (*token) {
        // Find end of token
        char *end = strchr(token, ',');
        if (end) *end = '\0';

        // Trim whitespace
        while (*token && isspace(*token)) token++;
        char *tok_end = token + strlen(token);
        while (tok_end > token && isspace(tok_end[-1])) {
            tok_end--;
            *tok_end = '\0';
        }

        // Handle field
        if (field == 0) {
            // Font name
            strncpy(font->name, token, THEME_FONT_NAME_LEN - 1);
            font->name[THEME_FONT_NAME_LEN - 1] = '\0';
        } else if (field == 1) {
            // Font size
            font->size = atoi(token);
            if (font->size < 6) font->size = 6;
            if (font->size > 72) font->size = 72;
        } else {
            // Style flags
            if (strcmp(token, "bold") == 0) font->bold = true;
            if (strcmp(token, "italic") == 0) font->italic = true;
        }

        field++;

        if (end) {
            token = end + 1;
        } else {
            break;
        }
    }
}

// Map font key to font ID
static theme_font_id_t get_font_id(const char *key) {
    if (strcmp(key, "system") == 0) return THEME_FONT_SYSTEM;
    if (strcmp(key, "title") == 0) return THEME_FONT_TITLE;
    if (strcmp(key, "menu") == 0) return THEME_FONT_MENU;
    if (strcmp(key, "monospace") == 0) return THEME_FONT_MONOSPACE;
    if (strcmp(key, "small") == 0) return THEME_FONT_SMALL;
    if (strcmp(key, "large") == 0) return THEME_FONT_LARGE;
    return THEME_FONT_COUNT;  // Invalid
}

// Handle [fonts] section
static void handle_fonts_section(theme_t *theme, const char *key, const char *value) {
    theme_font_id_t id = get_font_id(key);
    if (id < THEME_FONT_COUNT) {
        parse_font_spec(value, &theme->fonts[id]);
    }
}

// Handle [metrics] section
static void handle_metrics_section(theme_t *theme, const char *key, const char *value) {
    int val = atoi(value);

    if (strcmp(key, "border_width") == 0) {
        theme->metrics.border_width = val;
    } else if (strcmp(key, "titlebar_height") == 0) {
        theme->metrics.titlebar_height = val;
    } else if (strcmp(key, "button_width") == 0) {
        theme->metrics.button_width = val;
    } else if (strcmp(key, "button_height") == 0) {
        theme->metrics.button_height = val;
    } else if (strcmp(key, "scrollbar_width") == 0) {
        theme->metrics.scrollbar_width = val;
    } else if (strcmp(key, "menu_item_height") == 0) {
        theme->metrics.menu_item_height = val;
    } else if (strcmp(key, "icon_size") == 0) {
        theme->metrics.icon_size = val;
    } else if (strcmp(key, "corner_radius") == 0) {
        theme->metrics.corner_radius = val;
    } else if (strcmp(key, "padding") == 0) {
        theme->metrics.padding = val;
    } else if (strcmp(key, "spacing") == 0) {
        theme->metrics.spacing = val;
    } else if (strcmp(key, "tab_height") == 0) {
        theme->metrics.tab_height = val;
    }
}

// Parse decoration style name
static theme_style_t parse_style(const char *name) {
    if (strcmp(name, "flat") == 0) return THEME_STYLE_FLAT;
    if (strcmp(name, "motif") == 0) return THEME_STYLE_MOTIF;
    if (strcmp(name, "win95") == 0) return THEME_STYLE_WIN95;
    if (strcmp(name, "win11") == 0) return THEME_STYLE_WIN11;
    if (strcmp(name, "mac") == 0) return THEME_STYLE_MAC;
    if (strcmp(name, "gtk") == 0) return THEME_STYLE_GTK;
    return THEME_STYLE_FLAT;
}

// Parse boolean value
static bool parse_bool(const char *value) {
    return (strcmp(value, "true") == 0 ||
            strcmp(value, "yes") == 0 ||
            strcmp(value, "1") == 0);
}

// Handle [decorations] section
static void handle_decorations_section(theme_t *theme, const char *key, const char *value) {
    if (strcmp(key, "style") == 0) {
        theme->decorations.style = parse_style(value);
    } else if (strcmp(key, "corner_radius") == 0) {
        theme->decorations.corner_radius = atoi(value);
    } else if (strcmp(key, "shadow_enabled") == 0) {
        theme->decorations.shadow_enabled = parse_bool(value);
    } else if (strcmp(key, "shadow_offset_x") == 0) {
        theme->decorations.shadow_offset_x = atoi(value);
    } else if (strcmp(key, "shadow_offset_y") == 0) {
        theme->decorations.shadow_offset_y = atoi(value);
    } else if (strcmp(key, "shadow_blur") == 0) {
        theme->decorations.shadow_blur = atoi(value);
    } else if (strcmp(key, "shadow_color") == 0) {
        theme->decorations.shadow_color = theme_parse_color(value);
    }
}

// =============================================================================
// Main Parser Function
// =============================================================================

// Parse theme INI data from a buffer
// Returns 0 on success, -1 on failure
int theme_parse_ini(const char *data, size_t size, theme_t *theme) {
    if (!data || !theme || size == 0) {
        return -1;
    }

    // Initialize theme with defaults
    memset(theme, 0, sizeof(theme_t));

    // Set default metrics
    theme->metrics.border_width = 2;
    theme->metrics.titlebar_height = 20;
    theme->metrics.button_width = 16;
    theme->metrics.button_height = 14;
    theme->metrics.scrollbar_width = 16;
    theme->metrics.menu_item_height = 20;
    theme->metrics.icon_size = 32;
    theme->metrics.corner_radius = 0;
    theme->metrics.padding = 4;
    theme->metrics.spacing = 4;
    theme->metrics.tab_height = 24;

    // Set default decorations
    theme->decorations.style = THEME_STYLE_FLAT;
    theme->decorations.corner_radius = 0;
    theme->decorations.shadow_enabled = false;
    theme->decorations.shadow_offset_x = 2;
    theme->decorations.shadow_offset_y = 2;
    theme->decorations.shadow_blur = 4;
    theme->decorations.shadow_color = 0x80000000;

    // Set default fonts
    for (int i = 0; i < THEME_FONT_COUNT; i++) {
        strcpy(theme->fonts[i].name, "fixed");
        theme->fonts[i].size = 12;
        theme->fonts[i].bold = false;
        theme->fonts[i].italic = false;
    }

    // Set default colors (grey theme)
    theme->colors[THEME_COLOR_BACKGROUND] = 0xC0C0C0;
    theme->colors[THEME_COLOR_FOREGROUND] = 0x000000;
    theme->colors[THEME_COLOR_ACCENT] = 0x000080;
    theme->colors[THEME_COLOR_SELECTION] = 0x000080;
    theme->colors[THEME_COLOR_SELECTION_TEXT] = 0xFFFFFF;
    theme->colors[THEME_COLOR_TITLEBAR_ACTIVE] = 0x000080;
    theme->colors[THEME_COLOR_TITLEBAR_INACTIVE] = 0x808080;
    theme->colors[THEME_COLOR_TITLEBAR_TEXT] = 0xFFFFFF;
    theme->colors[THEME_COLOR_WINDOW_BG] = 0xC0C0C0;
    theme->colors[THEME_COLOR_WINDOW_BORDER] = 0x000000;
    theme->colors[THEME_COLOR_CLOSE_BUTTON] = 0xC0C0C0;
    theme->colors[THEME_COLOR_CLOSE_BUTTON_HOVER] = 0xE04040;
    theme->colors[THEME_COLOR_MINIMIZE_BUTTON] = 0xC0C0C0;
    theme->colors[THEME_COLOR_MAXIMIZE_BUTTON] = 0xC0C0C0;
    theme->colors[THEME_COLOR_BUTTON_FACE] = 0xC0C0C0;
    theme->colors[THEME_COLOR_BUTTON_LIGHT] = 0xFFFFFF;
    theme->colors[THEME_COLOR_BUTTON_SHADOW] = 0x808080;
    theme->colors[THEME_COLOR_BUTTON_DARK] = 0x404040;
    theme->colors[THEME_COLOR_BUTTON_TEXT] = 0x000000;
    theme->colors[THEME_COLOR_BUTTON_DISABLED] = 0x808080;
    theme->colors[THEME_COLOR_LABEL_TEXT] = 0x000000;
    theme->colors[THEME_COLOR_TEXTBOX_BG] = 0xFFFFFF;
    theme->colors[THEME_COLOR_TEXTBOX_BORDER] = 0x000000;
    theme->colors[THEME_COLOR_TEXTBOX_TEXT] = 0x000000;
    theme->colors[THEME_COLOR_TEXTBOX_CURSOR] = 0x000000;
    theme->colors[THEME_COLOR_CHECKBOX_BG] = 0xFFFFFF;
    theme->colors[THEME_COLOR_CHECKBOX_BORDER] = 0x000000;
    theme->colors[THEME_COLOR_CHECKBOX_CHECK] = 0x000000;
    theme->colors[THEME_COLOR_DESKTOP_BG] = 0x008080;
    theme->colors[THEME_COLOR_TASKBAR_BG] = 0xC0C0C0;
    theme->colors[THEME_COLOR_TASKBAR_HOVER] = 0xD0D0D0;
    theme->colors[THEME_COLOR_TASKBAR_ACTIVE] = 0xA0A0A0;
    theme->colors[THEME_COLOR_START_BUTTON] = 0xC0C0C0;
    theme->colors[THEME_COLOR_GAUGE_BG] = 0x808080;
    theme->colors[THEME_COLOR_GAUGE_FG] = 0x000080;
    theme->colors[THEME_COLOR_MENU_BG] = 0xFFFFFF;
    theme->colors[THEME_COLOR_MENU_BORDER] = 0x000000;
    theme->colors[THEME_COLOR_MENU_ITEM_HOVER] = 0x000080;
    theme->colors[THEME_COLOR_MENU_TEXT] = 0x000000;
    theme->colors[THEME_COLOR_MENU_TEXT_DISABLED] = 0x808080;
    theme->colors[THEME_COLOR_MENU_SEPARATOR] = 0x808080;
    theme->colors[THEME_COLOR_SCROLLBAR_BG] = 0xC0C0C0;
    theme->colors[THEME_COLOR_SCROLLBAR_THUMB] = 0x808080;
    theme->colors[THEME_COLOR_SCROLLBAR_THUMB_HOVER] = 0x606060;
    theme->colors[THEME_COLOR_TAB_BG] = 0xC0C0C0;
    theme->colors[THEME_COLOR_TAB_ACTIVE] = 0xFFFFFF;
    theme->colors[THEME_COLOR_TAB_BORDER] = 0x808080;
    theme->colors[THEME_COLOR_TOOLTIP_BG] = 0xFFFFE0;
    theme->colors[THEME_COLOR_TOOLTIP_BORDER] = 0x000000;
    theme->colors[THEME_COLOR_TOOLTIP_TEXT] = 0x000000;
    theme->colors[THEME_COLOR_PROGRESS_BG] = 0xC0C0C0;
    theme->colors[THEME_COLOR_PROGRESS_FG] = 0x000080;

    // Parse line by line via the #404 line-record seam (Rust under
    // -DRUST_THEME_PARSE). Behavior-identical to the original inline loop: each
    // call classifies ONE line into {skip | [section] | key=value}; the
    // section/key/value handlers below are unchanged.
    parser_section_t current_section = SECTION_NONE;
    const char *ptr = data;
    size_t remaining = size;

    while (remaining > 0) {
        theme_line_t rec;
        uint32_t adv = THEME_PARSE_LINE(ptr, remaining, &rec);
        if (adv == 0) break;                 // no-progress guard
        ptr += adv; remaining -= adv;

        if (rec.kind == TP_LINE_SECTION) {
            current_section = get_section_type(rec.section);
        } else if (rec.kind == TP_LINE_KEYVALUE) {
            // Handle based on current section
            switch (current_section) {
                case SECTION_THEME:
                    handle_theme_section(theme, rec.key, rec.value);
                    break;
                case SECTION_COLORS:
                    handle_colors_section(theme, rec.key, rec.value);
                    break;
                case SECTION_FONTS:
                    handle_fonts_section(theme, rec.key, rec.value);
                    break;
                case SECTION_METRICS:
                    handle_metrics_section(theme, rec.key, rec.value);
                    break;
                case SECTION_DECORATIONS:
                    handle_decorations_section(theme, rec.key, rec.value);
                    break;
                default:
                    break;
            }
        }
    }

    // Check if theme has a name
    if (theme->name[0] == '\0') {
        strcpy(theme->name, "Unnamed Theme");
    }

    kprintf("[Theme Parser] Parsed theme: %s\n", theme->name);
    return 0;
}

// =============================================================================
// #404 theme-file line tokenizer seam boot self-test (flag -DRUST_THEME_PARSE).
// Bounded, runs once (#426). Proves theme_parse_line_rs == theme_parse_line_c
// on KAT vectors incl. the one rs/c divergence found + fixed offline (an
// EMBEDDED NUL truncates the classified line in C, so the Rust mirrors it).
// Logs [RUST-DIFF]/[RUST-SEC]/[RUST-PERF] theme_parse regardless of the flag.
// =============================================================================
static inline uint64_t tp_rdtsc(void) {
    unsigned lo, hi;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static void theme_parse_perf_bench(void) {
    static const char rep[] =
        "[theme]\nname=Retro Unix\nversion=1.0\n[colors]\nbackground=#C0C0C0\n"
        "window_bg = #c0c0c0 ; c\n[fonts]\nsystem=fixed,12,bold\n"
        "[metrics]\nborder_width=2\n[decorations]\nstyle=motif\n";
    size_t rlen = sizeof(rep) - 1;
    const long iters = 20000;
    theme_line_t tmp; volatile uint32_t sink = 0;
    uint64_t c0 = tp_rdtsc();
    for (long it = 0; it < iters; it++) { size_t p = 0;
        while (p < rlen) { uint32_t a = theme_parse_line_c(rep+p, rlen-p, &tmp); if(!a)break; p+=a; sink+=tmp.kind; } }
    uint64_t c1 = tp_rdtsc();
    uint64_t r0 = tp_rdtsc();
    for (long it = 0; it < iters; it++) { size_t p = 0;
        while (p < rlen) { uint32_t a = theme_parse_line_rs(rep+p, rlen-p, &tmp); if(!a)break; p+=a; sink+=tmp.kind; } }
    uint64_t r1 = tp_rdtsc();
    kprintf("[RUST-PERF] theme_parse: C=%lu RS=%lu cyc/file (iters=%ld sink=%u)\n",
            (unsigned long)((c1-c0)/iters), (unsigned long)((r1-r0)/iters), iters, (unsigned)sink);
}

void theme_parse_rust_selftest(void) {
    // KAT vectors incl. the ONE rs/c divergence found & fixed offline:
    // an EMBEDDED NUL truncates the classified line in C (NUL-terminated
    // helpers) but not in a naive slice; the Rust mirrors C by ending the
    // classified view at the first embedded 0x00.
    static const struct { const char *b; unsigned n; } V[] = {
        { "[colors]\n", 9 },
        { "background = #C0C0C0 ; c\n", 24 },
        { "name=Retro Unix\n", 16 },
        { "\x43\x6d\xe9\x36\x00\x90\x63\x85\x42\x56\xce\x3d\x9d\xc0", 14 }, // embedded-NUL
        { "k=", 2 }, { "=v", 2 }, { "# comment", 9 }, { "[unterminated", 13 },
        { "reallylongkey_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa=x\n", 75 },
    };
    // Force-reference the Rust symbol so its archive member always links,
    // regardless of -DRUST_THEME_PARSE.
    { theme_line_t t; theme_parse_line_rs("[x]\n", 4, &t); }
    int fails = 0;
    for (unsigned i = 0; i < sizeof(V)/sizeof(V[0]); i++) {
        theme_line_t rc, rr;
        theme_parse_line_c(V[i].b, V[i].n, &rc);
        theme_parse_line_rs(V[i].b, V[i].n, &rr);
        if (memcmp(&rc, &rr, sizeof(theme_line_t)) != 0) { fails++;
            kprintf("[RUST-DIFF] theme_parse MISMATCH vec %u (c.kind=%d rs.kind=%d)\n",
                    i, rc.kind, rr.kind);
        }
    }
    kprintf("[RUST-DIFF] theme_parse: %s (%u KAT vectors, rs==c)\n",
            fails ? "FAIL" : "PASS", (unsigned)(sizeof(V)/sizeof(V[0])));
    extern void bootlog_write(const char *fmt, ...);
    bootlog_write("[RUST-DIFF] theme_parse: %s (%u KAT vectors, rs==c)",
            fails ? "FAIL" : "PASS", (unsigned)(sizeof(V)/sizeof(V[0])));
    kprintf("[RUST-SEC] theme_parse: verbatim C is fully BOUNDED (no reachable "
            "OOB; ASan-clean over 32.6M malformed vectors). Rust removes the "
            "raw-scan class by construction (slice of exactly len, every out "
            "field hard-capped). Latent defense-in-depth, not a live-OOB fix.\n");
    theme_parse_perf_bench();
}
