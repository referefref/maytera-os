// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// theme.h - Runtime Theme System for MayteraOS Userland Applications
// Provides both static fallback colors and runtime theme query via syscalls
// Updated by P14 - Theme Specialist
#ifndef THEME_H
#define THEME_H

#include "syscall.h"

// ============================================================================
// Theme Color IDs (must match kernel's theme_color_id_t in gui/theme.h)
// ============================================================================

typedef enum {
    THEME_COLOR_BACKGROUND = 0,
    THEME_COLOR_FOREGROUND,
    THEME_COLOR_ACCENT,
    THEME_COLOR_SELECTION,
    THEME_COLOR_SELECTION_TEXT,
    THEME_COLOR_TITLEBAR_ACTIVE,
    THEME_COLOR_TITLEBAR_INACTIVE,
    THEME_COLOR_TITLEBAR_TEXT,
    THEME_COLOR_WINDOW_BG,
    THEME_COLOR_WINDOW_BORDER,
    THEME_COLOR_CLOSE_BUTTON,
    THEME_COLOR_CLOSE_BUTTON_HOVER,
    THEME_COLOR_MINIMIZE_BUTTON,
    THEME_COLOR_MAXIMIZE_BUTTON,
    THEME_COLOR_BUTTON_FACE,
    THEME_COLOR_BUTTON_LIGHT,
    THEME_COLOR_BUTTON_SHADOW,
    THEME_COLOR_BUTTON_DARK,
    THEME_COLOR_BUTTON_TEXT,
    THEME_COLOR_BUTTON_DISABLED,
    THEME_COLOR_LABEL_TEXT,
    THEME_COLOR_TEXTBOX_BG,
    THEME_COLOR_TEXTBOX_BORDER,
    THEME_COLOR_TEXTBOX_TEXT,
    THEME_COLOR_TEXTBOX_CURSOR,
    THEME_COLOR_CHECKBOX_BG,
    THEME_COLOR_CHECKBOX_BORDER,
    THEME_COLOR_CHECKBOX_CHECK,
    THEME_COLOR_DESKTOP_BG,
    THEME_COLOR_TASKBAR_BG,
    THEME_COLOR_TASKBAR_HOVER,
    THEME_COLOR_TASKBAR_ACTIVE,
    THEME_COLOR_START_BUTTON,
    THEME_COLOR_GAUGE_BG,
    THEME_COLOR_GAUGE_FG,
    THEME_COLOR_MENU_BG,
    THEME_COLOR_MENU_BORDER,
    THEME_COLOR_MENU_ITEM_HOVER,
    THEME_COLOR_MENU_TEXT,
    THEME_COLOR_MENU_TEXT_DISABLED,
    THEME_COLOR_MENU_SEPARATOR,
    THEME_COLOR_SCROLLBAR_BG,
    THEME_COLOR_SCROLLBAR_THUMB,
    THEME_COLOR_SCROLLBAR_THUMB_HOVER,
    THEME_COLOR_TAB_BG,
    THEME_COLOR_TAB_ACTIVE,
    THEME_COLOR_TAB_BORDER,
    THEME_COLOR_TOOLTIP_BG,
    THEME_COLOR_TOOLTIP_BORDER,
    THEME_COLOR_TOOLTIP_TEXT,
    THEME_COLOR_PROGRESS_BG,
    THEME_COLOR_PROGRESS_FG,
    // (#704) Four fields already existed in the live kernel theme_t
    // (color_error/warning/success/info, parseable from a .mtheme file's
    // legacy color_* keys via the offset table in kernel/gui/themes.c) but
    // were never reachable from userland: theme_get_color_by_id()'s switch
    // stopped at case 51 and fell through to window_bg for anything past it.
    // These four ids extend the switch rather than adding new theme_t
    // fields or new file keys, so every existing .mtheme file already has
    // real values for them. APPEND ONLY; never renumber.
    THEME_COLOR_ERROR,
    THEME_COLOR_WARNING,
    THEME_COLOR_SUCCESS,
    THEME_COLOR_INFO,
    // (#704) c_on_surface_muted existed only in the v2 semantic struct with
    // no reader anywhere (kernel or userland). Wired here for "muted/hint"
    // text (secondary labels, disabled-adjacent hints) that is not quite
    // BUTTON_DISABLED and had no other legitimate home; apps were choosing a
    // raw gray literal for exactly this instead.
    THEME_COLOR_MUTED,
    // (#745) Ink for the taskbar SURFACE. taskbar_bg is drawn by the
    // compositor's taskbar, by Settings' left nav and by solitaire's status
    // strip, and until now had no companion foreground token: text on it was
    // either derived per-app or borrowed from a token contracted against a
    // different background. APPEND ONLY; never renumber.
    THEME_COLOR_TASKBAR_TEXT,
    THEME_COLOR_TASKBAR_TEXT_MUTED,
    THEME_COLOR_TASKBAR_SELECTED_TEXT,
    // (#745) The keyboard focus ring. color.focus_ring has been parsed out of
    // every one of the 14 shipped .mtheme files into theme_t.c_focus_ring
    // since #711, and had ZERO readers: theme_get_color_by_id() had no case
    // for it, so no userland app could ask for it and the shared style engine
    // drew its ring from `accent` instead. This id is the reader.
    // APPEND ONLY; never renumber.
    THEME_COLOR_FOCUS_RING,
    // (#745) The two titlebar gradient stops (color.titlebar_top /
    // color.titlebar_bottom). Parsed since #711, decorator-only until now.
    // A theme preview that cannot read these has to invent the gradient.
    // APPEND ONLY; never renumber. Kernel side: theme_get_color_by_id() cases
    // 61 and 62 in kernel/gui/themes.c.
    THEME_COLOR_TITLEBAR_TOP,
    THEME_COLOR_TITLEBAR_BOTTOM,
    // (confirm-modal, docs/CONFIRM_MODAL_DESIGN.html) Seven more v2 tokens
    // that were already parsed into every .mtheme file's theme_t since #711
    // but had no reader in kernel/gui/themes.c's theme_get_color_by_id()
    // until this port added cases 63-69. The confirm/notice modal card is
    // the first caller (see confirmdialog.c and gui.c's gui_confirm_*): a
    // card body filled with surface_overlay, a footer band filled with
    // surface_raised one elevation step below it, ink on both
    // (on_surface/on_surface_muted - the latter already had a reader,
    // THEME_COLOR_MUTED), and a destructive/primary action button filled
    // with danger/accent under fixed on_danger/on_accent ink (floored at
    // draw time with gui_ensure_contrast(), never trusted raw - see the
    // design doc 5.1 WCAG table). APPEND ONLY; ids must match
    // kernel/gui/themes.c's switch exactly.
    THEME_COLOR_SURFACE_OVERLAY,
    THEME_COLOR_SURFACE_RAISED,
    THEME_COLOR_ON_SURFACE,
    THEME_COLOR_ON_ACCENT,
    THEME_COLOR_DANGER,
    THEME_COLOR_ON_DANGER,
    THEME_COLOR_BORDER_SUBTLE,
    THEME_COLOR_COUNT
} theme_color_id_t;

// ============================================================================
// Theme Metric IDs (must match kernel)
// ============================================================================

// (#711) mtheme v2 integer metrics. MUST match kernel gui/themes.h
// theme_metric_v2_t exactly - these ids are the SYS_THEME_METRIC wire values.
// APPEND ONLY; never renumber.
typedef enum {
    THEME_METRIC_TITLEBAR_H = 0,
    THEME_METRIC_BORDER_W,
    THEME_METRIC_BTN_H,
    THEME_METRIC_INPUT_H,
    THEME_METRIC_PAD,
    THEME_METRIC_GAP,
    THEME_METRIC_FOCUS_W,
    THEME_METRIC_WINMENU_ROWW,
    THEME_METRIC_WINMENU_ROWH,
    THEME_METRIC_WINMENU_HDR,
    THEME_METRIC_TITLEBAR_BTN,
    THEME_METRIC_TITLEBAR_BTN_GAP,
    THEME_METRIC_GRIP,
    THEME_METRIC_SCROLLBAR_W,
    THEME_METRIC_MENU_ROW_H,
    THEME_METRIC_RADIUS_BTN,
    THEME_METRIC_RADIUS_INPUT,
    THEME_METRIC_RADIUS_MENU,
    THEME_METRIC_RADIUS_CARD,
    THEME_METRIC_DECOR_STYLE,
    THEME_METRIC_DECOR_TITLEBAR_GRADIENT,
    THEME_METRIC_DECOR_GRIP,
    THEME_METRIC_TYPE_CAPTION,  THEME_METRIC_TYPE_CAPTION_LH, THEME_METRIC_TYPE_CAPTION_W,
    THEME_METRIC_TYPE_BODY,     THEME_METRIC_TYPE_BODY_LH,    THEME_METRIC_TYPE_BODY_W,
    THEME_METRIC_TYPE_TITLE,    THEME_METRIC_TYPE_TITLE_LH,   THEME_METRIC_TYPE_TITLE_W,
    THEME_METRIC_TYPE_HEADING,  THEME_METRIC_TYPE_HEADING_LH, THEME_METRIC_TYPE_HEADING_W,
    THEME_METRIC_TYPE_DISPLAY,  THEME_METRIC_TYPE_DISPLAY_LH, THEME_METRIC_TYPE_DISPLAY_W,
    THEME_METRIC_TITLE_INSET,   // #711 loop 2 (designer 1): appended, never renumber
    // #27: outer window corner chamfer extent in px (0 = square corners).
    // The kernel window decorator is the only consumer today; exposed here so
    // the two enums stay in lockstep, which is the whole ABI contract above.
    THEME_METRIC_RADIUS_WINDOW,
    THEME_METRIC_COUNT
} theme_metric_id_t;

// decor.style values (kernel gui/themes.h TDECOR_*)
#define THEME_DECOR_BEVELED   0
#define THEME_DECOR_FLAT      1
#define THEME_DECOR_GRADIENT  2

// ============================================================================
// Theme Syscall Numbers (must match kernel syscall.h)
// ============================================================================

#define SYS_THEME_GET_ACTIVE    120
#define SYS_THEME_GET_COLOR     121
#define SYS_THEME_GET_COUNT     122
#define SYS_THEME_SET_ACTIVE    123
#define SYS_THEME_GET_NAME      124
#define SYS_THEME_GET_METRIC    125

// ============================================================================
// Runtime Theme Query Functions
// ============================================================================

// (#285) Theme id namespace is the KERNEL theme id (0-11), returned by
// get_theme() (SYS_GET_THEME). The legacy SYS_THEME_* numbers (120-125) used
// here collided with the uid/gid syscalls (getuid/setuid/...), so theme_color()
// silently called setuid! These now route through SYS_THEME_COLOR(290) and
// get_theme()/set_theme(), giving every app the same colors the kernel
// decorator and compositor use.

// Get the currently active theme index (kernel theme id 0-11)
static inline int theme_get_active(void) {
    return (int)syscall0(SYS_GET_THEME);
}

// Get a color from the current theme (real theme_t color, 0x00RRGGBB)
static inline uint32_t theme_color(theme_color_id_t id) {
    return (uint32_t)syscall2(SYS_THEME_COLOR, (uint64_t)(-1), (uint64_t)id);
}

// Get a color from a specific theme id
static inline uint32_t theme_color_of(int theme_id, theme_color_id_t id) {
    return (uint32_t)syscall2(SYS_THEME_COLOR, (uint64_t)theme_id, (uint64_t)id);
}

// Number of built-in themes (kernel: THEME_DEFAULT..THEME_FLUENT_DARK)
static inline int theme_count(void) {
    return 12;
}

// Set the active theme (kernel theme id)
static inline int theme_set_active(int index) {
    return (int)syscall1(SYS_SET_THEME, (uint64_t)index);
}

// Theme name query is kernel-internal; userland keeps a static table to avoid
// the old SYS_THEME_GET_NAME(124)==GETEUID collision.
static inline int theme_get_name(int index, char *buf, int buf_size) {
    static const char *const names[12] = {
        "Retro UNIX","Dark","Light","High Contrast","Classic","Ocean",
        "Sunset","Forest","Modern Light","Modern Dark","Slate Light","Slate Dark"};
    const char *n = (index >= 0 && index < 12) ? names[index] : "Unknown";
    int i = 0;
    if (buf && buf_size > 0) {
        for (; n[i] && i < buf_size - 1; i++) buf[i] = n[i];
        buf[i] = 0;
    }
    return i;
}

// (#711) Theme metrics now come from the ACTIVE .mtheme FILE, through the same
// live kernel table SYS_THEME_COLOR reads, so a metric edit needs no rebuild.
// This replaces a hardcoded switch of eleven compiled-in constants that no
// theme file could influence. The old SYS_THEME_GET_METRIC(125) collided with
// GETEGID and is still not used; the getter is SYS_THEME_METRIC(357).
//
// A 0 return means "this kernel does not know that id"; callers that care must
// substitute their own default (see theme_metric_or()).
static inline int theme_metric(theme_metric_id_t id) {
    return (int)syscall2(SYS_THEME_METRIC, (uint64_t)(-1), (uint64_t)id);
}

// Same, from a specific theme id rather than the active one.
static inline int theme_metric_of(int theme_id, theme_metric_id_t id) {
    return (int)syscall2(SYS_THEME_METRIC, (uint64_t)theme_id, (uint64_t)id);
}

// (#wizflash) The theme's own 1x design value, with NO UI-scale multiply
// applied. theme_metric_of() above always returns the value already scaled
// for the current display (kernel/gui/themes.c: "THE GLOBAL UI SCALE FACTOR
// IS APPLIED HERE AND ONLY HERE"), which is correct for drawing straight onto
// the framebuffer but WRONG for a caller drawing through its OWN scale_on
// window: that window's draw syscalls (win_draw_rect et al) scale every
// coordinate again at the window boundary, so a pre-scaled metric passed
// through them is scaled twice. gui_theme_win_preview() (gui_theme.c) is
// exactly that caller: it wants the theme's 1x geometry and lets the window
// boundary apply the scale once, in step with the crop box it draws into.
static inline int theme_metric_raw_of(int theme_id, theme_metric_id_t id) {
    return (int)syscall2(SYS_THEME_METRIC_RAW, (uint64_t)theme_id, (uint64_t)id);
}

// Metric with a caller-supplied fallback for the 0 ("unknown id") case.
static inline int theme_metric_or(theme_metric_id_t id, int fallback) {
    int v = theme_metric(id);
    return v ? v : fallback;
}

// ============================================================================
// Convenience Macros - Query current theme colors
// These call the kernel for actual values, respecting runtime theme changes
// ============================================================================

#define THEME_WINDOW_BG             theme_color(THEME_COLOR_WINDOW_BG)
#define THEME_WINDOW_BORDER         theme_color(THEME_COLOR_WINDOW_BORDER)
#define THEME_TITLEBAR_ACTIVE       theme_color(THEME_COLOR_TITLEBAR_ACTIVE)
#define THEME_TITLEBAR_INACTIVE     theme_color(THEME_COLOR_TITLEBAR_INACTIVE)
#define THEME_TITLEBAR_TEXT         theme_color(THEME_COLOR_TITLEBAR_TEXT)
#define THEME_BUTTON_BG             theme_color(THEME_COLOR_BUTTON_FACE)
#define THEME_BUTTON_TEXT           theme_color(THEME_COLOR_BUTTON_TEXT)
#define THEME_LABEL_TEXT            theme_color(THEME_COLOR_LABEL_TEXT)
#define THEME_TEXTBOX_BG            theme_color(THEME_COLOR_TEXTBOX_BG)
#define THEME_TEXTBOX_TEXT          theme_color(THEME_COLOR_TEXTBOX_TEXT)
#define THEME_MENU_BG               theme_color(THEME_COLOR_MENU_BG)
#define THEME_MENU_TEXT             theme_color(THEME_COLOR_MENU_TEXT)
#define THEME_SELECTION_BG          theme_color(THEME_COLOR_SELECTION)
#define THEME_SELECTION_TEXT        theme_color(THEME_COLOR_SELECTION_TEXT)
#define THEME_SCROLLBAR_BG          theme_color(THEME_COLOR_SCROLLBAR_BG)
#define THEME_SCROLLBAR_THUMB       theme_color(THEME_COLOR_SCROLLBAR_THUMB)
// (#704) semantic status/hint tokens, newly reachable (see THEME_COLOR_ERROR
// et al above). Use these instead of a raw red/green/amber/gray literal for
// error text, success checks, warnings, and muted/hint captions.
#define THEME_COLOR_ERROR_C         theme_color(THEME_COLOR_ERROR)
#define THEME_COLOR_WARNING_C       theme_color(THEME_COLOR_WARNING)
#define THEME_COLOR_SUCCESS_C       theme_color(THEME_COLOR_SUCCESS)
#define THEME_COLOR_INFO_C          theme_color(THEME_COLOR_INFO)
#define THEME_COLOR_MUTED_C         theme_color(THEME_COLOR_MUTED)

// ============================================================================
// Static Fallback Colors (used if syscalls fail or for compatibility)
// These are dark theme defaults
// ============================================================================

// Background Colors
#define THEME_BG_PRIMARY        0x00282828
#define THEME_BG_SECONDARY      0x00353535
#define THEME_BG_TERTIARY       0x00404040

// Text Colors
#define THEME_TEXT_PRIMARY      0x00FFFFFF
#define THEME_TEXT_SECONDARY    0x00CCCCCC
#define THEME_TEXT_DISABLED     0x00888888

// Accent Colors
#define THEME_ACCENT            0x000066FF
#define THEME_ACCENT_HOVER      0x001177FF

// Status Colors
#define THEME_SUCCESS           0x0066FF66
#define THEME_WARNING           0x00FFCC00
#define THEME_ERROR             0x00FF6666
#define THEME_INFO              0x0066CCFF

// ============================================================================
// Terminal Colors (ANSI 16 colors - fixed across themes)
// ============================================================================

#define THEME_TERM_BLACK            0x00000000
#define THEME_TERM_RED              0x00AA0000
#define THEME_TERM_GREEN            0x0000AA00
#define THEME_TERM_YELLOW           0x00AAAA00
#define THEME_TERM_BLUE             0x000000AA
#define THEME_TERM_MAGENTA          0x00AA00AA
#define THEME_TERM_CYAN             0x0000AAAA
#define THEME_TERM_WHITE            0x00AAAAAA
#define THEME_TERM_BRIGHT_BLACK     0x00555555
#define THEME_TERM_BRIGHT_RED       0x00FF5555
#define THEME_TERM_BRIGHT_GREEN     0x0055FF55
#define THEME_TERM_BRIGHT_YELLOW    0x00FFFF55
#define THEME_TERM_BRIGHT_BLUE      0x005555FF
#define THEME_TERM_BRIGHT_MAGENTA   0x00FF55FF
#define THEME_TERM_BRIGHT_CYAN      0x0055FFFF
#define THEME_TERM_BRIGHT_WHITE     0x00FFFFFF

#define THEME_TERM_FG               THEME_TERM_BRIGHT_WHITE
#define THEME_TERM_BG               0x001E1E1E
#define THEME_TERM_CURSOR           0x0000FF00

// ============================================================================
// Calculator-specific (can query theme for these too)
// ============================================================================

#define BTN_COLOR_DIGIT     theme_color(THEME_COLOR_BUTTON_FACE)
#define BTN_COLOR_OP        0x00FF8C00
#define BTN_COLOR_CLEAR     0x00FF4040
#define BTN_COLOR_EQUALS    0x0040A040
#define BTN_COLOR_TEXT      theme_color(THEME_COLOR_BUTTON_TEXT)
#define DISPLAY_BG          theme_color(THEME_COLOR_TEXTBOX_BG)
#define DISPLAY_FG          theme_color(THEME_COLOR_TEXTBOX_TEXT)


// ============================================================================
// Missing convenience macros for slider, gauge, checkbox, button states
// ============================================================================

#ifndef THEME_SLIDER_TRACK
#define THEME_SLIDER_TRACK          theme_color(THEME_COLOR_SCROLLBAR_BG)
#define THEME_SLIDER_TRACK_H        6
#define THEME_SLIDER_FILL           theme_color(THEME_COLOR_PROGRESS_FG)
#define THEME_SLIDER_THUMB          theme_color(THEME_COLOR_SCROLLBAR_THUMB)
#define THEME_SLIDER_THUMB_HOVER    theme_color(THEME_COLOR_SCROLLBAR_THUMB_HOVER)
#define THEME_SLIDER_THUMB_SIZE     14
#define THEME_GAUGE_BG              theme_color(THEME_COLOR_GAUGE_BG)
#define THEME_GAUGE_FG              theme_color(THEME_COLOR_GAUGE_FG)
#define THEME_CHECKBOX_BORDER       theme_color(THEME_COLOR_CHECKBOX_BORDER)
#define THEME_BUTTON_BG_HOVER       theme_color(THEME_COLOR_BUTTON_LIGHT)
#define THEME_SCROLLBAR_WIDTH       16
#endif

#endif // THEME_H
