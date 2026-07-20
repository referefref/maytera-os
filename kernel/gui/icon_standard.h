// icon_standard.h - MayteraOS Icon Standard
// Defines icon sizes, formats, and system icon IDs
#ifndef ICON_STANDARD_H
#define ICON_STANDARD_H

#include "../types.h"

// ============================================================================
// MayteraOS Icon Standard v1.0
// ============================================================================
//
// Icon Sizes:
//   ICON_SIZE_SMALL  (16x16)  - Menu items, list views, small toolbar
//   ICON_SIZE_MEDIUM (32x32)  - Toolbar buttons, dock, file browser
//   ICON_SIZE_LARGE  (48x48)  - Desktop icons, large thumbnail
//
// Icon Format:
//   - 32-bit BGRA (with alpha channel for transparency)
//   - Can be embedded as C arrays or loaded from BMP files
//   - BMP files should be 32-bit with alpha or 24-bit with magenta (FF00FF) transparency
//
// Naming Convention:
//   - System icons: icon_<category>_<name>_<size>
//   - Example: icon_app_editor_32, icon_folder_home_16
//
// Storage:
//   - Embedded icons in gui/icons.c for core system icons
//   - File-based icons in /icons/ directory for user/app icons
//
// ============================================================================

// Icon size definitions
#define ICON_SIZE_SMALL     16
#define ICON_SIZE_MEDIUM    32
#define ICON_SIZE_LARGE     48

// Icon size enum for API calls
typedef enum {
    ICON_SMALL = 0,     // 16x16
    ICON_MEDIUM = 1,    // 32x32
    ICON_LARGE = 2      // 48x48
} icon_size_t;

// System icon categories
typedef enum {
    ICON_CAT_APP = 0,       // Application icons
    ICON_CAT_FILE,          // File type icons
    ICON_CAT_FOLDER,        // Folder icons
    ICON_CAT_ACTION,        // Action/toolbar icons
    ICON_CAT_STATUS,        // Status indicators
    ICON_CAT_DEVICE,        // Device icons
    ICON_CAT_GAME           // Game icons
} icon_category_t;

// System icon IDs - Applications
#define ICON_APP_GENERIC        0x0100
#define ICON_APP_TERMINAL       0x0101
#define ICON_APP_EDITOR         0x0102
#define ICON_APP_CALCULATOR     0x0103
#define ICON_APP_FILES          0x0104
#define ICON_APP_SETTINGS       0x0105
#define ICON_APP_IMAGEVIEWER    0x0106
#define ICON_APP_PAINT          0x0107
#define ICON_APP_AUDIOPLAYER    0x0108
#define ICON_APP_CLOCK          0x0109

// System icon IDs - Files
#define ICON_FILE_GENERIC       0x0200
#define ICON_FILE_TEXT          0x0201
#define ICON_FILE_IMAGE         0x0202
#define ICON_FILE_AUDIO         0x0203
#define ICON_FILE_VIDEO         0x0204
#define ICON_FILE_ARCHIVE       0x0205
#define ICON_FILE_EXECUTABLE    0x0206

// System icon IDs - Folders
#define ICON_FOLDER_GENERIC     0x0300
#define ICON_FOLDER_HOME        0x0301
#define ICON_FOLDER_DOCUMENTS   0x0302
#define ICON_FOLDER_PICTURES    0x0303
#define ICON_FOLDER_MUSIC       0x0304
#define ICON_FOLDER_DOWNLOADS   0x0305
#define ICON_FOLDER_OPEN        0x0306

// System icon IDs - Actions
#define ICON_ACTION_NEW         0x0400
#define ICON_ACTION_OPEN        0x0401
#define ICON_ACTION_SAVE        0x0402
#define ICON_ACTION_CLOSE       0x0403
#define ICON_ACTION_CUT         0x0404
#define ICON_ACTION_COPY        0x0405
#define ICON_ACTION_PASTE       0x0406
#define ICON_ACTION_UNDO        0x0407
#define ICON_ACTION_REDO        0x0408
#define ICON_ACTION_DELETE      0x0409
#define ICON_ACTION_REFRESH     0x040A
#define ICON_ACTION_SEARCH      0x040B
#define ICON_ACTION_ZOOM_IN     0x040C
#define ICON_ACTION_ZOOM_OUT    0x040D
#define ICON_ACTION_PLAY        0x040E
#define ICON_ACTION_PAUSE       0x040F
#define ICON_ACTION_STOP        0x0410
#define ICON_ACTION_NEXT        0x0411
#define ICON_ACTION_PREV        0x0412

// System icon IDs - Games
#define ICON_GAME_GENERIC       0x0500
#define ICON_GAME_DOOM          0x0501
#define ICON_GAME_PONG          0x0502
#define ICON_GAME_SOLITAIRE     0x0503
#define ICON_GAME_LEMMINGS      0x0504

// System icon IDs - Devices
#define ICON_DEVICE_COMPUTER    0x0600
#define ICON_DEVICE_HARDDISK    0x0601
#define ICON_DEVICE_CDROM       0x0602
#define ICON_DEVICE_USB         0x0603
#define ICON_DEVICE_NETWORK     0x0604

// Icon structure
typedef struct {
    uint16_t id;            // Icon ID (from defines above)
    uint8_t size;           // ICON_SIZE_SMALL/MEDIUM/LARGE
    uint8_t reserved;
    uint32_t width;         // Actual width in pixels
    uint32_t height;        // Actual height in pixels
    const uint32_t *data;   // BGRA pixel data (with alpha)
} icon_t;

// Icon registry entry
typedef struct {
    uint16_t id;
    const uint32_t *data_16;    // 16x16 version
    const uint32_t *data_32;    // 32x32 version
    const uint32_t *data_48;    // 48x48 version
} icon_entry_t;

// ============================================================================
// Icon API
// ============================================================================

// Initialize icon system
void icons_init(void);

// Get icon by ID and size
const uint32_t *icon_get(uint16_t icon_id, icon_size_t size);

// Get icon dimensions for a size
int icon_get_dimension(icon_size_t size);

// Draw icon at position
void icon_draw(int32_t x, int32_t y, uint16_t icon_id, icon_size_t size);

// Draw icon with transparency (alpha blending)
void icon_draw_alpha(int32_t x, int32_t y, uint16_t icon_id, icon_size_t size, uint8_t alpha);

// Load icon from BMP file
bool icon_load_from_file(const char *path, uint16_t icon_id, icon_size_t size);

// Register custom icon
void icon_register(uint16_t icon_id, icon_size_t size, const uint32_t *data);

#endif // ICON_STANDARD_H
