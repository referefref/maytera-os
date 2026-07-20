Retro Unix Cursor Set for MayteraOS
====================================

This cursor set provides classic X11/Windows 3.1 style cursors.

Features:
- 16x16 pixel dimensions
- 1-bit black and white with transparency mask
- Sharp, pixel-perfect edges
- Classic pointer designs

Cursor Types:
- default.cur    - Standard arrow pointer (hotspot 1,1)
- text.cur       - I-beam for text selection (hotspot 3,7)
- hand.cur       - Hand pointer for links (hotspot 5,1)
- wait.cur       - Hourglass for loading (hotspot 7,7)
- resize_ns.cur  - North-South resize arrow
- resize_ew.cur  - East-West resize arrow
- resize_nesw.cur - NE-SW diagonal resize
- resize_nwse.cur - NW-SE diagonal resize
- move.cur       - Four-way move arrow
- crosshair.cur  - Precision crosshair
- forbidden.cur  - Not-allowed circle
- help.cur       - Arrow with question mark

File Format:
Each .cur file is a plain text file with:
- Line 1: width height hotspot_x hotspot_y
- Following lines: bitmap rows using . (transparent) and X (foreground)
- Mask is generated automatically (any X pixel and its neighbors are visible)

Example (arrow):
16 16 1 1
X...............
XX..............
XXX.............
XXXX............
...
