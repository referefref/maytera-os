// theme.h - Retro UNIX Color Theme for MayteraOS
// Inspired by CDE, NeXTSTEP, and Sun OpenWindows
#ifndef RETRO_UNIX_THEME_H
#define RETRO_UNIX_THEME_H

// ============================================================================
// Retro UNIX Theme - CDE/Motif Style Colors
// ============================================================================

// All colors are 0x00RRGGBB format

// Window titlebar
#define RETRO_TITLEBAR_ACTIVE       0x5A6A8A    // CDE blue-gray active
#define RETRO_TITLEBAR_INACTIVE     0x9EA8B8    // CDE light gray inactive
#define RETRO_TITLEBAR_TEXT         0xFFFFFF    // White text

// Window body
#define RETRO_WINDOW_BG             0xD4D0C8    // Classic Windows gray
#define RETRO_WINDOW_BORDER         0x404040    // Dark border

// Window control buttons
#define RETRO_CLOSE_BUTTON          0xC84848    // Red close
#define RETRO_CLOSE_BUTTON_HOVER    0xE06060    // Lighter red on hover
#define RETRO_MINIMIZE_BUTTON       0xD4D0C8    // Gray
#define RETRO_MAXIMIZE_BUTTON       0xD4D0C8    // Gray

// Button widgets
#define RETRO_BUTTON_BG             0xD4D0C8    // Standard gray
#define RETRO_BUTTON_BG_HOVER       0xE0E0E0    // Lighter on hover
#define RETRO_BUTTON_BG_PRESSED     0xB0B0B0    // Darker when pressed
#define RETRO_BUTTON_BORDER         0x808080    // Medium gray border
#define RETRO_BUTTON_TEXT           0x000000    // Black text
#define RETRO_BUTTON_DISABLED       0xA0A0A0    // Grayed out

// 3D beveled edge colors (Motif style)
#define RETRO_BEVEL_LIGHT           0xFFFFFF    // Top/left edge highlight
#define RETRO_BEVEL_DARK            0x404040    // Bottom/right edge shadow

// Text widgets
#define RETRO_LABEL_TEXT            0x000000    // Black labels
#define RETRO_TEXTBOX_BG            0xFFFFFF    // White text input
#define RETRO_TEXTBOX_BORDER        0x808080    // Gray border
#define RETRO_TEXTBOX_TEXT          0x000000    // Black text
#define RETRO_TEXTBOX_CURSOR        0x000000    // Black cursor

// Checkbox
#define RETRO_CHECKBOX_BG           0xFFFFFF    // White background
#define RETRO_CHECKBOX_BORDER       0x808080    // Gray border
#define RETRO_CHECKBOX_CHECK        0x000000    // Black checkmark

// Desktop
#define RETRO_DESKTOP_BG            0x5F7D8E    // CDE teal-blue

// Taskbar/Dock
#define RETRO_TASKBAR_BG            0xD4D0C8    // Standard gray
#define RETRO_TASKBAR_HOVER         0xC0C0C0    // Slightly darker
#define RETRO_TASKBAR_ACTIVE        0xA0A0A0    // Active/pressed
#define RETRO_START_BUTTON          0x5A6A8A    // Same as titlebar

// Resource gauges
#define RETRO_GAUGE_BG              0x303030    // Dark background
#define RETRO_GAUGE_FG              0x60A060    // Green fill
#define RETRO_GAUGE_CPU             0x60A060    // Green for CPU
#define RETRO_GAUGE_MEM             0x6080C0    // Blue for memory
#define RETRO_GAUGE_DISK            0xC0A060    // Orange for disk
#define RETRO_GAUGE_NET             0xA060C0    // Purple for network

// Menu
#define RETRO_MENU_BG               0xD4D0C8    // Gray background
#define RETRO_MENU_BORDER           0x404040    // Dark border
#define RETRO_MENU_ITEM_HOVER       0x5A6A8A    // Blue highlight
#define RETRO_MENU_TEXT             0x000000    // Black text
#define RETRO_MENU_TEXT_DISABLED    0x808080    // Gray disabled
#define RETRO_MENU_SEPARATOR        0x808080    // Gray line

// Scrollbar
#define RETRO_SCROLLBAR_BG          0xD4D0C8    // Gray track
#define RETRO_SCROLLBAR_THUMB       0xB0B0B0    // Slightly darker thumb
#define RETRO_SCROLLBAR_THUMB_HOVER 0xA0A0A0    // Even darker on hover

// Selection
#define RETRO_SELECTION_BG          0x5A6A8A    // Blue selection
#define RETRO_SELECTION_TEXT        0xFFFFFF    // White text

// Terminal colors (VT220 inspired)
#define RETRO_TERM_BG               0x000000    // Black background
#define RETRO_TERM_FG               0x00FF00    // Green text (phosphor)
#define RETRO_TERM_FG_ALT           0xFFCC00    // Amber alternative
#define RETRO_TERM_CURSOR           0x00FF00    // Green cursor

// Icon default color
#define RETRO_ICON_COLOR            0xFFFFFF    // White (on dark backgrounds)
#define RETRO_ICON_SHADOW           0x404040    // Dark shadow

#endif // RETRO_UNIX_THEME_H
