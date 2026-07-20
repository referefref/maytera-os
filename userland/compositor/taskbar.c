// taskbar.c - Taskbar for Userland Compositor
#include "taskbar.h"
#include "compositor.h"
#include "desktop.h"
#include "../libc/stdio.h"
#include "../libc/string.h"
#include "../libc/syscall.h"

// Global taskbar state
static taskbar_state_t g_taskbar;

// External compositor access
extern compositor_t g_compositor;

// ============================================================================
// Initialization
// ============================================================================

int taskbar_init(void) {
    printf("[Taskbar] Initializing taskbar...\n");
    
    memset(&g_taskbar, 0, sizeof(g_taskbar));
    
    // Position at bottom of screen
    g_taskbar.height = TASKBAR_HEIGHT;
    g_taskbar.y = g_compositor.screen_height - TASKBAR_HEIGHT;
    g_taskbar.x = 0;
    g_taskbar.width = g_compositor.screen_width;
    
    // Default colors (dark theme)
    g_taskbar.bg_color = 0xFF1A1A1A;
    g_taskbar.button_color = 0xFF2D2D2D;
    g_taskbar.button_hover_color = 0xFF3D3D3D;
    g_taskbar.button_active_color = 0xFF4A90D9;
    g_taskbar.text_color = 0xFFFFFFFF;
    
    // Start menu state
    g_taskbar.start_menu_open = false;
    g_taskbar.start_item_count = 0;
    g_taskbar.start_hover_item = -1;
    
    g_taskbar.hover_button = -1;
    g_taskbar.last_clock_update = 0;
    strcpy(g_taskbar.clock_text, "00:00");
    g_taskbar.network_connected = false;
    
    // Add default start menu items
    taskbar_add_start_item("Files", "/apps/filebrowser", 0);
    taskbar_add_start_item("Terminal", "/apps/terminal", 0);
    taskbar_add_start_item("Editor", "/apps/editor", 0);
    taskbar_add_start_item("Calculator", "/apps/calc", 0);
    taskbar_add_start_item("", "", 0);  // Separator
    taskbar_add_start_item("Settings", "/apps/settings", 0);
    taskbar_add_start_item("", "", 0);  // Separator
    taskbar_add_start_item("Shutdown", "__shutdown__", 0);
    
    printf("[Taskbar] Initialized at y=%d, height=%d\n", g_taskbar.y, g_taskbar.height);
    return 0;
}

void taskbar_shutdown(void) {
    printf("[Taskbar] Taskbar shutdown\n");
}

// ============================================================================
// Update
// ============================================================================

void taskbar_update(void) {
    // Update clock every second
    uint64_t now = sys_clock();
    if (now - g_taskbar.last_clock_update >= 1000) {
        // Get current time from kernel
        uint64_t time = sys_time();
        int hours = (time / 3600) % 24;
        int minutes = (time / 60) % 60;
        
        // Format time
        g_taskbar.clock_text[0] = '0' + (hours / 10);
        g_taskbar.clock_text[1] = '0' + (hours % 10);
        g_taskbar.clock_text[2] = ':';
        g_taskbar.clock_text[3] = '0' + (minutes / 10);
        g_taskbar.clock_text[4] = '0' + (minutes % 10);
        g_taskbar.clock_text[5] = '\0';
        
        g_taskbar.last_clock_update = now;
    }
}

// ============================================================================
// Rendering
// ============================================================================

static void draw_text_simple(uint32_t *fb, int32_t fb_width, int32_t fb_height,
                             int32_t x, int32_t y, const char *text, uint32_t color) {
    // Very simple 5x7 font rendering (placeholder)
    // In practice, use proper font rendering
    int len = strlen(text);
    for (int i = 0; i < len && i < 20; i++) {
        // Draw a small rectangle for each character (placeholder)
        for (int py = 0; py < 12; py++) {
            for (int px = 0; px < 6; px++) {
                int32_t dx = x + i * 8 + px;
                int32_t dy = y + py;
                if (dx >= 0 && dx < fb_width && dy >= 0 && dy < fb_height) {
                    // Simple pattern based on character
                    if (py > 1 && py < 10 && px > 0 && px < 5) {
                        fb[dy * fb_width + dx] = color;
                    }
                }
            }
        }
    }
}

static void draw_rect(uint32_t *fb, int32_t fb_width, int32_t fb_height,
                      int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    for (int32_t py = y; py < y + h; py++) {
        if (py < 0 || py >= fb_height) continue;
        for (int32_t px = x; px < x + w; px++) {
            if (px < 0 || px >= fb_width) continue;
            fb[py * fb_width + px] = color;
        }
    }
}

static void draw_start_button(uint32_t *fb, int32_t fb_width, int32_t fb_height) {
    int32_t x = g_taskbar.x + TASKBAR_PADDING;
    int32_t y = g_taskbar.y + TASKBAR_PADDING;
    int32_t w = START_BUTTON_WIDTH;
    int32_t h = TASKBAR_BUTTON_HEIGHT;
    
    uint32_t color = g_taskbar.start_menu_open ? 
                     g_taskbar.button_active_color : g_taskbar.button_color;
    
    draw_rect(fb, fb_width, fb_height, x, y, w, h, color);
    
    // Draw "Start" text (simplified)
    draw_text_simple(fb, fb_width, fb_height, x + 20, y + 12, "Start", g_taskbar.text_color);
}

static void draw_window_buttons(uint32_t *fb, int32_t fb_width, int32_t fb_height) {
    int32_t btn_x = g_taskbar.x + TASKBAR_PADDING + START_BUTTON_WIDTH + TASKBAR_PADDING;
    int32_t btn_y = g_taskbar.y + TASKBAR_PADDING;
    int btn_index = 0;
    
    // Draw a button for each visible window
    comp_window_t *win = g_compositor.windows;
    while (win) {
        if (win->flags & WIN_FLAG_NO_TASKBAR) {
            win = win->next;
            continue;
        }
        
        uint32_t color = g_taskbar.button_color;
        if (win->flags & WIN_FLAG_FOCUSED) {
            color = g_taskbar.button_active_color;
        } else if (btn_index == g_taskbar.hover_button) {
            color = g_taskbar.button_hover_color;
        }
        
        // Draw button
        draw_rect(fb, fb_width, fb_height, btn_x, btn_y, 
                  TASKBAR_BUTTON_WIDTH, TASKBAR_BUTTON_HEIGHT, color);
        
        // Draw window title (truncated)
        char title[20];
        strncpy(title, win->title, 19);
        title[19] = '\0';
        draw_text_simple(fb, fb_width, fb_height, btn_x + 8, btn_y + 12, 
                         title, g_taskbar.text_color);
        
        btn_x += TASKBAR_BUTTON_WIDTH + TASKBAR_PADDING;
        btn_index++;
        
        // Don't overflow taskbar
        if (btn_x + TASKBAR_BUTTON_WIDTH > g_taskbar.width - SYSTRAY_WIDTH) {
            break;
        }
        
        win = win->next;
    }
}

static void draw_system_tray(uint32_t *fb, int32_t fb_width, int32_t fb_height) {
    int32_t tray_x = g_taskbar.width - SYSTRAY_WIDTH;
    int32_t tray_y = g_taskbar.y + TASKBAR_PADDING;
    int32_t tray_h = TASKBAR_BUTTON_HEIGHT;
    
    // Draw tray background
    draw_rect(fb, fb_width, fb_height, tray_x, tray_y, 
              SYSTRAY_WIDTH - TASKBAR_PADDING, tray_h, g_taskbar.button_color);
    
    // Draw network icon (simple indicator)
    uint32_t net_color = g_taskbar.network_connected ? 0xFF00FF00 : 0xFF808080;
    draw_rect(fb, fb_width, fb_height, tray_x + 10, tray_y + 12, 16, 14, net_color);
    
    // Draw clock
    draw_text_simple(fb, fb_width, fb_height, tray_x + 50, tray_y + 12,
                     g_taskbar.clock_text, g_taskbar.text_color);
}

static void draw_start_menu(uint32_t *fb, int32_t fb_width, int32_t fb_height) {
    if (!g_taskbar.start_menu_open) return;
    
    int32_t menu_width = 250;
    int32_t item_height = 36;
    int32_t menu_height = g_taskbar.start_item_count * item_height + 10;
    int32_t menu_x = g_taskbar.x + TASKBAR_PADDING;
    int32_t menu_y = g_taskbar.y - menu_height;
    
    // Menu background
    draw_rect(fb, fb_width, fb_height, menu_x, menu_y, menu_width, menu_height, 
              0xF0202020);
    
    // Draw items
    for (int i = 0; i < g_taskbar.start_item_count; i++) {
        start_menu_item_t *item = &g_taskbar.start_items[i];
        int32_t item_y = menu_y + 5 + i * item_height;
        
        if (item->is_separator || item->name[0] == '\0') {
            // Draw separator line
            draw_rect(fb, fb_width, fb_height, menu_x + 10, item_y + item_height/2,
                      menu_width - 20, 1, 0xFF404040);
        } else {
            // Highlight if hovered
            if (i == g_taskbar.start_hover_item) {
                draw_rect(fb, fb_width, fb_height, menu_x + 2, item_y,
                          menu_width - 4, item_height - 2, 0xFF4A90D9);
            }
            
            // Draw item text
            draw_text_simple(fb, fb_width, fb_height, menu_x + 40, item_y + 10,
                             item->name, g_taskbar.text_color);
        }
    }
}

void taskbar_render(uint32_t *fb, int32_t fb_width, int32_t fb_height) {
    // Draw taskbar background
    draw_rect(fb, fb_width, fb_height, g_taskbar.x, g_taskbar.y,
              g_taskbar.width, g_taskbar.height, g_taskbar.bg_color);
    
    // Draw start button
    draw_start_button(fb, fb_width, fb_height);
    
    // Draw window buttons
    draw_window_buttons(fb, fb_width, fb_height);
    
    // Draw system tray
    draw_system_tray(fb, fb_width, fb_height);
    
    // Draw start menu if open
    draw_start_menu(fb, fb_width, fb_height);
}

// ============================================================================
// Input Handling
// ============================================================================

static void launch_app(const char *path) {
    printf("[Taskbar] Launching: %s\n", path);
    
    if (strcmp(path, "__shutdown__") == 0) {
        printf("[Taskbar] Shutdown requested\n");
        // TODO: Trigger system shutdown
        return;
    }
    
    // Fork and exec
    int pid = sys_fork();
    if (pid == 0) {
        char *argv[] = {(char *)path, NULL};
        sys_exec(path, argv, NULL);
        sys_exit(1);
    }
}

bool taskbar_handle_mouse(int32_t x, int32_t y, uint32_t buttons) {
    static uint32_t last_buttons = 0;
    bool left_click = (buttons & 1) && !(last_buttons & 1);
    last_buttons = buttons;
    
    // Check if mouse is in taskbar area
    if (y < g_taskbar.y && !g_taskbar.start_menu_open) {
        return false;
    }
    
    // Handle start menu
    if (g_taskbar.start_menu_open) {
        int32_t menu_width = 250;
        int32_t item_height = 36;
        int32_t menu_height = g_taskbar.start_item_count * item_height + 10;
        int32_t menu_x = g_taskbar.x + TASKBAR_PADDING;
        int32_t menu_y = g_taskbar.y - menu_height;
        
        if (x >= menu_x && x < menu_x + menu_width &&
            y >= menu_y && y < menu_y + menu_height) {
            // Mouse is in start menu
            int item_idx = (y - menu_y - 5) / item_height;
            if (item_idx >= 0 && item_idx < g_taskbar.start_item_count) {
                g_taskbar.start_hover_item = item_idx;
                
                if (left_click) {
                    start_menu_item_t *item = &g_taskbar.start_items[item_idx];
                    if (!item->is_separator && item->name[0] != '\0') {
                        launch_app(item->exec_path);
                        g_taskbar.start_menu_open = false;
                    }
                }
            }
            return true;
        } else {
            g_taskbar.start_hover_item = -1;
            // Click outside menu closes it
            if (left_click && y < g_taskbar.y) {
                g_taskbar.start_menu_open = false;
                return true;
            }
        }
    }
    
    // Not in taskbar
    if (y < g_taskbar.y) {
        return false;
    }
    
    // Check start button
    int32_t start_x = g_taskbar.x + TASKBAR_PADDING;
    if (x >= start_x && x < start_x + START_BUTTON_WIDTH) {
        if (left_click) {
            taskbar_toggle_start_menu();
        }
        return true;
    }
    
    // Check window buttons
    int32_t btn_x = start_x + START_BUTTON_WIDTH + TASKBAR_PADDING;
    int btn_index = 0;
    
    comp_window_t *win = g_compositor.windows;
    while (win) {
        if (win->flags & WIN_FLAG_NO_TASKBAR) {
            win = win->next;
            continue;
        }
        
        if (x >= btn_x && x < btn_x + TASKBAR_BUTTON_WIDTH) {
            g_taskbar.hover_button = btn_index;
            
            if (left_click) {
                // Toggle minimize/focus
                if (win->flags & WIN_FLAG_MINIMIZED) {
                    win->flags &= ~WIN_FLAG_MINIMIZED;
                    win->flags |= WIN_FLAG_VISIBLE;
                    compositor_window_focus(win);
                } else if (win->flags & WIN_FLAG_FOCUSED) {
                    win->flags |= WIN_FLAG_MINIMIZED;
                    win->flags &= ~WIN_FLAG_VISIBLE;
                } else {
                    compositor_window_focus(win);
                }
            }
            return true;
        }
        
        btn_x += TASKBAR_BUTTON_WIDTH + TASKBAR_PADDING;
        btn_index++;
        
        if (btn_x + TASKBAR_BUTTON_WIDTH > g_taskbar.width - SYSTRAY_WIDTH) {
            break;
        }
        
        win = win->next;
    }
    
    g_taskbar.hover_button = -1;
    return true;  // Consumed by taskbar
}

// ============================================================================
// Start Menu Management
// ============================================================================

void taskbar_toggle_start_menu(void) {
    g_taskbar.start_menu_open = !g_taskbar.start_menu_open;
    g_taskbar.start_hover_item = -1;
    compositor_damage_all();
}

int taskbar_add_start_item(const char *name, const char *exec_path, uint32_t icon_id) {
    if (g_taskbar.start_item_count >= MAX_START_ITEMS) {
        return -1;
    }
    
    start_menu_item_t *item = &g_taskbar.start_items[g_taskbar.start_item_count];
    
    if (name[0] == '\0') {
        item->is_separator = true;
        item->name[0] = '\0';
        item->exec_path[0] = '\0';
    } else {
        item->is_separator = false;
        strncpy(item->name, name, sizeof(item->name) - 1);
        strncpy(item->exec_path, exec_path, sizeof(item->exec_path) - 1);
    }
    item->icon_id = icon_id;
    
    g_taskbar.start_item_count++;
    return g_taskbar.start_item_count - 1;
}

int32_t taskbar_get_height(void) {
    return TASKBAR_HEIGHT;
}
