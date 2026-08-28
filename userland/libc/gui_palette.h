// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_palette.h - file-based TERMINAL COLOUR SCHEME loader (#terminal-parity
// tier 2). See docs/TERMINAL_PARITY.md for the full design.
//
// THIS IS DELIBERATELY NOT gui_theme.h. A terminal colour scheme (Homebrew,
// Solarized Dark, Tango, Campbell...) and an OS theme (.mtheme, which paints
// window chrome) are two different concepts that a previous version of the
// terminal conflated under one word, "theme". A colour scheme is the 16 ANSI
// colours plus default foreground/background/cursor/selection - nothing else;
// it never touches title bars, buttons or menus. Keep the naming distinct in
// any UI that surfaces this: "Colour Scheme", never "Theme".
//
// Same data-driven idiom as gui_theme.c: every scheme is a plain-text file,
// /PALETTES/<slug>.tpalette, "key=value" lines, listed in
// /PALETTES/INDEX.TXT (one filename per line, defines display order). Adding
// a scheme needs no rebuild, only a new file plus an INDEX.TXT line - and an
// App Store package could ship one the same way themes already do.
//
// The special slug "system" is NOT a file. It means "do not use a named
// colour scheme at all; keep following the OS theme the way the terminal
// always did before this feature existed" (ANSI 16 = the built-in classic
// table, default fg/bg/cursor = theme_color_of() of the terminal's own
// selected OS theme). gui_palette_list() always prepends it as index -1's
// logical slot; callers that want it in a picker add it themselves (see
// term_prefs_dialog()) so this header stays free of any UI string literal.
#ifndef _GUI_PALETTE_H
#define _GUI_PALETTE_H

#include <stdint.h>

#define GUI_PALETTE_MAX_ENTRIES  32
#define GUI_PALETTE_SLUG_MAX     32
#define GUI_PALETTE_NAME_MAX     40
#define GUI_PALETTE_SYSTEM_SLUG  "system"

typedef struct {
    char slug[GUI_PALETTE_SLUG_MAX];   // filename without ".tpalette"
    char name[GUI_PALETTE_NAME_MAX];   // display name (file's "name=" line)
    int  index;                        // position in /PALETTES/INDEX.TXT
} gui_palette_entry_t;

// A fully-resolved colour scheme: the 16 ANSI slots (standard SGR order:
// black,red,green,yellow,blue,magenta,cyan,white, then the eight bright
// forms) plus the four scheme-level colours no single cell carries.
typedef struct {
    uint32_t ansi[16];
    uint32_t fg;            // default foreground (SGR 39 / no colour set)
    uint32_t bg;             // default background (SGR 49 / no colour set)
    uint32_t cursor;
    uint32_t selection_bg;
    uint32_t selection_fg;
} term_palette_t;

// List the schemes named in /PALETTES/INDEX.TXT, in file order. Returns the
// count, or 0 if /PALETTES/INDEX.TXT is missing/unreadable (caller should
// fall back to "system" only, never crash or blank the terminal).
int gui_palette_list(gui_palette_entry_t *out, int max);

// Fully parse /PALETTES/<slug>.tpalette into *out. Returns 0 on success.
// On any failure (missing file, unreadable, slug == "system") *out is filled
// with the built-in MayteraOS Classic values and -1 is returned, so a caller
// that forgets to check the return value still gets a legible terminal
// instead of uninitialised colours.
int gui_palette_load(const char *slug, term_palette_t *out);

#endif // _GUI_PALETTE_H
