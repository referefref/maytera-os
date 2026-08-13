// themes.h - Color theme system for MayteraOS GUI
// Provides comprehensive theming for all UI elements including colors and typography
#ifndef THEMES_H
#define THEMES_H

#include "../types.h"

// Maximum number of loaded themes (built-in + installed from /THEMES/*.mtheme).
// #565: palettes are no longer compiled in; this just bounds the in-memory
// table the file-based loader (theme_init()/theme_load_file_runtime()) fills.
#define MAX_THEMES  32

// ============================================================================
// Theme Style Categories
// ============================================================================

// Font rendering style (bitmap for retro, antialiased for modern)
typedef enum {
    THEME_STYLE_RETRO,      // Crisp bitmap fonts, 3D beveled UI
    THEME_STYLE_MODERN,     // Anti-aliased fonts, flat/minimal UI
    THEME_STYLE_MIXED,      // Mix of both styles
} theme_style_t;

// Theme identifiers
// THEME_DEFAULT (0) is the Retro UNIX/CDE theme - the signature MayteraOS look
#define THEME_DEFAULT       0   // Retro UNIX (CDE/Motif-inspired) - MayteraOS signature theme
#define THEME_DARK          1
#define THEME_LIGHT         2
#define THEME_HIGH_CONTRAST 3
#define THEME_CLASSIC       4   // Windows 95/98 style
#define THEME_OCEAN         5
#define THEME_SUNSET        6
#define THEME_FOREST        7
#define THEME_MODERN_LIGHT  8   // macOS Big Sur light mode
#define THEME_MODERN_DARK   9   // macOS Big Sur dark mode
#define THEME_FLUENT_LIGHT  10  // Windows 11 Fluent Design - Light Mode
#define THEME_FLUENT_DARK   11  // Windows 11 Fluent Design - Dark Mode

// Theme name max length
#define THEME_NAME_LEN      32

// Theme color structure - contains all UI colors
// All colors are in 0x00RRGGBB format (no alpha for simplicity)
typedef struct {
    char name[THEME_NAME_LEN];      // Theme display name
    theme_style_t style;            // Visual style category

    // Window titlebar colors
    uint32_t titlebar_active;       // Active window titlebar
    uint32_t titlebar_inactive;     // Inactive window titlebar
    uint32_t titlebar_text;         // Titlebar text color

    // Window body colors
    uint32_t window_bg;             // Window background (content area)
    uint32_t window_border;         // Window border color

    // Window control buttons
    uint32_t close_button;          // Close button color
    uint32_t close_button_hover;    // Close button hover color
    uint32_t minimize_button;       // Minimize button color
    uint32_t maximize_button;       // Maximize button color

    // Button widget colors
    uint32_t button_bg;             // Normal button background
    uint32_t button_bg_hover;       // Button hover background
    uint32_t button_bg_pressed;     // Button pressed background
    uint32_t button_border;         // Button border
    uint32_t button_text;           // Button text color
    uint32_t button_disabled;       // Disabled button color

    // Text widget colors
    uint32_t label_text;            // Label text color
    uint32_t textbox_bg;            // Textbox background
    uint32_t textbox_border;        // Textbox border
    uint32_t textbox_text;          // Textbox text
    uint32_t textbox_cursor;        // Textbox cursor

    // Checkbox colors
    uint32_t checkbox_bg;           // Checkbox background
    uint32_t checkbox_border;       // Checkbox border
    uint32_t checkbox_check;        // Checkmark color

    // Desktop colors
    uint32_t desktop_bg;            // Default desktop background

    // Taskbar/Dock colors
    uint32_t taskbar_bg;            // Taskbar background
    uint32_t taskbar_hover;         // Taskbar item hover
    uint32_t taskbar_active;        // Active taskbar item
    // (#745) Foreground tokens for the taskbar SURFACE. taskbar_bg had no
    // companion ink token, so every consumer that drew text on it either
    // derived one (the compositor's readable_ink()) or borrowed a token
    // contracted against a different background (Settings' sidebar used
    // label_text, whose contract is against window_bg). On a theme with a
    // light window and a dark taskbar that borrow renders dark-on-dark.
    // theme_ensure_all_contrast() enforces all three against the exact
    // background each is painted on, so a theme that omits them still gets
    // a readable value rather than the fallback palette's black.
    uint32_t taskbar_text;          // Primary ink on taskbar_bg / taskbar_hover
    uint32_t taskbar_text_muted;    // Secondary ink on taskbar_bg
    uint32_t taskbar_selected_text; // Ink on taskbar_active
    uint32_t start_button;          // Start button color
    uint32_t gauge_bg;              // Resource gauge background
    uint32_t gauge_fg;              // Resource gauge fill

    // Menu colors
    uint32_t menu_bg;               // Menu background
    uint32_t menu_border;           // Menu border
    uint32_t menu_item_hover;       // Menu item hover highlight
    uint32_t menu_text;             // Menu text color
    uint32_t menu_text_disabled;    // Disabled menu item text
    uint32_t menu_separator;        // Menu separator line

    // Scrollbar colors
    uint32_t scrollbar_bg;          // Scrollbar track
    uint32_t scrollbar_thumb;       // Scrollbar thumb
    uint32_t scrollbar_thumb_hover; // Scrollbar thumb hover

    // Selection colors
    uint32_t selection_bg;          // Selected item background
    uint32_t selection_text;        // Selected item text

    // Status/alert colors
    uint32_t color_error;           // Error/danger color
    uint32_t color_warning;         // Warning color
    uint32_t color_success;         // Success/positive color
    uint32_t color_info;            // Information color

    // Link colors
    uint32_t link_color;            // Hyperlink color
    uint32_t link_visited;          // Visited link color
    uint32_t link_hover;            // Link hover color

    // Tooltip colors
    uint32_t tooltip_bg;            // Tooltip background
    uint32_t tooltip_text;          // Tooltip text
    uint32_t tooltip_border;        // Tooltip border

    // ========================================================================
    // mtheme v2 (#711): namespaced tokens. Everything below is DATA, parsed
    // from the same flat key=value .mtheme file by the same dumb line reader.
    // A key absent from the file is filled by theme_fill_v2_defaults() from
    // the legacy 51 colours above, so every pre-v2 theme file still produces
    // a complete, correct v2 theme with no edit.
    // ========================================================================

    // --- color.* : semantic colour tokens -----------------------------------
    uint32_t c_surface_sunken;
    uint32_t c_surface;
    uint32_t c_surface_raised;
    uint32_t c_surface_overlay;
    uint32_t c_on_surface;
    uint32_t c_on_surface_muted;
    uint32_t c_on_surface_disabled;
    uint32_t c_accent;
    uint32_t c_on_accent;
    uint32_t c_accent_hover;
    uint32_t c_accent_active;
    uint32_t c_danger;
    uint32_t c_on_danger;
    uint32_t c_border_subtle;
    uint32_t c_border_strong;
    uint32_t c_focus_ring;
    uint32_t c_sel_bg;
    uint32_t c_sel_fg;
    uint32_t c_titlebar_top;
    uint32_t c_titlebar_bottom;
    uint32_t c_titlebar_inactive_top;
    uint32_t c_titlebar_inactive_bottom;
    uint32_t c_titlebar_text;
    uint32_t c_titlebar_text_inactive;
    // #711 loop 2 (designer 1, window decorations): wiring for the three
    // titlebar-button state tokens staged (data-only) in loop 1. hover/active
    // apply to the filter/minimize/maximize buttons; close_active is reserved
    // for a future mouse-up-executes-close change (see kernel/gui/window.c,
    // wm_handle_mouse_move, for why a true pressed flash cannot render under
    // the current mouse-down-executes semantics).
    uint32_t c_titlebar_btn_hover;
    uint32_t c_titlebar_btn_active;
    uint32_t c_titlebar_close_active;

    // --- state.* : the six explicit states per control family ---------------
    // (rest / hover / active(pressed) / focus / disabled / selected). No state
    // is derived by alpha, because there is no per-pixel alpha to derive with.
    uint32_t s_btn_rest_bg,     s_btn_rest_fg;
    uint32_t s_btn_hover_bg,    s_btn_hover_fg;
    uint32_t s_btn_active_bg,   s_btn_active_fg;
    uint32_t s_btn_focus_ring;
    uint32_t s_btn_disabled_bg, s_btn_disabled_fg;
    uint32_t s_btn_selected_bg, s_btn_selected_fg;
    uint32_t s_item_rest_bg,     s_item_rest_fg;
    uint32_t s_item_hover_bg,    s_item_hover_fg;
    uint32_t s_item_active_bg,   s_item_active_fg;
    uint32_t s_item_focus_ring;
    uint32_t s_item_disabled_bg, s_item_disabled_fg;
    uint32_t s_item_selected_bg, s_item_selected_fg;
    uint32_t s_input_rest_bg,     s_input_rest_fg,     s_input_rest_border;
    uint32_t s_input_hover_border;
    uint32_t s_input_active_border;
    uint32_t s_input_focus_ring;
    uint32_t s_input_disabled_bg, s_input_disabled_fg;
    uint32_t s_input_selected_bg, s_input_selected_fg;

    // --- metric.* / radius.* / decor.* / type.* : integers -------------------
    int32_t m_titlebar_h;
    int32_t m_border_w;
    int32_t m_btn_h;
    int32_t m_input_h;
    int32_t m_pad;
    int32_t m_gap;
    int32_t m_focus_w;
    int32_t m_winmenu_roww;
    int32_t m_winmenu_rowh;
    int32_t m_winmenu_hdr;
    int32_t m_titlebar_btn;      // titlebar control button box (was CLOSE_BUTTON_SIZE)
    int32_t m_titlebar_btn_gap;  // was TITLEBAR_BUTTON_SPACING
    int32_t m_title_inset;       // #711 loop 2: was the hardcoded "+ 4" in window.c
    int32_t m_grip;              // was RESIZE_GRIP_SIZE
    int32_t m_scrollbar_w;
    int32_t m_menu_row_h;

    int32_t r_btn;
    int32_t r_input;
    int32_t r_menu;
    int32_t r_card;
    // #27: outer window corner treatment. The key radius.window has shipped in
    // every .mtheme since the v2 rewrite (with 0 on retro_unix/classic/
    // high_contrast and a non-zero value everywhere else) but had no parser
    // row and no reader, so it was dead data. It is now the CHAMFER EXTENT in
    // pixels of the window's four outer corners; 0 means square.
    int32_t r_window;

    int32_t d_style;             // 0 beveled, 1 flat, 2 gradient
    int32_t d_titlebar_gradient; // 0/1
    int32_t d_grip;              // 0/1 draw the resize grips

    int32_t t_caption,  t_caption_lh,  t_caption_w;
    int32_t t_body,     t_body_lh,     t_body_w;
    int32_t t_title,    t_title_lh,    t_title_w;
    int32_t t_heading,  t_heading_lh,  t_heading_w;
    int32_t t_display,  t_display_lh,  t_display_w;

    // Set by theme_ensure_all_contrast()/theme_ensure_v2_contrast() during
    // parse (themes ticket, 2026-08-07): how many fg/bg pairs this theme
    // needed force-corrected for readability. 0 for a clean file. Not a
    // .mtheme key - always computed, never read from a file.
    int32_t contrast_corrected;

} theme_t;

// ============================================================================
// mtheme v2 integer metric ids (#711). MUST match userland libc theme.h
// theme_metric_id_t exactly - SYS_THEME_METRIC passes these across the ABI.
// APPEND ONLY: an id is a wire value, never renumber.
// ============================================================================
typedef enum {
    TM_TITLEBAR_H = 0,
    TM_BORDER_W,
    TM_BTN_H,
    TM_INPUT_H,
    TM_PAD,
    TM_GAP,
    TM_FOCUS_W,
    TM_WINMENU_ROWW,
    TM_WINMENU_ROWH,
    TM_WINMENU_HDR,
    TM_TITLEBAR_BTN,
    TM_TITLEBAR_BTN_GAP,
    TM_GRIP,
    TM_SCROLLBAR_W,
    TM_MENU_ROW_H,
    TM_RADIUS_BTN,
    TM_RADIUS_INPUT,
    TM_RADIUS_MENU,
    TM_RADIUS_CARD,
    TM_DECOR_STYLE,
    TM_DECOR_TITLEBAR_GRADIENT,
    TM_DECOR_GRIP,
    TM_TYPE_CAPTION,   TM_TYPE_CAPTION_LH,  TM_TYPE_CAPTION_W,
    TM_TYPE_BODY,      TM_TYPE_BODY_LH,     TM_TYPE_BODY_W,
    TM_TYPE_TITLE,     TM_TYPE_TITLE_LH,    TM_TYPE_TITLE_W,
    TM_TYPE_HEADING,   TM_TYPE_HEADING_LH,  TM_TYPE_HEADING_W,
    TM_TYPE_DISPLAY,   TM_TYPE_DISPLAY_LH,  TM_TYPE_DISPLAY_W,
    TM_TITLE_INSET,    // #711 loop 2 (designer 1): appended, never renumber
    TM_RADIUS_WINDOW,  // #27: appended, never renumber
    TM_COUNT
} theme_metric_v2_t;

// decor.style values
#define TDECOR_BEVELED   0
#define TDECOR_FLAT      1
#define TDECOR_GRADIENT  2

// Read one integer metric from a theme (theme_id < 0 = the active theme).
// Returns 0 for an out-of-range id, so a newer userland asking an older
// kernel for a metric it does not have degrades to use your own default
// rather than reading garbage.
int32_t theme_get_metric_by_id(int theme_id, int metric_id);

// Convenience for kernel drawing code: the ACTIVE theme's metric.
static inline int32_t theme_metric_i(int metric_id) {
    return theme_get_metric_by_id(-1, metric_id);
}

// Get the currently active theme
const theme_t *theme_get_current(void);

// Get a specific theme by ID
const theme_t *theme_get_by_id(int theme_id);

// Get the current theme ID
int theme_get_current_id(void);
int theme_get_contrast_corrections(int theme_id);  // (themes ticket) 0 = clean file

// (#285) Resolve userland theme_color_id_t -> active theme color
uint32_t theme_get_color_by_id(int theme_id, int color_id);

// Set the active theme by ID
void theme_set(int theme_id);

// Get the number of available themes
int theme_get_count(void);

// Get theme name by ID
const char *theme_get_name(int theme_id);

// Initialize the theme system (#565: scans /THEMES/INDEX.TXT and parses each
// listed /THEMES/*.mtheme file; falls back to one hardcoded minimal palette
// if no theme files are found, e.g. before the root filesystem is mounted)
void theme_init(void);

// #565: parse one /THEMES/*.mtheme file and add it to (or update it in) the
// live theme table without a reboot. Returns the resulting index, or -1 if
// the file could not be read/parsed. Used by the userland theme loader
// (gui_theme.c) after the App Store or Settings drops a new .mtheme file.
int theme_load_file_runtime(const char *path);

// ============================================================================
// Convenience macros to access current theme colors
// ============================================================================

#define THEME_TITLEBAR_ACTIVE       (theme_get_current()->titlebar_active)
#define THEME_TITLEBAR_INACTIVE     (theme_get_current()->titlebar_inactive)
#define THEME_TITLEBAR_TEXT         (theme_get_current()->titlebar_text)
#define THEME_WINDOW_BG             (theme_get_current()->window_bg)
#define THEME_WINDOW_BORDER         (theme_get_current()->window_border)
#define THEME_CLOSE_BUTTON          (theme_get_current()->close_button)
#define THEME_CLOSE_BUTTON_HOVER    (theme_get_current()->close_button_hover)
#define THEME_MINIMIZE_BUTTON       (theme_get_current()->minimize_button)
#define THEME_MAXIMIZE_BUTTON       (theme_get_current()->maximize_button)
#define THEME_BUTTON_BG             (theme_get_current()->button_bg)
#define THEME_BUTTON_BG_HOVER       (theme_get_current()->button_bg_hover)
#define THEME_BUTTON_BG_PRESSED     (theme_get_current()->button_bg_pressed)
#define THEME_BUTTON_BORDER         (theme_get_current()->button_border)
#define THEME_BUTTON_TEXT           (theme_get_current()->button_text)
#define THEME_BUTTON_DISABLED       (theme_get_current()->button_disabled)
#define THEME_LABEL_TEXT            (theme_get_current()->label_text)
#define THEME_TEXTBOX_BG            (theme_get_current()->textbox_bg)
#define THEME_TEXTBOX_BORDER        (theme_get_current()->textbox_border)
#define THEME_TEXTBOX_TEXT          (theme_get_current()->textbox_text)
#define THEME_TEXTBOX_CURSOR        (theme_get_current()->textbox_cursor)
#define THEME_CHECKBOX_BG           (theme_get_current()->checkbox_bg)
#define THEME_CHECKBOX_BORDER       (theme_get_current()->checkbox_border)
#define THEME_CHECKBOX_CHECK        (theme_get_current()->checkbox_check)
#define THEME_DESKTOP_BG            (theme_get_current()->desktop_bg)
#define THEME_TASKBAR_BG            (theme_get_current()->taskbar_bg)
#define THEME_TASKBAR_HOVER         (theme_get_current()->taskbar_hover)
#define THEME_TASKBAR_ACTIVE        (theme_get_current()->taskbar_active)
#define THEME_START_BUTTON          (theme_get_current()->start_button)
#define THEME_GAUGE_BG              (theme_get_current()->gauge_bg)
#define THEME_GAUGE_FG              (theme_get_current()->gauge_fg)
#define THEME_MENU_BG               (theme_get_current()->menu_bg)
#define THEME_MENU_BORDER           (theme_get_current()->menu_border)
#define THEME_MENU_ITEM_HOVER       (theme_get_current()->menu_item_hover)
#define THEME_MENU_TEXT             (theme_get_current()->menu_text)
#define THEME_MENU_TEXT_DISABLED    (theme_get_current()->menu_text_disabled)
#define THEME_MENU_SEPARATOR        (theme_get_current()->menu_separator)
#define THEME_SCROLLBAR_BG          (theme_get_current()->scrollbar_bg)
#define THEME_SCROLLBAR_THUMB       (theme_get_current()->scrollbar_thumb)
#define THEME_SCROLLBAR_THUMB_HOVER (theme_get_current()->scrollbar_thumb_hover)
#define THEME_SELECTION_BG          (theme_get_current()->selection_bg)
#define THEME_SELECTION_TEXT        (theme_get_current()->selection_text)

// Status colors
#define THEME_COLOR_ERROR           (theme_get_current()->color_error)
#define THEME_COLOR_WARNING         (theme_get_current()->color_warning)
#define THEME_COLOR_SUCCESS         (theme_get_current()->color_success)
#define THEME_COLOR_INFO            (theme_get_current()->color_info)

// Link colors
#define THEME_LINK_COLOR            (theme_get_current()->link_color)
#define THEME_LINK_VISITED          (theme_get_current()->link_visited)
#define THEME_LINK_HOVER            (theme_get_current()->link_hover)

// Tooltip colors
#define THEME_TOOLTIP_BG            (theme_get_current()->tooltip_bg)
#define THEME_TOOLTIP_TEXT          (theme_get_current()->tooltip_text)
#define THEME_TOOLTIP_BORDER        (theme_get_current()->tooltip_border)

// Style access
#define THEME_STYLE                 (theme_get_current()->style)
#define THEME_IS_RETRO              (theme_get_current()->style == THEME_STYLE_RETRO)
#define THEME_IS_MODERN             (theme_get_current()->style == THEME_STYLE_MODERN)

// ============================================================================
// Typography Integration
// ============================================================================

// Notify the font system when theme changes (for render mode updates)
void theme_notify_font_system(void);

#endif // THEMES_H
