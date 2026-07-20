// associations.c - File associations and "Open With" support for MayteraOS
// Manages default applications for file types with persistent storage

#include "associations.h"
#include "mime.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"

// External FAT filesystem
extern fat_fs_t g_fat_fs;

// ============================================================================
// Storage
// ============================================================================

static assoc_app_t g_apps[ASSOC_MAX_APPS];
static int g_app_count = 0;

static assoc_entry_t g_entries[ASSOC_MAX_ENTRIES];
static int g_entry_count = 0;

static bool g_assoc_initialized = false;

// ============================================================================
// Helper Functions
// ============================================================================

// Case-insensitive string comparison
static int assoc_stricmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return *a - *b;
}

// Get file extension from path
static const char *get_extension(const char *filepath) {
    if (!filepath) return NULL;
    const char *dot = NULL;
    for (const char *p = filepath; *p; p++) {
        if (*p == '.') dot = p;
        if (*p == '/') dot = NULL;
    }
    return dot;
}

// ============================================================================
// INI Parser Implementation
// ============================================================================

bool ini_parse(const char *data, size_t len,
               ini_section_callback section_cb,
               ini_keyvalue_callback kv_cb,
               void *userdata) {
    if (!data || len == 0) return false;
    
    char current_section[64] = "";
    char line[256];
    size_t line_pos = 0;
    
    for (size_t i = 0; i <= len; i++) {
        char c = (i < len) ? data[i] : '\n';
        
        if (c == '\n' || c == '\r') {
            line[line_pos] = '\0';
            
            // Skip empty lines and comments
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            
            if (*p && *p != '#' && *p != ';') {
                // Check for section header
                if (*p == '[') {
                    char *end = strchr(p, ']');
                    if (end) {
                        *end = '\0';
                        strncpy(current_section, p + 1, sizeof(current_section) - 1);
                        current_section[sizeof(current_section) - 1] = '\0';
                        if (section_cb) {
                            section_cb(current_section, userdata);
                        }
                    }
                }
                // Check for key=value
                else {
                    char *eq = strchr(p, '=');
                    if (eq) {
                        *eq = '\0';
                        char *key = p;
                        char *value = eq + 1;
                        
                        // Trim key
                        char *key_end = eq - 1;
                        while (key_end > key && (*key_end == ' ' || *key_end == '\t')) {
                            *key_end-- = '\0';
                        }
                        
                        // Trim value leading whitespace
                        while (*value == ' ' || *value == '\t') value++;
                        
                        // Trim value trailing whitespace
                        char *val_end = value + strlen(value) - 1;
                        while (val_end > value && (*val_end == ' ' || *val_end == '\t')) {
                            *val_end-- = '\0';
                        }
                        
                        if (kv_cb && current_section[0]) {
                            kv_cb(current_section, key, value, userdata);
                        }
                    }
                }
            }
            
            line_pos = 0;
        } else if (line_pos < sizeof(line) - 1) {
            line[line_pos++] = c;
        }
    }
    
    return true;
}

int ini_write_section(char *buf, size_t size, const char *section) {
    return snprintf(buf, size, "[%s]\n", section);
}

int ini_write_keyvalue(char *buf, size_t size, const char *key, const char *value) {
    return snprintf(buf, size, "%s=%s\n", key, value);
}

// ============================================================================
// Default Applications Registration
// ============================================================================

// Forward declarations for internal app handlers
extern void editor_launch(void);
extern void imageviewer_launch(const char *filepath);
extern void mediaplayer_launch(const char *filepath);
extern void paint_launch(const char *filepath);
extern void terminal_launch(void);
extern void filebrowser_launch(void);
extern void browser_launch_url(const char *url);

// Wrapper functions for apps that need void(void) signature
static void editor_open_wrapper(const char *filepath) {
    editor_launch();
    (void)filepath;
}

static void terminal_open_wrapper(const char *filepath) {
    terminal_launch();
    (void)filepath;
}

static void filebrowser_open_wrapper(const char *filepath) {
    filebrowser_launch();
    (void)filepath;
}

static void browser_open_wrapper(const char *filepath) {
    browser_launch_url(filepath);
}

static void assoc_register_defaults(void) {
    // Register Internal Applications
    assoc_register_app("Text Editor", "internal:editor", 
                       "Edit text and code files",
                       "icon_editor", 
                       APP_CAP_OPEN | APP_CAP_EDIT | APP_CAP_CREATE,
                       true, editor_open_wrapper);
    
    assoc_register_app("Image Viewer", "internal:imageviewer",
                       "View images and photos",
                       "icon_image",
                       APP_CAP_OPEN | APP_CAP_PREVIEW,
                       true, imageviewer_launch);
    
    assoc_register_app("Media Player", "internal:mediaplayer",
                       "Play video and audio files",
                       "icon_video",
                       APP_CAP_OPEN,
                       true, mediaplayer_launch);
    
    assoc_register_app("Paint", "internal:paint",
                       "Create and edit images",
                       "icon_paint",
                       APP_CAP_OPEN | APP_CAP_EDIT | APP_CAP_CREATE,
                       true, paint_launch);
    
    assoc_register_app("Terminal", "internal:terminal",
                       "Command line interface",
                       "icon_terminal",
                       APP_CAP_OPEN,
                       true, terminal_open_wrapper);
    
    assoc_register_app("Files", "internal:filebrowser",
                       "Browse files and folders",
                       "icon_folder",
                       APP_CAP_OPEN,
                       true, filebrowser_open_wrapper);
    
    assoc_register_app("Web Browser", "internal:browser",
                       "Browse the web",
                       "icon_browser",
                       APP_CAP_OPEN,
                       true, browser_open_wrapper);
    
    // Register MIME Type Handlers
    assoc_register_handler("text/plain", "Text Editor", true);
    assoc_register_handler("text/x-c", "Text Editor", true);
    assoc_register_handler("text/html", "Text Editor", true);
    assoc_register_handler("text/html", "Web Browser", false);
    
    assoc_register_handler("image/bmp", "Image Viewer", true);
    assoc_register_handler("image/png", "Image Viewer", true);
    assoc_register_handler("image/jpeg", "Image Viewer", true);
    assoc_register_handler("image/bmp", "Paint", false);
    
    assoc_register_handler("audio/wav", "Media Player", true);
    assoc_register_handler("audio/mpeg", "Media Player", true);
    
    assoc_register_handler("video/mp4", "Media Player", true);
    assoc_register_handler("video/x-msvideo", "Media Player", true);
    
    assoc_register_handler("inode/directory", "Files", true);
}

// ============================================================================
// Configuration File Loading/Saving
// ============================================================================

typedef struct {
    char current_mime[MIME_TYPE_MAX_LEN];
} assoc_parse_state_t;

static void config_section_cb(const char *section, void *userdata) {
    assoc_parse_state_t *state = (assoc_parse_state_t *)userdata;
    strncpy(state->current_mime, section, MIME_TYPE_MAX_LEN - 1);
    state->current_mime[MIME_TYPE_MAX_LEN - 1] = '\0';
}

static void config_kv_cb(const char *section, const char *key, 
                         const char *value, void *userdata) {
    (void)userdata;
    
    if (strcmp(key, "default") == 0) {
        assoc_set_default(section, value);
    }
}

bool assoc_load_config(void) {
    uint32_t size = 0;
    
    void *data = fat_read_file(&g_fat_fs, ASSOC_CONFIG_PATH, &size);
    if (!data || size == 0) {
        return false;
    }
    
    assoc_parse_state_t state;
    memset(&state, 0, sizeof(state));
    
    bool success = ini_parse((char *)data, size, config_section_cb, config_kv_cb, &state);
    
    kfree(data);
    
    if (success) {
        kprintf("[Associations] Loaded config from %s\n", ASSOC_CONFIG_PATH);
    }
    
    return success;
}

bool assoc_save_config(void) {
    char *buf = kmalloc(4096);
    if (!buf) return false;
    
    size_t pos = 0;
    
    pos += snprintf(buf + pos, 4096 - pos, 
                    "# MayteraOS File Associations\n\n");
    
    for (int i = 0; i < g_entry_count; i++) {
        assoc_entry_t *entry = &g_entries[i];
        if (!entry->registered || !entry->default_app[0]) continue;
        
        pos += ini_write_section(buf + pos, 4096 - pos, entry->mime_type);
        pos += ini_write_keyvalue(buf + pos, 4096 - pos, "default", entry->default_app);
        pos += snprintf(buf + pos, 4096 - pos, "\n");
        
        if (pos >= 4000) break;
    }
    
    int result = fat_write_file(&g_fat_fs, ASSOC_CONFIG_PATH, (uint8_t *)buf, pos);
    
    kfree(buf);
    
    if (result == 0) {
        kprintf("[Associations] Saved config to %s\n", ASSOC_CONFIG_PATH);
        return true;
    }
    
    return false;
}

// ============================================================================
// Initialization
// ============================================================================

void assoc_init(void) {
    if (g_assoc_initialized) return;
    
    memset(g_apps, 0, sizeof(g_apps));
    memset(g_entries, 0, sizeof(g_entries));
    g_app_count = 0;
    g_entry_count = 0;
    
    mime_init();
    assoc_register_defaults();
    assoc_load_config();
    
    g_assoc_initialized = true;
    kprintf("[Associations] Initialized with %d apps, %d entries\n", 
            g_app_count, g_entry_count);
}

// ============================================================================
// Application Registration
// ============================================================================

bool assoc_register_app(const char *name, const char *path, const char *description,
                        const char *icon_name, uint32_t capabilities, bool is_internal,
                        void (*open_handler)(const char *filepath)) {
    if (g_app_count >= ASSOC_MAX_APPS) {
        return false;
    }
    
    for (int i = 0; i < g_app_count; i++) {
        if (assoc_stricmp(g_apps[i].name, name) == 0) {
            assoc_app_t *app = &g_apps[i];
            strncpy(app->path, path, ASSOC_APP_PATH_MAX - 1);
            strncpy(app->description, description ? description : "", ASSOC_APP_DESC_MAX - 1);
            strncpy(app->icon_name, icon_name ? icon_name : "", 31);
            app->capabilities = capabilities;
            app->is_internal = is_internal;
            app->open_handler = open_handler;
            return true;
        }
    }
    
    assoc_app_t *app = &g_apps[g_app_count];
    
    strncpy(app->name, name, ASSOC_APP_NAME_MAX - 1);
    app->name[ASSOC_APP_NAME_MAX - 1] = '\0';
    
    strncpy(app->path, path, ASSOC_APP_PATH_MAX - 1);
    app->path[ASSOC_APP_PATH_MAX - 1] = '\0';
    
    strncpy(app->description, description ? description : "", ASSOC_APP_DESC_MAX - 1);
    app->description[ASSOC_APP_DESC_MAX - 1] = '\0';
    
    strncpy(app->icon_name, icon_name ? icon_name : "", 31);
    app->icon_name[31] = '\0';
    
    app->capabilities = capabilities;
    app->is_internal = is_internal;
    app->open_handler = open_handler;
    app->registered = true;
    
    g_app_count++;
    return true;
}

bool assoc_register_handler(const char *mime_type, const char *app_name, bool set_default) {
    if (!mime_type || !app_name) return false;
    
    assoc_entry_t *entry = NULL;
    for (int i = 0; i < g_entry_count; i++) {
        if (assoc_stricmp(g_entries[i].mime_type, mime_type) == 0) {
            entry = &g_entries[i];
            break;
        }
    }
    
    if (!entry) {
        if (g_entry_count >= ASSOC_MAX_ENTRIES) {
            return false;
        }
        entry = &g_entries[g_entry_count++];
        strncpy(entry->mime_type, mime_type, MIME_TYPE_MAX_LEN - 1);
        entry->mime_type[MIME_TYPE_MAX_LEN - 1] = '\0';
        entry->app_count = 0;
        entry->registered = true;
    }
    
    bool found = false;
    for (int i = 0; i < entry->app_count; i++) {
        if (assoc_stricmp(entry->apps[i], app_name) == 0) {
            found = true;
            break;
        }
    }
    
    if (!found && entry->app_count < ASSOC_MAX_APPS) {
        strncpy(entry->apps[entry->app_count], app_name, ASSOC_APP_NAME_MAX - 1);
        entry->apps[entry->app_count][ASSOC_APP_NAME_MAX - 1] = '\0';
        entry->app_count++;
    }
    
    if (set_default) {
        strncpy(entry->default_app, app_name, ASSOC_APP_NAME_MAX - 1);
        entry->default_app[ASSOC_APP_NAME_MAX - 1] = '\0';
    }
    
    return true;
}

// ============================================================================
// Lookup Functions
// ============================================================================

assoc_app_t *assoc_get_app(const char *name) {
    if (!name) return NULL;
    
    for (int i = 0; i < g_app_count; i++) {
        if (g_apps[i].registered && assoc_stricmp(g_apps[i].name, name) == 0) {
            return &g_apps[i];
        }
    }
    
    return NULL;
}

assoc_entry_t *assoc_get_entry(const char *mime_type) {
    if (!mime_type) return NULL;
    
    for (int i = 0; i < g_entry_count; i++) {
        if (g_entries[i].registered && 
            assoc_stricmp(g_entries[i].mime_type, mime_type) == 0) {
            return &g_entries[i];
        }
    }
    
    return NULL;
}

assoc_app_t *assoc_get_default(const char *mime_type) {
    assoc_entry_t *entry = assoc_get_entry(mime_type);
    if (entry && entry->default_app[0]) {
        return assoc_get_app(entry->default_app);
    }
    return NULL;
}

assoc_app_t *assoc_get_default_for_file(const char *filepath) {
    const char *ext = get_extension(filepath);
    if (!ext) return NULL;
    
    const char *mime_type = mime_type_from_extension(ext);
    return assoc_get_default(mime_type);
}

bool assoc_set_default(const char *mime_type, const char *app_name) {
    if (!assoc_get_app(app_name)) {
        return false;
    }
    
    assoc_entry_t *entry = assoc_get_entry(mime_type);
    if (!entry) {
        if (g_entry_count >= ASSOC_MAX_ENTRIES) {
            return false;
        }
        entry = &g_entries[g_entry_count++];
        strncpy(entry->mime_type, mime_type, MIME_TYPE_MAX_LEN - 1);
        entry->mime_type[MIME_TYPE_MAX_LEN - 1] = '\0';
        entry->app_count = 0;
        entry->registered = true;
    }
    
    strncpy(entry->default_app, app_name, ASSOC_APP_NAME_MAX - 1);
    entry->default_app[ASSOC_APP_NAME_MAX - 1] = '\0';
    
    bool found = false;
    for (int i = 0; i < entry->app_count; i++) {
        if (assoc_stricmp(entry->apps[i], app_name) == 0) {
            found = true;
            break;
        }
    }
    if (!found && entry->app_count < ASSOC_MAX_APPS) {
        strncpy(entry->apps[entry->app_count], app_name, ASSOC_APP_NAME_MAX - 1);
        entry->apps[entry->app_count][ASSOC_APP_NAME_MAX - 1] = '\0';
        entry->app_count++;
    }
    
    return true;
}

int assoc_get_all(const char *mime_type, assoc_app_t **apps) {
    assoc_entry_t *entry = assoc_get_entry(mime_type);
    if (!entry || !apps) return 0;
    
    int count = 0;
    for (int i = 0; i < entry->app_count && count < ASSOC_MAX_APPS; i++) {
        assoc_app_t *app = assoc_get_app(entry->apps[i]);
        if (app) {
            apps[count++] = app;
        }
    }
    
    return count;
}

int assoc_get_all_for_file(const char *filepath, assoc_app_t **apps) {
    const char *ext = get_extension(filepath);
    if (!ext) return 0;
    
    const char *mime_type = mime_type_from_extension(ext);
    return assoc_get_all(mime_type, apps);
}

assoc_app_t *assoc_get_all_apps(int *count) {
    if (count) *count = g_app_count;
    return g_apps;
}

assoc_entry_t *assoc_get_all_entries(int *count) {
    if (count) *count = g_entry_count;
    return g_entries;
}

// ============================================================================
// File Opening Functions
// ============================================================================

bool assoc_open_file(const char *filepath) {
    assoc_app_t *app = assoc_get_default_for_file(filepath);
    if (!app) {
        kprintf("[Associations] No default app for: %s\n", filepath);
        return false;
    }
    
    return assoc_open_with(filepath, app->name);
}

bool assoc_open_with(const char *filepath, const char *app_name) {
    assoc_app_t *app = assoc_get_app(app_name);
    if (!app) {
        kprintf("[Associations] App not found: %s\n", app_name);
        return false;
    }
    
    kprintf("[Associations] Opening '%s' with '%s'\n", filepath, app_name);
    
    if (app->is_internal && app->open_handler) {
        app->open_handler(filepath);
        return true;
    }
    
    return false;
}
