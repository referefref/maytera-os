# Retro UNIX Theme for MayteraOS

**The Default Theme - Nostalgic 1990s UNIX Workstation Aesthetic**

This theme is the signature look of MayteraOS, inspired by the golden age of UNIX
workstations from Sun Microsystems, Silicon Graphics, Hewlett-Packard, Digital
Equipment Corporation, and IBM.

## Design Inspiration

The Retro UNIX theme draws from these classic desktop environments:

### CDE (Common Desktop Environment)
- **Years**: 1993-2012
- **Platforms**: Solaris, HP-UX, AIX, Digital UNIX, UnixWare
- **Signature**: Deep teal titlebars, warm gray UI, front panel
- **Colors**: Teal (#336666), Warm Gray (#B4B4B4)

### Motif (OSF/Motif)
- **Years**: 1989-present
- **Description**: The X11 widget toolkit standard
- **Signature**: 3D beveled widgets, square corners, consistent look
- **Key Feature**: Light top-left edge, dark bottom-right edge on raised widgets

### NeXTSTEP
- **Years**: 1989-1997
- **Creator**: Steve Jobs at NeXT Computer
- **Signature**: Sophisticated gray tones, dock, refined typography
- **Influence**: Later became macOS

### SGI IRIX "Indigo Magic"
- **Years**: 1992-2006
- **Platform**: Silicon Graphics workstations
- **Signature**: Purple/blue accents on gray, advanced 3D graphics

### Sun OpenWindows
- **Years**: 1989-2000
- **Platform**: Sun Microsystems workstations
- **Signature**: Clean design, functional simplicity

## Color Palette

```
Primary Colors:
  Warm Gray (Background):  #B4B4B4
  Deep Teal (Accent):      #336666
  Black (Text):            #000000
  White (Highlights):      #FFFFFF

3D Bevel Colors:
  Light Edge:              #FFFFFF
  Shadow:                  #808080
  Dark Edge:               #404040
  Button Face:             #C8C8C8

Menu Colors:
  Background:              #D4D4D4
  Separator:               #808080
```

## Widget Style

### Motif 3D Beveled Look

The authentic Motif look requires proper bevel rendering:

**RAISED widgets (buttons, panels, toolbar buttons):**
```
+------------------+
|                  |  <- #FFFFFF (light edge)
|  +-----------+   |
|  |  BUTTON   |   |
|  +-----------+   |
|                  |  <- #404040 (dark edge)
+------------------+
```

**SUNKEN widgets (text fields, pressed buttons):**
```
+------------------+
|                  |  <- #404040 (dark edge)
|  +-----------+   |
|  |  INPUT    |   |
|  +-----------+   |
|                  |  <- #FFFFFF (light edge)
+------------------+
```

### Key Design Rules

1. **Square Corners Only** - No rounded corners; this is classic Motif
2. **2px Bevel Width** - Standard 3D effect depth
3. **High Contrast Text** - Black on light gray or white on dark backgrounds
4. **Consistent Spacing** - 8px grid alignment
5. **No Transparency** - Solid colors throughout
6. **No Shadows** - The 3D bevels provide depth instead

## Window Decorations

```
+--[Title Bar - #336666 active / #808080 inactive]--+
| [_] [#] [X]                      Window Title    |
+--------------------------------------------------+
|                                                  |
|               Window Content                     |
|                  #B4B4B4                         |
|                                                  |
|                                                  |
+--------------------------------------------------+
```

- **Title Bar Height**: 20px
- **Button Size**: 16x16px square
- **Buttons**: Window menu (left), then Min/Max/Close (right)
- **Button Style**: Gray 3D beveled squares with black glyphs

## Taskbar / Front Panel

The CDE-style front panel:

```
+===============================================+
| [Start] | App1 | App2 | App3 |  [Clock] |CPU|
+===============================================+
```

- **Background**: Warm gray (#B4B4B4) with raised 3D edge
- **Buttons**: Raised 3D style, sunken when pressed
- **Position**: Bottom of screen
- **Height**: 36px

## Directory Structure

```
THEMES/retro-unix/
  theme.ini         - Theme configuration
  README.md         - This documentation
  colors/           - Color palette definitions
  cursors/          - X11-style cursors (16x16)
    default.cur     - Arrow pointer
    text.cur        - I-beam for text
    hand.cur        - Link pointer
    wait.cur        - Hourglass
    resize_*.cur    - Resize cursors
    move.cur        - Move cursor
  icons/            - Pixel-art icons
    apps/           - Application icons
    mimetypes/      - File type icons
    places/         - Folder/drive icons
    actions/        - UI action icons
    status/         - Status indicators
  WALLPAPERS/       - Desktop backgrounds
    RETRO.BMP       - Solid teal desktop
    RETROSTIP.BMP   - Stippled pattern
```

## Font Recommendations

For the most authentic look:

1. **UI Font**: Helvetica, Liberation Sans, or similar sans-serif
2. **Monospace**: Terminus, Fixed, or Courier
3. **Size**: 12px for UI, 12px for monospace
4. **Rendering**: Bitmap (no anti-aliasing) for retro look

## Historical Context

The 1990s was the golden age of UNIX workstations. Companies like Sun, SGI, HP,
and DEC produced powerful machines that ran sophisticated graphical interfaces
years before Windows 95 existed.

These workstations were used for:
- Scientific visualization
- Computer-aided design (CAD)
- Film special effects (Jurassic Park, Toy Story)
- Financial trading systems
- Academic research

The warm gray colors and deep teal accents became synonymous with "serious
computing" and professional workstations. This aesthetic still triggers nostalgia
for engineers and developers who used these systems.

## Usage

This is the DEFAULT theme for MayteraOS. It loads automatically on boot.

To switch themes programmatically:
```c
#include "gui/themes.h"

// Set to Retro UNIX (default)
theme_set(THEME_DEFAULT);

// Or by name
theme_set(0);  // Index 0 is always the default theme
```

## Credits

Theme design inspired by:
- The Open Group's CDE specification
- OSF/Motif Style Guide
- Sun Microsystems OpenWindows Design Guidelines
- NeXT Computer Interface Guidelines

---

*"The computer is a bicycle for the mind."* - Steve Jobs

MayteraOS Development Team
