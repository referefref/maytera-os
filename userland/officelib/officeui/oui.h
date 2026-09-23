// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// oui.h - PRIVATE helpers shared by the office UI toolkit widgets (otoolbar,
// ocombo, omodal, otabs, othumbs, oruler, ofxbar). Not part of the app-facing
// API: apps include the per-widget headers (or officetk.h), never this.
//
// Everything here exists so that the seven widgets read the SAME theme
// metric, floor the SAME muted ink the SAME way, and centre text with ONE
// rule (docs/OFFICE_UI_DESIGN.md sections 1.2/1.3), instead of each file
// re-deriving them and drifting. All colours come from theme_color() at draw
// time and are contrast-repaired with gui_ensure_contrast(); no widget takes a
// colour argument (spec section 8, conventions).
#ifndef OFFICE_OUI_H
#define OFFICE_OUI_H

#include "../../libc/gui.h"          // types/syscall/keys/gui_mods/gui_style/textfield
#include "../../libc/gui_list.h"
#include "../../libc/theme.h"

// --- Live theme metrics (retro / modern fallbacks from the spec) ------------
static inline int oui_body(void)     { return theme_metric_or(THEME_METRIC_TYPE_BODY, 14); }
static inline int oui_caption(void)  { return theme_metric_or(THEME_METRIC_TYPE_CAPTION, 11); }
static inline int oui_heading(void)  { return theme_metric_or(THEME_METRIC_TYPE_HEADING, 20); }
static inline int oui_btn_h(void)    { return theme_metric_or(THEME_METRIC_BTN_H, 24); }
static inline int oui_input_h(void)  { return theme_metric_or(THEME_METRIC_INPUT_H, 24); }
static inline int oui_row_h(void)    { return theme_metric_or(THEME_METRIC_MENU_ROW_H, 22); }

// Theme colour with a fallback for a kernel that answers 0 for the id.
static inline uint32_t oui_tok(theme_color_id_t id, uint32_t fallback) {
    uint32_t c = theme_color(id);
    return c ? c : fallback;
}

// The ground tokens every strip uses (spec 1.1).
static inline uint32_t oui_surface(void)        { return oui_tok(THEME_COLOR_WINDOW_BG,       0x00B4B4B4); }
static inline uint32_t oui_raised(void)         { return oui_tok(THEME_COLOR_SURFACE_RAISED,  0x00C8C8C8); }
static inline uint32_t oui_overlay(void)        { return oui_tok(THEME_COLOR_SURFACE_OVERLAY, 0x00D4D4D4); }
static inline uint32_t oui_sunken(void)         { return oui_tok(THEME_COLOR_TEXTBOX_BG,      0x00FFFFFF); }
static inline uint32_t oui_on_surface(void)     { return oui_tok(THEME_COLOR_ON_SURFACE,      0x00000000); }
static inline uint32_t oui_border_subtle(void)  { return oui_tok(THEME_COLOR_BORDER_SUBTLE,   0x00808080); }
static inline uint32_t oui_border_strong(void)  { return oui_tok(THEME_COLOR_WINDOW_BORDER,   0x00404040); }
static inline uint32_t oui_accent(void)         { return oui_tok(THEME_COLOR_ACCENT,          0x00336666); }
static inline uint32_t oui_focus_color(void)    { return oui_tok(THEME_COLOR_FOCUS_RING,      0x002A5454); }

// Muted ink FLOORED against the ground it sits on. This is the spec's
// contrast fix (section 0): THEME_COLOR_MUTED on SURFACE_RAISED is 2.36:1 on
// retro_unix raw, so every muted ink in the toolkit passes through here.
static inline uint32_t oui_muted_on(uint32_t bg) {
    uint32_t m = oui_tok(THEME_COLOR_MUTED, 0x00808080);
    return gui_ensure_contrast(m, bg, GUI_FLOOR_TEXT);
}
// Primary ink, also floored (a theme can author on_surface too close to a
// raised/overlay ground; the repair is a no-op on every shipped theme).
static inline uint32_t oui_ink_on(uint32_t bg) {
    return gui_ensure_contrast(oui_on_surface(), bg, GUI_FLOOR_TEXT);
}
// Glyph (non-text) ink: 3:1 floor.
static inline uint32_t oui_glyph_on(uint32_t ink, uint32_t bg) {
    return gui_ensure_contrast(ink, bg, GUI_FLOOR_NONTEXT);
}

// Vertical centring of a TTF line in a box: the size argument IS the
// ascent-descent extent (gui_list.c's rule), so centre on size, not on a
// nominal line height.
static inline int oui_text_y(int box_y, int box_h, int size) {
    return box_y + (box_h - size) / 2;
}

// 2 px focus ring at 2 px outside a control (spec 3.4).
void oui_focus_ring(int win, int x, int y, int w, int h);

// Bold ("body_strong") text: a real bold face resolved once through
// gui_font_resolve() from the UI font's family, synthetic bold as fallback.
void oui_text_bold(int win, int x, int y, const char *s, int size, uint32_t ink);
int  oui_bold_width(const char *s, int size);
void oui_text_bold_centered(int win, int x, int y, int w, int h, const char *s, int size, uint32_t ink);

// Ellipsize `s` by MEASUREMENT (gui_ttf_render_width, the width function that
// matches win_draw_text_ttf) so it fits max_w at `size`. Never a clip.
void oui_ellipsize(char *out, int cap, const char *s, int size, int max_w);

// The interlaced-scanline scrim gui_confirm/Open/Save use (no framebuffer
// read-back exists, so a modal dims the page beneath it this way).
void oui_scrim(int win, int w, int h);

// Small helpers so no widget re-implements them.
static inline int oui_in(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}
void oui_copy(char *dst, int cap, const char *src);
int  oui_itoa(int v, char *out, int cap);

#endif // OFFICE_OUI_H
