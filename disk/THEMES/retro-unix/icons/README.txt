MayteraOS Retro UNIX Icon Theme
================================

This theme is inspired by classic UNIX desktop environments:
- CDE (Common Desktop Environment)
- NeXTSTEP
- Early Sun OpenWindows
- Motif

Design Principles:
- 24x24 and 32x32 primary sizes
- 1-bit monochrome (can be colorized at runtime)
- Clear silhouettes visible at small sizes
- Pixel-perfect edges, no anti-aliasing
- Functional over decorative
- High contrast for readability

Icon Categories:
- apps/     - Application icons
- mimetypes/- File type icons
- places/   - Folder, drive, network icons
- actions/  - UI action icons (close, minimize, arrows)
- status/   - Status indicators

Technical Format:
- C header files with uint8_t arrays
- 3 bytes per row (24 bits packed)
- Bit 7 of byte 0 = leftmost pixel

Colors are applied at render time using icon_draw_scaled().
