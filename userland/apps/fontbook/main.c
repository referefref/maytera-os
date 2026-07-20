// Font Book - MayteraOS userland app.
//
// An OS-wide font browser: lists every installed TrueType face from the kernel
// font registry (font_count / font_name) and previews the selected face at a
// range of sizes and styles (Regular / Bold / Italic / Bold+Italic) using the
// face-aware antialiased text path (win_draw_text_ttf_ex). Window chrome and
// labels use the default UI face (win_draw_text_ttf).
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_style.h"
#include "../../libc/syscall.h"

// ---- theme (dark, clean) ----
#define COL_BG        0x001E1E24u   // window background
#define COL_PANEL     0x0026262Eu   // sidebar / cards
#define COL_TEXT      0x00E8E8ECu   // primary text
#define COL_TEXT_DIM  0x0092929Cu   // secondary text / labels
#define COL_ACCENT    0x004A78C8u   // selection + accents
#define COL_SEP       0x0038384Au   // separators

#define MAX_FONTS 32

#define WIN_W 860
#define WIN_H 620
#define SIDEBAR_W 240
#define ROW_H 30

static int win = -1;
static int g_w = WIN_W, g_h = WIN_H;

static char g_names[MAX_FONTS][64];
static char g_styles[MAX_FONTS][32];
static int  g_face_of[MAX_FONTS];      // row -> face index (indices have holes)
static int  g_nfonts = 0;
static int  g_sel = 0;
static int  g_scroll = 0;   // sidebar scroll offset in rows

// The sample text shown across sizes and styles.
static const char *PANGRAM  = "The quick brown fox jumps over the lazy dog";
static const char *ALPHABET = "ABCDEFGHIJKLM abcdefghijklm 0123456789";

static void load_fonts(void) {
    int n = font_count();
    if (n < 0) n = 0;
    if (n > MAX_FONTS) n = MAX_FONTS;
    g_nfonts = 0;
    for (int i = 0; i < n; i++) {
        char *buf = g_names[g_nfonts];
        int len = font_name(i, buf, 63);
        // A zero-length name is an empty or UNINSTALLED slot, not a nameless
        // font: face indices are stable and may contain holes, so skip it
        // rather than inventing a "Face N" label for a font that is not there.
        if (len <= 0) continue;
        if (len > 63) len = 63;
        buf[len] = 0;

        int sl = font_style(i, g_styles[g_nfonts], 31);
        if (sl <= 0) { g_styles[g_nfonts][0] = 0; }
        else g_styles[g_nfonts][sl > 31 ? 31 : sl] = 0;

        g_face_of[g_nfonts] = i;
        g_nfonts++;
    }
}

// How many sidebar rows fit below the header.
static int visible_rows(void) {
    int avail = g_h - 56 - 8;
    int rows = avail / ROW_H;
    if (rows < 1) rows = 1;
    return rows;
}

static void clamp_state(void) {
    if (g_nfonts <= 0) { g_sel = 0; g_scroll = 0; return; }
    if (g_sel < 0) g_sel = 0;
    if (g_sel >= g_nfonts) g_sel = g_nfonts - 1;
    int vr = visible_rows();
    int maxscroll = g_nfonts - vr;
    if (maxscroll < 0) maxscroll = 0;
    if (g_scroll > maxscroll) g_scroll = maxscroll;
    if (g_scroll < 0) g_scroll = 0;
    // Keep the selection visible.
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + vr) g_scroll = g_sel - vr + 1;
    if (g_scroll < 0) g_scroll = 0;
}

// ---- drawing ----
static void draw_sidebar(void) {
    win_draw_rect(win, 0, 0, SIDEBAR_W, g_h, COL_PANEL);
    win_draw_rect(win, SIDEBAR_W, 0, 1, g_h, COL_SEP);

    // Header: "N fonts installed".
    char hdr[48];
    char nb[16]; int ni = 0; int v = g_nfonts;
    if (v == 0) { nb[ni++] = '0'; }
    else { char tmp[16]; int t = 0; while (v > 0) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
           while (t > 0) nb[ni++] = tmp[--t]; }
    nb[ni] = 0;
    int hi = 0;
    for (int i = 0; nb[i]; i++) hdr[hi++] = nb[i];
    const char *suffix = (g_nfonts == 1) ? " font installed" : " fonts installed";
    for (int i = 0; suffix[i]; i++) hdr[hi++] = suffix[i];
    hdr[hi] = 0;
    win_draw_text_ttf(win, 16, 18, hdr, 15, COL_TEXT);
    win_draw_rect(win, 12, 48, SIDEBAR_W - 24, 1, COL_SEP);

    if (g_nfonts <= 0) {
        win_draw_text_ttf(win, 16, 70, "No fonts installed", 14, COL_TEXT_DIM);
        return;
    }

    int vr = visible_rows();
    int y0 = 56;
    for (int r = 0; r < vr; r++) {
        int idx = g_scroll + r;
        if (idx >= g_nfonts) break;
        int ry = y0 + r * ROW_H;
        int sel = (idx == g_sel);
        if (sel) win_draw_rect(win, 6, ry, SIDEBAR_W - 12, ROW_H - 2, COL_ACCENT);
        // Render each entry in ITS OWN face so the list doubles as a preview.
        int face = g_face_of[idx];
        win_draw_text_ttf_ex(win, 16, ry + 6, g_names[idx], face, 15,
                             FONT_STYLE_NORMAL, sel ? 0x00FFFFFFu : COL_TEXT);
        if (g_styles[idx][0]) {
            int nw = gui_ttf_width(g_names[idx], 15);
            win_draw_text_ttf(win, 16 + nw + 8, ry + 8, g_styles[idx], 11,
                              sel ? 0x00D8E4F8u : COL_TEXT_DIM);
        }
    }

    // Scroll hint if the list overflows.
    if (g_nfonts > vr) {
        win_draw_text_ttf(win, 16, g_h - 22, "scroll for more", 11, COL_TEXT_DIM);
    }
}

static void draw_preview(void) {
    int x0 = SIDEBAR_W + 24;
    int cw = g_w - x0 - 20;
    win_draw_rect(win, SIDEBAR_W + 1, 0, g_w - SIDEBAR_W - 1, g_h, COL_BG);
    if (cw < 40) return;

    if (g_nfonts <= 0) {
        win_draw_text_ttf(win, x0, 40, "No fonts installed", 20, COL_TEXT_DIM);
        return;
    }

    int face = g_face_of[g_sel];
    const char *name = g_names[g_sel];

    // Family name, large, in its own face; subfamily beside it.
    win_draw_text_ttf_ex(win, x0, 24, name, face, 34, FONT_STYLE_NORMAL, COL_TEXT);
    if (g_styles[g_sel][0]) {
        int nw = gui_ttf_width(name, 34);
        win_draw_text_ttf(win, x0 + nw + 12, 40, g_styles[g_sel], 14, COL_TEXT_DIM);
    }
    win_draw_rect(win, x0, 70, cw, 1, COL_SEP);

    // Pangram + alphabet across several point sizes, each labeled with its size.
    int y = 88;
    static const int sizes[] = { 12, 16, 20, 28, 40 };
    for (int i = 0; i < 5; i++) {
        int sz = sizes[i];
        char lbl[8]; int li = 0;
        if (sz >= 10) lbl[li++] = (char)('0' + sz / 10);
        lbl[li++] = (char)('0' + sz % 10);
        lbl[li++] = 'p'; lbl[li++] = 'x'; lbl[li] = 0;
        // Small chrome label in the default UI face at the left margin.
        win_draw_text_ttf(win, x0, y + (sz - 12) / 2, lbl, 11, COL_TEXT_DIM);
        const char *sample = (i % 2 == 0) ? PANGRAM : ALPHABET;
        win_draw_text_ttf_ex(win, x0 + 40, y, sample, face, sz,
                             FONT_STYLE_NORMAL, COL_TEXT);
        y += sz + 12;
    }

    y += 8;
    win_draw_rect(win, x0, y, cw, 1, COL_SEP);
    y += 12;
    win_draw_text_ttf(win, x0, y, "Styles", 13, COL_TEXT_DIM);
    y += 24;

    static const char *style_lbl[4] = { "Regular", "Bold", "Italic", "Bold Italic" };
    static const int style_bits[4] = {
        FONT_STYLE_NORMAL,
        FONT_STYLE_BOLD,
        FONT_STYLE_ITALIC,
        FONT_STYLE_BOLD | FONT_STYLE_ITALIC
    };
    for (int i = 0; i < 4; i++) {
        win_draw_text_ttf(win, x0, y + 3, style_lbl[i], 11, COL_TEXT_DIM);
        win_draw_text_ttf_ex(win, x0 + 90, y, PANGRAM, face, 22,
                             style_bits[i], COL_TEXT);
        y += 34;
    }
}

static void draw_all(void) {
    clamp_state();
    win_draw_rect(win, 0, 0, g_w, g_h, COL_BG);
    draw_preview();
    draw_sidebar();
    win_invalidate(win);
}

// Which sidebar row (font index) is at window-relative (mx,my), or -1.
static int sidebar_hit(int mx, int my) {
    if (mx < 0 || mx >= SIDEBAR_W) return -1;
    int y0 = 56;
    if (my < y0) return -1;
    int r = (my - y0) / ROW_H;
    int idx = g_scroll + r;
    if (idx < 0 || idx >= g_nfonts) return -1;
    return idx;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    win = win_create("Font Book", 80, 60, WIN_W, WIN_H);
    if (win < 0) { printf("Font Book: cannot create window\n"); return 1; }

    load_fonts();

    int cw = 0, ch = 0;
    if (win_get_size(win, &cw, &ch) == 0 && cw > 0 && ch > 0) { g_w = cw; g_h = ch; }

    draw_all();

    gui_event_t ev;
    int running = 1;
    while (running) {
        int et = win_get_event(win, &ev, -1);
        if (et == 0) continue;
        switch (ev.type) {
            case EVENT_REDRAW:
                draw_all();
                break;
            case EVENT_RESIZE:
                if (ev.mouse_x > 0 && ev.mouse_y > 0) { g_w = ev.mouse_x; g_h = ev.mouse_y; }
                else win_get_size(win, &g_w, &g_h);
                draw_all();
                break;
            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;
            case EVENT_KEY_DOWN:
                if (ev.key_char == 27) { running = 0; break; }   // ESC
                // Arrow-key navigation (keycode) as a keyboard fallback.
                if (ev.keycode == 0x50 || ev.key_char == 'j') { g_sel++; draw_all(); }       // down
                else if (ev.keycode == 0x48 || ev.key_char == 'k') { g_sel--; draw_all(); }  // up
                break;
            case EVENT_MOUSE_SCROLL:
                g_scroll -= ev.scroll_delta;   // wheel up = scroll up
                draw_all();
                break;
            case EVENT_MOUSE_DOWN:
                if (ev.mouse_buttons & MOUSE_BUTTON_LEFT) {
                    int idx = sidebar_hit(ev.mouse_x, ev.mouse_y);
                    if (idx >= 0 && idx != g_sel) { g_sel = idx; draw_all(); }
                }
                break;
            default:
                break;
        }
    }

    win_destroy(win);
    return 0;
}
