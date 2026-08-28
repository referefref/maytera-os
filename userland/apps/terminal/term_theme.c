// term_theme.c
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.

#include "term_common.h"
#include "term_grid.h"
#include "term_theme.h"
#include "term_prefs.h"

// ANSI colors table (tier 2, docs/TERMINAL_PARITY.md: TERMINAL COLOUR SCHEME,
// NOT the OS theme - see the big comment on g_term_palette_slug below before
// touching this). No longer `const`: term_resolve_palette() overwrites it
// from either the built-in classic values below (the "system"/Follow-system-
// theme default, unchanged from every terminal window before colour schemes
// existed) or a loaded /PALETTES/<slug>.tpalette file.
uint32_t ansi_colors[16] = {
    0x00000000,  // Black
    0x00AA0000,  // Red
    0x0000AA00,  // Green
    0x00AAAA00,  // Yellow
    0x000000AA,  // Blue
    0x00AA00AA,  // Magenta
    0x0000AAAA,  // Cyan
    0x00AAAAAA,  // White
    0x00555555,  // Bright Black
    0x00FF5555,  // Bright Red
    0x0055FF55,  // Bright Green
    0x00FFFF55,  // Bright Yellow
    0x005555FF,  // Bright Blue
    0x00FF55FF,  // Bright Magenta
    0x0055FFFF,  // Bright Cyan
    0x00FFFFFF   // Bright White
};
char g_term_theme_slug[GUI_THEME_SLUG_MAX] = TERM_DEFAULT_THEME_SLUG;
int  g_term_theme_index = 1;   // resolved kernel theme id; re-resolved on load/apply
gui_font_sel_t g_term_font;    // family/style/size + resolved face/style_bits
// ---- tier 2: TERMINAL COLOUR SCHEME (docs/TERMINAL_PARITY.md) -------------
// NOT the "Window Theme" above. This is the 16 ANSI colours plus default
// foreground/background/cursor/selection, in the Terminal.app/Konsole/
// Windows Terminal sense (Solarized Dark, Tango, Campbell, ...). It is
// data-driven from /PALETTES/<slug>.tpalette (userland/libc/gui_palette.h),
// never a hardcoded C table, so an App Store package can add one with no
// rebuild, the same as an .mtheme already can for window themes.
//
// GUI_PALETTE_SYSTEM_SLUG ("system", the default) means "Follow system
// theme": ansi_colors[] stays the built-in classic values (below) and the
// default fg/bg/cursor keep resolving through g_term_theme_index exactly as
// they did before this feature existed - so a pre-existing TERMPREF.CFG with
// no palette field (see term_prefs_load()'s backward-compatible parse) opens
// looking identical to how it always did.
// Cursor shape/blink live copy (profiles). Defaults reproduce EXACTLY what
// every terminal window did before profiles existed: a blinking 2px underline.
// A different default here would silently restyle every existing user's
// cursor on upgrade, which is not what adding a setting is for.
int g_term_cursor_shape = TERM_CURSOR_UNDERLINE;
int g_term_cursor_blink = 1;
char g_term_palette_slug[GUI_PALETTE_SLUG_MAX] = GUI_PALETTE_SYSTEM_SLUG;
term_palette_t g_term_palette;   // resolved colours; only meaningful when !g_term_palette_is_system
int g_term_palette_is_system = 1;
// Resolve g_term_theme_slug to a live kernel theme id via gui_theme_list()
// (the REAL installed set - /THEMES/INDEX.TXT - not the stale 12-entry
// hardcoded name table in userland/libc/theme.h). Falls back to the first
// theme flagged is_dark in the list, then to index 1, so an unresolvable or
// deleted slug still lands on SOMETHING dark rather than index 0 (Retro
// UNIX, light).
void term_resolve_theme(void) {
    gui_theme_entry_t ents[GUI_THEME_MAX_ENTRIES];
    int n = gui_theme_list(ents, GUI_THEME_MAX_ENTRIES);
    int found = -1, first_dark = -1;
    for (int i = 0; i < n; i++) {
        if (first_dark < 0 && ents[i].is_dark) first_dark = ents[i].index;
        if (strcmp(ents[i].slug, g_term_theme_slug) == 0) { found = ents[i].index; break; }
    }
    if (found >= 0) g_term_theme_index = found;
    else if (first_dark >= 0) g_term_theme_index = first_dark;
    else g_term_theme_index = 1;
}

// Resolve g_term_palette_slug (tier 2) into ansi_colors[]/g_term_palette.
// gui_palette_load() fills *out with the built-in MayteraOS Classic values
// and returns -1 for BOTH the "system" pseudo-slug AND any real failure
// (missing/deleted/unreadable file) - one code path handles "the user chose
// Follow system theme" and "the chosen scheme's file went away" identically,
// which is the correct behaviour for the latter: fall back to something
// legible, never a crash or uninitialised colours. g_term_palette_is_system
// gates whether draw_row_cell()/term_bg_color()/term_cursor_color() take the
// default fg/bg/cursor from THIS scheme or from the terminal's own Window
// Theme (g_term_theme_index) - the ANSI 16 (ansi_colors[]) are always taken
// from here regardless, since "Follow system theme" for the ANSI 16 has
// always meant "the classic 16", not "recolour them from window chrome".
void term_resolve_palette(void) {
    term_palette_t p;
    int rc = gui_palette_load(g_term_palette_slug, &p);
    g_term_palette_is_system = (rc != 0);
    for (int i = 0; i < 16; i++) ansi_colors[i] = p.ansi[i];
    g_term_palette = p;
}

uint32_t term_bg_color(void) {
    return g_term_palette_is_system ? theme_color_of(g_term_theme_index, THEME_COLOR_TEXTBOX_BG)
                                     : g_term_palette.bg;
}
uint32_t term_fg_color(void) {
    return g_term_palette_is_system ? theme_color_of(g_term_theme_index, THEME_COLOR_TEXTBOX_TEXT)
                                     : g_term_palette.fg;
}
uint32_t term_cursor_color(void) {
    return g_term_palette_is_system ? theme_color_of(g_term_theme_index, THEME_COLOR_TEXTBOX_CURSOR)
                                     : g_term_palette.cursor;
}

// Recompute term_char_w/term_char_h from g_term_font (#241). Mirrors editor's
// ed_apply_font() exactly (same advance-of-'M' / ascent-descent+linegap
// derivation): for a monospace family every glyph's advance is identical, so
// 'M' IS the cell width; a proportional family letter-spaces rather than
// overlapping, the same accepted compromise ed_apply_font() documents.
void term_apply_font(void) {
    int m[3];
    if (font_metrics(g_term_font.face, g_term_font.size, m) == 0) {
        int lh = m[0] - m[1] + m[2];   // ascent - descent + line gap
        term_char_h = (lh > 4) ? lh : g_term_font.size + 2;
        term_ascent = m[0];
    } else {
        term_char_h = g_term_font.size + 2;
        term_ascent = g_term_font.size;
    }
    font_glyph_meta_t meta;
    int adv = font_glyph(g_term_font.face, g_term_font.size, g_term_font.style_bits,
                         'M', &meta, 0, 0);
    term_char_w = (adv > 0) ? adv : (g_term_font.size * 6 / 10);
    if (term_char_w < 4) term_char_w = 4;
    if (term_char_h < 6) term_char_h = 6;
}
