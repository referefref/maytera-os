#pragma GCC diagnostic ignored "-Wunused-function"
// themes.c - Color theme system implementation for MayteraOS GUI
//
// #565: this used to hold 12 compiled-in theme_t literals. That is gone: the
// palette DATA now lives as files (/THEMES/*.mtheme, one theme per file,
// simple key=value text) and this file is just the loader + the live table
// every syscall (SYS_GET_THEME / SYS_SET_THEME / SYS_THEME_COLOR) reads. The
// userland loader (userland/libc/gui_theme.c) is what actually decides which
// theme is active (it owns /CONFIG/THEME.CFG and drives Settings + the App
// Store "type=theme" install path); this file's job is to parse whatever
// file it is pointed at and make the result visible system-wide with no
// per-app changes, exactly like the 12-compiled-in version did.
//
// Exactly ONE minimal palette (g_fallback_theme, below) stays hardcoded in C.
// It is used only if /THEMES/INDEX.TXT is missing or unreadable (e.g. before
// the root filesystem is mounted, or a golden that shipped without /THEMES),
// so the login screen and the kernel's own fallback desktop draw never see
// an uninitialized/black theme. Every other palette (the 11 other built-ins
// plus anything the App Store installs) is data, not code.
#include "themes.h"
#include "uiscale.h"
#include "font.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"
#include "../mm/heap.h"

extern fat_fs_t g_fat_fs;

#define THEME_INDEX_FILE   "/THEMES/INDEX.TXT"
#define THEME_DIR_PREFIX   "/THEMES/"

// Current theme ID
static int g_current_theme = THEME_DEFAULT;
static bool g_themes_initialized = false;

// Live theme table. Populated by theme_init() from /THEMES/*.mtheme (see
// THEME_INDEX_FILE) rather than compiled in; theme_load_file_runtime() can
// append/update a slot at runtime (used by the App Store install path).
static theme_t g_themes[MAX_THEMES];
static int g_theme_count = 0;


// Maps an .mtheme file key (color field name) to its offset in theme_t so
// the parser below can set fields generically instead of a 51-way switch.
typedef struct { const char *key; unsigned long offset; } theme_field_t;

// ---------------------------------------------------------------------------
// The INT table's rows carry one extra bit: is this metric a COUNT OF PIXELS,
// or is it an ENUM/BOOLEAN that merely happens to be stored as an int?
//
// This exists because of the global UI scale factor (rustkern/uiscale.rs).
// theme_get_metric_by_id() below is the ONE function every integer metric read
// in the kernel AND every SYS_THEME_METRIC from Ring 3 passes through, so it is
// where the scale multiply belongs: every widget that was already theme-wired
// scales for free, with no call-site edit anywhere. But `decor.style=gradient`
// is the value 2 meaning GRADIENT, and `type.body_weight=bold` is the value 1
// meaning BOLD. Multiplying either by 1.5 turns a gradient titlebar into
// nothing and bold text into nothing, because 3 and 2 are not valid values of
// those enums.
//
// THE FLAG LIVES IN THE ROW, NOT IN A PARALLEL ARRAY, DELIBERATELY. A parallel
// bitmask or a second array is a second thing to keep in step with this one,
// and this file already carries three separate warnings about what happens
// when an index-linked structure drifts (see the _Static_assert below and the
// DEF_C comment). A field in the row cannot drift from the row.
//
// A NEW ROW MUST CHOOSE. TM_PX is the common case and the safe default for a
// geometry metric; TM_ENUM is for anything whose value is a NAME rather than a
// LENGTH (that is exactly the set theme_int_parse() accepts words for).
// ---------------------------------------------------------------------------
#define TM_ENUM 0   // a name stored as an int: never scaled
#define TM_PX   1   // a count of pixels: scaled by the global UI scale factor
typedef struct { const char *key; unsigned long offset; unsigned char kind; } theme_int_field_t;
static const theme_field_t g_theme_fields[] = {
    { "titlebar_active", offsetof(theme_t, titlebar_active) },
    { "titlebar_inactive", offsetof(theme_t, titlebar_inactive) },
    { "titlebar_text", offsetof(theme_t, titlebar_text) },
    { "window_bg", offsetof(theme_t, window_bg) },
    { "window_border", offsetof(theme_t, window_border) },
    { "close_button", offsetof(theme_t, close_button) },
    { "close_button_hover", offsetof(theme_t, close_button_hover) },
    { "minimize_button", offsetof(theme_t, minimize_button) },
    { "maximize_button", offsetof(theme_t, maximize_button) },
    { "button_bg", offsetof(theme_t, button_bg) },
    { "button_bg_hover", offsetof(theme_t, button_bg_hover) },
    { "button_bg_pressed", offsetof(theme_t, button_bg_pressed) },
    { "button_border", offsetof(theme_t, button_border) },
    { "button_text", offsetof(theme_t, button_text) },
    { "button_disabled", offsetof(theme_t, button_disabled) },
    { "label_text", offsetof(theme_t, label_text) },
    { "textbox_bg", offsetof(theme_t, textbox_bg) },
    { "textbox_border", offsetof(theme_t, textbox_border) },
    { "textbox_text", offsetof(theme_t, textbox_text) },
    { "textbox_cursor", offsetof(theme_t, textbox_cursor) },
    { "checkbox_bg", offsetof(theme_t, checkbox_bg) },
    { "checkbox_border", offsetof(theme_t, checkbox_border) },
    { "checkbox_check", offsetof(theme_t, checkbox_check) },
    { "desktop_bg", offsetof(theme_t, desktop_bg) },
    { "taskbar_bg", offsetof(theme_t, taskbar_bg) },
    { "taskbar_hover", offsetof(theme_t, taskbar_hover) },
    { "taskbar_active", offsetof(theme_t, taskbar_active) },
    { "taskbar_text", offsetof(theme_t, taskbar_text) },                     /* #745 */
    { "taskbar_text_muted", offsetof(theme_t, taskbar_text_muted) },         /* #745 */
    { "taskbar_selected_text", offsetof(theme_t, taskbar_selected_text) },   /* #745 */
    { "start_button", offsetof(theme_t, start_button) },
    { "gauge_bg", offsetof(theme_t, gauge_bg) },
    { "gauge_fg", offsetof(theme_t, gauge_fg) },
    { "menu_bg", offsetof(theme_t, menu_bg) },
    { "menu_border", offsetof(theme_t, menu_border) },
    { "menu_item_hover", offsetof(theme_t, menu_item_hover) },
    { "menu_text", offsetof(theme_t, menu_text) },
    { "menu_text_disabled", offsetof(theme_t, menu_text_disabled) },
    { "menu_separator", offsetof(theme_t, menu_separator) },
    { "scrollbar_bg", offsetof(theme_t, scrollbar_bg) },
    { "scrollbar_thumb", offsetof(theme_t, scrollbar_thumb) },
    { "scrollbar_thumb_hover", offsetof(theme_t, scrollbar_thumb_hover) },
    { "selection_bg", offsetof(theme_t, selection_bg) },
    { "selection_text", offsetof(theme_t, selection_text) },
    { "color_error", offsetof(theme_t, color_error) },
    { "color_warning", offsetof(theme_t, color_warning) },
    { "color_success", offsetof(theme_t, color_success) },
    { "color_info", offsetof(theme_t, color_info) },
    { "link_color", offsetof(theme_t, link_color) },
    { "link_visited", offsetof(theme_t, link_visited) },
    { "link_hover", offsetof(theme_t, link_hover) },
    { "tooltip_bg", offsetof(theme_t, tooltip_bg) },
    { "tooltip_text", offsetof(theme_t, tooltip_text) },
    { "tooltip_border", offsetof(theme_t, tooltip_border) },
};
#define THEME_FIELD_COUNT (sizeof(g_theme_fields) / sizeof(g_theme_fields[0]))

// ============================================================================
// mtheme v2 (#711): two more generic key->offset tables, driven by the same
// dumb line reader. Colours parse as hex, everything else as a bounded
// decimal (or one of a tiny fixed word set). NOTHING here is a token
// reference: a .mtheme value is always a literal, so the Ring 0 parser never
// resolves, recurses or allocates. That is deliberate; see docs/UI_STYLE_GUIDE.md.
// ============================================================================
static const theme_field_t g_theme_v2_color_fields[] = {
    { "color.surface_sunken", offsetof(theme_t, c_surface_sunken) },
    { "color.surface", offsetof(theme_t, c_surface) },
    { "color.surface_raised", offsetof(theme_t, c_surface_raised) },
    { "color.surface_overlay", offsetof(theme_t, c_surface_overlay) },
    { "color.on_surface", offsetof(theme_t, c_on_surface) },
    { "color.on_surface_muted", offsetof(theme_t, c_on_surface_muted) },
    { "color.on_surface_disabled", offsetof(theme_t, c_on_surface_disabled) },
    { "color.accent", offsetof(theme_t, c_accent) },
    { "color.on_accent", offsetof(theme_t, c_on_accent) },
    { "color.accent_hover", offsetof(theme_t, c_accent_hover) },
    { "color.accent_active", offsetof(theme_t, c_accent_active) },
    { "color.danger", offsetof(theme_t, c_danger) },
    { "color.on_danger", offsetof(theme_t, c_on_danger) },
    { "color.border_subtle", offsetof(theme_t, c_border_subtle) },
    { "color.border_strong", offsetof(theme_t, c_border_strong) },
    { "color.focus_ring", offsetof(theme_t, c_focus_ring) },
    { "color.sel_bg", offsetof(theme_t, c_sel_bg) },
    { "color.sel_fg", offsetof(theme_t, c_sel_fg) },
    { "color.titlebar_top", offsetof(theme_t, c_titlebar_top) },
    { "color.titlebar_bottom", offsetof(theme_t, c_titlebar_bottom) },
    { "color.titlebar_inactive_top", offsetof(theme_t, c_titlebar_inactive_top) },
    { "color.titlebar_inactive_bottom", offsetof(theme_t, c_titlebar_inactive_bottom) },
    { "color.titlebar_text", offsetof(theme_t, c_titlebar_text) },
    { "color.titlebar_text_inactive", offsetof(theme_t, c_titlebar_text_inactive) },
    { "state.btn_rest_bg", offsetof(theme_t, s_btn_rest_bg) },
    { "state.btn_rest_fg", offsetof(theme_t, s_btn_rest_fg) },
    { "state.btn_hover_bg", offsetof(theme_t, s_btn_hover_bg) },
    { "state.btn_hover_fg", offsetof(theme_t, s_btn_hover_fg) },
    { "state.btn_active_bg", offsetof(theme_t, s_btn_active_bg) },
    { "state.btn_active_fg", offsetof(theme_t, s_btn_active_fg) },
    { "state.btn_focus_ring", offsetof(theme_t, s_btn_focus_ring) },
    { "state.btn_disabled_bg", offsetof(theme_t, s_btn_disabled_bg) },
    { "state.btn_disabled_fg", offsetof(theme_t, s_btn_disabled_fg) },
    { "state.btn_selected_bg", offsetof(theme_t, s_btn_selected_bg) },
    { "state.btn_selected_fg", offsetof(theme_t, s_btn_selected_fg) },
    { "state.item_rest_bg", offsetof(theme_t, s_item_rest_bg) },
    { "state.item_rest_fg", offsetof(theme_t, s_item_rest_fg) },
    { "state.item_hover_bg", offsetof(theme_t, s_item_hover_bg) },
    { "state.item_hover_fg", offsetof(theme_t, s_item_hover_fg) },
    { "state.item_active_bg", offsetof(theme_t, s_item_active_bg) },
    { "state.item_active_fg", offsetof(theme_t, s_item_active_fg) },
    { "state.item_focus_ring", offsetof(theme_t, s_item_focus_ring) },
    { "state.item_disabled_bg", offsetof(theme_t, s_item_disabled_bg) },
    { "state.item_disabled_fg", offsetof(theme_t, s_item_disabled_fg) },
    { "state.item_selected_bg", offsetof(theme_t, s_item_selected_bg) },
    { "state.item_selected_fg", offsetof(theme_t, s_item_selected_fg) },
    { "state.input_rest_bg", offsetof(theme_t, s_input_rest_bg) },
    { "state.input_rest_fg", offsetof(theme_t, s_input_rest_fg) },
    { "state.input_rest_border", offsetof(theme_t, s_input_rest_border) },
    { "state.input_hover_border", offsetof(theme_t, s_input_hover_border) },
    { "state.input_active_border", offsetof(theme_t, s_input_active_border) },
    { "state.input_focus_ring", offsetof(theme_t, s_input_focus_ring) },
    { "state.input_disabled_bg", offsetof(theme_t, s_input_disabled_bg) },
    { "state.input_disabled_fg", offsetof(theme_t, s_input_disabled_fg) },
    { "state.input_selected_bg", offsetof(theme_t, s_input_selected_bg) },
    { "state.input_selected_fg", offsetof(theme_t, s_input_selected_fg) },
    // #711 loop 2 (designer 1, window decorations): APPENDED at the end so
    // every existing DEF_C(...,idx,...) index above keeps meaning array
    // position; do not insert these earlier in the array without also
    // renumbering every DEF_C call below.
    { "color.titlebar_btn_hover", offsetof(theme_t, c_titlebar_btn_hover) },
    { "color.titlebar_btn_active", offsetof(theme_t, c_titlebar_btn_active) },
    { "color.titlebar_close_active", offsetof(theme_t, c_titlebar_close_active) },
};
#define THEME_V2C_COUNT (sizeof(g_theme_v2_color_fields) / sizeof(g_theme_v2_color_fields[0]))

// Integer fields. The array index IS the theme_metric_v2_t id, so
// theme_get_metric_by_id() is a bounds check plus one load: no switch that
// could drift out of step with the enum. The _Static_assert locks the count.
static const theme_int_field_t g_theme_int_fields[] = {
    /* TM_TITLEBAR_H              */ { "metric.titlebar_h", offsetof(theme_t, m_titlebar_h), TM_PX },
    /* TM_BORDER_W                */ { "metric.border_w", offsetof(theme_t, m_border_w), TM_PX },
    /* TM_BTN_H                   */ { "metric.btn_h", offsetof(theme_t, m_btn_h), TM_PX },
    /* TM_INPUT_H                 */ { "metric.input_h", offsetof(theme_t, m_input_h), TM_PX },
    /* TM_PAD                     */ { "metric.pad", offsetof(theme_t, m_pad), TM_PX },
    /* TM_GAP                     */ { "metric.gap", offsetof(theme_t, m_gap), TM_PX },
    /* TM_FOCUS_W                 */ { "metric.focus_w", offsetof(theme_t, m_focus_w), TM_PX },
    /* TM_WINMENU_ROWW            */ { "metric.winmenu_roww", offsetof(theme_t, m_winmenu_roww), TM_PX },
    /* TM_WINMENU_ROWH            */ { "metric.winmenu_rowh", offsetof(theme_t, m_winmenu_rowh), TM_PX },
    /* TM_WINMENU_HDR             */ { "metric.winmenu_hdr", offsetof(theme_t, m_winmenu_hdr), TM_PX },
    /* TM_TITLEBAR_BTN            */ { "metric.titlebar_btn", offsetof(theme_t, m_titlebar_btn), TM_PX },
    /* TM_TITLEBAR_BTN_GAP        */ { "metric.titlebar_btn_gap", offsetof(theme_t, m_titlebar_btn_gap), TM_PX },
    /* TM_GRIP                    */ { "metric.grip", offsetof(theme_t, m_grip), TM_PX },
    /* TM_SCROLLBAR_W             */ { "metric.scrollbar_w", offsetof(theme_t, m_scrollbar_w), TM_PX },
    /* TM_MENU_ROW_H              */ { "metric.menu_row_h", offsetof(theme_t, m_menu_row_h), TM_PX },
    /* TM_RADIUS_BTN              */ { "radius.btn", offsetof(theme_t, r_btn), TM_PX },
    /* TM_RADIUS_INPUT            */ { "radius.input", offsetof(theme_t, r_input), TM_PX },
    /* TM_RADIUS_MENU             */ { "radius.menu", offsetof(theme_t, r_menu), TM_PX },
    /* TM_RADIUS_CARD             */ { "radius.card", offsetof(theme_t, r_card), TM_PX },
    /* TM_DECOR_STYLE             */ { "decor.style", offsetof(theme_t, d_style), TM_ENUM },
    /* TM_DECOR_TITLEBAR_GRADIENT */ { "decor.titlebar_gradient", offsetof(theme_t, d_titlebar_gradient), TM_ENUM },
    /* TM_DECOR_GRIP              */ { "decor.grip", offsetof(theme_t, d_grip), TM_ENUM },
    /* TM_TYPE_CAPTION            */ { "type.caption", offsetof(theme_t, t_caption), TM_PX },
    /* TM_TYPE_CAPTION_LH         */ { "type.caption_lineheight", offsetof(theme_t, t_caption_lh), TM_PX },
    /* TM_TYPE_CAPTION_W          */ { "type.caption_weight", offsetof(theme_t, t_caption_w), TM_ENUM },
    /* TM_TYPE_BODY               */ { "type.body", offsetof(theme_t, t_body), TM_PX },
    /* TM_TYPE_BODY_LH            */ { "type.body_lineheight", offsetof(theme_t, t_body_lh), TM_PX },
    /* TM_TYPE_BODY_W             */ { "type.body_weight", offsetof(theme_t, t_body_w), TM_ENUM },
    /* TM_TYPE_TITLE              */ { "type.title", offsetof(theme_t, t_title), TM_PX },
    /* TM_TYPE_TITLE_LH           */ { "type.title_lineheight", offsetof(theme_t, t_title_lh), TM_PX },
    /* TM_TYPE_TITLE_W            */ { "type.title_weight", offsetof(theme_t, t_title_w), TM_ENUM },
    /* TM_TYPE_HEADING            */ { "type.heading", offsetof(theme_t, t_heading), TM_PX },
    /* TM_TYPE_HEADING_LH         */ { "type.heading_lineheight", offsetof(theme_t, t_heading_lh), TM_PX },
    /* TM_TYPE_HEADING_W          */ { "type.heading_weight", offsetof(theme_t, t_heading_w), TM_ENUM },
    /* TM_TYPE_DISPLAY            */ { "type.display", offsetof(theme_t, t_display), TM_PX },
    /* TM_TYPE_DISPLAY_LH         */ { "type.display_lineheight", offsetof(theme_t, t_display_lh), TM_PX },
    /* TM_TYPE_DISPLAY_W          */ { "type.display_weight", offsetof(theme_t, t_display_w), TM_ENUM },
    /* TM_TITLE_INSET             */ { "metric.title_inset", offsetof(theme_t, m_title_inset), TM_PX },
    /* TM_RADIUS_WINDOW           */ { "radius.window", offsetof(theme_t, r_window), TM_PX },
};
#define THEME_INT_COUNT (sizeof(g_theme_int_fields) / sizeof(g_theme_int_fields[0]))
_Static_assert(THEME_INT_COUNT == (size_t)TM_COUNT,
               "g_theme_int_fields[] is indexed BY theme_metric_v2_t: a new id needs a new row in the SAME position");

// Bounded decimal / fixed-word value parser for the integer fields. Accepts a
// signed decimal, or one of a tiny closed word set (so a designer may write
// decor.style=gradient instead of 2). Anything else yields 0.
static int32_t theme_int_parse(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if ((*s >= '0' && *s <= '9') || *s == '-' || *s == '+') {
        int neg = (*s == '-');
        if (*s == '-' || *s == '+') s++;
        int32_t v = 0;
        int n = 0;
        while (*s >= '0' && *s <= '9' && n < 9) { v = v * 10 + (*s - '0'); s++; n++; }
        return neg ? -v : v;
    }
    if (strcmp(s, "beveled") == 0)  return TDECOR_BEVELED;
    if (strcmp(s, "flat") == 0)     return TDECOR_FLAT;
    if (strcmp(s, "gradient") == 0) return TDECOR_GRADIENT;
    if (strcmp(s, "regular") == 0)  return 0;
    if (strcmp(s, "bold") == 0)     return 1;
    if (strcmp(s, "none") == 0)     return 0;
    if (strcmp(s, "yes") == 0 || strcmp(s, "on") == 0 || strcmp(s, "true") == 0) return 1;
    if (strcmp(s, "no") == 0 || strcmp(s, "off") == 0 || strcmp(s, "false") == 0) return 0;
    return 0;
}

// Fill every v2 field the file did NOT set. This is what makes mtheme v2 a
// strict SUPERSET of the #565 format: an untouched pre-v2 theme file (or a
// third-party one from the App Store) still yields a complete, self-consistent
// v2 theme, derived from the 51 legacy colours plus its style= line, so it
// keeps rendering exactly as it does today. Missing key = compiled fallback,
// and the fallback is DERIVED, not invented, wherever a legacy colour already
// carries the same meaning.
static void theme_fill_v2_defaults(theme_t *t, const uint8_t *seen_c, const uint8_t *seen_i) {
    #define DEF_C(field, idx, expr) do { if (!seen_c[idx]) t->field = (expr); } while (0)
    #define DEF_I(field, idx, expr) do { if (!seen_i[idx]) t->field = (expr); } while (0)
    int retro = (t->style == THEME_STYLE_RETRO);

    DEF_C(c_surface_sunken, 0, t->textbox_bg);
    DEF_C(c_surface, 1, t->window_bg);
    DEF_C(c_surface_raised, 2, t->button_bg);
    DEF_C(c_surface_overlay, 3, t->menu_bg);
    DEF_C(c_on_surface, 4, t->label_text);
    DEF_C(c_on_surface_muted, 5, t->menu_text_disabled);
    DEF_C(c_on_surface_disabled, 6, t->button_disabled);
    DEF_C(c_accent, 7, t->selection_bg);
    DEF_C(c_on_accent, 8, t->selection_text);
    DEF_C(c_accent_hover, 9, t->selection_bg);
    DEF_C(c_accent_active, 10, t->selection_bg);
    DEF_C(c_danger, 11, t->color_error);
    DEF_C(c_on_danger, 12, 0x00FFFFFF);
    DEF_C(c_border_subtle, 13, t->menu_separator);
    DEF_C(c_border_strong, 14, t->window_border);
    DEF_C(c_focus_ring, 15, t->selection_bg);
    DEF_C(c_sel_bg, 16, t->selection_bg);
    DEF_C(c_sel_fg, 17, t->selection_text);
    DEF_C(c_titlebar_top, 18, t->titlebar_active);
    DEF_C(c_titlebar_bottom, 19, t->titlebar_active);
    DEF_C(c_titlebar_inactive_top, 20, t->titlebar_inactive);
    DEF_C(c_titlebar_inactive_bottom, 21, t->titlebar_inactive);
    DEF_C(c_titlebar_text, 22, t->titlebar_text);
    DEF_C(c_titlebar_text_inactive, 23, t->titlebar_text);

    DEF_C(s_btn_rest_bg, 24, t->button_bg);
    DEF_C(s_btn_rest_fg, 25, t->button_text);
    DEF_C(s_btn_hover_bg, 26, t->button_bg_hover);
    DEF_C(s_btn_hover_fg, 27, t->button_text);
    DEF_C(s_btn_active_bg, 28, t->button_bg_pressed);
    DEF_C(s_btn_active_fg, 29, t->button_text);
    DEF_C(s_btn_focus_ring, 30, t->c_focus_ring);
    DEF_C(s_btn_disabled_bg, 31, t->button_bg);
    DEF_C(s_btn_disabled_fg, 32, t->button_disabled);
    DEF_C(s_btn_selected_bg, 33, t->selection_bg);
    DEF_C(s_btn_selected_fg, 34, t->selection_text);
    DEF_C(s_item_rest_bg, 35, t->menu_bg);
    DEF_C(s_item_rest_fg, 36, t->menu_text);
    DEF_C(s_item_hover_bg, 37, t->menu_item_hover);
    DEF_C(s_item_hover_fg, 38, t->menu_text);
    DEF_C(s_item_active_bg, 39, t->selection_bg);
    DEF_C(s_item_active_fg, 40, t->selection_text);
    DEF_C(s_item_focus_ring, 41, t->c_focus_ring);
    DEF_C(s_item_disabled_bg, 42, t->menu_bg);
    DEF_C(s_item_disabled_fg, 43, t->menu_text_disabled);
    DEF_C(s_item_selected_bg, 44, t->selection_bg);
    DEF_C(s_item_selected_fg, 45, t->selection_text);
    DEF_C(s_input_rest_bg, 46, t->textbox_bg);
    DEF_C(s_input_rest_fg, 47, t->textbox_text);
    DEF_C(s_input_rest_border, 48, t->textbox_border);
    DEF_C(s_input_hover_border, 49, t->textbox_border);
    DEF_C(s_input_active_border, 50, t->c_focus_ring);
    DEF_C(s_input_focus_ring, 51, t->c_focus_ring);
    DEF_C(s_input_disabled_bg, 52, t->textbox_bg);
    DEF_C(s_input_disabled_fg, 53, t->button_disabled);
    DEF_C(s_input_selected_bg, 54, t->selection_bg);
    DEF_C(s_input_selected_fg, 55, t->selection_text);

    // #711 loop 2 (designer 1, window decorations): the 3 titlebar-button
    // state colours staged in loop 1. Indices 56-58 are the ARRAY POSITION
    // of the matching g_theme_v2_color_fields[] rows above (appended at the
    // end), not a free choice. Defaults are DERIVED from the closest existing
    // legacy meaning, same rule the rest of this function follows: a theme
    // file that says nothing about these three keys renders identically to
    // before this loop (min/max/filter buttons stay flat, close stays flat).
    DEF_C(c_titlebar_btn_hover, 56, t->button_bg_hover);
    DEF_C(c_titlebar_btn_active, 57, t->button_bg_pressed);
    DEF_C(c_titlebar_close_active, 58, t->close_button_hover);

    // Metric defaults are exactly the values the kernel HAD compiled in before
    // #711 (TITLEBAR_HEIGHT 20, BORDER_WIDTH 2, CLOSE_BUTTON_SIZE 16, ...), so
    // a theme file that says nothing about geometry renders pixel-identically
    // to the pre-#711 build. That is the compatibility contract.
    DEF_I(m_titlebar_h, TM_TITLEBAR_H, 20);
    DEF_I(m_border_w, TM_BORDER_W, 2);
    DEF_I(m_btn_h, TM_BTN_H, 24);
    DEF_I(m_input_h, TM_INPUT_H, 24);
    DEF_I(m_pad, TM_PAD, 10);
    DEF_I(m_gap, TM_GAP, 8);
    DEF_I(m_focus_w, TM_FOCUS_W, 2);
    DEF_I(m_winmenu_roww, TM_WINMENU_ROWW, 210);
    DEF_I(m_winmenu_rowh, TM_WINMENU_ROWH, 26);
    DEF_I(m_winmenu_hdr, TM_WINMENU_HDR, 22);
    DEF_I(m_titlebar_btn, TM_TITLEBAR_BTN, 16);
    DEF_I(m_titlebar_btn_gap, TM_TITLEBAR_BTN_GAP, 2);
    // #711 loop 2: 4 reproduces window.c's pre-loop-2 hardcoded
    // "title_x = x + BORDER_WIDTH + 4" exactly, so an unset theme is pixel-
    // identical to before.
    DEF_I(m_title_inset, TM_TITLE_INSET, 4);
    DEF_I(m_grip, TM_GRIP, 10);
    DEF_I(m_scrollbar_w, TM_SCROLLBAR_W, 16);
    DEF_I(m_menu_row_h, TM_MENU_ROW_H, 22);
    DEF_I(r_btn, TM_RADIUS_BTN, retro ? 0 : 6);
    DEF_I(r_input, TM_RADIUS_INPUT, retro ? 0 : 6);
    DEF_I(r_menu, TM_RADIUS_MENU, retro ? 0 : 6);
    DEF_I(r_card, TM_RADIUS_CARD, retro ? 0 : 10);
    // #27: square corners for a retro-styled theme (retro_unix, Classic,
    // High Contrast are all style=retro), a 4px chamfer for everything else.
    // Every shipped theme also states radius.window explicitly, so this
    // default only ever applies to a hand-written or App Store theme file.
    DEF_I(r_window, TM_RADIUS_WINDOW, retro ? 0 : 4);
    DEF_I(d_style, TM_DECOR_STYLE, retro ? TDECOR_BEVELED : TDECOR_GRADIENT);
    // Pre-#711 this decision was a case-insensitive substring match on the
    // theme NAME ("classic"/"retro"/"cde"/"motif") in window.c. The default
    // below reproduces it from the style= line instead, which is what the name
    // match was always approximating, and a theme can now just say so.
    DEF_I(d_titlebar_gradient, TM_DECOR_TITLEBAR_GRADIENT, retro ? 0 : 1);
    DEF_I(d_grip, TM_DECOR_GRIP, 1);
    DEF_I(t_caption, TM_TYPE_CAPTION, 11);
    DEF_I(t_body, TM_TYPE_BODY, 14);
    DEF_I(t_title, TM_TYPE_TITLE, 16);
    DEF_I(t_heading, TM_TYPE_HEADING, 20);
    DEF_I(t_display, TM_TYPE_DISPLAY, 28);
    DEF_I(t_caption_lh, TM_TYPE_CAPTION_LH, (t->t_caption * 14 + 5) / 10);
    DEF_I(t_body_lh, TM_TYPE_BODY_LH, (t->t_body * 14 + 5) / 10);
    DEF_I(t_title_lh, TM_TYPE_TITLE_LH, (t->t_title * 14 + 5) / 10);
    DEF_I(t_heading_lh, TM_TYPE_HEADING_LH, (t->t_heading * 14 + 5) / 10);
    DEF_I(t_display_lh, TM_TYPE_DISPLAY_LH, (t->t_display * 14 + 5) / 10);
    DEF_I(t_caption_w, TM_TYPE_CAPTION_W, 0);
    DEF_I(t_body_w, TM_TYPE_BODY_W, 0);
    DEF_I(t_title_w, TM_TYPE_TITLE_W, 0);
    DEF_I(t_heading_w, TM_TYPE_HEADING_W, 1);
    DEF_I(t_display_w, TM_TYPE_DISPLAY_W, 1);
    #undef DEF_C
    #undef DEF_I
}

// mtheme v2 integer read. Bounds-checked; an unknown id returns 0 so a newer
// userland asking an older kernel degrades to its own default (see themes.h).
int32_t theme_get_metric_by_id(int theme_id, int metric_id) {
    int32_t raw = theme_get_metric_raw(theme_id, metric_id);
    if (metric_id < 0 || metric_id >= (int)THEME_INT_COUNT) return 0;
    if (g_theme_int_fields[metric_id].kind != TM_PX) return raw;
    // THE GLOBAL UI SCALE FACTOR IS APPLIED HERE AND ONLY HERE, for the whole
    // theme system, kernel and Ring 3 alike. See rustkern/uiscale.rs for why
    // the theme file keeps its 1x design values and the multiply happens at
    // read time (short version: a theme is per-LOOK, scale is per-DISPLAY, and
    // build/assets/theme-scale-lint.sh enforces the 1x design scales that
    // multiplying the stored values would break for all 14 shipped themes).
    //
    // A ZERO IS NEVER MANUFACTURED HERE. ui_px() never turns a nonzero into a
    // zero, which matters more than it looks: this function's contract is that
    // 0 means "this kernel does not know that id", and theme_metric_or() in
    // Ring 3 substitutes the caller's own fallback on a 0. A scale that could
    // round a 1px focus ring down to 0 would not draw a thinner ring, it would
    // silently hand every caller its hardcoded default and un-theme the UI.
    // A raw 0 (a legitimately zero radius, say) still returns 0, unchanged.
    if (raw == 0) return 0;
    return ui_px(raw);
}

// The UNSCALED value, for the handful of callers that genuinely want the
// theme's own 1x design number rather than what to draw: the theme-preview
// thumbnail in Settings (which draws a miniature window inside a fixed box and
// would overflow it), and the scale lint's own reasoning about a theme file.
// Everything that DRAWS wants theme_get_metric_by_id(), not this.
int32_t theme_get_metric_raw(int theme_id, int metric_id) {
    const theme_t *t = (theme_id < 0) ? theme_get_current() : theme_get_by_id(theme_id);
    if (!t) t = theme_get_current();
    if (metric_id < 0 || metric_id >= (int)THEME_INT_COUNT) return 0;
    return *(const int32_t *)((const char *)t + g_theme_int_fields[metric_id].offset);
}

// The single hardcoded fallback (#565) - see file header comment.
static const theme_t g_fallback_theme = {
    .name = "Retro UNIX",
    .style = THEME_STYLE_RETRO,
    .titlebar_active = 0x00336666,
    .titlebar_inactive = 0x00808080,
    .titlebar_text = 0x00ffffff,
    .window_bg = 0x00b4b4b4,
    .window_border = 0x00404040,
    .close_button = 0x00c8c8c8,
    .close_button_hover = 0x00d8d8d8,
    .minimize_button = 0x00c8c8c8,
    .maximize_button = 0x00c8c8c8,
    .button_bg = 0x00c8c8c8,
    .button_bg_hover = 0x00d4d4d4,
    .button_bg_pressed = 0x00a8a8a8,
    .button_border = 0x00404040,
    .button_text = 0x00000000,
    .button_disabled = 0x00808080,
    .label_text = 0x00000000,
    .textbox_bg = 0x00ffffff,
    .textbox_border = 0x00404040,
    .textbox_text = 0x00000000,
    .textbox_cursor = 0x00000000,
    .checkbox_bg = 0x00ffffff,
    .checkbox_border = 0x00404040,
    .checkbox_check = 0x00000000,
    .desktop_bg = 0x00336666,
    .taskbar_bg = 0x00b4b4b4,
    .taskbar_hover = 0x00c8c8c8,
    .taskbar_active = 0x00a0a0a0,
    .taskbar_text = 0x00000000,           /* #745: Retro UNIX is a light bar */
    .taskbar_text_muted = 0x00383838,     /* #745 */
    .taskbar_selected_text = 0x00000000,  /* #745 */
    .start_button = 0x00c8c8c8,
    .gauge_bg = 0x00606060,
    .gauge_fg = 0x00006666,
    .menu_bg = 0x00d4d4d4,
    .menu_border = 0x00404040,
    .menu_item_hover = 0x00336666,
    .menu_text = 0x00000000,
    .menu_text_disabled = 0x00808080,
    .menu_separator = 0x00808080,
    .scrollbar_bg = 0x00b4b4b4,
    .scrollbar_thumb = 0x00c8c8c8,
    .scrollbar_thumb_hover = 0x00d4d4d4,
    .selection_bg = 0x00336666,
    .selection_text = 0x00ffffff,
    .color_error = 0x00cc0000,
    .color_warning = 0x00cc8800,
    .color_success = 0x00006600,
    .color_info = 0x00000080,
    .link_color = 0x00000080,
    .link_visited = 0x00800080,
    .link_hover = 0x000000ff,
    .tooltip_bg = 0x00ffffc0,
    .tooltip_text = 0x00000000,
    .tooltip_border = 0x00000000,
};

// Parse a hex color like "0x00B4B4B4" or "B4B4B4" (case-insensitive, no
// leading-0x form accepted too - the .mtheme files this ships write 0x...).
static uint32_t theme_hex_parse(const char *s) {
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
        s++;
    }
    return v;
}

// Integer-only luma approximation (ITU-R BT.601 weights). The kernel target
// is soft-float with SSE disabled (docs/UI_STYLE_GUIDE.md, CLAUDE.md "Rust
// kernel policy" hard limit applies equally to C: no FPU state is saved
// across a context switch), so a true gamma-corrected WCAG relative
// luminance (which needs a fractional power) is not on the table here. This
// is a safety net against genuinely broken pairs, not a design-time
// certifier: build/assets/theme-scale-lint.sh does the real WCAG math
// offline against the shipped .mtheme files. 0..255.
static uint32_t theme_luma(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (r * 299 + g * 587 + b * 114) / 1000;
}

// Runtime contrast floor (themes ticket, 2026-08-07). If *fg and bg are too
// close in luma to read, force *fg to whichever of pure black/white is
// farther from bg, and say so on serial so the fix is visible, not silent.
// THEME_MIN_LUMA_DELTA is deliberately generous (60/255, ~24%): this must
// never override a theme author's deliberate, readable, merely-subtle
// contrast choice, only catch a genuine near-collision (the black-on-black
// / white-on-white class of bug that keeps recurring per-site around this
// codebase - file browser text, taskbar gauges, widget headers, tray icons,
// a calendar "today" circle, selection text).
#define THEME_MIN_LUMA_DELTA 60u
// Corrections made by the CURRENT theme_parse_buffer() call. Reset at the top
// of that function and read back into theme_t.contrast_corrected at the end,
// so a caller (theme_load_file_runtime -> SYS_THEME_LOAD_FILE ->
// SYS_THEME_CONTRAST_CORRECTIONS) can surface "this theme needed N fixes" to
// the user instead of the fix being visible only on serial. A plain static is
// safe here: theme parsing is synchronous and single-threaded (one call
// completes, struct is copied out, before another can start).
static int g_contrast_fix_count;
static void theme_ensure_contrast(uint32_t *fg, uint32_t bg, const char *field, const char *theme_name) {
    uint32_t lf = theme_luma(*fg);
    uint32_t lb = theme_luma(bg);
    uint32_t delta = (lf > lb) ? (lf - lb) : (lb - lf);
    if (delta >= THEME_MIN_LUMA_DELTA) return;
    uint32_t dist_to_white = 255u - lb;
    uint32_t corrected = (dist_to_white >= lb) ? 0x00FFFFFFu : 0x00000000u;
    kprintf("[Themes] contrast fix: '%s' was unreadable against its "
            "background (luma delta %u/255) in theme '%s', forced to %s\n",
            field, delta, theme_name,
            (corrected == 0x00FFFFFFu) ? "white" : "black");
    *fg = corrected;
    g_contrast_fix_count++;
}

// Run the contrast floor over every legacy fg/bg pair a theme file
// realistically drives text/marks against. Called BEFORE theme_fill_v2_defaults()
// (see the call site), so a v2 field that theme_fill_v2_defaults() derives from
// one of these legacy fields inherits the CORRECTED value, not the original bad
// one - a v2 default is a copy, not a live reference, so if this ran after the
// fill instead, a fixed legacy label_text would leave an already-copied,
// still-bad c_on_surface untouched. That ordering bug existed in the first cut
// of this function and is why it is documented here explicitly.
static void theme_ensure_all_contrast(theme_t *t) {
    theme_ensure_contrast(&t->label_text, t->window_bg, "label_text/window_bg", t->name);
    theme_ensure_contrast(&t->button_text, t->button_bg, "button_text/button_bg", t->name);
    theme_ensure_contrast(&t->textbox_text, t->textbox_bg, "textbox_text/textbox_bg", t->name);
    theme_ensure_contrast(&t->menu_text, t->menu_bg, "menu_text/menu_bg", t->name);
    theme_ensure_contrast(&t->selection_text, t->selection_bg, "selection_text/selection_bg", t->name);
    theme_ensure_contrast(&t->titlebar_text, t->titlebar_active, "titlebar_text/titlebar_active", t->name);
    theme_ensure_contrast(&t->tooltip_text, t->tooltip_bg, "tooltip_text/tooltip_bg", t->name);
    theme_ensure_contrast(&t->gauge_fg, t->gauge_bg, "gauge_fg/gauge_bg", t->name);
    theme_ensure_contrast(&t->checkbox_check, t->checkbox_bg, "checkbox_check/checkbox_bg", t->name);
    // (#745) Taskbar-surface ink. This is the fail-safe that makes the three
    // new tokens optional for a third-party theme: omit them and they arrive
    // here holding the fallback palette's near-black, which on a dark bar is
    // exactly the unreadable case, and get corrected to white. Note
    // taskbar_text_muted IS included even though the other deliberately-muted
    // fields are excluded above. That exclusion is right for "disabled", where
    // reduced contrast is the intended meaning; it is wrong here, because this
    // token's only job is the version line the user is expected to read, and a
    // muted colour that cannot clear a 60/255 luma delta is not muted, it is
    // invisible. 60/255 remains a floor, not the target: the target is the
    // 4.5:1 WCAG AA that build/assets/theme-scale-lint.sh check #9 enforces.
    theme_ensure_contrast(&t->taskbar_text, t->taskbar_bg, "taskbar_text/taskbar_bg", t->name);
    theme_ensure_contrast(&t->taskbar_text_muted, t->taskbar_bg, "taskbar_text_muted/taskbar_bg", t->name);
    theme_ensure_contrast(&t->taskbar_selected_text, t->taskbar_active, "taskbar_selected_text/taskbar_active", t->name);
}

// v2-token counterpart of theme_ensure_all_contrast(), run AFTER
// theme_fill_v2_defaults() so it sees the FINAL value of every v2 field
// regardless of whether that value came from the file directly (a theme can
// set state.item_selected_fg/bg explicitly, bypassing every legacy field
// entirely) or from a default derived above. This is what actually protects
// the historically-broken surfaces named in the themes ticket once an app
// reads v2 tokens instead of the legacy 51: selected file-list rows
// (state.item_selected_*), selection text generally (color.sel_*), taskbar/
// widget accents (color.accent/on_accent), and both titlebar text roles
// checked against BOTH gradient stops (the renderer's one gradient keeps the
// two stops within ~12% luma of each other - theme-scale-lint.sh enforces
// that at commit time for shipped themes - so two sequential single-bg
// corrections converge to a value good against both in practice; a
// downloaded theme that violates that assumption still gets a value proven
// correct against at least the SECOND stop, never an uncorrected pair).
// Disabled/muted fields are deliberately excluded: reduced contrast there is
// intentional design, not a bug (WCAG AA does not require it either).
static void theme_ensure_v2_contrast(theme_t *t) {
    theme_ensure_contrast(&t->c_on_surface, t->c_surface, "color.on_surface/color.surface", t->name);
    theme_ensure_contrast(&t->c_on_accent, t->c_accent, "color.on_accent/color.accent", t->name);
    theme_ensure_contrast(&t->c_sel_fg, t->c_sel_bg, "color.sel_fg/color.sel_bg", t->name);
    theme_ensure_contrast(&t->c_titlebar_text, t->c_titlebar_top, "color.titlebar_text/color.titlebar_top", t->name);
    theme_ensure_contrast(&t->c_titlebar_text, t->c_titlebar_bottom, "color.titlebar_text/color.titlebar_bottom", t->name);
    theme_ensure_contrast(&t->c_titlebar_text_inactive, t->c_titlebar_inactive_top, "color.titlebar_text_inactive/color.titlebar_inactive_top", t->name);
    theme_ensure_contrast(&t->c_titlebar_text_inactive, t->c_titlebar_inactive_bottom, "color.titlebar_text_inactive/color.titlebar_inactive_bottom", t->name);

    theme_ensure_contrast(&t->s_btn_rest_fg, t->s_btn_rest_bg, "state.btn_rest_fg/state.btn_rest_bg", t->name);
    theme_ensure_contrast(&t->s_btn_hover_fg, t->s_btn_hover_bg, "state.btn_hover_fg/state.btn_hover_bg", t->name);
    theme_ensure_contrast(&t->s_btn_active_fg, t->s_btn_active_bg, "state.btn_active_fg/state.btn_active_bg", t->name);
    theme_ensure_contrast(&t->s_btn_selected_fg, t->s_btn_selected_bg, "state.btn_selected_fg/state.btn_selected_bg", t->name);

    theme_ensure_contrast(&t->s_item_rest_fg, t->s_item_rest_bg, "state.item_rest_fg/state.item_rest_bg", t->name);
    theme_ensure_contrast(&t->s_item_hover_fg, t->s_item_hover_bg, "state.item_hover_fg/state.item_hover_bg", t->name);
    theme_ensure_contrast(&t->s_item_active_fg, t->s_item_active_bg, "state.item_active_fg/state.item_active_bg", t->name);
    theme_ensure_contrast(&t->s_item_selected_fg, t->s_item_selected_bg, "state.item_selected_fg/state.item_selected_bg", t->name);

    theme_ensure_contrast(&t->s_input_rest_fg, t->s_input_rest_bg, "state.input_rest_fg/state.input_rest_bg", t->name);
    theme_ensure_contrast(&t->s_input_selected_fg, t->s_input_selected_bg, "state.input_selected_fg/state.input_selected_bg", t->name);
}

// Parse one /THEMES/*.mtheme file's bytes (flat "key=value" text, '#'/';'
// comments, blank lines ignored) into *out. Unknown keys are ignored so
// newer/older files stay forward/backward compatible. Returns 0 on success
// (a file with no recognizable "name=" line still succeeds, defaulting the
// name to "Untitled", since a mistyped metadata line should not lose an
// otherwise-good palette).
static int theme_parse_buffer(const char *data, uint32_t size, theme_t *out) {
    // FAIL-CLOSED FIX (themes ticket, 2026-08-07): start every parse from the
    // proven-readable g_fallback_theme palette, not zeroed memory. Previously
    // a theme file that omitted a legacy-51 key (missing window_bg, missing
    // label_text, a truncated/near-empty file, ...) left that field at
    // 0x000000. A minimal or corrupt file that set almost nothing rendered
    // literally black text on a black window: an unreadable desktop, exactly
    // the failure mode this ticket asked to close as a mechanism rather than
    // another per-site patch. Any key the file DOES set below still
    // overwrites this baseline, so a complete theme is unaffected byte for
    // byte; only what a partial file leaves unspecified now degrades to
    // Retro UNIX's colors instead of black. See theme_ensure_contrast()
    // further down for the complementary fix: a key that IS present but
    // carries a garbage/out-of-range value (not merely absent).
    *out = g_fallback_theme;
    out->name[0] = 0; // restore the "Untitled" default for an unnamed file
    // #711: which v2 keys the file actually carried. 0 is a legal colour AND a
    // legal metric, so "unset" cannot be a sentinel value; it has to be a
    // separate bit per field. Both arrays are fixed-size and indexed by the
    // same _Static_assert-locked tables, so this adds no unbounded state.
    uint8_t seen_c[THEME_V2C_COUNT];
    uint8_t seen_i[THEME_INT_COUNT];
    memset(seen_c, 0, sizeof(seen_c));
    memset(seen_i, 0, sizeof(seen_i));
    uint32_t i = 0;
    while (i < size) {
        char line[128];
        int ll = 0;
        while (i < size && data[i] != '\n' && ll < (int)sizeof(line) - 1) {
            line[ll++] = data[i];
            i++;
        }
        if (i < size && data[i] == '\n') i++;
        if (ll > 0 && line[ll - 1] == '\r') ll--;
        line[ll] = 0;

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == '#' || *p == ';') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p;
        char *val = eq + 1;

        if (strcmp(key, "name") == 0) {
            strncpy(out->name, val, THEME_NAME_LEN - 1);
            out->name[THEME_NAME_LEN - 1] = 0;
            continue;
        }
        if (strcmp(key, "style") == 0) {
            if (strcmp(val, "retro") == 0) out->style = THEME_STYLE_RETRO;
            else if (strcmp(val, "modern") == 0) out->style = THEME_STYLE_MODERN;
            else out->style = THEME_STYLE_MIXED;
            continue;
        }
        // "author", "version", "dark" are metadata for pickers (read directly
        // from the file by the userland loader); themes.h's theme_t has no
        // field for them, so the kernel-side struct just ignores those keys.
        if (strcmp(key, "author") == 0 || strcmp(key, "version") == 0 ||
            strcmp(key, "dark") == 0) {
            continue;
        }

        int matched = 0;
        for (size_t f = 0; f < THEME_FIELD_COUNT; f++) {
            if (strcmp(key, g_theme_fields[f].key) == 0) {
                *(uint32_t *)((char *)out + g_theme_fields[f].offset) = theme_hex_parse(val);
                matched = 1;
                break;
            }
        }
        if (matched) continue;
        for (size_t f = 0; f < THEME_V2C_COUNT; f++) {
            if (strcmp(key, g_theme_v2_color_fields[f].key) == 0) {
                *(uint32_t *)((char *)out + g_theme_v2_color_fields[f].offset) = theme_hex_parse(val);
                seen_c[f] = 1;
                matched = 1;
                break;
            }
        }
        if (matched) continue;
        for (size_t f = 0; f < THEME_INT_COUNT; f++) {
            if (strcmp(key, g_theme_int_fields[f].key) == 0) {
                *(int32_t *)((char *)out + g_theme_int_fields[f].offset) = theme_int_parse(val);
                seen_i[f] = 1;
                break;
            }
        }
    }
    if (out->name[0] == 0) {
        strncpy(out->name, "Untitled", THEME_NAME_LEN - 1);
    }
    // Order matters: fix the legacy fields FIRST so any v2 field
    // theme_fill_v2_defaults() derives from them copies the corrected value,
    // then fix the v2 fields (both defaulted and file-set explicitly) last so
    // nothing downstream of either path can still be a bad pair.
    g_contrast_fix_count = 0;
    theme_ensure_all_contrast(out);
    theme_fill_v2_defaults(out, seen_c, seen_i);
    theme_ensure_v2_contrast(out);
    out->contrast_corrected = g_contrast_fix_count;
    return 0;
}

// Read + parse one theme file from disk (FAT or ext2 root - fat_read_file
// already redirects transparently via g_root_ext2, see fs/ext2.c).
static int theme_load_from_file(const char *path, theme_t *out) {
    uint32_t sz = 0;
    char *data = (char *)fat_read_file(&g_fat_fs, path, &sz);
    if (!data) return -1;
    int rc = theme_parse_buffer(data, sz, out);
    kfree(data);
    return rc;
}

// #565 runtime install: parse one .mtheme file and add it to (or update it
// in, matched by name) the live table without a reboot. Returns the
// resulting index, or -1 on failure. Called from SYS_THEME_LOAD_FILE, which
// the userland loader (gui_theme.c) invokes right after writing a new
// /THEMES/<slug>.mtheme (Settings "browse a custom theme" and the App Store
// "type=theme" install path both go through this).
int theme_load_file_runtime(const char *path) {
    if (!g_themes_initialized) theme_init();

    theme_t t;
    if (theme_load_from_file(path, &t) != 0) {
        kprintf("[Themes] Could not load theme file: %s\n", path);
        return -1;
    }
    for (int i = 0; i < g_theme_count; i++) {
        if (strcmp(g_themes[i].name, t.name) == 0) {
            g_themes[i] = t;
            kprintf("[Themes] Reloaded theme '%s' (index %d) from %s\n", t.name, i, path);
            return i;
        }
    }
    if (g_theme_count >= MAX_THEMES) {
        kprintf("[Themes] Cannot load %s: theme table full (%d)\n", path, MAX_THEMES);
        return -1;
    }
    g_themes[g_theme_count] = t;
    int idx = g_theme_count;
    g_theme_count++;
    kprintf("[Themes] Loaded theme '%s' as new index %d from %s\n", t.name, idx, path);
    return idx;
}


// Initialize the theme system: read /THEMES/INDEX.TXT (one filename per
// line, in load order - this defines which numeric index each theme gets,
// preserving the old THEME_DEFAULT..THEME_FLUENT_DARK 0-11 meaning for every
// existing call site) and parse each listed /THEMES/*.mtheme file into the
// live table. If the index file or every listed file is unreadable (no
// /THEMES on this image, or too early in boot), fall back to the single
// hardcoded g_fallback_theme so the login screen never renders black.
void theme_init(void) {
    if (g_themes_initialized) {
        return;
    }

    g_theme_count = 0;

    uint32_t isz = 0;
    char *idx = (char *)fat_read_file(&g_fat_fs, THEME_INDEX_FILE, &isz);
    if (idx) {
        uint32_t p = 0;
        while (p < isz && g_theme_count < MAX_THEMES) {
            char fname[64];
            int fl = 0;
            while (p < isz && idx[p] != '\n' && fl < (int)sizeof(fname) - 1) {
                fname[fl++] = idx[p];
                p++;
            }
            if (p < isz && idx[p] == '\n') p++;
            if (fl > 0 && fname[fl - 1] == '\r') fl--;
            fname[fl] = 0;
            if (fl == 0) continue;

            char path[96];
            snprintf(path, sizeof(path), "%s%s", THEME_DIR_PREFIX, fname);
            theme_t t;
            if (theme_load_from_file(path, &t) == 0) {
                g_themes[g_theme_count++] = t;
            } else {
                // Terminal-theme pitfall (#241/termscroll, 2026-08-22): this used to
                // just "continue" without incrementing g_theme_count. That SKIPS the
                // slot instead of filling it, so every theme listed AFTER this one in
                // INDEX.TXT shifts down by one kernel index. Userland's
                // gui_theme_list() (userland/libc/gui_theme.c) always assigns an
                // index by LISTING POSITION alone, regardless of whether the kernel
                // could read that particular file - it has no way to know this
                // happened. The next theme_color_of(index, ...) call for anything
                // after the failed file then reads a DIFFERENT theme's colors out of
                // this table: a transient read failure for ONE file (heap pressure,
                // an ext2 hiccup, anything short of the file being entirely absent)
                // silently desyncs every theme index after it for the rest of the
                // boot, with no serial trace beyond one easy-to-miss warning line.
                // This is exactly the shape of bug the terminal's independent theme
                // (term_resolve_theme() in userland/apps/terminal/main.c) was
                // reported to hit: a theme that "saved correctly" but did not
                // visibly repaint, or repainted as some other theme.
                //
                // Fix the MECHANISM, not the instance: keep the SLOT. A fallback
                // palette goes in this exact index so position in g_themes[] always
                // matches position in INDEX.TXT, whether or not this one file loaded.
                // A theme that fails to load still looks wrong (fallback colors, not
                // its own), but every OTHER theme's index stays correct - the failure
                // is now contained to the one slot instead of corrupting every index
                // after it.
                kprintf("[Themes] Warning: could not load %s; using the fallback "
                        "palette for index %d so every later theme keeps its correct "
                        "index\n", path, g_theme_count);
                theme_t fb = g_fallback_theme;
                fb.name[0] = 0;
                strncpy(fb.name, fname, THEME_NAME_LEN - 1);
                fb.name[THEME_NAME_LEN - 1] = 0;
                {
                    uint8_t zc[THEME_V2C_COUNT], zi[THEME_INT_COUNT];
                    memset(zc, 0, sizeof(zc));
                    memset(zi, 0, sizeof(zi));
                    theme_fill_v2_defaults(&fb, zc, zi);
                }
                g_themes[g_theme_count++] = fb;
            }
        }
        kfree(idx);
    }

    if (g_theme_count == 0) {
        kprintf("[Themes] No usable /THEMES/INDEX.TXT; using the single built-in fallback theme\n");
        g_themes[0] = g_fallback_theme;
        {   // #711: the pre-mount fallback needs its v2 fields filled too, or
            // the login screen would draw with a 0-pixel titlebar.
            uint8_t zc[THEME_V2C_COUNT], zi[THEME_INT_COUNT];
            memset(zc, 0, sizeof(zc));
            memset(zi, 0, sizeof(zi));
            theme_fill_v2_defaults(&g_themes[0], zc, zi);
        }
        g_theme_count = 1;
    }

    g_current_theme = (THEME_DEFAULT < g_theme_count) ? THEME_DEFAULT : 0;
    g_themes_initialized = true;

    kprintf("[Themes] Theme system initialized with %d themes (file-based)\n", g_theme_count);
    kprintf("[Themes] Active theme: %s (titlebar=0x%08X, text=0x%08X)\n",
            g_themes[g_current_theme].name, g_themes[g_current_theme].titlebar_active,
            g_themes[g_current_theme].titlebar_text);
}


// Get the currently active theme
const theme_t *theme_get_current(void) {
    if (g_current_theme >= 0 && g_current_theme < g_theme_count) {
        return &g_themes[g_current_theme];
    }
    return &g_themes[0];  // Fallback to default
}

// Get a specific theme by ID
const theme_t *theme_get_by_id(int theme_id) {
    if (theme_id >= 0 && theme_id < g_theme_count) {
        return &g_themes[theme_id];
    }
    return NULL;
}

// Get the current theme ID
int theme_get_current_id(void) {
    return g_current_theme;
}

// How many fg/bg pairs theme_ensure_all_contrast()/theme_ensure_v2_contrast()
// had to force-correct the last time this theme was parsed (0 for a clean
// file). Backs SYS_THEME_CONTRAST_CORRECTIONS, which the userland loader
// (gui_theme.c) checks right after a load so it can tell the user their
// theme was adjusted for readability instead of that being visible only on
// serial. theme_id < 0 = current theme, matching every other accessor here.
int theme_get_contrast_corrections(int theme_id) {
    const theme_t *t = (theme_id < 0) ? theme_get_current() : theme_get_by_id(theme_id);
    if (!t) return 0;
    return t->contrast_corrected;
}

// (#285) Resolve a userland theme_color_id_t to the active theme's actual
// color from the single-source-of-truth theme_t table. theme_id < 0 = current.
// Color ids MUST match userland libc theme.h theme_color_id_t enum order.
uint32_t theme_get_color_by_id(int theme_id, int color_id) {
    const theme_t *t = (theme_id < 0) ? theme_get_current()
                                      : theme_get_by_id(theme_id);
    if (!t) t = theme_get_current();
    switch (color_id) {
        case 0:  return t->window_bg;            // BACKGROUND
        case 1:  return t->label_text;           // FOREGROUND
        case 2:  return t->c_accent;             // ACCENT (#711 color.accent)
        case 3:  return t->c_sel_bg;             // SELECTION (#711 color.sel_bg)
        case 4:  return t->c_sel_fg;             // SELECTION_TEXT (#711 color.sel_fg)
        case 5:  return t->titlebar_active;      // TITLEBAR_ACTIVE
        case 6:  return t->titlebar_inactive;    // TITLEBAR_INACTIVE
        case 7:  return t->titlebar_text;        // TITLEBAR_TEXT
        case 8:  return t->window_bg;            // WINDOW_BG
        case 9:  return t->c_border_strong;      // WINDOW_BORDER (#711 color.border_strong)
        case 10: return t->close_button;         // CLOSE_BUTTON
        case 11: return t->close_button_hover;   // CLOSE_BUTTON_HOVER
        case 12: return t->minimize_button;      // MINIMIZE_BUTTON
        case 13: return t->maximize_button;      // MAXIMIZE_BUTTON
        case 14: return t->button_bg;            // BUTTON_FACE
        case 15: return t->button_bg_hover;      // BUTTON_LIGHT
        case 16: return t->button_border;        // BUTTON_SHADOW
        case 17: return t->button_border;        // BUTTON_DARK
        case 18: return t->button_text;          // BUTTON_TEXT
        case 19: return t->button_disabled;      // BUTTON_DISABLED
        case 20: return t->label_text;           // LABEL_TEXT
        case 21: return t->textbox_bg;           // TEXTBOX_BG
        case 22: return t->textbox_border;       // TEXTBOX_BORDER
        case 23: return t->textbox_text;         // TEXTBOX_TEXT
        case 24: return t->textbox_cursor;       // TEXTBOX_CURSOR
        case 25: return t->checkbox_bg;          // CHECKBOX_BG
        case 26: return t->checkbox_border;      // CHECKBOX_BORDER
        case 27: return t->checkbox_check;       // CHECKBOX_CHECK
        case 28: return t->desktop_bg;           // DESKTOP_BG
        case 29: return t->taskbar_bg;           // TASKBAR_BG
        case 30: return t->taskbar_hover;        // TASKBAR_HOVER
        case 31: return t->taskbar_active;       // TASKBAR_ACTIVE
        case 32: return t->start_button;         // START_BUTTON
        case 33: return t->gauge_bg;             // GAUGE_BG
        case 34: return t->gauge_fg;             // GAUGE_FG
        case 35: return t->menu_bg;              // MENU_BG
        case 36: return t->menu_border;          // MENU_BORDER
        case 37: return t->s_item_hover_bg;      // MENU_ITEM_HOVER (#711 state.item_hover_bg)
        case 38: return t->menu_text;            // MENU_TEXT
        case 39: return t->menu_text_disabled;   // MENU_TEXT_DISABLED
        case 40: return t->menu_separator;       // MENU_SEPARATOR
        case 41: return t->scrollbar_bg;         // SCROLLBAR_BG
        case 42: return t->scrollbar_thumb;      // SCROLLBAR_THUMB
        case 43: return t->scrollbar_thumb_hover;// SCROLLBAR_THUMB_HOVER
        case 44: return t->window_bg;            // TAB_BG
        case 45: return t->c_accent;             // TAB_ACTIVE (#711 color.accent)
        case 46: return t->window_border;        // TAB_BORDER
        case 47: return t->tooltip_bg;           // TOOLTIP_BG
        case 48: return t->tooltip_border;       // TOOLTIP_BORDER
        case 49: return t->tooltip_text;         // TOOLTIP_TEXT
        case 50: return t->gauge_bg;             // PROGRESS_BG
        case 51: return t->gauge_fg;             // PROGRESS_FG
        // (#704) These four theme_t fields have existed since before #711 and
        // are already parsed from every .mtheme file's color_error/warning/
        // success/info keys (see the offset table above); nothing until now
        // read them back out to userland, so an app that wanted "the theme's
        // error red" had no token and reached for a raw literal instead.
        case 52: return t->color_error;          // ERROR
        case 53: return t->color_warning;        // WARNING
        case 54: return t->color_success;        // SUCCESS
        case 55: return t->color_info;           // INFO
        case 56: return t->c_on_surface_muted;   // MUTED (#711 color.on_surface_muted)
        // (#745) Taskbar-surface ink. APPEND ONLY; ids must match the
        // userland libc theme.h theme_color_id_t enum order.
        case 57: return t->taskbar_text;           // TASKBAR_TEXT
        case 58: return t->taskbar_text_muted;     // TASKBAR_TEXT_MUTED
        case 59: return t->taskbar_selected_text;  // TASKBAR_SELECTED_TEXT
        // (#745) FOCUS_RING. c_focus_ring has been parsed out of every .mtheme
        // file since #711 and this is its FIRST reader: there was no case for
        // it here, so userland could not ask for it and libc's gui_button()
        // drew its focus ring from `accent` instead (2.89:1 on the default
        // dark surface, below the 3:1 non-text floor). Written in C, not Rust,
        // for one reason: it is a single arm of an existing C switch over an
        // existing C struct field, and an FFI seam for one enum case would add
        // a boundary without adding any Rust.
        case 60: return t->c_focus_ring;         // FOCUS_RING (#711 color.focus_ring)
        // (#745) The two titlebar GRADIENT STOPS. Parsed from color.titlebar_top
        // / color.titlebar_bottom into theme_t since #711, read by the kernel
        // decorator ever since, and never reachable from userland: there was no
        // case here, so a userland theme PREVIEW had to guess the gradient. The
        // guess is badly wrong for the two themes that ship distinct stops
        // (maytera_light/maytera_dark are nearly flat; the derived fallback
        // lifts 22% toward white), which is exactly the "invented preview
        // colour" the theme-preview work exists to remove. Same one-arm-of-an-
        // existing-C-switch justification as FOCUS_RING above for staying C.
        // APPEND ONLY; ids must match libc theme.h theme_color_id_t order.
        case 61: return t->c_titlebar_top;       // TITLEBAR_TOP
        case 62: return t->c_titlebar_bottom;    // TITLEBAR_BOTTOM
        // (confirm-modal, docs/CONFIRM_MODAL_DESIGN.html) Seven more v2 token
        // fields that have existed in theme_t (and been parsed from every
        // .mtheme file's color.* keys, see the offset table above) since #711,
        // with no reader here, so no userland caller could ever ask for them by
        // name: surface_overlay/surface_raised/on_surface/on_accent/danger/
        // on_danger/border_subtle. The confirm/notice modal is the first
        // caller that needs a filled danger/accent SURFACE under fixed text
        // rather than accent used as a boundary colour (see the design doc
        // 5.1's WCAG table: 7 of 14 themes fail 4.5:1 on this exact pair,
        // which is why the porting code floors these through
        // gui_ensure_contrast() at draw time instead of trusting the raw
        // value). Same one-arm-of-an-existing-C-switch justification as
        // FOCUS_RING/TITLEBAR_TOP above for staying C. APPEND ONLY; ids must
        // match userland libc theme.h theme_color_id_t enum order.
        case 63: return t->c_surface_overlay;    // SURFACE_OVERLAY (color.surface_overlay)
        case 64: return t->c_surface_raised;     // SURFACE_RAISED (color.surface_raised)
        case 65: return t->c_on_surface;         // ON_SURFACE (color.on_surface)
        case 66: return t->c_on_accent;          // ON_ACCENT (color.on_accent)
        case 67: return t->c_danger;             // DANGER (color.danger)
        case 68: return t->c_on_danger;          // ON_DANGER (color.on_danger)
        case 69: return t->c_border_subtle;      // BORDER_SUBTLE (color.border_subtle)
        default: return t->window_bg;
    }
}


// Set the active theme by ID
void theme_set(int theme_id) {
    if (theme_id >= 0 && theme_id < g_theme_count) {
        g_current_theme = theme_id;
        kprintf("[Themes] Theme changed to: %s (id=%d)\n", g_themes[theme_id].name, theme_id);

        // Notify font system of theme change
        theme_notify_font_system();
    } else {
        kprintf("[Themes] Invalid theme ID: %d\n", theme_id);
    }
}


// Get theme name by ID
const char *theme_get_name(int theme_id) {
    if (theme_id >= 0 && theme_id < g_theme_count) {
        return g_themes[theme_id].name;
    }
    return "Unknown";
}

// Notify the font system when theme changes
// This updates the font rendering mode based on theme style
void theme_notify_font_system(void) {
    const theme_t *theme = theme_get_current();

    if (theme->style == THEME_STYLE_RETRO) {
        font_set_render_mode(FONT_RENDER_BITMAP);
    } else if (theme->style == THEME_STYLE_MODERN) {
        font_set_render_mode(FONT_RENDER_ANTIALIAS);
    } else {
        // Mixed style - use bitmap for crisp rendering
        font_set_render_mode(FONT_RENDER_BITMAP);
    }
}
