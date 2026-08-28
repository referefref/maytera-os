// term_render.c
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.

#include "term_common.h"
#include "term_grid.h"
#include "term_scrollback.h"
#include "term_theme.h"
#include "term_emu.h"
#include "term_render.h"
#include "term_select.h"   // #221: selection highlight colours
#include "term_search.h"   // #221: find-in-scrollback match colours
#include "term_notify.h"

// The compositor window handle. Declared in term_render.h.
int window_handle = -1;

// Chrome hooks (see term_render.h). A fixed table, not a list: at most a
// handful of chrome modules can exist in one terminal window, and a bounded
// array cannot fail to allocate in a draw path.
static void (*g_chrome_hooks[TERM_CHROME_HOOK_MAX])(void);
static int g_chrome_hook_count = 0;

int term_render_add_chrome_hook(void (*fn)(void)) {
    if (!fn || g_chrome_hook_count >= TERM_CHROME_HOOK_MAX) return -1;
    g_chrome_hooks[g_chrome_hook_count++] = fn;
    return 0;
}

void term_render_draw_chrome(void) {
    for (int i = 0; i < g_chrome_hook_count; i++) g_chrome_hooks[i]();
}
// ---------------------------------------------------------------------------
// Colour resolution.
//
// A cell colour is one of three things (term_emu.h): DEFAULT, an index 0..255,
// or 24-bit RGB. Only the DEFAULT case consults the terminal's own theme /
// colour scheme, which is the distinction the old `if (cell->bg == 0)` /
// `if (cell->fg == 7)` guess could not make: a program that explicitly asked
// for ANSI black-on-white got the THEME's colours instead of the ones it named.

// The standard xterm 256-colour layout: 0-15 the scheme's ANSI slots, 16-231 a
// 6x6x6 cube on the levels {0,95,135,175,215,255}, 232-255 a 24-step grey ramp
// from 8 to 238. These are the published constants, not an approximation; a
// terminal that gets the cube levels wrong makes every 256-colour theme in
// existence subtly the wrong shade.
static uint32_t xterm256_rgb(int idx) {
    static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
    if (idx < 0) idx = 0;
    if (idx < 16) return ansi_colors[idx];
    if (idx < 232) {
        int i = idx - 16;
        return ((uint32_t)lv[(i / 36) % 6] << 16) |
               ((uint32_t)lv[(i / 6) % 6]  << 8)  |
                (uint32_t)lv[i % 6];
    }
    if (idx > 255) idx = 255;
    uint32_t g = (uint32_t)(8 + (idx - 232) * 10);
    return (g << 16) | (g << 8) | g;
}

static uint32_t te_resolve(uint32_t col, int is_fg) {
    switch (TE_COL_KIND(col)) {
        case TE_COL_KIND_IDX: return xterm256_rgb((int)TE_COL_VALUE(col));
        case TE_COL_KIND_RGB: return TE_COL_VALUE(col);
        default:              return is_fg ? term_fg_color() : term_bg_color();
    }
}

// SGR 2 (dim / faint). Half-intensity toward the background is what xterm does;
// a fixed grey would be invisible on a light colour scheme.
static uint32_t blend_rgb(uint32_t a, uint32_t b, int num, int den) {
    int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF), bg = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    int r = ar + (br - ar) * num / den;
    int g = ag + (bg - ag) * num / den;
    int bl = ab + (bb - ab) * num / den;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}


// ===========================================================================
// GLYPH RASTERISATION AND CELL COMPOSITION
//
// EVERY cell, ASCII included, is composited in USERLAND and uploaded as
// pixels. It did not used to be: ASCII went through the kernel's
// win_draw_text_ttf_ex() and only non-ASCII was composited here. Two separate
// reasons forced the change, and the first one is the important one.
//
// 1. THE KERNEL'S TEXT PATH LETS GLYPH INK LEAVE ITS CELL, AND THAT MAKES
//    DAMAGE TRACKING IMPOSSIBLE TO GET RIGHT.
//    sys_win_draw_text_ttf_ex() (kernel/proc/syscall.c) draws a glyph at
//    `baseline + g->yoff` for `g->height` rows and clips ONLY to the window:
//      for (int row = 0; row < g->height; row++) {
//          int py = gy + row; if (py < 0 || py >= ch) continue;      // ch = WINDOW height
//    So a glyph whose ink box is taller than TERM_CHAR_H (which happens at
//    almost every font size, because the cell height is ascent-descent+gap and
//    individual glyphs can exceed it) writes into the NEIGHBOURING CELL. When
//    that neighbour is repainted the stray ink is erased by its background
//    fill; when it is not, the ink stays.
//
//    A full-grid repaint therefore hides the problem completely: every cell is
//    background-filled every frame, so no stale ink can survive. Damage
//    tracking exposes it: a cell whose glyph changes leaves the part of its
//    OLD ink that lay outside its own box behind forever, because the only
//    thing that would have erased it was the neighbour's background fill.
//
//    MEASURED on VM <vmid>, golden 2066, 80x24 pane (build with the kernel text
//    path + damage tracking): after `help` twice, PageUp then PageDown left 42
//    pixels differing from the same screen painted in full, and the count was
//    still exactly 42 after five more cycles - a stable residue, not a
//    compounding one, which is exactly what "old ink nobody erases" looks
//    like. The same build, with no scrolling at all, differed from a forced
//    full repaint of identical content by 45 pixels. The SHIPPED (full-repaint)
//    terminal measured 0 on the identical test, so this was a real regression
//    and not measurement noise.
//
//    Expanding the damage by a ring of cells does NOT fix it: repainting the
//    ring erases the ink the ring's OWN neighbours had put on it, so the error
//    moves outward by one cell per ring and never disappears. The only fix
//    that terminates is to stop the ink leaving the cell at all, and there is
//    no clipped text syscall - there is no scissor of any kind on window
//    drawing, checked, not assumed.
//
//    Compositing here clips each glyph to its own cell BY CONSTRUCTION, so the
//    order cells are painted in stops mattering and a damage repaint and a
//    full repaint produce identical pixels. That is what makes #220's
//    "PageUp then PageDown is pixel-identical" hold under damage tracking.
//
// 2. IT ALSO COLLAPSES THE SYSCALL COUNT, which is the reported complaint.
//    The old path issued one win_draw_rect PLUS one win_draw_text_ttf_ex per
//    non-blank cell: up to 14,400 syscalls for a full-screen repaint. This
//    issues ONE win_draw_image for the whole damaged rectangle.
//
// THE PIXELS ARE IDENTICAL, not merely similar. SYS_FONT_GLYPH hands back the
// output of the SAME ttf_get_glyph_f() the kernel's text path rasterises with,
// term_ascent is the SAME ttf_get_metrics_f() ascent it uses for its baseline
// (term_apply_font(), term_theme.c), and the blend below is a line-for-line
// copy of the kernel's, including its `a >= 250 -> write the colour solid`
// shortcut. Anything less than a copy would have changed how every character
// in the terminal looks.
// ===========================================================================

// Direct-mapped glyph cache. Direct-mapped (not LRU) on purpose: no bookkeeping
// to get wrong, and a collision costs one re-rasterise, not a correctness bug.
// 512 slots rather than the 128 that were enough when only non-ASCII came
// through here: ASCII now does too, and 95 printable ASCII codepoints in a
// 128-slot table keyed on `cp & 127` would collide with every Latin-1
// accented character an actual document contains.
#define GC_SLOTS 512
#define GC_SCRATCH (192 * 192)     /* rasteriser output cap; font size caps at 96 */
typedef struct {
    uint32_t cp;
    int face, size, style;
    font_glyph_meta_t m;
    unsigned char *bmp;            // malloc'd m.width*m.height, or NULL for a blank glyph
    int valid;
} gcache_t;
static gcache_t g_gc[GC_SLOTS];
static unsigned char g_gc_scratch[GC_SCRATCH];

static gcache_t *glyph_get(uint32_t cp, int face, int size, int style) {
    gcache_t *e = &g_gc[cp & (GC_SLOTS - 1)];
    if (e->valid && e->cp == cp && e->face == face && e->size == size && e->style == style)
        return e;
    if (e->bmp) { free(e->bmp); e->bmp = NULL; }
    e->cp = cp; e->face = face; e->size = size; e->style = style; e->valid = 1;
    e->m.width = e->m.height = e->m.xoff = e->m.yoff = e->m.advance = 0;
    int adv = font_glyph(face, size, style, (int)cp, &e->m, g_gc_scratch, (int)sizeof(g_gc_scratch));
    if (adv < 0 || e->m.width <= 0 || e->m.height <= 0) { e->m.width = e->m.height = 0; return e; }
    long n = (long)e->m.width * (long)e->m.height;
    if (n > (long)sizeof(g_gc_scratch)) { e->m.width = e->m.height = 0; return e; }
    e->bmp = (unsigned char *)malloc((unsigned long)n);
    if (!e->bmp) { e->m.width = e->m.height = 0; return e; }
    for (long i = 0; i < n; i++) e->bmp[i] = g_gc_scratch[i];
    return e;
}

// The kernel's own blend (sys_win_draw_text_ttf_ex), copied so that a glyph
// composited here and the same glyph drawn by the kernel produce the same
// bytes. The `a >= 250` shortcut is part of that, not an optimisation.
static void blend_px(uint32_t *d, uint32_t color, unsigned a) {
    if (a >= 250) { *d = color; return; }
    unsigned cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;
    uint32_t bg = *d;
    unsigned br = (bg >> 16) & 0xFF, bgc = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    unsigned inv = 255u - a;
    *d = (((cr * a + br * inv) / 255u) << 16) |
         (((cg * a + bgc * inv) / 255u) << 8) |
          ((cb * a + bb * inv) / 255u);
}

// Composite ONE cell into `buf` (a bw x bh BGRA image) with its top-left at
// (dx, dy). Everything is clipped to the intersection of the cell's own box
// and the buffer, so a cell whose box lies partly (or wholly) outside the
// damaged rectangle contributes only the part that belongs there - which is
// what lets a double-width character whose LEAD sits left of the damaged
// rectangle still paint its right half.
static void compose_cell(uint32_t *buf, int bw, int bh, int dx, int dy,
                         const term_cell_desc_t *d) {
    if (!d || d->ch == 0) return;           // right half: its lead paints both columns
    int cw = (d->wide ? d->wide : 1) * TERM_CHAR_W;
    int chh = TERM_CHAR_H;
    if (cw <= 0 || chh <= 0) return;

    // Background, then the cursor's own fill if it sits here. A block cursor
    // fills the cell and the glyph is then drawn in the cell's BACKGROUND
    // colour, which is how every terminal keeps the character under a block
    // cursor readable.
    uint32_t fill = d->bg;
    uint32_t ink  = d->fg;
    int bar = 0, underbar = 0;
    if (d->cursor) {
        int shape = (int)d->cursor - 1;
        uint32_t cc = term_cursor_color();
        if (shape == TERM_CURSOR_BLOCK) { fill = cc; ink = d->bg; }
        else if (shape == TERM_CURSOR_BAR) bar = 1;
        else underbar = 1;
    }
    for (int r = 0; r < chh; r++) {
        int py = dy + r; if (py < 0 || py >= bh) continue;
        uint32_t *row = buf + (long)py * bw;
        for (int c = 0; c < cw; c++) {
            int px = dx + c; if (px < 0 || px >= bw) continue;
            row[px] = fill;
        }
    }

    // The glyph, on the SAME baseline the kernel uses: y + ascent + yoff.
    if (d->ch != ' ') {
        gcache_t *e = glyph_get(d->ch, g_term_font.face, g_term_font.size, d->style);
        if (e->m.width > 0 && e->bmp) {
            int gx = e->m.xoff;
            int gy = term_ascent + e->m.yoff;
            for (int r = 0; r < e->m.height; r++) {
                int cy = gy + r;
                if (cy < 0 || cy >= chh) continue;      // CLIPPED TO THE CELL
                int py = dy + cy; if (py < 0 || py >= bh) continue;
                uint32_t *row = buf + (long)py * bw;
                for (int c = 0; c < e->m.width; c++) {
                    int cx = gx + c;
                    if (cx < 0 || cx >= cw) continue;   // CLIPPED TO THE CELL
                    int px = dx + cx; if (px < 0 || px >= bw) continue;
                    unsigned a = e->bmp[(long)r * e->m.width + c];
                    if (a) blend_px(&row[px], ink, a);
                }
            }
        }
    }

    // Decorations, all inside the cell's own box.
    if (d->deco & 1) {                       // underline: the historical 2px-up rule
        int ry = chh - 2;
        if (ry >= 0 && ry < chh) { int py = dy + ry;
            if (py >= 0 && py < bh) for (int c = 0; c < cw; c++) {
                int px = dx + c; if (px >= 0 && px < bw) buf[(long)py * bw + px] = ink; } }
    }
    if (d->deco & 2) {                       // strike-through
        int ry = chh / 2;
        if (ry >= 0 && ry < chh) { int py = dy + ry;
            if (py >= 0 && py < bh) for (int c = 0; c < cw; c++) {
                int px = dx + c; if (px >= 0 && px < bw) buf[(long)py * bw + px] = ink; } }
    }
    if (bar) {                               // 2px vertical rule at the left edge
        uint32_t cc = term_cursor_color();
        for (int r = 0; r < chh; r++) { int py = dy + r; if (py < 0 || py >= bh) continue;
            for (int c = 0; c < 2 && c < cw; c++) { int px = dx + c;
                if (px >= 0 && px < bw) buf[(long)py * bw + px] = cc; } }
    }
    if (underbar) {                          // 2px bar on the baseline
        uint32_t cc = term_cursor_color();
        for (int r = chh - 2; r < chh; r++) { if (r < 0) continue;
            int py = dy + r; if (py < 0 || py >= bh) continue;
            for (int c = 0; c < TERM_CHAR_W && c < cw; c++) { int px = dx + c;
                if (px >= 0 && px < bw) buf[(long)py * bw + px] = cc; } }
    }
}

// ===========================================================================
// (#damage) DAMAGE TRACKING
//
// THE PROBLEM, as reported from real use: an SSH session running a TUI that
// updates a spinner and a status line several times a second forced a repaint
// of EVERY cell of the grid at that rate, because term_redraw() cleared the
// content area and then painted all term_rows * term_cols cells. On an 80x24
// pane that is 1920 win_draw_rect syscalls plus up to 1920 win_draw_text_ttf_ex
// syscalls per update; on a full-screen 160x45 pane it is 7200 of each. The
// pty pump runs at 10 ms while a child is alive, so the ceiling was ~100 of
// those per second.
//
// WHY IT IS SO MUCH WORSE ON REAL HARDWARE THAN IN A VM, and why the cell
// count is the honest metric rather than wall-clock in a VM:
//
//   - win_invalidate() is not a flag. sys_win_invalidate() calls
//     uw_commit_content(), a memcpy of the WHOLE content_width*content_height
//     buffer, and window_invalidate() -> window_draw() runs synchronously in
//     the caller's own context. MEASURED (blame.md, VM <vmid>, golden 1025):
//     83.4 us for one small draw plus one invalidate, against 134 ns for a
//     bare syscall. The present is the most expensive thing in the loop.
//   - Worse, the NON-ASCII cell path self-commits. sys_win_draw_image()
//     (kernel/proc/syscall.c) ends with wm_invalidate_rect_async() over the
//     whole window bounds AND uw_commit_content(), so EVERY non-ASCII cell
//     drawn costs one full-window commit. A TUI whose frame is box-drawing
//     characters therefore paid one whole-window memcpy per border cell, per
//     frame. Damage tracking is what makes that cost vanish: a border that did
//     not change is not drawn at all.
//
// THE CONSTRUCTION. Every cell's fully resolved descriptor (term_render.h) is
// kept from the last paint, per pane, and a cell is painted only when its
// descriptor differs. A shadow COMPARISON rather than dirty flags set at the
// mutation sites, deliberately: `cells[...]` is written directly at 39 sites
// across term_grid.c, term_parse.c, term_pty.c, term_search.c and
// term_select.c, and a scheme where each of those must remember to mark
// damage is a scheme that will eventually miss one and show a stale cell. The
// comparison cannot miss one. It also catches, for free, the three things a
// mutation-site flag could not see at all: a colour-scheme change (the
// resolved colours move while term_cell_t does not), a selection or find-match
// arriving over an untouched cell, and the cursor moving.
//
// WHAT THIS DOES NOT FIX: SCROLLING. A one-line scroll moves every row's
// content up by one, so every row's descriptor changes and the viewport is
// repainted. There is no way around that here: the window drawing syscalls can
// upload pixels from userland (SYS_WIN_DRAW_IMAGE / SYS_WIN_BLIT) but there is
// no primitive that copies a rect WITHIN a window, and userland cannot read
// its own window back to shift it itself. The saving is still large in
// practice, because the case being complained about does not scroll: a
// full-screen TUI repaints in place (alternate screen, or cursor-addressed
// updates), and streaming output scrolls at the rate of NEW LINES, not at the
// rate of UPDATES, which is the whole difference. A scroll costs one viewport
// repaint per line; a spinner now costs one cell per tick instead of a grid.
// A "scroll rect within a window" syscall is the follow-up that would close
// it; it is kernel work and is deliberately not bundled here.
// ===========================================================================

// Pane 0 adopts a static array for the same reason it adopts static `cells`:
// the one-tab, one-pane terminal must allocate exactly what it always did.
// Only a SECOND pane costs a malloc (term_layout.c).
static term_cell_desc_t g_shadow0[TERM_MAX_ROWS][TERM_MAX_COLS];
term_cell_desc_t (*term_shadow)[TERM_MAX_COLS] = g_shadow0;
unsigned term_shadow_gen = 0;
gui_scroll_t term_sb_painted;
int          term_sb_painted_valid = 0;

// Bumped by term_render_invalidate_all(). Starts at 1 so that the initial
// per-pane generation of 0 is already stale and the very first term_redraw()
// of every pane is a full repaint, with no separate "first frame" case.
static unsigned g_shadow_gen_now = 1;
static int      g_suppress_present = 0;
static int      g_last_painted = 0;

unsigned long term_stat_frames = 0;
unsigned long term_stat_full_frames = 0;
unsigned long term_stat_cells_scanned = 0;
unsigned long term_stat_cells_painted = 0;
unsigned long term_stat_invalidates = 0;
unsigned long term_stat_frames_idle = 0;

void term_stat_reset(void) {
    term_stat_frames = 0;
    term_stat_full_frames = 0;
    term_stat_cells_scanned = 0;
    term_stat_cells_painted = 0;
    term_stat_invalidates = 0;
    term_stat_frames_idle = 0;
}

void term_render_invalidate_all(void) {
    g_shadow_gen_now++;
    // Wrap is not a correctness problem in the normal sense (a pane would have
    // to sit unpainted for 2^32 invalidations), but a pane whose banked
    // generation happened to equal the wrapped value would skip its full
    // repaint, so skip the one value that can collide with "never painted".
    if (g_shadow_gen_now == 0) g_shadow_gen_now = 1;
    term_sb_painted_valid = 0;
}

int term_render_last_painted(void) { return g_last_painted; }

void term_render_suppress_present(int on) { g_suppress_present = on ? 1 : 0; }

// ---------------------------------------------------------------------------
// THE ONE DEFINITION of what a cell resolves to. Both the comparison and the
// drawing go through this; see term_render.h for why that matters.
static void resolve_cell(const term_cell_t *src_row, int col, int dest_row,
                         unsigned cursor, term_cell_desc_t *d) {
    // Zero the whole record, padding included, so two descriptors that mean
    // the same thing always compare equal. (There is no padding in
    // term_cell_desc_t today; relying on that silently is how a field added
    // later starts producing spurious repaints.)
    for (unsigned i = 0; i < sizeof(*d); i++) ((unsigned char *)d)[i] = 0;
    d->cursor = (uint8_t)cursor;

    const term_cell_t *cell = &src_row[col];
    d->ch = cell->ch;
    // ch == 0 is the RIGHT HALF of a double-width character. Its left half
    // already painted both columns; drawing it again would paint the wide
    // glyph's background over its own right half. It resolves to "nothing is
    // drawn here", which is a CONSTANT - so a lead that changes repaints both
    // columns itself, and this half's own record never needs to move.
    if (cell->ch == 0) return;

    int cw = (term_emu_wcwidth(cell->ch) == 2) ? 2 : 1;
    if (col + cw > term_cols) cw = 1;
    d->wide = (uint8_t)cw;

    uint32_t fg = te_resolve(cell->fg, 1);
    uint32_t bg = te_resolve(cell->bg, 0);
    uint8_t at = cell->attr;

    // Reverse video is applied HERE, at draw time. See term_emu.h.
    if (at & TE_ATTR_REVERSE) { uint32_t t = fg; fg = bg; bg = t; }
    // xterm's long-standing convention: bold brightens an indexed 0-7 colour.
    // This is what the old `current_fg |= 0x08` was reaching for; the
    // difference is that it is no longer destructive, so SGR 22 can undo it.
    if ((at & TE_ATTR_BOLD) && TE_COL_KIND(cell->fg) == TE_COL_KIND_IDX) {
        unsigned idx = TE_COL_VALUE(cell->fg);
        if (idx < 8 && !(at & TE_ATTR_REVERSE)) fg = xterm256_rgb((int)idx + 8);
    }
    if (at & TE_ATTR_DIM)    fg = blend_rgb(fg, bg, 1, 2);
    if (at & TE_ATTR_HIDDEN) fg = bg;

    // #221 SELECTION HIGHLIGHT, applied AFTER every SGR effect (reverse,
    // bold-brighten, dim, hidden) so a selected cell is legible whatever
    // rendition it carries. term_select owns the screen-cell -> selection
    // mapping, so no argument had to be added here and no drawing code is
    // duplicated: a selected cell simply resolves to the COLOUR SCHEME's
    // selection_bg/selection_fg (gui_palette.h), two fields every shipped
    // /PALETTES/*.tpalette has always carried and nothing had ever read.
    // #221 FIND-IN-SCROLLBACK MATCH HIGHLIGHT, applied on the same hook and
    // for the same reason, and BEFORE the selection so an explicit selection
    // still wins where the two overlap. Like term_select, term_search owns the
    // screen-cell -> match mapping, so the renderer needs no new argument and
    // no drawing code is duplicated.
    {
        uint32_t mfg, mbg;
        if (term_search_cell_colors(dest_row, col, &mfg, &mbg)) { fg = mfg; bg = mbg; }
    }
    {
        uint32_t sfg, sbg;
        if (term_select_cell_colors(dest_row, col, &sfg, &sbg)) { fg = sfg; bg = sbg; }
    }

    int style = g_term_font.style_bits;
    if (at & TE_ATTR_BOLD)   style |= FONT_STYLE_BOLD;
    if (at & TE_ATTR_ITALIC) style |= FONT_STYLE_ITALIC;

    d->fg = fg;
    d->bg = bg;
    d->style = (uint8_t)style;
    d->deco = (uint8_t)(((at & TE_ATTR_UNDERLINE) ? 1 : 0) |
                        ((at & TE_ATTR_STRIKE)    ? 2 : 0));
}


// ---------------------------------------------------------------------------
// THE DAMAGED RECTANGLE, composed once and uploaded once.
//
// One win_draw_image() per frame, not one per cell. That matters twice over:
// sys_win_draw_image() ends in wm_invalidate_rect_async() AND uw_commit_content(),
// a memcpy of the ENTIRE content_width * content_height buffer, so it is the
// most expensive call the terminal can make and issuing one per cell (which the
// non-ASCII path used to do) meant a TUI drawn with box-drawing characters paid
// one whole-window memcpy per border cell per frame. MEASURED on the reference
// trace: 36,882 of them over 30 seconds before this work.
static uint32_t *g_compose = NULL;
static long      g_compose_cap = 0;      // in pixels

static uint32_t *compose_buf(long need) {
    if (need <= g_compose_cap) return g_compose;
    uint32_t *nb = (uint32_t *)malloc((unsigned long)need * 4u);
    if (!nb) return NULL;                 // caller falls back; see term_redraw()
    if (g_compose) free(g_compose);
    g_compose = nb; g_compose_cap = need;
    return g_compose;
}

// Paint ONE already-resolved cell, on its own. Every path that paints a single
// cell goes through this, which is not tidiness: the first version of the
// degraded path called a helper that re-resolved the cell WITHOUT its cursor
// mark, so a pane that fell back to per-cell painting silently lost its
// cursor. The host harness caught it (tests/run_damage.sh) precisely because
// it compares the two paths pixel for pixel.
static void paint_one(int col, int dest_row, const term_cell_desc_t *d) {
    if (!d || d->ch == 0) return;
    int cw = (d->wide ? d->wide : 1) * TERM_CHAR_W;
    long need = (long)cw * TERM_CHAR_H;
    uint32_t *buf = compose_buf(need);
    if (!buf) return;
    for (long i = 0; i < need; i++) buf[i] = d->bg;
    compose_cell(buf, cw, TERM_CHAR_H, 0, 0, d);
    win_draw_image(window_handle,
                   term_origin_x + col * TERM_CHAR_W + 2,
                   term_origin_y + dest_row * TERM_CHAR_H + 2,
                   cw, TERM_CHAR_H, buf);
}

// Draw a single cell of `src_row` at screen row `dest_row`, on its own.
// (#damage) Retained as the module's public single-cell entry point. It paints
// UNCONDITIONALLY and does not touch the shadow, so a caller that uses it is
// asking for a cell to be repainted, not asking whether it needs to be. It
// draws no cursor, because a caller naming one cell is naming a cell, not a
// view state; term_redraw() owns the cursor.
void draw_row_cell(const term_cell_t *src_row, int col, int dest_row) {
    term_cell_desc_t d;
    resolve_cell(src_row, col, dest_row, 0, &d);
    paint_one(col, dest_row, &d);
}

static int desc_eq(const term_cell_desc_t *a, const term_cell_desc_t *b) {
#ifdef TERM_DAMAGE_SELFTEST_BLIND
    // DELIBERATELY WRONG, and compiled ONLY by tests/run_damage.sh --self-test.
    // It compares the codepoint and nothing else, so a cell whose COLOUR,
    // attribute or cursor changed is judged unchanged and is left stale - the
    // exact class of bug damage tracking can introduce. The self-test asserts
    // the harness reports a pixel MISMATCH under this flag and reports
    // IDENTICAL without it. A comparison harness that has never been seen to
    // fail is not evidence of anything (blame.md: "prove destructively").
    return a->ch == b->ch;
#else
    return a->ch == b->ch && a->fg == b->fg && a->bg == b->bg &&
           a->style == b->style && a->deco == b->deco &&
           a->wide == b->wide && a->cursor == b->cursor;
#endif
}

static int scroll_state_eq(const gui_scroll_t *a, const gui_scroll_t *b) {
    return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h &&
           a->content_px == b->content_px && a->step_px == b->step_px &&
           a->offset == b->offset && a->drag == b->drag && a->hover == b->hover;
}

// Redraw the terminal. Renders whatever term_scroll_view.offset currently
// points at: the live grid when pinned to the bottom (the common case), or a
// mix of scrollback + live rows while scrolled back (#206).
void term_redraw(void) {
    term_scrollback_reconfigure();
    term_scroll_sync_bottom();
    // #221: follow the scrollback ring's evictions before anything is mapped
    // to a screen row. Cheap (one int compare when nothing is selected).
    term_select_track();
    term_search_track();

    // A pane with no shadow (its allocation failed) is not a broken pane; it
    // is a pane that repaints in full every frame, which is exactly what every
    // pane did before this feature.
    int full = (term_shadow == NULL) || (term_shadow_gen != g_shadow_gen_now);

    term_stat_frames++;
    if (full) {
        term_stat_full_frames++;
        // Clear the full content area. This is the ONLY thing that paints the
        // 2px padding around the grid and the sub-row leftover strip at the
        // bottom that no row is ever drawn into, and it covers the fill colour
        // the kernel writes into the newly exposed part of a RESIZED window
        // (sys_win_resize memcpys the old content in top-left aligned and
        // leaves 0xFFF5F5F5 everywhere else).
        // term_bg_color(): the active COLOUR SCHEME's bg, or the Window
        // Theme's if "Follow system theme" is selected (tier 2,
        // docs/TERMINAL_PARITY.md).
        win_draw_rect(window_handle, term_origin_x, term_origin_y,
                      term_px_w, term_px_h, term_bg_color());
        if (term_shadow) {
            // A record no resolve_cell() can produce, so every cell mismatches
            // exactly once and then settles. ch == 0 with wide == 0xFF cannot
            // occur: a ch == 0 record is all-zero by construction.
            for (int r = 0; r < TERM_MAX_ROWS; r++)
                for (int c = 0; c < TERM_MAX_COLS; c++) {
                    term_shadow[r][c].ch = 0;
                    term_shadow[r][c].wide = 0xFF;
                }
        }
        term_shadow_gen = g_shadow_gen_now;
    }

    // The cursor only makes sense against the live screen: when pinned to the
    // bottom, row == live_row directly (top_line == sb_count), so cursor_y is
    // already the right destination row. While scrolled back there is no
    // sensible on-screen cursor position, so it is simply not drawn there,
    // same as every other terminal.
    //
    // FOUR separate pieces of state, and they are not interchangeable:
    //   cursor_visible  - the PROGRAM asked for a cursor (DECTCEM).
    //   cursor_blink_on - which half of the blink cycle we are in.
    //   g_term_cursor_blink - whether this PROFILE blinks at all.
    //   term_cursor_suppressed - the pane being drawn is not the focused one.
    // The fourth used to be folded into the first, so focusing a pane undid an
    // application's ESC[?25l. See term_grid.h.
    // "Blink off" is honoured HERE rather than by freezing the cycle, so the
    // phase counter keeps running untouched and the two cannot desync; turning
    // blinking back on resumes mid-cycle instead of needing to be restarted.
    //
    // (#damage) The cursor is part of the CELL's descriptor, which is why the
    // 500 ms blink no longer repaints the grid: a blink toggle changes exactly
    // one cell's record, and a cursor MOVE changes exactly two (the cell it
    // left and the cell it arrived at), both of which the comparison finds
    // without anyone having to remember to mark them.
    int cur_row = -1, cur_col = -1;
    unsigned cur_mark = 0;
    if (cursor_visible && !term_cursor_suppressed &&
        (cursor_blink_on || !g_term_cursor_blink) && term_at_bottom) {
        cur_row = cursor_y;
        cur_col = cursor_x;
        cur_mark = 1u + (unsigned)g_term_cursor_shape;
    }

    // ---- PASS 1: resolve every cell, find what changed, and bound it -------
    int r0 = TERM_MAX_ROWS, r1 = -1, c0 = TERM_MAX_COLS, c1 = -1;
    int painted = 0;
    int top_line = gui_scroll_first_item(&term_scroll_view);  // 0 = oldest retained line
    for (int row = 0; row < term_rows; row++) {
        int vline = top_line + row;
        const term_cell_t *src;
        if (vline < 0) continue;
        if (vline < sb_count) {
            src = sb_row(vline);
        } else {
            int live_row = vline - sb_count;
            if (live_row < 0 || live_row >= term_rows) continue;
            src = cells[live_row];
        }
        // What the pre-damage renderer painted for this row, unconditionally.
        term_stat_cells_scanned += (unsigned long)term_cols;
        for (int col = 0; col < term_cols; col++) {
            term_cell_desc_t d;
            resolve_cell(src, col, row,
                         (row == cur_row && col == cur_col) ? cur_mark : 0u, &d);
            if (term_shadow) {
                if (desc_eq(&d, &term_shadow[row][col])) continue;
                term_shadow[row][col] = d;
            } else {
                // No shadow: nothing to compose from later, so paint now,
                // while the correctly-resolved descriptor (cursor mark and
                // all) is in hand. This is the degraded path for a failed
                // 141 KB allocation and is what every pane did before damage
                // tracking existed.
                paint_one(col, row, &d);
            }
            if (row < r0) r0 = row;
            if (row > r1) r1 = row;
            if (col < c0) c0 = col;
            if (col > c1) c1 = col;
            painted++;
        }
    }
    term_stat_cells_painted += (unsigned long)painted;

    // ---- PASS 2: compose the damaged rectangle once and upload it once ----
    //
    // A RECTANGLE rather than one upload per run of changed cells, because
    // every upload costs a whole-window commit in the kernel: two small
    // uploads are more expensive than one larger one almost immediately.
    // Composing is pure userland memory work and is cheap by comparison.
    //
    // Cells INSIDE the rectangle that did not change are composed too - they
    // have to be, the upload overwrites them - and they are read back out of
    // the shadow, which by this point holds the CURRENT descriptor for every
    // cell (changed ones were just updated; unchanged ones already matched).
    if (painted > 0 && r1 >= 0 && term_shadow) {
        // A double-width character whose LEAD sits immediately left of the
        // rectangle still paints INTO the rectangle, so start one column
        // earlier and let compose_cell()'s clipping drop the half that falls
        // outside. Cheaper and less error-prone than testing each row's first
        // cell for a continuation.
        int cs = (c0 > 0) ? c0 - 1 : c0;
        int bw = (c1 - cs + 1) * TERM_CHAR_W;
        int bh = (r1 - r0 + 1) * TERM_CHAR_H;
        long need = (long)bw * (long)bh;
        uint32_t *buf = (bw > 0 && bh > 0) ? compose_buf(need) : NULL;
        if (buf) {
            for (long i = 0; i < need; i++) buf[i] = term_bg_color();
            for (int row = r0; row <= r1; row++)
                for (int col = cs; col <= c1; col++)
                    compose_cell(buf, bw, bh,
                                 (col - cs) * TERM_CHAR_W, (row - r0) * TERM_CHAR_H,
                                 &term_shadow[row][col]);
            win_draw_image(window_handle,
                           term_origin_x + cs * TERM_CHAR_W + 2,
                           term_origin_y + r0 * TERM_CHAR_H + 2,
                           bw, bh, buf);
        } else {
            // The compose buffer could not be allocated. Fall back to one
            // upload per cell rather than to a blank screen: slow, correct,
            // and it cannot happen twice in a row without the allocation
            // succeeding, because a single cell's buffer is far smaller. The
            // descriptors come out of the SHADOW, which already carries the
            // cursor mark - re-resolving here without it is exactly the bug
            // the harness caught once.
            for (int row = r0; row <= r1; row++)
                for (int col = cs; col <= c1; col++)
                    paint_one(col, row, &term_shadow[row][col]);
        }
    }

    // Shared scrollbar widget (#206; do not hand-roll one, see #96). Draws
    // nothing when the content fits (gui_scroll_needed() false), so an
    // untouched 24-line shell prompt spends no pixels on chrome.
    //
    // (#damage) Repainted when its own state moved, OR on any frame that is
    // painting anyway. Text never occupies the gutter (term_handle_resize()
    // reserves GUI_SCROLL_W out of the width before dividing into columns), so
    // a cell repaint can never damage it; the second clause is only there
    // because the frame is being presented regardless and four rects with no
    // commit of their own cost nothing next to the present. The first clause
    // is what keeps a hover or a thumb drag - which move the widget without
    // changing a single cell - from being invisible.
    {
        int sb_changed = !term_sb_painted_valid ||
                         !scroll_state_eq(&term_sb_painted, &term_scroll_view);
        if (gui_scroll_needed(&term_scroll_view)) {
            if (sb_changed || painted) {
                gui_scroll_draw_on(window_handle, &term_scroll_view, term_bg_color());
                term_sb_painted = term_scroll_view;
                term_sb_painted_valid = 1;
                if (sb_changed) painted++;
            }
        } else if (sb_changed) {
            // It stopped being needed. The gutter it used still holds its
            // pixels, so this is a real repaint, not a state note.
            win_draw_rect(window_handle,
                          term_origin_x + term_px_w - GUI_SCROLL_W, term_origin_y,
                          GUI_SCROLL_W, term_rows * TERM_CHAR_H, term_bg_color());
            term_sb_painted = term_scroll_view;
            term_sb_painted_valid = 1;
            painted++;
        }
    }

    // PHASE 1: the visual bell frame and the attention pip go on TOP of the
    // grid and the scrollbar, and before the invalidate, so one repaint
    // carries them. Draws nothing when neither is active.
    //
    // (#damage) Called UNCONDITIONALLY, before the present gate, because it
    // owns state as well as pixels (it latches whether a flash is still due,
    // and term_notify_tick() compares against that latch) and skipping it
    // would stall that state machine. It draws only while a flash or a mark is
    // live, and both of those transitions come back through
    // term_notify_tick() -> term_layout_redraw_all(), a full repaint, so
    // neither can be left stranded on screen.
    //
    // NOT counted as a paint. A pip or a flash that is merely CONTINUING
    // changes nothing, and counting it would keep an otherwise idle window
    // presenting at the pump rate for as long as a tab carries a mark.
    term_notify_paint_overlay();

    // A frame that painted nothing cannot have changed a pixel, so there is
    // nothing to present and nothing for the chrome to repair. This is the
    // half of the fix that gets the idle cost to zero: win_invalidate() is a
    // SYNCHRONOUS window_draw() plus a full content-buffer memcpy in the
    // kernel (83 us measured, against 134 ns for a bare syscall), so the
    // 500 ms cursor blink on an unchanging window used to cost two of those a
    // second forever.
    g_last_painted = painted;
    if (painted == 0) { term_stat_frames_idle++; return; }

    // (#307) Window-level chrome LAST, so an open menu popup overlays the pane
    // instead of being painted under it.
    term_render_draw_chrome();
    if (!g_suppress_present) {
        term_stat_invalidates++;
        win_invalidate(window_handle);
    }
}
