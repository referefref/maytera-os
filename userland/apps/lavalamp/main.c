// lavalamp - proper 3D lava lamp (TinyGL, metaball wax + buoyancy convection)
// for MayteraOS. Thin app wrapper over the shared gldemo lava lamp core
// (userland/libgl/src/gldemo_lavalamp.c), same pattern as glcube/glmatrix:
// render into an ARGB buffer, push with SYS_WIN_BLIT. Also selectable as a
// fullscreen screensaver (compositor SS_GLLAVALAMP, kernel id 23). (lavalamp)
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "gldemo.h"

#define MAXW 1280
#define MAXH 800
static uint32_t g_blit[MAXW * MAXH];

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    // portrait-ish window: the lamp is a tall object
    int win = win_create("Lava Lamp", 200, 40, 560, 720);
    if (win < 0) return 1;
    int cw = 540, ch = 700;
    if (win_get_size(win, &cw, &ch) != 0 || cw <= 0 || ch <= 0) { cw = 540; ch = 700; }
    if (cw > MAXW) cw = MAXW; if (ch > MAXH) ch = MAXH;
    gldemo_init(GLDEMO_LAVALAMP, cw, ch);

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
