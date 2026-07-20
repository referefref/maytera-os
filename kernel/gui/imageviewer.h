// imageviewer.h - Image Viewer Application for MayteraOS
#ifndef IMAGEVIEWER_H
#define IMAGEVIEWER_H

#include "window.h"
#include "image.h"

// Maximum path length
#define IV_MAX_PATH 256
#define IV_MAX_FILES 256

// Zoom levels (percentage)
#define IV_ZOOM_MIN     10
#define IV_ZOOM_MAX     800
#define IV_ZOOM_DEFAULT 100
#define IV_ZOOM_STEP    25

// View modes
typedef enum {
    IV_MODE_ACTUAL,     // 100% actual size
    IV_MODE_FIT,        // Fit to window
    IV_MODE_FILL,       // Fill window (crop if needed)
    IV_MODE_ZOOM        // Custom zoom level
} iv_view_mode_t;

// Image viewer state
typedef struct {
    window_t *window;           // Application window

    // Current image
    image_t *image;             // Loaded image data
    char filepath[IV_MAX_PATH]; // Current file path
    bool has_image;             // Is an image loaded?

    // View settings
    iv_view_mode_t mode;        // Current view mode
    int zoom;                   // Zoom percentage (10-800)
    int pan_x, pan_y;           // Pan offset (when zoomed)

    // Directory navigation
    char directory[IV_MAX_PATH];        // Current directory
    char files[IV_MAX_FILES][64];       // Image files in directory
    int file_count;                     // Number of image files
    int current_index;                  // Current file index

    // UI state
    bool show_info;             // Show image info overlay
    bool fullscreen;            // Fullscreen mode
    rect_t saved_bounds;        // Saved window bounds for fullscreen toggle

    // Drag state for panning
    bool dragging;
    int drag_start_x, drag_start_y;
    int drag_pan_x, drag_pan_y;

} imageviewer_t;

// Create image viewer application
imageviewer_t *imageviewer_create(void);

// Destroy image viewer
void imageviewer_destroy(imageviewer_t *iv);

// Open image file
bool imageviewer_open(imageviewer_t *iv, const char *path);

// View operations
void imageviewer_zoom_in(imageviewer_t *iv);
void imageviewer_zoom_out(imageviewer_t *iv);
void imageviewer_zoom_actual(imageviewer_t *iv);
void imageviewer_zoom_fit(imageviewer_t *iv);
void imageviewer_zoom_fill(imageviewer_t *iv);
void imageviewer_set_zoom(imageviewer_t *iv, int percent);

// Navigation
void imageviewer_next(imageviewer_t *iv);
void imageviewer_prev(imageviewer_t *iv);
void imageviewer_first(imageviewer_t *iv);
void imageviewer_last(imageviewer_t *iv);

// Fullscreen toggle
void imageviewer_toggle_fullscreen(imageviewer_t *iv);

// Toggle info display
void imageviewer_toggle_info(imageviewer_t *iv);

// Event handling
void imageviewer_handle_event(imageviewer_t *iv, gui_event_t *event);

// Drawing
void imageviewer_draw(imageviewer_t *iv);

// Launch image viewer with optional file
void imageviewer_launch(const char *filepath);

#endif // IMAGEVIEWER_H
