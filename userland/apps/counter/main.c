// counter - a tiny tally counter for MayteraOS (App Store demo app, v1.0.0)
// A genuinely new, self-contained app used to demonstrate installing a brand
// new package from the App Store. Native MayteraOS style (theme + gui_style).
#include "../../libc/gui.h"
#include "../../libc/syscall.h"
#include "../../libc/theme.h"
#include "../../libc/string.h"

#define APP_VERSION "1.1.0"

static int g_win = -1;
static int g_w = 360, g_h = 300;
static long g_count = 0;

static uint32_t C_bg, C_ink, C_dim, C_accent, C_card, C_border;

static void palette(void) {
    C_bg     = theme_color(THEME_COLOR_WINDOW_BG);
    C_ink    = theme_color(THEME_COLOR_LABEL_TEXT);
    C_accent = theme_color(THEME_COLOR_ACCENT);
    C_border = theme_color(THEME_COLOR_WINDOW_BORDER);
    if (C_bg == 0 && C_ink == 0) { C_bg = 0x1E1E1E; C_ink = 0xFFFFFF; }
    if (C_accent == 0) C_accent = 0x2D7DF6;
    int lum = ((C_bg>>16&0xFF)*30 + (C_bg>>8&0xFF)*59 + (C_bg&0xFF)*11)/100;
    C_card = lum < 128 ? gui_lighten(C_bg, 16) : 0xFFFFFF;
    C_dim  = gui_mix(C_ink, C_bg, 110);
    if (C_border == 0) C_border = gui_mix(C_ink, C_bg, 180);
    gui_set_style(GUI_STYLE_MODERN);
    gui_palette_t p = { C_bg, C_card, C_ink, C_dim, C_accent, gui_lighten(C_accent,18),
                        C_border, C_card, C_border, gui_mix(C_ink,C_bg,190) };
    gui_set_palette(&p);
}

// button rects
static int bminus_x, bplus_x, breset_x, bten_x, btn_y, btn_w = 78, btn_h = 44;

static void draw(void) {
    gui_fill_rect(g_win, 0, 0, g_w, g_h, C_bg);
    // title
    win_draw_text_ttf(g_win, 20, 18, "Counter", 20, C_ink);
    win_draw_text_ttf(g_win, 20, 44, "Tally counter - now with steps", 12, C_dim);

    // big number card
    int cx = 20, cy = 74, cw = g_w - 40, ch = 100;
    gui_fill_rounded_aa(g_win, cx, cy, cw, ch, 14, C_card, C_bg);
    gui_rounded_border(g_win, cx, cy, cw, ch, 14, C_border);
    char num[24]; gui_itoa(g_count, num, sizeof(num));
    int nw = gui_ttf_width(num, 52);
    win_draw_text_ttf(g_win, cx + (cw - nw)/2, cy + (ch - 52)/2, num, 52, C_accent);

    // buttons
    btn_y = 196;
    int total = btn_w*4 + 12*3;
    int sx = (g_w - total)/2;
    bminus_x = sx; breset_x = sx + (btn_w+12); bplus_x = sx + (btn_w+12)*2; bten_x = sx + (btn_w+12)*3;
    gui_button(g_win, bminus_x, btn_y, btn_w, btn_h, "-", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    gui_button(g_win, breset_x, btn_y, btn_w, btn_h, "Reset", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    gui_button(g_win, bplus_x, btn_y, btn_w, btn_h, "+", GUI_BTN_PRIMARY, GUI_ST_NORMAL);
    gui_button(g_win, bten_x, btn_y, btn_w, btn_h, "+10", GUI_BTN_PRIMARY, GUI_ST_NORMAL);

    win_draw_text_ttf(g_win, 20, g_h - 24, "v" APP_VERSION, 11, C_dim);
    win_invalidate(g_win);
}

static int hit(int mx, int my, int x) {
    return mx >= x && mx < x + btn_w && my >= btn_y && my < btn_y + btn_h;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    g_win = win_create("Counter", 120, 100, g_w, g_h);
    if (g_win < 0) return 1;
    palette();
    draw();
    gui_event_t ev; int run = 1;
    while (run) {
        int et = win_get_event(g_win, &ev, 200);
        if (et == 0) continue;
        switch (ev.type) {
            case EVENT_RESIZE: if (ev.mouse_x>200&&ev.mouse_y>200){g_w=ev.mouse_x;g_h=ev.mouse_y;} draw(); break;
            case EVENT_REDRAW: draw(); break;
            case EVENT_WINDOW_CLOSE: run = 0; break;
            case EVENT_MOUSE_DOWN:
                if (hit(ev.mouse_x, ev.mouse_y, bplus_x)) g_count++;
                else if (hit(ev.mouse_x, ev.mouse_y, bminus_x)) g_count--;
                else if (hit(ev.mouse_x, ev.mouse_y, breset_x)) g_count = 0;
                else if (hit(ev.mouse_x, ev.mouse_y, bten_x)) g_count += 10;
                draw();
                break;
            default: break;
        }
    }
    win_destroy(g_win);
    return 0;
}
