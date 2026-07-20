// editor - Text Editor for MayteraOS (user-space version)
// #272 uplift: line-number gutter, find/replace, rich status bar, multi-line
// selection (keyboard + mouse drag), clipboard, and lightweight linting cues.
// Existing open/save behaviour is preserved.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_font.h"

// Editor dimensions
static int g_ed_w = 640, g_ed_h = 480;  // live content size (EVENT_RESIZE)
#define EDITOR_WIDTH    g_ed_w
#define EDITOR_HEIGHT   g_ed_h
// Cell metrics, derived from the SELECTED font at runtime (#351). These were
// #define 8 / 16, hardwired to the 8x16 bitmap font, which is what made the
// editor unable to honour a font choice at all. ed_apply_font() recomputes them.
static int g_cell_w = 8;
static int g_cell_h = 16;
#define CHAR_W          g_cell_w
#define CHAR_H          g_cell_h
#define MENU_HEIGHT     24
#define FIND_HEIGHT     26      // search bar height (only when open)
#define STATUS_HEIGHT   20
#define LINE_NUM_WIDTH  48      // Space for line numbers

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

// Title-bar inset used for screen->local coordinate conversion.
#define TITLEBAR_INSET  22
#define BORDER_INSET    2

// Content area dimensions (recomputed per-frame so the find bar can push it down)
#define CONTENT_X       LINE_NUM_WIDTH
#define CONTENT_W       (EDITOR_WIDTH - LINE_NUM_WIDTH - 16)  // -16 for scrollbar

// Buffer limits
#define MAX_BUFFER      (64 * 1024)  // 64KB text buffer
#define MAX_LINES       4096
#define MAX_PATH        256
#define MAX_FIND        128
#define MAX_CLIP        (16 * 1024)

// Colors (theme-driven at runtime; see apply_theme()). Dark defaults shown.
static uint32_t BG_COLOR       = 0x001E1E1E;  // text content background
static uint32_t TEXT_COLOR     = 0x00D4D4D4;  // body text
static uint32_t LINE_NUM_COLOR = 0x00858585;  // gutter line numbers (dim)
static uint32_t LINE_NUM_BG    = 0x00252525;  // gutter/panel background
static uint32_t SELECTION_BG   = 0x00264F78;  // selection highlight
static uint32_t CURSOR_COLOR   = 0x00AEAFAD;  // caret
static uint32_t MENU_BG        = 0x00333333;  // menu bar / find bar panel
static uint32_t MENU_TEXT      = 0x00FFFFFF;  // menu / button text
static uint32_t MENU_HINT      = 0x00AAAAAA;  // secondary/dim text on chrome
static uint32_t STATUS_BG      = 0x00007ACC;  // status bar / focus accent
static uint32_t STATUS_TEXT    = 0x00FFFFFF;  // status bar text
static uint32_t FIND_BG        = 0x002D2D30;  // find bar background
static uint32_t FIND_FIELD_BG  = 0x003C3C3C;  // input field background
static uint32_t FIND_BORDER    = 0x00505050;  // field border / scrollbar thumb
static uint32_t BUTTON_BG      = 0x00505050;  // find-bar prev/next buttons
static uint32_t SCROLL_TRACK   = 0x00303030;  // scrollbar track
static uint32_t SCROLL_THUMB   = 0x00686868;  // scrollbar thumb
static uint32_t CUR_LINE_BG    = 0x002A2D2E;  // current-line highlight
static uint32_t MATCH_BG       = 0x00504000;  // highlight for all matches
static uint32_t MATCH_CUR_BG   = 0x00B58900;  // highlight for the active match
static uint32_t TRAIL_WS_BG    = 0x00552222;  // trailing-whitespace flag
static uint32_t BRACKET_BG     = 0x00405E40;  // matched-bracket highlight
static uint32_t WARN_COLOR     = 0x00FFCC00;  // unbalanced-bracket warning

// Live theme tracking. get_theme(): 1=Dark 2=Light 4=Classic 5=Ocean 9=Nord.
static int g_theme_last = -1;

static void apply_theme(void) {
    int kt = get_theme();
    switch (kt) {
        case 2:  // Light
            BG_COLOR=0x00FFFFFF; TEXT_COLOR=0x00202020; LINE_NUM_COLOR=0x00999999;
            LINE_NUM_BG=0x00F0F0F0; SELECTION_BG=0x00D6E4FB; CURSOR_COLOR=0x00202020;
            MENU_BG=0x00F0F0F0; MENU_TEXT=0x00202020; MENU_HINT=0x00606060;
            STATUS_BG=0x002D6CDF; STATUS_TEXT=0x00FFFFFF; FIND_BG=0x00F8F8F8;
            FIND_FIELD_BG=0x00FFFFFF; FIND_BORDER=0x00CCCCCC; BUTTON_BG=0x00E0E0E0;
            SCROLL_TRACK=0x00E8E8E8; SCROLL_THUMB=0x00BBBBBB; CUR_LINE_BG=0x00EEF3FC;
            break;
        case 4:  // Classic
            BG_COLOR=0x00FFFFFF; TEXT_COLOR=0x00000000; LINE_NUM_COLOR=0x00808080;
            LINE_NUM_BG=0x00C0C0C0; SELECTION_BG=0x00000080; CURSOR_COLOR=0x00000000;
            MENU_BG=0x00C0C0C0; MENU_TEXT=0x00000000; MENU_HINT=0x00404040;
            STATUS_BG=0x00000080; STATUS_TEXT=0x00FFFFFF; FIND_BG=0x00C0C0C0;
            FIND_FIELD_BG=0x00FFFFFF; FIND_BORDER=0x00000000; BUTTON_BG=0x00D0D0D0;
            SCROLL_TRACK=0x00C0C0C0; SCROLL_THUMB=0x00808080; CUR_LINE_BG=0x00E8E8E8;
            break;
        case 5:  // Ocean
            BG_COLOR=0x00183040; TEXT_COLOR=0x00E0F0FF; LINE_NUM_COLOR=0x00607080;
            LINE_NUM_BG=0x001A3A4A; SELECTION_BG=0x00305060; CURSOR_COLOR=0x0040C0E0;
            MENU_BG=0x001E4050; MENU_TEXT=0x00E0F0FF; MENU_HINT=0x0090B0C0;
            STATUS_BG=0x0040C0E0; STATUS_TEXT=0x00183040; FIND_BG=0x001E4050;
            FIND_FIELD_BG=0x00183040; FIND_BORDER=0x00406070; BUTTON_BG=0x00254555;
            SCROLL_TRACK=0x001A3A4A; SCROLL_THUMB=0x00406070; CUR_LINE_BG=0x00224455;
            break;
        case 9:  // Nord
            BG_COLOR=0x002B303B; TEXT_COLOR=0x00ECEFF4; LINE_NUM_COLOR=0x00707A8C;
            LINE_NUM_BG=0x002E3440; SELECTION_BG=0x00434C5E; CURSOR_COLOR=0x0088C0D0;
            MENU_BG=0x00343B49; MENU_TEXT=0x00ECEFF4; MENU_HINT=0x00AEB6C5;
            STATUS_BG=0x0088C0D0; STATUS_TEXT=0x002B303B; FIND_BG=0x00343B49;
            FIND_FIELD_BG=0x002B303B; FIND_BORDER=0x004C566A; BUTTON_BG=0x00343B49;
            SCROLL_TRACK=0x002E3440; SCROLL_THUMB=0x004C566A; CUR_LINE_BG=0x003B4252;
            break;
        default: // Dark
            BG_COLOR=0x001E1E1E; TEXT_COLOR=0x00D4D4D4; LINE_NUM_COLOR=0x00858585;
            LINE_NUM_BG=0x00252525; SELECTION_BG=0x00264F78; CURSOR_COLOR=0x00AEAFAD;
            MENU_BG=0x00333333; MENU_TEXT=0x00FFFFFF; MENU_HINT=0x00AAAAAA;
            STATUS_BG=0x00007ACC; STATUS_TEXT=0x00FFFFFF; FIND_BG=0x002D2D30;
            FIND_FIELD_BG=0x003C3C3C; FIND_BORDER=0x00505050; BUTTON_BG=0x00505050;
            SCROLL_TRACK=0x00303030; SCROLL_THUMB=0x00686868; CUR_LINE_BG=0x002A2D2E;
            break;
    }
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
// Keyboard selection mode. The kernel gui_event_t has no shift/modifier field,
// so shift+arrow cannot be detected from userland. Ctrl+Space toggles a
// selection mode in which arrow/home/end/pgup/pgdn keys extend the selection.
static bool kbd_sel_mode = false;

// Clipboard
static char clipboard[MAX_CLIP];
static uint32_t clip_len = 0;

// File state
static char filename[MAX_PATH] = "";
static bool modified = false;

// Find/Replace state
static bool find_open = false;
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
static int content_y(void) {
    return MENU_HEIGHT + (find_open ? FIND_HEIGHT : 0);
}
static int content_h(void) {
    return EDITOR_HEIGHT - content_y() - STATUS_HEIGHT - TITLEBAR_INSET;
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

static void ed_save_font(void) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s|%s|%d\n",
             g_font.family[0] ? g_font.family : "Default",
             g_font.style[0] ? g_font.style : "Regular", g_font.size);
    int fd = open(ED_FONT_CFG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    write(fd, buf, strlen(buf));
    close(fd);
}

static void ed_load_font(void) {
    memset(&g_font, 0, sizeof(g_font));
    g_font.size = 16;
    strncpy(g_font.style, "Regular", GUI_FONT_STYLE_MAX - 1);
    int fd = open(ED_FONT_CFG, O_RDONLY);
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

// Draw the menu bar
static void draw_menu_bar(void) {
    win_draw_rect(window_handle, 0, 0, EDITOR_WIDTH, MENU_HEIGHT, MENU_BG);

    const char *menus[] = { "File", "Edit", "Search", "Font" };
    int menu_x[] = { 8, 56, 112, 176 };
    int menu_w[] = { 40, 48, 56, 44 };

    for (int i = 0; i < 4; i++) {
        win_draw_rect(window_handle, menu_x[i] - 4, 2, menu_w[i], MENU_HEIGHT - 4, MENU_BG);
        win_draw_text(window_handle, menu_x[i], 4, menus[i], MENU_TEXT);
    }

    // Compact hint on the right.
    const char *hint = "Ctrl+F Find  Ctrl+H Replace  Ctrl+S Save";
    int hw = gui_string_width(hint);
    win_draw_text_small(window_handle, EDITOR_WIDTH - hw / 2 - 12, 7, hint, MENU_HINT);
}

// Draw the find / replace bar
static void draw_find_bar(void) {
    if (!find_open) return;
    int y = MENU_HEIGHT;
    win_draw_rect(window_handle, 0, y, EDITOR_WIDTH, FIND_HEIGHT, FIND_BG);

    // Label
    win_draw_text(window_handle, 6, y + 5, "Find:", MENU_TEXT);

    // Find field
    int fx = 56, fw = 200, fh = 18;
    uint32_t fb = (find_field == 0) ? gui_lighten(FIND_FIELD_BG, 16) : FIND_FIELD_BG;
    win_draw_rect(window_handle, fx, y + 4, fw, fh, fb);
    gui_draw_rect_outline(window_handle, fx, y + 4, fw, fh,
                          (find_field == 0) ? STATUS_BG : FIND_BORDER);
    win_draw_text(window_handle, fx + 4, y + 5, find_text, TEXT_COLOR);
    if (find_field == 0) {
        int cx = fx + 4 + gui_string_width(find_text);
        win_draw_rect(window_handle, cx, y + 5, 1, CHAR_H - 2, CURSOR_COLOR);
    }

    // Match count badge
    char mc[40];
    if (find_len == 0) {
        snprintf(mc, sizeof(mc), "no query");
    } else if (match_count == 0) {
        snprintf(mc, sizeof(mc), "0 matches");
    } else {
        snprintf(mc, sizeof(mc), "%d of %d", match_index, match_count);
    }
    win_draw_text(window_handle, fx + fw + 10, y + 5, mc, MENU_HINT);

    // Prev / Next buttons
    int bx = fx + fw + 110;
    win_draw_rect(window_handle, bx, y + 4, 24, fh, BUTTON_BG);
    win_draw_text(window_handle, bx + 8, y + 5, "<", MENU_TEXT);
    win_draw_rect(window_handle, bx + 28, y + 4, 24, fh, BUTTON_BG);
    win_draw_text(window_handle, bx + 36, y + 5, ">", MENU_TEXT);

    if (replace_mode) {
        // Replace row is drawn within the same bar height by re-using vertical
        // space is tight, so we render a second compact line just below the find
        // field overlapping the content top is avoided because content_y already
        // accounts for FIND_HEIGHT; for replace we expand visually via a popup
        // line at the bottom of the bar.
        // Simpler: show replace field to the right.
        int rx = bx + 60;
        win_draw_text(window_handle, rx, y + 5, "Repl:", MENU_TEXT);
        int rfx = rx + 44, rfw = 150;
        uint32_t rb = (find_field == 1) ? gui_lighten(FIND_FIELD_BG, 16) : FIND_FIELD_BG;
        win_draw_rect(window_handle, rfx, y + 4, rfw, fh, rb);
        gui_draw_rect_outline(window_handle, rfx, y + 4, rfw, fh,
                              (find_field == 1) ? STATUS_BG : FIND_BORDER);
        win_draw_text(window_handle, rfx + 4, y + 5, repl_text, TEXT_COLOR);
        if (find_field == 1) {
            int cx = rfx + 4 + gui_string_width(repl_text);
            win_draw_rect(window_handle, cx, y + 5, 1, CHAR_H - 2, CURSOR_COLOR);
        }
        // Replace hint
        win_draw_text_small(window_handle, rfx + rfw + 6, y + 8,
                            "Enter=Repl  Ctrl+Enter=All", MENU_HINT);
    }
}

// Draw line numbers
static void draw_line_numbers(void) {
    int cy = content_y();
    int ch = content_h();
    win_draw_rect(window_handle, 0, cy, LINE_NUM_WIDTH, ch, LINE_NUM_BG);

    char num_str[8];
    int rows = visible_rows();
    for (int row = 0; row < rows && scroll_line + (uint32_t)row < line_count; row++) {
        int line_num = scroll_line + row + 1;
        int y = cy + row * CHAR_H;

        gui_itoa(line_num, num_str, 8);

        int text_w = gui_string_width(num_str);
        int x = LINE_NUM_WIDTH - text_w - 8;

        uint32_t color = (scroll_line + (uint32_t)row == cursor_line) ? TEXT_COLOR : LINE_NUM_COLOR;
        win_draw_text(window_handle, x, y, num_str, color);
    }
}

// Draw the text content
static void draw_content(void) {
    int cy = content_y();
    int ch = content_h();
    int rows = visible_rows();
    int cols = visible_cols();

    win_draw_rect(window_handle, CONTENT_X, cy, CONTENT_W, ch, BG_COLOR);

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

            // Cell background priority: selection > active match > matches >
            // bracket-match > trailing whitespace.
            uint32_t cell_bg = 0;
            bool have_bg = false;
            if (pos_in_selection(pos)) { cell_bg = SELECTION_BG; have_bg = true; }
            else if (find_len > 0 && have_cur_match && pos >= cur_match_pos && pos < cur_match_pos + find_len) {
                cell_bg = MATCH_CUR_BG; have_bg = true;
            } else if (find_len > 0) {
                // is pos within ANY match? check the start of the match window
                uint32_t s = (pos >= find_len - 1) ? pos - (find_len - 1) : 0;
                for (uint32_t m = s; m <= pos; m++) {
                    if (match_at(m) && pos < m + find_len) { cell_bg = MATCH_BG; have_bg = true; break; }
                }
            }
            if (!have_bg && have_bracket_match &&
                (pos == bracket_match_pos ||
                 (cursor_pos > 0 && pos == cursor_pos - 1 && (is_open_bracket(c) || is_close_bracket(c))) ||
                 (pos == cursor_pos && (is_open_bracket(c) || is_close_bracket(c))))) {
                if (is_open_bracket(c) || is_close_bracket(c)) { cell_bg = BRACKET_BG; have_bg = true; }
            }
            if (!have_bg && is_trailing_ws(pos, line)) { cell_bg = TRAIL_WS_BG; have_bg = true; }

            if (have_bg) win_draw_rect(window_handle, x, y, CHAR_W, CHAR_H, cell_bg);

            if (c == '\t') {
                // render tab as a faint marker, advance is still 1 cell here
            } else if (c >= ' ' && c < 127) {
                char str[2] = { c, '\0' };
                // Face-aware: honours the family/style/size from gui_font_dialog().
                win_draw_text_ttf_ex(window_handle, x, y, str,
                                     g_font.face, g_font.size, g_font.style_bits,
                                     TEXT_COLOR);
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

    // Vertical scrollbar
    if ((int)line_count > rows) {
        int sb_x = CONTENT_X + CONTENT_W;
        win_draw_rect(window_handle, sb_x, cy, 14, ch, SCROLL_TRACK);
        int thumb_h = rows * ch / (int)line_count;
        if (thumb_h < 16) thumb_h = 16;
        int range = (int)line_count - rows;
        int thumb_y = cy + (range > 0 ? (int)scroll_line * (ch - thumb_h) / range : 0);
        win_draw_rect(window_handle, sb_x + 2, thumb_y, 10, thumb_h, SCROLL_THUMB);
    }
}

// Draw status bar
static void draw_status_bar(void) {
    int y = EDITOR_HEIGHT - STATUS_HEIGHT - TITLEBAR_INSET;
    win_draw_rect(window_handle, 0, y, EDITOR_WIDTH, STATUS_HEIGHT, STATUS_BG);

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
    win_draw_text(window_handle, 8, y + 2, left, STATUS_TEXT);

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
    if (mid[0]) {
        int mw = gui_string_width(mid);
        uint32_t mc = (bracket_balance != 0) ? WARN_COLOR : STATUS_TEXT;
        win_draw_text(window_handle, (EDITOR_WIDTH - mw) / 2, y + 2, mid, mc);
    }

    // Right: line:col + total lines
    char info[48];
    snprintf(info, sizeof(info), "Ln %u, Col %u  |  %u lines",
             (unsigned)(cursor_line + 1), (unsigned)(cursor_col + 1),
             (unsigned)line_count);
    int info_w = gui_string_width(info);
    win_draw_text(window_handle, EDITOR_WIDTH - info_w - 16, y + 2, info, STATUS_TEXT);
}

// Full redraw
static void editor_redraw(void) {
    draw_menu_bar();
    draw_find_bar();
    draw_line_numbers();
    draw_content();
    draw_status_bar();
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

    int fd = open(filename, 1);  // O_WRONLY (simplified)
    if (fd < 0) return;

    write(fd, buffer, buffer_len);
    close(fd);

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

int main(int argc, char **argv) {
    (void)argc;

    window_handle = win_create("Editor", win_x, win_y, EDITOR_WIDTH, EDITOR_HEIGHT);
    ed_load_font();   // #351: restore the saved face before the first paint
    if (window_handle < 0) {
        return 1;
    }

    printf("Editor window created (handle=%d)\n", window_handle);

    apply_theme();
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
        {
            int th = get_theme();
            if (th != g_theme_last) { g_theme_last = th; apply_theme(); editor_redraw(); }
        }

        if (event_type == 0) {
            continue;
        }

        // Refresh window position for coordinate conversion.
        win_get_pos(window_handle, &win_x, &win_y);

        switch (event.type) {
            case EVENT_REDRAW:
                editor_redraw();
                break;

            case EVENT_RESIZE:
                if (event.mouse_x > 0 && event.mouse_y > 0) { g_ed_w = event.mouse_x; g_ed_h = event.mouse_y; }
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
                    else if (keycode == 0x53) {         // Delete key
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
                    else if (keycode == 0x47) { move_home(ext); recompute_lint(); handled = true; }
                    else if (keycode == 0x4F) { move_end(ext); recompute_lint(); handled = true; }
                    else if (keycode == 0x49) {         // Page Up
                        int r = visible_rows();
                        for (int i = 0; i < r && cursor_line > 0; i++) move_up(ext);
                        handled = true;
                    }
                    else if (keycode == 0x51) {         // Page Down
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

                    // Find bar buttons (prev/next) when open.
                    if (find_open && local_y >= MENU_HEIGHT && local_y < MENU_HEIGHT + FIND_HEIGHT) {
                        int y = MENU_HEIGHT;
                        int bx = 56 + 200 + 110;
                        if (local_x >= bx && local_x < bx + 24 && local_y >= y + 4 && local_y < y + 22) {
                            find_next(have_cur_match ? (cur_match_pos > 0 ? cur_match_pos : 0) : cursor_pos, false);
                        } else if (local_x >= bx + 28 && local_x < bx + 52 && local_y >= y + 4 && local_y < y + 22) {
                            find_next(have_cur_match ? cur_match_pos + 1 : cursor_pos, true);
                        } else if (local_x >= 56 && local_x < 256) {
                            find_field = 0;
                        }
                        editor_redraw();
                        break;
                    }

                    if (local_y >= 0 && local_y < MENU_HEIGHT) {
                        // Menu clicks: Search menu toggles find bar.
                        if (local_x >= 108 && local_x < 168) {
                            find_open = !find_open;
                            if (find_open) { replace_mode = false; find_field = 0; recompute_matches(); }
                        }
                        // Font menu: the SHARED picker (#351). The editor owns no
                        // font UI; it hands its current selection in and takes the
                        // new one back. Adding a font UI here would be the exact
                        // duplication this dialog exists to prevent.
                        else if (local_x >= 172 && local_x < 224) {
                            g_font.title = "Editor Font";
                            g_font.preview_text = "int main(void) { return 0; }";
                            if (gui_font_dialog(&g_font)) {
                                ed_apply_font();   // re-metric the cell grid
                                ed_save_font();
                            }
                        }
                        editor_redraw();
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
                if (mouse_selecting) {
                    // Already content-relative (kernel b330). Subtracting the
                    // window origin here again double-offsets every click.
                    int local_x = event.mouse_x;
                    int local_y = event.mouse_y;
                    uint32_t pos;
                    if (click_to_pos(local_x, local_y, &pos)) {
                        cursor_pos = pos;
                        has_selection = (pos != sel_anchor);
                        update_cursor_pos();
                        ensure_visible();
                        editor_redraw();
                    }
                }
                break;

            case EVENT_MOUSE_UP:
                mouse_selecting = false;
                break;

            case EVENT_MOUSE_SCROLL:
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
