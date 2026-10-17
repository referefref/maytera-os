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
#include "../../libc/gui_filedlg.h"
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
    // [filesui] zero tests removed (see libc/theme.h: 0 is black, not unset).
    // The gui_ensure_contrast() call below is what actually makes this ink
    // readable and it does so for a black MUTED just as well as for a grey one.
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
        // [filesui] Two zero tests removed. The first fired only when the
        // theme's window_bg AND label_text were BOTH black, which it read as
        // "no theme loaded" and is in fact a legal (if unreadable) palette;
        // the kernel's own theme_ensure_all_contrast() pass is what prevents
        // that, not this line. See libc/theme.h: 0 is black, never unset.
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
// File Open / Save As dialogs
// ===========================================================================
// [filesui] THE DIALOG MOVED TO libc/gui_filedlg.c, AND WHY.
//
// About 340 lines of it lived here: the entry scan, the sort, the extension
// filter, the modal card, the nested event loop, the keyboard map. Writer, Calc
// and Slides already shared it, which was right. What was wrong is that it was
// reachable only by an app built on THIS shell, so every app that is not an
// office app grew its own. Measured across userland on 2026-09-30: four more
// fully navigable browsers (apps/paint/ui.c's fb_*, apps/gallery, apps/imgconv,
// apps/ide) and about ten flat single-directory pickers, no two agreeing about
// where to start, plus three separate idioms for "where is home".
//
// It also did not know the user HAD a home. Its start directory was "/DOCS" if
// that existed and "/" otherwise, and "/DOCS" exists on no shipping image and
// was mentioned nowhere else in the tree, so in practice every Open and every
// Save in all three office apps opened at the filesystem root.
//
// So the code is now libc/gui_filedlg.c, it has a Places sidebar driven by the
// same libc/places.h table the Files app's sidebar uses, and it opens on the
// session's Documents folder. These three functions are the office shell's
// adaptor onto it: a host descriptor, because the dialog draws into the app's
// window and has to be able to repaint the app underneath itself.
static void oapp_fd_size(void *c, int *w, int *h) {
    office_app *a = (office_app *)c;
    if (w) *w = a->w;
    if (h) *h = a->h;
}
static void oapp_fd_repaint(void *c) {
    office_app *a = (office_app *)c;
    if (a->last_on_draw) shell_redraw(a, a->last_on_draw);
}
static void oapp_fd_resize(void *c, int w, int h) { oapp__note_resize((office_app *)c, w, h); }

static void oapp_fd_host(office_app *a, filedlg_host_t *h) {
    // ZEROED FIRST, and this is not defensive tidiness: the two callers below
    // declare `filedlg_host_t h;` on the STACK with no initialiser and rely on
    // this function to fill every field. So the moment gui_filedlg.h grows a
    // field, that field is whatever was on the stack, and the first one it grew
    // is a POINTER the chooser dereferences (filedlg_host_t.contract, which the
    // nested loop hands to contract_live_poll). Garbage there is a wild
    // dereference on every Open and every Save in Writer, Sheets and Slides.
    //
    // memset means a field this function has not been taught about reads as
    // "not supplied", which is what a NULL callback already means everywhere
    // else in this struct, rather than as a plausible-looking address.
    memset(h, 0, sizeof(*h));
    h->win = a->win;
    h->ctx = a;
    h->size = oapp_fd_size;
    h->repaint = oapp_fd_repaint;
    h->note_resize = oapp_fd_resize;
    // The office apps declare no ct_contract_t yet (coverage plan task 3 item
    // 2), so there is nothing to serve and the chooser keeps its blocking
    // event wait unchanged. Stated explicitly rather than left to the memset,
    // because this is the line that opts Writer / Sheets / Slides in once they
    // have a contract.
    h->contract = 0;
}

const char *oapp_open_dialog_titled(office_app *a, const char *exts, const char *title) {
    if (!a) return 0;
    filedlg_host_t h; oapp_fd_host(a, &h);
    return filedlg_open(&h, exts, title);
}

const char *oapp_open_dialog(office_app *a, const char *exts) {
    return oapp_open_dialog_titled(a, exts, "Open Document");
}

const char *oapp_save_dialog(office_app *a, const char *defname) {
    if (!a) return 0;
    filedlg_host_t h; oapp_fd_host(a, &h);
    return filedlg_save(&h, defname, "Save Document");
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
