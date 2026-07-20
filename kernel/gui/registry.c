// registry.c - System Registry for MayteraOS
// Centralized storage for file associations, application menus, and system settings

#include "registry.h"
#include "icons.h"
#include "../serial.h"
#include "../string.h"

// Forward declarations for app launchers (defined in their respective files)
extern void terminal_launch(void);
extern void calculator_launch(void);
extern void filebrowser_launch(void);
extern void editor_launch(void);
extern void settings_launch(void);
extern void paint_launch(void);
extern void imageviewer_launch(const char *path);
extern void mediaplayer_launch(const char *path);
extern void taskmanager_launch(void);
extern void recyclebin_launch(void);
extern void syslog_viewer_launch(void);
extern void netsettings_launch(void);
extern void irc_launch(void);
extern void pong_launch(void);
extern void solitaire_launch(void);
extern void doom_launch(void);  // DOOM - MayteraOS port
extern void browser_launch(void);  // Browser - userland web browser

// ============================================================================
// Storage
// ============================================================================

static file_assoc_t g_file_assocs[REG_MAX_EXTENSIONS];
static int g_file_assoc_count = 0;

static menu_category_t g_menu_categories[REG_MAX_MENU_CATEGORIES];
static int g_menu_category_count = 0;

static bool g_registry_initialized = false;

// ============================================================================
// Helper Functions
// ============================================================================

// Case-insensitive string comparison
static int stricmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return *a - *b;
}

// Extract extension from filename
static const char *get_extension(const char *filename) {
    const char *dot = NULL;
    for (const char *p = filename; *p; p++) {
        if (*p == '.') dot = p;
        if (*p == '/') dot = NULL;  // Reset on path separator
    }
    return dot ? dot : "";
}

// ============================================================================
// Initialization
// ============================================================================

void registry_init(void) {
    if (g_registry_initialized) return;

    memset(g_file_assocs, 0, sizeof(g_file_assocs));
    memset(g_menu_categories, 0, sizeof(g_menu_categories));
    g_file_assoc_count = 0;
    g_menu_category_count = 0;

    // Register all defaults
    registry_register_defaults();

    g_registry_initialized = true;
    kprintf("[Registry] Initialized with %d file associations, %d menu categories\n",
            g_file_assoc_count, g_menu_category_count);
}

// ============================================================================
// File Association API
// ============================================================================

bool registry_register_file_assoc(const char *extension,
                                   const char *mime_type,
                                   const char *app_name,
                                   void (*open_handler)(const char *path),
                                   file_category_t category,
                                   uint32_t icon_id) {
    if (g_file_assoc_count >= REG_MAX_EXTENSIONS) {
        return false;
    }

    file_assoc_t *assoc = &g_file_assocs[g_file_assoc_count];

    strncpy(assoc->extension, extension, REG_MAX_EXT_LEN - 1);
    assoc->extension[REG_MAX_EXT_LEN - 1] = '\0';

    strncpy(assoc->mime_type, mime_type, REG_MAX_MIME_TYPE - 1);
    assoc->mime_type[REG_MAX_MIME_TYPE - 1] = '\0';

    strncpy(assoc->app_name, app_name, REG_MAX_APP_NAME - 1);
    assoc->app_name[REG_MAX_APP_NAME - 1] = '\0';

    assoc->open_handler = open_handler;
    assoc->category = category;
    assoc->icon_id = icon_id;
    assoc->registered = true;

    g_file_assoc_count++;
    return true;
}

file_assoc_t *registry_get_file_assoc(const char *extension) {
    for (int i = 0; i < g_file_assoc_count; i++) {
        if (stricmp(g_file_assocs[i].extension, extension) == 0) {
            return &g_file_assocs[i];
        }
    }
    return NULL;
}

file_assoc_t *registry_get_file_assoc_by_name(const char *filename) {
    const char *ext = get_extension(filename);
    if (!ext || !*ext) return NULL;
    return registry_get_file_assoc(ext);
}

bool registry_open_file(const char *path) {
    file_assoc_t *assoc = registry_get_file_assoc_by_name(path);
    if (assoc && assoc->open_handler) {
        assoc->open_handler(path);
        return true;
    }
    return false;
}

uint32_t registry_get_file_icon(const char *filename) {
    file_assoc_t *assoc = registry_get_file_assoc_by_name(filename);
    if (assoc) {
        return assoc->icon_id;
    }
    // Default icon based on whether it looks like a directory
    return ICON_FILE;
}

file_category_t registry_get_file_category(const char *filename) {
    file_assoc_t *assoc = registry_get_file_assoc_by_name(filename);
    if (assoc) {
        return assoc->category;
    }
    return FILE_CAT_UNKNOWN;
}

// ============================================================================
// Menu API
// ============================================================================

bool registry_register_menu_category(const char *category_name) {
    if (g_menu_category_count >= REG_MAX_MENU_CATEGORIES) {
        return false;
    }

    // Check if already exists
    for (int i = 0; i < g_menu_category_count; i++) {
        if (strcmp(g_menu_categories[i].name, category_name) == 0) {
            return true;  // Already registered
        }
    }

    menu_category_t *cat = &g_menu_categories[g_menu_category_count];
    strncpy(cat->name, category_name, REG_MAX_CATEGORY_NAME - 1);
    cat->name[REG_MAX_CATEGORY_NAME - 1] = '\0';
    cat->item_count = 0;
    cat->registered = true;

    g_menu_category_count++;
    return true;
}

bool registry_register_app(const char *category,
                           const char *app_name,
                           uint32_t icon_id,
                           void (*launch_handler)(void),
                           bool separator_after) {
    // Find category
    menu_category_t *cat = NULL;
    for (int i = 0; i < g_menu_category_count; i++) {
        if (strcmp(g_menu_categories[i].name, category) == 0) {
            cat = &g_menu_categories[i];
            break;
        }
    }

    if (!cat) {
        // Create category if it doesn't exist
        if (!registry_register_menu_category(category)) {
            return false;
        }
        cat = &g_menu_categories[g_menu_category_count - 1];
    }

    if (cat->item_count >= REG_MAX_MENU_ITEMS) {
        return false;
    }

    menu_item_reg_t *item = &cat->items[cat->item_count];
    strncpy(item->name, app_name, REG_MAX_APP_NAME - 1);
    item->name[REG_MAX_APP_NAME - 1] = '\0';
    item->icon_id = icon_id;
    item->launch_handler = launch_handler;
    item->separator_after = separator_after;
    item->registered = true;

    cat->item_count++;
    return true;
}

int registry_get_menu_categories(menu_category_t **categories) {
    *categories = g_menu_categories;
    return g_menu_category_count;
}

int registry_get_menu_items(const char *category, menu_item_reg_t **items) {
    for (int i = 0; i < g_menu_category_count; i++) {
        if (strcmp(g_menu_categories[i].name, category) == 0) {
            *items = g_menu_categories[i].items;
            return g_menu_categories[i].item_count;
        }
    }
    *items = NULL;
    return 0;
}

int registry_get_all_apps(menu_item_reg_t **items, int max_items) {
    static menu_item_reg_t all_items[REG_MAX_MENU_ITEMS * REG_MAX_MENU_CATEGORIES];
    int count = 0;

    for (int i = 0; i < g_menu_category_count && count < max_items; i++) {
        for (int j = 0; j < g_menu_categories[i].item_count && count < max_items; j++) {
            all_items[count++] = g_menu_categories[i].items[j];
        }
    }

    *items = all_items;
    return count;
}

// ============================================================================
// Default Registrations
// ============================================================================

// Wrapper for imageviewer (takes path)
static void open_image(const char *path) {
    imageviewer_launch(path);
}

// Wrapper for audio files (routed to the Media Player, which plays audio;
// the Audio Player app was removed in favor of the Maytera HiFi / Music Player)
static void open_audio(const char *path) {
    mediaplayer_launch(path);
}

// Wrapper for editor (opens file)
extern void editor_open_file(const char *path);
static void open_text(const char *path) {
    // Launch editor and open file
    editor_launch();
    // TODO: Actually open the file in the editor
    (void)path;
}

void registry_register_defaults(void) {
    // ========================================================================
    // File Associations
    // ========================================================================

    // Text files
    registry_register_file_assoc(".txt", "text/plain", "Editor", open_text, FILE_CAT_TEXT, ICON_FILE_TEXT);
    registry_register_file_assoc(".c", "text/x-c", "Editor", open_text, FILE_CAT_TEXT, ICON_FILE_CODE);
    registry_register_file_assoc(".h", "text/x-c", "Editor", open_text, FILE_CAT_TEXT, ICON_FILE_CODE);
    registry_register_file_assoc(".asm", "text/x-asm", "Editor", open_text, FILE_CAT_TEXT, ICON_FILE_CODE);
    registry_register_file_assoc(".md", "text/markdown", "Editor", open_text, FILE_CAT_TEXT, ICON_FILE_TEXT);
    registry_register_file_assoc(".cfg", "text/plain", "Editor", open_text, FILE_CAT_TEXT, ICON_FILE_CONFIG);
    registry_register_file_assoc(".ini", "text/plain", "Editor", open_text, FILE_CAT_TEXT, ICON_FILE_CONFIG);
    registry_register_file_assoc(".log", "text/plain", "Editor", open_text, FILE_CAT_TEXT, ICON_FILE_TEXT);

    // Image files
    registry_register_file_assoc(".bmp", "image/bmp", "Image Viewer", open_image, FILE_CAT_IMAGE, ICON_FILE_IMAGE);
    registry_register_file_assoc(".jpg", "image/jpeg", "Image Viewer", open_image, FILE_CAT_IMAGE, ICON_FILE_IMAGE);
    registry_register_file_assoc(".jpeg", "image/jpeg", "Image Viewer", open_image, FILE_CAT_IMAGE, ICON_FILE_IMAGE);
    registry_register_file_assoc(".png", "image/png", "Image Viewer", open_image, FILE_CAT_IMAGE, ICON_FILE_IMAGE);
    registry_register_file_assoc(".gif", "image/gif", "Image Viewer", open_image, FILE_CAT_IMAGE, ICON_FILE_IMAGE);

    // Audio files
    registry_register_file_assoc(".wav", "audio/wav", "Media Player", open_audio, FILE_CAT_AUDIO, ICON_FILE_AUDIO);
    registry_register_file_assoc(".mp3", "audio/mpeg", "Media Player", open_audio, FILE_CAT_AUDIO, ICON_FILE_AUDIO);

    // Executables
    registry_register_file_assoc(".elf", "application/x-elf", "System", NULL, FILE_CAT_EXECUTABLE, ICON_FILE_EXEC);
    registry_register_file_assoc(".exe", "application/x-msdos-program", "System", NULL, FILE_CAT_EXECUTABLE, ICON_FILE_EXEC);
    registry_register_file_assoc(".com", "application/x-msdos-program", "System", NULL, FILE_CAT_EXECUTABLE, ICON_FILE_EXEC);

    // Archives
    registry_register_file_assoc(".zip", "application/zip", "Archive", NULL, FILE_CAT_ARCHIVE, ICON_FILE_ARCHIVE);
    registry_register_file_assoc(".tar", "application/x-tar", "Archive", NULL, FILE_CAT_ARCHIVE, ICON_FILE_ARCHIVE);
    registry_register_file_assoc(".gz", "application/gzip", "Archive", NULL, FILE_CAT_ARCHIVE, ICON_FILE_ARCHIVE);

    // ========================================================================
    // Application Menu Categories and Apps
    // ========================================================================

    // Applications category
    registry_register_menu_category("Applications");
    registry_register_app("Applications", "Terminal", ICON_APP_TERMINAL, terminal_launch, false);
    registry_register_app("Applications", "Files", ICON_APP_FILES, filebrowser_launch, false);
    registry_register_app("Applications", "Editor", ICON_APP_EDITOR, editor_launch, false);
    registry_register_app("Applications", "Calculator", ICON_APP_CALCULATOR, calculator_launch, false);
    registry_register_app("Applications", "Paint", ICON_APP_PAINT, paint_launch, false);
    registry_register_app("Applications", "Image Viewer", ICON_APP_IMAGE, (void(*)(void))imageviewer_launch, false);
    registry_register_app("Applications", "IRC Client", ICON_APP_IRC, irc_launch, false);
    registry_register_app("Applications", "Browser", ICON_NETWORK, browser_launch, false);

    // Games category
    registry_register_menu_category("Games");
    registry_register_app("Games", "DOOM", ICON_GAME_PONG, doom_launch, false);  // Using Pong icon for now
    registry_register_app("Games", "Solitaire", ICON_GAME_SOLITAIRE, solitaire_launch, false);
    registry_register_app("Games", "Pong", ICON_GAME_PONG, pong_launch, false);

    // System category
    registry_register_menu_category("System");
    registry_register_app("System", "Task Manager", ICON_SYS_TASKMAN, taskmanager_launch, false);
    registry_register_app("System", "System Log", ICON_SYS_LOG, syslog_viewer_launch, false);
    registry_register_app("System", "Recycle Bin", ICON_SYS_RECYCLE, recyclebin_launch, true);
    registry_register_app("System", "Network Settings", ICON_SYS_NETWORK, netsettings_launch, false);
    registry_register_app("System", "Settings", ICON_SYS_SETTINGS, settings_launch, false);
}
