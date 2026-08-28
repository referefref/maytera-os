// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// term_emu.h - the MayteraOS Terminal EMULATION CORE.
//
// WHY THIS FILE EXISTS
//
// Until now the whole escape-sequence parser lived inside
// userland/apps/terminal/main.c as one 250-line `handle_escape_char()` switch
// that was simultaneously the tokenizer, the SGR interpreter and the grid
// mutator. Three consequences, all measured (see docs/TERMINAL_EMULATION.md):
//
//   1. It could not be tested. Every claim about a sequence had to be made by
//      running a whole TUI in a VM and looking at it, which is why the tier-1
//      pass could prove "vi works" but could not prove insert/delete-line.
//   2. It leaked. Anything the switch did not recognise as a FINAL byte was
//      treated as a final byte, so the REST of the sequence printed as text.
//      `ESC ] 0 ; title BEL` (every shell that sets a window title) put
//      `0;title` on the screen. So did `ESC ( B`, `ESC [ > 4 ; 2 m`,
//      `ESC [ SP q` and `ESC [ ! p`.
//   3. It had no notion of a codepoint. `term_putc(char c)` stored only
//      `c >= ' ' && c < 127`; `char` is signed here, so EVERY byte of every
//      UTF-8 sequence was silently DISCARDED. (docs/TERMINAL_PARITY.md said
//      these rendered "as their individual raw bytes (mojibake)". They did
//      not; they vanished. Measured, not assumed - see the host test.)
//
// This module is the tokenizer and the state that belongs with it, and NOTHING
// ELSE. It draws nothing, it owns no grid, it makes no syscalls, and it does
// not include gui.h. That is deliberate: it compiles unmodified with a hosted
// gcc, so `tests/term_emu_test.c` can drive every sequence directly and assert
// on the parse, which is the evidence the brief asked for. The grid mutation
// stays in main.c (or moves to whatever the chrome restructure produces); this
// module hands it a fully-parsed, never-leaking sequence record.
//
// THE STATE MACHINE IS PAUL WILLIAMS' VT500 PARSER, not an ad-hoc switch. That
// is the whole point of the rewrite: a table-driven machine cannot leak,
// because every byte in every state has a defined destination. An ad-hoc
// switch leaks by omission, silently, and only for the sequences nobody
// happened to test. Reference: https://vt100.net/emu/dec_ansi_parser
//
// WHAT THE CALLER STILL OWNS
//   - the cell grid, cursor, scroll region, scrollback, alternate screen
//   - drawing, fonts, colour schemes
//   - writing replies (DSR/DA/mouse) to the pty master
// This module tells the caller WHAT arrived. It never decides what it means to
// the screen.

#ifndef _TERM_EMU_H
#define _TERM_EMU_H

// The host test (tests/term_emu_test.c) compiles this module with a HOSTED
// gcc, where libc/types.h's own off_t/size_t typedefs collide with the system
// headers. That collision is the only thing standing between "this module is
// testable" and "this module is not", so it gets an explicit switch rather
// than a clever guess about __STDC_HOSTED__ (which is 1 in the freestanding
// app build too).
#ifdef TERM_EMU_HOSTTEST
#include <stdint.h>
#else
#include "../../libc/types.h"
#endif

// ---------------------------------------------------------------------------
// Colour representation
//
// The old cell carried `uint8_t fg, bg` - a 4-bit ANSI index in an 8-bit box.
// That is why `38;5;n` and `38;2;r;g;b` were "silently ignored parameters"
// (docs/TERMINAL_PARITY.md tier 3) even though the terminal advertises
// TERM=maytera-256color. A tagged 32-bit colour carries all three cases the
// SGR grammar can express, and - this is the part the old uint8_t could not do
// at all - it distinguishes "default" from "index 0 (black)" and from
// "index 7 (white)".
//
// That distinction is not pedantry. The old draw path had to guess:
//     if (cell->bg == 0) bg = term_bg_color();
//     if (cell->fg == 7) fg = term_fg_color();
// which means a program that explicitly asked for ANSI black-on-white got the
// THEME's background and foreground instead, and could not be told apart from
// a cell nobody had coloured. SGR 39/49 (default fg/bg) - which the old parser
// did not implement at all - now has something to set.
//
//   kind 0: TE_COL_DEFAULT     -> the terminal's own default fg/bg
//   kind 1: indexed 0..255     -> ansi_colors[] for 0..15, the 6x6x6 cube for
//                                 16..231, the 24-step ramp for 232..255
//   kind 2: direct 24-bit RGB
#define TE_COL_DEFAULT    0u
#define TE_COL_KIND(c)    ((unsigned)(c) >> 24)
#define TE_COL_KIND_DEF   0u
#define TE_COL_KIND_IDX   1u
#define TE_COL_KIND_RGB   2u
#define TE_COL_IDX(i)     (0x01000000u | ((unsigned)(i) & 0xFFu))
#define TE_COL_RGB(r,g,b) (0x02000000u | (((unsigned)(r) & 0xFFu) << 16) \
                                       | (((unsigned)(g) & 0xFFu) << 8)  \
                                       |  ((unsigned)(b) & 0xFFu))
#define TE_COL_VALUE(c)   ((unsigned)(c) & 0x00FFFFFFu)

// ---------------------------------------------------------------------------
// Cell attributes. One bit each, so a cell carries them ALL simultaneously.
//
// The old parser had no attribute storage whatsoever, so it faked bold with
// `current_fg |= 0x08` (destructive: it cannot be undone by SGR 22, and
// `ESC[1m ESC[31m` produced plain red because the later `31` overwrote the
// whole field including the bit that meant bold), and it faked reverse by
// SWAPPING current_fg/current_bg at parse time (so a colour set AFTER
// `ESC[7m` landed in the wrong slot, and SGR 0 could not restore the pair).
// Both bugs disappear by construction once the attribute is a bit and the
// swap happens at DRAW time.
#define TE_ATTR_BOLD      0x01u
#define TE_ATTR_DIM       0x02u
#define TE_ATTR_ITALIC    0x04u
#define TE_ATTR_UNDERLINE 0x08u
#define TE_ATTR_BLINK     0x10u
#define TE_ATTR_REVERSE   0x20u
#define TE_ATTR_HIDDEN    0x40u
#define TE_ATTR_STRIKE    0x80u

// The current text rendition. One of these is the "pen"; a copy of it is what
// DECSC/DECRC saves and restores (the old code saved only fg/bg, so a
// save/restore across `ESC[1m` lost the bold).
typedef struct {
    uint32_t fg;      // TE_COL_*
    uint32_t bg;      // TE_COL_*
    uint8_t  attr;    // TE_ATTR_* bitset
} term_sgr_t;

// ---------------------------------------------------------------------------
// One grid cell.
//
// 16 bytes (was 3). At TERM_MAX_COLS=170 that is 2.7 KB per row: the two
// static grids cost 141 KB each and the 2000-line scrollback ring costs
// 5.4 MB, allocated with malloc() on a 512 MB demand-mapped userland heap
// (userland/libc/stdlib.c). Measured against the alternative of a side table
// for non-ASCII glyphs: a side table saves memory that is not scarce and buys
// an aliasing bug that is, because scrollback rows are memcpy'd.
//
// ch == 0 is the RIGHT HALF of a double-width character whose left half is in
// the previous column. It is never a printable value, blank cells hold ' ',
// and every writer either sets both halves or neither, so a half-overwritten
// wide character cannot exist (see term_emu_wcwidth()).
typedef struct {
    uint32_t ch;      // Unicode codepoint, or 0 = wide-char continuation
    uint32_t fg;      // TE_COL_*
    uint32_t bg;      // TE_COL_*
    uint8_t  attr;    // TE_ATTR_*
    uint8_t  pad[3];
} term_cell_t;

// ---------------------------------------------------------------------------
// A fully-parsed sequence handed to the caller.
//
// `params` are ALWAYS present and ALWAYS defaulted: an omitted parameter reads
// back as -1 so the caller can apply the sequence's OWN default (which is 1
// for CUU but 0 for ED and 0 for SGR), instead of the old code's habit of
// storing 0 and then writing `p > 0 ? p : 1` at forty call sites and getting
// it wrong at three of them (ED and EL both ignored their parameter entirely).
#define TE_MAX_PARAMS   32
#define TE_MAX_INTER    2
#define TE_MAX_STR      512   // OSC / DCS payload cap; longer is truncated, never overflowed

typedef struct {
    char final;                     // the final byte: 'm', 'H', 'J', ...
    char priv;                      // '?', '>', '<', '=' or 0   (private marker)
    char inter[TE_MAX_INTER + 1];   // intermediates: ' ', '!', '#', '$', '"' ...
    int  ninter;
    int  params[TE_MAX_PARAMS];     // -1 = omitted (use the sequence's default)
    // sep[i] is the separator that ENDED params[i]: ';' or ':'. The colon form
    // is how ITU T.416 / xterm write sub-parameters, and 38:2:r:g:b is emitted
    // by real programs (libvte, kitty, alacritty), so it cannot be treated as
    // a semicolon: `38:2:1:2:3` is ONE colour, `38;2;1;2;3` is also one colour,
    // but `4:3` (curly underline) and `4;3` (underline THEN italic) are
    // different things entirely.
    char sep[TE_MAX_PARAMS];
    int  nparams;
} term_seq_t;

// ---------------------------------------------------------------------------
// Callbacks. Any may be NULL; the parser then discards that class of event
// (still consuming it correctly - a NULL callback must never cause a leak).
typedef struct {
    void (*print)(void *ctx, uint32_t cp);              // one decoded codepoint
    void (*execute)(void *ctx, uint8_t c);              // a C0/C1 control byte
    void (*csi)(void *ctx, const term_seq_t *s);        // CSI ... final
    void (*esc)(void *ctx, const term_seq_t *s);        // ESC [inter] final
    void (*osc)(void *ctx, const char *s, int len);     // OSC payload (no ESC/BEL/ST)
    void (*dcs)(void *ctx, const term_seq_t *s,
                const char *payload, int len);          // DCS ... ST
} term_cb_t;

// ---------------------------------------------------------------------------
// Parser state. Zero it, or call term_emu_reset(), before first use.
typedef struct {
    int state;

    // in-flight sequence
    term_seq_t seq;
    int cur_param;        // index being accumulated
    int cur_has_digit;    // has the current slot seen a digit yet?

    // OSC / DCS string accumulator
    char str[TE_MAX_STR];
    int  str_len;
    int  str_trunc;
    int  str_from;        // which string state an in-flight ESC interrupted

    // incremental UTF-8 decoder. A pty read can split a multi-byte sequence
    // across two reads; a decoder that lives in a local variable inside the
    // read loop corrupts exactly those characters and only under load, which
    // is the worst possible failure mode to debug. State lives here.
    uint32_t u_cp;
    int      u_need;      // continuation bytes still expected
    int      u_seen;
    uint32_t u_min;       // smallest value legal for this length (overlong check)
} term_parser_t;

// Reset to GROUND with no in-flight sequence and no partial UTF-8 character.
void term_emu_reset(term_parser_t *p);

// Feed raw bytes. Never blocks, never allocates, never leaks a byte to print().
void term_emu_feed(term_parser_t *p, const unsigned char *buf, int len,
                   const term_cb_t *cb, void *ctx);

// Read a parameter with a per-sequence default applied. `i` past the end, or
// an omitted parameter, both yield `def`.
int term_emu_param(const term_seq_t *s, int i, int def);

// ---------------------------------------------------------------------------
// Display width of a codepoint: 0 (combining / zero-width), 1 (normal),
// 2 (East Asian Wide or Fullwidth), or -1 (a control character, nothing to
// draw). Getting this wrong does not merely look wrong: a wide character
// written into one cell desynchronises every subsequent column on the row
// against what the application believes it drew, and the corruption surfaces
// later as "random rendering bugs" in an unrelated place.
int term_emu_wcwidth(uint32_t cp);

// Apply an SGR sequence to a pen. Handles 0/1/2/3/4/5/7/8/9, the 2x/29
// disables, 30-37/40-47/90-97/100-107, 39/49 (default), and BOTH the
// semicolon and colon forms of 38/48 with `5;n` (256-index) and `2;r;g;b`
// (direct RGB), including the 6-subparameter `38:2:<cs>:r:g:b` colour-space
// variant. Unknown parameters are skipped, never mis-consumed.
void term_emu_sgr(term_sgr_t *pen, const term_seq_t *s);

// The default pen: default fg, default bg, no attributes.
void term_emu_sgr_reset(term_sgr_t *pen);

// ---------------------------------------------------------------------------
// Mouse reporting encoders (the caller decides WHEN; this decides HOW).
//
// `btn` is the raw xterm button code (0 left, 1 middle, 2 right, 3 release,
// 64/65 wheel up/down), already OR'd with any modifier bits and the +32 motion
// bit by the caller. `col`/`row` are 0-based; both encoders add the 1-based
// bias themselves.
//
// Returns the number of bytes written to `out` (which must have room for at
// least 32), or 0 if the position cannot be represented in the legacy encoding
// (col/row > 222), which is exactly why SGR (?1006) exists and why a terminal
// that only implements the legacy form breaks on a window wider than 223
// columns.
// ---------------------------------------------------------------------------
// DEC PRIVATE MODE STATE - ONE COPY, SHARED.
//
// These are the modes an application turns on and off, and more than one part
// of the terminal has to READ them:
//
//   - the pty loop, to decide whether a mouse event becomes a report
//   - the SELECTION code (term_select), to decide whether a click selects text
//     or belongs to the application
//   - the paste path, to decide whether to wrap the paste in ESC[200~/ESC[201~
//
// It lives HERE, in the emulation core that parses the modes, and every reader
// goes through the accessors below. The alternative - each component tracking
// "is mouse reporting on" for itself - is the multi-copy fault this project
// keeps paying for, and it would show up as selection and reporting disagreeing
// about who owns a click, which is a bug nobody would find by reading either
// file alone.
//
// THE SELECTION/REPORTING CONVENTION, so both sides implement the same one:
// while mouse reporting is on, a bare click belongs to the APPLICATION; holding
// SHIFT overrides that and gives the click to local selection. That is what
// xterm, Konsole and every other terminal do, and it is buildable now that
// modifier state is queryable at event time (userland/libc/gui_mods.h).
typedef struct {
    // 0 = off, else 1000 (press/release), 1002 (+drag), 1003 (any motion).
    // These are LEVELS, not flags: a program that enables 1002 and later
    // disables 1000 expects the mouse to go quiet.
    int mouse_mode;
    int mouse_sgr;          // ?1006: SGR encoding rather than the legacy form
    int bracketed_paste;    // ?2004
    int focus_events;       // ?1004 - PARSED AND RECORDED, NEVER EMITTED. See below.
} term_modes_t;

// The one instance. Defined in term_emu.c.
extern term_modes_t g_term_modes;

// Is the application currently asking to be told about the mouse? Any caller
// deciding "does this click belong to the app or to local selection" must use
// THIS, not a private copy.
static inline int term_emu_mouse_reporting(void) { return g_term_modes.mouse_mode != 0; }
// Does the application want motion reported while a button is held (?1002) or
// unconditionally (?1003)?
static inline int term_emu_mouse_motion(void)    { return g_term_modes.mouse_mode >= 1002; }
static inline int term_emu_mouse_any_motion(void){ return g_term_modes.mouse_mode == 1003; }
// Should a paste be wrapped in ESC[200~ / ESC[201~?
static inline int term_emu_bracketed_paste(void) { return g_term_modes.bracketed_paste != 0; }

// Clear every mode. Called on RIS and after a foreground child exits, so a
// program that crashed with mouse reporting on cannot leave the SHELL PROMPT
// emitting mouse bytes as text.
void term_emu_modes_reset(void);

int term_emu_mouse_x10(char *out, int btn, int col, int row);
int term_emu_mouse_sgr(char *out, int btn, int col, int row, int release);

// Self-check for the wcwidth range tables: 1 if both are strictly ascending
// and non-overlapping, 0 otherwise. term_emu_wcwidth() binary-searches them,
// so an out-of-order entry does not fail loudly, it silently returns the wrong
// width for a whole block of characters. Asserted by the host test.
int term_emu_tables_sorted(void);

#endif // _TERM_EMU_H
