// open_with_dialog.c - "Open With" dialog for MayteraOS

#include "open_with_dialog.h"
#include "associations.h"
#include "mime.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../gui/window.h"
#include "../gui/desktop.h"
#include "../drivers/mouse.h"

static open_with_dialog_t *g_active_dialog = NULL;

// ============================================================================
// Helper Functions
// ============================================================================

static void dialog_draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    while (*text) {
        const uint8_t *glyph = font_get_glyph(*text);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(x + col, y + row, color);
                    }
                }
            }
        }
        x += FONT_WIDTH;
        text++;
    }
}

static void dialog_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    fb_fill_rect(x, y, w, h, color & 0xFFFFFF);
}

static void dialog_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    for (int i = 0; i < w; i++) {
        fb_put_pixel(x + i, y, color & 0xFFFFFF);
        fb_put_pixel(x + i, y + h - 1, color & 0xFFFFFF);
    }
    for (int i = 0; i < h; i++) {
        fb_put_pixel(x, y + i, color & 0xFFFFFF);
        fb_put_pixel(x + w - 1, y + i, color & 0xFFFFFF);
    }
}

static const char *get_filename(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') name = p + 1;
    }
    return name;
}

// ============================================================================
// Drawing Functions
// ============================================================================

static void dialog_draw_app_list(open_with_dialog_t *dialog) {
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(dialog->window, &wx, &wy, &ww, &wh);
    
    int32_t list_x = wx + OPENWITH_PADDING;
    int32_t list_y = wy + 60;
    int32_t list_w = ww - OPENWITH_PADDING * 2;
    int32_t list_h = wh - 140;
    
    dialog_fill_rect(list_x, list_y, list_w, list_h, OPENWITH_ITEM_BG);
    dialog_draw_rect(list_x, list_y, list_w, list_h, OPENWITH_BORDER);
    
    int visible_items = list_h / OPENWITH_ITEM_HEIGHT;
    int start_idx = dialog->scroll_offset;
    int end_idx = start_idx + visible_items;
    if (end_idx > dialog->app_count) end_idx = dialog->app_count;
    
    int32_t item_y = list_y + 2;
    for (int i = start_idx; i < end_idx; i++) {
        assoc_app_t *app = dialog->apps[i];
        if (!app) continue;
        
        int32_t item_x = list_x + 2;
        int32_t item_w = list_w - 4;
        
        uint32_t bg_color = OPENWITH_ITEM_BG;
        if (i == dialog->selected_index) {
            bg_color = OPENWITH_ITEM_SELECTED;
        } else if (i == dialog->hover_index) {
            bg_color = OPENWITH_ITEM_HOVER;
        }
        
        dialog_fill_rect(item_x, item_y, item_w, OPENWITH_ITEM_HEIGHT - 2, bg_color);
        dialog_fill_rect(item_x + 4, item_y + 6, 24, 24, 0xFF4A90C2);
        dialog_draw_text(item_x + 36, item_y + 4, app->name, OPENWITH_TEXT);
        
        if (app->description[0]) {
            dialog_draw_text(item_x + 36, item_y + 20, app->description, OPENWITH_TEXT_DESC);
        }
        
        item_y += OPENWITH_ITEM_HEIGHT;
    }
}

static void dialog_draw_checkbox(open_with_dialog_t *dialog) {
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(dialog->window, &wx, &wy, &ww, &wh);
    
    int32_t cb_x = wx + OPENWITH_PADDING;
    int32_t cb_y = wy + wh - 70;
    
    uint32_t cb_bg = dialog->checkbox_hover ? OPENWITH_ITEM_HOVER : OPENWITH_CHECKBOX_BG;
    dialog_fill_rect(cb_x, cb_y, OPENWITH_CHECKBOX_SIZE, OPENWITH_CHECKBOX_SIZE, cb_bg);
    dialog_draw_rect(cb_x, cb_y, OPENWITH_CHECKBOX_SIZE, OPENWITH_CHECKBOX_SIZE, OPENWITH_BORDER);
    
    if (dialog->always_use) {
        for (int i = 0; i < 4; i++) {
            fb_put_pixel(cb_x + 3 + i, cb_y + 8 + i, OPENWITH_CHECKBOX_CHECK);
        }
        for (int i = 0; i < 8; i++) {
            fb_put_pixel(cb_x + 6 + i, cb_y + 11 - i, OPENWITH_CHECKBOX_CHECK);
        }
    }
    
    dialog_draw_text(cb_x + OPENWITH_CHECKBOX_SIZE + 8, cb_y + 2, 
                     "Always use this application", OPENWITH_TEXT);
}

static void dialog_draw_buttons(open_with_dialog_t *dialog) {
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(dialog->window, &wx, &wy, &ww, &wh);
    
    int32_t btn_y = wy + wh - 38;
    
    int32_t ok_x = wx + ww - OPENWITH_PADDING - OPENWITH_BUTTON_WIDTH;
    uint32_t ok_bg = dialog->ok_hover ? OPENWITH_BUTTON_HOVER : OPENWITH_BUTTON_BG;
    dialog_fill_rect(ok_x, btn_y, OPENWITH_BUTTON_WIDTH, OPENWITH_BUTTON_HEIGHT, ok_bg);
    dialog_draw_rect(ok_x, btn_y, OPENWITH_BUTTON_WIDTH, OPENWITH_BUTTON_HEIGHT, OPENWITH_BORDER);
    dialog_draw_text(ok_x + 28, btn_y + 7, "OK", OPENWITH_BUTTON_TEXT);
    
    int32_t cancel_x = ok_x - 10 - OPENWITH_BUTTON_WIDTH;
    uint32_t cancel_bg = dialog->cancel_hover ? OPENWITH_BUTTON_HOVER : OPENWITH_BUTTON_BG;
    dialog_fill_rect(cancel_x, btn_y, OPENWITH_BUTTON_WIDTH, OPENWITH_BUTTON_HEIGHT, cancel_bg);
    dialog_draw_rect(cancel_x, btn_y, OPENWITH_BUTTON_WIDTH, OPENWITH_BUTTON_HEIGHT, OPENWITH_BORDER);
    dialog_draw_text(cancel_x + 16, btn_y + 7, "Cancel", OPENWITH_BUTTON_TEXT);
    
    int32_t browse_x = wx + OPENWITH_PADDING;
    uint32_t browse_bg = dialog->browse_hover ? OPENWITH_BUTTON_HOVER : OPENWITH_BUTTON_BG;
    dialog_fill_rect(browse_x, btn_y, OPENWITH_BUTTON_WIDTH, OPENWITH_BUTTON_HEIGHT, browse_bg);
    dialog_draw_rect(browse_x, btn_y, OPENWITH_BUTTON_WIDTH, OPENWITH_BUTTON_HEIGHT, OPENWITH_BORDER);
    dialog_draw_text(browse_x + 12, btn_y + 7, "Browse...", OPENWITH_BUTTON_TEXT);
}

void open_with_dialog_draw(open_with_dialog_t *dialog) {
    if (!dialog || !dialog->window) return;
    
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(dialog->window, &wx, &wy, &ww, &wh);
    
    dialog_fill_rect(wx, wy, ww, wh, OPENWITH_BG);
    
    dialog_draw_text(wx + OPENWITH_PADDING, wy + 10, "Choose an application to open:", 
                     OPENWITH_TEXT);
    
    const char *filename = get_filename(dialog->filepath);
    dialog_draw_text(wx + OPENWITH_PADDING, wy + 30, filename, 0xFF0066CC);
    
    char mime_info[128];
    snprintf(mime_info, sizeof(mime_info), "Type: %s", 
             mime_get_description(dialog->mime_type));
    dialog_draw_text(wx + OPENWITH_PADDING, wy + 46, mime_info, OPENWITH_TEXT_DESC);
    
    dialog_draw_app_list(dialog);
    dialog_draw_checkbox(dialog);
    dialog_draw_buttons(dialog);
}

// ============================================================================
// Event Handling
// ============================================================================

static void dialog_handle_click(open_with_dialog_t *dialog, int32_t mx, int32_t my) {
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(dialog->window, &wx, &wy, &ww, &wh);
    
    int32_t list_x = wx + OPENWITH_PADDING;
    int32_t list_y = wy + 60;
    int32_t list_w = ww - OPENWITH_PADDING * 2;
    int32_t list_h = wh - 140;
    
    if (mx >= list_x && mx < list_x + list_w &&
        my >= list_y && my < list_y + list_h) {
        int item_idx = (my - list_y) / OPENWITH_ITEM_HEIGHT + dialog->scroll_offset;
        if (item_idx >= 0 && item_idx < dialog->app_count) {
            dialog->selected_index = item_idx;
        }
        return;
    }
    
    int32_t cb_x = wx + OPENWITH_PADDING;
    int32_t cb_y = wy + wh - 70;
    int32_t cb_w = OPENWITH_CHECKBOX_SIZE + 200;
    
    if (mx >= cb_x && mx < cb_x + cb_w &&
        my >= cb_y && my < cb_y + OPENWITH_CHECKBOX_SIZE) {
        dialog->always_use = !dialog->always_use;
        return;
    }
    
    int32_t btn_y = wy + wh - 38;
    
    int32_t ok_x = wx + ww - OPENWITH_PADDING - OPENWITH_BUTTON_WIDTH;
    if (mx >= ok_x && mx < ok_x + OPENWITH_BUTTON_WIDTH &&
        my >= btn_y && my < btn_y + OPENWITH_BUTTON_HEIGHT) {
        if (dialog->selected_index >= 0 && dialog->selected_index < dialog->app_count) {
            assoc_app_t *app = dialog->apps[dialog->selected_index];
            if (app) {
                if (dialog->always_use) {
                    assoc_set_default(dialog->mime_type, app->name);
                    assoc_save_config();
                }
                if (dialog->callback) {
                    dialog->callback(dialog->filepath, app->name, dialog->always_use);
                }
                assoc_open_with(dialog->filepath, app->name);
            }
        }
        dialog->running = false;
        return;
    }
    
    int32_t cancel_x = ok_x - 10 - OPENWITH_BUTTON_WIDTH;
    if (mx >= cancel_x && mx < cancel_x + OPENWITH_BUTTON_WIDTH &&
        my >= btn_y && my < btn_y + OPENWITH_BUTTON_HEIGHT) {
        dialog->running = false;
        return;
    }
}

static void dialog_handle_motion(open_with_dialog_t *dialog, int32_t mx, int32_t my) {
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(dialog->window, &wx, &wy, &ww, &wh);
    
    dialog->hover_index = -1;
    dialog->checkbox_hover = false;
    dialog->ok_hover = false;
    dialog->cancel_hover = false;
    dialog->browse_hover = false;
    
    int32_t list_x = wx + OPENWITH_PADDING;
    int32_t list_y = wy + 60;
    int32_t list_w = ww - OPENWITH_PADDING * 2;
    int32_t list_h = wh - 140;
    
    if (mx >= list_x && mx < list_x + list_w &&
        my >= list_y && my < list_y + list_h) {
        int item_idx = (my - list_y) / OPENWITH_ITEM_HEIGHT + dialog->scroll_offset;
        if (item_idx >= 0 && item_idx < dialog->app_count) {
            dialog->hover_index = item_idx;
        }
        return;
    }
    
    int32_t cb_x = wx + OPENWITH_PADDING;
    int32_t cb_y = wy + wh - 70;
    int32_t cb_w = OPENWITH_CHECKBOX_SIZE + 200;
    
    if (mx >= cb_x && mx < cb_x + cb_w &&
        my >= cb_y && my < cb_y + OPENWITH_CHECKBOX_SIZE) {
        dialog->checkbox_hover = true;
        return;
    }
    
    int32_t btn_y = wy + wh - 38;
    
    int32_t ok_x = wx + ww - OPENWITH_PADDING - OPENWITH_BUTTON_WIDTH;
    if (mx >= ok_x && mx < ok_x + OPENWITH_BUTTON_WIDTH &&
        my >= btn_y && my < btn_y + OPENWITH_BUTTON_HEIGHT) {
        dialog->ok_hover = true;
        return;
    }
    
    int32_t cancel_x = ok_x - 10 - OPENWITH_BUTTON_WIDTH;
    if (mx >= cancel_x && mx < cancel_x + OPENWITH_BUTTON_WIDTH &&
        my >= btn_y && my < btn_y + OPENWITH_BUTTON_HEIGHT) {
        dialog->cancel_hover = true;
        return;
    }
    
    int32_t browse_x = wx + OPENWITH_PADDING;
    if (mx >= browse_x && mx < browse_x + OPENWITH_BUTTON_WIDTH &&
        my >= btn_y && my < btn_y + OPENWITH_BUTTON_HEIGHT) {
        dialog->browse_hover = true;
        return;
    }
}

static void dialog_handle_scroll(open_with_dialog_t *dialog, int delta) {
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(dialog->window, &wx, &wy, &ww, &wh);
    
    int32_t list_h = wh - 140;
    int visible_items = list_h / OPENWITH_ITEM_HEIGHT;
    int max_scroll = dialog->app_count - visible_items;
    if (max_scroll < 0) max_scroll = 0;
    
    dialog->scroll_offset -= delta;
    if (dialog->scroll_offset < 0) dialog->scroll_offset = 0;
    if (dialog->scroll_offset > max_scroll) dialog->scroll_offset = max_scroll;
}

// ============================================================================
// Window Manager Callbacks
// ============================================================================

void open_with_dialog_on_event(void *app_data, gui_event_t *event) {
    open_with_dialog_t *dialog = (open_with_dialog_t *)app_data;
    if (!dialog || !event) return;
    
    switch (event->type) {
        case EVENT_MOUSE_DOWN:
            dialog_handle_click(dialog, event->mouse_x, event->mouse_y);
            open_with_dialog_draw(dialog);
            break;
            
        case EVENT_MOUSE_MOVE:
            dialog_handle_motion(dialog, event->mouse_x, event->mouse_y);
            open_with_dialog_draw(dialog);
            break;
            
        case EVENT_MOUSE_SCROLL:
            dialog_handle_scroll(dialog, event->scroll_delta);
            open_with_dialog_draw(dialog);
            break;
            
        case EVENT_KEY_DOWN:
            if (event->keycode == 0x01) {  // Escape
                dialog->running = false;
            } else if (event->keycode == 0x1C) {  // Enter
                if (dialog->selected_index >= 0) {
                    int32_t wx, wy, ww, wh;
                    window_get_content_bounds(dialog->window, &wx, &wy, &ww, &wh);
                    dialog_handle_click(dialog, 
                                        wx + ww - OPENWITH_PADDING - OPENWITH_BUTTON_WIDTH/2,
                                        wy + wh - 38 + OPENWITH_BUTTON_HEIGHT/2);
                }
            }
            break;
            
        case EVENT_WINDOW_CLOSE:
            dialog->running = false;
            break;
            
        default:
            break;
    }
}

void open_with_dialog_on_draw(void *app_data) {
    open_with_dialog_t *dialog = (open_with_dialog_t *)app_data;
    if (dialog) {
        open_with_dialog_draw(dialog);
    }
}

void open_with_dialog_on_destroy(void *app_data) {
    open_with_dialog_t *dialog = (open_with_dialog_t *)app_data;
    if (dialog) {
        dialog->running = false;
    }
}

// ============================================================================
// Dialog API
// ============================================================================

open_with_dialog_t *open_with_dialog_create(const char *filepath, 
                                             open_with_callback_t callback) {
    if (!filepath) return NULL;
    
    assoc_init();
    
    open_with_dialog_t *dialog = kmalloc(sizeof(open_with_dialog_t));
    if (!dialog) return NULL;
    
    memset(dialog, 0, sizeof(open_with_dialog_t));
    
    strncpy(dialog->filepath, filepath, sizeof(dialog->filepath) - 1);
    
    const char *ext = filepath;
    for (const char *p = filepath; *p; p++) {
        if (*p == '.') ext = p;
        if (*p == '/') ext = filepath;
    }
    const char *mime = mime_type_from_extension(ext);
    strncpy(dialog->mime_type, mime, sizeof(dialog->mime_type) - 1);
    
    dialog->app_count = assoc_get_all(dialog->mime_type, dialog->apps);
    
    if (dialog->app_count == 0) {
        int total = 0;
        assoc_app_t *all_apps = assoc_get_all_apps(&total);
        for (int i = 0; i < total && dialog->app_count < OPENWITH_MAX_APPS; i++) {
            if (all_apps[i].registered && (all_apps[i].capabilities & APP_CAP_OPEN)) {
                dialog->apps[dialog->app_count++] = &all_apps[i];
            }
        }
    }
    
    dialog->selected_index = -1;
    dialog->hover_index = -1;
    dialog->scroll_offset = 0;
    dialog->always_use = false;
    dialog->callback = callback;
    dialog->running = true;
    
    assoc_app_t *default_app = assoc_get_default(dialog->mime_type);
    if (default_app) {
        for (int i = 0; i < dialog->app_count; i++) {
            if (dialog->apps[i] == default_app) {
                dialog->selected_index = i;
                break;
            }
        }
    }
    
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();
    int32_t win_x = (screen_w - OPENWITH_WIDTH) / 2;
    int32_t win_y = (screen_h - OPENWITH_HEIGHT) / 2;
    
    dialog->window = window_create("Open With", win_x, win_y, 
                                   OPENWITH_WIDTH, OPENWITH_HEIGHT);
    if (!dialog->window) {
        kfree(dialog);
        return NULL;
    }
    
    dialog->app_id = wm_register_app(dialog->window, dialog,
                                      open_with_dialog_on_event,
                                      open_with_dialog_on_draw,
                                      open_with_dialog_on_destroy);
    
    g_active_dialog = dialog;
    
    open_with_dialog_draw(dialog);
    
    kprintf("[OpenWith] Created dialog for: %s (%s)\n", filepath, dialog->mime_type);
    
    return dialog;
}

void open_with_dialog_destroy(open_with_dialog_t *dialog) {
    if (!dialog) return;
    
    if (dialog->window) {
        window_destroy(dialog->window);
    }
    
    if (g_active_dialog == dialog) {
        g_active_dialog = NULL;
    }
    
    kfree(dialog);
}

void open_with_dialog_run(open_with_dialog_t *dialog) {
    if (!dialog) return;
    
    while (dialog->running) {
        extern void wm_process_events(void);
        wm_process_events();
        
        for (volatile int i = 0; i < 10000; i++);
    }
    
    open_with_dialog_destroy(dialog);
}

void assoc_show_open_with_dialog(const char *filepath, open_with_callback_t callback) {
    open_with_dialog_t *dialog = open_with_dialog_create(filepath, callback);
    if (dialog) {
        open_with_dialog_run(dialog);
    }
}
