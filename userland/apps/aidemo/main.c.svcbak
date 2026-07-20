// aidemo - AI userland-compiler demo app (#294).
//
// A deliberately tiny app whose only job is to display ONE prominent string.
// The AI-driven build pipeline recompiles this app from a chat request (changing
// the string), redeploys the new ELF to /APPS/aidemo, and relaunches it, so a
// before/after screendump proves the compile -> deploy -> relaunch loop.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"

// ---- The one line the AI rewrites. Keep this marker stable. ----
#define AIDEMO_MESSAGE "AIDEMO v1: original build"

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    int win = win_create(AIDEMO_MESSAGE, 240, 200, 420, 200);
    if (win < 0) return 1;

    for (;;) {
        win_draw_rect(win, 0, 0, 420, 200, 0x00203040);
        win_draw_text_ttf(win, 24, 70, AIDEMO_MESSAGE, 22, 0x00FFFF80);
        win_draw_text(win, 24, 120, "AI userland compiler demo (#294)", 0x00FFFFFF);
        win_invalidate(win);

        gui_event_t ev;
        int et = win_get_event(win, &ev, 500);
        if (et != 0) {
            if (ev.type == EVENT_WINDOW_CLOSE) break;
        }
    }
    win_destroy(win);
    return 0;
}
