// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// term_damage_test.c - MEASURE the terminal's repaint cost on a real TUI byte
// stream, on the build host, with no VM involved.
//
// WHY A HOST HARNESS AND NOT A STOPWATCH IN A VM. The fault being fixed was
// reported from real hardware and does not reproduce in a VM: whatever makes a
// full-grid repaint ruinous on a laptop is nearly free under QEMU. A wall-clock
// number measured in a VM would therefore say "fine" and mean nothing. The
// quantity that is the same on every machine is the AMOUNT OF WORK: how many
// cells the renderer paints per update, and how many draw syscalls that is.
//
// It measures that by RUNNING THE PRODUCTION RENDERER. term_render.c,
// term_grid.c, term_parse.c, term_scrollback.c and term_emu.c are compiled
// here exactly as the app links them, with syscall0..6 (extern in
// libc/syscall.h, defined in syscall.asm) replaced by counting stubs. The
// numbers are therefore not a model of the renderer; they are the renderer.
//
//     tests/run_damage.sh          builds and runs it over tests/traces/*
//
// See term_damage_hostio.c for why the file I/O lives in a second file.

#include "../term_common.h"
#include "../term_grid.h"
#include "../term_emu.h"
#include "../term_render.h"
#include "../term_parse.h"
#include "../term_scrollback.h"
#include "../term_theme.h"
#include "term_damage_hostio.h"

// ---------------------------------------------------------------------------
// THE SYSCALL FLOOR. Redefining syscall0..6 neutralises EVERY MayteraOS
// syscall in ONE place: nothing this harness links can reach the host kernel
// with a MayteraOS syscall number, and every window draw the renderer issues is
// counted instead of performed.
static long g_sc_rect, g_sc_text, g_sc_image, g_sc_inval;

// ---------------------------------------------------------------------------
// AN EMULATED WINDOW, so the two arms can be compared as PIXELS.
//
// This is the point of the harness that matters most. Counting cells proves
// the renderer does less work; it does not prove it draws the RIGHT thing, and
// a damage-tracking bug looks exactly like a stale cell nobody notices for a
// week. So every window draw the renderer issues is applied to a plain array
// here, and at the end the full-repaint arm's array and the damage-tracked
// arm's array are compared byte for byte. Equal means: over a real 30-second
// TUI stream, painting only the changed cells produced the SAME WINDOW as
// painting all of them, pixel for pixel.
//
// Text cannot be rasterised here (that needs the kernel's font), so a text
// draw stamps the glyph box with a value derived from the string, the style
// and the colour. That is exactly the right fidelity for an EQUIVALENCE test:
// two runs agree iff they issued the same text, in the same place, in the same
// colour and style. It would be the wrong fidelity for judging appearance, and
// it is not used for that.
#define FBW 1280
#define FBH 800
static unsigned int g_fb[FBW * FBH];
static void fb_reset(void) { for (long i = 0; i < (long)FBW * FBH; i++) g_fb[i] = 0; }
static void fb_fill(int x, int y, int w, int h, unsigned int c) {
    for (int r = 0; r < h; r++) {
        int py = y + r; if (py < 0 || py >= FBH) continue;
        for (int q = 0; q < w; q++) {
            int px = x + q; if (px < 0 || px >= FBW) continue;
            g_fb[(long)py * FBW + px] = c;
        }
    }
}
static unsigned long fb_checksum(void) {
    unsigned long h = 1469598103934665603ul;
    for (long i = 0; i < (long)FBW * FBH; i++) { h ^= g_fb[i]; h *= 1099511628211ul; }
    return h;
}
static unsigned int fnv(const char *s, unsigned int seed) {
    unsigned int hsh = seed ? seed : 2166136261u;
    while (s && *s) { hsh ^= (unsigned char)*s++; hsh *= 16777619u; }
    return hsh | 1u;   // never 0, so "text was drawn" is distinguishable
}

static long sc(long num) {
    switch (num) {
        case SYS_WIN_DRAW_RECT:    g_sc_rect++;  break;
        case SYS_WIN_DRAW_TTF_EX:  g_sc_text++;  break;
        case SYS_WIN_DRAW_TTF:     g_sc_text++;  break;
        case SYS_WIN_DRAW_TEXT:    g_sc_text++;  break;
        case SYS_WIN_DRAW_IMAGE:   g_sc_image++; break;
        case SYS_WIN_INVALIDATE:   g_sc_inval++; break;
        default: break;
    }
    return 0;
}
long syscall0(long n) { return sc(n); }
long syscall1(long n, long a) { (void)a; return sc(n); }
long syscall2(long n, long a, long b) { (void)a;(void)b; return sc(n); }
long syscall3(long n, long a, long b, long c) { (void)a;(void)b;(void)c; return sc(n); }
long syscall4(long n, long a, long b, long c, long d) { (void)a;(void)b;(void)c;(void)d; return sc(n); }

// A SYNTHETIC FONT.
//
// The renderer now rasterises and composites every glyph itself through
// SYS_FONT_GLYPH, so a harness that returns "no glyph" would compare two blank
// screens and prove nothing. This hands back a deterministic 8-bit alpha
// bitmap per codepoint instead. Two properties are deliberate:
//
//   - It is a pure function of the codepoint, style and size, so the two arms
//     see identical glyphs and any pixel difference is the renderer's.
//   - EVERY SEVENTH CODEPOINT OVERHANGS ITS CELL, vertically and horizontally.
//     That is the whole bug this file exists to catch: the kernel's text path
//     clips glyph ink only to the WINDOW, so ink that leaves a cell is never
//     erased by the neighbour's background fill, and under damage tracking it
//     stays on screen forever. compose_cell() clips to the cell; these
//     oversized glyphs are what exercises that clip on every trace.
static int synth_glyph(int cp, int size, int style, int *meta, unsigned char *bmp, int cap) {
    int over = (cp % 7) == 0;
    int w = 5 + (cp % 3), h = term_char_h - 3 + (over ? 5 : 0);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int xoff = over ? -1 : 1;
    int yoff = -(term_ascent) + (over ? -2 : 1);
    if (meta) { meta[0] = w; meta[1] = h; meta[2] = xoff; meta[3] = yoff; meta[4] = term_char_w; }
    if (bmp && cap >= w * h) {
        unsigned int hsh = (unsigned int)(cp * 2654435761u) ^ (unsigned int)(size * 40503u)
                         ^ (unsigned int)(style * 97u);
        for (int i = 0; i < w * h; i++) {
            hsh ^= hsh << 13; hsh ^= hsh >> 17; hsh ^= hsh << 5;
            bmp[i] = (unsigned char)(hsh & 0xFF);
        }
    }
    return term_char_w;
}

long syscall5(long n, long a, long b, long c, long d, long e) {
    if (n == SYS_FONT_GLYPH) {
        int size = (int)((a >> 8) & 0xFFFF), style = (int)((a >> 24) & 0xFF);
        return synth_glyph((int)b, size, style, (int *)c, (unsigned char *)d, (int)e);
    }
    if (n == SYS_WIN_DRAW_TTF_EX) {
        // win_draw_text_ttf_ex packs (x,y) into b and (face,size,style) into d.
        int x = (int)(short)(b & 0xFFFF), y = (int)(short)((b >> 16) & 0xFFFF);
        int size = (int)((d >> 8) & 0xFFFF), style = (int)((d >> 24) & 0xFF);
        unsigned int stamp = fnv((const char *)c,
                                 (unsigned int)(e * 2654435761u) ^
                                 (unsigned int)(size * 40503u) ^ (unsigned int)style);
        fb_fill(x, y, term_char_w, term_char_h, stamp);
    }
    (void)a;
    return sc(n);
}
long syscall6(long n, long a, long b, long c, long d, long e, long f) {
    if (n == SYS_WIN_DRAW_RECT)
        fb_fill((int)b, (int)c, (int)d, (int)e, (unsigned int)f);
    else if (n == SYS_WIN_DRAW_IMAGE) {
        const unsigned int *px = (const unsigned int *)f;
        int x = (int)b, y = (int)c, w = (int)d, hgt = (int)e;
        for (int r = 0; r < hgt; r++) {
            int py = y + r; if (py < 0 || py >= FBH) continue;
            for (int q = 0; q < w; q++) {
                int pxx = x + q; if (pxx < 0 || pxx >= FBW) continue;
                g_fb[(long)py * FBW + pxx] = px[(long)r * w + q];
            }
        }
    }
    (void)a;
    return sc(n);
}

// ---------------------------------------------------------------------------
// The modules the renderer and the parser reference but that this harness does
// not compile. Each is that module's DEFAULT, no-feature behaviour: no
// selection, no find match, no notification, no layout, no pty.
uint32_t ansi_colors[16] = {
    0x000000,0xCC0000,0x00CC00,0xCCCC00,0x0000CC,0xCC00CC,0x00CCCC,0xCCCCCC,
    0x555555,0xFF5555,0x55FF55,0xFFFF55,0x5555FF,0xFF55FF,0x55FFFF,0xFFFFFF };
gui_font_sel_t g_term_font;
int g_term_cursor_shape = TERM_CURSOR_BLOCK;
int g_term_cursor_blink = 1;
int g_term_theme_index = 0;
uint32_t term_fg_color(void)     { return 0xCCCCCC; }
uint32_t term_bg_color(void)     { return 0x101010; }
uint32_t term_cursor_color(void) { return 0xCCCCCC; }

int  term_select_cell_colors(int r, int c, uint32_t *fg, uint32_t *bg) { (void)r;(void)c;(void)fg;(void)bg; return 0; }
void term_select_track(void) {}
int  term_search_cell_colors(int r, int c, uint32_t *fg, uint32_t *bg) { (void)r;(void)c;(void)fg;(void)bg; return 0; }
void term_search_track(void) {}
void term_notify_paint_overlay(void) {}
void term_notify_bell(int tab) { (void)tab; }
int  term_layout_active_tab(void) { return 0; }
// term_pty.c's, for the DSR reply path. -1 means "no foreground child owns
// this terminal", so a reply is dropped, which is right: this harness replays
// a capture and has nowhere to answer.
int  g_active_master_fd = -1;

// libc/gui.c's colour helpers, pulled in by the REAL gui_scroll.c (which this
// harness links rather than stubs, because gui_scroll_first_item() is what maps
// a screen row to a virtual line and getting that wrong is #220). Stubbed
// rather than linking gui.c: they decide the scrollbar's SHADE, and a shade
// cannot change a cell count.
int      gui_contrast_x100(uint32_t a, uint32_t b) { (void)a;(void)b; return 700; }
uint32_t gui_ensure_contrast(uint32_t fg, uint32_t bg, int m) { (void)bg;(void)m; return fg; }
uint32_t gui_ensure_contrast2(uint32_t fg, uint32_t b1, uint32_t b2, int m) { (void)b1;(void)b2;(void)m; return fg; }
void     gui_bevel_pair(uint32_t base, uint32_t *sh, uint32_t *hi) { if (sh) *sh = base; if (hi) *hi = base; }
ui_style_t gui_active_style(void) { ui_style_t s; for (unsigned i = 0; i < sizeof s; i++) ((unsigned char *)&s)[i] = 0; return s; }

// The shadow the app allocates per pane (term_layout.c) or adopts as a static
// (term_render.c). Owned here so the baseline arm can take it away again.
static term_cell_desc_t g_harness_shadow[TERM_MAX_ROWS][TERM_MAX_COLS];
static unsigned int g_fb_prev[FBW * FBH];

// ---------------------------------------------------------------------------
void damage_run(const char *trace, long tracelen,
                const long *frames, int nframes,
                int cols, int rows, int no_damage, dmg_result_t *out) {
    // The metrics the app derives from the selected font. Fixed here so the
    // measurement is reproducible and independent of which fonts a build host
    // happens to have.
    term_char_w = 8;
    term_char_h = 16;
    term_ascent = 12;
    window_handle = 1;

    // Reset every module global this harness can be asked to run twice over.
    term_shadow = g_harness_shadow;
    term_shadow_gen = 0;
    term_sb_painted_valid = 0;
    sb_count = 0; sb_head = 0;
    term_emu_reset(&g_parser);
    term_emu_sgr_reset(&g_pen);
    term_clear_calls = 0;
    in_alt_screen = 0;
    fb_reset();
    term_scrollback_alloc();
    // + GUI_SCROLL_W because term_handle_resize() reserves the scrollbar
    // gutter out of the width it is given; asking for `cols` columns means
    // handing it the gutter as well.
    term_handle_resize(cols * term_char_w + GUI_SCROLL_W, rows * term_char_h);
    term_clear();

    // The first frame is a full repaint by construction (see term_render.h),
    // and it is not part of what is being measured: nobody claims damage
    // tracking helps the FIRST paint. Reset after it.
    term_redraw();
    term_stat_reset();
    g_sc_rect = g_sc_text = g_sc_image = g_sc_inval = 0;
    // AFTER the first full paint, so the baseline arm still gets its content
    // area cleared once, exactly as the app does.
    if (no_damage) term_shadow = 0;

    static long s_per_frame[65536];
    static unsigned long s_frame_sum[65536];
    long pos = 0, ran = 0;
    for (int i = 0; i < nframes && pos < tracelen; i++) {
        long take = frames[i];
        if (take <= 0) continue;
        if (pos + take > tracelen) take = tracelen - pos;
        for (long k = 0; k < take; k++) term_putc((unsigned char)trace[pos + k]);
        pos += take;
        {
            long before = (long)term_stat_cells_painted;
            term_redraw();      // one repaint per event-loop tick, as the app does
            if (ran < (long)(sizeof s_per_frame / sizeof s_per_frame[0])) {
                s_per_frame[ran] = (long)term_stat_cells_painted - before;
                s_frame_sum[ran] = fb_checksum();
            }
        }
        ran++;
    }
    // Whatever the plan did not cover still has to be consumed, or a truncated
    // plan would quietly measure only part of the stream and report a flattering
    // number for the rest.
    if (pos < tracelen) {
        for (long k = pos; k < tracelen; k++) term_putc((unsigned char)trace[k]);
        term_redraw();
        ran++;
    }

    out->grid_cols = term_cols;
    out->grid_rows = term_rows;
    out->bytes = tracelen;
    out->frames = ran;
    out->frames_idle = (long)term_stat_frames_idle;
    out->cells_scanned = (long)term_stat_cells_scanned;
    out->cells_painted = (long)term_stat_cells_painted;
    out->sc_rect = g_sc_rect;
    out->sc_text = g_sc_text;
    out->sc_image = g_sc_image;
    out->sc_invalidate = g_sc_inval;
    out->per_frame = s_per_frame;
    out->per_frame_n = ran;

    out->fb_sum = fb_checksum();
    out->fb_frame_sum = s_frame_sum;
    out->fb_frame_n = (ran < 65536) ? ran : 65536;
    // Compare against whatever the previous run left, so a caller running the
    // baseline arm and then the damage arm gets an exact per-pixel difference
    // count rather than only "the checksums differ".
    out->fb_diff_vs_prev = 0;
    for (long i = 0; i < (long)FBW * FBH; i++)
        if (g_fb[i] != g_fb_prev[i]) out->fb_diff_vs_prev++;
    for (long i = 0; i < (long)FBW * FBH; i++) g_fb_prev[i] = g_fb[i];
}
