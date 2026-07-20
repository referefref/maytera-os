// registry.h - System Registry for MayteraOS
// Centralized storage for file associations, application menus, and system settings
#ifndef REGISTRY_H
#define REGISTRY_H

#include "../types.h"

// ============================================================================
// File Association System
// ============================================================================

#define REG_MAX_EXTENSIONS      64
#define REG_MAX_EXT_LEN         16
#define REG_MAX_APP_NAME        32
#define REG_MAX_MIME_TYPE       64

// File type categories
typedef enum {
    FILE_CAT_UNKNOWN = 0,
    FILE_CAT_TEXT,
    FILE_CAT_IMAGE,
    FILE_CAT_AUDIO,
    FILE_CAT_VIDEO,
    FILE_CAT_DOCUMENT,
    FILE_CAT_ARCHIVE,
    FILE_CAT_EXECUTABLE,
    FILE_CAT_SYSTEM,
    FILE_CAT_FOLDER
} file_category_t;

// File association entry
typedef struct {
    char extension[REG_MAX_EXT_LEN];      // e.g., ".txt", ".bmp"
    char mime_type[REG_MAX_MIME_TYPE];    // e.g., "text/plain", "image/bmp"
    char app_name[REG_MAX_APP_NAME];      // e.g., "Editor", "Image Viewer"
    void (*open_handler)(const char *path);  // Function to open file
    file_category_t category;
    uint32_t icon_id;                     // Icon to display for this file type
    bool registered;
} file_assoc_t;

// ============================================================================
// Application Menu System
// ============================================================================

#define REG_MAX_MENU_ITEMS      32
#define REG_MAX_MENU_CATEGORIES 8
#define REG_MAX_CATEGORY_NAME   24

// Menu item
typedef struct {
    char name[REG_MAX_APP_NAME];
    uint32_t icon_id;
    void (*launch_handler)(void);
    bool separator_after;
    bool registered;
} menu_item_reg_t;

// Menu category
typedef struct {
    char name[REG_MAX_CATEGORY_NAME];
    menu_item_reg_t items[REG_MAX_MENU_ITEMS];
    int item_count;
    bool registered;
} menu_category_t;

// ============================================================================
// Registry API - File Associations
// ============================================================================

// Initialize the registry system
void registry_init(void);

// Register a file association
bool registry_register_file_assoc(const char *extension,
                                   const char *mime_type,
                                   const char *app_name,
                                   void (*open_handler)(const char *path),
                                   file_category_t category,
                                   uint32_t icon_id);

// Get file association for an extension
file_assoc_t *registry_get_file_assoc(const char *extension);

// Get file association by filename (extracts extension)
file_assoc_t *registry_get_file_assoc_by_name(const char *filename);

// Open a file using the registered handler
bool registry_open_file(const char *path);

// Get icon for a file
uint32_t registry_get_file_icon(const char *filename);

// Get file category
file_category_t registry_get_file_category(const char *filename);

// ============================================================================
// Registry API - Application Menus
// ============================================================================

// Register a menu category
bool registry_register_menu_category(const char *category_name);

// Register an application in a category
bool registry_register_app(const char *category,
                           const char *app_name,
                           uint32_t icon_id,
                           void (*launch_handler)(void),
                           bool separator_after);

// Get all menu categories
int registry_get_menu_categories(menu_category_t **categories);

// Get items in a category
int registry_get_menu_items(const char *category, menu_item_reg_t **items);

// Get all apps (flat list for context menus)
int registry_get_all_apps(menu_item_reg_t **items, int max_items);

// ============================================================================
// Default Registrations
// ============================================================================

// Called during init to register all default file associations and apps
void registry_register_defaults(void);

#endif // REGISTRY_H
