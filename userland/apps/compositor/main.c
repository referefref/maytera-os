// main.c - MayteraOS Userland Compositor
// Entry point and frame orchestrator for the Phase 3 desktop compositor.
// Responsibilities: init, input polling, event dispatch, and render loop.
//
// NOTE: Do NOT call sys_putchar() anywhere in this file. It writes to the
// process's PTY (fds[1]), which has no reader, causing the compositor to
// block indefinitely and hang the entire desktop.

#include "compositor.h"
#include "../../libc/notify.h"
#include "../../libc/syscall.h"
#include "../../libc/theme.h"   // (#285) theme_color_of + color ids

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
uint64_t g_idle_ms           = 0;

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

// (#116) Cursor style + size, set from the UI profile (profile.c reads
// curstyle/cursize from UIPROFIL.YML). style: 0=Light, 1=Dark, 2=Glow.
// size is a scale percent: 100 = 1.0x (default, unchanged), up to 250 = 2.5x.
int g_cursor_style = 0;
int g_cursor_size  = 100;

static inline void cursor_plot(int px, int py, uint32_t color)
{
    if (px < 0 || py < 0 || px >= g_fb_width || py >= g_fb_height) return;
    g_fb[py * g_fb_pitch + px] = color;
}

void cursor_render(void)
{
    // (#116) Pull the live cursor style/size from the kernel every frame so a
    // change made in Settings (which calls set_cursor()) applies WITHOUT a reboot,
    // the same way theme/opacity propagate. Cheap syscall; packed style|size<<8.
    int pk = get_cursor();
    if (pk >= 0) {
        g_cursor_style = pk & 0xFF;
        int sz = (pk >> 8) & 0xFFFF;
        g_cursor_size = (sz >= 50) ? sz : 100;
    }

    int scale = (g_cursor_size >= 100 && g_cursor_size <= 250) ? g_cursor_size : 100;

    if (g_cursor_style == 2) {
        // Glow: a pulsing accent disc with a thin white line on the inner AND
        // outer edge so it stays readable over accent-colored buttons. Radius +
        // brightness pulse via a free-running frame counter (no time source).
        static unsigned tick = 0;
        tick++;
        unsigned ph  = tick & 63u;
        int      tri = (ph < 32u) ? (int)ph : (int)(64u - ph);   // 0..32 triangle
        int base = 5 * scale / 100; if (base < 5) base = 5;
        int r    = base + (base / 2) * tri / 32;                  // pulse the radius
        int br   = 205 + 50 * tri / 32;                           // 205..255 brightness
        // Bright, saturated azure: low R, mid-high G, full B, brightness-pulsed.
        uint32_t accent = 0xFF000000u
                        | ((uint32_t)(br / 5)        << 16)       // R
                        | ((uint32_t)(br * 7 / 10)   << 8)        // G
                        | ((uint32_t)br);                          // B
        const uint32_t white = 0xFFFFFFFFu;
        int cx = g_mouse_x, cy = g_mouse_y;
        for (int dy = -(r + 1); dy <= r + 1; dy++) {
            for (int dx = -(r + 1); dx <= r + 1; dx++) {
                int d2 = dx * dx + dy * dy;
                uint32_t col;
                if      (d2 <= (r - 2) * (r - 2)) col = accent;   // accent core
                else if (d2 <= (r - 1) * (r - 1)) col = white;    // inner white line
                else if (d2 <=  r      *  r)      col = accent;   // accent edge
                else if (d2 <= (r + 1) * (r + 1)) col = white;    // outer white line
                else continue;
                cursor_plot(cx + dx, cy + dy, col);
            }
        }
        return;
    }

    // Light (default) or Dark arrow, nearest-neighbor scaled by `scale`.
    int outdim = 12 * scale / 100;
    if (outdim < 12) outdim = 12;
    for (int oy = 0; oy < outdim; oy++) {
        int sy = oy * 100 / scale; if (sy > 11) sy = 11;
        for (int ox = 0; ox < outdim; ox++) {
            int sx = ox * 100 / scale; if (sx > 11) sx = 11;
            uint8_t v = cursor_data[sy][sx];
            if (v == 0) continue;
            uint32_t color;
            if (g_cursor_style == 1)              // Dark: white outline, dark fill
                color = (v == 1) ? 0xFFFFFFFFu : 0xFF202020u;
            else                                  // Light: black outline, white fill
                color = (v == 1) ? 0xFF000000u : 0xFFFFFFFFu;
            cursor_plot(g_mouse_x + ox, g_mouse_y + oy, color);
        }
    }
}

// ============================================================================
// compositor_init: set up the framebuffer and all subsystems
// ============================================================================

// #185 forward declarations (definitions live further down with launch_app)
static int  g_aichat_pid;
static void aichat_write_cfg(int enabled);

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

    // Capture baseline idle timestamp (monotonic ms)
    g_idle_ms = uptime_ms();


    // Subsystem initialisation (order matters: wallpaper before desktop)
    wallpaper_init();
    desktop_init();
    taskbar_init();
    // Load shared color icons (SVG-derived) for Computer + Browser (#66).
    icon_load_color(ICON_COMPUTER, "/ICONS/COMPUTER.ICN");
    icon_load_color(ICON_BROWSER,  "/ICONS/BROWSER.ICN");
    icon_load_color(ICON_TRASH,    "/ICONS/RECYCLE.ICN");
    icon_load_color(ICON_TERMINAL, "/ICONS/TERMINAL.ICN");
    icon_load_color(ICON_COG,      "/ICONS/SETTINGS.ICN");
    icon_load_color(ICON_GAME,     "/ICONS/DOOM.ICN");
    // #214: the GAMES start-menu entry for DOOM uses id ICON_GAME_DOOM, so load
    // the real DOOM icon into that id too. Keeps the menu icon identical to the
    // desktop DOOM icon (which uses ICON_GAME). Lemmings/Pong have no dedicated
    // .ICN and fall back to the built-in generic game glyph (acceptable).
    icon_load_color(ICON_GAME_DOOM, "/ICONS/DOOM.ICN");
    icon_load_color(ICON_IRC,      "/ICONS/IRC.ICN");
    icon_load_color(ICON_HIGHLIGHT,  "/ICONS/EDITOR.ICN");
    icon_load_color(ICON_CALCULATOR, "/ICONS/CALC.ICN");
    icon_load_color(ICON_IMAGE,      "/ICONS/IMGVIEW.ICN");
    icon_load_color(ICON_MUSIC,      "/ICONS/APLAYER.ICN");
    icon_load_color(ICON_VIDEO,      "/ICONS/MPLAYER.ICN");
    icon_load_color(ICON_CLOCK,      "/ICONS/CLOCK.ICN");
    icon_load_color(ICON_FOLDER,     "/ICONS/FILES.ICN");
    icon_load_color(ICON_NETWORK,    "/ICONS/NETWORK.ICN");
    icon_load_color(ICON_PAINT,         "/ICONS/PAINT.ICN");
    icon_load_color(ICON_GAME_SOLITAIRE,"/ICONS/SOLITR.ICN");
    icon_load_color(ICON_SLIDERS,       "/ICONS/SLIDERS.ICN");
    icon_load_color(ICON_CHEVD,         "/ICONS/CHEVD.ICN");
    icon_load_color(ICON_CHEVR,         "/ICONS/CHEVR.ICN");
    icon_load_color(ICON_WIN3X,         "/ICONS/WIN3X.ICN");   // #208 Win16 games
    icon_load_color(ICON_DOSAPP,        "/ICONS/DOSAPP.ICN");  // #208 DOS games
    startmenu_init();
    contextmenu_init();
    traymenu_init();
    profile_load();   /* #92 apply saved UI profile */
    stickies_load();  /* #270 load persisted sticky notes */
    {   /* #185 auto-launch the AI Chat app iff its persisted flag is enabled */
        extern int g_aichat_enabled;
        if (g_aichat_enabled) { aichat_write_cfg(1); g_aichat_pid = sys_spawn("/APPS/aichat"); }
    }
    set_cursor(g_cursor_style, g_cursor_size);  /* (#116) seed kernel with persisted cursor */
    dock_style_write_cfg(g_dock_style);  /* #387 seed Settings with the loaded dock style */
    profile_save();   /* ensure the profile file exists */
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
static bool s_dragging_sheep = false;
static bool s_dragging_dog = false;
static bool s_dragging_widget = false;
static bool s_dragging_sticky = false;    // sticky-note title-bar drag (#270)
static bool s_dragging_desktop = false;   // icon drag / rubber-band in progress
static bool s_dragging_settings = false;  // #419b settings-modal title-bar / scrollbar drag
// widgets.c drag API (relocatable desktop widgets)
int  widget_hit(int x, int y);
void widget_grab(int x, int y);
void widget_drag_to(int x, int y);
void widget_release(void);
// #419b widgets.c draggable settings modal + scrollable HA entity list
int  widget_settings_press(int x, int y);
void widget_settings_drag_to(int x, int y);
void widget_settings_drag_end(void);
int  widget_settings_handle_scroll(int x, int y, int delta);
// widgets.c per-widget right-click menu (Hide / Lock)
void widget_menu_open(int which, int x, int y);
int  widget_menu_is_open(void);
void widget_menu_render(void);
int  widget_menu_handle(int x, int y, int click);

// Double-click tracking
static uint64_t s_last_click_ticks = 0;
#define DBL_CLICK_THRESHOLD 500   // ticks (approximately milliseconds)

static void process_input(void)
{
    // Reset per-frame edge signals
    s_left_pressed  = false;
    s_left_released = false;
    s_right_pressed = false;
    s_dbl_click     = false;
    s_last_key      = -1;

    // Read absolute mouse position from kernel.
    // The kernel's SYS_GET_MOUSE returns absolute screen coordinates
    // via three separate pointer arguments (not a struct).
    int abs_x = 0, abs_y = 0;
    unsigned int buttons = 0;
    get_mouse(&abs_x, &abs_y, &buttons);

    // Detect movement by comparing with previous frame position.
    int mx = abs_x - g_mouse_x;
    int my = abs_y - g_mouse_y;

    // Use the kernel-tracked position directly (already clamped).
    g_mouse_x = abs_x;
    g_mouse_y = abs_y;

    // Derive button edge events
    uint32_t prev = g_mouse_prev_buttons;

    // Suppress button events during the first 2 seconds after compositor
    // start to avoid phantom right-clicks from stale mouse state.
    static uint64_t s_boot_ticks = 0;
    if (s_boot_ticks == 0) s_boot_ticks = (uint64_t)sys_clock();
    bool suppressed = ((uint64_t)sys_clock() - s_boot_ticks) < 500; // 2 seconds at 250 Hz


    // Left button
    if (!suppressed && (buttons & 1) && !(prev & 1)) {
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
    if (!suppressed && (buttons & 2) && !(prev & 2)) {
        s_right_pressed = true;
    }

    g_mouse_buttons      = buttons;
    g_mouse_prev_buttons = buttons;

    // Read up to 8 pending keyboard events per frame.
    // Key releases have bit 7 set (value >= 0x80 for regular keys, or in
    // special ranges). We forward releases to apps but only track presses
    // in s_last_key so that compositor shortcuts fire on the press event.
    bool got_input = (mx != 0 || my != 0 || buttons != prev);
    for (int i = 0; i < 8; i++) {
        int key = sys_get_keyboard();
        if (key < 0) {
            break;
        }
        got_input = true;
        // Forward every key (press and release) to app windows
        sys_inject_key(key);
        // Track key-down events for compositor shortcuts.
        // Regular presses: 0x00-0x7F (ASCII chars).
        // Special key presses: 0x80-0x8F (F1-F12, arrows, etc.).
        // Special key releases: 0x90-0x9F (special key + 0x10).
        // Regular key releases: char | 0x80 (>= 0xA0 for printable).
        // We want to process 0x00-0x8F as key-down events.
        if (key >= 0 && key <= 0x8F) {
            s_last_key = key;
        }
    }

    // (#200 SkiFree) A running Win16 app (e.g. SkiFree/TETRIS) is the SOLE
    // keyboard consumer: SYS_GET_KEYBOARD returns -1 to us while it owns the
    // foreground, so the loop above never sees a key and our idle timer would
    // run out and the screensaver would black the game out. Treat a foreground
    // Win16 app as continuous activity so the screensaver stays away while one
    // is up (it still arms normally once the Win16 app is closed).
    if (sys_win16_active()) {
        got_input = true;
    }

    if (got_input) {
        screensaver_on_input();
        g_idle_ms      = uptime_ms();
        g_needs_redraw = true;
    }
}

// ============================================================================
// launch_app: fork + exec helper (never call sys_putchar in here)
// ============================================================================
static void launch_app(const char *path)
{
    // Use sys_spawn() instead of fork+exec. Forking the compositor process
    // hangs the OS because it duplicates the framebuffer mapping and
    // exclusive input state.
    sys_spawn(path);
}

// ============================================================================
// AI Chat widget lifecycle (#185). The chat is a separate userland app
// (/APPS/aichat), but it is toggled like an in-compositor widget: g_aichat_enabled
// (widgets.c) is the persisted flag. Enable launches the app once; disable writes
// "enabled=0" into /CONFIG/AICHAT.CFG, which the running app polls and then exits.
// We do not have a userland kill syscall, so disable relies on this cooperative
// self-exit. The app's panel width is also stored in that file, so we preserve it.
// ============================================================================
/* g_aichat_pid forward-declared above */ static int g_aichat_pid = -1;

// Rewrite /CONFIG/AICHAT.CFG keeping any existing width, with the given enabled.
static void aichat_write_cfg(int enabled)
{
    int width = 380;     // defaults preserved across rewrites
    int position = 0;    // #185: 0=right 1=left 2=top
    int fd = sys_open("/CONFIG/AICHAT.CFG", 0 /*O_RDONLY*/);
    if (fd >= 0) {
        char rb[256];
        long n = sys_read(fd, rb, sizeof(rb) - 1);
        sys_close(fd);
        if (n > 0) {
            rb[n] = 0;
            for (int i = 0; rb[i]; i++) {
                if (rb[i]=='w'&&rb[i+1]=='i'&&rb[i+2]=='d'&&rb[i+3]=='t'&&rb[i+4]=='h'&&rb[i+5]=='=') {
                    int j = i + 6, v = 0, any = 0;
                    while (rb[j] >= '0' && rb[j] <= '9') { v = v*10 + (rb[j]-'0'); j++; any = 1; }
                    if (any) width = v;
                } else if (rb[i]=='p'&&rb[i+1]=='o'&&rb[i+2]=='s'&&rb[i+3]=='i'&&rb[i+4]=='t'&&
                           rb[i+5]=='i'&&rb[i+6]=='o'&&rb[i+7]=='n'&&rb[i+8]=='=') {
                    int j = i + 9, v = 0, any = 0;
                    while (rb[j] >= '0' && rb[j] <= '9') { v = v*10 + (rb[j]-'0'); j++; any = 1; }
                    if (any) position = v;
                }
            }
        }
    }
    int wfd = sys_open("/CONFIG/AICHAT.CFG", 0x41 | 0x200 /*O_WRONLY|O_CREAT|O_TRUNC*/);
    if (wfd < 0) return;
    char buf[96]; int len = 0;
    const char *wk = "width=";
    for (const char *c = wk; *c; c++) buf[len++] = *c;
    { char t[8]; int tn = 0, w = width; if (w <= 0) w = 380;
      while (w) { t[tn++] = '0' + w % 10; w /= 10; } if (tn == 0) t[tn++] = '0';
      while (tn) buf[len++] = t[--tn]; }
    buf[len++] = '\n';
    const char *ek = "enabled=";
    for (const char *c = ek; *c; c++) buf[len++] = *c;
    buf[len++] = enabled ? '1' : '0';
    buf[len++] = '\n';
    const char *pk = "position=";
    for (const char *c = pk; *c; c++) buf[len++] = *c;
    buf[len++] = (char)('0' + (position >= 0 && position <= 2 ? position : 0));
    buf[len++] = '\n';
    sys_write(wfd, buf, (unsigned long)len);
    sys_close(wfd);
}

// Toggle the AI Chat app on/off. Called from the tray menu (traymenu.c) and at
// startup. Robust: never wedges the compositor; guards every path.
void aichat_set_enabled(int on)
{
    extern int g_aichat_enabled;
    g_aichat_enabled = on ? 1 : 0;
    aichat_write_cfg(g_aichat_enabled);   // app polls this file to self-exit
    if (on) {
        // launch only if we have not already launched a live instance
        if (g_aichat_pid < 0) g_aichat_pid = sys_spawn("/APPS/aichat");
    } else {
        // cooperative shutdown via the cfg flag above; forget the pid so a
        // subsequent enable relaunches a fresh instance.
        g_aichat_pid = -1;
    }
}

// #387: dock-layout live channel. The Settings app writes a single ASCII digit
// (0..3) to /DOCKSTYL.CFG when the user picks a dock style; the compositor polls
// it and applies the layout live, then profile_tick() persists dock_style into
// UIPROFIL.YML. On boot the compositor writes the current (profile-loaded) value
// back so Settings shows the right selection.
void dock_style_write_cfg(int v) {
    if (v < 0 || v > 3) v = 0;
    int fd = sys_open("/DOCKSTYL.CFG", 0x41 | 0x200 /*O_WRONLY|O_CREAT|O_TRUNC*/);
    if (fd < 0) return;
    char c = (char)('0' + v);
    sys_write(fd, &c, 1);
    sys_close(fd);
}
static void dock_style_poll(void) {
    int fd = sys_open("/DOCKSTYL.CFG", 0 /*O_RDONLY*/);
    if (fd < 0) return;
    char c = 0;
    long n = sys_read(fd, &c, 1);
    sys_close(fd);
    if (n != 1) return;
    int v = c - '0';
    if (v < 0 || v > 3) return;
    if (v != g_dock_style) {
        taskbar_set_style(v);      // apply live (forces a full redraw)
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

        // F1 toggles the command launcher (Spotlight) - a keyboard equivalent of
        // the taskbar Maytera-logo button, so the launcher is reachable without
        // the pointer. Handled before the modal key-capture below so it can also
        // close the launcher.
        if (key == 0x88 /* F1 */) {
            launcher_toggle();
            s_last_key = -1;
        } else
        // The command launcher (Spotlight) captures ALL keys while open.
        if (g_launcher_open) {
            launcher_handle_key(key);
            s_last_key = -1;
        } else
        // The widget Settings dialog text field captures all keys while open.
        if (widget_settings_is_open()) {
            widget_settings_handle_key(key);
            s_last_key = -1;
        } else if (stickies_editing()) {
            // A focused sticky note captures all typing (text, backspace, ESC).
            stickies_handle_key(key);
            s_last_key = -1;
        } else {

        // Function-key shortcuts removed per user request: F11 was opening the
        // start menu and F7 (0x8A) was launching the Terminal, both firing by
        // accident. Use the taskbar start button and the menu/desktop to launch
        // apps. (F5 settings launcher removed too for consistency.)


        // F11: toggle maximize/restore of the focused app window.
        if (key == 0x85) {
            sys_wm_maximize_focused();
            s_last_key = -1;
        }

        // ESC closes any open overlay (kernel maps ESC to 0x1B)
        if (key == 0x1B /* ESC */) {
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
        }   // end else (widget Settings dialog not open)
    }

    // Mouse event processing: work top-to-bottom through the UI stack.
    // Each handler returns true if it consumed the event.
    bool consumed = false;

    // Desktop pet: once grabbed, keep dragging until the button is released,
    // then let it fall under gravity onto the taskbar.
    if (s_dragging_sheep) {
        if (g_mouse_buttons & 1) { sheep_drag_to(g_mouse_x, g_mouse_y); }
        else { sheep_release(); s_dragging_sheep = false; }
        consumed = true;
    } else if (s_dragging_dog) {
        if (g_mouse_buttons & 1) { dog_drag_to(g_mouse_x, g_mouse_y); }
        else { dog_release(); s_dragging_dog = false; }
        consumed = true;
    } else if (s_dragging_widget) {
        if (g_mouse_buttons & 1) { widget_drag_to(g_mouse_x, g_mouse_y); }
        else { widget_release(); s_dragging_widget = false; profile_save(); }
        consumed = true;
    } else if (s_dragging_settings) {
        // #419b: dragging the settings modal by its title bar (or its list scrollbar).
        if (g_mouse_buttons & 1) { widget_settings_drag_to(g_mouse_x, g_mouse_y); }
        else { widget_settings_drag_end(); s_dragging_settings = false; }
        consumed = true;
    } else if (s_dragging_sticky) {
        if (g_mouse_buttons & 1) { stickies_drag_to(g_mouse_x, g_mouse_y); }
        else { stickies_release(); s_dragging_sticky = false; }
        consumed = true;
    } else if (s_dragging_desktop) {
        // Desktop icon drag or rubber-band selection in progress.
        if (g_mouse_buttons & 1) { desktop_drag(g_mouse_x, g_mouse_y); }
        else { desktop_release(g_mouse_x, g_mouse_y); s_dragging_desktop = false; }
        consumed = true;
    }

    // The command launcher (Spotlight) is modal: while open it captures every
    // click (run a suggestion, or click outside to cancel) before anything else.
    if (!consumed && g_launcher_open) {
        launcher_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed ? 1 : 0);
        consumed = true;
    }

    // #419b: while the settings modal is open, the mouse wheel scrolls its
    // (HA) entity list. Read + consume the scroll here so it does not also
    // reach the window manager underneath the modal.
    if (!consumed && widget_settings_is_open()) {
        int sd = get_mouse_scroll();
        if (sd != 0) widget_settings_handle_scroll(g_mouse_x, g_mouse_y, sd);
    }

    // The widget Settings dialog (modal) captures clicks while open. A press on
    // the title bar starts a modal drag; a press on the list scrollbar starts a
    // scroll drag; anything else is a normal control click (#419b).
    if (!consumed && widget_settings_is_open() && s_left_pressed) {
        if (widget_settings_press(g_mouse_x, g_mouse_y)) s_dragging_settings = true;
        consumed = true;
    }

    // An open per-widget menu captures the next click (select item or dismiss).
    if (!consumed && widget_menu_is_open() && (s_left_pressed || s_right_pressed)) {
        widget_menu_handle(g_mouse_x, g_mouse_y, 1);
        consumed = true;
    }

    if (!consumed && g_wallpaper_picker_open) {
        consumed = wallpaper_picker_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    if (!consumed && g_start_menu_open) {
        consumed = startmenu_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    if (!consumed && g_context_menu_open) {
        consumed = contextmenu_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    if (!consumed && g_tray_menu_open) {
        consumed = traymenu_handle_mouse(g_mouse_x, g_mouse_y,
                                         s_left_pressed, (g_mouse_buttons & 1) != 0);
    }

    // #168: notification toasts / center get first crack at clicks.
    if (!consumed) {
        consumed = notif_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    // #241: while the performance popup is open, it gets first crack at the
    // click (anywhere on screen) so it can be dismissed or acted on.
    if (!consumed && taskbar_popup_active()) {
        consumed = taskbar_popup_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    if (!consumed) {
        consumed = taskbar_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    // --- Forward mouse to the kernel window manager (app windows) ---
    // Under exclusive mode the kernel desktop loop does not process input, so we
    // relay mouse activity into the kernel WM for window dragging, title-bar
    // buttons (minimize/maximize/close), resize grips, and click-to-focus.
    //
    // MOVE and UP are injected unconditionally so an in-progress drag/resize
    // keeps tracking and always terminates, even if the cursor passes over the
    // taskbar or desktop. The kernel WM ignores MOVE/UP when no drag is active,
    // so this is harmless. DOWN is only injected when no compositor overlay or
    // the taskbar has already consumed the click; if a window consumes the DOWN,
    // we mark the event consumed so the desktop-icon layer does not also fire.
    {
        static int s_prev_mx = -1;
        static int s_prev_my = -1;

        if (g_mouse_x != s_prev_mx || g_mouse_y != s_prev_my) {
            sys_inject_mouse(g_mouse_x, g_mouse_y, MOUSE_EVENT_MOVE, 0);
            s_prev_mx = g_mouse_x;
            s_prev_my = g_mouse_y;
        }

        // OS-wide mouse wheel: read the kernel scroll delta and dispatch an
        // EVENT_MOUSE_SCROLL to the window under the cursor so any app that
        // handles scrolling (browser, files, chat transcript, ...) responds.
        {
            int sd = get_mouse_scroll();
            if (sd != 0) {
                sys_inject_mouse(g_mouse_x, g_mouse_y, MOUSE_EVENT_SCROLL, sd);
            }
        }

        if (!consumed && s_left_pressed) {
            if (sys_inject_mouse(g_mouse_x, g_mouse_y, MOUSE_EVENT_DOWN, 1) > 0) {
                consumed = true;
            }
        }

        if (s_left_released) {
            sys_inject_mouse(g_mouse_x, g_mouse_y, MOUSE_EVENT_UP, 1);
        }

        // Right-button press: route to the window under the cursor so the app
        // can show its own context menu. If a window is hit, mark the event
        // consumed so the desktop context menu does NOT also open (#87: no more
        // right-clicking "through" an app onto the desktop).
        if (!consumed && s_right_pressed) {
            if (sys_inject_mouse(g_mouse_x, g_mouse_y, MOUSE_EVENT_DOWN, 2) > 0) {
                consumed = true;
            }
        }
    }

    // Sticky notes (#270): a press on a note edits/drags/closes it. Notes sit in
    // the desktop widget layer (behind app windows), so this runs after the WM
    // injection but before framework widgets / sheep / desktop icons.
    if (!consumed && s_left_pressed && stickies_hit(g_mouse_x, g_mouse_y) >= 0) {
        if (stickies_press(g_mouse_x, g_mouse_y)) {
            if (stickies_is_dragging()) s_dragging_sticky = true;
            consumed = true;
        }
    }
    // A left-press that misses every note still commits an in-progress edit.
    else if (!consumed && s_left_pressed && stickies_editing()) {
        stickies_press(g_mouse_x, g_mouse_y);   // hit<0 path: deselect + save
    }

    // Relocatable desktop widgets: grab a widget if the press landed on it and no
    // window/overlay/taskbar consumed it first (widgets sit behind app windows).
    if (!consumed && s_left_pressed && widget_hit(g_mouse_x, g_mouse_y) >= 0) {
        widget_grab(g_mouse_x, g_mouse_y);
        s_dragging_widget = true;
        consumed = true;
    }

    // Grab the sheep if the press landed on it (and no window/overlay took it).
    if (!consumed && s_left_pressed && dog_hit(g_mouse_x, g_mouse_y)) {
        dog_grab(g_mouse_x, g_mouse_y);
        s_dragging_dog = true;
        consumed = true;
    }
    if (!consumed && s_left_pressed && sheep_hit(g_mouse_x, g_mouse_y)) {
        sheep_grab(g_mouse_x, g_mouse_y);
        s_dragging_sheep = true;
        consumed = true;
    }

    if (!consumed) {
        // Right-click on a desktop widget opens ONLY its menu (Hide/Lock/Settings).
        // Previously the desktop layer ALSO opened its context menu on the same
        // right-click, so both popped up at once. Now: if the cursor is over a
        // widget, open the widget menu and do not run the desktop handler;
        // otherwise the desktop layer handles icons + its own right-click menu.
        int wh = s_right_pressed ? widget_hit(g_mouse_x, g_mouse_y) : -1;
        if (wh >= 0) {
            widget_menu_open(wh, g_mouse_x, g_mouse_y);
        } else if (s_left_pressed) {
            // Begin an icon drag or a rubber-band selection. The drag is then
            // tracked frame-to-frame at the top of process_events via
            // s_dragging_desktop; release decides click vs drag (and launches
            // on double-click within the window).
            if (desktop_press(g_mouse_x, g_mouse_y)) {
                s_dragging_desktop = true;
            }
        } else {
            // Right-click (context menu) and kernel double-click launches.
            desktop_handle_mouse(g_mouse_x, g_mouse_y,
                                 s_left_pressed, s_right_pressed, s_dbl_click);
        }
    }
}

// ============================================================================
// Window drop shadows (#160, Phase 4b) - drawn by the compositor onto the
// desktop layer BEFORE the kernel paints app windows on top, so each window
// casts a soft shadow into the desktop margin around its bottom/right edges.
// Gated on MODERN themes; Classic (theme id 4) draws nothing.
// ============================================================================

int g_compositor_theme_id = 1;   // last theme applied (set by compositor_apply_theme)

// True-fullscreen app detection: a visible, non-minimized window whose bounds
// cover the ENTIRE framebuffer (incl. the bottom dock strip window_maximize
// leaves at fb_h-80). A maximized normal window stops at fb_h-80, so it never
// matches -> it keeps the taskbar. Used to suppress the desktop chrome so a
// fullscreen game (Maytera Arena) covers the whole screen.
static bool fullscreen_app_on_top(void) {
    wm_window_info_t wins[16];
    int n = wm_get_windows(wins, 16);
    for (int i = 0; i < n; i++) {
        wm_window_info_t *w = &wins[i];
        if (!w->visible || w->minimized) continue;
        if (w->x <= 0 && w->y <= 0 &&
            w->x + w->width  >= (int)g_fb_width &&
            w->y + w->height >= (int)g_fb_height)
            return true;
    }
    return false;
}
extern int g_draw_blend;          // 0-255 alpha for draw_* (defined in draw.c)

static void windows_render_shadows(void)
{
    return;  // #189: app window drop shadows removed (user request)
    if (g_compositor_theme_id == 4) return;   // Classic: no shadows, zero cost

    wm_window_info_t wins[16];
    int n = wm_get_windows(wins, 16);
    if (n <= 0) return;

    const uint32_t SH = 0xFF000000;   // black, alpha-blended via g_draw_blend
    const int FE   = 10;              // feather radius: rings expanding from the edge
    const int CORE = 60;              // alpha at the window edge (innermost ring)
    const int BX = 1, BY = 2;         // gentle drop bias (down + right)

    // window_list is top-first; draw bottom-to-top so upper shadows layer last.
    for (int i = n - 1; i >= 0; i--) {
        wm_window_info_t *w = &wins[i];
        if (!w->visible || w->minimized) continue;
        if (w->width < 8 || w->height < 8) continue;
        // Skip maximized / full-width windows (a screen-wide shadow band looks wrong).
        if (w->width >= g_fb_width - 2) continue;

        // Single-contribution distance-field shadow: expanding 1px rings that HUG
        // the window edge (s=1 is the ring just outside the frame) and fade out to
        // FE. Each pixel belongs to exactly ONE ring (its distance), so corners
        // fade uniformly with no additive seam/dark-point, and there is no offset
        // gap. The small BX/BY bias pushes the rings down+right for a drop feel
        // (the window overpaints its interior, so top/left stay subtle).
        for (int s = 1; s <= FE; s++) {
            int a = CORE * (FE - s + 1) / (FE + 1);
            if (a < 3) a = 3;
            g_draw_blend = a;
            draw_rect_outline(w->x - s + BX, w->y - s + BY,
                              w->width + 2 * s, w->height + 2 * s, SH);
        }
    }
    g_draw_blend = 255;   // restore opaque default
}

// ============================================================================
// render_frame: composite all layers and present to screen
// ============================================================================

static void apply_display_effects(void);

static void render_frame(void)
{
    // #102/#379: a full frame always composites the whole screen; reset any clip
    // left over from an idle dirty-rect present.
    draw_clear_clip();
    // Tell the kernel to stop blitting user windows straight to the FB while the
    // screensaver owns the display (otherwise e.g. the terminal's cursor-blink
    // redraws punch through the screensaver). Only fires on state transitions.
    static bool s_ss_blank_prev = false;
    if (g_screensaver_active != s_ss_blank_prev) {
        set_win_blank(g_screensaver_active ? 1 : 0);
        s_ss_blank_prev = g_screensaver_active;
    }

    // Screensaver takes over the full display
    if (g_screensaver_active) {
        screensaver_render();
        cursor_render();
        apply_display_effects();
        fb_flip();
        return;
    }

    // Layer order (back to front):
    wallpaper_render_background();   // 1. Background / wallpaper
    desktop_render();                // 2. Desktop icons
    desktop_render_version();        // 3. Version string overlay
    widgets_render();                // 3b. Desktop widgets (clock/calendar/pet)
    stickies_render();               // 3b2. Desktop sticky notes (#270)
    windows_render_shadows();        // 3c. drop shadows (DISABLED #189)
    compositor_render_windows();     // 4. Kernel draws app windows on our FB
    desktop_render_overlay();        // 4b. Rubber-band selection rectangle
    // Fullscreen app on top -> hide desktop chrome (taskbar/start/dock/clock).
    bool fs = fullscreen_app_on_top();

    if (!fs) taskbar_render();        // 5. Taskbar bar (above windows)

    if (!fs && g_start_menu_open) {
        startmenu_render();          // 6. Start menu (above taskbar)
    }

    if (g_context_menu_open) {
        contextmenu_render();        // 7. Context menu
    }

    if (g_wallpaper_picker_open) {
        wallpaper_render_picker();   // 8. Wallpaper picker dialog
    }

    if (!fs && g_tray_menu_open) {
        traymenu_render();           // 8b. Tray quick-settings menu (above taskbar)
    }

    if (!fs) clock_render();         // 9. Floating clock widget
    if (!fs) widget_menu_render();   // 9b. Per-widget right-click menu (Hide/Lock/Settings)
    if (!fs) widget_settings_render();// 9c. Per-widget Settings dialog (modal)
    if (!fs) launcher_render();      // 9c2. Command launcher (Spotlight) overlay
    notif_render();                  // 9d. Notification toasts + center (#168)
    cursor_render();                 // 10. Hardware cursor drawn last
    apply_display_effects();         // 10b. Brightness / night-light
    fb_flip();                       // 11. Present
}

// ============================================================================
// render_frame_idle: DIRTY-RECT present (#102/#379). Recomposite ONLY the
// damage rectangles collected this frame, each redrawn back-to-front (same
// z-order as render_frame) but CLIPPED to that rect, then a single present.
//
// Reached only on the pure-idle desktop: no visible app windows, no open menu /
// dialog, no drag, no screensaver, no recent input. So the interactive overlays
// and kernel app-window compositing are not needed here; the persistent back
// buffer already holds the last full frame, and unchanged regions are left
// untouched. This is what drops idle host CPU from ~full-core to single digits:
// instead of blitting the whole 1280x800 wallpaper + every widget's TTF text at
// a fixed cadence, only the small rects that changed are redrawn (and if nothing
// changed, damage_count()==0 and we present nothing at all).
// ============================================================================
static void render_frame_idle(void)
{
    g_widgets_draw_only = 1;         // widgets draw without re-advancing state
    int n = damage_count();
    for (int i = 0; i < n; i++) {
        int x, y, w, h;
        if (!damage_get(i, &x, &y, &w, &h)) continue;
        draw_set_clip(x, y, w, h);
        // b740: tell the kernel to present ONLY this rectangle. Without this the
        // kernel sys_fb_flip fell back to a full ~4MB back->front copy every idle
        // frame; reporting the exact redrawn rects turns the present partial.
        fb_damage(x, y, w, h);

        wallpaper_render_background();   // 1. wallpaper (clipped blit)
        desktop_render();                // 2. desktop icons
        desktop_render_version();        // 3. version overlay
        widgets_render();                // 3b. widgets + pets (draw-only)
        stickies_render();               // 3b2. sticky notes
        taskbar_render();                // 5. taskbar
        notif_render();                  // 9d. any lingering toast overlapping
        cursor_render();                 // 10. cursor on top
        apply_display_effects();         // 10b. brightness / night-light (clipped)
    }
    draw_clear_clip();
    g_widgets_draw_only = 0;
    fb_flip();                       // 11. single present (kernel copies back->front)
}

// ============================================================================
// main: entry point
// ============================================================================

// --- Scalable UI text via the kernel TTF engine (#58) ---
int g_font_px = 16;   // Medium default
void draw_text_ttf(int32_t x, int32_t y, const char *text, int size, uint32_t color) {
    // #102/#379 dirty-rect: text is rasterized in the KERNEL (SYS_DRAW_TTF) and
    // cannot honor the userland clip per-pixel, so cull the whole syscall when
    // the text's (generous) bounding box lies entirely outside the active clip.
    // Widgets are placed apart, so a vertical-band + left-edge test never drops
    // visible text but skips every off-region string during a clipped recompose.
    if (size < 4) size = 4;
    if (y + 2 * size < g_clip_y0 || y - size > g_clip_y1 ||
        x > g_clip_x1 || x + 4096 < g_clip_x0) {
        return;
    }
    ttf_text(x, y, text, size, color);
}
int text_width_ttf(const char *text, int size) {
    return ttf_measure(text, size);
}

// --- Display post-effects: brightness dim + night-light warm tint (#57) ---
int g_brightness = 100;   // 100 = normal
int g_win_opacity = 242;  // global default window opacity (0-255); ~95% default
int g_nightlight = 0;     // 0 = off, else warm-tint strength 1-100

static void apply_display_effects(void)
{
    if (g_brightness >= 100 && g_nightlight <= 0) return;  // nothing to do
    // #102/#379: honor the active clip so the idle path post-processes ONLY the
    // freshly-composited dirty rect (applying the effect to already-dimmed
    // unchanged pixels would darken them again every frame).
    int y0 = g_clip_y0 < 0 ? 0 : g_clip_y0;
    int x0 = g_clip_x0 < 0 ? 0 : g_clip_x0;
    int y1 = g_clip_y1 > g_fb_height ? g_fb_height : g_clip_y1;
    int x1 = g_clip_x1 > g_fb_width  ? g_fb_width  : g_clip_x1;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = &g_fb[yy * g_fb_pitch];
        for (int xx = x0; xx < x1; xx++) {
            uint32_t c = row[xx];
            uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
            if (g_brightness < 100) {
                r = r * (uint32_t)g_brightness / 100;
                g = g * (uint32_t)g_brightness / 100;
                b = b * (uint32_t)g_brightness / 100;
            }
            if (g_nightlight > 0) {
                b = b * (uint32_t)(100 - g_nightlight) / 100;
                g = g * (uint32_t)(100 - g_nightlight / 2) / 100;
            }
            row[xx] = (c & 0xFF000000u) | (r << 16) | (g << 8) | b;
        }
    }
}

// --- Live desktop icon sizing (driven by get_icon_size(), see #63) ---
int DESKTOP_ICON_SIZE = 48;
int DESKTOP_ICON_SPACING_X = 100;
int DESKTOP_ICON_SPACING_Y = 90;

void compositor_apply_icon_size(int sz)
{
    switch (sz) {
    case 0: DESKTOP_ICON_SIZE=32; DESKTOP_ICON_SPACING_X=72;  DESKTOP_ICON_SPACING_Y=66;  break;
    case 2: DESKTOP_ICON_SIZE=64; DESKTOP_ICON_SPACING_X=128; DESKTOP_ICON_SPACING_Y=116; break;
    default: DESKTOP_ICON_SIZE=48; DESKTOP_ICON_SPACING_X=100; DESKTOP_ICON_SPACING_Y=90; break;
    }
}

// --- Theme-aware chrome colors (driven by get_theme(), see #55) ---
uint32_t CLR_TASKBAR_BG = 0xFF2D2D2D;
uint32_t CLR_TASKBAR_BORDER = 0xFF505050;
uint32_t CLR_TASKBAR_HOVER = 0xFF4A4A4A;
uint32_t CLR_START_BTN = 0xFF2D2D2D;
uint32_t CLR_MENU_BG = 0xFF2D2D2D;
uint32_t CLR_MENU_SHADOW = 0xFF1A1A1A;
uint32_t CLR_MENU_BORDER = 0xFF606060;
uint32_t CLR_MENU_ITEM_HOVER = 0xFF4A4A4A;
uint32_t CLR_MENU_ITEM_NORM = 0xFF383838;
uint32_t CLR_MENU_CAT_BG = 0xFF404050;
uint32_t CLR_MENU_TEXT = 0xFFE0E0E0;
uint32_t CLR_MENU_SEP = 0xFF505050;
uint32_t CLR_CTX_BG = 0xFF303030;
uint32_t CLR_CTX_BORDER = 0xFF606060;
uint32_t CLR_CTX_HOVER = 0xFF505050;
uint32_t CLR_CHROME_TEXT = 0xFFFFFFFF;
uint32_t CLR_GAUGE_BG = 0xFF1A1A1A;       // #110: set per-theme below
uint32_t CLR_GAUGE_BORDER = 0xFF606060;   // #110: set per-theme below
// Focused taskbar task-tile outline. Deliberately a MID-GREY, never white: a
// white outline around the dark active-tile box read as harsh in dark themes.
// Derived per-theme in compositor_apply_theme() as a 50% blend of the taskbar
// background and its readable ink, so it stays subtle in every palette.
uint32_t CLR_TASK_FOCUS_BORDER = 0xFF707070;

// Apply a system-wide theme to the compositor chrome. Maps the kernel
// theme id (see Settings: 1=Dark 2=Light 4=Classic 5=Ocean 9=Nord) to a
// chrome palette. Unknown ids fall back to the dark palette.
// (#285) Small ARGB darken helper (compositor has no gui_style link).
static uint32_t darken_argb(uint32_t c, int amt) {
    int r = (int)((c >> 16) & 0xFF) - amt; if (r < 0) r = 0;
    int g = (int)((c >> 8)  & 0xFF) - amt; if (g < 0) g = 0;
    int b = (int)( c        & 0xFF) - amt; if (b < 0) b = 0;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Apply a system-wide theme to the compositor chrome. (#285) ALL chrome colors
// are now derived from the kernel theme table via SYS_THEME_COLOR, the single
// source of truth shared with the window decorator and every app. This makes
// every one of the 12 kernel themes recolor the taskbar / start menu / context
// menus consistently (the old hardcoded switch only knew 5 ids).
void compositor_apply_theme(int kernel_theme_id)
{
    g_compositor_theme_id = kernel_theme_id;   // remembered for window-shadow gating (#160)

    // (#337) Fork-compat guard: the ole2c kernel (Word6 fork) does not implement
    // SYS_THEME_COLOR (#285). On an unsupported syscall the kernel returns -1, so
    // theme_color_of() yields 0xFFFFFFFF (a real theme color has a zero high byte)
    // and every chrome color would collapse to white. Detect that sentinel and
    // keep the compositor's built-in dark defaults instead, which already match
    // the Retro-UNIX palette the ole2c kernel decorator uses. On the main kernel
    // SYS_THEME_COLOR works normally, so full theming applies as before.
    if ((theme_color_of(kernel_theme_id, THEME_COLOR_TASKBAR_BG) & 0xFF000000u) != 0)
        return;

    // theme_color_of() returns 0x00RRGGBB; chrome wants opaque 0xFFRRGGBB.
    #define TC(cid) (0xFF000000u | theme_color_of(kernel_theme_id, (cid)))
    CLR_TASKBAR_BG      = TC(THEME_COLOR_TASKBAR_BG);
    CLR_TASKBAR_BORDER  = TC(THEME_COLOR_WINDOW_BORDER);
    CLR_TASKBAR_HOVER   = TC(THEME_COLOR_TASKBAR_HOVER);
    CLR_START_BTN       = TC(THEME_COLOR_START_BUTTON);
    CLR_MENU_BG         = TC(THEME_COLOR_MENU_BG);
    CLR_MENU_SHADOW     = darken_argb(CLR_MENU_BG, 24);
    CLR_MENU_BORDER     = TC(THEME_COLOR_MENU_BORDER);
    CLR_MENU_ITEM_HOVER = TC(THEME_COLOR_MENU_ITEM_HOVER);
    CLR_MENU_ITEM_NORM  = TC(THEME_COLOR_MENU_BG);
    CLR_MENU_CAT_BG     = TC(THEME_COLOR_SELECTION);
    CLR_MENU_TEXT       = TC(THEME_COLOR_MENU_TEXT);
    CLR_MENU_SEP        = TC(THEME_COLOR_MENU_SEPARATOR);
    CLR_CTX_BG          = TC(THEME_COLOR_MENU_BG);
    CLR_CTX_BORDER      = TC(THEME_COLOR_MENU_BORDER);
    CLR_CTX_HOVER       = TC(THEME_COLOR_MENU_ITEM_HOVER);
    #undef TC

    // #128: guarantee legible text by deriving menu/chrome ink from the actual
    // background luminance (fixes light themes especially).
    CLR_MENU_TEXT   = readable_ink(CLR_MENU_BG);
    CLR_CHROME_TEXT = readable_ink(CLR_TASKBAR_BG);
    // Focused task-tile outline: a subtle mid-grey (50% blend of the taskbar bg
    // and its ink), never the pure-white chrome ink. (desktop UX pass)
    {
        uint32_t ink = readable_ink(CLR_TASKBAR_BG);
        uint32_t br = (CLR_TASKBAR_BG >> 16) & 0xFF, bg = (CLR_TASKBAR_BG >> 8) & 0xFF, bb = CLR_TASKBAR_BG & 0xFF;
        uint32_t ir = (ink >> 16) & 0xFF, ig = (ink >> 8) & 0xFF, ib = ink & 0xFF;
        CLR_TASK_FOCUS_BORDER = 0xFF000000u
            | (((br + ir) / 2) << 16) | (((bg + ig) / 2) << 8) | ((bb + ib) / 2);
    }
    // #110: taskbar gauges (CPU/RAM/DSK/NET) follow the active theme: a darker
    // inset of the taskbar bg with the themed taskbar border.
    {
        uint32_t b = CLR_TASKBAR_BG;
        uint32_t r  = (((b >> 16) & 0xFF) * 7) / 10;
        uint32_t g  = (((b >> 8)  & 0xFF) * 7) / 10;
        uint32_t bl = (( b        & 0xFF) * 7) / 10;
        CLR_GAUGE_BG = 0xFF000000u | (r << 16) | (g << 8) | bl;
        CLR_GAUGE_BORDER = CLR_TASKBAR_BORDER;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // NOTE: No sys_putchar() calls here. They write to a PTY with no reader
    // and will block the compositor indefinitely.

    // Initialise the framebuffer and all subsystems
    if (compositor_init() < 0) {
        sys_exit(1);
    }

    // TODO: Re-enable login once desktop is fully working.
    // login_run() plays /SOUNDS/STARTUP.WAV on success internally.
    // login_run();
    g_logged_in = true;
    g_login_uid = 0;
    strncpy(g_login_username, "root", 63);
    g_login_username[63] = '\0';

    // Reset idle timer so screensaver does not activate immediately.
    g_idle_ms = uptime_ms();

    // Main compositor loop
    notif_init();         // #168 clear the spool for a fresh session
    // #168: a startup notification demonstrates the live producer->toast pipeline.
    notify_post("MayteraOS", "Desktop ready - notifications are active", NOTIFY_INFO);

    while (1) {

        int loop_sleep_ms = 33;   // #102/#379 adaptive idle poll interval

        taskbar_update();     // Refresh gauge samples (CPU, RAM, disk)
        process_input();      // Poll mouse and keyboard
        { static int s_dp = 0; if (++s_dp >= 10) { s_dp = 0; dock_style_poll(); } }  // #387 live dock style
        profile_tick();       // #92 persist UI settings on change
        stickies_tick();      // #270 persist sticky notes when changed
        notif_tick();         // #168 poll notification spool + age toasts
        process_events();     // Dispatch events to UI layers

        // Apply a wallpaper change requested by another app (e.g. Settings),
        // so the Appearance panel's wallpaper selector takes effect live.
        {
            // #246 progressive: a wallpaper change shows a fast coarse pass
            // first, then refines to full resolution on the next frame.
            extern int  wallpaper_current(void);
            extern void wallpaper_load_progressive(int index);
            extern int  wallpaper_refine(void);
            int wp = get_wallpaper();
            if (wp >= 0 && wp != wallpaper_current()) {
                wallpaper_load_progressive(wp);
                g_needs_redraw = true;
            } else if (wallpaper_refine()) {
                g_needs_redraw = true;
            }
        }

        // Apply a theme change requested by Settings (system-wide chrome).
        {
            static int s_cur_theme = -1;
            int th = get_theme();
            if (th != s_cur_theme) {
                s_cur_theme = th;
                compositor_apply_theme(th);
                g_needs_redraw = true;
            }
        }

        // Apply an icon-size change requested by Settings (live).
        {
            static int s_cur_icon = -1;
            int isz = get_icon_size();
            if (isz != s_cur_icon) {
                s_cur_icon = isz;
                compositor_apply_icon_size(isz);
                g_needs_redraw = true;
            }
        }

        // Apply display effects (brightness / night-light) requested by Settings.
        {
            static int s_fx = -1;
            int fx = get_display_fx();
            if (fx != s_fx) {
                s_fx = fx;
                g_brightness = fx & 0xFF;
                g_nightlight = (fx >> 8) & 0xFF;
                g_needs_redraw = true;
            }
        }

        // Apply a font-size change requested by Settings (#58).
        {
            static int s_fs = -1;
            int fs = get_font_size();
            if (fs != s_fs) {
                s_fs = fs;
                static const int px[4] = {12, 16, 20, 24};
                g_font_px = px[(fs >= 0 && fs < 4) ? fs : 1];
                g_needs_redraw = true;
            }
        }

        // Apply a screensaver-type change requested by Settings.
        {
            static int s_cur_ss = -1;
            int sst = get_screensaver();
            if (sst != s_cur_ss) {
                s_cur_ss = sst;
                screensaver_set_type(sst);
            }
        }

        // One-shot "Test Screensaver" trigger from Settings.
        if (get_ss_test()) {
            g_screensaver_active = true;
            g_needs_redraw = true;
        }

        // Screensaver animates continuously; always redraw when active.
        if (g_screensaver_active) {
            g_needs_redraw = true;
        }

        // Dirty-rectangle compositing (#102/#379): poll input at ~30Hz (cheap).
        // When anything INTERACTIVE is happening (recent input, an app window, a
        // drag, an open menu/dialog, or the screensaver) present a full frame,
        // exactly as before. Otherwise, on the pure-idle desktop, collect the
        // rectangles that actually changed (a clock tick, a sysmon sample, the
        // walking pet) and recomposite + present ONLY those - or present nothing
        // if nothing changed. This is what finishes #102: the idle desktop no
        // longer re-blits the whole 1280x800 wallpaper + every widget's TTF text
        // every frame, so the host core drops from ~pegged to single digits.
        {
            uint64_t now = uptime_ms();
            bool recent_input = (now - g_idle_ms) < 500;   // 0.5s (real ms)
            wm_window_info_t _w[16];
            int _n = wm_get_windows(_w, 16), _apps = 0;
            for (int _i = 0; _i < _n; _i++)
                if (_w[_i].visible && !_w[_i].minimized) _apps++;
            bool interactive = recent_input || g_screensaver_active || _apps > 0 ||
                        s_dragging_sheep || s_dragging_dog || s_dragging_widget ||
                        s_dragging_sticky || stickies_editing() ||
                        s_dragging_desktop || widget_menu_is_open() ||
                        g_start_menu_open || g_context_menu_open ||
                        g_wallpaper_picker_open || g_tray_menu_open ||
                        g_launcher_open ||
                        widget_settings_is_open() || taskbar_popup_active();

            if (g_screensaver_active) {
                render_frame();           // screensaver owns the whole display
                g_needs_redraw = false;
                vnc_mark_full_dirty();    // #440: whole screen changed
            } else if (interactive || g_needs_redraw) {
                // #440: only tell VNC the whole screen changed when it actually
                // did (recent input or an explicit redraw). A window/menu merely
                // sitting open keeps this branch "interactive" every frame; if we
                // marked full-dirty unconditionally we would resend the entire
                // ~4MB frame every ~33ms over VNC for no visual change, which
                // burns bandwidth (and hits the kernel TCP send limit fast). A
                // static interactive frame gets recomposited locally but is NOT
                // re-pushed to the remote viewer.
                bool vnc_changed = recent_input || g_needs_redraw;
                render_frame();           // full-screen composite + present
                g_needs_redraw = false;
                if (vnc_changed) vnc_mark_full_dirty();
                // Mouse feel: while the user is actively moving the pointer /
                // typing, poll + present at ~120Hz so the cursor tracks the hand
                // with low latency instead of the 33ms (30Hz) desktop cadence.
                // The cursor is re-composited every frame here (render_frame draws
                // it last), so it is never gated by the idle partial-present path.
                if (recent_input) loop_sleep_ms = 8;
            } else {
                // Pure idle: recomposite + present ONLY the changed rectangles,
                // or present nothing when nothing changed. widgets_collect_damage
                // advances pets/sysmon on their own time cadence (so animation
                // speed is independent of the poll rate) and records damage.
                damage_reset();
                widgets_collect_damage();
                taskbar_collect_damage();
                static int quiet = 0;
                if (damage_count() > 0) {
                    render_frame_idle();
                    quiet = 0;
                    loop_sleep_ms = 50;    // animating idle: ~20Hz poll is plenty
                    // #440: feed the exact same damage rects to the RFB layer so
                    // a connected VNC viewer gets a real incremental update
                    // instead of a full-frame resend.
                    int n = damage_count();
                    for (int di = 0; di < n; di++) {
                        int dx, dy, dw, dh;
                        if (damage_get(di, &dx, &dy, &dw, &dh))
                            vnc_mark_rect_dirty(dx, dy, dw, dh);
                    }
                } else {
                    if (quiet < 60) quiet++;
                    // Static desktop: nothing changed. Back the poll interval off
                    // so an idle desktop stops burning CPU on the 30Hz loop. Any
                    // input makes the next iteration interactive (-> 33ms) and a
                    // per-second clock/sysmon tick still wakes us in time.
                    loop_sleep_ms = (quiet > 6) ? 120 : 50;
                }
            }
        }

        // Remote screen capture: check for a /SCREENSHOT.REQ trigger at the
        // normal frame cadence (no busy-wait). g_fb now holds the frame we just
        // presented, so the capture matches exactly what is on screen.
        screenshot_poll();

        // Live VNC/RFB server (#440): rides this same adaptive cadence, never
        // blocks (see vnc.c for the no-busy-wait / non-blocking socket design).
        vnc_poll();

        sys_sleep(loop_sleep_ms);   // adaptive: 33ms active, up to 120ms idle
    }

    // Not reached; suppress compiler warnings about missing return
    return 0;
}
