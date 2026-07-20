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
    THEME_COLOR_COUNT
} theme_color_id_t;

// ============================================================================
// Theme Metric IDs (must match kernel)
// ============================================================================

typedef enum {
    THEME_METRIC_BORDER_WIDTH = 0,
    THEME_METRIC_TITLEBAR_HEIGHT,
    THEME_METRIC_BUTTON_WIDTH,
    THEME_METRIC_BUTTON_HEIGHT,
    THEME_METRIC_SCROLLBAR_WIDTH,
    THEME_METRIC_MENU_ITEM_HEIGHT,
    THEME_METRIC_ICON_SIZE,
    THEME_METRIC_CORNER_RADIUS,
    THEME_METRIC_PADDING,
    THEME_METRIC_SPACING,
    THEME_METRIC_TAB_HEIGHT,
    THEME_METRIC_COUNT
} theme_metric_id_t;

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

// Theme metrics: sensible CDE/Motif defaults (no syscall; old id 125==GETEGID).
static inline int theme_metric(theme_metric_id_t id) {
    switch (id) {
        case THEME_METRIC_BORDER_WIDTH:     return 1;
        case THEME_METRIC_TITLEBAR_HEIGHT:  return 24;
        case THEME_METRIC_BUTTON_WIDTH:     return 20;
        case THEME_METRIC_BUTTON_HEIGHT:    return 18;
        case THEME_METRIC_SCROLLBAR_WIDTH:  return 16;
        case THEME_METRIC_MENU_ITEM_HEIGHT: return 22;
        case THEME_METRIC_ICON_SIZE:        return 32;
        case THEME_METRIC_CORNER_RADIUS:    return 0;
        case THEME_METRIC_PADDING:          return 8;
        case THEME_METRIC_SPACING:          return 6;
        case THEME_METRIC_TAB_HEIGHT:       return 24;
        default:                            return 0;
    }
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
