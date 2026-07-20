#include "../../libc/maytera.h"
#include "../../libc/gui.h"

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    int W = 400, H = 480;
    int win = win_create("Tip Calculator", 220, 120, W, H);
    if (win < 0) return 1;

    int bill_cents = 5000;
    int tip_percent = 18;

    int btn_w = 60, btn_h = 40;
    int bill_btn_y = 100;
    int bill_x[4] = {30, 120, 210, 300};

    int tip_btn_y = 220;
    int tip_x[5] = {30, 100, 170, 240, 310};

    for (;;) {
        win_draw_rect(win, 0, 0, W, H, 0x00F5F5F0);

        win_draw_text_ttf(win, 30, 20, "Tip Calculator", 28, 0x00202830);

        char buf[64];
        snprintf(buf, sizeof(buf), "Bill: $%d.%02d", bill_cents / 100, bill_cents % 100);
        win_draw_text_ttf(win, 30, 60, buf, 22, 0x00202830);

        gui_draw_button(win, bill_x[0], bill_btn_y, btn_w, btn_h, "-$1", 0x00D0D0D0, 0x00202830, 0, 0);
        gui_draw_button(win, bill_x[1], bill_btn_y, btn_w, btn_h, "+$1", 0x00D0D0D0, 0x00202830, 0, 0);
        gui_draw_button(win, bill_x[2], bill_btn_y, btn_w, btn_h, "-10c", 0x00D0D0D0, 0x00202830, 0, 0);
        gui_draw_button(win, bill_x[3], bill_btn_y, btn_w, btn_h, "+10c", 0x00D0D0D0, 0x00202830, 0, 0);

        snprintf(buf, sizeof(buf), "Tip: %d%%", tip_percent);
        win_draw_text_ttf(win, 30, 170, buf, 22, 0x00202830);

        gui_draw_button(win, tip_x[0], tip_btn_y, btn_w, btn_h, "10%", tip_percent == 10 ? 0x00FF8C00 : 0x00D0D0D0, 0x00202830, tip_percent == 10, 0);
        gui_draw_button(win, tip_x[1], tip_btn_y, btn_w, btn_h, "15%", tip_percent == 15 ? 0x00FF8C00 : 0x00D0D0D0, 0x00202830, tip_percent == 15, 0);
        gui_draw_button(win, tip_x[2], tip_btn_y, btn_w, btn_h, "18%", tip_percent == 18 ? 0x00FF8C00 : 0x00D0D0D0, 0x00202830, tip_percent == 18, 0);
        gui_draw_button(win, tip_x[3], tip_btn_y, btn_w, btn_h, "20%", tip_percent == 20 ? 0x00FF8C00 : 0x00D0D0D0, 0x00202830, tip_percent == 20, 0);
        gui_draw_button(win, tip_x[4], tip_btn_y, btn_w, btn_h, "25%", tip_percent == 25 ? 0x00FF8C00 : 0x00D0D0D0, 0x00202830, tip_percent == 25, 0);

        int tip_cents = (bill_cents * tip_percent) / 100;
        int total_cents = bill_cents + tip_cents;

        snprintf(buf, sizeof(buf), "Tip Amount: $%d.%02d", tip_cents / 100, tip_cents % 100);
        win_draw_text_ttf(win, 30, 300, buf, 24, 0x00FF8C00);
        snprintf(buf, sizeof(buf), "Total: $%d.%02d", total_cents / 100, total_cents % 100);
        win_draw_text_ttf(win, 30, 350, buf, 28, 0x00308030);

        win_invalidate(win);

        gui_event_t ev;
        int t = win_get_event(win, &ev, 500);
        if (t == 0) continue;
        if (ev.type == EVENT_WINDOW_CLOSE) break;
        if (ev.type == EVENT_MOUSE_DOWN) {
            if (gui_point_in_rect(ev.mouse_x, ev.mouse_y, bill_x[0], bill_btn_y, btn_w, btn_h)) {
                if (bill_cents >= 100) bill_cents -= 100;
            }
            if (gui_point_in_rect(ev.mouse_x, ev.mouse_y, bill_x[1], bill_btn_y, btn_w, btn_h)) {
                bill_cents += 100;
            }
            if (gui_point_in_rect(ev.mouse_x, ev.mouse_y, bill_x[2], bill_btn_y, btn_w, btn_h)) {
                if (bill_cents >= 10) bill_cents -= 10;
            }
            if (gui_point_in_rect(ev.mouse_x, ev.mouse_y, bill_x[3], bill_btn_y, btn_w, btn_h)) {
                bill_cents += 10;
            }

            int tvals[5] = {10, 15, 18, 20, 25};
            for (int i = 0; i < 5; i++) {
                if (gui_point_in_rect(ev.mouse_x, ev.mouse_y, tip_x[i], tip_btn_y, btn_w, btn_h)) {
                    tip_percent = tvals[i];
                }
            }
        }
    }

    win_destroy(win);
    return 0;
}