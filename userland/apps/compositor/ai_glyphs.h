// ai_glyphs.h - #745: the AI command-launcher glyph (two-star "sparkle" mark),
// embedded as data rather than shipped as a file - same reasoning as
// tray_glyphs.h (see that file's own header comment): no asset-pipeline
// entanglement, no filename to get wrong.
//
// Replaces the taskbar/panel "Maytera-logo" launcher button. The user
// reported the previous glyph (a thin scaled-down wordmark silhouette of the
// full Maytera logo, loaded at runtime from /MAYLOGO.DAT) "too skinny to be
// readable" at button size, and asked for an AI/stars/magic icon instead,
// which also better names the function: this button opens the AI command
// launcher (launcher.c), not a branding mark.
//
// PROVENANCE: original geometric artwork, authored for this exact treatment
// (an 8-vertex "sparkle" star polygon, big + small pair), not sourced from
// Wikimedia Commons or any external asset - no licence attribution is owed.
// <workspace> (already shipped in this repo's
// asset base, unwired per main.c's own comment on its sibling CHATBUB.ICN)
// already uses a similar sparkle-inside-a-bubble motif as this OS's own
// visual language for "AI"; that file is a flat, pre-composited 64x64 RGBA
// colour icon (pink/red gradient square, white bubble, red sparkle) and
// cannot be reused directly here, because it has no alpha-only coverage
// form and would not tint per theme like every other chrome glyph. This is
// a fresh coverage-mask rendition of the same idea (a two-star sparkle,
// no bubble, no colour), built for this button.
//
// Two exact sizes are ever requested (see taskbar.c: 24px by DOCK_DEFAULT's
// taskbar, 20px by DOCK_XFCE's panel) and each is rasterized OFFLINE at
// that EXACT size - never scaled at runtime - following the tray-glyph
// precedent (#745): a glyph this small loses its silhouette to
// nearest-neighbour runtime scaling (see blame.md, "A glyph under about
// 24px cannot be judged at 1:1"). Each is an 8-bit alpha/coverage mask,
// row-major, supersampled 16x and downsampled with area-averaging offline,
// tinted at draw time via tray_blit_mask() -> draw_hspan_alpha(), so it
// reads correctly on all 14 themes exactly like every other chrome glyph.
#ifndef AI_GLYPHS_H
#define AI_GLYPHS_H

static const uint8_t AISTAR_24_MASK[24*24] = {
    0, 0, 0, 0, 0, 0, 0, 1, 0, 28, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 1, 0, 139, 17, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 1, 0, 10, 233, 49, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 3, 0, 52, 255, 98, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 4, 0, 112, 254, 159, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 3, 0, 176, 255, 216, 4, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1, 4, 4, 11, 231, 255, 252, 42, 4, 6, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 3, 4, 1, 0, 0, 0, 52, 255, 250, 255, 99, 0, 0, 0, 0, 4, 4, 2, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 27, 79, 143, 215, 254, 253, 254, 229, 154, 93, 38, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    34, 92, 153, 203, 241, 255, 255, 255, 255, 255, 255, 255, 255, 255, 248, 212, 163, 107, 47, 4, 1, 0, 0, 0,
    51, 131, 202, 245, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 251, 212, 150, 69, 5, 1, 0, 0, 0,
    0, 0, 0, 14, 63, 124, 188, 240, 254, 254, 254, 246, 198, 139, 77, 23, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 4, 2, 0, 0, 0, 0, 74, 255, 249, 255, 119, 0, 0, 0, 0, 1, 4, 4, 0, 0, 0, 0, 0,
    0, 0, 0, 1, 3, 5, 2, 14, 238, 255, 254, 50, 1, 7, 3, 1, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 2, 0, 188, 255, 225, 9, 0, 1, 0, 0, 1, 0, 25, 6, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 4, 0, 123, 254, 169, 0, 3, 0, 0, 1, 6, 3, 132, 40, 3, 3, 0, 0,
    0, 0, 0, 0, 0, 0, 3, 0, 58, 255, 105, 0, 4, 0, 0, 0, 0, 0, 220, 72, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 1, 0, 14, 239, 54, 0, 3, 0, 1, 27, 67, 120, 255, 185, 86, 46, 7, 0,
    0, 0, 0, 0, 0, 0, 0, 1, 0, 156, 21, 0, 1, 1, 3, 70, 169, 239, 254, 253, 203, 116, 20, 1,
    0, 0, 0, 0, 0, 0, 0, 1, 0, 37, 3, 0, 0, 0, 0, 0, 0, 10, 245, 111, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 5, 0, 181, 56, 1, 6, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 2, 1, 75, 20, 0, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static const uint8_t AISTAR_20_MASK[20*20] = {
    0, 0, 0, 0, 0, 1, 0, 22, 19, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 4, 0, 99, 89, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 4, 0, 170, 160, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 1, 1, 222, 214, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 2, 1, 36, 253, 249, 31, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 3, 4, 6, 0, 89, 255, 255, 86, 0, 6, 4, 3, 1, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 21, 170, 254, 254, 170, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    7, 28, 67, 124, 184, 235, 255, 255, 255, 255, 236, 185, 125, 70, 30, 8, 1, 0, 0, 0,
    68, 176, 251, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254, 180, 70, 3, 1, 0, 0,
    0, 0, 25, 76, 140, 203, 250, 254, 254, 250, 203, 141, 77, 28, 0, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 149, 254, 255, 150, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0,
    0, 0, 2, 3, 7, 0, 76, 255, 255, 79, 0, 7, 3, 2, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 2, 0, 21, 245, 249, 26, 0, 2, 0, 2, 3, 65, 9, 2, 0, 0,
    0, 0, 0, 0, 0, 2, 0, 205, 213, 0, 1, 0, 0, 0, 0, 182, 14, 0, 0, 0,
    0, 0, 0, 0, 0, 4, 0, 147, 158, 0, 4, 0, 10, 45, 98, 248, 129, 53, 15, 0,
    0, 0, 0, 0, 0, 3, 0, 76, 86, 0, 4, 1, 24, 116, 199, 255, 215, 132, 38, 1,
    0, 0, 0, 0, 0, 0, 0, 9, 10, 0, 0, 0, 0, 0, 0, 219, 33, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 1, 119, 15, 3, 2, 0,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 13, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

#endif // AI_GLYPHS_H
