# MayteraOS Desktop Theming System

## Overview

The MayteraOS theming system provides comprehensive desktop customization similar to KDE Plasma or LiteStep. It supports both built-in themes and custom user themes loaded from INI files.

## Features

- **8 Built-in Themes**: Default, Dark Mode, Light Mode, High Contrast, Classic (Win95), Ocean, Sunset, Forest
- **Custom Theme Loading**: Load themes from `/THEMES/<name>/theme.ini` files
- **Complete UI Customization**: Colors for all UI elements
- **Window Decoration Settings**: Titlebar height, border width, button size/style
- **Shadow Effects**: Configurable window shadows
- **Font Styling**: Per-element font customization (system, title, menu, button, mono)
- **Theme Creation/Export**: Create custom themes and save to disk

## Theme Components

### Colors (70+ color settings)

- **Window**: Titlebar (active/inactive), background, border
- **Controls**: Close/minimize/maximize buttons with hover states
- **Widgets**: Buttons, textboxes, checkboxes, labels
- **Desktop**: Background, icon text/shadow
- **Taskbar**: Background, hover, active, clock
- **Menus**: Background, hover, text, separators
- **Selection**: Background and text colors
- **Tooltips**: Background, border, text
- **Scrollbars**: Track, thumb, arrows
- **Progress/Sliders**: Track, fill, thumb
- **Links**: Normal, hover, visited

### Window Decorations

- Titlebar height (pixels)
- Border width (pixels)
- Border radius (for rounded corners)
- Button size and spacing
- Button style (square/round/circular)
- Button position (left/right side)

### Shadow Effects

- Enable/disable shadows
- X/Y offset
- Blur radius
- Spread
- Color and opacity

### Font Styles

- System font (default UI)
- Title font (window titles)
- Menu font
- Button font
- Mono font (terminal/code)
- Small font
- Heading font

## API Reference

### Basic Theme Access

```c
// Initialize the theme system (call once at startup)
void theme_init(void);

// Get current theme
const theme_t *theme_get_current(void);

// Get theme by ID
const theme_t *theme_get_by_id(int theme_id);

// Set active theme
void theme_set(int theme_id);

// Get theme count
int theme_get_count(void);

// Get theme name
const char *theme_get_name(int theme_id);
```

### Color Access

```c
// Get color from current theme
uint32_t theme_get_color(theme_color_id_t id);

// Get color from specific theme
uint32_t theme_get_color_from(int theme_id, theme_color_id_t id);
```

### Font Access

```c
// Get font style from current theme
const theme_font_style_t *theme_get_font(theme_font_id_t id);
```

### Custom Theme Operations

```c
// Load theme from /THEMES/<name>/theme.ini
int theme_load(const char *theme_name);

// Save current theme to file
int theme_save(const char *theme_name);

// Create new custom theme based on existing
int theme_create_custom(const char *name, int base_theme_id);

// Delete custom theme
int theme_delete_custom(const char *name);
```

## Theme INI File Format

Themes are stored in INI format at `/THEMES/<name>/theme.ini`:

```ini
; MayteraOS Theme File
[Theme]
name=My Custom Theme
author=Your Name
version=1.0
dark=0  ; 0 for light themes, 1 for dark themes

[Titlebar]
active=0x3A3A3A
inactive=0x2D2D2D
text=0xE0E0E0
text_inactive=0x909090

[Window]
background=0xF5F5F5
border=0x505050

[Buttons]
close=0x606060
close_hover=0xE04040
minimize=0x707070
maximize=0x707070

[Button]
background=0xD0D0D0
hover=0xE0E0E0
pressed=0xA0A0A0
border=0x606060
text=0x000000

[Textbox]
background=0xFFFFFF
border=0x606060
text=0x000000
cursor=0x000000
selection=0x3399FF

[Desktop]
background=0x2E5A88
icon_text=0xFFFFFF

[Taskbar]
background=0x2D2D2D
hover=0x3D3D3D
active=0x4A4A4A
text=0xE0E0E0

[Menu]
background=0xFFFFFF
border=0x909090
hover=0x3399FF
text=0x000000
separator=0xC0C0C0

[Selection]
background=0x3399FF
text=0xFFFFFF

[Tooltip]
background=0xFFFFD0
border=0x000000
text=0x000000

[Scrollbar]
track=0xE0E0E0
thumb=0x808080
thumb_hover=0x606060

[Shadow]
enabled=1
offset_x=2
offset_y=4
blur=8
color=0x404040
opacity=100

[Decoration]
titlebar_height=24
border_width=1
border_radius=0
button_size=20
button_spacing=4
button_style=0
buttons_left=0
```

## Convenience Macros

For backward compatibility and convenience, macros are provided:

```c
#define THEME_TITLEBAR_ACTIVE       (theme_get_current()->titlebar_active)
#define THEME_WINDOW_BG             (theme_get_current()->window_bg)
#define THEME_BUTTON_BG             (theme_get_current()->button_bg)
// ... etc
```

## Directory Structure

```
/THEMES/
  custom_theme_1/
    theme.ini
    icons/        (optional custom icons)
    cursors/      (optional custom cursors)
  custom_theme_2/
    theme.ini
```

## Built-in Theme IDs

```c
#define THEME_DEFAULT       0   // Modern dark grey
#define THEME_DARK          1   // Full dark mode
#define THEME_LIGHT         2   // Clean light theme
#define THEME_HIGH_CONTRAST 3   // Accessibility theme
#define THEME_CLASSIC       4   // Windows 95/98 style
#define THEME_OCEAN         5   // Blue ocean theme
#define THEME_SUNSET        6   // Warm orange/red theme
#define THEME_FOREST        7   // Green nature theme
```

## Usage Example

```c
// Initialize theme system
theme_init();

// Apply a theme
theme_set(THEME_DARK);

// Use theme colors in rendering
uint32_t bg = theme_get_color(THEME_COLOR_WINDOW_BG);
fb_fill_rect(0, 0, width, height, bg);

// Or use convenience macros
fb_fill_rect(0, 0, width, height, THEME_WINDOW_BG);
```

## Settings Integration

The Settings app provides a "Themes" tab where users can:
- View and select from available themes (built-in and custom)
- See a preview of each theme's colors
- Apply themes with immediate visual feedback

## Future Enhancements

- Custom icon pack loading
- Custom cursor pack loading
- Theme import/export functionality
- More granular font configuration
- Animation settings
- Accent color customization
