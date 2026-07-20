// syslog.c - System Log and Log Viewer for MayteraOS
#include "syslog.h"
#include "window.h"
#include "icons.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../video/font.h"

// External timer
extern volatile uint64_t timer_ticks;

// Global log storage
static log_entry_t g_log_entries[SYSLOG_MAX_ENTRIES];
static int g_log_count = 0;
static int g_log_head = 0;  // Circular buffer head
static bool g_log_initialized = false;

// Colors
#define SL_BG_COLOR         0x1A1A2E
#define SL_HEADER_COLOR     0x252540
#define SL_SELECT_COLOR     0x3D3D5D
#define SL_TEXT_COLOR       0xE0E0E0
#define SL_TEXT_DIM_COLOR   0x707070
#define SL_BORDER_COLOR     0x404060

// Level colors
#define SL_COLOR_DEBUG      0x808080
#define SL_COLOR_INFO       0x40C040
#define SL_COLOR_WARNING    0xE0E040
#define SL_COLOR_ERROR      0xE04040
#define SL_COLOR_CRITICAL   0xFF0000

// UI dimensions
#define SL_ROW_HEIGHT       18
#define SL_HEADER_HEIGHT    28
#define SL_FOOTER_HEIGHT    32
#define SL_PADDING          6

// Initialize system log
void syslog_init(void) {
    if (g_log_initialized) return;

    memset(g_log_entries, 0, sizeof(g_log_entries));
    g_log_count = 0;
    g_log_head = 0;
    g_log_initialized = true;
}

// Level prefixes for serial output
static const char *level_prefix[] = {
    "[DBG]", "[INF]", "[WRN]", "[ERR]", "[CRT]"
};

// Add log entry
void syslog_log(log_level_t level, const char *message) {
    if (!g_log_initialized) {
        syslog_init();
    }

    if (!message) return;

    // Always output to serial for persistence
    uint32_t secs = (uint32_t)(timer_ticks / 100);
    int mins = secs / 60;
    secs = secs % 60;
    kprintf("%02d:%02d %s %s\n", mins, secs, level_prefix[level], message);

    // Find insertion point (circular buffer)
    int idx;
    if (g_log_count < SYSLOG_MAX_ENTRIES) {
        idx = g_log_count;
        g_log_count++;
    } else {
        // Overwrite oldest entry
        idx = g_log_head;
        g_log_head = (g_log_head + 1) % SYSLOG_MAX_ENTRIES;
    }

    log_entry_t *entry = &g_log_entries[idx];
    strncpy(entry->message, message, SYSLOG_MSG_MAX - 1);
    entry->message[SYSLOG_MSG_MAX - 1] = '\0';
    entry->level = level;
    entry->timestamp = timer_ticks;
    entry->valid = true;
}

// Get log count
int syslog_get_count(void) {
    return g_log_count;
}

// Get log entry (0 = oldest)
log_entry_t *syslog_get_entry(int index) {
    if (index < 0 || index >= g_log_count) {
        return NULL;
    }

    // Convert to circular buffer index
    int real_idx = (g_log_head + index) % SYSLOG_MAX_ENTRIES;
    return &g_log_entries[real_idx];
}

// Clear all logs
void syslog_clear(void) {
    for (int i = 0; i < SYSLOG_MAX_ENTRIES; i++) {
        g_log_entries[i].valid = false;
    }
    g_log_count = 0;
    g_log_head = 0;
}

// Create log viewer
syslog_viewer_t *syslog_viewer_create(void) {
    syslog_init();

    syslog_viewer_t *sv = (syslog_viewer_t *)kmalloc(sizeof(syslog_viewer_t));
    if (!sv) return NULL;

    memset(sv, 0, sizeof(syslog_viewer_t));

    sv->window = window_create("System Log", 100, 60, 550, 400);
    if (!sv->window) {
        kfree(sv);
        return NULL;
    }

    sv->scroll_offset = 0;
    sv->selected = -1;
    sv->filter_level = LOG_DEBUG;  // Show all by default
    sv->auto_scroll = true;
    sv->last_update = timer_ticks;

    // Log that viewer was opened
    syslog_log(LOG_INFO, "[SysLog] Log viewer opened");

    return sv;
}

// Destroy log viewer
void syslog_viewer_destroy(syslog_viewer_t *sv) {
    if (!sv) return;
    if (sv->window) {
        window_destroy(sv->window);
    }
    kfree(sv);
}

// Get level color
static uint32_t get_level_color(log_level_t level) {
    switch (level) {
        case LOG_DEBUG:    return SL_COLOR_DEBUG;
        case LOG_INFO:     return SL_COLOR_INFO;
        case LOG_WARNING:  return SL_COLOR_WARNING;
        case LOG_ERROR:    return SL_COLOR_ERROR;
        case LOG_CRITICAL: return SL_COLOR_CRITICAL;
        default:           return SL_TEXT_COLOR;
    }
}

// Get level name
static const char *get_level_name(log_level_t level) {
    switch (level) {
        case LOG_DEBUG:    return "DBG";
        case LOG_INFO:     return "INF";
        case LOG_WARNING:  return "WRN";
        case LOG_ERROR:    return "ERR";
        case LOG_CRITICAL: return "CRT";
        default:           return "???";
    }
}

// Draw text helper
static void sl_draw_text(const char *text, int x, int y, uint32_t color) {
    for (int i = 0; text[i]; i++) {
        const uint8_t *glyph = font_get_glyph(text[i]);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    fb_put_pixel(x + i * 8 + col, y + row, color);
                }
            }
        }
    }
}

// Draw button
static void sl_draw_button(int x, int y, int w, int h, const char *label, bool active) {
    uint32_t bg = active ? 0x505070 : 0x353550;
    fb_fill_rect(x, y, w, h, bg);
    fb_draw_rect(x, y, w, h, SL_BORDER_COLOR);

    int text_len = strlen(label) * 8;
    int text_x = x + (w - text_len) / 2;
    int text_y = y + (h - 16) / 2;
    sl_draw_text(label, text_x, text_y, SL_TEXT_COLOR);
}

// Count visible entries (respecting filter)
static int count_visible_entries(syslog_viewer_t *sv) {
    int count = 0;
    for (int i = 0; i < g_log_count; i++) {
        log_entry_t *entry = syslog_get_entry(i);
        if (entry && entry->level >= sv->filter_level) {
            count++;
        }
    }
    return count;
}

// Get nth visible entry
static log_entry_t *get_visible_entry(syslog_viewer_t *sv, int visible_index) {
    int count = 0;
    for (int i = 0; i < g_log_count; i++) {
        log_entry_t *entry = syslog_get_entry(i);
        if (entry && entry->level >= sv->filter_level) {
            if (count == visible_index) {
                return entry;
            }
            count++;
        }
    }
    return NULL;
}

// Event handling
void syslog_viewer_handle_event(syslog_viewer_t *sv, gui_event_t *event) {
    if (!sv || !event) return;

    switch (event->type) {
        case EVENT_KEY_DOWN:
            switch (event->keycode) {
                case 0x48:  // Up arrow
                    if (sv->selected > 0) sv->selected--;
                    sv->auto_scroll = false;
                    break;
                case 0x50:  // Down arrow
                    if (sv->selected < count_visible_entries(sv) - 1) sv->selected++;
                    break;
                case 0x49:  // Page Up
                    sv->scroll_offset -= 10;
                    if (sv->scroll_offset < 0) sv->scroll_offset = 0;
                    sv->auto_scroll = false;
                    break;
                case 0x51:  // Page Down
                    sv->scroll_offset += 10;
                    break;
                case 0x47:  // Home
                    sv->scroll_offset = 0;
                    sv->auto_scroll = false;
                    break;
                case 0x4F:  // End
                    sv->auto_scroll = true;
                    break;
                case 'c':
                case 'C':
                    syslog_clear();
                    sv->selected = -1;
                    sv->scroll_offset = 0;
                    break;
                case '1':
                    sv->filter_level = LOG_DEBUG;
                    sv->scroll_offset = 0;
                    break;
                case '2':
                    sv->filter_level = LOG_INFO;
                    sv->scroll_offset = 0;
                    break;
                case '3':
                    sv->filter_level = LOG_WARNING;
                    sv->scroll_offset = 0;
                    break;
                case '4':
                    sv->filter_level = LOG_ERROR;
                    sv->scroll_offset = 0;
                    break;
                case 'a':
                case 'A':
                    sv->auto_scroll = !sv->auto_scroll;
                    break;
            }
            break;

        case EVENT_MOUSE_DOWN:
            {
                int32_t wx, wy, ww, wh;
                window_get_content_bounds(sv->window, &wx, &wy, &ww, &wh);

                int mx = event->mouse_x;
                int my = event->mouse_y;

                // Check list area
                int list_y = wy + SL_HEADER_HEIGHT;
                int list_h = wh - SL_HEADER_HEIGHT - SL_FOOTER_HEIGHT;

                if (my >= list_y && my < list_y + list_h) {
                    int clicked_idx = (my - list_y) / SL_ROW_HEIGHT + sv->scroll_offset;
                    int visible_count = count_visible_entries(sv);
                    if (clicked_idx >= 0 && clicked_idx < visible_count) {
                        sv->selected = clicked_idx;
                        sv->auto_scroll = false;
                    }
                }

                // Check footer buttons
                int footer_y = wy + wh - SL_FOOTER_HEIGHT;
                int btn_y = footer_y + 4;
                int btn_h = 24;

                // Clear button
                int clear_x = wx + SL_PADDING;
                if (mx >= clear_x && mx < clear_x + 50 &&
                    my >= btn_y && my < btn_y + btn_h) {
                    syslog_clear();
                    sv->selected = -1;
                    sv->scroll_offset = 0;
                }

                // Filter buttons
                int filter_x = wx + 60;
                int filter_w = 32;
                for (int i = 0; i < 4; i++) {
                    int bx = filter_x + i * (filter_w + 4);
                    if (mx >= bx && mx < bx + filter_w &&
                        my >= btn_y && my < btn_y + btn_h) {
                        sv->filter_level = (log_level_t)i;
                        sv->scroll_offset = 0;
                    }
                }

                // Auto-scroll button
                int auto_x = wx + ww - SL_PADDING - 50;
                if (mx >= auto_x && mx < auto_x + 50 &&
                    my >= btn_y && my < btn_y + btn_h) {
                    sv->auto_scroll = !sv->auto_scroll;
                }
            }
            break;

        default:
            break;
    }
}

// Drawing
void syslog_viewer_draw(syslog_viewer_t *sv) {
    if (!sv || !sv->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(sv->window, &wx, &wy, &ww, &wh);

    // Background
    fb_fill_rect(wx, wy, ww, wh, SL_BG_COLOR);

    // Header
    fb_fill_rect(wx, wy, ww, SL_HEADER_HEIGHT, SL_HEADER_COLOR);

    // Log icon
    icon_draw(ICON_LOG_VIEWER, wx + SL_PADDING, wy + 2, SL_TEXT_COLOR);

    sl_draw_text("System Log", wx + SL_PADDING + 28, wy + 6, SL_TEXT_COLOR);

    // Entry count
    int visible_count = count_visible_entries(sv);
    char count_str[32];
    char *cp = count_str;
    int val = visible_count;
    if (val == 0) {
        *cp++ = '0';
    } else {
        char tmp[8];
        int i = 0;
        while (val > 0) {
            tmp[i++] = '0' + (val % 10);
            val /= 10;
        }
        while (i > 0) *cp++ = tmp[--i];
    }
    *cp++ = '/';
    val = g_log_count;
    if (val == 0) {
        *cp++ = '0';
    } else {
        char tmp[8];
        int i = 0;
        while (val > 0) {
            tmp[i++] = '0' + (val % 10);
            val /= 10;
        }
        while (i > 0) *cp++ = tmp[--i];
    }
    *cp = '\0';

    int count_w = strlen(count_str) * 8;
    sl_draw_text(count_str, wx + ww - SL_PADDING - count_w, wy + 6, SL_TEXT_DIM_COLOR);

    // Separator
    fb_fill_rect(wx, wy + SL_HEADER_HEIGHT, ww, 1, SL_BORDER_COLOR);

    // Log list
    int list_y = wy + SL_HEADER_HEIGHT + 1;
    int list_h = wh - SL_HEADER_HEIGHT - SL_FOOTER_HEIGHT - 1;
    int visible_rows = list_h / SL_ROW_HEIGHT;

    // Auto-scroll to bottom
    if (sv->auto_scroll && visible_count > visible_rows) {
        sv->scroll_offset = visible_count - visible_rows;
    }

    // Clamp scroll offset
    if (sv->scroll_offset > visible_count - visible_rows) {
        sv->scroll_offset = visible_count - visible_rows;
    }
    if (sv->scroll_offset < 0) sv->scroll_offset = 0;

    if (visible_count == 0) {
        sl_draw_text("No log entries", wx + ww / 2 - 56, list_y + list_h / 2 - 8, SL_TEXT_DIM_COLOR);
    } else {
        for (int i = 0; i < visible_rows && i + sv->scroll_offset < visible_count; i++) {
            int idx = i + sv->scroll_offset;
            log_entry_t *entry = get_visible_entry(sv, idx);
            if (!entry) continue;

            int row_y = list_y + i * SL_ROW_HEIGHT;

            // Selection highlight
            if (idx == sv->selected) {
                fb_fill_rect(wx, row_y, ww, SL_ROW_HEIGHT, SL_SELECT_COLOR);
            }

            // Level tag
            uint32_t level_color = get_level_color(entry->level);
            const char *level_name = get_level_name(entry->level);
            sl_draw_text(level_name, wx + SL_PADDING, row_y + 1, level_color);

            // Timestamp (simplified - just show ticks as seconds)
            char time_str[16];
            uint32_t secs = (uint32_t)(entry->timestamp / 100);  // Assuming 100 ticks/sec
            int mins = secs / 60;
            secs = secs % 60;
            int j = 0;
            if (mins < 10) time_str[j++] = '0';
            if (mins == 0) {
                time_str[j++] = '0';
            } else {
                char tmp[8];
                int k = 0;
                while (mins > 0) {
                    tmp[k++] = '0' + (mins % 10);
                    mins /= 10;
                }
                while (k > 0) time_str[j++] = tmp[--k];
            }
            time_str[j++] = ':';
            if (secs < 10) time_str[j++] = '0';
            if (secs == 0) {
                time_str[j++] = '0';
            } else {
                char tmp[8];
                int k = 0;
                while (secs > 0) {
                    tmp[k++] = '0' + (secs % 10);
                    secs /= 10;
                }
                while (k > 0) time_str[j++] = tmp[--k];
            }
            time_str[j] = '\0';
            sl_draw_text(time_str, wx + SL_PADDING + 32, row_y + 1, SL_TEXT_DIM_COLOR);

            // Message
            int msg_x = wx + SL_PADDING + 80;
            int max_chars = (ww - 80 - SL_PADDING * 2) / 8;
            char msg_truncated[SYSLOG_MSG_MAX];
            strncpy(msg_truncated, entry->message, max_chars);
            msg_truncated[max_chars] = '\0';
            sl_draw_text(msg_truncated, msg_x, row_y + 1, SL_TEXT_COLOR);
        }
    }

    // Footer separator
    int footer_y = wy + wh - SL_FOOTER_HEIGHT;
    fb_fill_rect(wx, footer_y, ww, 1, SL_BORDER_COLOR);

    // Footer
    fb_fill_rect(wx, footer_y + 1, ww, SL_FOOTER_HEIGHT - 1, SL_HEADER_COLOR);

    int btn_y = footer_y + 4;

    // Clear button
    sl_draw_button(wx + SL_PADDING, btn_y, 50, 24, "Clear", true);

    // Filter buttons
    const char *filter_labels[] = {"D", "I", "W", "E"};
    uint32_t filter_colors[] = {SL_COLOR_DEBUG, SL_COLOR_INFO, SL_COLOR_WARNING, SL_COLOR_ERROR};
    int filter_x = wx + 60;
    for (int i = 0; i < 4; i++) {
        bool active = (sv->filter_level == (log_level_t)i);
        fb_fill_rect(filter_x + i * 36, btn_y, 32, 24, active ? 0x505070 : 0x353550);
        fb_draw_rect(filter_x + i * 36, btn_y, 32, 24, SL_BORDER_COLOR);
        sl_draw_text(filter_labels[i], filter_x + i * 36 + 12, btn_y + 4, filter_colors[i]);
    }

    // Auto-scroll indicator
    sl_draw_button(wx + ww - SL_PADDING - 50, btn_y, 50, 24,
                   sv->auto_scroll ? "Auto" : "Man", sv->auto_scroll);
}

// Launch log viewer
void syslog_viewer_launch(void) {
    syslog_viewer_t *sv = syslog_viewer_create();
    if (!sv) {
        kprintf("[SysLog] Failed to create log viewer\n");
        return;
    }

    wm_register_app(sv->window, sv,
                    (app_event_handler_t)syslog_viewer_handle_event,
                    (app_draw_handler_t)syslog_viewer_draw,
                    (app_destroy_handler_t)syslog_viewer_destroy);

    kprintf("[SysLog] Log viewer launched\n");
}
