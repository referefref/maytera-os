// window_manager.c - Window Manager for Userland Compositor
#include "window_manager.h"
#include "compositor.h"
#include "../libc/stdio.h"
#include "../libc/string.h"

// Global configuration
static wm_config_t g_wm_config;

// External compositor state access
extern compositor_t g_compositor;

// ============================================================================
// Initialization
// ============================================================================

int window_manager_init(void) {
    printf("[WM] Initializing window manager...\n");
    
    // Default configuration
    g_wm_config.enable_animations = true;
    g_wm_config.anim_duration_ms = 150;
    
    g_wm_config.enable_snap = true;
    g_wm_config.snap_distance = 10;
    
    g_wm_config.focus_follows_mouse = false;
    g_wm_config.raise_on_focus = true;
    
    g_wm_config.titlebar_height = 28;
    g_wm_config.border_width = 1;
    g_wm_config.active_titlebar_color = 0xFF4A90D9;
    g_wm_config.inactive_titlebar_color = 0xFF808080;
    g_wm_config.border_color = 0xFF2060A0;
    
    printf("[WM] Window manager initialized\n");
    return 0;
}

void window_manager_shutdown(void) {
    printf("[WM] Window manager shutdown\n");
}

wm_config_t *wm_get_config(void) {
    return &g_wm_config;
}

// ============================================================================
// Tiling
// ============================================================================

void wm_tile_window(comp_window_t *win, tile_mode_t mode) {
    if (!win) return;
    
    int32_t sw = g_compositor.screen_width;
    int32_t sh = g_compositor.screen_height;
    int32_t taskbar_height = 48;  // Reserve space for taskbar
    int32_t usable_height = sh - taskbar_height;
    
    // Store original position for restore
    if (mode != TILE_NONE && !(win->flags & WIN_FLAG_MAXIMIZED)) {
        win->stored_x = win->x;
        win->stored_y = win->y;
        win->stored_w = win->width;
        win->stored_h = win->height;
    }
    
    switch (mode) {
        case TILE_NONE:
            // Restore to original
            win->x = win->stored_x;
            win->y = win->stored_y;
            win->width = win->stored_w;
            win->height = win->stored_h;
            win->flags &= ~WIN_FLAG_MAXIMIZED;
            break;
            
        case TILE_LEFT:
            win->x = 0;
            win->y = 0;
            win->width = sw / 2;
            win->height = usable_height;
            break;
            
        case TILE_RIGHT:
            win->x = sw / 2;
            win->y = 0;
            win->width = sw / 2;
            win->height = usable_height;
            break;
            
        case TILE_TOP_LEFT:
            win->x = 0;
            win->y = 0;
            win->width = sw / 2;
            win->height = usable_height / 2;
            break;
            
        case TILE_TOP_RIGHT:
            win->x = sw / 2;
            win->y = 0;
            win->width = sw / 2;
            win->height = usable_height / 2;
            break;
            
        case TILE_BOTTOM_LEFT:
            win->x = 0;
            win->y = usable_height / 2;
            win->width = sw / 2;
            win->height = usable_height / 2;
            break;
            
        case TILE_BOTTOM_RIGHT:
            win->x = sw / 2;
            win->y = usable_height / 2;
            win->width = sw / 2;
            win->height = usable_height / 2;
            break;
            
        case TILE_MAXIMIZE:
            win->x = 0;
            win->y = 0;
            win->width = sw;
            win->height = usable_height;
            win->flags |= WIN_FLAG_MAXIMIZED;
            break;
    }
    
    // Notify client of resize
    // compositor_send_configure_event(win);
}

void wm_cascade_windows(void) {
    int32_t start_x = 50;
    int32_t start_y = 50;
    int32_t offset = 30;
    int count = 0;
    
    comp_window_t *win = g_compositor.windows;
    while (win) {
        if (win->flags & WIN_FLAG_VISIBLE) {
            win->x = start_x + (count * offset);
            win->y = start_y + (count * offset);
            count++;
        }
        win = win->next;
    }
    
    compositor_damage_all();
}

void wm_tile_all_windows(void) {
    // Count visible windows
    int count = 0;
    comp_window_t *win = g_compositor.windows;
    while (win) {
        if ((win->flags & WIN_FLAG_VISIBLE) && !(win->flags & WIN_FLAG_MINIMIZED)) {
            count++;
        }
        win = win->next;
    }
    
    if (count == 0) return;
    
    // Calculate grid
    int cols = 1;
    int rows = 1;
    while (cols * rows < count) {
        if (cols <= rows) cols++;
        else rows++;
    }
    
    int32_t sw = g_compositor.screen_width;
    int32_t sh = g_compositor.screen_height - 48;  // Taskbar
    int32_t tile_w = sw / cols;
    int32_t tile_h = sh / rows;
    
    int idx = 0;
    win = g_compositor.windows;
    while (win) {
        if ((win->flags & WIN_FLAG_VISIBLE) && !(win->flags & WIN_FLAG_MINIMIZED)) {
            int col = idx % cols;
            int row = idx / cols;
            
            win->x = col * tile_w;
            win->y = row * tile_h;
            win->width = tile_w;
            win->height = tile_h;
            
            idx++;
        }
        win = win->next;
    }
    
    compositor_damage_all();
}

void wm_minimize_all(void) {
    comp_window_t *win = g_compositor.windows;
    while (win) {
        if (win->flags & WIN_FLAG_VISIBLE) {
            win->flags |= WIN_FLAG_MINIMIZED;
            win->flags &= ~WIN_FLAG_VISIBLE;
        }
        win = win->next;
    }
    compositor_damage_all();
}

void wm_show_desktop(void) {
    wm_minimize_all();
}

void wm_cycle_windows(bool reverse) {
    if (!g_compositor.windows) return;
    
    comp_window_t *current = g_compositor.focused;
    comp_window_t *next = NULL;
    
    if (reverse) {
        // Find previous window
        if (current && current->next) {
            next = current->next;
        } else {
            // Wrap to last window
            comp_window_t *w = g_compositor.windows;
            while (w && w->next) w = w->next;
            next = w;
        }
    } else {
        // Find next window
        if (current && current->prev) {
            next = current->prev;
        } else {
            // Wrap to first window
            next = g_compositor.windows;
        }
    }
    
    if (next && next != current) {
        // Restore if minimized
        if (next->flags & WIN_FLAG_MINIMIZED) {
            next->flags &= ~WIN_FLAG_MINIMIZED;
            next->flags |= WIN_FLAG_VISIBLE;
        }
        compositor_window_focus(next);
    }
}

void wm_close_focused(void) {
    if (!g_compositor.focused) return;
    
    // Send close request to client
    // compositor_send_close_request(g_compositor.focused);
}

// ============================================================================
// Keyboard Shortcuts
// ============================================================================

// Key codes (match scancode values)
#define KEY_TAB     0x0F
#define KEY_F4      0x3E
#define KEY_LEFT    0x4B
#define KEY_RIGHT   0x4D
#define KEY_UP      0x48
#define KEY_DOWN    0x50
#define KEY_D       0x20
#define KEY_M       0x32

bool wm_handle_shortcut(uint32_t keycode, uint32_t modifiers) {
    bool alt = (modifiers & KEY_MOD_ALT) != 0;
    bool super = (modifiers & KEY_MOD_SUPER) != 0;
    bool shift = (modifiers & KEY_MOD_SHIFT) != 0;
    
    if (alt) {
        switch (keycode) {
            case KEY_TAB:
                wm_cycle_windows(shift);
                return true;
            case KEY_F4:
                wm_close_focused();
                return true;
        }
    }
    
    if (super) {
        comp_window_t *win = g_compositor.focused;
        
        switch (keycode) {
            case KEY_LEFT:
                if (win) wm_tile_window(win, TILE_LEFT);
                return true;
            case KEY_RIGHT:
                if (win) wm_tile_window(win, TILE_RIGHT);
                return true;
            case KEY_UP:
                if (win) wm_tile_window(win, TILE_MAXIMIZE);
                return true;
            case KEY_DOWN:
                if (win) wm_tile_window(win, TILE_NONE);
                return true;
            case KEY_D:
                wm_show_desktop();
                return true;
            case KEY_M:
                wm_minimize_all();
                return true;
        }
    }
    
    return false;
}
