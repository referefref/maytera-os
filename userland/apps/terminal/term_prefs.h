// term_prefs.h
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.
#ifndef TERM_PREFS_H
#define TERM_PREFS_H

#include "term_common.h"


// ---------------------------------------------------------------------------
// #241: Terminal theme / font / text-size preferences.
//
// WHY A TERMINAL-LOCAL FILE, NOT SETTINGS.CFG (docs/UI_STYLE_GUIDE.md,
// userland/libc/settingscfg.h):
// settingscfg.h is the reader for keys OTHER processes need from the
// Settings app's OWN persisted state (clock format, double-click speed -
// things Settings draws a control for). These three preferences are not
// state Settings has any control for; inventing SETTINGS_ITEMS[] rows in a
// completely different app just to reuse one file would be routing through
// someone else's config for no reason, and would make pref-reader-lint
// (repo-guard check 14, scoped to userland/apps/settings/main.c's
// SETTINGS_ITEMS[]) responsible for a key it does not own. This mirrors
// exactly how userland/apps/editor/main.c owns /CONFIG/EDFONT.CFG (#351)
// instead of routing its font choice through Settings: "the editor
// remembers its own font, independent of the system UI font", the same
// argument applies to the terminal's theme, font and size.
//
// WHY THE TERMINAL HAS ITS OWN THEME, NOT get_theme() (the system theme):
// Calculator and Editor both follow the live system theme via get_theme()/
// theme_color() - correct for them, because they have no default-appearance
// requirement of their own. This ticket's requirement is explicit and
// stronger: "default as dark mode" on a virgin image. The system's own
// default theme is THEME_DEFAULT (0, Retro UNIX - a light CDE look, see
// kernel/gui/themes.h), so a terminal that merely followed the system theme
// would open in Retro UNIX on a virgin image, not dark, whenever the system
// default is left untouched. An INDEPENDENT terminal theme, defaulting to
// the built-in "Dark" theme regardless of the system's own default, is the
// only way to satisfy "default as dark mode" without also changing what
// every OTHER app defaults to. gui_theme_list()/theme_color_of() (both
// already shared) are exactly what makes this possible: the terminal chooses
// its own THEME ID (this "Window Theme") for window chrome and, when no
// named COLOUR SCHEME is selected (the "Follow system theme" default, tier 2
// below), for the default fg/bg/cursor too, instead of the one SYS_GET_THEME
// reports for the rest of the desktop.
//
// CORRECTED (docs/TERMINAL_PARITY.md, tier 2): this comment used to end
// "...the terminal's ANSI 16 colors and default fg/bg/cursor all still
// resolve through the same 14-theme system", which was true and was ALSO the
// exact conflation the owner flagged: a "theme" in the sense above (window
// chrome, g_term_theme_slug) and a terminal COLOUR SCHEME in the Terminal.app/
// Konsole sense (the 16 ANSI colors + default fg/bg/cursor/selection, e.g.
// Solarized Dark or Tango) are two different things. They are now two
// different pieces of state: g_term_theme_slug (this section, "Window
// Theme") and g_term_palette_slug (below, "Colour Scheme"). See
// userland/libc/gui_palette.h.
#define TERM_PREF_CFG   "/CONFIG/TERMPREF.CFG"
#define TERM_DEFAULT_THEME_SLUG   "dark"
#define TERM_DEFAULT_FONT_FAMILY  "DejaVu Sans Mono"
#define TERM_DEFAULT_FONT_STYLE   "Regular"
#define TERM_DEFAULT_FONT_SIZE    14

void term_prefs_save(void);
void term_prefs_load(void);
// Throttled (~1/s) content-hash poll of TERMPREF.CFG. Returns 1 if the grid
// was reflowed, in which case the caller must re-issue TIOCSWINSZ if it owns a
// pty (#227: SIGWINCH must keep reaching the child) and redraw.
int  term_prefs_poll_reload(void);
// Modal. Blocks until OK/Cancel/close. Returns 1 if the selection changed and
// was applied + persisted. F9 opens it, including while a foreground TUI owns
// the pty.
int  term_prefs_dialog(void);

// The terminal.* tool contract (#241/#233), for `terminal --contract ...`.
extern const ct_contract_t TERMINAL_CONTRACT;

#endif // TERM_PREFS_H
