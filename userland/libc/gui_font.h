// gui_font.h - MayteraOS shared font picker (the ChooseFont common dialog)
//
// WHY THIS EXISTS (#351):
// The user's requirement, verbatim: "install, select and a common UI for it so i
// dont have to build a new one in each application, similar to how its done in
// windows". That is the Windows ChooseFont contract: ONE dialog, owned by the
// platform, that every application calls. No app builds a font UI again.
//
// Before this, the OS shipped exactly one font (/FONT.TTF) and Settings had only
// a four-item "Font Size" dropdown, so there was no font UI to share and nothing
// to select between. The kernel side (a /FONTS registry of real TrueType faces,
// keyed off the name table) is what makes a picker meaningful; this is the UI.
//
// USAGE - the whole API is one blocking call:
//
//     gui_font_sel_t sel = {0};
//     gui_font_sel_default(&sel);              // or preload family/style/size
//     if (gui_font_dialog(&sel)) {
//         // sel.face + sel.style_bits + sel.size are ready to draw with:
//         win_draw_text_ttf_ex(win, x, y, "Hello", sel.face, sel.size,
//                              sel.style_bits, color);
//     }
//
// The dialog is MODAL: it opens its own window, runs its own event loop, and
// returns 1 (OK) or 0 (Cancel/closed). It blocks in win_get_event(), which parks
// on the kernel wait-queue (#453), so it costs no CPU while it sits open.
//
// SELECTION MODEL - why both `face` and `style_bits` come back:
// A style is preferentially a REAL face. "DejaVu Sans Bold" is its own .ttf with
// its own outlines, so picking Bold returns that face with style_bits = 0. Only
// when a family has no real face for the requested style does the dialog fall
// back to the rasteriser's synthetic emboldening/slanting and set style_bits.
// Callers must therefore pass BOTH through to win_draw_text_ttf_ex(); using the
// face alone silently drops synthetic styles, and using style_bits alone
// double-slants a real italic.
#ifndef _GUI_FONT_H
#define _GUI_FONT_H

#include "types.h"

#define GUI_FONT_NAME_MAX   48
#define GUI_FONT_STYLE_MAX  32

// In/out selection. Zero it, or call gui_font_sel_default(), before use.
typedef struct {
    // --- in/out: the selection itself.
    char family[GUI_FONT_NAME_MAX];   // e.g. "DejaVu Sans"; empty = default face
    char style[GUI_FONT_STYLE_MAX];   // e.g. "Regular", "Bold", "Semibold Italic"
    int  size;                        // point size; <=0 selects 14

    // --- out: what to actually draw with. See "SELECTION MODEL" above.
    int  face;                        // face index for win_draw_text_ttf_ex()
    int  style_bits;                  // FONT_STYLE_* to synthesise, usually 0

    // --- in: optional presentation. NULL for sensible defaults.
    const char *title;                // dialog title    (default "Font")
    const char *preview_text;         // preview string  (default a pangram)
} gui_font_sel_t;

// Fill `sel` with the system UI font: the current /CONFIG/UIFONT.CFG selection
// if one exists, otherwise face 0 Regular at 14.
void gui_font_sel_default(gui_font_sel_t *sel);

// THE DIALOG. Modal. Returns 1 if the user pressed OK (and `sel` now holds the
// chosen family/style/size plus the resolved face/style_bits), 0 on Cancel or
// window close, in which case `sel` is untouched.
int gui_font_dialog(gui_font_sel_t *sel);

// Resolve a family+style name pair to a face index and any synthetic style bits,
// WITHOUT opening the dialog. Use this to turn a stored selection (a config
// file, a document header) back into something drawable. Returns 0 on an exact
// or synthesised match, -1 if the family is unknown (face/style_bits are then
// set to the default face, so the caller can always draw something).
int gui_font_resolve(const char *family, const char *style, int *face, int *style_bits);

// --- Install / uninstall ---------------------------------------------------
// Copy a .ttf/.otf into the system font store (/FONTS) and register it live, so
// every app can use it WITHOUT a reboot. Returns the new face index (>=0), or:
//   -1 source unreadable   -2 not a font   -3 store write failed   -4 registry full
// Already-installed files are re-registered harmlessly.
int gui_font_install(const char *src_path);

// Uninstall the font backing `face`: removes it from the registry immediately
// and deletes its file from /FONTS. Face 0 (the default UI font) is refused.
// Returns 0 on success, -1 otherwise.
int gui_font_uninstall(int face);

// --- System UI font --------------------------------------------------------
// Persist a selection as the system UI font (/CONFIG/UIFONT.CFG) and broadcast
// it so running apps restyle live rather than at the next boot. Returns 0 on
// success.
int gui_font_set_system(const gui_font_sel_t *sel);

#endif // _GUI_FONT_H
