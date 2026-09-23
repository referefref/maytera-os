// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// See docs/OFFICE_SUITE_ARCHITECTURE.md for the module map and contracts.
// NOTE: officelib headers are SELF-SUFFICIENT (raw C types only). Do NOT add
//       #include "types.h" or any libc header here: libc's bool/off_t/pid_t
//       leak into apps carrying legacy compat typedefs and break the userland.
#ifndef OFFICE_OFFICEUI_H
#define OFFICE_OFFICEUI_H
// Shared app shell for Writer/Calc/Slides: window + menubar + status bar +
// File open/save dialogs, so the three apps share chrome and the Settings/Files
// design language. Owner: agents 9/10 (officeui/officeui.c). Thin over gui.h.
typedef struct office_app office_app;

office_app *oapp_create(const char *title, int w, int h);
int         oapp_win(office_app *a);        // window handle for drawing
void        oapp_set_status(office_app *a, const char *msg);
void        oapp_set_title(office_app *a, const char *title);
// Event loop. on_draw is called for redraw; on_event returns 1 to keep running,
// 0 to quit. type/a/b mirror the gui event (type, mouse_x/key, mouse_y).
int         oapp_run(office_app *a, void (*on_draw)(office_app *),
                     int (*on_event)(office_app *, int type, int a, int b));
// Blocking dialogs (return a borrowed static path or NULL if cancelled).
const char *oapp_open_dialog(office_app *a, const char *exts);   // e.g. ".docx.odt"
const char *oapp_save_dialog(office_app *a, const char *defname);
void        oapp_destroy(office_app *a);

// ---------------------------------------------------------------------------
// ADDITIVE (agent 9, officewriter, no-ticket). Nothing above this line was
// touched: every existing declaration keeps its exact signature, so agent 10
// (Calc/Slides) links against this header unchanged. Two things were missing
// to actually build Writer against the frozen contract, flagged here rather
// than smuggled in silently:
//
// 1. oapp_run()'s on_event callback mirrors a *gui_event_t* (type/mouse_x/
//    mouse_y), but the shell owns the File menu (Open/Save/Save As/Close) and
//    the app-specific action lives outside gui.h's event_type_t entirely.
//    These four ids are chosen well outside event_type_t's range (which tops
//    out under 20) so a `switch(type)` can tell a menu action apart from a
//    passed-through window event with zero risk of collision, now or if
//    event_type_t grows. `a`/`b` are always 0 for these. Delivered to
//    on_event() the same way as a normal event - the app does not receive a
//    separate callback for menu actions, it is meant to look like any other
//    dispatch: a File>Open selection arrives as
//    on_event(app, OAPP_EVENT_FILE_OPEN, 0, 0).
#define OAPP_EVENT_FILE_NEW      1000
#define OAPP_EVENT_FILE_OPEN     1001
#define OAPP_EVENT_FILE_SAVE     1002
#define OAPP_EVENT_FILE_SAVE_AS  1003
#define OAPP_EVENT_FILE_CLOSE    1004

// 2. An app needs to know where its own content viewport is (below the shell's
// menubar, above its status bar) to lay out and scroll its document - the
// shell owns those two chrome strips and their heights are a shell
// implementation detail, not something every app should hardcode a second
// copy of. Window-relative; content_w/content_h already exclude both strips.
void        oapp_content_rect(office_app *a, int *x, int *y, int *w, int *h);

// ---------------------------------------------------------------------------
// ADDITIVE (officetoolkit, no-ticket; docs/OFFICE_UI_DESIGN.md section 8.8).
// Nothing above this line changed. The shared office UI toolkit (otoolbar,
// ocombo, omodal, otabs, othumbs, oruler, ofxbar, oicon under
// userland/officelib/officeui/, umbrella header officeui/officetk.h) hooks
// into the shell through these.
//
// Events delivered to on_event(), in the same >=1000 id space as the File
// events above (event_type_t tops out under 20):
#define OAPP_EVENT_TOOL   1100   // a = toolbar item id, b = value: toggle state (0/1),
                                 //     combo index (>=0), or -1 = typed combo text
                                 //     (read it from otb_combo(tb,id)->text)
#define OAPP_EVENT_MENU   1101   // a = app menu item id (menus added via oapp_set_menus)
#define OAPP_EVENT_TICK   1102   // periodic (see oapp_set_tick_ms); a = uptime ms low 31 bits

// Menu item ids RESERVED by the shell inside an app's own gui_menu_t tables:
// these keep arriving as the OAPP_EVENT_FILE_* events above, every other id
// arrives as OAPP_EVENT_MENU with a = id. Ids 1..9 are reserved.
#define OAPP_MID_FILE_OPEN     1
#define OAPP_MID_FILE_SAVE     2
#define OAPP_MID_FILE_SAVE_AS  3
#define OAPP_MID_FILE_CLOSE    4
#define OAPP_MID_FILE_NEW      5
#define OAPP_MID_RESERVED_MAX  9

// The full menu bar (File/Edit/Format/Insert/View ...), replacing the shell's
// default File/Edit. `menus` must outlive the app (a static const table).
// DECLARED ONLY WHEN gui_menu.h HAS BEEN INCLUDED FIRST: gui_menu_t is an
// anonymous-struct typedef that cannot be forward-declared, and this header
// must not pull libc in (see the NOTE at the top). Include gui.h + gui_menu.h
// before officeui.h, as the apps already do.
#ifdef _GUI_MENU_H
void        oapp_set_menus(office_app *a, const gui_menu_t *menus, int n);
#endif

// The toolbar (otoolbar.h). The shell positions it under the menu bar
// (otb_move), lays it out on every resize, draws it, routes its input, draws
// its tooltip/popups last, and subtracts otb_height() from the content rect.
// The app keeps ownership of the otb_toolbar_t (a static is fine) and
// configures items/combos through the otoolbar API. Pass NULL to remove.
struct otb_toolbar;
void        oapp_set_toolbar(office_app *a, struct otb_toolbar *tb);

// Extra chrome strips the APP draws inside on_draw (ruler / formula bar above
// the content, sheet-tab strip below it): the shell reserves the space and
// oapp_content_rect() shrinks accordingly. oapp_chrome_rects() gives back
// the two reserved bands (window coordinates; h = 0 when none).
void        oapp_set_chrome(office_app *a, int extra_top_px, int extra_bottom_px);
void        oapp_chrome_rects(office_app *a, int *top_y, int *top_h, int *bot_y, int *bot_h);

// Open dialog with a caller title ("Insert Image"); same contract as
// oapp_open_dialog() otherwise.
const char *oapp_open_dialog_titled(office_app *a, const char *exts, const char *title);

// Dirty-state tracking. The flag is what the app consults on Close/New/Open/
// window close to decide whether to run omodal_confirm3(). (No window-title
// syscall exists on this kernel, see oapp_set_title(); the "*" title marker
// is applied by oapp_set_title() the moment SYS_WIN_SET_TITLE exists, and
// until then the status bar shows the marker.)
int         oapp_is_dirty(office_app *a);
void        oapp_set_dirty(office_app *a, int dirty);

// Periodic OAPP_EVENT_TICK every `ms` (0 = none, the default). The wait is a
// kernel event-queue timeout (win_get_event), never a poll loop (#426).
void        oapp_set_tick_ms(office_app *a, int ms);

// Current window content size and a full chrome+content repaint of the app's
// last frame with no overlays: what a blocking modal (omodal) paints under
// its scrim, and what an app running its own nested loop should call.
void        oapp_size(office_app *a, int *w, int *h);
void        oapp_repaint(office_app *a);
#endif
