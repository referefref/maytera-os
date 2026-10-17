// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// SLIDES - MayteraOS native office suite presentation viewer/editor. Built on
// the shared office_app shell (officelib/officeui) and the shared office UI
// toolkit (officelib/officeui/officetk.h: otoolbar, othumbs, omodal). Opens/
// saves .pptx/.odp (pptx_save/odp_save) and reads legacy .ppt read-only
// (ppt_ole_load, via office_open_presentation()).
//
// Chrome (docs/OFFICE_UI_DESIGN.md 2.3 / 5.3): the Slides toolbar
// (OFNEW OFOPEN OFSAVE | SLNEW SLLAYOUT | OFTEXTBX OFIMAGE | OFBOLD OFITALIC |
// SLPREV SLNEXT SLPLAY), the 168 px othumbs strip (16:9 thumbnails, numbered,
// current slide ringed, wheel/keys/click), the canvas with the current slide
// fit to the viewport, shape selection (accent ring + handles), and the
// omodal dialogs: Slide Layout picker (2x2 tiles), New Slide (same picker,
// "Add"), Insert Image (titled open dialog) and the three-button unsaved-
// changes prompt. Present mode (F5 / SLPLAY) is a full-window slideshow:
// the window maximises, the slide is drawn fit-to-window on black over the
// WHOLE window (menu bar, toolbar and status bar included), Right/Space/PgDn
// advance, Left/PgUp go back, Esc leaves.
//
// Rendering the slide: title/text-box shapes with bulleted paragraphs,
// SHP_RECT fills, SHP_IMAGE images. pptx.c/odp.c do not populate shape
// x/y/w/h on import today, so a shape with no geometry is auto-flowed top-
// to-bottom instead of positioned; shapes this app creates (layout
// placeholders, text boxes, images) DO carry geometry in slide px units.
#include "../../libc/gui.h"
#include "../../libc/gui_menu.h"
#include "../../libc/theme.h"
#include "../../libc/stdio.h"
#include "../../officelib/include/officeui.h"
#include "../../officelib/officeui/officetk.h"
#include "../../officelib/include/formats.h"
#include "../../libc/contract.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define PAD_PX      24
#define GAP_PX      10
#define CANVAS_INSET 16          // spec 2.3: sheet fit to (area - 32 px)
#define SLIDES_MAX_IMG   16
#define SLIDES_MAX_SHAPES 64     // per-slide hit-test rect cache
#define SLIDES_MAX_LAYOUT 256    // per-slide layout override cache

// --- Toolbar item ids (delivered as OAPP_EVENT_TOOL) ---------------------------
enum { T_NEW = 1, T_OPEN, T_SAVE, T_SLNEW, T_LAYOUT, T_TEXTBOX, T_IMAGE,
       T_BOLD, T_ITAL, T_PREV, T_NEXT, T_PLAY };

// --- App menu ids (>OAPP_MID_RESERVED_MAX; delivered as OAPP_EVENT_MENU) -------
enum { M_EDIT_UNDO = 20, M_EDIT_REDO, M_EDIT_CUT, M_EDIT_COPY, M_EDIT_PASTE,
       M_EDIT_DUP_SLIDE, M_EDIT_DEL_SLIDE,
       M_INS_NEW_SLIDE = 30, M_INS_TEXTBOX, M_INS_IMAGE,
       M_FMT_LAYOUT = 40, M_FMT_BOLD, M_FMT_ITAL,
       M_VIEW_THUMBS = 50, M_VIEW_PRESENT };

// Forward declarations: TB_ITEMS/FILE_ITEMS/EDIT_ITEMS/INSERT_ITEMS/
// FORMAT_ITEMS just below now carry these as their own .afn (task 3 phase 4,
// otoolbar.h/gui_menu.h), so they must be declared before the tables; bodies
// are defined later, unchanged from when they were reached only through
// SLIDES_ITEMS[].
static int run_file_new(const ct_item_t *it, int argc, char **argv, char *out, int ocap);
static int run_file_open(const ct_item_t *it, int argc, char **argv, char *out, int ocap);
static int run_file_save(const ct_item_t *it, int argc, char **argv, char *out, int ocap);
static int run_file_save_as(const ct_item_t *it, int argc, char **argv, char *out, int ocap);
static int run_file_close(const ct_item_t *it, int argc, char **argv, char *out, int ocap);
static int run_new_slide_dialog(const ct_item_t *it, int argc, char **argv, char *out, int ocap);
static int run_layout_dialog(const ct_item_t *it, int argc, char **argv, char *out, int ocap);
static int run_duplicate_slide(const ct_item_t *it, int argc, char **argv, char *out, int ocap);
static int run_delete_slide(const ct_item_t *it, int argc, char **argv, char *out, int ocap);
static int run_insert_textbox(const ct_item_t *it, int argc, char **argv, char *out, int ocap);
static int run_insert_image(const ct_item_t *it, int argc, char **argv, char *out, int ocap);
static int run_toggle_bold(const ct_item_t *it, int argc, char **argv, char *out, int ocap);
static int run_toggle_italic(const ct_item_t *it, int argc, char **argv, char *out, int ocap);
static int run_present(const ct_item_t *it, int argc, char **argv, char *out, int ocap);

static const ct_terms_t k_t_newdoc = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW, 0, "doc.has_path", 0
};
static const ct_terms_t k_t_save_to = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_FS, CT_FAIL_IO | CT_FAIL_NOSPACE, "doc.path", 0
};
static const ct_terms_t k_t_open_path = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_DOC,
    CT_FAIL_NOTFOUND | CT_FAIL_IO | CT_FAIL_UNSUPPORTED, "doc.path", 0
};
static const ct_terms_t k_t_slide_edit = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_DOC, CT_FAIL_NOSPACE, "doc.slide_count", 0
};
static const ct_terms_t k_t_slide_nav = {
    CT_TIME_INSTANT, 0, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW, CT_FAIL_RANGE, "doc.current_slide", 0
};
static const ct_terms_t k_t_shape_edit = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_DOC, CT_FAIL_NEEDSLIVE, "shape.count", 0
};
static const ct_terms_t k_t_shape_select = {
    CT_TIME_INSTANT, 0, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW, CT_FAIL_RANGE | CT_FAIL_NEEDSLIVE, "shape.selected", 0
};
static const ct_terms_t k_t_fmt = {
    CT_TIME_INSTANT, 0, 0, CT_SUCC_ITEM, CT_DEPTH_DOC, CT_FAIL_NEEDSLIVE, "shape.bold", 0
};
// Dialog-opening actions: MODAL. Refused on the spawn path, allowed against a
// live instance where a person can answer the dialog.
static const ct_terms_t k_t_file_dialog = {
    CT_TIME_SLOW, CT_TF_MODAL | CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_FS,
    CT_FAIL_IO | CT_FAIL_NOTFOUND, "doc.path", 0
};
static const ct_terms_t k_t_file_close = {
    CT_TIME_FAST, CT_TF_MODAL | CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW, 0, "doc.has_path", 0
};
static const ct_terms_t k_t_modal_dialog = {
    CT_TIME_SLOW, CT_TF_MODAL | CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_DOC,
    CT_FAIL_IO | CT_FAIL_UNSUPPORTED, "doc.slide_count", 0
};
static const ct_terms_t k_t_insert_image = {
    CT_TIME_SLOW, CT_TF_MODAL | CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_DOC,
    CT_FAIL_IO | CT_FAIL_UNSUPPORTED, "shape.count", 0
};
// See the block comment above: no bound exists, refused on spawn, armed on
// live (the call will usually time out while presentation mode runs on).
static const ct_terms_t k_t_present = {
    CT_TIME_UNBOUNDED, CT_TF_ASYNC, 0, CT_SUCC_ARMED, CT_DEPTH_VIEW, 0, "presenting", 0
};

// otb_item_t now carries the contract exposure too (task 3 phase 4): a
// row's .tok/.afn/.desc/.terms are read by slides_project() below exactly as
// .icon/.tooltip are read by otb_draw(), the SAME table handle_tool()
// dispatches from via otb_ctl_dispatch(). T_PREV/T_NEXT stay tok==0/afn==0
// (special-cased in handle_tool: they move RELATIVE to the current slide,
// which is not what slide.goto's absolute-index afn does).
static const otb_item_t TB_ITEMS[] = {
    { OTB_BUTTON, T_NEW,     "OFNEW",    "New (Ctrl+N)",             0, 0,
      .tok = "file.new", .afn = run_file_new,
      .desc = "Discard the current presentation (after an unsaved-changes prompt if dirty) and start a new blank one",
      .terms = &k_t_newdoc },
    { OTB_BUTTON, T_OPEN,    "OFOPEN",   "Open (Ctrl+O)",            0, 0,
      .tok = "file.open", .afn = run_file_open,
      .desc = "Raise the interactive Open dialog (.pptx/.odp/.ppt) and load the chosen file",
      .terms = &k_t_file_dialog },
    { OTB_BUTTON, T_SAVE,    "OFSAVE",   "Save (Ctrl+S)",            0, 0,
      .tok = "file.save", .afn = run_file_save,
      .desc = "Save to the current path, or raise Save As when the presentation has none yet",
      .terms = &k_t_file_dialog },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_BUTTON, T_SLNEW,   "SLNEW",    "New Slide... (Ctrl+M)",    0, 0,
      .tok = "slide.new_dialog", .afn = run_new_slide_dialog,
      .desc = "Raise the Slide Layout picker and insert a new slide with the chosen layout after the active one",
      .terms = &k_t_modal_dialog },
    { OTB_BUTTON, T_LAYOUT,  "SLLAYOUT", "Slide Layout...",          0, 0,
      .tok = "slide.layout_dialog", .afn = run_layout_dialog,
      .desc = "Raise the Slide Layout picker and apply the chosen layout to the active slide",
      .terms = &k_t_modal_dialog },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_BUTTON, T_TEXTBOX, "OFTEXTBX", "Text Box",                 0, 0,
      .tok = "insert.textbox", .afn = run_insert_textbox,
      .desc = "Add a text box to the active slide (initial text \"Text box\") and select it",
      .terms = &k_t_shape_edit },
    { OTB_BUTTON, T_IMAGE,   "OFIMAGE",  "Insert Image...",          0, 0,
      .tok = "insert.image", .afn = run_insert_image, .risk = CT_GUARDED, .cap = "fs.read",
      .desc = "Raise the Insert Image file picker and embed the chosen PNG/JPEG/BMP, centred at 50% slide width",
      .terms = &k_t_insert_image },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_TOGGLE, T_BOLD,    "OFBOLD",   "Bold (Ctrl+B)",            0, 0,
      .tok = "format.toggle_bold", .afn = run_toggle_bold,
      .desc = "Toggle bold across every run of the selected text shape (Ctrl+B)", .terms = &k_t_fmt },
    { OTB_TOGGLE, T_ITAL,    "OFITALIC", "Italic (Ctrl+I)",          0, 0,
      .tok = "format.toggle_italic", .afn = run_toggle_italic,
      .desc = "Toggle italic across every run of the selected text shape (Ctrl+I)", .terms = &k_t_fmt },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_BUTTON, T_PREV,    "SLPREV",   "Previous Slide",           0, 0 },
    { OTB_BUTTON, T_NEXT,    "SLNEXT",   "Next Slide",               0, 0 },
    { OTB_BUTTON, T_PLAY,    "SLPLAY",   "Start Presentation (F5)",  0, 0,
      .tok = "view.present", .afn = run_present,
      .desc = "Start full-screen presentation mode from the active slide (F5); blocks until a person exits it",
      .terms = &k_t_present },
};
#define TB_N ((int)(sizeof(TB_ITEMS) / sizeof(TB_ITEMS[0])))

// Menu tables are static (not const) so the View > Thumbnails check mark and
// the Format > Bold/Italic enabled state can follow the live state.
// New/Open/Save are tok == 0: TB_ITEMS owns file.new/open/save, and the
// shell intercepts OAPP_MID_FILE_NEW/OPEN/SAVE/SAVE_AS/CLOSE before
// handle_menu() runs anyway (same reasoning as writer/sheets' FILE_ITEMS).
static gui_menu_item_t FILE_ITEMS[] = {
    { "New",        "Ctrl+N", OAPP_MID_FILE_NEW,     true, false },
    { "Open...",    "Ctrl+O", OAPP_MID_FILE_OPEN,    true, false },
    { "Save",       "Ctrl+S", OAPP_MID_FILE_SAVE,    true, false },
    { "Save As...", 0,        OAPP_MID_FILE_SAVE_AS, true, false,
      .tok = "file.save_as", .afn = run_file_save_as,
      .desc = "Raise the Save As dialog regardless of whether the presentation already has a path",
      .terms = &k_t_file_dialog },
    { 0, 0, 0, false, false },
    { "Close",      0,        OAPP_MID_FILE_CLOSE,   true, false,
      .tok = "file.close", .afn = run_file_close,
      .desc = "Close the presentation (after an unsaved-changes prompt if dirty) back to a new blank one",
      .terms = &k_t_file_close },
};
// Undo/Redo/Cut/Copy/Paste stay enabled==false with no afn: handle_menu() has
// no case for any of M_EDIT_UNDO/REDO/CUT/COPY/PASTE at all (unimplemented,
// not merely unwired), so leaving them inert here is not a regression.
static gui_menu_item_t EDIT_ITEMS[] = {
    { "Undo",  "Ctrl+Z", M_EDIT_UNDO,  false, false },
    { "Redo",  "Ctrl+Y", M_EDIT_REDO,  false, false },
    { 0, 0, 0, false, false },
    { "Cut",   "Ctrl+X", M_EDIT_CUT,   false, false },
    { "Copy",  "Ctrl+C", M_EDIT_COPY,  false, false },
    { "Paste", "Ctrl+V", M_EDIT_PASTE, false, false },
    { 0, 0, 0, false, false },
    { "Duplicate Slide", 0, M_EDIT_DUP_SLIDE, true, false,
      .tok = "slide.duplicate", .afn = run_duplicate_slide,
      .desc = "Duplicate the active slide (title and shapes) and select the copy", .terms = &k_t_slide_edit },
    { "Delete Slide",    0, M_EDIT_DEL_SLIDE, true, false,
      .tok = "slide.delete", .afn = run_delete_slide,
      .desc = "Delete the active slide (a presentation always keeps at least one)", .terms = &k_t_slide_edit },
};
// tok == 0 throughout: TB_ITEMS owns slide.new_dialog/insert.textbox/
// insert.image, but M_INS_* reach handle_menu() live, so each still needs
// its own .afn.
static gui_menu_item_t INSERT_ITEMS[] = {
    { "New Slide...", "Ctrl+M", M_INS_NEW_SLIDE, true, false, .afn = run_new_slide_dialog, .terms = &k_t_modal_dialog },
    { "Text Box",     0,        M_INS_TEXTBOX,   true, false, .afn = run_insert_textbox,   .terms = &k_t_shape_edit },
    { "Image...",     0,        M_INS_IMAGE,     true, false, .afn = run_insert_image,     .terms = &k_t_insert_image },
};
// tok == 0 throughout: TB_ITEMS owns slide.layout_dialog/format.toggle_bold/
// italic, but M_FMT_* reach handle_menu() live, same reasoning as above.
static gui_menu_item_t FORMAT_ITEMS[] = {
    { "Slide Layout...", 0,        M_FMT_LAYOUT, true,  false, .afn = run_layout_dialog,  .terms = &k_t_modal_dialog },
    { 0, 0, 0, false, false },
    { "Bold",            "Ctrl+B", M_FMT_BOLD,   false, false, .afn = run_toggle_bold,    .terms = &k_t_fmt },
    { "Italic",          "Ctrl+I", M_FMT_ITAL,   false, false, .afn = run_toggle_italic,  .terms = &k_t_fmt },
};
// Thumbnails is a VALUE row (view.thumbnails, CT_RW), special-cased in
// handle_menu() like writer's view.ruler. Start Presentation's afn is wired
// for the same M_VIEW_PRESENT-reaches-handle_menu()-live reason as above;
// tok stays 0 since TB_ITEMS' T_PLAY already owns view.present.
static gui_menu_item_t VIEW_ITEMS[] = {
    { "Thumbnails",         0,    M_VIEW_THUMBS,  true, true },
    { 0, 0, 0, false, false },
    { "Start Presentation", "F5", M_VIEW_PRESENT, true, false, .afn = run_present, .terms = &k_t_present },
};
static const gui_menu_t MENUS[] = {
    { "File",   FILE_ITEMS,   6 },
    { "Edit",   EDIT_ITEMS,   9 },
    { "Insert", INSERT_ITEMS, 3 },
    { "Format", FORMAT_ITEMS, 4 },
    { "View",   VIEW_ITEMS,   3 },
};

// --- Slide layouts (spec 5.3: Title Slide, Title and Content, Two Content, Blank)
enum { LAY_TITLE = 0, LAY_TITLE_CONTENT, LAY_TWO_CONTENT, LAY_BLANK, LAY_N };
static const char *LAYOUT_NAMES[LAY_N] = { "Title Slide", "Title and Content", "Two Content", "Blank" };

// Placeholder geometry in 1/1000 of the slide size.
typedef struct { shape_type type; int fx, fy, fw, fh; } lay_ph_t;
static const lay_ph_t LAY_TITLE_PH[]   = { { SHP_TITLE, 80, 300, 840, 190 }, { SHP_TEXTBOX, 150, 540, 700, 150 } };
static const lay_ph_t LAY_TC_PH[]      = { { SHP_TITLE, 60, 50, 880, 160 },  { SHP_TEXTBOX, 60, 250, 880, 650 } };
static const lay_ph_t LAY_TWO_PH[]     = { { SHP_TITLE, 60, 50, 880, 160 },  { SHP_TEXTBOX, 60, 250, 420, 650 }, { SHP_TEXTBOX, 520, 250, 420, 650 } };
static const lay_ph_t *LAYOUT_PH[LAY_N] = { LAY_TITLE_PH, LAY_TC_PH, LAY_TWO_PH, 0 };
static const int        LAYOUT_NPH[LAY_N] = { 2, 2, 3, 0 };

// --- State ----------------------------------------------------------------------
static office_app   *g_app;
static presentation *g_pres;
static char          g_path[512];
static int           g_has_path;
static int           g_readonly;
static int           g_cur_slide;
static int           g_sel_shape = -1;        // selected shape on the current slide, -1 none
static int           g_show_thumbs = 1;
static int           g_presenting;
static int           g_quit;
static int           g_mouse_x, g_mouse_y;    // last pointer position (wheel events carry no x)
static otb_toolbar_t g_tb;
static othumbs_t     g_thumbs;
static int           g_layout_of[SLIDES_MAX_LAYOUT];   // per-slide layout override, -1 = infer

// Geometry cached by relayout(), reused by draw()/navigation/hit-testing.
static int g_strip_x, g_strip_y, g_strip_w, g_strip_h;
static int g_view_x, g_view_y, g_view_w, g_view_h;
static int g_slide_x, g_slide_y, g_slide_w, g_slide_h;
static double g_scale;
typedef struct { int x, y, w, h; } rect_t;
static rect_t g_shape_rc[SLIDES_MAX_SHAPES];  // per-shape window rects from the last canvas draw
static int    g_shape_rc_n;

typedef struct { void *px; int w, h; int shape_idx; } img_cache_ent_t;
static img_cache_ent_t g_imgcache[SLIDES_MAX_IMG];
static int g_imgcache_n;

static void sync_chrome(void);
static void relayout(void);

// --- Small helpers --------------------------------------------------------------
static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

static int has_ext_ci(const char *path, const char *ext) {
    int pl = (int)strlen(path), el = (int)strlen(ext);
    if (pl < el) return 0;
    return strcasecmp(path + pl - el, ext) == 0;
}

static int native_w(void) { return (g_pres && g_pres->cx > 0) ? g_pres->cx : 960; }
static int native_h(void) { return (g_pres && g_pres->cy > 0) ? g_pres->cy : 720; }

static slide *cur_slide(void) {
    if (!g_pres || g_cur_slide < 0 || g_cur_slide >= g_pres->nslide) return 0;
    return &g_pres->slides[g_cur_slide];
}

static shape *sel_shape(void) {
    slide *sl = cur_slide();
    if (!sl || g_sel_shape < 0 || g_sel_shape >= sl->nshape) return 0;
    return &sl->shapes[g_sel_shape];
}

static int shape_is_text(const shape *sh) { return sh && (sh->type == SHP_TITLE || sh->type == SHP_TEXTBOX); }

static int shape_has_text(const shape *sh) {
    if (!shape_is_text(sh)) return 0;
    for (int k = 0; k < sh->npara; k++)
        for (int r = 0; r < sh->paras[k].nrun; r++)
            if (sh->paras[k].runs[r].text && sh->paras[k].runs[r].text[0]) return 1;
    return 0;
}

static void para_concat(const doc_para *p, char *out, int cap) {
    int o = 0; out[0] = 0;
    if (!p) return;
    for (int i = 0; i < p->nrun; i++) {
        const char *t = p->runs[i].text;
        if (!t) continue;
        for (int j = 0; t[j] && o < cap - 1; j++) out[o++] = t[j];
    }
    out[o] = 0;
}

static void shape_text(const shape *sh, char *out, int cap) {
    out[0] = 0;
    for (int k = 0; k < sh->npara; k++) {
        char one[256]; para_concat(&sh->paras[k], one, sizeof(one));
        if (!one[0]) continue;
        if (out[0]) strncat(out, " ", (size_t)cap - strlen(out) - 1);
        strncat(out, one, (size_t)cap - strlen(out) - 1);
    }
}

static uint32_t run_color(const doc_runfmt *f, uint32_t default_ink) {
    if (!f) return default_ink;
    if (f->color == 0xFFFFFFFF) return default_ink;
    return f->color;
}

static int run_style(const doc_runfmt *f) {
    if (!f) return 0;
    return (f->bold ? FONT_STYLE_BOLD : 0) | (f->italic ? FONT_STYLE_ITALIC : 0);
}

// The first run's format of a text shape (what the Bold/Italic toggles reflect).
static const doc_runfmt *shape_first_fmt(const shape *sh) {
    if (!shape_is_text(sh)) return 0;
    for (int k = 0; k < sh->npara; k++) if (sh->paras[k].nrun > 0) return &sh->paras[k].runs[0].fmt;
    return 0;
}

static void set_dirty(void) { oapp_set_dirty(g_app, 1); }

// Model release helpers. slidemodel.c has no pres_remove_slide()/
// slide_remove_shape(); its per-slide teardown is static inside pres_free(),
// so the two are mirrored here (same ownership rules: a SHP_IMAGE owns its
// doc_image and its bytes; paras own their runs' text).
static void shape_release(shape *sh) {
    if (!sh) return;
    for (int k = 0; k < sh->npara; k++) {
        for (int r = 0; r < sh->paras[k].nrun; r++) free(sh->paras[k].runs[r].text);
        free(sh->paras[k].runs);
    }
    free(sh->paras);
    if (sh->image) { free(sh->image->bytes); free(sh->image); }
    memset(sh, 0, sizeof(*sh));
}

static void slide_release(slide *s) {
    if (!s) return;
    free(s->title);
    for (int j = 0; j < s->nshape; j++) shape_release(&s->shapes[j]);
    free(s->shapes);
    memset(s, 0, sizeof(*s));
}

static void slide_remove_shape(slide *s, int idx) {
    if (!s || idx < 0 || idx >= s->nshape) return;
    shape_release(&s->shapes[idx]);
    memmove(&s->shapes[idx], &s->shapes[idx + 1], (size_t)(s->nshape - idx - 1) * sizeof(shape));
    s->nshape--;
}

// Adds a run of text to a text shape as a new paragraph (used by Duplicate).
static doc_para *shape_add_para(shape *sh) {
    doc_para *np = (doc_para *)realloc(sh->paras, (size_t)(sh->npara + 1) * sizeof(doc_para));
    if (!np) return 0;
    sh->paras = np;
    doc_para *p = &sh->paras[sh->npara++];
    memset(p, 0, sizeof(*p));
    p->style_id = -1; p->list_level = -1;
    return p;
}

static int shape_copy(shape *dst, const shape *src) {
    memset(dst, 0, sizeof(*dst));
    dst->type = src->type; dst->x = src->x; dst->y = src->y; dst->w = src->w; dst->h = src->h; dst->fill = src->fill;
    for (int k = 0; k < src->npara; k++) {
        const doc_para *sp = &src->paras[k];
        doc_para *dp = shape_add_para(dst);
        if (!dp) return 0;
        dp->align = sp->align; dp->style_id = sp->style_id; dp->list_level = sp->list_level; dp->list_ordered = sp->list_ordered;
        for (int r = 0; r < sp->nrun; r++) {
            doc_run *nr = (doc_run *)realloc(dp->runs, (size_t)(dp->nrun + 1) * sizeof(doc_run));
            if (!nr) return 0;
            dp->runs = nr;
            dp->runs[dp->nrun].fmt = sp->runs[r].fmt;
            dp->runs[dp->nrun].text = sp->runs[r].text ? strdup(sp->runs[r].text) : 0;
            dp->nrun++;
        }
    }
    if (src->image) {
        doc_image *im = (doc_image *)calloc(1, sizeof(doc_image));
        if (!im) return 0;
        *im = *src->image;
        im->bytes = 0; im->nbytes = 0;
        if (src->image->bytes && src->image->nbytes) {
            im->bytes = (unsigned char *)malloc(src->image->nbytes);
            if (im->bytes) { memcpy(im->bytes, src->image->bytes, src->image->nbytes); im->nbytes = src->image->nbytes; }
        }
        dst->image = im;
    }
    return 1;
}

// --- Layout inference / application ----------------------------------------------
static int layout_infer(const slide *sl) {
    int ntitle = 0, ntext = 0, nother = 0, nlist = 0, npara = 0;
    for (int j = 0; j < sl->nshape; j++) {
        const shape *sh = &sl->shapes[j];
        if (sh->type == SHP_TITLE) ntitle++;
        else if (sh->type == SHP_TEXTBOX) {
            ntext++;
            for (int k = 0; k < sh->npara; k++) { npara++; if (sh->paras[k].list_level >= 0) nlist++; }
        } else nother++;
    }
    if (ntitle == 0) return LAY_BLANK;
    if (ntext >= 2) return LAY_TWO_CONTENT;
    if (ntext == 0) return LAY_TITLE;
    // One title + one text box: a subtitle (one plain paragraph) reads as a
    // Title Slide; bullets or several paragraphs read as Title and Content.
    if (npara <= 1 && nlist == 0) return LAY_TITLE;
    return LAY_TITLE_CONTENT;
}

static int slide_layout(int idx) {
    if (!g_pres || idx < 0 || idx >= g_pres->nslide) return LAY_BLANK;
    if (idx < SLIDES_MAX_LAYOUT && g_layout_of[idx] >= 0) return g_layout_of[idx];
    return layout_infer(&g_pres->slides[idx]);
}

static void layout_set(int idx, int lay) { if (idx >= 0 && idx < SLIDES_MAX_LAYOUT) g_layout_of[idx] = lay; }
static void layout_reset_all(void) { for (int i = 0; i < SLIDES_MAX_LAYOUT; i++) g_layout_of[i] = -1; }
static void layout_insert_at(int idx) {
    if (idx < 0 || idx >= SLIDES_MAX_LAYOUT) return;
    memmove(&g_layout_of[idx + 1], &g_layout_of[idx], (size_t)(SLIDES_MAX_LAYOUT - idx - 1) * sizeof(int));
    g_layout_of[idx] = -1;
}
static void layout_remove_at(int idx) {
    if (idx < 0 || idx >= SLIDES_MAX_LAYOUT) return;
    memmove(&g_layout_of[idx], &g_layout_of[idx + 1], (size_t)(SLIDES_MAX_LAYOUT - idx - 1) * sizeof(int));
    g_layout_of[SLIDES_MAX_LAYOUT - 1] = -1;
}

static void ph_geom(const lay_ph_t *ph, int *x, int *y, int *w, int *h) {
    int nw = native_w(), nh = native_h();
    *x = ph->fx * nw / 1000; *y = ph->fy * nh / 1000; *w = ph->fw * nw / 1000; *h = ph->fh * nh / 1000;
}

// Apply a layout to a slide, preserving content: empty text placeholders are
// dropped, then each of the layout's placeholders claims the next existing
// shape of its type (repositioning it) or is added empty. Blank only drops
// the empty placeholders; shapes with content are never destroyed.
static void apply_layout(slide *sl, int lay) {
    if (!sl || lay < 0 || lay >= LAY_N) return;
    for (int j = sl->nshape - 1; j >= 0; j--)
        if (shape_is_text(&sl->shapes[j]) && !shape_has_text(&sl->shapes[j])) slide_remove_shape(sl, j);
    int used[SLIDES_MAX_SHAPES]; memset(used, 0, sizeof(used));
    for (int p = 0; p < LAYOUT_NPH[lay]; p++) {
        const lay_ph_t *ph = &LAYOUT_PH[lay][p];
        int x, y, w, h; ph_geom(ph, &x, &y, &w, &h);
        shape *target = 0;
        for (int j = 0; j < sl->nshape && j < SLIDES_MAX_SHAPES; j++)
            if (!used[j] && sl->shapes[j].type == ph->type) { used[j] = 1; target = &sl->shapes[j]; break; }
        if (!target) {
            target = slide_add_shape(sl, ph->type);
            if (!target) return;
            if (sl->nshape - 1 < SLIDES_MAX_SHAPES) used[sl->nshape - 1] = 1;
        }
        target->x = x; target->y = y; target->w = w; target->h = h;
    }
}

// --- Image cache ------------------------------------------------------------------
static void free_image_cache(void) {
    for (int i = 0; i < g_imgcache_n; i++) free(g_imgcache[i].px);
    g_imgcache_n = 0;
}

// Decode every SHP_IMAGE shape on the CURRENT slide into a cached BGRA buffer
// sized to the current slide geometry, once per (slide, layout) change -
// never from inside draw() itself, so a redraw triggered by an ordinary
// key/mouse event never re-runs a PNG/JPEG decode (#426).
static void build_image_cache(void) {
    free_image_cache();
    slide *sl = cur_slide();
    if (!sl || g_slide_w <= 0 || g_slide_h <= 0) return;
    int box_w = g_slide_w - 2 * PAD_PX; if (box_w < 16) box_w = 16;
    int box_h = g_slide_h - 2 * PAD_PX; if (box_h < 16) box_h = 16;
    for (int j = 0; j < sl->nshape && g_imgcache_n < SLIDES_MAX_IMG; j++) {
        shape *sh = &sl->shapes[j];
        if (sh->type != SHP_IMAGE || !sh->image || !sh->image->bytes || sh->image->nbytes == 0) continue;
        int tw = box_w, th = box_h;
        if (sh->w > 0 && sh->h > 0) {
            tw = (int)(sh->w * g_scale); th = (int)(sh->h * g_scale);
            if (tw < 8) tw = 8;
            if (th < 8) th = 8;
        }
        unsigned int cap = (unsigned int)(tw * th * 4);
        void *buf = malloc(cap);
        if (!buf) continue;
        int dims[2] = { 0, 0 };
        int r = decode_image(sh->image->bytes, (unsigned int)sh->image->nbytes, tw, th, buf, cap, dims);
        if (r <= 0 || dims[0] <= 0 || dims[1] <= 0) { free(buf); continue; }
        g_imgcache[g_imgcache_n].px = buf;
        g_imgcache[g_imgcache_n].w = dims[0];
        g_imgcache[g_imgcache_n].h = dims[1];
        g_imgcache[g_imgcache_n].shape_idx = j;
        g_imgcache_n++;
    }
}

static const img_cache_ent_t *cached_image_for(int shape_idx) {
    for (int i = 0; i < g_imgcache_n; i++) if (g_imgcache[i].shape_idx == shape_idx) return &g_imgcache[i];
    return 0;
}

// --- Geometry ----------------------------------------------------------------------
// Fit the slide sheet (16:9 or whatever the deck says) into a view rect with
// `inset` px of clear on every side, preserving aspect, centred.
static void fit_slide(int vx, int vy, int vw, int vh, int inset) {
    int nw = native_w(), nh = native_h();
    int avail_w = vw - 2 * inset; if (avail_w < 40) avail_w = 40;
    int avail_h = vh - 2 * inset; if (avail_h < 40) avail_h = 40;
    double sw = (double)avail_w / nw, sh = (double)avail_h / nh;
    g_scale = sw < sh ? sw : sh; if (g_scale <= 0.0) g_scale = 0.5;
    g_slide_w = (int)(nw * g_scale);
    g_slide_h = (int)(nh * g_scale);
    g_slide_x = vx + (vw - g_slide_w) / 2;
    g_slide_y = vy + (vh - g_slide_h) / 2;
}

static void thumbs_sync(void) {
    int n = g_pres ? g_pres->nslide : 0;
    othumbs_config(&g_thumbs, g_strip_x, g_strip_y, g_strip_w, g_strip_h, n, g_cur_slide);
    othumbs_reveal(&g_thumbs);
}

static void relayout(void) {
    if (!g_app) return;
    int cx, cy, cw, ch;
    oapp_content_rect(g_app, &cx, &cy, &cw, &ch);
    g_strip_w = g_show_thumbs ? OTHUMB_STRIP_W : 0;
    g_strip_x = cx; g_strip_y = cy; g_strip_h = ch;
    g_view_x = cx + g_strip_w; g_view_y = cy;
    g_view_w = cw - g_strip_w; if (g_view_w < 40) g_view_w = 40;
    g_view_h = ch; if (g_view_h < 40) g_view_h = 40;
    fit_slide(g_view_x, g_view_y, g_view_w, g_view_h, CANVAS_INSET);
    thumbs_sync();
    build_image_cache();
}

// --- Chrome sync (status bar, toolbar enabled/checked, menu state) -------------------
static void sync_chrome(void) {
    char msg[360];
    int n = g_pres ? g_pres->nslide : 0;
    const char *name = g_has_path ? basename_of(g_path) : "Untitled";
    snprintf(msg, sizeof(msg), "%s%s  -  Slide %d of %d  -  Layout: %s",
             name, g_readonly ? " (read-only)" : "", n ? g_cur_slide + 1 : 0, n,
             n ? LAYOUT_NAMES[slide_layout(g_cur_slide)] : "-");
    oapp_set_status(g_app, msg);
    char title[220];
    snprintf(title, sizeof(title), "SLIDES - %s%s", name, g_readonly ? " [Read-Only]" : "");
    oapp_set_title(g_app, title);

    const shape *sh = sel_shape();
    const doc_runfmt *f = shape_first_fmt(sh);
    int text_sel = (f != 0);
    otb_set_enabled(&g_tb, T_BOLD, text_sel);
    otb_set_enabled(&g_tb, T_ITAL, text_sel);
    otb_set_checked(&g_tb, T_BOLD, text_sel && f->bold);
    otb_set_checked(&g_tb, T_ITAL, text_sel && f->italic);
    FORMAT_ITEMS[2].enabled = text_sel; FORMAT_ITEMS[2].checked = text_sel && f->bold;
    FORMAT_ITEMS[3].enabled = text_sel; FORMAT_ITEMS[3].checked = text_sel && f->italic;
    otb_set_enabled(&g_tb, T_PREV, n > 0 && g_cur_slide > 0);
    otb_set_enabled(&g_tb, T_NEXT, n > 0 && g_cur_slide < n - 1);
    otb_set_enabled(&g_tb, T_PLAY, n > 0);
    EDIT_ITEMS[8].enabled = n > 0;
    VIEW_ITEMS[0].checked = g_show_thumbs ? true : false;
}

static void goto_slide(int idx) {
    if (!g_pres || g_pres->nslide <= 0) return;
    if (idx < 0) idx = 0;
    if (idx >= g_pres->nslide) idx = g_pres->nslide - 1;
    if (idx == g_cur_slide) { g_thumbs.sel = idx; return; }
    g_cur_slide = idx;
    g_sel_shape = -1;
    g_thumbs.sel = idx;
    othumbs_reveal(&g_thumbs);
    build_image_cache();
    sync_chrome();
}

// --- Document lifecycle --------------------------------------------------------------
static void new_presentation(void) {
    if (g_pres) { pres_free(g_pres); g_pres = 0; }
    free_image_cache();
    g_pres = pres_new();
    layout_reset_all();
    if (g_pres) {
        slide *s = pres_add_slide(g_pres);
        if (s) { apply_layout(s, LAY_TITLE); layout_set(0, LAY_TITLE); }
    }
    g_cur_slide = 0; g_sel_shape = -1; g_has_path = 0; g_readonly = 0; g_path[0] = 0;
    oapp_set_dirty(g_app, 0);
    relayout();
    sync_chrome();
}

static void load_presentation(const char *path) {
    presentation *np = 0; int ro = 0;
    int rc = office_open_presentation(path, &np, &ro);
    if (rc != 0 || !np) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Failed to open %s", path);
        oapp_set_status(g_app, msg);
        return;
    }
    if (np->nslide == 0) pres_add_slide(np);   // defensive: never show a slideless deck
    if (g_pres) pres_free(g_pres);
    free_image_cache();
    g_pres = np; g_readonly = ro;
    layout_reset_all();
    strncpy(g_path, path, sizeof(g_path) - 1); g_path[sizeof(g_path) - 1] = 0;
    g_has_path = 1;
    g_cur_slide = 0; g_sel_shape = -1;
    oapp_set_dirty(g_app, 0);
    relayout();
    sync_chrome();
}

// Returns 1 on success.
static int save_presentation(const char *path) {
    unsigned char *out = 0; unsigned long outlen = 0;
    int rc = has_ext_ci(path, ".odp") ? odp_save(g_pres, &out, &outlen)
                                      : pptx_save(g_pres, &out, &outlen);
    if (rc != 0 || !out) {
        oapp_set_status(g_app, "Save failed: could not serialize presentation");
        if (out) free(out);
        return 0;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        oapp_set_status(g_app, "Save failed: could not write file");
        free(out);
        return 0;
    }
    unsigned long written = 0;
    while (written < outlen) {
        long w = write(fd, out + written, (size_t)(outlen - written));
        if (w <= 0) break;
        written += (unsigned long)w;
    }
    close(fd);
    free(out);
    if (written != outlen) {
        oapp_set_status(g_app, "Save failed: short write");
        return 0;
    }
    strncpy(g_path, path, sizeof(g_path) - 1); g_path[sizeof(g_path) - 1] = 0;
    g_has_path = 1;
    g_readonly = 0;
    oapp_set_dirty(g_app, 0);
    sync_chrome();
    oapp_set_status(g_app, "Saved");
    return 1;
}

// Save (to the current path when writable, else Save As). Returns 1 if saved.
static int do_save(int force_dialog) {
    if (!force_dialog && !g_readonly && g_has_path) return save_presentation(g_path);
    const char *path = oapp_save_dialog(g_app, g_has_path ? basename_of(g_path) : "Untitled.pptx");
    if (!path) return 0;
    return save_presentation(path);
}

// Unsaved-changes prompt (spec 5.1.5 / 5.3.4). Returns 1 when the caller may
// proceed (clean, discarded, or saved), 0 when cancelled.
static int confirm_discard(const char *what) {
    if (!oapp_is_dirty(g_app)) return 1;
    char body[300];
    snprintf(body, sizeof(body), "Save changes to %s before %s?",
             g_has_path ? basename_of(g_path) : "Untitled.pptx", what);
    int r = omodal_confirm3(g_app, "Unsaved changes", body, "Save", "Discard");
    if (r == OM_CANCEL) return 0;
    if (r == OM_THIRD) return do_save(0);
    return 1;   // OM_OK = Discard
}

static void do_open(void) {
    if (!confirm_discard("opening another file")) return;
    const char *path = oapp_open_dialog(g_app, ".pptx.odp.ppt");
    if (path) load_presentation(path);
}

static void do_new(void) {
    if (!confirm_discard("starting a new presentation")) return;
    new_presentation();
}

// --- Slide edits ----------------------------------------------------------------------
// Insert a new slide with `lay` after the current one and select it.
static void add_slide_after_current(int lay) {
    if (!g_pres) return;
    slide *ns = pres_add_slide(g_pres);
    if (!ns) return;
    int at = g_pres->nslide == 1 ? 0 : g_cur_slide + 1;
    if (at < g_pres->nslide - 1) {
        slide tmp = g_pres->slides[g_pres->nslide - 1];
        memmove(&g_pres->slides[at + 1], &g_pres->slides[at], (size_t)(g_pres->nslide - 1 - at) * sizeof(slide));
        g_pres->slides[at] = tmp;
    }
    layout_insert_at(at);
    apply_layout(&g_pres->slides[at], lay);
    layout_set(at, lay);
    g_cur_slide = at; g_sel_shape = -1;
    set_dirty();
    thumbs_sync();
    g_thumbs.sel = at; othumbs_reveal(&g_thumbs);
    build_image_cache();
    sync_chrome();
}

static void duplicate_slide(void) {
    slide *src = cur_slide();
    if (!src) return;
    slide *ns = pres_add_slide(g_pres);
    if (!ns) return;
    src = cur_slide();   // slides[] may have moved
    slide copy; memset(&copy, 0, sizeof(copy));
    copy.title = src->title ? strdup(src->title) : 0;
    for (int j = 0; j < src->nshape; j++) {
        shape *sh = slide_add_shape(&copy, src->shapes[j].type);
        if (!sh) break;
        if (!shape_copy(sh, &src->shapes[j])) break;
    }
    int at = g_cur_slide + 1;
    memmove(&g_pres->slides[at + 1], &g_pres->slides[at], (size_t)(g_pres->nslide - 1 - at) * sizeof(slide));
    g_pres->slides[at] = copy;
    layout_insert_at(at);
    layout_set(at, slide_layout(g_cur_slide));
    g_cur_slide = at; g_sel_shape = -1;
    set_dirty();
    thumbs_sync();
    g_thumbs.sel = at; othumbs_reveal(&g_thumbs);
    build_image_cache();
    sync_chrome();
}

static void delete_slide(void) {
    if (!g_pres || g_pres->nslide <= 0) return;
    int at = g_cur_slide;
    slide_release(&g_pres->slides[at]);
    memmove(&g_pres->slides[at], &g_pres->slides[at + 1], (size_t)(g_pres->nslide - at - 1) * sizeof(slide));
    g_pres->nslide--;
    layout_remove_at(at);
    if (g_pres->nslide == 0) {
        slide *s = pres_add_slide(g_pres);
        if (s) { apply_layout(s, LAY_TITLE); layout_set(0, LAY_TITLE); }
    }
    if (g_cur_slide >= g_pres->nslide) g_cur_slide = g_pres->nslide - 1;
    g_sel_shape = -1;
    set_dirty();
    thumbs_sync();
    g_thumbs.sel = g_cur_slide; othumbs_reveal(&g_thumbs);
    build_image_cache();
    sync_chrome();
}

static void add_text_box(void) {
    slide *sl = cur_slide();
    if (!sl) return;
    shape *sh = slide_add_shape(sl, SHP_TEXTBOX);
    if (!sh) return;
    int nw = native_w(), nh = native_h();
    sh->x = nw / 4; sh->y = nh * 2 / 5; sh->w = nw / 2; sh->h = nh / 5;
    // A visible starting text: with no in-place text editor yet an empty box
    // would only ever exist as a placeholder outline, and would not survive
    // a save as anything a reader could see.
    doc_para *p = shape_add_para(sh);
    if (p) {
        doc_run *nr = (doc_run *)calloc(1, sizeof(doc_run));
        if (nr) { nr->text = strdup("Text box"); nr->fmt.color = 0xFFFFFFFF; p->runs = nr; p->nrun = 1; }
    }
    g_sel_shape = sl->nshape - 1;
    set_dirty();
    sync_chrome();
}

static unsigned char *read_whole_file(const char *path, unsigned long *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned long cap = 65536, len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) { close(fd); return 0; }
    for (;;) {
        if (len == cap) {
            unsigned long ncap = cap * 2;
            unsigned char *nb = (unsigned char *)realloc(buf, ncap);
            if (!nb) { free(buf); close(fd); return 0; }
            buf = nb; cap = ncap;
        }
        long r = read(fd, buf + len, (size_t)(cap - len));
        if (r <= 0) break;
        len += (unsigned long)r;
    }
    close(fd);
    if (len == 0) { free(buf); return 0; }
    *out_len = len;
    return buf;
}

static int g_image_seq;

static void insert_image(void) {
    slide *sl = cur_slide();
    if (!sl) return;
    const char *path = oapp_open_dialog_titled(g_app, ".png.jpg.jpeg.bmp", "Insert Image");
    if (!path) return;
    unsigned long n = 0;
    unsigned char *bytes = read_whole_file(path, &n);
    if (!bytes) { oapp_set_status(g_app, "Insert Image: could not read the file"); return; }
    // Probe the natural aspect with a small decode (dims come back fitted).
    int dims[2] = { 0, 0 };
    {
        unsigned int cap = 256u * 256u * 4u;
        void *probe = malloc(cap);
        if (probe) {
            int r = decode_image(bytes, (unsigned int)n, 256, 256, probe, cap, dims);
            free(probe);
            if (r <= 0) dims[0] = dims[1] = 0;
        }
    }
    if (dims[0] <= 0 || dims[1] <= 0) { free(bytes); oapp_set_status(g_app, "Insert Image: unsupported or corrupt image"); return; }
    doc_image *im = (doc_image *)calloc(1, sizeof(doc_image));
    if (!im) { free(bytes); return; }
    im->bytes = bytes; im->nbytes = n; im->w = dims[0]; im->h = dims[1];
    snprintf(im->id, sizeof(im->id), "image%d", ++g_image_seq);
    shape *sh = slide_add_shape(sl, SHP_IMAGE);
    if (!sh) { free(bytes); free(im); return; }
    sh->image = im;                       // the shape OWNS the doc_image (slidemodel.c ownership note)
    int nw = native_w(), nh = native_h();
    sh->w = nw / 2;                       // spec 5.3.3: centred at 50% slide width
    sh->h = sh->w * dims[1] / dims[0];
    if (sh->h > nh - 40) { sh->h = nh - 40; sh->w = sh->h * dims[0] / dims[1]; }
    sh->x = (nw - sh->w) / 2; sh->y = (nh - sh->h) / 2;
    g_sel_shape = sl->nshape - 1;
    set_dirty();
    build_image_cache();
    sync_chrome();
}

static void delete_selected_shape(void) {
    slide *sl = cur_slide();
    if (!sl || g_sel_shape < 0 || g_sel_shape >= sl->nshape) return;
    slide_remove_shape(sl, g_sel_shape);
    g_sel_shape = -1;
    set_dirty();
    build_image_cache();
    sync_chrome();
}

// Bold / Italic: toggle across every run of the selected text shape (the
// state shown is the first run's). Persisting run formatting is the format
// layer's job (fmt_pptx_odp); this edits the model and the renderer honours it.
static void toggle_style(int italic) {
    shape *sh = sel_shape();
    const doc_runfmt *f = shape_first_fmt(sh);
    if (!f) { oapp_set_status(g_app, "Select a text box first"); return; }
    int nv = italic ? !f->italic : !f->bold;
    for (int k = 0; k < sh->npara; k++)
        for (int r = 0; r < sh->paras[k].nrun; r++) {
            if (italic) sh->paras[k].runs[r].fmt.italic = nv; else sh->paras[k].runs[r].fmt.bold = nv;
        }
    set_dirty();
    sync_chrome();
}

// --- Slide Layout / New Slide picker (omodal custom body, spec 5.3.1 / 5.3.2) ----------
#define LT_W   144
#define LT_H   81
#define LT_LBL 18
#define LT_GUT 16
#define LT_BODY_H 220

typedef struct { int sel; int ox, oy; } laypick_t;

static void lay_tile_rect(const laypick_t *lp, int i, int *x, int *y) {
    *x = lp->ox + (i % 2) * (LT_W + LT_GUT);
    *y = lp->oy + (i / 2) * (LT_H + LT_LBL + LT_GUT);
}

// Schematic of a layout inside a tile (title bar + placeholder rects).
static void lay_tile_schematic(int win, int lay, int x, int y, int w, int h, uint32_t line, uint32_t ink) {
    for (int p = 0; p < LAYOUT_NPH[lay]; p++) {
        const lay_ph_t *ph = &LAYOUT_PH[lay][p];
        int px = x + ph->fx * w / 1000, py = y + ph->fy * h / 1000, pw = ph->fw * w / 1000, phh = ph->fh * h / 1000;
        if (ph->type == SHP_TITLE) {
            win_draw_rect(win, px + pw / 6, py + phh / 2 - 1, pw * 2 / 3, 3, ink);
        } else {
            gui_draw_rect_outline(win, px, py, pw, phh, line);
            int ly = py + 6, n = 0;
            while (ly + 2 < py + phh - 4 && n < 5) { win_draw_rect(win, px + 5, ly, pw * (5 + (n % 3)) / 10, 1, line); ly += 7; n++; }
        }
    }
}

static void laypick_draw(int win, int bx, int by, int bw, int bh, void *ctx) {
    laypick_t *lp = (laypick_t *)ctx;
    (void)bh;
    lp->ox = bx + (bw - (2 * LT_W + LT_GUT)) / 2;
    lp->oy = by + 4;
    uint32_t overlay = theme_color(THEME_COLOR_SURFACE_OVERLAY);   // [filesui] see libc/theme.h
    uint32_t paper = theme_color(THEME_COLOR_TEXTBOX_BG);
    uint32_t line = gui_ensure_contrast(theme_color(THEME_COLOR_BORDER_SUBTLE), paper, GUI_FLOOR_NONTEXT);
    uint32_t strong = gui_ensure_contrast(theme_color(THEME_COLOR_WINDOW_BORDER), overlay, GUI_FLOOR_NONTEXT);
    uint32_t acc = gui_ensure_contrast(theme_color(THEME_COLOR_ACCENT), overlay, GUI_FLOOR_NONTEXT);
    uint32_t ink = gui_ensure_contrast(theme_color(THEME_COLOR_ON_SURFACE), overlay, GUI_FLOOR_TEXT);
    uint32_t ink_paper = gui_ensure_contrast(theme_color(THEME_COLOR_ON_SURFACE), paper, GUI_FLOOR_TEXT);
    int cap = theme_metric_or(THEME_METRIC_TYPE_CAPTION, 11);
    for (int i = 0; i < LAY_N; i++) {
        int x, y; lay_tile_rect(lp, i, &x, &y);
        win_draw_rect(win, x, y, LT_W, LT_H, paper);
        lay_tile_schematic(win, i, x, y, LT_W, LT_H, line, ink_paper);
        gui_draw_rect_outline(win, x, y, LT_W, LT_H, strong);
        if (i == lp->sel) {
            win_draw_rect(win, x - 4, y - 4, LT_W + 8, 2, acc);
            win_draw_rect(win, x - 4, y + LT_H + 2, LT_W + 8, 2, acc);
            win_draw_rect(win, x - 4, y - 4, 2, LT_H + 8, acc);
            win_draw_rect(win, x + LT_W + 2, y - 4, 2, LT_H + 8, acc);
        }
        gui_text_ttf_centered(win, x, y + LT_H + 2, LT_W, LT_LBL, LAYOUT_NAMES[i], ink, cap);
    }
}

static int laypick_press(int bx, int by, int mx, int my, void *ctx) {
    laypick_t *lp = (laypick_t *)ctx;
    (void)bx; (void)by;
    for (int i = 0; i < LAY_N; i++) {
        int x, y; lay_tile_rect(lp, i, &x, &y);
        if (mx >= x - 4 && mx < x + LT_W + 4 && my >= y - 4 && my < y + LT_H + LT_LBL) { lp->sel = i; return 1; }
    }
    return 0;
}

static int laypick_key(unsigned int kc, char ch, void *ctx) {
    laypick_t *lp = (laypick_t *)ctx;
    (void)ch;
    int s = lp->sel;
    switch (kc) {
        case GUI_KEY_LEFT:  if (s % 2 == 1) s--; break;
        case GUI_KEY_RIGHT: if (s % 2 == 0) s++; break;
        case GUI_KEY_UP:    if (s >= 2) s -= 2; break;
        case GUI_KEY_DOWN:  if (s < 2) s += 2; break;
        default: return 0;
    }
    if (s < 0) s = 0;
    if (s >= LAY_N) s = LAY_N - 1;
    lp->sel = s;
    return 1;
}

// Returns the chosen layout or -1 on Cancel.
static int layout_picker(const char *title, const char *ok_label, int initial) {
    laypick_t lp; memset(&lp, 0, sizeof(lp));
    lp.sel = (initial >= 0 && initial < LAY_N) ? initial : LAY_TITLE_CONTENT;
    om_custom_t body = { laypick_draw, laypick_press, laypick_key, &lp };
    int r = omodal_run_custom(g_app, title, OM_SIZE_M, LT_BODY_H, ok_label, 0, &body);
    return r == OM_OK ? lp.sel : -1;
}

static void modal_slide_layout(void) {
    slide *sl = cur_slide();
    if (!sl) return;
    int lay = layout_picker("Slide Layout", "Apply", slide_layout(g_cur_slide));
    if (lay < 0) return;
    apply_layout(cur_slide(), lay);
    layout_set(g_cur_slide, lay);
    g_sel_shape = -1;
    set_dirty();
    build_image_cache();
    sync_chrome();
}

static void modal_new_slide(void) {
    int lay = layout_picker("New Slide", "Add", LAY_TITLE_CONTENT);
    if (lay < 0) return;
    add_slide_after_current(lay);
}

// --- Drawing --------------------------------------------------------------------------
static void draw_dashed_outline(int win, int x, int y, int w, int h, uint32_t c) {
    for (int i = 0; i < w; i += 8) { int l = w - i < 4 ? w - i : 4; win_draw_rect(win, x + i, y, l, 1, c); win_draw_rect(win, x + i, y + h - 1, l, 1, c); }
    for (int i = 0; i < h; i += 8) { int l = h - i < 4 ? h - i : 4; win_draw_rect(win, x, y + i, 1, l, c); win_draw_rect(win, x + w - 1, y + i, 1, l, c); }
}

// One paragraph's concatenated text at (x,y,w). Bulleted (list_level>=0)
// paragraphs get an ASCII "- " prefix - win_draw_text_ttf_ex's UTF-8 decode
// mangles a real bullet glyph into mojibake (Writer's finding), so this
// matches Writer's workaround rather than inventing a different one.
static void draw_para_line(int win, int x, int y, int w, const doc_para *p, int size, uint32_t default_ink) {
    char text[512];
    para_concat(p, text, sizeof(text));
    uint32_t ink = default_ink; int style = 0;
    if (p->nrun > 0) { ink = run_color(&p->runs[0].fmt, default_ink); style = run_style(&p->runs[0].fmt); }
    char line[560];
    if (p->list_level >= 0) snprintf(line, sizeof(line), "- %s", text);
    else                    snprintf(line, sizeof(line), "%s", text);
    if (!line[0]) return;
    int tw = gui_ttf_render_width(line, size);
    int lx = x;
    if (p->align == DOC_ALIGN_C) lx = x + (w - tw) / 2;
    else if (p->align == DOC_ALIGN_R) lx = x + w - tw;
    if (lx < x) lx = x;
    win_draw_text_ttf_ex(win, lx, y, line, 0, size, style, ink);
}

// Window rect of shape j on the current slide for the current g_slide_*
// geometry. Explicit geometry is scaled; a shape without geometry is
// auto-flowed (cursor_y in/out). Returns 1 if the rect is at least 1 px tall.
static int shape_rect(const shape *sh, int j, int *cursor_y, int *rx, int *ry, int *rw, int *rh) {
    int title_size = (int)(40 * g_scale); if (title_size < 16) title_size = 16; if (title_size > 54) title_size = 54;
    int body_size  = (int)(22 * g_scale); if (body_size  < 11) body_size  = 11; if (body_size  > 28) body_size  = 28;
    int line_h = body_size + 10;
    int content_x = g_slide_x + PAD_PX, content_w = g_slide_w - 2 * PAD_PX;
    if (content_w < 20) content_w = 20;
    int explicit_geom = (sh->w > 0 && sh->h > 0);
    if (explicit_geom) {
        *rx = g_slide_x + (int)(sh->x * g_scale);
        *ry = g_slide_y + (int)(sh->y * g_scale);
        *rw = (int)(sh->w * g_scale);
        *rh = (int)(sh->h * g_scale);
    } else {
        *rx = content_x; *rw = content_w; *ry = *cursor_y;
        switch (sh->type) {
            case SHP_TITLE:   *rh = title_size + 24; break;
            case SHP_TEXTBOX: *rh = sh->npara * line_h + 12; if (*rh < line_h) *rh = line_h; break;
            case SHP_IMAGE: {
                const img_cache_ent_t *im = cached_image_for(j);
                *rh = im ? im->h + 8 : (int)(0.5 * content_w);
                break;
            }
            default: *rh = 60; break;   // SHP_RECT with no geometry: a visible band
        }
    }
    if (*ry + *rh > g_slide_y + g_slide_h) *rh = (g_slide_y + g_slide_h) - *ry;  // clip to the sheet, no scroll in v1
    if (*rh < 0) *rh = 0;
    if (!explicit_geom) *cursor_y = *ry + *rh + GAP_PX;
    return *rh > 0;
}

// The slide sheet at g_slide_* (editor canvas or present mode).
static void draw_slide_sheet(int win, int editing) {
    uint32_t border = theme_color(THEME_COLOR_WINDOW_BORDER);   // [filesui] zero test removed: 0 is BLACK, never unset (libc/theme.h).
    // The slide sheet itself stays white regardless of the active OS theme:
    // it represents printed/exported slide content, the same convention every
    // presentation editor uses (a dark app chrome around a white page).
    uint32_t canvas_bg = 0x00FFFFFF;
    uint32_t canvas_ink = 0x00202020;
    uint32_t hint = 0x00909090;
    uint32_t accent = theme_color(THEME_COLOR_ACCENT);   // [filesui] zero test removed: 0 is BLACK, never unset (libc/theme.h).
    accent = gui_ensure_contrast(accent, canvas_bg, GUI_FLOOR_NONTEXT);

    win_draw_rect(win, g_slide_x, g_slide_y, g_slide_w, g_slide_h, canvas_bg);
    if (editing) gui_draw_rect_outline(win, g_slide_x - 1, g_slide_y - 1, g_slide_w + 2, g_slide_h + 2, border);

    g_shape_rc_n = 0;
    slide *sl = cur_slide();
    if (!sl) return;
    if (sl->nshape == 0 && editing) {
        gui_text_ttf_centered(win, g_slide_x, g_slide_y, g_slide_w, g_slide_h,
                              "Blank slide. Use Slide Layout or Insert to add content.", hint, 14);
        return;
    }

    int title_size = (int)(40 * g_scale); if (title_size < 16) title_size = 16; if (title_size > 54) title_size = 54;
    int body_size  = (int)(22 * g_scale); if (body_size  < 11) body_size  = 11; if (body_size  > 28) body_size  = 28;
    int line_h = body_size + 10;
    int cursor_y = g_slide_y + PAD_PX;

    for (int j = 0; j < sl->nshape; j++) {
        shape *sh = &sl->shapes[j];
        int rx, ry, rw, rh;
        int vis = shape_rect(sh, j, &cursor_y, &rx, &ry, &rw, &rh);
        if (j < SLIDES_MAX_SHAPES) { g_shape_rc[j].x = rx; g_shape_rc[j].y = ry; g_shape_rc[j].w = rw; g_shape_rc[j].h = rh; g_shape_rc_n = j + 1; }
        if (!vis) continue;

        switch (sh->type) {
            case SHP_TITLE: {
                if (!shape_has_text(sh)) {
                    if (editing) { draw_dashed_outline(win, rx, ry, rw, rh, hint); gui_text_ttf_centered(win, rx, ry, rw, rh, "Click to add title", hint, body_size); }
                    break;
                }
                char t[512]; shape_text(sh, t, sizeof(t));
                const doc_runfmt *f = shape_first_fmt(sh);
                int style = run_style(f);
                uint32_t ink = run_color(f, canvas_ink);
                int ts = title_size;
                int tw = gui_ttf_render_width(t, ts);
                while (tw > rw - 8 && ts > 14) { ts -= 2; tw = gui_ttf_render_width(t, ts); }
                int tx = rx + (rw - tw) / 2; if (tx < rx) tx = rx;
                win_draw_text_ttf_ex(win, tx, ry + (rh - ts) / 2, t, 0, ts, style, ink);
                break;
            }
            case SHP_TEXTBOX: {
                if (!shape_has_text(sh)) {
                    if (editing) { draw_dashed_outline(win, rx, ry, rw, rh, hint); gui_text_ttf_centered(win, rx, ry, rw, rh, "Click to add text", hint, body_size); }
                    break;
                }
                int ly = ry + 6;
                for (int k = 0; k < sh->npara; k++) {
                    if (ly + body_size > ry + rh) break;
                    draw_para_line(win, rx + 6, ly, rw - 12, &sh->paras[k], body_size, canvas_ink);
                    ly += line_h;
                }
                break;
            }
            case SHP_RECT:
                win_draw_rect(win, rx, ry, rw, rh, sh->fill ? sh->fill : 0x00CCCCCC);
                break;
            case SHP_IMAGE: {
                const img_cache_ent_t *im = cached_image_for(j);
                if (im && im->px) {
                    int ix = rx + (rw - im->w) / 2; if (ix < rx) ix = rx;
                    int iy = ry + (rh - im->h) / 2; if (iy < ry) iy = ry;
                    win_draw_image(win, ix, iy, im->w, im->h, im->px);
                } else if (editing) {
                    draw_dashed_outline(win, rx, ry, rw, rh, hint);
                    gui_text_ttf_centered(win, rx, ry, rw, rh, "Image", hint, body_size);
                }
                break;
            }
            default: break;
        }
    }

    // Selected shape: 1 px accent outline + 6x6 handles at the 8 points (spec 2.3).
    if (editing && g_sel_shape >= 0 && g_sel_shape < g_shape_rc_n) {
        rect_t r = g_shape_rc[g_sel_shape];
        gui_draw_rect_outline(win, r.x - 1, r.y - 1, r.w + 2, r.h + 2, accent);
        int hx[3] = { r.x - 3, r.x + r.w / 2 - 3, r.x + r.w - 3 };
        int hy[3] = { r.y - 3, r.y + r.h / 2 - 3, r.y + r.h - 3 };
        for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) {
            if (a == 1 && b == 1) continue;
            win_draw_rect(win, hx[a], hy[b], 6, 6, accent);
        }
    }
}

// Thumbnail content: a scaled schematic render of slide `i` into (x,y,w,h):
// titles as real (caption-size) text so the strip is legible, body
// paragraphs as bars, rects as fills, images as a muted block.
static void thumb_render(int win, int i, int x, int y, int w, int h, void *ctx) {
    (void)ctx;
    if (!g_pres || i < 0 || i >= g_pres->nslide) return;
    const slide *sl = &g_pres->slides[i];
    uint32_t paper = theme_color(THEME_COLOR_TEXTBOX_BG);   // [filesui] see libc/theme.h
    uint32_t ink = gui_ensure_contrast(theme_color(THEME_COLOR_ON_SURFACE), paper, GUI_FLOOR_TEXT);
    uint32_t bar = gui_ensure_contrast(theme_color(THEME_COLOR_MUTED), paper, GUI_FLOOR_NONTEXT);
    uint32_t imgfill = gui_mix(paper, bar, 96);
    int cap = theme_metric_or(THEME_METRIC_TYPE_CAPTION, 11);
    int nw = native_w(), nh = native_h();
    int cursor = y + 6;
    int content_x = x + 4, content_w = w - 8;
    for (int j = 0; j < sl->nshape; j++) {
        const shape *sh = &sl->shapes[j];
        int explicit_geom = (sh->w > 0 && sh->h > 0);
        int rx, ry, rw, rh;
        if (explicit_geom) {
            rx = x + sh->x * w / nw; ry = y + sh->y * h / nh; rw = sh->w * w / nw; rh = sh->h * h / nh;
        } else {
            rx = content_x; rw = content_w; ry = cursor;
            switch (sh->type) {
                case SHP_TITLE:   rh = cap + 6; break;
                case SHP_TEXTBOX: rh = sh->npara * 6 + 2; break;
                case SHP_IMAGE:   rh = rw / 3; break;
                default:          rh = 10; break;
            }
        }
        if (ry + rh > y + h) rh = y + h - ry;
        if (rh <= 0) continue;
        switch (sh->type) {
            case SHP_TITLE: {
                char t[160]; shape_text(sh, t, sizeof(t));
                if (!t[0]) break;
                int tw = gui_ttf_render_width(t, cap);
                int max_w = rw - 4;
                if (tw > max_w) {
                    int n = (int)strlen(t);
                    while (n > 1 && gui_ttf_render_width(t, cap) > max_w) { t[--n] = 0; if (n >= 2) { t[n - 1] = '.'; t[n - 2] = '.'; } }
                    tw = gui_ttf_render_width(t, cap);
                }
                int tx = rx + (rw - tw) / 2; if (tx < rx) tx = rx;
                int ty = explicit_geom ? ry + (rh - cap) / 2 : ry;
                if (ty + cap <= y + h) win_draw_text_ttf(win, tx, ty, t, cap, ink);
                break;
            }
            case SHP_TEXTBOX: {
                int ly = ry + (explicit_geom ? 4 : 0);
                for (int k = 0; k < sh->npara && ly + 2 <= ry + rh; k++) {
                    char one[128]; para_concat(&sh->paras[k], one, sizeof(one));
                    int len = (int)strlen(one);
                    if (!len) { ly += 6; continue; }
                    int bw = rw * (len > 30 ? 90 : 25 + len * 2) / 100; if (bw > rw - 6) bw = rw - 6;
                    int indent = sh->paras[k].list_level >= 0 ? 4 : 0;
                    win_draw_rect(win, rx + 3 + indent, ly, bw - indent, 2, bar);
                    ly += 6;
                }
                break;
            }
            case SHP_RECT:
                win_draw_rect(win, rx, ry, rw, rh, sh->fill ? sh->fill : 0x00CCCCCC);
                break;
            case SHP_IMAGE:
                win_draw_rect(win, rx, ry, rw, rh, imgfill);
                break;
            default: break;
        }
        if (!explicit_geom) cursor = ry + rh + 3;
    }
}

static void draw(office_app *a) {
    int win = oapp_win(a);
    // [filesui] zero test removed: it overrode a legitimately black window_bg
    // with light grey. See libc/theme.h.
    uint32_t bg = theme_color(THEME_COLOR_WINDOW_BG);
    win_draw_rect(win, g_view_x, g_view_y, g_view_w, g_view_h, bg);
    if (g_show_thumbs) othumbs_draw(win, &g_thumbs, thumb_render, 0);
    draw_slide_sheet(win, 1);
}

// --- Present mode (spec 2.3: F5 / SLPLAY; Right/Space/PgDn next, Left/PgUp previous, Esc)
// A full-window slideshow: the window is maximised (restored on exit) and the
// slide is drawn fit-to-window on black over the WHOLE window, chrome
// included, from a nested event loop on the app window (the same technique
// the shell's blocking dialogs use). Native fullscreen (SYS_WM_FULLSCREEN_ENTER,
// #158) is deliberately NOT used: it force-exits on focus loss and its
// compositor watchdog is keyed on the content-commit sequence, which a static
// slide (no commits between key presses) would trip. See CHANGELOG.
static void present_draw(int win, int w, int h) {
    win_draw_rect(win, 0, 0, w, h, 0x00000000);
    fit_slide(0, 0, w, h, 0);
    draw_slide_sheet(win, 0);
    // Slide counter, bottom-right, small and dim.
    char c[32]; snprintf(c, sizeof(c), "%d / %d", g_cur_slide + 1, g_pres ? g_pres->nslide : 0);
    int cap = 11; int tw = gui_ttf_render_width(c, cap);
    win_draw_text_ttf(win, w - tw - 8, h - cap - 6, c, cap, 0x00707070);
    win_invalidate(win);
}

static void present_run(void) {
    if (!g_pres || g_pres->nslide <= 0) return;
    int win = oapp_win(g_app);
    int was_max = (win_get_state(win) & WIN_STATE_MAXIMIZED) != 0;
    if (!was_max) sys_wm_maximize_focused();
    g_presenting = 1;
    int prev_sel = g_sel_shape; g_sel_shape = -1;
    int w = 0, h = 0;
    int done = 0;
    // The image cache is sized to the editor's sheet; rebuild at the present
    // size once, then again on any resize (never inside the draw itself).
    win_get_size(win, &w, &h); if (w <= 0 || h <= 0) oapp_size(g_app, &w, &h);
    fit_slide(0, 0, w, h, 0); build_image_cache();
    present_draw(win, w, h);
    while (!done) {
        gui_event_t ev;
        int et = gui_mods_next_event(win, &ev, -1);
        if (et <= 0) continue;
        int redraw = 0;
        switch (ev.type) {
            case EVENT_WINDOW_CLOSE: done = 1; g_quit = 1; break;
            case EVENT_RESIZE:
                if (ev.mouse_x > 0) w = ev.mouse_x;
                if (ev.mouse_y > 0) h = ev.mouse_y;
                fit_slide(0, 0, w, h, 0); build_image_cache();
                redraw = 1;
                break;
            case EVENT_REDRAW: redraw = 1; break;
            case EVENT_KEY_DOWN: {
                uint32_t kc = ev.keycode; char c = ev.key_char;
                if (c == GUI_KEY_ESC) { done = 1; break; }
                int before = g_cur_slide;
                if (kc == GUI_KEY_RIGHT || kc == GUI_KEY_DOWN || kc == GUI_KEY_PGDN || c == ' ' || c == GUI_KEY_ENTER || kc == GUI_KEY_ENTER) {
                    if (g_cur_slide < g_pres->nslide - 1) g_cur_slide++;
                    else done = 1;   // past the last slide: leave the show
                } else if (kc == GUI_KEY_LEFT || kc == GUI_KEY_UP || kc == GUI_KEY_PGUP || c == GUI_KEY_BKSP) {
                    if (g_cur_slide > 0) g_cur_slide--;
                } else if (kc == GUI_KEY_HOME) g_cur_slide = 0;
                else if (kc == GUI_KEY_END) g_cur_slide = g_pres->nslide - 1;
                if (g_cur_slide != before) { build_image_cache(); redraw = 1; }
                break;
            }
            case EVENT_MOUSE_DOWN:
                if (g_cur_slide < g_pres->nslide - 1) { g_cur_slide++; build_image_cache(); redraw = 1; }
                else done = 1;
                break;
            default: break;
        }
        if (!done && redraw) present_draw(win, w, h);
    }
    g_presenting = 0;
    g_sel_shape = prev_sel;
    if (!was_max) sys_wm_maximize_focused();   // toggle back; the restore EVENT_RESIZE reaches the shell loop
    g_thumbs.sel = g_cur_slide;
    relayout();
    sync_chrome();
}

// --- Input ------------------------------------------------------------------------------
static int canvas_hit_shape(int mx, int my) {
    // Topmost (last drawn) shape wins.
    for (int j = g_shape_rc_n - 1; j >= 0; j--) {
        rect_t r = g_shape_rc[j];
        if (mx >= r.x - 2 && mx < r.x + r.w + 2 && my >= r.y - 2 && my < r.y + r.h + 2) return j;
    }
    return -1;
}

static void handle_mouse_down(int mx, int my) {
    if (g_show_thumbs) {
        int r = othumbs_press(&g_thumbs, mx, my);
        if (r >= 0) { goto_slide(r); return; }
        if (r == -2) return;
    }
    if (mx >= g_view_x && mx < g_view_x + g_view_w && my >= g_view_y && my < g_view_y + g_view_h) {
        int hit = canvas_hit_shape(mx, my);
        if (hit != g_sel_shape) { g_sel_shape = hit; sync_chrome(); }
    }
}

// Forward-declared: the contract's setter for view.thumbnails (defined with
// the rest of the contract, below on_event()), reused here so the View menu
// and the contract row cannot disagree.
static int ct_set_show_thumbs(int v);

// T_PREV/T_NEXT stay special-cased (see TB_ITEMS): they move RELATIVE to
// g_cur_slide, which no afn here expresses (slide.goto's afn is absolute-
// index-only, matching the ORIGINAL static table, which never had a
// separate relative-move token either). Every other id is table-driven.
static int handle_tool(int id) {
    switch (id) {
        case T_PREV: goto_slide(g_cur_slide - 1); return g_quit ? 0 : 1;
        case T_NEXT: goto_slide(g_cur_slide + 1); return g_quit ? 0 : 1;
        default: break;
    }
    char scratch[80];
    otb_ctl_dispatch(TB_ITEMS, TB_N, id, -1, scratch, sizeof(scratch), 0);
    return g_quit ? 0 : 1;
}

// M_VIEW_THUMBS stays special-cased: it toggles the view.thumbnails VALUE
// row, not a separate action token, same as writer's view.ruler. Every
// other menu item is table-driven.
static int handle_menu(int id) {
    if (id == M_VIEW_THUMBS) {
        ct_set_show_thumbs(!g_show_thumbs);   // same setter view.thumbnails calls
        return g_quit ? 0 : 1;
    }
    char scratch[80];
    if (menu_ctl_dispatch(FILE_ITEMS, (int)(sizeof(FILE_ITEMS)/sizeof(FILE_ITEMS[0])), id, scratch, sizeof(scratch), 0)) return g_quit ? 0 : 1;
    if (menu_ctl_dispatch(EDIT_ITEMS, (int)(sizeof(EDIT_ITEMS)/sizeof(EDIT_ITEMS[0])), id, scratch, sizeof(scratch), 0)) return g_quit ? 0 : 1;
    if (menu_ctl_dispatch(INSERT_ITEMS, (int)(sizeof(INSERT_ITEMS)/sizeof(INSERT_ITEMS[0])), id, scratch, sizeof(scratch), 0)) return g_quit ? 0 : 1;
    if (menu_ctl_dispatch(FORMAT_ITEMS, (int)(sizeof(FORMAT_ITEMS)/sizeof(FORMAT_ITEMS[0])), id, scratch, sizeof(scratch), 0)) return g_quit ? 0 : 1;
    if (menu_ctl_dispatch(VIEW_ITEMS, (int)(sizeof(VIEW_ITEMS)/sizeof(VIEW_ITEMS[0])), id, scratch, sizeof(scratch), 0)) return g_quit ? 0 : 1;
    return g_quit ? 0 : 1;
}

static int handle_key(int p1, int p2) {
    uint32_t kc = (uint32_t)p1; char c = (char)p2;
    gui_event_t ev; memset(&ev, 0, sizeof(ev)); ev.type = EVENT_KEY_DOWN; ev.keycode = kc; ev.key_char = c;
    if (gui_mods_is(GUI_MOD_CTRL)) {
        int L = gui_mods_letter(&ev);
        switch (L) {
            case 'n': do_new(); return 1;
            case 'o': do_open(); return 1;
            case 's': do_save(0); return 1;
            case 'b': toggle_style(0); return 1;
            case 'i': toggle_style(1); return 1;
            case 'm': modal_new_slide(); return 1;
            default: break;
        }
        if (kc == GUI_KEY_PGUP) { goto_slide(g_cur_slide - 1); return 1; }
        if (kc == GUI_KEY_PGDN) { goto_slide(g_cur_slide + 1); return 1; }
        return 1;
    }
    if (kc == GUI_KEY_F5) { present_run(); return g_quit ? 0 : 1; }
    if (c == GUI_KEY_ESC) { if (g_sel_shape >= 0) { g_sel_shape = -1; sync_chrome(); } return 1; }
    if (kc == GUI_KEY_DEL) { if (g_sel_shape >= 0) delete_selected_shape(); return 1; }
    // Strip navigation keys (Up/Down/PgUp/PgDn/Home/End) go through othumbs so
    // the selection and the scroll position stay together; Left/Right/Space
    // are the presenter-style prev/next.
    if (g_show_thumbs && othumbs_key(&g_thumbs, kc)) { othumbs_reveal(&g_thumbs); goto_slide(g_thumbs.sel); return 1; }
    if (kc == GUI_KEY_PGDN || kc == GUI_KEY_RIGHT || kc == GUI_KEY_DOWN || c == ' ') goto_slide(g_cur_slide + 1);
    else if (kc == GUI_KEY_PGUP || kc == GUI_KEY_LEFT || kc == GUI_KEY_UP) goto_slide(g_cur_slide - 1);
    else if (kc == GUI_KEY_HOME) goto_slide(0);
    else if (kc == GUI_KEY_END) goto_slide(g_pres ? g_pres->nslide - 1 : 0);
    return 1;
}

#ifdef SLIDES_VERIFY_AUTOMODAL
static int g_verify_ticks;
#endif

static int on_event(office_app *a, int type, int p1, int p2) {
    (void)a;
    switch (type) {
        case OAPP_EVENT_TOOL:      return handle_tool(p1);
        case OAPP_EVENT_MENU:      return handle_menu(p1);
        case OAPP_EVENT_FILE_NEW:  do_new(); return 1;
        case OAPP_EVENT_FILE_OPEN: do_open(); return 1;
        case OAPP_EVENT_FILE_SAVE: do_save(0); return 1;
        case OAPP_EVENT_FILE_SAVE_AS: do_save(1); return 1;
        case OAPP_EVENT_FILE_CLOSE:
            if (!confirm_discard("closing")) return 1;
            new_presentation();
            return 1;
        case EVENT_WINDOW_CLOSE:
            if (!confirm_discard("closing")) return 1;
            return 0;
        case EVENT_RESIZE:
            relayout();
            return 1;
        case EVENT_MOUSE_SCROLL:
            // (a,b) = (scroll_delta, mouse_y); the event carries no x, so the
            // last motion x decides whether the wheel scrolls the strip.
            if (g_show_thumbs && g_mouse_x >= g_strip_x && g_mouse_x < g_strip_x + g_strip_w) {
                othumbs_wheel(&g_thumbs, g_mouse_x, p2, p1);
                return 1;
            }
            if (p1 > 0) goto_slide(g_cur_slide - 1); else if (p1 < 0) goto_slide(g_cur_slide + 1);
            return 1;
        case EVENT_KEY_DOWN:
            return handle_key(p1, p2);
        case EVENT_MOUSE_DOWN:
            g_mouse_x = p1; g_mouse_y = p2;
            handle_mouse_down(p1, p2);
            return 1;
        case EVENT_MOUSE_UP:
            othumbs_release(&g_thumbs);
            return 1;
        case EVENT_MOUSE_MOVE:
            g_mouse_x = p1; g_mouse_y = p2;
            if (g_show_thumbs) othumbs_motion(&g_thumbs, p1, p2);
            return 1;
        case OAPP_EVENT_TICK:
#ifdef SLIDES_VERIFY_AUTOMODAL
            // THROWAWAY VERIFICATION BUILD ONLY (never in the landed source):
            // opens the Slide Layout picker by itself after N ticks so a
            // headless screendump can show the modal without a mouse or a
            // key reaching the guest (#334/#440).
            if (++g_verify_ticks == SLIDES_VERIFY_AUTOMODAL) modal_slide_layout();
#endif
            return 1;
        default:
            return 1;
    }
}

// ===========================================================================
// THE CONTRACT (docs/CONTRACT_COVERAGE_PLAN.md task 3 phase 4). See
// writer/main.c's own contract block for the full argument; the summary:
//
// PROJECTED, not static, for every action with a toolbar or menu control:
// otb_item_t/gui_menu_item_t now carry tok/afn/desc/terms, so TB_ITEMS[],
// FILE_ITEMS[] and EDIT_ITEMS[] above are the SAME control tables
// slides_project() (below) reads, the exact tables handle_tool()/
// handle_menu() dispatch from via otb_ctl_dispatch()/menu_ctl_dispatch().
// Fourteen rows moved from a hand-written SLIDES_ITEMS[] entry into this
// projection (file.new/open/save/save_as/close, slide.new_dialog/
// layout_dialog/duplicate/delete, insert.textbox/image,
// format.toggle_bold/italic, view.present), each still calling the
// IDENTICAL run_*() function it always did.
//
// FIVE ROWS REMAIN STATIC in SLIDES_ITEMS below: file.save_to, file.open_path,
// slide.goto, shape.select have no toolbar/menu control at all (slide.goto's
// absolute index has no UI equivalent to T_PREV/T_NEXT's relative move;
// shape.select has none either), and shape.delete_selected is reached only
// by the Delete KEY in handle_key(), never a table row. Each wraps the SAME
// model function the interactive path uses (save_presentation,
// load_presentation, direct g_sel_shape assignment with the same bounds
// check canvas_hit_shape()'s caller applies), so a non-interactive caller
// can author and navigate a deck without a mouse or a file dialog. Slides'
// 13 value rows also stay static, same as every other contracted app.
//
// main() checks contract_is_invocation() right after oapp_create() (see
// writer/main.c's note on why check 8 does not see this ordering, and why it
// is still safe). Dialog-raising actions (file.open/save/save_as/close,
// slide.new_dialog, slide.layout_dialog, insert.image) declare CT_TF_MODAL
// and are refused on this spawn path, allowed against a live instance.
//
// view.present is CT_TIME_UNBOUNDED with no bound ANY caller can honestly be
// given: present_run() holds the process in its own input loop until a
// person exits presentation mode, which could be any length of time. It is
// refused on the spawn path exactly like a MODAL action, and on a live call
// contract_invoke_live()'s OWN bounded wait (CTLIVE_WAIT_MS) will usually
// time out (CT_LIVE_TIMEOUT) while presentation mode keeps running - an
// honest "armed, not observed finishing" outcome, which is why it declares
// CT_SUCC_ARMED rather than claiming the call's return means anything about
// when the audience stopped looking at slide one. NOT exercised by this
// tranche's automated verification for exactly that reason: it would either
// time the harness out or require a person to press Escape.
//
// NOT COVERED: mouse click-to-select-shape, drag-to-move/resize, and the
// thumbnail strip's scroll/drag - gestures rather than discrete controls.
// shape.select and slide.goto are the primitives that reach the same end
// state without them.
// ===========================================================================


// ---- the handler wrappers: each calls the EXACT function the menu/toolbar
// switch calls. ---------------------------------------------------------
static int run_file_new(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    do_new();
    snprintf(out, (size_t)ocap, "has_path=%d slide_count=%d", g_has_path, g_pres ? g_pres->nslide : 0);
    return 0;
}
static int run_file_open(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    do_open();
    snprintf(out, (size_t)ocap, "path=%s", g_has_path ? g_path : "(none)");
    return 0;
}
static int run_file_save(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    int ok = do_save(0);
    snprintf(out, (size_t)ocap, "path=%s", g_has_path ? g_path : "(none)");
    return ok ? 0 : -1;
}
static int run_file_save_as(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    int ok = do_save(1);
    snprintf(out, (size_t)ocap, "path=%s", g_has_path ? g_path : "(none)");
    return ok ? 0 : -1;
}
static int run_file_close(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    if (confirm_discard("closing")) new_presentation();
    snprintf(out, (size_t)ocap, "has_path=%d", g_has_path);
    return 0;
}
static int run_file_save_to(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it;
    if (argc < 1 || !argv[0] || !argv[0][0]) { snprintf(out, (size_t)ocap, "need-path"); return -1; }
    int ok = save_presentation(argv[0]);
    snprintf(out, (size_t)ocap, "path=%s", g_has_path ? g_path : "(none)");
    return ok ? 0 : -1;
}
static int run_file_open_path(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it;
    if (argc < 1 || !argv[0] || !argv[0][0]) { snprintf(out, (size_t)ocap, "need-path"); return -1; }
    if (!confirm_discard("opening another file")) { snprintf(out, (size_t)ocap, "discard-declined"); return -1; }
    load_presentation(argv[0]);
    snprintf(out, (size_t)ocap, "path=%s has_path=%d", g_path, g_has_path);
    return g_has_path ? 0 : -1;
}
static int run_new_slide_dialog(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    int before = g_pres ? g_pres->nslide : 0;
    modal_new_slide();
    snprintf(out, (size_t)ocap, "slide_count=%d current_slide=%d", g_pres ? g_pres->nslide : 0, g_cur_slide);
    return (g_pres && g_pres->nslide > before) ? 0 : -1;
}
static int run_layout_dialog(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    if (!cur_slide()) { snprintf(out, (size_t)ocap, "no-slide"); return -1; }
    modal_slide_layout();
    snprintf(out, (size_t)ocap, "layout=%d", slide_layout(g_cur_slide));
    return 0;
}
static int run_duplicate_slide(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    if (!cur_slide()) { snprintf(out, (size_t)ocap, "no-slide"); return -1; }
    int before = g_pres->nslide;
    duplicate_slide();
    snprintf(out, (size_t)ocap, "slide_count=%d current_slide=%d", g_pres->nslide, g_cur_slide);
    return g_pres->nslide > before ? 0 : -1;
}
static int run_delete_slide(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    if (!g_pres) { snprintf(out, (size_t)ocap, "no-presentation"); return -1; }
    delete_slide();
    snprintf(out, (size_t)ocap, "slide_count=%d current_slide=%d", g_pres->nslide, g_cur_slide);
    return 0;
}
static int run_goto_slide(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it;
    if (argc < 1 || !argv[0] || !g_pres) { snprintf(out, (size_t)ocap, "need-index"); return -1; }
    int idx = atoi(argv[0]);
    if (idx < 0 || idx >= g_pres->nslide) { snprintf(out, (size_t)ocap, "out-of-range 0..%d", g_pres->nslide - 1); return -1; }
    goto_slide(idx);
    snprintf(out, (size_t)ocap, "current_slide=%d", g_cur_slide);
    return 0;
}
static int run_insert_textbox(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    slide *sl = cur_slide();
    if (!sl) { snprintf(out, (size_t)ocap, "no-slide"); return -1; }
    int before = sl->nshape;
    add_text_box();
    sl = cur_slide();
    snprintf(out, (size_t)ocap, "shape_count=%d selected=%d", sl ? sl->nshape : 0, g_sel_shape);
    return (sl && sl->nshape > before) ? 0 : -1;
}
static int run_insert_image(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    slide *sl = cur_slide();
    if (!sl) { snprintf(out, (size_t)ocap, "no-slide"); return -1; }
    int before = sl->nshape;
    insert_image();
    sl = cur_slide();
    snprintf(out, (size_t)ocap, "shape_count=%d selected=%d", sl ? sl->nshape : 0, g_sel_shape);
    return (sl && sl->nshape > before) ? 0 : -1;
}
static int run_delete_selected_shape(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    slide *sl = cur_slide();
    if (!sl || g_sel_shape < 0) { snprintf(out, (size_t)ocap, "no-selection"); return -1; }
    delete_selected_shape();
    sl = cur_slide();
    snprintf(out, (size_t)ocap, "shape_count=%d", sl ? sl->nshape : 0);
    return 0;
}
// A caller-named shape pick with no click: the SAME g_sel_shape assignment
// canvas_hit_shape()'s caller applies, with the SAME bound check.
static int run_select_shape(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it;
    slide *sl = cur_slide();
    if (!sl) { snprintf(out, (size_t)ocap, "no-slide"); return -1; }
    if (argc < 1 || !argv[0]) { snprintf(out, (size_t)ocap, "need-index"); return -1; }
    int idx = atoi(argv[0]);
    if (idx < -1 || idx >= sl->nshape) { snprintf(out, (size_t)ocap, "out-of-range -1..%d", sl->nshape - 1); return -1; }
    g_sel_shape = idx;
    sync_chrome();
    snprintf(out, (size_t)ocap, "selected=%d", g_sel_shape);
    return 0;
}
static int run_toggle_bold(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    if (!sel_shape()) { snprintf(out, (size_t)ocap, "no-text-shape-selected"); return -1; }
    toggle_style(0);
    const doc_runfmt *f = shape_first_fmt(sel_shape());
    snprintf(out, (size_t)ocap, "bold=%d", f ? f->bold : 0);
    return 0;
}
static int run_toggle_italic(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    if (!sel_shape()) { snprintf(out, (size_t)ocap, "no-text-shape-selected"); return -1; }
    toggle_style(1);
    const doc_runfmt *f = shape_first_fmt(sel_shape());
    snprintf(out, (size_t)ocap, "italic=%d", f ? f->italic : 0);
    return 0;
}
static int run_present(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    present_run();   // blocks in its own input loop until the person exits
    snprintf(out, (size_t)ocap, "presenting=%d", g_presenting);
    return 0;
}

// CT_CHECK_ACTFN: a leading-`it`-parameter mismatch is a warning, not an
// error (contract.h); these turn it into a build-stopping one.
CT_CHECK_ACTFN(run_file_new);
CT_CHECK_ACTFN(run_file_open);
CT_CHECK_ACTFN(run_file_save);
CT_CHECK_ACTFN(run_file_save_as);
CT_CHECK_ACTFN(run_file_close);
CT_CHECK_ACTFN(run_file_save_to);
CT_CHECK_ACTFN(run_file_open_path);
CT_CHECK_ACTFN(run_new_slide_dialog);
CT_CHECK_ACTFN(run_layout_dialog);
CT_CHECK_ACTFN(run_duplicate_slide);
CT_CHECK_ACTFN(run_delete_slide);
CT_CHECK_ACTFN(run_goto_slide);
CT_CHECK_ACTFN(run_insert_textbox);
CT_CHECK_ACTFN(run_insert_image);
CT_CHECK_ACTFN(run_delete_selected_shape);
CT_CHECK_ACTFN(run_select_shape);
CT_CHECK_ACTFN(run_toggle_bold);
CT_CHECK_ACTFN(run_toggle_italic);
CT_CHECK_ACTFN(run_present);

// ---- the readable state a caller checks a control against ------------------
static int ct_get_has_path(void) { return g_has_path; }
static int ct_get_readonly(void) { return g_readonly; }
static int ct_get_dirty(void)    { return g_app ? oapp_is_dirty(g_app) : 0; }
static int ct_get_slide_count(void)    { return g_pres ? g_pres->nslide : 0; }
static int ct_get_current_slide(void)  { return g_cur_slide; }
static int ct_get_current_layout(void) { return (g_pres && g_pres->nslide > 0) ? slide_layout(g_cur_slide) : 0; }
static int ct_get_shape_selected(void) { return g_sel_shape; }
static int ct_get_shape_count(void) { slide *sl = cur_slide(); return sl ? sl->nshape : 0; }
static int ct_get_shape_bold(void)   { const doc_runfmt *f = shape_first_fmt(sel_shape()); return f ? f->bold : 0; }
static int ct_get_shape_italic(void) { const doc_runfmt *f = shape_first_fmt(sel_shape()); return f ? f->italic : 0; }
static int ct_get_presenting(void) { return g_presenting; }
static int ct_str_path(char *o, int n) { snprintf(o, (size_t)n, "%s", g_has_path ? g_path : "(none)"); return 0; }
static int ct_get_show_thumbs(void) { return g_show_thumbs; }
static int ct_set_show_thumbs(int v) {
    g_show_thumbs = v ? 1 : 0;
    VIEW_ITEMS[0].checked = g_show_thumbs ? true : false;
    relayout(); sync_chrome();
    return 0;
}

static const ct_item_t SLIDES_ITEMS[] = {
    // ---- value rows ---------------------------------------------------
    {
      .name = "doc.path",
      .type = CT_STR,
      .access = CT_READ,
      .risk = CT_SAFE,
      .getstrfn = ct_str_path,
      .desc = "Absolute path of the open presentation, or (none) for an unsaved Untitled",
    },
    {
      .name = "doc.has_path",
      .type = CT_BOOL,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 1,
      .getfn = ct_get_has_path,
      .desc = "Whether the open presentation has ever been saved to a path",
    },
    {
      .name = "doc.readonly",
      .type = CT_BOOL,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 1,
      .getfn = ct_get_readonly,
      .desc = "Whether the open presentation was loaded read-only (legacy .ppt)",
    },
    {
      .name = "doc.dirty",
      .type = CT_BOOL,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 1,
      .getfn = ct_get_dirty,
      .desc = "Whether the presentation has unsaved changes",
    },
    {
      .name = "doc.slide_count",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 100000,
      .getfn = ct_get_slide_count,
      .desc = "Number of slides in the open presentation",
    },
    {
      .name = "doc.current_slide",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 100000,
      .getfn = ct_get_current_slide,
      .desc = "Index of the active slide",
    },
    {
      .name = "doc.current_layout",
      .type = CT_ENUM,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 3,
      .options = "Title Slide|Title and Content|Two Content|Blank",
      .getfn = ct_get_current_layout,
      .desc = "Layout of the active slide",
    },
    {
      .name = "shape.selected",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .lo = -1,
      .hi = 1000,
      .getfn = ct_get_shape_selected,
      .desc = "Index of the selected shape on the active slide, -1 for none",
    },
    {
      .name = "shape.count",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 1000,
      .getfn = ct_get_shape_count,
      .desc = "Number of shapes on the active slide",
    },
    {
      .name = "shape.bold",
      .type = CT_BOOL,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 1,
      .getfn = ct_get_shape_bold,
      .desc = "Whether the selected text shape's first run is bold",
    },
    {
      .name = "shape.italic",
      .type = CT_BOOL,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 1,
      .getfn = ct_get_shape_italic,
      .desc = "Whether the selected text shape's first run is italic",
    },
    {
      .name = "view.thumbnails",
      .type = CT_BOOL,
      .access = CT_RW,
      .risk = CT_SAFE,
      .hi = 1,
      .getfn = ct_get_show_thumbs,
      .setfn = ct_set_show_thumbs,
      .desc = "Whether the slide thumbnail strip is shown (View > Thumbnails)",
    },
    {
      .name = "presenting",
      .type = CT_BOOL,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 1,
      .getfn = ct_get_presenting,
      .desc = "Whether presentation mode is currently running",
    },

    // ---- action rows: STATIC because there is no toolbar/menu control to --
    // project them from (see the big comment above). Each still calls the
    // exact function a UI control would call if one existed for it.
    {
      .name = "file.save_to",
      .type = CT_ACTION,
      .access = CT_WRITE,
      .risk = CT_GUARDED,
      .actfn = run_file_save_to,
      .ctx = "fs.write",
      .desc = "Save the presentation to an exact path with no dialog, the same pptx_save/odp_save Save uses",
      .terms = &k_t_save_to,
    },
    {
      .name = "file.open_path",
      .type = CT_ACTION,
      .access = CT_WRITE,
      .risk = CT_GUARDED,
      .actfn = run_file_open_path,
      .ctx = "fs.read",
      .desc = "Open an exact path with no dialog (after an unsaved-changes prompt if dirty), the same office_open_presentation() Open uses",
      .terms = &k_t_open_path,
    },
    {
      .name = "slide.goto",
      .type = CT_ACTION,
      .access = CT_WRITE,
      .risk = CT_SAFE,
      .actfn = run_goto_slide,
      .desc = "Move to the slide at argv[0] (an index)",
      .terms = &k_t_slide_nav,
    },
    {
      .name = "shape.delete_selected",
      .type = CT_ACTION,
      .access = CT_WRITE,
      .risk = CT_SAFE,
      .actfn = run_delete_selected_shape,
      .desc = "Delete the selected shape from the active slide (Delete key)",
      .terms = &k_t_shape_edit,
    },
    {
      .name = "shape.select",
      .type = CT_ACTION,
      .access = CT_WRITE,
      .risk = CT_SAFE,
      .actfn = run_select_shape,
      .desc = "Select the shape at argv[0] (an index, -1 to clear) on the active slide",
      .terms = &k_t_shape_select,
    },
};
#define SLIDES_N_ITEMS ((int)(sizeof(SLIDES_ITEMS) / sizeof(SLIDES_ITEMS[0])))

// slides_project() chains the three control tables that carry a contract
// token, toolbar first.
static int slides_project(int idx, ct_item_t *out) {
    if (idx < 0) return 0;
    int n = otb_ctl_count(TB_ITEMS, TB_N);
    if (idx < n) return otb_ctl_project(TB_ITEMS, TB_N, idx, out);
    idx -= n;
#define SL_MENU_N(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))
    n = menu_ctl_count(FILE_ITEMS, SL_MENU_N(FILE_ITEMS));
    if (idx < n) return menu_ctl_project(FILE_ITEMS, SL_MENU_N(FILE_ITEMS), idx, out);
    idx -= n;
    n = menu_ctl_count(EDIT_ITEMS, SL_MENU_N(EDIT_ITEMS));
    if (idx < n) return menu_ctl_project(EDIT_ITEMS, SL_MENU_N(EDIT_ITEMS), idx, out);
#undef SL_MENU_N
    return 0;
}

static const ct_contract_t SLIDES_CONTRACT = {
    "slides", "Slides",
    "Every File/Edit/Insert/Format/View toolbar and menu action with a "
    "control is PROJECTED from TB_ITEMS[]/FILE_ITEMS[]/EDIT_ITEMS[] "
    "(slides_project(), below), the SAME tables otb_draw()/"
    "gui_menu_bar_draw() and handle_tool()/handle_menu() read and dispatch "
    "from. Five rows stay STATIC in SLIDES_ITEMS below because there is no "
    "table to project them from: file.save_to, file.open_path, slide.goto "
    "and shape.select have no toolbar/menu control at all, and "
    "shape.delete_selected is reached only by the Delete key. Dialog-opening "
    "rows (file.open/save/save_as/close, slide.new_dialog, "
    "slide.layout_dialog, insert.image) declare CT_TF_MODAL and are refused "
    "on a fresh spawn; call them against a live instance. view.present is "
    "CT_TIME_UNBOUNDED (see the block comment above run_present()): refused "
    "on spawn, armed rather than awaited on live, and NOT exercised by this "
    "tranche's automated verification. NOT covered: mouse "
    "click-to-select-shape, drag-to-move/resize, and the thumbnail strip's "
    "scroll/drag - gestures rather than discrete controls - shape.select "
    "and slide.goto are the primitives that reach the same end state "
    "without them.",
    SLIDES_ITEMS, SLIDES_N_ITEMS,
    slides_project,
    0,   // no persisted config to load
    0    // no persisted config to commit
};


int main(int argc, char **argv) {
    g_app = oapp_create("SLIDES", 960, 620);
    if (!g_app) return 1;
    layout_reset_all();
    oapp_set_menus(g_app, MENUS, 5);
    otb_init(&g_tb, TB_ITEMS, TB_N, 0, 0, 960);   // the shell re-lays it out to the real width
    oapp_set_toolbar(g_app, &g_tb);
    othumbs_config(&g_thumbs, 0, 0, OTHUMB_STRIP_W, 100, 0, -1);
    new_presentation();

    // ---- THE CONTRACT, ANSWERED BEFORE oapp_run() IS EVER ENTERED ----------
    // See writer/main.c's own note: contract-lint check 8 does not see this
    // ordering (it matches a literal win_create()/win_create_bg() token,
    // which office_app's oapp_create() is not), but the real property holds:
    // this returns before oapp_run()'s event loop is ever entered, so the
    // process never paints and never blocks.
    if (contract_is_invocation(argc, argv))
        return contract_cli(argc, argv, &SLIDES_CONTRACT);
#ifdef SLIDES_VERIFY_AUTOOPEN
    // THROWAWAY VERIFICATION BUILD ONLY (never in the shipped/landed source,
    // never defined by the normal Makefile target): headless QMP keyboard
    // injection into this VM is unreliable past the first synthesized key per
    // boot, so this bypasses the interactive File>Open dialog to screendump
    // the render path directly (pptx/odp/ppt load -> chrome + shape draw).
    load_presentation(SLIDES_VERIFY_AUTOOPEN);
#ifdef SLIDES_VERIFY_GOTO2
    // Liveness proof for the verification screendump: navigates to slide 2
    // with no interactive input (#334/#440). Never defined by the normal
    // Makefile target.
    goto_slide(1);
#endif
#endif
#ifdef SLIDES_VERIFY_AUTOMODAL
    oapp_set_tick_ms(g_app, 1000);
#endif
    oapp_run(g_app, draw, on_event);
    free_image_cache();
    if (g_pres) pres_free(g_pres);
    oapp_destroy(g_app);
    return 0;
}
