// glcube - spinning textured 3D cube (TinyGL) for MayteraOS. Renders the shared
// gldemo cube core into an ARGB buffer and pushes it with SYS_WIN_BLIT. (#319)
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "gldemo.h"

#define MAXW 1280
#define MAXH 800
static uint32_t g_blit[MAXW * MAXH];

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    int win = win_create("GL Cube", 120, 90, 740, 560);
    if (win < 0) return 1;
    int cw = 720, ch = 540;
    if (win_get_size(win, &cw, &ch) != 0 || cw <= 0 || ch <= 0) { cw = 720; ch = 540; }
    if (cw > MAXW) cw = MAXW; if (ch > MAXH) ch = MAXH;
    gldemo_init(GLDEMO_CUBE, cw, ch);

    gui_event_t ev;
    while (1) {
        int et = win_get_event(win, &ev, 16);
        if (et == EVENT_WINDOW_CLOSE) break;
        if (et == EVENT_RESIZE) {
            int nw, nh;
            if (win_get_size(win, &nw, &nh) == 0 && nw > 0 && nh > 0) {
                if (nw > MAXW) nw = MAXW; if (nh > MAXH) nh = MAXH;
                gldemo_resize(nw, nh);
            }
        }
        int w = gldemo_width(), h = gldemo_height();
        gldemo_frame(g_blit, w);
        syscall5(SYS_WIN_BLIT, win, 0, 0, (w & 0xFFFF) | ((h & 0xFFFF) << 16), (long)g_blit);
        win_invalidate(win);
    }
    gldemo_shutdown();
    win_destroy(win);
    return 0;
}
