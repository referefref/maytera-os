// contextmenu.c - Right-click context menu for MayteraOS userland compositor
// Renders a popup menu at the cursor position and dispatches actions on click.
// No dynamic allocation: all state lives in fixed-size static arrays.

#include "compositor.h"
#include "../../libc/syscall.h"

// ============================================================================
// Action IDs
// ============================================================================

#define CTX_ACTION_NEW_FOLDER   1
#define CTX_ACTION_NEW_FILE     2
#define CTX_ACTION_REFRESH      3
// (#745) 4 (Paste) and 7 (Properties) are RETIRED, not reused. Both rendered as
// enabled menu items and dispatched to a bare `break;`, so clicking either did
// literally nothing:
//   PROPERTIES  - the entry the user reported. There was no desktop properties
//                 dialog anywhere in the tree to open, wired or unwired: no
//                 handler, no state id, no renderer. It was a label, not a
//                 feature that lost its wiring. Removed.
//   PASTE       - dead for a structural reason, so it is removed rather than
//                 left as a promise. The kernel clipboard (SYS_CLIPBOARD_*) is
//                 TEXT ONLY, and Files' Cut/Copy/Paste is process-local state
//                 inside Files. There is no file clipboard for the desktop to
//                 paste FROM, and inventing one is a new IPC design, not a fix.
// The numbers stay retired so an old build's action id can never be silently
// reinterpreted as a new action.
#define CTX_ACTION_SETTINGS     5
#define CTX_ACTION_WALLPAPER    6
#define CTX_ACTION_AUTO_ARRANGE 8
#define CTX_ACTION_ALIGN_GRID   9
#define CTX_ACTION_NEW_STICKY   10

// Start-menu item context actions (contextmenu_open_for_menuitem mode). Kept
// in a disjoint number range from the desktop CTX_ACTION_* set above so one
// action switch can safely tell them apart if ever needed; in practice each
// mode has its own small dispatch (see contextmenu_handle_mouse).
#define CTX_ACTION_MI_PIN         101   // Pin to Favorites / Unpin (label flips)
#define CTX_ACTION_MI_ADD_DESKTOP 102
#define CTX_ACTION_MI_PROPERTIES  103

// #44 dock item context actions (contextmenu_open_for_dock() mode). Own
// number range, same discipline as the MI_* block above.
#define CTX_ACTION_DOCK_SHOW         201
#define CTX_ACTION_DOCK_MINIMIZE     202
#define CTX_ACTION_DOCK_MAXIMIZE     203   // label flips Maximize/Restore
#define CTX_ACTION_DOCK_CLOSE        204
#define CTX_ACTION_DOCK_FORCE_QUIT   205
#define CTX_ACTION_DOCK_PIN          206   // Pin to Dock (RUNNING, identity resolved)
#define CTX_ACTION_DOCK_UNPIN        207   // Unpin from Favorites (PINNED/MERGED)
#define CTX_ACTION_DOCK_CHANGE_ICON  208
#define CTX_ACTION_DOCK_SPEED        209   // #778: DOS guest windows only

// #250 removable-volume icon actions (contextmenu_open_for_volume() mode).
// Own disjoint range, same discipline as the MI_* and DOCK_* blocks above.
#define CTX_ACTION_VOL_OPEN          301
#define CTX_ACTION_VOL_EJECT         302

// ============================================================================
// Static state
// ============================================================================

static ctx_menu_item_t g_ctx_items[CTX_MENU_MAX_ITEMS];
static int             g_ctx_item_count;
static int             g_ctx_hover;

// Which surface opened the menu, and (for MENUITEM mode) which Start-menu item
// index the actions below apply to. Desktop mode ignores g_ctx_target_item.
static int g_ctx_mode;          // CTX_MODE_DESKTOP (default), MENUITEM, or DOCK
static int g_ctx_target_item;   // index into startmenu.c's g_menu_items, MENUITEM mode only

// #44 DOCK mode target. Captured at open time (taskbar.c resolves all of
// this BEFORE calling contextmenu_open_for_dock() - see its own comment for
// why win_id/app_id/exec_path can each independently be "unknown").
static int       g_ctx_dock_win_id;               // -1 = no window (pinned, not running)
static char      g_ctx_dock_app_id[32];            // "" = no resolvable pid
static char      g_ctx_dock_exec_path[128];        // "" = no resolvable identity
static icon_id_t g_ctx_dock_icon_id;                // meaningful iff exec_path[0]
// #778: set at contextmenu_open_for_dock() time by the SAME detector
// taskbar.c's simpler tbmenu popup uses (dosspeed_window_is_dos()), so there
// is one place that decides "is this a DOS guest window", not two.
static bool g_ctx_dock_is_dos;
static char g_ctx_dock_game[40];

// #250 VOLUME mode target: which desktop icon the menu was opened on.
// The icon index and not the kernel volume index, because desktop.c owns the
// mapping between the two and matches it by mount point; capturing the kernel
// index here would be a second, independently-staleable copy of it.
static int g_ctx_vol_icon = -1;

// ============================================================================
// Internal helpers
// ============================================================================

// Compute the total pixel height of the menu based on item types.
// Each normal item occupies CTX_MENU_ITEM_H pixels; each separator
// occupies CTX_MENU_SEP_H pixels. An additional 8 pixels of top and
// bottom padding is added to the overall height.
static int32_t menu_height(void) {
    int32_t h = 8; // top + bottom padding
    for (int i = 0; i < g_ctx_item_count; i++) {
        h += g_ctx_items[i].is_separator ? CTX_MENU_SEP_H : CTX_MENU_ITEM_H;
    }
    return h;
}

// Return the Y offset (relative to the menu top, including the 4-pixel top
// padding) of the top edge of item at index idx.
static int32_t item_offset_y(int idx) {
    int32_t y = 4; // top padding
    for (int i = 0; i < idx; i++) {
        y += g_ctx_items[i].is_separator ? CTX_MENU_SEP_H : CTX_MENU_ITEM_H;
    }
    return y;
}

// Map a Y coordinate relative to the menu top to an item index.
// Returns -1 if the position falls on a separator or outside all items.
static int item_at_rel_y(int32_t rel_y) {
    int32_t off = 4; // top padding
    for (int i = 0; i < g_ctx_item_count; i++) {
        if (g_ctx_items[i].is_separator) {
            off += CTX_MENU_SEP_H;
            continue;
        }
        if (rel_y >= off && rel_y < off + CTX_MENU_ITEM_H) {
            return i;
        }
        off += CTX_MENU_ITEM_H;
    }
    return -1;
}

// ============================================================================
// contextmenu_init
// ============================================================================

void contextmenu_init(void) {
    g_ctx_mode       = CTX_MODE_DESKTOP;
    g_ctx_item_count = 0;
    g_ctx_hover      = -1;

    // Item 0: New Folder (#745: was a dead stub until the desktop gained a real
    // backing directory at <home>/DESKTOP; now creates a folder there.)
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        strncpy(it->label, "New Folder", sizeof(it->label) - 1);
        it->label[sizeof(it->label) - 1] = '\0';
        it->is_separator = false;
        it->action_id    = CTX_ACTION_NEW_FOLDER;
    }

    // Item 1: New File (#745: same, creates NEWFILE.TXT in <home>/DESKTOP.)
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        strncpy(it->label, "New File", sizeof(it->label) - 1);
        it->label[sizeof(it->label) - 1] = '\0';
        it->is_separator = false;
        it->action_id    = CTX_ACTION_NEW_FILE;
    }

    // Item 2: Refresh
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        strncpy(it->label, "Refresh", sizeof(it->label) - 1);
        it->label[sizeof(it->label) - 1] = '\0';
        it->is_separator = false;
        it->action_id    = CTX_ACTION_REFRESH;
    }

    // Item: New Sticky Note (#270) - creates a colored note at the click point.
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        strncpy(it->label, "New Sticky Note", sizeof(it->label) - 1);
        it->label[sizeof(it->label) - 1] = '\0';
        it->is_separator = false;
        it->action_id    = CTX_ACTION_NEW_STICKY;
    }

    // Item: Auto Arrange (re-flow icons into the default horizontal-top grid)
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        strncpy(it->label, "Auto Arrange", sizeof(it->label) - 1);
        it->label[sizeof(it->label) - 1] = '\0';
        it->is_separator = false;
        it->action_id    = CTX_ACTION_AUTO_ARRANGE;
    }

    // Item: Align to Grid (snap current icon positions to the grid)
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        strncpy(it->label, "Align to Grid", sizeof(it->label) - 1);
        it->label[sizeof(it->label) - 1] = '\0';
        it->is_separator = false;
        it->action_id    = CTX_ACTION_ALIGN_GRID;
    }

    // Item 4: Separator
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        it->label[0]     = '\0';
        it->is_separator = true;
        it->action_id    = 0;
    }

    // Item 5: Display Settings
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        strncpy(it->label, "Display Settings", sizeof(it->label) - 1);
        it->label[sizeof(it->label) - 1] = '\0';
        it->is_separator = false;
        it->action_id    = CTX_ACTION_SETTINGS;
    }

    // Item 6: Change Background
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        strncpy(it->label, "Change Background", sizeof(it->label) - 1);
        it->label[sizeof(it->label) - 1] = '\0';
        it->is_separator = false;
        it->action_id    = CTX_ACTION_WALLPAPER;
    }
    // (#745) The trailing separator that used to sit above Properties goes with
    // it. A separator whose only purpose was to divide off a removed item is
    // dead chrome, and leaving it would end the menu on a rule.
}

// ============================================================================
// contextmenu_render
// ============================================================================

void contextmenu_render(void) {
    if (!g_context_menu_open) {
        return;
    }

    int32_t mh = menu_height();
    int32_t mw = CTX_MENU_WIDTH;

    // Clamp position so the menu stays within the screen bounds.
    int32_t mx = g_context_menu_x;
    int32_t my = g_context_menu_y;

    // (local 81) Shared clamp. This block used to be duplicated VERBATIM in
    // contextmenu_handle_mouse(), two copies that had to be kept identical by
    // hand or a click would land on the wrong row; both now call the one
    // helper, so they cannot diverge.
    popup_clamp_to_work_area(mw, mh, &mx, &my);

    // Background fill.
    draw_fill_rect(mx, my, mw, mh, CLR_CTX_BG);

    // Border outline drawn on top of background.
    draw_rect_outline(mx, my, mw, mh, CLR_CTX_BORDER);

    // Render each item.
    for (int i = 0; i < g_ctx_item_count; i++) {
        int32_t item_y = my + item_offset_y(i);

        if (g_ctx_items[i].is_separator) {
            // Draw a horizontal rule at the vertical midpoint of the separator row.
            int32_t line_y = item_y + CTX_MENU_SEP_H / 2;
            draw_hline(mx + 4, line_y, mw - 8, CLR_CTX_BORDER);
            continue;
        }

        // Hover highlight behind the item text.
        bool hovered = (i == g_ctx_hover);
        if (hovered) {
            draw_fill_rect(mx + 1, item_y, mw - 2, CTX_MENU_ITEM_H, CLR_CTX_HOVER);
        }

        // Item label in the antialiased TTF font (same style as desktop icon
        // labels), vertically centered within the item row.
        //
        // (#745) This used to be CLR_CHROME_TEXT unconditionally, i.e.
        // readable_ink(CLR_TASKBAR_BG) - ink picked for the TASKBAR's colour,
        // not this menu's. CLR_CTX_BG is TC(THEME_COLOR_MENU_BG), a different
        // theme key than taskbar_bg, and on any theme where the two land on
        // opposite luminance classes (ocean/forest/sunset: dark taskbar,
        // near-white menu_bg) the resting item text was picked for the wrong
        // surface. Only a hovered row happened to read, because CLR_CTX_HOVER
        // (menu_item_hover) is usually still close enough to the taskbar's
        // own class to look right by coincidence. Measure against what THIS
        // row actually paints, same fix as the start menu above.
        int ttf_sz = 15;
        int32_t text_y = item_y + (CTX_MENU_ITEM_H - ttf_sz) / 2;
        uint32_t ink = readable_ink(hovered ? CLR_CTX_HOVER : CLR_CTX_BG);
        draw_text_ttf(mx + 10, text_y, g_ctx_items[i].label, ttf_sz, ink);
    }
}

// ============================================================================
// contextmenu_handle_mouse
// ============================================================================

bool contextmenu_handle_mouse(int32_t x, int32_t y, bool clicked) {
    if (!g_context_menu_open) {
        return false;
    }

    int32_t mh = menu_height();
    int32_t mw = CTX_MENU_WIDTH;

    // Apply the same clamping that render uses so hit-testing is consistent.
    int32_t mx = g_context_menu_x;
    int32_t my = g_context_menu_y;

    popup_clamp_to_work_area(mw, mh, &mx, &my);

    bool inside = (x >= mx && x < mx + mw && y >= my && y < my + mh);

    if (!inside) {
        if (clicked) {
            // Click outside the menu dismisses it.
            contextmenu_close();
            g_needs_redraw = true;
        }
        // A hover outside does not consume the event so other surfaces can
        // still receive it.
        return clicked;
    }

    // Mouse is inside the menu bounds.
    int32_t rel_y   = y - my;
    int     hovered = item_at_rel_y(rel_y);

    if (hovered != g_ctx_hover) {
        g_ctx_hover    = hovered;
        g_needs_redraw = true;
    }

    if (clicked && hovered >= 0) {
        int action = g_ctx_items[hovered].action_id;

        // Execute action.
        switch (action) {
            case CTX_ACTION_WALLPAPER:
                // #74: open Settings on the Appearance tab (wallpaper/themes).
                // #129: settings_open_panel() focuses an already-open Settings
                // window instead of spawning a duplicate (see compositor.h).
                settings_open_panel(SETTINGS_PANEL_APPEARANCE);
                break;

            case CTX_ACTION_SETTINGS:
                // #74: "Display Settings" opens Settings on the Display tab.
                settings_open_panel(SETTINGS_PANEL_DISPLAY);
                break;

            case CTX_ACTION_REFRESH:
                // (#745) Refresh now means something: re-read <home>/DESKTOP
                // immediately instead of waiting out the 2s poll, then repaint.
                desktop_rescan_home(1);
                g_needs_redraw = true;
                break;

            case CTX_ACTION_NEW_FOLDER:
                desktop_new_folder();
                break;

            case CTX_ACTION_NEW_FILE:
                desktop_new_file();
                break;

            case CTX_ACTION_AUTO_ARRANGE:
                // Re-flow all desktop icons into the default horizontal-top grid.
                desktop_auto_arrange();
                break;

            case CTX_ACTION_ALIGN_GRID:
                // Snap current icon positions to the nearest grid cells.
                desktop_align_to_grid();
                break;

            case CTX_ACTION_NEW_STICKY:
                // #270: drop a new sticky note where the menu was opened.
                sticky_new_at(g_context_menu_x, g_context_menu_y);
                g_needs_redraw = true;
                break;

            // Start-menu item actions (CTX_MODE_MENUITEM; g_ctx_target_item
            // is the index set by contextmenu_open_for_menuitem()).
            case CTX_ACTION_MI_PIN:
                startmenu_item_toggle_favorite(g_ctx_target_item);
                break;
            case CTX_ACTION_MI_ADD_DESKTOP:
                startmenu_item_add_to_desktop(g_ctx_target_item);
                break;
            case CTX_ACTION_MI_PROPERTIES:
                startmenu_item_open_properties(g_ctx_target_item);
                break;

            // #250 removable-volume actions. g_ctx_vol_icon was captured at
            // open time; desktop.c re-resolves the volume from the icon's
            // mount point on each call, so a drive pulled while the menu was
            // open simply resolves to nothing and both actions no-op.
            case CTX_ACTION_VOL_OPEN:
                desktop_icon_open_volume(g_ctx_vol_icon);
                break;
            case CTX_ACTION_VOL_EJECT:
                desktop_icon_eject(g_ctx_vol_icon);
                break;

            // #44 dock item actions. win_id/app_id/exec_path were validated
            // at contextmenu_open_for_dock() time (that is what decided
            // whether these menu rows exist at all), so no re-check here.
            case CTX_ACTION_DOCK_SHOW:
                wm_focus(g_ctx_dock_win_id);
                break;
            case CTX_ACTION_DOCK_MINIMIZE:
                wm_minimize(g_ctx_dock_win_id);
                break;
            case CTX_ACTION_DOCK_MAXIMIZE:
                // SYS_WM_MAXIMIZE_WINDOW operates on the FOCUSED window
                // (wm_toggle_maximize_focused(), a TOGGLE - see the
                // wm_window_info_t.maximized comment in kernel/gui/window.h
                // for why the label above is picked from the real kernel
                // state rather than assumed). Focus first so the toggle
                // lands on the window this menu was opened for, even if it
                // was not the focused window at click time.
                wm_focus(g_ctx_dock_win_id);
                sys_wm_maximize_focused();
                break;
            case CTX_ACTION_DOCK_CLOSE:
                taskbar_close_window(g_ctx_dock_win_id);
                break;
            case CTX_ACTION_DOCK_FORCE_QUIT:
                taskbar_force_quit_app_id(g_ctx_dock_app_id);
                break;
            case CTX_ACTION_DOCK_PIN:
            case CTX_ACTION_DOCK_UNPIN:
                startmenu_toggle_favorite_path(g_ctx_dock_exec_path);
                break;
            case CTX_ACTION_DOCK_CHANGE_ICON:
                iconpicker_open(g_ctx_dock_exec_path, g_ctx_dock_icon_id);
                break;
            case CTX_ACTION_DOCK_SPEED:
                // #778: g_ctx_dock_game was captured at open time above.
                dosspeed_open(g_ctx_dock_win_id, g_ctx_dock_game);
                break;

            default:
                break;
        }

        contextmenu_close();
        g_needs_redraw = true;
    }

    // Consume the event: no surface beneath the menu should receive it.
    return true;
}

// ============================================================================
// contextmenu_open
// ============================================================================

void contextmenu_open(int32_t x, int32_t y) {
    g_ctx_mode          = CTX_MODE_DESKTOP;
    g_context_menu_x    = x;
    g_context_menu_y    = y;
    g_context_menu_open = true;
    g_ctx_hover         = -1;
    g_needs_redraw      = true;
}

// ============================================================================
// contextmenu_open_for_menuitem (#: Start-menu right-click context menu)
// ============================================================================
// Builds a fresh 3-action item list (Pin/Unpin flips its label based on the
// item's CURRENT favorite state, so it must be rebuilt on every open rather
// than reusing a fixed array like the desktop's contextmenu_init() does) and
// opens the SAME render/hit-test primitive the desktop context menu uses.
void contextmenu_open_for_menuitem(int32_t x, int32_t y, int menu_item_idx) {
    g_ctx_mode        = CTX_MODE_MENUITEM;
    g_ctx_target_item = menu_item_idx;
    g_ctx_item_count  = 0;

    bool fav = startmenu_item_is_favorite(menu_item_idx);
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        strncpy(it->label, fav ? "Unpin from Favorites" : "Pin to Favorites", sizeof(it->label) - 1);
        it->label[sizeof(it->label) - 1] = '\0';
        it->is_separator = false;
        it->action_id    = CTX_ACTION_MI_PIN;
    }
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        strncpy(it->label, "Add to Desktop", sizeof(it->label) - 1);
        it->label[sizeof(it->label) - 1] = '\0';
        it->is_separator = false;
        it->action_id    = CTX_ACTION_MI_ADD_DESKTOP;
    }
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        it->label[0]     = '\0';
        it->is_separator = true;
        it->action_id    = 0;
    }
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        strncpy(it->label, "Properties", sizeof(it->label) - 1);
        it->label[sizeof(it->label) - 1] = '\0';
        it->is_separator = false;
        it->action_id    = CTX_ACTION_MI_PROPERTIES;
    }

    g_context_menu_x    = x;
    g_context_menu_y    = y;
    g_context_menu_open = true;
    g_ctx_hover         = -1;
    g_needs_redraw      = true;
}

// ============================================================================
// contextmenu_open_for_dock (#44: dock item right-click)
// ============================================================================
// Small local helpers so the item list below reads as a list of decisions,
// not eight copies of the same six-line struct-fill block.
static void ctx_push(const char *label) {
    if (g_ctx_item_count >= CTX_MENU_MAX_ITEMS) return;
    ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
    strncpy(it->label, label, sizeof(it->label) - 1);
    it->label[sizeof(it->label) - 1] = '\0';
    it->is_separator = false;
}
static void ctx_push_action(const char *label, int action_id) {
    ctx_push(label);
    g_ctx_items[g_ctx_item_count - 1].action_id = action_id;
}
static void ctx_push_sep(void) {
    if (g_ctx_item_count >= CTX_MENU_MAX_ITEMS) return;
    ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
    it->label[0]     = '\0';
    it->is_separator = true;
    it->action_id    = 0;
}

// Builds a menu whose content depends on what the caller actually resolved
// for this dock slot - the THREE item kinds taskbar.c already models
// (PINNED/MERGED/RUNNING) fall naturally out of which arguments are valid,
// rather than needing their own switch here:
//   PINNED (favorite, not running): win_id < 0, exec_path set.
//     -> Change Icon, Unpin from Favorites. No window actions: there is no
//        window.
//   MERGED (favorite AND running): win_id >= 0, exec_path set, is_favorite.
//     -> window actions + Change Icon + Unpin from Favorites.
//   RUNNING (running, not pinned), identity resolved: win_id >= 0, exec_path
//   set (via #41 app_id reverse-lookup), not is_favorite.
//     -> window actions + Pin to Dock + Change Icon.
//   RUNNING, identity NOT resolved (app_id empty, or app_id resolved to no
//   g_menu_items[] entry - e.g. a Win16/DOS shared host name, or an app not
//   in the Start Menu at all): win_id >= 0, exec_path empty.
//     -> window actions only. No Pin, no Change Icon: there is nothing to
//        pin or override an icon FOR. A menu entry with no real target is
//        worse than no entry (see this task's own brief) - omit, do not
//        wire it to a guess.
// Force Quit is gated independently on app_id alone (needs a name to search
// SYS_PROC_LIST for a pid - see taskbar_force_quit_app_id()), since a window
// can have a resolvable app_id even when the FULL start-menu identity
// (exec_path) does not resolve.
// ============================================================================
// contextmenu_open_for_volume (#250: removable-volume desktop icon right-click)
// ============================================================================
// Eject has to live somewhere the user can reach it, and the desktop
// right-click was reaching the BACKGROUND menu regardless of what was under
// the cursor, so there was nowhere to put it. This is the same
// rebuild-per-target pattern as the two menus above, not a new mechanism.
//
// Open is omitted, not disabled, when the volume's filesystem cannot serve
// reads: an enabled menu row that does nothing is exactly what #745 retired
// two of.
void contextmenu_open_for_volume(int32_t x, int32_t y, int icon_idx, int readable) {
    g_ctx_mode     = CTX_MODE_VOLUME;
    g_ctx_vol_icon = icon_idx;

    g_ctx_item_count = 0;
    if (readable) {
        ctx_push_action("Open", CTX_ACTION_VOL_OPEN);
        ctx_push_sep();
    }
    ctx_push_action("Eject", CTX_ACTION_VOL_EJECT);
    if (g_ctx_item_count == 0) return;

    g_context_menu_x    = x;
    g_context_menu_y    = y;
    g_context_menu_open = true;
    g_ctx_hover         = -1;
    g_needs_redraw      = true;
}

void contextmenu_open_for_dock(int32_t x, int32_t y, int win_id, bool maximized,
                               const char *app_id, const char *exec_path,
                               icon_id_t icon_id, bool is_favorite) {
    g_ctx_mode        = CTX_MODE_DOCK;
    g_ctx_dock_win_id = win_id;
    g_ctx_dock_app_id[0] = '\0';
    if (app_id && app_id[0]) {
        strncpy(g_ctx_dock_app_id, app_id, sizeof(g_ctx_dock_app_id) - 1);
        g_ctx_dock_app_id[sizeof(g_ctx_dock_app_id) - 1] = '\0';
    }
    g_ctx_dock_exec_path[0] = '\0';
    if (exec_path && exec_path[0]) {
        strncpy(g_ctx_dock_exec_path, exec_path, sizeof(g_ctx_dock_exec_path) - 1);
        g_ctx_dock_exec_path[sizeof(g_ctx_dock_exec_path) - 1] = '\0';
    }
    g_ctx_dock_icon_id = icon_id;
    // #778: resolved once here, at open time - never re-derived per frame.
    // Covers exactly the "RUNNING, identity NOT resolved" case this file's
    // own comment above describes for a DOS guest (app_id is the shared
    // kernel-owned host name, exec_path is empty), which is why this cannot
    // reuse exec_path/app_id and needs its own detector.
    g_ctx_dock_is_dos = win_id >= 0 &&
        dosspeed_window_is_dos(win_id, g_ctx_dock_game, (int)sizeof(g_ctx_dock_game)) != 0;

    g_ctx_item_count = 0;
    if (win_id >= 0) {
        ctx_push_action("Show Window", CTX_ACTION_DOCK_SHOW);
        ctx_push_action("Minimize",    CTX_ACTION_DOCK_MINIMIZE);
        ctx_push_action(maximized ? "Restore" : "Maximize", CTX_ACTION_DOCK_MAXIMIZE);
        if (g_ctx_dock_is_dos) ctx_push_action("Speed...", CTX_ACTION_DOCK_SPEED);
        ctx_push_sep();
        ctx_push_action("Close", CTX_ACTION_DOCK_CLOSE);
        if (g_ctx_dock_app_id[0]) ctx_push_action("Force Quit", CTX_ACTION_DOCK_FORCE_QUIT);
    }
    if (g_ctx_dock_exec_path[0]) {
        if (win_id >= 0) ctx_push_sep();
        if (is_favorite) ctx_push_action("Unpin from Favorites", CTX_ACTION_DOCK_UNPIN);
        else              ctx_push_action("Pin to Dock",          CTX_ACTION_DOCK_PIN);
        ctx_push_action("Change Icon...", CTX_ACTION_DOCK_CHANGE_ICON);
    }
    if (g_ctx_item_count == 0) return;   // nothing resolvable: do not open an empty menu

    g_context_menu_x    = x;
    g_context_menu_y    = y;
    g_context_menu_open = true;
    g_ctx_hover          = -1;
    g_needs_redraw       = true;
}

// ============================================================================
// contextmenu_close
// ============================================================================

void contextmenu_close(void) {
    g_context_menu_open = false;
    g_ctx_hover         = -1;
    g_needs_redraw      = true;
}
