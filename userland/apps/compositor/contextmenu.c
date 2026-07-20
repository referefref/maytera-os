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
#define CTX_ACTION_PASTE        4
#define CTX_ACTION_SETTINGS     5
#define CTX_ACTION_WALLPAPER    6
#define CTX_ACTION_PROPERTIES   7
#define CTX_ACTION_AUTO_ARRANGE 8
#define CTX_ACTION_ALIGN_GRID   9
#define CTX_ACTION_NEW_STICKY   10

// ============================================================================
// Static state
// ============================================================================

static ctx_menu_item_t g_ctx_items[CTX_MENU_MAX_ITEMS];
static int             g_ctx_item_count;
static int             g_ctx_hover;

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
    g_ctx_item_count = 0;
    g_ctx_hover      = -1;

    // Item 0: New Folder
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        strncpy(it->label, "New Folder", sizeof(it->label) - 1);
        it->label[sizeof(it->label) - 1] = '\0';
        it->is_separator = false;
        it->action_id    = CTX_ACTION_NEW_FOLDER;
    }

    // Item 1: New File
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

    // Item 3: Paste
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        strncpy(it->label, "Paste", sizeof(it->label) - 1);
        it->label[sizeof(it->label) - 1] = '\0';
        it->is_separator = false;
        it->action_id    = CTX_ACTION_PASTE;
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

    // Item 7: Separator
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        it->label[0]     = '\0';
        it->is_separator = true;
        it->action_id    = 0;
    }

    // Item 8: Properties
    {
        ctx_menu_item_t *it = &g_ctx_items[g_ctx_item_count++];
        strncpy(it->label, "Properties", sizeof(it->label) - 1);
        it->label[sizeof(it->label) - 1] = '\0';
        it->is_separator = false;
        it->action_id    = CTX_ACTION_PROPERTIES;
    }
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

    if (mx + mw > g_fb_width) {
        mx = g_fb_width - mw;
    }
    if (mx < 0) {
        mx = 0;
    }
    if (my + mh > g_fb_height) {
        my = g_fb_height - mh;
    }
    if (my < 0) {
        my = 0;
    }

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
        if (i == g_ctx_hover) {
            draw_fill_rect(mx + 1, item_y, mw - 2, CTX_MENU_ITEM_H, CLR_CTX_HOVER);
        }

        // Item label in the antialiased TTF font (same style as desktop icon
        // labels), vertically centered within the item row.
        int ttf_sz = 15;
        int32_t text_y = item_y + (CTX_MENU_ITEM_H - ttf_sz) / 2;
        draw_text_ttf(mx + 10, text_y, g_ctx_items[i].label, ttf_sz, CLR_CHROME_TEXT);
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

    if (mx + mw > g_fb_width)  mx = g_fb_width  - mw;
    if (mx < 0)                 mx = 0;
    if (my + mh > g_fb_height)  my = g_fb_height - mh;
    if (my < 0)                 my = 0;

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
                set_settings_tab(0);   // PANEL_APPEARANCE
                sys_spawn("/APPS/settings");
                break;

            case CTX_ACTION_SETTINGS:
                // #74: "Display Settings" opens Settings on the Display tab.
                set_settings_tab(1);   // PANEL_DISPLAY
                sys_spawn("/APPS/settings");
                break;

            case CTX_ACTION_REFRESH:
                // Force a full redraw.
                g_needs_redraw = true;
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

            // These are stubs for now.
            case CTX_ACTION_NEW_FOLDER:
            case CTX_ACTION_NEW_FILE:
            case CTX_ACTION_PASTE:
            case CTX_ACTION_PROPERTIES:
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
    g_context_menu_x    = x;
    g_context_menu_y    = y;
    g_context_menu_open = true;
    g_ctx_hover         = -1;
    g_needs_redraw      = true;
}

// ============================================================================
// contextmenu_close
// ============================================================================

void contextmenu_close(void) {
    g_context_menu_open = false;
    g_ctx_hover         = -1;
    g_needs_redraw      = true;
}
