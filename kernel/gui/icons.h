// icons.h - Boxicons-based icon system for MayteraOS
// Icons converted from SVG to bitmap for bare-metal rendering
#ifndef ICONS_H
#define ICONS_H

#include "../types.h"

// Icon size (all icons are 24x24)
#define ICON_SIZE       24
#define ICON_SMALL      16
#define ICON_LARGE      32

// Icon identifiers
typedef enum {
    // System icons (0-19)
    ICON_CATEGORIES = 0,    // Start menu / app grid
    ICON_TERMINAL,          // Terminal app
    ICON_HIGHLIGHT,         // Text editor
    ICON_FOLDER,            // File browser
    ICON_CALCULATOR,        // Calculator app
    ICON_COG,               // Settings
    ICON_INFO_CIRCLE,       // About/Info
    ICON_IMAGE,             // Image viewer
    ICON_MUSIC,             // Music player
    ICON_WINDOW,            // Generic window
    ICON_POWER,             // Shutdown
    ICON_REFRESH,           // Reboot/Refresh
    ICON_HOME,              // Home directory
    ICON_FILE,              // Generic file
    ICON_PALETTE,           // Wallpaper/Theme

    // New application icons (20-39)
    ICON_PAINT,             // Paint application
    ICON_CLOCK,             // Clock widget
    ICON_TASK_MANAGER,      // Task manager
    ICON_LOG_VIEWER,        // System log viewer
    ICON_TRASH,             // Recycle bin
    ICON_TRASH_FULL,        // Recycle bin with items

    // Game icons (40-59)
    ICON_GAME,              // Generic game
    ICON_GAME_DOOM,         // DOOM
    ICON_GAME_PONG,         // Pong
    ICON_GAME_SOLITAIRE,    // Solitaire/Cards
    ICON_GAME_LEMMINGS,     // Lemmings

    // Action icons (60-79)
    ICON_PLAY,              // Play button
    ICON_PAUSE,             // Pause button
    ICON_STOP,              // Stop button
    ICON_NEXT,              // Next track
    ICON_PREV,              // Previous track
    ICON_ZOOM_IN,           // Zoom in
    ICON_ZOOM_OUT,          // Zoom out
    ICON_SAVE,              // Save
    ICON_OPEN,              // Open
    ICON_NEW,               // New document
    ICON_CUT,               // Cut
    ICON_COPY,              // Copy
    ICON_PASTE,             // Paste
    ICON_UNDO,              // Undo
    ICON_REDO,              // Redo

    // Tool icons (80-99) - for paint app
    ICON_PENCIL,            // Pencil tool
    ICON_BRUSH,             // Brush tool
    ICON_ERASER,            // Eraser tool
    ICON_FILL,              // Fill bucket
    ICON_LINE,              // Line tool
    ICON_RECT,              // Rectangle tool
    ICON_ELLIPSE,           // Ellipse tool
    ICON_SELECT,            // Selection tool
    ICON_TEXT,              // Text tool
    ICON_EYEDROPPER,        // Color picker
    ICON_SPRAY,             // Spray can

    // Network icons (100+)
    ICON_NETWORK,           // Network
    ICON_DOWNLOAD,          // Download
    ICON_UPLOAD,            // Upload
    ICON_IRC,               // IRC/Chat

    // File type icons (120+)
    ICON_FILE_TEXT,         // Text document
    ICON_FILE_CODE,         // Source code file
    ICON_FILE_IMAGE,        // Image file
    ICON_FILE_AUDIO,        // Audio file
    ICON_FILE_VIDEO,        // Video file
    ICON_FILE_ARCHIVE,      // Archive/compressed file
    ICON_FILE_EXEC,         // Executable file
    ICON_FILE_CONFIG,       // Config file

    // Storage/Device icons (140+)
    ICON_USB_DRIVE,         // USB flash drive
    ICON_HARD_DRIVE,        // Hard drive
    ICON_EJECT,             // Eject/safely remove

    ICON_COUNT              // Total number of icons
} icon_id_t;

// Application icon aliases for registry (use existing icons)
#define ICON_APP_TERMINAL   ICON_TERMINAL
#define ICON_APP_FILES      ICON_FOLDER
#define ICON_APP_EDITOR     ICON_HIGHLIGHT
#define ICON_APP_CALCULATOR ICON_CALCULATOR
#define ICON_APP_PAINT      ICON_PAINT
#define ICON_APP_IMAGE      ICON_IMAGE
#define ICON_APP_AUDIO      ICON_MUSIC
#define ICON_APP_IRC        ICON_IRC

// System app icon aliases
#define ICON_SYS_SETTINGS   ICON_COG
#define ICON_SYS_TASKMAN    ICON_TASK_MANAGER
#define ICON_SYS_LOG        ICON_LOG_VIEWER
#define ICON_SYS_RECYCLE    ICON_TRASH
#define ICON_SYS_NETWORK    ICON_NETWORK

// Icon data structure
typedef struct {
    const uint8_t *data;    // Bitmap data (1 bit per pixel, row-major)
    uint8_t width;
    uint8_t height;
} icon_t;

/**
 * Get icon data by ID
 * @param id    Icon identifier
 * @return      Pointer to icon data, or NULL if invalid
 */
const icon_t *icon_get(icon_id_t id);

/**
 * Draw an icon at the specified position
 * @param id    Icon identifier
 * @param x     X position
 * @param y     Y position
 * @param color Icon color (0xRRGGBB)
 */
void icon_draw(icon_id_t id, int x, int y, uint32_t color);

/**
 * Draw an icon scaled to a specific size
 * @param id    Icon identifier
 * @param x     X position
 * @param y     Y position
 * @param size  Target size (width and height)
 * @param color Icon color (0xRRGGBB)
 */
void icon_draw_scaled(icon_id_t id, int x, int y, int size, uint32_t color);

#endif // ICONS_H
