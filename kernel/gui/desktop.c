// desktop.c - Desktop manager and dock implementation for MayteraOS
#include "desktop.h"
#include "window.h"
#include "image.h"
#include "icons.h"
#include "themes.h"
#include "ttf.h"
#include "background.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../drivers/mouse.h"
#include "../cpu/isr.h"
#include "../fs/fat.h"
#include "../net/net.h"
#include "../version.h"
#include "terminal.h"
#include "calculator.h"
#include "settings.h"
#include "pong.h"
#include "clock.h"
#include "irc.h"
#include "imageviewer.h"
#include "paint.h"
#include "taskmanager.h"
#include "recyclebin.h"
#include "syslog.h"
#include "screensaver.h"
#include "netsettings.h"
#include "solitaire.h"
#include "mediaplayer.h"
#include "lemmings.h"
#include "browser.h"
// Forward declarations for app launchers
extern void editor_launch(void);
extern void filebrowser_launch(void);
extern void settings_launch(void);
extern void solitaire_launch(void);
#include "../exec/elf.h"
#include "../proc/process.h"

// External timer ticks from ISR
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;  // Current timer frequency
static uint64_t s_last_kb_tick = 0;  // Shared 20 Hz keyboard throttle

// CPU usage tracking
static volatile uint64_t g_cpu_busy_ticks = 0;      // Ticks spent doing work
static volatile uint64_t g_cpu_total_ticks = 0;     // Total ticks for measurement period
static volatile uint64_t g_cpu_last_tick = 0;       // Last tick count
static volatile int g_cpu_usage_percent = 0;        // Current CPU usage percentage

// Calculate CPU usage percentage (call periodically)
static void cpu_update_usage(void) {
    uint64_t current_tick = timer_ticks;
    uint64_t elapsed = current_tick - g_cpu_last_tick;

    // Update every ~18 ticks (~1 second at 18.2 Hz timer)
    if (elapsed >= g_timer_hz) {
        if (g_cpu_total_ticks > 0) {
            g_cpu_usage_percent = (g_cpu_busy_ticks * 100) / g_cpu_total_ticks;
            if (g_cpu_usage_percent > 100) g_cpu_usage_percent = 100;
        }
        // Reset counters for next period
        g_cpu_busy_ticks = 0;
        g_cpu_total_ticks = 0;
        g_cpu_last_tick = current_tick;
    }
}

// Get current CPU usage percentage
static int cpu_get_usage(void) {
    return g_cpu_usage_percent;
}

// External filesystem from main
extern fat_fs_t g_fat_fs;

// Session user identity (set by login screen before desktop_run)
static uint32_t g_session_uid = 0;
static uint32_t g_session_gid = 0;

void desktop_set_session(uint32_t uid, uint32_t gid) {
    g_session_uid = uid;
    g_session_gid = gid;
    kprintf("[DESKTOP] Session UID=%u, GID=%u\n", uid, gid);
}

uint32_t desktop_get_session_uid(void) {
    return g_session_uid;
}

uint32_t desktop_get_session_gid(void) {
    return g_session_gid;
}

// ============================================================================
// User-space application launcher
// ============================================================================

// Launch a user-space ELF application from filesystem
static int launch_userspace_app(const char *path) {
    if (!g_fat_fs.mounted) {
        LOG_ERROR("[UserSpace] Filesystem not mounted");
        return -1;
    }

    // Read ELF file from disk
    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, path, &size);
    if (!data || size == 0) {
        LOG_ERROR("[UserSpace] Cannot read file from disk");
        return -1;
    }

    // Validate ELF format
    int elf_err = elf_validate(data, size);
    if (elf_err != ELF_SUCCESS) {
        LOG_ERROR("[UserSpace] Invalid ELF format");
        kfree(data);
        return -1;
    }

    // Extract filename for process name
    const char *name = path;
    const char *p = path;
    while (*p) {
        if (*p == '/') name = p + 1;
        p++;
    }

    // Create user-space process
    int pid = proc_create_user(name, data, size, NULL, NULL);
    if (pid > 0) {
        // Apply session UID/GID to launched process
        extern process_t *proc_get(uint32_t pid);
        process_t *child = proc_get(pid);
        if (child) {
            child->uid  = g_session_uid;
            child->gid  = g_session_gid;
            child->euid = g_session_uid;
            child->egid = g_session_gid;
        }
        LOG_INFO("[UserSpace] Process created successfully");
    } else {
        LOG_ERROR("[UserSpace] proc_create_user failed");
    }

    kfree(data);
    return pid;
}

// User-space Calculator launcher
static void userspace_calc_launch(void) {
    LOG_INFO("[Calculator] Loading from /apps/calc (user-space)");
    int pid = launch_userspace_app("/apps/calc");
    if (pid > 0) {
        LOG_INFO("[Calculator] User-space process started successfully");
    } else {
        // Fallback to kernel calculator if user-space fails
        LOG_INFO("[Calculator] User-space failed - using kernel version");
        calculator_launch();
    }
}

// User-space Test launcher (Debug app with extensive logging)
static void userspace_test_launch(void) {
    kprintf("[TEST] Loading from /apps/test (user-space debug app)\n");
    int pid = launch_userspace_app("/apps/test");
    if (pid > 0) {
        kprintf("[TEST] User-space process started successfully, PID=%d\n", pid);
    } else {
        kprintf("[TEST] Failed to launch user-space test app, PID=%d\n", pid);
    }
}


// User-space Terminal launcher
static void userspace_terminal_launch(void) {
    LOG_INFO("[Terminal] Loading from /apps/terminal (user-space)");
    int pid = launch_userspace_app("/apps/terminal");
    if (pid > 0) {
        LOG_INFO("[Terminal] User-space process started successfully");
    } else {
        LOG_INFO("[Terminal] User-space failed - using kernel version");
        terminal_launch();
    }
}

// User-space Files launcher
static void userspace_files_launch(void) {
    LOG_INFO("[Files] Loading from /apps/files (user-space)");
    int pid = launch_userspace_app("/apps/files");
    if (pid > 0) {
        LOG_INFO("[Files] User-space process started successfully");
    } else {
        LOG_INFO("[Files] User-space failed - using kernel version");
        filebrowser_launch();
    }
}

// User-space Editor launcher
static void userspace_editor_launch(void) {
    LOG_INFO("[Editor] Loading from /apps/editor (user-space)");
    int pid = launch_userspace_app("/apps/editor");
    if (pid > 0) {
        LOG_INFO("[Editor] User-space process started successfully");
    } else {
        LOG_INFO("[Editor] User-space failed - using kernel version");
        editor_launch();
    }
}

// User-space Settings launcher
static void userspace_settings_launch(void) {
    LOG_INFO("[Settings] Loading from /apps/settings (user-space)");
    int pid = launch_userspace_app("/apps/settings");
    if (pid > 0) {
        LOG_INFO("[Settings] User-space process started successfully");
    } else {
        LOG_INFO("[Settings] User-space failed - using kernel version");
        settings_launch();
    }
}

// User-space App Store launcher (#402)
static void userspace_appstore_launch(void) {
    LOG_INFO("[AppStore] Loading from /apps/appstore (user-space)");
    launch_userspace_app("/apps/appstore");
}

// User-space Solitaire launcher
static void userspace_solitaire_launch(void) {
    LOG_INFO("[Solitaire] Loading from /apps/solitr (user-space)");
    int pid = launch_userspace_app("/apps/solitr");
    if (pid > 0) {
        LOG_INFO("[Solitaire] User-space process started successfully");
    } else {
        LOG_INFO("[Solitaire] User-space failed - using kernel version");
        solitaire_launch();
    }
}

// User-space Python launcher
__attribute__((unused)) static void userspace_python_launch(void) {
    LOG_INFO("[Python] Loading from /apps/python (user-space)");
    int pid = launch_userspace_app("/apps/python");
    if (pid > 0) {
        LOG_INFO("[Python] User-space process started successfully");
    } else {
        LOG_INFO("[Python] User-space failed");
    }
}

// User-space System Log launcher
static void userspace_syslog_launch(void) {
    LOG_INFO("[SysLog] Loading from /apps/syslog (user-space)");
    int pid = launch_userspace_app("/apps/syslog");
    if (pid > 0) {
        LOG_INFO("[SysLog] User-space process started successfully");
    } else {
        LOG_INFO("[SysLog] User-space failed - using kernel version");
        syslog_viewer_launch();
    }
}

// #447: in-kernel Cybersecurity app (BadUSB / USB threat scan + keystroke monitor)
extern void cybersecurity_launch(void);
// Global desktop state

// Forward declarations for kernel-mode app launchers used as fallbacks
extern void browser_launch(void);
extern void browser_launch_kernel(void);
extern void doom_launch(void);
extern void lemmings_launch(void);
extern void pong_launch(void);
extern void netsettings_launch(void);
extern void taskmanager_launch(void);
extern void recyclebin_launch(void);
extern void audio_config_launch(void);
extern void nethack_launch(void);
extern void imageviewer_launch(const char *path);
extern void paint_launch(const char *path);
static void imageviewer_launch_default(void);
static void paint_launch_default(void);

// Browser launcher - uses kernel browser directly (userland stub has no networking)
static void userspace_browser_launch(void) {
    browser_launch_kernel();
}

// User-space Paint launcher
static void userspace_paint_launch(void) {
    LOG_INFO("[Paint] Loading from /apps/paint");
    int pid = launch_userspace_app("/apps/paint");
    if (pid > 0) {
        LOG_INFO("[Paint] User-space process started successfully");
    } else {
        LOG_INFO("[Paint] User-space failed, using kernel version");
        paint_launch_default();
    }
}

// User-space Image Viewer launcher
static void userspace_imageviewer_launch(void) {
    LOG_INFO("[ImageViewer] Loading from /apps/imgview");
    int pid = launch_userspace_app("/apps/imgview");
    if (pid > 0) {
        LOG_INFO("[ImageViewer] User-space process started successfully");
    } else {
        LOG_INFO("[ImageViewer] User-space failed, using kernel version");
        imageviewer_launch_default();
    }
}

// User-space Media Player launcher
static void userspace_mediaplayer_launch(void) {
    LOG_INFO("[MediaPlayer] Loading from /apps/mplayer");
    int pid = launch_userspace_app("/apps/mplayer");
    if (pid > 0) {
        LOG_INFO("[MediaPlayer] User-space process started successfully");
    } else {
        LOG_INFO("[MediaPlayer] User-space failed, using kernel version");
        mediaplayer_launch_simple();
    }
}

// User-space Clock launcher
static void userspace_clock_launch(void) {
    LOG_INFO("[Clock] Loading from /apps/clock");
    int pid = launch_userspace_app("/apps/clock");
    if (pid > 0) {
        LOG_INFO("[Clock] User-space process started successfully");
    } else {
        LOG_INFO("[Clock] User-space failed, using kernel version");
        clock_launch();
    }
}

// User-space IRC launcher
static void userspace_irc_launch(void) {
    LOG_INFO("[IRC] Loading from /apps/irc");
    int pid = launch_userspace_app("/apps/irc");
    if (pid > 0) {
        LOG_INFO("[IRC] User-space process started successfully");
    } else {
        LOG_INFO("[IRC] User-space failed, using kernel version");
        irc_launch();
    }
}

// User-space DOOM launcher
static void userspace_doom_launch(void) {
    LOG_INFO("[DOOM] Loading from /apps/DOOM.ELF");
    int pid = launch_userspace_app("/apps/DOOM.ELF");
    if (pid > 0) {
        LOG_INFO("[DOOM] User-space process started successfully");
    } else {
        LOG_INFO("[DOOM] User-space failed, using kernel version");
        doom_launch();
    }
}

// User-space Lemmings launcher
static void userspace_lemmings_launch(void) {
    LOG_INFO("[Lemmings] Loading from /apps/lemmings");
    int pid = launch_userspace_app("/apps/lemmings");
    if (pid > 0) {
        LOG_INFO("[Lemmings] User-space process started successfully");
    } else {
        LOG_INFO("[Lemmings] User-space failed, using kernel version");
        lemmings_launch();
    }
}

// User-space Pong launcher
static void userspace_pong_launch(void) {
    LOG_INFO("[Pong] Loading from /apps/pong");
    int pid = launch_userspace_app("/apps/pong");
    if (pid > 0) {
        LOG_INFO("[Pong] User-space process started successfully");
    } else {
        LOG_INFO("[Pong] User-space failed, using kernel version");
        pong_launch();
    }
}

// User-space Network launcher
static void userspace_network_launch(void) {
    LOG_INFO("[Network] Loading from /apps/network");
    int pid = launch_userspace_app("/apps/network");
    if (pid > 0) {
        LOG_INFO("[Network] User-space process started successfully");
    } else {
        LOG_INFO("[Network] User-space failed, using kernel version");
        netsettings_launch();
    }
}

// User-space Task Manager launcher
static void userspace_taskmanager_launch(void) {
    LOG_INFO("[TaskManager] Loading from /apps/taskmgr");
    int pid = launch_userspace_app("/apps/taskmgr");
    if (pid > 0) {
        LOG_INFO("[TaskManager] User-space process started successfully");
    } else {
        LOG_INFO("[TaskManager] User-space failed, using kernel version");
        taskmanager_launch();
    }
}

// User-space Recycle Bin launcher
static void userspace_recyclebin_launch(void) {
    LOG_INFO("[RecycleBin] Loading from /apps/recycle");
    int pid = launch_userspace_app("/apps/recycle");
    if (pid > 0) {
        LOG_INFO("[RecycleBin] User-space process started successfully");
    } else {
        LOG_INFO("[RecycleBin] User-space failed, using kernel version");
        recyclebin_launch();
    }
}

// User-space Audio Config launcher
static void userspace_audioconfig_launch(void) {
    LOG_INFO("[AudioConfig] Loading from /apps/aconfig");
    int pid = launch_userspace_app("/apps/aconfig");
    if (pid > 0) {
        LOG_INFO("[AudioConfig] User-space process started successfully");
    } else {
        LOG_INFO("[AudioConfig] User-space failed, using kernel version");
        audio_config_launch();
    }
}

// User-space Screensaver launcher
__attribute__((unused)) static void userspace_screensaver_launch(void) {
    LOG_INFO("[Screensaver] Loading from /apps/ssaver");
    int pid = launch_userspace_app("/apps/ssaver");
    if (pid > 0) {
        LOG_INFO("[Screensaver] User-space process started successfully");
    }
}

static desktop_t g_desktop;

// Background image storage
static image_t g_bg_image_data;

// Start menu state
static bool g_start_menu_open = false;  // DEBUG: Auto-open start menu

// ============================================================================
// Enhanced Start Menu - Recent Files, View Modes, Sorting, Submenus
// ============================================================================

// Recent files tracking
#define MAX_RECENT_FILES 10
#define RECENT_FILE_PATH_LEN 256
#define RECENT_FILE_NAME_LEN 64

typedef struct {
    char path[RECENT_FILE_PATH_LEN];
    char name[RECENT_FILE_NAME_LEN];
    uint64_t timestamp;         // Timer ticks when accessed
    uint32_t access_count;      // Frequency counter
} recent_file_t;

static recent_file_t g_recent_files[MAX_RECENT_FILES];
static int g_recent_file_count = 0;

// View modes for start menu
typedef enum {
    VIEW_GRID,      // Icons in grid (4 columns)
    VIEW_LIST,      // Text list with icons
    VIEW_COMPACT    // Small icons, more items per row
} menu_view_t;

// Sorting modes
typedef enum {
    SORT_NAME,      // Alphabetical
    SORT_DATE,      // Most recent first
    SORT_FREQUENCY  // Most used first
} menu_sort_t;

// Menu configuration (persisted to /CONFIG/MENU.CFG)
typedef struct {
    int height;             // Adjustable height (300-600)
    menu_view_t view;       // Current view mode
    menu_sort_t sort;       // Current sort mode
    bool show_recent;       // Show recent files section
    bool show_search;       // Show search bar
    bool categories_expanded[8];  // Which categories are expanded
} menu_config_t;

static menu_config_t g_menu_config = {
    .height = 450,
    .view = VIEW_LIST,
    .sort = SORT_NAME,
    .show_recent = true,
    .show_search = true,
    .categories_expanded = {true, true, true, false, true, false, false, false}
};

// Search state
#define MENU_SEARCH_MAX 32
static char g_menu_search[MENU_SEARCH_MAX] = {0};
static int g_menu_search_len = 0;
static bool g_menu_search_focused = false;

// Submenu state (for cascading menus)
static int g_submenu_category = -1;   // Which category has open submenu
static int g_expanded_category = 0;   // Which category is expanded (0=Apps, 1=Games, 2=System)
static bool g_category_header_clicked = false;  // True if click was on a category header
static int g_submenu_x = 0;
static int g_submenu_y = 0;

// Hover tracking for visual feedback
static int g_menu_hover_category = -1;
static int g_menu_hover_item = -1;
static int g_menu_hover_recent = -1;

// Menu item definition with icon support
typedef struct {
    const char *name;
    int icon_id;
    void (*action)(void);
    const char *userspace_path;  // If set, launch this ELF from disk
} menu_item_t;

// Menu category definition
typedef struct {
    const char *name;
    int icon_id;
    menu_item_t *items;
    int item_count;
    bool is_separator;
} menu_category_t;

// Forward declarations for app launchers (defined in respective app files)


// Wrapper for imageviewer_launch (takes filepath param, but we want no-arg version)
static void imageviewer_launch_default(void) {
    imageviewer_launch(NULL);
}

// Wrapper for paint_launch
static void paint_launch_default(void) {
    paint_launch(NULL);
}

// Forward declarations for power options
static void do_shutdown(void);
static void do_restart(void);

// task #306: launch the graphical install-to-disk wizard (kernel GUI app).
static void installer_launch_action(void) {
    extern void installer_run(void);
    installer_run();
}

// Application menu items
static menu_item_t g_apps_items[] = {
    {"Terminal",     ICON_TERMINAL,    userspace_terminal_launch, NULL},

    {"Browser",      ICON_NETWORK,     userspace_browser_launch, NULL},

    {"Files",        ICON_FOLDER,      userspace_files_launch, NULL},

    {"Editor",       ICON_HIGHLIGHT,   userspace_editor_launch, NULL},

    {"Calculator",   ICON_CALCULATOR,  userspace_calc_launch, NULL},

    {"Paint",        ICON_PAINT,       userspace_paint_launch, NULL},

    {"Image Viewer", ICON_IMAGE,       userspace_imageviewer_launch, NULL},

    {"Media Player", ICON_PLAY,        userspace_mediaplayer_launch, NULL},

    {"Clock",        ICON_CLOCK,       userspace_clock_launch, NULL},

    {"IRC",          ICON_NETWORK,     userspace_irc_launch, NULL},

    {"Python", ICON_TERMINAL, userspace_python_launch, NULL},

    {"Debug User-Space", ICON_TERMINAL, userspace_test_launch, NULL},

    {NULL, -1, NULL, NULL}

};

// Games menu items
static menu_item_t g_games_items[] = {
    {"DOOM",        ICON_GAME_DOOM,       userspace_doom_launch, NULL},

    {"Lemmings",    ICON_GAME_LEMMINGS,   userspace_lemmings_launch, NULL},

    {"Solitaire",   ICON_GAME_SOLITAIRE,  userspace_solitaire_launch, NULL},

    {"Pong",        ICON_GAME_PONG,       userspace_pong_launch, NULL},

    {"NetHack",     ICON_GAME,            nethack_launch, NULL},

    {NULL, -1, NULL, NULL}

};

// System menu items
static menu_item_t g_system_items[] = {
    {"Settings",     ICON_COG,           userspace_settings_launch, NULL},

    {"Network",      ICON_NETWORK,       userspace_network_launch, NULL},

    {"Task Manager", ICON_TASK_MANAGER,  userspace_taskmanager_launch, NULL},

    {"Recycle Bin",  ICON_TRASH,         userspace_recyclebin_launch, NULL},

    {"System Log",   ICON_LOG_VIEWER,    userspace_syslog_launch, NULL},

    {"Install MayteraOS", ICON_SAVE,    installer_launch_action, NULL},

    {"Wallpaper",    ICON_PALETTE,       NULL, NULL},  // Will open wallpaper picker
    {"Security",     ICON_COG,           cybersecurity_launch, NULL},
    {NULL, -1, NULL, NULL}

};

// Power menu items
static menu_item_t g_power_items[] = {
    {"Restart",     ICON_REFRESH,     do_restart, NULL},

    {"Shutdown",    ICON_POWER,       do_shutdown, NULL},

    {NULL, -1, NULL, NULL}

};

// Count items in array
static int count_menu_items(menu_item_t *items) {
    int count = 0;
    while (items[count].name) count++;
    return count;
}


#define MAX_MENU_CATEGORIES 10
static menu_category_t g_start_menu_categories[MAX_MENU_CATEGORIES] = {
    {"Applications", ICON_CATEGORIES, g_apps_items, 0, false},
    {"Games",        ICON_GAME,       g_games_items, 0, false},
    {"System",       ICON_COG,        g_system_items, 0, false},
    {NULL,           -1,              NULL, 0, true},  // Separator
    {"Power",        ICON_POWER,      g_power_items, 0, false},
    {NULL, -1, NULL, 0, false}  // Terminator
};

// Store clicked category and item for action execution
static int g_clicked_category = -1;
static int g_clicked_item = -1;

// Enhanced start menu dimensions
#define START_MENU_WIDTH        300     // Wider for grid view
#define START_MENU_MIN_HEIGHT   300     // Minimum height
#define START_MENU_MAX_HEIGHT   600     // Maximum height
#define START_MENU_ITEM_H       26      // Item height
#define START_MENU_CAT_H        28      // Category header height
#define START_MENU_PADDING      8       // Padding
#define START_MENU_SEP_H        12      // Separator height
#define START_MENU_SEARCH_H     32      // Search bar height
#define START_MENU_RECENT_H     24      // Recent file item height
#define START_MENU_GRID_COLS    5       // Columns in grid view
#define START_MENU_GRID_ITEM_W  52      // Grid item width
#define START_MENU_GRID_ITEM_H  72      // Grid item height (icon + label)
#define START_MENU_POWER_H      40      // Power buttons area height
#define SUBMENU_WIDTH           180     // Cascading submenu width
#define SUBMENU_ARROW_SIZE      8       // Arrow indicator size

// Forward declarations for start menu functions
static void draw_start_menu(void);
static int get_start_menu_item_at(int x, int y);
static bool is_start_button_click(int x, int y);
static void start_menu_add_recent(const char *path, const char *name);
static void start_menu_save_config(void);
static void start_menu_load_config(void);
static void __attribute__((unused)) start_menu_toggle_view(void);
static void start_menu_cycle_sort(void);
static void draw_start_menu_search(int x, int y, int width);
static void draw_start_menu_recent(int x, int *y, int width);
static void draw_start_menu_grid(int x, int *y, int width, menu_item_t *items, int count);
static void draw_start_menu_list(int x, int *y, int width, menu_item_t *items, int count);
// static void draw_cascading_submenu(int cat_index); // Unused - using accordion now


// ============================================================================
// STRTMENU.CFG - Config-driven start menu
// ============================================================================

#define CFG_MAX_ITEMS_PER_CAT 16
#define CFG_MAX_CATS 8
#define CFG_NAME_LEN 32
#define CFG_PATH_LEN 64

// Storage for dynamically loaded menu items from STRTMENU.CFG
static menu_item_t g_cfg_items[CFG_MAX_CATS][CFG_MAX_ITEMS_PER_CAT + 1];
static char g_cfg_names[CFG_MAX_CATS * CFG_MAX_ITEMS_PER_CAT][CFG_NAME_LEN];
static char g_cfg_paths[CFG_MAX_CATS * CFG_MAX_ITEMS_PER_CAT][CFG_PATH_LEN];
static char g_cfg_cat_names[CFG_MAX_CATS][CFG_NAME_LEN];
static int g_cfg_name_idx = 0;
static bool g_startmenu_cfg_loaded = false;

// Map icon name string to ICON_* constant
static int cfg_lookup_icon(const char *name) {
    if (!name || !name[0]) return ICON_FILE;
    if (strcmp(name, "terminal") == 0) return ICON_TERMINAL;
    if (strcmp(name, "network") == 0) return ICON_NETWORK;
    if (strcmp(name, "folder") == 0) return ICON_FOLDER;
    if (strcmp(name, "highlight") == 0) return ICON_HIGHLIGHT;
    if (strcmp(name, "calculator") == 0) return ICON_CALCULATOR;
    if (strcmp(name, "paint") == 0) return ICON_PAINT;
    if (strcmp(name, "image") == 0) return ICON_IMAGE;
    if (strcmp(name, "music") == 0) return ICON_MUSIC;
    if (strcmp(name, "play") == 0) return ICON_PLAY;
    if (strcmp(name, "clock") == 0) return ICON_CLOCK;
    if (strcmp(name, "cog") == 0) return ICON_COG;
    if (strcmp(name, "game_doom") == 0) return ICON_GAME_DOOM;
    if (strcmp(name, "game_solitaire") == 0) return ICON_GAME_SOLITAIRE;
    if (strcmp(name, "game_pong") == 0) return ICON_GAME_PONG;
    if (strcmp(name, "game_lemmings") == 0) return ICON_GAME_LEMMINGS;
    if (strcmp(name, "game") == 0) return ICON_GAME;
    if (strcmp(name, "power") == 0) return ICON_POWER;
    if (strcmp(name, "refresh") == 0) return ICON_REFRESH;
    if (strcmp(name, "trash") == 0) return ICON_TRASH;
    if (strcmp(name, "log") == 0) return ICON_LOG_VIEWER;
    if (strcmp(name, "task_manager") == 0) return ICON_TASK_MANAGER;
    if (strcmp(name, "palette") == 0) return ICON_PALETTE;
    if (strcmp(name, "categories") == 0) return ICON_CATEGORIES;
    if (strcmp(name, "home") == 0) return ICON_HOME;
    if (strcmp(name, "info") == 0) return ICON_INFO_CIRCLE;
    if (strcmp(name, "window") == 0) return ICON_WINDOW;
    if (strcmp(name, "file") == 0) return ICON_FILE;
    return ICON_FILE;
}

// Map kernel app name to launch function
typedef void (*cfg_launch_fn)(void);

static cfg_launch_fn cfg_lookup_kernel(const char *name) {
    if (!name || !name[0]) return NULL;
    if (strcmp(name, "terminal") == 0) return userspace_terminal_launch;
    if (strcmp(name, "ssh") == 0) { extern void sshterm_launch(void); return sshterm_launch; }
    if (strcmp(name, "browser") == 0) return userspace_browser_launch;
    if (strcmp(name, "files") == 0) return userspace_files_launch;
    if (strcmp(name, "editor") == 0) return userspace_editor_launch;
    if (strcmp(name, "calculator") == 0) return userspace_calc_launch;
    if (strcmp(name, "paint") == 0) return userspace_paint_launch;
    if (strcmp(name, "image") == 0) return userspace_imageviewer_launch;
    if (strcmp(name, "media") == 0) return userspace_mediaplayer_launch;
    if (strcmp(name, "clock") == 0) return userspace_clock_launch;
    if (strcmp(name, "irc") == 0) return userspace_irc_launch;
    if (strcmp(name, "doom") == 0) return userspace_doom_launch;
    if (strcmp(name, "lemmings") == 0) return userspace_lemmings_launch;
    if (strcmp(name, "solitaire") == 0) return userspace_solitaire_launch;
    if (strcmp(name, "pong") == 0) return userspace_pong_launch;
    if (strcmp(name, "nethack") == 0) return nethack_launch;
    if (strcmp(name, "settings") == 0) return userspace_settings_launch;
    if (strcmp(name, "netsettings") == 0) return userspace_network_launch;
    if (strcmp(name, "taskmanager") == 0) return userspace_taskmanager_launch;
    if (strcmp(name, "recyclebin") == 0) return userspace_recyclebin_launch;
    if (strcmp(name, "syslog") == 0) return userspace_syslog_launch;
    if (strcmp(name, "cybersecurity") == 0) return cybersecurity_launch;
    if (strcmp(name, "audio_config") == 0) return userspace_audioconfig_launch;
    if (strcmp(name, "restart") == 0) return do_restart;
    if (strcmp(name, "shutdown") == 0) return do_shutdown;
    return NULL;
}

// Map category name to default icon
static int cfg_cat_icon(const char *name) {
    if (!name) return ICON_CATEGORIES;
    if (strcmp(name, "Applications") == 0) return ICON_CATEGORIES;
    if (strcmp(name, "Games") == 0) return ICON_GAME;
    if (strcmp(name, "System") == 0) return ICON_COG;
    if (strcmp(name, "Power") == 0) return ICON_POWER;
    return ICON_CATEGORIES;
}

// Helper: trim trailing whitespace in place
static void cfg_trim(char *s, int len) {
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\r'))
        s[--len] = '\0';
}

// Load start menu entries from /STRTMENU.CFG and append user-installed apps
// from /APPS/REGINI.CFG so that installs/uninstalls made over the remote
// shell show up on the next menu load (either at boot or after an
// explicit desktop_menu_reload() call from remote_ctrl).
static void start_menu_load_startmenu_cfg(void) {
    if (!g_fat_fs.mounted) return;

    uint32_t size = 0;
    char *data = (char *)fat_read_file(&g_fat_fs, "/STRTMENU.CFG", &size);

    // Try to merge /APPS/REGINI.CFG. Entries there should use the same
    // CATEGORY=.../APP=... syntax; conventionally they begin with
    // CATEGORY=Installed so they land in their own section.
    uint32_t reg_size = 0;
    char *reg_data = (char *)fat_read_file(&g_fat_fs, "/APPS/REGINI.CFG", &reg_size);
    if (reg_data && reg_size > 0) {
        uint32_t base_size = data ? size : 0;
        uint32_t combined_size = base_size + 1 + reg_size + 1;
        char *combined = (char *)kmalloc(combined_size);
        if (combined) {
            if (data) memcpy(combined, data, base_size);
            combined[base_size] = '\n';
            memcpy(combined + base_size + 1, reg_data, reg_size);
            combined[combined_size - 1] = '\0';
            if (data) kfree(data);
            data = combined;
            size = combined_size - 1;
            kprintf("[StartMenu] REGINI.CFG merged (%u bytes)\n", reg_size);
        }
        kfree(reg_data);
    }
    if (!data || size == 0) {
        kprintf("[StartMenu] No STRTMENU.CFG found, using built-in defaults\n");
        return;
    }

    g_cfg_name_idx = 0;
    int cat_count = 0;     // Total categories built so far
    int current_cat = -1;  // Index of current category being populated
    int item_counts[CFG_MAX_CATS];
    memset(item_counts, 0, sizeof(item_counts));

    char *line = data;
    while (*line) {
        // Skip whitespace
        while (*line == ' ' || *line == '\t') line++;

        // Skip comments and blank lines
        if (*line == '#' || *line == '\n' || *line == '\r' || *line == '\0') {
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
            continue;
        }

        // Find end of line for length calculation
        char *eol = line;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;

        // Display settings (backwards compatible)
        if (strncmp(line, "HEIGHT=", 7) == 0) {
            g_menu_config.height = atoi(line + 7);
            if (g_menu_config.height < START_MENU_MIN_HEIGHT)
                g_menu_config.height = START_MENU_MIN_HEIGHT;
            if (g_menu_config.height > START_MENU_MAX_HEIGHT)
                g_menu_config.height = START_MENU_MAX_HEIGHT;
        } else if (strncmp(line, "VIEW=", 5) == 0) {
            int v = atoi(line + 5);
            if (v >= 0 && v <= 2) g_menu_config.view = (menu_view_t)v;
        } else if (strncmp(line, "SORT=", 5) == 0) {
            int s = atoi(line + 5);
            if (s >= 0 && s <= 2) g_menu_config.sort = (menu_sort_t)s;
        } else if (strncmp(line, "RECENT=", 7) == 0) {
            g_menu_config.show_recent = (atoi(line + 7) != 0);
        } else if (strncmp(line, "SEARCH=", 7) == 0) {
            g_menu_config.show_search = (atoi(line + 7) != 0);
        }
        // SEPARATOR line
        else if (strncmp(line, "SEPARATOR", 9) == 0) {
            if (cat_count < CFG_MAX_CATS) {
                // Add a separator entry
                g_cfg_cat_names[cat_count][0] = '\0';
                g_cfg_items[cat_count][0] = (menu_item_t){NULL, -1, NULL, NULL};
                item_counts[cat_count] = 0;
                cat_count++;
            }
        }
        // CATEGORY=Name or CATEGORY=Name,icon
        else if (strncmp(line, "CATEGORY=", 9) == 0) {
            if (cat_count < CFG_MAX_CATS) {
                char *p = line + 9;
                int len = 0;
                while (p[len] && p[len] != '\n' && p[len] != '\r' && p[len] != ',' && len < CFG_NAME_LEN - 1)
                    len++;
                memcpy(g_cfg_cat_names[cat_count], p, len);
                g_cfg_cat_names[cat_count][len] = '\0';
                cfg_trim(g_cfg_cat_names[cat_count], len);

                g_cfg_items[cat_count][0] = (menu_item_t){NULL, -1, NULL, NULL};
                item_counts[cat_count] = 0;
                current_cat = cat_count;
                cat_count++;
            }
        }
        // APP=Name,icon,command
        else if (strncmp(line, "APP=", 4) == 0 && current_cat >= 0) {
            if (item_counts[current_cat] < CFG_MAX_ITEMS_PER_CAT &&
                g_cfg_name_idx < CFG_MAX_CATS * CFG_MAX_ITEMS_PER_CAT) {

                char *p = line + 4;

                // Parse display name (until comma)
                char name[CFG_NAME_LEN];
                int ni = 0;
                while (*p && *p != ',' && *p != '\n' && *p != '\r' && ni < CFG_NAME_LEN - 1)
                    name[ni++] = *p++;
                name[ni] = '\0';
                if (*p == ',') p++;

                // Parse icon name (until comma)
                char icon_name[32];
                int ii = 0;
                while (*p && *p != ',' && *p != '\n' && *p != '\r' && ii < 31)
                    icon_name[ii++] = *p++;
                icon_name[ii] = '\0';
                if (*p == ',') p++;

                // Parse command (rest of line)
                char command[CFG_PATH_LEN];
                int ci = 0;
                while (*p && *p != '\n' && *p != '\r' && ci < CFG_PATH_LEN - 1)
                    command[ci++] = *p++;
                command[ci] = '\0';
                cfg_trim(command, ci);

                // Build menu item
                int idx = item_counts[current_cat];
                menu_item_t *item = &g_cfg_items[current_cat][idx];

                // Store name string persistently
                strncpy(g_cfg_names[g_cfg_name_idx], name, CFG_NAME_LEN - 1);
                g_cfg_names[g_cfg_name_idx][CFG_NAME_LEN - 1] = '\0';
                item->name = g_cfg_names[g_cfg_name_idx];

                // Look up icon
                item->icon_id = cfg_lookup_icon(icon_name);

                // Determine launch method
                item->action = NULL;
                item->userspace_path = NULL;

                if (strncmp(command, "kernel:", 7) == 0) {
                    // Kernel built-in app
                    item->action = cfg_lookup_kernel(command + 7);
                } else if (strcmp(command, "wallpicker") == 0) {
                    // Special wallpaper picker (detected by ICON_PALETTE + NULL action)
                    item->icon_id = ICON_PALETTE;
                } else if (command[0] == '/') {
                    // Userspace ELF path
                    strncpy(g_cfg_paths[g_cfg_name_idx], command, CFG_PATH_LEN - 1);
                    g_cfg_paths[g_cfg_name_idx][CFG_PATH_LEN - 1] = '\0';
                    item->userspace_path = g_cfg_paths[g_cfg_name_idx];
                    // Try to find a kernel fallback using the filename
                    const char *app_name = command;
                    for (const char *s = command; *s; s++) {
                        if (*s == '/') app_name = s + 1;
                    }
                    item->action = cfg_lookup_kernel(app_name);
                }

                g_cfg_name_idx++;
                item_counts[current_cat]++;

                // Keep NULL terminator after last item
                g_cfg_items[current_cat][item_counts[current_cat]] =
                    (menu_item_t){NULL, -1, NULL, NULL};
            }
        }

        // Advance to next line
        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }

    kfree(data);

    if (cat_count == 0) {
        kprintf("[StartMenu] STRTMENU.CFG: no categories found\n");
        return;
    }

    // Rebuild g_start_menu_categories from parsed config
    int i;
    for (i = 0; i < cat_count && i < MAX_MENU_CATEGORIES - 1; i++) {
        if (g_cfg_cat_names[i][0] == '\0') {
            // Separator
            g_start_menu_categories[i] = (menu_category_t){NULL, -1, NULL, 0, true};
        } else {
            g_start_menu_categories[i] = (menu_category_t){
                g_cfg_cat_names[i],
                cfg_cat_icon(g_cfg_cat_names[i]),
                g_cfg_items[i],
                0,
                false
            };
        }
    }
    // Terminator
    g_start_menu_categories[i] = (menu_category_t){NULL, -1, NULL, 0, false};

    g_startmenu_cfg_loaded = true;
    kprintf("[StartMenu] STRTMENU.CFG loaded: %d categories\n", cat_count);
    for (i = 0; i < cat_count; i++) {
        if (g_cfg_cat_names[i][0])
            kprintf("[StartMenu]   %s: %d items\n", g_cfg_cat_names[i], item_counts[i]);
    }
}


// Menu categories
// Runtime menu initialization (workaround for .data section loading issue)
static void init_start_menu_items(void) {
    kprintf("[StartMenu] Runtime init of menu items...\n");
    g_apps_items[0] = (menu_item_t){"Terminal", ICON_TERMINAL, userspace_terminal_launch, NULL};
    g_apps_items[1] = (menu_item_t){"Browser", ICON_NETWORK, userspace_browser_launch, NULL};
    g_apps_items[2] = (menu_item_t){"Files", ICON_FOLDER, userspace_files_launch, NULL};
    g_apps_items[3] = (menu_item_t){"Editor", ICON_HIGHLIGHT, userspace_editor_launch, NULL};
    g_apps_items[4] = (menu_item_t){"Calculator", ICON_CALCULATOR, userspace_calc_launch, NULL};
    g_apps_items[5] = (menu_item_t){"Paint", ICON_PAINT, userspace_paint_launch, NULL};
    g_apps_items[6] = (menu_item_t){"Images", ICON_IMAGE, userspace_imageviewer_launch, NULL};
    g_apps_items[7] = (menu_item_t){"Media", ICON_PLAY, userspace_mediaplayer_launch, NULL};
    g_apps_items[8] = (menu_item_t){"Clock", ICON_CLOCK, userspace_clock_launch, NULL};
    g_apps_items[9] = (menu_item_t){"IRC", ICON_NETWORK, userspace_irc_launch, NULL};
    g_apps_items[10] = (menu_item_t){"App Store", ICON_NETWORK, userspace_appstore_launch, NULL};
    g_apps_items[11] = (menu_item_t){NULL, -1, NULL, NULL};
    g_games_items[0] = (menu_item_t){"DOOM", ICON_GAME_DOOM, userspace_doom_launch, NULL};
    g_games_items[1] = (menu_item_t){"Solitaire", ICON_GAME_SOLITAIRE, userspace_solitaire_launch, NULL};
    g_games_items[2] = (menu_item_t){"Pong", ICON_GAME_PONG, userspace_pong_launch, NULL};
    g_games_items[3] = (menu_item_t){"Lemmings", ICON_GAME_LEMMINGS, userspace_lemmings_launch, NULL};
    g_games_items[4] = (menu_item_t){"NetHack", ICON_GAME, nethack_launch, NULL};
    g_games_items[5] = (menu_item_t){NULL, -1, NULL, NULL};
    g_system_items[0] = (menu_item_t){"Settings", ICON_COG, userspace_settings_launch, NULL};
    g_system_items[1] = (menu_item_t){"Network", ICON_NETWORK, userspace_network_launch, NULL};
    g_system_items[2] = (menu_item_t){"Task Mgr", ICON_TASK_MANAGER, userspace_taskmanager_launch, NULL};
    g_system_items[3] = (menu_item_t){"Recycle", ICON_TRASH, userspace_recyclebin_launch, NULL};
    g_system_items[4] = (menu_item_t){"Sys Log", ICON_LOG_VIEWER, userspace_syslog_launch, NULL};
    g_system_items[5] = (menu_item_t){"Audio Cfg", ICON_MUSIC, userspace_audioconfig_launch, NULL};
    g_system_items[6] = (menu_item_t){"Security", ICON_COG, cybersecurity_launch, NULL};
    g_system_items[7] = (menu_item_t){NULL, -1, NULL, NULL};
    g_power_items[0] = (menu_item_t){"Restart", ICON_REFRESH, do_restart, NULL};
    g_power_items[1] = (menu_item_t){"Shutdown", ICON_POWER, do_shutdown, NULL};
    g_power_items[2] = (menu_item_t){NULL, -1, NULL, NULL};
    g_start_menu_categories[0] = (menu_category_t){"Applications", ICON_CATEGORIES, g_apps_items, 0, false};
    g_start_menu_categories[1] = (menu_category_t){"Games", ICON_GAME, g_games_items, 0, false};
    g_start_menu_categories[2] = (menu_category_t){"System", ICON_COG, g_system_items, 0, false};
    g_start_menu_categories[3] = (menu_category_t){NULL, -1, NULL, 0, true};
    g_start_menu_categories[4] = (menu_category_t){"Power", ICON_POWER, g_power_items, 0, false};
    g_start_menu_categories[5] = (menu_category_t){NULL, -1, NULL, 0, false};

    // Initialize recent files list
    for (int i = 0; i < MAX_RECENT_FILES; i++) {
        g_recent_files[i].path[0] = '\0';
        g_recent_files[i].name[0] = '\0';
        g_recent_files[i].timestamp = 0;
        g_recent_files[i].access_count = 0;
    }
    g_recent_file_count = 0;

    // Load menu configuration from disk
    start_menu_load_config();

    // Load app entries from STRTMENU.CFG (overrides built-in defaults if present)
    start_menu_load_startmenu_cfg();

    kprintf("[StartMenu] Menu items initialized (view=%d, sort=%d, height=%d)\n",
            g_menu_config.view, g_menu_config.sort, g_menu_config.height);
}

// Public reload hook. Called from net/remote_ctrl.c after an install or
// uninstall mutates /APPS/REGINI.CFG, so the new entries appear in the
// start menu without a reboot.
void desktop_menu_reload(void) {
    start_menu_load_startmenu_cfg();
    for (int c = 0;
         g_start_menu_categories[c].name || g_start_menu_categories[c].is_separator;
         c++) {
        if (g_start_menu_categories[c].items) {
            g_start_menu_categories[c].item_count =
                count_menu_items(g_start_menu_categories[c].items);
        }
    }
    kprintf("[StartMenu] reload complete\n");
}

// ============================================================================
// Recent Files Management
// ============================================================================

// Add a file to recent files list (called by file opener apps)
// Exported so other modules can call it
__attribute__((unused))
void start_menu_add_recent(const char *path, const char *name) {
    if (!path || !name) return;

    // Check if already in list - if so, update timestamp and move to front
    for (int i = 0; i < g_recent_file_count; i++) {
        if (strcmp(g_recent_files[i].path, path) == 0) {
            g_recent_files[i].timestamp = timer_ticks;
            g_recent_files[i].access_count++;
            // Move to front
            if (i > 0) {
                recent_file_t temp = g_recent_files[i];
                for (int j = i; j > 0; j--) {
                    g_recent_files[j] = g_recent_files[j-1];
                }
                g_recent_files[0] = temp;
            }
            return;
        }
    }

    // Add new entry - shift everything down
    if (g_recent_file_count < MAX_RECENT_FILES) {
        g_recent_file_count++;
    }
    for (int i = g_recent_file_count - 1; i > 0; i--) {
        g_recent_files[i] = g_recent_files[i-1];
    }

    // Copy to first slot
    strncpy(g_recent_files[0].path, path, RECENT_FILE_PATH_LEN - 1);
    g_recent_files[0].path[RECENT_FILE_PATH_LEN - 1] = '\0';
    strncpy(g_recent_files[0].name, name, RECENT_FILE_NAME_LEN - 1);
    g_recent_files[0].name[RECENT_FILE_NAME_LEN - 1] = '\0';
    g_recent_files[0].timestamp = timer_ticks;
    g_recent_files[0].access_count = 1;

    kprintf("[StartMenu] Added recent file: %s\n", name);
}

// Format relative time string (e.g., "just now", "1 hour ago")
static void format_relative_time(uint64_t timestamp, char *buf, int buflen) {
    uint64_t now = timer_ticks;
    uint64_t diff = now - timestamp;

    // Timer is 1000 Hz, so 1000 ticks = 1 second
    uint64_t seconds = diff / g_timer_hz;

    if (seconds < 60) {
        strncpy(buf, "just now", buflen);
    } else if (seconds < 3600) {
        int minutes = seconds / 60;
        if (minutes == 1) {
            strncpy(buf, "1 min ago", buflen);
        } else {
            snprintf(buf, buflen, "%d min ago", minutes);
        }
    } else if (seconds < 86400) {
        int hours = seconds / 3600;
        if (hours == 1) {
            strncpy(buf, "1 hour ago", buflen);
        } else {
            snprintf(buf, buflen, "%d hours ago", hours);
        }
    } else {
        int days = seconds / 86400;
        if (days == 1) {
            strncpy(buf, "yesterday", buflen);
        } else {
            snprintf(buf, buflen, "%d days ago", days);
        }
    }
    buf[buflen - 1] = '\0';
}

// ============================================================================
// Menu Configuration (Save/Load)
// ============================================================================

// Save menu configuration to /CONFIG/MENU.CFG
static void start_menu_save_config(void) {
    if (!g_fat_fs.mounted) return;

    // Create config string
    char config[512];
    snprintf(config, sizeof(config),
        "HEIGHT=%d\nVIEW=%d\nSORT=%d\nRECENT=%d\nSEARCH=%d\n",
        g_menu_config.height,
        (int)g_menu_config.view,
        (int)g_menu_config.sort,
        g_menu_config.show_recent ? 1 : 0,
        g_menu_config.show_search ? 1 : 0
    );

    // Write to file
    int result = fat_write_file(&g_fat_fs, "/CONFIG/MENU.CFG", config, strlen(config));
    if (result >= 0) {
        kprintf("[StartMenu] Config saved to /CONFIG/MENU.CFG\n");
    } else {
        kprintf("[StartMenu] Failed to save config\n");
    }
}

// Load menu configuration from /CONFIG/MENU.CFG
static void start_menu_load_config(void) {
    if (!g_fat_fs.mounted) return;

    uint32_t size = 0;
    char *data = (char *)fat_read_file(&g_fat_fs, "/CONFIG/MENU.CFG", &size);
    if (!data || size == 0) {
        kprintf("[StartMenu] No config file found, using defaults\n");
        return;
    }

    // Parse config (simple key=value format)
    char *line = data;
    while (*line) {
        // Skip whitespace
        while (*line == ' ' || *line == '\t') line++;

        if (strncmp(line, "HEIGHT=", 7) == 0) {
            g_menu_config.height = atoi(line + 7);
            if (g_menu_config.height < START_MENU_MIN_HEIGHT)
                g_menu_config.height = START_MENU_MIN_HEIGHT;
            if (g_menu_config.height > START_MENU_MAX_HEIGHT)
                g_menu_config.height = START_MENU_MAX_HEIGHT;
        } else if (strncmp(line, "VIEW=", 5) == 0) {
            int v = atoi(line + 5);
            if (v >= 0 && v <= 2) g_menu_config.view = (menu_view_t)v;
        } else if (strncmp(line, "SORT=", 5) == 0) {
            int s = atoi(line + 5);
            if (s >= 0 && s <= 2) g_menu_config.sort = (menu_sort_t)s;
        } else if (strncmp(line, "RECENT=", 7) == 0) {
            g_menu_config.show_recent = (atoi(line + 7) != 0);
        } else if (strncmp(line, "SEARCH=", 7) == 0) {
            g_menu_config.show_search = (atoi(line + 7) != 0);
        }

        // Move to next line
        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }

    kfree(data);
    kprintf("[StartMenu] Config loaded: height=%d view=%d sort=%d\n",
            g_menu_config.height, g_menu_config.view, g_menu_config.sort);
}

// Toggle between view modes
static void __attribute__((unused)) start_menu_toggle_view(void) {
    g_menu_config.view = (menu_view_t)((g_menu_config.view + 1) % 3);
    start_menu_save_config();
    kprintf("[StartMenu] View mode changed to %d\n", g_menu_config.view);
}

// Cycle through sort modes (for future use in settings)
__attribute__((unused))
static void start_menu_cycle_sort(void) {
    g_menu_config.sort = (menu_sort_t)((g_menu_config.sort + 1) % 3);
    start_menu_save_config();
    kprintf("[StartMenu] Sort mode changed to %d\n", g_menu_config.sort);
}

// Adjust menu height (for future use in settings or drag-to-resize)
__attribute__((unused))
static void start_menu_adjust_height(int delta) {
    g_menu_config.height += delta;
    if (g_menu_config.height < START_MENU_MIN_HEIGHT)
        g_menu_config.height = START_MENU_MIN_HEIGHT;
    if (g_menu_config.height > START_MENU_MAX_HEIGHT)
        g_menu_config.height = START_MENU_MAX_HEIGHT;
    start_menu_save_config();
}

// ============================================================================
// Context Menu (Right-click on desktop)
// ============================================================================
static bool g_context_menu_open = false;
static int32_t g_context_menu_x = 0;
static int32_t g_context_menu_y = 0;
static int g_context_submenu_open = -1;  // Index of open submenu, -1 = none

#define CONTEXT_MENU_WIDTH   160
#define CONTEXT_MENU_ITEM_H  24
#define CONTEXT_MENU_SEP_H   8
#define SUBMENU_ARROW_WIDTH  12

typedef struct {
    const char *name;
    void (*action)(void);
    menu_item_t *submenu;      // NULL if no submenu, otherwise points to submenu items
    int submenu_count;         // Number of items in submenu
} context_item_t;

// Forward declarations
static void open_wallpaper_picker(void);
static void close_wallpaper_picker(void);
static void open_screensaver_settings(void);
static void close_screensaver_settings(void);
static void do_refresh(void);
static void do_new_folder(void);
static void do_new_file(void);
static void do_paste(void);
static void do_display_settings(void);
static void do_properties(void);

// Context menu items (right-click on desktop)
static context_item_t g_context_items[] = {
    {"New Folder",         do_new_folder, NULL, 0},
    {"New File",           do_new_file, NULL, 0},
    {"Refresh",            do_refresh, NULL, 0},
    {"Paste",              do_paste, NULL, 0},
    {"---",                NULL, NULL, 0},  // Separator
    {"Display Settings",   do_display_settings, NULL, 0},
    {"Change Background",  open_wallpaper_picker, NULL, 0},
    {"---",                NULL, NULL, 0},  // Separator
    {"Properties",         do_properties, NULL, 0},
    {NULL, NULL, NULL, 0}
};

// Context menu action handlers
static void do_new_folder(void) {
    kprintf("[Desktop] New Folder requested\n");
    // TODO: Implement create new folder dialog
}

static void do_new_file(void) {
    kprintf("[Desktop] New File requested\n");
    // TODO: Implement create new file dialog
}

static void do_paste(void) {
    kprintf("[Desktop] Paste requested\n");
    // TODO: Implement paste from clipboard
}

static void do_display_settings(void) {
    kprintf("[Desktop] Display Settings requested\n");
    settings_launch();  // Open settings app
}

static void do_properties(void) {
    kprintf("[Desktop] Properties requested\n");
    // TODO: Implement desktop properties dialog
}

// Refresh handler - just invalidate to trigger redraw
static void do_refresh(void) {
    kprintf("[Desktop] Refresh requested\n");
    wm_invalidate_all();
}

// Forward declarations for context menu
static void draw_context_menu(void);
static int get_context_menu_item_at(int x, int y);

// ============================================================================
// Wallpaper Picker
// ============================================================================
static bool g_wallpaper_picker_open = false;
static int32_t g_picker_x = 0;
static int32_t g_picker_y = 0;

// Available wallpapers (matching files on disk)
typedef struct {
    const char *name;
    const char *filename;
} wallpaper_entry_t;

static wallpaper_entry_t g_wallpapers[] = {
    // Default background
    {"Default Blue", "BACK.BMP"},
    // Eberhard Grossgasteiger landscapes
    {"Mountain Vista 1", "EBERG01.BMP"},
    {"Mountain Vista 2", "EBERG02.BMP"},
    {"Mountain Vista 3", "EBERG03.BMP"},
    {"Mountain Vista 4", "EBERG04.BMP"},
    {"Mountain Vista 5", "EBERG05.BMP"},
    {"Alpine Scenery 1", "EBERG06.BMP"},
    {"Alpine Scenery 2", "EBERG07.BMP"},
    {"Alpine Scenery 3", "EBERG08.BMP"},
    {"Alpine Scenery 4", "EBERG09.BMP"},
    {"Alpine Scenery 5", "EBERG10.BMP"},
    {"Nature Landscape 1", "EBERG11.BMP"},
    {"Nature Landscape 2", "EBERG12.BMP"},
    {"Nature Landscape 3", "EBERG13.BMP"},
    {"Nature Landscape 4", "EBERG15.BMP"},
    {"Nature Landscape 5", "EBERG16.BMP"},
    {"Scenic View 1", "EBERG17.BMP"},
    {"Scenic View 2", "EBERG18.BMP"},
    {"Scenic View 3", "EBERG19.BMP"},
    {"Scenic View 4", "EBERG20.BMP"},
    {"Scenic View 5", "EBERG21.BMP"},
    {"Panorama 1", "EBERG22.BMP"},
    {"Panorama 2", "EBERG25.BMP"},
    {"Panorama 3", "EBERG26.BMP"},
    {"Panorama 4", "EBERG27.BMP"},
    {"Panorama 5", "EBERG28.BMP"},
    {"Panorama 6", "EBERG29.BMP"},
    {"Panorama 7", "EBERG30.BMP"},
    // Ocean images
    {"Ocean Waves 1", "OCEAN01.BMP"},
    {"Ocean Waves 2", "OCEAN02.BMP"},
    {"Ocean Sunset 1", "OCEAN03.BMP"},
    {"Ocean Sunset 2", "OCEAN04.BMP"},
    {"Seascape 1", "OCEAN05.BMP"},
    {"Seascape 2", "OCEAN06.BMP"},
    {"Seascape 3", "OCEAN07.BMP"},
    {"Coastal View 1", "OCEAN08.BMP"},
    {"Coastal View 2", "OCEAN10.BMP"},
    {"Beach Scene 1", "OCEAN12.BMP"},
    {"Beach Scene 2", "OCEAN13.BMP"},
    {"Beach Scene 3", "OCEAN14.BMP"},
    // Macro nature images
    {"Macro Nature 1", "MACRO01.BMP"},
    {"Macro Nature 2", "MACRO02.BMP"},
    {"Macro Nature 3", "MACRO05.BMP"},
    {"Macro Nature 4", "MACRO06.BMP"},
    {"Macro Nature 5", "MACRO07.BMP"},
    {"Macro Details 1", "MACRO08.BMP"},
    {"Macro Details 2", "MACRO11.BMP"},
    {"Macro Details 3", "MACRO12.BMP"},
    {"Macro Details 4", "MACRO13.BMP"},
    {"Macro Details 5", "MACRO14.BMP"},
    {"Close-up Nature 1", "MACRO15.BMP"},
    {"Close-up Nature 2", "MACRO16.BMP"},
    {"Close-up Nature 3", "MACRO17.BMP"},
    {"Close-up Nature 4", "MACRO19.BMP"},
    {"Close-up Nature 5", "MACRO20.BMP"},
    // Theme default wallpapers
    {"Default Theme", "DEFAULT.BMP"},
    {"Retro Unix", "RETRO.BMP"},
    {"Retro Stipple", "RETROSTIP.BMP"},
    {"Modern Light", "MODLIGHT.BMP"},
    {"Modern Dark", "MODDARK.BMP"},
    {"Fluent Light", "FLULIGHT.BMP"},
    {"Fluent Dark", "FLUDARK.BMP"},
    {"Ocean Theme", "OCEAN.BMP"},
    {"Sunset Theme", "SUNSET.BMP"},
    {"Forest Theme", "FOREST.BMP"},
    {"Classic Win95", "CLASSIC.BMP"},
    {"Dark Mode", "DARKMODE.BMP"},
    {"Light Mode", "LIGHTMODE.BMP"},
    {"High Contrast", "HIGHCON.BMP"},
    // Gradient option
    {"Gradient (Blue)", NULL},
    {NULL, NULL}
};

// Thumbnail dimensions (64x48 maintains 4:3 aspect ratio)
#define THUMB_WIDTH     64
#define THUMB_HEIGHT    48
#define THUMB_PADDING   8
#define THUMB_BORDER    2

// Grid layout: columns and spacing
#define THUMB_COLS      5
#define THUMB_CELL_W    (THUMB_WIDTH + THUMB_PADDING)
#define THUMB_CELL_H    (THUMB_HEIGHT + THUMB_PADDING + 16)  // +16 for name text below

// Picker dimensions calculated from grid
#define PICKER_WIDTH    (THUMB_COLS * THUMB_CELL_W + THUMB_PADDING * 2)
#define PICKER_HEIGHT   400
#define PICKER_ITEM_H   20
#define PICKER_TITLE_H  24
#define PICKER_CONTENT_H (PICKER_HEIGHT - PICKER_TITLE_H - 8)

// Maximum wallpapers (for cache sizing)
#define MAX_WALLPAPERS  64

// Thumbnail cache structure
typedef struct {
    uint32_t *pixels;       // Thumbnail pixel data
    uint32_t width;         // Actual thumbnail width
    uint32_t height;        // Actual thumbnail height
    bool loaded;            // Whether thumbnail is loaded
} thumbnail_t;

// Thumbnail cache
static thumbnail_t g_thumbnails[MAX_WALLPAPERS];
static bool g_thumbnails_initialized = false;
static int g_picker_scroll = 0;     // Current scroll offset in rows
static int g_picker_max_scroll = 0; // Maximum scroll offset
static int g_current_wallpaper_index = 0;  // Currently selected wallpaper index

// Forward declarations for wallpaper picker
static void draw_wallpaper_picker(void);
static int get_wallpaper_item_at(int x, int y);
static void set_wallpaper(const char *filename);
static void generate_thumbnail(int index);
static void generate_thumbnails(void);
static void free_thumbnails(void);
static uint32_t *scale_image_to_thumbnail(uint32_t *src_pixels, uint32_t src_w, uint32_t src_h,
                                          uint32_t *out_w, uint32_t *out_h);

// ============================================================================
// Screensaver Settings Picker
// ============================================================================
static bool g_ss_settings_open = false;
static int32_t g_ss_settings_x = 0;
static int32_t g_ss_settings_y = 0;
__attribute__((unused)) static int g_ss_selected_type = 0;
__attribute__((unused)) static int g_ss_selected_timeout = 2;  // Index into timeout options

#define SS_SETTINGS_WIDTH   280
#define SS_SETTINGS_HEIGHT  260
#define SS_ITEM_H           22

// Timeout options in seconds
static const uint32_t g_ss_timeouts[] = {60, 120, 300, 600, 0};  // 1, 2, 5, 10 min, Never
static const char *g_ss_timeout_names[] = {"1 minute", "2 minutes", "5 minutes", "10 minutes", "Never", NULL};

// Forward declarations for screensaver settings
static void draw_screensaver_settings(void);
static int get_ss_settings_item_at(int x, int y);

// ============================================================================
// Internal Helper Functions
// ============================================================================

// Convert ARGB color (0xAARRGGBB) to framebuffer format (BGRA)
static uint32_t argb_to_fb(uint32_t argb) {
    uint8_t a = (argb >> 24) & 0xFF;
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >> 8) & 0xFF;
    uint8_t b = argb & 0xFF;
    UNUSED(a);  // Alpha handled separately for blending
    // Framebuffer is BGRA: blue in low byte
    return (b << 16) | (g << 8) | r;
}

// Simple alpha blending (integer math only, no floating point)
// alpha is 0-255 where 255 is fully opaque
static uint32_t blend_color(uint32_t bg, uint32_t fg, uint8_t alpha) {
    if (alpha == 255) return fg;
    if (alpha == 0) return bg;

    uint8_t bg_r = bg & 0xFF;
    uint8_t bg_g = (bg >> 8) & 0xFF;
    uint8_t bg_b = (bg >> 16) & 0xFF;

    uint8_t fg_r = fg & 0xFF;
    uint8_t fg_g = (fg >> 8) & 0xFF;
    uint8_t fg_b = (fg >> 16) & 0xFF;

    // Integer alpha blending: out = bg + (fg - bg) * alpha / 255
    uint8_t out_r = bg_r + (((fg_r - bg_r) * alpha) >> 8);
    uint8_t out_g = bg_g + (((fg_g - bg_g) * alpha) >> 8);
    uint8_t out_b = bg_b + (((fg_b - bg_b) * alpha) >> 8);

    return (out_b << 16) | (out_g << 8) | out_r;
}

// Draw a filled rectangle with alpha blending
__attribute__((unused))
static void draw_rect_alpha(int32_t x, int32_t y, uint32_t w, uint32_t h,
                            uint32_t argb_color) {
    uint8_t alpha = (argb_color >> 24) & 0xFF;
    uint32_t fb_color = argb_to_fb(argb_color);

    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    // Clipping
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;

    if (alpha == 255) {
        // Fully opaque - use fast fill
        fb_fill_rect(x, y, w, h, fb_color);
    } else {
        // Semi-transparent - blend each pixel
        for (uint32_t py = 0; py < h; py++) {
            for (uint32_t px = 0; px < w; px++) {
                uint32_t bg = fb_get_pixel(x + px, y + py);
                uint32_t blended = blend_color(bg, fb_color, alpha);
                fb_put_pixel(x + px, y + py, blended);
            }
        }
    }
}

// Draw a rounded rectangle (simplified - just corners clipped)
static void draw_rounded_rect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                              uint32_t radius, uint32_t argb_color) {
    uint8_t alpha = (argb_color >> 24) & 0xFF;
    uint32_t fb_color = argb_to_fb(argb_color);

    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    // Draw main body (excluding corners)
    for (uint32_t py = 0; py < h; py++) {
        for (uint32_t px = 0; px < w; px++) {
            int32_t sx = x + px;
            int32_t sy = y + py;

            // Skip if outside screen
            if (sx < 0 || sy < 0 || (uint32_t)sx >= screen_w || (uint32_t)sy >= screen_h)
                continue;

            // Check if in corner region and should be clipped
            bool in_corner = false;
            int32_t dx = 0, dy = 0;

            // Top-left corner
            if (px < radius && py < radius) {
                dx = radius - px - 1;
                dy = radius - py - 1;
                in_corner = true;
            }
            // Top-right corner
            else if (px >= w - radius && py < radius) {
                dx = px - (w - radius);
                dy = radius - py - 1;
                in_corner = true;
            }
            // Bottom-left corner
            else if (px < radius && py >= h - radius) {
                dx = radius - px - 1;
                dy = py - (h - radius);
                in_corner = true;
            }
            // Bottom-right corner
            else if (px >= w - radius && py >= h - radius) {
                dx = px - (w - radius);
                dy = py - (h - radius);
                in_corner = true;
            }

            // If in corner, check if outside the rounded corner circle
            if (in_corner) {
                // Distance squared from corner center
                uint32_t dist_sq = dx * dx + dy * dy;
                uint32_t radius_sq = radius * radius;
                if (dist_sq > radius_sq) {
                    continue;  // Outside corner, skip this pixel
                }
            }

            // Draw the pixel
            if (alpha == 255) {
                fb_put_pixel(sx, sy, fb_color);
            } else {
                uint32_t bg = fb_get_pixel(sx, sy);
                fb_put_pixel(sx, sy, blend_color(bg, fb_color, alpha));
            }
        }
    }
}

// Draw a character at position using font
static void draw_char(int32_t x, int32_t y, char c, uint32_t color) {
    if (ttf_is_ready()) {
        ttf_draw_char(x, y, (unsigned char)c, TTF_SIZE_NORMAL, TTF_STYLE_NORMAL, color);
        return;
    }
    const uint8_t *glyph = font_get_glyph(c);
    if (!glyph) return;

    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    for (int row = 0; row < FONT_HEIGHT; row++) {
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (glyph[row] & (0x80 >> col)) {
                int32_t px = x + col;
                int32_t py = y + row;
                if (px >= 0 && py >= 0 && (uint32_t)px < screen_w && (uint32_t)py < screen_h) {
                    fb_put_pixel(px, py, color);
                }
            }
        }
    }
}

// Draw a string at position
static void draw_string(int32_t x, int32_t y, const char *str, uint32_t color) {
    if (ttf_is_ready()) {
        ttf_draw_string(x, y, str, TTF_SIZE_NORMAL, color);
        return;
    }
    while (*str) {
        draw_char(x, y, *str, color);
        x += FONT_WIDTH;
        str++;
    }
}

// Get string width in pixels
static uint32_t string_width(const char *str) {
    if (ttf_is_ready()) {
        return ttf_measure_string(str, TTF_SIZE_NORMAL);
    }
    return strlen(str) * FONT_WIDTH;
}

// Calculate taskbar position and dimensions (half width, left-aligned at bottom)
static void dock_recalculate(void) {
    dock_t *dock = &g_desktop.dock;

    // Taskbar is half screen width, left-aligned at bottom
    dock->width = g_desktop.screen_width / 2;
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
    dock->height = TASKBAR_HEIGHT;
    dock->x = 0;  // Left-aligned
    dock->y = g_desktop.screen_height - dock->height;

    // App icons start after the Start button
    int32_t icon_x = START_BTN_WIDTH + TASKBAR_PADDING;
    int32_t icon_y = dock->y + (TASKBAR_HEIGHT - PROCESS_BTN_SIZE) / 2;

    // Calculate positions for each app (square buttons)
    for (uint32_t i = 0; i < DOCK_MAX_APPS; i++) {
        if (dock->apps[i].active) {
            dock->apps[i].x = icon_x;
            dock->apps[i].y = icon_y;
            icon_x += PROCESS_BTN_SIZE + TASKBAR_ICON_SPACING;
        }
    }

    kprintf("[Taskbar] Recalculated: %u apps, at (%d,%d) size %ux%u\n",
            dock->app_count, dock->x, dock->y, dock->width, dock->height);
}

// ============================================================================
// Clock Widget - CMOS RTC Functions
// ============================================================================

// CMOS RTC port addresses
#define CMOS_ADDRESS_PORT   0x70
#define CMOS_DATA_PORT      0x71

// CMOS RTC register addresses
#define CMOS_REG_SECONDS    0x00
#define CMOS_REG_MINUTES    0x02
#define CMOS_REG_HOURS      0x04
#define CMOS_REG_STATUS_B   0x0B

// Read a byte from CMOS register
static uint8_t cmos_read(uint8_t reg) {
    // Disable NMI (bit 7) and select register
    outb(CMOS_ADDRESS_PORT, (1 << 7) | reg);
    io_wait();
    return inb(CMOS_DATA_PORT);
}

// Convert BCD to binary
static uint8_t bcd_to_binary(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

// Check if CMOS is in BCD mode (bit 2 of status register B = 0 means BCD)
static bool cmos_is_bcd(void) {
    uint8_t status_b = cmos_read(CMOS_REG_STATUS_B);
    return !(status_b & 0x04);
}

// Read current time from CMOS RTC
static void rtc_get_time(uint8_t *hours, uint8_t *minutes, uint8_t *seconds) {
    bool is_bcd = cmos_is_bcd();

    *seconds = cmos_read(CMOS_REG_SECONDS);
    *minutes = cmos_read(CMOS_REG_MINUTES);
    *hours = cmos_read(CMOS_REG_HOURS);

    // Convert from BCD if needed
    if (is_bcd) {
        *seconds = bcd_to_binary(*seconds);
        *minutes = bcd_to_binary(*minutes);
        *hours = bcd_to_binary(*hours & 0x7F);  // Mask bit 7 for 12-hour mode AM/PM
    }
}

// Clock widget dimensions
#define CLOCK_PADDING_X     12
#define CLOCK_PADDING_Y     6
#define CLOCK_MARGIN_RIGHT  16
#define CLOCK_MARGIN_TOP    10
#define CLOCK_CORNER_RADIUS 10

// Draw the taskbar clock in the top-right corner
static void desktop_clock_draw(void) {
    uint8_t hours, minutes, seconds;
    rtc_get_time(&hours, &minutes, &seconds);

    // Format time string as HH:MM:SS
    char time_str[9];
    time_str[0] = '0' + (hours / 10);
    time_str[1] = '0' + (hours % 10);
    time_str[2] = ':';
    time_str[3] = '0' + (minutes / 10);
    time_str[4] = '0' + (minutes % 10);
    time_str[5] = ':';
    time_str[6] = '0' + (seconds / 10);
    time_str[7] = '0' + (seconds % 10);
    time_str[8] = '\0';

    // Calculate clock widget dimensions
    uint32_t text_width = string_width(time_str);
    uint32_t widget_width = text_width + (CLOCK_PADDING_X * 2);
    uint32_t widget_height = FONT_HEIGHT + (CLOCK_PADDING_Y * 2);

    // Position in top-right corner
    int32_t widget_x = g_desktop.screen_width - widget_width - CLOCK_MARGIN_RIGHT;
    int32_t widget_y = CLOCK_MARGIN_TOP;

    // Draw semi-transparent dark background pill shape
    // 0xCC = ~80% opacity, 0x222222 = dark gray
    draw_rounded_rect(widget_x, widget_y, widget_width, widget_height,
                      CLOCK_CORNER_RADIUS, 0xCC222222);

    // Draw white text
    int32_t text_x = widget_x + CLOCK_PADDING_X;
    int32_t text_y = widget_y + CLOCK_PADDING_Y;
    draw_string(text_x, text_y, time_str, argb_to_fb(0xFFFFFFFF));
}

// ============================================================================
// Desktop API Implementation
// ============================================================================

// Generate a nice gradient background (ocean/sky theme to match Pexels image)
static void generate_gradient_background(void) {
    uint32_t width = g_desktop.screen_width;
    uint32_t height = g_desktop.screen_height;

    // Allocate pixel buffer
    size_t size = width * height * sizeof(uint32_t);
    uint32_t *pixels = (uint32_t *)kmalloc(size);
    if (!pixels) {
        kprintf("[Desktop] Failed to allocate gradient buffer\n");
        return;
    }

    // Generate gradient from deep blue (top) to cyan/teal (bottom)
    // Colors matching ocean theme like the Pexels image
    for (uint32_t y = 0; y < height; y++) {
        // Interpolate from top color to bottom color
        uint32_t t = (y * 255) / height;  // 0-255 based on y position

        // Top: sky blue (RGB: 0x4a90c2)  Bottom: ocean blue (RGB: 0x1e5a8a)
        uint8_t r = 0x4a - ((0x4a - 0x1e) * t) / 255;
        uint8_t g = 0x90 - ((0x90 - 0x5a) * t) / 255;
        uint8_t b = 0xc2 - ((0xc2 - 0x8a) * t) / 255;

        // Use argb_to_fb for correct color conversion
        // argb_to_fb converts from ARGB to framebuffer format (BGRA)
        uint32_t argb = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        uint32_t fb_color = argb_to_fb(argb);

        for (uint32_t x = 0; x < width; x++) {
            pixels[y * width + x] = fb_color;
        }
    }

    // Set up image structure
    g_bg_image_data.width = width;
    g_bg_image_data.height = height;
    g_bg_image_data.pixels = pixels;
    g_desktop.bg_image = &g_bg_image_data;

    kprintf("[Desktop] Generated %ux%u gradient background\n", width, height);
}

// Load background image from filesystem
static void load_background_image(void) {
    // Try to load background image from boot disk
    extern fat_fs_t g_fat_fs;
    static fat_fs_t boot_fs;
    fat_fs_t *fs_to_use = NULL;

    // First try the globally mounted filesystem
    if (g_fat_fs.mounted) {
        fs_to_use = &g_fat_fs;
        kprintf("[Desktop] Using globally mounted FAT filesystem\n");
    }

    // If not mounted, try mounting the boot disk's GPT partition
    // Boot disk is typically on primary master (drive 0), and GPT EFI partition starts at LBA 2048
    if (!fs_to_use) {
        kprintf("[Desktop] Trying to mount boot disk GPT partition...\n");
        if (fat_mount_lba(0, 2048, &boot_fs) == 0) {
            fs_to_use = &boot_fs;
            kprintf("[Desktop] Successfully mounted boot disk\n");
        } else {
            // Also try secondary master (drive 1)
            if (fat_mount_lba(1, 2048, &boot_fs) == 0) {
                fs_to_use = &boot_fs;
                kprintf("[Desktop] Mounted from drive 1\n");
            } else {
                // Try raw mount at LBA 0 (in case it's a raw FAT disk)
                if (fat_mount_lba(0, 0, &boot_fs) == 0) {
                    fs_to_use = &boot_fs;
                }
            }
        }
    }

    if (!fs_to_use) {
        kprintf("[Desktop] No FAT filesystem available, using gradient background\n");
        generate_gradient_background();
        return;
    }

    kprintf("[Desktop] Searching for background image...\n");

    // Try different possible filenames
    const char *bg_files[] = {"/BACK.BMP", "/BACKGROUND.BMP", "/BG.BMP", "BACK.BMP", NULL};

    for (int i = 0; bg_files[i] != NULL; i++) {
        kprintf("[Desktop] Trying %s...\n", bg_files[i]);
        uint32_t size = 0;
        void *data = fat_read_file(fs_to_use, bg_files[i], &size);
        kprintf("[Desktop] fat_read_file returned: data=%p size=%u\n", data, size);

        if (data && size > 54) {
            kprintf("[Desktop] Loading background from %s (%u bytes)...\n", bg_files[i], size);

            int result = image_load_bmp(data, size, &g_bg_image_data);
            if (result == IMAGE_SUCCESS) {
                // Convert to the desktop image structure format
                g_desktop.bg_image = (image_t *)&g_bg_image_data;
                kprintf("[Desktop] Background image loaded: %ux%u\n",
                        g_bg_image_data.width, g_bg_image_data.height);
                kfree(data);
                return;
            } else {
                kprintf("[Desktop] Failed to load BMP: %s\n", image_error_string(result));
            }
            kfree(data);
        }
    }

    kprintf("[Desktop] No background image found, using gradient\n");
    generate_gradient_background();
}

// ============================================================================
// Desktop Icons Implementation
// ============================================================================

// Calculate screen position for a desktop icon based on grid position
static void desktop_icon_get_position(int grid_x, int grid_y, int32_t *screen_x, int32_t *screen_y) {
    *screen_x = DESKTOP_ICON_MARGIN_X + grid_x * DESKTOP_ICON_SPACING_X;
    *screen_y = DESKTOP_ICON_MARGIN_Y + grid_y * DESKTOP_ICON_SPACING_Y;
}

// Add a desktop icon
int desktop_add_icon(const char *name, int icon_id, int grid_x, int grid_y, void (*launch)(void)) {
    if (g_desktop.icon_count >= DESKTOP_ICON_MAX) {
        kprintf("[Desktop] Cannot add icon: maximum reached\n");
        return -1;
    }

    // Find an empty slot
    for (int i = 0; i < DESKTOP_ICON_MAX; i++) {
        if (!g_desktop.icons[i].active) {
            desktop_icon_t *icon = &g_desktop.icons[i];
            strncpy(icon->name, name, DESKTOP_ICON_NAME_LEN - 1);
            icon->name[DESKTOP_ICON_NAME_LEN - 1] = '\0';
            icon->icon_id = icon_id;
            icon->grid_x = grid_x;
            icon->grid_y = grid_y;
            icon->launch = launch;
            icon->active = true;
            icon->selected = false;
            g_desktop.icon_count++;
            kprintf("[Desktop] Added icon: %s at grid (%d, %d)\n", name, grid_x, grid_y);
            return i;
        }
    }
    return -1;
}

// Remove a desktop icon by index
void desktop_remove_icon(int index) {
    if (index < 0 || index >= DESKTOP_ICON_MAX) return;
    if (!g_desktop.icons[index].active) return;

    g_desktop.icons[index].active = false;
    g_desktop.icon_count--;
}

// Draw a single desktop icon with label
static void draw_desktop_icon(desktop_icon_t *icon) {
    if (!icon->active) return;

    int32_t x, y;
    desktop_icon_get_position(icon->grid_x, icon->grid_y, &x, &y);

    // Draw selection highlight if selected
    if (icon->selected) {
        fb_fill_rect(x - 4, y - 4, DESKTOP_ICON_SIZE + 8,
                     DESKTOP_ICON_SIZE + DESKTOP_ICON_LABEL_H + 8,
                     argb_to_fb(0x40FFFFFF));
    }

    // Draw the icon (scaled to DESKTOP_ICON_SIZE)
    if (icon->icon_id >= 0) {
        icon_draw_scaled(icon->icon_id, x, y, DESKTOP_ICON_SIZE, 0xFFFFFF);
    } else {
        // Fallback: draw a colored rectangle
        fb_fill_rect(x, y, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, argb_to_fb(0xFF808080));
    }

    // Draw icon label below the icon (centered, with shadow for readability)
    int label_len = strlen(icon->name);
    int label_width = label_len * FONT_WIDTH;
    int label_x = x + (DESKTOP_ICON_SIZE - label_width) / 2;
    int label_y = y + DESKTOP_ICON_SIZE + 4;

    // Clamp label_x to keep it on screen
    if (label_x < 2) label_x = 2;

    // Draw text shadow for readability
    draw_string(label_x + 1, label_y + 1, icon->name, argb_to_fb(0xFF000000));
    // Draw text
    draw_string(label_x, label_y, icon->name, argb_to_fb(0xFFFFFFFF));
}

// Draw all desktop icons
void desktop_draw_icons(void) {
    for (int i = 0; i < DESKTOP_ICON_MAX; i++) {
        if (g_desktop.icons[i].active) {
            draw_desktop_icon(&g_desktop.icons[i]);
        }
    }
}

// Get desktop icon at screen coordinates
int desktop_get_icon_at(int x, int y) {
    for (int i = 0; i < DESKTOP_ICON_MAX; i++) {
        if (!g_desktop.icons[i].active) continue;

        int32_t icon_x, icon_y;
        desktop_icon_get_position(g_desktop.icons[i].grid_x,
                                   g_desktop.icons[i].grid_y,
                                   &icon_x, &icon_y);

        // Check if point is within icon bounds (including label area)
        int total_height = DESKTOP_ICON_SIZE + DESKTOP_ICON_LABEL_H;
        if (x >= icon_x && x < icon_x + DESKTOP_ICON_SIZE &&
            y >= icon_y && y < icon_y + total_height) {
            return i;
        }
    }
    return -1;
}

// Handle double-click on desktop icon
void desktop_icon_activate(int index) {
    if (index < 0 || index >= DESKTOP_ICON_MAX) return;
    if (!g_desktop.icons[index].active) return;

    desktop_icon_t *icon = &g_desktop.icons[index];
    kprintf("[Desktop] Activating icon: %s\n", icon->name);

    if (icon->launch) {
        icon->launch();
    }
}

// Initialize default desktop icons
static void desktop_init_default_icons(void) {
    // Clear any existing icons
    for (int i = 0; i < DESKTOP_ICON_MAX; i++) {
        g_desktop.icons[i].active = false;
    }
    g_desktop.icon_count = 0;

    // Add default desktop icons
    extern void filebrowser_launch(void);
    extern void recyclebin_launch(void);
    extern void terminal_launch(void);
    extern void settings_launch(void);

    desktop_add_icon("Computer",     ICON_FOLDER,    0, 0, filebrowser_launch);
    desktop_add_icon("Recycle Bin",  ICON_TRASH,     0, 1, recyclebin_launch);
    desktop_add_icon("Terminal",     ICON_TERMINAL,  0, 2, terminal_launch);
    desktop_add_icon("Settings",     ICON_COG,       0, 3, settings_launch);
    desktop_add_icon("DOOM",         ICON_GAME_DOOM, 0, 4, doom_launch);

    kprintf("[Desktop] Initialized %u default desktop icons\n", g_desktop.icon_count);
}

void desktop_init(void) {
    LOG_INFO("[Desktop] Initializing desktop manager");
    kprintf("[Desktop] g_start_menu_open at init start: %d\n", g_start_menu_open);
    g_start_menu_open = false;  // Start menu closed by default
    // Initialize TrueType font rendering
    ttf_init();
    ttf_selfcheck_digits();  // #302: verify digits 0-9 (esp. '7') render OK

    init_start_menu_items();  // Initialize menu items at runtime
    kprintf("[Desktop] Initializing desktop manager...\n");

    // Initialize the theme system
    theme_init();

    // Clear desktop state
    memset(&g_desktop, 0, sizeof(desktop_t));
    memset(&g_bg_image_data, 0, sizeof(g_bg_image_data));

    // Get screen dimensions from framebuffer
    g_desktop.screen_width = fb_get_width();
    g_desktop.screen_height = fb_get_height();
    kprintf("[Desktop] Screen resolution: %ux%u\n", g_desktop.screen_width, g_desktop.screen_height);

    // Set default background color
    g_desktop.bg_color = DESKTOP_BG_COLOR;
    g_desktop.bg_image = NULL;

    // Try to load background image
    load_background_image();

    // Initialize dock
    g_desktop.dock.app_count = 0;
    g_desktop.dock.hover_index = -1;
    g_desktop.dock.visible = true;

    // Calculate dock position (must be done after screen dimensions are set)
    dock_recalculate();

    // Dock apps are added dynamically when launched from start menu
    // and removed when the application closes

    // Initialize desktop icons
    desktop_init_default_icons();

    // Context menu no longer needs Applications submenu (now has direct items)


    // Pre-initialize menu category item counts
    for (int c = 0; g_start_menu_categories[c].name || g_start_menu_categories[c].is_separator; c++) {
        if (g_start_menu_categories[c].items) {
            g_start_menu_categories[c].item_count = count_menu_items(g_start_menu_categories[c].items);
            kprintf("[Desktop] Menu category %d item_count=%d\n", c, g_start_menu_categories[c].item_count);
        }
    }
    g_desktop.initialized = true;
    kprintf("[Desktop] g_start_menu_open at init end: %d\n", g_start_menu_open);

    kprintf("[Desktop] Desktop initialized: %ux%u, %u dock apps\n",
            g_desktop.screen_width, g_desktop.screen_height, g_desktop.dock.app_count);
}

void desktop_draw(void) {
    if (!g_desktop.initialized) {
        kprintf("[Desktop] Error: desktop not initialized\n");
        return;
    }

    // Draw background
    if (g_bg_image_data.pixels != NULL && g_bg_image_data.width > 0) {
        // Draw background image scaled to fill the entire screen
        image_blit_scaled(&g_bg_image_data, 0, 0,
                         g_desktop.screen_width, g_desktop.screen_height);
    } else if (g_desktop.bg_image && g_desktop.bg_image->pixels) {
        // Draw background image scaled to fill (legacy path)
        image_blit_scaled(g_desktop.bg_image, 0, 0,
                         g_desktop.screen_width, g_desktop.screen_height);
    } else {
        // Draw solid color background
        uint32_t fb_color = argb_to_fb(g_desktop.bg_color);
        fb_clear(fb_color);
    }

    // Draw desktop icons (before dock so they appear behind it)
    desktop_draw_icons();

    // Draw dock
    if (g_desktop.dock.visible) {
        dock_draw();
    }

    // Note: Start menu is now drawn AFTER windows (see desktop_run)
    // so it appears on top of all windows

    // Note: Context menu is now drawn AFTER windows (see desktop_run)
    // so it appears on top of all windows

    // Draw wallpaper picker if open (drawn directly, not using window manager)
    if (g_wallpaper_picker_open) {
        draw_wallpaper_picker();
    }

    // Draw screensaver settings if open
    if (g_ss_settings_open) {
        draw_screensaver_settings();
    }

    // Draw clock widget in top-right corner
    desktop_clock_draw();

    // Note: Version info moved to desktop_run() main loop so it appears on top of windows
}

void desktop_set_background_color(uint32_t color) {
    g_desktop.bg_color = color;
    kprintf("[Desktop] Background color set to 0x%08X\n", color);
}

void desktop_set_background_image(image_t *img) {
    g_desktop.bg_image = img;
    if (img) {
        kprintf("[Desktop] Background image set: %ux%u\n", img->width, img->height);
    } else {
        kprintf("[Desktop] Background image cleared\n");
    }
}

void desktop_handle_click(int x, int y) {
    if (!g_desktop.initialized) return;

    // Check if screensaver settings is open first
    if (g_ss_settings_open) {
        // Check if click is on the dialog
        if (x >= g_ss_settings_x && x < g_ss_settings_x + SS_SETTINGS_WIDTH &&
            y >= g_ss_settings_y && y < g_ss_settings_y + SS_SETTINGS_HEIGHT) {

            int item = get_ss_settings_item_at(x, y);
            if (item == -2) {
                // Close button clicked
                close_screensaver_settings();
                return;
            }
            if (item >= 0 && item < SCREENSAVER_COUNT) {
                // Type selected
                kprintf("[Desktop] Screensaver type selected: %s\n",
                        screensaver_get_type_name((screensaver_type_t)item));
                screensaver_set_type((screensaver_type_t)item);
                if (item == SCREENSAVER_NONE) {
                    screensaver_set_enabled(false);
                } else {
                    screensaver_set_enabled(true);
                }
                wm_invalidate_all();
                return;
            }
            if (item >= 10 && item < 20) {
                // Timeout selected
                int timeout_idx = item - 10;
                uint32_t timeout = g_ss_timeouts[timeout_idx];
                kprintf("[Desktop] Screensaver timeout selected: %s\n", g_ss_timeout_names[timeout_idx]);
                if (timeout == 0) {
                    screensaver_set_enabled(false);
                } else {
                    screensaver_set_timeout(timeout);
                    screensaver_set_enabled(true);
                }
                wm_invalidate_all();
                return;
            }
            // Click on dialog but not on item - do nothing
            return;
        }
        // Click outside dialog - close it
        close_screensaver_settings();
        return;
    }

    // Check if wallpaper picker is open first
    if (g_wallpaper_picker_open) {
        // Check if click is on the picker
        if (x >= g_picker_x && x < g_picker_x + PICKER_WIDTH &&
            y >= g_picker_y && y < g_picker_y + PICKER_HEIGHT) {

            int item = get_wallpaper_item_at(x, y);
            if (item == -2) {
                // Close button clicked
                close_wallpaper_picker();
                return;
            }
            if (item == -3) {
                // Scroll up
                if (g_picker_scroll > 0) {
                    g_picker_scroll--;
                    wm_invalidate_all();
                }
                return;
            }
            if (item == -4) {
                // Scroll down
                if (g_picker_scroll < g_picker_max_scroll) {
                    g_picker_scroll++;
                    wm_invalidate_all();
                }
                return;
            }
            if (item >= 0) {
                // Wallpaper selected
                kprintf("[Desktop] Wallpaper selected: %s\n", g_wallpapers[item].name);
                g_current_wallpaper_index = item;  // Track current selection
                set_wallpaper(g_wallpapers[item].filename);
                close_wallpaper_picker();
                return;
            }
            // Click on picker but not on item - do nothing
            return;
        }
        // Click outside picker - close it
        close_wallpaper_picker();
        return;
    }

    // Check if context menu is open
    if (g_context_menu_open) {
        int item = get_context_menu_item_at(x, y);
        if (item == -2) {
            // Submenu item clicked - g_clicked_item has the index
            menu_item_t *submenu = g_context_items[g_context_submenu_open].submenu;
            if (submenu && submenu[g_clicked_item].name) {
                kprintf("[Desktop] Submenu item clicked: %s\n", submenu[g_clicked_item].name);
                g_context_menu_open = false;
                g_context_submenu_open = -1;
                wm_invalidate_all();
                if (submenu[g_clicked_item].action) {
                    submenu[g_clicked_item].action();
                }
            }
            return;
        }
        if (item >= 0) {
            // Main menu item clicked
            if (g_context_items[item].submenu) {
                // Has submenu - toggle it open (already handled by get_context_menu_item_at)
                wm_invalidate_all();
                return;
            }
            kprintf("[Desktop] Context menu item clicked: %s\n", g_context_items[item].name);
            g_context_menu_open = false;
            g_context_submenu_open = -1;
            if (g_context_items[item].action) {
                g_context_items[item].action();
            }
            wm_invalidate_all();
            return;
        }
        // Click outside menu - close it
        g_context_menu_open = false;
        g_context_submenu_open = -1;
        wm_invalidate_all();
        return;
    }

    // Check if start menu is open and click is on menu item
    if (g_start_menu_open) {
        int menu_item = get_start_menu_item_at(x, y);
        if (menu_item >= 0 && g_clicked_category >= 0) {
            menu_item_t *item = &g_start_menu_categories[g_clicked_category].items[g_clicked_item];
            kprintf("[Desktop] Menu item clicked: %s (cat %d, item %d)\n",
                   item->name, g_clicked_category, g_clicked_item);

            g_start_menu_open = false;
            wm_invalidate_all();  // Close menu before launching

            // Handle wallpaper picker specially
            if (item->icon_id == ICON_PALETTE && item->action == NULL) {
                open_wallpaper_picker();
                return;
            }

            if (item->userspace_path && item->userspace_path[0]) {
                kprintf("[Desktop] Launching userspace: %s -> %s\n",
                        item->name, item->userspace_path);
                int pid = launch_userspace_app(item->userspace_path);
                if (pid <= 0 && item->action) {
                    kprintf("[Desktop] Userspace failed, kernel fallback\n");
                    item->action();
                }
            } else if (item->action) {
                kprintf("[Desktop] Calling action for: %s\n", item->name);
                for (volatile int i = 0; i < 100000; i++);
                item->action();
                kprintf("[Desktop] Action returned for: %s\n", item->name);
            }
            return;
        }
        // Check if we clicked a category header (accordion toggle)
        if (g_category_header_clicked) {
            g_category_header_clicked = false;  // Reset flag
            // Don't close - just toggled a category
            return;
        }
        // Click outside menu - close it
        g_start_menu_open = false;
        wm_invalidate_all();
        return;
    }

    // Check if click is on start button
    if (is_start_button_click(x, y)) {
        kprintf("[Desktop] Start button clicked\n");
        g_start_menu_open = !g_start_menu_open;
        wm_invalidate_all();
        return;
    }

    // Check if click is on dock app
    int app_index = dock_get_app_at(x, y);
    if (app_index >= 0) {
        dock_app_t *app = &g_desktop.dock.apps[app_index];
        kprintf("[Desktop] Dock app clicked: %s\n", app->name);

        if (app->launch) {
            app->launch();
        } else {
            kprintf("[Desktop] No launch handler for %s\n", app->name);
        }
        return;
    }

    // Check if click is on a desktop icon (double-click to activate)
    static uint64_t last_icon_click_time = 0;
    static int last_icon_clicked = -1;
    extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;  // Current timer frequency

    int icon_index = desktop_get_icon_at(x, y);
    if (icon_index >= 0) {
        // Deselect all icons first
        for (int i = 0; i < DESKTOP_ICON_MAX; i++) {
            g_desktop.icons[i].selected = false;
        }

        // Select the clicked icon
        g_desktop.icons[icon_index].selected = true;

        // Check for double-click (within 500ms at 1000Hz)
        uint64_t current_time = timer_ticks;
        if (icon_index == last_icon_clicked && (current_time - last_icon_click_time) < (g_timer_hz / 2)) {
            // Double-click - activate the icon
            desktop_icon_activate(icon_index);
            g_desktop.icons[icon_index].selected = false;
            last_icon_clicked = -1;
        } else {
            // Single click - just select
            last_icon_click_time = current_time;
            last_icon_clicked = icon_index;
        }
        wm_invalidate_all();
        return;
    }

    // Clicked on empty desktop - deselect all icons
    for (int i = 0; i < DESKTOP_ICON_MAX; i++) {
        g_desktop.icons[i].selected = false;
    }
    wm_invalidate_all();
}

// Calculate context menu height accounting for separators
static int calc_context_menu_height(void) {
    int height = 8;  // Padding
    for (int i = 0; g_context_items[i].name; i++) {
        if (strcmp(g_context_items[i].name, "---") == 0) {
            height += CONTEXT_MENU_SEP_H;
        } else {
            height += CONTEXT_MENU_ITEM_H;
        }
    }
    return height;
}

// Handle right-click on desktop (context menu)
void desktop_handle_right_click(int x, int y) {
    if (!g_desktop.initialized) return;

    // Close any open menus
    g_start_menu_open = false;
    close_wallpaper_picker();
    close_screensaver_settings();

    // Don't show context menu if clicking on dock
    if (dock_hit_test(x, y)) {
        return;
    }

    // Open context menu at click position
    g_context_menu_open = true;
    g_context_submenu_open = -1;  // Reset submenu state
    g_context_menu_x = x;
    g_context_menu_y = y;

    // Make sure menu doesn't go off screen
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    int menu_height = calc_context_menu_height();

    // Clamp X position
    if (g_context_menu_x + CONTEXT_MENU_WIDTH > (int32_t)screen_w) {
        g_context_menu_x = screen_w - CONTEXT_MENU_WIDTH;
    }
    if (g_context_menu_x < 0) {
        g_context_menu_x = 0;
    }

    // Clamp Y position
    if (g_context_menu_y + menu_height > (int32_t)screen_h - TASKBAR_HEIGHT) {
        g_context_menu_y = screen_h - TASKBAR_HEIGHT - menu_height;
    }
    if (g_context_menu_y < 0) {
        g_context_menu_y = 0;
    }

    kprintf("[Desktop] Context menu opened at (%d, %d)\n", g_context_menu_x, g_context_menu_y);
    wm_invalidate_all();
}

void desktop_handle_mouse_move(int x, int y) {
    if (!g_desktop.initialized) return;

    // Handle context menu hover (for submenu expansion)
    if (g_context_menu_open) {
        int old_submenu = g_context_submenu_open;
        get_context_menu_item_at(x, y);  // This updates g_context_submenu_open
        if (old_submenu != g_context_submenu_open) {
            wm_invalidate_all();
        }
    }

    int old_hover = g_desktop.dock.hover_index;
    g_desktop.dock.hover_index = dock_get_app_at(x, y);

    // Invalidate dock area if hover state changed
    if (old_hover != g_desktop.dock.hover_index) {
        rect_t dock_rect = {
            g_desktop.dock.x,
            g_desktop.dock.y,
            (int32_t)g_desktop.dock.width,
            (int32_t)g_desktop.dock.height
        };
        wm_invalidate_rect(&dock_rect);
    }
}

// ============================================================================
// Dock API Implementation
// ============================================================================

int dock_add_app_with_icon(const char *name, uint32_t color, int icon_id, void (*launch)(void)) {
    dock_t *dock = &g_desktop.dock;

    if (dock->app_count >= DOCK_MAX_APPS) {
        kprintf("[Dock] Error: dock is full\n");
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
        return -1;
    }

    // Find first empty slot
    int index = -1;
    for (int i = 0; i < DOCK_MAX_APPS; i++) {
        if (!dock->apps[i].active) {
            index = i;
            break;
        }
    }

    if (index < 0) {
        kprintf("[Dock] Error: no free slots\n");
        return -1;
    }

    // Initialize the app entry
    dock_app_t *app = &dock->apps[index];
    memset(app, 0, sizeof(dock_app_t));

    strncpy(app->name, name, DOCK_APP_NAME_LEN - 1);
    app->name[DOCK_APP_NAME_LEN - 1] = '\0';
    app->icon_color = color;
    app->icon_id = icon_id;
    app->launch = launch;
    app->active = true;

    dock->app_count++;

    // Recalculate dock layout
    dock_recalculate();

    kprintf("[Dock] Added app: %s (index %d, icon %d)\n", name, index, icon_id);
    return index;
}

// Backward compatible function - auto-assigns icon based on name
int dock_add_app(const char *name, uint32_t color, void (*launch)(void)) {
    int icon_id = -1;

    // Auto-assign icons based on app name
    if (strcmp(name, "Terminal") == 0) {
        icon_id = ICON_TERMINAL;
    } else if (strcmp(name, "Editor") == 0) {
        icon_id = ICON_HIGHLIGHT;
    } else if (strcmp(name, "Files") == 0) {
        icon_id = ICON_FOLDER;
    } else if (strcmp(name, "Calculator") == 0) {
        icon_id = ICON_CALCULATOR;
    } else if (strcmp(name, "Settings") == 0) {
        icon_id = ICON_COG;
    } else if (strcmp(name, "About") == 0) {
        icon_id = ICON_INFO_CIRCLE;
    }

    return dock_add_app_with_icon(name, color, icon_id, launch);
}

void dock_remove_app(int index) {
    dock_t *dock = &g_desktop.dock;

    if (index < 0 || index >= DOCK_MAX_APPS) return;
    if (!dock->apps[index].active) return;
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }

    kprintf("[Dock] Removed app: %s\n", dock->apps[index].name);

    dock->apps[index].active = false;
    dock->app_count--;

    // Recalculate dock layout
    dock_recalculate();
}

void dock_clear(void) {
    dock_t *dock = &g_desktop.dock;

    for (int i = 0; i < DOCK_MAX_APPS; i++) {
        dock->apps[i].active = false;
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
    }
    dock->app_count = 0;
    dock->hover_index = -1;

    dock_recalculate();
    kprintf("[Dock] Cleared all apps\n");
}

// Power management functions
static void do_shutdown(void) {
    kprintf("[Desktop] Shutdown requested\n");
    // For QEMU: use ACPI shutdown (port 0x604)
    outw(0x604, 0x2000);
    // Fallback: halt the CPU
    __asm__ volatile("cli; hlt");
}

static void do_restart(void) {
    kprintf("[Desktop] Restart requested\n");
    // Use keyboard controller to reset
    outb(0x64, 0xFE);
    // Fallback: halt
    __asm__ volatile("cli; hlt");
}

// ============================================================================
// Enhanced Start Menu Drawing Functions
// ============================================================================

// Draw search bar at top of menu
static void draw_start_menu_search(int x, int y, int width) {
    // Search box background
    fb_fill_rect(x + 4, y + 4, width - 8, START_MENU_SEARCH_H - 8, argb_to_fb(0xFF404040));
    fb_draw_rect(x + 4, y + 4, width - 8, START_MENU_SEARCH_H - 8, argb_to_fb(0xFF606060));

    // Search icon (using ZOOM_IN as search icon)
    int icon_x = x + 10;
    int icon_y = y + (START_MENU_SEARCH_H - 12) / 2;
    icon_draw_scaled(ICON_ZOOM_IN, icon_x, icon_y, 12, 0x888888);

    // Search text or placeholder
    if (g_menu_search_len > 0) {
        draw_string(x + 28, y + (START_MENU_SEARCH_H - FONT_HEIGHT) / 2,
                   g_menu_search, argb_to_fb(0xFFE0E0E0));
    } else {
        draw_string(x + 28, y + (START_MENU_SEARCH_H - FONT_HEIGHT) / 2,
                   "Search...", argb_to_fb(0xFF707070));
    }

    // Close button (X) - draw as text
    draw_string(x + width - 20, y + (START_MENU_SEARCH_H - FONT_HEIGHT) / 2, "X", argb_to_fb(0xFF888888));
}

// Draw recent files section
static void draw_start_menu_recent(int x, int *y, int width) {
    if (!g_menu_config.show_recent || g_recent_file_count == 0) return;

    // Section header
    draw_string(x + START_MENU_PADDING, *y, "RECENT FILES", argb_to_fb(0xFF888888));
    *y += START_MENU_CAT_H;

    // Draw recent file items (max 5 shown)
    int shown = (g_recent_file_count > 5) ? 5 : g_recent_file_count;
    for (int i = 0; i < shown; i++) {
        bool hover = (g_menu_hover_recent == i);

        // Item background
        uint32_t bg_color = hover ? 0xFF4A4A4A : 0xFF383838;
        fb_fill_rect(x + 4, *y, width - 8, START_MENU_RECENT_H - 2, argb_to_fb(bg_color));

        // File icon (based on extension)
        int icon_id = ICON_FILE;
        const char *ext = strrchr(g_recent_files[i].name, '.');
        if (ext) {
            if (strcmp(ext, ".txt") == 0 || strcmp(ext, ".TXT") == 0) icon_id = ICON_FILE;
            else if (strcmp(ext, ".c") == 0 || strcmp(ext, ".C") == 0) icon_id = ICON_HIGHLIGHT;
            else if (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0) icon_id = ICON_IMAGE;
        }
        icon_draw_scaled(icon_id, x + 8, *y + 2, 16, 0xDDDDDD);

        // File name (truncated if needed)
        char truncated_name[24];
        strncpy(truncated_name, g_recent_files[i].name, 20);
        truncated_name[20] = '\0';
        if (strlen(g_recent_files[i].name) > 20) {
            strcat(truncated_name, "...");
        }
        draw_string(x + 30, *y + (START_MENU_RECENT_H - FONT_HEIGHT) / 2,
                   truncated_name, argb_to_fb(0xFFD0D0D0));

        // Time ago
        char time_str[16];
        format_relative_time(g_recent_files[i].timestamp, time_str, sizeof(time_str));
        int time_w = strlen(time_str) * FONT_WIDTH;
        draw_string(x + width - time_w - 8, *y + (START_MENU_RECENT_H - FONT_HEIGHT) / 2,
                   time_str, argb_to_fb(0xFF707070));

        *y += START_MENU_RECENT_H;
    }

    // Separator after recent files
    fb_fill_rect(x + 12, *y + START_MENU_SEP_H / 2 - 1, width - 24, 1, argb_to_fb(0xFF505050));
    *y += START_MENU_SEP_H;
}

// Draw items in grid view (4 columns)
static void draw_start_menu_grid(int x, int *y, int width, menu_item_t *items, int count) {
    int col = 0;
    int start_x = x + (width - START_MENU_GRID_COLS * START_MENU_GRID_ITEM_W) / 2;

    for (int i = 0; i < count; i++) {
        if (!items[i].name) break;

        int item_x = start_x + col * START_MENU_GRID_ITEM_W;
        int item_y = *y;

        // Check hover state
        bool hover = (g_menu_hover_item == i);
        if (hover) {
            fb_fill_rect(item_x, item_y, START_MENU_GRID_ITEM_W - 4,
                        START_MENU_GRID_ITEM_H - 4, argb_to_fb(0xFF4A4A4A));
        }

        // Draw icon (32x32 centered)
        int icon_x = item_x + (START_MENU_GRID_ITEM_W - 32) / 2;
        int icon_y = item_y + 8;
        if (items[i].icon_id >= 0) {
            icon_draw_scaled(items[i].icon_id, icon_x, icon_y, 32, 0xFFFFFF);
        }

        // Draw name below icon (centered, truncated)
        char short_name[9];
        strncpy(short_name, items[i].name, 8);
        short_name[8] = '\0';
        if (strlen(items[i].name) > 8) {
            short_name[7] = '.';
            short_name[8] = '.';
            short_name[8] = '\0';
        }
        int name_w = strlen(short_name) * FONT_WIDTH;
        int name_x = item_x + (START_MENU_GRID_ITEM_W - name_w) / 2;
        draw_string(name_x, item_y + 48, short_name, argb_to_fb(0xFFD0D0D0));

        col++;
        if (col >= START_MENU_GRID_COLS) {
            col = 0;
            *y += START_MENU_GRID_ITEM_H;
        }
    }

    // Finish last row if not complete
    if (col > 0) {
        *y += START_MENU_GRID_ITEM_H;
    }
}

// Draw items in list view (icons + text)
static void draw_start_menu_list(int x, int *y, int width, menu_item_t *items, int count) {
    for (int i = 0; i < count; i++) {
        if (!items[i].name) break;

        bool hover = (g_menu_hover_item == i);
        uint32_t bg_color = hover ? 0xFF4A4A4A : 0xFF383838;

        // Item background
        fb_fill_rect(x + 4, *y, width - 8, START_MENU_ITEM_H - 2, argb_to_fb(bg_color));

        // Draw icon
        if (items[i].icon_id >= 0) {
            int icon_size = 18;
            int icon_x = x + 10;
            int icon_y = *y + (START_MENU_ITEM_H - icon_size) / 2;
            icon_draw_scaled(items[i].icon_id, icon_x, icon_y, icon_size, 0xDDDDDD);
        }

        // Item name
        draw_string(x + 36, *y + (START_MENU_ITEM_H - FONT_HEIGHT) / 2,
                   items[i].name, argb_to_fb(0xFFE0E0E0));

        *y += START_MENU_ITEM_H;
    }
}

// Draw cascading submenu for a category (appears to the right)
__attribute__((unused)) static void draw_cascading_submenu(int cat_index) {
    if (cat_index < 0 || !g_start_menu_categories[cat_index].items) return;

    int item_count = count_menu_items(g_start_menu_categories[cat_index].items);
    if (item_count == 0) return;

    int submenu_height = item_count * START_MENU_ITEM_H + START_MENU_PADDING * 2;

    // Draw submenu background (shadow effect)
    fb_fill_rect(g_submenu_x + 2, g_submenu_y + 2, SUBMENU_WIDTH, submenu_height, argb_to_fb(0xFF1A1A1A));
    fb_fill_rect(g_submenu_x, g_submenu_y, SUBMENU_WIDTH, submenu_height, argb_to_fb(0xFF2D2D2D));
    fb_draw_rect(g_submenu_x, g_submenu_y, SUBMENU_WIDTH, submenu_height, argb_to_fb(0xFF606060));

    // Draw items
    int y = g_submenu_y + START_MENU_PADDING;
    menu_item_t *items = g_start_menu_categories[cat_index].items;
    for (int i = 0; i < item_count; i++) {
        bool hover = (g_menu_hover_item == i && g_submenu_category == cat_index);
        uint32_t bg_color = hover ? 0xFF4A4A4A : 0xFF383838;

        fb_fill_rect(g_submenu_x + 4, y, SUBMENU_WIDTH - 8, START_MENU_ITEM_H - 2, argb_to_fb(bg_color));

        if (items[i].icon_id >= 0) {
            icon_draw_scaled(items[i].icon_id, g_submenu_x + 10, y + 3, 16, 0xDDDDDD);
        }
        draw_string(g_submenu_x + 32, y + (START_MENU_ITEM_H - FONT_HEIGHT) / 2,
                   items[i].name, argb_to_fb(0xFFE0E0E0));

        y += START_MENU_ITEM_H;
    }
}

// Draw the enhanced start menu with all features
static void draw_start_menu(void) {
    if (!g_start_menu_open) return;

    dock_t *dock = &g_desktop.dock;
    int menu_height = g_menu_config.height;
    
    // CRITICAL FIX: If height is 0 or invalid, use default
    if (menu_height <= 0 || menu_height > 800) {
        kprintf("[StartMenu] WARNING: Invalid menu_height=%d, using default 450\n", menu_height);
        menu_height = 450;
        g_menu_config.height = 450;  // Fix the corrupted config
    }
    
    int menu_width = START_MENU_WIDTH;

    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
    int menu_x = dock->x + TASKBAR_PADDING;
    int menu_y = dock->y - menu_height - 4;
    if (menu_y < 0) { kprintf("[StartMenu] ERROR: menu_y=%d is negative, fixing\n", menu_y); menu_y = 10; }
    kprintf("[StartMenu] Drawing: dock_y=%d, menu_height=%d, menu_y=%d, menu_x=%d\n", dock->y, menu_height, menu_y, menu_x);

    // Initialize category item counts
    for (int c = 0; g_start_menu_categories[c].name || g_start_menu_categories[c].is_separator; c++) {
        if (g_start_menu_categories[c].items) {
            g_start_menu_categories[c].item_count = count_menu_items(g_start_menu_categories[c].items);
        }
    }

    // Draw menu background with shadow effect
    fb_fill_rect(menu_x + 3, menu_y + 3, menu_width, menu_height, argb_to_fb(0xFF1A1A1A));  // Shadow
    fb_fill_rect(menu_x, menu_y, menu_width, menu_height, argb_to_fb(0xFF2D2D2D));
    fb_draw_rect(menu_x, menu_y, menu_width, menu_height, argb_to_fb(0xFF606060));

    int y = menu_y;

    // Draw search bar at top (if enabled)
    if (g_menu_config.show_search) {
        draw_start_menu_search(menu_x, y, menu_width);
        y += START_MENU_SEARCH_H;
    }

    // Draw recent files section
    draw_start_menu_recent(menu_x, &y, menu_width);

    // APPLICATIONS - Accordion header (collapses when Games/System is opened)
    bool apps_expanded = (g_expanded_category == 0);
    uint32_t apps_header_bg = apps_expanded ? 0xFF404050 : 0xFF2D2D2D;
    fb_fill_rect(menu_x + 4, y + 2, menu_width - 8, START_MENU_CAT_H - 4, argb_to_fb(apps_header_bg));
    draw_string(menu_x + START_MENU_PADDING, y + (START_MENU_CAT_H - FONT_HEIGHT) / 2,
               "APPLICATIONS", argb_to_fb(0xFF888888));
    draw_string(menu_x + menu_width - 16, y + (START_MENU_CAT_H - FONT_HEIGHT) / 2,
               apps_expanded ? "v" : ">", argb_to_fb(0xFF888888));
    y += START_MENU_CAT_H;

    // Draw applications only when expanded
    if (apps_expanded) {
        if (g_menu_config.view == VIEW_GRID) {
            { int c = count_menu_items(g_apps_items); kprintf("[StartMenu] Drawing %d apps in grid view\n", c); draw_start_menu_grid(menu_x, &y, menu_width, g_apps_items, c); }
        } else {
            { int c = count_menu_items(g_apps_items); kprintf("[StartMenu] Drawing %d apps in list view\n", c); draw_start_menu_list(menu_x, &y, menu_width, g_apps_items, c); }
        }
    }

    // Separator
    fb_fill_rect(menu_x + 12, y + START_MENU_SEP_H / 2 - 1, menu_width - 24, 1, argb_to_fb(0xFF505050));
    y += START_MENU_SEP_H;

    // GAMES - Accordion header (click to expand)
    bool games_expanded = (g_expanded_category == 1);
    uint32_t games_header_bg = games_expanded ? 0xFF404050 : 0xFF2D2D2D;
    fb_fill_rect(menu_x + 4, y + 2, menu_width - 8, START_MENU_CAT_H - 4, argb_to_fb(games_header_bg));
    draw_string(menu_x + START_MENU_PADDING, y + (START_MENU_CAT_H - FONT_HEIGHT) / 2,
               "GAMES", argb_to_fb(0xFF888888));
    draw_string(menu_x + menu_width - 16, y + (START_MENU_CAT_H - FONT_HEIGHT) / 2,
               games_expanded ? "v" : ">", argb_to_fb(0xFF888888));
    y += START_MENU_CAT_H;
    
    // If Games expanded, show games grid
    if (games_expanded) {
        int c = count_menu_items(g_games_items);
        draw_start_menu_grid(menu_x, &y, menu_width, g_games_items, c);
    }

    // SYSTEM - Accordion header (click to expand)
    bool system_expanded = (g_expanded_category == 2);
    uint32_t system_header_bg = system_expanded ? 0xFF404050 : 0xFF2D2D2D;
    fb_fill_rect(menu_x + 4, y + 2, menu_width - 8, START_MENU_CAT_H - 4, argb_to_fb(system_header_bg));
    draw_string(menu_x + START_MENU_PADDING, y + (START_MENU_CAT_H - FONT_HEIGHT) / 2,
               "SYSTEM", argb_to_fb(0xFF888888));
    draw_string(menu_x + menu_width - 16, y + (START_MENU_CAT_H - FONT_HEIGHT) / 2,
               system_expanded ? "v" : ">", argb_to_fb(0xFF888888));
    y += START_MENU_CAT_H;
    
    // If System expanded, show system grid
    if (system_expanded) {
        int c = count_menu_items(g_system_items);
        draw_start_menu_grid(menu_x, &y, menu_width, g_system_items, c);
    }

    // Separator before power section
    fb_fill_rect(menu_x + 12, y + START_MENU_SEP_H / 2 - 1, menu_width - 24, 1, argb_to_fb(0xFF505050));
    y += START_MENU_SEP_H;

    // Power buttons at bottom (horizontal layout)
    int power_y = menu_y + menu_height - START_MENU_POWER_H; kprintf("[StartMenu] power_y=%d\n", power_y);
    int btn_width = (menu_width - 24) / 2;

    // Restart button
    bool restart_hover = (g_menu_hover_category == 4 && g_menu_hover_item == 0);
    uint32_t restart_bg = restart_hover ? 0xFF4A4A4A : 0xFF383838;
    fb_fill_rect(menu_x + 8, power_y + 4, btn_width - 4, START_MENU_POWER_H - 8, argb_to_fb(restart_bg));
    icon_draw_scaled(ICON_REFRESH, menu_x + 16, power_y + 10, 16, 0xDDDDDD);
    draw_string(menu_x + 38, power_y + (START_MENU_POWER_H - FONT_HEIGHT) / 2, "Restart", argb_to_fb(0xFFD0D0D0));

    // Shutdown button
    bool shutdown_hover = (g_menu_hover_category == 4 && g_menu_hover_item == 1);
    uint32_t shutdown_bg = shutdown_hover ? 0xFF4A4A4A : 0xFF383838;
    fb_fill_rect(menu_x + btn_width + 12, power_y + 4, btn_width - 4, START_MENU_POWER_H - 8, argb_to_fb(shutdown_bg));
    icon_draw_scaled(ICON_POWER, menu_x + btn_width + 20, power_y + 10, 16, 0xE06060);
    draw_string(menu_x + btn_width + 42, power_y + (START_MENU_POWER_H - FONT_HEIGHT) / 2, "Shutdown", argb_to_fb(0xFFD0D0D0));

    // Accordion menus - no cascading submenu needed
}

// Get menu item at position for enhanced start menu
// Sets g_clicked_category and g_clicked_item for use by click handler
// Also handles hover state updates and cascading submenu triggers
static int get_start_menu_item_at(int x, int y) {
    if (!g_start_menu_open) return -1;

    dock_t *dock = &g_desktop.dock;
    int menu_height = g_menu_config.height;
    int menu_width = START_MENU_WIDTH;
    int menu_x = dock->x + TASKBAR_PADDING;
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
    int menu_y = dock->y - menu_height - 4;
    if (menu_y < 0) { kprintf("[StartMenu] ERROR: menu_y=%d is negative, fixing\n", menu_y); menu_y = 10; }
    kprintf("[StartMenu] Drawing: dock_y=%d, menu_height=%d, menu_y=%d, menu_x=%d\n", dock->y, menu_height, menu_y, menu_x);

    // Reset hover state
    g_menu_hover_category = -1;
    g_menu_hover_item = -1;
    g_menu_hover_recent = -1;

    // First check if click is in cascading submenu
    if (g_submenu_category >= 0) {
        int item_count = count_menu_items(g_start_menu_categories[g_submenu_category].items);
        int submenu_height = item_count * START_MENU_ITEM_H + START_MENU_PADDING * 2;

        if (x >= g_submenu_x && x < g_submenu_x + SUBMENU_WIDTH &&
            y >= g_submenu_y && y < g_submenu_y + submenu_height) {

            int rel_y = y - g_submenu_y - START_MENU_PADDING;
            int item_idx = rel_y / START_MENU_ITEM_H;
            if (item_idx >= 0 && item_idx < item_count) {
                g_clicked_category = g_submenu_category;
                g_clicked_item = item_idx;
                return item_idx;
            }
        }
    }

    // Check if outside main menu bounds
    if (x < menu_x || x >= menu_x + menu_width) {
        g_submenu_category = -1;  // Close submenu
        return -1;
    }
    if (y < menu_y || y >= menu_y + menu_height) return -1;

    int cur_y = menu_y;

    // Search bar area
    if (g_menu_config.show_search) {
        if (y >= cur_y && y < cur_y + START_MENU_SEARCH_H) {
            // Check if close button clicked
            if (x >= menu_x + menu_width - 28 && x < menu_x + menu_width - 4) {
                g_start_menu_open = false;
                return -1;
            }
            // Search area clicked - could focus search
            g_menu_search_focused = true;
            return -1;
        }
        cur_y += START_MENU_SEARCH_H;
    }

    // Recent files section
    if (g_menu_config.show_recent && g_recent_file_count > 0) {
        cur_y += START_MENU_CAT_H;  // Header

        int shown = (g_recent_file_count > 5) ? 5 : g_recent_file_count;
        for (int i = 0; i < shown; i++) {
            if (y >= cur_y && y < cur_y + START_MENU_RECENT_H) {
                g_menu_hover_recent = i;
                // Click opens the recent file (would need file opener)
                kprintf("[StartMenu] Recent file clicked: %s\n", g_recent_files[i].path);
                return -1;  // Handled specially
            }
            cur_y += START_MENU_RECENT_H;
        }
        cur_y += START_MENU_SEP_H;  // Separator
    }

    // Applications header - click to toggle accordion
    if (y >= cur_y && y < cur_y + START_MENU_CAT_H) {
        g_expanded_category = (g_expanded_category == 0) ? -1 : 0;
        g_category_header_clicked = true;
        wm_invalidate_all();
        return -1;
    }
    cur_y += START_MENU_CAT_H;

    // Applications items (only when expanded)
    if (g_expanded_category == 0) {
        int apps_count = count_menu_items(g_apps_items);
        if (g_menu_config.view == VIEW_GRID) {
            // Grid view hit testing
            int rows = (apps_count + START_MENU_GRID_COLS - 1) / START_MENU_GRID_COLS;
            int grid_height = rows * START_MENU_GRID_ITEM_H;
            int start_grid_x = menu_x + (menu_width - START_MENU_GRID_COLS * START_MENU_GRID_ITEM_W) / 2;

            if (y >= cur_y && y < cur_y + grid_height) {
                int col = (x - start_grid_x) / START_MENU_GRID_ITEM_W;
                int row = (y - cur_y) / START_MENU_GRID_ITEM_H;

                if (col >= 0 && col < START_MENU_GRID_COLS) {
                    int idx = row * START_MENU_GRID_COLS + col;
                    if (idx >= 0 && idx < apps_count) {
                        g_clicked_category = 0;  // Applications
                        g_clicked_item = idx;
                        g_menu_hover_category = 0;
                        g_menu_hover_item = idx;
                        return idx;
                    }
                }
            }
            cur_y += grid_height;
        } else {
            // List view hit testing
            for (int i = 0; i < apps_count; i++) {
                if (y >= cur_y && y < cur_y + START_MENU_ITEM_H) {
                    g_clicked_category = 0;  // Applications
                    g_clicked_item = i;
                    g_menu_hover_category = 0;
                    g_menu_hover_item = i;
                    return i;
                }
                cur_y += START_MENU_ITEM_H;
            }
        }
    }

    // Separator
    cur_y += START_MENU_SEP_H;

    // Games category header - toggle accordion
    if (y >= cur_y && y < cur_y + START_MENU_CAT_H) {
        g_expanded_category = (g_expanded_category == 1) ? 0 : 1;  // Toggle Games
        g_category_header_clicked = true;  // Don't close menu
        wm_invalidate_all();
        return -1;
    }
    cur_y += START_MENU_CAT_H;
    
    // If Games is expanded, hit-test the games grid
    if (g_expanded_category == 1) {
        int games_count = count_menu_items(g_games_items);
        int games_rows = (games_count + START_MENU_GRID_COLS - 1) / START_MENU_GRID_COLS;
        int games_grid_h = games_rows * START_MENU_GRID_ITEM_H;
        int start_grid_x = menu_x + (menu_width - START_MENU_GRID_COLS * START_MENU_GRID_ITEM_W) / 2;

        if (y >= cur_y && y < cur_y + games_grid_h) {
            int col = (x - start_grid_x) / START_MENU_GRID_ITEM_W;
            int row = (y - cur_y) / START_MENU_GRID_ITEM_H;
            if (col >= 0 && col < START_MENU_GRID_COLS) {
                int idx = row * START_MENU_GRID_COLS + col;
                if (idx >= 0 && idx < games_count) {
                    g_clicked_category = 1;  // Games
                    g_clicked_item = idx;
                    g_menu_hover_category = 1;
                    g_menu_hover_item = idx;
                    return idx;
                }
            }
        }
        cur_y += games_grid_h;
    }

    // System category header - toggle accordion
    if (y >= cur_y && y < cur_y + START_MENU_CAT_H) {
        g_expanded_category = (g_expanded_category == 2) ? 0 : 2;  // Toggle System
        g_category_header_clicked = true;
        wm_invalidate_all();
        return -1;
    }
    cur_y += START_MENU_CAT_H;
    
    // System items (only if expanded)
    if (g_expanded_category == 2) {
        int system_count = count_menu_items(g_system_items);
        int rows = (system_count + START_MENU_GRID_COLS - 1) / START_MENU_GRID_COLS;
        int grid_height = rows * START_MENU_GRID_ITEM_H;
        int start_grid_x = menu_x + (menu_width - START_MENU_GRID_COLS * START_MENU_GRID_ITEM_W) / 2;

        if (y >= cur_y && y < cur_y + grid_height) {
            int col = (x - start_grid_x) / START_MENU_GRID_ITEM_W;
            int row = (y - cur_y) / START_MENU_GRID_ITEM_H;

            if (col >= 0 && col < START_MENU_GRID_COLS) {
                int idx = row * START_MENU_GRID_COLS + col;
                if (idx >= 0 && idx < system_count) {
                    g_clicked_category = 2;  // System
                    g_clicked_item = idx;
                    g_menu_hover_category = 2;
                    g_menu_hover_item = idx;
                    return idx;
                }
            }
        }
        cur_y += grid_height;
    }

    // Power buttons at bottom
    int power_y = menu_y + menu_height - START_MENU_POWER_H; kprintf("[StartMenu] power_y=%d\n", power_y);
    if (y >= power_y && y < power_y + START_MENU_POWER_H) {
        int btn_width = (menu_width - 24) / 2;

        // Restart button
        if (x >= menu_x + 8 && x < menu_x + 8 + btn_width) {
            g_clicked_category = 4;  // Power
            g_clicked_item = 0;      // Restart
            g_menu_hover_category = 4;
            g_menu_hover_item = 0;
            return 0;
        }

        // Shutdown button
        if (x >= menu_x + btn_width + 12 && x < menu_x + btn_width * 2 + 8) {
            g_clicked_category = 4;  // Power
            g_clicked_item = 1;      // Shutdown
            g_menu_hover_category = 4;
            g_menu_hover_item = 1;
            return 1;
        }
    }

    g_clicked_category = -1;
    g_clicked_item = -1;
    return -1;
}

// Check if click is on start button
static bool is_start_button_click(int x, int y) {
    dock_t *dock = &g_desktop.dock;
    // Match the start button position from dock_draw()
    int32_t start_size = TASKBAR_HEIGHT - 4;  // Same as in dock_draw
    int32_t start_x = dock->x + TASKBAR_PADDING;
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
    int32_t start_y = dock->y + 2;

    return (x >= start_x && x < start_x + start_size &&
            y >= start_y && y < start_y + start_size);
}

// ============================================================================
// Context Menu Implementation
// ============================================================================

static void draw_context_menu(void) {
    if (!g_context_menu_open) return;

    // Count items and calculate height (accounting for separators)
    int item_count = 0;
    int menu_height = 8;  // Padding
    while (g_context_items[item_count].name) {
        if (strcmp(g_context_items[item_count].name, "---") == 0) {
            menu_height += CONTEXT_MENU_SEP_H;  // Separator height
        } else {
            menu_height += CONTEXT_MENU_ITEM_H;  // Normal item height
        }
        item_count++;
    }

    kprintf("[ContextMenu] Drawing context menu with %d items at (%d,%d), height=%d\n",
            item_count, g_context_menu_x, g_context_menu_y, menu_height);
    for (int i = 0; i < item_count; i++) {
        kprintf("[ContextMenu] Item %d: '%s'\n", i, g_context_items[i].name);
    }

    // Draw menu background
    fb_fill_rect(g_context_menu_x, g_context_menu_y, CONTEXT_MENU_WIDTH, menu_height, argb_to_fb(0xFF303030));
    fb_draw_rect(g_context_menu_x, g_context_menu_y, CONTEXT_MENU_WIDTH, menu_height, argb_to_fb(0xFF606060));

    // Draw items
    int y = g_context_menu_y + 4;
    for (int i = 0; g_context_items[i].name; i++) {
        // Check for separator
        if (strcmp(g_context_items[i].name, "---") == 0) {
            // Draw separator line
            fb_fill_rect(g_context_menu_x + 8, y + CONTEXT_MENU_SEP_H / 2 - 1,
                        CONTEXT_MENU_WIDTH - 16, 1, argb_to_fb(0xFF505050));
            y += CONTEXT_MENU_SEP_H;
            continue;
        }

        // Highlight if this submenu is open
        if (i == g_context_submenu_open) {
            fb_fill_rect(g_context_menu_x + 2, y, CONTEXT_MENU_WIDTH - 4, CONTEXT_MENU_ITEM_H - 2, argb_to_fb(0xFF505050));
        }

        // Draw item name
        draw_string(g_context_menu_x + 10, y + (CONTEXT_MENU_ITEM_H - FONT_HEIGHT) / 2,
                   g_context_items[i].name, argb_to_fb(0xFFFFFFFF));

        // Draw arrow indicator if has submenu
        if (g_context_items[i].submenu) {
            draw_string(g_context_menu_x + CONTEXT_MENU_WIDTH - 16, y + (CONTEXT_MENU_ITEM_H - FONT_HEIGHT) / 2,
                       ">", argb_to_fb(0xFFAAAAAA));
        }

        y += CONTEXT_MENU_ITEM_H;
    }

    // Draw submenu if open
    if (g_context_submenu_open >= 0 && g_context_items[g_context_submenu_open].submenu) {
        menu_item_t *submenu = g_context_items[g_context_submenu_open].submenu;
        int sub_count = g_context_items[g_context_submenu_open].submenu_count;

        // Position submenu to the right of main menu
        int sub_x = g_context_menu_x + CONTEXT_MENU_WIDTH - 2;
        int sub_y = g_context_menu_y + 4 + g_context_submenu_open * CONTEXT_MENU_ITEM_H;
        int sub_height = sub_count * CONTEXT_MENU_ITEM_H + 8;

        // Adjust if submenu would go off screen
        uint32_t screen_w = fb_get_width();
        uint32_t screen_h = fb_get_height();
        if (sub_x + CONTEXT_MENU_WIDTH > (int32_t)screen_w) {
            sub_x = g_context_menu_x - CONTEXT_MENU_WIDTH + 2;  // Show on left side
        }
        if (sub_y + sub_height > (int32_t)screen_h - TASKBAR_HEIGHT) {
            sub_y = screen_h - TASKBAR_HEIGHT - sub_height;
        }

        // Draw submenu background
        fb_fill_rect(sub_x, sub_y, CONTEXT_MENU_WIDTH, sub_height, argb_to_fb(0xFF303030));
        fb_draw_rect(sub_x, sub_y, CONTEXT_MENU_WIDTH, sub_height, argb_to_fb(0xFF606060));

        // Draw submenu items
        int sub_item_y = sub_y + 4;
        for (int i = 0; i < sub_count && submenu[i].name; i++) {
            // Draw icon if available
            if (submenu[i].icon_id >= 0) {
                int icon_size = 16;
                int icon_x = sub_x + 6;
                int icon_y = sub_item_y + (CONTEXT_MENU_ITEM_H - icon_size) / 2;
                icon_draw_scaled(submenu[i].icon_id, icon_x, icon_y, icon_size, 0xDDDDDD);
            }

            // Draw item name
            draw_string(sub_x + 28, sub_item_y + (CONTEXT_MENU_ITEM_H - FONT_HEIGHT) / 2,
                       submenu[i].name, argb_to_fb(0xFFFFFFFF));

            sub_item_y += CONTEXT_MENU_ITEM_H;
        }
    }
}

// Returns: -1 = outside, -2 = on submenu item (sets g_clicked_item), >= 0 = main menu item index
static int get_context_menu_item_at(int x, int y) {
    if (!g_context_menu_open) return -1;

    int menu_height = calc_context_menu_height();

    // First check if we're in an open submenu
    if (g_context_submenu_open >= 0 && g_context_items[g_context_submenu_open].submenu) {
        menu_item_t *submenu = g_context_items[g_context_submenu_open].submenu;
        int sub_count = g_context_items[g_context_submenu_open].submenu_count;

        // Calculate Y offset for submenu item (accounting for separators before it)
        int submenu_y_offset = 4;
        for (int i = 0; i < g_context_submenu_open; i++) {
            if (strcmp(g_context_items[i].name, "---") == 0) {
                submenu_y_offset += CONTEXT_MENU_SEP_H;
            } else {
                submenu_y_offset += CONTEXT_MENU_ITEM_H;
            }
        }

        // Calculate submenu position (same as in draw)
        int sub_x = g_context_menu_x + CONTEXT_MENU_WIDTH - 2;
        int sub_y = g_context_menu_y + submenu_y_offset;
        int sub_height = sub_count * CONTEXT_MENU_ITEM_H + 8;

        // Adjust if off screen
        uint32_t screen_w = fb_get_width();
        uint32_t screen_h = fb_get_height();
        if (sub_x + CONTEXT_MENU_WIDTH > (int32_t)screen_w) {
            sub_x = g_context_menu_x - CONTEXT_MENU_WIDTH + 2;
        }
        if (sub_y + sub_height > (int32_t)screen_h - TASKBAR_HEIGHT) {
            sub_y = screen_h - TASKBAR_HEIGHT - sub_height;
        }

        // Check if click is in submenu
        if (x >= sub_x && x < sub_x + CONTEXT_MENU_WIDTH &&
            y >= sub_y && y < sub_y + sub_height) {

            int sub_item_y = sub_y + 4;
            for (int i = 0; i < sub_count && submenu[i].name; i++) {
                if (y >= sub_item_y && y < sub_item_y + CONTEXT_MENU_ITEM_H) {
                    g_clicked_item = i;  // Store which submenu item was clicked
                    return -2;  // Signal that a submenu item was clicked
                }
                sub_item_y += CONTEXT_MENU_ITEM_H;
            }
            return -1;  // In submenu area but not on item
        }
    }

    // Check main menu bounds
    if (x < g_context_menu_x || x >= g_context_menu_x + CONTEXT_MENU_WIDTH) {
        // If outside main menu and not in submenu, close submenu
        g_context_submenu_open = -1;
        return -1;
    }
    if (y < g_context_menu_y || y >= g_context_menu_y + menu_height) {
        return -1;
    }

    // Find which main menu item (accounting for separators)
    int item_y = g_context_menu_y + 4;
    for (int i = 0; g_context_items[i].name; i++) {
        // Check for separator
        if (strcmp(g_context_items[i].name, "---") == 0) {
            item_y += CONTEXT_MENU_SEP_H;
            continue;  // Can't click on separator
        }

        if (y >= item_y && y < item_y + CONTEXT_MENU_ITEM_H) {
            // Open/close submenu on hover
            if (g_context_items[i].submenu) {
                g_context_submenu_open = i;
            } else {
                g_context_submenu_open = -1;
            }
            return i;
        }
        item_y += CONTEXT_MENU_ITEM_H;
    }

    return -1;
}

// ============================================================================
// Wallpaper Picker Implementation
// ============================================================================

/**
 * Draw a thumbnail image at the specified position
 */
static void draw_thumbnail(int index, int x, int y, bool selected) {
    if (index < 0 || index >= MAX_WALLPAPERS) return;

    thumbnail_t *thumb = &g_thumbnails[index];

    // Draw selection border if selected (currently set wallpaper)
    if (selected) {
        // Draw highlight border around the cell
        fb_fill_rect(x - 2, y - 2, THUMB_WIDTH + 4, THUMB_HEIGHT + 4, argb_to_fb(0xFF4080FF));
    }

    // Draw thumbnail background (dark gray if no thumbnail loaded)
    fb_fill_rect(x, y, THUMB_WIDTH, THUMB_HEIGHT, argb_to_fb(0xFF3A3A3A));

    if (thumb->loaded && thumb->pixels) {
        // Center the thumbnail if it's smaller than the cell
        int offset_x = (THUMB_WIDTH - thumb->width) / 2;
        int offset_y = (THUMB_HEIGHT - thumb->height) / 2;

        // Draw thumbnail pixels directly to framebuffer
        for (uint32_t py = 0; py < thumb->height; py++) {
            uint32_t *src_row = thumb->pixels + py * thumb->width;
            for (uint32_t px = 0; px < thumb->width; px++) {
                fb_put_pixel(x + offset_x + px, y + offset_y + py, src_row[px]);
            }
        }
    } else {
        // Draw "Loading..." text if not loaded
        draw_string_small(x + 8, y + THUMB_HEIGHT / 2 - 4, "Load...", argb_to_fb(0xFF808080));
    }

    // Draw border around thumbnail
    fb_draw_rect(x, y, THUMB_WIDTH, THUMB_HEIGHT, argb_to_fb(0xFF505050));
}

/**
 * Get a short display name for a wallpaper (truncate if needed)
 */
static void get_short_name(const char *name, char *buf, int max_len) {
    int len = 0;
    while (name[len]) len++;

    if (len <= max_len) {
        for (int i = 0; i <= len; i++) buf[i] = name[i];
    } else {
        // Truncate and add "..."
        for (int i = 0; i < max_len - 3; i++) buf[i] = name[i];
        buf[max_len - 3] = '.';
        buf[max_len - 2] = '.';
        buf[max_len - 1] = '.';
        buf[max_len] = '\0';
    }
}

static void draw_wallpaper_picker(void) {
    if (!g_wallpaper_picker_open) return;

    // Draw picker background (dark panel with border)
    fb_fill_rect(g_picker_x, g_picker_y, PICKER_WIDTH, PICKER_HEIGHT, argb_to_fb(0xFF2D2D2D));
    fb_draw_rect(g_picker_x, g_picker_y, PICKER_WIDTH, PICKER_HEIGHT, argb_to_fb(0xFF505050));

    // Draw title bar
    fb_fill_rect(g_picker_x + 2, g_picker_y + 2, PICKER_WIDTH - 4, 20, argb_to_fb(0xFF3A3A3A));
    draw_string(g_picker_x + 8, g_picker_y + 6, "Choose Wallpaper", argb_to_fb(0xFFE0E0E0));

    // Draw close button
    int close_x = g_picker_x + PICKER_WIDTH - 18;
    int close_y = g_picker_y + 4;
    fb_fill_rect(close_x, close_y, 14, 14, argb_to_fb(0xFF606060));
    draw_string(close_x + 3, close_y + 2, "X", argb_to_fb(0xFFFFFFFF));

    // Draw scroll indicators if needed
    if (g_picker_max_scroll > 0) {
        // Scroll up indicator
        if (g_picker_scroll > 0) {
            draw_string(g_picker_x + PICKER_WIDTH / 2 - 8, g_picker_y + 6, "^", argb_to_fb(0xFFFFFFFF));
        }
        // Scroll down indicator
        if (g_picker_scroll < g_picker_max_scroll) {
            draw_string(g_picker_x + PICKER_WIDTH / 2 - 8, g_picker_y + PICKER_HEIGHT - 12, "v", argb_to_fb(0xFFFFFFFF));
        }
    }

    // Content area
    int content_x = g_picker_x + THUMB_PADDING;
    int content_y = g_picker_y + PICKER_TITLE_H;

    // Count wallpapers
    int count = 0;
    for (int i = 0; g_wallpapers[i].name; i++) count++;

    // Draw thumbnails in grid
    int start_index = g_picker_scroll * THUMB_COLS;
    int visible_rows = PICKER_CONTENT_H / THUMB_CELL_H;
    int end_index = start_index + (visible_rows + 1) * THUMB_COLS;
    if (end_index > count) end_index = count;

    for (int i = start_index; i < end_index; i++) {
        int row = (i - start_index) / THUMB_COLS;
        int col = (i - start_index) % THUMB_COLS;

        int thumb_x = content_x + col * THUMB_CELL_W;
        int thumb_y = content_y + row * THUMB_CELL_H;

        // Skip if outside visible area
        if (thumb_y + THUMB_CELL_H > g_picker_y + PICKER_HEIGHT - 4) continue;

        // Check if this is the currently selected wallpaper
        bool selected = (i == g_current_wallpaper_index);

        // Draw the thumbnail
        draw_thumbnail(i, thumb_x, thumb_y, selected);

        // Draw name below thumbnail (using small font)
        char short_name[12];
        get_short_name(g_wallpapers[i].name, short_name, 11);
        int name_x = thumb_x + (THUMB_WIDTH - (int)strlen(short_name) * FONT_SMALL_WIDTH) / 2;
        draw_string_small(name_x, thumb_y + THUMB_HEIGHT + 2, short_name, argb_to_fb(0xFFCCCCCC));
    }
}

static int get_wallpaper_item_at(int x, int y) {
    if (!g_wallpaper_picker_open) return -1;

    // Check if click is on close button
    int close_x = g_picker_x + PICKER_WIDTH - 18;
    int close_y = g_picker_y + 4;
    if (x >= close_x && x < close_x + 14 && y >= close_y && y < close_y + 14) {
        return -2;  // Close button clicked
    }

    // Check scroll up area
    if (g_picker_scroll > 0 && y >= g_picker_y && y < g_picker_y + PICKER_TITLE_H) {
        if (x >= g_picker_x + PICKER_WIDTH / 2 - 20 && x < g_picker_x + PICKER_WIDTH / 2 + 20) {
            return -3;  // Scroll up
        }
    }

    // Check scroll down area
    if (g_picker_scroll < g_picker_max_scroll &&
        y >= g_picker_y + PICKER_HEIGHT - 16 && y < g_picker_y + PICKER_HEIGHT) {
        if (x >= g_picker_x + PICKER_WIDTH / 2 - 20 && x < g_picker_x + PICKER_WIDTH / 2 + 20) {
            return -4;  // Scroll down
        }
    }

    // Content area starts after title bar
    int content_x = g_picker_x + THUMB_PADDING;
    int content_y = g_picker_y + PICKER_TITLE_H;

    // Check if click is in content area
    if (x < content_x || x >= g_picker_x + PICKER_WIDTH - THUMB_PADDING ||
        y < content_y || y >= g_picker_y + PICKER_HEIGHT - 4) {
        return -1;  // Outside content area
    }

    // Calculate which grid cell was clicked
    int rel_x = x - content_x;
    int rel_y = y - content_y;

    int col = rel_x / THUMB_CELL_W;
    int row = rel_y / THUMB_CELL_H;

    // Check if click is within the thumbnail area (not in padding between cells)
    int cell_x = rel_x % THUMB_CELL_W;
    int cell_y = rel_y % THUMB_CELL_H;

    // Accept clicks on the thumbnail and the name label below it
    if (cell_x >= THUMB_CELL_W - THUMB_PADDING / 2 || cell_y >= THUMB_HEIGHT + 14) {
        return -1;  // Click in padding area
    }

    if (col < 0 || col >= THUMB_COLS) {
        return -1;  // Outside grid columns
    }

    // Calculate wallpaper index
    int index = (g_picker_scroll + row) * THUMB_COLS + col;

    // Count wallpapers to validate index
    int count = 0;
    for (int i = 0; g_wallpapers[i].name; i++) count++;

    if (index >= 0 && index < count) {
        return index;
    }

    return -1;
}

static void set_wallpaper(const char *filename) {
    kprintf("[Desktop] Setting wallpaper to: %s\n", filename ? filename : "(gradient)");

    // Free old background if exists
    if (g_bg_image_data.pixels) {
        kfree(g_bg_image_data.pixels);
        g_bg_image_data.pixels = NULL;
        g_desktop.bg_image = NULL;
    }

    // NULL filename means use gradient
    if (!filename) {
        generate_gradient_background();
        wm_invalidate_all();
        return;
    }

    // Get filesystem
    extern fat_fs_t g_fat_fs;
    fat_fs_t *fs_to_use = NULL;

    // Check if g_fat_fs is mounted
    if (g_fat_fs.bytes_per_sector > 0) {
        fs_to_use = &g_fat_fs;
    }

    if (!fs_to_use) {
        kprintf("[Desktop] No filesystem available\n");
        generate_gradient_background();
        return;
    }

    // Load the file from disk
    uint32_t size = 0;
    void *data = fat_read_file(fs_to_use, filename, &size);

    if (data && size > 54) {
        int result = image_load_bmp(data, size, &g_bg_image_data);
        if (result == IMAGE_SUCCESS) {
            g_desktop.bg_image = &g_bg_image_data;
            kprintf("[Desktop] Wallpaper loaded: %ux%u\n",
                    g_bg_image_data.width, g_bg_image_data.height);
            wm_invalidate_all();
        } else {
            kprintf("[Desktop] Failed to load BMP: %s\n", image_error_string(result));
            generate_gradient_background();
        }
        kfree(data);
    } else {
        kprintf("[Desktop] Failed to read file: %s\n", filename);
        generate_gradient_background();
    }
}

static void open_wallpaper_picker(void) {
    // Close context menu
    g_context_menu_open = false;

    // If already open, do nothing
    if (g_wallpaper_picker_open) {
        return;
    }

    // Center the picker on screen
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();
    g_picker_x = (screen_w - PICKER_WIDTH) / 2;
    g_picker_y = (screen_h - PICKER_HEIGHT) / 2;

    // Reset scroll position
    g_picker_scroll = 0;

    // Generate thumbnails if not already cached
    generate_thumbnails();

    g_wallpaper_picker_open = true;
    kprintf("[Desktop] Wallpaper picker opened at %d,%d (size %dx%d)\n",
            g_picker_x, g_picker_y, PICKER_WIDTH, PICKER_HEIGHT);
    wm_invalidate_all();
}

static void close_wallpaper_picker(void) {
    g_wallpaper_picker_open = false;
    wm_invalidate_all();
}

// ============================================================================
// Thumbnail Generation Functions
// ============================================================================

/**
 * Scale an image down to thumbnail size using nearest-neighbor sampling.
 * Uses integer math only - no floating point.
 *
 * @param src_pixels Source pixel data (BGRA format)
 * @param src_w Source width
 * @param src_h Source height
 * @param out_w Output: actual thumbnail width
 * @param out_h Output: actual thumbnail height
 * @return Allocated thumbnail pixel buffer, or NULL on failure
 */
static uint32_t *scale_image_to_thumbnail(uint32_t *src_pixels, uint32_t src_w, uint32_t src_h,
                                          uint32_t *out_w, uint32_t *out_h) {
    if (!src_pixels || src_w == 0 || src_h == 0) {
        return NULL;
    }

    // Calculate scaled dimensions maintaining aspect ratio
    // Use integer math: scale = min(THUMB_WIDTH/src_w, THUMB_HEIGHT/src_h)
    // To avoid division, we compare cross-products:
    // THUMB_WIDTH/src_w < THUMB_HEIGHT/src_h  =>  THUMB_WIDTH*src_h < THUMB_HEIGHT*src_w
    uint32_t thumb_w, thumb_h;

    if (THUMB_WIDTH * src_h < THUMB_HEIGHT * src_w) {
        // Width is the limiting factor
        thumb_w = THUMB_WIDTH;
        // thumb_h = src_h * THUMB_WIDTH / src_w
        thumb_h = (src_h * THUMB_WIDTH) / src_w;
        if (thumb_h == 0) thumb_h = 1;
    } else {
        // Height is the limiting factor
        thumb_h = THUMB_HEIGHT;
        // thumb_w = src_w * THUMB_HEIGHT / src_h
        thumb_w = (src_w * THUMB_HEIGHT) / src_h;
        if (thumb_w == 0) thumb_w = 1;
    }

    // Allocate thumbnail buffer
    uint32_t *thumb_pixels = (uint32_t *)kmalloc(thumb_w * thumb_h * sizeof(uint32_t));
    if (!thumb_pixels) {
        return NULL;
    }

    // Use 16.16 fixed-point for scaling ratios
    uint32_t x_ratio = (src_w << 16) / thumb_w;
    uint32_t y_ratio = (src_h << 16) / thumb_h;

    // Nearest-neighbor scaling
    for (uint32_t y = 0; y < thumb_h; y++) {
        uint32_t src_y = (y * y_ratio) >> 16;
        if (src_y >= src_h) src_y = src_h - 1;

        uint32_t *src_row = src_pixels + src_y * src_w;
        uint32_t *dst_row = thumb_pixels + y * thumb_w;

        for (uint32_t x = 0; x < thumb_w; x++) {
            uint32_t src_x = (x * x_ratio) >> 16;
            if (src_x >= src_w) src_x = src_w - 1;
            dst_row[x] = src_row[src_x];
        }
    }

    *out_w = thumb_w;
    *out_h = thumb_h;
    return thumb_pixels;
}

/**
 * Generate thumbnail for a single wallpaper
 */
static void generate_thumbnail(int index) {
    if (index < 0 || index >= MAX_WALLPAPERS || !g_wallpapers[index].name) {
        return;
    }

    // Already loaded?
    if (g_thumbnails[index].loaded) {
        return;
    }

    const char *filename = g_wallpapers[index].filename;

    // Handle gradient (no file) - create a simple gradient thumbnail
    if (!filename) {
        uint32_t *pixels = (uint32_t *)kmalloc(THUMB_WIDTH * THUMB_HEIGHT * sizeof(uint32_t));
        if (pixels) {
            // Create a blue gradient similar to the desktop gradient
            for (uint32_t y = 0; y < THUMB_HEIGHT; y++) {
                // Gradient from dark blue (top) to lighter blue (bottom)
                uint32_t ratio = (y << 8) / THUMB_HEIGHT;  // 0-255
                uint8_t r = (uint8_t)((40 * ratio) >> 8);
                uint8_t g = (uint8_t)((80 + (40 * ratio)) >> 8);
                uint8_t b = (uint8_t)((120 + (60 * ratio)) >> 8);
                uint32_t color = (b << 16) | (g << 8) | r;  // BGRA format for framebuffer

                for (uint32_t x = 0; x < THUMB_WIDTH; x++) {
                    pixels[y * THUMB_WIDTH + x] = color;
                }
            }
            g_thumbnails[index].pixels = pixels;
            g_thumbnails[index].width = THUMB_WIDTH;
            g_thumbnails[index].height = THUMB_HEIGHT;
            g_thumbnails[index].loaded = true;
            kprintf("[Thumb] Generated gradient thumbnail for index %d\n", index);
        }
        return;
    }

    // Get filesystem
    extern fat_fs_t g_fat_fs;
    fat_fs_t *fs = NULL;
    if (g_fat_fs.bytes_per_sector > 0) {
        fs = &g_fat_fs;
    }

    if (!fs) {
        kprintf("[Thumb] No filesystem for thumbnail %d\n", index);
        return;
    }

    // Load the BMP file
    uint32_t size = 0;
    void *data = fat_read_file(fs, filename, &size);

    if (!data || size <= 54) {
        kprintf("[Thumb] Failed to read %s\n", filename);
        if (data) kfree(data);
        return;
    }

    // Parse the BMP
    image_t img;
    int result = image_load_bmp(data, size, &img);
    kfree(data);  // Free raw file data

    if (result != IMAGE_SUCCESS) {
        kprintf("[Thumb] Failed to parse BMP %s: %s\n", filename, image_error_string(result));
        return;
    }

    // Scale to thumbnail size
    uint32_t thumb_w, thumb_h;
    uint32_t *thumb_pixels = scale_image_to_thumbnail(img.pixels, img.width, img.height,
                                                       &thumb_w, &thumb_h);

    // Free full-size image
    image_free(&img);

    if (thumb_pixels) {
        g_thumbnails[index].pixels = thumb_pixels;
        g_thumbnails[index].width = thumb_w;
        g_thumbnails[index].height = thumb_h;
        g_thumbnails[index].loaded = true;
        kprintf("[Thumb] Generated %ux%u thumbnail for %s\n", thumb_w, thumb_h, filename);
    } else {
        kprintf("[Thumb] Failed to scale thumbnail for %s\n", filename);
    }
}

/**
 * Generate thumbnails for all wallpapers (called when picker opens)
 */
static void generate_thumbnails(void) {
    if (!g_thumbnails_initialized) {
        // Initialize cache
        for (int i = 0; i < MAX_WALLPAPERS; i++) {
            g_thumbnails[i].pixels = NULL;
            g_thumbnails[i].width = 0;
            g_thumbnails[i].height = 0;
            g_thumbnails[i].loaded = false;
        }
        g_thumbnails_initialized = true;
    }

    // Count wallpapers
    int count = 0;
    for (int i = 0; g_wallpapers[i].name; i++) {
        count++;
    }

    kprintf("[Thumb] Generating thumbnails for %d wallpapers...\n", count);

    // Generate thumbnails for all wallpapers
    for (int i = 0; i < count && i < MAX_WALLPAPERS; i++) {
        generate_thumbnail(i);
    }

    // Calculate max scroll
    int rows = (count + THUMB_COLS - 1) / THUMB_COLS;  // Ceiling division
    int visible_rows = PICKER_CONTENT_H / THUMB_CELL_H;
    g_picker_max_scroll = rows - visible_rows;
    if (g_picker_max_scroll < 0) g_picker_max_scroll = 0;

    kprintf("[Thumb] Thumbnail generation complete. Rows: %d, Visible: %d, MaxScroll: %d\n",
            rows, visible_rows, g_picker_max_scroll);
}

/**
 * Free all cached thumbnails (for future cleanup support)
 */
__attribute__((unused))
static void free_thumbnails(void) {
    for (int i = 0; i < MAX_WALLPAPERS; i++) {
        if (g_thumbnails[i].pixels) {
            kfree(g_thumbnails[i].pixels);
            g_thumbnails[i].pixels = NULL;
        }
        g_thumbnails[i].width = 0;
        g_thumbnails[i].height = 0;
        g_thumbnails[i].loaded = false;
    }
    kprintf("[Thumb] Thumbnails freed\n");
}

// ============================================================================
// Screensaver Settings Implementation
// ============================================================================

static void draw_screensaver_settings(void) {
    if (!g_ss_settings_open) return;

    // Get current config
    screensaver_config_t *cfg = screensaver_get_config();

    // Draw dialog background
    fb_fill_rect(g_ss_settings_x, g_ss_settings_y, SS_SETTINGS_WIDTH, SS_SETTINGS_HEIGHT, argb_to_fb(0xFF2D2D2D));
    fb_draw_rect(g_ss_settings_x, g_ss_settings_y, SS_SETTINGS_WIDTH, SS_SETTINGS_HEIGHT, argb_to_fb(0xFF505050));

    // Draw title bar
    fb_fill_rect(g_ss_settings_x + 2, g_ss_settings_y + 2, SS_SETTINGS_WIDTH - 4, 20, argb_to_fb(0xFF3A3A3A));
    draw_string(g_ss_settings_x + 8, g_ss_settings_y + 6, "Screensaver Settings", argb_to_fb(0xFFE0E0E0));

    // Draw close button
    int close_x = g_ss_settings_x + SS_SETTINGS_WIDTH - 18;
    int close_y = g_ss_settings_y + 4;
    fb_fill_rect(close_x, close_y, 14, 14, argb_to_fb(0xFF606060));
    draw_string(close_x + 3, close_y + 2, "X", argb_to_fb(0xFFFFFFFF));

    int content_y = g_ss_settings_y + 28;

    // Section: Screensaver Type
    draw_string(g_ss_settings_x + 8, content_y, "Type:", argb_to_fb(0xFF888888));
    content_y += 18;

    // Draw type options
    for (int i = SCREENSAVER_NONE; i < SCREENSAVER_COUNT; i++) {
        const char *name = screensaver_get_type_name((screensaver_type_t)i);
        bool selected = (cfg->type == (screensaver_type_t)i);

        // Highlight selected item
        if (selected) {
            fb_fill_rect(g_ss_settings_x + 6, content_y, SS_SETTINGS_WIDTH - 12, SS_ITEM_H - 2, argb_to_fb(0xFF4060A0));
        } else {
            fb_fill_rect(g_ss_settings_x + 6, content_y, SS_SETTINGS_WIDTH - 12, SS_ITEM_H - 2, argb_to_fb(0xFF404040));
        }

        // Draw radio button indicator
        uint32_t radio_color = selected ? 0xFF00CC00 : 0xFF808080;
        fb_fill_rect(g_ss_settings_x + 12, content_y + 6, 8, 8, argb_to_fb(radio_color));

        draw_string(g_ss_settings_x + 26, content_y + (SS_ITEM_H - FONT_HEIGHT) / 2, name, argb_to_fb(0xFFFFFFFF));
        content_y += SS_ITEM_H;
    }

    content_y += 10;

    // Section: Timeout
    draw_string(g_ss_settings_x + 8, content_y, "Wait:", argb_to_fb(0xFF888888));
    content_y += 18;

    // Draw timeout options
    for (int i = 0; g_ss_timeout_names[i]; i++) {
        bool selected = (cfg->timeout_seconds == g_ss_timeouts[i]) ||
                       (g_ss_timeouts[i] == 0 && !cfg->enabled);

        // Highlight selected item
        if (selected) {
            fb_fill_rect(g_ss_settings_x + 6, content_y, SS_SETTINGS_WIDTH - 12, SS_ITEM_H - 2, argb_to_fb(0xFF4060A0));
        } else {
            fb_fill_rect(g_ss_settings_x + 6, content_y, SS_SETTINGS_WIDTH - 12, SS_ITEM_H - 2, argb_to_fb(0xFF404040));
        }

        // Draw radio button indicator
        uint32_t radio_color = selected ? 0xFF00CC00 : 0xFF808080;
        fb_fill_rect(g_ss_settings_x + 12, content_y + 6, 8, 8, argb_to_fb(radio_color));

        draw_string(g_ss_settings_x + 26, content_y + (SS_ITEM_H - FONT_HEIGHT) / 2, g_ss_timeout_names[i], argb_to_fb(0xFFFFFFFF));
        content_y += SS_ITEM_H;
    }
}

// Returns: -1 = outside, -2 = close button, 0-5 = type selection, 10-14 = timeout selection
static int get_ss_settings_item_at(int x, int y) {
    if (!g_ss_settings_open) return -1;

    // Check if click is on close button
    int close_x = g_ss_settings_x + SS_SETTINGS_WIDTH - 18;
    int close_y = g_ss_settings_y + 4;
    if (x >= close_x && x < close_x + 14 && y >= close_y && y < close_y + 14) {
        return -2;  // Close button clicked
    }

    // Check bounds
    if (x < g_ss_settings_x + 6 || x >= g_ss_settings_x + SS_SETTINGS_WIDTH - 6 ||
        y < g_ss_settings_y + 28 || y >= g_ss_settings_y + SS_SETTINGS_HEIGHT) {
        return -1;
    }

    int content_y = g_ss_settings_y + 28 + 18;  // After "Type:" label

    // Check type items
    for (int i = SCREENSAVER_NONE; i < SCREENSAVER_COUNT; i++) {
        if (y >= content_y && y < content_y + SS_ITEM_H) {
            return i;  // Return type index (0-5)
        }
        content_y += SS_ITEM_H;
    }

    content_y += 10 + 18;  // After gap and "Wait:" label

    // Check timeout items
    for (int i = 0; g_ss_timeout_names[i]; i++) {
        if (y >= content_y && y < content_y + SS_ITEM_H) {
            return 10 + i;  // Return 10+ for timeout selection
        }
        content_y += SS_ITEM_H;
    }

    return -1;
}

__attribute__((unused))
static void open_screensaver_settings(void) {
    // Close context menu
    g_context_menu_open = false;
    g_context_submenu_open = -1;

    // Close wallpaper picker if open
    g_wallpaper_picker_open = false;

    // Center on screen
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();
    g_ss_settings_x = (screen_w - SS_SETTINGS_WIDTH) / 2;
    g_ss_settings_y = (screen_h - SS_SETTINGS_HEIGHT) / 2;

    // Initialize selection from current config
    screensaver_config_t *cfg = screensaver_get_config();
    g_ss_selected_type = cfg->type;

    g_ss_settings_open = true;
    kprintf("[Desktop] Screensaver settings opened\n");
    wm_invalidate_all();
}

static void close_screensaver_settings(void) {
    g_ss_settings_open = false;
    wm_invalidate_all();
}

// Draw a gauge bar with label and value (uses regular 8x16 font for readability)
static void draw_gauge(int32_t x, int32_t y, int32_t width, int32_t height,
                       int percent, uint32_t color, const char *label, const char *value) {
    // Background
    fb_fill_rect(x, y, width, height, argb_to_fb(GAUGE_BG_COLOR));

    // Fill based on percentage
    int fill_width = (width * percent) / 100;
    if (fill_width > 0) {
        fb_fill_rect(x, y, fill_width, height, argb_to_fb(color));
    }

    // Border
    fb_draw_rect(x, y, width, height, argb_to_fb(0xFF606060));

    // Draw label on left, value on right (using regular font for better readability)
    int text_y = y + (height - FONT_HEIGHT) / 2;
    if (label) {
        draw_string(x + 3, text_y, label, argb_to_fb(0xFFFFFFFF));
    }
    if (value) {
        int value_width = strlen(value) * FONT_WIDTH;
        draw_string(x + width - value_width - 3, text_y, value, argb_to_fb(0xFFFFFFFF));
    }
}

void dock_draw(void) {
    dock_t *dock = &g_desktop.dock;
    if (!dock->visible) return;

    // Draw taskbar background (full width, square edges)
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
    fb_fill_rect(dock->x, dock->y, dock->width, dock->height, argb_to_fb(TASKBAR_BG_COLOR));

    // Draw top border line
    fb_fill_rect(dock->x, dock->y, dock->width, 1, argb_to_fb(0xFF606060));

    // ========== START BUTTON (with categories icon) ==========
    int32_t start_size = TASKBAR_HEIGHT - 4;  // Square button
    int32_t start_x = dock->x + TASKBAR_PADDING;
    int32_t start_y = dock->y + 2;

    // Start button background (square)
    fb_fill_rect(start_x, start_y, start_size, start_size, argb_to_fb(START_BTN_COLOR));

    // Draw categories icon (centered in button)
    int icon_size = 20;
    int icon_x = start_x + (start_size - icon_size) / 2;
    int icon_y = start_y + (start_size - icon_size) / 2;
    icon_draw_scaled(ICON_CATEGORIES, icon_x, icon_y, icon_size, 0xFFFFFF);

    // ========== APP BUTTONS (square with thumbnail) ==========
    int32_t app_x = dock->x + START_BTN_WIDTH + TASKBAR_PADDING;
    int32_t btn_y = dock->y + (TASKBAR_HEIGHT - PROCESS_BTN_SIZE) / 2;

    for (uint32_t i = 0; i < DOCK_MAX_APPS; i++) {
        dock_app_t *app = &dock->apps[i];
        if (!app->active) continue;

        bool hovered = ((int32_t)i == dock->hover_index);

        // Square app button background (acts as border/frame)
        uint32_t border_color = hovered ? TASKBAR_HOVER_COLOR : 0xFF303030;
        fb_fill_rect(app_x, btn_y, PROCESS_BTN_SIZE, PROCESS_BTN_SIZE, argb_to_fb(border_color));

        // Draw icon or colored square
        if (app->icon_id >= 0) {
            // Draw icon centered in button
            int icon_sz = 20;
            int ix = app_x + (PROCESS_BTN_SIZE - icon_sz) / 2;
            int iy = btn_y + (PROCESS_BTN_SIZE - icon_sz) / 2;
            icon_draw_scaled(app->icon_id, ix, iy, icon_sz, 0xFFFFFF);
        } else {
            // Fallback: colored square
            fb_fill_rect(app_x + 2, btn_y + 2, PROCESS_BTN_SIZE - 4, PROCESS_BTN_SIZE - 4,
                        argb_to_fb(app->icon_color));
        }

        // Update app position for hit testing
        app->x = app_x;
        app->y = btn_y;

        app_x += PROCESS_BTN_SIZE + TASKBAR_ICON_SPACING;
    }

    // ========== SYSTEM GAUGES (right side) ==========
    int32_t gauge_h = 22;  // Taller to fit regular 8x16 font
    int32_t gauge_x = dock->x + dock->width - (GAUGE_WIDTH * 4 + GAUGE_SPACING * 3 + TASKBAR_PADDING);
    int32_t gauge_y = dock->y + (TASKBAR_HEIGHT - gauge_h) / 2;
    char value_buf[16];

    // CPU gauge - real usage tracking
    int cpu_percent = cpu_get_usage();
    if (cpu_percent >= 100) {
        value_buf[0] = '1';
        value_buf[1] = '0';
        value_buf[2] = '0';
        value_buf[3] = '%';
        value_buf[4] = '\0';
    } else if (cpu_percent >= 10) {
        value_buf[0] = (cpu_percent / 10) + '0';
        value_buf[1] = (cpu_percent % 10) + '0';
        value_buf[2] = '%';
        value_buf[3] = '\0';
    } else {
        value_buf[0] = cpu_percent + '0';
        value_buf[1] = '%';
        value_buf[2] = '\0';
    }
    draw_gauge(gauge_x, gauge_y, GAUGE_WIDTH, gauge_h, cpu_percent, 0xFF00AA00, "CPU", value_buf);
    gauge_x += GAUGE_WIDTH + GAUGE_SPACING;

    // Memory gauge - use real memory stats
    uint64_t total_pages = pmm_get_total_pages();
    uint64_t used_pages = pmm_get_used_pages();
    int mem_percent = (total_pages > 0) ? (used_pages * 100 / total_pages) : 0;
    uint64_t used_mb = (used_pages * 4096) / (1024 * 1024);

    if (used_mb >= 100) {
        value_buf[0] = (used_mb / 100) + '0';
        value_buf[1] = ((used_mb / 10) % 10) + '0';
        value_buf[2] = (used_mb % 10) + '0';
        value_buf[3] = 'M';
        value_buf[4] = '\0';
    } else if (used_mb >= 10) {
        value_buf[0] = (used_mb / 10) + '0';
        value_buf[1] = (used_mb % 10) + '0';
        value_buf[2] = 'M';
        value_buf[3] = '\0';
    } else {
        value_buf[0] = used_mb + '0';
        value_buf[1] = 'M';
        value_buf[2] = '\0';
    }
    draw_gauge(gauge_x, gauge_y, GAUGE_WIDTH, gauge_h, mem_percent, 0xFF0088CC, "RAM", value_buf);
    gauge_x += GAUGE_WIDTH + GAUGE_SPACING;

    // Disk gauge - show actual filesystem usage
    int dsk_percent = 0;
    strcpy(value_buf, "N/A");
    if (g_fat_fs.mounted) {
        static uint32_t cached_free = 0;
        static uint64_t last_disk_check = 0;
        
        // Only recalculate every 5 seconds
        if (timer_ticks - last_disk_check >= 500 || cached_free == 0) {
            cached_free = fat_get_free_clusters(&g_fat_fs);
            last_disk_check = timer_ticks;
        }
        
        uint32_t total_clusters = g_fat_fs.cluster_count;
        uint32_t used_clusters = total_clusters - cached_free;
        dsk_percent = (total_clusters > 0) ? (used_clusters * 100 / total_clusters) : 0;
        
        uint32_t cluster_size = g_fat_fs.sectors_per_cluster * g_fat_fs.bytes_per_sector;
        uint32_t used_mb_disk = (used_clusters * cluster_size) / (1024 * 1024);
        if (used_mb_disk >= 100) {
            value_buf[0] = (used_mb_disk / 100) + '0';
            value_buf[1] = ((used_mb_disk / 10) % 10) + '0';
            value_buf[2] = (used_mb_disk % 10) + '0';
            value_buf[3] = 'M';
            value_buf[4] = '\0';
        } else if (used_mb_disk >= 10) {
            value_buf[0] = (used_mb_disk / 10) + '0';
            value_buf[1] = (used_mb_disk % 10) + '0';
            value_buf[2] = 'M';
            value_buf[3] = '\0';
        } else {
            value_buf[0] = used_mb_disk + '0';
            value_buf[1] = 'M';
            value_buf[2] = '\0';
        }
    }
    draw_gauge(gauge_x, gauge_y, GAUGE_WIDTH, gauge_h, dsk_percent, 0xFFCC8800, "DSK", value_buf);
    gauge_x += GAUGE_WIDTH + GAUGE_SPACING;

    // Network gauge - show link status
    int net_percent = 0;
    strcpy(value_buf, "Down");
    if (nic_link_up()) {
        net_percent = 50;
        strcpy(value_buf, "Up");
    }
    draw_gauge(gauge_x, gauge_y, GAUGE_WIDTH, gauge_h, net_percent, 0xFF8800CC, "NET", value_buf);
}

bool dock_hit_test(int x, int y) {
    dock_t *dock = &g_desktop.dock;

    return (x >= dock->x && x < (int32_t)(dock->x + dock->width) &&
            y >= dock->y && y < (int32_t)(dock->y + dock->height));
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
}

int dock_get_app_at(int x, int y) {
    dock_t *dock = &g_desktop.dock;

    // Quick bounds check for entire dock
    if (!dock_hit_test(x, y)) {
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
        return -1;
    }

    // Check each app button (square buttons)
    for (uint32_t i = 0; i < DOCK_MAX_APPS; i++) {
        dock_app_t *app = &dock->apps[i];
        if (!app->active) continue;

        if (x >= app->x && x < app->x + (int32_t)PROCESS_BTN_SIZE &&
            y >= app->y && y < app->y + (int32_t)PROCESS_BTN_SIZE) {
            return i;
        }
    }

    return -1;
}

desktop_t *desktop_get_state(void) {
    return &g_desktop;
}

// Draw mouse cursor at position (public API)
void desktop_draw_cursor(int32_t x, int32_t y) {
    // Simple arrow cursor (12x16 pixels)
    static const uint8_t cursor[] = {
        0b10000000, 0b00000000,
        0b11000000, 0b00000000,
        0b11100000, 0b00000000,
        0b11110000, 0b00000000,
        0b11111000, 0b00000000,
        0b11111100, 0b00000000,
        0b11111110, 0b00000000,
        0b11111111, 0b00000000,
        0b11111111, 0b10000000,
        0b11111111, 0b11000000,
        0b11111100, 0b00000000,
        0b11101100, 0b00000000,
        0b11000110, 0b00000000,
        0b10000110, 0b00000000,
        0b00000011, 0b00000000,
        0b00000011, 0b00000000,
    };

    uint32_t white = 0xFFFFFF;
    uint32_t black = 0x000000;

    for (int row = 0; row < 16; row++) {
        uint16_t bits = (cursor[row*2] << 8) | cursor[row*2 + 1];
        for (int col = 0; col < 12; col++) {
            if (bits & (0x8000 >> col)) {
                fb_put_pixel(x + col, y + row, white);
                // Add black outline
                fb_put_pixel(x + col + 1, y + row, black);
                fb_put_pixel(x + col, y + row + 1, black);
            }
        }
    }
}

// Main desktop event loop (new event-driven architecture)
// #317 pass 2: optional boot auto-launch. /CONFIG/AUTORUN.CFG holds one app
// path (e.g. "/APPS/files"); launched a few seconds after boot once the
// compositor has rendered. No-op when the file is absent (kiosk/test aid).
static void autorun_worker(void *arg) {
    (void)arg;
    extern void proc_sleep(uint32_t ms);
    proc_sleep(14000);
    uint32_t sz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/AUTORUN.CFG", &sz);
    if (!cfg) return;
    char path[128]; int i = 0;
    while (cfg[i] && cfg[i] != '\n' && cfg[i] != '\r' && i < 127) { path[i] = cfg[i]; i++; }
    path[i] = 0;
    kfree(cfg);
    if (path[0] == '/') launch_userspace_app(path);
}

void desktop_run(void) {
    if (!g_desktop.initialized) {
        kprintf("[Desktop] Error: desktop not initialized\n");
        return;
    }

    kprintf("[Desktop] Starting desktop environment...\n");

    // Auto-launch the userland compositor. While it starts we KEEP THE BOOT
    // SPLASH on screen (do not draw the kernel desktop) so the handoff to the
    // usermode compositor is seamless - no flash of the kernel taskbar/icons.
    // The kernel desktop is only drawn as a fallback if the compositor is absent.
    extern int g_compositor_launched;
    {
        // #430 verification gate: if /PTTEST.RUN exists on the FAT ESP, launch
        // the signals+pthreads test app INSTEAD of the compositor. This runs it
        // at the same steady-state-safe spawn site, but with no compositor (so
        // no AI-Chat pegging the CPU) - the test gets a clean core and its
        // "PTTEST:" output goes straight to the serial log. Marker absent on
        // normal boots => behavior is completely unchanged.
        {
            extern fat_fs_t g_fat_fs;
            uint32_t __pt_sz = 0;
            void *__pt_mark = fat_read_file(&g_fat_fs, "/PTTEST.RUN", &__pt_sz);
            if (__pt_mark) {
                kfree(__pt_mark);
                kprintf("[Desktop] #430 PTTEST.RUN present: running /APPS/PTTEST "
                        "instead of the compositor\n");
                int tpid = launch_userspace_app("/APPS/PTTEST");
                kprintf("[Desktop] #430 pttest launched pid=%d\n", tpid);
                // Fall through into the desktop loop with no compositor; the
                // test proc is scheduled normally and prints to serial.
                goto pttest_after_launch;
            }
        }
        int cpid = launch_userspace_app("/APPS/COMPOSIT");
        if (cpid > 0) {
            g_compositor_launched = 1;
            proc_create_ex("autorun", autorun_worker, 0, PRIO_LOW, 64*1024);
            kprintf("[Desktop] Compositor launched (pid %d); keeping splash until it renders\n", cpid);
        } else {
            kprintf("[Desktop] WARNING: compositor not found; falling back to kernel desktop\n");
            wm_invalidate_all();
            desktop_draw();
            wm_draw_all();
            wm_draw_apps();
            wm_draw_winmenu();   // Task A: decorator popup on top of app content
            extern void fb_swap_buffers(void);
            fb_swap_buffers();
        }
    }

pttest_after_launch:;   // #430: gate jumps here, skipping the compositor path

    // #95: start enabled+autostart background services now that the
    // compositor (the first user process) is up and the allocator is in
    // steady state. svc_init() already built the registry in main().
    {
        extern void svc_autostart(void);
        svc_autostart();
    }

    // Store last mouse position and button state
    int32_t last_mouse_x = 0, last_mouse_y = 0;
    uint8_t last_buttons = 0;
    mouse_get_position(&last_mouse_x, &last_mouse_y);

    // Initialize CPU tracking
    g_cpu_last_tick = timer_ticks;
    g_cpu_busy_ticks = 0;
    g_cpu_total_ticks = 0;

    // Main loop
    // Drain any stray scancodes left over from keyboard controller init
    // (e.g. 0xFA ACK, ESC, F12) that could trigger unwanted actions.
    {
        extern int keyboard_has_char(void);
        extern int keyboard_get_char(void);
        int drained = 0;
        while (keyboard_has_char()) {
            keyboard_get_char();
            drained++;
        }
        if (drained > 0) {
            kprintf("[Desktop] Drained %d stray boot scancodes\n", drained);
        }
    }

    int running = 1;
    while (running) {
        // Check if exclusive mode is active (DOOM fullscreen, etc.)
        // If so, yield CPU and skip desktop processing
        if (wm_is_exclusive_mode() || g_compositor_launched) {
            // The userland compositor owns the screen (or was just launched and is
            // about to render): idle, do NOT draw the kernel desktop, so the boot
            // splash persists seamlessly until the compositor's first frame (#268).
            // The userland compositor owns the screen. REALLY sleep (yield the
            // CPU) instead of busy-spinning a volatile delay loop - that loop ran
            // forever under the compositor and pegged pid 0 at ~100% (#180).
            extern void proc_sleep(uint32_t ms);
            proc_sleep(50);
            continue;
        }

        // Track this iteration for CPU usage
        g_cpu_total_ticks++;

        bool this_iteration_busy = false;

        // ========== 1. COLLECT INPUT (non-blocking) ==========

        // Track if there was any input this frame
        bool had_input = false;

        // Check for keyboard input - throttled to 20 Hz shared with desktop_process_tick
        uint32_t _kb_iv = (g_timer_hz >= 20) ? (g_timer_hz / 20) : 1;
        extern volatile int g_win16_owns_screen;
        if (!g_win16_owns_screen && (timer_ticks - s_last_kb_tick >= _kb_iv) && keyboard_has_char()) {
            s_last_kb_tick = timer_ticks;
            had_input = true;
            int c = keyboard_get_char();

            // Ignore ALL keypresses during first 2 seconds of boot.
            // The keyboard controller sends spurious scancodes (ESC, F12, etc.)
            // during initialization that can trigger unwanted actions.
            if (timer_ticks < 2 * g_timer_hz) {
                continue;
            }

            // F12 launches DOOM
            if (c == 0x86) {
                kprintf("[Desktop] F12 - launching DOOM\n");
                extern void doom_launch(void);
                doom_launch();
                continue;
            }

            // F11 toggles maximize of the focused window
            if (c == 0x85) {
                window_t *focused = window_get_focused();
                if (focused) {
                    if (focused->flags & WINDOW_FLAG_MAXIMIZED) {
                        window_restore(focused);
                    } else {
                        window_maximize(focused);
                    }
                    wm_invalidate_all();
                }
                continue;
            }

            // F6 launches Terminal (Phase J2 quick-test hotkey)
            if (c == 0x8A) {
                kprintf("[Desktop] F6 - launching Terminal (PTY+msh)\n");
                extern void terminal_launch(void);
                terminal_launch();
                continue;
            }

            // ESC exits desktop only when no window is focused
            if (c == 27 && window_get_focused() == NULL) {
                kprintf("[Desktop] Exit key pressed, exiting desktop\n");
                running = 0;
                continue;
            }

            // F1 or Space toggles start menu for testing
            if (c == 32 && window_get_focused() == NULL) { // Space key
                kprintf("[Desktop] Space pressed - toggling start menu\n");
                g_start_menu_open = !g_start_menu_open;
                wm_invalidate_all();
                continue;
            }

            // Queue key event for window manager
            gui_event_t key_event = {0};
            // Check if this is a key release:
            // 1. Special release codes (0x90-0x98): subtract 0x10 to get press code
            // 2. Regular ASCII with bit 7 set (>= 0x80): clear bit 7
            if (c >= 0x90 && c <= 0x98) {
                // Special key release (Ctrl, Shift, arrows)
                key_event.type = EVENT_KEY_UP;
                key_event.keycode = c - 0x10;
                key_event.key_char = 0;
            } else if (c >= 0x80 && c < 0x90) {
                // Special key press (arrows, ctrl, shift, etc.) - NOT a release
                key_event.type = EVENT_KEY_DOWN;
                key_event.keycode = c;
                key_event.key_char = 0;
            } else if (c > 0x98) {
                // Regular ASCII key release (ASCII | 0x80, range 0xA0-0xFF)
                key_event.type = EVENT_KEY_UP;
                key_event.keycode = c & 0x7F;  // Clear bit 7 to get ASCII
                key_event.key_char = 0;
            } else {
                // Regular key press (ASCII < 0x80)
                key_event.type = EVENT_KEY_DOWN;
                key_event.keycode = c;
                key_event.key_char = c;
            }
            wm_queue_event(&key_event);
        }

        // Poll mouse state
        mouse_poll();
        int32_t mouse_x, mouse_y;
        uint8_t buttons;
        mouse_get_position(&mouse_x, &mouse_y);
        buttons = mouse_get_buttons();

        // Queue mouse movement event
        if (mouse_x != last_mouse_x || mouse_y != last_mouse_y) {
            had_input = true;
            gui_event_t move_event = {0};
            move_event.type = EVENT_MOUSE_MOVE;
            move_event.mouse_x = mouse_x;
            move_event.mouse_y = mouse_y;
            move_event.mouse_buttons = buttons;
            wm_queue_event(&move_event);
        }

        // Queue mouse button down event
        if ((buttons & MOUSE_LEFT_BTN) && !(last_buttons & MOUSE_LEFT_BTN)) {
            had_input = true;
            gui_event_t down_event = {0};
            down_event.type = EVENT_MOUSE_DOWN;
            down_event.mouse_x = mouse_x;
            down_event.mouse_y = mouse_y;
            down_event.mouse_buttons = MOUSE_BUTTON_LEFT;
            wm_queue_event(&down_event);
        }

        // Queue right-click event
        if ((buttons & MOUSE_RIGHT_BTN) && !(last_buttons & MOUSE_RIGHT_BTN)) {
            gui_event_t down_event = {0};
            down_event.type = EVENT_MOUSE_DOWN;
            down_event.mouse_x = mouse_x;
            down_event.mouse_y = mouse_y;
            down_event.mouse_buttons = MOUSE_BUTTON_RIGHT;
            wm_queue_event(&down_event);
        }

        // Queue mouse button up event
        if (!(buttons & MOUSE_LEFT_BTN) && (last_buttons & MOUSE_LEFT_BTN)) {
            gui_event_t up_event = {0};
            up_event.type = EVENT_MOUSE_UP;
            up_event.mouse_x = mouse_x;
            up_event.mouse_y = mouse_y;
            up_event.mouse_buttons = MOUSE_BUTTON_LEFT;
            wm_queue_event(&up_event);
        }

        if (!(buttons & MOUSE_RIGHT_BTN) && (last_buttons & MOUSE_RIGHT_BTN)) {
            gui_event_t up_event = {0};
            up_event.type = EVENT_MOUSE_UP;
            up_event.mouse_x = mouse_x;
            up_event.mouse_y = mouse_y;
            up_event.mouse_buttons = MOUSE_BUTTON_RIGHT;
            wm_queue_event(&up_event);
        }

        // Queue mouse scroll event
        int8_t scroll = mouse_get_scroll();
        if (scroll != 0) {
            had_input = true;
            gui_event_t scroll_event = {0};
            scroll_event.type = EVENT_MOUSE_SCROLL;
            scroll_event.mouse_x = mouse_x;
            scroll_event.mouse_y = mouse_y;
            scroll_event.scroll_delta = scroll;
            wm_queue_event(&scroll_event);
        }

        // Mark iteration as busy if there was input
        if (had_input) {
            this_iteration_busy = true;
        }

        // ========== SCREENSAVER HANDLING ==========

        // Update screensaver idle tracking
        if (had_input) {
            screensaver_on_input();
        }
        screensaver_update();

        // If screensaver is active, draw it and skip normal processing
        if (screensaver_is_active()) {
            screensaver_draw();
            fb_swap_buffers();

            // Update tracking variables
            last_mouse_x = mouse_x;
            last_mouse_y = mouse_y;
            last_buttons = buttons;

            // Poll network even during screensaver (critical for connectivity)
            extern void net_poll(void);
            net_poll();
            // Sleep instead of proc_yield(): yield keeps this proc READY so it is
            // re-selected immediately (a busy spin). ~30 FPS is plenty for the saver.
            extern void proc_sleep(uint32_t ms); proc_sleep(33);
            continue;
        }

        // ========== 2. PROCESS EVENTS AND DISPATCH TO APPS ==========

        // Process queued events through window manager
        gui_event_t event;
        while (wm_poll_event(&event)) {
            switch (event.type) {
                case EVENT_MOUSE_MOVE:
                    // Let WM handle dragging/resizing
                    wm_handle_mouse_move(event.mouse_x, event.mouse_y);
                    // Update desktop hover states
                    desktop_handle_mouse_move(event.mouse_x, event.mouse_y);
                    break;

                case EVENT_MOUSE_DOWN:
                    // First check if click is on a window
                    if (window_get_at_point(event.mouse_x, event.mouse_y)) {
                        wm_handle_mouse_down(event.mouse_x, event.mouse_y, event.mouse_buttons);
                    } else {
                        // Click is on desktop
                        if (event.mouse_buttons & MOUSE_BUTTON_LEFT) {
                            desktop_handle_click(event.mouse_x, event.mouse_y);
                        } else if (event.mouse_buttons & MOUSE_BUTTON_RIGHT) {
                            desktop_handle_right_click(event.mouse_x, event.mouse_y);
                        }
                    }
                    break;
                case EVENT_MOUSE_UP:
                    wm_handle_mouse_up(event.mouse_x, event.mouse_y, event.mouse_buttons);
                    break;

                case EVENT_KEY_DOWN:
                    wm_handle_key_down(event.keycode, event.key_char);
                    break;

                case EVENT_KEY_UP:
                    wm_handle_key_up(event.keycode);
                    break;

                default:
                    break;
            }

            // Dispatch event to registered apps
            wm_dispatch_event(&event);
        }

        // ========== 3. RENDER IF DIRTY ==========

        // Check if we need to redraw (and mark iteration as busy if so)
        bool needs_redraw = wm_is_dirty();
        if (needs_redraw) {
            this_iteration_busy = true;
        }

        if (needs_redraw) {
            // For now, do full redraw (dirty rect optimization can come later)
            // Draw desktop background and dock
            desktop_draw();

            // Draw all windows (back to front)
            wm_draw_all();

            // Draw app-specific content inside windows
            wm_draw_apps();
            wm_draw_winmenu();   // Task A: decorator popup on top of app content

            // Draw version info in bottom-right corner (after windows, before context menu)
            char version_text[128];
            snprintf(version_text, sizeof(version_text), "v%s Build %d",
                     MAYTERA_VERSION_STRING, MAYTERA_BUILD_NUMBER);
            int version_x = g_desktop.screen_width - (strlen(version_text) * FONT_WIDTH) - 10;
            int version_y = g_desktop.screen_height - FONT_HEIGHT - 5;
            draw_string(version_x + 1, version_y + 1, version_text, argb_to_fb(0xFF000000)); // Shadow
            draw_string(version_x, version_y, version_text, argb_to_fb(0xFFCCCCCC)); // Light gray text

            // Draw start menu AFTER windows so it appears on top
            if (g_start_menu_open) {
                draw_start_menu();
            }

            // Draw context menu AFTER windows so it appears on top
            if (g_context_menu_open) {
                draw_context_menu();
            }

            // Draw cursor on top (in back buffer)
            desktop_draw_cursor(mouse_x, mouse_y);

            // Always do a full swap to avoid dirty-rect race conditions with
            // the idle process (desktop_process_tick) clearing the dirty flag.
            extern void fb_swap_buffers(void);
            fb_swap_buffers();

            // Clear dirty state
            wm_clear_dirty();
        }

        // Update tracking variables
        last_mouse_x = mouse_x;
        last_mouse_y = mouse_y;
        last_buttons = buttons;

        // Poll network for incoming packets
        extern void net_poll(void);
        net_poll();
        extern void proc_yield(void); proc_yield();  // Allow kernel threads to run

        // ========== CPU USAGE TRACKING ==========

        // Track busy ticks if this iteration did work
        if (this_iteration_busy) {
            g_cpu_busy_ticks++;
        }

        // Update CPU usage calculation (~1 second intervals)
        cpu_update_usage();

        // Periodic taskbar refresh to update gauges (every ~120 iterations)
        static uint32_t refresh_counter = 0;
        refresh_counter++;
        if (refresh_counter >= 120) {
            refresh_counter = 0;
            // Only invalidate taskbar region, not entire screen
            rect_t taskbar_rect = {
                .x = 0,
                .y = (int32_t)(fb_get_height() - TASKBAR_HEIGHT),
                .width = (int32_t)fb_get_width(),
                .height = TASKBAR_HEIGHT
            };
            wm_invalidate_rect(&taskbar_rect);
        }

        // #102/#180: yield the CPU instead of busy-spinning on `pause` until
        // the next timer tick (that pegged a full core ~98%). proc_sleep lets the
        // idle proc HLT. Under the compositor the kernel desktop does no drawing
        // and the compositor frame already services net_poll, so sleep longer.
        {
            extern int g_compositor_launched;
            extern void proc_sleep(uint32_t ms);
            proc_sleep(g_compositor_launched ? 30 : 8);
        }
    }

    kprintf("[Desktop] Desktop environment exited\n");
}
// ============================================================================
// wm_process_input_events: collect and dispatch input for compositor syscall
// ============================================================================
// Called by sys_compositor_render_windows() so that apps remain interactive
// while the userland compositor is in exclusive mode.
void wm_process_input_events(void) {
    if (!g_desktop.initialized) return;

    // Service the network stack every compositor frame. Under the userland
    // compositor the kernel desktop_run loop (which used to call net_poll) no
    // longer runs, and desktop_process_tick early-returns in exclusive mode, so
    // without this the TCP/IP stack is never polled: no RX, no ARP replies, no
    // connectivity for IRC / remote-control / DNS while the compositor owns the
    // screen.
    { extern void net_poll(void); net_poll(); }

    // Poll mouse
    mouse_poll();
    int32_t mx, my;
    uint8_t buttons;
    mouse_get_position(&mx, &my);
    buttons = mouse_get_buttons();

    static int32_t  s_wpi_last_mx = -1;
    static int32_t  s_wpi_last_my = -1;
    static uint8_t  s_wpi_last_btn = 0;

    // Queue mouse movement
    if (mx != s_wpi_last_mx || my != s_wpi_last_my) {
        gui_event_t ev = {0};
        ev.type = EVENT_MOUSE_MOVE;
        ev.mouse_x = mx;
        ev.mouse_y = my;
        ev.mouse_buttons = buttons;
        wm_queue_event(&ev);
    }

    // Queue mouse button down (left)
    if ((buttons & MOUSE_LEFT_BTN) && !(s_wpi_last_btn & MOUSE_LEFT_BTN)) {
        gui_event_t ev = {0};
        ev.type = EVENT_MOUSE_DOWN;
        ev.mouse_x = mx;
        ev.mouse_y = my;
        ev.mouse_buttons = MOUSE_BUTTON_LEFT;
        wm_queue_event(&ev);
    }
    // Queue mouse button down (right)
    if ((buttons & MOUSE_RIGHT_BTN) && !(s_wpi_last_btn & MOUSE_RIGHT_BTN)) {
        gui_event_t ev = {0};
        ev.type = EVENT_MOUSE_DOWN;
        ev.mouse_x = mx;
        ev.mouse_y = my;
        ev.mouse_buttons = MOUSE_BUTTON_RIGHT;
        wm_queue_event(&ev);
    }
    // Queue mouse button up (left)
    if (!(buttons & MOUSE_LEFT_BTN) && (s_wpi_last_btn & MOUSE_LEFT_BTN)) {
        gui_event_t ev = {0};
        ev.type = EVENT_MOUSE_UP;
        ev.mouse_x = mx;
        ev.mouse_y = my;
        ev.mouse_buttons = MOUSE_BUTTON_LEFT;
        wm_queue_event(&ev);
    }
    // Queue mouse button up (right)
    if (!(buttons & MOUSE_RIGHT_BTN) && (s_wpi_last_btn & MOUSE_RIGHT_BTN)) {
        gui_event_t ev = {0};
        ev.type = EVENT_MOUSE_UP;
        ev.mouse_x = mx;
        ev.mouse_y = my;
        ev.mouse_buttons = MOUSE_BUTTON_RIGHT;
        wm_queue_event(&ev);
    }

    s_wpi_last_mx  = mx;
    s_wpi_last_my  = my;
    s_wpi_last_btn = buttons;

    // Poll keyboard
    extern int keyboard_has_char(void);
    extern int keyboard_get_char(void);
    while (keyboard_has_char()) {
        int c = keyboard_get_char();
        gui_event_t ev = {0};
        if (c >= 0x90 && c <= 0x98) {
            ev.type = EVENT_KEY_UP;
            ev.keycode = c - 0x10;
        } else if (c >= 0x80 && c < 0x90) {
            ev.type = EVENT_KEY_DOWN;
            ev.keycode = c;
        } else if (c > 0x98) {
            ev.type = EVENT_KEY_UP;
            ev.keycode = c & 0x7F;
        } else {
            ev.type = EVENT_KEY_DOWN;
            ev.keycode = c;
            ev.key_char = c;
        }
        wm_queue_event(&ev);
    }

    // Dispatch queued events to window manager and apps
    gui_event_t event;
    while (wm_poll_event(&event)) {
        switch (event.type) {
            case EVENT_MOUSE_MOVE:
                wm_handle_mouse_move(event.mouse_x, event.mouse_y);
                break;
            case EVENT_MOUSE_DOWN:
                if (window_get_at_point(event.mouse_x, event.mouse_y)) {
                    wm_handle_mouse_down(event.mouse_x, event.mouse_y, event.mouse_buttons);
                }
                break;
            case EVENT_MOUSE_UP:
                wm_handle_mouse_up(event.mouse_x, event.mouse_y, event.mouse_buttons);
                break;
            case EVENT_KEY_DOWN:
                wm_handle_key_down(event.keycode, event.key_char);
                break;
            case EVENT_KEY_UP:
                wm_handle_key_up(event.keycode);
                break;
            default:
                break;
        }
        wm_dispatch_event(&event);
    }
}

// Process one desktop frame (called from idle or syscall)
void desktop_process_tick(void) {
    if (!g_desktop.initialized) return;
    {
        // Win16 app owns the screen: its pump is the sole key consumer.
        extern volatile int g_win16_owns_screen;
        if (g_win16_owns_screen) return;
    }
    // Once the usermode compositor is launched it owns the screen; the kernel
    // desktop must not draw (would flash the kernel taskbar/icons over the
    // boot splash before the compositor's first frame).
    extern int g_compositor_launched;
    if (g_compositor_launched) return;
    if (wm_is_exclusive_mode()) return;
    
    // Quick mouse position update
    int32_t mouse_x, mouse_y;
    uint8_t buttons;
    mouse_get_position(&mouse_x, &mouse_y);
    buttons = mouse_get_buttons();
    
    // Check for keyboard input - throttled to 100 Hz to prevent
    // multiple events per physical keypress at 250 Hz timer rate
    extern int keyboard_has_char(void);
    extern int keyboard_get_char(void);
    uint32_t kb_interval = (g_timer_hz >= 20) ? (g_timer_hz / 20) : 1;
    if (timer_ticks - s_last_kb_tick >= kb_interval) {
        s_last_kb_tick = timer_ticks;
    while (keyboard_has_char()) {
        int c = keyboard_get_char();

        // Ignore ALL keypresses during first 2 seconds of boot.
        // The keyboard controller sends spurious scancodes (ESC, F12, etc.)
        // during initialization that can trigger unwanted actions.
        if (timer_ticks < 2 * g_timer_hz) {
            continue;
        }

        gui_event_t key_event = {0};
        // Check if this is a key release:
        // 1. Special release codes (0x90-0x98): subtract 0x10 to get press code
        // 2. Regular ASCII with bit 7 set (> 0x98): clear bit 7
        if (c >= 0x90 && c <= 0x98) {
            // Special key release (Ctrl, Shift, arrows)
            key_event.type = EVENT_KEY_UP;
            key_event.keycode = c - 0x10;
            key_event.key_char = 0;
        } else if (c >= 0x80 && c < 0x90) {
            // Special key press (arrows, ctrl, shift, etc.)
            key_event.type = EVENT_KEY_DOWN;
            key_event.keycode = c;
            key_event.key_char = 0;
        } else if (c > 0x98) {
            // Regular ASCII key release (ASCII | 0x80)
            key_event.type = EVENT_KEY_UP;
            key_event.keycode = c & 0x7F;
            key_event.key_char = 0;
        } else {
            // Regular key press (ASCII < 0x80)
            key_event.type = EVENT_KEY_DOWN;
            key_event.keycode = c;
            key_event.key_char = c;
        }
        wm_queue_event(&key_event);
    }
    // Any keyboard input dismisses screensaver
    screensaver_on_input();
    } // end kb_interval throttle

    // Check for mouse button events  
    static uint8_t last_buttons = 0;
    static int32_t last_mouse_x = -1, last_mouse_y = -1;
    if (buttons != last_buttons) {
        // Queue mouse button down event
        if ((buttons & MOUSE_LEFT_BTN) && !(last_buttons & MOUSE_LEFT_BTN)) {
            gui_event_t down_event = {0};
            down_event.type = EVENT_MOUSE_DOWN;
            down_event.mouse_x = mouse_x;
            down_event.mouse_y = mouse_y;
            down_event.mouse_buttons = MOUSE_BUTTON_LEFT;
            wm_queue_event(&down_event);
        }

        // Queue mouse button up event
        if (!(buttons & MOUSE_LEFT_BTN) && (last_buttons & MOUSE_LEFT_BTN)) {
            gui_event_t up_event = {0};
            up_event.type = EVENT_MOUSE_UP;
            up_event.mouse_x = mouse_x;
            up_event.mouse_y = mouse_y;
            up_event.mouse_buttons = MOUSE_BUTTON_LEFT;
            wm_queue_event(&up_event);
        }
        last_buttons = buttons;
        screensaver_on_input();  // Mouse button dismisses screensaver
    }

    // Check for mouse movement and update cursor
    if (mouse_x != last_mouse_x || mouse_y != last_mouse_y) {
        // Queue mouse move event
        gui_event_t move_event = {0};
        move_event.type = EVENT_MOUSE_MOVE;
        move_event.mouse_x = mouse_x;
        move_event.mouse_y = mouse_y;
        move_event.mouse_buttons = buttons;
        wm_queue_event(&move_event);
        
        last_mouse_x = mouse_x;
        last_mouse_y = mouse_y;
        
        // Force redraw for cursor update
        wm_invalidate_all();
        screensaver_on_input();  // Mouse movement dismisses screensaver
    }
    
    // Screensaver: update idle timer, activate if idle, draw if active
    screensaver_update();
    if (screensaver_is_active()) {
        screensaver_draw();
        extern void fb_swap_buffers(void);
        fb_swap_buffers();
        return;  // Skip normal WM processing while screensaver is active
    }

    // Process queued events through window manager (like desktop_run does)
    gui_event_t event;
    while (wm_poll_event(&event)) {
        switch (event.type) {
            case EVENT_MOUSE_DOWN:
                // First check if click is on a window
                if (window_get_at_point(event.mouse_x, event.mouse_y)) {
                    wm_handle_mouse_down(event.mouse_x, event.mouse_y, event.mouse_buttons);
                } else {
                    // Click is on desktop
                    if (event.mouse_buttons & MOUSE_BUTTON_LEFT) {
                        desktop_handle_click(event.mouse_x, event.mouse_y);
                    }
                }
                break;

            case EVENT_MOUSE_MOVE:
                wm_handle_mouse_move(event.mouse_x, event.mouse_y);
                break;

            case EVENT_MOUSE_UP:
                wm_handle_mouse_up(event.mouse_x, event.mouse_y, event.mouse_buttons);
                break;

            default:
                break;
        }

        // Dispatch event to registered apps
        wm_dispatch_event(&event);
    }
    
    // Redraw if dirty
    if (wm_is_dirty()) {
        desktop_draw();
        wm_draw_all();
        // Blit user-mode app content buffers into their windows.
        extern void wm_draw_apps(void);
        wm_draw_apps();
        wm_draw_winmenu();   // Task A: decorator popup on top of app content
        // Draw cursor on top
        desktop_draw_cursor(mouse_x, mouse_y);
        extern void fb_swap_buffers(void);
        fb_swap_buffers();
        wm_clear_dirty();
    }
}
