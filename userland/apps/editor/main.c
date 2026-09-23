// editor - Text Editor for MayteraOS (user-space version)
// #272 uplift: line-number gutter, find/replace, rich status bar, multi-line
// selection (keyboard + mouse drag), clipboard, and lightweight linting cues.
// Existing open/save behaviour is preserved.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_font.h"
#include "../../libc/gui_menu.h"
#include "../../libc/userconf.h"   // #743: checked whole-file write + per-user paths
#include "../../libc/keys.h"   // #243: GUI_KEY_* nav codes
#include "../../libc/theme.h"  // [no-ticket] editor uplift: theme_color()/theme_metric()
#include "../../libc/gui_style.h" // (edglass) gui_glass_backdrop_* / gui_fill_rounded_aa / gui_soft_shadow / gui_chevron

// Editor dimensions
static int g_ed_w = 640, g_ed_h = 480;  // live content size (EVENT_RESIZE)
#define EDITOR_WIDTH    g_ed_w
#define EDITOR_HEIGHT   g_ed_h

// ---------------------------------------------------------------------------
// (edglass) Glass geometry: the SAME numbers the Calculator (calc/main.c) and
// the Task Manager (taskmgr logic.rs) use, so every glass window shares one
// geometry. Two panels sit on the frosted backdrop: a header panel holding
// the menu bar (and the find row when it is open) and a body panel holding
// the gutter, the document, the scrollbar and the status row. The margins
// between and around them ARE the backdrop blit; nothing is ever cleared
// with a flat fill (docs/UI_GLASS_DESIGN_SYSTEM.md section 11).
// ---------------------------------------------------------------------------
#define PAD          10      // window margin: the backdrop shows here
#define PANEL_R      12      // panel corner radius
#define PANEL_IN     12      // inset from a panel edge to its content
#define PANEL_GAP    8       // vertical gap between the two panels
#define HDR_BAR_H    36      // header panel height with the find row closed
#define FIND_ROW_H   30      // extra header height while the find row is open
#define MENU_BAR_H   26      // menu bar height (TAB_H everywhere else)
#define BODY_TOP_IN  8       // body panel top edge -> first text row
#define GUTTER_W     48      // line-number gutter width inside the body panel
#define GUTTER_GAP   8       // gutter hairline -> first text column
#define STATUS_ROW_H 24      // status row inside the body panel bottom
#define SB_TRACK_W   8       // scrollbar pill width (centred in the reservation)
// Cell metrics, derived from the SELECTED font at runtime (#351). These were
// #define 8 / 16, hardwired to the 8x16 bitmap font, which is what made the
// editor unable to honour a font choice at all. ed_apply_font() recomputes them.
static int g_cell_w = 8;
static int g_cell_h = 16;
#define CHAR_W          g_cell_w
#define CHAR_H          g_cell_h

// ---- Selected font (#351). Rendered via the shared registry; chosen via the
// shared gui_font_dialog(), so the editor contains NO font UI of its own.
static gui_font_sel_t g_font;
static int g_ascent = 12;

// Pull the cell size out of the chosen face. Width comes from the advance of
// 'M': for a monospace family every advance is identical, so this IS the cell;
// for a proportional family it is the widest-ish cell, which letter-spaces the
// text rather than overlapping it (readable, and the same compromise a grid
// editor always makes).
static void ed_apply_font(void) {
    int m[3];
    if (font_metrics(g_font.face, g_font.size, m) == 0) {
        g_ascent = m[0];
        int lh = m[0] - m[1] + m[2];        // ascent - descent + line gap
        g_cell_h = (lh > 4) ? lh : g_font.size + 2;
    } else {
        g_ascent = g_font.size;
        g_cell_h = g_font.size + 2;
    }
    font_glyph_meta_t meta;
    int adv = font_glyph(g_font.face, g_font.size, g_font.style_bits, 'M', &meta, 0, 0);
    g_cell_w = (adv > 0) ? adv : (g_font.size * 6 / 10);
    if (g_cell_w < 4) g_cell_w = 4;
    if (g_cell_h < 6) g_cell_h = 6;
}

// #528: window chrome. win_create() takes the OUTER window size and the kernel
// subtracts the chrome to get the drawable canvas, so the chrome is ADDED ON at
// create time and the canvas is then re-read with win_get_size(). Same constants
// and same convention as Settings (SET_CHROME_W/H), which is the reference app.
// An app that passes its exact content size to win_create() silently loses its
// bottom row with no error.
//
// This replaces a TITLEBAR_INSET of 22 whose own comment said it was for
// "screen->local coordinate conversion": a leftover from before the kernel began
// delivering content-relative mouse coords. That conversion is long gone from the
// event handlers, but the stale constant survived as a fudge subtracted from the
// window HEIGHT. It was wrong in both directions: 22 understates the real 24px
// chrome, so the status bar lost its bottom 2 rows; and after an EVENT_RESIZE set
// g_ed_h to the true CONTENT height it became 22px of pure dead space with the
// status bar floating above the bottom edge.
#define ED_CHROME_W      4
#define ED_CHROME_H     24
#define BORDER_INSET    2

// [no-ticket] Scrollbar width, derived from the theme's own metric instead of
// a hardcoded literal. Every caller that needs the scrollbar's width (the
// content-area reservation AND the two draw-time rects) goes through this one
// function, so they cannot drift apart the way CONTENT_W's old "-16" and the
// draw-time "14"/"10" literals did (a real, currently-shipping mismatch: see
// CHANGELOG). theme_metric() already applies the live UI-scale factor; do not
// multiply by scale again here.
static int ed_scrollbar_w(void) {
    return theme_metric_or(THEME_METRIC_SCROLLBAR_W, 16);
}

// (edglass) Panel rects. ONE definition each: the draw path and every hit
// test read these, so a rect cannot drift between the two (glass doc
// section 8, "one definition for anything three places agree about").
static bool find_open;   // defined with the find state below; needed here
static int hdr_h(void)   { return HDR_BAR_H + (find_open ? FIND_ROW_H : 0); }
static int hdr_x(void)   { return PAD; }
static int hdr_y(void)   { return PAD; }
static int hdr_w(void)   { return EDITOR_WIDTH - 2 * PAD; }
static int body_x(void)  { return PAD; }
static int body_y(void)  { return PAD + hdr_h() + PANEL_GAP; }
static int body_w(void)  { return EDITOR_WIDTH - 2 * PAD; }
static int body_h(void)  { return EDITOR_HEIGHT - PAD - body_y(); }
static int body_bottom(void) { return body_y() + body_h(); }
// Status row: the bottom STATUS_ROW_H of the body panel, a hairline above it.
static int status_y(void) { return body_bottom() - STATUS_ROW_H; }

// Content area dimensions (recomputed per-frame so the find bar can push it down)
#define CONTENT_X       (body_x() + GUTTER_W + GUTTER_GAP)
#define CONTENT_RIGHT   (body_x() + body_w() - PANEL_IN - ed_scrollbar_w())
#define CONTENT_W       (CONTENT_RIGHT - CONTENT_X)

// Buffer limits
#define MAX_BUFFER      (64 * 1024)  // 64KB text buffer
#define MAX_LINES       4096
#define MAX_PATH        256
#define MAX_FIND        128
#define MAX_CLIP        (16 * 1024)

// ---------------------------------------------------------------------------
// (edglass) Colour tokens: docs/UI_GLASS_DESIGN_SYSTEM.md section 1, the
// same names and hex the Calculator / Task Manager / Media Player / Image
// Viewer glass restyles use. FIXED dark glass regardless of theme, as those
// four are: the window carries its own frosted backdrop, so the theme's
// window/menu colours never appear inside it. (The previous restyle read
// every colour from theme_color(); that stays true of the MENU METRICS, font
// size and row height, which still track the live theme via
// gui_menu_sync_theme(), and of the scrollbar reservation, ed_scrollbar_w().)
// ---------------------------------------------------------------------------
#define C_PANEL       0x00122420   // WEL_BG_MID: panel fill, glass tint, the outer colour AA edges blend toward
#define C_CARD        0x000E1D1B   // DK_CARD_FILL: menu popup fill, scrollbar track, find-row buttons
#define C_EDGE        0x002C4A44   // DK_STROKE_UNSEL: panel border, hairlines, separators
#define C_EDGE_GLASS  0x006FA99E   // DK_EDGE_GLASS: strokes on glass (popup border), scroll thumb, match outline
#define C_INK         0x00F3FBF9   // DK_HEADLINE: document text, current line's numeral
#define C_INK_DIM     0x00A9D9CC   // DK_BODY: menu labels, gutter numerals, status, match count
#define C_ACCENT      0x006AE2CF   // DK_ACCENT: selection fill, caret, open menu, focused field border, bracket outline
#define C_ACCENT_INK  0x0004231A   // text on the accent
#define C_ERR         0x00FFAAA2   // DK_ERROR: save-failed / unbalanced-bracket cue
#define C_BTN_TOP     0x000F8068   // DK_BTN_TOP: active find match fill (white ink, 4.9:1)
#define C_FIELD_LABEL 0x008FCFC0   // DK_FIELD_LABEL: "Find" / "Repl" eyebrow labels
#define C_IN_FILL     0x00213B34   // DK_INPUT_FILL
#define C_IN_BORDER   0x004E7168   // DK_INPUT_BORDER (resting)
#define C_IN_TEXT     0x00EAF6F2   // DK_INPUT_TEXT
#define WEL_BG_TOP    0x000A1614   // backdrop gradient fallback, top stop
#define WEL_BG_BOTTOM 0x00050A09   // backdrop gradient fallback, bottom stop
#define C_WHITE       0x00FFFFFF   // DK_BTN_TEXT: ink on C_BTN_TOP

// The names the draw code below has always used, now bound to the tokens
// above so every use site reads as the role it plays. Derived fills
// (current line, menu hover) come from gui_lighten() of C_PANEL, the same
// way the Calculator lightens its panel for a hovered pill.
#define TEXT_COLOR       C_INK
#define LINE_NUM_COLOR   C_INK_DIM
#define SELECTION_BG     C_ACCENT
#define SELECTION_FG     C_ACCENT_INK
#define CURSOR_COLOR     C_ACCENT
#define MENU_HINT        C_INK_DIM
#define STATUS_TEXT      C_INK_DIM
#define SCROLL_TRACK     C_CARD
#define SCROLL_THUMB     C_EDGE_GLASS
#define MATCH_CUR_BG     C_BTN_TOP
#define MATCH_CUR_FG     C_WHITE
#define BRACKET_OUTLINE  C_ACCENT
#define MATCH_OUTLINE    C_EDGE_GLASS
#define TRAIL_WS_DOT     C_INK_DIM
#define WARN_COLOR       C_ERR
#define CUR_LINE_BG      gui_lighten(C_PANEL, 10)
#define BG_COLOR         C_PANEL       // the document sits directly on the body panel

// Live theme tracking. get_theme() returns the active theme index (0-11).
// (edglass) Still polled, for METRICS only: the menu bar's type size and row
// height and the scrollbar reservation come from the theme; the colours do not.
static int g_theme_last = -1;

// ---------------------------------------------------------------------------
// (edglass) The frosted-wallpaper backdrop: the shared libc recipe
// (userland/libc/gui_style.h gui_glass_backdrop_*, consolidated at glasslib).
// This app owns only the two persistent pieces the API asks for.
// ---------------------------------------------------------------------------
static uint32_t g_bd[GUI_GLASS_BD_W * GUI_GLASS_BD_H];
static int g_bd_wi = GUI_GLASS_BD_NEVER;   // wallpaper index the backdrop was built for
static int g_chrome_dirty = 1;             // the ONLY thing that can make editor_redraw() blit the backdrop
// Layout facts the last blitted frame was composed for. A change in either
// UNCOVERS margin (the find row closing moves the body panel up and leaves
// header-panel pixels in the new gap; a menu popup closing or switching
// leaves popup pixels over the gap and margins it overlapped), so the next
// frame must blit the backdrop again (blame.md audglass trap 4). Tracked at
// the top of editor_redraw() by construction rather than at each toggle site.
static int g_layout_find_open = -1;
static int g_layout_menu_open = -1;

// Rebuild the backdrop if the wallpaper changed; a changed index marks the
// chrome dirty (same poll the Calculator makes every frame).
static void sync_backdrop(void) {
    if (gui_glass_backdrop_sync(g_bd, &g_bd_wi, C_PANEL, 158, WEL_BG_TOP, WEL_BG_BOTTOM))
        g_chrome_dirty = 1;
}

// Editor state
static int window_handle = -1;
static char buffer[MAX_BUFFER];
static uint32_t buffer_len = 0;
static uint32_t cursor_pos = 0;
static uint32_t line_starts[MAX_LINES];
static uint32_t line_count = 1;
static uint32_t cursor_line = 0;
static uint32_t cursor_col = 0;
static uint32_t scroll_line = 0;
static uint32_t scroll_col = 0;

// Selection (anchor-based: sel_anchor is fixed, cursor_pos is the moving end)
static bool has_selection = false;
static uint32_t sel_anchor = 0;        // where the selection began
static bool mouse_selecting = false;   // dragging with the mouse button held
// Keyboard selection mode: Ctrl+Space toggles a mode in which arrow/home/end/
// pgup/pgdn extend the selection.
//
// CORRECTED 2026-08-25 (#221 phase 0). This comment used to end with "The
// kernel gui_event_t has no shift/modifier field, so shift+arrow cannot be
// detected from userland", and the Ctrl+Space mode existed BECAUSE of that
// sentence. The premise is true and the conclusion is FALSE: gui_event_t
// carries no modifier field, but Shift, Ctrl and Alt arrive as ordinary
// keycoded press AND release events in the same per-window queue as the key
// they modify, so the state is trivially trackable.
//
// MEASURED, by typing, on VM 2221 / golden build 2040 with a Ring-3 probe
// (tools/testing/probes/keyprobe.c) rather than by reading the source:
// Shift+Up delivered LSHIFT press (0x95), UP (0x80), UP release, LSHIFT
// release (0x87) - the exact sequence this comment said was unobservable.
//
// A false claim in the tree is more expensive than no claim: this one was
// quoted back during the #221 terminal design pass as a reason the whole
// Ctrl+Shift shortcut scheme could not be built. libc/gui_mods.h is now the
// shared tracker. Wiring real shift+arrow selection into this editor is a
// separate change and has deliberately NOT been done here; the Ctrl+Space
// mode below still works exactly as before.
static bool kbd_sel_mode = false;

// Clipboard
static char clipboard[MAX_CLIP];
static uint32_t clip_len = 0;

// File state
static char filename[MAX_PATH] = "";
static bool modified = false;

// Find/Replace state (find_open itself is declared above, beside the panel
// geometry that depends on it; false at start like every other flag here)
static bool replace_mode = false;      // true => show replace field too
static int  find_field = 0;            // 0 = find field, 1 = replace field
static char find_text[MAX_FIND] = "";
static char repl_text[MAX_FIND] = "";
static uint32_t find_len = 0;
static uint32_t repl_len = 0;
static int  match_count = 0;
static int  match_index = 0;           // 1-based index of the active match (0 = none)
static uint32_t cur_match_pos = 0;     // buffer pos of the active match
static bool have_cur_match = false;

// Window position for coordinate conversion (refreshed each event)
static int win_x = 50;
static int win_y = 30;

// --- Menu bar (#562: fixed via the shared gui_menu primitive, #512) --------
// Previously four menus were DRAWN (File, Edit, Search, Font) but only two
// (Search, Font) were hit-tested, via a hand-coded array of x-ranges that had
// drifted out of sync with what was drawn - File and Edit were pure
// decoration. Fixing that "locally" would have meant hand-rolling a fourth
// slightly-different dropdown in this file; gui_menu_bar_t (built on the
// shared gui_list_t, #512) is the one place that owns menu geometry and hit
// testing instead, so a mismatch like that structurally cannot happen again.
typedef enum {
    ID_FILE_NEW = 1, ID_FILE_SAVE, ID_FILE_EXIT,
    ID_EDIT_CUT, ID_EDIT_COPY, ID_EDIT_PASTE, ID_EDIT_SELECT_ALL,
    ID_SEARCH_FIND, ID_SEARCH_FIND_NEXT, ID_SEARCH_REPLACE,
    ID_FONT_CHOOSE
} menu_action_id_t;

static const gui_menu_item_t FILE_ITEMS[] = {
    { "New",  "Ctrl+N", ID_FILE_NEW,  true },
    { "Save", "Ctrl+S", ID_FILE_SAVE, true },
    { NULL,   NULL,     0,            false },   // separator
    { "Exit", NULL,     ID_FILE_EXIT, true },
};
static const gui_menu_item_t EDIT_ITEMS[] = {
    { "Cut",         "Ctrl+X", ID_EDIT_CUT,        true },
    { "Copy",        "Ctrl+C", ID_EDIT_COPY,       true },
    { "Paste",       "Ctrl+V", ID_EDIT_PASTE,      true },
    { NULL,          NULL,     0,                  false },   // separator
    { "Select All",  "Ctrl+A", ID_EDIT_SELECT_ALL, true },
};
static const gui_menu_item_t SEARCH_ITEMS[] = {
    { "Find...",     "Ctrl+F", ID_SEARCH_FIND,      true },
    { "Find Next",   "Ctrl+G", ID_SEARCH_FIND_NEXT, true },
    { "Replace...",  "Ctrl+H", ID_SEARCH_REPLACE,   true },
};
static const gui_menu_item_t FONT_ITEMS[] = {
    { "Font...", NULL, ID_FONT_CHOOSE, true },
};
static const gui_menu_t EDITOR_MENUS[] = {
    { "File",   FILE_ITEMS,   4 },
    { "Edit",   EDIT_ITEMS,   5 },
    { "Search", SEARCH_ITEMS, 3 },
    { "Font",   FONT_ITEMS,   1 },
};
#define EDITOR_MENU_COUNT 4
static gui_menu_bar_t g_menu;

// (edglass) The backdrop colour under content pixel (x, y): what the
// kernel's nearest-neighbour scale put there, to within the blur. Every AA
// edge and shadow drawn onto the backdrop takes its outer colour from here;
// a flat guess is what produces a square halo around a round corner.
static uint32_t bd_at(int x, int y) {
    return gui_glass_backdrop_at(g_bd, EDITOR_WIDTH, EDITOR_HEIGHT, x, y);
}

// One glass panel: soft shadow, AA rounded fill, 1px border, 1px top
// highlight; the layering the Calculator's and Task Manager's draw_panel()
// use. Every outer colour is sampled from the backdrop under that edge so
// the fringe and corners match what the blit put there. Drawn EVERY frame:
// the same inputs give the same pixels, so the repaint is idempotent and
// there is no static/dynamic chrome split to keep in step (section 11: only
// the backdrop blit itself is a commit).
static void draw_panel(int x, int y, int w, int h) {
    uint32_t below = bd_at(x + w / 2, y + h + 3);
    uint32_t c0 = bd_at(x + 4, y + 4),     c1 = bd_at(x + w - 4, y + 4),
             c2 = bd_at(x + 4, y + h - 4), c3 = bd_at(x + w - 4, y + h - 4);
    uint32_t outer = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t m = (((c0 >> sh) & 0xFF) + ((c1 >> sh) & 0xFF) + ((c2 >> sh) & 0xFF) + ((c3 >> sh) & 0xFF)) / 4;
        outer |= m << sh;
    }
    gui_soft_shadow(window_handle, x, y + 2, w, h, PANEL_R, below);
    gui_fill_rounded_aa(window_handle, x, y, w, h, PANEL_R, C_PANEL, outer);
    gui_rounded_border(window_handle, x, y, w, h, PANEL_R, C_EDGE);
    win_draw_rect(window_handle, x + PANEL_R, y + 1, w - 2 * PANEL_R, 1, gui_lighten(C_PANEL, 16));
}

// The menu bar's colours: the glass tokens, applied AFTER every
// gui_menu_sync_theme() so the widget keeps the theme's metrics but never
// the theme's colours (gui_menu.h: "gui_menu_set_palette() remains the
// override for an app with its own chrome identity"). The popup floats over
// the backdrop, so its border is the on-glass stroke, not the panel one.
static void apply_menu_palette(void) {
    gui_menu_palette_t pal;
    pal.bar_bg             = C_PANEL;
    pal.bar_text           = C_INK_DIM;
    pal.bar_hover_bg       = gui_lighten(C_PANEL, 18);
    pal.bar_open_bg        = C_ACCENT;
    pal.bar_open_text      = C_ACCENT_INK;
    pal.popup_bg           = C_CARD;
    pal.popup_border       = C_EDGE_GLASS;
    pal.item_text          = C_INK;
    pal.item_text_disabled = gui_mix(C_INK_DIM, C_CARD, 140);
    pal.item_hover_bg      = C_ACCENT;
    pal.item_hover_text    = C_ACCENT_INK;
    pal.shortcut_text      = C_INK_DIM;
    pal.separator          = C_EDGE;
    gui_menu_set_palette(&g_menu, &pal);
}

// Lint cues
static int bracket_balance = 0;        // net (open - close) over the whole file
static bool have_bracket_match = false;
static uint32_t bracket_match_pos = 0; // partner of the bracket next to caret

// Forward declarations
static void editor_redraw(void);
static void recalc_lines(void);
static void update_cursor_pos(void);
static void ensure_visible(void);
static void insert_char(char c);
static void delete_char(void);
static void delete_selection(void);
static void recompute_lint(void);
static void recompute_matches(void);

// --- geometry helpers (find bar shifts content down when open) -------------
// (edglass) The document lives inside the body panel: from BODY_TOP_IN below
// the panel's top edge down to just above the status row's hairline.
static int content_y(void) {
    return body_y() + BODY_TOP_IN;
}
static int content_h(void) {
    int h = (status_y() - 1 - 4) - content_y();
    return h > 0 ? h : 0;
}
static int visible_rows(void) { return content_h() / CHAR_H; }
static int visible_cols(void) { return CONTENT_W / CHAR_W; }

// Recalculate line start positions
static void recalc_lines(void) {
    line_count = 1;
    line_starts[0] = 0;

    for (uint32_t i = 0; i < buffer_len && line_count < MAX_LINES; i++) {
        if (buffer[i] == '\n') {
            line_starts[line_count++] = i + 1;
        }
    }
}

// Update cursor line/col from position
static void update_cursor_pos(void) {
    cursor_line = 0;
    cursor_col = 0;

    for (uint32_t i = 0; i < cursor_pos && i < buffer_len; i++) {
        if (buffer[i] == '\n') {
            cursor_line++;
            cursor_col = 0;
        } else {
            cursor_col++;
        }
    }
}

// Ensure cursor is visible (adjust scroll)
static void ensure_visible(void) {
    int rows = visible_rows();
    int cols = visible_cols();
    if (cursor_line < scroll_line) {
        scroll_line = cursor_line;
    }
    if ((int)cursor_line >= (int)scroll_line + rows) {
        scroll_line = cursor_line - rows + 1;
    }
    if (cursor_col < scroll_col) {
        scroll_col = cursor_col;
    }
    if ((int)cursor_col >= (int)scroll_col + cols) {
        scroll_col = cursor_col - cols + 1;
    }
}

// Get line start position
static uint32_t get_line_start(uint32_t line) {
    if (line >= line_count) return buffer_len;
    return line_starts[line];
}

// Get line length (excluding newline)
static uint32_t get_line_length(uint32_t line) {
    if (line >= line_count) return 0;
    uint32_t start = line_starts[line];
    uint32_t end;
    if (line + 1 < line_count) {
        end = line_starts[line + 1] - 1;
    } else {
        end = buffer_len;
    }
    return end > start ? end - start : 0;
}

// Selection bounds (ordered). Returns true when a non-empty selection exists.
static bool selection_range(uint32_t *out_start, uint32_t *out_end) {
    if (!has_selection) return false;
    uint32_t a = sel_anchor;
    uint32_t b = cursor_pos;
    uint32_t start = a < b ? a : b;
    uint32_t end = a > b ? a : b;
    if (start == end) return false;
    if (out_start) *out_start = start;
    if (out_end) *out_end = end;
    return true;
}

// Check if position is in selection
static bool pos_in_selection(uint32_t pos) {
    uint32_t start, end;
    if (!selection_range(&start, &end)) return false;
    return pos >= start && pos < end;
}

// Begin / extend / clear selection helpers --------------------------------
static void clear_selection(void) {
    has_selection = false;
}
static void start_or_keep_selection(bool extend) {
    if (extend) {
        if (!has_selection) {
            sel_anchor = cursor_pos;
            has_selection = true;
        }
    } else {
        clear_selection();
    }
}

// --- Find / matching -------------------------------------------------------
// Case-sensitive substring at a given buffer position.
static bool match_at(uint32_t pos) {
    if (find_len == 0) return false;
    if (pos + find_len > buffer_len) return false;
    for (uint32_t i = 0; i < find_len; i++) {
        if (buffer[pos + i] != find_text[i]) return false;
    }
    return true;
}

static void recompute_matches(void) {
    match_count = 0;
    match_index = 0;
    if (find_len == 0 || buffer_len == 0) { have_cur_match = false; return; }
    int idx_of_cur = 0;
    int n = 0;
    for (uint32_t p = 0; p + find_len <= buffer_len; p++) {
        if (match_at(p)) {
            n++;
            if (have_cur_match && p == cur_match_pos) idx_of_cur = n;
        }
    }
    match_count = n;
    if (have_cur_match && idx_of_cur > 0) match_index = idx_of_cur;
}

// Find next match at-or-after `from` (wrapping). Updates the active match.
static void find_next(uint32_t from, bool forward) {
    if (find_len == 0 || buffer_len == 0) { have_cur_match = false; recompute_matches(); return; }
    if (forward) {
        for (uint32_t p = from; p + find_len <= buffer_len; p++) {
            if (match_at(p)) { cur_match_pos = p; have_cur_match = true; break; }
        }
        if (!have_cur_match || cur_match_pos < from) {
            // wrap from start
            for (uint32_t p = 0; p + find_len <= buffer_len; p++) {
                if (match_at(p)) { cur_match_pos = p; have_cur_match = true; break; }
            }
        }
    } else {
        bool found = false;
        // search backwards from `from`
        uint32_t limit = (from > 0) ? from - 1 : 0;
        for (uint32_t p = limit; ; p--) {
            if (p + find_len <= buffer_len && match_at(p)) { cur_match_pos = p; found = true; break; }
            if (p == 0) break;
        }
        if (!found) {
            // wrap from end
            for (uint32_t p = (buffer_len >= find_len) ? buffer_len - find_len : 0; ; p--) {
                if (match_at(p)) { cur_match_pos = p; found = true; break; }
                if (p == 0) break;
            }
        }
        have_cur_match = found;
    }
    if (have_cur_match) {
        cursor_pos = cur_match_pos;
        // select the match
        sel_anchor = cur_match_pos;
        cursor_pos = cur_match_pos + find_len;
        has_selection = true;
        update_cursor_pos();
        ensure_visible();
    }
    recompute_matches();
}

// Replace the active match with repl_text; returns true if a replace happened.
static bool replace_current(void) {
    if (!have_cur_match || find_len == 0) return false;
    if (!match_at(cur_match_pos)) return false;
    long delta = (long)repl_len - (long)find_len;
    if ((long)buffer_len + delta >= MAX_BUFFER - 1) return false;
    uint32_t pos = cur_match_pos;
    if (delta > 0) {
        for (uint32_t i = buffer_len; i > pos + find_len; i--) {
            buffer[i - 1 + delta] = buffer[i - 1];
        }
    } else if (delta < 0) {
        for (uint32_t i = pos + find_len; i < buffer_len; i++) {
            buffer[i + delta] = buffer[i];
        }
    }
    for (uint32_t i = 0; i < repl_len; i++) buffer[pos + i] = repl_text[i];
    buffer_len += delta;
    cursor_pos = pos + repl_len;
    have_cur_match = false;
    has_selection = false;
    modified = true;
    recalc_lines();
    update_cursor_pos();
    ensure_visible();
    recompute_lint();
    return true;
}

static int replace_all(void) {
    if (find_len == 0) return 0;
    int n = 0;
    uint32_t scan = 0;
    // Walk forward; after each in-place replace continue past the inserted text.
    while (scan + find_len <= buffer_len) {
        if (match_at(scan)) {
            cur_match_pos = scan;
            have_cur_match = true;
            if (!replace_current()) break;     // out of buffer space
            scan = cursor_pos;                 // replace_current left cursor past the insert
            n++;
            if (n > 100000) break;             // safety
        } else {
            scan++;
        }
    }
    have_cur_match = false;
    recompute_matches();
    return n;
}

// --- Lint cues -------------------------------------------------------------
static const char *file_ext(void) {
    int n = (int)strlen(filename);
    for (int i = n - 1; i >= 0; i--) {
        if (filename[i] == '.') return &filename[i];
        if (filename[i] == '/') break;
    }
    return "";
}
static bool ext_is_code(void) {
    const char *e = file_ext();
    return strcasecmp(e, ".c") == 0 || strcasecmp(e, ".h") == 0 ||
           strcasecmp(e, ".cpp") == 0 || strcasecmp(e, ".js") == 0 ||
           strcasecmp(e, ".py") == 0 || strcasecmp(e, ".sh") == 0 ||
           strcasecmp(e, ".cfg") == 0 || strcasecmp(e, ".ini") == 0;
}
static bool is_open_bracket(char c)  { return c == '(' || c == '[' || c == '{'; }
static bool is_close_bracket(char c) { return c == ')' || c == ']' || c == '}'; }
static char matching_bracket(char c) {
    switch (c) {
        case '(': return ')'; case ')': return '(';
        case '[': return ']'; case ']': return '[';
        case '{': return '}'; case '}': return '{';
    }
    return 0;
}

// Compute net bracket balance + locate the partner of the bracket adjacent to
// the caret (char to the left, then char to the right).
static void recompute_lint(void) {
    bracket_balance = 0;
    have_bracket_match = false;
    if (!ext_is_code()) return;

    for (uint32_t i = 0; i < buffer_len; i++) {
        char c = buffer[i];
        if (is_open_bracket(c)) bracket_balance++;
        else if (is_close_bracket(c)) bracket_balance--;
    }

    // Find a bracket adjacent to the caret and scan for its partner.
    uint32_t bpos = 0; char bc = 0; bool have = false;
    if (cursor_pos > 0 && (is_open_bracket(buffer[cursor_pos - 1]) || is_close_bracket(buffer[cursor_pos - 1]))) {
        bpos = cursor_pos - 1; bc = buffer[cursor_pos - 1]; have = true;
    } else if (cursor_pos < buffer_len && (is_open_bracket(buffer[cursor_pos]) || is_close_bracket(buffer[cursor_pos]))) {
        bpos = cursor_pos; bc = buffer[cursor_pos]; have = true;
    }
    if (!have) return;

    char want = matching_bracket(bc);
    int depth = 0;
    if (is_open_bracket(bc)) {
        for (uint32_t i = bpos; i < buffer_len; i++) {
            if (buffer[i] == bc) depth++;
            else if (buffer[i] == want) { depth--; if (depth == 0) { bracket_match_pos = i; have_bracket_match = true; break; } }
        }
    } else {
        for (uint32_t i = bpos + 1; i > 0; i--) {
            uint32_t j = i - 1;
            if (buffer[j] == bc) depth++;
            else if (buffer[j] == want) { depth--; if (depth == 0) { bracket_match_pos = j; have_bracket_match = true; break; } }
        }
    }
}

// Is this position a trailing-whitespace cell (space/tab before EOL)?
static bool is_trailing_ws(uint32_t pos, uint32_t line) {
    if (pos >= buffer_len) return false;
    char c = buffer[pos];
    if (c != ' ' && c != '\t') return false;
    uint32_t end = get_line_start(line) + get_line_length(line);
    for (uint32_t i = pos; i < end; i++) {
        if (buffer[i] != ' ' && buffer[i] != '\t') return false;
    }
    return true;
}

// The editor remembers its own font, independent of the system UI font: a code
// editor wants monospace even when the desktop is set to a sans.
#define ED_FONT_CFG "/CONFIG/EDFONT.CFG"

// #743: set when a save did not reach the disk, cleared by a save that did.
// The editor's only previous signal was the "*" next to the filename, and that
// was cleared unconditionally by file_save(), so a failed save looked exactly
// like a successful one.
static bool save_failed = false;

static void ed_save_font(void) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s|%s|%d\n",
             g_font.family[0] ? g_font.family : "Default",
             g_font.style[0] ? g_font.style : "Regular", g_font.size);
    // #743: write() and close() were both discarded. On an ext2-backed fd the
    // bytes are buffered and the real write happens inside close(), so the
    // discarded close() result was the only error report there was.
    // Also now a per-user path (#683): /CONFIG is not writable by a non-root
    // session, so this preference could never be saved by one.
    int fd = userconf_open_write("EDFONT.CFG");
    if (userconf_finish_write(fd, buf, strlen(buf)) != 0) {
        // A font preference is cosmetic and there is no sensible recovery, but
        // it must not be silent: the status bar shows the same alarm as a
        // failed document save.
        save_failed = true;
    }
}

static void ed_load_font(void) {
    memset(&g_font, 0, sizeof(g_font));
    // [no-ticket] editor uplift: this used to default to an EMPTY family,
    // which resolves to face 0 (DejaVu Sans, a PROPORTIONAL face). Combined
    // with ed_apply_font() measuring 'M's advance as if it were fixed pitch,
    // that is why unthemed text looked double-wide / letter-spaced. Do NOT
    // change ed_apply_font()'s measurement formula: it is correct once the
    // face genuinely IS monospace (every glyph shares one advance, so
    // measuring 'M' gives the right number for every column). The fix is
    // only this default. Same family lineage as the OS default UI face
    // (visual consistency), ships real Bold/Oblique/BoldOblique, and is what
    // the tree's own font regression checks are measured against. This only
    // changes the default for an install with no saved EDFONT.CFG - a saved
    // preference below still wins, unchanged. Inconsolata and IBM Plex Mono
    // stay fully selectable via the existing Font... menu item.
    strncpy(g_font.family, "DejaVu Sans Mono", GUI_FONT_NAME_MAX - 1);
    g_font.size = 16;
    strncpy(g_font.style, "Regular", GUI_FONT_STYLE_MAX - 1);
    // #743: per-user copy first, falling back to the legacy path so an existing
    // install keeps its editor font (#683).
    int fd = userconf_open_read("EDFONT.CFG", ED_FONT_CFG);
    if (fd >= 0) {
        char buf[160];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = 0;
            char *b1 = 0, *b2 = 0;
            for (char *q = buf; *q; q++) {
                if (*q == '\n' || *q == '\r') { *q = 0; break; }
                if (*q == '|') { if (!b1) b1 = q; else if (!b2) b2 = q; }
            }
            if (b1 && b2) {
                *b1 = 0; *b2 = 0;
                strncpy(g_font.family, buf, GUI_FONT_NAME_MAX - 1);
                strncpy(g_font.style, b1 + 1, GUI_FONT_STYLE_MAX - 1);
                int sz = atoi(b2 + 1);
                if (sz >= 6 && sz <= 128) g_font.size = sz;
            }
        }
    }
    gui_font_resolve(g_font.family, g_font.style, &g_font.face, &g_font.style_bits);
    ed_apply_font();
}

// [no-ticket] editor uplift: this used to be sync_menu_theme(), which built a
// custom gui_menu_palette_t by hand every time (a "VSCode-style" override of
// the shared widget's default theming). Deleted in favour of the shared
// widget's own default: gui_menu_bar_init()'s doc comment says "A bar is
// themed by DEFAULT... gui_menu_set_palette() remains the override for an app
// with its own chrome identity (Editor)" - we are explicitly removing that
// override so the editor's menu bar tracks the same theme-driven palette,
// metrics, and layout every other app's menu bar does. Both call sites below
// now call gui_menu_sync_theme(&g_menu, -1) directly instead.

// Draw the menu bar
// [no-ticket] editor uplift: this used to also draw a right-aligned hint
// string ("Ctrl+F Find  Ctrl+H Replace  Ctrl+S Save"). Removed outright, to
// match Settings/Files, neither of which decorates its menu bar with
// shortcut hints. Nothing replaces it; the right side is deliberately empty.
// gui_menu_bar_draw() already draws TTF internally, so there is nothing
// bitmap-drawn left in this function to convert.
// [no-ticket] editor uplift: body text size (docs/UI_STYLE_GUIDE.md 4.x
// type.body) and caption size (type.caption), used for every TTF draw/measure
// pair in this file so a draw call and its matching gui_ttf_width() measure
// can never drift to different sizes. (edglass) ED_TTF_FIELD is the find
// row's control size (glass doc section 5: labels and buttons at 12).
#define ED_TTF_BODY     14
#define ED_TTF_CAPTION  11
#define ED_TTF_FIELD    12

// (edglass) Find-row geometry. ONE definition, read by draw_find_bar() AND
// by the click handler, so the rectangles that are drawn are the rectangles
// that are hit-tested (the old code carried a second copy of these literals
// in the mouse handler). All relative to the header panel.
#define FR_FIELD_H   22
#define FR_BTN_W     26
#define FR_FIND_W    190
#define FR_REPL_W    150
static int fr_y(void)        { return hdr_y() + HDR_BAR_H; }             // row top
static int fr_ctl_y(void)    { return fr_y() + (FIND_ROW_H - FR_FIELD_H) / 2; }
static int fr_label_x(void)  { return hdr_x() + PANEL_IN; }
static int fr_find_x(void)   { return fr_label_x() + 36; }
static int fr_count_x(void)  { return fr_find_x() + FR_FIND_W + 10; }
static int fr_prev_x(void)   { return fr_count_x() + 84; }
static int fr_next_x(void)   { return fr_prev_x() + FR_BTN_W + 4; }
static int fr_repl_lbl_x(void) { return fr_next_x() + FR_BTN_W + 14; }
static int fr_repl_x(void)   { return fr_repl_lbl_x() + 36; }
static int fr_repl_w(void) {
    int room = hdr_x() + hdr_w() - PANEL_IN - fr_repl_x();
    return room < FR_REPL_W ? room : FR_REPL_W;
}

// (edglass) The header panel: menu bar on the top row (the shared gui_menu
// widget, glass palette), the replace hint right-aligned beside it when the
// replace field is up, then the find row below when it is open. The panel
// is drawn every frame like every other panel here.
static void draw_header(void) {
    draw_panel(hdr_x(), hdr_y(), hdr_w(), hdr_h());
    gui_menu_bar_draw(window_handle, &g_menu);
    if (find_open && replace_mode) {
        const char *hint = "Enter replaces, Ctrl+A replaces all";
        int hw = gui_ttf_width(hint, ED_TTF_CAPTION);
        int hx = hdr_x() + hdr_w() - PANEL_IN - hw;
        int menu_right = g_menu.item_x[EDITOR_MENU_COUNT - 1] + g_menu.item_w[EDITOR_MENU_COUNT - 1];
        if (hx > menu_right + 12)
            win_draw_text_ttf(window_handle, hx, hdr_y() + 5 + (MENU_BAR_H - ED_TTF_CAPTION) / 2 - 1,
                              hint, ED_TTF_CAPTION, MENU_HINT);
    }
}

// One find-row input field (glass doc section 6: radius 4, DK_INPUT_FILL,
// 1px DK_INPUT_BORDER, the accent border when focused). The AA corner blends
// toward the panel it sits on. Caret x MUST come from the same measure
// (gui_ttf_width at the SAME size) as the draw call, or it drifts off the
// end of the typed glyphs.
static void draw_field(int x, int w, const char *text, bool focused) {
    int y = fr_ctl_y();
    gui_fill_rounded_aa(window_handle, x, y, w, FR_FIELD_H, 4, C_IN_FILL, C_PANEL);
    gui_rounded_border(window_handle, x, y, w, FR_FIELD_H, 4, focused ? C_ACCENT : C_IN_BORDER);
    int ty = y + (FR_FIELD_H - ED_TTF_FIELD) / 2 - 1;
    win_draw_text_ttf(window_handle, x + 6, ty, text, ED_TTF_FIELD, C_IN_TEXT);
    if (focused) {
        int cx = x + 6 + gui_ttf_width(text, ED_TTF_FIELD);
        if (cx < x + w - 3)
            win_draw_rect(window_handle, cx, y + 4, 1, FR_FIELD_H - 8, CURSOR_COLOR);
    }
}

// Prev / next: a nested-card key (radius 6, C_CARD, C_EDGE border) with the
// shared chevron, stroked not typed (glass doc section 6).
static void draw_nav_btn(int x, int dir) {
    int y = fr_ctl_y();
    gui_fill_rounded_aa(window_handle, x, y, FR_BTN_W, FR_FIELD_H, 6, C_CARD, C_PANEL);
    gui_rounded_border(window_handle, x, y, FR_BTN_W, FR_FIELD_H, 6, C_EDGE);
    gui_chevron(window_handle, x + FR_BTN_W / 2, y + FR_FIELD_H / 2, dir, C_INK_DIM);
}

// Draw the find / replace row (inside the header panel, which draw_header()
// has already painted).
static void draw_find_bar(void) {
    if (!find_open) return;
    int ly = fr_y() + (FIND_ROW_H - ED_TTF_FIELD) / 2 - 1;

    win_draw_text_ttf(window_handle, fr_label_x(), ly, "Find", ED_TTF_FIELD, C_FIELD_LABEL);
    draw_field(fr_find_x(), FR_FIND_W, find_text, find_field == 0);

    // Match count
    char mc[40];
    if (find_len == 0) {
        snprintf(mc, sizeof(mc), "no query");
    } else if (match_count == 0) {
        snprintf(mc, sizeof(mc), "0 matches");
    } else if (match_index == 0) {
        // (edglass) Reopening the row after a close, or after an edit
        // invalidated the active match: no match is current, so "0 of 2"
        // (what this used to print) reads as a contradiction of the 2.
        snprintf(mc, sizeof(mc), "%d matches", match_count);
    } else {
        snprintf(mc, sizeof(mc), "%d of %d", match_index, match_count);
    }
    win_draw_text_ttf(window_handle, fr_count_x(), ly, mc, ED_TTF_FIELD, MENU_HINT);

    draw_nav_btn(fr_prev_x(), GUI_CHEV_LEFT);
    draw_nav_btn(fr_next_x(), GUI_CHEV_RIGHT);

    if (replace_mode && fr_repl_w() >= 40) {
        win_draw_text_ttf(window_handle, fr_repl_lbl_x(), ly, "Repl", ED_TTF_FIELD, C_FIELD_LABEL);
        draw_field(fr_repl_x(), fr_repl_w(), repl_text, find_field == 1);
    }
}

// Draw line numbers: the gutter is the body panel itself (no separate fill,
// so nothing square ever reaches the panel's rounded corners), numerals
// right-aligned, a hairline at the gutter's right edge.
static void draw_line_numbers(void) {
    int cy = content_y();
    int ch = content_h();
    int gx = body_x() + GUTTER_W;
    win_draw_rect(window_handle, gx, cy, 1, ch, C_EDGE);

    char num_str[8];
    int rows = visible_rows();
    for (int row = 0; row < rows && scroll_line + (uint32_t)row < line_count; row++) {
        int line_num = scroll_line + row + 1;
        int y = cy + row * CHAR_H;

        gui_itoa(line_num, num_str, 8);

        // Gutter numerals are caption role, right-aligned (UI_STYLE_GUIDE.md
        // 4.4: "List/table rows... numerals right-aligned").
        int text_w = gui_ttf_width(num_str, ED_TTF_CAPTION);
        int x = gx - text_w - 8;

        uint32_t color = (scroll_line + (uint32_t)row == cursor_line) ? TEXT_COLOR : LINE_NUM_COLOR;
        win_draw_text_ttf(window_handle, x, y, num_str, ED_TTF_CAPTION, color);
    }
}

// Draw the text content
static void draw_content(void) {
    int cy = content_y();
    int ch = content_h();
    int rows = visible_rows();
    int cols = visible_cols();

    // (edglass) No content fill: the document sits directly on the body
    // panel, which draw_panel() has just repainted, so every cell already
    // holds BG_COLOR (== C_PANEL). The fills below are the exceptions.

    // Current-line highlight when the caret line is visible.
    if (cursor_line >= scroll_line && (int)cursor_line < (int)scroll_line + rows) {
        int hy = cy + (cursor_line - scroll_line) * CHAR_H;
        win_draw_rect(window_handle, CONTENT_X, hy, CONTENT_W, CHAR_H, CUR_LINE_BG);
    }

    for (int row = 0; row < rows && scroll_line + (uint32_t)row < line_count; row++) {
        uint32_t line = scroll_line + row;
        uint32_t line_start = get_line_start(line);
        uint32_t line_len = get_line_length(line);

        int y = cy + row * CHAR_H;

        for (int col = 0; col < cols && scroll_col + (uint32_t)col < line_len; col++) {
            uint32_t pos = line_start + scroll_col + col;
            char c = buffer[pos];

            int x = CONTENT_X + col * CHAR_W;

            // [no-ticket] editor uplift: fills first (selection > active-match
            // fill; current-line fill was already drawn for the whole row
            // above), THEN outlines on top (bracket-pair, all-matches), THEN
            // the trailing-ws dot glyph drawn alongside the text. The old
            // "all matches" and "matched bracket" cell BACKGROUNDS are gone:
            // this renderer has no per-pixel alpha, so a 4th/5th flat fill
            // color risked colliding with the selection/current-line fills
            // under z-order. An outline reuses tokens that already exist
            // instead of inventing new ones.
            bool sel_here = pos_in_selection(pos);
            uint32_t cell_bg = 0;
            bool have_bg = false;
            if (sel_here) { cell_bg = SELECTION_BG; have_bg = true; }
            else if (find_len > 0 && have_cur_match && pos >= cur_match_pos && pos < cur_match_pos + find_len) {
                cell_bg = MATCH_CUR_BG; have_bg = true;
            }
            if (have_bg) win_draw_rect(window_handle, x, y, CHAR_W, CHAR_H, cell_bg);

            bool is_active_match = (find_len > 0 && have_cur_match &&
                                    pos >= cur_match_pos && pos < cur_match_pos + find_len);
            if (find_len > 0 && !is_active_match) {
                // Is pos within ANY (non-active) match? Check the start of the
                // match window, same scan the old fill branch used.
                uint32_t s = (pos >= find_len - 1) ? pos - (find_len - 1) : 0;
                for (uint32_t m = s; m <= pos; m++) {
                    if (match_at(m) && pos < m + find_len) {
                        gui_draw_rect_outline(window_handle, x, y, CHAR_W, CHAR_H, MATCH_OUTLINE);
                        break;
                    }
                }
            }
            if (have_bracket_match &&
                (pos == bracket_match_pos ||
                 (cursor_pos > 0 && pos == cursor_pos - 1 && (is_open_bracket(c) || is_close_bracket(c))) ||
                 (pos == cursor_pos && (is_open_bracket(c) || is_close_bracket(c)))) &&
                (is_open_bracket(c) || is_close_bracket(c))) {
                gui_draw_rect_outline(window_handle, x, y, CHAR_W, CHAR_H, BRACKET_OUTLINE);
            }

            if (c == '\t') {
                // render tab as a faint marker, advance is still 1 cell here
            } else if (c >= ' ' && c < 127) {
                char str[2] = { c, '\0' };
                // Face-aware: honours the family/style/size from gui_font_dialog().
                // Selected text swaps to SELECTION_FG so contrast against
                // SELECTION_BG is guaranteed on every theme, active-match
                // cells swap to MATCH_CUR_FG the same way.
                uint32_t ink = sel_here ? SELECTION_FG : (is_active_match ? MATCH_CUR_FG : TEXT_COLOR);
                win_draw_text_ttf_ex(window_handle, x, y, str,
                                     g_font.face, g_font.size, g_font.style_bits,
                                     ink);
            }

            // Trailing whitespace: a small centered dot glyph, not a filled
            // cell. Blended against whatever this cell's actual fill is (the
            // renderer has no per-pixel alpha, so gui_fill_circle_aa needs the
            // real background to anti-alias against).
            if (is_trailing_ws(pos, line)) {
                uint32_t dot_bg = have_bg ? cell_bg
                                 : ((line == cursor_line) ? CUR_LINE_BG : BG_COLOR);
                int d = 3;
                int dx = x + (CHAR_W - d) / 2;
                int dy = y + (CHAR_H - d) / 2;
                gui_fill_circle_aa(window_handle, dx, dy, d, TRAIL_WS_DOT, dot_bg);
            }
        }
    }

    // Draw caret
    if (cursor_col >= scroll_col && (int)cursor_col < (int)scroll_col + cols &&
        cursor_line >= scroll_line && (int)cursor_line < (int)scroll_line + rows) {
        int cx = CONTENT_X + (cursor_col - scroll_col) * CHAR_W;
        int cyy = cy + (cursor_line - scroll_line) * CHAR_H;
        win_draw_rect(window_handle, cx, cyy, 2, CHAR_H, CURSOR_COLOR);
    }

    // Vertical scrollbar. [no-ticket] editor uplift: track width, thumb width
    // and CONTENT_W's reservation ALL now trace to the one ed_scrollbar_w()
    // call - previously CONTENT_W reserved a hardcoded 16px while the track
    // drew at 14px and the thumb at 10px, none of which matched, which is
    // exactly the class of bug ("magic constants that can silently drift
    // apart") this pass exists to remove. Thumb is inset 2px each side,
    // matching gui_scroll.c's convention (GUI_SCROLL_W - 4).
    // DISPLAY-ONLY in this pass: no click-drag, no hit-testing. That gap
    // already existed before this change; not adding real interactivity here.
    // (edglass) The reservation is still the theme metric; the pill drawn
    // inside it is SB_TRACK_W wide and centred, track C_CARD, thumb the
    // on-glass stroke colour, both AA-blended toward the panel.
    if ((int)line_count > rows) {
        int sb_w = ed_scrollbar_w();
        int sb_x = CONTENT_RIGHT + (sb_w - SB_TRACK_W) / 2;
        gui_fill_rounded_aa(window_handle, sb_x, cy, SB_TRACK_W, ch, SB_TRACK_W / 2, SCROLL_TRACK, C_PANEL);
        int thumb_h = rows * ch / (int)line_count;
        if (thumb_h < 16) thumb_h = 16;
        int range = (int)line_count - rows;
        int thumb_y = cy + (range > 0 ? (int)scroll_line * (ch - thumb_h) / range : 0);
        gui_fill_rounded_aa(window_handle, sb_x, thumb_y, SB_TRACK_W, thumb_h, SB_TRACK_W / 2, SCROLL_THUMB, SCROLL_TRACK);
    }
}

// Draw the status row: the bottom of the body panel, under a hairline inset
// by PANEL_R so it never meets the rounded corners. Text sits on the panel
// (refilled every frame by draw_panel(), so a changed string never draws
// over its predecessor).
static void draw_status_bar(void) {
    int y = status_y();
    win_draw_rect(window_handle, body_x() + PANEL_R, y - 1, body_w() - 2 * PANEL_R, 1, C_EDGE);
    int ty = y + (STATUS_ROW_H - ED_TTF_CAPTION) / 2 - 2;
    int left_x = body_x() + PANEL_IN;
    int right_x = body_x() + body_w() - PANEL_IN;

    // Left: filename + modified flag
    char left[160];
    const char *f = filename[0] ? filename : "Untitled";
    uint32_t selstart, selend;
    if (selection_range(&selstart, &selend)) {
        snprintf(left, sizeof(left), "%s%s  [%u selected]",
                 f, modified ? " *" : "", (unsigned)(selend - selstart));
    } else {
        snprintf(left, sizeof(left), "%s%s", f, modified ? " *" : "");
    }
    win_draw_text_ttf(window_handle, left_x, ty, left, ED_TTF_CAPTION, STATUS_TEXT);

    // Center: lint cue
    char mid[64];
    mid[0] = '\0';
    if (ext_is_code()) {
        if (bracket_balance != 0) {
            snprintf(mid, sizeof(mid), "Unbalanced brackets: %d",
                     bracket_balance > 0 ? bracket_balance : -bracket_balance);
        } else {
            snprintf(mid, sizeof(mid), "Brackets balanced");
        }
    }
    // #743: a failed save outranks the lint cue. Without this the only signal
    // was the "*", which is indistinguishable from "not saved yet".
    if (save_failed)
        snprintf(mid, sizeof(mid), "SAVE FAILED - document NOT written to disk");

    if (mid[0]) {
        int mw = gui_ttf_width(mid, ED_TTF_CAPTION);
        uint32_t mc = (bracket_balance != 0 || save_failed) ? WARN_COLOR : STATUS_TEXT;
        win_draw_text_ttf(window_handle, (EDITOR_WIDTH - mw) / 2, ty, mid, ED_TTF_CAPTION, mc);
    }

    // Right: line:col + total lines
    char info[48];
    snprintf(info, sizeof(info), "Ln %u, Col %u  |  %u lines",
             (unsigned)(cursor_line + 1), (unsigned)(cursor_col + 1),
             (unsigned)line_count);
    int info_w = gui_ttf_width(info, ED_TTF_CAPTION);
    win_draw_text_ttf(window_handle, right_x - info_w, ty, info, ED_TTF_CAPTION, STATUS_TEXT);
}

// Full redraw.
//
// (edglass) THE ANTI-FLASH CONTRACT (docs/UI_GLASS_DESIGN_SYSTEM.md section
// 11). SYS_WIN_BLIT self-commits: the kernel publishes the window the
// instant the backdrop lands, and a compositor sample taken between that
// commit and the win_invalidate() below would show a backdrop with no
// document on it. So the blit runs ONLY when the chrome is dirty (start,
// EVENT_RESIZE, EVENT_REDRAW, wallpaper change, and the two layout changes
// tracked here that uncover margin), never on a keystroke, a caret move or
// a hover. Everything else is plain draws, which accumulate unpublished
// until the single invalidate at the end. The window is never cleared with
// a flat fill: the two panels cover every pixel that changes, and the
// margins are the backdrop.
static void editor_redraw(void) {
    int menu_open = g_menu.open;
    if (g_layout_find_open >= 0 && g_layout_find_open != (int)find_open) g_chrome_dirty = 1;
    if (g_layout_menu_open >= 0 && g_layout_menu_open != menu_open) g_chrome_dirty = 1;
    g_layout_find_open = (int)find_open;
    g_layout_menu_open = menu_open;

    sync_backdrop();
    if (g_chrome_dirty) {
        gui_glass_backdrop_blit(window_handle, g_bd);
        g_chrome_dirty = 0;
    }
    draw_header();
    draw_find_bar();
    draw_panel(body_x(), body_y(), body_w(), body_h());
    draw_line_numbers();
    draw_content();
    draw_status_bar();
    // LAST: overlay the open menu popup (if any) on top of everything else,
    // same overlay convention as Settings' dropdown_render().
    gui_menu_popup_draw(window_handle, &g_menu, EDITOR_WIDTH, EDITOR_HEIGHT);
    win_invalidate(window_handle);
}

// Insert a character at cursor position
static void insert_char(char c) {
    if (buffer_len >= MAX_BUFFER - 1) return;

    if (has_selection && selection_range(NULL, NULL)) {
        delete_selection();
    } else {
        clear_selection();
    }

    for (uint32_t i = buffer_len; i > cursor_pos; i--) {
        buffer[i] = buffer[i - 1];
    }

    buffer[cursor_pos] = c;
    buffer_len++;
    cursor_pos++;

    modified = true;
    recalc_lines();
    update_cursor_pos();
    ensure_visible();
}

// Delete character before cursor
static void delete_char(void) {
    if (has_selection && selection_range(NULL, NULL)) {
        delete_selection();
        return;
    }

    if (cursor_pos == 0) return;

    for (uint32_t i = cursor_pos - 1; i < buffer_len - 1; i++) {
        buffer[i] = buffer[i + 1];
    }

    buffer_len--;
    cursor_pos--;

    modified = true;
    recalc_lines();
    update_cursor_pos();
    ensure_visible();
}

// Delete selection
static void delete_selection(void) {
    uint32_t start, end;
    if (!selection_range(&start, &end)) { clear_selection(); return; }
    uint32_t len = end - start;

    for (uint32_t i = start; i < buffer_len - len; i++) {
        buffer[i] = buffer[i + len];
    }

    buffer_len -= len;
    cursor_pos = start;
    clear_selection();

    modified = true;
    recalc_lines();
    update_cursor_pos();
    ensure_visible();
}

// Clipboard: copy / cut / paste on the active selection.
static void clip_copy(void) {
    uint32_t start, end;
    if (!selection_range(&start, &end)) return;
    uint32_t len = end - start;
    if (len >= MAX_CLIP) len = MAX_CLIP - 1;
    for (uint32_t i = 0; i < len; i++) clipboard[i] = buffer[start + i];
    clip_len = len;
}
static void clip_cut(void) {
    clip_copy();
    delete_selection();
}
static void clip_paste(void) {
    if (clip_len == 0) return;
    if (has_selection && selection_range(NULL, NULL)) delete_selection();
    if (buffer_len + clip_len >= MAX_BUFFER - 1) return;
    for (uint32_t i = buffer_len; i > cursor_pos; i--) {
        buffer[i - 1 + clip_len] = buffer[i - 1];
    }
    for (uint32_t i = 0; i < clip_len; i++) buffer[cursor_pos + i] = clipboard[i];
    buffer_len += clip_len;
    cursor_pos += clip_len;
    modified = true;
    clear_selection();
    recalc_lines();
    update_cursor_pos();
    ensure_visible();
}

// Select all
static void select_all(void) {
    sel_anchor = 0;
    cursor_pos = buffer_len;
    has_selection = (buffer_len > 0);
    update_cursor_pos();
    ensure_visible();
}

// Cursor movement (extend keeps/extends the selection anchor)
static void move_left(bool extend) {
    start_or_keep_selection(extend);
    if (cursor_pos > 0) cursor_pos--;
    update_cursor_pos();
    ensure_visible();
}
static void move_right(bool extend) {
    start_or_keep_selection(extend);
    if (cursor_pos < buffer_len) cursor_pos++;
    update_cursor_pos();
    ensure_visible();
}
static void move_up(bool extend) {
    start_or_keep_selection(extend);
    if (cursor_line > 0) {
        uint32_t new_line = cursor_line - 1;
        uint32_t new_line_len = get_line_length(new_line);
        uint32_t new_col = cursor_col < new_line_len ? cursor_col : new_line_len;
        cursor_pos = get_line_start(new_line) + new_col;
    }
    update_cursor_pos();
    ensure_visible();
}
static void move_down(bool extend) {
    start_or_keep_selection(extend);
    if (cursor_line < line_count - 1) {
        uint32_t new_line = cursor_line + 1;
        uint32_t new_line_len = get_line_length(new_line);
        uint32_t new_col = cursor_col < new_line_len ? cursor_col : new_line_len;
        cursor_pos = get_line_start(new_line) + new_col;
    }
    update_cursor_pos();
    ensure_visible();
}
static void move_home(bool extend) {
    start_or_keep_selection(extend);
    cursor_pos = get_line_start(cursor_line);
    update_cursor_pos();
    ensure_visible();
}
static void move_end(bool extend) {
    start_or_keep_selection(extend);
    cursor_pos = get_line_start(cursor_line) + get_line_length(cursor_line);
    update_cursor_pos();
    ensure_visible();
}

// New file
static void file_new(void) {
    buffer_len = 0;
    buffer[0] = '\0';
    cursor_pos = 0;
    filename[0] = '\0';
    modified = false;
    clear_selection();
    scroll_line = 0;
    scroll_col = 0;
    recalc_lines();
    update_cursor_pos();
    recompute_lint();
}

// Load file
static void file_open(const char *path) {
    int fd = open(path, 0);  // O_RDONLY
    if (fd < 0) return;

    buffer_len = 0;
    long n;
    while ((n = read(fd, buffer + buffer_len, MAX_BUFFER - buffer_len - 1)) > 0) {
        buffer_len += n;
    }
    buffer[buffer_len] = '\0';

    close(fd);

    int i = 0;
    while (path[i] && i < MAX_PATH - 1) {
        filename[i] = path[i];
        i++;
    }
    filename[i] = '\0';

    cursor_pos = 0;
    modified = false;
    clear_selection();
    scroll_line = 0;
    scroll_col = 0;
    recalc_lines();
    update_cursor_pos();
    recompute_lint();
}

// Save file
static void file_save(void) {
    if (!filename[0]) return;  // Need filename

    // #743: THE DATA-LOSS SITE. This was:
    //
    //     int fd = open(filename, 1);   // O_WRONLY, no O_CREAT, no O_TRUNC
    //     if (fd < 0) return;
    //     write(fd, buffer, buffer_len);   // result discarded
    //     close(fd);                       // result discarded
    //     modified = false;                // UNCONDITIONAL
    //
    // Four separate faults, and the last one is what turns the others into lost
    // work:
    //
    //  1. `modified = false` ran whatever happened. The status bar's "*" is the
    //     only thing telling the user there is unsaved work, so a save that
    //     failed looked EXACTLY like one that succeeded. The user then closes
    //     the editor and the document is gone. No hardware fault is needed: a
    //     full volume is enough.
    //
    //  2. close() was discarded, which on this kernel is the important one. An
    //     ext2-backed fd buffers the bytes and does the REAL write inside
    //     close() (kernel/proc/syscall.c: the e2fd family calls
    //     ext2_write_file() from sys_close and returns its rc). So the one call
    //     that could report "the disk is full" was the one being ignored.
    //
    //  3. No O_CREAT: saving to a filename that does not exist yet failed at
    //     the open and returned silently.
    //
    //  4. No O_TRUNC. On the ext2 root this happens to be harmless, because the
    //     ext2 write path replaces the whole file anyway; on a FAT-backed path
    //     the write is POSITIONAL, so saving a shorter document over a longer
    //     one left the tail of the old one behind and the file kept its old
    //     length. Stated as measured: filesystem-dependent, not universal.
    //
    // userconf_write_all() supplies O_CREAT|O_TRUNC, writes every byte, fsyncs,
    // and checks the close, returning 0 only if the bytes actually landed.
    if (userconf_write_all(filename, buffer, buffer_len) != 0) {
        // Keep `modified` TRUE. The document stays marked unsaved, the "*"
        // stays in the status bar, and the buffer is untouched, so the user can
        // fix the problem and press Ctrl+S again.
        save_failed = true;
        return;
    }

    save_failed = false;
    modified = false;
}

// Convert a screen click to a buffer position in the content area.
static bool click_to_pos(int local_x, int local_y, uint32_t *out_pos) {
    int cy = content_y();
    int ch = content_h();
    if (local_x < CONTENT_X || local_y < cy || local_y >= cy + ch) return false;
    uint32_t click_col = (local_x - CONTENT_X) / CHAR_W + scroll_col;
    uint32_t click_row = (local_y - cy) / CHAR_H + scroll_line;
    if (click_row >= line_count) click_row = line_count - 1;
    uint32_t line_len = get_line_length(click_row);
    if (click_col > line_len) click_col = line_len;
    *out_pos = get_line_start(click_row) + click_col;
    return true;
}

// Typing into a find/replace field. Returns true if consumed.
static bool find_bar_key(gui_event_t *ev) {
    char c = ev->key_char;
    uint32_t kc = ev->keycode;

    char *txt = (find_field == 0) ? find_text : repl_text;
    uint32_t *len = (find_field == 0) ? &find_len : &repl_len;

    if (c == 27) {                       // ESC closes the bar
        find_open = false; replace_mode = false;
        return true;
    }
    if (kc == 0x0F) {                    // Tab toggles fields when replace mode
        if (replace_mode) find_field ^= 1;
        return true;
    }
    if (c == '\b' || kc == 0x0E) {       // Backspace
        if (*len > 0) { (*len)--; txt[*len] = '\0'; }
        if (find_field == 0) { have_cur_match = false; recompute_matches(); }
        return true;
    }
    if (kc == 0x1C || c == '\n' || c == '\r') {  // Enter
        if (replace_mode && find_field == 1) {
            // Ctrl+Enter handled by caller via ctrl flag; plain Enter = replace one
            replace_current();
            find_next(cursor_pos, true);
        } else {
            find_next(have_cur_match ? cur_match_pos + 1 : cursor_pos, true);
        }
        return true;
    }
    if (c >= ' ' && c < 127) {
        if (*len < MAX_FIND - 1) { txt[*len] = c; (*len)++; txt[*len] = '\0'; }
        if (find_field == 0) { have_cur_match = false; find_next(0, true); }
        return true;
    }
    return false;
}

// Dispatch a menu_action_id_t returned by gui_menu_bar_click()/gui_menu_key().
// Every action here already existed as a keyboard shortcut (Ctrl+N/S/X/C/V/A/
// F/G/H) or the Font-menu click; the menu is a second way to reach the same
// code, not new behaviour.
static void handle_menu_action(int id, int *running) {
    switch (id) {
        case ID_FILE_NEW:
            file_new(); kbd_sel_mode = false;
            break;
        case ID_FILE_SAVE:
            file_save();
            break;
        case ID_FILE_EXIT:
            *running = 0;
            break;
        case ID_EDIT_CUT:
            clip_cut(); recompute_lint();
            break;
        case ID_EDIT_COPY:
            clip_copy();
            break;
        case ID_EDIT_PASTE:
            clip_paste(); recompute_lint();
            break;
        case ID_EDIT_SELECT_ALL:
            select_all();
            break;
        case ID_SEARCH_FIND:
            find_open = true; replace_mode = false; find_field = 0;
            have_cur_match = false; recompute_matches();
            break;
        case ID_SEARCH_FIND_NEXT:
            if (find_len > 0) find_next(have_cur_match ? cur_match_pos + 1 : cursor_pos, true);
            break;
        case ID_SEARCH_REPLACE:
            find_open = true; replace_mode = true; find_field = 0;
            have_cur_match = false; recompute_matches();
            break;
        case ID_FONT_CHOOSE:
            // Font menu: the SHARED picker (#351). The editor owns no font UI;
            // it hands its current selection in and takes the new one back.
            g_font.title = "Editor Font";
            g_font.preview_text = "int main(void) { return 0; }";
            if (gui_font_dialog(&g_font)) {
                ed_apply_font();
                ed_save_font();
            }
            break;
        default:
            break;
    }
}

int main(int argc, char **argv) {
    (void)argc;

    // g_ed_w/g_ed_h are the CONTENT size (that is what EVENT_RESIZE delivers and
    // what every layout helper below assumes), so add the chrome to get the OUTER
    // size win_create() wants. Passing the content size directly, as this did,
    // made the kernel carve the chrome back out of it and silently clipped the
    // bottom of the layout.
    window_handle = win_create("Editor", win_x, win_y,
                               EDITOR_WIDTH + ED_CHROME_W, EDITOR_HEIGHT + ED_CHROME_H);
    ed_load_font();   // #351: restore the saved face before the first paint
    if (window_handle < 0) {
        return 1;
    }

    // Authoritative canvas size: ask the kernel what the canvas actually is rather
    // than inferring it from the create arguments (Settings does the same). This
    // keeps g_ed_w/g_ed_h honest even if the kernel clamps the window.
    {
        int cw = 0, chh = 0;
        if (win_get_size(window_handle, &cw, &chh) == 0 && cw > 0 && chh > 0) {
            g_ed_w = cw; g_ed_h = chh;
        }
    }

    printf("Editor window created (handle=%d)\n", window_handle);

    // (edglass) The menu bar lives inside the header panel: 6px in from the
    // panel's left edge, 5px down, MENU_BAR_H tall (the glass TAB_H).
    gui_menu_bar_init(&g_menu, EDITOR_MENUS, EDITOR_MENU_COUNT, PAD + 6, PAD + 5, MENU_BAR_H);
    gui_menu_sync_theme(&g_menu, -1);   // metrics from the theme...
    apply_menu_palette();               // ...colours from the glass tokens
    g_theme_last = get_theme();

    file_new();

    if (argc > 1 && argv[1]) {
        file_open(argv[1]);
    }

    editor_redraw();

    gui_event_t event;
    int running = 1;

    while (running) {
        int event_type = win_get_event(window_handle, &event, 100);

        // Live-apply a theme change made in Settings while we are running.
        // (edglass) Metrics only: the menu bar's type size / row height and
        // the scrollbar reservation follow the theme; the colours are the
        // fixed glass palette, re-applied over the widget's theme read.
        {
            int th = get_theme();
            if (th != g_theme_last) {
                g_theme_last = th;
                gui_menu_sync_theme(&g_menu, -1);
                apply_menu_palette();
                g_chrome_dirty = 1;
                editor_redraw();
            }
        }

        if (event_type == 0) {
            // Idle healing for the menu bar: the window lost focus, so drop
            // any open menu and any hover highlight. A window stops receiving
            // EVENT_MOUSE_MOVE the instant the pointer crosses into another
            // window, so the hover state freezes at the edge and the bar goes
            // on drawing a hovered label indefinitely.
            //
            // THIS KERNEL EMITS NO FOCUS OR BLUR EVENT TO AN APP.
            // EVENT_WINDOW_BLUR appears in the tree only as an enum
            // declaration; kernel/proc/syscall.h says so at SYS_KEY_MODS and
            // again at SYS_WIN_GET_STATE. An earlier version of this fix
            // handled EVENT_WINDOW_BLUR and was DEAD CODE, caught by running
            // it and not by reading it. SYS_WIN_GET_STATE (#221) is the
            // purpose-built replacement: it reports the caller's OWN window
            // state, focus included. Checked here on the existing 100 ms idle
            // branch, beside the theme poll that already uses this point.
            if (gui_menu_is_open(&g_menu) || g_menu.hot_top >= 0) {
                int st = win_get_state(window_handle);
                if (st >= 0 && !(st & WIN_STATE_FOCUSED)) {
                    gui_menu_leave(&g_menu);
                    editor_redraw();
                }
            }
            continue;
        }

        // Refresh window position for coordinate conversion.
        win_get_pos(window_handle, &win_x, &win_y);

        switch (event.type) {
            case EVENT_REDRAW:
                // (edglass) the compositor asked for the whole window again:
                // chrome dirty, so the backdrop is blitted before the panels.
                g_chrome_dirty = 1;
                editor_redraw();
                break;

            case EVENT_RESIZE:
                // (edglass) a resize reallocates the content buffer, so the
                // backdrop must be blitted again: chrome dirty.
                if (event.mouse_x > 0 && event.mouse_y > 0) { g_ed_w = event.mouse_x; g_ed_h = event.mouse_y; }
                g_chrome_dirty = 1;
                editor_redraw();
                break;

            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;

            case EVENT_KEY_DOWN:
                {
                    char c = event.key_char;
                    uint32_t keycode = event.keycode;
                    bool ext = kbd_sel_mode;   // extend selection in keyboard-select mode
                    bool handled = false;

                    // An open menu owns the keyboard: Esc/arrows/Enter navigate
                    // it, and every other key is swallowed rather than typed
                    // into the document underneath.
                    if (gui_menu_is_open(&g_menu)) {
                        int mid = gui_menu_key(&g_menu, keycode, c);
                        if (mid >= 0) handle_menu_action(mid, &running);
                        editor_redraw();
                        break;
                    }

                    // --- find/replace bar focus eats most keys ----------------
                    if (find_open) {
                        // Ctrl+Enter (find_field==1) replaces all; key_char 10 on
                        // some layouts. Otherwise route to the field editor.
                        if (replace_mode && (c == 1)) {           // Ctrl+A => replace all
                            replace_all();
                            editor_redraw();
                            break;
                        }
                        if (find_bar_key(&event)) {
                            editor_redraw();
                            break;
                        }
                        // fall through for Ctrl+F/Ctrl+H toggles below
                    }

                    // --- global Ctrl shortcuts --------------------------------
                    if (c == 6) {                       // Ctrl+F - find
                        find_open = true; replace_mode = false; find_field = 0;
                        have_cur_match = false; recompute_matches();
                        handled = true;
                    }
                    else if (c == 8 && keycode != 0x0E) {  // Ctrl+H - replace
                        find_open = true; replace_mode = true; find_field = 0;
                        have_cur_match = false; recompute_matches();
                        handled = true;
                    }
                    else if (c == 7) {                  // Ctrl+G - find next
                        if (find_len > 0) find_next(have_cur_match ? cur_match_pos + 1 : cursor_pos, true);
                        handled = true;
                    }
                    else if (c == 1) {                  // Ctrl+A - select all
                        select_all(); handled = true;
                    }
                    else if (c == 3) {                  // Ctrl+C - copy
                        clip_copy(); handled = true;
                    }
                    else if (c == 24) {                 // Ctrl+X - cut
                        clip_cut(); recompute_lint(); handled = true;
                    }
                    else if (c == 22) {                 // Ctrl+V - paste
                        clip_paste(); recompute_lint(); handled = true;
                    }
                    else if (c == 19) {                 // Ctrl+S - save
                        file_save(); handled = true;
                    }
                    else if (c == 14) {                 // Ctrl+N - new
                        file_new(); kbd_sel_mode = false; handled = true;
                    }
                    else if (c == 0) {                  // Ctrl+Space toggles kbd selection mode
                        // Many layouts deliver Ctrl+Space as NUL.
                        kbd_sel_mode = !kbd_sel_mode;
                        if (kbd_sel_mode) { sel_anchor = cursor_pos; has_selection = false; }
                        else clear_selection();
                        handled = true;
                    }
                    // --- editing / navigation ---------------------------------
                    else if (c == 27) {                 // ESC
                        if (kbd_sel_mode) { kbd_sel_mode = false; clear_selection(); }
                        else running = 0;
                        handled = true;
                    }
                    else if (keycode == 0x1C || c == '\n' || c == '\r') {
                        insert_char('\n'); recompute_lint(); kbd_sel_mode = false; handled = true;
                    }
                    else if (c == '\b' || keycode == 0x0E) {
                        delete_char(); recompute_lint(); kbd_sel_mode = false; handled = true;
                    }
                    else if (keycode == GUI_KEY_DEL) {  // Delete key
                        if (has_selection && selection_range(NULL, NULL)) {
                            delete_selection();
                        } else if (cursor_pos < buffer_len) {
                            cursor_pos++;
                            delete_char();
                        }
                        recompute_lint(); kbd_sel_mode = false; handled = true;
                    }
                    else if (keycode == 0x82) { move_left(ext); recompute_lint(); handled = true; }
                    else if (keycode == 0x83) { move_right(ext); recompute_lint(); handled = true; }
                    else if (keycode == 0x80) { move_up(ext); recompute_lint(); handled = true; }
                    else if (keycode == 0x81) { move_down(ext); recompute_lint(); handled = true; }
                    else if (keycode == GUI_KEY_HOME) { move_home(ext); recompute_lint(); handled = true; }
                    else if (keycode == GUI_KEY_END) { move_end(ext); recompute_lint(); handled = true; }
                    else if (keycode == GUI_KEY_PGUP) { // Page Up
                        int r = visible_rows();
                        for (int i = 0; i < r && cursor_line > 0; i++) move_up(ext);
                        handled = true;
                    }
                    else if (keycode == GUI_KEY_PGDN) { // Page Down
                        int r = visible_rows();
                        for (int i = 0; i < r && cursor_line < line_count - 1; i++) move_down(ext);
                        handled = true;
                    }
                    else if (c == '\t') {
                        for (int i = 0; i < 4; i++) insert_char(' ');
                        recompute_lint(); kbd_sel_mode = false; handled = true;
                    }
                    else if (c >= ' ' && c < 127) {     // printable
                        insert_char(c); recompute_lint(); kbd_sel_mode = false; handled = true;
                    }

                    (void)handled;
                    editor_redraw();
                }
                break;

            case EVENT_MOUSE_DOWN:
                {
                    // Already content-relative (kernel b330). Subtracting the
                    // window origin here again double-offsets every click.
                    int local_x = event.mouse_x;
                    int local_y = event.mouse_y;

                    // The menu bar (gui_menu, #562/#512) gets first refusal on
                    // every click, open or closed: an open popup can extend
                    // well below the header panel, over the find row or content.
                    int mid = gui_menu_bar_click(&g_menu, local_x, local_y,
                                                 EDITOR_WIDTH, EDITOR_HEIGHT);
                    if (mid != -1) {
                        if (mid >= 0) handle_menu_action(mid, &running);
                        editor_redraw();
                        break;
                    }

                    // Find row (prev/next buttons, field focus) when open. The
                    // rects are the fr_*() ones draw_find_bar() paints.
                    if (find_open && local_y >= fr_y() && local_y < fr_y() + FIND_ROW_H) {
                        int cy0 = fr_ctl_y(), cy1 = fr_ctl_y() + FR_FIELD_H;
                        bool in_row = (local_y >= cy0 && local_y < cy1);
                        if (in_row && local_x >= fr_prev_x() && local_x < fr_prev_x() + FR_BTN_W) {
                            find_next(have_cur_match ? (cur_match_pos > 0 ? cur_match_pos : 0) : cursor_pos, false);
                        } else if (in_row && local_x >= fr_next_x() && local_x < fr_next_x() + FR_BTN_W) {
                            find_next(have_cur_match ? cur_match_pos + 1 : cursor_pos, true);
                        } else if (in_row && local_x >= fr_find_x() && local_x < fr_find_x() + FR_FIND_W) {
                            find_field = 0;
                        } else if (in_row && replace_mode && local_x >= fr_repl_x() &&
                                   local_x < fr_repl_x() + fr_repl_w()) {
                            find_field = 1;
                        }
                        editor_redraw();
                        break;
                    }

                    if (local_y < body_y()) {
                        // gui_menu_bar_click() above already owns every top-level
                        // label; a click here landed in the header panel's blank
                        // space or the margin and is deliberately a no-op.
                    } else {
                        uint32_t pos;
                        if (click_to_pos(local_x, local_y, &pos)) {
                            cursor_pos = pos;
                            sel_anchor = pos;
                            has_selection = false;
                            mouse_selecting = true;
                            update_cursor_pos();
                            recompute_lint();
                            editor_redraw();
                        }
                    }
                }
                break;

            case EVENT_MOUSE_MOVE:
                {
                    int local_x = event.mouse_x;
                    int local_y = event.mouse_y;
                    if (gui_menu_is_open(&g_menu)) {
                        if (gui_menu_motion(&g_menu, local_x, local_y, EDITOR_WIDTH, EDITOR_HEIGHT))
                            editor_redraw();
                        break;
                    }
                    if (mouse_selecting) {
                        uint32_t pos;
                        if (click_to_pos(local_x, local_y, &pos)) {
                            cursor_pos = pos;
                            has_selection = (pos != sel_anchor);
                            update_cursor_pos();
                            ensure_visible();
                            editor_redraw();
                        }
                    }
                }
                break;

            case EVENT_MOUSE_UP:
                gui_menu_release(&g_menu);
                mouse_selecting = false;
                break;

            case EVENT_MOUSE_SCROLL:
                if (gui_menu_is_open(&g_menu)) {
                    if (gui_menu_wheel(&g_menu, event.mouse_x, event.mouse_y,
                                       EDITOR_WIDTH, EDITOR_HEIGHT, event.scroll_delta))
                        editor_redraw();
                    break;
                }
                {
                    int delta = event.scroll_delta;
                    int rows = visible_rows();
                    if (delta < 0 && scroll_line > 0) {
                        scroll_line--;
                        editor_redraw();
                    } else if (delta > 0 && (int)scroll_line + rows < (int)line_count) {
                        scroll_line++;
                        editor_redraw();
                    }
                }
                break;

            default:
                break;
        }
    }

    win_destroy(window_handle);
    printf("Editor closed\n");

    return 0;
}
