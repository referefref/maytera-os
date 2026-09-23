// aquarium - hyper-real 3D reef aquarium (TinyGL) for MayteraOS. Thin app
// wrapper over the shared gldemo aquarium core
// (userland/libgl/src/gldemo_aquarium.c), same pattern as glcube/glmatrix/
// lavalamp: render into an ARGB buffer, push with SYS_WIN_BLIT. Also
// selectable as a fullscreen screensaver (compositor SS_GLAQUARIUM,
// kernel id 24).
//
// Options (the "diverse options" surface) are live-adjustable by keyboard,
// mapped straight onto gldemo_aquarium_option():
//   p  population (sparse / normal / busy)
//   f  species mix (all -> clown+tang+school -> angel+goby+school ->
//      big fish only -> school only)
//   t  water tint (clear blue / lagoon green / deep ocean / sunset)
//   l  light level (dim / normal / bright)
//   c  caustics + god rays on/off
//   b  bubbles (off / normal / dense)
//   v  camera (orbit / fixed / drift)
//   g  plant density (few / normal / lush)
//   d  detail (performance / quality)
// The screensaver path uses the defaults. (aquarium)
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "gldemo.h"

#define MAXW 1280
#define MAXH 800
static uint32_t g_blit[MAXW * MAXH];

// species mix presets cycled by 'f' (bit 0 clown, 1 tang, 2 angel, 3 goby,
// 4 neon school)
static const int SPECIES_PRESETS[] = { 31, 19, 28, 15, 16 };
#define NPRESET 5

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    int win = win_create("Aquarium", 120, 60, 900, 600);
    if (win < 0) return 1;
    int cw = 880, ch = 570;
    if (win_get_size(win, &cw, &ch) != 0 || cw <= 0 || ch <= 0) { cw = 880; ch = 570; }
    if (cw > MAXW) cw = MAXW; if (ch > MAXH) ch = MAXH;
    gldemo_init(GLDEMO_AQUARIUM, cw, ch);

    // local copies of cycling state (the core clamps everything anyway)
    int pop = 1, preset = 0, tint = 0, light = 1, caustics = 1;
    int bubbles = 1, camera = 0, plants = 1, quality = 1;

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
        if (et == EVENT_KEY_DOWN) {
            switch (ev.key_char) {
            case 'p': pop = (pop + 1) % 3;
                      gldemo_aquarium_option(AQOPT_POPULATION, pop); break;
            case 'f': preset = (preset + 1) % NPRESET;
                      gldemo_aquarium_option(AQOPT_SPECIES, SPECIES_PRESETS[preset]); break;
            case 't': tint = (tint + 1) % 4;
                      gldemo_aquarium_option(AQOPT_TINT, tint); break;
            case 'l': light = (light + 1) % 3;
                      gldemo_aquarium_option(AQOPT_LIGHT, light); break;
            case 'c': caustics ^= 1;
                      gldemo_aquarium_option(AQOPT_CAUSTICS, caustics); break;
            case 'b': bubbles = (bubbles + 1) % 3;
                      gldemo_aquarium_option(AQOPT_BUBBLES, bubbles); break;
            case 'v': camera = (camera + 1) % 3;
                      gldemo_aquarium_option(AQOPT_CAMERA, camera); break;
            case 'g': plants = (plants + 1) % 3;
                      gldemo_aquarium_option(AQOPT_PLANTS, plants); break;
            case 'd': quality ^= 1;
                      gldemo_aquarium_option(AQOPT_QUALITY, quality); break;
            default: break;
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
