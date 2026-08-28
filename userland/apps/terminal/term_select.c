// term_select.c
// PHASE 1 (terminal uplift, #221): selection and clipboard. See term_select.h
// for the ownership contract, the clipboard decision and the coordinate-space
// rationale.

#include "term_common.h"
#include "term_util.h"
#include "term_emu.h"
#include "term_grid.h"
#include "term_scrollback.h"
#include "term_parse.h"   // term_putc(): the shell-prompt paste echo
#include "term_render.h"
#include "term_theme.h"
#include "term_shell.h"
#include "term_pty.h"
#include "term_select.h"
#include "../../libc/settingscfg.h"   // THE shared double-click threshold (#236)

// ---------------------------------------------------------------------------
// Selection state, in VIRTUAL LINE / column coordinates (see term_select.h).
// ---------------------------------------------------------------------------
#define SEL_MODE_CHAR 0
#define SEL_MODE_WORD 1
#define SEL_MODE_LINE 2

static int sel_active   = 0;   // a selection exists
static int sel_dragging = 0;   // a button is down and extending it
static int sel_mode     = SEL_MODE_CHAR;
static int sel_av = 0, sel_ac = 0;   // anchor cell (inclusive)
static int sel_bv = 0, sel_bc = 0;   // focus cell (inclusive)
// The word/line span the anchor click landed on, so a double-click DRAG
// extends by whole words instead of collapsing back to a character.
static int sel_anchor_lo_v = 0, sel_anchor_lo_c = 0;
static int sel_anchor_hi_v = 0, sel_anchor_hi_c = 0;
// Scrollback-eviction tracking; see term_select_track().
static int sel_snap_head = 0;
// Click-count state for double/triple click.
static unsigned long sel_last_click_ms = 0;
static int sel_last_click_v = -1, sel_last_click_c = -1;
static int sel_click_count = 0;
// The pane the CLICK STREAK belongs to, which is deliberately NOT sel_owner:
// a single click with no drag clears the selection (and therefore its owner)
// on button-up, so comparing against sel_owner made every second click of a
// double-click look like a click in a different pane and reset the streak to
// one. That turned every double-click back into two single clicks and no word
// was ever selected. MEASURED on VM <vmid> before this line existed.
static int sel_click_pane = -1;
// Press position, so a click that never moves is a DESELECT rather than a
// zero-length selection. That threshold also leaves room for the OSC-8 / URL
// click the chrome spec defers to "the moment selection ships": a plain click
// stays unambiguously a click.
static int sel_press_v = 0, sel_press_c = 0;
static int sel_moved = 0;
// PANE OWNERSHIP (tabs/splits). -1 = no owner yet. See term_select.h.
static int sel_owner = -1;
static int sel_cur_pane = -1;

void term_select_note_pane(int pane) { sel_cur_pane = pane; }

// ---------------------------------------------------------------------------
// Read-only grid access. NULL for a virtual line that is not backed by
// anything (out of range, or scrollback whose malloc failed at startup).
// `cells` is a POINTER to rows since the tabs/splits change, so cells[live]
// is still exactly a row pointer.
// ---------------------------------------------------------------------------
static int vline_max(void) { return sb_count + term_rows - 1; }

static const term_cell_t *vrow(int v) {
    if (v < 0) return NULL;
    if (v < sb_count) return sb_lines ? sb_row(v) : NULL;
    int live = v - sb_count;
    if (live < 0 || live >= term_rows) return NULL;
    return cells[live];
}

// The codepoint at (v,c). 0 in the grid means "right half of a double-width
// character" (term_emu.h), which for selection purposes reads as its lead
// cell's text, so it is reported as a space and copies as nothing extra.
static uint32_t vcp(int v, int c) {
    const term_cell_t *r = vrow(v);
    if (!r || c < 0 || c >= term_cols) return ' ';
    uint32_t cp = r[c].ch;
    return cp ? cp : ' ';
}

// ---------------------------------------------------------------------------
// Ordering. Returns the selection normalised to lo <= hi in reading order.
// ---------------------------------------------------------------------------
static void sel_ordered(int *lov, int *loc, int *hiv, int *hic) {
    if (sel_av < sel_bv || (sel_av == sel_bv && sel_ac <= sel_bc)) {
        *lov = sel_av; *loc = sel_ac; *hiv = sel_bv; *hic = sel_bc;
    } else {
        *lov = sel_bv; *loc = sel_bc; *hiv = sel_av; *hic = sel_ac;
    }
}

int term_select_have(void) {
    if (!sel_active) return 0;
    int lov, loc, hiv, hic;
    sel_ordered(&lov, &loc, &hiv, &hic);
    return (hiv > lov) || (hic >= loc);
}

void term_select_clear(void) {
    sel_active = 0;
    sel_dragging = 0;
    sel_mode = SEL_MODE_CHAR;
    sel_owner = -1;
}

// ---------------------------------------------------------------------------
// Colours. The COLOUR SCHEME already carries selection_bg/selection_fg
// (term_palette_t, userland/libc/gui_palette.h) and every shipped
// /PALETTES/*.tpalette sets them; before this file NOTHING read either field.
// "Follow system theme" falls back to the OS theme's selection pair, resolved
// against the terminal's OWN window theme index, exactly the way
// term_bg_color()/term_fg_color() already do it.
// ---------------------------------------------------------------------------
static uint32_t sel_color_bg(void) {
    return g_term_palette_is_system
             ? theme_color_of(g_term_theme_index, THEME_COLOR_SELECTION)
             : g_term_palette.selection_bg;
}
static uint32_t sel_color_fg(void) {
    return g_term_palette_is_system
             ? theme_color_of(g_term_theme_index, THEME_COLOR_SELECTION_TEXT)
             : g_term_palette.selection_fg;
}

int term_select_cell_colors(int screen_row, int col, uint32_t *fg, uint32_t *bg) {
    if (!sel_active) return 0;
    // The selection belongs to ONE pane; do not paint it over another's grid.
    if (sel_owner >= 0 && sel_cur_pane >= 0 && sel_owner != sel_cur_pane) return 0;
    int v = gui_scroll_first_item(&term_scroll_view) + screen_row;
    int lov, loc, hiv, hic;
    sel_ordered(&lov, &loc, &hiv, &hic);
    if (v < lov || v > hiv) return 0;
    if (v == lov && col < loc) return 0;
    if (v == hiv && col > hic) return 0;
    if (fg) *fg = sel_color_fg();
    if (bg) *bg = sel_color_bg();
    return 1;
}

// ---------------------------------------------------------------------------
// Pixel -> cell. draw_row_cell() lays row R of the pane at
// y = term_origin_y + R*TERM_CHAR_H + 2 and column C at
// x = term_origin_x + C*TERM_CHAR_W + 2, so this is that mapping inverted,
// pane origin included. Clamped rather than rejected, because a drag that
// leaves the pane still has to extend the selection to the nearest edge,
// which is what every terminal does.
// ---------------------------------------------------------------------------
static void px_to_cell(int mx, int my, int *v, int *c) {
    int row = (my - term_origin_y - 2);
    int col = (mx - term_origin_x - 2);
    row = (row < 0) ? 0 : row / TERM_CHAR_H;
    col = (col < 0) ? 0 : col / TERM_CHAR_W;
    if (row >= term_rows) row = term_rows - 1;
    if (col >= term_cols) col = term_cols - 1;
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    *v = gui_scroll_first_item(&term_scroll_view) + row;
    *c = col;
    if (*v < 0) *v = 0;
    if (*v > vline_max()) *v = vline_max();
}

// ---------------------------------------------------------------------------
// Word boundaries. The word character set is Konsole's default (alphanumeric
// plus ":@-./_~"), chosen so a double-click on a path or a URL selects the
// whole thing rather than one segment of it. Every non-ASCII codepoint counts
// as a word character: splitting CJK or accented text on a byte-oriented rule
// would be worse than not splitting it.
// ---------------------------------------------------------------------------
static int is_word_char(uint32_t cp) {
    if (cp >= 128) return 1;
    if (cp >= '0' && cp <= '9') return 1;
    if (cp >= 'a' && cp <= 'z') return 1;
    if (cp >= 'A' && cp <= 'Z') return 1;
    return cp == ':' || cp == '@' || cp == '-' || cp == '.' ||
           cp == '/' || cp == '_' || cp == '~';
}

// Expand (v,c) to the word span containing it. A double-click on whitespace
// selects the run of whitespace, matching xterm.
static void expand_word(int v, int c, int *loc, int *hic) {
    int want = is_word_char(vcp(v, c));
    int a = c, b = c;
    while (a > 0 && is_word_char(vcp(v, a - 1)) == want) a--;
    while (b < term_cols - 1 && is_word_char(vcp(v, b + 1)) == want) b++;
    *loc = a; *hic = b;
}

// ---------------------------------------------------------------------------
// Text extraction. Trailing blanks are trimmed on every line, because a
// terminal row is ALWAYS padded to full width with spaces; copying them
// verbatim would paste sixty spaces after every short line.
//
// Non-ASCII cells are UTF-8 encoded on the way out, which is what every other
// consumer of the clipboard expects and what the terminal itself accepts back
// on a paste (term_emu.c decodes UTF-8 input).
// ---------------------------------------------------------------------------
static int put_utf8(char *out, int cap, int n, uint32_t cp) {
    if (cp < 0x80) {
        if (n + 1 > cap) return -1;
        out[n++] = (char)cp;
    } else if (cp < 0x800) {
        if (n + 2 > cap) return -1;
        out[n++] = (char)(0xC0 | (cp >> 6));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (n + 3 > cap) return -1;
        out[n++] = (char)(0xE0 | (cp >> 12));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (n + 4 > cap) return -1;
        out[n++] = (char)(0xF0 | (cp >> 18));
        out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    }
    return n;
}

int term_select_get_text(char *out, int cap) {
    int n = 0;
    if (!out || cap <= 0) return 0;
    out[0] = '\0';
    if (!term_select_have()) return 0;

    int lov, loc, hiv, hic;
    sel_ordered(&lov, &loc, &hiv, &hic);
    for (int v = lov; v <= hiv; v++) {
        int c0 = (v == lov) ? loc : 0;
        int c1 = (v == hiv) ? hic : term_cols - 1;
        if (c1 >= term_cols) c1 = term_cols - 1;
        int end = c1;
        while (end >= c0 && vcp(v, end) == ' ') end--;
        for (int c = c0; c <= end; c++) {
            const term_cell_t *r = vrow(v);
            uint32_t cp = (r && c < term_cols) ? r[c].ch : ' ';
            if (cp == 0) continue;       // wide-char continuation: no second copy
            if (cp < ' ' || cp == 127) cp = ' ';
            int m = put_utf8(out, cap - 1, n, cp);
            if (m < 0) { out[n] = '\0'; return n; }
            n = m;
        }
        if (v < hiv) {
            if (n >= cap - 1) { out[n] = '\0'; return n; }
            out[n++] = '\n';
        }
    }
    out[n] = '\0';
    return n;
}

// The one staging buffer, shared by copy and paste. Sized to the working set
// this feature actually needs rather than the clipboard's 64 KiB cap; a whole
// 2000-line Select All truncates here, and that is stated rather than
// discovered.
#define TERM_SEL_BUF 8192
static char sel_buf[TERM_SEL_BUF];

int term_select_copy(void) {
    int n = term_select_get_text(sel_buf, TERM_SEL_BUF);
    if (n <= 0) return 0;
    // The SHARED, OS-WIDE clipboard (#542). Not a terminal-local one.
    return clipboard_set(sel_buf, n);
}

void term_select_all(void) {
    sel_active = 1;
    sel_dragging = 0;
    sel_mode = SEL_MODE_LINE;
    sel_av = 0; sel_ac = 0;
    sel_bv = vline_max(); sel_bc = term_cols - 1;
    sel_snap_head = sb_head;
    sel_owner = sel_cur_pane;
}

// ---------------------------------------------------------------------------
// PASTE.
//
// Two destinations, because this terminal has two input modes:
//   a foreground child owns the pty  -> bytes go to g_active_master_fd
//   the built-in shell prompt        -> bytes go into the one-line editor
//
// SAFETY, and this is the point of the feature rather than a nicety:
//
//   * ESC and every other C0 control except TAB is stripped from the pasted
//     bytes. Clipboard content is untrusted (it can come from a web page via
//     the browser, or from any other app), and an unfiltered paste lets it
//     drive the terminal's own escape parser. xterm's allowPasteControls is
//     off by default for exactly this reason.
//   * When the child has enabled ?2004 the text is wrapped in ESC[200~ and
//     ESC[201~ so a readline shell treats it as literal text instead of
//     executing every line, and vi inserts it instead of interpreting it as
//     commands. The literal byte sequence "ESC[201~" is ALSO stripped from
//     the payload: without that, clipboard content containing the end marker
//     terminates the bracket early and the remainder executes, which is the
//     classic bracketed-paste bypass.
//   * LF is converted to CR, which is what a real keyboard Enter delivers on
//     a tty (term_pty.c's key_event_to_bytes() does the same conversion).
// ---------------------------------------------------------------------------
static int paste_sanitize(char *dst, int cap, const char *src, int n) {
    int o = 0;
    for (int i = 0; i < n && o < cap; i++) {
        unsigned char ch = (unsigned char)src[i];
        // Strip a literal end-marker so it cannot close the bracket early.
        if (ch == 0x1b && i + 5 < n &&
            src[i+1] == '[' && src[i+2] == '2' && src[i+3] == '0' &&
            src[i+4] == '1' && src[i+5] == '~') { i += 5; continue; }
        if (ch == '\n' || ch == '\r') {
            dst[o++] = '\r';
            // CRLF collapses to one CR.
            if (ch == '\r' && i + 1 < n && src[i+1] == '\n') i++;
            continue;
        }
        if (ch == '\t') { dst[o++] = '\t'; continue; }
        if (ch < 0x20 || ch == 0x7f) continue;   // every other control byte
        dst[o++] = (char)ch;                     // UTF-8 continuation bytes are >= 0x80: kept
    }
    return o;
}

int term_select_paste(void) {
    int held = clipboard_get(sel_buf, TERM_SEL_BUF);
    if (held <= 0) return 0;

    static char clean[TERM_SEL_BUF];
    int n = paste_sanitize(clean, TERM_SEL_BUF, sel_buf, held);
    if (n <= 0) return 0;

    if (g_active_master_fd >= 0) {
        // term_emu owns the ?2004 flag; this file must not keep a copy.
        int bracket = term_emu_bracketed_paste();
        if (bracket) write(g_active_master_fd, "\033[200~", 6);
        write(g_active_master_fd, clean, n);
        if (bracket) write(g_active_master_fd, "\033[201~", 6);
        return n;
    }

    // Built-in shell prompt. This is a ONE-LINE editor (input_line /
    // input_pos in term_shell.h; there is no multi-line command state and no
    // readline), so a multi-line paste is inserted UP TO the first line break
    // and the remainder is dropped. Feeding the rest through would mean
    // executing every pasted line, which is precisely the hazard bracketed
    // paste exists to prevent; silently turning the breaks into spaces would
    // change what the text means. ?2004 never applies on this path: the
    // built-in shell does not, and cannot, request it.
    int wrote = 0;
    for (int i = 0; i < n; i++) {
        char ch = clean[i];
        if (ch == '\r' || ch == '\n') break;
        if (ch < ' ' || ch >= 127) continue;
        if (input_pos >= TERM_SHELL_INPUT_MAX - 1) break;
        input_line[input_pos++] = ch;
        input_line[input_pos] = '\0';
        term_putc(ch);
        wrote++;
    }
    // Pasting IS typing, so it snaps the view back to the live screen the way
    // main.c's scroll-on-keystroke rule does; a paste you cannot see land is a
    // paste you will do twice.
    if (wrote && !term_at_bottom) {
        term_scrollback_reconfigure();
        gui_scroll_set(&term_scroll_view, gui_scroll_max(&term_scroll_view));
        term_scroll_sync_bottom();
    }
    return wrote;
}

// ---------------------------------------------------------------------------
// ARBITRATION. Is the CHILD entitled to this event?
//
// term_emu_mouse_reporting() is the ONE reader of ?1000/?1002/?1003 (term_emu
// owns the flag). Shift is the universal override, and it is exactly why #221
// phase 0 (gui_mods.h) had to exist first: without a tracked Shift bit there
// is no way to reach selection at all inside a full-screen mouse-aware
// program. term_emu.h writes the same convention down at its own declaration.
// ---------------------------------------------------------------------------
int term_select_app_owns_mouse(void) {
    if (!term_emu_mouse_reporting()) return 0;
    if (g_active_master_fd < 0) return 0;           // nobody to report to
    if (gui_mods_get() & GUI_MOD_SHIFT) return 0;   // user override
    return 1;
}

int term_select_handle_mouse(const gui_event_t *ev, int etype) {
    // TERM_SEL_PASS here means "give it to term_mouse_report()".
    if (term_select_app_owns_mouse()) return TERM_SEL_PASS;
    if (etype == EVENT_MOUSE_SCROLL) return TERM_SEL_PASS;  // the wheel is scrollback's

    int v, c;
    // The scroll widget geometry must be current before any screen-row ->
    // virtual-line mapping. Cheap and idempotent; the caller has normally
    // just called it for the scrollbar anyway.
    term_scrollback_reconfigure();
    px_to_cell(ev->mouse_x, ev->mouse_y, &v, &c);

    switch (etype) {
        case EVENT_MOUSE_DOWN: {
            unsigned long now = uptime_ms();
            int win = settingscfg_dblclick_ms();   // the OS-wide setting (#236), not a local constant
            if (sel_click_count > 0 && sel_click_pane == sel_cur_pane &&
                v == sel_last_click_v && c == sel_last_click_c &&
                (now - sel_last_click_ms) <= (unsigned long)win) {
                sel_click_count++;
                if (sel_click_count > 3) sel_click_count = 1;
            } else {
                sel_click_count = 1;
            }
            sel_last_click_ms = now;
            sel_last_click_v = v;
            sel_last_click_c = c;
            sel_click_pane = sel_cur_pane;

            sel_active = 1;
            sel_dragging = 1;
            sel_moved = 0;
            sel_press_v = v; sel_press_c = c;
            sel_snap_head = sb_head;
            sel_owner = sel_cur_pane;

            if (sel_click_count >= 3) {
                sel_mode = SEL_MODE_LINE;
                sel_anchor_lo_v = v; sel_anchor_lo_c = 0;
                sel_anchor_hi_v = v; sel_anchor_hi_c = term_cols - 1;
            } else if (sel_click_count == 2) {
                sel_mode = SEL_MODE_WORD;
                int a, b;
                expand_word(v, c, &a, &b);
                sel_anchor_lo_v = v; sel_anchor_lo_c = a;
                sel_anchor_hi_v = v; sel_anchor_hi_c = b;
            } else {
                sel_mode = SEL_MODE_CHAR;
                sel_anchor_lo_v = v; sel_anchor_lo_c = c;
                sel_anchor_hi_v = v; sel_anchor_hi_c = c;
            }
            sel_av = sel_anchor_lo_v; sel_ac = sel_anchor_lo_c;
            sel_bv = sel_anchor_hi_v; sel_bc = sel_anchor_hi_c;
            return TERM_SEL_REDRAW;
        }

        case EVENT_MOUSE_MOVE: {
            if (!sel_dragging) return TERM_SEL_TAKEN;
            if (sel_owner != sel_cur_pane) return TERM_SEL_TAKEN;
            if (v != sel_press_v || c != sel_press_c) sel_moved = 1;
            // Extend from whichever end of the ANCHOR SPAN the pointer is
            // past, so a word- or line-mode drag grows by whole words/lines
            // in both directions instead of collapsing to a character.
            int before = (v < sel_anchor_lo_v) ||
                         (v == sel_anchor_lo_v && c < sel_anchor_lo_c);
            if (before) {
                sel_av = sel_anchor_hi_v; sel_ac = sel_anchor_hi_c;
                if (sel_mode == SEL_MODE_LINE)      { sel_bv = v; sel_bc = 0; }
                else if (sel_mode == SEL_MODE_WORD) { int a, b; expand_word(v, c, &a, &b); sel_bv = v; sel_bc = a; (void)b; }
                else                                { sel_bv = v; sel_bc = c; }
            } else {
                sel_av = sel_anchor_lo_v; sel_ac = sel_anchor_lo_c;
                if (sel_mode == SEL_MODE_LINE)      { sel_bv = v; sel_bc = term_cols - 1; }
                else if (sel_mode == SEL_MODE_WORD) { int a, b; expand_word(v, c, &a, &b); sel_bv = v; sel_bc = b; (void)a; }
                else                                { sel_bv = v; sel_bc = c; }
            }
            return TERM_SEL_REDRAW;
        }

        case EVENT_MOUSE_UP: {
            if (!sel_dragging) return TERM_SEL_TAKEN;
            sel_dragging = 0;
            // A single click that never moved is a DESELECT, not a
            // one-character selection. This is also what keeps a plain click
            // unambiguous for the OSC-8 / URL-click work the chrome spec
            // defers to "the moment selection ships".
            if (sel_mode == SEL_MODE_CHAR && !sel_moved) {
                term_select_clear();
                return TERM_SEL_REDRAW;
            }
            return TERM_SEL_TAKEN;
        }

        default:
            return TERM_SEL_PASS;
    }
}

// ---------------------------------------------------------------------------
// Keyboard. Konsole's scheme, which is what docs/TERMINAL_KONSOLE_CHROME_SPEC
// specifies: Ctrl+Shift+C / V / A, leaving plain Ctrl+C free to be SIGINT.
//
// gui_mods_is() is EXACT, not a subset test, which is the entire reason plain
// Ctrl+C still reaches the child: mods == CTRL does not match CTRL|SHIFT.
// gui_mods_letter() undoes the kernel's case/control folding, which is
// necessary because Ctrl+Shift+c arrives as an ordinary capital 'C',
// indistinguishable from Shift+c by its character alone (gui_mods.h).
// ---------------------------------------------------------------------------
int term_select_handle_key(const gui_event_t *ev) {
    if (ev->type != EVENT_KEY_DOWN) return TERM_SEL_PASS;
    if (!gui_mods_is(GUI_MOD_CTRL | GUI_MOD_SHIFT)) return TERM_SEL_PASS;
    switch (gui_mods_letter(ev)) {
        case 'c':
            if (!term_select_have()) return TERM_SEL_TAKEN;   // consumed: never send a stray 'C'
            term_select_copy();
            return TERM_SEL_TAKEN;
        case 'v':
            term_select_paste();
            return TERM_SEL_REDRAW;
        case 'a':
            term_select_all();
            return TERM_SEL_REDRAW;
        default:
            return TERM_SEL_PASS;
    }
}

// ---------------------------------------------------------------------------
// Invalidation. See the long note in term_select.h for why scrolling and
// ordinary output need no work and only ring EVICTION and RESIZE do.
// ---------------------------------------------------------------------------
void term_select_on_resize(void) {
    term_select_clear();
    sel_click_count = 0;
    sel_last_click_v = -1;
    sel_last_click_c = -1;
    sel_click_pane = -1;
}

void term_select_track(void) {
    if (!sel_active) return;
    if (sel_owner >= 0 && sel_cur_pane >= 0 && sel_owner != sel_cur_pane) return;
    // Once the ring is full every push evicts the oldest line and sb_head
    // advances, which shifts every virtual line down by one. This is the
    // same compensation term_history_push() already applies to the scroll
    // offset; without it a selection silently creeps onto newer text.
    if (sb_count >= SCROLLBACK_LINES && sb_head != sel_snap_head) {
        int d = sb_head - sel_snap_head;
        if (d < 0) d += SCROLLBACK_LINES;
        sel_av -= d; sel_bv -= d;
        sel_anchor_lo_v -= d; sel_anchor_hi_v -= d;
        if (sel_last_click_v >= 0) sel_last_click_v -= d;
    }
    sel_snap_head = sb_head;

    // Scrolled entirely off the top: the text is gone, so the selection is.
    if (sel_av < 0 && sel_bv < 0) { term_select_clear(); return; }
    if (sel_av < 0) { sel_av = 0; sel_ac = 0; }
    if (sel_bv < 0) { sel_bv = 0; sel_bc = 0; }

    int vmax = vline_max();
    if (sel_av > vmax) sel_av = vmax;
    if (sel_bv > vmax) sel_bv = vmax;
    if (sel_ac >= term_cols) sel_ac = term_cols - 1;
    if (sel_bc >= term_cols) sel_bc = term_cols - 1;
}
