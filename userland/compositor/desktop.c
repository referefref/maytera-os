// desktop.c - Desktop Manager for Userland Compositor
#include "desktop.h"
#include "compositor.h"
#include "../libc/stdio.h"
#include "../libc/string.h"
#include "../libc/stdlib.h"
#include "../libc/syscall.h"

// Global desktop state
static desktop_state_t g_desktop;

// Default icon (folder icon) - 48x48 simplified
static const uint32_t default_icon_data[48*48] = {0};  // Would contain actual icon data

// ============================================================================
// Initialization
// ============================================================================

int desktop_init(void) {
    printf("[Desktop] Initializing desktop manager...\n");
    
    memset(&g_desktop, 0, sizeof(g_desktop));
    
    // Default blue background
    g_desktop.bg_color = 0xFF2C5AA0;
    g_desktop.wallpaper = NULL;
    g_desktop.icon_count = 0;
    g_desktop.selected_icon = -1;
    g_desktop.selecting = false;
    g_desktop.last_click_icon = (uint32_t)-1;
    g_desktop.last_click_time = 0;
    
    // Add default desktop icons
    desktop_add_icon("Files", "/apps/filebrowser", 20, 20);
    desktop_add_icon("Terminal", "/apps/terminal", 20, 100);
    desktop_add_icon("Settings", "/apps/settings", 20, 180);
    desktop_add_icon("Calculator", "/apps/calc", 20, 260);
    desktop_add_icon("Editor", "/apps/editor", 20, 340);
    
    printf("[Desktop] Initialized with %d icons\n", g_desktop.icon_count);
    return 0;
}

void desktop_shutdown(void) {
    // Free wallpaper if allocated
    if (g_desktop.wallpaper) {
        free(g_desktop.wallpaper);
        g_desktop.wallpaper = NULL;
    }
    
    // Free icon data
    for (int i = 0; i < g_desktop.icon_count; i++) {
        if (g_desktop.icons[i].icon_data && 
            g_desktop.icons[i].icon_data != default_icon_data) {
            free(g_desktop.icons[i].icon_data);
        }
    }
    
    printf("[Desktop] Desktop shutdown\n");
}

// ============================================================================
// Update
// ============================================================================

void desktop_update(void) {
    // Nothing to update for now
    // Future: animations, live wallpaper, etc.
}

// ============================================================================
// Rendering
// ============================================================================

static void draw_icon(uint32_t *fb, int32_t fb_width, int32_t fb_height,
                      desktop_icon_t *icon) {
    if (!icon->visible) return;
    
    // Draw icon background if selected
    if (icon->selected) {
        uint32_t sel_color = 0x604A90D9;  // Semi-transparent blue
        for (int32_t y = 0; y < ICON_HEIGHT + ICON_LABEL_HEIGHT; y++) {
            for (int32_t x = 0; x < ICON_WIDTH; x++) {
                int32_t px = icon->x + x;
                int32_t py = icon->y + y;
                if (px >= 0 && px < fb_width && py >= 0 && py < fb_height) {
                    fb[py * fb_width + px] = sel_color;
                }
            }
        }
    }
    
    // Draw icon image (or placeholder)
    uint32_t icon_color = 0xFFFFD700;  // Gold folder color
    for (int32_t y = 0; y < ICON_HEIGHT; y++) {
        for (int32_t x = 0; x < ICON_WIDTH; x++) {
            int32_t px = icon->x + x;
            int32_t py = icon->y + y;
            if (px >= 0 && px < fb_width && py >= 0 && py < fb_height) {
                // Simple folder icon shape
                if (y < 8 && x >= 4 && x < 24) {
                    // Folder tab
                    fb[py * fb_width + px] = icon_color;
                } else if (y >= 8 && y < ICON_HEIGHT - 4 &&
                           x >= 2 && x < ICON_WIDTH - 2) {
                    // Folder body
                    fb[py * fb_width + px] = icon_color;
                }
            }
        }
    }
    
    // Draw icon label (simplified - just a background bar)
    uint32_t label_bg = icon->selected ? 0xFF4A90D9 : 0x80000000;
    for (int32_t y = 0; y < ICON_LABEL_HEIGHT; y++) {
        for (int32_t x = 0; x < ICON_WIDTH; x++) {
            int32_t px = icon->x + x;
            int32_t py = icon->y + ICON_HEIGHT + y;
            if (px >= 0 && px < fb_width && py >= 0 && py < fb_height) {
                fb[py * fb_width + px] = label_bg;
            }
        }
    }
    
    // TODO: Draw actual text label using font rendering
}

void desktop_render(uint32_t *fb, int32_t width, int32_t height) {
    // Draw wallpaper or solid color
    if (g_desktop.wallpaper) {
        // Scale/tile wallpaper to fit screen
        for (int32_t y = 0; y < height; y++) {
            for (int32_t x = 0; x < width; x++) {
                int32_t sx = (x * g_desktop.wp_width) / width;
                int32_t sy = (y * g_desktop.wp_height) / height;
                fb[y * width + x] = g_desktop.wallpaper[sy * g_desktop.wp_width + sx];
            }
        }
    } else {
        // Solid color already filled by compositor
    }
    
    // Draw desktop icons
    for (int i = 0; i < g_desktop.icon_count; i++) {
        draw_icon(fb, width, height, &g_desktop.icons[i]);
    }
    
    // Draw selection rectangle if selecting
    if (g_desktop.selecting) {
        int32_t x1 = g_desktop.sel_start_x < g_desktop.sel_end_x ? 
                     g_desktop.sel_start_x : g_desktop.sel_end_x;
        int32_t y1 = g_desktop.sel_start_y < g_desktop.sel_end_y ?
                     g_desktop.sel_start_y : g_desktop.sel_end_y;
        int32_t x2 = g_desktop.sel_start_x > g_desktop.sel_end_x ?
                     g_desktop.sel_start_x : g_desktop.sel_end_x;
        int32_t y2 = g_desktop.sel_start_y > g_desktop.sel_end_y ?
                     g_desktop.sel_start_y : g_desktop.sel_end_y;
        
        uint32_t sel_border = 0xFF4A90D9;
        uint32_t sel_fill = 0x404A90D9;
        
        // Draw filled rectangle
        for (int32_t y = y1; y < y2; y++) {
            for (int32_t x = x1; x < x2; x++) {
                if (x >= 0 && x < width && y >= 0 && y < height) {
                    fb[y * width + x] = sel_fill;
                }
            }
        }
        
        // Draw border
        for (int32_t x = x1; x < x2; x++) {
            if (x >= 0 && x < width) {
                if (y1 >= 0 && y1 < height) fb[y1 * width + x] = sel_border;
                if (y2 >= 0 && y2 < height) fb[(y2-1) * width + x] = sel_border;
            }
        }
        for (int32_t y = y1; y < y2; y++) {
            if (y >= 0 && y < height) {
                if (x1 >= 0 && x1 < width) fb[y * width + x1] = sel_border;
                if (x2 >= 0 && x2 < width) fb[y * width + x2-1] = sel_border;
            }
        }
    }
}

// ============================================================================
// Input Handling
// ============================================================================

static desktop_icon_t *find_icon_at(int32_t x, int32_t y) {
    for (int i = g_desktop.icon_count - 1; i >= 0; i--) {
        desktop_icon_t *icon = &g_desktop.icons[i];
        if (!icon->visible) continue;
        
        if (x >= icon->x && x < icon->x + ICON_WIDTH &&
            y >= icon->y && y < icon->y + ICON_HEIGHT + ICON_LABEL_HEIGHT) {
            return icon;
        }
    }
    return NULL;
}

static void launch_app(const char *path) {
    printf("[Desktop] Launching: %s\n", path);
    
    // Fork and exec the application
    int pid = sys_fork();
    if (pid == 0) {
        // Child process
        char *argv[] = {(char *)path, NULL};
        sys_exec(path, argv, NULL);
        sys_exit(1);  // exec failed
    } else if (pid > 0) {
        printf("[Desktop] Spawned process %d\n", pid);
    } else {
        printf("[Desktop] ERROR: fork failed\n");
    }
}

bool desktop_handle_mouse(int32_t x, int32_t y, uint32_t buttons) {
    static uint32_t last_buttons = 0;
    bool left_pressed = (buttons & 1) && !(last_buttons & 1);
    bool left_released = !(buttons & 1) && (last_buttons & 1);
    
    // Update selection rectangle if selecting
    if (g_desktop.selecting) {
        g_desktop.sel_end_x = x;
        g_desktop.sel_end_y = y;
        
        // Update icon selection based on rectangle
        int32_t x1 = g_desktop.sel_start_x < g_desktop.sel_end_x ? 
                     g_desktop.sel_start_x : g_desktop.sel_end_x;
        int32_t y1 = g_desktop.sel_start_y < g_desktop.sel_end_y ?
                     g_desktop.sel_start_y : g_desktop.sel_end_y;
        int32_t x2 = g_desktop.sel_start_x > g_desktop.sel_end_x ?
                     g_desktop.sel_start_x : g_desktop.sel_end_x;
        int32_t y2 = g_desktop.sel_start_y > g_desktop.sel_end_y ?
                     g_desktop.sel_start_y : g_desktop.sel_end_y;
        
        for (int i = 0; i < g_desktop.icon_count; i++) {
            desktop_icon_t *icon = &g_desktop.icons[i];
            // Check if icon intersects selection rectangle
            bool intersects = !(icon->x + ICON_WIDTH < x1 ||
                               icon->x > x2 ||
                               icon->y + ICON_HEIGHT + ICON_LABEL_HEIGHT < y1 ||
                               icon->y > y2);
            icon->selected = intersects;
        }
        
        if (left_released) {
            g_desktop.selecting = false;
        }
        
        last_buttons = buttons;
        return true;
    }
    
    if (left_pressed) {
        desktop_icon_t *icon = find_icon_at(x, y);
        
        if (icon) {
            // Check for double-click
            uint64_t now = sys_clock();
            if (icon->id == g_desktop.last_click_icon &&
                (now - g_desktop.last_click_time) < 500) {  // 500ms double-click time
                // Double-click - launch app
                launch_app(icon->exec_path);
                g_desktop.last_click_icon = (uint32_t)-1;
            } else {
                // Single click - select icon
                for (int i = 0; i < g_desktop.icon_count; i++) {
                    g_desktop.icons[i].selected = false;
                }
                icon->selected = true;
                g_desktop.last_click_icon = icon->id;
                g_desktop.last_click_time = now;
            }
            
            last_buttons = buttons;
            return true;
        } else {
            // Click on empty desktop - start selection rectangle
            for (int i = 0; i < g_desktop.icon_count; i++) {
                g_desktop.icons[i].selected = false;
            }
            g_desktop.selecting = true;
            g_desktop.sel_start_x = x;
            g_desktop.sel_start_y = y;
            g_desktop.sel_end_x = x;
            g_desktop.sel_end_y = y;
            g_desktop.last_click_icon = (uint32_t)-1;
            
            last_buttons = buttons;
            return false;  // Let compositor know click was on desktop
        }
    }
    
    last_buttons = buttons;
    return false;
}

// ============================================================================
// Icon Management
// ============================================================================

int desktop_add_icon(const char *name, const char *exec_path, int32_t x, int32_t y) {
    if (g_desktop.icon_count >= MAX_DESKTOP_ICONS) {
        return -1;
    }
    
    desktop_icon_t *icon = &g_desktop.icons[g_desktop.icon_count];
    icon->id = g_desktop.icon_count + 1;
    strncpy(icon->name, name, sizeof(icon->name) - 1);
    strncpy(icon->exec_path, exec_path, sizeof(icon->exec_path) - 1);
    icon->x = x;
    icon->y = y;
    icon->icon_data = NULL;  // Use default icon
    icon->selected = false;
    icon->visible = true;
    
    g_desktop.icon_count++;
    
    return icon->id;
}

void desktop_remove_icon(uint32_t icon_id) {
    for (int i = 0; i < g_desktop.icon_count; i++) {
        if (g_desktop.icons[i].id == icon_id) {
            // Free icon data if allocated
            if (g_desktop.icons[i].icon_data && 
                g_desktop.icons[i].icon_data != default_icon_data) {
                free(g_desktop.icons[i].icon_data);
            }
            
            // Shift remaining icons
            for (int j = i; j < g_desktop.icon_count - 1; j++) {
                g_desktop.icons[j] = g_desktop.icons[j + 1];
            }
            g_desktop.icon_count--;
            return;
        }
    }
}

int desktop_load_wallpaper(const char *path) {
    // TODO: Load BMP/PNG image from file
    // For now, just set a solid color
    printf("[Desktop] Loading wallpaper: %s\n", path);
    return -1;  // Not implemented
}

void desktop_set_bg_color(uint32_t color) {
    g_desktop.bg_color = color;
    if (g_desktop.wallpaper) {
        free(g_desktop.wallpaper);
        g_desktop.wallpaper = NULL;
    }
}

void desktop_arrange_icons(void) {
    int32_t x = 20;
    int32_t y = 20;
    int32_t row_height = ICON_HEIGHT + ICON_LABEL_HEIGHT + ICON_SPACING;
    int32_t col_width = ICON_WIDTH + ICON_SPACING;
    
    // Get screen height for wrapping (minus taskbar)
    int32_t max_y = 768 - 48 - row_height;  // TODO: get actual screen height
    
    for (int i = 0; i < g_desktop.icon_count; i++) {
        g_desktop.icons[i].x = x;
        g_desktop.icons[i].y = y;
        
        y += row_height;
        if (y > max_y) {
            y = 20;
            x += col_width;
        }
    }
}
