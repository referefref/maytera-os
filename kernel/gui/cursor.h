// cursor.h - Cursor system for MayteraOS
// Supports multiple cursor types and themes (retro/modern)
#ifndef CURSOR_H
#define CURSOR_H

#include "../types.h"

// ============================================================================
// Cursor Types
// ============================================================================

typedef enum {
    CURSOR_DEFAULT = 0,     // Standard arrow pointer
    CURSOR_TEXT,            // I-beam for text selection
    CURSOR_HAND,            // Hand/pointer for clickable items
    CURSOR_WAIT,            // Hourglass/spinner for loading
    CURSOR_RESIZE_NS,       // North-South resize
    CURSOR_RESIZE_EW,       // East-West resize
    CURSOR_RESIZE_NESW,     // Northeast-Southwest resize (diagonal)
    CURSOR_RESIZE_NWSE,     // Northwest-Southeast resize (diagonal)
    CURSOR_MOVE,            // Four-way arrow for moving
    CURSOR_CROSSHAIR,       // Precision selection
    CURSOR_NOT_ALLOWED,     // Disabled/forbidden (circle with slash)
    CURSOR_HELP,            // Help cursor (arrow with question mark)
    CURSOR_PROGRESS,        // Arrow with small spinner
    CURSOR_COUNT            // Total number of cursor types
} cursor_type_t;

// ============================================================================
// Cursor Theme Types
// ============================================================================

typedef enum {
    CURSOR_THEME_RETRO = 0,     // Classic 1-bit B&W, sharp pixels
    CURSOR_THEME_MODERN_LIGHT,  // Anti-aliased, light theme
    CURSOR_THEME_MODERN_DARK,   // Anti-aliased, dark theme
    CURSOR_THEME_COUNT
} cursor_theme_t;

// ============================================================================
// Cursor Data Structures
// ============================================================================

// Maximum cursor size
#define CURSOR_MAX_WIDTH    32
#define CURSOR_MAX_HEIGHT   32

// Cursor image data (1-bit with mask)
typedef struct {
    uint8_t width;              // Cursor width in pixels (max 32)
    uint8_t height;             // Cursor height in pixels (max 32)
    int8_t hotspot_x;           // Hotspot X offset from top-left
    int8_t hotspot_y;           // Hotspot Y offset from top-left
    const uint8_t *image;       // Cursor image bitmap (1 = foreground)
    const uint8_t *mask;        // Cursor mask bitmap (1 = visible, 0 = transparent)
} cursor_bitmap_t;

// Animated cursor frame
typedef struct {
    const cursor_bitmap_t *frame;   // Frame image
    uint16_t duration_ms;           // Duration of this frame in ms
} cursor_anim_frame_t;

// Cursor definition (static or animated)
typedef struct {
    cursor_type_t type;             // Cursor type identifier
    bool animated;                  // Is this an animated cursor?
    union {
        const cursor_bitmap_t *static_image;   // For non-animated cursors
        struct {
            const cursor_anim_frame_t *frames; // Frame array
            uint8_t frame_count;               // Number of frames
        } animation;
    } data;
} cursor_def_t;

// Cursor state (runtime)
typedef struct {
    cursor_type_t current_type;     // Current cursor type
    cursor_theme_t current_theme;   // Current theme
    int32_t x;                      // Current X position
    int32_t y;                      // Current Y position
    bool visible;                   // Is cursor visible?

    // Animation state
    uint8_t anim_frame;             // Current animation frame
    uint32_t anim_timer;            // Time in current frame (ms)

    // Saved background for cursor restoration
    uint32_t saved_bg[CURSOR_MAX_WIDTH * CURSOR_MAX_HEIGHT];
    int32_t saved_x;
    int32_t saved_y;
    int32_t saved_width;
    int32_t saved_height;
    bool bg_saved;
} cursor_state_t;

// ============================================================================
// Cursor System API
// ============================================================================

/**
 * Initialize the cursor system
 * Must be called before any other cursor functions
 */
void cursor_init(void);

/**
 * Set the current cursor theme
 * @param theme     Theme to use
 */
void cursor_set_theme(cursor_theme_t theme);

/**
 * Get the current cursor theme
 * @return          Current theme
 */
cursor_theme_t cursor_get_theme(void);

/**
 * Set the current cursor type
 * @param type      Cursor type to display
 */
void cursor_set_type(cursor_type_t type);

/**
 * Get the current cursor type
 * @return          Current cursor type
 */
cursor_type_t cursor_get_type(void);

/**
 * Set cursor position
 * @param x         Screen X coordinate
 * @param y         Screen Y coordinate
 */
void cursor_set_position(int32_t x, int32_t y);

/**
 * Get cursor position
 * @param x         Output X coordinate
 * @param y         Output Y coordinate
 */
void cursor_get_position(int32_t *x, int32_t *y);

/**
 * Show the cursor
 */
void cursor_show(void);

/**
 * Hide the cursor
 */
void cursor_hide(void);

/**
 * Check if cursor is visible
 * @return          true if visible
 */
bool cursor_is_visible(void);

/**
 * Draw the cursor at its current position
 * Called each frame to render the cursor
 */
void cursor_draw(void);

/**
 * Restore the background under the cursor
 * Call before redrawing the area under the cursor
 */
void cursor_restore_background(void);

/**
 * Update cursor animation state
 * @param delta_ms  Time elapsed since last update in milliseconds
 */
void cursor_update(uint32_t delta_ms);

/**
 * Get the cursor hotspot offset
 * @param type      Cursor type
 * @param x         Output hotspot X offset
 * @param y         Output hotspot Y offset
 */
void cursor_get_hotspot(cursor_type_t type, int8_t *x, int8_t *y);

/**
 * Get cursor bounds for hit testing
 * Returns the bounding rectangle of the cursor image
 * @param x         Output X position (top-left)
 * @param y         Output Y position (top-left)
 * @param w         Output width
 * @param h         Output height
 */
void cursor_get_bounds(int32_t *x, int32_t *y, int32_t *w, int32_t *h);

// ============================================================================
// Theme-Specific Cursor Data Access
// ============================================================================

/**
 * Get cursor definition for a specific type and theme
 * @param type      Cursor type
 * @param theme     Theme
 * @return          Pointer to cursor definition, or NULL if not found
 */
const cursor_def_t *cursor_get_def(cursor_type_t type, cursor_theme_t theme);

/**
 * Register a custom cursor for a type/theme combination
 * @param type      Cursor type
 * @param theme     Theme
 * @param def       Cursor definition
 * @return          0 on success, -1 on failure
 */
int cursor_register(cursor_type_t type, cursor_theme_t theme, const cursor_def_t *def);

#endif // CURSOR_H
