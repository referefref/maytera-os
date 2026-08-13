// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_theme.h - file-based theme loader for the MayteraOS shared style engine (#565)
//
// Palettes used to be 12 structs compiled into kernel/gui/themes.c. Now every
// theme (the built-ins and anything the App Store installs) is a plain-text
// file: /THEMES/<slug>.mtheme, "key=value" lines (name/author/version/style/
// dark metadata + one line per color field - see docs/THEMES.md). Which one
// is active is a one-line pointer, /CONFIG/THEME.CFG ("active=<slug>").
//
// This header is the single place that: lists the installed themes (from
// /THEMES/INDEX.TXT, the same file the kernel's theme_init() reads at boot,
// so userland and kernel always agree on theme -> index order - the exact
// #517 wallpapers.h pattern, applied here to avoid the same kind of
// index-drift bug), resolves the active slug, and activates a theme (loads
// its file into the kernel's live table via SYS_THEME_LOAD_FILE, then
// set_theme() so the change is visible to every process on its next redraw -
// no polling, no IPC, since theme_color()/theme_color_of() already read the
// kernel's live table directly). Settings and the App Store client both call
// this instead of hand-rolling their own file I/O, so a store-installed
// theme applies exactly the same way a built-in one does.
#ifndef _GUI_THEME_H
#define _GUI_THEME_H

#define GUI_THEME_MAX_ENTRIES  32
#define GUI_THEME_SLUG_MAX     32
#define GUI_THEME_NAME_MAX     40

typedef struct {
    char slug[GUI_THEME_SLUG_MAX];   // filename without ".mtheme", e.g. "ocean"
    char name[GUI_THEME_NAME_MAX];   // display name (the file's "name=" line)
    int  is_dark;                    // the file's "dark=" line (0/1)
    int  is_classic;                 // the file's "style=" line == "retro"
                                      // (beveled CDE/Win95 renderer family vs
                                      // the modern rounded one)
    int  index;                      // position in /THEMES/INDEX.TXT (matches
                                      // the kernel's theme index for this slot)
} gui_theme_entry_t;

// List the themes named in /THEMES/INDEX.TXT, in file order (index 0..N-1,
// matching what theme_init() loaded at boot). Returns the count, or 0 if
// /THEMES/INDEX.TXT is missing/unreadable.
int gui_theme_list(gui_theme_entry_t *out, int max);

// Read /CONFIG/THEME.CFG's "active=" line into slug (cap bytes). Returns 0
// and slug[0]=0 if unset/unreadable (caller should treat that as "index 0").
int gui_theme_get_active_slug(char *slug, int cap);

// Is the ACTIVE theme's widget style classic (beveled CDE/Win95 family)?
// Returns 1 if its file's "style=" line is "retro", 0 for anything else
// (modern/mixed). This reads the active theme FILE, not the kernel theme
// id - the old per-app pattern of `theme_get_active() == 4` / `get_theme()
// == 4` only matched the built-in "Classic" theme's id, so a custom or
// App Store theme with style=retro still rendered the modern widget
// family. Every app that needs to pick beveled-vs-rounded widgets should
// call this instead of comparing the theme id to 4.
//
// Falls back to the old id==4 check only if /CONFIG/THEME.CFG has not
// been written yet (pre-first-boot-write edge case), so behavior in that
// corner case is unchanged.
int gui_theme_is_classic(void);

// Activate a theme file at an explicit path (not necessarily under
// /THEMES/ or named "<slug>.mtheme" - used by the App Store client, which
// knows the exact dest path a package wrote from its own INSTALL manifest).
// index_filename is the bare filename to append to /THEMES/INDEX.TXT (e.g.
// "oceanbreeze.mtheme") so the theme is still found after a reboot, not just
// for the rest of this boot. Returns the applied theme index, or -1 on
// failure. Both gui_theme_activate() below and the App Store install path
// go through this - one place that loads a file, activates it, and persists
// the choice.
int gui_theme_activate_path(const char *mtheme_path, const char *index_filename);

// Make <slug> the active theme: loads /THEMES/<slug>.mtheme into the
// kernel's live table (SYS_THEME_LOAD_FILE - adds it if new, updates it if
// already loaded) and calls set_theme() on the result, then writes
// /CONFIG/THEME.CFG so the choice survives a reboot. Every process that
// queries theme_color()/theme_color_of() (the compositor chrome and ~20
// apps) sees the new palette on its next redraw with no code change needed
// there. Returns the applied theme index, or -1 on failure (bad slug, file
// missing/unparseable).
int gui_theme_activate(const char *slug);

// (#711) LIVE FILE RELOAD. Re-reads the ACTIVE theme file and, if its bytes
// changed since the last call, loads it into the kernel's live table
// (SYS_THEME_LOAD_FILE) and re-activates it - the same gui_theme_activate path
// Settings drives, minus the /CONFIG/THEME.CFG write (nothing was chosen; the
// file simply changed). Returns 1 if a reload happened, 0 otherwise.
//
// The FIRST call only records a baseline and returns 0, so startup never
// double-applies the theme the compositor just activated.
//
// It compares a hash of the file CONTENT, not its mtime: this kernel's ext2
// writer is not relied on to move mtime, and content is the thing that
// actually matters. The file is a couple of KB, so the read is cheap; the
// CALLER owns the throttle (the compositor polls it about every 2s, the
// established throttled-flat-file-re-read cadence in this tree - there is no
// filesystem watcher anywhere in kernel or compositor).
int gui_theme_poll_reload(void);

// Append "<filename>" to /THEMES/INDEX.TXT if not already listed (no-op,
// returns 0, if it is). Used right after installing a new .mtheme file (App
// Store "type=theme" packages) so the new theme is still found after a
// reboot, not just for the rest of this boot.
int gui_theme_index_append(const char *filename);

// (#745) Draw the TOP-RIGHT CORNER of an example window in theme `theme_index`
// (the `index` field of gui_theme_entry_t, i.e. the kernel theme id), at 1:1
// device scale, clipped to w x h. This is the theme PREVIEW, and it is shared
// so the first-boot wizard and the Settings theme picker cannot show the user
// two different pictures of the same theme.
//
// It is a mirror of draw_window() in kernel/gui/window.c, step for step:
// window background, 1px border on the top and right edges, the radius.window
// CHAMFER (a 45 degree cut, never a radius - retro_unix, Classic and High
// Contrast ship 0 and stay square), the titlebar at metric.border_w with
// metric.titlebar_h, the #140 near-white recolour to taskbar_bg, the
// decor.titlebar_gradient stops, and the four right-aligned buttons
// (filter / minimise / maximise / close) at metric.titlebar_btn spaced by
// metric.titlebar_btn_gap. The minimise/maximise glyph ink is win_title_ink()
// of the ACTUAL titlebar fill, NOT color.titlebar_text and NOT
// color.minimize_button/maximize_button: the decorator reads none of those for
// an active window, so drawing them would misrepresent the theme.
//
// `cut_bg` is what shows through the chamfer cut. There is no framebuffer
// readback in this stack, so the caller must hand over the colour actually
// behind the preview rect; passing the wrong one shows as a wrong-coloured
// notch, not as a crash.
//
// No title text is drawn. In a real top-right crop the title is far off to the
// left, so inventing one would be the opposite of a truthful preview.
void gui_theme_win_preview(int handle, int x, int y, int w, int h,
                           int theme_index, unsigned int cut_bg);

#endif // _GUI_THEME_H
