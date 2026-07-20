// settings_main.c - Main window and navigation for unified Settings application
// MayteraOS Unified Settings Framework
#include "settings_panel.h"
#include "settings_widgets.h"
#include "../window.h"
#include "../desktop.h"
#include "../themes.h"
#include "../../types.h"
#include "../../serial.h"
#include "../../mm/heap.h"
#include "../../string.h"
#include "../../video/framebuffer.h"
#include "../../video/font.h"
#include "../icons.h"

// ============================================================================
// Settings Application Constants
// ============================================================================

#define SETTINGS_WINDOW_WIDTH   700
#define SETTINGS_WINDOW_HEIGHT  500
#define SETTINGS_WINDOW_TITLE   "Settings"

// Navigation panel constants
#define NAV_SEARCH_HEIGHT       36
#define NAV_SECTION_PADDING     4
#define NAV_ITEM_INDENT         24

// Content panel colors
#define SETTINGS_NAV_BG THEME_WINDOW_BG  // Light gray nav background
#define SETTINGS_CONTENT_BG THEME_TEXTBOX_BG  // White content background
#define SETTINGS_NAV_SELECTED   0x00E0E0E0  // Selected nav item
#define SETTINGS_NAV_HOVER      0x00E8E8E8  // Hovered nav item
#define SETTINGS_SECTION_TEXT   0x00666666  // Section header text
#define SETTINGS_NAV_TEXT       0x00333333  // Normal nav item text
#define SETTINGS_NAV_ACTIVE THEME_LABEL_TEXT  // Active nav item text

// ============================================================================
// Panel Registry
// ============================================================================

static settings_panel_t g_panels[SETTINGS_MAX_PANELS];
static int g_panel_count = 0;
static int g_current_panel = -1;

// ============================================================================
// Settings Application State
// ============================================================================

typedef struct settings_app {
    window_t *window;
    bool running;
    int app_id;
    int dock_index;

    // Navigation state
    int nav_hover_index;        // Currently hovered nav item
    int32_t nav_scroll_y;       // Navigation scroll offset

    // Search state
    char search_text[64];
    int search_cursor;
    bool search_focused;

    // Content state
    int32_t content_scroll_y;   // Content area scroll offset

    // Cached layout
    int32_t nav_x, nav_y, nav_width, nav_height;
    int32_t content_x, content_y, content_width, content_height;
} settings_app_t;

static settings_app_t *g_settings_app = NULL;

// ============================================================================
// Category Names
// ============================================================================

static const char *category_names[SETTINGS_NUM_CATEGORIES] = {
    "Appearance",
    "System",
    "Network",
    "Security",
    "About"
};

const char *settings_get_category_name(int category) {
    if (category >= 0 && category < SETTINGS_NUM_CATEGORIES) {
        return category_names[category];
    }
    return "Unknown";
}

// ============================================================================
// Panel Registration Implementation
// ============================================================================

int settings_register_panel(const settings_panel_def_t *def) {
    if (!def || !def->name) {
        kprintf("[Settings] Cannot register panel: invalid definition\n");
        return -1;
    }

    if (g_panel_count >= SETTINGS_MAX_PANELS) {
        kprintf("[Settings] Cannot register panel: maximum reached\n");
        return -1;
    }

    // Check for duplicate
    for (int i = 0; i < g_panel_count; i++) {
        if (strcmp(g_panels[i].def.name, def->name) == 0) {
            kprintf("[Settings] Panel '%s' already registered\n", def->name);
            return i;
        }
    }

    // Copy definition
    int index = g_panel_count;
    memset(&g_panels[index], 0, sizeof(settings_panel_t));
    memcpy(&g_panels[index].def, def, sizeof(settings_panel_def_t));
    g_panels[index].initialized = false;
    g_panels[index].dirty = false;
    g_panels[index].scroll_y = 0;
    g_panels[index].content_height = 0;
    g_panel_count++;

    kprintf("[Settings] Registered panel '%s' (category: %d, priority: %d)\n",
            def->name, def->category, def->priority);

    return index;
}

void settings_unregister_panel(const char *name) {
    for (int i = 0; i < g_panel_count; i++) {
        if (strcmp(g_panels[i].def.name, name) == 0) {
            // Cleanup if initialized
            if (g_panels[i].initialized && g_panels[i].def.cleanup) {
                g_panels[i].def.cleanup(&g_panels[i]);
            }

            // Shift remaining panels
            for (int j = i; j < g_panel_count - 1; j++) {
                memcpy(&g_panels[j], &g_panels[j + 1], sizeof(settings_panel_t));
            }
            g_panel_count--;

            // Update current panel if needed
            if (g_current_panel == i) {
                g_current_panel = (g_panel_count > 0) ? 0 : -1;
            } else if (g_current_panel > i) {
                g_current_panel--;
            }

            kprintf("[Settings] Unregistered panel '%s'\n", name);
            return;
        }
    }
}

settings_panel_t *settings_get_panel(const char *name) {
    for (int i = 0; i < g_panel_count; i++) {
        if (strcmp(g_panels[i].def.name, name) == 0) {
            return &g_panels[i];
        }
    }
    return NULL;
}

settings_panel_t *settings_get_panel_at(int index) {
    if (index >= 0 && index < g_panel_count) {
        return &g_panels[index];
    }
    return NULL;
}

int settings_get_panel_count(void) {
    return g_panel_count;
}

int settings_get_panels_in_category(int category, int *indices, int max_indices) {
    int count = 0;
    for (int i = 0; i < g_panel_count && count < max_indices; i++) {
        if (g_panels[i].def.category == category) {
            indices[count++] = i;
        }
    }
    return count;
}

// ============================================================================
// Panel Navigation Implementation
// ============================================================================

void settings_switch_panel(const char *name) {
    for (int i = 0; i < g_panel_count; i++) {
        if (strcmp(g_panels[i].def.name, name) == 0) {
            settings_switch_panel_index(i);
            return;
        }
    }
    kprintf("[Settings] Panel '%s' not found\n", name);
}

void settings_switch_panel_index(int index) {
    if (index < 0 || index >= g_panel_count) {
        return;
    }

    // Cleanup previous panel if switching
    if (g_current_panel >= 0 && g_current_panel != index) {
        settings_panel_t *prev = &g_panels[g_current_panel];
        if (prev->initialized && prev->def.cleanup) {
            prev->def.cleanup(prev);
            prev->initialized = false;
        }
    }

    g_current_panel = index;
    settings_panel_t *panel = &g_panels[index];

    // Initialize if needed
    if (!panel->initialized && panel->def.init) {
        panel->def.init(panel);
        panel->initialized = true;
    }

    // Reset scroll
    panel->scroll_y = 0;
    if (g_settings_app) {
        g_settings_app->content_scroll_y = 0;
    }

    kprintf("[Settings] Switched to panel '%s'\n", panel->def.name);

    // Request redraw
    if (g_settings_app && g_settings_app->window) {
        wm_invalidate_rect(&g_settings_app->window->bounds);
    }
}

settings_panel_t *settings_get_current_panel(void) {
    if (g_current_panel >= 0 && g_current_panel < g_panel_count) {
        return &g_panels[g_current_panel];
    }
    return NULL;
}

int settings_get_current_panel_index(void) {
    return g_current_panel;
}

// ============================================================================
// Panel Helper Functions
// ============================================================================

void settings_panel_mark_dirty(settings_panel_t *panel) {
    if (panel) {
        panel->dirty = true;
    }
}

bool settings_has_unsaved_changes(void) {
    for (int i = 0; i < g_panel_count; i++) {
        if (g_panels[i].dirty) {
            return true;
        }
    }
    return false;
}

void settings_apply_all(void) {
    for (int i = 0; i < g_panel_count; i++) {
        if (g_panels[i].dirty && g_panels[i].def.apply) {
            g_panels[i].def.apply(&g_panels[i]);
            g_panels[i].dirty = false;
        }
    }
    kprintf("[Settings] Applied all changes\n");
}

void settings_discard_all(void) {
    for (int i = 0; i < g_panel_count; i++) {
        if (g_panels[i].dirty) {
            // Re-initialize to discard changes
            if (g_panels[i].def.cleanup) {
                g_panels[i].def.cleanup(&g_panels[i]);
            }
            g_panels[i].initialized = false;
            g_panels[i].dirty = false;
        }
    }
    kprintf("[Settings] Discarded all changes\n");
}

void settings_panel_set_scroll(settings_panel_t *panel, int32_t scroll_y) {
    if (panel) {
        panel->scroll_y = scroll_y;
    }
}

int32_t settings_panel_get_scroll(settings_panel_t *panel) {
    return panel ? panel->scroll_y : 0;
}

// ============================================================================
// Drawing Functions
// ============================================================================

// Draw text at a position
static void settings_draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    while (*text) {
        const uint8_t *glyph = font_get_glyph(*text);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(x + col, y + row, color);
                    }
                }
            }
        }
        x += FONT_WIDTH;
        text++;
    }
}

// Draw the search bar
static void settings_draw_search(settings_app_t *app) {
    int32_t sx = app->nav_x + NAV_SECTION_PADDING;
    int32_t sy = app->nav_y + NAV_SECTION_PADDING;
    int32_t sw = app->nav_width - (NAV_SECTION_PADDING * 2);
    int32_t sh = NAV_SEARCH_HEIGHT - NAV_SECTION_PADDING;

    // Search box background
    uint32_t bg = app->search_focused ? 0xFFFFFF : 0xF8F8F8;
    fb_fill_rect(sx, sy, sw, sh, bg);
    fb_draw_rect(sx, sy, sw, sh, 0xCCCCCC);

    // Search icon placeholder (magnifying glass shape)
    int32_t icon_x = sx + 6;
    int32_t icon_y = sy + (sh - 12) / 2;
    fb_fill_rect(icon_x, icon_y, 10, 10, 0x999999);  // Placeholder circle

    // Search text or placeholder
    int32_t text_x = sx + 22;
    int32_t text_y = sy + (sh - FONT_HEIGHT) / 2;

    if (app->search_text[0]) {
        settings_draw_text(text_x, text_y, app->search_text, THEME_LABEL_TEXT);

        // Draw cursor if focused
        if (app->search_focused) {
            int cursor_x = text_x + strlen(app->search_text) * FONT_WIDTH;
            fb_draw_line(cursor_x, text_y, cursor_x, text_y + FONT_HEIGHT, 0x000000);
        }
    } else {
        settings_draw_text(text_x, text_y, "Search settings...", 0x999999);
    }
}

// Draw navigation panel
static void settings_draw_nav(settings_app_t *app) {
    // Navigation background
    fb_fill_rect(app->nav_x, app->nav_y, app->nav_width, app->nav_height, SETTINGS_NAV_BG);

    // Draw search bar
    settings_draw_search(app);

    // Calculate nav item start position
    int32_t item_y = app->nav_y + NAV_SEARCH_HEIGHT + NAV_SECTION_PADDING;
    int item_index = 0;

    // Draw panels grouped by category
    for (int cat = 0; cat < SETTINGS_NUM_CATEGORIES; cat++) {
        // Count panels in this category
        int panel_indices[SETTINGS_MAX_PANELS];
        int panel_count = settings_get_panels_in_category(cat, panel_indices, SETTINGS_MAX_PANELS);

        if (panel_count == 0) continue;

        // Draw category header
        settings_draw_text(app->nav_x + NAV_PADDING, item_y + 4,
                          category_names[cat], SETTINGS_SECTION_TEXT);
        item_y += NAV_ITEM_HEIGHT;

        // Draw panels in this category
        for (int i = 0; i < panel_count; i++) {
            int pidx = panel_indices[i];
            settings_panel_t *panel = &g_panels[pidx];

            // Determine background color
            uint32_t bg_color = SETTINGS_NAV_BG;
            if (pidx == g_current_panel) {
                bg_color = SETTINGS_NAV_SELECTED;
            } else if (item_index == app->nav_hover_index) {
                bg_color = SETTINGS_NAV_HOVER;
            }

            // Draw item background
            fb_fill_rect(app->nav_x, item_y, app->nav_width, NAV_ITEM_HEIGHT, bg_color);

            // Draw selection indicator for current panel
            if (pidx == g_current_panel) {
                fb_fill_rect(app->nav_x, item_y, 3, NAV_ITEM_HEIGHT, THEME_SELECTION_BG);
            }

            // Draw panel name (indented)
            uint32_t text_color = (pidx == g_current_panel) ?
                                  SETTINGS_NAV_ACTIVE : SETTINGS_NAV_TEXT;
            settings_draw_text(app->nav_x + NAV_ITEM_INDENT, item_y + (NAV_ITEM_HEIGHT - FONT_HEIGHT) / 2,
                              panel->def.name, text_color);

            item_y += NAV_ITEM_HEIGHT;
            item_index++;
        }

        // Add spacing between categories
        item_y += NAV_SECTION_PADDING;
    }

    // Draw border between nav and content
    fb_draw_line(app->nav_x + app->nav_width - 1, app->nav_y,
                 app->nav_x + app->nav_width - 1, app->nav_y + app->nav_height, 0xDDDDDD);
}

// Draw content panel
static void settings_draw_content(settings_app_t *app) {
    // Content background
    fb_fill_rect(app->content_x, app->content_y, app->content_width, app->content_height,
                 SETTINGS_CONTENT_BG);

    settings_panel_t *panel = settings_get_current_panel();
    if (!panel) {
        // No panel selected - draw placeholder
        const char *msg = "Select a setting from the left";
        int msg_len = strlen(msg);
        int msg_x = app->content_x + (app->content_width - msg_len * FONT_WIDTH) / 2;
        int msg_y = app->content_y + app->content_height / 2 - FONT_HEIGHT / 2;
        settings_draw_text(msg_x, msg_y, msg, 0x999999);
        return;
    }

    // Draw panel header
    int32_t header_y = app->content_y + CONTENT_PADDING;
    settings_draw_text(app->content_x + CONTENT_PADDING, header_y,
                      panel->def.name, 0x000000);

    // Draw separator line
    int32_t sep_y = header_y + FONT_HEIGHT + 8;
    fb_draw_line(app->content_x + CONTENT_PADDING, sep_y,
                 app->content_x + app->content_width - CONTENT_PADDING, sep_y, 0xE0E0E0);

    // Set up content bounds for panel drawing
    panel->content_x = app->content_x + CONTENT_PADDING;
    panel->content_y = sep_y + CONTENT_PADDING;
    panel->content_width = app->content_width - (CONTENT_PADDING * 2);
    panel->content_height_visible = app->content_height - (sep_y - app->content_y + CONTENT_PADDING * 2);

    // Call panel's draw function if available
    if (panel->def.draw) {
        panel->def.draw(panel, panel->content_x, panel->content_y,
                       panel->content_width, panel->content_height_visible);
    }
}

// Draw the entire settings window
static void settings_redraw(settings_app_t *app) {
    if (!app || !app->window) return;

    // Get content bounds
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(app->window, &wx, &wy, &ww, &wh);

    // Calculate layout
    app->nav_x = wx;
    app->nav_y = wy;
    app->nav_width = NAV_PANEL_WIDTH;
    app->nav_height = wh;

    app->content_x = wx + NAV_PANEL_WIDTH;
    app->content_y = wy;
    app->content_width = ww - NAV_PANEL_WIDTH;
    app->content_height = wh;

    // Draw components
    settings_draw_nav(app);
    settings_draw_content(app);
}

// ============================================================================
// Event Handling
// ============================================================================

// Get nav item index at position
static int settings_get_nav_item_at(settings_app_t *app, int32_t x, int32_t y) {
    if (x < app->nav_x || x >= app->nav_x + app->nav_width) return -1;

    int32_t item_y = app->nav_y + NAV_SEARCH_HEIGHT + NAV_SECTION_PADDING;
    int item_index = 0;

    for (int cat = 0; cat < SETTINGS_NUM_CATEGORIES; cat++) {
        int panel_indices[SETTINGS_MAX_PANELS];
        int panel_count = settings_get_panels_in_category(cat, panel_indices, SETTINGS_MAX_PANELS);

        if (panel_count == 0) continue;

        // Skip category header
        item_y += NAV_ITEM_HEIGHT;

        // Check each panel item
        for (int i = 0; i < panel_count; i++) {
            if (y >= item_y && y < item_y + NAV_ITEM_HEIGHT) {
                return panel_indices[i];
            }
            item_y += NAV_ITEM_HEIGHT;
            item_index++;
        }

        item_y += NAV_SECTION_PADDING;
    }

    return -1;
}

// Handle mouse events
static void settings_handle_mouse(settings_app_t *app, gui_event_t *event) {
    int32_t mx = event->mouse_x;
    int32_t my = event->mouse_y;

    // Check if in search bar
    int32_t search_x = app->nav_x + NAV_SECTION_PADDING;
    int32_t search_y = app->nav_y + NAV_SECTION_PADDING;
    int32_t search_w = app->nav_width - (NAV_SECTION_PADDING * 2);
    int32_t search_h = NAV_SEARCH_HEIGHT - NAV_SECTION_PADDING;

    bool in_search = (mx >= search_x && mx < search_x + search_w &&
                      my >= search_y && my < search_y + search_h);

    if (event->type == EVENT_MOUSE_UP && (event->mouse_buttons & MOUSE_BUTTON_LEFT)) {
        // Handle search focus
        if (in_search) {
            app->search_focused = true;
            wm_invalidate_rect(&app->window->bounds);
            return;
        } else {
            app->search_focused = false;
        }

        // Handle nav item click
        int nav_idx = settings_get_nav_item_at(app, mx, my);
        if (nav_idx >= 0) {
            settings_switch_panel_index(nav_idx);
            return;
        }

        // Handle content area clicks - pass to current panel
        settings_panel_t *panel = settings_get_current_panel();
        if (panel && panel->def.handle_event) {
            panel->def.handle_event(panel, event);
        }
    }
    else if (event->type == EVENT_MOUSE_MOVE) {
        // Update nav hover
        int prev_hover = app->nav_hover_index;
        app->nav_hover_index = settings_get_nav_item_at(app, mx, my);

        if (prev_hover != app->nav_hover_index) {
            wm_invalidate_rect(&app->window->bounds);
        }

        // Pass to current panel
        settings_panel_t *panel = settings_get_current_panel();
        if (panel && panel->def.handle_event) {
            panel->def.handle_event(panel, event);
        }
    }
}

// Handle keyboard events
static void settings_handle_key(settings_app_t *app, gui_event_t *event) {
    char c = event->key_char;

    // Handle search input
    if (app->search_focused) {
        if (c == '\b' && app->search_cursor > 0) {
            app->search_text[--app->search_cursor] = '\0';
            wm_invalidate_rect(&app->window->bounds);
        } else if (c >= ' ' && c <= '~' && app->search_cursor < 63) {
            app->search_text[app->search_cursor++] = c;
            app->search_text[app->search_cursor] = '\0';
            // TODO: Filter panels based on search
            wm_invalidate_rect(&app->window->bounds);
        } else if (c == 27) {  // ESC
            app->search_focused = false;
            app->search_text[0] = '\0';
            app->search_cursor = 0;
            wm_invalidate_rect(&app->window->bounds);
        }
        return;
    }

    // Global key handling
    if (c == 27) {  // ESC
        // Close settings
        window_close(app->window);
    } else if (c == '\t') {
        // Tab to next panel
        int next = (g_current_panel + 1) % g_panel_count;
        settings_switch_panel_index(next);
    }

    // Pass to current panel
    settings_panel_t *panel = settings_get_current_panel();
    if (panel && panel->def.handle_event) {
        panel->def.handle_event(panel, event);
    }
}

// ============================================================================
// Window Manager Callbacks
// ============================================================================

static void settings_main_on_event(void *app_data, gui_event_t *event) {
    settings_app_t *app = (settings_app_t *)app_data;
    if (!app || !event) return;

    switch (event->type) {
        case EVENT_MOUSE_MOVE:
        case EVENT_MOUSE_DOWN:
        case EVENT_MOUSE_UP:
            settings_handle_mouse(app, event);
            break;

        case EVENT_KEY_DOWN:
            settings_handle_key(app, event);
            break;

        case EVENT_WINDOW_CLOSE:
            kprintf("[Settings] Close button clicked\n");

            // Check for unsaved changes
            if (settings_has_unsaved_changes()) {
                // TODO: Show confirmation dialog
                kprintf("[Settings] Warning: Discarding unsaved changes\n");
                settings_discard_all();
            }

            // Cleanup and close
            wm_unregister_app(app->app_id);
            if (app->dock_index >= 0) {
                dock_remove_app(app->dock_index);
            }
            window_hide(app->window);
            wm_invalidate_all();
            break;

        default:
            break;
    }
}

static void settings_main_on_draw(void *app_data) {
    settings_app_t *app = (settings_app_t *)app_data;
    if (!app || !app->window) return;

    window_draw(app->window);
    settings_redraw(app);
}

static void settings_main_on_destroy(void *app_data) {
    settings_app_t *app = (settings_app_t *)app_data;
    if (!app) return;

    kprintf("[Settings] Destroying settings app\n");

    // Cleanup all panels
    for (int i = 0; i < g_panel_count; i++) {
        if (g_panels[i].initialized && g_panels[i].def.cleanup) {
            g_panels[i].def.cleanup(&g_panels[i]);
            g_panels[i].initialized = false;
        }
    }

    // Free app data
    if (app->window) {
        window_destroy(app->window);
    }
    kfree(app);

    if (g_settings_app == app) {
        g_settings_app = NULL;
    }
}

// ============================================================================
// Public API
// ============================================================================

// Create and launch the settings application
void settings_unified_launch(void) {
    kprintf("[Settings] Launching unified settings app...\n");

    // Check if already running
    if (g_settings_app) {
        kprintf("[Settings] Already running, bringing to front\n");
        wm_focus_window(g_settings_app->window);
        return;
    }

    // Allocate app state
    settings_app_t *app = (settings_app_t *)kzalloc(sizeof(settings_app_t));
    if (!app) {
        kprintf("[Settings] Failed to allocate app state\n");
        return;
    }

    // Initialize state
    memset(app, 0, sizeof(settings_app_t));
    app->app_id = -1;
    app->dock_index = -1;
    app->nav_hover_index = -1;

    // Center window on screen
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();
    int x = (screen_w - SETTINGS_WINDOW_WIDTH) / 2;
    int y = (screen_h - SETTINGS_WINDOW_HEIGHT) / 2 - 20;

    // Create window
    app->window = window_create(SETTINGS_WINDOW_TITLE, x, y,
                                SETTINGS_WINDOW_WIDTH, SETTINGS_WINDOW_HEIGHT);
    if (!app->window) {
        kprintf("[Settings] Failed to create window\n");
        kfree(app);
        return;
    }

    // Set window properties
    app->window->bg_color = SETTINGS_CONTENT_BG;

    // Add to taskbar
    app->dock_index = dock_add_app("Settings", DOCK_ICON_SETTINGS, NULL);

    // Register with window manager
    app->app_id = wm_register_app(
        app->window,
        app,
        settings_main_on_event,
        settings_main_on_draw,
        settings_main_on_destroy
    );

    if (app->app_id < 0) {
        kprintf("[Settings] Failed to register with window manager\n");
        if (app->dock_index >= 0) {
            dock_remove_app(app->dock_index);
        }
        window_destroy(app->window);
        kfree(app);
        return;
    }

    g_settings_app = app;
    app->running = true;

    // Select first panel if any
    if (g_panel_count > 0 && g_current_panel < 0) {
        settings_switch_panel_index(0);
    }

    wm_invalidate_all();
    kprintf("[Settings] Unified settings app launched (app_id=%d)\n", app->app_id);
}

// Check if settings app is running
bool settings_is_running(void) {
    return g_settings_app != NULL;
}

// Get settings window (for external access)
window_t *settings_get_window(void) {
    return g_settings_app ? g_settings_app->window : NULL;
}
