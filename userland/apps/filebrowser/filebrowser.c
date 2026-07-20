// filebrowser.c - Userland File Browser for MayteraOS
// Uses compositor client library for GUI

#include "../../compositor/client.h"
#include "../../compositor/protocol.h"
#include "../../libc/syscall.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"
#include "../../libc/stdlib.h"

// ============================================================================
// File Browser State
// ============================================================================

#define MAX_FILES           256
#define MAX_PATH_LEN        512
#define ICON_SIZE           32
#define ITEM_HEIGHT         36
#define SIDEBAR_WIDTH       180

typedef struct {
    char name[256];
    bool is_directory;
    uint32_t size;
    // uint64_t modified_time;
} file_entry_t;

typedef struct {
    compositor_connection_t *conn;
    client_window_t *window;
    
    // Current directory
    char current_path[MAX_PATH_LEN];
    
    // File listing
    file_entry_t files[MAX_FILES];
    int file_count;
    int selected_index;
    int scroll_offset;
    
    // UI state
    bool running;
    int hover_index;
} filebrowser_t;

static filebrowser_t g_fb;

// ============================================================================
// Colors
// ============================================================================

#define COLOR_WINDOW_BG     0xFFF5F5F5
#define COLOR_SIDEBAR_BG    0xFFE8E8E8
#define COLOR_SELECTED      0xFF4A90D9
#define COLOR_HOVER         0xFFD0E4F7
#define COLOR_TEXT          0xFF202020
#define COLOR_TEXT_WHITE    0xFFFFFFFF
#define COLOR_FOLDER        0xFFFFD700
#define COLOR_FILE          0xFF4A90D9
#define COLOR_BORDER        0xFFCCCCCC
#define COLOR_TOOLBAR_BG    0xFFE0E0E0

// ============================================================================
// File System Operations
// ============================================================================

// Directory entry from kernel
typedef struct {
    char name[256];
    uint32_t size;
    uint32_t type;  // 0 = file, 1 = directory
} dirent_t;

static int read_directory(const char *path) {
    g_fb.file_count = 0;
    g_fb.selected_index = -1;
    g_fb.scroll_offset = 0;
    
    // Open directory
    int fd = sys_open(path, 0);  // O_RDONLY
    if (fd < 0) {
        printf("[FileBrowser] Cannot open directory: %s\n", path);
        return -1;
    }
    
    // Read directory entries
    dirent_t entry;
    while (sys_read(fd, &entry, sizeof(entry)) == sizeof(entry)) {
        if (g_fb.file_count >= MAX_FILES) break;
        
        if (entry.name[0] == '\0') break;  // End of directory
        
        // Skip . entry
        if (strcmp(entry.name, ".") == 0) continue;
        
        file_entry_t *f = &g_fb.files[g_fb.file_count];
        strncpy(f->name, entry.name, sizeof(f->name) - 1);
        f->is_directory = (entry.type == 1);
        f->size = entry.size;
        
        g_fb.file_count++;
    }
    
    sys_close(fd);
    
    // Sort: directories first, then alphabetical
    // (Simple bubble sort)
    for (int i = 0; i < g_fb.file_count - 1; i++) {
        for (int j = i + 1; j < g_fb.file_count; j++) {
            bool swap = false;
            
            // Directories come first
            if (!g_fb.files[i].is_directory && g_fb.files[j].is_directory) {
                swap = true;
            } else if (g_fb.files[i].is_directory == g_fb.files[j].is_directory) {
                // Same type, sort alphabetically
                if (strcmp(g_fb.files[i].name, g_fb.files[j].name) > 0) {
                    swap = true;
                }
            }
            
            if (swap) {
                file_entry_t temp = g_fb.files[i];
                g_fb.files[i] = g_fb.files[j];
                g_fb.files[j] = temp;
            }
        }
    }
    
    printf("[FileBrowser] Loaded %d entries from %s\n", g_fb.file_count, path);
    return 0;
}

static void navigate_to(const char *path) {
    strncpy(g_fb.current_path, path, MAX_PATH_LEN - 1);
    read_directory(g_fb.current_path);
}

static void navigate_up(void) {
    // Go to parent directory
    char *last_slash = strrchr(g_fb.current_path, '/');
    if (last_slash && last_slash != g_fb.current_path) {
        *last_slash = '\0';
    } else {
        strcpy(g_fb.current_path, "/");
    }
    navigate_to(g_fb.current_path);
}

static void open_selected(void) {
    if (g_fb.selected_index < 0 || g_fb.selected_index >= g_fb.file_count) {
        return;
    }
    
    file_entry_t *f = &g_fb.files[g_fb.selected_index];
    
    if (f->is_directory) {
        // Navigate into directory
        char new_path[MAX_PATH_LEN];
        if (strcmp(f->name, "..") == 0) {
            navigate_up();
        } else if (strcmp(g_fb.current_path, "/") == 0) {
            snprintf(new_path, MAX_PATH_LEN, "/%s", f->name);
            navigate_to(new_path);
        } else {
            snprintf(new_path, MAX_PATH_LEN, "%s/%s", g_fb.current_path, f->name);
            navigate_to(new_path);
        }
    } else {
        // Open file with appropriate application
        // For now, just print the path
        char full_path[MAX_PATH_LEN];
        snprintf(full_path, MAX_PATH_LEN, "%s/%s", g_fb.current_path, f->name);
        printf("[FileBrowser] Open file: %s\n", full_path);
        
        // Launch editor for text files
        // TODO: Check file extension and launch appropriate app
    }
}

// ============================================================================
// Drawing
// ============================================================================

static void draw_toolbar(void) {
    client_window_t *win = g_fb.window;
    
    // Toolbar background
    window_fill_rect(win, 0, 0, win->width, 36, COLOR_TOOLBAR_BG);
    
    // Back button
    window_fill_rect(win, 4, 4, 28, 28, COLOR_BORDER);
    window_fill_rect(win, 5, 5, 26, 26, COLOR_WINDOW_BG);
    // Draw < arrow
    for (int i = 0; i < 10; i++) {
        window_put_pixel(win, 14 - i/2, 18 - 5 + i, COLOR_TEXT);
        window_put_pixel(win, 14 - i/2, 18 + 5 - i, COLOR_TEXT);
    }
    
    // Forward button
    window_fill_rect(win, 36, 4, 28, 28, COLOR_BORDER);
    window_fill_rect(win, 37, 5, 26, 26, COLOR_WINDOW_BG);
    
    // Up button
    window_fill_rect(win, 68, 4, 28, 28, COLOR_BORDER);
    window_fill_rect(win, 69, 5, 26, 26, COLOR_WINDOW_BG);
    
    // Path bar
    window_fill_rect(win, 100, 4, win->width - 108, 28, COLOR_BORDER);
    window_fill_rect(win, 101, 5, win->width - 110, 26, 0xFFFFFFFF);
    // TODO: Draw path text
}

static void draw_sidebar(void) {
    client_window_t *win = g_fb.window;
    int sidebar_y = 36;
    int sidebar_h = win->height - sidebar_y;
    
    // Sidebar background
    window_fill_rect(win, 0, sidebar_y, SIDEBAR_WIDTH, sidebar_h, COLOR_SIDEBAR_BG);
    
    // Quick access items
    const char *locations[] = {"Home", "Desktop", "Documents", "Downloads"};
    int num_locations = sizeof(locations) / sizeof(locations[0]);
    
    for (int i = 0; i < num_locations; i++) {
        int item_y = sidebar_y + 8 + i * 32;
        
        // Draw folder icon
        window_fill_rect(win, 12, item_y + 4, 20, 16, COLOR_FOLDER);
        
        // TODO: Draw text label
    }
    
    // Separator
    window_fill_rect(win, 8, sidebar_y + 8 + num_locations * 32 + 8, 
                     SIDEBAR_WIDTH - 16, 1, COLOR_BORDER);
}

static void draw_file_list(void) {
    client_window_t *win = g_fb.window;
    int list_x = SIDEBAR_WIDTH + 1;
    int list_y = 36;
    int list_w = win->width - SIDEBAR_WIDTH - 1;
    int list_h = win->height - list_y;
    
    // Background
    window_fill_rect(win, list_x, list_y, list_w, list_h, COLOR_WINDOW_BG);
    
    // Vertical separator
    window_fill_rect(win, SIDEBAR_WIDTH, list_y, 1, list_h, COLOR_BORDER);
    
    // Draw files
    int visible_items = list_h / ITEM_HEIGHT;
    
    for (int i = 0; i < visible_items && (i + g_fb.scroll_offset) < g_fb.file_count; i++) {
        int file_idx = i + g_fb.scroll_offset;
        file_entry_t *f = &g_fb.files[file_idx];
        
        int item_y = list_y + i * ITEM_HEIGHT;
        
        // Selection/hover highlight
        if (file_idx == g_fb.selected_index) {
            window_fill_rect(win, list_x, item_y, list_w, ITEM_HEIGHT, COLOR_SELECTED);
        } else if (file_idx == g_fb.hover_index) {
            window_fill_rect(win, list_x, item_y, list_w, ITEM_HEIGHT, COLOR_HOVER);
        }
        
        // Icon
        uint32_t icon_color = f->is_directory ? COLOR_FOLDER : COLOR_FILE;
        window_fill_rect(win, list_x + 8, item_y + 4, 28, 28, icon_color);
        
        // File name (simplified - just squares for characters)
        uint32_t text_color = (file_idx == g_fb.selected_index) ? COLOR_TEXT_WHITE : COLOR_TEXT;
        int text_x = list_x + 44;
        int text_y = item_y + 10;
        
        // Draw simplified text representation
        int name_len = strlen(f->name);
        for (int c = 0; c < name_len && c < 30; c++) {
            // Each character is a small rectangle
            for (int py = 0; py < 12; py++) {
                for (int px = 0; px < 6; px++) {
                    if (py > 1 && py < 10 && px > 0 && px < 5) {
                        window_put_pixel(win, text_x + c * 8 + px, text_y + py, text_color);
                    }
                }
            }
        }
        
        // File size (for files)
        if (!f->is_directory) {
            // Draw size at right side
            // TODO: Format and draw size
        }
    }
    
    // Scrollbar if needed
    if (g_fb.file_count > visible_items) {
        int scroll_height = list_h * visible_items / g_fb.file_count;
        int scroll_pos = list_h * g_fb.scroll_offset / g_fb.file_count;
        
        window_fill_rect(win, win->width - 12, list_y, 12, list_h, 0xFFE0E0E0);
        window_fill_rect(win, win->width - 10, list_y + scroll_pos, 8, scroll_height, 0xFF808080);
    }
}

static void redraw(void) {
    if (!g_fb.window) return;
    
    // Clear window
    window_clear(g_fb.window, COLOR_WINDOW_BG);
    
    // Draw UI elements
    draw_toolbar();
    draw_sidebar();
    draw_file_list();
    
    // Commit changes to compositor
    window_commit(g_fb.window);
}

// ============================================================================
// Event Handlers
// ============================================================================

static void on_close(client_window_t *win) {
    g_fb.running = false;
}

static void on_resize(client_window_t *win, int32_t width, int32_t height) {
    redraw();
}

static void on_mouse_button(client_window_t *win, int32_t x, int32_t y, int button, bool pressed) {
    if (!pressed) return;
    if (button != 1) return;  // Left click only
    
    // Check toolbar buttons
    if (y < 36) {
        if (x >= 4 && x < 32) {
            // Back button
            navigate_up();
            redraw();
        } else if (x >= 68 && x < 96) {
            // Up button
            navigate_up();
            redraw();
        }
        return;
    }
    
    // Check sidebar
    if (x < SIDEBAR_WIDTH) {
        int item = (y - 36 - 8) / 32;
        if (item >= 0 && item < 4) {
            const char *paths[] = {"/home", "/home/Desktop", "/home/Documents", "/home/Downloads"};
            navigate_to(paths[item]);
            redraw();
        }
        return;
    }
    
    // Check file list
    int list_y = 36;
    int item_idx = (y - list_y) / ITEM_HEIGHT + g_fb.scroll_offset;
    
    if (item_idx >= 0 && item_idx < g_fb.file_count) {
        static int last_click_idx = -1;
        static uint64_t last_click_time = 0;
        
        uint64_t now = sys_clock();
        
        if (item_idx == last_click_idx && (now - last_click_time) < 500) {
            // Double click - open
            g_fb.selected_index = item_idx;
            open_selected();
            last_click_idx = -1;
        } else {
            // Single click - select
            g_fb.selected_index = item_idx;
            last_click_idx = item_idx;
            last_click_time = now;
        }
        redraw();
    }
}

static void on_mouse_move(client_window_t *win, int32_t x, int32_t y, uint32_t buttons) {
    int old_hover = g_fb.hover_index;
    
    if (x > SIDEBAR_WIDTH && y > 36) {
        int list_y = 36;
        g_fb.hover_index = (y - list_y) / ITEM_HEIGHT + g_fb.scroll_offset;
        if (g_fb.hover_index >= g_fb.file_count) {
            g_fb.hover_index = -1;
        }
    } else {
        g_fb.hover_index = -1;
    }
    
    if (g_fb.hover_index != old_hover) {
        redraw();
    }
}

static void on_key(client_window_t *win, uint32_t keycode, bool pressed, uint32_t mods) {
    if (!pressed) return;
    
    switch (keycode) {
        case 0x48:  // Up arrow
            if (g_fb.selected_index > 0) {
                g_fb.selected_index--;
                redraw();
            }
            break;
        case 0x50:  // Down arrow
            if (g_fb.selected_index < g_fb.file_count - 1) {
                g_fb.selected_index++;
                redraw();
            }
            break;
        case 0x1C:  // Enter
            open_selected();
            redraw();
            break;
        case 0x0E:  // Backspace
            navigate_up();
            redraw();
            break;
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    printf("MayteraOS File Browser\n");
    
    memset(&g_fb, 0, sizeof(g_fb));
    g_fb.selected_index = -1;
    g_fb.hover_index = -1;
    g_fb.running = true;
    
    // Connect to compositor
    g_fb.conn = compositor_connect("File Browser");
    if (!g_fb.conn) {
        printf("[FileBrowser] Failed to connect to compositor\n");
        return 1;
    }
    
    // Create window
    g_fb.window = window_create(g_fb.conn, "Files",
                                 -1, -1, 800, 600,
                                 WIN_FLAG_RESIZABLE);
    if (!g_fb.window) {
        printf("[FileBrowser] Failed to create window\n");
        compositor_disconnect(g_fb.conn);
        return 1;
    }
    
    // Set event handlers
    g_fb.window->on_close = on_close;
    g_fb.window->on_resize = on_resize;
    g_fb.window->on_mouse_button = on_mouse_button;
    g_fb.window->on_mouse_move = on_mouse_move;
    g_fb.window->on_key = on_key;
    
    // Load initial directory
    const char *start_path = (argc > 1) ? argv[1] : "/";
    navigate_to(start_path);
    
    // Initial draw
    redraw();
    window_show(g_fb.window);
    
    // Event loop
    while (g_fb.running && g_fb.conn->num_windows > 0) {
        compositor_wait_events(g_fb.conn, 100);
    }
    
    // Cleanup
    compositor_disconnect(g_fb.conn);
    
    printf("[FileBrowser] Exiting\n");
    return 0;
}
