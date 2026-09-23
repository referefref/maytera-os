// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// oicon.h - THE shared MICO .ICN icon loader/drawer for the office toolkit
// (docs/OFFICE_UI_DESIGN.md 8.1).
//
// Files and Settings each carry a private mico_get()+draw_mico(); this is
// written once for the office apps so a third and fourth copy do not appear.
// Its intended home is libc as gui_icon.h (Files/Settings migrate later: a
// file rename, the signatures are already what that header will carry). It
// lives here because this change does not touch libc.
//
// Format (measured from tools/icons/png2icn.py and the two shipped readers):
// 'M','I','C','O' + u32 LE width + u32 LE height, then width*height pixels,
// each a little-endian u32 0xAARRGGBB (on-disk B,G,R,A). Shipped toolbar
// glyphs are 64x64 WHITE-on-transparent; the consumer tints at draw time using
// luminance as coverage (cov = (r*30+g*59+b*11)/100; a = a*cov/255).
//
// Sampling: a source larger than the target is BOX-AVERAGED into it (64->16 is
// a 4x4 box, 64->32 a 2x2, 24->16 an integer-bounded 1..2 box), a source equal
// to the target copies 1:1. This is why the loader caches the COVERAGE MAP at
// the requested size (256 bytes for a 16 px glyph) rather than the 16 KB
// source: 48 slots cost 50 KB of .bss instead of 768 KB.
//
// Header is self-sufficient (raw C types only).
#ifndef OFFICE_OICON_H
#define OFFICE_OICON_H

#define OICON_MAX_SIZE 32     // largest target the cache holds (16 or 32 expected)
#define OICON_SLOTS    48     // the three office families total 63 files; one app uses ~20

// Draw /ICONS/<name>.ICN tinted to `ink` at size x size at (x,y), composited
// over `bg` (no framebuffer read-back exists, so the caller names the ground).
// One win_draw_image() per icon, not one syscall per pixel.
// Returns 1 drawn, 0 missing/unreadable (caller draws its fallback letter).
int  oicon_draw(int win, const char *name, int x, int y, int size,
                unsigned int ink, unsigned int bg);
// 1 if the file loads (at 16 px), 0 otherwise. Cached like oicon_draw.
int  oicon_present(const char *name);
// Drop the cache (theme/scale change, or after an icon file is replaced).
void oicon_flush(void);

#endif // OFFICE_OICON_H
