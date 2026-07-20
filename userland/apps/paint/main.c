// Maytera Studio - main entry + event loop only. All logic lives in ui.c.
// Grows the former Paint app (main_v1_reference.c.txt) into a layered,
// GIMP-class image editor. The desktop still launches /apps/paint, so the
// build TARGET stays "paint" (see Makefile) though the window is titled
// "Maytera Studio".
#include "studio.h"
#include "../../libc/gui.h"

int main(int argc, char **argv) {
    int win_w = 1180, win_h = 740;
    int win = win_create("Maytera Studio", 20, 20, win_w, win_h);
    if (win < 0) {
        printf("studio: window create failed\n");
        return 1;
    }

    // Modern chrome: gradients + soft elevation + antialiased TTF, matching the
    // Settings/Files design language.
    gui_set_style(GUI_STYLE_MODERN);
    ui_splash(win, win_w, win_h);   // brief branded splash before first frame

    // Build the initial document (one opaque white layer). ui_init draws.
    if (doc_new(STUDIO_DEF_W, STUDIO_DEF_H, argb(255, 255, 255, 255)) != 0) {
        printf("studio: doc alloc failed\n");
        win_destroy(win);
        return 1;
    }

    studio_register_all();   // register all Colors/Filters ops
    ui_init(win, win_w, win_h);

    // If launched with a file path (Files "Open with", or a shell arg), open it
    // over the blank document instead of showing an empty canvas.
    if (argc >= 2 && argv && argv[1] && argv[1][0])
        ui_open_path(argv[1]);

    ui_full_redraw();

    gui_event_t ev;
    int run = 1;
    while (run) {
        int et = win_get_event(win, &ev, 100);
        if (et == 0) continue;                 // timeout, nothing pending
        if (ev.type == EVENT_WINDOW_CLOSE) break;
        run = ui_handle_event(&ev);            // 0 => quit
    }

    doc_free();
    win_destroy(win);
    return 0;
}
