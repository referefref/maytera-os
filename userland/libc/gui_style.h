// gui_style.h - MayteraOS shared widget-style engine (Phase 0-2)
// A style-aware UI primitive layer used by all apps. A theme selects a
// renderer family (classic / modern / flat) plus a semantic palette; the
// primitives below render accordingly. See the internal UI primitives design notes.
#ifndef _GUI_STYLE_H
#define _GUI_STYLE_H

#include "types.h"

// --- Renderer families -----------------------------------------------------
typedef enum {
    GUI_STYLE_CLASSIC = 0,  // CDE/Motif: beveled, square corners, no gradients
    GUI_STYLE_MODERN  = 1,  // rounded, gradient, soft elevation shadows
    GUI_STYLE_FLAT    = 2   // minimal: flat fill, thin border, no bevel/gradient
} gui_base_style_t;

// --- Button variants -------------------------------------------------------
typedef enum {
    GUI_BTN_PRIMARY   = 0,  // accent fill, contrast ink
    GUI_BTN_SECONDARY = 1,  // surface fill, ink
    GUI_BTN_GHOST     = 2   // no fill, accent ink
} gui_btn_variant_t;

// --- Control states --------------------------------------------------------
typedef enum {
    GUI_ST_NORMAL   = 0,
    GUI_ST_HOVER    = 1,
    GUI_ST_PRESSED  = 2,
    GUI_ST_FOCUS    = 3,
    GUI_ST_DISABLED = 4
} gui_state_t;

// --- Design tokens ---------------------------------------------------------
#define GUI_RADIUS       6
#define GUI_PAD          10
#define GUI_GAP          8
#define GUI_FOCUS_W      2
#define GUI_TTF_SIZE     14

// --- Semantic palette (filled by the app from its active theme) ------------
typedef struct {
    uint32_t surface;        // window / content background
    uint32_t surface_raised; // card / panel background
    uint32_t ink;            // primary text
    uint32_t ink_dim;        // secondary text
    uint32_t accent;         // accent / selection
    uint32_t accent_hover;   // accent, hovered
    uint32_t border;         // outlines
    uint32_t field_bg;       // input background
    uint32_t field_border;   // input outline
    uint32_t track;          // slider/scrollbar track
} gui_palette_t;

// --- Active style descriptor ----------------------------------------------
typedef struct {
    gui_base_style_t base;
    int  radius;       // corner radius for modern/flat (0 = square)
    bool gradients;
    bool shadows;
} ui_style_t;

// Style + palette management (one source of truth in libc).
void        gui_set_style(gui_base_style_t base);   // sensible defaults per family
ui_style_t  gui_active_style(void);
void        gui_set_palette(const gui_palette_t *p);
gui_palette_t *gui_pal(void);

// --- Foundation drawing helpers -------------------------------------------
uint32_t gui_mix(uint32_t a, uint32_t b, int t);     // blend a->b, t in 0..255
uint32_t gui_lighten(uint32_t c, int amt);
uint32_t gui_darken(uint32_t c, int amt);
uint32_t gui_ink_on(uint32_t bg);                    // black/white for contrast
int      gui_ttf_width(const char *s, int size);
void     gui_text_ttf_centered(int handle, int x, int y, int w, int h,
                               const char *s, uint32_t color, int size);
void     gui_fill_rounded(int handle, int x, int y, int w, int h, int r, uint32_t color);
void     gui_fill_rounded_grad(int handle, int x, int y, int w, int h, int r,
                               uint32_t top, uint32_t bottom);
void     gui_rounded_border(int handle, int x, int y, int w, int h, int r, uint32_t color);
void     gui_soft_shadow(int handle, int x, int y, int w, int h, int r, uint32_t bg);
// Antialiased rounded fill / circle: edge pixels blend the fill color toward the
// caller-supplied background (no framebuffer read-back). r==0 falls back to a rect.
void     gui_fill_rounded_aa(int handle, int x, int y, int w, int h, int r,
                             uint32_t color, uint32_t bg);
void     gui_fill_circle_aa(int handle, int x, int y, int d, uint32_t color, uint32_t bg);

// --- Style-aware primitives (4 states each) -------------------------------
void gui_button(int handle, int x, int y, int w, int h, const char *label,
                gui_btn_variant_t variant, gui_state_t st);
void gui_checkbox(int handle, int x, int y, int sz, bool checked,
                  const char *label, gui_state_t st);
void gui_toggle(int handle, int x, int y, int w, int h, bool on, gui_state_t st);
void gui_slider(int handle, int x, int y, int w, int value, int max_val, gui_state_t st);
void gui_textfield2(int handle, int x, int y, int w, int h, const char *text, bool focused);
void gui_progress(int handle, int x, int y, int w, int h, int pct);
void gui_card(int handle, int x, int y, int w, int h);

#endif // _GUI_STYLE_H
