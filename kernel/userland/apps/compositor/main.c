// main.c - MayteraOS Userland Compositor
// Entry point and frame orchestrator for the Phase 3 desktop compositor.
// Responsibilities: init, input polling, event dispatch, and render loop.

#include "compositor.h"
#include "../../libc/syscall.h"

// fb_info_t, fb_map(), fb_info(), fb_flip(), grab_input(), get_mouse_evt(),
// mouse_evt_t are all provided by ../../libc/syscall.h.

// ============================================================================
// Global state definitions (declared extern in compositor.h)
// ============================================================================

uint32_t *g_fb            = NULL;
int32_t   g_fb_width      = 0;
int32_t   g_fb_height     = 0;
int32_t   g_fb_pitch      = 0;

int32_t   g_mouse_x       = 0;
int32_t   g_mouse_y       = 0;
uint32_t  g_mouse_buttons = 0;
uint32_t  g_mouse_prev_buttons = 0;

bool g_start_menu_open      = false;
bool g_context_menu_open    = false;
int32_t g_context_menu_x   = 0;
int32_t g_context_menu_y   = 0;
bool g_wallpaper_picker_open = false;

bool g_logged_in    = false;
int  g_login_uid    = -1;
char g_login_username[64] = {0};

bool     g_screensaver_active = false;
uint64_t g_idle_ticks         = 0;

bool g_needs_redraw = true;

// ============================================================================
// Cursor data: 12x12 arrow
// 0 = transparent, 1 = black outline, 2 = white fill
// ============================================================================

static const uint8_t cursor_data[12][12] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,1,1,1,1,0,0,0},
    {1,2,2,1,2,1,0,0,0,0,0,0},
    {1,1,1,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},
};

// ============================================================================
// cursor_render: blit the 12x12 arrow onto the framebuffer at the mouse position
// ============================================================================

void cursor_render(void)
{
    for (int row = 0; row < 12; row++) {
        for (int col = 0; col < 12; col++) {
            uint8_t v = cursor_data[row][col];
            if (v == 0) {
                continue;
            }
            int px = g_mouse_x + col;
            int py = g_mouse_y + row;
            if (px < 0 || py < 0 || px >= g_fb_width || py >= g_fb_height) {
                continue;
            }
            uint32_t color = (v == 1) ? 0xFF000000u : 0xFFFFFFFFu;
            g_fb[py * g_fb_pitch + px] = color;
        }
    }
}

// ============================================================================
// compositor_init: set up the framebuffer and all subsystems
// ============================================================================

static int compositor_init(void)
{
    fb_info_t fi;
    if (fb_info(&fi) < 0) {
        return -1;
    }

    g_fb = (uint32_t *)(unsigned long)fb_map();
    if (!g_fb) {
        return -1;
    }

    g_fb_width  = (int32_t)fi.width;
    g_fb_height = (int32_t)fi.height;
    // pitch is in bytes; convert to pixel units (32-bit pixels = 4 bytes each)
    g_fb_pitch  = (int32_t)(fi.pitch / 4);

    // Request exclusive input so the kernel does not also process events
    grab_input(1);

    // Start mouse in screen center
    g_mouse_x = g_fb_width  / 2;
    g_mouse_y = g_fb_height / 2;

    // Capture baseline idle timestamp
    g_idle_ticks = (uint64_t)sys_clock();

    // Subsystem initialisation (order matters: wallpaper before desktop)
    wallpaper_init();
    desktop_init();
    taskbar_init();
    startmenu_init();
    contextmenu_init();
    screensaver_init();
    login_init();

    return 0;
}

// ============================================================================
// process_input: read mouse and keyboard; update global state
// ============================================================================

// Frame-local input state shared between process_input and process_events
static bool s_left_pressed  = false;
static bool s_left_released = false;
static bool s_right_pressed = false;
static bool s_dbl_click     = false;
static int  s_last_key      = -1;

// Double-click tracking
static uint64_t s_last_click_ticks = 0;
#define DBL_CLICK_THRESHOLD 500   // ticks (approximately milliseconds)

// #71 mouse-lag fix: see the main-loop comment below. Input is sampled every
// INPUT_POLL_MS while the actual screen repaint stays throttled to once every
// INPUT_POLLS_PER_FRAME * INPUT_POLL_MS (== the old fixed 16ms frame budget).
#define INPUT_POLL_MS         4
#define INPUT_POLLS_PER_FRAME 4

static void process_input(void)
{
    // Reset per-frame edge signals
    s_left_pressed  = false;
    s_left_released = false;
    s_right_pressed = false;
    s_dbl_click     = false;
    s_last_key      = -1;

    // Read mouse event (delta-based)
    mouse_evt_t mevt;
    memset(&mevt, 0, sizeof(mevt));
    get_mouse_evt(&mevt);
    int mx = mevt.dx;
    int my = mevt.dy;
    unsigned int buttons = mevt.buttons;

    // Apply delta and clamp to screen
    g_mouse_x += mx;
    g_mouse_y += my;
    if (g_mouse_x < 0)               g_mouse_x = 0;
    if (g_mouse_y < 0)               g_mouse_y = 0;
    if (g_mouse_x >= g_fb_width)     g_mouse_x = g_fb_width  - 1;
    if (g_mouse_y >= g_fb_height)    g_mouse_y = g_fb_height - 1;

    // Derive button edge events
    uint32_t prev = g_mouse_prev_buttons;

    // Left button
    if ((buttons & 1) && !(prev & 1)) {
        s_left_pressed = true;

        // Double-click detection
        uint64_t now = (uint64_t)sys_clock();
        if ((now - s_last_click_ticks) < DBL_CLICK_THRESHOLD) {
            s_dbl_click = true;
        }
        s_last_click_ticks = now;
    }
    if (!(buttons & 1) && (prev & 1)) {
        s_left_released = true;
    }

    // Right button
    if ((buttons & 2) && !(prev & 2)) {
        s_right_pressed = true;
    }

    g_mouse_buttons      = buttons;
    g_mouse_prev_buttons = buttons;

    // Read up to 4 pending keyboard events per frame
    bool got_input = (mx != 0 || my != 0 || buttons != prev);
    for (int i = 0; i < 4; i++) {
        int key = sys_get_keyboard();
        if (key < 0) {
            break;
        }
        s_last_key = key;
        got_input  = true;
    }

    if (got_input) {
        screensaver_on_input();
        g_idle_ticks   = (uint64_t)sys_clock();
        g_needs_redraw = true;
    }
}

// ============================================================================
// process_events: dispatch input to the correct UI layer
// ============================================================================

static void process_events(void)
{
    // While the screensaver is active, suppress all UI events
    if (screensaver_check_timeout()) {
        return;
    }

    // Keyboard: global shortcuts
    if (s_last_key >= 0) {
        int key = s_last_key;

        // F12 launches DOOM
        if (key == 0x86 /* F12 scancode */) {
            sys_exec("/apps/DOOM");
        }

        // ESC closes any open overlay
        if (key == 0x01 /* ESC */) {
            if (g_wallpaper_picker_open) {
                wallpaper_picker_close();
            } else if (g_start_menu_open) {
                startmenu_toggle();
            } else if (g_context_menu_open) {
                contextmenu_close();
            }
        }

        // Forward key to the login layer if it wants it (post-login this is a no-op)
        login_handle_key(key);
    }

    // Mouse event processing: work top-to-bottom through the UI stack.
    // Each handler returns true if it consumed the event.
    bool consumed = false;

    if (!consumed && g_wallpaper_picker_open) {
        consumed = wallpaper_picker_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    if (!consumed && g_start_menu_open) {
        consumed = startmenu_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    if (!consumed && g_context_menu_open) {
        consumed = contextmenu_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    if (!consumed) {
        consumed = taskbar_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    if (!consumed) {
        // Pass all click variants to the desktop icon layer
        desktop_handle_mouse(g_mouse_x, g_mouse_y,
                             s_left_pressed, s_right_pressed, s_dbl_click);

        // Right-click on the desktop (not inside any widget) opens the context menu
        if (s_right_pressed) {
            contextmenu_open(g_mouse_x, g_mouse_y);
        }
    }
}

// ============================================================================
// render_frame: composite all layers and present to screen
// ============================================================================

static void render_frame(void)
{
    // Screensaver takes over the full display
    if (g_screensaver_active) {
        screensaver_render();
        cursor_render();
        fb_flip();
        return;
    }

    // Layer order (back to front):
    wallpaper_render_background();   // 1. Background / wallpaper
    desktop_render();                // 2. Desktop icons
    desktop_render_version();        // 3. Version string overlay
    taskbar_render();                // 4. Taskbar bar

    if (g_start_menu_open) {
        startmenu_render();          // 5. Start menu (above taskbar)
    }

    if (g_context_menu_open) {
        contextmenu_render();        // 6. Context menu
    }

    if (g_wallpaper_picker_open) {
        wallpaper_render_picker();   // 7. Wallpaper picker dialog
    }

    clock_render();                  // 8. Floating clock widget
    cursor_render();                 // 9. Hardware cursor drawn last
    fb_flip();                       // 10. Present
}

// ============================================================================
// main: entry point
// ============================================================================

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // Print a startup banner to the serial console
    const char *banner = "MayteraOS Compositor starting...\n";
    for (const char *p = banner; *p; p++) {
        sys_putchar((int)(unsigned char)*p);
    }

    // Initialise the framebuffer and all subsystems
    if (compositor_init() < 0) {
        const char *err = "compositor_init failed\n";
        for (const char *p = err; *p; p++) {
            sys_putchar((int)(unsigned char)*p);
        }
        sys_exit(1);
    }

    // Block here until the user authenticates.
    // login_run() plays /SOUNDS/STARTUP.WAV on success internally.
    login_run();

    // Main compositor loop.
    //
    // #71 mouse-lag fix: previously this looped once per ~16ms (60 FPS) frame,
    // calling process_input() (which drains ONE accumulated mouse-delta event
    // via get_mouse_evt()) exactly once per frame. The real Apple mouse
    // reports at ~100Hz (bInterval=10ms) and the kernel HID poller now runs at
    // ~250Hz (usb_hid.c), so multiple fresh reports could already be sitting
    // in the kernel-side delta accumulator by the time this loop got back
    // around to draining them -- up to 16ms of pure coalescing/sampling delay
    // stacked on top of the xHCI latency, on every single frame. Fix: sample
    // input (process_input + process_events, both cheap: a couple of
    // syscalls and comparisons, no drawing) at a much finer grain
    // (INPUT_POLL_MS) than the actual screen repaint (render_frame(), which
    // stays at the same ~60 FPS / 16ms cadence it always had -- unchanged CPU
    // cost for drawing). get_mouse_evt() is delta-based, so polling it more
    // often just means each call returns a smaller delta; the position the
    // eventual redraw uses is simply fresher (as new as INPUT_POLL_MS instead
    // of as new as a full frame). Still entirely sys_sleep()-driven yields,
    // never a busy-spin (#426); total elapsed time per outer iteration is
    // unchanged (INPUT_POLLS_PER_FRAME * INPUT_POLL_MS == the old 16ms).
    while (1) {
        taskbar_update();     // Refresh gauge samples (CPU, RAM, disk)

        for (int i = 0; i < INPUT_POLLS_PER_FRAME; i++) {
            process_input();      // Poll mouse and keyboard
            process_events();     // Dispatch events to UI layers
            sys_sleep(INPUT_POLL_MS);
        }

        if (g_needs_redraw) {
            render_frame();
            g_needs_redraw = false;
        }
    }

    // Not reached; suppress compiler warnings about missing return
    return 0;
}
