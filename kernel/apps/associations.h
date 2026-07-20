// associations.h - File associations and "Open With" support for MayteraOS
// Manages default applications for file types with persistent storage
#ifndef ASSOCIATIONS_H
#define ASSOCIATIONS_H

#include "../types.h"
#include "mime.h"

// Configuration
#define ASSOC_CONFIG_PATH       "/etc/associations.conf"
#define ASSOC_MAX_APPS          32      // Max apps that can handle a MIME type
#define ASSOC_MAX_ENTRIES       128     // Max total association entries
#define ASSOC_APP_NAME_MAX      64      // Max app name length
#define ASSOC_APP_PATH_MAX      256     // Max app path length
#define ASSOC_APP_DESC_MAX      128     // Max app description length

// Application capability flags
#define APP_CAP_OPEN            0x01    // Can open files
#define APP_CAP_EDIT            0x02    // Can edit files
#define APP_CAP_PRINT           0x04    // Can print files
#define APP_CAP_PREVIEW         0x08    // Can provide quick preview
#define APP_CAP_CREATE          0x10    // Can create new files of this type
#define APP_CAP_DEFAULT         0x20    // Is the default app for this type

// Application entry (an app that can handle files)
typedef struct {
    char name[ASSOC_APP_NAME_MAX];      // Display name (e.g., "Text Editor")
    char path[ASSOC_APP_PATH_MAX];      // Path to app or internal ID
    char description[ASSOC_APP_DESC_MAX]; // Description of the app
    char icon_name[32];                  // Icon identifier
    uint32_t capabilities;               // APP_CAP_* flags
    bool is_internal;                    // Internal (kernel) app vs external
    bool registered;                     // Entry is active
    
    // Handler function for internal apps
    void (*open_handler)(const char *filepath);
} assoc_app_t;

// MIME type to app association entry
typedef struct {
    char mime_type[MIME_TYPE_MAX_LEN];  // MIME type (e.g., "text/plain")
    char default_app[ASSOC_APP_NAME_MAX]; // Default app name
    char apps[ASSOC_MAX_APPS][ASSOC_APP_NAME_MAX]; // All apps that can handle this type
    int app_count;                       // Number of associated apps
    bool registered;                     // Entry is active
} assoc_entry_t;

// ============================================================================
// Associations API
// ============================================================================

/**
 * Initialize the associations system
 */
void assoc_init(void);

/**
 * Load associations from configuration file
 * @return true on success
 */
bool assoc_load_config(void);

/**
 * Save associations to configuration file
 * @return true on success
 */
bool assoc_save_config(void);

/**
 * Register an application
 * @param name Application display name
 * @param path Path to executable or internal ID
 * @param description Description text
 * @param icon_name Icon identifier
 * @param capabilities APP_CAP_* flags
 * @param is_internal Is this an internal kernel app?
 * @param open_handler Function to call for internal apps (can be NULL for external)
 * @return true on success
 */
bool assoc_register_app(const char *name, const char *path, const char *description,
                        const char *icon_name, uint32_t capabilities, bool is_internal,
                        void (*open_handler)(const char *filepath));

/**
 * Register that an app can handle a MIME type
 * @param mime_type MIME type string
 * @param app_name Application name (must be registered first)
 * @param set_default Set this app as default for this type?
 * @return true on success
 */
bool assoc_register_handler(const char *mime_type, const char *app_name, bool set_default);

/**
 * Get the default application for a MIME type
 * @param mime_type MIME type string
 * @return Application entry or NULL if no default set
 */
assoc_app_t *assoc_get_default(const char *mime_type);

/**
 * Get the default application for a file by path
 * @param filepath File path (extension will be used to determine MIME type)
 * @return Application entry or NULL if no default set
 */
assoc_app_t *assoc_get_default_for_file(const char *filepath);

/**
 * Set the default application for a MIME type
 * @param mime_type MIME type string
 * @param app_name Application name
 * @return true on success
 */
bool assoc_set_default(const char *mime_type, const char *app_name);

/**
 * Get all applications that can handle a MIME type
 * @param mime_type MIME type string
 * @param apps Output array of app pointers (must be ASSOC_MAX_APPS size)
 * @return Number of apps found
 */
int assoc_get_all(const char *mime_type, assoc_app_t **apps);

/**
 * Get all applications that can handle a file
 * @param filepath File path
 * @param apps Output array of app pointers
 * @return Number of apps found
 */
int assoc_get_all_for_file(const char *filepath, assoc_app_t **apps);

/**
 * Open a file with its default application
 * @param filepath Full path to the file
 * @return true if opened successfully
 */
bool assoc_open_file(const char *filepath);

/**
 * Open a file with a specific application
 * @param filepath Full path to the file
 * @param app_name Application name to use
 * @return true if opened successfully
 */
bool assoc_open_with(const char *filepath, const char *app_name);

/**
 * Get an application by name
 * @param name Application name
 * @return Application entry or NULL if not found
 */
assoc_app_t *assoc_get_app(const char *name);

/**
 * Get all registered applications
 * @param count Output: number of apps
 * @return Array of all app entries
 */
assoc_app_t *assoc_get_all_apps(int *count);

/**
 * Get association entry for a MIME type
 * @param mime_type MIME type string
 * @return Association entry or NULL if not found
 */
assoc_entry_t *assoc_get_entry(const char *mime_type);

/**
 * Get all MIME types that have associations
 * @param count Output: number of entries
 * @return Array of association entries
 */
assoc_entry_t *assoc_get_all_entries(int *count);

// ============================================================================
// "Open With" Dialog Support
// ============================================================================

// Callback when user selects an app in "Open With" dialog
typedef void (*open_with_callback_t)(const char *filepath, const char *app_name, bool always);

/**
 * Show "Open With" dialog for a file
 * @param filepath Path to file
 * @param callback Function to call when user makes a selection (can be NULL)
 * Note: This creates a GUI dialog - implementation in open_with_dialog.c
 */
void assoc_show_open_with_dialog(const char *filepath, open_with_callback_t callback);

// ============================================================================
// INI Parser Support (for associations.conf)
// ============================================================================

// INI section callback
typedef void (*ini_section_callback)(const char *section, void *userdata);

// INI key-value callback  
typedef void (*ini_keyvalue_callback)(const char *section, const char *key, 
                                       const char *value, void *userdata);

/**
 * Parse an INI file
 * @param data INI file content
 * @param len Length of data
 * @param section_cb Called for each [section]
 * @param kv_cb Called for each key=value
 * @param userdata User data passed to callbacks
 * @return true on success
 */
bool ini_parse(const char *data, size_t len,
               ini_section_callback section_cb,
               ini_keyvalue_callback kv_cb,
               void *userdata);

/**
 * Write an INI section header
 * @param buf Output buffer
 * @param size Buffer size
 * @param section Section name
 * @return Number of chars written
 */
int ini_write_section(char *buf, size_t size, const char *section);

/**
 * Write an INI key-value pair
 * @param buf Output buffer
 * @param size Buffer size
 * @param key Key name
 * @param value Value string
 * @return Number of chars written
 */
int ini_write_keyvalue(char *buf, size_t size, const char *key, const char *value);

#endif // ASSOCIATIONS_H
