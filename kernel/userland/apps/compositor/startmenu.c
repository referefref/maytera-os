// startmenu.c - Accordion-style start menu for the MayteraOS userland compositor.
// Renders a vertically stacked list of expandable category headers. Each header
// can be clicked to reveal or hide its application items. A fixed power section
// at the bottom provides Restart and Shutdown buttons. No malloc; all state is
// stored in static arrays sized at compile time.

#include "compositor.h"
#include "../../libc/syscall.h"

// ============================================================================
// Static state
// ============================================================================

#define MAX_CATEGORIES 3

static menu_category_t g_categories[MAX_CATEGORIES];
static menu_item_t     g_menu_items[START_MENU_MAX_ITEMS];
static int             g_total_items;
static int             g_hover_item;   // index into g_menu_items, or -1
static int             g_hover_cat;    // index into g_categories, or -1

// Power button hover: 0 = none, 1 = Restart, 2 = Shutdown
static int g_hover_power;

// ============================================================================
// Internal helpers
// ============================================================================

// Append one item to g_menu_items and return its index. The item is associated
// with whichever category was most recently registered.
static void add_item(const char *name, icon_id_t icon, const char *path)
{
    if (g_total_items >= START_MENU_MAX_ITEMS)
        return;

    menu_item_t *it = &g_menu_items[g_total_items];
    strncpy(it->name,      name, sizeof(it->name) - 1);
    it->name[sizeof(it->name) - 1] = '\0';
    strncpy(it->exec_path, path, sizeof(it->exec_path) - 1);
    it->exec_path[sizeof(it->exec_path) - 1] = '\0';
    it->icon_id      = icon;
    it->is_separator = false;

    // Increment the item_count of the last registered category.
    int last_cat = -1;
    for (int i = 0; i < MAX_CATEGORIES; i++) {
        if (g_categories[i].label[0] != '\0')
            last_cat = i;
    }
    if (last_cat >= 0)
        g_categories[last_cat].item_count++;

    g_total_items++;
}

// Register a category. Must be called before add_item() calls for that group.
static void add_category(int index, const char *label, bool expanded)
{
    menu_category_t *cat = &g_categories[index];
    strncpy(cat->label, label, sizeof(cat->label) - 1);
    cat->label[sizeof(cat->label) - 1] = '\0';
    cat->expanded   = expanded;
    cat->item_start = g_total_items;
    cat->item_count = 0;
}

// Calculate the total pixel height of the menu given current expanded state.
// Layout: for each category, CAT_H header + (ITEM_H * visible_items if expanded),
// with a SEP_H gap drawn between categories, and POWER_H at the very bottom.
static int32_t calc_menu_height(void)
{
    int32_t h = 0;
    for (int c = 0; c < MAX_CATEGORIES; c++) {
        if (c > 0)
            h += START_MENU_SEP_H;
        h += START_MENU_CAT_H;
        if (g_categories[c].expanded)
            h += START_MENU_ITEM_H * g_categories[c].item_count;
    }
    h += START_MENU_POWER_H;
    return h;
}

// ============================================================================
// Public API
// ============================================================================

void startmenu_init(void)
{
    memset(g_categories, 0, sizeof(g_categories));
    memset(g_menu_items,  0, sizeof(g_menu_items));
    g_total_items = 0;
    g_hover_item  = -1;
    g_hover_cat   = -1;
    g_hover_power = 0;

    // Category 0: APPLICATIONS (expanded by default)
    add_category(0, "APPLICATIONS", true);
    add_item("Terminal",     ICON_TERMINAL,    "/APPS/TERMINAL");
    add_item("Files",        ICON_FOLDER,      "/APPS/FILES");
    add_item("Editor",       ICON_HIGHLIGHT,   "/APPS/EDITOR");
    add_item("Calculator",   ICON_CALCULATOR,  "/APPS/CALC");
    add_item("Paint",        ICON_PAINT,       "/APPS/PAINT");
    add_item("Image Viewer", ICON_IMAGE,       "/APPS/IMGVIEW");
    add_item("Music Player", ICON_MUSIC,       "/APPS/MUSICPLR");
    add_item("IRC Client",   ICON_TERMINAL,    "/APPS/IRC");

    // Category 1: GAMES (collapsed by default)
    add_category(1, "GAMES", false);
    add_item("DOOM",         ICON_GAME,        "/APPS/DOOM");
    add_item("Lemmings",     ICON_GAME,        "/APPS/LEMMINGS");
    add_item("Solitaire",    ICON_GAME,        "/APPS/SOLITAIRE");
    add_item("Pong",         ICON_GAME,        "/APPS/PONG");

    // Category 2: SYSTEM (collapsed by default)
    add_category(2, "SYSTEM", false);
    add_item("Settings",     ICON_COG,         "/APPS/SETTINGS");
    add_item("Task Manager", ICON_TASK_MANAGER, "/APPS/TASKMGR");
    add_item("System Log",   ICON_LOG_VIEWER,  "/APPS/SYSLOG");
}

void startmenu_render(void)
{
    if (!g_start_menu_open)
        return;

    int32_t w  = START_MENU_WIDTH;
    int32_t mh = calc_menu_height();
    int32_t mx = TASKBAR_PADDING;
    int32_t my = taskbar_get_y() - mh - 4;

    // Keep menu on screen if taskbar is at the top.
    if (my < 0)
        my = 0;

    // Shadow
    draw_fill_rect(mx + 3, my + 3, w, mh, CLR_MENU_SHADOW);

    // Background
    draw_fill_rect(mx, my, w, mh, CLR_MENU_BG);

    // Border
    draw_rect_outline(mx, my, w, mh, CLR_MENU_BORDER);

    int32_t cy = my;  // running y cursor

    for (int c = 0; c < MAX_CATEGORIES; c++) {
        menu_category_t *cat = &g_categories[c];

        // Separator line between categories (not before the first one).
        if (c > 0) {
            draw_hline(mx + 8, cy, w - 16, CLR_MENU_SEP);
            cy += START_MENU_SEP_H;
        }

        // Category header background.
        uint32_t cat_bg = cat->expanded ? CLR_MENU_CAT_BG : CLR_MENU_BG;
        if (g_hover_cat == c)
            cat_bg = CLR_MENU_ITEM_HOVER;
        draw_fill_rect(mx, cy, w, START_MENU_CAT_H, cat_bg);

        // Category label (left-padded).
        draw_text(mx + START_MENU_PADDING + 4,
                  cy + (START_MENU_CAT_H - FONT_CHAR_H) / 2,
                  cat->label, CLR_MENU_TEXT);

        // Expand/collapse indicator on the right side.
        const char *indicator = cat->expanded ? "v" : ">";
        draw_text(mx + w - START_MENU_PADDING - FONT_CHAR_W - 2,
                  cy + (START_MENU_CAT_H - FONT_CHAR_H) / 2,
                  indicator, CLR_MENU_TEXT);

        cy += START_MENU_CAT_H;

        // Items for this category, drawn only when expanded.
        if (cat->expanded) {
            for (int i = 0; i < cat->item_count; i++) {
                int item_idx = cat->item_start + i;
                menu_item_t *it = &g_menu_items[item_idx];

                // Hover highlight.
                uint32_t item_bg = CLR_MENU_BG;
                if (g_hover_item == item_idx)
                    item_bg = CLR_MENU_ITEM_HOVER;
                draw_fill_rect(mx, cy, w, START_MENU_ITEM_H, item_bg);

                // Icon (20x20, vertically centred in the row).
                int32_t icon_y = cy + (START_MENU_ITEM_H - 20) / 2;
                icon_draw_scaled(it->icon_id, mx + 8, icon_y, 20, CLR_TEXT_WHITE);

                // App name text.
                draw_text(mx + 34,
                          cy + (START_MENU_ITEM_H - FONT_CHAR_H) / 2,
                          it->name, CLR_MENU_TEXT);

                cy += START_MENU_ITEM_H;
            }
        }
    }

    // Power section separator.
    draw_hline(mx + 8, cy, w - 16, CLR_MENU_SEP);

    // Power section background.
    draw_fill_rect(mx, cy, w, START_MENU_POWER_H, CLR_MENU_BG);

    // Two side-by-side buttons inside the power section.
    int32_t btn_w   = (w - 24) / 2;
    int32_t btn_h   = START_MENU_POWER_H - 8;
    int32_t btn_y   = cy + 4;
    int32_t btn_x1  = mx + 8;
    int32_t btn_x2  = mx + 8 + btn_w + 8;

    // Restart button.
    uint32_t rst_bg = (g_hover_power == 1) ? CLR_MENU_ITEM_HOVER : CLR_MENU_ITEM_NORM;
    draw_fill_rect(btn_x1, btn_y, btn_w, btn_h, rst_bg);
    draw_rect_outline(btn_x1, btn_y, btn_w, btn_h, CLR_MENU_BORDER);
    icon_draw_scaled(ICON_REFRESH,
                     btn_x1 + 4,
                     btn_y + (btn_h - 14) / 2,
                     14, CLR_TEXT_WHITE);
    draw_text(btn_x1 + 22,
              btn_y + (btn_h - FONT_CHAR_H) / 2,
              "Restart", CLR_MENU_TEXT);

    // Shutdown button.
    uint32_t sdn_bg = (g_hover_power == 2) ? CLR_MENU_ITEM_HOVER : CLR_MENU_ITEM_NORM;
    draw_fill_rect(btn_x2, btn_y, btn_w, btn_h, sdn_bg);
    draw_rect_outline(btn_x2, btn_y, btn_w, btn_h, CLR_MENU_BORDER);
    icon_draw_scaled(ICON_POWER,
                     btn_x2 + 4,
                     btn_y + (btn_h - 14) / 2,
                     14, CLR_POWER_RED);
    draw_text(btn_x2 + 22,
              btn_y + (btn_h - FONT_CHAR_H) / 2,
              "Shutdown", CLR_MENU_TEXT);
}

bool startmenu_handle_mouse(int32_t x, int32_t y, bool clicked)
{
    if (!g_start_menu_open)
        return false;

    int32_t w  = START_MENU_WIDTH;
    int32_t mh = calc_menu_height();
    int32_t mx = TASKBAR_PADDING;
    int32_t my = taskbar_get_y() - mh - 4;
    if (my < 0)
        my = 0;

    // Outside menu bounds: do not consume.
    if (x < mx || x >= mx + w || y < my || y >= my + mh)
        return false;

    // Reset hover state; we re-derive it below.
    g_hover_item  = -1;
    g_hover_cat   = -1;
    g_hover_power = 0;

    int32_t cy = my;

    for (int c = 0; c < MAX_CATEGORIES; c++) {
        menu_category_t *cat = &g_categories[c];

        // Account for the separator gap before categories 1 and 2.
        if (c > 0)
            cy += START_MENU_SEP_H;

        // Check category header region.
        if (y >= cy && y < cy + START_MENU_CAT_H) {
            g_hover_cat = c;
            if (clicked) {
                cat->expanded = !cat->expanded;
                g_needs_redraw = true;
            }
            return true;
        }
        cy += START_MENU_CAT_H;

        // Check item rows when the category is expanded.
        if (cat->expanded) {
            for (int i = 0; i < cat->item_count; i++) {
                if (y >= cy && y < cy + START_MENU_ITEM_H) {
                    int item_idx = cat->item_start + i;
                    g_hover_item = item_idx;
                    if (clicked) {
                        // Fork and exec the selected application.
                        int pid = sys_fork();
                        if (pid == 0) {
                            // Child process: replace image with the app.
                            sys_exec(g_menu_items[item_idx].exec_path);
                            sys_exit(1);
                        }
                        // Parent: close the menu and request redraw.
                        g_start_menu_open = false;
                        g_hover_item      = -1;
                        g_hover_cat       = -1;
                        g_hover_power     = 0;
                        g_needs_redraw    = true;
                    }
                    return true;
                }
                cy += START_MENU_ITEM_H;
            }
        }
    }

    // Power section: starts at cy (after all categories and their separators).
    int32_t btn_w  = (w - 24) / 2;
    int32_t btn_h  = START_MENU_POWER_H - 8;
    int32_t btn_y  = cy + 4;
    int32_t btn_x1 = mx + 8;
    int32_t btn_x2 = mx + 8 + btn_w + 8;

    if (y >= btn_y && y < btn_y + btn_h) {
        if (x >= btn_x1 && x < btn_x1 + btn_w) {
            // Restart button.
            g_hover_power = 1;
            if (clicked) {
                g_start_menu_open = false;
                g_needs_redraw    = true;
                // Issue a soft reboot via sys_exit with a sentinel code that the
                // kernel shell interprets as a reboot request. If a dedicated
                // syscall is added later, replace this call accordingly.
                sys_exit(0xFE);
            }
            return true;
        }
        if (x >= btn_x2 && x < btn_x2 + btn_w) {
            // Shutdown button.
            g_hover_power = 2;
            if (clicked) {
                g_start_menu_open = false;
                g_needs_redraw    = true;
                // Issue a shutdown via sys_exit with a sentinel code that the
                // kernel interprets as a power-off request.
                sys_exit(0xFF);
            }
            return true;
        }
    }

    // Cursor is inside the menu bounds but over a gap or border: consume the
    // event so clicks do not pass through to the desktop underneath.
    return true;
}

void startmenu_toggle(void)
{
    g_start_menu_open = !g_start_menu_open;
    g_hover_item      = -1;
    g_hover_cat       = -1;
    g_hover_power     = 0;
    g_needs_redraw    = true;
}
