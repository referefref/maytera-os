#pragma GCC diagnostic ignored "-Wunused-function"
// themes.c - Color theme system implementation for MayteraOS GUI
#include "themes.h"
#include "font.h"
#include "../serial.h"
#include "../string.h"

// Current theme ID
static int g_current_theme = THEME_DEFAULT;
static bool g_themes_initialized = false;

// Built-in themes
static theme_t g_themes[MAX_THEMES] = {
    // ========================================================================
    // THEME_DEFAULT (0) - Retro UNIX / CDE / Motif inspired theme
    // The signature MayteraOS look - nostalgic 1990s UNIX workstation aesthetic
    // Inspired by CDE (Common Desktop Environment), Motif, NeXTSTEP, SGI IRIX
    // Features: 3D beveled widgets, warm grays, teal accents, square corners
    // ========================================================================
    {
        .name = "Retro UNIX",
        .style = THEME_STYLE_RETRO,         // Bitmap fonts, 3D beveled UI

        // Window titlebar - CDE-style deep teal for active, gray for inactive
        // Active windows get distinctive teal gradient feel
        .titlebar_active = 0x00336666,      // Deep teal (CDE active titlebar)
        .titlebar_inactive = 0x00808080,    // Medium gray (stippled pattern feel)
        .titlebar_text = 0x00FFFFFF,        // White text on titlebar

        // Window body - Classic warm gray Motif background
        // The iconic #b4b4b4 warm gray that defined UNIX workstations
        .window_bg = 0x00B4B4B4,            // Warm gray (classic Motif #b4b4b4)
        .window_border = 0x00404040,        // Dark gray border for 3D depth

        // Window buttons - Motif-style beveled gray buttons (square, not round)
        // These should have 3D raised appearance with light/dark edges
        .close_button = 0x00C8C8C8,         // Light gray button face
        .close_button_hover = 0x00D8D8D8,   // Lighter on hover (raised effect)
        .minimize_button = 0x00C8C8C8,      // Light gray (Motif style)
        .maximize_button = 0x00C8C8C8,      // Light gray (Motif style)

        // Button widgets - Classic Motif 3D beveled style
        // IMPORTANT: Render with light top-left edge (#FFFFFF), dark bottom-right (#404040)
        .button_bg = 0x00C8C8C8,            // Button face (#c8c8c8)
        .button_bg_hover = 0x00D4D4D4,      // Slightly lighter on hover
        .button_bg_pressed = 0x00A8A8A8,    // Darker when pressed (sunken look)
        .button_border = 0x00404040,        // Dark edge (3D shadow side)
        .button_text = 0x00000000,          // Black text
        .button_disabled = 0x00808080,      // Gray disabled text

        // Text widgets - Sunken field style (3D inset appearance)
        // Input fields have dark top-left edge, light bottom-right (opposite of buttons)
        .label_text = 0x00000000,           // Black text on gray background
        .textbox_bg = 0x00FFFFFF,           // White input field interior
        .textbox_border = 0x00404040,       // Dark border (sunken look)
        .textbox_text = 0x00000000,         // Black text
        .textbox_cursor = 0x00000000,       // Black cursor

        // Checkbox - Motif style with sunken indicator box
        .checkbox_bg = 0x00FFFFFF,          // White checkbox interior
        .checkbox_border = 0x00404040,      // Dark sunken border
        .checkbox_check = 0x00000000,       // Black checkmark

        // Desktop - CDE-inspired deep teal workspace background
        // The iconic CDE teal that evokes Sun Microsystems and SGI workstations
        .desktop_bg = 0x00336666,           // Deep teal (CDE workspace color)

        // Taskbar/Panel - CDE front panel style (raised gray with bevels)
        // The panel should have visible 3D raised edges
        .taskbar_bg = 0x00B4B4B4,           // Warm gray panel (matches window bg)
        .taskbar_hover = 0x00C8C8C8,        // Lighter on hover
        .taskbar_active = 0x00A0A0A0,       // Darker when active/pressed
        .start_button = 0x00C8C8C8,         // Gray button (CDE style)
        .gauge_bg = 0x00606060,             // Dark gauge track (sunken)
        .gauge_fg = 0x00006666,             // Teal fill (accent color)

        // Menus - Classic Motif pulldown style with shadow
        // Menus appear as raised panels with 3D borders
        .menu_bg = 0x00D4D4D4,              // Light gray menu background
        .menu_border = 0x00404040,          // Dark border for shadow effect
        .menu_item_hover = 0x00336666,      // Teal highlight (CDE accent)
        .menu_text = 0x00000000,            // Black text
        .menu_text_disabled = 0x00808080,   // Gray disabled text
        .menu_separator = 0x00808080,       // Gray separator (etched line)

        // Scrollbar - Motif style with arrow buttons and raised thumb
        // Classic look: gray track, raised thumb with 3D bevels
        .scrollbar_bg = 0x00B4B4B4,         // Gray track (matches window)
        .scrollbar_thumb = 0x00C8C8C8,      // Light gray thumb (raised)
        .scrollbar_thumb_hover = 0x00D4D4D4,// Lighter on hover

        // Selection - CDE teal selection highlight
        .selection_bg = 0x00336666,         // Deep teal (consistent accent)
        .selection_text = 0x00FFFFFF,       // White text on selection

        // Status colors
        .color_error = 0x00CC0000,          // Classic red
        .color_warning = 0x00CC8800,        // Classic orange
        .color_success = 0x00006600,        // Classic green
        .color_info = 0x00000080,           // Navy blue

        // Link colors
        .link_color = 0x00000080,           // Navy blue
        .link_visited = 0x00800080,         // Purple
        .link_hover = 0x000000FF,           // Bright blue

        // Tooltip colors
        .tooltip_bg = 0x00FFFFC0,           // Classic pale yellow
        .tooltip_text = 0x00000000,         // Black
        .tooltip_border = 0x00000000,       // Black
    },

    // ========================================================================
    // THEME_DARK (1) - Full dark mode theme
    // ========================================================================
    {
        .name = "Dark Mode",
        .style = THEME_STYLE_MIXED,

        // Window titlebar
        .titlebar_active = 0x00202020,      // Very dark grey
        .titlebar_inactive = 0x00181818,    // Almost black
        .titlebar_text = 0x00E0E0E0,        // Light text

        // Window body
        .window_bg = 0x002D2D2D,            // Dark grey
        .window_border = 0x00404040,        // Medium grey

        // Window buttons
        .close_button = 0x00404040,         // Dark grey
        .close_button_hover = 0x00C03030,   // Red
        .minimize_button = 0x00505050,      // Grey
        .maximize_button = 0x00505050,      // Grey

        // Button widgets
        .button_bg = 0x00404040,            // Dark gray
        .button_bg_hover = 0x00505050,      // Medium gray
        .button_bg_pressed = 0x00303030,    // Darker gray
        .button_border = 0x00606060,        // Grey border
        .button_text = 0x00E0E0E0,          // Light text
        .button_disabled = 0x00606060,      // Grey

        // Text widgets
        .label_text = 0x00E0E0E0,           // Light text
        .textbox_bg = 0x00383838,           // Dark
        .textbox_border = 0x00606060,       // Grey
        .textbox_text = 0x00E0E0E0,         // Light
        .textbox_cursor = 0x00FFFFFF,       // White

        // Checkbox
        .checkbox_bg = 0x00383838,          // Dark
        .checkbox_border = 0x00606060,      // Grey
        .checkbox_check = 0x0080C0FF,       // Light blue

        // Desktop
        .desktop_bg = 0x00101820,           // Very dark blue

        // Taskbar
        .taskbar_bg = 0x00181818,           // Almost black
        .taskbar_hover = 0x00282828,        // Dark grey
        .taskbar_active = 0x00383838,       // Medium grey
        .start_button = 0x00282828,         // Dark
        .gauge_bg = 0x00101010,             // Very dark
        .gauge_fg = 0x00606060,             // Grey

        // Menus
        .menu_bg = 0x002D2D2D,              // Dark
        .menu_border = 0x00505050,          // Grey
        .menu_item_hover = 0x00405080,      // Dark blue
        .menu_text = 0x00E0E0E0,            // Light
        .menu_text_disabled = 0x00606060,   // Grey
        .menu_separator = 0x00404040,       // Dark grey

        // Scrollbar
        .scrollbar_bg = 0x00303030,         // Dark
        .scrollbar_thumb = 0x00505050,      // Grey
        .scrollbar_thumb_hover = 0x00606060,// Lighter

        // Selection
        .selection_bg = 0x00405080,         // Dark blue
        .selection_text = 0x00FFFFFF,       // White

        // Status colors
        .color_error = 0x00FF4444,          // Bright red
        .color_warning = 0x00FFAA00,        // Orange
        .color_success = 0x0044AA44,        // Green
        .color_info = 0x004488CC,           // Blue

        // Link colors
        .link_color = 0x006699FF,           // Light blue
        .link_visited = 0x00AA88CC,         // Light purple
        .link_hover = 0x0088BBFF,           // Lighter blue

        // Tooltip colors
        .tooltip_bg = 0x00404040,           // Dark gray
        .tooltip_text = 0x00FFFFFF,         // White
        .tooltip_border = 0x00606060,       // Medium gray
    },

    // ========================================================================
    // THEME_LIGHT (2) - Clean light theme
    // ========================================================================
    {
        .name = "Light Mode",
        .style = THEME_STYLE_MIXED,

        // Window titlebar
        .titlebar_active = 0x00FFFFFF,      // White
        .titlebar_inactive = 0x00F0F0F0,    // Light grey
        .titlebar_text = 0x00303030,        // Dark text

        // Window body
        .window_bg = 0x00FFFFFF,            // White
        .window_border = 0x00C0C0C0,        // Light grey

        // Window buttons
        .close_button = 0x00E0E0E0,         // Light
        .close_button_hover = 0x00E04040,   // Red
        .minimize_button = 0x00E0E0E0,      // Light
        .maximize_button = 0x00E0E0E0,      // Light

        // Button widgets
        .button_bg = 0x00E8E8E8,            // Light gray
        .button_bg_hover = 0x00D0D0D0,      // Medium gray
        .button_bg_pressed = 0x00B0B0B0,    // Darker gray
        .button_border = 0x00A0A0A0,        // Grey border
        .button_text = 0x00000000,          // Black
        .button_disabled = 0x00A0A0A0,      // Grey

        // Text widgets
        .label_text = 0x00000000,           // Black
        .textbox_bg = 0x00FFFFFF,           // White
        .textbox_border = 0x00B0B0B0,       // Grey
        .textbox_text = 0x00000000,         // Black
        .textbox_cursor = 0x00000000,       // Black

        // Checkbox
        .checkbox_bg = 0x00FFFFFF,          // White
        .checkbox_border = 0x00A0A0A0,      // Grey
        .checkbox_check = 0x00000000,       // Black

        // Desktop
        .desktop_bg = 0x00E8F0F8,           // Very light blue

        // Taskbar
        .taskbar_bg = 0x00F5F5F5,           // Light grey
        .taskbar_hover = 0x00E0E0E0,        // Grey
        .taskbar_active = 0x00D0D0D0,       // Medium grey
        .start_button = 0x00FFFFFF,         // White
        .gauge_bg = 0x00E0E0E0,             // Light grey
        .gauge_fg = 0x00808080,             // Grey

        // Menus
        .menu_bg = 0x00FFFFFF,              // White
        .menu_border = 0x00C0C0C0,          // Grey
        .menu_item_hover = 0x00D0E8FF,      // Light blue
        .menu_text = 0x00000000,            // Black
        .menu_text_disabled = 0x00909090,   // Grey
        .menu_separator = 0x00E0E0E0,       // Light grey

        // Scrollbar
        .scrollbar_bg = 0x00F0F0F0,         // Light
        .scrollbar_thumb = 0x00C0C0C0,      // Grey
        .scrollbar_thumb_hover = 0x00A0A0A0,// Darker

        // Selection
        .selection_bg = 0x000078D7,         // Windows blue
        .selection_text = 0x00FFFFFF,       // White

        // Status colors
        .color_error = 0x00DD0000,          // Red
        .color_warning = 0x00DD8800,        // Orange
        .color_success = 0x00008800,        // Green
        .color_info = 0x000066CC,           // Blue

        // Link colors
        .link_color = 0x000066CC,           // Blue
        .link_visited = 0x00660099,         // Purple
        .link_hover = 0x000088FF,           // Bright blue

        // Tooltip colors
        .tooltip_bg = 0x00FFFFC0,           // Pale yellow
        .tooltip_text = 0x00000000,         // Black
        .tooltip_border = 0x00606060,       // Gray
    },

    // ========================================================================
    // THEME_HIGH_CONTRAST (3) - High contrast for accessibility
    // ========================================================================
    {
        .name = "High Contrast",
        .style = THEME_STYLE_RETRO,

        // Window titlebar
        .titlebar_active = 0x00000080,      // Navy blue
        .titlebar_inactive = 0x00000000,    // Black
        .titlebar_text = 0x00FFFFFF,        // White

        // Window body
        .window_bg = 0x00000000,            // Black
        .window_border = 0x00FFFFFF,        // White

        // Window buttons
        .close_button = 0x00FF0000,         // Bright red
        .close_button_hover = 0x00FF4040,   // Lighter red
        .minimize_button = 0x0000FF00,      // Green
        .maximize_button = 0x000000FF,      // Blue

        // Button widgets
        .button_bg = 0x00000080,            // Navy
        .button_bg_hover = 0x000000C0,      // Brighter blue
        .button_bg_pressed = 0x00000040,    // Darker blue
        .button_border = 0x00FFFFFF,        // White
        .button_text = 0x00FFFFFF,          // White
        .button_disabled = 0x00808080,      // Grey

        // Text widgets
        .label_text = 0x00FFFFFF,           // White
        .textbox_bg = 0x00000000,           // Black
        .textbox_border = 0x00FFFFFF,       // White
        .textbox_text = 0x00FFFF00,         // Yellow
        .textbox_cursor = 0x00FFFFFF,       // White

        // Checkbox
        .checkbox_bg = 0x00000000,          // Black
        .checkbox_border = 0x00FFFFFF,      // White
        .checkbox_check = 0x0000FF00,       // Green

        // Desktop
        .desktop_bg = 0x00000000,           // Black

        // Taskbar
        .taskbar_bg = 0x00000000,           // Black
        .taskbar_hover = 0x00000080,        // Navy
        .taskbar_active = 0x000000C0,       // Blue
        .start_button = 0x00008000,         // Green
        .gauge_bg = 0x00404040,             // Dark grey
        .gauge_fg = 0x0000FF00,             // Bright green

        // Menus
        .menu_bg = 0x00000000,              // Black
        .menu_border = 0x00FFFFFF,          // White
        .menu_item_hover = 0x00000080,      // Navy
        .menu_text = 0x00FFFFFF,            // White
        .menu_text_disabled = 0x00808080,   // Grey
        .menu_separator = 0x00FFFFFF,       // White

        // Scrollbar
        .scrollbar_bg = 0x00000000,         // Black
        .scrollbar_thumb = 0x00FFFFFF,      // White
        .scrollbar_thumb_hover = 0x00FFFF00,// Yellow

        // Selection
        .selection_bg = 0x00FFFF00,         // Yellow
        .selection_text = 0x00000000,       // Black

        // Status colors - high contrast
        .color_error = 0x00FF0000,          // Pure red
        .color_warning = 0x00FFFF00,        // Yellow
        .color_success = 0x0000FF00,        // Pure green
        .color_info = 0x0000FFFF,           // Cyan

        // Link colors
        .link_color = 0x0000FFFF,           // Cyan
        .link_visited = 0x00FF00FF,         // Magenta
        .link_hover = 0x00FFFFFF,           // White

        // Tooltip colors
        .tooltip_bg = 0x000000FF,           // Blue
        .tooltip_text = 0x00FFFFFF,         // White
        .tooltip_border = 0x00FFFFFF,       // White
    },

    // ========================================================================
    // THEME_CLASSIC (4) - Windows 95/98 style
    // ========================================================================
    {
        .name = "Classic (Win95)",
        .style = THEME_STYLE_RETRO,

        // Window titlebar
        .titlebar_active = 0x00000080,      // Navy blue
        .titlebar_inactive = 0x00808080,    // Grey
        .titlebar_text = 0x00FFFFFF,        // White

        // Window body
        .window_bg = 0x00C0C0C0,            // Silver/grey
        .window_border = 0x00000000,        // Black

        // Window buttons (Win95 style - grey raised buttons)
        .close_button = 0x00C0C0C0,         // Grey
        .close_button_hover = 0x00E0E0E0,   // Lighter
        .minimize_button = 0x00C0C0C0,      // Grey
        .maximize_button = 0x00C0C0C0,      // Grey

        // Button widgets (3D raised look)
        .button_bg = 0x00C0C0C0,            // Silver
        .button_bg_hover = 0x00D0D0D0,      // Lighter
        .button_bg_pressed = 0x00A0A0A0,    // Darker (sunken)
        .button_border = 0x00000000,        // Black
        .button_text = 0x00000000,          // Black
        .button_disabled = 0x00808080,      // Grey

        // Text widgets
        .label_text = 0x00000000,           // Black
        .textbox_bg = 0x00FFFFFF,           // White
        .textbox_border = 0x00000000,       // Black
        .textbox_text = 0x00000000,         // Black
        .textbox_cursor = 0x00000000,       // Black

        // Checkbox
        .checkbox_bg = 0x00FFFFFF,          // White
        .checkbox_border = 0x00000000,      // Black
        .checkbox_check = 0x00000000,       // Black

        // Desktop
        .desktop_bg = 0x00008080,           // Teal (classic Windows)

        // Taskbar
        .taskbar_bg = 0x00C0C0C0,           // Silver
        .taskbar_hover = 0x00D0D0D0,        // Lighter
        .taskbar_active = 0x00A0A0A0,       // Darker (pressed)
        .start_button = 0x00C0C0C0,         // Silver
        .gauge_bg = 0x00808080,             // Grey
        .gauge_fg = 0x00000080,             // Navy

        // Menus
        .menu_bg = 0x00C0C0C0,              // Silver
        .menu_border = 0x00000000,          // Black
        .menu_item_hover = 0x00000080,      // Navy
        .menu_text = 0x00000000,            // Black
        .menu_text_disabled = 0x00808080,   // Grey
        .menu_separator = 0x00808080,       // Grey

        // Scrollbar
        .scrollbar_bg = 0x00C0C0C0,         // Silver
        .scrollbar_thumb = 0x00808080,      // Grey
        .scrollbar_thumb_hover = 0x00606060,// Darker

        // Selection
        .selection_bg = 0x00000080,         // Navy
        .selection_text = 0x00FFFFFF,       // White

        // Status colors - Windows 95 style
        .color_error = 0x00FF0000,          // Red
        .color_warning = 0x00FFFF00,        // Yellow
        .color_success = 0x00008000,        // Dark green
        .color_info = 0x00000080,           // Navy

        // Link colors
        .link_color = 0x00000080,           // Navy
        .link_visited = 0x00800080,         // Purple
        .link_hover = 0x000000FF,           // Blue

        // Tooltip colors
        .tooltip_bg = 0x00FFFFE0,           // Light yellow
        .tooltip_text = 0x00000000,         // Black
        .tooltip_border = 0x00000000,       // Black
    },

    // ========================================================================
    // THEME_OCEAN (5) - Blue ocean theme
    // ========================================================================
    {
        .name = "Ocean",
        .style = THEME_STYLE_MIXED,

        // Window titlebar
        .titlebar_active = 0x00205080,      // Deep blue
        .titlebar_inactive = 0x00304050,    // Dark slate
        .titlebar_text = 0x00FFFFFF,        // White

        // Window body
        .window_bg = 0x00E8F4FC,            // Very light blue
        .window_border = 0x00306090,        // Medium blue

        // Window buttons
        .close_button = 0x00408090,         // Teal blue
        .close_button_hover = 0x00C05050,   // Red
        .minimize_button = 0x00508090,      // Lighter teal
        .maximize_button = 0x00508090,      // Lighter teal

        // Button widgets
        .button_bg = 0x0080B0D0,            // Light blue
        .button_bg_hover = 0x0090C0E0,      // Lighter blue
        .button_bg_pressed = 0x00609090,    // Darker
        .button_border = 0x00306080,        // Blue
        .button_text = 0x00FFFFFF,          // White
        .button_disabled = 0x00808080,      // Grey

        // Text widgets
        .label_text = 0x00203040,           // Dark blue
        .textbox_bg = 0x00FFFFFF,           // White
        .textbox_border = 0x00608090,       // Blue grey
        .textbox_text = 0x00203040,         // Dark blue
        .textbox_cursor = 0x00205080,       // Deep blue

        // Checkbox
        .checkbox_bg = 0x00FFFFFF,          // White
        .checkbox_border = 0x00608090,      // Blue grey
        .checkbox_check = 0x00205080,       // Deep blue

        // Desktop
        .desktop_bg = 0x00306090,           // Ocean blue

        // Taskbar
        .taskbar_bg = 0x00204060,           // Dark blue
        .taskbar_hover = 0x00305070,        // Lighter
        .taskbar_active = 0x00406080,       // Even lighter
        .start_button = 0x00305080,         // Blue
        .gauge_bg = 0x00102030,             // Very dark
        .gauge_fg = 0x0060A0C0,             // Light blue

        // Menus
        .menu_bg = 0x00F0F8FF,              // Alice blue
        .menu_border = 0x00608090,          // Blue grey
        .menu_item_hover = 0x00205080,      // Deep blue
        .menu_text = 0x00203040,            // Dark blue
        .menu_text_disabled = 0x00808090,   // Grey blue
        .menu_separator = 0x00C0D0E0,       // Light blue grey

        // Scrollbar
        .scrollbar_bg = 0x00D0E0F0,         // Light blue
        .scrollbar_thumb = 0x0080A0C0,      // Blue
        .scrollbar_thumb_hover = 0x00609090,// Darker

        // Selection
        .selection_bg = 0x00205080,         // Deep blue
        .selection_text = 0x00FFFFFF,       // White

        // Status colors
        .color_error = 0x00C04040,
        .color_warning = 0x00C09030,
        .color_success = 0x00309060,
        .color_info = 0x003080C0,

        // Link colors
        .link_color = 0x00205080,
        .link_visited = 0x00604080,
        .link_hover = 0x003090C0,

        // Tooltip colors
        .tooltip_bg = 0x00E8F4FC,
        .tooltip_text = 0x00203040,
        .tooltip_border = 0x00608090,
    },

    // ========================================================================
    // THEME_SUNSET (6) - Warm orange/red theme
    // ========================================================================
    {
        .name = "Sunset",
        .style = THEME_STYLE_MIXED,

        // Window titlebar
        .titlebar_active = 0x00804020,      // Dark orange/brown
        .titlebar_inactive = 0x00503020,    // Dark brown
        .titlebar_text = 0x00FFFFFF,        // White

        // Window body
        .window_bg = 0x00FFF8F0,            // Cream/warm white
        .window_border = 0x00905030,        // Burnt orange

        // Window buttons
        .close_button = 0x00A04020,         // Darker orange
        .close_button_hover = 0x00C03030,   // Red
        .minimize_button = 0x00B06030,      // Orange
        .maximize_button = 0x00B06030,      // Orange

        // Button widgets
        .button_bg = 0x00E0A080,            // Peach
        .button_bg_hover = 0x00F0B090,      // Lighter peach
        .button_bg_pressed = 0x00C08060,    // Darker
        .button_border = 0x00906040,        // Brown
        .button_text = 0x00402010,          // Dark brown
        .button_disabled = 0x00808080,      // Grey

        // Text widgets
        .label_text = 0x00402010,           // Dark brown
        .textbox_bg = 0x00FFFFF8,           // Cream
        .textbox_border = 0x00A07050,       // Brown
        .textbox_text = 0x00402010,         // Dark brown
        .textbox_cursor = 0x00804020,       // Brown

        // Checkbox
        .checkbox_bg = 0x00FFFFF8,          // Cream
        .checkbox_border = 0x00A07050,      // Brown
        .checkbox_check = 0x00804020,       // Brown

        // Desktop
        .desktop_bg = 0x00D06830,           // Sunset orange

        // Taskbar
        .taskbar_bg = 0x00402820,           // Dark brown
        .taskbar_hover = 0x00503830,        // Lighter
        .taskbar_active = 0x00604840,       // Even lighter
        .start_button = 0x00A05020,         // Orange
        .gauge_bg = 0x00201810,             // Very dark
        .gauge_fg = 0x00E08040,             // Orange

        // Menus
        .menu_bg = 0x00FFF0E8,              // Light peach
        .menu_border = 0x00A07050,          // Brown
        .menu_item_hover = 0x00C06020,      // Orange
        .menu_text = 0x00402010,            // Dark brown
        .menu_text_disabled = 0x00908070,   // Grey brown
        .menu_separator = 0x00D0C0B0,       // Light brown

        // Scrollbar
        .scrollbar_bg = 0x00E0D0C0,         // Light brown
        .scrollbar_thumb = 0x00C09060,      // Brown
        .scrollbar_thumb_hover = 0x00A07040,// Darker

        // Selection
        .selection_bg = 0x00C06020,         // Orange
        .selection_text = 0x00FFFFFF,       // White

        // Status colors
        .color_error = 0x00C03030,
        .color_warning = 0x00C09020,
        .color_success = 0x00608040,
        .color_info = 0x00A06040,

        // Link colors
        .link_color = 0x00804020,
        .link_visited = 0x00604040,
        .link_hover = 0x00C06030,

        // Tooltip colors
        .tooltip_bg = 0x00FFF8F0,
        .tooltip_text = 0x00402010,
        .tooltip_border = 0x00A07050,
    },

    // ========================================================================
    // THEME_FOREST (7) - Green nature theme
    // ========================================================================
    {
        .name = "Forest",
        .style = THEME_STYLE_MIXED,

        // Window titlebar
        .titlebar_active = 0x00205020,      // Dark green
        .titlebar_inactive = 0x00303830,    // Grey green
        .titlebar_text = 0x00E0FFE0,        // Light green

        // Window body
        .window_bg = 0x00F0F8F0,            // Very light green
        .window_border = 0x00408040,        // Medium green

        // Window buttons
        .close_button = 0x00506050,         // Grey green
        .close_button_hover = 0x00C04040,   // Red
        .minimize_button = 0x00608060,      // Green
        .maximize_button = 0x00608060,      // Green

        // Button widgets
        .button_bg = 0x0080C080,            // Medium green
        .button_bg_hover = 0x0090D090,      // Lighter
        .button_bg_pressed = 0x00609060,    // Darker
        .button_border = 0x00306030,        // Dark green
        .button_text = 0x00103010,          // Very dark green
        .button_disabled = 0x00808080,      // Grey

        // Text widgets
        .label_text = 0x00203020,           // Dark green
        .textbox_bg = 0x00FFFFFF,           // White
        .textbox_border = 0x00609060,       // Green
        .textbox_text = 0x00203020,         // Dark green
        .textbox_cursor = 0x00205020,       // Dark green

        // Checkbox
        .checkbox_bg = 0x00FFFFFF,          // White
        .checkbox_border = 0x00609060,      // Green
        .checkbox_check = 0x00205020,       // Dark green

        // Desktop
        .desktop_bg = 0x00306830,           // Forest green

        // Taskbar
        .taskbar_bg = 0x00203820,           // Dark green
        .taskbar_hover = 0x00304830,        // Lighter
        .taskbar_active = 0x00405840,       // Even lighter
        .start_button = 0x00406040,         // Green
        .gauge_bg = 0x00101810,             // Very dark
        .gauge_fg = 0x0060A060,             // Medium green

        // Menus
        .menu_bg = 0x00F0FFF0,              // Very light green
        .menu_border = 0x00609060,          // Green
        .menu_item_hover = 0x00408040,      // Medium green
        .menu_text = 0x00203020,            // Dark green
        .menu_text_disabled = 0x00809080,   // Grey green
        .menu_separator = 0x00C0D0C0,       // Light grey green

        // Scrollbar
        .scrollbar_bg = 0x00D0E0D0,         // Light green
        .scrollbar_thumb = 0x0080A080,      // Green
        .scrollbar_thumb_hover = 0x00608060,// Darker

        // Selection
        .selection_bg = 0x00408040,         // Medium green
        .selection_text = 0x00FFFFFF,       // White

        // Status colors
        .color_error = 0x00A03030,
        .color_warning = 0x00A08020,
        .color_success = 0x00306030,
        .color_info = 0x00306060,

        // Link colors
        .link_color = 0x00205020,
        .link_visited = 0x00405040,
        .link_hover = 0x00408040,

        // Tooltip colors
        .tooltip_bg = 0x00F0FFF0,
        .tooltip_text = 0x00203020,
        .tooltip_border = 0x00609060,
    },

    // ========================================================================
    // THEME_MODERN_LIGHT (8) - macOS Big Sur light mode
    // Clean, modern design with subtle depth and rounded feel
    // ========================================================================
    {
        .name = "Modern Light",
        .style = THEME_STYLE_MODERN,

        // Window titlebar - translucent gray appearance
        .titlebar_active = 0x00E8E8E8,      // Light translucent gray
        .titlebar_inactive = 0x00F0F0F0,    // Even lighter inactive
        .titlebar_text = 0x001D1D1F,        // Near-black text

        // Window body - clean white with subtle borders
        .window_bg = 0x00FFFFFF,            // Pure white
        .window_border = 0x00D1D1D6,        // Subtle gray border

        // Window buttons - macOS traffic lights (left side)
        .close_button = 0x00FF5F57,         // Red (close)
        .close_button_hover = 0x00FF7369,   // Lighter red on hover
        .minimize_button = 0x00FFBD2E,      // Yellow (minimize)
        .maximize_button = 0x0028CA42,      // Green (maximize/zoom)

        // Button widgets - rounded pill style
        .button_bg = 0x00FFFFFF,            // White background
        .button_bg_hover = 0x00F5F5F7,      // Off-white on hover
        .button_bg_pressed = 0x00E5E5E7,    // Slightly darker when pressed
        .button_border = 0x00D1D1D6,        // Subtle border
        .button_text = 0x001D1D1F,          // Near-black text
        .button_disabled = 0x00C7C7CC,      // Gray disabled

        // Text widgets - clean input fields
        .label_text = 0x001D1D1F,           // Near-black
        .textbox_bg = 0x00FFFFFF,           // White
        .textbox_border = 0x00D1D1D6,       // Subtle border
        .textbox_text = 0x001D1D1F,         // Near-black
        .textbox_cursor = 0x00007AFF,       // System blue cursor

        // Checkbox - rounded style
        .checkbox_bg = 0x00FFFFFF,          // White
        .checkbox_border = 0x00D1D1D6,      // Subtle border
        .checkbox_check = 0x00007AFF,       // System blue checkmark

        // Desktop - macOS Big Sur gradient-inspired solid
        .desktop_bg = 0x005BADE9,           // Sky blue (Big Sur inspired)

        // Dock/Taskbar - translucent appearance
        .taskbar_bg = 0x00F5F5F7,           // Off-white translucent
        .taskbar_hover = 0x00E5E5E7,        // Slightly darker hover
        .taskbar_active = 0x00D1D1D6,       // Active indicator
        .start_button = 0x00007AFF,         // System blue accent
        .gauge_bg = 0x00E5E5E7,             // Light gray track
        .gauge_fg = 0x00007AFF,             // System blue fill

        // Menus - floating panels with shadows
        .menu_bg = 0x00FFFFFF,              // White
        .menu_border = 0x00E5E5E7,          // Very subtle border
        .menu_item_hover = 0x00007AFF,      // System blue highlight
        .menu_text = 0x001D1D1F,            // Near-black
        .menu_text_disabled = 0x00C7C7CC,   // Gray
        .menu_separator = 0x00E5E5E7,       // Subtle separator

        // Scrollbar - thin overlay style
        .scrollbar_bg = 0x00F5F5F7,         // Nearly invisible track
        .scrollbar_thumb = 0x00C7C7CC,      // Gray thumb
        .scrollbar_thumb_hover = 0x00A1A1A6,// Darker on hover

        // Selection - system blue
        .selection_bg = 0x00007AFF,         // System blue
        .selection_text = 0x00FFFFFF,       // White text

        // Status colors - macOS style
        .color_error = 0x00FF3B30,          // iOS red
        .color_warning = 0x00FF9500,        // iOS orange
        .color_success = 0x0034C759,        // iOS green
        .color_info = 0x00007AFF,           // iOS blue

        // Link colors
        .link_color = 0x00007AFF,           // System blue
        .link_visited = 0x005856D6,         // Purple
        .link_hover = 0x00409CFF,           // Lighter blue

        // Tooltip colors
        .tooltip_bg = 0x00F5F5F7,
        .tooltip_text = 0x001D1D1F,
        .tooltip_border = 0x00D1D1D6,
    },

    // ========================================================================
    // THEME_MODERN_DARK (9) - macOS Big Sur dark mode
    // Dark surfaces with vibrant accent colors and depth
    // ========================================================================
    {
        .name = "Modern Dark",
        .style = THEME_STYLE_MODERN,

        // Window titlebar - dark translucent
        .titlebar_active = 0x002D2D2D,      // Dark gray
        .titlebar_inactive = 0x00252525,    // Darker inactive
        .titlebar_text = 0x00F5F5F7,        // Off-white text

        // Window body - dark with subtle borders
        .window_bg = 0x001E1E1E,            // Dark background
        .window_border = 0x003D3D3F,        // Subtle dark border

        // Window buttons - macOS traffic lights (same vibrant colors)
        .close_button = 0x00FF5F57,         // Red (close)
        .close_button_hover = 0x00FF7369,   // Lighter red on hover
        .minimize_button = 0x00FFBD2E,      // Yellow (minimize)
        .maximize_button = 0x0028CA42,      // Green (maximize/zoom)

        // Button widgets - dark pill style
        .button_bg = 0x00323234,            // Dark gray
        .button_bg_hover = 0x003A3A3C,      // Slightly lighter
        .button_bg_pressed = 0x00484848,    // Lighter when pressed
        .button_border = 0x003D3D3F,        // Subtle border
        .button_text = 0x00F5F5F7,          // Off-white
        .button_disabled = 0x00636366,      // Gray disabled

        // Text widgets - dark input fields
        .label_text = 0x00F5F5F7,           // Off-white
        .textbox_bg = 0x00262628,           // Dark field
        .textbox_border = 0x003D3D3F,       // Subtle border
        .textbox_text = 0x00F5F5F7,         // Off-white
        .textbox_cursor = 0x000A84FF,       // Brighter blue cursor

        // Checkbox - dark rounded style
        .checkbox_bg = 0x00262628,          // Dark
        .checkbox_border = 0x003D3D3F,      // Subtle border
        .checkbox_check = 0x000A84FF,       // Bright blue checkmark

        // Desktop - macOS Big Sur dark gradient-inspired solid
        .desktop_bg = 0x001C1C28,           // Deep dark purple-blue

        // Dock/Taskbar - dark translucent appearance
        .taskbar_bg = 0x00252528,           // Dark translucent
        .taskbar_hover = 0x003A3A3C,        // Slightly lighter hover
        .taskbar_active = 0x00484848,       // Active indicator
        .start_button = 0x000A84FF,         // Bright blue accent
        .gauge_bg = 0x003A3A3C,             // Dark track
        .gauge_fg = 0x000A84FF,             // Bright blue fill

        // Menus - dark floating panels
        .menu_bg = 0x00262628,              // Dark background
        .menu_border = 0x003D3D3F,          // Subtle border
        .menu_item_hover = 0x000A84FF,      // Bright blue highlight
        .menu_text = 0x00F5F5F7,            // Off-white
        .menu_text_disabled = 0x00636366,   // Gray
        .menu_separator = 0x003D3D3F,       // Subtle separator

        // Scrollbar - thin dark overlay style
        .scrollbar_bg = 0x002D2D2D,         // Nearly invisible track
        .scrollbar_thumb = 0x00636366,      // Gray thumb
        .scrollbar_thumb_hover = 0x008E8E93,// Lighter on hover

        // Selection - bright blue
        .selection_bg = 0x000A84FF,         // Bright blue
        .selection_text = 0x00FFFFFF,       // White text

        // Status colors - macOS dark style
        .color_error = 0x00FF453A,          // iOS red (dark mode)
        .color_warning = 0x00FF9F0A,        // iOS orange (dark mode)
        .color_success = 0x0032D74B,        // iOS green (dark mode)
        .color_info = 0x000A84FF,           // iOS blue (dark mode)

        // Link colors
        .link_color = 0x000A84FF,           // Bright blue
        .link_visited = 0x00BF5AF2,         // Purple
        .link_hover = 0x0040A9FF,           // Lighter blue

        // Tooltip colors
        .tooltip_bg = 0x00323234,
        .tooltip_text = 0x00F5F5F7,
        .tooltip_border = 0x003D3D3F,
    },

    // ========================================================================
    // THEME_FLUENT_LIGHT (10) - Windows 11 Fluent Design Light Mode
    // Clean, modern design with Mica material effect aesthetics
    // Features: rounded corners (8px), flat design, subtle hover effects
    // ========================================================================
    {
        .name = "Fluent Light",
        .style = THEME_STYLE_MODERN,

        // Window titlebar - Mica material effect (light translucent)
        .titlebar_active = 0x00F3F3F3,      // Light Mica background
        .titlebar_inactive = 0x00F9F9F9,    // Lighter inactive
        .titlebar_text = 0x001A1A1A,        // Near-black text

        // Window body - clean white surfaces
        .window_bg = 0x00FFFFFF,            // Pure white
        .window_border = 0x00E5E5E5,        // Subtle light border

        // Window buttons - Windows 11 style (right-aligned)
        .close_button = 0x00E5E5E5,         // Light gray base
        .close_button_hover = 0x00C42B1C,   // Windows red on hover
        .minimize_button = 0x00E5E5E5,      // Light gray
        .maximize_button = 0x00E5E5E5,      // Light gray

        // Button widgets - Fluent pill/rounded style
        .button_bg = 0x00FBFBFB,            // Almost white
        .button_bg_hover = 0x00F0F0F0,      // Light gray on hover
        .button_bg_pressed = 0x00E0E0E0,    // Darker when pressed
        .button_border = 0x00E5E5E5,        // Subtle border
        .button_text = 0x001A1A1A,          // Near-black text
        .button_disabled = 0x00A0A0A0,      // Gray disabled

        // Text widgets - clean input fields with rounded borders
        .label_text = 0x001A1A1A,           // Near-black
        .textbox_bg = 0x00FFFFFF,           // White
        .textbox_border = 0x00E5E5E5,       // Light border
        .textbox_text = 0x001A1A1A,         // Near-black
        .textbox_cursor = 0x000078D4,       // Windows blue cursor

        // Checkbox - rounded style with accent
        .checkbox_bg = 0x00FFFFFF,          // White
        .checkbox_border = 0x00E5E5E5,      // Light border
        .checkbox_check = 0x000078D4,       // Windows blue checkmark

        // Desktop - Windows 11 default light background
        .desktop_bg = 0x00B4C8D8,           // Light blue-gray (Win11 default)

        // Taskbar - centered, translucent Acrylic-inspired
        .taskbar_bg = 0x00F3F3F3,           // Light Mica/Acrylic
        .taskbar_hover = 0x00E8E8E8,        // Subtle hover
        .taskbar_active = 0x00DDDDDD,       // Active indicator
        .start_button = 0x000078D4,         // Windows blue
        .gauge_bg = 0x00E0E0E0,             // Light track
        .gauge_fg = 0x000078D4,             // Windows blue fill

        // Menus - Fluent flyout panels with rounded corners
        .menu_bg = 0x00FCFCFC,              // Off-white
        .menu_border = 0x00E5E5E5,          // Subtle border
        .menu_item_hover = 0x000078D4,      // Windows blue highlight
        .menu_text = 0x001A1A1A,            // Near-black
        .menu_text_disabled = 0x00A0A0A0,   // Gray
        .menu_separator = 0x00E5E5E5,       // Subtle separator

        // Scrollbar - thin modern overlay style
        .scrollbar_bg = 0x00F0F0F0,         // Very subtle track
        .scrollbar_thumb = 0x00A0A0A0,      // Gray thumb
        .scrollbar_thumb_hover = 0x00787878,// Darker on hover

        // Selection - Windows accent blue
        .selection_bg = 0x000078D4,         // Windows blue
        .selection_text = 0x00FFFFFF,       // White text

        // Status colors - Windows 11 style
        .color_error = 0x00C42B1C,          // Windows red
        .color_warning = 0x00F5A623,        // Windows orange
        .color_success = 0x0017A653,        // Windows green
        .color_info = 0x000078D4,           // Windows blue

        // Link colors
        .link_color = 0x000078D4,           // Windows blue
        .link_visited = 0x006B5EAE,         // Purple
        .link_hover = 0x0060CDFF,           // Light blue

        // Tooltip colors
        .tooltip_bg = 0x00FCFCFC,
        .tooltip_text = 0x001A1A1A,
        .tooltip_border = 0x00E5E5E5,
    },

    // ========================================================================
    // THEME_FLUENT_DARK (11) - Windows 11 Fluent Design Dark Mode
    // Dark surfaces with Mica material aesthetics and vibrant accents
    // Features: rounded corners (8px), flat design, subtle hover effects
    // ========================================================================
    {
        .name = "Fluent Dark",
        .style = THEME_STYLE_MODERN,

        // Window titlebar - Dark Mica material effect
        .titlebar_active = 0x00202020,      // Dark Mica background
        .titlebar_inactive = 0x00282828,    // Slightly lighter inactive
        .titlebar_text = 0x00FFFFFF,        // White text

        // Window body - dark surfaces
        .window_bg = 0x00282828,            // Dark background
        .window_border = 0x003D3D3D,        // Subtle dark border

        // Window buttons - Windows 11 dark style
        .close_button = 0x00383838,         // Dark gray base
        .close_button_hover = 0x00C42B1C,   // Windows red on hover
        .minimize_button = 0x00383838,      // Dark gray
        .maximize_button = 0x00383838,      // Dark gray

        // Button widgets - Dark Fluent pill style
        .button_bg = 0x00323232,            // Dark gray
        .button_bg_hover = 0x003C3C3C,      // Slightly lighter on hover
        .button_bg_pressed = 0x00484848,    // Lighter when pressed
        .button_border = 0x003D3D3D,        // Subtle border
        .button_text = 0x00FFFFFF,          // White text
        .button_disabled = 0x00707070,      // Gray disabled

        // Text widgets - dark input fields
        .label_text = 0x00FFFFFF,           // White
        .textbox_bg = 0x00303030,           // Dark field
        .textbox_border = 0x003D3D3D,       // Subtle border
        .textbox_text = 0x00FFFFFF,         // White
        .textbox_cursor = 0x0060CDFF,       // Lighter blue cursor

        // Checkbox - dark rounded style
        .checkbox_bg = 0x00303030,          // Dark
        .checkbox_border = 0x003D3D3D,      // Subtle border
        .checkbox_check = 0x0060CDFF,       // Lighter blue checkmark

        // Desktop - Windows 11 dark default background
        .desktop_bg = 0x00202428,           // Deep dark blue-gray

        // Taskbar - dark centered, translucent Acrylic-inspired
        .taskbar_bg = 0x00202020,           // Dark Mica/Acrylic
        .taskbar_hover = 0x002D2D2D,        // Subtle hover
        .taskbar_active = 0x00383838,       // Active indicator
        .start_button = 0x0060CDFF,         // Light blue accent
        .gauge_bg = 0x00383838,             // Dark track
        .gauge_fg = 0x0060CDFF,             // Light blue fill

        // Menus - Dark Fluent flyout panels
        .menu_bg = 0x002C2C2C,              // Dark background
        .menu_border = 0x003D3D3D,          // Subtle border
        .menu_item_hover = 0x0060CDFF,      // Light blue highlight
        .menu_text = 0x00FFFFFF,            // White
        .menu_text_disabled = 0x00707070,   // Gray
        .menu_separator = 0x003D3D3D,       // Subtle separator

        // Scrollbar - thin dark overlay style
        .scrollbar_bg = 0x00282828,         // Very subtle track
        .scrollbar_thumb = 0x00606060,      // Gray thumb
        .scrollbar_thumb_hover = 0x00888888,// Lighter on hover

        // Selection - Windows accent light blue
        .selection_bg = 0x0060CDFF,         // Light blue
        .selection_text = 0x00000000,       // Black text for contrast

        // Status colors - Windows 11 dark style
        .color_error = 0x00FF6961,          // Lighter red
        .color_warning = 0x00FFBF47,        // Lighter orange
        .color_success = 0x0029CF62,        // Lighter green
        .color_info = 0x0060CDFF,           // Light blue

        // Link colors
        .link_color = 0x0060CDFF,           // Light blue
        .link_visited = 0x00B47EDE,         // Light purple
        .link_hover = 0x0090DDFF,           // Lighter blue

        // Tooltip colors
        .tooltip_bg = 0x002C2C2C,
        .tooltip_text = 0x00FFFFFF,
        .tooltip_border = 0x003D3D3D,
    },
};

// Number of available themes
static const int g_theme_count = 12;

// Runtime initialization helper to ensure theme colors are set
// This fixes issues with static initialization not working in kernel environment
static void init_theme_colors_runtime(void) {
    // Theme 0: Retro UNIX (CDE/Motif)
    g_themes[0].titlebar_active = 0x00336666;    // Deep teal
    g_themes[0].titlebar_inactive = 0x00808080;  // Medium gray
    g_themes[0].titlebar_text = 0x00FFFFFF;      // White text
    g_themes[0].window_bg = 0x00B4B4B4;          // Warm gray
    g_themes[0].window_border = 0x00404040;      // Dark gray
    g_themes[0].close_button = 0x00C8C8C8;       // Light gray
    g_themes[0].close_button_hover = 0x00D8D8D8; // Lighter gray
    g_themes[0].minimize_button = 0x00C8C8C8;    // Light gray
    g_themes[0].maximize_button = 0x00C8C8C8;    // Light gray
    g_themes[0].button_bg = 0x00C8C8C8;
    g_themes[0].button_text = 0x00000000;
    g_themes[0].menu_bg = 0x00D4D4D4;
    g_themes[0].menu_text = 0x00000000;
    g_themes[0].menu_item_hover = 0x00336666;

    // Theme 1: Dark Mode
    g_themes[1].titlebar_active = 0x00202020;    // Very dark grey
    g_themes[1].titlebar_inactive = 0x00181818;  // Almost black
    g_themes[1].titlebar_text = 0x00E0E0E0;      // Light text
    g_themes[1].window_bg = 0x002D2D2D;          // Dark grey
    g_themes[1].window_border = 0x00404040;      // Medium grey
    g_themes[1].close_button = 0x00404040;       // Dark grey
    g_themes[1].close_button_hover = 0x00C03030; // Red
    g_themes[1].minimize_button = 0x00505050;    // Grey
    g_themes[1].maximize_button = 0x00505050;    // Grey
    g_themes[1].button_bg = 0x00404040;
    g_themes[1].button_text = 0x00E0E0E0;
    g_themes[1].menu_bg = 0x002D2D2D;
    g_themes[1].menu_text = 0x00E0E0E0;
    g_themes[1].menu_item_hover = 0x00405080;

    // Theme 2: Light
    g_themes[2].titlebar_active = 0x00FFFFFF;    // White
    g_themes[2].titlebar_inactive = 0x00F0F0F0;  // Light grey
    g_themes[2].titlebar_text = 0x00303030;      // Dark text
    g_themes[2].window_bg = 0x00FFFFFF;          // White
    g_themes[2].window_border = 0x00A0A0A0;      // Grey
    g_themes[2].close_button = 0x00E0E0E0;       // Light grey
    g_themes[2].close_button_hover = 0x00E04040; // Red
    g_themes[2].minimize_button = 0x00E0E0E0;    // Light grey
    g_themes[2].maximize_button = 0x00E0E0E0;    // Light grey
    g_themes[2].button_bg = 0x00F0F0F0;
    g_themes[2].button_text = 0x00000000;
    g_themes[2].menu_bg = 0x00FFFFFF;
    g_themes[2].menu_text = 0x00000000;
    g_themes[2].menu_item_hover = 0x003399FF;

    // Theme 3: High Contrast
    g_themes[3].titlebar_active = 0x00000080;    // Navy blue
    g_themes[3].titlebar_inactive = 0x00404040;  // Dark grey
    g_themes[3].titlebar_text = 0x00FFFFFF;      // White
    g_themes[3].window_bg = 0x00000000;          // Black
    g_themes[3].window_border = 0x00FFFFFF;      // White
    g_themes[3].close_button = 0x00000000;       // Black
    g_themes[3].close_button_hover = 0x00FF0000; // Red
    g_themes[3].minimize_button = 0x00000000;    // Black
    g_themes[3].maximize_button = 0x00000000;    // Black
    g_themes[3].button_bg = 0x00000000;
    g_themes[3].button_text = 0x00FFFFFF;
    g_themes[3].menu_bg = 0x00000000;
    g_themes[3].menu_text = 0x00FFFFFF;
    g_themes[3].menu_item_hover = 0x00FFFF00;

    // Theme 4: Classic (Windows 95/98 style)
    g_themes[4].titlebar_active = 0x00000080;    // Navy blue
    g_themes[4].titlebar_inactive = 0x00808080;  // Grey
    g_themes[4].titlebar_text = 0x00FFFFFF;      // White
    g_themes[4].window_bg = 0x00C0C0C0;          // Silver
    g_themes[4].window_border = 0x00404040;      // Dark grey
    g_themes[4].close_button = 0x00C0C0C0;       // Silver
    g_themes[4].close_button_hover = 0x00D0D0D0; // Light silver
    g_themes[4].minimize_button = 0x00C0C0C0;    // Silver
    g_themes[4].maximize_button = 0x00C0C0C0;    // Silver
    g_themes[4].button_bg = 0x00C0C0C0;
    g_themes[4].button_text = 0x00000000;
    g_themes[4].menu_bg = 0x00C0C0C0;
    g_themes[4].menu_text = 0x00000000;
    g_themes[4].menu_item_hover = 0x00000080;

    // Themes 5-11: Initialize with reasonable defaults if static init failed
    for (int i = 5; i < MAX_THEMES; i++) {
        if (g_themes[i].titlebar_text == 0 && g_themes[i].titlebar_active == 0) {
            // Theme appears uninitialized, set defaults
            g_themes[i].titlebar_active = 0x00336666;
            g_themes[i].titlebar_inactive = 0x00808080;
            g_themes[i].titlebar_text = 0x00FFFFFF;
            g_themes[i].window_bg = 0x00B4B4B4;
            g_themes[i].window_border = 0x00404040;
            g_themes[i].close_button = 0x00C8C8C8;
            g_themes[i].close_button_hover = 0x00D8D8D8;
            g_themes[i].minimize_button = 0x00C8C8C8;
            g_themes[i].maximize_button = 0x00C8C8C8;
            g_themes[i].button_bg = 0x00C8C8C8;
            g_themes[i].button_text = 0x00000000;
            g_themes[i].menu_bg = 0x00D4D4D4;
            g_themes[i].menu_text = 0x00000000;
            g_themes[i].menu_item_hover = 0x00336666;
        }
    }
}

// Initialize the theme system
void theme_init(void) {
    // Guard against re-initialization
    if (g_themes_initialized) {
        return;
    }

    // Explicitly initialize theme colors at runtime
    // This ensures colors work even if static initialization fails
    init_theme_colors_runtime();

    g_current_theme = THEME_DEFAULT;
    g_themes_initialized = true;

    kprintf("[Themes] Theme system initialized with %d themes\n", g_theme_count);
    kprintf("[Themes] Default theme: %s (titlebar=0x%08X, text=0x%08X)\n",
            g_themes[0].name, g_themes[0].titlebar_active, g_themes[0].titlebar_text);
}


// Get the currently active theme
const theme_t *theme_get_current(void) {
    if (g_current_theme >= 0 && g_current_theme < g_theme_count) {
        return &g_themes[g_current_theme];
    }
    return &g_themes[0];  // Fallback to default
}

// Get a specific theme by ID
const theme_t *theme_get_by_id(int theme_id) {
    if (theme_id >= 0 && theme_id < g_theme_count) {
        return &g_themes[theme_id];
    }
    return NULL;
}

// Get the current theme ID
int theme_get_current_id(void) {
    return g_current_theme;
}

// (#285) Resolve a userland theme_color_id_t to the active theme's actual
// color from the single-source-of-truth theme_t table. theme_id < 0 = current.
// Color ids MUST match userland libc theme.h theme_color_id_t enum order.
uint32_t theme_get_color_by_id(int theme_id, int color_id) {
    const theme_t *t = (theme_id < 0) ? theme_get_current()
                                      : theme_get_by_id(theme_id);
    if (!t) t = theme_get_current();
    switch (color_id) {
        case 0:  return t->window_bg;            // BACKGROUND
        case 1:  return t->label_text;           // FOREGROUND
        case 2:  return t->selection_bg;         // ACCENT
        case 3:  return t->selection_bg;         // SELECTION
        case 4:  return t->selection_text;       // SELECTION_TEXT
        case 5:  return t->titlebar_active;      // TITLEBAR_ACTIVE
        case 6:  return t->titlebar_inactive;    // TITLEBAR_INACTIVE
        case 7:  return t->titlebar_text;        // TITLEBAR_TEXT
        case 8:  return t->window_bg;            // WINDOW_BG
        case 9:  return t->window_border;        // WINDOW_BORDER
        case 10: return t->close_button;         // CLOSE_BUTTON
        case 11: return t->close_button_hover;   // CLOSE_BUTTON_HOVER
        case 12: return t->minimize_button;      // MINIMIZE_BUTTON
        case 13: return t->maximize_button;      // MAXIMIZE_BUTTON
        case 14: return t->button_bg;            // BUTTON_FACE
        case 15: return t->button_bg_hover;      // BUTTON_LIGHT
        case 16: return t->button_border;        // BUTTON_SHADOW
        case 17: return t->button_border;        // BUTTON_DARK
        case 18: return t->button_text;          // BUTTON_TEXT
        case 19: return t->button_disabled;      // BUTTON_DISABLED
        case 20: return t->label_text;           // LABEL_TEXT
        case 21: return t->textbox_bg;           // TEXTBOX_BG
        case 22: return t->textbox_border;       // TEXTBOX_BORDER
        case 23: return t->textbox_text;         // TEXTBOX_TEXT
        case 24: return t->textbox_cursor;       // TEXTBOX_CURSOR
        case 25: return t->checkbox_bg;          // CHECKBOX_BG
        case 26: return t->checkbox_border;      // CHECKBOX_BORDER
        case 27: return t->checkbox_check;       // CHECKBOX_CHECK
        case 28: return t->desktop_bg;           // DESKTOP_BG
        case 29: return t->taskbar_bg;           // TASKBAR_BG
        case 30: return t->taskbar_hover;        // TASKBAR_HOVER
        case 31: return t->taskbar_active;       // TASKBAR_ACTIVE
        case 32: return t->start_button;         // START_BUTTON
        case 33: return t->gauge_bg;             // GAUGE_BG
        case 34: return t->gauge_fg;             // GAUGE_FG
        case 35: return t->menu_bg;              // MENU_BG
        case 36: return t->menu_border;          // MENU_BORDER
        case 37: return t->menu_item_hover;      // MENU_ITEM_HOVER
        case 38: return t->menu_text;            // MENU_TEXT
        case 39: return t->menu_text_disabled;   // MENU_TEXT_DISABLED
        case 40: return t->menu_separator;       // MENU_SEPARATOR
        case 41: return t->scrollbar_bg;         // SCROLLBAR_BG
        case 42: return t->scrollbar_thumb;      // SCROLLBAR_THUMB
        case 43: return t->scrollbar_thumb_hover;// SCROLLBAR_THUMB_HOVER
        case 44: return t->window_bg;            // TAB_BG
        case 45: return t->selection_bg;         // TAB_ACTIVE
        case 46: return t->window_border;        // TAB_BORDER
        case 47: return t->tooltip_bg;           // TOOLTIP_BG
        case 48: return t->tooltip_border;       // TOOLTIP_BORDER
        case 49: return t->tooltip_text;         // TOOLTIP_TEXT
        case 50: return t->gauge_bg;             // PROGRESS_BG
        case 51: return t->gauge_fg;             // PROGRESS_FG
        default: return t->window_bg;
    }
}


// Set the active theme by ID
void theme_set(int theme_id) {
    if (theme_id >= 0 && theme_id < g_theme_count) {
        g_current_theme = theme_id;
        kprintf("[Themes] Theme changed to: %s (id=%d)\n", g_themes[theme_id].name, theme_id);

        // Notify font system of theme change
        theme_notify_font_system();
    } else {
        kprintf("[Themes] Invalid theme ID: %d\n", theme_id);
    }
}


// Get theme name by ID
const char *theme_get_name(int theme_id) {
    if (theme_id >= 0 && theme_id < g_theme_count) {
        return g_themes[theme_id].name;
    }
    return "Unknown";
}

// Notify the font system when theme changes
// This updates the font rendering mode based on theme style
void theme_notify_font_system(void) {
    const theme_t *theme = theme_get_current();

    if (theme->style == THEME_STYLE_RETRO) {
        font_set_render_mode(FONT_RENDER_BITMAP);
    } else if (theme->style == THEME_STYLE_MODERN) {
        font_set_render_mode(FONT_RENDER_ANTIALIAS);
    } else {
        // Mixed style - use bitmap for crisp rendering
        font_set_render_mode(FONT_RENDER_BITMAP);
    }
}
