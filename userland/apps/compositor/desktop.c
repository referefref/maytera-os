// desktop.c - Desktop surface for MayteraOS userland compositor
// Renders desktop icons, handles mouse interaction, and draws version string.
// No dynamic allocation: all state lives in fixed-size static arrays.
//
// Icons carry an absolute (px,py) screen position. The DEFAULT layout is a
// horizontal row across the TOP of the screen, wrapping to additional rows.
// Icons can be dragged (single or whole multi-selection), multi-selected via a
// rubber-band rectangle on the empty desktop, auto-arranged, and snapped to a
// grid. Positions persist to UIPROFIL.YML (see profile.c).

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../../kernel/version.h"
#include "../../libc/stdio.h"

// ============================================================================
// Static state
// ============================================================================

static desktop_icon_t g_icons[DESKTOP_ICON_MAX];
static int            g_icon_count;
static int            g_selected_icon;    // index of last single-selected icon, or -1
static uint64_t       g_last_click_time;  // sys_clock() value at last click
static int            g_last_click_icon;  // icon index at last click, or -1

// Drag / rubber-band state machine.
// Modes: 0 = idle, 1 = dragging icon(s), 2 = rubber-band selection.
static int      g_drag_mode;
static int32_t  g_press_x, g_press_y;     // where the button went down
static int32_t  g_drag_off_x, g_drag_off_y; // cursor offset within the grabbed icon
static int      g_drag_anchor;            // primary icon being dragged, or -1
static bool     g_drag_moved;             // did the cursor move beyond the dead zone
static int32_t  g_band_x, g_band_y, g_band_w, g_band_h; // current rubber-band rect

#define DRAG_DEAD_ZONE 5   // pixels of slop before a press becomes a drag

// ============================================================================
// Layout helpers
// ============================================================================

// Full bounding box of an icon (image + label row).
static int32_t icon_box_w(void) { return DESKTOP_ICON_SIZE; }
static int32_t icon_box_h(void) { return DESKTOP_ICON_SIZE + 20; }

// Usable desktop bounds (between any top bar and the bottom bar) for clamping.
// #387: layout-aware so icons stay clear of the active dock/menu bar(s).
static int32_t desk_bottom(void) { return taskbar_get_y(); }
static int32_t desk_top(void)    { return DESKTOP_ICON_MARGIN_Y + taskbar_top_inset(); }

// Clamp an icon top-left so the whole box stays on the visible desktop.
static void clamp_icon(int32_t *x, int32_t *y) {
    int32_t maxx = g_fb_width  - icon_box_w();
    int32_t maxy = desk_bottom() - icon_box_h();
    if (maxx < 0) maxx = 0;
    if (maxy < 0) maxy = 0;
    if (*x < 0) *x = 0; else if (*x > maxx) *x = maxx;
    if (*y < 0) *y = 0; else if (*y > maxy) *y = maxy;
}

// Place the icon at logical grid cell (col,row) of the default TOP layout.
static void grid_cell_pos(int col, int row, int32_t *x, int32_t *y) {
    *x = DESKTOP_ICON_MARGIN_X + col * DESKTOP_ICON_SPACING_X;
    *y = desk_top() + row * DESKTOP_ICON_SPACING_Y;
}

// Number of columns that fit horizontally before wrapping to the next row.
static int grid_cols(void) {
    int avail = g_fb_width - DESKTOP_ICON_MARGIN_X * 2;
    int cols  = avail / DESKTOP_ICON_SPACING_X;
    if (cols < 1) cols = 1;
    return cols;
}

// Re-flow all visible icons into the default horizontal-top grid, left to right,
// wrapping to a second row when they run past the right edge.
static void layout_default_top(void) {
    int cols = grid_cols();
    int n = 0;
    for (int i = 0; i < g_icon_count; i++) {
        if (!g_icons[i].visible) continue;
        int col = n % cols;
        int row = n / cols;
        grid_cell_pos(col, row, &g_icons[i].px, &g_icons[i].py);
        clamp_icon(&g_icons[i].px, &g_icons[i].py);
        n++;
    }
}

// Snap one coordinate pair to the nearest default-grid cell.
static void snap_to_grid(int32_t *x, int32_t *y) {
    int col = (*x - DESKTOP_ICON_MARGIN_X + DESKTOP_ICON_SPACING_X / 2) / DESKTOP_ICON_SPACING_X;
    int row = (*y - desk_top() + DESKTOP_ICON_SPACING_Y / 2) / DESKTOP_ICON_SPACING_Y;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    grid_cell_pos(col, row, x, y);
    clamp_icon(x, y);
}

// Return the index of the topmost icon whose box contains (px,py), or -1.
static int find_icon_at(int32_t px, int32_t py) {
    for (int i = g_icon_count - 1; i >= 0; i--) {
        if (!g_icons[i].visible) continue;
        int32_t ix = g_icons[i].px;
        int32_t iy = g_icons[i].py;
        if (px >= ix && px < ix + icon_box_w() &&
            py >= iy && py < iy + icon_box_h()) {
            return i;
        }
    }
    return -1;
}

static void clear_selection(void) {
    for (int i = 0; i < g_icon_count; i++) g_icons[i].selected = false;
    g_selected_icon = -1;
}

static int selection_count(void) {
    int c = 0;
    for (int i = 0; i < g_icon_count; i++) if (g_icons[i].visible && g_icons[i].selected) c++;
    return c;
}

// Launch the app at exec_path using sys_spawn (no fork; forking from the
// compositor hangs the OS because it duplicates framebuffer mappings).
static void launch_app(const char *exec_path) {
    if (!exec_path || exec_path[0] == '\0') return;
    // #239: "@RECYCLE" opens Files directly in its integrated Recycle Bin view
    // (drop a one-shot sentinel the Files app consumes at startup).
    if (exec_path[0] == '@' && exec_path[1] == 'R') {
        int fd = sys_open("/RECYVIEW.FLG", 0x41);
        if (fd >= 0) { sys_write(fd, "1", 1); sys_close(fd); }
        sys_spawn("/APPS/FILES");
        return;
    }
    sys_spawn(exec_path);
}

// ============================================================================
// desktop_init
// ============================================================================

static void add_icon(const char *name, const char *path, icon_id_t id) {
    if (g_icon_count >= DESKTOP_ICON_MAX) return;
    desktop_icon_t *ic = &g_icons[g_icon_count++];
    strncpy(ic->name, name, DESKTOP_ICON_NAME_LEN - 1);
    ic->name[DESKTOP_ICON_NAME_LEN - 1] = '\0';
    strncpy(ic->exec_path, path, sizeof(ic->exec_path) - 1);
    ic->exec_path[sizeof(ic->exec_path) - 1] = '\0';
    ic->icon_id  = id;
    ic->px = ic->py = 0;
    ic->selected = false;
    ic->visible  = true;
}

void desktop_init(void) {
    g_icon_count      = 0;
    g_selected_icon   = -1;
    g_last_click_time = 0;
    g_last_click_icon = -1;
    g_drag_mode       = 0;
    g_drag_anchor     = -1;
    g_drag_moved      = false;
    g_band_w = g_band_h = 0;

    add_icon("Computer",    "/APPS/files",          ICON_COMPUTER);
    add_icon("Recycle Bin", "@RECYCLE",             ICON_TRASH);
    add_icon("Terminal",    "/APPS/terminal",       ICON_TERMINAL);
    add_icon("Settings",    "/APPS/settings",       ICON_COG);
    add_icon("Browser",     "/APPS/browser",        ICON_BROWSER);
    add_icon("DOOM",        "/GAMES/DOOM/DOOM.ELF", ICON_GAME);

    // Default layout: a horizontal row across the TOP of the screen. profile.c
    // overrides these positions afterwards if saved coordinates exist.
    layout_default_top();
}

// ============================================================================
// desktop_render
// ============================================================================

void desktop_render(void) {
    for (int i = 0; i < g_icon_count; i++) {
        if (!g_icons[i].visible) continue;

        int32_t sx = g_icons[i].px;
        int32_t sy = g_icons[i].py;

        // Selection highlight behind the icon image + label row.
        if (g_icons[i].selected) {
            draw_fill_rect(sx - 4, sy - 4,
                           icon_box_w() + 8, icon_box_h() + 4,
                           CLR_ICON_SEL_BG);
        }

        icon_draw_scaled(g_icons[i].icon_id, sx, sy,
                         DESKTOP_ICON_SIZE, CLR_TEXT_WHITE);

        int32_t label_y  = sy + DESKTOP_ICON_SIZE + 4;
        int32_t label_cx = sx + DESKTOP_ICON_SIZE / 2;

        int lsz = (g_font_px <= 12) ? 12 : (g_font_px <= 16) ? 14 : (g_font_px <= 20) ? 18 : 20;
        int32_t tw = text_width_ttf(g_icons[i].name, lsz);
        int32_t label_x = label_cx - tw / 2;
        const char *nm = g_icons[i].name;
        static const int ox[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
        static const int oy[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
        for (int o = 0; o < 8; o++)
            draw_text_ttf(label_x + ox[o], label_y + oy[o], nm, lsz, CLR_TEXT_SHADOW);
        draw_text_ttf(label_x, label_y, nm, lsz, CLR_TEXT_WHITE);
    }
}

// Draw the rubber-band selection rectangle.
void desktop_render_overlay(void) {
    if (g_drag_mode != 2 || (g_band_w == 0 && g_band_h == 0)) return;
    draw_fill_rect(g_band_x, g_band_y, g_band_w, g_band_h, CLR_ICON_SEL_BG);
    draw_rect_outline(g_band_x, g_band_y, g_band_w, g_band_h, CLR_TEXT_WHITE);
}

// ============================================================================
// desktop_render_version
// ============================================================================

void desktop_render_version(void) {
    // Show the LIVE kernel version (SYS_GET_VERSION) so the desktop never lags
    // the running kernel just because the compositor wasn't rebuilt. Falls back
    // to the compile-time string if the syscall returns nothing.
    static char ver[64]; char vbuf[48]; vbuf[0] = 0;
    get_version(vbuf, sizeof(vbuf));
    if (vbuf[0]) snprintf(ver, sizeof(ver), "v%s", vbuf);
    else snprintf(ver, sizeof(ver), "v%s Build %d", MAYTERA_VERSION_STRING, MAYTERA_BUILD_NUMBER);

    int32_t tw = text_width(ver);
    int32_t vx = g_fb_width  - tw - 10;
    int32_t vy = taskbar_get_y() - 20;

    draw_text_shadow(vx, vy, ver, CLR_VERSION_TEXT, CLR_TEXT_SHADOW);
}

// ============================================================================
// Auto-arrange / align (context-menu actions)
// ============================================================================

void desktop_auto_arrange(void) {
    layout_default_top();
    g_needs_redraw = true;
}

void desktop_align_to_grid(void) {
    for (int i = 0; i < g_icon_count; i++) {
        if (!g_icons[i].visible) continue;
        snap_to_grid(&g_icons[i].px, &g_icons[i].py);
    }
    g_needs_redraw = true;
}

// ============================================================================
// Persistence hooks (profile.c)
// ============================================================================

int desktop_icon_count(void) { return g_icon_count; }

void desktop_get_icon_pos(int idx, int32_t *x, int32_t *y) {
    if (idx < 0 || idx >= g_icon_count) { *x = 0; *y = 0; return; }
    *x = g_icons[idx].px;
    *y = g_icons[idx].py;
}

void desktop_set_icon_pos(int idx, int32_t x, int32_t y) {
    if (idx < 0 || idx >= g_icon_count) return;
    g_icons[idx].px = x;
    g_icons[idx].py = y;
    clamp_icon(&g_icons[idx].px, &g_icons[idx].py);
}

int desktop_positions_hash(void) {
    int h = 0;
    for (int i = 0; i < g_icon_count; i++) {
        h += (g_icons[i].px + 1) * (7 + i * 2) + (g_icons[i].py + 1) * (13 + i * 2);
    }
    return h;
}

// ============================================================================
// Drag + rubber-band state machine (driven per-frame from main.c)
// ============================================================================

bool desktop_is_dragging(void) { return g_drag_mode != 0; }

// Left button just pressed. Returns true when the icon layer should treat the
// event as consumed for this frame.
bool desktop_press(int32_t x, int32_t y) {
    g_press_x    = x;
    g_press_y    = y;
    g_drag_moved = false;

    int hit = find_icon_at(x, y);
    if (hit >= 0) {
        // Press on an icon: begin a (potential) drag.
        // If the icon is not part of the current selection, make it the sole
        // selection. If it is already selected, keep the whole selection so a
        // multi-drag moves everything together.
        if (!g_icons[hit].selected) {
            clear_selection();
            g_icons[hit].selected = true;
        }
        g_selected_icon = hit;
        g_drag_anchor   = hit;
        g_drag_off_x    = x - g_icons[hit].px;
        g_drag_off_y    = y - g_icons[hit].py;
        g_drag_mode     = 1;
        g_needs_redraw  = true;
        return true;
    }

    // Press on empty desktop: clear selection and start a rubber-band.
    clear_selection();
    g_last_click_icon = -1;
    g_drag_anchor     = -1;
    g_drag_mode       = 2;
    g_band_x = x; g_band_y = y; g_band_w = 0; g_band_h = 0;
    g_needs_redraw    = true;
    return true;
}

// Mouse moved while the left button is held.
void desktop_drag(int32_t x, int32_t y) {
    if (g_drag_mode == 0) return;

    int dx = x - g_press_x;
    int dy = y - g_press_y;
    if (!g_drag_moved && (dx*dx + dy*dy) >= DRAG_DEAD_ZONE * DRAG_DEAD_ZONE) {
        g_drag_moved = true;
    }

    if (g_drag_mode == 1) {
        if (!g_drag_moved) return;   // still within the dead zone: not a real drag yet
        if (g_drag_anchor < 0) return;

        int32_t nx = x - g_drag_off_x;
        int32_t ny = y - g_drag_off_y;
        int32_t want_dx = nx - g_icons[g_drag_anchor].px;
        int32_t want_dy = ny - g_icons[g_drag_anchor].py;

        if (selection_count() > 1) {
            // Move the whole selection by the same delta, clamping each.
            for (int i = 0; i < g_icon_count; i++) {
                if (!g_icons[i].visible || !g_icons[i].selected) continue;
                int32_t ix = g_icons[i].px + want_dx;
                int32_t iy = g_icons[i].py + want_dy;
                clamp_icon(&ix, &iy);
                g_icons[i].px = ix;
                g_icons[i].py = iy;
            }
        } else {
            clamp_icon(&nx, &ny);
            g_icons[g_drag_anchor].px = nx;
            g_icons[g_drag_anchor].py = ny;
        }
        g_needs_redraw = true;
        return;
    }

    if (g_drag_mode == 2) {
        // Update rubber-band rectangle (normalize to positive w/h).
        int32_t x0 = g_press_x, y0 = g_press_y;
        int32_t x1 = x,         y1 = y;
        if (x1 < x0) { int32_t t = x0; x0 = x1; x1 = t; }
        if (y1 < y0) { int32_t t = y0; y0 = y1; y1 = t; }
        g_band_x = x0; g_band_y = y0;
        g_band_w = x1 - x0; g_band_h = y1 - y0;

        // Live-select icons intersecting the band.
        for (int i = 0; i < g_icon_count; i++) {
            if (!g_icons[i].visible) { g_icons[i].selected = false; continue; }
            int32_t ax = g_icons[i].px, ay = g_icons[i].py;
            int32_t aw = icon_box_w(), ah = icon_box_h();
            bool isect = !(ax + aw <= g_band_x || ax >= g_band_x + g_band_w ||
                           ay + ah <= g_band_y || ay >= g_band_y + g_band_h);
            g_icons[i].selected = isect;
        }
        g_needs_redraw = true;
        return;
    }
}

// Left button released: finish the drag/rubber-band, or treat a no-move press as
// a click (select; double-click launches).
void desktop_release(int32_t x, int32_t y) {
    int mode = g_drag_mode;
    g_drag_mode = 0;

    if (mode == 2) {
        // Rubber-band already applied live during drag; just clear the rect.
        g_band_w = g_band_h = 0;
        g_needs_redraw = true;
        return;
    }

    if (mode == 1) {
        if (g_drag_moved) {
            // A real drag happened: positions are already updated. profile_tick
            // picks up the change via desktop_positions_hash and persists it.
            g_band_w = g_band_h = 0;
            g_needs_redraw    = true;
            g_last_click_icon = -1;   // a drop is never a double-click
            return;
        }

        // No movement: treat as a click on the anchor icon.
        int hit = g_drag_anchor;
        if (hit < 0) hit = find_icon_at(x, y);
        if (hit < 0) { g_needs_redraw = true; return; }

        uint64_t now = (uint64_t)sys_clock();
        bool should_launch = (hit == g_last_click_icon &&
                              (now - g_last_click_time) < 500ULL);

        // Single-click selects only this icon.
        clear_selection();
        g_icons[hit].selected = true;
        g_selected_icon  = hit;
        g_needs_redraw   = true;

        if (should_launch) {
            launch_app(g_icons[hit].exec_path);
            clear_selection();          /* deselect after opening */
            g_last_click_icon = -1;
            g_last_click_time = 0;
        } else {
            g_last_click_icon = hit;
            g_last_click_time = now;
        }
    }
}

// ============================================================================
// desktop_handle_mouse (legacy entry: right-click + explicit double-click)
// ============================================================================

void desktop_handle_mouse(int32_t x, int32_t y,
                          bool left_click, bool right_click, bool dbl_click) {
    // Right-click: open context menu at cursor position.
    if (right_click) {
        contextmenu_open(x, y);
        return;
    }

    // Explicit kernel double-click on an icon launches immediately. (The press/
    // drag/release path in main.c handles single clicks, selection, and drags.)
    if (dbl_click) {
        int hit = find_icon_at(x, y);
        if (hit >= 0) {
            clear_selection();
            g_icons[hit].selected = true;
            g_selected_icon = hit;
            launch_app(g_icons[hit].exec_path);
            g_last_click_icon = -1;
            g_last_click_time = 0;
            g_needs_redraw = true;
        }
        return;
    }

    // Plain left_click without the press/drag path (should not normally happen
    // since main.c routes presses through desktop_press). Fall back to a select.
    if (left_click) {
        int hit = find_icon_at(x, y);
        if (hit < 0) { clear_selection(); g_last_click_icon = -1; }
        else { clear_selection(); g_icons[hit].selected = true; g_selected_icon = hit; }
        g_needs_redraw = true;
    }
}
