// settings.c - Userland Settings App for MayteraOS
#include "../../compositor/client.h"
#include "../../compositor/protocol.h"
#include "../../libc/syscall.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"
#include "../../libc/stdlib.h"

// ============================================================================
// Settings State
// ============================================================================

#define SIDEBAR_WIDTH       200
#define NUM_CATEGORIES      6

typedef enum {
    CAT_DISPLAY = 0,
    CAT_NETWORK,
    CAT_AUDIO,
    CAT_DATETIME,
    CAT_ABOUT,
    CAT_SYSTEM
} settings_category_t;

static const char *category_names[] = {
    "Display",
    "Network",
    "Audio", 
    "Date & Time",
    "About",
    "System"
};

typedef struct {
    compositor_connection_t *conn;
    client_window_t *window;
    
    settings_category_t current_category;
    int hover_category;
    
    // Display settings
    int brightness;         // 0-100
    int resolution_idx;     // Index into resolution list
    
    // Audio settings
    int volume;             // 0-100
    bool muted;
    
    // System info
    char os_version[64];
    char hostname[64];
    uint64_t total_memory;
    uint64_t free_memory;
    
    bool running;
} settings_t;

static settings_t g_settings;

// ============================================================================
// Colors
// ============================================================================

#define COLOR_BG            0xFFF0F0F0
#define COLOR_SIDEBAR       0xFFE0E0E0
#define COLOR_SELECTED      0xFF4A90D9
#define COLOR_HOVER         0xFFD0D0D0
#define COLOR_TEXT          0xFF202020
#define COLOR_TEXT_WHITE    0xFFFFFFFF
#define COLOR_ACCENT        0xFF4A90D9
#define COLOR_CONTROL_BG    0xFFFFFFFF
#define COLOR_SLIDER_BG     0xFFCCCCCC
#define COLOR_SLIDER_FG     0xFF4A90D9

// ============================================================================
// Drawing Helpers
// ============================================================================

static void draw_text_simple(client_window_t *win, int32_t x, int32_t y, 
                             const char *text, uint32_t color) {
    int len = strlen(text);
    for (int i = 0; i < len && i < 40; i++) {
        for (int py = 0; py < 14; py++) {
            for (int px = 0; px < 8; px++) {
                if (py > 1 && py < 12 && px > 0 && px < 7) {
                    window_put_pixel(win, x + i * 10 + px, y + py, color);
                }
            }
        }
    }
}

static void draw_slider(client_window_t *win, int32_t x, int32_t y, 
                        int32_t width, int value) {
    // Background track
    window_fill_rect(win, x, y + 6, width, 8, COLOR_SLIDER_BG);
    
    // Filled portion
    int filled_width = (width * value) / 100;
    window_fill_rect(win, x, y + 6, filled_width, 8, COLOR_SLIDER_FG);
    
    // Thumb
    int thumb_x = x + filled_width - 8;
    if (thumb_x < x) thumb_x = x;
    window_fill_rect(win, thumb_x, y, 16, 20, COLOR_ACCENT);
}

static void draw_checkbox(client_window_t *win, int32_t x, int32_t y, 
                          bool checked, const char *label) {
    // Box
    window_fill_rect(win, x, y, 20, 20, COLOR_TEXT);
    window_fill_rect(win, x + 2, y + 2, 16, 16, checked ? COLOR_ACCENT : COLOR_CONTROL_BG);
    
    // Check mark if checked
    if (checked) {
        for (int i = 0; i < 8; i++) {
            window_put_pixel(win, x + 5 + i, y + 10 - i/2, COLOR_TEXT_WHITE);
            window_put_pixel(win, x + 5 + i, y + 11 - i/2, COLOR_TEXT_WHITE);
        }
    }
    
    // Label
    draw_text_simple(win, x + 28, y + 3, label, COLOR_TEXT);
}

// ============================================================================
// Category Drawing
// ============================================================================

static void draw_display_settings(client_window_t *win, int32_t x, int32_t y) {
    draw_text_simple(win, x, y, "Display Settings", COLOR_TEXT);
    y += 40;
    
    // Brightness
    draw_text_simple(win, x, y, "Brightness:", COLOR_TEXT);
    y += 25;
    draw_slider(win, x, y, 300, g_settings.brightness);
    y += 40;
    
    // Resolution (placeholder)
    draw_text_simple(win, x, y, "Resolution:", COLOR_TEXT);
    y += 25;
    
    // Resolution options (simplified)
    const char *resolutions[] = {"1920x1080", "1280x720", "1024x768"};
    for (int i = 0; i < 3; i++) {
        bool selected = (i == g_settings.resolution_idx);
        window_fill_rect(win, x + i * 120, y, 110, 30, 
                         selected ? COLOR_SELECTED : COLOR_CONTROL_BG);
        draw_text_simple(win, x + i * 120 + 8, y + 8, resolutions[i],
                        selected ? COLOR_TEXT_WHITE : COLOR_TEXT);
    }
}

static void draw_network_settings(client_window_t *win, int32_t x, int32_t y) {
    draw_text_simple(win, x, y, "Network Settings", COLOR_TEXT);
    y += 40;
    
    draw_text_simple(win, x, y, "Status: Connected", COLOR_TEXT);
    y += 30;
    
    draw_text_simple(win, x, y, "IP Address: 10.0.2.15", COLOR_TEXT);
    y += 30;
    
    draw_text_simple(win, x, y, "Gateway: 10.0.2.2", COLOR_TEXT);
    y += 30;
    
    draw_text_simple(win, x, y, "DNS: 10.0.2.3", COLOR_TEXT);
}

static void draw_audio_settings(client_window_t *win, int32_t x, int32_t y) {
    draw_text_simple(win, x, y, "Audio Settings", COLOR_TEXT);
    y += 40;
    
    // Volume
    draw_text_simple(win, x, y, "Volume:", COLOR_TEXT);
    y += 25;
    draw_slider(win, x, y, 300, g_settings.muted ? 0 : g_settings.volume);
    y += 40;
    
    // Mute checkbox
    draw_checkbox(win, x, y, g_settings.muted, "Mute");
}

static void draw_datetime_settings(client_window_t *win, int32_t x, int32_t y) {
    draw_text_simple(win, x, y, "Date & Time", COLOR_TEXT);
    y += 40;
    
    // Get current time
    uint64_t time = sys_time();
    int hours = (time / 3600) % 24;
    int minutes = (time / 60) % 60;
    int seconds = time % 60;
    
    char time_str[32];
    time_str[0] = '0' + hours / 10;
    time_str[1] = '0' + hours % 10;
    time_str[2] = ':';
    time_str[3] = '0' + minutes / 10;
    time_str[4] = '0' + minutes % 10;
    time_str[5] = ':';
    time_str[6] = '0' + seconds / 10;
    time_str[7] = '0' + seconds % 10;
    time_str[8] = '\0';
    
    draw_text_simple(win, x, y, "Current Time:", COLOR_TEXT);
    draw_text_simple(win, x + 150, y, time_str, COLOR_ACCENT);
    y += 40;
    
    draw_text_simple(win, x, y, "Timezone: UTC", COLOR_TEXT);
}

static void draw_about_settings(client_window_t *win, int32_t x, int32_t y) {
    draw_text_simple(win, x, y, "About MayteraOS", COLOR_TEXT);
    y += 40;
    
    // Logo (simplified)
    window_fill_rect(win, x, y, 80, 80, COLOR_ACCENT);
    y += 100;
    
    draw_text_simple(win, x, y, "MayteraOS v1.8.0", COLOR_TEXT);
    y += 30;
    
    draw_text_simple(win, x, y, "A modern 64-bit operating system", COLOR_TEXT);
    y += 30;
    
    draw_text_simple(win, x, y, "with AI-assisted capabilities", COLOR_TEXT);
    y += 50;
    
    char mem_str[64];
    // snprintf(mem_str, sizeof(mem_str), "Memory: %lu MB", g_settings.total_memory / 1024 / 1024);
    strcpy(mem_str, "Memory: 256 MB");
    draw_text_simple(win, x, y, mem_str, COLOR_TEXT);
}

static void draw_system_settings(client_window_t *win, int32_t x, int32_t y) {
    draw_text_simple(win, x, y, "System Settings", COLOR_TEXT);
    y += 40;
    
    // Hostname
    draw_text_simple(win, x, y, "Hostname:", COLOR_TEXT);
    y += 25;
    window_fill_rect(win, x, y, 250, 30, COLOR_CONTROL_BG);
    draw_text_simple(win, x + 8, y + 8, "maytera", COLOR_TEXT);
    y += 50;
    
    // Power options
    draw_text_simple(win, x, y, "Power:", COLOR_TEXT);
    y += 30;
    
    // Restart button
    window_fill_rect(win, x, y, 120, 36, COLOR_ACCENT);
    draw_text_simple(win, x + 20, y + 10, "Restart", COLOR_TEXT_WHITE);
    
    // Shutdown button
    window_fill_rect(win, x + 140, y, 120, 36, 0xFFCC4444);
    draw_text_simple(win, x + 152, y + 10, "Shutdown", COLOR_TEXT_WHITE);
}

// ============================================================================
// Main Drawing
// ============================================================================

static void draw_sidebar(void) {
    client_window_t *win = g_settings.window;
    
    // Sidebar background
    window_fill_rect(win, 0, 0, SIDEBAR_WIDTH, win->height, COLOR_SIDEBAR);
    
    // Title
    draw_text_simple(win, 16, 20, "Settings", COLOR_TEXT);
    
    // Categories
    for (int i = 0; i < NUM_CATEGORIES; i++) {
        int item_y = 60 + i * 44;
        
        // Highlight
        if (i == g_settings.current_category) {
            window_fill_rect(win, 0, item_y, SIDEBAR_WIDTH, 40, COLOR_SELECTED);
        } else if (i == g_settings.hover_category) {
            window_fill_rect(win, 0, item_y, SIDEBAR_WIDTH, 40, COLOR_HOVER);
        }
        
        // Category name
        uint32_t text_color = (i == g_settings.current_category) ? COLOR_TEXT_WHITE : COLOR_TEXT;
        draw_text_simple(win, 16, item_y + 12, category_names[i], text_color);
    }
}

static void draw_content(void) {
    client_window_t *win = g_settings.window;
    
    // Content area background
    window_fill_rect(win, SIDEBAR_WIDTH, 0, win->width - SIDEBAR_WIDTH, win->height, COLOR_BG);
    
    int content_x = SIDEBAR_WIDTH + 30;
    int content_y = 30;
    
    switch (g_settings.current_category) {
        case CAT_DISPLAY:
            draw_display_settings(win, content_x, content_y);
            break;
        case CAT_NETWORK:
            draw_network_settings(win, content_x, content_y);
            break;
        case CAT_AUDIO:
            draw_audio_settings(win, content_x, content_y);
            break;
        case CAT_DATETIME:
            draw_datetime_settings(win, content_x, content_y);
            break;
        case CAT_ABOUT:
            draw_about_settings(win, content_x, content_y);
            break;
        case CAT_SYSTEM:
            draw_system_settings(win, content_x, content_y);
            break;
    }
}

static void redraw(void) {
    if (!g_settings.window) return;
    
    window_clear(g_settings.window, COLOR_BG);
    draw_sidebar();
    draw_content();
    window_commit(g_settings.window);
}

// ============================================================================
// Event Handlers
// ============================================================================

static void on_close(client_window_t *win) {
    g_settings.running = false;
}

static void on_resize(client_window_t *win, int32_t width, int32_t height) {
    redraw();
}

static void on_mouse_move(client_window_t *win, int32_t x, int32_t y, uint32_t buttons) {
    int old_hover = g_settings.hover_category;
    
    if (x < SIDEBAR_WIDTH && y >= 60) {
        g_settings.hover_category = (y - 60) / 44;
        if (g_settings.hover_category >= NUM_CATEGORIES) {
            g_settings.hover_category = -1;
        }
    } else {
        g_settings.hover_category = -1;
    }
    
    if (g_settings.hover_category != old_hover) {
        redraw();
    }
}

static void on_mouse_button(client_window_t *win, int32_t x, int32_t y, int button, bool pressed) {
    if (!pressed || button != 1) return;
    
    // Sidebar click
    if (x < SIDEBAR_WIDTH && y >= 60) {
        int cat = (y - 60) / 44;
        if (cat >= 0 && cat < NUM_CATEGORIES) {
            g_settings.current_category = cat;
            redraw();
        }
        return;
    }
    
    // Content area click handling
    int content_x = SIDEBAR_WIDTH + 30;
    int rel_x = x - content_x;
    
    switch (g_settings.current_category) {
        case CAT_DISPLAY:
            // Brightness slider
            if (y >= 95 && y < 115 && rel_x >= 0 && rel_x < 300) {
                g_settings.brightness = (rel_x * 100) / 300;
                redraw();
            }
            // Resolution buttons
            if (y >= 150 && y < 180) {
                int res_idx = rel_x / 120;
                if (res_idx >= 0 && res_idx < 3) {
                    g_settings.resolution_idx = res_idx;
                    redraw();
                }
            }
            break;
            
        case CAT_AUDIO:
            // Volume slider
            if (y >= 95 && y < 115 && rel_x >= 0 && rel_x < 300) {
                g_settings.volume = (rel_x * 100) / 300;
                g_settings.muted = false;
                redraw();
            }
            // Mute checkbox
            if (y >= 135 && y < 155 && rel_x >= 0 && rel_x < 20) {
                g_settings.muted = !g_settings.muted;
                redraw();
            }
            break;
            
        case CAT_SYSTEM:
            // Restart button
            if (y >= 140 && y < 176 && rel_x >= 0 && rel_x < 120) {
                printf("[Settings] Restart requested\n");
                // TODO: Send restart command
            }
            // Shutdown button  
            if (y >= 140 && y < 176 && rel_x >= 140 && rel_x < 260) {
                printf("[Settings] Shutdown requested\n");
                // TODO: Send shutdown command
            }
            break;
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    printf("MayteraOS Settings\n");
    
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.current_category = CAT_DISPLAY;
    g_settings.hover_category = -1;
    g_settings.brightness = 75;
    g_settings.resolution_idx = 0;
    g_settings.volume = 50;
    g_settings.muted = false;
    g_settings.running = true;
    strcpy(g_settings.os_version, "1.8.0");
    strcpy(g_settings.hostname, "maytera");
    
    // Connect to compositor
    g_settings.conn = compositor_connect("Settings");
    if (!g_settings.conn) {
        printf("[Settings] Failed to connect to compositor\n");
        return 1;
    }
    
    // Create window
    g_settings.window = window_create(g_settings.conn, "Settings",
                                       -1, -1, 700, 500,
                                       WIN_FLAG_RESIZABLE);
    if (!g_settings.window) {
        printf("[Settings] Failed to create window\n");
        compositor_disconnect(g_settings.conn);
        return 1;
    }
    
    // Set event handlers
    g_settings.window->on_close = on_close;
    g_settings.window->on_resize = on_resize;
    g_settings.window->on_mouse_move = on_mouse_move;
    g_settings.window->on_mouse_button = on_mouse_button;
    
    // Initial draw
    redraw();
    window_show(g_settings.window);
    
    // Event loop
    while (g_settings.running && g_settings.conn->num_windows > 0) {
        compositor_wait_events(g_settings.conn, 100);
        
        // Update time display if on datetime category
        if (g_settings.current_category == CAT_DATETIME) {
            redraw();
        }
    }
    
    // Cleanup
    compositor_disconnect(g_settings.conn);
    
    printf("[Settings] Exiting\n");
    return 0;
}
