// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// officeui.c - shared app shell for Writer/Calc/Slides (agent 9/10 contract,
// include/officeui.h). Owns: the window, a real menu bar (File: Open/Save/
// Save As/Close; Edit: stub, all items disabled), a status bar, and the
// blocking File Open/Save-As dialogs. Built on the SHARED widgets (gui_menu.h,
// gui_list.h, gui_scroll.h, theme.h, gui_style.h) per docs/UI_STYLE_GUIDE.md -
// no bespoke chrome.
//
// (officetoolkit) The shell additionally hosts the office toolkit's toolbar
// (otoolbar.h): it positions it under the menu bar, draws it, routes its
// mouse/keys, draws its tooltip and popups LAST, delivers activations as
// OAPP_EVENT_TOOL, delivers an app's own menu items as OAPP_EVENT_MENU,
// reserves extra chrome strips (oapp_set_chrome) and drives the tooltip
// timer / OAPP_EVENT_TICK through the event-wait timeout (no poll loop).
//
// KEYBOARD FIRST (#334): headless QMP mouse automation cannot reliably drive
// this UI, so every path here must also work with no mouse at all. Alt+F (or
// Alt+E) opens a top-level menu (gui_menu_alt_open, the same mnemonic the
// Terminal's menu bar uses); an open menu is driven with Up/Down/Left/Right/
// Enter/Esc (gui_menu_key); the file dialogs below are driven with Up/Down to
// move the selection, Enter to activate, Tab to move focus to the filename
// field (Save dialog only), and Esc to cancel. Alt+T moves keyboard focus
// into the toolbar (Left/Right walk, Space activates, Esc leaves). The mouse
// paths are additive, not the only path.
#include "../../libc/gui.h"          // types/syscall/keys/gui_mods/gui_style/textfield
#include "../../libc/gui_menu.h"     // BEFORE officeui.h: oapp_set_menus() is declared only once gui_menu_t exists
#include "../../libc/gui_list.h"
#include "../../libc/theme.h"
#include "officeui.h"
#include "officeui_priv.h"
#include "otoolbar.h"
#include "oui.h"
#include "../../libc/dirent.h"
#include "../../libc/string.h"
#include "../../libc/stdio.h"
#include <stdlib.h>

// --- Shell chrome geometry (see officeui.h's ADDITIVE oapp_content_rect) ----
#define OAPP_MENU_H    28
#define OAPP_STATUS_H  26

struct office_app {
    int  win;
    int  w, h;
    char title[128];
    char status[160];
    gui_menu_bar_t menu;
    // Remembered so a blocking dialog (which runs its own nested event loop
    // on the SAME window) can repaint the app's own last frame underneath the
    // dialog overlay, and so it can restore it cleanly once the dialog closes.
    void (*last_on_draw)(office_app *);
    // (officetoolkit) toolkit hooks.
    otb_toolbar_t *tb;                 // app-owned, NULL when none
    int  extra_top, extra_bottom;      // oapp_set_chrome strips
    int  dirty;
    int  tick_ms;
    const gui_menu_t *app_menus; int app_menu_n;
};

// ---------------------------------------------------------------------------
// Menu bar: File (real) + Edit (stub - every item disabled, pure decoration,
// same convention as a real app that has not built Undo/Cut/Copy/Paste yet).
// ---------------------------------------------------------------------------
enum { OUI_ID_FILE_OPEN = OAPP_MID_FILE_OPEN, OUI_ID_FILE_SAVE = OAPP_MID_FILE_SAVE,
       OUI_ID_FILE_SAVE_AS = OAPP_MID_FILE_SAVE_AS, OUI_ID_FILE_CLOSE = OAPP_MID_FILE_CLOSE };

static gui_menu_item_t FILE_ITEMS[] = {
    { "Open...",    "Ctrl+O", OUI_ID_FILE_OPEN,    true,  false },
    { "Save",       "Ctrl+S", OUI_ID_FILE_SAVE,    true,  false },
    { "Save As...", 0,        OUI_ID_FILE_SAVE_AS, true,  false },
    { 0, 0, 0, false, false },   // separator
    { "Close",      0,        OUI_ID_FILE_CLOSE,   true,  false },
};
static gui_menu_item_t EDIT_ITEMS[] = {
    { "Undo",  "Ctrl+Z", 0, false, false },
    { "Cut",   "Ctrl+X", 0, false, false },
    { "Copy",  "Ctrl+C", 0, false, false },
    { "Paste", "Ctrl+V", 0, false, false },
};
static const gui_menu_t OAPP_MENUS[] = {
    { "File", FILE_ITEMS, 5 },
    { "Edit", EDIT_ITEMS, 4 },
};

// A menu id -> the event delivered to the app. The shell's File ids (1..5)
// map to the OAPP_EVENT_FILE_* events; any other id from an app-supplied
// table arrives as OAPP_EVENT_MENU with the id in `a`.
static int oapp_menu_id_to_event(int mid, int *arg) {
    if (arg) *arg = 0;
    switch (mid) {
        case OAPP_MID_FILE_OPEN:    return OAPP_EVENT_FILE_OPEN;
        case OAPP_MID_FILE_SAVE:    return OAPP_EVENT_FILE_SAVE;
        case OAPP_MID_FILE_SAVE_AS: return OAPP_EVENT_FILE_SAVE_AS;
        case OAPP_MID_FILE_CLOSE:   return OAPP_EVENT_FILE_CLOSE;
        case OAPP_MID_FILE_NEW:     return OAPP_EVENT_FILE_NEW;
        default:
            if (mid > OAPP_MID_RESERVED_MAX) { if (arg) *arg = mid; return OAPP_EVENT_MENU; }
            return -1;
    }
}

static void oapp_copy(char *dst, int cap, const char *src) {
    int i = 0;
    if (src) { for (; src[i] && i < cap - 1; i++) dst[i] = src[i]; }
    dst[i] = 0;
}

static void draw_status_bar(office_app *a) {
    int win = a->win, w = a->w, h = a->h;
    uint32_t bg     = theme_color(THEME_COLOR_SURFACE_RAISED);
    uint32_t border = theme_color(THEME_COLOR_WINDOW_BORDER);
    uint32_t ink    = theme_color(THEME_COLOR_MUTED);
    if (!bg)  bg  = 0x00252525;
    if (!ink) ink = 0x00A0A0A0;
    // (officetoolkit) THE SPEC'S CONTRAST FIX (docs/OFFICE_UI_DESIGN.md
    // section 0): THEME_COLOR_MUTED on SURFACE_RAISED raw is #808080 on
    // #C8C8C8 = 2.36:1 on retro_unix, under the 4.5:1 text floor. Floor the
    // muted ink against the ground it actually sits on, the same rule
    // gui_menu_palette_theme() applies to its own inks (resolves to about
    // #5E5E5E on retro).
    ink = gui_ensure_contrast(ink, bg, GUI_FLOOR_TEXT);
    int cap = theme_metric_or(THEME_METRIC_TYPE_CAPTION, 11);
    win_draw_rect(win, 0, h - OAPP_STATUS_H, w, OAPP_STATUS_H, bg);
    win_draw_rect(win, 0, h - OAPP_STATUS_H - 1, w, 1, border);
    const char *msg = a->status[0] ? a->status : "Ready";
    win_draw_text_ttf(win, 12, h - OAPP_STATUS_H + (OAPP_STATUS_H - cap) / 2, msg, cap, ink);
    if (a->dirty) {
        // Unsaved-changes marker, right-aligned (the title bar cannot carry
        // it yet: no SYS_WIN_SET_TITLE, see oapp_set_title()).
        const char *m = "Modified";
        int tw = gui_ttf_render_width(m, cap);
        win_draw_text_ttf(win, w - 12 - tw, h - OAPP_STATUS_H + (OAPP_STATUS_H - cap) / 2, m, cap, ink);
    }
}

static void shell_place_toolbar(office_app *a) {
    if (a->tb) otb_move(a->tb, 0, OAPP_MENU_H, a->w);
}

static int shell_toolbar_h(office_app *a) { return a->tb ? otb_height() : 0; }

// The one place a full chrome+content repaint happens: the closed menu bar,
// the toolbar, the app's own content (on_draw, drawn INTO the rect
// oapp_content_rect() reports), the status bar, then the overlays LAST so
// they sit on top of everything else (toolbar tooltip / MORE popup / open
// combo popup, then the menu popup; same convention as gui_menu_popup_draw's
// own doc comment and every adopter in the tree, e.g. Editor/Terminal).
static void shell_menu_band(office_app *a) {
    // gui_menu_bar_draw() paints only its label boxes (its rect width is the
    // sum of item_w), so the rest of the 28 px band keeps whatever was there
    // last: MEASURED after a modal closed, the scrim's scanlines stayed
    // painted right of "View". Paint the whole band first.
    win_draw_rect(a->win, 0, 0, a->w, OAPP_MENU_H, a->menu.pal.bar_bg);
    gui_menu_bar_draw(a->win, &a->menu);
}

static void shell_redraw(office_app *a, void (*on_draw)(office_app *)) {
    if (!a) return;
    shell_menu_band(a);
    if (a->tb) otb_draw(a->win, a->tb);
    if (on_draw) on_draw(a);
    // Repaint the closed bar (and the toolbar) AGAIN, on top of whatever the
    // app just drew. layout_draw()'s clip contract (scroll_x/scroll_y/vh, no
    // separate viewport-top parameter) cannot express a content region that
    // starts below y=0 without also being told the viewport's own top edge,
    // so an app scrolled deep into a tall document can - in the one-viewport-
    // worth-above-the-fold edge case - hand back a span whose y lands in the
    // chrome band above oapp_content_rect()'s top. Rather than force a
    // signature change onto layout.h for an edge case only this shell needs
    // to defend against, the chrome simply repaints itself last: correct in
    // the common case with zero extra cost, and self-healing in the rare one.
    shell_menu_band(a);
    if (a->tb) otb_draw(a->win, a->tb);
    draw_status_bar(a);
    if (a->tb) otb_draw_overlay(a->win, a->tb, a->w, a->h);
    gui_menu_popup_draw(a->win, &a->menu, a->w, a->h);
    win_invalidate(a->win);
}

office_app *oapp_create(const char *title, int w, int h) {
    office_app *a = (office_app *)calloc(1, sizeof(*a));
    if (!a) return 0;
    a->w = w; a->h = h;
    a->win = win_create(title ? title : "Office", 120, 90, w, h);
    if (a->win < 0) { free(a); return 0; }
    // win_create()'s w/h are the OUTER window size (chrome included, #528) -
    // the real drawable canvas is smaller once the compositor's titlebar/
    // border are subtracted. Re-read the true content size here; trusting
    // the caller's request would draw the status bar (anchored to a->h)
    // below the actual content bottom edge, where the compositor clips it
    // and it silently never appears (measured on VM 2402, screendump).
    { int rw = 0, rh = 0; if (win_get_size(a->win, &rw, &rh) == 0 && rw > 0 && rh > 0) { a->w = rw; a->h = rh; } }
    oapp_copy(a->title, sizeof(a->title), title ? title : "Office");
    a->status[0] = 0;
    // gui_menu_bar_init() themes itself from the ACTIVE theme by default (see
    // gui_menu.c) - no separate palette call needed for the bar.
    gui_menu_bar_init(&a->menu, OAPP_MENUS, 2, 0, 0, OAPP_MENU_H);
    // gui_style.h's palette/style engine is a process-wide singleton (one
    // draw process = one live theme), so setting it here is what makes the
    // app's OWN content drawing (gui_button/gui_card/gui_fill_rounded_aa/...)
    // consistent with this shell's chrome, with nothing further for the app
    // to configure.
    gui_style_sync_from_theme();
    {
        uint32_t bg     = theme_color(THEME_COLOR_WINDOW_BG);
        uint32_t ink    = theme_color(THEME_COLOR_LABEL_TEXT);
        uint32_t accent = theme_color(THEME_COLOR_ACCENT);
        uint32_t border = theme_color(THEME_COLOR_WINDOW_BORDER);
        if (!bg && !ink) { bg = 0x001E1E1E; ink = 0x00FFFFFF; }
        if (!accent) accent = 0x002D7DF6;
        int lum = ((int)((bg >> 16) & 0xFF) * 30 + (int)((bg >> 8) & 0xFF) * 59 + (int)(bg & 0xFF) * 11) / 100;
        uint32_t card = (lum < 128) ? gui_lighten(bg, 16) : 0x00FFFFFF;
        uint32_t dim  = gui_mix(ink, bg, 110);
        if (!border) border = gui_mix(ink, bg, 180);
        gui_palette_t p = { bg, card, ink, dim, accent, gui_lighten(accent, 18),
                            border, card, border, gui_mix(ink, bg, 190), 0, 0 };
        gui_set_palette(&p);
    }
    return a;
}

int oapp_win(office_app *a) { return a ? a->win : -1; }

void oapp_set_status(office_app *a, const char *m) {
    if (!a) return;
    oapp_copy(a->status, sizeof(a->status), m);
}

void oapp_set_title(office_app *a, const char *t) {
    // No handle-based window-title syscall exists on this kernel (win_create()
    // takes the title once, at creation, and there is no SYS_WIN_SET_TITLE).
    // compositor_client.h's comp_window_set_title() talks to a SEPARATE IPC
    // protocol (MSG_SET_TITLE over a channel to a compositor service) that no
    // shipping app actually uses - every app in the tree, including this one,
    // gets its window through win_create()'s syscall path instead, and mixing
    // the two would mean opening a second, parallel channel to a window this
    // process does not own that way. Wiring that up is future work, not a
    // five-minute add; documented here rather than left silently unimplemented
    // in a way that looks finished. The app's chosen title is still tracked
    // (a->title) so a future syscall only needs one call site to use it, and
    // oapp_set_status() remains the one live way to show the current document
    // name to the user today (Writer does this).
    if (a && t) oapp_copy(a->title, sizeof(a->title), t);
}

void oapp_content_rect(office_app *a, int *x, int *y, int *w, int *h) {
    if (!a) return;
    int top = OAPP_MENU_H + shell_toolbar_h(a) + a->extra_top;
    if (x) *x = 0;
    if (y) *y = top;
    if (w) *w = a->w;
    if (h) { int ch = a->h - top - a->extra_bottom - OAPP_STATUS_H; if (ch < 0) ch = 0; *h = ch; }
}

// --- (officetoolkit) shell additions ------------------------------------------
void oapp_set_menus(office_app *a, const gui_menu_t *menus, int n) {
    if (!a) return;
    if (!menus || n <= 0) { menus = OAPP_MENUS; n = 2; }
    if (n > GUI_MENU_MAX_TOP) n = GUI_MENU_MAX_TOP;
    a->app_menus = menus; a->app_menu_n = n;
    gui_menu_bar_init(&a->menu, menus, n, 0, 0, OAPP_MENU_H);
}

void oapp_set_toolbar(office_app *a, struct otb_toolbar *tb) {
    if (!a) return;
    a->tb = tb;
    shell_place_toolbar(a);
}

void oapp_set_chrome(office_app *a, int extra_top_px, int extra_bottom_px) {
    if (!a) return;
    a->extra_top = extra_top_px < 0 ? 0 : extra_top_px;
    a->extra_bottom = extra_bottom_px < 0 ? 0 : extra_bottom_px;
}

void oapp_chrome_rects(office_app *a, int *top_y, int *top_h, int *bot_y, int *bot_h) {
    if (!a) return;
    int ty = OAPP_MENU_H + shell_toolbar_h(a);
    if (top_y) *top_y = ty;
    if (top_h) *top_h = a->extra_top;
    if (bot_y) *bot_y = a->h - OAPP_STATUS_H - a->extra_bottom;
    if (bot_h) *bot_h = a->extra_bottom;
}

int  oapp_is_dirty(office_app *a) { return a ? a->dirty : 0; }
void oapp_set_dirty(office_app *a, int dirty) { if (a) a->dirty = dirty ? 1 : 0; }
void oapp_set_tick_ms(office_app *a, int ms) { if (a) a->tick_ms = ms < 0 ? 0 : ms; }
void oapp_size(office_app *a, int *w, int *h) { if (!a) return; if (w) *w = a->w; if (h) *h = a->h; }
void oapp_repaint(office_app *a) { if (a) shell_redraw(a, a->last_on_draw); }
void oapp__note_resize(office_app *a, int w, int h) {
    if (!a) return;
    if (w > 0) a->w = w;
    if (h > 0) a->h = h;
    shell_place_toolbar(a);
}

// ===========================================================================
// File Open / Save As dialogs - inline modal overlay on the SAME window (the
// Settings/widget-modal convention this codebase already uses: never a
// separate window, never click-away-dismissable, closed only by an explicit
// button/Enter/Esc). BLOCKING: each call runs its own nested win_get_event
// loop and does not return until the user resolves it.
// ===========================================================================
#define OD_MAX_ENTRIES 512
#define OD_ROW_H       22
#define OD_PATH_MAX    400

typedef struct { char name[256]; int is_dir; } od_entry_t;

static od_entry_t   g_od_entries[OD_MAX_ENTRIES];
static int          g_od_count;
static char         g_od_dir[OD_PATH_MAX];
static char         g_od_exts[128];
static gui_list_t   g_od_list;
static int          g_od_sel;
static char         g_od_result[512];
static char         g_od_fname[192];
static textfield_t  g_od_tf;
static int          g_od_tf_focus;
static int          g_od_btn_cancel_x, g_od_btn_ok_x, g_od_btn_y, g_od_btn_w, g_od_btn_h;

static void od_join(char *out, int cap, const char *dir, const char *name) {
    int n = 0;
    for (int i = 0; dir[i] && n < cap - 1; i++) out[n++] = dir[i];
    if (n == 0 || out[n - 1] != '/') { if (n < cap - 1) out[n++] = '/'; }
    for (int i = 0; name[i] && n < cap - 1; i++) out[n++] = name[i];
    out[n] = 0;
}

static void od_parent(char *dir) {
    int n = (int)strlen(dir);
    while (n > 1 && dir[n - 1] == '/') dir[--n] = 0;
    while (n > 1 && dir[n - 1] != '/') n--;
    if (n > 1) dir[n - 1] = 0; else { dir[0] = '/'; dir[1] = 0; }
}

// exts: concatenated dot-prefixed tokens, e.g. ".docx.odt" -> {".docx",".odt"}.
// Empty/NULL means "match everything" (used by the Save dialog's browser).
static int od_ext_match(const char *name, const char *exts) {
    if (!exts || !*exts) return 1;
    int namelen = (int)strlen(name);
    const char *p = exts;
    while (*p) {
        const char *q = p + 1;
        while (*q && *q != '.') q++;
        int toklen = (int)(q - p);
        if (namelen >= toklen) {
            int match = 1;
            for (int i = 0; i < toklen; i++) {
                char x = name[namelen - toklen + i], y = p[i];
                if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
                if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
                if (x != y) { match = 0; break; }
            }
            if (match) return 1;
        }
        p = q;
    }
    return 0;
}

static int od_cmp(const void *pa, const void *pb) {
    const od_entry_t *a = (const od_entry_t *)pa, *b = (const od_entry_t *)pb;
    int ap = !strcmp(a->name, ".."), bp = !strcmp(b->name, "..");
    if (ap != bp) return ap ? -1 : 1;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

static void od_scan(const char *dir, const char *exts) {
    g_od_count = 0;
    if (strcmp(dir, "/") != 0 && g_od_count < OD_MAX_ENTRIES) {
        od_entry_t *e = &g_od_entries[g_od_count++];
        oapp_copy(e->name, sizeof(e->name), "..");
        e->is_dir = 1;
    }
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != 0 && g_od_count < OD_MAX_ENTRIES) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            int isdir = (de->d_type == DT_DIR);
            if (!isdir && !od_ext_match(de->d_name, exts)) continue;
            od_entry_t *e = &g_od_entries[g_od_count++];
            oapp_copy(e->name, sizeof(e->name), de->d_name);
            e->is_dir = isdir;
        }
        closedir(d);
    }
    if (g_od_count > 0) qsort(g_od_entries, g_od_count, sizeof(od_entry_t), od_cmp);
}

static const char *od_label(void *ctx, int index, char *buf, int cap) {
    (void)ctx;
    if (index < 0 || index >= g_od_count) return "";
    od_entry_t *e = &g_od_entries[index];
    if (e->is_dir) snprintf(buf, cap, "%s%s", e->name, !strcmp(e->name, "..") ? "" : "/");
    else           snprintf(buf, cap, "%s", e->name);
    return buf;
}

static void od_enter_dir(od_entry_t *e) {
    if (!strcmp(e->name, "..")) od_parent(g_od_dir);
    else { char nd[OD_PATH_MAX]; od_join(nd, sizeof(nd), g_od_dir, e->name); oapp_copy(g_od_dir, sizeof(g_od_dir), nd); }
    od_scan(g_od_dir, g_od_exts);
    g_od_sel = (g_od_count > 0) ? 0 : -1;
    g_od_list.scroll.offset = 0;
}

// Open-mode activation: a directory navigates; a file confirms (*confirm=1).
static void od_activate_open(int *confirm) {
    if (confirm) *confirm = 0;
    if (g_od_sel < 0 || g_od_sel >= g_od_count) return;
    od_entry_t *e = &g_od_entries[g_od_sel];
    if (e->is_dir) { od_enter_dir(e); return; }
    od_join(g_od_result, sizeof(g_od_result), g_od_dir, e->name);
    if (confirm) *confirm = 1;
}

// Save-mode list activation: a directory navigates; a file copies its name
// into the filename field (does not confirm - the field is authoritative).
static void od_activate_save_list(void) {
    if (g_od_sel < 0 || g_od_sel >= g_od_count) return;
    od_entry_t *e = &g_od_entries[g_od_sel];
    if (e->is_dir) { od_enter_dir(e); return; }
    tf_set_text(&g_od_tf, e->name);
    g_od_tf_focus = 1;
}

static void od_geom(office_app *a, int *cx, int *cy, int *cw, int *ch) {
    int w = a->w, h = a->h;
    int dw = w - 64; if (dw > 560) dw = 560; if (dw < 300) dw = 300;
    int dh = h - 64; if (dh > 440) dh = 440; if (dh < 240) dh = 240;
    *cw = dw; *ch = dh; *cx = (w - dw) / 2; *cy = (h - dh) / 2;
}

static void od_draw(office_app *a, const char *title, int save_mode) {
    int win = a->win, w = a->w, h = a->h;
    // Interlaced-scanline scrim (gui_confirm.c's technique - no framebuffer
    // read-back exists, so this is how a modal dims the page beneath it).
    for (int yy = 0; yy < h; yy += 2) win_draw_rect(win, 0, yy, w, 1, 0x00000000);

    int cx, cy, cw, ch; od_geom(a, &cx, &cy, &cw, &ch);
    uint32_t surface = theme_color(THEME_COLOR_SURFACE_OVERLAY);
    uint32_t border  = theme_color(THEME_COLOR_WINDOW_BORDER);
    uint32_t ink     = theme_color(THEME_COLOR_ON_SURFACE);
    uint32_t dim     = theme_color(THEME_COLOR_MUTED);
    uint32_t fieldbg = theme_color(THEME_COLOR_TEXTBOX_BG);
    int radius = theme_metric(THEME_METRIC_RADIUS_CARD);
    if (!surface) surface = 0x00303030;
    if (!ink) ink = 0x00FFFFFF;
    // (officetoolkit) same floor as the status bar: muted on the overlay card.
    dim = gui_ensure_contrast(dim ? dim : 0x00808080, surface, GUI_FLOOR_TEXT);

    gui_fill_rounded(win, cx, cy, cw, ch, radius, surface);
    gui_draw_rect_outline(win, cx, cy, cw, ch, border);
    win_draw_text_ttf(win, cx + 20, cy + 16, title, 16, ink);
    win_draw_rect(win, cx + 20, cy + 44, cw - 40, 1, border);
    win_draw_text_ttf(win, cx + 20, cy + 52, g_od_dir, 12, dim);

    int list_y = cy + 76;
    int list_h = ch - 76 - 56 - (save_mode ? 36 : 0);
    if (list_h < 40) list_h = 40;
    gui_list_config(&g_od_list, cx + 20, list_y, cw - 40, list_h, OD_ROW_H, g_od_count);
    gui_list_draw(win, &g_od_list, g_od_sel, fieldbg, border, ink,
                 theme_color(THEME_COLOR_SELECTION), theme_color(THEME_COLOR_SELECTION_TEXT),
                 od_label, 0);

    int by = cy + ch - 40;
    if (save_mode) {
        int fy = list_y + list_h + 14;
        win_draw_text_ttf(win, cx + 20, fy, "File name:", 12, dim);
        int fx = cx + 110, fw = cw - 40 - 110;
        gui_textfield_tf(win, fx, fy - 5, fw, 24, g_od_tf.buf, g_od_tf.len, g_od_tf.cursor,
                         g_od_tf.sel_anchor, g_od_tf_focus, "filename.docx");
        by = fy + 30;
    }

    int bw = 90, bh = 28, gap = 12;
    g_od_btn_ok_x = cx + cw - 20 - bw;
    g_od_btn_cancel_x = g_od_btn_ok_x - bw - gap;
    g_od_btn_y = by; g_od_btn_w = bw; g_od_btn_h = bh;
    gui_button(win, g_od_btn_cancel_x, by, bw, bh, "Cancel", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    gui_button(win, g_od_btn_ok_x, by, bw, bh, save_mode ? "Save" : "Open", GUI_BTN_PRIMARY, GUI_ST_NORMAL);
    win_invalidate(win);
}

static int od_hit_button(int mx, int my, int bx) {
    return mx >= bx && mx < bx + g_od_btn_w && my >= g_od_btn_y && my < g_od_btn_y + g_od_btn_h;
}

static void od_start_dir(void) {
    oapp_copy(g_od_dir, sizeof(g_od_dir), "/");
    DIR *t = opendir("/DOCS");
    if (t) { closedir(t); oapp_copy(g_od_dir, sizeof(g_od_dir), "/DOCS"); }
}

const char *oapp_open_dialog_titled(office_app *a, const char *exts, const char *title) {
    if (!a) return 0;
    if (!title || !*title) title = "Open Document";
    oapp_copy(g_od_exts, sizeof(g_od_exts), exts);
    od_start_dir();
    memset(&g_od_list, 0, sizeof(g_od_list));
    od_scan(g_od_dir, g_od_exts);
    g_od_sel = (g_od_count > 0) ? 0 : -1;

    if (a->last_on_draw) shell_redraw(a, a->last_on_draw);
    od_draw(a, title, 0);

    int done = 0, ok = 0;
    while (!done) {
        gui_event_t ev;
        int et = gui_mods_next_event(a->win, &ev, -1);
        if (et <= 0) continue;
        switch (ev.type) {
            case EVENT_WINDOW_CLOSE: done = 1; ok = 0; break;
            case EVENT_RESIZE:
                oapp__note_resize(a, ev.mouse_x, ev.mouse_y);
                if (a->last_on_draw) shell_redraw(a, a->last_on_draw);
                break;
            case EVENT_KEY_DOWN: {
                uint32_t kc = ev.keycode; char c = ev.key_char;
                if (c == GUI_KEY_ESC) { done = 1; ok = 0; }
                else if (c == GUI_KEY_ENTER || kc == GUI_KEY_ENTER) {
                    int confirm = 0; od_activate_open(&confirm);
                    if (confirm) { done = 1; ok = 1; }
                } else if (kc == GUI_KEY_UP)   gui_list_move_sel(&g_od_list, &g_od_sel, -1);
                else if (kc == GUI_KEY_DOWN)   gui_list_move_sel(&g_od_list, &g_od_sel, 1);
                else if (kc == GUI_KEY_PGUP)   gui_list_move_sel(&g_od_list, &g_od_sel, -8);
                else if (kc == GUI_KEY_PGDN)   gui_list_move_sel(&g_od_list, &g_od_sel, 8);
                else if (kc == GUI_KEY_HOME)   gui_list_move_sel(&g_od_list, &g_od_sel, -g_od_count);
                else if (kc == GUI_KEY_END)    gui_list_move_sel(&g_od_list, &g_od_sel, g_od_count);
                break;
            }
            case EVENT_MOUSE_DOWN: {
                int mx = ev.mouse_x, my = ev.mouse_y;
                if (od_hit_button(mx, my, g_od_btn_cancel_x)) { done = 1; ok = 0; break; }
                if (od_hit_button(mx, my, g_od_btn_ok_x)) {
                    int confirm = 0; od_activate_open(&confirm);
                    if (confirm) { done = 1; ok = 1; }
                    break;
                }
                int r = gui_list_press(&g_od_list, mx, my);
                if (r >= 0) {
                    g_od_sel = r;
                    if (r < g_od_count && g_od_entries[r].is_dir) { int c2 = 0; od_activate_open(&c2); }
                }
                break;
            }
            case EVENT_MOUSE_MOVE:   gui_list_motion(&g_od_list, ev.mouse_x, ev.mouse_y); break;
            case EVENT_MOUSE_UP:     gui_list_release(&g_od_list); break;
            case EVENT_MOUSE_SCROLL: gui_list_wheel(&g_od_list, ev.mouse_x, ev.mouse_y, ev.scroll_delta); break;
            default: break;
        }
        if (!done) od_draw(a, title, 0);
    }
    if (a->last_on_draw) shell_redraw(a, a->last_on_draw);
    return ok ? g_od_result : 0;
}

const char *oapp_open_dialog(office_app *a, const char *exts) {
    return oapp_open_dialog_titled(a, exts, "Open Document");
}

const char *oapp_save_dialog(office_app *a, const char *defname) {
    if (!a) return 0;
    g_od_exts[0] = 0;   // Save browser shows every entry; the extension is chosen by the typed name.
    od_start_dir();
    memset(&g_od_list, 0, sizeof(g_od_list));
    od_scan(g_od_dir, g_od_exts);
    g_od_sel = (g_od_count > 0) ? 0 : -1;
    tf_init(&g_od_tf, g_od_fname, sizeof(g_od_fname));
    tf_set_text(&g_od_tf, defname ? defname : "");
    g_od_tf_focus = 1;

    if (a->last_on_draw) shell_redraw(a, a->last_on_draw);
    od_draw(a, "Save Document", 1);

    int done = 0, ok = 0;
    while (!done) {
        gui_event_t ev;
        int et = gui_mods_next_event(a->win, &ev, -1);
        if (et <= 0) continue;
        switch (ev.type) {
            case EVENT_WINDOW_CLOSE: done = 1; ok = 0; break;
            case EVENT_RESIZE:
                oapp__note_resize(a, ev.mouse_x, ev.mouse_y);
                if (a->last_on_draw) shell_redraw(a, a->last_on_draw);
                break;
            case EVENT_KEY_DOWN: {
                uint32_t kc = ev.keycode; char c = ev.key_char;
                if (c == GUI_KEY_ESC) { done = 1; ok = 0; }
                else if (kc == GUI_KEY_TAB) { g_od_tf_focus = !g_od_tf_focus; }
                else if (c == GUI_KEY_ENTER || kc == GUI_KEY_ENTER) {
                    if (g_od_tf.len > 0) {
                        od_join(g_od_result, sizeof(g_od_result), g_od_dir, g_od_tf.buf);
                        done = 1; ok = 1;
                    }
                } else if (kc == GUI_KEY_UP)   gui_list_move_sel(&g_od_list, &g_od_sel, -1);
                else if (kc == GUI_KEY_DOWN)   gui_list_move_sel(&g_od_list, &g_od_sel, 1);
                else if (kc == GUI_KEY_PGUP)   gui_list_move_sel(&g_od_list, &g_od_sel, -8);
                else if (kc == GUI_KEY_PGDN)   gui_list_move_sel(&g_od_list, &g_od_sel, 8);
                else if (g_od_tf_focus)        tf_handle_key(&g_od_tf, &ev);
                break;
            }
            case EVENT_MOUSE_DOWN: {
                int mx = ev.mouse_x, my = ev.mouse_y;
                if (od_hit_button(mx, my, g_od_btn_cancel_x)) { done = 1; ok = 0; break; }
                if (od_hit_button(mx, my, g_od_btn_ok_x)) {
                    if (g_od_tf.len > 0) {
                        od_join(g_od_result, sizeof(g_od_result), g_od_dir, g_od_tf.buf);
                        done = 1; ok = 1;
                    }
                    break;
                }
                int r = gui_list_press(&g_od_list, mx, my);
                if (r >= 0) {
                    g_od_sel = r;
                    if (r < g_od_count && g_od_entries[r].is_dir) od_enter_dir(&g_od_entries[r]);
                    else od_activate_save_list();
                }
                break;
            }
            case EVENT_MOUSE_MOVE:   gui_list_motion(&g_od_list, ev.mouse_x, ev.mouse_y); break;
            case EVENT_MOUSE_UP:     gui_list_release(&g_od_list); break;
            case EVENT_MOUSE_SCROLL: gui_list_wheel(&g_od_list, ev.mouse_x, ev.mouse_y, ev.scroll_delta); break;
            default: break;
        }
        if (!done) od_draw(a, "Save Document", 1);
    }
    if (a->last_on_draw) shell_redraw(a, a->last_on_draw);
    return ok ? g_od_result : 0;
}

// ===========================================================================
// Main event loop
// ===========================================================================
// Deliver a toolbar activation: on_event(app, OAPP_EVENT_TOOL, id, value).
static int shell_deliver_tool(office_app *a, int id, int (*on_ev)(office_app *, int, int, int)) {
    if (id <= 0 || !on_ev) return 1;
    return on_ev(a, OAPP_EVENT_TOOL, id, a->tb ? a->tb->last_value : 0);
}

// Deliver a menu activation (shell File ids or the app's own).
static int shell_deliver_menu(office_app *a, int mid, int (*on_ev)(office_app *, int, int, int)) {
    int arg = 0;
    int oe = oapp_menu_id_to_event(mid, &arg);
    if (oe >= 0 && on_ev) return on_ev(a, oe, arg, 0);
    return 1;
}

int oapp_run(office_app *a, void (*on_draw)(office_app *),
             int (*on_ev)(office_app *, int, int, int)) {
    if (!a) return -1;
    a->last_on_draw = on_draw;
    int running = 1;
    shell_place_toolbar(a);
    shell_redraw(a, on_draw);
    unsigned long long last_tick = uptime_ms();

    while (running) {
        gui_event_t ev;
        // The wait timeout is the toolbar's tooltip timer or the app's tick
        // period, whichever is sooner; -1 (block forever) when neither is
        // armed. This is a kernel wait-queue timeout inside win_get_event,
        // not a poll loop (#426).
        int timeout = -1;
        unsigned long long now = uptime_ms();
        if (a->tb) { int t = otb_timer_ms(a->tb, now); if (t >= 0) timeout = t; }
        if (a->tick_ms > 0) {
            unsigned long long el = now - last_tick;
            int t = (el >= (unsigned long long)a->tick_ms) ? 1 : (int)(a->tick_ms - el);
            if (timeout < 0 || t < timeout) timeout = t;
        }
        int et = gui_mods_next_event(a->win, &ev, timeout);
        if (et < 0) continue;
        int redraw_needed = 0;
        if (et == 0) {
            // Timeout: tooltip timer and/or app tick.
            now = uptime_ms();
            if (a->tb && otb_tick(a->tb, now)) redraw_needed = 1;
            if (a->tick_ms > 0 && now - last_tick >= (unsigned long long)a->tick_ms) {
                last_tick = now;
                running = on_ev ? on_ev(a, OAPP_EVENT_TICK, (int)(now & 0x7FFFFFFF), 0) : 1;
                redraw_needed = 1;
            }
            if (running && redraw_needed) shell_redraw(a, on_draw);
            continue;
        }

        switch (ev.type) {
            case EVENT_WINDOW_CLOSE:
                running = on_ev ? on_ev(a, ev.type, 0, 0) : 0;
                break;
            case EVENT_WINDOW_FOCUS:
                running = on_ev ? on_ev(a, ev.type, 0, 0) : 1;
                break;
            case EVENT_WINDOW_BLUR:
                // The kernel emits no motion once the pointer leaves this
                // window, so a hovered/open menu would otherwise freeze
                // exactly as it was (same fix editor/terminal apply).
                gui_menu_leave(&a->menu);
                if (a->tb) otb_leave(a->tb);
                running = on_ev ? on_ev(a, ev.type, 0, 0) : 1;
                redraw_needed = 1;
                break;
            case EVENT_RESIZE:
                if (ev.mouse_x > 0) a->w = ev.mouse_x;
                if (ev.mouse_y > 0) a->h = ev.mouse_y;
                shell_place_toolbar(a);
                running = on_ev ? on_ev(a, ev.type, ev.mouse_x, ev.mouse_y) : 1;
                redraw_needed = 1;
                break;
            case EVENT_REDRAW:
                redraw_needed = 1;
                break;
            case EVENT_MOUSE_DOWN: {
                // Order: an open menu popup, then an open toolbar popup, then
                // the menu bar, then the toolbar, then the app.
                if (a->tb && !gui_menu_is_open(&a->menu) && otb_popup_open(a->tb)) {
                    int r = otb_press(a->tb, ev.mouse_x, ev.mouse_y, a->w, a->h);
                    if (r > 0) running = shell_deliver_tool(a, r, on_ev);
                    redraw_needed = 1;
                    break;
                }
                int mid = gui_menu_bar_click(&a->menu, ev.mouse_x, ev.mouse_y, a->w, a->h);
                if (mid != -1) {
                    if (mid >= 0) running = shell_deliver_menu(a, mid, on_ev);
                    redraw_needed = 1;
                    break;
                }
                if (a->tb) {
                    int r = otb_press(a->tb, ev.mouse_x, ev.mouse_y, a->w, a->h);
                    if (r != -1) {
                        if (r > 0) running = shell_deliver_tool(a, r, on_ev);
                        redraw_needed = 1;
                        break;
                    }
                }
                running = on_ev ? on_ev(a, ev.type, ev.mouse_x, ev.mouse_y) : 1;
                redraw_needed = 1;
                break;
            }
            case EVENT_MOUSE_MOVE:
                if (gui_menu_is_open(&a->menu)) {
                    if (gui_menu_motion(&a->menu, ev.mouse_x, ev.mouse_y, a->w, a->h)) redraw_needed = 1;
                } else {
                    if (a->tb && otb_motion(a->tb, ev.mouse_x, ev.mouse_y)) redraw_needed = 1;
                    if (a->tb && otb_popup_open(a->tb)) { redraw_needed = 1; break; }
                    running = on_ev ? on_ev(a, ev.type, ev.mouse_x, ev.mouse_y) : 1;
                    redraw_needed = 1;
                }
                break;
            case EVENT_MOUSE_UP: {
                gui_menu_release(&a->menu);
                if (a->tb) {
                    int r = otb_release(a->tb, ev.mouse_x, ev.mouse_y);
                    if (r != -1) {
                        if (r > 0) running = shell_deliver_tool(a, r, on_ev);
                        redraw_needed = 1;
                        break;
                    }
                }
                running = on_ev ? on_ev(a, ev.type, ev.mouse_x, ev.mouse_y) : 1;
                redraw_needed = 1;
                break;
            }
            case EVENT_MOUSE_SCROLL:
                if (gui_menu_is_open(&a->menu)) {
                    if (gui_menu_wheel(&a->menu, ev.mouse_x, ev.mouse_y, a->w, a->h, ev.scroll_delta))
                        redraw_needed = 1;
                } else if (a->tb && otb_popup_open(a->tb)) {
                    if (otb_wheel(a->tb, ev.mouse_x, ev.mouse_y, ev.scroll_delta)) redraw_needed = 1;
                } else {
                    // (a,b) = (scroll_delta, mouse_y) for this event type - see
                    // officeui.h's mapping table above oapp_run().
                    running = on_ev ? on_ev(a, ev.type, (int)ev.scroll_delta, ev.mouse_y) : 1;
                    redraw_needed = 1;
                }
                break;
            case EVENT_KEY_DOWN: {
                if (gui_menu_is_open(&a->menu)) {
                    int mid = gui_menu_key(&a->menu, ev.keycode, ev.key_char);
                    if (mid >= 0) running = shell_deliver_menu(a, mid, on_ev);
                    redraw_needed = 1;
                    break;
                }
                if (a->tb && (otb_popup_open(a->tb) || a->tb->focus >= 0)) {
                    int r = otb_key(a->tb, ev.keycode, ev.key_char, a->w, a->h);
                    if (r != -1) {
                        if (r > 0) running = shell_deliver_tool(a, r, on_ev);
                        redraw_needed = 1;
                        break;
                    }
                }
                if (gui_mods_is(GUI_MOD_ALT)) {
                    int L = gui_mods_letter(&ev);
                    if (L && gui_menu_alt_open(&a->menu, L)) { redraw_needed = 1; break; }
                    if (L == 't' && a->tb) { otb_focus_enter(a->tb); redraw_needed = 1; break; }
                }
                running = on_ev ? on_ev(a, ev.type, (int)ev.keycode, (int)ev.key_char) : 1;
                redraw_needed = 1;
                break;
            }
            case EVENT_KEY_UP:
                running = on_ev ? on_ev(a, ev.type, (int)ev.keycode, (int)ev.key_char) : 1;
                break;
            default:
                break;
        }

        if (running && redraw_needed) shell_redraw(a, on_draw);
    }
    return 0;
}

void oapp_destroy(office_app *a) {
    if (!a) return;
    if (a->win >= 0) win_destroy(a->win);
    free(a);
}
