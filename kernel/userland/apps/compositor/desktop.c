// desktop.c - Desktop surface for MayteraOS userland compositor
// Renders desktop icons, handles mouse interaction, and draws version string.
// No dynamic allocation: all state lives in fixed-size static arrays.

#include "compositor.h"
#include "../../libc/syscall.h"

// ============================================================================
// Static state
// ============================================================================

static desktop_icon_t g_icons[DESKTOP_ICON_MAX];
static int            g_icon_count;
static int            g_selected_icon;    // index of selected icon, or -1
static uint64_t       g_last_click_time;  // sys_clock() value at last click
static int            g_last_click_icon;  // icon index at last click, or -1

// ============================================================================
// Internal helpers
// ============================================================================

// Return the screen-space X coordinate for an icon at grid position gx.
static int32_t icon_screen_x(int32_t gx) {
    return DESKTOP_ICON_MARGIN_X + gx * DESKTOP_ICON_SPACING_X;
}

// Return the screen-space Y coordinate for an icon at grid position gy.
static int32_t icon_screen_y(int32_t gy) {
    return DESKTOP_ICON_MARGIN_Y + gy * DESKTOP_ICON_SPACING_Y;
}

// Return the index of the icon whose bounding box contains (px, py),
// or -1 if no icon is hit. The bounding box covers DESKTOP_ICON_SIZE
// wide and (DESKTOP_ICON_SIZE + 20) tall (icon plus label row).
static int find_icon_at(int32_t px, int32_t py) {
    for (int i = 0; i < g_icon_count; i++) {
        if (!g_icons[i].visible) {
            continue;
        }
        int32_t ix = icon_screen_x(g_icons[i].grid_x);
        int32_t iy = icon_screen_y(g_icons[i].grid_y);
        int32_t iw = DESKTOP_ICON_SIZE;
        int32_t ih = DESKTOP_ICON_SIZE + 20;

        if (px >= ix && px < ix + iw && py >= iy && py < iy + ih) {
            return i;
        }
    }
    return -1;
}

// Launch the app at exec_path using fork + exec.
// The child process replaces itself with the target binary.
// The parent returns immediately without waiting.
static void launch_app(const char *exec_path) {
    if (!exec_path || exec_path[0] == '\0') {
        return;
    }
    int pid = sys_fork();
    if (pid == 0) {
        // Child: replace with the target executable.
        sys_exec(exec_path);
        // If exec fails the child exits cleanly.
        sys_exit(1);
    }
    // Parent continues without blocking.
}

// ============================================================================
// desktop_init
// ============================================================================

void desktop_init(void) {
    g_icon_count     = 0;
    g_selected_icon  = -1;
    g_last_click_time = 0;
    g_last_click_icon = -1;

    // Helper macro to fill in one icon entry.
    // Written out explicitly to keep it readable without macros.

    // Icon 0: Computer (home folder / file browser)
    {
        desktop_icon_t *ic = &g_icons[g_icon_count++];
        strncpy(ic->name, "Computer", DESKTOP_ICON_NAME_LEN - 1);
        ic->name[DESKTOP_ICON_NAME_LEN - 1] = '\0';
        strncpy(ic->exec_path, "/APPS/FILES", sizeof(ic->exec_path) - 1);
        ic->exec_path[sizeof(ic->exec_path) - 1] = '\0';
        ic->icon_id = ICON_HOME;
        ic->grid_x  = 0;
        ic->grid_y  = 0;
        ic->selected = false;
        ic->visible  = true;
    }

    // Icon 1: Recycle Bin (no exec, decorative only)
    {
        desktop_icon_t *ic = &g_icons[g_icon_count++];
        strncpy(ic->name, "Recycle Bin", DESKTOP_ICON_NAME_LEN - 1);
        ic->name[DESKTOP_ICON_NAME_LEN - 1] = '\0';
        ic->exec_path[0] = '\0';
        ic->icon_id = ICON_TRASH;
        ic->grid_x  = 0;
        ic->grid_y  = 1;
        ic->selected = false;
        ic->visible  = true;
    }

    // Icon 2: Terminal
    {
        desktop_icon_t *ic = &g_icons[g_icon_count++];
        strncpy(ic->name, "Terminal", DESKTOP_ICON_NAME_LEN - 1);
        ic->name[DESKTOP_ICON_NAME_LEN - 1] = '\0';
        strncpy(ic->exec_path, "/APPS/TERMINAL", sizeof(ic->exec_path) - 1);
        ic->exec_path[sizeof(ic->exec_path) - 1] = '\0';
        ic->icon_id = ICON_TERMINAL;
        ic->grid_x  = 0;
        ic->grid_y  = 2;
        ic->selected = false;
        ic->visible  = true;
    }

    // Icon 3: Settings
    {
        desktop_icon_t *ic = &g_icons[g_icon_count++];
        strncpy(ic->name, "Settings", DESKTOP_ICON_NAME_LEN - 1);
        ic->name[DESKTOP_ICON_NAME_LEN - 1] = '\0';
        strncpy(ic->exec_path, "/APPS/SETTINGS", sizeof(ic->exec_path) - 1);
        ic->exec_path[sizeof(ic->exec_path) - 1] = '\0';
        ic->icon_id = ICON_COG;
        ic->grid_x  = 0;
        ic->grid_y  = 3;
        ic->selected = false;
        ic->visible  = true;
    }

    // Icon 4: DOOM
    {
        desktop_icon_t *ic = &g_icons[g_icon_count++];
        strncpy(ic->name, "DOOM", DESKTOP_ICON_NAME_LEN - 1);
        ic->name[DESKTOP_ICON_NAME_LEN - 1] = '\0';
        strncpy(ic->exec_path, "/APPS/DOOM", sizeof(ic->exec_path) - 1);
        ic->exec_path[sizeof(ic->exec_path) - 1] = '\0';
        ic->icon_id = ICON_GAME;
        ic->grid_x  = 0;
        ic->grid_y  = 4;
        ic->selected = false;
        ic->visible  = true;
    }
}

// ============================================================================
// desktop_render
// ============================================================================

void desktop_render(void) {
    for (int i = 0; i < g_icon_count; i++) {
        if (!g_icons[i].visible) {
            continue;
        }

        int32_t sx = icon_screen_x(g_icons[i].grid_x);
        int32_t sy = icon_screen_y(g_icons[i].grid_y);

        // Draw selection highlight behind the icon when selected.
        if (g_icons[i].selected) {
            // Expand the highlight by 4 px on each side.
            draw_fill_rect(sx - 4, sy - 4,
                           DESKTOP_ICON_SIZE + 8, DESKTOP_ICON_SIZE + 8,
                           CLR_ICON_SEL_BG);
        }

        // Draw the icon itself.
        icon_draw_scaled(g_icons[i].icon_id, sx, sy,
                         DESKTOP_ICON_SIZE, CLR_TEXT_WHITE);

        // Draw the label with a drop shadow, centered below the icon.
        // The label row starts just below the icon image.
        int32_t label_y  = sy + DESKTOP_ICON_SIZE + 4;
        int32_t label_cx = sx + DESKTOP_ICON_SIZE / 2;

        // Center horizontally: compute x from the center point.
        int32_t tw = text_width(g_icons[i].name);
        int32_t label_x = label_cx - tw / 2;

        draw_text_shadow(label_x, label_y,
                         g_icons[i].name,
                         CLR_TEXT_WHITE, CLR_TEXT_SHADOW);
    }
}

// ============================================================================
// desktop_render_version
// ============================================================================

void desktop_render_version(void) {
    const char *ver = "v1.9.0 Build 1";

    int32_t tw = text_width(ver);
    int32_t vx = g_fb_width  - tw - 10;
    int32_t vy = g_fb_height - TASKBAR_HEIGHT - 20;

    draw_text_shadow(vx, vy, ver, CLR_VERSION_TEXT, CLR_TEXT_SHADOW);
}

// ============================================================================
// desktop_handle_mouse
// ============================================================================

void desktop_handle_mouse(int32_t x, int32_t y,
                          bool left_click, bool right_click, bool dbl_click) {
    // Right-click: open context menu at cursor position and return.
    if (right_click) {
        contextmenu_open(x, y);
        return;
    }

    if (left_click || dbl_click) {
        int hit = find_icon_at(x, y);

        if (hit < 0) {
            // Click on empty desktop: deselect everything.
            for (int i = 0; i < g_icon_count; i++) {
                g_icons[i].selected = false;
            }
            g_selected_icon  = -1;
            g_last_click_icon = -1;
            g_needs_redraw    = true;
            return;
        }

        // Determine whether this is a launch event.
        // Launch if: explicit double-click flag is set, OR the user clicked
        // the same already-selected icon within 500 ms of the last click.
        uint64_t now = (uint64_t)sys_clock();
        bool should_launch = false;

        if (dbl_click) {
            should_launch = true;
        } else if (hit == g_last_click_icon &&
                   g_icons[hit].selected &&
                   (now - g_last_click_time) < 500ULL) {
            should_launch = true;
        }

        // Update selection state: select the hit icon, deselect others.
        for (int i = 0; i < g_icon_count; i++) {
            g_icons[i].selected = (i == hit);
        }
        g_selected_icon  = hit;
        g_last_click_time = now;
        g_last_click_icon = hit;
        g_needs_redraw    = true;

        if (should_launch) {
            launch_app(g_icons[hit].exec_path);
            // Reset double-click tracking after a launch so a quick
            // third click does not immediately launch again.
            g_last_click_icon = -1;
            g_last_click_time = 0;
        }
    }
}
