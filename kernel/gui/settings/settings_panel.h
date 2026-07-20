// settings_panel.h - Panel interface for unified Settings application
// MayteraOS Unified Settings Framework
#ifndef SETTINGS_PANEL_H
#define SETTINGS_PANEL_H

#include "../../types.h"
#include "../window.h"

// Forward declarations
struct settings_panel;
struct settings_app;

// ============================================================================
// Panel Constants
// ============================================================================

#define SETTINGS_MAX_PANELS         16
#define SETTINGS_PANEL_NAME_LEN     32
#define SETTINGS_PANEL_ICON_LEN     16

// Panel categories
#define SETTINGS_CAT_APPEARANCE     0   // Themes, Desktop, Display
#define SETTINGS_CAT_SYSTEM         1   // Sound, Hardware, Date/Time
#define SETTINGS_CAT_NETWORK        2   // Network settings
#define SETTINGS_CAT_SECURITY       3   // Users, Security
#define SETTINGS_CAT_ABOUT          4   // About, Help
#define SETTINGS_NUM_CATEGORIES     5

// Navigation item dimensions
#define NAV_ITEM_HEIGHT             28
#define NAV_PANEL_WIDTH             160
#define NAV_ICON_SIZE               16
#define NAV_PADDING                 8

// Search bar dimensions
#define SEARCH_BAR_HEIGHT           32
#define SEARCH_BAR_PADDING          8

// Content area padding
#define CONTENT_PADDING             16
#define CONTENT_SECTION_GAP         20
#define CONTENT_LINE_HEIGHT         24

// ============================================================================
// Panel Lifecycle Callbacks
// ============================================================================

// Initialize panel (allocate resources, load settings)
typedef void (*panel_init_fn)(struct settings_panel *panel);

// Draw panel content to the content area
typedef void (*panel_draw_fn)(struct settings_panel *panel, int32_t x, int32_t y,
                              int32_t width, int32_t height);

// Handle GUI events (mouse clicks, key presses)
typedef void (*panel_event_fn)(struct settings_panel *panel, gui_event_t *event);

// Apply changes made in the panel
typedef void (*panel_apply_fn)(struct settings_panel *panel);

// Cleanup panel resources
typedef void (*panel_cleanup_fn)(struct settings_panel *panel);

// ============================================================================
// Panel Definition Structure
// ============================================================================

// Panel definition - used to register a settings panel
typedef struct settings_panel_def {
    const char *name;           // Panel name (e.g., "Appearance")
    const char *icon;           // Icon character/identifier (e.g., "paint")
    int category;               // Category ID
    int priority;               // Sort priority within category (lower = first)

    // Lifecycle callbacks
    panel_init_fn init;         // Called when panel is first selected
    panel_draw_fn draw;         // Called to render panel content
    panel_event_fn handle_event;// Called for GUI events
    panel_apply_fn apply;       // Called when Apply button clicked
    panel_cleanup_fn cleanup;   // Called when panel is deselected/destroyed
} settings_panel_def_t;

// ============================================================================
// Panel Instance Structure
// ============================================================================

// Panel instance - runtime state for a registered panel
typedef struct settings_panel {
    settings_panel_def_t def;   // Panel definition (copied)
    void *user_data;            // Panel-specific data (allocated by init)
    bool initialized;           // Has init() been called?
    bool dirty;                 // Have settings changed?
    int32_t scroll_y;           // Vertical scroll offset
    int32_t content_height;     // Total content height (for scrolling)

    // Content bounds (set before draw is called)
    int32_t content_x;
    int32_t content_y;
    int32_t content_width;
    int32_t content_height_visible;
} settings_panel_t;

// ============================================================================
// Panel Registration API
// ============================================================================

// Register a settings panel
// Returns panel index on success, -1 on failure
int settings_register_panel(const settings_panel_def_t *def);

// Unregister a panel by name
void settings_unregister_panel(const char *name);

// Get panel by name (returns NULL if not found)
settings_panel_t *settings_get_panel(const char *name);

// Get panel by index (returns NULL if invalid)
settings_panel_t *settings_get_panel_at(int index);

// Get number of registered panels
int settings_get_panel_count(void);

// ============================================================================
// Panel Navigation API
// ============================================================================

// Switch to a different panel
void settings_switch_panel(const char *name);

// Switch to panel by index
void settings_switch_panel_index(int index);

// Get current panel
settings_panel_t *settings_get_current_panel(void);

// Get current panel index
int settings_get_current_panel_index(void);

// ============================================================================
// Panel Helper Functions
// ============================================================================

// Mark current panel as dirty (settings changed)
void settings_panel_mark_dirty(settings_panel_t *panel);

// Check if any panel has unsaved changes
bool settings_has_unsaved_changes(void);

// Apply all pending changes
void settings_apply_all(void);

// Discard all pending changes
void settings_discard_all(void);

// Set panel scroll position
void settings_panel_set_scroll(settings_panel_t *panel, int32_t scroll_y);

// Get panel scroll position
int32_t settings_panel_get_scroll(settings_panel_t *panel);

// ============================================================================
// Category Names
// ============================================================================

// Get category name by ID
const char *settings_get_category_name(int category);

// Get panels in a category
// Returns count, fills array with panel indices
int settings_get_panels_in_category(int category, int *indices, int max_indices);

#endif // SETTINGS_PANEL_H
