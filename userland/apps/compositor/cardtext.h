// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// cardtext.h - rotated (sideways) antialiased text for the Cardfile deck.
//
// The Cardfile layout labels every card with a SIDEWAYS tab plate, like the
// printed label on a file-folder divider: the text reads top-to-bottom down
// the plate (CSS writing-mode: vertical-rl). No such primitive exists in the
// compositor's draw.c or in the kernel: SYS_DRAW_TTF/SYS_WIN_DRAW_TTF only
// ever draw a HORIZONTAL run straight into the framebuffer.
//
// It does NOT need one. The kernel already exposes SYS_FONT_GLYPH (309,
// libc/syscall.h font_glyph()), which rasterises one codepoint to an 8-bit
// alpha coverage bitmap plus metrics INTO A USERLAND BUFFER, and the
// compositor already owns direct framebuffer access (g_fb). So the whole
// rotation is a userland compositor-side operation and needs no new syscall:
//
//   1. lay the horizontal run out into a small malloc'd 8-bit alpha buffer,
//      glyph by glyph, using font_glyph()/font_kern()/font_metrics() exactly
//      as the kernel's own ttf_draw_string() would (so widths match
//      text_width_ttf());
//   2. rotate that buffer 90 degrees clockwise (first glyph ends up at the
//      TOP, so the label reads top-to-bottom);
//   3. alpha-blit the rotated strip into g_fb at the tab plate, blending the
//      ink colour by per-pixel coverage and honouring the active clip.
//
// This keeps the draw thread non-blocking (#426): font_glyph() is a bounded
// syscall per glyph, no wait, no poll.

#ifndef COMPOSITOR_CARDTEXT_H
#define COMPOSITOR_CARDTEXT_H

#include <stdint.h>

// Measure the on-plate LENGTH (the tall axis) a vertical label would occupy
// for `text` at logical `size`, in pixels. Equal to the horizontal run width,
// which is what gets rotated onto the tall axis. Matches text_width_ttf().
int cardtext_vertical_len(const char *text, int size);

// The plate THICKNESS (the short axis) for `size`: the font line height
// (ascent - descent) at that size. Callers size the tab plate width from this.
int cardtext_vertical_thickness(int size);

// Draw `text` as a sideways label, reading top-to-bottom.
//   cx     : centre x of the label strip (the strip is `thickness` wide,
//            centred on cx).
//   y_top  : y of the first glyph (top of the label).
//   max_len: hard cap on the tall axis; the run is ellipsis-free hard-clipped
//            to this many pixels (the deck ellipsises upstream, 4.5).
//   size   : logical TTF size (the same units draw_text_ttf() takes).
//   color  : 0xAARRGGBB ink; alpha ignored, coverage supplies the alpha.
// Honours g_clip_*; no-op if the whole strip is outside the clip. Returns the
// tall-axis length actually drawn (<= max_len).
int cardtext_vertical(int32_t cx, int32_t y_top, int max_len,
                      const char *text, int size, uint32_t color);

#endif // COMPOSITOR_CARDTEXT_H
