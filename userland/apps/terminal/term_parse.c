// term_parse.c - what a PARSED escape sequence means to the grid.
// See term_parse.h and docs/TERMINAL_EMULATION.md.

#include "term_common.h"
#include "term_util.h"
#include "term_grid.h"
#include "term_scrollback.h"
#include "term_render.h"
#include "term_theme.h"
#include "term_pty.h"
#include "term_notify.h"
#include "term_layout.h"
#include "term_emu.h"
#include "term_parse.h"

// ===========================================================================
// THE EMULATION GLUE
//
// term_emu.c owns the byte-level state machine, the UTF-8 decoder, wcwidth and
// the SGR grammar. Everything below is what those parse results MEAN to this
// terminal's grid: cursor motion, erasure, scrolling, modes and replies.
//
// The old code did all of it in one 250-line switch that was also the
// tokenizer. Three of its bugs are worth naming here because the fixes are
// visible in this file rather than in term_emu.c:
//
//   - ED and EL IGNORED THEIR PARAMETER. `ESC[2K` (erase the whole line, which
//     readline emits on every redraw and every progress bar uses) erased only
//     from the cursor rightwards, and `ESC[1J` erased the wrong half of the
//     screen. Both now honour 0/1/2 (and 3 for ED).
//   - CHA/VPA/ECH/SU/SD/CNL/CPL/CBT/CHT/REP/DA were simply absent. Not "leaked"
//     - silently swallowed, so the cursor did not move and the program's next
//     write landed in the wrong column. ncurses picks the CHEAPEST sequence
//     from terminfo, and CHA/VPA are in every terminfo, so this was not an
//     exotic path.
//   - A TAB WROTE SPACES. Tab is non-destructive cursor motion; writing spaces
//     erases whatever a full-screen program had already drawn there.
// ---------------------------------------------------------------------------

// Reply on the foreground child's pty master. Silently dropped when no child
// owns the terminal, which is correct: the built-in shell never queries.
void term_reply(const char *b, int n) {
    if (g_active_master_fd >= 0 && n > 0) write(g_active_master_fd, b, (unsigned long)n);
}

// ---------------------------------------------------------------------------
// Wide-character grid integrity.
//
// A double-width character occupies TWO cells: the lead cell holds the
// codepoint, the next holds ch == 0. Overwriting either half alone would leave
// a lead with no continuation (or a continuation with no lead), and from then
// on the grid and the application disagree about which column is which for the
// rest of that row. Every write goes through this first.
// ---------------------------------------------------------------------------
// Combining marks.
//
// A zero-width mark must NOT occupy a cell and must NOT advance the cursor -
// getting that wrong shifts every later column on the row. Beyond that, the
// mark should ideally be DRAWN, and the cheap way to draw one is to compose it
// with the base character into the single precomposed codepoint the font
// already has a glyph for. This table is the Latin coverage (NFC of every
// ASCII letter with the thirteen common marks); anything outside it still
// costs zero cells, it just is not rendered. Stated plainly rather than
// implied: a Devanagari or Arabic mark is dropped, not stacked.
static const struct { uint16_t base, mark, comp; } k_precomp[] = {
    {0x0041,0x0300,0x00C0}, {0x0041,0x0301,0x00C1}, {0x0041,0x0302,0x00C2}, {0x0041,0x0303,0x00C3},
    {0x0041,0x0304,0x0100}, {0x0041,0x0306,0x0102}, {0x0041,0x0307,0x0226}, {0x0041,0x0308,0x00C4},
    {0x0041,0x030A,0x00C5}, {0x0041,0x030C,0x01CD}, {0x0041,0x0328,0x0104}, {0x0043,0x0301,0x0106},
    {0x0043,0x0302,0x0108}, {0x0043,0x0307,0x010A}, {0x0043,0x030C,0x010C}, {0x0043,0x0327,0x00C7},
    {0x0044,0x030C,0x010E}, {0x0045,0x0300,0x00C8}, {0x0045,0x0301,0x00C9}, {0x0045,0x0302,0x00CA},
    {0x0045,0x0304,0x0112}, {0x0045,0x0306,0x0114}, {0x0045,0x0307,0x0116}, {0x0045,0x0308,0x00CB},
    {0x0045,0x030C,0x011A}, {0x0045,0x0327,0x0228}, {0x0045,0x0328,0x0118}, {0x0047,0x0301,0x01F4},
    {0x0047,0x0302,0x011C}, {0x0047,0x0306,0x011E}, {0x0047,0x0307,0x0120}, {0x0047,0x030C,0x01E6},
    {0x0047,0x0327,0x0122}, {0x0048,0x0302,0x0124}, {0x0048,0x030C,0x021E}, {0x0049,0x0300,0x00CC},
    {0x0049,0x0301,0x00CD}, {0x0049,0x0302,0x00CE}, {0x0049,0x0303,0x0128}, {0x0049,0x0304,0x012A},
    {0x0049,0x0306,0x012C}, {0x0049,0x0307,0x0130}, {0x0049,0x0308,0x00CF}, {0x0049,0x030C,0x01CF},
    {0x0049,0x0328,0x012E}, {0x004A,0x0302,0x0134}, {0x004B,0x030C,0x01E8}, {0x004B,0x0327,0x0136},
    {0x004C,0x0301,0x0139}, {0x004C,0x030C,0x013D}, {0x004C,0x0327,0x013B}, {0x004E,0x0300,0x01F8},
    {0x004E,0x0301,0x0143}, {0x004E,0x0303,0x00D1}, {0x004E,0x030C,0x0147}, {0x004E,0x0327,0x0145},
    {0x004F,0x0300,0x00D2}, {0x004F,0x0301,0x00D3}, {0x004F,0x0302,0x00D4}, {0x004F,0x0303,0x00D5},
    {0x004F,0x0304,0x014C}, {0x004F,0x0306,0x014E}, {0x004F,0x0307,0x022E}, {0x004F,0x0308,0x00D6},
    {0x004F,0x030B,0x0150}, {0x004F,0x030C,0x01D1}, {0x004F,0x0328,0x01EA}, {0x0052,0x0301,0x0154},
    {0x0052,0x030C,0x0158}, {0x0052,0x0327,0x0156}, {0x0053,0x0301,0x015A}, {0x0053,0x0302,0x015C},
    {0x0053,0x030C,0x0160}, {0x0053,0x0327,0x015E}, {0x0054,0x030C,0x0164}, {0x0054,0x0327,0x0162},
    {0x0055,0x0300,0x00D9}, {0x0055,0x0301,0x00DA}, {0x0055,0x0302,0x00DB}, {0x0055,0x0303,0x0168},
    {0x0055,0x0304,0x016A}, {0x0055,0x0306,0x016C}, {0x0055,0x0308,0x00DC}, {0x0055,0x030A,0x016E},
    {0x0055,0x030B,0x0170}, {0x0055,0x030C,0x01D3}, {0x0055,0x0328,0x0172}, {0x0057,0x0302,0x0174},
    {0x0059,0x0301,0x00DD}, {0x0059,0x0302,0x0176}, {0x0059,0x0304,0x0232}, {0x0059,0x0308,0x0178},
    {0x005A,0x0301,0x0179}, {0x005A,0x0307,0x017B}, {0x005A,0x030C,0x017D}, {0x0061,0x0300,0x00E0},
    {0x0061,0x0301,0x00E1}, {0x0061,0x0302,0x00E2}, {0x0061,0x0303,0x00E3}, {0x0061,0x0304,0x0101},
    {0x0061,0x0306,0x0103}, {0x0061,0x0307,0x0227}, {0x0061,0x0308,0x00E4}, {0x0061,0x030A,0x00E5},
    {0x0061,0x030C,0x01CE}, {0x0061,0x0328,0x0105}, {0x0063,0x0301,0x0107}, {0x0063,0x0302,0x0109},
    {0x0063,0x0307,0x010B}, {0x0063,0x030C,0x010D}, {0x0063,0x0327,0x00E7}, {0x0064,0x030C,0x010F},
    {0x0065,0x0300,0x00E8}, {0x0065,0x0301,0x00E9}, {0x0065,0x0302,0x00EA}, {0x0065,0x0304,0x0113},
    {0x0065,0x0306,0x0115}, {0x0065,0x0307,0x0117}, {0x0065,0x0308,0x00EB}, {0x0065,0x030C,0x011B},
    {0x0065,0x0327,0x0229}, {0x0065,0x0328,0x0119}, {0x0067,0x0301,0x01F5}, {0x0067,0x0302,0x011D},
    {0x0067,0x0306,0x011F}, {0x0067,0x0307,0x0121}, {0x0067,0x030C,0x01E7}, {0x0067,0x0327,0x0123},
    {0x0068,0x0302,0x0125}, {0x0068,0x030C,0x021F}, {0x0069,0x0300,0x00EC}, {0x0069,0x0301,0x00ED},
    {0x0069,0x0302,0x00EE}, {0x0069,0x0303,0x0129}, {0x0069,0x0304,0x012B}, {0x0069,0x0306,0x012D},
    {0x0069,0x0308,0x00EF}, {0x0069,0x030C,0x01D0}, {0x0069,0x0328,0x012F}, {0x006A,0x0302,0x0135},
    {0x006A,0x030C,0x01F0}, {0x006B,0x030C,0x01E9}, {0x006B,0x0327,0x0137}, {0x006C,0x0301,0x013A},
    {0x006C,0x030C,0x013E}, {0x006C,0x0327,0x013C}, {0x006E,0x0300,0x01F9}, {0x006E,0x0301,0x0144},
    {0x006E,0x0303,0x00F1}, {0x006E,0x030C,0x0148}, {0x006E,0x0327,0x0146}, {0x006F,0x0300,0x00F2},
    {0x006F,0x0301,0x00F3}, {0x006F,0x0302,0x00F4}, {0x006F,0x0303,0x00F5}, {0x006F,0x0304,0x014D},
    {0x006F,0x0306,0x014F}, {0x006F,0x0307,0x022F}, {0x006F,0x0308,0x00F6}, {0x006F,0x030B,0x0151},
    {0x006F,0x030C,0x01D2}, {0x006F,0x0328,0x01EB}, {0x0072,0x0301,0x0155}, {0x0072,0x030C,0x0159},
    {0x0072,0x0327,0x0157}, {0x0073,0x0301,0x015B}, {0x0073,0x0302,0x015D}, {0x0073,0x030C,0x0161},
    {0x0073,0x0327,0x015F}, {0x0074,0x030C,0x0165}, {0x0074,0x0327,0x0163}, {0x0075,0x0300,0x00F9},
    {0x0075,0x0301,0x00FA}, {0x0075,0x0302,0x00FB}, {0x0075,0x0303,0x0169}, {0x0075,0x0304,0x016B},
    {0x0075,0x0306,0x016D}, {0x0075,0x0308,0x00FC}, {0x0075,0x030A,0x016F}, {0x0075,0x030B,0x0171},
    {0x0075,0x030C,0x01D4}, {0x0075,0x0328,0x0173}, {0x0077,0x0302,0x0175}, {0x0079,0x0301,0x00FD},
    {0x0079,0x0302,0x0177}, {0x0079,0x0304,0x0233}, {0x0079,0x0308,0x00FF}, {0x007A,0x0301,0x017A},
    {0x007A,0x0307,0x017C}, {0x007A,0x030C,0x017E},
};

static uint32_t precompose(uint32_t base, uint32_t mark) {
    if (base > 0xFFFF || mark > 0xFFFF) return 0;
    unsigned key = ((unsigned)base << 16) | (unsigned)mark;
    int lo = 0, hi = (int)(sizeof(k_precomp) / sizeof(k_precomp[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        unsigned k = ((unsigned)k_precomp[mid].base << 16) | (unsigned)k_precomp[mid].mark;
        if (key < k) hi = mid - 1;
        else if (key > k) lo = mid + 1;
        else return k_precomp[mid].comp;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// print: one decoded codepoint reaches the grid.
static uint32_t g_last_print = ' ';   // for REP (CSI b)
// The last title an application asked for via OSC 0/2. See tcb_osc().
char g_osc_title[128];

static void term_put_cp(uint32_t cp) {
    int w = term_emu_wcwidth(cp);
    if (w < 0) return;                 // a control character has nothing to draw

    if (w == 0) {
        // Combining mark. Attach to the cell to the LEFT of the cursor if we
        // can compose it; never take a cell, never move the cursor.
        int col = cursor_x - 1;
        if (col >= 0 && cells[cursor_y][col].ch == 0 && col > 0) col--;   // skip a wide continuation
        if (col >= 0) {
            uint32_t comp = precompose(cells[cursor_y][col].ch, cp);
            if (comp) cells[cursor_y][col].ch = comp;
        }
        return;
    }

    if (term_autowrap && cursor_x + w > term_cols) term_newline();
    if (cursor_x + w > term_cols) {
        // Autowrap off and no room: overwrite the last column, which is what a
        // program that turned autowrap off to draw a full-width status line
        // expects. A wide character with one column left is dropped rather
        // than written as half of itself.
        if (w == 2) return;
        cursor_x = term_cols - 1;
    }

    cell_break_wide(cursor_y, cursor_x);
    term_cell_t *c = &cells[cursor_y][cursor_x];
    c->ch = cp; c->fg = g_pen.fg; c->bg = g_pen.bg; c->attr = g_pen.attr;
    c->pad[0] = c->pad[1] = c->pad[2] = 0;
    if (w == 2) {
        cell_break_wide(cursor_y, cursor_x + 1);
        term_cell_t *k = &cells[cursor_y][cursor_x + 1];
        *k = *c;
        k->ch = 0;                     // continuation: drawn by the lead
    }
    g_last_print = cp;
    cursor_x += w;
    if (cursor_x >= term_cols) {
        if (term_autowrap) term_newline();
        else cursor_x = term_cols - 1;
    }
}

static void tcb_print(void *ctx, uint32_t cp) { (void)ctx; term_put_cp(cp); }

// ---------------------------------------------------------------------------
// execute: a C0 control byte.
static void tcb_execute(void *ctx, uint8_t c) {
    (void)ctx;
    switch (c) {
        case 0x08:                     // BS
            if (cursor_x > 0) cursor_x--;
            break;
        case 0x09: {                   // HT - NON-DESTRUCTIVE, see the header comment
            int x = cursor_x + 1;
            while (x < term_cols && !g_tabstop[x]) x++;
            cursor_x = (x < term_cols) ? x : term_cols - 1;
            break;
        }
        case 0x0A: case 0x0B: case 0x0C:   // LF, VT, FF all index the cursor
            term_newline();
            break;
        case 0x0D:                     // CR
            cursor_x = 0;
            break;
        case 0x07:
            // PHASE 1: BEL. Rung for the tab this parser is currently driving -
            // with splits that is NOT necessarily the tab on screen, since a
            // background pane's pty is pumped every tick too. Handled HERE
            // rather than in the pty pump because only the PARSER knows whether
            // a 0x07 is a bell at all: inside an OSC string it is the string
            // TERMINATOR, and term_emu.c has already routed that case to the
            // OSC handler before this callback can see it.
            term_notify_bell(term_layout_active_tab());
            break;
        default:
            // SO/SI (charset shift) and the rest: nothing this terminal models.
            // Consumed, never printed.
            break;
    }
}

// ---------------------------------------------------------------------------
// A soft reset (DECSTR, CSI ! p). Distinct from RIS: it does not clear the
// screen, which is the whole reason a program uses it.
void term_soft_reset(void) {
    term_emu_sgr_reset(&g_pen);
    scroll_top = 0; scroll_bottom = -1;
    term_autowrap = 1;
    cursor_visible = true; cursor_blink_on = true;
    term_emu_modes_reset();
    saved_cursor_x = 0; saved_cursor_y = 0;
    term_emu_sgr_reset(&g_saved_pen);
    term_tabstops_default(g_tabstop);
}

// ---------------------------------------------------------------------------
// esc: ESC [intermediates] final
static void tcb_esc(void *ctx, const term_seq_t *sq) {
    (void)ctx;
    if (sq->ninter > 0) {
        // ESC ( B, ESC ) 0, ESC * ... : charset designation. NOT modelled (the
        // DEC line-drawing set is a separate piece of work) but correctly
        // swallowed; the old code printed the final byte.
        if (sq->inter[0] == '#' && sq->final == '8') {
            // DECALN: fill the screen with 'E'. Rarely used by applications;
            // kept because it is the canonical alignment self-test and makes a
            // grid fault obvious at a glance.
            for (int r = 0; r < term_rows; r++)
                for (int c = 0; c < term_cols; c++) {
                    blank_cell(&cells[r][c]);
                    cells[r][c].ch = 'E';
                }
            cursor_x = 0; cursor_y = 0;
        }
        return;
    }
    switch (sq->final) {
        case '7':   // DECSC
            saved_cursor_x = cursor_x; saved_cursor_y = cursor_y;
            g_saved_pen = g_pen;
            break;
        case '8':   // DECRC
            cursor_x = saved_cursor_x; cursor_y = saved_cursor_y;
            g_pen = g_saved_pen;
            if (cursor_x >= term_cols) cursor_x = term_cols - 1;
            if (cursor_y >= term_rows) cursor_y = term_rows - 1;
            break;
        case 'D': { // IND
            int bot = term_scroll_bottom_eff();
            if (cursor_y == bot) term_scroll_region(scroll_top, bot);
            else if (cursor_y < term_rows - 1) cursor_y++;
            break;
        }
        case 'E':   // NEL - index and return to column 0
            term_newline();
            break;
        case 'M':   // RI
            if (cursor_y == scroll_top) term_scroll_region_down(scroll_top, term_scroll_bottom_eff());
            else if (cursor_y > 0) cursor_y--;
            break;
        case 'H':   // HTS - set a tab stop at the cursor column
            if (cursor_x >= 0 && cursor_x < TERM_MAX_COLS) g_tabstop[cursor_x] = 1;
            break;
        case 'c':   // RIS
            term_full_reset();
            break;
        default:
            // ESC =, ESC > (keypad modes), ESC n/o (locking shifts) and the
            // rest: consumed, not modelled, never printed.
            break;
    }
}

// ---------------------------------------------------------------------------
// DEC private modes (CSI ? n h / l)
static void csi_private_mode(const term_seq_t *sq, int on) {
    for (int i = 0; i < sq->nparams; i++) {
        int m = term_emu_param(sq, i, 0);
        switch (m) {
            case 7:    term_autowrap = on; break;                 // DECAWM
            case 25:   cursor_visible = on ? true : false; break; // DECTCEM
            case 47: case 1047: case 1049:
                if (on && !in_alt_screen) {
                    for (int r = 0; r < TERM_MAX_ROWS; r++)
                        for (int cc = 0; cc < TERM_MAX_COLS; cc++)
                            alt_saved_cells[r][cc] = cells[r][cc];
                    alt_saved_cursor_x = cursor_x;
                    alt_saved_cursor_y = cursor_y;
                    in_alt_screen = 1;
                    term_clear();
                } else if (!on && in_alt_screen) {
                    for (int r = 0; r < TERM_MAX_ROWS; r++)
                        for (int cc = 0; cc < TERM_MAX_COLS; cc++)
                            cells[r][cc] = alt_saved_cells[r][cc];
                    cursor_x = alt_saved_cursor_x;
                    cursor_y = alt_saved_cursor_y;
                    in_alt_screen = 0;
                    term_scrollback_reconfigure();
                    gui_scroll_set(&term_scroll_view, gui_scroll_max(&term_scroll_view));
                    term_scroll_sync_bottom();
                }
                break;
            // Mouse reporting. 1000 = press/release only, 1002 adds motion
            // while a button is held, 1003 reports every motion. They are
            // MUTUALLY EXCLUSIVE levels, not flags: a program that turns 1002
            // on and later turns 1000 off expects the mouse to go quiet.
            case 1000: g_term_modes.mouse_mode = on ? 1000 : 0; break;
            case 1002: g_term_modes.mouse_mode = on ? 1002 : 0; break;
            case 1003: g_term_modes.mouse_mode = on ? 1003 : 0; break;
            case 1006: g_term_modes.mouse_sgr = on; break;                   // SGR encoding
            case 1004:
                // FOCUS EVENTS. Recorded so the mode is not leaked, and
                // DELIBERATELY NEVER EMITTED: the kernel sends apps no focus or
                // blur event to source them from. EVENT_WINDOW_BLUR exists in
                // the tree exactly once, as the enum declaration, and the four
                // apps with a handler arm for it have never executed that arm.
                // Parsing a mode and then never firing it is honest; claiming
                // ?1004 support would not be. See docs/TERMINAL_EMULATION.md.
                g_term_modes.focus_events = on;
                break;
            case 2004: g_term_modes.bracketed_paste = on; break;
            // 1: DECCKM application cursor keys. 12: cursor blink. 1005/1015:
            // superseded mouse encodings this terminal does not emit.
            // Consumed deliberately: see docs/TERMINAL_EMULATION.md.
            default: break;
        }
    }
}

// ---------------------------------------------------------------------------
// Erase helpers
static void erase_row_span(int row, int from, int to) {
    if (row < 0 || row >= term_rows) return;
    if (from < 0) from = 0;
    if (to > term_cols - 1) to = term_cols - 1;
    for (int c = from; c <= to; c++) blank_cell(&cells[row][c]);
}

// ---------------------------------------------------------------------------
// csi: the main dispatch.
static void tcb_csi(void *ctx, const term_seq_t *sq) {
    (void)ctx;
    int n = term_emu_param(sq, 0, 1);
    if (n < 1) n = 1;
    int bot = term_scroll_bottom_eff();

    if (sq->priv == '?') {
        if (sq->final == 'h' || sq->final == 'l') { csi_private_mode(sq, sq->final == 'h'); return; }
        if (sq->final == 'p' && sq->ninter == 1 && sq->inter[0] == '$') {
            // DECRQM: report every mode as "not recognised" (0) rather than
            // leaving the query unanswered. A program that waits on a reply it
            // never gets hangs; one that is told "no" moves on.
            char r[32];
            int k = snprintf(r, sizeof(r), "\033[?%d;0$y", term_emu_param(sq, 0, 0));
            if (k > 0) term_reply(r, k);
            return;
        }
        return;   // other private finals: consumed
    }
    if (sq->priv == '>') {
        if (sq->final == 'c') term_reply("\033[>0;10;1c", 10);   // secondary DA
        return;                                                   // ESC[>4;2m etc: consumed
    }
    if (sq->priv) return;

    if (sq->ninter > 0) {
        if (sq->inter[0] == '!' && sq->final == 'p') { term_soft_reset(); return; }
        // ESC[ SP q (DECSCUSR cursor shape), ESC[ " q (DECSCA): consumed. The
        // cursor here is a fixed underline; honouring the shape request would
        // mean a cursor-style model this terminal does not have yet.
        return;
    }

    switch (sq->final) {
        // ---- cursor motion -------------------------------------------------
        case 'A': {   // CUU
            int top = (cursor_y >= scroll_top) ? scroll_top : 0;
            cursor_y -= n; if (cursor_y < top) cursor_y = top;
            break;
        }
        case 'B': {   // CUD
            int lim = (cursor_y <= bot) ? bot : term_rows - 1;
            cursor_y += n; if (cursor_y > lim) cursor_y = lim;
            break;
        }
        case 'C': case 'a':   // CUF / HPR
            cursor_x += n; if (cursor_x > term_cols - 1) cursor_x = term_cols - 1;
            break;
        case 'D':             // CUB
            cursor_x -= n; if (cursor_x < 0) cursor_x = 0;
            break;
        case 'E':             // CNL - n lines down, column 0
            cursor_y += n; if (cursor_y > term_rows - 1) cursor_y = term_rows - 1;
            cursor_x = 0;
            break;
        case 'F':             // CPL - n lines up, column 0
            cursor_y -= n; if (cursor_y < 0) cursor_y = 0;
            cursor_x = 0;
            break;
        case 'G': case '`':   // CHA / HPA - absolute column
            cursor_x = n - 1;
            if (cursor_x < 0) cursor_x = 0;
            if (cursor_x > term_cols - 1) cursor_x = term_cols - 1;
            break;
        case 'd':             // VPA - absolute row
            cursor_y = n - 1;
            if (cursor_y < 0) cursor_y = 0;
            if (cursor_y > term_rows - 1) cursor_y = term_rows - 1;
            break;
        case 'e':             // VPR - relative row
            cursor_y += n; if (cursor_y > term_rows - 1) cursor_y = term_rows - 1;
            break;
        case 'H': case 'f': { // CUP / HVP
            int row = term_emu_param(sq, 0, 1) - 1;
            int col = term_emu_param(sq, 1, 1) - 1;
            if (row < 0) row = 0;
            if (col < 0) col = 0;
            cursor_y = row < term_rows ? row : term_rows - 1;
            cursor_x = col < term_cols ? col : term_cols - 1;
            break;
        }
        case 'I': {           // CHT - forward n tab stops
            for (int k = 0; k < n; k++) {
                int x = cursor_x + 1;
                while (x < term_cols && !g_tabstop[x]) x++;
                cursor_x = (x < term_cols) ? x : term_cols - 1;
            }
            break;
        }
        case 'Z': {           // CBT - back n tab stops
            for (int k = 0; k < n; k++) {
                int x = cursor_x - 1;
                while (x > 0 && !g_tabstop[x]) x--;
                cursor_x = (x > 0) ? x : 0;
            }
            break;
        }

        // ---- erasure -------------------------------------------------------
        case 'J': {           // ED - honours its parameter now (it did not)
            int mode = term_emu_param(sq, 0, 0);
            if (mode == 0) {
                erase_row_span(cursor_y, cursor_x, term_cols - 1);
                for (int r = cursor_y + 1; r < term_rows; r++) erase_row_span(r, 0, term_cols - 1);
            } else if (mode == 1) {
                for (int r = 0; r < cursor_y; r++) erase_row_span(r, 0, term_cols - 1);
                erase_row_span(cursor_y, 0, cursor_x);
            } else if (mode == 2) {
                term_clear();
            } else if (mode == 3) {
                // ED 3 erases the SCROLLBACK too. Distinct from ED 2, and the
                // only sequence that is supposed to drop history.
                term_clear();
                sb_count = 0; sb_head = 0;
                term_scrollback_reconfigure();
                gui_scroll_set(&term_scroll_view, gui_scroll_max(&term_scroll_view));
                term_scroll_sync_bottom();
            }
            break;
        }
        case 'K': {           // EL - honours its parameter now (it did not)
            int mode = term_emu_param(sq, 0, 0);
            if (mode == 0)      erase_row_span(cursor_y, cursor_x, term_cols - 1);
            else if (mode == 1) erase_row_span(cursor_y, 0, cursor_x);
            else if (mode == 2) erase_row_span(cursor_y, 0, term_cols - 1);
            break;
        }
        case 'X':             // ECH - erase n characters in place, no shifting
            erase_row_span(cursor_y, cursor_x, cursor_x + n - 1);
            break;

        // ---- insert / delete / scroll --------------------------------------
        case 'L':             // IL
            for (int k = 0; k < n; k++) term_insert_line(cursor_y, scroll_top, bot);
            break;
        case 'M':             // DL
            for (int k = 0; k < n; k++) term_delete_line(cursor_y, scroll_top, bot);
            break;
        case '@':             // ICH
            term_insert_chars(cursor_y, cursor_x, n);
            break;
        case 'P':             // DCH
            term_delete_chars(cursor_y, cursor_x, n);
            break;
        case 'S':             // SU - scroll the region up n lines
            for (int k = 0; k < n; k++) term_scroll_region(scroll_top, bot);
            break;
        case 'T':             // SD - scroll the region down n lines
            for (int k = 0; k < n; k++) term_scroll_region_down(scroll_top, bot);
            break;
        case 'b':             // REP - repeat the last printed character n times
            for (int k = 0; k < n; k++) term_put_cp(g_last_print);
            break;

        // ---- tab stops -----------------------------------------------------
        case 'g': {           // TBC
            int mode = term_emu_param(sq, 0, 0);
            if (mode == 3) { for (int i = 0; i < TERM_MAX_COLS; i++) g_tabstop[i] = 0; }
            else if (cursor_x >= 0 && cursor_x < TERM_MAX_COLS) g_tabstop[cursor_x] = 0;
            break;
        }

        // ---- rendition -----------------------------------------------------
        case 'm':
            term_emu_sgr(&g_pen, sq);
            break;

        // ---- reports -------------------------------------------------------
        case 'c':             // DA - primary device attributes
            // VT220 with 132 columns, selective erase and colour. A program
            // that queries DA and is never answered can block.
            term_reply("\033[?62;1;6;22c", 13);
            break;
        case 'n': {           // DSR
            int q = term_emu_param(sq, 0, 0);
            if (q == 6) {
                char r[32];
                int k = snprintf(r, sizeof(r), "\033[%d;%dR", cursor_y + 1, cursor_x + 1);
                if (k > 0) term_reply(r, k);
            } else if (q == 5) {
                term_reply("\033[0n", 4);
            }
            break;
        }

        // ---- scroll region / cursor save ------------------------------------
        case 'r': {           // DECSTBM
            int top = term_emu_param(sq, 0, 1) - 1;
            int b   = term_emu_param(sq, 1, term_rows) - 1;
            if (top < 0) top = 0;
            if (top >= term_rows) top = term_rows - 1;
            if (b >= term_rows) b = term_rows - 1;
            if (top < b) { scroll_top = top; scroll_bottom = b; }
            else         { scroll_top = 0;   scroll_bottom = -1; }
            cursor_x = 0;
            cursor_y = scroll_top;
            break;
        }
        case 's':             // SCOSC
            saved_cursor_x = cursor_x; saved_cursor_y = cursor_y;
            break;
        case 'u':             // SCORC
            cursor_x = saved_cursor_x; cursor_y = saved_cursor_y;
            if (cursor_x >= term_cols) cursor_x = term_cols - 1;
            if (cursor_y >= term_rows) cursor_y = term_rows - 1;
            break;

        // ---- modes / window ops we deliberately consume ---------------------
        case 'h': case 'l':   // ANSI modes: 4 (IRM), 20 (LNM). Not modelled.
        case 't':             // window manipulation (resize/report). Not modelled.
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// osc: OSC 0/1/2 set the window title. The old parser had no OSC state at all,
// so every title a shell set printed as visible text on the prompt line.
static void tcb_osc(void *ctx, const char *str, int len) {
    (void)ctx; (void)len;
    int i = 0, code = 0, has = 0;
    while (str[i] >= '0' && str[i] <= '9') { code = code * 10 + (str[i] - '0'); i++; has = 1; }
    if (!has || str[i] != ';') return;
    i++;
    if (code == 0 || code == 2) {
        // 0 = icon+title, 2 = title only. RECORDED, NOT APPLIED: window chrome
        // for a kernel-WM window is drawn by the kernel and there is no
        // set-title syscall for it (grepped: SYS_WIN_* has no title op; only
        // the compositor-client path has comp_window_set_title(), which this
        // app does not use). The valuable half of OSC support is that the
        // title no longer PRINTS ITSELF onto the prompt, and that half is
        // real; the title bar itself needs a kernel syscall and is a separate
        // ticket. Storing it means that ticket is a one-line change here.
        int k = 0;
        while (str[i + k] && k < (int)sizeof(g_osc_title) - 1) { g_osc_title[k] = str[i + k]; k++; }
        g_osc_title[k] = 0;
    }
    // 4 (palette), 10/11 (fg/bg query), 52 (clipboard) and the rest are
    // consumed. Consuming is the whole point: an unimplemented OSC must be
    // invisible, not printed.
}

static const term_cb_t g_term_cb = {
    tcb_print, tcb_execute, tcb_csi, tcb_esc, tcb_osc, 0
};

// Feed bytes to the emulator. Everything the terminal displays - pty output and
// this app's own messages alike - goes through here, so there is exactly ONE
// interpretation of a sequence in the program.
term_parser_t g_parser;
static int g_parser_ready = 0;
void term_feed(const unsigned char *b, int n) {
    if (!g_parser_ready) { term_emu_reset(&g_parser); term_tabstops_default(g_tabstop); g_parser_ready = 1; }
    term_emu_feed(&g_parser, b, n, &g_term_cb, 0);
}

// Put a character to terminal (kept for this app's own printf-style output).
void term_putc(char c) {
    unsigned char b = (unsigned char)c;
    term_feed(&b, 1);
}


// Print a string
void term_puts(const char *str) {
    while (*str) term_putc(*str++);
}

// Print an integer
void term_put_int(int num) {
    char buf[20];
    int_to_str(num, buf);
    term_puts(buf);
}
