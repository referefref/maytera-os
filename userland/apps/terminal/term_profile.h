// term_profile.h - named terminal PROFILES (Konsole's organising idea).
//
// A PROFILE is one named set of everything that decides what a terminal window
// looks like and starts as: colour scheme, window theme, font, cursor,
// scrollback depth, starting directory and a command to run. Create, edit,
// duplicate, delete, and pick which one new windows open with.
//
// ---------------------------------------------------------------------------
// TWO FILES, AND WHICH ONE IS THE TRUTH
// ---------------------------------------------------------------------------
// /CONFIG/TERMPROF.CFG (this file's store) is the SOURCE OF TRUTH. It holds
// every profile and which one is the default.
//
// /CONFIG/TERMPREF.CFG (term_prefs.c) is a DERIVED MIRROR of the live state of
// an open window. It is kept because two shipped mechanisms read it and must
// keep working: the `terminal --contract set ...` tool contract (#241/#233)
// and term_prefs_poll_reload()'s ~1/s live-reload, which is how a change made
// in one process reaches an already-open window. Anything that writes live
// state writes BOTH, so the mirror can never drift into being a second,
// disagreeing source of truth.
//
// A terminal STARTS from the DEFAULT PROFILE, not from the mirror. That is the
// whole point of "set as default": if startup read the mirror instead, marking
// a profile default would change nothing on the next launch and would be
// exactly the kind of dead control this app is not allowed to ship. The mirror
// is still read at startup, but only to seed the live-reload hash baseline, so
// opening a second window does not make the first one spuriously reload.
//
// ---------------------------------------------------------------------------
// COLOUR SCHEME vs WINDOW THEME: still two things, now two FIELDS
// ---------------------------------------------------------------------------
// `palette` is the TERMINAL COLOUR SCHEME. It paints the CELL GRID: the 16
// ANSI colours and the default fg/bg/cursor. `theme` is the WINDOW THEME. It
// paints the chrome THE TERMINAL DRAWS ITSELF: the tab strip and pane headers
// (term_layout.c tl_c()) and the selection (term_select.c). MEASURED on golden
// 2049: it does NOT repaint the OS title bar or frame. window_t::theme_override
// exists per window but no syscall exposes it, so an app cannot ask the
// decorator to draw its frame in a chosen theme. The owner corrected an
// earlier version of this product for conflating them. They are separate
// fields here, they are separate lists in the dialog, and each list has its
// own preview tile so the difference is visible without reading a label.
// Never collapse one into the other.
//
// ---------------------------------------------------------------------------
// FILE FORMAT (/CONFIG/TERMPROF.CFG)
// ---------------------------------------------------------------------------
// Plain "key=value" lines, one profile per [Name] section, same flat-text
// idiom as /PALETTES/*.tpalette and the .mtheme files - no new parser style,
// and hand-editable:
//
//   default=Solarized work
//   [Default]
//   palette=system
//   theme=dark
//   font=DejaVu Sans Mono
//   style=Regular
//   size=14
//   cursor=block
//   blink=1
//   scrollback=2000
//   dir=
//   cmd=
//
// Unknown keys are ignored and missing keys keep the compiled default, so a
// file written by a later build still loads here.
#ifndef TERM_PROFILE_H
#define TERM_PROFILE_H

#include "term_common.h"
#include "term_theme.h"   // TERM_CURSOR_* and the live appearance globals

#define TERM_PROFILE_CFG        "/CONFIG/TERMPROF.CFG"
// 12 is a bounded, .bss-friendly cap (~5 KB): user.ld links this app as a
// single RWX PT_LOAD and a large .bss breaks that loader (see blame.md), so
// the table is deliberately fixed rather than grown on the heap.
#define TERM_PROFILE_MAX        12
#define TERM_PROFILE_NAME_MAX   32
#define TERM_PROFILE_DIR_MAX    96
#define TERM_PROFILE_CMD_MAX    96
#define TERM_PROFILE_DEFAULT_NAME "Default"

// Scrollback stops offered by the slider. Discrete, not a free integer,
// because the ring is a single contiguous allocation of
// lines * TERM_MAX_COLS * sizeof(term_cell_t) and the user is entitled to know
// what they are asking for. term_cell_t is SIXTEEN bytes since the emulation
// core landed (a 32-bit codepoint plus 32-bit tagged fg and bg plus attrs), so
// a 170-column line costs 2720 bytes and the top stop, 10000 lines, is 27 MB.
// That is affordable (the userland heap ceiling is 512 MB and the ring is
// demand-paged, so untouched lines cost no physical memory) but it is not
// something a slider should let a drag land on silently: the dialog prints the
// byte cost next to the value, computed from sizeof so it cannot go stale the
// way this comment nearly did.
#define TERM_SB_STOPS 5
extern const int term_sb_stops[TERM_SB_STOPS];

typedef struct {
    char name[TERM_PROFILE_NAME_MAX];
    char palette[GUI_PALETTE_SLUG_MAX];    // COLOUR SCHEME: the CELL GRID
    char theme[GUI_THEME_SLUG_MAX];        // WINDOW THEME: the CHROME
    char font_family[GUI_FONT_NAME_MAX];
    char font_style[GUI_FONT_STYLE_MAX];
    int  font_size;
    int  cursor_shape;                     // TERM_CURSOR_* (term_theme.h)
    int  cursor_blink;                     // 1 = blink, 0 = solid
    int  scrollback;                       // lines; one of term_sb_stops[]
    char start_dir[TERM_PROFILE_DIR_MAX];  // "" = the session user's home
    char start_cmd[TERM_PROFILE_CMD_MAX];  // "" = none
} term_profile_t;

extern term_profile_t g_term_profiles[TERM_PROFILE_MAX];
extern int g_term_profile_count;
extern int g_term_profile_default;   // index into g_term_profiles, always valid
extern int g_term_profile_active;    // profile THIS window is running

// Fill *p with the compiled-in defaults (dark chrome, "Follow system theme"
// cell grid, DejaVu Sans Mono 14, blinking block cursor, 2000 lines, no
// starting directory or command). This is the ONE definition of "a new
// terminal's settings"; a tab or split that needs a starting point must call
// this or copy g_term_profiles[g_term_profile_default], never re-list the
// values itself.
void term_profile_defaults(term_profile_t *p);

// Read /CONFIG/TERMPROF.CFG, or CREATE it seeded from the current live state
// when it does not exist yet (so an upgrade from a build that predates
// profiles keeps the settings that build had saved, as a profile called
// "Default"). Never leaves the table empty: there is always at least one
// profile and g_term_profile_default always indexes a real one.
void term_profiles_load(void);

// Write the whole table back. Returns 0 on success, -1 if the write failed
// (checked, never discarded - a preference that silently fails to save is a
// bug users report as "it didn't save", #743).
int  term_profiles_save(void);

// Live state <-> profile.
void term_profile_capture(term_profile_t *p);       // live appearance -> *p
void term_profile_apply(const term_profile_t *p);   // *p -> live appearance

// Table edits. Each returns the new selection index, or -1 if refused
// (table full for new/duplicate; last remaining profile for delete).
int  term_profile_new(const char *name, const term_profile_t *seed);
int  term_profile_delete(int idx);

// Startup wiring, called from main() exactly once.
//   term_profile_startup(): apply the DEFAULT profile to live state and, if it
//   names a starting directory that exists, chdir there and copy it into
//   cwd_buf (main.c's shell cwd). Safe to call with a NULL buffer.
void term_profile_startup(char *cwd_buf, int cap);
// The active profile's start command, or NULL/"" when it has none. main.c runs
// it once, after the first prompt, through the normal shell path so it shows
// up in history and in the scrollback exactly like a typed command.
const char *term_profile_start_cmd(void);

#endif // TERM_PROFILE_H
