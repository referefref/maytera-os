// FSSCALE v3 - #fsscale: animated diagnostic. Blits a 1280x800 buffer with a
// moving bar (position changes every frame) at max rate, no sleep between
// blits, mimicking Arena's own frame-rate/geometry. Used to tell "stretched
// but stale" apart from "not stretched": a STATIC checkerboard cannot show a
// stale frame, a MOVING bar can, because if the compositor freezes at the
// pre-resize commit, the bar visibly stops moving even though the process
// keeps calling sys_win_blit().
#include <stdio.h>
#include "syscall.h"
#include "gui.h"

#define SRC_W 1280
#define SRC_H 800
static uint32_t g_src[SRC_W * SRC_H];
static int g_frame = 0;

static void draw_frame(int frame) {
    // background: dark
    for (int i = 0; i < SRC_W * SRC_H; i++) g_src[i] = 0xFF101018u;
    // moving vertical bar, position cycles across the width
    int barw = 40;
    int bx = frame % (SRC_W - barw);
    uint32_t col = ((frame / 40) & 1) ? 0xFFFF3030u : 0xFF30FF30u; // alternates colour every 40 frames too
    for (int y = 0; y < SRC_H; y++) {
        for (int x = bx; x < bx + barw; x++) g_src[y * SRC_W + x] = col;
    }
    // frame counter as a growing horizontal bar at the top (0..SRC_W over 1000 frames)
    int fw = (frame * SRC_W / 1000) % SRC_W;
    for (int y = 0; y < 20; y++)
        for (int x = 0; x < fw; x++) g_src[y * SRC_W + x] = 0xFFFFFF00u;
}

int main(void) {
    fb_info_t fi; int scr_w = 1280, scr_h = 800;
    if (fb_info(&fi) == 0 && fi.width > 0 && fi.height > 0) {
        scr_w = (int)fi.width; scr_h = (int)fi.height;
    }
    int win = win_create("FSSCALE anim", 60, 60, 640, 480);
    if (win < 0) { printf("win_create failed\n"); return 1; }
    char line[200];
    snprintf(line, sizeof(line), "[FSSCALEANIM] created win=%d scr_w=%d scr_h=%d", win, scr_w, scr_h);
    sys_bootlog(line);

    for (;;) {
        gui_event_t ev;
        int t = win_get_event(win, &ev, 0);
        if (t > 0 && ev.type == EVENT_WINDOW_CLOSE) break;
        draw_frame(g_frame++);
        syscall5(SYS_WIN_BLIT, win, 0, 0, (SRC_W & 0xFFFF) | ((SRC_H & 0xFFFF) << 16), (long)g_src);
        win_invalidate(win);
        if ((g_frame % 200) == 0) {
            snprintf(line, sizeof(line), "[FSSCALEANIM] frame=%d", g_frame);
            sys_bootlog(line);
        }
    }
    win_destroy(win);
    return 0;
}
