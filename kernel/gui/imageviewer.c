// imageviewer.c - Image Viewer Application for MayteraOS
#include "imageviewer.h"
#include "window.h"
#include "desktop.h"
#include "filedialog.h"
#include "icons.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../fs/fat.h"
#include "syslog.h"

// External filesystem
extern fat_fs_t g_fat_fs;

// Forward declarations
static void iv_load_directory(imageviewer_t *iv);
static void iv_draw_image(imageviewer_t *iv, int32_t wx, int32_t wy, int32_t ww, int32_t wh);
static void iv_draw_toolbar(imageviewer_t *iv, int32_t wx, int32_t wy, int32_t ww);
static void iv_draw_info(imageviewer_t *iv, int32_t wx, int32_t wy, int32_t ww, int32_t wh);
static bool iv_is_image_file(const char *name);

// Toolbar height
#define IV_TOOLBAR_HEIGHT 28
#define IV_BUTTON_SIZE    24
#define IV_BUTTON_PADDING 4

// Helper to build a full path from directory and filename
static void iv_build_path(char *dest, const char *dir, const char *file) {
    char *p = dest;
    const char *s = dir;
    while (*s) *p++ = *s++;
    if (p > dest && *(p-1) != '/') *p++ = '/';
    s = file;
    while (*s) *p++ = *s++;
    *p = '\0';
}

// Helper to format an integer into a string (returns pointer past last char written)
static char *iv_itoa(int val, char *buf) {
    char *p = buf;
    if (val == 0) {
        *p++ = '0';
        return p;
    }
    if (val < 0) {
        *p++ = '-';
        val = -val;
    }
    char tmp[12];
    int i = 0;
    while (val > 0) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (i > 0) *p++ = tmp[--i];
    return p;
}

// Create image viewer
imageviewer_t *imageviewer_create(void) {
    imageviewer_t *iv = (imageviewer_t *)kmalloc(sizeof(imageviewer_t));
    if (!iv) return NULL;

    memset(iv, 0, sizeof(imageviewer_t));

    // Create window
    iv->window = window_create("Image Viewer", 100, 50, 640, 480);
    if (!iv->window) {
        kfree(iv);
        return NULL;
    }

    // Initialize state
    iv->mode = IV_MODE_FIT;
    iv->zoom = IV_ZOOM_DEFAULT;
    iv->show_info = false;
    iv->fullscreen = false;

    return iv;
}

// Destroy image viewer
void imageviewer_destroy(imageviewer_t *iv) {
    if (!iv) return;

    if (iv->image) {
        image_free(iv->image);
    }
    if (iv->window) {
        window_destroy(iv->window);
    }
    kfree(iv);
}

// Check if file is an image
static bool iv_is_image_file(const char *name) {
    int len = strlen(name);
    if (len < 4) return false;

    // Check 4-character extensions
    const char *ext = name + len - 4;
    char lower[6];
    for (int i = 0; i < 4 && ext[i]; i++) {
        char c = ext[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    lower[4] = '\0';

    if (strcmp(lower, ".bmp") == 0 ||
        strcmp(lower, ".jpg") == 0 ||
        strcmp(lower, ".png") == 0) {
        return true;
    }

    // Check 5-character extensions (.jpeg, .webp)
    if (len >= 5) {
        ext = name + len - 5;
        for (int i = 0; i < 5 && ext[i]; i++) {
            char c = ext[i];
            lower[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        }
        lower[5] = '\0';

        if (strcmp(lower, ".jpeg") == 0 ||
            strcmp(lower, ".webp") == 0) {
            return true;
        }
    }

    return false;
}

// Load directory listing of images
static void iv_load_directory(imageviewer_t *iv) {
    iv->file_count = 0;
    iv->current_index = -1;

    if (strlen(iv->directory) == 0) return;

    // Open directory
    fat_file_t dir;
    if (fat_open(&g_fat_fs, iv->directory, &dir) != 0) return;
    if (!dir.is_dir) {
        fat_close(&dir);
        return;
    }

    // Read directory entries
    fat_dir_entry_t entry;
    char name[256];

    while (fat_readdir(&dir, &entry, name) == 0 && iv->file_count < IV_MAX_FILES) {
        // Skip directories and hidden files
        if (entry.attr & (FAT_ATTR_DIRECTORY | FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM)) continue;

        // Check if it's an image file
        if (iv_is_image_file(name)) {
            strncpy(iv->files[iv->file_count], name, 63);
            iv->files[iv->file_count][63] = '\0';
            iv->file_count++;
        }
    }

    fat_close(&dir);

    // Find current file index
    if (strlen(iv->filepath) > 0) {
        // Extract filename from path
        const char *filename = iv->filepath;
        for (int i = strlen(iv->filepath) - 1; i >= 0; i--) {
            if (iv->filepath[i] == '/') {
                filename = &iv->filepath[i + 1];
                break;
            }
        }

        for (int i = 0; i < iv->file_count; i++) {
            if (strcmp(iv->files[i], filename) == 0) {
                iv->current_index = i;
                break;
            }
        }
    }
}

// Open image file
bool imageviewer_open(imageviewer_t *iv, const char *path) {
    if (!iv || !path) return false;

    kprintf("[ImageViewer] Opening: %s\n", path);

    // Free existing image
    if (iv->image) {
        image_free(iv->image);
        kfree(iv->image);
        iv->image = NULL;
    }

    // Read file from disk
    fat_file_t file;
    if (fat_open(&g_fat_fs, path, &file) != 0) {
        kprintf("[ImageViewer] Failed to open file: %s\n", path);
        iv->has_image = false;
        return false;
    }

    // Allocate buffer for file data
    uint32_t file_size = file.file_size;
    uint8_t *file_data = (uint8_t *)kmalloc(file_size);
    if (!file_data) {
        fat_close(&file);
        kprintf("[ImageViewer] Failed to allocate memory for file\n");
        iv->has_image = false;
        return false;
    }

    // Read file
    size_t bytes_read = fat_read(&file, file_data, file_size);
    fat_close(&file);

    if (bytes_read != file_size) {
        kfree(file_data);
        kprintf("[ImageViewer] Failed to read file\n");
        iv->has_image = false;
        return false;
    }

    // Allocate image structure
    iv->image = (image_t *)kmalloc(sizeof(image_t));
    if (!iv->image) {
        kfree(file_data);
        kprintf("[ImageViewer] Failed to allocate image structure\n");
        iv->has_image = false;
        return false;
    }

    // Load BMP from memory
    int err = image_load(file_data, file_size, iv->image);
    kfree(file_data);

    if (err != IMAGE_SUCCESS) {
        kfree(iv->image);
        iv->image = NULL;
        kprintf("[ImageViewer] Failed to decode image: %s\n", image_error_string(err));
        iv->has_image = false;
        return false;
    }

    // Store path
    strncpy(iv->filepath, path, IV_MAX_PATH - 1);
    iv->filepath[IV_MAX_PATH - 1] = '\0';

    // Extract directory
    strcpy(iv->directory, path);
    for (int i = strlen(iv->directory) - 1; i >= 0; i--) {
        if (iv->directory[i] == '/') {
            iv->directory[i] = '\0';
            break;
        }
    }
    if (strlen(iv->directory) == 0) {
        strcpy(iv->directory, "/");
    }

    // Load directory listing
    iv_load_directory(iv);

    // Update window title - build manually
    const char *filename = path;
    for (int i = strlen(path) - 1; i >= 0; i--) {
        if (path[i] == '/') {
            filename = &path[i + 1];
            break;
        }
    }

    char title[128];
    char *tp = title;
    const char *prefix = "Image Viewer - ";
    while (*prefix) *tp++ = *prefix++;
    while (*filename && (tp - title) < 120) *tp++ = *filename++;
    *tp = '\0';
    window_set_title(iv->window, title);

    // Reset view
    iv->mode = IV_MODE_FIT;
    iv->pan_x = 0;
    iv->pan_y = 0;
    iv->has_image = true;

    kprintf("[ImageViewer] Loaded %dx%d image\n", iv->image->width, iv->image->height);
    return true;
}

// Zoom operations
void imageviewer_zoom_in(imageviewer_t *iv) {
    if (!iv) return;
    iv->mode = IV_MODE_ZOOM;
    iv->zoom += IV_ZOOM_STEP;
    if (iv->zoom > IV_ZOOM_MAX) iv->zoom = IV_ZOOM_MAX;
}

void imageviewer_zoom_out(imageviewer_t *iv) {
    if (!iv) return;
    iv->mode = IV_MODE_ZOOM;
    iv->zoom -= IV_ZOOM_STEP;
    if (iv->zoom < IV_ZOOM_MIN) iv->zoom = IV_ZOOM_MIN;
}

void imageviewer_zoom_actual(imageviewer_t *iv) {
    if (!iv) return;
    iv->mode = IV_MODE_ACTUAL;
    iv->zoom = 100;
    iv->pan_x = 0;
    iv->pan_y = 0;
}

void imageviewer_zoom_fit(imageviewer_t *iv) {
    if (!iv) return;
    iv->mode = IV_MODE_FIT;
    iv->pan_x = 0;
    iv->pan_y = 0;
}

void imageviewer_zoom_fill(imageviewer_t *iv) {
    if (!iv) return;
    iv->mode = IV_MODE_FILL;
    iv->pan_x = 0;
    iv->pan_y = 0;
}

void imageviewer_set_zoom(imageviewer_t *iv, int percent) {
    if (!iv) return;
    iv->mode = IV_MODE_ZOOM;
    iv->zoom = percent;
    if (iv->zoom < IV_ZOOM_MIN) iv->zoom = IV_ZOOM_MIN;
    if (iv->zoom > IV_ZOOM_MAX) iv->zoom = IV_ZOOM_MAX;
}

// Navigation
void imageviewer_next(imageviewer_t *iv) {
    if (!iv || iv->file_count == 0) return;

    iv->current_index++;
    if (iv->current_index >= iv->file_count) {
        iv->current_index = 0;  // Wrap around
    }

    // Build full path
    char path[IV_MAX_PATH];
    iv_build_path(path, iv->directory, iv->files[iv->current_index]);
    imageviewer_open(iv, path);
}

void imageviewer_prev(imageviewer_t *iv) {
    if (!iv || iv->file_count == 0) return;

    iv->current_index--;
    if (iv->current_index < 0) {
        iv->current_index = iv->file_count - 1;  // Wrap around
    }

    // Build full path
    char path[IV_MAX_PATH];
    iv_build_path(path, iv->directory, iv->files[iv->current_index]);
    imageviewer_open(iv, path);
}

void imageviewer_first(imageviewer_t *iv) {
    if (!iv || iv->file_count == 0) return;

    iv->current_index = 0;
    char path[IV_MAX_PATH];
    iv_build_path(path, iv->directory, iv->files[iv->current_index]);
    imageviewer_open(iv, path);
}

void imageviewer_last(imageviewer_t *iv) {
    if (!iv || iv->file_count == 0) return;

    iv->current_index = iv->file_count - 1;
    char path[IV_MAX_PATH];
    iv_build_path(path, iv->directory, iv->files[iv->current_index]);
    imageviewer_open(iv, path);
}

// Toggle fullscreen
void imageviewer_toggle_fullscreen(imageviewer_t *iv) {
    if (!iv) return;

    if (iv->fullscreen) {
        // Restore window
        window_move(iv->window, iv->saved_bounds.x, iv->saved_bounds.y);
        window_resize(iv->window, iv->saved_bounds.width, iv->saved_bounds.height);
        iv->fullscreen = false;
    } else {
        // Save bounds and go fullscreen
        iv->saved_bounds = iv->window->bounds;
        window_move(iv->window, 0, 0);
        window_resize(iv->window, fb_get_width(), fb_get_height());
        iv->fullscreen = true;
    }
}

// Toggle info display
void imageviewer_toggle_info(imageviewer_t *iv) {
    if (iv) iv->show_info = !iv->show_info;
}

// Draw toolbar
static void iv_draw_toolbar(imageviewer_t *iv, int32_t wx, int32_t wy, int32_t ww) {
    // Toolbar background
    fb_fill_rect(wx, wy, ww, IV_TOOLBAR_HEIGHT, 0x404040);
    fb_fill_rect(wx, wy + IV_TOOLBAR_HEIGHT - 1, ww, 1, 0x303030);

    int x = wx + IV_BUTTON_PADDING;
    int y = wy + 2;

    // Open button
    icon_draw_scaled(ICON_FOLDER, x, y, 20, 0xFFFFFF);
    x += IV_BUTTON_SIZE + IV_BUTTON_PADDING;

    // Separator
    fb_fill_rect(x, wy + 4, 1, IV_TOOLBAR_HEIGHT - 8, 0x606060);
    x += IV_BUTTON_PADDING * 2;

    // Zoom buttons
    icon_draw_scaled(ICON_IMAGE, x, y, 20, 0xFFFFFF);  // Zoom out (placeholder)
    x += IV_BUTTON_SIZE + IV_BUTTON_PADDING;
    icon_draw_scaled(ICON_IMAGE, x, y, 20, 0xFFFFFF);  // Zoom in
    x += IV_BUTTON_SIZE + IV_BUTTON_PADDING;

    // Separator
    fb_fill_rect(x, wy + 4, 1, IV_TOOLBAR_HEIGHT - 8, 0x606060);
    x += IV_BUTTON_PADDING * 2;

    // Navigation buttons
    icon_draw_scaled(ICON_PREV, x, y, 20, 0xFFFFFF);
    x += IV_BUTTON_SIZE + IV_BUTTON_PADDING;
    icon_draw_scaled(ICON_NEXT, x, y, 20, 0xFFFFFF);
    x += IV_BUTTON_SIZE + IV_BUTTON_PADDING;

    // File counter on right side
    if (iv->file_count > 0) {
        char counter[32];
        // Build "N / M" manually
        char *cp = counter;
        cp = iv_itoa(iv->current_index + 1, cp);
        *cp++ = ' '; *cp++ = '/'; *cp++ = ' ';
        cp = iv_itoa(iv->file_count, cp);
        *cp = '\0';
        int text_x = wx + ww - strlen(counter) * 8 - 8;
        int text_y = wy + (IV_TOOLBAR_HEIGHT - 16) / 2;
        for (int i = 0; counter[i]; i++) {
            const uint8_t *glyph = font_get_glyph(counter[i]);
            for (int row = 0; row < 16; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(text_x + i * 8 + col, text_y + row, 0xFFFFFF);
                    }
                }
            }
        }
    }
}

// Draw image
static void iv_draw_image(imageviewer_t *iv, int32_t wx, int32_t wy, int32_t ww, int32_t wh) {
    if (!iv->has_image || !iv->image) {
        // Draw placeholder
        fb_fill_rect(wx, wy, ww, wh, 0x202020);

        const char *msg = "No image loaded";
        int text_x = wx + (ww - strlen(msg) * 8) / 2;
        int text_y = wy + (wh - 16) / 2;
        for (int i = 0; msg[i]; i++) {
            const uint8_t *glyph = font_get_glyph(msg[i]);
            for (int row = 0; row < 16; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(text_x + i * 8 + col, text_y + row, 0x808080);
                    }
                }
            }
        }
        return;
    }

    // Calculate display dimensions using integer math (no floats)
    int img_w = iv->image->width;
    int img_h = iv->image->height;
    int disp_w = img_w;  // Initialize to actual size
    int disp_h = img_h;
    int disp_x, disp_y;

    switch (iv->mode) {
        case IV_MODE_ACTUAL:
            disp_w = img_w;
            disp_h = img_h;
            break;

        case IV_MODE_FIT: {
            // Fit to window, maintain aspect ratio (integer math)
            // Calculate scale * 1000 to avoid floats
            int scale_w_1000 = (ww * 1000) / img_w;
            int scale_h_1000 = (wh * 1000) / img_h;
            int scale_1000 = (scale_w_1000 < scale_h_1000) ? scale_w_1000 : scale_h_1000;
            disp_w = (img_w * scale_1000) / 1000;
            disp_h = (img_h * scale_1000) / 1000;
            break;
        }

        case IV_MODE_FILL: {
            // Fill window, crop if needed (integer math)
            int scale_w_1000 = (ww * 1000) / img_w;
            int scale_h_1000 = (wh * 1000) / img_h;
            int scale_1000 = (scale_w_1000 > scale_h_1000) ? scale_w_1000 : scale_h_1000;
            disp_w = (img_w * scale_1000) / 1000;
            disp_h = (img_h * scale_1000) / 1000;
            break;
        }

        case IV_MODE_ZOOM:
        default:
            disp_w = (img_w * iv->zoom) / 100;
            disp_h = (img_h * iv->zoom) / 100;
            break;
    }

    // Center the image
    disp_x = wx + (ww - disp_w) / 2 + iv->pan_x;
    disp_y = wy + (wh - disp_h) / 2 + iv->pan_y;

    // Fill background
    fb_fill_rect(wx, wy, ww, wh, 0x202020);

    // Draw image scaled
    image_blit_scaled(iv->image, disp_x, disp_y, disp_w, disp_h);
}

// Draw info overlay
static void iv_draw_info(imageviewer_t *iv, int32_t wx, int32_t wy, int32_t ww, int32_t wh) {
    (void)ww; (void)wh;
    if (!iv->show_info || !iv->has_image) return;

    // Info box background
    int info_w = 200;
    int info_h = 80;
    int info_x = wx + 10;
    int info_y = wy + 10;

    fb_fill_rect(info_x, info_y, info_w, info_h, 0x000000 | 0x80000000);  // Semi-transparent

    // Draw info text
    char line[64];
    int y = info_y + 8;

    // Build "Size: WxH" string manually
    char *lp = line;
    const char *s = "Size: ";
    while (*s) *lp++ = *s++;
    lp = iv_itoa(iv->image->width, lp);
    *lp++ = 'x';
    lp = iv_itoa(iv->image->height, lp);
    *lp = '\0';

    for (int i = 0; line[i]; i++) {
        const uint8_t *glyph = font_get_glyph(line[i]);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    fb_put_pixel(info_x + 8 + i * 8 + col, y + row, 0xFFFFFF);
                }
            }
        }
    }
    y += 20;

    // Build "Zoom: N%" string manually
    lp = line;
    s = "Zoom: ";
    while (*s) *lp++ = *s++;
    lp = iv_itoa(iv->zoom, lp);
    *lp++ = '%';
    *lp = '\0';

    for (int i = 0; line[i]; i++) {
        const uint8_t *glyph = font_get_glyph(line[i]);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    fb_put_pixel(info_x + 8 + i * 8 + col, y + row, 0xFFFFFF);
                }
            }
        }
    }
}

// Event handling
void imageviewer_handle_event(imageviewer_t *iv, gui_event_t *event) {
    if (!iv || !event) return;

    switch (event->type) {
        case EVENT_KEY_DOWN:
            switch (event->keycode) {
                case '+':
                case '=':
                    imageviewer_zoom_in(iv);
                    break;
                case '-':
                case '_':
                    imageviewer_zoom_out(iv);
                    break;
                case '0':
                    imageviewer_zoom_actual(iv);
                    break;
                case 'f':
                case 'F':
                    imageviewer_zoom_fit(iv);
                    break;
                case 0x4D:  // Right arrow
                case ' ':
                    imageviewer_next(iv);
                    break;
                case 0x4B:  // Left arrow
                case 0x08:  // Backspace
                    imageviewer_prev(iv);
                    break;
                case 0x47:  // Home
                    imageviewer_first(iv);
                    break;
                case 'i':
                case 'I':
                    imageviewer_toggle_info(iv);
                    break;
                case 0x0D:  // Enter - toggle fullscreen
                    imageviewer_toggle_fullscreen(iv);
                    break;
                case 'o':
                    // Open file dialog (lowercase only to avoid conflict with End key)
                    {
                        char filepath[IV_MAX_PATH];
                        if (filedialog_open("Open Image", "/", "*.bmp", "Image Files", filepath)) {
                            imageviewer_open(iv, filepath);
                        }
                    }
                    break;
            }
            break;

        case EVENT_MOUSE_DOWN:
            {
                // Get window content bounds
                int32_t wx, wy, ww, wh;
                window_get_content_bounds(iv->window, &wx, &wy, &ww, &wh);
                (void)wh;  // Unused

                int mx = event->mouse_x;
                int my = event->mouse_y;

                // Check if click is in toolbar area
                if (my >= wy && my < wy + IV_TOOLBAR_HEIGHT) {
                    int btn_x = wx + IV_BUTTON_PADDING;
                    int btn_y = wy + 2;

                    // Open button (first button)
                    if (mx >= btn_x && mx < btn_x + IV_BUTTON_SIZE &&
                        my >= btn_y && my < btn_y + IV_BUTTON_SIZE) {
                        // Open file dialog
                        char filepath[IV_MAX_PATH];
                        if (filedialog_open("Open Image", "/", "*.bmp", "Image Files", filepath)) {
                            imageviewer_open(iv, filepath);
                        }
                        break;
                    }
                    btn_x += IV_BUTTON_SIZE + IV_BUTTON_PADDING;  // Past open button
                    btn_x += IV_BUTTON_PADDING * 2;  // Past separator

                    // Zoom out button
                    if (mx >= btn_x && mx < btn_x + IV_BUTTON_SIZE &&
                        my >= btn_y && my < btn_y + IV_BUTTON_SIZE) {
                        imageviewer_zoom_out(iv);
                        break;
                    }
                    btn_x += IV_BUTTON_SIZE + IV_BUTTON_PADDING;

                    // Zoom in button
                    if (mx >= btn_x && mx < btn_x + IV_BUTTON_SIZE &&
                        my >= btn_y && my < btn_y + IV_BUTTON_SIZE) {
                        imageviewer_zoom_in(iv);
                        break;
                    }
                    btn_x += IV_BUTTON_SIZE + IV_BUTTON_PADDING;
                    btn_x += IV_BUTTON_PADDING * 2;  // Past separator

                    // Previous button
                    if (mx >= btn_x && mx < btn_x + IV_BUTTON_SIZE &&
                        my >= btn_y && my < btn_y + IV_BUTTON_SIZE) {
                        imageviewer_prev(iv);
                        break;
                    }
                    btn_x += IV_BUTTON_SIZE + IV_BUTTON_PADDING;

                    // Next button
                    if (mx >= btn_x && mx < btn_x + IV_BUTTON_SIZE &&
                        my >= btn_y && my < btn_y + IV_BUTTON_SIZE) {
                        imageviewer_next(iv);
                        break;
                    }
                } else {
                    // Click is in image area - start panning
                    iv->dragging = true;
                    iv->drag_start_x = event->mouse_x;
                    iv->drag_start_y = event->mouse_y;
                    iv->drag_pan_x = iv->pan_x;
                    iv->drag_pan_y = iv->pan_y;
                }
            }
            break;

        case EVENT_MOUSE_UP:
            iv->dragging = false;
            break;

        case EVENT_MOUSE_MOVE:
            // Handle mouse for panning
            if (iv->dragging) {
                iv->pan_x = iv->drag_pan_x + (event->mouse_x - iv->drag_start_x);
                iv->pan_y = iv->drag_pan_y + (event->mouse_y - iv->drag_start_y);
            }
            break;

        default:
            break;
    }
}

// Drawing
void imageviewer_draw(imageviewer_t *iv) {
    if (!iv || !iv->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(iv->window, &wx, &wy, &ww, &wh);

    // Draw toolbar
    iv_draw_toolbar(iv, wx, wy, ww);

    // Draw image area (below toolbar)
    int img_y = wy + IV_TOOLBAR_HEIGHT;
    int img_h = wh - IV_TOOLBAR_HEIGHT;
    iv_draw_image(iv, wx, img_y, ww, img_h);

    // Draw info overlay
    iv_draw_info(iv, wx, img_y, ww, img_h);
}

// Launch image viewer
void imageviewer_launch(const char *filepath) {
    LOG_INFO("[ImageViewer] Application launched");
    imageviewer_t *iv = imageviewer_create();
    if (!iv) {
        LOG_ERROR("[ImageViewer] Failed to create viewer");
        kprintf("[ImageViewer] Failed to create viewer\n");
        return;
    }

    if (filepath && strlen(filepath) > 0) {
        imageviewer_open(iv, filepath);
    }

    // Register with window manager
    wm_register_app(iv->window, iv,
                    (app_event_handler_t)imageviewer_handle_event,
                    (app_draw_handler_t)imageviewer_draw,
                    (app_destroy_handler_t)imageviewer_destroy);

    kprintf("[ImageViewer] Launched\n");
}
