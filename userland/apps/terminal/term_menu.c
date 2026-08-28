// term_menu.c - the Terminal's menu bar. Read term_menu.h first; the rule this
// file is built around (no dead entries) and the #220 geometry argument both
// live there.

#include "term_common.h"
#include "term_util.h"
#include "term_grid.h"
#include "term_scrollback.h"
#include "term_parse.h"
#include "term_render.h"
#include "term_theme.h"
#include "term_prefs.h"
#include "term_profile.h"
#include "term_pty.h"
#include "term_shell.h"
#include "term_select.h"
#include "term_notify.h"
#include "term_layout.h"
#include "term_search.h"
#include "term_menu.h"
#include "../../libc/gui_menu.h"
#include "../../libc/spawn.h"

// ===========================================================================
// Action ids. Ranges, not one flat enum, because three of the lists are built
// at runtime from state on disk (the colour schemes in /PALETTES, the profiles
// in TERMPROF.CFG, the bookmarks in TERMBOOK.CFG) and their ids have to be
// "base + index".
// ===========================================================================
#define MID_NEW_TAB          1
#define MID_NEW_WINDOW       2
#define MID_CLOSE_TAB        3
#define MID_CLOSE_WINDOW     4
#define MID_COPY             5
#define MID_PASTE            6
#define MID_SELECT_ALL       7
#define MID_CLEAR_SB         8
#define MID_RESET            9
#define MID_PREFS           10
#define MID_SPLIT_RIGHT     11
#define MID_SPLIT_DOWN      12
#define MID_CLOSE_SPLIT     13
#define MID_MAXIMIZE_PANE   14
#define MID_PANE_TO_TAB     15
#define MID_ZOOM_IN         16
#define MID_ZOOM_OUT        17
#define MID_ZOOM_RESET      18
#define MID_SHOW_MENUBAR    19
#define MID_MON_FINISH      20
#define MID_MON_ACTIVITY    21
#define MID_MON_SILENCE     22
#define MID_MON_BELL        23
#define MID_BM_ADD          24
#define MID_BM_REMOVE       25
#define MID_HELP_COMMANDS   26
#define MID_HELP_KEYS       27
#define MID_HELP_ABOUT      28
#define MID_FIND            29
#define MID_FIND_NEXT       30
#define MID_FIND_PREV       31
#define MID_SCHEME_BASE    200   // + index into g_schemes[]
#define MID_PROFILE_BASE   300   // + index into g_term_profiles[]
#define MID_BOOKMARK_BASE  400   // + index into g_bookmarks[]

// ===========================================================================
// State
// ===========================================================================
int term_menu_quit = 0;

static gui_menu_bar_t g_bar;
static int g_menu_visible = 1;   // Settings > Show Menu Bar
static int g_pty_active   = 0;   // a foreground child owns the focused pane
static int g_inited       = 0;

#define TM_MAX_SCHEMES  16
static gui_palette_entry_t g_schemes[TM_MAX_SCHEMES];
static int g_scheme_count = 0;

// --- bookmarks -------------------------------------------------------------
// New state (the spec flags it as 100% new). One absolute path per line in a
// per-user config file, exactly the shape TERMPREF.CFG already uses, so it
// inherits userconf's "read falls back to the legacy /CONFIG copy, writes go
// to the user's own home" rule for free.
#define TM_BOOKMARK_CFG    "TERMBOOK.CFG"
#define TM_BOOKMARK_LEGACY "/CONFIG/TERMBOOK.CFG"
#define TM_MAX_BOOKMARKS   12
#define TM_PATH_MAX        128
static char g_bookmarks[TM_MAX_BOOKMARKS][TM_PATH_MAX];
static int  g_bookmark_count = 0;

// ===========================================================================
// The item tables.
//
// NOT `const`, unlike the Editor's, for two independent reasons that both
// matter:
//   1. `enabled` is LIVE STATE. Rows dim when the command has no meaning right
//      now, which is the difference between an honest transient and the dead
//      control #208 is about.
//   2. Three of the lists (schemes, profiles, bookmarks) are built from state
//      on disk, so their labels are runtime strings.
// gui_menu_t holds `const gui_menu_item_t *`, so pointing it at a mutable
// array is exactly the supported usage; the widget never writes through it.
// ===========================================================================
static gui_menu_item_t FILE_ITEMS[] = {
    { "New Tab",           "Ctrl+Shift+T", MID_NEW_TAB,      true,  false },
    { "New Window",        "Ctrl+Shift+N", MID_NEW_WINDOW,   true,  false },
    { NULL, NULL, 0, false, false },
    { "Close Tab",         "Ctrl+Shift+W", MID_CLOSE_TAB,    true,  false },
    { "Close Window",      NULL,           MID_CLOSE_WINDOW, true,  false },
};
#define FILE_N ((int)(sizeof(FILE_ITEMS)/sizeof(FILE_ITEMS[0])))

static gui_menu_item_t EDIT_ITEMS[] = {
    { "Copy",              "Ctrl+Shift+C", MID_COPY,       true, false },
    { "Paste",             "Ctrl+Shift+V", MID_PASTE,      true, false },
    { "Select All",        "Ctrl+Shift+A", MID_SELECT_ALL, true, false },
    { NULL, NULL, 0, false, false },
    { "Find...",           "Ctrl+Shift+F", MID_FIND,       true, false },
    { "Find Next",         "F3",           MID_FIND_NEXT,  true, false },
    { "Find Previous",     "Shift+F3",     MID_FIND_PREV,  true, false },
    { NULL, NULL, 0, false, false },
    { "Clear Scrollback",  "Ctrl+Shift+K", MID_CLEAR_SB,   true, false },
    { "Reset Terminal",    "Ctrl+Shift+X", MID_RESET,      true, false },
    { NULL, NULL, 0, false, false },
    { "Preferences...",    "F9",           MID_PREFS,      true, false },
};
#define EDIT_N ((int)(sizeof(EDIT_ITEMS)/sizeof(EDIT_ITEMS[0])))

static gui_menu_item_t VIEW_ITEMS[] = {
    { "Split Left/Right",  "Ctrl+Shift+\\",     MID_SPLIT_RIGHT,   true, false },
    { "Split Top/Bottom",  "Ctrl+Shift+-",      MID_SPLIT_DOWN,    true, false },
    { "Close Split",       "Ctrl+Shift+O",      MID_CLOSE_SPLIT,   true, false },
    { NULL, NULL, 0, false, false },
    { "Maximize This Split", "Ctrl+Shift+Enter", MID_MAXIMIZE_PANE, true, false },
    { "Move This Split To A New Tab", NULL,     MID_PANE_TO_TAB,   true, false },
    { NULL, NULL, 0, false, false },
    { "Zoom In",           NULL,                MID_ZOOM_IN,       true, false },
    { "Zoom Out",          NULL,                MID_ZOOM_OUT,      true, false },
    { "Reset Zoom",        NULL,                MID_ZOOM_RESET,    true, false },
};
#define VIEW_N ((int)(sizeof(VIEW_ITEMS)/sizeof(VIEW_ITEMS[0])))

// Bookmarks: two fixed rows, a separator, then up to TM_MAX_BOOKMARKS live
// rows appended by tm_rebuild_bookmark_items(). item_count is adjusted to
// match, so an empty bookmark file shows two rows and no dangling separator.
#define TM_BM_FIXED 3
static gui_menu_item_t BOOKMARK_ITEMS[TM_BM_FIXED + TM_MAX_BOOKMARKS] = {
    { "Add Bookmark (current directory)",    "Ctrl+Shift+D", MID_BM_ADD,    true, false },
    { "Remove Bookmark (current directory)", NULL,           MID_BM_REMOVE, true, false },
    { NULL, NULL, 0, false, false },
};
static int g_bookmark_item_count = 2;   // no separator until there is a list

// Settings: Show Menu Bar, the four notification monitors, the live colour
// scheme radio list, the live profile radio list, and the profile editor (the
// same F9 dialog as Edit > Preferences; Konsole reaches one action from two
// paths too, and so do we, deliberately).
#define TM_SET_FIXED_HEAD 7
static gui_menu_item_t SETTINGS_ITEMS[TM_SET_FIXED_HEAD + 1 + TM_MAX_SCHEMES
                                      + 1 + TERM_PROFILE_MAX + 2] = {
    { "Show Menu Bar",             "F10", MID_SHOW_MENUBAR, true, true },
    { NULL, NULL, 0, false, false },
    { "Notify: Command Finished",  NULL, MID_MON_FINISH,   true, false },
    { "Notify: Activity",          NULL, MID_MON_ACTIVITY, true, false },
    { "Notify: Silence",           NULL, MID_MON_SILENCE,  true, false },
    { "Notify: Bell",              NULL, MID_MON_BELL,     true, false },
    { NULL, NULL, 0, false, false },
};
static int g_settings_item_count = TM_SET_FIXED_HEAD;

static gui_menu_item_t HELP_ITEMS[] = {
    { "Shell Commands",     NULL, MID_HELP_COMMANDS, true, false },
    { "Keyboard Shortcuts", NULL, MID_HELP_KEYS,     true, false },
    { NULL, NULL, 0, false, false },
    { "About Terminal",     NULL, MID_HELP_ABOUT,    true, false },
};
#define HELP_N ((int)(sizeof(HELP_ITEMS)/sizeof(HELP_ITEMS[0])))

static gui_menu_t MENUS[] = {
    { "File",      FILE_ITEMS,     FILE_N },
    { "Edit",      EDIT_ITEMS,     EDIT_N },
    { "View",      VIEW_ITEMS,     VIEW_N },
    { "Bookmarks", BOOKMARK_ITEMS, 2 },
    { "Settings",  SETTINGS_ITEMS, TM_SET_FIXED_HEAD },
    { "Help",      HELP_ITEMS,     HELP_N },
};
#define MENU_COUNT ((int)(sizeof(MENUS)/sizeof(MENUS[0])))
#define MENU_IDX_BOOKMARKS 3
#define MENU_IDX_SETTINGS  4

// Storage for the runtime labels. gui_menu_item_t holds `const char *`, so the
// strings have to outlive the table; static buffers, not malloc, so a tight
// heap cannot silently produce a menu of empty rows.
static char g_scheme_labels[TM_MAX_SCHEMES][GUI_PALETTE_NAME_MAX + 8];
static char g_profile_labels[TERM_PROFILE_MAX][TERM_PROFILE_NAME_MAX + 12];
static char g_bookmark_labels[TM_MAX_BOOKMARKS][TM_PATH_MAX];

// ===========================================================================
// Small helpers
// ===========================================================================

// The WINDOW's content size, which is NOT term_px_w/term_px_h: since tabs and
// splits landed those are the FOCUSED PANE's size. gui_menu needs the window,
// because that is what its popup has to fit inside.
static int tm_win_w(void) { int w = 0, h = 0; term_layout_content_size(&w, &h); return w; }
static int tm_win_h(void) { int w = 0, h = 0; term_layout_content_size(&w, &h); return h; }

// Emit a block of text into the focused pane as if a command had produced it,
// then leave the shell exactly where it was: prompt redrawn, whatever the user
// had half-typed re-echoed and still in input_line. Used by the two Help rows,
// which deliberately write INTO the terminal rather than opening a dialog:
// there is no multi-line notice dialog in libc (gui_confirm_t holds three
// wrapped lines), the output is scrollable and copyable where a dialog is
// neither, and it needs no new modal machinery in a draw path.
static void tm_begin_output(void) {
    for (int i = 0; i < input_pos; i++) { term_putc('\b'); term_putc(' '); term_putc('\b'); }
    term_puts("\r\n");
}
static void tm_end_output(void) {
    print_prompt();
    for (int i = 0; i < input_pos; i++) term_putc(input_line[i]);
}

// Run `cmd` exactly as if the user had typed it and pressed Enter: echoed,
// added to history, executed through the one shell entry point, prompt
// reprinted. No second command path, so a bookmark `cd` gets builtin_cd()'s
// existing validation (it stats the target and refuses a non-directory) rather
// than a private copy of it.
//
// SAFE ONLY AT A PROMPT, and that is enforced rather than trusted:
// execute_command() can reach term_layout_run_foreground(), so calling it
// while a child already owns the pane would be a second foreground command in
// one pane. Every row that reaches this function is dimmed by
// tm_apply_context() whenever g_pty_active is set.
static void tm_run_command(const char *cmd) {
    for (int i = 0; i < input_pos; i++) { term_putc('\b'); term_putc(' '); term_putc('\b'); }
    input_pos = 0; input_line[0] = '\0';
    term_puts(cmd);
    term_puts("\r\n");
    add_to_history(cmd);
    execute_command(cmd);
    if (!term_layout_pane_busy()) print_prompt();
    history_pos = history_count;
}

// ===========================================================================
// Bookmarks: load / save / table rebuild
// ===========================================================================
static void tm_bookmarks_load(void) {
    g_bookmark_count = 0;
    int fd = userconf_open_read(TM_BOOKMARK_CFG, TM_BOOKMARK_LEGACY);
    if (fd < 0) return;
    char buf[TM_MAX_BOOKMARKS * TM_PATH_MAX + 8];
    int n = read(fd, buf, (int)sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    int i = 0;
    while (buf[i] && g_bookmark_count < TM_MAX_BOOKMARKS) {
        int j = 0;
        while (buf[i] && buf[i] != '\n' && buf[i] != '\r') {
            if (j < TM_PATH_MAX - 1) g_bookmarks[g_bookmark_count][j++] = buf[i];
            i++;
        }
        g_bookmarks[g_bookmark_count][j] = '\0';
        while (buf[i] == '\n' || buf[i] == '\r') i++;
        if (j > 0) g_bookmark_count++;
    }
}

static void tm_bookmarks_save(void) {
    char buf[TM_MAX_BOOKMARKS * TM_PATH_MAX + 8];
    int o = 0;
    for (int b = 0; b < g_bookmark_count; b++) {
        for (int k = 0; g_bookmarks[b][k] && o < (int)sizeof(buf) - 2; k++) buf[o++] = g_bookmarks[b][k];
        buf[o++] = '\n';
    }
    buf[o] = '\0';
    int fd = userconf_open_write(TM_BOOKMARK_CFG);
    // #743: a preference that silently fails to save is still a bug users
    // report as "it didn't save". term_prefs.c's g_term_pref_save_failed is
    // written and never read, and that is exactly how nobody noticed that a
    // non-root session cannot write its own TERMPREF.CFG at all (measured,
    // golden 2045: the file does not exist on the image). Report it.
    if (userconf_finish_write(fd, buf, (unsigned long)o) != 0)
        term_puts("\033[31mbookmarks: save failed\033[0m\r\n");
}

// The check mark on a bookmark row means "this is the directory you are in",
// which changes on every `cd`, not only when the list changes. MEASURED on
// golden 2045: bookmark /APPS stayed ticked after `cd /FONTS`, because the
// only thing that recomputed it was the add/remove path. Anything that OPENS
// the menu calls this first; it is a handful of string compares.
static void tm_sync_bookmark_checks(void) {
    for (int b = 0; b < g_bookmark_count; b++)
        BOOKMARK_ITEMS[TM_BM_FIXED + b].checked = str_eq(g_bookmarks[b], cwd);
}

static void tm_rebuild_bookmark_items(void) {
    int n = TM_BM_FIXED - 1;   // the two fixed rows; the separator is index 2
    if (g_bookmark_count > 0) {
        n = TM_BM_FIXED;       // include the separator
        for (int b = 0; b < g_bookmark_count; b++) {
            str_copy(g_bookmark_labels[b], g_bookmarks[b], TM_PATH_MAX);
            BOOKMARK_ITEMS[n].label    = g_bookmark_labels[b];
            BOOKMARK_ITEMS[n].shortcut = NULL;
            BOOKMARK_ITEMS[n].id       = MID_BOOKMARK_BASE + b;
            BOOKMARK_ITEMS[n].enabled  = true;
            BOOKMARK_ITEMS[n].checked  = str_eq(g_bookmarks[b], cwd);
            n++;
        }
    }
    g_bookmark_item_count = n;
    MENUS[MENU_IDX_BOOKMARKS].item_count = n;
}

// ===========================================================================
// Settings: the colour-scheme and profile radio lists
// ===========================================================================
static void tm_rebuild_settings_items(void) {
    // Row 0 of the scheme list is the "Follow system theme" pseudo-scheme.
    // gui_palette.h is explicit that "system" is NOT a file and that callers
    // add it to their own picker, which is what term_prefs_dialog() already
    // does; the same convention, not a second one.
    g_scheme_count = 0;
    str_copy(g_schemes[0].slug, GUI_PALETTE_SYSTEM_SLUG, GUI_PALETTE_SLUG_MAX);
    str_copy(g_schemes[0].name, "Follow System Theme", GUI_PALETTE_NAME_MAX);
    g_schemes[0].index = -1;
    g_scheme_count = 1;
    gui_palette_entry_t ents[GUI_PALETTE_MAX_ENTRIES];
    int n = gui_palette_list(ents, GUI_PALETTE_MAX_ENTRIES);
    for (int i = 0; i < n && g_scheme_count < TM_MAX_SCHEMES; i++)
        g_schemes[g_scheme_count++] = ents[i];

    int r = TM_SET_FIXED_HEAD;
    for (int i = 0; i < g_scheme_count; i++) {
        str_copy(g_scheme_labels[i],
                 g_schemes[i].name[0] ? g_schemes[i].name : g_schemes[i].slug,
                 (int)sizeof(g_scheme_labels[i]));
        SETTINGS_ITEMS[r].label    = g_scheme_labels[i];
        SETTINGS_ITEMS[r].shortcut = NULL;
        SETTINGS_ITEMS[r].id       = MID_SCHEME_BASE + i;
        SETTINGS_ITEMS[r].enabled  = true;
        SETTINGS_ITEMS[r].checked  = str_eq(g_schemes[i].slug, g_term_palette_slug);
        r++;
    }
    SETTINGS_ITEMS[r].label = NULL; SETTINGS_ITEMS[r].shortcut = NULL;
    SETTINGS_ITEMS[r].id = 0; SETTINGS_ITEMS[r].enabled = false;
    SETTINGS_ITEMS[r].checked = false; r++;

    // The profiles term_profile.c actually holds. A profile row APPLIES that
    // profile to this window, which is term_profile_apply() and nothing else.
    for (int i = 0; i < g_term_profile_count && i < TERM_PROFILE_MAX; i++) {
        int o = 0;
        const char *pre = "Profile: ";
        for (int k = 0; pre[k] && o < (int)sizeof(g_profile_labels[i]) - 1; k++)
            g_profile_labels[i][o++] = pre[k];
        for (int k = 0; g_term_profiles[i].name[k] &&
                        o < (int)sizeof(g_profile_labels[i]) - 1; k++)
            g_profile_labels[i][o++] = g_term_profiles[i].name[k];
        g_profile_labels[i][o] = '\0';
        SETTINGS_ITEMS[r].label    = g_profile_labels[i];
        SETTINGS_ITEMS[r].shortcut = NULL;
        SETTINGS_ITEMS[r].id       = MID_PROFILE_BASE + i;
        SETTINGS_ITEMS[r].enabled  = true;
        SETTINGS_ITEMS[r].checked  = (i == g_term_profile_active);
        r++;
    }
    SETTINGS_ITEMS[r].label = NULL; SETTINGS_ITEMS[r].shortcut = NULL;
    SETTINGS_ITEMS[r].id = 0; SETTINGS_ITEMS[r].enabled = false;
    SETTINGS_ITEMS[r].checked = false; r++;

    SETTINGS_ITEMS[r].label    = "Edit Current Profile...";
    SETTINGS_ITEMS[r].shortcut = "F9";
    SETTINGS_ITEMS[r].id       = MID_PREFS;
    SETTINGS_ITEMS[r].enabled  = true;
    SETTINGS_ITEMS[r].checked  = false;
    r++;

    g_settings_item_count = r;
    MENUS[MENU_IDX_SETTINGS].item_count = r;

    SETTINGS_ITEMS[0].checked = g_menu_visible ? true : false;
    // The four monitors are per-tab state owned by term_notify.
    int tab = term_layout_active_tab();
    SETTINGS_ITEMS[2].checked = term_notify_get_monitor(tab, TERM_MON_FINISH)   ? true : false;
    SETTINGS_ITEMS[3].checked = term_notify_get_monitor(tab, TERM_MON_ACTIVITY) ? true : false;
    SETTINGS_ITEMS[4].checked = term_notify_get_monitor(tab, TERM_MON_SILENCE)  ? true : false;
    SETTINGS_ITEMS[5].checked = term_notify_get_monitor(tab, TERM_MON_BELL)     ? true : false;
}

// ===========================================================================
// Colours. The menu bar is CHROME, so it follows the terminal's WINDOW THEME
// (g_term_theme_index / theme_color_of), never the COLOUR SCHEME, which governs
// the cell grid only. Conflating those two is the exact mistake
// docs/TERMINAL_MODULES.md's rule 5 records the owner correcting.
// ===========================================================================
// Which theme the bar's CACHED palette and metrics were built from. -1 = never.
static int g_bar_theme_synced = -1;

static void tm_sync_palette(void) {
    // This function used to BE the token-to-palette mapping, including the
    // gui_ensure_contrast() calls. All of it moved INTO the widget
    // (gui_menu_sync_theme -> gui_menu_palette_theme), because every adopter
    // needs exactly the same mapping and the contrast correction is not
    // optional: modern_light.mtheme ships near-black item_hover_fg on saturated
    // blue. One call now re-reads the colours AND the metrics (type.body,
    // metric.menu_row_h, metric.gap) for the terminal's own window theme, and
    // re-lays the label boxes, since the type size may have moved.
    gui_menu_sync_theme(&g_bar, g_term_theme_index);
    g_bar_theme_synced = g_term_theme_index;
}

// ===========================================================================
// The chrome hook. term_render calls this at the end of EVERY term_redraw()
// and term_layout_redraw_all() calls it too, so the bar and any open popup
// survive a repaint triggered from anywhere: the cursor blink, program output,
// a resize, a pane switch.
// ===========================================================================
static void tm_draw_chrome(void) {
    if (!g_menu_visible) return;

    // A CACHE NEEDS AN INVALIDATION, AND THE DOCUMENTED ONE HAD NO CALLER.
    // gui_menu caches its palette and metrics (each is a syscall and the draw
    // path touches them per label per frame). term_menu_refresh() exists for
    // exactly this, its header says "call after the F9 dialog or a live config
    // reload" - and grepping the whole tree, NOTHING calls it from either.
    // MEASURED: switching the terminal's window theme to Light Mode in the F9
    // dialog relit the tab strip and the pane headers and left the menu bar
    // dark, because the bar was still holding the palette it was built with.
    //
    // g_term_theme_index is written from at least four places (prefs load,
    // profile apply, the `--contract set` path, the TERMPREF.CFG poll-reload),
    // and "remember to call the refresh" has already been shown not to work
    // here. One int compare on the draw path cannot miss a write from any of
    // them. It is a comparison, not a poll loop: nothing waits, nothing spins.
    if (g_bar_theme_synced != g_term_theme_index) tm_sync_palette();

    gui_menu_bar_draw(window_handle, &g_bar);
    // The bar measures only as wide as its labels; fill the rest of the row so
    // the window background does not show through beside "Help".
    int used = g_bar.item_x[MENU_COUNT - 1] + g_bar.item_w[MENU_COUNT - 1];
    int win_w = tm_win_w();
    if (used < win_w)
        win_draw_rect(window_handle, used, 0, win_w - used, TERM_MENU_BAR_H, g_bar.pal.bar_bg);
    gui_menu_popup_draw(window_handle, &g_bar, win_w, tm_win_h());
}

// ===========================================================================
// Enable/disable: the transient half of "no dead entries"
// ===========================================================================
static void tm_apply_context(void) {
    // Rows that act on the SHELL of the focused pane. Meaningless while a
    // child owns it, so they dim and absorb the click; they are back the
    // moment it exits.
    bool shell = !g_pty_active;
    EDIT_ITEMS[8].enabled = shell;   // Clear Scrollback
    EDIT_ITEMS[9].enabled = shell;   // Reset Terminal
    // Find Next / Find Previous only mean something once a search has matches.
    // term_search owns that fact; asking it is what stops two rows that
    // silently do nothing whenever the bar has never been opened.
    {
        bool has = term_search_match_count() > 0;
        EDIT_ITEMS[5].enabled = has;   // Find Next
        EDIT_ITEMS[6].enabled = has;   // Find Previous
    }
    HELP_ITEMS[0].enabled = shell;   // Shell Commands  (runs `help`)
    HELP_ITEMS[1].enabled = shell;   // Keyboard Shortcuts (writes at the prompt)
    HELP_ITEMS[3].enabled = shell;   // About Terminal     (writes at the prompt)
    BOOKMARK_ITEMS[0].enabled = shell;
    BOOKMARK_ITEMS[1].enabled = shell;
    for (int i = TM_BM_FIXED; i < g_bookmark_item_count; i++)
        BOOKMARK_ITEMS[i].enabled = shell;

    // Copy needs something selected. term_select owns that fact; asking it is
    // what stops a Copy row that puts nothing on the clipboard and reports
    // nothing, which is a dead control with extra steps.
    EDIT_ITEMS[0].enabled = term_select_have() ? true : false;

    // The layout rows ASK term_layout_can() rather than re-deriving "is there
    // more than one pane" or "would this split fit". One rule, one owner, so
    // the menu and the pane-header buttons cannot disagree.
    FILE_ITEMS[0].enabled = term_layout_can(TL_CMD_NEW_TAB)      ? true : false;
    FILE_ITEMS[3].enabled = term_layout_can(TL_CMD_CLOSE_TAB)    ? true : false;
    VIEW_ITEMS[0].enabled = term_layout_can(TL_CMD_SPLIT_RIGHT)  ? true : false;
    VIEW_ITEMS[1].enabled = term_layout_can(TL_CMD_SPLIT_DOWN)   ? true : false;
    VIEW_ITEMS[2].enabled = term_layout_can(TL_CMD_CLOSE_SPLIT)  ? true : false;
    VIEW_ITEMS[4].enabled = term_layout_can(TL_CMD_MAXIMIZE_PANE)? true : false;
    VIEW_ITEMS[5].enabled = term_layout_can(TL_CMD_PANE_TO_TAB)  ? true : false;
    // Everything else (New Window, Paste, Select All, zoom, schemes, profiles,
    // prefs, Show Menu Bar, the monitors, Close Window) is correct in every
    // state and stays live.
}

void term_menu_set_pty(int active) {
    g_pty_active = active ? 1 : 0;
    tm_apply_context();
}

// ===========================================================================
// Actions
// ===========================================================================
static void tm_set_font_size(int size) {
    if (size < 8) size = 8;
    if (size > 32) size = 32;
    if (size == g_term_font.size) return;
    g_term_font.size = size;
    term_apply_font();
    term_prefs_save();
    // EVERY pane's grid changes, and every pane's child needs SIGWINCH. That
    // is term_layout's one reflow, not a term_handle_resize() here: with N
    // panes, a private reflow would update one of them (#227).
    term_layout_reflow();
}

static void tm_clear_scrollback(void) {
    sb_count = 0;
    sb_head  = 0;
    term_scrollback_reconfigure();
    gui_scroll_set(&term_scroll_view, gui_scroll_max(&term_scroll_view));
    term_scroll_sync_bottom();
    term_redraw();
}

static void tm_help_shortcuts(void) {
    tm_begin_output();
    term_puts("\033[1mTerminal keyboard shortcuts\033[0m\r\n");
    term_puts("  F9              Preferences (window theme, font, colour scheme)\r\n");
    term_puts("  F10             Show/hide the menu bar\r\n");
    term_puts("  Ctrl+Shift+T    New tab\r\n");
    term_puts("  Ctrl+Shift+W    Close tab\r\n");
    term_puts("  Ctrl+Shift+N    New terminal window\r\n");
    term_puts("  Ctrl+Shift+C    Copy the selection\r\n");
    term_puts("  Ctrl+Shift+V    Paste from the system clipboard\r\n");
    term_puts("  Ctrl+Shift+A    Select all\r\n");
    term_puts("  Ctrl+Shift+F    Find in scrollback  (F3 / Shift+F3 next, previous)\r\n");
    term_puts("  Ctrl+Shift+K    Clear scrollback\r\n");
    term_puts("  Ctrl+Shift+X    Reset terminal\r\n");
    term_puts("  Ctrl+Shift+D    Bookmark the current directory\r\n");
    term_puts("  PageUp/PageDown Scroll back through history\r\n");
    term_puts("  Home/End        Oldest retained line / live screen\r\n");
    term_puts("  Up/Down         Command history\r\n");
    term_puts("  Alt+F E V B S H Open that menu; then Left/Right switch, Up/Down move,\r\n");
    term_puts("                  Enter picks, Esc closes\r\n");
    term_puts("Splits and zoom are on the View menu; they have no accelerator.\r\n");
    tm_end_output();
    term_redraw();
}

static void tm_help_about(void) {
    char line[160];
    tm_begin_output();
    term_puts("\033[1mMayteraOS Terminal\033[0m\r\n");
    term_puts("  ANSI/VT100 emulator with a pty, job control, tabs, splits and scrollback.\r\n");
    snprintf(line, sizeof(line), "  Pane grid     %d cols x %d rows\r\n", term_cols, term_rows);
    term_puts(line);
    snprintf(line, sizeof(line), "  Tabs / panes  %d / %d\r\n",
             term_layout_tab_count(), term_layout_pane_count());
    term_puts(line);
    snprintf(line, sizeof(line), "  Font          %s %s %dpt\r\n",
             g_term_font.family, g_term_font.style, g_term_font.size);
    term_puts(line);
    snprintf(line, sizeof(line), "  Window theme  %s\r\n", g_term_theme_slug);
    term_puts(line);
    snprintf(line, sizeof(line), "  Colour scheme %s\r\n", g_term_palette_slug);
    term_puts(line);
    term_puts("  Licence       MIT (userland). See /APPS and ATTRIBUTION.md.\r\n");
    tm_end_output();
    term_redraw();
}

static void tm_new_window(void) {
    // posix_spawnp, the shared libc wrapper over SYS_SPAWN_ARGS, with the PATH
    // search msh and this terminal's own resolver already use. Not a
    // hand-rolled sys_spawn_args() with a hardcoded "/APPS/TERMINAL", which
    // would break for a per-user install (#745 put <home>/APPS first).
    char *const argv[] = { (char *)"terminal", NULL };
    pid_t pid = 0;
    if (posix_spawnp(&pid, "terminal", NULL, NULL, argv, NULL) != 0) {
        tm_begin_output();
        term_puts("\033[31mNew Window: could not launch terminal\033[0m\r\n");
        tm_end_output();
        term_redraw();
    }
}

static void tm_bookmark_add(void) {
    for (int b = 0; b < g_bookmark_count; b++)
        if (str_eq(g_bookmarks[b], cwd)) return;   // already there; silent no-op
    if (g_bookmark_count >= TM_MAX_BOOKMARKS) {
        tm_begin_output();
        term_puts("\033[33mBookmarks are full (12).\033[0m\r\n");
        tm_end_output();
        term_redraw();
        return;
    }
    str_copy(g_bookmarks[g_bookmark_count], cwd, TM_PATH_MAX);
    g_bookmark_count++;
    tm_bookmarks_save();
    tm_rebuild_bookmark_items();
    tm_apply_context();
    term_redraw();
}

static void tm_bookmark_remove(void) {
    for (int b = 0; b < g_bookmark_count; b++) {
        if (!str_eq(g_bookmarks[b], cwd)) continue;
        for (int k = b; k + 1 < g_bookmark_count; k++)
            str_copy(g_bookmarks[k], g_bookmarks[k + 1], TM_PATH_MAX);
        g_bookmark_count--;
        tm_bookmarks_save();
        tm_rebuild_bookmark_items();
        tm_apply_context();
        term_redraw();
        return;
    }
}

static void tm_set_menu_visible(int on) {
    if (on == g_menu_visible) return;
    g_menu_visible = on ? 1 : 0;
    term_content_y = g_menu_visible ? TERM_MENU_BAR_H : 0;
    if (!g_menu_visible) gui_menu_close(&g_bar);
    gui_menu_bar_init(&g_bar, MENUS, MENU_COUNT, 0, 0, TERM_MENU_BAR_H);
    tm_sync_palette();
    SETTINGS_ITEMS[0].checked = g_menu_visible ? true : false;
    // Every pane gains or loses whole rows. Same single path a real window
    // resize takes, so the row-aligned scroll geometry and every child's
    // SIGWINCH are handled by the code that already owns them (#220/#227).
    term_layout_reflow();
}

static void tm_pick_scheme(int idx) {
    if (idx < 0 || idx >= g_scheme_count) return;
    str_copy(g_term_palette_slug, g_schemes[idx].slug, GUI_PALETTE_SLUG_MAX);
    term_resolve_palette();
    term_prefs_save();
    tm_rebuild_settings_items();
    term_layout_redraw_all();
}

static void tm_pick_profile(int idx) {
    if (idx < 0 || idx >= g_term_profile_count) return;
    g_term_profile_active = idx;
    term_profile_apply(&g_term_profiles[idx]);
    tm_sync_palette();
    tm_rebuild_settings_items();
    // A profile carries a font size and a scrollback depth, so the grid can
    // change. One reflow, every pane, every child.
    term_layout_reflow();
}

static void tm_toggle_monitor(int monitor) {
    int tab = term_layout_active_tab();
    term_notify_set_monitor(tab, monitor, !term_notify_get_monitor(tab, monitor));
    tm_rebuild_settings_items();
    term_layout_redraw_all();
}

static void tm_prefs(void) {
    term_prefs_dialog();
    term_menu_refresh();
    // term_prefs_dialog() reflows internally when it needs to; this repaints
    // the chrome it drew over either way.
    term_layout_redraw_all();
}

// Dispatch one id returned by gui_menu_bar_click()/gui_menu_key().
static void term_menu_action(int id) {
    if (id >= MID_BOOKMARK_BASE) {
        int b = id - MID_BOOKMARK_BASE;
        if (b >= 0 && b < g_bookmark_count) {
            char cmd[TM_PATH_MAX + 8];
            snprintf(cmd, sizeof(cmd), "cd %s", g_bookmarks[b]);
            tm_run_command(cmd);
            tm_rebuild_bookmark_items();
            term_redraw();
        }
        return;
    }
    if (id >= MID_PROFILE_BASE) { tm_pick_profile(id - MID_PROFILE_BASE); return; }
    if (id >= MID_SCHEME_BASE)  { tm_pick_scheme(id - MID_SCHEME_BASE);   return; }

    switch (id) {
        case MID_NEW_TAB:       term_layout_command(TL_CMD_NEW_TAB); break;
        case MID_NEW_WINDOW:    tm_new_window(); break;
        case MID_CLOSE_TAB:
            if (term_layout_command(TL_CMD_CLOSE_TAB) < 0) term_menu_quit = 1;
            break;
        case MID_CLOSE_WINDOW:  term_menu_quit = 1; break;
        case MID_COPY:          term_select_copy(); break;
        case MID_PASTE:         term_select_paste(); term_redraw(); break;
        case MID_SELECT_ALL:    term_select_all(); term_redraw(); break;
        case MID_FIND:          term_search_open(); term_layout_redraw_all(); break;
        case MID_FIND_NEXT:     term_search_next(); term_layout_redraw_all(); break;
        case MID_FIND_PREV:     term_search_prev(); term_layout_redraw_all(); break;
        case MID_CLEAR_SB:      tm_clear_scrollback(); break;
        case MID_RESET:         term_full_reset(); term_redraw(); break;
        case MID_PREFS:         tm_prefs(); break;
        case MID_SPLIT_RIGHT:   term_layout_command(TL_CMD_SPLIT_RIGHT); break;
        case MID_SPLIT_DOWN:    term_layout_command(TL_CMD_SPLIT_DOWN); break;
        case MID_CLOSE_SPLIT:   term_layout_command(TL_CMD_CLOSE_SPLIT); break;
        case MID_MAXIMIZE_PANE: term_layout_command(TL_CMD_MAXIMIZE_PANE); break;
        case MID_PANE_TO_TAB:   term_layout_command(TL_CMD_PANE_TO_TAB); break;
        case MID_ZOOM_IN:       tm_set_font_size(g_term_font.size + 1); break;
        case MID_ZOOM_OUT:      tm_set_font_size(g_term_font.size - 1); break;
        case MID_ZOOM_RESET:    tm_set_font_size(TERM_DEFAULT_FONT_SIZE); break;
        case MID_SHOW_MENUBAR:  tm_set_menu_visible(!g_menu_visible); break;
        case MID_MON_FINISH:    tm_toggle_monitor(TERM_MON_FINISH); break;
        case MID_MON_ACTIVITY:  tm_toggle_monitor(TERM_MON_ACTIVITY); break;
        case MID_MON_SILENCE:   tm_toggle_monitor(TERM_MON_SILENCE); break;
        case MID_MON_BELL:      tm_toggle_monitor(TERM_MON_BELL); break;
        case MID_BM_ADD:        tm_bookmark_add(); break;
        case MID_BM_REMOVE:     tm_bookmark_remove(); break;
        case MID_HELP_COMMANDS: tm_run_command("help"); term_redraw(); break;
        case MID_HELP_KEYS:     tm_help_shortcuts(); break;
        case MID_HELP_ABOUT:    tm_help_about(); break;
        default: break;
    }
    tm_apply_context();
}

// ===========================================================================
// Public entry points
// ===========================================================================
// Idle healing: the window lost focus, so drop any open menu and any hover
// highlight. A window stops receiving EVENT_MOUSE_MOVE the instant the pointer
// crosses into another window, so hot_top freezes at the edge and the bar goes
// on drawing a hovered label indefinitely.
//
// THIS KERNEL EMITS NO FOCUS OR BLUR EVENT TO AN APP. EVENT_WINDOW_BLUR
// appears exactly twice in the whole tree and both are enum declarations -
// kernel/proc/syscall.h says so at SYS_KEY_MODS and again at SYS_WIN_GET_STATE,
// and terminal/main.c's own event loop is built around it ("that expiry is the
// ONLY safe healing point"). An earlier version of this fix handled
// EVENT_WINDOW_BLUR and was DEAD CODE; it was caught by running it, not by
// reading it: the Calculator was given focus and the terminal's menu stayed
// open with its label still highlighted.
//
// SYS_WIN_GET_STATE (#221) is the purpose-built replacement: it reports the
// caller's OWN window state, focus included. Checked on the existing 100 ms
// idle branch, which is the established healing point in this loop and adds no
// wait of its own.
//
// Returns 1 if the caller must redraw. Cheap when there is nothing to heal:
// two int compares before it makes the syscall.
int term_menu_tick(void) {
    if (!g_menu_visible) return 0;
    if (!gui_menu_is_open(&g_bar) && g_bar.hot_top < 0) return 0;
    int st = win_get_state(window_handle);
    if (st < 0 || (st & WIN_STATE_FOCUSED)) return 0;
    gui_menu_leave(&g_bar);
    return 1;
}

void term_menu_refresh(void) {
    tm_sync_palette();
    tm_rebuild_settings_items();
    tm_bookmarks_load();
    tm_rebuild_bookmark_items();
    tm_sync_bookmark_checks();
    tm_apply_context();
}

void term_menu_init(void) {
    if (g_inited) return;
    g_inited = 1;
    gui_menu_bar_init(&g_bar, MENUS, MENU_COUNT, 0, 0, TERM_MENU_BAR_H);
    term_content_y = TERM_MENU_BAR_H;
    term_menu_refresh();
    term_render_add_chrome_hook(tm_draw_chrome);
}

// Before opening or hit-testing, refresh the state the rows display. Cheap:
// a handful of string compares plus term_layout_can() calls, none of which
// allocate or touch the filesystem.
static void tm_pre_input(void) {
    tm_sync_bookmark_checks();
    tm_apply_context();
}

int term_menu_event(const gui_event_t *ev) {
    if (!ev || !g_menu_visible) {
        // F10 has to work even when the bar is hidden, or hiding it would be a
        // one-way door with no way back. Konsole warns about exactly this.
        if (ev && ev->type == EVENT_KEY_DOWN && ev->keycode == GUI_KEY_F10) {
            tm_set_menu_visible(1);
            return 1;
        }
        return 0;
    }

    switch (ev->type) {
        case EVENT_MOUSE_DOWN: {
            tm_pre_input();
            int id = gui_menu_bar_click(&g_bar, ev->mouse_x, ev->mouse_y,
                                        tm_win_w(), tm_win_h());
            if (id == -1) return 0;              // not the menu's click
            if (id >= 0) term_menu_action(id);
            term_layout_redraw_all();
            return 1;
        }
        case EVENT_MOUSE_MOVE:
            // Only claim the event when something visibly changed. When the
            // menu is closed and the pointer is off the bar this is a no-op
            // returning 0, so the scrollbar drag and term_select's drag still
            // see every move they need.
            tm_pre_input();
            if (!gui_menu_motion(&g_bar, ev->mouse_x, ev->mouse_y,
                                 tm_win_w(), tm_win_h())) return 0;
            term_layout_redraw_all();
            return 1;
        case EVENT_MOUSE_UP:
            if (!gui_menu_is_open(&g_bar)) return 0;
            gui_menu_release(&g_bar);
            return 1;
        case EVENT_MOUSE_SCROLL:
            if (!gui_menu_is_open(&g_bar)) return 0;
            // A wheel event anywhere while a popup is open belongs to the
            // popup: scrolling a pane out from under an open menu is not what
            // any menu does.
            if (gui_menu_wheel(&g_bar, ev->mouse_x, ev->mouse_y,
                               tm_win_w(), tm_win_h(), ev->scroll_delta))
                term_layout_redraw_all();
            return 1;
        case EVENT_KEY_DOWN:
            break;                                // handled below
        default:
            return 0;
    }

    // 1. An open popup owns the keyboard outright.
    if (gui_menu_is_open(&g_bar)) {
        int id = gui_menu_key(&g_bar, ev->keycode, ev->key_char);
        if (id >= 0) term_menu_action(id);
        term_layout_redraw_all();
        return 1;   // swallow everything, including the keys gui_menu ignores
    }

    // 2. F10 toggles the bar. It is free: key_event_to_bytes() has no case for
    //    it, so nothing is stolen from the shell or from a foreground child.
    //    (keys.h: 0x87 is ALSO the DELIVERED LShift RELEASE code, which is why
    //    this is gated on EVENT_KEY_DOWN - the release only ever arrives as
    //    EVENT_KEY_UP. Matching the keycode without the type would toggle the
    //    menu bar every time the user let go of Shift.)
    if (ev->keycode == GUI_KEY_F10) {
        tm_set_menu_visible(!g_menu_visible);
        return 1;
    }

    // 3. Alt+<first letter of a menu label> opens that menu, the classic
    //    mnemonic. Alt IS deliverable on this kernel (measured, #221: press
    //    0x9A, release 0x1C), so this is a real path, not a hopeful one. The
    //    MATCHING lives in the shared widget (gui_menu_alt_open), not here, so
    //    the Editor's menu bar gets the same behaviour for one line.
    //
    //    GATED ON !g_pty_active, and that is correctness, not caution:
    //    Alt+<letter> is a BINDING INSIDE most full-screen programs (vi's meta
    //    keys, mc's menu, emacs' entire M-x world). Claiming it while a child
    //    owns the terminal would silently eat that program's key. At a shell
    //    prompt nothing is bound to Alt. The mouse and F10 reach the menu in
    //    both states; only this route is state-gated.
    if (!g_pty_active && gui_mods_is(GUI_MOD_ALT)) {
        int L = gui_mods_letter(ev);
        if (L) {
            tm_pre_input();
            if (gui_menu_alt_open(&g_bar, L)) { term_layout_redraw_all(); return 1; }
        }
    }

    // 4. Ctrl+Shift+<letter> accelerators, through the SHARED modifier tracker
    //    (userland/libc/gui_mods.h, #221). This file tracks no modifier state
    //    of its own; main() drives the tracker by dequeuing through
    //    gui_mods_next_event(). gui_mods_is() is an EXACT chord test, so plain
    //    Ctrl+C still reaches the pty as 0x03 and is not intercepted here.
    //    ONLY THE FOUR NOBODY ELSE OWNS. Ctrl+Shift+C/V/A are term_select's
    //    (term_select_handle_key), and Ctrl+Shift+T/W/O and the two split
    //    characters are term_layout's (tl_shortcut). Both run after this
    //    function returns 0, so binding them here as well would be two
    //    handlers for one chord: the classic way a shortcut ends up firing
    //    twice, or firing the version that was not maintained. The MENU ROWS
    //    still call term_select_copy()/term_layout_command() - a menu is a
    //    second ROUTE to one function, which is the point - and each row's
    //    shortcut hint names the accelerator that actually fires, which is a
    //    different thing from this file owning it.
    if (gui_mods_is(GUI_MOD_CTRL | GUI_MOD_SHIFT)) {
        tm_pre_input();
        switch (gui_mods_letter(ev)) {
            case 'n': term_menu_action(MID_NEW_WINDOW); return 1;
            case 'k': if (!g_pty_active) { term_menu_action(MID_CLEAR_SB); return 1; } break;
            case 'x': if (!g_pty_active) { term_menu_action(MID_RESET);    return 1; } break;
            case 'd': if (!g_pty_active) { term_menu_action(MID_BM_ADD);   return 1; } break;
            default: break;
        }
    }
    return 0;
}
