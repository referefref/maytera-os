// Maytera Studio - main entry + event loop only. All logic lives in ui.c.
// Grows the former Paint app (main_v1_reference.c.txt) into a layered,
// GIMP-class image editor. The desktop still launches /apps/paint, so the
// build TARGET stays "paint" (see Makefile) though the window is titled
// "Maytera Studio".
#include "studio.h"
#include "../../libc/gui.h"

int main(int argc, char **argv) {
    // Tier-2 wire: answer a spawned `--contract` invocation BEFORE creating a
    // window (the stateless path; a live instance answers via contract_live_poll
    // in the loop below). This also lets `ctl describe paint` / `ctl list paint`
    // work whether or not Studio is running.
    if (contract_is_invocation(argc, argv))
        return contract_cli(argc, argv, &PAINT_CONTRACT);

    int win_w = 1180, win_h = 740;
    int win = win_create("Maytera Studio", 20, 20, win_w, win_h);
    if (win < 0) {
        printf("studio: window create failed\n");
        return 1;
    }

    // Modern chrome: gradients + soft elevation + antialiased TTF, matching the
    // Settings/Files design language.
    gui_set_style(GUI_STYLE_MODERN);
    // #472: the splash IS the real startup - it builds the initial document
    // (one opaque white layer) and registers every Colors/Filters op itself,
    // interleaved with the steps that say it is doing so, instead of running
    // that work silently after a canned animation.
    if (ui_splash(win, win_w, win_h) != 0) {
        printf("studio: doc alloc failed\n");
        win_destroy(win);
        return 1;
    }
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
        // Tier-2 wire: serve any AI/contract call delivered to THIS live
        // instance, on the timeout path too so it is served within ~100ms even
        // when the user is idle. contract_live_poll is non-blocking and the
        // action's actfn repaints itself; the return is a belt-and-braces
        // redraw. This is the whole point of the live path: the OS AI's action
        // lands on the document the user is looking at, not a throwaway spawn.
        if (contract_live_poll(&PAINT_CONTRACT))
            ui_full_redraw();
        // P6 assistant panel: poll the in-flight LLM request and animate the
        // thinking indicator from the loop, never from a blocking call, so the
        // canvas stays usable while a reply is on the wire (same cadence as
        // the contract poll above: every event and every 100 ms timeout).
        ui_tick();
        if (et == 0) continue;                 // timeout, nothing pending
        if (ev.type == EVENT_WINDOW_CLOSE) break;
        run = ui_handle_event(&ev);            // 0 => quit
    }

    doc_free();
    win_destroy(win);
    return 0;
}
