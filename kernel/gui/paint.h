// paint.h - Paint Application for MayteraOS (MS Paint Win95 equivalent)
#ifndef PAINT_H
#define PAINT_H

#include "window.h"
#include "image.h"

// Canvas limits
#define PAINT_MAX_WIDTH     4096
#define PAINT_MAX_HEIGHT    4096
#define PAINT_DEFAULT_WIDTH  640
#define PAINT_DEFAULT_HEIGHT 480

// Undo/redo stack size
#define PAINT_UNDO_LEVELS   20

// Brush sizes
#define PAINT_BRUSH_MIN     1
#define PAINT_BRUSH_MAX     50

// Tool types
typedef enum {
    TOOL_PENCIL = 0,    // Freehand draw (1px)
    TOOL_BRUSH,         // Freehand draw (variable size)
    TOOL_ERASER,        // Erase to background color
    TOOL_FILL,          // Flood fill
    TOOL_PICKER,        // Color picker (eyedropper)
    TOOL_LINE,          // Straight line
    TOOL_RECT,          // Rectangle (outline)
    TOOL_RECT_FILL,     // Rectangle (filled)
    TOOL_ELLIPSE,       // Ellipse (outline)
    TOOL_ELLIPSE_FILL,  // Ellipse (filled)
    TOOL_POLYGON,       // Polygon (click to add points)
    TOOL_TEXT,          // Text insertion
    TOOL_SELECT_RECT,   // Rectangular selection
    TOOL_SELECT_FREE,   // Freeform selection
    TOOL_SPRAY,         // Spray can/airbrush
    TOOL_MAGNIFIER,     // Zoom tool
    TOOL_COUNT
} paint_tool_t;

// Selection state
typedef struct {
    bool active;            // Is there an active selection?
    int x, y;               // Selection top-left
    int width, height;      // Selection dimensions
    uint32_t *buffer;       // Selection pixel buffer (for cut/copy)
    int buffer_w, buffer_h; // Buffer dimensions
    bool floating;          // Is selection floating (after paste)?
} paint_selection_t;

// Undo state
typedef struct {
    uint32_t *pixels;       // Pixel data snapshot
    int width, height;      // Canvas dimensions at time of snapshot
} paint_undo_state_t;

// Paint application state
typedef struct {
    window_t *window;           // Application window

    // Canvas
    uint32_t *canvas;           // Canvas pixel data (BGRA)
    int canvas_width;           // Canvas width
    int canvas_height;          // Canvas height
    bool modified;              // Has canvas been modified?
    char filepath[256];         // Current file path

    // View
    int zoom;                   // Zoom level (100 = 100%)
    int scroll_x, scroll_y;     // Scroll offset

    // Current tool
    paint_tool_t tool;          // Active tool
    int brush_size;             // Brush/eraser size

    // Colors
    uint32_t fg_color;          // Foreground (primary) color
    uint32_t bg_color;          // Background (secondary) color
    uint32_t palette[28];       // Color palette

    // Selection
    paint_selection_t selection;

    // Drawing state
    bool drawing;               // Currently drawing?
    int last_x, last_y;         // Last mouse position (for line drawing)
    int start_x, start_y;       // Start position (for shapes)

    // Polygon state
    int poly_points[64][2];     // Polygon vertices
    int poly_count;             // Number of vertices

    // Text state
    char text_buffer[256];      // Text being entered
    int text_cursor;            // Text cursor position
    int text_x, text_y;         // Text insertion point

    // Undo/redo
    paint_undo_state_t undo_stack[PAINT_UNDO_LEVELS];
    int undo_index;             // Current undo position
    int undo_count;             // Number of undo states

    // UI dimensions
    int toolbar_width;          // Left toolbar width
    int palette_height;         // Bottom palette height
    int status_height;          // Status bar height

} paint_t;

// Create paint application
paint_t *paint_create(void);

// Destroy paint application
void paint_destroy(paint_t *p);

// Canvas operations
bool paint_new(paint_t *p, int width, int height);
bool paint_open(paint_t *p, const char *path);
bool paint_save(paint_t *p, const char *path);
bool paint_save_as(paint_t *p);
void paint_resize_canvas(paint_t *p, int width, int height);

// Tool selection
void paint_set_tool(paint_t *p, paint_tool_t tool);
void paint_set_brush_size(paint_t *p, int size);

// Color operations
void paint_set_fg_color(paint_t *p, uint32_t color);
void paint_set_bg_color(paint_t *p, uint32_t color);
void paint_swap_colors(paint_t *p);
uint32_t paint_pick_color(paint_t *p, int x, int y);

// Selection operations
void paint_select_all(paint_t *p);
void paint_select_none(paint_t *p);
void paint_cut(paint_t *p);
void paint_copy(paint_t *p);
void paint_paste(paint_t *p);
void paint_delete_selection(paint_t *p);

// Transform operations
void paint_flip_horizontal(paint_t *p);
void paint_flip_vertical(paint_t *p);
void paint_rotate_90(paint_t *p, bool clockwise);
void paint_invert_colors(paint_t *p);
void paint_clear_canvas(paint_t *p);

// Undo/redo
void paint_undo(paint_t *p);
void paint_redo(paint_t *p);
void paint_save_undo_state(paint_t *p);

// View operations
void paint_zoom_in(paint_t *p);
void paint_zoom_out(paint_t *p);
void paint_zoom_actual(paint_t *p);

// Event handling
void paint_handle_event(paint_t *p, gui_event_t *event);

// Drawing
void paint_draw(paint_t *p);

// Launch paint application
void paint_launch(const char *filepath);

#endif // PAINT_H
