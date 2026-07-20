// hello.c - Simple GUI Hello World app for MayteraOS
// Demonstrates user-space GUI capabilities

#include "../../libc/maytera.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("Hello GUI Test starting...\n");

    // Create a window
    int win = win_create("Hello World", 200, 150, 300, 200);
    if (win < 0) {
        printf("Failed to create window!\n");
        return 1;
    }

    printf("Window created with handle %d\n", win);

    // Draw some text
    win_draw_text(win, 50, 50, "Hello, MayteraOS!", 0x000000);
    win_draw_text(win, 50, 80, "User-space GUI works!", 0x0000FF);
    win_draw_text(win, 50, 110, "Press Ctrl+C to exit", 0x808080);

    // Request window redraw
    win_invalidate(win);

    // Simple event loop (wait for a bit then exit)
    for (int i = 0; i < 300; i++) {
        sleep(100);  // Sleep 100ms

        // Check for events
        int event = win_get_event(win, NULL, -1);
        if (event < 0) break;
    }

    // Clean up
    win_destroy(win);
    printf("Window destroyed, exiting.\n");

    return 0;
}
