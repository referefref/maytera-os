// default_apps.h - Default Applications settings panel for MayteraOS
#ifndef DEFAULT_APPS_H
#define DEFAULT_APPS_H

#include "../../types.h"
#include "../window.h"
#include "../../apps/associations.h"

// Panel dimensions
#define DEFAPPS_PADDING         15
#define DEFAPPS_ROW_HEIGHT      32
#define DEFAPPS_LABEL_WIDTH     150
#define DEFAPPS_COMBO_WIDTH     180
#define DEFAPPS_COMBO_HEIGHT    24

// Maximum categories to show
#define DEFAPPS_MAX_CATEGORIES  10

// Category for grouping MIME types
typedef struct {
    const char *name;           // Category display name (e.g., "Web Browser")
    const char *mime_type;      // Primary MIME type
    const char *description;    // Description
    assoc_app_t *current_app;   // Currently selected app
    int app_count;              // Number of available apps
    assoc_app_t *apps[ASSOC_MAX_APPS]; // Available apps
} defapps_category_t;

// Panel state
typedef struct {
    defapps_category_t categories[DEFAPPS_MAX_CATEGORIES];
    int category_count;
    int hover_row;              // Row being hovered (-1 if none)
    int dropdown_row;           // Row with open dropdown (-1 if none)
    int dropdown_hover;         // Item in dropdown being hovered
    int scroll_offset;          // Scroll position
} defapps_panel_t;

// ============================================================================
// Default Applications Panel API
// ============================================================================

/**
 * Initialize the default apps panel data
 * @param panel Panel state structure
 */
void defapps_init(defapps_panel_t *panel);

/**
 * Draw the default apps panel
 * @param panel Panel state
 * @param x Panel x position
 * @param y Panel y position
 * @param w Panel width
 * @param h Panel height
 */
void defapps_draw(defapps_panel_t *panel, int32_t x, int32_t y, int32_t w, int32_t h);

/**
 * Handle mouse click in the panel
 * @param panel Panel state
 * @param x Click x position (relative to panel)
 * @param y Click y position (relative to panel)
 * @param panel_x Panel x position on screen
 * @param panel_y Panel y position on screen
 * @return true if click was handled
 */
bool defapps_handle_click(defapps_panel_t *panel, int32_t x, int32_t y,
                          int32_t panel_x, int32_t panel_y);

/**
 * Handle mouse motion in the panel
 * @param panel Panel state
 * @param x Mouse x position (relative to panel)
 * @param y Mouse y position (relative to panel)
 */
void defapps_handle_motion(defapps_panel_t *panel, int32_t x, int32_t y);

/**
 * Handle scroll in the panel
 * @param panel Panel state
 * @param delta Scroll delta
 */
void defapps_handle_scroll(defapps_panel_t *panel, int delta);

/**
 * Save changes to configuration
 * @param panel Panel state
 * @return true on success
 */
bool defapps_save(defapps_panel_t *panel);

#endif // DEFAULT_APPS_H
