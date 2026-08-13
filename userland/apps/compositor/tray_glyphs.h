// tray_glyphs.h - #745 second follow-up: real-artwork tray glyph coverage
// masks, embedded as data rather than shipped as files (see taskbar.c for
// why: no asset-pipeline entanglement, no filename to get wrong). Every
// table here is an 8-bit alpha/coverage mask, row-major, tinted at draw time
// via tray_blit_mask() -> draw_hspan_alpha(). Provenance for each source is
// documented at its call site in taskbar.c; summarized here:
//   SPEAKER_MASK - Wikimedia Commons File:Speaker_Icon.svg, public domain
//                  (author Mobius, 2006).
//   BT_MASK      - Wikimedia Commons File:Bluetooth.svg, rune path only
//                  (public domain geometry; Bluetooth SIG trademark notice
//                  on the mark itself, used as the conventional OS glyph).
//   WIFI_MASK    - Wikimedia Commons File:WiFi_icon.png, CC0 (author
//                  Lycopene579). NOT the user's original Vexels reference
//                  (CC BY-SA 4.0, not used - see taskbar.c).
//   SHEEP_SILH / SHEEP_HEAD - user-supplied
//                  <workspace> sheep.png (40x37 RGBA),
//                  split into silhouette and head/legs/outline material.
#ifndef TRAY_GLYPHS_H
#define TRAY_GLYPHS_H

// Speaker glyph, 18x14, rasterized offline (no runtime scaling) from
// Wikimedia Commons File:Speaker_Icon.svg (public domain, author Mobius,
// 2006). Coverage mask (alpha only); tinted at draw time.
static const uint8_t SPEAKER_MASK[18*14] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 82, 175, 0, 0, 0, 180, 23, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 117, 246, 225, 0, 19, 117, 115, 162, 0, 0, 0,
    0, 0, 3, 34, 34, 34, 155, 241, 255, 225, 29, 28, 210, 62, 225, 25, 0, 0,
    0, 0, 82, 246, 238, 238, 243, 255, 255, 225, 91, 169, 96, 153, 149, 95, 0, 0,
    0, 0, 88, 242, 255, 255, 255, 255, 255, 225, 6, 235, 38, 203, 106, 133, 0, 0,
    0, 0, 88, 242, 255, 255, 255, 255, 255, 225, 0, 237, 32, 209, 100, 138, 0, 0,
    0, 0, 87, 246, 247, 247, 253, 255, 255, 225, 60, 198, 74, 172, 133, 109, 0, 0,
    0, 0, 19, 100, 102, 102, 219, 247, 255, 225, 67, 61, 174, 89, 206, 46, 0, 0,
    0, 0, 0, 0, 0, 0, 13, 194, 246, 225, 0, 24, 193, 75, 198, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 4, 161, 215, 0, 0, 0, 213, 54, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 19, 0, 0, 0, 18, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

// Bluetooth rune, 11x14, rasterized offline from Wikimedia Commons
// File:Bluetooth.svg (background plate dropped, rune path only; the rune
// geometry is simple public-domain shape information per Commons, though
// the Bluetooth word/rune mark itself is a registered trademark of the
// Bluetooth SIG - used here only as the conventional OS status glyph, the
// same way every other OS does).
static const uint8_t BT_MASK[11*14] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 186, 20, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 248, 196, 8, 0, 0, 0,
    0, 0, 33, 118, 0, 219, 118, 166, 0, 0, 0,
    0, 0, 4, 170, 136, 220, 148, 158, 0, 0, 0,
    0, 0, 0, 4, 171, 255, 158, 2, 0, 0, 0,
    0, 0, 0, 2, 157, 255, 133, 0, 0, 0, 0,
    0, 0, 3, 161, 149, 224, 174, 129, 0, 0, 0,
    0, 0, 34, 129, 1, 219, 87, 191, 0, 0, 0,
    0, 0, 0, 0, 0, 240, 214, 23, 0, 0, 0,
    0, 0, 0, 0, 0, 240, 48, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 12, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

// Wifi glyph, 18x14, area-averaged offline from Wikimedia Commons
// File:WiFi_icon.png (96x96, CC0, author Lycopene579). (The user's original
// Vexels reference is CC BY-SA 4.0, attribution+share-alike, entangling for
// this GPLv2 repo, so not used; a second CC0 candidate, File:Wifi_symbol.svg,
// turned out to be a radio-tower emblem, the wrong metaphor for a tray
// status icon, so this bars-and-dot CC0 PNG was used instead.)
static const uint8_t WIFI_MASK[18*14] = {
    0, 0, 0, 0, 34, 125, 188, 228, 249, 249, 228, 188, 125, 34, 0, 0, 0, 0,
    0, 0, 27, 160, 251, 255, 229, 182, 159, 159, 182, 229, 255, 251, 160, 27, 0, 0,
    0, 91, 242, 253, 158, 46, 0, 0, 0, 0, 0, 0, 46, 158, 253, 242, 91, 0,
    122, 255, 215, 54, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 54, 215, 255, 122,
    210, 184, 13, 0, 0, 62, 157, 219, 249, 249, 219, 157, 62, 0, 0, 13, 184, 210,
    3, 0, 0, 10, 166, 255, 251, 197, 161, 161, 197, 251, 255, 166, 10, 0, 0, 3,
    0, 0, 5, 199, 252, 140, 23, 0, 0, 0, 0, 23, 140, 252, 199, 5, 0, 0,
    0, 0, 9, 202, 89, 0, 0, 0, 0, 0, 0, 0, 0, 89, 202, 9, 0, 0,
    0, 0, 0, 0, 0, 0, 61, 184, 243, 243, 184, 61, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 76, 252, 241, 170, 170, 241, 252, 76, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 53, 159, 23, 0, 0, 23, 159, 53, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 195, 195, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 195, 195, 0, 0, 0, 0, 0, 0, 0, 0,
};

// Sheep glyph, 15x14, from the user-supplied <workspace>
// black sheep.png (40x37 RGBA), pre-scaled OFFLINE with area averaging
// (never at runtime). Split into two coverage masks: the whole silhouette
// and just the head/legs/outline material - see tray_draw_sheep() for why.
static const uint8_t SHEEP_SILH[15*14] = {
    0, 0, 0, 0, 43, 198, 255, 255, 255, 227, 128, 85, 0, 0, 0,
    0, 0, 0, 43, 255, 255, 255, 255, 255, 255, 255, 255, 213, 0, 0,
    0, 0, 0, 198, 255, 255, 255, 255, 255, 255, 255, 255, 255, 213, 0,
    0, 0, 57, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 57,
    0, 0, 170, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 128,
    0, 43, 227, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 227,
    43, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    227, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    213, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 170,
    57, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 57,
    0, 128, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 113,
    0, 192, 255, 255, 128, 128, 128, 128, 255, 255, 128, 170, 255, 255, 170,
    0, 43, 227, 85, 0, 0, 0, 0, 0, 0, 0, 0, 170, 213, 28,
};
static const uint8_t SHEEP_HEAD[15*14] = {
    0, 0, 0, 0, 43, 85, 85, 85, 85, 85, 85, 85, 0, 0, 0,
    0, 0, 0, 43, 64, 0, 0, 0, 0, 0, 0, 0, 128, 0, 0,
    0, 0, 0, 170, 85, 0, 0, 0, 0, 0, 0, 0, 0, 128, 0,
    0, 0, 57, 255, 255, 170, 28, 0, 0, 0, 0, 0, 0, 43, 57,
    0, 0, 170, 128, 192, 255, 255, 128, 43, 0, 0, 0, 0, 0, 85,
    0, 43, 227, 113, 128, 255, 255, 255, 142, 0, 0, 0, 0, 0, 85,
    43, 255, 255, 128, 255, 128, 85, 128, 0, 0, 0, 0, 0, 0, 85,
    227, 255, 255, 255, 213, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85,
    255, 255, 255, 255, 128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85,
    213, 255, 255, 213, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85,
    57, 255, 255, 85, 0, 0, 0, 0, 0, 0, 0, 0, 0, 43, 57,
    0, 128, 255, 170, 0, 0, 28, 0, 0, 0, 0, 0, 113, 255, 113,
    0, 192, 255, 255, 128, 128, 128, 128, 128, 128, 128, 170, 255, 255, 170,
    0, 43, 227, 85, 0, 0, 0, 0, 0, 0, 0, 0, 170, 213, 28,
};

#endif // TRAY_GLYPHS_H
